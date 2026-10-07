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
#include <set>
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

    // ---- R4.5 (moving an app's own output away) -------------------------------------
    bool outputMovable = false;     // canMoveAppOutput(); off = the earlier tests' router
    bool canReadAppDevice = true;   // getAppEndpoint() answers
    /** Windows' per-app devices, per executable (lower case, no ".exe"); absent = the system default. */
    std::map<std::string, std::string> appDevices;
    /** setAppEndpoint behaves like the audio service: it sets the executable's
        per-app device, moves that executable's sessions there (the system
        default for ""), except pinned processes, and fails for an unknown
        process or one in failMoves. Off: every move just succeeds. */
    bool followMoves = false;
    std::string systemDefault;
    std::set<uint32_t> pinned;    // processes whose streams stay where they are (the app picks the device)
    std::set<uint32_t> failMoves; // processes whose moves fail ("Access is denied.")

    static std::string exeKey (const std::string& executable)
    {
        auto key = juce::String (executable).toLowerCase();
        if (key.endsWith (".exe"))
            key = key.dropLastCharacters (4);
        return key.toStdString();
    }

    /** Moves the sessions of `executable` (not pinned ones) to `endpoint` (lock held). */
    void placeSessions (const std::string& executable, const std::string& endpoint)
    {
        for (auto& s : sessions)
            if (exeKey (s.executableName) == executable && pinned.count (s.processId) == 0)
            {
                s.currentEndpointId = endpoint;
                s.activeEndpointIds.clear();
                if (s.isActive)
                    s.activeEndpointIds.push_back (endpoint);
            }
    }

    std::string getAppDevice (const std::string& executable)
    {
        const std::lock_guard<std::mutex> g (lock);
        const auto it = appDevices.find (exeKey (executable));
        return it != appDevices.end() ? it->second : std::string();
    }

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

    bool setAppEndpoint (uint32_t processId, const std::string& endpointId, std::string& error) override
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
        if (! script.followMoves)
            return true;

        const auto* session = findSession (processId);
        if (session == nullptr || script.failMoves.count (processId) != 0)
        {
            error = session == nullptr ? "No such process." : "Access is denied.";
            return false;
        }
        const auto executable = AudioServiceScript::exeKey (session->executableName);
        if (endpointId.empty())
            script.appDevices.erase (executable);
        else
            script.appDevices[executable] = endpointId;
        script.placeSessions (executable, endpointId.empty() ? script.systemDefault : endpointId);
        return true;
    }

    bool canMoveAppOutput() const override
    {
        const std::lock_guard<std::mutex> g (script.lock);
        return script.outputMovable;
    }

    bool getAppEndpoint (uint32_t processId, std::string& endpointId, std::string& error) override
    {
        const std::lock_guard<std::mutex> g (script.lock);
        endpointId.clear();
        const auto* session = findSession (processId);
        if (! script.canReadAppDevice || session == nullptr)
        {
            error = "unknown (test)";
            return false;
        }
        if (const auto it = script.appDevices.find (AudioServiceScript::exeKey (session->executableName)); it != script.appDevices.end())
            endpointId = it->second;
        return true;
    }

    void openSystemRoutingSettings() override {}

private:
    /** The first session of the process (script.lock held). */
    const flub::platform::AudioSessionInfo* findSession (uint32_t processId) const
    {
        for (const auto& s : script.sessions)
            if (s.processId == processId)
                return &s;
        return nullptr;
    }

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
