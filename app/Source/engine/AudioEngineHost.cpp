#include "AudioEngineHost.h"

#include "flub/common/Denormals.h"
#include "platform/PlatformBridge.h"
#include "platform/pipewire/PipeWireDeviceType.h"

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

// The calling thread's kernel thread id (REAL-TIME AUDIO THREAD); 0 where
// the platform has none.
uint64_t osThreadId() noexcept
{
#if FLUB_HAS_PLATFORM_SERVICES
    return flub::platform::RealtimeScheduling::currentThreadId();
#else
    return 0;
#endif
}

// What the audio thread's own promotion is called on this system.
const char* ownPromotionName() noexcept
{
#if defined(_WIN32)
    return "MMCSS Pro Audio";
#elif defined(__APPLE__)
    return "time constraint";
#else
    return "FIFO";
#endif
}

constexpr const char* kRealtimeFix =
    "install and start RealtimeKit (the rtkit package), or give your user a real-time limit "
    "(the audio or realtime group with an rtprio limit in /etc/security/limits.d), or use the JACK device type "
    "(PipeWire's JACK runs the callback on its own real-time thread)";

// The capture fold's surround <-> stereo passthrough ramp: ProcessingChain's
// kPassFadeMs.
constexpr float kCaptureFoldRampMs = 400.0f;

uint64_t steadyNowNs() noexcept
{
    return static_cast<uint64_t> (
        std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ---- Device selection (docs/11 E51) ---------------------------------------------
/** The output device a JUCE DEVICESETUP element names. */
juce::String outputNameIn (const juce::XmlElement& xml)
{
    const auto both = xml.getStringAttribute ("audioDeviceName");
    return both.isNotEmpty() ? both : xml.getStringAttribute ("audioOutputDeviceName");
}

/** Fills the endpoint id and hardware id of the endpoint `choice.deviceName`
    names in `endpoints` (JUCE's duplicate numbering undone); unchanged when
    none matches. */
void rememberIdentity (flub::app::AudioEngineHost::OutputChoice& choice, const std::vector<flub::platform::OutputEndpointIdentity>& endpoints)
{
    std::vector<flub::platform::AppAudioRouter::OutputEndpoint> plain;
    plain.reserve (endpoints.size());
    for (const auto& e : endpoints)
        plain.push_back ({ e.id, e.name });
    const auto id = flub::platform::AppAudioRouter::matchOutputDeviceName (plain, choice.deviceName.toStdString());
    for (const auto& e : endpoints)
        if (! id.empty() && e.id == id)
        {
            choice.endpointId = juce::String (e.id);
            choice.hardwareId = juce::String (e.hardwareId);
        }
}

/** The saved setup's input device, channels, rate and block size (as
    AudioDeviceManager::initialiseFromXML reads them), for a device opened
    by the selection while none is open: JUCE keeps no setup of a device
    that failed to open. The output name is the selection's. */
void applySavedSetup (const juce::XmlElement& xml, juce::AudioDeviceManager::AudioDeviceSetup& setup)
{
    const auto both = xml.getStringAttribute ("audioDeviceName");
    if (setup.inputDeviceName.isEmpty())
        setup.inputDeviceName = both.isNotEmpty() ? both : xml.getStringAttribute ("audioInputDeviceName");
    setup.bufferSize = xml.getIntAttribute ("audioDeviceBufferSize", setup.bufferSize);
    setup.sampleRate = xml.getDoubleAttribute ("audioDeviceRate", setup.sampleRate);
    if (xml.hasAttribute ("audioDeviceInChans"))
    {
        setup.inputChannels.parseString (xml.getStringAttribute ("audioDeviceInChans"), 2);
        setup.useDefaultInputChannels = false;
    }
    if (xml.hasAttribute ("audioDeviceOutChans"))
    {
        setup.outputChannels.parseString (xml.getStringAttribute ("audioDeviceOutChans"), 2);
        setup.useDefaultOutputChannels = false;
    }
}

/** A name that says headphones or a headset (no platform form factor known). */
bool soundsLikeHeadphones (const juce::String& name)
{
    const auto lower = name.toLowerCase();
    for (const auto* t : { "headphone", "headset", "earphone", "earbud", "casque", "kopfh" })
        if (lower.contains (t))
            return true;
    return false;
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

#if FLUB_HAS_PLATFORM_SERVICES
    realtimeHooks.query = [] (uint64_t threadId) { return flub::platform::RealtimeScheduling::queryThread (threadId); };
    realtimeHooks.request = [] (uint64_t threadId, int priority)
    { return flub::platform::RealtimeScheduling::requestRealtimeKit (threadId, priority); };
    channelMapQuery = [] (const juce::String& type, const juce::String& name, int channels)
    { return flub::platform::AudioChannelMaps::queryInputPositions (type.toStdString(), name.toStdString(), channels); };
#endif

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

    // The chosen output and its identity (DEVICE SELECTION); settings from
    // before docs/11 E51 carry the name only, and the identity is filled in
    // once the device is seen.
    chosen = {};
    if (savedState != nullptr && savedState->hasTagName ("DEVICESETUP"))
    {
        chosen.deviceName = outputNameIn (*savedState);
        chosen.endpointId = savedState->getStringAttribute ("flubOutputEndpointId");
        chosen.hardwareId = savedState->getStringAttribute ("flubOutputHardwareId");
    }
    lastExplicitOutput = chosen.deviceName;
    selection = {};
    unavailableOutputs.clear();
    recoveryPending = recoveryCheckHealth = suspended = false;
    recoveryAttempts = 0;
    managingDevice = true;

#if FLUB_HAS_PLATFORM_SERVICES
    if (deviceWatcher == nullptr && ! deviceWatcherInjected)
        deviceWatcher = flub::platform::AudioDeviceWatcher::create();
#endif
    if (deviceWatcher != nullptr && ! deviceWatcherStarted)
        deviceWatcherStarted = deviceWatcher->start ([this] (const flub::platform::AudioDeviceEvent& e) { postDeviceEvent (e); });

    // docs/11 E48: the native PipeWire node is offered as the "PipeWire"
    // device type, after JUCE's own (a no-op in a build without libpipewire's
    // headers, or on a system where libpipewire-0.3.so.0 does not load; R1.2).
    flub::platform::pipewire::addDeviceType (deviceManager);

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

   #if JUCE_LINUX
    // docs/11 E48: nothing saved (first run): prefer the native PipeWire node,
    // which creates the strips' sinks, links them and plays to the default
    // output by itself, so no setup script or manual wiring is needed. Only
    // in the app's own device list (JUCE's ALSA type present, never over a
    // type a caller added), only when a PipeWire server answers and plays
    // the audio (R1.2: it has an output sink that is not Flubsound's; a
    // PipeWire run for screen capture beside PulseAudio has none, and the
    // node would play into nothing) and only when it opens; JUCE's default
    // type otherwise. Not "chosen": nothing is persisted until the user
    // picks a device.
    juce::String pipewireFallbackType;
    if (savedState == nullptr && flub::platform::pipewire::kHasDeviceType)
    {
        bool haveAlsa = false, havePipeWire = false;
        for (auto* type : deviceManager.getAvailableDeviceTypes())
        {
            haveAlsa = haveAlsa || type->getTypeName() == "ALSA";
            havePipeWire = havePipeWire || type->getTypeName() == flub::platform::pipewire::kDeviceTypeName;
        }
        std::string notPlaying;
        if (haveAlsa && havePipeWire && deviceManager.getCurrentAudioDeviceType() != flub::platform::pipewire::kDeviceTypeName
            && flub::platform::pipewire::serverPlaysAudio (notPlaying))
        {
            pipewireFallbackType = deviceManager.getCurrentAudioDeviceType();
            deviceManager.setCurrentAudioDeviceType (flub::platform::pipewire::kDeviceTypeName, false);
        }
    }
   #endif

    // Explicit selection: never JUCE's selectDefaultDeviceOnFailure, whose
    // default may be the input's loopback partner (the cable setup's system
    // default IS CABLE Input). reselectOutput() picks instead.
    auto error = deviceManager.initialise (maxInputChannels, maxOutputChannels, savedState, false);

   #if JUCE_LINUX
    if (pipewireFallbackType.isNotEmpty() && (error.isNotEmpty() || deviceManager.getCurrentAudioDevice() == nullptr))
    {
        deviceManager.setCurrentAudioDeviceType (pipewireFallbackType, false);
        error = deviceManager.initialise (maxInputChannels, maxOutputChannels, nullptr, false);
    }
   #endif

   #if JUCE_WINDOWS
    if (fallbackType.isNotEmpty() && (error.isNotEmpty() || deviceManager.getCurrentAudioDevice() == nullptr))
    {
        deviceManager.setCurrentAudioDeviceType (fallbackType, false);
        error = deviceManager.initialise (maxInputChannels, maxOutputChannels, nullptr, false);
    }
   #endif

    deviceManager.addChangeListener (this);
    noteExplicitOutput();
    reselectOutput();
    // docs/11 E42c: the profile's buffer size, before the engine attaches (one
    // device start fewer). Upgrading: the saved state decides Automatic first.
    decidePendingAutomaticBuffer();
    lastBufferAttempt = {};
    applyBufferPolicy();
    if (deviceManager.getCurrentAudioDevice() != nullptr)
        error = {};
    else if (error.isEmpty() && selection.reason == OutputReason::NoSafeOutput && chosen.deviceName.isNotEmpty())
        error = "\"" + chosen.deviceName + "\" is not available and no other output is safe to use";

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
    if (managingDevice)
    {
        deviceManager.removeChangeListener (this);
        managingDevice = false;
    }
    if (deviceWatcher != nullptr && deviceWatcherStarted)
    {
        deviceWatcher->stop();
        deviceWatcherStarted = false;
    }
    {
        const juce::ScopedLock sl (eventLock);
        pendingEvents.clear();
    }
    recoveryPending = reselectPending = false;
    applySafeSpeakerProfile (false);
    const bool hadFallback = selection.fallback;
    selection = {};
    if (hadFallback)
        updateFallbackState();

    if (callbackAttached)
    {
        deviceManager.removeAudioCallback (this);
        callbackAttached = false;
    }
    deviceManager.closeAudioDevice();
    engineReady.store (false, std::memory_order_release);
    callbackRunning.store (false, std::memory_order_release);
    deviceInputLatency = deviceOutputLatency = 0;
    deviceInputChannels.store (0, std::memory_order_relaxed);
    // No callback runs now: a latency probe stops here (takeLatencyProbe
    // hands it back unfinished).
    unpublishLatencyProbes();

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
        deviceInputFirst[i].store (-1);

    // A neural model follows its strip (the first unused strip of the same
    // name), so the one reconfigure() below carries it to the strip's new
    // index: no engine ever runs it on the strip that now has its old index.
    std::array<NeuralModelSetup, kMaxStrips> movedModels {};
    std::array<bool, kMaxStrips> modelTaken {};
    for (size_t i = 0; i < newLayout.size(); ++i)
        for (size_t j = 0; j < layout.size() && j < static_cast<size_t> (kMaxStrips); ++j)
            if (! modelTaken[j] && layout[j].name == newLayout[i].name)
            {
                modelTaken[j] = true;
                movedModels[i] = std::move (neuralModels[j]);
                break;
            }
    neuralModels = std::move (movedModels);

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

void AudioEngineHost::setNeuralModel (int strip, NeuralModelFactory factory, const flub::NeuralSlotConfig& config, NeuralSafety safetyRule)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (strip < 0 || strip >= latest->numStrips)
    {
        jassertfalse;
        return;
    }
    neuralModels[static_cast<size_t> (strip)] = { std::move (factory), config, safetyRule };
    reconfigure();
}

int AudioEngineHost::safetyFramesForBuffer (int blockSize, int frameSize) noexcept
{
    if (frameSize <= 0)
        return 1;
    const int block = std::max (1, blockSize);
    const int frames = block / frameSize + (block % frameSize != 0 ? 1 : 0); // ceil, without overflow
    return std::clamp (frames, 1, flub::AsyncModelConfig::kMaxSafetyFrames);
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
    // from the first block. NeuralSafety::OneDeviceBuffer resolves the safety
    // frames here, for the buffer this engine is built for.
    const auto installNeuralModel = [this] (int strip, flub::ProcessingChain& chain)
    {
        const auto& model = neuralModels[static_cast<size_t> (strip)];
        if (model.factory == nullptr)
            return;
        auto runner = model.factory();
        if (runner == nullptr)
            return;
        flub::NeuralSlotConfig config = model.config;
        if (model.safetyRule == NeuralSafety::OneDeviceBuffer)
            config.processor.safetyFrames = safetyFramesForBuffer (currentBlockSize, runner->describe().frameSize);
        chain.setNeuralModel (std::move (runner), config);
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

    // New chains start without the safe speaker profile's bypasses.
    if (safeProfileActive)
        applySafeSpeakerProfile (true);

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
    enforceOutputPin();
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

    // A new device thread: its real-time status, RealtimeKit if needed.
    serviceAudioThreadRealtime();

    // docs/11 E51: a recovery step or a re-selection when one is due; the
    // safe speaker profile's bypasses held (an audition release may have
    // cleared one).
    serviceDeviceRecovery();
    if (reselectPending && static_cast<int32_t> (juce::Time::getMillisecondCounter() - reselectDueMs) >= 0)
    {
        reselectPending = false;
        reselectOutput();
    }
    if (safeProfileActive)
        applySafeSpeakerProfile (true);
}

void AudioEngineHost::handleAsyncUpdate()
{
    enforceOutputPin(); // docs/11 E53: before anything re-selects

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
        // docs/11 E51: re-open the output unless callbacks keep running.
        if (managingDevice)
            scheduleRecovery (message, recoveryTiming.settleMs, true);
    }

    if (deviceEventsPending.exchange (false, std::memory_order_acq_rel))
        handleDeviceEvents();

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
// Real-device soak hooks (docs/11 E53)
// =============================================================================
void AudioEngineHost::setOutputPin (const juce::String& outputDeviceName)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (! callbackRunning.load (std::memory_order_acquire)); // audioDeviceAboutToStart reads it
    outputPin = outputDeviceName;
}

void AudioEngineHost::enforceOutputPin()
{
    if (! pinBlocked.load (std::memory_order_acquire))
        return;
    // Silenced since its first callback; nothing of it reached the device.
    if (auto* device = deviceManager.getCurrentAudioDevice(); device != nullptr && device->getName() != outputPin)
    {
        ++pinViolations;
        deviceManager.closeAudioDevice(); // the change message re-selects: the pin, or nothing
    }
    pinBlocked.store (false, std::memory_order_release);
}

bool AudioEngineHost::setDeviceSignalSource (StripSignalSource* source)
{
    JUCE_ASSERT_MESSAGE_THREAD
    // seq_cst on both sides: the waitForAudioThreadToPass handshake (a
    // callback that read the old pointer has returned once the counter moved).
    deviceSignalSource.store (source, std::memory_order_seq_cst);
    uint64_t counter = 0;
    return source != nullptr || waitForAudioThreadToPass (counter);
}

bool AudioEngineHost::setOutputTap (flub::StreamTap* tap)
{
    JUCE_ASSERT_MESSAGE_THREAD
    outputTap.store (tap, std::memory_order_seq_cst);
    uint64_t counter = 0;
    return tap != nullptr || waitForAudioThreadToPass (counter);
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
                    const auto order = stripInputOrder (firstInput, channels, alsaOrder);
                    for (int c = 0; c < channels; ++c)
                    {
                        const int src = firstInput + order[static_cast<size_t> (c)];
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

                // docs/11 E42d, Through Flubsound: the probe is this strip's
                // input; every other strip's input fades out meanwhile (the
                // captures above keep being read, so their FIFOs stay level).
                if (probeInBlock != nullptr && probeInBlock->getPath() == latency::Path::ThroughFlubsound)
                {
                    if (s == probeInBlock->getStrip())
                    {
                        if (! fed)
                            block.clear();
                        probeInBlock->fillStrip (block, pos);
                        fed = true;
                    }
                    else if (fed)
                    {
                        probeInBlock->muteOther (block, pos);
                    }
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
        // docs/11 E44: which kernel thread this is, for the message thread
        // (serviceAudioThreadRealtime), which reads its scheduling and asks
        // RealtimeKit when the promotion above was not allowed.
        audioThreadId.store (osThreadId(), std::memory_order_relaxed);
        audioThreadPromoted.store (promotionHandle != nullptr, std::memory_order_relaxed);
        audioThreadGeneration.fetch_add (1, std::memory_order_release);
    }

    const bool ready = engineReady.load (std::memory_order_acquire);
    // The guard reads the device input map here too, so a map that starts
    // feeding a strip while the pair loops is muted from that block on.
    const bool guarded = loopbackPair.load (std::memory_order_acquire) && deviceInputFeedsStrip();

    // docs/11 E42d: the first latency probe session handed over that still
    // plays (see LATENCY PROBE). seq_cst: pairs with takeLatencyProbe() /
    // waitForAudioThreadToPass(), like the capture slots.
    latency::ProbeSession* probe = nullptr;
    for (auto& slot : probeLive)
    {
        auto* session = slot.load (std::memory_order_seq_cst);
        if (session == nullptr || ! session->isPlaying())
            continue;
        session->beginCallback(); // a session cancelled before it played ends here
        if (session->isPlaying())
        {
            probe = session;
            break;
        }
    }
    probeInBlock = probe;

    // docs/11 E53: a device that is not the soak's pinned output plays nothing.
    const bool blocked = pinBlocked.load (std::memory_order_acquire);
    if (ready && ! blocked && ! (guarded && guardGain <= 0.0f))
    {
        processBlock (inputChannelData, numInputChannels, outputChannelData, numOutputChannels, numSamples,
                      deviceSignalSource.load (std::memory_order_seq_cst)); // seq_cst: see setDeviceSignalSource
        if (guarded || guardGain < 1.0f)
            applyGuardToOutput (outputChannelData, numOutputChannels, numSamples, guarded);
        applyOutputTrim (outputChannelData, numOutputChannels, numSamples);
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

    // The probe replaces (device only) or caps (through Flubsound) what the
    // engine wrote, records the inputs and advances; silent while the guard
    // holds the output, the engine is not ready or the device is not the
    // pinned output (docs/11 E53: such a device plays nothing).
    if (probe != nullptr)
        probe->process (inputChannelData, numInputChannels, outputChannelData, numOutputChannels, numSamples, ready && ! guarded && ! blocked);
    probeInBlock = nullptr;

    // docs/11 E53: the output as handed to the device (after the engine, the
    // guard, the trim and a latency probe), for the soak's analysis.
    if (auto* tap = outputTap.load (std::memory_order_seq_cst))
        tap->write (outputChannelData, numOutputChannels, numSamples);

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

void AudioEngineHost::applyOutputTrim (float* const* outputs, int numOutputs, int numSamples) noexcept
{
    // docs/11 E51: the safe speaker profile's trim, a linear ramp of at most
    // kTrimRampMs towards the target (1 = off, nothing to do).
    const float target = outputTrimTarget.load (std::memory_order_relaxed);
    float g = outputTrimGain;
    if (g == 1.0f && target == 1.0f)
        return;
    const double rate = deviceSampleRate.load (std::memory_order_relaxed);
    const float step = static_cast<float> (1.0 / std::max (1.0, kTrimRampMs * 0.001 * (rate > 0.0 ? rate : 48000.0)));
    for (int k = 0; k < numSamples; ++k)
    {
        if (g != target)
            g = target > g ? std::min (target, g + step) : std::max (target, g - step);
        for (int c = 0; c < numOutputs; ++c)
            if (outputs[c] != nullptr)
                outputs[c][k] *= g;
    }
    outputTrimGain = g;
}

void AudioEngineHost::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const double sampleRate = device->getCurrentSampleRate();
    const int blockSize = device->getCurrentBufferSizeSamples();

    // R1.2: the native PipeWire device reports a lost server or a removed
    // node to this host directly. JUCE 9.0.2's AudioDeviceManager starts
    // devices through a wrapper (CallbackMaxSizeEnforcer) that drops
    // audioDeviceError, so the error would otherwise never arrive and the
    // E51 recovery never start. A no-op for every other device.
    flub::platform::pipewire::setDeviceErrorTarget (device, this);

    // Read by the callback, which has not started yet.
    alsaChannelOrder.store (isAlsaDeviceType (device->getTypeName()), std::memory_order_relaxed);
    deviceInputChannels.store (device->getActiveInputChannels().countNumberOfSetBits(), std::memory_order_relaxed);
    readDeviceChannelMap (*device);
    deviceSampleRate.store (sampleRate, std::memory_order_relaxed);
    callbackTiming.restartIntervals();
    // docs/11 E53: a device started under another name than the pin (JUCE's
    // own fallback) plays silence from its first callback; the timer closes it.
    pinBlocked.store (outputPin.isNotEmpty() && device->getName() != outputPin, std::memory_order_release);
    deviceStarts.fetch_add (1, std::memory_order_acq_rel);
    if (pinBlocked.load (std::memory_order_relaxed))
        triggerAsyncUpdate(); // handleAsyncUpdate closes it at once

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
    // A latency probe does not continue on another device start (its
    // recording would mix two devices): takeLatencyProbe hands it back.
    unpublishLatencyProbes();
    // No callback runs now. The next device's thread is checked again even if
    // it reuses this one's pthread id (a new kernel thread may).
    promotedThread = nullptr;
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
    deviceErrors.fetch_add (1, std::memory_order_acq_rel);
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
    // The fallback part always describes the current selection (DEVICE SELECTION).
    next.outputFallback = managingDevice && selection.fallback;
    next.safeSpeakerProfile = next.outputFallback && safeProfileActive;
    next.chosenOutputName = next.outputFallback ? chosen.deviceName : juce::String();
    next.fallbackOutputName = next.outputFallback ? selection.deviceName : juce::String();
    next.fallbackMessage = {};
    if (next.outputFallback)
    {
        auto& m = next.fallbackMessage;
        m << "\"" << chosen.deviceName << "\" "
          << (selection.chosenUnavailable ? "could not be opened (another program may use it in exclusive mode)" : "is not connected");
        if (selection.deviceName.isEmpty())
            m << ", and no other output is safe to use.";
        else
            m << ": playing on \"" << selection.deviceName << "\"";
        if (next.safeSpeakerProfile)
            m << " with the safe speaker profile (virtualiser off, bass at most +" << juce::String (kSafeSpeakerBassCapDb, 0) << " dB, "
              << juce::String (kSafeSpeakerTrimDb, 0) << " dB)";
        if (selection.deviceName.isNotEmpty())
            m << ". Flubsound switches back when it is available again.";
    }

    if (next.kind == safety.kind && next.message == safety.message && next.inputDeviceName == safety.inputDeviceName
        && next.outputDeviceName == safety.outputDeviceName && next.outputMuted == safety.outputMuted
        && next.outputFallback == safety.outputFallback && next.safeSpeakerProfile == safety.safeSpeakerProfile
        && next.chosenOutputName == safety.chosenOutputName && next.fallbackOutputName == safety.fallbackOutputName
        && next.fallbackMessage == safety.fallbackMessage)
        return;
    next.generation = safety.generation + 1;
    safety = std::move (next);
    // Listeners are told asynchronously: this may run inside
    // audioDeviceAboutToStart (under the device manager's callback lock).
    safetyNotifyPending.store (true, std::memory_order_release);
    triggerAsyncUpdate();
}

// =============================================================================
// Output selection and recovery (docs/11 E51)
// =============================================================================
bool AudioEngineHost::isVirtualOutput (const juce::String& name, const flub::platform::OutputEndpointIdentity* identity)
{
    if (identity != nullptr && identity->transport == flub::platform::EndpointTransport::Virtual)
        return true;
    return hasVirtualDeviceToken (name.toLowerCase());
}

AudioEngineHost::OutputSelection AudioEngineHost::selectOutput (const OutputChoice& choice, const juce::StringArray& outputs,
                                                                int defaultIndex,
                                                                const std::vector<flub::platform::OutputEndpointIdentity>& endpoints,
                                                                const juce::String& inputDeviceName, bool inputFeedsStrip,
                                                                const juce::StringArray& unavailable)
{
    using flub::platform::AudioDeviceWatcher;
    using flub::platform::EndpointFormFactor;

    // Each output with its platform identity, matched by name like the
    // doubling guard (JUCE numbers duplicate names " (2)"); names only when
    // the platform lists nothing.
    std::vector<flub::platform::AppAudioRouter::OutputEndpoint> plain;
    plain.reserve (endpoints.size());
    for (const auto& e : endpoints)
        plain.push_back ({ e.id, e.name });
    std::vector<flub::platform::OutputEndpointIdentity> present;
    present.reserve (static_cast<size_t> (outputs.size()));
    for (const auto& name : outputs)
    {
        flub::platform::OutputEndpointIdentity identity;
        const auto id = flub::platform::AppAudioRouter::matchOutputDeviceName (plain, name.toStdString());
        for (const auto& e : endpoints)
            if (! id.empty() && e.id == id)
                identity = e;
        identity.name = name.toStdString(); // what the selection opens
        present.push_back (std::move (identity));
    }

    const auto usable = [&] (int i) { return ! unavailable.contains (outputs[i]); };
    const auto pick = [&] (int i, OutputReason reason)
    {
        OutputSelection s;
        s.deviceName = outputs[i];
        s.reason = reason;
        return s;
    };

    const bool hasChoice = choice.deviceName.isNotEmpty() || choice.endpointId.isNotEmpty();
    bool chosenUnavailable = false;
    if (hasChoice)
    {
        // 1) The same endpoint (also renamed), 2) the same name unless the
        // hardware says it is another device, 3) recognised elsewhere.
        const auto wantedId = choice.endpointId.toStdString(), wantedHardware = choice.hardwareId.toStdString();
        for (int i = 0; i < outputs.size(); ++i)
            if (! wantedId.empty() && present[static_cast<size_t> (i)].id == wantedId)
            {
                if (usable (i))
                    return pick (i, outputs[i] == choice.deviceName ? OutputReason::Chosen : OutputReason::Recognised);
                chosenUnavailable = true;
            }
        for (int i = 0; i < outputs.size(); ++i)
        {
            const auto& hw = present[static_cast<size_t> (i)].hardwareId;
            if (outputs[i] == choice.deviceName && (wantedHardware.empty() || hw.empty() || juce::String (hw).equalsIgnoreCase (choice.hardwareId)))
            {
                if (usable (i))
                    return pick (i, OutputReason::Chosen);
                chosenUnavailable = true;
            }
        }
        flub::platform::OutputEndpointIdentity remembered;
        remembered.id = wantedId;
        remembered.name = choice.deviceName.toStdString();
        remembered.hardwareId = wantedHardware;
        std::vector<flub::platform::OutputEndpointIdentity> candidates;
        std::vector<int> indices;
        for (int i = 0; i < outputs.size(); ++i)
            if (usable (i))
            {
                candidates.push_back (present[static_cast<size_t> (i)]);
                indices.push_back (i);
            }
        AudioDeviceWatcher::Match how = AudioDeviceWatcher::Match::None;
        if (const int found = AudioDeviceWatcher::findEndpoint (candidates, remembered, &how); found >= 0)
            return pick (indices[static_cast<size_t> (found)], OutputReason::Recognised);
    }

    // Fallback: the system default unless it is virtual or loops, else the
    // first output that is neither.
    const auto safe = [&] (int i)
    {
        return usable (i) && ! isVirtualOutput (outputs[i], &present[static_cast<size_t> (i)])
               && ! (inputFeedsStrip && isLoopbackPair (inputDeviceName, outputs[i]));
    };
    OutputSelection s;
    s.reason = OutputReason::NoSafeOutput;
    if (defaultIndex >= 0 && defaultIndex < outputs.size() && safe (defaultIndex))
        s = pick (defaultIndex, OutputReason::SystemDefault);
    else
        for (int i = 0; i < outputs.size(); ++i)
            if (safe (i))
            {
                s = pick (i, OutputReason::FirstSafe);
                break;
            }

    s.fallback = hasChoice;
    s.chosenUnavailable = hasChoice && chosenUnavailable;
    if (s.fallback && s.deviceName.isNotEmpty())
    {
        const auto form = present[static_cast<size_t> (outputs.indexOf (s.deviceName))].formFactor;
        const bool headphones = form == EndpointFormFactor::Headphones || form == EndpointFormFactor::Headset
                                || (form == EndpointFormFactor::Unknown && soundsLikeHeadphones (s.deviceName));
        s.safeSpeakerProfile = ! headphones;
    }
    return s;
}

std::unique_ptr<juce::XmlElement> AudioEngineHost::createDeviceStateXml() const
{
    auto xml = deviceManager.createStateXml();
    if (xml != nullptr && chosen.deviceName.isNotEmpty() && outputNameIn (*xml) == chosen.deviceName)
    {
        if (chosen.endpointId.isNotEmpty())
            xml->setAttribute ("flubOutputEndpointId", chosen.endpointId);
        if (chosen.hardwareId.isNotEmpty())
            xml->setAttribute ("flubOutputHardwareId", chosen.hardwareId);
    }
    return xml;
}

void AudioEngineHost::setDeviceWatcher (std::unique_ptr<flub::platform::AudioDeviceWatcher> watcher)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (deviceWatcher != nullptr && deviceWatcherStarted)
        deviceWatcher->stop();
    deviceWatcherStarted = false;
    deviceWatcher = std::move (watcher);
    deviceWatcherInjected = true;
    if (managingDevice && deviceWatcher != nullptr)
        deviceWatcherStarted = deviceWatcher->start ([this] (const flub::platform::AudioDeviceEvent& e) { postDeviceEvent (e); });
}

void AudioEngineHost::postDeviceEvent (const flub::platform::AudioDeviceEvent& event)
{
    {
        const juce::ScopedLock sl (eventLock);
        pendingEvents.push_back (event);
    }
    deviceEventsPending.store (true, std::memory_order_release);
    triggerAsyncUpdate();
}

void AudioEngineHost::handleDeviceEvents()
{
    std::vector<flub::platform::AudioDeviceEvent> events;
    {
        const juce::ScopedLock sl (eventLock);
        events.swap (pendingEvents);
    }
    if (! managingDevice)
        return;

    bool devicesChanged = false;
    for (const auto& e : events)
    {
        ++deviceEventsHandled;
        switch (e.kind)
        {
            case flub::platform::AudioDeviceEvent::Kind::Suspending:
                suspended = true;
                break;
            case flub::platform::AudioDeviceEvent::Kind::Resumed:
                // The devices may take a moment to come back: check after
                // settleMs whether callbacks run, re-open if not (a repeated
                // resume notification only moves the check).
                suspended = false;
                scheduleRecovery ("the system resumed from sleep", recoveryTiming.settleMs, true);
                break;
            case flub::platform::AudioDeviceEvent::Kind::DefaultOutputChanged:
            case flub::platform::AudioDeviceEvent::Kind::DeviceAdded:
            case flub::platform::AudioDeviceEvent::Kind::DeviceRemoved:
            case flub::platform::AudioDeviceEvent::Kind::DeviceStateChanged:
                devicesChanged = true;
                break;
        }
    }

    // Devices come and go during sleep; the resume check re-selects.
    if (devicesChanged && ! suspended)
    {
        unavailableOutputs.clear(); // a device event is worth another try
        // JUCE's WASAPI types watch the endpoints themselves and announce a
        // changed list through the device manager (whose change message
        // re-selects); a rescan here first would make them miss the change
        // (they compare against their last scan). Other types are rescanned
        // here, and the manager's listeners told when the list changed.
        if (auto* type = deviceManager.getCurrentDeviceTypeObject(); type != nullptr && ! type->getTypeName().startsWith ("Windows Audio"))
        {
            const auto listed = [type] { return type->getDeviceNames (false).joinIntoString ("\n") + "\n\n" + type->getDeviceNames (true).joinIntoString ("\n"); };
            const auto before = listed();
            type->scanForDevices();
            if (listed() != before)
                deviceManager.sendChangeMessage();
        }
        reselectOutput();
        // Once more after the device type has caught up with the OS.
        reselectPending = true;
        reselectDueMs = juce::Time::getMillisecondCounter() + kReselectSettleMs;
    }
}

void AudioEngineHost::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // The device manager changed: a device came or went (JUCE may have
    // fallen back by itself), the user chose another device, or the rate /
    // block size changed.
    if (! managingDevice || selecting || applyingBuffer)
        return;
    noteExplicitOutput();
    reselectOutput();
    decidePendingAutomaticBuffer(); // docs/11 E42c: upgrading, no device at openDevice()
    applyBufferPolicy();            // docs/11 E42c: a new device or rate gets the profile's size
}

std::vector<flub::platform::OutputEndpointIdentity> AudioEngineHost::listEndpoints()
{
    return deviceWatcher != nullptr ? deviceWatcher->listOutputs() : std::vector<flub::platform::OutputEndpointIdentity> {};
}

void AudioEngineHost::noteExplicitOutput()
{
    // JUCE's explicit setup is what the user chose (Settings > Audio, or the
    // saved state): its own fallback and the host's never change it.
    const auto xml = deviceManager.createStateXml();
    const auto explicitOutput = xml != nullptr ? outputNameIn (*xml) : juce::String();
    if (explicitOutput == lastExplicitOutput)
        return;
    lastExplicitOutput = explicitOutput;
    if (explicitOutput.isEmpty())
        return;
    // Picking an output while following the default: that output from now on.
    const bool endsFollowing = followDefault && explicitOutput != selection.deviceName;
    if (endsFollowing)
    {
        followDefault = false;
        if (onFollowSystemDefaultChanged)
            onFollowSystemDefaultChanged();
    }
    if (explicitOutput == chosen.deviceName)
        return;

    // A new choice: remember the identity of the endpoint it names now.
    endpointCache = listEndpoints();
    chosen = { explicitOutput, {}, {} };
    rememberIdentity (chosen, endpointCache);
}

void AudioEngineHost::setFollowSystemDefault (bool follow)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (followDefault == follow)
        return;
    followDefault = follow;
    unavailableOutputs.clear();
    reselectOutput(); // nothing before openDevice()
}

void AudioEngineHost::reselectOutput()
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (! managingDevice || selecting)
        return;
    auto* type = deviceManager.getCurrentDeviceTypeObject();
    if (type == nullptr)
        return;
    const juce::ScopedValueSetter<bool> busy (selecting, true);

    endpointCache = listEndpoints();
    const auto outputs = type->getDeviceNames (false);
    const auto inputs = type->getDeviceNames (true);

    // An output that fails to open is skipped and the next candidate tried.
    OutputSelection next;
    for (int attempt = 0; attempt <= outputs.size(); ++attempt)
    {
        const auto setup = deviceManager.getAudioDeviceSetup();
        auto* device = deviceManager.getCurrentAudioDevice();
        // Following the system default: as if nothing were chosen (the choice is kept).
        next = selectOutput (followDefault ? OutputChoice {} : chosen, outputs, type->getDefaultDeviceIndex (false), endpointCache,
                             setup.inputDeviceName, deviceInputFeedsStrip(), unavailableOutputs);
        if (outputPin.isNotEmpty())
        {
            // docs/11 E53: the pinned output or nothing, never a fallback.
            next = {};
            next.reason = OutputReason::NoSafeOutput;
            if (outputs.contains (outputPin) && ! unavailableOutputs.contains (outputPin))
            {
                next.deviceName = outputPin;
                next.reason = OutputReason::Chosen;
            }
        }
        if (next.deviceName.isEmpty() || (device != nullptr && setup.outputDeviceName == next.deviceName))
            break;

        auto target = setup;
        if (device == nullptr)
            if (const auto xml = deviceManager.createStateXml())
                applySavedSetup (*xml, target);
        target.outputDeviceName = next.deviceName;
        if (target.inputDeviceName.isNotEmpty() && ! inputs.contains (target.inputDeviceName))
            target.inputDeviceName = inputs[type->getDefaultDeviceIndex (true)]; // "" when there is no input
        // Not "chosen": JUCE's explicit setup keeps what the user chose.
        const auto error = deviceManager.setAudioDeviceSetup (target, false);
        if (error.isEmpty() && deviceManager.getCurrentAudioDevice() != nullptr)
            break;

        unavailableOutputs.addIfNotAlreadyThere (next.deviceName);
        scheduleRecovery ("\"" + next.deviceName + "\" could not be opened: " + error, recoveryTiming.firstRetryMs, false);
    }

    // docs/11 E53: with a pin, any other device that is open now is closed.
    if (outputPin.isNotEmpty())
        if (auto* stillOpen = deviceManager.getCurrentAudioDevice(); stillOpen != nullptr && stillOpen->getName() != outputPin)
            deviceManager.closeAudioDevice();

    // The chosen output from settings older than its identity: fill it in.
    if (next.reason == OutputReason::Chosen && chosen.endpointId.isEmpty())
        rememberIdentity (chosen, endpointCache);

    selection = next;
    applySafeSpeakerProfile (selection.safeSpeakerProfile);
    updateFallbackState();
}

void AudioEngineHost::scheduleRecovery (const juce::String& reason, int delayMs, bool checkHealth)
{
    // A check `delayMs` from now: with checkHealth the device must have run
    // callbacks since, otherwise it is re-opened; outputs that failed to open
    // are tried again in any case. A pending check keeps the earlier time
    // unless this one is due sooner.
    const auto due = juce::Time::getMillisecondCounter() + static_cast<juce::uint32> (std::max (0, delayMs));
    if (! recoveryPending || static_cast<int32_t> (due - recoveryDueMs) < 0 || checkHealth)
        recoveryDueMs = due;
    if (checkHealth)
        recoveryCallbackMark = callbackCounter.load (std::memory_order_acquire);
    recoveryCheckHealth = recoveryCheckHealth || checkHealth;
    recoveryPending = true;
    recoveryReason = reason;
}

void AudioEngineHost::serviceDeviceRecovery()
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (! recoveryPending || ! managingDevice || suspended)
        return;
    const auto now = juce::Time::getMillisecondCounter();
    if (static_cast<int32_t> (now - recoveryDueMs) < 0)
        return;

    auto* device = deviceManager.getCurrentAudioDevice();
    const bool running = device != nullptr && callbackCounter.load (std::memory_order_acquire) != recoveryCallbackMark;
    if ((running || ! recoveryCheckHealth) && unavailableOutputs.isEmpty() && device != nullptr)
    {
        // Callbacks ran since the event (or there was nothing to check) and
        // no output waits for a retry: recovered.
        recoveryPending = recoveryCheckHealth = false;
        recoveryAttempts = 0;
        if (running)
            clearDeviceError();
        return;
    }

    if (recoveryAttempts >= recoveryTiming.maxAttempts)
    {
        // Given up until the next device event (or Retry).
        recoveryPending = recoveryCheckHealth = false;
        if (device == nullptr || ! running)
        {
            DeviceSafetyState next = safety;
            if (next.kind != DeviceSafetyState::Kind::LoopbackPair)
            {
                next.kind = DeviceSafetyState::Kind::DeviceError;
                next.message = "The audio device did not come back (" + recoveryReason + "). Retry, or choose another output device.";
                setSafetyState (next);
            }
        }
        return;
    }

    ++recoveryAttempts;
    unavailableOutputs.clear();
    if (device != nullptr && recoveryCheckHealth && ! running)
        deviceManager.closeAudioDevice(); // stalled (a device that did not survive sleep): open it again
    reselectOutput();

    // Check again after the backoff delay.
    const int delay = std::min (recoveryTiming.maxRetryMs, recoveryTiming.firstRetryMs << std::min (recoveryAttempts - 1, 16));
    recoveryPending = true;
    recoveryCheckHealth = true;
    recoveryCallbackMark = callbackCounter.load (std::memory_order_acquire);
    recoveryDueMs = juce::Time::getMillisecondCounter() + static_cast<juce::uint32> (std::max (0, delay));
}

void AudioEngineHost::applySafeSpeakerProfile (bool on)
{
    if (! on && ! safeProfileActive)
        return;
    safeProfileActive = on;
    outputTrimTarget.store (on ? juce::Decibels::decibelsToGain (kSafeSpeakerTrimDb) : 1.0f, std::memory_order_relaxed);
    auto& engine = latest->engine;
    for (int s = 0; s < engine.getNumStrips(); ++s)
    {
        engine.chain (s).setAuditionBypass (flub::param::VirtualizerOn, on);
        // Bass capped, not bypassed: the bass engine's boost and the preset's EQ low lift together.
        engine.chain (s).setSafeSpeakerBassCapDb (on ? kSafeSpeakerBassCapDb : flub::ProcessingChain::kNoBassCap);
    }
}

void AudioEngineHost::updateFallbackState()
{
    setSafetyState (safety);
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
        {
            // The FIFO's fill target plus its resampler's fixed delay (docs/11 E50).
            const auto stats = slot.fifo.getStats();
            info.captureBufferMs = std::max (info.captureBufferMs, static_cast<double> (stats.targetMs + stats.resamplerDelayMs));
        }

    info.totalMs = info.deviceInputMs + info.deviceOutputMs + info.engineMs + info.graphQuantumMs;
    return info;
}

flub::platform::NativeAudioNodeStatus AudioEngineHost::getNativeNodeStatus() const
{
    return flub::platform::pipewire::getDeviceStatus (deviceManager.getCurrentAudioDevice());
}

bool AudioEngineHost::isNativeNodeDevice() const
{
    const auto* device = deviceManager.getCurrentAudioDevice();
    return device != nullptr && device->getTypeName() == flub::platform::pipewire::kDeviceTypeName;
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
    st.audioThread = audioThreadRealtime;
    st.inputChannelMap = st.deviceOpen || callbackRunning.load (std::memory_order_acquire) ? inputChannelMap : InputChannelMap::None;
    return st;
}

// =============================================================================
// Device buffer size (docs/11 E42c)
// =============================================================================
juce::String AudioEngineHost::bufferDeviceKey (const juce::String& deviceTypeName, const juce::String& outputDeviceName)
{
    return deviceTypeName + "|" + outputDeviceName;
}

bool AudioEngineHost::managesBufferSize (const juce::String& deviceTypeName)
{
    // Plain shared WASAPI opens at the engine's period whatever is asked;
    // ALSA and JACK on Linux run in PipeWire's or JACK's graph, whose
    // quantum the latency profile already requests (docs/11 E48a); the native
    // PipeWire node takes the profile's node.latency.
    for (const auto* type : { "Windows Audio", "DirectSound", "ALSA", "ALSA HW", "JACK" })
        if (deviceTypeName == type)
            return false;
    return deviceTypeName.isNotEmpty() && deviceTypeName != flub::platform::pipewire::kDeviceTypeName;
}

AudioEngineHost::BufferInfo AudioEngineHost::getBufferInfo() const
{
    BufferInfo info;
    info.automatic = autoBuffer;
    info.profile = bufferProfile;
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
        return info;
    info.deviceOpen = true;
    info.deviceTypeName = device->getTypeName();
    info.outputDeviceName = deviceManager.getAudioDeviceSetup().outputDeviceName;
    if (info.outputDeviceName.isEmpty())
        info.outputDeviceName = device->getName();
    info.sampleRate = device->getCurrentSampleRate();
    info.current = device->getCurrentBufferSizeSamples();
    info.deviceDefault = device->getDefaultBufferSize();
    std::vector<int> sizes;
    for (const int s : device->getAvailableBufferSizes())
        sizes.push_back (s);
    info.available = buffer::normalise (sizes);
    if (! info.available.empty())
    {
        info.smallest = info.available.front();
        info.largest = info.available.back();
    }
    if (const auto it = bufferFloors.find (bufferDeviceKey (info.deviceTypeName, info.outputDeviceName)); it != bufferFloors.end())
        info.floor = it->second;
    info.managed = managesBufferSize (info.deviceTypeName);
    const auto choice = buffer::choose (bufferProfile, info.available, info.deviceDefault, info.sampleRate, info.floor);
    info.reason = choice.reason;
    info.target = info.managed ? choice.samples : info.current;
    return info;
}

void AudioEngineHost::setAutomaticBufferSize (bool automatic)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (automatic && ! autoBuffer)
    {
        // Back on: try the profile's size again from scratch.
        if (auto* device = deviceManager.getCurrentAudioDevice())
        {
            auto output = deviceManager.getAudioDeviceSetup().outputDeviceName;
            bufferFloors.erase (bufferDeviceKey (device->getTypeName(), output.isNotEmpty() ? output : device->getName()));
        }
    }
    autoBuffer = automatic;
    autoBufferPending = false;
    lastBufferAttempt = {};
    applyBufferPolicy();
}

void AudioEngineHost::setAutomaticBufferSizeFromSavedState()
{
    JUCE_ASSERT_MESSAGE_THREAD
    autoBufferPending = true;
    autoBuffer = true;
    decidePendingAutomaticBuffer();
}

void AudioEngineHost::decidePendingAutomaticBuffer()
{
    if (! autoBufferPending)
        return;
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
        return; // at the next device start
    autoBufferPending = false;
    // Before docs/11 E42c the app never asked for a size: a device that opened
    // from the saved state at another size than its default runs at the
    // user's pick from Settings > Audio's buffer list, which must stay.
    const int current = device->getCurrentBufferSizeSamples(), deflt = device->getDefaultBufferSize();
    autoBuffer = ! (managesBufferSize (device->getTypeName()) && current > 0 && deflt > 0 && current != deflt);
    juce::Logger::writeToLog ("Automatic buffer size (first start with it): " + juce::String (autoBuffer ? "on" : "off")
                              + " (the device runs at " + juce::String (current) + " samples, its default is " + juce::String (deflt)
                              + (autoBuffer ? ")" : "; that size was picked in Settings > Audio and is kept)"));
    lastBufferAttempt = {};
    if (onBufferChoiceChanged != nullptr)
        onBufferChoiceChanged();
}

void AudioEngineHost::setBufferProfile (flub::param::LatencyProfileValue profile)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (profile == bufferProfile)
        return;
    bufferProfile = profile;
    lastBufferAttempt = {};
    applyBufferPolicy();
}

void AudioEngineHost::setBufferFloors (std::map<juce::String, int> floors)
{
    JUCE_ASSERT_MESSAGE_THREAD
    bufferFloors = std::move (floors);
    lastBufferAttempt = {};
}

bool AudioEngineHost::applyBufferPolicy()
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (! autoBuffer || autoBufferPending || applyingBuffer || selecting)
        return false;
    if (deviceManager.getCurrentAudioDevice() == nullptr)
        return false;
    const auto info = getBufferInfo();
    if (! info.managed || info.target <= 0 || info.target == info.current)
        return false;

    // Once per device, rate and target: a device that opens at another size
    // than asked is not re-opened at every change message.
    const auto attempt = bufferDeviceKey (info.deviceTypeName, info.outputDeviceName) + "|" + juce::String (info.sampleRate) + "|"
                         + juce::String (info.target);
    if (attempt == lastBufferAttempt)
        return false;
    lastBufferAttempt = attempt;

    const juce::ScopedValueSetter<bool> busy (applyingBuffer, true);
    const auto previous = deviceManager.getAudioDeviceSetup();
    auto setup = previous;
    setup.bufferSize = info.target;
    // Not "chosen": the saved device state keeps what the user picked.
    const auto error = deviceManager.setAudioDeviceSetup (setup, false);
    auto* device = deviceManager.getCurrentAudioDevice();
    if (error.isNotEmpty() || device == nullptr)
    {
        // JUCE closed the device: back to the size it ran at.
        auto restore = previous;
        restore.bufferSize = info.current;
        deviceManager.setAudioDeviceSetup (restore, false);
        if (deviceManager.getCurrentAudioDevice() == nullptr && managingDevice)
            scheduleRecovery ("the output could not be re-opened at " + juce::String (info.target) + " samples: " + error,
                              recoveryTiming.firstRetryMs, true);
        return false;
    }
    if (device->getCurrentBufferSizeSamples() == info.target)
        lastBufferAttempt = {}; // done: a later re-open at another size is put right again
    return true;
}

bool AudioEngineHost::raiseBufferOneStep()
{
    JUCE_ASSERT_MESSAGE_THREAD
    const auto info = getBufferInfo();
    if (! info.deviceOpen || ! info.managed || ! info.automatic || autoBufferPending)
        return false;
    const auto next = buffer::nextLarger (info.available, info.current, info.deviceDefault);
    if (! next.has_value())
        return false;
    bufferFloors[bufferDeviceKey (info.deviceTypeName, info.outputDeviceName)] = *next;
    lastBufferAttempt = {};
    applyBufferPolicy();
    if (onBufferChoiceChanged != nullptr)
        onBufferChoiceChanged();
    return true;
}

// =============================================================================
// Latency probe (docs/11 E42d)
// =============================================================================
bool AudioEngineHost::startLatencyProbe (std::vector<std::unique_ptr<latency::ProbeSession>>& sessions)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (sessions.empty() || sessions.size() > static_cast<size_t> (kMaxProbeSessions) || probeFront() >= 0
        || ! callbackRunning.load (std::memory_order_acquire))
        return false;
    for (const auto& s : sessions)
        if (s == nullptr)
            return false;
    // In order: the callback plays the first slot's session before it looks
    // at the second.
    for (size_t i = 0; i < sessions.size(); ++i)
    {
        probeOwned[i] = std::move (sessions[i]);
        probeLive[i].store (probeOwned[i].get(), std::memory_order_seq_cst);
    }
    sessions.clear();
    return true;
}

bool AudioEngineHost::startLatencyProbe (std::unique_ptr<latency::ProbeSession>& session)
{
    std::vector<std::unique_ptr<latency::ProbeSession>> one;
    one.push_back (std::move (session));
    if (startLatencyProbe (one))
        return true;
    session = std::move (one.front());
    return false;
}

int AudioEngineHost::probeFront() const noexcept
{
    for (int i = 0; i < kMaxProbeSessions; ++i)
        if (probeOwned[static_cast<size_t> (i)] != nullptr)
            return i;
    return -1;
}

const latency::ProbeSession* AudioEngineHost::getLatencyProbe() const noexcept
{
    const int i = probeFront();
    return i >= 0 ? probeOwned[static_cast<size_t> (i)].get() : nullptr;
}

bool AudioEngineHost::isLatencyProbeLive() const noexcept
{
    const int i = probeFront();
    return i >= 0 && probeLive[static_cast<size_t> (i)].load (std::memory_order_acquire) != nullptr;
}

void AudioEngineHost::unpublishLatencyProbes() noexcept
{
    for (auto& slot : probeLive)
        slot.store (nullptr, std::memory_order_seq_cst);
}

void AudioEngineHost::cancelLatencyProbe() noexcept
{
    for (auto& session : probeOwned)
        if (session != nullptr)
            session->requestCancel();
}

std::unique_ptr<latency::ProbeSession> AudioEngineHost::takeLatencyProbe()
{
    JUCE_ASSERT_MESSAGE_THREAD
    const int front = probeFront();
    if (front < 0)
        return nullptr;
    const auto i = static_cast<size_t> (front);
    const bool stopped = ! callbackRunning.load (std::memory_order_acquire) || probeLive[i].load (std::memory_order_acquire) == nullptr;
    if (! probeOwned[i]->finished() && ! stopped)
        return nullptr;
    // Like a capture slot: unpublish, then wait until a callback that may
    // have read the pointer has returned (bounded; retried later if the audio
    // thread is stalled). The next slot's session keeps playing meanwhile.
    probeLive[i].store (nullptr, std::memory_order_seq_cst);
    uint64_t counter = 0;
    if (! waitForAudioThreadToPass (counter))
        return nullptr;
    return std::move (probeOwned[i]);
}

std::array<int, flub::kMaxChannels> AudioEngineHost::deviceInputOrder (const juce::String& deviceTypeName, int stripChannels) noexcept
{
    std::array<int, flub::kMaxChannels> order {};
    const bool permute = usesAlsaSurroundOrder (isAlsaDeviceType (deviceTypeName), stripChannels);
    for (int c = 0; c < flub::kMaxChannels; ++c)
        order[static_cast<size_t> (c)] = permute && c < stripChannels ? kAlsaSurroundOrder[static_cast<size_t> (c)] : c;
    return order;
}

// =============================================================================
// Device channel order (docs/11 E27 step 4)
// =============================================================================
bool AudioEngineHost::orderFromPositions (const flub::platform::SpeakerPosition* positions, int stripChannels,
                                          std::array<int, flub::kMaxChannels>& order) noexcept
{
    using P = flub::platform::SpeakerPosition;
    // The engine's layouts; a slot may accept two positions (5.1's surround
    // pair is side or rear, depending on who named it).
    struct Slot
    {
        P a, b;
    };
    static constexpr Slot kStereo[] = { { P::FL, P::FL }, { P::FR, P::FR } };
    static constexpr Slot kFiveOne[] = { { P::FL, P::FL }, { P::FR, P::FR }, { P::FC, P::FC }, { P::LFE, P::LFE }, { P::SL, P::RL }, { P::SR, P::RR } };
    static constexpr Slot kSevenOne[] = { { P::FL, P::FL }, { P::FR, P::FR }, { P::FC, P::FC }, { P::LFE, P::LFE },
                                          { P::RL, P::RL }, { P::RR, P::RR }, { P::SL, P::SL }, { P::SR, P::SR } };
    const Slot* layout = stripChannels == 2 ? kStereo : stripChannels == 6 ? kFiveOne : stripChannels == 8 ? kSevenOne : nullptr;
    if (positions == nullptr || layout == nullptr)
        return false;

    std::array<int, flub::kMaxChannels> result {};
    for (int c = 0; c < flub::kMaxChannels; ++c)
        result[static_cast<size_t> (c)] = c;
    uint32_t used = 0;
    for (int c = 0; c < stripChannels; ++c)
    {
        const Slot slot = layout[c];
        int found = -1;
        for (int d = 0; d < stripChannels; ++d)
        {
            if (positions[d] == P::Unknown || (positions[d] != slot.a && positions[d] != slot.b))
                continue;
            if (found >= 0)
                return false; // two channels claim the slot
            found = d;
        }
        if (found < 0 || (used & (1u << found)) != 0)
            return false;
        used |= 1u << found;
        result[static_cast<size_t> (c)] = found;
    }
    order = result;
    return true;
}

flub::platform::SpeakerPosition AudioEngineHost::positionFromChannelName (const juce::String& name)
{
    using P = flub::platform::SpeakerPosition;
    auto token = name.fromLastOccurrenceOf (":", false, false).trim().toLowerCase();
    for (const char* prefix : { "monitor_", "capture_", "playback_", "input_", "output_" })
        if (token.startsWith (prefix))
        {
            token = token.substring (static_cast<int> (std::strlen (prefix)));
            break;
        }
    token = token.replaceCharacter ('-', '_').replaceCharacter (' ', '_');

    struct Name
    {
        const char* text;
        P position;
    };
    static constexpr Name kNames[] = {
        { "fl", P::FL },           { "front_left", P::FL },    { "fr", P::FR },          { "front_right", P::FR },
        { "fc", P::FC },           { "front_center", P::FC },  { "front_centre", P::FC }, { "center", P::FC },
        { "centre", P::FC },       { "lfe", P::LFE },          { "rl", P::RL },          { "rear_left", P::RL },
        { "bl", P::RL },           { "back_left", P::RL },     { "rr", P::RR },          { "rear_right", P::RR },
        { "br", P::RR },           { "back_right", P::RR },    { "sl", P::SL },          { "side_left", P::SL },
        { "sr", P::SR },           { "side_right", P::SR },
    };
    for (const auto& n : kNames)
        if (token == n.text)
            return n.position;
    return P::Unknown;
}

void AudioEngineHost::readDeviceChannelMap (juce::AudioIODevice& device)
{
    // Positions of all the device's input channels up to the last active one,
    // then those of the active ones: the callback receives only those.
    const auto activeInputs = device.getActiveInputChannels();
    const int total = std::max (0, activeInputs.getHighestBit() + 1);
    std::vector<flub::platform::SpeakerPosition> positions (static_cast<size_t> (total), flub::platform::SpeakerPosition::Unknown);
    const auto anyKnown = [] (const std::vector<flub::platform::SpeakerPosition>& p)
    { return std::any_of (p.begin(), p.end(), [] (auto v) { return v != flub::platform::SpeakerPosition::Unknown; }); };

    inputChannelMap = InputChannelMap::None;
    if (channelMapQuery != nullptr && total > 0)
    {
        // A JUCE device is named after its output when it has one: the input's
        // own name is in the device manager's setup (message thread).
        juce::String inputName;
        if (juce::MessageManager::existsAndIsCurrentThread() && deviceManager.getCurrentAudioDevice() == &device)
            inputName = deviceManager.getAudioDeviceSetup().inputDeviceName;
        if (inputName.isEmpty())
            inputName = device.getName();
        auto backend = channelMapQuery (device.getTypeName(), inputName, total);
        if (static_cast<int> (backend.size()) == total && anyKnown (backend))
        {
            positions = std::move (backend);
            inputChannelMap = InputChannelMap::Backend;
        }
    }
    if (inputChannelMap == InputChannelMap::None)
    {
        const auto names = device.getInputChannelNames();
        for (int i = 0; i < total && i < names.size(); ++i)
            positions[static_cast<size_t> (i)] = positionFromChannelName (names[i]);
        if (anyKnown (positions))
            inputChannelMap = InputChannelMap::ChannelNames;
    }

    int k = 0;
    for (int i = 0; i < total && k < kMaxDeviceInputs; ++i)
        if (activeInputs[i])
            deviceInputPositions[static_cast<size_t> (k++)].store (static_cast<uint8_t> (positions[static_cast<size_t> (i)]), std::memory_order_relaxed);
    for (; k < kMaxDeviceInputs; ++k)
        deviceInputPositions[static_cast<size_t> (k)].store (0, std::memory_order_relaxed);
}

std::array<int, flub::kMaxChannels> AudioEngineHost::stripInputOrder (int firstInput, int stripChannels, bool alsaOrder) const noexcept
{
    std::array<flub::platform::SpeakerPosition, flub::kMaxChannels> positions {};
    const int channels = std::min (stripChannels, flub::kMaxChannels);
    for (int c = 0; c < channels; ++c)
        if (const int d = firstInput + c; d >= 0 && d < kMaxDeviceInputs)
            positions[static_cast<size_t> (c)] = static_cast<flub::platform::SpeakerPosition> (deviceInputPositions[static_cast<size_t> (d)].load (std::memory_order_relaxed));

    std::array<int, flub::kMaxChannels> order {};
    if (orderFromPositions (positions.data(), channels, order))
        return order;
    const bool permute = usesAlsaSurroundOrder (alsaOrder, channels);
    for (int c = 0; c < flub::kMaxChannels; ++c)
        order[static_cast<size_t> (c)] = permute && c < channels ? kAlsaSurroundOrder[static_cast<size_t> (c)] : c;
    return order;
}

// =============================================================================
// Real-time audio thread (docs/11 E44)
// =============================================================================
juce::String AudioThreadRealtime::describe() const
{
    switch (state)
    {
        case State::RealTime:
        {
            juce::String text = "real-time (" + (policy.isNotEmpty() ? policy : juce::String ("yes"));
            if (priority > 0)
                text << " " << priority;
            if (via == Via::RealtimeKit)
                text << " via rtkit";
            else if (via == Via::Backend)
                text << ", the audio server's thread";
            return text + ")";
        }
        case State::NotRealTime:
            return "NOT real-time: " + detail;
        case State::Unknown:
            break;
    }
    return "real-time status unknown";
}

void AudioEngineHost::setRealtimeHooks (RealtimeHooks hooks)
{
    JUCE_ASSERT_MESSAGE_THREAD
    realtimeHooks = std::move (hooks);
    realtimeHooksInjected = true;
}

void AudioEngineHost::serviceAudioThreadRealtime()
{
    using State = AudioThreadRealtime::State;
    using Via = AudioThreadRealtime::Via;
    const uint32_t generation = audioThreadGeneration.load (std::memory_order_acquire);
    if (generation == realtimeGenerationSeen)
        return;
    realtimeGenerationSeen = generation;
    const uint64_t threadId = audioThreadId.load (std::memory_order_relaxed);
    const bool promoted = audioThreadPromoted.load (std::memory_order_relaxed);

    AudioThreadRealtime next;
    next.rtkitRequests = audioThreadRealtime.rtkitRequests;
    const auto previousVia = threadId == realtimeThreadId ? audioThreadRealtime.via : Via::None;
    realtimeThreadId = threadId;

    const auto realTime = [&next] (Via via, const flub::platform::ThreadScheduling& s)
    {
        next.state = State::RealTime;
        next.via = via;
        next.policy = s.policy;
        next.priority = s.priority;
    };

    if (threadId == 0 || realtimeHooks.query == nullptr)
    {
        // No thread ids here (Windows, macOS): only the callback's own
        // promotion is known.
        if (promoted)
        {
            next.state = State::RealTime;
            next.via = Via::Promoted;
            next.policy = ownPromotionName();
        }
        audioThreadRealtime = next;
        return;
    }

    const auto now = realtimeHooks.query (threadId);
    if (! now.known)
    {
        audioThreadRealtime = next; // the thread has gone already
        return;
    }
    if (now.realtime)
    {
        realTime (promoted ? Via::Promoted : previousVia == Via::RealtimeKit ? Via::RealtimeKit : Via::Backend, now);
        audioThreadRealtime = next;
        return;
    }

    next.state = State::NotRealTime;
    next.policy = now.policy;
    // RealtimeKit is asked for a device thread only: never for the message
    // thread (a test or offline driver calling the callback itself), and with
    // the platform's own hooks only while a device this host opened runs.
    const bool deviceThread = threadId != osThreadId() && (realtimeHooksInjected || deviceManager.getCurrentAudioDevice() != nullptr);
    if (realtimeHooks.request == nullptr || ! deviceThread)
    {
        next.detail = juce::String ("the audio thread runs at ") + now.policy + " scheduling; " + kRealtimeFix;
        audioThreadRealtime = next;
        return;
    }

    ++next.rtkitRequests;
    const auto result = realtimeHooks.request (threadId, kRealtimeKitPriority);
    const auto after = realtimeHooks.query (threadId);
    if (after.known && after.realtime)
        realTime (Via::RealtimeKit, after);
    else if (result.outcome == flub::platform::RealtimeKitResult::Outcome::Granted)
        next.detail = juce::String ("RealtimeKit accepted the request, but the thread still runs at ") + (after.known ? after.policy : now.policy)
                      + " scheduling; " + kRealtimeFix;
    else
        next.detail = juce::String (result.message) + "; " + kRealtimeFix;
    audioThreadRealtime = next;
}
} // namespace flub::app
