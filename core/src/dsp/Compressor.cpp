#include "flub/dsp/Compressor.h"

#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace flub
{
namespace
{
// Continuous parameters (threshold, ratio, knee, the upward curve, makeup,
// mix, sidechain corner) glide with this one-pole time constant, so macro
// moves and automation never step the gain computer. Without it a threshold
// jump with a 0.1 ms attack would move the gain by many dB in a few samples.
constexpr float kParamSmoothMs = 20.0f;

// Sidechain high-pass on / off is discrete: the detector input is crossfaded
// between the raw and the high-passed signal over this time instead.
constexpr float kHpFadeMs = 20.0f;

// 12 dB/oct Butterworth: no resonant bump, so the detector never reads hot
// near the corner.
constexpr double kHpQ = 0.70710678118654752;

// Lowest frequency the peak hold has to cover when the sidechain is full-band.
constexpr double kLowestDetectorHz = 20.0;

constexpr float kMaxLookaheadMs = 10.0f;

// The upward gain fades out over the 12 dB above upFloorDb (header contract).
constexpr float kUpTaperDb = 12.0f;

// Detector level clamp: keeps the curve (and so the gain) finite for +Inf or
// absurdly hot input. +100 dBFS is far beyond anything real.
constexpr float kMaxLevelDb = 100.0f;

// Auto release: reduction that has lasted kSustainMs releases at releaseMs,
// a short transient at kFastReleaseFraction * releaseMs. Reduction shallower
// than kActiveReductionDb does not count as "reducing".
constexpr float kSustainMs = 100.0f;
constexpr float kFastReleaseFraction = 0.25f;
constexpr float kActiveReductionDb = 0.5f;

// Auto makeup is limited to the manual makeup range.
constexpr float kMaxMakeupDb = 24.0f;

// The smoothed gain lands exactly on its target once this close, so a steady
// state costs nothing (no exp per sample) and there is no endless tail.
constexpr float kLandDb = 1.0e-5f;

// Filter states below this are flushed (the host also sets FTZ/DAZ).
constexpr float kStateFlush = 1.0e-20f;

// Control-rate work runs on the global sample phase (not per host block), so
// results do not depend on how the host splits blocks. Both are powers of two.
constexpr uint32_t kHpCoeffInterval = 16;     // sidechain corner glide
constexpr uint32_t kHousekeepingInterval = 64; // denormal flush / NaN recovery

constexpr float kDbToLog = 0.11512925464970228f; // ln(10) / 20

float sanitise (float v, float lo, float hi, float fallback) noexcept
{
    return std::isfinite (v) ? std::clamp (v, lo, hi) : fallback;
}

float slopeFromRatio (float ratio) noexcept
{
    return 1.0f - 1.0f / ratio;
}

/** One per-sample step of a parameter smoother. OnePoleSmoother only snaps
    within 1e-6 (1 + |target|), finer than float lets a slow one-pole get:
    once coeff * (current - target) rounds back to (current - target) the
    recursion stalls a few ulps short of any target with |target| >~ 1
    (-60 dB stalls ~0.002 dB short at 48 kHz, ~0.007 dB at 192 kHz). The
    curve would then sit just off its setting and the module would never go
    idle again. So a step that no longer moves the value lands on the target;
    that last step is below 0.01 dB (or 0.1 % of the sidechain corner). */
void glide (OnePoleSmoother& s) noexcept
{
    if (! s.isSmoothing())
        return;
    const float before = s.getCurrent();
    if (s.next() == before)
        s.setImmediate (s.getTarget());
}
} // namespace

//==============================================================================
CompressorParams Compressor::sanitised (const CompressorParams& in, const CompressorParams& fallback) noexcept
{
    // Out-of-range values are clamped; non-finite ones keep the last valid value.
    CompressorParams p = in;
    p.thresholdDb = sanitise (p.thresholdDb, -60.0f, 0.0f, fallback.thresholdDb);
    p.ratio = sanitise (p.ratio, 1.0f, 20.0f, fallback.ratio);
    p.kneeDb = sanitise (p.kneeDb, 0.0f, 24.0f, fallback.kneeDb);
    p.attackMs = sanitise (p.attackMs, 0.1f, 200.0f, fallback.attackMs);
    p.releaseMs = sanitise (p.releaseMs, 10.0f, 2000.0f, fallback.releaseMs);
    p.makeupDb = sanitise (p.makeupDb, -12.0f, kMaxMakeupDb, fallback.makeupDb);

    // 0 (or below) is "off"; any other value is a corner inside 20 .. 300 Hz.
    if (! std::isfinite (p.sidechainHpHz))
        p.sidechainHpHz = fallback.sidechainHpHz;
    else
        p.sidechainHpHz = p.sidechainHpHz <= 0.0f ? 0.0f : std::clamp (p.sidechainHpHz, 20.0f, 300.0f);

    p.mix = sanitise (p.mix, 0.0f, 1.0f, fallback.mix);
    p.upThresholdDb = sanitise (p.upThresholdDb, -80.0f, -10.0f, fallback.upThresholdDb);
    p.upRatio = sanitise (p.upRatio, 1.0f, 10.0f, fallback.upRatio);
    p.upMaxGainDb = sanitise (p.upMaxGainDb, 0.0f, 18.0f, fallback.upMaxGainDb);
    p.upFloorDb = sanitise (p.upFloorDb, -100.0f, -40.0f, fallback.upFloorDb);
    return p;
}

Compressor::Curve Compressor::makeCurve (const CompressorParams& p) noexcept
{
    Curve c;
    c.thresholdDb = p.thresholdDb;
    c.kneeDb = p.kneeDb;
    c.slope = slopeFromRatio (p.ratio);
    c.upThresholdDb = p.upThresholdDb;
    c.upSlope = slopeFromRatio (p.upRatio);
    c.upMaxGainDb = p.upMaxGainDb;
    c.upFloorDb = p.upFloorDb;
    return c;
}

Compressor::CurveGain Compressor::evaluateCurve (const Curve& c, float levelDb) noexcept
{
    CurveGain g;

    // Downward, soft knee (Giannoulis, Massberg & Reiss 2012). With
    // slope = 1 - 1/R the three regions of the header read
    //   below the knee : 0
    //   in the knee    : -slope (x - T + W/2)^2 / (2W)
    //   above the knee : -slope (x - T)
    // which join with matching value and first derivative at T -/+ W/2.
    // W == 0 is the hard knee (the knee branch would divide by zero).
    const float over = levelDb - c.thresholdDb;
    if (c.kneeDb > 0.0f && 2.0f * std::abs (over) <= c.kneeDb)
    {
        const float t = over + 0.5f * c.kneeDb;
        g.down = -c.slope * t * t / (2.0f * c.kneeDb);
    }
    else if (over > 0.0f)
    {
        g.down = -c.slope * over;
    }

    // Upward: lift what is below upThreshold by (1 - 1/upR) per dB, capped at
    // upMax, and fade the lift out towards the floor so that silence, hiss
    // and room tone are never pulled up.
    if (c.upMaxGainDb > 0.0f && levelDb < c.upThresholdDb)
    {
        const float taper = std::clamp ((levelDb - c.upFloorDb) / kUpTaperDb, 0.0f, 1.0f);
        g.up = std::min (c.upMaxGainDb, (c.upThresholdDb - levelDb) * c.upSlope) * taper;
    }

    return g;
}

float Compressor::computeGainDb (const CompressorParams& p, float levelDb) noexcept
{
    const CompressorParams s = sanitised (p, CompressorParams {});
    const float x = std::isnan (levelDb) ? kMinusInfDb : std::clamp (levelDb, kMinusInfDb, kMaxLevelDb);
    const CurveGain g = evaluateCurve (makeCurve (s), x);
    return g.down + g.up;
}

float Compressor::autoMakeupDb (const CompressorParams& p) noexcept
{
    // Half of the reduction a 0 dBFS peak receives: gets most of the lost level
    // back without pushing the compressed signal into the limiter.
    return std::clamp (-0.5f * evaluateCurve (makeCurve (p), 0.0f).down, 0.0f, kMaxMakeupDb);
}

float Compressor::reductionOnsetGain (const Curve& c) noexcept
{
    // The detector peak (linear) at which gDown reaches -kActiveReductionDb,
    // i.e. the inverse of the downward curve at that depth. Auto release
    // measures how long the *instantaneous* peak stays above it, so the peak
    // hold (which keeps the held level up for B .. 2B samples after the
    // signal has gone) is not mistaken for persistence.
    constexpr float d = kActiveReductionDb;
    if (c.slope <= 0.0f)
        return std::numeric_limits<float>::max(); // ratio 1: never reduces
    float x = c.thresholdDb + d / c.slope;        // hard knee, or above a shallow knee
    if (c.kneeDb > 0.0f && c.slope * c.kneeDb >= 2.0f * d)
        x = c.thresholdDb - 0.5f * c.kneeDb + std::sqrt (2.0f * d * c.kneeDb / c.slope); // inside the knee
    return std::exp (std::min (x, 2.0f * kMaxLevelDb) * kDbToLog);
}

//==============================================================================
int Compressor::holdSamplesFor (float sidechainHpHz) const noexcept
{
    // The peak hold must span half a period of the lowest frequency the
    // detector lets through: then a steady tone always has a waveform peak
    // inside the held range, the level does not ripple at twice the signal
    // frequency, and the gain does not modulate (bass distortion). With the
    // sidechain high-pass on, content an octave below the corner is already
    // 12 dB down, so that octave is the lowest frequency that matters.
    // The hold must also cover the look-ahead, so the loudest sample still in
    // the delay line is always held: the release can never start while that
    // sample is yet to be played.
    const double lowest = sidechainHpHz > 0.0f ? std::max (kLowestDetectorHz, 0.5 * static_cast<double> (sidechainHpHz))
                                               : kLowestDetectorHz;
    const int halfPeriod = static_cast<int> (std::lround (spec.sampleRate / (2.0 * lowest)));
    return std::max ({ 1, latency, halfPeriod });
}

double Compressor::timeCoeff (float ms) const noexcept
{
    // Double: in float, 1 - 1/(2000 ms * 192 kHz) = 1 - 2.6e-6 is only
    // representable to ~1 %, and the time constant would drift with fs.
    return std::exp (-1.0 / (static_cast<double> (ms) * 0.001 * spec.sampleRate));
}

void Compressor::updateTimeConstants() noexcept
{
    attackCoeff = timeCoeff (params.attackMs);
    releaseCoeff = timeCoeff (params.releaseMs);
    autoCoeffSustain = -1.0f; // invalidate the auto-release cache
}

void Compressor::updateHpCoeffs() noexcept
{
    const double freq = std::exp (static_cast<double> (logHpFreq.getCurrent()));
    hpCoeffs = SvfCoeffs::make (FilterType::HighPass, freq, kHpQ, 0.0, spec.sampleRate);
}

//==============================================================================
void Compressor::prepare (const ProcessSpec& newSpec)
{
    spec = newSpec;
    if (! (spec.sampleRate > 0.0) || ! std::isfinite (spec.sampleRate))
        spec.sampleRate = 48000.0;
    spec.numChannels = std::clamp (spec.numChannels, 1, kMaxChannels);
    spec.maxBlockSize = std::max (1, spec.maxBlockSize);

    // Structural: the look-ahead is the module's latency and is only read here.
    const float la = std::isfinite (lookaheadMs) ? std::clamp (lookaheadMs, 0.0f, kMaxLookaheadMs) : 0.0f;
    latency = msToSamples (la, spec.sampleRate);
    delay.prepare (spec.numChannels, latency);

    const double sustainLength = static_cast<double> (kSustainMs) * 0.001 * spec.sampleRate;
    sustainSamples = std::max (1, static_cast<int> (std::lround (sustainLength)));
    sustainStep = 1.0f / static_cast<float> (sustainSamples);

    reset();
}

void Compressor::reset() noexcept
{
    const double fs = spec.sampleRate;
    const bool hpOn = params.sidechainHpHz > 0.0f;

    // After a reset there is no previous output to click against, so every
    // smoother starts at its target.
    thresholdS.reset (fs, kParamSmoothMs, params.thresholdDb);
    kneeS.reset (fs, kParamSmoothMs, params.kneeDb);
    slopeS.reset (fs, kParamSmoothMs, slopeFromRatio (params.ratio));
    upThresholdS.reset (fs, kParamSmoothMs, params.upThresholdDb);
    upSlopeS.reset (fs, kParamSmoothMs, slopeFromRatio (params.upRatio));
    upMaxS.reset (fs, kParamSmoothMs, params.upMaxGainDb);
    upFloorS.reset (fs, kParamSmoothMs, params.upFloorDb);
    makeupS.reset (fs, kParamSmoothMs, params.autoMakeup ? autoMakeupDb (params) : params.makeupDb);
    mixS.reset (fs, kParamSmoothMs, params.mix);
    hpMix.reset (fs, kHpFadeMs, hpOn ? 1.0f : 0.0f);
    logHpFreq.reset (fs, kParamSmoothMs, hpOn ? std::log (params.sidechainHpHz) : logHpFreq.getTarget());
    curve = makeCurve (params);
    smoothing = false;
    curveDirty = true;
    onsetDirty = true;

    for (auto& s : hpState)
        s.reset();
    hpRunning = hpOn;
    updateHpCoeffs();

    delay.reset();

    bucketLength = holdSamplesFor (params.sidechainHpHz);
    bucketCountdown = bucketLength;
    bucketPeak = prevBucketPeak = 0.0f;
    heldPeak = -1.0f; // forces the first gain computation
    levelDb = kMinusInfDb;
    target = {};

    gainDb = 0.0; // the curve's value for silence
    updateTimeConstants();
    sustain = 0.0f;
    activeRun = 0;
    loudRun = 0;

    lastWetDb = 0.0f;
    lastMix = -1.0f; // forces the first output-factor computation
    outFactor = 1.0f;
    tick = 0;

    grDb.store (0.0f, std::memory_order_relaxed);
    upDb.store (0.0f, std::memory_order_relaxed);
}

int Compressor::latencySamples() const noexcept
{
    return latency;
}

//==============================================================================
void Compressor::setParams (const CompressorParams& newParams) noexcept
{
    const CompressorParams p = sanitised (newParams, params);
    if (p == params)
        return; // the chain pushes every block; unchanged values cost nothing

    const bool timesChanged = p.attackMs != params.attackMs || p.releaseMs != params.releaseMs;
    params = p;

    thresholdS.setTarget (p.thresholdDb);
    kneeS.setTarget (p.kneeDb);
    slopeS.setTarget (slopeFromRatio (p.ratio));
    upThresholdS.setTarget (p.upThresholdDb);
    upSlopeS.setTarget (slopeFromRatio (p.upRatio));
    upMaxS.setTarget (p.upMaxGainDb);
    upFloorS.setTarget (p.upFloorDb);
    makeupS.setTarget (p.autoMakeup ? autoMakeupDb (p) : p.makeupDb);
    mixS.setTarget (p.mix);

    if (p.sidechainHpHz > 0.0f)
    {
        const float logHz = std::log (p.sidechainHpHz);
        if (! hpRunning)
        {
            // The filter was idle: start it from rest, directly at the new
            // corner. Its output weight (hpMix) is 0 right now and fades in,
            // so neither the jump nor the start-up transient is audible.
            for (auto& s : hpState)
                s.reset();
            logHpFreq.setImmediate (logHz);
            updateHpCoeffs();
            hpRunning = true;
        }
        else
        {
            logHpFreq.setTarget (logHz);
        }
        hpMix.setTarget (1.0f);
    }
    else
    {
        hpMix.setTarget (0.0f); // keeps running at its last corner while fading out
    }

    // A new hold length only changes when the next bucket boundary falls;
    // the gain smoother absorbs any resulting step in the detected level.
    bucketLength = holdSamplesFor (p.sidechainHpHz);
    bucketCountdown = std::min (bucketCountdown, bucketLength);

    if (timesChanged)
        updateTimeConstants();
    autoCoeffSustain = -1.0f; // releaseMs or the mode may have changed

    smoothing = true;
}

void Compressor::advanceSmoothers() noexcept
{
    const bool downMoving = thresholdS.isSmoothing() || kneeS.isSmoothing() || slopeS.isSmoothing();
    glide (thresholdS);
    glide (kneeS);
    glide (slopeS);
    glide (upThresholdS);
    glide (upSlopeS);
    glide (upMaxS);
    glide (upFloorS);
    curve.thresholdDb = thresholdS.getCurrent();
    curve.kneeDb = kneeS.getCurrent();
    curve.slope = slopeS.getCurrent();
    curve.upThresholdDb = upThresholdS.getCurrent();
    curve.upSlope = upSlopeS.getCurrent();
    curve.upMaxGainDb = upMaxS.getCurrent();
    curve.upFloorDb = upFloorS.getCurrent();
    curveDirty = true;
    onsetDirty = onsetDirty || downMoving;

    glide (makeupS);
    glide (mixS);
    hpMix.next();

    if (logHpFreq.isSmoothing())
    {
        glide (logHpFreq);
        // The SVF is modulation-safe, so the corner can step at control rate;
        // the last step lands exactly on the target.
        if ((tick & (kHpCoeffInterval - 1u)) == 0u || ! logHpFreq.isSmoothing())
            updateHpCoeffs();
    }

    if (hpMix.getCurrent() == 0.0f && ! hpMix.isSmoothing())
        hpRunning = false; // fully faded out: stop spending cycles on the filter

    smoothing = thresholdS.isSmoothing() || kneeS.isSmoothing() || slopeS.isSmoothing() || upThresholdS.isSmoothing()
                || upSlopeS.isSmoothing() || upMaxS.isSmoothing() || upFloorS.isSmoothing() || makeupS.isSmoothing()
                || mixS.isSmoothing() || hpMix.isSmoothing() || logHpFreq.isSmoothing();
}

void Compressor::housekeeping() noexcept
{
    // Flush near-zero filter states so a decaying sidechain cannot leave
    // subnormals circulating, and recover from non-finite input: a NaN or Inf
    // stuck in the high-pass would otherwise blind the detector for good.
    // Per channel: restarting a healthy channel's filter mid-signal would pass
    // its unfiltered level (e.g. a loud sub) to the detector for a few ms.
    for (int c = 0; c < spec.numChannels; ++c)
    {
        auto& s = hpState[static_cast<size_t> (c)];
        if (std::abs (s.ic1) < kStateFlush)
            s.ic1 = 0.0f;
        if (std::abs (s.ic2) < kStateFlush)
            s.ic2 = 0.0f;
        if (! std::isfinite (s.ic1 + s.ic2))
            s.reset();
    }

    if (! std::isfinite (gainDb))
        gainDb = 0.0;
}

//==============================================================================
void Compressor::process (const AudioBlock& block) noexcept FLUB_NONBLOCKING
{
    const int numSamples = block.numSamples;
    const int numCh = std::min ({ block.numChannels, spec.numChannels, kMaxChannels });
    if (numSamples <= 0 || numCh <= 0)
        return;

    std::array<float*, kMaxChannels> data {};
    for (int c = 0; c < numCh; ++c)
        data[static_cast<size_t> (c)] = block.channel (c);

    // Channels this block leaves out (narrower than the prepared width) are
    // fed silence: their delay lines share the write head, and a channel that
    // rejoins later must not replay audio from before it was dropped.
    for (int c = numCh; c < spec.numChannels; ++c)
        hpState[static_cast<size_t> (c)].reset();

    float blockMinDb = 0.0f, blockMaxDb = 0.0f;

    // Everything runs per sample (the gain computer only when its input
    // changes), so the result is exactly independent of the host block size.
    for (int i = 0; i < numSamples; ++i)
    {
        if (smoothing)
            advanceSmoothers();

        // ---- 1) linked sidechain peak -----------------------------------------
        // One level for all channels: unlinked gain would move sounds between
        // the ears and corrupt positional cues.
        float peak = 0.0f;
        if (hpRunning)
        {
            const float w = hpMix.getCurrent();
            for (int c = 0; c < numCh; ++c)
            {
                const size_t ci = static_cast<size_t> (c);
                const float x = data[ci][i];
                const float hp = svfTick (hpCoeffs, hpState[ci], x);
                peak = std::max (peak, std::abs (x + w * (hp - x))); // NaN input is ignored here
            }
        }
        else
        {
            for (int c = 0; c < numCh; ++c)
                peak = std::max (peak, std::abs (data[static_cast<size_t> (c)][i]));
        }

        // ---- 2) peak hold: two alternating buckets ----------------------------
        // max(current, previous) always covers the last bucketLength + 1 to
        // 2 * bucketLength samples: a steady tone reads its true peak without
        // ripple and every sample inside the look-ahead window stays held.
        bucketPeak = std::max (bucketPeak, peak);
        const float held = std::max (bucketPeak, prevBucketPeak);
        if (--bucketCountdown <= 0)
        {
            prevBucketPeak = bucketPeak;
            bucketPeak = 0.0f;
            bucketCountdown = bucketLength;
        }

        // ---- 3) static gain computer ------------------------------------------
        if (held != heldPeak)
        {
            heldPeak = held;
            levelDb = std::min (gainToDb (held), kMaxLevelDb);
            curveDirty = true;
        }
        if (curveDirty)
        {
            curveDirty = false;
            target = evaluateCurve (curve, levelDb);
        }
        if (onsetDirty)
        {
            onsetDirty = false;
            onsetGain = reductionOnsetGain (curve);
        }
        const double targetDb = static_cast<double> (target.down) + static_cast<double> (target.up);

        // ---- 4) how long has the reduction lasted? (auto release) ------------
        // An episode runs while the held level asks for reduction; its
        // persistence is the time from its start to the last sample whose
        // instantaneous peak was loud enough to reduce. That excludes the
        // B .. 2B samples the hold keeps the level up after the signal has
        // gone, so a gunshot really releases at ~releaseMs / 4. While a
        // release is still running the value is kept, so dense material that
        // never fully recovers keeps the slow release (no pumping); it is
        // forgotten over kSustainMs only once the gain is back near 0 dB.
        if (target.down < -kActiveReductionDb)
        {
            activeRun = std::min (activeRun + 1, sustainSamples);
            if (peak > onsetGain)
                loudRun = activeRun;
            sustain = std::max (sustain, static_cast<float> (loudRun) * sustainStep);
        }
        else
        {
            activeRun = loudRun = 0;
            if (gainDb >= -kActiveReductionDb)
                sustain = std::max (0.0f, sustain - sustainStep);
        }

        // ---- 5) attack / release in the gain (dB) domain ----------------------
        // GainSmoother semantics: a falling gain is the attack (level rising,
        // for both the downward and the upward part), a rising gain the release.
        if (gainDb != targetDb)
        {
            double coeff = attackCoeff;
            if (targetDb > gainDb)
            {
                coeff = releaseCoeff;
                // Program-dependent release applies to recovery from reduction;
                // the upward lift (gain above 0 dB) always rises at releaseMs so
                // short gaps are not pumped up.
                if (params.autoRelease && gainDb < 0.0)
                {
                    if (sustain != autoCoeffSustain)
                    {
                        autoCoeffSustain = sustain;
                        const float scale = kFastReleaseFraction + (1.0f - kFastReleaseFraction) * sustain;
                        autoCoeff = timeCoeff (params.releaseMs * scale);
                    }
                    coeff = autoCoeff;
                }
            }
            gainDb = targetDb + coeff * (gainDb - targetDb);
            if (std::abs (gainDb - targetDb) < static_cast<double> (kLandDb))
                gainDb = targetDb;
        }
        const float gain = static_cast<float> (gainDb);
        blockMinDb = std::min (blockMinDb, gain);
        blockMaxDb = std::max (blockMaxDb, gain);

        // ---- 6) look-ahead delay, makeup and dry/wet --------------------------
        // The delayed signal is both the wet input and the dry path, so the
        // parallel mix is phase-aligned and collapses to one gain factor:
        //   out = d (1 - mix) + d g mix,
        // which is exactly 1 for mix = 0 (or g = 1) and exactly g for mix = 1
        // (the form 1 + mix (g - 1) would lose g's precision below ~-60 dB).
        const float wetDb = gain + makeupS.getCurrent();
        const float mix = mixS.getCurrent();
        if (wetDb != lastWetDb || mix != lastMix)
        {
            lastWetDb = wetDb;
            lastMix = mix;
            outFactor = (1.0f - mix) + mix * std::exp (wetDb * kDbToLog);
        }

        for (int c = 0; c < numCh; ++c)
        {
            float* d = data[static_cast<size_t> (c)];
            d[i] = delay.processSample (c, d[i]) * outFactor;
        }
        for (int c = numCh; c < spec.numChannels; ++c)
            (void) delay.processSample (c, 0.0f);
        delay.advance();

        if ((++tick & (kHousekeepingInterval - 1u)) == 0u)
            housekeeping();
    }

    grDb.store (blockMinDb, std::memory_order_relaxed);
    upDb.store (blockMaxDb, std::memory_order_relaxed);
}
} // namespace flub
