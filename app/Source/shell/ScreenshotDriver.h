// Flubsound Pro - headless visual verification.
//
//   FlubsoundPro --screenshot out.png [--mode music|gaming] [--size WxH] [--seconds S] [--scale F]
//
// No audio device is opened. The engine runs offline on synthetic programme
// audio (TestSignalGenerator: drum/bass/pad music; in gaming mode a 7.1 game
// scene on the Game strip plus background music on the Music strip), paced in
// real time from a 60 Hz timer so the UI's own timers see a live-looking
// stream: meters, analyser and history views fill up exactly as they would
// with a real device. After `seconds` (default 3.5) the main component is
// rendered with createComponentSnapshot() into a PNG (at --scale, default 1,
// e.g. 2 for a HiDPI check) and the app quits (exit code 0 on success, 1 on
// failure, 2 for bad arguments).
//
// Note for UI code: in this mode the component is on screen (under xvfb on CI),
// isShowing() is true and every juce::Timer runs normally.
#pragma once

#include "engine/EngineController.h"
#include "engine/TestSignalGenerator.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace flub::app
{
class ScreenshotDriver final : private juce::Timer
{
public:
    struct Options
    {
        juce::File output;
        bool gamingMode = false;
        int width = 1280, height = 820;
        double seconds = 3.5; // > 3 s so the short-term (3 s) loudness window is full
        float scale = 1.0f;
    };

    /** Parses the screenshot arguments; returns false if --screenshot is absent
        (error stays empty) or malformed (error set). */
    static bool parseCommandLine (const juce::StringArray& args, Options& options, juce::String& error);

    using Completion = std::function<void (bool ok, const juce::String& message)>;

    ScreenshotDriver (EngineController& controller, juce::Component& target, Options options, Completion onFinished);
    ~ScreenshotDriver() override;

    /** Sets up the scene (strip modes, boost, sources) and starts rendering. */
    void start();

private:
    void timerCallback() override;
    void setUpScene();
    void finish();

    EngineController& controller;
    juce::Component& target;
    Options options;
    Completion onFinished;
    std::unique_ptr<TestSignalGenerator> generator;
    double startMs = 0.0, lastMs = 0.0, sampleRate = 48000.0;
    int64_t renderedSamples = 0;
    bool finished = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScreenshotDriver)
};
} // namespace flub::app
