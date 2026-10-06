#include "Spectrogram.h"

namespace flub::app::ui
{
Spectrogram::Spectrogram (int c, int r)
    : columns (juce::jmax (1, c)), rows (juce::jmax (2, r)),
      // A software image: rows are written through BitmapData every hop,
      // which must not map / copy a GPU image.
      image (juce::Image::ARGB, columns, rows, true, juce::SoftwareImageType())
{
    setColours (juce::Colours::black, juce::Colours::cyan, juce::Colours::white);
}

void Spectrogram::setColours (juce::Colour background, juce::Colour accent, juce::Colour hot)
{
    // floor -> a dark tint of the accent -> accent -> hot (text colour) at the top.
    const juce::Colour stops[] = { background, background.interpolatedWith (accent, 0.07f), background.interpolatedWith (accent, 0.32f),
                                   accent, accent.interpolatedWith (hot, 0.8f) };
    const float at[] = { 0.0f, 0.45f, 0.7f, 0.9f, 1.0f };
    for (size_t i = 0; i < lut.size(); ++i)
    {
        const float t = static_cast<float> (i) / static_cast<float> (lut.size() - 1);
        size_t s = 0;
        while (s + 2 < 5 && t > at[s + 1])
            ++s;
        const float local = juce::jlimit (0.0f, 1.0f, (t - at[s]) / (at[s + 1] - at[s]));
        lut[i] = stops[s].interpolatedWith (stops[s + 1], local).withAlpha (1.0f);
    }
}

juce::Colour Spectrogram::colourFor (float t) const noexcept
{
    const auto i = static_cast<size_t> (juce::jlimit (0, static_cast<int> (lut.size()) - 1, juce::roundToInt (t * static_cast<float> (lut.size() - 1))));
    return lut[i];
}

void Spectrogram::writeRow (const float* db, int n, float minDb, float maxDb)
{
    newest = newest < 0 ? rows - 1 : (newest - 1 + rows) % rows; // decreasing: newest on top when drawn
    ++written;
    const float scale = 1.0f / juce::jmax (1.0e-3f, maxDb - minDb);
    juce::Image::BitmapData data (image, 0, newest, columns, 1, juce::Image::BitmapData::writeOnly);
    for (int x = 0; x < columns; ++x)
    {
        const float level = x < n ? db[x] : minDb;
        data.setPixelColour (x, 0, colourFor ((level - minDb) * scale));
    }
}

void Spectrogram::clear()
{
    const auto floor = lut[0];
    juce::Image::BitmapData data (image, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < columns; ++x)
            data.setPixelColour (x, y, floor);
    newest = -1;
    written = 0;
}

void Spectrogram::draw (juce::Graphics& g, juce::Rectangle<float> area) const
{
    if (area.isEmpty())
        return;
    const float sx = area.getWidth() / static_cast<float> (columns);
    const float rowH = area.getHeight() / static_cast<float> (rows);
    const int top = newest < 0 ? 0 : newest;

    // Ring rows [top, rows) from the top of the area, then [0, top) below them.
    const float splitY = area.getY() + static_cast<float> (rows - top) * rowH;
    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (juce::Rectangle<float>::leftTopRightBottom (area.getX(), area.getY(), area.getRight(), splitY)
                                .getSmallestIntegerContainer());
        g.drawImageTransformed (image, juce::AffineTransform::translation (0.0f, static_cast<float> (-top))
                                           .scaled (sx, rowH)
                                           .translated (area.getX(), area.getY()));
    }
    if (top > 0)
    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (juce::Rectangle<float>::leftTopRightBottom (area.getX(), splitY, area.getRight(), area.getBottom())
                                .getSmallestIntegerContainer());
        g.drawImageTransformed (image, juce::AffineTransform::translation (0.0f, static_cast<float> (rows - top))
                                           .scaled (sx, rowH)
                                           .translated (area.getX(), area.getY()));
    }
}
} // namespace flub::app::ui
