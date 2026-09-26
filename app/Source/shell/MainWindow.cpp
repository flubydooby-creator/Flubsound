#include "MainWindow.h"

#include "ui/MainComponent.h"

namespace flub::app
{
MainWindow::MainWindow (EngineController& c, std::function<void()> onClose)
    : juce::DocumentWindow ("Flubsound Pro", backgroundColour(), juce::DocumentWindow::allButtons),
      controller (c),
      onCloseButton (std::move (onClose))
{
    setUsingNativeTitleBar (true);
    setContentOwned (new ui::MainComponent (controller), true);
    setResizable (true, false);
    setResizeLimits (kMinWidth, kMinHeight, 16384, 16384);

    const auto saved = controller.getSettings().getWindowState();
    if (saved.isEmpty() || ! restoreWindowStateFromString (saved))
        centreWithSize (kDefaultWidth, kDefaultHeight);
}

MainWindow::~MainWindow()
{
    clearContentComponent();
}

void MainWindow::closeButtonPressed()
{
    saveWindowState();
    if (onCloseButton != nullptr)
        onCloseButton();
}

void MainWindow::saveWindowState()
{
    controller.getSettings().setWindowState (getWindowStateAsString());
}

void MainWindow::setExactContentSize (int width, int height)
{
    setResizeLimits (64, 64, 16384, 16384);
    if (auto* content = getContentComponent())
        content->setSize (width, height);
    setContentComponentSize (width, height);
}
} // namespace flub::app
