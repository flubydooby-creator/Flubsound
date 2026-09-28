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
// Runs at 1x, 2x (default) or 4x via Oversampler, or an explicit
// Oversampler::Design (the chain uses Oversampler::forProfile: 4x with the
// 2x designs' latency below 176.4 kHz); latency = oversampler latency.
// Dry/wet mix is latency-aligned internally (the dry path is delayed).
// Telemetry: getDistortionDb() = THD+N of the stage over the last completed
// 25 ms analysis window (DistortionEstimator.h), measured at the oversampled rate around the curve,
// where input and shaped output are aligned; depth, mix and output gain are
// folded in, the linear post filters (residual-path and tube DC blockers, tape
// head bump) are not.
// Residual-path DC blocker (docs/11 E10): the band-limited deviation passes a
// 5 Hz 1st-order high-pass before it joins the dry path, so no curve leaves
// DC; the programme never passes it.
#pragma once

#include "DistortionEstimator.h"
#include "Oversampler.h"
#include "Processor.h"
#include "Svf.h"
#include "flub/common/DelayLine.h"
#include "flub/common/SmoothedValue.h"

#include <array>
#include <atomic>
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
        osDesign = Oversampler::design (factor, q);
    }
    /** Structural: call before prepare(). An explicit design (factor 1, 2 or 4). */
    void setOversampling (const Oversampler::Design& design) noexcept { osDesign = design; }

    void prepare (const ProcessSpec& spec) override;
    void reset() noexcept FLUB_NONBLOCKING override;
    void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING override;
    int latencySamples() const noexcept override;
    const char* name() const noexcept override { return "Saturation"; }

    void setParams (const SaturatorParams& p) noexcept FLUB_NONBLOCKING;
    const SaturatorParams& getParams() const noexcept { return params; }

    /** The static curve, exposed for tests and the GUI transfer plot. */
    static float shape (SaturationType type, float x) noexcept;

    /** THD+N of the stage over the last 25 ms analysis window (dB re its output; -160 = clean, drive 0 or mix 0). */
    float getDistortionDb() const noexcept { return distortionDb.load (std::memory_order_relaxed); }

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
        double residualDcLp = 0.0; // residual-path DC blocker on the deviation (base rate)
    };

    static DriveGains driveGains (float driveDb) noexcept;

    void updateTypeFade() noexcept;
    void computeControls (int length) noexcept;
    void runCurve (SaturationType type, ChannelState& st, float* d, int n) noexcept;
    void processSegment (const AudioBlock& io, int start, int length) noexcept;

    Oversampler::Design osDesign = Oversampler::design (2, Oversampler::Quality::High);
    ProcessSpec spec;
    SaturatorParams params;

    Oversampler oversampler;
    DelayLine dryDelay;    // latency-aligns the dry path with the oversampled wet path
    AudioBuffer dryBuffer; // [channel][maxBlockSize]

    // Per-segment control arrays (allocated in prepare(), never resized).
    std::vector<float> osGain, osInvGain, osScratch, osInput;        // factor * maxBlockSize (osInput: delta oversampling)
    std::vector<float> depthBuf, tubeBuf, bumpBuf, gainBuf, mixBuf; // maxBlockSize
    std::vector<float> distWeightBuf; // maxBlockSize: curve deviation weight re the linear path (THD+N telemetry)

    DistortionWindow distortionWindow; // sums over a window of at least 25 ms (closes at a segment boundary)
    std::atomic<float> distortionDb { -160.0f };

    std::array<ChannelState, kMaxChannels> channelState {};
    SvfCoeffs preEmphasis, deEmphasis, headBump;
    float dcBlockG = 0.0f; // TPT one-pole coefficient g / (1 + g) of the 10 Hz DC blocker
    double residualDcG = 0.0; // the same for the 5 Hz residual-path DC blocker

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
