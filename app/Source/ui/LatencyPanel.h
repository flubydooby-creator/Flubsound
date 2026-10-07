// Flubsound Pro - Settings > Audio > LATENCY (docs/11 E42c / E42d; docs/06 §6.11).
//
//   Automatic buffer size  switch (EngineController::setAutomaticBufferSize,
//                          default on) and one line on the buffer: the size,
//                          why it is that size (the profile's choice, the
//                          device's only size, the back-off floor, or the
//                          user's pick with Automatic off) and the latency the
//                          device and the engine report.
//   Measure latency        Device only / Through Flubsound (<strip> strip) /
//                          Both (default); "Measure latency..." asks first
//                          (turn the volume down, hold the microphone at the
//                          earcup or use a cable, mic monitoring off), then
//                          starts EngineController::startLatencyMeasurement;
//                          Cancel fades the sweep out. The text under it:
//                          why it cannot start, the progress, the result
//                          (latency::describe / describeBoth) or why it failed.
//
// The Audio page refreshes it with the dialog's 2 Hz timer and places it
// under the device selector (getHeightForWidth).
#pragma once

#include "engine/EngineController.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace flub::app::ui
{
class LatencyPanel : public juce::Component
{
public:
    explicit LatencyPanel (EngineController& controller);

    void refresh();

    /** The height this panel needs at `width` (its text wraps). */
    int getHeightForWidth (int width) const;

    /** The buffer line (see the header). */
    static juce::String describeBuffer (const AudioEngineHost::BufferInfo& info, const LatencyInfo& reported, uint64_t backoffSteps);
    /** The measurement text: why not, progress, the result, or the error. */
    static juce::String describeMeasurement (const LatencyMeasurer::State& state, const juce::String& whyNot);
    /** The confirmation dialog's message. */
    static juce::String confirmationText (const juce::String& output, const juce::String& input, EngineController::LatencyMode mode,
                                          double seconds);
    /** The mode box's selection. */
    EngineController::LatencyMode getMode() const noexcept;

    // Controls and painted text (tests, screenshots).
    juce::ToggleButton& getAutomaticToggle() noexcept { return automaticToggle; }
    juce::ComboBox& getModeBox() noexcept { return modeBox; }
    juce::TextButton& getMeasureButton() noexcept { return measureButton; }
    juce::TextButton& getCancelButton() noexcept { return cancelButton; }
    const juce::String& getBufferText() const noexcept { return bufferText; }
    const juce::String& getMeasurementText() const noexcept { return measurementText; }
    bool isMeasurementTextAWarning() const noexcept { return measurementWarning; }
    /** Starts without the confirmation (the dialog's Start; tests). */
    void startConfirmed();

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void askToStart();
    juce::TextLayout layoutText (const juce::String& text, int width, float size, juce::Colour colour) const;

    EngineController& controller;
    juce::ToggleButton automaticToggle { "Automatic buffer size (follows the latency profile)" };
    juce::ComboBox modeBox;
    juce::TextButton measureButton { "Measure latency..." }, cancelButton { "Cancel" };
    juce::String bufferText, measurementText, stripName;
    bool measurementWarning = false;
    juce::TextLayout bufferLayout, measurementLayout;
    juce::Rectangle<int> titleArea, introArea, bufferArea, measurementArea;
    juce::ScopedMessageBox confirmBox;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LatencyPanel)
};
} // namespace flub::app::ui
