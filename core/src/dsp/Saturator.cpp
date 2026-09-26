// Flubsound Pro - oversampled saturation (tape / tube / digital).
//
// Signal flow per channel (x = input, L = oversampler round-trip latency):
//
//   x --+--> upsample --> curve (per type, oversampled) --> downsample --> s
//       |                                                                 |
//       +--> dry delay (L samples) -----------------------------> x_d     |
//                                                                         v
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
// state, and blocks are split only where a type crossfade ends, so the output
// is identical for any host block size (bit-exact apart from when a state
// below 1e-15 gets flushed to zero, which is checked at segment ends).
//
// outputDb is a wet-path (make-up) gain: mix = 0 is always the exact,
// latency-aligned dry signal.
#include "flub/dsp/Saturator.h"

#include "flub/common/Math.h"

#include <algorithm>
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

    preparedFactor = osFactor >= 4 ? 4 : (osFactor >= 2 ? 2 : 1);
    oversampler.prepare (spec.numChannels, spec.maxBlockSize, preparedFactor, osQuality);
    dryDelay.prepare (spec.numChannels, oversampler.latencySamples());
    dryBuffer.setSize (spec.numChannels, spec.maxBlockSize);

    const auto osSize = static_cast<size_t> (preparedFactor) * static_cast<size_t> (spec.maxBlockSize);
    const auto baseSize = static_cast<size_t> (spec.maxBlockSize);
    osGain.assign (osSize, 1.0f);
    osInvGain.assign (osSize, 1.0f);
    osScratch.assign (osSize, 0.0f);
    for (auto* v : { &depthBuf, &tubeBuf, &bumpBuf, &gainBuf, &mixBuf })
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

    driveSmoother.reset (spec.sampleRate, kSmoothingMs, params.driveDb);
    mixSmoother.reset (spec.sampleRate, kSmoothingMs, params.mix);
    outputSmoother.reset (spec.sampleRate, kSmoothingMs, dbToGain (params.outputDb));

    fadeLength = std::max (1, msToSamples (kTypeFadeMs, spec.sampleRate));
    invFadeLength = 1.0f / static_cast<float> (fadeLength);
    invFadeLengthOs = 1.0f / static_cast<float> (fadeLength * preparedFactor);

    prepared = true;
    reset();
}

void Saturator::reset() noexcept
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
}

int Saturator::latencySamples() const noexcept
{
    return oversampler.latencySamples();
}

void Saturator::setParams (const SaturatorParams& p) noexcept
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
    switch (type)
    {
        case SaturationType::Tape:
            if (driveRamping)
                curveLoop<SaturationType::Tape, true> (preEmphasis, deEmphasis, st.pre, st.de, d, n, g, ig, g0, ig0);
            else
                curveLoop<SaturationType::Tape, false> (preEmphasis, deEmphasis, st.pre, st.de, d, n, g, ig, g0, ig0);
            flushState (st.pre);
            flushState (st.de);
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
        if (! fading)
        {
            runCurve (activeType, st, d, nOs);
            continue;
        }

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
            d[k] += w * (alt[k] - d[k]);
        }
    }
    oversampler.downsample (seg);

    // 4. Base-rate post-processing and latency-aligned dry/wet.
    const SvfCoeffs bumpC = headBump;
    const float dcG = dcBlockG;
    for (int c = 0; c < numChannels; ++c)
    {
        auto& st = channelState[static_cast<size_t> (c)];
        float* y = seg.channel (c);
        const float* x = dry.channel (c);
        float lpState = st.dcLp;
        SvfState bumpState = st.bump;
        for (int i = 0; i < length; ++i)
        {
            const auto si = static_cast<size_t> (i);
            float s = y[i];

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
        st.bump = bumpState;
        flushState (st.bump);
    }
}

void Saturator::process (const AudioBlock& block) noexcept
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
