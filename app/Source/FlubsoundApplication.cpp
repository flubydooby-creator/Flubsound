#include "FlubsoundApplication.h"

#include "diagnostics/DiagnosticsSession.h"
#include "diagnostics/UpdateCheck.h"
#include "engine/EngineController.h"
#include "platform/PlatformBridge.h"
#include "shell/HotkeyManager.h"
#include "shell/MainWindow.h"
#include "shell/ScreenshotDriver.h"
#include "shell/TrayIcon.h"
#include "ui/FlubLookAndFeel.h"
#include "ui/MainComponent.h"
#include "ui/Theme.h"

#include <cstdio>

namespace flub::app
{
namespace
{
bool commandLineRequestsScreenshot()
{
    return juce::JUCEApplicationBase::getCommandLineParameterArray().contains ("--screenshot");
}

std::unique_ptr<juce::LookAndFeel_V4> makeLookAndFeel()
{
    // The application-wide look-and-feel is the UI's own: dialogs, alert
    // windows, tooltips and the tray menu then match the main window, and
    // the main component switches its accent colour with the mode.
    return std::make_unique<ui::FlubLookAndFeel>();
}

/** Screenshot option `--theme standard|high-contrast` (default standard). */
bool parseScreenshotTheme (const juce::StringArray& args, ui::UiTheme& theme, juce::String& error)
{
    theme = ui::UiTheme::Standard;
    const int index = args.indexOf ("--theme");
    if (index < 0)
        return true;
    const auto value = index + 1 < args.size() ? args[index + 1].toLowerCase() : juce::String();
    if (value == "high-contrast" || value == "highcontrast")
        theme = ui::UiTheme::HighContrast;
    else if (value != "standard")
    {
        error = "--theme must be 'standard' or 'high-contrast'";
        return false;
    }
    return true;
}

void printLine (bool toStdErr, const juce::String& text)
{
    std::fprintf (toStdErr ? stderr : stdout, "%s\n", text.toRawUTF8());
    std::fflush (toStdErr ? stderr : stdout);
    // Problems also go to the diagnostic log (interactive mode only).
    if (toStdErr && juce::Logger::getCurrentLogger() != nullptr)
        juce::Logger::writeToLog (text);
}
} // namespace

FlubsoundApplication::FlubsoundApplication() = default;
FlubsoundApplication::~FlubsoundApplication() = default;

bool FlubsoundApplication::moreThanOneInstanceAllowed()
{
    // Headless screenshot runs must never be swallowed by a running instance.
    return commandLineRequestsScreenshot();
}

void FlubsoundApplication::initialise (const juce::String&)
{
    lookAndFeel = makeLookAndFeel();
    juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get());

    screenshotMode = commandLineRequestsScreenshot();
    if (screenshotMode)
    {
        if (! initialiseScreenshot())
        {
            setApplicationReturnValue (2);
            quit();
        }
        return;
    }

    initialiseInteractive();
}

void FlubsoundApplication::initialiseInteractive()
{
    // First, so a crash during start-up is reported (docs/11 E54).
    diagnosticsSession = std::make_unique<diagnostics::DiagnosticsSession>();

    // Keep the audio / capture threads off efficiency cores (Windows EcoQoS).
    platform_bridge::disablePowerThrottling();

    controller = std::make_unique<EngineController>();
    if (controller->getLastDeviceError().isNotEmpty())
        std::fprintf (stderr, "Flubsound: audio device: %s\n", controller->getLastDeviceError().toRawUTF8()); // the log gets it from the monitor
    diagnosticsSession->attach (*controller);

    // Settings > General > UI scale and theme, before any window exists (the
    // saved window position is in the scaled coordinates).
    ui::Theme::applyUiScale (controller->getSettings().getUiScalePercent());
    ui::Theme::setTheme (controller->getSettings().getHighContrast() ? ui::UiTheme::HighContrast : ui::UiTheme::Standard);

    mainWindow = std::make_unique<MainWindow> (*controller, [this] { closeButtonPressed(); });

    TrayIcon::Callbacks trayCallbacks;
    trayCallbacks.openWindow = [this] { showMainWindow(); };
    trayCallbacks.quit = [this] { systemRequestedQuit(); };
    trayIcon = std::make_unique<TrayIcon> (*controller, std::move (trayCallbacks));

    hotkeys = std::make_unique<HotkeyManager> (*controller);
    hotkeys->onActionPerformed = [this] (HotkeyAction, const juce::String& feedback)
    {
        if (trayIcon != nullptr)
            trayIcon->notify ("Flubsound Pro", feedback);
    };
    hotkeys->registerAll();
    for (const auto& failure : hotkeys->getFailures())
        printLine (true, "Flubsound: hotkey: " + failure);

    // docs/11 E54: the notify-only update check; nothing runs while it is off.
    updateCheck = diagnostics::update::startAtLaunch (controller->getSettings().getPropertiesFile(),
                                                      [this] (const juce::String& title, const juce::String& message)
                                                      {
                                                          if (trayIcon != nullptr)
                                                              trayIcon->notify (title, message);
                                                      });

   #if ! JUCE_MAC
    // An enabled start-up entry follows this executable if the app was moved,
    // updated in a new folder or its AppImage renamed. (macOS registers the
    // bundle itself, so there is nothing to refresh.)
    if (auto autoStart = platform_bridge::createAutoStart(); autoStart != nullptr && autoStart->isSupported() && autoStart->isEnabled())
    {
        std::string error;
        if (! autoStart->setEnabled (true, {}, error))
            printLine (true, "Flubsound: start with the OS: " + juce::String::fromUTF8 (error.c_str()));
    }
   #endif

    // The settings dialog edits the hotkeys: give it access to the manager.
    if (auto* content = dynamic_cast<ui::MainComponent*> (mainWindow->getContentComponent()))
    {
        ui::HotkeyHooks hooks;
        hooks.isSupported = [this] { return hotkeys != nullptr && hotkeys->isSupported(); };
        hooks.getFailures = [this] { return hotkeys != nullptr ? hotkeys->getFailures() : juce::StringArray(); };
        hooks.getStatus = [this] (HotkeyAction action) { return hotkeys != nullptr ? hotkeys->getStatus (action) : HotkeyManager::ActionStatus(); };
        hooks.reRegister = [this]
        {
            if (hotkeys != nullptr)
                hotkeys->registerAll(); // unregisters the previous chords first
        };
        content->setHotkeyHooks (std::move (hooks));
    }

    if (controller->getSettings().getStartMinimised())
    {
       #if JUCE_LINUX || JUCE_BSD
        // No guaranteed tray host on Linux desktops: start iconified instead.
        mainWindow->setVisible (true);
        mainWindow->setMinimised (true);
       #endif
    }
    else
    {
        mainWindow->setVisible (true);
    }
}

bool FlubsoundApplication::initialiseScreenshot()
{
    ScreenshotDriver::Options options;
    juce::String error;
    ui::UiTheme theme {};
    if (! ScreenshotDriver::parseCommandLine (getCommandLineParameterArray(), options, error)
        || ! parseScreenshotTheme (getCommandLineParameterArray(), theme, error))
    {
        printLine (true, "Flubsound: " + (error.isNotEmpty() ? error : juce::String ("invalid --screenshot arguments")));
        return false;
    }

    // --theme picks the palette; --scale is also the UI scale (the snapshot
    // is rendered at that scale, which is exactly what the UI looks like at
    // it; the window keeps its --size in logical pixels).
    ui::Theme::setTheme (theme);
    ui::Theme::applyUiScale (juce::roundToInt (options.scale * 100.0f));

    EngineController::Options engineOptions;
    engineOptions.openAudioDevice = false;
    engineOptions.restoreState = false;
    engineOptions.enableAppRouting = false;
    engineOptions.persistSettings = false;
    engineOptions.settingsFile = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("FlubsoundPro-screenshot.settings");
    controller = std::make_unique<EngineController> (engineOptions);

    mainWindow = std::make_unique<MainWindow> (*controller, [] { quit(); });
    mainWindow->setExactContentSize (options.width, options.height);
    mainWindow->setVisible (true);

    auto* content = mainWindow->getContentComponent();
    if (content == nullptr)
        return false;

    screenshot = std::make_unique<ScreenshotDriver> (*controller, *content, options,
                                                     [this] (bool ok, const juce::String& message)
                                                     {
                                                         printLine (! ok, message);
                                                         setApplicationReturnValue (ok ? 0 : 1);
                                                         quit();
                                                     });
    screenshot->start();
    return true;
}

void FlubsoundApplication::shutdown()
{
    screenshot.reset();
    updateCheck.reset(); // cancels a running request

    if (hotkeys != nullptr)
        hotkeys->unregisterAll();
    hotkeys.reset();
    trayIcon.reset();

    if (mainWindow != nullptr && ! screenshotMode)
        mainWindow->saveWindowState();
    mainWindow.reset();

    if (diagnosticsSession != nullptr)
        diagnosticsSession->detach();
    if (controller != nullptr)
        controller->shutdown();
    controller.reset();

    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    lookAndFeel.reset();
    diagnosticsSession.reset(); // the "stopped" line; disarms the crash handler
}

void FlubsoundApplication::systemRequestedQuit()
{
    quit();
}

void FlubsoundApplication::anotherInstanceStarted (const juce::String&)
{
    showMainWindow();
}

void FlubsoundApplication::showMainWindow()
{
    if (mainWindow == nullptr)
        return;
    mainWindow->setVisible (true);
    if (mainWindow->isMinimised())
        mainWindow->setMinimised (false);
    mainWindow->toFront (true);
}

void FlubsoundApplication::closeButtonPressed()
{
    if (controller != nullptr && controller->getSettings().getCloseToTray() && trayIcon != nullptr)
    {
       #if JUCE_LINUX || JUCE_BSD
        mainWindow->setMinimised (true);
       #else
        mainWindow->setVisible (false);
       #endif
        return;
    }
    systemRequestedQuit();
}
} // namespace flub::app
