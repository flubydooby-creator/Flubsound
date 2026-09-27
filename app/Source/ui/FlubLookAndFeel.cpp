#include "FlubLookAndFeel.h"

#include "Widgets.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace flub::app::ui
{
namespace
{
/** Live instances, for Theme::setTheme (message thread only). */
std::vector<FlubLookAndFeel*>& instances()
{
    static std::vector<FlubLookAndFeel*> list;
    return list;
}

Icon tickIcon()
{
    Icon i;
    i.stroke.startNewSubPath (5.0f, 12.5f);
    i.stroke.lineTo (10.0f, 17.0f);
    i.stroke.lineTo (19.0f, 7.0f);
    return i;
}

juce::Path roundedShape (juce::Rectangle<float> r, float radius, const juce::Button& b)
{
    const bool flatL = b.isConnectedOnLeft(), flatR = b.isConnectedOnRight();
    const bool flatT = b.isConnectedOnTop(), flatB = b.isConnectedOnBottom();
    juce::Path p;
    p.addRoundedRectangle (r.getX(), r.getY(), r.getWidth(), r.getHeight(), radius, radius, ! (flatL || flatT), ! (flatR || flatT),
                           ! (flatL || flatB), ! (flatR || flatB));
    return p;
}
} // namespace

FlubLookAndFeel::FlubLookAndFeel()
{
    instances().push_back (this);

    // The whole app uses the same UI family, including JUCE's own dialogs.
    if (Theme::fontFamily() != juce::Font::getDefaultSansSerifFontName())
        setDefaultSansSerifTypefaceName (Theme::fontFamily());

    applyColours();
}

FlubLookAndFeel::~FlubLookAndFeel()
{
    auto& list = instances();
    list.erase (std::remove (list.begin(), list.end(), this), list.end());
}

void FlubLookAndFeel::forEachInstance (const std::function<void (FlubLookAndFeel&)>& fn)
{
    for (auto* lnf : instances())
        fn (*lnf);
}

void FlubLookAndFeel::applyPalette (const PaletteTokens& previous)
{
    accent = Theme::remapColour (accent, previous, Palette::detail::active);
    applyColours();
}

void FlubLookAndFeel::applyColours()
{
    using Scheme = juce::LookAndFeel_V4::ColourScheme;
    auto scheme = getDarkColourScheme();
    scheme.setUIColour (Scheme::windowBackground, Palette::background);
    scheme.setUIColour (Scheme::widgetBackground, Palette::panelRaised);
    scheme.setUIColour (Scheme::menuBackground, Palette::menu);
    scheme.setUIColour (Scheme::outline, Palette::borderStrong);
    scheme.setUIColour (Scheme::defaultText, Palette::text);
    scheme.setUIColour (Scheme::defaultFill, accent);
    scheme.setUIColour (Scheme::highlightedText, Palette::text);
    scheme.setUIColour (Scheme::highlightedFill, accent.withAlpha (0.25f));
    scheme.setUIColour (Scheme::menuText, Palette::text);
    setColourScheme (scheme);

    // Everything that does not depend on the accent.
    setColour (juce::ResizableWindow::backgroundColourId, Palette::background);
    setColour (juce::DocumentWindow::textColourId, Palette::text);

    setColour (juce::Label::textColourId, Palette::text);
    setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textWhenEditingColourId, Palette::text);
    setColour (juce::Label::backgroundWhenEditingColourId, Palette::well);

    setColour (juce::TextButton::buttonColourId, Palette::panelRaised);
    setColour (juce::TextButton::textColourOffId, Palette::text.withAlpha (0.88f));
    setColour (juce::TextButton::textColourOnId, Palette::text);
    setColour (juce::ToggleButton::textColourId, Palette::text.withAlpha (0.9f));
    setColour (juce::ToggleButton::tickDisabledColourId, Palette::faint);

    setColour (juce::ComboBox::backgroundColourId, Palette::panelRaised);
    setColour (juce::ComboBox::textColourId, Palette::text);
    setColour (juce::ComboBox::outlineColourId, Palette::borderStrong);
    setColour (juce::ComboBox::buttonColourId, Palette::panelRaised);
    setColour (juce::ComboBox::arrowColourId, Palette::muted);

    setColour (juce::PopupMenu::backgroundColourId, Palette::menu);
    setColour (juce::PopupMenu::textColourId, Palette::text);
    setColour (juce::PopupMenu::headerTextColourId, Palette::muted);
    setColour (juce::PopupMenu::highlightedTextColourId, Palette::text);

    setColour (juce::Slider::backgroundColourId, Palette::track);
    setColour (juce::Slider::rotarySliderOutlineColourId, Palette::track);
    setColour (juce::Slider::thumbColourId, Palette::text);
    setColour (juce::Slider::textBoxTextColourId, Palette::text.withAlpha (0.9f));
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);

    setColour (juce::TooltipWindow::backgroundColourId, Palette::tooltip);
    setColour (juce::TooltipWindow::textColourId, Palette::text);
    setColour (juce::TooltipWindow::outlineColourId, Palette::borderStrong);

    setColour (juce::ScrollBar::backgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);
    setColour (juce::ScrollBar::thumbColourId, Palette::scrollThumb);

    setColour (juce::TextEditor::backgroundColourId, Palette::well);
    setColour (juce::TextEditor::textColourId, Palette::text);
    setColour (juce::TextEditor::outlineColourId, Palette::borderStrong);
    setColour (juce::TextEditor::shadowColourId, juce::Colours::transparentBlack);
    setColour (juce::TextEditor::highlightedTextColourId, Palette::text);

    setColour (juce::AlertWindow::backgroundColourId, Palette::panel);
    setColour (juce::AlertWindow::textColourId, Palette::text);
    setColour (juce::AlertWindow::outlineColourId, Palette::borderStrong);

    setColour (juce::ListBox::backgroundColourId, Palette::well);
    setColour (juce::ListBox::outlineColourId, Palette::border);
    setColour (juce::ListBox::textColourId, Palette::text);
    setColour (juce::GroupComponent::outlineColourId, Palette::border);
    setColour (juce::GroupComponent::textColourId, Palette::muted);
    setColour (juce::HyperlinkButton::textColourId, Palette::teal);

    applyAccentColours();
}

void FlubLookAndFeel::setAccent (juce::Colour newAccent)
{
    if (newAccent == accent)
        return;
    accent = newAccent;
    applyAccentColours();
}

void FlubLookAndFeel::applyAccentColours()
{
    setColour (juce::TextButton::buttonOnColourId, accent.withAlpha (0.2f));
    setColour (juce::ToggleButton::tickColourId, accent);
    setColour (juce::ComboBox::focusedOutlineColourId, accent);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.16f));
    setColour (juce::Slider::rotarySliderFillColourId, accent);
    setColour (juce::Slider::trackColourId, accent);
    setColour (juce::Slider::textBoxHighlightColourId, accent.withAlpha (0.35f));
    setColour (juce::TextEditor::focusedOutlineColourId, accent);
    setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.35f));
    setColour (juce::CaretComponent::caretColourId, accent);
    setColour (juce::Label::outlineWhenEditingColourId, accent);
    setColour (juce::HyperlinkButton::textColourId, accent);
}

juce::String FlubLookAndFeel::styleOf (const juce::Component& c)
{
    return c.getProperties()[styleProperty].toString();
}

// =============================================================================
// Sliders
// =============================================================================
void FlubLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos, float startAngle,
                                        float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const float size = juce::jmin (bounds.getWidth(), bounds.getHeight());
    if (size < 8.0f)
        return;

    const auto centre = bounds.getCentre();
    const bool enabled = slider.isEnabled();
    const auto fill = enabled ? accent : Palette::faint;
    const float track = juce::jlimit (2.5f, 6.0f, size * 0.075f);
    const float radius = size * 0.5f - track * 0.5f - 3.0f; // leaves room for the effective-value ring

    auto arc = [&] (float from, float to, float r)
    {
        juce::Path p;
        p.addCentredArc (centre.x, centre.y, r, r, 0.0f, juce::jmin (from, to), juce::jmax (from, to), true);
        return p;
    };
    const auto rounded = [] (float w) { return juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded); };

    // ---- Track + value arc (bipolar parameters grow from their zero point) ----
    g.setColour (Palette::track);
    g.strokePath (arc (startAngle, endAngle, radius), rounded (track));

    float zeroPos = 0.0f;
    if (slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0)
        zeroPos = static_cast<float> (slider.valueToProportionOfLength (0.0));
    const float zeroAngle = startAngle + zeroPos * (endAngle - startAngle);
    const float valueAngle = startAngle + sliderPos * (endAngle - startAngle);

    if (std::abs (valueAngle - zeroAngle) > 0.002f)
    {
        const auto valueArc = arc (zeroAngle, valueAngle, radius);
        if (enabled)
        {
            g.setColour (fill.withAlpha (0.16f));
            g.strokePath (valueArc, rounded (track + 4.0f));
        }
        g.setColour (fill);
        g.strokePath (valueArc, rounded (track));
    }

    // ---- Effective (post-macro) value: thin outer ring + dot ----
    if (const auto* effective = slider.getProperties().getVarPointer (effectiveProperty))
    {
        const auto e = static_cast<float> (static_cast<double> (*effective));
        if (std::abs (e - sliderPos) > 0.004f)
        {
            const float effAngle = startAngle + juce::jlimit (0.0f, 1.0f, e) * (endAngle - startAngle);
            const float ringR = radius + track * 0.5f + 2.0f;
            g.setColour (Palette::text.withAlpha (enabled ? 0.8f : 0.3f));
            g.strokePath (arc (valueAngle, effAngle, ringR), rounded (1.5f));
            const auto dot = juce::Point<float> (centre.x + ringR * std::sin (effAngle), centre.y - ringR * std::cos (effAngle));
            g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre (dot));
        }
    }

    // ---- Knob body ----
    const float bodyR = radius - track * 0.5f - juce::jmax (2.0f, size * 0.07f);
    const auto body = juce::Rectangle<float> (bodyR * 2.0f, bodyR * 2.0f).withCentre (centre);
    g.setGradientFill (juce::ColourGradient (Palette::knobTop, body.getX(), body.getY(), Palette::knobBottom, body.getX(),
                                             body.getBottom(), false));
    g.fillEllipse (body);
    g.setColour (Palette::knobRim);
    g.drawEllipse (body.reduced (0.5f), 1.0f);
    if (size > 36.0f)
    {
        g.setColour (Palette::highlight.withAlpha (0.05f));
        g.strokePath (arc (-1.1f, 1.1f, bodyR - 1.5f), rounded (1.0f));
    }

    // ---- Pointer ----
    const float inner = bodyR * 0.28f, outer = bodyR * 0.84f;
    const juce::Point<float> dir (std::sin (valueAngle), -std::cos (valueAngle));
    g.setColour (enabled ? Palette::text : Palette::muted);
    g.drawLine (juce::Line<float> (centre + dir * inner, centre + dir * outer), juce::jmax (1.6f, size * 0.035f));

    if (slider.hasKeyboardFocus (true))
    {
        g.setColour (accent.withAlpha (0.55f));
        g.drawEllipse (body.expanded (2.0f), 1.0f);
    }
}

void FlubLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos, float minSliderPos,
                                        float maxSliderPos, juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (slider.isBar() || slider.isTwoValue() || slider.isThreeValue())
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const bool horizontal = slider.isHorizontal();
    const bool enabled = slider.isEnabled();
    const float thickness = 4.0f;

    const auto trackRect = horizontal ? juce::Rectangle<float> (bounds.getX(), bounds.getCentreY() - thickness * 0.5f, bounds.getWidth(), thickness)
                                      : juce::Rectangle<float> (bounds.getCentreX() - thickness * 0.5f, bounds.getY(), thickness, bounds.getHeight());
    g.setColour (Palette::track);
    g.fillRoundedRectangle (trackRect, thickness * 0.5f);

    float zeroCoord = horizontal ? bounds.getX() : bounds.getBottom();
    if (slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0)
        zeroCoord = static_cast<float> (slider.getPositionOfValue (0.0));

    const float a = juce::jmin (zeroCoord, sliderPos), b = juce::jmax (zeroCoord, sliderPos);
    const auto valueRect = horizontal ? trackRect.withX (a).withRight (b) : trackRect.withY (a).withBottom (b);
    g.setColour (enabled ? accent : Palette::faint);
    g.fillRoundedRectangle (valueRect, thickness * 0.5f);

    const float thumb = 12.0f;
    const auto thumbCentre = horizontal ? juce::Point<float> (sliderPos, bounds.getCentreY()) : juce::Point<float> (bounds.getCentreX(), sliderPos);
    const auto thumbRect = juce::Rectangle<float> (thumb, thumb).withCentre (thumbCentre);
    g.setColour (enabled ? Palette::text : Palette::muted);
    g.fillEllipse (thumbRect);
    g.setColour (Palette::background.withAlpha (0.6f));
    g.drawEllipse (thumbRect.reduced (0.5f), 1.0f);

    if (slider.hasKeyboardFocus (true))
    {
        g.setColour (accent.withAlpha (0.6f));
        g.drawEllipse (thumbRect.expanded (2.5f), 1.0f);
    }
}

int FlubLookAndFeel::getSliderThumbRadius (juce::Slider& slider)
{
    return slider.isRotary() ? LookAndFeel_V4::getSliderThumbRadius (slider) : 7;
}

juce::Label* FlubLookAndFeel::createSliderTextBox (juce::Slider& slider)
{
    auto* l = LookAndFeel_V4::createSliderTextBox (slider);
    l->setFont (Theme::numeric (12.0f, false));
    l->setJustificationType (juce::Justification::centred);
    l->setColour (juce::Label::textColourId, Palette::text.withAlpha (0.86f));
    l->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    l->setColour (juce::Label::backgroundWhenEditingColourId, Palette::well);
    l->setColour (juce::Label::outlineWhenEditingColourId, accent);
    l->setColour (juce::Label::textWhenEditingColourId, Palette::text);
    l->setMinimumHorizontalScale (0.8f);
    return l;
}

// =============================================================================
// Buttons
// =============================================================================
juce::Font FlubLookAndFeel::getTextButtonFont (juce::TextButton& b, int buttonHeight)
{
    const auto style = styleOf (b);
    const bool bold = style == "tab";
    if (style == "chip")
        return Theme::font (juce::jmin (11.5f, static_cast<float> (buttonHeight) * 0.5f), true);
    return Theme::font (juce::jmin (13.0f, static_cast<float> (buttonHeight) * 0.5f), bold);
}

void FlubLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool isHighlighted, bool isDown)
{
    const auto bounds = b.getLocalBounds().toFloat().reduced (0.5f);
    const auto style = styleOf (b);
    const bool on = b.getToggleState();
    const auto shape = roundedShape (bounds, Theme::kControlRadius, b);

    if (style == "tab")
    {
        // Neutral selection (the accent is reserved for the mode switch): a
        // raised fill plus an accent underline.
        if (on || isHighlighted || isDown)
        {
            g.setColour (on ? Palette::tabOn : Palette::panelHover.withAlpha (isDown ? 0.9f : 0.6f));
            g.fillPath (shape);
        }
        if (on)
        {
            g.setColour (accent);
            g.fillRoundedRectangle (bounds.getX() + bounds.getWidth() * 0.3f, bounds.getBottom() - 3.0f, bounds.getWidth() * 0.4f, 2.0f, 1.0f);
        }
    }
    else if (style == "chip")
    {
        // Small pill toggles (analyser options): outline when off, tinted when on.
        const float r = bounds.getHeight() * 0.5f;
        if (on)
        {
            g.setColour (accent.withAlpha (isDown ? 0.22f : 0.13f));
            g.fillRoundedRectangle (bounds, r);
        }
        else if (isHighlighted || isDown)
        {
            g.setColour (Palette::panelHover);
            g.fillRoundedRectangle (bounds, r);
        }
        g.setColour (on ? accent.withAlpha (0.5f) : Palette::borderStrong);
        g.drawRoundedRectangle (bounds.reduced (0.5f), r, 1.0f);
    }
    else if (style == "warning" && on)
    {
        g.setColour (Palette::amber.withAlpha (isDown ? 0.28f : (isHighlighted ? 0.24f : 0.18f)));
        g.fillPath (shape);
        g.setColour (Palette::amber.withAlpha (0.8f));
        g.strokePath (shape, juce::PathStrokeType (1.0f));
    }
    else
    {
        const auto base = isDown ? Palette::panel : (isHighlighted ? Palette::panelHover : Palette::panelRaised);
        g.setGradientFill (Theme::sheen (bounds, base, 0.05f));
        g.fillPath (shape);
        if (on)
        {
            g.setColour (accent.withAlpha (0.14f));
            g.fillPath (shape);
        }
        g.setColour (on ? accent.withAlpha (0.65f) : (isHighlighted ? Palette::borderStrong.brighter (0.25f) : Palette::borderStrong));
        g.strokePath (shape, juce::PathStrokeType (1.0f));
    }

    if (b.hasKeyboardFocus (false))
    {
        g.setColour (accent.withAlpha (0.75f));
        g.strokePath (roundedShape (bounds.reduced (1.0f), Theme::kControlRadius - 1.0f, b), juce::PathStrokeType (1.2f));
    }
}

void FlubLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool isHighlighted, bool)
{
    const auto style = styleOf (b);
    const bool on = b.getToggleState();

    juce::Colour colour = isHighlighted ? Palette::text : Palette::text.withAlpha (0.86f);
    if (style == "tab")
        colour = on ? Palette::text : (isHighlighted ? Palette::text.withAlpha (0.9f) : Palette::muted);
    else if (style == "chip")
        colour = on ? accent.brighter (0.25f) : (isHighlighted ? Palette::text : Palette::muted);
    else if (style == "warning" && on)
        colour = Palette::amber;
    else if (on)
        colour = Palette::text;
    if (! b.isEnabled())
        colour = Palette::faint;

    g.setColour (colour);
    g.setFont (getTextButtonFont (b, b.getHeight()));
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (6, 0), juce::Justification::centred, 1, 0.85f);
}

void FlubLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool isHighlighted, bool isDown)
{
    const auto style = styleOf (b);
    auto bounds = b.getLocalBounds().toFloat();
    const bool on = b.getToggleState();
    const bool enabled = b.isEnabled();
    const auto textColour = enabled ? (isHighlighted ? Palette::text : Palette::text.withAlpha (0.88f)) : Palette::faint;

    if (style == "power")
    {
        const float d = juce::jmin (bounds.getHeight(), 24.0f);
        const auto circle = bounds.removeFromLeft (d).withSizeKeepingCentre (d, d).reduced (1.0f);
        const auto colour = ! enabled ? Palette::faint : (on ? accent : (isHighlighted ? Palette::text : Palette::muted));
        if (on && enabled)
        {
            g.setColour (accent.withAlpha (isDown ? 0.3f : 0.16f));
            g.fillEllipse (circle);
        }
        else if (isHighlighted)
        {
            g.setColour (Palette::panelHover);
            g.fillEllipse (circle);
        }
        g.setColour (on && enabled ? accent.withAlpha (0.8f) : Palette::borderStrong);
        g.drawEllipse (circle.reduced (0.5f), 1.0f);
        drawIcon (g, Icons::power(), circle.reduced (d * 0.24f), colour, 2.0f);

        if (b.hasKeyboardFocus (false))
        {
            g.setColour (accent.withAlpha (0.7f));
            g.drawEllipse (circle.expanded (1.5f), 1.0f);
        }
        if (b.getButtonText().isNotEmpty())
        {
            g.setColour (textColour);
            g.setFont (Theme::font (13.0f, true));
            g.drawText (b.getButtonText(), bounds.withTrimmedLeft (8.0f), juce::Justification::centredLeft, true);
        }
        return;
    }

    if (style == "switch")
    {
        const float h = juce::jmin (bounds.getHeight() - 4.0f, 16.0f);
        const float w = h * 1.8f;
        auto pill = bounds.removeFromLeft (w + 2.0f).withSizeKeepingCentre (w, h);
        g.setColour (on && enabled ? accent.withAlpha (0.85f) : Palette::borderStrong);
        g.fillRoundedRectangle (pill, h * 0.5f);
        const float knob = h - 4.0f;
        const auto knobRect = juce::Rectangle<float> (knob, knob).withCentre ({ on ? pill.getRight() - h * 0.5f : pill.getX() + h * 0.5f,
                                                                                pill.getCentreY() });
        g.setColour (on && enabled ? Palette::background : Palette::text.withAlpha (enabled ? 0.85f : 0.4f));
        g.fillEllipse (knobRect);
        if (b.hasKeyboardFocus (false))
        {
            g.setColour (accent.withAlpha (0.7f));
            g.drawRoundedRectangle (pill.expanded (2.0f), h * 0.5f + 2.0f, 1.0f);
        }
        g.setColour (textColour);
        g.setFont (Theme::font (13.0f));
        g.drawText (b.getButtonText(), bounds.withTrimmedLeft (8.0f), juce::Justification::centredLeft, true);
        return;
    }

    const float box = juce::jmin (16.0f, bounds.getHeight() - 4.0f);
    drawTickBox (g, b, bounds.getX() + 1.0f, bounds.getCentreY() - box * 0.5f, box, box, on, enabled, isHighlighted, isDown);
    g.setColour (textColour);
    g.setFont (Theme::font (13.0f));
    g.drawText (b.getButtonText(), bounds.withTrimmedLeft (box + 9.0f), juce::Justification::centredLeft, true);
}

void FlubLookAndFeel::drawTickBox (juce::Graphics& g, juce::Component& c, float x, float y, float w, float h, bool ticked, bool isEnabled,
                                   bool isHighlighted, bool)
{
    const juce::Rectangle<float> r (x, y, w, h);
    if (ticked)
    {
        g.setColour (isEnabled ? accent : Palette::faint);
        g.fillRoundedRectangle (r, 3.5f);
        juce::Path tick;
        tick.startNewSubPath (r.getX() + w * 0.24f, r.getCentreY());
        tick.lineTo (r.getX() + w * 0.43f, r.getY() + h * 0.7f);
        tick.lineTo (r.getX() + w * 0.77f, r.getY() + h * 0.3f);
        g.setColour (Palette::background);
        g.strokePath (tick, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    else
    {
        g.setColour (isHighlighted ? Palette::panelHover : Palette::well);
        g.fillRoundedRectangle (r, 3.5f);
        g.setColour (isHighlighted ? Palette::muted : Palette::borderStrong);
        g.drawRoundedRectangle (r.reduced (0.5f), 3.5f, 1.0f);
    }
    if (c.hasKeyboardFocus (false))
    {
        g.setColour (accent.withAlpha (0.7f));
        g.drawRoundedRectangle (r.expanded (2.0f), 5.0f, 1.0f);
    }
}

// =============================================================================
// Combo boxes / popup menus
// =============================================================================
void FlubLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown, int, int, int, int, juce::ComboBox& box)
{
    const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, static_cast<float> (width), static_cast<float> (height)).reduced (0.5f);
    const bool hover = box.isMouseOver (true);
    const auto base = isButtonDown ? Palette::panel : (hover ? Palette::panelHover : Palette::panelRaised);
    g.setGradientFill (Theme::sheen (bounds, base, 0.05f));
    g.fillRoundedRectangle (bounds, Theme::kControlRadius);
    g.setColour (box.hasKeyboardFocus (true) ? accent.withAlpha (0.75f) : (hover ? Palette::borderStrong.brighter (0.25f) : Palette::borderStrong));
    g.drawRoundedRectangle (bounds, Theme::kControlRadius, 1.0f);

    const float arrow = juce::jmin (12.0f, static_cast<float> (height) * 0.45f);
    const auto arrowArea = juce::Rectangle<float> (bounds.getRight() - arrow - 10.0f, bounds.getCentreY() - arrow * 0.5f, arrow, arrow);
    drawIcon (g, Icons::chevronDown(), arrowArea, box.isEnabled() ? Palette::muted : Palette::faint, 2.0f);
}

juce::Font FlubLookAndFeel::getComboBoxFont (juce::ComboBox& box)
{
    return Theme::font (juce::jmin (13.0f, static_cast<float> (box.getHeight()) * 0.5f));
}

void FlubLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (8, 1, box.getWidth() - 30, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
    label.setMinimumHorizontalScale (0.9f);
}

void FlubLookAndFeel::drawComboBoxTextWhenNothingSelected (juce::Graphics& g, juce::ComboBox& box, juce::Label& label)
{
    g.setColour (Palette::muted);
    g.setFont (getComboBoxFont (box));
    g.drawFittedText (box.getTextWhenNothingSelected(), label.getBounds().reduced (2, 0), label.getJustificationType(), 1, 0.9f);
}

void FlubLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (Palette::menu);
    g.setColour (Palette::borderStrong);
    g.drawRect (0, 0, width, height, 1);
}

void FlubLookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                                         bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                                         const juce::String& shortcutKeyText, const juce::Drawable* icon, const juce::Colour* textColour)
{
    if (isSeparator)
    {
        g.setColour (Palette::border);
        g.fillRect (area.reduced (10, 0).withHeight (1).withY (area.getCentreY()));
        return;
    }

    auto r = area.toFloat();
    if (isHighlighted && isActive)
    {
        g.setColour (accent.withAlpha (0.16f));
        g.fillRoundedRectangle (r.reduced (4.0f, 1.0f), 4.0f);
    }

    auto content = r.reduced (12.0f, 0.0f);
    auto tickArea = content.removeFromLeft (16.0f);
    if (isTicked)
        drawIcon (g, tickIcon(), tickArea.withSizeKeepingCentre (12.0f, 12.0f), accent, 2.4f);
    else if (icon != nullptr)
        icon->drawWithin (g, tickArea.withSizeKeepingCentre (12.0f, 12.0f), juce::RectanglePlacement::centred, 1.0f);
    content.removeFromLeft (6.0f);

    auto colour = textColour != nullptr ? *textColour : Palette::text;
    if (! isActive)
        colour = Palette::faint;
    else if (! isHighlighted && textColour == nullptr)
        colour = Palette::text.withAlpha (0.9f);

    if (hasSubMenu)
    {
        drawIcon (g, Icons::chevronRight(), content.removeFromRight (12.0f).withSizeKeepingCentre (10.0f, 10.0f), Palette::muted, 2.0f);
        content.removeFromRight (6.0f);
    }

    g.setFont (getPopupMenuFont());
    if (shortcutKeyText.isNotEmpty())
    {
        g.setColour (Palette::muted);
        g.drawText (shortcutKeyText, content, juce::Justification::centredRight, true);
        content.removeFromRight (juce::GlyphArrangement::getStringWidth (getPopupMenuFont(), shortcutKeyText) + 10.0f);
    }
    g.setColour (colour);
    g.drawFittedText (text, content.toNearestInt(), juce::Justification::centredLeft, 1, 0.9f);
}

void FlubLookAndFeel::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& sectionName)
{
    auto r = area.toFloat().reduced (12.0f, 0.0f);
    r.removeFromTop (r.getHeight() * 0.3f);
    Theme::drawCaption (g, sectionName.toUpperCase(), r, Palette::muted);
}

void FlubLookAndFeel::getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight, int& idealWidth,
                                                 int& idealHeight)
{
    if (isSeparator)
    {
        idealWidth = 60;
        idealHeight = 9;
        return;
    }
    idealHeight = standardMenuItemHeight > 0 ? juce::jmax (standardMenuItemHeight, 26) : 28;
    idealWidth = static_cast<int> (juce::GlyphArrangement::getStringWidth (getPopupMenuFont(), text)) + 70;
}

juce::Font FlubLookAndFeel::getPopupMenuFont()
{
    return Theme::font (13.0f);
}

int FlubLookAndFeel::getPopupMenuBorderSize()
{
    return 5;
}

// =============================================================================
// Misc
// =============================================================================
int FlubLookAndFeel::getDefaultScrollbarWidth()
{
    return 10;
}

void FlubLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height, bool isScrollbarVertical,
                                     int thumbStartPosition, int thumbSize, bool isMouseOver, bool isMouseDown)
{
    juce::Rectangle<float> thumb;
    if (isScrollbarVertical)
        thumb = juce::Rectangle<float> (static_cast<float> (x), static_cast<float> (thumbStartPosition), static_cast<float> (width),
                                        static_cast<float> (thumbSize));
    else
        thumb = juce::Rectangle<float> (static_cast<float> (thumbStartPosition), static_cast<float> (y), static_cast<float> (thumbSize),
                                        static_cast<float> (height));
    // Faint track so a scrollable area reads as such, then the thumb.
    const auto track = juce::Rectangle<float> (static_cast<float> (x), static_cast<float> (y), static_cast<float> (width),
                                               static_cast<float> (height))
                           .reduced (3.0f);
    g.setColour (Palette::highlight.withAlpha (0.035f));
    g.fillRoundedRectangle (track, juce::jmin (track.getWidth(), track.getHeight()) * 0.5f);

    thumb = thumb.reduced (isMouseOver || isMouseDown ? 2.0f : 3.0f);
    g.setColour (isMouseDown ? accent.withAlpha (0.7f) : (isMouseOver ? Palette::scrollThumbHover : Palette::scrollThumb));
    g.fillRoundedRectangle (thumb, juce::jmin (thumb.getWidth(), thumb.getHeight()) * 0.5f);
}

juce::TextLayout FlubLookAndFeel::layoutTooltip (const juce::String& text, juce::Colour colour)
{
    juce::AttributedString s;
    s.setJustification (juce::Justification::topLeft);
    s.append (text, Theme::font (12.5f), colour);
    juce::TextLayout tl;
    tl.createLayoutWithBalancedLineLengths (s, 300.0f);
    return tl;
}

juce::Rectangle<int> FlubLookAndFeel::getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea)
{
    const auto tl = layoutTooltip (tipText, Palette::text);
    const int w = static_cast<int> (std::ceil (tl.getWidth())) + 22;
    const int h = static_cast<int> (std::ceil (tl.getHeight())) + 14;
    return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 20,
                                 screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 8) : screenPos.y + 10, w, h)
        .constrainedWithin (parentArea);
}

void FlubLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, static_cast<float> (width), static_cast<float> (height));
    g.setColour (Palette::tooltip);
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (Palette::borderStrong);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);
    layoutTooltip (text, Palette::text).draw (g, bounds.reduced (11.0f, 7.0f));
}

void FlubLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    // Inline editors of labels (slider value boxes) fill their label exactly.
    if (dynamic_cast<juce::Label*> (editor.getParentComponent()) != nullptr)
    {
        g.fillAll (Palette::well);
        return;
    }
    g.setColour (editor.findColour (juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle (juce::Rectangle<float> (0.0f, 0.0f, static_cast<float> (width), static_cast<float> (height)), 5.0f);
}

void FlubLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    if (! editor.isEnabled())
        return;
    const auto r = juce::Rectangle<float> (0.0f, 0.0f, static_cast<float> (width), static_cast<float> (height)).reduced (0.5f);
    const bool focused = editor.hasKeyboardFocus (true) && ! editor.isReadOnly();
    g.setColour (focused ? accent.withAlpha (0.85f) : Palette::borderStrong);
    g.drawRoundedRectangle (r, 5.0f, focused ? 1.3f : 1.0f);
}

juce::Font FlubLookAndFeel::getAlertWindowTitleFont()
{
    return Theme::font (16.0f, true);
}

juce::Font FlubLookAndFeel::getAlertWindowMessageFont()
{
    return Theme::font (14.0f);
}

juce::Font FlubLookAndFeel::getAlertWindowFont()
{
    return Theme::font (13.0f);
}

juce::Font FlubLookAndFeel::getLabelFont (juce::Label& label)
{
    return label.getFont();
}

void FlubLookAndFeel::drawLabel (juce::Graphics& g, juce::Label& label)
{
    if (dynamic_cast<juce::ComboBox*> (label.getParentComponent()) == nullptr || label.isBeingEdited())
    {
        LookAndFeel_V4::drawLabel (g, label);
        return;
    }
    const float alpha = label.isEnabled() ? 1.0f : 0.5f;
    g.fillAll (label.findColour (juce::Label::backgroundColourId).withMultipliedAlpha (alpha));
    g.setColour (label.findColour (juce::Label::textColourId).withMultipliedAlpha (alpha));
    g.setFont (getLabelFont (label));
    const auto area = getLabelBorderSize (label).subtractedFrom (label.getLocalBounds());
    g.drawText (label.getText(), area, label.getJustificationType(), true);
}

void FlubLookAndFeel::drawLevelMeter (juce::Graphics& g, int width, int height, float level)
{
    const auto r = juce::Rectangle<float> (0.0f, 0.0f, static_cast<float> (width), static_cast<float> (height)).withSizeKeepingCentre (
        static_cast<float> (width), juce::jmin (8.0f, static_cast<float> (height)));
    g.setColour (Palette::well);
    g.fillRoundedRectangle (r, 3.0f);
    g.setColour (Palette::border);
    g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
    // `level` is a linear gain; show it on a dB-like curve.
    const float amount = juce::jlimit (0.0f, 1.0f, std::sqrt (juce::jmax (0.0f, level)) * 1.2f);
    if (amount > 0.0f)
    {
        g.setColour (amount > 0.95f ? Theme::statusColours (meterPalette).hot : accent);
        g.fillRoundedRectangle (r.reduced (1.0f).withWidth ((r.getWidth() - 2.0f) * amount), 2.0f);
    }
}

void FlubLookAndFeel::drawCallOutBoxBackground (juce::CallOutBox&, juce::Graphics& g, const juce::Path& path, juce::Image&)
{
    g.setColour (Palette::shadow.withAlpha (0.4f));
    g.fillPath (path, juce::AffineTransform::translation (0.0f, 3.0f));
    g.setColour (Palette::panelRaised);
    g.fillPath (path);
    g.setColour (Palette::borderStrong);
    g.strokePath (path, juce::PathStrokeType (1.0f));
}
} // namespace flub::app::ui
