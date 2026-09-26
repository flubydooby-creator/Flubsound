#include "Widgets.h"

#include "FlubLookAndFeel.h"

#include <cmath>

namespace flub::app::ui
{
namespace
{
constexpr float kPi = juce::MathConstants<float>::pi;

/** Point on a circle, angle measured clockwise from 12 o'clock (JUCE arc convention). */
juce::Point<float> onCircle (float cx, float cy, float r, float angle)
{
    return { cx + r * std::sin (angle), cy - r * std::cos (angle) };
}

void polyline (juce::Path& p, std::initializer_list<juce::Point<float>> points)
{
    bool first = true;
    for (const auto& pt : points)
    {
        if (first)
            p.startNewSubPath (pt);
        else
            p.lineTo (pt);
        first = false;
    }
}

/** Arrow head at `tip`, pointing along `direction` (unit vector). */
void arrowHead (juce::Path& p, juce::Point<float> tip, juce::Point<float> direction, float size)
{
    const juce::Point<float> normal (-direction.y, direction.x);
    polyline (p, { tip - direction * size + normal * size, tip, tip - direction * size - normal * size });
}
} // namespace

// =============================================================================
// Icons (24 x 24 design grid)
// =============================================================================
namespace Icons
{
Icon gear()
{
    Icon i;
    constexpr int teeth = 8;
    constexpr float outer = 9.8f, inner = 7.4f;
    for (int t = 0; t < teeth; ++t)
    {
        const float a = static_cast<float> (t) * 2.0f * kPi / teeth;
        const auto p0 = onCircle (12.0f, 12.0f, inner, a - 0.30f);
        const auto p1 = onCircle (12.0f, 12.0f, outer, a - 0.16f);
        const auto p2 = onCircle (12.0f, 12.0f, outer, a + 0.16f);
        const auto p3 = onCircle (12.0f, 12.0f, inner, a + 0.30f);
        if (t == 0)
            i.stroke.startNewSubPath (p0);
        else
            i.stroke.lineTo (p0);
        i.stroke.lineTo (p1);
        i.stroke.lineTo (p2);
        i.stroke.lineTo (p3);
    }
    i.stroke.closeSubPath();
    i.stroke.addEllipse (8.8f, 8.8f, 6.4f, 6.4f);
    return i;
}

Icon chevronLeft()
{
    Icon i;
    polyline (i.stroke, { { 14.5f, 6.0f }, { 8.5f, 12.0f }, { 14.5f, 18.0f } });
    return i;
}

Icon chevronRight()
{
    Icon i;
    polyline (i.stroke, { { 9.5f, 6.0f }, { 15.5f, 12.0f }, { 9.5f, 18.0f } });
    return i;
}

Icon chevronDown()
{
    Icon i;
    polyline (i.stroke, { { 6.0f, 9.5f }, { 12.0f, 15.5f }, { 18.0f, 9.5f } });
    return i;
}

Icon more()
{
    Icon i;
    for (const float x : { 5.5f, 12.0f, 18.5f })
        i.fill.addEllipse (x - 1.8f, 10.2f, 3.6f, 3.6f);
    return i;
}

Icon copy()
{
    Icon i;
    i.stroke.addRoundedRectangle (9.0f, 9.0f, 11.0f, 11.0f, 2.2f);
    i.stroke.startNewSubPath (5.0f, 15.0f);
    i.stroke.lineTo (5.0f, 6.5f);
    i.stroke.quadraticTo (5.0f, 4.5f, 7.0f, 4.5f);
    i.stroke.lineTo (15.0f, 4.5f);
    return i;
}

Icon power()
{
    Icon i;
    i.stroke.addCentredArc (12.0f, 13.0f, 7.5f, 7.5f, 0.0f, 0.75f, 2.0f * kPi - 0.75f, true);
    polyline (i.stroke, { { 12.0f, 3.5f }, { 12.0f, 11.5f } });
    return i;
}

Icon expand()
{
    Icon i;
    polyline (i.stroke, { { 14.0f, 4.0f }, { 20.0f, 4.0f }, { 20.0f, 10.0f } });
    polyline (i.stroke, { { 20.0f, 4.0f }, { 13.5f, 10.5f } });
    polyline (i.stroke, { { 10.0f, 20.0f }, { 4.0f, 20.0f }, { 4.0f, 14.0f } });
    polyline (i.stroke, { { 4.0f, 20.0f }, { 10.5f, 13.5f } });
    return i;
}

Icon collapse()
{
    Icon i;
    polyline (i.stroke, { { 14.0f, 5.0f }, { 14.0f, 10.0f }, { 19.0f, 10.0f } });
    polyline (i.stroke, { { 14.0f, 10.0f }, { 20.0f, 4.0f } });
    polyline (i.stroke, { { 10.0f, 19.0f }, { 10.0f, 14.0f }, { 5.0f, 14.0f } });
    polyline (i.stroke, { { 10.0f, 14.0f }, { 4.0f, 20.0f } });
    return i;
}

Icon ear()
{
    Icon i;
    i.stroke.addCentredArc (12.0f, 13.5f, 8.0f, 8.0f, 0.0f, -kPi * 0.5f, kPi * 0.5f, true);
    i.fill.addRoundedRectangle (3.0f, 13.0f, 4.6f, 7.5f, 1.8f);
    i.fill.addRoundedRectangle (16.4f, 13.0f, 4.6f, 7.5f, 1.8f);
    return i;
}

Icon plus()
{
    Icon i;
    polyline (i.stroke, { { 12.0f, 5.0f }, { 12.0f, 19.0f } });
    polyline (i.stroke, { { 5.0f, 12.0f }, { 19.0f, 12.0f } });
    return i;
}

Icon close()
{
    Icon i;
    polyline (i.stroke, { { 6.5f, 6.5f }, { 17.5f, 17.5f } });
    polyline (i.stroke, { { 17.5f, 6.5f }, { 6.5f, 17.5f } });
    return i;
}

Icon reset()
{
    Icon i;
    const float start = 0.9f, end = 2.0f * kPi - 0.35f;
    i.stroke.addCentredArc (12.0f, 12.5f, 7.5f, 7.5f, 0.0f, start, end, true);
    // Arrow head at the end of the arc, pointing clockwise along the tangent.
    const auto tip = onCircle (12.0f, 12.5f, 7.5f, end);
    const juce::Point<float> tangent (std::cos (end), std::sin (end));
    arrowHead (i.stroke, tip + tangent * 1.2f, tangent, 3.0f);
    return i;
}

Icon external()
{
    Icon i;
    i.stroke.startNewSubPath (18.0f, 13.5f);
    i.stroke.lineTo (18.0f, 18.0f);
    i.stroke.quadraticTo (18.0f, 20.0f, 16.0f, 20.0f);
    i.stroke.lineTo (6.0f, 20.0f);
    i.stroke.quadraticTo (4.0f, 20.0f, 4.0f, 18.0f);
    i.stroke.lineTo (4.0f, 8.0f);
    i.stroke.quadraticTo (4.0f, 6.0f, 6.0f, 6.0f);
    i.stroke.lineTo (10.5f, 6.0f);
    polyline (i.stroke, { { 14.0f, 4.0f }, { 20.0f, 4.0f }, { 20.0f, 10.0f } });
    polyline (i.stroke, { { 20.0f, 4.0f }, { 11.5f, 12.5f } });
    return i;
}

Icon musicNote()
{
    Icon i;
    polyline (i.stroke, { { 10.0f, 17.5f }, { 10.0f, 5.5f }, { 19.0f, 3.5f }, { 19.0f, 15.0f } });
    polyline (i.stroke, { { 10.0f, 9.0f }, { 19.0f, 7.0f } });
    i.fill.addEllipse (4.0f, 15.0f, 6.6f, 5.2f);
    i.fill.addEllipse (13.0f, 12.6f, 6.6f, 5.2f);
    return i;
}

Icon gamepad()
{
    Icon i;
    auto& p = i.stroke;
    p.startNewSubPath (7.0f, 7.5f);
    p.lineTo (17.0f, 7.5f);
    p.cubicTo (20.6f, 7.5f, 22.2f, 11.0f, 22.2f, 14.8f);
    p.cubicTo (22.2f, 18.0f, 20.0f, 19.2f, 18.3f, 17.6f);
    p.lineTo (16.0f, 15.2f);
    p.lineTo (8.0f, 15.2f);
    p.lineTo (5.7f, 17.6f);
    p.cubicTo (4.0f, 19.2f, 1.8f, 18.0f, 1.8f, 14.8f);
    p.cubicTo (1.8f, 11.0f, 3.4f, 7.5f, 7.0f, 7.5f);
    p.closeSubPath();
    polyline (p, { { 7.0f, 9.6f }, { 7.0f, 13.4f } });
    polyline (p, { { 5.1f, 11.5f }, { 8.9f, 11.5f } });
    i.fill.addEllipse (15.0f, 9.2f, 2.3f, 2.3f);
    i.fill.addEllipse (17.4f, 11.6f, 2.3f, 2.3f);
    return i;
}

Icon speaker()
{
    Icon i;
    polyline (i.stroke, { { 4.0f, 9.0f }, { 8.0f, 9.0f }, { 13.0f, 5.0f }, { 13.0f, 19.0f }, { 8.0f, 15.0f }, { 4.0f, 15.0f }, { 4.0f, 9.0f } });
    i.stroke.addCentredArc (13.0f, 12.0f, 3.8f, 3.8f, 0.0f, 0.75f, kPi - 0.75f, true);
    i.stroke.addCentredArc (13.0f, 12.0f, 7.2f, 7.2f, 0.0f, 0.8f, kPi - 0.8f, true);
    return i;
}

Icon speakerMuted()
{
    Icon i;
    polyline (i.stroke, { { 4.0f, 9.0f }, { 8.0f, 9.0f }, { 13.0f, 5.0f }, { 13.0f, 19.0f }, { 8.0f, 15.0f }, { 4.0f, 15.0f }, { 4.0f, 9.0f } });
    polyline (i.stroke, { { 16.0f, 9.5f }, { 21.0f, 14.5f } });
    polyline (i.stroke, { { 21.0f, 9.5f }, { 16.0f, 14.5f } });
    return i;
}

Icon logo()
{
    Icon i;
    auto& p = i.stroke;
    p.startNewSubPath (3.0f, 12.0f);
    p.cubicTo (5.0f, 12.0f, 5.5f, 7.0f, 7.5f, 7.0f);
    p.cubicTo (9.8f, 7.0f, 10.0f, 18.0f, 12.3f, 18.0f);
    p.cubicTo (14.5f, 18.0f, 14.6f, 4.5f, 16.8f, 4.5f);
    p.cubicTo (18.8f, 4.5f, 19.2f, 12.0f, 21.0f, 12.0f);
    return i;
}
} // namespace Icons

void drawIcon (juce::Graphics& g, const Icon& icon, juce::Rectangle<float> area, juce::Colour colour, float strokeWidth)
{
    if (area.isEmpty())
        return;

    const auto transform = juce::RectanglePlacement (juce::RectanglePlacement::centred)
                               .getTransformToFit (juce::Rectangle<float> (0.0f, 0.0f, 24.0f, 24.0f), area);
    const float scale = juce::jmin (area.getWidth(), area.getHeight()) / 24.0f;

    g.setColour (colour);
    if (! icon.fill.isEmpty())
    {
        auto f = icon.fill;
        f.applyTransform (transform);
        g.fillPath (f);
    }
    if (! icon.stroke.isEmpty())
    {
        auto s = icon.stroke;
        s.applyTransform (transform);
        g.strokePath (s, juce::PathStrokeType (juce::jmax (1.0f, strokeWidth * scale * 1.15f), juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
    }
}

// =============================================================================
// IconButton
// =============================================================================
IconButton::IconButton (const juce::String& accessibleName, Icon i, Style s)
    : juce::Button (accessibleName), icon (std::move (i)), style (s)
{
    setTitle (accessibleName);
    setMouseClickGrabsKeyboardFocus (false);
}

void IconButton::setIcon (Icon newIcon)
{
    icon = std::move (newIcon);
    repaint();
}

void IconButton::setText (const juce::String& newText)
{
    text = newText;
    repaint();
}

void IconButton::setStyle (Style newStyle)
{
    style = newStyle;
    repaint();
}

void IconButton::paintButton (juce::Graphics& g, bool isHighlighted, bool isDown)
{
    auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    const auto accent = Theme::accent (*this);
    const bool on = getToggleState();
    const bool enabled = isEnabled();

    // ---- Chrome ----
    if (style == Style::Round)
    {
        const auto circle = bounds.withSizeKeepingCentre (juce::jmin (bounds.getWidth(), bounds.getHeight()),
                                                          juce::jmin (bounds.getWidth(), bounds.getHeight()));
        g.setColour (isDown ? Palette::panel : (isHighlighted ? Palette::panelHover : Palette::panelRaised));
        g.fillEllipse (circle);
        g.setColour (on ? accent.withAlpha (0.6f) : Palette::borderStrong);
        g.drawEllipse (circle.reduced (0.5f), 1.0f);
    }
    else if (style == Style::Framed)
    {
        g.setColour (isDown ? Palette::panel : (isHighlighted ? Palette::panelHover : Palette::panelRaised));
        g.fillRoundedRectangle (bounds, Theme::kControlRadius);
        g.setColour (on ? accent.withAlpha (0.55f) : (isHighlighted ? Palette::borderStrong.brighter (0.2f) : Palette::borderStrong));
        g.drawRoundedRectangle (bounds, Theme::kControlRadius, 1.0f);
    }
    else if (isHighlighted || isDown || (on && accentWhenOn))
    {
        g.setColour (on && accentWhenOn ? accent.withAlpha (isDown ? 0.22f : 0.14f)
                                        : (isDown ? Palette::panelHover.darker (0.2f) : Palette::panelHover));
        g.fillRoundedRectangle (bounds, Theme::kControlRadius);
    }

    if (hasKeyboardFocus (false))
    {
        g.setColour (accent.withAlpha (0.7f));
        g.drawRoundedRectangle (bounds.reduced (1.0f), Theme::kControlRadius, 1.2f);
    }

    // ---- Icon (+ text) ----
    juce::Colour colour = iconColour.value_or (isHighlighted ? Palette::text : Palette::muted);
    if (on && accentWhenOn)
        colour = accent;
    if (! enabled)
        colour = Palette::faint.withAlpha (0.7f);

    const float h = bounds.getHeight();
    if (text.isEmpty())
    {
        const float size = juce::jmin (bounds.getWidth(), h) * 0.58f;
        drawIcon (g, icon, bounds.withSizeKeepingCentre (size, size), colour);
    }
    else
    {
        auto r = bounds.reduced (h * 0.22f, 0.0f);
        const float size = h * 0.52f;
        drawIcon (g, icon, r.removeFromLeft (size).withSizeKeepingCentre (size, size), colour);
        r.removeFromLeft (h * 0.22f);
        g.setColour (enabled ? (isHighlighted ? Palette::text : Palette::text.withAlpha (0.88f)) : Palette::faint);
        g.setFont (Theme::font (juce::jmin (13.0f, h * 0.46f)));
        g.drawText (text, r, juce::Justification::centredLeft, true);
    }
}

// =============================================================================
// Styles
// =============================================================================
namespace Style
{
void segment (juce::TextButton& b, int radioGroupId, int connectedEdges)
{
    b.getProperties().set (FlubLookAndFeel::styleProperty, "segment");
    b.setRadioGroupId (radioGroupId, juce::dontSendNotification);
    b.setClickingTogglesState (true);
    b.setConnectedEdges (connectedEdges);
    b.setMouseClickGrabsKeyboardFocus (false);
}

void set (juce::Component& c, const juce::String& style)
{
    c.getProperties().set (FlubLookAndFeel::styleProperty, style);
    if (auto* b = dynamic_cast<juce::Button*> (&c))
        b->setMouseClickGrabsKeyboardFocus (false);
}

void describe (juce::Component& c, const juce::String& title, const juce::String& tooltip)
{
    c.setTitle (title);
    if (tooltip.isNotEmpty())
    {
        c.setHelpText (tooltip);
        if (auto* client = dynamic_cast<juce::SettableTooltipClient*> (&c))
            client->setTooltip (tooltip);
    }
}
} // namespace Style
} // namespace flub::app::ui
