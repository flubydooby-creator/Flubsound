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
//               breakdown with CPU / xruns / overloads, the per-app capture
//               streams' FIFO statistics, device-input routing, per-app
//               routing method and the meter palette (standard / colour-blind
//               safe).
//   Hotkeys     system-wide shortcut list: edit a chord as text
//               ("Ctrl+Alt+F"), reset to default, enable / disable; chords
//               that could not be registered are listed.
//   General     start with the OS (reflects the OS's actual entry; hidden
//               where unsupported), start minimised, close to tray, file
//               locations, version.
#pragma once

#include "Theme.h"
#include "Widgets.h"
#include "engine/EngineController.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

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
        help text wraps more on narrow windows). The Audio and Processing
        pages scroll when their live content is taller. */
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

    /** One line per per-app capture stream (EngineController::getCaptureStreams):
        application and strip, the DriftCompensatedFifo fill against its target,
        the drift correction and the underrun / overflow / dropped-frame counts.
        Empty if there are none. */
    static juce::String describeCaptureStreams (const std::vector<EngineController::CaptureStream>& streams);

    /** CPU load, device xruns (when reported) and the overload watchdog's
        state and session count, on one line. */
    static juce::String describeCpuLine (const EngineStatus& status, const OverloadWatchdog::State& overload);

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
    juce::Viewport processingView; // ... and the Processing page when there are many capture streams
    std::unique_ptr<HotkeysPage> hotkeysPage;
    std::unique_ptr<GeneralPage> generalPage;
    Page current = Page::Audio;
    juce::Rectangle<int> navArea, pageArea;
};
} // namespace flub::app::ui
