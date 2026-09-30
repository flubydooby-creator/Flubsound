// Flubsound Pro - bass engine: adaptive boost + psychoacoustic harmonics.
//
// Signal flow (per block, all IIR; zero latency unless setLookaheadMs):
//   1. Subsonic high-pass (Butterworth 24 dB/oct at subsonicHz, or 12 dB/oct
//      with subsonicOrder 2; 0 = off) removes DC and inaudible rumble that
//      would waste limiter headroom. The 2nd order keeps about half the group
//      delay of the 4th (docs/11 E02 subsonic slice: at 40 Hz, HP4 at 25 Hz
//      8.2 ms, at 20 Hz 5.9 ms, HP2 at 20 Hz 3.3 ms); changing the order
//      crossfades the two over 20 ms (both run while the stage is on).
//   2. Mono-bass (stereo only): LR4 split at monoBelowHz; the low band is
//      replaced by its mid (L+R)/2 -> tight, centred low end, no phasey sub.
//   3. Adaptive low boost: low-shelf (Q 0.7) at boostFrequency (the shelf
//      corner = half-gain point) whose gain is boostDb minus a protection
//      term. A detector (LP at max(150 Hz, 1.5 x boostFrequency), linked,
//      10 ms / 150 ms envelope) predicts the boosted LF level; if
//      level + boost exceeds protectThresholdDb the boost is withdrawn by the
//      excess (soft knee 6 dB). This is what keeps "+12 dB bass" from turning
//      into limiter pumping on bass-heavy material and explosions.
//      Split-band protection (splitProtection, docs/11 E02 (a), off by
//      default): a sub detector (the LR4 low band at 85 Hz, 25 ms hold)
//      withdraws the shelf; what the classic prediction (with an 8 ms hold,
//      the punch detector) still exceeds after that is taken by a wide bell
//      in the 60-150 Hz punch band, at most as deep as the shelf's boost
//      there, so a punchy kick above 60 Hz no longer takes the sub boost with
//      it. Both withdrawals have a program-dependent release: once hits
//      recur (level onsets <= 1.2 s apart) a withdrawal is held for the
//      recent onset spacing + 1/8, so a sustained line under repeated kicks
//      keeps one steady gain instead of pumping in time with them (32 Hz
//      line under 55 Hz kicks, protection alone: 3.5-4.7 -> 0.0 dB of
//      modulation, at a steadily lower boost); an isolated hit releases as
//      before. Cap: <= 0.5 dB over it for any tone and shelf, as classic.
//      Look-ahead (setLookaheadMs, meant for the Quality profile at 2 ms;
//      the chain does not set it yet, docs/11 E02): the audio path from
//      the classic detector on is delayed; the split detectors read the
//      undelayed signal with a 1 ms attack and their withdrawals skip the
//      5 ms gain smoothing, so an onset meets its withdrawal in place
//      instead of 10 - 40 ms of overshoot (40 Hz at -6 dBFS into +12 dB,
//      cap 0 dBFS: onset peak +4.0 -> -0.4 dBFS). With splitProtection
//      off the engine only delays its output (up to the peak holds'
//      bucket grid, which the delay shifts).
//   3a. Impact's punch (docs/11 E20; the chain sets `punch` from Gaming
//      Impact, no parameter): an onset detector (TransientShaper's attack
//      indicator, low-band timing) on the 40 - 150 Hz LR4 band keys a burst
//      envelope (held 80 ms after the onset's peak, then a 60 ms release, so
//      80 - 300 ms long); the burst lifts the band by up to 6 dB x punch (a
//      77 Hz bell built as x + (g - 1) BP (x), exactly x at unity) and adds
//      the harmonics generator's output at up to 1 x punch while it lasts.
//      Steady rumble never reads as an onset, so it is not lifted. The lift
//      is headroom-reserved: it never takes the band's held peak over
//      protectThresholdDb (a loud explosion keeps only its harmonics); the
//      chain governs `punch` by the SafetyGovernor's scale. It sits before
//      the protection detector, so the shelf withdraws for it too.
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
//   5. Tighten: TransientShaper detecting the LR4 low band (< 150 Hz) with
//      negative sustain = shorter, drier bass decay ("punchy" rather than
//      "boomy"), gated by the onset so the first 30-50 ms of a note keep
//      their level, applied as a one-pole low shelf (unity = the input
//      itself; docs/11 E04 step 2).
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

#include "Crossover.h"
#include "EnvelopeFollower.h"
#include "ParallelDistortion.h"
#include "Processor.h"
#include "Svf.h"
#include "TransientShaper.h"
#include "flub/common/SmoothedValue.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

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
    int subsonicOrder = 4;             // 2 or 4 (12 / 24 dB per octave)
    bool splitProtection = false;      // sub / punch detectors with program-dependent release (docs/11 E02 (a))
    float punch = 0.0f;                // 0 .. 1: Impact's event-keyed LF burst (docs/11 E20; the chain's, no parameter)

    bool operator== (const BassEngineParams&) const = default;
};

class BassEngine final : public Processor
{
public:
    /** Structural: call before prepare(). Look-ahead of the split-band
        protection (docs/11 E02 (a), 0 .. 5 ms, default 0): the latency. */
    void setLookaheadMs (float ms) noexcept { lookaheadMs = std::isfinite (ms) ? std::clamp (ms, 0.0f, 5.0f) : 0.0f; }

    void prepare (const ProcessSpec& spec) override;
    void reset() noexcept FLUB_NONBLOCKING override;
    void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING override;
    int latencySamples() const noexcept override { return lookahead; }
    const char* name() const noexcept override { return "Bass Engine"; }

    void setParams (const BassEngineParams& p) noexcept FLUB_NONBLOCKING;
    const BassEngineParams& getParams() const noexcept { return params; }

    /** Boost currently withdrawn by the protection stage (dB >= 0), for the GUI. */
    float getProtectionDb() const noexcept { return protectionDb.load (std::memory_order_relaxed); }

    /** Harmonics generator: energy of the added harmonics relative to the
        output over the last 25 ms analysis window (dB; -160 = harmonics off
        or silent). See the header comment. */
    float getDistortionDb() const noexcept FLUB_NONBLOCKING { return distortionDb.load (std::memory_order_relaxed); }

    /** Impact's punch (docs/11 E20): the burst's lift of the 40 - 150 Hz
        band now (dB >= 0). Audio thread (tests, meters). */
    float getPunchDb() const noexcept FLUB_NONBLOCKING { return punchGainDb; }

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

    /** Split-band protection, one band (control rate except the detector):
        peak hold -> 10 / 150 ms follower, then a program-dependent hold on
        the band's withdrawal (see the header comment). */
    struct BandProtector
    {
        TransientShaper::PeakHold hold;
        EnvelopeFollower env;
        float held = 0.0f;       // held withdrawal, dB
        float valleyDb = -160.0f; // onset detector: the band level's recent low
        float releaseCoeff = 0.0f, valleyCoeff = 0.0f;
        int holdTicks = 0, countdown = 0, sinceOnset = 0, spacingLast = 0, spacingPrev = 0, maxTicks = 1;
        bool armed = true;

        void reset (float envLevel) noexcept;
        float update (float levelDb, float rawWithdrawDb) noexcept; // control rate
    };

    void updateTargets() noexcept;
    void updateSplitDesigns() noexcept;
    bool setStage (ParkedStage& stage, bool wanted, float hz) noexcept;
    bool tickStage (ParkedStage& stage, bool effectSettled) noexcept;
    void updateHarmonicFilters() noexcept;
    void updateHarmonicWeights() noexcept;
    void clearHarmonics() noexcept;
    void clearAllStates() noexcept;
    float flushStates() noexcept;
    void controlTick() noexcept;
    void splitDetect (const std::array<float, kMaxChannels>& x, int numCh, float lfPeak) noexcept;
    void startPunch() noexcept;
    float processPunch (std::array<float, kMaxChannels>& x, int numCh) noexcept;
    void processSegment (const AudioBlock& block, int numCh, int pos, int len) noexcept;

    ProcessSpec spec;
    BassEngineParams params;
    std::atomic<float> protectionDb { 0.0f };
    ParallelDistortionWindow distortionWindow; // harmonics telemetry: sums over a window of at least 25 ms (closes at a block boundary)
    std::atomic<float> distortionDb { -160.0f };

    double controlRate = 48000.0 / kControlInterval;
    int controlCountdown = kControlInterval;

    // 1. Subsonic high-pass (Butterworth, 2 sections; or 1 section, Q 0.707,
    // with subsonicOrder 2: orderBlend 0 = 4th order .. 1 = 2nd order).
    ParkedStage subsonic;
    std::array<SvfCoeffs, 2> subsonicHp {};
    SvfCoeffs subsonicHp2;
    std::array<Hp4State, kMaxChannels> subsonicState {};
    std::array<SvfState, kMaxChannels> subsonic2State {};
    LinearSmoothedValue orderBlend;

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

    // 3b. Split-band protection (params.splitProtection): detectors and the
    // 60-150 Hz bell. The detectors run only while it is on (they start
    // from the classic detector's level); the bell glides back to 0 dB.
    BandProtector subProtect, punchProtect;
    bool splitRunning = false;
    SvfCoeffs splitXo;                                 // the sub detector's LR4 (85 Hz)
    std::array<Lr4State, kMaxChannels> splitState {};
    double bellHz = 95.0;
    float bellShelfRatio = 0.0f;                       // the shelf's boost at bellHz per dB of boost
    OnePoleSmoother bellCutSmoothed;
    float bellCutDb = 0.0f;
    bool bellActive = false;
    TransientShaper::SvfGlide bell;
    std::array<SvfState, kMaxChannels> bellState {};

    // 3c. Look-ahead (setLookaheadMs): the audio delay after the split
    // detectors (planar, lookahead samples per channel) and the punch
    // detector's own low-pass on the undelayed signal.
    float lookaheadMs = 0.0f;
    int lookahead = 0, lookaheadPos = 0;
    std::vector<float> lookaheadBuf;
    std::array<SvfState, kMaxChannels> lookaheadLpState {};

    // 3a. Impact's punch (docs/11 E20): the detector band, its onset
    // indicator and held level, the burst envelope (instant rise, a hold,
    // then a one-pole release), the lift in dB (2 ms rise / 20 ms fall) and
    // the bell's unity band-pass. Runs while punch or its burst is non-zero.
    bool punchActive = false;
    LinkwitzRileyBand punchBand;
    TransientShaper punchDetector;
    TransientShaper::PeakHold punchLevel;
    LinearSmoothedValue punchAmount;      // per sample, 20 ms
    int punchWarm = 0;                    // samples before an onset may key a burst (warm-up)
    int punchHoldLeft = 0, punchHoldSamples = 1;
    float punchEnv = 0.0f, punchReleaseCoeff = 0.0f;
    float punchGainDb = 0.0f, punchRiseCoeff = 0.0f, punchFallCoeff = 0.0f;
    SvfCoeffs punchBell;
    std::array<SvfState, kMaxChannels> punchBellState {};

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

    // 5. Tighten: transient shaper (negative sustain) detecting the LR4 low
    // band, applied through a one-pole low-pass at the same corner.
    ParkedStage tight;
    SvfCoeffs tightXo;
    std::array<Lr4State, kMaxChannels> tightState {};
    std::array<float, kMaxChannels> tightLp {}; // TPT one-pole state
    TransientShaper tightShaper;
};
} // namespace flub
