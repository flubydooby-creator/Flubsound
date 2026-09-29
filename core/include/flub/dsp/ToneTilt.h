// Flubsound Pro - the Warmth tone tilt (docs/11 E14): a broad body bell up
// and a high shelf down, with automatic level compensation, so Warmth is
// heard as tone rather than as level.
//
//   sections  body bell 200 Hz, Q 0.7, +3.5 dB x amount (half of it at 100
//             and 400 Hz: the upper bass and low mids - kick body, the bass
//             line's upper partials and the chest of a voice) and high shelf
//             7 kHz, Q 0.707, -3.0 dB x amount (-1.5 dB at 7 kHz, -2.6 dB at
//             10 kHz) (SVF, Svf.h). The bell leaves the sub-bass nearly
//             alone (+0.5 dB at 50 Hz, +0.3 dB at 1 kHz): a 300 Hz low shelf
//             (+3.8 dB) lifted the 40 - 80 Hz energy that dominates
//             bass-heavy programme, so the level compensation took the mids
//             and vocals down 3 dB and the low mids came out lower than at
//             Warmth 0 (docs/11 E14 Status, verifier). Pink noise through the
//             chain at amount 1, compensation included, re amount 0
//             (tests/test_warmth.cpp): +3.5 dB in the 200 Hz third-octave
//             band, +0.4 dB at 1 kHz, -2.5 dB at 10 kHz; half of each at
//             amount 0.5 (the gains are linear in dB).
//   level     the tilt's loudness change depends on the programme (pink
//             noise: about 0 LU at amount 1; the drum-and-bass test
//             programme: +0.7 LU), so no fixed trim can hold both. The stage
//             measures it: the K-weighted (BS.1770) mean square of its input
//             and of its input through the full tilt (amount 1), one-pole
//             over kCompTimeConstantMs (a running mean for the first time
//             constant after a start, so it settles in a few hundred ms),
//             both channels summed. L1 = the ratio in dB, clamped to the
//             tilt's own range [-3.0, +3.5] dB; the trim at amount a is
//             -a L1 (the loudness change is linear in a within 0.05 dB).
//             Open loop: the measure reads the stage's input, never its
//             output (in the chain the stage runs ahead of every module the
//             SafetyGovernor scales, so nothing downstream feeds back into
//             it either); the trim
//             moves with the programme's spectral balance over seconds, never
//             with a beat, and follows the amount at once, so turning Warmth
//             does not change the level either. Silent chunks (below -80 dB
//             K-weighted) are skipped. The automatic preamp's model counts
//             the sections and this trim at the target amount (ProcessingChain).
//   glide     the amount moves towards its target by at most
//             kGlidePerSecond (0 -> 1 in 200 ms); coefficients and trim are
//             recomputed every kUpdateSamples and interpolated sample by
//             sample in between, so a step of the Warmth macro, an A/B bank
//             switch or a preset change is a glide, never a click.
//   off       at amount 0 the sections are the identity (m0 = 1, m1 = m2 =
//             0) and the trim is 0 dB; once the amount has glided there and
//             the target is 0 the stage idles and does not touch the signal
//             (bit-exact), and the next start begins from cleared filters
//             and a fresh level measure.
//
// Latency 0. Stereo-linked coefficients, one filter state per channel.
#pragma once

#include "flub/common/AudioBlock.h"
#include "flub/common/Realtime.h"
#include "flub/dsp/Biquad.h"
#include "flub/dsp/Processor.h"
#include "flub/dsp/Svf.h"

#include <array>
#include <atomic>

namespace flub
{
struct ToneTiltParams
{
    float amount = 0.0f; // 0 .. 1 (warmth.tone; the Music Warmth macro drives it)

    bool operator== (const ToneTiltParams&) const = default;
};

class ToneTilt final : public Processor
{
public:
    static constexpr double kBodyHz = 200.0, kBodyQ = 0.7, kBodyMaxDb = 3.5;
    static constexpr double kHighShelfHz = 7000.0, kHighShelfQ = 0.7071, kHighShelfMaxDb = -3.0;
    static constexpr float kGlidePerSecond = 5.0f;
    static constexpr int kUpdateSamples = 32;
    static constexpr float kCompTimeConstantMs = 3000.0f;

    void prepare (const ProcessSpec& spec) override;
    /** Filter states and the level measure cleared; the amount jumps to its target. */
    void reset() noexcept FLUB_NONBLOCKING override;
    void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING override;
    const char* name() const noexcept override { return "Warmth tilt"; }

    /** Audio thread, at block start. */
    void setParams (const ToneTiltParams& p) noexcept FLUB_NONBLOCKING;

    /** The body bell and the high shelf at `amount` (0 .. 1), without the
        level compensation (the chain's static-boost model, docs/11 E11).
        RT-safe. */
    static void sections (float amount, double sampleRate, SvfCoeffs& bodyBell, SvfCoeffs& highShelf) noexcept FLUB_NONBLOCKING;

    /** True while the target is above 0, and after that until the amount has
        glided back to 0. process() is a no-op otherwise. */
    bool isRunning() const noexcept { return running; }

    /** What is applied now: the amount, and the level compensation (dB).
        Published once per block; any thread. */
    float getAppliedAmount() const noexcept { return appliedAmount.load (std::memory_order_relaxed); }
    float getCompensationDb() const noexcept { return appliedTrimDb.load (std::memory_order_relaxed); }
    /** The measured loudness change of the full tilt on the programme (L1,
        dB; 0 after a start until the first measure). Any thread. */
    float getFullTiltLoudnessDb() const noexcept { return fullTiltDb.load (std::memory_order_relaxed); }

private:
    void startMeasure() noexcept;
    void update() noexcept;

    double sampleRate = 48000.0;
    int numChannels = 2;
    float target = 0.0f, amount = 0.0f;
    float maxStep = 0.0f;       // amount per update
    double compAlpha = 0.0;     // one-pole weight per update (steady state)
    bool running = false, snapPending = false;
    int untilUpdate = 0;

    SvfCoeffs body, high, bodyFrom, highFrom;
    float trimDb = 0.0f, trimFromDb = 0.0f;
    std::array<SvfState, kMaxChannels> bodyState {}, highState {};

    // Level measure: K-weighting (two biquads per channel) of the input, and
    // the full tilt on the K-weighted input.
    Biquad kStage1, kStage2;
    SvfCoeffs fullBody, fullHigh;
    std::array<SvfState, kMaxChannels> fullBodyState {}, fullHighState {};
    double chunkIn = 0.0, chunkFull = 0.0; // sums over the current update
    double msIn = 0.0, msFull = 0.0;
    int measuredUpdates = 0; // since the last start (the running mean's count)
    float loudnessDb = 0.0f; // L1

    std::atomic<float> appliedAmount { 0.0f }, appliedTrimDb { 0.0f }, fullTiltDb { 0.0f };
};
} // namespace flub
