// Flubsound Pro - main window content.
//
// Layout (scales from 1100 x 700 to 2560 x 1440, proportional with limits):
//
//   +--------------------------------------------------------------------+
//   | HeaderBar: logo, mode, strip, presets, A/B, bypass, latency, gear  |
//   +-----------+----------------------------------------+---------------+
//   | Routing   | BoostPanel (Boost Intensity + macros)  | LevelMeters   |
//   | Panel     +----------------------------------------+               |
//   | (strips,  | AnalyzerPanel: SpectrumAnalyzer +      +---------------+
//   |  gain,    |   EqCurveEditor (largest area)         | LoudnessPanel |
//   |  apps)    +----------------------------------------+               |
//   |           | ModuleRack (scrolling cards)           |               |
//   +-----------+----------------------------------------+---------------+
//   | WaveformHistory (output envelope + short-term LUFS)                |
//   +--------------------------------------------------------------------+
//
// Refresh model: one VBlankAttachment callback per display frame reads the
// selected strip's MeterBus into a MeterSnapshot, drains the analyser taps
// (AnalyzerFeed is their single consumer) and advances every view; views
// repaint only what changed and paint() never does analysis work. Controls
// are bound to the ParameterStore through ParameterBinders (30 Hz version
// polling); structural events arrive through EngineController::Listener. The
// accent colour follows the selected strip's mode.
//
// Contract with the shell: namespace flub::app::ui, constructible from an
// EngineController&, owned by MainWindow.
#pragma once

#include "AnalyzerFeed.h"
#include "AnalyzerPanel.h"
#include "BoostPanel.h"
#include "FlubLookAndFeel.h"
#include "HeaderBar.h"
#include "LevelMeters.h"
#include "LoudnessPanel.h"
#include "MeterSnapshot.h"
#include "ModuleRack.h"
#include "RoutingPanel.h"
#include "SettingsDialog.h"
#include "WaveformHistory.h"
#include "engine/EngineController.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace flub::app::ui
{
class MainComponent final : public juce::Component, private EngineController::Listener
{
public:
    explicit MainComponent (EngineController& controller);
    ~MainComponent() override;

    /** Gives the settings dialog access to the application's hotkey manager. */
    void setHotkeyHooks (HotkeyHooks hooks) { hotkeyHooks = std::move (hooks); }

    void paint (juce::Graphics& g) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;

private:
    void frame (double timestampSeconds);
    void engineControllerChanged (EngineController::Change change) override;
    void applyMode (flub::param::ModeValue mode);
    void resetAnalysis();
    void requestLoudnessReset();
    void openSettings();
    juce::String currentStripSignature() const;
    void loadUiPreferences();
    void saveUiPreferences();
    FlubLookAndFeel& lookAndFeel();

    EngineController& controller;
    std::unique_ptr<FlubLookAndFeel> ownLookAndFeel; // only when the app default is not a FlubLookAndFeel

    HeaderBar header;
    RoutingPanel routing;
    BoostPanel boost;
    AnalyzerPanel analyzer;
    ModuleRack rack;
    LevelMeters levels;
    LoudnessPanel loudness;
    WaveformHistory history;
    std::unique_ptr<juce::TooltipWindow> tooltips; // none in headless screenshot runs

    AnalyzerFeed feed;
    MeterSnapshot snapshot;
    HotkeyHooks hotkeyHooks;
    std::unique_ptr<juce::VBlankAttachment> vblank;
    juce::Component::SafePointer<juce::DialogWindow> settingsWindow;
    juce::String stripSignature; // names + channel counts: rebuild strip views only when it changes

    double lastFrameTime = -1.0;
    int frameCounter = 0, lastStrip = -1;
    uint32_t lastGeneration = 0;
    flub::param::ModeValue mode = flub::param::ModeValue::Music;
    bool modeKnown = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
} // namespace flub::app::ui
