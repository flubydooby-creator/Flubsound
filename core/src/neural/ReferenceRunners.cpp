#include "flub/neural/ReferenceRunners.h"

#include "flub/common/Math.h"

#include <algorithm>

namespace flub
{
namespace
{
ModelDescription makeDescription (int frameSize, int numControls, ControlKind kind) noexcept
{
    ModelDescription d;
    d.frameSize = frameSize;
    d.numInputChannels = 1;
    d.numControls = numControls;
    d.controlKind = kind;
    return d;
}
} // namespace

IdentityRunner::IdentityRunner (int frameSize, int numControls, ControlKind kind)
    : desc (makeDescription (frameSize, numControls, kind))
{
}

bool IdentityRunner::run (const float*, float* outControls)
{
    std::fill (outControls, outControls + desc.numControls, 1.0f);
    return true;
}

ConstantGainRunner::ConstantGainRunner (int frameSize, float gainDb, int numControls, ControlKind kind)
    : desc (makeDescription (frameSize, numControls, kind)), gain (dbToGain (gainDb))
{
}

bool ConstantGainRunner::run (const float*, float* outControls)
{
    std::fill (outControls, outControls + desc.numControls, gain);
    return true;
}

FailingRunner::FailingRunner (int frameSize)
    : desc (makeDescription (frameSize, 1, ControlKind::BroadbandGain))
{
}

bool FailingRunner::run (const float*, float*)
{
    return false;
}
} // namespace flub
