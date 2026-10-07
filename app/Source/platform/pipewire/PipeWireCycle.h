// Flubsound Pro - one PipeWire graph cycle of the native node (docs/11 E48).
//
// Plain C++ (no libpipewire): PipeWireNative.cpp's process callback fetches
// each port's buffer (pw_filter_get_dsp_buffer, which returns null for a
// port without a buffer) and hands the pointers to CycleRunner::run on
// PipeWire's real-time data thread. The runner never allocates, locks or
// blocks: prepare() sizes everything beforehand. After the cycle the same
// callback hands the driver's clock to XrunCounter (R1.2), which counts the
// cycles this node missed or finished too late. Tests drive both with plain
// arrays and clocks.
#pragma once

#include "../PlatformServices.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace flub::platform::pipewire
{
class CycleRunner
{
public:
    /** Not real time: sizes the silence, scratch and pointer arrays. */
    void prepare (int inputs, int outputs, int maxBlockFrames)
    {
        numInputs = std::max (0, inputs);
        numOutputs = std::max (0, outputs);
        maxFrames = std::max (1, maxBlockFrames);
        silence.assign (static_cast<size_t> (maxFrames), 0.0f);
        scratch.assign (static_cast<size_t> (numOutputs) * static_cast<size_t> (maxFrames), 0.0f);
        inputPointers.assign (static_cast<size_t> (numInputs), nullptr);
        outputPointers.assign (static_cast<size_t> (numOutputs), nullptr);
    }

    int getNumInputs() const noexcept { return numInputs; }
    int getNumOutputs() const noexcept { return numOutputs; }
    int getMaxBlockFrames() const noexcept { return maxFrames; }

    /** Real time. portInputs / portOutputs: one pointer per port as PipeWire
        gave it (null = no buffer this cycle), each good for 'frames' samples.
        The callback gets never-null pointers in blocks of at most
        maxBlockFrames; a null output is written to scratch and dropped.
        cycleNs / rate: when the driver started this cycle (CLOCK_MONOTONIC
        ns) and the graph rate; each block's time is cycleNs plus its offset,
        so a split quantum reads as blocks one period apart, not as a burst
        (0 when either is unknown). Returns the number of callback calls. */
    int run (const float* const* portInputs, float* const* portOutputs, uint32_t frames, NativeAudioNode::Callback& callback, uint64_t cycleNs = 0,
             uint32_t rate = 0) noexcept FLUB_NONBLOCKING
    {
        int calls = 0;
        uint32_t done = 0;
        while (done < frames)
        {
            const uint32_t block = std::min (frames - done, static_cast<uint32_t> (maxFrames));
            const uint64_t blockNs = cycleNs != 0 && rate != 0 ? cycleNs + static_cast<uint64_t> (done) * kNsPerSecond / rate : 0;
            for (int i = 0; i < numInputs; ++i)
            {
                const float* port = portInputs != nullptr ? portInputs[i] : nullptr;
                inputPointers[static_cast<size_t> (i)] = port != nullptr ? port + done : silence.data();
            }
            for (int o = 0; o < numOutputs; ++o)
            {
                float* port = portOutputs != nullptr ? portOutputs[o] : nullptr;
                outputPointers[static_cast<size_t> (o)] = port != nullptr ? port + done : scratch.data() + static_cast<size_t> (o) * static_cast<size_t> (maxFrames);
            }
            callback.nodeProcess (inputPointers.data(), numInputs, outputPointers.data(), numOutputs, static_cast<int> (block), blockNs);
            done += block;
            ++calls;
        }
        return calls;
    }

    static constexpr uint64_t kNsPerSecond = 1000000000u;

private:
    int numInputs = 0, numOutputs = 0, maxFrames = 1;
    std::vector<float> silence, scratch;
    std::vector<const float*> inputPointers;
    std::vector<float*> outputPointers;
};

/** One graph cycle as the driver's clock (spa_io_position::clock) gives it. */
struct CycleClock
{
    uint32_t driverId = 0;  // clock.id: the node that drives the graph (it changes when the output moves to another device)
    uint32_t rate = 0;      // clock.rate.denom: samples per second; 0 = unknown
    uint64_t nsec = 0;      // clock.nsec: when the driver started this cycle (CLOCK_MONOTONIC); 0 = unknown
    uint64_t position = 0;  // clock.position, in samples at 'rate'
    uint64_t duration = 0;  // clock.duration: this cycle's quantum, in samples at 'rate'
    bool freewheel = false; // SPA_IO_CLOCK_FLAG_FREEWHEEL: no deadline (export), never an xrun
};

/** R1.2: the native node's own xrun count, for juce::AudioIODevice::
    getXRunCount and so for the app's glitch accounting (the header's "xr",
    the overload watchdog, the diagnostics log). libpipewire gives a client no
    xrun event, so the count is derived from the driver's clock, once per
    cycle, after the node's work:
    * late: the node finished after the next cycle was due (nsec + duration
      at rate). The driver then found the graph unfinished: PipeWire's own
      definition of an xrun, and the sink played without this node's output.
    * missed: the position moved on by more than the previous cycle's
      duration on the same driver at the same rate, i.e. the graph ran cycles
      without this node (a stall longer than a period).
    A cycle that is both counts once. A new driver (the output moved to
    another device), a new rate, a position that went backwards (the driver
    restarted) or freewheeling re-bases the comparison without counting.
    The first kSettleCycles cycles of a run (after reset()) and after the node
    streams again (restart(): PipeWire paused and resumed it, while the graph
    may have run on without it) are not judged; they only set the baseline.
    Real time: cycleDone() is wait-free; count() and restart() from any
    thread. */
class XrunCounter
{
public:
    /** Cycles not judged at the start of a run or after a pause. A run's
        first cycle is often late without anything going wrong: the data
        thread and the engine start cold, and the node joins a graph that is
        already running (CI, before this: 1 xrun within the first 20 cycles
        in 4 to 9 of 20 runs on an idle server, none in the 0.6 s after;
        with it, none in 20). */
    static constexpr int kSettleCycles = 2;

    /** Not real time (before the node starts): back to zero, no baseline. */
    void reset() noexcept
    {
        haveLast = false;
        settle = kSettleCycles;
        restartPending.store (false, std::memory_order_relaxed);
        xruns.store (0, std::memory_order_relaxed);
    }

    /** Any thread, wait-free (the loop thread, when the node enters
        streaming again): the next cycle starts a new baseline, and it and
        the one after are not judged. The count is kept. */
    void restart() noexcept { restartPending.store (true, std::memory_order_release); }

    /** Real time, once per cycle: 'doneNs' is when this node finished the
        cycle (CLOCK_MONOTONIC ns). True when the cycle counts as an xrun. */
    bool cycleDone (const CycleClock& cycle, uint64_t doneNs) noexcept FLUB_NONBLOCKING
    {
        if (restartPending.load (std::memory_order_relaxed) && restartPending.exchange (false, std::memory_order_acquire))
        {
            haveLast = false;
            settle = kSettleCycles;
        }
        if (cycle.freewheel || cycle.rate == 0 || cycle.duration == 0)
        {
            haveLast = false;
            return false;
        }
        bool xrun = false;
        if (settle > 0)
            --settle; // settling: a baseline only
        else
        {
            if (cycle.nsec != 0 && doneNs > cycle.nsec + cycle.duration * CycleRunner::kNsPerSecond / cycle.rate)
                xrun = true; // late
            if (haveLast && cycle.driverId == lastDriver && cycle.rate == lastRate && cycle.position > lastPosition + lastDuration)
                xrun = true; // missed
        }
        haveLast = true;
        lastDriver = cycle.driverId;
        lastRate = cycle.rate;
        lastPosition = cycle.position;
        lastDuration = cycle.duration;
        if (xrun)
            xruns.fetch_add (1, std::memory_order_relaxed);
        return xrun;
    }

    int count() const noexcept { return xruns.load (std::memory_order_relaxed); }

private:
    // The data thread's own (cycleDone), or before the node starts (reset):
    bool haveLast = false;
    int settle = kSettleCycles;
    uint32_t lastDriver = 0, lastRate = 0;
    uint64_t lastPosition = 0, lastDuration = 0;
    std::atomic<bool> restartPending { false };
    std::atomic<int> xruns { 0 };
};
} // namespace flub::platform::pipewire
