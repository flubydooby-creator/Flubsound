// Flubsound Pro - loudness, dynamics and stereo telemetry of the selected strip.
//
//   LOUDNESS  momentary / short-term / integrated LUFS (EBU R128 / BS.1770),
//             loudness range (LU), true-peak maximum, auto-level gain; what
//             the strip does to loudness (docs/11 E38 / E11): the short-term
//             in -> out difference (LU), the share of time the maximizer's
//             limiter reduces gain by more than 1 dB (about the last 10 s:
//             "LIM", E11's "Limiter active x %", spelled out on hover) and
//             the automatic preamp; while the hearing guard knows the
//             headset's sensitivity (docs/11 E32 (c)), a row with the
//             estimated level, today's estimated dose and the cap ("EST.
//             72 dB(A)  DOSE 12 %  CAP 85"), left out otherwise
//   DYNAMICS  gain-reduction meters: compressor (with upward gain), limiter,
//             multiband glue, bass protection, master safety limiter,
//             distortion (measured THD+N of saturator and clipper; the
//             SafetyGovernor's -30 dB budget marked at protection strength
//             Off, the only strength that budgets it: Normal / Strict budget
//             the audible residual under PROTECTION) and the harmonics the bass
//             harmonics and air exciter add on purpose (not budgeted)
//   PROTECTION the SafetyGovernor's measured loop at protection strength
//             Normal / Strict (docs/11 E06 / E07): the audible (weighted)
//             residual against its budget, the output PLR against its
//             budget and the brightness lifts (presence / harsh / air over
//             200 Hz - 1 kHz) against theirs; "Off" says how to switch it on.
//             Left out when the panel is too short for it.
//   STEREO    correlation meter (-1 .. +1) and effective width
// All values come from one MeterSnapshot per display frame; gain-reduction
// bars get a short release so they stay readable. Their colours, the
// correlation meter's and the true-peak warning follow the meter palette
// (Theme::statusColours). Clicking the integrated readout resets it
// (onResetRequested -> MeterBus::resetLoudnessRequest).
#pragma once

#include "MeterSnapshot.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>

namespace flub::app
{
class EngineController;
}

namespace flub::app::ui
{
class LoudnessPanel : public juce::Component, public juce::SettableTooltipClient
{
public:
    LoudnessPanel();

    void update (const MeterSnapshot& snapshot, double dtSeconds);
    void reset();

    /** Displayed values (after smoothing). */
    float getDisplayedCorrelation() const noexcept { return shown.correlation; }
    float getDisplayedShortTermLufs() const noexcept { return shown.shortTerm; }
    /** Share of the last ~10 s (0..1) with the limiter more than 1 dB down. */
    float getLimiterActiveShare() const noexcept { return shown.limiterActive; }
    /** Whether the last paint had room for the PROTECTION section. */
    bool isShowingProtection() const noexcept { return protectionShown; }
    /** "PLR 9.1 / 8", "BRIGHT +1.2 +0.4 +0.8" as painted (tests). */
    static juce::String formatPlr (float plrDb, float budgetDb);
    static juce::String formatBrightness (const std::array<float, 3>& liftDb);

    /** "+2.3 LU" (out - in, short-term), or "--" while either side is below
        -70 LUFS (silence: no meaningful difference). */
    static juce::String formatInOutDelta (float inLufs, float outLufs);
    /** How long the "limiter active" share averages over. */
    static constexpr float kLimiterActiveSeconds = 10.0f;
    static constexpr float kLimiterActiveThresholdDb = -1.0f;
    /** The LIM readout's tooltip (docs/11 E11's "Limiter active x %"):
        "Limiter active 12 % of the last 10 s ...". */
    static juce::String describeLimiterActive (float share01);
    /** Hover text of the readout under the pointer (the LIM readout; the
        panel's own tooltip elsewhere). */
    juce::String getTooltip() override;

    std::function<void()> onResetRequested;

    /** The hearing guard's readings for the dose row (docs/11 E32 (c)). */
    struct HearingReadout
    {
        bool known = false;        // a sensitivity is in use; the row shows only then
        float levelDbA = -1000.0f; // the estimate now (HearingMeters::kUnknown when there is none)
        double doseToday = 0.0;    // fraction of the weekly allowance
        bool capOn = false, capActive = false;
        float capDbA = 85.0f;
        bool operator== (const HearingReadout&) const = default;
    };
    /** EngineController::getHearing() as the row reads it. */
    static HearingReadout hearingReadoutOf (const EngineController& controller);
    /** Polled by update() when set (MainComponent sets it). */
    std::function<HearingReadout()> hearingSource;
    void setHearing (const HearingReadout& readout);
    /** Whether the last paint drew the dose row. */
    bool isShowingDose() const noexcept { return doseShown; }
    /** The dose readout's value: "12 %" ("4.5 %" under 10 %). */
    static juce::String formatDose (double fraction);

    void paint (juce::Graphics& g) override;
    void mouseUp (const juce::MouseEvent& e) override;

private:
    struct Shown
    {
        float momentary = -160.0f, shortTerm = -160.0f, integrated = -160.0f, range = 0.0f, truePeakMax = -160.0f;
        float autoLevel = 0.0f;
        float inShortTerm = -160.0f, limiterActive = 0.0f, preamp = 0.0f;
        float comp = 0.0f, compUp = 0.0f, limiter = 0.0f, glue = 0.0f, clip = -160.0f, harmonics = -160.0f, bass = 0.0f, master = 0.0f;
        float correlation = 1.0f, width = 1.0f;
        bool active = false;
        int strength = 0;
        float residual = -160.0f, residualBudget = -35.0f, plr = 1000.0f, plrBudget = 8.0f;
        std::array<float, 3> lift { -160.0f, -160.0f, -160.0f }, liftBudget { 3.0f, 3.0f, 4.0f };
        HearingReadout hearing;
    };

    void drawGainReductionRow (juce::Graphics& g, juce::Rectangle<float> row, const juce::String& name, float reductionDb,
                               float rangeDb, float upwardDb = 0.0f);
    /** A -60 .. -10 dB level row (distortion, harmonics); budgetDb > -60
        draws the budget marker and turns the bar hot above it. */
    void drawLevelRow (juce::Graphics& g, juce::Rectangle<float> row, const juce::String& name, float levelDb, float budgetDb,
                       juce::Colour colour);

    Shown shown, painted;
    float sinceRepaint = 0.0f; // readouts refresh at <= 20 Hz
    bool protectionShown = false, doseShown = false;
    juce::Rectangle<float> integratedArea, limiterActiveArea, doseArea;
};
} // namespace flub::app::ui
