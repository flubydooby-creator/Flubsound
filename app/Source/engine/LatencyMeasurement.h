// Flubsound Pro - the live latency measurement (docs/11 E42d in the app).
//
// Settings > Audio > "Measure latency" plays flub::latency::LatencyProbe's
// exponential sweeps on the current output device and records the current
// input device IN THE SAME DEVICE CALLBACK, so the recording is
// sample-locked to playback:
//
//   probe sample i leaves in the callback that also delivers input sample i,
//   so recording[i + R] holds what came back and LatencyProbe::analyse()
//   reads the round trip R directly.
//
// R is everything between the callback and itself: the output buffering, the
// driver, the converters and the USB / wireless link, the air between the
// earcup and the microphone (about 0.03 ms per cm) or a cable, and the same
// on the way in. Two paths:
//
//   DeviceOnly        the probe replaces the device outputs (the engine keeps
//                     running underneath, unheard): R = the device's own
//                     round trip.
//   ThroughFlubsound  the probe is the input of one strip (every other strip's
//                     input is faded out for the measurement) and plays
//                     through its chain, its sync padding and the master
//                     limiter: R also holds the strip's engine latency. The
//                     programme that was playing still leaves the chain for
//                     its latency (and its tails) after the inputs fade, so
//                     an output GATE fades the outputs out at the start and
//                     holds them silent through the probe's lead-in; it opens
//                     (5 ms) just as the first sweep reaches the output (the
//                     lead-in + the strip latency, setOutputLatency). The
//                     chain may raise the sweep (Boost, the maximizer, a
//                     loudness-matched bypass), so from the gate's opening
//                     the output is held under kThroughCapDbfs by a
//                     sample-peak cap whose gain only ever falls (a fixed
//                     gain after the first loud sweep, which the
//                     deconvolution does not mind); the programme's tail
//                     never reaches the cap, so it cannot turn the sweep
//                     down. The cap stops where the returning programme
//                     could reach the output and eases back to unity.
//
// Both (device only, then through Flubsound) runs as two sessions chained on
// the audio thread (chainTo): the second starts in the callback after the
// first ends, the first leaves the outputs silent at its end and the second
// starts with its gate closed, so the programme never plays between them.
//
// Safety: a modest level (kProbeLevelDbfs peak), a short probe (5 sweeps of
// 0.5 s, 6.55 s in all at any rate), played once (never looped), 5 ms
// crossfades in and out of the programme, and a cancel that fades out in 5
// ms (Through: the gate closes in 5 ms, stays closed until the delayed sweep
// has left the chain and fades the programme back in, uncapped). The UI asks
// for confirmation and to turn the volume down first.
//
// From the round trip the analysis estimates the playback path (what the
// listener hears): the device's reported output latency (+ the engine's on
// the Through path) plus half of what the device does not report (the
// converters and the link are assumed to split evenly between the two
// directions), with the bounds "all of it on the input side" .. "all of it on
// the output side". It grades the result (high / medium / low / none) from
// the runs accepted, their spread (clock drift between separate devices, a
// device that changed its buffering), the median SNR, the strongest other
// arrival (an echo, a headset's sidetone) and the glitches during the pass.
//
// Threads: ProbeSession is built on the message thread (allocates: the probe
// and the recording), handed to the device callback through
// AudioEngineHost::startLatencyProbe and read back once finished() (after the
// host has waited for the audio thread to let go of it). Everything the
// callback calls is noexcept and FLUB_NONBLOCKING: no allocation, no lock.
// analyse() allocates (LatencyProbe's FFTs) and runs on a worker thread.
//
// Plain C++ (no JUCE), so tests/test_rtsan.cpp can check the audio-thread
// signatures.
#pragma once

#include "flub/analysis/LatencyProbe.h"
#include "flub/common/AudioBlock.h"
#include "flub/common/Realtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace flub::app::latency
{
enum class Path : uint8_t
{
    DeviceOnly,
    ThroughFlubsound
};

/** The sweep's peak level. */
inline constexpr double kProbeLevelDbfs = -24.0;
/** Through Flubsound: the output never exceeds this sample peak. */
inline constexpr double kThroughCapDbfs = -18.0;
/** The crossfade into and out of the probe, and the cancel's fade. */
inline constexpr double kFadeMs = 5.0;
/** Through Flubsound, after a cancel whose strip latency is not known
    (setOutputLatency not called): the gate stays closed this long (the
    chain's delayed tail; the longest app engine latency is about 31 ms). */
inline constexpr double kCancelTailMs = 100.0;

/** The app's probe at `sampleRate`: 5 sweeps of 0.5 s (20 Hz .. 20 kHz) at
    kProbeLevelDbfs, gaps for round trips up to 500 ms, 0.3 s of lead-in:
    6.55 s in all. */
flub::latency::ProbeSettings liveProbeSettings (double sampleRate);

class ProbeSession
{
public:
    static constexpr int kMaxRecordChannels = 2;

    /** Allocates the probe and the recording. `recordChannels` (0 ..
        kMaxRecordChannels): the device inputs to record (the first ones);
        `strip`: the strip the probe enters on the Through path. Settings
        must validate(). */
    ProbeSession (const flub::latency::ProbeSettings& probeSettings, Path probePath, int probeStrip, int channelsToRecord);

    // ---- Message thread, before the hand-over -------------------------------------
    /** Through Flubsound: the strip's output latency in samples (chain + sync
        padding + master limiter). The output gate then opens, and the cap
        starts, just as the first sweep reaches the output, the cap stops
        where the returning programme could reach it, and a cancel's silence
        lasts until the delayed sweep has left. < 0 (the default): unknown;
        the gate opens as the first sweep enters the strip and a cancel waits
        kCancelTailMs. */
    void setOutputLatency (int samples) noexcept;
    int getOutputLatency() const noexcept { return static_cast<int> (outputLatency); }

    /** Both: this DeviceOnly session is followed at once by `following`,
        handed over with it (AudioEngineHost plays it from the callback after
        this one ends). At its natural end this session leaves the outputs
        silent instead of fading back to the programme and tells `following`,
        which then starts with its output gate closed: the programme never
        plays between the passes. A cancel fades back to the programme as
        usual; `following`, cancelled too, then ends at its first callback
        without touching the audio. */
    void chainTo (ProbeSession& following) noexcept;

    Path getPath() const noexcept { return path; }
    int getStrip() const noexcept { return strip; }
    int getRecordChannels() const noexcept { return recordChannels; }
    const flub::latency::ProbeSettings& getSettings() const noexcept { return settings; }
    int64_t length() const noexcept { return static_cast<int64_t> (probe.size()); }
    /** Through: where the first sweep reaches the output (the lead-in + the
        strip latency) and where the output gate is fully open (the same, at
        least two fades in). */
    int64_t sweepArrival() const noexcept { return arrival; }
    int64_t gateOpenAt() const noexcept { return openAt; }

    // ---- Audio thread (the device callback) ---------------------------------------
    /** At the start of every callback while the session is handed over: a
        cancel request moves the end to 5 ms from here (Through: the gate
        closes and reopens once the delayed sweep has left the chain). A
        session cancelled before it played anything ends at once, untouched
        (the pass after a cancelled one). */
    void beginCallback() noexcept FLUB_NONBLOCKING
    {
        if (! begun)
        {
            begun = true;
            silentStart = outputSilentAtStart.load (std::memory_order_relaxed);
            gate = silentStart ? 0.0f : 1.0f;
        }
        if (! cancelRequested.load (std::memory_order_relaxed) || cancelledHere)
            return;
        cancelledHere = true;
        cancelled.store (true, std::memory_order_release);
        if (playPos == 0 && ! silentStart)
        {
            end = stopAt = 0;
            finish();
            return;
        }
        end = std::min (end, playPos + fade);
        if (path == Path::ThroughFlubsound)
        {
            cancelAt = playPos;
            reopenAt = cancelAt + fade + (outputLatency >= 0 ? outputLatency + fade : tail);
            stopAt = reopenAt + fade;
        }
        else
        {
            stopAt = std::min (stopAt, end);
        }
    }

    bool isPlaying() const noexcept FLUB_NONBLOCKING { return ! done.load (std::memory_order_relaxed); }

    /** Through Flubsound, the probe's strip: this callback's samples [offset,
        offset + numSamples) of the probe crossfaded into the block's first two
        channels (the others faded out). */
    void fillStrip (const flub::AudioBlock& block, int offset) const noexcept FLUB_NONBLOCKING
    {
        for (int k = 0; k < block.numSamples; ++k)
        {
            const int64_t index = playPos + offset + k;
            const float w = ramp (index, true, true);
            const float x = index < length() ? w * probe[static_cast<size_t> (index)] : 0.0f;
            for (int c = 0; c < block.numChannels; ++c)
            {
                float& v = block.channel (c)[k];
                v = (1.0f - w) * v + (c < 2 ? x : 0.0f);
            }
        }
    }

    /** Through Flubsound, every other strip: its input faded out while the
        probe plays. */
    void muteOther (const flub::AudioBlock& block, int offset) const noexcept FLUB_NONBLOCKING
    {
        for (int k = 0; k < block.numSamples; ++k)
        {
            const float g = 1.0f - ramp (playPos + offset + k, true, true);
            for (int c = 0; c < block.numChannels; ++c)
                block.channel (c)[k] *= g;
        }
    }

    /** Once per callback after the engine wrote `outputs`: DeviceOnly
        crossfades the probe in place of them; ThroughFlubsound gates and caps
        them (it only ever turns them down). With `outputAllowed` false (the
        feedback-loop guard holds the output, the engine is not ready, or the
        device is not a soak's pinned output) the DeviceOnly probe is not
        written. Then the inputs are recorded and the session advances; it
        finishes at the end of the probe (or of a cancel's fade, or of a
        Through cancel's silence). */
    void process (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int numSamples,
                  bool outputAllowed) noexcept FLUB_NONBLOCKING
    {
        if (done.load (std::memory_order_relaxed))
            return;
        if (path == Path::DeviceOnly)
        {
            if (outputAllowed)
            {
                // Chained (and not cancelled): no fade back at the end; past
                // the probe the outputs stay silent for the next pass.
                const bool fadeOut = next == nullptr || cancelledHere;
                for (int k = 0; k < numSamples; ++k)
                {
                    const int64_t index = playPos + k;
                    const float w = ramp (index, ! silentStart, fadeOut);
                    const float x = index < length() ? w * probe[static_cast<size_t> (index)] : 0.0f;
                    for (int c = 0; c < numOutputs; ++c)
                        if (float* d = outputs[c]; d != nullptr)
                            d[k] = (1.0f - w) * d[k] + (c < 2 ? x : 0.0f);
                }
            }
        }
        else
        {
            gateAndCap (outputs, numOutputs, numSamples);
        }

        for (int c = 0; c < recordChannels; ++c)
        {
            auto& r = rec[static_cast<size_t> (c)];
            const float* in = c < numInputs ? inputs[c] : nullptr;
            for (int k = 0; k < numSamples; ++k)
                if (const int64_t index = playPos + k; index < length())
                    r[static_cast<size_t> (index)] = in != nullptr ? in[k] : 0.0f;
        }

        playPos += numSamples;
        published.store (std::min (playPos, length()), std::memory_order_release);
        if (playPos >= stopAt)
            finish();
    }

    // ---- Any thread -------------------------------------------------------------------
    /** Samples played so far (0 .. length()). */
    int64_t position() const noexcept { return published.load (std::memory_order_acquire); }
    /** The probe was played to its end (or a cancel faded it out): the
        recording is complete. */
    bool finished() const noexcept { return done.load (std::memory_order_acquire); }
    bool wasCancelled() const noexcept { return cancelled.load (std::memory_order_acquire); }
    /** Fades the probe out within 5 ms and finishes (the callback takes it at
        its next start). */
    void requestCancel() noexcept { cancelRequested.store (true, std::memory_order_relaxed); }
    /** Through Flubsound: how far the cap turned the output down (dB, <= 0;
        0 when it never acted), and how far it had before the first sweep
        reached the output (sound left over from before the measurement,
        louder than the cap; normally 0). The sweep's own share is the
        difference. */
    double getCapGainDb() const noexcept;
    double getCapBeforeSweepDb() const noexcept;

    /** Message thread, once finished(): what input `channel` recorded. */
    const std::vector<float>& recording (int channel) const noexcept { return rec[static_cast<size_t> (std::clamp (channel, 0, kMaxRecordChannels - 1))]; }

private:
    /** 0 -> 1 over the first `fade` samples (fadeIn), 1 -> 0 over the `fade`
        samples before `end` (fadeOut; without it 1 up to and past the end). */
    float ramp (int64_t index, bool fadeIn, bool fadeOut) const noexcept FLUB_NONBLOCKING
    {
        if (index < 0)
            return 0.0f;
        if (index >= end)
            return fadeOut ? 0.0f : 1.0f;
        int64_t edge = fade;
        if (fadeIn)
            edge = std::min (edge, index);
        if (fadeOut)
            edge = std::min (edge, end - 1 - index);
        return edge >= fade ? 1.0f : static_cast<float> (edge + 1) / static_cast<float> (fade + 1);
    }

    /** Through: the output gate (a 5 ms ramp, closed until it opens for the
        first sweep, and after a cancel until reopenAt) and the -18 dBFS cap
        (tracking from the gate's opening to capUntil, or to the end of a
        cancel's fade; past capUntil it eases back to unity by stopAt). */
    void gateAndCap (float* const* outputs, int numOutputs, int numSamples) noexcept FLUB_NONBLOCKING
    {
        for (int k = 0; k < numSamples; ++k)
        {
            const int64_t index = playPos + k;
            const bool open = cancelAt >= 0 ? index >= reopenAt : index >= openAt - fade;
            gate = open ? std::min (1.0f, gate + gateStep) : std::max (0.0f, gate - gateStep);

            const bool capOn = cancelAt < 0 || index < cancelAt + fade;
            const bool tracking = capOn && (cancelAt >= 0 || (index >= openAt - fade && index < capUntil));
            if (tracking)
            {
                float peak = 0.0f;
                for (int c = 0; c < numOutputs; ++c)
                    if (const float* d = outputs[c]; d != nullptr)
                        peak = std::max (peak, std::abs (d[k]));
                const float level = peak * gate;
                if (level * capGain > capLinear)
                {
                    capGain = capLinear / level;
                    capMin = std::min (capMin, capGain);
                    if (index < arrival)
                        capBefore = capGain;
                }
            }
            else if (capOn && cancelAt < 0 && index >= capUntil && capGain < 1.0f)
            {
                // Past the last sweep: back to unity by the end, so the
                // returning programme does not step up when the session ends.
                capGain += (1.0f - capGain) / static_cast<float> (std::max<int64_t> (1, stopAt - index));
            }

            const float g = gate * (capOn ? capGain : 1.0f);
            if (g != 1.0f)
                for (int c = 0; c < numOutputs; ++c)
                    if (float* d = outputs[c]; d != nullptr)
                        d[k] *= g;
        }
        capMinStat.store (capMin, std::memory_order_relaxed);
        capBeforeStat.store (capBefore, std::memory_order_relaxed);
    }

    void finish() noexcept FLUB_NONBLOCKING
    {
        if (next != nullptr)
            next->outputSilentAtStart.store (path == Path::DeviceOnly && ! cancelledHere, std::memory_order_relaxed);
        done.store (true, std::memory_order_release);
    }

    /** The Through timeline (arrival, openAt, capUntil) from the settings, the
        strip latency and the fade. */
    void plan() noexcept;

    flub::latency::ProbeSettings settings;
    Path path;
    int strip = 0, recordChannels = 0;
    std::vector<float> probe;
    std::array<std::vector<float>, kMaxRecordChannels> rec;
    int64_t fade = 1, tail = 0, outputLatency = -1;
    int64_t arrival = 0, openAt = 0, capUntil = 0; // Through (plan())
    float gateStep = 1.0f, capLinear = 1.0f;
    ProbeSession* next = nullptr; // chainTo (message thread, before the hand-over)

    // Audio thread (fillStrip / muteOther read them inside the same callback):
    // the probe's weight falls to 0 at `end`; the session finishes at `stopAt`.
    int64_t playPos = 0, end = 0, stopAt = 0;
    int64_t cancelAt = -1, reopenAt = 0; // Through, after a cancel
    float gate = 1.0f, capGain = 1.0f, capMin = 1.0f, capBefore = 1.0f;
    bool begun = false, silentStart = false, cancelledHere = false;

    std::atomic<int64_t> published { 0 };
    std::atomic<bool> done { false }, cancelRequested { false }, cancelled { false };
    std::atomic<bool> outputSilentAtStart { false }; // set by the previous chained session as it ends
    std::atomic<float> capMinStat { 1.0f }, capBeforeStat { 1.0f };
};

// =============================================================================
// Analysis
// =============================================================================
/** What the device and the engine reported while the pass ran (the message
    thread's snapshot when it started, and the glitch count when it ended). */
struct DeviceContext
{
    std::string outputDevice, inputDevice, deviceType, stripName;
    double sampleRate = 0.0;
    int bufferSize = 0;
    int reportedOutputSamples = 0, reportedInputSamples = 0; // juce::AudioIODevice::getOutput / getInputLatencyInSamples
    int stripLatencySamples = 0; // the probe strip's output latency in the engine (chain + sync padding + master limiter)
    int engineSamples = 0;       // the engine's (the largest strip's), for the DeviceOnly estimate
    double captureBufferMs = 0.0; // per-app capture FIFO target + resampler (0 without captures)
    int64_t glitchesBefore = -1, glitchesAfter = -1; // the device's glitch counter (EngineStatus::glitches), -1 unknown
};

enum class Confidence : uint8_t
{
    None,   // no result
    Low,    // a result, but ambiguous (runs disagree, a second arrival nearly as strong, glitches)
    Medium, // a result with a weak signal, a rejected run or a small spread
    High
};

struct PassReport
{
    Path path = Path::DeviceOnly;
    bool ok = false;
    std::string error;                 // why there is no result (ok false)
    DeviceContext context;
    flub::latency::Result result;      // the chosen input channel's analysis
    int channel = 0;                   // which input channel (0-based)
    int runs = 0;                      // runs in the probe

    double roundTripSamples = 0.0, roundTripMs = 0.0;
    double spreadMs = 0.0;
    double reportedOutputMs = 0.0, reportedInputMs = 0.0;
    double engineMs = 0.0;             // the engine's share of the round trip (Through: the strip's latency; DeviceOnly: 0)
    double unexplainedMs = 0.0;        // round trip - (reported out + in + engine): converters, link, air, unreported buffering
    double playbackMs = 0.0;           // estimated playback path (output side): reported out + engine + unexplained / 2
    double playbackMinMs = 0.0, playbackMaxMs = 0.0; // the bounds (all unexplained on the input / output side)
    double withEngineMs = 0.0;         // DeviceOnly: the playback estimate plus the engine's reported latency; Through: playbackMs
    double capGainDb = 0.0;            // Through: the cap's lowest gain (0 = never acted)
    double capBeforeSweepDb = 0.0;     // Through: its gain before the first sweep reached the output (leftover sound; normally 0)
    int64_t glitches = -1;             // glitches during the pass (-1 unknown)

    Confidence confidence = Confidence::None;
    std::vector<std::string> warnings; // what lowers the confidence, and notes
};

/** Analyses each recorded channel with LatencyProbe and keeps the best (a
    result first, then the most accepted runs, then the highest median SNR);
    fills the split, the estimate and the grade. Allocates; not for the audio
    thread. */
PassReport analyse (const ProbeSession& session, const DeviceContext& context);

/** The grade's thresholds. */
inline constexpr double kHighMaxSpreadMs = 0.25, kLowMinSpreadMs = 1.0;
inline constexpr double kHighMinSnrDb = 40.0;
inline constexpr double kAmbiguousSecondaryDb = -6.0;

/** "high", "medium", "low", "none". */
const char* confidenceName (Confidence c) noexcept;

/** The report as the Settings page shows it (several lines). */
std::string describe (const PassReport& report);
/** Both paths: each pass, then the engine's latency as measured (Through -
    DeviceOnly) against what it reports. */
std::string describeBoth (const PassReport& deviceOnly, const PassReport& through);
/** One line for the log. */
std::string summarise (const PassReport& report);
} // namespace flub::app::latency
