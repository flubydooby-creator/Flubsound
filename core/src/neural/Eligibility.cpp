#include "flub/neural/Eligibility.h"

#include <cmath>

namespace flub
{
int neuralLatencyBudgetFrames (param::LatencyProfileValue profile) noexcept
{
    switch (profile)
    {
        case param::LatencyProfileValue::LowLatency: return 1;
        case param::LatencyProfileValue::Balanced: return 2;
        case param::LatencyProfileValue::Quality: return 0;
    }
    return 0;
}

bool isEligible (param::LatencyProfileValue profile, int modelLatencySamples, double sampleRate, ModelContext context) noexcept
{
    if (modelLatencySamples < 0 || ! std::isfinite (sampleRate) || sampleRate <= 0.0)
        return false;
    if (context == ModelContext::Offline || profile == param::LatencyProfileValue::Quality)
        return true;

    // latency / sampleRate <= budget * 10 ms, compared without dividing, so
    // exactly 480 samples at 48 kHz (441 at 44.1 kHz) is one frame.
    const int budget = neuralLatencyBudgetFrames (profile);
    if (budget <= 0)
        return false; // unknown profile value
    return static_cast<double> (modelLatencySamples) * 1000.0 <= static_cast<double> (budget) * kNeuralReferenceFrameMs * sampleRate;
}
} // namespace flub
