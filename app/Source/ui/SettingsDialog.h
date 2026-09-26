// Flubsound Pro - the settings dialog (opened from the header's gear button).
//
// Pages (left navigation):
//   Audio       juce::AudioDeviceSelectorComponent (device type, device,
//               sample rate, buffer size, channels); the selection is
//               persisted by the EngineController automatically.
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

    SettingsDialog (EngineController& controller, HotkeyHooks hooks, std::function<void (MeterPalette)> onMeterPaletteChanged,
                    MeterPalette currentPalette);
    ~SettingsDialog() override;

    /** Opens the dialog (non-modal, owns its content). The caller should keep
        the returned window (SafePointer) and delete it before the controller. */
    static juce::DialogWindow* show (EngineController& controller, juce::Component* parent, HotkeyHooks hooks,
                                     std::function<void (MeterPalette)> onMeterPaletteChanged, MeterPalette currentPalette,
                                     Page page = Page::Audio);

    void showPage (Page page);

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    class ProcessingPage;
    class HotkeysPage;
    class GeneralPage;

    void timerCallback() override;

    EngineController& controller;
    std::array<juce::TextButton, 4> navButtons;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> audioPage;
    std::unique_ptr<ProcessingPage> processingPage;
    std::unique_ptr<HotkeysPage> hotkeysPage;
    std::unique_ptr<GeneralPage> generalPage;
    Page current = Page::Audio;
    juce::Rectangle<int> navArea, pageArea;
};
} // namespace flub::app::ui
