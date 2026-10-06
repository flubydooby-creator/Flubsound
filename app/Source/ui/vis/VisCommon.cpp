#include "VisCommon.h"

#include "../Theme.h"

#include <algorithm>
#include <cmath>

namespace flub::app::ui::vis
{
// =============================================================================
// Drawing
// =============================================================================
void drawWell (juce::Graphics& g, juce::Rectangle<float> area)
{
    g.setColour (Palette::well);
    g.fillRoundedRectangle (area, kWellRadius);
}

juce::Font labelFont()
{
    return Theme::font (kLabelFontHeight);
}

juce::Colour labelColour()
{
    return Palette::faint.brighter (0.25f);
}

float drawLegend (juce::Graphics& g, juce::Rectangle<float> area, std::initializer_list<LegendItem> items)
{
    const auto font = Theme::font (11.0f);
    g.setFont (font);
    const float start = area.getX();
    for (const auto& item : items)
    {
        const float textWidth = juce::GlyphArrangement::getStringWidth (font, item.text);
        if (14.0f + textWidth + 12.0f > area.getWidth())
            break;
        auto swatch = area.removeFromLeft (14.0f).withSizeKeepingCentre (12.0f, 3.0f);
        g.setColour (item.colour);
        if (item.dashed)
        {
            g.fillRect (swatch.withWidth (4.0f));
            g.fillRect (swatch.withTrimmedLeft (8.0f));
        }
        else
        {
            g.fillRoundedRectangle (swatch, 1.5f);
        }
        area.removeFromLeft (4.0f);
        g.setColour (Palette::muted);
        g.drawText (item.text, area.removeFromLeft (textWidth + 2.0f), juce::Justification::centredLeft, false);
        area.removeFromLeft (10.0f);
    }
    return area.getX() - start;
}

juce::Colour frequencyColour (double hz)
{
    const double t = juce::jlimit (0.0, 1.0, std::log (juce::jmax (hz, 1.0) / 20.0) / std::log (1000.0));
    // Red at the bottom of the range through amber, green, cyan and blue to violet.
    return juce::Colour::fromHSV (static_cast<float> (0.78 * t), 0.72f, 0.98f, 1.0f);
}

juce::String formatValue (float value, float floorValue, int decimals)
{
    if (! std::isfinite (value) || value < floorValue)
        return "-inf";
    return juce::String (value, decimals);
}

// =============================================================================
// SlotHistory
// =============================================================================
SlotHistory::SlotHistory (int slots, double seconds, std::initializer_list<Combine> how, float empty)
    : numSlots (juce::jmax (2, slots)), numValues (juce::jmax (1, static_cast<int> (how.size()))),
      slotSeconds (juce::jmax (1.0e-3, seconds)), emptyValue (empty), combine (how)
{
    values.assign (static_cast<size_t> (numSlots * numValues), emptyValue);
    running.assign (static_cast<size_t> (numValues), emptyValue);
}

void SlotHistory::clear() noexcept
{
    std::fill (values.begin(), values.end(), emptyValue);
    std::fill (running.begin(), running.end(), emptyValue);
    newest = -1;
    filled = 0;
    elapsed = 0.0;
    runningEmpty = true;
    ++version;
}

void SlotHistory::startSlot() noexcept
{
    newest = (newest + 1) % numSlots;
    std::copy (running.begin(), running.end(), values.begin() + newest * numValues);
    filled = juce::jmin (numSlots, filled + 1);
    ++version;
}

void SlotHistory::add (const float* v, double dtSeconds) noexcept
{
    for (int k = 0; k < numValues; ++k)
    {
        auto& r = running[static_cast<size_t> (k)];
        if (runningEmpty)
        {
            r = v[k];
            continue;
        }
        switch (combine[static_cast<size_t> (k)])
        {
            case Combine::Max: r = juce::jmax (r, v[k]); break;
            case Combine::Min: r = juce::jmin (r, v[k]); break;
            case Combine::Last: r = v[k]; break;
        }
    }
    runningEmpty = false;
    elapsed += juce::jmax (0.0, dtSeconds);
    // A long frame completes several slots: each later one holds this frame's reading.
    for (int guard = 0; elapsed >= slotSeconds && guard < numSlots; ++guard)
    {
        startSlot();
        elapsed -= slotSeconds;
        std::copy (v, v + numValues, running.begin());
    }
    if (elapsed >= slotSeconds)
        elapsed = std::fmod (elapsed, slotSeconds);
}

float SlotHistory::get (int age, int k) const noexcept
{
    if (age < 0 || age >= filled || k < 0 || k >= numValues)
        return emptyValue;
    const int slot = ((newest - age) % numSlots + numSlots) % numSlots;
    return values[static_cast<size_t> (slot * numValues + k)];
}

// =============================================================================
// Phosphor
// =============================================================================
Phosphor::Phosphor (int w, int h)
    : width (juce::jmax (2, w)), height (juce::jmax (2, h)),
      // Software image: written through BitmapData every frame.
      image (juce::Image::ARGB, width, height, true, juce::SoftwareImageType())
{
    cells.assign (static_cast<size_t> (width * height), 0.0f);
    setColour (juce::Colours::cyan, juce::Colours::white);
}

void Phosphor::setColour (juce::Colour colour, juce::Colour hot)
{
    for (size_t i = 0; i < lut.size(); ++i)
    {
        const float t = static_cast<float> (i) / static_cast<float> (lut.size() - 1);
        juce::Colour c;
        if (t < 0.7f)
            c = colour.withAlpha (std::pow (t / 0.7f, 0.75f));
        else
            c = colour.interpolatedWith (hot, (t - 0.7f) / 0.3f * 0.85f);
        lut[i] = c.getPixelARGB(); // premultiplied
    }
}

void Phosphor::fade (float factor) noexcept
{
    const float f = juce::jlimit (0.0f, 1.0f, factor);
    for (auto& c : cells)
        c = c * f < 1.0e-4f ? 0.0f : c * f;
}

void Phosphor::splat (float x, float y, float amount) noexcept
{
    const float fx = std::floor (x), fy = std::floor (y);
    const int x0 = static_cast<int> (fx), y0 = static_cast<int> (fy);
    if (x0 < 0 || y0 < 0 || x0 + 1 >= width || y0 + 1 >= height)
        return;
    const float ax = x - fx, ay = y - fy;
    float* row0 = cells.data() + y0 * width + x0;
    float* row1 = row0 + width;
    row0[0] += amount * (1.0f - ax) * (1.0f - ay);
    row0[1] += amount * ax * (1.0f - ay);
    row1[0] += amount * (1.0f - ax) * ay;
    row1[1] += amount * ax * ay;
}

void Phosphor::clear() noexcept
{
    std::fill (cells.begin(), cells.end(), 0.0f);
}

void Phosphor::render() noexcept
{
    juce::Image::BitmapData data (image, juce::Image::BitmapData::writeOnly);
    constexpr float kKnee = 0.6f; // soft saturation: t = I / (I + knee) * (1 + knee)
    for (int y = 0; y < height; ++y)
    {
        const float* src = cells.data() + y * width;
        for (int x = 0; x < width; ++x)
        {
            const float v = src[x];
            const float t = v <= 0.0f ? 0.0f : juce::jmin (1.0f, v / (v + kKnee) * (1.0f + kKnee));
            *reinterpret_cast<juce::PixelARGB*> (data.getPixelPointer (x, y)) = lut[static_cast<size_t> (t * 255.0f)];
        }
    }
}
} // namespace flub::app::ui::vis
