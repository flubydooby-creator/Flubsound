#include "FlubsoundApplication.h"

#include "engine/EngineController.h"
#include "platform/PlatformBridge.h"
#include "shell/HotkeyManager.h"
#include "shell/MainWindow.h"
#include "shell/ScreenshotDriver.h"
#include "shell/TrayIcon.h"

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
    using Scheme = juce::LookAndFeel_V4::ColourScheme;
    auto scheme = juce::LookAndFeel_V4::getDarkColourScheme();
    scheme.setUIColour (Scheme::windowBackground, MainWindow::backgroundColour());
    scheme.setUIColour (Scheme::widgetBackground, juce::Colour (0xff171a21));
    scheme.setUIColour (Scheme::menuBackground, juce::Colour (0xff171a21));
    scheme.setUIColour (Scheme::outline, juce::Colour (0xff262b36));
    scheme.setUIColour (Scheme::defaultText, juce::Colour (0xffe5e7eb));
    scheme.setUIColour (Scheme::defaultFill, juce::Colour (0xff22d3c5));
    scheme.setUIColour (Scheme::highlightedText, juce::Colour (0xff0f1115));
    scheme.setUIColour (Scheme::highlightedFill, juce::Colour (0xff22d3c5));
    scheme.setUIColour (Scheme::menuText, juce::Colour (0xffe5e7eb));
    return std::make_unique<juce::LookAndFeel_V4> (scheme);
}

void printLine (bool toStdErr, const juce::String& text)
{
    std::fprintf (toStdErr ? stderr : stdout, "%s\n", text.toRawUTF8());
    std::fflush (toStdErr ? stderr : stdout);
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
    // Keep the audio / capture threads off efficiency cores (Windows EcoQoS).
    platform_bridge::disablePowerThrottling();

    controller = std::make_unique<EngineController>();
    if (controller->getLastDeviceError().isNotEmpty())
        printLine (true, "Flubsound: audio device: " + controller->getLastDeviceError());

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
    if (! ScreenshotDriver::parseCommandLine (getCommandLineParameterArray(), options, error))
    {
        printLine (true, "Flubsound: " + (error.isNotEmpty() ? error : juce::String ("invalid --screenshot arguments")));
        return false;
    }

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

    if (hotkeys != nullptr)
        hotkeys->unregisterAll();
    hotkeys.reset();
    trayIcon.reset();

    if (mainWindow != nullptr && ! screenshotMode)
        mainWindow->saveWindowState();
    mainWindow.reset();

    if (controller != nullptr)
        controller->shutdown();
    controller.reset();

    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    lookAndFeel.reset();
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
