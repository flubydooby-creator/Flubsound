// Flubsound Pro - waveform before / after (visualiser "waveform").
//
// A scrolling oscilloscope-style envelope of the input (pre tap, grey) and
// the output (post mid, accent) over the last kSeconds: each stream is cut
// into kColumns columns of kSeconds / kColumns (5 ms) and each column keeps
// the lowest and highest sample, drawn as a mirrored filled shape on a
// linear -1 .. +1 (0 dBFS) axis with -6 dB lines. Boost raising the
// body, Punch sharpening attacks and the limiter flattening the peaks show
// as the accent shape against the grey one. The two streams scroll
// independently with their own sample counts (the output lags by the
// chain's latency, as it does in reality). Kept fed while hidden.
#pragma once

#include "Visualiser.h"

#include <array>
#include <vector>

namespace flub::app::ui::vis
{
class WaveformView : public Visualiser
{
public:
    static constexpr int kColumns = 800;
    static constexpr double kSeconds = 4.0;

    struct Column
    {
        float lo = 0.0f, hi = 0.0f;
    };

    WaveformView();

    void setSampleRate (double sampleRate) override;
    void reset() override;
    void pushPre (const float* mid, int numSamples) override;
    void pushPost (const float* mid, const float* side, int numSamples) override;
    void advance (const FrameContext& frame) override;
    bool keepsHistory() const override { return true; }

    /** A completed column of the input (post = false) or output (post = true):
        age 0 = the newest. */
    const Column& getColumn (bool post, int age) const noexcept;
    int getSamplesPerColumn() const noexcept { return samplesPerColumn; }
    int getFilled (bool post) const noexcept { return streams[post ? 1 : 0].filled; }

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    struct Stream
    {
        std::vector<Column> columns;
        Column running;
        int count = 0, newest = -1, filled = 0;
        bool changed = false;
    };

    void push (Stream& s, const float* samples, int numSamples) noexcept;
    void rebuildPaths();
    void buildShape (const Stream& s, juce::Path& shape) const;
    float yFor (float amplitude) const noexcept;

    std::array<Stream, 2> streams; // 0 = input, 1 = output
    int samplesPerColumn = 240;
    juce::Path preShape, postShape;
    juce::Rectangle<float> well, plot;
};
} // namespace flub::app::ui::vis
