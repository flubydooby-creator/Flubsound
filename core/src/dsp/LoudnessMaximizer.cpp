#include "flub/dsp/LoudnessMaximizer.h"

#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace flub
{
namespace
{
// Continuous parameters (drive, ceiling, clip amount / knee, glue) glide
// linearly over this time, so macro moves and automation never step the gain.
constexpr float kParamSmoothMs = 50.0f;

// Glue on/off and clipper on/off are discrete: the stage is crossfaded
// against the (latency-aligned) signal that bypasses it. The glue fade is a
// little longer because the band sum is an all-pass of the input, and the
// partial sum during the fade is briefly comb-filtered.
constexpr float kGlueFadeMs = 30.0f;
constexpr float kClipFadeMs = 20.0f;

// A stage that is switched on runs silently (weight 0) for a moment first, so
// its filters are primed with the real signal before they are heard.
constexpr float kGlueWarmupMs = 10.0f;
constexpr int kClipWarmupExtra = 8; // clipper: 2 x oversampler latency + this

// Glue: ThreeBandSplitter corners, per-band compressor (header contract).
constexpr double kGlueLowMidHz = 120.0;
constexpr double kGlueMidHighHz = 4000.0;
constexpr float kGlueThresholdGain = 0.50118723f; // ceiling - 6 dB
constexpr float kGlueAttackMs = 5.0f;
constexpr float kGlueReleaseMs = 80.0f;

// Peak hold per band = half a period of the lowest frequency the band carries
// (an octave below its lower corner; 30 Hz for the low band), so a steady
// tone's level has no ripple and the gain does not modulate the waveform.
constexpr std::array<double, 3> kBandLowestHz { 30.0, 60.0, 2000.0 };

// LF-first limiter (docs/11 E05 step 5): the low band's threshold under the
// ceiling and its follower. The detector is the band's held peak (half a
// period of 30 Hz, see kBandLowestHz), so the gain is flat on steady bass;
// the 1 ms attack takes a kick's first cycle, the release outlasts its body
// and the gap to the next one. Tuned on the E59 quality suite at Music
// Boost 100 (threshold 0 / -1 / -2 / -3 / -6 dB: kick onset - body -0.55 /
// -0.29 / -0.02 / +0.21 / +0.50 dB, 2 kHz probe dip 3.81 / 3.39 / 2.91 /
// 2.55 / 2.05 dB; release 60 / 120 / 250 ms: bass two-tone IMD at 12 dB
// drive -27.1 / -31.7 / -37.2 dB).
constexpr float kLfHeadroomGain = 0.70794578f; // -3 dB
constexpr float kLfAttackMs = 1.0f;
constexpr float kLfReleaseMs = 250.0f;

// Bed-lift budget (docs/11 E19 step 3, header): the input peak's release,
// the headroom over the drive from which only the budget applies, and the
// cap's glide (a loud onset raises it this fast; its fall follows the peak).
constexpr float kBedPeakReleaseMs = 400.0f;
constexpr float kBedKneeDb = 6.0f;
constexpr float kBedCapGlideMs = 20.0f;
constexpr float kMaxBedTrimDb = 12.0f;
constexpr float kNoBedBudgetDb = 24.0f;

// Clipper threshold headroom above the ceiling: lerp (+6 dB, +0.3 dB, amount).
constexpr float kClipHeadroomMaxDb = 6.0f;
constexpr float kClipHeadroomMinDb = 0.3f;
// Residual-path DC blocker (docs/11 E10): the clipper's band-limited
// correction clip(x) - x passes a 1st-order high-pass at this frequency
// before it is added to the dry path, so a clipped asymmetric waveform
// leaves no DC (the programme itself never passes the filter).
constexpr double kClipDcBlockHz = 5.0;

// Crest gate and depth cap (docs/11 E05 stage 1): the clipper input's
// short-term power (linked, one-pole) and the width of the cap's C1 blend
// (a fraction of (1 - depthGain) t, so it never reaches the knee start).
constexpr float kClipRmsMs = 5.0f; // per stage of a 2-stage cascade
constexpr float kCapBlend = 0.45f;

// Envelope values below this are flushed (the host also sets FTZ/DAZ).
constexpr float kEnvFlush = 1.0e-15f;

// A -400 dB offset on the splitter input, alternating 1e-20 / 0 (DC plus
// Nyquist, so both the low-pass and the band-pass states stay excited), keeps
// its recursive SVF states in the normal float range when the input falls
// silent: they cannot decay into subnormals even without FTZ (the states
// belong to the shared splitter, so they are not ours to flush).
constexpr float kAntiDenormal = 1.0e-20f;

float sanitise (float v, float lo, float hi, float fallback) noexcept
{
    return std::isfinite (v) ? std::clamp (v, lo, hi) : fallback;
}

/** A NaN / Inf input sample (or one that the drive pushes past the float
    range) becomes silence at the door, so it can neither poison the glue
    splitter's recursive states (whose reset would glitch every channel) nor
    smear through the oversampler's FIRs into its neighbours. */
float finiteOrZero (float x) noexcept
{
    return std::abs (x) <= std::numeric_limits<float>::max() ? x : 0.0f;
}
} // namespace

//==============================================================================
float LoudnessMaximizer::softClip (float x, float threshold, float knee) noexcept
{
    if (! (threshold > 0.0f))
        return 0.0f;
    const float k = std::isfinite (knee) ? std::clamp (knee, 0.0f, 1.0f) : 0.0f;
    const float ax = std::abs (x);
    // Knee start ks = t (1 - knee / 2) >= t / 2, so t - ks is exact (Sterbenz)
    // and ks + (t - ks) * tanh(.) can never round above t.
    const float ks = threshold * (1.0f - 0.5f * k);
    if (ax <= ks)
        return x; // identity below the knee (NaN falls through below)

    const float span = threshold - ks;
    float y = threshold; // knee 0: hard clip
    if (span > 0.0f)
        y = ks + span * std::tanh ((ax - ks) / span); // C1-continuous at ks (slope 1), -> t asymptotically
    return std::copysign (std::min (y, threshold), x);
}

float LoudnessMaximizer::softClipCapped (float x, float threshold, float knee, float depthGain) noexcept
{
    const float y = softClip (x, threshold, knee);
    const float ax = std::abs (x), ay = std::abs (y);
    if (! (depthGain > 0.0f) || ! (ay < ax))
        return y; // uncapped, or identity below the knee (NaN falls through)
    // |y| = smoothmax (|clip (x)|, depthGain |x|): a quadratic C1 blend over
    // |d| < w. At the knee start d = (depthGain - 1) ks <= -(1 - depthGain) t / 2
    // < -w, so the curve is untouched up to there.
    const float w = kCapBlend * (1.0f - std::min (depthGain, 1.0f)) * threshold;
    const float d = depthGain * ax - ay;
    if (d <= -w)
        return y;
    const float m = d >= w ? depthGain * ax : ay + (d + w) * (d + w) / (4.0f * w);
    return std::copysign (m, x);
}

//==============================================================================
MaximizerParams LoudnessMaximizer::sanitised (const MaximizerParams& in, const MaximizerParams& fallback) noexcept
{
    MaximizerParams p = in;
    p.driveDb = sanitise (p.driveDb, 0.0f, 24.0f, fallback.driveDb);
    p.ceilingDb = sanitise (p.ceilingDb, -12.0f, 0.0f, fallback.ceilingDb);
    p.clipAmount = sanitise (p.clipAmount, 0.0f, 1.0f, fallback.clipAmount);
    p.clipKnee = sanitise (p.clipKnee, 0.0f, 1.0f, fallback.clipKnee);
    p.glue = sanitise (p.glue, 0.0f, 1.0f, fallback.glue);
    p.lfLimit = sanitise (p.lfLimit, 0.0f, 1.0f, fallback.lfLimit);
    p.bedLiftDb = sanitise (p.bedLiftDb, 0.0f, kNoBedBudgetDb, fallback.bedLiftDb);
    p.releaseMs = sanitise (p.releaseMs, 5.0f, 1000.0f, fallback.releaseMs);
    p.clipCrestDb = sanitise (p.clipCrestDb, 0.0f, 24.0f, fallback.clipCrestDb);
    p.clipMaxDepthDb = sanitise (p.clipMaxDepthDb, 0.5f, 24.0f, fallback.clipMaxDepthDb);
    return p;
}

void LoudnessMaximizer::updateCeiling (float newCeilingDb) noexcept
{
    ceilingDb = newCeilingDb;
    ceilingLin = dbToGain (newCeilingDb);
    glueThreshold = ceilingLin * kGlueThresholdGain;
    lfThreshold = ceilingLin * kLfHeadroomGain;
}

void LoudnessMaximizer::updateClipThreshold() noexcept
{
    const float headroomDb = lerp (kClipHeadroomMaxDb, kClipHeadroomMinDb, clipAmountS.getCurrent());
    clipThreshold = dbToGain (ceilingDb + headroomDb);
}

void LoudnessMaximizer::updateClipShape() noexcept
{
    // Gains, not dB, glide: both ends of each range are continuous in gain
    // (crest 0 = gate off = gain 0; depth 24 = uncapped = gain 0, next to
    // 10^(-24/20) = 0.063, where the cap hardly ever binds).
    crestGainS.setTarget (params.clipCrestDb > 0.0f ? dbToGain (params.clipCrestDb) : 0.0f);
    depthGainS.setTarget (params.clipMaxDepthDb < 24.0f ? dbToGain (-params.clipMaxDepthDb) : 0.0f);
}

void LoudnessMaximizer::startGlue (bool immediate) noexcept
{
    // The splitter and detectors start from rest; unless this is the very
    // first block, the stage then runs unheard for kGlueWarmupMs before its
    // output is faded in.
    splitter.reset();
    antiDenormal = 0.0f;
    for (auto& b : bands)
    {
        b.bucket = b.prevBucket = b.env = 0.0f;
        b.countdown = b.holdLength;
    }
    lfEnv = 0.0f;
    glueRunning = true;
    if (immediate)
    {
        glueS.setImmediate (params.glue);
        lfLimitS.setImmediate (params.lfLimit);
        glueMixS.setImmediate (1.0f);
        glueWarmup = 0;
    }
    else
    {
        glueS.setImmediate (0.0f);
        glueS.setTarget (params.glue);
        lfLimitS.setImmediate (0.0f);
        lfLimitS.setTarget (params.lfLimit);
        glueMixS.setImmediate (0.0f);
        glueWarmup = glueWarmupLength;
    }
}

void LoudnessMaximizer::startClipper (bool immediate) noexcept
{
    oversampler.reset();
    clipDcLp.fill (0.0);
    clipRunning = true;
    if (immediate)
    {
        clipMixS.setImmediate (1.0f);
        clipWarmup = 0;
    }
    else
    {
        clipMixS.setImmediate (0.0f);
        clipWarmup = clipWarmupLength;
    }
}

void LoudnessMaximizer::applyParamsImmediately() noexcept
{
    driveDbS.setImmediate (params.driveDb);
    driveGain = dbToGain (params.driveDb);
    // A budget starts at its floor (nothing heard yet; a loud start raises
    // the cap within the glide).
    bedCapActive = params.bedLiftDb < kNoBedBudgetDb;
    bedCapDb = bedCapActive ? params.driveDb - std::clamp (upstreamLiftDb + params.driveDb - params.bedLiftDb, 0.0f, kMaxBedTrimDb)
                            : kNoBedBudgetDb;
    if (bedCapActive)
        driveGain = dbToGain (bedCapDb);
    ceilingDbS.setImmediate (params.ceilingDb);
    updateCeiling (params.ceilingDb);
    clipAmountS.setImmediate (params.clipAmount);
    clipKneeS.setImmediate (params.clipKnee);
    updateClipThreshold();
    updateClipShape();
    crestGainS.setImmediate (crestGainS.getTarget());
    depthGainS.setImmediate (depthGainS.getTarget());

    if (bandStageWanted (params))
    {
        startGlue (true);
    }
    else
    {
        glueRunning = false;
        glueWarmup = 0;
        glueS.setImmediate (0.0f);
        lfLimitS.setImmediate (0.0f);
        glueMixS.setImmediate (0.0f);
    }

    if (params.clipAmount > 0.0f)
    {
        startClipper (true);
    }
    else
    {
        clipRunning = false;
        clipWarmup = 0;
        clipMixS.setImmediate (0.0f);
    }
}

//==============================================================================
void LoudnessMaximizer::prepare (const ProcessSpec& newSpec)
{
    spec = newSpec;
    if (! (spec.sampleRate > 0.0) || ! std::isfinite (spec.sampleRate))
        spec.sampleRate = 48000.0;
    spec.numChannels = std::clamp (spec.numChannels, 1, kMaxChannels);
    spec.maxBlockSize = std::max (1, spec.maxBlockSize);
    const double fs = spec.sampleRate;

    // Structural: clip oversampling (1, 2 or 4) is only read here. The dry
    // path is delayed by the same round trip whether or not the clipper runs,
    // so the latency is constant.
    osFactor = clipOsFactor <= 1 ? 1 : (clipOsFactor <= 2 ? 2 : 4);
    oversampler.prepare (spec.numChannels, spec.maxBlockSize, osFactor, clipOsQuality);
    dryDelay.prepare (spec.numChannels, oversampler.latencySamples());
    dryBuffer.setSize (spec.numChannels, spec.maxBlockSize);
    padBuffer.setSize (spec.numChannels, spec.maxBlockSize);
    residualRef.setSize (spec.numChannels, spec.maxBlockSize);
    thresholdBuf.assign (static_cast<size_t> (spec.maxBlockSize), 1.0f);
    kneeBuf.assign (static_cast<size_t> (spec.maxBlockSize), 0.0f);
    clipMixBuf.assign (static_cast<size_t> (spec.maxBlockSize), 0.0f);
    depthBuf.assign (static_cast<size_t> (spec.maxBlockSize), 0.0f);
    clipWarmupLength = 2 * oversampler.latencySamples() + kClipWarmupExtra;
    const double gDc = std::tan (kPi * kClipDcBlockHz / fs);
    clipDcG = gDc / (1.0 + gDc);
    clipPowerCoeff = 1.0f - onePoleCoeff (kClipRmsMs, fs);

    splitter.prepare (fs, kGlueLowMidHz, kGlueMidHighHz);
    for (size_t b = 0; b < bands.size(); ++b)
    {
        bands[b].holdLength = std::max (1, static_cast<int> (std::lround (fs / (2.0 * kBandLowestHz[b]))));
        bands[b].attack = onePoleCoeff (kGlueAttackMs, fs);
        bands[b].release = onePoleCoeff (kGlueReleaseMs, fs);
    }
    glueWarmupLength = std::max (1, msToSamples (kGlueWarmupMs, fs));
    bedPeakRelease = onePoleCoeff (kBedPeakReleaseMs, fs);
    bedCapCoeff = onePoleCoeff (kBedCapGlideMs, fs);
    lfAttack = onePoleCoeff (kLfAttackMs, fs);
    lfRelease = onePoleCoeff (kLfReleaseMs, fs);
    distortionWindow.prepare (fs);
    residualWindow.prepare (fs);
    grWindowLength = std::max (1, msToSamples (kGrWindowMs, fs));

    // Structural limiter settings are passed through here.
    limiter.setLookaheadMs (lookaheadMs);
    limiter.setTruePeakDetection (truePeak);
    limiter.setEnvelope (limiterEnvelope);
    limiter.prepare (spec);
    limiter.setParams ({ params.ceilingDb, params.releaseMs, params.autoRelease });
    residualDelay.prepare (spec.numChannels, oversampler.latencySamples() + limiter.latencySamples());

    prepared = true;
    reset();
}

void LoudnessMaximizer::reset() noexcept FLUB_NONBLOCKING
{
    const double fs = spec.sampleRate;
    oversampler.reset();
    dryDelay.reset();
    clipDcLp.fill (0.0);
    clipPower = clipPowerFast = 0.0f;
    splitter.reset();
    antiDenormal = 0.0f;
    lfEnv = 0.0f;
    bedPeak = 0.0f;
    bedQuiet = 1.0f;
    bedCapDb = kNoBedBudgetDb;
    bedCapActive = false;
    limiter.reset();
    for (auto& b : bands)
    {
        b.bucket = b.prevBucket = b.env = 0.0f;
        b.countdown = b.holdLength;
    }

    driveDbS.reset (fs, kParamSmoothMs, params.driveDb);
    ceilingDbS.reset (fs, kParamSmoothMs, params.ceilingDb);
    clipAmountS.reset (fs, kParamSmoothMs, params.clipAmount);
    clipKneeS.reset (fs, kParamSmoothMs, params.clipKnee);
    glueS.reset (fs, kParamSmoothMs, params.glue);
    lfLimitS.reset (fs, kParamSmoothMs, params.lfLimit);
    glueMixS.reset (fs, kGlueFadeMs, 0.0f);
    clipMixS.reset (fs, kClipFadeMs, 0.0f);
    crestGainS.reset (fs, kParamSmoothMs, 0.0f);
    depthGainS.reset (fs, kParamSmoothMs, 0.0f);
    applyParamsImmediately(); // no previous output to click against

    limiterGrDb.store (0.0f, std::memory_order_relaxed);
    glueGrDb.store (0.0f, std::memory_order_relaxed);
    lfGrDb.store (0.0f, std::memory_order_relaxed);
    clipRatioDb.store (kMinusInfDb, std::memory_order_relaxed);
    clipInputEnergy.store (0.0f, std::memory_order_relaxed);
    distortionWindow.reset();
    distortionDb.store (kMinusInfDb, std::memory_order_relaxed);
    residualDelay.reset();
    residualWindow.reset();
    residualDb.store (kMinusInfDb, std::memory_order_relaxed);
    windowClipDb.store (kMinusInfDb, std::memory_order_relaxed);
    grWindowCount = 0;
    grWindowMin = 0.0f;
    analysisPending = 0;
    windowGrDb.store (0.0f, std::memory_order_relaxed);
    fresh = true;
}

int LoudnessMaximizer::latencySamples() const noexcept
{
    return oversampler.latencySamples() + limiter.latencySamples();
}

void LoudnessMaximizer::setParams (const MaximizerParams& newParams) noexcept FLUB_NONBLOCKING
{
    const MaximizerParams p = sanitised (newParams, params);
    if (p == params)
        return; // pushed every block by the chain; unchanged values cost nothing

    params = p;
    limiter.setParams ({ p.ceilingDb, p.releaseMs, p.autoRelease });

    if (fresh)
    {
        applyParamsImmediately();
        return;
    }

    updateClipShape();
    driveDbS.setTarget (p.driveDb);
    ceilingDbS.setTarget (p.ceilingDb);
    clipAmountS.setTarget (p.clipAmount);
    clipKneeS.setTarget (p.clipKnee);

    if (bandStageWanted (p))
    {
        if (! glueRunning)
        {
            startGlue (false);
        }
        else
        {
            glueS.setTarget (p.glue);
            lfLimitS.setTarget (p.lfLimit);
            if (glueWarmup == 0)
                glueMixS.setTarget (1.0f);
        }
    }
    else
    {
        // Fade the bands out; the stage stops once the fade has finished.
        glueS.setTarget (0.0f);
        lfLimitS.setTarget (0.0f);
        glueWarmup = 0;
        glueMixS.setTarget (0.0f);
    }

    if (p.clipAmount > 0.0f)
    {
        if (! clipRunning)
            startClipper (false);
        else if (clipWarmup == 0)
            clipMixS.setTarget (1.0f);
    }
    else
    {
        clipWarmup = 0;
        clipMixS.setTarget (0.0f);
    }
}

//==============================================================================
void LoudnessMaximizer::processSegment (const AudioBlock& seg, double& clipDiffEnergy, double& clipInEnergy,
                                        float& glueMinGain, float& lfMinGain) noexcept
{
    const int n = seg.numSamples;
    const int numCh = seg.numChannels;
    std::array<float*, kMaxChannels> data {};
    for (int c = 0; c < numCh; ++c)
        data[static_cast<size_t> (c)] = seg.channel (c);

    // ---- 1) per sample: controls, drive, glue --------------------------------
    for (int i = 0; i < n; ++i)
    {
        const auto si = static_cast<size_t> (i);
        if (driveDbS.isSmoothing())
            driveGain = dbToGain (driveDbS.next());
        if (params.bedLiftDb < kNoBedBudgetDb || bedCapActive)
        {
            // Bed-lift budget: the drive follows the input's headroom (header).
            float peak = 0.0f;
            for (int c = 0; c < numCh; ++c)
                peak = std::max (peak, std::abs (data[static_cast<size_t> (c)][i]));
            bedPeak = peak >= bedPeak ? std::min (peak, 1.0e6f) : peak + bedPeakRelease * (bedPeak - peak);
            const float drive = driveDbS.getCurrent();
            // A budget switched on mid-stream starts its glide from the drive
            // it replaces (the cap may be stale from an earlier release).
            if (! bedCapActive)
                bedCapDb = drive;
            const float excess = std::clamp (upstreamLiftDb + drive - params.bedLiftDb, 0.0f, kMaxBedTrimDb);
            const float spareDb = ceilingDb - gainToDb (bedPeak) - drive;
            bedQuiet = std::clamp (spareDb / kBedKneeDb, 0.0f, 1.0f);
            const float target = params.bedLiftDb < kNoBedBudgetDb ? drive - excess * bedQuiet : drive;
            bedCapDb = target + bedCapCoeff * (bedCapDb - target);
            // Released: back on the plain drive once the cap has glided up to it.
            bedCapActive = params.bedLiftDb < kNoBedBudgetDb || bedCapDb < drive - 1.0e-3f;
            driveGain = dbToGain (bedCapActive ? std::min (drive, bedCapDb) : drive);
        }

        bool thresholdDirty = false;
        if (ceilingDbS.isSmoothing())
        {
            updateCeiling (ceilingDbS.next());
            thresholdDirty = true;
        }
        if (clipAmountS.isSmoothing())
        {
            clipAmountS.next();
            thresholdDirty = true;
        }
        if (thresholdDirty)
            updateClipThreshold();

        // Clipper controls for this base-rate sample (held for its oversampled
        // sub-samples; they move far too slowly for that to matter).
        if (clipWarmup > 0 && --clipWarmup == 0)
            clipMixS.setTarget (1.0f);
        thresholdBuf[si] = clipThreshold;
        kneeBuf[si] = clipKneeS.next();
        clipMixBuf[si] = clipMixS.next();
        depthBuf[si] = depthGainS.next();

        if (! glueRunning)
        {
            for (int c = 0; c < numCh; ++c)
            {
                float& x = data[static_cast<size_t> (c)][i];
                x = finiteOrZero (x * driveGain);
            }
            continue;
        }

        // Glue: split every channel, one linked level per band.
        if (glueWarmup > 0 && --glueWarmup == 0 && bandStageWanted (params))
            glueMixS.setTarget (1.0f);
        const float mix = glueMixS.next();
        const float amount = glueS.next();
        const float lfAmount = lfLimitS.next();

        std::array<std::array<float, kNumBands>, kMaxChannels> split {};
        std::array<float, kNumBands> level {};
        float bandSum = 0.0f;
        antiDenormal = kAntiDenormal - antiDenormal;
        for (int c = 0; c < numCh; ++c)
        {
            const auto ci = static_cast<size_t> (c);
            const float x = finiteOrZero (data[ci][i] * driveGain);
            data[ci][i] = x;
            auto& s = split[ci];
            splitter.processSample (c, x + antiDenormal, s[0], s[1], s[2]);
            bandSum += s[0] + s[1] + s[2];
        }
        if (! std::isfinite (bandSum))
        {
            // Last resort (the input is already finite): absurd but finite
            // levels overflowed the IIR states. Restart from rest rather than
            // staying non-finite forever.
            splitter.reset();
            for (int c = 0; c < numCh; ++c)
                split[static_cast<size_t> (c)] = {};
        }
        for (int c = 0; c < numCh; ++c)
            for (size_t b = 0; b < kNumBands; ++b)
                level[b] = std::max (level[b], std::abs (split[static_cast<size_t> (c)][b]));

        std::array<float, kNumBands> bandGain {};
        float lowHeld = 0.0f;
        for (size_t b = 0; b < kNumBands; ++b)
        {
            auto& det = bands[b];
            // Two alternating buckets: max(current, previous) always covers
            // the last holdLength .. 2 holdLength samples.
            det.bucket = std::max (det.bucket, level[b]);
            const float held = std::max (det.bucket, det.prevBucket);
            if (b == 0)
                lowHeld = held;
            if (--det.countdown <= 0)
            {
                det.prevBucket = det.bucket;
                det.bucket = 0.0f;
                det.countdown = det.holdLength;
            }
            const float coeff = held > det.env ? det.attack : det.release;
            det.env = held + coeff * (det.env - held);
            if (det.env < kEnvFlush)
                det.env = 0.0f; // decayed: no subnormal tail

            // Ratio 2:1 above T in the linear domain: out = T (env / T)^(1/2),
            // i.e. gain = sqrt (T / env). Blended by the glue amount.
            const float g = det.env > glueThreshold ? std::sqrt (glueThreshold / det.env) : 1.0f;
            bandGain[b] = 1.0f + amount * (g - 1.0f);
            glueMinGain = std::min (glueMinGain, 1.0f + mix * (bandGain[b] - 1.0f));
        }

        // LF-first limiter on the low band (after its glue gain): followed
        // whatever the amount, so it starts from the real level.
        {
            const float lowLevel = lowHeld * bandGain[0];
            const float coeff = lowLevel > lfEnv ? lfAttack : lfRelease;
            lfEnv = lowLevel + coeff * (lfEnv - lowLevel);
            if (lfEnv < kEnvFlush)
                lfEnv = 0.0f;
            if (lfAmount > 0.0f && lfEnv > lfThreshold)
            {
                const float lfGain = 1.0f + lfAmount * (lfThreshold / lfEnv - 1.0f);
                bandGain[0] *= lfGain;
                lfMinGain = std::min (lfMinGain, 1.0f + mix * (lfGain - 1.0f));
            }
        }

        for (int c = 0; c < numCh; ++c)
        {
            const auto ci = static_cast<size_t> (c);
            const auto& s = split[ci];
            const float x = data[ci][i];
            const float wet = s[0] * bandGain[0] + s[1] * bandGain[1] + s[2] * bandGain[2];
            data[ci][i] = x + mix * (wet - x);
        }

        // Fully faded out and switched off: stop spending cycles on the stage.
        if (mix == 0.0f && ! glueMixS.isSmoothing() && glueWarmup == 0 && ! bandStageWanted (params))
            glueRunning = false;
    }

    // The clipper's input is the whole-stage residual's reference (aligned
    // with the limiter's output in process()).
    residualRef.block (numCh, n).copyFrom (seg);

    // ---- 2) oversampled soft clipper, crossfaded against the aligned dry path --
    // Crest gate: the clipper input's short-term linked power, tracked
    // whether or not the clipper runs (so it starts from the real level);
    // the threshold rises to crestGain x RMS.
    for (int i = 0; i < n; ++i)
    {
        float p = 0.0f;
        for (int c = 0; c < numCh; ++c)
        {
            const float x = data[static_cast<size_t> (c)][i];
            p = std::max (p, x * x);
        }
        clipPowerFast += clipPowerCoeff * (std::min (p, std::numeric_limits<float>::max()) - clipPowerFast);
        clipPower += clipPowerCoeff * (clipPowerFast - clipPower);
        const float crestGain = crestGainS.next();
        if (crestGain > 0.0f)
        {
            auto& t = thresholdBuf[static_cast<size_t> (i)];
            t = std::max (t, crestGain * std::sqrt (clipPower));
        }
    }
    if (clipPower < kEnvFlush && clipPowerFast < kEnvFlush)
        clipPower = clipPowerFast = 0.0f;

    if (clipRunning)
    {
        const AudioBlock dry = dryBuffer.block (numCh, n);
        dry.copyFrom (seg);
        dryDelay.process (dry);

        const AudioBlock up = oversampler.upsample (seg); // factor 1: seg itself
        double diffEnergy = 0.0, inEnergy = 0.0;
        for (int c = 0; c < up.numChannels; ++c)
        {
            float* u = up.channel (c);
            // THD+N around the curve, at the oversampled rate where x^ and
            // the clipped x^ are aligned: the effective output is
            // x^ - removed (the clip weight included), i.e. d = -removed.
            // xx and dd are the clip-energy sums; only <x, removed> is new.
            double xx = 0.0, xr = 0.0, rr = 0.0;
            int k = 0;
            for (int i = 0; i < n; ++i)
            {
                const auto si = static_cast<size_t> (i);
                const float t = thresholdBuf[si];
                const float knee = kneeBuf[si];
                const float w = clipMixBuf[si];
                const float depthGain = depthBuf[si];
                for (int f = 0; f < osFactor; ++f, ++k)
                {
                    const float x = u[k];
                    const float y = softClipCapped (x, t, knee, depthGain);
                    const double xd = static_cast<double> (x);
                    const double removed = static_cast<double> (w) * static_cast<double> (x - y);
                    rr += removed * removed;
                    xx += xd * xd;
                    xr += xd * removed;
                    // Delta oversampling: only the clipping correction goes
                    // through the band-limiting downsampler.
                    u[k] = y - x;
                }
            }
            diffEnergy += rr;
            inEnergy += xx;
            if (const DistortionSums sums { xx, -xr, rr }; sums.isFinite())
                distortionWindow.channel (c).merge (sums);
        }
        if (std::isfinite (diffEnergy) && std::isfinite (inEnergy))
        {
            clipDiffEnergy += diffEnergy;
            clipInEnergy += inEnergy;
        }
        oversampler.downsample (seg); // seg now holds the band-limited correction

        // out = x (delayed exactly by the round trip) + LP(clip(x^) - x^). The
        // programme itself never passes the half-band filters, so unclipped
        // audio is bit-transparent and the top octave does not droop (the 2x/4x
        // stage-1 filters would otherwise cut 20 kHz by 1-2.3 dB at 44.1 kHz).
        // The clip mix is applied after the downsampler (as a base-rate
        // crossfade weight), so with the mix at 0 the output is exactly the
        // dry path whatever the filter tail holds - block-size independent.
        // The correction first passes the residual-path DC blocker, a 1st-
        // order TPT high-pass HP(r) = r - LP(r) with double state (docs/11
        // E10): only what the clipper adds is filtered, never the programme.
        const double dcG = clipDcG;
        for (int c = 0; c < numCh; ++c)
        {
            float* y = data[static_cast<size_t> (c)];
            const float* d = dry.channel (c);
            double lpState = clipDcLp[static_cast<size_t> (c)];
            for (int i = 0; i < n; ++i)
            {
                const double r = static_cast<double> (y[i]);
                const double v = (r - lpState) * dcG;
                const double lp = v + lpState;
                lpState = lp + v;
                y[i] = d[i] + clipMixBuf[static_cast<size_t> (i)] * static_cast<float> (r - lp);
            }
            // No denormals while the correction is silent (below every threshold).
            clipDcLp[static_cast<size_t> (c)] = std::abs (lpState) < 1.0e-30 ? 0.0 : lpState;
        }

        if (clipMixS.getCurrent() == 0.0f && ! clipMixS.isSmoothing() && clipWarmup == 0 && ! (params.clipAmount > 0.0f))
            clipRunning = false;
    }
    else
    {
        dryDelay.process (seg); // keeps the latency constant with the clipper off
    }

    // ---- 3) the true-peak limiter at the ceiling runs in process() ------------
}

void LoudnessMaximizer::process (const AudioBlock& block) noexcept FLUB_NONBLOCKING
{
    const int numCh = std::min ({ block.numChannels, spec.numChannels, kMaxChannels });
    const int numSamples = block.numSamples;
    if (! prepared || numSamples <= 0 || numCh <= 0)
        return;
    fresh = false;

    const AudioBlock io = block.firstChannels (numCh);
    double clipDiff = 0.0, clipIn = 0.0;
    float glueMin = 1.0f, lfMin = 1.0f, grMin = 0.0f;
    double closedGrSumDb = 0.0;
    int closedGrWindows = 0;
    // The block runs in pieces that end on a fixed grid of kGrWindowMs
    // windows counted from reset() (every stage is strictly per sample, so
    // the split changes nothing audible): the limiter's deepest GR is taken
    // per window, and the analysis windows (the clipper's THD+N and the
    // whole-stage residual, >= 25 ms) close only on that grid, so neither
    // depends on the host block size (docs/11 E06).
    for (int pos = 0; pos < numSamples;)
    {
        const int len = std::min ({ spec.maxBlockSize, numSamples - pos, grWindowLength - grWindowCount });
        AudioBlock seg = io.subBlock (pos, len);
        pos += len;
        // Prepared channels missing from this block are run on silence (the
        // output of those lanes is discarded). Otherwise their dry-delay line
        // and oversampler history would stand still and release audio from
        // before the gap when a full-width block comes back.
        for (int c = numCh; c < spec.numChannels; ++c)
        {
            float* pad = padBuffer.channel (c);
            std::fill (pad, pad + len, 0.0f);
            seg.ch[static_cast<size_t> (c)] = pad;
        }
        seg.numChannels = spec.numChannels;
        processSegment (seg, clipDiff, clipIn, glueMin, lfMin);

        // ---- 3) true-peak limiter at the ceiling ----------------------------
        limiter.process (seg);
        const float pieceGrDb = limiter.getGainReductionDb();
        grMin = std::min (grMin, pieceGrDb);
        grWindowMin = std::min (grWindowMin, pieceGrDb);
        grWindowCount += len;
        analysisPending += len;
        const bool onGrid = grWindowCount >= grWindowLength;
        if (onGrid)
        {
            closedGrSumDb += grWindowMin;
            ++closedGrWindows;
            grWindowMin = 0.0f;
            grWindowCount = 0;
        }

        // ---- 4) whole-stage residual: limiter output against the aligned clipper input
        {
            const AudioBlock ref = residualRef.block (spec.numChannels, len);
            residualDelay.process (ref);
            for (int c = 0; c < numCh; ++c)
            {
                const float* x = ref.channel (c);
                const float* y = seg.channel (c);
                DistortionSums sums;
                for (int i = 0; i < len; ++i)
                    sums.add (x[i], y[i] - x[i]);
                if (sums.isFinite())
                    residualWindow.channel (c).merge (sums);
            }
        }

        if (onGrid)
        {
            if (float db = kMinusInfDb, clipDb = kMinusInfDb; distortionWindow.advance (analysisPending, db, &clipDb))
            {
                distortionDb.store (db, std::memory_order_relaxed);
                windowClipDb.store (clipDb, std::memory_order_relaxed);
            }
            if (float db = kMinusInfDb; residualWindow.advance (analysisPending, db))
                residualDb.store (db, std::memory_order_relaxed);
            analysisPending = 0;
        }
    }

    limiterGrDb.store (grMin, std::memory_order_relaxed);
    if (closedGrWindows > 0) // several in a long block: their mean, so each window counts once
        windowGrDb.store (static_cast<float> (closedGrSumDb / closedGrWindows), std::memory_order_relaxed);
    glueGrDb.store (gainToDb (glueMin), std::memory_order_relaxed);
    lfGrDb.store (gainToDb (lfMin), std::memory_order_relaxed);
    const float ratioDb = clipDiff > 0.0 && clipIn > 0.0
                              ? static_cast<float> (std::max (static_cast<double> (kMinusInfDb), 10.0 * std::log10 (clipDiff / clipIn)))
                              : kMinusInfDb;
    clipRatioDb.store (ratioDb, std::memory_order_relaxed);
    clipInputEnergy.store (std::isfinite (clipIn) ? static_cast<float> (clipIn) : 0.0f, std::memory_order_relaxed);
}
} // namespace flub
