// Flubsound Pro - runs the live latency measurement (docs/11 E42d in the app).
//
// Settings > Audio > Measure latency: one or two passes of
// latency::ProbeSession through AudioEngineHost (DeviceOnly, ThroughFlubsound,
// or Both: device only first, then through Flubsound, so the engine's own
// latency is measured as their difference), then latency::analyse() for each
// on a worker thread, then the reports. EngineController owns one and polls
// it from its 2 Hz timer; tests poll it directly.
//
//   Idle --start()--> Running (pass 1 [, pass 2]) --> Analysing --> Done
//                        |  cancel(), the device stopped
//                        +--> Failed (error says why; "Cancelled." for a cancel)
//
// Both passes are handed to the host at start(), chained (ProbeSession::
// chainTo): the second plays from the callback after the first ends, so the
// programme never returns between them and the 2 Hz poll only collects
// them.
//
// start() refuses (and says why, whyNot()) without a running device, with no
// input channel open, while the feedback-loop guard holds the output, for a
// strip that does not exist or (Through) is muted, while a measurement runs,
// and during a device soak (an output pin or a device signal source is set,
// docs/11 E53). What the device and the engine report
// (latency::DeviceContext) is snapshot at start(); each pass's glitches are
// counted from start() (the first) or from the last poll before the pass
// before it was collected (the second) to the poll that collects it: a glitch
// near the passes' boundary counts for both.
//
// Message thread only (the worker only runs analyse() on sessions it owns).
#pragma once

#include "AudioEngineHost.h"
#include "LatencyMeasurement.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace flub::app
{
class LatencyMeasurer
{
public:
    enum class Mode : uint8_t
    {
        DeviceOnly,
        ThroughFlubsound,
        Both
    };

    enum class Phase : uint8_t
    {
        Idle,
        Running,
        Analysing,
        Done,
        Failed
    };

    struct State
    {
        Phase phase = Phase::Idle;
        Mode mode = Mode::Both;
        int pass = 0, passes = 0;   // the pass running (1-based) of passes
        double progress = 0.0;      // 0 .. 1 over every pass
        double secondsLeft = 0.0;   // of playing
        juce::String error;         // Failed: why
        std::optional<latency::PassReport> deviceOnly, through;
        uint32_t generation = 0;    // increments on every change

        /** The result as the Settings page shows it (empty unless Done). */
        juce::String describe() const;
    };

    struct Request
    {
        Mode mode = Mode::Both;
        int strip = 0;              // the strip the probe enters (Through) and whose latency is reported
        juce::String stripName;
        /** Other probe settings (tests: shorter sweeps); the sample rate is the device's. */
        std::optional<flub::latency::ProbeSettings> settings;
    };

    explicit LatencyMeasurer (AudioEngineHost& host);
    ~LatencyMeasurer();

    /** "" when `request` can start now, else why not (user-presentable). */
    juce::String whyNot (const Request& request) const;
    /** Starts the first pass; returns whyNot() when it cannot. */
    juce::String start (const Request& request);
    /** Fades the probe out (5 ms); the next poll() reports "Cancelled.". */
    void cancel();
    /** One step: progress, the next pass, the analysis. True when the state
        changed (progress included). */
    bool poll();

    const State& getState() const noexcept { return state; }
    bool isBusy() const noexcept { return state.phase == Phase::Running || state.phase == Phase::Analysing; }
    /** Called from poll() when a measurement ends (Done or Failed). */
    std::function<void (const State&)> onFinished;

    /** The probe's total playing time for a mode at a rate (both passes). */
    static double durationSeconds (Mode mode, double sampleRate, const std::optional<flub::latency::ProbeSettings>& settings = {});

private:
    static flub::latency::ProbeSettings settingsFor (const Request& r, double sampleRate);
    latency::DeviceContext snapshot() const;
    void fail (const juce::String& error);
    void finishAnalysis();

    AudioEngineHost& host;
    Request request;
    State state;
    std::vector<latency::Path> plan;
    bool cancelling = false;                    // cancel() during this measurement
    juce::String failure;                       // set on a cancel or a stop; reported once every pass is handed back
    int64_t playedBefore = 0, totalSamples = 0; // over the plan
    latency::DeviceContext context;             // the running pass's (snapshot at the start; glitches from its start)
    int64_t glitchesAtLastPoll = -1;            // the device's glitch count at the previous poll
    std::vector<std::pair<std::unique_ptr<latency::ProbeSession>, latency::DeviceContext>> passes;

    std::thread worker;
    std::atomic<bool> workerDone { false };
    std::vector<latency::PassReport> workerReports; // written by the worker before workerDone

    JUCE_DECLARE_NON_COPYABLE (LatencyMeasurer)
};
} // namespace flub::app
