// Flubsound Pro - one display frame worth of telemetry.
//
// MainComponent reads the selected strip's flub::MeterBus atomics ONCE per
// display frame into this plain struct and hands it to the meter / loudness /
// boost / EQ views, so these views never touch the atomics and all show
// values from the same instant. (RoutingPanel's per-strip mini meters are
// the exception: they read outPeakDb of every strip's MeterBus themselves.)
#pragma once

#include "flub/engine/MeterBus.h"

#include <array>
#include <cstdint>

namespace flub::app::ui
{
struct MeterSnapshot
{
    std::array<float, 2> inPeakDb { -160.0f, -160.0f }, inRmsDb { -160.0f, -160.0f };
    std::array<float, 2> outPeakDb { -160.0f, -160.0f }, outRmsDb { -160.0f, -160.0f };
    float outTruePeakDb = -160.0f, outTruePeakMaxDb = -160.0f;

    float inShortTermLufs = -160.0f;
    float momentaryLufs = -160.0f, shortTermLufs = -160.0f, integratedLufs = -160.0f, loudnessRangeLu = 0.0f;

    float correlation = 1.0f, effectiveWidth = 1.0f;

    float compGainReductionDb = 0.0f, compUpwardGainDb = 0.0f;
    float maxGainReductionDb = 0.0f, glueGainReductionDb = 0.0f, clipEnergyRatioDb = -160.0f, distortionDb = -160.0f;
    float bassProtectionDb = 0.0f;
    std::array<float, flub::MeterBus::kMaxDynBands> dynEqGainDb {};
    float governorScale = 1.0f, autoLevelGainDb = 0.0f, autoDriveDb = 0.0f;
    // SafetyGovernor (docs/11 E06): SafetyGovernor::State, kReason* bits and
    // the ~3 s averages its budgets are compared with.
    int governorState = 0;
    uint32_t governorReason = 0;
    float governorGrDb = 0.0f, governorDistortionDb = -160.0f;
    // The measured loop at protection strength Normal / Strict (docs/11 E06
    // Phase 3): the strength the chain ran at, the harmonics and tonal
    // scales, the audible residuals (drive span, harmonics, bass span) and
    // the output PLR against their budgets (-160 dB / MeterBus::
    // governorNoReading while Off or not measured yet).
    int governorStrength = 0;
    float governorHarmonicsScale = 1.0f, governorTonalScale = 1.0f;
    float governorDriveResidualDb = -160.0f, governorHarmonicsResidualDb = -160.0f, governorBassResidualDb = -160.0f;
    float governorPlrDb = flub::MeterBus::governorNoReading;
    float governorResidualBudgetDb = -35.0f, governorGrBudgetDb = -6.0f, governorPlrBudgetDb = 8.0f;
    // Brightness (docs/11 E07): the net lift of presence / harsh / air over
    // 200 Hz - 1 kHz (dB; -160 while not measured) and their budgets.
    std::array<float, 3> tonalLiftDb { -160.0f, -160.0f, -160.0f }, tonalBudgetDb { 3.0f, 3.0f, 4.0f };
    float harmonicsDb = -160.0f; // intended harmonics (bass harmonics, air exciter)
    int inputFold = 0;           // 0 surround (virtualiser), 1 stereo passthrough

    // Not on the MeterBus: the chain's automatic preamp (docs/11 E11), set
    // by the owner from ProcessingChain::getAutoPreampDb / getPredictedBoostDb.
    float autoPreampDb = 0.0f, predictedBoostDb = 0.0f;

    float latencyMs = 0.0f;
    float masterGainReductionDb = 0.0f; // master safety limiter (MixEngine)
    bool active = false;                // the strip currently receives audio

    /** Relaxed loads of every field. */
    void read (const flub::MeterBus& bus) noexcept;
};
} // namespace flub::app::ui
