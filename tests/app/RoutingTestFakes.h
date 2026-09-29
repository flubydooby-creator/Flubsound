// Flubsound Pro - fakes for the docs/11 E47 routing tests (route journal,
// doubling guard): a scripted platform::AppAudioRouter ("the OS's audio
// service": sessions, output endpoints, every endpoint move) and process
// captures that always start. AppRouting owns the router and calls it from
// its worker thread, so the script is shared and locked.
#pragma once

#include "AppTestSupport.h"

#include "engine/AppRouting.h"
#include "engine/AudioEngineHost.h"
#include "platform/PlatformServices.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace flubapptest
{
struct AudioServiceScript
{
    std::mutex lock;
    bool movable = true; // canMoveEndpoint()
    std::vector<flub::platform::AudioSessionInfo> sessions;
    std::vector<flub::platform::AppAudioRouter::OutputEndpoint> endpoints;
    std::vector<std::pair<uint32_t, std::string>> moves; // every setAppEndpoint call (pid, endpoint)
    int enumerations = 0;
    /** Runs on the worker thread at each move, before it is recorded (no lock held). */
    std::function<void (uint32_t, const std::string&)> onMove;

    void setSessions (std::vector<flub::platform::AudioSessionInfo> s)
    {
        const std::lock_guard<std::mutex> g (lock);
        sessions = std::move (s);
    }

    std::vector<std::pair<uint32_t, std::string>> getMoves()
    {
        const std::lock_guard<std::mutex> g (lock);
        return moves;
    }

    int getEnumerations()
    {
        const std::lock_guard<std::mutex> g (lock);
        return enumerations;
    }

    int countMoves (uint32_t pid, const std::string& endpoint)
    {
        int n = 0;
        for (const auto& m : getMoves())
            n += m.first == pid && m.second == endpoint ? 1 : 0;
        return n;
    }
};

class ScriptedRouter final : public flub::platform::AppAudioRouter
{
public:
    explicit ScriptedRouter (AudioServiceScript& s) : script (s) {}

    bool isSupported() const override { return canMoveEndpoint(); }
    bool canList() const override { return true; }

    bool canMoveEndpoint() const override
    {
        const std::lock_guard<std::mutex> g (script.lock);
        return script.movable;
    }

    std::vector<flub::platform::AudioSessionInfo> enumerateSessions() override
    {
        const std::lock_guard<std::mutex> g (script.lock);
        ++script.enumerations;
        return script.sessions;
    }

    std::vector<OutputEndpoint> listOutputEndpoints() override
    {
        const std::lock_guard<std::mutex> g (script.lock);
        return script.endpoints;
    }

    bool setAppEndpoint (uint32_t processId, const std::string& endpointId, std::string&) override
    {
        std::function<void (uint32_t, const std::string&)> hook;
        {
            const std::lock_guard<std::mutex> g (script.lock);
            hook = script.onMove;
        }
        if (hook != nullptr)
            hook (processId, endpointId);
        const std::lock_guard<std::mutex> g (script.lock);
        script.moves.emplace_back (processId, endpointId);
        return true;
    }

    void openSystemRoutingSettings() override {}

private:
    AudioServiceScript& script;
};

/** A capture that always starts and counts its starts per process. */
class CountingCapture final : public flub::platform::ProcessLoopbackCapture
{
public:
    explicit CountingCapture (std::shared_ptr<std::map<uint32_t, int>> s) : starts (std::move (s)) {}

    bool isSupported() const override { return true; }

    bool start (uint32_t processId, bool, double, int, FrameCallback, std::string&) override
    {
        ++(*starts)[processId];
        running = true;
        return true;
    }

    void stop() override { running = false; }
    bool isRunning() const override { return running; }

private:
    std::shared_ptr<std::map<uint32_t, int>> starts;
    bool running = false;
};

/** Installs CountingCaptures on the host; returns their start counts (message thread). */
inline std::shared_ptr<std::map<uint32_t, int>> useCountingCaptures (flub::app::AudioEngineHost& host)
{
    auto starts = std::make_shared<std::map<uint32_t, int>>();
    host.setCaptureFactory ([starts] { return std::make_unique<CountingCapture> (starts); });
    return starts;
}

inline flub::platform::AudioSessionInfo makeSession (uint32_t pid, const char* exe, const char* endpoint = "", uint64_t startTime = 0,
                                                     bool active = true)
{
    flub::platform::AudioSessionInfo s;
    s.processId = pid;
    s.executableName = exe;
    s.currentEndpointId = endpoint;
    s.processStartTime = startTime;
    s.isActive = active;
    if (active && endpoint[0] != '\0')
        s.activeEndpointIds.push_back (endpoint);
    return s;
}

inline const flub::app::AppRouting::AppState* findRoutedApp (const flub::app::AppRouting& routing, uint32_t pid)
{
    for (const auto& a : routing.getApps())
        if (a.processId == pid)
            return &a;
    return nullptr;
}
} // namespace flubapptest
