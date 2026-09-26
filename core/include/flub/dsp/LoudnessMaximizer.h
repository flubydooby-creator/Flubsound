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

#include "Oversampler.h"
#include "Processor.h"

#include <atomic>

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
    int clipOsFactor = 4;
    Oversampler::Quality clipOsQuality = Oversampler::Quality::High;
    float lookaheadMs = 1.5f;
    bool truePeak = true;
    ProcessSpec spec;
    MaximizerParams params;
    std::atomic<float> limiterGrDb { 0.0f }, glueGrDb { 0.0f }, clipRatioDb { -160.0f };
};
} // namespace flub
