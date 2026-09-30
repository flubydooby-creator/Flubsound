// Flubsound Pro - clarity & transient-detail enhancer.
//
//   1. Transient shaper (linked): attackDb / sustainDb. Full band while
//      both band offsets are 0 (the shaper before docs/11 E04 step 3, bit-
//      exact); otherwise 3 bands (below lowSplitHz, 60 - 200 Hz, up to
//      4 kHz, above), each with its own TransientShaper timed for its band
//      (TransientShaper::Timing::lowBand / midBand / highBand) at attackDb +
//      attackLowDb / attackDb / attackDb + attackHighDb (each within
//      +-12 dB) and sustainDb, and transientSpeed on their timing. The
//      shapers read LR2 / LR4 bands; their gains apply to complementary
//      one-pole bands that sum to the input exactly (docs/11 E04 step 4),
//      so the path is the input at unity gains. It crossfades in over
//      50 ms once its shapers have warmed up, and out again when both
//      offsets return to 0. Optional look-ahead (setLookaheadMs, docs/11
//      E04 step 5): the latency.
//   2. De-mud: dynamic cut, bell ~250 Hz Q 1.0, CutAbove, range up to -4 dB
//      scaled by deMud, threshold tracks the broadband level (-12 dB rel.)
//      so it only acts when the low-mids are disproportionately loud.
//   3. Dynamic presence: bell at presenceFrequency (default 3.2 kHz, Q 0.8),
//      BoostBelow-style (inverse level): up to +6 dB * presence when the
//      band is quiet, withdrawn as it gets loud -> intelligibility without
//      harshness on loud passages. presenceMode Absolute (the default)
//      reads the band's level against a fixed -18 dB RMS; Relative
//      (docs/11 E07 step 3) reads it against the programme's own 200 Hz -
//      1 kHz body, so the lift does not depend on the playback or
//      mastering level, only on how bright the programme is.
//   4. Air exciter: band 3.5 - 7 kHz (24 dB/oct each side) -> envelope-
//      normalised polynomial of order <= 3 (2nd + 3rd harmonics) -> high-
//      pass 7 kHz -> mixed in at up to -12 dB * air, plus a +2 dB * air high
//      shelf at 10 kHz. Order <= 3 on content <= 7 kHz keeps every product
//      below 21 kHz, so no oversampling is needed at >= 44.1 kHz for content
//      inside the band; strong tones on the 24 dB/oct skirt (7.6-9 kHz) leave
//      small aliases (-25..-44 dB) folding to 17-21 kHz. The chain disables
//      air below 42 kHz sample rate.
// Zero latency, unless setLookaheadMs.
//
// Telemetry: getDistortionDb() = the share of the exciter's generated
// harmonics in its output over the last completed 25 ms analysis window
// (ParallelDistortion.h), taken where they are added (before the linear air
// shelf): per channel, the signal entering the exciter, the band signal
// through the same 7 kHz high-pass and mix (the shaper's linear branch) and
// the added harmonics; what the two linear references explain is not
// counted. -160 dB while air is off.
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
/** How the dynamic presence reads its band (docs/11 E07 step 3). */
enum class PresenceMode : int
{
    Absolute = 0, // band level against a fixed threshold (the law before E07 step 3)
    Relative = 1  // band level against the programme's 200 Hz - 1 kHz body
};

struct ClarityParams
{
    float attackDb = 0.0f;            // -12 .. +12
    float sustainDb = 0.0f;           // -12 .. +12
    float presence = 0.0f;            // 0 .. 1
    float presenceFrequency = 3200.0f;// 1000 .. 6000 Hz
    float air = 0.0f;                 // 0 .. 1
    float deMud = 0.0f;               // 0 .. 1
    PresenceMode presenceMode = PresenceMode::Absolute;
    // Stage 1's 3-band path (docs/11 E04 step 3). The offsets are the
    // clarity.attackLow / clarity.attackHigh parameters; the split and the
    // speed are module settings (no parameter yet).
    float attackLowDb = 0.0f;         // -12 .. +12, over attackDb below the split
    float attackHighDb = 0.0f;        // -12 .. +12, over attackDb above 4 kHz
    float lowSplitHz = 120.0f;        // 60 .. 200 Hz (25 ms log glide)
    float transientSpeed = 1.0f;      // 0.5 .. 2: the band shapers' slow attack and releases / speed

    bool operator== (const ClarityParams&) const = default;
};

class ClarityEnhancer final : public Processor
{
public:
    /** Structural: call before prepare(). Look-ahead of stage 1 (docs/11
        E04 step 5; 0 .. 5 ms, default 0, meant for the Quality profile):
        the shapers read the signal this much ahead of what they shape, so
        an onset's lift is in place when it arrives. It is the module's
        latency (its whole output is delayed by it). */
    void setLookaheadMs (float ms) noexcept { lookaheadMs = std::isfinite (ms) ? std::clamp (ms, 0.0f, 5.0f) : 0.0f; }

    void prepare (const ProcessSpec& spec) override;
    void reset() noexcept FLUB_NONBLOCKING override;
    void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING override;
    int latencySamples() const noexcept override { return lookahead; }
    const char* name() const noexcept override { return "Clarity"; }

    void setParams (const ClarityParams& p) noexcept FLUB_NONBLOCKING;
    const ClarityParams& getParams() const noexcept { return params; }

    /** Air exciter: energy of the added harmonics relative to the exciter's
        output over the last 25 ms analysis window (dB; -160 = air off or
        silent). See the header comment. */
    float getDistortionDb() const noexcept FLUB_NONBLOCKING { return distortionDb.load (std::memory_order_relaxed); }

    /** Stage 1 runs its 3-band path (fully or crossfading); false = the full-
        band shaper alone. Audio thread (tests). */
    bool isBandPathActive() const noexcept FLUB_NONBLOCKING { return bands.active; }

private:
    // ---- implementation-defined below this line ----
    static constexpr int kControlInterval = 16;

    /** Level-dependent bell (de-mud cut / presence boost): linked mean-square
        detector on a unity-gain band-pass, gain computer at control rate,
        EQ gain = amount * smoothed dynamic gain, gliding per sample. */
    struct DynamicBell
    {
        bool active = false;
        OnePoleSmoother amount;              // 0..1, control rate
        GainSmoother gain;                   // dynamic gain before the amount scaling (dB)
        float appliedDb = 0.0f;              // what the EQ is designed for
        float bandMs = 0.0f, broadMs = 0.0f; // linked mean squares (band / broadband)
        SvfCoeffs detector;
        std::array<SvfState, kMaxChannels> detectorState {}, eqState {};
        TransientShaper::SvfGlide eq;
    };

    /** Per-channel exciter state: HP4 3.5 kHz, LP4 7 kHz, HP4 7 kHz, HP4 7 kHz
        on the band (telemetry: the shaper's linear branch), and the
        envelopes of the band and of everything above 3.5 kHz. */
    struct AirChannel
    {
        std::array<SvfState, 8> filters {};
        TransientShaper::PeakHold bandHold, highHold;
        float bandRelease = 0.0f, highRelease = 0.0f, env = 0.0f;
    };

    /** Relative presence (docs/11 E07 step 3): the body band's detector,
        its fast mean square and the slow balance followers of the presence
        band and the body; running while Relative is selected or still
        mixed in. */
    struct PresenceBalance
    {
        bool running = false, fromNothing = false;
        int warmTicks = 0;                    // control ticks until the followers are valid
        OnePoleSmoother mix;                  // 0 = Absolute law .. 1 = Relative law, control rate
        float bodyMs = 0.0f;                  // fast (20 ms) linked mean square of the body
        float slowBandMs = 0.0f, slowBodyMs = 0.0f;
        std::array<std::array<SvfState, 2>, kMaxChannels> bodyState {};
    };

    /** Stage 1's 3-band path (docs/11 E04 step 3). While it warms up its
        shapers run on the bands but the output is still the full-band
        shaper's; then mix crossfades (per sample) to the bands' gains.
        The LR4 bands feed the shapers' detectors only; the gains apply to
        complementary one-pole bands of the signal (step 4), which sum to
        it exactly: low = LP1 at the split, high = HP1 at 4 kHz of the rest
        (x - low), mid what remains. */
    struct BandPath
    {
        bool active = false;
        int warmCountdown = 0;                  // samples until the crossfade may start
        LinearSmoothedValue mix;                // 0 = full-band shaper .. 1 = 3 bands
        ThreeBandSplitter splitter;
        std::array<TransientShaper, 3> shapers; // low, mid, high
        OnePoleSmoother logSplitHz;             // per sample while the path runs
        float splitHz = 120.0f;
        float lowG = 0.0f, highG = 0.0f;        // TPT one-pole g / (1 + g) at the split / 4 kHz
        std::array<float, kMaxChannels> lowState {}, highState {};
        std::array<float, kMaxChannels> lowDetState1 {}, lowDetState2 {}; // the low band's detector (LR2 at the split)
    };

    bool wantsBands() const noexcept { return params.attackLowDb != 0.0f || params.attackHighDb != 0.0f; }
    void setBandTargets() noexcept;
    void activateBands() noexcept;
    void processShaper (const AudioBlock& block, int numCh, int pos, int len) noexcept;
    void processBands (const AudioBlock& block, int numCh, int pos, int len) noexcept;
    void setApplySplit (double hz) noexcept;
    /** The look-ahead's delay of one frame (x in, x - lookahead out); a no-op without one. */
    void delayFrame (std::array<float, kMaxChannels>& x, int numCh) noexcept;
    void activateBell (DynamicBell& bell, double hz, double q) noexcept;
    void startBalance (bool fromNothing) noexcept;
    float updateBalance() noexcept;
    float relativePresenceDb() const noexcept;
    void processBody (const AudioBlock& block, int numCh, int pos, int len) noexcept;
    void updateBell (DynamicBell& bell, float gainDb, double hz, double q, bool moved) noexcept;
    void activateAir() noexcept;
    void processBell (DynamicBell& bell, const AudioBlock& block, int numCh, int pos, int len, int phase, bool trackBroadband) noexcept;
    void processAir (const AudioBlock& block, int numCh, int pos, int len, int phase) noexcept;
    void applyGlide (TransientShaper::SvfGlide& glide, std::array<SvfState, kMaxChannels>& state,
                     const AudioBlock& block, int numCh, int pos, int len, int phase) noexcept;
    void controlTick() noexcept;
    void clearAllStates() noexcept;
    float flushStates() noexcept;

    ProcessSpec spec;
    ClarityParams params;

    double controlRate = 48000.0 / kControlInterval;
    int controlCountdown = kControlInterval;
    float msCoeff = 0.0f; // mean-square detector one-pole

    // 1. Transient shaper (full band, linked) and the 3-band path, and
    // their look-ahead (setLookaheadMs): the delay of the shaped signal
    // (planar, lookahead samples per channel).
    TransientShaper shaper;
    BandPath bands;
    float lookaheadMs = 0.0f;
    int lookahead = 0, lookaheadPos = 0;
    std::vector<float> lookaheadBuf;

    // 2. De-mud, 3. dynamic presence.
    DynamicBell deMud, presence;
    OnePoleSmoother logPresenceHz;
    float presenceHz = 3200.0f;
    PresenceBalance balance;
    std::array<SvfCoeffs, 2> bodyFilters {}; // HP2 200 Hz, LP2 1 kHz
    float balanceCoeff = 0.0f;               // slow followers' one-pole per control tick

    // 4. Air exciter + high shelf.
    bool airActive = false;
    OnePoleSmoother airAmount;  // control rate: shelf gain = 2 dB * air
    LinearSmoothedValue airMix; // per sample: exciter mix = air * -12 dB
    std::array<SvfCoeffs, 6> airFilters {};
    std::array<AirChannel, kMaxChannels> airChannels {};
    float airReleaseCoeff = 0.0f, airSmoothCoeff = 0.0f;
    TransientShaper::SvfGlide airShelf;
    float airShelfDb = 0.0f;
    std::array<SvfState, kMaxChannels> airShelfState {};
    ParallelDistortionWindow distortionWindow; // exciter telemetry: sums over a window of at least 25 ms (closes at a block boundary)
    std::atomic<float> distortionDb { -160.0f };

    // Per-segment scratch (a segment never exceeds one control interval).
    std::array<SvfCoeffs, kControlInterval> rampScratch {};
    std::array<float, kControlInterval> scratchA {}, scratchB {};
};
} // namespace flub
