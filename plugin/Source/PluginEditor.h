// Flubsound FX - plug-in editor.
//
// For now this wraps juce::GenericAudioProcessorEditor (every parameter,
// grouped like flub::param::layout()) and adds a small toolbar:
//   * Import / Export of Flubsound preset JSON (*.flubpreset.json - the same
//     files the desktop app and flubsound-cli use)
//   * a telemetry line (latency, output loudness, true peak, limiter gain
//     reduction, Safety Governor) polled from the chain's MeterBus atomics.
//
// ROADMAP (docs/07-roadmap.md, item 2.9): replace the generic editor with the
// custom editor built from the desktop app's UI components (macro knobs,
// spectrum, meters), shared between app and plug-in.
#pragma once

#include "PluginProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace flub::plugin
{
class FlubsoundEditor final : public juce::AudioProcessorEditor,
                              private juce::Timer
{
public:
    explicit FlubsoundEditor (FlubsoundProcessor& processor);
    ~FlubsoundEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void importPreset();
    void exportPreset();
    void showMessage (const juce::String& title, const juce::String& text);

    FlubsoundProcessor& flubProcessor;

    juce::Label title, status;
    juce::TextButton importButton { "Import preset..." }, exportButton { "Export preset..." };
    juce::GenericAudioProcessorEditor generic;
    std::unique_ptr<juce::FileChooser> chooser; // alive while an async dialog is open

    static constexpr int kToolbarHeight = 40;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FlubsoundEditor)
};
} // namespace flub::plugin
