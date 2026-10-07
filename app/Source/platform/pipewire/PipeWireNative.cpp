// Flubsound Pro - libpipewire client code: registry mirror, links, the
// native filter node (docs/11 E48). See PipeWireNative.h. Compiled only with
// FLUB_HAS_PIPEWIRE (pkg-config found libpipewire-0.3's headers); the
// library itself is opened at run time (R1.2, PipeWireLibrary.h).
#if defined(__linux__) && defined(FLUB_HAS_PIPEWIRE) && FLUB_HAS_PIPEWIRE

#include "PipeWireNative.h"

#include "PipeWireCycle.h"
#include "PipeWireLibrary.h"

#include <pipewire/extensions/metadata.h>
#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <pipewire/version.h>
#include <spa/node/io.h>
#include <spa/utils/dict.h>

#include "PipeWireApi.h"

#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>

// R1.2: every exported libpipewire function this file calls goes through the
// table PipeWireLibrary.cpp fills with dlsym (after the headers, so their
// inline functions are untouched). The app does not link libpipewire, so a
// call missing here fails the link instead of reaching the library.
#define pw_init (::flub::platform::pipewire::api().init)
#define pw_thread_loop_new (::flub::platform::pipewire::api().thread_loop_new)
#define pw_thread_loop_get_loop (::flub::platform::pipewire::api().thread_loop_get_loop)
#define pw_thread_loop_start (::flub::platform::pipewire::api().thread_loop_start)
#define pw_thread_loop_stop (::flub::platform::pipewire::api().thread_loop_stop)
#define pw_thread_loop_destroy (::flub::platform::pipewire::api().thread_loop_destroy)
#define pw_thread_loop_lock (::flub::platform::pipewire::api().thread_loop_lock)
#define pw_thread_loop_unlock (::flub::platform::pipewire::api().thread_loop_unlock)
#define pw_thread_loop_signal (::flub::platform::pipewire::api().thread_loop_signal)
#define pw_thread_loop_get_time (::flub::platform::pipewire::api().thread_loop_get_time)
#define pw_thread_loop_timed_wait_full (::flub::platform::pipewire::api().thread_loop_timed_wait_full)
#define pw_context_new (::flub::platform::pipewire::api().context_new)
#define pw_context_destroy (::flub::platform::pipewire::api().context_destroy)
#define pw_context_connect (::flub::platform::pipewire::api().context_connect)
#define pw_core_disconnect (::flub::platform::pipewire::api().core_disconnect)
#define pw_proxy_destroy (::flub::platform::pipewire::api().proxy_destroy)
#define pw_proxy_add_listener (::flub::platform::pipewire::api().proxy_add_listener)
#define pw_properties_new (::flub::platform::pipewire::api().properties_new)
#define pw_properties_setf (::flub::platform::pipewire::api().properties_setf)
#define pw_properties_free (::flub::platform::pipewire::api().properties_free)
#define pw_filter_new (::flub::platform::pipewire::api().filter_new)
#define pw_filter_add_listener (::flub::platform::pipewire::api().filter_add_listener)
#define pw_filter_add_port (::flub::platform::pipewire::api().filter_add_port)
#define pw_filter_connect (::flub::platform::pipewire::api().filter_connect)
#define pw_filter_disconnect (::flub::platform::pipewire::api().filter_disconnect)
#define pw_filter_destroy (::flub::platform::pipewire::api().filter_destroy)
#define pw_filter_get_node_id (::flub::platform::pipewire::api().filter_get_node_id)
#define pw_filter_get_dsp_buffer (::flub::platform::pipewire::api().filter_get_dsp_buffer)
#define pw_filter_update_properties (::flub::platform::pipewire::api().filter_update_properties)

namespace flub::platform::pipewire
{
namespace
{
void log (const std::string& text);

void ensureInitialised()
{
    static std::once_flag once;
    std::call_once (once, []
                    {
                        pw_init (nullptr, nullptr);
                        log ("libpipewire " + library().version + " opened at run time (built against " + std::string (pw_get_headers_version())
                             + ")");
                    });
}

const char* lookup (const spa_dict* dict, const char* key) { return dict != nullptr ? spa_dict_lookup (dict, key) : nullptr; }

std::string joined (const std::vector<std::string>& parts)
{
    std::string text;
    for (const auto& part : parts)
        text += (text.empty() ? "" : "; ") + part;
    return text;
}

void log (const std::string& text) { std::fprintf (stderr, "Flubsound: PipeWire: %s\n", text.c_str()); }

constexpr int kDebounceMs = 20;
} // namespace

//==============================================================================
// Session
//==============================================================================
struct Session::Hooks
{
    spa_hook core {};
    spa_hook registry {};
    spa_hook metadata {};
    pw_core_events coreEvents {};
    pw_registry_events registryEvents {};
    pw_metadata_events metadataEvents {};
};

struct SessionEvents
{
    static void coreDone (void* data, uint32_t id, int seq)
    {
        auto* session = static_cast<Session*> (data);
        if (id == PW_ID_CORE && seq == session->pendingSeq)
            session->syncDone = true;
        pw_thread_loop_signal (session->loop, false);
    }

    static void coreError (void* data, uint32_t id, int /*seq*/, int res, const char* message)
    {
        auto* session = static_cast<Session*> (data);
        if (id == PW_ID_CORE && res == -EPIPE)
        {
            const bool first = ! session->connectionLost;
            session->connectionLost = true;
            log (std::string ("lost the connection to the server") + (message != nullptr ? std::string (": ") + message : std::string()));
            if (first && session->onConnectionLost)
                session->onConnectionLost();
        }
        pw_thread_loop_signal (session->loop, false);
    }

    static void global (void* data, uint32_t id, uint32_t /*permissions*/, const char* type, uint32_t /*version*/, const spa_dict* props)
    {
        auto* session = static_cast<Session*> (data);
        if (type == nullptr)
            return;
        if (std::strcmp (type, PW_TYPE_INTERFACE_Metadata) == 0)
        {
            const char* name = lookup (props, PW_KEY_METADATA_NAME);
            if (name != nullptr && std::strcmp (name, "default") == 0 && session->defaultMetadata == nullptr)
            {
                session->defaultMetadata = static_cast<pw_metadata*> (pw_registry_bind (session->registryHandle, id, type, PW_VERSION_METADATA, 0));
                if (session->defaultMetadata != nullptr)
                {
                    session->defaultMetadataId = id;
                    pw_metadata_add_listener (session->defaultMetadata, &session->hooks->metadata, &session->hooks->metadataEvents, session);
                }
            }
            return;
        }
        if (addGlobal (session->state, id, type, [props] (const char* key) { return lookup (props, key); }))
            session->scheduleGraphChanged();
        pw_thread_loop_signal (session->loop, false);
    }

    static void globalRemove (void* data, uint32_t id)
    {
        auto* session = static_cast<Session*> (data);
        if (session->defaultMetadata != nullptr && id == session->defaultMetadataId)
        {
            spa_hook_remove (&session->hooks->metadata);
            pw_proxy_destroy (reinterpret_cast<pw_proxy*> (session->defaultMetadata));
            session->defaultMetadata = nullptr;
            session->defaultSinkName.clear();
            session->scheduleGraphChanged();
        }
        if (removeGlobal (session->state, id))
            session->scheduleGraphChanged();
        pw_thread_loop_signal (session->loop, false);
    }

    static int metadataProperty (void* data, uint32_t subject, const char* key, const char* /*type*/, const char* value)
    {
        auto* session = static_cast<Session*> (data);
        if (subject != PW_ID_CORE || (key != nullptr && std::strcmp (key, "default.audio.sink") != 0))
            return 0;
        auto name = key != nullptr ? metadataName (value) : std::string(); // key == nullptr: everything was cleared
        if (name != session->defaultSinkName)
        {
            session->defaultSinkName = std::move (name);
            session->scheduleGraphChanged();
        }
        pw_thread_loop_signal (session->loop, false);
        return 0;
    }

    static void debounceFired (void* data, uint64_t /*expirations*/)
    {
        auto* session = static_cast<Session*> (data);
        if (session->onGraphChanged)
            session->onGraphChanged();
    }
};

Session::Session() = default;

Session::~Session() { disconnect(); }

bool Session::connect (const std::string& clientName, std::string& error)
{
    disconnect();
    if (! library().loaded)
    {
        error = library().error; // R1.2: not installed; the caller falls back (ALSA / JACK, pw-link)
        return false;
    }
    ensureInitialised();

    loop = pw_thread_loop_new ("flubsound-pw", nullptr);
    if (loop == nullptr)
    {
        error = "Cannot start a PipeWire loop thread";
        return false;
    }
    // Real-time data thread for the filter's process callback: module-rt
    // (SCHED_FIFO where allowed, else RealtimeKit). PipeWire 1.1+ loads it
    // from client.conf; 1.0 only from client-rt.conf.
#if ! PW_CHECK_VERSION(1, 1, 0)
    context = pw_context_new (pw_thread_loop_get_loop (loop), pw_properties_new (PW_KEY_CONFIG_NAME, "client-rt.conf", nullptr), 0);
#endif
    if (context == nullptr)
        context = pw_context_new (pw_thread_loop_get_loop (loop), nullptr, 0);
    if (context == nullptr || pw_thread_loop_start (loop) < 0)
    {
        error = "Cannot create a PipeWire context";
        stopLoop();
        return false;
    }

    lock();
    hooks = std::make_unique<Hooks>();
    connectionLost = false;
    coreHandle = pw_context_connect (context, pw_properties_new (PW_KEY_APP_NAME, clientName.c_str(), nullptr), 0);
    if (coreHandle == nullptr)
    {
        const int code = errno;
        unlock();
        stopLoop();
        error = std::string ("No PipeWire server is running (") + std::strerror (code) + ")";
        return false;
    }

    hooks->coreEvents.version = PW_VERSION_CORE_EVENTS;
    hooks->coreEvents.done = &SessionEvents::coreDone;
    hooks->coreEvents.error = &SessionEvents::coreError;
    pw_core_add_listener (coreHandle, &hooks->core, &hooks->coreEvents, this);

    hooks->registryEvents.version = PW_VERSION_REGISTRY_EVENTS;
    hooks->registryEvents.global = &SessionEvents::global;
    hooks->registryEvents.global_remove = &SessionEvents::globalRemove;
    hooks->metadataEvents.version = PW_VERSION_METADATA_EVENTS;
    hooks->metadataEvents.property = &SessionEvents::metadataProperty;
    registryHandle = pw_core_get_registry (coreHandle, PW_VERSION_REGISTRY, 0);
    if (registryHandle != nullptr)
        pw_registry_add_listener (registryHandle, &hooks->registry, &hooks->registryEvents, this);

    debounce = pw_loop_add_timer (pw_thread_loop_get_loop (loop), &SessionEvents::debounceFired, this);

    // Two round trips: the registry globals, then the metadata bound from them.
    const bool answered = registryHandle != nullptr && roundTrip() && roundTrip();
    unlock();
    if (! answered)
    {
        disconnect();
        error = "The PipeWire server did not answer";
        return false;
    }
    return true;
}

void Session::disconnect()
{
    if (loop == nullptr)
        return;

    lock();
    onGraphChanged = nullptr;
    onConnectionLost = nullptr;
    if (debounce != nullptr)
        pw_loop_destroy_source (pw_thread_loop_get_loop (loop), debounce);
    debounce = nullptr;
    if (defaultMetadata != nullptr)
    {
        spa_hook_remove (&hooks->metadata);
        pw_proxy_destroy (reinterpret_cast<pw_proxy*> (defaultMetadata));
        defaultMetadata = nullptr;
    }
    if (registryHandle != nullptr)
    {
        spa_hook_remove (&hooks->registry);
        pw_proxy_destroy (reinterpret_cast<pw_proxy*> (registryHandle));
        registryHandle = nullptr;
    }
    if (coreHandle != nullptr)
    {
        spa_hook_remove (&hooks->core);
        pw_core_disconnect (coreHandle); // destroys every proxy of this connection
        coreHandle = nullptr;
    }
    unlock();

    stopLoop();
    state = {};
    defaultSinkName.clear();
}

void Session::stopLoop()
{
    if (loop != nullptr)
        pw_thread_loop_stop (loop);
    if (context != nullptr)
        pw_context_destroy (context);
    if (loop != nullptr)
        pw_thread_loop_destroy (loop);
    context = nullptr;
    loop = nullptr;
    hooks.reset();
}

bool Session::isConnected() const { return loop != nullptr && coreHandle != nullptr && ! connectionLost; }

void Session::lock() const
{
    if (loop != nullptr)
        pw_thread_loop_lock (loop);
}

void Session::unlock() const
{
    if (loop != nullptr)
        pw_thread_loop_unlock (loop);
}

void Session::scheduleGraphChanged()
{
    if (loop == nullptr || debounce == nullptr)
        return;
    timespec value {};
    value.tv_nsec = kDebounceMs * 1000000L;
    pw_loop_update_timer (pw_thread_loop_get_loop (loop), debounce, &value, nullptr, false);
}

bool Session::waitFor (const std::function<bool()>& done, int timeoutMs)
{
    if (loop == nullptr)
        return false;
    timespec deadline {};
    pw_thread_loop_get_time (loop, &deadline, static_cast<int64_t> (timeoutMs) * 1000000);
    while (! done())
        if (pw_thread_loop_timed_wait_full (loop, &deadline) != 0)
            return done();
    return true;
}

bool Session::roundTrip (int timeoutMs)
{
    if (coreHandle == nullptr || connectionLost)
        return false;
    syncDone = false;
    pendingSeq = pw_core_sync (coreHandle, PW_ID_CORE, 0);
    return waitFor ([this] { return syncDone || connectionLost; }, timeoutMs) && syncDone;
}

//==============================================================================
// LinkSet
//==============================================================================
struct LinkSet::Owned
{
    pw_proxy* proxy = nullptr;
    spa_hook listener {};
    pw_proxy_events events {};
    bool removed = false; // the server destroyed the link (a port went away) or refused it
    std::string error;

    void destroyProxy()
    {
        if (proxy == nullptr)
            return;
        spa_hook_remove (&listener);
        pw_proxy_destroy (proxy); // not lingering: the link goes with it
        proxy = nullptr;
    }

    static void onDestroy (void* data)
    {
        auto* self = static_cast<Owned*> (data);
        spa_hook_remove (&self->listener);
        self->proxy = nullptr; // the connection went away
        self->removed = true;
    }

    static void onRemoved (void* data) { static_cast<Owned*> (data)->removed = true; }

    static void onError (void* data, int /*seq*/, int res, const char* message)
    {
        auto* self = static_cast<Owned*> (data);
        self->removed = true;
        self->error = message != nullptr ? message : std::strerror (-res);
    }
};

LinkSet::LinkSet (Session& owner) : session (owner) {}

LinkSet::~LinkSet() { clear(); }

void LinkSet::clear()
{
    for (auto& entry : owned)
        entry.second->destroyProxy();
    owned.clear();
}

int LinkSet::apply (const std::vector<PortLink>& wanted, std::vector<std::string>& problems)
{
    const auto& graph = session.graph();
    const std::set<PortLink> wantedSet (wanted.begin(), wanted.end());
    const auto exists = [&graph] (const PortLink& link)
    {
        return std::any_of (graph.links.begin(), graph.links.end(), [&] (const Link& l) { return l.outputPort == link.outputPort && l.inputPort == link.inputPort; });
    };

    for (auto it = owned.begin(); it != owned.end();)
    {
        auto& link = *it->second;
        if (wantedSet.count (it->first) != 0 && ! link.removed && link.proxy != nullptr)
        {
            ++it;
            continue;
        }
        if (wantedSet.count (it->first) != 0 && ! link.error.empty())
            problems.push_back ("PipeWire refused the link from port " + std::to_string (it->first.outputPort) + " to " + std::to_string (it->first.inputPort)
                                + ": " + link.error);
        link.destroyProxy();
        it = owned.erase (it);
    }

    int requested = 0;
    for (const auto& want : wanted)
    {
        if (owned.count (want) != 0 || exists (want))
            continue;
        const auto findPort = [&graph] (uint32_t id) -> const Port*
        {
            for (const auto& port : graph.ports)
                if (port.id == id)
                    return &port;
            return nullptr;
        };
        const Port* out = findPort (want.outputPort);
        const Port* in = findPort (want.inputPort);
        if (out == nullptr || in == nullptr || session.core() == nullptr)
            continue;

        auto* props = pw_properties_new (PW_KEY_OBJECT_LINGER, "false", nullptr);
        pw_properties_setf (props, PW_KEY_LINK_OUTPUT_NODE, "%u", out->nodeId);
        pw_properties_setf (props, PW_KEY_LINK_OUTPUT_PORT, "%u", out->id);
        pw_properties_setf (props, PW_KEY_LINK_INPUT_NODE, "%u", in->nodeId);
        pw_properties_setf (props, PW_KEY_LINK_INPUT_PORT, "%u", in->id);
        auto* proxy = static_cast<pw_proxy*> (pw_core_create_object (session.core(), "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0));
        pw_properties_free (props);
        if (proxy == nullptr)
        {
            problems.push_back ("Cannot ask PipeWire for a link (" + std::string (std::strerror (errno)) + ")");
            continue;
        }

        auto link = std::make_unique<Owned>();
        link->proxy = proxy;
        link->events.version = PW_VERSION_PROXY_EVENTS;
        link->events.destroy = &Owned::onDestroy;
        link->events.removed = &Owned::onRemoved;
        link->events.error = &Owned::onError;
        pw_proxy_add_listener (proxy, &link->listener, &link->events, link.get());
        owned.emplace (want, std::move (link));
        ++requested;
    }
    return requested;
}

//==============================================================================
// RegistryLinker
//==============================================================================
RegistryLinker::RegistryLinker() : links (session) {}

RegistryLinker::~RegistryLinker()
{
    session.lock();
    session.onGraphChanged = nullptr;
    links.clear();
    session.unlock();
    session.disconnect();
}

bool RegistryLinker::connect (std::string& error)
{
    if (! session.connect ("Flubsound Pro (routing)", error))
        return false;
    session.lock();
    session.onGraphChanged = [this] { relinkLocked(); };
    session.unlock();
    return true;
}

bool RegistryLinker::update (const std::vector<AppAudioRouter::EndpointInput>& inputs, std::string& status)
{
    session.lock();
    const bool changed = inputs != wantedInputs;
    wantedInputs = inputs;
    if (changed)
    {
        relinkLocked();
        // Wait for the links to show up, so the answer is about them.
        session.roundTrip (500);
        relinkLocked();
    }
    const bool ok = lastOk;
    status = lastStatus;
    session.unlock();
    return ok;
}

size_t RegistryLinker::ownedLinks() const
{
    session.lock();
    const auto count = links.size();
    session.unlock();
    return count;
}

void RegistryLinker::relinkLocked()
{
    const auto plan = planMonitorLinks (session.graph(), static_cast<uint32_t> (::getpid()), wantedInputs);
    auto problems = plan.problems;
    const int requested = links.apply (plan.wanted, problems);
    if (requested > 0)
        for (const auto& sink : plan.sinks)
            log ("'" + sink.sinkName + "' monitor -> Flubsound input channels " + std::to_string (sink.firstChannel + 1) + "-"
                 + std::to_string (sink.firstChannel + sink.channels));
    if (countLinked (session.graph(), plan.wanted) < static_cast<int> (plan.wanted.size()) && requested == 0 && problems.empty())
        problems.push_back ("Waiting for PipeWire to make " + std::to_string (plan.wanted.size()) + " links");
    auto text = joined (problems);
    if (! text.empty())
        text += ".";
    lastOk = problems.empty();
    lastStatus = text;
}

//==============================================================================
// PipeWireNativeNode
//==============================================================================
namespace
{
class PipeWireNativeNode final : public NativeAudioNode
{
public:
    PipeWireNativeNode() : links (session) {}
    ~PipeWireNativeNode() override { stop(); }

    // R1.2: libpipewire is opened at run time; without it, say why.
    bool isSupported() const override { return library().loaded; }
    std::string unsupportedReason() const override { return library().loaded ? std::string() : library().error; }
    int getXrunCount() const noexcept override { return xrunCounter.count(); }

    bool start (const NativeAudioNodeConfig& requested, Callback& cb, std::string& error) override
    {
        stop();
        config = requested;
        if (config.strips.empty())
            config.strips = defaultStrips();
        config.outputChannels = std::clamp (config.outputChannels, 1, 8);
        config.maxBlockFrames = std::clamp (config.maxBlockFrames, 16, 8192);
        if (config.sampleRate < 8000 || config.sampleRate > 768000)
            config.sampleRate = 48000;

        if (! isSupported())
        {
            error = unsupportedReason();
            return false;
        }
        if (! session.connect ("Flubsound Pro", error))
            return false;

        int numInputs = 0;
        for (const auto& strip : config.strips)
            numInputs += static_cast<int> (strip.positions.size());
        runner.prepare (numInputs, config.outputChannels, config.maxBlockFrames);
        inputBuffers.assign (static_cast<size_t> (numInputs), nullptr);
        outputBuffers.assign (static_cast<size_t> (config.outputChannels), nullptr);
        quantum.store (0);
        rate.store (0);
        cycles.store (0);
        xrunCounter.reset();
        errorReported = false;
        callback = &cb;
        cb.nodeStarting (static_cast<double> (config.sampleRate), config.maxBlockFrames);

        session.lock();
        session.onConnectionLost = [this] { reportErrorLocked ("The PipeWire server went away (it quit or restarted)"); };
        createMissingSinksLocked();
        if (! createFilterLocked (error))
        {
            session.unlock();
            stop();
            return false;
        }
        lockTimer = pw_loop_add_timer (pw_thread_loop_get_loop (session.threadLoop()), &PipeWireNativeNode::onLockTimer, this);
        armQuantumLockLocked();
        session.onGraphChanged = [this] { relinkLocked(); };
        relinkLocked();
        session.roundTrip (500);
        relinkLocked();
        logProblems = true; // from here on a problem is news, not start-up
        if (! linkStatus.message.empty())
            log (linkStatus.message);
        running.store (true);
        session.unlock();
        return true;
    }

    void stop() override
    {
        if (session.threadLoop() != nullptr)
        {
            session.lock();
            session.onGraphChanged = nullptr;
            session.onConnectionLost = nullptr;
            errorReported = true; // a stop is not an error
            if (lockTimer != nullptr)
                pw_loop_destroy_source (pw_thread_loop_get_loop (session.threadLoop()), lockTimer);
            lockTimer = nullptr;
            if (filter != nullptr)
            {
                spa_hook_remove (&filterListener);
                pw_filter_disconnect (filter);
                pw_filter_destroy (filter); // no process() call after this returns
                filter = nullptr;
            }
            links.clear();
            for (auto* proxy : sinkProxies)
                pw_proxy_destroy (proxy); // the sink goes with it (not lingering)
            sinkProxies.clear();
            session.unlock();
            session.disconnect();
        }
        inputPorts.clear();
        outputPorts.clear();
        nodeId = 0;
        linkStatus = {};
        logProblems = false;
        running.store (false);
        if (callback != nullptr)
        {
            callback->nodeStopped();
            callback = nullptr;
        }
    }

    bool isRunning() const override { return running.load(); }

    bool setLatency (NativeAudioNodeConfig::Latency latency) override
    {
        session.lock();
        config.latency = latency;
        const bool ok = filter != nullptr && updateLatencyLocked (false);
        if (ok)
            armQuantumLockLocked();
        session.unlock();
        return ok;
    }

    bool setOutputTarget (const std::string& sinkName) override
    {
        session.lock();
        config.outputTarget = sinkName;
        const bool ok = filter != nullptr;
        if (ok)
            relinkLocked();
        session.unlock();
        return ok;
    }

    NativeAudioNodeStatus getStatus() const override
    {
        session.lock();
        auto status = linkStatus;
        status.running = running.load() && session.isConnected();
        status.nodeId = nodeId;
        session.unlock();
        status.quantumFrames = quantum.load (std::memory_order_relaxed);
        status.sampleRate = rate.load (std::memory_order_relaxed);
        status.cycles = cycles.load (std::memory_order_relaxed);
        return status;
    }

private:
    /** PipeWire's data thread (PW_FILTER_FLAG_RT_PROCESS): buffer lookups
        (lock-free in libpipewire), the cycle runner and the xrun count (a
        vDSO clock read); nothing else. */
    static void onProcess (void* data, spa_io_position* position)
    {
        auto* self = static_cast<PipeWireNativeNode*> (data);
        if (position == nullptr || self->callback == nullptr)
            return;
        const auto& driver = position->clock;
        const auto frames = static_cast<uint32_t> (driver.duration);
        for (size_t i = 0; i < self->inputPorts.size(); ++i)
            self->inputBuffers[i] = static_cast<const float*> (pw_filter_get_dsp_buffer (self->inputPorts[i], frames));
        for (size_t o = 0; o < self->outputPorts.size(); ++o)
            self->outputBuffers[o] = static_cast<float*> (pw_filter_get_dsp_buffer (self->outputPorts[o], frames));
        self->runner.run (self->inputBuffers.data(), self->outputBuffers.data(), frames, *self->callback, driver.nsec, driver.rate.denom);

        // R1.2: was this cycle late, or were cycles missed? (XrunCounter)
        timespec now {};
        ::clock_gettime (CLOCK_MONOTONIC, &now);
        CycleClock cycle;
        cycle.driverId = driver.id;
        cycle.rate = driver.rate.denom;
        cycle.nsec = driver.nsec;
        cycle.position = driver.position;
        cycle.duration = driver.duration;
#ifdef SPA_IO_CLOCK_FLAG_FREEWHEEL
        cycle.freewheel = (driver.flags & SPA_IO_CLOCK_FLAG_FREEWHEEL) != 0;
#endif
        self->xrunCounter.cycleDone (cycle, static_cast<uint64_t> (now.tv_sec) * CycleRunner::kNsPerSecond + static_cast<uint64_t> (now.tv_nsec));

        self->quantum.store (frames, std::memory_order_relaxed);
        self->rate.store (driver.rate.denom, std::memory_order_relaxed);
        self->cycles.fetch_add (1, std::memory_order_relaxed);
    }

    /** Loop thread, loop lock held: tells the owner once per run that the
        node stopped working (nodeError), so it can re-open it. */
    void reportErrorLocked (const std::string& message)
    {
        if (errorReported || ! running.load() || callback == nullptr)
            return;
        errorReported = true;
        log (message);
        callback->nodeError (message);
    }

    /*  node.lock-quantum keeps the graph at the quantum it has when the
        lock arrives, so a lock sent together with a smaller node.latency
        would freeze the old one. The latency goes first, unlocked; this
        timer (loop thread, every 20 ms) locks once the data thread reports
        the requested quantum, and gives up after a second (another client
        holds a larger quantum: the request stays, unlocked). */
    static void onLockTimer (void* data, uint64_t /*expirations*/)
    {
        auto* self = static_cast<PipeWireNativeNode*> (data);
        const auto wanted = nodeLatency (quantumRequestFor (self->config.latency));
        if (! wanted.lockQuantum || self->filter == nullptr)
            return self->disarmLockTimerLocked();
        const auto frames = self->quantum.load (std::memory_order_relaxed), graphRate = self->rate.load (std::memory_order_relaxed);
        if (frames != 0 && graphRate != 0 && static_cast<double> (frames) / graphRate <= requestedSeconds (wanted.latency) * 1.001)
        {
            self->updateLatencyLocked (true);
            return self->disarmLockTimerLocked();
        }
        if (++self->lockTries >= 50)
        {
            log ("the graph stays at a quantum of " + std::to_string (frames) + " (another client holds it), so Low Latency's "
                 + wanted.latency + " is requested but not locked");
            self->disarmLockTimerLocked();
        }
    }

    static double requestedSeconds (const std::string& latency)
    {
        const auto slash = latency.find ('/');
        const double frames = std::strtod (latency.c_str(), nullptr), rate = std::strtod (latency.c_str() + slash + 1, nullptr);
        return rate > 0.0 ? frames / rate : 0.0;
    }

    void armQuantumLockLocked()
    {
        lockTries = 0;
        if (lockTimer == nullptr || ! nodeLatency (quantumRequestFor (config.latency)).lockQuantum)
            return;
        timespec interval {};
        interval.tv_nsec = 20 * 1000000L;
        pw_loop_update_timer (pw_thread_loop_get_loop (session.threadLoop()), lockTimer, &interval, &interval, false);
    }

    void disarmLockTimerLocked()
    {
        if (lockTimer == nullptr)
            return;
        timespec zero {};
        pw_loop_update_timer (pw_thread_loop_get_loop (session.threadLoop()), lockTimer, &zero, nullptr, false);
    }

    static void onStateChanged (void* data, pw_filter_state /*old*/, pw_filter_state state, const char* error)
    {
        auto* self = static_cast<PipeWireNativeNode*> (data);
        self->filterState = state;
        if (state == PW_FILTER_STATE_ERROR)
            self->filterError = error != nullptr ? error : "unknown error";
        if (self->filter != nullptr)
        {
            const uint32_t id = pw_filter_get_node_id (self->filter);
            if (id != SPA_ID_INVALID)
                self->nodeId = id;
        }
        // R1.2: while running (stop() removes this listener before it
        // disconnects), an error or an unconnected filter means the server
        // failed or removed the node: no more cycles until a re-open.
        if (state == PW_FILTER_STATE_ERROR)
            self->reportErrorLocked ("PipeWire stopped the Flubsound node: " + self->filterError);
        else if (state == PW_FILTER_STATE_UNCONNECTED)
            self->reportErrorLocked ("PipeWire removed the Flubsound node" + (error != nullptr ? " (" + std::string (error) + ")" : std::string()));
        pw_thread_loop_signal (self->session.threadLoop(), false);
    }

    void createMissingSinksLocked()
    {
        linkStatus.createdSinks.clear();
        if (! config.createSinks)
            return;
        for (const auto& strip : config.strips)
        {
            if (findSink (session.graph(), strip.sinkName) != nullptr)
                continue;
            auto* props = pw_properties_new ("factory.name", "support.null-audio-sink", PW_KEY_NODE_NAME, strip.sinkName.c_str(),
                                             PW_KEY_NODE_DESCRIPTION, strip.description.c_str(), PW_KEY_MEDIA_CLASS, "Audio/Sink",
                                             "audio.position", positionList (strip.positions).c_str(), "monitor.channel-volumes", "true",
                                             PW_KEY_OBJECT_LINGER, "false", nullptr);
            auto* proxy = static_cast<pw_proxy*> (pw_core_create_object (session.core(), "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &props->dict, 0));
            pw_properties_free (props);
            if (proxy == nullptr)
                continue;
            sinkProxies.push_back (proxy);
            linkStatus.createdSinks.push_back (strip.sinkName);
        }
        // The sinks' monitor ports reach the registry a little after the
        // sinks: wait for them (at most 1 s), so the first plan links them.
        if (! sinkProxies.empty())
            session.waitFor ([this]
                             {
                                 for (const auto& strip : config.strips)
                                 {
                                     const Node* sink = findSink (session.graph(), strip.sinkName);
                                     if (sink == nullptr || audioPorts (session.graph(), sink->id, false, true).size() < strip.positions.size())
                                         return false;
                                 }
                                 return true;
                             },
                             1000);
    }

    bool createFilterLocked (std::string& error)
    {
        // Unlocked at first; armQuantumLockLocked locks Low Latency's quantum
        // once the graph runs at it.
        const auto latency = nodeLatency (quantumRequestFor (config.latency));
        // A DSP filter (pw-filter's own example properties), not a device:
        // no media.class, so the session manager neither offers it as a sink
        // or source nor links it; the node links itself. node.name stays an
        // identifier ("flubsound_engine"); the desktop shows the description,
        // the nick and the application name (R1.2).
        auto* props = pw_properties_new (PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Filter", PW_KEY_MEDIA_ROLE, "DSP",
                                         PW_KEY_NODE_NAME, config.nodeName.c_str(), PW_KEY_NODE_DESCRIPTION, config.nodeDescription.c_str(),
                                         PW_KEY_NODE_NICK, "Flubsound Pro", PW_KEY_APP_NAME, "Flubsound Pro",
                                         PW_KEY_NODE_LATENCY, latency.latency.c_str(), PW_KEY_NODE_LOCK_QUANTUM, "false",
                                         PW_KEY_NODE_ALWAYS_PROCESS, "true", nullptr);
        pw_properties_setf (props, PW_KEY_NODE_RATE, "1/%u", config.sampleRate);
        filter = pw_filter_new (session.core(), config.nodeName.c_str(), props);
        if (filter == nullptr)
        {
            error = std::string ("Cannot create the PipeWire filter (") + std::strerror (errno) + ")";
            return false;
        }
        filterEvents = {};
        filterEvents.version = PW_VERSION_FILTER_EVENTS;
        filterEvents.state_changed = &PipeWireNativeNode::onStateChanged;
        filterEvents.process = &PipeWireNativeNode::onProcess;
        pw_filter_add_listener (filter, &filterListener, &filterEvents, this);

        const auto addPort = [this] (pw_direction direction, const std::string& name, const std::string& channel)
        {
            return pw_filter_add_port (filter, direction, PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof (uint64_t),
                                       pw_properties_new (PW_KEY_FORMAT_DSP, "32 bit float mono audio", PW_KEY_PORT_NAME, name.c_str(),
                                                          PW_KEY_AUDIO_CHANNEL, channel.c_str(), nullptr),
                                       nullptr, 0);
        };
        for (const auto& strip : config.strips)
            for (const auto& position : strip.positions)
                inputPorts.push_back (addPort (PW_DIRECTION_INPUT, inputPortName (strip, position), position));
        for (const auto& position : outputPositions (config.outputChannels))
            outputPorts.push_back (addPort (PW_DIRECTION_OUTPUT, outputPortName (position), position));
        if (std::find (inputPorts.begin(), inputPorts.end(), nullptr) != inputPorts.end()
            || std::find (outputPorts.begin(), outputPorts.end(), nullptr) != outputPorts.end())
        {
            error = "Cannot add the PipeWire filter's ports";
            return false;
        }

        filterError.clear();
        if (pw_filter_connect (filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) < 0)
        {
            error = std::string ("Cannot connect the PipeWire filter (") + std::strerror (errno) + ")";
            return false;
        }
        const bool bound = session.waitFor ([this] { return nodeId != 0 || ! filterError.empty(); }, 2000);
        if (! bound || nodeId == 0)
        {
            error = "The PipeWire filter did not start" + (filterError.empty() ? std::string() : ": " + filterError);
            return false;
        }
        // Its ports reach the registry after the node (at most 1 s).
        const auto ports = inputPorts.size() + outputPorts.size();
        session.waitFor ([this, ports] { return audioPorts (session.graph(), nodeId, true, false).size() + audioPorts (session.graph(), nodeId, false, false).size() >= ports; },
                         1000);
        return true;
    }

    /** node.latency for the profile, with node.lock-quantum = 'lock'. */
    bool updateLatencyLocked (bool lock)
    {
        const auto latency = nodeLatency (quantumRequestFor (config.latency));
        spa_dict_item items[2] {};
        items[0].key = PW_KEY_NODE_LATENCY;
        items[0].value = latency.latency.c_str();
        items[1].key = PW_KEY_NODE_LOCK_QUANTUM;
        items[1].value = lock && latency.lockQuantum ? "true" : "false";
        spa_dict dict {};
        dict.n_items = 2;
        dict.items = items;
        return pw_filter_update_properties (filter, nullptr, &dict) >= 0;
    }

    void relinkLocked()
    {
        const auto& graph = session.graph();
        const auto inputs = planStripLinks (graph, nodeId, config.strips);
        bool fellBack = false;
        const auto target = config.outputTarget.empty() ? chooseOutputSink (graph, session.defaultSink(), config.strips, fellBack) : config.outputTarget;
        const auto outputs = planOutputLinks (graph, nodeId, target, config.strips);

        auto wanted = inputs.wanted;
        wanted.insert (wanted.end(), outputs.wanted.begin(), outputs.wanted.end());
        auto problems = inputs.problems;
        problems.insert (problems.end(), outputs.problems.begin(), outputs.problems.end());
        if (fellBack && isFlubsoundSink (session.defaultSink(), config.strips))
            problems.push_back ("The default output is Flubsound's own '" + session.defaultSink() + "', so Flubsound plays to '" + target + "'");
        links.apply (wanted, problems);

        linkStatus.inputLinksWanted = static_cast<int> (inputs.wanted.size());
        linkStatus.inputLinksMade = countLinked (graph, inputs.wanted);
        linkStatus.outputLinksWanted = static_cast<int> (outputs.wanted.size());
        linkStatus.outputLinksMade = countLinked (graph, outputs.wanted);
        linkStatus.outputSink = outputs.wanted.empty() ? std::string() : target;
        const auto message = joined (problems);
        if (logProblems && ! message.empty() && message != linkStatus.message)
            log (message);
        linkStatus.message = message;
    }

    mutable Session session;
    LinkSet links;
    NativeAudioNodeConfig config;
    Callback* callback = nullptr;

    // Loop thread / loop lock:
    pw_filter* filter = nullptr;
    spa_hook filterListener {};
    pw_filter_events filterEvents {};
    pw_filter_state filterState = PW_FILTER_STATE_UNCONNECTED;
    std::string filterError;
    uint32_t nodeId = 0;
    spa_source* lockTimer = nullptr;
    int lockTries = 0;
    bool logProblems = false;
    std::vector<pw_proxy*> sinkProxies;
    NativeAudioNodeStatus linkStatus;

    // Set before the filter connects, read by the data thread:
    std::vector<void*> inputPorts, outputPorts;
    std::vector<const float*> inputBuffers;
    std::vector<float*> outputBuffers;
    CycleRunner runner;

    std::atomic<uint32_t> quantum { 0 }, rate { 0 };
    std::atomic<uint64_t> cycles { 0 };
    std::atomic<bool> running { false };
    XrunCounter xrunCounter;     // the data thread counts, any thread reads (R1.2)
    bool errorReported = false;  // loop thread / loop lock: nodeError sent this run
};
} // namespace
} // namespace flub::platform::pipewire

namespace flub::platform
{
std::unique_ptr<NativeAudioNode> NativeAudioNode::create() { return std::make_unique<pipewire::PipeWireNativeNode>(); }
} // namespace flub::platform

#endif
