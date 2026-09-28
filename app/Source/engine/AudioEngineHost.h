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
//     never changed on the audio thread. A structural change builds a NEW
//     engine instance (MixEngine + this class's per-engine working set) on
//     the MESSAGE THREAD and never modifies one the audio thread can see:
//       - device (re)start (sample rate / buffer size / device change):
//         audioDeviceAboutToStart() runs on the message thread in practice
//         and replaces the engine synchronously - the device is stopped, so
//         nothing is processing. If a backend ever calls it from another
//         thread, the callback outputs silence and the configure is posted to
//         the message thread (AsyncUpdater). The first kSwapFadeMs of a
//         started device fade in (no step from silence).
//       - reconfigure() while the device runs (strip layout, latency profile,
//         neural model install / removal, anything MixEngine::needsReprepare()
//         reports): the CROSSFADED ENGINE SWAP. The new instance is configured with
//         MixEngine::configureFrom (it shares the running engine's
//         ParameterStores) while the old one keeps playing, and is handed to
//         the audio thread through one atomic pointer (pendingSwap). The
//         audio thread takes it at the start of a callback, runs both engines
//         on the same input while the new one pre-rolls (its latency +
//         kSwapSettleMs, so its delay lines hold real signal; a strip the new
//         layout removed gets silence in the old engine, so its tail decays
//         and fades out with it instead of being cut), then either
//           equal latency : crossfades old -> new over kSwapFadeMs with
//                           equal-GAIN raised-cosine curves (gOld + gNew = 1;
//                           both carry the same programme, so equal power
//                           would bump +3 dB and could pass the master
//                           limiter's ceiling; equal gain never exceeds
//                           max(|old|, |new|)), or
//           latency change: fades old out over kSwapFadeMs ending where the
//                           new one fades in over kSwapFadeMs (a 2 x 10 ms
//                           dip: overlapping two copies offset in time would
//                           comb-filter, and a sine could cancel; the
//                           programme jumps by the latency difference inside
//                           the dip, which no crossfade can avoid).
//         The old instance goes back through a second atomic (retiredSwap)
//         and is destroyed on the message thread (timer / next reconfigure):
//         the audio thread never allocates or frees. One swap runs at a time;
//         a newer request replaces a pending one that the audio thread has
//         not taken yet (disposed at once, it never ran).
//     Consequently message-thread code may use getMixEngine() / chain(i)
//     (meters, analyser taps) freely, but must RE-FETCH them after a
//     reconfiguration (getStructureGeneration() changes): the newest engine
//     has new ProcessingChain objects. The ParameterStore objects survive
//     reconfiguration (they only disappear if a strip is removed from the
//     layout).
//   * Per-app captures: each capture has its own DriftCompensatedFifo in a
//     fixed slot array (the capture thread is the producer, the audio thread
//     the consumer). Slots are only (re)allocated after the audio thread has
//     provably stopped reading them: releaseSlot() waits (bounded) for one
//     callback to finish; if the device callback is stalled past the bound,
//     the slot is QUARANTINED and no capture reuses it until a callback has
//     completed (see waitForAudioThreadToPass()). Captures run at the device
//     rate (FIFO nominal ratio 1): when the device rate changes, every
//     running capture is restarted at the new rate on the message thread
//     (restartCapturesAtDeviceRate()), keeping its capture id.
//
// DEVICE SAFETY (docs/11 E51 Phase A)
//   * Loopback-pair guard: at every device start the output and input device
//     names are checked (isLoopbackPair: CABLE Input <-> CABLE Output, the
//     same Voicemeeter bus, BlackHole / Soundflower / Loopback as both ends,
//     a sink and its ".monitor"). A pair whose input feeds a strip closes a
//     feedback loop through the engine (typical: a wireless headset drops, JUCE falls back to the
//     system default output, which in the cable setup IS CABLE Input). The
//     output is then held at silence from the first sample (or ramped down
//     over kSwapFadeMs if the guard trips while running) and the engine is
//     frozen, so no protection loop (governor, AutoLevel) accumulates
//     step-downs. A new device start builds a fresh engine anyway, so every
//     device change starts from clean protection state. allowLoopbackPair()
//     is the per-pair override for deliberate setups.
//   * getDeviceSafetyState() / onDeviceSafetyChanged: what a banner shows
//     (loopback pair, device error, device failed to open), cleared by the
//     next successful device start.
//   * getLatencyInfo().valid is false while no device runs (or the guard
//     holds the output): show "--", never a stale number.
#pragma once

#include "DriftCompensatedFifo.h"
#include "flub/engine/MixEngine.h"
#include "platform/PlatformServices.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace flub::app
{
struct LatencyInfo
{
    /** One strip's output latency in the engine (docs/11 E42a; the
        MixEngine's getStripLatencySamples / getStripPaddingSamples):
        ownSamples is what the strip's own chain plus the master limiter
        needs, paddingSamples what aligning it with the slowest strip of its
        sync group adds (0 for a strip in no group, the default), and
        outputSamples the sum. engineSamples is the largest outputSamples. */
    struct Strip
    {
        int ownSamples = 0, paddingSamples = 0, outputSamples = 0;
        double ownMs = 0.0, paddingMs = 0.0, outputMs = 0.0;
    };

    /** False while no device runs (closed, stopped, failed, offline
        rendering) or the loopback guard holds the output at silence: the
        numbers below describe no audible path, so show "--". */
    bool valid = false;
    /** Always true today: device latencies are what the driver reports, and
        Bluetooth codec delay and the OS mixer are not included. Nothing is
        measured end to end yet (docs/11 E42d). Label totals "estimated". */
    bool estimated = true;

    double sampleRate = 0.0;
    int blockSize = 0;
    int deviceInputSamples = 0, deviceOutputSamples = 0, engineSamples = 0;
    double deviceInputMs = 0.0, deviceOutputMs = 0.0, engineMs = 0.0;
    double graphQuantumMs = 0.0;  // a known audio-graph quantum above the device (setGraphQuantumMs), 0 if unknown
    double captureBufferMs = 0.0; // extra FIFO latency of per-app capture streams (max over captures)
    double totalMs = 0.0;         // device input + device output + engine + graph quantum
    int numStrips = 0;
    std::array<Strip, flub::MixEngine::kMaxStrips> strips {};
};

/** What a device-error banner shows (docs/11 E51). Message thread. */
struct DeviceSafetyState
{
    enum class Kind
    {
        None,
        LoopbackPair, // the output is the loopback partner of the input: output held at silence
        DeviceError,  // the device reported an error, or no output device could be opened
    };

    Kind kind = Kind::None;
    juce::String message;                             // banner text, empty for None
    juce::String inputDeviceName, outputDeviceName;  // the pair (LoopbackPair) or the device (DeviceError)
    bool outputMuted = false;                         // the loopback guard holds the output at silence
    uint32_t generation = 0;                          // increments on every change
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
    int getNumStrips() const noexcept { return latest->engine.getNumStrips(); }

    /** Re-creates the engine for the current device format. While the device
        runs, the new engine replaces the old one through the crossfaded swap
        (see THREADING CONTRACT; no dropout); otherwise at once. */
    void reconfigure();

    /** Swap timing (see THREADING CONTRACT). A swap lasts the new engine's
        latency + kSwapSettleMs + kSwapFadeMs (equal latency) or the same
        plus another kSwapFadeMs of fade-in (latency change), during which
        both engines run. */
    static constexpr double kSwapFadeMs = 10.0, kSwapSettleMs = 10.0;

    /** True from reconfigure() publishing a new engine until the audio thread
        has finished crossfading to it (message thread). */
    bool isSwapInProgress() const noexcept;
    /** Engine swaps the audio thread has completed (any thread). */
    uint32_t getCompletedSwaps() const noexcept { return swapsCompleted.load (std::memory_order_acquire); }

    /** A structural parameter changed (latency profile): reconfigure() needed.
        Polled by this class at 5 Hz on the message thread, which then calls
        reconfigure() by itself. */
    bool needsReprepare() const noexcept { return latest->engine.needsReprepare(); }

    /** Neural model for strip `strip`'s neural slot (docs/09 §1.1; message
        thread). Installing, replacing or clearing (nullptr) a model is a
        structural change and goes through reconfigure(): the crossfaded swap
        while the device runs (the model's latency moves the output, so the
        swap takes the latency-change path), at once otherwise. `factory`
        makes a fresh runner for every engine built from now on (swaps,
        device restarts): the engine a swap fades out keeps running its own
        model, so one runner is never in two engines, and a model installed
        directly with ProcessingChain::setNeuralModel() would not carry over.
        Whether the model joins the chain (latency profile, sample rate,
        block size) is getMixEngine().chain (strip).getNeuralStatus(). */
    using NeuralModelFactory = std::function<std::unique_ptr<flub::ModelRunner>()>;
    void setNeuralModel (int strip, NeuralModelFactory factory, const flub::NeuralSlotConfig& config = {});
    void clearNeuralModel (int strip) { setNeuralModel (strip, nullptr); }
    bool hasNeuralModel (int strip) const noexcept;

    /** Increments after every MixEngine::configure (chains were re-created). */
    uint32_t getStructureGeneration() const noexcept { return structureGeneration.load (std::memory_order_acquire); }

    /** Direct access to the newest engine (the one a swap in progress fades
        to). params()/chain() are message-thread safe; do not call
        configure()/process()/setStrip*() on it - use this class. */
    flub::MixEngine& getMixEngine() noexcept { return latest->engine; }

    /** Called (asynchronously) on the message thread after every
        (re)configuration. */
    std::function<void()> onEngineConfigured;

    /** Called on the message thread when the device reports an error. */
    std::function<void (const juce::String&)> onDeviceError;
    juce::String getLastDeviceError() const;

    // =========================================================================
    // Device safety (message thread; see DEVICE SAFETY above)
    // =========================================================================
    /** True when capturing `inputDeviceName` while playing to
        `outputDeviceName` closes a loop: the output is the loopback partner
        of the input (exact partner pairs, not vendor tokens). Pure. */
    static bool isLoopbackPair (const juce::String& inputDeviceName, const juce::String& outputDeviceName);

    /** Re-checks a device pair now (called at every device start with the
        device manager's names; UI code may call it after changing devices
        itself). A pair trips the guard, anything else releases it. */
    void checkLoopbackPair (const juce::String& inputDeviceName, const juce::String& outputDeviceName);

    /** Per-pair override for deliberate setups (e.g. monitoring through a
        cable on purpose). Re-checks the current pair. Not persisted here. */
    void allowLoopbackPair (const juce::String& inputDeviceName, const juce::String& outputDeviceName);
    void clearAllowedLoopbackPairs();

    DeviceSafetyState getDeviceSafetyState() const { return safety; }
    /** Called on the message thread whenever getDeviceSafetyState() changes. */
    std::function<void()> onDeviceSafetyChanged;
    /** Dismisses a device error (e.g. after the user pressed Retry and the
        device reopened, or chose to ignore it). A tripped loopback guard
        stays until the pair changes or is allowed. */
    void clearDeviceError();
    /** True while the loopback guard holds the output at silence: the pair
        is a loopback pair (not allowed) AND the device input feeds a strip
        (a pair whose input nothing uses is not a loop). */
    bool isOutputMutedByGuard() const noexcept
    {
        return loopbackPair.load (std::memory_order_acquire) && deviceInputFeedsStrip();
    }

    // =========================================================================
    // Strip mix controls (any thread; applied click-free on the audio thread)
    // =========================================================================
    void setStripGainDb (int strip, float gainDb) noexcept;
    float getStripGainDb (int strip) const noexcept;
    void setStripMuted (int strip, bool muted) noexcept;
    bool isStripMuted (int strip) const noexcept;
    void setMasterCeilingDb (float db) noexcept;
    float getMasterCeilingDb() const noexcept { return masterCeilingDb.load (std::memory_order_relaxed); }

    // =========================================================================
    // Device correction (message thread; docs/11 E15)
    // =========================================================================
    /** The output endpoint's correction curve, run on the stereo sum before
        the master limiter (MixEngine::getDeviceCorrection). While the device
        runs it reaches the audio thread through the DeviceCorrection's SPSC
        hand-off and crossfades in over DeviceCorrection::kCrossfadeMs (an
        endpoint change swaps curves without a click); otherwise it applies
        at once. Every engine built later (swaps, device restarts) starts on
        it. Not a parameter: presets, A/B banks and automatic profiles never
        touch it (EngineController keys it to the output endpoint). */
    void setDeviceCorrection (const flub::DeviceCorrectionSettings& settings);
    const flub::DeviceCorrectionSettings& getDeviceCorrection() const noexcept { return deviceCorrection; }
    /** The automatic preamp of the current curve (dB, <= 0) and the
        predicted maximum boost it came from (docs/11 E11). */
    float getDeviceCorrectionPreampDb() const noexcept { return latest->engine.getDeviceCorrection().getPreampDb(); }
    flub::headroom::Prediction getDeviceCorrectionPrediction() const noexcept
    {
        return latest->engine.getDeviceCorrection().getPrediction();
    }

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
        double sampleRate = 0.0;   // the rate the capture delivers (the device rate, see restartCapturesAtDeviceRate)
        juce::String restartError; // non-empty while a restart at a new device rate is failing
        DriftCompensatedFifo::Stats stats;
    };
    std::vector<CaptureInfo> getCaptures() const;

    /** Restarts every capture whose rate differs from the device rate at the
        device rate (same id, same strip, FIFO re-primed). Runs by itself
        (asynchronously, then retried at 1 Hz for kMaxCaptureRestartAttempts)
        after a device rate change; public for tests. Message thread. */
    void restartCapturesAtDeviceRate();
    static constexpr int kMaxCaptureRestartAttempts = 3;

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
    /** "12.3 ms" style total (device + engine + quantum + app capture) with
        "est." when estimated, or "--" when !valid. */
    static juce::String formatTotalLatency (const LatencyInfo& info);
    /** A known audio-graph quantum that sits above the device and is not in
        the device's reported latency (e.g. the PipeWire quantum when JUCE
        talks ALSA to PipeWire's pcm plug-in); 0 = none / unknown. Added to
        LatencyInfo::totalMs. Message thread. */
    void setGraphQuantumMs (double ms) noexcept { graphQuantumMs = std::max (0.0, ms); }
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
        // Message thread only:
        int channels = 2;
        bool restartPending = false;       // stopped; to be restarted at the device rate
        int restartAttempts = 0;
        juce::String restartError;
        bool quarantined = false;          // released while a callback was stalled (see waitForAudioThreadToPass)
        uint64_t quarantineCounter = 0;    // callbackCounter when it was released
    };

    /** One complete engine: the MixEngine plus the audio thread's working set
        for it. Built and destroyed on the message thread only. */
    struct EngineInstance
    {
        flub::MixEngine engine;
        std::array<flub::AudioBuffer, kMaxStrips> stripBuffers;
        std::array<flub::AudioBlock, kMaxStrips> stripBlocks {};
        std::array<const flub::AudioBlock*, kMaxStrips> stripInputs {};
        std::array<float, kMaxStrips> appliedGainDb {};
        std::array<bool, kMaxStrips> appliedMuted {};
        float appliedCeilingDb = -1.0f;
        flub::AudioBuffer mixOutput;
        int numStrips = 0, maxBlock = 512, latencySamples = 0;
        int prerollSamples = 0;     // audible only after this many samples (latency + settle)
        std::vector<float> fadeIn;  // raised-cosine 0 -> 1 over kSwapFadeMs (its fade-out is 1 - fadeIn)
    };

    void handleAsyncUpdate() override;
    void timerCallback() override;
    void configureEngine (double sampleRate, int blockSize, bool fadeIn);
    std::unique_ptr<EngineInstance> buildEngine();
    void replaceEngineNow (std::unique_ptr<EngineInstance> next, bool fadeIn);
    void afterStructureChange();
    void collectRetired();
    void clearRemovedStrips() noexcept;
    void dispose (EngineInstance* instance);
    void processBlock (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int numSamples,
                       StripSignalSource* offlineSource) noexcept;
    void beginPendingSwap() noexcept;
    void mixSwap (const flub::AudioBlock& mix, int numSamples) noexcept;
    void applyPendingMixSettings (EngineInstance& instance) noexcept;
    /** Waits (at most kAudioThreadPassTimeoutMs) until a callback that may
        have read the old state has returned. False if it timed out while a
        callback was stalled; `startCounter` receives the counter it waited on. */
    bool waitForAudioThreadToPass (uint64_t& startCounter);
    static constexpr juce::uint32 kAudioThreadPassTimeoutMs = 250;
    void releaseSlot (CaptureSlot& slot);
    bool isSlotQuarantined (CaptureSlot& slot) noexcept;
    bool startCaptureInSlot (CaptureSlot& slot, flub::platform::ProcessLoopbackCapture& capture, uint32_t processId, std::string& error);
    void applyDeviceStartSafety (juce::AudioIODevice* device);
    void setSafetyState (DeviceSafetyState next);
    void applyGuardToOutput (float* const* outputs, int numOutputs, int numSamples, bool muted) noexcept;
    bool deviceInputFeedsStrip() const noexcept;

    juce::AudioDeviceManager deviceManager;
    std::vector<flub::StripConfig> layout;

    // ---- Engine instances ---------------------------------------------------------
    // `instances` owns every live instance (message thread). `latest` is the
    // newest (message thread; getMixEngine). The audio thread (or the message
    // thread while no callback can run) uses `active`, plus `fading` - the old
    // engine - during a swap; pendingSwap / retiredSwap hand instances between
    // the two threads.
    std::vector<std::unique_ptr<EngineInstance>> instances;
    EngineInstance* latest = nullptr;
    EngineInstance* active = nullptr;
    EngineInstance* fading = nullptr;
    std::atomic<EngineInstance*> pendingSwap { nullptr }, retiredSwap { nullptr };
    std::atomic<uint32_t> swapsCompleted { 0 };

    // ---- Audio-thread working set (host level; per-engine state is in EngineInstance)
    std::array<int, kMaxStrips> hangoverRemaining {};
    int hangoverSamples = 24000;
    // A swap (or the fade-in after a device start, which has no old engine)
    // in samples since it began: the old engine's gain falls from
    // swapFadeOutStart, the new one's rises from swapFadeInStart, the old one
    // is retired at swapOldEnd and the transition ends at swapEnd.
    bool swapRunning = false, swapCounts = false;
    int swapPos = 0, swapFadeInStart = 0, swapFadeOutStart = 0, swapOldEnd = 0, swapEnd = 0;
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
    std::atomic<bool> loopbackPair { false };  // loopback guard: the current pair loops (message -> audio thread)
    std::atomic<bool> safetyNotifyPending { false };
    float guardGain = 1.0f;                    // audio thread (or message thread before the callback starts)
    std::atomic<uint64_t> callbackCounter { 0 };
    std::atomic<uint32_t> structureGeneration { 0 };
    std::array<CaptureSlot, kMaxCaptures> captureSlots;

    // ---- Message-thread state ----------------------------------------------------------
    CaptureFactory captureFactory;
    struct NeuralModelSetup
    {
        NeuralModelFactory factory;
        flub::NeuralSlotConfig config;
    };
    std::array<NeuralModelSetup, kMaxStrips> neuralModels;
    flub::DeviceCorrectionSettings deviceCorrection; // every new engine starts on it
    double currentSampleRate = 48000.0;
    int currentBlockSize = 512;
    int deviceInputLatency = 0, deviceOutputLatency = 0;
    double pendingSampleRate = 0.0;
    int pendingBlockSize = 0, pendingInputLatency = 0, pendingOutputLatency = 0;
    bool callbackAttached = false;
    uint32_t swapsRequested = 0; // swaps published that the audio thread will complete
    juce::CriticalSection errorLock;
    juce::String lastDeviceError;
    DeviceSafetyState safety;
    std::vector<std::pair<juce::String, juce::String>> allowedLoopbackPairs; // (input, output)
    juce::String guardInputName, guardOutputName; // the pair last checked
    bool captureRestartNeeded = false;
    bool lastWaitTimedOut = false;      // waitForAudioThreadToPass gave up ...
    uint64_t lastTimedOutCounter = 0;   // ... while the counter stood here
    juce::uint32 lastCaptureRestartMs = 0;
    double graphQuantumMs = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngineHost)
};
} // namespace flub::app
