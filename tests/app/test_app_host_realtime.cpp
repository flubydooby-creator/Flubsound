// App-level tests: the real-time status of the device callback's thread
// (docs/11 E44 Phase A). The callback is driven from its own "device"
// thread, as a backend drives it; the platform's scheduling reads and the
// RealtimeKit request are replaced by fakes (setRealtimeHooks), so nothing
// here changes the test process's scheduling or limits (the platform side
// runs against a mock rtkit in tests/test_platform_linux.cpp).
//   * The first callback on a new thread promotes it and records its kernel
//     thread id without allocating, freeing or locking.
//   * The message thread (serviceAudioThreadRealtime) reads that thread's
//     scheduling, asks RealtimeKit once when it is not real-time, and the
//     outcome is what EngineStatus::audioThread (EngineController::
//     getStatus()) reports: granted, refused with the fix, or the audio
//     server's own real-time thread.
#include "AppTestSupport.h"

#include "engine/AudioEngineHost.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

using namespace flub::app;
using flub::platform::RealtimeKitResult;
using flub::platform::ThreadScheduling;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

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

/** Stands in for the kernel and rtkit: a scheduling per thread id, and the
    requests made. */
struct FakeScheduler
{
    std::mutex mutex;
    ThreadScheduling current { true, false, 0, "OTHER" };
    ThreadScheduling afterGrant { true, true, 20, "RR" };
    RealtimeKitResult answer { RealtimeKitResult::Outcome::Granted, 20, 200000, {} };
    std::vector<std::pair<uint64_t, int>> requests;
    std::vector<uint64_t> queried;

    AudioEngineHost::RealtimeHooks hooks()
    {
        AudioEngineHost::RealtimeHooks h;
        h.query = [this] (uint64_t threadId)
        {
            std::lock_guard<std::mutex> guard (mutex);
            queried.push_back (threadId);
            return current;
        };
        h.request = [this] (uint64_t threadId, int priority)
        {
            std::lock_guard<std::mutex> guard (mutex);
            requests.emplace_back (threadId, priority);
            if (answer.outcome == RealtimeKitResult::Outcome::Granted)
                current = afterGrant;
            return answer;
        };
        return h;
    }
};

/** Runs `blocks` callbacks on a new thread (a device thread); returns that
    thread's kernel id and what the first callback allocated, freed and
    locked. */
struct DeviceThreadRun
{
    uint64_t threadId = 0;
    int64_t firstAllocations = -1, firstFrees = -1, firstLocks = -1;
};

DeviceThreadRun runOnDeviceThread (AudioEngineHost& host, int blocks)
{
    DeviceThreadRun run;
    std::thread device (
        [&host, &run, blocks]
        {
            std::array<std::vector<float>, 2> in, out;
            std::array<const float*, 2> inPtr {};
            std::array<float*, 2> outPtr {};
            for (size_t c = 0; c < 2; ++c)
            {
                in[c].assign (kBlock, 0.0f);
                out[c].assign (kBlock, 0.0f);
                inPtr[c] = in[c].data();
                outPtr[c] = out[c].data();
            }
            run.threadId = flub::platform::RealtimeScheduling::currentThreadId();
            const juce::AudioIODeviceCallbackContext context;
            for (int b = 0; b < blocks; ++b)
            {
                flubapptest::RealtimeProbe probe;
                host.audioDeviceIOCallbackWithContext (inPtr.data(), 2, outPtr.data(), 2, kBlock, context);
                if (b == 0)
                {
                    run.firstAllocations = probe.allocations();
                    run.firstFrees = probe.deallocations();
                    run.firstLocks = probe.locks();
                }
            }
        });
    device.join();
    return run;
}
} // namespace

TEST_CASE ("App: a new device thread is recorded without allocating, and the message thread asks RealtimeKit for it once (E44)")
{
    AudioEngineHost host;
    FakeScheduler scheduler;
    host.setRealtimeHooks (scheduler.hooks());
    FakeAudioDevice device;
    host.audioDeviceAboutToStart (&device);
    CHECK (host.getStatus().audioThread.state == AudioThreadRealtime::State::Unknown);
    CHECK (host.getStatus().audioThread.describe() == "real-time status unknown");

    [[maybe_unused]] const auto first = runOnDeviceThread (host, 8);
#if defined(__linux__)
    // The first callback promotes its thread (SCHED_FIFO where the user's
    // limits allow) and records the thread id: no allocation (before this
    // item: one, the saved policy), no free, no lock.
    CHECK (first.firstAllocations == 0);
    CHECK (first.firstFrees == 0);
    CHECK (first.firstLocks == 0);
    CHECK (first.threadId != 0);
#endif

    host.serviceAudioThreadRealtime();
    const auto status = host.getStatus().audioThread;
#if defined(__linux__)
    REQUIRE (scheduler.requests.size() == 1);
    CHECK (scheduler.requests[0].first == first.threadId);
    CHECK (scheduler.requests[0].second == AudioEngineHost::kRealtimeKitPriority);
    CHECK (status.state == AudioThreadRealtime::State::RealTime);
    CHECK (status.via == AudioThreadRealtime::Via::RealtimeKit);
    CHECK (status.policy == "RR");
    CHECK (status.priority == 20);
    CHECK (status.rtkitRequests == 1);
    CHECK (status.describe() == "real-time (RR 20 via rtkit)");
    CHECK (host.getAudioThreadRealtime().describe() == status.describe());

    // Nothing new: no second request, whether the timer runs again or the
    // same thread calls back again.
    host.serviceAudioThreadRealtime();
    host.audioDeviceIOCallbackWithContext (nullptr, 0, nullptr, 0, 0, {});
    host.serviceAudioThreadRealtime(); // the message thread's own call: a new thread, never asked for
    CHECK (scheduler.requests.size() == 1);

    // A device restart runs on a new thread: asked for again.
    host.audioDeviceStopped();
    host.audioDeviceAboutToStart (&device);
    scheduler.current = { true, false, 0, "OTHER" };
    const auto second = runOnDeviceThread (host, 2);
    host.serviceAudioThreadRealtime();
    REQUIRE (scheduler.requests.size() == 2);
    CHECK (scheduler.requests[1].first == second.threadId);
    CHECK (host.getStatus().audioThread.rtkitRequests == 2);
    CHECK (host.getStatus().audioThread.state == AudioThreadRealtime::State::RealTime);
#else
    // No thread ids here: only the callback's own promotion is known, and
    // RealtimeKit is never asked.
    CHECK (scheduler.requests.empty());
    CHECK (status.state != AudioThreadRealtime::State::NotRealTime);
#endif
    host.audioDeviceStopped();
}

TEST_CASE ("App: RealtimeKit refused, the audio server's own thread and the message thread - the audio thread status says which, and the fix (E44)")
{
#if defined(__linux__)
    FakeAudioDevice device;
    {
        // Refused (rtkit's burst limit, policy, or no rtkit): NOT real-time,
        // with rtkit's reason and what the user can do.
        AudioEngineHost host;
        FakeScheduler scheduler;
        scheduler.answer = { RealtimeKitResult::Outcome::Refused, 20, 200000, "RealtimeKit refused real-time priority 20 (Operation not permitted)" };
        host.setRealtimeHooks (scheduler.hooks());
        host.audioDeviceAboutToStart (&device);
        runOnDeviceThread (host, 2);
        host.serviceAudioThreadRealtime();
        const auto status = host.getStatus().audioThread;
        CHECK (scheduler.requests.size() == 1);
        CHECK (status.state == AudioThreadRealtime::State::NotRealTime);
        CHECK (status.policy == "OTHER");
        CHECK (status.describe().startsWith ("NOT real-time: RealtimeKit refused real-time priority 20 (Operation not permitted); "));
        CHECK (status.detail.contains ("rtkit package"));
        CHECK (status.detail.contains ("rtprio"));
        CHECK (status.detail.contains ("JACK"));
        host.audioDeviceStopped();
    }
    {
        // The audio server's own real-time thread (JACK / PipeWire-JACK):
        // already real-time and not promoted by us: nothing is asked.
        AudioEngineHost host;
        FakeScheduler scheduler;
        scheduler.current = { true, true, 83, "FIFO" };
        host.setRealtimeHooks (scheduler.hooks());
        host.audioDeviceAboutToStart (&device);
        runOnDeviceThread (host, 2);
        host.serviceAudioThreadRealtime();
        const auto status = host.getStatus().audioThread;
        CHECK (scheduler.requests.empty());
        CHECK (status.state == AudioThreadRealtime::State::RealTime);
        // Promoted when this process could make it FIFO itself (RT rights),
        // else the server's thread.
        CHECK ((status.via == AudioThreadRealtime::Via::Backend || status.via == AudioThreadRealtime::Via::Promoted));
        if (status.via == AudioThreadRealtime::Via::Backend)
            CHECK (status.describe() == "real-time (FIFO 83, the audio server's thread)");
        host.audioDeviceStopped();
    }
    {
        // A callback driven from the message thread (offline / test drivers)
        // is never made real-time, even with a stand-in rtkit.
        AudioEngineHost host;
        FakeScheduler scheduler;
        host.setRealtimeHooks (scheduler.hooks());
        host.audioDeviceAboutToStart (&device);
        std::array<float, kBlock> l {}, r {};
        std::array<float*, 2> out { l.data(), r.data() };
        host.audioDeviceIOCallbackWithContext (nullptr, 0, out.data(), 2, kBlock, {});
        host.serviceAudioThreadRealtime();
        CHECK (scheduler.requests.empty());
        CHECK (host.getStatus().audioThread.state == AudioThreadRealtime::State::NotRealTime);
        CHECK (host.getStatus().audioThread.detail.contains ("OTHER"));
        host.audioDeviceStopped();
    }
#endif
    // The platform's own hooks ask RealtimeKit only while a device the host
    // opened itself runs: a fake device started by hand is left alone.
    AudioEngineHost plain;
    FakeAudioDevice other;
    plain.audioDeviceAboutToStart (&other);
    runOnDeviceThread (plain, 2);
    plain.serviceAudioThreadRealtime();
    CHECK (plain.getStatus().audioThread.rtkitRequests == 0);
    plain.audioDeviceStopped();
}
