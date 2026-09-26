// Flubsound Pro - OS integration services used by the desktop app.
//
// Plain C++ interfaces (no JUCE types) so the platform code can be compiled
// and tested in isolation. Each OS provides an implementation; unsupported
// features return isSupported() == false and the UI hides/greys them.
//
//   Windows : RegisterHotKey on a message-only window; per-app routing via
//             the (undocumented, version-dependent) IAudioPolicyConfig
//             "persisted default endpoint" API with a documented fallback
//             (ms-settings:apps-volume); per-process capture via
//             AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK (Win10 20348+);
//             EcoQoS opt-out via SetProcessInformation(ProcessPowerThrottling);
//             MMCSS "Pro Audio" for the audio thread.
//   macOS   : Carbon RegisterEventHotKey; per-app capture via Core Audio
//             process taps (CATapDescription, macOS 14.2+); routing = tap
//             with mute-when-tapped.
//   Linux   : xdg-desktop-portal GlobalShortcuts (Wayland) / XGrabKey (X11);
//             per-app routing by moving PipeWire/Pulse sink-inputs to the
//             "Flubsound <Strip>" null sinks.
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
    uint32_t keyCode = 0; // ASCII upper-case letter/digit, or F1..F24 as 0x70 + n (VK-style)

    std::string toString() const;
};

class GlobalHotkeys
{
public:
    virtual ~GlobalHotkeys() = default;
    virtual bool isSupported() const = 0;

    /** Registers a system-wide shortcut. The callback is invoked on the
        thread that created the service (the app's message thread). Returns
        false if the chord is taken by another application. */
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
/** Process / thread tuning for glitch-free audio. */
struct SystemTuning
{
    /** Opt the process out of Windows EcoQoS / power throttling so the audio
        and capture threads are not parked on efficiency cores. */
    static bool disablePowerThrottling();

    /** Promote the calling thread for audio (MMCSS "Pro Audio" on Windows,
        time-constraint policy on macOS, SCHED_FIFO / rtkit on Linux).
        Returns an opaque handle to pass to revertAudioThread (may be null). */
    static void* promoteAudioThread();
    static void revertAudioThread (void* handle);
};
} // namespace flub::platform
