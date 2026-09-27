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
//        Delta design: out = x + HP5(clip(x^) - x^), the correction band-
//        limited by the downsampler and passed through a 5 Hz 1st-order
//        high-pass (docs/11 E10), so clipping an asymmetric waveform leaves
//        no DC on the driver.
//     -> [limit] TruePeakLimiter at the ceiling (look-ahead, true peak).
// Telemetry: clipEnergyRatioDb = 10 log10(sum (x - clip(x))^2 / sum x^2) over
//   the last block (how hard the clipper works), the limiter's gain
//   reduction, and distortionDb: the clipper's THD+N over the last analysis
//   window (closed at the first segment boundary at or after 25 ms),
//   measured at the oversampled rate around the curve itself, where input
//   and clipped output are aligned (DistortionEstimator.h: residual after the
//   least-squares gain, relative to the output energy). The chain feeds the
//   limiter GR (deepest per 10 ms window, getWindowGainReductionDb()) and the combined THD+N of the nonlinear stages (this one
//   floored at the clip energy ratio over the same window,
//   getWindowClipEnergyDb()) to the SafetyGovernor, which backs the
//   governed macro contributions off when either exceeds its budget.
// latency = clipper oversampler latency + limiter latency.
#pragma once

#include "Crossover.h"
#include "DistortionEstimator.h"
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
    void reset() noexcept FLUB_NONBLOCKING override;
    void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING override;
    int latencySamples() const noexcept override;
    const char* name() const noexcept override { return "Loudness Maximizer"; }

    void setParams (const MaximizerParams& p) noexcept FLUB_NONBLOCKING;
    const MaximizerParams& getParams() const noexcept { return params; }

    /** Soft-clip transfer curve (threshold t, knee 0..1), exposed for tests/GUI. */
    static float softClip (float x, float threshold, float knee) noexcept;

    /** Deepest limiter gain reduction in the last block (dB <= 0; the meter). */
    float getGainReductionDb() const noexcept { return limiterGrDb.load (std::memory_order_relaxed); }
    /** Deepest limiter gain reduction per fixed kGrWindowMs window (dB <= 0),
        the mean over the windows that closed in the last block that closed
        any: the SafetyGovernor's GR input, independent of the host block size. */
    float getWindowGainReductionDb() const noexcept { return windowGrDb.load (std::memory_order_relaxed); }
    static constexpr float kGrWindowMs = 10.0f;
    /** Engagements of the limiter's final safety clamp since prepare() (0 in normal operation). */
    uint64_t getSafetyClipCount() const noexcept { return limiter.getSafetyClipCount(); }
    float getGlueReductionDb() const noexcept { return glueGrDb.load (std::memory_order_relaxed); }
    float getClipEnergyRatioDb() const noexcept { return clipRatioDb.load (std::memory_order_relaxed); }
    /** THD+N of the soft clipper over the last 25 ms analysis window (dB re its output; -160 = clean or off). */
    float getDistortionDb() const noexcept { return distortionDb.load (std::memory_order_relaxed); }
    /** The clip energy ratio over that same window (the chain floors the
        governor's clipper input with it; getClipEnergyRatioDb() is per block). */
    float getWindowClipEnergyDb() const noexcept { return windowClipDb.load (std::memory_order_relaxed); }

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
    //    (flat to 0.4535 fs, see TruePeakDetector.h); very hard clipping
    //    creates intermodulation up to fs/2, where every 4x interpolator
    //    rolls off, so an ideal reconstruction can read slightly higher
    //    (measured: every factory preset at full macros stays below the
    //    ceiling on the 4x meter, worst -1.04 dBTP for a -1 dBTP ceiling).

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
    std::atomic<float> limiterGrDb { 0.0f }, glueGrDb { 0.0f }, clipRatioDb { -160.0f }, distortionDb { -160.0f },
        windowClipDb { -160.0f }, windowGrDb { 0.0f };
    int grWindowLength = 480, grWindowCount = 0; // limiter GR window (kGrWindowMs), samples
    float grWindowMin = 0.0f;                    // deepest GR in the open window (dB)
    DistortionWindow distortionWindow; // clipper THD+N sums over a window of at least 25 ms (closes at a segment boundary)

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
    // Residual-path DC blocker on the clipper's correction (docs/11 E10):
    // TPT one-pole low-pass state per channel (double) and its coefficient.
    std::array<double, kMaxChannels> clipDcLp {};
    double clipDcG = 0.0;

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
