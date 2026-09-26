// Flubsound Pro - the module rack: one ModuleCard per processing module, in
// a horizontally scrolling Viewport. The cards follow the chain's processing
// order, except that the Headphone Virtualizer (which the chain runs first,
// before the gate) sits with the stereo modules.
//
// Expanding a card switches the rack into a focused view: only that card is
// shown, at the full rack size, with every parameter of the module
// (MainComponent then gives the rack more height). Escape or the collapse
// button returns to the row of cards.
//
// All cards share one ParameterBinder bound to the selected strip's store;
// updateFromEngine() feeds them the post-macro "effective" module states. The
// cards' ears (hold-to-bypass A/B) go to EngineController::setAuditionBypass.
#pragma once

#include "ModuleCard.h"
#include "ParameterBinding.h"
#include "engine/EngineController.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace flub::app::ui
{
class ModuleRack : public juce::Component, private juce::ScrollBar::Listener
{
public:
    explicit ModuleRack (EngineController& controller);
    ~ModuleRack() override;

    /** Card states from the selected strip's store / effective values (~15 Hz). */
    void updateFromEngine();

    /** Ends any ear hold (A/B listen); call on strip switches and engine
        reconfigurations. */
    void releaseListening();

    bool hasExpandedCard() const noexcept { return expandedCard != nullptr; }
    void collapse();
    std::function<void()> onLayoutModeChanged;

    /** Keeps the EQ card on the band selected in the curve editor. */
    void setSelectedEqBand (int band);
    std::function<void (int band)> onEqBandSelected;

    void paint (juce::Graphics& g) override;
    void paintOverChildren (juce::Graphics& g) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;

private:
    void expand (ModuleCard& card, bool shouldExpand);
    void scrollBarMoved (juce::ScrollBar*, double) override { repaint(); }

    EngineController& controller;
    ParameterBinder binder;
    juce::Component content; // declared before the viewport that shows it
    juce::Viewport viewport;
    std::vector<std::unique_ptr<ModuleCard>> cards;
    ModuleCard* expandedCard = nullptr;
    int listenStrip = 0; // strip of the current ear holds (a strip switch releases them)
};
} // namespace flub::app::ui
