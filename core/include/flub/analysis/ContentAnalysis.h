// Flubsound Pro - the per-strip content analysis tap (docs/11 E34).
//
// Measures what the programme is, not what the chain does to it, so the
// macros can respond to the material ("Smart" scaling, MacroMap.h
// MacroModulation) and `flubsound-cli analyze` can describe a file:
//
//   * loudness : K-weighted (BS.1770) mean square over the window, LUFS
//   * PLR      : the highest sample peak in the window over that loudness
//                (short-term peak-to-loudness ratio, LU): a brick-walled
//                master reads 6-8, open material 12 and more
//   * crest    : the same peak over the plain RMS of both channels, dB
//   * tilt     : least-squares slope of octave-band levels (constant-Q
//                band-passes on the mid, 125 Hz .. 8 kHz) over log2 f,
//                dB / octave; pink noise reads 0, white noise +3
//   * highTilt : the 8 kHz (and 16 kHz) bands' mean level over the
//                500 Hz .. 2 kHz bands' mean (0 on pink noise), dB
//   * lowShare : the mid's energy below 100 Hz (4th-order Butterworth
//                low-pass) over its whole energy, dB (pink over 20 Hz ..
//                20 kHz: about -6.3 dB)
//   * flux     : mean positive change of the octave-band levels from one
//                frame to the next, dB per frame
//   * onsets   : per second of programme; a 10 ms energy of the mid, or of
//                its octave bands from 4 kHz up (hats, consonants), more
//                than 6 dB over the mean of the 50 ms before it and above
//                -70 dB, at most one per 50 ms
//   * correlation and side: E[LR] / sqrt(E[L^2] E[R^2]) and the side's
//                energy over the mid's (M/S width, dB) over the window
//
// Frames of kFrameMs (10 Hz, frameSamples()) close on a sample count kept
// since reset(), whatever the host blocks: the state after a frame depends
// only on the samples before it. The window is the last kWindowFrames
// frames that carried programme: a frame whose RMS is at or below
// kSilenceDb is left out (the state holds through pauses and silence),
// and the state is valid once kMinFrames programme frames are in. Surround
// input (6 / 8 channels, FL FR FC LFE [BL BR] SL SR) is analysed as a
// stereo mixdown: the centre and each side's surrounds at -3 dB, the LFE at
// -6 dB into both sides.
//
// Real time: process() and reset() allocate nothing and take no lock (the
// ring buffers are fixed arrays); about 15 double biquads per sample.
// ProcessingChain runs it in-line, ahead of the stereo fold, only while
// Smart macros or a reader need it, so an offline render is deterministic
// and identical to the live one. AnalysisSnapshot hands the state to other
// threads (a seqlock of relaxed words, as the governor's memory box).
#pragma once

#include "flub/common/AudioBlock.h"
#include "flub/common/Math.h"
#include "flub/common/Realtime.h"
#include "flub/dsp/Biquad.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace flub
{
struct AnalysisState
{
    static constexpr float kNoReading = 1000.0f;

    uint32_t frames = 0;        // programme frames measured since reset (all, not only the window's)
    bool valid = false;         // at least ContentAnalysis::kMinFrames programme frames in the window
    float windowSeconds = 0.0f; // programme the window holds
    float loudnessLufs = kMinusInfDb;
    float peakDbfs = kMinusInfDb;
    float plrDb = kNoReading;   // peak - loudness
    float crestDb = kNoReading; // peak - RMS
    float tiltDbPerOctave = 0.0f;
    float highTiltDb = 0.0f;
    float lowShareDb = kMinusInfDb;
    float fluxDb = 0.0f;
    float onsetsPerSecond = 0.0f;
    float correlation = 1.0f;
    float sideDb = kMinusInfDb;
};

class ContentAnalysis
{
public:
    static constexpr float kFrameMs = 100.0f;
    static constexpr int kWindowFrames = 30; // 3 s of programme
    static constexpr int kMinFrames = 5;     // 0.5 s before the state is valid
    static constexpr float kSilenceDb = -70.0f; // a frame's plain RMS (both channels) at or below it is a pause
    static constexpr int kNumBands = 9; // octaves 63 Hz .. 16 kHz
    static constexpr std::array<double, kNumBands> kBandHz { 62.5, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0 };

    /** Non-RT. frameSamples 0 = kFrameMs at this rate. */
    void prepare (double sampleRate, int frameSamples = 0);
    void reset() noexcept FLUB_NONBLOCKING;
    /** 1 .. 8 channels (1: mono on both sides). measure false only advances
        the frame clock (a block the chain's sanitiser hid from its loops). */
    void process (const AudioBlock& block, bool measure = true) noexcept FLUB_NONBLOCKING;
    /** The state after the last closed frame (audio thread, or after a render). */
    const AnalysisState& getState() const noexcept { return state; }
    int frameSamples() const noexcept { return frameLength; }

    /** Whole-programme analysis (non-RT helper for the CLI and tests):
        runs a fresh analyser over planar channels and returns the state of
        one window as long as the programme (every programme frame counts;
        flux and onsets are the whole programme's means). */
    static AnalysisState analyseWhole (const float* const* channels, int numChannels, int64_t numSamples, double sampleRate);

private:
    struct Frame
    {
        double kPower = 0.0;     // sum of the K-weighted, channel-summed power
        double plainPower = 0.0; // sum of L^2 + R^2
        double mm = 0.0, ss = 0.0, ll = 0.0, rr = 0.0, lr = 0.0;
        double low = 0.0;        // sum of the mid's power below 100 Hz
        std::array<double, kNumBands> band {};
        float peak = 0.0f;
        float flux = 0.0f;
        int onsets = 0;
        int samples = 0;
    };

    void closeFrame() noexcept FLUB_NONBLOCKING;
    void updateState() noexcept FLUB_NONBLOCKING;
    void analyseSample (double l, double r) noexcept FLUB_NONBLOCKING;

    double sampleRate = 48000.0;
    int frameLength = 4800, subLength = 480;
    int numBands = kNumBands; // bands whose centre is below 0.4 x the sample rate
    BiquadCoeffs kStage1, kStage2;
    std::array<BiquadCoeffs, 2> lowPass {}; // 4th-order Butterworth at 100 Hz
    std::array<BiquadCoeffs, kNumBands> bandCoeffs {};
    std::array<double, kNumBands> bandNorm {}; // each band's power gain on pink noise, inverted
    std::array<BiquadState, 2> kState1 {}, kState2 {};
    std::array<BiquadState, 2> lowState {};
    std::array<BiquadState, kNumBands> bandState {};

    Frame current;
    std::array<float, kNumBands> previousBandDb {};
    bool previousBandsValid = false;
    // Onsets: the 10 ms sub-frames' energy of the mid and of its first
    // difference, and the kOnsetHistory sub-frames before.
    static constexpr size_t kOnsetHistory = 5;
    std::array<double, 2> subEnergy {};
    int subCount = 0;
    std::array<std::array<double, kOnsetHistory>, 2> subHistory {};
    int subHistoryCount = 0, subHistoryPos = 0, refractory = 0;

    std::array<Frame, kWindowFrames> ring {};
    int ringCount = 0, ringPos = 0;
    // analyseWhole(): one window over the whole programme instead of the ring.
    bool wholeProgramme = false;
    Frame whole;
    int wholeFrames = 0;
    double wholeFlux = 0.0;

    AnalysisState state;
};

/** A lock-free hand-over of an AnalysisState from the audio thread to any
    other: a seqlock of relaxed 32-bit words. */
class AnalysisSnapshot
{
public:
    static_assert (std::is_trivially_copyable_v<AnalysisState>);
    void publish (const AnalysisState& s) noexcept FLUB_NONBLOCKING;
    /** False if nothing was published yet or a write kept overlapping. */
    bool read (AnalysisState& s) const noexcept;
    void clear() noexcept;

private:
    static constexpr size_t kWords = (sizeof (AnalysisState) + sizeof (uint32_t) - 1) / sizeof (uint32_t);
    std::atomic<uint32_t> sequence { 0 };
    std::array<std::atomic<uint32_t>, kWords> words {};
};
} // namespace flub
