// Flubsound Pro - look-ahead true-peak brickwall limiter.
//
// Guarantee: the output never exceeds the ceiling in sample peak, and stays
// within ~0.1 dB of it in true (inter-sample) peak, with no gain overshoot.
// (True-peak scope: content inside the 4x detector's band, flat to ~0.4 fs -
// i.e. any ADC-recorded programme. Strong synthetic content between 0.4 fs
// and fs/2, e.g. raw digital white noise, is under-read by any short 4x
// interpolator; the sample-peak guarantee still holds exactly.)
//
// Algorithm (per sample n, all channels linked):
//   p[n]   = max over channels of the 4x TruePeakDetector interpolator (with
//            parabolic refinement of each local maximum of the 4x sequence),
//            or |x| when truePeak is off; the detector adds kDelay samples.
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
#include "flub/common/DelayLine.h"
#include "flub/common/SmoothedValue.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

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
    // Notes (details in TruePeakLimiter.cpp):
    //  * The ceiling glides over 50 ms (linear in dB). The safety clamp uses
    //    the ceiling that was in force when each output sample's gain was
    //    computed (ceilingRing), so a falling ceiling never trips it.
    //  * Auto release blends the release coefficient from fast to slow while
    //    the span of the current limiting run grows from 25 to 50 ms.

    /** Linked-detection front end: exactly TruePeakDetector's 4x polyphase
        interpolator (same taps, same kDelay), but instead of the plain maximum
        of the four grid points it also refines every local maximum of the 4x
        sequence with a parabola through its two neighbours. The plain grid
        maximum under-reads a peak that falls between grid points by up to
        0.12 dB for content at 0.3 fs and 0.25 dB at 0.4 fs - more than the
        0.05 dB margin; refined, the residual is < 0.03 dB. */
    class RefinedPeakDetector
    {
    public:
        void prepare (int numChannels);
        void reset() noexcept;
        /** Consumes x[n]; returns the peak magnitude over positions
            [n - kDelay - 1/8, n - kDelay + 7/8). */
        float processSample (int ch, float x) noexcept;

    private:
        static constexpr int kPhases = TruePeakDetector::kPhases;
        static constexpr int kTaps = TruePeakDetector::kTapsPerPhase;
        static constexpr int kDelay = TruePeakDetector::kDelay;
        std::array<std::array<float, kTaps>, kPhases> phaseTaps {};
        std::array<std::vector<float>, kMaxChannels> history; // mirrored, newest first
        std::array<int, kMaxChannels> pos {};
        std::array<float, kMaxChannels> lastPhase {};          // |z| at n - kDelay - 1/4
    };

    static LimiterParams sanitised (const LimiterParams& p, const LimiterParams& fallback) noexcept;
    void updateReleaseCoeffs() noexcept;
    void updateCeiling (float ceilingDb) noexcept;

    float lookaheadMs = 1.5f;
    bool truePeak = true;
    ProcessSpec spec;
    LimiterParams params;
    std::atomic<float> grDb { 0.0f };
    std::atomic<uint64_t> safetyClips { 0 };

    // Structural values latched by prepare() (the setters above only take
    // effect there, so the latency can never change behind the chain's back).
    bool prepared = false;
    bool fresh = true;     // nothing processed since prepare()/reset(): setParams() applies instantly
    bool detectTruePeak = true;
    int lookahead = 0;     // L
    int detectorDelay = 0; // D (TruePeakDetector::kDelay, or 0 for sample peak)

    RefinedPeakDetector detector;
    DelayLine audioDelay;  // L + D

    // Sliding minimum of r over the last L + 2 samples: a monotonic deque
    // (values increase from front to back) kept in a fixed power-of-two ring.
    std::vector<float> dequeValue;
    std::vector<uint32_t> dequeIndex;
    uint32_t dequeMask = 0, dequeFront = 0, dequeBack = 0; // back = one past the newest
    uint32_t window = 2;                                   // L + 2
    uint32_t sampleIndex = 0;                              // wraps; only differences are used

    // Box filter (running mean over L + 1 samples of the sliding minimum) and
    // the per-sample ceiling history for the safety clamp; both rings share ringPos.
    std::vector<float> boxRing, ceilingRing;
    int ringSize = 1, ringPos = 0;
    double boxSum = 1.0, boxLength = 1.0;

    // Ceiling: smoothed in dB; linear value and detector threshold derived from it.
    LinearSmoothedValue ceilingDbS;
    float ceilingLin = 1.0f, thresholdLin = 1.0f;

    // Gain state and release. Double precision: a float one-pole with a long
    // release stalls a few 1e-5 below its target (the per-sample step drops
    // under half an ulp of 1.0), which would leave a permanent tiny reduction.
    double gain = 1.0;
    double fastCoeff = 0.0, slowCoeff = 0.0;
    int sinceOver = 0, gapSamples = 1;      // samples since r < 1; run continuity gap
    int runAge = 0, runSpan = 0;            // age of the current limiting run / span to its last over
    int blendStart = 1, blendEnd = 2;       // runSpan range over which release goes fast -> slow
    double blendScale = 1.0;                // 1 / (blendEnd - blendStart)
};
} // namespace flub
