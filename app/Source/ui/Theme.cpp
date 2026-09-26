#include "Theme.h"

#include "FlubLookAndFeel.h"

#include <cmath>

namespace flub::app::ui::Theme
{
juce::Colour accentForMode (flub::param::ModeValue mode)
{
    return mode == flub::param::ModeValue::Gaming ? Palette::magenta : Palette::teal;
}

juce::Colour accent (const juce::Component& c)
{
    if (auto* lnf = dynamic_cast<const FlubLookAndFeel*> (&c.getLookAndFeel()))
        return lnf->getAccent();
    return Palette::teal;
}

MeterPalette meterPalette (const juce::Component& c)
{
    if (auto* lnf = dynamic_cast<const FlubLookAndFeel*> (&c.getLookAndFeel()))
        return lnf->getMeterPalette();
    return MeterPalette::Standard;
}

namespace
{
const MeterColours colourBlindSafe { juce::Colour (0xff56b4e9), juce::Colour (0xfff0e442), juce::Colour (0xffd55e00) }; // Okabe-Ito
} // namespace

MeterColours meterColours (const juce::Component& c)
{
    if (meterPalette (c) == MeterPalette::ColourBlindSafe)
        return colourBlindSafe;
    return { juce::Colour (0xff34d399), Palette::amber, juce::Colour (0xffef4444) };
}

MeterColours statusColours (MeterPalette palette)
{
    if (palette == MeterPalette::ColourBlindSafe)
        return colourBlindSafe;
    return { Palette::green, Palette::amber, Palette::red };
}

juce::Colour meterColourForDb (const MeterColours& colours, float db)
{
    if (db >= -3.0f)
        return colours.hot;
    if (db >= -12.0f)
        return colours.warn;
    return colours.safe;
}

// =============================================================================
const juce::String& fontFamily()
{
    static const juce::String family = []
    {
        // Modern UI faces in order of preference; the first installed one wins.
        static const char* const preferred[] = { "Inter",          "Inter Variable", "Segoe UI Variable Text", "Segoe UI",
                                                 "SF Pro Text",    "Helvetica Neue", "Roboto",                 "Noto Sans",
                                                 "Open Sans",      "Cantarell",      "Liberation Sans" };
        const auto installed = juce::Font::findAllTypefaceNames();
        for (const auto* name : preferred)
            if (installed.contains (name, true))
                return juce::String (name);
        return juce::Font::getDefaultSansSerifFontName();
    }();
    return family;
}

juce::Font font (float height, bool bold)
{
    return juce::Font (juce::FontOptions (fontFamily(), height, bold ? juce::Font::bold : juce::Font::plain));
}

juce::Font caption (float height)
{
    return juce::Font (juce::FontOptions (fontFamily(), height, juce::Font::bold).withKerningFactor (0.08f));
}

juce::Font numeric (float height, bool bold)
{
    return juce::Font (juce::FontOptions (fontFamily(), height, bold ? juce::Font::bold : juce::Font::plain).withKerningFactor (0.01f));
}

// =============================================================================
juce::ColourGradient panelFill (juce::Rectangle<float> bounds)
{
    // Slightly lighter at the top so panels read as raised surfaces.
    return juce::ColourGradient (Palette::panel.brighter (0.035f), 0.0f, bounds.getY(), Palette::panel, 0.0f,
                                 bounds.getY() + juce::jmin (160.0f, bounds.getHeight()), false);
}

void drawPanel (juce::Graphics& g, juce::Rectangle<float> bounds, float radius)
{
    g.setGradientFill (panelFill (bounds));
    g.fillRoundedRectangle (bounds, radius);
    g.setColour (Palette::border);
    g.drawRoundedRectangle (bounds.reduced (0.5f), radius, 1.0f);
}

float drawCaption (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, juce::Colour colour,
                   juce::Justification just)
{
    const auto f = caption (11.0f);
    g.setFont (f);
    g.setColour (colour);
    g.drawText (text, area, just, false);
    return juce::GlyphArrangement::getStringWidth (f, text);
}

void drawPill (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& text, juce::Colour colour, bool filled)
{
    const float r = area.getHeight() * 0.5f;
    if (filled)
    {
        g.setColour (colour.withAlpha (0.18f));
        g.fillRoundedRectangle (area, r);
    }
    g.setColour (colour.withAlpha (filled ? 0.55f : 0.45f));
    g.drawRoundedRectangle (area.reduced (0.5f), r, 1.0f);
    g.setColour (colour);
    g.setFont (caption (juce::jmin (10.0f, area.getHeight() - 5.0f)));
    g.drawText (text, area, juce::Justification::centred, false);
}

juce::ColourGradient sheen (juce::Rectangle<float> area, juce::Colour base, float amount)
{
    return juce::ColourGradient (base.brighter (amount), 0.0f, area.getY(), base.darker (amount * 0.5f), 0.0f, area.getBottom(), false);
}

// =============================================================================
juce::String formatDb (float db, int decimals, float floorDb)
{
    if (! std::isfinite (db) || db <= floorDb)
        return "-inf";
    return juce::String (db, decimals);
}

juce::String formatSignedDb (float db, int decimals)
{
    if (! std::isfinite (db))
        return "0.0";
    const auto rounded = juce::String (std::abs (db), decimals);
    if (rounded.getDoubleValue() == 0.0)
        return rounded;
    return (db > 0.0f ? "+" : "-") + rounded;
}

juce::String formatLufs (float lufs)
{
    return lufs <= -70.0f || ! std::isfinite (lufs) ? juce::String ("--.-") : juce::String (lufs, 1);
}
} // namespace flub::app::ui::Theme
