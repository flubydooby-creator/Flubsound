// Flubsound Pro - clarity & transient-detail enhancer.
//
//   1. Transient shaper (full band, linked): attackDb / sustainDb.
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
// Zero latency.
//
// Telemetry: getDistortionDb() = the share of the exciter's generated
// harmonics in its output over the last completed 25 ms analysis window
// (ParallelDistortion.h), taken where they are added (before the linear air
// shelf): per channel, the signal entering the exciter, the band signal
// through the same 7 kHz high-pass and mix (the shaper's linear branch) and
// the added harmonics; what the two linear references explain is not
// counted. -160 dB while air is off.
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

    bool operator== (const ClarityParams&) const = default;
};

class ClarityEnhancer final : public Processor
{
public:
    void prepare (const ProcessSpec& spec) override;
    void reset() noexcept FLUB_NONBLOCKING override;
    void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING override;
    const char* name() const noexcept override { return "Clarity"; }

    void setParams (const ClarityParams& p) noexcept FLUB_NONBLOCKING;
    const ClarityParams& getParams() const noexcept { return params; }

    /** Air exciter: energy of the added harmonics relative to the exciter's
        output over the last 25 ms analysis window (dB; -160 = air off or
        silent). See the header comment. */
    float getDistortionDb() const noexcept FLUB_NONBLOCKING { return distortionDb.load (std::memory_order_relaxed); }

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

    // 1. Transient shaper (full band, linked).
    TransientShaper shaper;

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
