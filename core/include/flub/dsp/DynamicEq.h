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

#include "ParametricEq.h"
#include "Processor.h"

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
    ProcessSpec spec;
    std::array<DynEqBandParams, kMaxBands> targets {};
    std::array<std::atomic<float>, kMaxBands> appliedGainDb {};
};
} // namespace flub
