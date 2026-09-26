// Flubsound Pro - the settings dialog (opened from the header's gear button).
//
// Pages (left navigation):
//   Audio       juce::AudioDeviceSelectorComponent (device type, device,
//               sample rate, buffer size, channels); the selection is
//               persisted by the EngineController automatically. Above it:
//               the matched headset / device profile, its connection, the
//               safety ceiling applied to the master limiter and all of its
//               guidance (the page scrolls when that is long).
//   Processing  latency profile (applied to every strip and both A/B banks;
//               the engine re-prepares with a brief dropout), live latency
//               breakdown, device-input routing, per-app routing method and
//               the meter palette (standard / colour-blind safe).
//   Hotkeys     system-wide shortcut list: edit a chord as text
//               ("Ctrl+Alt+F"), reset to default, enable / disable; chords
//               that could not be registered are listed.
//   General     start minimised, close to tray, file locations, version.
#pragma once

#include "Theme.h"
#include "Widgets.h"
#include "engine/EngineController.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>

namespace flub::app::ui
{
/** Access to the application's HotkeyManager (owned by FlubsoundApplication). */
struct HotkeyHooks
{
    std::function<bool()> isSupported;
    std::function<juce::StringArray()> getFailures;
    std::function<void()> reRegister;
};

class SettingsDialog : public juce::Component, private juce::Timer
{
public:
    enum class Page
    {
        Audio,
        Processing,
        Hotkeys,
        General
    };

    /** Smallest window size at which every page fits (the Processing page's
        help text wraps more on narrow windows). */
    static constexpr int kMinWidth = 720, kMinHeight = 580;

    SettingsDialog (EngineController& controller, HotkeyHooks hooks, std::function<void (MeterPalette)> onMeterPaletteChanged,
                    MeterPalette currentPalette);
    ~SettingsDialog() override;

    /** Opens the dialog (non-modal, owns its content). The caller should keep
        the returned window (SafePointer) and delete it before the controller. */
    static juce::DialogWindow* show (EngineController& controller, juce::Component* parent, HotkeyHooks hooks,
                                     std::function<void (MeterPalette)> onMeterPaletteChanged, MeterPalette currentPalette,
                                     Page page = Page::Audio);

    void showPage (Page page);

    /** Output device + matched headset profile, connection, safety ceiling and
        the profile's guidance (EngineController::getDeviceAdvice), one line
        each; maxMessages < 0 includes every guidance message. */
    static juce::String describeOutputDevice (EngineController& controller, int maxMessages = -1);

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    class AudioPage;
    class ProcessingPage;
    class HotkeysPage;
    class GeneralPage;

    void timerCallback() override;

    EngineController& controller;
    std::array<juce::TextButton, 4> navButtons;
    std::unique_ptr<AudioPage> audioPage;
    juce::Viewport audioView; // the Audio page scrolls when the guidance is long
    std::unique_ptr<ProcessingPage> processingPage;
    std::unique_ptr<HotkeysPage> hotkeysPage;
    std::unique_ptr<GeneralPage> generalPage;
    Page current = Page::Audio;
    juce::Rectangle<int> navArea, pageArea;
};
} // namespace flub::app::ui
