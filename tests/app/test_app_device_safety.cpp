// App-level tests: device safety in AudioEngineHost.
//   * Captures follow the device rate: a device rate change restarts every
//     running per-app capture at the new rate (same id), so the capture FIFO
//     never resamples at a large ratio with its non-band-limiting
//     interpolator (hardening: routing-fifo).
//   * A capture slot released while the device callback is stalled is
//     quarantined, never re-prepared under a reader (hardening: routing-fifo).
//   * docs/11 E51 Phase A: the loopback-pair guard (pair table; output held
//     at silence from the first sample; engine frozen; banner state; per-pair
//     override), the exact real-world case through JUCE's own device-list
//     fallback (a fake juce::AudioIODeviceType: the preferred wireless
//     headset disappears, JUCE falls back to the default output "CABLE
//     Input" while the input is "CABLE Output"), and no stale latency readout.
//   * docs/11 E42a: per-strip latency readout (own / padding / output).
#include "AppTestSupport.h"

#include "engine/AudioEngineHost.h"

#include "flub/engine/Parameters.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

using namespace flub::app;

namespace
{
constexpr int kBlock = 256;

class FakeAudioDevice final : public juce::AudioIODevice
{
public:
    explicit FakeAudioDevice (double sampleRate = 48000.0, const juce::String& deviceName = "Fake Output")
        : juce::AudioIODevice (deviceName, "Fake"), rate (sampleRate)
    {
    }

    juce::StringArray getOutputChannelNames() override { return { "Left", "Right" }; }
    juce::StringArray getInputChannelNames() override { return { "In 1", "In 2" }; }
    juce::Array<double> getAvailableSampleRates() override { return { rate }; }
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
    double getCurrentSampleRate() override { return rate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return juce::BigInteger (0x3); }
    juce::BigInteger getActiveInputChannels() const override { return juce::BigInteger (0x3); }
    int getOutputLatencyInSamples() override { return kBlock; }
    int getInputLatencyInSamples() override { return kBlock; }

private:
    double rate;
};

/** Stands in for a platform::ProcessLoopbackCapture and records every start. */
class RecordingCapture final : public flub::platform::ProcessLoopbackCapture
{
public:
    bool isSupported() const override { return true; }

    bool start (uint32_t, bool, double sampleRate, int numChannels, FrameCallback frameCallback, std::string& error) override
    {
        startRates.push_back (sampleRate);
        if (failStarts > 0)
        {
            --failStarts;
            error = "Simulated start failure";
            return false;
        }
        channels = numChannels;
        callback = std::move (frameCallback);
        running = true;
        return true;
    }

    void stop() override { running = false; }
    bool isRunning() const override { return running; }

    void deliver (int numFrames, double rate)
    {
        packet.resize (static_cast<size_t> (numFrames * channels));
        for (int i = 0; i < numFrames; ++i, ++frame)
            for (int c = 0; c < channels; ++c)
                packet[static_cast<size_t> (i * channels + c)] =
                    0.3f * static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * static_cast<double> (frame) / rate));
        if (running && callback != nullptr)
            callback (packet.data(), numFrames, channels);
    }

    std::vector<double> startRates;
    int failStarts = 0;

private:
    FrameCallback callback;
    std::vector<float> packet;
    int channels = 2;
    int64_t frame = 0;
    bool running = false;
};

/** Runs `numBlocks` callbacks on a fresh thread (the backend's audio thread)
    and joins it. `inputs` / `outputs` hold one block per channel. */
struct BlockRunner
{
    std::array<std::vector<float>, 2> in { std::vector<float> (kBlock), std::vector<float> (kBlock) };
    std::array<std::vector<float>, 2> out { std::vector<float> (kBlock), std::vector<float> (kBlock) };
    std::array<std::vector<float>, 2> recorded;
    int64_t sample = 0;
    double rate = 48000.0;
    bool feedback = false; // input = previous output (a closed loop) + the sine
    std::function<void()> beforeBlock;

    template <typename Callback>
    void run (int numBlocks, Callback&& callback)
    {
        std::thread t ([&]
        {
            for (int b = 0; b < numBlocks; ++b)
            {
                if (beforeBlock != nullptr)
                    beforeBlock();
                for (size_t c = 0; c < 2; ++c)
                    for (int i = 0; i < kBlock; ++i)
                    {
                        const float sine = 0.25f * static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * 220.0
                                                                                 * static_cast<double> (sample + i) / rate));
                        in[c][static_cast<size_t> (i)] = sine + (feedback ? out[c][static_cast<size_t> (i)] : 0.0f);
                    }
                sample += kBlock;
                const std::array<const float*, 2> ins { in[0].data(), in[1].data() };
                const std::array<float*, 2> outs { out[0].data(), out[1].data() };
                callback (ins.data(), outs.data());
                for (size_t c = 0; c < 2; ++c)
                    recorded[c].insert (recorded[c].end(), out[c].begin(), out[c].end());
            }
        });
        t.join();
    }

    void runHost (AudioEngineHost& host, int numBlocks)
    {
        run (numBlocks, [&host] (const float* const* ins, float* const* outs)
             { host.audioDeviceIOCallbackWithContext (ins, 2, outs, 2, kBlock, juce::AudioIODeviceCallbackContext {}); });
    }

    float peak (size_t from, size_t to) const
    {
        float p = 0.0f;
        for (const auto& ch : recorded)
            for (size_t i = from; i < std::min (to, ch.size()); ++i)
                p = std::max (p, std::abs (ch[i]));
        return p;
    }

    float maxStep (size_t from, size_t to) const
    {
        float s = 0.0f;
        for (const auto& ch : recorded)
            for (size_t i = std::max<size_t> (from, 1); i < std::min (to, ch.size()); ++i)
                s = std::max (s, std::abs (ch[i] - ch[i - 1]));
        return s;
    }
};

// ---- A fake device backend for AudioDeviceManager (JUCE's own fallback path) ----
class FakeBackendDevice final : public juce::AudioIODevice
{
public:
    FakeBackendDevice (const juce::String& outputName, const juce::String& inputName)
        : juce::AudioIODevice (outputName.isNotEmpty() ? outputName : inputName, "FakeBackend"), hasInput (inputName.isNotEmpty()),
          hasOutput (outputName.isNotEmpty())
    {
    }

    ~FakeBackendDevice() override { stop(); }

    juce::StringArray getOutputChannelNames() override { return hasOutput ? juce::StringArray { "Left", "Right" } : juce::StringArray(); }
    juce::StringArray getInputChannelNames() override { return hasInput ? juce::StringArray { "Left", "Right" } : juce::StringArray(); }
    juce::Array<double> getAvailableSampleRates() override { return { 48000.0 }; }
    juce::Array<int> getAvailableBufferSizes() override { return { kBlock }; }
    int getDefaultBufferSize() override { return kBlock; }
    juce::String open (const juce::BigInteger& ins, const juce::BigInteger& outs, double, int) override
    {
        activeIns = hasInput ? ins : juce::BigInteger();
        activeOuts = hasOutput ? outs : juce::BigInteger();
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
    int getCurrentBufferSizeSamples() override { return kBlock; }
    double getCurrentSampleRate() override { return 48000.0; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return activeOuts; }
    juce::BigInteger getActiveInputChannels() const override { return activeIns; }
    int getOutputLatencyInSamples() override { return kBlock; }
    int getInputLatencyInSamples() override { return kBlock; }

    /** The backend's audio thread calls this (JUCE's CallbackHandler). */
    void process (const float* const* ins, float* const* outs)
    {
        if (callback != nullptr)
            callback->audioDeviceIOCallbackWithContext (ins, 2, outs, 2, kBlock, juce::AudioIODeviceCallbackContext {});
    }

private:
    juce::AudioIODeviceCallback* callback = nullptr;
    juce::BigInteger activeIns, activeOuts;
    bool hasInput, hasOutput, opened = false;
};

class FakeBackend final : public juce::AudioIODeviceType
{
public:
    FakeBackend() : juce::AudioIODeviceType ("FakeBackend") {}

    juce::StringArray outputs, inputs;
    int defaultOutput = 0, defaultInput = 0;

    void scanForDevices() override {}
    juce::StringArray getDeviceNames (bool wantInputNames) const override { return wantInputNames ? inputs : outputs; }
    int getDefaultDeviceIndex (bool forInput) const override { return forInput ? defaultInput : defaultOutput; }
    int getIndexOfDevice (juce::AudioIODevice* device, bool asInput) const override
    {
        return device != nullptr ? (asInput ? inputs : outputs).indexOf (device->getName()) : -1;
    }
    bool hasSeparateInputsAndOutputs() const override { return true; }
    juce::AudioIODevice* createDevice (const juce::String& outputName, const juce::String& inputName) override
    {
        if ((outputName.isNotEmpty() && ! outputs.contains (outputName)) || (inputName.isNotEmpty() && ! inputs.contains (inputName)))
            return nullptr;
        return new FakeBackendDevice (outputName, inputName);
    }

    /** The OS reported a device list change (hot-plug). */
    void devicesChanged() { callDeviceChangeListeners(); }
};
} // namespace

TEST_CASE ("App: loopback-pair table matches exact partner pairs, not vendor tokens (docs/11 E51)")
{
    const auto pair = [] (const char* in, const char* out) { return AudioEngineHost::isLoopbackPair (in, out); };

    // VB-Audio cables: the Output side records what is played to the Input side.
    CHECK (pair ("CABLE Output (VB-Audio Virtual Cable)", "CABLE Input (VB-Audio Virtual Cable)"));
    CHECK (pair ("CABLE-A Output (VB-Audio Cable A)", "CABLE-A Input (VB-Audio Cable A)"));
    CHECK (pair ("Hi-Fi Cable Output (VB-Audio Hi-Fi Cable)", "Hi-Fi Cable Input (VB-Audio Hi-Fi Cable)"));
    // ... but two different cables are a legitimate chain.
    CHECK (! pair ("CABLE Output (VB-Audio Virtual Cable)", "CABLE-A Input (VB-Audio Cable A)"));
    CHECK (! pair ("CABLE-B Output (VB-Audio Cable B)", "CABLE-A Input (VB-Audio Cable A)"));

    // Voicemeeter: the same VAIO driver (its bus) is a pair; another bus is not.
    CHECK (pair ("Voicemeeter Out B1 (VB-Audio Voicemeeter VAIO)", "Voicemeeter Input (VB-Audio Voicemeeter VAIO)"));
    CHECK (pair ("Voicemeeter Out B2 (VB-Audio Voicemeeter AUX VAIO)", "Voicemeeter AUX Input (VB-Audio Voicemeeter AUX VAIO)"));
    CHECK (pair ("VoiceMeeter Output (VB-Audio VoiceMeeter VAIO)", "VoiceMeeter Input (VB-Audio VoiceMeeter VAIO)"));
    CHECK (! pair ("Voicemeeter Out B1 (VB-Audio Voicemeeter VAIO)", "Voicemeeter AUX Input (VB-Audio Voicemeeter AUX VAIO)"));

    // One virtual device as both ends (macOS), a sink and its monitor (Linux).
    CHECK (pair ("BlackHole 2ch", "BlackHole 2ch"));
    CHECK (pair ("Soundflower (2ch)", "Soundflower (2ch)"));
    CHECK (pair ("Loopback Audio", "Loopback Audio"));
    CHECK (pair ("flubsound_game.monitor", "flubsound_game"));
    CHECK (pair ("Monitor of Built-in Audio Analog Stereo", "Built-in Audio Analog Stereo"));
    CHECK (! pair ("BlackHole 16ch", "BlackHole 2ch"));

    // Stereo Mix records the same codec's output.
    CHECK (pair ("Stereo Mix (Realtek(R) Audio)", "Speakers (Realtek(R) Audio)"));
    CHECK (! pair ("Stereo Mix (Realtek(R) Audio)", "Headphones (USB Audio)"));

    // Physical devices: a headset's microphone and earcups share a name; a
    // cable feeding a physical output is the normal cable setup.
    CHECK (! pair ("Arctis 7 Game", "Arctis 7 Game"));
    CHECK (! pair ("Microphone (Arctis 7 Chat)", "Headphones (Arctis 7 Game)"));
    CHECK (! pair ("CABLE Output (VB-Audio Virtual Cable)", "Headphones (Arctis 7 Game)"));
    CHECK (! pair ("Line Input (Focusrite USB Audio)", "Line Output (Focusrite USB Audio)"));
    CHECK (! pair ("", "CABLE Input (VB-Audio Virtual Cable)"));
}

TEST_CASE ("App: a device rate change restarts running captures at the new rate (same id), not a 3:1 resample")
{
    AudioEngineHost host;
    RecordingCapture* capture = nullptr;
    host.setCaptureFactory ([&capture]
                            {
                                auto c = std::make_unique<RecordingCapture>();
                                capture = c.get();
                                return c;
                            });

    FakeAudioDevice device48 (48000.0);
    host.audioDeviceAboutToStart (&device48);
    juce::String error;
    const int id = host.startProcessCapture (1, 4242, error);
    REQUIRE (id >= 0);
    REQUIRE (capture != nullptr);
    REQUIRE (capture->startRates.size() == 1);
    CHECK (capture->startRates[0] == 48000.0);

    // The output switches to a hands-free headset (16 kHz): the device restarts.
    host.audioDeviceStopped();
    FakeAudioDevice device16 (16000.0);
    host.audioDeviceAboutToStart (&device16);
    CHECK (host.getSampleRate() == 16000.0);
    // The restart runs on the message thread after the device start (it can
    // block; the start may hold JUCE's callback lock).
    CHECK (flubapptest::pumpMessagesUntil ([&] { return capture->startRates.size() >= 2; }, 5000));
    REQUIRE (capture->startRates.size() == 2);
    CHECK (capture->startRates[1] == 16000.0);

    auto captures = host.getCaptures();
    REQUIRE (captures.size() == 1);
    CHECK (captures[0].id == id);
    CHECK (captures[0].strip == 1);
    CHECK (captures[0].running);
    CHECK (captures[0].sampleRate == 16000.0); // the FIFO's nominal ratio is 1 again
    CHECK (captures[0].restartError.isEmpty());

    // The restarted capture streams: 10 ms packets at 16 kHz feed the strip.
    BlockRunner audio;
    audio.rate = 16000.0;
    int64_t delivered = 0;
    audio.beforeBlock = [&]
    {
        while (delivered < audio.sample + 2 * kBlock)
        {
            capture->deliver (160, 16000.0);
            delivered += 160;
        }
    };
    audio.runHost (host, 200);
    captures = host.getCaptures();
    CHECK (captures[0].stats.streaming);
    CHECK (host.isStripActive (1));

    // A restart that fails is reported (not running, with the reason) and
    // retried; a later successful restart clears it.
    host.audioDeviceStopped();
    capture->failStarts = 1;
    host.audioDeviceAboutToStart (&device48);
    CHECK (flubapptest::pumpMessagesUntil ([&] { return capture->startRates.size() >= 3; }, 5000));
    captures = host.getCaptures();
    REQUIRE (captures.size() == 1);
    CHECK (! captures[0].running);
    CHECK (captures[0].restartError == "Simulated start failure");
    host.restartCapturesAtDeviceRate();
    captures = host.getCaptures();
    CHECK (captures[0].running);
    CHECK (captures[0].sampleRate == 48000.0);
    CHECK (captures[0].restartError.isEmpty());

    // Same rate again (buffer-size-only restart): no capture restart.
    host.audioDeviceStopped();
    const auto starts = capture->startRates.size();
    host.audioDeviceAboutToStart (&device48);
    flubapptest::pumpMessagesUntil ([] { return false; }, 50); // let any async restart run (bounded)
    CHECK (capture->startRates.size() == starts);

    host.audioDeviceStopped();
    host.stopAllCaptures();
}

TEST_CASE ("App: a capture slot released while the device callback is stalled is quarantined, not reused")
{
    AudioEngineHost host;
    host.setCaptureFactory ([] { return std::make_unique<RecordingCapture>(); });
    FakeAudioDevice device;
    host.audioDeviceAboutToStart (&device); // the device "runs", but no callback completes: a stalled audio thread

    juce::String error;
    const int first = host.startProcessCapture (1, 100, error);
    REQUIRE (first == 0);

    // The release cannot confirm that the stalled callback left the FIFO: the
    // slot must not be handed to the next capture (it would re-allocate the
    // FIFO under the reader).
    host.stopProcessCapture (first);
    const int second = host.startProcessCapture (1, 200, error);
    CHECK (second >= 0);
    CHECK (second != first);

    // A second release while the same callback is still stalled does not
    // wait again (bounded message-thread cost) and is quarantined as well.
    const auto begin = juce::Time::getMillisecondCounter();
    host.stopProcessCapture (second);
    CHECK (juce::Time::getMillisecondCounter() - begin < 200u); // the first wait took >= 250 ms; this one does not wait
    const int third = host.startProcessCapture (1, 300, error);
    CHECK (third >= 0);
    CHECK (third != first);
    CHECK (third != second);

    // Once a callback has completed, the quarantined slots are free again.
    BlockRunner audio;
    audio.runHost (host, 1);
    host.stopProcessCapture (third);
    CHECK (host.startProcessCapture (1, 400, error) == first);

    host.audioDeviceStopped();
    host.stopAllCaptures();
}

TEST_CASE ("App: loopback guard holds the output silent from the first sample, freezes the engine, and the override releases it")
{
    AudioEngineHost host;
    host.setDeviceInputRouting (1, 0);
    int notifications = 0;
    host.onDeviceSafetyChanged = [&notifications] { ++notifications; };

    // BlackHole as output and input: everything played is recorded again.
    FakeAudioDevice device (48000.0, "BlackHole 2ch");
    host.audioDeviceAboutToStart (&device);
    CHECK (host.isOutputMutedByGuard());
    auto state = host.getDeviceSafetyState();
    CHECK (state.kind == DeviceSafetyState::Kind::LoopbackPair);
    CHECK (state.outputMuted);
    CHECK (state.message.contains ("BlackHole 2ch"));
    CHECK (state.message.contains ("feedback loop"));
    CHECK (flubapptest::pumpMessagesUntil ([&] { return notifications >= 1; }, 5000));

    // No stale latency while the output is held (docs/11 E51).
    CHECK (! host.getLatencyInfo().valid);
    CHECK (AudioEngineHost::formatTotalLatency (host.getLatencyInfo()) == "--");

    BlockRunner audio;
    audio.feedback = true; // the loop: input = what was played + the source
    audio.runHost (host, 100);
    CHECK (audio.peak (0, audio.recorded[0].size()) == 0.0f); // silent from the very first sample
    CHECK (! host.isStripActive (1));                         // the engine is frozen, nothing is processed

    // The user allows this pair (a deliberate setup): the output ramps up.
    host.allowLoopbackPair ("BlackHole 2ch", "BlackHole 2ch");
    CHECK (! host.isOutputMutedByGuard());
    CHECK (host.getDeviceSafetyState().kind == DeviceSafetyState::Kind::None);
    CHECK (host.getLatencyInfo().valid);
    audio.feedback = false;
    const size_t release = audio.recorded[0].size();
    audio.runHost (host, 200);
    const size_t end = audio.recorded[0].size();
    CHECK (audio.peak (end - 50 * kBlock, end) > 0.05f);
    // No step from silence: bounded by the steady signal's own step plus the ramp.
    const float steadyStep = audio.maxStep (end - 50 * kBlock, end);
    const float ramp = audio.peak (end - 50 * kBlock, end) / static_cast<float> (std::lround (AudioEngineHost::kSwapFadeMs * 48.0));
    CHECK_LE (audio.maxStep (release, end), 1.1f * steadyStep + ramp);
    std::cerr << "    guard release: max step " << audio.maxStep (release, end) << ", steady " << steadyStep << "\n";

    // Tripped while running (the allowed pair is withdrawn): ramped to
    // silence within one fade, then frozen.
    host.clearAllowedLoopbackPairs();
    CHECK (host.isOutputMutedByGuard());
    const size_t trip = audio.recorded[0].size();
    audio.runHost (host, 20);
    const auto fade = static_cast<size_t> (std::lround (AudioEngineHost::kSwapFadeMs * 48.0));
    CHECK (audio.peak (trip + fade, audio.recorded[0].size()) == 0.0f);
    CHECK_LE (audio.maxStep (trip, trip + fade + 1), 1.1f * steadyStep + ramp);

    // With the device input feeding no strip the pair is no loop: not muted,
    // and the banner follows (timer); feeding a strip again mutes at once.
    host.setDeviceInputRouting (-1);
    CHECK (! host.isOutputMutedByGuard());
    CHECK (flubapptest::pumpMessagesUntil ([&] { return host.getDeviceSafetyState().kind == DeviceSafetyState::Kind::None; }, 5000));
    host.setDeviceInputRouting (1, 0);
    CHECK (host.isOutputMutedByGuard());
    CHECK (flubapptest::pumpMessagesUntil ([&] { return host.getDeviceSafetyState().kind == DeviceSafetyState::Kind::LoopbackPair; },
                                           5000));

    host.audioDeviceStopped();
    CHECK (! host.getLatencyInfo().valid); // stopped: no latency
}

TEST_CASE ("App: wireless headset drops, JUCE falls back to CABLE Input with CABLE Output as input: output silent, banner, recovery (E51)")
{
    const juce::String headset ("Headphones (Wireless Headset)");
    const juce::String cableIn ("CABLE Input (VB-Audio Virtual Cable)");
    const juce::String cableOut ("CABLE Output (VB-Audio Virtual Cable)");

    AudioEngineHost host;
    auto backendOwner = std::make_unique<FakeBackend>();
    auto* backend = backendOwner.get();
    backend->outputs = { headset, cableIn };
    backend->inputs = { cableOut };
    backend->defaultOutput = 1; // the cable setup: the system default output IS CABLE Input
    backend->defaultInput = 0;
    host.getDeviceManager().addAudioDeviceType (std::move (backendOwner));
    host.setDeviceInputRouting (1, 0);

    juce::XmlElement saved ("DEVICESETUP");
    saved.setAttribute ("deviceType", "FakeBackend");
    saved.setAttribute ("audioOutputDeviceName", headset);
    saved.setAttribute ("audioInputDeviceName", cableOut);
    REQUIRE (host.openDevice (&saved, 2, 2).isEmpty());

    const auto currentDevice = [&host] { return dynamic_cast<FakeBackendDevice*> (host.getDeviceManager().getCurrentAudioDevice()); };
    REQUIRE (currentDevice() != nullptr);
    CHECK (currentDevice()->getName() == headset);
    CHECK (! host.isOutputMutedByGuard());
    CHECK (host.getDeviceSafetyState().kind == DeviceSafetyState::Kind::None);

    BlockRunner audio;
    const auto runDevice = [&] (int blocks)
    { audio.run (blocks, [&] (const float* const* ins, float* const* outs) { currentDevice()->process (ins, outs); }); };
    runDevice (300);
    CHECK (host.getLatencyInfo().valid);
    CHECK (AudioEngineHost::formatTotalLatency (host.getLatencyInfo()).endsWith ("ms (est.)"));
    size_t end = audio.recorded[0].size();
    CHECK (audio.peak (end - 50 * kBlock, end) > 0.05f);
    const auto before = host.getMixEngine().chain (1).meters().governorScale.load();

    // The headset drops: JUCE's audioDeviceListChanged reopens with
    // selectDefaultDeviceOnFailure, i.e. the default output CABLE Input.
    backend->outputs = { cableIn };
    backend->devicesChanged();
    REQUIRE (currentDevice() != nullptr);
    CHECK (currentDevice()->getName() == cableIn);
    CHECK (host.getDeviceManager().getAudioDeviceSetup().inputDeviceName == cableOut);

    CHECK (host.isOutputMutedByGuard());
    const auto state = host.getDeviceSafetyState();
    CHECK (state.kind == DeviceSafetyState::Kind::LoopbackPair);
    CHECK (state.inputDeviceName == cableOut);
    CHECK (state.outputDeviceName == cableIn);
    CHECK (state.message.contains (cableIn));
    CHECK (! host.getLatencyInfo().valid);

    // The loop is closed (anything played to CABLE Input comes back on CABLE
    // Output), but nothing is played: silent from the first sample.
    audio.feedback = true;
    const size_t drop = audio.recorded[0].size();
    runDevice (200);
    CHECK (audio.peak (drop, audio.recorded[0].size()) == 0.0f);

    // The headset returns and the preferred output is selected again (what
    // EngineController::trackPreferredOutput does): the guard releases, the
    // banner clears and the output comes back with its protection state
    // not stepped down by the loop.
    audio.feedback = false;
    backend->outputs = { headset, cableIn };
    backend->devicesChanged();
    auto setup = host.getDeviceManager().getAudioDeviceSetup();
    setup.outputDeviceName = headset;
    REQUIRE (host.getDeviceManager().setAudioDeviceSetup (setup, true).isEmpty());
    CHECK (currentDevice()->getName() == headset);
    CHECK (! host.isOutputMutedByGuard());
    CHECK (host.getDeviceSafetyState().kind == DeviceSafetyState::Kind::None);
    runDevice (300);
    end = audio.recorded[0].size();
    CHECK (audio.peak (end - 50 * kBlock, end) > 0.05f);
    const auto after = host.getMixEngine().chain (1).meters().governorScale.load();
    std::cerr << "    headset drop: governor scale " << before << " -> " << after << "\n";
    CHECK (after >= before - 0.01f);
    CHECK (host.getLatencyInfo().valid);

    // A device error shows in the banner state and clears with the next start.
    host.audioDeviceError ("The device was invalidated");
    CHECK (flubapptest::pumpMessagesUntil ([&] { return host.getDeviceSafetyState().kind == DeviceSafetyState::Kind::DeviceError; },
                                           5000));
    CHECK (host.getDeviceSafetyState().message == "The device was invalidated");
    host.getDeviceManager().closeAudioDevice();
    host.getDeviceManager().restartLastAudioDevice();
    CHECK (host.getDeviceSafetyState().kind == DeviceSafetyState::Kind::None);

    host.closeDevice();
}

TEST_CASE ("App: per-strip latency readout shows each strip's own latency and the padding its sync group adds (E42a)")
{
    using Profile = flub::param::LatencyProfileValue;
    AudioEngineHost host;
    host.prepareOffline (48000.0, kBlock);

    // Offline: no device, so no valid (audible) latency.
    auto info = host.getLatencyInfo();
    CHECK (! info.valid);
    CHECK (info.estimated);
    REQUIRE (info.numStrips == 4);

    // Game on Low Latency, Music on Quality (like a Quality preset on Music).
    const auto setProfiles = [&host]
    {
        host.getMixEngine().params (0).set (flub::param::LatencyProfile, static_cast<float> (Profile::LowLatency));
        host.getMixEngine().params (1).set (flub::param::LatencyProfile, static_cast<float> (Profile::Quality));
        host.getMixEngine().params (2).set (flub::param::LatencyProfile, static_cast<float> (Profile::LowLatency));
        host.getMixEngine().params (3).set (flub::param::LatencyProfile, static_cast<float> (Profile::LowLatency));
        host.reconfigure();
    };
    setProfiles();
    info = host.getLatencyInfo();

    // Every strip in a group of its own (the default layout): nobody is
    // padded, and the readout is what the MixEngine runs, strip by strip.
    auto& engine = host.getMixEngine();
    for (int s = 0; s < info.numStrips; ++s)
    {
        const auto& strip = info.strips[static_cast<size_t> (s)];
        CHECK (strip.paddingSamples == 0);
        CHECK (strip.outputSamples == engine.getStripLatencySamples (s));
        CHECK (strip.ownSamples == strip.outputSamples);
    }
    std::cerr << "    per-strip latency, no sync group: Game " << info.strips[0].outputSamples << ", Music " << info.strips[1].outputSamples
              << " (engine " << info.engineSamples << ")\n";
    CHECK (info.strips[1].ownSamples > info.strips[0].ownSamples); // Quality needs more than Low Latency
    CHECK (info.strips[1].outputSamples == info.engineSamples);     // the slowest strip sets the engine figure
    CHECK (info.strips[0].outputSamples < info.engineSamples);      // ... and no longer delays Game
    CHECK (std::abs (info.strips[0].ownMs - 1000.0 * info.strips[0].ownSamples / 48000.0) < 1.0e-9);

    // Game and Music in one sync group: Game is padded to Music, and the
    // readout says by how much.
    auto layout = host.getStripLayout();
    REQUIRE (layout.size() >= 2);
    layout[0].syncGroup = 1;
    layout[1].syncGroup = 1;
    host.setStripLayout (layout);
    setProfiles();
    info = host.getLatencyInfo();
    const auto& game = info.strips[0];
    const auto& music = info.strips[1];
    std::cerr << "    per-strip latency, one sync group: Game own " << game.ownSamples << " + padding " << game.paddingSamples << " = "
              << game.outputSamples << "; Music own " << music.ownSamples << " (engine " << info.engineSamples << ")\n";
    CHECK (music.paddingSamples == 0); // the slowest strip of the group is not padded
    CHECK (music.ownSamples == info.engineSamples);
    CHECK (game.outputSamples == music.outputSamples);
    CHECK (game.paddingSamples == music.ownSamples - game.ownSamples);
    CHECK (game.paddingSamples > 0);
    CHECK (game.paddingSamples == host.getMixEngine().getStripPaddingSamples (0));
    CHECK (info.strips[2].paddingSamples == 0); // Chat is in no group

    // A known graph quantum is part of the (estimated) total.
    const double withoutQuantum = info.totalMs;
    host.setGraphQuantumMs (21.3);
    info = host.getLatencyInfo();
    CHECK (std::abs (info.totalMs - withoutQuantum - 21.3) < 1.0e-9);
    CHECK (info.graphQuantumMs == 21.3);
}
