// App-level tests: the Simple view (docs/11 E39, Phase 3 app track).
//
// * AppSettings::getMainView: Simple by default (also for an absent or
//   damaged value), the last choice kept across a restart.
// * MainComponent: starts in the saved view; the header's view button and the
//   Simple panel's Advanced view button switch views and save the choice; the
//   Simple view hides the routing panel, analyser, rack, meters and history
//   and shows the Boost panel (Simple layout) and the status row.
// * Layout: at 800 x 560 (the minimum since the reflow, docs/11 E39),
//   1093 x 614 (a 1366 x 768 laptop at 125 %), 1100 x 700 (with and without
//   banners), 1280 x 820, 1920 x 1080 and 2560 x 1440, in both views, no
//   visible child leaves the window, the header's controls do not overlap,
//   and the Simple controls (mode, preset, Boost dial, the five macros, the
//   headset status, the loudness meter, the Advanced view button) are shown
//   with a usable size and do not overlap.
// * The E38 active-now chips in the Simple view: wrapped over up to three
//   rows (BoostPanel::layoutChips), so Signature at Boost 55 shows every chip
//   where the Advanced view's single row counts some as "+N".
// * SimpleStatusPanel's readouts (headset status, loudness change) and the
//   screenshot driver's --view option.
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "engine/TestSignalGenerator.h"
#include "shell/ScreenshotDriver.h"
#include "ui/BoostPanel.h"
#include "ui/MainComponent.h"
#include "ui/SimpleStatusPanel.h"

#include "flub/engine/Parameters.h"

#include <cmath>
#include <iostream>
#include <vector>

using namespace flub::app;
using namespace flub::param;
using View = AppSettings::MainView;

namespace
{
EngineController::Options headlessOptions (const flubapptest::TempFolder& temp, bool persist = false)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = persist;
    o.foregroundAppFactory = [] { return std::unique_ptr<flub::platform::ForegroundApp>(); };
    return o;
}

/** Every visible child lies inside its parent (the window for the panels),
    recursively; a Viewport's content scrolls by design, so only the
    Viewport itself is checked. */
void checkInside (juce::Component& c, const juce::String& path)
{
    for (auto* child : c.getChildren())
    {
        if (! child->isVisible())
            continue;
        const auto name = path + "/" + (child->getTitle().isNotEmpty() ? child->getTitle() : child->getName());
        const bool inside = c.getLocalBounds().contains (child->getBounds());
        if (! inside)
            std::cerr << "    outside its parent: " << name << " at " << child->getBounds().toString() << " in "
                      << c.getLocalBounds().toString() << "\n";
        CHECK (inside);
        if (dynamic_cast<juce::Viewport*> (child) == nullptr)
            checkInside (*child, name);
    }
}

bool shows (juce::Component& root, juce::Component& c, int minW, int minH)
{
    const auto r = root.getLocalArea (&c, c.getLocalBounds());
    return c.isVisible() && r.getWidth() >= minW && r.getHeight() >= minH && root.getLocalBounds().contains (r);
}

/** Renders `seconds` of the test programme through the engine in 512-sample blocks. */
void render (EngineController& c, TestSignalGenerator& source, double seconds)
{
    const int total = static_cast<int> (seconds * c.getHost().getSampleRate());
    for (int done = 0; done < total; done += 512)
        c.renderOffline (source, juce::jmin (512, total - done));
}
} // namespace

// =============================================================================
// Settings
// =============================================================================
TEST_CASE ("App UI: the main view is Simple by default and the last choice is kept across a restart (E39)")
{
    const flubapptest::TempFolder temp;
    {
        AppSettings settings (temp.file ("s.settings"), true);
        CHECK (settings.getMainView() == View::Simple);
        settings.setMainView (View::Advanced);
        CHECK (settings.getMainView() == View::Advanced);
        settings.save();
    }
    {
        AppSettings settings (temp.file ("s.settings"), true);
        CHECK (settings.getMainView() == View::Advanced);
        settings.getPropertiesFile().setValue ("ui.view", "sideways"); // damaged: the default
        CHECK (settings.getMainView() == View::Simple);
        settings.setMainView (View::Simple);
        CHECK (settings.getPropertiesFile().getValue ("ui.view") == "simple");
    }
}

// =============================================================================
// The window
// =============================================================================
TEST_CASE ("App UI: MainComponent starts in the saved view; the view buttons switch it and save the choice (E39)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    {
        ui::MainComponent main (controller);
        main.setSize (1280, 820);
        CHECK (main.getView() == View::Simple);
        CHECK (main.getSimplePanel().isVisible());
        CHECK (main.getBoostPanel().getLayout() == ui::BoostPanel::Layout::Simple);
        for (auto* c : main.getAdvancedOnlyComponents())
            CHECK (! c->isVisible());
        CHECK (main.getHeader().getViewButton().getTitle() == "Advanced view");

        // The Simple panel's button: Advanced, saved.
        main.getSimplePanel().getAdvancedButton().triggerClick();
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return main.getView() == View::Advanced; }, 2000));
        CHECK (controller.getSettings().getMainView() == View::Advanced);
        CHECK (! main.getSimplePanel().isVisible());
        CHECK (main.getBoostPanel().getLayout() == ui::BoostPanel::Layout::Standard);
        for (auto* c : main.getAdvancedOnlyComponents())
            CHECK (c->isVisible());
        CHECK (main.getHeader().getViewButton().getTitle() == "Simple view");
    }
    {
        // A new window (the next start) opens in Advanced; the header's button goes back.
        ui::MainComponent main (controller);
        main.setSize (1280, 820);
        CHECK (main.getView() == View::Advanced);
        main.getHeader().getViewButton().triggerClick();
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return main.getView() == View::Simple; }, 2000));
        CHECK (controller.getSettings().getMainView() == View::Simple);
        // A programmatic switch that is not the user's (screenshots) is not saved.
        main.setView (View::Advanced, false);
        CHECK (controller.getSettings().getMainView() == View::Simple);
    }
}

TEST_CASE ("App UI: in both views and at 800 x 560 .. 2560 x 1440 no control leaves the window; the Simple controls are shown and do not overlap (E39)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    controller.getPresetManager().setUserPresetFolder (temp.file ("Presets"));
    ui::MainComponent main (controller);

    const auto checkSize = [&main] (int w, int h)
    {
        main.setSize (w, h);
        std::cerr << "    window " << w << "x" << h << (main.getView() == View::Simple ? " simple" : " advanced") << "\n";
        checkInside (main, "main");
        // The header's own controls never overlap, whatever the width.
        std::vector<juce::Component*> headerControls;
        for (auto* child : main.getHeader().getChildren())
            if (child->isVisible())
                headerControls.push_back (child);
        for (size_t i = 0; i < headerControls.size(); ++i)
            for (size_t j = i + 1; j < headerControls.size(); ++j)
            {
                const bool overlap = headerControls[i]->getBounds().intersects (headerControls[j]->getBounds());
                if (overlap)
                    std::cerr << "    header overlap: " << headerControls[i]->getTitle() << " / " << headerControls[j]->getTitle() << "\n";
                CHECK (! overlap);
            }
        if (main.getView() != View::Simple)
            return;
        auto& boost = main.getBoostPanel();
        auto& simple = main.getSimplePanel();
        CHECK (shows (main, main.getHeader(), w, 56));
        CHECK (shows (main, boost, 600, 150));
        CHECK (shows (main, simple, 600, 200));
        CHECK (! boost.getBounds().intersects (simple.getBounds()));
        CHECK (boost.getBounds().getY() >= main.getHeader().getBottom());
        const auto dial = boost.getDialBounds();
        CHECK (dial.getWidth() >= 104);
        CHECK (dial.getWidth() == dial.getHeight());
        int knobs = 0;
        for (auto* child : boost.getChildren())
            if (auto* knob = dynamic_cast<ui::ParamKnob*> (child); knob != nullptr && shows (main, *knob, 60, 90))
                ++knobs;
        CHECK (knobs == 5);
        CHECK (boost.getChipRowCount() >= 1);
        CHECK (shows (main, simple.getAdvancedButton(), 100, 24));
    };

    for (const auto view : { View::Simple, View::Advanced })
    {
        main.setView (view, false);
        checkSize (800, 560);
        checkSize (1093, 614);
        checkSize (1100, 700);
        checkSize (1280, 820);
        checkSize (1920, 1080);
        checkSize (2560, 1440);
    }

    // The minimum size with the advice banner and a notice (a headset and a
    // preset made for another profile): everything still fits.
    controller.simulateOutputDevice ("Headphones (Stealth 700 Gen 2 MAX)", controller.getHost().getSampleRate(), 2);
    juce::String error;
    const int music = controller.findStrip ("Music");
    controller.setSelectedStrip (music);
    REQUIRE (controller.loadPreset (*controller.getPresetManager().findByName ("Audiophile Subtle"), music, error));
    REQUIRE (main.getNoticeBar().shouldShow());
    for (const auto view : { View::Simple, View::Advanced })
    {
        main.setView (view, false);
        checkSize (800, 560);
        checkSize (1093, 614);
        checkSize (1100, 700);
    }
    // Three chip rows in the Simple view at the default size.
    main.setView (View::Simple, false);
    main.setSize (1280, 820);
    CHECK (main.getBoostPanel().getChipRowCount() == 3);
}

// =============================================================================
// Active-now chips in the Simple view (E38 in E39)
// =============================================================================
TEST_CASE ("App UI: BoostPanel::layoutChips wraps chips over the rows and counts what does not fit as +N (E39 / E38)")
{
    using BP = ui::BoostPanel;
    const std::vector<juce::Rectangle<float>> one { { 0.0f, 0.0f, 200.0f, 20.0f } };
    const std::vector<juce::Rectangle<float>> two { { 0.0f, 0.0f, 200.0f, 20.0f }, { 0.0f, 22.0f, 200.0f, 20.0f } };

    // 3 x 60 px + 2 gaps = 190 px: one row holds all.
    auto l = BP::layoutChips ({ 60.0f, 60.0f, 60.0f }, one);
    CHECK (l.pills.size() == 3);
    CHECK (l.hidden == 0);
    CHECK (l.pills[2].getRight() <= 200.0f);

    // Four: on a single (last) row the third leaves no room for "+N".
    l = BP::layoutChips ({ 60.0f, 60.0f, 60.0f, 60.0f }, one);
    CHECK (l.pills.size() == 2);
    CHECK (l.hidden == 2);
    CHECK (l.more.getX() >= l.pills[1].getRight());

    // Two rows: the fourth wraps, none hidden.
    l = BP::layoutChips ({ 60.0f, 60.0f, 60.0f, 60.0f }, two);
    REQUIRE (l.pills.size() == 4);
    CHECK (l.hidden == 0);
    CHECK (l.pills[3].getY() > l.pills[2].getY());
    CHECK (l.pills[3].getX() == 0.0f);

    // A chip wider than every row: counted, never drawn over the edge.
    l = BP::layoutChips ({ 260.0f, 40.0f }, two);
    CHECK (l.pills.empty());
    CHECK (l.hidden == 2);
    CHECK (BP::layoutChips ({ 40.0f }, {}).hidden == 1);
    CHECK (BP::layoutChips ({}, two).pills.empty());
}

TEST_CASE ("App UI: the Simple view shows every active-now chip of Signature at Boost 55 where the Advanced view's row counts some as +N (E39 / E38)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    const int music = controller.findStrip ("Music");
    controller.setSelectedStrip (music);
    juce::String error;
    REQUIRE (controller.loadPreset (*controller.getPresetManager().findByName ("Flubsound Signature"), music, error));
    controller.setMode (ModeValue::Music, music);
    controller.setBoost (0.55f, music);
    TestSignalGenerator source (controller.getHost().getSampleRate());
    source.setProgramme (music, TestSignalGenerator::Programme::Music, 0.0f);
    render (controller, source, 0.5);

    ui::MainComponent main (controller);
    main.setSize (1280, 820);
    auto& boost = main.getBoostPanel();
    ui::MeterSnapshot snapshot;
    snapshot.read (controller.getChain (music).meters());
    for (int i = 0; i < 16; ++i) // the chips refresh every 15 frames
        boost.update (snapshot);
    const auto stages = static_cast<int> (boost.getActiveStages().size());
    REQUIRE (stages >= 8);

    const auto shownIn = [&] (View view)
    {
        main.setView (view, false);
        (void) boost.createComponentSnapshot (boost.getLocalBounds()); // paints: lays the chips out
        return boost.getShownStageCount();
    };
    const int advanced = shownIn (View::Advanced);
    const int simple = shownIn (View::Simple);
    std::cerr << "    chips " << stages << ", shown: advanced " << advanced << ", simple " << simple << "\n";
    CHECK (advanced < stages);
    CHECK (simple == stages);
}

// =============================================================================
// SimpleStatusPanel
// =============================================================================
TEST_CASE ("App UI: SimpleStatusPanel names the headset, connection, ceiling, correction and device problems in plain words (E39)")
{
    using SP = ui::SimpleStatusPanel;
    using flub::device::Connection;
    flub::device::Advice advice;
    advice.ceilingDbTp = -2.0f;
    advice.messages = { "Turn off the headset's own EQ.", "Second message." };
    EngineController::DeviceCorrectionInfo correction;
    DeviceSafetyState safe;

    auto s = SP::describeOutput ({}, {}, Connection::Unknown, advice, correction, safe, false);
    CHECK (s.title == "No output device");
    CHECK (s.correction.isEmpty());
    CHECK (! s.recognised);

    s = SP::describeOutput ("Speakers (Realtek)", {}, Connection::Analog, advice, correction, safe, false);
    CHECK (s.title == "Speakers (Realtek)");
    CHECK (s.detail.contains ("Generic output, no headset profile"));
    CHECK (s.detail.contains ("wired"));
    CHECK (s.detail.contains ("ceiling -2.0 dBTP"));
    CHECK (s.correction == "No headphone correction");
    CHECK (s.advice == "Turn off the headset's own EQ.");

    correction.hasCurve = true;
    correction.name = "HD 600 ParametricEQ.txt";
    correction.enabled = true;
    s = SP::describeOutput ("Headphones (Stealth 700)", "Turtle Beach Stealth series", Connection::Bluetooth, advice, correction, safe, true);
    CHECK (s.recognised);
    CHECK (s.title == "Turtle Beach Stealth series");
    CHECK (s.detail.startsWith ("Headphones (Stealth 700)"));
    CHECK (s.detail.contains ("Bluetooth"));
    CHECK (s.correction == "Headphone correction: HD 600 ParametricEQ.txt (on)");
    CHECK (s.advice.isEmpty()); // the banner shows it
    correction.enabled = false;
    CHECK (SP::describeOutput ("x", {}, Connection::Unknown, advice, correction, safe, true).correction.endsWith ("(off)"));

    DeviceSafetyState muted;
    muted.kind = DeviceSafetyState::Kind::LoopbackPair;
    s = SP::describeOutput ("CABLE Input", {}, Connection::Unknown, advice, correction, muted, true);
    CHECK (s.problem);
    CHECK (s.detail.startsWith ("Output muted: feedback loop"));
    muted.kind = DeviceSafetyState::Kind::DeviceError;
    CHECK (SP::describeOutput ("USB", {}, Connection::Unknown, advice, correction, muted, true).detail.startsWith ("Audio device error"));
}

TEST_CASE ("App UI: SimpleStatusPanel says in plain words what the strip does to loudness (E39)")
{
    using SP = ui::SimpleStatusPanel;
    CHECK (SP::describeLoudnessChange (-18.0f, -15.1f, true) == "2.9 LU louder than the input");
    CHECK (SP::describeLoudnessChange (-14.0f, -15.4f, true) == "1.4 LU quieter than the input");
    CHECK (SP::describeLoudnessChange (-14.0f, -14.3f, true) == "About as loud as the input");
    CHECK (SP::describeLoudnessChange (-14.0f, -12.0f, false) == "No audio on this strip right now");
    CHECK (SP::describeLoudnessChange (-80.0f, -12.0f, true) == "No audio on this strip right now");
    CHECK (SP::describeLoudnessChange (-14.0f, std::nanf (""), true) == "No audio on this strip right now");
}

TEST_CASE ("App UI: the Simple view's headset status follows the output device; the advice is not repeated under the banner (E39)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    ui::MainComponent main (controller);
    main.setSize (1280, 820);
    auto& panel = main.getSimplePanel();
    CHECK (panel.getOutputStatus().title == "No output device");

    controller.simulateOutputDevice ("Headphones (Stealth 700 Gen 2 MAX)", controller.getHost().getSampleRate(), 2);
    CHECK (panel.getOutputStatus().recognised);
    CHECK (panel.getOutputStatus().title == controller.getDeviceProfileName());
    CHECK (panel.getOutputStatus().detail.contains ("Headphones (Stealth 700 Gen 2 MAX)"));
    CHECK (panel.getOutputStatus().advice.isEmpty()); // the advice banner shows it
    CHECK (panel.getBounds().getY() > main.getHeader().getBottom() + ui::DeviceAdviceBanner::kHeight);

    controller.simulateOutputDevice ("Speakers (Generic USB Audio)", controller.getHost().getSampleRate(), 2);
    CHECK (! panel.getOutputStatus().recognised);
    CHECK (panel.getOutputStatus().title == "Speakers (Generic USB Audio)");
}

TEST_CASE ("App UI: the screenshot driver's --view option picks the view; others are refused (E39)")
{
    ScreenshotDriver::Options o;
    juce::String error;
    CHECK (ScreenshotDriver::parseCommandLine ({ "--screenshot", "out.png" }, o, error));
    CHECK (! o.simpleView); // the full window, as every earlier screenshot
    CHECK (ScreenshotDriver::parseCommandLine ({ "--screenshot", "out.png", "--view", "simple" }, o, error));
    CHECK (o.simpleView);
    ScreenshotDriver::Options bad;
    CHECK (! ScreenshotDriver::parseCommandLine ({ "--screenshot", "out.png", "--view", "tiny" }, bad, error));
    CHECK (error == "--view must be 'advanced' or 'simple'");
}
