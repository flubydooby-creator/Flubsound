#include "Theme.h"

#include "FlubLookAndFeel.h"

#include "settings/AppSettings.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace flub::app::ui
{
namespace
{
juce::Colour argb (juce::uint32 value) { return juce::Colour (value); }

PaletteTokens makeStandard()
{
    PaletteTokens t;
    t.background = argb (0xff0e1014);
    t.well = argb (0xff0a0c10);
    t.panel = argb (0xff161a21);
    t.panelRaised = argb (0xff1c212a);
    t.panelHover = argb (0xff222834);
    t.menu = argb (0xff171b22);
    t.tooltip = argb (0xff1f2530);
    t.tabOn = argb (0xff262d3a);
    t.border = argb (0xff232a35);
    t.borderStrong = argb (0xff2f3847);
    t.track = argb (0xff2f3847);
    t.grid = argb (0xff1d232d);
    t.gridMinor = argb (0xff141920);
    t.gridMajor = argb (0xff232b37);
    t.scrollThumb = argb (0xff343c4a);
    t.scrollThumbHover = argb (0xff4a5466);
    t.text = argb (0xffe6e9ef);
    t.muted = argb (0xff8a93a3);
    // Was 0xff5a6373 (2.9:1 on a panel): raised to meet WCAG AA (4.5:1) on
    // every surface that carries faint text (captions, scale labels, help).
    t.faint = argb (0xff7f899b);
    t.teal = argb (0xff22d3ee);
    t.magenta = argb (0xffe879f9);
    t.amber = argb (0xfffbbf24);
    t.red = argb (0xfff87171);
    t.green = argb (0xff34d399);
    t.dynamicEq = argb (0xfffbbf24);
    t.knobTop = argb (0xff2a313d);
    t.knobBottom = argb (0xff171b22);
    t.knobRim = argb (0xff343d4c);
    t.meterSafe = argb (0xff34d399);
    t.meterHot = argb (0xffef4444);
    t.highlight = argb (0xffffffff);
    t.shadow = argb (0xff000000);
    t.eqBands = { argb (0xfff87171), argb (0xfffb923c), argb (0xfffbbf24), argb (0xffa3e635), argb (0xff34d399),
                  argb (0xff22d3ee), argb (0xff60a5fa), argb (0xff818cf8), argb (0xffc084fc), argb (0xfff472b6) };
    return t;
}

/** Black surfaces, white text, bright borders; every text token >= 4.5:1 and
    every line / meter / band colour >= 3:1 on every surface (WCAG AA). */
PaletteTokens makeHighContrast()
{
    auto t = makeStandard(); // the EQ band colours are bright enough on black
    t.background = argb (0xff000000);
    t.well = argb (0xff000000);
    t.panel = argb (0xff0c0d10);
    t.panelRaised = argb (0xff16181c);
    t.panelHover = argb (0xff262a31);
    t.menu = argb (0xff0c0d10);
    t.tooltip = argb (0xff16181c);
    t.tabOn = argb (0xff2a2f38);
    t.border = argb (0xff9aa3b2);
    t.borderStrong = argb (0xffc8ced8);
    // Mid grey: visible on black (>= 3:1) and still >= 3:1 against the
    // accent value arc drawn over it, so a knob's value reads without hue.
    t.track = argb (0xff606878);
    t.grid = argb (0xff3a404a);
    t.gridMinor = argb (0xff262b33);
    t.gridMajor = argb (0xff4a5260);
    t.scrollThumb = argb (0xff9aa3b2);
    t.scrollThumbHover = argb (0xffd0d6e0);
    t.text = argb (0xffffffff);
    t.muted = argb (0xffe2e6ee);
    t.faint = argb (0xffc3c9d4);
    t.teal = argb (0xff3de8ff);
    t.magenta = argb (0xffffa0ff);
    t.amber = argb (0xffffd23f);
    t.red = argb (0xffff8080);
    t.green = argb (0xff4cf5a8);
    t.dynamicEq = t.amber;
    t.knobTop = argb (0xff2a2f38);
    t.knobBottom = argb (0xff16181c);
    t.knobRim = argb (0xffc8ced8);
    t.meterSafe = t.green;
    t.meterHot = argb (0xffff5a5a);
    return t;
}

UiTheme activeTheme = UiTheme::Standard;
} // namespace

PaletteTokens Palette::detail::active = makeStandard();
} // namespace flub::app::ui

namespace flub::app::ui::Theme
{
const PaletteTokens& tokens (UiTheme theme)
{
    static const PaletteTokens standard = makeStandard(), highContrast = makeHighContrast();
    return theme == UiTheme::HighContrast ? highContrast : standard;
}

UiTheme currentTheme() { return activeTheme; }

namespace
{
/** Every colour token of a palette, in a fixed order (remapColour pairs them up). */
std::vector<juce::Colour> allTokens (const PaletteTokens& t)
{
    std::vector<juce::Colour> v { t.background, t.well, t.panel, t.panelRaised, t.panelHover, t.menu, t.tooltip, t.tabOn,
                                  t.border, t.borderStrong, t.track, t.grid, t.gridMinor, t.gridMajor, t.scrollThumb, t.scrollThumbHover,
                                  t.text, t.muted, t.faint, t.teal, t.magenta, t.amber, t.red, t.green, t.dynamicEq,
                                  t.knobTop, t.knobBottom, t.knobRim, t.meterSafe, t.meterHot, t.highlight, t.shadow };
    v.insert (v.end(), t.eqBands.begin(), t.eqBands.end());
    return v;
}
} // namespace

juce::Colour remapColour (juce::Colour c, const PaletteTokens& from, const PaletteTokens& to)
{
    const auto a = allTokens (from), b = allTokens (to);
    const auto rgb = c.getARGB() & 0x00ffffffu;
    for (size_t i = 0; i < a.size(); ++i)
        if ((a[i].getARGB() & 0x00ffffffu) == rgb)
            return b[i].withAlpha (c.getAlpha());
    return c;
}

void remapComponentColours (juce::Component& c, const PaletteTokens& from, const PaletteTokens& to)
{
    // Component::setColour stores each override as a "jcclr_<id>" property.
    const auto& props = c.getProperties();
    for (int i = 0; i < props.size(); ++i)
    {
        const auto name = props.getName (i).toString();
        if (! name.startsWith ("jcclr_"))
            continue;
        const auto id = name.substring (6).getHexValue32();
        const auto old = c.findColour (id);
        if (const auto mapped = remapColour (old, from, to); mapped != old)
            c.setColour (id, mapped);
    }
    for (auto* child : c.getChildren())
        remapComponentColours (*child, from, to);
}

void setTheme (UiTheme theme)
{
    if (theme == activeTheme)
        return;
    const auto previous = Palette::detail::active;
    const auto& to = tokens (theme);
    Palette::detail::active = to;
    activeTheme = theme;

    FlubLookAndFeel::forEachInstance ([&] (FlubLookAndFeel& lnf) { lnf.applyPalette (previous); });

    auto& desktop = juce::Desktop::getInstance();
    for (int i = 0; i < desktop.getNumComponents(); ++i)
        if (auto* window = desktop.getComponent (i))
        {
            remapComponentColours (*window, previous, to);
            window->sendLookAndFeelChange(); // repaints; cached layers (analyser grid, EQ) are re-rendered
        }
}

// =============================================================================
double relativeLuminance (juce::Colour c)
{
    const auto channel = [] (juce::uint8 v)
    {
        const double s = v / 255.0;
        return s <= 0.03928 ? s / 12.92 : std::pow ((s + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel (c.getRed()) + 0.7152 * channel (c.getGreen()) + 0.0722 * channel (c.getBlue());
}

double contrastRatio (juce::Colour a, juce::Colour b)
{
    const double la = relativeLuminance (a), lb = relativeLuminance (b);
    return (std::max (la, lb) + 0.05) / (std::min (la, lb) + 0.05);
}

// =============================================================================
float scaleFactorForPercent (int percent)
{
    const int clamped = AppSettings::clampUiScalePercent (percent);
    return clamped == AppSettings::kUiScaleFollowSystem ? 1.0f : static_cast<float> (clamped) / 100.0f;
}

namespace
{
const juce::Identifier minimumSizeProperty { "flubMinimumSize" };

void fitToScreen (juce::ResizableWindow& window, juce::Point<int> designMinimum)
{
    const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (window.getScreenBounds());
    if (display == nullptr)
        return; // headless
    const auto area = display->userBounds.getLargestIntegerWithin();
    const auto minimum = minimumWindowSize (designMinimum, area);
    if (auto* constrainer = window.getConstrainer())
        constrainer->setMinimumSize (minimum.x, minimum.y);

    auto bounds = window.getBounds();
    bounds.setSize (juce::jlimit (minimum.x, juce::jmax (minimum.x, area.getWidth()), bounds.getWidth()),
                    juce::jlimit (minimum.y, juce::jmax (minimum.y, area.getHeight()), bounds.getHeight()));
    window.setBoundsConstrained (bounds.constrainedWithin (area));
}
} // namespace

void applyUiScale (int percent)
{
    auto& desktop = juce::Desktop::getInstance();
    desktop.setGlobalScaleFactor (scaleFactorForPercent (percent));

    // The windows keep their size in logical pixels, so they grow on screen:
    // re-fit the ones with a minimum size to their display.
    for (int i = 0; i < desktop.getNumComponents(); ++i)
        if (auto* window = dynamic_cast<juce::ResizableWindow*> (desktop.getComponent (i)))
            if (const auto* stored = window->getProperties().getVarPointer (minimumSizeProperty))
                fitToScreen (*window, juce::Point<int> (static_cast<int> ((*stored)[0]), static_cast<int> ((*stored)[1])));
}

juce::Point<int> minimumWindowSize (juce::Point<int> designMinimum, juce::Rectangle<int> userArea)
{
    return { juce::jmax (1, juce::jmin (designMinimum.x, userArea.getWidth())), juce::jmax (1, juce::jmin (designMinimum.y, userArea.getHeight())) };
}

void setMinimumWindowSize (juce::ResizableWindow& window, juce::Point<int> designMinimum)
{
    window.getProperties().set (minimumSizeProperty, juce::Array<juce::var> { designMinimum.x, designMinimum.y });
    fitToScreen (window, designMinimum);
}

// =============================================================================
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
    return { Palette::meterSafe, Palette::amber, Palette::meterHot };
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
