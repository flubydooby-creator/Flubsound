// Flubsound Pro - correlation meter strip (visualiser "correlation").
//
// A horizontal -1 .. +1 bar under the spectrum (or under any view): the
// short-term phase correlation of the output's left and right channels
// (L = mid + side, R = mid - side from the aligned post pairs; one-pole
// averages of LR, L^2 and R^2 over kIntegrationSeconds), drawn as a needle
// with a fill from 0, coloured good (>= 0.3) / caution (0 .. 0.3) / alert
// (< 0) in the status colours (colour-blind palette aware), a red zone
// below 0 and a hold marker at the lowest value of the last kHoldSeconds
// (then it rises back at kHoldReleasePerSecond). Below kGateDb (mean
// square of L and R) it reads "no signal" and the needle dims.
#pragma once

#include "Visualiser.h"

namespace flub::app::ui::vis
{
class CorrelationMeter : public Visualiser
{
public:
    static constexpr double kIntegrationSeconds = 0.1;
    static constexpr double kHoldSeconds = 3.0;
    static constexpr float kHoldReleasePerSecond = 0.5f;
    static constexpr float kGateDb = -70.0f;

    CorrelationMeter();

    void setSampleRate (double sampleRate) override;
    void reset() override;
    void pushPost (const float* mid, const float* side, int numSamples) override;
    void advance (const FrameContext& frame) override;
    int getStripHeight() const override { return 30; }

    /** Short-term correlation (-1 .. +1; the last reading while there is no signal). */
    float getCorrelation() const noexcept { return correlation; }
    /** The hold marker: the lowest correlation of the last kHoldSeconds. */
    float getHold() const noexcept { return hold; }
    /** True while the output is above kGateDb. */
    bool hasSignal() const noexcept { return signal; }

    /** The bar (component coordinates) and the x of a correlation on it. */
    juce::Rectangle<float> getBarArea() const noexcept { return bar; }
    float xForCorrelation (float value) const noexcept;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    double sampleRate = 48000.0, coefficient = 0.0;
    double lr = 0.0, ll = 0.0, rr = 0.0;
    float correlation = 1.0f, hold = 1.0f, painted = 2.0f, paintedHold = 2.0f;
    double holdAge = 0.0;
    bool signal = false, paintedSignal = true;
    juce::Rectangle<float> well, bar, readout;
};
} // namespace flub::app::ui::vis
