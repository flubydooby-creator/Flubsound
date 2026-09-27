// Flubsound Pro - visual design tokens shared by every UI component.
//
// Palette   : near-black background, raised panels with 1 px borders, one
//             accent colour that follows the processing mode (electric teal
//             for Music, magenta for Gaming) and a meter palette with a
//             colour-blind safe alternative (Okabe-Ito blue / yellow /
//             vermillion instead of green / amber / red). Two themes: the
//             standard dark one and a high-contrast one (WCAG AA: text >= 4.5:1,
//             graphics >= 3:1 on every surface; tests/app/test_app_accessibility).
// Fonts     : one sans family picked once at start-up (Inter if installed,
//             else the platform's modern UI face, else JUCE's default sans).
// Drawing   : panel / section-title / pill helpers so every panel has the
//             same radius, border and caption style.
// Formatting: short numeric readouts (dB, LUFS, %).
// UI scale  : Settings > General > UI scale, applied app-wide through the
//             Desktop's global scale factor (vector drawing stays crisp and
//             mouse hit-testing follows the scaled coordinates).
//
// Message thread only (the accent lives in FlubLookAndFeel; components ask for
// it through Theme::accent (component) so a mode switch only needs a
// sendLookAndFeelChange() + repaint()).
#pragma once

#include "flub/engine/Parameters.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

namespace flub::app::ui
{
/** The UI theme (Settings > General > Theme, AppSettings::getUiTheme). */
enum class UiTheme
{
    Standard,    // the dark design language
    HighContrast // black surfaces, white text, bright borders (WCAG AA)
};

/** Every colour the UI draws with, one set per UiTheme. Components never
    hard-code a colour: they use the Palette:: names below, which refer to
    the active theme's tokens. */
struct PaletteTokens
{
    // Surfaces
    juce::Colour background, well, panel, panelRaised, panelHover, menu, tooltip, tabOn;
    // Lines
    juce::Colour border, borderStrong, track, grid, gridMinor, gridMajor, scrollThumb, scrollThumbHover;
    // Text (and the accents / status colours, which are also used for text)
    juce::Colour text, muted, faint, teal, magenta, amber, red, green, dynamicEq;
    // Knob body, level-meter ends, neutral overlays
    juce::Colour knobTop, knobBottom, knobRim, meterSafe, meterHot, highlight, shadow;
    // EQ band colours (EqCurveEditor::bandColour)
    std::array<juce::Colour, 10> eqBands;
};

namespace Palette
{
namespace detail
{
/** The active theme's tokens (Theme::setTheme). Message thread only. */
extern PaletteTokens active;
} // namespace detail

inline const juce::Colour& background = detail::active.background;
inline const juce::Colour& well = detail::active.well;        // meter wells, plot backgrounds
inline const juce::Colour& panel = detail::active.panel;
inline const juce::Colour& panelRaised = detail::active.panelRaised; // buttons, combo boxes
inline const juce::Colour& panelHover = detail::active.panelHover;
inline const juce::Colour& menu = detail::active.menu;       // popup menus
inline const juce::Colour& tooltip = detail::active.tooltip; // tooltips, EQ node readouts
inline const juce::Colour& tabOn = detail::active.tabOn;     // selected "tab" button
inline const juce::Colour& border = detail::active.border;
inline const juce::Colour& borderStrong = detail::active.borderStrong;
inline const juce::Colour& track = detail::active.track; // knob / slider track behind the accent value arc
inline const juce::Colour& grid = detail::active.grid;
inline const juce::Colour& gridMinor = detail::active.gridMinor; // analyser: minor frequency lines
inline const juce::Colour& gridMajor = detail::active.gridMajor; // analyser: decades
inline const juce::Colour& scrollThumb = detail::active.scrollThumb;
inline const juce::Colour& scrollThumbHover = detail::active.scrollThumbHover;
inline const juce::Colour& text = detail::active.text;
inline const juce::Colour& muted = detail::active.muted;
inline const juce::Colour& faint = detail::active.faint;
inline const juce::Colour& teal = detail::active.teal;
inline const juce::Colour& magenta = detail::active.magenta;
inline const juce::Colour& amber = detail::active.amber;
inline const juce::Colour& red = detail::active.red;
inline const juce::Colour& green = detail::active.green;
inline const juce::Colour& dynamicEq = detail::active.dynamicEq; // ghost markers of the dynamic EQ
inline const juce::Colour& knobTop = detail::active.knobTop;
inline const juce::Colour& knobBottom = detail::active.knobBottom;
inline const juce::Colour& knobRim = detail::active.knobRim;
inline const juce::Colour& meterSafe = detail::active.meterSafe; // standard meter palette: safe / hot ends
inline const juce::Colour& meterHot = detail::active.meterHot;
inline const juce::Colour& highlight = detail::active.highlight; // translucent sheen overlays
inline const juce::Colour& shadow = detail::active.shadow;       // translucent drop shadows
inline const std::array<juce::Colour, 10>& eqBands = detail::active.eqBands;
} // namespace Palette

enum class MeterPalette
{
    Standard,       // green -> amber -> red
    ColourBlindSafe // sky blue -> yellow -> vermillion (Okabe-Ito)
};

struct MeterColours
{
    juce::Colour safe, warn, hot;
};

namespace Theme
{
// ---- Themes ------------------------------------------------------------------
/** The tokens of a theme (the active one is Palette::). */
const PaletteTokens& tokens (UiTheme theme);
UiTheme currentTheme();

/** Makes `theme` the active palette: re-points every Palette:: colour,
    re-applies the colours of every FlubLookAndFeel (a mode accent follows
    along), re-maps colours that components on the desktop set explicitly
    from the old palette (Component::setColour) and sends them a
    look-and-feel change so cached layers are redrawn. Message thread only. */
void setTheme (UiTheme theme);

/** The token of `to` in the role that `c` has in `from` (same RGB), keeping
    c's alpha; c itself if it is not a token of `from`. */
juce::Colour remapColour (juce::Colour c, const PaletteTokens& from, const PaletteTokens& to);

/** remapColour for every colour set on `root` and its children with
    Component::setColour (what setTheme does for each window on the desktop). */
void remapComponentColours (juce::Component& root, const PaletteTokens& from, const PaletteTokens& to);

/** WCAG 2.x relative luminance (0 black .. 1 white) and contrast ratio
    (1 .. 21) of two opaque colours. */
double relativeLuminance (juce::Colour c);
double contrastRatio (juce::Colour a, juce::Colour b);

// ---- UI scale -------------------------------------------------------------------
/** The Desktop global scale factor for a UI scale setting
    (AppSettings::getUiScalePercent: 0 follows the system, i.e. only the OS's
    display scaling applies and the factor is 1; otherwise 75 .. 200 %). */
float scaleFactorForPercent (int percent);
/** Applies the setting app-wide (juce::Desktop::setGlobalScaleFactor) and
    re-fits every window registered with setMinimumWindowSize to its screen. */
void applyUiScale (int percent);

/** The minimum size of a window whose design needs `designMinimum` on a
    display whose user area is `userArea` (logical pixels, i.e. already
    divided by the UI scale, as juce::Displays reports it): the design
    minimum, but never more than the screen, so a large UI scale on a small
    screen still leaves a window that fits. */
juce::Point<int> minimumWindowSize (juce::Point<int> designMinimum, juce::Rectangle<int> userArea);

/** Gives `window` the minimum size above for the display it is on (its
    constrainer's minimum), shrinks it to fit that display if it is larger and
    remembers the design minimum so applyUiScale re-fits it. */
void setMinimumWindowSize (juce::ResizableWindow& window, juce::Point<int> designMinimum);

/** Accent for a processing mode. */
juce::Colour accentForMode (flub::param::ModeValue mode);

/** The current accent of the component's look-and-feel (teal if the
    look-and-feel is not a FlubLookAndFeel). */
juce::Colour accent (const juce::Component& c);

/** Meter palette of the component's look-and-feel (Standard if it is not a
    FlubLookAndFeel). */
MeterPalette meterPalette (const juce::Component& c);

/** Meter colours of the component's look-and-feel. */
MeterColours meterColours (const juce::Component& c);

/** Good / caution / alert colours for indicators that encode a state by
    colour outside the level bars (correlation, gain reduction, clipper,
    over-ceiling readouts, muted strips). Standard keeps the UI's green /
    amber / red; the colour-blind safe palette uses its meter colours. */
MeterColours statusColours (MeterPalette palette);
inline MeterColours statusColours (const juce::Component& c) { return statusColours (meterPalette (c)); }

/** Colour of a level in dBFS on a meter (safe / warn / hot zones). */
juce::Colour meterColourForDb (const MeterColours& colours, float db);

// ---- Fonts -------------------------------------------------------------------
/** The UI font family (chosen once). */
const juce::String& fontFamily();
juce::Font font (float height, bool bold = false);
/** Upper-case captions: bold, slightly tracked. */
juce::Font caption (float height = 11.0f);
/** Numeric readouts. */
juce::Font numeric (float height, bool bold = true);

// ---- Drawing -------------------------------------------------------------------
constexpr float kPanelRadius = 10.0f;
constexpr float kControlRadius = 6.0f;

void drawPanel (juce::Graphics& g, juce::Rectangle<float> bounds, float radius = kPanelRadius);
/** The fill drawPanel uses for a panel with these bounds (lets opaque children match it). */
juce::ColourGradient panelFill (juce::Rectangle<float> panelBounds);
/** Caption in the panel header style, e.g. "LOUDNESS". Returns its width. */
float drawCaption (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
                   juce::Colour colour = Palette::muted, juce::Justification just = juce::Justification::centredLeft);
/** Small rounded status pill ("7.1", "MACRO", ...). */
void drawPill (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& text, juce::Colour colour, bool filled = false);
/** A subtle vertical sheen used on key controls. */
juce::ColourGradient sheen (juce::Rectangle<float> area, juce::Colour base, float amount = 0.06f);

// ---- Formatting --------------------------------------------------------------------
/** "-12.3" / "-inf" for values below floorDb. */
juce::String formatDb (float db, int decimals = 1, float floorDb = -99.0f);
juce::String formatSignedDb (float db, int decimals = 1);
juce::String formatLufs (float lufs);
} // namespace Theme
} // namespace flub::app::ui
