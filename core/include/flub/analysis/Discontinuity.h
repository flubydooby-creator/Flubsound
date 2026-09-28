// Flubsound Pro - discontinuity (glitch) detector (docs/11 E53 step 4).
//
// DiscontinuityDetector watches a stream of planar audio, block by block,
// for the four ways a real-time path or a parameter change breaks a signal:
//   Click      a single break in value: an impulse, a step, a skipped or
//              repeated sample. Found on the 4th-order difference of the
//              signal (a high-pass that is -60 dB at 1 kHz and +24 dB at
//              Nyquist at 48 kHz, so smooth programme leaves little and a
//              break leaves a spike). A residual sample counts when it is
//                * clickRatioDb over the residual's RMS on BOTH sides
//                  (windowMs each, a few samples of guard): the onset or
//                  end of a sound, where the residual stays up on one side,
//                  is not a click. A side's RMS is that of the second-
//                  quietest of its four blocks, so a second break close by
//                  does not hide the first;
//                * above clickFloorDb (default -50 dBFS: the residual of
//                  a -60 dBFS step, E53's glitch level; a -66 dBFS impulse);
//                * a break in value: cubic fits of the 8 samples on either
//                  side show a jump in value (or one sample off both) that
//                  explains at least half of the spike and more than a
//                  change of slope would. A break in slope only (a kink:
//                  the corner where a limiter or clipper catches a peak) is
//                  the processing's distortion, counted apart (kinks());
//                * not recurring: no spike of at least 0.3 x its size
//                  between 0.5 and repeatMs away on either side. A spike
//                  that recurs is the waveform's own structure - the
//                  corners of a clipped bass wave, the pulses of a buzzy
//                  voice through saturation, once per pitch period - not a
//                  break (counted apart: recurring()). Recurrences at 1 or
//                  2 x blockSize (when given) do not count: zipper noise, a
//                  step at every block boundary while a parameter moves,
//                  recurs at exactly that lag.
//              One event per windowMs.
//   Dropout    a run of exact zeros of at least minDropoutMs that starts
//              abruptly (the residual at its edge is clickRatioDb over the
//              residual before it and above clickFloorDb) on a channel
//              whose RMS over the 10 ms before was at least
//              dropoutActivityDb: an underrun or a muted block. A fade into
//              digital silence is not abrupt. Clicks at the edges of a
//              dropout belong to it and are not counted again.
//   NonFinite  a run of NaN / Inf samples (one event per run). The run is
//              read as the last finite sample held, so it causes no click
//              or dropout of its own.
//   DcStep     the signal's DC (a 2nd-order 2 Hz low-pass) moves by at least
//              dcStepDb within dcStepWindowMs (reported up to that long after
//              the step; one event per 2 x dcStepWindowMs). A step also
//              reads as a Click where it is abrupt.
// Sensitivity is relative, so it depends on the programme: on tones and
// tonal music a 1-sample skip or a -60 dBFS impulse reads 30-100 dB over
// the threshold; on broadband noise the residual is high and only large
// breaks show (the E53 soak uses tonal programme for that reason).
//
// prepare() allocates; process(), finish() and reset() do not, so the
// detector can watch a real-time stream (the events list is reserved to
// maxReported; the counts are always complete). The CLI's `soak` and
// `analyze --glitches` use it (tools/flubsound-cli/Soak.h).
#pragma once

#include "flub/common/Realtime.h"

#include <array>
#include <cstdint>
#include <vector>

namespace flub
{
enum class DiscontinuityType
{
    Click,
    Dropout,
    NonFinite,
    DcStep
};

inline constexpr int kNumDiscontinuityTypes = 4;

/** "click", "dropout", "non-finite", "dc-step". */
const char* discontinuityName (DiscontinuityType type) noexcept;

struct DiscontinuitySettings
{
    float clickRatioDb = 24.0f;       // Click: residual spike over its RMS on both sides ...
    float clickFloorDb = -50.0f;      // ... and at least this (dBFS; a -60 dBFS step or a -66 dBFS impulse)
    double windowMs = 2.0;            // ... RMS window on each side
    double repeatMs = 25.0;           // ... and not recurring within this (a pitch period)
    int blockSize = 0;                // ... except at 1 and 2 blocks (0 = unknown)
    double minDropoutMs = 0.5;        // Dropout: exact zeros for at least this long ...
    float dropoutActivityDb = -60.0f; // ... after at least this RMS (10 ms before)
    float dcStepDb = -30.0f;          // DcStep: the 2 Hz low-passed signal moves this much ...
    double dcStepWindowMs = 250.0;    // ... within this long
    int maxReported = 100;            // events kept in events() (the counts are always complete)
};

struct Discontinuity
{
    DiscontinuityType type = DiscontinuityType::Click;
    int channel = 0;
    int64_t frame = 0;       // Click: within 4 samples of the break; Dropout / NonFinite: first
                             // sample of the run; DcStep: where it was detected
    int64_t length = 0;      // Dropout / NonFinite: samples in the run; else 0
    float levelDb = 0.0f;    // Click: residual spike (dBFS); Dropout: RMS before the run (dBFS);
                             // DcStep: the DC change (dBFS); NonFinite: 0
    float overDb = 0.0f;     // Click: spike over the residual's RMS on the louder side; else 0
};

class DiscontinuityDetector
{
public:
    /** Allocates; resets. numChannels 1..32. */
    void prepare (double sampleRate, int numChannels, const DiscontinuitySettings& settings = {});

    /** Forgets the stream (counts, events, history); frame numbers restart at 0. */
    void reset() noexcept;

    /** channels[c][0, numFrames) for every prepared channel. */
    void process (const float* const* channels, int numFrames) noexcept FLUB_NONBLOCKING;

    /** End of the stream: closes open dropout / non-finite runs. The last
        few ms (the look-ahead: repeatMs or windowMs, + 6 samples) are not
        judged for clicks. */
    void finish() noexcept;

    int64_t count (DiscontinuityType type) const noexcept { return counts[static_cast<size_t> (type)]; }
    int64_t total() const noexcept;
    /** Click candidates set aside because they recur (waveform structure)
        or break the slope only (kinks: a limiter or clipper catching a peak). */
    int64_t recurring() const noexcept { return recurringCount; }
    int64_t kinks() const noexcept { return kinkCount; }
    const std::vector<Discontinuity>& events() const noexcept { return list; }
    int64_t framesSeen() const noexcept { return frames; }
    double sampleRate() const noexcept { return fs; }

private:
    static constexpr int kBlocks = 4; // each RMS window is kBlocks blocks

    struct Channel
    {
        // 4th-order difference: the last four (finite) inputs.
        std::array<double, 4> x {};
        int primed = 0;
        double lastFinite = 0.0;
        // Input and residual rings, and the running sums of r^2 over the
        // blocks of the two windows (block 0 nearest the centre).
        std::vector<double> xs, r;
        std::array<double, kBlocks> before {}, after {};
        int64_t holdUntil = -1;    // no Click before this centre frame
        int64_t quietFrom = -1, quietTo = -1; // Clicks suppressed in [from, to] (dropout / non-finite edges)
        // Dropout: activity (one-pole mean square, 10 ms), the current zero
        // run, the residual RMS before it and the largest residual at its edge.
        double activity = 0.0, activityAtRun = 0.0, residualBeforeRun = 0.0, edgePeak = 0.0;
        int64_t zeroStart = -1;
        // NonFinite run.
        int64_t nanStart = -1;
        // DcStep: 2 Hz low-pass state and its history (one value per kDcDecimation frames).
        double lp1 = 0.0, lp2 = 0.0, lpIn1 = 0.0, lpIn2 = 0.0;
        std::vector<double> dcHistory;
        int dcPos = 0, dcFilled = 0;
        int64_t dcHoldUntil = -1;
    };

    void push (Channel& ch, int c, double sample, int64_t frame) noexcept;
    void judgeClick (Channel& ch, int c, int64_t centre) noexcept;
    bool recurs (const Channel& ch, int64_t centre, double peak) const noexcept;
    bool isValueBreak (const Channel& ch, int64_t centre, double peak) const noexcept;
    bool isDropout (const Channel& ch, int64_t length) const noexcept;
    void closeZeroRun (Channel& ch, int c, int64_t end) noexcept;
    void closeNanRun (Channel& ch, int c, int64_t end) noexcept;
    void report (const Discontinuity& d) noexcept;

    DiscontinuitySettings settings;
    double fs = 48000.0;
    int window = 96, block = 24, guard = 6, lookAhead = 1206, ringSize = 2412;
    int minRepeat = 24, maxRepeat = 1200, blockSize = 0;
    int minDropout = 24;
    double clickRatio = 15.85, clickFloor = 3.16e-3, activityThreshold = 1.0e-6, dcStep = 0.03;
    double activityCoeff = 0.0;
    std::array<double, 3> lpB {}, lpA {};
    int dcDelay = 375;
    // Cubic least-squares fits over kFit samples on one side of a break
    // (positions -kFit..-1 and 1..kFit): the weights of the value and the
    // slope at 0, and the hat matrix for the fit's residual.
    static constexpr int kFit = 8;
    struct SideFit
    {
        std::array<double, kFit> value {}, slope {};
        std::array<std::array<double, kFit>, kFit> hat {};
    };
    SideFit fitLeft, fitRight;
    int64_t frames = 0, recurringCount = 0, kinkCount = 0;
    std::array<int64_t, kNumDiscontinuityTypes> counts {};
    std::vector<Channel> chans;
    std::vector<Discontinuity> list;
};
} // namespace flub
