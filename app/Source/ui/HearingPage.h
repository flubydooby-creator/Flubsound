// Flubsound Pro - Settings > Hearing (docs/11 E32 (c), E33).
//
//   Listening level  the hearing guard's ESTIMATE (EngineController::
//                    getHearing, flub::HearingGuard): the headset
//                    sensitivity in use - the listener's own figure (a
//                    switch and a value that takes typed numbers, stored per
//                    output) or the matched device profile's, marked
//                    "manufacturer figure, not lab-verified" unless the
//                    device lab measured it - and the level now, over the
//                    last 5 s and this session, with the system volume it
//                    counts. Without a sensitivity the page says "unknown"
//                    and nothing is estimated or applied.
//   Cap              the listening-level cap: a switch (off by default) and
//                    its level (60 .. 100 dB(A), 85 by default); it acts only
//                    while a sensitivity is known and says when it holds the
//                    level down.
//   Dose             today's estimated dose and the last 7 days' against the
//                    WHO / ITU-T H.870 reference (80 dB(A) for 40 h a week),
//                    kept per day by the controller.
//   Personal profile the per-ear listening preference (PersonalProfileEditor).
// Non-medical wording throughout: an estimate, not a measurement, a hearing
// test or a medical device. The page sets its own height (the dialog shows
// it in a scrolling view) and refreshes with the dialog's timer.
#pragma once

#include "PersonalProfileEditor.h"
#include "engine/EngineController.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace flub::app::ui
{
class HearingPage : public juce::Component
{
public:
    explicit HearingPage (EngineController& controller);

    void refresh();

    /** The sensitivity line: whose figure is in use and what it is, or why
        it is unknown ("Unknown: ..."). */
    static juce::String describeSensitivity (const EngineController::HearingInfo& info);
    /** "Estimated now 72 dB(A) ..." or "Unknown: ..." without a sensitivity. */
    static juce::String describeEstimate (const EngineController::HearingInfo& info);
    /** "Today about 12 % of the weekly allowance ..." or "Unknown: ...". */
    static juce::String describeDose (const EngineController::HearingInfo& info);
    /** The cap's help line (what it does, and whether it acts now). */
    static juce::String describeCap (const EngineController::HearingInfo& info);
    /** "12 %" (at least one decimal below 10 %). */
    static juce::String formatDosePercent (double fraction);

    // Controls (tests, screenshots).
    juce::ToggleButton& getOwnFigureToggle() noexcept { return ownFigure; }
    juce::Slider& getSensitivitySlider() noexcept { return sensitivity; }
    juce::ToggleButton& getCapToggle() noexcept { return capToggle; }
    juce::Slider& getCapSlider() noexcept { return capSlider; }
    PersonalProfileEditor& getProfileEditor() noexcept { return profileEditor; }
    /** The lines as painted (tests). */
    const juce::String& getSensitivityText() const noexcept { return sensitivityText; }
    const juce::String& getEstimateText() const noexcept { return estimateText; }
    const juce::String& getDoseText() const noexcept { return doseText; }
    juce::Rectangle<int> getProfileSectionTitle() const noexcept { return profileTitle; }

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    EngineController& controller;
    juce::ToggleButton ownFigure { "Use my own sensitivity figure" };
    juce::Slider sensitivity;
    juce::ToggleButton capToggle { "Cap the listening level" };
    juce::Slider capSlider;
    PersonalProfileEditor profileEditor;
    bool refreshing = false;
    juce::String sensitivityText, estimateText, doseText, capText;
    juce::Rectangle<int> introArea, levelTitle, sensitivityCaption, sensitivityHelp, estimateCaption, estimateArea, capTitle, capCaption,
        capHelp, doseTitle, doseArea, profileTitle;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HearingPage)
};
} // namespace flub::app::ui
