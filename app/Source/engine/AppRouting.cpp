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
constexpr const char* kRoutedAppsKey = "routing.routedApps"; // executables whose endpoint move is not undone yet

juce::String normaliseExecutable (const juce::String& name)
{
    auto n = name.trim().replaceCharacter ('\\', '/').fromLastOccurrenceOf ("/", false, false).toLowerCase();
    if (n.endsWith (".exe"))
        n = n.dropLastCharacters (4);
    return n;
}

bool sameRoutes (const std::vector<AppRoute>& x, const std::vector<AppRoute>& y)
{
    return std::equal (x.begin(), x.end(), y.begin(), y.end(),
                       [] (const AppRoute& a, const AppRoute& b) { return a.executable == b.executable && a.stripName == b.stripName; });
}

bool sameState (const AppRouting::AppState& x, const AppRouting::AppState& y)
{
    return x.processId == y.processId && x.strip == y.strip && x.isActive == y.isActive && x.routed == y.routed
           && x.captureId == y.captureId && x.error == y.error && x.endpointId == y.endpointId;
}
} // namespace

AppRouting::AppRouting (AudioEngineHost& h, AppSettings& s)
    : AppRouting (h, s, platform_bridge::createAppAudioRouter(), platform_bridge::isProcessCaptureSupported())
{
}

AppRouting::AppRouting (AudioEngineHost& h, AppSettings& s, std::unique_ptr<flub::platform::AppAudioRouter> r, bool canCapture)
    : juce::Thread ("Flubsound routing"), host (h), settings (s), router (std::move (r)), captureSupported (canCapture)
{
    method = settings.getRoutingMethod();
    routes = settings.getAppRoutes();

    if (auto xml = settings.getPropertiesFile().getXmlValue (kRoutedAppsKey))
        for (auto* e : xml->getChildWithTagNameIterator ("APP"))
            if (const auto exe = normaliseExecutable (e->getStringAttribute ("exe")); exe.isNotEmpty())
                routedExecutables.insert (exe);
    persistedRoutedExecutables = routedExecutables;
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
    // Apps that exited while routed stay in routedExecutables (persisted), so
    // their routes are undone when they next appear.
    if (router != nullptr)
    {
        std::set<juce::String> restored, failed;
        for (const auto& [pid, routed] : routedEndpoints)
        {
            std::string error;
            if (router->setAppEndpoint (pid, {}, error)) // back to the system default
                restored.insert (routed.executable);
            else
                failed.insert (routed.executable);
        }
        for (const auto& exe : restored)
            if (failed.count (exe) == 0)
                routedExecutables.erase (exe);
    }
    routedEndpoints.clear();
    persistRoutedExecutables (routedExecutables);
    stopAllCaptures();
}

// =============================================================================
bool AppRouting::canEnumerateApps() const noexcept
{
    return router != nullptr && router->canList();
}

bool AppRouting::isEndpointRoutingSupported() const noexcept
{
    // Listing alone is not enough: Windows lists everywhere, but moves only
    // with the opt-in adapter; without it every move would fail.
    return canEnumerateApps() && router->canMoveEndpoint();
}

AppRouting::Method AppRouting::getEffectiveMethod() const noexcept
{
    switch (method)
    {
        case Method::EndpointRouting: return isEndpointRoutingSupported() ? Method::EndpointRouting : Method::Disabled;
        case Method::ProcessCapture: return isCaptureSupported() ? Method::ProcessCapture : Method::Disabled;
        case Method::Disabled: return Method::Disabled;
        case Method::Automatic: break;
    }
    if (isEndpointRoutingSupported())
        return Method::EndpointRouting;
    if (isCaptureSupported())
        return Method::ProcessCapture;
    return Method::Disabled;
}

juce::String AppRouting::getUnavailableReason() const
{
    if (getEffectiveMethod() != Method::Disabled)
        return {};

    constexpr const char* instead = "Choose a Flubsound output device per app in the system sound settings instead.";
    if (method == Method::Disabled)
        return "Per-app routing is switched off (Settings > Processing).";
    if (router == nullptr && ! platform_bridge::servicesCompiledIn())
        return juce::String ("Per-app routing needs the Flubsound platform services, which are not part of this build. ") + instead;
    // The platform's own reason says what to do instead (e.g. macOS: pick the
    // device inside the app); the generic ones get the system settings hint.
    const juce::String platformReason (router != nullptr ? juce::String (router->cannotMoveReason()) : juce::String());
    if (! canEnumerateApps())
        return platformReason.isNotEmpty() ? platformReason
                                           : juce::String ("Running applications cannot be listed on this system, so per-app routing is unavailable. ") + instead;

    const auto moveReason = platformReason.isNotEmpty() ? platformReason
                                                        : juce::String ("Moving applications to another output device is not supported on this system. ") + instead;
#if JUCE_WINDOWS
    const juce::String captureReason ("Per-app capture needs Windows 10 version 2004 (build 19041) or later, or Windows 11.");
#else
    const juce::String captureReason ("Per-app capture is not available on this system.");
#endif

    switch (method)
    {
        case Method::EndpointRouting:
            return moveReason + (isCaptureSupported() ? " Choose Automatic or Process capture in Settings > Processing." : "");
        case Method::ProcessCapture:
            return captureReason + (isEndpointRoutingSupported() ? juce::String (" Choose Automatic or Endpoint routing in Settings > Processing.")
                                                                 : " " + juce::String (instead));
        case Method::Automatic:
        case Method::Disabled: break;
    }
    return captureReason + " " + moveReason; // Automatic: neither method works
}

int AppRouting::getProcessedAppCount() const
{
    std::set<uint32_t> processed;
    for (const auto& a : apps)
        if (a.routed || a.captureId >= 0)
            processed.insert (a.processId);
    return static_cast<int> (processed.size());
}

void AppRouting::setMethod (Method newMethod)
{
    if (newMethod == method)
        return;
    method = newMethod;
    settings.setRoutingMethod (method);
    captureFailures.clear(); // a deliberate change retries given-up captures
    publishConfig();         // the worker tears down what the previous method set up
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
    captureFailures.clear(); // a deliberate change retries given-up captures
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
    // AudioEngineHost::setStripLayout() has already released the slots of
    // captures whose strip disappeared, and surviving strips may have a new
    // channel count: drop every capture so no stale id is ever stopped later
    // (its slot may be reused by another capture) and let the next pass
    // restart them with FIFOs sized for the new layout.
    stopAllCaptures();
    captureFailures.clear();
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

    // `config` is only written here, on the message thread: reading it
    // without the lock is safe. A pass computed from an older generation is
    // discarded (handleAsyncUpdate), so make sure a fresh one follows.
    const bool changed = c.method != config.method || ! sameRoutes (c.routes, config.routes) || c.stripNames != config.stripNames
                         || c.stripEndpoints != config.stripEndpoints;
    if (changed)
        ++configGeneration;
    c.generation = configGeneration;

    {
        const juce::ScopedLock sl (lock);
        config = std::move (c);
    }
    if (changed)
        refresh();
}

void AppRouting::persistRoutedExecutables (const std::set<juce::String>& executables)
{
    if (executables == persistedRoutedExecutables)
        return;
    persistedRoutedExecutables = executables;

    auto& properties = settings.getPropertiesFile();
    if (executables.empty())
    {
        properties.removeValue (kRoutedAppsKey);
        return;
    }
    juce::XmlElement xml ("ROUTEDAPPS");
    for (const auto& exe : executables)
        xml.createNewChildElement ("APP")->setAttribute ("exe", exe);
    properties.setValue (kRoutedAppsKey, &xml);
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

        // Also runs while endpoints we moved are outstanding (or apps that
        // exited while routed may come back), so that routes are undone when
        // the method changes or apps are un-mapped.
        if (c.active || ! routedEndpoints.empty() || ! routedExecutables.empty())
        {
            const auto sessions = router->enumerateSessions();
            std::vector<AppState> states;
            std::set<uint32_t> seen;
            std::set<juce::String> restored; // executables moved back to the default in this pass

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
                if (seen.insert (a.processId).second)
                {
                    const auto executable = normaliseExecutable (a.executable);
                    auto existing = routedEndpoints.find (a.processId);
                    if (existing != routedEndpoints.end() && existing->second.executable != executable)
                    {
                        // The pid now belongs to another program (the entry
                        // survived an empty enumeration): that move was not
                        // this process's. Its executable stays remembered.
                        routedEndpoints.erase (existing);
                        existing = routedEndpoints.end();
                    }
                    if (c.method == Method::EndpointRouting && a.strip >= 0)
                    {
                        const auto& target = c.stripEndpoints[static_cast<size_t> (a.strip)];
                        if (existing == routedEndpoints.end() || existing->second.endpoint != target)
                        {
                            std::string error;
                            if (router->setAppEndpoint (a.processId, target.toStdString(), error))
                            {
                                routedEndpoints[a.processId] = { target, executable };
                                routedExecutables.insert (executable);
                            }
                            else
                            {
                                a.error = juce::String (error);
                            }
                        }
                    }
                    else if (existing != routedEndpoints.end())
                    {
                        // Un-mapped, or routing switched off / to capture.
                        std::string error;
                        if (router->setAppEndpoint (a.processId, {}, error))
                            restored.insert (existing->second.executable);
                        routedEndpoints.erase (existing);
                    }
                    else if (routedExecutables.count (executable) != 0 && restoreTried.insert (a.processId).second)
                    {
                        // A new process of an app we routed that exited before
                        // its route was undone (in this run or an earlier one):
                        // the OS kept the per-app endpoint, so move it back.
                        std::string error;
                        if (router->setAppEndpoint (a.processId, {}, error))
                            restored.insert (executable);
                    }
                }
                a.routed = routedEndpoints.count (a.processId) > 0;
                states.push_back (std::move (a));
            }

            // Forget processes that exited; their executables stay in
            // routedExecutables until a later process of theirs is moved back.
            // An empty list is far more likely a failed enumeration (COM,
            // pactl) than every routed app gone at once: keep them then.
            if (! sessions.empty())
                for (auto it = routedEndpoints.begin(); it != routedEndpoints.end();)
                    it = seen.count (it->first) == 0 ? routedEndpoints.erase (it) : std::next (it);
            for (auto it = restoreTried.begin(); it != restoreTried.end();)
                it = seen.count (*it) == 0 ? restoreTried.erase (it) : std::next (it);

            // One move back resets the app's remembered endpoint (the OS keeps
            // it per application); done unless a process of it is still routed.
            for (const auto& exe : restored)
                if (std::none_of (routedEndpoints.begin(), routedEndpoints.end(), [&exe] (const auto& e) { return e.second.executable == exe; }))
                    routedExecutables.erase (exe);

            {
                const juce::ScopedLock sl (lock);
                workerResult = std::move (states);
                resultRoutedExecutables = routedExecutables;
                resultGeneration = c.generation;
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
    std::set<juce::String> routedApps;
    uint64_t generation = 0;
    {
        const juce::ScopedLock sl (lock);
        if (! resultPending)
            return;
        next = std::move (workerResult);
        routedApps = std::move (resultRoutedExecutables);
        generation = resultGeneration;
        resultPending = false;
    }

    // The worker's endpoint moves happened whatever the configuration is now.
    persistRoutedExecutables (routedApps);

    // Strip indices from an outdated route set / strip layout: applying them
    // would start or re-point captures into the wrong strip. The change that
    // bumped the generation already woke the worker for a fresh pass.
    if (generation != configGeneration)
        return;

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
    bool stoppedAny = false;

    for (auto& a : states)
    {
        if (! seen.insert (a.processId).second)
            continue;

        auto existing = captures.find (a.processId);
        if (captureMethod && a.strip >= 0)
        {
            if (existing != captures.end() && existing->second.strip != a.strip)
            {
                const auto layout = host.getStripLayout();
                const auto channelsOf = [&layout] (int strip)
                { return strip >= 0 && strip < static_cast<int> (layout.size()) ? layout[static_cast<size_t> (strip)].inputChannels : 0; };

                if (channelsOf (existing->second.strip) == channelsOf (a.strip))
                {
                    // Same width: just re-point the running capture.
                    host.setCaptureStrip (existing->second.id, a.strip);
                    existing->second.strip = a.strip;
                }
                else
                {
                    // The FIFO is sized (and down-mixed) for the old strip's
                    // channel count: restart the capture for the new strip.
                    host.stopProcessCapture (existing->second.id);
                    captures.erase (existing);
                    existing = captures.end();
                    stoppedAny = true;
                }
            }

            if (existing == captures.end())
            {
                auto failure = captureFailures.find (a.processId);
                if (failure != captureFailures.end() && failure->second.strip != a.strip)
                {
                    captureFailures.erase (failure); // re-mapped: a fresh set of attempts
                    failure = captureFailures.end();
                }

                if (failure == captureFailures.end() || failure->second.attempts < kMaxCaptureAttempts)
                {
                    juce::String error;
                    const int id = host.startProcessCapture (a.strip, a.processId, error);
                    if (id >= 0)
                    {
                        captures[a.processId] = { id, a.strip };
                        if (failure != captureFailures.end())
                            captureFailures.erase (failure);
                    }
                    else
                    {
                        auto& f = captureFailures[a.processId];
                        ++f.attempts;
                        f.error = error;
                        f.strip = a.strip;
                        a.error = error;
                    }
                }
                else
                {
                    // Gave up: keep showing why the app is mapped but silent.
                    a.error = failure->second.error;
                }
            }
        }
        else
        {
            if (existing != captures.end())
            {
                host.stopProcessCapture (existing->second.id);
                captures.erase (existing);
                stoppedAny = true;
            }
            captureFailures.erase (a.processId); // a later mapping starts afresh
        }
    }

    // Processes that went away.
    for (auto it = captures.begin(); it != captures.end();)
    {
        if (seen.count (it->first) == 0)
        {
            host.stopProcessCapture (it->second.id);
            it = captures.erase (it);
            stoppedAny = true;
        }
        else
        {
            ++it;
        }
    }
    for (auto it = captureFailures.begin(); it != captureFailures.end();)
        it = seen.count (it->first) == 0 ? captureFailures.erase (it) : std::next (it);

    // A stopped capture frees a slot ("Too many applications are being
    // captured") by the next pass: give the apps that were given up another
    // round of attempts.
    if (stoppedAny)
        for (auto& entry : captureFailures)
            entry.second.attempts = 0;

    for (auto& a : states)
    {
        const auto cap = captures.find (a.processId);
        a.captureId = cap != captures.end() ? cap->second.id : -1;
    }
}

void AppRouting::stopAllCaptures()
{
    for (const auto& entry : captures)
        host.stopProcessCapture (entry.second.id);
    captures.clear();
}
} // namespace flub::app
