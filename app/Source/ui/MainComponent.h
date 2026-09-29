// Flubsound Pro - main window content.
//
// Layout (scales from 1100 x 700 to 2560 x 1440, proportional with limits):
//
//   +--------------------------------------------------------------------+
//   | HeaderBar: logo, mode, strip, presets, A/B, bypass, latency, gear  |
//   +--------------------------------------------------------------------+
//   | DeviceErrorBanner (only while the output is muted or failed, E51)  |
//   | DeviceAdviceBanner (only for a recognised headset / Bluetooth)     |
//   | NoticeBar (preset warnings, settings recovery, latency prompt)     |
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
// Simple view (docs/11 E39, the default; AppSettings::getMainView keeps the
// last choice): the header and banners as above, then the BoostPanel in its
// Simple layout (larger dial, the five macros, up to three rows of active-now
// chips) over a SimpleStatusPanel (headset / output status, one loudness
// meter, the Advanced view button), at most 1180 px wide and centred. The
// routing panel, analyser, module rack, meters and history are hidden; the
// header's view button and the panel's Advanced view button switch views.
//
//   +--------------------------------------------------------------------+
//   | HeaderBar (with the view button)                                   |
//   +--------------------------------------------------------------------+
//   | banners (as above)                                                 |
//   |        +--------------------------------------------------+        |
//   |        | BoostPanel, Simple layout: dial | 5 macros       |        |
//   |        |                                 | ACTIVE chips   |        |
//   |        +------------------------+-------------------------+        |
//   |        | OUTPUT (headset)       | LOUDNESS (one meter)    |        |
//   |        +------------------------+-------------------------+        |
//   |        | ... in the Advanced view.        [Advanced view] |        |
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
// UI scale and theme (Settings > General) are app-wide (Theme::applyUiScale,
// Theme::setTheme); the main component registers its window's design minimum
// (1100 x 700) so the window's minimum size never exceeds its screen at a
// large UI scale.
//
// Contract with the shell: namespace flub::app::ui, constructible from an
// EngineController&, owned by MainWindow.
#pragma once

#include "AnalyzerFeed.h"
#include "AnalyzerPanel.h"
#include "BoostPanel.h"
#include "DeviceAdviceBanner.h"
#include "export/ExportDialog.h"
#include "FlubLookAndFeel.h"
#include "HeaderBar.h"
#include "LevelMeters.h"
#include "LoudnessPanel.h"
#include "MeterSnapshot.h"
#include "ModuleRack.h"
#include "NoticeBanners.h"
#include "RoutingPanel.h"
#include "SettingsDialog.h"
#include "SimpleStatusPanel.h"
#include "WaveformHistory.h"
#include "engine/EngineController.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <optional>
#include <vector>

namespace flub::app::ui
{
class MainComponent final : public juce::Component, private EngineController::Listener
{
public:
    explicit MainComponent (EngineController& controller);
    ~MainComponent() override;

    /** Gives the settings dialog access to the application's hotkey manager. */
    void setHotkeyHooks (HotkeyHooks hooks) { hotkeyHooks = std::move (hooks); }

    /** The banners under the header (headless screenshots and tests). */
    DeviceErrorBanner& getDeviceErrorBanner() noexcept { return deviceError; }
    NoticeBar& getNoticeBar() noexcept { return notices; }
    BoostPanel& getBoostPanel() noexcept { return boost; }
    HeaderBar& getHeader() noexcept { return header; }
    SimpleStatusPanel& getSimplePanel() noexcept { return simple; }

    /** Simple or Advanced view (docs/11 E39). persist: remember the choice in
        the settings (every switch the user makes). */
    using View = AppSettings::MainView;
    void setView (View view, bool persist = true);
    View getView() const noexcept { return view; }
    /** The components of the Advanced view that the Simple view hides. */
    std::vector<juce::Component*> getAdvancedOnlyComponents();

    void paint (juce::Graphics& g) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;
    void parentHierarchyChanged() override;

private:
    void frame (double timestampSeconds);
    void engineControllerChanged (EngineController::Change change) override;
    void applyMode (flub::param::ModeValue mode);
    void resetAnalysis();
    void requestLoudnessReset();
    /** Opens Settings (on `page`, else the Audio page), or brings the open
        dialog to the front; a page also switches an open dialog to it. */
    void openSettings (std::optional<SettingsDialog::Page> page = std::nullopt);
    /** Opens the Export / batch process dialog, or brings it to the front. */
    void openExport();
    void refreshDeviceBanner();
    void layoutSimple (juce::Rectangle<int> area);
    /** Posts the preset reader warnings and the latency-profile suggestion
        the controller queued (Change::Preset). */
    void takePresetNotices();
    juce::String currentStripSignature() const;
    void loadUiPreferences();
    void saveUiPreferences();
    FlubLookAndFeel& lookAndFeel();

    EngineController& controller;
    std::unique_ptr<FlubLookAndFeel> ownLookAndFeel; // only when the app default is not a FlubLookAndFeel

    HeaderBar header;
    DeviceErrorBanner deviceError;
    DeviceAdviceBanner deviceBanner;
    NoticeBar notices;
    RoutingPanel routing;
    BoostPanel boost;
    AnalyzerPanel analyzer;
    ModuleRack rack;
    LevelMeters levels;
    LoudnessPanel loudness;
    WaveformHistory history;
    SimpleStatusPanel simple;
    View view = View::Advanced;
    std::unique_ptr<juce::TooltipWindow> tooltips; // none in headless screenshot runs
    bool screenshotRun = false;                    // headless --screenshot: exact size, no window limits

    AnalyzerFeed feed;
    MeterSnapshot snapshot;
    HotkeyHooks hotkeyHooks;
    std::unique_ptr<juce::VBlankAttachment> vblank;
    juce::Component::SafePointer<juce::DialogWindow> settingsWindow, exportWindow;
    juce::String stripSignature; // names + channel counts: rebuild strip views only when it changes

    double lastFrameTime = -1.0;
    int frameCounter = 0, lastStrip = -1;
    uint32_t lastGeneration = 0;
    flub::param::ModeValue mode = flub::param::ModeValue::Music;
    bool modeKnown = false;
    std::optional<EngineController::LatencySuggestion> shownSuggestion; // the latency prompt on the notice bar

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
} // namespace flub::app::ui
