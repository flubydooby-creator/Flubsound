// Flubsound Pro - a labelled rotary knob: caption above, knob, value below.
//
// The value box is the slider's own text box (click to type a value such as
// "3.2k" or "-6", double-click the knob to reset). Binding to a parameter is
// done by a ParameterBinder on the owner's side:  binder.bindSlider (knob.slider, id).
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

namespace flub::app::ui
{
class ParamKnob : public juce::Component
{
public:
    enum class Size
    {
        Small,  // dense grids (all-parameter views)
        Medium, // module cards
        Large   // macro knobs
    };

    explicit ParamKnob (const juce::String& label = {}, Size size = Size::Medium);

    void setLabel (const juce::String& newLabel);
    const juce::String& getLabel() const noexcept { return label; }
    void setLabelColour (std::optional<juce::Colour> colour);
    void setKnobSize (Size newSize);

    /** Preferred size for this knob style. */
    juce::Rectangle<int> getPreferredSize() const;

    void paint (juce::Graphics& g) override;
    void resized() override;

    juce::Slider slider;

private:
    int labelHeight() const noexcept;
    int valueHeight() const noexcept;

    juce::String label;
    Size size;
    std::optional<juce::Colour> labelColour;
};
} // namespace flub::app::ui
