#include "BoostPanel.h"

#include "FlubLookAndFeel.h"
#include "ModuleCard.h"
#include "ParamHints.h"
#include "Theme.h"

#include <algorithm>
#include <cmath>

namespace flub::app::ui
{
using namespace flub::param;

namespace
{
const juce::String kDot { juce::CharPointer_UTF8 (" \xc2\xb7 ") };

juce::String percent (float v01)
{
    return juce::String (juce::roundToInt (v01 * 100.0f)) + "%";
}
} // namespace

// =============================================================================
// Protection readouts (docs/11 E06 / E07)
// =============================================================================
// The budget is quoted before the first reading too: it is the mode's and
// the strength's from the chain's first block (docs/11 E06).
juce::String BoostPanel::describeProtectionLevel (float levelDb, float budgetDb)
{
    const auto budget = " (budget " + juce::String (juce::roundToInt (budgetDb)) + " dB)";
    if (! std::isfinite (levelDb) || levelDb <= -150.0f)
        return "not measured yet" + budget;
    return juce::String (juce::roundToInt (levelDb)) + " dB" + budget;
}

juce::String BoostPanel::describePlr (float plrDb, float budgetDb)
{
    juce::String t;
    if (! std::isfinite (plrDb) || plrDb >= flub::MeterBus::governorNoReading * 0.5f)
        t = "not measured yet";
    else
        t = juce::String (plrDb, 1) + " dB";
    if (std::isfinite (budgetDb) && budgetDb > 0.0f)
        t << " (at least " << juce::String (budgetDb, 0) << " dB)";
    else
        t << " (no budget in this mode)";
    return t;
}

juce::String BoostPanel::describeBrightness (const MeterSnapshot& s)
{
    static const char* names[] = { "presence", "harsh", "air" };
    juce::StringArray parts;
    for (size_t b = 0; b < s.tonalLiftDb.size(); ++b)
    {
        const float lift = s.tonalLiftDb[b];
        if (! std::isfinite (lift) || lift <= -150.0f)
            return "not measured yet (budgets " + Theme::formatSignedDb (s.tonalBudgetDb[0], 0) + " / " + Theme::formatSignedDb (s.tonalBudgetDb[1], 0)
                   + " / " + Theme::formatSignedDb (s.tonalBudgetDb[2], 0) + " dB)";
        parts.add (juce::String (names[b]) + " " + Theme::formatSignedDb (lift, 1) + " dB (budget " + Theme::formatSignedDb (s.tonalBudgetDb[b], 0) + ")");
    }
    return parts.joinIntoString (", ");
}

// =============================================================================
// BoostDial
// =============================================================================
BoostDial::BoostDial()
{
    setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    setRotaryParameters (FlubLookAndFeel::kRotaryStart, FlubLookAndFeel::kRotaryEnd, true);
    setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    setMouseDragSensitivity (300);
    setTitle ("Boost Intensity");
    setWantsKeyboardFocus (true);
}

void BoostDial::setGovernorScale (float scale)
{
    scale = juce::jlimit (0.0f, 1.0f, std::isfinite (scale) ? scale : 1.0f);
    if (std::abs (scale - governor) > 0.002f)
    {
        governor = scale;
        repaint();
    }
}

void BoostDial::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const float size = juce::jmin (bounds.getWidth(), bounds.getHeight());
    const auto centre = bounds.getCentre();
    const auto accent = Theme::accent (*this);
    const float start = FlubLookAndFeel::kRotaryStart, end = FlubLookAndFeel::kRotaryEnd;
    const auto value = static_cast<float> (valueToProportionOfLength (getValue()));
    const float valueAngle = start + value * (end - start);

    const float track = juce::jlimit (6.0f, 12.0f, size * 0.075f);
    const float radius = size * 0.5f - track * 0.5f - 4.0f;
    const auto stroke = [] (float w) { return juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded); };
    auto arc = [&] (float from, float to, float r)
    {
        juce::Path p;
        p.addCentredArc (centre.x, centre.y, r, r, 0.0f, from, to, true);
        return p;
    };

    // Tick marks every 10 %.
    for (int i = 0; i <= 10; ++i)
    {
        const float a = start + (end - start) * static_cast<float> (i) / 10.0f;
        const juce::Point<float> dir (std::sin (a), -std::cos (a));
        const float r0 = radius + track * 0.5f + 2.0f, r1 = r0 + (i % 5 == 0 ? 4.0f : 2.5f);
        g.setColour (static_cast<float> (i) / 10.0f <= value + 0.001f ? accent.withAlpha (0.8f) : Palette::borderStrong);
        g.drawLine (juce::Line<float> (centre + dir * r0, centre + dir * r1), 1.2f);
    }

    // Track
    g.setColour (Palette::well);
    g.strokePath (arc (start, end, radius), stroke (track + 2.0f));
    g.setColour (Palette::borderStrong.withAlpha (0.7f));
    g.strokePath (arc (start, end, radius), stroke (track - 2.0f));

    // Value arc with glow + gradient.
    if (value > 0.001f)
    {
        const auto valueArc = arc (start, valueAngle, radius);
        g.setColour (accent.withAlpha (0.12f));
        g.strokePath (valueArc, stroke (track + 10.0f));
        g.setColour (accent.withAlpha (0.2f));
        g.strokePath (valueArc, stroke (track + 4.0f));
        g.setGradientFill (juce::ColourGradient (accent.darker (0.45f), bounds.getX(), bounds.getBottom(), accent.brighter (0.25f), bounds.getRight(),
                                                 bounds.getY(), false));
        g.strokePath (valueArc, stroke (track));
    }

    // Applied (governed) share: thin inner arc.
    const float innerR = radius - track * 0.5f - 6.0f;
    g.setColour (Palette::border);
    g.strokePath (arc (start, end, innerR), stroke (2.0f));
    if (value > 0.001f)
    {
        const float applied = value * governor;
        g.setColour (governor < 0.985f ? Palette::amber : Palette::text.withAlpha (0.55f));
        g.strokePath (arc (start, start + applied * (end - start), innerR), stroke (2.0f));
    }

    // Thumb dot at the value.
    {
        const juce::Point<float> dir (std::sin (valueAngle), -std::cos (valueAngle));
        const auto p = centre + dir * radius;
        g.setColour (Palette::text);
        g.fillEllipse (juce::Rectangle<float> (track * 0.9f, track * 0.9f).withCentre (p));
        g.setColour (Palette::background.withAlpha (0.5f));
        g.drawEllipse (juce::Rectangle<float> (track * 0.9f, track * 0.9f).withCentre (p), 1.0f);
    }

    // Centre readout: value and caption.
    const float textSize = juce::jlimit (22.0f, 46.0f, size * 0.27f);
    auto textArea = juce::Rectangle<float> (size * 0.7f, textSize * 1.1f).withCentre ({ centre.x, centre.y - textSize * 0.15f });
    g.setColour (Palette::text);
    g.setFont (Theme::numeric (textSize));
    g.drawText (juce::String (juce::roundToInt (value * 100.0f)), textArea, juce::Justification::centred, false);
    const float captionSize = juce::jlimit (8.5f, 10.5f, size * 0.065f);
    auto captionArea = textArea.translated (0.0f, textSize * 0.86f).withHeight (captionSize + 3.0f);
    g.setColour (Palette::muted);
    g.setFont (Theme::caption (captionSize));
    g.drawText ("BOOST %", captionArea, juce::Justification::centred, false);


    if (hasKeyboardFocus (false))
    {
        g.setColour (accent.withAlpha (0.5f));
        g.drawEllipse (juce::Rectangle<float> (innerR * 2.0f - 10.0f, innerR * 2.0f - 10.0f).withCentre (centre), 1.0f);
    }
}

// =============================================================================
// BoostPanel
// =============================================================================
BoostPanel::BoostPanel (EngineController& c)
    : controller (c),
      binder ([this] { return &controller.getSelectedParams(); },
              [this] { return &controller.getChain (controller.getSelectedStrip()); })
{
    addAndMakeVisible (dial);
    binder.bindSlider (dial, BoostIntensity);
    dial.setTooltip ("Boost Intensity: one control for the whole enhancement. Clarity and width come first, bass next and "
                     "loudness last, always watched by the safety governor.");

    for (size_t i = 0; i < macros.size(); ++i)
    {
        auto& knob = macros[i];
        knob.setKnobSize (ParamKnob::Size::Large);
        addAndMakeVisible (knob);
        binder.bindSlider (knob.slider, Macro1 + static_cast<int> (i));
    }
    // Dynamic Range and Smoothness (Simple layout; see the header).
    rangeBox.setTitle ("Dynamic Range");
    rangeBox.setTooltip ("Dynamic Range: holds sudden loud sounds - an explosion, gunfire, a loud scene cut - to this much over the "
                         "level just before them, so a quiet scene stays audible and nothing startles. Off by default.");
    binder.bindChoice (rangeBox, GuardRange);
    addChildComponent (rangeBox);
    smoothSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    smoothSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 20);
    smoothSlider.setTitle ("Smoothness");
    smoothSlider.setTooltip ("Smoothness: takes back the sharp \"s\" and harsh top end that Boost and the macros add, never the "
                             "source's own. 0 % (off) by default.");
    binder.bindSlider (smoothSlider, SmoothAmount);
    addChildComponent (smoothSlider);
    // The Warmth chip (Music; see the header): a click switches Tape Grit.
    Style::set (warmthChip, "chip");
    warmthChip.setTitle ("Warmth colour");
    binder.bindToggle (warmthChip, WarmthTapeGrit);
    addChildComponent (warmthChip);
    // The headset enhancement cap's chips (Gaming; see the header).
    for (auto& chip : cappedChips)
    {
        Style::set (chip, "chip");
        chip.setToggleState (true, juce::dontSendNotification); // tinted: a state, not an option
        chip.setTitle ("Capped by the headset enhancement setting");
        chip.setTooltip ("Capped at 30 %: this output's own enhancement (Superhuman Hearing / on-board EQ) is ON, so this macro "
                         "reaches the sound at most at 30 % and Flubsound's virtual surround is off. The knob keeps its value. "
                         "Click to change it (also in Settings > Audio).");
        chip.setClickingTogglesState (false);
        chip.onClick = [this, &chip] { showCappedMenu (chip); };
        addChildComponent (chip);
    }

    governor = describeGovernor ({}, controller.getProtectionStrength());
    setMode (ModeValue::Music);
}

void BoostPanel::setMode (ModeValue newMode)
{
    mode = newMode;
    dial.setTooltip (ParamHints::tooltip (BoostIntensity, mode));
    for (size_t i = 0; i < macros.size(); ++i)
    {
        const auto name = EngineController::getMacroName (mode, static_cast<int> (i));
        macros[i].setLabel (name);
        macros[i].slider.setTitle (name);
        // The plain-language hint of the macro in this mode (docs/11 E39).
        macros[i].slider.setTooltip (ParamHints::get (Macro1 + static_cast<int> (i), mode));
    }
    warmthChip.setVisible (mode == ModeValue::Music);
    refreshWarmth();
    refreshCapped();
    repaint();
}

void BoostPanel::setLayout (Layout newLayout)
{
    if (newLayout == panelLayout)
        return;
    panelLayout = newLayout;
    resized();
    repaint();
}

void BoostPanel::setGovernorScale (float scale)
{
    MeterSnapshot s;
    s.governorScale = scale;
    s.governorState = scale < 0.985f ? static_cast<int> (flub::SafetyGovernor::State::Holding) : 0;
    update (s);
}

void BoostPanel::update (const MeterSnapshot& snapshot)
{
    refreshCapped();
    dial.setGovernorScale (snapshot.governorScale);
    auto next = describeGovernor (snapshot, controller.getProtectionStrength());
    if (next.text != governor.text || next.detail != governor.detail)
    {
        const bool textChanged = next.text != governor.text;
        governor = std::move (next);
        if (textChanged)
            repaint (headerArea);
    }

    // The chips change with parameters and slow meters: a few times a second.
    if (++framesSinceStages >= 15)
    {
        framesSinceStages = 0;
        refreshStages (snapshot);
        refreshWarmth();
    }
}

// =============================================================================
// Warmth chip (docs/11 E14)
// =============================================================================
BoostPanel::WarmthReadout BoostPanel::describeWarmth (const std::function<float (int)>& base, const std::function<float (int)>& effective)
{
    WarmthReadout r;
    const auto colour = WarmthColour::of (base, effective);
    r.tapeGrit = base (WarmthTapeGrit) >= 0.5f;
    constexpr const char* kClick = "\nClick to switch Tape Grit (the classic tape Warmth) on or off for this strip.";
    switch (colour.kind)
    {
        case WarmthColour::Kind::Tube:
            r.text = "TUBE";
            r.detail = colour.detail;
            break;
        case WarmthColour::Kind::TapeGrit:
            r.text = "TAPE";
            r.detail = colour.detail;
            break;
        case WarmthColour::Kind::None:
            r.text = r.tapeGrit ? "TAPE" : "TONE";
            r.detail = r.tapeGrit ? "Tape Grit is on: raise Warmth for the classic tape saturation with extra bass and harmonics."
                                  : "Warmth is a tone control: more body around 200 Hz and a softer top (up to +3.5 / -3 dB at "
                                    "100 %), level matched; with the saturator off it also chooses the gentle Tube colour.";
            if (! r.tapeGrit && base (SaturationOn) >= 0.5f)
                r.detail << " The saturator is on, so it keeps the type you chose.";
            break;
    }
    r.detail << kClick;
    return r;
}

void BoostPanel::refreshWarmth()
{
    if (mode != ModeValue::Music)
        return;
    const int strip = controller.getSelectedStrip();
    auto& store = controller.getParams (strip);
    auto& chain = controller.getChain (strip);
    const auto w = describeWarmth ([&store] (int id) { return store.get (id); }, [&chain] (int id) { return chain.effectiveValue (id); });
    if (w.text != warmthChip.getButtonText())
        warmthChip.setButtonText (w.text);
    warmthChip.setTooltip (w.detail);
    warmthChip.setDescription (w.text + ". " + w.detail);
}

// =============================================================================
// Headset enhancement cap chips (docs/11 E16)
// =============================================================================
void BoostPanel::refreshCapped()
{
    const bool capped = mode == ModeValue::Gaming
                        && controller.getChain (controller.getSelectedStrip()).meters().onboardCapActive.load (std::memory_order_relaxed);
    for (auto& chip : cappedChips)
        if (chip.isVisible() != capped)
            chip.setVisible (capped);
}

void BoostPanel::showCappedMenu (juce::Component& chip)
{
    juce::PopupMenu menu;
    menu.addSectionHeader ("Headset enhancement is ON for " + controller.getOutputDeviceName());
    menu.addItem (1, "Remove the cap: the headset plays flat");
    menu.addItem (2, "Keep Footsteps and Detail at most 30 %");
    juce::Component::SafePointer<BoostPanel> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&chip),
                        [safe] (int result)
                        {
                            if (safe != nullptr && result == 1)
                                safe->controller.setOnboardEnhancement (false);
                        });
}

void BoostPanel::refreshStages (const MeterSnapshot& snapshot)
{
    const int strip = controller.getSelectedStrip();
    auto& chain = controller.getChain (strip);
    auto next = describeActiveStages ([&chain] (int id) { return chain.effectiveValue (id); }, snapshot, controller.getStripChannels (strip));
    const bool same = next.size() == stages.size()
                      && std::equal (next.begin(), next.end(), stages.begin(), [] (const ActiveStage& a, const ActiveStage& b) { return a.text == b.text; });
    stages = std::move (next);
    if (! same)
    {
        juce::StringArray texts;
        for (const auto& st : stages)
            texts.add (st.text);
        setDescription ("Active now: " + (texts.isEmpty() ? juce::String ("nothing") : texts.joinIntoString (", ")));
        repaint (chipsArea);
    }
}

BoostPanel::GovernorReadout BoostPanel::describeGovernor (const MeterSnapshot& s, flub::ProtectionStrength strength)
{
    using G = flub::SafetyGovernor;
    GovernorReadout r;
    const float scale = juce::jlimit (0.0f, 1.0f, std::isfinite (s.governorScale) ? s.governorScale : 1.0f);
    const auto state = static_cast<G::State> (juce::jlimit (0, 3, s.governorState));
    const auto pct = percent (scale);
    r.limiting = scale < 0.985f;

    juce::StringArray reasons;
    if ((s.governorReason & G::kReasonLimiter) != 0)
        reasons.add ("limiter");
    if ((s.governorReason & G::kReasonDistortion) != 0)
        reasons.add ("distortion");
    if ((s.governorReason & G::kReasonDynamics) != 0)
        reasons.add ("dynamics");
    if ((s.governorReason & G::kReasonHarmonics) != 0)
        reasons.add ("harmonics");
    if ((s.governorReason & G::kReasonTonal) != 0)
        reasons.add ("brightness");

    if (! r.limiting && state == G::State::Idle)
        r.text = "Safety governor OK";
    else
    {
        r.text = "Governor " + pct;
        if (state == G::State::BackingOff)
            r.text << kDot << (reasons.isEmpty() ? juce::String ("backing off") : reasons.joinIntoString (" + "));
        else if (state == G::State::Holding)
            r.text << kDot << "holding";
        else if (state == G::State::Recovering)
            r.text << kDot << "recovering";
    }

    r.detail << "The safety governor applies " << pct << " of the governed Boost and macro amounts (bass, drive, saturation).\n";
    switch (state)
    {
        case G::State::Idle: r.detail << "Within its budgets."; break;
        case G::State::BackingOff:
            r.detail << "Backing off: " << (reasons.isEmpty() ? juce::String ("over budget") : reasons.joinIntoString (" and ") + " over budget") << ".";
            break;
        case G::State::Holding: r.detail << "Holding: close to a budget, the amounts stay where they are."; break;
        case G::State::Recovering: r.detail << "Recovering: comfortably within budget, the amounts rise again."; break;
    }
    // The budgets the chain published for its strength and mode (docs/11 E06
    // Phase 3): Off's fixed ones only while it runs at Off.
    const bool measured = s.governorStrength != static_cast<int> (flub::ProtectionStrength::Off);
    const float grBudget = measured && std::isfinite (s.governorGrBudgetDb) ? s.governorGrBudgetDb : G::kGrBudgetDb;
    r.detail << "\nLimiter, 3 s average: " << Theme::formatDb (s.governorGrDb, 1) << " dB (budget " << juce::String (grBudget, 0) << " dB)";
    if (measured)
    {
        r.detail << "\nAudible distortion (weighted residual): " << describeProtectionLevel (s.governorDriveResidualDb, s.governorResidualBudgetDb)
                 << "; bass harmonics " << describeProtectionLevel (s.governorHarmonicsResidualDb, s.governorResidualBudgetDb)
                 << " (scale " << percent (s.governorHarmonicsScale) << ")";
        r.detail << "\nDynamics (PLR, 3 s): " << describePlr (s.governorPlrDb, s.governorPlrBudgetDb);
        r.detail << "\nBrightness over 200 Hz - 1 kHz: " << describeBrightness (s) << " (tonal scale " << percent (s.governorTonalScale) << ")";
    }
    else
    {
        r.detail << "\nDistortion (THD+N), 3 s average: " << Theme::formatDb (s.governorDistortionDb, 0, -120.0f) << " dB (budget "
                 << juce::String (G::kDistortionBudgetDb, 0) << " dB)";
    }
    r.detail << "\nProtection strength: " << EngineController::getProtectionStrengthName (strength);
    switch (strength)
    {
        case flub::ProtectionStrength::Off: r.detail << " (the macro amounts only)"; break;
        case flub::ProtectionStrength::Normal: r.detail << " (also the preset's own maximizer drive, saturation drive and bass harmonics)"; break;
        case flub::ProtectionStrength::Strict: r.detail << " (as Normal, and the amounts may fall to 0)"; break;
    }
    r.detail << ". Click to change.";
    return r;
}

std::vector<BoostPanel::ActiveStage> BoostPanel::describeActiveStages (const std::function<float (int)>& e, const MeterSnapshot& s,
                                                                      int stripChannels)
{
    std::vector<ActiveStage> out;
    const auto on = [&e] (int id) { return e (id) > 0.5f; };
    const auto add = [&out] (juce::String text, juce::String detail) { out.push_back ({ std::move (text), std::move (detail) }); };
    const auto finite = [] (float v, float fallback) { return std::isfinite (v) ? v : fallback; };

    if (on (AutoLevelOn))
        add ("Auto level " + Theme::formatSignedDb (finite (s.autoLevelGainDb, 0.0f)) + " dB",
             "Auto level: levels the input towards " + juce::String (juce::roundToInt (e (AutoLevelTargetLufs))) + " LUFS");
    if (const float preamp = finite (s.autoPreampDb, 0.0f); preamp < -0.05f)
        add ("Preamp " + Theme::formatSignedDb (preamp) + " dB",
             "Automatic preamp: takes the predicted boost of the chain (" + Theme::formatSignedDb (finite (s.predictedBoostDb, 0.0f))
                 + " dB) less its allowance off the signal");
    if (on (GateOn))
        add ("Noise gate", "Spectral noise gate");

    if (on (EqOn))
    {
        int bands = 0;
        for (int b = 0; b < kEqBands; ++b)
        {
            const bool bandOn = e (eq (b, EqFieldOn)) > 0.5f;
            const int type = juce::roundToInt (e (eq (b, EqFieldType)));
            const bool hasGain = type <= 2; // Bell, Low Shelf, High Shelf; cuts, notch and band pass shape without gain
            if (bandOn && (! hasGain || std::abs (e (eq (b, EqFieldGain))) >= 0.1f))
                ++bands;
        }
        const float output = e (EqOutputGainDb);
        if (bands > 0 || std::abs (output) >= 0.1f)
        {
            juce::String text = "EQ " + juce::String (bands) + (bands == 1 ? " band" : " bands");
            if (std::abs (output) >= 0.1f)
                text << ", " << Theme::formatSignedDb (output) << " dB";
            add (text, "Parametric EQ: bands that shape the sound, and the EQ output gain");
        }
    }
    if (on (DynEqOn))
    {
        float deepest = 0.0f;
        for (const float g : s.dynEqGainDb)
            if (std::abs (finite (g, 0.0f)) > std::abs (deepest))
                deepest = g;
        if (std::abs (deepest) >= 0.5f)
            add ("Dynamic EQ " + Theme::formatSignedDb (deepest) + " dB", "Dynamic EQ: the band acting most right now");
    }

    if (on (BassOn))
    {
        if (const float hz = e (BassSubsonic); hz > 0.5f)
            add ("Subsonic " + juce::String (juce::roundToInt (hz)) + " Hz", "Subsonic high-pass: removes rumble below the speaker's range");
        if (const float db = e (BassBoostDb); db >= 0.1f)
            add ("Bass " + Theme::formatSignedDb (db) + " dB @ " + juce::String (juce::roundToInt (e (BassBoostFreq))) + " Hz", "Bass shelf");
        if (const float h = e (BassHarmonics); h >= 0.01f)
            add ("Harmonics " + percent (h), "Harmonic bass: adds harmonics so small speakers suggest the low end");
        if (const float t = e (BassTighten); t >= 0.01f)
            add ("Tighten " + percent (t), "Bass tighten: shortens the low end's decay");
        if (const float hz = e (BassMonoBelow); hz > 0.5f)
            add ("Mono bass < " + juce::String (juce::roundToInt (hz)) + " Hz", "Bass below this frequency is played in mono");
    }
    if (on (ClarityOn))
    {
        if (const float v = e (ClarityPresence); v >= 0.01f)
            add ("Presence " + percent (v), "Presence lift around " + juce::String (juce::roundToInt (e (ClarityPresenceFreq))) + " Hz");
        if (const float v = e (ClarityAir); v >= 0.01f)
            add ("Air " + percent (v), "Air: high-frequency shelf");
        if (const float v = e (ClarityDeMud); v >= 0.01f)
            add ("De-mud " + percent (v), "De-mud: lowers the 200 - 500 Hz build-up");
        if (const float v = e (ClarityAttackDb); std::abs (v) >= 0.1f)
            add ("Attack " + Theme::formatSignedDb (v) + " dB", "Transient shaper: attack");
        if (const float v = e (ClaritySustainDb); std::abs (v) >= 0.1f)
            add ("Sustain " + Theme::formatSignedDb (v) + " dB", "Transient shaper: sustain");
    }
    if (on (SaturationOn))
        if (const float drive = e (SatDriveDb); drive >= 0.1f)
        {
            const auto& types = layout()[static_cast<size_t> (SatType)].choices;
            const auto t = static_cast<size_t> (juce::jlimit (0, static_cast<int> (types.size()) - 1, juce::roundToInt (e (SatType))));
            add (juce::String (types[t]) + " " + juce::String (drive, 1) + " dB", "Saturation drive");
        }
    if (on (SpatialOn))
    {
        if (const float w = e (SpatialWidth); std::abs (w - 1.0f) >= 0.02f)
            add ("Width " + percent (w), "Stereo width (100% = unchanged)");
        if (const float v = e (SpatialFocus); v >= 0.01f)
            add ("Focus " + percent (v), "Positional focus");
        if (const float v = e (SpatialSpace); v >= 0.01f)
            add ("Space " + percent (v), "Space: early reflections");
        if (const float v = e (SpatialCrossfeed); v >= 0.01f)
            add ("Crossfeed " + percent (v), "Headphone crossfeed");
    }
    if (on (VirtualizerOn) && stripChannels > 2 && s.inputFold == 0)
        add ("Virtualizer", "Headphone virtualiser: binaural render of the surround input");
    if (on (CompressorOn))
    {
        juce::String text = "Compressor " + juce::String (e (CompRatio), 1) + ":1";
        if (const float gr = finite (s.compGainReductionDb, 0.0f); gr < -0.5f)
            text << " " << Theme::formatSignedDb (gr) << " dB";
        add (text, "Compressor ratio and its gain reduction now");
    }
    if (on (MaximizerOn))
    {
        if (const float drive = e (MaxDriveDb); drive >= 0.1f)
            add ("Maximizer " + Theme::formatSignedDb (drive) + " dB drive", "Loudness maximizer drive into the clipper and limiter");
        if (const float gr = finite (s.maxGainReductionDb, 0.0f); gr < -0.5f)
            add ("Limiter " + Theme::formatSignedDb (gr) + " dB", "The maximizer's limiter is reducing gain now");
    }
    return out;
}

juce::String BoostPanel::getTooltip()
{
    const auto p = getMouseXYRelative().toFloat();
    if (governorChip.contains (p))
        return governor.detail;
    if (chipsArea.toFloat().contains (p))
    {
        if (stages.empty())
            return "Active now: no stage changes the sound (flat).";
        juce::String tip ("Active now:");
        for (const auto& st : stages)
            tip << "\n" << st.text << kDot << st.detail;
        return tip;
    }
    return {};
}

void BoostPanel::mouseUp (const juce::MouseEvent& e)
{
    if (governorChip.contains (e.position))
        showStrengthMenu();
}

void BoostPanel::showStrengthMenu()
{
    using S = flub::ProtectionStrength;
    juce::PopupMenu menu;
    menu.addSectionHeader ("Protection strength");
    const auto current = controller.getProtectionStrength();
    menu.addItem (1, "Off: govern the macro amounts only", true, current == S::Off);
    menu.addItem (2, "Normal: also the preset's own drive and harmonics", true, current == S::Normal);
    menu.addItem (3, "Strict: as Normal, down to 0", true, current == S::Strict);
    juce::Component::SafePointer<BoostPanel> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (governorChip.toNearestInt())),
                        [safe] (int result)
                        {
                            if (safe == nullptr || result < 1 || result > 3)
                                return;
                            safe->controller.setProtectionStrength (static_cast<S> (result - 1));
                        });
}

void BoostPanel::paint (juce::Graphics& g)
{
    Theme::drawPanel (g, getLocalBounds().toFloat());

    auto header = headerArea.toFloat();
    Theme::drawCaption (g, "BOOST INTENSITY", header.removeFromLeft (static_cast<float> (dialArea.getWidth()) + 20.0f));
    Theme::drawCaption (g, mode == ModeValue::Gaming ? "GAMING MACROS" : "MUSIC MACROS", header, Palette::muted);

    // Safety governor status (right end of the header): how much of the
    // governed Boost / macro amounts is applied right now, and why.
    const bool limiting = governor.limiting;
    g.setFont (Theme::font (11.5f));
    const float sw = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), governor.text);
    auto chip = header.removeFromRight (sw + 26.0f).withSizeKeepingCentre (sw + 26.0f, 18.0f);
    governorChip = chip;
    const auto colour = limiting ? Palette::amber : Palette::green;
    g.setColour (colour.withAlpha (0.1f));
    g.fillRoundedRectangle (chip, 9.0f);
    g.setColour (colour.withAlpha (0.35f));
    g.drawRoundedRectangle (chip.reduced (0.5f), 9.0f, 1.0f);
    g.setColour (colour);
    g.fillEllipse (chip.withWidth (18.0f).withSizeKeepingCentre (6.0f, 6.0f).translated (4.0f, 0.0f));
    g.setColour (limiting ? Palette::amber : Palette::text.withAlpha (0.85f));
    g.drawText (governor.text, chip.withTrimmedLeft (18.0f).withTrimmedRight (6.0f), juce::Justification::centred, false);

    // Active-now chips under the macros (wrapped over chipRows); what does
    // not fit is counted.
    if (! chipsArea.isEmpty())
    {
        g.setFont (Theme::font (11.0f));
        Theme::drawCaption (g, "ACTIVE", chipsArea.toFloat().withHeight (20.0f).withWidth (48.0f), Palette::faint);
        const auto accent = Theme::accent (*this);
        shownStages = 0;
        if (stages.empty())
        {
            g.setColour (Palette::faint);
            g.drawText ("Flat: no stage changes the sound", chipRows.front(), juce::Justification::centredLeft, true);
        }
        std::vector<float> widths;
        widths.reserve (stages.size());
        for (const auto& st : stages)
            widths.push_back (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), st.text) + 16.0f);
        const auto chips = layoutChips (widths, chipRows);
        for (size_t i = 0; i < chips.pills.size(); ++i)
        {
            const auto pill = chips.pills[i];
            g.setColour (accent.withAlpha (0.1f));
            g.fillRoundedRectangle (pill, 8.5f);
            g.setColour (accent.withAlpha (0.35f));
            g.drawRoundedRectangle (pill.reduced (0.5f), 8.5f, 1.0f);
            g.setColour (Palette::text.withAlpha (0.9f));
            g.drawText (stages[i].text, pill, juce::Justification::centred, false);
        }
        shownStages = static_cast<int> (chips.pills.size());
        if (chips.hidden > 0)
        {
            g.setColour (Palette::muted);
            g.drawText ("+" + juce::String (chips.hidden), chips.more, juce::Justification::centredLeft, false);
        }
    }

    if (rangeBox.isVisible())
    {
        Theme::drawCaption (g, "DYNAMIC RANGE", rangeCaption.toFloat(), Palette::faint);
        Theme::drawCaption (g, "SMOOTHNESS", smoothCaption.toFloat(), Palette::faint);
    }

    // Divider between the dial and the macros.
    g.setColour (Palette::border);
    g.fillRect (static_cast<float> (macroArea.getX()) - 12.0f, static_cast<float> (macroArea.getY()) + 8.0f, 1.0f,
                static_cast<float> (macroArea.getHeight()) - 16.0f);
}

BoostPanel::ChipLayout BoostPanel::layoutChips (const std::vector<float>& widths, const std::vector<juce::Rectangle<float>>& rows)
{
    ChipLayout out;
    size_t next = 0;
    for (size_t r = 0; r < rows.size() && next < widths.size(); ++r)
    {
        auto row = rows[r];
        const bool lastRow = r + 1 == rows.size();
        while (next < widths.size())
        {
            const float w = widths[next];
            const float reserve = lastRow && next + 1 < widths.size() ? kMoreWidth : 0.0f;
            if (w + reserve > row.getWidth())
            {
                if (lastRow)
                {
                    out.hidden = static_cast<int> (widths.size() - next);
                    out.more = row;
                    return out;
                }
                break; // next row
            }
            out.pills.push_back (row.removeFromLeft (w).withSizeKeepingCentre (w, kChipHeight));
            row.removeFromLeft (kChipGap);
            ++next;
        }
    }
    if (next < widths.size()) // no rows at all
        out.hidden = static_cast<int> (widths.size() - next);
    return out;
}

void BoostPanel::resized()
{
    const bool simple = panelLayout == Layout::Simple;
    auto r = getLocalBounds().reduced (14, 10);
    headerArea = r.removeFromTop (18);
    r.removeFromTop (2);

    // The dial is the hero: as tall as the panel allows.
    const int dialSize = juce::jlimit (104, simple ? 280 : 196, r.getHeight());
    auto left = r.removeFromLeft (juce::jmax (dialSize, 150));
    dialArea = left;
    dial.setBounds (left.withSizeKeepingCentre (dialSize, dialSize));

    r.removeFromLeft (26);
    macroArea = r;
    // The chips take the bottom of the macro column when the knobs can spare
    // it: one 20 px row (Standard), or up to three (Simple).
    // A Simple row more only while the knobs keep about 118 px.
    constexpr int kRowH = 20;
    // The Simple layout's Dynamic Range / Smoothness row (above the chips)
    // while the knobs keep about 118 px and one chip row fits too.
    constexpr int kGuardRowH = 24, kGuardRowGap = 8;
    const bool guardRow = simple && r.getHeight() >= 118 + kGuardRowH + kGuardRowGap + kRowH + 6;
    const int guardSpace = guardRow ? kGuardRowH + kGuardRowGap : 0;
    const int rowsWanted = simple ? juce::jlimit (1, 3, (r.getHeight() - 118 - guardSpace) / (kRowH + 2)) : 1;
    chipsArea = r.getHeight() >= 104 ? r.removeFromBottom (rowsWanted * kRowH + (rowsWanted - 1) * 2) : juce::Rectangle<int>();
    chipRows.clear();
    if (! chipsArea.isEmpty())
    {
        auto rows = chipsArea.toFloat().withTrimmedLeft (48.0f); // right of the ACTIVE caption
        for (int i = 0; i < rowsWanted; ++i)
        {
            chipRows.push_back (rows.removeFromTop (static_cast<float> (kRowH)));
            rows.removeFromTop (2.0f);
        }
    }
    if (simple && ! chipsArea.isEmpty())
        r.removeFromBottom (6);
    rangeBox.setVisible (guardRow);
    smoothSlider.setVisible (guardRow);
    if (guardRow)
    {
        auto row = r.removeFromBottom (kGuardRowH);
        r.removeFromBottom (kGuardRowGap);
        // The captions keep their text's width down to the 800 px window (docs/11 E39).
        rangeCaption = row.removeFromLeft (juce::jmin (104, juce::jmax (98, row.getWidth() / 5)));
        rangeBox.setBounds (row.removeFromLeft (juce::jmin (160, row.getWidth() / 3)).reduced (0, 1));
        row.removeFromLeft (24);
        smoothCaption = row.removeFromLeft (juce::jmin (88, juce::jmax (84, row.getWidth() / 4)));
        smoothSlider.setBounds (row.removeFromLeft (juce::jmin (240, row.getWidth())));
    }
    else
    {
        rangeCaption = smoothCaption = {};
    }

    // Macros: evenly spaced (at most 170 px apart, centred), smaller than the dial.
    const int n = static_cast<int> (macros.size());
    const int cellW = juce::jmin (170, r.getWidth() / n);
    r = r.withSizeKeepingCentre (cellW * n, r.getHeight());
    const int knobH = juce::jmin (r.getHeight(), simple ? 156 : 124);
    const int knobW = juce::jmin (cellW - 6, simple ? 124 : 96);
    for (int i = 0; i < n; ++i)
    {
        auto cell = r.removeFromLeft (cellW);
        macros[static_cast<size_t> (i)].setBounds (cell.withSizeKeepingCentre (knobW, knobH));
    }

    // The Warmth chip beside the Warmth knob's value (bottom right, clear of
    // the value text and the dial's arc), out into the cell's margin.
    {
        constexpr int kChipH = 17, kValueHalfW = 17;
        const auto knob = macros[4].getBounds();
        const int x = knob.getCentreX() + kValueHalfW;
        const int w = juce::jmin (40, knob.getCentreX() + cellW / 2 - x - 1);
        warmthChip.setBounds (x, knob.getBottom() - kChipH - 1, w, kChipH);
    }

    // The CAPPED chips beside the Footsteps and Detail values, placed like
    // the Warmth chip; "CAP" where the cell's margin is narrow.
    for (size_t i = 0; i < cappedChips.size(); ++i)
    {
        constexpr int kChipH = 17, kValueHalfW = 17;
        const auto knob = macros[i == 0 ? 0 : 3].getBounds();
        const int x = knob.getCentreX() + kValueHalfW;
        const int w = juce::jmin (60, knob.getCentreX() + cellW / 2 - x - 1);
        cappedChips[i].setButtonText (w >= 56 ? "CAPPED" : "CAP");
        cappedChips[i].setBounds (x, knob.getBottom() - kChipH - 1, juce::jmax (30, w), kChipH);
    }
}
} // namespace flub::app::ui
