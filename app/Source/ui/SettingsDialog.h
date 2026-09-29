// Flubsound Pro - the settings dialog (opened from the header's gear button).
//
// Pages (left navigation):
//   Audio       juce::AudioDeviceSelectorComponent (device type, device,
//               sample rate, buffer size, channels); the selection is
//               persisted by the EngineController automatically. Above it:
//               the matched headset / device profile, its connection, the
//               safety ceiling applied to the master limiter and all of its
//               guidance (the page scrolls when that is long), and the
//               feedback-loop guard (docs/11 E51): what it muted, "Allow
//               this pair" for a deliberate cable monitor and the allowed
//               pairs with Remove.
//   Correction  the output device's headphone / speaker correction (docs/11
//               E15): import an AutoEQ or Equalizer APO / Peace
//               ParametricEQ.txt, switch it on / off, hold a level-fair
//               compare, remove it; shows the output it belongs to, the
//               filter count, the automatic preamp and the predicted maximum
//               boost, and what an import refused or ignored.
//   Processing  latency profile (applied to every strip and both A/B banks;
//               the engine re-prepares with a brief dropout), the opt-in
//               automatic overload response (switch, what it changed and a
//               Restore button), live latency breakdown with CPU / xruns /
//               overloads, the per-app capture streams' FIFO statistics,
//               device-input routing, per-app routing method, protection
//               strength (docs/11 E06: Off / Normal / Strict, engine-wide),
//               the selected strip's Automatic Preamp (docs/11 E11) with its
//               live prediction, the listening level (docs/11 E32: the
//               selected strip's loudness contour, following the system
//               volume - off by default - and the reference volume with a
//               "Use current volume" button and the live level), and the
//               meter palette (standard / colour-blind safe).
//   Hotkeys     system-wide shortcut list: edit a chord as text
//               ("Ctrl+Alt+F"), reset to default, enable / disable; each
//               row shows its registration status (registered, in use,
//               declined by the desktop, bound by the desktop as another
//               key, waiting for the desktop), updated as results arrive.
//   General     start with the OS (reflects the OS's actual entry; hidden
//               where unsupported), start minimised, close to tray, UI scale
//               (Follow system or 75 - 200 %) and theme (standard / high
//               contrast), both applied app-wide at once and persisted, file
//               locations, version.
#pragma once

#include "Theme.h"
#include "Widgets.h"
#include "engine/EngineController.h"
#include "shell/HotkeyManager.h"

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
    /** Per-action status (HotkeyManager::getStatus); optional. */
    std::function<HotkeyManager::ActionStatus (HotkeyAction)> getStatus;
};

class SettingsDialog : public juce::Component, private juce::Timer
{
public:
    enum class Page
    {
        Audio,
        Correction,
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

    /** The Correction page's status line: "HD 600.txt  -  10 filters  -
        preamp -5.7 dB (max boost +5.7 dB at 3.7 kHz)", "... off", or that the
        output has no correction / no output is open. */
    static juce::String describeDeviceCorrection (const EngineController::DeviceCorrectionInfo& info);

    /** The Processing page's listening-level line (docs/11 E32): the system
        volume read (or why it cannot be), the reference volume and the level
        the contour plays at relative to it; "Off: ..." while not following. */
    static juce::String describeListeningLevel (const EngineController::ListeningLevel& level);

    /** The Audio page's feedback-loop guard text (docs/11 E51): whether the
        output is muted for a loopback pair and which pair, or that the
        current pair is allowed, or what the guard does. */
    static juce::String describeLoopbackGuard (EngineController& controller);
    /** The input / output pair "Allow this pair" would allow: the pair the
        guard muted, else the current devices when they look like a loopback
        pair that is not allowed yet; empty names when there is none. */
    static AppSettings::LoopbackPair loopbackPairToAllow (EngineController& controller);

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    class AudioPage;
    class CorrectionPage;
    class ProcessingPage;
    class HotkeysPage;
    class GeneralPage;

    void timerCallback() override;

    EngineController& controller;
    std::array<juce::TextButton, 5> navButtons;
    std::unique_ptr<AudioPage> audioPage;
    juce::Viewport audioView; // the Audio page scrolls when the guidance is long
    std::unique_ptr<CorrectionPage> correctionPage;
    std::unique_ptr<ProcessingPage> processingPage;
    juce::Viewport processingView; // ... and the Processing page when there are many capture streams
    std::unique_ptr<HotkeysPage> hotkeysPage;
    std::unique_ptr<GeneralPage> generalPage;
    Page current = Page::Audio;
    juce::Rectangle<int> navArea, pageArea;
};
} // namespace flub::app::ui
