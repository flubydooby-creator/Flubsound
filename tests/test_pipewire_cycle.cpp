// Flubsound Pro - the native PipeWire node's bookkeeping that needs no
// server (R1.2, app/Source/platform/pipewire/PipeWireCycle.h and
// PipeWireGraph.h): the driver time each block of a split quantum is stamped
// with, the xrun count the "PipeWire" device reports through
// juce::AudioIODevice::getXRunCount, and the first start's choice of the
// node only where PipeWire plays audio. Plain C++ with made-up driver clocks
// and graphs, so it runs on every platform (the cycle runner's buffer
// handling and the link plans are tested in test_platform_linux.cpp, the
// node against a real server in tests/app/test_app_pipewire.cpp).
#include "TestFramework.h"

#include "../app/Source/platform/pipewire/PipeWireCycle.h"
#include "../app/Source/platform/pipewire/PipeWireGraph.h"

#include <cstdint>
#include <vector>

using namespace flub::platform;

namespace
{
/** Records each block's frame count and time stamp. */
struct StampCallback final : NativeAudioNode::Callback
{
    void nodeStarting (double, int) override {}
    void nodeProcess (const float* const*, int, float* const* outputs, int numOutputs, int numFrames, uint64_t timeNs) noexcept FLUB_NONBLOCKING override
    {
        for (int o = 0; o < numOutputs; ++o)
            for (int n = 0; n < numFrames; ++n)
                outputs[o][n] = 0.0f;
        if (calls < 8)
        {
            frames[calls] = numFrames;
            times[calls] = timeNs;
        }
        ++calls;
    }
    void nodeStopped() override {}

    int calls = 0;
    int frames[8] {};
    uint64_t times[8] {};
};

constexpr uint64_t kStartNs = 1000000000u; // 1 s on the monotonic clock
constexpr uint32_t kRate = 48000;
constexpr uint64_t kQuantum = 256;
constexpr uint64_t kPeriodNs = kQuantum * 1000000000u / kRate; // 5 333 333 ns

/** Cycle k of a steady graph on driver 30: 256 frames at 48 kHz. */
pipewire::CycleClock steady (uint64_t k)
{
    pipewire::CycleClock c;
    c.driverId = 30;
    c.rate = kRate;
    c.nsec = kStartNs + k * kPeriodNs;
    c.position = 4096 + k * kQuantum;
    c.duration = kQuantum;
    return c;
}
} // namespace

TEST_CASE ("Platform: the native node stamps each block of a split quantum with the driver's time, one block period apart (R1.2)")
{
    pipewire::CycleRunner runner;
    runner.prepare (1, 1, 256);
    std::vector<float> in (1024, 0.0f), out (1024, 0.0f);
    const float* inputs[1] = { in.data() };
    float* outputs[1] = { out.data() };

    // A 1024-frame quantum in 256-frame blocks: 0, 5.33, 10.67, 16 ms after
    // the cycle's start, so the callback timing (docs/11 E45) sees one block
    // per block period, not four back to back and a 3-period gap.
    StampCallback split;
    {
        flubtest::AllocationGuard guard;
        CHECK (runner.run (inputs, outputs, 1024, split, kStartNs, kRate) == 4);
        CHECK (guard.allocations() == 0);
    }
    CHECK (split.frames[0] == 256);
    CHECK (split.frames[3] == 256);
    CHECK (split.times[0] == kStartNs);
    CHECK (split.times[1] == kStartNs + 5333333u);
    CHECK (split.times[2] == kStartNs + 10666666u);
    CHECK (split.times[3] == kStartNs + 16000000u);

    // A quantum that fits is one block at the cycle's start.
    StampCallback whole;
    CHECK (runner.run (inputs, outputs, 128, whole, kStartNs + 7, kRate) == 1);
    CHECK (whole.times[0] == kStartNs + 7);

    // Unknown time or rate: 0 ("no host time"), never a made-up stamp.
    StampCallback unknownTime, unknownRate, none;
    runner.run (inputs, outputs, 512, unknownTime, 0, kRate);
    runner.run (inputs, outputs, 512, unknownRate, kStartNs, 0);
    runner.run (inputs, outputs, 512, none);
    CHECK (unknownTime.calls == 2);
    CHECK (unknownTime.times[0] == 0);
    CHECK (unknownTime.times[1] == 0);
    CHECK (unknownRate.times[1] == 0);
    CHECK (none.times[0] == 0);
}

TEST_CASE ("Platform: the native node counts a late cycle and missed cycles as xruns, and nothing else (R1.2)")
{
    pipewire::XrunCounter counter;
    CHECK (counter.count() == 0);

    // 200 steady cycles, each finished 1 ms after it started: none.
    uint64_t k = 0;
    {
        flubtest::AllocationGuard guard;
        for (; k < 200; ++k)
            CHECK (! counter.cycleDone (steady (k), steady (k).nsec + 1000000u));
        CHECK (guard.allocations() == 0);
    }
    CHECK (counter.count() == 0);

    // Finished exactly at the next cycle's start: still in time. 1 ns later:
    // late (the driver found the graph unfinished), once.
    CHECK (! counter.cycleDone (steady (k), steady (k).nsec + kPeriodNs));
    ++k;
    CHECK (counter.cycleDone (steady (k), steady (k).nsec + kPeriodNs + 1));
    CHECK (counter.count() == 1);
    ++k;

    // Two cycles ran without this node (the position moved on by three
    // quanta): missed, once.
    k += 2;
    CHECK (counter.cycleDone (steady (k), steady (k).nsec + 1000000u));
    CHECK (counter.count() == 2);
    ++k;

    // Late and missed in the same cycle count once.
    k += 1;
    CHECK (counter.cycleDone (steady (k), steady (k).nsec + 2 * kPeriodNs));
    CHECK (counter.count() == 3);
    ++k;

    // A smaller quantum (Low Latency, node.latency changed in place): the
    // position moves on by the previous cycle's duration; no xrun.
    auto c = steady (k);
    CHECK (! counter.cycleDone (c, c.nsec + 1000000u));
    auto small = c;
    small.position = c.position + c.duration;
    small.duration = 128;
    small.nsec = c.nsec + kPeriodNs;
    CHECK (! counter.cycleDone (small, small.nsec + 1000000u)); // 1 ms < 2.67 ms
    auto smallNext = small;
    smallNext.position += 128;
    smallNext.nsec += 2666666u;
    CHECK (! counter.cycleDone (smallNext, smallNext.nsec + 2000000u));
    CHECK (counter.cycleDone (smallNext, smallNext.nsec + 3000000u)); // late at 128 frames (and the same position: not missed twice)
    CHECK (counter.count() == 4);

    // The output moved to another device (a new driver, any position), the
    // graph changed rate, or the driver restarted (position back): re-based,
    // not counted.
    auto other = steady (k + 50);
    other.driverId = 31;
    other.position = 7;
    CHECK (! counter.cycleDone (other, other.nsec + 1000000u));
    auto resampled = other;
    resampled.rate = 44100;
    resampled.position = 999999;
    CHECK (! counter.cycleDone (resampled, resampled.nsec + 1000000u));
    auto restarted = resampled;
    restarted.position = 0;
    CHECK (! counter.cycleDone (restarted, restarted.nsec + 1000000u));
    CHECK (counter.count() == 4);

    // Freewheeling (an export runs as fast as it can, no deadline): never an
    // xrun, and the cycle after it starts a new baseline.
    auto freewheel = steady (k + 100);
    freewheel.freewheel = true;
    CHECK (! counter.cycleDone (freewheel, freewheel.nsec + 10 * kPeriodNs));
    auto afterFreewheel = steady (k + 500);
    CHECK (! counter.cycleDone (afterFreewheel, afterFreewheel.nsec + 1000000u));
    CHECK (counter.count() == 4);

    // A driver that gives no time: only missed cycles can count.
    auto noTime = steady (k + 501);
    noTime.nsec = 0;
    CHECK (! counter.cycleDone (noTime, kStartNs * 100));
    auto noTimeGap = steady (k + 503);
    noTimeGap.nsec = 0;
    CHECK (counter.cycleDone (noTimeGap, kStartNs * 100));
    CHECK (counter.count() == 5);

    // Nonsense clocks (no rate, no duration) are skipped.
    pipewire::CycleClock empty;
    CHECK (! counter.cycleDone (empty, kStartNs * 1000));
    CHECK (counter.count() == 5);

    // A new run starts from zero, without a baseline.
    counter.reset();
    CHECK (counter.count() == 0);
    CHECK (! counter.cycleDone (steady (10000), steady (10000).nsec));
    CHECK (counter.count() == 0);
}

TEST_CASE ("Platform: the native node does not judge its first cycles after a start or a pause - a late start-up cycle is no xrun, a later one is (R1.2)")
{
    constexpr uint64_t kInTimeNs = 1000000u; // finished 1 ms into the cycle
    static_assert (pipewire::XrunCounter::kSettleCycles == 2);

    // A new run: its first two cycles finish late (the data thread and the
    // engine start cold): they only set the baseline. The third is judged.
    pipewire::XrunCounter counter;
    {
        flubtest::AllocationGuard guard;
        CHECK (! counter.cycleDone (steady (0), steady (0).nsec + 2 * kPeriodNs));
        CHECK (! counter.cycleDone (steady (1), steady (1).nsec + kPeriodNs + 1));
        CHECK (counter.count() == 0);
        CHECK (counter.cycleDone (steady (2), steady (2).nsec + kPeriodNs + 1));
        CHECK (counter.count() == 1);
        CHECK (guard.allocations() == 0);
    }

    // reset() (the next open): the same again, from zero.
    counter.reset();
    CHECK (counter.count() == 0);
    CHECK (! counter.cycleDone (steady (100), steady (100).nsec + 3 * kPeriodNs));
    CHECK (! counter.cycleDone (steady (101), steady (101).nsec + kInTimeNs));
    CHECK (counter.cycleDone (steady (102), steady (102).nsec + kPeriodNs + 1));
    CHECK (counter.count() == 1);

    // Paused and streaming again on the same driver, which ran 500 cycles
    // without the node. restart() (the filter entered STREAMING): the jump is
    // no missed cycle and the first two cycles after it are not judged, even
    // a late one; the count is kept, and a real gap after them counts again.
    for (uint64_t k = 103; k < 110; ++k)
        CHECK (! counter.cycleDone (steady (k), steady (k).nsec + kInTimeNs));
    counter.restart();
    counter.restart(); // twice before the next cycle: one restart
    {
        flubtest::AllocationGuard guard;
        CHECK (! counter.cycleDone (steady (610), steady (610).nsec + 2 * kPeriodNs));
        CHECK (! counter.cycleDone (steady (611), steady (611).nsec + kInTimeNs));
        CHECK (guard.allocations() == 0);
    }
    CHECK (counter.count() == 1);
    CHECK (! counter.cycleDone (steady (612), steady (612).nsec + kInTimeNs));
    CHECK (counter.cycleDone (steady (614), steady (614).nsec + kInTimeNs)); // one cycle missed
    CHECK (counter.count() == 2);

    // Without restart() the same pause reads as 500 missed cycles (one xrun):
    // why the node calls it whenever it streams again.
    pipewire::XrunCounter noRestart;
    for (uint64_t k = 0; k < 5; ++k)
        CHECK (! noRestart.cycleDone (steady (k), steady (k).nsec + kInTimeNs));
    CHECK (noRestart.cycleDone (steady (505), steady (505).nsec + kInTimeNs));
    CHECK (noRestart.count() == 1);

    // A restart whose next cycle carries no clock (no rate or duration):
    // still a fresh baseline, and the settling waits for real cycles.
    noRestart.restart();
    CHECK (! noRestart.cycleDone (pipewire::CycleClock {}, kStartNs));
    CHECK (! noRestart.cycleDone (steady (900), steady (900).nsec + 2 * kPeriodNs));
    CHECK (! noRestart.cycleDone (steady (901), steady (901).nsec + kInTimeNs));
    CHECK (noRestart.cycleDone (steady (902), steady (902).nsec + kPeriodNs + 1));
    CHECK (noRestart.count() == 2);
}

namespace
{
pipewire::Node graphNode (uint32_t id, const std::string& name, const std::string& mediaClass)
{
    pipewire::Node node;
    node.id = id;
    node.name = name;
    node.mediaClass = mediaClass;
    return node;
}
} // namespace

TEST_CASE ("Platform: a first start prefers the PipeWire node only where PipeWire plays audio - an output sink that is not Flubsound's own (R1.2)")
{
    const auto strips = pipewire::defaultStrips();
    const std::string card = "alsa_output.pci-0000_00_1f.3.analog-stereo";

    // A PipeWire run for screen capture and cameras beside PulseAudio: it
    // answers, but its graph has no output, so the first start keeps ALSA
    // (pulse-alsa) instead of a node that would play into nothing.
    pipewire::Graph capture;
    capture.nodes[30] = graphNode (30, "v4l2_input.pci-0000_00_14.0-usb-0_5_1.0", "Video/Source");
    capture.nodes[31] = graphNode (31, "xdg-desktop-portal-gnome", "Stream/Input/Video");
    capture.nodes[32] = graphNode (32, "alsa_input.usb-microphone", "Audio/Source"); // a source is not an output
    CHECK (! pipewire::playsAudio (capture, "", strips));
    CHECK (! pipewire::playsAudio (capture, card, strips)); // a default named in the metadata but not in the graph
    capture.nodes[40] = graphNode (40, "flubsound_game", "Audio/Sink"); // only Flubsound's own sinks: no output either
    capture.nodes[41] = graphNode (41, "flubsound_system", "Audio/Sink");
    CHECK (! pipewire::playsAudio (capture, "flubsound_system", strips));

    // A PipeWire desktop: a sound card's sink, the default or not, or
    // WirePlumber's fallback "Dummy Output" while no card is up.
    pipewire::Graph desktop = capture;
    desktop.nodes[50] = graphNode (50, card, "Audio/Sink");
    CHECK (pipewire::playsAudio (desktop, card, strips));
    CHECK (pipewire::playsAudio (desktop, "", strips));
    CHECK (pipewire::playsAudio (desktop, "flubsound_system", strips)); // System made the default: the card is still there
    pipewire::Graph fallback;
    fallback.nodes[60] = graphNode (60, "auto_null", "Audio/Sink");
    CHECK (pipewire::playsAudio (fallback, "auto_null", strips));
}
