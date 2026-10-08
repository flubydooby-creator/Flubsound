// Flubsound Pro - runs a ModelRunner off the audio thread behind a fixed latency
// (docs/09-future-roadmap.md §1.1).
//
//   audio thread (process)                      inference worker (one thread)
//   copy input into the current model frame     sleep until woken (safety timeout)
//   frame full -> input FrameQueue       ────►  frame -> runner->run() -> controls
//   end of the call: wake the worker     ────►    (every queued frame, then sleep)
//   every frame boundary: pop the result ◄────  push the control frame
//     for the frame now reaching the output
//   output = input delayed by L, times the smoothed controls
//   (BandGains: input delayed by safetyFrames frames, then the band renderer)
//
// Latency. L = frameSize * (1 + safetyFrames) for BroadbandGain and
// ChannelGains, frameSize * (2 + safetyFrames) for BandGains (its overlap-add
// adds one frame, see "Band gains" below), reported by latencySamples() and
// constant for the processor's lifetime. The control computed from input
// frame k is applied to exactly the samples of frame k when they leave the
// delay line, so the worker has safetyFrames frame periods from the moment
// frame k is complete to deliver its result (the same for every kind). That budget must cover one host
// block, the model's worst-case run time, the worker's wake-up latency (one
// poll interval when it polls) and scheduling jitter. The block is a hard limit: a result can only be picked
// up by a later process() call than the one that submitted its frame, so
// with blocks longer than safetyFrames * frameSize about
// (block - safetyFrames * frameSize) / block of the frames miss even with an
// instant model (getMaxBlockSizeWithoutMisses(); ProcessingChain keeps such
// a model out of the chain). safetyFrames = 0 is allowed but only useful in
// offline mode: asynchronously, expect every frame to miss.
//
// Band gains (ControlKind::BandGains, flub/neural/BandGains.h). The controls
// are the gains of numControls frequency bands, applied to every channel by
// an STFT renderer on the audio thread: the input is delayed by
// safetyFrames * frameSize, cut into hops of frameSize, and at every frame
// boundary the last two hops are windowed (Vorbis, 2 * frameSize), zero-
// padded to fftSize, transformed, multiplied by the bin gains interpolated
// from the band gains, transformed back, windowed again and overlap-added.
// The window that ends with input frame k is the one the model analysed for
// frame k, so its gains land on exactly the spectra they were computed from.
// Overlap-add completes a hop one frame after its second window, so
// L = frameSize * (2 + safetyFrames); with unity gains the output is the
// input delayed by L to within float rounding (a null below -120 dB). The
// controls change once per frame and the overlapping windows crossfade the
// change over one frame (controlRampMs does not apply); the fallback holds
// the last good band gains, then sets them to unity. Cost on the audio
// thread: one forward and one inverse real FFT of fftSize per channel per
// frame. The model sees the same frames as with the other kinds.
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
// Waking the worker (getWorkerWake(), getWorkerWakeups()). The worker sleeps
// until the audio thread has queued work: at the end of every process() call
// that queued a frame, the audio thread signals a WakeEvent
// (flub/neural/WakeEvent.h). That is an atomic exchange, plus one
// non-blocking OS wake-up call only when the worker is asleep: SetEvent
// (Windows), a futex wake (Linux), semaphore_signal (macOS). It never waits
// or takes a lock; RealtimeSanitizer cannot tell it from a blocking system
// call, so that one call is exempted on purpose (WakeEvent.h). The worker
// therefore wakes once per process() call that completes a frame (at most
// one wake-up per host block and per model frame: 100 a second with
// 480-sample blocks and 5 ms frames) and answers within the OS's wake-up
// latency rather than up to one poll interval plus timer slack later. Its
// sleep has a long safety timeout
// (workerTimeoutMicroseconds; auto = 100 ms, so an idle prepared worker
// wakes 10 times a second), and a stop request signals it, so prepare() and
// the destructor do not wait for a timeout. Until 2026-10-08 the worker
// polled the queue with a bounded sleep instead (frame period / 8, clamped to
// 100 .. 1000 us: 1 600 wake-ups a second for 5 ms frames, busy or idle,
// where the OS timer is precise; on Windows a short sleep lasts at least one
// timer period, ~1 ms after timeBeginPeriod (1)). config.workerPolls keeps
// that behaviour (workerPollMicroseconds), and it is the fallback when the OS
// object cannot be created.
//
// Worker scheduling (getWorkerScheduling()). The worker has a deadline, so
// it must get a CPU as soon as it is woken (and, polling, its sleeps must end
// on time). macOS coalesces the timers of every thread that is not real
// time, whatever its QoS class: on the GitHub macOS runner a 625 us sleep
// lasted 5.6 - 6.2 ms (median) on a default, a utility and a user-interactive
// QoS thread alike, and the polling worker's response reached 8.5 ms (99th
// percentile) of its 10 ms budget at 480-sample blocks. So on macOS the
// worker gives itself the Mach time-constraint (real-time) policy Core
// Audio's I/O threads have, whose timers are not coalesced (the same sleep:
// 0.64 - 0.66 ms; the response: about 1 ms): period one model frame,
// computation half of it, constraint one frame (the period clamped to
// 1 .. 40 ms). A refused policy leaves the worker as it was (Default).
// Windows and Linux keep the OS default. On every OS the model runs inside a
// ScopedNoDenormals (FTZ / DAZ, AArch64 FZ), like every real-time entry
// point; offline mode too, so both paths compute the same bits.
//
// Threading. prepare() (non-RT) stops a running worker, allocates the queues,
// calls runner->prepare() and starts one worker thread (none in offline
// mode). process() and reset() are RT-safe (no allocation, no lock, no wait;
// bounded by the queue capacities and the block length; process() makes at
// most one non-blocking wake-up call, above; in offline mode it also runs
// the model); reset() may be called on the audio thread,
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
#include "flub/dsp/Fft.h"
#include "flub/dsp/Processor.h"
#include "flub/neural/BandGains.h"
#include "flub/neural/FrameQueue.h"
#include "flub/neural/ModelRunner.h"
#include "flub/neural/WakeEvent.h"

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
    static constexpr int kMaxSafetyFrames = 16;
    int safetyFrames = 1;             // 0 .. kMaxSafetyFrames frames of head room for the worker
                                      // (L = frameSize * (1 + safetyFrames); BandGains: frameSize * (2 + safetyFrames))
    int fallbackAfterFrames = 4;      // K >= 1: consecutive missed / failed frames before the ramp to neutral
    float controlRampMs = 5.0f;       // every control change is a linear ramp this long (>= 1 sample)
    float maxGain = 4.0f;             // controls are clamped to [0, maxGain] (+12 dB by default)
    int workerTimeoutMicroseconds = 0; // the woken worker's safety timeout: 0 = auto (100 ms); 100 .. 1 000 000 us
    bool workerPolls = false;         // poll the queue instead of being woken (the behaviour before 2026-10-08; "Waking the worker")
    int workerPollMicroseconds = 0;   // polling only: 0 = auto (frame period / 8, 100 .. 1000 us); at most 100 000 us
    bool offline = false;             // run the model inside process() on the calling thread (non-RT callers only, see above)
};

/** How the inference worker learns that a frame is queued ("Waking the worker" above). */
enum class NeuralWorkerWake : int
{
    None = 0,   // no worker thread running (unprepared, offline mode, model not active)
    Signal = 1, // the audio thread wakes it (the default)
    Poll = 2    // it polls the queue (config.workerPolls, or no OS wake-up object could be created)
};

/** How the inference worker thread is scheduled ("Worker scheduling" above). */
enum class NeuralWorkerScheduling : int
{
    None = 0,          // no worker thread running (unprepared, offline mode, model not active), or not started yet
    Default = 1,       // the OS's default policy for a new thread (Windows, Linux; macOS if the policy was refused)
    TimeConstraint = 2 // macOS: the Mach time-constraint (real-time) policy
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
    /** frameSize * (1 + safetyFrames); BandGains: frameSize * (2 + safetyFrames). */
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
    /** The worker thread's scheduling: None until the worker has started
        (shortly after prepare()) and after it stops, then what it got. */
    NeuralWorkerScheduling getWorkerScheduling() const noexcept
    {
        return static_cast<NeuralWorkerScheduling> (workerScheduling.load (std::memory_order_acquire));
    }
    /** How the worker learns about queued frames: Signal or Poll from
        prepare() while a worker runs, None otherwise ("Waking the worker"). */
    NeuralWorkerWake getWorkerWake() const noexcept
    {
        return static_cast<NeuralWorkerWake> (workerWakeMode.load (std::memory_order_acquire));
    }
    /** The worker's sleeps that ended (woken, timed out or a poll interval
        over), i.e. its OS wake-ups; a wait that found a signal already there
        did not sleep and is not counted. The energy measure of "Waking the worker". */
    uint64_t getWorkerWakeups() const noexcept { return workerWakeups.load (std::memory_order_relaxed); }

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
    void waitForWork() noexcept;
    void resetAudioState() noexcept;
    bool submitFrame() noexcept; // true when the frame went to the worker's queue
    void runFrame (const float* frame, uint64_t seq, bool& resetPending) noexcept;
    void consumeResult() noexcept;
    void setTargets (const float* controls) noexcept;
    void setNeutralTargets() noexcept;
    void renderBands() noexcept; // BandGains: one frame of the STFT renderer, every channel

    std::unique_ptr<ModelRunner> runner;
    ModelDescription desc;
    AsyncModelConfig config;
    bool descriptionValid = false;
    int frameSize = 0, latency = 0;
    int activeControls = 1;               // controls the audio path applies (1 for BroadbandGain)
    int inputFloats = 0;                  // numInputChannels * frameSize
    bool bandGains = false;               // ControlKind::BandGains
    int lineLength = 0;                   // delay line per channel: latency, or safetyFrames * frameSize for BandGains

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

    // BandGains renderer (audio thread).
    Fft fft;
    bandgains::BandMap bandMap;
    std::vector<float> window;            // 2 * frameSize
    std::vector<float> hopIn, prevHop;    // per channel: the delayed hop being collected, the previous one
    std::vector<float> olaTail, hopOut;   // per channel: overlap-add carry, the hop being output
    std::vector<float> fftBuffer;         // fftSize
    std::vector<Fft::Complex> bins;       // fftSize / 2 + 1
    std::vector<float> bandTargets;       // numControls band gains in effect
    std::vector<float> binGains;          // fftSize / 2 + 1
    FrameQueue inQueue, outQueue;

    // Worker thread (the calling thread in offline mode).
    std::thread worker;
    std::vector<float> workerControls;
    bool workerSignalled = false;         // set in prepare(): the audio thread signals `wake` (else the worker polls)
    int pollMicroseconds = 1000;          // polling: the sleep between looks at the queue
    int timeoutMicroseconds = 100000;     // signalled: the safety timeout of one sleep
    double framePeriodSeconds = 0.0;      // the worker's scheduling period (frameSize / sample rate)

    // Shared.
    WakeEvent wake;                       // audio thread -> worker ("Waking the worker")
    std::atomic<bool> stopRequested { false };
    std::atomic<uint64_t> firstUsefulSeq { 0 }; // frames below this are past their deadline
    std::atomic<uint64_t> deadlineMisses { 0 }, modelFailures { 0 }, framesProcessed { 0 };
    std::atomic<uint64_t> framesSubmitted { 0 }, framesHandled { 0 };
    std::atomic<uint64_t> workerWakeups { 0 };
    std::atomic<int> workerScheduling { 0 };    // NeuralWorkerScheduling, written by the worker
    std::atomic<int> workerWakeMode { 0 };      // NeuralWorkerWake, written by prepare() / stopWorker()
};
} // namespace flub
