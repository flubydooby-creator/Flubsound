// Flubsound Pro - audio thread -> GUI telemetry.
//
// Scalars: relaxed atomics written once per block by the audio thread and
// polled by the GUI at display rate (~60 Hz). No locks, no allocation.
// Streams : two SPSC rings (pre / post processing, mid = (L+R)/2 samples) that
// feed the spectrum analyser and waveform view. If the GUI is slow or hidden
// the rings simply fill and further pushes are dropped (never blocks audio).
#pragma once

#include "flub/common/SpscRing.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace flub
{
struct MeterBus
{
    static constexpr int kMaxMeterChannels = 2;
    static constexpr int kMaxDynBands = 8;

    // Levels (dBFS)
    std::array<std::atomic<float>, kMaxMeterChannels> inPeakDb {}, inRmsDb {}, outPeakDb {}, outRmsDb {};
    std::atomic<float> outTruePeakDb { -160.0f };    // max over last block
    std::atomic<float> outTruePeakMaxDb { -160.0f }; // hold since reset

    // Loudness (LUFS / LU)
    std::atomic<float> inShortTermLufs { -160.0f };
    std::atomic<float> momentaryLufs { -160.0f }, shortTermLufs { -160.0f }, integratedLufs { -160.0f }, loudnessRangeLu { 0.0f };

    // Stereo
    std::atomic<float> correlation { 1.0f }, effectiveWidth { 1.0f };

    // Dynamics / protection (dB; reductions <= 0)
    std::atomic<float> compGainReductionDb { 0.0f }, compUpwardGainDb { 0.0f };
    std::atomic<float> maxGainReductionDb { 0.0f }, glueGainReductionDb { 0.0f }, clipEnergyRatioDb { -160.0f };
    std::atomic<float> distortionDb { -160.0f }; // measured THD+N of saturator + clipper (dB re output, 300 ms smoothing)
    // The share of the harmonics the bass harmonics and the air exciter add on
    // purpose (dB re their output, 300 ms smoothing; not budgeted, docs/03 §14.5).
    std::atomic<float> harmonicsDb { -160.0f };
    std::atomic<float> bassProtectionDb { 0.0f };
    std::array<std::atomic<float>, kMaxDynBands> dynEqGainDb {};
    std::atomic<float> governorScale { 1.0f };  // 1 = Boost Intensity fully applied
    // SafetyGovernor (docs/11 E06): its SafetyGovernor::State, the
    // SafetyGovernor::kReason* bits of the budgets that made it back off (0
    // while the scale is 1), and the ~3 s averages compared with the budgets.
    std::atomic<int> governorState { 0 };
    std::atomic<uint32_t> governorReason { 0 };
    std::atomic<float> governorGrDb { 0.0f }, governorDistortionDb { -160.0f };
    // The measured loop at protection strength Normal / Strict (docs/11 E06
    // Phase 3 / batch 2; the reason bits above then also carry
    // kReasonDynamics / kReasonHarmonics / kReasonTonal). governorStrength:
    // the ProtectionStrength the chain ran at. The harmonics and tonal scales
    // are 1 at Off. The audible residuals (WeightedResidual, dB re the
    // output) are what the drive loop compares (the drive span with the
    // bass engine's non-harmonic share) and what the harmonics loop compares,
    // and the bass engine's whole span; the output PLR over ~3 s; the
    // budgets they are held to in the current mode (SafetyGovernor::
    // budgetsFor). -160 dB / governorNoReading while Off or not yet measured.
    static constexpr float governorNoReading = 1000.0f; // PlrMeter::kNoReading
    std::atomic<int> governorStrength { 0 };
    std::atomic<float> governorHarmonicsScale { 1.0f }, governorTonalScale { 1.0f };
    std::atomic<float> governorDriveResidualDb { -160.0f }, governorHarmonicsResidualDb { -160.0f }, governorBassResidualDb { -160.0f };
    std::atomic<float> governorPlrDb { governorNoReading };
    std::atomic<float> governorResidualBudgetDb { -35.0f }, governorGrBudgetDb { -6.0f }, governorPlrBudgetDb { 8.0f };
    // Brightness (docs/11 E07, TonalBalanceMeter): the chain's net lift of
    // presence 2-5 kHz, harsh 5-10 kHz and air 10-16 kHz over its
    // 200 Hz - 1 kHz lift (dB), measured at Normal / Strict; -160 dB
    // otherwise. The budgets are SafetyGovernor::Budgets' presence / harsh / air.
    std::array<std::atomic<float>, 3> tonalLiftDb { { { -160.0f }, { -160.0f }, { -160.0f } } };
    std::array<std::atomic<float>, 3> tonalBudgetDb { { { 3.0f }, { 3.0f }, { 4.0f } } };
    // The Smoothness stage (docs/11 E07, SmoothnessGuard): its deepest cut
    // of the 5 - 10 kHz band over the last block (dB <= 0; 0 while it idles).
    std::atomic<float> smoothnessCutDb { 0.0f };
    std::atomic<float> autoLevelGainDb { 0.0f };
    std::atomic<float> autoDriveDb { 0.0f };

    // Input channels (5.1 / 7.1 strips, docs/11 E27): bit c = channel c
    // carries content (ActiveChannelDetector); the fold in use (0 surround,
    // 1 stereo passthrough) and whether surround content was confirmed.
    std::atomic<uint32_t> activeChannelMask { 0 };
    std::atomic<int> inputFold { 0 };
    std::atomic<bool> surroundConfirmed { false };

    // Engine
    std::atomic<float> latencyMs { 0.0f };
    std::atomic<uint64_t> safetyClipCount { 0 };
    // Input sanitiser (docs/11 E10), since prepare(): finite samples beyond
    // +24 dBFS muted, and blocks dropped for a NaN / Inf.
    std::atomic<uint64_t> corruptSampleCount { 0 }, droppedBlockCount { 0 };

    /** GUI -> audio: request integrated loudness / TP-hold reset. */
    std::atomic<bool> resetLoudnessRequest { false };
};

struct AnalyzerTaps
{
    static constexpr size_t kCapacity = 1 << 15; // ~0.68 s at 48 kHz

    SpscRing<float> pre { kCapacity };
    SpscRing<float> post { kCapacity };
};
} // namespace flub
