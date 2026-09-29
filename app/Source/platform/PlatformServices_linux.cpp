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
// works identically against PulseAudio and PipeWire's pulse server. The
// sinks' monitors are linked to the engine's input with pw-dump / pw-link
// (PipeWire only), and creating the router exports PIPEWIRE_LATENCY so
// Flubsound's streams ask for a 256/48000 quantum. Global
// hotkeys use the X11 headers when present at build time and load libX11
// with dlopen at run time (no link dependency); in Wayland sessions they go
// through the xdg-desktop-portal GlobalShortcuts interface over D-Bus, with
// libdbus-1 loaded the same way (no headers, no link dependency). The
// foreground application (automatic profiles) is read from the X11
// _NET_ACTIVE_WINDOW / _NET_WM_PID properties through the same run-time
// libX11; Wayland sessions report it unsupported.
//
// Start with the OS: an XDG autostart entry (Desktop Application Autostart
// Specification), honoured by GNOME, KDE Plasma, Xfce, Cinnamon, MATE and
// LXQt; bare window managers need a helper such as dex.
//
// Threading: AppAudioRouter calls block while pactl runs (typically 5-30 ms);
// call them from a background thread if that matters. SystemTuning must be
// called on the thread it tunes. GlobalHotkeys callbacks run on the
// service's own X event / D-Bus thread, not on the thread that created it.
// AutoStart does
// small blocking file IO: message thread, on user action. ForegroundApp:
// create, query and destroy on the message thread.
#if defined(__linux__)

#include "PlatformServices.h"
#include "PlatformServicesInternal.h"

#include "flub/io/Json.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

// X11 global hotkeys (see LinuxGlobalHotkeys) and the foreground window
// (LinuxForegroundApp): headers only, libX11 itself is loaded at run time.
// FLUB_NO_X11 forces the header-less build (no global hotkeys) for testing.
#if ! defined(FLUB_NO_X11) && __has_include(<X11/Xlib.h>) && __has_include(<X11/keysym.h>) && __has_include(<X11/Xatom.h>)
    #define FLUB_HAVE_X11_HEADERS 1
    #include <X11/Xatom.h>
    #include <X11/Xlib.h>
    #include <X11/keysym.h>
    // X.h defines None and Xlib.h Status as macros, which would break
    // KeyChord::None and GlobalHotkeys::BindingResult::Status (used below
    // and by tests/test_platform_linux.cpp, which includes this file).
    #undef None
    #undef Status
#else
    #define FLUB_HAVE_X11_HEADERS 0
#endif

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
// PipeWire: quantum request and monitor links (docs/11 E48a; unnamed
// namespace, tests reach the helpers by including this file)
//==============================================================================
namespace pipewire
{
/*  Quantum. PipeWire runs the whole graph in cycles of one quantum, the
    smallest node.latency any running node asks for (within the server's
    clock.min-quantum / clock.max-quantum). Without a request Flubsound's own
    streams get the client library's default, 1024/48000 = 21.3 ms for
    pipewire-jack, and the null sink's monitor hop adds up to one such
    quantum. PIPEWIRE_LATENCY is read when a client opens its stream
    (pipewire-jack, PipeWire's ALSA plug-in), so it must be in the
    environment before the audio device opens. Balanced only asks (other
    clients can ask for less and the graph still follows the smallest
    request); Low Latency also locks the quantum while it runs
    (node.lock-quantum through PIPEWIRE_PROPS). A value the user exported
    is never replaced. */
enum class QuantumRequest
{
    Balanced,  // 256/48000 = 5.3 ms, a request only
    LowLatency // 128/48000 = 2.7 ms, locked while running
};

constexpr const char* kBalancedLatency = "256/48000";
constexpr const char* kLowLatency = "128/48000";
constexpr const char* kLockQuantumProps = "{ node.lock-quantum = true }";

/** "<frames>/<rate>" as PipeWire parses node.latency: frames 1..8192, rate
    8000..768000, digits only. */
bool isValidLatency (const std::string& text)
{
    const auto slash = text.find ('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= text.size() || slash > 4 || text.size() - slash - 1 > 6)
        return false;
    const auto digits = [] (const std::string& s) { return std::all_of (s.begin(), s.end(), [] (char c) { return c >= '0' && c <= '9'; }); };
    const auto frames = text.substr (0, slash), rate = text.substr (slash + 1);
    if (! digits (frames) || ! digits (rate))
        return false;
    const long f = std::strtol (frames.c_str(), nullptr, 10), r = std::strtol (rate.c_str(), nullptr, 10);
    return f >= 1 && f <= 8192 && r >= 8000 && r <= 768000;
}

struct LatencyEnvironment
{
    std::string latency; // value for PIPEWIRE_LATENCY; empty = leave the variable alone
    std::string props;   // value for PIPEWIRE_PROPS; empty = leave the variable alone
};

/** What to export for 'request', given the current values (nullptr = unset).
    A non-empty value the user set wins, valid or not: PipeWire reports a bad
    one itself, and the user asked for it. */
LatencyEnvironment planLatencyEnvironment (QuantumRequest request, const char* currentLatency, const char* currentProps)
{
    LatencyEnvironment plan;
    if (currentLatency != nullptr && *currentLatency != '\0')
        return plan;

    plan.latency = request == QuantumRequest::LowLatency ? kLowLatency : kBalancedLatency;
    if (request == QuantumRequest::LowLatency && (currentProps == nullptr || *currentProps == '\0'))
        plan.props = kLockQuantumProps;
    return plan;
}

/** Exports the plan for this process (and the children it starts). Call it
    before the audio device opens, on the message thread while no other
    thread reads the environment: setenv is not safe against a concurrent
    getenv. overwrite = 0 keeps a value that appeared in between. */
void applyLatencyEnvironment (QuantumRequest request)
{
    const char* const current = std::getenv ("PIPEWIRE_LATENCY");
    const auto plan = planLatencyEnvironment (request, current, std::getenv ("PIPEWIRE_PROPS"));
    if (plan.latency.empty() && current != nullptr && ! isValidLatency (current))
        std::fprintf (stderr, "Flubsound: PipeWire: PIPEWIRE_LATENCY=\"%s\" is kept but is not <frames>/<rate> (e.g. 256/48000)\n", current);
    if (! plan.latency.empty())
        ::setenv ("PIPEWIRE_LATENCY", plan.latency.c_str(), 0);
    if (! plan.props.empty())
        ::setenv ("PIPEWIRE_PROPS", plan.props.c_str(), 0);
}

/*  Monitor links. Applications play into the flubsound_<strip> null sinks;
    the engine reads a strip from its device input starting at the strip's
    channel (Settings > Processing > Input feeds strip, or the deviceInputMap
    setting, e.g. "Game=0;Music=8"). PipeWire does not connect a sink's
    monitor to that input by itself, so the router does: `pw-dump` (JSON, read
    with flub::json) lists the nodes, ports and links, the plan below picks
    monitor port k of each sink -> input port first + k of Flubsound's own
    node (the node whose application.process.id, or whose client's
    pipewire.sec.pid, is this process), and `pw-link <out> <in>` creates what
    is missing. The command lines carry port ids (integers) only: no name
    ever reaches the shell. Links other programs made are left alone; links
    the router made and no longer wants (the map changed) are removed with
    `pw-link -d`. */
struct Node
{
    uint32_t id = 0;
    std::string name;       // node.name
    std::string mediaClass; // media.class, e.g. "Audio/Sink"
    uint32_t processId = 0; // application.process.id, else its client's pid; 0 = unknown
    uint32_t clientId = 0;  // client.id; 0 = none
};

struct Port
{
    uint32_t id = 0;
    uint32_t nodeId = 0;
    int64_t index = -1; // port.id: position within the node's ports of one direction; -1 = not given
    bool isInput = false;
    bool isMonitor = false;
    bool isAudio = true; // false for MIDI / control ports (format.dsp)
    std::string name;    // port.name, e.g. "monitor_FL", "in_1"
};

struct Link
{
    uint32_t id = 0;
    uint32_t outputPort = 0;
    uint32_t inputPort = 0;
};

struct Graph
{
    std::map<uint32_t, Node> nodes;
    std::vector<Port> ports;
    std::vector<Link> links;
};

/** Parses `pw-dump` output (PipeWire 0.3.x and 1.x): a JSON array of objects
    {"id", "type": "PipeWire:Interface:<Node|Port|Link|Client>", "info"}.
    Objects that are not understood are skipped; only a document that is not
    a JSON array fails. */
bool parseDump (const std::string& text, Graph& graph, std::string& error)
{
    json::Value root;
    if (! json::parse (text, root, error))
    {
        error = "Unexpected pw-dump output: " + error;
        return false;
    }
    if (! root.isArray())
    {
        error = "Unexpected pw-dump output: not a JSON array";
        return false;
    }

    std::map<uint32_t, uint32_t> clientPids;
    for (const auto& object : root.asArray())
    {
        uint32_t id = 0;
        if (! object.isObject() || ! pactl::toUInt32 (object["id"], id))
            continue;
        const auto& type = object["type"].asString();
        const auto& info = object["info"];
        const auto& props = info["props"];

        if (type == "PipeWire:Interface:Client")
        {
            uint32_t pid = 0;
            if ((pactl::toUInt32 (props["pipewire.sec.pid"], pid) && pid != 0) || pactl::toUInt32 (props["application.process.id"], pid))
                clientPids[id] = pid;
        }
        else if (type == "PipeWire:Interface:Node")
        {
            Node node;
            node.id = id;
            node.name = props["node.name"].asString();
            node.mediaClass = props["media.class"].asString();
            pactl::toUInt32 (props["application.process.id"], node.processId);
            pactl::toUInt32 (props["client.id"], node.clientId);
            graph.nodes[id] = std::move (node);
        }
        else if (type == "PipeWire:Interface:Port")
        {
            Port port;
            port.id = id;
            if (! pactl::toUInt32 (props["node.id"], port.nodeId))
                continue;
            const auto& direction = info["direction"].isString() ? info["direction"].asString() : props["port.direction"].asString();
            if (direction != "input" && direction != "output" && direction != "in" && direction != "out")
                continue;
            port.isInput = direction == "input" || direction == "in";
            port.name = props["port.name"].asString();
            port.isMonitor = props["port.monitor"].asBool (false) || (! port.isInput && port.name.rfind ("monitor_", 0) == 0);
            const auto& dsp = props["format.dsp"];
            port.isAudio = ! dsp.isString() || dsp.asString().find ("audio") != std::string::npos;
            uint32_t index = 0;
            if (pactl::toUInt32 (props["port.id"], index))
                port.index = index;
            graph.ports.push_back (std::move (port));
        }
        else if (type == "PipeWire:Interface:Link")
        {
            Link link;
            link.id = id;
            if (pactl::toUInt32 (info["output-port-id"], link.outputPort) && pactl::toUInt32 (info["input-port-id"], link.inputPort))
                graph.links.push_back (link);
        }
    }

    // A node without application.process.id (some pipewire-jack versions put
    // it on the client only) inherits its client's pid.
    for (auto& [id, node] : graph.nodes)
        if (node.processId == 0 && node.clientId != 0)
            if (const auto it = clientPids.find (node.clientId); it != clientPids.end())
                node.processId = it->second;

    return true;
}

struct PortLink
{
    uint32_t outputPort = 0;
    uint32_t inputPort = 0;

    bool operator< (const PortLink& other) const noexcept
    {
        return outputPort != other.outputPort ? outputPort < other.outputPort : inputPort < other.inputPort;
    }
    bool operator== (const PortLink&) const = default;
};

/** One sink's links as planned, for messages. */
struct SinkLinks
{
    std::string sinkName;
    int firstChannel = 0; // 0-based engine input channel of the first link
    int channels = 0;     // links planned
};

struct LinkPlan
{
    uint32_t engineNode = 0;          // 0 = Flubsound has no PipeWire input node
    int engineInputs = 0;             // its audio input ports
    std::vector<PortLink> wanted;     // every link the map asks for (existing or not)
    std::vector<SinkLinks> sinks;     // per mapped sink that could be linked
    std::vector<std::string> problems; // user-presentable, one per issue
};

/** The audio ports of one node and direction, in channel order (port.id,
    then object id). */
std::vector<const Port*> audioPorts (const Graph& graph, uint32_t nodeId, bool inputs, bool monitorsOnly)
{
    std::vector<const Port*> result;
    for (const auto& port : graph.ports)
        if (port.nodeId == nodeId && port.isInput == inputs && port.isAudio && (! monitorsOnly || port.isMonitor))
            result.push_back (&port);
    std::stable_sort (result.begin(), result.end(), [] (const Port* a, const Port* b)
                      { return a->index != b->index ? (a->index >= 0 && (b->index < 0 || a->index < b->index)) : a->id < b->id; });
    return result;
}

/** Plans monitor port k of each mapped endpoint's sink -> input port
    first + k of this process's node with the most audio inputs. A sink gets
    as many links as it has monitor ports, but never reaches the next mapped
    strip's first channel or past the engine's last input. */
LinkPlan planMonitorLinks (const Graph& graph, uint32_t ownPid, const std::vector<AppAudioRouter::EndpointInput>& inputs)
{
    LinkPlan plan;

    std::vector<const Port*> engineInputs;
    for (const auto& [id, node] : graph.nodes)
    {
        if (ownPid == 0 || node.processId != ownPid)
            continue;
        auto ports = audioPorts (graph, id, true, false);
        if (ports.size() > engineInputs.size())
        {
            engineInputs = std::move (ports);
            plan.engineNode = id;
        }
    }
    plan.engineInputs = static_cast<int> (engineInputs.size());

    std::vector<int> firsts;
    for (const auto& input : inputs)
        if (input.firstInputChannel >= 0)
            firsts.push_back (input.firstInputChannel);
    if (firsts.empty())
        return plan;

    if (plan.engineNode == 0)
    {
        plan.problems.push_back ("Flubsound's audio input is not a PipeWire node: choose the JACK device type "
                                 "(PipeWire provides it through pipewire-jack) to have the Flubsound sinks linked to it");
        return plan;
    }

    for (const auto& input : inputs)
    {
        const int first = input.firstInputChannel;
        if (first < 0)
            continue;

        const Node* sink = nullptr;
        for (const auto& [id, node] : graph.nodes)
            if (node.name == input.endpointId && node.mediaClass.rfind ("Audio/Sink", 0) == 0)
                sink = &node;
        if (sink == nullptr)
        {
            plan.problems.push_back ("PipeWire has no sink '" + input.endpointId
                                     + "' (create the Flubsound sinks with platform/linux/flubsound-pipewire-setup.sh install)");
            continue;
        }

        const auto monitors = audioPorts (graph, sink->id, false, true);
        if (monitors.empty())
        {
            plan.problems.push_back ("The sink '" + input.endpointId + "' has no monitor ports");
            continue;
        }

        if (first >= plan.engineInputs)
        {
            plan.problems.push_back ("'" + input.endpointId + "' feeds input channel " + std::to_string (first + 1) + ", but Flubsound's input has only "
                                     + std::to_string (plan.engineInputs) + " channel" + (plan.engineInputs == 1 ? "" : "s"));
            continue;
        }

        int limit = plan.engineInputs;
        for (const int other : firsts)
            if (other > first)
                limit = std::min (limit, other);
        const int count = std::min (static_cast<int> (monitors.size()), limit - first);

        for (int k = 0; k < count; ++k)
            plan.wanted.push_back ({ monitors[static_cast<size_t> (k)]->id, engineInputs[static_cast<size_t> (first + k)]->id });
        plan.sinks.push_back ({ input.endpointId, first, count });

        if (count < static_cast<int> (monitors.size()))
            plan.problems.push_back ("Only " + std::to_string (count) + " of the " + std::to_string (monitors.size()) + " channels of '"
                                     + input.endpointId + "' fit into Flubsound's input channels " + std::to_string (first + 1) + "-"
                                     + std::to_string (first + count));
    }
    return plan;
}

/** The pw-link command line for one link: port ids only, never a name. */
std::string linkCommand (const PortLink& link, bool disconnect)
{
    return std::string ("LC_ALL=C pw-link ") + (disconnect ? "-d " : "") + std::to_string (link.outputPort) + " "
           + std::to_string (link.inputPort) + " 2>&1";
}

/** pw-link's answer when the link is there already (a race with another
    linker, or with JUCE's own JACK connections), which counts as done. */
bool alreadyLinked (const std::string& output)
{
    return output.find ("File exists") != std::string::npos;
}

constexpr const char* kNoToolsMessage =
    "pw-dump and pw-link were not found, so the Flubsound sinks cannot be linked to Flubsound's input automatically. "
    "Install the PipeWire tools (pipewire-bin on Debian / Ubuntu, pipewire-utils on Fedora, pipewire on Arch) "
    "or link each sink's monitor to Flubsound's input channels with qpwgraph or Helvum.";

void log (const std::string& text) { std::fprintf (stderr, "Flubsound: PipeWire links: %s\n", text.c_str()); }
} // namespace pipewire

//==============================================================================
// GlobalHotkeys - X11 key grabs (XGrabKey on the root window)
//==============================================================================
/*  There is no single global-shortcut API on Linux:
      - X11 (implemented here): XGrabKey on the root window for every chord x
        {none, CapsLock, NumLock, both} so the lock keys do not defeat the
        shortcut. libX11 is loaded at run time (as JUCE itself does), so the
        app and the unit tests build and run without it; the grabs use a
        private Display connection served by one event thread, which calls
        the callbacks (HotkeyManager hops them to the message thread).
        A chord another client already grabbed fails with BadAccess, which is
        caught with a temporary error handler around an XSync and reported
        as "in use" (registerHotkey returns false).
      - Wayland: grabbing keys is forbidden by design (an X grab through
        XWayland only sees keys while an XWayland window has focus, so it is
        not global). The sanctioned route is the xdg-desktop-portal
        GlobalShortcuts interface (KDE Plasma 5.27+, GNOME 48+, Hyprland),
        implemented by PortalGlobalHotkeys below; GlobalHotkeys::create()
        picks it in a Wayland session. Without the portal isSupported() ==
        false (until one appears on the bus) and the UI asks users to bind
        the actions in their desktop's keyboard settings instead. */

/** True in a Wayland session, where X key grabs are not global. */
bool isWaylandSession()
{
    const char* type = std::getenv ("XDG_SESSION_TYPE");
    if (type != nullptr && std::string (type) == "wayland")
        return true;
    const char* wayland = std::getenv ("WAYLAND_DISPLAY");
    return wayland != nullptr && *wayland != '\0';
}

#if FLUB_HAVE_X11_HEADERS
/** The libX11 entry points the hotkey and foreground-window services need,
    resolved with dlopen. */
struct X11Api
{
    void* lib = nullptr;
    Display* (*openDisplay) (const char*) = nullptr;
    int (*closeDisplay) (Display*) = nullptr;
    Window (*defaultRootWindow) (Display*) = nullptr;
    KeyCode (*keysymToKeycode) (Display*, KeySym) = nullptr;
    int (*grabKey) (Display*, int, unsigned int, Window, Bool, int, int) = nullptr;
    int (*ungrabKey) (Display*, int, unsigned int, Window) = nullptr;
    int (*selectInput) (Display*, Window, long) = nullptr;
    int (*pending) (Display*) = nullptr;
    int (*nextEvent) (Display*, XEvent*) = nullptr;
    int (*sync) (Display*, Bool) = nullptr;
    int (*flush) (Display*) = nullptr;
    int (*connectionNumber) (Display*) = nullptr;
    XErrorHandler (*setErrorHandler) (XErrorHandler) = nullptr;
    Bool (*setDetectableAutoRepeat) (Display*, Bool, Bool*) = nullptr; // Xkb, optional
    Atom (*internAtom) (Display*, const char*, Bool) = nullptr;
    int (*getWindowProperty) (Display*, Window, Atom, long, long, Bool, Atom, Atom*, int*, unsigned long*, unsigned long*,
                              unsigned char**) = nullptr;
    int (*free) (void*) = nullptr;

    static const X11Api* get()
    {
        static const X11Api api = []
        {
            X11Api a;
            a.lib = ::dlopen ("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
            if (a.lib == nullptr)
                return a;
            const auto sym = [&a] (auto& fn, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype (fn)>> (::dlsym (a.lib, name)); return fn != nullptr; };
            const bool ok = sym (a.openDisplay, "XOpenDisplay") && sym (a.closeDisplay, "XCloseDisplay")
                            && sym (a.defaultRootWindow, "XDefaultRootWindow") && sym (a.keysymToKeycode, "XKeysymToKeycode")
                            && sym (a.grabKey, "XGrabKey") && sym (a.ungrabKey, "XUngrabKey") && sym (a.selectInput, "XSelectInput")
                            && sym (a.pending, "XPending") && sym (a.nextEvent, "XNextEvent") && sym (a.sync, "XSync")
                            && sym (a.flush, "XFlush") && sym (a.connectionNumber, "XConnectionNumber")
                            && sym (a.setErrorHandler, "XSetErrorHandler") && sym (a.internAtom, "XInternAtom")
                            && sym (a.getWindowProperty, "XGetWindowProperty") && sym (a.free, "XFree");
            sym (a.setDetectableAutoRepeat, "XkbSetDetectableAutoRepeat");
            if (! ok)
            {
                ::dlclose (a.lib);
                a = X11Api();
            }
            return a;
        }();
        return api.lib != nullptr ? &api : nullptr;
    }
};

/** KeyChord key code (ASCII upper-case letter / digit, F1..F24 as 0x70 + n,
    VK-style navigation keys) to an X keysym; 0 if unmappable. */
KeySym keysymForChord (uint32_t keyCode)
{
    if (keyCode >= 'A' && keyCode <= 'Z')
        return static_cast<KeySym> (XK_a + (keyCode - 'A'));
    if (keyCode >= '0' && keyCode <= '9')
        return static_cast<KeySym> (XK_0 + (keyCode - '0'));
    if (keyCode >= 0x70 && keyCode < 0x70 + 24)
        return static_cast<KeySym> (XK_F1 + (keyCode - 0x70));
    switch (keyCode)
    {
        case 0x20: return XK_space;
        case 0x21: return XK_Prior;
        case 0x22: return XK_Next;
        case 0x23: return XK_End;
        case 0x24: return XK_Home;
        case 0x25: return XK_Left;
        case 0x26: return XK_Up;
        case 0x27: return XK_Right;
        case 0x28: return XK_Down;
        case 0x2D: return XK_Insert;
        case 0x2E: return XK_Delete;
        default: break;
    }
    return 0;
}

unsigned int x11Modifiers (uint32_t modifiers)
{
    unsigned int m = 0;
    if ((modifiers & KeyChord::Ctrl) != 0) m |= ControlMask;
    if ((modifiers & KeyChord::Alt) != 0) m |= Mod1Mask;
    if ((modifiers & KeyChord::Shift) != 0) m |= ShiftMask;
    if ((modifiers & KeyChord::Super) != 0) m |= Mod4Mask;
    return m;
}

// Lock-key variants grabbed for every chord (CapsLock = LockMask, NumLock =
// Mod2Mask on practically every keymap).
constexpr unsigned int kLockVariants[] = { 0u, LockMask, Mod2Mask, LockMask | Mod2Mask };
constexpr unsigned int kChordModifierMask = ControlMask | Mod1Mask | ShiftMask | Mod4Mask;

std::atomic<Display*> grabErrorDisplay { nullptr };
std::atomic<bool> grabFailed { false };
XErrorHandler previousErrorHandler = nullptr;

int onGrabError (Display* display, XErrorEvent* event)
{
    if (display == grabErrorDisplay.load())
    {
        if (event->error_code == BadAccess)
            grabFailed = true;
        return 0;
    }
    return previousErrorHandler != nullptr ? previousErrorHandler (display, event) : 0;
}

class LinuxGlobalHotkeys final : public GlobalHotkeys
{
public:
    LinuxGlobalHotkeys()
    {
        api = X11Api::get();
        const char* displayName = std::getenv ("DISPLAY");
        if (api == nullptr || isWaylandSession() || displayName == nullptr || *displayName == '\0')
            return;
        display = api->openDisplay (nullptr);
        if (display == nullptr)
            return;
        root = api->defaultRootWindow (display);
        api->selectInput (display, root, KeyPressMask | KeyReleaseMask);
        if (api->setDetectableAutoRepeat != nullptr)
        {
            Bool supported = False;
            api->setDetectableAutoRepeat (display, True, &supported); // held keys: one press, not a stream
        }
        if (::pipe2 (wakePipe, O_CLOEXEC | O_NONBLOCK) != 0)
        {
            api->closeDisplay (display);
            display = nullptr;
            return;
        }
        running = true;
        thread = std::thread ([this] { eventLoop(); });
    }

    ~LinuxGlobalHotkeys() override
    {
        unregisterAll();
        if (thread.joinable())
        {
            running = false;
            const char byte = 0;
            [[maybe_unused]] const auto written = ::write (wakePipe[1], &byte, 1);
            thread.join();
        }
        if (display != nullptr)
        {
            api->closeDisplay (display);
            ::close (wakePipe[0]);
            ::close (wakePipe[1]);
        }
    }

    using GlobalHotkeys::registerHotkey;
    bool isSupported() const override { return display != nullptr; }

    /** X11 has no list of shortcuts to show 'description' in. */
    bool registerHotkey (int id, const KeyChord& chord, const std::string&, std::function<void()> callback) override
    {
        const bool ok = grab (id, chord, std::move (callback));
        reportBinding (id, ok ? BindingResult::Status::Registered : BindingResult::Status::Unavailable);
        return ok;
    }

    void unregisterHotkey (int id) override
    {
        std::lock_guard<std::mutex> guard (mutex);
        const auto it = bindings.find (id);
        if (it == bindings.end())
            return;
        for (const unsigned int lockMask : kLockVariants)
            api->ungrabKey (display, it->second.keycode, it->second.modifiers | lockMask, root);
        // Sync, not flush: when this returns the server has released the
        // chord, so another client (or an immediate re-bind) can grab it.
        api->sync (display, False);
        bindings.erase (it);
        wakeEventThread();
    }

    void unregisterAll() override
    {
        std::vector<int> ids;
        {
            std::lock_guard<std::mutex> guard (mutex);
            for (const auto& b : bindings)
                ids.push_back (b.first);
        }
        for (const int id : ids)
            unregisterHotkey (id);
    }

private:
    /** XGrabKey for every lock-key variant; false if the chord is invalid,
        unmappable, grabbed by another client or already bound to another id
        here (X lets a client grab its own chord again without an error, so
        one press would run both actions; Windows refuses it too). */
    bool grab (int id, const KeyChord& chord, std::function<void()> callback)
    {
        if (display == nullptr || ! callback)
            return false;
        const KeySym keysym = keysymForChord (chord.keyCode);
        const unsigned int mods = x11Modifiers (chord.modifiers);
        if (! callback || ! detail::isValidChord (chord) || keysym == 0
            || mods == 0) // bare keys (even F-keys) would steal normal typing under X
            return false;
        unregisterHotkey (id);

        std::lock_guard<std::mutex> guard (mutex);
        const KeyCode keycode = api->keysymToKeycode (display, keysym);
        if (keycode == 0)
            return false;
        for (const auto& [otherId, other] : bindings)
            if (otherId != id && other.keycode == keycode && other.modifiers == mods)
                return false;

        // Grab synchronously so a BadAccess (someone else owns the chord) is
        // seen here. The error handler is process-wide, so it is only swapped
        // in for this call (on the message thread, like JUCE's own X calls).
        grabErrorDisplay = display;
        grabFailed = false;
        previousErrorHandler = api->setErrorHandler (&onGrabError);
        for (const unsigned int lock : kLockVariants)
            api->grabKey (display, keycode, mods | lock, root, False, GrabModeAsync, GrabModeAsync);
        api->sync (display, False);
        const bool failed = grabFailed;
        if (failed)
        {
            for (const unsigned int lock : kLockVariants)
                api->ungrabKey (display, keycode, mods | lock, root);
            api->sync (display, False);
        }
        api->setErrorHandler (previousErrorHandler);
        grabErrorDisplay = nullptr;
        wakeEventThread();
        if (failed)
            return false;

        bindings[id] = Binding { keycode, mods, std::move (callback), false };
        return true;
    }

    struct Binding
    {
        KeyCode keycode = 0;
        unsigned int modifiers = 0;
        std::function<void()> callback;
        bool down = false; // suppresses key repeat until the release
    };

    /** XSync outside the event thread reads whatever is waiting on the
        socket, key events included, into Xlib's queue; the event thread may
        then sleep in poll() on an empty socket with a press queued. A byte
        on the wake pipe makes it drain the queue (a spurious wake is
        harmless). Mutex held. */
    void wakeEventThread()
    {
        const char byte = 0;
        [[maybe_unused]] const auto written = ::write (wakePipe[1], &byte, 1);
    }

    void eventLoop()
    {
        pollfd fds[2] = { { api->connectionNumber (display), POLLIN, 0 }, { wakePipe[0], POLLIN, 0 } };
        while (running)
        {
            std::vector<std::function<void()>> fire;
            {
                std::lock_guard<std::mutex> guard (mutex);
                while (api->pending (display) > 0)
                {
                    XEvent event;
                    api->nextEvent (display, &event);
                    if (event.type != KeyPress && event.type != KeyRelease)
                        continue;
                    const auto& key = event.xkey;
                    const unsigned int mods = key.state & kChordModifierMask;
                    for (auto& [id, b] : bindings)
                    {
                        if (b.keycode != key.keycode)
                            continue;
                        // A release ends the press whatever modifiers are
                        // still held: users often let go of Ctrl/Alt first,
                        // and the key's release then carries no modifiers.
                        if (event.type == KeyRelease)
                        {
                            b.down = false;
                            continue;
                        }
                        if (b.modifiers != mods)
                            continue;
                        if (! b.down)
                            fire.push_back (b.callback);
                        b.down = true;
                    }
                }
            }
            for (auto& f : fire) // outside the lock: a callback may (un)register
                f();
            ::poll (fds, 2, -1);
            if ((fds[1].revents & POLLIN) != 0)
            {
                char buffer[16];
                while (::read (wakePipe[0], buffer, sizeof (buffer)) > 0) {}
            }
        }
    }

    const X11Api* api = nullptr;
    Display* display = nullptr;
    Window root = 0;
    int wakePipe[2] = { -1, -1 };
    std::atomic<bool> running { false };
    std::thread thread;
    std::mutex mutex;
    std::map<int, Binding> bindings;
};
#else
class LinuxGlobalHotkeys final : public GlobalHotkeys
{
public:
    using GlobalHotkeys::registerHotkey;
    bool isSupported() const override { return false; } // built without X11 headers

    bool registerHotkey (int id, const KeyChord&, const std::string&, std::function<void()>) override
    {
        reportBinding (id, BindingResult::Status::Unavailable);
        return false;
    }

    void unregisterHotkey (int) override {}
    void unregisterAll() override {}
};
#endif

//==============================================================================
// ForegroundApp - X11 _NET_ACTIVE_WINDOW + _NET_WM_PID
//==============================================================================
/*  EWMH window managers (every X11 desktop: GNOME/Xorg, KDE/X11, Xfce,
    Cinnamon, MATE, i3, ...) publish the focused client window as
    _NET_ACTIVE_WINDOW on the root window, and clients publish their process
    id as _NET_WM_PID (Xlib-based toolkits, GTK, Qt, SDL, Wine and JUCE all
    do). Each query is two XGetWindowProperty round trips on a private
    Display connection plus, when the window changed, a readlink of
    /proc/<pid>/exe. A window that is destroyed between the two reads raises
    BadWindow, which a temporary error handler (for this connection only)
    swallows. Wayland has no portable equivalent: XWayland only knows about
    X clients, so the service reports itself unsupported in a Wayland
    session. Message thread only (like the key grabs, the error handler is
    process-wide and only swapped in around the query). */
namespace foreground
{
/** First NUL-terminated field of /proc/<pid>/cmdline text (argv[0]). */
std::string firstArgument (const std::string& cmdline)
{
    return cmdline.substr (0, cmdline.find ('\0'));
}

std::string baseName (const std::string& path)
{
    const auto slash = path.find_last_of ("/\\");
    return slash == std::string::npos ? path : path.substr (slash + 1);
}

bool endsWithExe (const std::string& name)
{
    if (name.size() < 5)
        return false;
    std::string tail = name.substr (name.size() - 4);
    for (auto& c : tail)
        c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
    return tail == ".exe";
}

/** The executable a process should be known by, from its /proc/<pid>/exe
    link target, its cmdline and its comm name (any may be empty: exe is
    unreadable for other users' processes). Wine and Proton run every
    Windows program in a "wine[64][-preloader]" loader whose argv[0] is the
    program's Windows path ("C:\\Games\\cs2.exe"): that path is reported,
    so rules written for the Windows executable match on Linux too. */
void describeExecutable (const std::string& exeLink, const std::string& cmdline, const std::string& comm, ForegroundAppInfo& info)
{
    const auto loader = baseName (exeLink);
    const bool isWine = loader == "wine" || loader == "wine64" || loader == "wine-preloader" || loader == "wine64-preloader";
    const auto argument = firstArgument (cmdline);
    if (isWine && endsWithExe (argument))
    {
        info.executablePath = argument;
        info.executableName = baseName (argument);
        return;
    }
    if (! exeLink.empty())
    {
        info.executablePath = exeLink;
        info.executableName = loader;
        return;
    }
    info.executablePath.clear();
    info.executableName = ! comm.empty() ? comm : baseName (argument);
}

/** Small /proc text file (cmdline / comm), at most 4 KiB; "" on failure. */
std::string readProcFile (const std::string& path)
{
    const int fd = ::open (path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return {};
    std::string text (4096, '\0');
    size_t used = 0;
    while (used < text.size())
    {
        const auto n = ::read (fd, text.data() + used, text.size() - used);
        if (n <= 0)
            break;
        used += static_cast<size_t> (n);
    }
    ::close (fd);
    text.resize (used);
    while (! text.empty() && text.back() == '\n')
        text.pop_back();
    return text;
}

std::string readExeLink (uint32_t pid)
{
    std::string target (PATH_MAX, '\0');
    const auto n = ::readlink (("/proc/" + std::to_string (pid) + "/exe").c_str(), target.data(), target.size());
    if (n <= 0)
        return {};
    target.resize (static_cast<size_t> (n));
    // A replaced / deleted binary reads as "<path> (deleted)".
    static constexpr const char deleted[] = " (deleted)";
    constexpr size_t deletedLength = sizeof (deleted) - 1;
    if (target.size() > deletedLength && target.compare (target.size() - deletedLength, deletedLength, deleted) == 0)
        target.resize (target.size() - deletedLength);
    return target;
}

/** Fills 'info' for process 'pid' from /proc; false if the process is gone. */
bool describeProcess (uint32_t pid, ForegroundAppInfo& info)
{
    const auto dir = "/proc/" + std::to_string (pid);
    const auto comm = readProcFile (dir + "/comm");
    const auto cmdline = readProcFile (dir + "/cmdline");
    const auto exe = readExeLink (pid);
    if (comm.empty() && cmdline.empty() && exe.empty())
        return false;
    info.processId = pid;
    describeExecutable (exe, cmdline, comm, info);
    info.bundleId.clear();
    info.isThisProcess = pid == static_cast<uint32_t> (::getpid());
    return ! info.executableName.empty();
}
} // namespace foreground

#if FLUB_HAVE_X11_HEADERS
std::atomic<Display*> foregroundErrorDisplay { nullptr };
XErrorHandler previousForegroundErrorHandler = nullptr;

int onForegroundError (Display* display, XErrorEvent* event)
{
    if (display == foregroundErrorDisplay.load())
        return 0; // BadWindow: the window closed between the reads
    return previousForegroundErrorHandler != nullptr ? previousForegroundErrorHandler (display, event) : 0;
}

class LinuxForegroundApp final : public ForegroundApp
{
public:
    LinuxForegroundApp()
    {
        api = X11Api::get();
        const char* displayName = std::getenv ("DISPLAY");
        if (api == nullptr || isWaylandSession() || displayName == nullptr || *displayName == '\0')
            return;
        display = api->openDisplay (nullptr);
        if (display == nullptr)
            return;
        root = api->defaultRootWindow (display);
        activeWindowAtom = api->internAtom (display, "_NET_ACTIVE_WINDOW", False);
        pidAtom = api->internAtom (display, "_NET_WM_PID", False);
    }

    ~LinuxForegroundApp() override
    {
        if (display != nullptr)
            api->closeDisplay (display);
    }

    bool isSupported() const override { return display != nullptr; }

    std::string unsupportedReason() const override
    {
        if (display != nullptr)
            return {};
        if (isWaylandSession())
            return "Wayland does not let applications see which window is in the foreground, so profiles cannot follow the active "
                   "application. Switch presets by hand or with the global shortcuts, or use an X11 session.";
        if (api == nullptr)
            return "The foreground application cannot be detected: the X11 client library (libX11) is not installed.";
        return "The foreground application cannot be detected: no X11 display is available.";
    }

    bool query (ForegroundAppInfo& info) override
    {
        if (display == nullptr)
            return false;

        foregroundErrorDisplay = display;
        previousForegroundErrorHandler = api->setErrorHandler (&onForegroundError);
        unsigned long window = 0, pid = 0;
        const bool found = readProperty (root, activeWindowAtom, XA_WINDOW, window) && window != 0
                           && readProperty (static_cast<Window> (window), pidAtom, XA_CARDINAL, pid) && pid != 0
                           && pid <= std::numeric_limits<uint32_t>::max();
        api->setErrorHandler (previousForegroundErrorHandler);
        foregroundErrorDisplay = nullptr;
        if (! found)
            return false;

        // Same window, same process: the cached description (no /proc reads).
        if (static_cast<Window> (window) != cachedWindow || static_cast<uint32_t> (pid) != cached.processId)
        {
            cachedWindow = 0;
            if (! foreground::describeProcess (static_cast<uint32_t> (pid), cached))
                return false;
            cachedWindow = static_cast<Window> (window);
        }
        info = cached;
        return true;
    }

private:
    /** One 32-bit item of a property of the given type; false if missing. */
    bool readProperty (Window window, Atom property, Atom type, unsigned long& value) const
    {
        Atom actualType = 0;
        int actualFormat = 0;
        unsigned long count = 0, remaining = 0;
        unsigned char* data = nullptr;
        const int status = api->getWindowProperty (display, window, property, 0, 1, False, type, &actualType, &actualFormat, &count,
                                                   &remaining, &data);
        const bool ok = status == Success && actualType == type && actualFormat == 32 && count >= 1 && data != nullptr;
        if (ok)
            std::memcpy (&value, data, sizeof (value)); // format 32 is returned as an array of long
        if (data != nullptr)
            api->free (data);
        return ok;
    }

    const X11Api* api = nullptr;
    Display* display = nullptr;
    Window root = 0;
    Atom activeWindowAtom = 0, pidAtom = 0;
    Window cachedWindow = 0;
    ForegroundAppInfo cached;
};
#else
class LinuxForegroundApp final : public ForegroundApp
{
public:
    bool isSupported() const override { return false; } // built without X11 headers
    bool query (ForegroundAppInfo&) override { return false; }

    std::string unsupportedReason() const override
    {
        return isWaylandSession() ? "Wayland does not let applications see which window is in the foreground, so profiles cannot "
                                    "follow the active application."
                                  : "This build cannot detect the foreground application (built without X11 support).";
    }
};
#endif

//==============================================================================
// GlobalHotkeys - xdg-desktop-portal GlobalShortcuts (Wayland sessions)
//==============================================================================
/*  Protocol (org.freedesktop.portal.GlobalShortcuts, version 1, served by
    org.freedesktop.portal.Desktop at /org/freedesktop/portal/desktop):
      1. CreateSession ({handle_token, session_handle_token}) returns a
         Request object path; the result arrives later as that Request's
         Response signal (u response, a{sv} results) with
         results["session_handle"].
      2. BindShortcuts (session, [(id, {description, preferred_trigger})],
         parent_window, {handle_token}) answers the same way with
         results["shortcuts"]: the shortcuts actually bound, each as
         (id, {description, trigger_description}). The compositor may show a
         dialog; the user may pick another key or decline, so
         preferred_trigger is only a hint.
      3. Activated / Deactivated (session, shortcut_id, timestamp, options)
         signals while the session lives.
    Response codes: 0 success, 1 cancelled by the user, 2 other failure.
    Request paths are predictable (.../request/<sender>/<handle_token>), so
    the Response is matched even when it overtakes the method reply.

    Rebinding: the interface has no "unbind", and binding a session's
    shortcuts is a one-time step (a second BindShortcuts on the same session
    is not portable across portal back-ends). A changed set is therefore
    applied by closing the session (Session.Close) and binding the whole new
    set in a fresh one. Desktops remember the user's choice per application
    and shortcut id, so re-binding known ids does not ask again. Changes are
    coalesced: HotkeyManager::registerAll() unregisters everything and then
    registers each action in a burst; that becomes one new session
    (settleTime after the last change), and a burst that ends with the set
    already bound is not sent at all. ListShortcuts is not used: the
    BindShortcuts response already lists what was bound.

    Start-up probe: Properties.Get (GlobalShortcuts, "version"), sent by the
    service thread without blocking. The constructor waits for its answer
    only briefly (kProbeWait, 250 ms): a running portal, or D-Bus saying
    there is none, answers at once, but a portal that D-Bus is still
    starting at login may take seconds. Until the answer the service counts
    as supported and requests wait for it. No portal or no GlobalShortcuts
    interface makes it unsupported, until NameOwnerChanged shows a portal
    appearing, which is probed again; a timeout or failed activation is
    retried with a growing delay (2 s up to 60 s). Losing the session bus
    makes it unsupported for the rest of the run.

    Results are asynchronous, so registerHotkey() returns true when the chord
    can be requested (valid chord with a trigger name, portal present or
    still being probed). The outcome goes to the binding listener from the
    service thread once the batch is answered: Registered, Reassigned (the
    response's trigger_description names another key than
    preferred_trigger, or one portal::sameTrigger cannot read; shown with
    the desktop's own text, never as Registered), Declined (response code
    1 or 2, the id is missing from the bound list, or GNOME bound it
    without a key) or Unavailable (a D-Bus error, or no portal to ask). A
    batch that needs no new binding repeats the previous outcome. An answer
    to a batch the wanted set has changed since is not reported (it is not
    the outcome of what is wanted now); the next batch's is. Refusals and
    errors are also logged to stderr.

    Threading: one private connection to the session bus, serviced by the
    service's own thread (poll on the bus fd + a wake pipe, like the X11
    class). After the constructor all D-Bus traffic happens on that thread;
    registerHotkey / unregister* only edit the wanted set under a mutex and
    wake it. Callbacks run on that thread, once per Activated signal (the
    Deactivated signal, key release, is ignored). Signals count only when
    they come from the portal's unique bus name and name the current
    session. */
namespace dbus
{
/*  libdbus-1 is declared here from its stable C ABI instead of including
    <dbus/dbus.h>. The headers ship in libdbus-1-dev, which most build
    machines (CI, this project's build hosts) do not have; a
    __has_include(<dbus/dbus.h>) switch with a fallback would give two
    declaration paths, one of them never compiled or tested. libdbus-1.so.3
    has kept its ABI since 1.0 (2006) and the D-Bus project guarantees it, so
    these prototypes, the type codes and the two structs the caller
    allocates are fixed:
      - DBusError { const char* name; const char* message; unsigned dummy
        bit-fields; void* padding1; }: declared field for field, plus spare
        room;
      - DBusMessageIter: opaque, 72 bytes on LP64, only ever passed by
        pointer: over-allocated here (libdbus writes at most its real size). */
struct Connection;
struct Message;
using Boolean = uint32_t; // dbus_bool_t ("Bool" is an X11 macro)

struct Error
{
    const char* name = nullptr;
    const char* message = nullptr;
    unsigned int dummyBits = 0;
    void* padding1 = nullptr;
    void* spare[4] {};
};

struct Iter
{
    void* opaque[16] {};
};

constexpr int kTypeUInt32 = 'u';
constexpr int kTypeString = 's';
constexpr int kTypeObjectPath = 'o';
constexpr int kTypeVariant = 'v';
constexpr int kTypeArray = 'a';
constexpr int kTypeStruct = 'r';
constexpr int kTypeDictEntry = 'e';

constexpr int kMessageMethodReturn = 2;
constexpr int kMessageError = 3;
constexpr int kMessageSignal = 4;

constexpr int kDispatchDataRemains = 0;

/** The libdbus-1 entry points the portal client needs, resolved with dlopen. */
struct Api
{
    void* lib = nullptr;
    void (*errorInit) (Error*) = nullptr;
    void (*errorFree) (Error*) = nullptr;
    Connection* (*connectionOpenPrivate) (const char*, Error*) = nullptr;
    Boolean (*busRegister) (Connection*, Error*) = nullptr;
    const char* (*busGetUniqueName) (Connection*) = nullptr;
    void (*busAddMatch) (Connection*, const char*, Error*) = nullptr;
    void (*connectionSetExitOnDisconnect) (Connection*, Boolean) = nullptr;
    void (*connectionClose) (Connection*) = nullptr;
    void (*connectionUnref) (Connection*) = nullptr;
    Boolean (*connectionGetUnixFd) (Connection*, int*) = nullptr;
    Boolean (*connectionReadWrite) (Connection*, int) = nullptr;
    int (*connectionGetDispatchStatus) (Connection*) = nullptr;
    Message* (*connectionPopMessage) (Connection*) = nullptr;
    Boolean (*connectionSend) (Connection*, Message*, uint32_t*) = nullptr;
    void (*connectionFlush) (Connection*) = nullptr;
    Boolean (*connectionHasMessagesToSend) (Connection*) = nullptr;
    Message* (*sendWithReplyAndBlock) (Connection*, Message*, int, Error*) = nullptr;
    Message* (*messageNewMethodCall) (const char*, const char*, const char*, const char*) = nullptr;
    void (*messageUnref) (Message*) = nullptr;
    int (*messageGetType) (Message*) = nullptr;
    const char* (*messageGetPath) (Message*) = nullptr;
    const char* (*messageGetInterface) (Message*) = nullptr;
    const char* (*messageGetMember) (Message*) = nullptr;
    const char* (*messageGetSender) (Message*) = nullptr;
    const char* (*messageGetErrorName) (Message*) = nullptr;
    uint32_t (*messageGetReplySerial) (Message*) = nullptr;
    void (*messageSetNoReply) (Message*, Boolean) = nullptr;
    Boolean (*iterInit) (Message*, Iter*) = nullptr;
    void (*iterInitAppend) (Message*, Iter*) = nullptr;
    int (*iterGetArgType) (Iter*) = nullptr;
    Boolean (*iterNext) (Iter*) = nullptr;
    void (*iterRecurse) (Iter*, Iter*) = nullptr;
    void (*iterGetBasic) (Iter*, void*) = nullptr;
    Boolean (*iterAppendBasic) (Iter*, int, const void*) = nullptr;
    Boolean (*iterOpenContainer) (Iter*, int, const char*, Iter*) = nullptr;
    Boolean (*iterCloseContainer) (Iter*, Iter*) = nullptr;

    static const Api* get()
    {
        static const Api api = []
        {
            Api a;
            a.lib = ::dlopen ("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
            if (a.lib == nullptr)
                return a;
            const auto sym = [&a] (auto& fn, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype (fn)>> (::dlsym (a.lib, name)); return fn != nullptr; };
            Boolean (*threadsInitDefault)() = nullptr;
            const bool ok = sym (threadsInitDefault, "dbus_threads_init_default") && sym (a.errorInit, "dbus_error_init")
                            && sym (a.errorFree, "dbus_error_free") && sym (a.connectionOpenPrivate, "dbus_connection_open_private")
                            && sym (a.busRegister, "dbus_bus_register") && sym (a.busGetUniqueName, "dbus_bus_get_unique_name")
                            && sym (a.busAddMatch, "dbus_bus_add_match")
                            && sym (a.connectionSetExitOnDisconnect, "dbus_connection_set_exit_on_disconnect")
                            && sym (a.connectionClose, "dbus_connection_close") && sym (a.connectionUnref, "dbus_connection_unref")
                            && sym (a.connectionGetUnixFd, "dbus_connection_get_unix_fd")
                            && sym (a.connectionReadWrite, "dbus_connection_read_write")
                            && sym (a.connectionGetDispatchStatus, "dbus_connection_get_dispatch_status")
                            && sym (a.connectionPopMessage, "dbus_connection_pop_message")
                            && sym (a.connectionSend, "dbus_connection_send") && sym (a.connectionFlush, "dbus_connection_flush")
                            && sym (a.connectionHasMessagesToSend, "dbus_connection_has_messages_to_send")
                            && sym (a.sendWithReplyAndBlock, "dbus_connection_send_with_reply_and_block")
                            && sym (a.messageNewMethodCall, "dbus_message_new_method_call") && sym (a.messageUnref, "dbus_message_unref")
                            && sym (a.messageGetType, "dbus_message_get_type") && sym (a.messageGetPath, "dbus_message_get_path")
                            && sym (a.messageGetInterface, "dbus_message_get_interface")
                            && sym (a.messageGetMember, "dbus_message_get_member") && sym (a.messageGetSender, "dbus_message_get_sender")
                            && sym (a.messageGetErrorName, "dbus_message_get_error_name")
                            && sym (a.messageGetReplySerial, "dbus_message_get_reply_serial")
                            && sym (a.messageSetNoReply, "dbus_message_set_no_reply") && sym (a.iterInit, "dbus_message_iter_init")
                            && sym (a.iterInitAppend, "dbus_message_iter_init_append")
                            && sym (a.iterGetArgType, "dbus_message_iter_get_arg_type") && sym (a.iterNext, "dbus_message_iter_next")
                            && sym (a.iterRecurse, "dbus_message_iter_recurse") && sym (a.iterGetBasic, "dbus_message_iter_get_basic")
                            && sym (a.iterAppendBasic, "dbus_message_iter_append_basic")
                            && sym (a.iterOpenContainer, "dbus_message_iter_open_container")
                            && sym (a.iterCloseContainer, "dbus_message_iter_close_container");
            // Several threads use libdbus (this service, and any other
            // libdbus user in the process); must precede every other call.
            if (! ok || threadsInitDefault() == 0)
            {
                ::dlclose (a.lib);
                a = Api();
            }
            return a;
        }();
        return api.lib != nullptr ? &api : nullptr;
    }
};

struct MessageUnref
{
    void operator() (Message* message) const { Api::get()->messageUnref (message); }
};
using MessageRef = std::unique_ptr<Message, MessageUnref>;

inline std::string str (const char* text) { return text != nullptr ? std::string (text) : std::string(); }

/** Appends a string-like basic value ('s' or 'o'). libdbus aborts the
    process on an invalid UTF-8 string or object path, so callers only pass
    ASCII built here or paths checked with portal::isValidObjectPath. */
inline bool appendBasic (const Api& api, Iter* iter, int type, const std::string& value)
{
    const char* text = value.c_str();
    return api.iterAppendBasic (iter, type, &text) != 0;
}

/** Appends {key: variant<type>(value)} to an a{sv} under construction. */
inline bool appendDictEntry (const Api& api, Iter* dict, const char* key, int type, const std::string& value)
{
    Iter entry, variant;
    const char signature[2] = { static_cast<char> (type), '\0' };
    return api.iterOpenContainer (dict, kTypeDictEntry, nullptr, &entry) != 0 && appendBasic (api, &entry, kTypeString, key)
           && api.iterOpenContainer (&entry, kTypeVariant, signature, &variant) != 0 && appendBasic (api, &variant, type, value)
           && api.iterCloseContainer (&entry, &variant) != 0 && api.iterCloseContainer (dict, &entry) != 0;
}

/** Reads an 's' or 'o' argument; false for any other type. */
inline bool readString (const Api& api, Iter* iter, std::string& out)
{
    const int type = api.iterGetArgType (iter);
    if (type != kTypeString && type != kTypeObjectPath)
        return false;
    const char* text = nullptr;
    api.iterGetBasic (iter, &text);
    out = str (text);
    return true;
}

/** Finds 'key' in the a{sv} at 'dict' and points 'value' into its variant. */
inline bool findInVardict (const Api& api, Iter* dict, const char* key, Iter& value)
{
    if (api.iterGetArgType (dict) != kTypeArray)
        return false;
    Iter entries;
    api.iterRecurse (dict, &entries);
    for (; api.iterGetArgType (&entries) == kTypeDictEntry; api.iterNext (&entries))
    {
        Iter entry;
        api.iterRecurse (&entries, &entry);
        std::string name;
        if (readString (api, &entry, name) && name == key && api.iterNext (&entry) != 0 && api.iterGetArgType (&entry) == kTypeVariant)
        {
            api.iterRecurse (&entry, &value);
            return true;
        }
    }
    return false;
}

inline bool readVardictString (const Api& api, Iter* dict, const char* key, std::string& out)
{
    Iter value;
    return findInVardict (api, dict, key, value) && readString (api, &value, out);
}
} // namespace dbus

namespace portal
{
constexpr const char* kService = "org.freedesktop.portal.Desktop";
constexpr const char* kObjectPath = "/org/freedesktop/portal/desktop";
constexpr const char* kShortcutsInterface = "org.freedesktop.portal.GlobalShortcuts";
constexpr const char* kRequestInterface = "org.freedesktop.portal.Request";
constexpr const char* kSessionInterface = "org.freedesktop.portal.Session";

// Broadcast signals only reach a connection with a matching rule. The portal
// sends Response / Activated to its client directly, but the rules keep
// portal back-ends that broadcast working too.
constexpr const char* kMatchRules[] = {
    "type='signal',interface='org.freedesktop.portal.Request',member='Response'",
    "type='signal',interface='org.freedesktop.portal.GlobalShortcuts'",
    "type='signal',interface='org.freedesktop.portal.Session',member='Closed'",
    "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.freedesktop.portal.Desktop'",
};

/** A KeyChord as a trigger in the XDG shortcuts spec format: the modifiers
    CTRL, ALT, SHIFT and LOGO, then the xkb keysym name of the unshifted key,
    joined with '+' ("CTRL+ALT+Up", "CTRL+SHIFT+m", "LOGO+F13"). "" when the
    key has no name. */
std::string triggerFor (const KeyChord& chord)
{
    std::string key;
    if (detail::isLetterKey (chord.keyCode))
        key = std::string (1, static_cast<char> ('a' + (chord.keyCode - 'A')));
    else if (detail::isDigitKey (chord.keyCode))
        key = std::string (1, static_cast<char> (chord.keyCode));
    else if (const int f = detail::functionKeyNumber (chord.keyCode); f > 0)
        key = "F" + std::to_string (f);
    else
    {
        switch (chord.keyCode)
        {
            case 0x20: key = "space"; break;
            case 0x21: key = "Page_Up"; break;
            case 0x22: key = "Page_Down"; break;
            case 0x23: key = "End"; break;
            case 0x24: key = "Home"; break;
            case 0x25: key = "Left"; break;
            case 0x26: key = "Up"; break;
            case 0x27: key = "Right"; break;
            case 0x28: key = "Down"; break;
            case 0x2D: key = "Insert"; break;
            case 0x2E: key = "Delete"; break;
            default: return {};
        }
    }

    std::string trigger;
    if ((chord.modifiers & KeyChord::Ctrl) != 0) trigger += "CTRL+";
    if ((chord.modifiers & KeyChord::Alt) != 0) trigger += "ALT+";
    if ((chord.modifiers & KeyChord::Shift) != 0) trigger += "SHIFT+";
    if ((chord.modifiers & KeyChord::Super) != 0) trigger += "LOGO+";
    return trigger + key;
}

/** A trigger reduced to its modifiers and a canonical key name, so the
    preferred_trigger sent ("CTRL+ALT+Page_Up") can be compared with the
    trigger_description a desktop answers with ("Ctrl+Alt+PgUp",
    "<Control><Alt>Page_Up", "Alt+Ctrl+Page Up"). Besides the English names
    it knows the modifier and key names Qt shows in the first target
    languages (KDE's description is Qt's native text: German
    "Strg+Umschalt+Bild auf", French "Ctrl+Maj+Haut", Spanish "Ctrl+Mayús+Arriba",
    Brazilian Portuguese "Ctrl+Alt+Cima"). The tables only map names to the
    same key in every language, so an alias can never make a different key
    compare equal. A description still not understood compares as a different
    key and is reported as reassigned with the desktop's own text, never as
    registered. */
struct CanonicalTrigger
{
    uint32_t modifiers = 0;
    std::string key;
    bool operator== (const CanonicalTrigger&) const = default;
};

/** 'name' (lower case, without spaces, '_' and '-') as a KeyChord modifier;
    0 if it names none. */
uint32_t modifierFromName (const std::string& name)
{
    static const std::map<std::string, uint32_t> modifierNames {
        { "ctrl", KeyChord::Ctrl },  { "control", KeyChord::Ctrl }, { "primary", KeyChord::Ctrl }, { "strg", KeyChord::Ctrl },
        { "alt", KeyChord::Alt },    { "mod1", KeyChord::Alt },     { "shift", KeyChord::Shift },  { "logo", KeyChord::Super },
        { "super", KeyChord::Super }, { "meta", KeyChord::Super },  { "win", KeyChord::Super },    { "windows", KeyChord::Super },
        { "mod4", KeyChord::Super }, { "cmd", KeyChord::Super },    { "command", KeyChord::Super },
        // Shift as Qt translates it: de, fr, es, it.
        { "umschalt", KeyChord::Shift }, { "maj", KeyChord::Shift }, { "may\xc3\xbas", KeyChord::Shift }, { "mayus", KeyChord::Shift },
        { "maiusc", KeyChord::Shift },
    };
    const auto it = modifierNames.find (name);
    return it != modifierNames.end() ? it->second : 0;
}

/** 'name' (as for modifierFromName) as the canonical key name. */
std::string canonicalKeyName (const std::string& name)
{
    static const std::map<std::string, std::string> keyAliases {
        { "pgup", "pageup" },     { "prior", "pageup" },   { "pgdown", "pagedown" }, { "pgdn", "pagedown" },
        { "next", "pagedown" },   { "del", "delete" },     { "ins", "insert" },      { "spacebar", "space" },
        { "uparrow", "up" },      { "downarrow", "down" }, { "leftarrow", "left" },  { "rightarrow", "right" },
        { "\xe2\x86\x91", "up" }, { "\xe2\x86\x93", "down" }, { "\xe2\x86\x90", "left" }, { "\xe2\x86\x92", "right" },
        // German (Qt): Hoch, Runter, Links, Rechts, Bild auf / ab, Pos1, Ende, Einfg, Entf, Leertaste.
        { "hoch", "up" },         { "runter", "down" },    { "links", "left" },      { "rechts", "right" },
        { "bildauf", "pageup" },  { "bildab", "pagedown" }, { "pos1", "home" },      { "ende", "end" },
        { "einfg", "insert" },    { "entf", "delete" },    { "leertaste", "space" },
        // French: Haut, Bas, Gauche, Droite, Page haut / bas, Début, Fin, Inser, Suppr, Espace.
        { "haut", "up" },         { "bas", "down" },       { "gauche", "left" },     { "droite", "right" },
        { "pagehaut", "pageup" }, { "pagebas", "pagedown" }, { "d\xc3\xa9" "but", "home" }, { "fin", "end" },
        { "inser", "insert" },    { "suppr", "delete" },   { "espace", "space" },
        // Spanish: Arriba, Abajo, Izquierda, Derecha, Re Pág / Av Pág, Inicio, Fin, Insert, Supr, Espacio.
        { "arriba", "up" },       { "abajo", "down" },     { "izquierda", "left" },  { "derecha", "right" },
        { "rep\xc3\xa1g", "pageup" }, { "avp\xc3\xa1g", "pagedown" }, { "inicio", "home" }, { "supr", "delete" },
        { "espacio", "space" },
        // Brazilian Portuguese: Cima, Baixo, Esquerda, Direita, Início, Fim, Espaço.
        { "cima", "up" },         { "baixo", "down" },     { "esquerda", "left" },   { "direita", "right" },
        { "in\xc3\xad" "cio", "home" }, { "fim", "end" }, { "espa\xc3\xa7o", "space" },
    };
    const auto it = keyAliases.find (name);
    return it != keyAliases.end() ? it->second : name;
}

/** Lower case (ASCII letters; other UTF-8 bytes are kept) without the
    spaces, '_' and '-' that names are written with or without. */
std::string normalisedName (const std::string& text)
{
    std::string out;
    for (const char c : text)
        if (c != ' ' && c != '_' && c != '-')
            out += static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
    return out;
}

/** false when 'text' names no key, or more than one. */
bool canonicalTrigger (const std::string& text, CanonicalTrigger& out)
{
    out = {};
    std::vector<std::string> tokens;
    std::string token;
    for (const char c : text + "+")
    {
        if (c == '+' || c == '<' || c == '>')
        {
            if (! token.empty())
                tokens.push_back (normalisedName (token));
            token.clear();
        }
        else
            token += c;
    }
    for (const auto& t : tokens)
    {
        if (t.empty())
            continue;
        if (const uint32_t modifier = modifierFromName (t); modifier != 0)
            out.modifiers |= modifier;
        else if (! out.key.empty())
            return false;
        else
            out.key = canonicalKeyName (t);
    }
    return ! out.key.empty();
}

/** The single triggers a trigger_description lists. GNOME answers with its
    GTK accelerators inside a localised sentence ("Press <Control><Alt>Up",
    "Press <Control><Alt>Up or <Super>u"): each "<Mod>...key" run is one
    trigger and the words around them are dropped. Other desktops list
    triggers separated by ',' or ';' (Qt's list format). */
std::vector<std::string> triggerAlternatives (const std::string& text)
{
    std::vector<std::string> out;
    const auto isSpace = [] (char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    if (text.find ('<') != std::string::npos)
    {
        for (size_t start = text.find ('<'); start != std::string::npos; start = text.find ('<', start))
        {
            size_t end = start;
            while (end < text.size() && text[end] == '<') // the <Modifier> groups
            {
                const size_t close = text.find ('>', end);
                end = close == std::string::npos ? text.size() : close + 1;
            }
            while (end < text.size() && ! isSpace (text[end]) && text[end] != ',' && text[end] != ';' && text[end] != '<')
                ++end; // the key name
            out.push_back (text.substr (start, end - start));
            start = end;
        }
        return out;
    }
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i)
    {
        if (i < text.size() && text[i] != ',' && text[i] != ';')
            continue;
        const std::string piece = text.substr (start, i - start);
        if (piece.find_first_not_of (" \t\r\n") != std::string::npos)
            out.push_back (piece);
        start = i + 1;
    }
    return out;
}

/** True when one trigger 'alternative' names 'wanted'. Leading words that
    are not a modifier are skipped if the whole text does not match, which
    drops a localised prefix such as "Press F13" / "Drücken Sie Strg+Alt+Hoch". */
bool alternativeNames (const std::string& alternative, const CanonicalTrigger& wanted)
{
    std::string rest = alternative;
    for (;;)
    {
        CanonicalTrigger actual;
        if (canonicalTrigger (rest, actual) && actual == wanted)
            return true;
        const size_t wordStart = rest.find_first_not_of (" \t");
        if (wordStart == std::string::npos)
            return false;
        const size_t wordEnd = rest.find_first_of (" \t", wordStart);
        if (wordEnd == std::string::npos)
            return false; // one word left: already compared
        const std::string word = rest.substr (wordStart, wordEnd - wordStart);
        if (word.find ('+') != std::string::npos || modifierFromName (normalisedName (word)) != 0)
            return false; // part of the trigger, not a prefix
        rest = rest.substr (wordEnd);
    }
}

/** True when the desktop's trigger_description names the trigger requested
    (as one of the triggers it lists). */
bool sameTrigger (const std::string& preferredTrigger, const std::string& triggerDescription)
{
    CanonicalTrigger preferred;
    if (! canonicalTrigger (preferredTrigger, preferred))
        return false;
    for (const auto& alternative : triggerAlternatives (triggerDescription))
        if (alternativeNames (alternative, preferred))
            return true;
    return false;
}

/** True when $XDG_CURRENT_DESKTOP (a ':' list, "ubuntu:GNOME") names GNOME,
    whose portal back-end leaves trigger_description out for a shortcut
    without a key. */
bool desktopIsGnome()
{
    const char* desktop = std::getenv ("XDG_CURRENT_DESKTOP");
    if (desktop == nullptr)
        return false;
    std::string list = std::string (desktop) + ":";
    for (size_t start = 0, colon = 0; (colon = list.find (':', start)) != std::string::npos; start = colon + 1)
        if (normalisedName (list.substr (start, colon - start)) == "gnome")
            return true;
    return false;
}

/** 'text' with NUL bytes and every byte that does not start a valid UTF-8
    sequence (overlong forms, surrogates, code points above U+10FFFF, the
    noncharacters U+FFFE / U+FFFF) replaced by '?': libdbus aborts the
    process on an invalid string, and descriptions come from the caller. */
std::string validUtf8 (const std::string& text)
{
    static constexpr uint32_t kMinimum[] = { 0, 0, 0x80, 0x800, 0x10000 }; // shortest form per length
    std::string out;
    out.reserve (text.size());
    size_t i = 0;
    while (i < text.size())
    {
        const auto lead = static_cast<unsigned char> (text[i]);
        size_t length = 0; // 0: not a lead byte (or NUL)
        if (lead >= 0x01 && lead < 0x80)
            length = 1;
        else if ((lead & 0xE0) == 0xC0)
            length = 2;
        else if ((lead & 0xF0) == 0xE0)
            length = 3;
        else if ((lead & 0xF8) == 0xF0)
            length = 4;
        uint32_t codePoint = length == 1 ? lead : lead & (0x7Fu >> length);

        bool valid = length != 0 && i + length <= text.size();
        for (size_t k = 1; valid && k < length; ++k)
        {
            const auto next = static_cast<unsigned char> (text[i + k]);
            valid = (next & 0xC0) == 0x80;
            codePoint = (codePoint << 6) | (next & 0x3Fu);
        }
        valid = valid && (length == 1 || codePoint >= kMinimum[length]) && codePoint <= 0x10FFFF
                && (codePoint < 0xD800 || codePoint > 0xDFFF) && codePoint != 0xFFFE && codePoint != 0xFFFF;
        if (valid)
        {
            out.append (text, i, length);
            i += length;
        }
        else
        {
            out += '?';
            ++i;
        }
    }
    return out;
}

/** The BindShortcuts description: the application's name and the action
    ("Flubsound Pro: Boost +10%"), or the chord when no description is given. */
std::string shortcutDescription (const std::string& description, const KeyChord& chord)
{
    return "Flubsound Pro: " + (description.empty() ? chord.toString() : validUtf8 (description));
}

/** Shortcut ids are what desktops store the user's choice under. */
std::string shortcutId (int id) { return "flubsound-" + std::to_string (id); }

/** D-Bus object path syntax: "/" or "/a/b_1" (elements of [A-Za-z0-9_]). */
bool isValidObjectPath (const std::string& path)
{
    if (path.empty() || path.front() != '/')
        return false;
    if (path.size() == 1)
        return true;
    if (path.back() == '/')
        return false;
    for (size_t i = 1; i < path.size(); ++i)
    {
        const char c = path[i];
        const bool element = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (! element && ! (c == '/' && path[i - 1] != '/'))
            return false;
    }
    return true;
}

/** Request / session object path element for a unique bus name: ":1.42"
    becomes "1_42". */
std::string busPathElement (const std::string& uniqueName)
{
    std::string element = uniqueName.substr (uniqueName.empty() || uniqueName.front() != ':' ? 0 : 1);
    std::replace (element.begin(), element.end(), '.', '_');
    return element;
}

/** The session bus address: $DBUS_SESSION_BUS_ADDRESS, else the systemd
    user bus socket $XDG_RUNTIME_DIR/bus. libdbus's own fallback would also
    try X11 autolaunch, which can start a stray bus daemon; that is avoided. */
std::string sessionBusAddress()
{
    if (const char* address = std::getenv ("DBUS_SESSION_BUS_ADDRESS"); address != nullptr && *address != '\0')
        return address;

    const char* runtimeDir = std::getenv ("XDG_RUNTIME_DIR");
    if (runtimeDir == nullptr || runtimeDir[0] != '/')
        return {};
    const std::string socketPath = std::string (runtimeDir) + "/bus";
    struct stat info {};
    if (::stat (socketPath.c_str(), &info) != 0 || ! S_ISSOCK (info.st_mode))
        return {};

    // D-Bus address values escape every byte outside [-0-9A-Za-z_/.\*] as %XX.
    std::string address = "unix:path=";
    for (const char c : socketPath)
    {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'
                           || c == '/' || c == '.' || c == '\\' || c == '*';
        if (plain)
            address += c;
        else
        {
            char escaped[4];
            std::snprintf (escaped, sizeof (escaped), "%%%02X", static_cast<unsigned int> (static_cast<unsigned char> (c)));
            address += escaped;
        }
    }
    return address;
}

void log (const std::string& text) { std::fprintf (stderr, "Flubsound: global shortcuts: %s\n", text.c_str()); }
} // namespace portal

class PortalGlobalHotkeys final : public GlobalHotkeys
{
public:
    /** Changes closer together than this are bound as one set. */
    static constexpr std::chrono::milliseconds kSettleTime { 50 };
    /** How long the constructor waits for the start-up probe (the portal's
        GlobalShortcuts version). A running portal answers in milliseconds,
        and so does D-Bus when there is no portal at all (ServiceUnknown). A
        portal that D-Bus is still starting at login can take seconds; the
        constructor (on the message thread) does not wait for that, the
        answer is handled on the service thread when it comes. */
    static constexpr std::chrono::milliseconds kProbeWait { 250 };
    /** A probe that gets no answer at all within this is given up and sent
        again (hang guard for a portal that owns the name but never replies;
        D-Bus itself fails an activation after about 25 s). */
    static constexpr std::chrono::seconds kProbeReplyTimeout { 30 };
    /** Delay before the probe is repeated after a timeout or a failed
        activation; doubled per failure up to kProbeRetryMax. */
    static constexpr std::chrono::seconds kProbeRetryFirst { 2 }, kProbeRetryMax { 60 };

    explicit PortalGlobalHotkeys (std::chrono::milliseconds settleTimeIn = kSettleTime, std::chrono::milliseconds probeWait = kProbeWait)
        : settleTime (settleTimeIn), gnomeBackend (portal::desktopIsGnome())
    {
        api = dbus::Api::get();
        if (api == nullptr || ! start())
        {
            closeConnection();
            std::lock_guard<std::mutex> guard (mutex);
            stopped = true;
            return;
        }
        waitUntilProbed (probeWait);
    }

    ~PortalGlobalHotkeys() override
    {
        if (thread.joinable())
        {
            running = false;
            wake();
            thread.join();
        }
        if (connection != nullptr)
        {
            closeSession();
            api->connectionFlush (connection);
        }
        closeConnection();
        for (int& fd : wakePipe)
            if (fd >= 0)
                ::close (fd);
    }

    using GlobalHotkeys::registerHotkey;

    /** True while the portal can be asked: from construction until the start-up
        probe finds no GlobalShortcuts portal (or the session bus is lost),
        and again once a portal with it appears on the bus. When the probe
        is still unanswered after the constructor's wait (a portal starting
        at login), this is true and requests wait for the answer. */
    bool isSupported() const override { return supported; }

    /** GlobalShortcuts interface version the portal reported (0 = none yet). */
    uint32_t getPortalVersion() const noexcept { return portalVersion; }

    bool registerHotkey (int id, const KeyChord& chord, const std::string& description, std::function<void()> callback) override
    {
        Shortcut shortcut { portal::triggerFor (chord), portal::shortcutDescription (description, chord) };
        bool accepted = false;
        if (callback && detail::isValidChord (chord) && ! shortcut.trigger.empty())
        {
            // Under the lock the service thread takes to switch support off,
            // so a request is either refused here or reported by it.
            std::lock_guard<std::mutex> guard (mutex);
            if (supported && ! stopped)
            {
                wanted[id] = Wanted { std::move (shortcut), std::move (callback) };
                noteChange();
                accepted = true;
            }
        }
        if (! accepted)
        {
            reportBinding (id, BindingResult::Status::Unavailable);
            return false;
        }
        wake();
        return true;
    }

    void unregisterHotkey (int id) override
    {
        {
            std::lock_guard<std::mutex> guard (mutex);
            if (wanted.erase (id) == 0)
                return;
            noteChange();
        }
        wake();
    }

    void unregisterAll() override
    {
        {
            std::lock_guard<std::mutex> guard (mutex);
            if (wanted.empty())
                return;
            wanted.clear();
            noteChange();
        }
        wake();
    }

    /** Sends pending changes now instead of after the settle time (tests). */
    void applyNow()
    {
        {
            std::lock_guard<std::mutex> guard (mutex);
            applyImmediately = true;
        }
        wake();
    }

    /** Waits, at most 'timeout', until every change so far has been sent and
        the portal has answered, or the service has stopped (tests). */
    bool waitUntilSettled (std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock (mutex);
        return stateCondition.wait_for (lock, timeout, [this] { return settledRevision == revision || stopped; });
    }

    /** Waits, at most 'timeout', until the first probe has been answered (or
        failed), so isSupported() is no longer tentative, or the service has
        stopped. */
    bool waitUntilProbed (std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock (mutex);
        return stateCondition.wait_for (lock, timeout, [this] { return probeAnswered || stopped; });
    }

private:
    struct Shortcut
    {
        std::string trigger;     // preferred_trigger, XDG shortcuts format
        std::string description; // shown in the desktop's dialog / settings
        bool operator== (const Shortcut&) const = default;
    };

    struct Wanted
    {
        Shortcut shortcut;
        std::function<void()> callback;
    };

    enum class Stage
    {
        Idle,
        CreatingSession,
        Binding
    };

    /** The one portal request in flight (service thread only). */
    struct Pending
    {
        Stage stage = Stage::Idle;
        uint32_t serial = 0;
        std::string requestPath, returnedPath;
        std::map<int, Shortcut> shortcuts;
        uint64_t revision = 0;
    };

    /** What the service knows about the portal (service thread only). */
    enum class Probe
    {
        Waiting, // Properties.Get sent, no answer yet
        Retry,   // no usable answer (timeout, failed activation): asked again at probeRetryAt
        Present, // GlobalShortcuts answered: requests are bound
        Absent,  // no portal or no GlobalShortcuts: unsupported until a portal appears
        Gone     // the portal left the bus: asked again when it comes back
    };

    static constexpr uint64_t kReapply = ~uint64_t (0);

    /** Connects and starts the service thread, which probes the portal. The
        match rules (NameOwnerChanged included) are in place whatever the
        probe finds, so a portal that appears later is noticed. */
    bool start()
    {
        const std::string address = portal::sessionBusAddress();
        if (address.empty())
            return false;
        dbus::Error error;
        api->errorInit (&error);
        connection = api->connectionOpenPrivate (address.c_str(), &error);
        if (connection != nullptr)
        {
            api->connectionSetExitOnDisconnect (connection, 0);
            if (api->busRegister (connection, &error) == 0)
                closeConnection();
        }
        api->errorFree (&error);
        if (connection == nullptr)
            return false;

        uniqueName = dbus::str (api->busGetUniqueName (connection));
        int fd = -1;
        if (api->connectionGetUnixFd (connection, &fd) == 0 || ::pipe2 (wakePipe, O_CLOEXEC | O_NONBLOCK) != 0)
            return false;

        for (const char* rule : portal::kMatchRules)
            api->busAddMatch (connection, rule, nullptr); // asynchronous; ordered before any portal call
        api->connectionFlush (connection);

        busFd = fd;
        supported = true; // tentatively, until the probe answers
        running = true;
        thread = std::thread ([this] { run(); });
        return true;
    }

    void closeConnection()
    {
        if (connection == nullptr)
            return;
        api->connectionClose (connection); // private connections must be closed before the last unref
        api->connectionUnref (connection);
        connection = nullptr;
    }

    void wake()
    {
        if (wakePipe[1] < 0)
            return;
        const char byte = 0;
        [[maybe_unused]] const auto written = ::write (wakePipe[1], &byte, 1);
    }

    void noteChange() // mutex held
    {
        ++revision;
        lastChange = std::chrono::steady_clock::now();
    }

    void markSettled (uint64_t rev)
    {
        {
            std::lock_guard<std::mutex> guard (mutex);
            settledRevision = rev;
        }
        stateCondition.notify_all();
    }

    /** True when the wanted set changed after the batch 'batchRevision' was
        sent: its answer is not the outcome of what is wanted now, so it is
        not reported. The next applyWantedSet() re-reports it (same set) or
        binds the new set and reports that. */
    bool superseded (uint64_t batchRevision)
    {
        std::lock_guard<std::mutex> guard (mutex);
        return revision != batchRevision;
    }

    //--------------------------------------------------------------------------
    // Service thread
    void run()
    {
        pollfd fds[2] = { { busFd, POLLIN, 0 }, { wakePipe[0], POLLIN, 0 } };
        sendProbe();
        while (running)
        {
            if (api->connectionReadWrite (connection, 0) == 0) // reads and writes what it can, never blocks
            {
                onDisconnected();
                break;
            }
            while (dbus::Message* message = api->connectionPopMessage (connection))
            {
                const dbus::MessageRef owner (message);
                handleMessage (message);
            }

            const int probeTimeoutMs = serviceProbe();
            const int applyTimeoutMs = applyWantedSet();
            int timeoutMs = probeTimeoutMs < 0 ? applyTimeoutMs : (applyTimeoutMs < 0 ? probeTimeoutMs : std::min (probeTimeoutMs, applyTimeoutMs));
            if (api->connectionGetDispatchStatus (connection) == dbus::kDispatchDataRemains)
                timeoutMs = 0; // already read into libdbus's queue: poll would not see it
            fds[0].events = static_cast<short> (POLLIN | (api->connectionHasMessagesToSend (connection) != 0 ? POLLOUT : 0));
            ::poll (fds, 2, timeoutMs);
            if ((fds[1].revents & POLLIN) != 0)
            {
                char buffer[16];
                while (::read (wakePipe[0], buffer, sizeof (buffer)) > 0) {}
            }
        }
    }

    /** The session bus connection is gone (dbus restart): nothing can be
        bound any more. Every wanted shortcut is reported Unavailable, and
        from now on the service is unsupported, so later requests are refused
        (and reported) at once instead of waiting forever. */
    void onDisconnected()
    {
        portal::log ("lost the connection to the session bus; shortcuts stop working");
        std::map<int, Shortcut> lost;
        {
            std::lock_guard<std::mutex> guard (mutex);
            stopped = true;
            supported = false;
            probeAnswered = true;
            for (const auto& [id, w] : wanted)
                lost.emplace (id, w.shortcut);
        }
        stateCondition.notify_all();
        report (lost, BindingResult::Status::Unavailable);
    }

    //--------------------------------------------------------------------------
    // Probe: Properties.Get (GlobalShortcuts, "version"), sent without
    // blocking and answered in handleMessage. It also learns the portal's
    // unique bus name, the only sender whose signals are accepted.
    void sendProbe()
    {
        dbus::MessageRef call (api->messageNewMethodCall (portal::kService, portal::kObjectPath, "org.freedesktop.DBus.Properties", "Get"));
        dbus::Iter args;
        uint32_t serial = 0;
        if (call != nullptr)
            api->iterInitAppend (call.get(), &args);
        if (call != nullptr && dbus::appendBasic (*api, &args, dbus::kTypeString, portal::kShortcutsInterface)
            && dbus::appendBasic (*api, &args, dbus::kTypeString, "version") && api->connectionSend (connection, call.get(), &serial) != 0)
        {
            probe = Probe::Waiting;
            probeSerial = serial;
            probeDeadline = std::chrono::steady_clock::now() + kProbeReplyTimeout;
            return;
        }
        onProbeFailed ("could not ask the portal for its version");
    }

    /** Sends a due retry or gives up on an unanswered probe; returns the poll
        timeout in ms until the next of those (-1 = none). */
    int serviceProbe()
    {
        const auto now = std::chrono::steady_clock::now();
        if (probe == Probe::Retry && now >= probeRetryAt)
            sendProbe();
        else if (probe == Probe::Waiting && now >= probeDeadline)
            onProbeFailed ("the portal did not answer");

        const auto msUntil = [now] (std::chrono::steady_clock::time_point t)
        { return static_cast<int> (std::max<int64_t> (0, std::chrono::ceil<std::chrono::milliseconds> (t - now).count())); };
        if (probe == Probe::Waiting)
            return msUntil (probeDeadline);
        if (probe == Probe::Retry)
            return msUntil (probeRetryAt);
        return -1;
    }

    void onProbeReply (dbus::Message* message, int type)
    {
        if (type == dbus::kMessageMethodReturn)
        {
            uint32_t version = 0;
            dbus::Iter it, value;
            if (api->iterInit (message, &it) != 0 && api->iterGetArgType (&it) == dbus::kTypeVariant)
            {
                api->iterRecurse (&it, &value);
                if (api->iterGetArgType (&value) == dbus::kTypeUInt32)
                    api->iterGetBasic (&value, &version);
            }
            const std::string owner = dbus::str (api->messageGetSender (message));
            if (version > 0 && ! owner.empty())
                onPortalFound (owner, version);
            else
                onPortalAbsent ("the portal reports no GlobalShortcuts version");
            return;
        }

        // No portal on the bus (and none D-Bus can start) or no GlobalShortcuts
        // interface: a definite no. Anything else (TimedOut while D-Bus starts
        // the portal, a failed activation, NoReply) is asked again later.
        const std::string name = dbus::str (api->messageGetErrorName (message));
        static const std::set<std::string> absent {
            "org.freedesktop.DBus.Error.ServiceUnknown",   "org.freedesktop.DBus.Error.NameHasNoOwner",
            "org.freedesktop.DBus.Error.InvalidArgs",      "org.freedesktop.DBus.Error.UnknownInterface",
            "org.freedesktop.DBus.Error.UnknownProperty",  "org.freedesktop.DBus.Error.UnknownMethod",
            "org.freedesktop.DBus.Error.UnknownObject",
        };
        if (absent.count (name) != 0)
            onPortalAbsent ("no GlobalShortcuts portal (" + name + ")");
        else
            onProbeFailed ("the portal could not be asked (" + name + ")");
    }

    /** A GlobalShortcuts portal answered: supported, and whatever is wanted
        (requested while the probe was out, or before the portal went away)
        is bound and reported. */
    void onPortalFound (const std::string& owner, uint32_t version)
    {
        if (probeFailureLogged)
            portal::log ("the portal answered; binding the shortcuts");
        portalOwner = owner;
        portalVersion = version;
        probe = Probe::Present;
        probeRetryDelay = kProbeRetryFirst;
        probeFailureLogged = false;
        {
            std::lock_guard<std::mutex> guard (mutex);
            supported = true;
            probeAnswered = true;
        }
        stateCondition.notify_all();
        handledRevision = kReapply; // binds (or re-reports) the wanted set
    }

    /** No GlobalShortcuts portal: unsupported (requests are refused at once)
        until NameOwnerChanged shows a portal appearing, which is probed
        again. Shortcuts already requested are reported Unavailable but kept,
        so they are bound if a portal turns up. */
    void onPortalAbsent (const std::string& reason)
    {
        probe = Probe::Absent;
        std::map<int, Shortcut> lost;
        {
            std::lock_guard<std::mutex> guard (mutex);
            supported = false;
            probeAnswered = true;
            for (const auto& [id, w] : wanted)
                lost.emplace (id, w.shortcut);
        }
        stateCondition.notify_all();
        if (! lost.empty())
        {
            portal::log (reason);
            report (lost, BindingResult::Status::Unavailable);
        }
    }

    /** No usable answer: still supported (the portal may be starting), asked
        again after a growing delay. Requested shortcuts are reported
        Unavailable meanwhile and bound once the portal answers. */
    void onProbeFailed (const std::string& reason)
    {
        probe = Probe::Retry;
        probeRetryAt = std::chrono::steady_clock::now() + probeRetryDelay;
        probeRetryDelay = std::min<std::chrono::seconds> (probeRetryDelay * 2, kProbeRetryMax);
        if (! probeFailureLogged)
            portal::log (reason + "; asking again");
        probeFailureLogged = true;
        std::map<int, Shortcut> waiting;
        {
            std::lock_guard<std::mutex> guard (mutex);
            probeAnswered = true;
            for (const auto& [id, w] : wanted)
                waiting.emplace (id, w.shortcut);
        }
        stateCondition.notify_all();
        report (waiting, BindingResult::Status::Unavailable);
    }

    //--------------------------------------------------------------------------
    /** Starts binding the wanted set once it has settled; returns the poll
        timeout in ms (-1 = until woken). */
    int applyWantedSet()
    {
        if (pending.stage != Stage::Idle || probe == Probe::Waiting)
            return -1; // the probe's answer applies the set

        std::map<int, Shortcut> want;
        uint64_t rev = 0;
        {
            std::lock_guard<std::mutex> guard (mutex);
            if (revision == handledRevision)
                return -1;
            const auto sinceChange = std::chrono::steady_clock::now() - lastChange;
            if (! applyImmediately && sinceChange < settleTime)
                return static_cast<int> (std::chrono::ceil<std::chrono::milliseconds> (settleTime - sinceChange).count());
            applyImmediately = false;
            rev = revision;
            for (const auto& [id, w] : wanted)
                want.emplace (id, w.shortcut);
        }
        handledRevision = rev;

        if (probe != Probe::Present)
        {
            // No portal to ask (retrying, absent or gone): nothing can be
            // bound now; the set is bound when a portal answers.
            report (want, BindingResult::Status::Unavailable);
            markSettled (rev);
            return -1;
        }
        if (want == bound && (want.empty() || ! session.empty()))
        {
            // e.g. unregisterAll() + the same registrations again: nothing to
            // bind, the outcome is the previous one.
            for (const auto& [id, result] : results)
                reportBinding (id, result.status, result.trigger);
            markSettled (rev);
            return -1;
        }
        closeSession();
        bound.clear();
        results.clear();
        if (! want.empty() && ! createSession (want, rev))
            report (want, BindingResult::Status::Unavailable);
        if (want.empty() || pending.stage == Stage::Idle)
            markSettled (rev);
        return -1;
    }

    /** Reports every id of 'shortcuts' with one status (service thread). */
    void report (const std::map<int, Shortcut>& shortcuts, BindingResult::Status status)
    {
        for (const auto& entry : shortcuts)
            reportBinding (entry.first, status);
    }

    /** report() unless the batch was superseded (see superseded()). */
    void reportBatch (const Pending& batch, BindingResult::Status status)
    {
        if (! superseded (batch.revision))
            report (batch.shortcuts, status);
    }

    std::string nextToken() { return "flubsound" + std::to_string (++tokenCounter); }

    bool send (dbus::MessageRef call, Stage stage, const std::string& requestToken, std::map<int, Shortcut> shortcuts, uint64_t rev)
    {
        uint32_t serial = 0;
        if (api->connectionSend (connection, call.get(), &serial) == 0)
        {
            portal::log ("could not send a request to the portal");
            return false;
        }
        pending = Pending { stage, serial, std::string (portal::kObjectPath) + "/request/" + portal::busPathElement (uniqueName) + "/" + requestToken,
                            {}, std::move (shortcuts), rev };
        return true;
    }

    bool createSession (std::map<int, Shortcut> want, uint64_t rev)
    {
        dbus::MessageRef call (api->messageNewMethodCall (portal::kService, portal::kObjectPath, portal::kShortcutsInterface, "CreateSession"));
        if (call == nullptr)
            return false;
        const std::string requestToken = nextToken();
        dbus::Iter args, options;
        api->iterInitAppend (call.get(), &args);
        const bool built = api->iterOpenContainer (&args, dbus::kTypeArray, "{sv}", &options) != 0
                           && dbus::appendDictEntry (*api, &options, "handle_token", dbus::kTypeString, requestToken)
                           && dbus::appendDictEntry (*api, &options, "session_handle_token", dbus::kTypeString, nextToken())
                           && api->iterCloseContainer (&args, &options) != 0;
        return built && send (std::move (call), Stage::CreatingSession, requestToken, std::move (want), rev);
    }

    bool bindShortcuts()
    {
        dbus::MessageRef call (api->messageNewMethodCall (portal::kService, portal::kObjectPath, portal::kShortcutsInterface, "BindShortcuts"));
        if (call == nullptr)
            return false;
        const std::string requestToken = nextToken();
        dbus::Iter args, list, options;
        api->iterInitAppend (call.get(), &args);
        bool built = dbus::appendBasic (*api, &args, dbus::kTypeObjectPath, session)
                     && api->iterOpenContainer (&args, dbus::kTypeArray, "(sa{sv})", &list) != 0;
        for (const auto& [id, shortcut] : pending.shortcuts)
        {
            dbus::Iter entry, properties;
            built = built && api->iterOpenContainer (&list, dbus::kTypeStruct, nullptr, &entry) != 0
                    && dbus::appendBasic (*api, &entry, dbus::kTypeString, portal::shortcutId (id))
                    && api->iterOpenContainer (&entry, dbus::kTypeArray, "{sv}", &properties) != 0
                    && dbus::appendDictEntry (*api, &properties, "description", dbus::kTypeString, shortcut.description)
                    && dbus::appendDictEntry (*api, &properties, "preferred_trigger", dbus::kTypeString, shortcut.trigger)
                    && api->iterCloseContainer (&entry, &properties) != 0 && api->iterCloseContainer (&list, &entry) != 0;
        }
        // parent_window "": the interface gives no window handle to pass.
        built = built && api->iterCloseContainer (&args, &list) != 0 && dbus::appendBasic (*api, &args, dbus::kTypeString, "")
                && api->iterOpenContainer (&args, dbus::kTypeArray, "{sv}", &options) != 0
                && dbus::appendDictEntry (*api, &options, "handle_token", dbus::kTypeString, requestToken)
                && api->iterCloseContainer (&args, &options) != 0;
        return built && send (std::move (call), Stage::Binding, requestToken, std::move (pending.shortcuts), pending.revision);
    }

    void closeSession()
    {
        if (session.empty())
            return;
        if (dbus::MessageRef call { api->messageNewMethodCall (portal::kService, session.c_str(), portal::kSessionInterface, "Close") })
        {
            api->messageSetNoReply (call.get(), 1);
            api->connectionSend (connection, call.get(), nullptr);
        }
        session.clear();
    }

    void finishPending()
    {
        const uint64_t rev = pending.revision;
        pending = Pending();
        markSettled (rev);
    }

    void handleMessage (dbus::Message* message)
    {
        const int type = api->messageGetType (message);
        if ((type == dbus::kMessageMethodReturn || type == dbus::kMessageError) && probe == Probe::Waiting
            && api->messageGetReplySerial (message) == probeSerial)
        {
            onProbeReply (message, type);
            return;
        }
        if ((type == dbus::kMessageMethodReturn || type == dbus::kMessageError) && pending.stage != Stage::Idle
            && api->messageGetReplySerial (message) == pending.serial)
        {
            dbus::Iter it;
            std::string text;
            if (type == dbus::kMessageMethodReturn)
            {
                if (api->iterInit (message, &it) != 0 && dbus::readString (*api, &it, text))
                    pending.returnedPath = text; // normally == requestPath (portals older than 0.9 differ)
                return;
            }
            if (api->iterInit (message, &it) != 0)
                dbus::readString (*api, &it, text);
            portal::log (std::string (pending.stage == Stage::Binding ? "BindShortcuts" : "CreateSession") + " failed: "
                         + dbus::str (api->messageGetErrorName (message)) + " " + text);
            reportBatch (pending, BindingResult::Status::Unavailable);
            finishPending();
            return;
        }
        if (type != dbus::kMessageSignal)
            return;

        const std::string interfaceName = dbus::str (api->messageGetInterface (message));
        const std::string member = dbus::str (api->messageGetMember (message));
        const std::string path = dbus::str (api->messageGetPath (message));
        const std::string sender = dbus::str (api->messageGetSender (message));

        if (interfaceName == "org.freedesktop.DBus" && member == "NameOwnerChanged" && sender == "org.freedesktop.DBus")
            onPortalOwnerChanged (message);
        else if (portalOwner.empty() || sender != portalOwner)
            return; // nobody but the portal may press our shortcuts
        else if (interfaceName == portal::kRequestInterface && member == "Response" && pending.stage != Stage::Idle
                 && (path == pending.requestPath || (! pending.returnedPath.empty() && path == pending.returnedPath)))
            onResponse (message);
        else if (interfaceName == portal::kShortcutsInterface && member == "Activated")
            onActivated (message);
        else if (interfaceName == portal::kSessionInterface && member == "Closed" && ! session.empty() && path == session)
        {
            portal::log ("the desktop closed the session; shortcuts are requested again when the hotkey settings change");
            report (bound, BindingResult::Status::Declined);
            // A BindShortcuts still waiting for its answer binds into the
            // closed session: its shortcuts can never fire, so they are
            // declined now and its late Response is ignored.
            if (pending.stage == Stage::Binding)
            {
                reportBatch (pending, BindingResult::Status::Declined);
                finishPending();
            }
            session.clear();
            bound.clear();
            results.clear();
        }
    }

    void onResponse (dbus::Message* message)
    {
        dbus::Iter response;
        uint32_t code = 2;
        if (api->iterInit (message, &response) != 0 && api->iterGetArgType (&response) == dbus::kTypeUInt32)
        {
            api->iterGetBasic (&response, &code);
            api->iterNext (&response);
        }

        if (pending.stage == Stage::CreatingSession)
        {
            std::string handle; // 's' in the spec, 'o' in some back-ends
            if (code == 0 && dbus::readVardictString (*api, &response, "session_handle", handle) && portal::isValidObjectPath (handle))
            {
                session = handle;
                if (bindShortcuts())
                    return;
                reportBatch (pending, BindingResult::Status::Unavailable);
            }
            else
            {
                portal::log ("the desktop did not open a session (response " + std::to_string (code) + ")");
                reportBatch (pending, code != 0 ? BindingResult::Status::Declined : BindingResult::Status::Unavailable);
            }
            finishPending();
            return;
        }

        // Each bound shortcut comes back as (id, {description, trigger_description});
        // the trigger is the desktop's own text for the key it bound.
        std::map<std::string, std::string> boundTriggers;
        dbus::Iter list;
        if (code == 0 && dbus::findInVardict (*api, &response, "shortcuts", list) && api->iterGetArgType (&list) == dbus::kTypeArray)
        {
            dbus::Iter items;
            api->iterRecurse (&list, &items);
            for (; api->iterGetArgType (&items) == dbus::kTypeStruct; api->iterNext (&items))
            {
                dbus::Iter item;
                std::string shortcut, trigger;
                api->iterRecurse (&items, &item);
                if (! dbus::readString (*api, &item, shortcut))
                    continue;
                if (api->iterNext (&item) != 0)
                    dbus::readVardictString (*api, &item, "trigger_description", trigger);
                boundTriggers[shortcut] = trigger;
            }
        }
        if (code != 0)
            portal::log (code == 1 ? "the user declined the shortcuts" : "the desktop refused the shortcuts (response " + std::to_string (code) + ")");

        // Answered after the wanted set changed again: remembered (so the
        // same set is not bound twice) but not reported, see superseded().
        const bool current = ! superseded (pending.revision);
        results.clear();
        for (const auto& [id, shortcut] : pending.shortcuts)
        {
            BindingResult result { id, BindingResult::Status::Declined, {} };
            const auto found = boundTriggers.find (portal::shortcutId (id));
            if (found == boundTriggers.end())
            {
                if (code == 0)
                    portal::log ("\"" + shortcut.description + "\" (" + shortcut.trigger + ") was not bound by the desktop");
            }
            else if (found->second.empty())
            {
                // No trigger_description. GNOME leaves it out only when the
                // user removed every key in its dialog (or set three or
                // more), so the shortcut may have no key: not "Registered".
                // Other back-ends send none at all (bound as requested).
                if (gnomeBackend)
                    portal::log ("\"" + shortcut.description + "\" was bound without a key the desktop names");
                else
                    result.status = BindingResult::Status::Registered;
            }
            else if (portal::sameTrigger (shortcut.trigger, found->second))
                result.status = BindingResult::Status::Registered;
            else
                result = BindingResult { id, BindingResult::Status::Reassigned, found->second };
            results[id] = result;
            if (current)
                reportBinding (id, result.status, result.trigger);
        }

        // Remembered even when refused, so re-registering the same set does
        // not ask the user again; a changed set starts a new session.
        bound = std::move (pending.shortcuts);
        finishPending();
    }

    void onActivated (dbus::Message* message)
    {
        dbus::Iter it;
        std::string sessionHandle, shortcut;
        if (api->iterInit (message, &it) == 0 || ! dbus::readString (*api, &it, sessionHandle) || api->iterNext (&it) == 0
            || ! dbus::readString (*api, &it, shortcut))
            return;
        if (session.empty() || sessionHandle != session)
            return; // a closed session's late signal

        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> guard (mutex);
            for (const auto& [id, w] : wanted)
                if (portal::shortcutId (id) == shortcut)
                    callback = w.callback;
        }
        if (callback) // outside the lock: a callback may (un)register
            callback();
    }

    /** The portal restarted, went away or appeared: its sessions are gone. A
        new instance is probed (it may lack GlobalShortcuts), and the wanted
        set is bound again once it answers. */
    void onPortalOwnerChanged (dbus::Message* message)
    {
        dbus::Iter it;
        std::string name, oldOwner, newOwner;
        if (api->iterInit (message, &it) == 0 || ! dbus::readString (*api, &it, name) || name != portal::kService
            || api->iterNext (&it) == 0 || ! dbus::readString (*api, &it, oldOwner) || api->iterNext (&it) == 0
            || ! dbus::readString (*api, &it, newOwner))
            return;
        portalOwner = newOwner;
        session.clear();
        bound.clear();
        results.clear();
        if (newOwner.empty() && probe == Probe::Present)
            report (wantedShortcuts(), BindingResult::Status::Unavailable);
        if (pending.stage != Stage::Idle)
            finishPending();
        if (newOwner.empty())
        {
            if (probe != Probe::Absent)
                probe = Probe::Gone; // an Absent service stays unsupported
        }
        else
        {
            probeRetryDelay = kProbeRetryFirst;
            sendProbe();
        }
    }

    std::map<int, Shortcut> wantedShortcuts()
    {
        std::map<int, Shortcut> want;
        std::lock_guard<std::mutex> guard (mutex);
        for (const auto& [id, w] : wanted)
            want.emplace (id, w.shortcut);
        return want;
    }

    const dbus::Api* api = nullptr;
    const std::chrono::milliseconds settleTime;
    const bool gnomeBackend; // XDG_CURRENT_DESKTOP names GNOME (see onResponse)
    dbus::Connection* connection = nullptr;
    std::string uniqueName;
    std::atomic<uint32_t> portalVersion { 0 };
    std::atomic<bool> supported { false }; // written under 'mutex'
    int busFd = -1;
    int wakePipe[2] = { -1, -1 };
    std::atomic<bool> running { false };
    std::thread thread;

    // Shared with the callers of registerHotkey / unregister* (mutex).
    std::mutex mutex;
    std::condition_variable stateCondition; // settledRevision, probeAnswered, stopped
    std::map<int, Wanted> wanted;
    uint64_t revision = 0, settledRevision = 0;
    std::chrono::steady_clock::time_point lastChange;
    bool applyImmediately = false;
    bool probeAnswered = false; // the first probe was answered (or failed)
    bool stopped = false;       // no service thread: never started, or the bus connection was lost

    // Service thread only.
    std::string portalOwner;
    std::string session;
    std::map<int, Shortcut> bound;
    std::map<int, BindingResult> results; // the outcome for each id of 'bound'
    Pending pending;
    uint64_t handledRevision = 0;
    unsigned int tokenCounter = 0;
    Probe probe = Probe::Waiting;
    uint32_t probeSerial = 0;
    std::chrono::steady_clock::time_point probeDeadline, probeRetryAt;
    std::chrono::seconds probeRetryDelay = kProbeRetryFirst;
    bool probeFailureLogged = false;
};

//==============================================================================
// AppAudioRouter - pactl (PulseAudio / PipeWire-pulse)
//==============================================================================
class LinuxAppAudioRouter final : public AppAudioRouter
{
public:
    LinuxAppAudioRouter()
        : havePactl (pactl::isExecutableInPath ("pactl")),
          havePipeWireTools (pactl::isExecutableInPath ("pw-dump") && pactl::isExecutableInPath ("pw-link"))
    {
    }

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

    /** Links each mapped strip sink's monitor to Flubsound's PipeWire input
        (see pipewire::planMonitorLinks). pw-dump costs a few ms of CPU on a
        busy graph, so an unchanged map is re-checked every kRecheck (links
        vanish when the audio device re-opens) and at once after a pass that
        changed something, to confirm it. What cannot be linked is logged to
        stderr once per change of the message. */
    bool connectEndpointInputs (const std::vector<EndpointInput>& inputs, std::string& status) override
    {
        const bool anyMapped = std::any_of (inputs.begin(), inputs.end(), [] (const EndpointInput& i) { return i.firstInputChannel >= 0; });
        if (! anyMapped && ourLinks.empty())
        {
            lastInputs = inputs;
            return report (true, {}, status);
        }

        if (! havePipeWireTools)
            return report (false, pipewire::kNoToolsMessage, status);

        const auto now = std::chrono::steady_clock::now();
        if (inputs == lastInputs && now < nextPass)
        {
            status = lastStatus;
            return lastOk;
        }
        lastInputs = inputs;
        nextPass = now + kRecheck;

        const auto dump = pactl::run ("LC_ALL=C pw-dump 2>/dev/null");
        pipewire::Graph graph;
        std::string error;
        if (dump.exitCode != 0)
            return report (false, "Cannot reach PipeWire (pw-dump failed), so the Flubsound sinks are not linked to Flubsound's input. "
                                  "With PulseAudio, choose a sink's monitor as the input device instead.", status);
        if (! pipewire::parseDump (dump.output, graph, error))
            return report (false, error, status);

        const auto plan = pipewire::planMonitorLinks (graph, static_cast<uint32_t> (::getpid()), inputs);
        auto problems = plan.problems;

        std::set<pipewire::PortLink> existing, wanted (plan.wanted.begin(), plan.wanted.end());
        std::set<uint32_t> portIds;
        for (const auto& link : graph.links)
            existing.insert ({ link.outputPort, link.inputPort });
        for (const auto& port : graph.ports)
            portIds.insert (port.id);

        bool changed = false;
        for (auto it = ourLinks.begin(); it != ourLinks.end();)
        {
            if (wanted.count (*it) != 0 && portIds.count (it->outputPort) != 0 && portIds.count (it->inputPort) != 0)
            {
                ++it;
                continue;
            }
            // No longer wanted (the map changed): remove it. Gone with its
            // ports (the device re-opened): forget it.
            if (existing.count (*it) != 0)
            {
                pactl::run (pipewire::linkCommand (*it, true));
                changed = true;
            }
            it = ourLinks.erase (it);
        }

        int created = 0;
        for (const auto& link : plan.wanted)
        {
            if (existing.count (link) != 0)
                continue;
            const auto result = pactl::run (pipewire::linkCommand (link, false));
            if (result.exitCode == 0 || pipewire::alreadyLinked (result.output))
            {
                ourLinks.insert (link);
                ++created;
            }
            else
            {
                problems.push_back ("pw-link could not link port " + std::to_string (link.outputPort) + " to " + std::to_string (link.inputPort)
                                    + ": " + pactl::firstLine (result.output));
            }
        }

        if (created > 0)
        {
            for (const auto& sink : plan.sinks)
                pipewire::log ("'" + sink.sinkName + "' monitor -> Flubsound input channels " + std::to_string (sink.firstChannel + 1) + "-"
                               + std::to_string (sink.firstChannel + sink.channels));
            changed = true;
        }
        if (changed)
            nextPass = now; // confirm on the next pass

        std::string text;
        for (const auto& problem : problems)
            text += (text.empty() ? "" : "; ") + problem;
        if (! text.empty())
            text += ".";
        return report (problems.empty(), text, status);
    }

private:
    static constexpr std::chrono::seconds kRecheck { 10 };

    bool report (bool ok, const std::string& text, std::string& status)
    {
        if (! text.empty() && text != lastStatus)
            pipewire::log (text);
        lastOk = ok;
        lastStatus = text;
        status = text;
        return ok;
    }

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

    // connectEndpointInputs (one caller thread at a time)
    const bool havePipeWireTools;
    std::set<pipewire::PortLink> ourLinks; // links this router made and still wants
    std::vector<EndpointInput> lastInputs;
    std::chrono::steady_clock::time_point nextPass {};
    std::string lastStatus;
    bool lastOk = true;
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
// AutoStart - XDG autostart entry (unnamed namespace; tests reach the helpers)
//==============================================================================
namespace autostart
{
constexpr const char* kFileName = "flubsound-pro.desktop";
constexpr size_t kMaxEntryBytes = 64u * 1024u; // a sane .desktop file is < 1 KB

std::string withoutTrailingSlashes (std::string path)
{
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    return path;
}

/** $XDG_CONFIG_HOME when set to an absolute path (the XDG Base Directory spec
    says relative values are invalid and must be ignored), else $HOME/.config,
    else the passwd entry's home + "/.config". Empty when no home is known. */
std::string configHome()
{
    if (const char* xdg = std::getenv ("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] == '/')
        return withoutTrailingSlashes (xdg);

    std::string home;
    if (const char* h = std::getenv ("HOME"); h != nullptr && h[0] == '/')
        home = h;
    else if (const passwd* pw = ::getpwuid (::getuid()); pw != nullptr && pw->pw_dir != nullptr && pw->pw_dir[0] == '/')
        home = pw->pw_dir; // message thread only: getpwuid is not reentrant

    return home.empty() ? std::string() : withoutTrailingSlashes (home) + "/.config";
}

std::string directory()
{
    const auto base = configHome();
    return base.empty() ? base : base + "/autostart";
}

std::string entryPath()
{
    const auto dir = directory();
    return dir.empty() ? dir : dir + "/" + kFileName;
}

/** Strict UTF-8 check (no overlong forms, surrogates or code points above
    U+10FFFF): Desktop Entry values must be UTF-8. */
bool isValidUtf8 (const std::string& text)
{
    size_t i = 0;
    while (i < text.size())
    {
        const auto lead = static_cast<unsigned char> (text[i]);
        size_t length = 0;
        uint32_t cp = 0;
        if (lead < 0x80)
        {
            ++i;
            continue;
        }
        if (lead >= 0xC2 && lead <= 0xDF)
        {
            length = 2;
            cp = lead & 0x1Fu;
        }
        else if (lead >= 0xE0 && lead <= 0xEF)
        {
            length = 3;
            cp = lead & 0x0Fu;
        }
        else if (lead >= 0xF0 && lead <= 0xF4)
        {
            length = 4;
            cp = lead & 0x07u;
        }
        else
        {
            return false;
        }

        if (i + length > text.size())
            return false;
        for (size_t k = 1; k < length; ++k)
        {
            const auto c = static_cast<unsigned char> (text[i + k]);
            if ((c & 0xC0u) != 0x80u)
                return false;
            cp = (cp << 6) | (c & 0x3Fu);
        }

        if ((length == 3 && cp < 0x800) || (length == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += length;
    }
    return true;
}

/** The Exec value that starts `program` with no arguments, following the
    Desktop Entry spec ("The Exec key"):
      1. the path is always double-quoted, and inside the quotes the four
         characters " ` $ \ get a backslash;
      2. the value is then a string value, whose escape rule doubles every
         backslash - so a quote is written \\" and a backslash \\\;
      3. '%' starts a field code and is written %%.
    Verified against GLib 2.80 (GKeyFile + g_shell_parse_argv). One GLib
    quirk remains: it checks that the program exists BEFORE expanding %%, so
    GNOME skips an entry whose path contains '%' (KDE starts it).
    Fails (with a user-presentable error) for what an Exec line cannot carry:
    an empty or relative path, invalid UTF-8, control characters. */
bool execValueFor (const std::string& program, std::string& exec, std::string& error)
{
    if (program.empty() || program.front() != '/')
    {
        error = "The program path for the start-up entry must be absolute: \"" + program + "\".";
        return false;
    }
    if (! isValidUtf8 (program))
    {
        error = "The program path is not valid UTF-8, which a start-up (.desktop) entry cannot store.";
        return false;
    }

    exec = "\"";
    for (const char c : program)
    {
        const auto u = static_cast<unsigned char> (c);
        if (u < 0x20 || u == 0x7F)
        {
            error = "The program path contains a control character, which a start-up (.desktop) entry cannot store.";
            return false;
        }

        switch (c)
        {
            case '"':
            case '`':
            case '$': exec += "\\\\"; exec += c; break; // quoting \x, then string-escaped backslash
            case '\\': exec += "\\\\\\\\"; break;       // quoting \\, each string-escaped
            case '%': exec += "%%"; break;
            default: exec += c; break;
        }
    }
    exec += '"';
    return true;
}

std::string desktopEntry (const std::string& execValue)
{
    return "[Desktop Entry]\n"
           "Type=Application\n"
           "Version=1.5\n"
           "Name=Flubsound Pro\n"
           "Comment=Music and gaming audio enhancer, started at sign-in\n"
           "Exec=" + execValue + "\n"
           "Terminal=false\n"
           "X-GNOME-Autostart-enabled=true\n";
}

/** Keys of the [Desktop Entry] group, values as written (not unescaped).
    Comments, blank lines and other groups are skipped. */
std::map<std::string, std::string> parseDesktopEntryGroup (const std::string& text)
{
    std::map<std::string, std::string> keys;
    bool inGroup = false;
    size_t pos = 0;
    while (pos < text.size())
    {
        auto end = text.find ('\n', pos);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr (pos, end - pos);
        pos = end + 1;

        if (! line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line.front() == '#')
            continue;
        if (line.front() == '[')
        {
            inGroup = line == "[Desktop Entry]";
            continue;
        }
        if (! inGroup)
            continue;

        const auto eq = line.find ('=');
        if (eq == std::string::npos)
            continue;
        auto key = line.substr (0, eq);
        auto value = line.substr (eq + 1);
        // The spec allows spaces around '='.
        while (! key.empty() && key.back() == ' ')
            key.pop_back();
        const auto first = value.find_first_not_of (' ');
        value.erase (0, first == std::string::npos ? value.size() : first);
        keys.emplace (std::move (key), std::move (value)); // first occurrence wins
    }
    return keys;
}

bool readFile (const std::string& path, std::string& contents)
{
    FILE* file = std::fopen (path.c_str(), "re");
    if (file == nullptr)
        return false;

    contents.clear();
    char buffer[4096];
    size_t bytes = 0;
    while ((bytes = std::fread (buffer, 1, sizeof (buffer), file)) > 0 && contents.size() <= kMaxEntryBytes)
        contents.append (buffer, bytes);
    const bool ok = std::ferror (file) == 0 && contents.size() <= kMaxEntryBytes;
    std::fclose (file);
    return ok;
}

/** An entry starts the app unless it is missing, has no [Desktop Entry]
    group, or is switched off with Hidden=true (KDE, the spec's way to
    delete) or X-GNOME-Autostart-enabled=false (GNOME Tweaks). */
bool isEntryEnabled (const std::string& path)
{
    std::string text;
    if (path.empty() || ! readFile (path, text))
        return false;

    const auto keys = parseDesktopEntryGroup (text);
    if (keys.empty())
        return false;
    const auto hidden = keys.find ("Hidden");
    const auto gnome = keys.find ("X-GNOME-Autostart-enabled");
    return ! (hidden != keys.end() && hidden->second == "true") && ! (gnome != keys.end() && gnome->second == "false");
}

std::string errnoText (int error) { return std::strerror (error); } // message thread only

/** mkdir -p; new directories get 0700 as the XDG Base Directory spec asks. */
bool makeDirectories (const std::string& path, std::string& error)
{
    for (size_t slash = path.find ('/', 1);; slash = path.find ('/', slash + 1))
    {
        const std::string partial = slash == std::string::npos ? path : path.substr (0, slash);
        if (! partial.empty() && ::mkdir (partial.c_str(), 0700) != 0 && errno != EEXIST)
        {
            error = "Could not create " + partial + ": " + errnoText (errno);
            return false;
        }
        if (slash == std::string::npos)
            break;
    }

    struct stat info {};
    if (::stat (path.c_str(), &info) != 0 || ! S_ISDIR (info.st_mode))
    {
        error = "Could not create the folder " + path + ".";
        return false;
    }
    return true;
}

/** Writes a sibling temp file, fsyncs it and renames it over `path`, so a
    crash or full disk never leaves a truncated entry behind. */
bool writeFileAtomically (const std::string& path, const std::string& contents, std::string& error)
{
    const std::string temp = path + ".tmp-" + std::to_string (static_cast<long> (::getpid()));
    const int fd = ::open (temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0)
    {
        error = "Could not write " + temp + ": " + errnoText (errno);
        return false;
    }

    const auto fail = [&] (const std::string& what, int code)
    {
        ::close (fd);
        ::unlink (temp.c_str());
        error = what + ": " + errnoText (code);
        return false;
    };

    size_t written = 0;
    while (written < contents.size())
    {
        const ssize_t n = ::write (fd, contents.data() + written, contents.size() - written);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return fail ("Could not write " + temp, errno);
        }
        written += static_cast<size_t> (n);
    }

    if (::fsync (fd) != 0)
        return fail ("Could not write " + temp, errno);
    if (::close (fd) != 0)
    {
        const int code = errno;
        ::unlink (temp.c_str());
        error = "Could not write " + temp + ": " + errnoText (code);
        return false;
    }
    if (::rename (temp.c_str(), path.c_str()) != 0)
    {
        const int code = errno;
        ::unlink (temp.c_str());
        error = "Could not create " + path + ": " + errnoText (code);
        return false;
    }
    return true;
}

/** The program to start: $APPIMAGE when running from an AppImage (the
    executable itself lives in a temporary mount), else /proc/self/exe
    without the " (deleted)" suffix it gets after an in-place upgrade. */
std::string runningExecutable()
{
    if (const char* appImage = std::getenv ("APPIMAGE"); appImage != nullptr && appImage[0] == '/')
        return appImage;

    char buffer[PATH_MAX];
    const ssize_t n = ::readlink ("/proc/self/exe", buffer, sizeof (buffer) - 1);
    if (n <= 0)
        return {};
    std::string path (buffer, static_cast<size_t> (n));
    const std::string deleted = " (deleted)";
    if (path.size() > deleted.size() && path.compare (path.size() - deleted.size(), deleted.size(), deleted) == 0)
        path.resize (path.size() - deleted.size());
    return path;
}
} // namespace autostart

class LinuxAutoStart final : public AutoStart
{
public:
    bool isSupported() const override { return ! autostart::entryPath().empty(); }
    bool isEnabled() const override { return autostart::isEntryEnabled (autostart::entryPath()); }

    bool setEnabled (bool shouldStart, const std::string& executablePath, std::string& error) override
    {
        const auto path = autostart::entryPath();
        if (path.empty())
        {
            error = "No home folder is known, so no start-up entry can be written.";
            return false;
        }

        if (! shouldStart)
        {
            if (::unlink (path.c_str()) != 0 && errno != ENOENT)
            {
                error = "Could not remove " + path + ": " + autostart::errnoText (errno);
                return false;
            }
            return true;
        }

        std::string exec;
        const auto program = executablePath.empty() ? autostart::runningExecutable() : executablePath;
        return autostart::execValueFor (program, exec, error) && autostart::makeDirectories (autostart::directory(), error)
               && autostart::writeFileAtomically (path, autostart::desktopEntry (exec), error);
    }
};

//==============================================================================
/** Saved scheduling state behind the handle promoteAudioThread returns. One
    per thread in the thread's own storage (static TLS of the executable, set
    up when the thread is created), so promoting the device thread on its
    first callback allocates nothing (docs/11 E44). */
struct SavedSchedulingPolicy
{
    int policy = SCHED_OTHER;
    sched_param param {};
};

thread_local SavedSchedulingPolicy savedSchedulingPolicy;

/** Same ceiling rtkit grants by default and that PipeWire's data thread runs
    at under rtkit, so a promoted Flubsound thread never out-ranks the sound
    server it feeds. */
constexpr int kPreferredFifoPriority = 20;

//==============================================================================
// RealtimeKit (docs/11 E44)
//==============================================================================
/*  org.freedesktop.RealtimeKit1, object /org/freedesktop/RealtimeKit1, on
    the SYSTEM bus:
      MakeThreadRealtime (t thread, u priority)
      properties MaxRealtimePriority (i) and RTTimeUSecMax (x), rtkit 0.11+
    rtkit takes the process from the caller's bus credentials and checks
    that the thread belongs to it, that the priority is at most
    MaxRealtimePriority and that the process's RLIMIT_RTTIME hard limit is at
    most RTTimeUSecMax; it also rate-limits bursts of requests per user. It
    then sets SCHED_RR | SCHED_RESET_ON_FORK itself, so the call can come
    from any thread of the process: the app's message thread asks for the
    audio thread it saw in the device callback. rtkit without the properties
    (older than 0.11) runs with its defaults, used here then. One private
    connection per request (a device start), closed again at once. */
namespace rtkit
{
constexpr const char* kService = "org.freedesktop.RealtimeKit1";
constexpr const char* kObjectPath = "/org/freedesktop/RealtimeKit1";
constexpr const char* kInterface = "org.freedesktop.RealtimeKit1";
constexpr int kTimeoutMs = 1000;
constexpr int kDefaultMaxPriority = 20;        // rtkit-daemon --max-realtime-priority
constexpr int64_t kDefaultRttimeUsec = 200000; // rtkit-daemon --rttime-usec-max

/** $DBUS_SYSTEM_BUS_ADDRESS, else the standard system bus socket. */
std::string systemBusAddress()
{
    if (const char* address = std::getenv ("DBUS_SYSTEM_BUS_ADDRESS"); address != nullptr && *address != '\0')
        return address;
    return "unix:path=/var/run/dbus/system_bus_socket";
}

/** The bus answers for a name nobody owns (and that it cannot activate). */
bool isNoService (const std::string& errorName)
{
    return errorName == "org.freedesktop.DBus.Error.ServiceUnknown" || errorName == "org.freedesktop.DBus.Error.NameHasNoOwner";
}

/** Sends 'call' and waits for the reply; nullptr with 'errorName' /
    'errorText' set on an error reply or a timeout. */
dbus::MessageRef call (const dbus::Api& api, dbus::Connection* connection, dbus::Message* message, std::string& errorName, std::string& errorText)
{
    dbus::Error error;
    api.errorInit (&error);
    dbus::MessageRef reply (api.sendWithReplyAndBlock (connection, message, kTimeoutMs, &error));
    errorName = dbus::str (error.name);
    errorText = dbus::str (error.message);
    api.errorFree (&error);
    return reply;
}

/** Reads one integer property ('i' or 'x') of RealtimeKit1. false with the
    error name when the call failed, false with none when the property is
    not an integer. */
bool integerProperty (const dbus::Api& api, dbus::Connection* connection, const char* name, int64_t& value, std::string& errorName)
{
    errorName.clear();
    const dbus::MessageRef request (api.messageNewMethodCall (kService, kObjectPath, "org.freedesktop.DBus.Properties", "Get"));
    if (request == nullptr)
        return false;
    dbus::Iter args;
    api.iterInitAppend (request.get(), &args);
    if (! dbus::appendBasic (api, &args, dbus::kTypeString, kInterface) || ! dbus::appendBasic (api, &args, dbus::kTypeString, name))
        return false;
    std::string errorText;
    const auto reply = call (api, connection, request.get(), errorName, errorText);
    dbus::Iter result, variant;
    if (reply == nullptr || api.iterInit (reply.get(), &result) == 0 || api.iterGetArgType (&result) != dbus::kTypeVariant)
        return false;
    api.iterRecurse (&result, &variant);
    if (const int type = api.iterGetArgType (&variant); type == 'i')
    {
        int32_t v = 0;
        api.iterGetBasic (&variant, &v);
        value = v;
        return true;
    }
    else if (type == 'x')
    {
        int64_t v = 0;
        api.iterGetBasic (&variant, &v);
        value = v;
        return true;
    }
    return false;
}

/** Lowers this process's RLIMIT_RTTIME to at most 'maxUsec' (soft and hard),
    which rtkit requires; a limit already within it is kept. 'inForce' is
    the hard limit afterwards. */
bool limitRttime (int64_t maxUsec, int64_t& inForce, std::string& error)
{
    rlimit limit {};
    if (::getrlimit (RLIMIT_RTTIME, &limit) != 0)
    {
        error = "RLIMIT_RTTIME could not be read";
        return false;
    }
    const auto ceiling = static_cast<rlim_t> (std::max<int64_t> (maxUsec, 1));
    if (limit.rlim_max != RLIM_INFINITY && limit.rlim_max <= ceiling)
    {
        inForce = static_cast<int64_t> (limit.rlim_max);
        return true;
    }
    limit.rlim_max = ceiling;
    limit.rlim_cur = std::min (limit.rlim_cur, ceiling); // RLIM_INFINITY is the largest value
    if (::setrlimit (RLIMIT_RTTIME, &limit) != 0)
    {
        error = "RLIMIT_RTTIME could not be lowered to " + std::to_string (maxUsec) + " us, which RealtimeKit requires";
        return false;
    }
    inForce = static_cast<int64_t> (ceiling);
    return true;
}

const char* policyName (int policy)
{
    switch (policy & ~SCHED_RESET_ON_FORK)
    {
        case SCHED_FIFO:  return "FIFO";
        case SCHED_RR:    return "RR";
        case SCHED_OTHER: return "OTHER";
        case SCHED_BATCH: return "BATCH";
        case SCHED_IDLE:  return "IDLE";
        default:          return "other";
    }
}
} // namespace rtkit

//==============================================================================
// ALSA channel maps (docs/11 E27 step 4)
//==============================================================================
/*  A JUCE ALSA device is known by its name only; the PCM behind it is found
    the way JUCE lists it: "ALSA" devices by snd_device_name_hint (name = the
    hint's DESC with line breaks as "; ", else its NAME), "ALSA HW" devices
    by card and PCM ("<card name>, <pcm name>", plus " {<subdevice name>}"
    when the PCM has several subdevices). Only a card PCM (hw:) has a map to
    read without opening it: snd_pcm_query_chmaps_from_hw asks the driver
    through the card's control device. Plug-in PCMs (default, pipewire,
    pulse, surround71, ...) report nothing here; they deliver ALSA's default
    order, which the engine host assumes for them.

    libasound.so.2 is loaded at run time and declared here from its stable
    C ABI (the JUCE ALSA backend links it anyway; the declarations avoid a
    header dependency): snd_pcm_chmap_query_t is { int type;
    snd_pcm_chmap_t map; } with snd_pcm_chmap_t { unsigned int channels;
    unsigned int pos[]; }, and the positions are enum snd_pcm_chmap_position
    (UNKNOWN 0, NA 1, MONO 2, FL 3, FR 4, RL 5, RR 6, FC 7, LFE 8, SL 9,
    SR 10, ...). */
namespace alsa
{
struct Ctl;
struct CardInfo;
struct PcmInfo;
struct ChmapQuery
{
    int type = 0;
    unsigned int channels = 0; // followed by 'channels' positions
};

constexpr int kStreamCapture = 1; // SND_PCM_STREAM_CAPTURE
constexpr int kCtlNonblock = 1;   // SND_CTL_NONBLOCK

inline const unsigned int* positionsOf (const ChmapQuery& query) { return &query.channels + 1; }

struct Api
{
    void* lib = nullptr;
    int (*deviceNameHint) (int, const char*, void***) = nullptr;
    char* (*deviceNameGetHint) (const void*, const char*) = nullptr;
    int (*deviceNameFreeHint) (void**) = nullptr;
    int (*cardGetIndex) (const char*) = nullptr;
    int (*cardNext) (int*) = nullptr;
    int (*ctlOpen) (Ctl**, const char*, int) = nullptr;
    int (*ctlClose) (Ctl*) = nullptr;
    int (*cardInfoMalloc) (CardInfo**) = nullptr;
    void (*cardInfoFree) (CardInfo*) = nullptr;
    int (*ctlCardInfo) (Ctl*, CardInfo*) = nullptr;
    const char* (*cardInfoGetId) (const CardInfo*) = nullptr;
    const char* (*cardInfoGetName) (const CardInfo*) = nullptr;
    int (*ctlPcmNextDevice) (Ctl*, int*) = nullptr;
    int (*pcmInfoMalloc) (PcmInfo**) = nullptr;
    void (*pcmInfoFree) (PcmInfo*) = nullptr;
    void (*pcmInfoSetDevice) (PcmInfo*, unsigned int) = nullptr;
    void (*pcmInfoSetSubdevice) (PcmInfo*, unsigned int) = nullptr;
    void (*pcmInfoSetStream) (PcmInfo*, int) = nullptr;
    int (*ctlPcmInfo) (Ctl*, PcmInfo*) = nullptr;
    const char* (*pcmInfoGetName) (const PcmInfo*) = nullptr;
    unsigned int (*pcmInfoGetSubdevicesCount) (const PcmInfo*) = nullptr;
    const char* (*pcmInfoGetSubdeviceName) (const PcmInfo*) = nullptr;
    ChmapQuery** (*queryChmapsFromHw) (int, int, int, int) = nullptr;
    void (*freeChmaps) (ChmapQuery**) = nullptr;

    static const Api* get()
    {
        static const Api api = []
        {
            Api a;
            a.lib = ::dlopen ("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
            if (a.lib == nullptr)
                return a;
            const auto sym = [&a] (auto& fn, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype (fn)>> (::dlsym (a.lib, name)); return fn != nullptr; };
            const bool ok = sym (a.deviceNameHint, "snd_device_name_hint") && sym (a.deviceNameGetHint, "snd_device_name_get_hint")
                            && sym (a.deviceNameFreeHint, "snd_device_name_free_hint") && sym (a.cardGetIndex, "snd_card_get_index")
                            && sym (a.cardNext, "snd_card_next") && sym (a.ctlOpen, "snd_ctl_open") && sym (a.ctlClose, "snd_ctl_close")
                            && sym (a.cardInfoMalloc, "snd_ctl_card_info_malloc") && sym (a.cardInfoFree, "snd_ctl_card_info_free")
                            && sym (a.ctlCardInfo, "snd_ctl_card_info") && sym (a.cardInfoGetId, "snd_ctl_card_info_get_id")
                            && sym (a.cardInfoGetName, "snd_ctl_card_info_get_name") && sym (a.ctlPcmNextDevice, "snd_ctl_pcm_next_device")
                            && sym (a.pcmInfoMalloc, "snd_pcm_info_malloc") && sym (a.pcmInfoFree, "snd_pcm_info_free")
                            && sym (a.pcmInfoSetDevice, "snd_pcm_info_set_device") && sym (a.pcmInfoSetSubdevice, "snd_pcm_info_set_subdevice")
                            && sym (a.pcmInfoSetStream, "snd_pcm_info_set_stream") && sym (a.ctlPcmInfo, "snd_ctl_pcm_info")
                            && sym (a.pcmInfoGetName, "snd_pcm_info_get_name")
                            && sym (a.pcmInfoGetSubdevicesCount, "snd_pcm_info_get_subdevices_count")
                            && sym (a.pcmInfoGetSubdeviceName, "snd_pcm_info_get_subdevice_name")
                            && sym (a.queryChmapsFromHw, "snd_pcm_query_chmaps_from_hw") && sym (a.freeChmaps, "snd_pcm_free_chmaps");
            if (! ok)
            {
                ::dlclose (a.lib);
                a = Api();
            }
            return a;
        }();
        return api.lib != nullptr ? &api : nullptr;
    }
};

/** A card PCM: card index, device, subdevice (-1 = any). */
struct HwAddress
{
    int card = -1;
    int device = -1;
    int subdevice = -1;
    bool operator== (const HwAddress&) const = default;
};

/** The card and device of a hw PCM name, "hw:CARD=PCH,DEV=3" (as hints
    give it) or "hw:0,3[,1]" (as the ALSA HW list does); the card as written
    (an index or a card id) for snd_card_get_index. false for any other PCM. */
bool parseHwName (const std::string& name, std::string& card, int& device, int& subdevice)
{
    if (name.rfind ("hw:", 0) != 0)
        return false;
    card.clear();
    device = 0; // ALSA's default when DEV is left out
    subdevice = -1;
    std::vector<std::string> fields;
    std::string field;
    for (const char c : name.substr (3) + ",")
    {
        if (c != ',')
            field += c;
        else
        {
            fields.push_back (field);
            field.clear();
        }
    }
    const auto toInt = [] (const std::string& text, int& out)
    {
        if (text.empty() || text.size() > 4 || ! std::all_of (text.begin(), text.end(), [] (char ch) { return ch >= '0' && ch <= '9'; }))
            return false;
        out = std::stoi (text);
        return true;
    };
    for (size_t i = 0; i < fields.size(); ++i)
    {
        const auto& f = fields[i];
        const auto eq = f.find ('=');
        const std::string key = eq == std::string::npos ? std::string() : f.substr (0, eq);
        const std::string value = eq == std::string::npos ? f : f.substr (eq + 1);
        if (key == "CARD" || (key.empty() && i == 0))
            card = value;
        else if ((key == "DEV" || (key.empty() && i == 1)) && ! toInt (value, device))
            return false;
        else if ((key == "SUBDEV" || (key.empty() && i == 2)) && ! toInt (value, subdevice))
            return false;
    }
    return ! card.empty();
}

/** The name JUCE's "ALSA" list shows for a hint: DESC with its line breaks as
    "; ", else NAME. */
std::string hintDeviceName (const std::string& hintName, const std::string& description)
{
    if (description.empty())
        return hintName;
    std::string name;
    for (const char c : description)
        name += c == '\n' ? std::string ("; ") : std::string (1, c);
    return name;
}

SpeakerPosition speakerPosition (unsigned int chmapPosition)
{
    switch (chmapPosition)
    {
        case 3:  return SpeakerPosition::FL;
        case 4:  return SpeakerPosition::FR;
        case 5:  return SpeakerPosition::RL;
        case 6:  return SpeakerPosition::RR;
        case 7:  return SpeakerPosition::FC;
        case 8:  return SpeakerPosition::LFE;
        case 9:  return SpeakerPosition::SL;
        case 10: return SpeakerPosition::SR;
        default: return SpeakerPosition::Unknown;
    }
}

/** The positions of the first map for 'channels' channels in a
    snd_pcm_query_chmaps* list (nullptr-terminated); empty when there is
    none or it names no position the engine knows. */
std::vector<SpeakerPosition> positionsFromChmaps (const ChmapQuery* const* maps, int channels)
{
    if (maps == nullptr || channels <= 0)
        return {};
    for (; *maps != nullptr; ++maps)
    {
        const ChmapQuery& query = **maps;
        if (static_cast<int> (query.channels) != channels)
            continue;
        std::vector<SpeakerPosition> positions (static_cast<size_t> (channels));
        bool any = false;
        for (int c = 0; c < channels; ++c)
        {
            positions[static_cast<size_t> (c)] = speakerPosition (positionsOf (query)[c]);
            any = any || positions[static_cast<size_t> (c)] != SpeakerPosition::Unknown;
        }
        return any ? positions : std::vector<SpeakerPosition>();
    }
    return {};
}

/** The card PCM behind a JUCE "ALSA" device name (from the PCM hints). */
bool findHintAddress (const Api& api, const std::string& deviceName, HwAddress& address)
{
    void** hints = nullptr;
    if (api.deviceNameHint (-1, "pcm", &hints) != 0 || hints == nullptr)
        return false;
    const auto hint = [&api] (const void* h, const char* id)
    {
        char* text = api.deviceNameGetHint (h, id);
        std::string value = text != nullptr ? text : "";
        ::free (text);
        return value;
    };
    int matches = 0;
    for (void** h = hints; *h != nullptr; ++h)
    {
        const std::string name = hint (*h, "NAME");
        if (name.empty() || hintDeviceName (name, hint (*h, "DESC")) != deviceName)
            continue;
        ++matches;
        std::string card;
        HwAddress found;
        if (parseHwName (name, card, found.device, found.subdevice) && (found.card = api.cardGetIndex (card.c_str())) >= 0)
            address = found;
        else
            address = {};
    }
    api.deviceNameFreeHint (hints);
    return matches == 1 && address.card >= 0; // a duplicated name is ambiguous
}

/** The card PCM behind a JUCE "ALSA HW" device name (from the cards). */
bool findCardAddress (const Api& api, const std::string& deviceName, HwAddress& address)
{
    CardInfo* cardInfo = nullptr;
    PcmInfo* pcmInfo = nullptr;
    if (api.cardInfoMalloc (&cardInfo) != 0 || api.pcmInfoMalloc (&pcmInfo) != 0)
    {
        if (cardInfo != nullptr)
            api.cardInfoFree (cardInfo);
        return false;
    }
    int matches = 0;
    for (int card = -1; api.cardNext (&card) == 0 && card >= 0;)
    {
        Ctl* ctl = nullptr;
        if (api.ctlOpen (&ctl, ("hw:" + std::to_string (card)).c_str(), kCtlNonblock) < 0)
            continue;
        if (api.ctlCardInfo (ctl, cardInfo) >= 0)
        {
            std::string cardName = dbus::str (api.cardInfoGetName (cardInfo));
            if (cardName.empty())
                cardName = dbus::str (api.cardInfoGetId (cardInfo));
            for (int device = -1; api.ctlPcmNextDevice (ctl, &device) >= 0 && device >= 0;)
            {
                api.pcmInfoSetDevice (pcmInfo, static_cast<unsigned int> (device));
                for (unsigned int sub = 0, subs = 1; sub < subs; ++sub)
                {
                    api.pcmInfoSetSubdevice (pcmInfo, sub);
                    api.pcmInfoSetStream (pcmInfo, kStreamCapture);
                    if (api.ctlPcmInfo (ctl, pcmInfo) < 0)
                        break;
                    subs = std::max (1u, api.pcmInfoGetSubdevicesCount (pcmInfo));
                    std::string name = cardName + ", " + dbus::str (api.pcmInfoGetName (pcmInfo));
                    if (subs > 1)
                        name += " {" + dbus::str (api.pcmInfoGetSubdeviceName (pcmInfo)) + "}";
                    if (name == deviceName)
                    {
                        ++matches;
                        address = { card, device, subs > 1 ? static_cast<int> (sub) : -1 };
                    }
                }
            }
        }
        api.ctlClose (ctl);
    }
    api.pcmInfoFree (pcmInfo);
    api.cardInfoFree (cardInfo);
    return matches == 1;
}
} // namespace alsa
} // namespace

// ============================================================================
// AudioEndpoints
// ============================================================================
EndpointTransport AudioEndpoints::queryOutputTransport (const std::string&)
{
    // JUCE's ALSA/JACK device names do not map 1:1 to PipeWire/Pulse sinks, so
    // the transport (PipeWire "device.bus") is not queried here; headset
    // profiles fall back to flub::device::detectConnection() heuristics.
    return EndpointTransport::Unknown;
}

EndpointVolume AudioEndpoints::queryOutputVolume (const std::string& deviceName)
{
    // docs/11 E32. JUCE's ALSA "pipewire" / "pulse" / "default" devices play
    // to the default sink; a PipeWire / Pulse sink name (always dotted:
    // alsa_output.<card>.<profile>, bluez_output.<address>.<n>) is used as is.
    EndpointVolume result;
    const bool isSinkName = deviceName.find ('.') != std::string::npos && pactl::isSafeToken (deviceName);
    const std::string sink = pactl::shellQuote (isSinkName ? deviceName : std::string ("@DEFAULT_SINK@"));

    // "Volume: front-left: 32768 /  50% / -18.06 dB,   front-right: ... dB\n
    //  balance 0.00": every "<number> dB" (or "-inf dB") is one channel.
    const auto volume = pactl::run ("LC_ALL=C pactl get-sink-volume " + sink + " 2>&1");
    if (volume.exitCode != 0)
    {
        result.error = volume.exitCode < 0 ? "pactl could not be run" : "pactl get-sink-volume failed: " + pactl::firstLine (volume.output);
        return result;
    }
    double sum = 0.0;
    int channels = 0;
    for (size_t at = volume.output.find (" dB"); at != std::string::npos; at = volume.output.find (" dB", at + 3))
    {
        size_t begin = at;
        while (begin > 0 && volume.output[begin - 1] != '/' && volume.output[begin - 1] != ' ')
            --begin;
        const std::string text = volume.output.substr (begin, at - begin);
        if (text == "-inf")
        {
            sum += EndpointVolume::kSilentDb;
            ++channels;
            continue;
        }
        char* end = nullptr;
        const double db = std::strtod (text.c_str(), &end);
        if (text.empty() || end != text.c_str() + text.size() || ! std::isfinite (db))
            continue;
        sum += std::max (db, static_cast<double> (EndpointVolume::kSilentDb));
        ++channels;
    }
    if (channels == 0)
    {
        result.error = "Unexpected pactl output (get-sink-volume): " + pactl::firstLine (volume.output);
        return result;
    }
    result.known = true;
    result.volumeDb = std::max (static_cast<float> (sum / channels), EndpointVolume::kSilentDb); // > 0 dB when over-amplified
    const auto mute = pactl::run ("LC_ALL=C pactl get-sink-mute " + sink + " 2>&1");
    result.muted = mute.exitCode == 0 && mute.output.find ("Mute: yes") != std::string::npos;
    return result;
}

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
        get RealtimeKit instead: the engine host asks it from the message
        thread for the thread id it saw here (RealtimeScheduling, docs/11
        E44), because a D-Bus round trip cannot run on the audio thread.
        When Flubsound runs as a JACK / PipeWire client the server already
        calls our process callback on its own real-time thread.

        SCHED_RESET_ON_FORK keeps children (e.g. pactl started via popen) from
        inheriting real-time priority; rtkit requires it as well. */
    const pthread_t self = ::pthread_self();

    int currentPolicy = SCHED_OTHER;
    sched_param currentParam {};
    if (::pthread_getschedparam (self, &currentPolicy, &currentParam) != 0)
        return nullptr;

    // Already real-time (JACK / PipeWire-JACK call us on the server's client
    // thread, typically SCHED_FIFO 70+): leave it alone. Setting our own
    // priority here would DEMOTE the sound server's thread.
    const int basePolicy = currentPolicy & ~SCHED_RESET_ON_FORK;
    if (basePolicy == SCHED_FIFO || basePolicy == SCHED_RR)
        return nullptr;

    SavedSchedulingPolicy* const saved = &savedSchedulingPolicy;
    saved->policy = currentPolicy;
    saved->param = currentParam;

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

    return promoted ? saved : nullptr;
}

void SystemTuning::revertAudioThread (void* handle)
{
    if (handle == nullptr)
        return;

    const auto* saved = static_cast<const SavedSchedulingPolicy*> (handle);
    const pthread_t self = ::pthread_self();

    // Exact restore first. Without CAP_SYS_NICE the kernel refuses to CLEAR
    // SCHED_RESET_ON_FORK once set (sched(7)), i.e. a desktop user promoted via
    // RLIMIT_RTPRIO gets EPERM here and would stay SCHED_FIFO forever. Keeping
    // the flag is harmless for a normal-policy thread, so retry with it.
    if (::pthread_setschedparam (self, saved->policy, &saved->param) != 0)
        ::pthread_setschedparam (self, saved->policy | SCHED_RESET_ON_FORK, &saved->param);
}

//==============================================================================
// RealtimeScheduling (docs/11 E44)
//==============================================================================
uint64_t RealtimeScheduling::currentThreadId() noexcept FLUB_NONBLOCKING
{
    // glibc's gettid() wrapper (2.30+): RealtimeSanitizer flags the generic
    // syscall() entry point, not this one.
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 30))
    return static_cast<uint64_t> (::gettid());
#else
    return static_cast<uint64_t> (::syscall (SYS_gettid));
#endif
}

ThreadScheduling RealtimeScheduling::queryThread (uint64_t threadId)
{
    ThreadScheduling result;
    if (threadId == 0 || threadId > static_cast<uint64_t> (std::numeric_limits<pid_t>::max()))
        return result;
    const auto tid = static_cast<pid_t> (threadId);
    const int policy = ::sched_getscheduler (tid);
    sched_param param {};
    if (policy < 0 || ::sched_getparam (tid, &param) != 0)
        return result;
    const int base = policy & ~SCHED_RESET_ON_FORK;
    result.known = true;
    result.realtime = base == SCHED_FIFO || base == SCHED_RR;
    result.priority = result.realtime ? param.sched_priority : 0;
    result.policy = rtkit::policyName (policy);
    return result;
}

RealtimeKitResult RealtimeScheduling::requestRealtimeKit (uint64_t threadId, int wantedPriority)
{
    using Outcome = RealtimeKitResult::Outcome;
    RealtimeKitResult result;
    const auto* api = dbus::Api::get();
    if (api == nullptr)
    {
        result.message = "libdbus-1 is not installed, so RealtimeKit cannot be asked";
        return result;
    }
    if (threadId == 0)
    {
        result.message = "no audio thread to promote";
        return result;
    }

    const std::string address = rtkit::systemBusAddress();
    dbus::Error error;
    api->errorInit (&error);
    dbus::Connection* connection = api->connectionOpenPrivate (address.c_str(), &error);
    if (connection != nullptr)
    {
        api->connectionSetExitOnDisconnect (connection, 0);
        if (api->busRegister (connection, &error) == 0)
        {
            api->connectionClose (connection);
            api->connectionUnref (connection);
            connection = nullptr;
        }
    }
    const std::string connectError = dbus::str (error.message);
    api->errorFree (&error);
    if (connection == nullptr)
    {
        result.message = "the system D-Bus is not reachable, so RealtimeKit cannot be asked" + (connectError.empty() ? std::string() : " (" + connectError + ")");
        return result;
    }
    struct Closer
    {
        const dbus::Api& library;
        dbus::Connection* open;
        ~Closer()
        {
            library.connectionClose (open);
            library.connectionUnref (open);
        }
    } closer { *api, connection };

    // The limits rtkit enforces; its defaults when it has no properties.
    int64_t maxPriority = rtkit::kDefaultMaxPriority, rttimeMax = rtkit::kDefaultRttimeUsec;
    std::string errorName;
    if (! rtkit::integerProperty (*api, connection, "MaxRealtimePriority", maxPriority, errorName) && rtkit::isNoService (errorName))
    {
        result.message = "RealtimeKit is not running (install the rtkit package)";
        return result;
    }
    rtkit::integerProperty (*api, connection, "RTTimeUSecMax", rttimeMax, errorName);

    result.priority = static_cast<int> (std::clamp<int64_t> (std::min<int64_t> (wantedPriority, maxPriority), 1, 99));
    std::string limitError;
    if (! rtkit::limitRttime (rttimeMax, result.rttimeUsec, limitError))
    {
        result.outcome = Outcome::Refused;
        result.message = limitError;
        return result;
    }

    const dbus::MessageRef request (api->messageNewMethodCall (rtkit::kService, rtkit::kObjectPath, rtkit::kInterface, "MakeThreadRealtime"));
    if (request == nullptr)
    {
        result.message = "out of memory";
        return result;
    }
    dbus::Iter args;
    api->iterInitAppend (request.get(), &args);
    const auto priority = static_cast<uint32_t> (result.priority);
    if (api->iterAppendBasic (&args, 't', &threadId) == 0 || api->iterAppendBasic (&args, dbus::kTypeUInt32, &priority) == 0)
    {
        result.message = "out of memory";
        return result;
    }
    std::string errorText;
    if (rtkit::call (*api, connection, request.get(), errorName, errorText) != nullptr)
    {
        result.outcome = Outcome::Granted;
        return result;
    }
    if (rtkit::isNoService (errorName))
    {
        result.message = "RealtimeKit is not running (install the rtkit package)";
        return result;
    }
    result.outcome = Outcome::Refused;
    result.message = "RealtimeKit refused real-time priority " + std::to_string (result.priority) + " ("
                     + (errorText.empty() ? errorName : errorText) + ")";
    return result;
}

//==============================================================================
// AudioChannelMaps (docs/11 E27 step 4)
//==============================================================================
std::vector<SpeakerPosition> AudioChannelMaps::queryInputPositions (const std::string& deviceTypeName, const std::string& deviceName, int channels)
{
    const bool alsaPcm = deviceTypeName == "ALSA", alsaHw = deviceTypeName == "ALSA HW";
    const auto* api = alsa::Api::get();
    if ((! alsaPcm && ! alsaHw) || api == nullptr || deviceName.empty() || channels <= 0)
        return {};
    alsa::HwAddress address;
    if (! (alsaHw ? alsa::findCardAddress (*api, deviceName, address) : alsa::findHintAddress (*api, deviceName, address)))
        return {};
    alsa::ChmapQuery** maps = api->queryChmapsFromHw (address.card, address.device, address.subdevice, alsa::kStreamCapture);
    auto positions = alsa::positionsFromChmaps (maps, channels);
    if (maps != nullptr)
        api->freeChmaps (maps);
    return positions;
}

//==============================================================================
std::unique_ptr<GlobalHotkeys> GlobalHotkeys::create()
{
    // X key grabs are not global under Wayland (XWayland only sees keys while
    // one of its windows has focus): there the portal is the only route, and
    // without it the service is unsupported.
    if (isWaylandSession())
        return std::make_unique<PortalGlobalHotkeys>();
    return std::make_unique<LinuxGlobalHotkeys>();
}
std::unique_ptr<AppAudioRouter> AppAudioRouter::create()
{
    // The app creates its router on the message thread before the audio
    // device opens (AppRouting, in EngineController's constructor): the
    // moment to ask PipeWire for a smaller quantum than the client library's
    // 1024/48000 (docs/11 E48a). Balanced's request never locks the quantum.
    pipewire::applyLatencyEnvironment (pipewire::QuantumRequest::Balanced);
    return std::make_unique<LinuxAppAudioRouter>();
}
std::unique_ptr<ProcessLoopbackCapture> ProcessLoopbackCapture::create() { return std::make_unique<LinuxProcessLoopbackCapture>(); }
std::unique_ptr<AutoStart> AutoStart::create() { return std::make_unique<LinuxAutoStart>(); }
std::unique_ptr<ForegroundApp> ForegroundApp::create() { return std::make_unique<LinuxForegroundApp>(); }
} // namespace flub::platform

#endif // __linux__
