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

// Clipper threshold headroom above the ceiling: lerp (+6 dB, +0.3 dB, amount).
constexpr float kClipHeadroomMaxDb = 6.0f;
constexpr float kClipHeadroomMinDb = 0.3f;

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

//==============================================================================
MaximizerParams LoudnessMaximizer::sanitised (const MaximizerParams& in, const MaximizerParams& fallback) noexcept
{
    MaximizerParams p = in;
    p.driveDb = sanitise (p.driveDb, 0.0f, 24.0f, fallback.driveDb);
    p.ceilingDb = sanitise (p.ceilingDb, -12.0f, 0.0f, fallback.ceilingDb);
    p.clipAmount = sanitise (p.clipAmount, 0.0f, 1.0f, fallback.clipAmount);
    p.clipKnee = sanitise (p.clipKnee, 0.0f, 1.0f, fallback.clipKnee);
    p.glue = sanitise (p.glue, 0.0f, 1.0f, fallback.glue);
    p.releaseMs = sanitise (p.releaseMs, 5.0f, 1000.0f, fallback.releaseMs);
    return p;
}

void LoudnessMaximizer::updateCeiling (float newCeilingDb) noexcept
{
    ceilingDb = newCeilingDb;
    ceilingLin = dbToGain (newCeilingDb);
    glueThreshold = ceilingLin * kGlueThresholdGain;
}

void LoudnessMaximizer::updateClipThreshold() noexcept
{
    const float headroomDb = lerp (kClipHeadroomMaxDb, kClipHeadroomMinDb, clipAmountS.getCurrent());
    clipThreshold = dbToGain (ceilingDb + headroomDb);
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
    glueRunning = true;
    if (immediate)
    {
        glueS.setImmediate (params.glue);
        glueMixS.setImmediate (1.0f);
        glueWarmup = 0;
    }
    else
    {
        glueS.setImmediate (0.0f);
        glueS.setTarget (params.glue);
        glueMixS.setImmediate (0.0f);
        glueWarmup = glueWarmupLength;
    }
}

void LoudnessMaximizer::startClipper (bool immediate) noexcept
{
    oversampler.reset();
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
    ceilingDbS.setImmediate (params.ceilingDb);
    updateCeiling (params.ceilingDb);
    clipAmountS.setImmediate (params.clipAmount);
    clipKneeS.setImmediate (params.clipKnee);
    updateClipThreshold();

    if (params.glue > 0.0f)
    {
        startGlue (true);
    }
    else
    {
        glueRunning = false;
        glueWarmup = 0;
        glueS.setImmediate (0.0f);
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
    thresholdBuf.assign (static_cast<size_t> (spec.maxBlockSize), 1.0f);
    kneeBuf.assign (static_cast<size_t> (spec.maxBlockSize), 0.0f);
    clipMixBuf.assign (static_cast<size_t> (spec.maxBlockSize), 0.0f);
    clipWarmupLength = 2 * oversampler.latencySamples() + kClipWarmupExtra;

    splitter.prepare (fs, kGlueLowMidHz, kGlueMidHighHz);
    for (size_t b = 0; b < bands.size(); ++b)
    {
        bands[b].holdLength = std::max (1, static_cast<int> (std::lround (fs / (2.0 * kBandLowestHz[b]))));
        bands[b].attack = onePoleCoeff (kGlueAttackMs, fs);
        bands[b].release = onePoleCoeff (kGlueReleaseMs, fs);
    }
    glueWarmupLength = std::max (1, msToSamples (kGlueWarmupMs, fs));

    // Structural limiter settings are passed through here.
    limiter.setLookaheadMs (lookaheadMs);
    limiter.setTruePeakDetection (truePeak);
    limiter.prepare (spec);
    limiter.setParams ({ params.ceilingDb, params.releaseMs, params.autoRelease });

    prepared = true;
    reset();
}

void LoudnessMaximizer::reset() noexcept
{
    const double fs = spec.sampleRate;
    oversampler.reset();
    dryDelay.reset();
    splitter.reset();
    antiDenormal = 0.0f;
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
    glueMixS.reset (fs, kGlueFadeMs, 0.0f);
    clipMixS.reset (fs, kClipFadeMs, 0.0f);
    applyParamsImmediately(); // no previous output to click against

    limiterGrDb.store (0.0f, std::memory_order_relaxed);
    glueGrDb.store (0.0f, std::memory_order_relaxed);
    clipRatioDb.store (kMinusInfDb, std::memory_order_relaxed);
    fresh = true;
}

int LoudnessMaximizer::latencySamples() const noexcept
{
    return oversampler.latencySamples() + limiter.latencySamples();
}

void LoudnessMaximizer::setParams (const MaximizerParams& newParams) noexcept
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

    driveDbS.setTarget (p.driveDb);
    ceilingDbS.setTarget (p.ceilingDb);
    clipAmountS.setTarget (p.clipAmount);
    clipKneeS.setTarget (p.clipKnee);

    if (p.glue > 0.0f)
    {
        if (! glueRunning)
        {
            startGlue (false);
        }
        else
        {
            glueS.setTarget (p.glue);
            if (glueWarmup == 0)
                glueMixS.setTarget (1.0f);
        }
    }
    else
    {
        // Fade the bands out; the stage stops once the fade has finished.
        glueS.setTarget (0.0f);
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
                                        float& glueMinGain) noexcept
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
        if (glueWarmup > 0 && --glueWarmup == 0 && params.glue > 0.0f)
            glueMixS.setTarget (1.0f);
        const float mix = glueMixS.next();
        const float amount = glueS.next();

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
        for (size_t b = 0; b < kNumBands; ++b)
        {
            auto& det = bands[b];
            // Two alternating buckets: max(current, previous) always covers
            // the last holdLength .. 2 holdLength samples.
            det.bucket = std::max (det.bucket, level[b]);
            const float held = std::max (det.bucket, det.prevBucket);
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

        for (int c = 0; c < numCh; ++c)
        {
            const auto ci = static_cast<size_t> (c);
            const auto& s = split[ci];
            const float x = data[ci][i];
            const float wet = s[0] * bandGain[0] + s[1] * bandGain[1] + s[2] * bandGain[2];
            data[ci][i] = x + mix * (wet - x);
        }

        // Fully faded out and switched off: stop spending cycles on the stage.
        if (mix == 0.0f && ! glueMixS.isSmoothing() && glueWarmup == 0 && ! (params.glue > 0.0f))
            glueRunning = false;
    }

    // ---- 2) oversampled soft clipper, crossfaded against the aligned dry path --
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
            int k = 0;
            for (int i = 0; i < n; ++i)
            {
                const auto si = static_cast<size_t> (i);
                const float t = thresholdBuf[si];
                const float knee = kneeBuf[si];
                const float w = clipMixBuf[si];
                for (int f = 0; f < osFactor; ++f, ++k)
                {
                    const float x = u[k];
                    const float y = softClip (x, t, knee);
                    const double removed = static_cast<double> (w) * static_cast<double> (x - y);
                    diffEnergy += removed * removed;
                    inEnergy += static_cast<double> (x) * static_cast<double> (x);
                    // Delta oversampling: only the clipping correction goes
                    // through the band-limiting downsampler.
                    u[k] = y - x;
                }
            }
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
        for (int c = 0; c < numCh; ++c)
        {
            float* y = data[static_cast<size_t> (c)];
            const float* d = dry.channel (c);
            for (int i = 0; i < n; ++i)
                y[i] = d[i] + clipMixBuf[static_cast<size_t> (i)] * y[i];
        }

        if (clipMixS.getCurrent() == 0.0f && ! clipMixS.isSmoothing() && clipWarmup == 0 && ! (params.clipAmount > 0.0f))
            clipRunning = false;
    }
    else
    {
        dryDelay.process (seg); // keeps the latency constant with the clipper off
    }

    // ---- 3) true-peak limiter at the ceiling -----------------------------------
    limiter.process (seg);
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
    float glueMin = 1.0f, grMin = 0.0f;
    for (int pos = 0; pos < numSamples; pos += spec.maxBlockSize)
    {
        const int len = std::min (spec.maxBlockSize, numSamples - pos);
        AudioBlock seg = io.subBlock (pos, len);
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
        processSegment (seg, clipDiff, clipIn, glueMin);
        grMin = std::min (grMin, limiter.getGainReductionDb());
    }

    limiterGrDb.store (grMin, std::memory_order_relaxed);
    glueGrDb.store (gainToDb (glueMin), std::memory_order_relaxed);
    const float ratioDb = clipDiff > 0.0 && clipIn > 0.0
                              ? static_cast<float> (std::max (static_cast<double> (kMinusInfDb), 10.0 * std::log10 (clipDiff / clipIn)))
                              : kMinusInfDb;
    clipRatioDb.store (ratioDb, std::memory_order_relaxed);
}
} // namespace flub
