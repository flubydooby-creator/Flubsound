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
//   ears, low-passed 120 Hz, at lfeGainDb re one main channel, through the
//   LfeFold that the chain's BS.775 fold shares (Bs775Fold.h, docs/11 E01).
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
//     Early reflections: 6 taps 4-19 ms, band-passed (HP 200 Hz, LP 5 kHz),
//           alternating ears,
//           level = roomAmount -> externalisation ("out of head").
// Renderer B - Measured HRIRs: per-speaker left/right impulse responses
//   (e.g. from a SOFA file, resampled to the session rate on a background
//   thread) convolved directly in the time domain. Production swaps this for
//   uniformly-partitioned FFT convolution (latency = 0 with a direct-form
//   head block). Setting an HRIR set is structural (before prepare()).
//
// Level match (docs/11 E28a, levelMatch): the speakers' binaural sum is
//   scaled so that its loudness equals the chain's BS.775 downmix of the same
//   input (virt on/off within 1 LU). A per-layout diffuse-field gain (the
//   K-weighted pink-noise power of every ear path, computed from the model or
//   the HRIRs) is the starting point; a feed-forward servo (K-weighted powers
//   of the downmix and of the render, 3 s averages, -70 LUFS gate, 6 dB/s
//   slew, +-4 dB around the diffuse gain) trims it to the content, because
//   correlated and uncorrelated content need different gains. The LFE is not
//   scaled (it is the same in both folds).
// Fold headroom (foldHeadroom): a linked zero-latency peak limiter keeps the
//   binaural output at or below 0 dBFS, so correlated full-scale content on
//   every channel is not handed to the chain as a +10 dBFS over (instant
//   attack, 10 ms hold, 150 ms release; it never acts below 0 dBFS).
// Output: binaural stereo in channels 0/1; channels >= 2 are cleared. The
// chain treats everything after this module as stereo. -3 dB headroom trim.
// Zero latency (ITD delays are part of the binaural cue, not added latency).
#pragma once

#include "Biquad.h"
#include "Bs775Fold.h"
#include "Processor.h"
#include "Svf.h"
#include "flub/common/SmoothedValue.h"

#include <array>
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
    float lfeGainDb = 0.0f;      // -20 .. +16, re one main channel
    bool lfeOn = true;           // false: the LFE is not rendered (fades out over 20 ms)
    bool levelMatch = true;      // loudness of the BS.775 downmix (E28a); off glides to unity
    bool foldHeadroom = true;    // binaural output held at or below 0 dBFS (E28a)

    bool operator== (const VirtualizerParams&) const = default;
};

class HeadphoneVirtualizer final : public Processor
{
public:
    /** Structural (call before prepare()). nullptr = parametric renderer. */
    void setHrirSet (std::shared_ptr<const HrirSet> set) { hrir = std::move (set); }

    void prepare (const ProcessSpec& spec) override;
    void reset() noexcept FLUB_NONBLOCKING override;
    void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING override;
    const char* name() const noexcept override { return "Headphone Virtualizer"; }

    /** RT-safe. Angle / head-radius changes glide (one-pole, 30 ms) and the
        filters are redesigned on 16-sample control ticks; a layout change
        fades out (~5 ms), swaps at silence, pre-rolls (2 ms, up to 10 ms for
        an HRIR) and fades back in (~5 ms). See HeadphoneVirtualizer.cpp. */
    void setParams (const VirtualizerParams& p) noexcept FLUB_NONBLOCKING;
    const VirtualizerParams& getParams() const noexcept { return params; }

    /** Speaker azimuth in degrees for a channel of a layout (NaN for LFE). */
    static float speakerAzimuthDeg (ChannelLayout layout, int channel, const VirtualizerParams& p) noexcept;

    /** Level match state, for meters and tests (read on the audio thread or
        between process() calls): the make-up applied to the speakers now,
        the running layout's diffuse-field gain, and the fold-headroom gain. */
    float getMakeupDb() const noexcept { return gainToDb (makeupTo); }
    float getDiffuseMakeupDb() const noexcept { return gainToDb (diffuseGain); }
    float getHeadroomGainDb() const noexcept { return gainToDb (headroomGain); }

private:
    // ---- implementation-defined below this line ----
    static constexpr int kControlInterval = 16; // samples between geometry updates
    static constexpr int kNumReflections = 6;

    enum class Role : uint8_t
    {
        None = 0, // not part of the layout: ignored and cleared
        Speaker,  // virtual loudspeaker at speakerAzimuthDeg()
        Lfe       // low-passed to both ears
    };

    /** One ear of one virtual speaker (parametric renderer). "prev" values are
        the design at the previous control tick; while `ramping` the running
        values are interpolated per sample from prev to current. */
    struct EarPath
    {
        float delay = 0.0f, prevDelay = 0.0f; // Woodworth delay (samples)
        std::array<float, 4> taps {};         // Lagrange taps for `delay`
        int base = 0;                         // integer read offset of taps[0]
        BiquadCoeffs shadow, prevShadow;      // Brown-Duda head shadow
        BiquadState state;
    };

    struct Speaker
    {
        Role role = Role::None;
        float azimuth = 0.0f;
        OnePoleSmoother shelfDb; // rear-cue shelf gain, control rate
        SvfCoeffs shelf, prevShelf;
        SvfState shelfState;
        std::array<EarPath, 2> ears {}; // 0 = left, 1 = right
        bool clean = true;              // every state of this channel is zero
    };

    /** Renderer B data for one speaker: mirrored input history (2 x length,
        so the newest `length` samples are always contiguous) and the
        time-reversed HRIR of each ear. */
    struct HrirPath
    {
        std::vector<float> history;
        std::array<std::vector<float>, 2> reversed;
        double diffusePower = 0.0; // both ears, K-weighted pink (level match)
        bool present = false;
    };

    bool loadHrir();
    void tick() noexcept;
    void swapLayout() noexcept;
    void updateGeometry (bool snap) noexcept;
    void clearState() noexcept;
    void clearChannel (int channel) noexcept;
    void renderSegment (const AudioBlock& block, int start, int length, int numInputs) noexcept;
    template <bool Ramp>
    void renderParametric (Speaker& sp, float* line, const float* x, int length) noexcept;
    void renderHrir (HrirPath& path, const float* x, int length) noexcept;
    void renderLfe (const float* x, int length) noexcept;
    void renderReflections (int length) noexcept;
    double pathWeight (double freqHz) const noexcept;
    float diffuseGainFor() const noexcept;
    void updateMakeup() noexcept;

    std::shared_ptr<const HrirSet> hrir;
    ProcessSpec spec;
    VirtualizerParams params;

    ChannelLayout runningLayout = ChannelLayout::Surround71;
    std::array<Speaker, kMaxChannels> speakers {};
    OnePoleSmoother frontAngle, sideAngle, rearAngle, headRadius; // control rate

    // Parametric ITD delay lines (one per channel, shared write position).
    std::array<std::vector<float>, kMaxChannels> itdLines;
    int itdMask = 0, itdWrite = 0;
    float maxDelaySamples = 0.0f;

    // Renderer B.
    std::array<HrirPath, kMaxChannels> hrirPaths;
    int hrirLength = 0, hrirWrite = 0;
    ChannelLayout hrirLayout = ChannelLayout::Surround71;
    bool hrirValid = false, useHrir = false;

    // LFE: 4th-order Butterworth low-pass and level (shared with Bs775Fold).
    LfeFold lfe;

    // Early reflections from the band-limited mono speaker sum.
    SvfCoeffs reflHpCoeffs, reflLpCoeffs;
    SvfState reflHpState, reflLpState;
    std::vector<float> reflLine;
    std::array<int, kNumReflections> reflDelay {};
    int reflMask = 0, reflWrite = 0;
    LinearSmoothedValue roomGain;

    // Scratch (maxBlockSize): ear accumulators, the reflection bus, the LFE
    // (kept apart from the level match) and the reference downmix D.
    std::vector<float> accL, accR, bus, lfeBus, refL, refR;

    // Level match (E28a). K-weighting (BS.1770 shelf and RLB high-pass) of
    // four lanes: D left / right, binaural left / right.
    SvfCoeffs kShelf, kHighPass;
    std::array<float, 4> kShelf1 {}, kShelf2 {}, kHp1 {}, kHp2 {}; // SVF states per lane
    double periodRef = 0.0, periodBin = 0.0; // this period's K-weighted energies
    double avgRef = 0.0, avgBin = 0.0;       // their 3 s averages
    double averageCoeff = 0.0;               // per period
    float diffuseGain = 1.0f;                // running layout's diffuse-field gain
    float makeupFrom = 1.0f, makeupTo = 1.0f; // per-sample ramp across one period
    float makeupSlew = 1.0f;                 // largest ratio per period (6 dB/s)
    int servoPhase = 0;                      // samples into the period (stream time)
    bool learned = false;                    // the averages hold gated content
    bool servoSeeded = false;                // false until the first swap after prepare()

    // Fold headroom (E28a): linked zero-latency peak gain.
    float headroomGain = 1.0f, headroomRelease = 0.0f;
    int headroomHold = 0, headroomHoldSamples = 0;

    // Control-rate state, aligned to absolute stream time.
    int samplesToTick = 0;
    int rampPos = 0;
    bool ramping = false;
    bool busy = false;
    int fadeSamples = 240; // layout-swap dip, each direction (whole control periods)
    int fadePos = 240;
    int fadeDir = 0;
    // Silent pre-roll after a swap so the new paths fill their (cleared) delay
    // lines before the fade-in starts (whole control periods).
    int holdParametric = 96, holdHrir = 96, holdRemaining = 0;
};
} // namespace flub
