// Flubsound Pro - loudness, dynamics and stereo telemetry of the selected strip.
//
//   LOUDNESS  momentary / short-term / integrated LUFS (EBU R128 / BS.1770),
//             loudness range (LU), true-peak maximum, auto-level gain
//   DYNAMICS  gain-reduction meters: compressor (with upward gain), limiter,
//             multiband glue, clipper energy (vs. the SafetyGovernor's
//             -30 dB budget), bass protection, master safety limiter
//   STEREO    correlation meter (-1 .. +1) and effective width
// All values come from one MeterSnapshot per display frame; gain-reduction
// bars get a short release so they stay readable. Their colours, the
// correlation meter's and the true-peak warning follow the meter palette
// (Theme::statusColours). Clicking the integrated readout resets it
// (onResetRequested -> MeterBus::resetLoudnessRequest).
#pragma once

#include "MeterSnapshot.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace flub::app::ui
{
class LoudnessPanel : public juce::Component, public juce::SettableTooltipClient
{
public:
    LoudnessPanel();

    void update (const MeterSnapshot& snapshot, double dtSeconds);
    void reset();

    std::function<void()> onResetRequested;

    void paint (juce::Graphics& g) override;
    void mouseUp (const juce::MouseEvent& e) override;

private:
    struct Shown
    {
        float momentary = -160.0f, shortTerm = -160.0f, integrated = -160.0f, range = 0.0f, truePeakMax = -160.0f;
        float autoLevel = 0.0f;
        float comp = 0.0f, compUp = 0.0f, limiter = 0.0f, glue = 0.0f, clip = -160.0f, bass = 0.0f, master = 0.0f;
        float correlation = 1.0f, width = 1.0f;
        bool active = false;
    };

    void drawGainReductionRow (juce::Graphics& g, juce::Rectangle<float> row, const juce::String& name, float reductionDb,
                               float rangeDb, float upwardDb = 0.0f);

    Shown shown, painted;
    float sinceRepaint = 0.0f; // readouts refresh at <= 20 Hz
    juce::Rectangle<float> integratedArea;
};
} // namespace flub::app::ui
