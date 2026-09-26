// Flubsound Pro - intelligent true-peak loudness maximizer + soft clipper.
//
//   x * drive
//     -> [glue] 3-band pre-compression (ThreeBandSplitter 120 Hz / 4 kHz,
//        linked per band, ratio 2:1, threshold ceiling - 6 dB, 5/80 ms,
//        amount = glue). Balances the band levels so one band (usually the
//        bass) does not dominate the limiter: louder AND less pumping.
//     -> [clip] oversampled soft clipper (clipOversampling x, default 4x):
//        threshold t = ceilingLin * 10^(clipHeadroomDb/20), with
//        clipHeadroomDb = lerp(+6 dB, +0.3 dB, clipAmount); soft knee:
//          |x| <= ks          : y = x                   ks = t (1 - 0.5 knee)
//          |x| >  ks          : y = sign * (ks + (t - ks) tanh((|x| - ks)/(t - ks)))
//        Shaves sub-millisecond transients (snare/gunshot crack) cheaply, so
//        the limiter only handles the longer peaks -> less audible pumping.
//        clipAmount = 0 disables the clipper entirely (limiter-only).
//     -> [limit] TruePeakLimiter at the ceiling (look-ahead, true peak).
// THD protection telemetry: clipEnergyRatioDb = 10 log10(sum (x - clip(x))^2
//   / sum x^2) over the last block, and the limiter's gain reduction; the
//   SafetyGovernor backs Boost Intensity off when either exceeds its budget.
// latency = clipper oversampler latency + limiter latency.
#pragma once

#include "Crossover.h"
#include "Oversampler.h"
#include "Processor.h"
#include "TruePeakLimiter.h"
#include "flub/common/DelayLine.h"
#include "flub/common/SmoothedValue.h"

#include <array>
#include <atomic>
#include <vector>

namespace flub
{
struct MaximizerParams
{
    float driveDb = 0.0f;    // 0 .. 24
    float ceilingDb = -1.0f; // -12 .. 0 dBTP
    float clipAmount = 0.5f; // 0 .. 1
    float clipKnee = 0.5f;   // 0 .. 1
    float glue = 0.0f;       // 0 .. 1
    float releaseMs = 60.0f; // 5 .. 1000
    bool autoRelease = true;

    bool operator== (const MaximizerParams&) const = default;
};

class LoudnessMaximizer final : public Processor
{
public:
    /** Structural: call before prepare(). */
    void setClipOversampling (int factor, Oversampler::Quality q = Oversampler::Quality::High) noexcept
    {
        clipOsFactor = factor;
        clipOsQuality = q;
    }
    void setLookaheadMs (float ms) noexcept { lookaheadMs = ms; }
    void setTruePeakDetection (bool enabled) noexcept { truePeak = enabled; }

    void prepare (const ProcessSpec& spec) override;
    void reset() noexcept override;
    void process (const AudioBlock& block) noexcept override;
    int latencySamples() const noexcept override;
    const char* name() const noexcept override { return "Loudness Maximizer"; }

    void setParams (const MaximizerParams& p) noexcept;
    const MaximizerParams& getParams() const noexcept { return params; }

    /** Soft-clip transfer curve (threshold t, knee 0..1), exposed for tests/GUI. */
    static float softClip (float x, float threshold, float knee) noexcept;

    float getGainReductionDb() const noexcept { return limiterGrDb.load (std::memory_order_relaxed); }
    float getGlueReductionDb() const noexcept { return glueGrDb.load (std::memory_order_relaxed); }
    float getClipEnergyRatioDb() const noexcept { return clipRatioDb.load (std::memory_order_relaxed); }

private:
    // ---- implementation-defined below this line ----
    // Notes (details in LoudnessMaximizer.cpp):
    //  * Continuous parameters glide over 50 ms; the glue and clipper stages
    //    are switched on/off by crossfading against a latency-aligned dry path
    //    (after a short warm-up of the stage), so on/off is click-free and the
    //    latency never changes.
    //  * Glue detector per band: linked |x| -> two-bucket peak hold (half a
    //    period of the band's lowest frequency) -> 5/80 ms branching follower
    //    -> ratio-2 gain sqrt(T / env) above T.
    //  * The final limiter holds the sample ceiling exactly. Its true-peak
    //    accuracy (<= +0.1 dB) needs the signal inside the 4x detector band
    //    (~0.4 fs); very hard clipping (clip energy above about -12 dB)
    //    creates intermodulation up to fs/2 and can then read up to ~+0.3 dB
    //    on an ideal reconstruction.

    /** Linked level detector of one glue band. */
    struct BandDetector
    {
        float bucket = 0.0f, prevBucket = 0.0f, env = 0.0f;
        int countdown = 1, holdLength = 1;
        float attack = 0.0f, release = 0.0f;
    };

    static constexpr int kNumBands = 3;

    static MaximizerParams sanitised (const MaximizerParams& p, const MaximizerParams& fallback) noexcept;
    void applyParamsImmediately() noexcept;
    void startGlue (bool immediate) noexcept;
    void startClipper (bool immediate) noexcept;
    void updateCeiling (float newCeilingDb) noexcept;
    void updateClipThreshold() noexcept;
    void processSegment (const AudioBlock& seg, double& clipDiffEnergy, double& clipInEnergy, float& glueMinGain) noexcept;

    int clipOsFactor = 4;
    Oversampler::Quality clipOsQuality = Oversampler::Quality::High;
    float lookaheadMs = 1.5f;
    bool truePeak = true;
    ProcessSpec spec;
    MaximizerParams params;
    std::atomic<float> limiterGrDb { 0.0f }, glueGrDb { 0.0f }, clipRatioDb { -160.0f };

    bool prepared = false;
    bool fresh = true;           // nothing processed since prepare()/reset(): setParams() applies instantly

    // Silence for prepared channels that a block leaves out: every stage (dry
    // delay, oversampler, splitter, limiter) keeps running on all prepared
    // channels, so no stale audio is released when a wider block returns.
    AudioBuffer padBuffer;

    // Clipper: oversampled soft clip, crossfaded against a dry path delayed by
    // exactly the oversampler round trip.
    int osFactor = 1;
    Oversampler oversampler;
    DelayLine dryDelay;
    AudioBuffer dryBuffer;
    std::vector<float> thresholdBuf, kneeBuf, clipMixBuf; // per-sample clip controls (maxBlockSize)
    bool clipRunning = false;
    int clipWarmup = 0, clipWarmupLength = 1;

    // Glue: 3-band split, per-band linked compressor, crossfaded against the input.
    ThreeBandSplitter splitter;
    std::array<BandDetector, kNumBands> bands {};
    bool glueRunning = false;
    int glueWarmup = 0, glueWarmupLength = 1;
    float antiDenormal = 0.0f;   // alternates 1e-20 / 0 on the splitter input

    TruePeakLimiter limiter;

    // Smoothed controls.
    LinearSmoothedValue driveDbS, ceilingDbS, clipAmountS, clipKneeS, glueS, glueMixS, clipMixS;
    float driveGain = 1.0f, ceilingDb = -1.0f, ceilingLin = 1.0f, glueThreshold = 0.5f, clipThreshold = 1.0f;
};
} // namespace flub
