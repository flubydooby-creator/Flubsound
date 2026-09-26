// Flubsound Pro - the module rack: one ModuleCard per processing module, in
// signal-flow order, in a horizontally scrolling Viewport.
//
// Expanding a card switches the rack into a focused view: only that card is
// shown, at the full rack size, with every parameter of the module
// (MainComponent then gives the rack more height). Escape or the collapse
// button returns to the row of cards.
//
// All cards share one ParameterBinder bound to the selected strip's store;
// updateFromEngine() feeds them the post-macro "effective" module states.
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
class ModuleRack : public juce::Component
{
public:
    explicit ModuleRack (EngineController& controller);
    ~ModuleRack() override;

    /** Card states from the selected strip's store / effective values (~15 Hz). */
    void updateFromEngine();

    bool hasExpandedCard() const noexcept { return expandedCard != nullptr; }
    void collapse();
    std::function<void()> onLayoutModeChanged;

    /** Keeps the EQ card on the band selected in the curve editor. */
    void setSelectedEqBand (int band);
    std::function<void (int band)> onEqBandSelected;

    void paint (juce::Graphics& g) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;

private:
    void expand (ModuleCard& card, bool shouldExpand);

    EngineController& controller;
    ParameterBinder binder;
    juce::Component content; // declared before the viewport that shows it
    juce::Viewport viewport;
    std::vector<std::unique_ptr<ModuleCard>> cards;
    ModuleCard* expandedCard = nullptr;
};
} // namespace flub::app::ui
