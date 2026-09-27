#include "flub/engine/Protection.h"

#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>

namespace flub
{
namespace
{
/** Move `current` towards `target` by at most up/down dB per second. */
float slew (float current, float target, float upDbPerSec, float downDbPerSec, double seconds) noexcept
{
    const float maxUp = static_cast<float> (upDbPerSec * seconds);
    const float maxDown = static_cast<float> (downDbPerSec * seconds);
    if (target > current)
        return std::min (target, current + maxUp);
    return std::max (target, current - maxDown);
}

/** dB (power ratio) -> power; kMinusInfDb and below (and NaN) -> 0. */
float dbToPower (float db) noexcept
{
    return db > kMinusInfDb ? std::pow (10.0f, db * 0.1f) : 0.0f;
}
} // namespace

// ---------------------------------------------------------------------------
void DistortionMonitor::prepare (double sampleRate) noexcept
{
    sr = sampleRate;
    reset();
}

void DistortionMonitor::reset() noexcept FLUB_NONBLOCKING
{
    blockDb = smoothedDb = kMinusInfDb;
    smoothedPow = 0.0f;
}

float DistortionMonitor::combineDb (float aDb, float bDb) noexcept FLUB_NONBLOCKING
{
    // A missing term passes the other through exactly (no dB round trip).
    if (! (aDb > kMinusInfDb))
        return bDb > kMinusInfDb ? bDb : kMinusInfDb;
    if (! (bDb > kMinusInfDb))
        return aDb;
    return powerToDb (dbToPower (aDb) + dbToPower (bDb));
}

float DistortionMonitor::update (float saturatorDb, float clipperDb, int numSamples) noexcept FLUB_NONBLOCKING
{
    // Stages in series: the saturator's residual passes the (linear part of
    // the) clipper at the same ratio to the signal, and the two residuals
    // are treated as uncorrelated, so their ratios add in power.
    const float blockPow = dbToPower (saturatorDb) + dbToPower (clipperDb);
    blockDb = combineDb (saturatorDb, clipperDb);
    const float a = static_cast<float> (std::exp (-(numSamples / sr) / kMeterTauSeconds));
    smoothedPow = a * smoothedPow + (1.0f - a) * blockPow;
    smoothedDb = powerToDb (smoothedPow);
    return blockDb;
}

// ---------------------------------------------------------------------------
void SafetyGovernor::prepare (double sampleRate) noexcept
{
    sr = sampleRate;
    reset();
}

void SafetyGovernor::reset() noexcept FLUB_NONBLOCKING
{
    avgGrDb = 0.0f;
    avgDistortionDb = kMinusInfDb;
    scale = 1.0f;
}

void SafetyGovernor::update (float limiterGrDb, float distortionDb, int numSamples) noexcept FLUB_NONBLOCKING
{
    constexpr float kHysteresisDb = 1.5f;
    constexpr float kFallPerSec = 0.15f;
    constexpr float kRisePerSec = 0.03f;
    constexpr float kMinScale = 0.3f;

    const double dt = numSamples / sr;
    const float a = static_cast<float> (std::exp (-dt / 3.0)); // ~3 s averaging

    avgGrDb = a * avgGrDb + (1.0f - a) * limiterGrDb;
    // Average the THD+N in the power domain (dB averages would under-weight bursts).
    avgDistortionDb = powerToDb (a * dbToPower (avgDistortionDb) + (1.0f - a) * dbToPower (distortionDb));

    const bool over = avgGrDb < kGrBudgetDb || avgDistortionDb > kDistortionBudgetDb;
    const bool comfortablyUnder = avgGrDb > kGrBudgetDb + kHysteresisDb && avgDistortionDb < kDistortionBudgetDb - kHysteresisDb;

    if (over)
        scale = std::max (kMinScale, scale - static_cast<float> (kFallPerSec * dt));
    else if (comfortablyUnder)
        scale = std::min (1.0f, scale + static_cast<float> (kRisePerSec * dt));
}

// ---------------------------------------------------------------------------
void AutoLevel::prepare (double sampleRate, int numChannels)
{
    sr = sampleRate;
    follower.prepare (sampleRate, numChannels);
    reset();
}

void AutoLevel::reset() noexcept
{
    follower.reset();
    gainDb = 0.0f;
    lastLinear = 1.0f;
}

void AutoLevel::process (const AudioBlock& block) noexcept
{
    follower.process (block); // measured BEFORE our gain: open-loop, unconditionally stable
    const double dt = block.numSamples / sr;

    if (enabled)
    {
        if (follower.isActive()) // gated: frozen during silence, pauses and fade-outs
        {
            const float desired = std::clamp (target - follower.getLufs(), -12.0f, 12.0f);
            gainDb = slew (gainDb, desired, 1.0f, 4.0f, dt);
        }
    }
    else
    {
        gainDb = slew (gainDb, 0.0f, 4.0f, 4.0f, dt);
    }

    const float newLinear = dbToGain (gainDb);
    block.applyGainRamp (lastLinear, newLinear);
    lastLinear = newLinear;
}

// ---------------------------------------------------------------------------
void AutoDrive::prepare (double sampleRate, int numChannels)
{
    sr = sampleRate;
    follower.prepare (sampleRate, numChannels);
    reset();
}

void AutoDrive::reset() noexcept
{
    follower.reset();
    reductionDb = 0.0f;
}

float AutoDrive::update (const AudioBlock& output, float targetLufs, bool enabled, float requestedDriveDb) noexcept
{
    follower.process (output);
    const double dt = output.numSamples / sr;
    // Only the requested drive can be taken away (drive is floored at 0 dB).
    const float floorDb = -std::clamp (std::isfinite (requestedDriveDb) ? requestedDriveDb : 0.0f, 0.0f, 24.0f);
    reductionDb = std::max (reductionDb, floorDb);

    if (! enabled)
    {
        reductionDb = slew (reductionDb, 0.0f, 4.0f, 4.0f, dt);
        return reductionDb;
    }
    if (! follower.isActive())
        return reductionDb;

    // Closed loop on the processed output: integrate the error slowly with a
    // 0.5 LU dead band; the result is only ever a reduction (<= 0 dB).
    const float error = follower.getLufs() - targetLufs;
    if (error > 0.5f)
        reductionDb = std::max (floorDb, reductionDb - static_cast<float> (std::min (2.0, 0.5 * error) * dt));
    else if (error < -0.5f)
        reductionDb = std::min (0.0f, reductionDb + static_cast<float> (std::min (2.0, -0.5 * error) * dt));
    return reductionDb;
}

// ---------------------------------------------------------------------------
void LoudnessMatch::prepare (double sampleRate, int numChannels)
{
    sr = sampleRate;
    dryF.prepare (sampleRate, numChannels);
    wetF.prepare (sampleRate, numChannels);
    reset();
}

void LoudnessMatch::reset() noexcept
{
    dryF.reset();
    wetF.reset();
    gainDb = 0.0f;
}

void LoudnessMatch::measureDry (const AudioBlock& dry) noexcept { dryF.process (dry); }
void LoudnessMatch::measureWet (const AudioBlock& wet) noexcept { wetF.process (wet); }

float LoudnessMatch::getDryGainDb (int numSamples) noexcept
{
    if (dryF.isActive() && wetF.isActive())
    {
        const float desired = std::clamp (wetF.getLufs() - dryF.getLufs(), -12.0f, 12.0f);
        gainDb = slew (gainDb, desired, 3.0f, 3.0f, numSamples / sr);
    }
    return gainDb;
}
} // namespace flub
