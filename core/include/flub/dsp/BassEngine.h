// Flubsound Pro - bass engine: adaptive boost + psychoacoustic harmonics.
//
// Signal flow (per block, zero latency, all IIR):
//   1. Subsonic high-pass (Butterworth 24 dB/oct at subsonicHz; 0 = off)
//      removes DC and inaudible rumble that would waste limiter headroom.
//   2. Mono-bass (stereo only): LR4 split at monoBelowHz; the low band is
//      replaced by its mid (L+R)/2 -> tight, centred low end, no phasey sub.
//   3. Adaptive low boost: low-shelf (Q 0.7) at boostFrequency (the shelf
//      corner = half-gain point) whose gain is boostDb minus a protection
//      term. A detector (LP at max(150 Hz, 1.5 x boostFrequency), linked,
//      10 ms / 150 ms envelope) predicts the boosted LF level; if
//      level + boost exceeds protectThresholdDb the boost is withdrawn by the
//      excess (soft knee 6 dB). This is what keeps "+12 dB bass" from turning
//      into limiter pumping on bass-heavy material and explosions.
//   4. Psychoacoustic bass ("missing fundamental"): the mid signal is band
//      limited to [~25 Hz, harmonicsCutoff]; an amplitude-normalised
//      Chebyshev waveshaper generates exact harmonics of a sinusoid:
//        xn = clamp(x / env, -1, 1)
//        h  = w2 T2(xn) + w3 T3(xn) + w4 T4(xn) + w5 T5(xn)
//        T2 = 2x^2-1, T3 = 4x^3-3x, T4 = 8x^4-8x^2+1, T5 = 16x^5-20x^3+5x
//        y  = h * env  (harmonic level tracks the fundamental linearly)
//      weights morph from even/warm (w2=1, w3=.35, w4=.3, w5=.1) to
//      odd/punchy (w2=.35, w3=1, w4=.1, w5=.3) with harmonicsCharacter.
//      The harmonics are band-passed to [cutoff, 6*cutoff] and added equally
//      to all channels. With replaceFundamental (small speakers / laptops)
//      the original content below the cutoff is high-passed away, reclaiming
//      headroom the transducer cannot use anyway.
//   5. Tighten: TransientShaper on the LR4 low band (< 150 Hz) with negative
//      sustain = shorter, drier bass decay ("punchy" rather than "boomy").
//
// Telemetry: getDistortionDb() = the share of the generated harmonics in the
// stage output over the last completed 25 ms analysis window
// (ParallelDistortion.h), taken where the harmonics are added: per channel,
// the dry path at that point (after the shelf and the optional
// replace-fundamental high-pass), the shaper's input through the same band
// pass and mix (its linear branch) and the added harmonics. What the two
// linear references explain is not counted, so neither path's filters are,
// only what the waveshaper generates. -160 dB while the harmonics are off.
#pragma once

#include "EnvelopeFollower.h"
#include "ParallelDistortion.h"
#include "Processor.h"
#include "Svf.h"
#include "TransientShaper.h"
#include "flub/common/SmoothedValue.h"

#include <array>
#include <atomic>

namespace flub
{
struct BassEngineParams
{
    float boostDb = 0.0f;              // 0 .. 15
    float boostFrequency = 70.0f;      // 30 .. 200 Hz
    float protectThresholdDb = -12.0f; // -30 .. 0 dBFS predicted LF level cap
    float harmonicsAmount = 0.0f;      // 0 .. 1 (mix of generated harmonics, up to +6 dB)
    float harmonicsCutoff = 120.0f;    // 40 .. 250 Hz (device low-frequency limit)
    float harmonicsCharacter = 0.5f;   // 0 = even/warm .. 1 = odd/punchy
    bool replaceFundamental = false;   // small-speaker mode
    float tighten = 0.0f;              // 0 .. 1 -> 0 .. -12 dB low-band sustain
    float monoBelowHz = 0.0f;          // 0 = off, else 40 .. 250 Hz
    float subsonicHz = 20.0f;          // 0 = off, else 10 .. 40 Hz

    bool operator== (const BassEngineParams&) const = default;
};

class BassEngine final : public Processor
{
public:
    void prepare (const ProcessSpec& spec) override;
    void reset() noexcept FLUB_NONBLOCKING override;
    void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING override;
    const char* name() const noexcept override { return "Bass Engine"; }

    void setParams (const BassEngineParams& p) noexcept FLUB_NONBLOCKING;
    const BassEngineParams& getParams() const noexcept { return params; }

    /** Boost currently withdrawn by the protection stage (dB >= 0), for the GUI. */
    float getProtectionDb() const noexcept { return protectionDb.load (std::memory_order_relaxed); }

    /** Harmonics generator: energy of the added harmonics relative to the
        output over the last 25 ms analysis window (dB; -160 = harmonics off
        or silent). See the header comment. */
    float getDistortionDb() const noexcept FLUB_NONBLOCKING { return distortionDb.load (std::memory_order_relaxed); }

private:
    // ---- implementation-defined below this line ----
    static constexpr int kControlInterval = 16;

    /** While a corner frequency glides, the SVF prewarped frequency g moves
        linearly per sample from the previous control-rate design to the new
        one (a1..a3 re-derived per sample), so a sweep has no 16-sample steps. */
    struct GGlide
    {
        void start (double fromG, double toG) noexcept
        {
            from = static_cast<float> (fromG);
            delta = static_cast<float> (toG - fromG);
            active = delta != 0.0f;
        }

        float at (float t) const noexcept { return from + delta * t; }

        float from = 0.0f, delta = 0.0f;
        bool active = false;
    };

    /** A filter stage that can be switched in and out without a click or a
        momentary notch. Crossfading dry against a filtered path directly would
        cancel around the corner (an LR4 sum is -180 degrees there), so a stage
        is engaged by a short crossfade while its corner is parked at a
        subsonic frequency, and only then glides to its target; disengaging
        glides back to the park frequency first and crossfades out there. */
    struct ParkedStage
    {
        bool wanted = false, active = false;
        float targetHz = 100.0f, parkHz = 10.0f, hz = 10.0f;
        float logTarget = 0.0f, logPark = 0.0f;
        OnePoleSmoother logHz;     // control rate, log-frequency glide
        LinearSmoothedValue blend; // per sample: 0 = dry .. 1 = processed
        GGlide glide;              // per-sample g between two control-rate designs
    };

    using Lr4State = std::array<SvfState, 3>; // split section, low section, high section
    using Hp4State = std::array<SvfState, 2>;

    void updateTargets() noexcept;
    bool setStage (ParkedStage& stage, bool wanted, float hz) noexcept;
    bool tickStage (ParkedStage& stage, bool effectSettled) noexcept;
    void updateHarmonicFilters() noexcept;
    void updateHarmonicWeights() noexcept;
    void clearHarmonics() noexcept;
    void clearAllStates() noexcept;
    float flushStates() noexcept;
    void controlTick() noexcept;
    void processSegment (const AudioBlock& block, int numCh, int pos, int len) noexcept;

    ProcessSpec spec;
    BassEngineParams params;
    std::atomic<float> protectionDb { 0.0f };
    ParallelDistortionWindow distortionWindow; // harmonics telemetry: sums over a 25 ms window
    std::atomic<float> distortionDb { -160.0f };

    double controlRate = 48000.0 / kControlInterval;
    int controlCountdown = kControlInterval;

    // 1. Subsonic high-pass (Butterworth, 2 sections).
    ParkedStage subsonic;
    std::array<SvfCoeffs, 2> subsonicHp {};
    std::array<Hp4State, kMaxChannels> subsonicState {};

    // 2. Mono bass: LR4 built from one Butterworth LP design (raw SVF outputs).
    ParkedStage mono;
    SvfCoeffs monoXo;
    std::array<Lr4State, kMaxChannels> monoState {};

    // 3. Adaptive low shelf + headroom protection.
    OnePoleSmoother boostSmoothed, logBoostHz, thresholdSmoothed, shelfGainSmoothed;
    OnePoleSmoother withdrawSmoothed; // telemetry only (getProtectionDb)
    float boostHz = 70.0f, shelfGainDb = 0.0f;
    bool shelfActive = false;
    TransientShaper::SvfGlide shelf;
    std::array<SvfState, kMaxChannels> shelfState {};
    SvfCoeffs detectorLp;
    std::array<SvfState, kMaxChannels> detectorState {};
    TransientShaper::PeakHold detectorHold;
    EnvelopeFollower detectorEnv;

    // 4. Psychoacoustic harmonics (mid signal) + replace-fundamental high-pass.
    bool harmonicsActive = false;
    LinearSmoothedValue harmonicsMix; // linear gain: amount * 2 (+6 dB)
    OnePoleSmoother characterSmoothed, logCutoff;
    float cutoffHz = 120.0f;
    std::array<float, 4> weights {}; // w2 .. w5
    SvfCoeffs harmPreHp, harmPostHp;
    std::array<SvfCoeffs, 2> harmPreLp {}, harmPostLp {};
    // preHp, preLp[0], preLp[1], postHp, postLp[0], postLp[1], then postHp,
    // postLp[0], postLp[1] once more on the shaper's input (telemetry: its linear branch)
    std::array<SvfState, 9> harmState {};
    GGlide cutoffGlide, upperGlide;       // g of the cutoff and 6 * cutoff designs
    TransientShaper::PeakHold harmHold;
    EnvelopeFollower harmEnv;

    ParkedStage replace;
    std::array<SvfCoeffs, 2> replaceHp {};
    std::array<Hp4State, kMaxChannels> replaceState {};

    // 5. Tighten: transient shaper (negative sustain) on the LR4 low band.
    ParkedStage tight;
    SvfCoeffs tightXo;
    std::array<Lr4State, kMaxChannels> tightState {};
    TransientShaper tightShaper;
};
} // namespace flub
