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
// Both need the router to list the running apps (canList). Endpoint routing
// also needs it to move them (canMoveEndpoint): a Windows build without
// FLUB_ENABLE_UNDOCUMENTED_ROUTING lists but cannot move, so Automatic takes
// process capture there, or Disabled with getUnavailableReason(); it never
// picks a method whose every move fails.
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
//
// Route journal (docs/11 E47). Every endpoint move is recorded in a journal
// file next to the settings (RouteJournal, "route-journal.json") BEFORE the
// move is made, atomically (temporary file + rename), and the record goes
// when the move is undone. A crash, TerminateProcess or SIGKILL therefore
// leaves a journal that names every app that may still sit on a Flubsound
// endpoint; the next start reads it and undoes those moves (a process of the
// recorded executable is moved back to the system default when it is seen;
// the very process that was moved, named by process id + start time, is kept
// on its endpoint instead while it is still assigned there). A move whose
// journal entry cannot be written is not made.
//
// Doubling guard (docs/11 E47). A process capture copies an app's audio into
// a strip while the app keeps playing to its own endpoint. When that endpoint
// is the device Flubsound plays to (a headset used for both), the original
// and the processed copy are heard together, a few milliseconds apart (comb
// filtering, smeared transients). Such a capture is not started (a running
// one stops); the app is reported (AppState::doublingBlocked, amber in the
// routing panel) with the fix: set its output to another device
// (getSpareEndpoints), where the capture still reaches it.
//
// Tournament mode (docs/11 E55). setTournamentMode (true) freezes routing:
// the worker stops enumerating sessions (no process is opened, no audio
// session is touched), makes no endpoint move and starts or stops no
// capture; the app list and the running captures stay as they were. Leaving
// it runs a pass at once. The router itself opens each process once while
// its sessions last (platform::ProcessInfoCache).
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

/** docs/11 E47: the write-ahead record of the endpoint moves AppRouting made
    and has not undone yet. A small JSON file, replaced atomically on every
    change (TemporaryFile + rename, so a crash leaves either the old or the
    new content, never a torn file) and removed when nothing is left. Not
    thread-safe: AppRouting uses it from its worker thread only (and from
    the message thread while the worker is not running). */
class RouteJournal
{
public:
    struct Entry
    {
        uint32_t processId = 0;        // 0: the process exited while moved; its executable's next process is moved back
        uint64_t processStartTime = 0; // AudioSessionInfo::processStartTime; 0 = unknown
        juce::String executable;       // normalised (lower case, no directory, no ".exe")
        juce::String executablePath;   // full path when the OS reports it
        juce::String endpoint;         // the endpoint it was moved to
        juce::String previousEndpoint; // the endpoint it played to before the move
        bool pending = false;          // written before the move: whether it happened is unknown, so it counts as made

        bool operator== (const Entry&) const = default;
    };

    /** An invalid file (juce::File()) keeps no journal: writes succeed and
        nothing survives the process (settings that are not persisted). */
    explicit RouteJournal (juce::File journalFile) : file (std::move (journalFile)) {}

    const juce::File& getFile() const noexcept { return file; }

    /** The entries on disk; empty when there is no file. A file that cannot
        be parsed is kept aside as "<name>.bad" (its executables are lost,
        which the settings' routing.routedApps record still covers). */
    std::vector<Entry> load() const;

    /** Replaces the file with `entries` (an empty list deletes it). Skips the
        write when they equal the last written ones. false when the file
        could not be written. */
    bool write (const std::vector<Entry>& entries);

    static constexpr int kVersion = 1;

private:
    juce::File file;
    std::vector<Entry> written;
    bool hasWritten = false;
};

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
        bool playsToOutput = false;   // docs/11 E47: it plays straight to the device Flubsound plays to (heard directly)
        bool doublingBlocked = false; // docs/11 E47: mapped for process capture, not captured because of that (doubling guard)
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

    /** Replaces the router and the capture probe before start() (tests: the
        routing of an EngineController built without app routing). Ignored
        once started. */
    void setRouter (std::unique_ptr<flub::platform::AppAudioRouter> newRouter, bool canCapture);

    /** Begins applying routes (worker thread, captures, endpoints). */
    void start();

    /** Stops the worker, restores endpoints and stops captures. Idempotent. */
    void shutdown();

    // ---- Capabilities ------------------------------------------------------------
    /** Apps can be listed and moved to another endpoint (router canList and
        canMoveEndpoint). */
    bool isEndpointRoutingSupported() const noexcept;
    /** Process captures can start: the platform captures and the router lists
        the apps to capture. */
    bool isCaptureSupported() const noexcept { return captureSupported && canEnumerateApps(); }
    /** True if sessions can be enumerated (router available and canList()). */
    bool canEnumerateApps() const noexcept;

    Method getMethod() const noexcept { return method; }
    void setMethod (Method newMethod);
    /** Automatic -> EndpointRouting if supported, else ProcessCapture if
        supported, else Disabled. An explicit method that is not supported
        gives Disabled. */
    Method getEffectiveMethod() const noexcept;
    /** Why getEffectiveMethod() is Disabled and what to do instead
        (user-presentable); empty when a method is in effect. */
    juce::String getUnavailableReason() const;

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
    /** Running applications whose audio reaches a strip through us right now
        (moved to a strip endpoint, or captured), counted once per process.
        0 = per-app routing processes nothing (the panel's red state). */
    int getProcessedAppCount() const;
    /** Requests an enumeration + apply pass now (asynchronous). */
    void refresh();
    /** Keep enumerating every 2 s even without routes (e.g. while a routing
        page is visible). */
    void setLiveUpdates (bool shouldUpdate);
    /** Re-reads strip names / endpoints after the strip layout changed. */
    void stripLayoutChanged();
    void openSystemRoutingSettings();

    // ---- Doubling guard (docs/11 E47) --------------------------------------------
    using OutputEndpoint = flub::platform::AppAudioRouter::OutputEndpoint;

    /** The app plays to `endpointId`: one of its active sessions does, or (none
        active) its current one. False for an empty id (output unknown). Pure. */
    static bool playsToEndpoint (const flub::platform::AudioSessionInfo& session, const std::string& endpointId);

    /** The endpoint Flubsound plays to as the router knows it, from the last
        pass in process-capture mode; an empty id = unknown, the guard is off. */
    const OutputEndpoint& getOutputEndpoint() const noexcept { return outputEndpoint; }
    /** Active output endpoints other than the output (and other than
        Flubsound's own), from the last pass that held a capture back: where an
        app can play instead so that only its captured copy is heard. */
    const std::vector<OutputEndpoint>& getSpareEndpoints() const noexcept { return spareEndpoints; }
    /** Assigned applications the doubling guard holds back (display names). */
    juce::StringArray getDoublingBlockedApps() const;
    /** What the guard did and the fix, user-presentable; empty when no
        assigned application is held back. */
    juce::String describeDoubling() const;

    /** Where the name of the output device comes from (message thread).
        Default: the host's device manager (the current device's output). */
    void setOutputDeviceSource (std::function<juce::String()> source);

    // ---- Tournament mode (docs/11 E55) ------------------------------------------
    /** Freezes routing (see the header comment); not persisted here. */
    void setTournamentMode (bool shouldFreeze);
    bool isTournamentMode() const noexcept { return tournament; }

    /** The route journal's file (next to the settings file). */
    const juce::File& getJournalFile() const noexcept { return journal.getFile(); }

    // ---- Device input links (docs/11 E48) ----------------------------------------
    /** What the router's last connectEndpointInputs pass said about linking
        the strips' endpoints to the device input (Linux: the sinks' monitors
        to Flubsound's PipeWire input), user-presentable; empty while nothing
        is wrong or nothing needs linking. The native PipeWire device links
        its own sinks, so the router links nothing for it (its status comes
        from AudioEngineHost::getNativeNodeStatus). Any thread. */
    juce::String getInputLinkStatus() const;
    /** false when the last pass left a wanted link missing. Any thread. */
    bool areInputsLinked() const;

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
        juce::String outputDevice; // the output device's name (doubling guard)
        bool frozen = false;       // tournament mode: no pass at all
        bool deviceLinksInputs = false; // docs/11 E48: the native PipeWire device links the strips' sinks itself
        uint64_t generation = 0;   // bumped when routes, method, strips, the output device or tournament mode change
    };

    void run() override;
    void handleAsyncUpdate() override;
    void publishConfig();
    void applyCaptures (std::vector<AppState>& states);
    void stopAllCaptures();
    void persistRoutedExecutables (const std::set<juce::String>& executables);
    juce::String currentOutputDevice() const;
    /** Writes the journal for the current moves plus `pendingMove` (the move
        about to be made), if any. Worker thread (or no worker running). */
    bool syncJournal (const RouteJournal::Entry* pendingMove);

    AudioEngineHost& host;
    AppSettings& settings;
    std::unique_ptr<flub::platform::AppAudioRouter> router;
    bool captureSupported = false, liveUpdates = false, isStarted = false, isShutDown = false, tournament = false;
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
    std::function<juce::String()> outputDeviceSource;
    OutputEndpoint outputEndpoint;
    std::vector<OutputEndpoint> spareEndpoints;

    // Shared with the worker
    juce::CriticalSection lock;
    WorkerConfig config;
    std::vector<AppState> workerResult;
    std::set<juce::String> resultRoutedExecutables;
    uint64_t resultGeneration = 0; // config generation workerResult was computed from
    OutputEndpoint resultOutputEndpoint;
    std::vector<OutputEndpoint> resultSpareEndpoints;
    bool resultPending = false;
    juce::String inputLinkStatus; // docs/11 E48: connectEndpointInputs' last status
    bool inputsLinked = true;

    // Worker thread only (and the message thread before it starts / after it stopped)
    struct RoutedEndpoint
    {
        juce::String endpoint;   // endpoint we assigned
        juce::String executable; // normalised executable of the process
        juce::String executablePath, previousEndpoint; // for the journal
        uint64_t processStartTime = 0;
    };
    std::map<uint32_t, RoutedEndpoint> routedEndpoints; // pid -> our move
    /** Normalised executables we moved and have not moved back yet (also
        those whose processes exited while routed); persisted in the settings. */
    std::set<juce::String> routedExecutables;
    std::set<uint32_t> restoreTried; // pids of such executables already moved back (once per process)
    RouteJournal journal;
    /** Moves an earlier run recorded (it did not undo them: a crash): pid ->
        entry, until the first non-empty enumeration shows whether that very
        process still runs. */
    std::map<uint32_t, RouteJournal::Entry> recoveredMoves;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppRouting)
};
} // namespace flub::app
