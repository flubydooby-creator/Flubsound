// Flubsound Pro - Boost Intensity + the five mode macros.
//
// BoostDial  the large 0-100 % "Boost Intensity" arc control (a juce::Slider,
//            so drag, wheel, keyboard, double-click reset and accessibility
//            come for free). An inner arc shows how much of it is actually
//            applied: MeterBus::governorScale scales every "governed" macro
//            contribution (bass, drive, saturation) when the SafetyGovernor
//            detects over-processing; a status chip in the header names it.
// Macros     five knobs whose captions come from flub::MacroMap::macroName
//            (Music: Punch / Width / Clarity / Loudness / Warmth, Gaming:
//            Footsteps / Positional / Impact / Detail / Voice & Score) and
//            change with the mode.
// Governor  the header chip names the SafetyGovernor's state and reason
//           (docs/11 E06: "Governor 72 % . limiter", also dynamics,
//           harmonics, brightness), its tooltip the ~3 s averages against the
//           budgets the chain runs with (at Normal / Strict the measured
//           loop's: weighted residual, PLR, brightness, E07) and the
//           protection strength; a
//           click on it chooses the strength (Off / Normal / Strict,
//           EngineController::setProtectionStrength).
// Active    "active now" chips under the macros (docs/11 E38 slice): the
//           stages that change the sound right now, from the chain's
//           effective values (after the macros and the governor) and its
//           meters, e.g. "Bass +3.1 dB @ 70 Hz", "Width 118 %", "Maximizer
//           +4.0 dB drive"; what does not fit is counted ("+3") and listed in
//           the tooltip.
// Layouts  Standard (the Advanced view's strip: one chip row) and Simple
//           (docs/11 E39: the Simple view's centre piece, a larger dial and up
//           to three wrapped chip rows).
// Warmth    Music only, a chip beside the Warmth macro's value (docs/11
//           E14): what Warmth does to the saturator now - TUBE (its
//           override row chose the Tube saturator), TAPE (warmth.tapeGrit:
//           the classic tape Warmth) or TONE (the tone tilt alone, or the
//           user's own saturator); a click switches Tape Grit on / off.
// Capped    Gaming only, a CAPPED chip beside the Footsteps and Detail
//           values while the output's headset enhancement cap applies
//           (docs/11 E16, MeterBus::onboardCapActive: at most 30 % reach the
//           sound, the virtualiser is off; the knobs keep their values); a
//           click offers to remove the cap (EngineController::
//           setOnboardEnhancement).
// Guards    Simple layout only, a row under the macros: Dynamic Range
//           (guard.range, docs/11 E21: the Startle Guard's ceiling over the
//           recent programme and the Gaming Tame band) and Smoothness
//           (smooth.amount, docs/11 E07: takes back the sibilance the chain
//           added). The Advanced view has them on the Compressor and Clarity
//           cards of the module rack.
// All controls are bound to the selected strip's ParameterStore.
#pragma once

#include "MeterSnapshot.h"
#include "ParamKnob.h"
#include "ParameterBinding.h"
#include "engine/EngineController.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <vector>

namespace flub::app::ui
{
class BoostDial : public juce::Slider
{
public:
    BoostDial();

    /** 0.3 .. 1: share of the governed macro amounts that is applied. */
    void setGovernorScale (float scale);
    float getGovernorScale() const noexcept { return governor; }

    void paint (juce::Graphics& g) override;

private:
    float governor = 1.0f;
};

class BoostPanel : public juce::Component, public juce::TooltipClient
{
public:
    explicit BoostPanel (EngineController& controller);

    enum class Layout
    {
        Standard, // the Advanced view: dial <= 196 px, one chip row
        Simple    // the Simple view (docs/11 E39): dial <= 280 px, up to three chip rows
    };
    void setLayout (Layout layout);
    Layout getLayout() const noexcept { return panelLayout; }

    /** Mode changes the macro captions / tooltips. */
    void setMode (flub::param::ModeValue mode);
    void setGovernorScale (float scale);
    /** Per display frame: the governor readout, and (every few frames) the
        active-now chips from the selected strip's effective values. */
    void update (const MeterSnapshot& snapshot);

    // ---- Readouts (pure; tested) -------------------------------------------------------
    struct GovernorReadout
    {
        juce::String text;   // the chip: "Safety governor OK", "Governor 72 % . limiter"
        juce::String detail; // tooltip: what it does, the averages against the budgets, the strength
        bool limiting = false;
    };
    static GovernorReadout describeGovernor (const MeterSnapshot& snapshot, flub::ProtectionStrength strength);
    /** "-41 dB (budget -35 dB)" or "not measured yet (budget -35 dB)". */
    static juce::String describeProtectionLevel (float levelDb, float budgetDb);
    /** "9.1 dB (at least 8 dB)", "... (no budget in this mode)", or "not
        measured yet" with the same budget text. */
    static juce::String describePlr (float plrDb, float budgetDb);
    /** "presence +1.2 dB (budget +3), harsh ..., air ..." or "not measured
        yet (budgets +3 / +3 / +4 dB)". */
    static juce::String describeBrightness (const MeterSnapshot& snapshot);

    struct ActiveStage
    {
        juce::String text;   // "Bass +3.1 dB @ 70 Hz"
        juce::String detail; // one tooltip line
    };
    /** The stages that change the sound now, in signal order: `effective`
        returns a parameter's effective value (ProcessingChain::effectiveValue),
        the snapshot the meters (limiter, dynamic EQ, compressor, auto level,
        automatic preamp, input fold). stripChannels > 2 lets the virtualiser
        count. */
    static std::vector<ActiveStage> describeActiveStages (const std::function<float (int)>& effective, const MeterSnapshot& snapshot,
                                                          int stripChannels);

    /** Where the chips go: each chip's pill, in order, over `rows` (top to
        bottom). A chip that does not fit moves to the next row; on the last
        row every chip but the last keeps kMoreWidth free for the "+N" of the
        chips that do not fit (`hidden`, drawn in `more`). Pure; tested. */
    struct ChipLayout
    {
        std::vector<juce::Rectangle<float>> pills;
        int hidden = 0;
        juce::Rectangle<float> more;
    };
    static constexpr float kChipHeight = 17.0f, kChipGap = 5.0f, kMoreWidth = 34.0f;
    static ChipLayout layoutChips (const std::vector<float>& chipWidths, const std::vector<juce::Rectangle<float>>& rows);

    /** The Warmth chip (docs/11 E14): text, tooltip and whether Tape Grit
        is on, from the selected strip's stored (`base`) and effective values
        (WarmthColour). Pure; tested. */
    struct WarmthReadout
    {
        juce::String text; // "TUBE", "TAPE", "TONE"
        juce::String detail;
        bool tapeGrit = false;
    };
    static WarmthReadout describeWarmth (const std::function<float (int)>& base, const std::function<float (int)>& effective);
    /** Visible in Music mode only; bound to the selected strip's warmth.tapeGrit. */
    juce::TextButton& getWarmthChip() noexcept { return warmthChip; }
    /** The headset enhancement cap's chips (docs/11 E16): 0 Footsteps, 1
        Detail; visible in Gaming mode while the selected strip's chain caps. */
    juce::TextButton& getCappedChip (int index) noexcept { return cappedChips[static_cast<size_t> (index != 0)]; }

    /** Chips drawn by the last paint (the rest are counted as "+N"). */
    int getShownStageCount() const noexcept { return shownStages; }
    int getChipRowCount() const noexcept { return static_cast<int> (chipRows.size()); }
    /** The Simple layout's Dynamic Range and Smoothness controls (hidden in
        the Standard layout and when the panel is too short for their row). */
    juce::ComboBox& getDynamicRangeBox() noexcept { return rangeBox; }
    juce::Slider& getSmoothnessSlider() noexcept { return smoothSlider; }
    juce::Rectangle<int> getDialBounds() const { return dial.getBounds(); }

    const GovernorReadout& getGovernorReadout() const noexcept { return governor; }
    const std::vector<ActiveStage>& getActiveStages() const noexcept { return stages; }

    juce::String getTooltip() override;
    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent& e) override;

private:
    void showStrengthMenu();
    void refreshStages (const MeterSnapshot& snapshot);
    void refreshWarmth();
    void refreshCapped();
    void showCappedMenu (juce::Component& chip);

    EngineController& controller;
    BoostDial dial;
    std::array<ParamKnob, 5> macros;
    juce::ComboBox rangeBox;
    juce::Slider smoothSlider;
    juce::TextButton warmthChip; // under the Warmth macro (Music)
    std::array<juce::TextButton, 2> cappedChips; // beside Footsteps and Detail (Gaming, docs/11 E16)
    ParameterBinder binder; // after the controls it binds
    flub::param::ModeValue mode = flub::param::ModeValue::Music;
    juce::Rectangle<int> dialArea, macroArea, headerArea, chipsArea, rangeCaption, smoothCaption;
    std::vector<juce::Rectangle<float>> chipRows; // inside chipsArea, right of the ACTIVE caption
    Layout panelLayout = Layout::Standard;
    juce::Rectangle<float> governorChip;
    GovernorReadout governor;
    std::vector<ActiveStage> stages;
    int shownStages = 0, framesSinceStages = 1000;
};
} // namespace flub::app::ui
