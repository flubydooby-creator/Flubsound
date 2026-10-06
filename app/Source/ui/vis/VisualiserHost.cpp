#include "VisualiserHost.h"

#include "VisualiserRegistry.h"

namespace flub::app::ui::vis
{
VisualiserHost::VisualiserHost()
{
    views.resize (registry().size());
}

Visualiser* VisualiserHost::get (const juce::String& id)
{
    const int i = indexOf (id);
    if (i < 0)
        return nullptr;
    auto& slot = views[static_cast<size_t> (i)];
    if (slot == nullptr)
    {
        const auto& d = registry()[static_cast<size_t> (i)];
        slot = d.create();
        slot->setTitle (juce::String (d.menuName));
        slot->setDescription (juce::String (d.description));
        if (auto* tip = dynamic_cast<juce::SettableTooltipClient*> (slot.get()); tip != nullptr && tip->getTooltip().isEmpty())
            tip->setTooltip (juce::String (d.description));
        slot->setSampleRate (sampleRate);
    }
    return slot.get();
}

Visualiser* VisualiserHost::findCreated (const juce::String& id) const noexcept
{
    const int i = indexOf (id);
    return i < 0 ? nullptr : views[static_cast<size_t> (i)].get();
}

int VisualiserHost::getNumCreated() const noexcept
{
    int n = 0;
    for (const auto& v : views)
        n += v != nullptr ? 1 : 0;
    return n;
}

void VisualiserHost::setSelected (const juce::String& mainId, const juce::String& stripId)
{
    mainIndex = indexOf (mainId);
    stripIndex = indexOf (stripId);
}

bool VisualiserHost::fed (size_t index) const noexcept
{
    const auto& v = views[index];
    if (v == nullptr)
        return false;
    const int i = static_cast<int> (index);
    return i == mainIndex || i == stripIndex || v->keepsHistory();
}

bool VisualiserHost::isFed (const juce::String& id) const noexcept
{
    const int i = indexOf (id);
    return i >= 0 && fed (static_cast<size_t> (i));
}

void VisualiserHost::setExtra (Visualiser* view)
{
    extra = view;
    if (extra != nullptr)
        extra->setSampleRate (sampleRate);
}

void VisualiserHost::setSampleRate (double newSampleRate)
{
    if (newSampleRate <= 0.0 || newSampleRate == sampleRate)
        return;
    sampleRate = newSampleRate;
    for (auto& v : views)
        if (v != nullptr)
            v->setSampleRate (sampleRate);
    if (extra != nullptr)
        extra->setSampleRate (sampleRate);
}

void VisualiserHost::reset()
{
    for (auto& v : views)
        if (v != nullptr)
            v->reset();
    if (extra != nullptr)
        extra->reset();
}

void VisualiserHost::pushPre (const float* mid, int numSamples)
{
    for (size_t i = 0; i < views.size(); ++i)
        if (fed (i))
            views[i]->pushPre (mid, numSamples);
    if (extra != nullptr)
        extra->pushPre (mid, numSamples);
}

void VisualiserHost::pushPost (const float* mid, const float* side, int numSamples)
{
    for (size_t i = 0; i < views.size(); ++i)
        if (fed (i))
            views[i]->pushPost (mid, side, numSamples);
    if (extra != nullptr)
        extra->pushPost (mid, side, numSamples);
}

void VisualiserHost::advance (const FrameContext& frame)
{
    for (size_t i = 0; i < views.size(); ++i)
        if (fed (i))
            views[i]->advance (frame);
    if (extra != nullptr)
        extra->advance (frame);
}
} // namespace flub::app::ui::vis
