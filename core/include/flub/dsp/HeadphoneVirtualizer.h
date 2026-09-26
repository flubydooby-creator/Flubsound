// Flubsound Pro - multichannel (5.1 / 7.1) to binaural headphone virtualiser.
//
// Games render true positional audio when the output device reports 7.1, so
// the "Flubsound Game" virtual endpoint advertises 7.1 and this module folds
// it down binaurally for headphones (the Windows Sonic / "virtual 7.1" idea).
//
// Channel order (Windows KSAUDIO_SPEAKER_*_SURROUND / WAVEFORMATEXTENSIBLE):
//   5.1 : FL FR FC LFE SL SR
//   7.1 : FL FR FC LFE BL BR SL SR
// Virtual speaker azimuths (deg, + = right): FL/FR -/+frontAngle (30),
//   FC 0, SL/SR -/+sideAngle (100), BL/BR -/+rearAngle (145). LFE -> both
//   ears, low-passed 120 Hz, at lfeGainDb.
//
// Renderer A - Parametric (built in, no data licence needed):
//   Brown & Duda (1998) spherical-head model per source/ear:
//     ITD : Woodworth  tau(theta) = (a/c)(1 - cos theta)          theta < 90 deg
//                               = (a/c)(1 + theta - pi/2)       theta >= 90 deg
//           theta = angle between source and ear axis, a = head radius,
//           c = 343 m/s; fractional delay via 3rd-order Lagrange.
//     ILD : head-shadow  H(s) = (1 + alpha s / (2 w0)) / (1 + s / (2 w0)),
//           w0 = c/a, alpha(theta) = 1.05 + 0.95 cos(theta * 180/150 deg),
//           bilinear-transformed (BiquadCoeffs::fromAnalogFirstOrder).
//     Rear cue: -4 dB high shelf @ 4 kHz for |azimuth| > 90 deg (pinna
//           shadow; resolves the front/back symmetry of a sphere).
//     Early reflections: 6 taps 4-19 ms, low-passed, alternating ears,
//           level = roomAmount -> externalisation ("out of head").
// Renderer B - Measured HRIRs: per-speaker left/right impulse responses
//   (e.g. from a SOFA file, resampled to the session rate on a background
//   thread) convolved directly in the time domain. Production swaps this for
//   uniformly-partitioned FFT convolution (latency = 0 with a direct-form
//   head block). Setting an HRIR set is structural (before prepare()).
//
// Output: binaural stereo in channels 0/1; channels >= 2 are cleared. The
// chain treats everything after this module as stereo. -3 dB headroom trim.
// Zero latency (ITD delays are part of the binaural cue, not added latency).
#pragma once

#include "Processor.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace flub
{
enum class ChannelLayout : uint8_t
{
    Stereo = 0,
    Surround51,
    Surround71
};

inline int channelCount (ChannelLayout l) noexcept
{
    return l == ChannelLayout::Surround71 ? 8 : (l == ChannelLayout::Surround51 ? 6 : 2);
}

struct HrirSet
{
    double sampleRate = 48000.0;
    ChannelLayout layout = ChannelLayout::Surround71;
    int length = 0;                     // taps per ear
    std::vector<std::vector<float>> left, right; // [speaker][tap], speaker order as layout
};

struct VirtualizerParams
{
    ChannelLayout layout = ChannelLayout::Surround71;
    float frontAngleDeg = 30.0f; // 22 .. 45
    float sideAngleDeg = 100.0f; // 80 .. 120
    float rearAngleDeg = 145.0f; // 120 .. 165
    float headRadiusMm = 87.5f;  // 70 .. 105 (personalisation)
    float roomAmount = 0.15f;    // 0 .. 1
    float lfeGainDb = 0.0f;      // -20 .. +10

    bool operator== (const VirtualizerParams&) const = default;
};

class HeadphoneVirtualizer final : public Processor
{
public:
    /** Structural (call before prepare()). nullptr = parametric renderer. */
    void setHrirSet (std::shared_ptr<const HrirSet> set) { hrir = std::move (set); }

    void prepare (const ProcessSpec& spec) override;
    void reset() noexcept override;
    void process (const AudioBlock& block) noexcept override;
    const char* name() const noexcept override { return "Headphone Virtualizer"; }

    /** Angle/head changes recompute filters at the next block (RT-safe,
        coefficient-only); layout changes take effect immediately. */
    void setParams (const VirtualizerParams& p) noexcept;
    const VirtualizerParams& getParams() const noexcept { return params; }

    /** Speaker azimuth in degrees for a channel of a layout (NaN for LFE). */
    static float speakerAzimuthDeg (ChannelLayout layout, int channel, const VirtualizerParams& p) noexcept;

private:
    // ---- implementation-defined below this line ----
    std::shared_ptr<const HrirSet> hrir;
    ProcessSpec spec;
    VirtualizerParams params;
};
} // namespace flub
