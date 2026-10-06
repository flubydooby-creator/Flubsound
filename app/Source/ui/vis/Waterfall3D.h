// Flubsound Pro - 3D waterfall (visualiser "waterfall-3d").
//
// The output spectrum's last kRows * kRowSeconds (6 s) as a landscape of
// ridges in perspective: log frequency across (20 Hz left .. 20 kHz right),
// level as height, time going into the screen (the live spectrum is the
// front ridge, older rows recede towards a horizon). Hidden-line style: the
// ridges are drawn back to front and each one is filled with the well colour
// (fading into the horizon's haze with depth) before its line is stroked, so
// a near ridge hides what is behind it. The front ridge glows; the lines
// fade from the accent (front) to the other mode's colour (back), over a
// perspective floor grid and a horizon glow (synthwave).
//
// Data: once per frame the analyser's displayed post levels
// (SpectrumAnalyzer::getDisplayLevelDb, after its ballistics) are sampled
// at kColumns log-spaced points and tilted +4.5 dB/octave around 1 kHz (so
// music reads level, as on the spectrum); each row keeps the highest level
// of its kRowSeconds. An automatic range keeps the loudest band near the
// top: it follows a louder spectrum at once and falls back over a few
// seconds. Rows scroll smoothly (sub-row offset). No second analysis.
//
// Message thread only. Rows, paths and the backdrop image are allocated in
// the constructor / resized(); a frame allocates nothing.
#pragma once

#include "Visualiser.h"

#include <array>
#include <vector>

namespace flub::app::ui::vis
{
class Waterfall3D : public Visualiser
{
public:
    static constexpr int kRows = 72;
    static constexpr int kColumns = 240;
    static constexpr double kRowSeconds = 1.0 / 12.0; // 72 rows: 6 s
    static constexpr float kTiltDbPerOctave = 4.5f;
    static constexpr float kRangeDb = 60.0f;          // the height scale: top - kRangeDb .. top
    static constexpr float kMinTopDb = -45.0f, kMaxTopDb = 6.0f;
    static constexpr float kTopReleaseSeconds = 3.0f;
    static constexpr float kFarDepth = 3.6f;          // z of the back row (the front is 1)

    Waterfall3D();

    void setSampleRate (double sampleRate) override;
    void reset() override;
    void advance (const FrameContext& frame) override;
    bool keepsHistory() const override { return false; }

    // ---- Pure mapping (tested) -------------------------------------------------------------
    /** Frequency of column c (log-spaced, 20 Hz .. 20 kHz). */
    static double columnHz (int column) noexcept;
    /** The column nearest `hz`. */
    static int columnFor (double hz) noexcept;
    /** Height 0..1 of a (tilted) level for the range top `topDb`. */
    static float heightFor (float db, float topDb) noexcept;
    /** Screen point of the landscape point at u (0 left .. 1 right), depth
        (0 the front row .. 1 the back row) and height (0 .. 1) in `area`:
        the front row is the widest and lowest, rows converge towards the
        horizon (kFarDepth perspective). */
    static juce::Point<float> project (juce::Rectangle<float> area, float u, float depth, float height) noexcept;

    // ---- State (tests) ----------------------------------------------------------------------
    /** Level (dB, tilted) of column c in the completed row `age` rows ago (0 = newest). */
    float getLevel (int age, int column) const noexcept;
    /** The live (front) row's level of column c (dB, tilted). */
    float getLiveLevel (int column) const noexcept { return live[static_cast<size_t> (column)]; }
    int getFilled() const noexcept { return filled; }
    float getTopDb() const noexcept { return topDb; }
    /** Where the landscape is drawn (component coordinates). */
    juce::Rectangle<float> getPlotArea() const noexcept { return plot; }

    void paint (juce::Graphics& g) override;
    void resized() override;
    void lookAndFeelChanged() override;
    void parentHierarchyChanged() override { lookAndFeelChanged(); }

private:
    void rebuildPaths();
    void buildRidge (juce::Path& line, juce::Path& fill, const float* levels, float depth) const;
    void renderBackdrop();

    std::vector<float> rows;                         // kRows x kColumns ring (dB, tilted)
    std::array<float, kColumns> live {}, running {}, tiltDb {};
    std::array<double, kColumns> hz {};
    int newest = -1, filled = 0;
    double elapsed = 0.0;
    bool runningEmpty = true, anyData = false;
    float topDb = -30.0f;

    std::array<juce::Path, kRows + 1> lines, fills; // [0] = live, [k] = completed row k - 1
    std::array<float, kRows + 1> depths {};
    int numDrawn = 0;
    juce::Path floorGrid;
    juce::Image backdrop;
    juce::Colour accent, farColour, haze;
    juce::Rectangle<float> well, plot;
};
} // namespace flub::app::ui::vis
