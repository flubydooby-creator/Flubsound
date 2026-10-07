// Flubsound Pro - records a hotkey by pressing it (Settings > Hotkeys, R4.4).
//
// HotkeyCaptureField shows an action's chord ("Ctrl+Alt+Shift+B"). A click
// (or Return / Space while it has the focus, or a screen reader's press)
// starts recording: the field reads "Press a shortcut..." and shows the
// modifiers held so far ("Ctrl+Alt+..."); the next key with its modifiers is
// offered to onChordPressed, which accepts it (recording ends) or refuses it
// (the page says why, recording goes on). Esc or leaving the field cancels,
// Backspace clears the action's chord; a right click, Shift+F10 or a screen
// reader's "show menu" asks the owner for a text entry instead
// (onTypeRequested). The owner suspends Flubsound's own global hotkeys while
// recording (HotkeyManager::setSuspended), so its chords reach the field,
// and ends any recording before another control registers them again; a
// chord another application holds never arrives at all (the system gives
// it to that application), which the page's prompt says.
#pragma once

#include "platform/PlatformServices.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace flub::app::ui
{
/** The chord a key press stands for: a letter, a digit, F1 - F24, Space or a
    navigation key (PageUp / PageDown / Home / End / arrows / Insert /
    Delete) with Ctrl, Alt, Shift and, on macOS, Cmd (KeyChord::Super). Any
    other character goes through `keyForCharacter` (default
    platform_bridge::keyCodeForCharacter: on Windows the letter or digit key
    that types it on the current layout, so AZERTY's '&' is the 1 key).
    False for other keys (numeric keypad, punctuation, Esc, Tab, Return ...).
    The result is not validated (HotkeyManager::validateChord). */
bool chordFromKeyPress (const juce::KeyPress& key, flub::platform::KeyChord& chord,
                        const std::function<uint32_t (uint32_t character)>& keyForCharacter = {});

/** "Ctrl+Alt+" style prefix for the modifiers held while recording ("" for
    none), in the settings' chord order (AppSettings::chordToString). */
juce::String describeHeldModifiers (const juce::ModifierKeys& mods);

class HotkeyCaptureField final : public juce::Button, private juce::Timer
{
public:
    explicit HotkeyCaptureField (const juce::String& actionName);

    /** The chord shown while not recording (AppSettings::chordToString). */
    void setChordText (const juce::String& newChordText);
    const juce::String& getChordText() const noexcept { return chordText; }
    /** Red frame: the action's hotkey is not active (HotkeyManager::isProblem). */
    void setProblem (bool hasProblem);
    bool hasProblem() const noexcept { return problem; }

    void startCapture();
    /** Ends recording without a change (Esc, focus lost). */
    void cancelCapture();
    bool isCapturing() const noexcept { return capturing; }
    /** What the field shows now ("Press a shortcut...", "Ctrl+Alt+...", the chord). */
    juce::String getDisplayText() const;

    /** Recording started (the owner suspends the global hotkeys). */
    std::function<void()> onCaptureStarted;
    /** Recording ended: a chord was accepted, the chord cleared or the
        recording cancelled (the owner registers the hotkeys again). */
    std::function<void()> onCaptureEnded;
    /** A chord was pressed: true accepts it (recording ends), false keeps
        recording (the owner shows why it was refused). */
    std::function<bool (const flub::platform::KeyChord& chord)> onChordPressed;
    /** Backspace while recording: the action gets no chord. */
    std::function<void()> onCleared;
    /** A key that cannot be part of a hotkey ("Tab", "numpad 4" ...). */
    std::function<void (const juce::String& keyDescription)> onUnsupportedKey;
    /** A right click, Shift+F10 or a screen reader's "show menu": the owner
        offers typing the chord as text instead ("Super+F5": JUCE reports no
        Win / Super key on Windows and Linux, so such chords cannot be
        recorded). */
    std::function<void()> onTypeRequested;

    bool keyPressed (const juce::KeyPress& key) override;
    void modifierKeysChanged (const juce::ModifierKeys& mods) override;
    void focusLost (FocusChangeType cause) override;
    void clicked() override;
    void clicked (const juce::ModifierKeys& modifiers) override;
    void paintButton (juce::Graphics& g, bool highlighted, bool down) override;
    /** A button whose press records and whose "show menu" types the chord. */
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    void endCapture();
    /** While recording: the modifiers held (JUCE sends modifier changes to
        the component under the mouse, which need not be this one). */
    void timerCallback() override;

    juce::String actionName, chordText;
    juce::ModifierKeys held;
    bool capturing = false, problem = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HotkeyCaptureField)
};
} // namespace flub::app::ui
