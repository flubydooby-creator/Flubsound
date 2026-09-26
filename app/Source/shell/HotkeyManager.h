// Flubsound Pro - system-wide hotkeys (flub::platform::GlobalHotkeys).
//
// Defaults (AppSettings::getDefaultHotkey, user-configurable):
//   Ctrl+Alt+F      enable / disable processing
//   Ctrl+Alt+M      toggle Music / Gaming mode (selected strip)
//   Ctrl+Alt+Up     Boost Intensity +10 %
//   Ctrl+Alt+Down   Boost Intensity -10 %
//   Ctrl+Alt+Right  next preset
//   Ctrl+Alt+Left   previous preset
//
// Registration failures (chord owned by another application, or no platform
// support) are collected in getFailures() so the UI can show them.
#pragma once

#include "engine/EngineController.h"
#include "platform/PlatformServices.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <memory>

namespace flub::app
{
class HotkeyManager final
{
public:
    explicit HotkeyManager (EngineController& controller);
    ~HotkeyManager();

    /** True when global hotkeys can be registered on this system. */
    bool isSupported() const noexcept;

    /** (Re-)registers every assigned chord from the settings. */
    void registerAll();
    void unregisterAll();

    /** Human-readable list of chords that could not be registered. */
    const juce::StringArray& getFailures() const noexcept { return failures; }

    /** Runs an action exactly as the hotkey would (the registered chords call
        it; the tray menu drives EngineController directly). */
    void perform (HotkeyAction action);

    /** Feedback after an action (e.g. for a tray bubble / on-screen display). */
    std::function<void (HotkeyAction action, const juce::String& feedback)> onActionPerformed;

private:
    EngineController& controller;
    std::unique_ptr<flub::platform::GlobalHotkeys> hotkeys;
    juce::StringArray failures;

    JUCE_DECLARE_WEAK_REFERENCEABLE (HotkeyManager)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HotkeyManager)
};
} // namespace flub::app
