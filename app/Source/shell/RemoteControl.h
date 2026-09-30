// Flubsound Pro - remote control of the running app (docs/11 E56):
// `flubsound-cli ctl <action> [strip] [value]` and `FlubsoundPro --ctl ...`
// from a second instance send one request over the per-user socket
// (tools/flubsound-cli/Ctl.h); this class listens on it and runs the action
// on the message thread.
//
// * Hotkey actions go through HotkeyManager::perform (action, strip): the
//   same code, the same feedback text and the same on-screen display as the
//   hotkey. Without a strip, the hotkey strip (EngineController::
//   getHotkeyStrip); a named strip must exist (case ignored).
// * Boost <strip> <0-100> sets Boost Intensity; LoadPreset <strip> <uuid>
//   loads a preset by its uuid (or id, PresetManager::findById); both show
//   their feedback on the display (onFeedback).
// * Show brings the main window up (onShowWindow; also what a second plain
//   start of the app sends on Linux, where JUCE forwards nothing).
// * Osd on|off and Earcon off|fullscreen|always set the display's options
//   (ui/Osd.h).
// The reply is the feedback text ("Game: Boost 60%"), a usage error (unknown
// strip, bad value) or a failure (e.g. "No presets available").
//
// The socket thread waits up to kReplyTimeoutMs for the message thread.
#pragma once

#include "Ctl.h"

#include "engine/EngineController.h"
#include "shell/HotkeyManager.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <memory>
#include <string>

namespace flub::app
{
class RemoteControl final
{
public:
    RemoteControl (EngineController& controller, HotkeyManager& hotkeys);
    ~RemoteControl(); // stop()

    /** Listens on socketPath (default: the per-user path). False with
        `error` when another instance already listens there or the socket
        cannot be created. */
    bool start (juce::String& error, const std::string& socketPath = flub::cli::ctl::defaultSocketPath());
    void stop();
    bool isListening() const noexcept { return server.isRunning(); }

    /** Runs one request (message thread; the socket thread calls it through
        the message loop). Public for tests. */
    flub::cli::ctl::Reply handle (const flub::cli::ctl::Request& request);

    /** Show / focus the main window (FlubsoundApplication::showMainWindow). */
    std::function<void()> onShowWindow;
    /** Feedback of Boost / LoadPreset for the display (title, text, level 0..1
        or < 0 for none); hotkey actions report through HotkeyManager's
        onActionPerformed instead. */
    std::function<void (const juce::String& title, const juce::String& text, float level)> onFeedback;

    static constexpr int kReplyTimeoutMs = 3000;

private:
    struct Shared;

    EngineController& controller;
    HotkeyManager& hotkeys;
    flub::cli::ctl::Server server;
    std::shared_ptr<Shared> shared;

    JUCE_DECLARE_WEAK_REFERENCEABLE (RemoteControl)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RemoteControl)
};
} // namespace flub::app
