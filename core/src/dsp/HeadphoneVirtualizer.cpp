// Flubsound Pro - multichannel (5.1 / 7.1) to binaural headphone virtualiser.
//
// Signal flow (every path adds into two ear accumulators; the sum is scaled by
// the -3 dB headroom trim and, during a layout swap, by the swap fade):
//
//   speaker (parametric):  x -> rear-cue shelf -> ITD line -+-> Lagrange(D_L) -> shadow_L -> ear L
//                                                           +-> Lagrange(D_R) -> shadow_R -> ear R
//   speaker (HRIR):        x -> history -> dot(h_L) -> ear L,  dot(h_R) -> ear R
//   LFE:                   x -> 4th-order Butterworth LP 120 Hz -> lfeGain -> both ears
//   room:                  sum of speaker inputs -> HP 200 Hz -> LP 5 kHz -> 6 taps
//                          (4 .. 19 ms, alternating ears) * roomAmount
//
// Geometry (Brown & Duda 1998 spherical head, radius a, c = 343 m/s): for a
// source at azimuth az and an ear at -90 (left) / +90 (right) degrees,
// theta = angle between the source and the ear axis (0 .. 180 deg):
//   delay  D = (a/c)(1 - cos theta) * fs             theta < 90 deg
//            = (a/c)(1 + theta - pi/2) * fs          theta >= 90 deg
//   shadow H(s) = (1 + alpha s / (2 w0)) / (1 + s / (2 w0)),  w0 = c/a,
//          alpha = 1.05 + 0.95 cos(theta * 180/150 deg)   (+6 dB .. -20 dB at HF)
// The delay is realised with a 4-tap (3rd-order) Lagrange interpolator. The
// centre speaker therefore reaches both ears after a/c (~12 samples at 48 kHz):
// that common path delay is part of the acoustic model, so the module
// reports zero latency (no look-ahead, no block delay).
//
// Control rate: every kControlInterval samples, counted in absolute stream
// time, a "busy" virtualiser advances its geometry smoothers (angles and head
// radius, one-pole 30 ms; rear-cue shelf gains, one-pole 10 ms), redesigns
// delays and filters, and steps the layout-swap state machine. Across the
// following control period delays and coefficients are interpolated per
// sample from the previous design to the new one, so a parameter sweep is a
// chain of short linear ramps (no zipper noise, no clicks). When nothing is
// busy the block is one segment; the output is sample-identical either way,
// so the result does not depend on the host block size. Room and LFE levels
// ramp linearly per sample (20 ms).
//
// Discrete changes: a layout change alters what each input channel means (and
// may switch renderer), so it is not glided. The output fades to silence over
// ~5 ms, roles / renderer / geometry are swapped and the per-channel state is
// cleared at silence. The new configuration then runs silently for a short
// pre-roll (2 ms; up to 10 ms for an HRIR) so its delay lines hold real input
// before the ~5 ms fade-in: fading in while a path is still empty would make
// the signal arrive as a step at a non-zero gain (a small click). The
// reflection line is not cleared, so content common to both layouts keeps
// its reflections continuous.
//
// Renderer selection: the HRIR renderer runs when a well-formed HrirSet was
// set before prepare(), its sampleRate equals the session rate (within
// 0.5 Hz) and its layout equals the running layout. Otherwise the parametric
// renderer runs (a mismatched set is ignored, never resampled here). The LFE
// path, the reflections and the trim are shared by both renderers.
//
// Channel handling: inputs beyond the layout are ignored; layout channels
// missing from a block are silent; a 1-channel block receives the mono
// fold-down (L + R) / 2 of the binaural pair.
#include "flub/dsp/HeadphoneVirtualizer.h"

#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace flub
{
namespace
{
constexpr double kSpeedOfSound = 343.0; // m/s

constexpr float kMinFrontDeg = 22.0f, kMaxFrontDeg = 45.0f;
constexpr float kMinSideDeg = 80.0f, kMaxSideDeg = 120.0f;
constexpr float kMinRearDeg = 120.0f, kMaxRearDeg = 165.0f;
constexpr float kMinHeadMm = 70.0f, kMaxHeadMm = 105.0f;
constexpr float kMinLfeDb = -20.0f, kMaxLfeDb = 10.0f;

// -3 dB headroom trim: a phantom source that hits both ears in phase (centre
// speaker plus reflections plus LFE) must not overload the rest of the chain.
constexpr float kTrim = 0.70794578f; // 10^(-3/20)

// Rear cue: the pinna shadows sources behind the head above ~3 kHz. A sphere
// is front/back symmetric, so without this the rear speakers fold forward.
constexpr double kRearShelfHz = 4000.0;
constexpr double kRearShelfQ = 0.70710678;
constexpr float kRearShelfDb = -4.0f;

constexpr double kLfeCutoffHz = 120.0;

// Early reflections: 6 taps, alternating ears (even -> left, odd -> right),
// weights paired so both ears receive the same reflected energy
// (0.5^2 + 0.4^2 + 0.3^2 = 0.5, i.e. -3 dB re the direct path at room = 1).
// The delays are mutually non-harmonic to avoid a periodic comb. The bus is
// band-limited: walls absorb the top end (low-pass) and low-frequency
// reflections would only add boom and comb the bass (high-pass).
constexpr std::array<float, 6> kReflectionMs { 4.0f, 5.3f, 8.9f, 11.7f, 15.4f, 19.0f };
constexpr std::array<float, 6> kReflectionWeight { 0.5f, 0.5f, 0.4f, 0.4f, 0.3f, 0.3f };
constexpr double kReflectionHpHz = 200.0;
constexpr double kReflectionLpHz = 5000.0;

constexpr float kGeometrySmoothingMs = 30.0f; // angles and head radius (one-pole)
constexpr float kShelfSmoothingMs = 10.0f;    // rear-cue shelf gain in dB (one-pole)
constexpr float kGainRampMs = 20.0f;          // room and LFE levels (linear)
constexpr float kFadeMs = 5.0f;               // layout swap: fade out, swap, fade in
constexpr float kPrerollMs = 2.0f;            // silent pre-roll after a swap (parametric)
constexpr float kMaxHrirPrerollMs = 10.0f;    // ... and the cap for an HRIR pre-roll

// Longest HRIR accepted by the direct-form convolver; longer sets are truncated.
constexpr int kMaxHrirTaps = 8192;

// Filter state below -300 dB re full scale is flushed to zero at the end of a
// segment so decaying IIR tails never crawl through subnormals when the host
// did not enable FTZ.
constexpr float kStateFloor = 1.0e-15f;

/** NaN -> fallback; everything else (including +-inf) clamps into range. */
float clampOr (float v, float lo, float hi, float fallback) noexcept
{
    return std::isnan (v) ? fallback : std::clamp (v, lo, hi);
}

ChannelLayout sanitiseLayout (ChannelLayout l) noexcept
{
    return static_cast<int> (l) > static_cast<int> (ChannelLayout::Surround71) ? ChannelLayout::Surround71 : l;
}

VirtualizerParams sanitise (const VirtualizerParams& p) noexcept
{
    const VirtualizerParams d;
    VirtualizerParams s;
    s.layout = sanitiseLayout (p.layout);
    s.frontAngleDeg = clampOr (p.frontAngleDeg, kMinFrontDeg, kMaxFrontDeg, d.frontAngleDeg);
    s.sideAngleDeg = clampOr (p.sideAngleDeg, kMinSideDeg, kMaxSideDeg, d.sideAngleDeg);
    s.rearAngleDeg = clampOr (p.rearAngleDeg, kMinRearDeg, kMaxRearDeg, d.rearAngleDeg);
    s.headRadiusMm = clampOr (p.headRadiusMm, kMinHeadMm, kMaxHeadMm, d.headRadiusMm);
    s.roomAmount = clampOr (p.roomAmount, 0.0f, 1.0f, d.roomAmount);
    s.lfeGainDb = clampOr (p.lfeGainDb, kMinLfeDb, kMaxLfeDb, d.lfeGainDb);
    return s;
}

bool hasLfe (ChannelLayout l) noexcept { return l != ChannelLayout::Stereo; }

/** Woodworth path length to one ear in units of the head radius (delay = value * a/c).
    theta is the angle between the source and the ear axis in radians (0 .. pi). */
double woodworth (double theta) noexcept
{
    return theta < 0.5 * kPi ? 1.0 - std::cos (theta) : 1.0 + theta - 0.5 * kPi;
}

/** Brown-Duda head-shadow HF gain: 2.0 (+6 dB) facing the ear, minimum 0.1
    (-20 dB) at 150 deg and a partial recovery (the "bright spot") at 180 deg. */
double shadowAlpha (double thetaDeg) noexcept
{
    return 1.05 + 0.95 * std::cos (thetaDeg * (180.0 / 150.0) * (kPi / 180.0));
}

/** 3rd-order (4-tap) Lagrange interpolator for a delay of `delay` samples,
    applied as sum h[k] x[n - base - k]. The integer base keeps the fractional
    position d = delay - base in [1, 2), the centred and best-behaved range of
    a 4-tap Lagrange filter; delays below 1 sample use d in [0, 1). The taps
    are continuous in `delay` (exact integer delays select a single tap). */
void lagrangeTaps (float delay, int& base, std::array<float, 4>& h) noexcept
{
    base = std::max (0, static_cast<int> (std::floor (delay)) - 1);
    const float d = delay - static_cast<float> (base);
    const float d1 = d - 1.0f, d2 = d - 2.0f, d3 = d - 3.0f;
    h[0] = -d1 * d2 * d3 * (1.0f / 6.0f);
    h[1] = d * d2 * d3 * 0.5f;
    h[2] = -d * d1 * d3 * 0.5f;
    h[3] = d * d1 * d2 * (1.0f / 6.0f);
}

/** Lagrange read ending at line index p (p = write index - base). Power-of-two
    mask; negative indices wrap correctly in two's complement. */
float readLagrange (const float* line, int p, int mask, const std::array<float, 4>& h) noexcept
{
    return h[0] * line[p & mask] + h[1] * line[(p - 1) & mask] + h[2] * line[(p - 2) & mask] + h[3] * line[(p - 3) & mask];
}

/** Coefficient interpolation with exact end points (t = 1 gives b bit-exactly). */
SvfCoeffs mixSvf (const SvfCoeffs& a, const SvfCoeffs& b, float t) noexcept
{
    const float u = 1.0f - t;
    SvfCoeffs c = b;
    c.a1 = u * a.a1 + t * b.a1;
    c.a2 = u * a.a2 + t * b.a2;
    c.a3 = u * a.a3 + t * b.a3;
    c.m0 = u * a.m0 + t * b.m0;
    c.m1 = u * a.m1 + t * b.m1;
    c.m2 = u * a.m2 + t * b.m2;
    return c;
}

/** Same for the first-order head-shadow sections (b2 = a2 = 0). Interpolating
    a1 between two stable poles keeps the pole inside the unit circle. */
BiquadCoeffs mixFirstOrder (const BiquadCoeffs& a, const BiquadCoeffs& b, double t) noexcept
{
    const double u = 1.0 - t;
    BiquadCoeffs c;
    c.b0 = u * a.b0 + t * b.b0;
    c.b1 = u * a.b1 + t * b.b1;
    c.a1 = u * a.a1 + t * b.a1;
    return c;
}

// Also resets non-finite state: one NaN / Inf input sample would otherwise
// latch in the recursive filters forever. The delay lines are FIR and flush
// themselves once overwritten, so the module recovers within one line length.
float flushed (float v) noexcept { return std::abs (v) < kStateFloor || ! std::isfinite (v) ? 0.0f : v; }
double flushed (double v) noexcept { return std::abs (v) < static_cast<double> (kStateFloor) || ! std::isfinite (v) ? 0.0 : v; }

void storeState (SvfState& state, const SvfState& s) noexcept
{
    state.ic1 = flushed (s.ic1);
    state.ic2 = flushed (s.ic2);
}

/** Both ears' dot products over one contiguous history window. Four partial
    sums per ear break the add dependency chain (and let the compiler use
    SIMD) while keeping a fixed, block-size independent summation order. */
void dotPair (const float* x, const float* hl, const float* hr, int n, float& outL, float& outR) noexcept
{
    float l0 = 0.0f, l1 = 0.0f, l2 = 0.0f, l3 = 0.0f;
    float r0 = 0.0f, r1 = 0.0f, r2 = 0.0f, r3 = 0.0f;
    int k = 0;
    for (; k + 4 <= n; k += 4)
    {
        l0 += x[k] * hl[k];
        l1 += x[k + 1] * hl[k + 1];
        l2 += x[k + 2] * hl[k + 2];
        l3 += x[k + 3] * hl[k + 3];
        r0 += x[k] * hr[k];
        r1 += x[k + 1] * hr[k + 1];
        r2 += x[k + 2] * hr[k + 2];
        r3 += x[k + 3] * hr[k + 3];
    }
    for (; k < n; ++k)
    {
        l0 += x[k] * hl[k];
        r0 += x[k] * hr[k];
    }
    outL = (l0 + l1) + (l2 + l3);
    outR = (r0 + r1) + (r2 + r3);
}
} // namespace

//==============================================================================
float HeadphoneVirtualizer::speakerAzimuthDeg (ChannelLayout layout, int channel, const VirtualizerParams& p) noexcept
{
    const VirtualizerParams s = sanitise (p);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float front = s.frontAngleDeg, side = s.sideAngleDeg, rear = s.rearAngleDeg;

    switch (sanitiseLayout (layout))
    {
        case ChannelLayout::Stereo:
        {
            const float az[] = { -front, front };
            return channel >= 0 && channel < 2 ? az[channel] : nan;
        }
        case ChannelLayout::Surround51:
        {
            // FL FR FC LFE SL SR
            const float az[] = { -front, front, 0.0f, nan, -side, side };
            return channel >= 0 && channel < 6 ? az[channel] : nan;
        }
        case ChannelLayout::Surround71:
        {
            // FL FR FC LFE BL BR SL SR
            const float az[] = { -front, front, 0.0f, nan, -rear, rear, -side, side };
            return channel >= 0 && channel < 8 ? az[channel] : nan;
        }
    }
    return nan;
}

//==============================================================================
bool HeadphoneVirtualizer::loadHrir()
{
    for (auto& path : hrirPaths)
    {
        path.history = {};
        path.reversed[0] = {};
        path.reversed[1] = {};
        path.present = false;
    }
    hrirLength = 0;
    hrirWrite = 0;

    if (hrir == nullptr)
        return false;

    const HrirSet& set = *hrir;
    if (static_cast<int> (set.layout) > static_cast<int> (ChannelLayout::Surround71) || set.length < 1)
        return false;

    // Measured responses are only valid at their own sample rate. Resampling
    // belongs to the (background) loader; a mismatched set falls back to the
    // parametric renderer instead of playing at the wrong pitch/geometry.
    if (! (std::abs (set.sampleRate - spec.sampleRate) <= 0.5))
        return false;

    // Entries follow the layout's channel order. The LFE entry of 5.1 / 7.1
    // may be present (ignored: the LFE always takes the low-pass path) or
    // omitted (then the entries are the layout's speakers without the LFE).
    const int numLayoutChannels = channelCount (set.layout);
    const int numEntries = static_cast<int> (set.left.size());
    const bool lfeOmitted = hasLfe (set.layout) && numEntries == numLayoutChannels - 1;
    if (static_cast<int> (set.right.size()) != numEntries || (numEntries != numLayoutChannels && ! lfeOmitted))
        return false;

    const int length = std::min (set.length, kMaxHrirTaps);
    const auto entryFor = [lfeOmitted] (int channel) { return lfeOmitted && channel > 3 ? channel - 1 : channel; };

    for (int c = 0; c < numLayoutChannels; ++c)
    {
        if (hasLfe (set.layout) && c == 3)
            continue;
        const auto e = static_cast<size_t> (entryFor (c));
        for (const auto* ir : { &set.left[e], &set.right[e] })
        {
            if (static_cast<int> (ir->size()) < length)
                return false;
            for (int k = 0; k < length; ++k)
                if (! std::isfinite ((*ir)[static_cast<size_t> (k)]))
                    return false;
        }
    }

    hrirLength = length;
    hrirLayout = set.layout;
    for (int c = 0; c < numLayoutChannels; ++c)
    {
        if (hasLfe (set.layout) && c == 3)
            continue;
        const auto e = static_cast<size_t> (entryFor (c));
        auto& path = hrirPaths[static_cast<size_t> (c)];
        const std::vector<float>* irs[2] = { &set.left[e], &set.right[e] };
        for (size_t ear = 0; ear < 2; ++ear)
        {
            // Time-reversed so the convolution is a forward dot product with
            // the (oldest-first) history window.
            path.reversed[ear].assign (static_cast<size_t> (length), 0.0f);
            for (int k = 0; k < length; ++k)
                path.reversed[ear][static_cast<size_t> (length - 1 - k)] = (*irs[ear])[static_cast<size_t> (k)];
        }
        path.history.assign (2 * static_cast<size_t> (length), 0.0f);
        path.present = true;
    }
    return true;
}

void HeadphoneVirtualizer::prepare (const ProcessSpec& newSpec)
{
    spec = newSpec;
    spec.numChannels = std::clamp (spec.numChannels, 1, kMaxChannels);
    spec.maxBlockSize = std::max (1, spec.maxBlockSize);
    // Non-finite or absurd rates would overflow the delay-line sizes (and hang
    // nextPowerOfTwo) or give an empty SVF frequency range.
    if (! (spec.sampleRate > 0.0) || ! std::isfinite (spec.sampleRate))
        spec.sampleRate = 48000.0;
    spec.sampleRate = std::clamp (spec.sampleRate, 8000.0, 768000.0);
    const double fs = spec.sampleRate;

    // ITD lines: the longest Woodworth delay (a = 105 mm, theta = 180 deg) is
    // (a/c)(1 + pi/2) = 787 us = 151 samples at 192 kHz. Room for the 4
    // Lagrange taps plus margin, rounded up to a power of two (256), so the
    // read index is a mask instead of a branch.
    const double maxTau = static_cast<double> (kMaxHeadMm) * 0.001 / kSpeedOfSound * (1.0 + 0.5 * kPi);
    const int maxDelay = static_cast<int> (std::ceil (maxTau * std::max (fs, 192000.0)));
    const int itdSize = nextPowerOfTwo (maxDelay + 8);
    itdMask = itdSize - 1;
    maxDelaySamples = static_cast<float> (itdSize - 5); // safety clamp only; never reached below 192 kHz
    for (auto& line : itdLines)
        line.assign (static_cast<size_t> (itdSize), 0.0f);
    itdWrite = 0;

    // Early reflections (integer taps: static delays need no interpolation).
    for (size_t k = 0; k < reflDelay.size(); ++k)
        reflDelay[k] = std::max (1, msToSamples (kReflectionMs[k], fs));
    const int reflSize = nextPowerOfTwo (*std::max_element (reflDelay.begin(), reflDelay.end()) + 1);
    reflMask = reflSize - 1;
    reflLine.assign (static_cast<size_t> (reflSize), 0.0f);
    reflWrite = 0;
    reflHpCoeffs = SvfCoeffs::make (FilterType::HighPass, kReflectionHpHz, butterworthQ (1, 0), 0.0, fs);
    reflLpCoeffs = SvfCoeffs::make (FilterType::LowPass, kReflectionLpHz, butterworthQ (1, 0), 0.0, fs);

    // LFE: 4th-order Butterworth = two SVF low-pass sections.
    for (int s = 0; s < 2; ++s)
        lfeCoeffs[static_cast<size_t> (s)] = SvfCoeffs::make (FilterType::LowPass, kLfeCutoffHz, butterworthQ (2, s), 0.0, fs);

    const auto scratch = static_cast<size_t> (spec.maxBlockSize);
    accL.assign (scratch, 0.0f);
    accR.assign (scratch, 0.0f);
    bus.assign (scratch, 0.0f);

    // Geometry smoothers step once per control period, so their coefficient
    // is computed for the control rate.
    const double controlRate = fs / kControlInterval;
    frontAngle.reset (controlRate, kGeometrySmoothingMs, params.frontAngleDeg);
    sideAngle.reset (controlRate, kGeometrySmoothingMs, params.sideAngleDeg);
    rearAngle.reset (controlRate, kGeometrySmoothingMs, params.rearAngleDeg);
    headRadius.reset (controlRate, kGeometrySmoothingMs, params.headRadiusMm);
    for (auto& sp : speakers)
        sp.shelfDb.reset (controlRate, kShelfSmoothingMs, 0.0f);
    lfeGain.reset (fs, kGainRampMs, dbToGain (params.lfeGainDb));
    roomGain.reset (fs, kGainRampMs, params.roomAmount);

    // The swap fade is a whole number of control periods, so a fade that
    // starts on a tick lands exactly on 0 / 1 on a later tick.
    const long periods = std::lround (static_cast<double> (kFadeMs) * 0.001 * fs / kControlInterval);
    fadeSamples = std::max (1, static_cast<int> (periods)) * kControlInterval;

    hrirValid = loadHrir();

    // Pre-roll after a swap, rounded up to whole control periods: at least the
    // longest ITD path plus the Lagrange taps (151 + 3 samples at 192 kHz, far
    // below 2 ms) and the shelf / shadow start-up. An HRIR needs its length
    // (capped: late taps are small, and the fade-in covers them).
    const auto periodsFor = [] (double samples)
    { return std::max (1, static_cast<int> (std::ceil (samples / kControlInterval))) * kControlInterval; };
    holdParametric = periodsFor (static_cast<double> (kPrerollMs) * 0.001 * fs);
    holdHrir = std::max (holdParametric, periodsFor (std::min (static_cast<double> (hrirLength), static_cast<double> (kMaxHrirPrerollMs) * 0.001 * fs)));

    reset();
}

void HeadphoneVirtualizer::reset() noexcept
{
    swapLayout();
    reflHpState.reset();
    reflLpState.reset();
    std::fill (reflLine.begin(), reflLine.end(), 0.0f);
    fadePos = fadeSamples;
    fadeDir = 0;
    holdRemaining = 0;
    lfeGain.setImmediate (lfeGain.getTarget());
    roomGain.setImmediate (roomGain.getTarget());
    samplesToTick = 0;
    rampPos = 0;
    ramping = false;
    busy = false;
}

//==============================================================================
void HeadphoneVirtualizer::setParams (const VirtualizerParams& p) noexcept
{
    const VirtualizerParams s = sanitise (p);
    if (s == params)
        return; // the chain pushes parameters every block: unchanged is free

    params = s;
    frontAngle.setTarget (s.frontAngleDeg);
    sideAngle.setTarget (s.sideAngleDeg);
    rearAngle.setTarget (s.rearAngleDeg);
    headRadius.setTarget (s.headRadiusMm);
    lfeGain.setTarget (dbToGain (s.lfeGainDb));
    roomGain.setTarget (s.roomAmount);
    // Geometry glides and layout swaps run on the control ticks, which sit at
    // fixed positions in the stream.
    busy = true;
}

//==============================================================================
void HeadphoneVirtualizer::updateGeometry (bool snap) noexcept
{
    VirtualizerParams p = params;
    p.frontAngleDeg = frontAngle.getCurrent();
    p.sideAngleDeg = sideAngle.getCurrent();
    p.rearAngleDeg = rearAngle.getCurrent();

    const double fs = spec.sampleRate;
    const double radius = static_cast<double> (headRadius.getCurrent()) * 0.001;
    const double headDelay = radius / kSpeedOfSound * fs; // a/c in samples
    const double w0 = kSpeedOfSound / radius;

    for (int c = 0; c < kMaxChannels; ++c)
    {
        auto& sp = speakers[static_cast<size_t> (c)];
        if (sp.role != Role::Speaker)
            continue;

        const float az = speakerAzimuthDeg (runningLayout, c, p);
        sp.azimuth = az;
        for (size_t e = 0; e < 2; ++e)
        {
            // Angle between the source and this ear's axis (ears at -90 / +90 deg).
            const double earAz = e == 0 ? -90.0 : 90.0;
            const double thetaDeg = std::abs (std::remainder (static_cast<double> (az) - earAz, 360.0));
            auto& ear = sp.ears[e];
            ear.delay = std::clamp (static_cast<float> (woodworth (thetaDeg * (kPi / 180.0)) * headDelay), 0.0f, maxDelaySamples);
            lagrangeTaps (ear.delay, ear.base, ear.taps);
            ear.shadow = BiquadCoeffs::fromAnalogFirstOrder (1.0, shadowAlpha (thetaDeg) / (2.0 * w0), 1.0, 1.0 / (2.0 * w0), fs);
            if (snap)
            {
                ear.prevDelay = ear.delay;
                ear.prevShadow = ear.shadow;
            }
        }

        const float shelfTarget = std::abs (az) > 90.0f ? kRearShelfDb : 0.0f;
        if (snap)
        {
            sp.shelfDb.setImmediate (shelfTarget);
            sp.shelf = SvfCoeffs::make (FilterType::HighShelf, kRearShelfHz, kRearShelfQ, static_cast<double> (shelfTarget), fs);
            sp.prevShelf = sp.shelf;
        }
        else
        {
            // Crossing 90 deg is a discrete change of the cue: the shelf gain
            // glides (10 ms) instead of stepping.
            sp.shelfDb.setTarget (shelfTarget);
        }
    }
}

void HeadphoneVirtualizer::clearChannel (int channel) noexcept
{
    auto& sp = speakers[static_cast<size_t> (channel)];
    sp.shelfState.reset();
    for (auto& ear : sp.ears)
        ear.state.reset();
    auto& line = itdLines[static_cast<size_t> (channel)];
    std::fill (line.begin(), line.end(), 0.0f);
    auto& path = hrirPaths[static_cast<size_t> (channel)];
    std::fill (path.history.begin(), path.history.end(), 0.0f);
    if (sp.role == Role::Lfe)
        for (auto& s : lfeState)
            s.reset();
    sp.clean = true;
}

void HeadphoneVirtualizer::clearState() noexcept
{
    // Per-channel paths and the LFE only. The reflection line is fed by the
    // mono speaker sum and stays continuous across a swap (reset() clears it).
    for (int c = 0; c < kMaxChannels; ++c)
        clearChannel (c);
    for (auto& s : lfeState)
        s.reset();
}

void HeadphoneVirtualizer::swapLayout() noexcept
{
    // Only called while the output is silent (swap fade at 0) or from reset(),
    // so everything may jump: new roles and renderer, geometry at its targets,
    // clean per-channel state. A silent pre-roll then lets the new paths fill
    // before the fade-in, which hides the rest of the filters' start-up.
    runningLayout = params.layout;
    const bool lfe = hasLfe (runningLayout);
    const int numLayoutChannels = channelCount (runningLayout);
    for (int c = 0; c < kMaxChannels; ++c)
    {
        auto& sp = speakers[static_cast<size_t> (c)];
        sp.role = c >= numLayoutChannels ? Role::None : (lfe && c == 3 ? Role::Lfe : Role::Speaker);
    }
    useHrir = hrirValid && hrirLayout == runningLayout;

    frontAngle.setImmediate (frontAngle.getTarget());
    sideAngle.setImmediate (sideAngle.getTarget());
    rearAngle.setImmediate (rearAngle.getTarget());
    headRadius.setImmediate (headRadius.getTarget());
    updateGeometry (true);
    clearState();
    fadeDir = 0;
    holdRemaining = useHrir ? holdHrir : holdParametric;
}

void HeadphoneVirtualizer::tick() noexcept
{
    // A coefficient ramp spans exactly one control period, which ends here:
    // the current design becomes the start point of any new ramp.
    for (auto& sp : speakers)
    {
        sp.prevShelf = sp.shelf;
        for (auto& ear : sp.ears)
        {
            ear.prevDelay = ear.delay;
            ear.prevShadow = ear.shadow;
        }
    }
    ramping = false;
    rampPos = 0;

    // 1. Layout: a discrete change (channel meaning, renderer). Fade out, swap
    //    at silence, pre-roll silently, fade back in. Returning to the running
    //    layout while the fade-out is still going simply fades back in; a new
    //    change during the pre-roll swaps again at once (still silent).
    if (params.layout != runningLayout)
    {
        if (fadePos == 0)
            swapLayout();
        else
            fadeDir = -1;
    }
    else if (holdRemaining > 0)
    {
        fadeDir = 0;
    }
    else
    {
        fadeDir = fadePos < fadeSamples ? 1 : 0;
    }

    // 2. Continuous geometry: glide angles and head radius, redesign delays and
    //    head-shadow filters, ramp to them across the coming control period.
    if (frontAngle.isSmoothing() || sideAngle.isSmoothing() || rearAngle.isSmoothing() || headRadius.isSmoothing())
    {
        frontAngle.next();
        sideAngle.next();
        rearAngle.next();
        headRadius.next();
        updateGeometry (false);
        ramping = true;
    }

    // 3. Rear-cue shelves (targets set by updateGeometry()).
    bool shelvesMoving = false;
    for (auto& sp : speakers)
    {
        if (sp.role != Role::Speaker || ! sp.shelfDb.isSmoothing())
            continue;
        const float db = sp.shelfDb.next();
        sp.shelf = SvfCoeffs::make (FilterType::HighShelf, kRearShelfHz, kRearShelfQ, static_cast<double> (db), spec.sampleRate);
        ramping = true;
        shelvesMoving = shelvesMoving || sp.shelfDb.isSmoothing();
    }

    busy = ramping || shelvesMoving || fadeDir != 0 || holdRemaining > 0 || params.layout != runningLayout || frontAngle.isSmoothing()
           || sideAngle.isSmoothing() || rearAngle.isSmoothing() || headRadius.isSmoothing();
}

//==============================================================================
template <bool Ramp>
void HeadphoneVirtualizer::renderParametric (Speaker& sp, float* line, const float* x, int length) noexcept
{
    // One fused per-sample loop: the ITD line (256 samples) is shorter than a
    // block, so each sample must be read back before the line wraps.
    float* const outL = accL.data();
    float* const outR = accR.data();
    const int mask = itdMask;
    const int write0 = itdWrite;
    EarPath& el = sp.ears[0];
    EarPath& er = sp.ears[1];

    // Local copies keep coefficients and state in registers (the output
    // pointers could otherwise alias them).
    SvfCoeffs shelf = sp.shelf;
    SvfState shelfState = sp.shelfState;
    BiquadCoeffs cl = el.shadow, cr = er.shadow;
    BiquadState sl = el.state, sr = er.state;
    std::array<float, 4> hl = el.taps, hr = er.taps;
    int bl = el.base, br = er.base;

    for (int i = 0; i < length; ++i)
    {
        if constexpr (Ramp)
        {
            // Position inside the control period -> exact end point at 16.
            const float t = static_cast<float> (std::min (rampPos + i + 1, kControlInterval)) * (1.0f / kControlInterval);
            shelf = mixSvf (sp.prevShelf, sp.shelf, t);
            lagrangeTaps ((1.0f - t) * el.prevDelay + t * el.delay, bl, hl);
            lagrangeTaps ((1.0f - t) * er.prevDelay + t * er.delay, br, hr);
            cl = mixFirstOrder (el.prevShadow, el.shadow, static_cast<double> (t));
            cr = mixFirstOrder (er.prevShadow, er.shadow, static_cast<double> (t));
        }

        const int w = write0 + i;
        line[w & mask] = svfTick (shelf, shelfState, x[i]);
        outL[i] += static_cast<float> (biquadTick (cl, sl, static_cast<double> (readLagrange (line, w - bl, mask, hl))));
        outR[i] += static_cast<float> (biquadTick (cr, sr, static_cast<double> (readLagrange (line, w - br, mask, hr))));
    }

    storeState (sp.shelfState, shelfState);
    el.state.z1 = flushed (sl.z1);
    er.state.z1 = flushed (sr.z1);
}

void HeadphoneVirtualizer::renderHrir (HrirPath& path, const float* x, int length) noexcept
{
    // Direct-form convolution. history[p + 1 .. p + n] always holds the newest
    // n inputs oldest-first (every sample is written twice, n apart), so each
    // output is one contiguous dot product with the time-reversed HRIR.
    const int n = hrirLength;
    float* const hist = path.history.data();
    const float* const hl = path.reversed[0].data();
    const float* const hr = path.reversed[1].data();
    float* const outL = accL.data();
    float* const outR = accR.data();

    int p = hrirWrite;
    for (int i = 0; i < length; ++i)
    {
        hist[p] = x[i];
        hist[p + n] = x[i];
        float yl = 0.0f, yr = 0.0f;
        dotPair (hist + p + 1, hl, hr, n, yl, yr);
        outL[i] += yl;
        outR[i] += yr;
        if (++p == n)
            p = 0;
    }
}

void HeadphoneVirtualizer::renderLfe (const float* x, int length) noexcept
{
    const SvfCoeffs c0 = lfeCoeffs[0], c1 = lfeCoeffs[1];
    SvfState s0 = lfeState[0], s1 = lfeState[1];
    float* const outL = accL.data();
    float* const outR = accR.data();
    for (int i = 0; i < length; ++i)
    {
        const float y = svfTick (c1, s1, svfTick (c0, s0, x[i])) * lfeGain.next();
        outL[i] += y;
        outR[i] += y;
    }
    storeState (lfeState[0], s0);
    storeState (lfeState[1], s1);
}

void HeadphoneVirtualizer::renderReflections (int length) noexcept
{
    const SvfCoeffs hp = reflHpCoeffs, lp = reflLpCoeffs;
    SvfState hs = reflHpState, ls = reflLpState;
    const float* const src = bus.data();
    float* const line = reflLine.data();
    float* const outL = accL.data();
    float* const outR = accR.data();
    const int mask = reflMask;
    int w = reflWrite;

    // The line is always fed (cheap) so raising the room level later starts
    // from real history rather than stale samples.
    if (! roomGain.isSmoothing() && roomGain.getCurrent() == 0.0f)
    {
        for (int i = 0; i < length; ++i, w = (w + 1) & mask)
            line[w] = svfTick (lp, ls, svfTick (hp, hs, src[i]));
    }
    else
    {
        const std::array<int, kNumReflections> d = reflDelay;
        constexpr auto g = kReflectionWeight;
        for (int i = 0; i < length; ++i, w = (w + 1) & mask)
        {
            line[w] = svfTick (lp, ls, svfTick (hp, hs, src[i]));
            const float room = roomGain.next();
            const float yl = g[0] * line[(w - d[0]) & mask] + g[2] * line[(w - d[2]) & mask] + g[4] * line[(w - d[4]) & mask];
            const float yr = g[1] * line[(w - d[1]) & mask] + g[3] * line[(w - d[3]) & mask] + g[5] * line[(w - d[5]) & mask];
            outL[i] += room * yl;
            outR[i] += room * yr;
        }
    }

    reflWrite = w;
    storeState (reflHpState, hs);
    storeState (reflLpState, ls);
}

void HeadphoneVirtualizer::renderSegment (const AudioBlock& block, int start, int length, int numInputs) noexcept
{
    float* const l = accL.data();
    float* const r = accR.data();
    float* const sum = bus.data();
    std::fill_n (l, length, 0.0f);
    std::fill_n (r, length, 0.0f);
    std::fill_n (sum, length, 0.0f);

    // Every input is read (into the accumulators) before any output is
    // written, so in-place processing is safe although ch 0/1 are both
    // speaker inputs and ear outputs.
    bool lfeRendered = false;
    for (int c = 0; c < kMaxChannels; ++c)
    {
        auto& sp = speakers[static_cast<size_t> (c)];
        if (sp.role == Role::None)
            continue;
        if (c >= numInputs)
        {
            // Channel missing from this block: silence. Clear its history once
            // so it restarts cleanly if it comes back.
            if (! sp.clean)
                clearChannel (c);
            continue;
        }

        sp.clean = false;
        const float* x = block.channel (c) + start;
        if (sp.role == Role::Lfe)
        {
            renderLfe (x, length);
            lfeRendered = true;
            continue;
        }

        for (int i = 0; i < length; ++i)
            sum[i] += x[i];

        if (useHrir)
        {
            if (hrirPaths[static_cast<size_t> (c)].present)
                renderHrir (hrirPaths[static_cast<size_t> (c)], x, length);
        }
        else if (ramping)
        {
            renderParametric<true> (sp, itdLines[static_cast<size_t> (c)].data(), x, length);
        }
        else
        {
            renderParametric<false> (sp, itdLines[static_cast<size_t> (c)].data(), x, length);
        }
    }
    if (! lfeRendered)
        lfeGain.skip (length); // keep the ramp in stream time

    renderReflections (length);

    itdWrite = (itdWrite + length) & itdMask;
    if (hrirLength > 0)
        hrirWrite = (hrirWrite + length) % hrirLength;

    // Output: trim (and the swap fade), binaural in ch 0/1, the rest cleared.
    const bool fading = fadeDir != 0 || fadePos != fadeSamples;
    const float invFade = 1.0f / static_cast<float> (fadeSamples);
    const auto gainAt = [this, fading, invFade] (int i) noexcept
    {
        if (! fading)
            return kTrim;
        // Integer position -> exact 0 / 1 end points.
        const int pos = std::clamp (fadePos + fadeDir * (i + 1), 0, fadeSamples);
        return kTrim * static_cast<float> (pos) * invFade;
    };

    float* const outL = block.channel (0) + start;
    if (numInputs >= 2)
    {
        float* const outR = block.channel (1) + start;
        for (int i = 0; i < length; ++i)
        {
            const float g = gainAt (i);
            outL[i] = l[i] * g;
            outR[i] = r[i] * g;
        }
    }
    else
    {
        // Mono host bus: the mono fold-down of the binaural pair.
        for (int i = 0; i < length; ++i)
            outL[i] = 0.5f * (l[i] + r[i]) * gainAt (i);
    }

    for (int c = 2; c < numInputs; ++c)
        std::fill_n (block.channel (c) + start, length, 0.0f);
}

void HeadphoneVirtualizer::process (const AudioBlock& block) noexcept
{
    const int numSamples = block.numSamples;
    const int numInputs = std::min (block.numChannels, kMaxChannels);
    const int scratch = static_cast<int> (accL.size());
    if (numSamples <= 0 || numInputs <= 0 || scratch == 0)
        return;

    int pos = 0;
    while (pos < numSamples)
    {
        if (samplesToTick == 0)
        {
            if (busy)
                tick();
            samplesToTick = kControlInterval;
        }

        // While nothing is busy every tick would be a no-op, so the rest of the
        // block is one segment (identical output, fewer loop set-ups).
        const int len = std::min (busy ? std::min (samplesToTick, numSamples - pos) : numSamples - pos, scratch);
        renderSegment (block, pos, len, numInputs);

        if (ramping)
            rampPos = std::min (rampPos + len, kControlInterval);
        if (fadeDir != 0)
            fadePos = std::clamp (fadePos + fadeDir * len, 0, fadeSamples);
        else if (holdRemaining > 0)
            holdRemaining = std::max (0, holdRemaining - len);

        // Keep the tick grid aligned to absolute stream time.
        samplesToTick = ((samplesToTick - len) % kControlInterval + kControlInterval) % kControlInterval;
        pos += len;
    }
}
} // namespace flub
