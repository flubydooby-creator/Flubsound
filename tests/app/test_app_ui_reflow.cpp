// App-level tests: the small-window reflow, the relevance-ordered rack, the
// tray flyout and the protection readouts (docs/11 E39, E06 / E07).
//
// * Header below 1100 px: the strip buttons become a strip menu, the copy
//   button and the latency / CPU readout go into an overflow menu, the
//   wordmark leaves the logo; at 1100 px and wider nothing changes.
// * Advanced view at 800 x 560 and 1093 x 614: the routing panel leaves the
//   row and opens as a drawer (Escape closes it), the waveform history is
//   left out below 700 px of height; the window's minimum is 800 x 560.
// * The rack: relevance order per mode and strip, the Noise Gate card only
//   in the Quality latency profile, the virtualiser first on a 7.1 strip in
//   Gaming mode and last on a stereo strip.
// * QuickControls (the tray flyout): Boost, the preset stepper, Bypass and
//   "open the window" act on the selected strip; the layout fits.
// * Protection readouts: the governor chip's tooltip at Normal quotes the
//   budgets the chain publishes (not Off's), names the new reasons; the
//   loudness panel's PROTECTION section formats PLR and brightness and shows
//   the measured loop's readings after a render at Normal.
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "engine/TestSignalGenerator.h"
#include "shell/MainWindow.h"
#include "shell/ScreenshotDriver.h"
#include "ui/BoostPanel.h"
#include "ui/LoudnessPanel.h"
#include "ui/MainComponent.h"
#include "ui/ModuleRack.h"
#include "ui/QuickControls.h"

#include "flub/engine/Parameters.h"
#include "flub/engine/Protection.h"

#include <iostream>

using namespace flub::app;
using namespace flub::param;
using View = AppSettings::MainView;

namespace
{
EngineController::Options headlessOptions (const flubapptest::TempFolder& temp)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = false;
    o.foregroundAppFactory = [] { return std::unique_ptr<flub::platform::ForegroundApp>(); };
    return o;
}

juce::StringArray shownIds (ui::ModuleRack& rack)
{
    juce::StringArray ids;
    for (auto* card : rack.getShownCards())
        ids.add (card->getDescriptor().id);
    return ids;
}
} // namespace

TEST_CASE ("App UI: below 1100 px the header folds the strips into a menu and the copy button and readout into an overflow menu (E39)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    ui::HeaderBar header (controller);

    for (const int w : { 800, 1093 })
    {
        header.setSize (w, 56);
        CHECK (header.isNarrow());
        CHECK (header.getStripBox().isVisible());
        CHECK (header.getStripBox().getNumItems() == controller.getNumStrips());
        CHECK (header.getOverflowButton().isVisible());
        CHECK (header.getLocalBounds().contains (header.getOverflowButton().getBounds()));
        CHECK (header.getLocalBounds().contains (header.getPresetBox().getBounds()));
        CHECK (header.getPresetBox().getWidth() >= 120);
        CHECK (header.getBankButton (Bank::A).getWidth() >= 24);
    }
    // The strip menu selects the strip.
    const int music = controller.findStrip ("Music");
    header.getStripBox().setSelectedId (music + 1, juce::sendNotificationSync);
    CHECK (controller.getSelectedStrip() == music);

    header.setSize (1100, 56);
    CHECK (! header.isNarrow());
    CHECK (! header.getStripBox().isVisible());
    CHECK (! header.getOverflowButton().isVisible());
    CHECK (MainWindow::kMinWidth == 800);
    CHECK (MainWindow::kMinHeight == 560);
}

TEST_CASE ("App UI: the Advanced view at 800 x 560 moves the routing panel into a drawer and leaves the history out (E39)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    controller.getPresetManager().setUserPresetFolder (temp.file ("Presets"));
    ui::MainComponent main (controller);
    main.setView (View::Advanced, false);

    main.setSize (800, 560);
    CHECK (! main.getRoutingPanel().isVisible());
    int historyVisible = 0;
    for (auto* c : main.getAdvancedOnlyComponents())
        historyVisible += c->isVisible() ? 1 : 0;
    CHECK (historyVisible == 4); // analyser, rack, meters, loudness (no routing, no history)
    CHECK (main.getRack().getHeight() >= 150);
    CHECK (main.getLoudnessPanel().getWidth() >= 200);

    main.setRoutingDrawerOpen (true);
    CHECK (main.getRoutingPanel().isVisible());
    CHECK (main.getLocalBounds().contains (main.getRoutingPanel().getBounds()));
    CHECK (main.getRoutingPanel().getWidth() >= 228);
    CHECK (main.getRoutingPanel().getY() >= main.getHeader().getBottom());
    CHECK (main.keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)));
    CHECK (! main.isRoutingDrawerOpen());
    CHECK (! main.getRoutingPanel().isVisible());

    // A wide window puts the panel back in its column (and forgets the drawer).
    main.setRoutingDrawerOpen (true);
    main.setSize (1280, 820);
    CHECK (! main.isRoutingDrawerOpen());
    CHECK (main.getRoutingPanel().isVisible());
    CHECK (main.getRoutingPanel().getX() < main.getRack().getX());
    for (auto* c : main.getAdvancedOnlyComponents())
        CHECK (c->isVisible());
}

TEST_CASE ("App UI: the rack orders its cards by relevance and shows the Noise Gate only in the Quality profile (E39)")
{
    using R = ui::ModuleRack;
    const auto music = R::relevanceOrder (ModeValue::Music, 2, false);
    CHECK (juce::StringArray (music.data(), static_cast<int> (music.size())).joinIntoString (",") == "eq,bass,clarity,spatial,sat,comp,max,dyneq,virt");
    const auto gaming71 = R::relevanceOrder (ModeValue::Gaming, 8, true);
    CHECK (juce::StringArray (gaming71.data(), static_cast<int> (gaming71.size())).joinIntoString (",")
           == "virt,dyneq,clarity,spatial,comp,bass,max,eq,sat,gate");
    const auto music71 = R::relevanceOrder (ModeValue::Music, 8, false);
    CHECK (music71[1] == "virt");
    for (const auto& order : { music, gaming71, music71, R::relevanceOrder (ModeValue::Gaming, 2, false) })
    {
        // Every module once (the gate only in Quality).
        CHECK (order.size() == ui::ModuleDescriptor::all().size() - (std::find (order.begin(), order.end(), "gate") == order.end() ? 1u : 0u));
        for (const auto& d : ui::ModuleDescriptor::all())
            CHECK (std::count (order.begin(), order.end(), d.id) <= 1);
    }

    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    const int musicStrip = controller.findStrip ("Music"), game = controller.findStrip ("Game");
    controller.setSelectedStrip (musicStrip);
    controller.setMode (ModeValue::Music);
    controller.setLatencyProfile (LatencyProfileValue::Balanced);
    ui::ModuleRack rack (controller);
    rack.setSize (1000, 180);
    rack.updateFromEngine();
    auto ids = shownIds (rack);
    CHECK (ids[0] == "eq");
    CHECK (! ids.contains ("gate"));
    CHECK (ids[ids.size() - 1] == "virt");
    int x = -1;
    for (auto* card : rack.getShownCards()) // left to right in that order
    {
        CHECK (card->isVisible());
        CHECK (card->getX() > x);
        x = card->getX();
    }

    controller.setLatencyProfile (LatencyProfileValue::Quality);
    rack.updateFromEngine();
    ids = shownIds (rack);
    CHECK (ids[ids.size() - 1] == "gate");

    controller.setSelectedStrip (game);
    controller.setMode (ModeValue::Gaming);
    rack.updateFromEngine();
    ids = shownIds (rack);
    if (controller.getStripChannels (game) > 2)
        CHECK (ids[0] == "virt");
    CHECK (ids.contains ("gate"));
    controller.setLatencyProfile (LatencyProfileValue::Balanced);
    rack.updateFromEngine();
    CHECK (! shownIds (rack).contains ("gate"));
}

TEST_CASE ("App UI: the tray flyout's Boost, preset stepper, Bypass and open button act on the selected strip (E39)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    const int music = controller.findStrip ("Music");
    controller.setSelectedStrip (music);
    ui::QuickControls flyout (controller);
    CHECK (flyout.getWidth() == ui::QuickControls::kWidth);
    for (auto* child : flyout.getChildren())
        CHECK (flyout.getLocalBounds().contains (child->getBounds()));

    flyout.getBoostSlider().setValue (0.8, juce::sendNotificationSync);
    CHECK (std::abs (controller.getBoost() - 0.8f) < 1.0e-4f);
    CHECK (flyout.getBoostSlider().getTooltip().startsWith ("Boost Intensity: "));

    const auto before = controller.getCurrentPresetId();
    flyout.getNextButton().triggerClick();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return controller.getCurrentPresetId() != before; }, 2000));
    CHECK (flyout.getPresetText().startsWith (controller.getCurrentPresetName()));
    flyout.getPreviousButton().triggerClick();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return controller.getCurrentPresetId() == before; }, 2000));

    flyout.getBypassButton().triggerClick();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return ! controller.isEnabled(); }, 2000));
    CHECK (flyout.getBypassButton().getButtonText() == "Bypassed");
    flyout.getBypassButton().triggerClick();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return controller.isEnabled(); }, 2000));

    bool opened = false;
    flyout.onOpenWindow = [&opened] { opened = true; };
    flyout.getOpenButton().triggerClick();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return opened; }, 2000));

    // The screenshot driver shows it.
    ScreenshotDriver::Options o;
    juce::String error;
    CHECK (ScreenshotDriver::parseCommandLine ({ "--screenshot", "a.png", "--state", "quick-controls,ab-matched,abx,bypass,routing-drawer,governor-normal" },
                                               o, error));
    CHECK (error.isEmpty());
    CHECK (o.states.size() == 6);
}

TEST_CASE ("App UI: the protection readouts quote the budgets the chain runs with and show the measured loop at Normal (E06 / E07)")
{
    using G = flub::SafetyGovernor;
    ui::MeterSnapshot s;
    s.governorScale = 0.8f;
    s.governorState = static_cast<int> (G::State::BackingOff);
    s.governorReason = G::kReasonDynamics | G::kReasonTonal;
    s.governorStrength = static_cast<int> (flub::ProtectionStrength::Normal);
    s.governorGrBudgetDb = -4.0f;
    s.governorResidualBudgetDb = -30.0f;
    s.governorDriveResidualDb = -41.2f;
    s.governorPlrDb = 7.5f;
    s.governorPlrBudgetDb = 8.0f;
    s.tonalLiftDb = { 1.2f, 3.6f, 0.4f };
    auto r = ui::BoostPanel::describeGovernor (s, flub::ProtectionStrength::Normal);
    CHECK (r.text.contains ("dynamics + brightness"));
    CHECK (r.detail.contains ("(budget -4 dB)"));
    CHECK (! r.detail.contains ("(budget -6 dB)"));
    CHECK (r.detail.contains ("Audible distortion (weighted residual): -41 dB (budget -30 dB)"));
    CHECK (r.detail.contains ("Dynamics (PLR, 3 s): 7.5 dB (at least 8 dB)"));
    CHECK (r.detail.contains ("harsh +3.6 dB (budget +3)"));
    // At Off the fixed budgets, as before.
    s.governorStrength = 0;
    r = ui::BoostPanel::describeGovernor (s, flub::ProtectionStrength::Off);
    CHECK (r.detail.contains ("(budget -6 dB)"));
    CHECK (r.detail.contains ("Distortion (THD+N)"));

    CHECK (ui::LoudnessPanel::formatPlr (9.14f, 8.0f) == "9.1 / 8");
    CHECK (ui::LoudnessPanel::formatPlr (9.14f, 0.0f) == "9.1");
    CHECK (ui::LoudnessPanel::formatPlr (flub::MeterBus::governorNoReading, 8.0f) == "--");
    CHECK (ui::LoudnessPanel::formatBrightness ({ 1.2f, -0.4f, 0.0f }) == "+1.2 -0.4 +0.0");
    CHECK (ui::LoudnessPanel::formatBrightness ({ -160.0f, 0.0f, 0.0f }) == "--");

    // End to end: Club Loud at Boost 100 and protection Normal, 4 s of music.
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    flub::StripConfig one;
    one.name = "Music";
    controller.setStripLayout ({ one });
    juce::String error;
    REQUIRE (controller.loadPreset (*controller.getPresetManager().findByName ("Club Loud"), 0, error));
    controller.setBoost (1.0f, 0);
    controller.setProtectionStrength (flub::ProtectionStrength::Normal);
    TestSignalGenerator source (controller.getHost().getSampleRate());
    source.setProgramme (0, TestSignalGenerator::Programme::Music, 0.0f);
    const int total = static_cast<int> (4.0 * controller.getHost().getSampleRate());
    for (int done = 0; done < total; done += 512)
        controller.renderOffline (source, juce::jmin (512, total - done));
    ui::MeterSnapshot live;
    live.read (controller.getChain (0).meters());
    std::cerr << "    Club Loud, Boost 100, Normal: residual " << live.governorDriveResidualDb << " dB (budget " << live.governorResidualBudgetDb
              << "), PLR " << live.governorPlrDb << " dB, brightness " << ui::LoudnessPanel::formatBrightness (live.tonalLiftDb) << "\n";
    CHECK (live.governorStrength == static_cast<int> (flub::ProtectionStrength::Normal));
    CHECK (live.governorDriveResidualDb > -150.0f);
    CHECK (live.governorPlrDb < flub::MeterBus::governorNoReading * 0.5f);
    CHECK (ui::LoudnessPanel::formatBrightness (live.tonalLiftDb) != "--");

    ui::LoudnessPanel panel;
    panel.setSize (300, 520);
    panel.update (live, 0.1);
    (void) panel.createComponentSnapshot (panel.getLocalBounds());
    CHECK (panel.isShowingProtection());
    panel.setSize (260, 250); // too short: the section is left out, the rest keeps its rows
    (void) panel.createComponentSnapshot (panel.getLocalBounds());
    CHECK (! panel.isShowingProtection());
}
