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
//             AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK (Win10 20348+);
//             EcoQoS opt-out via SetProcessInformation(ProcessPowerThrottling);
//             MMCSS "Pro Audio" for the audio thread.
//   macOS   : Carbon RegisterEventHotKey. Per-app capture and routing are
//             designed, not implemented (isSupported() == false; roadmap
//             3.2): capture via Core Audio process taps (CATapDescription,
//             macOS 14.2+), routing = tap with mute-when-tapped.
//   Linux   : per-app routing by moving PipeWire/Pulse sink-inputs to the
//             "flubsound_<strip>" null sinks (pactl). Global hotkeys via
//             XGrabKey under X11 (libX11 loaded at run time) and, in Wayland
//             sessions, via the xdg-desktop-portal GlobalShortcuts interface
//             over D-Bus (libdbus-1 loaded at run time; isSupported() ==
//             false without the portal). Callbacks come from the service's
//             own event thread. No per-process capture: apps are routed into
//             the null sinks.
//
// Start with the OS (AutoStart): Windows HKCU\...\CurrentVersion\Run value;
// macOS SMAppService.mainAppService (macOS 13+, unsupported on older
// systems); Linux an XDG autostart entry
// ($XDG_CONFIG_HOME/autostart/flubsound-pro.desktop).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
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
    virtual ~GlobalHotkeys() = default;
    virtual bool isSupported() const = 0;

    /** Registers a system-wide shortcut. On Windows and macOS the callback is
        invoked on the thread that created the service (the app's message
        thread); on Linux it runs on the service's own X event / D-Bus
        thread, so callers must hop to their own thread (HotkeyManager does).
        Returns false if the chord is invalid (see KeyChord), cannot be
        mapped on this system, or is taken by another application.
        Linux, Wayland (GlobalShortcuts portal): binding is asynchronous and
        the desktop may ask the user, who can choose another key or decline.
        true means the chord was requested; a refusal found later is logged
        to stderr, not reported here. Changes made within 50 ms are bound
        together as one portal session. */
    virtual bool registerHotkey (int id, const KeyChord& chord, std::function<void()> callback) = 0;
    virtual void unregisterHotkey (int id) = 0;
    virtual void unregisterAll() = 0;

    static std::unique_ptr<GlobalHotkeys> create();
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
    engine exposes one virtual endpoint per strip ("Flubsound Game", ...). */
class AppAudioRouter
{
public:
    virtual ~AppAudioRouter() = default;
    virtual bool isSupported() const = 0;

    virtual std::vector<AudioSessionInfo> enumerateSessions() = 0;

    /** Route an application (by process id / exe) to an output endpoint;
        empty endpointId restores the system default. */
    virtual bool setAppEndpoint (uint32_t processId, const std::string& endpointId, std::string& error) = 0;

    /** Opens the OS's own per-app device UI (fallback when unsupported). */
    virtual void openSystemRoutingSettings() = 0;

    static std::unique_ptr<AppAudioRouter> create();
};

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
/** Process / thread tuning for glitch-free audio. */
struct SystemTuning
{
    /** Opt the process out of Windows EcoQoS / power throttling so the audio
        and capture threads are not parked on efficiency cores. */
    static bool disablePowerThrottling();

    /** Promote the calling thread for audio (MMCSS "Pro Audio" on Windows,
        time-constraint policy on macOS, SCHED_FIFO on Linux where
        RLIMIT_RTPRIO allows it; RealtimeKit is not used).
        Returns an opaque handle to pass to revertAudioThread (may be null). */
    static void* promoteAudioThread();
    static void revertAudioThread (void* handle);
};
} // namespace flub::platform
