// Flubsound Pro - the JUCEApplication: lifetime of every top-level object.
//
// Normal start-up:
//   0. DiagnosticsSession (docs/11 E54): the rotating log in
//      <user data>/Logs becomes juce::Logger's current logger and the crash
//      handler is armed; engine events are logged once the controller exists
//   1. SystemTuning::disablePowerThrottling() (Windows EcoQoS opt-out)
//   2. EngineController (settings, device, engine, presets, routing)
//   3. MainWindow (content: ui::MainComponent), shown unless "start minimised"
//   4. TrayIcon (enable, mode, boost, preset quick list, open, quit)
//   5. HotkeyManager (system-wide shortcuts); a hotkey that cannot be
//      registered is printed to stderr and the log, and named once in the
//      notice under the header (R4.4)
//   6. the update check (docs/11 E54): only when the user switched it on;
//      a newer release is announced once in the tray
//   7. the on-screen display of hotkey actions (ui::Osd, docs/11 E56; the
//      tray bubble while it is off or Tournament mode holds it) and the
//      remote control socket (RemoteControl: `flubsound-cli ctl`)
// Closing the window hides it to the tray when "close to tray" is on (on
// Linux, where a tray host is not guaranteed, the window is minimised
// instead); "Quit" in the tray or a system quit request ends the app.
//
// Forwarding (docs/11 E56): `FlubsoundPro --ctl <action> [strip] [value]`
// sends that action to the running instance over RemoteControl's socket
// exactly as `flubsound-cli ctl` does, prints the reply and exits with ctl's
// code (3: not running; it never starts the app). A second plain start shows
// the running instance's window: JUCE forwards it on Windows and macOS; on
// Linux, where JUCE forwards nothing, it sends "Show" over the socket.
//
// Headless mode: --screenshot <out.png> [--mode music|gaming] [--size WxH]
// [--seconds S] [--scale F] [--device "output device name"] (see
// shell/ScreenshotDriver.h). No device, no tray, no hotkeys, no settings are
// written, nothing is logged and no crash handler is installed.
#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <memory>

namespace flub::app
{
class EngineController;
class MainWindow;
class TrayIcon;
class HotkeyManager;
class RemoteControl;
class ScreenshotDriver;
namespace ui
{
class Osd;
}
namespace diagnostics
{
class DiagnosticsSession;
namespace update
{
class UpdateChecker;
}
}

class FlubsoundApplication final : public juce::JUCEApplication
{
public:
    FlubsoundApplication();
    ~FlubsoundApplication() override;

    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override;

    void initialise (const juce::String& commandLine) override;
    void shutdown() override;
    void systemRequestedQuit() override;
    void anotherInstanceStarted (const juce::String& commandLine) override;

    /** Shows / focuses the main window (tray "Open", second instance). */
    void showMainWindow();
    /** Shows the main window with Settings > Hotkeys open (the tray's
        "hotkeys not active" item, R4.4). */
    void openHotkeySettings();

private:
    void initialiseInteractive();
    bool initialiseScreenshot();
    void forwardControlRequest();
    void closeButtonPressed();
    /** The hotkey notice (ui::MainComponent::announceHotkeyFailures), and a
        tray bubble while the window is hidden. */
    void announceHotkeyFailures();

    std::unique_ptr<diagnostics::DiagnosticsSession> diagnosticsSession; // first in, last out
    std::unique_ptr<juce::LookAndFeel_V4> lookAndFeel;
    std::unique_ptr<EngineController> controller;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<TrayIcon> trayIcon;
    std::unique_ptr<HotkeyManager> hotkeys;
    std::unique_ptr<ui::Osd> osd;                    // docs/11 E56
    std::unique_ptr<RemoteControl> remoteControl;    // docs/11 E56: `flubsound-cli ctl`
    std::unique_ptr<diagnostics::update::UpdateChecker> updateCheck; // docs/11 E54: opt-in, notify-only
    std::unique_ptr<ScreenshotDriver> screenshot;
    bool screenshotMode = false;
};
} // namespace flub::app
