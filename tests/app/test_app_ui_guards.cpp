// App-level tests: the Dynamic Range (guard.range, docs/11 E21: the Startle
// Guard and the Gaming Tame band) and Smoothness (smooth.amount, docs/11
// E07) controls.
// * The Simple view: a row under the macros in the Boost panel (Simple
//   layout only), inside the panel, clear of the knobs and the chips, bound
//   to the selected strip.
// * The Advanced view: Dynamic Range on the Compressor card and Smoothness
//   on the Clarity card of the module rack; both act with their card's
//   module off, so they are not dimmed with it.
// Phase 3 batch 3 (the delivered DSP keys outside the generic grid):
// * Maximizer card: Style (max.style) and LF Limit (max.lfLimit, docs/11
//   E05); a named style dims Clipper and Release and the note names it.
// * Saturation card: Tape Grit (warmth.tapeGrit, docs/11 E14); the note and
//   the Boost panel's Warmth chip say when Warmth chose Tube; the chip
//   switches Tape Grit.
// * Clarity card: the Smoothness cut (docs/11 E07) on its note line.
// * Settings > Processing: the automatic preamp's hot-programme switch
//   (auto.preampHot, docs/11 E11) under the preamp's, dimmed without it;
//   the loudness contour's curve (docs/11 E32) at the listening level.
// Phase 3 batch 5 (5D U1):
// * Clarity card: Presence Mode (clarity.presenceMode, docs/11 E07) and the
//   per-band attack offsets (clarity.attackLow / attackHigh, E04 step 3);
//   Stereo & Space: Crossfeed Type (spatial.crossfeedType, E12); the
//   Virtualizer: virt.renderer / virt.frontBack (E28), bound by key name and
//   left out while the core lacks them.
// * Settings > Processing: Smart macros per strip (docs/11 E34).
// * Settings > Processing: the chat duck's switch and depth (docs/11 E22).
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "engine/TestSignalGenerator.h"
#include "ui/BoostPanel.h"
#include "ui/MainComponent.h"
#include "ui/ModuleCard.h"
#include "ui/ModuleRack.h"
#include "ui/ParameterBinding.h"
#include "ui/SettingsDialog.h"

#include "flub/engine/Parameters.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

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

/** `c` and every parent up to `root` are visible (no peer needed). */
bool visibleIn (juce::Component& root, juce::Component& c)
{
    for (auto* p = &c; p != nullptr && p != &root; p = p->getParentComponent())
        if (! p->isVisible())
            return false;
    return true;
}

/** Renders `seconds` of the test programme through the engine in 512-sample blocks. */
void render (EngineController& c, TestSignalGenerator& source, double seconds)
{
    const int total = static_cast<int> (seconds * c.getHost().getSampleRate());
    for (int done = 0; done < total; done += 512)
        c.renderOffline (source, std::min (512, total - done));
}

juce::String nameOf (int id)
{
    return juce::String (ui::ParamFormat::info (id).name);
}

ui::ModuleCard* findCard (juce::Component& root, const juce::String& id)
{
    for (auto* child : root.getChildren())
    {
        if (auto* card = dynamic_cast<ui::ModuleCard*> (child); card != nullptr && card->getDescriptor().id == id)
            return card;
        if (auto* found = findCard (*child, id))
            return found;
    }
    return nullptr;
}
} // namespace

TEST_CASE ("App UI: the Simple view has Dynamic Range and Smoothness under the macros, bound to the selected strip (E21 / E07)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    ui::MainComponent main (controller);
    auto& boost = main.getBoostPanel();
    auto& range = boost.getDynamicRangeBox();
    auto& smooth = boost.getSmoothnessSlider();

    for (const auto& [w, h] : { std::pair { 1100, 700 }, std::pair { 1280, 820 }, std::pair { 2560, 1440 } })
    {
        main.setView (View::Simple, false);
        main.setSize (w, h);
        REQUIRE (range.isVisible());
        REQUIRE (smooth.isVisible());
        const auto panel = boost.getLocalBounds();
        CHECK (panel.contains (range.getBounds()));
        CHECK (panel.contains (smooth.getBounds()));
        CHECK (range.getWidth() >= 120);
        CHECK (smooth.getWidth() >= 120);
        CHECK (! range.getBounds().intersects (smooth.getBounds()));
        for (auto* child : boost.getChildren())
            if (auto* knob = dynamic_cast<ui::ParamKnob*> (child))
            {
                CHECK (! knob->getBounds().intersects (range.getBounds()));
                CHECK (! knob->getBounds().intersects (smooth.getBounds()));
            }
        CHECK (boost.getChipRowCount() >= 1);

        // The Advanced view's Boost strip has no room for them: the rack has.
        main.setView (View::Advanced, false);
        CHECK (! range.isVisible());
        CHECK (! smooth.isVisible());
    }
    main.setView (View::Simple, false);
    main.setSize (1280, 820);
    CHECK (boost.getChipRowCount() == 3); // the row costs the Simple view no chip row at the default size

    // Off / 0 % by default; they write the selected strip.
    auto& store = controller.getSelectedParams();
    CHECK (range.getText() == "Off");
    CHECK (smooth.getValue() == 0.0);
    CHECK (range.getNumItems() == 5);
    range.setSelectedItemIndex (3, juce::sendNotificationSync);
    CHECK (store.get (GuardRange) == static_cast<float> (GuardRangeValue::Lu10Balanced));
    smooth.setValue (0.5, juce::sendNotificationSync);
    CHECK (store.get (SmoothAmount) == 0.5f);
    CHECK (range.getTooltip().startsWith ("Dynamic Range:"));
    CHECK (smooth.getTooltip().startsWith ("Smoothness:"));
}

TEST_CASE ("App UI: the rack has Dynamic Range on the Compressor card and Smoothness on the Clarity card, not dimmed with the module (E21 / E07)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    ui::ModuleRack rack (controller);
    rack.setSize (2400, 200); // every card in view
    rack.updateFromEngine();

    auto* comp = findCard (rack, "comp");
    auto* clarity = findCard (rack, "clarity");
    REQUIRE (comp != nullptr);
    REQUIRE (clarity != nullptr);
    auto* range = findByTitle<juce::ComboBox> (*comp, "Dynamic Range");
    auto* smooth = findByTitle<juce::Slider> (*clarity, "Smoothness");
    REQUIRE (range != nullptr);
    REQUIRE (smooth != nullptr);
    CHECK (visibleIn (rack, *range));
    CHECK (visibleIn (rack, *smooth));
    CHECK (range->getTooltip().contains ("works with Compressor switched off too"));
    CHECK (smooth->getTooltip().contains ("works with Clarity switched off too"));
    CHECK (range->getWidth() >= 100); // wide enough for "10 LU (Balanced)"
    CHECK (comp->getLocalBounds().contains (comp->getLocalArea (range, range->getLocalBounds())));

    // Both modules are off by default: their own controls dim, these do not.
    auto* ratio = findByTitle<juce::Slider> (*comp, "Ratio");
    REQUIRE (ratio != nullptr);
    CHECK (ratio->getParentComponent()->getAlpha() < 0.5f);
    CHECK (range->getAlpha() == 1.0f);
    CHECK (smooth->getParentComponent()->getAlpha() == 1.0f);

    // Bound to the selected strip.
    auto& store = controller.getSelectedParams();
    range->setSelectedItemIndex (4, juce::sendNotificationSync);
    CHECK (store.get (GuardRange) == static_cast<float> (GuardRangeValue::Lu6Shield));
    smooth->setValue (0.25, juce::sendNotificationSync);
    CHECK (store.get (SmoothAmount) == 0.25f);
}

// =============================================================================
// Phase 3 batch 3: the delivered DSP keys outside the generic grid
// =============================================================================
TEST_CASE ("App UI: the Maximizer card has Style and LF Limit; a named style dims Clipper and Release and the note names it (E05)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    auto& store = controller.getSelectedParams();
    store.set (MaximizerOn, 1.0f);
    ui::ModuleRack rack (controller);
    rack.setSize (2600, 200); // every card in view
    rack.updateFromEngine();

    auto* max = findCard (rack, "max");
    REQUIRE (max != nullptr);
    auto* style = findByTitle<juce::ComboBox> (*max, nameOf (MaxStyle));
    auto* lf = findByTitle<juce::Slider> (*max, nameOf (MaxLfLimit));
    auto* clipper = findByTitle<juce::Slider> (*max, nameOf (MaxClipAmount));
    auto* release = findByTitle<juce::Slider> (*max, nameOf (MaxReleaseMs));
    auto* drive = findByTitle<juce::Slider> (*max, nameOf (MaxDriveDb));
    REQUIRE (style != nullptr);
    REQUIRE (lf != nullptr);
    REQUIRE (clipper != nullptr);
    REQUIRE (release != nullptr);
    REQUIRE (drive != nullptr);
    CHECK (visibleIn (rack, *style));
    CHECK (visibleIn (rack, *lf));
    CHECK (style->getWidth() >= 100); // "Transparent" / "Aggressive" fit
    CHECK (max->getLocalBounds().contains (max->getLocalArea (style, style->getLocalBounds())));
    CHECK (style->getText() == "Custom");
    CHECK (style->getNumItems() == 5);

    // Custom: every control at full strength, the ordinary note.
    CHECK (clipper->getParentComponent()->getAlpha() == 1.0f);
    CHECK (release->getParentComponent()->getAlpha() == 1.0f);
    CHECK (! max->getNoteText().startsWith ("Style"));

    // A named style: written to the selected strip; the style owns Clipper and
    // Release (their stored values return under Custom), so they dim.
    style->setSelectedItemIndex (static_cast<int> (MaxStyleValue::Punchy), juce::sendNotificationSync);
    CHECK (store.get (MaxStyle) == static_cast<float> (MaxStyleValue::Punchy));
    rack.updateFromEngine();
    CHECK (clipper->getParentComponent()->getAlpha() < 0.5f);
    CHECK (release->getParentComponent()->getAlpha() < 0.5f);
    CHECK (drive->getParentComponent()->getAlpha() == 1.0f);
    CHECK (lf->getParentComponent()->getAlpha() == 1.0f);
    CHECK (max->getNoteText() == "Style Punchy sets Clipper and Release");
    style->setSelectedItemIndex (static_cast<int> (MaxStyleValue::Custom), juce::sendNotificationSync);
    rack.updateFromEngine();
    CHECK (clipper->getParentComponent()->getAlpha() == 1.0f);

    // LF Limit writes max.lfLimit (0 .. 100 %), with the tooltip's hint.
    lf->setValue (0.5, juce::sendNotificationSync);
    CHECK (store.get (MaxLfLimit) == 0.5f);
    CHECK (lf->getTooltip().isNotEmpty());
    CHECK (style->getTooltip().isNotEmpty());
}

TEST_CASE ("App UI: Warmth's Tube choice shows on the Saturation card and the Boost panel's chip; the chip and the card switch Tape Grit (E14)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    const int music = controller.findStrip ("Music");
    REQUIRE (music >= 0);
    controller.setSelectedStrip (music);
    controller.setMode (ModeValue::Music, music);
    auto& store = controller.getParams (music);
    auto& chain = controller.getChain (music);
    const auto base = [&store] (int id) { return store.get (id); };
    const auto effective = [&chain] (int id) { return chain.effectiveValue (id); };
    TestSignalGenerator source (controller.getHost().getSampleRate());
    source.setProgramme (music, TestSignalGenerator::Programme::Music, 0.0f);

    ui::BoostPanel panel (controller);
    panel.setSize (1100, 240);
    panel.setMode (ModeValue::Music);
    auto& chip = panel.getWarmthChip();
    CHECK (chip.isVisible());
    CHECK (panel.getLocalBounds().contains (chip.getBounds()));
    CHECK (chip.getWidth() >= 30);
    // Warmth 0: a tone control, nothing chosen.
    CHECK (ui::WarmthColour::of (base, effective).kind == ui::WarmthColour::Kind::None);
    CHECK (chip.getButtonText() == "TONE");
    CHECK (! chip.getToggleState());

    // Warmth up with the saturator Warmth's alone: MacroMap's override row
    // selects Tube (the effective sat.type), which the card and the chip say.
    store.set (Macro5, 0.6f);
    render (controller, source, 0.1);
    CHECK (std::lround (chain.effectiveValue (SatType)) == 1); // Tube
    CHECK (ui::WarmthColour::of (base, effective).kind == ui::WarmthColour::Kind::Tube);
    ui::ModuleRack rack (controller);
    rack.setSize (2600, 200);
    rack.updateFromEngine();
    auto* sat = findCard (rack, "sat");
    REQUIRE (sat != nullptr);
    CHECK (sat->getNoteText() == "Tube chosen by Warmth");
    panel.setMode (ModeValue::Music); // re-reads the chip
    CHECK (chip.getButtonText() == "TUBE");
    CHECK (chip.getTooltip().contains ("Tube saturator"));
    CHECK (chip.getTooltip().contains ("Click to switch Tape Grit"));

    // The chip switches Tape Grit on the selected strip: the classic Warmth.
    chip.setToggleState (true, juce::sendNotificationSync); // a click
    CHECK (store.get (WarmthTapeGrit) == 1.0f);
    render (controller, source, 0.1);
    panel.setMode (ModeValue::Music);
    CHECK (chip.getButtonText() == "TAPE");
    CHECK (chip.getToggleState());
    CHECK (ui::WarmthColour::of (base, effective).kind == ui::WarmthColour::Kind::TapeGrit);
    rack.updateFromEngine();
    CHECK (sat->getNoteText() != "Tube chosen by Warmth");

    // The card's Tape Grit switch: not dimmed with the saturator off (Warmth
    // uses it with the module off), bound to the same key.
    auto* grit = findByTitle<juce::ToggleButton> (*sat, nameOf (WarmthTapeGrit));
    REQUIRE (grit != nullptr);
    CHECK (visibleIn (rack, *grit));
    CHECK (flubapptest::pumpMessagesUntil ([grit] { return grit->getToggleState(); }, 2000)); // the rack's binder follows the store
    CHECK (grit->getAlpha() == 1.0f);
    CHECK (grit->getTooltip().contains ("works with Saturation switched off too"));
    grit->setToggleState (false, juce::sendNotificationSync);
    CHECK (store.get (WarmthTapeGrit) == 0.0f);

    // The user's own saturator keeps its type: no Tube choice.
    store.set (SaturationOn, 1.0f);
    render (controller, source, 0.1);
    CHECK (ui::WarmthColour::of (base, effective).kind == ui::WarmthColour::Kind::None);
    panel.setMode (ModeValue::Music);
    CHECK (chip.getButtonText() == "TONE");
    CHECK (chip.getTooltip().contains ("keeps the type you chose"));

    // Gaming has no Warmth macro: no chip.
    panel.setMode (ModeValue::Gaming);
    CHECK (! chip.isVisible());
}

TEST_CASE ("App UI: the Clarity card's note gives the Smoothness cut while the stage cuts (E07)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    const int music = controller.findStrip ("Music");
    REQUIRE (music >= 0);
    controller.setSelectedStrip (music);
    ui::ModuleRack rack (controller);
    rack.setSize (2600, 200);
    rack.updateFromEngine();
    auto* clarity = findCard (rack, "clarity");
    REQUIRE (clarity != nullptr);
    CHECK (! clarity->getNoteText().startsWith ("Smoothness cut")); // Smoothness 0: no cut

    // The card formats what the rack hands it ...
    ui::ModuleCard::State s;
    s.smoothnessCutDb = -3.24f;
    clarity->setState (s);
    CHECK (clarity->getNoteText() == "Smoothness cut -3.2 dB (5 - 10 kHz)");
    s.smoothnessCutDb = -0.02f; // below the display's resolution
    clarity->setState (s);
    CHECK (! clarity->getNoteText().startsWith ("Smoothness cut"));

    // ... and the rack hands it the chain's cut: white noise (as bright as a
    // sibilant, its 5 - 10 kHz band over its body) through Boost and
    // Clarity at 100 and Smoothness 100.
    auto& store = controller.getParams (music);
    controller.setMode (ModeValue::Music, music);
    controller.setBoost (1.0f, music);
    store.set (Macro3, 1.0f); // Clarity
    store.set (SmoothAmount, 1.0f);
    auto& chain = controller.getChain (music);
    std::vector<float> left (512), right (512);
    float* channels[] = { left.data(), right.data() };
    juce::Random random (29);
    float deepest = 0.0f;
    for (int b = 0; b < 400 && deepest > -0.5f; ++b)
    {
        for (size_t i = 0; i < left.size(); ++i)
        {
            left[i] = 0.1f * (random.nextFloat() * 2.0f - 1.0f);
            right[i] = 0.1f * (random.nextFloat() * 2.0f - 1.0f);
        }
        chain.process (flub::AudioBlock (channels, 2, 512));
        deepest = std::min (deepest, chain.getSmoothnessCutDb());
    }
    REQUIRE (deepest <= -0.5f);
    rack.updateFromEngine();
    CHECK (clarity->getNoteText() == "Smoothness cut " + ui::Theme::formatSignedDb (chain.getSmoothnessCutDb(), 1) + " dB (5 - 10 kHz)");
}

TEST_CASE ("App UI: Settings > Processing has the hot-programme preamp switch under the automatic preamp's, dimmed without it (E11)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    ui::HotkeyHooks hooks;
    hooks.isSupported = [] { return false; };
    hooks.getFailures = [] { return juce::StringArray(); };
    hooks.reRegister = [] {};
    ui::SettingsDialog dialog (controller, hooks, [] (ui::MeterPalette) {}, ui::MeterPalette::Standard);
    dialog.setSize (900, 700);
    dialog.showPage (ui::SettingsDialog::Page::Processing);

    auto* preamp = findByTitle<juce::ToggleButton> (dialog, "Automatic preamp");
    auto* hot = findByTitle<juce::ToggleButton> (dialog, "Automatic preamp on hot programme");
    REQUIRE (preamp != nullptr);
    REQUIRE (hot != nullptr);
    CHECK (hot->isVisible());
    CHECK (hot->getY() > preamp->getY()); // right under it
    CHECK (hot->getY() - preamp->getBottom() < 120);
    CHECK (hot->getButtonText().contains (controller.getStripName (controller.getSelectedStrip())));
    auto& store = controller.getSelectedParams();
    CHECK (! hot->getToggleState());
    CHECK (! hot->isEnabled()); // auto.preamp off: the term does nothing

    preamp->setToggleState (true, juce::sendNotificationSync); // a click
    CHECK (store.get (AutoPreampOn) == 1.0f);
    dialog.showPage (ui::SettingsDialog::Page::Processing); // refresh
    CHECK (hot->isEnabled());
    hot->setToggleState (true, juce::sendNotificationSync);
    CHECK (store.get (AutoPreampHot) == 1.0f);
    // Another strip selected: the switch follows it.
    const int other = (controller.getSelectedStrip() + 1) % controller.getNumStrips();
    controller.setSelectedStrip (other);
    dialog.showPage (ui::SettingsDialog::Page::Processing);
    CHECK (flubapptest::pumpMessagesUntil ([hot] { return ! hot->getToggleState(); }, 2000)); // the page's binder follows the strip
    CHECK (hot->getButtonText().contains (controller.getStripName (other)));
}

TEST_CASE ("App UI: the Processing page draws the loudness contour's curve at the listening level, as the chain designs it (E32)")
{
    using flub::iso226::kFrequencies;
    const auto indexOf = [] (double hz)
    {
        return static_cast<size_t> (std::find_if (kFrequencies.begin(), kFrequencies.end(), [hz] (double f) { return std::abs (f - hz) < 0.5; })
                                    - kFrequencies.begin());
    };
    const size_t at50 = indexOf (50.0), at1k = indexOf (1000.0), at12k5 = indexOf (12500.0);
    REQUIRE (at12k5 < kFrequencies.size());

    // The curve: ISO 226 G (f) at the level, capped (LoudnessContour.h: +12.1
    // dB at 50 Hz, +4.4 dB at 12.5 kHz for 80 phon at -30 dB).
    auto curve = ui::SettingsDialog::contourCurve (true, 80.0f, -30.0f, 18.0f);
    CHECK (curve.on);
    CHECK (curve.levelDb == -30.0f);
    CHECK_NEAR (curve.liftDb[at50], 12.1f, 0.1f);
    CHECK_NEAR (curve.liftDb[at1k], 0.0f, 0.01f);
    CHECK_NEAR (curve.liftDb[at12k5], 4.4f, 0.1f);
    CHECK (ui::SettingsDialog::contourCurve (true, 80.0f, -30.0f, 6.0f).liftDb[at50] == 6.0f); // contour.maxLift
    CHECK (ui::SettingsDialog::contourCurve (true, 80.0f, -80.0f, 18.0f).levelDb == -60.0f);    // clamped as the stage does
    CHECK_NEAR (ui::SettingsDialog::contourCurve (true, 80.0f, 0.0f, 18.0f).liftDb[at50], 0.0f, 0.01f);
    const auto off = ui::SettingsDialog::contourCurve (false, 80.0f, -30.0f, 18.0f);
    CHECK (! off.on);
    CHECK (std::all_of (off.liftDb.begin(), off.liftDb.end(), [] (float v) { return v == 0.0f; }));
    CHECK (ui::SettingsDialog::describeContourCurve (curve, -9.9f)
           == "At -30.0 dB re the reference: +12.1 dB at 50 Hz, +4.4 dB at 12.5 kHz; level trim -9.9 dB (so the lift does not drive the limiter).");
    CHECK (ui::SettingsDialog::describeContourCurve (off, 0.0f).startsWith ("Off:"));

    // On the page: the selected strip's contour at its level, the chain's
    // design within its fit (<= 0.5 dB at 50 Hz).
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    const int music = controller.findStrip ("Music");
    REQUIRE (music >= 0);
    controller.setSelectedStrip (music);
    ui::HotkeyHooks hooks;
    hooks.isSupported = [] { return false; };
    hooks.getFailures = [] { return juce::StringArray(); };
    hooks.reRegister = [] {};
    ui::SettingsDialog dialog (controller, hooks, [] (ui::MeterPalette) {}, ui::MeterPalette::Standard);
    dialog.setSize (900, 700);
    dialog.showPage (ui::SettingsDialog::Page::Processing);
    auto* view = findByTitle<juce::Component> (dialog, "Loudness contour curve");
    REQUIRE (view != nullptr);
    CHECK (view->isVisible());
    CHECK (view->getWidth() >= 300);
    CHECK (view->getHeight() >= 100);
    CHECK (view->getDescription().startsWith ("Off:"));
    auto* contourToggle = findByTitle<juce::ToggleButton> (dialog, "Loudness contour");
    REQUIRE (contourToggle != nullptr);
    CHECK (view->getY() > contourToggle->getY()); // in the Listening level section

    auto& store = controller.getParams (music);
    store.set (ContourOn, 1.0f);
    store.set (ContourLevelDb, -30.0f);
    TestSignalGenerator source (controller.getHost().getSampleRate());
    source.setProgramme (music, TestSignalGenerator::Programme::Music, -12.0f);
    render (controller, source, 0.2);
    dialog.showPage (ui::SettingsDialog::Page::Processing); // refresh
    CHECK (view->getDescription().startsWith ("At -30.0 dB re the reference: +12.1 dB at 50 Hz"));
    CHECK_NEAR (controller.getChain (music).getLoudnessContour().targetLiftDb (50.0), curve.liftDb[at50], 0.5);
}

// =============================================================================
// Phase 3 batch 5: the new keys in their module cards, Smart macros
// =============================================================================
TEST_CASE ("App UI: the batch 5 keys sit in their cards outside the generic grid; a key the core lacks is left out (E07 / E04 / E12 / E28)")
{
    // Every key in the table resolves to a parameter (a named key the layout
    // lacks is dropped, never bound to -1).
    for (const auto& d : ui::ModuleDescriptor::all())
        for (const auto& key : d.keys)
            CHECK ((key.banded || (key.id >= 0 && key.id < kNumParams)));

    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    ui::ModuleRack rack (controller);
    rack.setSize (3400, 200); // every card in view
    rack.updateFromEngine();
    auto& store = controller.getSelectedParams();

    // Clarity: Presence Mode (a choice) and the per-band attack offsets.
    auto* clarity = findCard (rack, "clarity");
    REQUIRE (clarity != nullptr);
    auto* mode = findByTitle<juce::ComboBox> (*clarity, nameOf (ClarityPresenceMode));
    REQUIRE (mode != nullptr);
    CHECK (visibleIn (rack, *mode));
    CHECK (mode->getNumItems() == 2);
    CHECK (clarity->getLocalBounds().contains (clarity->getLocalArea (mode, mode->getLocalBounds())));
    mode->setSelectedItemIndex (static_cast<int> (PresenceModeValue::Relative), juce::sendNotificationSync);
    CHECK (store.get (ClarityPresenceMode) == static_cast<float> (PresenceModeValue::Relative));
    for (const char* key : { "clarity.attackLow", "clarity.attackHigh" })
    {
        const int id = findByKey (key);
        if (id < 0)
            continue; // not in this build's core yet: hidden
        auto* knob = findByTitle<juce::Slider> (*clarity, nameOf (id));
        REQUIRE (knob != nullptr);
        CHECK (visibleIn (rack, *knob));
        knob->setValue (3.0, juce::sendNotificationSync);
        CHECK (store.get (id) == 3.0f);
        CHECK (knob->getTooltip().isNotEmpty());
    }

    // Stereo & Space: Crossfeed Type (Bs2b / Meier / Mono-safe).
    auto* spatial = findCard (rack, "spatial");
    REQUIRE (spatial != nullptr);
    auto* crossfeed = findByTitle<juce::ComboBox> (*spatial, nameOf (SpatialCrossfeedType));
    REQUIRE (crossfeed != nullptr);
    CHECK (visibleIn (rack, *crossfeed));
    CHECK (crossfeed->getNumItems() == 3);
    crossfeed->setSelectedItemIndex (static_cast<int> (CrossfeedTypeValue::MonoSafe), juce::sendNotificationSync);
    CHECK (store.get (SpatialCrossfeedType) == static_cast<float> (CrossfeedTypeValue::MonoSafe));

    // Virtualizer: the renderer and its front / back contrast once the core has them.
    auto* virt = findCard (rack, "virt");
    REQUIRE (virt != nullptr);
    const int keysWithout = 3;
    int named = 0;
    for (const char* key : { "virt.renderer", "virt.frontBack" })
        if (const int id = findByKey (key); id >= 0)
        {
            ++named;
            juce::Component* control = findByTitle<juce::ComboBox> (*virt, nameOf (id));
            if (control == nullptr)
                control = findByTitle<juce::Slider> (*virt, nameOf (id));
            REQUIRE (control != nullptr);
            CHECK (visibleIn (rack, *control));
        }
    CHECK (virt->getDescriptor().keys.size() == static_cast<size_t> (keysWithout + named));
}

TEST_CASE ("App UI: Smart macros - a per-strip switch on the Processing page, persisted and handed to the strip's chain (E34)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    ui::HotkeyHooks hooks;
    hooks.isSupported = [] { return false; };
    hooks.getFailures = [] { return juce::StringArray(); };
    hooks.reRegister = [] {};
    ui::SettingsDialog dialog (controller, hooks, [] (ui::MeterPalette) {}, ui::MeterPalette::Standard);
    dialog.setSize (900, 700);
    dialog.showPage (ui::SettingsDialog::Page::Processing);

    auto* smart = findByTitle<juce::ToggleButton> (dialog, "Smart macros");
    auto* hot = findByTitle<juce::ToggleButton> (dialog, "Automatic preamp on hot programme");
    REQUIRE (smart != nullptr);
    REQUIRE (hot != nullptr);
    CHECK (visibleIn (dialog, *smart));
    CHECK (smart->getY() > hot->getY()); // with the preamp, under Protection
    const int strip = controller.getSelectedStrip();
    CHECK (smart->getButtonText().contains (controller.getStripName (strip)));
    CHECK (! smart->getToggleState());
    CHECK (! controller.getChain (strip).getSmartMacros());

    smart->setToggleState (true, juce::sendNotificationSync);
    CHECK (controller.getSmartMacros (strip));
    CHECK (controller.getChain (strip).getSmartMacros());
    CHECK (controller.getSettings().getSmartMacros (controller.getStripName (strip)));
    const int other = (strip + 1) % controller.getNumStrips();
    CHECK (! controller.getChain (other).getSmartMacros()); // per strip
    controller.setSelectedStrip (other);
    dialog.showPage (ui::SettingsDialog::Page::Processing); // refresh
    CHECK (! smart->getToggleState());
    CHECK (smart->getButtonText().contains (controller.getStripName (other)));
}

TEST_CASE ("App UI: Settings > Processing has the chat duck's switch and depth, driving the controller (E22)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    ui::HotkeyHooks hooks;
    hooks.isSupported = [] { return false; };
    hooks.getFailures = [] { return juce::StringArray(); };
    hooks.reRegister = [] {};
    ui::SettingsDialog dialog (controller, hooks, [] (ui::MeterPalette) {}, ui::MeterPalette::Standard);
    dialog.setSize (900, 700);
    dialog.showPage (ui::SettingsDialog::Page::Processing);

    auto* duck = findByTitle<juce::ToggleButton> (dialog, "Duck game under voice chat");
    auto* depth = findByTitle<juce::Slider> (dialog, "Chat duck depth");
    REQUIRE (duck != nullptr);
    REQUIRE (depth != nullptr);
    CHECK (visibleIn (dialog, *duck));
    CHECK (! duck->getToggleState());
    CHECK (! depth->isEnabled());
    CHECK (depth->getValue() == EngineController::kDefaultChatDuckDepthDb);

    duck->setToggleState (true, juce::sendNotificationSync);
    CHECK (controller.getChatDuck());
    CHECK (controller.getSettings().getChatDuck());
    dialog.showPage (ui::SettingsDialog::Page::Processing); // refresh
    CHECK (depth->isEnabled());
    depth->setValue (6.0, juce::sendNotificationSync);
    CHECK (controller.getChatDuckDepthDb() == 6.0f);
    controller.setChatDuck (false); // elsewhere (the Chat row): the page follows
    dialog.showPage (ui::SettingsDialog::Page::Processing);
    CHECK (! duck->getToggleState());
}
