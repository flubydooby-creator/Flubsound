// Flubsound Pro - the header bar.
//
//   [logo Flubsound Pro] [Music | Gaming] [Game Music Chat System]
//        [< preset v >][...]  [A|B][copy]  [Bypass]  latency / CPU  [view][gear]
//
// * Mode switch: segmented control with a sliding accent thumb; the accent
//   of the whole UI follows the mode (teal = Music, magenta = Gaming).
// * Strip selector: which strip (per-app profile) the UI edits; a dot shows
//   strips that currently receive audio.
// * Presets: the preset box names the strip's preset; a click on it (or
//   Space / Enter) opens the preset browser (PresetBrowser: search, filters,
//   favourites, preview, docs/11 E40) over the window; the arrow keys and
//   previous / next step through the list; a menu with browse, save, save
//   as, rename, delete, import, export, reveal folder, reset and "Export /
//   batch process audio files..." (the ExportDialog: render audio files with
//   these settings). A preset picked here or in the browser is recorded as
//   recent (AppSettings::addRecentPreset).
// * A/B: active bank of the selected strip + copy to the other bank.
// * Bypass: master enable (every strip); it is loudness matched while the
//   "Loudness-matched bypass" parameter is on (right-click to change).
// * Latency (device + engine) and CPU readout (with the device's xrun count
//   when it reports one, and the CPU-overload watchdog's warning), settings
//   dialog. The latency total is marked "~" while it is an estimate (driver
//   figures, LatencyInfo::estimated) and reads "--" when no audible path
//   runs; its tooltip breaks it down per strip (own chain vs sync padding).
//   A device problem (docs/11 E51: a loopback pair holding the output at
//   silence, or a device error) turns the bottom line into a DEVICE warning
//   with the message in the tooltip; a click opens Settings.
// * View: Simple <-> Advanced main window (docs/11 E39); the owner switches
//   (onViewToggleRequested) and tells the header which view is shown.
// Message thread only; refresh() pulls everything from the controller.
#pragma once

#include "PresetBrowser.h"
#include "SettingsDialog.h"
#include "Widgets.h"
#include "engine/EngineController.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace flub::app::ui
{
/** The header's preset box: its pop-up is the preset browser (onBrowse);
    without a handler, the plain list. */
class PresetBox final : public juce::ComboBox
{
public:
    std::function<void()> onBrowse;
    void showPopup() override
    {
        if (onBrowse != nullptr)
            onBrowse();
        else
            juce::ComboBox::showPopup();
    }
};

class HeaderBar : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit HeaderBar (EngineController& controller);
    ~HeaderBar() override;

    /** Mode / strip / preset / A-B / bypass state from the controller. */
    void refresh();
    /** Rebuilds the preset list (Change::Preset). */
    void refreshPresets();
    /** Rebuilds the strip selector (strip layout changed). */
    void rebuildStrips();
    /** Latency / CPU readout and strip activity dots (a few times per second). */
    void updateStatus();
    /** Per display frame: animates the mode switch thumb. */
    void animate (double dtSeconds);

    std::function<void()> onSettingsRequested;
    /** The view button: switch between the Simple and the Advanced view. */
    std::function<void()> onViewToggleRequested;
    /** The view shown (the button offers the other one). */
    void setSimpleView (bool simple);
    juce::Button& getViewButton() noexcept { return viewButton; }
    /** Preset menu > "Export / batch process audio files..." (ExportDialog). */
    std::function<void()> onExportRequested;

    /** Opens the preset browser over the parent (the main window's content),
        under the header; does nothing while it is open. */
    void showPresetBrowser();
    /** Closes it (a preview that was not loaded is cancelled). */
    void closePresetBrowser();
    /** The open browser, or nullptr. */
    PresetBrowser* getPresetBrowser() noexcept { return browserOverlay != nullptr ? &browserOverlay->getBrowser() : nullptr; }
    PresetBox& getPresetBox() noexcept { return presetBox; }

    /** The bottom line of the latency / CPU readout. */
    struct CpuReadout
    {
        juce::String caption;  // "CPU", "OVERLOAD" or "DEVICE" (hidden when compact)
        juce::String value;    // "42%", "42% . 3 xr" (middle dot; device xruns), "offline"
        bool warn = false;     // amber: load above 70 %
        bool overload = false; // hot: sustained overload (OverloadWatchdog)
    };
    /** A device safety state other than None (DeviceSafetyState) takes the
        line over: "DEVICE" + "muted" (loopback guard) or "error", hot. */
    static CpuReadout formatCpuReadout (const EngineStatus& status, const OverloadWatchdog::State& overload,
                                        const DeviceSafetyState& safety = {});

    /** The top line: "--" (!valid), "~12.3 ms" (estimated) or "12.3 ms". */
    static juce::String formatLatencyReadout (const LatencyInfo& info);
    /** The latency part of the tooltip: the total's parts, whether it is an
        estimate, and one line per strip with its own latency and the padding
        another strip's profile adds (stripNames[i] names strip i). */
    static juce::String describeLatency (const LatencyInfo& info, const juce::StringArray& stripNames);
    /** The device warning's tooltip line; empty for Kind::None. */
    static juce::String describeDeviceSafety (const DeviceSafetyState& safety);

    /** The CPU part of the readout's tooltip: load, device xruns, and the
        overload warning with the recommended action or the session count;
        then what the automatic overload response changed, if anything
        (EngineController::describeLoadReduction). */
    static juce::String describeCpu (const EngineStatus& status, const OverloadWatchdog::State& overload,
                                     const juce::String& loadReduction = {});

    void paint (juce::Graphics& g) override;
    void paintOverChildren (juce::Graphics& g) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;

private:
    class ModeSegment;
    class PopupButton;

    void showPresetMenu();
    void showBypassMenu();
    void saveAs();
    void renamePreset (const PresetInfo& preset);
    void deletePreset (const PresetInfo& preset);
    void importPreset();
    void exportPreset (const PresetInfo& preset);
    void resetStrip();
    void showError (const juce::String& title, const juce::String& message);
    const PresetInfo* currentPreset() const;

    EngineController& controller;

    std::unique_ptr<ModeSegment> musicSegment, gamingSegment;
    std::vector<std::unique_ptr<juce::TextButton>> stripButtons;
    PresetBox presetBox;
    IconButton prevPreset { "Previous preset", Icons::chevronLeft(), IconButton::Style::Framed };
    IconButton nextPreset { "Next preset", Icons::chevronRight(), IconButton::Style::Framed };
    IconButton presetMenu { "Preset actions", Icons::more(), IconButton::Style::Framed };
    juce::TextButton abA { "A" }, abB { "B" };
    IconButton copyAB { "Copy to the other bank", Icons::copy(), IconButton::Style::Framed };
    std::unique_ptr<PopupButton> bypassButton;
    IconButton settingsButton { "Settings", Icons::gear(), IconButton::Style::Framed };
    IconButton viewButton { "Advanced view", Icons::expand(), IconButton::Style::Framed };
    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<PresetBrowserOverlay> browserOverlay;
    std::shared_ptr<PresetLoudnessEstimator> loudnessEstimator; // kept between openings: its estimates are cached

    std::vector<juce::String> presetIds; // combo item id - 1 -> preset id
    juce::Rectangle<int> logoArea, modeArea, stripArea, presetArea, abArea, readoutArea;
    juce::String latencyText;
    CpuReadout cpu;
    bool compact = false, presetModified = false, wideReadout = false;
    std::vector<bool> stripActive;
    float thumbPos = 0.0f, thumbTarget = 0.0f; // 0 = Music, 1 = Gaming
};
} // namespace flub::app::ui
