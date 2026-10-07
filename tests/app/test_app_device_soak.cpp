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
//   * an injected pulse is found and classed "static" (no action near it);
//   * --replay of a virtual run reproduces its detections and actions.
#include "AppTestSupport.h"

#include "engine/AudioEngineHost.h"
#include "engine/TestSignalGenerator.h"
#include "shell/DeviceSoak.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <cmath>
#include <memory>
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

    for (const juce::StringArray bad : { juce::StringArray { "--device-soak" },                                   // no device
                                         juce::StringArray { "--device-soak", "--device", "X", "--buffer", "8" },  // too small
                                         juce::StringArray { "--device-soak", "--device", "X", "--buffer", "abc" },
                                         juce::StringArray { "--device-soak", "--device", "X", "--profile", "fast" },
                                         juce::StringArray { "--device-soak", "--device", "X", "--minutes", "0" },
                                         juce::StringArray { "--device-soak", "--device", "X", "--automation", "all" },
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

TEST_CASE ("App: E53 device soak: --replay of a virtual run reproduces its actions and detections")
{
    flubapptest::TempFolder folder;
    auto options = shortOptions (0.02); // 1.2 s: about 10 actions
    options.intervalMs = 80.0;
    options.report = folder.file ("original.json");
    VirtualSoak original;
    const auto first = original.run (options);
    REQUIRE (first.isObject());
    REQUIRE (options.report.existsAsFile());
    CHECK (options.report.withFileExtension ("txt").existsAsFile());

    DeviceSoakOptions replayed;
    std::vector<SoakAction> script;
    juce::String error;
    REQUIRE (DeviceSoak::readReplay (options.report, replayed, script, error));
    CHECK (replayed.bufferSize == 480);
    CHECK (replayed.sampleRate == 48000.0);
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
    CHECK (static_cast<int> (cmp.getProperty ("notReproduced", -1)) == 0);
    CHECK (static_cast<int> (cmp.getProperty ("replayOnly", -1)) == 0);
    CHECK (static_cast<int> (cmp.getProperty ("reproduced", -1)) == static_cast<int> (cmp.getProperty ("originalEvents", -2)));
}
