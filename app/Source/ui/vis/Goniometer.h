// Flubsound Pro - goniometer / vectorscope (visualiser "goniometer").
//
// The output's mid / side pairs plotted as a Lissajous figure: mid up, side
// across, so a mono signal is a vertical line, left only leans up-left (L),
// right only up-right (R), out-of-phase sound lies on the horizontal (S)
// axis and wide, decorrelated sound fills a round cloud. Every sample is
// splatted into a Phosphor grid (kCells x kCells) that fades with a
// kAfterglowSeconds time constant: the trail. An automatic gain (peak of
// |mid|, |side| with a slow release, 0 .. +30 dB) keeps quiet programme
// visible; the gain is printed in the corner, with the meter bus's
// correlation. Message thread only; no allocation after construction.
#pragma once

#include "VisCommon.h"
#include "Visualiser.h"

#include <vector>

namespace flub::app::ui::vis
{
class Goniometer : public Visualiser
{
public:
    static constexpr int kCells = 320;
    static constexpr float kAfterglowSeconds = 0.12f;
    static constexpr float kMaxGainDb = 30.0f;
    static constexpr float kGainReleaseSeconds = 0.8f;
    static constexpr float kTargetPeak = 0.95f; // the automatic gain puts the recent peak here (1 = the circle)
    static constexpr int kMaxSamplesPerFrame = 8192;

    Goniometer();

    void setSampleRate (double sampleRate) override;
    void reset() override;
    void pushPost (const float* mid, const float* side, int numSamples) override;
    void advance (const FrameContext& frame) override;

    /** Normalised scope position (x right, y up, the unit circle = full
        scale at `gain`) of one mid / side sample: x = -side * gain (left
        channel to the left), y = mid * gain. */
    static juce::Point<float> scopePoint (float mid, float side, float gain) noexcept;

    /** The automatic gain (linear, 1 .. +30 dB). */
    float getGain() const noexcept { return gain; }
    const Phosphor& getPhosphor() const noexcept { return phosphor; }
    /** The square the phosphor is drawn into (component coordinates). */
    juce::Rectangle<float> getScopeArea() const noexcept { return scope; }

    void paint (juce::Graphics& g) override;
    void resized() override;
    void lookAndFeelChanged() override;
    void parentHierarchyChanged() override { lookAndFeelChanged(); }

private:
    std::vector<float> mids, sides; // this frame's samples (the newest kMaxSamplesPerFrame)
    int count = 0, writePos = 0;
    Phosphor phosphor { kCells, kCells };
    double sampleRate = 48000.0;
    float peak = 0.0f, gain = 1.0f, correlation = 1.0f;
    bool lit = false, active = false;
    juce::Rectangle<float> well, scope;
};
} // namespace flub::app::ui::vis
