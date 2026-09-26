#include "ModuleCard.h"

#include "EqCurveEditor.h"
#include "Theme.h"

namespace flub::app::ui
{
using namespace flub::param;

// =============================================================================
// Descriptors
// =============================================================================
const std::vector<ModuleDescriptor>& ModuleDescriptor::all()
{
    using B = ModuleDescriptor::Banding;
    static const std::vector<ModuleDescriptor> modules = [] {
        std::vector<ModuleDescriptor> m;
        auto add = [&m] (const char* moduleId, const char* moduleName, const char* layoutGroup, const char* description, int enableParam,
                         B bandingKind, std::vector<Key> keyControls)
        {
            ModuleDescriptor d;
            d.id = moduleId;
            d.name = moduleName;
            d.group = layoutGroup;
            d.blurb = description;
            d.enableId = enableParam;
            d.banding = bandingKind;
            d.keys = std::move (keyControls);
            m.push_back (std::move (d));
        };

        add ("gate", "Noise Gate", "Noise Gate", "Spectral noise gate: removes hiss and hum between sounds", GateOn, B::None,
             { { GateThresholdDb, "Threshold" }, { GateReductionDb, "Reduction" }, { GateReleaseMs, "Release" } });
        add ("eq", "Parametric EQ", "EQ", "10-band parametric EQ", EqOn, B::Eq,
             { { EqFieldFreq, "Freq", true }, { EqFieldGain, "Gain", true }, { EqFieldQ, "Q", true }, { EqOutputGainDb, "Output" } });
        add ("dyneq", "Dynamic EQ", "Dynamic EQ", "Frequency-selective compression / expansion", DynEqOn, B::Dynamic,
             { { DynFieldOn, "Band", true },
               { DynFieldFreq, "Freq", true },
               { DynFieldThreshold, "Threshold", true },
               { DynFieldRange, "Range", true },
               { DynFieldRatio, "Ratio", true } });
        add ("bass", "Bass Engine", "Bass", "Bass boost, psychoacoustic harmonics and headroom protection", BassOn, B::None,
             { { BassBoostDb, "Boost" }, { BassBoostFreq, "Freq" }, { BassHarmonics, "Harmonics" }, { BassTighten, "Tighten" }, { BassProtectDb, "Protect" } });
        add ("clarity", "Clarity", "Clarity", "Presence, air, de-mud and transient shaping", ClarityOn, B::None,
             { { ClarityPresence, "Presence" }, { ClarityAir, "Air" }, { ClarityDeMud, "De-Mud" }, { ClarityAttackDb, "Attack" } });
        add ("sat", "Saturation", "Saturation", "Oversampled tape / tube / digital saturation", SaturationOn, B::None,
             { { SatType, "Type" }, { SatDriveDb, "Drive" }, { SatMix, "Mix" }, { SatOutputDb, "Output" } });
        add ("spatial", "Stereo & Space", "Stereo", "Width, positional focus, space and crossfeed with mono safety", SpatialOn, B::None,
             { { SpatialWidth, "Width" }, { SpatialFocus, "Focus" }, { SpatialSpace, "Space" }, { SpatialCrossfeed, "Crossfeed" } });
        add ("virt", "Headphone Virtualizer", "Virtualizer", "Binaural rendering of 5.1 / 7.1 game audio", VirtualizerOn, B::None,
             { { VirtRoom, "Room" }, { VirtHeadRadius, "Head" }, { VirtLfeGainDb, "LFE" } });
        add ("comp", "Compressor", "Compressor", "Look-ahead downward + upward compression", CompressorOn, B::None,
             { { CompThresholdDb, "Threshold" }, { CompRatio, "Ratio" }, { CompAttackMs, "Attack" }, { CompReleaseMs, "Release" }, { CompMakeupDb, "Makeup" } });
        add ("max", "Loudness Maximizer", "Maximizer", "Glue, soft clipper and true-peak limiter", MaximizerOn, B::None,
             { { MaxDriveDb, "Drive" }, { MaxCeilingDb, "Ceiling" }, { MaxClipAmount, "Clipper" }, { MaxGlue, "Glue" }, { MaxReleaseMs, "Release" } });
        return m;
    }();
    return modules;
}

// =============================================================================
// ModuleCard
// =============================================================================
ModuleCard::ModuleCard (const ModuleDescriptor& d, ParameterBinder& b)
    : descriptor (d), binder (b)
{
    setTitle (descriptor.name);
    setDescription (descriptor.blurb);

    Style::set (power, "power");
    Style::describe (power, descriptor.name + " on / off", "Switch " + descriptor.name + " on or off (click-free)");
    binder.bindToggle (power, descriptor.enableId);
    addAndMakeVisible (power);

    listenButton.setTooltip ("A/B listen: hold to hear the strip without " + descriptor.name);
    listenButton.setAccentWhenOn (false);
    listenButton.onStateChange = [this]
    {
        if (listenButton.isDown() && ! listening)
            startListening();
        else if (! listenButton.isDown() && listening)
            stopListening();
    };
    addAndMakeVisible (listenButton);

    expandButton.setTooltip ("Show all " + descriptor.name + " parameters");
    expandButton.onClick = [this]
    {
        if (onExpandRequested != nullptr)
            onExpandRequested (*this, ! expanded);
    };
    addAndMakeVisible (expandButton);

    if (descriptor.banding != ModuleDescriptor::Banding::None)
    {
        prevBand.onClick = [this] { setBand ((band + numBands() - 1) % numBands()); if (onBandChanged != nullptr) onBandChanged (band); };
        nextBand.onClick = [this] { setBand ((band + 1) % numBands()); if (onBandChanged != nullptr) onBandChanged (band); };
        addAndMakeVisible (prevBand);
        addAndMakeVisible (nextBand);
    }

    addAndMakeVisible (keyHolder);
    keyHolder.setInterceptsMouseClicks (false, true);
    for (const auto& key : descriptor.keys)
    {
        const int id = resolve (key);
        const auto& info = ParamFormat::info (id);
        if (info.unit == Unit::Choice)
        {
            auto combo = std::make_unique<juce::ComboBox>();
            keyHolder.addAndMakeVisible (*combo);
            keyControls.push_back (std::move (combo));
        }
        else if (info.unit == Unit::Toggle)
        {
            auto toggle = std::make_unique<juce::ToggleButton> ("On");
            Style::set (*toggle, "switch");
            keyHolder.addAndMakeVisible (*toggle);
            keyControls.push_back (std::move (toggle));
        }
        else
        {
            auto knob = std::make_unique<ParamKnob> (key.label, ParamKnob::Size::Medium);
            keyHolder.addAndMakeVisible (*knob);
            keyControls.push_back (std::move (knob));
        }
    }
    bindKeys();
}

ModuleCard::~ModuleCard()
{
    if (listening)
        stopListening();
    gridView.reset();
    binder.unbind (power);
    for (auto& c : keyControls)
    {
        if (auto* knob = dynamic_cast<ParamKnob*> (c.get()))
            binder.unbind (knob->slider);
        else
            binder.unbind (*c);
    }
}

int ModuleCard::numBands() const noexcept
{
    return descriptor.banding == ModuleDescriptor::Banding::Eq ? kEqBands : kDynEqBands;
}

int ModuleCard::resolve (const ModuleDescriptor::Key& key) const noexcept
{
    if (! key.banded)
        return key.id;
    if (descriptor.banding == ModuleDescriptor::Banding::Eq)
        return eq (band, static_cast<EqField> (key.id));
    return dyn (band, static_cast<DynField> (key.id));
}

void ModuleCard::bindKeys()
{
    for (size_t i = 0; i < keyControls.size(); ++i)
    {
        const auto& key = descriptor.keys[i];
        const int id = resolve (key);
        auto* c = keyControls[i].get();
        if (auto* knob = dynamic_cast<ParamKnob*> (c))
        {
            binder.bindSlider (knob->slider, id);
            knob->slider.setTooltip (juce::String (ParamFormat::info (id).name));
        }
        else if (auto* combo = dynamic_cast<juce::ComboBox*> (c))
        {
            binder.bindChoice (*combo, id);
            combo->setTooltip (juce::String (ParamFormat::info (id).name));
        }
        else if (auto* toggle = dynamic_cast<juce::ToggleButton*> (c))
        {
            toggle->setTitle (juce::String (ParamFormat::info (id).name));
            binder.bindToggle (*toggle, id);
        }
    }
}

void ModuleCard::setBand (int newBand)
{
    if (descriptor.banding == ModuleDescriptor::Banding::None)
        return;
    newBand = juce::jlimit (0, numBands() - 1, newBand);
    if (newBand == band)
        return;
    band = newBand;
    bindKeys();
    repaint();
}

void ModuleCard::setState (const State& newState)
{
    const bool changed = newState.baseOn != state.baseOn || newState.effectiveOn != state.effectiveOn
                         || newState.gateInactiveProfile != state.gateInactiveProfile
                         || newState.virtualizerNeedsSurround != state.virtualizerNeedsSurround;
    state = newState;
    if (! changed && hasState)
        return;
    hasState = true;

    keyHolder.setAlpha (state.effectiveOn ? 1.0f : 0.42f);
    const bool macroOnly = state.effectiveOn && ! state.baseOn;
    listenButton.setEnabled (state.effectiveOn && ! macroOnly);
    listenButton.setTooltip (macroOnly ? descriptor.name + " is engaged by Boost Intensity / a macro, so it cannot be bypassed here"
                                       : "A/B listen: hold to hear the strip without " + descriptor.name);
    repaint();
}

void ModuleCard::startListening()
{
    if (auto* store = binder.getStore())
    {
        listenRestore = store->get (descriptor.enableId);
        store->set (descriptor.enableId, 0.0f);
        listening = true;
    }
}

void ModuleCard::stopListening()
{
    listening = false;
    if (auto* store = binder.getStore())
        store->set (descriptor.enableId, listenRestore);
}

void ModuleCard::setExpanded (bool shouldExpand)
{
    if (shouldExpand == expanded)
        return;
    expanded = shouldExpand;
    expandButton.setIcon (expanded ? Icons::collapse() : Icons::expand());
    expandButton.setTooltip (expanded ? juce::String ("Back to the module rack") : "Show all " + descriptor.name + " parameters");

    if (expanded)
    {
        gridView = std::make_unique<juce::Viewport>();
        gridView->setScrollBarsShown (true, false);
        auto* g = new ParamGrid (binder, ParamGrid::idsForGroup (descriptor.group));
        grid = g;
        gridView->setViewedComponent (g, true);
        addAndMakeVisible (*gridView);
    }
    else
    {
        gridView.reset();
        grid = nullptr;
    }
    keyHolder.setVisible (! expanded);
    prevBand.setVisible (! expanded && descriptor.banding != ModuleDescriptor::Banding::None);
    nextBand.setVisible (! expanded && descriptor.banding != ModuleDescriptor::Banding::None);
    resized();
    repaint();
}

int ModuleCard::getPreferredWidth() const
{
    return juce::jmax (220, static_cast<int> (descriptor.keys.size()) * 72 + 28);
}

juce::String ModuleCard::noteText() const
{
    if (descriptor.id == "gate" && state.gateInactiveProfile)
        return "Active only in the Quality latency profile";
    if (descriptor.id == "virt" && state.virtualizerNeedsSurround)
        return "Active for 5.1 / 7.1 sources (Game strip)";
    if (state.effectiveOn && ! state.baseOn)
        return "Engaged by Boost Intensity / macros";
    if (descriptor.id == "eq")
        return "Drag the nodes in the analyser, wheel = Q";
    if (descriptor.id == "dyneq")
        return "Live gain: amber markers in the analyser";
    return "Hold the ear to A/B this module";
}

// =============================================================================
void ModuleCard::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto accent = Theme::accent (*this);
    Theme::drawPanel (g, bounds, 8.0f);
    if (expanded)
    {
        g.setColour (accent.withAlpha (0.35f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 8.0f, 1.0f);
    }

    // Title (+ AUTO chip)
    auto title = titleArea.toFloat();
    g.setColour (state.effectiveOn ? Palette::text : Palette::muted);
    g.setFont (Theme::font (13.0f, true));
    const float tw = juce::jmin (title.getWidth(), juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), descriptor.name) + 2.0f);
    g.drawFittedText (descriptor.name, title.removeFromLeft (tw).toNearestInt(), juce::Justification::centredLeft, 1, 0.85f);
    if (state.effectiveOn && ! state.baseOn && title.getWidth() > 40.0f)
    {
        title.removeFromLeft (6.0f);
        Theme::drawPill (g, title.removeFromLeft (38.0f).withSizeKeepingCentre (38.0f, 16.0f), "AUTO", accent, true);
    }

    // Band selector
    if (! expanded && descriptor.banding != ModuleDescriptor::Banding::None && ! bandArea.isEmpty())
    {
        auto r = bandArea.toFloat();
        const bool isEq = descriptor.banding == ModuleDescriptor::Banding::Eq;
        auto dot = r.removeFromLeft (10.0f).withSizeKeepingCentre (7.0f, 7.0f);
        g.setColour (isEq ? EqCurveEditor::bandColour (band) : Palette::dynamicEq);
        g.fillEllipse (dot);
        r.removeFromLeft (3.0f);
        g.setColour (Palette::text.withAlpha (0.9f));
        g.setFont (Theme::font (12.0f));
        g.drawText ((isEq ? "Band " : "Dyn ") + juce::String (band + 1), r, juce::Justification::centredLeft, false);
    }

    // Note
    if (! expanded && ! noteArea.isEmpty())
    {
        const bool warn = (descriptor.id == "gate" && state.gateInactiveProfile) || (descriptor.id == "virt" && state.virtualizerNeedsSurround);
        g.setColour (warn ? Palette::amber.withAlpha (0.9f) : Palette::faint);
        g.setFont (Theme::font (10.5f));
        g.drawFittedText (noteText(), noteArea, juce::Justification::centred, 1, 0.8f);
    }

    // Dynamic EQ band toggle caption
    for (size_t i = 0; i < keyControls.size() && ! expanded; ++i)
    {
        if (auto* toggle = dynamic_cast<juce::ToggleButton*> (keyControls[i].get()))
        {
            auto r = toggle->getBounds().translated (keyHolder.getX(), keyHolder.getY() - 20).withHeight (16);
            g.setColour (Palette::muted);
            g.setFont (Theme::font (11.5f));
            g.drawText (descriptor.keys[i].label, r, juce::Justification::centred, false);
        }
        else if (auto* combo = dynamic_cast<juce::ComboBox*> (keyControls[i].get()))
        {
            auto r = combo->getBounds().translated (keyHolder.getX(), keyHolder.getY() - 20).withHeight (16);
            g.setColour (Palette::muted);
            g.setFont (Theme::font (11.5f));
            g.drawText (descriptor.keys[i].label, r, juce::Justification::centred, false);
        }
    }
}

void ModuleCard::resized()
{
    auto r = getLocalBounds().reduced (10, 8);
    headerArea = r.removeFromTop (26);
    {
        auto h = headerArea;
        power.setBounds (h.removeFromLeft (24).withSizeKeepingCentre (24, 24));
        h.removeFromLeft (8);
        expandButton.setBounds (h.removeFromRight (24).withSizeKeepingCentre (24, 24));
        h.removeFromRight (2);
        listenButton.setBounds (h.removeFromRight (24).withSizeKeepingCentre (24, 24));
        h.removeFromRight (4);
        if (descriptor.banding != ModuleDescriptor::Banding::None && ! expanded)
        {
            auto sel = h.removeFromRight (100);
            nextBand.setBounds (sel.removeFromRight (20).withSizeKeepingCentre (20, 22));
            prevBand.setBounds (sel.removeFromLeft (20).withSizeKeepingCentre (20, 22));
            bandArea = sel.reduced (4, 0);
            h.removeFromRight (4);
        }
        else
        {
            bandArea = {};
        }
        titleArea = h;
    }

    if (expanded)
    {
        r.removeFromTop (6);
        noteArea = {};
        if (gridView != nullptr)
        {
            gridView->setBounds (r);
            if (grid != nullptr)
            {
                const int w = r.getWidth() - gridView->getScrollBarThickness() - 4;
                grid->setSize (w, grid->getHeightForWidth (w));
            }
        }
        return;
    }

    noteArea = r.removeFromBottom (16);
    r.removeFromBottom (2);
    keyHolder.setBounds (r);

    // Key controls: evenly spaced cells.
    const int n = static_cast<int> (keyControls.size());
    if (n == 0)
        return;
    auto cells = keyHolder.getLocalBounds();
    const int cellW = cells.getWidth() / n;
    const int knobH = juce::jmin (cells.getHeight(), 96);
    for (int i = 0; i < n; ++i)
    {
        auto cell = cells.removeFromLeft (cellW);
        auto* c = keyControls[static_cast<size_t> (i)].get();
        if (dynamic_cast<ParamKnob*> (c) != nullptr)
            c->setBounds (cell.withSizeKeepingCentre (juce::jmin (cellW, 76), knobH));
        else
            c->setBounds (cell.withSizeKeepingCentre (juce::jmin (cellW - 6, 84), 24).translated (0, 2));
    }
}
} // namespace flub::app::ui
