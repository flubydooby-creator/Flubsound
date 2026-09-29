// App-level tests: docs/11 E47's doubling guard. A process capture of an app
// that still plays straight to the device Flubsound plays to would be heard
// twice (original + processed copy); such a capture is held back, reported in
// the routing panel (amber) with the fix, and allowed once the app plays to
// another device.
#include "RoutingTestFakes.h"

#include "engine/EngineController.h"
#include "settings/AppSettings.h"
#include "ui/RoutingPanel.h"

using namespace flub::app;
using flubapptest::AudioServiceScript;
using flubapptest::findRoutedApp;
using flubapptest::makeSession;
using flubapptest::ScriptedRouter;
using OutputEndpoint = flub::platform::AppAudioRouter::OutputEndpoint;

namespace
{
const char* const kHeadset = "Headset Earphone (Stealth 600X Gen 2 USB)";
const char* const kSpeakers = "Speakers (Realtek(R) Audio)";

/** The owner's setup: the headset (through its USB dongle) is both the
    system default and Flubsound's output; there are speakers too, and an
    E46-style Flubsound endpoint that is never offered as a spare. */
void scriptEndpoints (AudioServiceScript& script)
{
    const std::lock_guard<std::mutex> g (script.lock);
    script.movable = false; // a default Windows build: Automatic -> process capture
    script.endpoints = { { "ep-headset", kHeadset }, { "ep-speakers", kSpeakers }, { "ep-flub-game", "Flubsound Game (Flubsound Virtual Audio)" } };
}
} // namespace

TEST_CASE ("App: E47 output endpoint lookup undoes JUCE's duplicate-name numbering")
{
    using Router = flub::platform::AppAudioRouter;
    const std::vector<OutputEndpoint> endpoints { { "id-default", "Headphones (Xbox Wireless Adapter)" },
                                                  { "id-a", "Speakers (Realtek(R) Audio)" },
                                                  { "id-b", "Headphones (Xbox Wireless Adapter)" },
                                                  { "id-c", "Headphones (Xbox Wireless Adapter)" } };
    CHECK (Router::matchOutputDeviceName (endpoints, "Headphones (Xbox Wireless Adapter)") == "id-default");
    CHECK (Router::matchOutputDeviceName (endpoints, "Headphones (Xbox Wireless Adapter) (2)") == "id-b");
    CHECK (Router::matchOutputDeviceName (endpoints, "Headphones (Xbox Wireless Adapter) (3)") == "id-c");
    CHECK (Router::matchOutputDeviceName (endpoints, "Speakers (Realtek(R) Audio)") == "id-a");
    CHECK (Router::matchOutputDeviceName (endpoints, "speakers (realtek(r) audio)").empty()); // JUCE's numbering is case-sensitive, so is the match
    CHECK (Router::matchOutputDeviceName (endpoints, "ASIO4ALL v2").empty());
    CHECK (Router::matchOutputDeviceName (endpoints, "").empty());
    CHECK (Router::matchOutputDeviceName ({}, "Speakers (Realtek(R) Audio)").empty());

    // The default implementation of findOutputEndpointId uses listOutputEndpoints.
    AudioServiceScript script;
    scriptEndpoints (script);
    ScriptedRouter router (script);
    CHECK (router.findOutputEndpointId (kHeadset) == "ep-headset");
    CHECK (router.findOutputEndpointId ("CABLE Input (VB-Audio Virtual Cable)").empty());

    // An app plays to an endpoint through any active session, else its current one.
    auto s = makeSession (1, "game.exe", "ep-speakers");
    s.activeEndpointIds = { "ep-speakers", "ep-headset" };
    CHECK (AppRouting::playsToEndpoint (s, "ep-headset"));
    CHECK (! AppRouting::playsToEndpoint (s, ""));
    const auto idle = makeSession (2, "idle.exe", "ep-headset", 0, false);
    CHECK (AppRouting::playsToEndpoint (idle, "ep-headset"));
    CHECK (! AppRouting::playsToEndpoint (idle, "ep-speakers"));
}

TEST_CASE ("App: E47 a capture on the output's endpoint is held back, another endpoint is allowed")
{
    const flubapptest::TempFolder temp;
    AppSettings settings (temp.file ("settings.xml"), false);
    AudioEngineHost host;
    auto starts = flubapptest::useCountingCaptures (host);

    AudioServiceScript script;
    scriptEndpoints (script);
    script.setSessions ({ makeSession (500, "cs2.exe", "ep-headset"), makeSession (700, "Spotify.exe", "ep-speakers"),
                          makeSession (900, "Discord.exe", "ep-headset") });

    AppRouting routing (host, settings, std::make_unique<ScriptedRouter> (script), true);
    juce::String output (kHeadset);
    routing.setOutputDeviceSource ([&output] { return output; });
    int changes = 0;
    routing.onChanged = [&changes] { ++changes; };
    REQUIRE (routing.getEffectiveMethod() == AppRouting::Method::ProcessCapture);
    routing.setRoute ("cs2", "Game");
    routing.setRoute ("spotify", "Music");
    routing.start();

    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        const auto* cs2 = findRoutedApp (routing, 500);
        const auto* spotify = findRoutedApp (routing, 700);
        return cs2 != nullptr && cs2->doublingBlocked && spotify != nullptr && spotify->captureId >= 0;
    }));
    const auto* cs2 = findRoutedApp (routing, 500);
    CHECK (cs2->playsToOutput);
    CHECK (cs2->captureId == -1);
    CHECK (cs2->error.isEmpty());
    CHECK ((*starts)[500] == 0); // never started: no moment of doubled audio
    CHECK ((*starts)[700] == 1); // plays to the speakers: captured
    CHECK (! findRoutedApp (routing, 700)->doublingBlocked);
    const auto* discord = findRoutedApp (routing, 900);
    CHECK (discord->playsToOutput);     // plays to the headset too...
    CHECK (! discord->doublingBlocked); // ...but is not assigned: nothing is held back
    CHECK (routing.getProcessedAppCount() == 1);

    CHECK ((routing.getOutputEndpoint() == OutputEndpoint { "ep-headset", kHeadset }));
    CHECK ((routing.getSpareEndpoints() == std::vector<OutputEndpoint> { { "ep-speakers", kSpeakers } })); // not the output, not Flubsound's own
    CHECK (routing.getDoublingBlockedApps() == juce::StringArray { "cs2.exe" });
    const auto text = routing.describeDoubling();
    CHECK (text.startsWith ("cs2.exe plays straight to " + juce::String (kHeadset)));
    CHECK (text.contains ("Volume mixer"));
    CHECK (text.contains ("Devices available: " + juce::String (kSpeakers) + "."));

    // The user applies the fix (cs2 -> speakers in Windows' volume mixer): captured now.
    script.setSessions ({ makeSession (500, "cs2.exe", "ep-speakers"), makeSession (700, "Spotify.exe", "ep-speakers"),
                          makeSession (900, "Discord.exe", "ep-headset") });
    routing.refresh();
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        const auto* a = findRoutedApp (routing, 500);
        return a != nullptr && a->captureId >= 0;
    }));
    CHECK (! findRoutedApp (routing, 500)->doublingBlocked);
    CHECK (routing.describeDoubling().isEmpty());
    CHECK ((*starts)[500] == 1);

    // Flubsound's output moves to the speakers (Spotify and cs2 play there):
    // both running captures stop; the headset is the spare device now.
    output = kSpeakers;
    routing.refresh();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return routing.getProcessedAppCount() == 0 && host.getCaptures().empty(); }));
    CHECK (findRoutedApp (routing, 500)->doublingBlocked);
    CHECK (findRoutedApp (routing, 700)->doublingBlocked);
    CHECK (routing.getDoublingBlockedApps().size() == 2);
    CHECK (routing.describeDoubling().startsWith ("cs2.exe and Spotify.exe play straight to " + juce::String (kSpeakers)));
    CHECK ((routing.getSpareEndpoints() == std::vector<OutputEndpoint> { { "ep-headset", kHeadset } }));

    // An output the router cannot name (an ASIO driver): the guard is off.
    output = "ASIO4ALL v2";
    routing.refresh();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return routing.getProcessedAppCount() == 2; }));
    CHECK (routing.getOutputEndpoint().id.empty());
    CHECK (routing.getDoublingBlockedApps().isEmpty());
    routing.shutdown();
    CHECK (host.getCaptures().empty());
}

TEST_CASE ("App: E47 the routing panel shows the amber \"original also audible\" state and the fix")
{
    const flubapptest::TempFolder temp;
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false; // started below with the fake router
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = false;
    o.foregroundAppFactory = [] { return std::unique_ptr<flub::platform::ForegroundApp>(); };
    EngineController controller (o);
    auto& routing = controller.getRouting();
    flubapptest::useCountingCaptures (controller.getHost());

    AudioServiceScript script;
    scriptEndpoints (script);
    script.setSessions ({ makeSession (500, "cs2.exe", "ep-headset"), makeSession (700, "Spotify.exe", "ep-speakers") });
    routing.setRouter (std::make_unique<ScriptedRouter> (script), true);
    routing.setOutputDeviceSource ([] { return juce::String (kHeadset); });
    routing.setRoute ("cs2", "Game");
    routing.start();

    ui::RoutingPanel panel (controller);
    panel.setSize (340, 900);

    // Only cs2 is assigned, and it is held back: nothing is processed, so the
    // red state explains why with the doubling text.
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        const auto* a = findRoutedApp (routing, 500);
        return a != nullptr && a->doublingBlocked;
    }));
    panel.refreshRouting();
    CHECK (panel.isShowingNoAppsProcessed());
    CHECK (! panel.isShowingDoubling());
    CHECK (panel.getNotice() == "No apps are being processed. " + routing.describeDoubling());
    const auto gameStrip = panel.getStripDescription (0);
    CHECK (gameStrip.contains ("cs2: original also audible"));

    // Spotify is captured as well: no red state, the amber notice instead.
    routing.setRoute ("spotify", "Music");
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return routing.getProcessedAppCount() == 1; }));
    panel.refreshRouting();
    CHECK (! panel.isShowingNoAppsProcessed());
    CHECK (panel.isShowingDoubling());
    CHECK (panel.getNotice() == "Original also audible. " + routing.describeDoubling());
    CHECK (panel.getDescription() == panel.getNotice()); // screen readers get it too
    CHECK (panel.getStripDescription (1).contains ("spotify: playing"));

    // Fixed: the amber state goes.
    script.setSessions ({ makeSession (500, "cs2.exe", "ep-speakers"), makeSession (700, "Spotify.exe", "ep-speakers") });
    routing.refresh();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return routing.getProcessedAppCount() == 2; }));
    panel.refreshRouting();
    CHECK (! panel.isShowingDoubling());
    CHECK (panel.getNotice().isEmpty());
    CHECK (! panel.getStripDescription (0).contains ("original also audible"));

    controller.shutdown();
}
