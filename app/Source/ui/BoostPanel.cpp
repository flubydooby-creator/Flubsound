#include "BoostPanel.h"

#include "FlubLookAndFeel.h"
#include "Theme.h"

#include <cmath>

namespace flub::app::ui
{
using namespace flub::param;

namespace
{
const char* macroTip (ModeValue mode, int index)
{
    static const char* music[] = { "Punch: transient attack and a tighter low end", "Width: stereo width and a sense of space",
                                   "Clarity: presence, air and de-mud (with dynamic de-harsh)",
                                   "Loudness: maximizer drive and multiband glue (safety governed)",
                                   "Warmth: tape saturation and harmonic bass (safety governed)" };
    static const char* gaming[] = { "Footsteps: lifts quiet high-frequency cues (steps, reloads) and tames masking booms",
                                    "Positional: sharpens left / right / front / back placement",
                                    "Impact: weight for explosions and gunshots (safety governed)",
                                    "Detail: brings up quiet ambience and distant cues (upward compression)",
                                    "Voice & Score: dialogue, comms and music intelligibility" };
    index = juce::jlimit (0, 4, index);
    return mode == ModeValue::Gaming ? gaming[index] : music[index];
}
} // namespace

// =============================================================================
// BoostDial
// =============================================================================
BoostDial::BoostDial()
{
    setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    setRotaryParameters (FlubLookAndFeel::kRotaryStart, FlubLookAndFeel::kRotaryEnd, true);
    setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    setMouseDragSensitivity (300);
    setTitle ("Boost Intensity");
    setWantsKeyboardFocus (true);
}

void BoostDial::setGovernorScale (float scale)
{
    scale = juce::jlimit (0.0f, 1.0f, std::isfinite (scale) ? scale : 1.0f);
    if (std::abs (scale - governor) > 0.002f)
    {
        governor = scale;
        repaint();
    }
}

void BoostDial::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const float size = juce::jmin (bounds.getWidth(), bounds.getHeight());
    const auto centre = bounds.getCentre();
    const auto accent = Theme::accent (*this);
    const float start = FlubLookAndFeel::kRotaryStart, end = FlubLookAndFeel::kRotaryEnd;
    const auto value = static_cast<float> (valueToProportionOfLength (getValue()));
    const float valueAngle = start + value * (end - start);

    const float track = juce::jlimit (6.0f, 12.0f, size * 0.075f);
    const float radius = size * 0.5f - track * 0.5f - 4.0f;
    const auto stroke = [] (float w) { return juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded); };
    auto arc = [&] (float from, float to, float r)
    {
        juce::Path p;
        p.addCentredArc (centre.x, centre.y, r, r, 0.0f, from, to, true);
        return p;
    };

    // Tick marks every 10 %.
    for (int i = 0; i <= 10; ++i)
    {
        const float a = start + (end - start) * static_cast<float> (i) / 10.0f;
        const juce::Point<float> dir (std::sin (a), -std::cos (a));
        const float r0 = radius + track * 0.5f + 2.0f, r1 = r0 + (i % 5 == 0 ? 4.0f : 2.5f);
        g.setColour (static_cast<float> (i) / 10.0f <= value + 0.001f ? accent.withAlpha (0.8f) : Palette::borderStrong);
        g.drawLine (juce::Line<float> (centre + dir * r0, centre + dir * r1), 1.2f);
    }

    // Track
    g.setColour (Palette::well);
    g.strokePath (arc (start, end, radius), stroke (track + 2.0f));
    g.setColour (Palette::borderStrong.withAlpha (0.7f));
    g.strokePath (arc (start, end, radius), stroke (track - 2.0f));

    // Value arc with glow + gradient.
    if (value > 0.001f)
    {
        const auto valueArc = arc (start, valueAngle, radius);
        g.setColour (accent.withAlpha (0.12f));
        g.strokePath (valueArc, stroke (track + 10.0f));
        g.setColour (accent.withAlpha (0.2f));
        g.strokePath (valueArc, stroke (track + 4.0f));
        g.setGradientFill (juce::ColourGradient (accent.darker (0.45f), bounds.getX(), bounds.getBottom(), accent.brighter (0.25f), bounds.getRight(),
                                                 bounds.getY(), false));
        g.strokePath (valueArc, stroke (track));
    }

    // Applied (governed) share: thin inner arc.
    const float innerR = radius - track * 0.5f - 6.0f;
    g.setColour (Palette::border);
    g.strokePath (arc (start, end, innerR), stroke (2.0f));
    if (value > 0.001f)
    {
        const float applied = value * governor;
        g.setColour (governor < 0.985f ? Palette::amber : Palette::text.withAlpha (0.55f));
        g.strokePath (arc (start, start + applied * (end - start), innerR), stroke (2.0f));
    }

    // Thumb dot at the value.
    {
        const juce::Point<float> dir (std::sin (valueAngle), -std::cos (valueAngle));
        const auto p = centre + dir * radius;
        g.setColour (Palette::text);
        g.fillEllipse (juce::Rectangle<float> (track * 0.9f, track * 0.9f).withCentre (p));
        g.setColour (Palette::background.withAlpha (0.5f));
        g.drawEllipse (juce::Rectangle<float> (track * 0.9f, track * 0.9f).withCentre (p), 1.0f);
    }

    // Centre readout.
    const float textSize = juce::jlimit (22.0f, 44.0f, size * 0.24f);
    auto textArea = juce::Rectangle<float> (size * 0.7f, textSize * 1.1f).withCentre ({ centre.x, centre.y - textSize * 0.12f });
    g.setColour (Palette::text);
    g.setFont (Theme::numeric (textSize));
    g.drawText (juce::String (juce::roundToInt (value * 100.0f)), textArea, juce::Justification::centred, false);
    g.setColour (Palette::muted);
    g.setFont (Theme::caption (juce::jlimit (9.0f, 11.0f, size * 0.06f)));
    g.drawText ("BOOST %", textArea.translated (0.0f, textSize * 0.82f).withHeight (14.0f), juce::Justification::centred, false);

    if (hasKeyboardFocus (false))
    {
        g.setColour (accent.withAlpha (0.5f));
        g.drawEllipse (juce::Rectangle<float> (innerR * 2.0f - 10.0f, innerR * 2.0f - 10.0f).withCentre (centre), 1.0f);
    }
}

// =============================================================================
// BoostPanel
// =============================================================================
BoostPanel::BoostPanel (EngineController& c)
    : controller (c),
      binder ([this] { return &controller.getSelectedParams(); },
              [this] { return controller.getChain (controller.getSelectedStrip()).effectiveValues(); })
{
    addAndMakeVisible (dial);
    binder.bindSlider (dial, BoostIntensity);
    dial.setTooltip ("Boost Intensity: one control for the whole enhancement. Clarity and width come first, bass next and "
                     "loudness last, always watched by the safety governor.");

    for (size_t i = 0; i < macros.size(); ++i)
    {
        auto& knob = macros[i];
        knob.setKnobSize (ParamKnob::Size::Large);
        addAndMakeVisible (knob);
        binder.bindSlider (knob.slider, Macro1 + static_cast<int> (i));
    }
    setMode (ModeValue::Music);
}

void BoostPanel::setMode (ModeValue newMode)
{
    mode = newMode;
    for (size_t i = 0; i < macros.size(); ++i)
    {
        const auto name = EngineController::getMacroName (mode, static_cast<int> (i));
        macros[i].setLabel (name);
        macros[i].slider.setTitle (name);
        macros[i].slider.setTooltip (macroTip (mode, static_cast<int> (i)));
    }
    repaint();
}

void BoostPanel::setGovernorScale (float scale)
{
    const float before = dial.getGovernorScale();
    dial.setGovernorScale (scale);
    if (std::abs (before - dial.getGovernorScale()) > 0.002f)
        repaint (pillArea);
}

void BoostPanel::paint (juce::Graphics& g)
{
    Theme::drawPanel (g, getLocalBounds().toFloat());
    const auto accent = Theme::accent (*this);

    auto header = headerArea.toFloat();
    Theme::drawCaption (g, "BOOST INTENSITY", header.removeFromLeft (static_cast<float> (dialArea.getWidth()) + 20.0f));
    Theme::drawCaption (g, mode == ModeValue::Gaming ? "GAMING MACROS" : "MUSIC MACROS", header, Palette::muted);

    // Divider between the dial and the macros.
    g.setColour (Palette::border);
    g.fillRect (static_cast<float> (macroArea.getX()) - 12.0f, static_cast<float> (macroArea.getY()) + 8.0f, 1.0f,
                static_cast<float> (macroArea.getHeight()) - 16.0f);

    // Governor pill
    const float gov = dial.getGovernorScale();
    const bool limiting = gov < 0.985f;
    const auto pill = pillArea.toFloat();
    const auto colour = limiting ? Palette::amber : accent;
    g.setColour (colour.withAlpha (0.12f));
    g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
    g.setColour (colour.withAlpha (0.45f));
    g.drawRoundedRectangle (pill.reduced (0.5f), pill.getHeight() * 0.5f, 1.0f);
    auto content = pill.reduced (9.0f, 0.0f);
    g.setColour (colour);
    g.fillEllipse (content.removeFromLeft (6.0f).withSizeKeepingCentre (6.0f, 6.0f));
    content.removeFromLeft (6.0f);
    g.setColour (Palette::text.withAlpha (0.9f));
    g.setFont (Theme::font (11.0f));
    g.drawFittedText (limiting ? "Governor " + juce::String (juce::roundToInt (gov * 100.0f)) + "% applied" : juce::String ("Safety governor OK"),
                      content.toNearestInt(), juce::Justification::centredLeft, 1, 0.85f);
}

void BoostPanel::resized()
{
    auto r = getLocalBounds().reduced (14, 10);
    headerArea = r.removeFromTop (18);
    r.removeFromTop (4);

    // Dial: square on the left, sized by the available height.
    const int dialSize = juce::jlimit (96, 190, r.getHeight() - 26);
    auto left = r.removeFromLeft (juce::jmax (dialSize, 150));
    dialArea = left;
    pillArea = left.removeFromBottom (22).withSizeKeepingCentre (juce::jmin (left.getWidth(), 164), 22);
    left.removeFromBottom (2);
    dial.setBounds (left.withSizeKeepingCentre (dialSize, juce::jmin (dialSize, left.getHeight())));

    r.removeFromLeft (26);
    macroArea = r;

    // Macros: evenly spaced, knob size from the available height.
    const int n = static_cast<int> (macros.size());
    const int cellW = r.getWidth() / n;
    const int knobH = juce::jmin (r.getHeight(), 132);
    const int knobW = juce::jmin (cellW - 6, juce::jmax (78, knobH - 20));
    for (int i = 0; i < n; ++i)
    {
        auto cell = r.removeFromLeft (cellW);
        macros[static_cast<size_t> (i)].setBounds (cell.withSizeKeepingCentre (knobW, knobH));
    }
}
} // namespace flub::app::ui
