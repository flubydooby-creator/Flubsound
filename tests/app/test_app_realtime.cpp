// App-level tests: the device callback path of AudioEngineHost is real-time
// safe (R1.5). The host is started with a fake juce::AudioIODevice (48 kHz /
// 256 samples) exactly as JUCE starts it, then the callback is driven block
// by block from a separate "device" thread with 8 device inputs feeding the
// 7.1 Game strip, a fake per-app capture feeding the Music strip through its
// DriftCompensatedFifo (including an underrun and re-prime), strip gain /
// mute / master-ceiling and parameter changes, the UI draining the analyser
// taps through AnalyzerFeed between blocks, and a crossfaded engine swap
// (latency profile Balanced -> Quality, reconfigured by the message thread
// between two probed blocks; the whole swap runs inside the probed blocks).
// Every processed block is wrapped in a RealtimeProbe: no allocation, no free
// and (Linux) no mutex. A second case runs the loopback guard (docs/11 E51:
// ramp to silence, frozen engine, ramp back) under the same probe.
#include "AppTestSupport.h"

#include "engine/AudioEngineHost.h"
#include "ui/AnalyzerFeed.h"
#include "ui/MeterSnapshot.h"

#include "flub/engine/Parameters.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

using namespace flub::app;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr int kInputs = 8;

class FakeAudioDevice final : public juce::AudioIODevice
{
public:
    FakeAudioDevice() : juce::AudioIODevice ("Fake Output", "Fake") {}

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

/** Stands in for a platform::ProcessLoopbackCapture: the test delivers the
    packets itself (deliver()) instead of an OS capture thread. */
class FakeCapture final : public flub::platform::ProcessLoopbackCapture
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

    /** One interleaved packet of a sine (the capture side of the FIFO). */
    void deliver (int numFrames)
    {
        packet.resize (static_cast<size_t> (numFrames * channels));
        for (int i = 0; i < numFrames; ++i, ++frame)
            for (int c = 0; c < channels; ++c)
                packet[static_cast<size_t> (i * channels + c)] =
                    0.3f * static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * 330.0 * static_cast<double> (frame) / kRate));
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

// Escapes a pointer so the optimiser cannot elide a new / delete pair.
void* volatile escapedPointer = nullptr;

struct BlockTotals
{
    int64_t allocations = 0, deallocations = 0, locks = 0;
    int blocks = 0;
    float outputPeak = 0.0f;
    bool finite = true;
};
} // namespace

TEST_CASE ("App: test harness counts allocations, frees and mutex locks on the probed thread")
{
    // The detectors used below must not be vacuous.
    flubapptest::RealtimeProbe probe;
    {
        std::vector<float> v (64);
        escapedPointer = v.data();
    }
    CHECK (probe.allocations() == 1);
    CHECK (probe.deallocations() == 1);

    if (flubapptest::lockCountingAvailable())
    {
        std::mutex m;
        juce::CriticalSection cs;
        const auto before = probe.locks();
        {
            const std::lock_guard<std::mutex> lock (m);
        }
        {
            const juce::ScopedLock lock (cs);
        }
        CHECK (probe.locks() - before >= 2);
    }

    // Another thread's allocations are not counted here (only std::thread's
    // own bookkeeping, allocated on this thread).
    flubapptest::RealtimeProbe mine;
    std::thread other ([]
                       {
                           for (int i = 0; i < 100; ++i)
                           {
                               std::vector<int> v (16);
                               escapedPointer = v.data();
                           }
                       });
    other.join();
    CHECK (mine.allocations() < 10);
}

TEST_CASE ("App: AudioEngineHost device callback allocates, frees and locks nothing per block (48 kHz / 256)")
{
    AudioEngineHost host;
    FakeCapture* capture = nullptr;
    host.setCaptureFactory ([&capture]
                            {
                                auto c = std::make_unique<FakeCapture>();
                                capture = c.get();
                                return c;
                            });

    // Device inputs 0..7 feed the Game strip (7.1); JUCE calls this on the
    // message thread when the callback is attached, which configures the
    // engine for the device format synchronously.
    host.setDeviceInputRouting (0, 0);
    FakeAudioDevice device;
    host.audioDeviceAboutToStart (&device);
    REQUIRE (host.getSampleRate() == kRate);
    REQUIRE (host.getBlockSize() == kBlock);

    juce::String error;
    const int captureId = host.startProcessCapture (1, 4242, error); // Music strip
    REQUIRE (captureId >= 0);
    REQUIRE (capture != nullptr);

    // The device thread re-fetches the engine after the swap (the UI does so
    // when getStructureGeneration() changes).
    std::atomic<flub::MixEngine*> engine { &host.getMixEngine() };
    engine.load()->params (0).set (flub::param::Mode, 1.0f); // Game strip: Gaming mode (virtualiser etc.)
    engine.load()->params (1).set (flub::param::BoostIntensity, 0.6f);

    // Device-side buffers (what a backend hands to the callback).
    std::array<std::vector<float>, kInputs> inputData;
    std::array<const float*, kInputs> inputs {};
    for (size_t c = 0; c < inputData.size(); ++c)
    {
        inputData[c].assign (kBlock, 0.0f);
        inputs[c] = inputData[c].data();
    }
    std::array<std::vector<float>, 2> outputData { std::vector<float> (kBlock), std::vector<float> (kBlock) };
    std::array<float*, 2> outputs { outputData[0].data(), outputData[1].data() };

    ui::AnalyzerFeed feed;
    int64_t tapSamples = 0;
    feed.addSink ([&tapSamples] (ui::AnalyzerFeed::Stream, const float*, int n) { tapSamples += n; });

    constexpr int kWarmUpBlocks = 8, kBlocks = 600, kSwapBlock = 470;
    BlockTotals totals;

    // Swap hand-shake: the device thread stops before block kSwapBlock (1),
    // this thread (the message thread) publishes the new engine (2).
    std::atomic<int> swapStage { 0 };
    const auto waitForStage = [&swapStage] (int stage)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30); // hang guard only
        while (swapStage.load() != stage && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        return swapStage.load() == stage;
    };
    bool handshakeOk = true;

    // The "device thread": the backend's audio thread calls the callback.
    std::thread deviceThread ([&]
    {
        const juce::AudioIODeviceCallbackContext context {};
        int64_t sample = 0, capturedFrames = 0;

        for (int b = 0; b < kWarmUpBlocks + kBlocks; ++b)
        {
            // ---- Outside the probe: producers and control changes -----------
            for (size_t c = 0; c < inputData.size(); ++c)
                for (int i = 0; i < kBlock; ++i)
                    inputData[c][static_cast<size_t> (i)] =
                        0.25f * static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * (110.0 + 55.0 * static_cast<double> (c)) * static_cast<double> (sample + i) / kRate));
            sample += kBlock;

            // 10 ms capture packets, paced by the device clock, with a 40-block
            // gap (FIFO underrun, then re-prime + fade-in).
            const bool gap = b >= 300 && b < 340;
            while (! gap && capturedFrames < sample + 2 * kBlock)
            {
                capture->deliver (480);
                capturedFrames += 480;
            }
            if (gap)
                capturedFrames = sample;

            if (b % 50 == 10)
                host.setStripGainDb (2, (b / 50) % 2 == 0 ? -6.0f : 0.0f);
            if (b == 200)
                host.setStripMuted (3, true);
            if (b == 250)
                host.setStripMuted (3, false);
            if (b == 400)
                host.setMasterCeilingDb (-2.0f);
            if (b % 64 == 0)
                engine.load()->params (1).set (flub::param::BoostIntensity, (b / 64) % 2 == 0 ? 0.3f : 0.8f);
            if (b == kSwapBlock)
            {
                swapStage.store (1);
                handshakeOk = waitForStage (2) && handshakeOk;
            }

            // ---- The callback, probed (not during warm-up: the first block on
            //      a new device thread promotes it, which may allocate once) --
            flubapptest::RealtimeProbe probe;
            host.audioDeviceIOCallbackWithContext (inputs.data(), kInputs, outputs.data(), 2, kBlock, context);
            if (b >= kWarmUpBlocks)
            {
                totals.allocations += probe.allocations();
                totals.deallocations += probe.deallocations();
                totals.locks += probe.locks();
                ++totals.blocks;
                for (const auto& ch : outputData)
                    for (const float v : ch)
                    {
                        totals.finite = totals.finite && std::isfinite (v);
                        totals.outputPeak = std::max (totals.outputPeak, std::abs (v));
                    }
            }

            // The UI drains the selected strip's taps (normally the message
            // thread; the rings are SPSC, one consumer at a time).
            if (b % 4 == 3)
                feed.pull (engine.load()->chain (1).taps());
        }
    });

    // The message thread: a latency-profile change on every strip, applied by
    // the crossfaded swap while the device thread keeps calling back.
    handshakeOk = waitForStage (1) && handshakeOk;
    for (int s = 0; s < host.getNumStrips(); ++s)
        host.getMixEngine().params (s).set (flub::param::LatencyProfile, static_cast<float> (flub::param::LatencyProfileValue::Quality));
    host.reconfigure();
    CHECK (host.isSwapInProgress());
    engine.store (&host.getMixEngine());
    swapStage.store (2);
    deviceThread.join();
    CHECK (handshakeOk);
    CHECK (host.getCompletedSwaps() == 1); // the whole swap ran inside the probed blocks
    CHECK (! host.isSwapInProgress());

    const auto captures = host.getCaptures();
    REQUIRE (captures.size() == 1);
    CHECK (captures[0].stats.underruns >= 1); // the gap ran the underrun / re-prime path
    CHECK (captures[0].stats.streaming);      // ... and the FIFO recovered

    host.audioDeviceStopped();
    host.stopAllCaptures();

    CHECK (totals.blocks == kBlocks);
    CHECK (totals.allocations == 0);
    CHECK (totals.deallocations == 0);
    CHECK (totals.locks == 0);
    if (totals.allocations != 0 || totals.deallocations != 0 || totals.locks != 0)
        std::cerr << "    per-block violations: " << totals.allocations << " allocations, " << totals.deallocations << " frees, "
                  << totals.locks << " locks\n";

    // The path really ran: audio out, both strips fed, analyser taps filled.
    CHECK (totals.finite);
    CHECK (totals.outputPeak > 0.05f);
    CHECK (totals.outputPeak <= 1.0f);
    CHECK (host.getStatus().callbacks == static_cast<uint64_t> (kWarmUpBlocks + kBlocks));
    CHECK (tapSamples > static_cast<int64_t> (kBlocks) * kBlock);

    ui::MeterSnapshot game, music;
    game.read (engine.load()->chain (0).meters());
    music.read (engine.load()->chain (1).meters());
    CHECK (game.inPeakDb[0] > -20.0f);
    CHECK (music.inPeakDb[0] > -20.0f);
}

TEST_CASE ("App: loopback guard trip, frozen blocks and release allocate, free and lock nothing on the audio thread (E51)")
{
    AudioEngineHost host;
    host.setDeviceInputRouting (0, 0);
    FakeAudioDevice device;
    host.audioDeviceAboutToStart (&device);

    std::array<std::vector<float>, kInputs> inputData;
    std::array<const float*, kInputs> inputs {};
    for (size_t c = 0; c < inputData.size(); ++c)
    {
        inputData[c].assign (kBlock, 0.0f);
        for (int i = 0; i < kBlock; ++i)
            inputData[c][static_cast<size_t> (i)] = 0.25f * static_cast<float> (std::sin (0.05 * static_cast<double> (i)));
        inputs[c] = inputData[c].data();
    }
    std::array<std::vector<float>, 2> outputData { std::vector<float> (kBlock), std::vector<float> (kBlock) };
    std::array<float*, 2> outputs { outputData[0].data(), outputData[1].data() };

    BlockTotals totals;
    bool warm = false;
    // Each batch runs on a fresh "device" thread; the guard is tripped and
    // released by this (the message) thread between batches.
    const auto runBlocks = [&] (int numBlocks)
    {
        std::thread t ([&]
        {
            const juce::AudioIODeviceCallbackContext context {};
            // The first block on a new thread promotes it (may allocate once).
            host.audioDeviceIOCallbackWithContext (inputs.data(), kInputs, outputs.data(), 2, kBlock, context);
            for (int b = 0; b < numBlocks; ++b)
            {
                flubapptest::RealtimeProbe probe;
                host.audioDeviceIOCallbackWithContext (inputs.data(), kInputs, outputs.data(), 2, kBlock, context);
                if (warm)
                {
                    totals.allocations += probe.allocations();
                    totals.deallocations += probe.deallocations();
                    totals.locks += probe.locks();
                    ++totals.blocks;
                }
            }
        });
        t.join();
    };

    runBlocks (20);
    warm = true;
    host.checkLoopbackPair ("CABLE Output (VB-Audio Virtual Cable)", "CABLE Input (VB-Audio Virtual Cable)");
    REQUIRE (host.isOutputMutedByGuard());
    runBlocks (10); // ramp down, then frozen
    CHECK (outputData[0][static_cast<size_t> (kBlock - 1)] == 0.0f);
    host.checkLoopbackPair ("CABLE Output (VB-Audio Virtual Cable)", "Headphones (USB Audio)");
    REQUIRE (! host.isOutputMutedByGuard());
    runBlocks (10); // ramp up
    host.audioDeviceStopped();

    CHECK (totals.blocks == 20);
    CHECK (totals.allocations == 0);
    CHECK (totals.deallocations == 0);
    CHECK (totals.locks == 0);
}
