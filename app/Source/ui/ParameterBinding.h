// Flubsound Pro - binds stock JUCE controls to flub::param::ParameterStore ids.
//
// ParamFormat   value <-> text for every flub::param::Unit (dB, Hz / kHz,
//               ms / s, %, LUFS, degrees, mm, dB/s, ratio x:1, choices,
//               toggles) and the NormalisableRange of a parameter (with the
//               layout's skewCentre).
// ParameterBinder
//               attaches juce::Slider / juce::Button / juce::ComboBox objects
//               to parameter ids of the store returned by a StoreProvider (the
//               selected strip's store, re-fetched on every use so strip
//               switches and engine reconfigurations are picked up).
//               * User gestures write the store (relaxed atomics, RT-safe).
//               * A 30 Hz timer polls store.version() (and the store address)
//                 and refreshes the controls only when something changed.
//                 Refreshes never write back, so there is no feedback loop,
//                 and a control that is being dragged is not refreshed.
//               * Optional EffectiveProvider (the strip's ProcessingChain,
//                 read through effectiveValue(), which is thread-safe) feeds
//                 the knob's post-macro "effective value" ring.
//               No audio-thread callbacks are involved anywhere.
//
// Lifetime: the binder must be destroyed before the controls it is bound to
// (declare it after them); its destructor detaches every callback.
#pragma once

#include "flub/engine/Parameters.h"

namespace flub
{
class ProcessingChain;
}

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace flub::app::ui
{
namespace ParamFormat
{
const flub::param::Info& info (int paramId);

/** Display text for a value, e.g. "+3.0 dB", "2.40 kHz", "4.0:1", "Tube". */
juce::String toText (int paramId, float value, bool withUnit = true);
/** Parses user input ("3.2k", "-6", "25 %", "Tube", "off" ...). */
float fromText (int paramId, const juce::String& text);

/** Unit suffix only (" dB", " Hz", "%" ...), empty for choices / toggles. */
juce::String unitSuffix (int paramId);

juce::NormalisableRange<double> range (int paramId);

/** True for continuous parameters whose range spans zero (knob arc grows from 0). */
bool isBipolar (int paramId);

/** Range, text conversion, double-click default, accessible title / tooltip. */
void configureSlider (juce::Slider& slider, int paramId);
} // namespace ParamFormat

class ParameterBinder final : private juce::Timer
{
public:
    using StoreProvider = std::function<flub::param::ParameterStore*()>;
    using EffectiveProvider = std::function<const flub::ProcessingChain*()>;

    explicit ParameterBinder (StoreProvider storeProvider, EffectiveProvider effectiveProvider = {});
    ~ParameterBinder() override;

    /** (Re)binds a slider to paramId (configures range / text functions). */
    void bindSlider (juce::Slider& slider, int paramId);
    /** Toggle parameter; the button toggles on click. */
    void bindToggle (juce::Button& button, int paramId);
    /** Choice parameter; the combo is filled with the choice labels. */
    void bindChoice (juce::ComboBox& combo, int paramId);

    void unbind (juce::Component& control);
    void unbindAll();

    /** Pulls every bound control from the store now (force = even if unchanged). */
    void refresh (bool force = false);

    flub::param::ParameterStore* getStore() const { return storeProvider != nullptr ? storeProvider() : nullptr; }

    /** Called on the message thread after a user gesture wrote a parameter. */
    std::function<void (int paramId)> onUserEdit;
    /** Called after the controls were refreshed from a changed store. */
    std::function<void()> onRefreshed;

private:
    enum class Kind
    {
        Slider,
        Toggle,
        Choice
    };

    struct Binding
    {
        juce::Component::SafePointer<juce::Component> control;
        Kind kind = Kind::Slider;
        int paramId = 0;
        float lastEffective = -1.0e9f;
    };

    void timerCallback() override;
    Binding* find (juce::Component& control);
    Binding& add (juce::Component& control, Kind kind, int paramId);
    void detach (Binding& b);
    void pull (Binding& b, const flub::param::ParameterStore& store);
    void write (int paramId, float value);
    void updateEffective();

    StoreProvider storeProvider;
    EffectiveProvider effectiveProvider;
    std::vector<std::unique_ptr<Binding>> bindings;
    const flub::param::ParameterStore* lastStore = nullptr;
    uint32_t lastVersion = 0;
    bool updating = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParameterBinder)
};
} // namespace flub::app::ui
