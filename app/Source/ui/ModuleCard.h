// Flubsound Pro - one processing module in the rack.
//
//   [power]  Module name   [AUTO]          [ear] [expand]
//     knob   knob   knob   knob            (3-5 key controls)
//   note / hint line
//
// * power      the module's enable parameter (bypass is a click-free,
//              latency-compensated crossfade in the engine)
// * AUTO chip  the module is off in the preset but engaged by Boost
//              Intensity / a mode macro (post-macro effective value)
// * ear        hold to hear the strip without this module ("A/B listen"):
//              an audition bypass in the engine (onListen), not a parameter
//              write, so it also works for macro-engaged modules and never
//              marks the preset modified; loudness matched by the rack
//              (ListenMatch, docs/11 E37)
// * expand     shows ALL parameters of the module's layout group
//              (auto-generated ParamGrid in a scrolling view)
// The card dims while the module is effectively bypassed. Banded modules
// (Parametric EQ, Dynamic EQ) edit one band at a time; the EQ card follows
// the band selected in the analyser's curve editor.
#pragma once

#include "ParamGrid.h"
#include "ParamKnob.h"
#include "ParameterBinding.h"
#include "Widgets.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace flub::app::ui
{
struct ModuleDescriptor
{
    enum class Banding
    {
        None,
        Eq,     // keys are EqField values of the selected EQ band
        Dynamic // keys are DynField values of the selected dynamic band
    };

    struct Key
    {
        int id = -1; // parameter id, or a band field for banded modules
        juce::String label;
        bool banded = false;
        /** Acts whether the module is on or not (its own stage: Smoothness on
            the Clarity card, Dynamic Range on the Compressor card), so the
            control is not dimmed with the module. */
        bool independent = false;
    };

    juce::String id, name, group, blurb;
    int enableId = -1;
    Banding banding = Banding::None;
    std::vector<Key> keys;

    /** The ten modules: the chain's processing order, except that the
        Headphone Virtualizer (which runs first, before the gate) sits with
        the stereo modules. The rack shows them by relevance
        (ModuleRack::relevanceOrder, docs/11 E39). */
    static const std::vector<ModuleDescriptor>& all();
};

class ModuleCard : public juce::Component, private juce::Timer
{
public:
    ModuleCard (const ModuleDescriptor& descriptor, ParameterBinder& binder);
    ~ModuleCard() override;

    const ModuleDescriptor& getDescriptor() const noexcept { return descriptor; }

    /** Engine state for the card (cheap; ~15 Hz). */
    struct State
    {
        bool baseOn = false, effectiveOn = false;
        bool gateInactiveProfile = false;   // Noise Gate: latency profile is not "Quality"
        bool virtualizerNeedsSurround = false;
    };
    void setState (const State& state);

    void setExpanded (bool shouldExpand);
    bool isExpanded() const noexcept { return expanded; }
    std::function<void (ModuleCard&, bool expand)> onExpandRequested;

    /** Banded modules: which band the key controls edit. */
    void setBand (int band);
    int getBand() const noexcept { return band; }
    std::function<void (int band)> onBandChanged;

    /** Ear (A/B listen): called with true when a hold starts and with false
        when it ends - on mouse-up, focus loss, when the card is hidden or
        destroyed, or on stopListening(). Every start gets exactly one end. */
    std::function<void (bool listen)> onListen;
    /** The pointer is over the ear (a hold may follow). */
    std::function<void()> onListenHover;
    /** Ends a hold now (strip switch, engine reconfiguration). */
    void stopListening();

    /** Preferred width in the collapsed rack. */
    int getPreferredWidth() const;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    int numBands() const noexcept;
    int resolve (const ModuleDescriptor::Key& key) const noexcept;
    /** Relative width of key `keyIndex`'s cell (a choice box is wider). */
    float cellWeight (size_t keyIndex) const;
    void bindKeys();
    void startListening();
    void timerCallback() override;
    juce::String noteText() const;

    ModuleDescriptor descriptor;
    ParameterBinder& binder;

    juce::ToggleButton power;
    IconButton listenButton { "Hold to hear without this module", Icons::ear() };
    IconButton expandButton { "Show all parameters", Icons::expand() };
    IconButton prevBand { "Previous band", Icons::chevronLeft() }, nextBand { "Next band", Icons::chevronRight() };
    std::vector<std::unique_ptr<juce::Component>> keyControls; // ParamKnob / ComboBox / ToggleButton per key
    juce::Component keyHolder;

    std::unique_ptr<juce::Viewport> gridView;
    ParamGrid* grid = nullptr;

    State state;
    bool expanded = false, listening = false, listenedInForeground = false, hasState = false;
    int band = 0;
    juce::Rectangle<int> headerArea, titleArea, bandArea, noteArea;
};
} // namespace flub::app::ui
