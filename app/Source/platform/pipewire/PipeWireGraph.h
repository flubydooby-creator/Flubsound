// Flubsound Pro - PipeWire graph model and link planning (docs/11 E48).
//
// Plain C++ (no libpipewire, no JUCE): the model of PipeWire's nodes, ports
// and links, the quantum request, and the plans that say which links
// Flubsound wants. Two readers fill the model: `pw-dump` JSON
// (pipewire::parseDump in PlatformServices_linux.cpp, the fallback without
// libpipewire) and the libpipewire registry (pipewire::Session in
// PipeWireNative.cpp, built when pkg-config finds libpipewire-0.3). Tests
// run the plans against fixtures on every Linux build.
#pragma once

#include "../PlatformServices.h"

#include "flub/io/Json.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace flub::platform::pipewire
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
    is never replaced. The native node sets the same values as its own
    node.latency / node.lock-quantum properties (nodeLatency below). */
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
inline bool isValidLatency (const std::string& text)
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
inline LatencyEnvironment planLatencyEnvironment (QuantumRequest request, const char* currentLatency, const char* currentProps)
{
    LatencyEnvironment plan;
    if (currentLatency != nullptr && *currentLatency != '\0')
        return plan;

    plan.latency = request == QuantumRequest::LowLatency ? kLowLatency : kBalancedLatency;
    if (request == QuantumRequest::LowLatency && (currentProps == nullptr || *currentProps == '\0'))
        plan.props = kLockQuantumProps;
    return plan;
}

/** The native node's own node.latency and node.lock-quantum for a request
    (the same values as the environment plan; a node can change them while it
    runs, pw_filter_update_properties, so no re-open is needed). */
struct NodeLatency
{
    std::string latency;      // node.latency
    bool lockQuantum = false; // node.lock-quantum
};

inline NodeLatency nodeLatency (QuantumRequest request)
{
    return request == QuantumRequest::LowLatency ? NodeLatency { kLowLatency, true } : NodeLatency { kBalancedLatency, false };
}

inline QuantumRequest quantumRequestFor (NativeAudioNodeConfig::Latency latency)
{
    return latency == NativeAudioNodeConfig::Latency::LowLatency ? QuantumRequest::LowLatency : QuantumRequest::Balanced;
}

/*  The graph as Flubsound sees it: nodes, their ports, and the links between
    ports. Filled from pw-dump JSON or from registry globals; the plans below
    only read it. */
struct Node
{
    uint32_t id = 0;
    std::string name;        // node.name
    std::string description; // node.description
    std::string mediaClass;  // media.class, e.g. "Audio/Sink"
    uint32_t processId = 0;  // application.process.id, else its client's pid; 0 = unknown
    uint32_t clientId = 0;   // client.id; 0 = none
    int64_t priority = 0;    // priority.session (the session manager's choice of default)
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
    std::string channel; // audio.channel, e.g. "FL", "MONO", "AUX0"; empty = not given
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

/** An id PipeWire prints as a decimal string ("42"); false for anything
    else (empty, signs, spaces, more than 32 bits). */
inline bool parseId (const char* text, uint32_t& out)
{
    if (text == nullptr || *text == '\0')
        return false;
    uint64_t value = 0;
    for (const char* c = text; *c != '\0'; ++c)
    {
        if (*c < '0' || *c > '9' || c - text >= 10)
            return false;
        value = value * 10u + static_cast<uint64_t> (*c - '0');
    }
    if (value > 0xffffffffull)
        return false;
    out = static_cast<uint32_t> (value);
    return true;
}

/*  The registry mirror: pipewire::Session (PipeWireNative.cpp) feeds every
    registry global / global_remove event through these, so the graph is
    current without polling. 'props' looks a global's property up (nullptr =
    not set). Clients are kept only for their pid (a node without
    application.process.id takes its client's pipewire.sec.pid, as
    parseDump does). */
struct RegistryState
{
    Graph graph;
    std::map<uint32_t, uint32_t> clientPids; // client id -> pid
};

using PropertyLookup = std::function<const char* (const char* key)>;

/** Applies one registry global. true when the graph changed. */
inline bool addGlobal (RegistryState& state, uint32_t id, const std::string& type, const PropertyLookup& props)
{
    const auto text = [&] (const char* key) { const char* v = props (key); return v != nullptr ? std::string (v) : std::string(); };
    auto& graph = state.graph;

    if (type == "PipeWire:Interface:Client")
    {
        uint32_t pid = 0;
        if (! ((parseId (props ("pipewire.sec.pid"), pid) && pid != 0) || parseId (props ("application.process.id"), pid)))
            return false;
        state.clientPids[id] = pid;
        bool changed = false;
        for (auto& [nodeId, node] : graph.nodes)
            if (node.clientId == id && node.processId == 0)
            {
                node.processId = pid;
                changed = true;
            }
        return changed;
    }
    if (type == "PipeWire:Interface:Node")
    {
        Node node;
        node.id = id;
        node.name = text ("node.name");
        node.description = text ("node.description");
        node.mediaClass = text ("media.class");
        parseId (props ("application.process.id"), node.processId);
        parseId (props ("client.id"), node.clientId);
        if (const char* priority = props ("priority.session"); priority != nullptr)
            node.priority = std::strtoll (priority, nullptr, 10);
        if (node.processId == 0 && node.clientId != 0)
            if (const auto it = state.clientPids.find (node.clientId); it != state.clientPids.end())
                node.processId = it->second;
        graph.nodes[id] = std::move (node);
        return true;
    }
    if (type == "PipeWire:Interface:Port")
    {
        Port port;
        port.id = id;
        const auto direction = text ("port.direction");
        if (! parseId (props ("node.id"), port.nodeId) || (direction != "in" && direction != "out" && direction != "input" && direction != "output"))
            return false;
        port.isInput = direction == "in" || direction == "input";
        port.name = text ("port.name");
        port.channel = text ("audio.channel");
        port.isMonitor = text ("port.monitor") == "true" || (! port.isInput && port.name.rfind ("monitor_", 0) == 0);
        const auto dsp = props ("format.dsp");
        port.isAudio = dsp == nullptr || std::string (dsp).find ("audio") != std::string::npos;
        uint32_t index = 0;
        if (parseId (props ("port.id"), index))
            port.index = index;
        graph.ports.erase (std::remove_if (graph.ports.begin(), graph.ports.end(), [id] (const Port& p) { return p.id == id; }), graph.ports.end());
        graph.ports.push_back (std::move (port));
        return true;
    }
    if (type == "PipeWire:Interface:Link")
    {
        Link link;
        link.id = id;
        if (! parseId (props ("link.output.port"), link.outputPort) || ! parseId (props ("link.input.port"), link.inputPort))
            return false;
        graph.links.erase (std::remove_if (graph.links.begin(), graph.links.end(), [id] (const Link& l) { return l.id == id; }), graph.links.end());
        graph.links.push_back (link);
        return true;
    }
    return false;
}

/** Applies one registry global_remove. true when the graph changed. */
inline bool removeGlobal (RegistryState& state, uint32_t id)
{
    auto& graph = state.graph;
    state.clientPids.erase (id);
    const auto ports = graph.ports.size(), links = graph.links.size();
    const bool node = graph.nodes.erase (id) != 0;
    graph.ports.erase (std::remove_if (graph.ports.begin(), graph.ports.end(), [id] (const Port& p) { return p.id == id; }), graph.ports.end());
    graph.links.erase (std::remove_if (graph.links.begin(), graph.links.end(), [id] (const Link& l) { return l.id == id; }), graph.links.end());
    return node || ports != graph.ports.size() || links != graph.links.size();
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
    uint32_t engineNode = 0;           // 0 = Flubsound has no PipeWire input node
    int engineInputs = 0;              // its audio input ports
    std::vector<PortLink> wanted;      // every link the map asks for (existing or not)
    std::vector<SinkLinks> sinks;      // per mapped sink that could be linked
    std::vector<std::string> problems; // user-presentable, one per issue
};

/** The audio ports of one node and direction, in channel order (port.id,
    then object id). */
inline std::vector<const Port*> audioPorts (const Graph& graph, uint32_t nodeId, bool inputs, bool monitorsOnly)
{
    std::vector<const Port*> result;
    for (const auto& port : graph.ports)
        if (port.nodeId == nodeId && port.isInput == inputs && port.isAudio && (! monitorsOnly || port.isMonitor))
            result.push_back (&port);
    std::stable_sort (result.begin(), result.end(), [] (const Port* a, const Port* b)
                      { return a->index != b->index ? (a->index >= 0 && (b->index < 0 || a->index < b->index)) : a->id < b->id; });
    return result;
}

/** The sink (media.class Audio/Sink...) called 'name'; nullptr = none. */
inline const Node* findSink (const Graph& graph, const std::string& name)
{
    const Node* sink = nullptr;
    for (const auto& [id, node] : graph.nodes)
        if (node.name == name && node.mediaClass.rfind ("Audio/Sink", 0) == 0)
            sink = &node;
    return sink;
}

/*  Monitor links for the JUCE device path. Applications play into the
    flubsound_<strip> null sinks; the engine reads a strip from its device
    input starting at the strip's channel (Settings > Routing > Input feeds
    strip, or its input map, the deviceInputMap setting, e.g. "Game=0;Music=8"). PipeWire
    does not connect a sink's monitor to that input by itself, so the router
    does: monitor port k of each sink -> input port first + k of Flubsound's
    own node (the node whose application.process.id, or whose client's
    pipewire.sec.pid, is this process). */

/** Plans monitor port k of each mapped endpoint's sink -> input port
    first + k of this process's node with the most audio inputs. A sink gets
    as many links as it has monitor ports, but never reaches the next mapped
    strip's first channel or past the engine's last input. */
inline LinkPlan planMonitorLinks (const Graph& graph, uint32_t ownPid, const std::vector<AppAudioRouter::EndpointInput>& inputs)
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

        const Node* sink = findSink (graph, input.endpointId);
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

//==============================================================================
// The native node (docs/11 E48): one pw_filter, one input port group per
// strip, an output port group linked to the output sink.
//==============================================================================

/** Game 7.1, Music, Chat and System, as platform/linux's setup script and
    90-flubsound-sinks.conf create them (same names, descriptions and
    channel positions), in the engine's input channel order: Game 0-7,
    Music 8-9, Chat 10-11, System 12-13 (the Linux default input map). */
inline std::vector<NativeAudioNodeConfig::Strip> defaultStrips()
{
    const std::vector<std::string> stereo { "FL", "FR" };
    return { { "Game", "flubsound_game", "Flubsound Game", { "FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR" } },
             { "Music", "flubsound_music", "Flubsound Music", stereo },
             { "Chat", "flubsound_chat", "Flubsound Chat", stereo },
             { "System", "flubsound_system", "Flubsound System", stereo } };
}

/** Speaker positions of an output port group: 1 = MONO, 2 = FL FR,
    6 = 5.1 (FL FR FC LFE RL RR), 8 = 7.1; other counts AUX0..AUXn-1. */
inline std::vector<std::string> outputPositions (int channels)
{
    switch (channels)
    {
        case 1: return { "MONO" };
        case 2: return { "FL", "FR" };
        case 6: return { "FL", "FR", "FC", "LFE", "RL", "RR" };
        case 8: return { "FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR" };
        default: break;
    }
    std::vector<std::string> aux;
    for (int i = 0; i < channels; ++i)
        aux.push_back ("AUX" + std::to_string (i));
    return aux;
}

/** "FL,FR,FC" (audio.position of a null sink created by the app). */
inline std::string positionList (const std::vector<std::string>& positions)
{
    std::string text;
    for (const auto& position : positions)
        text += (text.empty() ? "" : ",") + position;
    return text;
}

/** The filter's port names: "<strip>_<position>" in lower-case strip names
    for inputs ("game_FL", "music_FR"), "out_<position>" for outputs. */
inline std::string inputPortName (const NativeAudioNodeConfig::Strip& strip, const std::string& position)
{
    std::string name;
    for (const char c : strip.name)
        name += (c >= 'A' && c <= 'Z') ? static_cast<char> (c - 'A' + 'a') : ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : '_');
    return name + "_" + position;
}

inline std::string outputPortName (const std::string& position) { return "out_" + position; }

/** Flubsound's own sinks ("flubsound_<strip>"): never an output target,
    which would feed the engine its own output. */
inline bool isFlubsoundSink (const std::string& nodeName, const std::vector<NativeAudioNodeConfig::Strip>& strips)
{
    return std::any_of (strips.begin(), strips.end(), [&] (const NativeAudioNodeConfig::Strip& s) { return s.sinkName == nodeName; });
}

/** Pairs 'sources' with 'destinations' by audio.channel: each destination
    channel takes the source with the same channel; a MONO source feeds every
    destination and a MONO destination takes FL (else the first source).
    When no channel name matches at all (AUX ports, ports without
    audio.channel) they pair in order. */
inline std::vector<PortLink> pairByChannel (const std::vector<const Port*>& sources, const std::vector<const Port*>& destinations)
{
    std::vector<PortLink> links;
    bool anyMatch = false;
    for (const auto* destination : destinations)
    {
        const Port* match = nullptr;
        for (const auto* source : sources)
            if (! source->channel.empty() && source->channel == destination->channel)
                match = match != nullptr ? match : source;
        if (match == nullptr && sources.size() == 1 && sources.front()->channel == "MONO")
            match = sources.front();
        if (match == nullptr && destination->channel == "MONO" && ! sources.empty())
        {
            match = sources.front();
            for (const auto* source : sources)
                if (source->channel == "FL")
                    match = source;
        }
        if (match != nullptr)
        {
            anyMatch = true;
            links.push_back ({ match->id, destination->id });
        }
    }
    if (anyMatch)
        return links;

    for (size_t k = 0; k < std::min (sources.size(), destinations.size()); ++k)
        links.push_back ({ sources[k]->id, destinations[k]->id });
    return links;
}

/** The node's own ports of one direction whose names start with 'prefix'
    ("game_", "out_"), in port order. */
inline std::vector<const Port*> namedPorts (const Graph& graph, uint32_t nodeId, bool inputs, const std::string& prefix)
{
    std::vector<const Port*> result;
    for (const auto* port : audioPorts (graph, nodeId, inputs, false))
        if (port->name.rfind (prefix, 0) == 0)
            result.push_back (port);
    return result;
}

/** Links from each strip's sink monitors to the filter's input ports of that
    strip, matched by channel (a sink made with another channel order still
    reaches the right strip channel). A missing sink or a strip without its
    ports is a problem; a sink with fewer channels links what it has. */
inline LinkPlan planStripLinks (const Graph& graph, uint32_t filterNode, const std::vector<NativeAudioNodeConfig::Strip>& strips)
{
    LinkPlan plan;
    plan.engineNode = filterNode;
    if (filterNode == 0 || graph.nodes.count (filterNode) == 0)
    {
        plan.problems.push_back ("Flubsound's PipeWire node is not in the graph yet");
        return plan;
    }
    plan.engineInputs = static_cast<int> (audioPorts (graph, filterNode, true, false).size());

    int first = 0;
    for (const auto& strip : strips)
    {
        const int channels = static_cast<int> (strip.positions.size());
        const auto inputs = namedPorts (graph, filterNode, true, inputPortName (strip, ""));
        const Node* sink = findSink (graph, strip.sinkName);
        if (sink == nullptr)
            plan.problems.push_back ("PipeWire has no sink '" + strip.sinkName + "' for the " + strip.name + " strip");
        else if (inputs.empty())
            plan.problems.push_back ("Flubsound's node has no " + strip.name + " ports yet");
        else
        {
            const auto monitors = audioPorts (graph, sink->id, false, true);
            const auto links = pairByChannel (monitors, inputs);
            plan.wanted.insert (plan.wanted.end(), links.begin(), links.end());
            plan.sinks.push_back ({ strip.sinkName, first, static_cast<int> (links.size()) });
            if (monitors.empty())
                plan.problems.push_back ("The sink '" + strip.sinkName + "' has no monitor ports");
        }
        first += channels;
    }
    return plan;
}

/** The sink the node plays to when the app names none: the default output
    ("default" metadata), unless that is missing or one of Flubsound's own
    sinks (a user can make flubsound_system the default so that everything
    else is processed too). Then the sink with the highest priority.session
    that is not Flubsound's (lowest id on a tie); empty when there is none.
    'fellBack' says whether the default was passed over. */
inline std::string chooseOutputSink (const Graph& graph, const std::string& defaultSink, const std::vector<NativeAudioNodeConfig::Strip>& strips,
                                     bool& fellBack)
{
    fellBack = false;
    if (! defaultSink.empty() && ! isFlubsoundSink (defaultSink, strips) && findSink (graph, defaultSink) != nullptr)
        return defaultSink;
    const Node* best = nullptr;
    for (const auto& [id, node] : graph.nodes)
        if (node.mediaClass.rfind ("Audio/Sink", 0) == 0 && ! isFlubsoundSink (node.name, strips) && (best == nullptr || node.priority > best->priority))
            best = &node;
    fellBack = best != nullptr;
    return best != nullptr ? best->name : std::string();
}

/** R1.2: whether this PipeWire plays the desktop's audio, i.e. whether the
    native node would have an output to play to (chooseOutputSink finds a
    sink that is not one of Flubsound's own). A PipeWire that runs only for
    screen capture or cameras beside PulseAudio answers a connection too, but
    its graph has no Audio/Sink (PulseAudio owns the sound card), so the
    app's first start keeps JUCE's ALSA type there. A PipeWire desktop has
    one once a sound card is up (or WirePlumber's fallback "Dummy Output"
    null sink); a start before that also keeps ALSA, which PipeWire's own
    ALSA plug-in then serves. */
inline bool playsAudio (const Graph& graph, const std::string& defaultSink, const std::vector<NativeAudioNodeConfig::Strip>& strips)
{
    bool fellBack = false;
    return ! chooseOutputSink (graph, defaultSink, strips, fellBack).empty();
}

/** Links from the filter's output ports to 'target' (a sink's node.name)'s
    playback ports, matched by channel. Flubsound's own sinks are refused. */
inline LinkPlan planOutputLinks (const Graph& graph, uint32_t filterNode, const std::string& target,
                                 const std::vector<NativeAudioNodeConfig::Strip>& strips)
{
    LinkPlan plan;
    plan.engineNode = filterNode;
    if (filterNode == 0 || graph.nodes.count (filterNode) == 0)
        return plan;

    if (target.empty())
    {
        plan.problems.push_back ("PipeWire has no default output device; choose one as Flubsound's output");
        return plan;
    }
    if (isFlubsoundSink (target, strips))
    {
        plan.problems.push_back ("The output '" + target + "' is one of Flubsound's own inputs (a feedback loop): "
                                 "make a real device the default output or choose one as Flubsound's output");
        return plan;
    }
    const Node* sink = findSink (graph, target);
    if (sink == nullptr)
    {
        plan.problems.push_back ("PipeWire has no output device '" + target + "'");
        return plan;
    }

    const auto outputs = namedPorts (graph, filterNode, false, outputPortName (""));
    const auto playback = audioPorts (graph, sink->id, true, false);
    plan.wanted = pairByChannel (outputs, playback);
    plan.sinks.push_back ({ target, 0, static_cast<int> (plan.wanted.size()) });
    if (plan.wanted.empty())
        plan.problems.push_back ("The output device '" + target + "' has no playback ports");
    return plan;
}

/** How many of 'wanted' exist in the graph. */
inline int countLinked (const Graph& graph, const std::vector<PortLink>& wanted)
{
    int linked = 0;
    for (const auto& want : wanted)
        linked += std::any_of (graph.links.begin(), graph.links.end(), [&] (const Link& l)
                               { return l.outputPort == want.outputPort && l.inputPort == want.inputPort; })
                      ? 1
                      : 0;
    return linked;
}

/** The sink name in a "default" metadata value such as default.audio.sink,
    '{ "name": "alsa_output.usb-..." }'; empty when it has none. */
inline std::string metadataName (const char* value)
{
    if (value == nullptr)
        return {};
    json::Value root;
    std::string error;
    if (! json::parse (value, root, error) || ! root.isObject() || ! root["name"].isString())
        return {};
    return root["name"].asString();
}

/** One line for the routing panel, e.g. "Linked: 14 of 14 inputs, output
    to alsa_output.usb-headset (2 of 2)" followed by any problems. */
inline std::string describeLinks (const NativeAudioNodeStatus& status)
{
    std::string text = "Linked " + std::to_string (status.inputLinksMade) + " of " + std::to_string (status.inputLinksWanted) + " input channel"
                     + (status.inputLinksWanted == 1 ? "" : "s");
    if (! status.outputSink.empty())
        text += ", output to " + status.outputSink + " (" + std::to_string (status.outputLinksMade) + " of " + std::to_string (status.outputLinksWanted)
              + ")";
    else
        text += ", no output";
    if (status.quantumFrames != 0 && status.sampleRate != 0)
        text += ", quantum " + std::to_string (status.quantumFrames) + "/" + std::to_string (status.sampleRate);
    if (! status.message.empty())
        text += ". " + status.message;
    return text;
}
} // namespace flub::platform::pipewire
