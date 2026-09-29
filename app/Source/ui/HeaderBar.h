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
// * A/B: active bank of the selected strip + copy to the other bank. A
//   switch is loudness matched (docs/11 E37, BankComparison: the louder bank
//   is turned down to the quieter, from the first second, refined from the
//   strip's meters); the trim reads under the buttons ("B -3.1 dB"). Right-
//   click: the "Loudness-matched A/B" switch, copy, and the blind A/B/X test
//   (onBlindTestRequested; the owner opens AbxPanel over the window).
// * Bypass: master enable (every strip); it is loudness matched while the
//   "Loudness-matched bypass" parameter is on (right-click to change). While
//   bypassed, the line under it says how much louder the processed sound was
//   ("proc. +2.9 LU": short-term out minus in just before the bypass).
// * Latency (device + engine) and CPU readout (with the device's xrun count
//   when it reports one, and the CPU-overload watchdog's warning), settings
//   dialog. The latency total is marked "~" while it is an estimate (driver
//   figures, LatencyInfo::estimated) and reads "--" when no audible path
//   runs; its tooltip breaks it down per strip (own chain vs sync padding).
//   A device problem (docs/11 E51: a loopback pair holding the output at
//   silence, or a device error) turns the bottom line into a DEVICE warning
//   with the message in the tooltip; a click opens Settings.
// * TOURNAMENT badge (docs/11 E55), shown while Tournament mode is on (the
//   user's switch or an anti-cheat service): a pill after the strips from
//   kTournamentPillWidth up, below it an amber shield on the logo mark's
//   corner (the preset box keeps its room). Its tooltip says why and what
//   is paused; a click offers to switch it off and the automatic switch-on.
// * View: Simple <-> Advanced main window (docs/11 E39); the owner switches
//   (onViewToggleRequested) and tells the header which view is shown.
// * Narrow windows (< kNarrowWidth, docs/11 E39 reflow down to 800 px): the
//   wordmark, the strip buttons (a strip menu instead), the copy button and
//   the latency / CPU readout make way for an overflow button whose menu
//   holds the readout's lines, copy, the blind test, the routing panel
//   (onRoutingRequested) and Settings.
// Message thread only; refresh() pulls everything from the controller.
#pragma once

#include "Comparison.h"
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

    /** Below this width the header reflows (see the file comment). */
    static constexpr int kNarrowWidth = 1100;
    bool isNarrow() const noexcept { return narrow; }

    std::function<void()> onSettingsRequested;
    /** A/B menu > "Blind test (A/B/X)..." (and the overflow menu). */
    std::function<void()> onBlindTestRequested;
    /** Overflow menu > "Routing and strips" (narrow windows hide the panel). */
    std::function<void()> onRoutingRequested;

    /** The loudness estimator shared by the browser's preview, the matched
        A/B and the module listen (one per engine rate; its cache is kept). */
    std::shared_ptr<PresetLoudnessEstimator> getLoudnessEstimator();
    BankComparison& getComparison() noexcept { return *comparison; }
    /** Sets the "Loudness-matched A/B" switch (persisted; also the module listen's). */
    void setComparisonMatched (bool matched);
    std::function<void (bool matched)> onComparisonMatchedChanged;

    /** The line under Bypass while bypassed ("proc. +2.9 LU"; empty while
        not bypassed or unknown) and under A/B (BankComparison::shortText). */
    juce::String getBypassCaption() const;
    juce::String getAbCaption() const;
    /** Processed minus input loudness (LU) as last read while not bypassed. */
    static juce::String formatProcessedDelta (float lu);

    juce::Button& getBankButton (flub::param::Bank bank) noexcept;
    juce::Button& getOverflowButton() noexcept { return overflowButton; }
    juce::ComboBox& getStripBox() noexcept { return stripBox; }
    /** The TOURNAMENT badge (visible while Tournament mode is on, docs/11 E55);
        a pill from this width up, a shield on the logo below it. */
    static constexpr int kTournamentPillWidth = 1400;
    /** The TOURNAMENT badge (visible while Tournament mode is on, docs/11 E55). */
    juce::Button& getTournamentBadge() noexcept;
    /** Its tooltip: why it is on and what is paused. */
    static juce::String describeTournamentBadge (const juce::String& line);
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
    class TournamentBadge;

    void showPresetMenu();
    void showBypassMenu();
    void showCompareMenu (juce::Component& target);
    void showOverflowMenu();
    void showTournamentMenu();
    void updateTournamentBadge();
    void trackProcessedDelta();
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
    std::unique_ptr<PopupButton> abA, abB;
    juce::ComboBox stripBox; // narrow windows: the strip selector
    IconButton overflowButton { "More", Icons::more(), IconButton::Style::Framed };
    IconButton copyAB { "Copy to the other bank", Icons::copy(), IconButton::Style::Framed };
    std::unique_ptr<PopupButton> bypassButton;
    std::unique_ptr<TournamentBadge> tournamentBadge;
    IconButton settingsButton { "Settings", Icons::gear(), IconButton::Style::Framed };
    IconButton viewButton { "Advanced view", Icons::expand(), IconButton::Style::Framed };
    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<PresetBrowserOverlay> browserOverlay;
    std::shared_ptr<PresetLoudnessEstimator> loudnessEstimator; // kept between openings: its estimates are cached
    std::unique_ptr<BankComparison> comparison;                  // the matched A/B (docs/11 E37)

    std::vector<juce::String> presetIds; // combo item id - 1 -> preset id
    juce::Rectangle<int> logoArea, modeArea, stripArea, presetArea, abArea, readoutArea;
    juce::String latencyText;
    CpuReadout cpu;
    bool compact = false, narrow = false, presetModified = false, wideReadout = false;
    float processedDeltaLu = 0.0f;
    bool processedDeltaKnown = false;
    juce::String abCaption, bypassCaption, tournamentLine;
    std::vector<bool> stripActive;
    float thumbPos = 0.0f, thumbTarget = 0.0f; // 0 = Music, 1 = Gaming
};
} // namespace flub::app::ui
