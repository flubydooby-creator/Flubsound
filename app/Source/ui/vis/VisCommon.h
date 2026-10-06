// Flubsound Pro - building blocks shared by the visualiser views.
//
//   drawWell / drawLegend / axis label helpers: the analyser's plot style
//     (Palette::well rounded plot, faint 10.5 px labels, legend swatches).
//   frequencyColour: one colour per frequency (low warm -> high cool), the
//     same across views.
//   SlotHistory: a fixed ring of time slots (e.g. 600 x 0.1 s) of K values
//     each, filled from per-frame readings (max / min / last per slot); the
//     history views' storage. No allocation after construction.
//   Phosphor: a fixed-size intensity grid that fades every frame and is
//     splatted into (bilinear), shown through a colour table as an image:
//     the goniometer's afterglow, usable by any "scope" style view.
// Message thread only.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace flub::app::ui::vis
{
// ---- Drawing -----------------------------------------------------------------------
constexpr float kWellRadius = 6.0f;
constexpr float kLabelFontHeight = 10.5f;

/** The plot well (Palette::well, rounded like the spectrum's). */
void drawWell (juce::Graphics& g, juce::Rectangle<float> area);
/** Faint axis label font / colour. */
juce::Font labelFont();
juce::Colour labelColour();

struct LegendItem
{
    juce::Colour colour;
    juce::String text;
    bool dashed = false;
};
/** A one-line legend (swatch + text per item) from the left of `area`; returns
    the width used. Items that do not fit are left out. */
float drawLegend (juce::Graphics& g, juce::Rectangle<float> area, std::initializer_list<LegendItem> items);

/** One colour per frequency, 20 Hz (red / warm) .. 20 kHz (violet / cool), at
    a saturation that reads on the dark well. */
juce::Colour frequencyColour (double hz);

/** "-12.3" style number (one decimal), "-inf" below floorDb. */
juce::String formatValue (float value, float floorValue = -99.0f, int decimals = 1);

// ---- SlotHistory ----------------------------------------------------------------------
class SlotHistory
{
public:
    enum class Combine
    {
        Max,  // the largest reading in the slot (peaks, loudness)
        Min,  // the smallest (gain reductions, which are <= 0)
        Last  // the reading at the end of the slot
    };

    /** numSlots slots of slotSeconds, `combine.size()` values each; empty
        slots read `emptyValue`. */
    SlotHistory (int numSlots, double slotSeconds, std::initializer_list<Combine> combine, float emptyValue);

    /** One frame's readings (one per value) over dtSeconds. */
    void add (const float* values, double dtSeconds) noexcept;
    void clear() noexcept;

    int getNumSlots() const noexcept { return numSlots; }
    int getNumValues() const noexcept { return numValues; }
    double getSlotSeconds() const noexcept { return slotSeconds; }
    /** Completed slots so far (saturates at getNumSlots()). */
    int getFilled() const noexcept { return filled; }
    /** Value `k` of the completed slot `age` slots ago (0 = newest); emptyValue if none. */
    float get (int age, int k) const noexcept;
    /** The running (incomplete) slot's value k so far. */
    float getRunning (int k) const noexcept { return running[static_cast<size_t> (k)]; }
    /** How far the running slot has got (0..1): for smooth scrolling. */
    float getSlotFraction() const noexcept { return static_cast<float> (elapsed / slotSeconds); }
    /** Bumped by every completed slot (views rebuild when it changes). */
    uint64_t getVersion() const noexcept { return version; }

private:
    void startSlot() noexcept;

    int numSlots, numValues;
    double slotSeconds;
    float emptyValue;
    std::vector<Combine> combine;
    std::vector<float> values, running; // values: numSlots x numValues ring
    int newest = -1, filled = 0;
    double elapsed = 0.0;
    bool runningEmpty = true;
    uint64_t version = 0;
};

// ---- Phosphor ------------------------------------------------------------------------
class Phosphor
{
public:
    Phosphor (int width, int height);

    /** Colour table: 0 = transparent, then a dim tint, the colour, and hot (near white) at full. */
    void setColour (juce::Colour colour, juce::Colour hot);
    /** Multiplies every cell by `factor` (0..1): the afterglow. */
    void fade (float factor) noexcept;
    /** Adds `amount` at (x, y) in cell coordinates, spread bilinearly over four cells. */
    void splat (float x, float y, float amount) noexcept;
    void clear() noexcept;
    /** Writes the intensities (clipped at 1) through the colour table into the image. */
    void render() noexcept;

    int getWidth() const noexcept { return width; }
    int getHeight() const noexcept { return height; }
    float getCell (int x, int y) const noexcept { return cells[static_cast<size_t> (y * width + x)]; }
    const juce::Image& getImage() const noexcept { return image; }

private:
    int width, height;
    std::vector<float> cells;
    juce::Image image;
    std::array<juce::PixelARGB, 256> lut {};
};
} // namespace flub::app::ui::vis
