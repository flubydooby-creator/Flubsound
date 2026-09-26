#include "ParameterBinding.h"

#include "FlubLookAndFeel.h"
#include "Theme.h"

#include "flub/engine/ProcessingChain.h"

#include <cmath>

namespace flub::app::ui
{
using flub::param::Unit;

namespace ParamFormat
{
const flub::param::Info& info (int paramId)
{
    const auto& table = flub::param::layout();
    return table[static_cast<size_t> (juce::jlimit (0, flub::param::kNumParams - 1, paramId))];
}

namespace
{
juce::String trimmedNumber (float v, int decimals)
{
    // "70.0" -> "70" for whole numbers, keeps "31.5".
    if (decimals > 0 && std::abs (v - std::round (v)) < 0.05f)
        return juce::String (juce::roundToInt (v));
    return juce::String (v, decimals);
}

int choiceIndex (const flub::param::Info& i, float v)
{
    const int n = static_cast<int> (i.choices.size());
    return juce::jlimit (0, juce::jmax (0, n - 1), static_cast<int> (std::lround (v)));
}
} // namespace

juce::String toText (int paramId, float v, bool withUnit)
{
    const auto& i = info (paramId);
    const auto unit = [withUnit] (const char* suffix) { return withUnit ? juce::String (juce::CharPointer_UTF8 (suffix)) : juce::String(); };

    switch (i.unit)
    {
        case Unit::Choice:
            return i.choices.empty() ? juce::String (juce::roundToInt (v)) : juce::String (i.choices[static_cast<size_t> (choiceIndex (i, v))]);

        case Unit::Toggle:
            return v >= 0.5f ? "On" : "Off";

        case Unit::Db:
        {
            const int decimals = std::abs (v) >= 100.0f ? 0 : 1;
            const bool bipolar = i.minValue < 0.0f && i.maxValue > 0.0f;
            return (bipolar ? Theme::formatSignedDb (v, decimals) : juce::String (v, decimals)) + unit (" dB");
        }

        case Unit::Hz:
            if (v <= 0.0f && i.minValue <= 0.0f)
                return "Off";
            if (v < 1000.0f)
                return (v < 100.0f ? trimmedNumber (v, 1) : juce::String (juce::roundToInt (v))) + unit (" Hz");
            return juce::String (v / 1000.0f, v < 10000.0f ? 2 : 1) + unit (" kHz");

        case Unit::Ms:
            if (v < 10.0f)
                return juce::String (v, 1) + unit (" ms");
            if (v < 1000.0f)
                return juce::String (juce::roundToInt (v)) + unit (" ms");
            return juce::String (v / 1000.0f, 2) + unit (" s");

        case Unit::Percent:
            return juce::String (juce::roundToInt (v * 100.0f)) + unit ("%");

        case Unit::Ratio:
            return (v < 10.0f ? juce::String (v, 1) : juce::String (juce::roundToInt (v))) + ":1";

        case Unit::Lufs:
            return juce::String (v, 1) + unit (" LUFS");

        case Unit::Degrees:
            return juce::String (juce::roundToInt (v)) + unit ("\xc2\xb0");

        case Unit::Millimetres:
            return juce::String (v, 1) + unit (" mm");

        case Unit::DbPerSec:
            return juce::String (v, 1) + unit (" dB/s");

        case Unit::None:
            break;
    }
    return juce::String (v, 2);
}

float fromText (int paramId, const juce::String& text)
{
    const auto& i = info (paramId);
    const auto t = text.trim().toLowerCase();

    if (i.unit == Unit::Choice)
    {
        for (size_t c = 0; c < i.choices.size(); ++c)
            if (t == juce::String (i.choices[c]).toLowerCase())
                return static_cast<float> (c);
        for (size_t c = 0; c < i.choices.size(); ++c)
            if (t.isNotEmpty() && juce::String (i.choices[c]).toLowerCase().startsWith (t))
                return static_cast<float> (c);
        return i.clamp (static_cast<float> (t.getIntValue()));
    }

    if (i.unit == Unit::Toggle)
        return (t == "on" || t == "1" || t == "true" || t == "yes") ? 1.0f : 0.0f;

    if (t == "off" || t == "-inf" || t == "min")
        return i.minValue;
    if (t == "max")
        return i.maxValue;

    auto numberText = t;
    if (i.unit == Unit::Ratio)
        numberText = t.upToFirstOccurrenceOf (":", false, false);

    // ("+3" is not parsed by getDoubleValue(): a leading plus sign is simply dropped.)
    auto value = numberText.retainCharacters ("-0123456789.").getDoubleValue();

    if (i.unit == Unit::Hz && t.containsChar ('k'))
        value *= 1000.0;
    else if (i.unit == Unit::Ms && t.endsWithChar ('s') && ! t.endsWith ("ms"))
        value *= 1000.0;
    else if (i.unit == Unit::Percent)
        value /= 100.0;

    return i.clamp (static_cast<float> (value));
}

juce::String unitSuffix (int paramId)
{
    switch (info (paramId).unit)
    {
        case Unit::Db: return " dB";
        case Unit::Hz: return " Hz";
        case Unit::Ms: return " ms";
        case Unit::Percent: return "%";
        case Unit::Lufs: return " LUFS";
        case Unit::Degrees: return juce::String (juce::CharPointer_UTF8 ("\xc2\xb0"));
        case Unit::Millimetres: return " mm";
        case Unit::DbPerSec: return " dB/s";
        case Unit::Ratio: return ":1";
        case Unit::None:
        case Unit::Choice:
        case Unit::Toggle: break;
    }
    return {};
}

juce::NormalisableRange<double> range (int paramId)
{
    const auto& i = info (paramId);
    juce::NormalisableRange<double> r (i.minValue, i.maxValue);
    if (i.unit == Unit::Choice || i.unit == Unit::Toggle)
        r.interval = 1.0;
    else if (i.skewCentre > i.minValue && i.skewCentre < i.maxValue)
        r.setSkewForCentre (i.skewCentre);
    return r;
}

bool isBipolar (int paramId)
{
    const auto& i = info (paramId);
    return i.unit != Unit::Choice && i.unit != Unit::Toggle && i.minValue < 0.0f && i.maxValue > 0.0f;
}

void configureSlider (juce::Slider& slider, int paramId)
{
    const auto& i = info (paramId);
    slider.setNormalisableRange (range (paramId));
    slider.textFromValueFunction = [paramId] (double v) { return toText (paramId, static_cast<float> (v)); };
    slider.valueFromTextFunction = [paramId] (const juce::String& t) { return static_cast<double> (fromText (paramId, t)); };
    slider.setDoubleClickReturnValue (true, i.defaultValue);
    slider.setTitle (juce::String (i.name));
    slider.setHelpText (juce::String (i.name) + " (double-click to reset to " + toText (paramId, i.defaultValue) + ")");
    slider.updateText();
}
} // namespace ParamFormat

// =============================================================================
// ParameterBinder
// =============================================================================
ParameterBinder::ParameterBinder (StoreProvider s, EffectiveProvider e)
    : storeProvider (std::move (s)), effectiveProvider (std::move (e))
{
    startTimerHz (30);
}

ParameterBinder::~ParameterBinder()
{
    stopTimer();
    unbindAll();
}

ParameterBinder::Binding* ParameterBinder::find (juce::Component& control)
{
    for (auto& b : bindings)
        if (b->control.getComponent() == &control)
            return b.get();
    return nullptr;
}

ParameterBinder::Binding& ParameterBinder::add (juce::Component& control, Kind kind, int paramId)
{
    if (auto* existing = find (control))
    {
        detach (*existing);
        existing->kind = kind;
        existing->paramId = paramId;
        existing->lastEffective = -1.0e9f;
        return *existing;
    }
    auto b = std::make_unique<Binding>();
    b->control = &control;
    b->kind = kind;
    b->paramId = paramId;
    bindings.push_back (std::move (b));
    return *bindings.back();
}

void ParameterBinder::detach (Binding& b)
{
    auto* c = b.control.getComponent();
    if (c == nullptr)
        return;
    if (auto* slider = dynamic_cast<juce::Slider*> (c))
    {
        slider->onValueChange = nullptr;
        slider->getProperties().remove (FlubLookAndFeel::effectiveProperty);
    }
    else if (auto* button = dynamic_cast<juce::Button*> (c))
    {
        button->onClick = nullptr;
    }
    else if (auto* combo = dynamic_cast<juce::ComboBox*> (c))
    {
        combo->onChange = nullptr;
    }
}

void ParameterBinder::bindSlider (juce::Slider& slider, int paramId)
{
    auto& b = add (slider, Kind::Slider, paramId);
    const juce::ScopedValueSetter<bool> guard (updating, true);
    ParamFormat::configureSlider (slider, paramId);
    slider.onValueChange = [this, &slider, paramId]
    {
        if (! updating)
            write (paramId, static_cast<float> (slider.getValue()));
    };
    if (auto* store = getStore())
        pull (b, *store);
}

void ParameterBinder::bindToggle (juce::Button& button, int paramId)
{
    auto& b = add (button, Kind::Toggle, paramId);
    button.setClickingTogglesState (true);
    if (button.getTitle().isEmpty())
        button.setTitle (juce::String (ParamFormat::info (paramId).name));
    button.onClick = [this, &button, paramId]
    {
        if (! updating)
            write (paramId, button.getToggleState() ? 1.0f : 0.0f);
    };
    if (auto* store = getStore())
        pull (b, *store);
}

void ParameterBinder::bindChoice (juce::ComboBox& combo, int paramId)
{
    auto& b = add (combo, Kind::Choice, paramId);
    const auto& i = ParamFormat::info (paramId);
    {
        const juce::ScopedValueSetter<bool> guard (updating, true);
        combo.clear (juce::dontSendNotification);
        for (size_t c = 0; c < i.choices.size(); ++c)
            combo.addItem (juce::String (i.choices[c]), static_cast<int> (c) + 1);
    }
    if (combo.getTitle().isEmpty())
        combo.setTitle (juce::String (i.name));
    combo.onChange = [this, &combo, paramId]
    {
        if (! updating && combo.getSelectedId() > 0)
            write (paramId, static_cast<float> (combo.getSelectedId() - 1));
    };
    if (auto* store = getStore())
        pull (b, *store);
}

void ParameterBinder::unbind (juce::Component& control)
{
    for (auto it = bindings.begin(); it != bindings.end(); ++it)
    {
        if ((*it)->control.getComponent() == &control)
        {
            detach (**it);
            bindings.erase (it);
            return;
        }
    }
}

void ParameterBinder::unbindAll()
{
    for (auto& b : bindings)
        detach (*b);
    bindings.clear();
}

void ParameterBinder::pull (Binding& b, const flub::param::ParameterStore& store)
{
    auto* c = b.control.getComponent();
    if (c == nullptr)
        return;

    const float v = store.get (b.paramId);
    const juce::ScopedValueSetter<bool> guard (updating, true);
    switch (b.kind)
    {
        case Kind::Slider:
            if (auto* slider = dynamic_cast<juce::Slider*> (c); slider != nullptr && ! slider->isMouseButtonDown())
                slider->setValue (v, juce::dontSendNotification);
            break;
        case Kind::Toggle:
            if (auto* button = dynamic_cast<juce::Button*> (c))
                button->setToggleState (v >= 0.5f, juce::dontSendNotification);
            break;
        case Kind::Choice:
            if (auto* combo = dynamic_cast<juce::ComboBox*> (c))
                combo->setSelectedId (static_cast<int> (std::lround (v)) + 1, juce::dontSendNotification);
            break;
    }
}

void ParameterBinder::refresh (bool force)
{
    auto* store = getStore();
    if (store == nullptr)
        return;
    if (! force && store == lastStore && store->version() == lastVersion)
        return;

    lastStore = store;
    lastVersion = store->version();
    for (auto& b : bindings)
        pull (*b, *store);
    if (onRefreshed != nullptr)
        onRefreshed();
}

void ParameterBinder::write (int paramId, float value)
{
    if (auto* store = getStore())
    {
        store->set (paramId, value);
        if (onUserEdit != nullptr)
            onUserEdit (paramId);
    }
}

void ParameterBinder::updateEffective()
{
    if (effectiveProvider == nullptr)
        return;
    const auto* chain = effectiveProvider();
    auto* store = getStore();
    if (chain == nullptr || store == nullptr)
        return;

    for (auto& b : bindings)
    {
        if (b->kind != Kind::Slider)
            continue;
        auto* slider = dynamic_cast<juce::Slider*> (b->control.getComponent());
        if (slider == nullptr)
            continue;

        const auto& i = ParamFormat::info (b->paramId);
        const float eff = i.clamp (chain->effectiveValue (b->paramId));
        const float base = store->get (b->paramId);
        const float tolerance = 1.0e-4f * (i.maxValue - i.minValue);

        if (std::abs (eff - base) > tolerance)
        {
            const auto proportion = static_cast<float> (slider->valueToProportionOfLength (eff));
            if (std::abs (proportion - b->lastEffective) > 0.002f)
            {
                b->lastEffective = proportion;
                slider->getProperties().set (FlubLookAndFeel::effectiveProperty, proportion);
                slider->repaint();
            }
        }
        else if (b->lastEffective > -1.0e8f)
        {
            b->lastEffective = -1.0e9f;
            slider->getProperties().remove (FlubLookAndFeel::effectiveProperty);
            slider->repaint();
        }
    }
}

void ParameterBinder::timerCallback()
{
    refresh (false);
    updateEffective();
}
} // namespace flub::app::ui
