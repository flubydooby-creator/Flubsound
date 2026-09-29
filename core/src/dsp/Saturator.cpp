// Flubsound Pro - oversampled saturation (tape / tube / digital).
//
// Signal flow per channel (x = input, L = oversampler round-trip latency):
//
//   x --+--> upsample x^ --> curve f (per type) --> f(x^) - x^ --> downsample --> d --> HP5
//       |                                                                                 |
//       +--> dry delay (L samples) --------------------------------------------> x_d ---(+)--> s
//   (delta oversampling: only the curve's deviation is band-limited, so the
//    programme never passes the half-band filters and the top octave does
//    not droop; the round-trip latency L is unchanged. The deviation's
//    residual-path DC blocker, a 5 Hz 1st-order high-pass with double state
//    (docs/11 E10), removes the DC an asymmetric waveform gets from every
//    curve, e.g. 100 + 200 Hz through Tape: see test_signal_hygiene.cpp)
//   With ADAA (the chain's designs, docs/11 E10 Phase 2) f is replaced by its
//   mean over each oversampled step (adaaLoop) and x^ by the step's midpoint
//   (x^[k-1] + x^[k]) / 2: both half a sample late, which the decimator takes
//   back, so the deviation still lines up with the dry delay.
//
//   s'   = s - tubeW * LP10(s)                  tube DC blocker (10 Hz HP)
//   s''  = s' + bumpBeta * tapeW * BP80(s')     tape head bump (+1 dB * drive/24 @ 80 Hz)
//   core = x_d + depth * (s'' - x_d)            drive depth (0 at 0 dB drive)
//   y    = x_d + mix * (outGain * core - x_d)   latency-aligned dry/wet
//
// Oversampled curves, g = 10^(drive/20):
//   Tape    : de( tanh (g * pre(x)) / g )   pre = +6 dB / de = -6 dB SVF high
//             shelves at 3 kHz (Q 0.7071). They are exact inverses, so the
//             small-signal path is flat, but HF reaches the tanh 6 dB hotter.
//   Tube    : f(g x) / g, f(x) = (tanh(x + b) - tanh b) / (1 - tanh^2 b), b = 0.2,
//             evaluated as tanh(x) / (1 + tanh(b) tanh(x)) (same function via the
//             tanh addition theorem, but free of the cancellation that would
//             turn the subtraction into float noise at low levels).
//   Digital : f(g x) / g, f(x) = x - (4/27) x^3 for |x| < 1.5, sign(x) beyond.
// f'(0) = 1 for every curve, so quiet material passes at unity gain.
//
// Drive depth: with y = f(g x)/g alone, 0 dB drive would still be tanh(x) -
// several % THD and -2.4 dB peaks on a full-scale master, so engaging the
// module (e.g. the Warmth macro at 1 %) would be an audible jump. The curve is
// therefore blended in with depth = smoothstep(0, 6 dB, drive): exactly
// transparent at 0 dB, exactly the specified f(g x)/g from 6 dB up. Both terms
// have unity small-signal gain, so the blend keeps that property.
//
// Smoothing: drive (dB), mix and output gain are linear 20 ms ramps stepped
// per base-rate sample; the curve gain g is additionally interpolated linearly
// across the oversampled sub-samples, so a drive ramp has no base-rate steps.
// Type changes are crossfaded over 20 ms in the oversampled domain (both
// curves run during the fade); the tube DC blocker and the tape head bump run
// continuously and are faded with matching weights. Everything is per-sample
// state, and blocks are split only where a type crossfade ends (and into
// spec.maxBlockSize chunks for the scratch buffers), so the output
// is identical for any host block size (bit-exact apart from when a state
// below 1e-15 gets flushed to zero, which is checked at segment ends and
// every 64 oversampled samples for the tape emphasis).
//
// outputDb is a wet-path (make-up) gain: mix = 0 is always the exact,
// latency-aligned dry signal.
#include "flub/dsp/Saturator.h"

#include "flub/common/Math.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace flub
{
namespace
{
constexpr float kMaxDriveDb = 24.0f;
constexpr float kMinOutputDb = -12.0f, kMaxOutputDb = 12.0f;

constexpr float kSmoothingMs = 20.0f; // drive / mix / output ramps
constexpr float kTypeFadeMs = 20.0f;  // type crossfade

// Drive at which the curve is fully engaged (depth = 1).
constexpr float kFullDepthDb = 6.0f;

constexpr double kEmphasisHz = 3000.0;
constexpr double kEmphasisQ = 0.7071;
constexpr double kEmphasisDb = 6.0;

constexpr double kHeadBumpHz = 80.0;
constexpr double kHeadBumpQ = 1.0;
constexpr float kHeadBumpMaxDb = 1.0f; // at kMaxDriveDb

constexpr double kDcBlockHz = 10.0;
// Residual-path DC blocker on the curve's deviation (docs/11 E10), as on the
// maximizer's clipper correction.
constexpr double kResidualDcBlockHz = 5.0;

// tanh (0.2): the tube bias b.
constexpr float kTubeTanhBias = 0.19737532022490401f;

constexpr float kDigitalKnee = 1.5f;
constexpr float kDigitalCubic = 4.0f / 27.0f;

// ln(10) / 20: dB -> natural-log gain.
constexpr float kDbToLog = 0.11512925464970229f;

// Filter state below -300 dB re full scale is flushed at the end of every
// segment: inaudible, but it stops decaying IIR states from crawling through
// (or limit-cycling in) subnormals when the host did not enable FTZ. A
// non-finite state (only possible after non-finite input) is cleared too.
constexpr float kStateFloor = 1.0e-15f;

// Oversampled samples between flushes of the tape emphasis states. From the
// 1e-15 floor, subnormal range (1.2e-38) is 53 e-folds away, which takes at
// least ~150 samples at the fastest emphasis decay (44.1 kHz, 1x). 64 samples
// leaves a wide margin, so the flush always catches the state first.
constexpr int kTapeFlushInterval = 64;

float flushState (float v) noexcept
{
    return (std::abs (v) < kStateFloor || ! std::isfinite (v)) ? 0.0f : v;
}

void flushState (SvfState& s) noexcept
{
    s.ic1 = flushState (s.ic1);
    s.ic2 = flushState (s.ic2);
}

/** NaN -> fallback; everything else (including +-inf) clamps into range. */
float clampOr (float v, float lo, float hi, float fallback) noexcept
{
    return std::isnan (v) ? fallback : std::clamp (v, lo, hi);
}

SaturatorParams sanitise (const SaturatorParams& p) noexcept
{
    const SaturatorParams defaults;
    SaturatorParams s;
    s.type = static_cast<int> (p.type) > static_cast<int> (SaturationType::Digital) ? SaturationType::Digital : p.type;
    s.driveDb = clampOr (p.driveDb, 0.0f, kMaxDriveDb, defaults.driveDb);
    s.mix = clampOr (p.mix, 0.0f, 1.0f, defaults.mix);
    s.outputDb = clampOr (p.outputDb, kMinOutputDb, kMaxOutputDb, defaults.outputDb);
    return s;
}

inline float tubeCurve (float x) noexcept
{
    const float t = std::tanh (x);
    return t / (1.0f + kTubeTanhBias * t);
}

inline float digitalCurve (float x) noexcept
{
    if (std::abs (x) < kDigitalKnee)
        return x - kDigitalCubic * x * x * x;
    return x > 0.0f ? 1.0f : -1.0f;
}

// ---- First-order ADAA (docs/11 E10 Phase 2, Oversampler::Design::adaa) ----
// With u = g x and the curve phi (tanh, the tube curve, the digital clip),
// f(x) = phi (g x) / g. ADAA1 replaces f(x[n]) by its mean over the segment
// from x[n-1] to x[n]: (F(x[n]) - F(x[n-1])) / (x[n] - x[n-1]), F' = f.
// Only the deviation phi(u) - u is needed here (the delta oversampling of
// the header comment), so Psi(u) = Phi(u) - u^2 / 2 with Phi' = phi, and the
// deviation in x units is (Psi(ua) - Psi(ub)) / ((ua - ub) g). Everything is
// in double: Psi and u^2 / 2 nearly cancel for small u, and each form below
// keeps its absolute error proportional to u^2 (log1p of cosh u - 1 =
// 2 sinh^2 (u / 2)), so quiet signals stay exact. Below kAdaaMinStep the
// quotient is replaced by the deviation at the midpoint (error ~ step^2 / 24).
constexpr double kAdaaMinStep = 1.0e-6;
constexpr double kLn2 = 0.69314718055994531;

// ln cosh u - u^2 / 2 = sum c_n u^(2n), n >= 2 (the Taylor series, from the
// Bernoulli numbers), for |u| < kLnCoshSeriesLimit: 14 terms leave an error
// below 1e-17 there (below the exact form's own rounding), so the two forms
// meet without a step, and the series is about twice as fast.
constexpr double kLnCoshSeriesLimit = 0.5;
constexpr std::array<double, 14> kLnCoshSeries { -8.333333333333333e-02, 2.2222222222222223e-02, -6.746031746031746e-03,
                                                 2.1869488536155205e-03, -7.386029608251831e-04, 2.565805740408915e-04,
                                                 -9.09896491907074e-05, 3.277930227475478e-05, -1.1956455712177625e-05,
                                                 4.405244525877023e-06, -1.6365968284715348e-06, 6.122655795895756e-07,
                                                 -2.3041747198769394e-07, 8.715903837635848e-08 };

/** ln cosh u - u^2 / 2 (Tape's Psi). */
inline double lnCoshMinusHalfSquare (double u) noexcept
{
    const double a = std::abs (u), u2 = u * u;
    if (a < kLnCoshSeriesLimit)
    {
        double p = kLnCoshSeries.back();
        for (size_t k = kLnCoshSeries.size() - 1; k-- > 0;)
            p = p * u2 + kLnCoshSeries[k];
        return p * u2 * u2;
    }
    return a - kLn2 + std::log1p (std::exp (-2.0 * a)) - 0.5 * u2;
}

// Tube: ln (cosh u + b sinh u) = |u| + ln ((1 +- b) / 2) + log1p ((1 -+ b) / (1 +- b) e^(-2|u|)), sign of u.
// (b is the float kTubeTanhBias, as in the curve itself.)
constexpr double kTubeB = kTubeTanhBias;
constexpr double kTubeLnHalfPlus = -0.5130152464504354;  // ln ((1 + b) / 2)
constexpr double kTubeLnHalfMinus = -0.9130152612755894; // ln ((1 - b) / 2)

// Tube's Psi = sum d_k u^k, k = 3 .. 22 (Taylor series of the integral of
// tanh u / (1 + b tanh u), minus u^2 / 2), for |u| < kTubeSeriesLimit, where
// its error is below 1e-16 (the series converges within |u| < pi / 2).
constexpr double kTubeSeriesLimit = 0.35;
constexpr std::array<double, 20> kTubeSeries { -0.06579177578290303, -0.07359407837183081, 0.024778879404233935, 0.015982327147100947,
                                               -0.009230193291519431, -0.0034837581636299143, 0.0033565461474480635, 0.0006293163035627511,
                                               -0.0011865040292750944, -3.563606783282601e-05, 0.00040564709699824, -4.7908023333918235e-05,
                                               -0.00013307604683526924, 3.662081549585729e-05, 4.1330992113565494e-05, -1.920943884988969e-05,
                                               -1.1845330413154362e-05, 8.69016158058752e-06, 2.9539214204827618e-06, -3.6005439374155532e-06 };

template <SaturationType Type>
double curvePsi (double u) noexcept
{
    if constexpr (Type == SaturationType::Tape)
    {
        // Phi = ln cosh u
        return lnCoshMinusHalfSquare (u);
    }
    else if constexpr (Type == SaturationType::Tube)
    {
        // phi = tanh u / (1 + b tanh u): Phi = (ln (cosh u + b sinh u) - b u) / (1 - b^2)
        constexpr double b = kTubeB;
        const double a = std::abs (u);
        if (a < kTubeSeriesLimit)
        {
            double p = kTubeSeries.back();
            for (size_t k = kTubeSeries.size() - 1; k-- > 0;)
                p = p * u + kTubeSeries[k];
            return p * u * u * u;
        }
        double lnD;
        if (a < 1.0)
        {
            // cosh u - 1 = 2 sh^2, sinh u = 2 sh sqrt (1 + sh^2), sh = sinh (u / 2)
            const double sh = std::sinh (0.5 * u);
            lnD = std::log1p (2.0 * sh * (sh + b * std::sqrt (1.0 + sh * sh)));
        }
        else
        {
            const double c = u > 0.0 ? 1.0 + b : 1.0 - b;
            lnD = a + (u > 0.0 ? kTubeLnHalfPlus : kTubeLnHalfMinus) + std::log1p ((2.0 - c) / c * std::exp (-2.0 * a));
        }
        return (lnD - b * u) / (1.0 - b * b) - 0.5 * u * u;
    }
    else
    {
        // phi = u - (4/27) u^3 below the knee, +-1 beyond: Phi = u^2 / 2 - u^4 / 27, |u| - 9/16
        constexpr double knee = kDigitalKnee, cubic = kDigitalCubic;
        const double a = std::abs (u);
        if (a < knee)
            return -0.25 * cubic * u * u * u * u;
        return a + (0.5 * knee * knee - 0.25 * cubic * knee * knee * knee * knee - knee) - 0.5 * u * u; // Phi (knee) + |u| - knee
    }
}

template <SaturationType Type>
double curveDeviation (double u) noexcept
{
    if constexpr (Type == SaturationType::Tape)
        return std::tanh (u) - u;
    else if constexpr (Type == SaturationType::Tube)
    {
        const double t = std::tanh (u);
        return t / (1.0 + kTubeTanhBias * t) - u;
    }
    else
        return (std::abs (u) < kDigitalKnee ? u - kDigitalCubic * u * u * u : (u > 0.0 ? 1.0 : -1.0)) - u;
}

/** One curve over n oversampled samples with ADAA1, in place: d[i] becomes
    the curve's mean over (x[i-1], x[i]), i.e. half a sample late. prev = the
    curve input one sample before d[0] (Tape: after the pre-emphasis);
    updated. The midpoint (x[i-1] + x[i]) / 2 is the linear part, so a signal
    the curve leaves alone comes out as its half-sample average. */
template <SaturationType Type, bool Ramp>
void adaaLoop (const SvfCoeffs& preC, const SvfCoeffs& deC, SvfState& preState, SvfState& deState, float& prev, float* d, int n,
               const float* gains, float g0) noexcept
{
    SvfState ps = preState, ds = deState;
    double xPrev = prev;
    double psiPrev = Ramp ? 0.0 : curvePsi<Type> (static_cast<double> (g0) * xPrev);
    for (int i = 0; i < n; ++i)
    {
        const double g = Ramp ? gains[i] : g0;
        double x = d[i];
        if constexpr (Type == SaturationType::Tape)
            x = svfTick (preC, ps, d[i]);
        const double ua = g * x, ub = g * xPrev, du = ua - ub;
        const double psiA = curvePsi<Type> (ua);
        const double dev = std::abs (du) > kAdaaMinStep ? (psiA - (Ramp ? curvePsi<Type> (ub) : psiPrev)) / du
                                                        : curveDeviation<Type> (0.5 * (ua + ub));
        float y = static_cast<float> (0.5 * (x + xPrev) + dev / g);
        if constexpr (Type == SaturationType::Tape)
            y = svfTick (deC, ds, y);
        d[i] = y;
        psiPrev = psiA;
        xPrev = x;
    }
    prev = static_cast<float> (xPrev);
    preState = ps;
    deState = ds;
}

/** One curve over n oversampled samples, in place. Ramp = per-sample gain
    arrays (drive moving), otherwise the constant g / invG. Local copies keep
    the filter state in registers. */
template <SaturationType Type, bool Ramp>
void curveLoop (const SvfCoeffs& preC, const SvfCoeffs& deC, SvfState& preState, SvfState& deState, float* d, int n,
                const float* gains, const float* invGains, float g0, float invG0) noexcept
{
    if constexpr (Type == SaturationType::Tape)
    {
        const SvfCoeffs pc = preC, dc = deC;
        SvfState ps = preState, ds = deState;
        for (int i = 0; i < n; ++i)
        {
            const float g = Ramp ? gains[i] : g0;
            const float invG = Ramp ? invGains[i] : invG0;
            const float p = svfTick (pc, ps, d[i]);
            d[i] = svfTick (dc, ds, std::tanh (g * p) * invG);
        }
        preState = ps;
        deState = ds;
    }
    else
    {
        for (int i = 0; i < n; ++i)
        {
            const float g = Ramp ? gains[i] : g0;
            const float invG = Ramp ? invGains[i] : invG0;
            if constexpr (Type == SaturationType::Tube)
                d[i] = tubeCurve (g * d[i]) * invG;
            else
                d[i] = digitalCurve (g * d[i]) * invG;
        }
    }
}
} // namespace

//==============================================================================
float Saturator::shape (SaturationType type, float x) noexcept
{
    switch (type)
    {
        case SaturationType::Tape: return std::tanh (x);
        case SaturationType::Tube: return tubeCurve (x);
        case SaturationType::Digital: break;
    }
    return digitalCurve (x); // Digital, and out-of-range values (sanitise() maps them to Digital)
}

Saturator::DriveGains Saturator::driveGains (float driveDb) noexcept
{
    DriveGains d;
    d.g = std::exp (driveDb * kDbToLog);
    d.invG = 1.0f / d.g;
    d.depth = smoothstep (0.0f, kFullDepthDb, driveDb);
    d.bumpBeta = std::exp (kHeadBumpMaxDb * (driveDb / kMaxDriveDb) * kDbToLog) - 1.0f;
    return d;
}

//==============================================================================
void Saturator::prepare (const ProcessSpec& newSpec)
{
    spec = newSpec;
    spec.numChannels = std::clamp (spec.numChannels, 1, kMaxChannels);
    spec.maxBlockSize = std::max (1, spec.maxBlockSize);
    if (! (spec.sampleRate > 0.0) || ! std::isfinite (spec.sampleRate))
        spec.sampleRate = 48000.0;

    Oversampler::Design design = osDesign;
    design.factor = design.factor >= 8 ? 8 : (design.factor >= 4 ? 4 : (design.factor >= 2 ? 2 : 1));
    design.d1 = std::max (1, design.d1);
    design.d2 = std::max (1, design.d2);
    // ADAA needs stage 1's decimator to take its half sample back.
    design.adaa = design.adaa && design.factor > 1 && design.m1 > 0;
    adaa = design.adaa;
    preparedFactor = design.factor;
    oversampler.prepare (spec.numChannels, spec.maxBlockSize, design);
    dryDelay.prepare (spec.numChannels, oversampler.latencySamples());
    dryBuffer.setSize (spec.numChannels, spec.maxBlockSize);

    const auto osSize = static_cast<size_t> (preparedFactor) * static_cast<size_t> (spec.maxBlockSize);
    const auto baseSize = static_cast<size_t> (spec.maxBlockSize);
    osGain.assign (osSize, 1.0f);
    osInvGain.assign (osSize, 1.0f);
    osScratch.assign (osSize, 0.0f);
    osInput.assign (osSize, 0.0f);
    for (auto* v : { &depthBuf, &tubeBuf, &bumpBuf, &gainBuf, &mixBuf, &distWeightBuf })
        v->assign (baseSize, 0.0f);

    // Emphasis runs at the oversampled rate, where the curve is. The two
    // shelves are exact inverses (same frequency / Q, gain negated), so their
    // cascade is an identity whenever the tanh between them is linear.
    const double osRate = spec.sampleRate * preparedFactor;
    preEmphasis = SvfCoeffs::make (FilterType::HighShelf, kEmphasisHz, kEmphasisQ, kEmphasisDb, osRate);
    deEmphasis = SvfCoeffs::make (FilterType::HighShelf, kEmphasisHz, kEmphasisQ, -kEmphasisDb, osRate);
    // Unity-peak band-pass: x + beta * BP(x) is a peaking bell of exactly 1 + beta at 80 Hz.
    headBump = SvfCoeffs::make (FilterType::BandPass, kHeadBumpHz, kHeadBumpQ, 0.0, spec.sampleRate);
    const double gDc = std::tan (kPi * kDcBlockHz / spec.sampleRate);
    dcBlockG = static_cast<float> (gDc / (1.0 + gDc));
    const double gResidual = std::tan (kPi * kResidualDcBlockHz / spec.sampleRate);
    residualDcG = gResidual / (1.0 + gResidual);

    driveSmoother.reset (spec.sampleRate, kSmoothingMs, params.driveDb);
    mixSmoother.reset (spec.sampleRate, kSmoothingMs, params.mix);
    outputSmoother.reset (spec.sampleRate, kSmoothingMs, dbToGain (params.outputDb));

    fadeLength = std::max (1, msToSamples (kTypeFadeMs, spec.sampleRate));
    invFadeLength = 1.0f / static_cast<float> (fadeLength);
    invFadeLengthOs = 1.0f / static_cast<float> (fadeLength * preparedFactor);

    distortionWindow.prepare (spec.sampleRate);

    prepared = true;
    reset();
}

void Saturator::reset() noexcept FLUB_NONBLOCKING
{
    oversampler.reset();
    dryDelay.reset();
    for (auto& st : channelState)
        st = ChannelState {};

    driveSmoother.setImmediate (driveSmoother.getTarget());
    mixSmoother.setImmediate (mixSmoother.getTarget());
    outputSmoother.setImmediate (outputSmoother.getTarget());
    steady = driveGains (driveSmoother.getCurrent());
    lastGain = steady.g;
    driveRamping = false;

    activeType = fromType = toType = params.type;
    fading = false;
    fadePos = 0;
    distortionWindow.reset();
    distortionDb.store (kMinusInfDb, std::memory_order_relaxed);
}

int Saturator::latencySamples() const noexcept
{
    return oversampler.latencySamples();
}

void Saturator::setParams (const SaturatorParams& p) noexcept FLUB_NONBLOCKING
{
    const SaturatorParams s = sanitise (p);
    if (s == params)
        return; // the chain pushes parameters every block: unchanged is free

    params = s;
    driveSmoother.setTarget (s.driveDb);
    mixSmoother.setTarget (s.mix);
    outputSmoother.setTarget (dbToGain (s.outputDb));
    // The type is picked up at the next segment boundary (updateTypeFade).
}

//==============================================================================
void Saturator::updateTypeFade() noexcept
{
    const SaturationType target = params.type;
    if (! fading)
    {
        if (target == activeType)
            return;
        fromType = activeType;
        toType = target;
        fadePos = 0;
        fading = true;
        // Tape is the only curve with memory; the others are static. Its
        // filters were idle, so start them clean (the crossfade weight is 0 at
        // this instant, and the emphasis transient decays within ~0.1 ms).
        if (toType == SaturationType::Tape)
            for (auto& st : channelState)
            {
                st.pre.reset();
                st.de.reset();
                st.adaaPrevTape = 0.0f;
            }
        return;
    }

    // Going back to where the fade came from: reverse it from the current
    // position (both curves are running, so their states are valid). A third
    // type waits until the current fade has finished.
    if (target == fromType)
    {
        std::swap (fromType, toType);
        fadePos = fadeLength - fadePos;
    }
}

void Saturator::computeControls (int length) noexcept
{
    driveRamping = driveSmoother.isSmoothing();
    const float invFactor = 1.0f / static_cast<float> (preparedFactor);
    float gPrev = lastGain;

    for (int i = 0; i < length; ++i)
    {
        const auto si = static_cast<size_t> (i);
        DriveGains dg = steady;
        if (driveRamping)
        {
            dg = driveGains (driveSmoother.next());
            // Linear interpolation of g across the oversampled sub-samples of
            // this base sample, so the curve gain has no base-rate staircase.
            for (int j = 0; j < preparedFactor; ++j)
            {
                const auto k = static_cast<size_t> (i * preparedFactor + j);
                const float g = gPrev + (dg.g - gPrev) * static_cast<float> (j + 1) * invFactor;
                osGain[k] = g;
                osInvGain[k] = 1.0f / g;
            }
            gPrev = dg.g;
        }

        float tubeW = activeType == SaturationType::Tube ? 1.0f : 0.0f;
        float tapeW = activeType == SaturationType::Tape ? 1.0f : 0.0f;
        if (fading)
        {
            const float c = static_cast<float> (fadePos + i + 1) * invFadeLength;
            tubeW = (fromType == SaturationType::Tube ? 1.0f - c : 0.0f) + (toType == SaturationType::Tube ? c : 0.0f);
            tapeW = (fromType == SaturationType::Tape ? 1.0f - c : 0.0f) + (toType == SaturationType::Tape ? c : 0.0f);
        }

        depthBuf[si] = dg.depth;
        tubeBuf[si] = tubeW;
        bumpBuf[si] = dg.bumpBeta * tapeW;
        gainBuf[si] = outputSmoother.next();
        mixBuf[si] = mixSmoother.next();

        // THD+N telemetry: y = a x + b (f - x) with a = 1 - mix + mix * gain
        // (> 0: gain >= -12 dB) and b = mix * gain * depth, so relative to the
        // linear path the curve's deviation is weighted by b / a.
        const float wetGain = mixBuf[si] * gainBuf[si];
        distWeightBuf[si] = wetGain * dg.depth / (1.0f - mixBuf[si] + wetGain);
    }

    if (driveRamping)
    {
        lastGain = gPrev;
        if (! driveSmoother.isSmoothing())
        {
            // Landed exactly on the target: identical values to the ramp's last sample.
            steady = driveGains (driveSmoother.getCurrent());
            lastGain = steady.g;
        }
    }
}

void Saturator::runCurve (SaturationType type, ChannelState& st, float* d, int n) noexcept
{
    const float* g = osGain.data();
    const float* ig = osInvGain.data();
    const float g0 = steady.g, ig0 = steady.invG;
    if (adaa)
    {
        // First-order ADAA (see adaaLoop). Tube and Digital share the last
        // oversampled input as their previous sample (a copy: in a
        // crossfade both curves start from it; processSegment advances it).
        float prevIn = st.adaaPrevIn;
        switch (type)
        {
            case SaturationType::Tape:
                for (int pos = 0; pos < n; pos += kTapeFlushInterval) // as below
                {
                    const int len = std::min (kTapeFlushInterval, n - pos);
                    if (driveRamping)
                        adaaLoop<SaturationType::Tape, true> (preEmphasis, deEmphasis, st.pre, st.de, st.adaaPrevTape, d + pos, len, g + pos, g0);
                    else
                        adaaLoop<SaturationType::Tape, false> (preEmphasis, deEmphasis, st.pre, st.de, st.adaaPrevTape, d + pos, len, g, g0);
                    flushState (st.pre);
                    flushState (st.de);
                }
                st.adaaPrevTape = flushState (st.adaaPrevTape);
                break;
            case SaturationType::Tube:
                if (driveRamping)
                    adaaLoop<SaturationType::Tube, true> (preEmphasis, deEmphasis, st.pre, st.de, prevIn, d, n, g, g0);
                else
                    adaaLoop<SaturationType::Tube, false> (preEmphasis, deEmphasis, st.pre, st.de, prevIn, d, n, g, g0);
                break;
            case SaturationType::Digital:
                if (driveRamping)
                    adaaLoop<SaturationType::Digital, true> (preEmphasis, deEmphasis, st.pre, st.de, prevIn, d, n, g, g0);
                else
                    adaaLoop<SaturationType::Digital, false> (preEmphasis, deEmphasis, st.pre, st.de, prevIn, d, n, g, g0);
                break;
        }
        return;
    }
    switch (type)
    {
        case SaturationType::Tape:
            // The 3 kHz emphasis shelves decay fast (down to ~3 samples per
            // e-fold at 44.1 kHz, 1x), so after the input stops a state can
            // drop from the 1e-15 flush floor into subnormals within ~150
            // samples and then crawl or limit-cycle there. That costs up to 30x
            // without FTZ, and a segment can hold 4 * 4096 oversampled
            // samples. Flushing every kTapeFlushInterval samples keeps the
            // states out of the subnormal range. The check is a few
            // compares per 64 samples.
            for (int pos = 0; pos < n; pos += kTapeFlushInterval)
            {
                const int len = std::min (kTapeFlushInterval, n - pos);
                if (driveRamping)
                    curveLoop<SaturationType::Tape, true> (preEmphasis, deEmphasis, st.pre, st.de, d + pos, len, g + pos, ig + pos, g0, ig0);
                else
                    curveLoop<SaturationType::Tape, false> (preEmphasis, deEmphasis, st.pre, st.de, d + pos, len, g, ig, g0, ig0);
                flushState (st.pre);
                flushState (st.de);
            }
            break;
        case SaturationType::Tube:
            if (driveRamping)
                curveLoop<SaturationType::Tube, true> (preEmphasis, deEmphasis, st.pre, st.de, d, n, g, ig, g0, ig0);
            else
                curveLoop<SaturationType::Tube, false> (preEmphasis, deEmphasis, st.pre, st.de, d, n, g, ig, g0, ig0);
            break;
        case SaturationType::Digital:
            if (driveRamping)
                curveLoop<SaturationType::Digital, true> (preEmphasis, deEmphasis, st.pre, st.de, d, n, g, ig, g0, ig0);
            else
                curveLoop<SaturationType::Digital, false> (preEmphasis, deEmphasis, st.pre, st.de, d, n, g, ig, g0, ig0);
            break;
    }
}

void Saturator::processSegment (const AudioBlock& io, int start, int length) noexcept
{
    const AudioBlock seg = io.subBlock (start, length);
    const int numChannels = seg.numChannels;

    // 1. Dry path, delayed by exactly the oversampler round trip.
    const AudioBlock dry = dryBuffer.block (numChannels, length);
    dry.copyFrom (seg);
    dryDelay.process (dry);

    // 2. Per-sample control values (advanced once per sample, shared by all channels).
    computeControls (length);

    // 3. Oversampled curve(s), in place in the oversampler's buffer (or in
    //    `seg` itself at factor 1).
    const AudioBlock up = oversampler.upsample (seg);
    const int nOs = up.numSamples;
    for (int c = 0; c < up.numChannels; ++c)
    {
        auto& st = channelState[static_cast<size_t> (c)];
        float* d = up.channel (c);
        // Delta oversampling: keep the upsampled input so that only the
        // curve's deviation f(x^) - x^ passes the band-limiting downsampler;
        // the programme itself is taken from the exact dry delay (no top-octave
        // droop from the half-band filters, e.g. -2.3 dB at 20 kHz / 44.1 kHz
        // with the short 2x design).
        float* in = osInput.data();
        const float lastIn = d[nOs - 1];
        if (adaa)
        {
            // The ADAA curves are half an oversampled sample late: their
            // linear part is the midpoint of consecutive inputs, so that is
            // what the deviation and the telemetry are taken against (the
            // stage-1 decimator takes the half sample back, Oversampler.h).
            in[0] = 0.5f * (st.adaaPrevIn + d[0]);
            for (int k = 1; k < nOs; ++k)
                in[k] = 0.5f * (d[k - 1] + d[k]);
        }
        else
        {
            std::copy (d, d + nOs, in);
        }
        if (! fading)
        {
            runCurve (activeType, st, d, nOs); // the deviation is taken below
        }
        else
        {
            // Crossfade: both curves on the same oversampled input, linear
            // equal-gain weights (the two outputs are highly correlated).
            float* alt = osScratch.data();
            std::copy (d, d + nOs, alt);
            runCurve (fromType, st, d, nOs);
            runCurve (toType, st, alt, nOs);
            const int base = fadePos * preparedFactor;
            for (int k = 0; k < nOs; ++k)
            {
                const float w = static_cast<float> (base + k + 1) * invFadeLengthOs;
                d[k] += w * (alt[k] - d[k]) - in[k];
            }
        }

        // The deviation (outside a crossfade) and the THD+N telemetry around
        // the curve, where the oversampled input and the curve's deviation are
        // aligned (the weight is held across the sub-samples of each base-rate
        // sample, like the other controls). One pass: at 8x these per-sample
        // loops cost as much as the curve.
        DistortionSums sums;
        const bool subtract = ! fading;
        for (int i = 0, k = 0; i < length; ++i)
        {
            const float w = distWeightBuf[static_cast<size_t> (i)];
            for (int j = 0; j < preparedFactor; ++j, ++k)
            {
                if (subtract)
                    d[k] -= in[k];
                sums.add (in[k], w * d[k]);
            }
        }
        if (sums.isFinite())
            distortionWindow.channel (c).merge (sums);
        st.adaaPrevIn = flushState (lastIn);
    }
    oversampler.downsample (seg); // seg now holds the band-limited deviation
    if (float db = kMinusInfDb; distortionWindow.advance (length, db))
        distortionDb.store (db, std::memory_order_relaxed);

    // 4. Base-rate post-processing and latency-aligned dry/wet.
    const SvfCoeffs bumpC = headBump;
    const float dcG = dcBlockG;
    const double devG = residualDcG;
    for (int c = 0; c < numChannels; ++c)
    {
        auto& st = channelState[static_cast<size_t> (c)];
        float* y = seg.channel (c);
        const float* x = dry.channel (c);
        float lpState = st.dcLp;
        double devLp = st.residualDcLp;
        SvfState bumpState = st.bump;
        for (int i = 0; i < length; ++i)
        {
            const auto si = static_cast<size_t> (i);

            // Residual-path DC blocker (docs/11 E10): the band-limited curve
            // deviation passes a 1st-order TPT high-pass at 5 Hz with double
            // state, HP(d) = d - LP(d), so an asymmetric waveform leaves no DC
            // in any type; the programme itself never passes the filter.
            const double d = static_cast<double> (y[i]);
            const double dv = (d - devLp) * devG;
            const double dlp = dv + devLp;
            devLp = dlp + dv;
            float s = x[i] + static_cast<float> (d - dlp); // exact dry + band-limited curve deviation

            // Tube DC blocker: 1st-order TPT high-pass at 10 Hz, HP(s) = s - LP(s).
            // Always running so it is settled when a crossfade brings Tube in.
            const float v = (s - lpState) * dcG;
            const float lp = v + lpState;
            lpState = lp + v;
            s -= tubeBuf[si] * lp;

            // Tape head bump: s + beta * BP80(s) = peaking bell of 1 + beta at 80 Hz.
            s += bumpBuf[si] * svfTick (bumpC, bumpState, s);

            const float xd = x[i];
            const float core = xd + depthBuf[si] * (s - xd);
            y[i] = xd + mixBuf[si] * (gainBuf[si] * core - xd);
        }
        st.dcLp = flushState (lpState);
        // Flushed like the other states (kStateFloor, -300 dB), so a silent
        // tail reaches exact zero; cleared after non-finite input.
        st.residualDcLp = (std::abs (devLp) < kStateFloor || ! std::isfinite (devLp)) ? 0.0 : devLp;
        st.bump = bumpState;
        flushState (st.bump);
    }
}

void Saturator::process (const AudioBlock& block) noexcept FLUB_NONBLOCKING
{
    const int numChannels = std::min (block.numChannels, spec.numChannels);
    const int numSamples = block.numSamples;
    if (! prepared || numSamples <= 0 || numChannels <= 0)
        return;

    const AudioBlock io = block.firstChannels (numChannels);
    int pos = 0;
    while (pos < numSamples)
    {
        updateTypeFade();

        // Segments end where a type crossfade ends, so the next fade (if one is
        // queued) starts at the same stream position for any host block size.
        int len = std::min (numSamples - pos, spec.maxBlockSize);
        if (fading)
            len = std::min (len, fadeLength - fadePos);

        if (len > 0)
            processSegment (io, pos, len);

        if (fading)
        {
            fadePos += len;
            if (fadePos >= fadeLength)
            {
                activeType = toType;
                fading = false;
                fadePos = 0;
            }
        }
        pos += len;
    }
}
} // namespace flub
