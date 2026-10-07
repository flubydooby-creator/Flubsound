// App-level tests: docs/11 E53, the headless real-device soak
// (shell/DeviceSoak.h) and its hooks in AudioEngineHost (REAL-DEVICE SOAK
// HOOKS), on the soak's virtual device (no hardware):
//   * the command line;
//   * the device signal source and the output tap inside the device callback:
//     no allocation, no free, no lock (Linux), and the tap holds exactly what
//     the device was handed;
//   * the output pin: a device started under another name plays silence and
//     is closed, the pinned one is opened again;
//   * a short soak through the full EngineController: the report's fields,
//     every automation kind applied, engine swaps from the profile switches;
//   * an injected pulse is found and classed "static" (no action near it),
//     and "transition" 100 ms after a scripted action;
//   * --replay of a virtual run reproduces its detections (an injected pulse
//     among them) and actions; a damaged report is refused;
//   * --dump right at the start of the stream (shorter file, no crash);
//   * the system default output is refused without --allow-audible;
//   * a stopped virtual device ends the soak (aborted) instead of spinning;
//   * a stalled callback: clearing the hooks says so, and closing the device
//     waits for it (what the soak's end relies on).
#include "AppTestSupport.h"

#include "engine/AudioEngineHost.h"
#include "engine/TestSignalGenerator.h"
#include "shell/DeviceSoak.h"

#include "flub/io/WavFile.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

using namespace flub::app;

namespace
{
juce::String text (const juce::var& v, const char* key)
{
    return v.getProperty (key, {}).toString();
}

/** A soak on the virtual device through a full EngineController; returns the report. */
struct VirtualSoak
{
    flubapptest::TempFolder folder;
    std::unique_ptr<EngineController> controller;
    std::unique_ptr<DeviceSoak> soak;
    int exitCode = -1;
    std::function<void (EngineController&)> afterStart; // tests: called once the soak has started

    juce::var run (DeviceSoakOptions options, const std::vector<SoakAction>* script = nullptr, const juce::var& reference = {})
    {
        controller = std::make_unique<EngineController> (DeviceSoak::makeEngineOptions (options, DeviceSoak::Clock::Virtual, folder.file ("settings.xml")));
        soak = std::make_unique<DeviceSoak> (*controller, options, DeviceSoak::Clock::Virtual, [this] (int code, const juce::String&) { exitCode = code; });
        if (script != nullptr)
        {
            soak->setScriptedActions (*script);
            soak->setReplayReference (reference);
        }
        juce::String error;
        if (! soak->start (error))
        {
            std::cerr << "    soak did not start: " << error << "\n";
            return {};
        }
        if (afterStart)
            afterStart (*controller);
        flubapptest::pumpMessagesUntil ([this] { return soak->isFinished(); }, 20000);
        return soak->getReport();
    }

    ~VirtualSoak()
    {
        soak.reset();
        if (controller != nullptr)
            controller->shutdown();
    }
};

DeviceSoakOptions shortOptions (double minutes)
{
    DeviceSoakOptions o;
    o.device = SoakVirtualDeviceType::kOutputName;
    o.minutes = minutes;
    o.bufferSize = 480;
    o.sampleRate = 48000.0;
    o.warmupSeconds = 0.3;
    o.intervalMs = 60.0;
    o.seed = 7;
    return o;
}
} // namespace

TEST_CASE ("App: E53 device soak: command line")
{
    DeviceSoakOptions o;
    juce::String error;
    CHECK (! parseDeviceSoakCommandLine ({ "--screenshot", "x.png" }, o, error));
    CHECK (error.isEmpty());

    REQUIRE (parseDeviceSoakCommandLine ({ "--device-soak", "--device", "Digital Audio (S/PDIF) (High Definition Audio Device)", "--type",
                                           "Windows Audio (Low Latency Mode)", "--buffer", "min", "--minutes", "10", "--profile", "low", "--seed",
                                           "42", "--interval", "1500", "--automation", "off", "--report", "soak.json" },
                                         o, error));
    CHECK (o.device == "Digital Audio (S/PDIF) (High Definition Audio Device)");
    CHECK (o.type == "Windows Audio (Low Latency Mode)");
    CHECK (o.smallestBuffer);
    CHECK (o.minutes == 10.0);
    CHECK (o.profile == flub::param::LatencyProfileValue::LowLatency);
    CHECK (o.seed == 42u);
    CHECK (o.intervalMs == 1500.0);
    CHECK (! o.automation);
    CHECK (o.report.getFileName() == "soak.json");

    DeviceSoakOptions b;
    REQUIRE (parseDeviceSoakCommandLine ({ "--device-soak", "--device", "X", "--buffer", "144" }, b, error));
    CHECK (b.bufferSize == 144);
    CHECK (! b.smallestBuffer);
    CHECK (b.type.isEmpty()); // resolveDevice picks the app's default type

    CHECK (! b.allowAudible);
    CHECK (b.dumpAt.empty());

    DeviceSoakOptions d;
    REQUIRE (parseDeviceSoakCommandLine ({ "--device-soak", "--device", "X", "--dump", "0.1, 199.68,0", "--allow-audible" }, d, error));
    REQUIRE (d.dumpAt.size() == 3);
    CHECK (std::abs (d.dumpAt[0] - 0.1) < 1.0e-9);
    CHECK (std::abs (d.dumpAt[1] - 199.68) < 1.0e-9);
    CHECK (d.dumpAt[2] == 0.0);
    CHECK (d.allowAudible);

    // A reference, not a copy, per row (gcc / clang -Wrange-loop-construct).
    for (const auto& bad : { juce::StringArray { "--device-soak" },                                   // no device
                             juce::StringArray { "--device-soak", "--device", "X", "--buffer", "8" },  // too small
                             juce::StringArray { "--device-soak", "--device", "X", "--buffer", "abc" },
                             juce::StringArray { "--device-soak", "--device", "X", "--profile", "fast" },
                             juce::StringArray { "--device-soak", "--device", "X", "--minutes", "0" },
                             juce::StringArray { "--device-soak", "--device", "X", "--automation", "all" },
                             juce::StringArray { "--device-soak", "--device", "X", "--dump", "-1" },     // before the stream
                             juce::StringArray { "--device-soak", "--device", "X", "--dump", "abc" },    // was read as 0
                             juce::StringArray { "--device-soak", "--device", "X", "--dump", "1,x" },
                             juce::StringArray { "--device-soak", "--device", "X", "--dump", "1e3" },
                             juce::StringArray { "--device-soak", "--device", "X", "--dump", "1.2.3" },
                             juce::StringArray { "--device-soak", "--device", "X", "--dump" },
                             juce::StringArray { "--device-soak", "--replay" } })
    {
        DeviceSoakOptions x;
        juce::String e;
        CHECK (! parseDeviceSoakCommandLine (bad, x, e));
        CHECK (e.isNotEmpty());
    }
    DeviceSoakOptions list;
    CHECK (parseDeviceSoakCommandLine ({ "--device-soak", "--list" }, list, error));
    CHECK (list.list);

    // Every action kind has a name that reads back (the report's log, --replay).
    for (int k = 0; k < SoakAction::kNumKinds; ++k)
    {
        const auto kind = static_cast<SoakAction::Kind> (k);
        const auto back = SoakAction::kindFromName (SoakAction::kindName (kind));
        CHECK (back.has_value() && *back == kind);
    }
    CHECK (! SoakAction::kindFromName ("teleport").has_value());

    // --replay reads device-soak reports only.
    flubapptest::TempFolder folder;
    const auto notAReport = folder.file ("other.json");
    notAReport.replaceWithText ("{\"flubsound\": 1}");
    DeviceSoakOptions replayed;
    std::vector<SoakAction> script;
    CHECK (! DeviceSoak::readReplay (notAReport, replayed, script, error));
    CHECK (error.contains ("not a device-soak report"));
}

TEST_CASE ("App: E53 device soak: --replay refuses a report whose actions name a strip or a parameter that does not exist")
{
    flubapptest::TempFolder folder;
    const auto reportWith = [&folder] (const juce::String& action)
    {
        const auto file = folder.file ("report.json");
        file.replaceWithText ("{\"flubsoundDeviceSoak\": 1, \"config\": {\"device\": \"Virtual Output\", \"type\": \"x\", \"seed\": 7},"
                              " \"device\": {\"bufferSize\": 480, \"sampleRate\": 48000}, \"framesAnalysed\": 48000, \"programmeStartFrame\": 0,"
                              " \"actions\": [{\"kind\": \"boost\", \"appliedFrame\": 4800, \"strip\": 1, \"param\": -1, \"value\": 0.25}, "
                              + action + "]}");
        return file;
    };
    struct Row
    {
        juce::String action;
        bool valid;
        juce::String says;
    };
    const Row rows[] = {
        { "{\"kind\": \"macro\", \"appliedFrame\": 9600, \"strip\": 1, \"param\": -100000, \"value\": 0.5}", false, "not a macro" },
        { "{\"kind\": \"audition\", \"appliedFrame\": 9600, \"strip\": 99, \"param\": 0, \"value\": 1}", false, "strip 99" },
        { "{\"kind\": \"audition\", \"appliedFrame\": 9600, \"strip\": 1, \"param\": -3, \"value\": 1}", false, "not a module's ear" },
        { "{\"kind\": \"param\", \"appliedFrame\": 9600, \"strip\": 0, \"param\": 100000, \"value\": 1}", false, "parameter 100000" },
        { "{\"kind\": \"gain\", \"appliedFrame\": 9600, \"strip\": -1, \"param\": -1, \"value\": 3}", false, "strip -1" },
        { "{\"kind\": \"macro\", \"appliedFrame\": 9600, \"strip\": 1, \"param\": " + juce::String (flub::param::Macro3) + ", \"value\": 0.5}", true, {} },
    };
    for (const auto& row : rows)
    {
        DeviceSoakOptions o;
        std::vector<SoakAction> script;
        juce::String error;
        const bool ok = DeviceSoak::readReplay (reportWith (row.action), o, script, error);
        CHECK (ok == row.valid);
        if (row.valid)
            CHECK (script.size() == 2);
        else
        {
            CHECK (script.empty());
            if (! error.contains (row.says))
                std::cerr << "    error: " << error << "\n";
            CHECK (error.contains (row.says));
        }
    }

    // describe() of an action no report could carry indexes nothing out of range.
    EngineController::Options eo;
    eo.openAudioDevice = false;
    eo.restoreState = false;
    eo.enableAppRouting = false;
    eo.persistSettings = false;
    eo.settingsFile = folder.file ("settings.xml");
    EngineController controller (eo);
    SoakAction a;
    a.kind = SoakAction::Kind::Macro;
    a.strip = 1;
    a.param = -100000;
    a.value = 0.5f;
    CHECK (a.describe (controller).contains ("-100000"));
    a.kind = SoakAction::Kind::Audition;
    a.param = 1 << 20;
    CHECK (a.describe (controller).contains (juce::String (1 << 20)));
    controller.shutdown();
}

TEST_CASE ("App: E53 device soak: the device signal source and the output tap allocate, free and lock nothing in the callback")
{
    AudioEngineHost host;
    host.getDeviceManager().addAudioDeviceType (std::make_unique<SoakVirtualDeviceType>());
    host.setDeviceWatcher (nullptr);
    host.setOutputPin (SoakVirtualDeviceType::kOutputName);
    juce::XmlElement saved ("DEVICESETUP");
    saved.setAttribute ("deviceType", SoakVirtualDeviceType::kTypeName);
    saved.setAttribute ("audioOutputDeviceName", SoakVirtualDeviceType::kOutputName);
    saved.setAttribute ("audioDeviceBufferSize", 256);
    REQUIRE (host.openDevice (&saved, 0, 2).isEmpty());
    REQUIRE (host.getDeviceManager().getCurrentAudioDevice() != nullptr);
    CHECK (! host.isPinBlocked());

    TestSignalGenerator generator (48000.0);
    generator.setProgramme (0, TestSignalGenerator::Programme::Game71, -6.0f);
    generator.setProgramme (1, TestSignalGenerator::Programme::Music, -6.0f);
    flub::StreamTap tap;
    tap.prepare (48000);
    host.setOutputTap (&tap);
    host.setDeviceSignalSource (&generator);

    constexpr int kBlock = 256;
    std::array<std::vector<float>, 2> out { std::vector<float> (kBlock), std::vector<float> (kBlock) };
    const std::array<float*, 2> outs { out[0].data(), out[1].data() };
    const auto callback = [&]
    {
        host.audioDeviceIOCallbackWithContext (nullptr, 0, outs.data(), 2, kBlock, juce::AudioIODeviceCallbackContext {});
    };
    for (int b = 0; b < 20; ++b) // warm-up: the thread's promotion, the start-up fade
        callback();

    double energy = 0.0;
    {
        flubapptest::RealtimeProbe probe;
        for (int b = 0; b < 200; ++b)
        {
            callback();
            for (const float v : out[0])
                energy += static_cast<double> (v) * v;
        }
        CHECK (probe.allocations() == 0);
        CHECK (probe.deallocations() == 0);
        if (flubapptest::lockCountingAvailable())
            CHECK (probe.locks() == 0);
    }
    CHECK (energy > 1.0e-3); // the source fed the strips
    CHECK (tap.framesWritten() == 220 * kBlock);
    CHECK (tap.framesDropped() == 0);

    // The tap holds what the device was handed (the last chunk = the last block's end).
    flub::StreamTap::Chunk chunk, last;
    int64_t read = 0;
    while (tap.read (chunk))
    {
        read += chunk.numFrames;
        last = chunk;
    }
    CHECK (read == 220 * kBlock);
    bool same = true;
    for (int i = 0; i < last.numFrames; ++i)
        same = same && last.samples[2 * i] == out[0][static_cast<size_t> (kBlock - last.numFrames + i)]
               && last.samples[2 * i + 1] == out[1][static_cast<size_t> (kBlock - last.numFrames + i)];
    CHECK (same);

    host.setDeviceSignalSource (nullptr);
    host.setOutputTap (nullptr);
    callback();
    CHECK (tap.framesWritten() == 220 * kBlock); // detached: nothing more
    host.closeDevice();
}

TEST_CASE ("App: E53 output pin: a device started under another name plays silence, is closed, and the pinned output comes back")
{
    AudioEngineHost host;
    host.getDeviceManager().addAudioDeviceType (std::make_unique<SoakVirtualDeviceType> (juce::StringArray { "Other Output", "Virtual Output" }));
    host.setDeviceWatcher (nullptr);
    host.setOutputPin ("Virtual Output");

    // Asked for the other output (as JUCE's own fallback would open it): the
    // selection replaces it with the pin before anything plays.
    juce::XmlElement saved ("DEVICESETUP");
    saved.setAttribute ("deviceType", SoakVirtualDeviceType::kTypeName);
    saved.setAttribute ("audioOutputDeviceName", "Other Output");
    host.openDevice (&saved, 0, 2);
    flubapptest::pumpMessagesUntil ([&] { return ! host.isPinBlocked(); }, 2000);
    auto* device = host.getDeviceManager().getCurrentAudioDevice();
    REQUIRE (device != nullptr);
    CHECK (device->getName() == "Virtual Output");

    TestSignalGenerator generator (48000.0);
    generator.setProgramme (1, TestSignalGenerator::Programme::Music, 0.0f);
    flub::StreamTap tap;
    tap.prepare (48000);
    host.setOutputTap (&tap);
    host.setDeviceSignalSource (&generator);
    for (int b = 0; b < 40; ++b)
        dynamic_cast<SoakVirtualDevice*> (host.getDeviceManager().getCurrentAudioDevice())->process();
    float peak = 0.0f;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 480; ++i)
            peak = std::max (peak, std::abs (dynamic_cast<SoakVirtualDevice*> (host.getDeviceManager().getCurrentAudioDevice())->getOutput (c)[i]));
    CHECK (peak > 0.01f); // the pinned output plays

    // JUCE starts the other output by itself (its fallback after a hot-unplug).
    auto setup = host.getDeviceManager().getAudioDeviceSetup();
    setup.outputDeviceName = "Other Output";
    host.getDeviceManager().setAudioDeviceSetup (setup, false);
    auto* other = dynamic_cast<SoakVirtualDevice*> (host.getDeviceManager().getCurrentAudioDevice());
    REQUIRE (other != nullptr);
    CHECK (other->getName() == "Other Output");
    CHECK (host.isPinBlocked());
    const auto before = tap.framesWritten();
    for (int b = 0; b < 5; ++b)
        other->process();
    float otherPeak = 0.0f;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < other->getCurrentBufferSizeSamples(); ++i)
            otherPeak = std::max (otherPeak, std::abs (other->getOutput (c)[i]));
    CHECK (otherPeak == 0.0f); // silence from its first callback
    CHECK (tap.framesWritten() > before);

    // The message thread closes it and the selection opens the pin again.
    flubapptest::pumpMessagesUntil ([&]
                                    {
                                        auto* d = host.getDeviceManager().getCurrentAudioDevice();
                                        return ! host.isPinBlocked() && d != nullptr && d->getName() == "Virtual Output";
                                    },
                                    2000);
    CHECK (host.getPinViolations() == 1);
    REQUIRE (host.getDeviceManager().getCurrentAudioDevice() != nullptr);
    CHECK (host.getDeviceManager().getCurrentAudioDevice()->getName() == "Virtual Output");

    host.setDeviceSignalSource (nullptr);
    host.setOutputTap (nullptr);
    host.closeDevice();
}

TEST_CASE ("App: E53 device soak: a short virtual run - the report's fields and every automation kind")
{
    VirtualSoak run;
    const auto report = run.run (shortOptions (0.04)); // 2.4 s at 48 kHz, an action every 60 ms +-50 % from 0.3 s to 2.16 s
    REQUIRE (report.isObject());
    CHECK (run.exitCode == 0 || run.exitCode == 1);
    CHECK (static_cast<int> (report.getProperty ("flubsoundDeviceSoak", 0)) == 1);
    CHECK (text (report, "clock") == "virtual");
    CHECK (text (report, "result") == "completed");
    CHECK (static_cast<juce::int64> (report.getProperty ("framesAnalysed", 0)) == 115200);
    CHECK (static_cast<juce::int64> (report.getProperty ("programmeStartFrame", -1)) >= 0);

    const auto device = report.getProperty ("device", {});
    CHECK (text (device, "name") == SoakVirtualDeviceType::kOutputName);
    CHECK (static_cast<int> (device.getProperty ("bufferSize", 0)) == 480);
    CHECK (static_cast<double> (device.getProperty ("sampleRate", 0.0)) == 48000.0);
    CHECK (static_cast<int> (device.getProperty ("restarts", -1)) == 0); // started before the soak; none since
    CHECK (static_cast<int> (device.getProperty ("errors", -1)) == 0);
    CHECK (static_cast<int> (device.getProperty ("pinViolations", -1)) == 0);

    const auto timing = report.getProperty ("timing", {});
    CHECK (static_cast<juce::int64> (timing.getProperty ("callbacks", 0)) >= 240);
    for (const auto* key : { "durationP999Ms", "durationMaxMs", "intervalMaxMs", "late", "overBudget", "xrunsDevice", "glitchesJuce", "cpuLoadMean",
                             "processCpuPercent", "systemCpuPercent", "lateAt", "overBudgetAt" })
        CHECK (timing.hasProperty (key));

    const auto automation = report.getProperty ("automation", {});
    CHECK (static_cast<int> (automation.getProperty ("actions", 0)) >= 20);
    const auto perKind = automation.getProperty ("perKind", {});
    for (int k = 0; k < SoakAction::kNumDrawnKinds; ++k) // not "param": replay scripts only
    {
        const auto* name = SoakAction::kindName (static_cast<SoakAction::Kind> (k));
        const int count = static_cast<int> (perKind.getProperty (name, 0));
        if (count < 1)
            std::cerr << "    no '" << name << "' action\n";
        CHECK (count >= 1);
    }
    const auto* actions = report.getProperty ("actions", {}).getArray();
    REQUIRE (actions != nullptr);
    int64_t lastFrame = -1;
    bool ordered = true;
    for (const auto& a : *actions)
    {
        const auto applied = static_cast<juce::int64> (a.getProperty ("appliedFrame", -1));
        ordered = ordered && applied >= lastFrame && applied >= static_cast<juce::int64> (a.getProperty ("frame", 0));
        lastFrame = applied;
    }
    CHECK (ordered);
    // Every profile switch ran as a crossfaded engine swap.
    if (static_cast<int> (perKind.getProperty ("profile", 0)) > 0)
        CHECK (static_cast<int> (report.getProperty ("engine", {}).getProperty ("swapsCompleted", 0)) >= 1);

    const auto dis = report.getProperty ("discontinuities", {});
    for (const auto* key : { "click", "dropout", "non-finite", "dc-step", "kinks", "recurring", "tapGaps", "tapFramesDropped", "perClass", "events",
                             "dryProgramme", "outputPeakDbfs" })
        CHECK (dis.hasProperty (key));
    CHECK (static_cast<int> (dis.getProperty ("non-finite", -1)) == 0);
    CHECK (static_cast<int> (dis.getProperty ("tapGaps", -1)) == 0);
    CHECK (static_cast<double> (dis.getProperty ("outputPeakDbfs", 0.0)) > -40.0); // the programme played

    const auto memory = report.getProperty ("memory", {});
    CHECK (memory.hasProperty ("privateStartMB"));
    CHECK (memory.hasProperty ("privateEndMB"));
    REQUIRE (memory.getProperty ("samples", {}).getArray() != nullptr);
    CHECK (memory.getProperty ("samples", {}).getArray()->size() >= 2);
    CHECK (run.soak->getSummary().contains ("verdict"));
    // The soak's end closes its device (before the programme and the tap go).
    CHECK (run.controller->getDeviceManager().getCurrentAudioDevice() == nullptr);
}

TEST_CASE ("App: E53 device soak: an injected pulse is found and classed static")
{
    auto options = shortOptions (0.03); // 1.8 s
    options.automation = false;
    options.injectPulseAtSeconds = 1.05; // between the game's shots and blasts, after a footstep has died away
    VirtualSoak run;
    const auto report = run.run (options);
    REQUIRE (report.isObject());
    CHECK (run.exitCode == 1); // a finding
    const auto start = static_cast<juce::int64> (report.getProperty ("programmeStartFrame", 0));
    const auto* events = report.getProperty ("discontinuities", {}).getProperty ("events", {}).getArray();
    REQUIRE (events != nullptr);
    int found = 0;
    for (const auto& e : *events)
    {
        const auto frame = static_cast<juce::int64> (e.getProperty ("frame", 0)) - start;
        if (frame >= 50400 && frame <= 50400 + 4800) // within 100 ms (the engine's latency)
        {
            ++found;
            CHECK (text (e, "class") == "static");
            CHECK (text (e, "lastAction").isEmpty());
        }
    }
    CHECK (found >= 1);
}

TEST_CASE ("App: E53 device soak: a pulse 100 ms after a scripted action is classed transition and names the action")
{
    auto options = shortOptions (0.03); // 1.8 s
    options.automation = false;
    options.injectPulseAtSeconds = 1.05; // as in the static case above
    SoakAction protection;               // a global action (no strip), 100 ms before the pulse
    protection.kind = SoakAction::Kind::Protection;
    protection.frame = 45600; // 0.95 s, programme time
    protection.strip = 0;
    protection.value = 1.0f; // Normal
    const std::vector<SoakAction> script { protection };
    VirtualSoak run;
    const auto report = run.run (options, &script);
    REQUIRE (report.isObject());
    REQUIRE (run.soak->getActions().size() == 1);
    const auto start = static_cast<juce::int64> (report.getProperty ("programmeStartFrame", 0));
    const auto* events = report.getProperty ("discontinuities", {}).getProperty ("events", {}).getArray();
    REQUIRE (events != nullptr);
    int found = 0;
    for (const auto& e : *events)
    {
        const auto frame = static_cast<juce::int64> (e.getProperty ("frame", 0)) - start;
        if (frame >= 50400 && frame <= 50400 + 4800)
        {
            ++found;
            CHECK (text (e, "class") == "transition");
            CHECK (text (e, "lastAction") == "protection Normal");
            const double age = static_cast<double> (e.getProperty ("lastActionAgeMs", -1.0));
            CHECK (age >= 90.0);  // the pulse 100 ms after the action, plus the engine's latency
            CHECK (age <= 250.0); // well inside the 500 ms transition window
        }
    }
    CHECK (found >= 1);
    CHECK (static_cast<int> (report.getProperty ("discontinuities", {}).getProperty ("perClass", {}).getProperty ("transition", 0)) >= 1);
}

TEST_CASE ("App: E53 device soak: --replay of a virtual run reproduces its actions and detections")
{
    flubapptest::TempFolder folder;
    auto options = shortOptions (0.02); // 1.2 s: about 10 actions
    options.intervalMs = 80.0;
    // A detection the replay must bring back (without it the run has none and
    // the comparison below could not fail): before the first action, so no
    // mute or bypass can hide it.
    options.injectPulseAtSeconds = 0.2;
    options.report = folder.file ("original.json");
    VirtualSoak original;
    const auto first = original.run (options);
    REQUIRE (first.isObject());
    REQUIRE (options.report.existsAsFile());
    CHECK (options.report.withFileExtension ("txt").existsAsFile());
    const auto* firstEvents = first.getProperty ("discontinuities", {}).getProperty ("events", {}).getArray();
    REQUIRE (firstEvents != nullptr);
    REQUIRE (! firstEvents->isEmpty());

    DeviceSoakOptions replayed;
    std::vector<SoakAction> script;
    juce::String error;
    REQUIRE (DeviceSoak::readReplay (options.report, replayed, script, error));
    CHECK (replayed.bufferSize == 480);
    CHECK (replayed.sampleRate == 48000.0);
    CHECK (replayed.injectPulseAtSeconds == 0.2); // the report carries it
    CHECK (script.size() == original.soak->getActions().size());

    VirtualSoak again;
    const auto second = again.run (replayed, &script, first);
    REQUIRE (second.isObject());
    // The same actions at the same programme frames ...
    REQUIRE (again.soak->getActions().size() == original.soak->getActions().size());
    const auto start1 = static_cast<juce::int64> (first.getProperty ("programmeStartFrame", 0));
    const auto start2 = static_cast<juce::int64> (second.getProperty ("programmeStartFrame", 0));
    bool sameActions = true;
    for (size_t i = 0; i < script.size(); ++i)
    {
        const auto& a = original.soak->getActions()[i];
        const auto& b = again.soak->getActions()[i];
        sameActions = sameActions && a.kind == b.kind && a.strip == b.strip && a.value == b.value && a.preset == b.preset
                      && a.appliedFrame - start1 == b.appliedFrame - start2;
    }
    CHECK (sameActions);
    // ... and every detection again (the virtual path is deterministic).
    const auto cmp = second.getProperty ("replayComparison", {});
    REQUIRE (cmp.isObject());
    const int originals = static_cast<int> (cmp.getProperty ("originalEvents", -2));
    CHECK (originals == firstEvents->size());
    CHECK (originals >= 1);
    CHECK (static_cast<int> (cmp.getProperty ("notReproduced", -1)) == 0);
    CHECK (static_cast<int> (cmp.getProperty ("replayOnly", -1)) == 0);
    CHECK (static_cast<int> (cmp.getProperty ("reproduced", -1)) == originals);
    // The comparison matches type and frame only; on the virtual path the
    // levels and the frames are the same too.
    const auto* secondEvents = second.getProperty ("discontinuities", {}).getProperty ("events", {}).getArray();
    REQUIRE (secondEvents != nullptr);
    REQUIRE (secondEvents->size() == firstEvents->size());
    bool sameEvents = true;
    for (int i = 0; i < firstEvents->size(); ++i)
    {
        const auto& x = firstEvents->getReference (i);
        const auto& y = secondEvents->getReference (i);
        sameEvents = sameEvents && text (x, "type") == text (y, "type")
                     && static_cast<juce::int64> (x.getProperty ("programmeFrame", -1)) == static_cast<juce::int64> (y.getProperty ("programmeFrame", -2))
                     && std::abs (static_cast<double> (x.getProperty ("levelDb", 0.0)) - static_cast<double> (y.getProperty ("levelDb", 1.0))) < 0.005;
    }
    CHECK (sameEvents);
}

TEST_CASE ("App: E53 device soak: --dump right at the start of the stream writes a shorter file instead of crashing")
{
    flubapptest::TempFolder folder;
    auto options = shortOptions (0.02); // 1.2 s
    options.automation = false;
    options.report = folder.file ("dumped.json");
    options.dumpAt = { 0.1, 0.6, 30.0 }; // 0.1 s: the window starts before the stream; 30 s: past the run
    VirtualSoak run;
    const auto report = run.run (options);
    REQUIRE (report.isObject());
    CHECK (text (report, "result") == "completed");
    const auto* dumps = report.getProperty ("dumps", {}).getArray();
    REQUIRE (dumps != nullptr);
    REQUIRE (dumps->size() == 3);

    const auto start = static_cast<juce::int64> (report.getProperty ("programmeStartFrame", -1));
    REQUIRE (start >= 0);
    const auto readDump = [&folder] (const juce::var& d, flub::io::AudioFileData& wav)
    {
        std::string error;
        return flub::io::readWav (folder.file (text (d, "wav")).getFullPathName().toStdString(), wav, error);
    };
    // 0.1 s: from the stream's first frame to 0.6 s.
    const auto& early = dumps->getReference (0);
    CHECK (static_cast<bool> (early.getProperty ("written", false)));
    flub::io::AudioFileData wav;
    REQUIRE (readDump (early, wav));
    CHECK (wav.numChannels == 2);
    const auto earlyFrames = static_cast<juce::int64> (wav.channels[0].size());
    CHECK (earlyFrames == 28800 + start); // 0.6 s of programme plus what the tap held before it started
    CHECK (std::abs (static_cast<double> (early.getProperty ("toSeconds", 0.0)) - 0.6) < 1.0e-3);
    CHECK (static_cast<double> (early.getProperty ("fromSeconds", 1.0)) <= 0.0);
    CHECK (folder.file ("dumped-dump-0.100-strips.json").existsAsFile());
    // 0.6 s: the whole second around it.
    const auto& middle = dumps->getReference (1);
    flub::io::AudioFileData full;
    REQUIRE (readDump (middle, full));
    CHECK (full.channels[0].size() == 48000u);
    float peak = 0.0f;
    for (const float v : full.channels[0])
        peak = std::max (peak, std::abs (v));
    CHECK (peak > 0.01f); // the programme
    // 30 s: the run never got there.
    CHECK (! static_cast<bool> (dumps->getReference (2).getProperty ("written", true)));
    CHECK (run.soak->getSummary().contains ("not written"));
}

TEST_CASE ("App: E53 device soak: the system default output is refused without --allow-audible")
{
    juce::AudioDeviceManager manager;
    manager.addAudioDeviceType (std::make_unique<SoakVirtualDeviceType> (juce::StringArray { "Headset (the default)", "Spare Output" }));

    DeviceSoakOptions options;
    options.type = SoakVirtualDeviceType::kTypeName;
    options.device = "Headset (the default)"; // the type's default output (index 0)
    juce::String error;
    CHECK (! DeviceSoak::resolveDevice (options, error, &manager));
    CHECK (error.contains ("system default output"));
    CHECK (error.contains ("--allow-audible"));

    options.allowAudible = true;
    error = {};
    CHECK (DeviceSoak::resolveDevice (options, error, &manager));
    CHECK (error.isEmpty());

    DeviceSoakOptions spare;
    spare.type = SoakVirtualDeviceType::kTypeName;
    spare.device = "Spare Output";
    CHECK (DeviceSoak::resolveDevice (spare, error, &manager));

    DeviceSoakOptions missing;
    missing.type = SoakVirtualDeviceType::kTypeName;
    missing.device = "Speakers";
    CHECK (! DeviceSoak::resolveDevice (missing, error, &manager));
    CHECK (error.contains ("not an output"));
    CHECK (manager.getCurrentAudioDevice() == nullptr); // nothing was opened
}

TEST_CASE ("App: E53 device soak: a stopped virtual device ends the soak (aborted) instead of spinning")
{
    auto options = shortOptions (0.05); // 3 s that it never gets to play
    options.automation = false;
    VirtualSoak run;
    run.afterStart = [] (EngineController& controller)
    {
        if (auto* device = controller.getDeviceManager().getCurrentAudioDevice())
            device->stop(); // the virtual device's process() now runs no callback
    };
    const auto begin = juce::Time::getMillisecondCounterHiRes();
    const auto report = run.run (options);
    REQUIRE (report.isObject());
    CHECK (run.soak->isFinished());
    CHECK (run.exitCode == 4);
    CHECK (text (report, "result") == "aborted");
    CHECK (text (report, "reason").contains ("not running"));
    CHECK (juce::Time::getMillisecondCounterHiRes() - begin < 1500.0);
}

TEST_CASE ("App: E53 device soak hooks: a stalled callback - clearing reports it, closing the device waits for it")
{
    AudioEngineHost host;
    host.getDeviceManager().addAudioDeviceType (std::make_unique<SoakVirtualDeviceType>());
    host.setDeviceWatcher (nullptr);
    juce::XmlElement saved ("DEVICESETUP");
    saved.setAttribute ("deviceType", SoakVirtualDeviceType::kTypeName);
    saved.setAttribute ("audioOutputDeviceName", SoakVirtualDeviceType::kOutputName);
    saved.setAttribute ("audioDeviceBufferSize", 256);
    REQUIRE (host.openDevice (&saved, 0, 2).isEmpty());
    auto* device = dynamic_cast<SoakVirtualDevice*> (host.getDeviceManager().getCurrentAudioDevice());
    REQUIRE (device != nullptr);

    // A source whose render stalls (as a driver hang or a debugger would)
    // until it is released.
    struct StallingSource final : StripSignalSource
    {
        std::atomic<bool> stall { false }, entered { false }, release { false }, left { false };
        bool renderStrip (int strip, const flub::AudioBlock&) override
        {
            if (strip == 0 && stall.load())
            {
                entered.store (true);
                const auto begin = juce::Time::getMillisecondCounter();
                while (! release.load() && juce::Time::getMillisecondCounter() - begin < 3000)
                    juce::Thread::sleep (1);
                left.store (true);
            }
            return false;
        }
    } source;
    flub::StreamTap tap;
    tap.prepare (48000);
    host.setOutputTap (&tap);
    host.setDeviceSignalSource (&source);
    for (int b = 0; b < 4; ++b)
        device->process();

    source.stall.store (true);
    std::thread audio ([device] { device->process(); }); // the "device thread", stalled in the callback
    for (int i = 0; i < 2000 && ! source.entered.load(); ++i)
        juce::Thread::sleep (1);
    REQUIRE (source.entered.load());

    // The bounded wait gives up and says so: the old source may still be in use.
    CHECK (! host.setDeviceSignalSource (nullptr));
    CHECK (! host.setOutputTap (nullptr)); // the same stalled callback: no second wait
    CHECK (! source.left.load());

    // Closing the device waits for the callback in flight (the soak's end
    // does this before it frees the programme and the tap).
    std::thread releaser ([&source] { juce::Thread::sleep (150); source.release.store (true); });
    host.closeDevice();
    CHECK (source.left.load());
    audio.join();
    releaser.join();
    CHECK (host.setDeviceSignalSource (nullptr)); // nothing runs now
    CHECK (host.setOutputTap (nullptr));
}
