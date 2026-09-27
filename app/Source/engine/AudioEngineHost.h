// Flubsound Pro - the audio engine host: device I/O + multi-strip mixer.
//
// Owns the juce::AudioDeviceManager and the flub::MixEngine and is the
// device's AudioIODeviceCallback.
//
//   device inputs (virtual cable / Flubsound endpoint capture side) --+
//                                                                    +--> strip buffers
//   per-app ProcessLoopbackCapture --> DriftCompensatedFifo (1/app) --+     (preallocated)
//                                                                              |
//                                                                  MixEngine::process
//                                                                              |
//                                                                  device outputs 0/1
//
// Strips (default layout, configurable): 0 "Game" (7.1, 8 ch), 1 "Music" (2),
// 2 "Chat" (2), 3 "System" (2). Each strip has its own ParameterStore
// (profile) and ProcessingChain inside the MixEngine.
//
// THREADING CONTRACT
//   * The audio callback is RT-safe: no allocation, locks, I/O or logging.
//     It reads parameters from the ParameterStores (atomics), strip gain /
//     mute / ceiling from atomics owned by this class, and captured audio from
//     SPSC rings. Telemetry goes out through each chain's MeterBus.
//   * The engine STRUCTURE (MixEngine::configure: strips, chains, buffers) is
//     only ever changed on the MESSAGE THREAD, while the callback is detached:
//       - device (re)start: audioDeviceAboutToStart() runs on the message
//         thread in practice and configures synchronously. If a backend ever
//         calls it from another thread, the callback outputs silence and the
//         configure is posted to the message thread (AsyncUpdater).
//       - reconfigure(): removeAudioCallback -> configure -> addAudioCallback.
//         This is a brief, explicit dropout (one device restart of the
//         callback, typically 5-30 ms of silence). It is used for strip layout
//         changes and structural parameters (latency profile). A crossfaded,
//         double-buffered engine swap is a roadmap item.
//     Consequently message-thread code may use MixEngine::chain(i) (meters,
//     analyser taps) freely, but must RE-FETCH it after a reconfiguration:
//     MixEngine::configure re-creates the ProcessingChain objects. The
//     ParameterStore objects survive reconfiguration (they only disappear if a
//     strip is removed from the layout).
//   * Per-app captures: each capture has its own DriftCompensatedFifo in a
//     fixed slot array (the capture thread is the producer, the audio thread
//     the consumer). Slots are only (re)allocated after the audio thread has
//     provably stopped reading them (see waitForAudioThreadToPass()).
#pragma once

#include "DriftCompensatedFifo.h"
#include "flub/engine/MixEngine.h"
#include "platform/PlatformServices.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace flub::app
{
struct LatencyInfo
{
    double sampleRate = 0.0;
    int blockSize = 0;
    int deviceInputSamples = 0, deviceOutputSamples = 0, engineSamples = 0;
    double deviceInputMs = 0.0, deviceOutputMs = 0.0, engineMs = 0.0;
    double captureBufferMs = 0.0; // extra FIFO latency of per-app capture streams (max over captures)
    double totalMs = 0.0;         // device input + device output + engine
};

struct EngineStatus
{
    bool deviceOpen = false;
    bool running = false;          // callback attached and engine ready
    juce::String deviceName, deviceTypeName;
    int numInputChannels = 0, numOutputChannels = 0;
    double cpuLoad = 0.0;          // 0..1, JUCE's callback load measurement
    int xruns = -1;                // the device's own xrun count; -1 if it does not report them
    int glitches = 0;              // xruns (when reported) + callbacks that overran their buffer
                                   // period (juce::AudioDeviceManager::getXRunCount); 0 if no device
    uint64_t callbacks = 0;
};

/** Offline source of strip audio (headless rendering, screenshots, tests). */
class StripSignalSource
{
public:
    virtual ~StripSignalSource() = default;

    /** Fill `block` (the strip's channel count) for `strip`. Return false if the
        strip has no signal (the host then treats it like an unfed strip). */
    virtual bool renderStrip (int strip, const flub::AudioBlock& block) = 0;
};

class AudioEngineHost final : public juce::AudioIODeviceCallback, private juce::AsyncUpdater, private juce::Timer
{
public:
    static constexpr int kMaxCaptures = 16;
    static constexpr int kMaxStrips = flub::MixEngine::kMaxStrips;

    /** Game (7.1), Music, Chat, System. */
    static std::vector<flub::StripConfig> defaultStripLayout();

    AudioEngineHost();
    ~AudioEngineHost() override;

    // =========================================================================
    // Device (message thread)
    // =========================================================================
    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }

    /** Opens the device (saved XML state or system default; without saved
        state Windows prefers the "Windows Audio (Low Latency Mode)" type) and
        attaches the engine. Returns an error message, empty on success. */
    juce::String openDevice (const juce::XmlElement* savedState, int maxInputChannels = 8, int maxOutputChannels = 2);

    /** Detaches the engine and closes the device. */
    void closeDevice();

    // =========================================================================
    // Engine structure (message thread)
    // =========================================================================
    /** Replaces the strip layout (1..kMaxStrips strips) and reconfigures. */
    void setStripLayout (std::vector<flub::StripConfig> newLayout);
    /** Current layout, including the live strip gain / mute values. */
    std::vector<flub::StripConfig> getStripLayout() const;
    int getNumStrips() const noexcept { return mixEngine.getNumStrips(); }

    /** Re-creates the engine for the current device format. Brief dropout. */
    void reconfigure();

    /** A structural parameter changed (latency profile): reconfigure() needed.
        Polled by this class at 5 Hz on the message thread, which then calls
        reconfigure() by itself. */
    bool needsReprepare() const noexcept { return mixEngine.needsReprepare(); }

    /** Increments after every MixEngine::configure (chains were re-created). */
    uint32_t getStructureGeneration() const noexcept { return structureGeneration.load (std::memory_order_acquire); }

    /** Direct engine access. params()/chain() are message-thread safe; do not
        call configure()/process()/setStrip*() on it - use this class. */
    flub::MixEngine& getMixEngine() noexcept { return mixEngine; }

    /** Called (asynchronously) on the message thread after every
        (re)configuration. */
    std::function<void()> onEngineConfigured;

    /** Called on the message thread when the device reports an error. */
    std::function<void (const juce::String&)> onDeviceError;
    juce::String getLastDeviceError() const;

    // =========================================================================
    // Strip mix controls (any thread; applied click-free on the audio thread)
    // =========================================================================
    void setStripGainDb (int strip, float gainDb) noexcept;
    float getStripGainDb (int strip) const noexcept;
    void setStripMuted (int strip, bool muted) noexcept;
    bool isStripMuted (int strip) const noexcept;
    void setMasterCeilingDb (float db) noexcept;
    float getMasterCeilingDb() const noexcept { return masterCeilingDb.load (std::memory_order_relaxed); }

    /** True while the strip receives audio (device input, a capture, or the
        silence hang-over that lets tails decay after a source stops). */
    bool isStripActive (int strip) const noexcept;

    // =========================================================================
    // Sources
    // =========================================================================
    /** Device input -> strip mapping. firstChannels[strip] >= 0 feeds the strip
        from device inputs [first, first + stripChannels) (missing channels are
        silent); -1 = the strip is not fed by the device. Typical setups:
          Windows / macOS MVP : one virtual cable -> {0, -1, -1, -1}
          Linux (JACK / PipeWire monitors): {0, 8, 10, 12}
        Any thread (atomics, applied at the next block). */
    void setDeviceInputMap (const std::array<int, kMaxStrips>& firstChannels) noexcept;
    std::array<int, kMaxStrips> getDeviceInputMap() const noexcept;

    /** Convenience: only `strip` is fed, from `firstDeviceChannel`; -1 = none. */
    void setDeviceInputRouting (int strip, int firstDeviceChannel = 0) noexcept;
    /** First strip fed by the device inputs, -1 if none. */
    int getDeviceInputStrip() const noexcept;

    /** Factory for per-process capture objects. Defaults to the platform
        implementation (platform_bridge); tests / alternative capture sources
        can inject their own. Message thread, before starting captures. */
    using CaptureFactory = std::function<std::unique_ptr<flub::platform::ProcessLoopbackCapture>()>;
    void setCaptureFactory (CaptureFactory factory);

    /** Starts capturing a process (tree) into a strip. Message thread. Returns
        a capture id >= 0, or -1 with `error` set. */
    int startProcessCapture (int strip, uint32_t processId, juce::String& error);
    void stopProcessCapture (int captureId);
    void stopAllCaptures();
    /** Moves a running capture to another strip (message thread). */
    void setCaptureStrip (int captureId, int strip);

    struct CaptureInfo
    {
        int id = -1;
        int strip = -1;
        uint32_t processId = 0;
        bool running = false;
        DriftCompensatedFifo::Stats stats;
    };
    std::vector<CaptureInfo> getCaptures() const;

    // =========================================================================
    // Offline rendering (message thread, no device open)
    // =========================================================================
    /** Configures the engine without a device. Used at start-up (so strips and
        parameter stores exist before any device opens) and for headless
        rendering. */
    void prepareOffline (double sampleRate, int blockSize);

    /** Runs numSamples through the engine with strip inputs from `source`.
        outputs (optional) receives the stereo master output. */
    void renderOffline (StripSignalSource& source, int numSamples, float* const* outputs = nullptr, int numOutputs = 0);

    // =========================================================================
    // Telemetry (message thread)
    // =========================================================================
    LatencyInfo getLatencyInfo() const;
    EngineStatus getStatus() const;
    double getSampleRate() const noexcept { return currentSampleRate; }
    int getBlockSize() const noexcept { return currentBlockSize; }

    // =========================================================================
    // juce::AudioIODeviceCallback
    // =========================================================================
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels, int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String& errorMessage) override;

private:
    struct CaptureSlot
    {
        DriftCompensatedFifo fifo;
        std::unique_ptr<flub::platform::ProcessLoopbackCapture> capture;
        std::atomic<int> strip { -1 };
        std::atomic<bool> live { false }; // audio thread may read the FIFO
        uint32_t processId = 0;
    };

    void handleAsyncUpdate() override;
    void timerCallback() override;
    void configureEngine (double sampleRate, int blockSize);
    void processBlock (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int numSamples,
                       StripSignalSource* offlineSource) noexcept;
    void applyPendingMixSettings() noexcept;
    void waitForAudioThreadToPass();
    void releaseSlot (CaptureSlot& slot);

    juce::AudioDeviceManager deviceManager;
    flub::MixEngine mixEngine;
    std::vector<flub::StripConfig> layout;

    // ---- Audio-thread working set (sized by configureEngine) -------------------
    std::array<flub::AudioBuffer, kMaxStrips> stripBuffers;
    std::array<flub::AudioBlock, kMaxStrips> stripBlocks {};
    std::array<const flub::AudioBlock*, kMaxStrips> stripInputs {};
    std::array<int, kMaxStrips> hangoverRemaining {};
    std::array<float, kMaxStrips> appliedGainDb {};
    std::array<bool, kMaxStrips> appliedMuted {};
    float appliedCeilingDb = -1.0f;
    flub::AudioBuffer mixOutput;
    int maxBlock = 512, numStripsConfigured = 0, hangoverSamples = 24000;
    juce::Thread::ThreadID promotedThread = nullptr;
    void* promotionHandle = nullptr;

    // ---- Cross-thread state ---------------------------------------------------------
    std::array<std::atomic<float>, kMaxStrips> stripGainDb {};
    std::array<std::atomic<bool>, kMaxStrips> stripMuted {};
    std::array<std::atomic<bool>, kMaxStrips> stripActive {};
    std::atomic<float> masterCeilingDb { -1.0f };
    std::array<std::atomic<int>, kMaxStrips> deviceInputFirst {};
    std::atomic<bool> engineReady { false }, callbackRunning { false };
    std::atomic<bool> configurePending { false }, notifyPending { false }, errorPending { false };
    std::atomic<uint64_t> callbackCounter { 0 };
    std::atomic<uint32_t> structureGeneration { 0 };
    std::array<CaptureSlot, kMaxCaptures> captureSlots;

    // ---- Message-thread state ----------------------------------------------------------
    CaptureFactory captureFactory;
    double currentSampleRate = 48000.0;
    int currentBlockSize = 512;
    int deviceInputLatency = 0, deviceOutputLatency = 0;
    double pendingSampleRate = 0.0;
    int pendingBlockSize = 0, pendingInputLatency = 0, pendingOutputLatency = 0;
    bool callbackAttached = false;
    juce::CriticalSection errorLock;
    juce::String lastDeviceError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngineHost)
};
} // namespace flub::app
