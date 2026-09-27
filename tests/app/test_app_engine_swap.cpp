// App-level tests: the crossfaded engine swap of AudioEngineHost (R1.5).
// A structural change while the device runs (latency profile, strip layout,
// neural model install / removal) builds a new engine on the message thread and hands it to the audio thread,
// which pre-rolls it and crossfades (equal latency) or dips old out / new in
// (latency change) over AudioEngineHost::kSwapFadeMs; the old engine is
// destroyed back on the message thread. The host is started with a fake
// juce::AudioIODevice (48 kHz / 256) and its callback is driven block by
// block on a persistent "device" thread, so this thread (the message thread)
// can reconfigure between any two blocks. A 220 Hz sine feeds the Music strip.
#include "AppTestSupport.h"

#include "engine/AudioEngineHost.h"

#include "flub/engine/Parameters.h"
#include "flub/neural/ReferenceRunners.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

using namespace flub::app;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr double kSineHz = 220.0;
constexpr float kSineLevel = 0.25f;
const int kFadeSamples = static_cast<int> (std::lround (AudioEngineHost::kSwapFadeMs * 0.001 * kRate));    // 480
const int kSettleSamples = static_cast<int> (std::lround (AudioEngineHost::kSwapSettleMs * 0.001 * kRate)); // 480

class FakeAudioDevice final : public juce::AudioIODevice
{
public:
    FakeAudioDevice() : juce::AudioIODevice ("Fake Output", "Fake") {}

    juce::StringArray getOutputChannelNames() override { return { "Left", "Right" }; }
    juce::StringArray getInputChannelNames() override { return { "In 1", "In 2" }; }
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
    juce::BigInteger getActiveInputChannels() const override { return juce::BigInteger (0x3); }
    int getOutputLatencyInSamples() override { return kBlock; }
    int getInputLatencyInSamples() override { return kBlock; }
};

/** The backend's audio thread: one persistent thread that calls the host's
    callback when asked. run() returns once the blocks are done, so the test
    (the message thread) acts between any two blocks, never during one. */
class DeviceThread
{
public:
    explicit DeviceThread (AudioEngineHost& h) : host (h), thread ([this] { loop(); })
    {
        for (auto& ch : outputData)
            ch.assign (kBlock, 0.0f);
    }

    ~DeviceThread()
    {
        {
            const std::lock_guard<std::mutex> lock (mutex);
            quit = true;
        }
        wake.notify_all();
        thread.join();
    }

    /** Runs numBlocks callbacks; false if the hang guard expired. */
    bool run (int numBlocks)
    {
        std::unique_lock<std::mutex> lock (mutex);
        requested += numBlocks;
        wake.notify_all();
        return wake.wait_for (lock, std::chrono::seconds (60), [this] { return done == requested; });
    }

    /** Runs blocks until the host has completed `swaps` swaps (at most maxBlocks). */
    bool runUntilSwaps (uint32_t swaps, int maxBlocks = 100)
    {
        for (int b = 0; b < maxBlocks && host.getCompletedSwaps() < swaps; ++b)
            if (! run (1))
                return false;
        return host.getCompletedSwaps() == swaps;
    }

    // Recorded output (both channels, every block) and the realtime probe's
    // totals over all blocks; read between run() calls only.
    std::array<std::vector<float>, 2> recorded;
    int64_t allocations = 0, deallocations = 0, locks = 0;
    bool silentInput = false;

private:
    void loop()
    {
        std::unique_lock<std::mutex> lock (mutex);
        for (;;)
        {
            wake.wait (lock, [this] { return quit || done < requested; });
            if (quit)
                return;
            while (done < requested)
            {
                lock.unlock();
                processOneBlock();
                lock.lock();
                ++done;
            }
            wake.notify_all();
        }
    }

    void processOneBlock()
    {
        for (int i = 0; i < kBlock; ++i, ++sample)
        {
            const float v = silentInput ? 0.0f
                                        : kSineLevel * static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * kSineHz
                                                                                     * static_cast<double> (sample) / kRate));
            inputData[0][static_cast<size_t> (i)] = v;
            inputData[1][static_cast<size_t> (i)] = v;
        }
        const std::array<const float*, 2> inputs { inputData[0].data(), inputData[1].data() };
        const std::array<float*, 2> outputs { outputData[0].data(), outputData[1].data() };
        const juce::AudioIODeviceCallbackContext context {};

        // The first block on this thread promotes it (may allocate once).
        const bool probed = promoted;
        flubapptest::RealtimeProbe probe;
        host.audioDeviceIOCallbackWithContext (inputs.data(), 2, outputs.data(), 2, kBlock, context);
        if (probed)
        {
            allocations += probe.allocations();
            deallocations += probe.deallocations();
            locks += probe.locks();
        }
        promoted = true;

        for (size_t c = 0; c < 2; ++c)
            recorded[c].insert (recorded[c].end(), outputData[c].begin(), outputData[c].end());
    }

    AudioEngineHost& host;
    std::array<std::array<float, kBlock>, 2> inputData {};
    std::array<std::vector<float>, 2> outputData;
    int64_t sample = 0;
    bool promoted = false;

    std::mutex mutex;
    std::condition_variable wake;
    int requested = 0, done = 0;
    bool quit = false;
    std::thread thread; // last: starts once everything above exists
};

struct Analysis
{
    float peak = 0.0f, maxStep = 0.0f, rms = 0.0f;
    int longestGap = 0; // consecutive samples below the gap threshold
    bool finite = true;
};

/** Both channels of [from, to): peak, largest sample-to-sample step, RMS and
    the longest run of samples below gapThreshold. */
Analysis analyse (const std::array<std::vector<float>, 2>& rec, size_t from, size_t to, float gapThreshold = 0.0f)
{
    Analysis a;
    double sum = 0.0;
    for (const auto& ch : rec)
    {
        int run = 0;
        for (size_t i = from; i < to; ++i)
        {
            const float v = ch[i];
            a.finite = a.finite && std::isfinite (v);
            a.peak = std::max (a.peak, std::abs (v));
            if (i > from)
                a.maxStep = std::max (a.maxStep, std::abs (v - ch[i - 1]));
            sum += static_cast<double> (v) * v;
            run = std::abs (v) < gapThreshold ? run + 1 : 0;
            a.longestGap = std::max (a.longestGap, run);
        }
    }
    a.rms = static_cast<float> (std::sqrt (sum / static_cast<double> (2 * (to - from))));
    return a;
}

/** Bound on any sample step at output peak `peak`: the sine's own largest
    step per unit of peak (steady state, +10 %), plus what the steepest point
    of a raised-cosine fade (pi / 2F per sample) adds across two engines'
    outputs (<= 2 x peak apart). A hard cut or a restart from silence would
    step by a large part of the peak. */
float stepBound (float stepPerPeak, float peak)
{
    return 1.1f * stepPerPeak * peak + 2.0f * peak * juce::MathConstants<float>::pi / (2.0f * static_cast<float> (kFadeSamples));
}

void setProfile (AudioEngineHost& host, flub::param::LatencyProfileValue profile)
{
    for (int s = 0; s < host.getNumStrips(); ++s)
        host.getMixEngine().params (s).set (flub::param::LatencyProfile, static_cast<float> (profile));
}

/** Host with the Music strip (1) fed by device inputs 0/1, device started. */
void start (AudioEngineHost& host, FakeAudioDevice& device)
{
    host.setDeviceInputRouting (1, 0);
    host.audioDeviceAboutToStart (&device); // message thread: configures synchronously
}
} // namespace

TEST_CASE ("App: engine swap on a latency-profile change and a layout change mid-stream: no click, no gap longer than the fade")
{
    using Profile = flub::param::LatencyProfileValue;
    AudioEngineHost host;
    FakeAudioDevice device;
    start (host, device);
    DeviceThread audio (host);

    // ~2 s to settle (the device start fades in, the dynamics find their level).
    REQUIRE (audio.run (400));
    const size_t swap1 = audio.recorded[0].size();
    const auto before = analyse (audio.recorded, swap1 - 100 * kBlock, swap1);

    // Balanced -> Quality: the latency grows, so old fades out and new fades in.
    const int oldLatency = host.getLatencyInfo().engineSamples;
    auto* oldChain = &host.getMixEngine().chain (1);
    const auto generation = host.getStructureGeneration();
    setProfile (host, Profile::Quality);
    REQUIRE (host.needsReprepare());
    host.reconfigure();
    CHECK (host.isSwapInProgress());
    CHECK (! host.needsReprepare());                          // the newest engine is prepared ...
    CHECK (host.getStructureGeneration() != generation);      // ... re-fetch chains
    CHECK (&host.getMixEngine().chain (1) != oldChain);
    CHECK (host.getMixEngine().chain (1).getLatencyProfile() == Profile::Quality);
    const int newLatency = host.getLatencyInfo().engineSamples;
    CHECK (newLatency > oldLatency);
    REQUIRE (audio.runUntilSwaps (1));
    CHECK (! host.isSwapInProgress());
    // Pre-roll (new latency + settle, the old engine fading out over its last
    // kSwapFadeMs) + the fade-in, rounded up to whole blocks.
    const auto swapSamples = static_cast<int> (audio.recorded[0].size() - swap1);
    CHECK_GE (swapSamples, newLatency + kSettleSamples + kFadeSamples);
    CHECK (swapSamples < newLatency + kSettleSamples + kFadeSamples + kBlock);
    REQUIRE (audio.run (300));
    const size_t swap2 = audio.recorded[0].size();
    const auto middle = analyse (audio.recorded, swap2 - 100 * kBlock, swap2);

    // A layout change at the same latency (Chat 2 -> 6 channels, one strip
    // fewer): the two engines crossfade with equal gain.
    auto layout = host.getStripLayout();
    layout[2].inputChannels = 6;
    layout.pop_back();
    host.setStripLayout (layout);
    CHECK (host.isSwapInProgress());
    CHECK (host.getNumStrips() == 3);
    CHECK (host.getLatencyInfo().engineSamples == newLatency);
    REQUIRE (audio.runUntilSwaps (2));
    REQUIRE (audio.run (300));
    const size_t end = audio.recorded[0].size();
    const auto after = analyse (audio.recorded, end - 100 * kBlock, end);

    // Bound on any step: the sine's own at the loudest level seen anywhere,
    // plus the fade slope (stepBound).
    const size_t from = static_cast<size_t> (100 * kBlock);
    const auto whole = analyse (audio.recorded, from, end, 0.05f * before.peak);
    const float stepPerPeak = std::max ({ before.maxStep / before.peak, middle.maxStep / middle.peak, after.maxStep / after.peak });
    const float bound = stepBound (stepPerPeak, whole.peak);
    const auto swapOne = analyse (audio.recorded, swap1, swap1 + static_cast<size_t> (swapSamples), 0.05f * before.peak);
    std::cerr << "    swap: steady peak " << before.peak << " / " << middle.peak << " / " << after.peak << ", step bound " << bound
              << ", max step " << whole.maxStep << " (steady " << before.maxStep << "), longest gap " << whole.longestGap
              << " samples (in the dip " << swapOne.longestGap << "), latency " << oldLatency << " -> " << newLatency << "\n";

    CHECK (whole.finite);
    CHECK (before.peak > 0.1f);
    CHECK_LE (whole.maxStep, bound);
    CHECK_LE (whole.longestGap, kFadeSamples); // no silence longer than one fade
    CHECK (swapOne.longestGap > 10);           // (the latency change really went through the dip)
    // No bump: the equal-gain crossfade never exceeds either engine; the new
    // engine's own detectors start fresh and settle within ~0.2 dB over
    // ~100 ms (as after any prepare), hence +0.5 dB.
    CHECK_LE (whole.peak, std::max ({ before.peak, middle.peak, after.peak }) * 1.06f);
    CHECK (middle.rms > 0.7f * before.rms);    // the output continues at its level
    CHECK (after.rms > 0.9f * middle.rms);

    // Nothing was allocated or freed on the audio thread, the swaps included.
    CHECK (audio.allocations == 0);
    CHECK (audio.deallocations == 0);
    CHECK (audio.locks == 0);

    host.audioDeviceStopped();
}

TEST_CASE ("App: engine swap x100 (profiles and layouts, a superseded request) with no allocation on the audio thread")
{
    using Profile = flub::param::LatencyProfileValue;
    AudioEngineHost host;
    FakeAudioDevice device;
    start (host, device);
    DeviceThread audio (host);
    REQUIRE (audio.run (20));

    const auto defaultLayout = AudioEngineHost::defaultStripLayout();
    auto smallLayout = defaultLayout;
    smallLayout.resize (2);
    smallLayout[1].inputChannels = 6;

    uint32_t expected = 0;
    bool allCompleted = true;
    for (int i = 0; i < 100; ++i)
    {
        switch (i % 4)
        {
            case 0: setProfile (host, Profile::LowLatency); host.reconfigure(); break;
            case 1: host.setStripLayout (smallLayout); break;
            case 2: setProfile (host, Profile::Balanced); host.reconfigure(); break;
            default: host.setStripLayout (defaultLayout); break;
        }
        if (i % 10 == 9)
        {
            // Two requests before the audio thread runs: the first never
            // plays (disposed on the message thread), one swap completes.
            setProfile (host, Profile::Quality);
            host.reconfigure();
            setProfile (host, Profile::Balanced);
            host.reconfigure();
        }
        ++expected;
        allCompleted = allCompleted && audio.runUntilSwaps (expected);
    }
    CHECK (allCompleted);
    CHECK (host.getCompletedSwaps() == expected);
    CHECK (! host.isSwapInProgress());
    CHECK (host.getNumStrips() == 4);

    REQUIRE (audio.run (50));
    const size_t end = audio.recorded[0].size();
    const auto tail = analyse (audio.recorded, end - 20 * kBlock, end);
    CHECK (tail.finite);
    CHECK (tail.peak > 0.05f);

    CHECK (audio.allocations == 0);
    CHECK (audio.deallocations == 0);
    CHECK (audio.locks == 0);
    if (audio.allocations != 0 || audio.deallocations != 0 || audio.locks != 0)
        std::cerr << "    audio thread: " << audio.allocations << " allocations, " << audio.deallocations << " frees, " << audio.locks
                  << " locks\n";

    host.audioDeviceStopped();
    // Every engine is destroyed with the host (ASan builds report a leak otherwise).
}

TEST_CASE ("App: engine swap across a device restart (pending, mid-crossfade, while stopped) is safe and fades in")
{
    using Profile = flub::param::LatencyProfileValue;
    AudioEngineHost host;
    FakeAudioDevice device;
    start (host, device);
    DeviceThread audio (host);
    REQUIRE (audio.run (100));

    const auto restartIsClean = [&] (const char* what)
    {
        const size_t restart = audio.recorded[0].size();
        REQUIRE (audio.run (200));
        const size_t end = audio.recorded[0].size();
        const auto steady = analyse (audio.recorded, end - 100 * kBlock, end);
        const auto all = analyse (audio.recorded, restart, end);
        const float bound = 1.1f * steady.maxStep + steady.peak * juce::MathConstants<float>::pi / (2.0f * static_cast<float> (kFadeSamples));
        std::cerr << "    restart (" << what << "): max step " << all.maxStep << ", bound " << bound << "\n";
        CHECK (all.finite);
        CHECK (audio.recorded[0][restart] == 0.0f); // the start fades in from silence ...
        CHECK_LE (all.maxStep, bound);               // ... without a step
        CHECK (steady.peak > 0.1f);
        CHECK (! host.isSwapInProgress());
    };

    // 1. A swap is published, the device stops before the audio thread took it,
    //    and restarts: the newest engine (Quality) runs, no swap is left over.
    setProfile (host, Profile::Quality);
    host.reconfigure();
    CHECK (host.isSwapInProgress());
    host.audioDeviceStopped();
    host.audioDeviceAboutToStart (&device);
    CHECK (host.getMixEngine().chain (1).getLatencyProfile() == Profile::Quality);
    restartIsClean ("pending swap");

    // 2. The device stops in the middle of a crossfade.
    setProfile (host, Profile::LowLatency);
    host.reconfigure();
    REQUIRE (audio.run (2));
    CHECK (host.isSwapInProgress());
    host.audioDeviceStopped();
    host.audioDeviceAboutToStart (&device);
    CHECK (host.getMixEngine().chain (1).getLatencyProfile() == Profile::LowLatency);
    restartIsClean ("mid-crossfade");

    // 3. Changes while the device is stopped apply at once (nothing to fade).
    host.audioDeviceStopped();
    setProfile (host, Profile::Balanced);
    host.reconfigure();
    CHECK (! host.isSwapInProgress());
    CHECK (host.getMixEngine().chain (1).getLatencyProfile() == Profile::Balanced);
    auto layout = host.getStripLayout();
    layout.pop_back();
    host.setStripLayout (layout);
    CHECK (host.getNumStrips() == 3);
    host.audioDeviceAboutToStart (&device);
    restartIsClean ("while stopped");

    CHECK (host.getCompletedSwaps() == 0); // no swap ever completed: every one was cut short
    CHECK (audio.allocations == 0);
    CHECK (audio.deallocations == 0);
    CHECK (audio.locks == 0);
    host.audioDeviceStopped();
}

TEST_CASE ("App: neural model install and removal mid-stream go through the swap (model in the new engine, no click, no gap)")
{
    using Profile = flub::param::LatencyProfileValue;
    AudioEngineHost host;
    FakeAudioDevice device;
    start (host, device);
    DeviceThread audio (host);
    REQUIRE (audio.run (400));

    // A 10 ms-frame identity model (unity controls: the processor is a pure
    // delay of its latency L = 2 frames, eligible for Balanced), whether or
    // not its worker keeps up with this faster-than-real-time device: a
    // missed frame falls back to the same unity gain. Every engine built
    // gets its own runner from the factory.
    static constexpr int kFrame = 480;
    int runnersMade = 0;
    const auto factory = [&runnersMade]() -> std::unique_ptr<flub::ModelRunner>
    {
        ++runnersMade;
        return std::make_unique<flub::IdentityRunner> (kFrame);
    };

    const auto steadyBefore = analyse (audio.recorded, audio.recorded[0].size() - 100 * kBlock, audio.recorded[0].size());
    const int plainLatency = host.getLatencyInfo().engineSamples;
    const size_t install = audio.recorded[0].size();
    host.setNeuralModel (1, factory);
    CHECK (host.hasNeuralModel (1));
    CHECK (host.isSwapInProgress());
    CHECK (runnersMade == 1);
    CHECK (host.getMixEngine().chain (1).getNeuralStatus().state == flub::NeuralSlotState::Active);
    CHECK (host.getMixEngine().chain (0).getNeuralStatus().state == flub::NeuralSlotState::Empty);
    CHECK (! host.needsReprepare());
    const int modelLatency = host.getLatencyInfo().engineSamples;
    CHECK (modelLatency == plainLatency + 2 * kFrame);
    REQUIRE (audio.runUntilSwaps (1));
    REQUIRE (audio.run (300));
    const auto steadyModel = analyse (audio.recorded, audio.recorded[0].size() - 100 * kBlock, audio.recorded[0].size());

    // A profile change keeps the model (a fresh runner for the new engine);
    // Low Latency's budget (1 frame) leaves it out of the chain, Balanced
    // brings it back.
    setProfile (host, Profile::LowLatency);
    host.reconfigure();
    CHECK (runnersMade == 2);
    CHECK (host.getMixEngine().chain (1).getNeuralStatus().state == flub::NeuralSlotState::Ineligible);
    REQUIRE (audio.runUntilSwaps (2));
    setProfile (host, Profile::Balanced);
    host.reconfigure();
    CHECK (runnersMade == 3);
    CHECK (host.getMixEngine().chain (1).getNeuralStatus().state == flub::NeuralSlotState::Active);
    REQUIRE (audio.runUntilSwaps (3));
    REQUIRE (audio.run (300));

    // Removal: the next engine has no model and the old latency.
    const size_t removal = audio.recorded[0].size();
    host.clearNeuralModel (1);
    CHECK (! host.hasNeuralModel (1));
    CHECK (runnersMade == 3);
    CHECK (host.getMixEngine().chain (1).getNeuralStatus().state == flub::NeuralSlotState::Empty);
    CHECK (host.getLatencyInfo().engineSamples == plainLatency);
    REQUIRE (audio.runUntilSwaps (4));
    REQUIRE (audio.run (300));
    const size_t end = audio.recorded[0].size();
    const auto steadyAfter = analyse (audio.recorded, end - 100 * kBlock, end);

    // Install and removal: no step beyond the sine's own plus the fade slope,
    // no near-silence longer than one fade, and the level carries on.
    const float stepPerPeak = std::max ({ steadyBefore.maxStep / steadyBefore.peak, steadyModel.maxStep / steadyModel.peak,
                                          steadyAfter.maxStep / steadyAfter.peak });
    for (const size_t at : { install, removal })
    {
        const auto swap = analyse (audio.recorded, at - kBlock, at + 40 * kBlock, 0.05f * steadyBefore.peak);
        std::cerr << "    neural swap at " << at << ": max step " << swap.maxStep << " (bound " << stepBound (stepPerPeak, swap.peak)
                  << "), longest gap " << swap.longestGap << ", peak " << swap.peak << "\n";
        CHECK (swap.finite);
        CHECK_LE (swap.maxStep, stepBound (stepPerPeak, swap.peak));
        CHECK_LE (swap.longestGap, kFadeSamples);
        CHECK (swap.longestGap > 10); // the latency changed: through the dip
    }
    CHECK (steadyModel.rms > 0.9f * steadyBefore.rms); // the identity model leaves the level alone
    CHECK (steadyAfter.rms > 0.9f * steadyBefore.rms);

    CHECK (audio.allocations == 0);
    CHECK (audio.deallocations == 0);
    CHECK (audio.locks == 0);
    host.audioDeviceStopped();
}

TEST_CASE ("App: a strip the new layout removes plays its delayed tail out in the old engine instead of being cut")
{
    AudioEngineHost host;
    FakeAudioDevice device;
    host.setDeviceInputRouting (3, 0); // the sine feeds only System (strip 3)
    host.audioDeviceAboutToStart (&device);
    DeviceThread audio (host);
    REQUIRE (audio.run (300));
    const size_t swapAt = audio.recorded[0].size();
    const auto steady = analyse (audio.recorded, swapAt - 50 * kBlock, swapAt);
    const int latency = host.getLatencyInfo().engineSamples;

    // Remove System. Its source stops now (the new engine has no such strip),
    // but the old engine still holds `latency` samples of it in its delay
    // lines: they play out (it gets silence from here on) before the old
    // engine fades out, rather than stopping after the master limiter's few
    // samples of look-ahead.
    auto layout = host.getStripLayout();
    layout.pop_back();
    host.setStripLayout (layout);
    CHECK (host.getLatencyInfo().engineSamples == latency); // equal latency: overlapping crossfade
    REQUIRE (audio.runUntilSwaps (1));
    REQUIRE (audio.run (20));

    const auto tail = analyse (audio.recorded, swapAt + 100, swapAt + static_cast<size_t> (latency) - 20);
    const auto afterwards = analyse (audio.recorded, audio.recorded[0].size() - 10 * kBlock, audio.recorded[0].size());
    std::cerr << "    removed strip: steady rms " << steady.rms << ", tail rms " << tail.rms << " (latency " << latency
              << "), afterwards peak " << afterwards.peak << "\n";
    CHECK (steady.rms > 0.05f);
    CHECK (tail.rms > 0.3f * steady.rms);
    CHECK (afterwards.peak < 1.0e-4f); // nothing feeds the remaining strips
    CHECK (! host.isStripActive (3));
    CHECK (audio.allocations == 0);
    CHECK (audio.deallocations == 0);
    CHECK (audio.locks == 0);
    host.audioDeviceStopped();
}
