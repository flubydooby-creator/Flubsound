// Flubsound Pro - small shared widgets: vector icons, icon buttons and
// helpers that style stock JUCE buttons for the Flubsound look-and-feel.
//
// Icons are resolution independent paths in a 24 x 24 design grid (stroked
// with round caps, plus an optional filled part) so they stay crisp at any
// scale factor, from 1100 x 700 up to 2560 x 1440 on HiDPI screens.
#pragma once

#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

namespace flub::app::ui
{
struct Icon
{
    juce::Path stroke; // stroked with round joints / caps
    juce::Path fill;   // filled
};

namespace Icons
{
Icon gear();
Icon chevronLeft();
Icon chevronRight();
Icon chevronDown();
Icon more();       // three dots
Icon copy();       // two overlapping sheets
Icon power();
Icon expand();     // diagonal arrows out
Icon collapse();   // diagonal arrows in
Icon ear();        // "listen" (headphones)
Icon plus();
Icon close();
Icon reset();      // circular arrow
Icon external();   // box with arrow (open system settings)
Icon musicNote();
Icon gamepad();
Icon speaker();
Icon speakerMuted();
Icon logo();       // Flubsound mark (stylised wave in a rounded square)
} // namespace Icons

/** Draws an icon (24 x 24 design grid) fitted into `area`. */
void drawIcon (juce::Graphics& g, const Icon& icon, juce::Rectangle<float> area, juce::Colour colour, float strokeWidth = 1.7f);

/** A compact button that shows an icon (and an optional text to the right). */
class IconButton : public juce::Button
{
public:
    enum class Style
    {
        Ghost,   // no chrome until hovered
        Framed,  // panelRaised fill + border (like a TextButton)
        Round    // circular (transport-like)
    };

    IconButton (const juce::String& accessibleName, Icon icon, Style style = Style::Ghost);

    void setIcon (Icon newIcon);
    void setText (const juce::String& newText);
    void setStyle (Style newStyle);
    /** Use the accent colour for the icon while the toggle state is on. */
    void setAccentWhenOn (bool shouldUseAccent) { accentWhenOn = shouldUseAccent; repaint(); }
    void setIconColour (std::optional<juce::Colour> colour) { iconColour = colour; repaint(); }

    void paintButton (juce::Graphics& g, bool isHighlighted, bool isDown) override;

private:
    Icon icon;
    juce::String text;
    Style style;
    bool accentWhenOn = true;
    std::optional<juce::Colour> iconColour;
};

namespace Style
{
/** Named FlubLookAndFeel styles ("tab", "chip", "warning", "power", "switch"). */
void set (juce::Component& c, const juce::String& style);
/** Gives a component an accessible title and a tooltip in one call. */
void describe (juce::Component& c, const juce::String& title, const juce::String& tooltip = {});
} // namespace Style
} // namespace flub::app::ui
