// Flubsound Pro - Boost Intensity + the five mode macros.
//
// BoostDial  the large 0-100 % "Boost Intensity" arc control (a juce::Slider,
//            so drag, wheel, keyboard, double-click reset and accessibility
//            come for free). An inner arc shows how much of it is actually
//            applied: MeterBus::governorScale scales every "governed" macro
//            contribution (bass, drive, saturation) when the SafetyGovernor
//            detects over-processing; the pill below the dial names it.
// Macros     five knobs whose captions come from flub::MacroMap::macroName
//            (Music: Punch / Width / Clarity / Loudness / Warmth, Gaming:
//            Footsteps / Positional / Impact / Detail / Voice & Score) and
//            change with the mode.
// All controls are bound to the selected strip's ParameterStore.
#pragma once

#include "ParamKnob.h"
#include "ParameterBinding.h"
#include "engine/EngineController.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

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

class BoostPanel : public juce::Component
{
public:
    explicit BoostPanel (EngineController& controller);

    /** Mode changes the macro captions / tooltips. */
    void setMode (flub::param::ModeValue mode);
    void setGovernorScale (float scale);

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    EngineController& controller;
    BoostDial dial;
    std::array<ParamKnob, 5> macros;
    ParameterBinder binder;
    flub::param::ModeValue mode = flub::param::ModeValue::Music;
    juce::Rectangle<int> dialArea, pillArea, macroArea, headerArea;
};
} // namespace flub::app::ui
