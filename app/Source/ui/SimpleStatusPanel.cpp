#include "SimpleStatusPanel.h"

#include "DeviceAdviceBanner.h"
#include "Theme.h"
#include "Widgets.h"

#include <cmath>

namespace flub::app::ui
{
namespace
{
const juce::String kDot { juce::CharPointer_UTF8 (" \xc2\xb7 ") };
constexpr float kSilenceLufs = -70.0f;
} // namespace

SimpleStatusPanel::SimpleStatusPanel (EngineController& c) : controller (c)
{
    setTitle ("Output and loudness");
    outputButton.setTooltip ("Choose the output device and see all guidance for it (Settings > Audio)");
    outputButton.onClick = [this]
    {
        if (onOutputSettingsRequested != nullptr)
            onOutputSettingsRequested();
    };
    correctionButton.setTooltip ("Import or switch the headphone correction of this output (Settings > Correction)");
    correctionButton.onClick = [this]
    {
        if (onCorrectionRequested != nullptr)
            onCorrectionRequested();
    };
    Style::describe (advancedButton, "Advanced view", "Show every control: routing, spectrum and EQ, the module rack and every meter");
    advancedButton.onClick = [this]
    {
        if (onAdvancedRequested != nullptr)
            onAdvancedRequested();
    };
    for (auto* b : { &outputButton, &correctionButton, &advancedButton })
        addAndMakeVisible (b);
    refreshDevice (false);
}

// =============================================================================
// Readouts
// =============================================================================
SimpleStatusPanel::OutputStatus SimpleStatusPanel::describeOutput (const juce::String& deviceName, const juce::String& profileName,
                                                                   flub::device::Connection connection, const flub::device::Advice& advice,
                                                                   const EngineController::DeviceCorrectionInfo& correction,
                                                                   const DeviceSafetyState& safety, bool adviceShownElsewhere)
{
    OutputStatus s;
    s.recognised = profileName.isNotEmpty();
    s.problem = safety.kind != DeviceSafetyState::Kind::None;

    if (deviceName.isEmpty())
    {
        s.title = "No output device";
        s.detail = "Choose one in Output...";
    }
    else
    {
        s.title = s.recognised ? profileName : deviceName;
        juce::StringArray parts;
        parts.add (s.recognised ? deviceName : juce::String ("Generic output, no headset profile"));
        if (const auto ct = DeviceAdviceBanner::connectionText (connection); ct.isNotEmpty())
            parts.add (ct);
        parts.add ("ceiling " + juce::String (advice.ceilingDbTp, 1) + " dBTP");
        s.detail = parts.joinIntoString (kDot);
    }
    if (s.problem)
        s.detail = (safety.kind == DeviceSafetyState::Kind::LoopbackPair ? "Output muted: feedback loop" : "Audio device error") + kDot
                   + s.detail;

    if (correction.hasCurve)
        s.correction = "Headphone correction: " + correction.name + (correction.enabled ? " (on)" : " (off)");
    else
        s.correction = deviceName.isEmpty() ? juce::String() : juce::String ("No headphone correction");

    if (! adviceShownElsewhere && ! advice.messages.empty())
        s.advice = juce::String::fromUTF8 (advice.messages.front().c_str());
    return s;
}

juce::String SimpleStatusPanel::describeLoudnessChange (float inLufs, float outLufs, bool active)
{
    if (! active || ! std::isfinite (inLufs) || ! std::isfinite (outLufs) || inLufs <= kSilenceLufs || outLufs <= kSilenceLufs)
        return "No audio on this strip right now";
    const float delta = outLufs - inLufs;
    if (std::abs (delta) < 0.5f)
        return "About as loud as the input";
    return juce::String (std::abs (delta), 1) + (delta > 0.0f ? " LU louder than the input" : " LU quieter than the input");
}

// =============================================================================
// Updates
// =============================================================================
void SimpleStatusPanel::refreshDevice (bool adviceBannerShown)
{
    auto next = describeOutput (controller.getOutputDeviceName(), controller.getDeviceProfileName(), controller.getDeviceConnection(),
                                controller.getDeviceAdvice(), controller.getDeviceCorrection(), controller.getDeviceSafetyState(),
                                adviceBannerShown);
    const bool changed = next.title != output.title || next.detail != output.detail || next.correction != output.correction
                         || next.advice != output.advice || next.problem != output.problem;
    output = std::move (next);
    if (changed)
    {
        setDescription (output.title + ". " + output.detail + ". " + output.correction);
        repaint (outputCard);
    }
}

void SimpleStatusPanel::update (const MeterSnapshot& s, double dtSeconds)
{
    shown.in = s.inShortTermLufs;
    shown.out = s.shortTermLufs;
    shown.active = s.active;
    sinceRepaint += static_cast<float> (dtSeconds);
    if (sinceRepaint < 0.1f)
        return;
    sinceRepaint = 0.0f;
    if (std::abs (shown.out - painted.out) >= 0.05f || std::abs (shown.in - painted.in) >= 0.05f || shown.active != painted.active)
        repaint (loudnessCard);
}

// =============================================================================
// Layout and painting
// =============================================================================
void SimpleStatusPanel::resized()
{
    auto r = getLocalBounds();
    footer = r.removeFromBottom (34);
    r.removeFromBottom (10);
    const int gap = 10;
    outputCard = r.removeFromLeft ((r.getWidth() - gap) * 14 / 25); // 56 %
    r.removeFromLeft (gap);
    loudnessCard = r;

    advancedButton.setBounds (footer.removeFromRight (140).withSizeKeepingCentre (140, 30));

    auto buttons = outputCard.reduced (14, 12).removeFromBottom (28);
    outputButton.setBounds (buttons.removeFromRight (96));
    buttons.removeFromRight (8);
    correctionButton.setBounds (buttons.removeFromRight (juce::jmin (190, buttons.getWidth())));
}

void SimpleStatusPanel::paint (juce::Graphics& g)
{
    painted = shown;

    // ---- Output (headset status) ----
    {
        Theme::drawPanel (g, outputCard.toFloat());
        auto r = outputCard.reduced (14, 10);
        Theme::drawCaption (g, "OUTPUT", r.removeFromTop (18).toFloat());
        r.removeFromTop (6);
        r.removeFromBottom (28 + 8); // the buttons

        const auto status = Theme::statusColours (*this);
        const auto glyphColour = output.problem ? status.hot : output.recognised ? Theme::accent (*this) : Palette::muted;
        auto titleRow = r.removeFromTop (24);
        drawIcon (g, Icons::ear(), titleRow.removeFromLeft (24).toFloat().reduced (2.0f), glyphColour);
        titleRow.removeFromLeft (8);
        g.setFont (Theme::font (16.0f, true));
        g.setColour (Palette::text);
        g.drawText (output.title, titleRow, juce::Justification::centredLeft, true);
        r.removeFromTop (4);

        g.setFont (Theme::font (13.0f));
        g.setColour (output.problem ? status.hot : Palette::muted);
        const bool twoLines = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), output.detail) > static_cast<float> (r.getWidth());
        g.drawFittedText (output.detail, r.removeFromTop (twoLines ? 36 : 18), juce::Justification::topLeft, 2, 1.0f);
        r.removeFromTop (4);
        if (output.correction.isNotEmpty() && r.getHeight() >= 18)
        {
            g.setColour (Palette::muted);
            g.drawText (output.correction, r.removeFromTop (18), juce::Justification::centredLeft, true);
        }
        if (output.advice.isNotEmpty() && r.getHeight() >= 18)
        {
            r.removeFromTop (4);
            g.setColour (Palette::text.withAlpha (0.85f));
            g.drawFittedText (output.advice, r, juce::Justification::topLeft, juce::jmax (1, r.getHeight() / 17), 1.0f);
        }
    }

    // ---- Loudness (one meter) ----
    {
        Theme::drawPanel (g, loudnessCard.toFloat());
        auto r = loudnessCard.reduced (14, 10);
        Theme::drawCaption (g, "LOUDNESS", r.removeFromTop (18).toFloat());
        r.removeFromTop (8);

        const bool live = shown.active && shown.out > kSilenceLufs;
        g.setFont (Theme::numeric (26.0f));
        g.setColour (live ? Palette::text : Palette::faint);
        auto valueRow = r.removeFromTop (32);
        const auto value = live ? juce::String (shown.out, 1) : juce::String ("--");
        const int valueW = juce::roundToInt (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), value)) + 6;
        g.drawText (value, valueRow.removeFromLeft (valueW), juce::Justification::centredLeft, false);
        g.setFont (Theme::font (13.0f));
        g.setColour (Palette::muted);
        g.drawText ("LUFS short-term", valueRow, juce::Justification::centredLeft, true);
        r.removeFromTop (8);

        // The bar: output (accent) with the input as a marker.
        const auto bar = r.removeFromTop (14).toFloat();
        const auto x = [&bar] (float lufs)
        {
            const float t = juce::jlimit (0.0f, 1.0f, (lufs - kMeterMinLufs) / (kMeterMaxLufs - kMeterMinLufs));
            return bar.getX() + t * bar.getWidth();
        };
        g.setColour (Palette::well);
        g.fillRoundedRectangle (bar, 4.0f);
        g.setColour (Palette::border);
        g.drawRoundedRectangle (bar.reduced (0.5f), 4.0f, 1.0f);
        if (live)
        {
            g.setColour (Theme::accent (*this));
            g.fillRoundedRectangle (bar.withRight (x (shown.out)).reduced (0.0f, 2.0f), 3.0f);
        }
        if (shown.active && shown.in > kSilenceLufs)
        {
            g.setColour (Palette::text);
            g.fillRect (juce::Rectangle<float> (x (shown.in) - 1.0f, bar.getY() - 3.0f, 2.0f, bar.getHeight() + 6.0f));
        }
        auto scale = r.removeFromTop (16).toFloat();
        g.setFont (Theme::font (11.0f));
        g.setColour (Palette::faint);
        for (const float mark : { -30.0f, -20.0f, -10.0f, 0.0f })
        {
            const bool end = mark >= kMeterMaxLufs; // right-aligned inside the bar
            g.drawText (juce::String (juce::roundToInt (mark)),
                        juce::Rectangle<float> (x (mark) - (end ? 32.0f : 16.0f), scale.getY(), 32.0f, scale.getHeight()),
                        end ? juce::Justification::centredRight : juce::Justification::centred, false);
        }
        r.removeFromTop (6);

        g.setFont (Theme::font (13.0f));
        g.setColour (live ? Palette::text : Palette::muted);
        g.drawFittedText (describeLoudnessChange (shown.in, shown.out, shown.active), r.removeFromTop (36), juce::Justification::topLeft, 2,
                          1.0f);
        if (r.getHeight() >= 16)
        {
            g.setFont (Theme::font (12.0f));
            g.setColour (Palette::faint);
            g.drawFittedText ("The marker is the input's loudness.", r.removeFromTop (16), juce::Justification::topLeft, 1, 1.0f);
        }
    }

    // ---- Footer ----
    g.setFont (Theme::font (13.0f));
    g.setColour (Palette::muted);
    g.drawFittedText ("Spectrum and EQ, routing, the module rack and every meter are in the Advanced view.",
                      footer.withTrimmedRight (12), juce::Justification::centredRight, 1, 0.9f);
}
} // namespace flub::app::ui
