// Flubsound Pro - the main application window.
//
// A dark, resizable DocumentWindow (minimum 1100 x 700) whose content is
// ui::MainComponent. What the close button does is decided by the
// application (close to tray vs quit) through the onCloseButton callback.
// The window position / size is persisted in AppSettings.
#pragma once

#include "engine/EngineController.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace flub::app
{
class MainWindow final : public juce::DocumentWindow
{
public:
    static constexpr int kMinWidth = 1100, kMinHeight = 700;
    static constexpr int kDefaultWidth = 1280, kDefaultHeight = 820;

    static juce::Colour backgroundColour() { return juce::Colour (0xff0f1115); }

    MainWindow (EngineController& controller, std::function<void()> onCloseButton);
    ~MainWindow() override;

    void closeButtonPressed() override;

    /** Headless screenshots may render below the interactive minimum size. */
    void setExactContentSize (int width, int height);

    void saveWindowState();

private:
    EngineController& controller;
    std::function<void()> onCloseButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};
} // namespace flub::app
