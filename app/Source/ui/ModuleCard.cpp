#include "ModuleCard.h"

#include "EqCurveEditor.h"
#include "ParamHints.h"
#include "Theme.h"

#include <algorithm>
#include <cmath>

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
            // A key bound by name (below) that this build's layout lacks is
            // left out: the card shows what the core has, never a dead control.
            d.keys.erase (std::remove_if (d.keys.begin(), d.keys.end(), [] (const Key& k) { return ! k.banded && k.id < 0; }), d.keys.end());
            m.push_back (std::move (d));
        };
        // Keys that land in the same batch as their card (docs/11 E04 step 3,
        // E28) bind by their parameter key and appear once the core has them.
        auto named = [] (const char* key, const char* label) { return Key { findByKey (key), label }; };

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
        // Presence Mode (clarity.presenceMode, docs/11 E07 step 3) and the
        // per-band attack offsets (clarity.attackLow / attackHigh, E04 step 3).
        add ("clarity", "Clarity", "Clarity", "Presence, air, de-mud and transient shaping; Smoothness takes back added sibilance", ClarityOn,
             B::None,
             { { ClarityPresence, "Presence" }, { ClarityPresenceMode, "Presence Mode" }, { ClarityAir, "Air" }, { ClarityDeMud, "De-Mud" },
               { ClarityAttackDb, "Attack" }, named ("clarity.attackLow", "Attack Low"), named ("clarity.attackHigh", "Attack High"),
               { SmoothAmount, "Smooth", false, true } });
        // Tape Grit (warmth.tapeGrit, docs/11 E14) chooses what the Music
        // Warmth macro does to this stage, with the saturator off too.
        add ("sat", "Saturation", "Saturation", "Oversampled tape / tube / digital saturation; Tape Grit is the classic Warmth",
             SaturationOn, B::None,
             { { SatType, "Type" }, { SatDriveDb, "Drive" }, { SatMix, "Mix" }, { SatOutputDb, "Output" },
               { WarmthTapeGrit, "Tape Grit", false, true } });
        // Crossfeed Type (spatial.crossfeedType, docs/11 E12 Phase A).
        add ("spatial", "Stereo & Space", "Stereo", "Width, positional focus, space and crossfeed with mono safety", SpatialOn, B::None,
             { { SpatialWidth, "Width" }, { SpatialFocus, "Focus" }, { SpatialSpace, "Space" }, { SpatialCrossfeed, "Crossfeed" },
               { SpatialCrossfeedType, "Crossfeed Type" } });
        // The renderer (virt.renderer: Classic / Enhanced) and its front / back
        // contrast (virt.frontBack), docs/11 E28.
        add ("virt", "Headphone Virtualizer", "Virtualizer", "Binaural rendering of 5.1 / 7.1 game audio", VirtualizerOn, B::None,
             { { VirtRoom, "Room" }, { VirtHeadRadius, "Head" }, { VirtLfeGainDb, "LFE" }, named ("virt.renderer", "Renderer"),
               named ("virt.frontBack", "Front/Back") });
        add ("comp", "Compressor", "Compressor", "Look-ahead downward + upward compression; Dynamic Range holds sudden loud events",
             CompressorOn, B::None,
             { { CompThresholdDb, "Threshold" },
               { CompRatio, "Ratio" },
               { CompAttackMs, "Attack" },
               { CompReleaseMs, "Release" },
               { CompMakeupDb, "Makeup" },
               { GuardRange, "Dyn. Range", false, true } });
        // Style (max.style) and LF-first limiting (max.lfLimit), docs/11 E05:
        // a named style sets the clipper and the release (published as
        // effective values), so those two knobs dim under it.
        add ("max", "Loudness Maximizer", "Maximizer", "Glue, soft clipper and true-peak limiter; a style sets the clipper and release",
             MaximizerOn, B::None,
             { { MaxDriveDb, "Drive" },
               { MaxCeilingDb, "Ceiling" },
               { MaxStyle, "Style" },
               { MaxClipAmount, "Clipper", false, false, true },
               { MaxGlue, "Glue" },
               { MaxReleaseMs, "Release", false, false, true },
               { MaxLfLimit, "LF Limit" } });
        return m;
    }();
    return modules;
}

WarmthColour WarmthColour::of (const std::function<float (int)>& base, const std::function<float (int)>& effective)
{
    WarmthColour w;
    if (static_cast<int> (std::lround (base (Mode))) != static_cast<int> (ModeValue::Music) || base (Macro5) <= 0.0f)
        return w;
    if (base (WarmthTapeGrit) >= 0.5f)
    {
        w.kind = Kind::TapeGrit;
        w.pill = "TAPE";
        w.detail = "Tape Grit is on: Warmth is the classic tape saturation with extra bass and harmonics, not the tone tilt.";
        return w;
    }
    // MacroMap's override row: Tube while the saturator is Warmth's alone.
    constexpr long kTube = 1; // sat.type: Tape, Tube, Digital
    const auto& type = layout()[static_cast<size_t> (SatType)];
    if (base (SaturationOn) < 0.5f && base (SatType) == type.defaultValue && std::lround (effective (SatType)) == kTube)
    {
        w.kind = Kind::Tube;
        w.pill = "TUBE";
        w.detail = "Warmth chose the Tube saturator for a gentle, mostly 2nd-order colour (the saturator is Warmth's alone; "
                   "switch it on and pick a type to choose your own).";
    }
    return w;
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
    Style::describe (power, descriptor.name + " on / off",
                     ParamHints::get (descriptor.enableId, ModeValue::Music) + " Click to switch it on or off (click-free).");
    binder.bindToggle (power, descriptor.enableId);
    addAndMakeVisible (power);

    listenButton.setTooltip ("A/B listen: hold to hear the strip without " + descriptor.name);
    listenButton.setAccentWhenOn (false);
    listenButton.onStateChange = [this]
    {
        // The pointer over the ear: the loudness match can be estimated
        // before the hold starts (docs/11 E37).
        if (listenButton.isOver() && ! listening && onListenHover != nullptr)
            onListenHover();
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
        // The plain-language hint (docs/11 E39); the module cards hold no
        // mode-dependent keys, so the Music text is the one for both modes.
        const auto tip = ParamHints::tooltip (id, ModeValue::Music)
                         + (key.independent ? " (It works with " + descriptor.name + " switched off too.)" : juce::String());
        if (auto* knob = dynamic_cast<ParamKnob*> (c))
        {
            binder.bindSlider (knob->slider, id);
            knob->slider.setTooltip (tip);
        }
        else if (auto* combo = dynamic_cast<juce::ComboBox*> (c))
        {
            binder.bindChoice (*combo, id);
            combo->setTooltip (tip);
        }
        else if (auto* toggle = dynamic_cast<juce::ToggleButton*> (c))
        {
            toggle->setTitle (juce::String (ParamFormat::info (id).name));
            toggle->setTooltip (tip);
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
    // The Smoothness cut repaints the note at 0.1 dB steps.
    const bool changed = newState.baseOn != state.baseOn || newState.effectiveOn != state.effectiveOn
                         || newState.gateInactiveProfile != state.gateInactiveProfile
                         || newState.virtualizerNeedsSurround != state.virtualizerNeedsSurround
                         || std::abs (newState.smoothnessCutDb - state.smoothnessCutDb) >= 0.1f || newState.maxStyle != state.maxStyle
                         || newState.tubeByWarmth != state.tubeByWarmth;
    state = newState;
    if (! changed && hasState)
        return;
    hasState = true;

    // Controls of the module's own parameters dim while it is off; a key
    // that acts on its own (Key::independent) does not; one a named style
    // sets (Key::setByStyle) dims while that style is chosen.
    for (size_t i = 0; i < keyControls.size() && i < descriptor.keys.size(); ++i)
        keyControls[i]->setAlpha (keyDimmed (i) ? 0.42f : 1.0f);
    // The ear works whenever the module is heard, also when only a macro
    // engages it (the engine's audition bypass overrides the macros).
    listenButton.setEnabled (state.effectiveOn);
    repaint();
}

bool ModuleCard::keyDimmed (size_t keyIndex) const
{
    const auto& key = descriptor.keys[keyIndex];
    if (key.setByStyle && state.maxStyle.isNotEmpty())
        return true;
    return ! (state.effectiveOn || key.independent);
}

// =============================================================================
// Ear (A/B listen)
// =============================================================================
void ModuleCard::startListening()
{
    if (onListen == nullptr)
        return;
    listening = true;
    listenedInForeground = juce::Process::isForegroundProcess();
    onListen (true);
    startTimerHz (10);
}

void ModuleCard::stopListening()
{
    stopTimer();
    if (! listening)
        return;
    listening = false;
    if (onListen != nullptr)
        onListen (false);
}

void ModuleCard::timerCallback()
{
    // Safety net for releases the button may never see (the window lost focus
    // mid-hold, the card was hidden): the audition must never stick.
    const bool lostFocus = listenedInForeground && ! juce::Process::isForegroundProcess();
    if (! listenButton.isDown() || ! isShowing() || lostFocus)
        stopListening();
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

float ModuleCard::cellWeight (size_t keyIndex) const
{
    // A choice box needs room for its longest item ("10 LU (Balanced)").
    return keyIndex < keyControls.size() && dynamic_cast<juce::ComboBox*> (keyControls[keyIndex].get()) != nullptr ? 1.6f : 1.0f;
}

int ModuleCard::getPreferredWidth() const
{
    float cells = 0.0f;
    for (size_t i = 0; i < descriptor.keys.size(); ++i)
        cells += cellWeight (i);
    return juce::jmax (220, juce::roundToInt (cells * 72.0f) + 28);
}

juce::String ModuleCard::noteText() const
{
    if (descriptor.id == "gate" && state.gateInactiveProfile)
        return "Active only in the Quality latency profile";
    if (descriptor.id == "virt" && state.virtualizerNeedsSurround)
        return "Active for 5.1 / 7.1 sources (Game strip)";
    // What the module does now, where it has a reading (docs/11 E07 / E05 / E14).
    if (descriptor.id == "clarity" && state.smoothnessCutDb <= -0.05f)
        return "Smoothness cut " + Theme::formatSignedDb (state.smoothnessCutDb, 1) + " dB (5 - 10 kHz)";
    if (descriptor.id == "max" && state.maxStyle.isNotEmpty())
        return "Style " + state.maxStyle + " sets Clipper and Release";
    if (descriptor.id == "sat" && state.tubeByWarmth)
        return "Tube chosen by Warmth";
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
    float weights = 0.0f;
    for (size_t i = 0; i < keyControls.size(); ++i)
        weights += cellWeight (i);
    const float unitW = static_cast<float> (cells.getWidth()) / weights;
    const int knobH = juce::jmin (cells.getHeight(), 96);
    for (int i = 0; i < n; ++i)
    {
        const auto k = static_cast<size_t> (i);
        const int cellW = i + 1 == n ? cells.getWidth() : juce::roundToInt (unitW * cellWeight (k));
        auto cell = cells.removeFromLeft (cellW);
        auto* c = keyControls[k].get();
        if (dynamic_cast<ParamKnob*> (c) != nullptr)
            c->setBounds (cell.withSizeKeepingCentre (juce::jmin (cellW, 76), knobH));
        else if (dynamic_cast<juce::ComboBox*> (c) != nullptr)
            c->setBounds (cell.withSizeKeepingCentre (juce::jmin (cellW - 6, 128), 24).translated (0, 2));
        else
            c->setBounds (cell.withSizeKeepingCentre (juce::jmin (cellW - 6, 84), 24).translated (0, 2));
    }
}
} // namespace flub::app::ui
