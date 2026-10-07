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
//   Ctrl+Alt+Shift+B   bypass that strip only               (hotkey strip)
//                      (Ctrl+Alt+B before 2026-10-07; a saved chord is kept)
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
//
// Conflicts are first-class (R4.4): before registering, a chord that is not
// a valid shortcut is marked Invalid and a chord an earlier action already
// has is marked Conflict (one press would run both), so neither is blamed on
// another application. pickFreeChord() tries a short list of alternatives
// (AppSettings::getAlternativeHotkeys) and keeps the first one the system
// accepts; takeUnannouncedFailures() gives the failures the notice under the
// header has not named yet (once per action and chord, remembered in the
// settings). setSuspended() releases every chord while Settings > Hotkeys
// records a new one, so Flubsound's own chords reach the recorder.
#pragma once

#include "engine/EngineController.h"
#include "platform/PlatformServices.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

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

    /** (Re-)registers every assigned chord from the settings (and ends a
        suspension). */
    void registerAll();
    void unregisterAll();

    /** While suspended no chord is registered (the statuses stay as they
        were): Settings > Hotkeys records a new chord then, so pressing one of
        Flubsound's own chords reaches the recorder instead of running its
        action. Resuming (false) always registers everything from the
        settings again, also when a registerAll() in between already ended
        the suspension. */
    void setSuspended (bool shouldBeSuspended);
    bool isSuspended() const noexcept { return suspended; }

    /** Where an action's shortcut stands after the last registerAll(). */
    enum class Status
    {
        NotAssigned,  // no chord (or registerAll() not called yet)
        SwitchedOff,  // hotkeys disabled in the settings
        NotSupported, // no global-hotkey service on this system
        Pending,      // requested; the desktop has not answered yet (Wayland)
        Registered,   // active with the chord from the settings
        Reassigned,   // active, but the desktop bound another key: 'trigger'
        Unavailable,  // taken by another application, or reserved by the system
        Declined,     // the desktop (or the user in its dialog) declined it
        Conflict,     // an earlier action has the same chord: not registered ('detail' names it)
        Invalid       // not a valid shortcut ('detail' says why): not registered
    };

    /** The statuses shown in red, with "Pick a free combination". */
    static bool isProblem (Status status) noexcept;

    struct ActionStatus
    {
        Status status = Status::NotAssigned;
        juce::String chord;   // as registered (AppSettings::chordToString)
        juce::String trigger; // Reassigned: the desktop's name for the key bound
        juce::String detail;  // Conflict: the other action's name; Invalid: why
    };

    ActionStatus getStatus (HotkeyAction action) const;

    /** Short text for the Hotkeys page's row: "Registered", "Could not
        register" (another application holds it, the system reserves it, or
        the service could not take it), "Declined by the desktop", "Same chord as
        <action>", "Not a valid shortcut", "Bound by the desktop as
        <trigger>", "Waiting for the desktop", "Not assigned", "Off",
        "Not supported here". */
    static juce::String describe (const ActionStatus& status);
    juce::String getStatusText (HotkeyAction action) const { return describe (getStatus (action)); }

    /** Human-readable list of shortcuts that are not active (could not be
        registered, declined by the desktop, the same chord as another action,
        not a valid shortcut), or why hotkeys are unavailable altogether. */
    juce::StringArray getFailures() const;

    /** One action that is not active, for the notice and the page summary. */
    struct Failure
    {
        HotkeyAction action {};
        ActionStatus status;
    };
    /** The actions whose status isProblem(), in the settings' action order. */
    std::vector<Failure> getFailureList() const;
    /** "Bypass hotkey strip (Ctrl+Alt+B) could not be registered (another
        application may hold it, or the system reserves it)",
        "... was declined by the desktop", "... is the same chord as Toggle
        Music / Gaming", "... is not a valid shortcut: <why>". */
    static juce::String describeFailure (const Failure& failure);
    /** The failures the notice has not named yet, each then marked as named
        in the settings (per action and chord, so a failure is announced once,
        also across restarts); an action that registers or has no chord any
        more is forgotten, so a later failure is named again. */
    std::vector<Failure> takeUnannouncedFailures();

    // ---- Rebinding (Settings > Hotkeys) ------------------------------------------------
    /** Why `chord` cannot be a global hotkey ("" when it can). */
    static juce::String validateChord (const flub::platform::KeyChord& chord);
    /** Why Settings > Hotkeys refuses a chord the system would accept: it
        would take a key from every other application ("" when it does not).
        Always: Alt+F4, Alt+Space, Ctrl+Alt+Delete, Super+Space / L / D / Q,
        and F1 - F12 alone or with Shift only. `recorded` (pressed in the
        recorder, where one reflex press after a stray click is enough) also
        refuses one modifier besides Shift - Ctrl, Alt, on macOS Cmd - with a
        letter, digit, navigation key or F1 - F12 (copy, paste, undo, menus,
        Ctrl+F4 ...); typing such a chord takes it on purpose. Saved chords
        are never checked (registerAll keeps them working). */
    static juce::String commonShortcutProblem (const flub::platform::KeyChord& chord, bool recorded);
    /** The other action that has `chord` in the settings, if any. */
    static std::optional<HotkeyAction> findConflict (const AppSettings& settings, HotkeyAction action,
                                                     const flub::platform::KeyChord& chord);
    /** AppSettings::getAlternativeHotkeys without the action's own chord,
        chords other actions have and invalid ones. */
    static std::vector<flub::platform::KeyChord> freeChordCandidates (const AppSettings& settings, HotkeyAction action);

    struct PickResult
    {
        bool found = false;
        flub::platform::KeyChord chord; // found: now saved and registered
        juce::StringArray tried;        // in order
        juce::String message;           // for the page's status line
    };
    /** "Pick a free combination": registers the candidates in turn and keeps
        (and saves) the first one the system accepts - registered at once
        (Windows, macOS, X11) or handed to the desktop (Wayland portal, which
        may still decline it). Nothing changes when none is free. */
    PickResult pickFreeChord (HotkeyAction action);
    PickResult pickFreeChord (HotkeyAction action, const std::vector<flub::platform::KeyChord>& candidates);

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
    /** Registers one action's chord with the service; false when refused. */
    bool registerAction (HotkeyAction action, const flub::platform::KeyChord& chord);
    void applyBindingResult (const flub::platform::GlobalHotkeys::BindingResult& result);
    void setStatus (HotkeyAction action, ActionStatus status);

    EngineController& controller;
    std::unique_ptr<flub::platform::GlobalHotkeys> hotkeys;
    std::map<HotkeyAction, ActionStatus> statuses; // assigned actions only
    std::set<HotkeyAction> refusedAtOnce;          // registerHotkey() returned false in the last registerAll()
    bool unsupported = false; // hotkeys enabled, but no service
    bool suspended = false;   // setSuspended (true): nothing registered
    bool quiet = false;       // pickFreeChord: no onStatusChanged per candidate

    JUCE_DECLARE_WEAK_REFERENCEABLE (HotkeyManager)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HotkeyManager)
};
} // namespace flub::app
