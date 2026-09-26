// Flubsound Pro - scrolling output history.
//
// The post-processing tap (mono mid) is decimated into min / max columns
// (kSeconds of history over kColumns columns, independent of the pixel
// width) and drawn as a filled, mirrored envelope that scrolls right to left.
// The short-term loudness (LUFS) of the same moments is recorded per column
// and drawn as a trace on a -40 .. 0 LUFS axis. Paths are rebuilt once per
// frame in advance(); paint() only fills / strokes them.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

namespace flub::app::ui
{
class WaveformHistory : public juce::Component
{
public:
    static constexpr int kColumns = 1200;
    static constexpr double kSeconds = 12.0;

    WaveformHistory();

    void setSampleRate (double newSampleRate);
    void push (const float* samples, int numSamples);
    /** Short-term loudness at "now" (recorded into the running column). */
    void setLoudness (float shortTermLufs);
    void reset();
    /** Rebuilds the paths if new columns arrived. */
    void advance();

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    struct Column
    {
        float lo = 0.0f, hi = 0.0f, lufs = -160.0f;
    };

    void rebuildPaths();

    std::vector<Column> columns; // ring, newest at writeIndex - 1
    int writeIndex = 0, samplesInColumn = 0, samplesPerColumn = 480;
    float runningLo = 0.0f, runningHi = 0.0f, currentLufs = -160.0f, latestLufs = -160.0f;
    double sampleRate = 48000.0;
    bool dirty = true, hasAudio = false;

    juce::Rectangle<float> plot;
    juce::Path envelope, loudnessTrace;
};
} // namespace flub::app::ui
