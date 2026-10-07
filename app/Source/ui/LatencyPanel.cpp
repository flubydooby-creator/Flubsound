#include "LatencyPanel.h"

#include "Theme.h"
#include "Widgets.h"

#include <algorithm>
#include <cmath>

namespace flub::app::ui
{
namespace
{
constexpr int kToggleH = 26, kRowH = 30, kGap = 6;
constexpr float kTextSize = 12.0f;

juce::String ms (int samples, double rate)
{
    return juce::String (rate > 0.0 ? 1000.0 * samples / rate : 0.0, 1) + " ms";
}

juce::String profileName (flub::param::LatencyProfileValue p)
{
    switch (p)
    {
        case flub::param::LatencyProfileValue::Quality: return "Quality";
        case flub::param::LatencyProfileValue::LowLatency: return "Low Latency";
        case flub::param::LatencyProfileValue::Balanced: break;
    }
    return "Balanced";
}

void drawSectionTitle (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title)
{
    Theme::drawCaption (g, title.toUpperCase(), area.toFloat(), Palette::muted);
    g.setColour (Palette::border);
    g.fillRect (area.getX(), area.getBottom() - 1, area.getWidth(), 1);
}

const char* const kIntro = "The buffer is the device's share of the delay. Measuring plays short test sweeps on the output and records the "
                           "input at the same time, so the round trip is measured, not estimated.";
} // namespace

LatencyPanel::LatencyPanel (EngineController& c)
    : controller (c)
{
    setTitle ("Latency");

    Style::set (automaticToggle, "switch");
    automaticToggle.setTitle ("Automatic buffer size");
    automaticToggle.setTooltip ("On: Low Latency asks the device for its smallest buffer (at least 1.3 ms), Balanced for about 5 ms, "
                                "Quality for its default; dropouts raise it (to at least twice the size). Off: the buffer size chosen in the list above stays.");
    automaticToggle.onClick = [this]
    {
        controller.setAutomaticBufferSize (automaticToggle.getToggleState());
        refresh();
    };
    addAndMakeVisible (automaticToggle);

    modeBox.addItem ("Device only", 1);
    modeBox.addItem ("Through Flubsound", 2);
    modeBox.addItem ("Both: device only, then through Flubsound", 3);
    modeBox.setSelectedId (3, juce::dontSendNotification);
    modeBox.setTitle ("What to measure");
    modeBox.setTooltip ("Device only: the device's own round trip. Through Flubsound: the sweep plays through the selected strip, so the "
                        "engine's latency is in it. Both: the two, one after the other, and the engine's share as their difference.");
    modeBox.onChange = [this] { refresh(); };
    addAndMakeVisible (modeBox);

    measureButton.setTooltip ("Plays short test sweeps at -24 dBFS on the output and records the input (asks first)");
    measureButton.onClick = [this] { askToStart(); };
    addAndMakeVisible (measureButton);
    cancelButton.setTooltip ("Stop the measurement (the sweep fades out at once)");
    cancelButton.onClick = [this]
    {
        controller.cancelLatencyMeasurement();
        refresh();
    };
    addAndMakeVisible (cancelButton);
    refresh();
}

EngineController::LatencyMode LatencyPanel::getMode() const noexcept
{
    switch (modeBox.getSelectedId())
    {
        case 1: return EngineController::LatencyMode::DeviceOnly;
        case 2: return EngineController::LatencyMode::ThroughFlubsound;
        default: break;
    }
    return EngineController::LatencyMode::Both;
}

void LatencyPanel::refresh()
{
    automaticToggle.setToggleState (controller.getAutomaticBufferSize(), juce::dontSendNotification);
    const auto info = controller.getBufferInfo();
    automaticToggle.setEnabled (! info.deviceOpen || info.managed);

    const auto strip = controller.getStripName (controller.getSelectedStrip());
    if (strip != stripName)
    {
        stripName = strip;
        modeBox.changeItemText (2, "Through Flubsound (" + strip + " strip)");
        modeBox.changeItemText (3, "Both: device only, then through Flubsound (" + strip + " strip)");
    }

    const auto& state = controller.getLatencyMeasurement();
    const bool busy = state.phase == LatencyMeasurer::Phase::Running || state.phase == LatencyMeasurer::Phase::Analysing;
    const auto whyNot = busy ? juce::String() : controller.whyCannotMeasureLatency (getMode());
    measureButton.setEnabled (! busy && whyNot.isEmpty());
    cancelButton.setEnabled (state.phase == LatencyMeasurer::Phase::Running);
    modeBox.setEnabled (! busy);

    const auto bufferLine = describeBuffer (info, controller.getLatencyInfo(), controller.getBufferBackoffSteps());
    const auto text = describeMeasurement (state, whyNot);
    const bool warning = state.phase == LatencyMeasurer::Phase::Failed
                         || (state.phase == LatencyMeasurer::Phase::Idle && whyNot.isNotEmpty())
                         || (state.phase == LatencyMeasurer::Phase::Done
                             && ((state.through && state.through->confidence <= latency::Confidence::Low)
                                 || (state.deviceOnly && state.deviceOnly->confidence <= latency::Confidence::Low)));
    if (bufferLine != bufferText || text != measurementText || warning != measurementWarning)
    {
        bufferText = bufferLine;
        measurementText = text;
        measurementWarning = warning;
        resized();
        repaint();
    }
}

juce::String LatencyPanel::describeBuffer (const AudioEngineHost::BufferInfo& info, const LatencyInfo& reported, uint64_t backoffSteps)
{
    if (! info.deviceOpen)
        return "No audio device is open.";
    const double rate = info.sampleRate;
    juce::String s;
    s << "Buffer " << info.current << " samples (" << ms (info.current, rate) << ") at " << juce::String (rate / 1000.0, 1) << " kHz";
    if (! info.managed)
    {
        if (info.deviceTypeName == "Windows Audio")
            s << ". Plain Windows Audio (shared mode) always runs at the system's period: choose \"Windows Audio (Low Latency Mode)\" "
                 "above for a smaller one, where the device allows it.";
        else
            s << ". " << info.deviceTypeName << " runs in the audio server's quantum, which the latency profile requests.";
    }
    else if (! info.automatic)
    {
        s << ". Automatic is off: the size chosen in the list above stays (the device offers " << info.smallest << " .. " << info.largest
          << " samples).";
    }
    else
    {
        switch (info.reason)
        {
            case buffer::Reason::OnlyOne:
                s << ". The device offers only this size in this mode (its driver has no smaller period), so every latency profile uses it.";
                break;
            case buffer::Reason::Smallest:
                s << ". " << profileName (info.profile) << ": the smallest size the device offers of at least 1.3 ms (it offers "
                  << info.smallest << " .. " << info.largest << ").";
                break;
            case buffer::Reason::NearFiveMs:
                s << ". " << profileName (info.profile) << ": the size nearest to 5 ms the device offers (" << info.smallest << " .. "
                  << info.largest << ").";
                break;
            case buffer::Reason::Floor:
                s << ". Raised to " << info.floor << " samples after dropouts; switch Automatic off and on to try the profile's size again.";
                break;
            case buffer::Reason::DeviceDefault:
                s << ". " << profileName (info.profile) << ": the device's default size.";
                break;
        }
        if (info.target != info.current)
            s << " (Asked for " << info.target << " samples; the device runs " << info.current << ".)";
    }
    if (backoffSteps > 0)
        s << " Raised " << static_cast<juce::int64> (backoffSteps) << (backoffSteps == 1 ? " time" : " times") << " this session after dropouts.";
    if (reported.valid)
        s << "\nReported (an estimate): device out " << juce::String (reported.deviceOutputMs, 1) << " ms + engine "
          << juce::String (reported.engineMs, 1) << " ms + device in " << juce::String (reported.deviceInputMs, 1)
          << " ms. Converters, USB / wireless links and the driver's own buffering are not in these figures: measure them below.";
    return s;
}

juce::String LatencyPanel::describeMeasurement (const LatencyMeasurer::State& state, const juce::String& whyNot)
{
    using Phase = LatencyMeasurer::Phase;
    switch (state.phase)
    {
        case Phase::Running:
        {
            juce::String s ("Measuring");
            if (state.passes > 1)
                s << " (pass " << state.pass << " of " << state.passes << ": "
                  << (state.pass == 1 && state.mode == LatencyMeasurer::Mode::Both ? "device only" : "through Flubsound") << ")";
            s << "... " << juce::roundToInt (std::ceil (state.secondsLeft)) << " s left. Keep the microphone at the earcup and the room quiet.";
            return s;
        }
        case Phase::Analysing: return "Analysing the recording...";
        case Phase::Done: return state.describe();
        case Phase::Failed: return "Not measured: " + state.error;
        case Phase::Idle: break;
    }
    if (whyNot.isNotEmpty())
        return whyNot;
    return "Plays 5 short sweeps per pass (about 7 s, -24 dBFS) on the output and records the input in the same moment. Hold the "
           "headset's microphone against an earcup, or connect a cable from an output to a line input. The result: the round trip, how "
           "it splits, the playback latency you hear and how sure the measurement is.";
}

juce::String LatencyPanel::confirmationText (const juce::String& output, const juce::String& input, EngineController::LatencyMode mode,
                                              double seconds)
{
    juce::String s;
    s << "Flubsound will play short test sweeps (a rising tone, about " << juce::roundToInt (seconds) << " s in all, at -24 dBFS";
    if (mode != EngineController::LatencyMode::DeviceOnly)
        s << "; through Flubsound never above -18 dBFS";
    s << ") on \"" << output << "\" and record \"" << input << "\".\n\n"
      << "Before you start:\n"
      << "- Turn the volume down to a comfortable level.\n"
      << "- Hold the headset's microphone against an earcup, or connect a cable from the output to a line input.\n"
      << "- Turn off microphone monitoring (sidetone) if the headset has it.\n"
      << "- Keep the room quiet. Other sound stops while it measures; Cancel stops it at any time.";
    return s;
}

void LatencyPanel::askToStart()
{
    const auto setup = controller.getDeviceManager().getAudioDeviceSetup();
    const auto mode = getMode();
    const double seconds = LatencyMeasurer::durationSeconds (mode, controller.getBufferInfo().sampleRate);
    const auto message = confirmationText (setup.outputDeviceName, setup.inputDeviceName, mode, seconds);
    juce::Component::SafePointer<LatencyPanel> safe (this);
    confirmBox = juce::AlertWindow::showScopedAsync (juce::MessageBoxOptions()
                                                         .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                                         .withTitle ("Measure latency")
                                                         .withMessage (message)
                                                         .withButton ("Start")
                                                         .withButton ("Cancel")
                                                         .withAssociatedComponent (this),
                                                     [safe] (int result)
                                                     {
                                                         if (safe != nullptr && result == 1)
                                                             safe->startConfirmed();
                                                     });
}

void LatencyPanel::startConfirmed()
{
    const auto error = controller.startLatencyMeasurement (getMode());
    if (error.isNotEmpty())
    {
        measurementText = "Not started: " + error;
        measurementWarning = true;
        resized();
        repaint();
        return;
    }
    refresh();
}

juce::TextLayout LatencyPanel::layoutText (const juce::String& text, int width, float size, juce::Colour colour) const
{
    juce::AttributedString s;
    s.setWordWrap (juce::AttributedString::byWord);
    s.setLineSpacing (2.0f);
    s.append (text, Theme::font (size), colour);
    juce::TextLayout layout;
    layout.createLayout (s, static_cast<float> (juce::jmax (80, width)));
    return layout;
}

int LatencyPanel::getHeightForWidth (int width) const
{
    const int w = juce::jmax (80, width - 24);
    const int intro = static_cast<int> (std::ceil (layoutText (kIntro, w, 11.5f, Palette::faint).getHeight()));
    const int bufferH = static_cast<int> (std::ceil (layoutText (bufferText, w, kTextSize, Palette::text).getHeight()));
    const int measuredH = static_cast<int> (std::ceil (layoutText (measurementText, w, kTextSize, Palette::text).getHeight()));
    return 22 + 8 + intro + kGap + kToggleH + 4 + bufferH + 10 + kRowH + kGap + measuredH + 12;
}

void LatencyPanel::resized()
{
    auto r = getLocalBounds();
    const int w = juce::jmax (80, r.getWidth() - 24);
    titleArea = r.removeFromTop (22);
    r.removeFromTop (8);
    const int introH = static_cast<int> (std::ceil (layoutText (kIntro, w, 11.5f, Palette::faint).getHeight()));
    introArea = r.removeFromTop (introH).reduced (12, 0);
    r.removeFromTop (kGap);
    automaticToggle.setBounds (r.removeFromTop (kToggleH).reduced (12, 0).withWidth (juce::jmin (w, 520)));
    r.removeFromTop (4);
    bufferLayout = layoutText (bufferText, w, kTextSize, Palette::text.withAlpha (0.85f));
    bufferArea = r.removeFromTop (static_cast<int> (std::ceil (bufferLayout.getHeight()))).reduced (12, 0);
    r.removeFromTop (10);
    auto row = r.removeFromTop (kRowH).reduced (12, 2);
    modeBox.setBounds (row.removeFromLeft (juce::jmin (360, juce::jmax (160, row.getWidth() - 260))));
    row.removeFromLeft (8);
    measureButton.setBounds (row.removeFromLeft (150));
    row.removeFromLeft (8);
    cancelButton.setBounds (row.removeFromLeft (90));
    r.removeFromTop (kGap);
    measurementLayout = layoutText (measurementText, w, kTextSize, measurementWarning ? Palette::amber : Palette::text.withAlpha (0.85f));
    measurementArea = r.removeFromTop (static_cast<int> (std::ceil (measurementLayout.getHeight()))).reduced (12, 0);
}

void LatencyPanel::paint (juce::Graphics& g)
{
    drawSectionTitle (g, titleArea.withTrimmedLeft (10), "Latency");
    layoutText (kIntro, introArea.getWidth(), 11.5f, Palette::faint.brighter (0.2f)).draw (g, introArea.toFloat());
    bufferLayout.draw (g, bufferArea.toFloat());
    measurementLayout.draw (g, measurementArea.toFloat());
}
} // namespace flub::app::ui
