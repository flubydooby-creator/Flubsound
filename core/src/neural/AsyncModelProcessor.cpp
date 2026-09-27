#include "flub/neural/AsyncModelProcessor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace flub
{
namespace
{
constexpr int kMaxSafetyFrames = 16;

// Auto poll interval: an eighth of a frame period, so polling adds at most
// ~12 % of a frame to the worker's response time, within 100 .. 1000 us so
// tiny frames do not spin and long ones do not idle for whole milliseconds.
constexpr double kAutoPollFraction = 0.125;
constexpr int kMinAutoPollMicroseconds = 100;
constexpr int kMaxAutoPollMicroseconds = 1000;

// Explicit poll intervals are capped: the worker only notices a stop request
// between sleeps, so prepare() and the destructor wait up to one interval.
constexpr int kMaxPollMicroseconds = 100000;

// Frames that can be in flight between the audio thread and the worker: the
// safety frames, the frames one host block can complete, and a little slack
// so a worker that is merely late never finds the result queue full.
constexpr int kQueueSlackFrames = 4;

bool isValid (const ModelDescription& d) noexcept
{
    return d.frameSize >= 1 && d.frameSize <= kMaxModelFrameSize
        && d.numInputChannels >= 1 && d.numInputChannels <= kMaxChannels
        && d.numControls >= 1 && d.numControls <= kMaxModelControls
        && (d.controlKind == ControlKind::BroadbandGain || d.controlKind == ControlKind::ChannelGains)
        && std::isfinite (d.sampleRate) && d.sampleRate >= 0.0;
}
} // namespace

AsyncModelProcessor::AsyncModelProcessor (std::unique_ptr<ModelRunner> r, const AsyncModelConfig& c)
    : runner (std::move (r)), config (c)
{
    config.safetyFrames = std::clamp (config.safetyFrames, 0, kMaxSafetyFrames);
    config.fallbackAfterFrames = std::max (1, config.fallbackAfterFrames);
    config.controlRampMs = std::isfinite (config.controlRampMs) ? std::max (0.0f, config.controlRampMs) : 0.0f;
    config.maxGain = std::isfinite (config.maxGain) ? std::max (0.0f, config.maxGain) : 1.0f;
    config.workerPollMicroseconds = std::clamp (config.workerPollMicroseconds, 0, kMaxPollMicroseconds);

    if (runner != nullptr)
    {
        desc = runner->describe();
        descriptionValid = isValid (desc);
    }
    if (descriptionValid)
    {
        frameSize = desc.frameSize;
        latency = frameSize * (1 + config.safetyFrames);
        activeControls = desc.controlKind == ControlKind::BroadbandGain ? 1 : std::min (desc.numControls, kMaxChannels);
        inputFloats = desc.numInputChannels * frameSize;
    }
}

AsyncModelProcessor::~AsyncModelProcessor()
{
    stopWorker();
}

void AsyncModelProcessor::prepare (const ProcessSpec& s)
{
    stopWorker();
    prepared = false;
    modelActive = false;
    if (! descriptionValid)
        return;

    numChannels = std::clamp (s.numChannels, 1, kMaxChannels);
    delayBuffer.assign (static_cast<size_t> (numChannels) * static_cast<size_t> (latency), 0.0f);
    stagingFrame.assign (static_cast<size_t> (inputFloats), 0.0f);
    workerControls.assign (static_cast<size_t> (desc.numControls), 1.0f);

    const int blockFrames = (std::max (1, s.maxBlockSize) + frameSize - 1) / frameSize;
    const auto queueFrames = static_cast<size_t> (config.safetyFrames + blockFrames + kQueueSlackFrames);
    inQueue.allocate (queueFrames, static_cast<size_t> (inputFloats));
    outQueue.allocate (queueFrames, static_cast<size_t> (desc.numControls));

    smoothers.assign (static_cast<size_t> (activeControls), LinearSmoothedValue {});
    for (auto& sm : smoothers)
        sm.reset (s.sampleRate, config.controlRampMs, 1.0f);
    gains.assign (static_cast<size_t> (activeControls), 1.0f);

    if (config.workerPollMicroseconds > 0)
        pollMicroseconds = config.workerPollMicroseconds;
    else
        pollMicroseconds = std::clamp (static_cast<int> (kAutoPollFraction * 1.0e6 * frameSize / std::max (1.0, s.sampleRate)),
                                       kMinAutoPollMicroseconds, kMaxAutoPollMicroseconds);

    deadlineMisses.store (0, std::memory_order_relaxed);
    modelFailures.store (0, std::memory_order_relaxed);
    framesProcessed.store (0, std::memory_order_relaxed);
    framesSubmitted.store (0, std::memory_order_relaxed);
    framesHandled.store (0, std::memory_order_relaxed);
    nextSubmitSeq = 0;
    neededSeq = 0;
    firstUsefulSeq.store (0, std::memory_order_relaxed);
    resetAudioState();

    const bool rateMatches = desc.sampleRate == 0.0 || desc.sampleRate == s.sampleRate;
    if (rateMatches)
    {
        runner->prepare (s.sampleRate); // may throw: the processor then stays unprepared
        startWorker();                  // so may std::thread (std::system_error), with the same result
    }
    modelActive = rateMatches;
    prepared = true;
}

void AsyncModelProcessor::releaseResources() noexcept
{
    stopWorker();
    prepared = false;
    modelActive = false;
}

void AsyncModelProcessor::resetAudioState() noexcept
{
    std::fill (delayBuffer.begin(), delayBuffer.end(), 0.0f);
    std::fill (stagingFrame.begin(), stagingFrame.end(), 0.0f);
    delayPos = 0;
    framePos = 0;
    framesSinceReset = 0;
    consecutiveBad = 0;
    discontinuity = true;
    for (auto& sm : smoothers)
        sm.setImmediate (1.0f);
}

void AsyncModelProcessor::reset() noexcept FLUB_NONBLOCKING
{
    if (! prepared)
        return;
    resetAudioState();

    // Frame numbers keep counting across resets, so everything queued or in
    // flight now is older than the first frame of the new stream: the worker
    // skips those inputs, and their results are dropped here or at the next
    // boundary (consumeResult() discards anything older than neededSeq).
    neededSeq = nextSubmitSeq;
    firstUsefulSeq.store (neededSeq, std::memory_order_release);
    FrameQueue::Header h;
    while (outQueue.peek (h) != nullptr)
        outQueue.release();
}

int AsyncModelProcessor::getPendingFrames() const noexcept
{
    const uint64_t handled = framesHandled.load (std::memory_order_acquire);
    return static_cast<int> (framesSubmitted.load (std::memory_order_acquire) - handled);
}

// ---- audio thread -----------------------------------------------------------
void AsyncModelProcessor::process (const AudioBlock& block) noexcept FLUB_NONBLOCKING
{
    if (! prepared || latency == 0)
        return;

    const int channels = std::min (block.numChannels, numChannels);
    const int modelChannels = desc.numInputChannels;
    const float downmixScale = 1.0f / static_cast<float> (std::max (1, channels));
    const auto lineLength = static_cast<size_t> (latency);

    for (int start = 0; start < block.numSamples;)
    {
        const int n = std::min (block.numSamples - start, frameSize - framePos);
        for (int i = start; i < start + n; ++i)
        {
            // 1. Collect the model's input frame (before the sample is overwritten).
            const auto framePosition = static_cast<size_t> (framePos + (i - start));
            if (modelChannels == 1)
            {
                float sum = 0.0f;
                for (int c = 0; c < channels; ++c)
                    sum += block.channel (c)[i];
                stagingFrame[framePosition] = sum * downmixScale;
            }
            else
            {
                for (int m = 0; m < modelChannels; ++m)
                {
                    const int c = std::min (m, channels - 1);
                    stagingFrame[static_cast<size_t> (m) * static_cast<size_t> (frameSize) + framePosition] = c >= 0 ? block.channel (c)[i] : 0.0f;
                }
            }

            // 2. Advance the control ramps (one step per sample).
            for (size_t k = 0; k < gains.size(); ++k)
                gains[k] = smoothers[k].next();

            // 3. Delay by exactly `latency` and apply the controls.
            const auto pos = static_cast<size_t> (delayPos);
            for (int c = 0; c < channels; ++c)
            {
                float* line = delayBuffer.data() + static_cast<size_t> (c) * lineLength;
                const float delayed = line[pos];
                line[pos] = block.channel (c)[i];
                block.channel (c)[i] = delayed * gains[static_cast<size_t> (std::min (c, activeControls - 1))];
            }
            for (int c = channels; c < numChannels; ++c)
                delayBuffer[static_cast<size_t> (c) * lineLength + pos] = 0.0f; // absent channels stay silent, not stale
            delayPos = delayPos + 1 == latency ? 0 : delayPos + 1;
        }
        framePos += n;
        start += n;

        if (framePos == frameSize)
        {
            // Input frame k + safetyFrames is complete at the very sample where
            // output frame k starts (latency is a whole number of frames).
            framePos = 0;
            if (modelActive)
            {
                submitFrame();
                if (framesSinceReset <= config.safetyFrames)
                    ++framesSinceReset;
                if (framesSinceReset > config.safetyFrames) // safetyFrames + 1 frames in: output frame 0 starts
                    consumeResult();
            }
        }
    }
}

void AsyncModelProcessor::submitFrame() noexcept
{
    const uint64_t seq = nextSubmitSeq++;
    if (float* slot = inQueue.beginWrite())
    {
        std::memcpy (slot, stagingFrame.data(), sizeof (float) * static_cast<size_t> (inputFloats));
        FrameQueue::Header h;
        h.seq = seq;
        h.discontinuity = discontinuity;
        inQueue.commitWrite (h);
        discontinuity = false;
        framesSubmitted.fetch_add (1, std::memory_order_relaxed);
    }
    // A full queue drops the frame (the worker is stalled); its boundary then
    // counts as a deadline miss. The discontinuity flag waits for a frame that
    // does get through.
}

void AsyncModelProcessor::consumeResult() noexcept
{
    bool arrived = false, good = false;
    FrameQueue::Header h;
    while (const float* result = outQueue.peek (h))
    {
        if (h.seq < neededSeq)
        {
            outQueue.release(); // late result of a frame that already missed
            continue;
        }
        if (h.seq == neededSeq)
        {
            arrived = true;
            good = h.ok;
            if (good)
                setTargets (result);
            outQueue.release();
        }
        break; // a newer result (this frame was dropped or skipped) stays queued
    }

    if (good)
    {
        consecutiveBad = 0;
    }
    else
    {
        (arrived ? modelFailures : deadlineMisses).fetch_add (1, std::memory_order_relaxed);
        // Hold the last good controls (the current targets); from the K-th bad
        // frame in a row ramp to neutral.
        if (consecutiveBad < config.fallbackAfterFrames && ++consecutiveBad == config.fallbackAfterFrames)
            for (auto& sm : smoothers)
                sm.setTarget (1.0f);
    }

    ++neededSeq;
    firstUsefulSeq.store (neededSeq, std::memory_order_release);
}

void AsyncModelProcessor::setTargets (const float* controls) noexcept
{
    for (size_t k = 0; k < smoothers.size(); ++k)
        smoothers[k].setTarget (controls[k]);
}

// ---- worker thread ----------------------------------------------------------
void AsyncModelProcessor::startWorker()
{
    stopRequested.store (false, std::memory_order_relaxed);
    worker = std::thread ([this] { workerLoop(); });
}

void AsyncModelProcessor::stopWorker() noexcept
{
    if (! worker.joinable())
        return;
    stopRequested.store (true, std::memory_order_release);
    worker.join();
}

void AsyncModelProcessor::workerLoop() noexcept
{
    const auto numControls = static_cast<size_t> (desc.numControls);
    bool resetPending = false;
    while (! stopRequested.load (std::memory_order_acquire))
    {
        FrameQueue::Header in;
        const float* frame = inQueue.peek (in);
        if (frame == nullptr)
        {
            std::this_thread::sleep_for (std::chrono::microseconds (pollMicroseconds));
            continue;
        }
        resetPending = resetPending || in.discontinuity;

        if (in.seq >= firstUsefulSeq.load (std::memory_order_acquire))
        {
            if (resetPending)
            {
                try
                {
                    runner->reset();
                }
                catch (...)
                {
                }
                resetPending = false;
            }
            bool ok = false;
            try
            {
                ok = runner->run (frame, workerControls.data());
            }
            catch (...) // e.g. Ort::Exception: a failed frame, not a dead worker
            {
                ok = false;
            }
            framesProcessed.fetch_add (1, std::memory_order_relaxed);
            for (size_t k = 0; k < numControls && ok; ++k)
            {
                ok = std::isfinite (workerControls[k]);
                workerControls[k] = std::clamp (workerControls[k], 0.0f, config.maxGain);
            }

            // Drop the result if the queue is full: the audio thread has not
            // consumed for a while (stopped stream), and it would be stale.
            if (float* slot = outQueue.beginWrite())
            {
                std::memcpy (slot, workerControls.data(), sizeof (float) * numControls);
                FrameQueue::Header out;
                out.seq = in.seq;
                out.ok = ok;
                outQueue.commitWrite (out);
            }
        }
        inQueue.release();
        framesHandled.fetch_add (1, std::memory_order_release); // after the result is published
    }
}
} // namespace flub
