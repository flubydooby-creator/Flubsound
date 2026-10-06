// Flubsound Pro - chromagram (visualiser "chromagram", main view or strip).
//
// How much of each pitch class C .. B the output holds, every octave folded
// together (PitchEstimator::foldChroma: the spectral peaks' levels on a
// 50 dB scale, largest = 1), as 12 bars that rise fast and fall over
// kReleaseSeconds; beside them (below them in a narrow view) a scrolling
// chroma history, 12 rows by the last kHistorySeconds, newest at the right.
// The header shows the estimated key (music::KeyDetector, Krumhansl-
// Schmuckler over a ~15 s window) with its confidence; the key's scale
// tones are marked under the bars, the tonic in the accent.
// As a strip: 12 cells lit by the bars, and the key.
// Kept fed while hidden (keepsHistory): the history and the key carry on.
#pragma once

#include "MusicTheory.h"
#include "Visualiser.h"

#include <vector>

namespace flub::app::ui::vis
{
class ChromagramView : public Visualiser
{
public:
    static constexpr double kAttackSeconds = 0.03, kReleaseSeconds = 0.35;
    static constexpr double kColumnSeconds = 0.05;
    static constexpr int kColumns = 300; // 15 s
    static constexpr double kHistorySeconds = kColumns * kColumnSeconds;

    ChromagramView();

    void setSampleRate (double sampleRate) override;
    void reset() override;
    void pushPost (const float* mid, const float* side, int numSamples) override;
    void advance (const FrameContext& frame) override;
    bool keepsHistory() const override { return true; }
    int getStripHeight() const override { return 34; }

    /** The bar of pitch class pc (0 = C), 0..1. */
    float getBar (int pc) const noexcept { return bars[static_cast<size_t> (((pc % 12) + 12) % 12)]; }
    /** History: column `age` columns ago (0 = newest completed), pitch class pc. */
    float getHistory (int age, int pc) const noexcept;
    int getHistoryFilled() const noexcept { return filled; }
    const music::MusicListener& getListener() const noexcept { return listener; }

    void paint (juce::Graphics& g) override;
    void lookAndFeelChanged() override;

private:
    void renderColumn (int column) noexcept;
    void paintBars (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour accent) const;
    void paintHistory (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour accent);
    void paintHeader (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour accent) const;

    music::MusicListener listener;
    std::array<float, 12> target {}, bars {}, painted {}, column {};
    std::vector<float> values; // kColumns x 12 ring
    int newest = -1, filled = 0;
    double columnElapsed = 0.0;
    juce::Image image; // 12 rows (B top .. C bottom) x kColumns, same ring order as values
    std::array<juce::PixelARGB, 256> lut {};
    juce::Colour lutAccent;
    int paintedKey = -1;
    float paintedConfidence = 0.0f;
};
} // namespace flub::app::ui::vis
