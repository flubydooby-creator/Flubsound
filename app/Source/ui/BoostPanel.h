// Flubsound Pro - Boost Intensity + the five mode macros.
//
// BoostDial  the large 0-100 % "Boost Intensity" arc control (a juce::Slider,
//            so drag, wheel, keyboard, double-click reset and accessibility
//            come for free). An inner arc shows how much of it is actually
//            applied: MeterBus::governorScale scales every "governed" macro
//            contribution (bass, drive, saturation) when the SafetyGovernor
//            detects over-processing; a status chip in the header names it.
// Macros     five knobs whose captions come from flub::MacroMap::macroName
//            (Music: Punch / Width / Clarity / Loudness / Warmth, Gaming:
//            Footsteps / Positional / Impact / Detail / Voice & Score) and
//            change with the mode.
// Governor  the header chip names the SafetyGovernor's state and reason
//           (docs/11 E06: "Governor 72 % . limiter"), its tooltip the ~3 s
//           averages against the budgets and the protection strength; a
//           click on it chooses the strength (Off / Normal / Strict,
//           EngineController::setProtectionStrength).
// Active    "active now" chips under the macros (docs/11 E38 slice): the
//           stages that change the sound right now, from the chain's
//           effective values (after the macros and the governor) and its
//           meters, e.g. "Bass +3.1 dB @ 70 Hz", "Width 118 %", "Maximizer
//           +4.0 dB drive"; what does not fit is counted ("+3") and listed in
//           the tooltip.
// All controls are bound to the selected strip's ParameterStore.
#pragma once

#include "MeterSnapshot.h"
#include "ParamKnob.h"
#include "ParameterBinding.h"
#include "engine/EngineController.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <vector>

namespace flub::app::ui
{
class BoostDial : public juce::Slider
{
public:
    BoostDial();

    /** 0.3 .. 1: share of the governed macro amounts that is applied. */
    void setGovernorScale (float scale);
    float getGovernorScale() const noexcept { return governor; }

    void paint (juce::Graphics& g) override;

private:
    float governor = 1.0f;
};

class BoostPanel : public juce::Component, public juce::TooltipClient
{
public:
    explicit BoostPanel (EngineController& controller);

    /** Mode changes the macro captions / tooltips. */
    void setMode (flub::param::ModeValue mode);
    void setGovernorScale (float scale);
    /** Per display frame: the governor readout, and (every few frames) the
        active-now chips from the selected strip's effective values. */
    void update (const MeterSnapshot& snapshot);

    // ---- Readouts (pure; tested) -------------------------------------------------------
    struct GovernorReadout
    {
        juce::String text;   // the chip: "Safety governor OK", "Governor 72 % . limiter"
        juce::String detail; // tooltip: what it does, the averages against the budgets, the strength
        bool limiting = false;
    };
    static GovernorReadout describeGovernor (const MeterSnapshot& snapshot, flub::ProtectionStrength strength);

    struct ActiveStage
    {
        juce::String text;   // "Bass +3.1 dB @ 70 Hz"
        juce::String detail; // one tooltip line
    };
    /** The stages that change the sound now, in signal order: `effective`
        returns a parameter's effective value (ProcessingChain::effectiveValue),
        the snapshot the meters (limiter, dynamic EQ, compressor, auto level,
        automatic preamp, input fold). stripChannels > 2 lets the virtualiser
        count. */
    static std::vector<ActiveStage> describeActiveStages (const std::function<float (int)>& effective, const MeterSnapshot& snapshot,
                                                          int stripChannels);

    const GovernorReadout& getGovernorReadout() const noexcept { return governor; }
    const std::vector<ActiveStage>& getActiveStages() const noexcept { return stages; }

    juce::String getTooltip() override;
    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent& e) override;

private:
    void showStrengthMenu();
    void refreshStages (const MeterSnapshot& snapshot);

    EngineController& controller;
    BoostDial dial;
    std::array<ParamKnob, 5> macros;
    ParameterBinder binder;
    flub::param::ModeValue mode = flub::param::ModeValue::Music;
    juce::Rectangle<int> dialArea, macroArea, headerArea, chipsArea;
    juce::Rectangle<float> governorChip;
    GovernorReadout governor;
    std::vector<ActiveStage> stages;
    int shownStages = 0, framesSinceStages = 1000;
};
} // namespace flub::app::ui
