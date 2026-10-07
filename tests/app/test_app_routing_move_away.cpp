// App-level tests: docs/11 E47 / R4.5, "Move the app's own sound away
// automatically". An app Flubsound captures while it plays straight to the
// device Flubsound plays to is held back by the doubling guard; with the
// option on (off by default; the routing panel's one-click fix switches it
// on) Flubsound moves the app's own output to a silent device first and then
// captures it, journals the move, and puts the app's own device back when it
// is unassigned, the option goes off, at shutdown and after a crash (also
// one during a re-point; not in Tournament mode). An app whose stream stays
// on the silent device after the put-back is reported ("restart its
// playback"). Moves are kept per executable file.
//
// The audio service is scripted (RoutingTestFakes.h, followMoves): per-app
// devices kept per executable file like Windows keeps them, sessions that
// follow a move unless the app picks its device itself ("pinned": also how
// an open stream stays put on Windows 11). The owner's PC: the
// Turtle Beach Stealth 600PC Gen 3 headset is the default and Flubsound's
// output; S/PDIF (nothing plugged in) and two NVIDIA HDMI monitor outputs.
#include "RoutingTestFakes.h"

#include "engine/EngineController.h"
#include "settings/AppSettings.h"
#include "ui/RoutingPanel.h"
#include "ui/SettingsDialog.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

using namespace flub::app;
using flubapptest::AudioServiceScript;
using flubapptest::findRoutedApp;
using flubapptest::makeSession;
using flubapptest::ScriptedRouter;
using OutputEndpoint = flub::platform::AppAudioRouter::OutputEndpoint;
using MoveAway = AppRouting::MoveAway;

namespace
{
const char* const kHeadset = "Speakers (Stealth 600PC Gen 3)";
const char* const kSpdif = "Digital Audio (S/PDIF) (High Definition Audio Device)";
const char* const kHdmi1 = "LG ULTRAGEAR (NVIDIA High Definition Audio)";
const char* const kHdmi2 = "DELL U2720Q (NVIDIA High Definition Audio)";
const char* const kCable = "CABLE Input (VB-Audio Virtual Cable)";

std::vector<OutputEndpoint> ownerEndpoints()
{
    return { { "ep-headset", kHeadset }, { "ep-spdif", kSpdif }, { "ep-hdmi1", kHdmi1 }, { "ep-hdmi2", kHdmi2 } };
}

/** The owner's PC as the audio service: a default Windows build (no endpoint
    routing), apps' own outputs movable. */
void scriptOwnerPc (AudioServiceScript& script)
{
    const std::lock_guard<std::mutex> g (script.lock);
    script.movable = false;
    script.outputMovable = true;
    script.followMoves = true;
    script.systemDefault = "ep-headset";
    script.endpoints = ownerEndpoints();
}

void setEndpoints (AudioServiceScript& script, std::vector<OutputEndpoint> endpoints)
{
    const std::lock_guard<std::mutex> g (script.lock);
    script.endpoints = std::move (endpoints);
}

/** The user (or Windows) puts an executable's sessions on `endpoint`; with
    `device` it also sets the per-app device ("" removes it). `executable`:
    a name, or a path for sessions that have one. */
void placeApp (AudioServiceScript& script, const std::string& executable, const std::string& endpoint, const std::string* device = nullptr)
{
    const std::lock_guard<std::mutex> g (script.lock);
    script.placeSessions (executable, endpoint);
    if (device != nullptr)
    {
        if (device->empty())
            script.appDevices.erase (AudioServiceScript::keyFor (executable));
        else
            script.appDevices[AudioServiceScript::keyFor (executable)] = *device;
    }
}

/** A session of a process whose executable file is `file` (Windows reports the path). */
flub::platform::AudioSessionInfo sessionAt (uint32_t pid, const char* exe, const juce::File& file, const char* endpoint, uint64_t startTime)
{
    auto s = makeSession (pid, exe, endpoint, startTime);
    s.executablePath = file.getFullPathName().toStdString();
    return s;
}

/** Message-thread log lines (juce::Logger) while it is installed. */
struct CapturingLogger final : juce::Logger
{
    CapturingLogger() : previous (juce::Logger::getCurrentLogger()) { juce::Logger::setCurrentLogger (this); }
    ~CapturingLogger() override { juce::Logger::setCurrentLogger (previous); }
    juce::Logger* previous;
    void logMessage (const juce::String& message) override
    {
        const std::lock_guard<std::mutex> g (lock);
        lines.add (message);
    }
    juce::String text()
    {
        const std::lock_guard<std::mutex> g (lock);
        return lines.joinIntoString ("\n");
    }
    std::mutex lock;
    juce::StringArray lines;
};

std::vector<RouteJournal::Entry> readJournal (const juce::File& file)
{
    return RouteJournal (file).load();
}

/** Waits for `count` more enumerations of the fake audio service and for the
    last result to reach the message thread. */
void waitForPasses (AppRouting& routing, AudioServiceScript& script, int count)
{
    for (int i = 0; i < count; ++i)
    {
        const int before = script.getEnumerations();
        routing.refresh();
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return script.getEnumerations() > before; }));
    }
    bool drained = false;
    juce::Timer::callAfterDelay (20, [&drained] { drained = true; });
    REQUIRE (flubapptest::pumpMessagesUntil ([&drained] { return drained; }));
}

/** Settings, host, counting captures and AppRouting on a scripted audio service. */
struct Rig
{
    explicit Rig (const juce::File& settingsFile) : settings (settingsFile, false)
    {
        starts = flubapptest::useCountingCaptures (host);
        scriptOwnerPc (script);
    }

    /** Creates and starts the routing (after the settings and sessions are set). */
    void start()
    {
        routing = std::make_unique<AppRouting> (host, settings, std::make_unique<ScriptedRouter> (script), true);
        routing->setOutputDeviceSource ([] { return juce::String (kHeadset); });
        routing->setInputDeviceSource ([] { return juce::String(); });
        routing->start();
    }

    const AppRouting::AppState* app (uint32_t pid) const { return findRoutedApp (*routing, pid); }
    bool captured (uint32_t pid) const
    {
        const auto* a = app (pid);
        return a != nullptr && a->captureId >= 0;
    }
    bool movedAway (uint32_t pid) const
    {
        const auto* a = app (pid);
        return a != nullptr && a->moveAway == MoveAway::Moved && a->captureId >= 0;
    }
    juce::File journalFile() const { return routing->getJournalFile(); }

    AppSettings settings;
    AudioEngineHost host;
    std::shared_ptr<std::map<uint32_t, int>> starts;
    AudioServiceScript script;
    std::unique_ptr<AppRouting> routing; // last: it holds the script through its router
};
} // namespace

// =============================================================================
TEST_CASE ("App: R4.5 silent device rules: S/PDIF, then a free HDMI output; never the output, the default, Flubsound's own or a cable feeding Flubsound")
{
    using R = AppRouting;
    auto all = ownerEndpoints();
    all.push_back ({ "ep-cable", kCable });
    all.push_back ({ "ep-flub", "Flubsound Game (Flubsound Virtual Audio)" });
    const auto pick = [] (const std::vector<OutputEndpoint>& endpoints, const std::string& output, const juce::String& input,
                          const OutputEndpoint& chosen, const std::set<std::string>& busy, const std::string& current)
    { return R::chooseSilentEndpoint (endpoints, output, input, chosen, busy, current); };

    // Automatic: S/PDIF first, then an HDMI output, each only while no
    // unassigned app plays to it (a receiver or a monitor's speakers may be
    // in use then).
    CHECK (pick (all, "ep-headset", {}, {}, {}, {}).endpoint.id == "ep-spdif");
    CHECK (pick (all, "ep-headset", {}, {}, { "ep-spdif" }, {}).endpoint.id == "ep-hdmi1");
    CHECK (pick (all, "ep-headset", {}, {}, { "ep-spdif", "ep-hdmi1" }, {}).endpoint.id == "ep-hdmi2");
    CHECK (pick (all, "ep-headset", {}, {}, { "ep-hdmi2" }, "ep-hdmi2").endpoint.id == "ep-spdif"); // where the moves are, but now in use
    std::vector<OutputEndpoint> noSpdif;
    for (const auto& e : all)
        if (e.id != "ep-spdif")
            noSpdif.push_back (e);
    CHECK (pick (noSpdif, "ep-headset", {}, {}, {}, {}).endpoint.id == "ep-hdmi1");
    CHECK (pick (noSpdif, "ep-headset", {}, {}, { "ep-hdmi1" }, {}).endpoint.id == "ep-hdmi2");
    CHECK (pick (noSpdif, "ep-headset", {}, {}, { "ep-hdmi1", "ep-hdmi2" }, {}).endpoint.id.empty());
    CHECK (pick (all, "ep-headset", {}, {}, {}, "ep-hdmi2").endpoint.id == "ep-hdmi2");  // stays where the moves already are
    CHECK (pick (all, "ep-headset", {}, {}, {}, "ep-headset").endpoint.id == "ep-spdif"); // ... unless that is no candidate
    // Never a cable or Flubsound's own, even with nothing else.
    const std::vector<OutputEndpoint> laptop { { "ep-headset", kHeadset }, { "ep-speakers", "Speakers (Realtek(R) Audio)" }, { "ep-cable", kCable } };
    const auto none = pick (laptop, "ep-headset", {}, {}, {}, {});
    CHECK (none.endpoint.id.empty());
    CHECK (none.reason.startsWith ("No output device that is likely silent was found"));
    CHECK (none.reason.contains ("Settings > Routing"));
    // Flubsound plays to S/PDIF itself: an HDMI output then.
    CHECK (pick (all, "ep-spdif", {}, {}, {}, {}).endpoint.id == "ep-hdmi1");

    // A chosen device wins when it is allowed ...
    CHECK (pick (all, "ep-headset", {}, { "ep-hdmi2", kHdmi2 }, { "ep-hdmi2" }, {}).endpoint.id == "ep-hdmi2"); // busy does not matter then
    CHECK (pick (all, "ep-headset", {}, { "EP-HDMI2", kHdmi2 }, {}, {}).endpoint.id == "ep-hdmi2");             // ids compare ignoring case
    CHECK (pick (all, "ep-headset", "Microphone (Realtek(R) Audio)", { "ep-cable", kCable }, {}, {}).endpoint.id == "ep-cable");
    // ... and says why not otherwise.
    auto refused = pick (all, "ep-headset", {}, { "ep-headset", kHeadset }, {}, {});
    CHECK (refused.endpoint.id.empty());
    CHECK (refused.reason == juce::String (kHeadset) + " is the device Flubsound plays to. Choose another silent device in Settings > Routing.");
    refused = pick (all, "ep-spdif", {}, { "ep-headset", kHeadset }, {}, {});
    CHECK (refused.reason.contains ("system's default output"));
    refused = pick (all, "ep-headset", "CABLE Output (VB-Audio Virtual Cable)", { "ep-cable", kCable }, {}, {});
    CHECK (refused.reason.contains ("feeds Flubsound's input (CABLE Output (VB-Audio Virtual Cable))"));
    refused = pick (all, "ep-headset", {}, { "ep-flub", "Flubsound Game (Flubsound Virtual Audio)" }, {}, {});
    CHECK (refused.reason.contains ("one of Flubsound's own devices"));
    refused = pick (all, "ep-headset", {}, { "ep-gone", "Digital Audio (S/PDIF) (Old Card)" }, {}, {});
    CHECK (refused.endpoint.id.empty());
    CHECK (refused.reason.startsWith ("Digital Audio (S/PDIF) (Old Card) (the silent device chosen in Settings > Routing) is not connected"));

    CHECK (R::silentPreference (kSpdif) == 2);
    CHECK (R::silentPreference ("Realtek Digital Output (Realtek(R) Audio)") == 2);
    CHECK (R::silentPreference ("Optical Out (USB DAC)") == 2);
    CHECK (R::silentPreference (kHdmi1) == 1);
    CHECK (R::silentPreference ("DisplayPort (Intel(R) Display Audio)") == 1);
    CHECK (R::silentPreference (kHeadset) == 0);
    CHECK (R::silentPreference ("Speakers (Realtek(R) Audio)") == 0);
    CHECK (R::silentPreference ("SPDIF Out (Sound Blaster Z)") == 2);
    CHECK (R::silentPreference ("Digital Audio (HDMI) (High Definition Audio Device)") == 1);
    // Named after what one listens with, or a generic "Digital Audio" (USB DACs, headsets): never likely silent.
    CHECK (R::silentPreference ("Speakers (USB Digital Audio)") == 0);
    CHECK (R::silentPreference ("Headphones (Optical Out DAC)") == 0);
    CHECK (R::silentPreference ("Speakers (NVIDIA High Definition Audio)") == 0);
    CHECK (R::silentPreference ("Digital Audio (USB Audio Device)") == 0);
    CHECK (pick ({ { "ep-headset", kHeadset }, { "ep-dac", "Speakers (USB Digital Audio)" } }, "ep-headset", {}, {}, {}, {}).endpoint.id.empty());
    CHECK (R::looksLikeVirtualCable (kCable));
    CHECK (R::looksLikeVirtualCable ("VoiceMeeter Input (VB-Audio VoiceMeeter VAIO)"));
    CHECK (! R::looksLikeVirtualCable (kSpdif));
    CHECK (! R::looksLikeVirtualCable (kHeadset));
    CHECK (R::feedsInput (kCable, "CABLE Output (VB-Audio Virtual Cable)"));
    CHECK (! R::feedsInput (kCable, "CABLE-A Output (VB-Audio Cable A)"));
    CHECK (! R::feedsInput ("Speakers (Realtek(R) Audio)", "Microphone (Realtek(R) Audio)")); // same card, not a cable
    CHECK (! R::feedsInput (kCable, {}));

    // Windows names a per-app device by its device interface path.
    using Router = flub::platform::AppAudioRouter;
    CHECK (Router::endpointIdFromInterfacePath ("\\\\?\\SWD#MMDEVAPI#{0.0.0.00000000}.{8b2e6b1c-0000-4000-8000-000000000001}"
                                                "#{e6327cad-dcec-4949-ae8a-991e976a79d2}")
           == "{0.0.0.00000000}.{8b2e6b1c-0000-4000-8000-000000000001}");
    CHECK (Router::endpointIdFromInterfacePath ("\\\\?\\swd#mmdevapi#{0.0.0.00000000}.{ABC}#{e6327cad-dcec-4949-ae8a-991e976a79d2}")
           == "{0.0.0.00000000}.{ABC}");
    CHECK (Router::endpointIdFromInterfacePath ("{0.0.0.00000000}.{abc}") == "{0.0.0.00000000}.{abc}");
    CHECK (Router::endpointIdFromInterfacePath ("").empty());
}

TEST_CASE ("App: R4.5 off by default: an app on the headset is held back and nothing moves; the one-click fix moves it before its capture starts")
{
    const flubapptest::TempFolder temp;
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false; // started below with the scripted audio service
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = false;
    o.foregroundAppFactory = [] { return std::unique_ptr<flub::platform::ForegroundApp>(); };
    EngineController controller (o);
    auto& routing = controller.getRouting();

    // Captures that count their starts where the worker can read them.
    auto captureStarts = std::make_shared<std::atomic<int>> (0);
    struct Capture final : flub::platform::ProcessLoopbackCapture
    {
        explicit Capture (std::shared_ptr<std::atomic<int>> s) : counter (std::move (s)) {}
        bool isSupported() const override { return true; }
        bool start (uint32_t, bool, double, int, FrameCallback, std::string&) override
        {
            ++*counter;
            running = true;
            return true;
        }
        void stop() override { running = false; }
        bool isRunning() const override { return running; }
        std::shared_ptr<std::atomic<int>> counter;
        bool running = false;
    };
    controller.getHost().setCaptureFactory ([captureStarts] { return std::make_unique<Capture> (captureStarts); });

    AudioServiceScript script;
    scriptOwnerPc (script);
    script.setSessions ({ makeSession (1004, "msedge.exe", "ep-headset", 9001) }); // Edge's audio process
    std::atomic<int> capturesAtMove { -1 };
    script.onMove = [&] (uint32_t, const std::string& endpoint)
    {
        if (endpoint == "ep-spdif")
            capturesAtMove = captureStarts->load();
    };
    routing.setRouter (std::make_unique<ScriptedRouter> (script), true);
    routing.setOutputDeviceSource ([] { return juce::String (kHeadset); });
    routing.setRoute ("msedge", "Music");
    routing.start();

    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        const auto* a = findRoutedApp (routing, 1004);
        return a != nullptr && a->doublingBlocked;
    }));
    CHECK (routing.canMoveOriginalAway());
    CHECK (! routing.getMoveOriginalAway()); // off by default
    CHECK (script.getMoves().empty());
    CHECK (*captureStarts == 0);
    CHECK ((routing.getSilentTarget() == OutputEndpoint { "ep-spdif", kSpdif }));
    CHECK (routing.getMoveAwayAction() == "Move automatically to " + juce::String (kSpdif));
    CHECK (routing.describeDoubling().contains ("Or let Flubsound do it: \"Move automatically to " + juce::String (kSpdif) + "\""));
    CHECK (routing.describeMoveAway().startsWith ("Off (default)."));

    ui::RoutingPanel panel (controller);
    panel.setSize (340, 900);
    panel.refreshRouting();
    CHECK (panel.isShowingNoAppsProcessed()); // nothing else is processed: the red state with the doubling text
    CHECK (panel.getDoublingFixAction() == "Move automatically to " + juce::String (kSpdif));

    // The one click: the option goes on, Edge's own output moves, then its capture starts.
    panel.applyDoublingFixAction();
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        const auto* a = findRoutedApp (routing, 1004);
        return a != nullptr && a->captureId >= 0;
    }));
    CHECK (routing.getMoveOriginalAway());
    CHECK (controller.getSettings().getMoveOriginalAway()); // persisted
    CHECK (capturesAtMove == 0);                            // moved before any capture: never heard twice
    CHECK (script.countMoves (1004, "ep-spdif") == 1);
    CHECK (script.getAppDevice ("msedge") == "ep-spdif");
    const auto* edge = findRoutedApp (routing, 1004);
    CHECK (! edge->doublingBlocked);
    CHECK (edge->moveAway == MoveAway::Moved);
    CHECK (edge->movedTo == kSpdif);
    auto journal = readJournal (routing.getJournalFile());
    REQUIRE (journal.size() == 1);
    CHECK (journal[0].movedAway);
    CHECK (journal[0].executable == "msedge");
    CHECK (journal[0].processId == 1004);
    CHECK (journal[0].processStartTime == 9001);
    CHECK (journal[0].endpoint == "ep-spdif");
    CHECK (journal[0].previousEndpoint.isEmpty()); // it followed the system default
    CHECK (! journal[0].pending);

    // The pass after the move confirms it: still captured, nothing moved again.
    waitForPasses (routing, script, 2);
    CHECK (findRoutedApp (routing, 1004)->captureId >= 0);
    CHECK (*captureStarts == 1);
    CHECK (script.getMoves().size() == 1);
    CHECK (routing.describeMoveAway() == "On. 1 app plays its own sound to " + juce::String (kSpdif) + " while Flubsound captures it.");
    panel.refreshRouting();
    CHECK (! panel.isShowingNoAppsProcessed());
    CHECK (panel.getNotice().isEmpty());
    CHECK (panel.getDoublingFixAction().isEmpty());
    CHECK (panel.getStripDescription (1).contains ("msedge: playing (routed to Music), its own sound moved to " + juce::String (kSpdif)));

    // Quitting puts Edge back on the system default and empties the journal.
    controller.shutdown();
    CHECK (script.countMoves (1004, "") == 1);
    CHECK (script.getAppDevice ("msedge").empty());
    CHECK (! routing.getJournalFile().exists());
}

TEST_CASE ("App: R4.5 the app's own device comes back when it is unassigned or the option goes off; a device the user chose is restored")
{
    const flubapptest::TempFolder temp;
    Rig rig (temp.file ("settings.xml"));
    rig.script.setSessions ({ makeSession (700, "Spotify.exe", "ep-headset", 7001) });
    const std::string headset ("ep-headset");
    placeApp (rig.script, "spotify", "ep-headset", &headset); // the user set Spotify to the headset in the Volume mixer
    rig.settings.setMoveOriginalAway (true);
    rig.settings.setAppRoutes ({ { "spotify", "Music" } });
    rig.start();

    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return rig.movedAway (700); }));
    CHECK ((rig.script.getMoves() == std::vector<std::pair<uint32_t, std::string>> { { 700, "ep-spdif" } }));
    auto journal = readJournal (rig.journalFile());
    REQUIRE (journal.size() == 1);
    CHECK (journal[0].previousEndpoint == "ep-headset"); // its own device, to come back

    // Unassigned: back on the device the user chose, the capture stops, the journal goes.
    rig.routing->removeRoute ("spotify");
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    { return rig.script.getAppDevice ("spotify") == "ep-headset" && ! rig.journalFile().exists() && rig.host.getCaptures().empty(); }));
    CHECK (rig.script.countMoves (700, "ep-headset") == 1);

    // Assigned again: moved again. The option off: back again; the guard holds it.
    rig.routing->setRoute ("spotify", "Music");
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return rig.movedAway (700) && rig.script.countMoves (700, "ep-spdif") == 2; }));
    rig.routing->setMoveOriginalAway (false);
    CHECK (! rig.settings.getMoveOriginalAway());
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        const auto* a = rig.app (700);
        return rig.script.getAppDevice ("spotify") == "ep-headset" && a != nullptr && a->doublingBlocked;
    }));
    CHECK (rig.script.countMoves (700, "ep-headset") == 2);
    CHECK (! rig.journalFile().exists());
    CHECK (rig.app (700)->moveAway == MoveAway::None);

    // Endpoint routing or capture switched off: the same.
    rig.routing->setMoveOriginalAway (true);
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return rig.movedAway (700) && rig.script.countMoves (700, "ep-spdif") == 3; }));
    rig.routing->setMethod (AppRouting::Method::Disabled);
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return rig.script.countMoves (700, "ep-headset") == 3 && ! rig.journalFile().exists(); }));
    rig.routing->shutdown();
    CHECK (rig.script.getMoves().size() == 6); // nothing put back twice
}

namespace
{
/** Run 1 of the restart cases: Spotify is moved; the journal is copied at
    the move (still pending) and after it. Returns the run's journal file. */
juce::File moveSpotifyAndCopyJournal (const flubapptest::TempFolder& temp, const juce::File& beforeMove, const juce::File& afterMove,
                                      bool quitWithSpotifyClosed)
{
    Rig rig (temp.file ("run1/settings.xml"));
    const auto journalFile = rig.settings.getPropertiesFile().getFile().getSiblingFile ("route-journal.json");
    rig.script.setSessions ({ makeSession (700, "Spotify.exe", "ep-headset", 7001) });
    rig.script.onMove = [journalFile, beforeMove] (uint32_t, const std::string& endpoint)
    {
        if (endpoint == "ep-spdif")
            journalFile.copyFileTo (beforeMove);
    };
    rig.settings.setMoveOriginalAway (true);
    rig.settings.setAppRoutes ({ { "spotify", "Music" } });
    rig.start();
    REQUIRE (rig.journalFile() == journalFile);
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return rig.movedAway (700); }));
    REQUIRE (journalFile.copyFileTo (afterMove));
    if (quitWithSpotifyClosed)
    {
        // Spotify is closed, then Flubsound quits: nothing to move it back
        // through, so the journal keeps it.
        rig.script.setSessions ({ makeSession (300, "Discord.exe", "ep-headset", 7003) });
        waitForPasses (*rig.routing, rig.script, 1);
    }
    rig.routing->shutdown();
    CHECK (rig.script.countMoves (700, "") == (quitWithSpotifyClosed ? 0 : 1));
    return journalFile;
}
} // namespace

TEST_CASE ("App: R4.5 quit while a moved app is closed: the journal keeps the move and the next start puts the app back when it runs")
{
    const flubapptest::TempFolder temp;
    const auto journalOfRun1 = moveSpotifyAndCopyJournal (temp, temp.file ("before.json"), temp.file ("after.json"), true);
    REQUIRE (journalOfRun1.existsAsFile());
    const auto left = readJournal (journalOfRun1);
    REQUIRE (left.size() == 1);
    CHECK (left[0].movedAway);
    CHECK (left[0].executable == "spotify");

    // Run 2, without the route: Spotify starts again (Windows kept it on
    // S/PDIF, another pid) and is moved back through that process.
    Rig rig (journalOfRun1.getSiblingFile ("settings.xml"));
    rig.script.setSessions ({ makeSession (701, "Spotify.exe", "ep-spdif", 7101) });
    const std::string spdif ("ep-spdif");
    placeApp (rig.script, "spotify", "ep-spdif", &spdif);
    rig.start();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return rig.script.countMoves (701, "") == 1 && ! rig.journalFile().exists(); }));
    CHECK (rig.script.getAppDevice ("spotify").empty());
    rig.routing->shutdown();
    CHECK (rig.script.getMoves().size() == 1);
}

TEST_CASE ("App: R4.5 a crash after the move keeps the assigned app where it is; a crash before it moves the app afresh")
{
    const flubapptest::TempFolder temp;
    const auto beforeMove = temp.file ("crash-before-move.json"), afterMove = temp.file ("crash-after-move.json");
    moveSpotifyAndCopyJournal (temp, beforeMove, afterMove, false);
    REQUIRE (readJournal (beforeMove).size() == 1);
    CHECK (readJournal (beforeMove)[0].pending);
    CHECK (readJournal (afterMove)[0].movedAway);
    CHECK (! readJournal (afterMove)[0].pending);

    // Crashed right after the move, the app still assigned and running: it
    // stays where it is (no new move) and is put back at the next quit.
    {
        Rig rig (temp.file ("restart-after/settings.xml"));
        REQUIRE (temp.file ("restart-after").createDirectory().wasOk());
        REQUIRE (afterMove.copyFileTo (rig.settings.getPropertiesFile().getFile().getSiblingFile ("route-journal.json")));
        rig.script.setSessions ({ makeSession (700, "Spotify.exe", "ep-spdif", 7001) });
        const std::string spdif ("ep-spdif");
        placeApp (rig.script, "spotify", "ep-spdif", &spdif);
        rig.settings.setMoveOriginalAway (true);
        rig.settings.setAppRoutes ({ { "spotify", "Music" } });
        rig.start();
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return rig.movedAway (700); }));
        waitForPasses (*rig.routing, rig.script, 2);
        CHECK (rig.script.getMoves().empty());
        rig.routing->shutdown();
        CHECK ((rig.script.getMoves() == std::vector<std::pair<uint32_t, std::string>> { { 700, "" } }));
    }

    // Crashed between the journal write and the move: the move never
    // happened. It is not taken for the user's own change: moved now.
    {
        Rig rig (temp.file ("restart-before/settings.xml"));
        REQUIRE (temp.file ("restart-before").createDirectory().wasOk());
        REQUIRE (beforeMove.copyFileTo (rig.settings.getPropertiesFile().getFile().getSiblingFile ("route-journal.json")));
        rig.script.setSessions ({ makeSession (700, "Spotify.exe", "ep-headset", 7001) });
        rig.settings.setMoveOriginalAway (true);
        rig.settings.setAppRoutes ({ { "spotify", "Music" } });
        rig.start();
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return rig.movedAway (700); }));
        CHECK (rig.script.countMoves (700, "ep-spdif") == 1);
        CHECK (! readJournal (rig.journalFile()).front().pending);
        rig.routing->shutdown();
        CHECK (rig.script.countMoves (700, "") == 1);
    }
}

TEST_CASE ("App: R4.5 an app the user moved is left alone; a device change after Flubsound's move is the user's until \"Move again\"")
{
    const flubapptest::TempFolder temp;
    Rig rig (temp.file ("settings.xml"));
    // Edge was set to S/PDIF by hand (the owner's PC today); Spotify follows the default.
    rig.script.setSessions ({ makeSession (1004, "msedge.exe", "ep-spdif", 9001), makeSession (700, "Spotify.exe", "ep-headset", 7001) });
    const std::string spdif ("ep-spdif");
    placeApp (rig.script, "msedge", "ep-spdif", &spdif);
    rig.settings.setMoveOriginalAway (true);
    rig.settings.setAppRoutes ({ { "msedge", "Music" }, { "spotify", "Music" } });
    rig.start();

    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return rig.captured (1004) && rig.movedAway (700); }));
    CHECK (rig.script.countMoves (1004, "ep-spdif") == 0); // Edge is captured where the user put it ...
    CHECK (rig.app (1004)->moveAway == MoveAway::None);    // ... and not taken for Flubsound's move
    auto journal = readJournal (rig.journalFile());
    REQUIRE (journal.size() == 1);
    CHECK (journal[0].executable == "spotify");

    // Unassigning Edge moves nothing: its device stays the user's. It keeps
    // playing to S/PDIF, now as an app Flubsound does not capture, so S/PDIF
    // may be in use (a receiver): the automatic choice moves Spotify on to
    // the first HDMI output; its own earlier device stays the one to put back.
    rig.routing->removeRoute ("msedge");
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return rig.movedAway (700) && rig.app (700)->movedTo == kHdmi1; }));
    waitForPasses (*rig.routing, rig.script, 1);
    for (const auto& move : rig.script.getMoves())
        CHECK (move.first != 1004u);
    CHECK (rig.script.getAppDevice ("msedge") == "ep-spdif");
    CHECK (rig.script.countMoves (700, "ep-hdmi1") == 1);
    CHECK (readJournal (rig.journalFile()).front().previousEndpoint.isEmpty());

    // The user sets Spotify back to the headset in the Volume mixer: Flubsound
    // forgets its move, does not fight it, and says so.
    const std::string headset ("ep-headset");
    placeApp (rig.script, "spotify", "ep-headset", &headset);
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        rig.routing->refresh();
        const auto* a = rig.app (700);
        return a != nullptr && a->moveAway == MoveAway::LeftToUser && a->doublingBlocked;
    }));
    waitForPasses (*rig.routing, rig.script, 2);
    CHECK (rig.script.countMoves (700, "ep-spdif") == 1);
    CHECK (rig.script.countMoves (700, "ep-hdmi1") == 1);
    CHECK (rig.script.countMoves (700, "") == 0);
    CHECK (! rig.journalFile().exists());
    CHECK (rig.routing->describeDoubling().contains ("Spotify.exe's output was changed after Flubsound moved it, so Flubsound leaves it there."));
    CHECK (rig.routing->getMoveAwayAction() == "Move again to " + juce::String (kHdmi1));

    // "Move again": moved, and the headset (now its own device) is what comes back.
    rig.routing->moveOriginalsAwayNow();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return rig.movedAway (700) && rig.script.countMoves (700, "ep-hdmi1") == 2; }));
    journal = readJournal (rig.journalFile());
    REQUIRE (journal.size() == 1);
    CHECK (journal[0].previousEndpoint == "ep-headset");
    rig.routing->shutdown();
    CHECK (rig.script.getAppDevice ("spotify") == "ep-headset");
}

TEST_CASE ("App: R4.5 a browser's audio process, several sessions and processes of one app, and a re-launch")
{
    const flubapptest::TempFolder temp;
    Rig rig (temp.file ("settings.xml"));
    // Edge plays through its audio service process (1004, two sessions) and a second process (1010).
    rig.script.setSessions ({ makeSession (1004, "msedge.exe", "ep-headset", 9001), makeSession (1004, "msedge.exe", "ep-headset", 9001, false),
                              makeSession (1010, "msedge.exe", "ep-headset", 9002) });
    rig.settings.setMoveOriginalAway (true);
    rig.settings.setAppRoutes ({ { "msedge", "Music" } });
    rig.start();

    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return rig.movedAway (1004) && rig.movedAway (1010); }));
    CHECK ((rig.script.getMoves() == std::vector<std::pair<uint32_t, std::string>> { { 1004, "ep-spdif" } })); // once per executable
    CHECK ((*rig.starts)[1004] == 1);
    CHECK ((*rig.starts)[1010] == 1);

    // Re-launched: the new process starts on S/PDIF (Windows kept the device
    // for the executable): captured without a new move; it carries the record.
    rig.script.setSessions ({ makeSession (1020, "msedge.exe", "ep-spdif", 9100) });
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        rig.routing->refresh();
        return rig.movedAway (1020);
    }));
    waitForPasses (*rig.routing, rig.script, 1);
    CHECK (rig.script.getMoves().size() == 1);
    auto journal = readJournal (rig.journalFile());
    REQUIRE (journal.size() == 1);
    CHECK (journal[0].processId == 1020);
    CHECK (journal[0].processStartTime == 9100);

    // Unassigned: put back through the running process.
    rig.routing->removeRoute ("msedge");
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return rig.script.countMoves (1020, "") == 1 && ! rig.journalFile().exists(); }));
    rig.routing->shutdown();
    CHECK (rig.script.getMoves().size() == 2);
}

TEST_CASE ("App: R4.5 the silent device goes away: an automatic choice falls back, a chosen one pauses and says why")
{
    const flubapptest::TempFolder temp;
    Rig rig (temp.file ("settings.xml"));
    rig.script.setSessions ({ makeSession (700, "Spotify.exe", "ep-headset", 7001) });
    rig.settings.setMoveOriginalAway (true);
    rig.settings.setAppRoutes ({ { "spotify", "Music" } });
    rig.start();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return rig.movedAway (700); }));

    // S/PDIF goes away (Windows keeps the per-app device and plays the app on the default meanwhile).
    setEndpoints (rig.script, { { "ep-headset", kHeadset }, { "ep-hdmi1", kHdmi1 }, { "ep-hdmi2", kHdmi2 } });
    placeApp (rig.script, "spotify", "ep-headset");
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        rig.routing->refresh();
        return rig.movedAway (700) && rig.app (700)->movedTo == kHdmi1;
    }));
    CHECK (rig.script.countMoves (700, "ep-hdmi1") == 1);
    CHECK (readJournal (rig.journalFile()).front().previousEndpoint.isEmpty()); // still its own device from before the first move

    // A chosen device: moved there. It goes away: paused, held back, explained; no move.
    rig.routing->setSilentEndpointChoice ({ "ep-hdmi2", kHdmi2 });
    CHECK (rig.settings.getSilentEndpointId() == "ep-hdmi2");
    CHECK (rig.settings.getSilentEndpointName() == kHdmi2);
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return rig.movedAway (700) && rig.app (700)->movedTo == kHdmi2; }));
    const auto movesBefore = rig.script.getMoves().size();
    setEndpoints (rig.script, { { "ep-headset", kHeadset }, { "ep-hdmi1", kHdmi1 } });
    placeApp (rig.script, "spotify", "ep-headset");
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        rig.routing->refresh();
        const auto* a = rig.app (700);
        return a != nullptr && a->moveAway == MoveAway::NoTarget && a->doublingBlocked;
    }));
    CHECK (rig.script.getMoves().size() == movesBefore);
    CHECK (rig.routing->describeDoubling().contains (juce::String (kHdmi2) + " (the silent device chosen in Settings > Routing) is not connected"));
    CHECK (rig.routing->describeMoveAway().startsWith ("On. Paused: " + juce::String (kHdmi2)));
    CHECK (readJournal (rig.journalFile()).size() == 1); // still Flubsound's move, to be put back

    // It comes back (Windows returns the app to it): moved again without a move.
    setEndpoints (rig.script, { { "ep-headset", kHeadset }, { "ep-hdmi1", kHdmi1 }, { "ep-hdmi2", kHdmi2 } });
    placeApp (rig.script, "spotify", "ep-hdmi2");
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        rig.routing->refresh();
        return rig.movedAway (700);
    }));
    CHECK (rig.script.getMoves().size() == movesBefore);
    rig.routing->shutdown();
    CHECK (rig.script.getAppDevice ("spotify").empty());
}

TEST_CASE ("App: R4.5 an app that keeps playing to the headset after the move stays held back; a failed move is reported and tried 3 times")
{
    const flubapptest::TempFolder temp;
    Rig rig (temp.file ("settings.xml"));
    rig.script.setSessions ({ makeSession (900, "Discord.exe", "ep-headset", 9900), makeSession (500, "cs2.exe", "ep-headset", 5500) });
    {
        const std::lock_guard<std::mutex> g (rig.script.lock);
        rig.script.pinned.insert (900);    // Discord opens its own output device
        rig.script.failMoves.insert (500); // cs2: the audio service refuses
    }
    rig.settings.setMoveOriginalAway (true);
    rig.settings.setAppRoutes ({ { "discord", "Chat" }, { "cs2", "Game" } });
    rig.start();

    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        const auto* discord = rig.app (900);
        const auto* cs2 = rig.app (500);
        return discord != nullptr && discord->moveAway == MoveAway::StillPlays && discord->doublingBlocked && cs2 != nullptr
               && cs2->moveAway == MoveAway::Failed;
    }));
    // Discord: moved once; the pass after the move (250 ms) sees it still on
    // the headset, so it is never captured (no moment of doubled sound) and is
    // held back with the reason.
    CHECK (rig.script.countMoves (900, "ep-spdif") == 1);
    CHECK ((*rig.starts)[900] == 0);
    CHECK (! rig.captured (900));
    CHECK (rig.routing->describeDoubling().contains ("Discord.exe still plays to " + juce::String (kHeadset) + " after Flubsound moved its own sound to "
                                                     + juce::String (kSpdif) + ": the app picks that device itself."));
    // cs2: the error is on the app, nothing is recorded.
    CHECK (rig.app (500)->error == "Flubsound could not move its own sound to " + juce::String (kSpdif) + ": Access is denied.");
    CHECK ((*rig.starts)[500] == 0);

    waitForPasses (*rig.routing, rig.script, 4);
    CHECK (rig.script.countMoves (900, "ep-spdif") == 1); // no move loop
    CHECK (rig.script.countMoves (500, "ep-spdif") == 3); // given up after 3 attempts
    CHECK (rig.app (500)->error.contains ("Access is denied."));
    const auto journal = readJournal (rig.journalFile());
    REQUIRE (journal.size() == 1);
    CHECK (journal[0].executable == "discord");
    CHECK (rig.routing->getMoveAwayAction() == "Move again to " + juce::String (kSpdif)); // retries the failed one

    // Discord restarts its playback: the new stream opens on its per-app device (S/PDIF): captured.
    {
        const std::lock_guard<std::mutex> g (rig.script.lock);
        rig.script.pinned.erase (900);
        rig.script.placeSessions ("discord", "ep-spdif");
    }
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        rig.routing->refresh();
        return rig.movedAway (900);
    }));
    CHECK (rig.script.countMoves (900, "ep-spdif") == 1);

    rig.routing->shutdown();
    CHECK (rig.script.countMoves (900, "") == 1);
}

TEST_CASE ("App: R4.5 nothing is moved where the option is not offered or not switched on")
{
    const flubapptest::TempFolder temp;
    {
        // A router that cannot move an app's own output (macOS, Linux, or Windows before 1803).
        Rig rig (temp.file ("a/settings.xml"));
        {
            const std::lock_guard<std::mutex> g (rig.script.lock);
            rig.script.outputMovable = false;
        }
        rig.script.setSessions ({ makeSession (700, "Spotify.exe", "ep-headset", 7001) });
        rig.settings.setMoveOriginalAway (true);
        rig.settings.setAppRoutes ({ { "spotify", "Music" } });
        rig.start();
        REQUIRE (flubapptest::pumpMessagesUntil ([&]
        {
            const auto* a = rig.app (700);
            return a != nullptr && a->doublingBlocked;
        }));
        waitForPasses (*rig.routing, rig.script, 2);
        CHECK (! rig.routing->canMoveOriginalAway());
        CHECK (rig.script.getMoves().empty());
        CHECK (rig.routing->getMoveAwayAction().isEmpty());
        CHECK (! rig.routing->describeDoubling().contains ("Flubsound can"));
        CHECK (! rig.routing->describeMoveAway().startsWith ("On"));
        rig.routing->shutdown();
        CHECK (rig.script.getMoves().empty());
    }
    {
        // Endpoint routing in effect (process capture unsupported): not offered either.
        Rig rig (temp.file ("b/settings.xml"));
        {
            const std::lock_guard<std::mutex> g (rig.script.lock);
            rig.script.movable = true;
        }
        rig.script.setSessions ({ makeSession (700, "Spotify.exe", "ep-headset", 7001) });
        rig.settings.setMoveOriginalAway (true);
        rig.settings.setAppRoutes ({ { "spotify", "Music" } });
        rig.routing = std::make_unique<AppRouting> (rig.host, rig.settings, std::make_unique<ScriptedRouter> (rig.script), false);
        rig.routing->setOutputDeviceSource ([] { return juce::String (kHeadset); });
        rig.routing->start();
        REQUIRE (flubapptest::pumpMessagesUntil ([&]
        {
            const auto* a = rig.app (700);
            return a != nullptr && a->routed;
        }));
        CHECK (rig.routing->getEffectiveMethod() == AppRouting::Method::EndpointRouting);
        CHECK (! rig.routing->canMoveOriginalAway());
        for (const auto& move : rig.script.getMoves())
            CHECK (move.second != "ep-spdif");
        rig.routing->shutdown();
    }
}

TEST_CASE ("App: R4.5 an app put back while its stream stays on the silent device is reported until it restarts its playback")
{
    const flubapptest::TempFolder temp;
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false; // started below with the scripted audio service
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = false;
    o.foregroundAppFactory = [] { return std::unique_ptr<flub::platform::ForegroundApp>(); };
    EngineController controller (o);
    auto& routing = controller.getRouting();
    flubapptest::useCountingCaptures (controller.getHost());
    AudioServiceScript script;
    scriptOwnerPc (script);
    // Spotify follows the default (the headset); VLC plays to an HDMI output and is captured there.
    script.setSessions ({ makeSession (700, "Spotify.exe", "ep-headset", 7001), makeSession (800, "vlc.exe", "ep-hdmi2", 8001) });
    routing.setRouter (std::make_unique<ScriptedRouter> (script), true);
    routing.setOutputDeviceSource ([] { return juce::String (kHeadset); });
    routing.setInputDeviceSource ([] { return juce::String(); });
    routing.setMoveOriginalAway (true);
    routing.setRoute ("spotify", "Music");
    routing.setRoute ("vlc", "Game");
    routing.start();
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        const auto* a = findRoutedApp (routing, 700);
        return a != nullptr && a->moveAway == MoveAway::Moved && a->captureId >= 0 && findRoutedApp (routing, 800)->captureId >= 0;
    }));

    // As measured on Windows 11: Spotify's open stream stays where it is
    // when its per-app device changes; only a new stream follows.
    {
        const std::lock_guard<std::mutex> g (script.lock);
        script.pinned.insert (700);
    }
    routing.removeRoute ("spotify");
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return ! routing.getPutBackNotes().isEmpty(); }));
    CHECK (script.getAppDevice ("spotify").empty()); // its own device (the default) is set back ...
    CHECK (findRoutedApp (routing, 700)->captureId < 0); // ... the capture stopped, and it still plays to S/PDIF: not heard
    const auto note = routing.describePutBack();
    CHECK (note == "Spotify.exe still plays to " + juce::String (kSpdif) + ", where Flubsound had moved its own sound, so it is not heard. "
                   + "Its own output device is set back: restart its playback (reload, or pause and play) to hear it again.");
    CHECK (routing.describeMoveAway().endsWith (note));
    CHECK (! routing.getJournalFile().exists()); // put back: nothing left to record

    ui::RoutingPanel panel (controller);
    panel.setSize (340, 900);
    panel.refreshRouting();
    CHECK (! panel.isShowingNoAppsProcessed()); // VLC is processed
    CHECK (panel.isShowingPutBack());
    CHECK (panel.getNotice() == "Not heard. " + note);

    // It restarts its playback: the new stream opens on its own device (the
    // default, the headset), so it is heard again and the note goes.
    {
        const std::lock_guard<std::mutex> g (script.lock);
        script.pinned.erase (700);
        script.placeSessions ("spotify", "ep-headset");
    }
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        routing.refresh();
        return routing.getPutBackNotes().isEmpty();
    }));
    panel.refreshRouting();
    CHECK (! panel.isShowingPutBack());
    CHECK (panel.getNotice().isEmpty());

    // Assigned again: moved again. Its stream then stays on S/PDIF at quit:
    // the put-back is logged with that, as nothing can show it afterwards.
    routing.setRoute ("spotify", "Music");
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        const auto* a = findRoutedApp (routing, 700);
        return a != nullptr && a->moveAway == MoveAway::Moved && a->captureId >= 0;
    }));
    {
        const std::lock_guard<std::mutex> g (script.lock);
        script.pinned.insert (700);
    }
    CapturingLogger log;
    controller.shutdown();
    CHECK (script.getAppDevice ("spotify").empty());
    CHECK (log.text().contains ("Routing: Spotify.exe's own output device was put back at quit, but it was still playing to " + juce::String (kSpdif)
                                + " (where Flubsound had moved it): it is not heard until it restarts its playback."));
}

TEST_CASE ("App: R4.5 moves are kept per executable file: two programs of one name each move; an updated app's new file is moved afresh")
{
    const flubapptest::TempFolder temp;
    const auto oldDiscord = temp.file ("Discord/app-1.0.1/Discord.exe"), newDiscord = temp.file ("Discord/app-1.0.2/Discord.exe");
    const auto gameA = temp.file ("Games A/game.exe"), gameB = temp.file ("Games B/game.exe");
    for (const auto& f : { oldDiscord, newDiscord, gameA, gameB })
        REQUIRE (f.create().wasOk());
    const auto pathOf = [] (const juce::File& f) { return f.getFullPathName().toStdString(); };

    Rig rig (temp.file ("settings.xml"));
    rig.script.setSessions ({ sessionAt (900, "Discord.exe", oldDiscord, "ep-headset", 9900), sessionAt (500, "game.exe", gameA, "ep-headset", 5500),
                              sessionAt (501, "game.exe", gameB, "ep-headset", 5501) });
    rig.settings.setMoveOriginalAway (true);
    rig.settings.setAppRoutes ({ { "discord", "Chat" }, { "game", "Game" } });
    rig.start();

    // Two programs named game.exe: Windows keeps one device per file, so each is moved (through its own process) and captured.
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return rig.movedAway (900) && rig.movedAway (500) && rig.movedAway (501); }));
    CHECK (rig.script.countMoves (500, "ep-spdif") == 1);
    CHECK (rig.script.countMoves (501, "ep-spdif") == 1);
    CHECK (rig.script.getAppDevice (pathOf (gameA)) == "ep-spdif");
    CHECK (rig.script.getAppDevice (pathOf (gameB)) == "ep-spdif");
    CHECK (readJournal (rig.journalFile()).size() == 3);

    // Discord updates itself: the old folder goes and the new file starts on
    // the headset (Windows has no device for that file). That is not the
    // user's change: the new file is moved, and the old file's record is
    // forgotten (its setting can never apply again).
    REQUIRE (oldDiscord.getParentDirectory().deleteRecursively());
    rig.script.setSessions ({ sessionAt (901, "Discord.exe", newDiscord, "ep-headset", 9901), sessionAt (500, "game.exe", gameA, "ep-spdif", 5500),
                              sessionAt (501, "game.exe", gameB, "ep-spdif", 5501) });
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        rig.routing->refresh();
        return rig.movedAway (901);
    }));
    CHECK (rig.script.countMoves (901, "ep-spdif") == 1);
    CHECK (rig.routing->describeDoubling().isEmpty()); // no "changed after Flubsound moved it"
    std::set<juce::String> paths;
    for (const auto& e : readJournal (rig.journalFile()))
        paths.insert (e.executablePath);
    CHECK ((paths == std::set<juce::String> { newDiscord.getFullPathName(), gameA.getFullPathName(), gameB.getFullPathName() }));

    // Unassigning "game" puts both files back, each through its own process.
    rig.routing->removeRoute ("game");
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return rig.script.countMoves (500, "") == 1 && rig.script.countMoves (501, "") == 1; }));
    CHECK (rig.script.getAppDevice (pathOf (gameA)).empty());
    CHECK (rig.script.getAppDevice (pathOf (gameB)).empty());
    rig.routing->shutdown();
    CHECK (rig.script.countMoves (901, "") == 1);
    CHECK (rig.script.countMoves (900, "") == 0);
    CHECK (! rig.journalFile().exists());
}

TEST_CASE ("App: R4.5 a crash during a re-point keeps the app as Flubsound's move, so it is put back later")
{
    const flubapptest::TempFolder temp;
    Rig rig (temp.file ("settings.xml"));
    // The journal of a run that crashed while re-pointing Spotify from S/PDIF
    // to HDMI 1: the entry was written (pending, from S/PDIF), the move was not made.
    REQUIRE (RouteJournal (rig.settings.getPropertiesFile().getFile().getSiblingFile ("route-journal.json"))
                 .write ({ { 700, 7001, "spotify", {}, "ep-hdmi1", {}, true, true, "ep-spdif" } }));
    REQUIRE (readJournal (rig.settings.getPropertiesFile().getFile().getSiblingFile ("route-journal.json")).front().movedFrom == "ep-spdif");
    rig.script.setSessions ({ makeSession (700, "Spotify.exe", "ep-spdif", 7001) });
    const std::string spdif ("ep-spdif");
    placeApp (rig.script, "spotify", "ep-spdif", &spdif);
    rig.settings.setMoveOriginalAway (true);
    rig.settings.setAppRoutes ({ { "spotify", "Music" } });
    rig.start();

    // Still Flubsound's move: the re-point is finished and confirmed.
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return rig.movedAway (700) && rig.app (700)->movedTo == kHdmi1; }));
    CHECK ((rig.script.getMoves() == std::vector<std::pair<uint32_t, std::string>> { { 700, "ep-hdmi1" } }));
    auto journal = readJournal (rig.journalFile());
    REQUIRE (journal.size() == 1);
    CHECK (! journal[0].pending);
    CHECK (journal[0].movedFrom.isEmpty());
    CHECK (journal[0].previousEndpoint.isEmpty());

    // Unassigned, then quit: its own device (the default) is back, nothing is left.
    rig.routing->removeRoute ("spotify");
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return rig.script.countMoves (700, "") == 1 && ! rig.journalFile().exists(); }));
    rig.routing->shutdown();
    CHECK (rig.script.getAppDevice ("spotify").empty());
    CHECK (rig.script.getMoves().size() == 2);
}

TEST_CASE ("App: R4.5 Tournament mode moves and puts back nothing; the journal keeps the move for the next start")
{
    const flubapptest::TempFolder temp;
    const auto settingsFile = temp.file ("settings.xml");
    const auto placeSpotifyOnSpdif = [] (AudioServiceScript& script)
    {
        script.setSessions ({ makeSession (700, "Spotify.exe", "ep-spdif", 7001) });
        const std::string spdif ("ep-spdif");
        placeApp (script, "spotify", "ep-spdif", &spdif);
    };
    {
        Rig rig (settingsFile);
        // An earlier run moved Spotify; it is no longer assigned, so outside
        // Tournament mode it would be put back at once.
        REQUIRE (RouteJournal (settingsFile.getSiblingFile ("route-journal.json")).write ({ { 700, 7001, "spotify", {}, "ep-spdif", {}, false, true, {} } }));
        placeSpotifyOnSpdif (rig.script);
        rig.settings.setMoveOriginalAway (true);
        rig.routing = std::make_unique<AppRouting> (rig.host, rig.settings, std::make_unique<ScriptedRouter> (rig.script), true);
        rig.routing->setOutputDeviceSource ([] { return juce::String (kHeadset); });
        rig.routing->setTournamentMode (true);
        rig.routing->start();
        bool waited = false;
        juce::Timer::callAfterDelay (300, [&waited] { waited = true; });
        REQUIRE (flubapptest::pumpMessagesUntil ([&waited] { return waited; }));
        rig.routing->shutdown();
        CHECK (rig.script.getEnumerations() == 0); // no process opened, no session touched
        CHECK (rig.script.getMoves().empty());
        const auto kept = readJournal (rig.journalFile());
        REQUIRE (kept.size() == 1);
        CHECK (kept[0].movedAway);
        CHECK (kept[0].endpoint == "ep-spdif");
        CHECK (rig.script.getAppDevice ("spotify") == "ep-spdif");
    }
    {
        // The next start, outside Tournament mode: put back.
        Rig rig (settingsFile);
        placeSpotifyOnSpdif (rig.script);
        rig.start();
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return rig.script.countMoves (700, "") == 1 && ! rig.journalFile().exists(); }));
        rig.routing->shutdown();
        CHECK (rig.script.getAppDevice ("spotify").empty());
    }
}

// =============================================================================
namespace
{
template <typename T>
T* findByTitle (juce::Component& root, const juce::String& title)
{
    for (auto* child : root.getChildren())
    {
        if (auto* t = dynamic_cast<T*> (child); t != nullptr && t->getTitle() == title)
            return t;
        if (auto* found = findByTitle<T> (*child, title))
            return found;
    }
    return nullptr;
}

juce::StringArray itemsOf (const juce::ComboBox& box)
{
    juce::StringArray items;
    for (int i = 0; i < box.getNumItems(); ++i)
        items.add (box.getItemText (i));
    return items;
}
} // namespace

TEST_CASE ("App UI: Settings > Routing has the move-away switch, the silent device list and the device input map")
{
    const flubapptest::TempFolder temp;
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = false;
    o.foregroundAppFactory = [] { return std::unique_ptr<flub::platform::ForegroundApp>(); };
    EngineController controller (o);
    auto& routing = controller.getRouting();
    flubapptest::useCountingCaptures (controller.getHost());
    AudioServiceScript script;
    scriptOwnerPc (script);
    script.setSessions ({ makeSession (700, "Spotify.exe", "ep-headset", 7001) });
    routing.setRouter (std::make_unique<ScriptedRouter> (script), true);
    routing.setOutputDeviceSource ([] { return juce::String (kHeadset); });
    routing.setRoute ("spotify", "Music");
    routing.start();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return routing.getKnownOutputEndpoints().size() == 4; }));

    ui::HotkeyHooks hooks;
    hooks.isSupported = [] { return false; };
    hooks.getFailures = [] { return juce::StringArray(); };
    hooks.reRegister = [] {};
    ui::SettingsDialog dialog (controller, hooks, [] (ui::MeterPalette) {}, ui::MeterPalette::Standard);
    dialog.setSize (900, 700);
    dialog.showPage (ui::SettingsDialog::Page::Routing);

    auto* method = findByTitle<juce::ComboBox> (dialog, "Per-app routing method");
    auto* moveAway = findByTitle<juce::ToggleButton> (dialog, "Move the app's own sound away automatically");
    auto* silent = findByTitle<juce::ComboBox> (dialog, "Silent device");
    REQUIRE (method != nullptr);
    REQUIRE (moveAway != nullptr);
    REQUIRE (silent != nullptr);
    CHECK (method->isVisible());
    CHECK (moveAway->isEnabled());
    CHECK (! moveAway->getToggleState());
    CHECK ((itemsOf (*silent)
            == juce::StringArray { "Automatic (" + juce::String (kSpdif) + ")", juce::String (kHeadset) + " (Flubsound's output)", kSpdif, kHdmi1, kHdmi2 }));
    CHECK (silent->getSelectedId() == 1);
    CHECK (! silent->isItemEnabled (2)); // the headset: Flubsound's output and the default

    // Switched on here: Spotify moves.
    moveAway->setToggleState (true, juce::sendNotificationSync);
    CHECK (routing.getMoveOriginalAway());
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        const auto* a = findRoutedApp (routing, 700);
        return a != nullptr && a->moveAway == MoveAway::Moved && a->captureId >= 0;
    }));
    // A chosen device: HDMI 2 (persisted), and the app follows it.
    silent->setSelectedId (5, juce::sendNotificationSync);
    CHECK (controller.getSettings().getSilentEndpointId() == "ep-hdmi2");
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return script.getAppDevice ("spotify") == "ep-hdmi2"; }));
    // Back to Automatic: a fresh pick, S/PDIF again.
    silent->setSelectedId (1, juce::sendNotificationSync);
    CHECK (controller.getSettings().getSilentEndpointId().isEmpty());
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return script.getAppDevice ("spotify") == "ep-spdif"; }));

    // The device input map (R4.6): one first-channel choice per strip.
    auto* game = findByTitle<juce::ComboBox> (dialog, "Input map: Game");
    auto* music = findByTitle<juce::ComboBox> (dialog, "Input map: Music");
    REQUIRE (game != nullptr);
    REQUIRE (music != nullptr);
    CHECK (game->getSelectedId() == 1); // "Not fed": no map
    CHECK (game->getItemText (0) == "Not fed");
    CHECK (game->getItemText (1) == "Inputs 1 - 8"); // Game is 7.1
    CHECK (music->getItemText (9) == "Inputs 9 - 10");
    music->setSelectedId (2 + 8, juce::sendNotificationSync); // from input 9
    CHECK (controller.getSettings().getDeviceInputMap() == "Music=8");
    // Automatic with no device open: the device input is not processed, so the saved map is not used, and the page says so.
    CHECK (controller.getHost().getDeviceInputMap()[1] == -1);
    CHECK (ui::SettingsDialog::describeInputMap (controller).startsWith ("Device input is not processed now"));
    CHECK (ui::SettingsDialog::describeInputMap (controller).contains (": the map is not used."));
    controller.setDeviceInputMode (AppSettings::DeviceInputMode::Off);
    CHECK (ui::SettingsDialog::describeInputMap (controller).startsWith ("Device input is off: the map is not used."));
    controller.setDeviceInputMode (AppSettings::DeviceInputMode::On);
    CHECK (ui::SettingsDialog::describeInputMap (controller).startsWith ("The map is in use"));
    CHECK (controller.getHost().getDeviceInputMap()[1] == 8);
    CHECK (controller.getHost().getDeviceInputMap()[0] == -1); // the map replaces "Input feeds strip"

    juce::TextButton* fillButton = nullptr;
    juce::TextButton* clearButton = nullptr;
    std::function<void (juce::Component&)> findButtons = [&] (juce::Component& c)
    {
        for (auto* child : c.getChildren())
        {
            if (auto* b = dynamic_cast<juce::TextButton*> (child))
            {
                if (b->getButtonText() == "Fill in one after another")
                    fillButton = b;
                if (b->getButtonText() == "Clear map")
                    clearButton = b;
            }
            findButtons (*child);
        }
    };
    findButtons (dialog);
    REQUIRE (fillButton != nullptr);
    REQUIRE (clearButton != nullptr);
    fillButton->triggerClick();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return controller.getSettings().getDeviceInputMap() == "Game=0;Music=8;Chat=10;System=12"; }, 2000));
    const auto map = controller.getHost().getDeviceInputMap();
    CHECK (map[0] == 0);
    CHECK (map[1] == 8);
    CHECK (map[2] == 10);
    CHECK (map[3] == 12);
    CHECK (game->getSelectedId() == 2);
    clearButton->triggerClick();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return controller.getSettings().getDeviceInputMap().isEmpty(); }, 2000));
    CHECK (controller.getHost().getDeviceInputMap()[0] == 0); // "Input feeds strip" (Game) again

    controller.shutdown();
}

TEST_CASE ("App: R4.6 device input map text: parse, format and the consecutive layout")
{
    const juce::StringArray strips { "Game", "Music", "Chat", "System" };
    CHECK ((EngineController::parseDeviceInputMap ("Game=0;Music=8;Chat=10;System=12", strips) == std::vector<int> { 0, 8, 10, 12 }));
    CHECK ((EngineController::parseDeviceInputMap (" music = 2 , Bogus=4;Chat=x;System=;Game=-1", strips) == std::vector<int> { -1, 2, -1, -1 }));
    CHECK ((EngineController::parseDeviceInputMap ({}, strips) == std::vector<int> { -1, -1, -1, -1 }));
    CHECK (EngineController::formatDeviceInputMap ({ 0, -1, 10, 12 }, strips) == "Game=0;Chat=10;System=12");
    CHECK (EngineController::formatDeviceInputMap ({ -1, -1, -1, -1 }, strips).isEmpty());
    CHECK ((EngineController::consecutiveInputMap ({ 8, 2, 2, 2 }) == std::vector<int> { 0, 8, 10, 12 }));
    CHECK ((EngineController::consecutiveInputMap ({ 2, 0, 1 }) == std::vector<int> { 0, 2, 3 }));
}
