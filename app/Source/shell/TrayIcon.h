// Flubsound Pro - system tray / menu-bar icon.
//
// Left click (Windows / Linux): the quick-controls flyout (ui::QuickControls,
// docs/11 E39: Boost, the preset stepper, Bypass, open the window); a double
// click opens the main window. Right click (any click on macOS): quick menu
//   Enabled | Mode: Music / Gaming | Boost +10 % / -10 % | Presets > ... |
//   Tournament mode | Tournament mode > automatic | [Hotkeys not active] |
//   Quick controls | Open Flubsound Pro | Quit
// "N hotkeys not active - fix..." appears only while hotkeys failed to
// register (R4.4) and opens Settings > Hotkeys.
// Mode, boost and presets act on the selected strip. Tournament mode
// (docs/11 E55) is the user's switch; when it comes on by itself because an
// anti-cheat service runs, a bubble says so once. The icon is drawn in
// code: white equaliser bars on a fixed teal-to-blue gradient while enabled,
// dimmed bars on grey while bypassed; the macOS template variant is the bars
// alone.
#pragma once

#include "engine/EngineController.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>

namespace flub::app
{
class TrayIcon final : public juce::SystemTrayIconComponent, private EngineController::Listener
{
public:
    struct Callbacks
    {
        std::function<void()> openWindow;
        std::function<void()> quit;
        /** Opens Settings > Hotkeys (the "hotkeys not active" item); optional. */
        std::function<void()> openHotkeySettings;
        /** How many hotkeys are not active (HotkeyManager::getFailureList);
            optional, no item without it. */
        std::function<int()> countHotkeyProblems;
    };

    TrayIcon (EngineController& controller, Callbacks callbacks);
    ~TrayIcon() override;

    /** Short notification near the icon (where the OS supports it). */
    void notify (const juce::String& title, const juce::String& message);

    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    /** Opens the quick-controls flyout next to the icon (or at the mouse). */
    void showQuickControls();

    static juce::Image createIconImage (bool enabled, bool asTemplate, int size = 64);

    /** The quick menu's Tournament mode items (docs/11 E55): the switch
        (ticked while active; named after the anti-cheat that holds it on)
        and the automatic switch-on. */
    static void addTournamentItems (juce::PopupMenu& menu, EngineController& controller);

    /** The quick menu (above) for the controller's selected strip; the items
        call the controller, `actions` and `openQuickControls` (tests build
        it without a tray icon). */
    static juce::PopupMenu buildMenu (EngineController& controller, const Callbacks& actions, std::function<void()> openQuickControls);

private:
    void engineControllerChanged (EngineController::Change change) override;
    juce::PopupMenu buildMenu();
    void updateIcon();

    EngineController& controller;
    Callbacks callbacks;
    bool iconShowsEnabled = false, iconValid = false;
    bool overloadNotified = false;   // bubble shown for the current overload
    uint64_t reductionsNotified = 0; // automatic profile steps already shown in a bubble
    bool tournamentNotified = false; // bubble shown for the current automatic Tournament mode

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrayIcon)
};
} // namespace flub::app
