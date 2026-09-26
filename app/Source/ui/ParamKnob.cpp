#include "ParamKnob.h"

#include "FlubLookAndFeel.h"
#include "Theme.h"

namespace flub::app::ui
{
ParamKnob::ParamKnob (const juce::String& l, Size s)
    : label (l), size (s)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setRotaryParameters (FlubLookAndFeel::kRotaryStart, FlubLookAndFeel::kRotaryEnd, true);
    slider.setMouseDragSensitivity (220);
    slider.setVelocityBasedMode (false);
    slider.setTitle (l);
    addAndMakeVisible (slider);
    setKnobSize (s);
}

void ParamKnob::setLabel (const juce::String& newLabel)
{
    if (label == newLabel)
        return;
    label = newLabel;
    if (slider.getTitle().isEmpty())
        slider.setTitle (newLabel);
    repaint();
}

void ParamKnob::setLabelColour (std::optional<juce::Colour> colour)
{
    labelColour = colour;
    repaint();
}

void ParamKnob::setKnobSize (Size newSize)
{
    size = newSize;
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 76, valueHeight());
    resized();
}

int ParamKnob::labelHeight() const noexcept
{
    return size == Size::Large ? 16 : 14;
}

int ParamKnob::valueHeight() const noexcept
{
    return size == Size::Small ? 15 : 17;
}

juce::Rectangle<int> ParamKnob::getPreferredSize() const
{
    switch (size)
    {
        case Size::Small: return { 0, 0, 64, 78 };
        case Size::Medium: return { 0, 0, 70, 90 };
        case Size::Large: return { 0, 0, 84, 106 };
    }
    return { 0, 0, 70, 90 };
}

void ParamKnob::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().removeFromTop (labelHeight()).toFloat();
    g.setColour (labelColour.value_or (Palette::muted));
    g.setFont (size == Size::Large ? Theme::font (12.5f, true) : Theme::font (11.5f));
    g.drawFittedText (label, area.toNearestInt(), juce::Justification::centred, 1, 0.8f);
}

void ParamKnob::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop (labelHeight() + 1);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, juce::jmin (r.getWidth(), 84), valueHeight());
    slider.setBounds (r);
}
} // namespace flub::app::ui
