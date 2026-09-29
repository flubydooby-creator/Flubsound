// Flubsound Pro - OS integration services used by the desktop app.
//
// Plain C++ interfaces (no JUCE types) so the platform code can be compiled
// and tested in isolation. Each OS provides an implementation; unsupported
// features return isSupported() == false and the UI hides/greys them.
//
//   Windows : RegisterHotKey on a message-only window; per-app routing via
//             the (undocumented, version-dependent) IAudioPolicyConfig
//             "persisted default endpoint" API, compiled in only with
//             FLUB_ENABLE_UNDOCUMENTED_ROUTING (otherwise each move fails
//             and points to the documented fallback, ms-settings:apps-volume);
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
//             the null sinks.
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
};

/** Per-application routing: which output endpoint an app renders to. The
    engine exposes one virtual endpoint per strip ("Flubsound Game", ...).
    Two separate capabilities: listing the apps that play audio (canList)
    and moving one to another endpoint (canMoveEndpoint). Windows lists
    everywhere but moves only in builds with FLUB_ENABLE_UNDOCUMENTED_ROUTING,
    so a caller that needs moves must ask canMoveEndpoint(), not canList(). */
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
} // namespace flub::platform
