// Flubsound Pro - runs a ModelRunner off the audio thread behind a fixed latency
// (docs/09-future-roadmap.md §1.1).
//
//   audio thread (process)                      inference worker (one thread)
//   copy input into the current model frame     poll the input queue
//   frame full -> input FrameQueue       ────►  frame -> runner->run() -> controls
//   every frame boundary: pop the result ◄────  push the control frame
//     for the frame now reaching the output
//   output = input delayed by L, times the smoothed controls
//
// Latency. L = frameSize * (1 + safetyFrames), reported by latencySamples()
// and constant for the processor's lifetime. The control computed from input
// frame k is applied to exactly the samples of frame k when they leave the
// delay line, so the worker has safetyFrames frame periods from the moment
// frame k is complete to deliver its result. That budget must cover one host
// block (a result can only be picked up by a later process() call: with
// blocks longer than safetyFrames * frameSize every frame misses), the
// model's worst-case run time, the worker's poll interval and scheduling
// jitter. safetyFrames = 0 is allowed but only useful with models that are
// never late, which no asynchronous model is: expect every frame to miss.
//
// Graceful degradation. A result that is not there when its frame reaches
// the output is a deadline miss (getDeadlineMisses()); a result whose run()
// returned false or that holds a NaN / Inf is a model failure
// (getModelFailures()). Either way the last good control frame stays in
// place; after fallbackAfterFrames consecutive bad frames the controls ramp
// to neutral (unity gain = the delayed dry signal). The next good result
// ramps back. Every control change is a linear ramp of controlRampMs, so no
// transition can click: one sample moves a gain by at most
// |change| / rampSamples. A late result is dropped when it arrives, and the
// worker skips input frames whose deadline has already passed, so it catches
// up at once after a stall. process() never waits.
//
// Waking the worker. The audio thread only publishes frames to a lock-free
// queue; it does not signal the worker. std::atomic::wait / notify would
// need macOS 11 in libc++ and is a futex / WaitOnAddress system call on the
// audio thread (RTSan's contract forbids blocking calls there), so the worker
// polls the queue with a bounded sleep instead (workerPollMicroseconds; auto
// = frame period / 8, clamped to 100 .. 1000 us). The cost is CPU wakeups
// while idle (at most 10 000/s; 1 000 - 1 600/s for 5 - 10 ms frames at
// 48 kHz), and up to one poll interval plus the OS timer slack added to the
// model's run time, which the safety frames must absorb. On Windows a short
// sleep lasts up to one system timer period: 15.6 ms by default, ~1 ms once
// the process has called timeBeginPeriod (1). A host that needs the last
// millisecond can shorten the interval.
//
// Threading. prepare() (non-RT) stops a running worker, allocates the queues,
// calls runner->prepare() and starts one worker thread. process() and reset()
// are RT-safe (no allocation, no lock, no wait; bounded by the queue
// capacities and the block length); reset() may be called on the audio thread,
// as ProcessingChain does when it drops a NaN block, and it does not stop the
// worker: frames already queued become stale and are skipped. The destructor
// stops and joins the worker (it waits for a frame that is still running).
// The counters are relaxed atomics, readable from any thread; they restart
// at prepare() but not at reset() (they are telemetry).
//
// Not in ProcessingChain yet: the chain has a fixed array of module slots, so
// putting a neural module into it is still a code change (a new slot, its
// parameters and the latency-profile eligibility check in
// flub/neural/Eligibility.h).
#pragma once

#include "flub/common/SmoothedValue.h"
#include "flub/dsp/Processor.h"
#include "flub/neural/FrameQueue.h"
#include "flub/neural/ModelRunner.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace flub
{
struct AsyncModelConfig
{
    int safetyFrames = 1;             // 0 .. 16 frames of head room for the worker (L = frameSize * (1 + safetyFrames))
    int fallbackAfterFrames = 4;      // K >= 1: consecutive missed / failed frames before the ramp to neutral
    float controlRampMs = 5.0f;       // every control change is a linear ramp this long (>= 1 sample)
    float maxGain = 4.0f;             // controls are clamped to [0, maxGain] (+12 dB by default)
    int workerPollMicroseconds = 0;   // 0 = auto (frame period / 8, 100 .. 1000 us)
};

class AsyncModelProcessor final : public Processor
{
public:
    /** A null runner or an invalid description (see ModelDescription) gives an
        inert processor: zero latency, audio untouched, no worker. */
    explicit AsyncModelProcessor (std::unique_ptr<ModelRunner> runner, const AsyncModelConfig& config = {});
    ~AsyncModelProcessor() override;

    AsyncModelProcessor (const AsyncModelProcessor&) = delete;
    AsyncModelProcessor& operator= (const AsyncModelProcessor&) = delete;

    void prepare (const ProcessSpec& spec) override;
    void reset() noexcept override;
    void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING override;
    int latencySamples() const noexcept override { return latency; }
    const char* name() const noexcept override { return "Neural"; }

    const ModelDescription& getDescription() const noexcept { return desc; }
    const AsyncModelConfig& getConfig() const noexcept { return config; }

    /** True after prepare() when the model runs (valid description, matching
        sample rate). Otherwise a prepared processor with a valid description
        still delays by L at unity gain. */
    bool isModelActive() const noexcept { return modelActive; }

    // ---- telemetry (relaxed atomics, any thread; restart at prepare()) -------
    /** Frame boundaries whose result had not arrived in time. */
    uint64_t getDeadlineMisses() const noexcept { return deadlineMisses.load (std::memory_order_relaxed); }
    /** Frame boundaries whose result arrived in time but was a failure. */
    uint64_t getModelFailures() const noexcept { return modelFailures.load (std::memory_order_relaxed); }
    /** runner->run() calls the worker completed (successful or not). */
    uint64_t getFramesProcessed() const noexcept { return framesProcessed.load (std::memory_order_relaxed); }

    /** Input frames queued but not yet run or skipped by the worker. When it
        reads 0, every result for the frames submitted so far is in the result
        queue (acquire), so the next process() call on the audio thread sees
        them. Lets a test drive the real asynchronous path deterministically. */
    int getPendingFrames() const noexcept;

private:
    // ---- implementation-defined below this line ----
    void startWorker();
    void stopWorker() noexcept;
    void workerLoop() noexcept;
    void resetAudioState() noexcept;
    void submitFrame() noexcept;
    void consumeResult() noexcept;
    void setTargets (const float* controls) noexcept;

    std::unique_ptr<ModelRunner> runner;
    ModelDescription desc;
    AsyncModelConfig config;
    bool descriptionValid = false;
    int frameSize = 0, latency = 0;
    int activeControls = 1;               // controls the audio path applies (1 for BroadbandGain)
    int inputFloats = 0;                  // numInputChannels * frameSize

    // Audio thread (and prepare / reset).
    bool prepared = false, modelActive = false;
    int numChannels = 0;
    std::vector<float> delayBuffer;       // numChannels lines of `latency` samples
    int delayPos = 0;
    std::vector<float> stagingFrame;      // the model frame being collected (planar)
    int framePos = 0;                     // samples collected in stagingFrame
    int framesSinceReset = 0;             // saturates at safetyFrames + 1 (first boundary)
    bool discontinuity = true;            // next submitted frame starts a new stream
    uint64_t nextSubmitSeq = 0, neededSeq = 0;
    int consecutiveBad = 0;
    std::vector<LinearSmoothedValue> smoothers; // one per active control
    std::vector<float> gains;             // per-sample smoothed gains (scratch)
    FrameQueue inQueue, outQueue;

    // Worker thread.
    std::thread worker;
    std::vector<float> workerControls;
    int pollMicroseconds = 1000;

    // Shared.
    std::atomic<bool> stopRequested { false };
    std::atomic<uint64_t> firstUsefulSeq { 0 }; // frames below this are past their deadline
    std::atomic<uint64_t> deadlineMisses { 0 }, modelFailures { 0 }, framesProcessed { 0 };
    std::atomic<uint64_t> framesSubmitted { 0 }, framesHandled { 0 };
};
} // namespace flub
