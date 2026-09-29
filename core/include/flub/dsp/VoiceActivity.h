// Flubsound Pro - voice activity on the Chat strip's input (docs/11 E22).
//
// MixEngine's chat sidechain asks one question of the Chat strip: is a
// teammate talking right now? The detector answers it on the strip's input
// (before its chain), on the mono sum of the first two channels, in 10 ms
// frames:
//   band      the mean square in 300 - 3400 Hz (a 2nd-order high-pass and
//             low-pass, SVF), the telephone band that carries speech;
//   ratio     that band's share of the frame's whole mean square: speech has
//             most of its energy there, mixed music much of it in the kick
//             and bass below 300 Hz and in cymbals above 3.4 kHz;
//   flatness  the geometric over the arithmetic mean of six constant-Q
//             band-pass levels across 300 - 3400 Hz (1 for pink noise,
//             about 0.75 for white noise, low for formants): noise (a fan, a
//             game's ambience leaking into a teammate's microphone) is not
//             speech;
//   syllables speech falls between syllables and words: a frame that is at
//             least kSyllableDipDb under the band level's maximum of the
//             last kPeakWindowMs, while a voiced frame was seen in that
//             window, marks a syllable boundary. Sustained music (a pad, a
//             legato line, a mix) does not fall that far that often, and a
//             sound that starts after silence only rises.
// A frame is voiced when the band is over kMinLevelDb, its ratio over
// kMinBandRatio and its flatness under kMaxFlatness. It is speech when it is
// voiced and a syllable boundary was seen within kEvidenceMs. The detector
// is active while speech was seen within the last kHangoverMs (the gaps
// between words and phrases do not release the duck). So it answers about
// one syllable (150 - 250 ms) after a talker starts and kHangoverMs after
// they stop; tests/test_mix_engine_sidechain.cpp has the numbers (speech
// found, false-positive duty on music).
//
// What it cannot tell apart: staccato solo instruments in the voice band
// and sung vocals read as speech (a singer on Chat is a voice too).
//
// Threading: prepare() on the control thread before the audio starts;
// everything else on the audio thread (no allocation, fixed arrays). The
// state is published through one atomic for other threads.
#pragma once

#include "flub/common/AudioBlock.h"
#include "flub/common/Realtime.h"
#include "flub/dsp/Svf.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace flub
{
class VoiceActivity
{
public:
    static constexpr float kFrameMs = 10.0f;
    static constexpr float kBandLowHz = 300.0f, kBandHighHz = 3400.0f;
    static constexpr int kNumFlatnessBands = 6;
    static constexpr float kMinLevelDb = -70.0f;  // band mean square, dBFS
    static constexpr float kMinBandRatio = 0.5f;  // band / whole mean square
    static constexpr float kMaxFlatness = 0.55f;
    static constexpr float kSyllableDipDb = 9.0f;
    static constexpr float kPeakWindowMs = 300.0f, kEvidenceMs = 600.0f;
    static constexpr float kHangoverMs = 600.0f;

    /** Non-RT. Sets the filters and the frame length for `sampleRate` and resets. */
    void prepare (double sampleRate) noexcept;
    void reset() noexcept FLUB_NONBLOCKING;

    /** Analyses the first two channels of `in` (the first alone when it has
        one); never writes to it. */
    void process (const AudioBlock& in) noexcept FLUB_NONBLOCKING;
    /** `numSamples` of silence (a Chat strip that nobody feeds this block). */
    void processSilence (int numSamples) noexcept FLUB_NONBLOCKING;

    /** Starts as if speech had just been seen (active for kHangoverMs): the
        crossfaded engine swap carries an active talker over. Audio thread,
        or before the engine runs. */
    void seedActive() noexcept FLUB_NONBLOCKING;

    bool isActive() const noexcept { return active; }
    /** isActive() for other threads (relaxed). */
    bool isActivePublished() const noexcept { return activeFlag.load (std::memory_order_relaxed); }

    /** The last complete frame's features, for tests and diagnostics. */
    struct Frame
    {
        float bandDb = -200.0f, ratio = 0.0f, flatness = 1.0f;
        bool voiced = false, speech = false;
    };
    const Frame& lastFrame() const noexcept { return frame; }
    /** Frames analysed since prepare() / reset(), and how many were active. */
    uint64_t getFrames() const noexcept { return frames; }
    uint64_t getActiveFrames() const noexcept { return activeFrames; }

private:
    void endFrame() noexcept FLUB_NONBLOCKING;

    static constexpr int kMaxPeakFrames = 64;

    double sampleRate = 48000.0;
    int frameLength = 480, frameFill = 0;
    int peakFrames = 30, evidenceFrames = 60, hangoverFrames = 30;
    SvfCoeffs hp, lp;
    std::array<SvfCoeffs, kNumFlatnessBands> bandCoeffs {};
    SvfState hpState, lpState;
    std::array<SvfState, kNumFlatnessBands> bandStates {};
    double wholeSum = 0.0, bandSum = 0.0;
    std::array<double, kNumFlatnessBands> flatSums {};

    std::array<float, kMaxPeakFrames> history {}; // band level (dB) of the last peakFrames frames, a ring
    int historyPos = 0;
    int sinceVoiced = 1 << 20, sinceSyllable = 1 << 20, sinceSpeech = 1 << 20; // frames
    Frame frame;
    bool active = false;
    uint64_t frames = 0, activeFrames = 0;
    std::atomic<bool> activeFlag { false };
};
} // namespace flub
