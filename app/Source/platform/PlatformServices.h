// Flubsound Pro - OS integration services used by the desktop app.
//
// Plain C++ interfaces (no JUCE types) so the platform code can be compiled
// and tested in isolation. Each OS provides an implementation; unsupported
// features return isSupported() == false and the UI hides/greys them.
//
//   Windows : RegisterHotKey on a message-only window; per-app device moves
//             via the (undocumented, version-dependent) IAudioPolicyConfig
//             "persisted default endpoint" API: in every build for moving a
//             captured app's own output away (canMoveAppOutput, used only
//             once the user switched that on), and for endpoint routing to
//             strip endpoints only with FLUB_ENABLE_UNDOCUMENTED_ROUTING
//             (canMoveEndpoint; otherwise the UI points to the documented
//             fallback, ms-settings:apps-volume);
//             per-process capture via
//             AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK (Windows 10 2004,
//             build 19041+, and Windows 11; see windows_builds below);
//             EcoQoS opt-out via SetProcessInformation(ProcessPowerThrottling);
//             MMCSS "Pro Audio" for the audio thread.
//   macOS   : Carbon RegisterEventHotKey. Per-app capture and routing are
//             designed, not implemented (isSupported() == false; roadmap
//             3.2): capture via Core Audio process taps (CATapDescription,
//             macOS 14.2+), routing = tap with mute-when-tapped.
//   Linux   : per-app routing by moving PipeWire/Pulse sink-inputs to the
//             "flubsound_<strip>" null sinks (pactl); the sinks' monitors
//             are linked to the engine's input (pw-dump / pw-link), and
//             PIPEWIRE_LATENCY asks for a 256/48000 quantum. Global hotkeys via
//             XGrabKey under X11 (libX11 loaded at run time) and, in Wayland
//             sessions, via the xdg-desktop-portal GlobalShortcuts interface
//             over D-Bus (libdbus-1 loaded at run time; isSupported() ==
//             false without the portal). Callbacks come from the service's
//             own event thread. No per-process capture: apps are routed into
//             the null sinks. Real-time audio thread through RealtimeKit on
//             the system bus (same run-time libdbus-1) when the thread's own
//             SCHED_FIFO attempt is refused; ALSA card capture channel maps
//             through libasound, also loaded at run time.
//
// Foreground application (ForegroundApp, for automatic profile switching):
// Windows GetForegroundWindow + QueryFullProcessImageNameW; macOS
// NSWorkspace.frontmostApplication; Linux X11 _NET_ACTIVE_WINDOW +
// _NET_WM_PID (libX11 loaded at run time), unsupported under Wayland.
//
// Start with the OS (AutoStart): Windows HKCU\...\CurrentVersion\Run value;
// macOS SMAppService.mainAppService (macOS 13+, unsupported on older
// systems); Linux an XDG autostart entry
// ($XDG_CONFIG_HOME/autostart/flubsound-pro.desktop).
#pragma once

#include "flub/common/Realtime.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace flub::platform
{
// ---------------------------------------------------------------------------
struct KeyChord
{
    enum Modifier : uint32_t
    {
        None = 0,
        Ctrl = 1u << 0,
        Alt = 1u << 1,
        Shift = 1u << 2,
        Super = 1u << 3 // Win / Cmd
    };
    uint32_t modifiers = None;
    uint32_t keyCode = 0; // VK-style: ASCII upper-case letter/digit, F1..F24 = 0x70 + n,
                          // Space 0x20, PageUp/PageDown/End/Home 0x21..0x24,
                          // Left/Up/Right/Down 0x25..0x28, Insert 0x2D, Delete 0x2E

    std::string toString() const;
};

class GlobalHotkeys
{
public:
    /** What became of one registration, as the system finally reports it. */
    struct BindingResult
    {
        enum class Status : uint8_t
        {
            Registered,  // active with the requested chord
            Reassigned,  // active, but the desktop bound another key: see 'trigger'
            Unavailable, // not active: invalid or unmappable chord, taken by
                         // another application, or no working service
            Declined     // the desktop, or the user in its dialog, declined it
                         // (Wayland GlobalShortcuts portal)
        };

        int id = 0;
        Status status = Status::Unavailable;
        std::string trigger; // Reassigned: the desktop's own description of the
                             // key it bound (its trigger_description), as sent

        bool operator== (const BindingResult&) const = default;
    };

    using BindingListener = std::function<void (const BindingResult&)>;

    virtual ~GlobalHotkeys() = default;
    virtual bool isSupported() const = 0;

    /** Registers a system-wide shortcut. On Windows and macOS the callback is
        invoked on the thread that created the service (the app's message
        thread); on Linux it runs on the service's own X event / D-Bus
        thread, so callers must hop to their own thread (HotkeyManager does).
        Returns false if the chord is invalid (see KeyChord), cannot be
        mapped on this system, or is taken by another application.
        'description' names the action ("Boost +10%") where the desktop lists
        the shortcut (Wayland portal dialog and settings, as "Flubsound Pro:
        <description>"); empty = the chord's name. Windows and macOS have no
        such list and ignore it.
        Linux, Wayland (GlobalShortcuts portal): binding is asynchronous and
        the desktop may ask the user, who can choose another key or decline.
        true means the chord was requested; the outcome arrives later through
        the binding listener. Changes made within 50 ms are bound together as
        one portal session. */
    virtual bool registerHotkey (int id, const KeyChord& chord, const std::string& description, std::function<void()> callback) = 0;

    /** The same without a description (the chord's name is shown). */
    bool registerHotkey (int id, const KeyChord& chord, std::function<void()> callback)
    {
        return registerHotkey (id, chord, std::string(), std::move (callback));
    }

    virtual void unregisterHotkey (int id) = 0;
    virtual void unregisterAll() = 0;

    /** Receives the outcome of every registration (nullptr = none). Every
        registerHotkey() that returns false is reported as Unavailable on the
        calling thread before it returns; so are the synchronous results of
        Windows, macOS and X11 (Registered / Unavailable). The Wayland portal
        reports each registered id from its D-Bus thread once the desktop has
        answered the batch it belongs to (Registered, Reassigned or Declined;
        Unavailable after portal errors), also when a batch needed no new
        binding (the previous outcome again), and again if the desktop later
        closes the session (Declined) or the portal goes away (Unavailable).
        Called without the service's locks held; the listener must not
        destroy the service. Set it before registering. */
    void setBindingListener (BindingListener listener)
    {
        const std::lock_guard<std::mutex> guard (listenerMutex);
        bindingListener = std::move (listener);
    }

    static std::unique_ptr<GlobalHotkeys> create();

protected:
    /** For implementations: passes one result to the listener, if any. */
    void reportBinding (int id, BindingResult::Status status, const std::string& trigger = {}) const
    {
        BindingListener listener;
        {
            const std::lock_guard<std::mutex> guard (listenerMutex);
            listener = bindingListener;
        }
        if (listener)
            listener (BindingResult { id, status, trigger });
    }

private:
    mutable std::mutex listenerMutex;
    BindingListener bindingListener;
};

// ---------------------------------------------------------------------------
struct AudioSessionInfo
{
    uint32_t processId = 0;
    std::string executableName; // e.g. "cs2.exe", "Spotify.exe"
    std::string displayName;    // friendly name when available
    std::string currentEndpointId;
    bool isActive = false;      // currently producing audio
    // docs/11 E47 (route journal, doubling guard); empty / 0 where the OS
    // implementation does not fill them (Linux, macOS).
    std::string executablePath;          // absolute path of the process image (Windows)
    uint64_t processStartTime = 0;       // process creation time in OS units (Windows FILETIME, 100 ns); 0 = unknown.
                                         // With processId it names one process: a reused pid has another start time.
    std::vector<std::string> activeEndpointIds; // every endpoint the process has an active session on (Windows); empty = only currentEndpointId known
};

/** Per-application routing: which output endpoint an app renders to. The
    engine exposes one virtual endpoint per strip ("Flubsound Game", ...).
    Separate capabilities: listing the apps that play audio (canList),
    moving one to a strip endpoint (canMoveEndpoint) and moving an app's own
    output to another physical endpoint (canMoveAppOutput). Windows lists
    everywhere but routes to strip endpoints only in builds with
    FLUB_ENABLE_UNDOCUMENTED_ROUTING, so a caller that needs endpoint routing
    must ask canMoveEndpoint(), not canList(). */
class AppAudioRouter
{
public:
    virtual ~AppAudioRouter() = default;

    /** Per-app routing works end to end: listing and moving. */
    virtual bool isSupported() const = 0;

    /** enumerateSessions() reports the running audio applications. The
        default is isSupported() (an OS that does both or neither). */
    virtual bool canList() const { return isSupported(); }

    /** setAppEndpoint() can move an application in this build on this
        system (it may still fail for one app, e.g. access denied). */
    virtual bool canMoveEndpoint() const { return isSupported(); }

    /** A user-presentable reason when canMoveEndpoint() == false (what the
        user can do instead); empty = none known. */
    virtual std::string cannotMoveReason() const { return {}; }

    virtual std::vector<AudioSessionInfo> enumerateSessions() = 0;

    /** Route an application (by process id / exe) to an output endpoint;
        empty endpointId restores the system default. */
    virtual bool setAppEndpoint (uint32_t processId, const std::string& endpointId, std::string& error) = 0;

    /** docs/11 E47 (R4.5): setAppEndpoint() can move an application's own
        output to another physical output endpoint of listOutputEndpoints()
        and back, the doubling guard's automatic fix ("Move the app's own
        sound away"). Separate from canMoveEndpoint(): Windows moves apps in
        every build (the undocumented per-app device API, used only after the
        user switched that option on), while endpoint routing to strip
        endpoints stays opt-in (FLUB_ENABLE_UNDOCUMENTED_ROUTING) and needs
        the virtual driver. Default: false. Only the Windows router says
        true: its per-app device is kept per executable path and "" means
        the system default, which AppRouting's moves and put-backs rely on;
        a Linux stream move (pactl) does not work that way. */
    virtual bool canMoveAppOutput() const { return false; }

    /** docs/11 E47 (R4.5): the output endpoint the OS keeps for the
        application of processId (Windows: the per-app device of Settings >
        Sound > Volume mixer, persisted per executable path), as the id
        listOutputEndpoints() uses, or empty when the app follows the system
        default. false = unknown (error says why; the default: not
        supported). Blocks like setAppEndpoint (background thread). */
    virtual bool getAppEndpoint (uint32_t /*processId*/, std::string& endpointId, std::string& error)
    {
        endpointId.clear();
        error = "The per-app output device cannot be read on this system.";
        return false;
    }

    /** Windows names a per-app device by its device interface path
        ("\\?\SWD#MMDEVAPI#{0.0.0.00000000}.{guid}#{e6327cad-...}"); this gives
        the MMDevice id inside it ("{0.0.0.00000000}.{guid}"), the id
        listOutputEndpoints() and the sessions use. Text that is not such a
        path is returned unchanged. Pure. */
    static std::string endpointIdFromInterfacePath (const std::string& path);

    /** Opens the OS's own per-app device UI (fallback when unsupported). */
    virtual void openSystemRoutingSettings() = 0;

    /** One strip's endpoint and the first device-input channel that feeds
        the strip (-1: the strip does not read the device input). */
    struct EndpointInput
    {
        std::string endpointId;
        int firstInputChannel = -1;

        bool operator== (const EndpointInput&) const = default;
    };

    /** Connects each endpoint's capture side to the engine's device input at
        the strip's channel, where the OS does not do that by itself (Linux:
        the flubsound_<strip> null sink's monitor ports to Flubsound's own
        PipeWire input ports, through pw-dump / pw-link). Idempotent and cheap
        to call every pass; blocks like the other calls (background thread).
        false = a connection is missing; 'status' then says why and what to do
        instead (user-presentable). The default has nothing to connect. */
    virtual bool connectEndpointInputs (const std::vector<EndpointInput>& /*inputs*/, std::string& status)
    {
        status.clear();
        return true;
    }

    /** docs/11 E47: one active output endpoint. */
    struct OutputEndpoint
    {
        std::string id;   // the id AudioSessionInfo::currentEndpointId uses (Windows: the MMDevice id)
        std::string name; // the OS's friendly name ("Headset Earphone (Stealth 600X Gen 2 USB)")

        bool operator== (const OutputEndpoint&) const = default;
    };

    /** docs/11 E47: the active output endpoints, the system default first,
        then in the OS's enumeration order (the order JUCE's WASAPI device
        type lists them). Feeds the doubling guard (findOutputEndpointId) and
        its fix (a spare endpoint to move an app to). Blocks like
        enumerateSessions (background thread). Default: none known. */
    virtual std::vector<OutputEndpoint> listOutputEndpoints() { return {}; }

    /** docs/11 E47: the id of the endpoint the audio layer calls deviceName
        (a JUCE output device name), empty when none matches (an ASIO driver,
        a device that went away, a router that lists no endpoints). The
        default matches listOutputEndpoints() with matchOutputDeviceName. */
    virtual std::string findOutputEndpointId (const std::string& deviceName)
    {
        return matchOutputDeviceName (listOutputEndpoints(), deviceName);
    }

    /** JUCE names WASAPI devices by their friendly names in listOutputEndpoints()
        order and appends " (2)", " (3)" ... to later duplicates
        (StringArray::appendNumbersToDuplicates, case-sensitive); this undoes
        that: the id of the endpoint named deviceName, empty if none. Pure. */
    static std::string matchOutputDeviceName (const std::vector<OutputEndpoint>& endpoints, const std::string& deviceName);

    static std::unique_ptr<AppAudioRouter> create();
};

// ---------------------------------------------------------------------------
/** Windows build gates, here (not in PlatformServices_win.cpp) so tests on
    every OS can check them. */
namespace windows_builds
{
/** Per-process loopback capture (ActivateAudioInterfaceAsync with
    AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK). Microsoft documents it from
    build 20348 (Server 2022), but the activation works from Windows 10 2004
    (build 19041) on, which includes 20H2-22H2 (19042-19045): OBS's
    Application Audio Capture gates at 19041 for that reason. A build that
    still refuses it fails the activation or IAudioClient::Initialize, which
    start() reports as a readable error. */
inline constexpr uint32_t kFirstProcessLoopback = 19041;

constexpr bool hasProcessLoopback (uint32_t build) noexcept { return build >= kFirstProcessLoopback; }
} // namespace windows_builds

// ---------------------------------------------------------------------------
/** Captures the audio of one process (tree) as interleaved float frames.
    Frames are delivered on an internal capture thread via the callback, which
    must be RT-safe (typically: push into an SpscRing). */
class ProcessLoopbackCapture
{
public:
    using FrameCallback = std::function<void (const float* interleaved, int numFrames, int numChannels)>;

    virtual ~ProcessLoopbackCapture() = default;
    virtual bool isSupported() const = 0;
    virtual bool start (uint32_t processId, bool includeProcessTree, double sampleRate, int numChannels, FrameCallback callback, std::string& error) = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;

    static std::unique_ptr<ProcessLoopbackCapture> create();
};

// ---------------------------------------------------------------------------
/** How an output endpoint is physically attached. Feeds the headset device
    profiles (flub::device::Connection), e.g. to cap the ceiling on Bluetooth.
    Unknown when the OS cannot tell; callers then fall back to name/format
    heuristics. */
enum class EndpointTransport : uint8_t
{
    Unknown = 0,
    Analog,             // onboard codec / line out / 3.5 mm
    Usb,                // USB Audio Class (incl. 2.4 GHz USB transmitters)
    Bluetooth,          // Bluetooth stereo (A2DP)
    BluetoothHandsFree, // Bluetooth hands-free (HFP/HSP) endpoint
    Hdmi,               // display audio
    Virtual             // virtual / aggregate devices (incl. Flubsound's own)
};

/** docs/11 E32: the OS volume of an output endpoint, which the loudness
    contour follows (ProcessingChain::setListeningLevelDb gets volumeDb minus
    the volume the user set as the reference). */
struct EndpointVolume
{
    static constexpr float kSilentDb = -96.0f; // a 0 % (-inf dB) volume reads as this

    bool known = false;    // read; false: 'error' says why
    float volumeDb = 0.0f; // the endpoint's volume, dB (0 = full scale, above when a Pulse sink is over-amplified; the channels' mean), >= kSilentDb
    bool muted = false;
    std::string error;
};

struct AudioEndpoints
{
    /** Transport of the active OUTPUT endpoint whose name matches the device
        name the audio layer reports (JUCE device name / friendly name).
        Best effort; never throws, returns Unknown when not determinable.
          Windows : PKEY_Device_EnumeratorName (USB / BTHENUM / BTHHFENUM /
                    HDAUDIO ...) + PKEY_AudioEndpoint_FormFactor (HDMI)
          macOS   : kAudioDevicePropertyTransportType
          Linux   : Unknown (heuristics in flub::device::detectConnection) */
    static EndpointTransport queryOutputTransport (const std::string& deviceName);

    /** docs/11 E32: the OS volume of the OUTPUT endpoint the audio layer calls
        deviceName (empty: the default output). Best effort; never throws.
          Windows : IAudioEndpointVolume::GetMasterVolumeLevel / GetMute of
                    the active render endpoint with that friendly name (no
                    match: the default console render endpoint)
          macOS   : kAudioDevicePropertyVolumeDecibels on the output scope
                    (the main element, else the mean of channels 1 and 2) and
                    kAudioDevicePropertyMute of the device with that name (no
                    match: the default output device)
          Linux   : the PipeWire / PulseAudio sink volume through pactl
                    (get-sink-volume, get-sink-mute; pactl 14+ or
                    pipewire-pulse): the sink named deviceName when it is a
                    sink name (it contains a '.', e.g.
                    "alsa_output.usb-...analog-stereo"), else the default sink
                    (@DEFAULT_SINK@), which JUCE's ALSA "pipewire" / "pulse" /
                    "default" devices play to. An ALSA hw: card's own mixer is
                    not read.
        A headset's hardware dial is invisible to the OS on many USB and
        wireless headsets, so this is the software part of the playback level.
        Blocking (a pactl child, COM / Core Audio calls): a background or the
        message thread, a few times a second at most, never the audio thread. */
    static EndpointVolume queryOutputVolume (const std::string& deviceName);
};

// ---------------------------------------------------------------------------
/** "Start Flubsound Pro when I sign in". The OS entry is the source of truth:
    isEnabled() reads it every time (the user can also remove it in the OS's
    own start-up settings), it is not a cached flag.
      Windows : value "Flubsound Pro" = "<quoted exe path>" under
                HKCU\Software\Microsoft\Windows\CurrentVersion\Run; an entry
                the user switched off in Task Manager > Startup apps
                (Explorer\StartupApproved\Run) reads as disabled, and enabling
                clears that switch.
      macOS   : SMAppService.mainAppService (Login Items, macOS 13+); the
                running app bundle is registered, executablePath is ignored.
                isSupported() == false on older systems.
      Linux   : $XDG_CONFIG_HOME/autostart/flubsound-pro.desktop (default
                ~/.config), written atomically. A file with Hidden=true or
                X-GNOME-Autostart-enabled=false reads as disabled.
    Blocking file / registry IO: call from the message thread on user action,
    never from the audio thread. */
class AutoStart
{
public:
    virtual ~AutoStart() = default;
    virtual bool isSupported() const = 0;

    /** True when the OS will start the app at the next sign-in. */
    virtual bool isEnabled() const = 0;

    /** Adds (true) or removes (false) the start-up entry; idempotent.
        executablePath: absolute UTF-8 path of the program to start; empty =
        the running executable (Linux: $APPIMAGE when run from an AppImage).
        Returns false and sets a user-presentable 'error' on failure. */
    virtual bool setEnabled (bool shouldStart, const std::string& executablePath, std::string& error) = 0;

    static std::unique_ptr<AutoStart> create();
};

// ---------------------------------------------------------------------------
/** The application whose window has the keyboard focus. */
struct ForegroundAppInfo
{
    uint32_t processId = 0;
    std::string executablePath; // absolute UTF-8 path when known ("C:\\Games\\cs2.exe", "/usr/bin/foo")
    std::string executableName; // file name ("cs2.exe", "foo"); never empty after a successful query
    std::string bundleId;       // macOS bundle identifier ("com.spotify.client"); empty elsewhere
    bool isThisProcess = false; // Flubsound's own window is in the foreground

    bool operator== (const ForegroundAppInfo&) const = default;
};

/** "Which application is in the foreground?" - polled by the app's message
    thread (EngineController, 2 Hz) for automatic profile switching. Each
    query is a few cheap system calls; the process path is cached while the
    same window / process stays in front.
      Windows : GetForegroundWindow -> GetWindowThreadProcessId ->
                OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION) +
                QueryFullProcessImageNameW. A UWP app's frame window belongs
                to ApplicationFrameHost.exe; its hosted child's process is
                reported instead.
      macOS   : NSWorkspace.sharedWorkspace.frontmostApplication
                (processIdentifier, executableURL, bundleIdentifier). Needs no
                permission. Main thread only.
      Linux   : X11: _NET_ACTIVE_WINDOW on the root window (set by EWMH
                window managers) -> the window's _NET_WM_PID ->
                readlink /proc/<pid>/exe. Wine / Proton games report the
                Windows executable (argv[0], e.g. "cs2.exe") instead of the
                wine loader. libX11 is loaded at run time. Wayland has no
                portable foreground-window API: isSupported() == false in a
                Wayland session (XWayland only sees X clients), and without
                an X display.
    Create, query and destroy on one thread (the message thread). */
class ForegroundApp
{
public:
    virtual ~ForegroundApp() = default;
    virtual bool isSupported() const = 0;

    /** Fills 'info' with the current foreground application. false when
        there is none or it cannot be determined (no focused window, the
        desktop, a process we may not inspect, unsupported system). */
    virtual bool query (ForegroundAppInfo& info) = 0;

    /** A user-presentable reason when isSupported() == false; empty otherwise. */
    virtual std::string unsupportedReason() const = 0;

    static std::unique_ptr<ForegroundApp> create();
};

// ---------------------------------------------------------------------------
/** Process / thread tuning for glitch-free audio. */
struct SystemTuning
{
    /** Opt the process out of Windows EcoQoS / power throttling so the audio
        and capture threads are not parked on efficiency cores. */
    static bool disablePowerThrottling();

    /** Promote the calling thread for audio (MMCSS "Pro Audio" on Windows,
        time-constraint policy on macOS, SCHED_FIFO on Linux where
        RLIMIT_RTPRIO allows it; RealtimeKit is asked separately, from
        another thread: RealtimeScheduling below).
        Returns an opaque handle to pass to revertAudioThread (may be null).
        Linux: no allocation (the saved policy lives in the thread's own
        storage), so the handle must be reverted on the same thread. */
    static void* promoteAudioThread();
    static void revertAudioThread (void* handle);
};

// ---------------------------------------------------------------------------
/** docs/11 E44: an audio thread's scheduling, read and requested from
    another thread (the app's message thread). On most Linux desktops the
    user has no RLIMIT_RTPRIO allowance, so promoteAudioThread() leaves the
    thread at SCHED_OTHER; RealtimeKit (org.freedesktop.RealtimeKit1 on the
    system bus, libdbus-1 loaded at run time like the portal hotkeys) grants
    SCHED_RR up to its MaxRealtimePriority instead. Linux only: elsewhere
    currentThreadId() is 0, queryThread() is unknown and requestRealtimeKit()
    Unavailable. */
struct ThreadScheduling
{
    bool known = false;    // the thread exists and its policy could be read
    bool realtime = false; // SCHED_FIFO or SCHED_RR
    int priority = 0;      // its real-time priority (0 when not real-time)
    std::string policy;    // "FIFO", "RR", "OTHER", "BATCH", "IDLE"; empty when unknown
};

struct RealtimeKitResult
{
    enum class Outcome : uint8_t
    {
        Granted,    // RealtimeKit answered MakeThreadRealtime without error
        Refused,    // RealtimeKit answered with an error, or RLIMIT_RTTIME could not be set
        Unavailable // no libdbus-1, no system bus, or no RealtimeKit on it
    };

    Outcome outcome = Outcome::Unavailable;
    int priority = 0;       // the priority asked for: the wanted one, capped at MaxRealtimePriority
    int64_t rttimeUsec = 0; // RLIMIT_RTTIME (hard) in force for the request; 0 = not reached
    std::string message;    // user-presentable when not Granted
};

struct RealtimeScheduling
{
    /** The calling thread's kernel thread id (Linux gettid); 0 elsewhere.
        Audio thread: one system call, no allocation or lock. */
    static uint64_t currentThreadId() noexcept FLUB_NONBLOCKING;

    /** A thread's policy and priority (Linux sched_getscheduler /
        sched_getparam on its id). Any thread. */
    static ThreadScheduling queryThread (uint64_t threadId);

    /** Asks RealtimeKit to make 'threadId' (a thread of this process) real
        time at min(wantedPriority, MaxRealtimePriority). rtkit refuses a
        process whose RLIMIT_RTTIME hard limit is above its RTTimeUSecMax
        (200 ms by default), so the process's soft and hard limits are
        lowered to it first: from then on a real-time thread of this process
        that runs that long without blocking gets SIGXCPU / SIGKILL, rtkit's
        watchdog. Blocking (system bus round trips, 1 s timeout each): call
        from the message thread, never from the audio thread. */
    static RealtimeKitResult requestRealtimeKit (uint64_t threadId, int wantedPriority);
};

// ---------------------------------------------------------------------------
/** docs/11 E27 step 4: speaker positions of a device's input channels. RL /
    RR are the back pair (WAVEFORMATEXTENSIBLE's BL / BR); a 5.1 layout may
    use either RL RR or SL SR for its surround pair. */
enum class SpeakerPosition : uint8_t
{
    Unknown = 0,
    FL,
    FR,
    FC,
    LFE,
    RL,
    RR,
    SL,
    SR
};

struct AudioChannelMaps
{
    /** The speaker position of each of the first 'channels' input channels
        of a device, as its backend reports it; empty = not known here.
          Linux : a JUCE "ALSA" / "ALSA HW" device on a sound card (hw:),
                  the driver's capture channel map for 'channels'
                  (snd_pcm_query_chmaps_from_hw, libasound loaded at run
                  time). ALSA plug-in PCMs (default, pipewire, pulse) and
                  other device types report none here.
          others: none yet (WASAPI mask, CoreAudio AudioChannelLayout).
        Blocking (sound-card control queries): message thread or the thread
        that starts the device, never the audio thread. */
    static std::vector<SpeakerPosition> queryInputPositions (const std::string& deviceTypeName, const std::string& deviceName, int channels);
};

#if ! defined(__linux__)
inline uint64_t RealtimeScheduling::currentThreadId() noexcept FLUB_NONBLOCKING { return 0; }
inline ThreadScheduling RealtimeScheduling::queryThread (uint64_t) { return {}; }
inline RealtimeKitResult RealtimeScheduling::requestRealtimeKit (uint64_t, int)
{
    RealtimeKitResult result;
    result.message = "RealtimeKit exists on Linux only";
    return result;
}
inline std::vector<SpeakerPosition> AudioChannelMaps::queryInputPositions (const std::string&, const std::string&, int) { return {}; }
#endif

// ---------------------------------------------------------------------------
/** docs/11 E48: Flubsound's own PipeWire node (Linux, built when pkg-config
    finds libpipewire-0.3; elsewhere and without it isSupported() == false).
    One pw_filter with PW_FILTER_FLAG_RT_PROCESS: an input port group per
    strip (Game 7.1, Music, Chat, System) and an output port group. The
    flubsound_<strip> null sinks it needs are created by the node when they
    do not exist yet (they vanish with the process, so a crash leaves no
    dead sink behind and the desktop moves their streams to the default
    output). The node links each sink's monitor to its strip's ports and its
    outputs to the output sink itself, through libpipewire registry events
    (no pw-dump polling, no WirePlumber policy needed), and asks for the
    latency profile's quantum as its own node.latency. */
struct NativeAudioNodeConfig
{
    struct Strip
    {
        std::string name;                   // "Game": the strip, and the port names ("game_FL")
        std::string sinkName;               // "flubsound_game": the null sink the strip reads
        std::string description;            // "Flubsound Game": the sink's name in desktop mixers
        std::vector<std::string> positions; // audio.channel of each input port, e.g. FL FR FC LFE RL RR SL SR

        bool operator== (const Strip&) const = default;
    };

    enum class Latency : uint8_t
    {
        Balanced,  // node.latency 256/48000, a request (Quality and Balanced)
        LowLatency // node.latency 128/48000 with node.lock-quantum
    };

    std::vector<Strip> strips;   // empty = Game 7.1, Music, Chat, System (platform/linux's sinks)
    int outputChannels = 2;      // 1..8 output ports (2 = FL FR)
    std::string outputTarget;    // node.name of the sink to play to; empty = the default output ("default" metadata)
    bool createSinks = true;     // create the missing flubsound_<strip> sinks (false: link only what exists)
    Latency latency = Latency::Balanced;
    uint32_t sampleRate = 48000; // node.rate request; the graph may run at another rate (see NativeAudioNodeStatus)
    int maxBlockFrames = 1024;   // largest block the callback gets; a larger quantum is split
    std::string nodeName = "flubsound_engine";
    std::string nodeDescription = "Flubsound Engine";
};

struct NativeAudioNodeStatus
{
    bool running = false;            // connected, the filter exists
    uint32_t nodeId = 0;             // the filter's PipeWire node id; 0 = not yet known
    uint32_t quantumFrames = 0;      // the graph's quantum at the last cycle; 0 = no cycle yet
    uint32_t sampleRate = 0;         // the graph's rate at the last cycle; 0 = no cycle yet
    uint64_t cycles = 0;             // process cycles so far
    int inputLinksWanted = 0;        // sink monitor -> strip port links the plan asks for
    int inputLinksMade = 0;          // ... of which the graph has
    int outputLinksWanted = 0;       // output port -> output sink links
    int outputLinksMade = 0;
    std::string outputSink;          // node.name the outputs go to; empty = none
    std::vector<std::string> createdSinks; // sinks this node created (they go away with it)
    std::string message;             // user-presentable problems; empty when everything is linked
};

class NativeAudioNode
{
public:
    class Callback
    {
    public:
        virtual ~Callback() = default;

        /** Before the first nodeProcess() of a run, on the thread that calls
            start(): prepare for up to maxBlockFrames per call. */
        virtual void nodeStarting (double sampleRate, int maxBlockFrames) = 0;

        /** PipeWire's real-time data thread, once or more per graph cycle
            (a quantum above maxBlockFrames is split). inputs: every strip's
            channels in the config's order (Game FL..SR, Music FL FR, ...),
            outputs: the output channels; never null (a port without a buffer
            reads silence / writes to scratch). No allocation, lock or IO. */
        virtual void nodeProcess (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int numFrames) noexcept FLUB_NONBLOCKING = 0;

        /** After the last nodeProcess(), on the thread that calls stop(). */
        virtual void nodeStopped() = 0;
    };

    virtual ~NativeAudioNode() = default;
    virtual bool isSupported() const = 0;

    /** Why isSupported() == false (user-presentable); empty otherwise. */
    virtual std::string unsupportedReason() const = 0;

    /** Connects to PipeWire, creates the missing sinks and the filter, and
        starts linking. Blocks for server round trips (at most ~2 s): message
        thread. false with a user-presentable 'error' when there is no
        PipeWire server or the filter cannot be created. The callback must
        outlive the run. */
    virtual bool start (const NativeAudioNodeConfig& config, Callback& callback, std::string& error) = 0;

    /** Stops the filter (the callback is not called afterwards), removes the
        links and sinks it made and disconnects. Idempotent. */
    virtual void stop() = 0;

    virtual bool isRunning() const = 0;

    /** Changes node.latency / node.lock-quantum in place while running:
        PipeWire moves the graph to the new quantum between two cycles, with
        no re-open or dropout. */
    virtual bool setLatency (NativeAudioNodeConfig::Latency latency) = 0;

    /** Plays to another sink (empty = the default output) by moving the
        output links. */
    virtual bool setOutputTarget (const std::string& sinkName) = 0;

    /** For the UI (message thread, a few times a second); never the audio
        thread. */
    virtual NativeAudioNodeStatus getStatus() const = 0;

    static std::unique_ptr<NativeAudioNode> create();
};

#if ! defined(__linux__)
class UnsupportedNativeAudioNode final : public NativeAudioNode
{
public:
    bool isSupported() const override { return false; }
    std::string unsupportedReason() const override { return "The native PipeWire node exists on Linux only"; }
    bool start (const NativeAudioNodeConfig&, Callback&, std::string& error) override
    {
        error = unsupportedReason();
        return false;
    }
    void stop() override {}
    bool isRunning() const override { return false; }
    bool setLatency (NativeAudioNodeConfig::Latency) override { return false; }
    bool setOutputTarget (const std::string&) override { return false; }
    NativeAudioNodeStatus getStatus() const override { return {}; }
};

inline std::unique_ptr<NativeAudioNode> NativeAudioNode::create() { return std::make_unique<UnsupportedNativeAudioNode>(); }
#endif

// ---------------------------------------------------------------------------
/** docs/11 E51: what an output endpoint is physically (Windows
    PKEY_AudioEndpoint_FormFactor). The device selection plays the safe
    speaker profile on an unplanned fallback to anything but headphones. */
enum class EndpointFormFactor : uint8_t
{
    Unknown = 0,
    Speakers,
    Headphones,
    Headset,
    LineLevel,
    Digital, // S/PDIF
    Hdmi,    // display audio
    Other
};

/** docs/11 E51: one active output endpoint with what identifies it again
    after it went away and came back. */
struct OutputEndpointIdentity
{
    std::string id;         // the OS's endpoint id (Windows: the IMMDevice id). Stable while the device stays on its
                            // port and across renames; a USB device without a serial number gets a new one on another port
    std::string name;       // its friendly name ("Speakers (2- Stealth 700 Gen 2)"), as JUCE's WASAPI type names it
    std::string hardwareId; // bus, vendor and product ("USB\\VID_10F5&PID_0210&MI_00"): the same on every port; empty when unknown
    EndpointTransport transport = EndpointTransport::Unknown;
    EndpointFormFactor formFactor = EndpointFormFactor::Unknown;
    bool isDefault = false; // the system default (console) output

    bool operator== (const OutputEndpointIdentity&) const = default;
};

/** docs/11 E51: a change of the audio devices or of the power state. */
struct AudioDeviceEvent
{
    enum class Kind : uint8_t
    {
        DefaultOutputChanged, // the system default (console) output is now endpointId (empty: there is none)
        DeviceAdded,
        DeviceRemoved,
        DeviceStateChanged,   // enabled, disabled, unplugged, plugged in (IMMNotificationClient::OnDeviceStateChanged)
        Suspending,           // the system goes to sleep
        Resumed               // the system woke up (the audio devices may take a moment to come back)
    };

    Kind kind = Kind::DeviceAdded;
    std::string endpointId; // the endpoint concerned; empty for power events

    bool operator== (const AudioDeviceEvent&) const = default;
};

/** docs/11 E51: event-driven device handling. The host lists the output
    endpoints with their identities (to recognise its chosen output after a
    re-plug, a rename or another USB port) and hears about hot-plug, default
    device and sleep / resume changes the moment they happen, instead of
    polling.
      Windows : IMMDeviceEnumerator (active render endpoints, the default
                first, JUCE's WASAPI order), PKEY_AudioEndpoint_FormFactor,
                PKEY_Device_EnumeratorName, the adapter's device path through
                IDeviceTopology (vendor / product ids); events through
                IMMNotificationClient (eRender only) and
                PowerRegisterSuspendResumeNotification (powrprof.dll, loaded at
                run time).
      others  : isSupported() == false, listOutputs() empty, start() false:
                JUCE's own device-list notifications and the name-based
                selection remain. */
class AudioDeviceWatcher
{
public:
    using Listener = std::function<void (const AudioDeviceEvent&)>;

    virtual ~AudioDeviceWatcher() = default;
    virtual bool isSupported() const = 0;

    /** The active output endpoints, the system default first, then in the
        OS's enumeration order. Blocking (COM calls, a few ms): the message
        thread on a device event or a device start, never per block and never
        from the listener. */
    virtual std::vector<OutputEndpointIdentity> listOutputs() = 0;

    /** Starts delivering events on the OS's notification threads. The
        listener must only queue the event and return: no blocking, no call
        back into the watcher or the OS audio APIs, no destruction of the
        watcher. false when nothing can be watched. Idempotent. */
    virtual bool start (Listener listener) = 0;

    /** Stops the events; when it returns no listener call is running or
        follows. Idempotent; the destructor calls it. */
    virtual void stop() = 0;

    // ---- Identity helpers (pure; PlatformServices_common.cpp) ------------------
    /** How a remembered endpoint matches a present one. */
    enum class Match : uint8_t
    {
        None,
        Exact,    // the same endpoint id (also after a rename)
        Hardware, // the same vendor / product and the same name without Windows' instance number: re-plugged elsewhere
        Name      // the same name without the instance number (no hardware id on one side)
    };

    /** The friendly name without Windows' instance number, which a second
        instance of the same USB device gets (on another port): "Speakers
        (2- Stealth 700 Gen 2)" -> "Speakers (Stealth 700 Gen 2)", and without
        JUCE's " (2)" duplicate number at the end. Case is kept. */
    static std::string withoutInstanceNumber (const std::string& friendlyName);

    /** remembered = what was saved (its id, name and hardware id; any may be
        empty); candidate = a present endpoint. Names compare ignoring case.
        Different non-empty hardware ids never match. */
    static Match matchIdentity (const OutputEndpointIdentity& remembered, const OutputEndpointIdentity& candidate);

    /** The index of the best match in `endpoints` (Exact over Hardware over
        Name; the first of equals), -1 for none; `how` receives the match. */
    static int findEndpoint (const std::vector<OutputEndpointIdentity>& endpoints, const OutputEndpointIdentity& remembered,
                             Match* how = nullptr);

    /** "USB\VID_10F5&PID_0210&MI_00" from an adapter's device path or
        instance id ("{2}.\\?\usb#vid_10f5&pid_0210&mi_00#7&2b8e&0&0000#{...}",
        "USB\VID_10F5&PID_0210&MI_00\7&2B8E..."), or "BTHENUM\<address>" style
        for Bluetooth (the address is stable); empty when it names no vendor /
        product. Upper case. */
    static std::string hardwareIdFromDevicePath (const std::string& devicePath);

    static std::unique_ptr<AudioDeviceWatcher> create();
};

// ---------------------------------------------------------------------------
/** docs/11 E55: what the session enumeration needs to know about a process,
    looked up once per process instead of once per pass. A game's process is
    then opened (OpenProcess, PROCESS_QUERY_LIMITED_INFORMATION) once for its
    whole life, not every 2 s: anti-cheat drivers log handles to game
    processes, and the open is needless work anyway.
    Keyed by process id plus the session's instance identifier
    (IAudioSessionControl2::GetSessionInstanceIdentifier, which names the
    process and its session): a known key is served from the cache without
    touching the process. A known pid with an unknown key (a new session, or
    a new process that reuses the id) opens the process once and compares
    its creation time: the same process keeps its entry, a new one is
    resolved again. An entry whose pid had no session in a whole pass is
    dropped (endPass). Not thread-safe (the router's enumeration). */
class ProcessInfoCache
{
public:
    struct Info
    {
        std::string executablePath; // absolute path of the image; empty when it could not be read
        std::string executableName; // file name
        std::string description;    // the file's FileDescription (the mixer's name for an app without a session name); empty = none
        uint64_t startTime = 0;     // creation time (Windows FILETIME); 0 = unknown

        bool operator== (const Info&) const = default;
    };

    /** Opens the process once and reads its image path and creation time
        (false: it could not be opened). */
    using ProcessQuery = std::function<bool (uint32_t processId, std::string& executablePath, uint64_t& startTime)>;
    /** Reads the image file's description (file IO, no process handle). */
    using DescriptionQuery = std::function<std::string (const std::string& executablePath)>;

    ProcessInfoCache (ProcessQuery processQuery, DescriptionQuery descriptionQuery);

    /** Starts an enumeration pass. */
    void beginPass();
    /** The process behind a session seen in this pass (sessionKey: its
        instance identifier; empty = unknown, which opens the process on every
        pass). */
    const Info& lookup (uint32_t processId, const std::string& sessionKey);
    /** Ends the pass: drops every process that had no session in it. */
    void endPass();

    /** Process opens (ProcessQuery calls) so far. */
    uint64_t getProcessOpens() const noexcept { return processOpens; }
    size_t size() const noexcept { return entries.size(); }

private:
    struct Entry
    {
        Info info;
        std::vector<std::string> sessionKeys; // instance identifiers already seen for this process
        bool seen = false;                    // in the current pass
    };

    Info resolve (uint32_t processId, const std::string& path, uint64_t startTime);

    ProcessQuery processQuery;
    DescriptionQuery descriptionQuery;
    std::vector<std::pair<uint32_t, Entry>> entries;
    uint64_t processOpens = 0;
    Info empty;
};

/** docs/11 E55: anti-cheat services whose presence can switch Tournament
    mode on by itself. Windows: the services control manager (vgc for
    Vanguard, BEService for BattlEye, EasyAntiCheat / EasyAntiCheat_EOS,
    FACEITService, ESEADriver2); a service counts while it runs. Elsewhere:
    none. Blocking (SCM queries, well under 10 ms): message thread, at most
    every few seconds. */
struct AntiCheatServices
{
    /** The service names that run now (from the list above). */
    static std::vector<std::string> running();
};
} // namespace flub::platform
