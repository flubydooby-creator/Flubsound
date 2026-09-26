// Flubsound Pro - oversampled saturation (tape / tube / digital).
//
// Unity small-signal gain by construction: y = f(g x) / g, where g is the
// drive gain and f has f'(0) = 1, so drive changes the *character* and peak
// behaviour rather than the loudness of quiet material.
//   Tape    : pre-emphasis (+6 dB high shelf @ 3 kHz) -> f = tanh ->
//             de-emphasis (-6 dB) : HF saturates earlier, like tape; plus a
//             gentle +1 dB "head bump" bell at 80 Hz scaled by drive.
//   Tube    : asymmetric f(x) = (tanh(x + b) - tanh(b)) / (1 - tanh^2 b),
//             b = 0.2 -> even harmonics; DC removed by a 10 Hz high-pass.
//   Digital : cubic soft clip f(x) = x - (4/27) x^3 for |x| < 1.5, +-1 beyond
//             (f'(0) = 1, f(1.5) = 1, f'(1.5) = 0: odd harmonics, hard-ish).
// Runs at 2x (default) or 4x via Oversampler; latency = oversampler latency.
// Dry/wet mix is latency-aligned internally (the dry path is delayed).
#pragma once

#include "Oversampler.h"
#include "Processor.h"

#include <cstdint>

namespace flub
{
enum class SaturationType : uint8_t
{
    Tape = 0,
    Tube,
    Digital
};

struct SaturatorParams
{
    SaturationType type = SaturationType::Tape;
    float driveDb = 0.0f;  // 0 .. 24
    float mix = 1.0f;      // 0 .. 1
    float outputDb = 0.0f; // -12 .. +12

    bool operator== (const SaturatorParams&) const = default;
};

class Saturator final : public Processor
{
public:
    /** Structural: call before prepare(). factor 1, 2 or 4. */
    void setOversampling (int factor, Oversampler::Quality q = Oversampler::Quality::High) noexcept
    {
        osFactor = factor;
        osQuality = q;
    }

    void prepare (const ProcessSpec& spec) override;
    void reset() noexcept override;
    void process (const AudioBlock& block) noexcept override;
    int latencySamples() const noexcept override;
    const char* name() const noexcept override { return "Saturation"; }

    void setParams (const SaturatorParams& p) noexcept;
    const SaturatorParams& getParams() const noexcept { return params; }

    /** The static curve, exposed for tests and the GUI transfer plot. */
    static float shape (SaturationType type, float x) noexcept;

private:
    // ---- implementation-defined below this line ----
    int osFactor = 2;
    Oversampler::Quality osQuality = Oversampler::Quality::High;
    ProcessSpec spec;
    SaturatorParams params;
};
} // namespace flub
