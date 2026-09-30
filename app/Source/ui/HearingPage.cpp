#include "HearingPage.h"

#include "Theme.h"
#include "Widgets.h"

#include "flub/engine/HearingGuard.h"

#include <algorithm>
#include <cmath>

namespace flub::app::ui
{
namespace
{
constexpr int kRowHeight = 30;
constexpr int kCaptionWidth = 190;
constexpr int kLine = 15; // pitch of the 11.5 / 12 px help text
/** Where "Use my own sensitivity figure" starts when neither the listener
    nor the profile has one: a typical USB / wireless gaming headset at full
    volume (a starting point to correct, never used until switched on). */
constexpr float kStartingFigureDbSpl = 110.0f;

void drawSectionTitle (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title)
{
    Theme::drawCaption (g, title.toUpperCase(), area.toFloat(), Palette::muted);
    g.setColour (Palette::border);
    g.fillRect (area.getX(), area.getBottom() - 1, area.getWidth(), 1);
}

/** Height of `text` wrapped into `width` at the help font (at least one line). */
int textHeight (const juce::String& text, int width)
{
    const auto lines = juce::jmax (1, static_cast<int> (std::ceil (juce::GlyphArrangement::getStringWidth (Theme::font (11.5f), text)
                                                                    / juce::jmax (80.0f, static_cast<float> (width) - 8.0f))));
    return lines * kLine + 4;
}

juce::String dbA (float level)
{
    return level <= flub::HearingMeters::kUnknown * 0.5f ? juce::String ("--") : juce::String (juce::roundToInt (level)) + " dB(A)";
}
} // namespace

HearingPage::HearingPage (EngineController& c)
    : controller (c), profileEditor (c)
{
    setTitle ("Hearing");

    Style::set (ownFigure, "switch");
    ownFigure.setTitle ("Own sensitivity figure");
    ownFigure.setTooltip ("Use your own figure for this headset instead of the device profile's (stored for this output device)");
    ownFigure.onClick = [this]
    {
        if (refreshing)
            return;
        if (ownFigure.getToggleState())
            controller.setHearingSensitivity (static_cast<float> (sensitivity.getValue())); // prefilled from the profile
        else
            controller.setHearingSensitivity (std::nullopt);
        refresh();
    };
    addAndMakeVisible (ownFigure);

    sensitivity.setSliderStyle (juce::Slider::LinearHorizontal);
    sensitivity.setTextBoxStyle (juce::Slider::TextBoxRight, false, 96, 22);
    sensitivity.setTextBoxIsEditable (true); // type the figure
    sensitivity.setRange (flub::HearingGuard::kMinSensitivityDbSpl, flub::HearingGuard::kMaxSensitivityDbSpl, 0.5);
    sensitivity.setTextValueSuffix (" dB SPL");
    sensitivity.setTitle ("Headset sensitivity");
    sensitivity.setTooltip ("The level (dB SPL) a full-scale tone plays at with the system volume at 100 %: the headset's own figure, "
                            "or a measurement. A passive headphone's dB/mW figure also needs its amplifier's voltage.");
    sensitivity.onValueChange = [this]
    {
        if (! refreshing && ownFigure.getToggleState())
            controller.setHearingSensitivity (static_cast<float> (sensitivity.getValue()));
    };
    addAndMakeVisible (sensitivity);

    Style::set (capToggle, "switch");
    capToggle.setTitle ("Listening-level cap");
    capToggle.onClick = [this]
    {
        if (! refreshing)
            controller.setHearingCap (capToggle.getToggleState(), static_cast<float> (capSlider.getValue()));
        refresh();
    };
    addAndMakeVisible (capToggle);

    capSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    capSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 96, 22);
    capSlider.setTextBoxIsEditable (true);
    capSlider.setRange (flub::HearingGuard::kCapMinDbA, flub::HearingGuard::kCapMaxDbA, 1.0);
    capSlider.setTextValueSuffix (" dB(A)");
    capSlider.setDoubleClickReturnValue (true, flub::HearingGuard::kDefaultCapDbA);
    capSlider.setTitle ("Listening-level cap level");
    capSlider.setTooltip ("The highest estimated level over any 5 s. 85 dB(A) is the usual workplace action level; 75 dB(A) suits "
                          "children and quiet listening.");
    capSlider.onValueChange = [this]
    {
        if (! refreshing)
            controller.setHearingCap (capToggle.getToggleState(), static_cast<float> (capSlider.getValue()));
    };
    addAndMakeVisible (capSlider);

    addAndMakeVisible (profileEditor);
    refresh();
}

juce::String HearingPage::formatDosePercent (double fraction)
{
    const double pct = std::isfinite (fraction) ? std::max (0.0, fraction) * 100.0 : 0.0;
    return (pct < 10.0 ? juce::String (pct, 1) : juce::String (juce::roundToInt (pct))) + " %";
}

juce::String HearingPage::describeSensitivity (const EngineController::HearingInfo& info)
{
    using Source = flub::HearingGuard::SensitivitySource;
    if (info.output.isEmpty())
        return "Unknown: no output device is open, so nothing is estimated or applied.";
    const auto device = info.profileName.isNotEmpty() ? info.profileName : info.output;
    const auto profileLine = [&info, &device]
    {
        juce::String t;
        t << device << ": " << juce::String (info.profileDbSpl, 1) << " dB SPL at full volume ("
          << (info.profileLabVerified ? juce::String ("measured in the device lab")
                                      : (info.profileFigureSource.isNotEmpty() && info.profileFigureSource != "manufacturer"
                                             ? info.profileFigureSource + " figure, not lab-verified"
                                             : juce::String ("manufacturer figure, not lab-verified")))
          << ")";
        return t;
    };
    if (info.source == Source::User)
    {
        juce::String t;
        t << "Your figure for this output: " << juce::String (info.sensitivityDbSpl, 1) << " dB SPL at full volume.";
        if (! std::isnan (info.profileDbSpl))
            t << " The profile's: " << profileLine() << ".";
        return t;
    }
    if (info.source == Source::Profile)
        return "From the device profile: " + profileLine() + ". Switch on your own figure to correct it.";
    return "Unknown: " + (info.profileName.isNotEmpty() ? "the " + info.profileName + " profile" : juce::String ("this output"))
           + " has no sensitivity figure, so nothing is estimated or applied. Enter your headset's figure (dB SPL for a full-scale "
             "tone at full volume) to switch the estimate on.";
}

juce::String HearingPage::describeEstimate (const EngineController::HearingInfo& info)
{
    if (! info.known)
        return "Unknown: without a sensitivity nothing is estimated and the sound is not touched.";
    juce::String t;
    t << "Estimated now " << dbA (info.levelDbA) << ", last 5 s " << dbA (info.leq5sDbA) << ", this session " << dbA (info.sessionLeqDbA)
      << "; ";
    if (info.volumeKnown)
        t << "system volume " << juce::String (info.volumeDb, 1) << " dB.";
    else
        t << "the system volume cannot be read, so it counts as full volume.";
    t << " An estimate: the headset's own volume dial, fit and EQ can put it 10 - 20 dB off.";
    return t;
}

juce::String HearingPage::describeDose (const EngineController::HearingInfo& info)
{
    juce::String t;
    if (! info.known && info.doseToday <= 0.0 && info.doseWeek <= 0.0)
        return "Unknown: without a sensitivity no dose is estimated. The reference (WHO / ITU-T H.870) is 80 dB(A) for 40 hours a week.";
    t << "Today about " << formatDosePercent (info.doseToday) << " of the weekly allowance, the last 7 days "
      << formatDosePercent (info.doseWeek) << " (the WHO / ITU-T H.870 reference: 80 dB(A) for 40 hours a week; every 3 dB louder "
      << "halves the time).";
    if (info.doseWeek >= 1.0)
        t << " That is more than the reference allowance for a week: listening quieter or for less time gives the ears a rest.";
    if (! info.known)
        t << " Not counting now: no sensitivity.";
    return t;
}

juce::String HearingPage::describeCap (const EngineController::HearingInfo& info)
{
    juce::String t ("Holds the estimated level over any 5 s at or under the cap by turning the output down slowly (no distortion, no "
                    "latency). Off by default.");
    if (! info.known)
        return t + " It needs a sensitivity: without one it does nothing.";
    if (info.capEnabled && info.capActive)
        t << " Holding the level down now by " << juce::String (-info.capGainDb, 1) << " dB.";
    return t;
}

void HearingPage::refresh()
{
    const auto info = controller.getHearing();
    {
        const juce::ScopedValueSetter<bool> guard (refreshing, true);
        ownFigure.setEnabled (info.output.isNotEmpty());
        ownFigure.setToggleState (info.userDbSpl.has_value(), juce::dontSendNotification);
        // Prefilled from the profile: the value switching the own figure on starts from.
        const float shownFigure = info.userDbSpl.value_or (std::isnan (info.profileDbSpl) ? kStartingFigureDbSpl : info.profileDbSpl);
        if (! sensitivity.isMouseButtonDown())
            sensitivity.setValue (shownFigure, juce::dontSendNotification);
        sensitivity.setEnabled (info.userDbSpl.has_value());
        capToggle.setToggleState (info.capEnabled, juce::dontSendNotification);
        if (! capSlider.isMouseButtonDown())
            capSlider.setValue (info.capDbA, juce::dontSendNotification);
        capSlider.setEnabled (info.capEnabled);
    }
    const auto s = describeSensitivity (info), e = describeEstimate (info), d = describeDose (info), cap = describeCap (info);
    if (s != sensitivityText || e != estimateText || d != doseText || cap != capText)
    {
        const bool relayout = textHeight (s, getWidth() - kCaptionWidth) != textHeight (sensitivityText, getWidth() - kCaptionWidth)
                              || textHeight (e, getWidth() - kCaptionWidth) != textHeight (estimateText, getWidth() - kCaptionWidth)
                              || textHeight (d, getWidth()) != textHeight (doseText, getWidth())
                              || textHeight (cap, getWidth() - kCaptionWidth) != textHeight (capText, getWidth() - kCaptionWidth);
        sensitivityText = s;
        estimateText = e;
        doseText = d;
        capText = cap;
        if (relayout)
            resized();
        repaint();
    }
    profileEditor.refresh();
}

void HearingPage::paint (juce::Graphics& g)
{
    g.setColour (Palette::faint.brighter (0.2f));
    g.setFont (Theme::font (12.0f));
    g.drawFittedText ("Flubsound can estimate how loud your headset plays from the sound it sends, the system volume and the headset's "
                      "sensitivity, and keep a daily dose against the WHO reference. It is an estimate for your information, not a "
                      "measurement, a hearing test or a medical device.",
                      introArea, juce::Justification::topLeft, 4, 1.0f);

    drawSectionTitle (g, levelTitle, "Listening level");
    g.setColour (Palette::text.withAlpha (0.88f));
    g.setFont (Theme::font (13.0f));
    g.drawText ("Sensitivity", sensitivityCaption, juce::Justification::centredLeft, true);
    g.drawText ("Estimate", estimateCaption, juce::Justification::topLeft, true);
    g.setFont (Theme::font (11.5f));
    const bool unknown = sensitivityText.startsWith ("Unknown");
    g.setColour (unknown ? Palette::amber.withAlpha (0.9f) : Palette::faint.brighter (0.2f));
    g.drawFittedText (sensitivityText, sensitivityHelp, juce::Justification::topLeft, 5, 1.0f);
    g.setColour (estimateText.startsWith ("Unknown") ? Palette::faint : Palette::text.withAlpha (0.85f));
    g.drawFittedText (estimateText, estimateArea, juce::Justification::topLeft, 5, 1.0f);

    drawSectionTitle (g, capTitle, "Listening-level cap");
    g.setColour (Palette::text.withAlpha (0.88f));
    g.setFont (Theme::font (13.0f));
    g.drawText ("Cap level", capCaption, juce::Justification::centredLeft, true);
    g.setColour (Palette::faint.brighter (0.2f));
    g.setFont (Theme::font (11.5f));
    g.drawFittedText (capText, capHelp, juce::Justification::topLeft, 4, 1.0f);

    drawSectionTitle (g, doseTitle, "Estimated dose");
    g.setColour (doseText.startsWith ("Unknown") ? Palette::faint : Palette::text.withAlpha (0.85f));
    g.setFont (Theme::font (11.5f));
    g.drawFittedText (doseText, doseArea, juce::Justification::topLeft, 5, 1.0f);

    drawSectionTitle (g, profileTitle, "Personal profile (per ear)");
}

void HearingPage::resized()
{
    const int width = getWidth();
    auto r = getLocalBounds().withHeight (100000);
    introArea = r.removeFromTop (textHeight ("x", width) * 3);
    r.removeFromTop (10);
    levelTitle = r.removeFromTop (22);
    r.removeFromTop (8);
    ownFigure.setBounds (r.removeFromTop (kRowHeight).withWidth (juce::jmin (width, 420)).reduced (0, 2));
    {
        auto row = r.removeFromTop (kRowHeight);
        sensitivityCaption = row.removeFromLeft (kCaptionWidth);
        sensitivity.setBounds (row.withWidth (juce::jmin (row.getWidth(), 360)).reduced (0, 3));
    }
    sensitivityHelp = r.removeFromTop (textHeight (sensitivityText, width - kCaptionWidth)).withTrimmedLeft (kCaptionWidth);
    r.removeFromTop (6);
    estimateArea = r.removeFromTop (textHeight (estimateText, width - kCaptionWidth));
    estimateCaption = estimateArea.removeFromLeft (kCaptionWidth);

    r.removeFromTop (12);
    capTitle = r.removeFromTop (22);
    r.removeFromTop (8);
    capToggle.setBounds (r.removeFromTop (kRowHeight).withWidth (juce::jmin (width, 420)).reduced (0, 2));
    {
        auto row = r.removeFromTop (kRowHeight);
        capCaption = row.removeFromLeft (kCaptionWidth);
        capSlider.setBounds (row.withWidth (juce::jmin (row.getWidth(), 360)).reduced (0, 3));
    }
    capHelp = r.removeFromTop (textHeight (capText, width - kCaptionWidth)).withTrimmedLeft (kCaptionWidth);

    r.removeFromTop (12);
    doseTitle = r.removeFromTop (22);
    r.removeFromTop (8);
    doseArea = r.removeFromTop (textHeight (doseText, width));

    r.removeFromTop (12);
    profileTitle = r.removeFromTop (22);
    r.removeFromTop (8);
    profileEditor.setBounds (r.removeFromTop (PersonalProfileEditor::kPreferredHeight));
    if (const int h = profileEditor.getBottom() + 8; h != getHeight())
        setSize (width, h); // the dialog scrolls the page
}
} // namespace flub::app::ui
