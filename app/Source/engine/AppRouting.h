// Flubsound Pro - per-application routing model (executable -> strip).
//
// The user maps executables ("cs2.exe", "Spotify") to strips ("Game",
// "Music", ...). The map is persisted in AppSettings and applied with one of
// two methods (resolved per OS by getEffectiveMethod()):
//
//   EndpointRouting  flub::platform::AppAudioRouter::setAppEndpoint moves the
//                    app's audio to the strip's virtual endpoint (Windows:
//                    "Flubsound Game" endpoint; Linux: "flubsound_game" null
//                    sink), whose capture side / monitor feeds the engine
//                    through the device inputs. The OS remembers such a move
//                    per application (Windows per-app device preference,
//                    PulseAudio / PipeWire stream-restore), so every move is
//                    undone: when the app is un-mapped or the method changes,
//                    for apps still running at shutdown, and - for apps that
//                    exited while routed - the next time a process of that
//                    executable appears un-mapped, in this run or a later one
//                    (the executables are kept in the settings until then).
//   ProcessCapture   flub::platform::ProcessLoopbackCapture captures the app's
//                    process tree directly into the strip through a
//                    DriftCompensatedFifo (AudioEngineHost::startProcessCapture).
//
// Threading: AppAudioRouter calls block for 5-30 ms (COM / pactl), so session
// enumeration and endpoint moves run on a background worker ("Flubsound
// routing", every 2 s while routes exist or live updates are on, or on
// refresh()). Results are handed to the message thread, which owns the
// capture decisions (AudioEngineHost is message-thread only) and the
// published app list; a result computed from an outdated configuration (the
// routes, method or strip layout changed during the pass) is discarded and
// the next pass follows at once. A capture that fails to start is tried a few
// times, then given up until the mapping or method changes or another
// capture stops (and frees a slot). The public API is message-thread only.
#pragma once

#include "platform/PlatformServices.h"
#include "settings/AppSettings.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace flub::app
{
class AudioEngineHost;

class AppRouting final : private juce::Thread, private juce::AsyncUpdater
{
public:
    using Method = AppSettings::RoutingMethod;

    struct AppState
    {
        uint32_t processId = 0;
        juce::String executable, displayName, endpointId;
        bool isActive = false;     // currently producing audio
        int strip = -1;            // mapped strip index, -1 = unmapped
        bool routed = false;       // endpoint routing applied by us
        int captureId = -1;        // >= 0 while a capture runs
        juce::String error;        // last routing / capture error
    };

    /** Reads the routes / method from settings; does nothing until start(). */
    AppRouting (AudioEngineHost& host, AppSettings& settings);

    /** Same, with an injected router instead of the platform one (tests, or
        an alternative session source). nullptr = no enumeration at all;
        captureSupported replaces the platform probe (the captures themselves
        come from AudioEngineHost's capture factory). */
    AppRouting (AudioEngineHost& host, AppSettings& settings, std::unique_ptr<flub::platform::AppAudioRouter> router,
                bool captureSupported);
    ~AppRouting() override;

    /** Begins applying routes (worker thread, captures, endpoints). */
    void start();

    /** Stops the worker, restores endpoints and stops captures. Idempotent. */
    void shutdown();

    // ---- Capabilities ------------------------------------------------------------
    bool isEndpointRoutingSupported() const noexcept;
    bool isCaptureSupported() const noexcept { return captureSupported; }
    /** True if sessions can be enumerated at all (router object available). */
    bool canEnumerateApps() const noexcept { return router != nullptr; }

    Method getMethod() const noexcept { return method; }
    void setMethod (Method newMethod);
    /** Automatic -> EndpointRouting if supported, else ProcessCapture if
        supported, else Disabled. */
    Method getEffectiveMethod() const noexcept;

    // ---- Routes ----------------------------------------------------------------------
    const std::vector<AppRoute>& getRoutes() const noexcept { return routes; }
    /** Maps an executable to a strip (by name); an empty strip name removes it. */
    void setRoute (const juce::String& executable, const juce::String& stripName);
    void removeRoute (const juce::String& executable);
    juce::String getStripNameForExecutable (const juce::String& executable) const;

    /** Case-insensitive, ignores a trailing ".exe" and any directory part. */
    static bool executablesMatch (const juce::String& a, const juce::String& b);

    // ---- Live state ------------------------------------------------------------------
    /** Last enumeration result (message thread copy). */
    const std::vector<AppState>& getApps() const noexcept { return apps; }
    /** Requests an enumeration + apply pass now (asynchronous). */
    void refresh();
    /** Keep enumerating every 2 s even without routes (e.g. while a routing
        page is visible). */
    void setLiveUpdates (bool shouldUpdate);
    /** Re-reads strip names / endpoints after the strip layout changed. */
    void stripLayoutChanged();
    void openSystemRoutingSettings();

    /** Called on the message thread when the app list or routing state changed. */
    std::function<void()> onChanged;

private:
    /** Snapshot of everything the worker needs (copied under `lock`). */
    struct WorkerConfig
    {
        bool active = false;
        Method method = Method::Disabled;
        std::vector<AppRoute> routes;
        std::vector<juce::String> stripNames, stripEndpoints;
        uint64_t generation = 0; // bumped when routes, method or strips change
    };

    void run() override;
    void handleAsyncUpdate() override;
    void publishConfig();
    void applyCaptures (std::vector<AppState>& states);
    void stopAllCaptures();
    void persistRoutedExecutables (const std::set<juce::String>& executables);

    AudioEngineHost& host;
    AppSettings& settings;
    std::unique_ptr<flub::platform::AppAudioRouter> router;
    bool captureSupported = false, liveUpdates = false, isStarted = false, isShutDown = false;
    Method method = Method::Automatic;
    std::vector<AppRoute> routes;

    // Message thread
    std::vector<AppState> apps;
    struct RunningCapture
    {
        int id = -1;    // AudioEngineHost capture id
        int strip = -1; // strip it feeds (its FIFO is sized for that strip's channels)
    };
    std::map<uint32_t, RunningCapture> captures; // pid -> running capture
    struct CaptureFailure
    {
        int attempts = 0;
        juce::String error; // last start error, still reported once attempts run out
        int strip = -1;     // strip the failed starts were for
    };
    std::map<uint32_t, CaptureFailure> captureFailures; // pid -> failed starts
    uint64_t configGeneration = 0;
    std::set<juce::String> persistedRoutedExecutables; // last value written to the settings

    // Shared with the worker
    juce::CriticalSection lock;
    WorkerConfig config;
    std::vector<AppState> workerResult;
    std::set<juce::String> resultRoutedExecutables;
    uint64_t resultGeneration = 0; // config generation workerResult was computed from
    bool resultPending = false;

    // Worker thread only (and the message thread before it starts / after it stopped)
    struct RoutedEndpoint
    {
        juce::String endpoint;   // endpoint we assigned
        juce::String executable; // normalised executable of the process
    };
    std::map<uint32_t, RoutedEndpoint> routedEndpoints; // pid -> our move
    /** Normalised executables we moved and have not moved back yet (also
        those whose processes exited while routed); persisted in the settings. */
    std::set<juce::String> routedExecutables;
    std::set<uint32_t> restoreTried; // pids of such executables already moved back (once per process)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppRouting)
};
} // namespace flub::app
