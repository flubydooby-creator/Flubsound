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
// Moving the original away (docs/11 E47, R4.5; Windows). With "Move the app's
// own sound away automatically" on (setMoveOriginalAway, off by default; the
// routing panel's one-click fix switches it on) Flubsound applies that fix
// itself: an assigned app that plays to Flubsound's output under process
// capture has its own output moved (router->setAppEndpoint, the per-app
// device Windows keeps per executable path) to a silent device - the one
// chosen in Settings > Routing, or automatically an active S/PDIF / optical /
// digital output, else an HDMI / DisplayPort output, either one only while no
// unassigned app plays to it, never one named Speakers / Headphones /
// Headset, Flubsound's output, the system default, a Flubsound endpoint or a
// virtual cable (chooseSilentEndpoint). The capture then starts. Such moves
// are kept per executable path (the name where the path is unknown: a
// browser's audio comes from a child process, the session's process is
// moved, which Windows applies to its executable file; an app whose path
// changes on update is a new, not yet moved, program), journaled before they
// are made (RouteJournal::Entry::movedAway, with the app's own earlier
// per-app device, and for a re-point the device it is moved from), and put
// back to that device when the app is unassigned, the option goes off or
// capture stops being the method, at shutdown, and after a crash at the next
// start (as soon as a process of that executable is seen). Windows moves an
// already open stream on neither change (measured on Windows 11): an app
// that still plays to the silent device after the put-back, and is not
// captured, is not heard until it opens a new stream, so the next passes
// look for that and say so (describePutBack: restart its playback). An app
// that does not play to the output (e.g. the user set its device by hand) is
// left alone and nothing is recorded; when the user changes the device of an
// app Flubsound moved, Flubsound forgets that move and does not move the app
// again until the one-click fix asks for it (moveOriginalsAwayNow). A silent
// device that goes away pauses new moves (an automatic choice falls back to
// the next candidate) and the app shows why; an app that still plays to the
// output after the move (it picks the device itself) stays held back with
// that explanation.
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
        juce::String previousEndpoint; // the endpoint it played to before the move (movedAway: its own per-app device, "" = the system default)
        bool pending = false;          // written before the move: whether it happened is unknown, so it counts as made
        bool movedAway = false;        // R4.5: its own output moved off Flubsound's output (put back to previousEndpoint)
        juce::String movedFrom;        // R4.5, a pending re-point: the silent device Flubsound had moved it to before (empty otherwise)

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

    /** docs/11 E47 (R4.5): what "Move the app's own sound away" did for an app. */
    enum class MoveAway : uint8_t
    {
        None,       // nothing to do (option off, unassigned, or it does not play to the output)
        Moved,      // its own output is on the silent device (Flubsound's move)
        StillPlays, // moved, but it still plays to the output (the app picks that device itself)
        LeftToUser, // the user changed its device after Flubsound's move: not moved again
        NoTarget,   // no silent device (none found, or the chosen one is not connected)
        Failed      // the move failed (AppState::error says why)
    };

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
        MoveAway moveAway = MoveAway::None; // R4.5
        juce::String movedTo;         // R4.5: the silent device's name while Moved / StillPlays
        juce::String moveNote;        // R4.5: why it is still held back (user-presentable), empty otherwise
        bool movePending = false;     // R4.5: moved in this pass; captured once the next pass (250 ms) sees it off the output
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
        (moved to a strip endpoint, or captured; R4.5: or moved away in this
        pass, captured 250 ms later), counted once per process. 0 = per-app
        routing processes nothing (the panel's red state). */
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
    /** Where the name of Flubsound's input device comes from (a virtual cable
        that feeds Flubsound is never a silent device). Default: the host's
        device manager. */
    void setInputDeviceSource (std::function<juce::String()> source);

    // ---- Moving the original away (docs/11 E47, R4.5) ----------------------------
    /** The option is offered: the router can move an app's own output
        (canMoveAppOutput, Windows) and process capture works. */
    bool canMoveOriginalAway() const noexcept;
    bool getMoveOriginalAway() const noexcept { return moveAway; }
    /** Switches the option (persisted). Off: the next pass puts back what
        Flubsound moved. */
    void setMoveOriginalAway (bool shouldMove);
    /** The routing panel's one-click fix: switches the option on and lets
        Flubsound move again the apps whose device the user changed after an
        earlier move. */
    void moveOriginalsAwayNow();
    /** The silent device chosen in Settings > Routing (persisted); an empty
        id = automatic. */
    OutputEndpoint getSilentEndpointChoice() const;
    void setSilentEndpointChoice (const OutputEndpoint& endpoint);
    /** Where the last pass would move an app's own output; an empty id = none,
        and getSilentTargetReason() says why. Known once a process-capture
        pass has run with a router that can move. */
    const OutputEndpoint& getSilentTarget() const noexcept { return silentTarget; }
    const juce::String& getSilentTargetReason() const noexcept { return silentTargetReason; }
    /** The output endpoints the last such pass listed (the system default
        first): the choices of Settings > Routing. */
    const std::vector<OutputEndpoint>& getKnownOutputEndpoints() const noexcept { return knownEndpoints; }
    /** The one-click fix's label ("Move automatically to Digital Audio
        (S/PDIF)", or "Move again to ..." for an app the user took back), empty
        when it is not offered (option unavailable, nothing to move to, or the
        option on with nothing to retry). */
    juce::String getMoveAwayAction() const;
    /** One line for Settings > Routing: what the option does now. */
    juce::String describeMoveAway() const;
    /** Apps Flubsound put back (unassigned, option off, method change) that
        still play to the silent device and are not captured, so they are
        not heard until they open a new stream: one user-presentable line
        each ("Spotify still plays to Digital Audio (S/PDIF) ...: restart its
        playback ..."), from the last pass. Kept until the app plays
        elsewhere, exits, or the user sets that device for it. */
    const juce::StringArray& getPutBackNotes() const noexcept { return putBackNotes; }
    /** getPutBackNotes() joined, empty when there are none (the routing
        panel's notice). */
    juce::String describePutBack() const { return putBackNotes.joinIntoString (" "); }

    /** The silent device rules (pure). From `endpoints` (the system default
        first, as listOutputEndpoints() gives them) never the output
        (outputId), the system default, a Flubsound endpoint or one that feeds
        Flubsound's input device (feedsInput). A chosen id wins when it is
        listed and allowed; a chosen one that is not listed gives no target and
        says it is not connected. Automatic: `current` (where Flubsound's moves
        already are) while it is a candidate, else the first digital output
        (S/PDIF, optical), else the first display output (HDMI, DisplayPort),
        each only when it is not in `busy` (an unassigned app plays there: a
        receiver or a monitor's speakers may be in use); never a virtual cable;
        none: an explanation. */
    struct SilentTarget
    {
        OutputEndpoint endpoint; // empty id = none
        juce::String reason;     // why there is none (user-presentable)
    };
    static SilentTarget chooseSilentEndpoint (const std::vector<OutputEndpoint>& endpoints, const std::string& outputId,
                                              const juce::String& inputDevice, const OutputEndpoint& chosen,
                                              const std::set<std::string>& busy, const std::string& current);
    /** 2 = a digital output (S/PDIF, SPDIF, optical, TOSLINK, "Digital
        Output"), 1 = a display output (HDMI, DisplayPort, a graphics card's
        audio), 0 = anything else, also any endpoint whose name starts with
        Speakers, Headphones, Headset or Earphones (what one listens with,
        e.g. "Speakers (USB Digital Audio)"). Pure. */
    static int silentPreference (const juce::String& endpointName);
    /** A virtual cable / mixer endpoint (VB-CABLE, VoiceMeeter, BlackHole,
        "Virtual", "Loopback", Flubsound's own). Pure. */
    static bool looksLikeVirtualCable (const juce::String& endpointName);
    /** The render endpoint is the other end of the cable Flubsound records
        from (a virtual cable whose device part, the last "(...)", is the
        input device's). Pure. */
    static bool feedsInput (const juce::String& endpointName, const juce::String& inputDevice);

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
        bool moveAway = false;          // R4.5: the option is on (and offered)
        OutputEndpoint silentChoice;    // R4.5: the chosen silent device (empty id = automatic)
        juce::String inputDevice;       // R4.5: Flubsound's input device (a cable feeding it is never silent)
        uint64_t moveRetry = 0;         // R4.5: bumped by moveOriginalsAwayNow()
        uint64_t generation = 0;   // bumped when routes, method, strips, the output device or tournament mode change
    };

    /** One application's own output, moved away by Flubsound (R4.5). Kept
        per move key (moveKeyOf: the executable's path, or its name where the
        path is unknown). */
    struct SilentMove
    {
        juce::String endpoint;          // where Flubsound moved it
        juce::String previous;          // its own per-app device before ("" = the system default)
        uint32_t processId = 0;         // the latest process of it seen (moves and restores go through it)
        uint64_t processStartTime = 0;
        juce::String executable;        // normalised name (the journal; matches a process when the path is unknown)
        juce::String executablePath;
        bool unconfirmed = false;       // a pending journal entry of an earlier run: the move may never have happened
        juce::String repointFrom;       // unconfirmed re-point: the silent device it was on before (still Flubsound's move there)
    };

    /** An app Flubsound put back that may still play to the silent device
        (R4.5): Windows moves an open stream only when the app opens a new one. */
    struct PutBackWatch
    {
        juce::String executable;   // normalised name
        juce::String name;         // display name
        juce::String endpoint;     // the silent device it was moved to (id)
        juce::String endpointName;
    };

    void run() override;
    void handleAsyncUpdate() override;
    void publishConfig();
    void applyCaptures (std::vector<AppState>& states);
    void stopAllCaptures();
    void persistRoutedExecutables (const std::set<juce::String>& executables);
    juce::String currentOutputDevice() const;
    juce::String currentInputDevice() const;
    /** Puts the moved-away apps back through the processes `sessions` lists
        (same executable path, or name where the record has none; the recorded
        process when its start time matches). Returns the move keys put back
        (with their records). Worker thread, or no worker running. */
    std::map<juce::String, SilentMove> restoreSilentMoves (const std::vector<flub::platform::AudioSessionInfo>& sessions);
    /** The worker's R4.5 step of a pass: moves, re-points, puts back and
        explains (AppState::moveAway / movedTo / moveNote), per move key.
        `states` parallels `sessions`. Returns the keys moved or put back in
        this pass (the next pass then follows in 250 ms). */
    std::set<juce::String> applySilentMoves (const WorkerConfig& c, const std::vector<flub::platform::AudioSessionInfo>& sessions,
                                             std::vector<AppState>& states, const OutputEndpoint& output, const SilentTarget& silent,
                                             const std::vector<OutputEndpoint>& endpoints, bool canMoveAway);
    /** The worker's look at the apps put back (putBackWatch): the notes of
        those that still play to the silent device and are not captured.
        Drops a watch once the app plays elsewhere, exits, is moved again, or
        the user set that device for it. */
    juce::StringArray watchPutBacks (const WorkerConfig& c, const std::vector<flub::platform::AudioSessionInfo>& sessions,
                                     const std::vector<AppState>& states);
    /** Writes the journal for the current moves plus `pendingMove` (the move
        about to be made), if any. Worker thread (or no worker running). */
    bool syncJournal (const RouteJournal::Entry* pendingMove);

    AudioEngineHost& host;
    AppSettings& settings;
    std::unique_ptr<flub::platform::AppAudioRouter> router;
    bool captureSupported = false, liveUpdates = false, isStarted = false, isShutDown = false, tournament = false;
    Method method = Method::Automatic;
    std::vector<AppRoute> routes;
    bool moveAway = false;        // R4.5: the option (settings)
    OutputEndpoint silentChoice;  // R4.5: the chosen silent device (settings)
    uint64_t moveRetry = 0;

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
    std::function<juce::String()> outputDeviceSource, inputDeviceSource;
    OutputEndpoint outputEndpoint;
    std::vector<OutputEndpoint> spareEndpoints;
    OutputEndpoint silentTarget;                // R4.5: the last pass's silent device
    juce::String silentTargetReason;
    std::vector<OutputEndpoint> knownEndpoints; // R4.5: the last pass's output endpoints
    juce::StringArray putBackNotes;             // R4.5: the last pass's put-back notes

    // Shared with the worker
    juce::CriticalSection lock;
    WorkerConfig config;
    std::vector<AppState> workerResult;
    std::set<juce::String> resultRoutedExecutables;
    uint64_t resultGeneration = 0; // config generation workerResult was computed from
    OutputEndpoint resultOutputEndpoint;
    std::vector<OutputEndpoint> resultSpareEndpoints;
    SilentTarget resultSilentTarget;
    std::vector<OutputEndpoint> resultKnownEndpoints;
    juce::StringArray resultPutBackNotes;
    bool resultHasSilentTarget = false; // the pass computed one (process capture, a router that can move)
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
    /** R4.5: apps whose own output Flubsound moved away (move key -> move),
        also those whose processes exited (Windows keeps the device per
        executable path) and those an earlier run left (journal). */
    std::map<juce::String, SilentMove> silentMoves;
    std::map<juce::String, PutBackWatch> putBackWatch;     // move key -> an app put back, until it plays elsewhere
    std::set<juce::String> silentLeftToUser;               // move keys: the user changed the device after a move, not moved again
    std::map<juce::String, CaptureFailure> silentFailures; // failed moves per move key since the last configuration change
    uint64_t silentRetrySeen = 0, silentGenerationSeen = 0;
    OutputEndpoint silentChoiceSeen; // the choice of the last pass (a change re-picks the automatic device)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppRouting)
};
} // namespace flub::app
