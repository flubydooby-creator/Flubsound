// Flubsound Pro - look-ahead true-peak brickwall limiter.
//
// Guarantee: the output never exceeds the ceiling in sample peak, and stays
// within ~0.1 dB of it in true (inter-sample) peak, with no gain overshoot.
//
// Algorithm (per sample n, all channels linked):
//   p[n]   = max over channels of TruePeakDetector (4x) output, or |x| when
//            truePeak is off; the detector adds kDelay samples of delay.
//   r[n]   = min(1, ceilingLin / p[n])                  required gain
//   m[n]   = min(r[n-L-1 .. n])        sliding minimum (monotonic deque, O(1))
//   a[n]   = mean(m[n-L .. n])         box filter (running sum, double)
//            -> linear attack ramp that reaches r exactly when the peak
//               sample leaves the look-ahead delay; provably a[n] <= r[n-L].
//   g[n]   = min(a[n], release(g[n-1]))  one-pole release towards a[n]
//            autoRelease: fast (releaseMs/5) for isolated peaks, slow
//            (releaseMs) once limiting has been continuous for > 50 ms.
//   y[n]   = x[n - L - D] * g[n]       L = lookahead, D = detector delay
//   final safety: hard clamp to ceilingLin (counts engagements; must be 0
//            in tests - it exists only to make overs impossible).
// An internal margin of 0.05 dB below the ceiling absorbs interpolation error.
// latency = L + D (1.5 ms + 12 samples at 48 kHz = 84 samples by default).
#pragma once

#include "Processor.h"
#include "TruePeakDetector.h"

#include <atomic>
#include <cstdint>

namespace flub
{
struct LimiterParams
{
    float ceilingDb = -1.0f; // -12 .. 0 dBTP
    float releaseMs = 80.0f; // 5 .. 1000
    bool autoRelease = true;

    bool operator== (const LimiterParams&) const = default;
};

class TruePeakLimiter final : public Processor
{
public:
    /** Structural: call before prepare(). */
    void setLookaheadMs (float ms) noexcept { lookaheadMs = ms; }
    void setTruePeakDetection (bool enabled) noexcept { truePeak = enabled; }

    void prepare (const ProcessSpec& spec) override;
    void reset() noexcept override;
    void process (const AudioBlock& block) noexcept override;
    int latencySamples() const noexcept override;
    const char* name() const noexcept override { return "True-Peak Limiter"; }

    void setParams (const LimiterParams& p) noexcept;
    const LimiterParams& getParams() const noexcept { return params; }

    /** Deepest gain reduction in the last block (dB, <= 0). */
    float getGainReductionDb() const noexcept { return grDb.load (std::memory_order_relaxed); }
    /** Number of samples the final safety clamp had to touch since prepare(). */
    uint64_t getSafetyClipCount() const noexcept { return safetyClips.load (std::memory_order_relaxed); }

private:
    // ---- implementation-defined below this line ----
    float lookaheadMs = 1.5f;
    bool truePeak = true;
    ProcessSpec spec;
    LimiterParams params;
    std::atomic<float> grDb { 0.0f };
    std::atomic<uint64_t> safetyClips { 0 };
};
} // namespace flub
