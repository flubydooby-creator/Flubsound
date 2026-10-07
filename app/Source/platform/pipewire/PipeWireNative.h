// Flubsound Pro - libpipewire client code (docs/11 E48). Built only when
// pkg-config finds libpipewire-0.3's headers (FLUB_HAS_PIPEWIRE=1, see
// PlatformServices.cmake); without them the app keeps the pw-dump / pw-link
// router and NativeAudioNode::isSupported() is false. The library itself is
// opened at run time (R1.2, PipeWireLibrary.h); where it does not load,
// Session::connect fails with the reason and the same fallbacks apply.
//
//   Session       one connection: a pw_thread_loop, its context and core,
//                 and a registry mirror (pipewire::RegistryState) that
//                 registry events keep current, plus the "default" metadata's
//                 default.audio.sink. onGraphChanged runs on the loop thread
//                 20 ms after the last change of a burst.
//   LinkSet       the links one owner made through the link factory
//                 (object.linger = false: they go away with the process).
//   RegistryLinker  AppAudioRouter::connectEndpointInputs for the JUCE
//                 device path: planMonitorLinks on the mirror, re-planned on
//                 every graph change instead of polling pw-dump.
//   PipeWireNativeNode (PipeWireNative.cpp)  the NativeAudioNode.
//
// Threading: everything that touches the mirror, a proxy or the core holds
// the loop lock (lock() / unlock(), recursive) or runs on the loop thread,
// which holds it while it dispatches. Never from the audio thread.
#pragma once

#include "PipeWireGraph.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_registry;
struct pw_proxy;
struct pw_metadata;
struct spa_source;

namespace flub::platform::pipewire
{
class Session
{
public:
    Session();
    ~Session();

    Session (const Session&) = delete;
    Session& operator= (const Session&) = delete;

    /** Starts the loop thread and connects to the server ($PIPEWIRE_REMOTE,
        else $XDG_RUNTIME_DIR/pipewire-0), then waits for the registry's
        first round trip. false with 'error' when there is no server. */
    bool connect (const std::string& clientName, std::string& error);

    /** Disconnects and stops the loop thread. Idempotent. Call without the
        loop lock held. */
    void disconnect();

    /** Connected and the server has not dropped the connection. */
    bool isConnected() const;

    void lock() const;
    void unlock() const;

    // With the loop lock held:
    const Graph& graph() const { return state.graph; }
    const std::string& defaultSink() const { return defaultSinkName; }
    pw_core* core() const { return coreHandle; }
    pw_registry* registry() const { return registryHandle; }
    pw_thread_loop* threadLoop() const { return loop; }

    /** Waits until the server has handled everything sent before (and the
        registry events that follow from it arrived). Loop lock held; the
        wait releases it. false on timeout or a lost connection. */
    bool roundTrip (int timeoutMs = 2000);

    /** Waits (loop lock held, released while waiting) until 'done' returns
        true, rechecked after every loop event; false on timeout. */
    bool waitFor (const std::function<bool()>& done, int timeoutMs);

    /** Runs on the loop thread (lock held) 20 ms after the last graph or
        default-sink change of a burst. Set it with the lock held. */
    std::function<void()> onGraphChanged;

    /** R1.2: runs on the loop thread (lock held) once, when the server drops
        the connection (it quit or restarted). Set it with the lock held. */
    std::function<void()> onConnectionLost;

    /** Arms the 20 ms debounce by hand (loop lock held), e.g. after a
        request changed what the owner wants. */
    void scheduleGraphChanged();

    struct Hooks; // libpipewire listener storage (PipeWireNative.cpp)

private:
    friend struct SessionEvents;

    void stopLoop();

    pw_thread_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* coreHandle = nullptr;
    pw_registry* registryHandle = nullptr;
    pw_metadata* defaultMetadata = nullptr;
    uint32_t defaultMetadataId = 0;
    spa_source* debounce = nullptr;
    std::unique_ptr<Hooks> hooks;

    RegistryState state;
    std::string defaultSinkName;
    int pendingSeq = -1;
    bool syncDone = false;
    bool connectionLost = false;
};

/** Links one owner made. Loop lock held for every call. */
class LinkSet
{
public:
    explicit LinkSet (Session& session);
    ~LinkSet();

    LinkSet (const LinkSet&) = delete;
    LinkSet& operator= (const LinkSet&) = delete;

    /** Makes the wanted links the graph does not have (object.linger =
        false) and removes this set's links that are no longer wanted.
        Links others made are left alone. A link the server refused is
        reported in 'problems' and tried again on the next apply(). Returns
        the number of links requested. */
    int apply (const std::vector<PortLink>& wanted, std::vector<std::string>& problems);

    /** Removes every link this set made. */
    void clear();

    size_t size() const { return owned.size(); }

    struct Owned; // one link proxy and its listener (PipeWireNative.cpp)

private:
    Session& session;
    std::map<PortLink, std::unique_ptr<Owned>> owned;
};

/** The JUCE device path's monitor links through the registry. */
class RegistryLinker
{
public:
    RegistryLinker();
    ~RegistryLinker();

    bool connect (std::string& error);
    bool isConnected() const { return session.isConnected(); }

    /** planMonitorLinks for this process and 'inputs', applied now and again
        on every graph change until the next call. false = a link is missing;
        'status' says why (user-presentable), as the pw-link path does. */
    bool update (const std::vector<AppAudioRouter::EndpointInput>& inputs, std::string& status);

    /** Links currently owned (tests). */
    size_t ownedLinks() const;

private:
    void relinkLocked();

    Session session;
    LinkSet links;
    std::vector<AppAudioRouter::EndpointInput> wantedInputs;
    bool lastOk = true;
    std::string lastStatus;
};
} // namespace flub::platform::pipewire
