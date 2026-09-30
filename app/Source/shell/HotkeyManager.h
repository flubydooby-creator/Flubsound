// Flubsound Pro - system-wide hotkeys (flub::platform::GlobalHotkeys).
//
// Defaults (AppSettings::getDefaultHotkey, user-configurable):
//   Ctrl+Alt+F         enable / disable processing (every strip)
//   Ctrl+Alt+M         toggle Music / Gaming mode           (hotkey strip)
//   Ctrl+Alt+Up        Boost Intensity +10 %                (hotkey strip)
//   Ctrl+Alt+Down      Boost Intensity -10 %                (hotkey strip)
//   Ctrl+Alt+Right     next preset                          (hotkey strip)
//   Ctrl+Alt+Left      previous preset                      (hotkey strip)
//   Ctrl+Alt+S         Focus: latched Footsteps 100 %       (hotkey strip)
//   Ctrl+Alt+N         Night listening, latched dynamics    (hotkey strip)
//   Ctrl+Alt+B         bypass that strip only               (hotkey strip)
//   Ctrl+Alt+PageUp    ChatMix one step towards Chat (Game down, Chat up)
//   Ctrl+Alt+PageDown  ChatMix one step towards Game
//
// The hotkey strip (docs/11 E56) is EngineController::getHotkeyStrip(): the
// active automatic profile's strip, else the one chosen in Settings >
// Hotkeys (default Game) - never the strip selected in the window, so a
// mid-match Boost+ cannot land on Music because Music was clicked last. The
// feedback text names it ("Game: Boost 60%").
//
// Each action is registered under its name (AppSettings::getHotkeyActionName),
// which desktops that list shortcuts show (the Wayland portal dialog). The
// outcome of every registration is kept per action (getStatus): the
// service's binding listener reports it synchronously (Windows, macOS, X11)
// or later from its own thread (Wayland portal: the desktop may decline a
// shortcut or bind another key); results are moved onto the message thread.
// Actions that are not active are also listed by getFailures() for the UI.
#pragma once

#include "engine/EngineController.h"
#include "platform/PlatformServices.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <map>
#include <memory>
#include <set>

namespace flub::app
{
class HotkeyManager final
{
public:
    explicit HotkeyManager (EngineController& controller);

    /** Uses 'service' instead of the OS's (tests); nullptr = unsupported. */
    HotkeyManager (EngineController& controller, std::unique_ptr<flub::platform::GlobalHotkeys> service);
    ~HotkeyManager();

    /** True when global hotkeys can be registered on this system. */
    bool isSupported() const noexcept;

    /** (Re-)registers every assigned chord from the settings. */
    void registerAll();
    void unregisterAll();

    /** Where an action's shortcut stands after the last registerAll(). */
    enum class Status
    {
        NotAssigned,  // no chord (or registerAll() not called yet)
        SwitchedOff,  // hotkeys disabled in the settings
        NotSupported, // no global-hotkey service on this system
        Pending,      // requested; the desktop has not answered yet (Wayland)
        Registered,   // active with the chord from the settings
        Reassigned,   // active, but the desktop bound another key: 'trigger'
        Unavailable,  // taken by another application, or not allowed here
        Declined      // the desktop (or the user in its dialog) declined it
    };

    struct ActionStatus
    {
        Status status = Status::NotAssigned;
        juce::String chord;   // as registered (AppSettings::chordToString)
        juce::String trigger; // Reassigned: the desktop's name for the key bound
    };

    ActionStatus getStatus (HotkeyAction action) const;

    /** Short text for the Hotkeys page's row: "Registered", "In use / could
        not register", "Declined by the desktop", "Bound by the desktop as
        <trigger>", "Waiting for the desktop", "Not assigned", "Off",
        "Not supported here". */
    static juce::String describe (const ActionStatus& status);
    juce::String getStatusText (HotkeyAction action) const { return describe (getStatus (action)); }

    /** Human-readable list of shortcuts that are not active (could not be
        registered, or declined by the desktop), or why hotkeys are
        unavailable altogether. */
    juce::StringArray getFailures() const;

    /** Runs an action exactly as the hotkey would (the registered chords call
        it; the tray menu drives EngineController directly) and returns the
        feedback text. The second form names the strip (`flubsound-cli ctl`,
        docs/11 E56; global actions ignore it). */
    juce::String perform (HotkeyAction action);
    juce::String perform (HotkeyAction action, int strip);

    /** ChatMix balance change per press (5 presses from centre to one end:
        the other side -1.9 dB after one press, muted after five; docs/11 E22). */
    static constexpr float kChatMixStep = 0.2f;

    /** Feedback after an action: the on-screen display (ui/Osd.h), else a
        tray bubble (FlubsoundApplication). */
    std::function<void (HotkeyAction action, const juce::String& feedback)> onActionPerformed;

    /** Called on the message thread whenever an action's status changes. */
    std::function<void()> onStatusChanged;

private:
    void applyBindingResult (const flub::platform::GlobalHotkeys::BindingResult& result);
    void setStatus (HotkeyAction action, ActionStatus status);

    EngineController& controller;
    std::unique_ptr<flub::platform::GlobalHotkeys> hotkeys;
    std::map<HotkeyAction, ActionStatus> statuses; // assigned actions only
    std::set<HotkeyAction> refusedAtOnce;          // registerHotkey() returned false in the last registerAll()
    bool unsupported = false; // hotkeys enabled, but no service

    JUCE_DECLARE_WEAK_REFERENCEABLE (HotkeyManager)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HotkeyManager)
};
} // namespace flub::app
