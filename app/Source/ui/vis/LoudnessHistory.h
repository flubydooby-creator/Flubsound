// Flubsound Pro - loudness history (visualiser "loudness-history").
//
// The output's momentary (400 ms) and short-term (3 s) loudness from the
// meter bus (the LOUDNESS panel's readings), recorded every kSlotSeconds
// (momentary: the slot's maximum, short-term: its last reading) over the
// last kSeconds, scrolling right to left. Momentary is a thin line over a
// translucent area, short-term a bold accent line; readings below
// kFloorLufs leave a gap. When a loudness target is in force
// (FrameContext::targetLufs: the maximizer's automatic drive target or the
// auto-level target) it is a dashed amber line with its name. Axis
// kMinLufs .. kMaxLufs, grid every 6 LU. Kept fed while hidden (keepsHistory).
#pragma once

#include "VisCommon.h"
#include "Visualiser.h"

namespace flub::app::ui::vis
{
class LoudnessHistory : public Visualiser
{
public:
    static constexpr double kSeconds = 60.0, kSlotSeconds = 0.1;
    static constexpr int kSlots = 600;
    static constexpr float kMinLufs = -42.0f, kMaxLufs = 0.0f, kFloorLufs = -70.0f;
    enum Value
    {
        Momentary = 0,
        ShortTerm = 1
    };

    LoudnessHistory();

    void reset() override;
    void advance (const FrameContext& frame) override;
    bool keepsHistory() const override { return true; }

    const SlotHistory& getHistory() const noexcept { return history; }
    float getTargetLufs() const noexcept { return targetLufs; }
    float yForLufs (float lufs) const noexcept;
    juce::Rectangle<float> getPlotArea() const noexcept { return plot; }

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void rebuildPaths();

    SlotHistory history { kSlots, kSlotSeconds, { SlotHistory::Combine::Max, SlotHistory::Combine::Last }, -160.0f };
    float momentary = -160.0f, shortTerm = -160.0f, targetLufs = std::nanf ("");
    juce::String targetName;
    juce::Path momentaryLine, momentaryFill, shortLine;
    juce::Rectangle<float> well, plot;
};
} // namespace flub::app::ui::vis
