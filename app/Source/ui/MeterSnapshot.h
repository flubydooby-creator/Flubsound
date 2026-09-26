// Flubsound Pro - one display frame worth of telemetry.
//
// MainComponent reads the selected strip's flub::MeterBus atomics ONCE per
// display frame into this plain struct and hands it to the meter / loudness /
// boost / EQ views, so no view touches the atomics directly and every view
// shows values from the same instant.
#pragma once

#include "flub/engine/MeterBus.h"

#include <array>

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
    float maxGainReductionDb = 0.0f, glueGainReductionDb = 0.0f, clipEnergyRatioDb = -160.0f;
    float bassProtectionDb = 0.0f;
    std::array<float, flub::MeterBus::kMaxDynBands> dynEqGainDb {};
    float governorScale = 1.0f, autoLevelGainDb = 0.0f, autoDriveDb = 0.0f;

    float latencyMs = 0.0f;
    float masterGainReductionDb = 0.0f; // master safety limiter (MixEngine)
    bool active = false;                // the strip currently receives audio

    /** Relaxed loads of every field. */
    void read (const flub::MeterBus& bus) noexcept;
};
} // namespace flub::app::ui
