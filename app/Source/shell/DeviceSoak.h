// Flubsound Pro - headless real-device soak (docs/11 E53, R1.5).
//
//   FlubsoundPro --device-soak --device "<output>" [--type "<device type>"]
//                [--buffer <samples>|min] [--rate <Hz>] [--minutes <m>]
//                [--report <file.json>] [--profile quality|balanced|low]
//                [--seed <n>] [--interval <ms>] [--automation user|off] [--ui]
//                [--dump <seconds>[,<seconds>...]] [--allow-audible]
//   FlubsoundPro --device-soak --list [--device "<output>"] [--type "<device type>"]
//   FlubsoundPro --device-soak --replay <report.json> [--report <file.json>]
//
// The full app engine (EngineController -> AudioEngineHost -> MixEngine) plays
// on ONE real output for `minutes`, with temporary settings (never the user's
// settings file, never written), no tray, hotkeys, routing or remote control,
// and the system default output untouched. Several instances may run at once.
// It is a test tool: the programme reaches full scale (a 7.1 game scene whose
// fold goes over 0 dBFS, strip gain up to +6 dB, protection cycled through
// Off), so it belongs on an output nobody listens to.
//
// * Device: --device names the output exactly as the device type lists it;
//   --type the JUCE device type (default: the app's own first-run choice,
//   "Windows Audio (Low Latency Mode)" on Windows, else the first type).
//   The system default output (the one people listen to) is refused unless
//   --allow-audible is given (exit code 3, nothing opened).
//   The output is PINNED (AudioEngineHost::setOutputPin): the host only ever
//   asks for that output (never the system default or a first safe output),
//   and audio reaches no other one. JUCE itself may still open another device
//   (its own fallback when the pinned one fails or disappears); such a device
//   plays silence from its first callback, is closed again by the host's
//   next message-thread pass, and ends the soak (exit code 4). An output the
//   type does not list is not opened at all (exit code 3). --buffer picks the
//   buffer size (min: the smallest the device offers; default: the device's
//   own), --rate the sample rate.
// * Programme: the strips are fed inside the device callback
//   (AudioEngineHost::setDeviceSignalSource) by the app's TestSignalGenerator:
//   the 7.1 game scene on the Game strip and the music on the Music strip,
//   each at -6 dB; Chat and System stay silent.
// * Automation (--automation user, the default): from warmupSeconds on, one
//   host action every --interval ms +-50 % (seeded), through the same
//   EngineController calls the UI and the hotkeys make, on the Game or the
//   Music strip: a factory preset, Boost, a macro, the master bypass, the
//   strip bypass, the A/B bank, strip mute, strip gain, the latency profile
//   (a crossfaded engine swap), the mode, a module "ear" (audition bypass),
//   Night, Focus, protection strength and Smart macros. The kinds are drawn
//   from a seeded bag, so every kind comes up in every 20 actions. Each
//   action is logged with the stream frame it was applied at.
// * Watched: the engine's output as the callback hands it to the device
//   (after the loopback guard and the output trim), copied in the callback
//   into a preallocated flub::StreamTap (AudioEngineHost::setOutputTap) and
//   read on the message thread by a flub::DiscontinuityDetector (clicks,
//   dropouts, NaN / Inf, DC steps; restarted at a tap gap). What the device
//   then does with it is not seen: a device underrun (a late callback the
//   device could not cover) leaves no trace in the tap, so "0 dropouts"
//   speaks for the engine's output only; late callbacks are the proxy. A
//   second detector reads the dry programme (the same generator, rendered
//   again) as a self-check.
//   Also: the callback timing (flub::CallbackTiming: duration and interval
//   percentiles, over-budget and late callbacks with their times), the
//   device's xrun count (-1: the device type reports none) and JUCE's glitch
//   count, JUCE's CPU load, this process's CPU and the system's, device
//   starts and errors, the overload watchdog, engine swaps, and the private
//   bytes / working set every 10 s (growth after the first minute).
// * Triage: every detection is reported with the action before it and its
//   age, whether the master bypass was engaged, and a class:
//     restart    within 1 s after a device (re)start
//     gap        at a gap in the tap (the analysis fell behind)
//     headroom   the 7.1 fold's zero-latency headroom limiter acted just
//                before it (MeterBus::foldHeadroomDb, docs/11 E28a; read after
//                every block on the virtual device, at 50 Hz on a real one)
//     transition within kTransitionMs after an action (named)
//     programme  the dry programme has a break there too
//     static     none of these
// * Report: --report <file.json> (default: device-soak-<time>.json in the
//   current folder) and a human summary, printed and written next to it as
//   .txt. Exit code: 0 nothing found, 1 findings (any detection, late or
//   over-budget callback, xrun, device error, tap drop), 2 bad arguments,
//   3 the device could not be opened (or was refused), 4 aborted (pin,
//   stall, closed). The app is a windowed program: run it through
//   tools/scripts/device-soak.py or with its output redirected, so the shell
//   waits for it and keeps what it prints (an interactive prompt does not
//   wait; started with no standard output at all, it attaches to the console
//   of the process that started it).
// * The end: the soak closes its device (the host's callback is removed,
//   which waits for a callback in flight, and the backend's thread ends)
//   before it detaches and frees the programme and the tap, so no callback,
//   not even one stalled beyond the host's bounded wait, can still use them.
// * --replay re-runs a report's session on a virtual device (no hardware,
//   faster than real time): the same programme, buffer size and rate, every
//   logged action at its logged frame. Detections that come back (same
//   type, within one block and 3 ms of the same programme frame; levels are
//   not compared) are the processing's own response (or the action's);
//   those that do not came from the real-time path. The replay's report
//   lists both. A report whose actions name a strip or a parameter that
//   does not exist is refused (exit code 2).
// * --dump writes, for each programme time given (seconds >= 0), the device
//   output from 0.5 s before to 0.5 s after it (<report>-dump-<t>.wav, 32-bit
//   float, for `flubsound-cli analyze --glitches`; shorter when the stream
//   started less than 0.5 s before it) and the Game and Music strips' states
//   with both banks as the stream passed it (<report>-dump-<t>-strips.json).
//   The report's "dumps" lists what was written.
// * --list prints the device types and their outputs and, for --device, the
//   buffer sizes and rates it offers and whether a soak would accept it.
//   Nothing is opened or played.
#pragma once

#include "engine/EngineController.h"
#include "engine/TestSignalGenerator.h"

#include "flub/analysis/Discontinuity.h"
#include "flub/analysis/StreamTap.h"
#include "flub/common/Math.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace flub::app
{
struct DeviceSoakOptions
{
    juce::String device;         // --device: the output, as the device type lists it
    juce::String type;           // --type: a JUCE device type; empty = the app's default
    int bufferSize = 0;          // --buffer <n>; 0 = the device's default
    bool smallestBuffer = false; // --buffer min
    double sampleRate = 0.0;     // --rate; 0 = the device's default
    double minutes = 10.0;       // --minutes
    juce::File report;           // --report (empty: device-soak-<time>.json in the current folder)
    flub::param::LatencyProfileValue profile = flub::param::LatencyProfileValue::Balanced; // --profile
    uint32_t seed = 1;           // --seed
    double intervalMs = 2000.0;  // --interval: mean time between actions (+-50 %)
    bool automation = true;      // --automation user | off
    bool ui = false;             // --ui: show the main window as well
    bool list = false;           // --list
    juce::File replay;           // --replay <report.json>
    std::vector<double> dumpAt;  // --dump t[,t...]: programme seconds (>= 0) to write the output around (+-0.5 s)
    bool allowAudible = false;   // --allow-audible: the system default output may be soaked too

    // Not on the command line (tests; a report carries them, so --replay repeats them):
    double warmupSeconds = 5.0;         // no action before this
    double injectPulseAtSeconds = -1.0; // a one-sample impulse of +0.5 in the Music strip's input (programme time)
};

/** Parses the soak arguments; false if --device-soak is absent (error stays
    empty) or malformed (error set). */
bool parseDeviceSoakCommandLine (const juce::StringArray& args, DeviceSoakOptions& options, juce::String& error);

/** One automation action, as applied (the value it SET, so a replay repeats it). */
struct SoakAction
{
    enum class Kind
    {
        Preset,
        Boost,
        Macro,
        Bypass,      // master enable (value 1 = bypassed)
        StripBypass, // value 1 = bypassed
        Bank,        // value 0 = A, 1 = B
        Mute,        // value 1 = muted
        Gain,        // strip gain, dB
        Profile,     // latency profile index
        Mode,        // mode index
        Audition,    // module ear: param = the enable id, value 1 = held
        Night,       // value 1 = on
        Focus,       // value 1 = on
        Protection,  // flub::ProtectionStrength index
        Smart,       // value 1 = on
        Param        // any parameter (param = its id) of the strip's active bank; replay scripts only (triage), never drawn
    };
    static constexpr int kNumKinds = 16;
    static constexpr int kNumDrawnKinds = 15; // the automation draws the kinds before Param

    Kind kind = Kind::Boost;
    int64_t frame = 0;         // when it was due (stream frames)
    int64_t appliedFrame = -1; // the stream position when it was applied
    int strip = 0;
    int param = -1;            // Macro / Audition: the parameter id
    float value = 0.0f;
    juce::String preset;       // Preset: the preset id
    juce::String presetName;   // Preset: for the log

    static const char* kindName (Kind kind) noexcept;
    static std::optional<Kind> kindFromName (const juce::String& name);
    /** "boost Music 0.62", "preset Game Competitive FPS", "profile Low Latency". */
    juce::String describe (const EngineController& controller) const;
};

/** A virtual output device (no hardware): the soak's --replay and the tests.
    Its callback runs on whichever thread calls process(). */
class SoakVirtualDevice final : public juce::AudioIODevice
{
public:
    SoakVirtualDevice (const juce::String& outputName, const juce::String& typeName);
    ~SoakVirtualDevice() override;

    juce::StringArray getOutputChannelNames() override { return { "Left", "Right" }; }
    juce::StringArray getInputChannelNames() override { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return { 44100.0, 48000.0, 96000.0 }; }
    juce::Array<int> getAvailableBufferSizes() override { return { 32, 64, 128, 144, 256, 441, 480, 512, 1024 }; }
    int getDefaultBufferSize() override { return 480; }
    juce::String open (const juce::BigInteger& inputChannels, const juce::BigInteger& outputChannels, double sampleRate, int bufferSizeSamples) override;
    void close() override;
    bool isOpen() override { return opened; }
    void start (juce::AudioIODeviceCallback* callback) override;
    void stop() override;
    bool isPlaying() override { return callback != nullptr; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return blockSize; }
    double getCurrentSampleRate() override { return rate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return activeOutputs; }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return blockSize; }
    int getInputLatencyInSamples() override { return 0; }

    /** One callback of the current buffer size; false while not started. */
    bool process();
    /** The last callback's output, channel 0 / 1. */
    const float* getOutput (int channel) const noexcept { return outputs[static_cast<size_t> (channel & 1)].data(); }

private:
    juce::AudioIODeviceCallback* callback = nullptr;
    juce::BigInteger activeOutputs;
    std::array<std::vector<float>, 2> outputs;
    double rate = 48000.0;
    int blockSize = 480;
    bool opened = false;
};

/** The device type of SoakVirtualDevice: its outputs by name (the first is
    the default). */
class SoakVirtualDeviceType final : public juce::AudioIODeviceType
{
public:
    static constexpr const char* kTypeName = "Flubsound Soak (virtual)";
    static constexpr const char* kOutputName = "Virtual Output";

    explicit SoakVirtualDeviceType (juce::StringArray outputNames = { kOutputName });

    void scanForDevices() override {}
    juce::StringArray getDeviceNames (bool wantInputNames) const override { return wantInputNames ? juce::StringArray() : outputs; }
    int getDefaultDeviceIndex (bool forInput) const override { return forInput ? -1 : 0; }
    int getIndexOfDevice (juce::AudioIODevice* device, bool asInput) const override;
    bool hasSeparateInputsAndOutputs() const override { return true; }
    juce::AudioIODevice* createDevice (const juce::String& outputDeviceName, const juce::String& inputDeviceName) override;

private:
    juce::StringArray outputs;
};

class DeviceSoak final : private juce::Timer, private juce::AsyncUpdater
{
public:
    enum class Clock
    {
        Device, // a real device's thread runs the callbacks; the message thread watches (50 Hz)
        Virtual // SoakVirtualDevice: this object runs the callbacks itself, as fast as it can
    };

    /** Called once on the message thread when the soak ended (report written
        when a file was given). */
    using Completion = std::function<void (int exitCode, const juce::String& summary)>;

    /** The engine options a soak runs with: temporary `settingsFile` (never
        written), the device state for options.device / type / buffer / rate
        (or the virtual device), the output pinned, no routing, no automatic
        profiles, no Tournament mode switch. Virtual: the virtual device type is
        added and no device watcher runs. */
    static EngineController::Options makeEngineOptions (const DeviceSoakOptions& options, Clock clock, const juce::File& settingsFile);

    /** Device clock: checks that options.type lists options.device, that it
        is not the type's system default output (unless options.allowAudible),
        and fills in the device type's name when it was empty and the buffer
        size for --buffer min. Opens nothing. False with `error` otherwise.
        `manager`: the device types to look in (tests); nullptr = a fresh
        AudioDeviceManager's default types. */
    static bool resolveDevice (DeviceSoakOptions& options, juce::String& error, juce::AudioDeviceManager* manager = nullptr);
    /** --list: what resolveDevice would see, as text, and for --device
        whether it accepts it. Opens nothing. */
    static juce::String listDevices (const DeviceSoakOptions& options);

    /** --replay: the options and the action log of a report. False with `error`
        when the file is not a device-soak report, or when an action names a
        strip outside 0 .. kMaxStrips - 1, a parameter its kind does not take
        (a macro, a module ear, a parameter id) or a value that is not finite. */
    static bool readReplay (const juce::File& reportFile, DeviceSoakOptions& options, std::vector<SoakAction>& actions, juce::String& error);

    /** The controller is the soak's own (makeEngineOptions): when the soak
        ends (finish, or destruction while running) it closes the
        controller's device. */
    DeviceSoak (EngineController& controller, DeviceSoakOptions options, Clock clock, Completion onFinished);
    ~DeviceSoak() override;

    /** Replay: the actions to apply instead of the generated automation. */
    void setScriptedActions (std::vector<SoakAction> actions);
    /** Replay: the report it replays; the replay's report then says which of
        its detections came back (same type, within a block and 3 ms of the
        same programme frame). */
    void setReplayReference (const juce::var& originalReport) { replayReference = originalReport; }

    /** Sets up the scene and the hooks and starts. False with `error` when no
        device runs (Device) or the virtual device is not open (Virtual). */
    bool start (juce::String& error);
    /** Ends the soak now (reason: why, for the report; empty = completed). */
    void stop (const juce::String& reason);

    bool isStarted() const noexcept { return started; }
    bool isFinished() const noexcept { return finished; }
    int getExitCode() const noexcept { return exitCode; }
    /** The report (a JSON object) once finished. */
    const juce::var& getReport() const noexcept { return report; }
    juce::String getSummary() const { return summary; }
    const std::vector<SoakAction>& getActions() const noexcept { return actions; }

    /** The transition window of the triage. */
    static constexpr double kTransitionMs = 500.0;

private:
    class Programme;
    struct Analysis;

    void timerCallback() override;
    void handleAsyncUpdate() override;
    void setUpScene();
    void drainTap();
    void serviceDumps();
    void applyDueActions (int64_t position);
    SoakAction nextAction (int64_t frame);
    void apply (SoakAction& action);
    void scheduleNext (int64_t after);
    void sampleStatus (bool force);
    void sampleHeadroom();
    void sampleProcess (double elapsedSeconds);
    void pumpVirtual();
    void releaseHooks();
    void finish (const juce::String& reason);
    void buildReport (const juce::String& reason);
    juce::String classify (int64_t frame, flub::DiscontinuityType type, juce::String& lastAction, double& ageMs, bool& bypassed) const;
    double secondsAt (int64_t frame) const noexcept { return static_cast<double> (frame) / sampleRate; }
    int64_t framesFor (double seconds) const noexcept { return static_cast<int64_t> (seconds * sampleRate + 0.5); }

    EngineController& controller;
    DeviceSoakOptions options;
    Clock clock;
    Completion onFinished;

    double sampleRate = 48000.0;
    int blockSize = 0;
    int64_t totalFrames = 0;
    int gameStrip = 0, musicStrip = 1;

    std::unique_ptr<Programme> programme;    // the device callback's source
    flub::StreamTap tap;
    std::unique_ptr<Analysis> analysis;
    bool hooksAttached = false;              // the host's callback may use `programme` and `tap` (releaseHooks)
    int virtualIdleSteps = 0;                // virtual clock: steps in a row without a new frame

    // Automation
    bool scripted = false;
    std::vector<SoakAction> script;
    size_t scriptPos = 0;
    std::vector<SoakAction> actions;          // applied, in order
    std::vector<int> bag;                     // kinds still to draw this round
    flub::FastRandom rng { 1 };
    int64_t nextActionFrame = -1;
    std::array<int, AudioEngineHost::kMaxStrips> heldAudition {}; // the module ear held per strip (param id, -1: none)
    std::vector<flub::app::PresetInfo> factoryPresets;

    // Device and timing
    juce::String deviceName, deviceType;
    uint32_t startCount0 = 0, errorCount0 = 0;
    std::vector<int64_t> deviceStartFrames;   // tap position seen at each device (re)start after the first
    uint32_t seenStarts = 0;
    juce::StringArray deviceErrors;
    juce::String lastErrorSeen;
    flub::CallbackTiming::Snapshot timing0, timingPrev;
    int64_t lastLate = 0, lastOverBudget = 0;
    std::vector<std::pair<double, double>> lateTimes;       // (seconds, the window's longest interval ms)
    std::vector<std::pair<double, double>> overBudgetTimes; // (seconds, the window's peak load)
    double worstWindowLoad = 0.0, worstWindowSeconds = 0.0;
    double cpuSum = 0.0, cpuMax = 0.0;
    int cpuSamples = 0;
    int xrunsDevice0 = -1, xrunsDevice = -1, glitches0 = 0, glitches = 0;
    uint32_t swaps0 = 0;
    uint64_t watchdogEpisodes0 = 0;

    // Process statistics
    juce::Time startedAt;
    double wallStartMs = 0.0, lastStatusMs = 0.0, lastProcessMs = 0.0, lastProgressMs = 0.0;
    double cpu0 = -1.0, busy0 = -1.0, total0 = -1.0;
    double cpuLast = -1.0, busyLast = -1.0, totalLast = -1.0;
    struct MemorySample
    {
        double seconds = 0.0, privateMB = 0.0, workingSetMB = 0.0;
    };
    std::vector<MemorySample> memory;
    int64_t lastFramesSeen = 0;
    double lastFramesChangeMs = 0.0;

    // The fold's zero-latency headroom limiter (MeterBus::foldHeadroomDb).
    struct HeadroomEvent
    {
        int64_t frame = 0; // the stream position after the block it acted in
        int strip = 0;
        double db = 0.0;
    };
    std::vector<HeadroomEvent> headroomEvents;
    int64_t headroomBlocks = 0;
    double headroomMinDb = 0.0;

    struct Dump
    {
        double seconds = 0.0;
        juce::String states; // the strips' states as the stream passed it
        bool written = false;
        juce::String file;           // the WAV's file name
        double fromSeconds = 0.0;    // the WAV's span, programme time
        double toSeconds = 0.0;
    };
    std::vector<Dump> dumps;

    bool started = false, finished = false;
    int exitCode = 0;
    juce::var report, replayReference;
    juce::String summary;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceSoak)
};
} // namespace flub::app
