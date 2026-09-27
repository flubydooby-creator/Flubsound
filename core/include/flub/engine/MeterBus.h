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
    std::atomic<float> bassProtectionDb { 0.0f };
    std::array<std::atomic<float>, kMaxDynBands> dynEqGainDb {};
    std::atomic<float> governorScale { 1.0f };  // 1 = Boost Intensity fully applied
    std::atomic<float> autoLevelGainDb { 0.0f };
    std::atomic<float> autoDriveDb { 0.0f };

    // Engine
    std::atomic<float> latencyMs { 0.0f };
    std::atomic<uint64_t> safetyClipCount { 0 };

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
