#include "AppRouting.h"

#include "AudioEngineHost.h"
#include "platform/PlatformBridge.h"

#include <algorithm>
#include <iterator>
#include <set>

namespace flub::app
{
namespace
{
constexpr int kRefreshIntervalMs = 2000;
constexpr int kMaxCaptureAttempts = 3;

juce::String normaliseExecutable (const juce::String& name)
{
    auto n = name.trim().replaceCharacter ('\\', '/').fromLastOccurrenceOf ("/", false, false).toLowerCase();
    if (n.endsWith (".exe"))
        n = n.dropLastCharacters (4);
    return n;
}

bool sameState (const AppRouting::AppState& x, const AppRouting::AppState& y)
{
    return x.processId == y.processId && x.strip == y.strip && x.isActive == y.isActive && x.routed == y.routed
           && x.captureId == y.captureId && x.error == y.error && x.endpointId == y.endpointId;
}
} // namespace

AppRouting::AppRouting (AudioEngineHost& h, AppSettings& s)
    : juce::Thread ("Flubsound routing"), host (h), settings (s)
{
    router = platform_bridge::createAppAudioRouter();
    captureSupported = platform_bridge::isProcessCaptureSupported();
    method = settings.getRoutingMethod();
    routes = settings.getAppRoutes();
}

AppRouting::~AppRouting()
{
    shutdown();
}

void AppRouting::start()
{
    if (isStarted || isShutDown)
        return;
    isStarted = true;
    publishConfig();
    if (router != nullptr)
        startThread (juce::Thread::Priority::low);
    refresh();
}

void AppRouting::shutdown()
{
    if (isShutDown)
        return;
    isShutDown = true;

    if (isThreadRunning())
    {
        signalThreadShouldExit();
        notify();
        stopThread (5000);
    }
    cancelPendingUpdate();

    // The worker is stopped: the router and routedEndpoints are ours now.
    if (router != nullptr)
    {
        for (const auto& [pid, endpoint] : routedEndpoints)
        {
            std::string error;
            router->setAppEndpoint (pid, {}, error); // back to the system default
        }
    }
    routedEndpoints.clear();
    stopAllCaptures();
}

// =============================================================================
bool AppRouting::isEndpointRoutingSupported() const noexcept
{
    return router != nullptr && router->isSupported();
}

AppRouting::Method AppRouting::getEffectiveMethod() const noexcept
{
    switch (method)
    {
        case Method::EndpointRouting: return isEndpointRoutingSupported() ? Method::EndpointRouting : Method::Disabled;
        case Method::ProcessCapture: return captureSupported ? Method::ProcessCapture : Method::Disabled;
        case Method::Disabled: return Method::Disabled;
        case Method::Automatic: break;
    }
    if (isEndpointRoutingSupported())
        return Method::EndpointRouting;
    if (captureSupported)
        return Method::ProcessCapture;
    return Method::Disabled;
}

void AppRouting::setMethod (Method newMethod)
{
    if (newMethod == method)
        return;
    method = newMethod;
    settings.setRoutingMethod (method);
    publishConfig(); // the worker tears down what the previous method set up
    refresh();
}

// =============================================================================
bool AppRouting::executablesMatch (const juce::String& a, const juce::String& b)
{
    return normaliseExecutable (a) == normaliseExecutable (b);
}

void AppRouting::setRoute (const juce::String& executable, const juce::String& stripName)
{
    if (executable.trim().isEmpty())
        return;

    routes.erase (std::remove_if (routes.begin(), routes.end(),
                                  [&] (const AppRoute& r) { return executablesMatch (r.executable, executable); }),
                  routes.end());
    if (stripName.isNotEmpty())
        routes.push_back ({ executable.trim(), stripName });

    settings.setAppRoutes (routes);
    publishConfig();
    refresh();
}

void AppRouting::removeRoute (const juce::String& executable)
{
    setRoute (executable, {});
}

juce::String AppRouting::getStripNameForExecutable (const juce::String& executable) const
{
    for (const auto& r : routes)
        if (executablesMatch (r.executable, executable))
            return r.stripName;
    return {};
}

void AppRouting::setLiveUpdates (bool shouldUpdate)
{
    liveUpdates = shouldUpdate;
    publishConfig();
    if (shouldUpdate)
        refresh();
}

void AppRouting::stripLayoutChanged()
{
    publishConfig();
    refresh();
}

void AppRouting::refresh()
{
    if (isStarted && ! isShutDown && isThreadRunning())
        notify();
}

void AppRouting::openSystemRoutingSettings()
{
    if (router != nullptr)
        router->openSystemRoutingSettings();
}

void AppRouting::publishConfig()
{
    WorkerConfig c;
    c.method = getEffectiveMethod();
    c.active = isStarted && ! isShutDown && (liveUpdates || (! routes.empty() && c.method != Method::Disabled) || ! captures.empty());
    c.routes = routes;
    for (const auto& strip : host.getStripLayout())
    {
        const juce::String name (strip.name);
        c.stripNames.push_back (name);
        c.stripEndpoints.push_back (settings.getStripEndpointId (name));
    }

    const juce::ScopedLock sl (lock);
    config = std::move (c);
}

// =============================================================================
// Worker thread
// =============================================================================
void AppRouting::run()
{
    while (! threadShouldExit())
    {
        WorkerConfig c;
        {
            const juce::ScopedLock sl (lock);
            c = config;
        }

        // Also runs while endpoints we moved are outstanding, so that routes
        // are undone when the method changes or apps are un-mapped.
        if (c.active || ! routedEndpoints.empty())
        {
            const auto sessions = router->enumerateSessions();
            std::vector<AppState> states;
            std::set<uint32_t> seen;

            for (const auto& session : sessions)
            {
                if (threadShouldExit())
                    return;

                AppState a;
                a.processId = session.processId;
                a.executable = juce::String (session.executableName);
                a.displayName = session.displayName.empty() ? a.executable : juce::String (session.displayName);
                a.endpointId = juce::String (session.currentEndpointId);
                a.isActive = session.isActive;

                for (const auto& r : c.routes)
                {
                    if (! executablesMatch (r.executable, a.executable))
                        continue;
                    for (size_t i = 0; i < c.stripNames.size(); ++i)
                        if (c.stripNames[i].equalsIgnoreCase (r.stripName))
                            a.strip = static_cast<int> (i);
                    break;
                }

                // Several sessions can belong to one process: act once per process.
                const bool firstSessionOfProcess = seen.insert (a.processId).second;
                if (firstSessionOfProcess && c.method == Method::EndpointRouting)
                {
                    const auto existing = routedEndpoints.find (a.processId);
                    if (a.strip >= 0)
                    {
                        const auto& target = c.stripEndpoints[static_cast<size_t> (a.strip)];
                        if (existing == routedEndpoints.end() || existing->second != target)
                        {
                            std::string error;
                            if (router->setAppEndpoint (a.processId, target.toStdString(), error))
                                routedEndpoints[a.processId] = target;
                            else
                                a.error = juce::String (error);
                        }
                    }
                    else if (existing != routedEndpoints.end())
                    {
                        std::string error;
                        router->setAppEndpoint (a.processId, {}, error);
                        routedEndpoints.erase (existing);
                    }
                }
                a.routed = routedEndpoints.count (a.processId) > 0;
                states.push_back (std::move (a));
            }

            // Forget processes that exited; undo routes when routing is off.
            for (auto it = routedEndpoints.begin(); it != routedEndpoints.end();)
            {
                if (seen.count (it->first) == 0)
                {
                    it = routedEndpoints.erase (it);
                }
                else if (c.method != Method::EndpointRouting)
                {
                    std::string error;
                    router->setAppEndpoint (it->first, {}, error);
                    it = routedEndpoints.erase (it);
                }
                else
                {
                    ++it;
                }
            }

            {
                const juce::ScopedLock sl (lock);
                workerResult = std::move (states);
                resultPending = true;
            }
            triggerAsyncUpdate();
        }

        wait (kRefreshIntervalMs); // refresh() / config changes wake it early
    }
}

// =============================================================================
// Message thread
// =============================================================================
void AppRouting::handleAsyncUpdate()
{
    std::vector<AppState> next;
    {
        const juce::ScopedLock sl (lock);
        if (! resultPending)
            return;
        next = std::move (workerResult);
        resultPending = false;
    }

    applyCaptures (next);

    const bool changed = next.size() != apps.size() || ! std::equal (next.begin(), next.end(), apps.begin(), sameState);
    apps = std::move (next);
    publishConfig(); // the capture set may have changed "active"
    if (changed && onChanged != nullptr)
        onChanged();
}

void AppRouting::applyCaptures (std::vector<AppState>& states)
{
    const bool captureMethod = getEffectiveMethod() == Method::ProcessCapture && ! isShutDown;
    std::set<uint32_t> seen;

    for (auto& a : states)
    {
        if (! seen.insert (a.processId).second)
            continue;

        const auto existing = captures.find (a.processId);
        if (captureMethod && a.strip >= 0)
        {
            if (existing != captures.end())
            {
                host.setCaptureStrip (existing->second, a.strip);
            }
            else if (captureFailures[a.processId] < kMaxCaptureAttempts)
            {
                juce::String error;
                const int id = host.startProcessCapture (a.strip, a.processId, error);
                if (id >= 0)
                {
                    captures[a.processId] = id;
                }
                else
                {
                    ++captureFailures[a.processId];
                    a.error = error;
                }
            }
        }
        else if (existing != captures.end())
        {
            host.stopProcessCapture (existing->second);
            captures.erase (existing);
        }
    }

    // Processes that went away.
    for (auto it = captures.begin(); it != captures.end();)
    {
        if (seen.count (it->first) == 0)
        {
            host.stopProcessCapture (it->second);
            it = captures.erase (it);
        }
        else
        {
            ++it;
        }
    }
    for (auto it = captureFailures.begin(); it != captureFailures.end();)
        it = seen.count (it->first) == 0 ? captureFailures.erase (it) : std::next (it);

    for (auto& a : states)
    {
        const auto cap = captures.find (a.processId);
        a.captureId = cap != captures.end() ? cap->second : -1;
    }
}

void AppRouting::stopAllCaptures()
{
    for (const auto& [pid, id] : captures)
        host.stopProcessCapture (id);
    captures.clear();
}
} // namespace flub::app
