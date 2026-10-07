// Flubsound Pro - the native PipeWire node as a JUCE device type (docs/11
// E48). See PipeWireDeviceType.h.
#if defined(__linux__) && defined(FLUB_HAS_PIPEWIRE) && FLUB_HAS_PIPEWIRE

#include "PipeWireDeviceType.h"

#include "PipeWireGraph.h"
#include "PipeWireLibrary.h"
#include "PipeWireNative.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace flub::platform::pipewire
{
namespace
{
class PipeWireAudioIODevice final : public juce::AudioIODevice, private NativeAudioNode::Callback, private juce::AsyncUpdater
{
public:
    PipeWireAudioIODevice() : juce::AudioIODevice (kDeviceName, kDeviceTypeName), node (NativeAudioNode::create())
    {
        for (const auto& strip : defaultStrips())
            for (const auto& position : strip.positions)
                inputNames.add (juce::String (strip.name) + ":" + juce::String (position));
        for (const auto& position : outputPositions (2))
            outputNames.add ("Output:" + juce::String (position));
    }

    ~PipeWireAudioIODevice() override { close(); } // no nodeError() or pending update after this

    juce::StringArray getOutputChannelNames() override { return outputNames; }
    juce::StringArray getInputChannelNames() override { return inputNames; }
    juce::Array<double> getAvailableSampleRates() override { return { 44100.0, 48000.0, 88200.0, 96000.0 }; }
    juce::Array<int> getAvailableBufferSizes() override { return { 64, 128, 256, 512, 1024, 2048 }; }
    int getDefaultBufferSize() override { return 256; }

    juce::String open (const juce::BigInteger& inputChannels, const juce::BigInteger& outputChannels, double sampleRate, int bufferSizeSamples) override
    {
        close();
        // Every input: they are the strips' sinks, not a hardware selection.
        // JUCE asks for its default channel count (the first 8 when the app
        // opens it, or when it is picked in Settings), which would cut the
        // Music, Chat and System strips off.
        juce::ignoreUnused (inputChannels);
        activeInputs.clear();
        activeInputs.setRange (0, inputNames.size(), true);
        activeOutputs = outputChannels;
        activeOutputs.setRange (outputNames.size(), juce::jmax (0, activeOutputs.getHighestBit() + 1 - outputNames.size()), false);

        NativeAudioNodeConfig config;
        config.outputChannels = outputNames.size();
        config.sampleRate = static_cast<uint32_t> (sampleRate > 0.0 ? sampleRate : 48000.0);
        config.maxBlockFrames = bufferSizeSamples > 0 ? bufferSizeSamples : getDefaultBufferSize();
        config.latency = latency;
        config.outputTarget = outputTarget;
        currentRate = static_cast<double> (config.sampleRate);
        currentBlock = config.maxBlockFrames;
        xrunBase.store (-1); // set by this run's first callback (nodeProcess)

        std::string error;
        if (! node->start (config, *this, error))
        {
            lastError = error;
            return lastError;
        }
        opened = true;
        lastError.clear();
        return {};
    }

    void close() override
    {
        stop();
        if (opened)
            node->stop(); // takes the loop lock, under which nodeError runs: none follows
        opened = false;
        // R1.2 review: an error of this run that the message thread has not
        // handled yet belongs to the node just stopped. JUCE re-opens the
        // same device object for a new rate or buffer size (open() closes
        // first), and the next run's error target must not get it.
        cancelPendingUpdate();
        const std::lock_guard<std::mutex> guard (errorMutex);
        pendingError.clear();
    }

    /** The node error waiting for the message thread (tests). */
    juce::String getPendingError()
    {
        const std::lock_guard<std::mutex> guard (errorMutex);
        return juce::String (pendingError);
    }

    bool isOpen() override { return opened; }

    void start (juce::AudioIODeviceCallback* newCallback) override
    {
        if (! opened || newCallback == nullptr || newCallback == activeCallback.load())
            return;
        stop();
        newCallback->audioDeviceAboutToStart (this);
        activeCallback.store (newCallback);
    }

    void stop() override
    {
        errorTarget.store (nullptr); // the next start's audioDeviceAboutToStart registers again
        auto* previous = activeCallback.exchange (nullptr);
        while (inCallback.load())
            std::this_thread::yield(); // message thread: the audio thread finishes its block
        if (previous != nullptr)
            previous->audioDeviceStopped();
    }

    bool isPlaying() override { return activeCallback.load() != nullptr; }
    juce::String getLastError() override { return lastError; }
    int getCurrentBufferSizeSamples() override { return currentBlock; }
    double getCurrentSampleRate() override { return currentRate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return activeOutputs; }
    juce::BigInteger getActiveInputChannels() const override { return activeInputs; }

    /** R1.2: the node's own count (pipewire::XrunCounter: cycles it finished
        after the next one was due, or missed), so AudioEngineHost::getStatus,
        the header's "xr" and the overload watchdog see PipeWire's xruns as
        they see a JUCE backend's; from this run's first callback on (0
        before it). */
    int getXRunCount() const noexcept override
    {
        if (! opened)
            return -1;
        const int base = xrunBase.load();
        return base < 0 ? 0 : juce::jmax (0, node->getXrunCount() - base);
    }

    /** None of its own: the node runs in the same graph cycle as the sinks
        it reads and the sink it plays to. What the path adds is the graph's
        quantum (the output device's period), which changes while the device
        runs (node.latency in place), so it is reported through
        getStatus().quantumFrames and shown as the graph quantum
        (AudioEngineHost::setGraphQuantumMs, docs/11 E42), not read once at
        start as a device latency. */
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }

    bool setLatency (NativeAudioNodeConfig::Latency newLatency)
    {
        latency = newLatency;
        return opened && node->setLatency (newLatency);
    }

    bool setOutputTarget (const std::string& sinkName)
    {
        outputTarget = sinkName;
        return opened && node->setOutputTarget (sinkName);
    }

    NativeAudioNodeStatus getStatus() const { return node->getStatus(); }

    void setErrorTarget (juce::AudioIODeviceCallback* target) { errorTarget.store (target); }

private:
    // NativeAudioNode::Callback
    void nodeStarting (double, int maxBlockFrames) override
    {
        // The active channels only, as JUCE devices pass them.
        activeInputIndex.clear();
        for (int i = 0; i < inputNames.size(); ++i)
            if (activeInputs[i])
                activeInputIndex.push_back (i);
        activeOutputIndex.clear();
        for (int o = 0; o < outputNames.size(); ++o)
            if (activeOutputs[o])
                activeOutputIndex.push_back (o);
        inputs.assign (activeInputIndex.size(), nullptr);
        outputs.assign (activeOutputIndex.size(), nullptr);
        juce::ignoreUnused (maxBlockFrames);
    }

    void nodeProcess (const float* const* nodeInputs, int numInputs, float* const* nodeOutputs, int numOutputs, int numFrames,
                      uint64_t timeNs) noexcept FLUB_NONBLOCKING override
    {
        inCallback.store (true);
        auto* callback = activeCallback.load();
        for (int o = 0; o < numOutputs; ++o)
            std::fill (nodeOutputs[o], nodeOutputs[o] + numFrames, 0.0f);
        if (callback != nullptr && xrunBase.load (std::memory_order_relaxed) < 0)
        {
            // R1.2 review: xruns count from this run's first callback on, and
            // its first cycles settle (the engine's first blocks run cold).
            // Before it the node joins and links into the graph and plays
            // silence while the host prepares the engine; CI's start-up
            // xruns left after the node's own settling (1 of 20, then 5 of
            // 20 loops) were all counted before the host's first callback.
            node->settleXruns();
            xrunBase.store (node->getXrunCount(), std::memory_order_relaxed);
        }
        if (callback != nullptr)
        {
            for (size_t i = 0; i < activeInputIndex.size(); ++i)
                inputs[i] = activeInputIndex[i] < numInputs ? nodeInputs[activeInputIndex[i]] : nodeInputs[0];
            for (size_t o = 0; o < activeOutputIndex.size(); ++o)
                outputs[o] = activeOutputIndex[o] < numOutputs ? nodeOutputs[activeOutputIndex[o]] : nodeOutputs[0];
            // The driver's time of this block (docs/11 E45: the callback
            // timing's intervals then follow the graph's cadence, and a split
            // quantum is not a burst of "late" callbacks).
            juce::AudioIODeviceCallbackContext context;
            context.hostTimeNs = timeNs != 0 ? &timeNs : nullptr;
            callback->audioDeviceIOCallbackWithContext (inputs.data(), static_cast<int> (inputs.size()), outputs.data(), static_cast<int> (outputs.size()),
                                                        numFrames, context);
        }
        inCallback.store (false);
    }

    void nodeStopped() override {}

    /** PipeWire's loop thread: hand the error to the message thread, where
        start() / stop() swap the callback, so it reaches the current one
        (AudioEngineHost shows it and re-opens the device once callbacks stop:
        the E51 recovery, which brings the node back when the server is).
        It goes to the registered error target (setDeviceErrorTarget), since
        JUCE 9.0.2's AudioDeviceManager drops audioDeviceError from the
        callback it starts devices with; to that callback only without one. */
    void nodeError (const std::string& message) override
    {
        {
            const std::lock_guard<std::mutex> guard (errorMutex);
            pendingError = message;
        }
        triggerAsyncUpdate();
    }

    void handleAsyncUpdate() override
    {
        juce::String message;
        {
            const std::lock_guard<std::mutex> guard (errorMutex);
            message = juce::String (pendingError);
            pendingError.clear();
        }
        if (message.isEmpty())
            return;
        lastError = message;
        if (auto* target = errorTarget.load())
            target->audioDeviceError (message);
        else if (auto* callback = activeCallback.load())
            callback->audioDeviceError (message);
    }

    std::unique_ptr<NativeAudioNode> node;
    juce::StringArray inputNames, outputNames;
    juce::BigInteger activeInputs, activeOutputs;
    NativeAudioNodeConfig::Latency latency = NativeAudioNodeConfig::Latency::Balanced;
    std::string outputTarget;
    double currentRate = 48000.0;
    int currentBlock = 256;
    bool opened = false;
    juce::String lastError;

    // Sized in nodeStarting, before the first nodeProcess:
    std::vector<int> activeInputIndex, activeOutputIndex;
    std::vector<const float*> inputs;
    std::vector<float*> outputs;

    std::atomic<juce::AudioIODeviceCallback*> activeCallback { nullptr };
    std::atomic<bool> inCallback { false };
    std::atomic<int> xrunBase { -1 }; // the node's count at this run's first callback; -1 = none yet

    std::mutex errorMutex; // nodeError (loop thread) -> handleAsyncUpdate (message thread)
    std::string pendingError;
    std::atomic<juce::AudioIODeviceCallback*> errorTarget { nullptr }; // setDeviceErrorTarget
};

class PipeWireAudioIODeviceType final : public juce::AudioIODeviceType
{
public:
    PipeWireAudioIODeviceType() : juce::AudioIODeviceType (kDeviceTypeName) {}

    void scanForDevices() override {}
    juce::StringArray getDeviceNames (bool wantInputNames) const override { return { wantInputNames ? kInputDeviceName : kDeviceName }; }
    int getDefaultDeviceIndex (bool /*forInput*/) const override { return 0; }
    int getIndexOfDevice (juce::AudioIODevice* device, bool /*asInput*/) const override
    {
        return dynamic_cast<PipeWireAudioIODevice*> (device) != nullptr ? 0 : -1;
    }
    bool hasSeparateInputsAndOutputs() const override { return true; }

    juce::AudioIODevice* createDevice (const juce::String& outputDeviceName, const juce::String& inputDeviceName) override
    {
        if ((outputDeviceName.isNotEmpty() && outputDeviceName != kDeviceName) || (inputDeviceName.isNotEmpty() && inputDeviceName != kInputDeviceName))
            return nullptr;
        return new PipeWireAudioIODevice();
    }
};
} // namespace

std::unique_ptr<juce::AudioIODeviceType> createDeviceType() { return std::make_unique<PipeWireAudioIODeviceType>(); }

bool addDeviceType (juce::AudioDeviceManager& manager)
{
    // R1.2: libpipewire is opened at run time; without it there is no
    // "PipeWire" entry and the app keeps JUCE's ALSA / JACK types. With it
    // the entry is offered even where PipeWire does not play the audio (a
    // PulseAudio desktop with the library installed); the first start then
    // keeps ALSA (serverPlaysAudio, AudioEngineHost::openDevice).
    if (! library().loaded)
        return false;
    for (auto* type : manager.getAvailableDeviceTypes()) // creates JUCE's own first
        if (type->getTypeName() == kDeviceTypeName)
            return false;
    manager.addAudioDeviceType (createDeviceType());
    return true;
}

bool setDeviceLatency (juce::AudioIODevice* device, NativeAudioNodeConfig::Latency latency)
{
    auto* pipewireDevice = dynamic_cast<PipeWireAudioIODevice*> (device);
    return pipewireDevice != nullptr && pipewireDevice->setLatency (latency);
}

bool setDeviceOutputTarget (juce::AudioIODevice* device, const std::string& sinkName)
{
    auto* pipewireDevice = dynamic_cast<PipeWireAudioIODevice*> (device);
    return pipewireDevice != nullptr && pipewireDevice->setOutputTarget (sinkName);
}

NativeAudioNodeStatus getDeviceStatus (juce::AudioIODevice* device)
{
    auto* pipewireDevice = dynamic_cast<PipeWireAudioIODevice*> (device);
    return pipewireDevice != nullptr ? pipewireDevice->getStatus() : NativeAudioNodeStatus {};
}

bool setDeviceErrorTarget (juce::AudioIODevice* device, juce::AudioIODeviceCallback* target)
{
    auto* pipewireDevice = dynamic_cast<PipeWireAudioIODevice*> (device);
    if (pipewireDevice == nullptr)
        return false;
    pipewireDevice->setErrorTarget (target);
    return true;
}

juce::String getPendingDeviceError (juce::AudioIODevice* device)
{
    auto* pipewireDevice = dynamic_cast<PipeWireAudioIODevice*> (device);
    return pipewireDevice != nullptr ? pipewireDevice->getPendingError() : juce::String();
}

bool serverPlaysAudio (std::string& why)
{
    Session probe;
    if (! probe.connect ("Flubsound Pro (probe)", why)) // not installed, or no server
        return false;
    probe.lock();
    const bool plays = playsAudio (probe.graph(), probe.defaultSink(), defaultStrips());
    probe.unlock();
    probe.disconnect();
    if (! plays)
    {
        why = "PipeWire runs here but has no audio output (another sound server, e.g. PulseAudio, may own the sound card)";
        std::fprintf (stderr, "Flubsound: PipeWire: %s; the first start keeps the default device type\n", why.c_str());
    }
    return plays;
}
} // namespace flub::platform::pipewire

#endif
