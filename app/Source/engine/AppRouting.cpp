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
} // namespace

AppRouting::AppRouting (AudioEngineHost& h, AppSettings& s)
    : host (h), settings (s)
{
    router = platform_bridge::createAppAudioRouter();
    captureSupported = platform_bridge::isProcessCaptureSupported();
    method = settings.getRoutingMethod();
    routes = settings.getAppRoutes();
    updateTimer();
}

AppRouting::~AppRouting()
{
    shutdown();
}

void AppRouting::shutdown()
{
    if (isShutDown)
        return;
    isShutDown = true;
    stopTimer();
    restoreAllEndpoints();
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
    refresh(); // tears down whatever the previous method set up
    updateTimer();
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

    persistRoutes();
    refresh();
    updateTimer();
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

void AppRouting::persistRoutes()
{
    settings.setAppRoutes (routes);
}

int AppRouting::stripIndexForName (const juce::String& stripName) const
{
    const auto layout = host.getStripLayout();
    for (size_t i = 0; i < layout.size(); ++i)
        if (juce::String (layout[i].name).equalsIgnoreCase (stripName))
            return static_cast<int> (i);
    return -1;
}

void AppRouting::setLiveUpdates (bool shouldUpdate)
{
    liveUpdates = shouldUpdate;
    updateTimer();
    if (shouldUpdate)
        refresh();
}

void AppRouting::updateTimer()
{
    const bool wanted = ! isShutDown && router != nullptr && (liveUpdates || (! routes.empty() && getEffectiveMethod() != Method::Disabled));
    if (wanted && ! isTimerRunning())
        startTimer (kRefreshIntervalMs);
    else if (! wanted && isTimerRunning())
        stopTimer();
}

void AppRouting::timerCallback()
{
    refresh();
}

void AppRouting::openSystemRoutingSettings()
{
    if (router != nullptr)
        router->openSystemRoutingSettings();
}

// =============================================================================
void AppRouting::refresh()
{
    if (isShutDown || router == nullptr)
        return;

    const auto sessions = router->enumerateSessions();
    const auto effective = getEffectiveMethod();
    const auto layout = host.getStripLayout();

    std::vector<AppState> next;
    std::set<uint32_t> seen;

    for (const auto& session : sessions)
    {
        AppState a;
        a.processId = session.processId;
        a.executable = juce::String (session.executableName);
        a.displayName = session.displayName.empty() ? a.executable : juce::String (session.displayName);
        a.endpointId = juce::String (session.currentEndpointId);
        a.isActive = session.isActive;

        const auto stripName = getStripNameForExecutable (a.executable);
        a.strip = stripName.isNotEmpty() ? stripIndexForName (stripName) : -1;

        // One process may own several sessions: act once per process.
        const bool firstSessionOfProcess = seen.insert (a.processId).second;

        if (firstSessionOfProcess && effective == Method::EndpointRouting)
        {
            const auto existing = routedEndpoints.find (a.processId);
            if (a.strip >= 0)
            {
                const auto target = settings.getStripEndpointId (juce::String (layout[static_cast<size_t> (a.strip)].name));
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
                router->setAppEndpoint (a.processId, {}, error); // back to the system default
                routedEndpoints.erase (existing);
            }
        }

        if (firstSessionOfProcess && effective == Method::ProcessCapture)
        {
            const auto existing = captures.find (a.processId);
            if (a.strip >= 0)
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
                        captures[a.processId] = id;
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

        a.routed = routedEndpoints.count (a.processId) > 0;
        const auto cap = captures.find (a.processId);
        a.captureId = cap != captures.end() ? cap->second : -1;
        next.push_back (std::move (a));
    }

    // Processes that went away, or state left over from another method.
    for (auto it = captures.begin(); it != captures.end();)
    {
        if (seen.count (it->first) == 0 || effective != Method::ProcessCapture)
        {
            host.stopProcessCapture (it->second);
            it = captures.erase (it);
        }
        else
            ++it;
    }
    for (auto it = routedEndpoints.begin(); it != routedEndpoints.end();)
    {
        if (seen.count (it->first) == 0)
            it = routedEndpoints.erase (it); // process exited: nothing to restore
        else if (effective != Method::EndpointRouting)
        {
            std::string error;
            router->setAppEndpoint (it->first, {}, error);
            it = routedEndpoints.erase (it);
        }
        else
            ++it;
    }
    for (auto it = captureFailures.begin(); it != captureFailures.end();)
        it = seen.count (it->first) == 0 ? captureFailures.erase (it) : std::next (it);

    const bool changed = next.size() != apps.size()
                         || ! std::equal (next.begin(), next.end(), apps.begin(),
                                          [] (const AppState& x, const AppState& y)
                                          {
                                              return x.processId == y.processId && x.strip == y.strip && x.isActive == y.isActive
                                                     && x.routed == y.routed && x.captureId == y.captureId && x.error == y.error;
                                          });
    apps = std::move (next);
    if (changed && onChanged != nullptr)
        onChanged();
}

void AppRouting::restoreAllEndpoints()
{
    if (router != nullptr)
    {
        for (const auto& [pid, endpoint] : routedEndpoints)
        {
            std::string error;
            router->setAppEndpoint (pid, {}, error);
        }
    }
    routedEndpoints.clear();
}

void AppRouting::stopAllCaptures()
{
    for (const auto& [pid, id] : captures)
        host.stopProcessCapture (id);
    captures.clear();
}
} // namespace flub::app
