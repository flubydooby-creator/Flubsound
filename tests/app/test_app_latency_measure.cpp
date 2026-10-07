// App-level tests: R1.1, docs/11 E42c (the device buffer per latency
// profile, with a glitch back-off) and E42d in the app (the live latency
// measurement).
//   * buffer::choose / nextLarger / Backoff: the pure decision logic, on the
//     buffer lists real devices offer (an IAudioClient3 HD Audio endpoint:
//     128 .. 480 in steps of 32; the Turtle Beach Stealth 600PC Gen 3's
//     dongle: 480 only; WASAPI exclusive, ASIO and CoreAudio lists).
//   * AudioEngineHost on a fake JUCE device type whose devices honour the
//     buffer size: the profile's size is asked for when the device opens and
//     when the profile changes, Automatic off keeps the size, the back-off
//     raises it one size and remembers the floor, a device with one size
//     keeps it.
//   * The live measurement through a fake duplex device whose input is its
//     own output delayed by a known round trip (plus noise): device only and
//     through Flubsound (the strip's engine latency added) within 0.1 ms; a
//     weak or silent input and an echo nearly as strong as the direct path
//     are reported as such, never as a confident number; the probe allocates
//     nothing on the audio thread, stays silent while the feedback-loop guard
//     holds the output, and a cancel fades it out within 5 ms.
//   * EngineController: Automatic and the floors persisted, the profile
//     chosen by hand (never the overload response's) drives the buffer, the
//     back-off from the watchdog's glitches; the Settings > Audio panel.
#include "AppTestSupport.h"

#include "engine/AudioEngineHost.h"
#include "engine/BufferPolicy.h"
#include "engine/EngineController.h"
#include "engine/LatencyMeasurement.h"
#include "engine/LatencyMeasurer.h"
#include "ui/LatencyPanel.h"
#include "ui/SettingsDialog.h"

#include "flub/engine/Parameters.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

using namespace flub::app;
using Profile = flub::param::LatencyProfileValue;
using flub::app::buffer::Reason;

namespace
{
/** An IAudioClient3 HD Audio endpoint at 48 kHz (measured on the owner's
    PC: "Digital Audio (S/PDIF) (High Definition Audio Device)" reports
    default 480, fundamental 32, min 128, max 480 frames). */
std::vector<int> hdAudioSizes()
{
    std::vector<int> sizes;
    for (int s = 128; s <= 480; s += 32)
        sizes.push_back (s);
    return sizes;
}

/** JUCE's list for WASAPI exclusive mode with a 3 ms minimum period (144
    frames at 48 kHz) and a 10 ms default. */
std::vector<int> exclusiveSizes()
{
    std::vector<int> sizes { 480, 144 };
    int n = 64;
    for (int i = 0; i < 40; ++i)
    {
        if (n >= 144 && n <= 2048)
            sizes.push_back (n);
        n += n < 512 ? 32 : (n < 1024 ? 64 : 128);
    }
    return sizes;
}

// ---- A fake device type whose devices honour the buffer size ----------------------
const juce::String kHdAudio ("Digital Audio (S/PDIF) (High Definition Audio Device)");
const juce::String kStealth ("Speakers (Stealth 600PC Gen 3)");
const juce::String kStealthMic ("Microphone (Stealth 600PC Gen 3)");

class BufferDevice final : public juce::AudioIODevice
{
public:
    BufferDevice (const juce::String& outputName, const juce::String& inputName, std::vector<int> bufferSizes, int& openCount)
        : juce::AudioIODevice (outputName, "BufferBackend"), sizes (std::move (bufferSizes)), opens (openCount),
          hasInput (inputName.isNotEmpty())
    {
    }

    ~BufferDevice() override { stop(); }

    juce::StringArray getOutputChannelNames() override { return { "Left", "Right" }; }
    juce::StringArray getInputChannelNames() override { return hasInput ? juce::StringArray { "Left", "Right" } : juce::StringArray(); }
    juce::Array<double> getAvailableSampleRates() override { return { 48000.0 }; }
    juce::Array<int> getAvailableBufferSizes() override
    {
        juce::Array<int> a;
        for (const int s : sizes)
            a.add (s);
        return a;
    }
    int getDefaultBufferSize() override { return 480; }
    juce::String open (const juce::BigInteger& ins, const juce::BigInteger& outs, double, int bufferSize) override
    {
        ++opens;
        current = bufferSize > 0 ? bufferSize : 480;
        activeIns = hasInput ? ins : juce::BigInteger();
        activeOuts = outs;
        opened = true;
        return {};
    }
    void close() override
    {
        stop();
        opened = false;
    }
    bool isOpen() override { return opened; }
    void start (juce::AudioIODeviceCallback* cb) override
    {
        if (cb != nullptr && callback == nullptr)
        {
            cb->audioDeviceAboutToStart (this);
            callback = cb;
        }
    }
    void stop() override
    {
        if (auto* cb = std::exchange (callback, nullptr))
            cb->audioDeviceStopped();
    }
    bool isPlaying() override { return callback != nullptr; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return current; }
    double getCurrentSampleRate() override { return 48000.0; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return activeOuts; }
    juce::BigInteger getActiveInputChannels() const override { return activeIns; }
    // As JUCE's WASAPI shared types report it: the stream latency (0 before a
    // stream starts) + the buffer.
    int getOutputLatencyInSamples() override { return current; }
    int getInputLatencyInSamples() override { return current; }

private:
    std::vector<int> sizes;
    int& opens;
    bool hasInput;
    int current = 480;
    juce::AudioIODeviceCallback* callback = nullptr;
    juce::BigInteger activeIns, activeOuts;
    bool opened = false;
};

class BufferBackend final : public juce::AudioIODeviceType
{
public:
    BufferBackend() : juce::AudioIODeviceType ("BufferBackend") {}

    int opens = 0;

    void scanForDevices() override {}
    juce::StringArray getDeviceNames (bool wantInputNames) const override
    {
        return wantInputNames ? juce::StringArray { kStealthMic } : juce::StringArray { kHdAudio, kStealth };
    }
    int getDefaultDeviceIndex (bool) const override { return 0; }
    int getIndexOfDevice (juce::AudioIODevice* device, bool asInput) const override
    {
        return device != nullptr ? getDeviceNames (asInput).indexOf (device->getName()) : -1;
    }
    bool hasSeparateInputsAndOutputs() const override { return true; }
    juce::AudioIODevice* createDevice (const juce::String& outputName, const juce::String& inputName) override
    {
        if (outputName == kHdAudio)
            return new BufferDevice (outputName, inputName, hdAudioSizes(), opens);
        if (outputName == kStealth)
            return new BufferDevice (outputName, inputName, { 480 }, opens);
        return nullptr;
    }
};

/** A host on the fake backend, the device opened from a saved state. */
struct BufferRig
{
    AudioEngineHost host;
    BufferBackend* backend = nullptr;

    BufferRig()
    {
        auto b = std::make_unique<BufferBackend>();
        backend = b.get();
        host.getDeviceManager().addAudioDeviceType (std::move (b));
        host.setDeviceWatcher (nullptr);
    }

    ~BufferRig() { host.closeDevice(); }

    juce::String open (const juce::String& output)
    {
        juce::XmlElement saved ("DEVICESETUP");
        saved.setAttribute ("deviceType", "BufferBackend");
        saved.setAttribute ("audioOutputDeviceName", output);
        saved.setAttribute ("audioInputDeviceName", kStealthMic);
        saved.setAttribute ("audioDeviceBufferSize", 480); // what a state saved before docs/11 E42c holds
        return host.openDevice (&saved, 2, 2);
    }

    int buffer() { return host.getDeviceManager().getCurrentAudioDevice()->getCurrentBufferSizeSamples(); }
};

// ---- A fake duplex device whose input is its own output, delayed ------------------
constexpr int kBlock = 256;

class LoopDevice final : public juce::AudioIODevice
{
public:
    LoopDevice() : juce::AudioIODevice ("Loop Out", "Fake") {}

    juce::StringArray getOutputChannelNames() override { return { "Left", "Right" }; }
    juce::StringArray getInputChannelNames() override { return { "In 1", "In 2" }; }
    juce::Array<double> getAvailableSampleRates() override { return { 48000.0 }; }
    juce::Array<int> getAvailableBufferSizes() override { return { kBlock }; }
    int getDefaultBufferSize() override { return kBlock; }
    juce::String open (const juce::BigInteger&, const juce::BigInteger&, double, int) override { return {}; }
    void close() override {}
    bool isOpen() override { return true; }
    void start (juce::AudioIODeviceCallback*) override {}
    void stop() override {}
    bool isPlaying() override { return true; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return kBlock; }
    double getCurrentSampleRate() override { return 48000.0; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return juce::BigInteger (0x3); }
    juce::BigInteger getActiveInputChannels() const override { return juce::BigInteger (inputs ? 0x3 : 0x0); }
    int getOutputLatencyInSamples() override { return 2 * kBlock; }
    int getInputLatencyInSamples() override { return kBlock; }

    bool inputs = true;
};

/** The device's audio thread: input = loopGain x the output `delay` samples
    ago (+ an echo, + white noise), as fast as it can. */
struct LoopRig
{
    AudioEngineHost& host;
    int delay = 1512;           // the round trip of the device + the air or cable (31.5 ms)
    float loopGain = 1.0f;
    float echoGain = 0.0f;
    int echoDelay = 960;        // after the direct path
    float noiseRms = 0.0f;
    std::vector<float> history; // output channel 0
    std::atomic<bool> stop { false };
    std::thread thread;

    explicit LoopRig (AudioEngineHost& h) : host (h) { history.reserve (48000 * 60); }
    ~LoopRig() { halt(); }

    void start()
    {
        thread = std::thread ([this]
        {
            std::array<std::vector<float>, 2> in { std::vector<float> (kBlock), std::vector<float> (kBlock) };
            std::array<std::vector<float>, 2> out { std::vector<float> (kBlock), std::vector<float> (kBlock) };
            juce::Random rng (12345);
            const float noiseScale = noiseRms * std::sqrt (3.0f);
            int64_t t = 0;
            while (! stop.load (std::memory_order_relaxed))
            {
                for (int i = 0; i < kBlock; ++i)
                {
                    const auto at = [this] (int64_t index) { return index >= 0 && index < static_cast<int64_t> (history.size()) ? history[static_cast<size_t> (index)] : 0.0f; };
                    const int64_t now = t + i;
                    const float v = loopGain * at (now - delay) + echoGain * at (now - delay - echoDelay)
                                    + noiseScale * (2.0f * rng.nextFloat() - 1.0f);
                    in[0][static_cast<size_t> (i)] = v;
                    in[1][static_cast<size_t> (i)] = v;
                }
                const std::array<const float*, 2> ins { in[0].data(), in[1].data() };
                const std::array<float*, 2> outs { out[0].data(), out[1].data() };
                host.audioDeviceIOCallbackWithContext (ins.data(), 2, outs.data(), 2, kBlock, juce::AudioIODeviceCallbackContext {});
                history.insert (history.end(), out[0].begin(), out[0].end());
                t += kBlock;
            }
        });
    }

    void halt()
    {
        stop.store (true);
        if (thread.joinable())
            thread.join();
    }
};

/** Short sweeps keep each case fast (the app's own: 5 x 0.5 s, 500 ms). */
flub::latency::ProbeSettings quickProbe()
{
    auto s = latency::liveProbeSettings (48000.0);
    s.sweepSeconds = 0.25;
    s.maxDelayMs = 100.0;
    s.leadInSeconds = 0.1;
    s.runs = 5;
    return s;
}

bool runUntilFinished (LatencyMeasurer& m, int timeoutMs = 10000)
{
    return flubapptest::pumpMessagesUntil (
        [&m]
        {
            m.poll();
            const auto phase = m.getState().phase;
            return phase == LatencyMeasurer::Phase::Done || phase == LatencyMeasurer::Phase::Failed;
        },
        timeoutMs);
}

EngineController::Options headlessOptions (const flubapptest::TempFolder& temp)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = false;
    return o;
}
} // namespace

// =============================================================================
// E42c: the buffer per latency profile
// =============================================================================
TEST_CASE ("App: E42c buffer choice per latency profile on real devices' lists (Low Latency smallest >= 1.33 ms, Balanced ~5 ms, Quality the default)")
{
    using buffer::choose;
    // HD Audio through IAudioClient3 (128 .. 480 by 32): before E42c every
    // profile ran at JUCE's default, 480 (10 ms).
    CHECK (choose (Profile::LowLatency, hdAudioSizes(), 480, 48000.0).samples == 128);   // 2.67 ms
    CHECK (choose (Profile::LowLatency, hdAudioSizes(), 480, 48000.0).reason == Reason::Smallest);
    CHECK (choose (Profile::Balanced, hdAudioSizes(), 480, 48000.0).samples == 256);     // 5.33 ms (224 and 256 tie: the larger)
    CHECK (choose (Profile::Balanced, hdAudioSizes(), 480, 48000.0).reason == Reason::NearFiveMs);
    CHECK (choose (Profile::Quality, hdAudioSizes(), 480, 48000.0).samples == 480);
    CHECK (choose (Profile::Quality, hdAudioSizes(), 480, 48000.0).reason == Reason::DeviceDefault);

    // The Stealth 600PC Gen 3's dongle: 480 only (min = default = max).
    for (const auto p : { Profile::LowLatency, Profile::Balanced, Profile::Quality })
    {
        CHECK (choose (p, { 480 }, 480, 48000.0).samples == 480);
        CHECK (choose (p, { 480 }, 480, 48000.0).reason == Reason::OnlyOne);
    }

    // WASAPI exclusive (3 ms minimum), ASIO, CoreAudio (default 512).
    CHECK (choose (Profile::LowLatency, exclusiveSizes(), 480, 48000.0).samples == 144);
    CHECK (choose (Profile::Balanced, exclusiveSizes(), 480, 48000.0).samples == 256);
    CHECK (choose (Profile::Quality, exclusiveSizes(), 480, 48000.0).samples == 480);
    CHECK (choose (Profile::LowLatency, { 32, 64, 128, 256, 512 }, 256, 48000.0).samples == 64); // never below 1.33 ms
    CHECK (choose (Profile::Balanced, { 32, 64, 128, 256, 512 }, 256, 48000.0).samples == 256);
    std::vector<int> coreAudio;
    for (int s = 32; s <= 4096; s += 32)
        coreAudio.push_back (s);
    CHECK (choose (Profile::LowLatency, coreAudio, 512, 44100.0).samples == 64); // >= 59 at 44.1 kHz
    CHECK (choose (Profile::Balanced, coreAudio, 512, 44100.0).samples == 224);  // nearest to 220.5
    CHECK (choose (Profile::Quality, coreAudio, 512, 44100.0).samples == 512);
    CHECK (choose (Profile::LowLatency, coreAudio, 512, 96000.0).samples == 128);

    // Never above the device's default (a device whose default is small).
    CHECK (choose (Profile::Balanced, { 64, 128, 256, 512 }, 128, 48000.0).samples == 128);
    CHECK (choose (Profile::Balanced, { 64, 128, 256, 512 }, 128, 48000.0).reason == Reason::DeviceDefault);

    // The back-off floor: the profile's size no lower than it, capped at the default.
    CHECK (choose (Profile::LowLatency, hdAudioSizes(), 480, 48000.0, 160).samples == 160);
    CHECK (choose (Profile::LowLatency, hdAudioSizes(), 480, 48000.0, 160).reason == Reason::Floor);
    CHECK (choose (Profile::Balanced, hdAudioSizes(), 480, 48000.0, 160).samples == 256); // above the floor anyway
    CHECK (choose (Profile::LowLatency, hdAudioSizes(), 480, 48000.0, 4096).samples == 480);

    // The back-off's step: one size up, at most the default.
    CHECK (buffer::nextLarger (hdAudioSizes(), 128, 480) == 160);
    CHECK (buffer::nextLarger (hdAudioSizes(), 448, 480) == 480);
    CHECK (! buffer::nextLarger (hdAudioSizes(), 480, 480).has_value());
    CHECK (! buffer::nextLarger ({ 480 }, 480, 480).has_value());

    // Device types whose buffer the app leaves alone.
    CHECK (AudioEngineHost::managesBufferSize ("Windows Audio (Low Latency Mode)"));
    CHECK (AudioEngineHost::managesBufferSize ("Windows Audio (Exclusive Mode)"));
    CHECK (AudioEngineHost::managesBufferSize ("ASIO"));
    CHECK (AudioEngineHost::managesBufferSize ("CoreAudio"));
    CHECK (! AudioEngineHost::managesBufferSize ("Windows Audio")); // shared mode ignores the request
    CHECK (! AudioEngineHost::managesBufferSize ("ALSA"));
    CHECK (! AudioEngineHost::managesBufferSize ("JACK"));
    CHECK (! AudioEngineHost::managesBufferSize ("PipeWire"));
}

TEST_CASE ("App: E42c back-off: more than 2 glitches within 10 s (or a sustained overload) raise the buffer, then it holds 10 s")
{
    buffer::Backoff b;
    uint64_t glitches = 0;
    const auto poll = [&] (uint64_t fresh, bool overload = false) { glitches += fresh; return b.update (true, glitches, overload); };

    CHECK (! poll (0)); // baseline
    // Two glitches every 10 s go on for a minute: no step.
    int steps = 0;
    for (int i = 0; i < 120; ++i)
        steps += poll (i % 10 == 0 ? 1 : 0) ? 1 : 0;
    CHECK (steps == 0);
    // A third within the window: a step.
    for (int i = 0; i < 19; ++i)
        poll (0);
    CHECK (! poll (1));
    CHECK (! poll (1));
    CHECK (poll (1));
    // Then 20 polls (10 s) of hold: the device restart's own glitches do not count.
    for (int i = 0; i < 20; ++i)
        CHECK (! poll (5));
    CHECK (! poll (1));
    CHECK (! poll (1));
    CHECK (poll (1));
    // A sustained overload steps at once (after the hold).
    for (int i = 0; i < 20; ++i)
        poll (0);
    CHECK (poll (0, true));
    CHECK (b.getSteps() == 3);

    // A stopped device (and a restart by the profile) starts a hold.
    for (int i = 0; i < 20; ++i)
        poll (0);
    CHECK (! b.update (false, glitches, false));
    for (int i = 0; i < 20; ++i)
        CHECK (! poll (3));
    b.restarted();
    for (int i = 0; i < 20; ++i)
        CHECK (! poll (3, true));
    CHECK (poll (3));
}

TEST_CASE ("App: E42c the host asks for the profile's buffer; Automatic off keeps the size; the back-off raises it and remembers the floor")
{
    BufferRig rig;
    rig.host.setBufferProfile (Profile::LowLatency);
    REQUIRE (rig.open (kHdAudio).isEmpty());
    REQUIRE (rig.host.getDeviceManager().getCurrentAudioDevice() != nullptr);

    // Opened at the saved 480, then re-opened at the profile's size before the
    // engine attached; the engine runs at that block.
    CHECK (rig.buffer() == 128);
    CHECK (rig.host.getBlockSize() == 128);
    auto info = rig.host.getBufferInfo();
    CHECK (info.managed);
    CHECK (info.automatic);
    CHECK (info.target == 128);
    CHECK (info.reason == Reason::Smallest);
    CHECK (info.smallest == 128);
    CHECK (info.largest == 480);
    CHECK (rig.host.getReportedOutputLatency() == 128);
    const std::vector<int> before { 480, 480, 480 }; // every profile before E42c (JUCE's default)

    // The profile moves the buffer (one device re-open each).
    const int opens = rig.backend->opens;
    rig.host.setBufferProfile (Profile::Balanced);
    CHECK (rig.buffer() == 256);
    CHECK (rig.host.getBlockSize() == 256);
    rig.host.setBufferProfile (Profile::Quality);
    CHECK (rig.buffer() == 480);
    CHECK (rig.backend->opens == opens + 2);
    std::cout << "    buffer per profile (HD Audio, 48 kHz) before: Low Latency " << before[0] << ", Balanced " << before[1] << ", Quality "
              << before[2] << "; after: 128, 256, 480 samples\n";

    // Automatic off: the size stays, whatever the profile.
    rig.host.setAutomaticBufferSize (false);
    rig.host.setBufferProfile (Profile::LowLatency);
    CHECK (rig.buffer() == 480);
    CHECK (! rig.host.getBufferInfo().automatic);
    CHECK (! rig.host.raiseBufferOneStep());
    // ... and on again: the profile's size.
    rig.host.setAutomaticBufferSize (true);
    CHECK (rig.buffer() == 128);

    // The back-off: one size up, remembered as this device's floor.
    CHECK (rig.host.raiseBufferOneStep());
    CHECK (rig.buffer() == 160);
    CHECK (rig.host.getBufferInfo().reason == Reason::Floor);
    const auto key = AudioEngineHost::bufferDeviceKey ("BufferBackend", kHdAudio);
    REQUIRE (rig.host.getBufferFloors().count (key) == 1);
    CHECK (rig.host.getBufferFloors().at (key) == 160);
    rig.host.setBufferProfile (Profile::Balanced);
    CHECK (rig.buffer() == 256); // above the floor
    rig.host.setBufferProfile (Profile::LowLatency);
    CHECK (rig.buffer() == 160);
    // Off and on again forgets the floor.
    rig.host.setAutomaticBufferSize (false);
    rig.host.setAutomaticBufferSize (true);
    CHECK (rig.buffer() == 128);
    CHECK (rig.host.getBufferFloors().count (key) == 0);

    // A floor set before the device opens applies when it does.
    rig.host.closeDevice();
    rig.host.setBufferFloors ({ { key, 192 } });
    REQUIRE (rig.open (kHdAudio).isEmpty());
    CHECK (rig.buffer() == 192);
}

TEST_CASE ("App: E42c a device with one buffer size (the Stealth 600PC Gen 3 dongle) keeps it on every profile")
{
    BufferRig rig;
    rig.host.setBufferProfile (Profile::LowLatency);
    REQUIRE (rig.open (kStealth).isEmpty());
    CHECK (rig.buffer() == 480);
    const auto info = rig.host.getBufferInfo();
    CHECK (info.reason == Reason::OnlyOne);
    CHECK (info.target == 480);
    CHECK (! rig.host.raiseBufferOneStep());
    rig.host.setBufferProfile (Profile::Balanced);
    CHECK (rig.buffer() == 480);
    LatencyInfo latencyInfo;
    latencyInfo.valid = true;
    latencyInfo.deviceOutputMs = latencyInfo.deviceInputMs = 10.0;
    latencyInfo.engineMs = 5.4;
    const auto text = ui::LatencyPanel::describeBuffer (info, latencyInfo, 0);
    CHECK (text.contains ("Buffer 480 samples (10.0 ms)"));
    CHECK (text.contains ("offers only this size"));
    CHECK (text.contains ("device out 10.0 ms + engine 5.4 ms + device in 10.0 ms"));
}

// =============================================================================
// E42d: the live measurement
// =============================================================================
TEST_CASE ("App: E42d live measurement through a fake loopback finds the round trip within 0.1 ms, device only and through Flubsound")
{
    AudioEngineHost host;
    LoopDevice device;
    host.audioDeviceAboutToStart (&device);
    LoopRig rig (host);
    rig.noiseRms = 3.0e-4f; // -70 dBFS of white noise in the input
    rig.start();

    LatencyMeasurer measurer (host);
    LatencyMeasurer::Request request;
    request.mode = LatencyMeasurer::Mode::Both;
    request.strip = 1;
    request.stripName = "Music";
    request.settings = quickProbe();
    REQUIRE (measurer.whyNot (request).isEmpty());
    REQUIRE (measurer.start (request).isEmpty());
    CHECK (measurer.isBusy());
    CHECK (measurer.start (request).isNotEmpty()); // one at a time
    REQUIRE (runUntilFinished (measurer));
    rig.halt();

    const auto& st = measurer.getState();
    REQUIRE (st.phase == LatencyMeasurer::Phase::Done);
    REQUIRE (st.deviceOnly.has_value());
    REQUIRE (st.through.has_value());
    const auto& dev = *st.deviceOnly;
    const auto& thr = *st.through;
    const int engine = host.getMixEngine().getStripLatencySamples (1);
    REQUIRE (dev.ok);
    REQUIRE (thr.ok);
    // The loop's 1512 samples; through Flubsound + the Music strip's engine latency.
    CHECK_NEAR (dev.roundTripMs, 1512.0 / 48.0, 0.1);
    CHECK_NEAR (dev.roundTripSamples, 1512.0, 0.5);
    CHECK_NEAR (thr.roundTripMs, (1512.0 + engine) / 48.0, 0.1);
    CHECK_NEAR (thr.roundTripSamples - dev.roundTripSamples, static_cast<double> (engine), 0.5);
    CHECK (dev.confidence == latency::Confidence::High);
    CHECK (thr.confidence == latency::Confidence::High);
    CHECK (dev.result.acceptedRuns == 5);
    CHECK (dev.spreadMs < 0.01);

    // The split: what the device reports (out 512, in 256 samples) and the
    // engine; the rest is "not reported"; the playback estimate between its bounds.
    CHECK_NEAR (dev.reportedOutputMs, 512.0 / 48.0, 1.0e-9);
    CHECK_NEAR (dev.reportedInputMs, 256.0 / 48.0, 1.0e-9);
    CHECK_NEAR (dev.unexplainedMs, dev.roundTripMs - 768.0 / 48.0, 1.0e-9);
    CHECK_NEAR (dev.playbackMs, 512.0 / 48.0 + 0.5 * dev.unexplainedMs, 1.0e-9);
    CHECK_NEAR (dev.withEngineMs, dev.playbackMs + engine / 48.0, 1.0e-9);
    CHECK_NEAR (thr.engineMs, engine / 48.0, 1.0e-9);
    CHECK_NEAR (thr.playbackMs, (512.0 + engine) / 48.0 + 0.5 * thr.unexplainedMs, 1.0e-9);
    CHECK (thr.playbackMinMs <= thr.playbackMs);
    CHECK (thr.playbackMs <= thr.playbackMaxMs);
    CHECK (thr.capGainDb == 0.0); // the strip did not raise the -24 dBFS sweep past -18 dBFS

    const auto text = st.describe();
    CHECK (text.contains ("Device only"));
    CHECK (text.contains ("Through Flubsound (Music strip)"));
    CHECK (text.contains ("Round trip: 31.5 ms"));
    CHECK (text.contains ("confidence high"));
    CHECK (text.contains ("Playback latency (what you hear)"));
    CHECK (text.contains ("Flubsound's engine, measured"));
    std::cout << "    loop 1512 smp: device only " << juce::String (dev.roundTripSamples, 3) << " smp, through Flubsound "
              << juce::String (thr.roundTripSamples, 3) << " smp (engine " << engine << ", measured "
              << juce::String (thr.roundTripSamples - dev.roundTripSamples, 3) << "); SNR " << juce::String (dev.result.medianSnrDb, 1)
              << " dB, strongest other arrival " << juce::String (dev.result.secondaryDb, 1) << " dB\n";
}

TEST_CASE ("App: E42d a weak or silent input, or an echo nearly as strong as the direct path, is reported as such, not as a confident number")
{
    AudioEngineHost host;
    LoopDevice device;
    host.audioDeviceAboutToStart (&device);

    LatencyMeasurer::Request request;
    request.mode = LatencyMeasurer::Mode::DeviceOnly;
    request.strip = 1;
    request.stripName = "Music";
    request.settings = quickProbe();

    {
        // The sweep 80 dB down under -50 dBFS of noise: no result, and why.
        LoopRig rig (host);
        rig.loopGain = 1.0e-4f;
        rig.noiseRms = 3.2e-3f;
        rig.start();
        LatencyMeasurer measurer (host);
        REQUIRE (measurer.start (request).isEmpty());
        REQUIRE (runUntilFinished (measurer));
        rig.halt();
        const auto& st = measurer.getState();
        REQUIRE (st.phase == LatencyMeasurer::Phase::Done);
        REQUIRE (st.deviceOnly.has_value());
        CHECK (! st.deviceOnly->ok);
        CHECK (st.deviceOnly->confidence == latency::Confidence::None);
        CHECK (juce::String (st.deviceOnly->error).contains ("not found reliably"));
        CHECK (st.describe().contains ("No result"));
    }
    {
        // Silence (a muted microphone).
        LoopRig rig (host);
        rig.loopGain = 0.0f;
        rig.start();
        LatencyMeasurer measurer (host);
        REQUIRE (measurer.start (request).isEmpty());
        REQUIRE (runUntilFinished (measurer));
        rig.halt();
        REQUIRE (measurer.getState().deviceOnly.has_value());
        CHECK (! measurer.getState().deviceOnly->ok);
        CHECK (juce::String (measurer.getState().deviceOnly->error).contains ("recorded silence"));
    }
    {
        // A second path 2 dB down, 20 ms later (a headset's sidetone loop):
        // the direct path is still found, but the result is graded low.
        LoopRig rig (host);
        rig.echoGain = 0.8f;
        rig.echoDelay = 960;
        rig.start();
        LatencyMeasurer measurer (host);
        REQUIRE (measurer.start (request).isEmpty());
        REQUIRE (runUntilFinished (measurer));
        rig.halt();
        REQUIRE (measurer.getState().deviceOnly.has_value());
        const auto& r = *measurer.getState().deviceOnly;
        REQUIRE (r.ok);
        CHECK_NEAR (r.roundTripSamples, 1512.0, 0.5);
        CHECK_NEAR (r.result.secondaryDb, -1.94, 0.3);
        CHECK (r.confidence == latency::Confidence::Low);
        CHECK (measurer.getState().describe().contains ("sidetone"));
    }
}

TEST_CASE ("App: E42d the probe allocates nothing on the audio thread, stays silent under the loopback guard and a cancel fades it out in 5 ms")
{
    AudioEngineHost host;
    LoopDevice device;
    host.audioDeviceAboutToStart (&device);

    std::array<std::vector<float>, 2> in { std::vector<float> (kBlock, 0.0f), std::vector<float> (kBlock, 0.0f) };
    std::array<std::vector<float>, 2> out { std::vector<float> (kBlock), std::vector<float> (kBlock) };
    const std::array<const float*, 2> ins { in[0].data(), in[1].data() };
    const std::array<float*, 2> outs { out[0].data(), out[1].data() };
    std::vector<float> played;
    played.reserve (static_cast<size_t> (200 * kBlock)); // no allocation of the test's own inside the probed callbacks
    const auto callback = [&]
    {
        host.audioDeviceIOCallbackWithContext (ins.data(), 2, outs.data(), 2, kBlock, juce::AudioIODeviceCallbackContext {});
        played.insert (played.end(), out[0].begin(), out[0].end());
    };
    callback(); // the first callback on a thread promotes it (may allocate once)

    // Through Flubsound: no allocation, no lock while it plays.
    {
        auto session = std::make_unique<latency::ProbeSession> (quickProbe(), latency::Path::ThroughFlubsound, 1, 2);
        const auto* raw = session.get();
        REQUIRE (host.startLatencyProbe (session));
        CHECK (session == nullptr);
        flubapptest::RealtimeProbe probe;
        for (int b = 0; b < 40; ++b)
            callback();
        CHECK (probe.allocations() == 0);
        CHECK (probe.deallocations() == 0);
        if (flubapptest::lockCountingAvailable())
            CHECK (probe.locks() == 0);
        CHECK (raw->position() == 40 * kBlock);

        // Cancel: the sweep fades out within 5 ms (240 samples); through
        // Flubsound the session (and its output cap) lasts 100 ms more for
        // the chain's delayed tail, then it finishes.
        const size_t mark = played.size();
        host.cancelLatencyProbe();
        for (int b = 0; b < 40 && ! raw->finished(); ++b)
            callback();
        CHECK (raw->finished());
        CHECK (raw->wasCancelled());
        CHECK (raw->position() <= 40 * kBlock + 240 + 4800 + kBlock);
        CHECK (raw->position() >= 40 * kBlock + 240 + 4800);
        float step = 0.0f;
        for (size_t i = mark + 1; i < played.size(); ++i)
            step = std::max (step, std::abs (played[i] - played[i - 1]));
        CHECK (step < 0.02f); // no click: the -24 dBFS sweep's own slope at most
        // Handed back once a callback has passed (the device thread runs one).
        std::thread deviceThread ([&]
        {
            std::this_thread::sleep_for (std::chrono::milliseconds (20));
            callback();
        });
        auto back = host.takeLatencyProbe();
        deviceThread.join();
        REQUIRE (back != nullptr);
        CHECK (host.getLatencyProbe() == nullptr);
    }

    // The loopback guard holds the output: the measurement is refused, and a
    // probe handed over anyway stays silent (it still advances and records).
    host.setDeviceInputRouting (1, 0);
    host.checkLoopbackPair ("CABLE Output (VB-Audio Virtual Cable)", "CABLE Input (VB-Audio Virtual Cable)");
    REQUIRE (host.isOutputMutedByGuard());
    LatencyMeasurer measurer (host);
    LatencyMeasurer::Request request;
    request.mode = LatencyMeasurer::Mode::DeviceOnly;
    request.strip = 1;
    request.settings = quickProbe();
    CHECK (measurer.whyNot (request).contains ("feedback-loop guard"));
    CHECK (measurer.start (request).isNotEmpty());
    for (int b = 0; b < 4; ++b)
        callback(); // the guard ramps the output down (10 ms)
    auto session = std::make_unique<latency::ProbeSession> (quickProbe(), latency::Path::DeviceOnly, 1, 2);
    const auto* raw = session.get();
    REQUIRE (host.startLatencyProbe (session));
    for (int b = 0; b < 30; ++b)
    {
        callback();
        for (const float v : out[0])
            CHECK (v == 0.0f);
    }
    CHECK (raw->position() == 30 * kBlock);
    host.setDeviceInputRouting (1, -1);
    host.checkLoopbackPair ({}, {});

    // A device stop hands the unfinished session back.
    host.audioDeviceStopped();
    auto back = host.takeLatencyProbe();
    REQUIRE (back != nullptr);
    CHECK (! back->finished());
}

TEST_CASE ("App: E42d a device restart or a cancel during the measurement ends it with a message, never a hang")
{
    AudioEngineHost host;
    LoopDevice device;
    host.audioDeviceAboutToStart (&device);
    LatencyMeasurer::Request request;
    request.mode = LatencyMeasurer::Mode::Both;
    request.strip = 1;
    request.stripName = "Music";
    request.settings = quickProbe();

    {
        // The device restarts (a buffer or rate change) while the first pass plays.
        auto rig = std::make_unique<LoopRig> (host);
        rig->start();
        LatencyMeasurer measurer (host);
        REQUIRE (measurer.start (request).isEmpty());
        CHECK (flubapptest::pumpMessagesUntil ([&] { measurer.poll(); return measurer.getState().progress > 0.1; }, 5000));
        rig->halt();
        host.audioDeviceStopped();
        host.audioDeviceAboutToStart (&device);
        rig = std::make_unique<LoopRig> (host);
        rig->start();
        REQUIRE (runUntilFinished (measurer, 3000));
        rig->halt();
        CHECK (measurer.getState().phase == LatencyMeasurer::Phase::Failed);
        CHECK (measurer.getState().error.contains ("stopped during the measurement"));
        CHECK (host.getLatencyProbe() == nullptr);
    }
    {
        // Cancel: "Cancelled.", and the next measurement can start.
        LoopRig rig (host);
        rig.start();
        LatencyMeasurer measurer (host);
        REQUIRE (measurer.start (request).isEmpty());
        CHECK (flubapptest::pumpMessagesUntil ([&] { measurer.poll(); return measurer.getState().progress > 0.05; }, 5000));
        measurer.cancel();
        REQUIRE (runUntilFinished (measurer, 3000));
        CHECK (measurer.getState().phase == LatencyMeasurer::Phase::Failed);
        CHECK (measurer.getState().error == "Cancelled.");
        CHECK (measurer.whyNot (request).isEmpty());
        rig.halt();
    }
}

TEST_CASE ("App: E42d no measurement without an input or a running device; a muted strip only for device only")
{
    AudioEngineHost host;
    LatencyMeasurer measurer (host);
    LatencyMeasurer::Request request;
    request.strip = 1;
    request.stripName = "Music";
    CHECK (measurer.whyNot (request).contains ("No audio device is running"));

    LoopDevice device;
    device.inputs = false;
    host.audioDeviceAboutToStart (&device);
    CHECK (measurer.whyNot (request).contains ("No input is open"));
    host.audioDeviceStopped();
    device.inputs = true;
    host.audioDeviceAboutToStart (&device);
    CHECK (measurer.whyNot (request).isEmpty());
    host.setStripMuted (1, true);
    CHECK (measurer.whyNot (request).contains ("Music strip is muted"));
    request.mode = LatencyMeasurer::Mode::DeviceOnly;
    CHECK (measurer.whyNot (request).isEmpty());
    request.strip = 9;
    CHECK (measurer.whyNot (request).contains ("no such strip"));

    // The app's own probe: 5 sweeps of 0.5 s at -24 dBFS, 6.55 s per pass.
    const auto live = latency::liveProbeSettings (48000.0);
    CHECK (live.levelDbfs == -24.0);
    CHECK (live.runs == 5);
    CHECK (flub::latency::validate (live).empty());
    CHECK_NEAR (LatencyMeasurer::durationSeconds (LatencyMeasurer::Mode::DeviceOnly, 48000.0), 6.55, 1.0e-9);
    CHECK_NEAR (LatencyMeasurer::durationSeconds (LatencyMeasurer::Mode::Both, 44100.0), 13.1, 1.0e-3);
}

// =============================================================================
// EngineController and the Settings > Audio panel
// =============================================================================
TEST_CASE ("App: E42c the controller persists Automatic and the floors; the profile chosen by hand, never the overload response's, drives the buffer")
{
    flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    auto& host = controller.getHost();
    CHECK (controller.getAutomaticBufferSize());
    CHECK (host.getBufferProfile() == controller.getLatencyProfile());

    controller.setAutomaticBufferSize (false);
    CHECK (! host.getAutomaticBufferSize());
    CHECK (! controller.getSettings().getAutoBufferSize());
    controller.setAutomaticBufferSize (true);
    CHECK (controller.getSettings().getAutoBufferSize());

    // Floors round-trip through the settings.
    controller.getSettings().setBufferFloors ({ { "BufferBackend|" + kHdAudio, 160 }, { "ASIO|Focusrite USB ASIO", 128 } });
    const auto floors = controller.getSettings().getBufferFloors();
    REQUIRE (floors.size() == 2);
    CHECK (floors.at ("BufferBackend|" + kHdAudio) == 160);
    CHECK (floors.at ("ASIO|Focusrite USB ASIO") == 128);

    // By hand: the buffer follows.
    controller.setLatencyProfile (Profile::Quality);
    CHECK (host.getBufferProfile() == Profile::Quality);

    // The overload response steps the profile down: the buffer does not shrink with it.
    controller.setReduceLoadOnOverload (true);
    EngineStatus hot;
    hot.deviceOpen = hot.running = true;
    hot.cpuLoad = 0.97;
    hot.xruns = -1;
    for (int i = 0; i < 100 && controller.getLatencyProfile() == Profile::Quality; ++i) // after the manual choice's 30 s rate limit
        controller.updateOverloadWatchdog (hot);
    CHECK (controller.getLatencyProfile() == Profile::Balanced);
    CHECK (host.getBufferProfile() == Profile::Quality);
    controller.restoreLatencyProfile();
    CHECK (host.getBufferProfile() == Profile::Quality);

    // Headless: nothing to measure, and the panel says why.
    CHECK (controller.whyCannotMeasureLatency (EngineController::LatencyMode::Both).contains ("No audio device is running"));
    ui::LatencyPanel panel (controller);
    panel.setSize (640, 400);
    panel.refresh();
    CHECK (! panel.getMeasureButton().isEnabled());
    CHECK (! panel.getCancelButton().isEnabled());
    CHECK (panel.getMeasurementText().contains ("No audio device is running"));
    CHECK (panel.isMeasurementTextAWarning());
    CHECK (panel.getBufferText() == "No audio device is open.");
    CHECK (panel.getAutomaticToggle().getToggleState());
    CHECK (panel.getModeBox().getText().contains ("Both"));
    CHECK (panel.getHeightForWidth (640) > 100);
}

TEST_CASE ("App: E42c the controller's back-off raises an automatic buffer after glitches and persists the floor")
{
    flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    auto& host = controller.getHost();
    auto backend = std::make_unique<BufferBackend>();
    host.getDeviceManager().addAudioDeviceType (std::move (backend));
    host.setDeviceWatcher (nullptr);
    controller.setLatencyProfile (Profile::LowLatency);
    juce::XmlElement saved ("DEVICESETUP");
    saved.setAttribute ("deviceType", "BufferBackend");
    saved.setAttribute ("audioOutputDeviceName", kHdAudio);
    saved.setAttribute ("audioInputDeviceName", kStealthMic);
    REQUIRE (host.openDevice (&saved, 2, 2).isEmpty());
    CHECK (host.getBufferInfo().current == 128);

    // The watchdog's 2 Hz polls: after the profile change's 10 s hold,
    // glitches 1, 2, 3 within 10 s -> one size up.
    EngineStatus status;
    status.deviceOpen = status.running = true;
    status.cpuLoad = 0.3;
    status.xruns = -1;
    for (int i = 0; i < 20; ++i)
        controller.updateOverloadWatchdog (status);
    CHECK (controller.getBufferBackoffSteps() == 0);
    for (int g : { 0, 0, 1, 2, 3 })
    {
        status.glitches = g;
        controller.updateOverloadWatchdog (status);
    }
    CHECK (controller.getBufferBackoffSteps() == 1);
    CHECK (host.getBufferInfo().current == 160);
    const auto floors = controller.getSettings().getBufferFloors();
    REQUIRE (floors.count (AudioEngineHost::bufferDeviceKey ("BufferBackend", kHdAudio)) == 1);
    CHECK (floors.at (AudioEngineHost::bufferDeviceKey ("BufferBackend", kHdAudio)) == 160);
    host.closeDevice();
}

TEST_CASE ("App: E42c Settings > Audio: a size picked in the device list turns Automatic off and wins; the switch turns it back on")
{
    flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    auto& host = controller.getHost();
    host.getDeviceManager().addAudioDeviceType (std::make_unique<BufferBackend>());
    host.setDeviceWatcher (nullptr);
    controller.setLatencyProfile (Profile::LowLatency);
    juce::XmlElement saved ("DEVICESETUP");
    saved.setAttribute ("deviceType", "BufferBackend");
    saved.setAttribute ("audioOutputDeviceName", kHdAudio);
    saved.setAttribute ("audioInputDeviceName", kStealthMic);
    REQUIRE (host.openDevice (&saved, 2, 2).isEmpty());
    CHECK (host.getBufferInfo().current == 128);

    ui::HotkeyHooks hooks;
    hooks.isSupported = [] { return false; };
    hooks.getFailures = [] { return juce::StringArray(); };
    hooks.reRegister = [] {};
    ui::SettingsDialog dialog (controller, hooks, [] (ui::MeterPalette) {}, ui::MeterPalette::Standard);
    dialog.setSize (ui::SettingsDialog::kMinWidth, ui::SettingsDialog::kMinHeight);
    dialog.showPage (ui::SettingsDialog::Page::Audio);

    // JUCE's buffer list (found by its label, as the page finds it) and the panel's switch.
    juce::ComboBox* bufferList = nullptr;
    juce::ToggleButton* automatic = nullptr;
    std::function<void (juce::Component&)> search = [&] (juce::Component& parent)
    {
        for (auto* child : parent.getChildren())
        {
            if (auto* label = dynamic_cast<juce::Label*> (child); label != nullptr && label->getText().startsWith ("Audio buffer size"))
                bufferList = dynamic_cast<juce::ComboBox*> (label->getAttachedComponent());
            if (auto* toggle = dynamic_cast<juce::ToggleButton*> (child); toggle != nullptr && toggle->getTitle() == "Automatic buffer size")
                automatic = toggle;
            search (*child);
        }
    };
    search (dialog);
    REQUIRE (bufferList != nullptr);
    REQUIRE (automatic != nullptr);
    CHECK (automatic->getToggleState());

    // The user picks 320 samples: Automatic turns off and the size stays, whatever the profile.
    bufferList->setSelectedId (320, juce::sendNotificationSync);
    CHECK (! controller.getAutomaticBufferSize());
    CHECK (! controller.getSettings().getAutoBufferSize());
    CHECK (host.getBufferInfo().current == 320);
    controller.setLatencyProfile (Profile::Balanced);
    CHECK (host.getBufferInfo().current == 320);

    // The page shows it at its next refresh (the dialog's 2 Hz timer); the
    // switch back on: the profile's size again.
    CHECK (flubapptest::pumpMessagesUntil ([automatic] { return ! automatic->getToggleState(); }, 2000));
    automatic->setToggleState (true, juce::sendNotificationSync);
    CHECK (controller.getAutomaticBufferSize());
    CHECK (host.getBufferInfo().current == 256);
    host.closeDevice();
}

TEST_CASE ("App: E42d Settings > Audio latency texts: the confirmation, the progress and the buffer lines")
{
    const auto confirm = ui::LatencyPanel::confirmationText (kStealth, kStealthMic, EngineController::LatencyMode::Both, 13.1);
    CHECK (confirm.contains ("about 13 s"));
    CHECK (confirm.contains ("-24 dBFS"));
    CHECK (confirm.contains ("never above -18 dBFS"));
    CHECK (confirm.contains (kStealth));
    CHECK (confirm.contains (kStealthMic));
    CHECK (confirm.contains ("Turn the volume down"));
    CHECK (confirm.contains ("microphone monitoring"));
    CHECK (! ui::LatencyPanel::confirmationText ("A", "B", EngineController::LatencyMode::DeviceOnly, 6.55).contains ("-18 dBFS"));

    LatencyMeasurer::State st;
    st.phase = LatencyMeasurer::Phase::Running;
    st.mode = LatencyMeasurer::Mode::Both;
    st.pass = 1;
    st.passes = 2;
    st.secondsLeft = 9.2;
    CHECK (ui::LatencyPanel::describeMeasurement (st, {}) == "Measuring (pass 1 of 2: device only)... 10 s left. Keep the microphone at the earcup and the room quiet.");
    st.phase = LatencyMeasurer::Phase::Failed;
    st.error = "Cancelled.";
    CHECK (ui::LatencyPanel::describeMeasurement (st, {}) == "Not measured: Cancelled.");
    st.phase = LatencyMeasurer::Phase::Idle;
    CHECK (ui::LatencyPanel::describeMeasurement (st, "No input is open.") == "No input is open.");

    AudioEngineHost::BufferInfo info;
    info.deviceOpen = info.managed = info.automatic = true;
    info.deviceTypeName = "Windows Audio (Low Latency Mode)";
    info.sampleRate = 48000.0;
    info.current = info.target = 128;
    info.smallest = 128;
    info.largest = 480;
    info.profile = Profile::LowLatency;
    info.reason = Reason::Smallest;
    LatencyInfo none;
    CHECK (ui::LatencyPanel::describeBuffer (info, none, 0)
           == "Buffer 128 samples (2.7 ms) at 48.0 kHz. Low Latency: the smallest size the device offers of at least 1.3 ms (it offers 128 .. 480).");
    info.reason = Reason::Floor;
    info.floor = info.current = info.target = 160;
    CHECK (ui::LatencyPanel::describeBuffer (info, none, 1).contains ("Raised to 160 samples after dropouts"));
    CHECK (ui::LatencyPanel::describeBuffer (info, none, 1).contains ("Raised 1 time this session"));
    info.managed = false;
    info.deviceTypeName = "Windows Audio";
    CHECK (ui::LatencyPanel::describeBuffer (info, none, 0).contains ("Windows Audio (Low Latency Mode)"));
}

