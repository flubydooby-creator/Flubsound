// Flubsound Pro - multi-band dynamic EQ.
//
// Each band = a sidechain detector filter + a modulated EQ section whose gain
// is driven by a gain computer on the detector level:
//
//   detector : SVF band-pass (Bell) / low-pass (LowShelf) / high-pass
//              (HighShelf) at the band frequency/Q, unity gain
//   level    : peak envelope in dB, STEREO-LINKED (max over channels) so that
//              gain changes never shift the stereo/positional image
//   computer : overshoot o = level - threshold (dB)
//     CutAbove   (downward compression) : g = -min(range, max(0, o)(1 - 1/ratio))
//     BoostBelow (upward compression)   : g = +min(range, max(0,-o)(1 - 1/ratio)) * floorTaper
//     BoostAbove (upward expansion)     : g = +min(range, max(0, o)(ratio - 1))
//     CutBelow   (downward expansion)   : g = -min(range, max(0,-o)(ratio - 1))
//     floorTaper fades the boost to 0 as level falls from noiseFloorDb+10 to
//     noiseFloorDb, so silence / hiss is never lifted.
//     CueLift    (cue enhancer, docs/11 E19): g = +range x onset x cap x floorTaper,
//                keyed to the band's own BACKGROUND instead of a fixed threshold
//                (see "CueLift" below); threshold and ratio are not used.
//   smoothing: GainSmoother (attack = level rising), control-rate coefficient
//              update every kControlInterval samples (the SVF is modulation-safe).
//
// CueLift (the footstep / cue enhancer): a short cue under a steady bed
// must get its lift from its first milliseconds, and the bed must not be
// lifted at all. The band tracks
//   level      the detector's mean square over a few ms (2.5 ms, or four
//              periods of the band frequency if longer), stereo-linked (max),
//              in dB;
//   background a slow estimate of that level: it rises at most 5 dB/s and
//              falls with a 400 ms time constant, so it sits near the bed's
//              median and a 20-80 ms step barely moves it; never below the
//              hiss floor (out of digital silence it IS the floor); for
//              300 ms after a reset it follows the level (40 ms) to learn it;
// and lifts only what stands out of the background: onset = 0 at 2 dB
// over it, 1 at 4.5 dB over it, held for 30 ms after the level drops, then
// released. A cue therefore gets the full range within about a millisecond
// of rising out of the bed, whatever the programme level, and a stationary
// bed (rain, wind, room tone) is not raised. The lift is withdrawn fast
// (0.5 ms) by the loud cap - the band's PEAK 26..36 dB over the background
// (or over -55 dBFS, if that is higher), and -6..-1 dBFS absolute - so
// gunfire and explosions are not lifted.
//
// Upstream gain (docs/11 E19 step 4): the chain runs AutoLevel ahead of this
// module and hands its gain over every block (setReferenceOffsetDb). The
// background and the loud cap's relative term are kept in the terms of the
// level BEFORE that gain (level - offset), so AutoLevel's slow moves shift
// them with the programme instead of reading as the programme rising out of,
// or sinking into, its own background. The hiss floor stays where it is in
// the band (the background never sits under noiseFloorDb there), and the
// gain computer's floor taper and the absolute roll-off read the level as it
// is. An offset of 0 dB (the default; AutoLevel off) changes nothing.
//
// Onset flux (optional, DynEqBandParams::cueOnsetFlux; off by default): the
// gate also asks HOW FAST the level rose out of the background - a per-band
// flux. An event is lifted only if the level went from the gate's start
// (2 dB over the background) to its top (4.5 dB over it) within 15 ms (a
// rise of at least about 170 dB/s; a 20-80 ms step takes 2-6 ms). The rise
// is timed from a start crossed after the level sat under it for 30 ms, so
// the dips of a noisy band (a step's own, or a swell's) neither restart the
// timer nor end the event, which stays an onset until the level has been
// back under the start for 30 ms. A slow swell (a gust, an approaching engine)
// that climbs out of its background more slowly than that is not lifted;
// during those first 15 ms it gets the ordinary gate's lift (so a real
// onset loses nothing), then none.
//
// The chain's mode policy drives bands 4-7 (ProcessingChain::configureModeBands):
// Gaming: footstep detail (CueLift bell 3.2 kHz) and footstep body (CueLift
// bell 260 Hz), explosion anti-masking (off in the mode policy since docs/11
// E20's decoupling: the presets that tame loud LF carry it as a user band),
// voice / score (BoostBelow bell 2 kHz). Music: de-harsh (CutAbove bell
// 3.5 kHz), air lift (BoostBelow high shelf 12 kHz), de-boom (CutAbove bell
// 120 Hz).
#pragma once

#include "EnvelopeFollower.h"
#include "ParametricEq.h"
#include "Processor.h"
#include "Svf.h"
#include "flub/common/SmoothedValue.h"

#include <array>
#include <atomic>

namespace flub
{
enum class DynEqMode : uint8_t
{
    CutAbove = 0,
    BoostBelow,
    BoostAbove,
    CutBelow,
    CueLift // background-relative cue enhancer (mode bands only; see above)
};

struct DynEqBandParams
{
    bool enabled = false;
    DynEqMode mode = DynEqMode::CutAbove;
    EqBandType shape = EqBandType::Bell; // Bell, LowShelf or HighShelf only
    float frequency = 1000.0f;           // Hz
    float q = 1.0f;                      // 0.1 .. 10
    float thresholdDb = -24.0f;          // -80 .. 0 dBFS (detector level)
    float ratio = 2.0f;                  // 1 .. 20
    float rangeDb = 6.0f;                // 0 .. 24 : max magnitude of the dynamic gain
    float staticGainDb = 0.0f;           // -12 .. +12 : fixed gain added to the dynamic gain
    float attackMs = 5.0f;               // 0.1 .. 200
    float releaseMs = 80.0f;             // 5 .. 2000
    float noiseFloorDb = -70.0f;         // BoostBelow: no lift below this level
    bool cueOnsetFlux = false;           // CueLift: the gate is also keyed on onset flux (see above)

    bool operator== (const DynEqBandParams&) const = default;
};

class DynamicEq final : public Processor
{
public:
    static constexpr int kMaxBands = 8;
    static constexpr int kDefaultBands = 4;
    static constexpr int kControlInterval = 16;

    void prepare (const ProcessSpec& spec) override;
    void reset() noexcept FLUB_NONBLOCKING override;
    void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING override;
    const char* name() const noexcept override { return "Dynamic EQ"; }

    void setBand (int index, const DynEqBandParams& params) noexcept FLUB_NONBLOCKING;
    const DynEqBandParams& getBand (int index) const noexcept;

    /** Currently applied total gain of a band in dB (static + dynamic), for the
        GUI. Written by the audio thread (relaxed atomic), read by any thread. */
    float getBandGainDb (int index) const noexcept;

    /** The broadband gain (dB) applied upstream of this module that the
        CueLift backgrounds and loud cap must not see (AutoLevel's; docs/11
        E19 step 4, see above). Called once per block; non-finite reads 0. */
    void setReferenceOffsetDb (float db) noexcept FLUB_NONBLOCKING;

private:
    // ---- implementation-defined below this line (owned by the .cpp author) ----
    struct BandState
    {
        bool active = false;                  // being processed (enabled, or fading out / re-typing)
        DynEqMode mode = DynEqMode::CutAbove; // mode / shape currently running (targets may differ
        EqBandType shape = EqBandType::Bell;  // while a click-free swap is in progress)

        // Control-rate smoothing (one tick = kControlInterval samples).
        LinearSmoothedValue fade;             // 0..1, multiplies the total gain in dB
        OnePoleSmoother logFreq, logQ, thresholdDb, ratio, rangeDb, staticGainDb, noiseFloorDb;
        GainSmoother dynGain;                 // dynamic gain (dB), attack = detector level rising
        float freq = 1000.0f, q = 1.0f;       // current (smoothed) geometry

        // Sidechain detector (reads the module's dry input) and EQ section.
        SvfCoeffs detCoeffs, eqCoeffs;
        std::array<SvfState, kMaxChannels> detState {}, eqState {};
        // Last 5 detector outputs per channel (newest first), for the
        // inter-sample (midpoint) peak estimate.
        std::array<std::array<float, 5>, kMaxChannels> detHistory {};

        // After a coefficient update the EQ glides from the previous set to
        // eqCoeffs across the next control interval: (g, k, m0, m1, m2) are
        // interpolated per sample and a1..a3 re-derived, so every intermediate
        // filter is a valid, stable SVF and there is no control-rate zipper.
        struct EqRampPoint
        {
            float g = 0.0f, k = 0.0f, m0 = 0.0f, m1 = 0.0f, m2 = 0.0f;
        };
        EqRampPoint rampStart, rampDelta;
        bool eqRamping = false;

        // Linked peak envelope: max |detector| over channels (samples and
        // interpolated inter-sample midpoints), held over a sliding
        // window (two alternating buckets) so a steady tone reads its true peak
        // without ripple, then released exponentially.
        float segmentPeak = 0.0f, windowPeak = 0.0f, prevWindowPeak = 0.0f, env = 0.0f;
        float envRelease = 0.0f;
        int windowTicks = 1, windowCountdown = 1;

        float coeffGainDb = 0.0f;             // gain the EQ coefficients were built for

        // CueLift (see the header comment). The detector's energy is summed
        // per channel over each control interval for every band (cheap), but
        // only a CueLift band reads it.
        std::array<float, kMaxChannels> segmentEnergy {};
        float cuePower = 0.0f;                // linked mean square, smoothed
        float cuePowerCoeff = 0.0f;           // per-tick one-pole coefficient
        float cueBackgroundDb = 0.0f;
        bool cueBackgroundValid = false;      // false: the next tick starts it at the level
        float cueFallCoeff = 0.0f, cueRiseDbPerTick = 0.0f;
        float cueLearnCoeff = 0.0f;
        int cueLearnTicks = 0, cueLearnCountdown = 0;
        float cueHeld = 0.0f;                 // onset target after the hold (dB)
        int cueHoldTicks = 1, cueHoldCountdown = 0;
        float cueCap = 1.0f;                  // loud-cap factor, smoothed
        float cueCapDownCoeff = 0.0f, cueCapUpCoeff = 0.0f;
        // Onset flux (cueOnsetFlux): ticks since the rise started and the
        // limit, ticks the level has sat under the gate's start and how many
        // re-arm the timer, and whether this event rose fast enough.
        int cueRiseTicks = 0, cueFluxTicks = 1, cueQuietTicks = 0, cueArmTicks = 1;
        bool cueOnset = false;
    };

    void activateBand (int index, bool fadeIn) noexcept;
    void updateDetector (BandState& band) const noexcept;
    void updateEq (BandState& band, float totalDb, bool glide) const noexcept;
    static void clearBandState (BandState& band) noexcept;
    void controlTick (int index) noexcept;
    float cueTargetDb (BandState& band, float rangeDb, float noiseFloorDb, bool onsetFlux) const noexcept;

    ProcessSpec spec;
    double controlRate = 48000.0 / kControlInterval;
    int controlCountdown = kControlInterval;
    int lastNumChannels = 0; // channels in the previous block (states of absent channels go stale)
    float referenceOffsetDb = 0.0f; // upstream gain the CueLift backgrounds do not see (setReferenceOffsetDb)
    std::array<DynEqBandParams, kMaxBands> targets {};
    std::array<BandState, kMaxBands> bands {};
    std::array<SvfCoeffs, kControlInterval> rampScratch {}; // per-sample EQ coefficients while gliding
    std::array<std::atomic<float>, kMaxBands> appliedGainDb {};
};
} // namespace flub
