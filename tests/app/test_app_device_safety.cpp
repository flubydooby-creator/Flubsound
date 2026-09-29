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
//   * docs/11 E51 remainder: the safe speaker profile caps the bass lift at
//     +3 dB instead of bypassing the bass engine (fallback to speakers,
//     measured against the headset render); "Follow the system default
//     output" (host: honoured, ended by picking an output; EngineController:
//     persisted and applied); the device correction keyed by the endpoint's
//     identity (a curve saved on "Headset Earphone (Stealth 700 Gen 2)" is
//     applied after a re-plug as "(2- Stealth 700 Gen 2)"; files from
//     before E51, keyed by the name, still load); the preferred output
//     stored with its identity.
#include "AppTestSupport.h"

#include "engine/AudioEngineHost.h"
#include "engine/EngineController.h"

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
    std::function<float (int64_t)> source; // the input at a sample index instead of the 220 Hz sine

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
                        const float sine = source != nullptr ? source (sample + i)
                                                             : 0.25f * static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * 220.0
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

// =============================================================================
// docs/11 E51 remainder: safe-profile bass cap, follow the system default,
// the correction and the preferred output keyed by the endpoint's identity
// =============================================================================
namespace
{
const juce::String kStealth ("Headset Earphone (Stealth 700 Gen 2)");
const juce::String kStealthPort2 ("Headset Earphone (2- Stealth 700 Gen 2)"); // Windows' instance number: another USB port
const juce::String kRealtekSpeakers ("Speakers (Realtek(R) Audio)");
const juce::String kLineIn ("Line In (Realtek(R) Audio)");
constexpr const char* kStealthHardware = "USB\\VID_10F5&PID_0210&MI_00";
constexpr float kToneAmp = 0.003f; // -50 dBFS per tone: under the bass engine's protection with 19 dB of lift
constexpr int kMeasureBlocks = 150; // 0.8 s at 48 kHz: 32 cycles of 40 Hz, 800 of 1 kHz

/** Level of `freq` in the last kMeasureBlocks of `x`, dB re kToneAmp. */
double toneDb (const std::vector<float>& x, double freq)
{
    const size_t n = static_cast<size_t> (kMeasureBlocks * kBlock), from = x.size() - n;
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double a = 2.0 * juce::MathConstants<double>::pi * freq * static_cast<double> (i) / 48000.0;
        re += static_cast<double> (x[from + i]) * std::cos (a);
        im -= static_cast<double> (x[from + i]) * std::sin (a);
    }
    return 20.0 * std::log10 (std::max (1.0e-12, 2.0 * std::sqrt (re * re + im * im) / static_cast<double> (n) / kToneAmp));
}

/** The Music strip voiced for the headset: bass.boost +9 dB at 70 Hz, an EQ
    low shelf +6 dB at 100 Hz and a +4 dB bell at 60 Hz (19 dB of bass lift);
    everything else that could shape the two tones off. */
void voiceForHeadset (flub::param::ParameterStore& p)
{
    using namespace flub::param;
    p.set (Mode, static_cast<float> (ModeValue::Music));
    p.set (BoostIntensity, 0.0f);
    for (int id : { Macro1, Macro2, Macro3, Macro4, Macro5 })
        p.set (id, 0.0f);
    for (int id : { AutoLevelOn, AutoPreampOn, GateOn, DynEqOn, ClarityOn, SaturationOn, SpatialOn, VirtualizerOn, CompressorOn, MaximizerOn })
        p.set (id, 0.0f);
    p.set (EqOn, 1.0f);
    p.set (BassOn, 1.0f);
    p.set (BassBoostDb, 9.0f);
    p.set (BassBoostFreq, 70.0f);
    p.set (eq (1, EqFieldOn), 1.0f);
    p.set (eq (1, EqFieldType), 1.0f); // Low Shelf
    p.set (eq (1, EqFieldFreq), 100.0f);
    p.set (eq (1, EqFieldGain), 6.0f);
    p.set (eq (2, EqFieldOn), 1.0f);
    p.set (eq (2, EqFieldType), 0.0f); // Bell
    p.set (eq (2, EqFieldFreq), 60.0f);
    p.set (eq (2, EqFieldGain), 4.0f);
    p.set (eq (2, EqFieldQ), 0.7f);
}

flub::platform::OutputEndpointIdentity usbEndpoint (const char* id, const juce::String& name, const char* hardwareId)
{
    flub::platform::OutputEndpointIdentity e;
    e.id = id;
    e.name = name.toStdString();
    e.hardwareId = hardwareId;
    e.transport = flub::platform::EndpointTransport::Usb;
    return e;
}

constexpr const char* kStealthCurve = "Preamp: -4.0 dB\n"
                                      "Filter 1: ON LSC Fc 105 Hz Gain 3.5 dB Q 0.70\n"
                                      "Filter 2: ON PK Fc 2600 Hz Gain -2.0 dB Q 2.00\n";
} // namespace

TEST_CASE ("App: E51 fallback to speakers caps the bass at +3 dB re the flat mids instead of switching the bass engine off")
{
    AudioEngineHost host;
    auto backendOwner = std::make_unique<FakeBackend>();
    auto* backend = backendOwner.get();
    backend->outputs = { kStealth, kRealtekSpeakers };
    backend->inputs = { kLineIn };
    backend->defaultOutput = 1;
    host.getDeviceManager().addAudioDeviceType (std::move (backendOwner));
    host.setDeviceInputRouting (1, 0);
    voiceForHeadset (host.getMixEngine().params (1));

    juce::XmlElement saved ("DEVICESETUP");
    saved.setAttribute ("deviceType", "FakeBackend");
    saved.setAttribute ("audioOutputDeviceName", kStealth);
    saved.setAttribute ("audioInputDeviceName", kLineIn);
    REQUIRE (host.openDevice (&saved, 2, 2).isEmpty());
    const auto currentDevice = [&host] { return dynamic_cast<FakeBackendDevice*> (host.getDeviceManager().getCurrentAudioDevice()); };
    REQUIRE (currentDevice() != nullptr);
    CHECK (currentDevice()->getName() == kStealth);

    BlockRunner audio;
    audio.source = [] (int64_t i)
    {
        const double t = static_cast<double> (i) / 48000.0;
        return kToneAmp * static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * 40.0 * t)
                                              + std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * t));
    };
    const auto bassLiftDb = [&]
    {
        audio.recorded = {};
        audio.run (100 + kMeasureBlocks, [&] (const float* const* ins, float* const* outs) { currentDevice()->process (ins, outs); });
        return toneDb (audio.recorded[0], 40.0) - toneDb (audio.recorded[0], 1000.0);
    };
    const double headset = bassLiftDb();
    const double headsetMid = toneDb (audio.recorded[0], 1000.0);

    // The dongle is unplugged: the speakers with the safe speaker profile.
    backend->outputs = { kRealtekSpeakers };
    backend->defaultOutput = 0;
    backend->devicesChanged();
    CHECK (flubapptest::pumpMessagesUntil ([&] { return host.isSafeSpeakerProfileActive(); }, 2000)); // JUCE's own fallback, then the host's selection
    CHECK (currentDevice() != nullptr && currentDevice()->getName() == kRealtekSpeakers);
    REQUIRE (host.isSafeSpeakerProfileActive());
    auto& chain = host.getMixEngine().chain (1);
    CHECK (chain.getSafeSpeakerBassCapDb() == AudioEngineHost::kSafeSpeakerBassCapDb);
    CHECK (! chain.isAuditionBypassed (flub::param::BassOn)); // capped, not bypassed
    CHECK (chain.isAuditionBypassed (flub::param::VirtualizerOn));
    CHECK (host.getDeviceSafetyState().fallbackMessage.contains ("bass at most +3 dB"));
    const double fallback = bassLiftDb();
    const double fallbackMid = toneDb (audio.recorded[0], 1000.0);
    CHECK (chain.effectiveValue (flub::param::BassOn) == 1.0f);
    CHECK (std::abs (chain.effectiveValue (flub::param::BassBoostDb) - 9.0f * 3.0f / 19.0f) < 1.0e-3f);
    CHECK (host.getMixEngine().params (1).get (flub::param::BassBoostDb) == 9.0f); // the preset is untouched

    // Before this change the profile bypassed the bass engine and left the
    // preset's EQ lift in place (the same state, set by hand).
    for (int s = 0; s < host.getMixEngine().getNumStrips(); ++s)
    {
        host.getMixEngine().chain (s).setSafeSpeakerBassCapDb (flub::ProcessingChain::kNoBassCap);
        host.getMixEngine().chain (s).setAuditionBypass (flub::param::BassOn, true);
    }
    const double bypassed = bassLiftDb();
    for (int s = 0; s < host.getMixEngine().getNumStrips(); ++s)
    {
        host.getMixEngine().chain (s).setAuditionBypass (flub::param::BassOn, false);
        host.getMixEngine().chain (s).setSafeSpeakerBassCapDb (AudioEngineHost::kSafeSpeakerBassCapDb);
    }

    std::cerr << "    bass lift at 40 Hz re 1 kHz: headset " << headset << " dB; fallback to speakers " << fallback << " dB (1 kHz "
              << (fallbackMid - headsetMid) << " dB: the trim); with the bass engine bypassed instead " << bypassed << " dB\n";
    CHECK (headset > 15.0);
    CHECK (fallback <= 3.05);
    CHECK (fallback > 2.0);
    CHECK (std::abs (fallbackMid - headsetMid - static_cast<double> (AudioEngineHost::kSafeSpeakerTrimDb)) < 0.3);
    CHECK (bypassed > 6.0); // the old profile: the EQ shelf and bell stayed

    // The headset returns: the full voicing again.
    backend->outputs = { kStealth, kRealtekSpeakers };
    backend->devicesChanged();
    CHECK (flubapptest::pumpMessagesUntil ([&] { return ! host.isSafeSpeakerProfileActive(); }, 2000));
    CHECK (currentDevice() != nullptr && currentDevice()->getName() == kStealth);
    CHECK (host.getMixEngine().chain (1).getSafeSpeakerBassCapDb() == flub::ProcessingChain::kNoBassCap);
    CHECK (std::abs (bassLiftDb() - headset) < 0.2);
    host.closeDevice();
}

TEST_CASE ("App: E51 'Follow the system default output' plays the default whatever was chosen; picking an output ends it")
{
    AudioEngineHost host;
    auto backendOwner = std::make_unique<FakeBackend>();
    auto* backend = backendOwner.get();
    const juce::String cableIn ("CABLE Input (VB-Audio Virtual Cable)");
    const juce::String headphones ("Headphones (Realtek(R) Audio)");
    backend->outputs = { kStealth, kRealtekSpeakers, cableIn, headphones };
    backend->inputs = { kLineIn };
    backend->defaultOutput = 1;
    host.getDeviceManager().addAudioDeviceType (std::move (backendOwner));
    int endedCalls = 0;
    host.onFollowSystemDefaultChanged = [&] { ++endedCalls; };

    juce::XmlElement saved ("DEVICESETUP");
    saved.setAttribute ("deviceType", "FakeBackend");
    saved.setAttribute ("audioOutputDeviceName", kStealth);
    saved.setAttribute ("audioInputDeviceName", kLineIn);
    host.setFollowSystemDefault (true); // before the device opens, as EngineController does
    REQUIRE (host.openDevice (&saved, 2, 2).isEmpty());
    const auto output = [&host]
    {
        auto* d = host.getDeviceManager().getCurrentAudioDevice();
        return d != nullptr ? d->getName() : juce::String();
    };
    CHECK (output() == kRealtekSpeakers);
    CHECK (host.getChosenOutput().deviceName == kStealth); // the choice is kept
    CHECK (! host.getOutputSelection().fallback);          // following is no fallback: no banner ...
    CHECK (! host.isSafeSpeakerProfileActive());           // ... and no safe speaker profile
    CHECK (host.getOutputSelection().reason == AudioEngineHost::OutputReason::SystemDefault);

    // The default moves to the headset: followed; to the cable: not followed.
    backend->defaultOutput = 0;
    host.reselectOutput();
    CHECK (output() == kStealth);
    backend->defaultOutput = 2;
    host.reselectOutput();
    CHECK (output() == kStealth);
    CHECK (host.getOutputSelection().reason == AudioEngineHost::OutputReason::FirstSafe);

    // Off: back to the choice; on again: the default.
    backend->defaultOutput = 1;
    host.setFollowSystemDefault (false);
    CHECK (output() == kStealth);
    host.setFollowSystemDefault (true);
    CHECK (output() == kRealtekSpeakers);
    CHECK (endedCalls == 0);

    // Picking another output in Settings > Audio: that output from now on.
    auto setup = host.getDeviceManager().getAudioDeviceSetup();
    setup.outputDeviceName = headphones;
    REQUIRE (host.getDeviceManager().setAudioDeviceSetup (setup, true).isEmpty());
    CHECK (flubapptest::pumpMessagesUntil ([&] { return ! host.getFollowSystemDefault(); }, 2000));
    CHECK (endedCalls == 1);
    CHECK (output() == headphones);
    CHECK (host.getChosenOutput().deviceName == headphones);
    host.reselectOutput();
    CHECK (output() == headphones); // a choice is not moved by the default
    host.closeDevice();
}

TEST_CASE ("App: E51 the follow-default setting persists and reaches the host; the preferred output keeps its identity")
{
    const flubapptest::TempFolder temp;
    const auto options = [&temp]
    {
        EngineController::Options o;
        o.openAudioDevice = false;
        o.restoreState = false;
        o.enableAppRouting = false;
        o.settingsFile = temp.file ("settings.xml");
        o.persistSettings = true;
        return o;
    };
    {
        EngineController controller (options());
        CHECK (! controller.getFollowSystemDefaultOutput());
        CHECK (! controller.getHost().getFollowSystemDefault());
        controller.setFollowSystemDefaultOutput (true);
        CHECK (controller.getFollowSystemDefaultOutput());
        CHECK (controller.getHost().getFollowSystemDefault());

        // The host ends following (an output picked in Settings > Audio): stored.
        controller.getHost().setFollowSystemDefault (false);
        REQUIRE (controller.getHost().onFollowSystemDefaultChanged != nullptr);
        controller.getHost().onFollowSystemDefaultChanged();
        CHECK (! controller.getFollowSystemDefaultOutput());
        controller.setFollowSystemDefaultOutput (true);

        // The preferred output with its identity; a name from before E51 reads back alone.
        DeviceEndpointEntry preferred;
        preferred.name = kStealth;
        preferred.endpointId = "{0.0.0.00000000}.{port1}";
        preferred.hardwareId = kStealthHardware;
        controller.getSettings().setPreferredOutput (preferred);
        controller.shutdown();
    }
    {
        EngineController controller (options());
        CHECK (controller.getFollowSystemDefaultOutput());
        CHECK (controller.getHost().getFollowSystemDefault()); // applied before the device would open
        const auto preferred = controller.getSettings().getPreferredOutput();
        CHECK (preferred.name == kStealth);
        CHECK (preferred.endpointId == "{0.0.0.00000000}.{port1}");
        CHECK (preferred.hardwareId == juce::String (kStealthHardware));
        controller.getSettings().setPreferredOutputDevice ("Headphones (Old Name Only)");
        controller.getSettings().setPreferredOutput ({ {}, {}, "Headphones (Old Name Only)", false });
        CHECK (controller.getSettings().getPreferredOutput().endpointId.isEmpty());
        CHECK (controller.getSettings().getPreferredOutputDevice() == "Headphones (Old Name Only)");
        controller.shutdown();
    }
}

TEST_CASE ("App: E51 a correction saved on 'Headset Earphone (Stealth 700 Gen 2)' is applied after a re-plug as '(2- Stealth 700 Gen 2)'")
{
    const flubapptest::TempFolder temp;
    auto endpoints = std::make_shared<std::vector<flub::platform::OutputEndpointIdentity>>();
    *endpoints = { usbEndpoint ("{0.0.0.00000000}.{port1}", kStealth, kStealthHardware),
                   usbEndpoint ("{0.0.0.00000000}.{spk}", kRealtekSpeakers, "HDAUDIO\\FUNC_01&VEN_10EC&DEV_0897") };
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = false;
    o.outputEndpoints = [endpoints] { return *endpoints; };
    EngineController controller (o);
    auto& host = controller.getHost();

    controller.simulateOutputDevice (kStealth, 48000.0, 2);
    juce::String error;
    REQUIRE (controller.importDeviceCorrectionText (kStealthCurve, "Stealth 700 Gen 2 ParametricEQ.txt", error));
    const auto stored = controller.getSettings().getDeviceCorrections();
    REQUIRE (stored.size() == 1);
    CHECK (stored[0].endpoint == kStealth);
    CHECK (stored[0].endpointId == "{0.0.0.00000000}.{port1}");
    CHECK (stored[0].hardwareId == juce::String (kStealthHardware));
    const auto applied = host.getDeviceCorrection();
    CHECK (applied.curve.numFilters == 2);

    // Unplugged: the speakers have no correction (flat).
    *endpoints = { usbEndpoint ("{0.0.0.00000000}.{spk}", kRealtekSpeakers, "HDAUDIO\\FUNC_01&VEN_10EC&DEV_0897") };
    controller.simulateOutputDevice (kRealtekSpeakers, 48000.0, 2);
    CHECK (! controller.getDeviceCorrection().hasCurve);
    CHECK (host.getDeviceCorrection().curve.isEmpty());

    // Re-plugged into another USB port: a new endpoint id and "2- ". Before
    // E51's key the name did not match and the headset played uncorrected.
    endpoints->push_back (usbEndpoint ("{0.0.0.00000000}.{port2}", kStealthPort2, kStealthHardware));
    controller.simulateOutputDevice (kStealthPort2, 48000.0, 2);
    auto info = controller.getDeviceCorrection();
    CHECK (info.hasCurve);
    CHECK (info.name == "Stealth 700 Gen 2 ParametricEQ.txt");
    CHECK (host.getDeviceCorrection() == applied);
    // Switching it off there changes the one entry (now under the new port's id).
    controller.setDeviceCorrectionEnabled (false);
    auto after = controller.getSettings().getDeviceCorrections();
    REQUIRE (after.size() == 1);
    CHECK (! after[0].enabled);
    CHECK (after[0].endpointId == "{0.0.0.00000000}.{port2}");
    controller.setDeviceCorrectionEnabled (true);

    // Another model under the headset's name (another vendor / product) is not the headset.
    *endpoints = { usbEndpoint ("{0.0.0.00000000}.{other}", kStealth, "USB\\VID_1038&PID_12AD") };
    controller.simulateOutputDevice (kStealth, 48000.0, 2);
    CHECK (! controller.getDeviceCorrection().hasCurve);
    CHECK (host.getDeviceCorrection().curve.isEmpty());

    // Without platform ids (Linux, macOS): the name without the instance number.
    endpoints->clear();
    controller.simulateOutputDevice ("Headset Earphone (3- Stealth 700 Gen 2)", 48000.0, 2);
    CHECK (controller.getDeviceCorrection().hasCurve);
    controller.removeDeviceCorrection();
    CHECK (controller.getSettings().getDeviceCorrections().empty());
}

TEST_CASE ("App: E51 device corrections saved before endpoint ids (keyed by the name) load, apply after a re-plug and gain the ids")
{
    const flubapptest::TempFolder temp;
    const auto file = temp.file ("settings.xml");
    {
        // The file as batch 3 and earlier wrote it: <ENDPOINT id="<device name>">.
        juce::PropertiesFile::Options po;
        po.storageFormat = juce::PropertiesFile::storeAsXML;
        po.millisecondsBeforeSaving = -1;
        juce::PropertiesFile legacy (file, po);
        juce::XmlElement xml ("CORRECTIONS");
        auto* e = xml.createNewChildElement ("ENDPOINT");
        e->setAttribute ("id", kStealth);
        e->setAttribute ("name", "old.txt");
        e->setAttribute ("enabled", true);
        e->addTextElement (kStealthCurve);
        legacy.setValue ("device.corrections", &xml);
        REQUIRE (legacy.save());
    }
    auto endpoints = std::make_shared<std::vector<flub::platform::OutputEndpointIdentity>>();
    *endpoints = { usbEndpoint ("{0.0.0.00000000}.{port2}", kStealthPort2, kStealthHardware) };
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = file;
    o.persistSettings = false;
    o.outputEndpoints = [endpoints] { return *endpoints; };
    EngineController controller (o);

    auto entries = controller.getSettings().getDeviceCorrections();
    REQUIRE (entries.size() == 1);
    CHECK (entries[0].endpoint == kStealth);
    CHECK (entries[0].endpointId.isEmpty());

    controller.simulateOutputDevice (kStealthPort2, 48000.0, 2);
    const auto info = controller.getDeviceCorrection();
    CHECK (info.hasCurve);
    CHECK (info.name == "old.txt");
    CHECK (controller.getHost().getDeviceCorrection().curve.numFilters == 2);
    entries = controller.getSettings().getDeviceCorrections();
    REQUIRE (entries.size() == 1);
    CHECK (entries[0].endpointId == "{0.0.0.00000000}.{port2}"); // migrated in place, not duplicated
    CHECK (entries[0].hardwareId == juce::String (kStealthHardware));
    CHECK (entries[0].curveText.trim() == juce::String (kStealthCurve).trim());
}
