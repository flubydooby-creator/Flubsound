// Flubsound Pro - discontinuity (glitch) detector (docs/11 E53 step 4).
//
// DiscontinuityDetector watches a stream of planar audio, block by block,
// for the four ways a real-time path or a parameter change breaks a signal:
//   Click      a value discontinuity: an impulse, a step, a skipped or
//              repeated sample. Found on the 4th-order difference of the
//              signal (a high-pass that is -60 dB at 1 kHz and +24 dB at
//              Nyquist at 48 kHz, so smooth programme leaves little and a
//              break leaves a spike): a residual sample is a candidate when
//              it is clickRatioDb over the residual's RMS on BOTH sides
//              (windowMs each, a few samples of guard) and above
//              clickFloorDb. Both sides, so the onset or end of a sound (the
//              residual stays up on one side) is not one. A side's RMS is
//              that of the second-quietest of its four blocks, so a second
//              break close by does not hide the first. A candidate is a
//              click when cubic fits of the 8 samples on either side of it
//              show a jump in value (or one sample off both) that explains
//              at least half of the spike; a jump in slope only (a hard
//              clipper's corner) is distortion, counted apart as a kink.
//              One event per windowMs.
//   Dropout    a run of exact zeros of at least minDropoutMs that starts
//              abruptly (the residual at its edge passes the Click rule) on
//              a channel whose RMS over the 10 ms before was at least
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
// bass-heavy music a 1-sample skip or a -60 dBFS step reads tens of dB over
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
    float clickFloorDb = -70.0f;      // ... and at least this (dBFS)
    double windowMs = 2.0;            // ... RMS window on each side
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
        few ms (windowMs or minDropoutMs, + 6 samples) are not judged for
        clicks: there is no look-ahead left. */
    void finish() noexcept;

    int64_t count (DiscontinuityType type) const noexcept { return counts[static_cast<size_t> (type)]; }
    int64_t total() const noexcept;
    /** Slope-only breaks (a hard clipper's corners): distortion, not counted as clicks. */
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
    bool isValueBreak (const Channel& ch, int64_t centre, double peak) const noexcept;
    bool isDropout (const Channel& ch, int64_t length) const noexcept;
    void closeZeroRun (Channel& ch, int c, int64_t end) noexcept;
    void closeNanRun (Channel& ch, int c, int64_t end) noexcept;
    void report (const Discontinuity& d) noexcept;

    DiscontinuitySettings settings;
    double fs = 48000.0;
    int window = 96, block = 24, guard = 6, lookAhead = 102, ringSize = 206;
    int minDropout = 24;
    double clickRatio = 15.85, clickFloor = 3.16e-4, activityThreshold = 1.0e-6, dcStep = 0.01;
    double activityCoeff = 0.0;
    std::array<double, 3> lpB {}, lpA {};
    int dcDelay = 375;
    // Cubic least-squares fits over kFit samples on one side of a break
    // (positions -kFit..-1 and 1..kFit): the weights of the value at 0, and
    // the hat matrix for the fit's residual.
    static constexpr int kFit = 8;
    struct SideFit
    {
        std::array<double, kFit> value {};
        std::array<std::array<double, kFit>, kFit> hat {};
    };
    SideFit fitLeft, fitRight;
    int64_t frames = 0, kinkCount = 0;
    std::array<int64_t, kNumDiscontinuityTypes> counts {};
    std::vector<Channel> chans;
    std::vector<Discontinuity> list;
};
} // namespace flub
