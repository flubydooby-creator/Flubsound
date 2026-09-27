// App-level tests: UI scale and the high-contrast theme (roadmap 3.6).
//
// * Contrast: WCAG 2.x contrast of every text token on every surface it is
//   drawn on, for both palettes (standard: text >= 4.5:1 on the content
//   surfaces, text and muted on the controls; high contrast: every text token
//   >= 4.5:1 on every surface, lines / meter / EQ band colours >= 3:1).
// * Theme switch: Theme::setTheme re-points Palette::, re-colours every
//   FlubLookAndFeel (including the mode accent) and re-maps colours set on
//   components; switching back restores the standard look exactly.
// * Scale: AppSettings round trip and clamping (0 = follow the system,
//   75 .. 200 %), the Desktop scale factor for a setting, the minimum window
//   size against the screen, and the Settings > General boxes.
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "settings/AppSettings.h"
#include "ui/FlubLookAndFeel.h"
#include "ui/SettingsDialog.h"
#include "ui/Theme.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace flub::app;
using ui::PaletteTokens;
using ui::UiTheme;
namespace Theme = ui::Theme;

namespace
{
using Token = std::pair<const char*, juce::Colour>;

/** Switches to `theme` for the lifetime of the object (tests always end on
    the standard theme, whatever they check). */
struct ScopedTheme
{
    explicit ScopedTheme (UiTheme theme) { Theme::setTheme (theme); }
    ~ScopedTheme() { Theme::setTheme (UiTheme::Standard); }
};

std::vector<Token> textTokens (const PaletteTokens& t)
{
    return { { "text", t.text },   { "muted", t.muted }, { "faint", t.faint }, { "teal", t.teal },          { "magenta", t.magenta },
             { "amber", t.amber }, { "red", t.red },     { "green", t.green }, { "dynamicEq", t.dynamicEq } };
}

/** The top of a panel's gradient (Theme::panelFill), the lightest point of
    every panel, for the active theme. */
juce::Colour panelTop()
{
    return Theme::panelFill ({ 0.0f, 0.0f, 300.0f, 200.0f }).getColourAtPosition (0.0);
}

/** Checks every (foreground, background) pair; returns the failures as
    "name on surface (ratio)" lines so a failing test names them. */
std::vector<std::string> failures (const std::vector<Token>& foreground, const std::vector<Token>& backgrounds, double minimum)
{
    std::vector<std::string> failed;
    for (const auto& [fgName, fg] : foreground)
        for (const auto& [bgName, bg] : backgrounds)
            if (const double ratio = Theme::contrastRatio (fg, bg); ratio < minimum)
                failed.push_back (std::string (fgName) + " on " + bgName + " (" + juce::String (ratio, 2).toStdString() + ":1)");
    return failed;
}

/** One failure per pair below the minimum, naming it. */
void checkNone (const std::vector<std::string>& failed)
{
    for (const auto& f : failed)
        ::flubtest::reportFailure (__FILE__, __LINE__, "below the contrast minimum: " + f);
}

EngineController::Options headlessOptions (const flubapptest::TempFolder& temp)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = false;
    return o;
}

template <typename T>
T* findChild (juce::Component& root, const std::function<bool (T&)>& match)
{
    for (auto* child : root.getChildren())
    {
        if (auto* c = dynamic_cast<T*> (child); c != nullptr && match (*c))
            return c;
        if (auto* found = findChild<T> (*child, match))
            return found;
    }
    return nullptr;
}
} // namespace

TEST_CASE ("App: Theme::contrastRatio matches the WCAG 2 reference values")
{
    CHECK_NEAR (Theme::contrastRatio (juce::Colours::black, juce::Colours::white), 21.0, 1.0e-9);
    CHECK_NEAR (Theme::contrastRatio (juce::Colours::white, juce::Colours::black), 21.0, 1.0e-9); // order does not matter
    CHECK_NEAR (Theme::contrastRatio (juce::Colour (0xff336699), juce::Colour (0xff336699)), 1.0, 1.0e-9);
    // #777777 on white is the classic "just fails AA" grey (4.48:1); #767676 just passes (4.54:1).
    CHECK_NEAR (Theme::contrastRatio (juce::Colour (0xff777777), juce::Colours::white), 4.48, 0.01);
    CHECK_NEAR (Theme::contrastRatio (juce::Colour (0xff767676), juce::Colours::white), 4.54, 0.01);
    CHECK_NEAR (Theme::relativeLuminance (juce::Colours::white), 1.0, 1.0e-9);
    CHECK_NEAR (Theme::relativeLuminance (juce::Colour (0xff808080)), 0.2159, 0.0005);
}

TEST_CASE ("App: standard palette: every text token meets WCAG AA (4.5:1) on the content surfaces, text and muted on the controls")
{
    ScopedTheme scoped (UiTheme::Standard);
    const auto& t = Theme::tokens (UiTheme::Standard);
    const std::vector<Token> content { { "background", t.background }, { "well", t.well }, { "panel", t.panel }, { "panel top", panelTop() } };
    checkNone (failures (textTokens (t), content, 4.5));

    // Buttons, combo boxes, menus and tooltips carry text and muted labels
    // (disabled controls use faint, which WCAG exempts).
    const std::vector<Token> controls { { "panelRaised", t.panelRaised }, { "panelHover", t.panelHover }, { "menu", t.menu }, { "tooltip", t.tooltip } };
    checkNone (failures ({ { "text", t.text }, { "muted", t.muted } }, controls, 4.5));
    checkNone (failures ({ { "text", t.text } }, { { "tabOn", t.tabOn } }, 4.5));
}

TEST_CASE ("App: high-contrast palette: every text token >= 4.5:1 on every surface, text >= 7:1, lines, meters and EQ bands >= 3:1")
{
    ScopedTheme scoped (UiTheme::HighContrast);
    const auto& t = Theme::tokens (UiTheme::HighContrast);
    const std::vector<Token> surfaces { { "background", t.background }, { "well", t.well },       { "panel", t.panel },
                                        { "panel top", panelTop() },    { "panelRaised", t.panelRaised }, { "panelHover", t.panelHover },
                                        { "menu", t.menu },             { "tooltip", t.tooltip }, { "tabOn", t.tabOn } };
    checkNone (failures (textTokens (t), surfaces, 4.5));
    checkNone (failures ({ { "text", t.text }, { "muted", t.muted } }, surfaces, 7.0)); // AAA for body text

    // Graphics (WCAG 1.4.11): control outlines, the scrollbar thumb, knob
    // rims, both meter palettes and the EQ band colours, on the surfaces they
    // are drawn on.
    std::vector<Token> graphics { { "border", t.border },   { "borderStrong", t.borderStrong }, { "track", t.track },     { "scrollThumb", t.scrollThumb },
                                  { "knobRim", t.knobRim }, { "meterSafe", t.meterSafe },       { "meterHot", t.meterHot } };
    const auto colourBlind = Theme::statusColours (ui::MeterPalette::ColourBlindSafe);
    graphics.insert (graphics.end(), { { "cb safe", colourBlind.safe }, { "cb warn", colourBlind.warn }, { "cb hot", colourBlind.hot } });
    static const char* const bandNames[] = { "band 1", "band 2", "band 3", "band 4", "band 5", "band 6", "band 7", "band 8", "band 9", "band 10" };
    for (size_t i = 0; i < t.eqBands.size(); ++i)
        graphics.push_back ({ bandNames[i], t.eqBands[i] });
    const std::vector<Token> graphicSurfaces { { "background", t.background }, { "well", t.well }, { "panel", t.panel },
                                               { "panel top", panelTop() },    { "panelRaised", t.panelRaised } };
    checkNone (failures (graphics, graphicSurfaces, 3.0));

    // A knob's value arc (the mode accent) stands out from its track without relying on hue.
    checkNone (failures ({ { "teal", t.teal }, { "magenta", t.magenta } }, { { "track", t.track } }, 3.0));
}

TEST_CASE ("App: switching the theme re-colours Palette, every FlubLookAndFeel (mode accent included) and component colours; switching back restores them")
{
    const auto& standard = Theme::tokens (UiTheme::Standard);
    const auto& contrast = Theme::tokens (UiTheme::HighContrast);
    REQUIRE (Theme::currentTheme() == UiTheme::Standard);

    ui::FlubLookAndFeel lnf;
    lnf.setAccent (Theme::accentForMode (flub::param::ModeValue::Gaming));
    CHECK (lnf.findColour (juce::ResizableWindow::backgroundColourId) == standard.background);
    CHECK (lnf.findColour (juce::Label::textColourId) == standard.text);

    juce::Label label;
    label.setColour (juce::Label::textColourId, ui::Palette::muted.withAlpha (0.5f));
    label.setColour (juce::Label::backgroundColourId, juce::Colour (0xff123456)); // not a token: left alone
    // The high-contrast palette has several tokens of the same RGB (background,
    // well and shadow are all black): the role is remembered, not guessed back.
    REQUIRE (contrast.well == contrast.background);
    label.setColour (juce::Label::outlineColourId, standard.well);
    label.setColour (juce::Label::outlineWhenEditingColourId, standard.background);

    {
        ScopedTheme scoped (UiTheme::HighContrast);
        CHECK (Theme::currentTheme() == UiTheme::HighContrast);
        CHECK (ui::Palette::text == contrast.text);
        CHECK (ui::Palette::background == contrast.background);
        CHECK (Theme::accentForMode (flub::param::ModeValue::Music) == contrast.teal);

        CHECK (lnf.findColour (juce::ResizableWindow::backgroundColourId) == contrast.background);
        CHECK (lnf.findColour (juce::Label::textColourId) == contrast.text);
        CHECK (lnf.findColour (juce::ComboBox::outlineColourId) == contrast.borderStrong);
        CHECK (lnf.findColour (juce::PopupMenu::backgroundColourId) == contrast.menu);
        CHECK (lnf.findColour (juce::ScrollBar::thumbColourId) == contrast.scrollThumb);
        CHECK (lnf.getAccent() == contrast.magenta); // the gaming accent follows the theme
        CHECK (lnf.findColour (juce::Slider::rotarySliderFillColourId) == contrast.magenta);

        // A look-and-feel created while the theme is active starts in it.
        ui::FlubLookAndFeel later;
        CHECK (later.findColour (juce::TextEditor::backgroundColourId) == contrast.well);

        // setTheme re-maps the colours of the windows on the desktop; this
        // label has no window, so apply the same step to it directly.
        Theme::remapComponentColours (label, standard, contrast);
        CHECK (label.findColour (juce::Label::textColourId) == contrast.muted.withAlpha (0.5f));
        CHECK (label.findColour (juce::Label::backgroundColourId) == juce::Colour (0xff123456));
        CHECK (label.findColour (juce::Label::outlineColourId) == contrast.well);
        CHECK (label.findColour (juce::Label::outlineWhenEditingColourId) == contrast.background);
        Theme::remapComponentColours (label, contrast, standard);
    }

    CHECK (Theme::currentTheme() == UiTheme::Standard);
    CHECK (ui::Palette::text == standard.text);
    CHECK (lnf.findColour (juce::ResizableWindow::backgroundColourId) == standard.background);
    CHECK (lnf.findColour (juce::Label::textColourId) == standard.text);
    CHECK (lnf.getAccent() == standard.magenta);
    CHECK (label.findColour (juce::Label::textColourId) == standard.muted.withAlpha (0.5f));
    CHECK (label.findColour (juce::Label::outlineColourId) == standard.well);
    CHECK (label.findColour (juce::Label::outlineWhenEditingColourId) == standard.background);

    // Every token of the two palettes maps onto the other and back.
    CHECK (Theme::remapColour (standard.faint, standard, contrast) == contrast.faint);
    CHECK (Theme::remapColour (contrast.borderStrong, contrast, standard) == standard.borderStrong);
    CHECK (Theme::remapColour (juce::Colour (0xff123456), standard, contrast) == juce::Colour (0xff123456));
}

TEST_CASE ("App: UI scale setting round-trips through the settings file, 0 follows the system and other values clamp to 75 .. 200 %")
{
    flubapptest::TempFolder temp;
    const auto file = temp.file ("settings.xml");
    {
        AppSettings settings (file, true);
        CHECK (settings.getUiScalePercent() == AppSettings::kUiScaleFollowSystem); // default
        CHECK (! settings.getHighContrast());
        settings.setUiScalePercent (150);
        CHECK (settings.getUiScalePercent() == 150);
        settings.setHighContrast (true);
        settings.save();
    }
    {
        AppSettings settings (file, true);
        CHECK (settings.getUiScalePercent() == 150);
        CHECK (settings.getHighContrast());

        settings.setUiScalePercent (20);
        CHECK (settings.getUiScalePercent() == 75);
        settings.setUiScalePercent (500);
        CHECK (settings.getUiScalePercent() == 200);
        settings.setUiScalePercent (-3);
        CHECK (settings.getUiScalePercent() == AppSettings::kUiScaleFollowSystem);

        // A value edited into the file by hand is clamped when read.
        settings.getPropertiesFile().setValue ("ui.scalePercent", 900);
        CHECK (settings.getUiScalePercent() == 200);
        settings.getPropertiesFile().setValue ("ui.scalePercent", 40);
        CHECK (settings.getUiScalePercent() == 75);
    }

    CHECK_NEAR (Theme::scaleFactorForPercent (AppSettings::kUiScaleFollowSystem), 1.0f, 1.0e-6f);
    CHECK_NEAR (Theme::scaleFactorForPercent (75), 0.75f, 1.0e-6f);
    CHECK_NEAR (Theme::scaleFactorForPercent (150), 1.5f, 1.0e-6f);
    CHECK_NEAR (Theme::scaleFactorForPercent (30), 0.75f, 1.0e-6f);
    CHECK_NEAR (Theme::scaleFactorForPercent (1000), 2.0f, 1.0e-6f);
}

TEST_CASE ("App: the minimum window size is the design minimum, but never larger than the screen at a large UI scale")
{
    const juce::Point<int> design { 1100, 700 };
    // A 1920 x 1080 display with a 40 px task bar, in logical pixels at 100 %, 150 % and 200 %.
    CHECK (Theme::minimumWindowSize (design, { 0, 0, 1920, 1040 }) == juce::Point<int> (1100, 700));
    CHECK (Theme::minimumWindowSize (design, { 0, 0, 1280, 693 }) == juce::Point<int> (1100, 693));
    CHECK (Theme::minimumWindowSize (design, { 0, 0, 960, 520 }) == juce::Point<int> (960, 520));
    CHECK (Theme::minimumWindowSize (design, {}) == juce::Point<int> (1, 1)); // never zero
}

TEST_CASE ("App: Settings > General: the UI scale and Theme boxes save the choice and apply it app-wide")
{
    flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    ui::HotkeyHooks hooks;
    hooks.isSupported = [] { return false; };
    hooks.getFailures = [] { return juce::StringArray(); };
    hooks.reRegister = [] {};
    ui::SettingsDialog settings (controller, hooks, [] (ui::MeterPalette) {}, ui::MeterPalette::Standard);
    settings.setSize (ui::SettingsDialog::kMinWidth, ui::SettingsDialog::kMinHeight);
    settings.showPage (ui::SettingsDialog::Page::General);

    auto* scaleBox = findChild<juce::ComboBox> (settings, [] (juce::ComboBox& b) { return b.getTitle() == "UI scale"; });
    auto* themeBox = findChild<juce::ComboBox> (settings, [] (juce::ComboBox& b) { return b.getTitle() == "Theme"; });
    REQUIRE (scaleBox != nullptr);
    REQUIRE (themeBox != nullptr);
    CHECK (scaleBox->getParentComponent()->isVisible()); // on the General page
    CHECK (scaleBox->getText() == "Follow system");
    CHECK (themeBox->getText() == "Standard (dark)");
    CHECK (! scaleBox->getBounds().intersects (themeBox->getBounds()));
    CHECK (themeBox->getBottom() <= themeBox->getParentComponent()->getHeight()); // fits the smallest dialog

    auto& desktop = juce::Desktop::getInstance();
    scaleBox->setSelectedId (150, juce::sendNotificationSync);
    CHECK (controller.getSettings().getUiScalePercent() == 150);
    CHECK_NEAR (desktop.getGlobalScaleFactor(), 1.5f, 1.0e-6f);
    scaleBox->setSelectedId (1, juce::sendNotificationSync); // Follow system
    CHECK (controller.getSettings().getUiScalePercent() == AppSettings::kUiScaleFollowSystem);
    CHECK_NEAR (desktop.getGlobalScaleFactor(), 1.0f, 1.0e-6f);

    {
        ScopedTheme restore (UiTheme::Standard); // back to standard even if a check fails
        themeBox->setSelectedId (2, juce::sendNotificationSync);
        CHECK (controller.getSettings().getHighContrast());
        CHECK (Theme::currentTheme() == UiTheme::HighContrast);
        CHECK (ui::Palette::background == Theme::tokens (UiTheme::HighContrast).background);
        themeBox->setSelectedId (1, juce::sendNotificationSync);
        CHECK (! controller.getSettings().getHighContrast());
        CHECK (Theme::currentTheme() == UiTheme::Standard);
    }

    // A dialog opened later shows the stored choice.
    controller.getSettings().setUiScalePercent (125);
    controller.getSettings().setHighContrast (true);
    ui::SettingsDialog reopened (controller, hooks, [] (ui::MeterPalette) {}, ui::MeterPalette::Standard);
    reopened.setSize (ui::SettingsDialog::kMinWidth, ui::SettingsDialog::kMinHeight);
    auto* scale2 = findChild<juce::ComboBox> (reopened, [] (juce::ComboBox& b) { return b.getTitle() == "UI scale"; });
    auto* theme2 = findChild<juce::ComboBox> (reopened, [] (juce::ComboBox& b) { return b.getTitle() == "Theme"; });
    REQUIRE (scale2 != nullptr);
    REQUIRE (theme2 != nullptr);
    CHECK (scale2->getText() == "125 %");
    CHECK (theme2->getText() == "High contrast");
}
