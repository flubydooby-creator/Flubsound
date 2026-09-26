// Flubsound Pro - oversampled saturation (tape / tube / digital).
//
// Unity small-signal gain by construction: y = f(g x) / g, where g is the
// drive gain and f has f'(0) = 1, so drive changes the *character* and peak
// behaviour rather than the loudness of quiet material. The curve is blended
// in with depth = smoothstep(0, 6 dB, drive), so 0 dB drive is exactly
// transparent. Loud material is compressed (lower RMS): there is no automatic
// wet make-up; outputDb is a wet-path gain.
// Delta oversampling: only f(x^) - x^ passes the half-band downsampler and is
// added to the exactly delayed input, so the top octave does not droop.
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
#include "Svf.h"
#include "flub/common/DelayLine.h"
#include "flub/common/SmoothedValue.h"

#include <array>
#include <cstdint>
#include <vector>

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
    // Notes (details in Saturator.cpp):
    //  * The curve is blended in with depth = smoothstep(0, 6 dB, drive), so
    //    0 dB drive is exactly transparent and drive >= 6 dB is exactly
    //    f(g x)/g. Both terms have unity small-signal gain.
    //  * outputDb is the wet-path (make-up) gain; mix = 0 always yields the
    //    latency-aligned dry signal.
    //  * Type changes crossfade the two curves over 20 ms (oversampled domain).

    /** Everything derived from the (smoothed) drive in dB. */
    struct DriveGains
    {
        float g = 1.0f;        // curve input gain 10^(drive/20)
        float invG = 1.0f;     // 1 / g (unity small-signal gain)
        float depth = 0.0f;    // wet depth of the curve: 0 at 0 dB drive, 1 from 6 dB up
        float bumpBeta = 0.0f; // tape head bump: 10^(bumpDb/20) - 1, bumpDb = 1 dB * drive / 24
    };

    struct ChannelState
    {
        SvfState pre, de;  // tape pre-/de-emphasis (oversampled rate)
        SvfState bump;     // tape head-bump band-pass (base rate)
        float dcLp = 0.0f; // tube DC blocker integrator (base rate)
    };

    static DriveGains driveGains (float driveDb) noexcept;

    void updateTypeFade() noexcept;
    void computeControls (int length) noexcept;
    void runCurve (SaturationType type, ChannelState& st, float* d, int n) noexcept;
    void processSegment (const AudioBlock& io, int start, int length) noexcept;

    int osFactor = 2;
    Oversampler::Quality osQuality = Oversampler::Quality::High;
    ProcessSpec spec;
    SaturatorParams params;

    Oversampler oversampler;
    DelayLine dryDelay;    // latency-aligns the dry path with the oversampled wet path
    AudioBuffer dryBuffer; // [channel][maxBlockSize]

    // Per-segment control arrays (allocated in prepare(), never resized).
    std::vector<float> osGain, osInvGain, osScratch, osInput;        // factor * maxBlockSize (osInput: delta oversampling)
    std::vector<float> depthBuf, tubeBuf, bumpBuf, gainBuf, mixBuf; // maxBlockSize

    std::array<ChannelState, kMaxChannels> channelState {};
    SvfCoeffs preEmphasis, deEmphasis, headBump;
    float dcBlockG = 0.0f; // TPT one-pole coefficient g / (1 + g) of the 10 Hz DC blocker

    LinearSmoothedValue driveSmoother, mixSmoother, outputSmoother;
    DriveGains steady;     // drive-derived values while the drive is not ramping
    float lastGain = 1.0f; // g at the last processed base-rate sample (ramp interpolation start)
    bool driveRamping = false;

    // Type changes are crossfaded (fromType -> toType) in the oversampled domain.
    SaturationType activeType = SaturationType::Tape;
    SaturationType fromType = SaturationType::Tape;
    SaturationType toType = SaturationType::Tape;
    bool fading = false;
    int fadePos = 0;      // base-rate samples of the crossfade already done
    int fadeLength = 960; // base-rate samples
    float invFadeLength = 1.0f / 960.0f;
    float invFadeLengthOs = 1.0f / 1920.0f;

    int preparedFactor = 1; // oversampling factor actually prepared
    bool prepared = false;
};
} // namespace flub
