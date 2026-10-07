// Flubsound Pro - the settings dialog (opened from the header's gear button).
//
// Pages (left navigation):
//   Audio       juce::AudioDeviceSelectorComponent (device type, device,
//               sample rate, buffer size, channels); the selection is
//               persisted by the EngineController automatically. Above it:
//               the matched headset / device profile, its connection, the
//               safety ceiling applied to the master limiter and all of its
//               guidance (the page scrolls when that is long), the
//               output's "Headset enhancement (Superhuman Hearing / on-board
//               EQ) is ON" switch (docs/11 E16: caps Footsteps / Detail and
//               the virtualiser on every strip, stored per endpoint), and the
//               feedback-loop guard (docs/11 E51): what it muted, "Allow
//               this pair" for a deliberate cable monitor and the allowed
//               pairs with Remove; and "Follow the system default output"
//               (docs/11 E51, off by default; choosing an output in the
//               selector turns it off). Under the selector, LATENCY
//               (LatencyPanel.h, docs/11 E42c / E42d): "Automatic buffer
//               size" (on by default; picking a size in the selector's buffer
//               list turns it off) with the buffer and the latency the device
//               reports, and "Measure latency..." (device only / through
//               Flubsound / both) with its progress and result.
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
//               protection
//               strength (docs/11 E06: Off / Normal / Strict, engine-wide),
//               the selected strip's Automatic Preamp (docs/11 E11) with its
//               live prediction and its hot-programme switch
//               (auto.preampHot), the listening level (docs/11 E32: the
//               selected strip's loudness contour, following the system
//               volume - off by default - and the reference volume with a
//               "Use current volume" button and the live level; the
//               contour's curve at that level, read-only), and the meter
//               palette (standard / colour-blind safe). Smart macros
//               (docs/11 E34) for the selected strip, under the preamp;
//               Voice chat (docs/11 E22): the chat duck's switch and depth,
//               as on the routing panel's Chat row.
//   Routing     per-app routing method; "Move the app's own sound away
//               automatically" (docs/11 E47, R4.5; Windows, off by default)
//               with what it does now, and the silent device (Automatic, or
//               one of the output endpoints the last routing pass listed;
//               Flubsound's output and the system default greyed out, a
//               chosen one that is unplugged kept as "(not connected)");
//               device input processing, "Input feeds strip", and the input
//               map (R4.6 / docs/11 E48: per strip the first device input
//               channel, "Fill in one after another", "Clear map"; persisted
//               as deviceInput.map; its line says when the device input is
//               not processed, so the map is not used). Lists are not
//               rebuilt while open. The page scrolls.
//   Hearing     the hearing guard (docs/11 E32 (c); HearingPage.h): the
//               headset sensitivity in use (the listener's own figure per
//               output, or the device profile's, marked "manufacturer
//               figure, not lab-verified"), the estimated level, the
//               listening-level cap, today's and the week's estimated dose
//               against the WHO reference; "unknown" and nothing applied
//               without a sensitivity; non-medical wording. And the personal
//               per-ear profile (docs/11 E33; PersonalProfileEditor.h).
//   Hotkeys     system-wide shortcut list (R4.4): record a chord by
//               pressing it (HotkeyCapture.h; Flubsound's own hotkeys are
//               suspended meanwhile; a chord another action has or an
//               invalid one is refused with the reason; Esc cancels,
//               Backspace clears), reset to default, enable / disable; each
//               row shows its registration status (registered, in use by
//               another application, same chord as another action, not a
//               valid shortcut, declined by the desktop, bound by the
//               desktop as another key, waiting for the desktop), in red
//               while not active with "Pick a free combination"
//               (HotkeyManager::pickFreeChord), updated as results arrive.
//   General     start with the OS (reflects the OS's actual entry; hidden
//               where unsupported), start minimised, close to tray, UI scale
//               (Follow system or 75 - 200 %) and theme (standard / high
//               contrast), both applied app-wide at once and persisted, file
//               locations, version.
//   Diagnostics the diagnostic log and crash reports (docs/11 E54): where
//               they are (Show opens the folder), how many crash reports
//               there are, and Export diagnostics, which saves one zip
//               (system and device details, settings, logs, crash reports;
//               no audio, personal paths and names redacted) for a report.
//               Updates: the opt-in, notify-only update check (off by
//               default; channel Stable / Beta, Check now, the last result
//               and the link to a newer release's page; UpdateCheck.h).
#pragma once

#include "Theme.h"
#include "Widgets.h"
#include "engine/EngineController.h"
#include "shell/HotkeyManager.h"

#include "flub/dsp/LoudnessContour.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace flub::app::ui
{
class HearingPage; // HearingPage.h (docs/11 E32 (c), E33)

/** Access to the application's HotkeyManager (owned by FlubsoundApplication). */
struct HotkeyHooks
{
    std::function<bool()> isSupported;
    std::function<juce::StringArray()> getFailures;
    std::function<void()> reRegister;
    /** Per-action status (HotkeyManager::getStatus); optional. */
    std::function<HotkeyManager::ActionStatus (HotkeyAction)> getStatus;
    /** "Pick a free combination" (HotkeyManager::pickFreeChord); optional:
        without it the button is not shown. */
    std::function<HotkeyManager::PickResult (HotkeyAction)> pickFreeChord;
    /** Suspends / resumes the registered hotkeys while a chord is recorded
        (HotkeyManager::setSuspended); optional: without it the hotkeys are
        registered again (reRegister) when recording ends. */
    std::function<void (bool)> setSuspended;
};

class SettingsDialog : public juce::Component, private juce::Timer
{
public:
    enum class Page
    {
        Audio,
        Correction,
        Processing,
        Routing,
        Hearing,
        Hotkeys,
        General,
        Diagnostics
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
    Page getCurrentPage() const noexcept { return current; }

    /** The Hotkeys page's text under the rows: the problems with their
        reasons, the recorder's prompt or refusal, a pick's outcome (tests). */
    juce::String getHotkeysSummary() const;

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

    /** The Processing page's contour curve (docs/11 E32, read-only): the lift
        the selected strip's loudness contour aims at, at ISO 226's 29
        one-third-octave frequencies (20 Hz .. 12.5 kHz), for its reference
        loudness, its level (contour.level plus the listening level from the
        system volume) and its cap: G (f) of LoudnessContour.h, which the
        stage's four sections fit within about 1 dB. Flat while it is off. */
    struct ContourCurve
    {
        bool on = false;
        float levelDb = 0.0f; // the level designed for (LoudnessContour::effectiveLevelDb)
        std::array<float, flub::iso226::kNumFrequencies> liftDb {};
    };
    static ContourCurve contourCurve (bool on, float referencePhon, float levelDb, float maxLiftDb);
    /** "At -30.0 dB re the reference: +12.1 dB at 50 Hz ... ; level trim
        -9.9 dB." or "Off: ..." (trimDb: the stage's applied trim). */
    static juce::String describeContourCurve (const ContourCurve& curve, float trimDb);

    /** The Audio page's feedback-loop guard text (docs/11 E51): whether the
        output is muted for a loopback pair and which pair, or that the
        current pair is allowed, or what the guard does. */
    static juce::String describeLoopbackGuard (EngineController& controller);
    /** The input / output pair "Allow this pair" would allow: the pair the
        guard muted, else the current devices when they look like a loopback
        pair that is not allowed yet; empty names when there is none. */
    static AppSettings::LoopbackPair loopbackPairToAllow (EngineController& controller);

    /** The Routing page's input-map line (R4.6): whether the map is in use,
        empty, or not used because the device input is not processed now
        (Device input Off, or Automatic with an input that does not look
        like a virtual cable / loopback device), then the open device's
        active inputs (and, on Linux, the sink-monitor links). */
    static juce::String describeInputMap (EngineController& controller);
    /** The open device's active input channels (16 without a device): the
        choices of each input-map row. */
    static int inputMapChannelCount (EngineController& controller);

    /** The Diagnostics page's export (docs/11 E54): writes the diagnostics
        zip for this controller, with the logs and crash reports of
        `logFolder`, to `zipFile`. `listDevices` adds every device type's
        device names (this may scan the devices). Returns an empty string on
        success, else the error. */
    static juce::String exportDiagnostics (EngineController& controller, const juce::File& logFolder, const juce::File& zipFile,
                                           bool listDevices = true);
    /** "None", or "2 - the latest on 2026-09-29 14:02" for the crash reports
        in `logFolder`. */
    static juce::String describeCrashReports (const juce::File& logFolder);
    /** The Diagnostics page's update line (docs/11 E54): "Off: ...", "Not
        checked yet." or "Last check <time>: <result>". */
    static juce::String describeUpdateCheck (const juce::PropertiesFile& settings);

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    class AudioPage;
    class CorrectionPage;
    class ProcessingPage;
    class RoutingPage;
    class HotkeysPage;
    class GeneralPage;
    class DiagnosticsPage;

    void timerCallback() override;

    EngineController& controller;
    std::array<juce::TextButton, 8> navButtons;
    std::unique_ptr<AudioPage> audioPage;
    juce::Viewport audioView; // the Audio page scrolls when the guidance is long
    std::unique_ptr<CorrectionPage> correctionPage;
    std::unique_ptr<ProcessingPage> processingPage;
    juce::Viewport processingView; // ... and the Processing page when there are many capture streams
    std::unique_ptr<RoutingPage> routingPage;
    juce::Viewport routingView; // ... and the Routing page (one input-map row per strip)
    std::unique_ptr<HearingPage> hearingPage;
    juce::Viewport hearingView; // ... and the Hearing page (the per-ear editor is tall)
    std::unique_ptr<HotkeysPage> hotkeysPage;
    std::unique_ptr<GeneralPage> generalPage;
    std::unique_ptr<DiagnosticsPage> diagnosticsPage;
    Page current = Page::Audio;
    juce::Rectangle<int> navArea, pageArea;
};
} // namespace flub::app::ui
