// Flubsound Pro - input / output level meters.
//
// L/R bars for the strip input and output: RMS as the solid body, peak as
// the translucent top, a peak-hold line (1.5 s, then falls at 20 dB/s) and a
// latching clip indicator per pair (click to clear). Scale: IEC 60268-18
// deflection (more resolution near 0 dBFS). The meter colours come from the
// look-and-feel's meter palette (standard or colour-blind safe).
//
// On the right a true-peak readout shows the maximum since the last reset
// (MeterBus::outTruePeakMaxDb); clicking it asks the audio thread to reset
// the true-peak hold and the integrated loudness (onResetRequested ->
// MeterBus::resetLoudnessRequest).
#pragma once

#include "MeterSnapshot.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace flub::app::ui
{
class LevelMeters : public juce::Component, public juce::SettableTooltipClient
{
public:
    LevelMeters();

    /** Once per display frame. */
    void update (const MeterSnapshot& snapshot, double dtSeconds);
    void reset();

    std::function<void()> onResetRequested;

    /** IEC 60268-18 meter deflection, 0..1 for -70 .. 0 dBFS. */
    static float deflection (float db) noexcept;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;

private:
    struct Channel
    {
        float peak = -100.0f, rms = -100.0f, hold = -100.0f, holdAge = 0.0f;
    };

    void updateChannel (Channel& c, float peakDb, float rmsDb, float dt) noexcept;
    void drawPair (juce::Graphics& g, juce::Rectangle<float> area, const Channel& l, const Channel& r, bool clipped, const juce::String& label);

    std::array<Channel, 2> in, out;
    bool inClip = false, outClip = false, active = false;
    float truePeakMax = -160.0f, truePeakNow = -160.0f;
    juce::Rectangle<float> barsArea, inArea, outArea, truePeakArea, inClipArea, outClipArea;
    juce::String lastTip;
};
} // namespace flub::app::ui
