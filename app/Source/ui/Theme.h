// Flubsound Pro - visual design tokens shared by every UI component.
//
// Palette   : near-black background, raised panels with 1 px borders, one
//             accent colour that follows the processing mode (electric teal
//             for Music, magenta for Gaming) and a meter palette with a
//             colour-blind safe alternative (Okabe-Ito blue / yellow /
//             vermillion instead of green / amber / red).
// Fonts     : one sans family picked once at start-up (Inter if installed,
//             else the platform's modern UI face, else JUCE's default sans).
// Drawing   : panel / section-title / pill helpers so every panel has the
//             same radius, border and caption style.
// Formatting: short numeric readouts (dB, LUFS, %).
//
// Message thread only (the accent lives in FlubLookAndFeel; components ask for
// it through Theme::accent (component) so a mode switch only needs a
// sendLookAndFeelChange() + repaint()).
#pragma once

#include "flub/engine/Parameters.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace flub::app::ui
{
namespace Palette
{
inline const juce::Colour background { 0xff0e1014 };
inline const juce::Colour well { 0xff0a0c10 };        // meter wells, plot backgrounds
inline const juce::Colour panel { 0xff161a21 };
inline const juce::Colour panelRaised { 0xff1c212a }; // buttons, combo boxes
inline const juce::Colour panelHover { 0xff222834 };
inline const juce::Colour border { 0xff232a35 };
inline const juce::Colour borderStrong { 0xff2f3847 };
inline const juce::Colour grid { 0xff1d232d };
inline const juce::Colour text { 0xffe6e9ef };
inline const juce::Colour muted { 0xff8a93a3 };
inline const juce::Colour faint { 0xff5a6373 };
inline const juce::Colour teal { 0xff22d3ee };
inline const juce::Colour magenta { 0xffe879f9 };
inline const juce::Colour amber { 0xfffbbf24 };
inline const juce::Colour red { 0xfff87171 };
inline const juce::Colour green { 0xff34d399 };
inline const juce::Colour dynamicEq { 0xfffbbf24 };   // ghost markers of the dynamic EQ
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
/** Accent for a processing mode. */
juce::Colour accentForMode (flub::param::ModeValue mode);

/** The current accent of the component's look-and-feel (teal if the
    look-and-feel is not a FlubLookAndFeel). */
juce::Colour accent (const juce::Component& c);

/** Meter colours of the component's look-and-feel. */
MeterColours meterColours (const juce::Component& c);

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
