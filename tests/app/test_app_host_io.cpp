// App-level tests: what AudioEngineHost does with the device's and the
// captures' channels, and how it times its callbacks.
//   * docs/11 E27 step 4: the per-channel tone test. A fake device of JUCE
//     type "ALSA" (and, for contrast, "JACK") with 8 inputs feeding the 7.1
//     Game strip; a tone on one device input at a time, and the Game chain's
//     ActiveChannelDetector (MeterBus::activeChannelMask) names the engine
//     channel it reached. ALSA delivers FL FR RL RR FC LFE SL SR; the engine
//     wants FL FR FC LFE BL BR SL SR.
//   * docs/11 E01: a 7.1 capture read by a stereo strip is folded like the
//     chain folds (BS.775, the LFE at virt.lfe), not cut to FL / FR.
//   * docs/11 E45: the callback's duration / interval histograms, with host
//     timestamps from the callback context so the intervals are exact.
#include "AppTestSupport.h"

#include "engine/AudioEngineHost.h"

#include "flub/engine/Parameters.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

using namespace flub::app;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr int kInputs = 8;
constexpr double kPi = 3.14159265358979323846;

class FakeAudioDevice final : public juce::AudioIODevice
{
public:
    explicit FakeAudioDevice (const juce::String& deviceType) : juce::AudioIODevice ("Fake 7.1", deviceType) {}

    juce::StringArray getOutputChannelNames() override { return { "Left", "Right" }; }
    juce::StringArray getInputChannelNames() override
    {
        juce::StringArray names;
        for (int i = 0; i < kInputs; ++i)
            names.add ("In " + juce::String (i + 1));
        return names;
    }
    juce::Array<double> getAvailableSampleRates() override { return { kRate }; }
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
    double getCurrentSampleRate() override { return kRate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return juce::BigInteger (0x3); }
    juce::BigInteger getActiveInputChannels() const override { return juce::BigInteger ((1 << kInputs) - 1); }
    int getOutputLatencyInSamples() override { return kBlock; }
    int getInputLatencyInSamples() override { return kBlock; }
};

/** Drives the host's callback block by block on this thread. */
struct DeviceDriver
{
    std::array<std::vector<float>, kInputs> in;
    std::array<std::vector<float>, 2> out;
    std::array<const float*, kInputs> inPtr {};
    std::array<float*, 2> outPtr {};

    DeviceDriver()
    {
        for (int c = 0; c < kInputs; ++c)
        {
            in[static_cast<size_t> (c)].assign (kBlock, 0.0f);
            inPtr[static_cast<size_t> (c)] = in[static_cast<size_t> (c)].data();
        }
        for (int c = 0; c < 2; ++c)
        {
            out[static_cast<size_t> (c)].assign (kBlock, 0.0f);
            outPtr[static_cast<size_t> (c)] = out[static_cast<size_t> (c)].data();
        }
    }

    void run (AudioEngineHost& host, const juce::AudioIODeviceCallbackContext& context = {})
    {
        host.audioDeviceIOCallbackWithContext (inPtr.data(), kInputs, outPtr.data(), 2, kBlock, context);
    }
};

/** Stands in for a platform::ProcessLoopbackCapture: the test delivers
    interleaved 8-channel packets itself. Channel `channel` (and `channel2`
    when >= 0) carries a sine. */
class ToneCapture final : public flub::platform::ProcessLoopbackCapture
{
public:
    bool isSupported() const override { return true; }
    bool start (uint32_t, bool, double, int numChannels, FrameCallback frameCallback, std::string&) override
    {
        channels = numChannels;
        callback = std::move (frameCallback);
        running = true;
        return true;
    }
    void stop() override { running = false; }
    bool isRunning() const override { return running; }

    int channel = 0, channel2 = -1;
    double hz = 50.0;
    float amplitude = 0.25f;

    void deliver (int numFrames)
    {
        packet.assign (static_cast<size_t> (numFrames * channels), 0.0f);
        for (int i = 0; i < numFrames; ++i, ++frame)
        {
            const float v = amplitude * static_cast<float> (std::sin (2.0 * kPi * hz * static_cast<double> (frame) / kRate));
            packet[static_cast<size_t> (i * channels + channel)] = v;
            if (channel2 >= 0)
                packet[static_cast<size_t> (i * channels + channel2)] = v;
        }
        if (running && callback != nullptr)
            callback (packet.data(), numFrames, channels);
    }

private:
    FrameCallback callback;
    std::vector<float> packet;
    int channels = 2;
    int64_t frame = 0;
    bool running = false;
};

/** Bypassed, level-neutral chain: the strip's output is its folded input
    (virtualiser off: the BS.775 fold). */
void makeTransparent (flub::MixEngine& engine, int strip, flub::param::InputModeValue inputMode)
{
    auto& p = engine.params (strip);
    p.set (flub::param::BypassAll, 1.0f);
    p.set (flub::param::LoudnessMatchBypass, 0.0f);
    p.set (flub::param::VirtualizerOn, 0.0f);
    p.set (flub::param::VirtInputMode, static_cast<float> (inputMode));
}

double dbOf (double rms) { return 20.0 * std::log10 (std::max (rms, 1.0e-12)); }
} // namespace

TEST_CASE ("App: per-channel tone test - an ALSA device's 7.1 order reaches the Game strip as FL FR FC LFE BL BR SL SR (E27)")
{
    // ALSA order of the device's channels, and where each must land in the
    // engine (FL FR FC LFE BL BR SL SR = 0..7).
    struct Speaker
    {
        const char* name;
        int engineChannel;
    };
    constexpr std::array<Speaker, kInputs> alsa { { { "FL", 0 }, { "FR", 1 }, { "RL", 4 }, { "RR", 5 }, { "FC", 2 }, { "LFE", 3 }, { "SL", 6 }, { "SR", 7 } } };

    for (const char* type : { "ALSA", "JACK" })
    {
        const bool isAlsa = juce::String (type) == "ALSA";
        AudioEngineHost host;
        host.setDeviceInputRouting (0, 0);
        FakeAudioDevice device (type);
        DeviceDriver driver;
        int identified = 0;
        for (int k = 0; k < kInputs; ++k)
        {
            // A fresh engine per channel: the detector starts empty.
            host.audioDeviceStopped();
            host.audioDeviceAboutToStart (&device);
            const int expected = isAlsa ? alsa[static_cast<size_t> (k)].engineChannel : k;

            int64_t frame = 0;
            for (int b = 0; b < 20; ++b) // 107 ms of a -20 dBFS 1 kHz tone on device input k only
            {
                for (int c = 0; c < kInputs; ++c)
                    for (int i = 0; i < kBlock; ++i)
                        driver.in[static_cast<size_t> (c)][static_cast<size_t> (i)] =
                            c == k ? 0.1f * static_cast<float> (std::sin (2.0 * kPi * 1000.0 * static_cast<double> (frame + i) / kRate)) : 0.0f;
                frame += kBlock;
                driver.run (host);
            }
            const uint32_t mask = host.getMixEngine().chain (0).meters().activeChannelMask.load();
            if (mask != (1u << expected))
                std::cout << "    " << type << " device input " << k << " (" << (isAlsa ? alsa[static_cast<size_t> (k)].name : "?")
                          << ") reached engine mask 0x" << std::hex << mask << std::dec << ", expected channel " << expected << "\n";
            CHECK (mask == (1u << expected));
            identified += mask == (1u << expected) ? 1 : 0;
        }
        CHECK (identified == kInputs);
    }

    // The permutation itself: ALSA 5.1 and 7.1 strips, nothing else.
    const auto a8 = AudioEngineHost::deviceInputOrder ("ALSA", 8);
    CHECK ((a8[0] == 0 && a8[1] == 1 && a8[2] == 4 && a8[3] == 5 && a8[4] == 2 && a8[5] == 3 && a8[6] == 6 && a8[7] == 7));
    const auto a6 = AudioEngineHost::deviceInputOrder ("ALSA HW", 6);
    CHECK ((a6[0] == 0 && a6[1] == 1 && a6[2] == 4 && a6[3] == 5 && a6[4] == 2 && a6[5] == 3));
    for (const auto& [typeName, channels] : { std::pair<const char*, int> { "ALSA", 2 }, { "JACK", 8 }, { "Windows Audio", 8 }, { "CoreAudio", 6 } })
    {
        const auto order = AudioEngineHost::deviceInputOrder (typeName, channels);
        bool identity = true;
        for (int c = 0; c < flub::kMaxChannels; ++c)
            identity = identity && order[static_cast<size_t> (c)] == c;
        CHECK (identity);
    }
}

TEST_CASE ("App: a 7.1 capture moved to a stereo strip is folded like the chain folds, the LFE at virt.lfe (E01)")
{
    // Output level (dB, L / R) of one steady tone on one capture channel (or
    // two), read by the Game strip (the chain's own 7.1 fold) or moved to the
    // Music strip (stereo: the host folds it), over the last 0.3 s of the run.
    using flub::param::InputModeValue;
    const auto measure = [] (int channel, int channel2, double hz, bool moveToMusic,
                             InputModeValue inputMode = InputModeValue::ForceSurround, int blocks = 150) -> std::array<double, 2>
    {
        AudioEngineHost host;
        ToneCapture* capture = nullptr;
        host.setCaptureFactory ([&capture]
                                {
                                    auto c = std::make_unique<ToneCapture>();
                                    capture = c.get();
                                    return c;
                                });
        makeTransparent (host.getMixEngine(), 0, inputMode);
        makeTransparent (host.getMixEngine(), 1, inputMode);
        FakeAudioDevice device ("Fake");
        host.audioDeviceAboutToStart (&device);

        juce::String error;
        const int id = host.startProcessCapture (0, 4242, error); // Game: an 8-channel FIFO
        REQUIRE (id >= 0);
        REQUIRE (capture != nullptr);
        if (moveToMusic)
            host.setCaptureStrip (id, 1);
        capture->channel = channel;
        capture->channel2 = channel2;
        capture->hz = hz;

        DeviceDriver driver;
        double sum[2] = { 0.0, 0.0 };
        int counted = 0;
        int64_t allocations = 0, locks = 0;
        for (int b = 0; b < blocks; ++b)
        {
            capture->deliver (kBlock);
            {
                // The fold runs on the audio thread (the first block promotes
                // the thread, which may allocate once: not probed).
                flubapptest::RealtimeProbe probe;
                driver.run (host);
                if (b > 0)
                {
                    allocations += probe.allocations() + probe.deallocations();
                    locks += probe.locks();
                }
            }
            if (b >= blocks - 56)
            {
                for (int c = 0; c < 2; ++c)
                    for (const float v : driver.out[static_cast<size_t> (c)])
                        sum[c] += static_cast<double> (v) * v;
                counted += kBlock;
            }
        }
        CHECK (allocations == 0);
        CHECK (locks == 0);
        return { dbOf (std::sqrt (sum[0] / counted)), dbOf (std::sqrt (sum[1] / counted)) };
    };

    const double source = dbOf (0.25 / std::sqrt (2.0)); // the capture's tone
    const auto chainFl = measure (0, -1, 50.0, false), chainLfe = measure (3, -1, 50.0, false);
    const auto hostFl = measure (0, -1, 50.0, true), hostLfe = measure (3, -1, 50.0, true);
    const auto hostFc = measure (2, -1, 1000.0, true), hostSl = measure (6, -1, 1000.0, true);
    // FL / FR-only content with the input fold on Auto, 3.2 s: the detector
    // switches both folds to the stereo passthrough after 2 s.
    const auto chainStereo = measure (0, 1, 1000.0, false, InputModeValue::Auto, 600);
    const auto hostStereo = measure (0, 1, 1000.0, true, InputModeValue::Auto, 600);
    std::cout << "    re the source tone (L / R dB): chain fold FL " << chainFl[0] - source << ", LFE " << chainLfe[0] - source
              << ", FL + FR on Auto " << chainStereo[0] - source << "; host fold FL " << hostFl[0] - source << " / " << hostFl[1] - source
              << ", LFE " << hostLfe[0] - source << " / " << hostLfe[1] - source << ", FC " << hostFc[0] - source << " / "
              << hostFc[1] - source << ", SL " << hostSl[0] - source << " / " << hostSl[1] - source << ", FL + FR on Auto "
              << hostStereo[0] - source << " / " << hostStereo[1] - source << "\n";

    // FL at the surround fold's -3 dB, as in the chain (before: 0 dB, the
    // moved capture kept FL / FR as they were).
    CHECK_NEAR (hostFl[0] - source, -3.01, 0.1);
    CHECK (hostFl[1] < -100.0);
    // The LFE: dropped before (FL / FR only: silence), now +6 dB (virt.lfe's
    // default) re one main on both sides, as in the chain's fold (E01
    // Done-when: the LFE-to-mains ratio within 1 dB in every fold).
    CHECK_NEAR (hostLfe[0] - hostFl[0], 6.0, 0.1);
    CHECK_NEAR (hostLfe[1] - hostFl[0], 6.0, 0.1);
    CHECK_NEAR (hostLfe[0] - hostFl[0], chainLfe[0] - chainFl[0], 0.1);
    CHECK_NEAR (hostFl[0], chainFl[0], 0.1);
    // Centre (dialogue) and surrounds: silent before, now k x k = -6.02 dB
    // on both sides (FC) and on their own side (SL).
    CHECK_NEAR (hostFc[0] - source, -6.02, 0.1);
    CHECK_NEAR (hostFc[1] - source, -6.02, 0.1);
    CHECK_NEAR (hostSl[0] - source, -6.02, 0.1);
    CHECK (hostSl[1] < -100.0);
    // FL / FR-only content (a stereo game in the 8-channel container) comes
    // out at unity once detected, as from the chain's passthrough fold
    // (before: unity too).
    CHECK_NEAR (hostStereo[0] - source, 0.0, 0.1);
    CHECK_NEAR (hostStereo[1] - source, 0.0, 0.1);
    CHECK_NEAR (hostStereo[0], chainStereo[0], 0.1);
}

TEST_CASE ("App: the device callback's duration and interval histograms, from host timestamps (E45)")
{
    AudioEngineHost host;
    FakeAudioDevice device ("Fake");
    host.audioDeviceAboutToStart (&device);
    DeviceDriver driver;
    const auto periodNs = static_cast<uint64_t> (1.0e9 * kBlock / kRate); // 5.333 ms

    const auto before = host.getCallbackTiming();
    CHECK (before.callbacks == 0);

    uint64_t hostTime = 7'000'000'000u;
    juce::AudioIODeviceCallbackContext context;
    context.hostTimeNs = &hostTime;
    for (int i = 0; i < 100; ++i, hostTime += periodNs)
    {
        if (i == 60)
            hostTime += 2 * periodNs; // the backend stalls for two periods: one late callback
        driver.run (host, context);
    }

    const auto timing = host.getStatus().callbackTiming; // what EngineController::getStatus() hands on
    CHECK (timing.callbacks == 100);
    CHECK (timing.periodNs == periodNs);
    CHECK (timing.duration.count() == 100);
    CHECK (timing.duration.maxNs > 0);
    CHECK (timing.interval.count() == 99); // the first callback after the start has none
    CHECK (timing.late == 1);
    CHECK_GE (static_cast<double> (timing.interval.maxNs), 3.0 * static_cast<double> (periodNs));
    CHECK_NEAR (timing.interval.percentileNs (0.5), static_cast<double> (periodNs), 0.09 * static_cast<double> (periodNs));
    CHECK (host.getStatus().callbacks == 100);

    // A device restart is not a gap, and a backend that stops giving host
    // times starts a new run on the steady clock.
    host.audioDeviceStopped();
    host.audioDeviceAboutToStart (&device);
    hostTime += 1000u * periodNs;
    driver.run (host, context);
    driver.run (host); // no host time from here on
    driver.run (host);
    const auto after = host.getCallbackTiming().since (timing);
    CHECK (after.callbacks == 3);
    CHECK (after.late == 0);
    CHECK (after.interval.count() == 1); // steady clock, callback 2 -> 3
}
