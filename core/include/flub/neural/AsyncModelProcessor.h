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
// block, the model's worst-case run time, the worker's poll interval and
// scheduling jitter. The block is a hard limit: a result can only be picked
// up by a later process() call than the one that submitted its frame, so
// with blocks longer than safetyFrames * frameSize about
// (block - safetyFrames * frameSize) / block of the frames miss even with an
// instant model (getMaxBlockSizeWithoutMisses(); ProcessingChain keeps such
// a model out of the chain). safetyFrames = 0 is allowed but only useful in
// offline mode: asynchronously, expect every frame to miss.
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
// up at once after a stall. process() never waits (outside offline mode).
//
// Offline mode (AsyncModelConfig::offline; ProcessingChain sets it for
// ModelContext::Offline). A batch render calls process() back to back, far
// faster than real time: the sample-clock deadline of a frame then passes
// microseconds after it was submitted, before a polling worker can answer,
// so most frames would fall back to the dry signal, by an amount that
// depends on thread scheduling. In offline mode there is no worker:
// process() runs the model on the calling thread as soon as a frame is
// complete, so every frame's result is there when the frame reaches the
// output, whatever the render speed or the block length, and the output is
// the same on every run. process() then takes as long as the model does, so
// offline mode is for non-real-time callers only (a batch render, never an
// audio callback). Latency, controls, failure fallback and counters work as
// above; there are no deadline misses.
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
// calls runner->prepare() and starts one worker thread (none in offline
// mode). process() and reset() are RT-safe (no allocation, no lock, no wait;
// bounded by the queue capacities and the block length; in offline mode
// process() also runs the model); reset() may be called on the audio thread,
// as ProcessingChain does when it drops a NaN block, and it does not stop the
// worker: frames already queued become stale and are skipped. The destructor
// stops and joins the worker (it waits for a frame that is still running).
// The counters are relaxed atomics, readable from any thread; they restart
// at prepare() but not at reset() (they are telemetry).
//
// In the chain: ProcessingChain::setNeuralModel() wraps a runner in one of
// these and runs it in the chain's neural slot (after the gate, before the
// EQ and every dynamics stage) when isEligible() allows it for the prepared
// latency profile (flub/neural/Eligibility.h) and, in real time, the host
// buffer is no longer than getMaxBlockSizeWithoutMisses().
#pragma once

#include "flub/common/SmoothedValue.h"
#include "flub/dsp/Processor.h"
#include "flub/neural/FrameQueue.h"
#include "flub/neural/ModelRunner.h"

#include <atomic>
#include <cstdint>
#include <limits>
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
    int workerPollMicroseconds = 0;   // 0 = auto (frame period / 8, 100 .. 1000 us); at most 100 000 us
    bool offline = false;             // run the model inside process() on the calling thread (non-RT callers only, see above)
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
    /** Non-RT. Stops the worker and returns to the unprepared state (process()
        does nothing) until the next prepare(). ProcessingChain calls it for a
        model that is installed but not in the chain. */
    void releaseResources() noexcept;
    void reset() noexcept FLUB_NONBLOCKING override;
    void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING override;
    int latencySamples() const noexcept override { return latency; }
    const char* name() const noexcept override { return "Neural"; }

    const ModelDescription& getDescription() const noexcept { return desc; }
    const AsyncModelConfig& getConfig() const noexcept { return config; }

    /** True after prepare() when the model runs (valid description, matching
        sample rate). Otherwise a prepared processor with a valid description
        still delays by L at unity gain. */
    bool isModelActive() const noexcept { return modelActive; }

    /** The longest host block for which every result can be on time:
        safetyFrames * frameSize (0 with no safety frame), or INT_MAX in
        offline mode. Longer blocks make a fixed fraction of the frames miss
        whatever the model's speed (see "Latency" above). */
    int getMaxBlockSizeWithoutMisses() const noexcept
    {
        return config.offline ? (std::numeric_limits<int>::max)() : config.safetyFrames * frameSize; // parenthesised: windows.h max
    }

    // ---- telemetry (relaxed atomics, any thread; restart at prepare()) -------
    /** Frame boundaries whose result had not arrived in time. */
    uint64_t getDeadlineMisses() const noexcept { return deadlineMisses.load (std::memory_order_relaxed); }
    /** Frame boundaries whose result arrived in time but was a failure. */
    uint64_t getModelFailures() const noexcept { return modelFailures.load (std::memory_order_relaxed); }
    /** runner->run() calls the worker completed (successful or not). */
    uint64_t getFramesProcessed() const noexcept { return framesProcessed.load (std::memory_order_relaxed); }

    /** Input frames queued but not yet run or skipped by the worker (never
        negative; 0 in offline mode). Read on the thread that calls process(),
        0 means the worker has handled every frame submitted so far: each
        result it produced is in the result queue (acquire), so the next
        process() call sees it. Frames it skipped as stale, or whose result
        found the result queue full, leave no result. Lets a test drive the
        real asynchronous path deterministically. On any other thread the
        value is a snapshot that may lag behind by the frames in flight. */
    int getPendingFrames() const noexcept;

private:
    // ---- implementation-defined below this line ----
    void startWorker();
    void stopWorker() noexcept;
    void workerLoop() noexcept;
    void resetAudioState() noexcept;
    void submitFrame() noexcept;
    void runFrame (const float* frame, uint64_t seq, bool& resetPending) noexcept;
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

    // Worker thread (the calling thread in offline mode).
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
