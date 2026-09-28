#include "AudioEngineHost.h"

#include "flub/common/Denormals.h"
#include "platform/PlatformBridge.h"

#include <algorithm>
#include <chrono>
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

// ALSA's 5.1 / 7.1 order is FL FR RL RR FC LFE [SL SR]; the engine's is FL FR
// FC LFE [BL BR] SL SR. Entry c: the ALSA channel that carries engine channel
// c (the same for 5.1, whose engine SL SR are ALSA's RL RR).
constexpr std::array<int, 8> kAlsaSurroundOrder { 0, 1, 4, 5, 2, 3, 6, 7 };

bool isAlsaDeviceType (const juce::String& typeName)
{
    return typeName == "ALSA" || typeName == "ALSA HW";
}

bool usesAlsaSurroundOrder (bool alsaDevice, int stripChannels) noexcept
{
    return alsaDevice && (stripChannels == 6 || stripChannels == 8);
}

// The capture fold's surround <-> stereo passthrough ramp: ProcessingChain's
// kPassFadeMs.
constexpr float kCaptureFoldRampMs = 400.0f;

uint64_t steadyNowNs() noexcept
{
    return static_cast<uint64_t> (
        std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ---- Loopback-pair table (docs/11 E51) ------------------------------------------
// Device names as the OS reports them, e.g. WASAPI friendly names
// "CABLE Output (VB-Audio Virtual Cable)". Matching is on exact partner
// pairs, never on a vendor token alone: a VB-Cable or Voicemeeter chain whose
// output and input are different buses is legitimate (docs/11 §6 T7).
bool hasVirtualDeviceToken (const juce::String& lower)
{
    static const char* const tokens[] = { "cable", "vb-audio", "voicemeeter", "blackhole", "soundflower", "loopback", "flubsound", "virtual" };
    for (const auto* t : tokens)
        if (lower.contains (t))
            return true;
    return false;
}

/** "(driver)" at the end of a friendly name, lower case; empty if none. */
juce::String driverSuffix (const juce::String& lower)
{
    if (! lower.endsWithChar (')') || ! lower.containsChar ('('))
        return {};
    return lower.fromLastOccurrenceOf ("(", false, false).dropLastCharacters (1).trim();
}

/** The name with the whole word `word` replaced by a marker; empty if the word is absent. */
juce::String withWordReplaced (const juce::String& lower, const char* word)
{
    auto tokens = juce::StringArray::fromTokens (lower, " ", {});
    bool found = false;
    for (auto& t : tokens)
    {
        if (t == word)
        {
            t = "\x01";
            found = true;
        }
    }
    return found ? tokens.joinIntoString (" ") : juce::String();
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
    configureEngine (currentSampleRate, currentBlockSize, false);

    // Structural parameter changes (latency profile) are picked up here, and
    // engines retired by a swap are destroyed.
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

    // A failed open is shown like a device error (a successful start cleared
    // any earlier one in audioDeviceAboutToStart).
    if (error.isNotEmpty() && safety.kind != DeviceSafetyState::Kind::LoopbackPair)
    {
        DeviceSafetyState next;
        next.kind = DeviceSafetyState::Kind::DeviceError;
        next.message = error;
        setSafetyState (next);
    }

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

    // No callback can run now: a swap in flight ends here, on the newest engine.
    if (latest != nullptr)
        replaceEngineNow (nullptr, false);
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
    {
        deviceInputFirst[i].store (-1);
        neuralModels[i] = {};
    }

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
    collectRetired();

    if (! callbackRunning.load (std::memory_order_acquire))
    {
        // Nothing is processing: replace the engine at once.
        configureEngine (currentSampleRate, currentBlockSize, false);
        return;
    }

    // The device is starting from another thread (the callback is silent until
    // handleAsyncUpdate configures, which uses the current layout anyway).
    if (! engineReady.load (std::memory_order_acquire))
        return;

    // The crossfaded swap: build the new engine here while the old one keeps
    // running, then hand it to the audio thread (see the THREADING CONTRACT).
    auto next = buildEngine();
    EngineInstance* raw = next.get();
    instances.push_back (std::move (next));
    latest = raw;
    if (auto* superseded = pendingSwap.exchange (raw, std::memory_order_acq_rel))
        dispose (superseded); // never reached the audio thread; its swap is replaced by this one
    else
        ++swapsRequested;

    afterStructureChange();
}

void AudioEngineHost::setNeuralModel (int strip, NeuralModelFactory factory, const flub::NeuralSlotConfig& config)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (strip < 0 || strip >= latest->numStrips)
    {
        jassertfalse;
        return;
    }
    neuralModels[static_cast<size_t> (strip)] = { std::move (factory), config };
    reconfigure();
}

bool AudioEngineHost::hasNeuralModel (int strip) const noexcept
{
    return strip >= 0 && strip < kMaxStrips && neuralModels[static_cast<size_t> (strip)].factory != nullptr;
}

bool AudioEngineHost::isSwapInProgress() const noexcept
{
    return swapsRequested != swapsCompleted.load (std::memory_order_acquire);
}

std::unique_ptr<AudioEngineHost::EngineInstance> AudioEngineHost::buildEngine()
{
    std::vector<flub::StripConfig> configs = layout;
    for (size_t i = 0; i < configs.size(); ++i)
    {
        configs[i].inputChannels = sanitiseChannels (configs[i].inputChannels);
        configs[i].gainDb = stripGainDb[i].load (std::memory_order_relaxed);
        configs[i].muted = stripMuted[i].load (std::memory_order_relaxed);
    }

    // Every engine gets its own runner for each strip's neural model, installed
    // before the chain's prepare(), so it is in the chain (and its latency)
    // from the first block.
    const auto installNeuralModel = [this] (int strip, flub::ProcessingChain& chain)
    {
        const auto& model = neuralModels[static_cast<size_t> (strip)];
        if (model.factory != nullptr)
            if (auto runner = model.factory())
                chain.setNeuralModel (std::move (runner), model.config);
    };

    auto next = std::make_unique<EngineInstance>();
    auto& engine = next->engine;
    // The new engine shares the newest engine's ParameterStores (profiles),
    // which that engine may still be using on the audio thread.
    if (latest != nullptr)
        engine.configureFrom (latest->engine, configs, currentSampleRate, currentBlockSize, installNeuralModel);
    else
        engine.configure (configs, currentSampleRate, currentBlockSize, installNeuralModel);

    engine.getDeviceCorrection().setSettingsNow (deviceCorrection); // not yet visible to the audio thread
    next->numStrips = engine.getNumStrips();
    next->maxBlock = currentBlockSize;
    for (size_t i = 0; i < static_cast<size_t> (kMaxStrips); ++i)
    {
        const bool used = static_cast<int> (i) < next->numStrips;
        next->stripBuffers[i].setSize (used ? configs[i].inputChannels : 2, next->maxBlock);
        next->appliedGainDb[i] = used ? configs[i].gainDb : 0.0f;
        next->appliedMuted[i] = used && configs[i].muted;
    }
    next->mixOutput.setSize (2, next->maxBlock);
    next->appliedCeilingDb = masterCeilingDb.load (std::memory_order_relaxed);
    engine.setMasterCeilingDb (next->appliedCeilingDb);

    // Swap timing: audible only once its delay lines hold real signal and its
    // detectors have seen some of it; raised-cosine fades (smooth at both ends).
    next->latencySamples = engine.getLatencySamples();
    next->prerollSamples = next->latencySamples + static_cast<int> (std::lround (kSwapSettleMs * 0.001 * currentSampleRate));
    const auto fadeLength = static_cast<size_t> (std::max (1L, std::lround (kSwapFadeMs * 0.001 * currentSampleRate)));
    next->fadeIn.resize (fadeLength);
    for (size_t k = 0; k < fadeLength; ++k)
        next->fadeIn[k] = static_cast<float> (0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * (static_cast<double> (k) + 0.5)
                                                                        / static_cast<double> (fadeLength)));
    return next;
}

void AudioEngineHost::configureEngine (double sampleRate, int blockSize, bool fadeIn)
{
    JUCE_ASSERT_MESSAGE_THREAD
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    currentBlockSize = std::clamp (blockSize > 0 ? blockSize : 512, 16, 16384);

    replaceEngineNow (buildEngine(), fadeIn);

    for (size_t i = 0; i < static_cast<size_t> (kMaxStrips); ++i)
    {
        hangoverRemaining[i] = 0;
        stripActive[i].store (false, std::memory_order_relaxed);
    }
    hangoverSamples = static_cast<int> (std::lround (0.5 * currentSampleRate));

    // Captures keep their own clock, and run at the device rate so the
    // FIFO's nominal ratio is 1 (its Hermite interpolator does not
    // band-limit: 48 kHz -> 16 kHz hands-free at a ratio of 3 would alias
    // 8-24 kHz into the band). The audio thread is not reading the FIFOs
    // here. Same rate: only the consumer side follows the device. New rate:
    // the capture is parked (not read) and restarted at the device rate on
    // the message thread outside the device start (a capture start can block
    // for a while; this may run under JUCE's device callback lock).
    for (auto& slot : captureSlots)
    {
        if (slot.capture == nullptr)
            continue;
        if (slot.restartPending || std::abs (slot.fifo.getProducerSampleRate() - currentSampleRate) > 0.5)
        {
            slot.live.store (false, std::memory_order_seq_cst);
            slot.restartPending = true;
            slot.restartAttempts = 0;
            captureRestartNeeded = true;
        }
        else
        {
            slot.fifo.setConsumerFormat (currentSampleRate, currentBlockSize);
            prepareCaptureFold (slot);
        }
    }

    afterStructureChange(); // triggers handleAsyncUpdate, which restarts parked captures
}

void AudioEngineHost::replaceEngineNow (std::unique_ptr<EngineInstance> next, bool fadeIn)
{
    // Only while the audio thread cannot touch the engines: no callback is
    // running, or it runs silent (engineReady == false).
    if (next != nullptr)
    {
        latest = next.get();
        instances.push_back (std::move (next));
    }

    // Everything but the newest engine goes: a pending swap never started, a
    // swap in flight ends on the newest engine.
    pendingSwap.store (nullptr, std::memory_order_relaxed);
    retiredSwap.store (nullptr, std::memory_order_relaxed);
    instances.erase (std::remove_if (instances.begin(), instances.end(), [this] (const auto& i) { return i.get() != latest; }),
                     instances.end());
    active = latest;
    fading = nullptr;
    swapsRequested = swapsCompleted.load (std::memory_order_relaxed);
    clearRemovedStrips();

    // After a device start the output fades in (no step from silence): the
    // start-up variant of a swap, without an old engine.
    swapRunning = fadeIn;
    swapCounts = false;
    swapPos = 0;
    swapFadeOutStart = swapOldEnd = 0;
    swapFadeInStart = active->latencySamples;
    swapEnd = swapFadeInStart + static_cast<int> (active->fadeIn.size());
}

void AudioEngineHost::afterStructureChange()
{
    // Captures on strips that vanished are parked (not consumed).
    for (auto& slot : captureSlots)
        if (slot.capture != nullptr && slot.strip.load (std::memory_order_relaxed) >= latest->numStrips)
            slot.strip.store (-1, std::memory_order_relaxed);

    structureGeneration.fetch_add (1, std::memory_order_acq_rel);

    // Listeners are notified asynchronously: this may run inside JUCE's
    // audioDeviceAboutToStart (under the device manager's callback lock).
    notifyPending.store (true, std::memory_order_release);
    triggerAsyncUpdate();
}

void AudioEngineHost::clearRemovedStrips() noexcept
{
    // Strips beyond the active engine's layout are no longer active (and no
    // old engine is left whose tail they would feed).
    for (int s = active->numStrips; s < kMaxStrips; ++s)
    {
        hangoverRemaining[static_cast<size_t> (s)] = 0;
        stripActive[static_cast<size_t> (s)].store (false, std::memory_order_relaxed);
    }
}

void AudioEngineHost::collectRetired()
{
    if (auto* retired = retiredSwap.exchange (nullptr, std::memory_order_acq_rel))
        dispose (retired);
}

void AudioEngineHost::dispose (EngineInstance* instance)
{
    jassert (instance != latest && instance != nullptr);
    instances.erase (std::remove_if (instances.begin(), instances.end(), [instance] (const auto& i) { return i.get() == instance; }),
                     instances.end());
}

void AudioEngineHost::timerCallback()
{
    collectRetired();
    if (latest->engine.needsReprepare())
        reconfigure();

    // A device correction the hand-off ring had no room for (the audio thread
    // did not run for several changes) is handed over again.
    latest->engine.getDeviceCorrection().retryPending();

    // The guard's banner state follows device input map changes (the audio
    // thread applies them at once).
    if (loopbackPair.load (std::memory_order_relaxed) || safety.kind == DeviceSafetyState::Kind::LoopbackPair)
        checkLoopbackPair (guardInputName, guardOutputName);

    // A capture restart at a new device rate that failed is retried at 1 Hz.
    if (captureRestartNeeded && juce::Time::getMillisecondCounter() - lastCaptureRestartMs >= 1000)
        restartCapturesAtDeviceRate();
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
            configureEngine (pendingSampleRate, pendingBlockSize, true);
            // The callback runs silent until engineReady (release) publishes
            // the guard's gain set here.
            applyDeviceStartSafety (deviceManager.getCurrentAudioDevice());
            engineReady.store (true, std::memory_order_release);
        }
    }

    if (captureRestartNeeded)
        restartCapturesAtDeviceRate();

    if (errorPending.exchange (false, std::memory_order_acq_rel))
    {
        juce::String message;
        {
            const juce::ScopedLock sl (errorLock);
            message = lastDeviceError;
        }
        if (safety.kind != DeviceSafetyState::Kind::LoopbackPair) // the muted output is the more urgent banner
        {
            DeviceSafetyState next;
            next.kind = DeviceSafetyState::Kind::DeviceError;
            next.message = message;
            if (auto* device = deviceManager.getCurrentAudioDevice())
                next.outputDeviceName = device->getName();
            setSafetyState (next);
        }
        if (onDeviceError != nullptr)
            onDeviceError (message);
    }

    if (notifyPending.exchange (false, std::memory_order_acq_rel) && onEngineConfigured != nullptr)
        onEngineConfigured();

    if (safetyNotifyPending.exchange (false, std::memory_order_acq_rel) && onDeviceSafetyChanged != nullptr)
        onDeviceSafetyChanged();
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

void AudioEngineHost::setDeviceCorrection (const flub::DeviceCorrectionSettings& settings)
{
    JUCE_ASSERT_MESSAGE_THREAD
    deviceCorrection = settings;
    auto& correction = latest->engine.getDeviceCorrection();
    // A running device: the audio thread (the only consumer) crossfades to it.
    // A pending swap's engine gets it the same way when the audio thread takes
    // it; the engine it fades out keeps the old curve for those few ms.
    if (callbackRunning.load (std::memory_order_acquire))
        correction.setSettings (settings);
    else
        correction.setSettingsNow (settings);
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

void AudioEngineHost::applyPendingMixSettings (EngineInstance& instance) noexcept
{
    auto& engine = instance.engine;
    for (int i = 0; i < instance.numStrips; ++i)
    {
        const auto idx = static_cast<size_t> (i);
        const float g = stripGainDb[idx].load (std::memory_order_relaxed);
        if (g != instance.appliedGainDb[idx])
        {
            engine.setStripGainDb (i, g);
            instance.appliedGainDb[idx] = g;
        }
        const bool m = stripMuted[idx].load (std::memory_order_relaxed);
        if (m != instance.appliedMuted[idx])
        {
            engine.setStripMuted (i, m);
            instance.appliedMuted[idx] = m;
        }
    }

    const float ceiling = masterCeilingDb.load (std::memory_order_relaxed);
    if (ceiling != instance.appliedCeilingDb)
    {
        engine.setMasterCeilingDb (ceiling);
        instance.appliedCeilingDb = ceiling;
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
    if (strip < 0 || strip >= latest->numStrips)
    {
        error = "Invalid strip";
        return -1;
    }

    int slotIndex = -1;
    for (int i = 0; i < kMaxCaptures; ++i)
    {
        auto& s = captureSlots[static_cast<size_t> (i)];
        if (s.capture == nullptr && ! s.live.load (std::memory_order_acquire) && ! isSlotQuarantined (s))
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
    slot.channels = sanitiseChannels (layout[static_cast<size_t> (strip)].inputChannels);

    std::string startError;
    if (! startCaptureInSlot (slot, *capture, processId, startError))
    {
        error = juce::String (startError);
        return -1;
    }

    slot.capture = std::move (capture);
    slot.processId = processId;
    slot.restartPending = false;
    slot.restartAttempts = 0;
    slot.restartError = {};
    slot.strip.store (strip, std::memory_order_relaxed);
    slot.live.store (true, std::memory_order_release);
    return slotIndex;
}

bool AudioEngineHost::startCaptureInSlot (CaptureSlot& slot, flub::platform::ProcessLoopbackCapture& capture, uint32_t processId,
                                          std::string& error)
{
    // Request the device rate so the FIFO's nominal ratio is 1 and the
    // resampler only has to absorb clock drift. The slot is not live and its
    // producer is stopped: the FIFO can be (re)allocated.
    slot.fifo.prepare (slot.channels, currentSampleRate, currentSampleRate, currentBlockSize);
    prepareCaptureFold (slot);

    DriftCompensatedFifo* fifo = &slot.fifo;
    const bool ok = capture.start (processId, true, currentSampleRate, slot.channels,
                                   [fifo] (const float* interleaved, int numFrames, int numChannels)
                                   { fifo->push (interleaved, numFrames, numChannels); },
                                   error);
    if (! ok && error.empty())
        error = "Could not start the capture";
    return ok;
}

void AudioEngineHost::prepareCaptureFold (CaptureSlot& slot)
{
    // Message thread, slot not read by the audio thread (not live, or the
    // device stopped). Only a surround FIFO can need the fold.
    const int channels = slot.fifo.getNumChannels();
    if (channels >= 6)
    {
        slot.foldScratch.setSize (channels, currentBlockSize);
        slot.fold.prepare (currentSampleRate, 0.0f); // the strip's LFE level is set per block
        slot.detector.prepare (currentSampleRate, channels);
        slot.foldGain.reset (currentSampleRate, kCaptureFoldRampMs, flub::Bs775Fold::kMatrixGain); // the detector starts on surround
    }
    else
    {
        slot.foldScratch.setSize (0, 0);
    }
}

void AudioEngineHost::pullCapture (CaptureSlot& slot, int strip, const flub::AudioBlock& block, bool addToBlock) noexcept
{
    const int fifoChannels = slot.fifo.getNumChannels();
    const int n = block.numSamples;
    if (block.numChannels != 2 || fifoChannels < 6 || slot.foldScratch.getNumChannels() != fifoChannels
        || n > slot.foldScratch.getNumSamples())
    {
        slot.fifo.pull (block.ch.data(), block.numChannels, n, addToBlock);
        return;
    }

    // A surround capture on a stereo strip (docs/11 E01): fold it as the
    // chain folds its own surround input - the BS.775 matrix with the LFE
    // low-passed at the strip's virt.lfe level, at the surround fold's -3 dB,
    // or at unity when the content is FL / FR only (E27: the same choice as
    // ProcessingChain's stereoFoldFor; both folds are the same matrix, so a
    // gain ramp between them is exact) - instead of keeping only FL / FR.
    const flub::AudioBlock scratch = slot.foldScratch.block (fifoChannels, n);
    slot.fifo.pull (scratch.ch.data(), fifoChannels, n, false);
    slot.detector.process (scratch);

    auto& params = active->engine.params (strip);
    const auto mode = static_cast<flub::param::InputModeValue> (std::lround (params.get (flub::param::VirtInputMode)));
    const bool stereoFold = mode == flub::param::InputModeValue::ForceStereo
                         || (mode != flub::param::InputModeValue::ForceSurround
                             && (params.get (flub::param::VirtOwnHrtf) >= 0.5f
                                 || slot.detector.getFold() == flub::ActiveChannelDetector::Fold::Stereo));
    slot.foldGain.setTarget (stereoFold ? 1.0f : flub::Bs775Fold::kMatrixGain);
    slot.fold.setLfeGain (flub::LfeFold::gainFor (params.get (flub::param::VirtLfeFold) >= 0.5f,
                                                  params.get (flub::param::VirtLfeGainDb)));
    slot.fold.process (scratch, 1.0f);

    float* l = block.channel (0);
    float* r = block.channel (1);
    const float* fl = scratch.channel (0);
    const float* fr = scratch.channel (1);
    for (int k = 0; k < n; ++k)
    {
        const float g = slot.foldGain.next();
        l[k] = (addToBlock ? l[k] : 0.0f) + g * fl[k];
        r[k] = (addToBlock ? r[k] : 0.0f) + g * fr[k];
    }
}

void AudioEngineHost::restartCapturesAtDeviceRate()
{
    JUCE_ASSERT_MESSAGE_THREAD
    captureRestartNeeded = false;
    lastCaptureRestartMs = juce::Time::getMillisecondCounter();

    for (auto& slot : captureSlots)
    {
        if (slot.capture == nullptr || ! slot.restartPending || slot.restartAttempts >= kMaxCaptureRestartAttempts)
            continue;

        // Parked by configureEngine while no callback could read it (live ==
        // false since), so the FIFO is free once the producer has stopped.
        slot.capture->stop();
        std::string error;
        if (startCaptureInSlot (slot, *slot.capture, slot.processId, error))
        {
            slot.restartPending = false;
            slot.restartAttempts = 0;
            slot.restartError = {};
            slot.live.store (true, std::memory_order_release);
        }
        else
        {
            // The capture stays stopped (getCaptures() reports running ==
            // false and the reason); retried by the timer.
            slot.restartError = juce::String (error);
            if (++slot.restartAttempts < kMaxCaptureRestartAttempts)
                captureRestartNeeded = true;
        }
    }
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
    captureSlots[static_cast<size_t> (captureId)].strip.store (strip >= 0 && strip < latest->numStrips ? strip : -1,
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
    slot.restartPending = false;
    slot.restartAttempts = 0;
    slot.restartError = {};

    // 3. Make sure a block that was already reading the FIFO has finished
    //    before the slot can be re-prepared (re-allocated) by a new capture.
    //    If the callback is stalled (driver hang, debugger, suspend), the
    //    slot is quarantined instead: startProcessCapture skips it until a
    //    callback has completed, so a stalled pull() never reads a FIFO that
    //    was re-allocated under it.
    uint64_t start = 0;
    if (! waitForAudioThreadToPass (start))
    {
        slot.quarantined = true;
        slot.quarantineCounter = start;
    }

    slot.strip.store (-1, std::memory_order_relaxed);
    slot.processId = 0;
}

bool AudioEngineHost::isSlotQuarantined (CaptureSlot& slot) noexcept
{
    if (! slot.quarantined)
        return false;
    // Still the stalled callback (or no callback since): not reusable. Once
    // the counter moved, or the device stopped (the backend joined its
    // thread), the reader that might have seen live == true has returned.
    if (callbackRunning.load (std::memory_order_acquire) && callbackCounter.load (std::memory_order_seq_cst) == slot.quarantineCounter)
        return true;
    slot.quarantined = false;
    return false;
}

bool AudioEngineHost::waitForAudioThreadToPass (uint64_t& startCounter)
{
    startCounter = callbackCounter.load (std::memory_order_seq_cst);
    if (! callbackRunning.load (std::memory_order_acquire))
        return true;

    // callbackCounter increments at the END of every callback, so once it has
    // changed, any callback that could have seen the old state has returned.
    // This is a store(live) -> load(counter) / load(live) -> rmw(counter)
    // handshake (Dekker style): all four operations are seq_cst, otherwise the
    // StoreLoad reordering allowed by acquire/release could let this thread
    // read a counter value older than the callback that still sees live == true.
    //
    // The wait is bounded so a wedged device never hangs the message thread;
    // the caller quarantines what it could not hand off. A second release
    // while the same callback is still stalled does not wait again.
    if (lastWaitTimedOut && startCounter == lastTimedOutCounter)
        return false;

    const auto begin = juce::Time::getMillisecondCounter();
    while (callbackRunning.load (std::memory_order_acquire) && callbackCounter.load (std::memory_order_seq_cst) == startCounter)
    {
        if (juce::Time::getMillisecondCounter() - begin >= kAudioThreadPassTimeoutMs)
        {
            lastWaitTimedOut = true;
            lastTimedOutCounter = startCounter;
            return false;
        }
        juce::Thread::sleep (1);
    }
    lastWaitTimedOut = false;
    return true;
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
        info.running = slot.capture->isRunning() && ! slot.restartPending;
        info.sampleRate = slot.fifo.getProducerSampleRate();
        info.restartError = slot.restartError;
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
    configureEngine (sampleRate, blockSize, false);
}

void AudioEngineHost::renderOffline (StripSignalSource& source, int numSamples, float* const* outputs, int numOutputs)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (callbackRunning.load (std::memory_order_acquire))
    {
        jassertfalse; // the device owns the engine while it is running
        return;
    }

    // Offline rendering never crossfades: a swap or start-up fade left over
    // from a stopped device ends here, on the newest engine.
    if (swapRunning || fading != nullptr || pendingSwap.load (std::memory_order_acquire) != nullptr)
        replaceEngineNow (nullptr, false);

    flub::ScopedNoDenormals noDenormals;
    processBlock (nullptr, 0, outputs, outputs != nullptr ? numOutputs : 0, numSamples, &source);
}

// =============================================================================
// Audio thread
// =============================================================================
void AudioEngineHost::beginPendingSwap() noexcept
{
    // One swap at a time, and only once the message thread has collected the
    // engine the previous one retired (retiredSwap is a single slot).
    if (swapRunning || retiredSwap.load (std::memory_order_acquire) != nullptr)
        return;
    auto* next = pendingSwap.exchange (nullptr, std::memory_order_acq_rel);
    if (next == nullptr)
        return;

    fading = active;
    active = next;
    for (int s = next->numStrips; s < kMaxStrips; ++s)
        stripActive[static_cast<size_t> (s)].store (false, std::memory_order_relaxed); // removed by the new layout

    // Both engines run from here on; the new one is silent while it pre-rolls.
    //   equal latency : crossfade (gOld + gNew = 1) over [preroll, preroll + F)
    //   latency change: old fades out over [preroll - F, preroll) and is
    //                   retired, then the new one fades in over [preroll, preroll + F)
    const int fade = static_cast<int> (next->fadeIn.size());
    const int preroll = std::max (next->prerollSamples, fade);
    const bool overlap = next->latencySamples == fading->latencySamples;
    swapPos = 0;
    swapFadeInStart = preroll;
    swapFadeOutStart = overlap ? preroll : preroll - fade;
    swapOldEnd = swapFadeOutStart + fade;
    swapEnd = preroll + fade;
    swapCounts = true;
    swapRunning = true;
}

void AudioEngineHost::mixSwap (const flub::AudioBlock& mix, int numSamples) noexcept
{
    // mix holds the active (new) engine's output; the old engine's is in
    // fading->mixOutput. Both fade with the new engine's raised-cosine table.
    const float* fadeIn = active->fadeIn.data();
    const int fade = static_cast<int> (active->fadeIn.size());
    const auto gainAt = [fadeIn, fade] (int pos, int start) noexcept
    {
        return pos < start ? 0.0f : (pos - start < fade ? fadeIn[pos - start] : 1.0f);
    };

    for (int c = 0; c < 2; ++c)
    {
        float* d = mix.channel (c);
        const float* old = fading != nullptr ? fading->mixOutput.channel (c) : nullptr;
        for (int k = 0; k < numSamples; ++k)
        {
            const int pos = swapPos + k;
            const float gNew = gainAt (pos, swapFadeInStart);
            d[k] = old != nullptr ? gNew * d[k] + (1.0f - gainAt (pos, swapFadeOutStart)) * old[k] : gNew * d[k];
        }
    }

    swapPos += numSamples;
    if (fading != nullptr && swapPos >= swapOldEnd)
    {
        // The old engine is silent from here on: back to the message thread,
        // which destroys it (never freed here).
        retiredSwap.store (fading, std::memory_order_release);
        fading = nullptr;
        clearRemovedStrips();
    }
    if (swapPos >= swapEnd)
    {
        swapRunning = false;
        if (swapCounts)
            swapsCompleted.fetch_add (1, std::memory_order_acq_rel);
    }
}

void AudioEngineHost::processBlock (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                                    int numSamples, StripSignalSource* offlineSource) noexcept
{
    beginPendingSwap();

    auto& current = *active;
    const bool alsaOrder = alsaChannelOrder.load (std::memory_order_relaxed);
    applyPendingMixSettings (current);
    if (fading != nullptr)
        applyPendingMixSettings (*fading);

    for (int pos = 0; pos < numSamples;)
    {
        const int n = std::min ({ numSamples - pos, current.maxBlock, fading != nullptr ? fading->maxBlock : current.maxBlock });

        // ---- 1. Gather strip inputs into the preallocated strip buffers ----
        for (int s = 0; s < current.numStrips; ++s)
        {
            const auto si = static_cast<size_t> (s);
            auto& buffer = current.stripBuffers[si];
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
                    const bool permute = usesAlsaSurroundOrder (alsaOrder, channels);
                    for (int c = 0; c < channels; ++c)
                    {
                        const int src = firstInput + (permute ? kAlsaSurroundOrder[static_cast<size_t> (c)] : c);
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
                    pullCapture (slot, s, block, fed);
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

            current.stripBlocks[si] = block;
            current.stripInputs[si] = fed ? &current.stripBlocks[si] : nullptr;
            stripActive[si].store (fed, std::memory_order_relaxed);
        }

        // ---- 1b. During a swap the old engine gets the same input (copied:
        //          the engines process in place; its strip layout may differ) --
        if (fading != nullptr)
        {
            for (int s = 0; s < fading->numStrips; ++s)
            {
                const auto si = static_cast<size_t> (s);
                auto& buffer = fading->stripBuffers[si];
                const flub::AudioBlock block = buffer.block (buffer.getNumChannels(), n);
                if (s >= current.numStrips)
                {
                    // A strip the new layout removed (its sources are gone):
                    // silence while its hang-over lasts, so its tail decays
                    // and fades out with the old engine instead of being cut.
                    if (hangoverRemaining[si] <= 0)
                    {
                        fading->stripInputs[si] = nullptr;
                        continue;
                    }
                    block.clear();
                    hangoverRemaining[si] -= n;
                }
                else if (const flub::AudioBlock* src = current.stripInputs[si]; src != nullptr)
                {
                    for (int c = 0; c < block.numChannels; ++c)
                    {
                        if (c < src->numChannels)
                            std::memcpy (block.channel (c), src->channel (c), sizeof (float) * static_cast<size_t> (n));
                        else
                            std::memset (block.channel (c), 0, sizeof (float) * static_cast<size_t> (n));
                    }
                }
                else
                {
                    fading->stripInputs[si] = nullptr;
                    continue;
                }
                fading->stripBlocks[si] = block;
                fading->stripInputs[si] = &fading->stripBlocks[si];
            }
            fading->engine.process (fading->stripInputs.data(), fading->mixOutput.block (2, n));
        }

        // ---- 2. Mix (and the swap / start-up fade) --------------------------------
        const flub::AudioBlock mix = current.mixOutput.block (2, n);
        if (current.numStrips > 0)
            current.engine.process (current.stripInputs.data(), mix);
        else
            mix.clear();
        if (swapRunning)
            mixSwap (mix, n);

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
                                                        const juce::AudioIODeviceCallbackContext& context)
{
    const uint64_t startNs = steadyNowNs();
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

    const bool ready = engineReady.load (std::memory_order_acquire);
    // The guard reads the device input map here too, so a map that starts
    // feeding a strip while the pair loops is muted from that block on.
    const bool guarded = loopbackPair.load (std::memory_order_acquire) && deviceInputFeedsStrip();
    if (ready && ! (guarded && guardGain <= 0.0f))
    {
        processBlock (inputChannelData, numInputChannels, outputChannelData, numOutputChannels, numSamples, nullptr);
        if (guarded || guardGain < 1.0f)
            applyGuardToOutput (outputChannelData, numOutputChannels, numSamples, guarded);
    }
    else
    {
        // Not configured yet, or the loopback guard holds the output at
        // silence. The engine is frozen then: its protection loops see
        // nothing of the loop and keep their state.
        for (int c = 0; c < numOutputChannels; ++c)
            if (outputChannelData[c] != nullptr)
                std::memset (outputChannelData[c], 0, sizeof (float) * static_cast<size_t> (numSamples));
        if (ready)
            for (auto& a : stripActive)
                a.store (false, std::memory_order_relaxed);
    }

    // docs/11 E45: the interval runs on one clock only, so a backend that
    // starts or stops giving host times begins a new run.
    const bool fromHost = context.hostTimeNs != nullptr;
    if (fromHost != lastStampFromHost)
        callbackTiming.restartIntervals();
    lastStampFromHost = fromHost;
    const double rate = deviceSampleRate.load (std::memory_order_relaxed);
    const auto periodNs = rate > 0.0 ? static_cast<uint64_t> (1.0e9 * static_cast<double> (numSamples) / rate) : uint64_t { 0 };
    const uint64_t endNs = steadyNowNs();
    callbackTiming.record (fromHost ? *context.hostTimeNs : startNs, endNs - startNs, periodNs);

    callbackCounter.fetch_add (1, std::memory_order_seq_cst);
}

bool AudioEngineHost::deviceInputFeedsStrip() const noexcept
{
    for (const auto& first : deviceInputFirst)
        if (first.load (std::memory_order_relaxed) >= 0)
            return true;
    return false;
}

void AudioEngineHost::applyGuardToOutput (float* const* outputs, int numOutputs, int numSamples, bool muted) noexcept
{
    // Linear ramp over kSwapFadeMs towards the guard's target (0 while the
    // output is the input's loopback partner, 1 otherwise).
    const float target = muted ? 0.0f : 1.0f;
    const float step = 1.0f / static_cast<float> (std::max<size_t> (1, active->fadeIn.size()));
    float g = guardGain;
    for (int k = 0; k < numSamples; ++k)
    {
        g = target > g ? std::min (target, g + step) : std::max (target, g - step);
        for (int c = 0; c < numOutputs; ++c)
            if (outputs[c] != nullptr)
                outputs[c][k] *= g;
    }
    guardGain = g;
}

void AudioEngineHost::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const double sampleRate = device->getCurrentSampleRate();
    const int blockSize = device->getCurrentBufferSizeSamples();

    // Read by the callback, which has not started yet.
    alsaChannelOrder.store (isAlsaDeviceType (device->getTypeName()), std::memory_order_relaxed);
    deviceSampleRate.store (sampleRate, std::memory_order_relaxed);
    callbackTiming.restartIntervals();

    // Must be visible before any async configure request is handled.
    callbackRunning.store (true, std::memory_order_release);

    if (juce::MessageManager::existsAndIsCurrentThread())
    {
        configurePending.store (false, std::memory_order_release);
        deviceInputLatency = device->getInputLatencyInSamples();
        deviceOutputLatency = device->getOutputLatencyInSamples();
        configureEngine (sampleRate, blockSize, true);
        applyDeviceStartSafety (device);
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
    callbackTiming.restartIntervals();
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
// Device safety
// =============================================================================
bool AudioEngineHost::isLoopbackPair (const juce::String& inputDeviceName, const juce::String& outputDeviceName)
{
    const auto in = inputDeviceName.trim().toLowerCase();
    const auto out = outputDeviceName.trim().toLowerCase();
    if (in.isEmpty() || out.isEmpty())
        return false;

    // A sink and its monitor source (PulseAudio / PipeWire): the monitor
    // carries exactly what is played to the sink, whatever the sink is.
    if (in == out + ".monitor" || in == "monitor of " + out)
        return true;

    // "Stereo Mix" / "What U Hear" / "Wave Out Mix" record the output of the
    // same codec: "Stereo Mix (Realtek(R) Audio)" <-> "Speakers (Realtek(R) Audio)".
    const auto inDriver = driverSuffix (in);
    if ((in.startsWith ("stereo mix") || in.startsWith ("what u hear") || in.startsWith ("wave out mix"))
        && inDriver.isNotEmpty() && inDriver == driverSuffix (out))
        return true;

    // Everything below is a virtual device; physical devices whose input and
    // output share a name (USB headsets: microphone + earcups) are not loops.
    if (! hasVirtualDeviceToken (in) && ! hasVirtualDeviceToken (out))
        return false;

    // One virtual device as both ends: BlackHole 2ch, Soundflower (2ch),
    // Loopback Audio, VB-Cable on macOS, a Flubsound endpoint.
    if (in == out)
        return true;

    // VB-Audio cables: "<X> Output (<driver>)" records what is played to
    // "<X> Input (<driver>)" (CABLE, CABLE-A..D, Hi-Fi Cable, and the old
    // "VoiceMeeter Output" / "VoiceMeeter Input" names).
    if (const auto inKey = withWordReplaced (in, "output"); inKey.isNotEmpty() && inKey == withWordReplaced (out, "input"))
        return true;

    // Voicemeeter: each VAIO driver's recording endpoint is its bus (Out B1
    // on "VB-Audio Voicemeeter VAIO", Out B2 on "... AUX VAIO", Out B3 on
    // "... VAIO3"), which carries that driver's virtual input by default.
    // Different drivers are different buses and are left alone.
    if (in.contains ("voicemeeter") && out.contains ("voicemeeter") && inDriver.isNotEmpty() && inDriver == driverSuffix (out))
        return true;

    return false;
}

void AudioEngineHost::checkLoopbackPair (const juce::String& inputDeviceName, const juce::String& outputDeviceName)
{
    JUCE_ASSERT_MESSAGE_THREAD
    guardInputName = inputDeviceName;
    guardOutputName = outputDeviceName;

    const bool allowed = std::any_of (allowedLoopbackPairs.begin(), allowedLoopbackPairs.end(),
                                      [&] (const auto& p)
                                      { return p.first.equalsIgnoreCase (inputDeviceName) && p.second.equalsIgnoreCase (outputDeviceName); });
    loopbackPair.store (! allowed && isLoopbackPair (inputDeviceName, outputDeviceName), std::memory_order_release);

    // Only a pair whose input feeds a strip is a loop (the timer re-checks
    // when the device input map changes; the audio thread reads it itself).
    if (isOutputMutedByGuard())
    {
        DeviceSafetyState next;
        next.kind = DeviceSafetyState::Kind::LoopbackPair;
        next.inputDeviceName = inputDeviceName;
        next.outputDeviceName = outputDeviceName;
        next.outputMuted = true;
        next.message = "Output muted: \"" + outputDeviceName + "\" feeds the input \"" + inputDeviceName
                     + "\" back into Flubsound (a feedback loop). This happens when the output falls back to the virtual cable, e.g. "
                       "after a wireless headset disconnects. Choose another output device.";
        setSafetyState (next);
    }
    else if (safety.kind == DeviceSafetyState::Kind::LoopbackPair)
    {
        setSafetyState ({});
    }
}

void AudioEngineHost::allowLoopbackPair (const juce::String& inputDeviceName, const juce::String& outputDeviceName)
{
    JUCE_ASSERT_MESSAGE_THREAD
    allowedLoopbackPairs.emplace_back (inputDeviceName, outputDeviceName);
    checkLoopbackPair (guardInputName, guardOutputName);
}

void AudioEngineHost::clearAllowedLoopbackPairs()
{
    JUCE_ASSERT_MESSAGE_THREAD
    allowedLoopbackPairs.clear();
    checkLoopbackPair (guardInputName, guardOutputName);
}

void AudioEngineHost::clearDeviceError()
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (safety.kind == DeviceSafetyState::Kind::DeviceError)
        setSafetyState ({});
}

void AudioEngineHost::applyDeviceStartSafety (juce::AudioIODevice* device)
{
    // Message thread, before the engine is marked ready: the callback is not
    // running (or runs silent), so the guard gain can be set directly and
    // the guard applies from the first sample.

    // A device started: an earlier error or failed open no longer applies.
    if (safety.kind == DeviceSafetyState::Kind::DeviceError)
        setSafetyState ({});

    // The device manager's names (updated before the callbacks are told);
    // a device driven without it (tests, a duplex device) names both ends.
    const auto setup = deviceManager.getAudioDeviceSetup();
    auto output = setup.outputDeviceName, input = setup.inputDeviceName;
    if (device != nullptr)
    {
        if (output.isEmpty())
            output = device->getName();
        if (input.isEmpty() && ! device->getActiveInputChannels().isZero())
            input = device->getName();
    }
    checkLoopbackPair (input, output);
    guardGain = isOutputMutedByGuard() ? 0.0f : 1.0f;
}

void AudioEngineHost::setSafetyState (DeviceSafetyState next)
{
    if (next.kind == safety.kind && next.message == safety.message && next.inputDeviceName == safety.inputDeviceName
        && next.outputDeviceName == safety.outputDeviceName && next.outputMuted == safety.outputMuted)
        return;
    next.generation = safety.generation + 1;
    safety = std::move (next);
    // Listeners are told asynchronously: this may run inside
    // audioDeviceAboutToStart (under the device manager's callback lock).
    safetyNotifyPending.store (true, std::memory_order_release);
    triggerAsyncUpdate();
}

// =============================================================================
// Telemetry
// =============================================================================
LatencyInfo AudioEngineHost::getLatencyInfo() const
{
    LatencyInfo info;
    // Only a running device (with the guard not holding it silent) has a
    // latency; anything else would be a stale or made-up number (docs/11 E51).
    info.valid = callbackRunning.load (std::memory_order_acquire) && engineReady.load (std::memory_order_acquire)
              && ! isOutputMutedByGuard();
    info.estimated = true; // driver-reported device latencies; nothing is measured end to end (E42d)
    info.sampleRate = currentSampleRate;
    info.blockSize = currentBlockSize;
    const bool deviceOpen = deviceManager.getCurrentAudioDevice() != nullptr;
    info.deviceInputSamples = deviceOpen ? deviceInputLatency : 0;
    info.deviceOutputSamples = deviceOpen ? deviceOutputLatency : 0;
    auto& engine = latest->engine;
    info.engineSamples = engine.getLatencySamples();
    info.deviceInputMs = samplesToMs (info.deviceInputSamples, currentSampleRate);
    info.deviceOutputMs = samplesToMs (info.deviceOutputSamples, currentSampleRate);
    info.engineMs = samplesToMs (info.engineSamples, currentSampleRate);
    info.graphQuantumMs = graphQuantumMs;

    // Per strip (E42a), as the MixEngine runs it: the strip's own chain + the
    // master limiter, and the padding that aligns it with the slowest strip
    // of its sync group (none for a strip in no group).
    info.numStrips = std::min (engine.getNumStrips(), static_cast<int> (info.strips.size()));
    for (int s = 0; s < info.numStrips; ++s)
    {
        auto& strip = info.strips[static_cast<size_t> (s)];
        strip.outputSamples = engine.getStripLatencySamples (s);
        strip.paddingSamples = engine.getStripPaddingSamples (s);
        strip.ownSamples = strip.outputSamples - strip.paddingSamples;
        strip.ownMs = samplesToMs (strip.ownSamples, currentSampleRate);
        strip.outputMs = samplesToMs (strip.outputSamples, currentSampleRate);
        strip.paddingMs = samplesToMs (strip.paddingSamples, currentSampleRate);
    }

    for (const auto& slot : captureSlots)
        if (slot.capture != nullptr)
            info.captureBufferMs = std::max (info.captureBufferMs, static_cast<double> (slot.fifo.getStats().targetMs));

    info.totalMs = info.deviceInputMs + info.deviceOutputMs + info.engineMs + info.graphQuantumMs;
    return info;
}

juce::String AudioEngineHost::formatTotalLatency (const LatencyInfo& info)
{
    if (! info.valid)
        return "--";
    return juce::String (info.totalMs + info.captureBufferMs, 1) + (info.estimated ? " ms (est.)" : " ms");
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
    st.callbackTiming = callbackTiming.snapshot();
    return st;
}

std::array<int, flub::kMaxChannels> AudioEngineHost::deviceInputOrder (const juce::String& deviceTypeName, int stripChannels) noexcept
{
    std::array<int, flub::kMaxChannels> order {};
    const bool permute = usesAlsaSurroundOrder (isAlsaDeviceType (deviceTypeName), stripChannels);
    for (int c = 0; c < flub::kMaxChannels; ++c)
        order[static_cast<size_t> (c)] = permute && c < stripChannels ? kAlsaSurroundOrder[static_cast<size_t> (c)] : c;
    return order;
}
} // namespace flub::app
