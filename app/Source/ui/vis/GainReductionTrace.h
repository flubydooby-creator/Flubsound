// Flubsound Pro - gain-reduction history (visualiser "gain-reduction").
//
// The meter bus's gain reductions (the DYNAMICS rows of the loudness panel)
// over the last kSeconds, one slot per kSlotSeconds holding each stage's
// deepest reading: compressor (accent), limiter (status alert colour, with a
// translucent fill), glue (status caution colour), bass protection (blue)
// and the master safety limiter (text colour). 0 dB at the top; the scale
// is 0 .. -12 dB and grows to -24 dB while the history holds a reduction
// deeper than -11 dB. A stage only draws where it reduces by more than
// kVisibleDb (no flat lines piled up at 0). The legend carries the current
// values. Kept fed while hidden (keepsHistory).
#pragma once

#include "VisCommon.h"
#include "Visualiser.h"

#include <array>

namespace flub::app::ui::vis
{
class GainReductionTrace : public Visualiser
{
public:
    static constexpr double kSeconds = 15.0, kSlotSeconds = 0.025;
    static constexpr int kSlots = 600;
    static constexpr float kVisibleDb = -0.05f;
    enum Stage
    {
        Compressor = 0,
        Limiter,
        Glue,
        BassProtect,
        Master,
        kNumStages
    };

    GainReductionTrace();

    void reset() override;
    void advance (const FrameContext& frame) override;
    bool keepsHistory() const override { return true; }

    const SlotHistory& getHistory() const noexcept { return history; }
    /** The scale's depth (dB, 12 or 24). */
    float getRangeDb() const noexcept { return rangeDb; }
    static const char* stageName (int stage) noexcept;
    juce::Colour stageColour (int stage) const;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void rebuildPaths();
    float yForDb (float db) const noexcept;

    SlotHistory history { kSlots,
                          kSlotSeconds,
                          { SlotHistory::Combine::Min, SlotHistory::Combine::Min, SlotHistory::Combine::Min, SlotHistory::Combine::Min,
                            SlotHistory::Combine::Min },
                          0.0f };
    std::array<float, kNumStages> current {};
    std::array<juce::Path, kNumStages> lines;
    juce::Path limiterFill;
    float rangeDb = 12.0f;
    juce::Rectangle<float> well, plot;
};
} // namespace flub::app::ui::vis
