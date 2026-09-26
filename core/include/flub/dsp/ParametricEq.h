// Flubsound Pro - fully parametric EQ (10 bands by default, up to 16).
//
// Each band is a cascade of up to four TPT SVF sections:
//   Bell / LowShelf / HighShelf / Notch / BandPass : 1 section
//   LowCut / HighCut : Butterworth, slope 12/24/36/48 dB/oct = 1..4 sections
// Continuous parameters (frequency, gain, Q) are smoothed (frequency in the
// log domain, ~20 ms) and coefficients are recomputed at a control rate of
// kControlInterval samples while smoothing. Discrete changes (type, slope,
// enable) are applied click-free by fading the band's wet/dry mix to 0 over
// ~5 ms, swapping the topology + resetting its state, then fading back in.
// Zero latency (minimum-phase IIR). A linear-phase variant is a roadmap
// item for offline/batch mastering only (it would add latency).
#pragma once

#include "Processor.h"
#include "Svf.h"
#include "flub/common/SmoothedValue.h"

#include <array>

namespace flub
{
enum class EqBandType : uint8_t
{
    Bell = 0,
    LowShelf,
    HighShelf,
    LowCut,   // high-pass
    HighCut,  // low-pass
    Notch,
    BandPass
};

struct EqBandParams
{
    bool enabled = false;
    EqBandType type = EqBandType::Bell;
    float frequency = 1000.0f; // Hz, 20 .. 20000
    float gainDb = 0.0f;       // dB, -24 .. +24 (Bell / shelves)
    float q = 0.7071f;         // 0.1 .. 18 (shelf "Q" = slope/resonance)
    int slopeDbPerOct = 12;    // 12 / 24 / 36 / 48 (LowCut / HighCut only)

    bool operator== (const EqBandParams&) const = default;
};

class ParametricEq final : public Processor
{
public:
    static constexpr int kMaxBands = 16;
    static constexpr int kDefaultBands = 10;
    static constexpr int kControlInterval = 16;

    void prepare (const ProcessSpec& spec) override;
    void reset() noexcept override;
    void process (const AudioBlock& block) noexcept override;
    const char* name() const noexcept override { return "Parametric EQ"; }

    /** RT-safe. Values are clamped to the documented ranges. */
    void setBand (int index, const EqBandParams& params) noexcept;
    const EqBandParams& getBand (int index) const noexcept;
    void setOutputGainDb (float db) noexcept;

    /** Exact magnitude (dB) of a set of bands at freqHz, for GUI curves.
        Pure function: safe on any thread. Ignores smoothing state. */
    static double responseDb (const EqBandParams* bands, int numBands, double freqHz, double sampleRate) noexcept;

private:
    // ---- implementation-defined below this line (owned by the .cpp author) ----
    ProcessSpec spec;
    std::array<EqBandParams, kMaxBands> targets {};
    LinearSmoothedValue outputGain;
};
} // namespace flub
