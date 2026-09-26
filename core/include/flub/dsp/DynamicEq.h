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
//   smoothing: GainSmoother (attack = level rising), control-rate coefficient
//              update every kControlInterval samples (the SVF is modulation-safe).
//
// Gaming uses: footstep lift (BoostBelow ~3 kHz), explosion-masking control
// (CutAbove ~80 Hz), voice/score presence (BoostBelow ~1.8 kHz).
// Music uses: de-harsh (CutAbove ~3.5 kHz), de-boom (CutAbove ~120 Hz),
// air lift (BoostBelow high shelf 10 kHz).
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
    CutBelow
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

    bool operator== (const DynEqBandParams&) const = default;
};

class DynamicEq final : public Processor
{
public:
    static constexpr int kMaxBands = 8;
    static constexpr int kDefaultBands = 4;
    static constexpr int kControlInterval = 16;

    void prepare (const ProcessSpec& spec) override;
    void reset() noexcept override;
    void process (const AudioBlock& block) noexcept override;
    const char* name() const noexcept override { return "Dynamic EQ"; }

    void setBand (int index, const DynEqBandParams& params) noexcept;
    const DynEqBandParams& getBand (int index) const noexcept;

    /** Currently applied total gain of a band in dB (static + dynamic), for the
        GUI. Written by the audio thread (relaxed atomic), read by any thread. */
    float getBandGainDb (int index) const noexcept;

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
    };

    void activateBand (int index, bool fadeIn) noexcept;
    void updateDetector (BandState& band) const noexcept;
    void updateEq (BandState& band, float totalDb, bool glide) const noexcept;
    static void clearBandState (BandState& band) noexcept;
    void controlTick (int index) noexcept;

    ProcessSpec spec;
    double controlRate = 48000.0 / kControlInterval;
    int controlCountdown = kControlInterval;
    int lastNumChannels = 0; // channels in the previous block (states of absent channels go stale)
    std::array<DynEqBandParams, kMaxBands> targets {};
    std::array<BandState, kMaxBands> bands {};
    std::array<SvfCoeffs, kControlInterval> rampScratch {}; // per-sample EQ coefficients while gliding
    std::array<std::atomic<float>, kMaxBands> appliedGainDb {};
};
} // namespace flub
