// Flubsound Pro - system tray / menu-bar icon.
//
// Left click (Windows / Linux): open the main window. Right click (any
// click on macOS): quick menu
//   Enabled | Mode: Music / Gaming | Boost +10 % / -10 % | Presets > ... |
//   Open Flubsound Pro | Quit
// Mode, boost and presets act on the selected strip. The icon is drawn in
// code (accent colour while enabled, grey while bypassed) and doubles as a
// macOS template image.
#pragma once

#include "engine/EngineController.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>

namespace flub::app
{
class HotkeyManager;

class TrayIcon final : public juce::SystemTrayIconComponent, private EngineController::Listener
{
public:
    struct Callbacks
    {
        std::function<void()> openWindow;
        std::function<void()> quit;
    };

    TrayIcon (EngineController& controller, Callbacks callbacks);
    ~TrayIcon() override;

    /** Short notification near the icon (where the OS supports it). */
    void notify (const juce::String& title, const juce::String& message);

    void mouseDown (const juce::MouseEvent& e) override;

    static juce::Image createIconImage (bool enabled, bool asTemplate, int size = 64);

private:
    void engineControllerChanged (EngineController::Change change) override;
    juce::PopupMenu buildMenu();
    void updateIcon();

    EngineController& controller;
    Callbacks callbacks;
    bool iconShowsEnabled = false, iconValid = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrayIcon)
};
} // namespace flub::app
