// Flubsound Pro - owns the analyser panel's visualiser views and feeds them.
//
// Views are created from the registry on first use (nothing exists until a
// view is chosen) and then kept, so a view with a history keeps it across
// switches. Each frame's data goes to the views that are selected (the
// panel's main view and strip) and to the created views whose
// keepsHistory() is true; the rest cost nothing. Message thread only.
#pragma once

#include "Visualiser.h"

#include <memory>
#include <vector>

namespace flub::app::ui::vis
{
class VisualiserHost
{
public:
    VisualiserHost();

    /** The view with this id, created on first use (nullptr for an unknown id).
        A new view gets the current sample rate and the registry's title,
        description and tooltip. */
    Visualiser* get (const juce::String& id);
    /** The view if it has been created, else nullptr (never creates). */
    Visualiser* findCreated (const juce::String& id) const noexcept;
    /** Views created so far. */
    int getNumCreated() const noexcept;

    /** The views that are on screen: get fed and advanced every frame ("" = none). */
    void setSelected (const juce::String& mainId, const juce::String& stripId);
    /** True if the view with this id receives this frame's data. */
    bool isFed (const juce::String& id) const noexcept;

    void setSampleRate (double sampleRate);
    void reset();
    void pushPre (const float* mid, int numSamples);
    void pushPost (const float* mid, const float* side, int numSamples);
    void advance (const FrameContext& frame);

private:
    bool fed (size_t index) const noexcept;

    std::vector<std::unique_ptr<Visualiser>> views; // registry order; null until created
    int mainIndex = -1, stripIndex = -1;
    double sampleRate = 48000.0;
};
} // namespace flub::app::ui::vis
