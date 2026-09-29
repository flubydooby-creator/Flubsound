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
//   * docs/11 E27 step 4 remainder: the round-trip channel check. Device
//     channel maps from the backend (an injected ALSA chmap) and from the
//     channel names (PipeWire-JACK ports), in several orders; every speaker
//     must reach its own engine channel.
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
    /** 'inputNames' empty: "In 1" ... "In 8" (no speaker positions). */
    explicit FakeAudioDevice (const juce::String& deviceType, juce::StringArray inputNames = {})
        : juce::AudioIODevice ("Fake 7.1", deviceType), names (std::move (inputNames))
    {
        for (int i = names.size(); i < kInputs; ++i)
            names.add ("In " + juce::String (i + 1));
    }

    juce::StringArray getOutputChannelNames() override { return { "Left", "Right" }; }
    juce::StringArray getInputChannelNames() override { return names; }
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

private:
    juce::StringArray names;
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

namespace
{
using flub::platform::SpeakerPosition;

/** The engine channel of each speaker position in a 7.1 strip. */
int engineChannelOf (SpeakerPosition p)
{
    switch (p)
    {
        case SpeakerPosition::FL:  return 0;
        case SpeakerPosition::FR:  return 1;
        case SpeakerPosition::FC:  return 2;
        case SpeakerPosition::LFE: return 3;
        case SpeakerPosition::RL:  return 4;
        case SpeakerPosition::RR:  return 5;
        case SpeakerPosition::SL:  return 6;
        case SpeakerPosition::SR:  return 7;
        case SpeakerPosition::Unknown: break;
    }
    return -1;
}

/** The round-trip channel check: a -20 dBFS 1 kHz tone on one device input
    at a time, the speaker it carries named by `carried`; the Game chain's
    ActiveChannelDetector names the engine channel it reached. Returns how
    many of the 8 reached their speaker's engine channel. */
int roundTrip (FakeAudioDevice& device, const std::array<SpeakerPosition, kInputs>& carried, AudioEngineHost::ChannelMapQuery query,
               InputChannelMap expectedSource)
{
    AudioEngineHost host;
    host.setChannelMapQuery (std::move (query));
    host.setDeviceInputRouting (0, 0);
    DeviceDriver driver;
    int identified = 0;
    for (int k = 0; k < kInputs; ++k)
    {
        host.audioDeviceStopped();
        host.audioDeviceAboutToStart (&device);
        CHECK (host.getStatus().inputChannelMap == expectedSource);
        int64_t frame = 0;
        for (int b = 0; b < 20; ++b)
        {
            for (int c = 0; c < kInputs; ++c)
                for (int i = 0; i < kBlock; ++i)
                    driver.in[static_cast<size_t> (c)][static_cast<size_t> (i)] =
                        c == k ? 0.1f * static_cast<float> (std::sin (2.0 * kPi * 1000.0 * static_cast<double> (frame + i) / kRate)) : 0.0f;
            frame += kBlock;
            driver.run (host);
        }
        const uint32_t mask = host.getMixEngine().chain (0).meters().activeChannelMask.load();
        const int expected = engineChannelOf (carried[static_cast<size_t> (k)]);
        if (mask != (1u << expected))
            std::cout << "    " << device.getTypeName() << " device input " << k << " reached engine mask 0x" << std::hex << mask << std::dec
                      << ", expected channel " << expected << "\n";
        identified += mask == (1u << expected) ? 1 : 0;
    }
    return identified;
}
} // namespace

TEST_CASE ("App: round-trip channel check - the backend's channel map or the channel names put every speaker in its engine channel (E27)")
{
    using P = SpeakerPosition;
    constexpr std::array<P, kInputs> alsaOrder { P::FL, P::FR, P::RL, P::RR, P::FC, P::LFE, P::SL, P::SR };
    constexpr std::array<P, kInputs> waveOrder { P::FL, P::FR, P::FC, P::LFE, P::RL, P::RR, P::SL, P::SR };
    constexpr std::array<P, kInputs> scrambled { P::SR, P::LFE, P::FL, P::RR, P::SL, P::FC, P::RL, P::FR };
    const auto names = [] (const char* prefix, const std::array<P, kInputs>& order)
    {
        static const char* const shortNames[] = { "?", "FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR" };
        juce::StringArray result;
        for (const auto p : order)
            result.add (juce::String (prefix) + shortNames[static_cast<int> (p)]);
        return result;
    };
    const auto mapOf = [] (const std::array<P, kInputs>& order)
    {
        return [order] (const juce::String&, const juce::String&, int channels)
        { return channels == kInputs ? std::vector<P> (order.begin(), order.end()) : std::vector<P>(); };
    };
    const AudioEngineHost::ChannelMapQuery none = [] (const juce::String&, const juce::String&, int) { return std::vector<P>(); };

    // PipeWire-JACK: the device's channels are the ports of the client chosen
    // as input, e.g. the Flubsound Game sink's monitor ports. In the sink's
    // own (engine) order, in ALSA's order (a sink made with ALSA's 7.1 map),
    // scrambled. Before (identity for JACK): 8, 4 and 0 of 8 identified.
    FakeAudioDevice jackWave ("JACK", names ("Flubsound Game:monitor_", waveOrder));
    FakeAudioDevice jackAlsa ("JACK", names ("Flubsound Game:monitor_", alsaOrder));
    FakeAudioDevice jackScrambled ("JACK", names ("capture_", scrambled));
    CHECK (roundTrip (jackWave, waveOrder, none, InputChannelMap::ChannelNames) == kInputs);
    CHECK (roundTrip (jackAlsa, alsaOrder, none, InputChannelMap::ChannelNames) == kInputs);
    CHECK (roundTrip (jackScrambled, scrambled, none, InputChannelMap::ChannelNames) == kInputs);

    // ALSA card PCMs report their capture map (chmap). A USB audio class
    // device in WAVE order was permuted as if it delivered ALSA's order
    // before (4 of 8); now the map is followed. ALSA's order from a map and
    // a scrambled map come out right too.
    FakeAudioDevice hw ("ALSA HW"), pcm ("ALSA");
    CHECK (roundTrip (hw, waveOrder, mapOf (waveOrder), InputChannelMap::Backend) == kInputs);
    CHECK (roundTrip (pcm, alsaOrder, mapOf (alsaOrder), InputChannelMap::Backend) == kInputs);
    CHECK (roundTrip (hw, scrambled, mapOf (scrambled), InputChannelMap::Backend) == kInputs);
    // A map wins over names that say otherwise.
    FakeAudioDevice hwNamed ("ALSA HW", names ("", alsaOrder));
    CHECK (roundTrip (hwNamed, waveOrder, mapOf (waveOrder), InputChannelMap::Backend) == kInputs);

    // No map and no names (plug-in PCMs: default, pipewire, pulse): ALSA's
    // default order, as before.
    CHECK (roundTrip (pcm, alsaOrder, none, InputChannelMap::None) == kInputs);
    CHECK (roundTrip (pcm, alsaOrder, nullptr, InputChannelMap::None) == kInputs);

    // The query gets the device's type, name and channel count.
    juce::String askedType, askedName;
    int askedChannels = 0;
    AudioEngineHost host;
    host.setChannelMapQuery ([&] (const juce::String& type, const juce::String& name, int channels)
                             {
                                 askedType = type;
                                 askedName = name;
                                 askedChannels = channels;
                                 return std::vector<P>();
                             });
    host.audioDeviceAboutToStart (&hw);
    CHECK (askedType == "ALSA HW");
    CHECK (askedName == "Fake 7.1");
    CHECK (askedChannels == kInputs);
    host.audioDeviceStopped();
}

TEST_CASE ("App: speaker positions from channel names, and the engine order from positions (E27)")
{
    using P = SpeakerPosition;
    CHECK (AudioEngineHost::positionFromChannelName ("Flubsound Game:monitor_FL") == P::FL);
    CHECK (AudioEngineHost::positionFromChannelName ("capture_LFE") == P::LFE);
    CHECK (AudioEngineHost::positionFromChannelName ("playback_RR") == P::RR);
    CHECK (AudioEngineHost::positionFromChannelName ("BL") == P::RL);
    CHECK (AudioEngineHost::positionFromChannelName ("front-center") == P::FC);
    CHECK (AudioEngineHost::positionFromChannelName ("Side Right") == P::SR);
    CHECK (AudioEngineHost::positionFromChannelName ("rear_left") == P::RL);
    CHECK (AudioEngineHost::positionFromChannelName ("Center") == P::FC);
    for (const char* name : { "channel 1", "In 3", "in_3", "system:capture_1", "monitor_AUX0", "MONO", "", "Left" })
        CHECK (AudioEngineHost::positionFromChannelName (name) == P::Unknown);

    std::array<int, flub::kMaxChannels> order {};
    const P fiveOneRear[] = { P::FL, P::FR, P::RL, P::RR, P::FC, P::LFE };
    REQUIRE (AudioEngineHost::orderFromPositions (fiveOneRear, 6, order));
    CHECK ((order[0] == 0 && order[1] == 1 && order[2] == 4 && order[3] == 5 && order[4] == 2 && order[5] == 3));
    // The same as the fixed ALSA order the host used before for 5.1.
    const auto fixed = AudioEngineHost::deviceInputOrder ("ALSA", 6);
    CHECK (std::equal (fixed.begin(), fixed.begin() + 6, order.begin()));
    const P fiveOneSide[] = { P::FL, P::FR, P::FC, P::LFE, P::SL, P::SR };
    REQUIRE (AudioEngineHost::orderFromPositions (fiveOneSide, 6, order));
    CHECK ((order[0] == 0 && order[4] == 4 && order[5] == 5));
    const P swapped[] = { P::FR, P::FL };
    REQUIRE (AudioEngineHost::orderFromPositions (swapped, 2, order));
    CHECK ((order[0] == 1 && order[1] == 0));

    // Not the engine's layout: no order (the fallback applies).
    const P ambiguous[] = { P::FL, P::FR, P::FC, P::LFE, P::SL, P::RL };        // two left surrounds
    const P repeated[] = { P::FL, P::FL };
    const P partial[] = { P::FL, P::FR, P::FC, P::LFE, P::Unknown, P::RR, P::SL, P::SR };
    const P quad[] = { P::FL, P::FR, P::RL, P::RR };
    CHECK (! AudioEngineHost::orderFromPositions (ambiguous, 6, order));
    CHECK (! AudioEngineHost::orderFromPositions (repeated, 2, order));
    CHECK (! AudioEngineHost::orderFromPositions (partial, 8, order));
    CHECK (! AudioEngineHost::orderFromPositions (quad, 4, order));
    CHECK (! AudioEngineHost::orderFromPositions (nullptr, 2, order));
}
