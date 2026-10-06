// Flubsound Pro - the analyser's scrolling spectrogram (waterfall) image.
//
// One row per analysis hop: the post spectrum's display points (the
// analyser's log-spaced 20 Hz - 20 kHz axis, so the image's columns line up
// with xForFrequency) mapped through a 256-entry colour table (theme well ->
// accent -> text) into a fixed-size software image used as a ring of rows.
// The newest row is drawn at the top and older rows scroll down; nothing is
// moved in memory and nothing is allocated after construction (writeRow only
// touches one row's pixels, the colour table is a std::array).
//
// Message thread only (SpectrumAnalyzer::advance / paint).
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

namespace flub::app::ui
{
class Spectrogram
{
public:
    Spectrogram (int columns, int rows);

    /** Colour table: level 0 (floor) = background ... 1 (top) = hot, through accent. */
    void setColours (juce::Colour background, juce::Colour accent, juce::Colour hot);

    /** Writes the newest row: `db[i]` for column i (n values, n = columns),
        mapped linearly from minDb (floor) to maxDb (top). */
    void writeRow (const float* db, int n, float minDb, float maxDb);
    /** Fills every row with the floor colour (strip switch, mode change). */
    void clear();

    /** Draws the history into `area`: newest row at the top, the oldest at
        the bottom, columns stretched across the area's width. */
    void draw (juce::Graphics& g, juce::Rectangle<float> area) const;

    int getColumns() const noexcept { return columns; }
    int getRows() const noexcept { return rows; }
    /** Ring index of the newest row (-1 before the first write). */
    int getNewestRow() const noexcept { return newest; }
    int64_t getRowsWritten() const noexcept { return written; }
    const juce::Image& getImage() const noexcept { return image; }
    /** The colour a level of `t` (0 floor .. 1 top) gets. */
    juce::Colour colourFor (float t) const noexcept;

private:
    int columns, rows;
    juce::Image image;
    std::array<juce::Colour, 256> lut {};
    int newest = -1;
    int64_t written = 0;
};
} // namespace flub::app::ui
