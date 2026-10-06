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
//   ab-matched      bank B holds a louder sound (Boost 100 %, Loudness /
//                   Impact 100 %) and plays loudness matched (docs/11 E37):
//                   the trim reads under A / B
//   abx             the blind A/B/X test over the window, three trials in
//   bypass          the master Bypass switched on at 80 % of the run (use
//                   --seconds 5 or more: the output's short-term loudness
//                   reads after 3 s): "proc. +x LU" under it
//   routing-drawer  the routing panel's drawer open (narrow windows)
//   governor-normal as governor, at protection strength Normal: the
//                   PROTECTION readouts (residual, PLR, brightness)
//   quick-controls  the PNG shows the tray flyout (ui::QuickControls) at --size
//   settings-audio, settings-processing
//                   the PNG shows that page of the Settings dialog at --size
//                   instead of the main window (Audio: with loopback, the
//                   feedback-loop guard's muted pair, docs/11 E51;
//                   Processing: the listening level with following the
//                   system volume switched on, docs/11 E32)
//   module-readings the rack's controls and readings outside the generic
//                   grid (Music): Maximizer on with style Punchy and LF Limit
//                   50 % (E05), Boost and Clarity 100 % with Smoothness
//                   100 % (the cut, E07), Warmth 60 % with the saturator
//                   Warmth's alone (Tube chosen, E14); the rack scrolled to
//                   the Saturation card (--seconds 4 or more). The test
//                   programme is too dark for the Smoothness stage to cut,
//                   so the Clarity card's cut note is tested, not shown
//   contour-curve   settings-processing with the loudness contour on at
//                   -30 dB (its curve, E32) and the automatic preamp with
//                   its hot-programme switch on (E11)
//   onboard-cap     "Headset enhancement is ON" answered for a Turtle Beach
//                   headset (docs/11 E16; --device, else "Headset Earphone
//                   (Stealth 700 Gen 2 MAX)"): the CAPPED chips on Footsteps
//                   and Detail (--mode gaming), the switch on with
//                   settings-audio
//   settings-diagnostics
//                   the PNG shows the Settings dialog's Diagnostics page
//                   with its Updates section (docs/11 E54) at --size
//   analyzer-diff, analyzer-lows, analyzer-width, analyzer-keys,
//   analyzer-spectrogram
//                   that optional analyser view switched on (combinable, not
//                   saved to the settings)
//   analyzer-hover  the hover readout (crosshair, note, levels) at 62 Hz,
//                   as if the mouse rested there when the PNG is taken
//   analyzer-freeze the traces frozen at 40 % of the run, then EQ band 7
//                   (2 kHz) raised by 9 dB so the live trace moves away
//   vis-<id>        the analyser shows that visualiser (any id in
//                   ui/vis/VisualiserRegistry.cpp that can replace the
//                   spectrum, e.g. vis-goniometer); vis-beside puts it beside
//                   the spectrum; vis-strip-<id> adds that strip view (e.g.
//                   vis-strip-correlation). Not saved to the settings.
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
    /** True for "vis-beside", "vis-<id>" and "vis-strip-<id>" with a registered id usable there. */
    static bool isVisualiserState (const juce::String& state);

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
    std::unique_ptr<juce::Component> settingsView; // --state settings-*: what the PNG shows
    double startMs = 0.0, lastMs = 0.0, sampleRate = 48000.0;
    double bypassAtSeconds = 0.0; // --state bypass: when the master Bypass goes on
    double freezeAtSeconds = 0.0; // --state analyzer-freeze: when the traces are frozen
    int sceneStrip = 0;
    int64_t renderedSamples = 0;
    bool finished = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScreenshotDriver)
};
} // namespace flub::app
