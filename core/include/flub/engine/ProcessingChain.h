// Flubsound Pro - the per-strip processing chain (the heart of the engine).
//
//   in (2 / 6 / 8 ch)
//    -> input gain -> AutoLevel (LUFS)                 [all input channels]
//    -> ActiveChannelDetector (5.1/7.1: surround or stereo-only content?)
//    -> fold to stereo, one of (crossfaded, docs/11 E01 / E27):
//         HeadphoneVirtualizer (5.1/7.1 -> binaural, virt.on),
//         ITU-R BS.775 downmix at -3 dB (virt.on off), or
//         stereo passthrough (unity BS.775: FL/FR-only content, a forced
//         stereo fold or a game that renders its own HRTF);
//       the LFE is folded by the same LfeFold in all three
//       - from here on the chain is STEREO
//    -> (dry reference for the global bypass, input meters, "pre" tap)
//    -> automatic preamp (auto.preamp, docs/11 E11; unity while off)
//    -> [slot] SpectralNoiseGate      (Quality latency profile only)
//    -> [slot] Neural (AsyncModelProcessor; only while a model is installed
//              and eligible for the latency profile, see setNeuralModel)
//    -> [slot] ParametricEq (10 bands)
//    -> [slot] DynamicEq (4 user bands + 4 internal mode bands)
//    -> [slot] BassEngine
//    -> [slot] ClarityEnhancer
//    -> [slot] Saturator (oversampled)
//    -> [slot] StereoSpatializer      (forced width 1 / space 0 / crossfeed 0 /
//                                      focus 0 when the virtualiser produced
//                                      binaural output or the game renders
//                                      its own HRTF, virt.ownHrtf)
//    -> [slot] Compressor (look-ahead, up/down)
//    -> [slot] LoudnessMaximizer (glue + clipper + true-peak limiter)
//    -> output gain -> global bypass crossfade (dry delayed by total latency,
//       optionally loudness matched) -> meters / analyser taps -> out (2 ch)
//
// Per block (RT): input sanitiser -> snapshot store -> MacroMap (Boost
// Intensity + mode macros, governed; at protection strength Normal / Strict
// also the base drives, see Protection.h) -> mode policy -> push params into
// modules -> process -> telemetry (limiter GR, measured THD+N of saturator +
// clipper) -> SafetyGovernor / AutoDrive updates for the next block.
//
// Automatic preamp (docs/11 E11). The chain's static maximum boost is
// predicted from the effective values (macros included, the governor's
// scale not: the preamp follows what was asked for, not the governor's
// reaction to it; nor the GUI's momentary audition bypass) on the exact digital responses of the level-independent
// stages that can raise the level: the parametric EQ (bands and output
// gain), the dynamic EQ's static gains, the bass shelf (at its full boost,
// with the subsonic high-pass), presence (at its full lift) and the air
// shelf, the saturator's wet make-up; the surround fold's -3 dB trim counts
// against them. Left out: level-dependent boosts that withdraw on loud
// material (the dynamic-EQ ranges and mode bands, the transient shaper),
// harmonics (bass, air, saturation), the compressor's make-up (it follows
// its own gain reduction) and the maximizer's drive (loudness on purpose).
// The prediction uses headroom::predictMaxBoostWith (the 1/12-octave grid
// and golden-section refinement of DeviceCorrection.h) with the programme
// weighting, on the audio thread without allocation, whenever an input
// changes, at most once per kHeadroomUpdateMs (about 45 us for 25 sections).
// preamp = -max(0, prediction - auto.preampAllowance), glided over 20 ms.
// It is applied after the dry reference and the input meters: AutoLevel's
// detector does not see it (AutoLevel would otherwise cancel it), bypass
// compares against the unprocessed signal, and every module - EQ first -
// sees the lowered level, so a boosted chain reaches the maximizer with the
// headroom its boosts use instead of driving the limiter.
//
// Input sanitiser (docs/11 E10 Phase 1), before anything else sees the block:
//   * a NaN / Inf anywhere drops the whole block (silence out) and resets the
//     signal path, not the control loops (they never see it);
//   * a finite sample beyond +24 dBFS (kSanitiseLimit; no real source gets
//     there, a driver or decoder fault does, e.g. 1e30) is muted - set to 0,
//     not clamped: a clamped +24 dBFS sample would still hit every detector in
//     the signal path - and the block is hidden from the control loops
//     (AutoLevel, AutoDrive, the governor's detectors, the loudness match),
//     which hold for it, so one corrupt sample cannot move them for seconds.
//   Both are counted on MeterBus (corruptSampleCount, droppedBlockCount).
//
// Latency is the sum of the slot latencies for the current latency profile
// and is constant until the next prepare(). Latency profiles (48 kHz):
//   Quality    : gate 1024, sat 2x HQ, comp LA 3 ms, max 4x HQ + 2 ms TP limiter (1352 smp ~ 28.2 ms)
//   Balanced   : no gate,   sat 2x LQ, comp LA 1 ms, max 4x HQ + 1.5 ms  (192 smp = 4.0 ms)
//   LowLatency : no gate,   sat 2x LQ, comp LA 0.5 ms, max 2x LQ + 0.5 ms (100 smp ~ 2.1 ms)
// plus, while a neural model is active, its fixed latency L (docs/09 §1.1).
// Look-aheads and the gate frame are defined in ms (the frame is 21.3 ms,
// rounded to a power of two: 1024 at 44.1 / 48 kHz, 2048 at 96, 4096 at
// 192 kHz); the oversampler FIRs and the true-peak detector are in samples.
// Below 32 kHz (hands-free links) a requested Quality runs as Balanced
// (docs/11 E42a): getLatencyProfile() is the profile in effect,
// getRequestedLatencyProfile() the stored one.
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
#include "flub/dsp/ActiveChannelDetector.h"
#include "flub/dsp/BassEngine.h"
#include "flub/dsp/Bs775Fold.h"
#include "flub/dsp/ClarityEnhancer.h"
#include "flub/dsp/Compressor.h"
#include "flub/dsp/DeviceCorrection.h"
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
    /** The latency profile in effect since the last prepare() (constant until
        the next one): the stored latency.profile, except that Quality runs as
        Balanced below 32 kHz (docs/11 E42a). */
    param::LatencyProfileValue getLatencyProfile() const noexcept { return static_cast<param::LatencyProfileValue> (profileAtPrepare); }
    /** The latency.profile the store held at the last prepare(); differs from
        getLatencyProfile() only while Quality is clamped (for a UI note). */
    param::LatencyProfileValue getRequestedLatencyProfile() const noexcept { return static_cast<param::LatencyProfileValue> (requestedProfileAtPrepare); }
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

    /** 5.1 / 7.1 strips (docs/11 E27): forget the input-channel detection,
        including a confirmed-surround latch, and start over ("Surround,
        unconfirmed"), as prepare() does. For the host when the routed
        process or the source changes on the same device. Any thread (one
        atomic, taken by the next process()). */
    void redetectInputChannels() noexcept FLUB_NONBLOCKING { redetectRequest.store (true, std::memory_order_relaxed); }

    /** How far the SafetyGovernor reaches (Protection.h): Off (default)
        governs the macro amounts only, Normal also the base max.drive,
        sat.drive and bass.harmonics, Strict as Normal with the scale's floor
        at 0. A host / user safety setting, not a preset value: any thread
        (one atomic), taken by the next process(). */
    void setProtectionStrength (ProtectionStrength s) noexcept FLUB_NONBLOCKING { protectionStrength.store (static_cast<int> (s), std::memory_order_relaxed); }
    ProtectionStrength getProtectionStrength() const noexcept { return static_cast<ProtectionStrength> (protectionStrength.load (std::memory_order_relaxed)); }

    /** Input samples with a magnitude above this (+24 dBFS) are corrupt and
        muted by the sanitiser (see the header comment). */
    static constexpr float kSanitiseLimit = 15.85f;

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

    // ---- Automatic preamp (docs/11 E11; see the header comment) ----------
    /** The chain's static response as far as it can raise the level: the
        exact digital responses of its level-independent boosting stages and
        a broadband gain. Trivially copyable, no allocation. */
    struct StaticBoostModel
    {
        static constexpr int kMaxSections = 64; // EQ 10 x 4, dynamic EQ 4, bass 3, clarity 2
        std::array<SvfCoeffs, kMaxSections> sections {};
        int numSections = 0;
        double gainDb = 0.0; // EQ output, saturation make-up, the surround fold's trim
        double sampleRate = 48000.0;

        /** Magnitude in dB at freqHz. RT-safe. */
        double responseDb (double freqHz) const noexcept FLUB_NONBLOCKING;
    };

    /** Builds the model from effective values (param::kNumParams entries; a
        module counts when its enable value is on). surroundFold: a 5.1 / 7.1
        strip folds through the virtualiser or the BS.775 downmix (-3 dB).
        RT-safe. */
    static void buildStaticBoostModel (const float* effective, double sampleRate, bool surroundFold,
                                       StaticBoostModel& model) noexcept FLUB_NONBLOCKING;
    /** Maximum of the model's response over 20 Hz .. min (20 kHz, 0.49 fs). RT-safe. */
    static headroom::Prediction predictStaticBoost (const StaticBoostModel& model,
                                                    headroom::Weighting weighting) noexcept FLUB_NONBLOCKING;

    /** The latest programme-weighted prediction behind the automatic preamp
        (dB, and where), and the preamp it asks for (dB <= 0; 0 while
        auto.preamp is off). Published by the audio thread; any thread. */
    float getPredictedBoostDb() const noexcept { return predictedBoostDb.load (std::memory_order_relaxed); }
    float getPredictedBoostHz() const noexcept { return predictedBoostHz.load (std::memory_order_relaxed); }
    float getAutoPreampDb() const noexcept { return autoPreampDb.load (std::memory_order_relaxed); }

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
    /** process() after the input sanitiser, for one segment of the block
        that ends at or before the governor's next tick. */
    void processSegment (const AudioBlock& io, bool contaminated) noexcept FLUB_NONBLOCKING;
    /** Folds one segment's module readings into blockReadings. */
    void accumulateReadings() noexcept;
    /** reset() without the control loops (governor, AutoLevel, AutoDrive,
        ComparisonMatcher): the signal path, its meters and the distortion monitor. */
    void resetSignalState() noexcept;
    void foldToStereo (const AudioBlock& in) noexcept;
    /** Re-predicts the static boost when an input changed (at most once per
        kHeadroomUpdateMs; the first time after prepare() at once, with the
        preamp starting at its value) and sets the preamp's target. */
    void updateHeadroom (const float* e, bool surroundFold) noexcept FLUB_NONBLOCKING;
    void prepareNeuralSlot (const ProcessSpec& stereo);
    bool inChain (int slot) const noexcept { return (slot != SGate || gateInChain) && (slot != SNeural || neuralInChain); }

    param::ParameterStore& store;
    ChainConfig config;
    int profileAtPrepare = -1, requestedProfileAtPrepare = -1;
    int totalLatency = 0;

    std::vector<float> base, effective; // kNumParams each (allocated in ctor); audio thread only
    std::vector<float> governedBase;    // base with the governed base drives (protection Normal / Strict)
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

    // Automatic preamp (docs/11 E11): the prediction's inputs as of the last
    // prediction (headroomKey), a copy of the effective values it is made
    // from (with the ungoverned bass boost), the model.
    static constexpr int kHeadroomKeySize = 97;
    static constexpr float kHeadroomUpdateMs = 10.0f;
    std::array<float, kHeadroomKeySize> headroomKey {};
    bool headroomKeyValid = false;
    int headroomHoldoff = 0; // samples until the next prediction may run
    std::vector<float> headroomInput, ungoverned; // kNumParams each (allocated in ctor)
    StaticBoostModel headroomModel;
    LinearSmoothedValue preampGain;
    std::atomic<float> predictedBoostDb { 0.0f }, predictedBoostHz { 1000.0f }, autoPreampDb { 0.0f };
    AutoLevel autoLevel;
    AutoDrive autoDrive;
    SafetyGovernor governor;
    DistortionMonitor distortion;
    ComparisonMatcher loudnessMatch;
    std::atomic<int> protectionStrength { static_cast<int> (ProtectionStrength::Off) };
    ProtectionStrength appliedStrength = ProtectionStrength::Off; // as of the current segment (audio thread)
    uint64_t corruptSamples = 0, droppedBlocks = 0; // input sanitiser, since prepare()
    // The host block's module-meter extremes over its segments (process()).
    struct BlockReadings
    {
        float compGrDb = 0.0f, compUpDb = 0.0f, maxGrDb = 0.0f, glueGrDb = 0.0f;
        double clipRemoved = 0.0, clipInput = 0.0; // the clipper's energies (LoudnessMaximizer)
    };
    BlockReadings blockReadings;

    // Surround fold (5.1 / 7.1 input): the virtualiser's binaural render B,
    // the unity BS.775 matrix D (Bs775Fold, LFE included) and the detector.
    //   surround S = k D + virtMix (B - k D)   (virtMix: 20 ms, virt.on)
    //   output     = g (S + passMix (D - S))    (passMix: 400 ms, stereo fold)
    // g = 1 except during the passMix ramp p, where it holds the mix's RMS
    // on (1 - p) RMS_S + p RMS_D (block statistics pass*, see foldToStereo).
    // Paths whose weight is zero for a whole block are not run; one that
    // starts again is reset first (virtRan / foldRan).
    Bs775Fold fold;
    ActiveChannelDetector inputDetector;
    LinearSmoothedValue virtMix, passMix;
    AudioBuffer foldScratch;
    double passSs = 0.0, passDd = 0.0, passSd = 0.0; // mean S^2, D^2, S D (both channels)
    bool virtRan = false, foldRan = false, passStatsValid = false;
    std::atomic<bool> redetectRequest { false };

    // Global bypass dry path (post input stage, stereo): delayed by
    // totalLatency - dryLimiter latency, then (while bypass is engaged)
    // loudness-matched and true-peak limited at the ceiling by dryLimiter, so
    // it lines up with the processed path and never overshoots.
    AudioBuffer dryBuffer;
    DelayLine dryDelay;
    TruePeakLimiter dryLimiter;
    bool dryLimiterRunning = false;

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
