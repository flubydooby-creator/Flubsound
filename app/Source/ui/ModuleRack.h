// Flubsound Pro - the module rack: one ModuleCard per processing module, in
// a horizontally scrolling Viewport.
//
// Order (docs/11 E39): by relevance to the strip's mode, so the cards a
// listener reaches for first are the ones visible at a small window size:
//   Music   EQ, Bass, Clarity, Stereo & Space, Saturation, Compressor,
//           Maximizer, Dynamic EQ
//   Gaming  Dynamic EQ (the footstep bands), Clarity, Stereo & Space,
//           Compressor, Bass, Maximizer, EQ, Saturation
// The Headphone Virtualizer leads on a 5.1 / 7.1 strip in Gaming mode and
// comes second there in Music mode; on a stereo strip (where it has nothing
// to render) it is the last card. The Noise Gate card is shown only in the
// Quality latency profile, the only one whose chain runs the gate.
// relevanceOrder() is the pure table (tested).
//
// Expanding a card switches the rack into a focused view: only that card is
// shown, at the full rack size, with every parameter of the module
// (MainComponent then gives the rack more height). Escape or the collapse
// button returns to the row of cards.
//
// All cards share one ParameterBinder bound to the selected strip's store;
// updateFromEngine() feeds them the post-macro "effective" module states and
// their readings (the Smoothness cut, the named maximizer style, the Tube
// colour Warmth chose: docs/11 E07 / E05 / E14). The
// cards' ears (hold-to-bypass A/B) go to EngineController::setAuditionBypass,
// loudness matched by a ListenMatch (docs/11 E37) once the owner has given
// the rack an estimator (setEstimatorProvider).
#pragma once

#include "Comparison.h"
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

    /** Module ids (ModuleDescriptor::id) in the order the rack shows them
        for `mode`, on a strip with `stripChannels` channels, in the latency
        profile `quality` or not (the gate is left out outside Quality). */
    static std::vector<juce::String> relevanceOrder (flub::param::ModeValue mode, int stripChannels, bool quality);
    /** The visible cards, left to right (tests). */
    std::vector<ModuleCard*> getShownCards() const;

    /** The loudness estimator the ear's match uses (shared with the header). */
    void setEstimatorProvider (EstimatorProvider provider) { estimatorProvider = std::move (provider); }
    ListenMatch& getListenMatch() noexcept { return listenMatch; }

    /** Scrolls the row so the card with ModuleDescriptor::id `id` starts at
        the left edge (as far as the row allows; screenshots). */
    void scrollToCard (const juce::String& id);

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
    std::vector<ModuleCard*> order; // the shown cards, in relevance order
    ModuleCard* expandedCard = nullptr;
    EstimatorProvider estimatorProvider;
    ListenMatch listenMatch;
    int listenStrip = 0; // strip of the current ear holds (a strip switch releases them)
};
} // namespace flub::app::ui
