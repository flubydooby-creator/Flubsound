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
    static constexpr int kMaxSections = 4; // 48 dB/oct = 4 x 12 dB/oct SVF sections

    /** The discrete part of a band. Changing it cannot be smoothed, so it is
        crossfaded: wet mix -> 0, swap + clear state, wet mix -> 1. */
    struct Topology
    {
        bool enabled = false;
        EqBandType type = EqBandType::Bell;
        int numSections = 1; // 1 except for LowCut / HighCut

        bool operator== (const Topology&) const = default;
    };

    struct Band
    {
        Topology running;                     // what the filter currently implements
        OnePoleSmoother logFreq, gainDb, logQ; // control-rate smoothers: log2 Hz, dB, log2 Q
        std::array<SvfCoeffs, kMaxSections> coeffs {};     // designed at the latest tick (steady-state set)
        std::array<SvfCoeffs, kMaxSections> prevCoeffs {}; // designed at the tick before (ramp start)
        std::array<std::array<SvfState, kMaxSections>, kMaxChannels> state {};
        int fadePos = 0;            // wet mix = fadePos / fadeSamples (exact 0 and 1 at the ends)
        int fadeDir = 0;            // +1 fading in, -1 fading out, 0 settled
        int rampPos = 0;            // samples into the current coefficient ramp
        bool ramping = false;       // interpolate prevCoeffs -> coeffs across this control period
        bool audible = false;       // run the filter during this control period
        bool stateValid = true;     // false: clear (or prime) the state before the next sample
        bool primeOnStart = false;  // prime the LP integrator with the input when clearing
        bool coeffsDirty = true;    // smoothed values moved since the last design
        bool busy = false;          // needs control ticks (smoothing, ramping, fading or pending swap)
    };

    static Topology topologyOf (const EqBandParams& params) noexcept;
    static bool isTransparent (const Band& band) noexcept;
    void controlTick() noexcept;
    void updateBand (Band& band, const EqBandParams& target) noexcept;
    void snapBand (Band& band, const EqBandParams& target) noexcept;
    void swapTopology (Band& band, const Topology& wanted) noexcept;
    void refreshCoefficients (Band& band, const EqBandParams& target, bool ramp) const noexcept;
    void processBand (Band& band, const AudioBlock& block, int start, int length, int numChannels) noexcept;
    void applyOutputGain (const AudioBlock& block, int numChannels) noexcept;

    ProcessSpec spec;
    std::array<EqBandParams, kMaxBands> targets {};
    std::array<Band, kMaxBands> bandDsp {};
    LinearSmoothedValue outputGain;
    float outputGainDb = 0.0f;
    int fadeSamples = 240;           // ~5 ms, a whole number of control periods
    float invFadeSamples = 1.0f / 240.0f;
    int samplesToTick = 0;           // samples until the next control tick (0 = due now)
    bool anyBusy = false;            // false: every control tick would be a no-op
};
} // namespace flub
