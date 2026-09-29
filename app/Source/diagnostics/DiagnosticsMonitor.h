// Flubsound Pro - engine events for the diagnostic log (docs/11 E54).
//
// The audio thread already counts what goes wrong in atomics: device xruns
// and overrunning callbacks (EngineStatus::glitches), the master safety clamp
// and the input sanitiser's NaN / Inf drops (MeterBus, per strip), and the
// overload watchdog's episodes. DiagnosticsMonitor reads them on the message
// thread twice a second (nothing is added to the audio thread) and writes
// what changed to the log:
//   * at once: the device opening, changing (type, output, input, rate,
//     buffer), closing and its errors; an overload starting and ending;
//   * counters summed, at most one line per 10 s: "Last 10 s: 3 glitches;
//     Game: 1 safety clip, 1 block dropped (NaN / Inf)".
// EventLogBuilder is the pure part (snapshots in, lines out) the tests drive.
#pragma once

#include <juce_events/juce_events.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace flub::app
{
class EngineController;
}

namespace flub::app::diagnostics
{
/** What the monitor compares between polls. */
struct EngineSnapshot
{
    bool deviceOpen = false;
    juce::String deviceType, outputName, inputName;
    double sampleRate = 0.0;
    int bufferSize = 0;
    juce::String deviceError;
    int glitches = 0; // EngineStatus::glitches (xruns + overrunning callbacks)
    bool overloaded = false;
    uint64_t overloadEpisodes = 0;

    struct Strip
    {
        juce::String name;
        uint64_t safetyClips = 0, corruptSamples = 0, droppedBlocks = 0;
    };
    std::vector<Strip> strips;
};

/** Reads the controller (message thread). */
EngineSnapshot takeSnapshot (EngineController& controller);

/** "WASAPI / Headphones (Stealth 700), in: CABLE Output, 48000 Hz, 256 samples". */
juce::String describeDevice (const EngineSnapshot& snapshot);

class EventLogBuilder
{
public:
    explicit EventLogBuilder (juce::uint32 counterIntervalMs = 10000) : interval (counterIntervalMs) {}

    /** Lines for what changed since the previous snapshot. The first call
        describes the starting state. */
    juce::StringArray update (const EngineSnapshot& now, juce::uint32 nowMs);
    /** Counter line for anything not written yet (shutdown). */
    juce::StringArray flush (juce::uint32 nowMs);

private:
    struct Pending
    {
        uint64_t glitches = 0;
        std::vector<EngineSnapshot::Strip> strips; // deltas
        bool any() const noexcept;
    };

    juce::String counterLine (juce::uint32 nowMs) const;

    const juce::uint32 interval;
    bool started = false;
    EngineSnapshot previous;
    Pending pending;
    juce::uint32 lastCounterLineMs = 0, pendingSinceMs = 0;
    bool counterLineWritten = false;
};

/** Polls the controller at 2 Hz and passes EventLogBuilder's lines to
    `write` (the session's log). Flushes the counters when destroyed. */
class DiagnosticsMonitor final : private juce::Timer
{
public:
    DiagnosticsMonitor (EngineController& controller, std::function<void (const juce::String&)> write);
    ~DiagnosticsMonitor() override;

    void pollNow();

private:
    void timerCallback() override { pollNow(); }

    EngineController& controller;
    std::function<void (const juce::String&)> write;
    EventLogBuilder builder;
};
} // namespace flub::app::diagnostics
