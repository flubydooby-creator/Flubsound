// Flubsound Pro - the per-strip processing chain (the heart of the engine).
//
//   in (2 / 6 / 8 ch)
//    -> input gain -> AutoLevel (LUFS)                 [all input channels]
//    -> HeadphoneVirtualizer (5.1/7.1 -> binaural) or ITU-R BS.775 downmix
//       (virtualiser off) - from here on the chain is STEREO
//    -> [slot] SpectralNoiseGate      (Quality latency profile only)
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
// telemetry -> SafetyGovernor / AutoDrive updates for the next block.
//
// Latency is the sum of the slot latencies for the current latency profile
// and is constant until the next prepare(). Latency profiles (48 kHz):
//   Quality    : gate 1024, sat 2x HQ, comp LA 3 ms, max 4x HQ + 2 ms TP limiter
//   Balanced   : no gate,   sat 2x LQ, comp LA 1 ms, max 4x HQ + 1.5 ms  (192 smp = 4.0 ms)
//   LowLatency : no gate,   sat 2x LQ, comp LA 0.5 ms, max 2x LQ + 0.5 ms (100 smp ~ 2.1 ms)
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

#include <array>
#include <atomic>
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

class ProcessingChain
{
public:
    explicit ProcessingChain (param::ParameterStore& store);

    /** Non-RT. Reads structural parameters (latency profile) from the store. */
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

    /** True when a structural parameter changed since prepare(); the host
        must call prepare() again from a non-RT thread (with a short fade). */
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

    MeterBus& meters() noexcept { return meterBus; }
    AnalyzerTaps& taps() noexcept { return analyzerTaps; }

    /** Last effective (post-macro) value of a parameter, for GUI "ghost"
        markers. The audio thread publishes all of them once per block
        (relaxed atomics), so this may be called from any thread. */
    float effectiveValue (int paramId) const noexcept
    {
        return paramId >= 0 && paramId < param::kNumParams ? publishedEffective[static_cast<size_t> (paramId)].load (std::memory_order_relaxed) : 0.0f;
    }

private:
    void applyParameters() noexcept;
    void publishEffective() noexcept;
    void publishMeters (const AudioBlock& out, int numSamples) noexcept;
    void downmixToStereo (const AudioBlock& io) noexcept;

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

    enum SlotIndex { SGate, SEq, SDynEq, SBass, SClarity, SSat, SSpatial, SComp, SMax, kNumSlots };
    std::array<ModuleSlot, kNumSlots> slots;
    bool gateInChain = false;

    // Gain staging / protection
    LinearSmoothedValue inputGain, outputGain, bypassMix, dryMatchGain;
    AutoLevel autoLevel;
    AutoDrive autoDrive;
    SafetyGovernor governor;
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
