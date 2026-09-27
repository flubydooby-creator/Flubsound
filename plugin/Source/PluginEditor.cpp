#include "PluginEditor.h"

#include "flub/common/Math.h"

// Header-only; shared with the desktop app so both use one preset folder.
#include "../../app/Source/settings/UserDataFolder.h"

#include <cmath>

namespace flub::plugin
{
namespace
{
constexpr int kDefaultWidth = 720;
constexpr int kDefaultHeight = 560;

juce::String formatDb (float v, const char* unit)
{
    if (! std::isfinite (v) || v <= flub::kMinusInfDb + 0.5f)
        return juce::String ("-inf ") + unit;
    return juce::String (v, 1) + " " + unit;
}

juce::File defaultPresetFolder()
{
    // Same user folder as the desktop app's PresetManager.
    return flub::app::userDataFolder().getChildFile ("Presets");
}
} // namespace

FlubsoundEditor::FlubsoundEditor (FlubsoundProcessor& p)
    : juce::AudioProcessorEditor (p), flubProcessor (p), generic (p)
{
    title.setText ("Flubsound FX", juce::dontSendNotification);
    title.setFont (juce::Font (juce::FontOptions (17.0f, juce::Font::bold)));
    addAndMakeVisible (title);

    status.setFont (juce::Font (juce::FontOptions (13.0f)));
    status.setJustificationType (juce::Justification::centredRight);
    status.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible (status);

    importButton.setTooltip ("Load a Flubsound preset (.flubpreset.json) into the plug-in");
    exportButton.setTooltip ("Save the current settings as a Flubsound preset (.flubpreset.json)");
    importButton.onClick = [this] { importPreset(); };
    exportButton.onClick = [this] { exportPreset(); };
    addAndMakeVisible (importButton);
    addAndMakeVisible (exportButton);

    addAndMakeVisible (generic);

    setResizable (true, true);
    setResizeLimits (520, 260, 2400, 1800);
    setSize (juce::jmax (kDefaultWidth, generic.getWidth()), kDefaultHeight);
    startTimerHz (10);
}

FlubsoundEditor::~FlubsoundEditor()
{
    stopTimer();
}

void FlubsoundEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId).darker (0.15f));
}

void FlubsoundEditor::resized()
{
    auto area = getLocalBounds();
    auto bar = area.removeFromTop (kToolbarHeight).reduced (8, 6);
    title.setBounds (bar.removeFromLeft (130));
    importButton.setBounds (bar.removeFromLeft (130));
    bar.removeFromLeft (6);
    exportButton.setBounds (bar.removeFromLeft (130));
    bar.removeFromLeft (8);
    status.setBounds (bar);
    generic.setBounds (area);
}

void FlubsoundEditor::timerCallback()
{
    const auto& m = flubProcessor.getMeters();
    constexpr auto rl = std::memory_order_relaxed;
    const float latencyMs = m.latencyMs.load (rl);
    const float shortTerm = m.shortTermLufs.load (rl);
    const float integrated = m.integratedLufs.load (rl);
    const float truePeak = m.outTruePeakMaxDb.load (rl);
    const float limiter = m.maxGainReductionDb.load (rl);
    const float governor = m.governorScale.load (rl);

    status.setText ("Latency " + juce::String (latencyMs, 1) + " ms  |  S " + formatDb (shortTerm, "LUFS") + "  I "
                        + formatDb (integrated, "LUFS") + "  |  TP " + formatDb (truePeak, "dBTP") + "  |  Limiter "
                        + juce::String (limiter, 1) + " dB  |  Governor " + juce::String (juce::roundToInt (governor * 100.0f)) + " %",
                    juce::dontSendNotification);
}

void FlubsoundEditor::showMessage (const juce::String& heading, const juce::String& text)
{
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, heading, text);
}

void FlubsoundEditor::importPreset()
{
    chooser = std::make_unique<juce::FileChooser> ("Import Flubsound preset", defaultPresetFolder(), "*.json");
    const auto chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
    juce::Component::SafePointer<FlubsoundEditor> safeThis (this);
    chooser->launchAsync (chooserFlags, [safeThis] (const juce::FileChooser& fc) {
        if (safeThis == nullptr)
            return;
        const auto file = fc.getResult();
        if (file == juce::File())
            return; // cancelled
        juce::String error;
        juce::StringArray warnings;
        if (! safeThis->flubProcessor.importPreset (file, error, &warnings))
            safeThis->showMessage ("Import failed", error);
        else if (! warnings.isEmpty())
            safeThis->showMessage ("Imported with warnings", file.getFileName() + ":\n" + warnings.joinIntoString ("\n"));
    });
}

void FlubsoundEditor::exportPreset()
{
    const auto folder = defaultPresetFolder();
    folder.createDirectory();
    chooser = std::make_unique<juce::FileChooser> ("Export Flubsound preset", folder.getChildFile ("My Preset.flubpreset.json"), "*.json");
    const auto chooserFlags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                       | juce::FileBrowserComponent::warnAboutOverwriting;
    juce::Component::SafePointer<FlubsoundEditor> safeThis (this);
    chooser->launchAsync (chooserFlags, [safeThis] (const juce::FileChooser& fc) {
        if (safeThis == nullptr)
            return;
        auto file = fc.getResult();
        if (file == juce::File())
            return; // cancelled
        if (! file.getFileName().endsWithIgnoreCase (".json"))
            file = file.withFileExtension ("flubpreset.json");
        juce::String error;
        if (! safeThis->flubProcessor.exportPreset (file, error))
            safeThis->showMessage ("Export failed", error);
    });
}
} // namespace flub::plugin
