// Flubsound Pro - main window content.
//
// PLACEHOLDER UI: a compact, functional panel that exercises the whole
// EngineController surface (strip selection, master enable, mode, boost,
// macros, presets, A/B, meters, analyser taps, latency, device settings). It
// exists so the shell / engine host can be verified end to end and will be
// replaced by the full UI. The only contract the replacement must keep:
//
//     namespace flub::app::ui { class MainComponent : public juce::Component
//     { public: explicit MainComponent (EngineController&); ... }; }
//
// See EngineController.h for the threading / lifetime rules (re-fetch
// getChain() every tick, single consumer of the analyser taps, ...).
#pragma once

#include "engine/EngineController.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>

namespace flub::app::ui
{
class MainComponent final : public juce::Component, private juce::Timer, private EngineController::Listener
{
public:
    explicit MainComponent (EngineController& controller);
    ~MainComponent() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    class MeterPanel;
    class SpectrumView;

    void timerCallback() override;
    void engineControllerChanged (EngineController::Change change) override;

    void rebuildStripButtons();
    void refreshControls();
    void refreshPresetList();
    void updateStatusText();
    void openAudioSettings();

    EngineController& controller;

    juce::TextButton enableButton, audioSettingsButton;
    juce::OwnedArray<juce::TextButton> stripButtons;
    juce::TextButton musicButton { "MUSIC" }, gamingButton { "GAMING" };
    juce::Slider boostSlider;
    juce::Label boostLabel;
    std::array<juce::Slider, 5> macroSliders;
    std::array<juce::Label, 5> macroLabels;
    juce::ComboBox presetBox;
    juce::TextButton previousPresetButton { "<" }, nextPresetButton { ">" }, abButton, copyAbButton { "Copy to other" };
    std::unique_ptr<MeterPanel> meters;
    std::unique_ptr<SpectrumView> spectrum;

    juce::String statusText;
    juce::Rectangle<int> headerArea, stripArea, leftArea, footerArea;
    uint32_t lastStoreVersion = 0, lastGeneration = 0;
    int lastSelectedStrip = -1, tick = 0;
    std::vector<juce::String> presetIdsByItem;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
} // namespace flub::app::ui
