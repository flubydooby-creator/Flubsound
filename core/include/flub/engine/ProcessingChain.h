// Flubsound Pro - the per-strip processing chain (the heart of the engine).
//
//   in (2 / 6 / 8 ch)
//    -> input gain -> AutoLevel (LUFS)                 [all input channels]
//    -> HeadphoneVirtualizer (5.1/7.1 -> binaural) or ITU-R BS.775 downmix
//       (virtualiser off) - from here on the chain is STEREO
//    -> [slot] SpectralNoiseGate      (Quality latency profile only)
//    -> [slot] ParametricEq (10 bands)
//    -> [slot] DynamicEq (4 bands)
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
//   Balanced   : no gate,   sat 2x LQ, comp LA 1 ms, max 4x HQ + 1.5 ms  (~3.8 ms)
//   LowLatency : no gate,   sat 2x LQ, comp LA 0.5 ms, max 2x LQ + 0.5 ms (~1.9 ms)
#pragma once

#include "MacroMap.h"
#include "MeterBus.h"
#include "ModuleSlot.h"
#include "Parameters.h"
#include "Protection.h"
#include "flub/analysis/LoudnessMeter.h"
#include "flub/analysis/PeakMeters.h"
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
    void process (const AudioBlock& io) noexcept;

    int getLatencySamples() const noexcept { return totalLatency; }
    double getSampleRate() const noexcept { return config.sampleRate; }
    const ChainConfig& getConfig() const noexcept { return config; }

    /** True when a structural parameter changed since prepare(); the host
        must call prepare() again from a non-RT thread (with a short fade). */
    bool needsReprepare() const noexcept;

    MeterBus& meters() noexcept { return meterBus; }
    AnalyzerTaps& taps() noexcept { return analyzerTaps; }

    /** Last effective (post-macro) parameter values, for GUI "ghost" markers.
        Written by the audio thread; benign races on individual floats. */
    const float* effectiveValues() const noexcept { return effective.data(); }

private:
    void applyParameters() noexcept;
    void publishMeters (const AudioBlock& out, int numSamples) noexcept;
    void downmixToStereo (const AudioBlock& io) noexcept;

    param::ParameterStore& store;
    ChainConfig config;
    int profileAtPrepare = -1;
    int totalLatency = 0;

    std::vector<float> base, effective; // kNumParams each (allocated in ctor)

    // Modules (owned) and their bypass slots, in processing order.
    SpectralNoiseGate gate;
    ParametricEq eq;
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

    // Global bypass dry path (post input stage, stereo, delayed by totalLatency)
    AudioBuffer dryBuffer;
    DelayLine dryDelay;

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
