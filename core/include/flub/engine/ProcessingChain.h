// Flubsound Pro - the per-strip processing chain (the heart of the engine).
//
//   in (2 / 6 / 8 ch)
//    -> input gain -> AutoLevel (LUFS)                 [all input channels]
//    -> HeadphoneVirtualizer (5.1/7.1 -> binaural) or ITU-R BS.775 downmix
//       (virtualiser off) - from here on the chain is STEREO
//    -> [slot] SpectralNoiseGate      (Quality latency profile only)
//    -> [slot] Neural (AsyncModelProcessor; only while a model is installed
//              and eligible for the latency profile, see setNeuralModel)
//    -> [slot] ParametricEq (10 bands)
//    -> [slot] DynamicEq (4 user bands + 4 internal mode bands)
//    -> [slot] BassEngine
//    -> [slot] ClarityEnhancer
//    -> [slot] Saturator (oversampled)
//    -> [slot] StereoSpatializer      (forced width 1 / crossfeed 0 when the
//                                      virtualiser produced binaural output)
//    -> [slot] Compressor (look-ahead, up/down)
//    -> [slot] LoudnessMaximizer (glue + clipper + true-peak limiter)
//    -> output gain -> global bypass crossfade (dry delayed by total latency,
//       optionally loudness matched) -> meters / analyser taps -> out (2 ch)
//
// Per block (RT): snapshot store -> MacroMap (Boost Intensity + mode macros,
// governed) -> mode policy -> push params into modules -> process ->
// telemetry (limiter GR, measured THD+N of saturator + clipper) ->
// SafetyGovernor / AutoDrive updates for the next block.
//
// Latency is the sum of the slot latencies for the current latency profile
// and is constant until the next prepare(). Latency profiles (48 kHz):
//   Quality    : gate 1024, sat 2x HQ, comp LA 3 ms, max 4x HQ + 2 ms TP limiter
//   Balanced   : no gate,   sat 2x LQ, comp LA 1 ms, max 4x HQ + 1.5 ms  (192 smp = 4.0 ms)
//   LowLatency : no gate,   sat 2x LQ, comp LA 0.5 ms, max 2x LQ + 0.5 ms (100 smp ~ 2.1 ms)
// plus, while a neural model is active, its fixed latency L (docs/09 §1.1).
//
// Neural slot (docs/09-future-roadmap.md §1.1). Empty by default: the slot is
// then skipped like the gate outside Quality, so latency and output are
// exactly those of a chain without it. A model installed with
// setNeuralModel() takes effect at the next prepare() (needsReprepare()
// reports the pending change), where it joins the chain only if
// isEligible (profile, L, sampleRate) holds, its sample rate matches and (in
// real time) maxBlockSize fits its safety frames; otherwise it stays out (no
// worker, no latency) and getNeuralStatus() says why. It sits after the gate and before the EQ: upstream of every dynamics
// stage, so the compressor and the maximizer's true-peak limiter act on what
// the model's controls did and the ceiling guarantee holds whatever gain it
// applies (up to AsyncModelConfig::maxGain); downstream of the gate, whose
// noise-floor statistics would otherwise pump with the model's frame-rate
// gain; and ahead of the tonal / saturation / width stages, so a model sees
// the source signal it was trained on rather than the user's colouring.
#pragma once

#include "MacroMap.h"
#include "MeterBus.h"
#include "ModuleSlot.h"
#include "Parameters.h"
#include "Protection.h"
#include "flub/analysis/LoudnessMeter.h"
#include "flub/analysis/PeakMeters.h"
#include "flub/common/Realtime.h"
#include "flub/common/SmoothedValue.h"
#include "flub/dsp/BassEngine.h"
#include "flub/dsp/ClarityEnhancer.h"
#include "flub/dsp/Compressor.h"
#include "flub/dsp/DynamicEq.h"
#include "flub/dsp/HeadphoneVirtualizer.h"
#include "flub/dsp/LoudnessMaximizer.h"
#include "flub/dsp/ParametricEq.h"
#include "flub/dsp/Saturator.h"
#include "flub/dsp/SpectralNoiseGate.h"
#include "flub/dsp/StereoSpatializer.h"
#include "flub/neural/AsyncModelProcessor.h"
#include "flub/neural/Eligibility.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace flub
{
struct ChainConfig
{
    double sampleRate = 48000.0;
    int maxBlockSize = 512;
    int inputChannels = 2; // 2, 6 (5.1) or 8 (7.1)
};

/** Settings for the neural slot (ProcessingChain::setNeuralModel). */
struct NeuralSlotConfig
{
    AsyncModelConfig processor;                     // safety frames, fallback, ramp, gain bound (offline is set from context)
    /** Offline (batch render, never an audio callback): any model is
        eligible, any block length is accepted, and the processor runs in
        offline mode, so a render faster than real time gets every frame's
        result (AsyncModelProcessor.h). */
    ModelContext context = ModelContext::Realtime;
};

/** Why the neural slot is (not) in the chain, as of the last prepare(). */
enum class NeuralSlotState : int
{
    Empty = 0,              // no model installed
    Active = 1,             // in the chain: latency includes the model's L
    Ineligible = 2,         // L exceeds the prepared latency profile's budget
    SampleRateMismatch = 3, // the model was trained for another sample rate
    InvalidModel = 4,       // the runner's description is invalid (inert processor)
    PrepareFailed = 5,      // the runner's prepare() or the worker thread threw
    BlockTooLarge = 6       // Realtime: maxBlockSize > safetyFrames * frameSize, so frames would miss whatever the model's speed
};

struct NeuralSlotStatus
{
    NeuralSlotState state = NeuralSlotState::Empty;
    int modelLatencySamples = 0; // the model's L (whether or not it is in the chain)
    bool changePending = false;  // setNeuralModel / clearNeuralModel since the last prepare()
};

/** One-line explanation of a state, for the UI (static storage). */
const char* neuralSlotReason (NeuralSlotState state) noexcept;

/** The neural slot's telemetry (AsyncModelProcessor counters, restarting at
    prepare()); zero while the slot is not in the chain. */
struct NeuralSlotCounters
{
    uint64_t deadlineMisses = 0, modelFailures = 0, framesProcessed = 0;
};

class ProcessingChain
{
public:
    explicit ProcessingChain (param::ParameterStore& store);

    /** Non-RT. Reads structural parameters (latency profile) from the store,
        swaps in a pending neural model and decides whether it is in the chain. */
    void prepare (const ChainConfig& config);
    void reset() noexcept;

    /** RT. io.numChannels == config.inputChannels, numSamples <= maxBlockSize.
        Output is written to channels 0/1; channels >= 2 are cleared. */
    void process (const AudioBlock& io) noexcept FLUB_NONBLOCKING;

    int getLatencySamples() const noexcept { return totalLatency; }
    /** The latency profile the chain was prepared with (constant until the next prepare()). */
    param::LatencyProfileValue getLatencyProfile() const noexcept { return static_cast<param::LatencyProfileValue> (profileAtPrepare); }
    double getSampleRate() const noexcept { return config.sampleRate; }
    const ChainConfig& getConfig() const noexcept { return config; }

    /** True when a structural parameter changed since prepare(), or a neural
        model was installed or cleared; the host must call prepare() again
        from a non-RT thread (with a short fade). */
    bool needsReprepare() const noexcept;

    /** Momentary "listen without this module" (the GUI's hold-to-bypass A/B):
        forces the module whose enable parameter is `enableParamId` (GateOn,
        EqOn, DynEqOn, BassOn, ClarityOn, SaturationOn, SpatialOn,
        CompressorOn, MaximizerOn, VirtualizerOn) off whatever the store or
        the macros say, through the module's normal click-free bypass fade.
        Callable from any thread (one atomic); not stored, not in presets.
        Other ids are ignored. */
    void setAuditionBypass (int enableParamId, bool bypassed) noexcept;
    bool isAuditionBypassed (int enableParamId) const noexcept;

    /** The dynamic EQ's internal mode bands (footsteps / anti-masking / voice
        in Gaming, de-harsh / air / de-boom in Music) occupy bands 4..7. */
    static constexpr int kFirstModeBand = 4, kNumModeBands = 4;
    /** Centre / corner frequency of mode band `band` (4..7) in `mode`, for
        GUI markers; 0 for any other band. */
    static float modeBandFrequency (param::ModeValue mode, int band) noexcept;

    // ---- Neural slot (docs/09 §1.1) --------------------------------------
    /** Non-RT, on the thread that prepares the chain (never concurrently with
        prepare()). Installs a model; it replaces any previous one at the next
        prepare(), which also decides eligibility. A null runner is the same as
        clearNeuralModel(). */
    void setNeuralModel (std::unique_ptr<ModelRunner> runner, const NeuralSlotConfig& neuralConfig = {});
    /** Non-RT, as setNeuralModel(). Removes the model at the next prepare(). */
    void clearNeuralModel();

    /** State as of the last prepare(), plus whether a change is pending. Any thread. */
    NeuralSlotStatus getNeuralStatus() const noexcept FLUB_NONBLOCKING;
    /** Published by the audio thread once per block (relaxed atomics). Any thread. */
    NeuralSlotCounters getNeuralCounters() const noexcept FLUB_NONBLOCKING;

    /** Takes the active model out of the signal path (true) or back in, through
        the slot's click-free, latency-compensated bypass fade: the chain
        latency does not change. Any thread (one atomic); ignored while the
        slot is not in the chain. */
    void setNeuralBypass (bool bypassed) noexcept FLUB_NONBLOCKING { neuralBypass.store (bypassed, std::memory_order_relaxed); }
    bool isNeuralBypassed() const noexcept { return neuralBypass.load (std::memory_order_relaxed); }

    /** The active model's processor, nullptr while the slot is not in the
        chain. Valid until the next prepare(); for tests and diagnostics on
        the thread that prepares the chain (e.g. getPendingFrames()). */
    const AsyncModelProcessor* getNeuralProcessor() const noexcept { return neuralInChain ? neural.get() : nullptr; }

    MeterBus& meters() noexcept { return meterBus; }
    AnalyzerTaps& taps() noexcept { return analyzerTaps; }

    /** Last effective value of a parameter - after the macros and the chain's
        mode / format overrides (Gaming crossfeed, binaural lock, air below
        42 kHz, the Gaming compressor ratio), i.e. what is applied - for GUI
        "ghost" markers. The audio thread publishes all of them once per block
        (relaxed atomics), so this may be called from any thread. */
    float effectiveValue (int paramId) const noexcept
    {
        return paramId >= 0 && paramId < param::kNumParams ? publishedEffective[static_cast<size_t> (paramId)].load (std::memory_order_relaxed) : 0.0f;
    }

private:
    void applyParameters() noexcept;
    void publishEffective() noexcept;
    void publishMeters (const AudioBlock& out, int numSamples) noexcept;
    /** reset() without the control loops (governor, AutoLevel, AutoDrive,
        LoudnessMatch): the signal path, its meters and the distortion monitor. */
    void resetSignalState() noexcept;
    void downmixToStereo (const AudioBlock& io) noexcept;
    void prepareNeuralSlot (const ProcessSpec& stereo);
    bool inChain (int slot) const noexcept { return (slot != SGate || gateInChain) && (slot != SNeural || neuralInChain); }

    param::ParameterStore& store;
    ChainConfig config;
    int profileAtPrepare = -1;
    int totalLatency = 0;

    std::vector<float> base, effective; // kNumParams each (allocated in ctor); audio thread only
    std::vector<float> baseAtPrepare;   // structural values the chain was prepared with
    std::atomic<uint32_t> auditionMask { 0 }; // bit per module, see auditionBit()
    std::unique_ptr<std::atomic<float>[]> publishedEffective; // copy of effective for other threads

    // Modules (owned) and their bypass slots, in processing order.
    SpectralNoiseGate gate;
    ParametricEq paramEq;
    DynamicEq dynEq;
    BassEngine bass;
    ClarityEnhancer clarity;
    Saturator saturator;
    StereoSpatializer spatial;
    Compressor compressor;
    LoudnessMaximizer maximizer;
    HeadphoneVirtualizer virtualizer;

    // The neural processor is owned through a pointer (it is built around the
    // runner a host installs); its slot is part of the fixed array like every
    // other module, so it shares the bypass / latency / reset handling.
    std::unique_ptr<AsyncModelProcessor> neural;        // installed model (in the chain or not)
    std::unique_ptr<AsyncModelProcessor> pendingNeural; // setNeuralModel() until the next prepare()
    ModelContext neuralContext = ModelContext::Realtime, pendingContext = ModelContext::Realtime;

    enum SlotIndex { SGate, SNeural, SEq, SDynEq, SBass, SClarity, SSat, SSpatial, SComp, SMax, kNumSlots };
    std::array<ModuleSlot, kNumSlots> slots;
    bool gateInChain = false, neuralInChain = false;
    std::atomic<bool> neuralChangePending { false }, neuralBypass { false };
    std::atomic<int> neuralState { static_cast<int> (NeuralSlotState::Empty) }, neuralModelLatency { 0 };
    std::atomic<uint64_t> neuralMisses { 0 }, neuralFailures { 0 }, neuralFrames { 0 };

    // Gain staging / protection
    LinearSmoothedValue inputGain, outputGain, bypassMix, dryMatchGain;
    AutoLevel autoLevel;
    AutoDrive autoDrive;
    SafetyGovernor governor;
    DistortionMonitor distortion;
    LoudnessMatch loudnessMatch;

    // Surround fold: 20 ms crossfade between the binaural render and the
    // BS.775 downmix whenever virt.on changes (both paths run during it).
    LinearSmoothedValue virtMix;
    AudioBuffer foldScratch;

    // Global bypass dry path (post input stage, stereo): delayed by
    // totalLatency - dryLimiter latency, then (while bypass is engaged)
    // loudness-matched and true-peak limited at the ceiling by dryLimiter, so
    // it lines up with the processed path and never overshoots.
    AudioBuffer dryBuffer;
    DelayLine dryDelay;
    TruePeakLimiter dryLimiter;
    bool dryLimiterRunning = false;
    float dryPeakHold = 0.0f, dryPeakRelease = 0.0f; // keeps the matched reference below the ceiling

    // Metering
    LevelMeter inLevel, outLevel;
    TruePeakMeter outTruePeak;
    LoudnessMeter outLoudness;
    LoudnessFollower inLoudness;
    MeterBus meterBus;
    AnalyzerTaps analyzerTaps;
    std::vector<float> tapScratch;
};
} // namespace flub
