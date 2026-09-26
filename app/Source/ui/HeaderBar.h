// Flubsound Pro - the header bar.
//
//   [logo Flubsound Pro] [Music | Gaming] [Game Music Chat System]
//        [< preset v >][...]  [A|B][copy]  [Bypass]  latency / CPU  [gear]
//
// * Mode switch: segmented control with a sliding accent thumb; the accent
//   of the whole UI follows the mode (teal = Music, magenta = Gaming).
// * Strip selector: which strip (per-app profile) the UI edits; a dot shows
//   strips that currently receive audio.
// * Preset browser: combo box grouped by factory / user category, previous
//   / next, and a menu with save, save as, rename, delete, import, export,
//   reveal folder and reset.
// * A/B: active bank of the selected strip + copy to the other bank.
// * Bypass: master enable (every strip); it is loudness matched while the
//   "Loudness-matched bypass" parameter is on (right-click to change).
// * Latency (device + engine) and CPU readout, settings dialog.
// Message thread only; refresh() pulls everything from the controller.
#pragma once

#include "SettingsDialog.h"
#include "Widgets.h"
#include "engine/EngineController.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace flub::app::ui
{
class HeaderBar : public juce::Component
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

    void paint (juce::Graphics& g) override;
    void paintOverChildren (juce::Graphics& g) override;
    void resized() override;

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
    juce::ComboBox presetBox;
    IconButton prevPreset { "Previous preset", Icons::chevronLeft(), IconButton::Style::Framed };
    IconButton nextPreset { "Next preset", Icons::chevronRight(), IconButton::Style::Framed };
    IconButton presetMenu { "Preset actions", Icons::more(), IconButton::Style::Framed };
    juce::TextButton abA { "A" }, abB { "B" };
    IconButton copyAB { "Copy to the other bank", Icons::copy(), IconButton::Style::Framed };
    std::unique_ptr<PopupButton> bypassButton;
    IconButton settingsButton { "Settings", Icons::gear(), IconButton::Style::Framed };
    std::unique_ptr<juce::FileChooser> fileChooser;

    std::vector<juce::String> presetIds; // combo item id - 1 -> preset id
    juce::Rectangle<int> logoArea, modeArea, stripArea, presetArea, abArea, readoutArea;
    juce::String latencyText, cpuText;
    bool cpuHot = false, compact = false, presetModified = false, deviceOpen = false;
    std::vector<bool> stripActive;
    float thumbPos = 0.0f, thumbTarget = 0.0f; // 0 = Music, 1 = Gaming
};
} // namespace flub::app::ui
