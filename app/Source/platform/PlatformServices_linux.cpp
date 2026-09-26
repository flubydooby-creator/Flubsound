// Flubsound Pro - Linux implementation of PlatformServices.h
//
// Linux audio model (see platform/linux/README.md): every strip owns a
// PipeWire/PulseAudio null sink ("Flubsound Game", "Flubsound Music", ...).
// Applications are routed by moving their sink-inputs to those sinks; the
// engine reads each sink's monitor, processes it and plays the result on the
// real device. Per-process loopback capture is therefore not needed.
//
// Dependencies: flub_core (flub::json) and the C library only. Routing shells
// out to `pactl` (pulseaudio-utils >= 16 or pipewire-pulse's pactl), which
// works identically against PulseAudio and PipeWire's pulse server.
//
// Threading: AppAudioRouter calls block while pactl runs (typically 5-30 ms);
// call them from a background thread if that matters. SystemTuning must be
// called on the thread it tunes.
#if defined(__linux__)

#include "PlatformServices.h"
#include "PlatformServicesInternal.h"

#include "flub/io/Json.h"

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace flub::platform
{
namespace
{
//==============================================================================
// pactl helpers (unnamed namespace; tests reach them by including this file)
//==============================================================================
namespace pactl
{
/** Upper bound for captured command output (a list of a few hundred streams
    is ~1 MB of JSON); protects against a runaway child. */
constexpr size_t kMaxOutputBytes = 16u * 1024u * 1024u;

/** Sink names/indices we are willing to put on a shell command line.
    Whitelist only - PulseAudio/PipeWire node names use [A-Za-z0-9_.:@+-]
    (e.g. "alsa_output.pci-0000_00_1f.3.analog-stereo", "@DEFAULT_SINK@",
    "bluez_output.AA_BB_CC_DD_EE_FF.1"). A leading '-' is rejected so the
    value can never be parsed by pactl as an option. */
bool isSafeToken (const std::string& token)
{
    if (token.empty() || token.size() > 255 || token.front() == '-')
        return false;

    return std::all_of (token.begin(),
                        token.end(),
                        [] (char c)
                        {
                            const bool alphanumeric = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
                            return alphanumeric || c == '_' || c == '.' || c == ':' || c == '@' || c == '+' || c == '-';
                        });
}

/** Belt and braces on top of isSafeToken(): single-quote for /bin/sh. The
    whitelist already excludes the quote character itself. */
std::string shellQuote (const std::string& safeToken) { return "'" + safeToken + "'"; }

struct CommandResult
{
    int exitCode = -1; // -1: could not run / killed by a signal
    std::string output;
};

/** Runs a fixed, fully sanitised command line through popen. Never pass
    unsanitised input here: every variable part must have gone through
    isSafeToken() + shellQuote() or be a formatted integer. */
CommandResult run (const std::string& commandLine)
{
    CommandResult result;

    // "e" = O_CLOEXEC on the pipe, so other children never inherit it.
    FILE* pipe = ::popen (commandLine.c_str(), "re");
    if (pipe == nullptr)
        return result;

    char buffer[4096];
    size_t bytes = 0;
    while ((bytes = std::fread (buffer, 1, sizeof (buffer), pipe)) > 0)
    {
        if (result.output.size() + bytes <= kMaxOutputBytes)
            result.output.append (buffer, bytes);
    }

    const int status = ::pclose (pipe);
    if (status != -1 && WIFEXITED (status))
        result.exitCode = WEXITSTATUS (status);

    return result;
}

/** Reads an unsigned id that pactl prints either as a JSON number or as a
    string property ("application.process.id": "1234"). */
bool toUInt32 (const json::Value& value, uint32_t& out)
{
    if (value.isNumber())
    {
        const double d = value.asNumber();
        if (! (d >= 0.0 && d <= 4294967295.0) || std::floor (d) != d)
            return false;
        out = static_cast<uint32_t> (d);
        return true;
    }

    if (value.isString())
    {
        const auto& s = value.asString();
        if (s.empty() || s.size() > 10 || ! std::all_of (s.begin(), s.end(), [] (char c) { return c >= '0' && c <= '9'; }))
            return false;
        const unsigned long long v = std::strtoull (s.c_str(), nullptr, 10);
        if (v > 4294967295ull)
            return false;
        out = static_cast<uint32_t> (v);
        return true;
    }

    return false;
}

struct SinkInput
{
    uint32_t index = 0;
    uint32_t sinkIndex = 0;
    uint32_t processId = 0; // 0 = unknown
    bool corked = true;
    std::string binary;
    std::string applicationName;
};

/** Parses `pactl --format=json list sinks` into index -> sink name. */
bool parseSinks (const std::string& text, std::map<uint32_t, std::string>& sinkNames, std::string& error)
{
    json::Value root;
    if (! json::parse (text, root, error))
    {
        error = "Unexpected pactl output (sinks): " + error;
        return false;
    }
    if (! root.isArray())
    {
        error = "Unexpected pactl output (sinks): not a JSON array";
        return false;
    }

    for (const auto& sink : root.asArray())
    {
        uint32_t index = 0;
        if (toUInt32 (sink["index"], index) && sink["name"].isString())
            sinkNames[index] = sink["name"].asString();
    }
    return true;
}

/** Parses `pactl --format=json list sink-inputs`. */
bool parseSinkInputs (const std::string& text, std::vector<SinkInput>& inputs, std::string& error)
{
    json::Value root;
    if (! json::parse (text, root, error))
    {
        error = "Unexpected pactl output (sink-inputs): " + error;
        return false;
    }
    if (! root.isArray())
    {
        error = "Unexpected pactl output (sink-inputs): not a JSON array";
        return false;
    }

    for (const auto& item : root.asArray())
    {
        SinkInput input;
        if (! toUInt32 (item["index"], input.index))
            continue;

        toUInt32 (item["sink"], input.sinkIndex);
        input.corked = item["corked"].asBool (false);

        // Prefer PipeWire's socket-credential pid: the kernel reports it in
        // the sound server's (= our) pid namespace. application.process.id is
        // the client's own getpid(), which for Flatpak/Snap/container apps is
        // a sandbox-local pid (often 2 or 3) - it would collide between
        // sandboxes and name an unrelated host process. Classic PulseAudio
        // only has application.process.id.
        const auto& props = item["properties"];
        if (! toUInt32 (props["pipewire.sec.pid"], input.processId) || input.processId == 0)
        {
            input.processId = 0;
            toUInt32 (props["application.process.id"], input.processId);
        }

        input.binary = props["application.process.binary"].asString();
        input.applicationName = props["application.name"].asString();
        inputs.push_back (std::move (input));
    }
    return true;
}

/** One AudioSessionInfo per process (a process may own several streams; a
    playing one wins). Streams without a pid cannot be addressed through the
    pid-based interface and are skipped, as are our own streams. */
std::vector<AudioSessionInfo> toSessions (const std::vector<SinkInput>& inputs,
                                          const std::map<uint32_t, std::string>& sinkNames,
                                          uint32_t ownPid)
{
    std::vector<AudioSessionInfo> sessions;
    std::map<uint32_t, size_t> indexByPid;

    const auto sinkName = [&sinkNames] (uint32_t index)
    {
        const auto it = sinkNames.find (index);
        return it != sinkNames.end() ? it->second : std::to_string (index);
    };

    for (const auto& input : inputs)
    {
        if (input.processId == 0 || input.processId == ownPid)
            continue;

        const bool active = ! input.corked;

        if (const auto it = indexByPid.find (input.processId); it != indexByPid.end())
        {
            auto& existing = sessions[it->second];
            if (active && ! existing.isActive)
            {
                existing.isActive = true;
                existing.currentEndpointId = sinkName (input.sinkIndex);
            }
            continue;
        }

        AudioSessionInfo info;
        info.processId = input.processId;
        info.executableName = input.binary;
        info.displayName = ! input.applicationName.empty() ? input.applicationName : input.binary;
        info.currentEndpointId = sinkName (input.sinkIndex);
        info.isActive = active;

        indexByPid.emplace (input.processId, sessions.size());
        sessions.push_back (std::move (info));
    }

    return sessions;
}

/** First line of pactl's diagnostic output, for error messages. */
std::string firstLine (const std::string& text)
{
    auto line = text.substr (0, text.find ('\n'));
    if (line.size() > 200)
        line.resize (200);
    return line;
}

bool isExecutableInPath (const char* name)
{
    const char* path = std::getenv ("PATH");
    std::string dirs = path != nullptr ? path : "/usr/local/bin:/usr/bin:/bin";

    size_t start = 0;
    while (start <= dirs.size())
    {
        const size_t end = std::min (dirs.find (':', start), dirs.size());
        const std::string dir = dirs.substr (start, end - start);
        if (! dir.empty() && ::access ((dir + "/" + name).c_str(), X_OK) == 0)
            return true;
        start = end + 1;
    }
    return false;
}
} // namespace pactl

//==============================================================================
// GlobalHotkeys - not implemented on Linux yet
//==============================================================================
/*  There is no single global-shortcut API on Linux:
      - Wayland: grabbing keys is forbidden by design; the sanctioned route is
        the xdg-desktop-portal GlobalShortcuts interface
        (org.freedesktop.portal.GlobalShortcuts: CreateSession, BindShortcuts,
        "Activated" signal) - the compositor asks the user to confirm and
        may let them rebind. Implemented by KDE Plasma 5.27+, GNOME 48+,
        Hyprland; needs a D-Bus client (sd-bus/GDBus) and an app id.
      - X11: XGrabKey on the root window for every chord x {NumLock,
        CapsLock} mask combination, fed from JUCE's X11 event loop.
    Until one of these ships, isSupported() == false and the UI tells users
    to bind the actions in their desktop's keyboard settings instead. */
class LinuxGlobalHotkeys final : public GlobalHotkeys
{
public:
    bool isSupported() const override { return false; }
    bool registerHotkey (int, const KeyChord&, std::function<void()>) override { return false; }
    void unregisterHotkey (int) override {}
    void unregisterAll() override {}
};

//==============================================================================
// AppAudioRouter - pactl (PulseAudio / PipeWire-pulse)
//==============================================================================
class LinuxAppAudioRouter final : public AppAudioRouter
{
public:
    LinuxAppAudioRouter() : havePactl (pactl::isExecutableInPath ("pactl")) {}

    bool isSupported() const override { return havePactl; }

    std::vector<AudioSessionInfo> enumerateSessions() override
    {
        if (! havePactl)
            return {};

        std::string error;
        std::map<uint32_t, std::string> sinkNames;
        std::vector<pactl::SinkInput> inputs;

        const auto sinks = pactl::run ("LC_ALL=C pactl --format=json list sinks 2>/dev/null");
        if (sinks.exitCode == 0)
            pactl::parseSinks (sinks.output, sinkNames, error); // names are cosmetic; indices still work

        if (! listSinkInputs (inputs, error))
            return {};

        return pactl::toSessions (inputs, sinkNames, static_cast<uint32_t> (::getpid()));
    }

    /** Moves every current stream of the process to 'endpointId' (a sink name
        such as "flubsound_game", or a sink index); empty = the default sink.
        This affects running streams; PipeWire/WirePlumber (restore-stream)
        and PulseAudio (module-stream-restore) remember the choice for the
        application's future streams. See platform/linux for a rule file
        that pre-seeds routes without a running stream. */
    bool setAppEndpoint (uint32_t processId, const std::string& endpointId, std::string& error) override
    {
        if (! havePactl)
        {
            error = "pactl was not found. Install pulseaudio-utils (it also works with PipeWire).";
            return false;
        }

        if (processId == 0)
        {
            error = "Invalid process id.";
            return false;
        }

        if (processId == static_cast<uint32_t> (::getpid()))
        {
            error = "Flubsound's own output cannot be routed into its virtual devices (feedback loop).";
            return false;
        }

        const std::string target = endpointId.empty() ? std::string ("@DEFAULT_SINK@") : endpointId;
        if (! pactl::isSafeToken (target))
        {
            error = "Invalid output device name '" + endpointId + "'.";
            return false;
        }

        std::vector<pactl::SinkInput> inputs;
        if (! listSinkInputs (inputs, error))
            return false;

        int moved = 0;
        for (const auto& input : inputs)
        {
            if (input.processId != processId)
                continue;

            // Every variable part is an integer or a whitelisted, quoted token.
            const auto command =
                "LC_ALL=C pactl move-sink-input " + std::to_string (input.index) + " " + pactl::shellQuote (target) + " 2>&1";
            const auto result = pactl::run (command);
            if (result.exitCode != 0)
            {
                error = "pactl could not move the stream to '" + target + "': " + pactl::firstLine (result.output);
                return false;
            }
            ++moved;
        }

        if (moved == 0)
        {
            error = "Process " + std::to_string (processId)
                  + " is not playing audio right now. Start playback in the application and try again.";
            return false;
        }

        return true;
    }

    void openSystemRoutingSettings() override
    {
        // Constant command line (no user input). pavucontrol's tab 1 is
        // "Playback", which has a per-stream output selector. The trailing '&'
        // backgrounds the tool inside the shell, so system() returns at once
        // and the child is re-parented to init (no zombie).
        static constexpr const char* command =
            "( if command -v pavucontrol >/dev/null 2>&1; then exec pavucontrol --tab=1; "
            "elif command -v pwvucontrol >/dev/null 2>&1; then exec pwvucontrol; "
            "elif command -v gnome-control-center >/dev/null 2>&1; then exec gnome-control-center sound; "
            "elif command -v systemsettings >/dev/null 2>&1; then exec systemsettings kcm_pulseaudio; fi ) >/dev/null 2>&1 &";

        const int status = std::system (command);
        (void) status;
    }

private:
    static bool listSinkInputs (std::vector<pactl::SinkInput>& inputs, std::string& error)
    {
        // stderr is discarded so warnings can never corrupt the JSON on stdout.
        const auto result = pactl::run ("LC_ALL=C pactl --format=json list sink-inputs 2>/dev/null");
        if (result.exitCode == 0)
            return pactl::parseSinkInputs (result.output, inputs, error);

        // Tell "no server" apart from "pactl too old for --format=json".
        const auto info = pactl::run ("LC_ALL=C pactl info 2>&1");
        if (info.exitCode != 0)
            error = "Cannot reach the sound server (is PipeWire/pipewire-pulse or PulseAudio running?): " + pactl::firstLine (info.output);
        else
            error = "This pactl cannot print JSON; pactl 16 or newer (pulseaudio-utils 16+) is required.";
        return false;
    }

    const bool havePactl;
};

//==============================================================================
// ProcessLoopbackCapture - not applicable on Linux
//==============================================================================
/*  PipeWire could capture a single application's stream (link a capture
    stream to the app's output node, target.object = its node id), but the
    Linux design routes applications into per-strip null sinks instead and
    reads the sinks' monitors - one mechanism, persistent, visible in every
    mixer. So this reports isSupported() == false. */
class LinuxProcessLoopbackCapture final : public ProcessLoopbackCapture
{
public:
    bool isSupported() const override { return false; }

    bool start (uint32_t, bool, double, int, FrameCallback, std::string& error) override
    {
        error = "Per-application capture is not used on Linux: route the application to a Flubsound sink instead.";
        return false;
    }

    void stop() override {}
    bool isRunning() const override { return false; }
};

//==============================================================================
/** Saved scheduling state, owned by the handle promoteAudioThread returns. */
struct SavedSchedulingPolicy
{
    int policy = SCHED_OTHER;
    sched_param param {};
};

/** Same ceiling rtkit grants by default and that PipeWire's data thread runs
    at under rtkit, so a promoted Flubsound thread never out-ranks the sound
    server it feeds. */
constexpr int kPreferredFifoPriority = 20;
} // namespace

//==============================================================================
// SystemTuning
//==============================================================================
bool SystemTuning::disablePowerThrottling()
{
    // Linux has no per-process EcoQoS equivalent to opt out of: CPU frequency
    // is governed system-wide (cpufreq governor / power-profiles-daemon) and a
    // SCHED_FIFO audio thread already gets the scheduling it needs.
    return true;
}

void* SystemTuning::promoteAudioThread()
{
    /*  Best effort SCHED_FIFO. The kernel allows it when the user has an
        RLIMIT_RTPRIO allowance (e.g. "@audio - rtprio 95" in limits.conf, the
        'realtime' group, or CAP_SYS_NICE); otherwise it fails with EPERM and
        we silently stay at SCHED_OTHER. Desktop sessions without such limits
        would need RealtimeKit (org.freedesktop.RealtimeKit1.MakeThreadRealtime
        over the system D-Bus) - not wired up here to avoid a D-Bus dependency;
        when Flubsound runs as a JACK / PipeWire client the server already
        calls our process callback on its own real-time thread.

        SCHED_RESET_ON_FORK keeps children (e.g. pactl started via popen) from
        inheriting real-time priority; rtkit requires it as well. */
    const pthread_t self = ::pthread_self();

    auto saved = std::make_unique<SavedSchedulingPolicy>();
    if (::pthread_getschedparam (self, &saved->policy, &saved->param) != 0)
        return nullptr;

    const int minPriority = ::sched_get_priority_min (SCHED_FIFO);
    const int maxPriority = ::sched_get_priority_max (SCHED_FIFO);
    if (minPriority < 0 || maxPriority < minPriority)
        return nullptr;

    const auto tryPriority = [self, minPriority, maxPriority] (int priority)
    {
        sched_param param {};
        param.sched_priority = std::clamp (priority, minPriority, maxPriority);
        return ::pthread_setschedparam (self, SCHED_FIFO | SCHED_RESET_ON_FORK, &param) == 0;
    };

    bool promoted = tryPriority (kPreferredFifoPriority);

    if (! promoted)
    {
        // Allowed, but only up to a lower RLIMIT_RTPRIO? Retry at that ceiling.
        rlimit limit {};
        if (::getrlimit (RLIMIT_RTPRIO, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY && limit.rlim_cur > 0
            && limit.rlim_cur < static_cast<rlim_t> (kPreferredFifoPriority))
            promoted = tryPriority (static_cast<int> (limit.rlim_cur));
    }

    return promoted ? saved.release() : nullptr;
}

void SystemTuning::revertAudioThread (void* handle)
{
    if (handle == nullptr)
        return;

    const std::unique_ptr<SavedSchedulingPolicy> saved (static_cast<SavedSchedulingPolicy*> (handle));
    const pthread_t self = ::pthread_self();

    // Exact restore first. Without CAP_SYS_NICE the kernel refuses to CLEAR
    // SCHED_RESET_ON_FORK once set (sched(7)), i.e. a desktop user promoted via
    // RLIMIT_RTPRIO gets EPERM here and would stay SCHED_FIFO forever. Keeping
    // the flag is harmless for a normal-policy thread, so retry with it.
    if (::pthread_setschedparam (self, saved->policy, &saved->param) != 0)
        ::pthread_setschedparam (self, saved->policy | SCHED_RESET_ON_FORK, &saved->param);
}

//==============================================================================
std::unique_ptr<GlobalHotkeys> GlobalHotkeys::create() { return std::make_unique<LinuxGlobalHotkeys>(); }
std::unique_ptr<AppAudioRouter> AppAudioRouter::create() { return std::make_unique<LinuxAppAudioRouter>(); }
std::unique_ptr<ProcessLoopbackCapture> ProcessLoopbackCapture::create() { return std::make_unique<LinuxProcessLoopbackCapture>(); }
} // namespace flub::platform

#endif // __linux__
