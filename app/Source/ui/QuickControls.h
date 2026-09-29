// Flubsound Pro - the compact tray flyout (docs/11 E39): the few controls a
// listener reaches for without opening the window.
//
//   +--------------------------------------+
//   | Flubsound Pro . Music strip   [open] |
//   | BOOST  [==========o-------]  55 %    |
//   | [<]      Flubsound Signature     [>] |
//   | [ Bypass ]                           |
//   | GAME [=====o=====] CHAT  (o) voice   |
//   +--------------------------------------+
//
// Opened by a left click on the tray icon (TrayIcon; a double click opens
// the window) in a juce::CallOutBox next to the icon, instead of an
// always-on-top window (a fullscreen game hides one, and the hotkeys already
// give feedback in game). Acts on the selected strip like the tray menu:
// Boost through the strip's ParameterStore (bound, click-free), the preset
// stepper through EngineController::previousPreset / nextPreset, Bypass
// through setEnabled. Message thread only.
// ChatMix (docs/11 E22): one balance between the Game and the Chat strip
// (EngineController::setChatMix, the same the ChatMix hotkeys and the Chat
// strip's row move; double-click: centre), with a dot that lights while the
// Chat strip carries speech (EngineController::isChatVoiceActive, polled at
// kVoicePollHz). Greyed out without a Game and a Chat strip.
#pragma once

#include "ParameterBinding.h"
#include "Widgets.h"
#include "engine/EngineController.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace flub::app::ui
{
class QuickControls final : public juce::Component, private EngineController::Listener, private juce::Timer
{
public:
    static constexpr int kWidth = 330, kHeight = 216;
    static constexpr int kVoicePollHz = 15;

    explicit QuickControls (EngineController& controller);
    ~QuickControls() override;

    /** The "open the window" button. */
    std::function<void()> onOpenWindow;

    /** Shows a flyout next to `screenArea` (the tray icon); it closes when it
        loses focus. */
    static void show (EngineController& controller, juce::Rectangle<int> screenArea, std::function<void()> openWindow);

    juce::Slider& getBoostSlider() noexcept { return boost; }
    juce::Button& getPreviousButton() noexcept { return previous; }
    juce::Button& getNextButton() noexcept { return next; }
    juce::Button& getBypassButton() noexcept { return bypass; }
    juce::Button& getOpenButton() noexcept { return open; }
    juce::Slider& getChatMixSlider() noexcept { return chatMix; }
    /** The voice dot is lit (the Chat strip carries speech). */
    bool isVoiceDotLit() const noexcept { return voiceLit; }
    /** Reads the voice activity now (the timer does, kVoicePollHz). */
    void pollVoice();
    juce::String getPresetText() const { return presetName; }

    void refresh();
    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void engineControllerChanged (EngineController::Change change) override;
    void timerCallback() override { pollVoice(); }

    EngineController& controller;
    juce::Slider boost;
    IconButton previous { "Previous preset", Icons::chevronLeft(), IconButton::Style::Framed };
    IconButton next { "Next preset", Icons::chevronRight(), IconButton::Style::Framed };
    juce::TextButton bypass { "Bypass" };
    IconButton open { "Open Flubsound Pro", Icons::external(), IconButton::Style::Framed };
    juce::Slider chatMix;
    bool voiceLit = false;
    ParameterBinder binder; // after the slider it binds
    juce::String presetName;
    juce::Rectangle<int> titleArea, presetArea, boostCaption, gameCaption, chatCaption, voiceArea;
};
} // namespace flub::app::ui
