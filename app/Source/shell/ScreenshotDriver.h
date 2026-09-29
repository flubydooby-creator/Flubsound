// Flubsound Pro - headless visual verification.
//
//   FlubsoundPro --screenshot out.png [--mode music|gaming] [--size WxH] [--seconds S] [--scale F]
//                [--device "output device name"] [--state name[,name...]] [--view advanced|simple]
//
// No audio device is opened. The engine runs offline on synthetic programme
// audio (TestSignalGenerator: drum/bass/pad music; in gaming mode a 7.1 game
// scene on the Game strip plus background music on the Music strip), paced in
// real time from a 60 Hz timer so the UI's own timers see a live-looking
// stream: meters, analyser and history views fill up exactly as they would
// with a real device. After `seconds` (default 3.5) the main component is
// rendered with createComponentSnapshot() into a PNG (at --scale, default 1,
// e.g. 2 for a HiDPI check) and the app quits (exit code 0 on success, 1 on
// failure, 2 for bad arguments). --device pretends that output device is
// open, so the headset profile, its advice banner and ceiling cap show up.
// --state puts the UI into a state that needs a real device or a real
// mistake to reach, through the same code paths where it can:
//   device-error    the device reports an error (AudioEngineHost::audioDeviceError)
//   loopback        the output is the loopback partner of the input that
//                   feeds the Game strip (the guard mutes the output)
//   preset-warning  the reader warnings of a preset with a typo'd key, parsed
//                   by flub::preset::fromJson, as the notice bar shows them
//   recovery        the notice for a damaged settings file restored from .bak1
//   latency-prompt  a preset made for another latency profile is loaded
//                   (Audiophile Subtle / Competitive FPS on Balanced)
//   governor        Boost 100 %, Loudness / Impact 100 %, maximizer drive
//                   12 dB, protection Strict: the governor backs off (use
//                   --seconds 8 so its 3 s averages settle)
//   preset-browser  the preset browser open (docs/11 E40), searched for "late
//                   night quiet" ("night quiet" in gaming mode), the best
//                   match selected and previewing, loudness matched
// Without --state the notice bar starts empty (the scene's own preset loads
// would otherwise leave a latency prompt in every screenshot).
// --view picks the main window's view (docs/11 E39); the default is advanced,
// the full window every earlier screenshot shows (the app's own default, for
// a user without a saved choice, is simple).
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
        juce::String simulatedDevice; // --device: headset profile / advice banner preview
        juce::StringArray states;     // --state (see above)
        bool simpleView = false;      // --view simple
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
    void applyStates (int gameStrip, int focusStrip);
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
