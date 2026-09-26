#include "flub/dsp/DynamicEq.h"

#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>

namespace flub
{
namespace
{
// Continuous parameters (frequency, Q, threshold, ratio, range, static gain,
// noise floor) glide with this one-pole time constant, so automation, macro
// moves and A/B switches never step the gain computer or the filter geometry.
constexpr float kParamSmoothMs = 20.0f;

// Enable / disable and mode / shape swaps: the band's total gain (dB) is
// multiplied by a linear 0..1 fade over this time. At 0 dB every EQ shape used
// here is an exact identity (m0 = 1, m1 = m2 = 0), so at the bottom of the
// fade the band can be removed or re-typed without any discontinuity.
constexpr float kFadeMs = 20.0f;

// Peak-hold window limits. The window must span at least half a period of the
// lowest frequency the detector passes, otherwise the envelope ripples at twice
// the signal frequency and modulates the EQ gain (intermodulation). 25 ms
// covers 20 Hz (the low-shelf detector always uses it, see updateDetector());
// 1 ms keeps treble bands snappy while still spanning several cycles, which
// also averages out sample-peak vs true-peak jitter.
constexpr double kMinHoldSeconds = 0.001;
constexpr double kMaxHoldSeconds = 0.025;

// The shelf detectors (low-pass / high-pass) must never resonate above unity,
// or the level would read hot near the corner: their Q is capped at Butterworth.
constexpr double kMaxShelfDetectorQ = 0.70710678118654752;

// The boost of BoostBelow fades out over the 10 dB above the noise floor.
constexpr float kFloorTaperDb = 10.0f;

// Recursive states below this are flushed so silence cannot leave subnormals
// circulating (the host also sets FTZ/DAZ, this is a cheap second line).
constexpr float kStateFlush = 1.0e-20f;
constexpr float kEnvFloor = 1.0e-9f; // -180 dB, below kMinusInfDb

constexpr DynEqBandParams kDefaultBandParams {};

// Inter-sample peak estimate: a sample-peak detector reads a tone near fs/4
// up to 3 dB low (two samples straddling the crest at +-45 degrees), and a
// tone slightly off fs/4 (or fs/3, fs/6 ...) makes that error beat slowly,
// i.e. the level ripples and the band tremolos a steady 11-12 kHz tone. The
// midpoint between two detector samples is estimated with the maximally flat
// 6-point (Lagrange) half-sample interpolator (3/256, -25/256, 150/256, 150/256,
// -25/256, 3/256); max(|samples|, |midpoints|) keeps the worst-case under-read
// of a sine below 0.7 dB up to fs/4 and never over-reads (|H(w)| <= 1). The
// midpoint lags by 2.5 samples; the sample peak itself is still instant.
constexpr float kMid1 = 150.0f / 256.0f;
constexpr float kMid3 = -25.0f / 256.0f;
constexpr float kMid5 = 3.0f / 256.0f;

/** Flushes near-zero SVF states; returns their sum for a cheap finiteness check. */
float flushTiny (SvfState& s) noexcept
{
    if (std::abs (s.ic1) < kStateFlush)
        s.ic1 = 0.0f;
    if (std::abs (s.ic2) < kStateFlush)
        s.ic2 = 0.0f;
    return s.ic1 + s.ic2;
}

float sanitise (float v, float lo, float hi, float fallback) noexcept
{
    return std::isfinite (v) ? std::clamp (v, lo, hi) : fallback;
}

bool isExpander (DynEqMode mode) noexcept
{
    // Level up -> gain up: a rising gain is the attack (see GainSmoother).
    return mode == DynEqMode::BoostAbove || mode == DynEqMode::CutBelow;
}

/** y = v0: the EQ section passes its input unchanged, whatever its state. */
bool isIdentity (const SvfCoeffs& c) noexcept
{
    return c.m0 == 1.0f && c.m1 == 0.0f && c.m2 == 0.0f;
}

FilterType eqFilterType (EqBandType shape) noexcept
{
    switch (shape)
    {
        case EqBandType::LowShelf:  return FilterType::LowShelf;
        case EqBandType::HighShelf: return FilterType::HighShelf;
        default:                    return FilterType::Bell;
    }
}

/** Static gain computer (dB). level/threshold in dBFS, result in dB. */
float computeDynamicGainDb (DynEqMode mode, float levelDb, float thresholdDb, float ratio, float rangeDb, float noiseFloorDb) noexcept
{
    const float over = levelDb - thresholdDb;
    switch (mode)
    {
        case DynEqMode::CutAbove:
            return -std::min (rangeDb, std::max (0.0f, over) * (1.0f - 1.0f / ratio));
        case DynEqMode::BoostBelow:
        {
            // Never lift silence or hiss: the boost fades to 0 between
            // noiseFloor + 10 dB and the noise floor.
            const float taper = std::clamp ((levelDb - noiseFloorDb) / kFloorTaperDb, 0.0f, 1.0f);
            return std::min (rangeDb, std::max (0.0f, -over) * (1.0f - 1.0f / ratio)) * taper;
        }
        case DynEqMode::BoostAbove:
            return std::min (rangeDb, std::max (0.0f, over) * (ratio - 1.0f));
        case DynEqMode::CutBelow:
            return -std::min (rangeDb, std::max (0.0f, -over) * (ratio - 1.0f));
    }
    return 0.0f;
}
} // namespace

//==============================================================================
void DynamicEq::prepare (const ProcessSpec& newSpec)
{
    spec = newSpec;
    spec.numChannels = std::clamp (spec.numChannels, 1, kMaxChannels);
    controlRate = spec.sampleRate / static_cast<double> (kControlInterval);
    reset();
}

void DynamicEq::reset() noexcept
{
    controlCountdown = kControlInterval;
    lastNumChannels = spec.numChannels;
    for (int b = 0; b < kMaxBands; ++b)
    {
        if (targets[static_cast<size_t> (b)].enabled)
        {
            // After a reset there is no previous output to click against, so
            // enabled bands start fully faded in at their target settings.
            activateBand (b, false);
        }
        else
        {
            auto& band = bands[static_cast<size_t> (b)];
            band.active = false;
            clearBandState (band);
            appliedGainDb[static_cast<size_t> (b)].store (0.0f, std::memory_order_relaxed);
        }
    }
}

//==============================================================================
void DynamicEq::setBand (int index, const DynEqBandParams& params) noexcept
{
    if (index < 0 || index >= kMaxBands)
        return;

    const size_t idx = static_cast<size_t> (index);
    const DynEqBandParams& prev = targets[idx];
    DynEqBandParams p = params;

    if (static_cast<uint8_t> (p.mode) > static_cast<uint8_t> (DynEqMode::CutBelow))
        p.mode = DynEqMode::CutAbove;
    if (p.shape != EqBandType::Bell && p.shape != EqBandType::LowShelf && p.shape != EqBandType::HighShelf)
        p.shape = EqBandType::Bell;

    // Non-finite values keep the last valid setting.
    p.frequency = sanitise (p.frequency, 20.0f, 20000.0f, prev.frequency);
    p.q = sanitise (p.q, 0.1f, 10.0f, prev.q);
    p.thresholdDb = sanitise (p.thresholdDb, -80.0f, 0.0f, prev.thresholdDb);
    p.ratio = sanitise (p.ratio, 1.0f, 20.0f, prev.ratio);
    p.rangeDb = sanitise (p.rangeDb, 0.0f, 24.0f, prev.rangeDb);
    p.staticGainDb = sanitise (p.staticGainDb, -12.0f, 12.0f, prev.staticGainDb);
    p.attackMs = sanitise (p.attackMs, 0.1f, 200.0f, prev.attackMs);
    p.releaseMs = sanitise (p.releaseMs, 5.0f, 2000.0f, prev.releaseMs);
    p.noiseFloorDb = sanitise (p.noiseFloorDb, -120.0f, 0.0f, prev.noiseFloorDb);

    if (p == prev)
        return; // the chain pushes every block; unchanged values cost nothing

    targets[idx] = p;
    auto& band = bands[idx];

    if (! band.active)
    {
        // An idle band is silent, so it simply starts at its new settings;
        // only the fade-in is needed to make the switch-on click-free.
        if (p.enabled)
            activateBand (index, true);
        return;
    }

    // Running band: continuous values glide; discrete ones (enable, mode,
    // shape) are picked up by controlTick() through the fade.
    band.logFreq.setTarget (std::log (p.frequency));
    band.logQ.setTarget (std::log (p.q));
    band.thresholdDb.setTarget (p.thresholdDb);
    band.ratio.setTarget (p.ratio);
    band.rangeDb.setTarget (p.rangeDb);
    band.staticGainDb.setTarget (p.staticGainDb);
    band.noiseFloorDb.setTarget (p.noiseFloorDb);
    band.dynGain.setTimes (p.attackMs, p.releaseMs);
}

const DynEqBandParams& DynamicEq::getBand (int index) const noexcept
{
    if (index < 0 || index >= kMaxBands)
        return kDefaultBandParams;
    return targets[static_cast<size_t> (index)];
}

float DynamicEq::getBandGainDb (int index) const noexcept
{
    if (index < 0 || index >= kMaxBands)
        return 0.0f;
    return appliedGainDb[static_cast<size_t> (index)].load (std::memory_order_relaxed);
}

//==============================================================================
void DynamicEq::clearBandState (BandState& band) noexcept
{
    for (auto& s : band.detState)
        s.reset();
    for (auto& s : band.eqState)
        s.reset();
    for (auto& h : band.detHistory)
        h.fill (0.0f);
    band.segmentPeak = band.windowPeak = band.prevWindowPeak = band.env = 0.0f;
    band.windowCountdown = band.windowTicks;
}

void DynamicEq::activateBand (int index, bool fadeIn) noexcept
{
    const size_t idx = static_cast<size_t> (index);
    const DynEqBandParams& t = targets[idx];
    auto& band = bands[idx];

    band.mode = t.mode;
    band.shape = t.shape;
    band.freq = t.frequency;
    band.q = t.q;
    band.logFreq.reset (controlRate, kParamSmoothMs, std::log (t.frequency));
    band.logQ.reset (controlRate, kParamSmoothMs, std::log (t.q));
    band.thresholdDb.reset (controlRate, kParamSmoothMs, t.thresholdDb);
    band.ratio.reset (controlRate, kParamSmoothMs, t.ratio);
    band.rangeDb.reset (controlRate, kParamSmoothMs, t.rangeDb);
    band.staticGainDb.reset (controlRate, kParamSmoothMs, t.staticGainDb);
    band.noiseFloorDb.reset (controlRate, kParamSmoothMs, t.noiseFloorDb);
    band.dynGain.prepare (controlRate, t.attackMs, t.releaseMs, isExpander (t.mode));
    band.fade.reset (controlRate, kFadeMs, fadeIn ? 0.0f : 1.0f);

    updateDetector (band);
    clearBandState (band);

    const float totalDb = band.fade.getCurrent() * t.staticGainDb;
    updateEq (band, totalDb, false);
    appliedGainDb[idx].store (totalDb, std::memory_order_relaxed);
    band.active = true;
}

void DynamicEq::updateDetector (BandState& band) const noexcept
{
    // Unity-gain sidechain filter matching the region the EQ shape acts on.
    // The peak-hold window must cover half a period of the lowest frequency
    // that still reaches the detector at a significant level.
    const double freq = SvfCoeffs::clampFrequency (band.freq, spec.sampleRate);
    FilterType type = FilterType::BandPass;
    double detQ = band.q;
    double holdSeconds = kMaxHoldSeconds;
    switch (band.shape)
    {
        case EqBandType::LowShelf:
            // The low-pass passes everything below f at unity, all the way
            // down to the deepest bass, whatever f is: a window tied to f
            // would let a 30 Hz note ripple the level of a 500 Hz shelf and
            // distort the very bass it is shaping. Always hold for 20 Hz.
            type = FilterType::LowPass;
            detQ = std::min (detQ, kMaxShelfDetectorQ);
            holdSeconds = kMaxHoldSeconds;
            break;
        case EqBandType::HighShelf:
            // Four periods of f = half a period of f/8, where the 12 dB/oct
            // skirt is 36 dB down: loud bass under a treble shelf cannot
            // ripple its level.
            type = FilterType::HighPass;
            detQ = std::min (detQ, kMaxShelfDetectorQ);
            holdSeconds = 4.0 / freq;
            break;
        default:
        {
            // Two periods of the band-pass's lower -3 dB edge
            // fl = f (sqrt(1 + 1/(4Q^2)) - 1/(2Q)), i.e. half a period of
            // fl/4, where the skirt is 16 dB down at Q = 1 (12 dB at Q = 0.1,
            // 32 dB at Q = 10). Loud bass leaking through the skirt then
            // barely ripples the level of a mid band: a -6 dBFS 100 Hz note
            // under an acting 1 kHz band leaves intermod sidebands near
            // -90 dBc at an 80 ms release (half this window: -62 dBc).
            const double halfBw = 0.5 / detQ;
            holdSeconds = 2.0 / (freq * (std::sqrt (1.0 + halfBw * halfBw) - halfBw));
            break;
        }
    }
    holdSeconds = std::clamp (holdSeconds, kMinHoldSeconds, kMaxHoldSeconds);

    band.detCoeffs = SvfCoeffs::make (type, band.freq, detQ, 0.0, spec.sampleRate);
    band.windowTicks = std::max (1, static_cast<int> (std::lround (holdSeconds * controlRate)));
    band.windowCountdown = std::min (band.windowCountdown, band.windowTicks);
    // After the hold, the envelope decays with a time constant of one window,
    // which turns the bucket staircase of a decaying signal into a smooth fall.
    band.envRelease = static_cast<float> (std::exp (-1.0 / static_cast<double> (band.windowTicks)));
}

void DynamicEq::updateEq (BandState& band, float totalDb, bool glide) const noexcept
{
    const SvfCoeffs prev = band.eqCoeffs;
    const SvfCoeffs next = SvfCoeffs::make (eqFilterType (band.shape), band.freq, band.q, totalDb, spec.sampleRate);
    band.eqCoeffs = next;
    band.coeffGainDb = totalDb;

    // Nothing to glide if either the set is unchanged or both ends are the
    // 0 dB identity (then g / k only steer the hidden state, not the output).
    const bool changed = prev.g != next.g || prev.k != next.k || prev.m0 != next.m0 || prev.m1 != next.m1 || prev.m2 != next.m2;
    band.eqRamping = glide && changed && ! (isIdentity (prev) && isIdentity (next));
    if (band.eqRamping)
    {
        band.rampStart = { static_cast<float> (prev.g), static_cast<float> (prev.k), prev.m0, prev.m1, prev.m2 };
        band.rampDelta = { static_cast<float> (next.g - prev.g), static_cast<float> (next.k - prev.k),
                           next.m0 - prev.m0, next.m1 - prev.m1, next.m2 - prev.m2 };
    }
}

//==============================================================================
void DynamicEq::controlTick (int index) noexcept
{
    const size_t idx = static_cast<size_t> (index);
    const DynEqBandParams& t = targets[idx];
    auto& band = bands[idx];

    // ---- discrete changes: fade the band to 0 dB, swap, fade back in -------
    const bool swapPending = t.mode != band.mode || t.shape != band.shape;
    band.fade.setTarget (t.enabled && ! swapPending ? 1.0f : 0.0f);
    const float fadeGain = band.fade.next();

    // ---- continuous parameters -------------------------------------------
    bool geometryChanged = false;
    if (band.logFreq.isSmoothing())
    {
        band.freq = std::exp (band.logFreq.next());
        geometryChanged = true;
    }
    if (band.logQ.isSmoothing())
    {
        band.q = std::exp (band.logQ.next());
        geometryChanged = true;
    }
    const float thresholdDb = band.thresholdDb.next();
    const float ratio = band.ratio.next();
    const float rangeDb = band.rangeDb.next();
    const float staticDb = band.staticGainDb.next();
    const float noiseFloorDb = band.noiseFloorDb.next();

    if (geometryChanged)
        updateDetector (band);

    // ---- linked peak envelope ----------------------------------------------
    // Two alternating buckets: max(current, previous) always covers between one
    // and two full windows, and a window spans at least half a period of the
    // lowest detector frequency, so a steady tone always has a waveform peak
    // inside the held range: no ripple, hence no gain modulation distortion.
    band.windowPeak = std::max (band.windowPeak, band.segmentPeak);
    band.segmentPeak = 0.0f;
    if (--band.windowCountdown <= 0)
    {
        band.prevWindowPeak = band.windowPeak;
        band.windowPeak = 0.0f;
        band.windowCountdown = band.windowTicks;
    }
    const float held = std::max (band.windowPeak, band.prevWindowPeak);
    band.env = std::max (held, band.env * band.envRelease); // instant attack
    if (band.env < kEnvFloor)
        band.env = 0.0f;

    const float levelDb = gainToDb (band.env);

    // ---- gain computer + attack/release smoothing ---------------------------
    const float targetDb = computeDynamicGainDb (band.mode, levelDb, thresholdDb, ratio, rangeDb, noiseFloorDb);
    float dynDb = band.dynGain.process (targetDb);
    if (std::abs (dynDb - targetDb) < 1.0e-5f)
    {
        // Land exactly: no endless exponential tail (subnormals) and the EQ
        // coefficient cache below can kick in once the gain is steady.
        band.dynGain.reset (targetDb);
        dynDb = targetDb;
    }

    const float totalDb = fadeGain * (staticDb + dynDb);
    appliedGainDb[idx].store (totalDb, std::memory_order_relaxed);

    if (geometryChanged || totalDb != band.coeffGainDb)
        updateEq (band, totalDb, true);
    else
        band.eqRamping = false;

    // ---- housekeeping: flush tiny states, recover from non-finite input ----
    float sum = band.env;
    for (int c = 0; c < spec.numChannels; ++c)
        sum += flushTiny (band.detState[static_cast<size_t> (c)]) + flushTiny (band.eqState[static_cast<size_t> (c)]);
    if (! std::isfinite (sum))
        clearBandState (band);

    // ---- bottom of the fade: remove or re-type the band ---------------------
    // Only once the EQ has also finished gliding to the 0 dB identity.
    if (fadeGain == 0.0f && ! band.fade.isSmoothing() && ! band.eqRamping)
    {
        if (! t.enabled)
        {
            band.active = false;
            appliedGainDb[idx].store (0.0f, std::memory_order_relaxed);
        }
        else if (swapPending)
        {
            // The EQ is an exact identity here, and at 0 dB Bell, LowShelf and
            // HighShelf share the same (g, k, m) set, so the new topology
            // continues from the running filter state without a click. The
            // detector keeps its SVF state too (same core, new output mix);
            // only the level history of the old detector is dropped.
            band.mode = t.mode;
            band.shape = t.shape;
            band.dynGain.setExpanderMode (isExpander (t.mode));
            band.dynGain.reset (0.0f);
            band.segmentPeak = band.windowPeak = band.prevWindowPeak = band.env = 0.0f;
            for (auto& h : band.detHistory) // old-topology outputs must not feed the interpolator
                h.fill (0.0f);
            updateDetector (band);
        }
    }
}

//==============================================================================
void DynamicEq::process (const AudioBlock& block) noexcept
{
    const int numSamples = block.numSamples;
    if (numSamples <= 0)
        return;

    const int numCh = std::min ({ block.numChannels, spec.numChannels, kMaxChannels });

    // A channel that was absent from the previous block(s) kept the filter
    // states of whatever it last carried; resuming from them would ring out a
    // stale tail (a click). Channels that come back start from rest instead.
    if (numCh > lastNumChannels)
    {
        for (auto& band : bands)
        {
            for (int ch = std::max (0, lastNumChannels); ch < numCh; ++ch)
            {
                const size_t c = static_cast<size_t> (ch);
                band.detState[c].reset();
                band.eqState[c].reset();
                band.detHistory[c].fill (0.0f);
            }
        }
    }
    lastNumChannels = numCh;

    bool anyActive = false;
    for (const auto& band : bands)
        anyActive = anyActive || band.active;

    if (! anyActive)
    {
        // Disabled bands cost nothing; just keep the control-rate phase so tick
        // positions stay independent of the host block size.
        const int elapsed = kControlInterval - controlCountdown + numSamples;
        controlCountdown = kControlInterval - elapsed % kControlInterval;
        return;
    }

    for (int pos = 0; pos < numSamples;)
    {
        const int len = std::min (numSamples - pos, controlCountdown);

        // 1) Sidechain detectors read the module's dry input, before any EQ
        //    section has touched it, so every band's threshold refers to the
        //    input level and bands do not chase each other. The level is
        //    linked: one peak over all channels, so the image never shifts.
        //    Peak = max of |samples| and |interpolated midpoints| (see kMid1).
        //    NaN never enters: std::max keeps its first argument if either is NaN.
        for (auto& band : bands)
        {
            if (! band.active)
                continue;
            const SvfCoeffs c = band.detCoeffs;
            float peak = band.segmentPeak;
            for (int ch = 0; ch < numCh; ++ch)
            {
                const size_t cs = static_cast<size_t> (ch);
                SvfState s = band.detState[cs];
                auto& h = band.detHistory[cs];
                float y1 = h[0], y2 = h[1], y3 = h[2], y4 = h[3], y5 = h[4];
                const float* x = block.channel (ch) + pos;
                for (int i = 0; i < len; ++i)
                {
                    const float y0 = svfTick (c, s, x[i]);
                    const float mid = kMid1 * (y3 + y2) + kMid3 * (y4 + y1) + kMid5 * (y5 + y0); // between y3 and y2
                    peak = std::max (peak, std::max (std::abs (y0), std::abs (mid)));
                    y5 = y4;
                    y4 = y3;
                    y3 = y2;
                    y2 = y1;
                    y1 = y0;
                }
                h = { y1, y2, y3, y4, y5 };
                band.detState[cs] = s;
            }
            band.segmentPeak = peak;
        }

        // 2) EQ sections in series, in place. A band whose coefficients were
        //    just updated glides to them across this control interval; the
        //    ramp position is taken from the global control phase, so it does
        //    not depend on how the host splits blocks.
        const int phase = kControlInterval - controlCountdown; // samples since the last tick
        for (auto& band : bands)
        {
            if (! band.active)
                continue;

            if (band.eqRamping)
            {
                const auto& r0 = band.rampStart;
                const auto& dr = band.rampDelta;
                for (int i = 0; i < len; ++i)
                {
                    const float t = static_cast<float> (phase + i + 1) * (1.0f / static_cast<float> (kControlInterval));
                    const float g = r0.g + dr.g * t;
                    const float k = r0.k + dr.k * t;
                    auto& c = rampScratch[static_cast<size_t> (i)];
                    c.a1 = 1.0f / (1.0f + g * (g + k));
                    c.a2 = g * c.a1;
                    c.a3 = g * c.a2;
                    c.m0 = r0.m0 + dr.m0 * t;
                    c.m1 = r0.m1 + dr.m1 * t;
                    c.m2 = r0.m2 + dr.m2 * t;
                }
                for (int ch = 0; ch < numCh; ++ch)
                {
                    SvfState s = band.eqState[static_cast<size_t> (ch)];
                    float* d = block.channel (ch) + pos;
                    for (int i = 0; i < len; ++i)
                        d[i] = svfTick (rampScratch[static_cast<size_t> (i)], s, d[i]);
                    band.eqState[static_cast<size_t> (ch)] = s;
                }
            }
            else
            {
                const SvfCoeffs c = band.eqCoeffs;
                for (int ch = 0; ch < numCh; ++ch)
                {
                    SvfState s = band.eqState[static_cast<size_t> (ch)];
                    float* d = block.channel (ch) + pos;
                    for (int i = 0; i < len; ++i)
                        d[i] = svfTick (c, s, d[i]);
                    band.eqState[static_cast<size_t> (ch)] = s;
                }
            }
        }

        pos += len;
        controlCountdown -= len;
        if (controlCountdown == 0)
        {
            controlCountdown = kControlInterval;
            for (int b = 0; b < kMaxBands; ++b)
                if (bands[static_cast<size_t> (b)].active)
                    controlTick (b);
        }
    }
}
} // namespace flub
