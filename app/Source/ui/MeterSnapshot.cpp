#include "MeterSnapshot.h"

namespace flub::app::ui
{
void MeterSnapshot::read (const flub::MeterBus& bus) noexcept
{
    constexpr auto rl = std::memory_order_relaxed;
    for (size_t c = 0; c < 2; ++c)
    {
        inPeakDb[c] = bus.inPeakDb[c].load (rl);
        inRmsDb[c] = bus.inRmsDb[c].load (rl);
        outPeakDb[c] = bus.outPeakDb[c].load (rl);
        outRmsDb[c] = bus.outRmsDb[c].load (rl);
    }
    outTruePeakDb = bus.outTruePeakDb.load (rl);
    outTruePeakMaxDb = bus.outTruePeakMaxDb.load (rl);

    inShortTermLufs = bus.inShortTermLufs.load (rl);
    momentaryLufs = bus.momentaryLufs.load (rl);
    shortTermLufs = bus.shortTermLufs.load (rl);
    integratedLufs = bus.integratedLufs.load (rl);
    loudnessRangeLu = bus.loudnessRangeLu.load (rl);

    correlation = bus.correlation.load (rl);
    effectiveWidth = bus.effectiveWidth.load (rl);

    compGainReductionDb = bus.compGainReductionDb.load (rl);
    compUpwardGainDb = bus.compUpwardGainDb.load (rl);
    maxGainReductionDb = bus.maxGainReductionDb.load (rl);
    glueGainReductionDb = bus.glueGainReductionDb.load (rl);
    clipEnergyRatioDb = bus.clipEnergyRatioDb.load (rl);
    distortionDb = bus.distortionDb.load (rl);
    bassProtectionDb = bus.bassProtectionDb.load (rl);
    for (size_t b = 0; b < dynEqGainDb.size(); ++b)
        dynEqGainDb[b] = bus.dynEqGainDb[b].load (rl);
    governorScale = bus.governorScale.load (rl);
    governorState = bus.governorState.load (rl);
    governorReason = bus.governorReason.load (rl);
    governorGrDb = bus.governorGrDb.load (rl);
    governorDistortionDb = bus.governorDistortionDb.load (rl);
    harmonicsDb = bus.harmonicsDb.load (rl);
    inputFold = bus.inputFold.load (rl);
    autoLevelGainDb = bus.autoLevelGainDb.load (rl);
    autoDriveDb = bus.autoDriveDb.load (rl);
    latencyMs = bus.latencyMs.load (rl);
}
} // namespace flub::app::ui
