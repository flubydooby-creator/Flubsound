// Flubsound Pro - look-ahead true-peak brickwall limiter.
//
// Guarantee: the output never exceeds the ceiling in sample peak, and stays
// within ~0.1 dB of it in true (inter-sample) peak, with no gain overshoot.
// (True-peak scope: the detector is the meters' own 4x interpolator - 40
// taps/phase, Kaiser beta 5 - within -0.02 / +0.04 dB to 0.4535 fs, i.e. 20 kHz
// at 44.1 kHz. Measured: full-level noise band-limited to 0.45 fs (a CD-style
// 20 kHz passband) peaks within +0.02 dB of the ceiling; raw digital white
// noise, whose energy reaches fs/2 where every 4x interpolator rolls off, up
// to ~+1.2 dB. Mastered programme carries far less energy up there. The
// sample-peak guarantee holds exactly for any input.)
//
// Algorithm (per sample n, all channels linked):
//   p[n]   = max over channels of the 4x TruePeakDetector interpolator (with
//            parabolic refinement of each local maximum of the 4x sequence),
//            or |x| when truePeak is off; the detector adds kDelay samples.
//   r[n]   = min(1, threshold / p[n])                   required gain
//   m[n]   = min(r[n-L-Kh-1 .. n])     sliding minimum (monotonic deque, O(1))
//   a[n]   = mean(m[n-L+Kh .. n])      box filter (running sum, double)
//            -> linear attack ramp of L - Kh + 1 samples that reaches r Kh
//               samples before the peak sample leaves the look-ahead delay
//               and holds it until Kh samples after; provably a[n] <= r[j]
//               for every j in [n-L-Kh-1, n-L+Kh]. Kh = min(8, L/3) in
//               true-peak mode (the gain is flat under the central taps of
//               the interpolator that reads the peak), 0 in sample-peak mode.
//   g[n]   = min(a[n], release(g[n-1]))  one-pole release towards a[n]
//            autoRelease: fast (releaseMs/5) for isolated peaks, slow
//            (releaseMs) once limiting has been continuous for > 50 ms.
//   y[n]   = x[n - L - D] * g[n]       L = lookahead, D = detector delay
//   final safety: hard clamp to ceilingLin (counts engagements; must be 0
//            in tests - it exists only to make overs impossible).
// threshold = ceiling - 0.05 dB: the margin absorbs interpolation error.
// latency = L + D (1.5 ms + 20 samples at 48 kHz = 92 samples by default).
//
// Optional LF-safe envelope (docs/11 E05 stage 1; setEnvelope(), structural,
// all off by default - the maximizer turns them on, the MixEngine master and
// the chain's bypass-reference limiter keep the plain envelope):
//   * lowFrequencyHold: the sliding minimum also covers the H samples after
//     the peak (window L + Kh + 2 + H), so the gain holds H after each peak
//     before it releases. H = 10 ms, or 25 ms while energy below 50 Hz is
//     present (a 2nd-order 50 Hz low-pass of the channel mean carries more
//     than 10 % of its power, 100 ms averages; back to 10 ms below 5 %):
//     at least half a period of the lowest frequency present, so a steady
//     bass note's gain does not ripple at twice its frequency.
//   * smoothAttack: the box filter becomes two cascaded boxes whose lengths
//     add up to L - Kh + 2 (a triangular kernel over the same L - Kh + 1
//     samples): an S-shaped attack instead of a linear ramp with corners.
//     The support is unchanged, so a[n] <= r[j] still holds.
//   * programEnvelope: a slow program gain p follows g (attack 150 ms,
//     release 800 ms, one-pole in the linear gain) and the output gain is
//     min(g, p) <= g: sustained limiting keeps a base reduction, so dense
//     kicks modulate everything else less. The ceiling guarantee is g's.
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

/** The optional LF-safe envelope stages (see the header comment); structural. */
struct LimiterEnvelope
{
    bool lowFrequencyHold = false;
    bool smoothAttack = false;
    bool programEnvelope = false;

    bool operator== (const LimiterEnvelope&) const = default;
};

class TruePeakLimiter final : public Processor
{
public:
    /** Structural: call before prepare(). */
    void setLookaheadMs (float ms) noexcept { lookaheadMs = ms; }
    void setTruePeakDetection (bool enabled) noexcept { truePeak = enabled; }
    void setEnvelope (const LimiterEnvelope& e) noexcept { envelopeRequest = e; }

    void prepare (const ProcessSpec& spec) override;
    void reset() noexcept FLUB_NONBLOCKING override;
    void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING override;
    int latencySamples() const noexcept override;
    const char* name() const noexcept override { return "True-Peak Limiter"; }

    void setParams (const LimiterParams& p) noexcept FLUB_NONBLOCKING;
    const LimiterParams& getParams() const noexcept { return params; }

    /** Deepest gain reduction in the last block (dB, <= 0). */
    float getGainReductionDb() const noexcept { return grDb.load (std::memory_order_relaxed); }
    /** Number of samples the final safety clamp had to touch since prepare(). */
    uint64_t getSafetyClipCount() const noexcept { return safetyClips.load (std::memory_order_relaxed); }
    /** The gain hold after each peak at the end of the last block (ms; 0 without lowFrequencyHold). */
    float getHoldMs() const noexcept { return holdMs.load (std::memory_order_relaxed); }

private:
    // ---- implementation-defined below this line ----
    // Notes (details in TruePeakLimiter.cpp):
    //  * The ceiling glides over 50 ms (linear in dB). The safety clamp uses
    //    the ceiling that was in force when each output sample's gain was
    //    computed (ceilingRing), so a falling ceiling never trips it.
    //  * Auto release blends the release coefficient from fast to slow while
    //    the span of the current limiting run grows from 25 to 50 ms.

    /** Linked-detection front end: exactly TruePeakDetector's 4x polyphase
        interpolator (same taps via TruePeakDetector::designPhaseTaps, same
        kDelay), but instead of the plain maximum of the four grid points it
        also refines every local maximum of the 4x sequence with a parabola
        through its two neighbours. The plain grid maximum under-reads a peak
        that falls halfway between grid points by up to cos(pi f / 4 fs):
        0.24 dB at 0.3 fs and 0.44 dB at 0.4 fs - more than the 0.05 dB
        margin; refined, the residual is < 0.03 dB. */
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
        TruePeakDetector::PhaseTaps phaseTaps {};
        std::array<std::vector<float>, kMaxChannels> history; // mirrored, newest first
        std::array<int, kMaxChannels> pos {};
        std::array<float, kMaxChannels> lastPhase {};          // |z| at n - kDelay - 1/4
    };

    static LimiterParams sanitised (const LimiterParams& p, const LimiterParams& fallback) noexcept;
    void updateReleaseCoeffs() noexcept;
    void updateCeiling (float ceilingDb) noexcept;

    float lookaheadMs = 1.5f;
    bool truePeak = true;
    LimiterEnvelope envelopeRequest;
    ProcessSpec spec;
    LimiterParams params;
    std::atomic<float> grDb { 0.0f }, holdMs { 0.0f };
    std::atomic<uint64_t> safetyClips { 0 };

    // Structural values latched by prepare() (the setters above only take
    // effect there, so the latency can never change behind the chain's back).
    bool prepared = false;
    bool fresh = true;     // nothing processed since prepare()/reset(): setParams() applies instantly
    bool detectTruePeak = true;
    int lookahead = 0;     // L
    int detectorDelay = 0; // D (TruePeakDetector::kDelay, or 0 for sample peak)
    int hold = 0;          // Kh: extra gain hold each side of a peak (true-peak mode)
    LimiterEnvelope envelope; // latched from envelopeRequest

    RefinedPeakDetector detector;
    DelayLine audioDelay;  // L + D

    // Sliding minimum of r over the last L + Kh + 2 (+ H) samples: a monotonic
    // deque (values increase from front to back) kept in a fixed power-of-two ring.
    std::vector<float> dequeValue;
    std::vector<uint32_t> dequeIndex;
    uint32_t dequeMask = 0, dequeFront = 0, dequeBack = 0; // back = one past the newest
    uint32_t window = 2;                                   // L + Kh + 2 + H
    uint32_t baseWindow = 2;                               // L + Kh + 2
    uint32_t sampleIndex = 0;                              // wraps; only differences are used

    // Box filter (running mean over L - Kh + 1 samples of the sliding minimum;
    // with smoothAttack two cascaded boxes of ringSize + ring2Size - 1 = L -
    // Kh + 1 samples) and the per-sample ceiling history for the safety clamp
    // (L + 1 samples).
    std::vector<float> boxRing, ceilingRing;
    std::vector<double> box2Ring;
    int ringSize = 1, ringPos = 0, ceilingPos = 0, ring2Size = 1, ring2Pos = 0;
    double boxSum = 1.0, boxLength = 1.0, box2Sum = 1.0, box2Length = 1.0;

    // lowFrequencyHold: H (samples) and its LF detector (TPT SVF low-pass of
    // the channel mean, double state; 100 ms power averages).
    uint32_t holdShort = 0, holdLong = 0;
    bool lowFrequencyPresent = false;
    double lfA1 = 0.0, lfA2 = 0.0, lfA3 = 0.0, lfIc1 = 0.0, lfIc2 = 0.0;
    double lfPower = 0.0, fullPower = 0.0, lfPowerCoeff = 0.0;

    // programEnvelope: the slow program gain and its one-pole coefficients.
    double programGain = 1.0, programAttack = 0.0, programRelease = 0.0;

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
