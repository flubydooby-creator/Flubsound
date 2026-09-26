// Flubsound Pro - the JUCEApplication: lifetime of every top-level object.
//
// Normal start-up:
//   1. SystemTuning::disablePowerThrottling() (Windows EcoQoS opt-out)
//   2. EngineController (settings, device, engine, presets, routing)
//   3. MainWindow (content: ui::MainComponent), shown unless "start minimised"
//   4. TrayIcon (enable, mode, boost, preset quick list, open, quit)
//   5. HotkeyManager (system-wide shortcuts)
// Closing the window hides it to the tray when "close to tray" is on (on
// Linux, where a tray host is not guaranteed, the window is minimised
// instead); "Quit" in the tray or a system quit request ends the app.
//
// Headless mode: --screenshot <out.png> [--mode music|gaming] [--size WxH]
// [--seconds S] [--scale F] [--device "output device name"] (see
// shell/ScreenshotDriver.h). No device, no tray, no hotkeys, no settings are
// written.
#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <memory>

namespace flub::app
{
class EngineController;
class MainWindow;
class TrayIcon;
class HotkeyManager;
class ScreenshotDriver;

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

private:
    void initialiseInteractive();
    bool initialiseScreenshot();
    void closeButtonPressed();

    std::unique_ptr<juce::LookAndFeel_V4> lookAndFeel;
    std::unique_ptr<EngineController> controller;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<TrayIcon> trayIcon;
    std::unique_ptr<HotkeyManager> hotkeys;
    std::unique_ptr<ScreenshotDriver> screenshot;
    bool screenshotMode = false;
};
} // namespace flub::app
