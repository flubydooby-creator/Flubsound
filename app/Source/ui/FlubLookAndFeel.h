// Flubsound Pro - the application look-and-feel.
//
// A LookAndFeel_V4 subclass that draws every stock JUCE widget in the
// Flubsound design language: rotary knobs with a value arc (bipolar
// parameters grow from their zero point) and an optional "effective value"
// ring for macro-modulated parameters, thin linear sliders, text buttons
// with a few named styles, power / switch / tick toggle buttons, combo
// boxes, popup menus, scrollbars, tooltips and text editors.
//
// Button / slider styles are selected with component properties so that
// plain juce::TextButton / juce::ToggleButton / juce::Slider objects can be
// used everywhere:
//
//   button.getProperties().set (FlubLookAndFeel::styleProperty, "tab");
//   (or Style::set (button, "tab") from Widgets.h)
//
//   TextButton   "tab"      neutral selection with an accent underline
//                           (strip selector, A/B, settings navigation)
//                "chip"     small pill toggle (analyser display options)
//                "warning"  amber when toggled on (bypass)
//                (default)  raised button, accent tint when toggled on
//   ToggleButton "power"    round power icon (module enable)
//                "switch"   pill switch followed by the button text
//                (default)  tick box followed by the button text
//   Slider       property effectiveProperty (double, proportion 0..1): draws
//                the post-macro value as a thin outer ring on rotary knobs.
//
// The accent colour follows the processing mode (setAccent) and the meter
// palette can be switched to a colour-blind safe variant.
#pragma once

#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace flub::app::ui
{
class FlubLookAndFeel : public juce::LookAndFeel_V4
{
public:
    static inline const juce::Identifier styleProperty { "flubStyle" };
    static inline const juce::Identifier effectiveProperty { "flubEffective" };

    FlubLookAndFeel();
    ~FlubLookAndFeel() override = default;

    void setAccent (juce::Colour newAccent);
    juce::Colour getAccent() const noexcept { return accent; }

    void setMeterPalette (MeterPalette palette) noexcept { meterPalette = palette; }
    MeterPalette getMeterPalette() const noexcept { return meterPalette; }

    /** Rotary angles used by every knob (and the Boost dial): -135 .. +135 deg. */
    static constexpr float kRotaryStart = juce::MathConstants<float>::pi * 1.25f;
    static constexpr float kRotaryEnd = juce::MathConstants<float>::pi * 2.75f;

    // ---- Sliders -------------------------------------------------------------------
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos, float minSliderPos,
                           float maxSliderPos, juce::Slider::SliderStyle, juce::Slider&) override;
    int getSliderThumbRadius (juce::Slider&) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;

    // ---- Buttons -------------------------------------------------------------------
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour, bool isHighlighted,
                               bool isDown) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool isHighlighted, bool isDown) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool isHighlighted, bool isDown) override;
    void drawTickBox (juce::Graphics&, juce::Component&, float x, float y, float w, float h, bool ticked, bool isEnabled,
                      bool isHighlighted, bool isDown) override;

    // ---- Combo boxes / popup menus ---------------------------------------------------
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    void drawComboBoxTextWhenNothingSelected (juce::Graphics&, juce::ComboBox&, juce::Label&) override;

    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted,
                            bool isTicked, bool hasSubMenu, const juce::String& text, const juce::String& shortcutKeyText,
                            const juce::Drawable* icon, const juce::Colour* textColour) override;
    void drawPopupMenuSectionHeader (juce::Graphics&, const juce::Rectangle<int>& area, const juce::String& sectionName) override;
    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight, int& idealWidth,
                                    int& idealHeight) override;
    juce::Font getPopupMenuFont() override;
    int getPopupMenuBorderSize() override;

    // ---- Misc ------------------------------------------------------------------------------
    int getDefaultScrollbarWidth() override;
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height, bool isScrollbarVertical,
                        int thumbStartPosition, int thumbSize, bool isMouseOver, bool isMouseDown) override;

    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;

    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;

    juce::Font getAlertWindowTitleFont() override;
    juce::Font getAlertWindowMessageFont() override;
    juce::Font getAlertWindowFont() override;
    juce::Font getLabelFont (juce::Label&) override;
    /** Combo box labels: one line with an ellipsis instead of wrapping. */
    void drawLabel (juce::Graphics&, juce::Label&) override;

    void drawCallOutBoxBackground (juce::CallOutBox&, juce::Graphics&, const juce::Path&, juce::Image&) override;
    /** Input level bar of juce::AudioDeviceSelectorComponent. */
    void drawLevelMeter (juce::Graphics&, int width, int height, float level) override;

private:
    static juce::String styleOf (const juce::Component& c);
    static juce::TextLayout layoutTooltip (const juce::String& text, juce::Colour colour);
    void applyAccentColours();

    juce::Colour accent = Palette::teal;
    MeterPalette meterPalette = MeterPalette::Standard;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FlubLookAndFeel)
};
} // namespace flub::app::ui
