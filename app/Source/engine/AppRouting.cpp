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
constexpr int kVerifyMoveDelayMs = 250; // R4.5: the pass after a move looks again this soon (did the app follow?)
constexpr int kMaxCaptureAttempts = 3;
constexpr int kMaxMoveAttempts = 3;     // R4.5: failed moves of an app's own output, until the configuration changes
constexpr const char* kRoutedAppsKey = "routing.routedApps"; // executables whose endpoint move is not undone yet
constexpr const char* kJournalFileName = "route-journal.json"; // docs/11 E47, next to the settings file

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
           && x.captureId == y.captureId && x.error == y.error && x.endpointId == y.endpointId && x.playsToOutput == y.playsToOutput
           && x.doublingBlocked == y.doublingBlocked && x.moveAway == y.moveAway && x.movedTo == y.movedTo && x.moveNote == y.moveNote
           && x.movePending == y.movePending;
}

bool sameEndpoint (const juce::String& a, const juce::String& b)
{
    return a.equalsIgnoreCase (b); // Windows writes the ids' GUIDs in either case
}

/** The text inside the last "( ... )" of a device name: "CABLE Output
    (VB-Audio Virtual Cable)" -> "vb-audio virtual cable" (lower case). */
juce::String devicePart (const juce::String& name)
{
    const auto open = name.lastIndexOfChar ('(');
    const auto close = name.lastIndexOfChar (')');
    return open >= 0 && close > open ? name.substring (open + 1, close).trim().toLowerCase() : juce::String();
}

juce::String nameOf (const AppRouting::OutputEndpoint& e)
{
    return e.name.empty() ? juce::String (e.id) : juce::String (e.name);
}

juce::File journalFileFor (AppSettings& settings)
{
    const auto settingsFile = settings.getPropertiesFile().getFile();
    return settingsFile == juce::File() ? juce::File() : settingsFile.getSiblingFile (kJournalFileName);
}
} // namespace

// =============================================================================
// RouteJournal
// =============================================================================
std::vector<RouteJournal::Entry> RouteJournal::load() const
{
    std::vector<Entry> entries;
    if (file == juce::File() || ! file.existsAsFile())
        return entries;

    const auto json = juce::JSON::parse (file.loadFileAsString());
    const auto* list = json.getProperty ("entries", {}).getArray();
    if (! json.isObject() || list == nullptr)
    {
        // Unreadable: keep it for inspection instead of acting on half of it.
        file.moveFileTo (file.withFileExtension ("json.bad"));
        return entries;
    }

    for (const auto& item : *list)
    {
        Entry e;
        e.processId = static_cast<uint32_t> (static_cast<juce::int64> (item.getProperty ("pid", 0)));
        e.processStartTime = static_cast<uint64_t> (item.getProperty ("startTime", "0").toString().getLargeIntValue());
        e.executable = item.getProperty ("exe", {}).toString();
        e.executablePath = item.getProperty ("path", {}).toString();
        e.endpoint = item.getProperty ("endpoint", {}).toString();
        e.previousEndpoint = item.getProperty ("previousEndpoint", {}).toString();
        e.pending = static_cast<bool> (item.getProperty ("pending", false));
        e.movedAway = item.getProperty ("kind", {}).toString() == "away";
        if (e.executable.isNotEmpty())
            entries.push_back (std::move (e));
    }
    return entries;
}

bool RouteJournal::write (const std::vector<Entry>& entries)
{
    if (hasWritten && entries == written)
        return true;
    if (file == juce::File())
    {
        written = entries;
        hasWritten = true;
        return true;
    }

    bool ok = true;
    if (entries.empty())
    {
        ok = ! file.exists() || file.deleteFile();
    }
    else
    {
        juce::Array<juce::var> list;
        for (const auto& e : entries)
        {
            auto* item = new juce::DynamicObject();
            item->setProperty ("pid", static_cast<juce::int64> (e.processId));
            // A FILETIME does not fit a JSON number (double) exactly: text.
            item->setProperty ("startTime", juce::String (static_cast<juce::int64> (e.processStartTime)));
            item->setProperty ("exe", e.executable);
            item->setProperty ("path", e.executablePath);
            item->setProperty ("endpoint", e.endpoint);
            item->setProperty ("previousEndpoint", e.previousEndpoint);
            item->setProperty ("pending", e.pending);
            // R4.5: an app's own output moved away (put back to previousEndpoint);
            // absent = a move to a strip endpoint (put back to the default).
            if (e.movedAway)
                item->setProperty ("kind", "away");
            list.add (juce::var (item));
        }
        auto* root = new juce::DynamicObject();
        root->setProperty ("version", kVersion);
        root->setProperty ("entries", list);
        // Written to a temporary file next to it, then renamed over the old
        // one (rename / ReplaceFile): never a torn journal. One attempt; a
        // failed write is retried by the next pass (the move waits for it).
        const auto temp = file.getSiblingFile ("." + file.getFileName() + ".tmp");
        ok = file.getParentDirectory().createDirectory().wasOk();
        if (ok)
        {
            juce::FileOutputStream out (temp);
            ok = out.openedOk() && out.setPosition (0) && out.truncate().wasOk() && out.writeText (juce::JSON::toString (juce::var (root)), false, false, "\n");
            out.flush();
            ok = ok && out.getStatus().wasOk();
        }
        ok = ok && temp.replaceFileIn (file);
        if (! ok)
            temp.deleteFile();
    }
    if (ok)
    {
        written = entries;
        hasWritten = true;
    }
    return ok;
}

AppRouting::AppRouting (AudioEngineHost& h, AppSettings& s)
    : AppRouting (h, s, platform_bridge::createAppAudioRouter(), platform_bridge::isProcessCaptureSupported())
{
}

AppRouting::AppRouting (AudioEngineHost& h, AppSettings& s, std::unique_ptr<flub::platform::AppAudioRouter> r, bool canCapture)
    : juce::Thread ("Flubsound routing"), host (h), settings (s), router (std::move (r)), captureSupported (canCapture),
      journal (journalFileFor (s))
{
    method = settings.getRoutingMethod();
    routes = settings.getAppRoutes();
    moveAway = settings.getMoveOriginalAway();
    silentChoice = { settings.getSilentEndpointId().toStdString(), settings.getSilentEndpointName().toStdString() };

    if (auto xml = settings.getPropertiesFile().getXmlValue (kRoutedAppsKey))
        for (auto* e : xml->getChildWithTagNameIterator ("APP"))
            if (const auto exe = normaliseExecutable (e->getStringAttribute ("exe")); exe.isNotEmpty())
                routedExecutables.insert (exe);

    // Moves an earlier run made and did not undo (it crashed or was killed):
    // undone by the worker like those of apps that exited while routed.
    for (const auto& e : journal.load())
    {
        const auto exe = normaliseExecutable (e.executable);
        if (e.movedAway)
        {
            // R4.5: kept as this run's move (pending or not: putting the
            // earlier device back is right either way); the worker puts it
            // back when the app is seen unassigned or the option is off, and
            // keeps it while the app is still captured.
            silentMoves[exe] = { e.endpoint, e.previousEndpoint, e.processId, e.processStartTime, e.executablePath, e.pending };
            continue;
        }
        routedExecutables.insert (exe);
        if (e.processId != 0)
        {
            auto entry = e;
            entry.executable = exe;
            recoveredMoves[e.processId] = std::move (entry);
        }
    }
    persistedRoutedExecutables = routedExecutables;
}

AppRouting::~AppRouting()
{
    shutdown();
}

void AppRouting::setRouter (std::unique_ptr<flub::platform::AppAudioRouter> newRouter, bool canCapture)
{
    if (isStarted || isShutDown)
        return;
    router = std::move (newRouter);
    captureSupported = canCapture;
    publishConfig();
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

        // R4.5: the apps' own outputs go back to their earlier devices. One
        // last enumeration names the processes to do it through: a process
        // id from the last pass may belong to another program by now. Apps
        // that are not running keep their journal entries (the next start
        // puts them back when they are seen).
        // In Tournament mode (docs/11 E55) no process is opened: the journal
        // keeps them for the next start.
        if (! silentMoves.empty() && ! tournament)
            restoreSilentMoves (router->enumerateSessions());
    }
    routedEndpoints.clear();
    recoveredMoves.clear();
    syncJournal (nullptr);
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
        return "Per-app routing is switched off (Settings > Routing).";
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
            return moveReason + (isCaptureSupported() ? " Choose Automatic or Process capture in Settings > Routing." : "");
        case Method::ProcessCapture:
            return captureReason + (isEndpointRoutingSupported() ? juce::String (" Choose Automatic or Endpoint routing in Settings > Routing.")
                                                                 : " " + juce::String (instead));
        case Method::Automatic:
        case Method::Disabled: break;
    }
    return captureReason + " " + moveReason; // Automatic: neither method works
}

bool AppRouting::playsToEndpoint (const flub::platform::AudioSessionInfo& session, const std::string& endpointId)
{
    if (endpointId.empty())
        return false;
    if (! session.activeEndpointIds.empty())
        return std::find (session.activeEndpointIds.begin(), session.activeEndpointIds.end(), endpointId) != session.activeEndpointIds.end();
    return session.currentEndpointId == endpointId;
}

// =============================================================================
// Moving the original away (docs/11 E47, R4.5)
// =============================================================================
int AppRouting::silentPreference (const juce::String& endpointName)
{
    const auto name = endpointName.toLowerCase();
    for (const auto* p : { "s/pdif", "spdif", "digital audio", "digital output", "optical", "toslink" })
        if (name.contains (p))
            return 2;
    for (const auto* p : { "hdmi", "displayport", "display audio", "nvidia high definition audio", "amd high definition audio" })
        if (name.contains (p))
            return 1;
    return 0;
}

bool AppRouting::looksLikeVirtualCable (const juce::String& endpointName)
{
    const auto name = endpointName.toLowerCase();
    for (const auto* p : { "flubsound", "cable", "vb-audio", "voicemeeter", "virtual", "blackhole", "soundflower", "loopback" })
        if (name.contains (p))
            return true;
    return false;
}

bool AppRouting::feedsInput (const juce::String& endpointName, const juce::String& inputDevice)
{
    const auto part = devicePart (endpointName);
    return looksLikeVirtualCable (endpointName) && part.isNotEmpty() && part == devicePart (inputDevice);
}

AppRouting::SilentTarget AppRouting::chooseSilentEndpoint (const std::vector<OutputEndpoint>& endpoints, const std::string& outputId,
                                                           const juce::String& inputDevice, const OutputEndpoint& chosen,
                                                           const std::set<std::string>& busy, const std::string& current)
{
    const auto isDefault = [&endpoints] (const OutputEndpoint& e) { return ! endpoints.empty() && e.id == endpoints.front().id; };
    // Why an endpoint can never be the silent device (empty = it can).
    const auto refusal = [&] (const OutputEndpoint& e) -> juce::String
    {
        const auto name = nameOf (e);
        if (! outputId.empty() && sameEndpoint (juce::String (e.id), juce::String (outputId)))
            return name + " is the device Flubsound plays to.";
        if (isDefault (e))
            return name + " is the system's default output: apps that follow the default would play there too.";
        if (name.containsIgnoreCase ("Flubsound"))
            return name + " is one of Flubsound's own devices.";
        if (feedsInput (name, inputDevice))
            return name + " feeds Flubsound's input (" + inputDevice + "): the app would be heard twice.";
        return {};
    };

    SilentTarget result;
    if (! chosen.id.empty())
    {
        const auto chosenName = nameOf (chosen);
        for (const auto& e : endpoints)
            if (sameEndpoint (juce::String (e.id), juce::String (chosen.id)))
            {
                if (const auto why = refusal (e); why.isNotEmpty())
                    result.reason = why + " Choose another silent device in Settings > Routing.";
                else
                    result.endpoint = e;
                return result;
            }
        result.reason = chosenName + " (the silent device chosen in Settings > Routing) is not connected. Flubsound moves apps there again "
                        "when it is back; or choose another device.";
        return result;
    }

    // Automatic: stay where the moves already are, then digital, then display
    // outputs. A display output another app plays to may be a monitor with
    // speakers someone listens to; a digital output with apps on it is
    // usually where apps were parked (an S/PDIF jack with nothing plugged in).
    const auto candidate = [&] (const OutputEndpoint& e, int minimum)
    {
        const auto name = nameOf (e);
        const int preference = silentPreference (name);
        return refusal (e).isEmpty() && ! looksLikeVirtualCable (name) && preference >= minimum && (preference >= 2 || busy.count (e.id) == 0);
    };
    if (! current.empty())
        for (const auto& e : endpoints)
            if (sameEndpoint (juce::String (e.id), juce::String (current)) && candidate (e, 1))
                return { e, {} };
    for (const int preference : { 2, 1 })
        for (const auto& e : endpoints)
            if (candidate (e, preference) && silentPreference (nameOf (e)) == preference)
                return { e, {} };

    result.reason = "No output device that is likely silent was found (an S/PDIF or other digital output, or an HDMI / DisplayPort output "
                    "nothing else plays to). Choose a device you do not listen to in Settings > Routing, or set the app's output "
                    "yourself in Windows' Volume mixer.";
    return result;
}

bool AppRouting::canMoveOriginalAway() const noexcept
{
    return router != nullptr && router->canMoveAppOutput() && isCaptureSupported();
}

void AppRouting::setMoveOriginalAway (bool shouldMove)
{
    if (shouldMove == moveAway)
        return;
    moveAway = shouldMove;
    settings.setMoveOriginalAway (shouldMove);
    publishConfig();
    refresh();
}

void AppRouting::moveOriginalsAwayNow()
{
    ++moveRetry; // the worker forgets which apps the user took back, and the failed attempts
    if (! moveAway)
    {
        moveAway = true;
        settings.setMoveOriginalAway (true);
    }
    publishConfig();
    refresh();
}

AppRouting::OutputEndpoint AppRouting::getSilentEndpointChoice() const
{
    return silentChoice;
}

void AppRouting::setSilentEndpointChoice (const OutputEndpoint& endpoint)
{
    if (endpoint == silentChoice)
        return;
    silentChoice = endpoint.id.empty() ? OutputEndpoint() : endpoint;
    settings.setSilentEndpoint (juce::String (silentChoice.id), juce::String (silentChoice.name));
    publishConfig();
    refresh();
}

juce::String AppRouting::getMoveAwayAction() const
{
    if (! canMoveOriginalAway() || getEffectiveMethod() != Method::ProcessCapture || silentTarget.id.empty())
        return {};
    const auto target = nameOf (silentTarget);
    if (! moveAway)
        return "Move automatically to " + target;
    for (const auto& a : apps)
        if (a.doublingBlocked && (a.moveAway == MoveAway::LeftToUser || a.moveAway == MoveAway::Failed))
            return "Move again to " + target;
    return {};
}

juce::String AppRouting::describeMoveAway() const
{
    if (! canMoveOriginalAway())
    {
       #if JUCE_WINDOWS
        if (router != nullptr && ! router->canMoveAppOutput())
            return "Needs Windows 10 version 1803 or later.";
        return "Needs per-app capture (Windows 10 version 2004 or later).";
       #else
        return "Windows only: on this system apps are routed to the strips' own devices instead.";
       #endif
    }
    if (getEffectiveMethod() != Method::ProcessCapture)
        return "Acts with process capture only (Per-app routing above).";

    const juce::String output (outputEndpoint.name.empty() ? juce::String ("the device Flubsound plays to") : juce::String (outputEndpoint.name));
    const auto target = silentTarget.id.empty() ? juce::String() : nameOf (silentTarget);
    if (! moveAway)
        return "Off (default). A captured app that plays to " + output + " is held back (it would be heard twice). On: Flubsound moves its own sound to "
               + (target.isNotEmpty() ? target : juce::String ("a silent device")) + " and captures it; the app's own device comes back when you "
               + "unassign it, switch this off or quit Flubsound.";

    int moved = 0;
    juce::StringArray notes;
    std::set<juce::String> seenExecutables;
    for (const auto& a : apps)
    {
        if (! seenExecutables.insert (normaliseExecutable (a.executable)).second)
            continue;
        moved += a.moveAway == MoveAway::Moved ? 1 : 0;
        if (a.moveNote.isNotEmpty())
            notes.addIfNotAlreadyThere (a.moveNote);
    }
    juce::String text ("On. ");
    if (target.isEmpty())
        text << "Paused: " << silentTargetReason;
    else if (moved > 0)
        text << moved << (moved == 1 ? " app plays its own sound to " : " apps play their own sound to ") << target
             << " while Flubsound captures " << (moved == 1 ? "it." : "them.");
    else
        text << "Apps that play to " << output << " are moved to " << target << " when Flubsound captures them.";
    if (! notes.isEmpty())
        text << " " << notes.joinIntoString (" ");
    return text;
}

void AppRouting::restoreSilentMoves (const std::vector<flub::platform::AudioSessionInfo>& sessions)
{
    for (auto it = silentMoves.begin(); it != silentMoves.end();)
    {
        const auto& exe = it->first;
        const auto& m = it->second;
        // The recorded process when it still runs (same start time), else any process of the executable.
        uint32_t pid = 0;
        for (const auto& s : sessions)
        {
            if (normaliseExecutable (juce::String (s.executableName)) != exe)
                continue;
            const bool recorded = s.processId == m.processId
                                  && (m.processStartTime == 0 || s.processStartTime == 0 || m.processStartTime == s.processStartTime);
            if (pid == 0 || recorded)
                pid = s.processId;
            if (recorded)
                break;
        }
        std::string error;
        bool done = false;
        if (pid != 0)
        {
            done = router->setAppEndpoint (pid, m.previous.toStdString(), error);
            if (! done && m.previous.isNotEmpty()) // its own earlier device is gone: the system default then
                done = router->setAppEndpoint (pid, {}, error);
        }
        it = done ? silentMoves.erase (it) : std::next (it);
    }
}

juce::StringArray AppRouting::getDoublingBlockedApps() const
{
    juce::StringArray names;
    for (const auto& a : apps)
        if (a.doublingBlocked)
            names.addIfNotAlreadyThere (a.displayName.isNotEmpty() ? a.displayName : a.executable);
    return names;
}

juce::String AppRouting::describeDoubling() const
{
    const auto blocked = getDoublingBlockedApps();
    if (blocked.isEmpty())
        return {};

    const auto list = blocked.size() == 1 ? blocked[0]
                                          : blocked.joinIntoString (", ", 0, blocked.size() - 1) + " and " + blocked[blocked.size() - 1];
    const juce::String device (outputEndpoint.name.empty() ? juce::String ("the output device") : juce::String (outputEndpoint.name));
    const juce::String them (blocked.size() == 1 ? "it" : "them");
    auto text = list + (blocked.size() == 1 ? " plays" : " play") + " straight to " + device
                + ", the device Flubsound plays to, so the original is heard as it is. Flubsound does not capture " + them
                + " as well: the original and the processed copy would play together, a few milliseconds apart. Fix: set the app's output to "
                + "a device you do not listen to (Windows: Settings > System > Sound > Volume mixer, or App volume and device preferences). "
                + "The capture still reaches it there.";
    if (spareEndpoints.empty())
    {
        text += " No other output device is active: a virtual cable (e.g. VB-CABLE) can serve as one.";
    }
    else
    {
        juce::StringArray spare;
        for (const auto& e : spareEndpoints)
            spare.add (juce::String (e.name));
        text += " Devices available: " + spare.joinIntoString (", ") + ".";
    }

    // R4.5: Flubsound can make the move itself, or says why it did not.
    if (! canMoveOriginalAway())
        return text;
    if (! moveAway)
        return text
               + (silentTarget.id.empty() ? " Flubsound can also do this for you (Settings > Routing), but " + silentTargetReason
                                          : " Or let Flubsound do it: \"" + getMoveAwayAction() + "\" turns on \"Move the app's own sound away "
                                                + "automatically\" (Settings > Routing), and the app's own device comes back when you unassign it, "
                                                + "switch that off or quit Flubsound.");
    juce::StringArray notes;
    for (const auto& a : apps)
        if (a.doublingBlocked && a.moveNote.isNotEmpty())
            notes.addIfNotAlreadyThere (a.moveNote);
    return notes.isEmpty() ? text : text + " " + notes.joinIntoString (" ");
}

void AppRouting::setOutputDeviceSource (std::function<juce::String()> source)
{
    outputDeviceSource = std::move (source);
    publishConfig();
}

juce::String AppRouting::currentOutputDevice() const
{
    if (outputDeviceSource != nullptr)
        return outputDeviceSource();
    auto& manager = host.getDeviceManager();
    auto* device = manager.getCurrentAudioDevice();
    if (device == nullptr)
        return {};
    const auto name = manager.getAudioDeviceSetup().outputDeviceName;
    return name.isNotEmpty() ? name : device->getName();
}

void AppRouting::setInputDeviceSource (std::function<juce::String()> source)
{
    inputDeviceSource = std::move (source);
    publishConfig();
}

juce::String AppRouting::currentInputDevice() const
{
    if (inputDeviceSource != nullptr)
        return inputDeviceSource();
    auto& manager = host.getDeviceManager();
    return manager.getCurrentAudioDevice() != nullptr ? manager.getAudioDeviceSetup().inputDeviceName : juce::String();
}

int AppRouting::getProcessedAppCount() const
{
    std::set<uint32_t> processed;
    for (const auto& a : apps)
        if (a.routed || a.captureId >= 0 || a.movePending) // (R4.5: captured at the next pass, 250 ms later)
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

void AppRouting::setTournamentMode (bool shouldFreeze)
{
    if (tournament == shouldFreeze)
        return;
    tournament = shouldFreeze;
    publishConfig(); // a new generation: a pass that was running when it froze is discarded
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

juce::String AppRouting::getInputLinkStatus() const
{
    const juce::ScopedLock sl (lock);
    return inputLinkStatus;
}

bool AppRouting::areInputsLinked() const
{
    const juce::ScopedLock sl (lock);
    return inputsLinked;
}

void AppRouting::publishConfig()
{
    WorkerConfig c;
    c.method = getEffectiveMethod();
    c.active = isStarted && ! isShutDown && (liveUpdates || (! routes.empty() && c.method != Method::Disabled) || ! captures.empty());
    c.routes = routes;
    c.outputDevice = currentOutputDevice();
    c.frozen = tournament;
    c.deviceLinksInputs = host.isNativeNodeDevice();
    c.moveAway = moveAway && canMoveOriginalAway();
    c.silentChoice = silentChoice;
    c.inputDevice = currentInputDevice();
    c.moveRetry = moveRetry;
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
                         || c.stripEndpoints != config.stripEndpoints || c.outputDevice != config.outputDevice || c.frozen != config.frozen
                         || c.deviceLinksInputs != config.deviceLinksInputs || c.moveAway != config.moveAway
                         || c.silentChoice != config.silentChoice || c.inputDevice != config.inputDevice || c.moveRetry != config.moveRetry;
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

bool AppRouting::syncJournal (const RouteJournal::Entry* pendingMove)
{
    std::vector<RouteJournal::Entry> entries;
    std::set<juce::String> covered;
    const bool pendingAway = pendingMove != nullptr && pendingMove->movedAway;
    for (const auto& [pid, routed] : routedEndpoints)
    {
        if (pendingMove != nullptr && ! pendingAway && pendingMove->processId == pid)
            continue; // re-pointed: the pending entry replaces it
        entries.push_back ({ pid, routed.processStartTime, routed.executable, routed.executablePath, routed.endpoint, routed.previousEndpoint, false, false });
        covered.insert (routed.executable);
    }
    // R4.5: the apps' own outputs moved away, one entry per executable.
    for (const auto& [exe, moved] : silentMoves)
        if (! (pendingAway && pendingMove->executable == exe)) // re-pointed: the pending entry replaces it
            entries.push_back ({ moved.processId, moved.processStartTime, exe, moved.executablePath, moved.endpoint, moved.previous,
                                 moved.unconfirmed, true });
    if (pendingMove != nullptr)
    {
        entries.push_back (*pendingMove);
        if (! pendingAway)
            covered.insert (pendingMove->executable);
    }
    // Moves of an earlier run whose processes have not been seen yet keep
    // their process identity; other executables whose move is not undone
    // (their processes exited while routed) are recorded without one.
    for (const auto& [pid, recovered] : recoveredMoves)
        if (routedEndpoints.count (pid) == 0 && routedExecutables.count (recovered.executable) != 0 && covered.insert (recovered.executable).second)
            entries.push_back (recovered);
    for (const auto& exe : routedExecutables)
        if (covered.count (exe) == 0)
            entries.push_back ({ 0, 0, exe, {}, {}, {}, false, false });
    return journal.write (entries);
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

        // Tournament mode (docs/11 E55): nothing is enumerated, moved or linked.
        if (c.frozen)
        {
            wait (kRefreshIntervalMs);
            continue;
        }

        // Also runs while endpoints we moved are outstanding (or apps that
        // exited while routed may come back), so that routes are undone when
        // the method changes or apps are un-mapped.
        int nextPassMs = kRefreshIntervalMs;
        if (c.active || ! routedEndpoints.empty() || ! routedExecutables.empty() || ! silentMoves.empty())
        {
            const auto sessions = router->enumerateSessions();
            std::vector<AppState> states;
            std::set<uint32_t> seen;
            std::set<juce::String> restored; // executables moved back to the default in this pass
            std::set<std::string> busy;      // R4.5: endpoints an unassigned app plays to right now

            // Doubling guard: the endpoint Flubsound plays to (only captures can double).
            OutputEndpoint output;
            if (c.method == Method::ProcessCapture && c.outputDevice.isNotEmpty())
            {
                output.id = router->findOutputEndpointId (c.outputDevice.toStdString());
                output.name = c.outputDevice.toStdString();
            }

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
                a.playsToOutput = playsToEndpoint (session, output.id);

                for (const auto& r : c.routes)
                {
                    if (! executablesMatch (r.executable, a.executable))
                        continue;
                    for (size_t i = 0; i < c.stripNames.size(); ++i)
                        if (c.stripNames[i].equalsIgnoreCase (r.stripName))
                            a.strip = static_cast<int> (i);
                    break;
                }

                if (a.strip < 0 && session.isActive && silentMoves.count (normaliseExecutable (a.executable)) == 0)
                {
                    busy.insert (session.currentEndpointId);
                    busy.insert (session.activeEndpointIds.begin(), session.activeEndpointIds.end());
                }

                // Several sessions can belong to one process: act once per process.
                if (seen.insert (a.processId).second)
                {
                    const auto executable = normaliseExecutable (a.executable);

                    // A move an earlier run made (journal) of this very process
                    // (same executable and, where both are known, start time):
                    // it is still where that run put it. A pending entry is
                    // not taken over: that move may never have happened.
                    if (const auto recovered = recoveredMoves.find (a.processId); recovered != recoveredMoves.end())
                    {
                        const auto& e = recovered->second;
                        const bool sameProcess = e.executable == executable
                                                 && (e.processStartTime == 0 || session.processStartTime == 0 || e.processStartTime == session.processStartTime);
                        if (sameProcess && ! e.pending && routedEndpoints.count (a.processId) == 0)
                            routedEndpoints[a.processId] = { e.endpoint, executable, e.executablePath, e.previousEndpoint, e.processStartTime };
                        recoveredMoves.erase (recovered);
                    }

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
                            // Journal first: a crash after the move must find it.
                            // (Re-pointed from another strip: it played to the
                            // earlier recorded endpoint before Flubsound.)
                            const auto previous = existing != routedEndpoints.end() ? existing->second.previousEndpoint : a.endpointId;
                            const RouteJournal::Entry pending { a.processId, session.processStartTime, executable,
                                                                juce::String (session.executablePath), target, previous, true, false };
                            std::string error;
                            if (! syncJournal (&pending))
                            {
                                a.error = "Flubsound could not write its route journal (" + journal.getFile().getFullPathName()
                                          + "), so it does not move this app: after a crash it could stay on a Flubsound device.";
                            }
                            else if (router->setAppEndpoint (a.processId, target.toStdString(), error))
                            {
                                routedEndpoints[a.processId] = { target, executable, pending.executablePath, previous, session.processStartTime };
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

            // Recovered moves whose processes are gone: their executables stay
            // in routedExecutables (the next process is moved back).
            if (! sessions.empty())
                recoveredMoves.clear();

            // ---- R4.5: move a captured app's own output away -------------------
            const bool canMoveAway = c.method == Method::ProcessCapture && router->canMoveAppOutput();
            std::vector<OutputEndpoint> endpoints;
            SilentTarget silent;
            if (canMoveAway)
            {
                endpoints = router->listOutputEndpoints();
                // An automatic choice stays where the moves already are,
                // unless the user just changed the choice (a fresh pick then).
                const bool choiceChanged = c.silentChoice != silentChoiceSeen;
                silentChoiceSeen = c.silentChoice;
                const std::string current = silentMoves.empty() || choiceChanged ? std::string()
                                                                                 : silentMoves.begin()->second.endpoint.toStdString();
                silent = chooseSilentEndpoint (endpoints, output.id, c.inputDevice, c.silentChoice, busy, current);
            }
            if (! applySilentMoves (c, sessions, states, output, silent, endpoints, canMoveAway).empty())
                nextPassMs = kVerifyMoveDelayMs; // moved or put back: where do the apps play now?

            bool anyDoubled = false;
            for (const auto& a : states)
                anyDoubled = anyDoubled || (a.playsToOutput && a.strip >= 0);

            syncJournal (nullptr); // retried every pass until it is written

            // Where a held-back app could play instead (the guard's fix).
            std::vector<OutputEndpoint> spare;
            if (anyDoubled)
                for (auto& e : canMoveAway ? endpoints : router->listOutputEndpoints())
                    if (e.id != output.id && ! juce::String (e.name).containsIgnoreCase ("Flubsound"))
                        spare.push_back (e);

            {
                const juce::ScopedLock sl (lock);
                workerResult = std::move (states);
                resultRoutedExecutables = routedExecutables;
                resultGeneration = c.generation;
                resultOutputEndpoint = std::move (output);
                resultSpareEndpoints = std::move (spare);
                resultSilentTarget = std::move (silent);
                resultKnownEndpoints = std::move (endpoints);
                resultHasSilentTarget = canMoveAway;
                resultPending = true;
            }
            triggerAsyncUpdate();
        }

        // Connect each strip endpoint's capture side to the device input at
        // the strip's channel where the OS does not (Linux: the sinks'
        // monitors, docs/11 E48a); a no-op elsewhere. The router throttles
        // itself and logs what it cannot link; its status reaches the
        // routing panel (getInputLinkStatus). The native PipeWire device
        // links its own sinks (docs/11 E48): nothing is mapped for it, which
        // also removes links the router made for a device used before.
        {
            const auto firstChannels = host.getDeviceInputMap(); // atomics: safe off the message thread
            std::vector<flub::platform::AppAudioRouter::EndpointInput> inputs;
            for (size_t i = 0; i < c.stripEndpoints.size() && i < firstChannels.size(); ++i)
                inputs.push_back ({ c.stripEndpoints[i].toStdString(), c.deviceLinksInputs ? -1 : firstChannels[i] });
            std::string status;
            const bool linked = router->connectEndpointInputs (inputs, status);
            const juce::ScopedLock sl (lock);
            inputLinkStatus = juce::String (status);
            inputsLinked = linked;
        }

        wait (nextPassMs); // refresh() / config changes wake it early
    }
}

std::set<juce::String> AppRouting::applySilentMoves (const WorkerConfig& c, const std::vector<flub::platform::AudioSessionInfo>& sessions,
                                                     std::vector<AppState>& states, const OutputEndpoint& output, const SilentTarget& silent,
                                                     const std::vector<OutputEndpoint>& endpoints, bool canMoveAway)
{
    if (c.moveRetry != silentRetrySeen)
    {
        silentRetrySeen = c.moveRetry; // the one-click fix: move again what the user took back, retry failures
        silentLeftToUser.clear();
        silentFailures.clear();
    }
    if (c.generation != silentGenerationSeen)
    {
        silentGenerationSeen = c.generation; // a deliberate change retries failed moves
        silentFailures.clear();
    }
    if (! c.moveAway)
        silentLeftToUser.clear(); // switched on again later: a fresh start

    // Per executable: Windows keeps the device per executable, and a browser
    // plays through a child process (the session's process is the one moved).
    struct View
    {
        std::vector<size_t> rows; // indices into sessions / states (parallel)
        juce::String name;        // display name
        bool mapped = false, playsToOutput = false;
    };
    std::map<juce::String, View> views;
    for (size_t i = 0; i < states.size() && i < sessions.size(); ++i)
    {
        const auto exe = normaliseExecutable (states[i].executable);
        if (exe.isEmpty())
            continue;
        auto& v = views[exe];
        v.rows.push_back (i);
        if (v.name.isEmpty())
            v.name = states[i].displayName.isNotEmpty() ? states[i].displayName : states[i].executable;
        v.mapped = v.mapped || states[i].strip >= 0;
        v.playsToOutput = v.playsToOutput || (states[i].strip >= 0 && states[i].playsToOutput);
    }

    const juce::String outputName (output.name.empty() ? juce::String ("the device Flubsound plays to") : juce::String (output.name));
    const auto endpointName = [&endpoints] (const juce::String& id)
    {
        for (const auto& e : endpoints)
            if (sameEndpoint (juce::String (e.id), id))
                return nameOf (e);
        return id;
    };

    struct Outcome
    {
        MoveAway state = MoveAway::None;
        juce::String text;  // the note, or the error (Failed)
        juce::String where; // the silent device's name (Moved / StillPlays)
    };
    std::map<juce::String, Outcome> outcomes;
    std::set<juce::String> movedNow, putBack;

    for (auto& entry : views)
    {
        const auto& exe = entry.first; // (not a structured binding: older Apple Clang cannot capture those in lambdas)
        const auto& view = entry.second;

        // The process to act through: the recorded one while it runs, else the first.
        auto sm = silentMoves.find (exe);
        const auto* via = &sessions[view.rows.front()];
        if (sm != silentMoves.end())
            for (const auto row : view.rows)
                if (sessions[row].processId == sm->second.processId)
                    via = &sessions[row];
        const auto pid = via->processId;

        // Endpoint routing took the app over: its own restore applies.
        if (std::any_of (view.rows.begin(), view.rows.end(), [&] (size_t row) { return routedEndpoints.count (sessions[row].processId) != 0; }))
        {
            if (sm != silentMoves.end())
                silentMoves.erase (sm);
            continue;
        }

        // A move stays while the app is captured with the option on, also
        // while the output cannot be named (an ASIO driver: no new moves then,
        // as nothing is known to play to it).
        const bool wanted = canMoveAway && c.moveAway && view.mapped;

        // Moves the app's own output to the silent device, journaled first.
        const auto moveTo = [&] (const juce::String& previous)
        {
            auto& failure = silentFailures[exe];
            if (failure.attempts >= kMaxMoveAttempts)
            {
                outcomes[exe] = { MoveAway::Failed, failure.error, {} };
                return;
            }
            const RouteJournal::Entry pending { pid, via->processStartTime, exe, juce::String (via->executablePath),
                                                juce::String (silent.endpoint.id), previous, true, true };
            std::string error;
            juce::String problem;
            if (! syncJournal (&pending))
                problem = "Flubsound could not write its route journal (" + journal.getFile().getFullPathName()
                          + "), so it does not move this app's own sound: after a crash it could stay on " + nameOf (silent.endpoint) + ".";
            else if (! router->setAppEndpoint (pid, silent.endpoint.id, error))
                problem = "Flubsound could not move its own sound to " + nameOf (silent.endpoint) + ": " + juce::String (error);
            if (problem.isNotEmpty())
            {
                ++failure.attempts;
                failure.error = problem;
                outcomes[exe] = { MoveAway::Failed, problem, {} };
                return;
            }
            silentFailures.erase (exe);
            silentMoves[exe] = { juce::String (silent.endpoint.id), previous, pid, via->processStartTime, juce::String (via->executablePath), false };
            movedNow.insert (exe);
            outcomes[exe] = { MoveAway::Moved, {}, nameOf (silent.endpoint) };
        };

        if (sm != silentMoves.end())
        {
            auto& m = sm->second;
            m.processId = pid;
            m.processStartTime = via->processStartTime;
            if (! via->executablePath.empty())
                m.executablePath = juce::String (via->executablePath);

            if (! wanted)
            {
                // Unassigned, the option off or capture no longer the method:
                // its own earlier device comes back (the system default when
                // that device is gone). A failure is retried by the next pass.
                std::string error;
                bool done = router->setAppEndpoint (pid, m.previous.toStdString(), error);
                if (! done && m.previous.isNotEmpty())
                    done = router->setAppEndpoint (pid, {}, error);
                if (done)
                {
                    silentMoves.erase (sm);
                    putBack.insert (exe);
                }
                continue;
            }

            // Did the user change the app's device since the move?
            std::string persisted, readError;
            const bool known = router->getAppEndpoint (pid, persisted, readError);
            if (known && ! sameEndpoint (juce::String (persisted), m.endpoint))
            {
                const bool userChange = ! m.unconfirmed; // else: an earlier run's move that never happened
                silentMoves.erase (sm);
                if (userChange)
                {
                    silentLeftToUser.insert (exe);
                    outcomes[exe] = { MoveAway::LeftToUser,
                                      view.name + "'s output was changed after Flubsound moved it, so Flubsound leaves it there.", {} };
                    continue;
                }
                // Treated as never moved: below.
            }
            else
            {
                if (known)
                    m.unconfirmed = false;
                if (silent.endpoint.id.empty())
                {
                    // Paused: Windows plays the app on the default meanwhile,
                    // and returns it to the device when that comes back.
                    outcomes[exe] = { MoveAway::NoTarget, silent.reason, {} };
                    continue;
                }
                if (! sameEndpoint (m.endpoint, juce::String (silent.endpoint.id)))
                {
                    moveTo (m.previous); // re-pointed: its own earlier device stays the one to put back
                    continue;
                }
                const auto where = endpointName (m.endpoint);
                if (view.playsToOutput)
                    outcomes[exe] = { MoveAway::StillPlays,
                                      view.name + " still plays to " + outputName + " after Flubsound moved its own sound to " + where
                                          + ": the app picks that device itself. Set its output to Default inside the app, or restart its playback.",
                                      where };
                else
                    outcomes[exe] = { MoveAway::Moved, {}, where };
                continue;
            }
        }

        // Not moved by Flubsound: only an app that plays to the output is
        // moved. Elsewhere - e.g. a device the user set for it - it is left
        // alone and nothing is recorded.
        if (! wanted || ! view.playsToOutput)
            continue;
        if (silentLeftToUser.count (exe) != 0)
        {
            outcomes[exe] = { MoveAway::LeftToUser, view.name + "'s output was changed after Flubsound moved it, so Flubsound leaves it there.", {} };
            continue;
        }
        if (silent.endpoint.id.empty())
        {
            outcomes[exe] = { MoveAway::NoTarget, silent.reason, {} };
            continue;
        }
        // Its own device, put back later: unknown counts as the system default.
        std::string previous, readError;
        if (! router->getAppEndpoint (pid, previous, readError))
            previous.clear();
        if (sameEndpoint (juce::String (previous), juce::String (silent.endpoint.id)))
        {
            // Its device already is the silent one, yet it plays to the output.
            outcomes[exe] = { MoveAway::StillPlays,
                              view.name + " still plays to " + outputName + " although its output is set to " + nameOf (silent.endpoint)
                                  + ": the app picks that device itself. Set its output to Default inside the app, or restart its playback.",
                              nameOf (silent.endpoint) };
            continue;
        }
        moveTo (juce::String (previous));
    }

    for (auto& a : states)
    {
        if (a.strip < 0)
            continue;
        const auto exe = normaliseExecutable (a.executable);
        // Moved now: whether its streams follow is known at the next pass, in
        // 250 ms (a stream opened on the device itself stays until the app
        // opens a new one, as measured on Windows 11). Until then neither
        // captured (it could be heard twice) nor shown as held back.
        a.movePending = movedNow.count (exe) != 0;
        if (const auto o = outcomes.find (exe); o != outcomes.end())
        {
            a.moveAway = o->second.state;
            a.movedTo = o->second.where;
            if (o->second.state == MoveAway::Failed)
                a.error = o->second.text;
            else
                a.moveNote = o->second.text;
        }
    }
    putBack.insert (movedNow.begin(), movedNow.end());
    return putBack;
}

// =============================================================================
// Message thread
// =============================================================================
void AppRouting::handleAsyncUpdate()
{
    std::vector<AppState> next;
    std::set<juce::String> routedApps;
    uint64_t generation = 0;
    OutputEndpoint output;
    std::vector<OutputEndpoint> spare;
    SilentTarget silent;
    std::vector<OutputEndpoint> endpoints;
    bool hasSilentTarget = false;
    {
        const juce::ScopedLock sl (lock);
        if (! resultPending)
            return;
        next = std::move (workerResult);
        routedApps = std::move (resultRoutedExecutables);
        generation = resultGeneration;
        output = std::move (resultOutputEndpoint);
        spare = std::move (resultSpareEndpoints);
        silent = std::move (resultSilentTarget);
        endpoints = std::move (resultKnownEndpoints);
        hasSilentTarget = resultHasSilentTarget;
        resultPending = false;
    }

    // The worker's endpoint moves happened whatever the configuration is now.
    persistRoutedExecutables (routedApps);

    // The output device changed since the pass began (nothing tells this
    // class): its doubling verdicts are about the old one. Publishing bumps
    // the generation, so this result is dropped and a fresh pass follows.
    if (currentOutputDevice() != config.outputDevice)
        publishConfig();

    // Strip indices from an outdated route set / strip layout: applying them
    // would start or re-point captures into the wrong strip. The change that
    // bumped the generation already woke the worker for a fresh pass.
    if (generation != configGeneration)
        return;

    outputEndpoint = std::move (output);
    spareEndpoints = std::move (spare);
    silentTarget = hasSilentTarget ? silent.endpoint : OutputEndpoint();
    silentTargetReason = hasSilentTarget ? silent.reason : juce::String();
    if (hasSilentTarget)
        knownEndpoints = std::move (endpoints); // kept while no such pass runs (Settings' list)
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
        // R4.5: its own output was moved in this pass. The next pass, in
        // 250 ms, shows whether it plays elsewhere now: until then a running
        // capture (a re-pointed move) stays, and none starts.
        if (captureMethod && a.strip >= 0 && a.movePending)
        {
            a.doublingBlocked = false;
            continue;
        }
        // Doubling guard: the app plays to Flubsound's own output device, so
        // a capture would be heard on top of the original.
        a.doublingBlocked = captureMethod && a.strip >= 0 && a.playsToOutput;
        if (captureMethod && a.strip >= 0 && ! a.doublingBlocked)
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

    std::set<uint32_t> blocked;
    for (const auto& a : states)
        if (a.doublingBlocked)
            blocked.insert (a.processId);
    for (auto& a : states)
    {
        const auto cap = captures.find (a.processId);
        a.captureId = cap != captures.end() ? cap->second.id : -1;
        a.doublingBlocked = blocked.count (a.processId) != 0; // every session of the process
    }
}

void AppRouting::stopAllCaptures()
{
    for (const auto& entry : captures)
        host.stopProcessCapture (entry.second.id);
    captures.clear();
}
} // namespace flub::app
