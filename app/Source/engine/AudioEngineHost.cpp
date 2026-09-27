#include "AudioEngineHost.h"

#include "flub/common/Denormals.h"
#include "platform/PlatformBridge.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace flub::app
{
namespace
{
int sanitiseChannels (int channels) noexcept
{
    return std::clamp (channels, 2, flub::kMaxChannels);
}

double samplesToMs (int samples, double sampleRate) noexcept
{
    return sampleRate > 0.0 ? 1000.0 * static_cast<double> (samples) / sampleRate : 0.0;
}
} // namespace

// =============================================================================
std::vector<flub::StripConfig> AudioEngineHost::defaultStripLayout()
{
    std::vector<flub::StripConfig> strips (4);
    strips[0].name = "Game";
    strips[0].inputChannels = 8; // 7.1: FL FR FC LFE BL BR SL SR
    strips[1].name = "Music";
    strips[1].inputChannels = 2;
    strips[2].name = "Chat";
    strips[2].inputChannels = 2;
    strips[3].name = "System";
    strips[3].inputChannels = 2;
    return strips;
}

AudioEngineHost::AudioEngineHost()
{
    layout = defaultStripLayout();
    for (size_t i = 0; i < kMaxStrips; ++i)
    {
        stripGainDb[i].store (i < layout.size() ? layout[i].gainDb : 0.0f, std::memory_order_relaxed);
        stripMuted[i].store (i < layout.size() && layout[i].muted, std::memory_order_relaxed);
        stripActive[i].store (false, std::memory_order_relaxed);
        deviceInputFirst[i].store (-1, std::memory_order_relaxed);
    }

    // Strips and parameter stores must exist before anything else (UI, preset
    // restore) touches the engine, so configure once for a nominal format.
    configureEngine (currentSampleRate, currentBlockSize);

    // Structural parameter changes (latency profile) are picked up here.
    startTimerHz (5);
}

AudioEngineHost::~AudioEngineHost()
{
    stopTimer();
    cancelPendingUpdate();
    closeDevice();
    stopAllCaptures();
}

// =============================================================================
// Device
// =============================================================================
juce::String AudioEngineHost::openDevice (const juce::XmlElement* savedState, int maxInputChannels, int maxOutputChannels)
{
    JUCE_ASSERT_MESSAGE_THREAD
    closeDevice();

   #if JUCE_WINDOWS
    // Nothing saved (first run, or the user never changed the device): prefer
    // JUCE's IAudioClient3 low-latency shared mode to the default "Windows
    // Audio" type. A saved choice always wins; if the device cannot be opened
    // in that mode the default type is used.
    juce::String fallbackType;
    if (savedState == nullptr)
    {
        const juce::String lowLatencyType ("Windows Audio (Low Latency Mode)");
        for (auto* type : deviceManager.getAvailableDeviceTypes())
        {
            if (type->getTypeName() == lowLatencyType && deviceManager.getCurrentAudioDeviceType() != lowLatencyType)
            {
                fallbackType = deviceManager.getCurrentAudioDeviceType();
                deviceManager.setCurrentAudioDeviceType (lowLatencyType, false); // not "chosen": nothing gets persisted
                break;
            }
        }
    }
   #endif

    auto error = deviceManager.initialise (maxInputChannels, maxOutputChannels, savedState, true);

   #if JUCE_WINDOWS
    if (fallbackType.isNotEmpty() && (error.isNotEmpty() || deviceManager.getCurrentAudioDevice() == nullptr))
    {
        deviceManager.setCurrentAudioDeviceType (fallbackType, false);
        error = deviceManager.initialise (maxInputChannels, maxOutputChannels, nullptr, true);
    }
   #endif

    // Attaching triggers audioDeviceAboutToStart -> configureEngine for the
    // device's real sample rate / block size.
    deviceManager.addAudioCallback (this);
    callbackAttached = true;

    if (error.isEmpty() && deviceManager.getCurrentAudioDevice() == nullptr)
        error = "No audio output device could be opened";

    return error;
}

void AudioEngineHost::closeDevice()
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (callbackAttached)
    {
        deviceManager.removeAudioCallback (this);
        callbackAttached = false;
    }
    deviceManager.closeAudioDevice();
    engineReady.store (false, std::memory_order_release);
    callbackRunning.store (false, std::memory_order_release);
    deviceInputLatency = deviceOutputLatency = 0;
}

// =============================================================================
// Structure
// =============================================================================
void AudioEngineHost::setStripLayout (std::vector<flub::StripConfig> newLayout)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (newLayout.empty())
        newLayout = defaultStripLayout();
    if (newLayout.size() > static_cast<size_t> (kMaxStrips))
        newLayout.resize (static_cast<size_t> (kMaxStrips));

    for (auto& s : newLayout)
        s.inputChannels = sanitiseChannels (s.inputChannels);

    // Captures feeding strips that no longer exist are stopped.
    for (auto& slot : captureSlots)
        if (slot.capture != nullptr && slot.strip.load (std::memory_order_relaxed) >= static_cast<int> (newLayout.size()))
            releaseSlot (slot);

    for (size_t i = newLayout.size(); i < static_cast<size_t> (kMaxStrips); ++i)
        deviceInputFirst[i].store (-1);

    layout = std::move (newLayout);
    for (size_t i = 0; i < layout.size(); ++i)
    {
        stripGainDb[i].store (layout[i].gainDb, std::memory_order_relaxed);
        stripMuted[i].store (layout[i].muted, std::memory_order_relaxed);
    }

    reconfigure();
}

void AudioEngineHost::reconfigure()
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (callbackAttached && deviceManager.getCurrentAudioDevice() != nullptr)
    {
        // Explicit, brief dropout: detach (JUCE calls audioDeviceStopped), then
        // re-attach (JUCE calls audioDeviceAboutToStart on this thread, which
        // re-configures the engine before the first new callback).
        deviceManager.removeAudioCallback (this);
        deviceManager.addAudioCallback (this);
    }
    else
    {
        configureEngine (currentSampleRate, currentBlockSize);
    }
}

void AudioEngineHost::configureEngine (double sampleRate, int blockSize)
{
    JUCE_ASSERT_MESSAGE_THREAD
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    currentBlockSize = std::clamp (blockSize > 0 ? blockSize : 512, 16, 16384);
    maxBlock = currentBlockSize;

    std::vector<flub::StripConfig> configs = layout;
    for (size_t i = 0; i < configs.size(); ++i)
    {
        configs[i].inputChannels = sanitiseChannels (configs[i].inputChannels);
        configs[i].gainDb = stripGainDb[i].load (std::memory_order_relaxed);
        configs[i].muted = stripMuted[i].load (std::memory_order_relaxed);
    }

    mixEngine.configure (configs, currentSampleRate, maxBlock);
    numStripsConfigured = mixEngine.getNumStrips();

    for (size_t i = 0; i < static_cast<size_t> (kMaxStrips); ++i)
    {
        const bool used = static_cast<int> (i) < numStripsConfigured;
        stripBuffers[i].setSize (used ? configs[i].inputChannels : 2, maxBlock);
        stripBlocks[i] = {};
        stripInputs[i] = nullptr;
        hangoverRemaining[i] = 0;
        appliedGainDb[i] = used ? configs[i].gainDb : 0.0f;
        appliedMuted[i] = used && configs[i].muted;
        stripActive[i].store (false, std::memory_order_relaxed);
    }

    mixOutput.setSize (2, maxBlock);
    appliedCeilingDb = masterCeilingDb.load (std::memory_order_relaxed);
    mixEngine.setMasterCeilingDb (appliedCeilingDb);
    hangoverSamples = static_cast<int> (std::lround (0.5 * currentSampleRate));

    // Captures keep their own clock/rate; only the consumer side follows the
    // device. Captures on strips that vanished are parked (not consumed).
    for (auto& slot : captureSlots)
    {
        if (slot.capture == nullptr)
            continue;
        if (slot.strip.load (std::memory_order_relaxed) >= numStripsConfigured)
            slot.strip.store (-1, std::memory_order_relaxed);
        slot.fifo.setConsumerFormat (currentSampleRate, maxBlock);
    }

    structureGeneration.fetch_add (1, std::memory_order_acq_rel);

    // Listeners are notified asynchronously: this may run inside JUCE's
    // audioDeviceAboutToStart (under the device manager's callback lock).
    notifyPending.store (true, std::memory_order_release);
    triggerAsyncUpdate();
}

void AudioEngineHost::timerCallback()
{
    if (mixEngine.needsReprepare())
        reconfigure();
}

void AudioEngineHost::handleAsyncUpdate()
{
    if (configurePending.exchange (false, std::memory_order_acq_rel))
    {
        // Rare path: the backend started the device from a non-message thread.
        // The callback has been producing silence until now.
        if (callbackRunning.load (std::memory_order_acquire) && ! engineReady.load (std::memory_order_acquire))
        {
            deviceInputLatency = pendingInputLatency;
            deviceOutputLatency = pendingOutputLatency;
            configureEngine (pendingSampleRate, pendingBlockSize);
            engineReady.store (true, std::memory_order_release);
        }
    }

    if (errorPending.exchange (false, std::memory_order_acq_rel))
    {
        juce::String message;
        {
            const juce::ScopedLock sl (errorLock);
            message = lastDeviceError;
        }
        if (onDeviceError != nullptr)
            onDeviceError (message);
    }

    if (notifyPending.exchange (false, std::memory_order_acq_rel) && onEngineConfigured != nullptr)
        onEngineConfigured();
}

// =============================================================================
// Mix controls
// =============================================================================
void AudioEngineHost::setStripGainDb (int strip, float gainDb) noexcept
{
    if (strip >= 0 && strip < kMaxStrips)
        stripGainDb[static_cast<size_t> (strip)].store (std::clamp (gainDb, -60.0f, 12.0f), std::memory_order_relaxed);
}

float AudioEngineHost::getStripGainDb (int strip) const noexcept
{
    return strip >= 0 && strip < kMaxStrips ? stripGainDb[static_cast<size_t> (strip)].load (std::memory_order_relaxed) : 0.0f;
}

void AudioEngineHost::setStripMuted (int strip, bool muted) noexcept
{
    if (strip >= 0 && strip < kMaxStrips)
        stripMuted[static_cast<size_t> (strip)].store (muted, std::memory_order_relaxed);
}

bool AudioEngineHost::isStripMuted (int strip) const noexcept
{
    return strip >= 0 && strip < kMaxStrips && stripMuted[static_cast<size_t> (strip)].load (std::memory_order_relaxed);
}

void AudioEngineHost::setMasterCeilingDb (float db) noexcept
{
    masterCeilingDb.store (std::clamp (db, -12.0f, 0.0f), std::memory_order_relaxed);
}

bool AudioEngineHost::isStripActive (int strip) const noexcept
{
    return strip >= 0 && strip < kMaxStrips && stripActive[static_cast<size_t> (strip)].load (std::memory_order_relaxed);
}

std::vector<flub::StripConfig> AudioEngineHost::getStripLayout() const
{
    auto result = layout;
    for (size_t i = 0; i < result.size(); ++i)
    {
        result[i].gainDb = stripGainDb[i].load (std::memory_order_relaxed);
        result[i].muted = stripMuted[i].load (std::memory_order_relaxed);
    }
    return result;
}

void AudioEngineHost::applyPendingMixSettings() noexcept
{
    for (int i = 0; i < numStripsConfigured; ++i)
    {
        const auto idx = static_cast<size_t> (i);
        const float g = stripGainDb[idx].load (std::memory_order_relaxed);
        if (g != appliedGainDb[idx])
        {
            mixEngine.setStripGainDb (i, g);
            appliedGainDb[idx] = g;
        }
        const bool m = stripMuted[idx].load (std::memory_order_relaxed);
        if (m != appliedMuted[idx])
        {
            mixEngine.setStripMuted (i, m);
            appliedMuted[idx] = m;
        }
    }

    const float ceiling = masterCeilingDb.load (std::memory_order_relaxed);
    if (ceiling != appliedCeilingDb)
    {
        mixEngine.setMasterCeilingDb (ceiling);
        appliedCeilingDb = ceiling;
    }
}

// =============================================================================
// Sources
// =============================================================================
void AudioEngineHost::setDeviceInputMap (const std::array<int, kMaxStrips>& firstChannels) noexcept
{
    for (size_t i = 0; i < static_cast<size_t> (kMaxStrips); ++i)
        deviceInputFirst[i].store (firstChannels[i] >= 0 ? firstChannels[i] : -1, std::memory_order_relaxed);
}

std::array<int, AudioEngineHost::kMaxStrips> AudioEngineHost::getDeviceInputMap() const noexcept
{
    std::array<int, kMaxStrips> map {};
    for (size_t i = 0; i < static_cast<size_t> (kMaxStrips); ++i)
        map[i] = deviceInputFirst[i].load (std::memory_order_relaxed);
    return map;
}

void AudioEngineHost::setDeviceInputRouting (int strip, int firstDeviceChannel) noexcept
{
    std::array<int, kMaxStrips> map;
    map.fill (-1);
    if (strip >= 0 && strip < kMaxStrips)
        map[static_cast<size_t> (strip)] = std::max (0, firstDeviceChannel);
    setDeviceInputMap (map);
}

int AudioEngineHost::getDeviceInputStrip() const noexcept
{
    for (int i = 0; i < kMaxStrips; ++i)
        if (deviceInputFirst[static_cast<size_t> (i)].load (std::memory_order_relaxed) >= 0)
            return i;
    return -1;
}

void AudioEngineHost::setCaptureFactory (CaptureFactory factory)
{
    JUCE_ASSERT_MESSAGE_THREAD
    captureFactory = std::move (factory);
}

int AudioEngineHost::startProcessCapture (int strip, uint32_t processId, juce::String& error)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (strip < 0 || strip >= numStripsConfigured)
    {
        error = "Invalid strip";
        return -1;
    }

    int slotIndex = -1;
    for (int i = 0; i < kMaxCaptures; ++i)
    {
        const auto& s = captureSlots[static_cast<size_t> (i)];
        if (s.capture == nullptr && ! s.live.load (std::memory_order_acquire))
        {
            slotIndex = i;
            break;
        }
    }
    if (slotIndex < 0)
    {
        error = "Too many applications are being captured (maximum " + juce::String (kMaxCaptures) + ")";
        return -1;
    }

    auto capture = captureFactory != nullptr ? captureFactory() : platform_bridge::createProcessLoopbackCapture();
    if (capture == nullptr || ! capture->isSupported())
    {
        error = "Per-application capture is not supported on this system";
        return -1;
    }

    auto& slot = captureSlots[static_cast<size_t> (slotIndex)];
    const int channels = sanitiseChannels (layout[static_cast<size_t> (strip)].inputChannels);

    // Request the device rate so the FIFO's nominal ratio is 1 and the
    // resampler only has to absorb clock drift.
    const double captureRate = currentSampleRate;
    slot.fifo.prepare (channels, captureRate, currentSampleRate, maxBlock);

    DriftCompensatedFifo* fifo = &slot.fifo;
    std::string startError;
    const bool ok = capture->start (processId, true, captureRate, channels,
                                    [fifo] (const float* interleaved, int numFrames, int numChannels)
                                    { fifo->push (interleaved, numFrames, numChannels); },
                                    startError);
    if (! ok)
    {
        error = startError.empty() ? juce::String ("Could not start the capture") : juce::String (startError);
        return -1;
    }

    slot.capture = std::move (capture);
    slot.processId = processId;
    slot.strip.store (strip, std::memory_order_relaxed);
    slot.live.store (true, std::memory_order_release);
    return slotIndex;
}

void AudioEngineHost::stopProcessCapture (int captureId)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (captureId >= 0 && captureId < kMaxCaptures)
        releaseSlot (captureSlots[static_cast<size_t> (captureId)]);
}

void AudioEngineHost::stopAllCaptures()
{
    for (auto& slot : captureSlots)
        if (slot.capture != nullptr || slot.live.load())
            releaseSlot (slot);
}

void AudioEngineHost::setCaptureStrip (int captureId, int strip)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (captureId < 0 || captureId >= kMaxCaptures)
        return;
    captureSlots[static_cast<size_t> (captureId)].strip.store (strip >= 0 && strip < numStripsConfigured ? strip : -1,
                                                               std::memory_order_relaxed);
}

void AudioEngineHost::releaseSlot (CaptureSlot& slot)
{
    // 1. The audio thread stops reading the FIFO from its next block on.
    slot.live.store (false, std::memory_order_seq_cst);

    // 2. Stop the producer (joins the capture thread).
    if (slot.capture != nullptr)
    {
        slot.capture->stop();
        slot.capture.reset();
    }

    // 3. Make sure a block that was already reading the FIFO has finished
    //    before the slot can be re-prepared (re-allocated) by a new capture.
    waitForAudioThreadToPass();

    slot.strip.store (-1, std::memory_order_relaxed);
    slot.processId = 0;
}

void AudioEngineHost::waitForAudioThreadToPass()
{
    if (! callbackRunning.load (std::memory_order_acquire))
        return;

    // callbackCounter increments at the END of every callback, so once it has
    // changed, any callback that could have seen the old state has returned.
    // This is a store(live) -> load(counter) / load(live) -> rmw(counter)
    // handshake (Dekker style): all four operations are seq_cst, otherwise the
    // StoreLoad reordering allowed by acquire/release could let this thread
    // read a counter value older than the callback that still sees live == true.
    const auto start = callbackCounter.load (std::memory_order_seq_cst);
    const auto deadline = juce::Time::getMillisecondCounter() + 250;
    while (callbackRunning.load (std::memory_order_acquire) && callbackCounter.load (std::memory_order_seq_cst) == start
           && juce::Time::getMillisecondCounter() < deadline)
        juce::Thread::sleep (1);
}

std::vector<AudioEngineHost::CaptureInfo> AudioEngineHost::getCaptures() const
{
    std::vector<CaptureInfo> result;
    for (int i = 0; i < kMaxCaptures; ++i)
    {
        const auto& slot = captureSlots[static_cast<size_t> (i)];
        if (slot.capture == nullptr)
            continue;
        CaptureInfo info;
        info.id = i;
        info.strip = slot.strip.load (std::memory_order_relaxed);
        info.processId = slot.processId;
        info.running = slot.capture->isRunning();
        info.stats = slot.fifo.getStats();
        result.push_back (info);
    }
    return result;
}

// =============================================================================
// Offline
// =============================================================================
void AudioEngineHost::prepareOffline (double sampleRate, int blockSize)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (! callbackRunning.load());
    configureEngine (sampleRate, blockSize);
}

void AudioEngineHost::renderOffline (StripSignalSource& source, int numSamples, float* const* outputs, int numOutputs)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (callbackRunning.load (std::memory_order_acquire))
    {
        jassertfalse; // the device owns the engine while it is running
        return;
    }

    flub::ScopedNoDenormals noDenormals;
    processBlock (nullptr, 0, outputs, outputs != nullptr ? numOutputs : 0, numSamples, &source);
}

// =============================================================================
// Audio thread
// =============================================================================
void AudioEngineHost::processBlock (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                                    int numSamples, StripSignalSource* offlineSource) noexcept
{
    applyPendingMixSettings();

    for (int pos = 0; pos < numSamples;)
    {
        const int n = std::min (numSamples - pos, maxBlock);

        // ---- 1. Gather strip inputs into the preallocated strip buffers ----
        for (int s = 0; s < numStripsConfigured; ++s)
        {
            const auto si = static_cast<size_t> (s);
            auto& buffer = stripBuffers[si];
            const int channels = buffer.getNumChannels();
            const flub::AudioBlock block = buffer.block (channels, n);
            bool fed = false;

            if (offlineSource != nullptr)
            {
                fed = offlineSource->renderStrip (s, block);
            }
            else
            {
                const int firstInput = deviceInputFirst[si].load (std::memory_order_relaxed);
                if (inputs != nullptr && firstInput >= 0 && firstInput < numInputs)
                {
                    for (int c = 0; c < channels; ++c)
                    {
                        const int src = firstInput + c;
                        if (src < numInputs && inputs[src] != nullptr)
                            std::memcpy (block.channel (c), inputs[src] + pos, sizeof (float) * static_cast<size_t> (n));
                        else
                            std::memset (block.channel (c), 0, sizeof (float) * static_cast<size_t> (n));
                    }
                    fed = true;
                }

                for (auto& slot : captureSlots)
                {
                    // seq_cst: pairs with releaseSlot() / waitForAudioThreadToPass().
                    if (! slot.live.load (std::memory_order_seq_cst) || slot.strip.load (std::memory_order_relaxed) != s)
                        continue;
                    slot.fifo.pull (block.ch.data(), channels, n, fed);
                    fed = true;
                }
            }

            if (fed)
            {
                hangoverRemaining[si] = hangoverSamples;
            }
            else if (hangoverRemaining[si] > 0)
            {
                // Keep processing silence for a moment so reverb / virtualiser
                // tails and dynamics release decay naturally.
                block.clear();
                hangoverRemaining[si] -= n;
                fed = true;
            }

            stripBlocks[si] = block;
            stripInputs[si] = fed ? &stripBlocks[si] : nullptr;
            stripActive[si].store (fed, std::memory_order_relaxed);
        }

        // ---- 2. Mix ------------------------------------------------------------
        const flub::AudioBlock mix = mixOutput.block (2, n);
        if (numStripsConfigured > 0)
            mixEngine.process (stripInputs.data(), mix);
        else
            mix.clear();

        // ---- 3. Device outputs (stereo engine; extra channels silent) -----------
        for (int c = 0; c < numOutputs; ++c)
        {
            float* d = outputs[c];
            if (d == nullptr)
                continue;
            d += pos;
            if (numOutputs == 1)
            {
                const float* l = mix.channel (0);
                const float* r = mix.channel (1);
                for (int k = 0; k < n; ++k)
                    d[k] = 0.5f * (l[k] + r[k]);
            }
            else if (c < 2)
            {
                std::memcpy (d, mix.channel (c), sizeof (float) * static_cast<size_t> (n));
            }
            else
            {
                std::memset (d, 0, sizeof (float) * static_cast<size_t> (n));
            }
        }

        pos += n;
    }
}

void AudioEngineHost::audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                                        float* const* outputChannelData, int numOutputChannels, int numSamples,
                                                        const juce::AudioIODeviceCallbackContext&)
{
    flub::ScopedNoDenormals noDenormals;

    // Promote the device thread once (MMCSS "Pro Audio", time constraint,
    // SCHED_FIFO; threads that are already real-time are left alone). Backends
    // may use a new thread after a restart, so this is tracked per thread. The
    // handle is intentionally never reverted: revert must run on this thread,
    // and the backend's thread dies with the device. (On Linux the handle is a
    // few bytes of saved policy allocated once per new device thread.)
    if (const auto thisThread = juce::Thread::getCurrentThreadId(); thisThread != promotedThread)
    {
        promotionHandle = platform_bridge::promoteAudioThread();
        promotedThread = thisThread;
    }

    if (engineReady.load (std::memory_order_acquire))
    {
        processBlock (inputChannelData, numInputChannels, outputChannelData, numOutputChannels, numSamples, nullptr);
    }
    else
    {
        for (int c = 0; c < numOutputChannels; ++c)
            if (outputChannelData[c] != nullptr)
                std::memset (outputChannelData[c], 0, sizeof (float) * static_cast<size_t> (numSamples));
    }

    callbackCounter.fetch_add (1, std::memory_order_seq_cst);
}

void AudioEngineHost::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const double sampleRate = device->getCurrentSampleRate();
    const int blockSize = device->getCurrentBufferSizeSamples();

    // Must be visible before any async configure request is handled.
    callbackRunning.store (true, std::memory_order_release);

    if (juce::MessageManager::existsAndIsCurrentThread())
    {
        configurePending.store (false, std::memory_order_release);
        deviceInputLatency = device->getInputLatencyInSamples();
        deviceOutputLatency = device->getOutputLatencyInSamples();
        configureEngine (sampleRate, blockSize);
        engineReady.store (true, std::memory_order_release);
    }
    else
    {
        // Never touch the engine structure off the message thread: run silent
        // until the message thread has configured it (handleAsyncUpdate).
        engineReady.store (false, std::memory_order_release);
        pendingSampleRate = sampleRate;
        pendingBlockSize = blockSize;
        pendingInputLatency = device->getInputLatencyInSamples();
        pendingOutputLatency = device->getOutputLatencyInSamples();
        configurePending.store (true, std::memory_order_release);
        triggerAsyncUpdate();
    }
}

void AudioEngineHost::audioDeviceStopped()
{
    engineReady.store (false, std::memory_order_release);
    callbackRunning.store (false, std::memory_order_release);
    configurePending.store (false, std::memory_order_release);
}

void AudioEngineHost::audioDeviceError (const juce::String& errorMessage)
{
    // May be called from any thread (never from inside our processing).
    {
        const juce::ScopedLock sl (errorLock);
        lastDeviceError = errorMessage;
    }
    errorPending.store (true, std::memory_order_release);
    triggerAsyncUpdate();
}

juce::String AudioEngineHost::getLastDeviceError() const
{
    const juce::ScopedLock sl (errorLock);
    return lastDeviceError;
}

// =============================================================================
// Telemetry
// =============================================================================
LatencyInfo AudioEngineHost::getLatencyInfo() const
{
    LatencyInfo info;
    info.sampleRate = currentSampleRate;
    info.blockSize = currentBlockSize;
    const bool deviceOpen = deviceManager.getCurrentAudioDevice() != nullptr;
    info.deviceInputSamples = deviceOpen ? deviceInputLatency : 0;
    info.deviceOutputSamples = deviceOpen ? deviceOutputLatency : 0;
    info.engineSamples = mixEngine.getLatencySamples();
    info.deviceInputMs = samplesToMs (info.deviceInputSamples, currentSampleRate);
    info.deviceOutputMs = samplesToMs (info.deviceOutputSamples, currentSampleRate);
    info.engineMs = samplesToMs (info.engineSamples, currentSampleRate);

    for (const auto& slot : captureSlots)
        if (slot.capture != nullptr)
            info.captureBufferMs = std::max (info.captureBufferMs, static_cast<double> (slot.fifo.getStats().targetMs));

    info.totalMs = info.deviceInputMs + info.deviceOutputMs + info.engineMs;
    return info;
}

EngineStatus AudioEngineHost::getStatus() const
{
    EngineStatus st;
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        st.deviceOpen = true;
        st.deviceName = device->getName();
        st.deviceTypeName = device->getTypeName();
        st.numInputChannels = device->getActiveInputChannels().countNumberOfSetBits();
        st.numOutputChannels = device->getActiveOutputChannels().countNumberOfSetBits();
        st.xruns = device->getXRunCount();
        st.glitches = deviceManager.getXRunCount();
        st.cpuLoad = deviceManager.getCpuUsage();
    }
    st.running = callbackRunning.load (std::memory_order_acquire) && engineReady.load (std::memory_order_acquire);
    st.callbacks = callbackCounter.load (std::memory_order_relaxed);
    return st;
}
} // namespace flub::app
