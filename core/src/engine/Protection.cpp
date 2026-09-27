#include "flub/engine/Protection.h"

#include "flub/analysis/ChannelWeights.h"
#include "flub/analysis/LoudnessMeter.h"
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
    harmonicsBlockDb = harmonicsSmoothedDb = kMinusInfDb;
    harmonicsSmoothedPow = 0.0f;
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
    smoothedDb = smooth (smoothedPow, blockPow, numSamples);
    return blockDb;
}

float DistortionMonitor::updateHarmonics (float bassDb, float airDb, int numSamples) noexcept FLUB_NONBLOCKING
{
    // Same combination as the THD+N (the air exciter follows the bass engine
    // in the chain), kept in its own sums: these stages add harmonics on
    // purpose, so their share is reported, never budgeted.
    harmonicsBlockDb = combineDb (bassDb, airDb);
    harmonicsSmoothedDb = smooth (harmonicsSmoothedPow, dbToPower (bassDb) + dbToPower (airDb), numSamples);
    return harmonicsBlockDb;
}

float DistortionMonitor::smooth (float& statePow, float blockPow, int numSamples) const noexcept FLUB_NONBLOCKING
{
    const float a = static_cast<float> (std::exp (-(numSamples / sr) / kMeterTauSeconds));
    statePow = a * statePow + (1.0f - a) * blockPow;
    return powerToDb (statePow);
}

// ---------------------------------------------------------------------------
void SafetyGovernor::prepare (double sampleRate) noexcept
{
    sr = sampleRate;
    // The maximizer's limiter-GR window, counted the same way, so the ticks
    // fall where its windows close (both grids start at reset()).
    tickSamples = msToSamples (kTickMs, sampleRate);
    const double dt = tickSamples / sampleRate;
    tickAverage = static_cast<float> (std::exp (-dt / 3.0)); // ~3 s averaging
    tickFall = static_cast<float> (kFallPerSec * dt);
    tickRise = static_cast<float> (kRisePerSec * dt);
    reset();
}

void SafetyGovernor::reset() noexcept FLUB_NONBLOCKING
{
    avgGrDb = 0.0f;
    avgDistortionDb = kMinusInfDb;
    scale = 1.0f;
    state = State::Idle;
    reason = 0;
    pendingSamples = 0;
}

void SafetyGovernor::setStrength (ProtectionStrength s) noexcept FLUB_NONBLOCKING
{
    minScale = s == ProtectionStrength::Strict ? kStrictMinScale : kMinScale;
    scale = std::max (scale, minScale);
}

void SafetyGovernor::update (float limiterGrDb, float distortionDb, int numSamples) noexcept FLUB_NONBLOCKING
{
    pendingSamples += numSamples;
    while (pendingSamples >= tickSamples)
    {
        pendingSamples -= tickSamples;
        tick (limiterGrDb, distortionDb);
    }
}

void SafetyGovernor::skip (int numSamples) noexcept FLUB_NONBLOCKING
{
    pendingSamples = (pendingSamples + numSamples) % tickSamples;
}

void SafetyGovernor::tick (float limiterGrDb, float distortionDb) noexcept FLUB_NONBLOCKING
{
    const float a = tickAverage;
    avgGrDb = a * avgGrDb + (1.0f - a) * limiterGrDb;
    // Average the THD+N in the power domain (dB averages would under-weight bursts).
    avgDistortionDb = powerToDb (a * dbToPower (avgDistortionDb) + (1.0f - a) * dbToPower (distortionDb));

    const bool grOver = avgGrDb < kGrBudgetDb, distortionOver = avgDistortionDb > kDistortionBudgetDb;
    const bool comfortablyUnder = avgGrDb > kGrBudgetDb + kHysteresisDb && avgDistortionDb < kDistortionBudgetDb - kHysteresisDb;

    if (grOver || distortionOver)
    {
        scale = std::max (minScale, scale - tickFall);
        reason |= (grOver ? kReasonLimiter : 0u) | (distortionOver ? kReasonDistortion : 0u);
        state = State::BackingOff;
    }
    else if (comfortablyUnder)
    {
        scale = std::min (1.0f, scale + tickRise);
        state = scale < 1.0f ? State::Recovering : State::Idle;
    }
    else
    {
        state = scale < 1.0f ? State::Holding : State::Idle;
    }
    if (state == State::Idle)
        reason = 0;
}

// ---------------------------------------------------------------------------
void AutoLevel::prepare (double sampleRate, int numChannels)
{
    sr = sampleRate;
    follower.setUpperGate (true);
    follower.prepare (sampleRate, numChannels);
    reset();
}

void AutoLevel::reset() noexcept
{
    follower.reset();
    gainDb = 0.0f;
    lastLinear = 1.0f;
    recoveryLeft = 0.0;
    frozen = false;
}

void AutoLevel::process (const AudioBlock& block) noexcept FLUB_NONBLOCKING
{
    run (block, true);
}

void AutoLevel::processUnmeasured (const AudioBlock& block) noexcept FLUB_NONBLOCKING
{
    run (block, false);
}

void AutoLevel::run (const AudioBlock& block, bool measure) noexcept FLUB_NONBLOCKING
{
    if (measure)
        follower.process (block); // measured BEFORE our gain: open-loop, unconditionally stable
    const double dt = block.numSamples / sr;

    if (enabled)
    {
        if (! measure)
        {
            // Hidden from the loop (docs/11 E10): the gain holds, as in a pause.
        }
        else if (follower.isActive()) // frozen during silence, pauses, fade-outs and loud events
        {
            if (frozen)
            {
                // Programme was kept out (a loud event held by the upper gate,
                // or a level far below the last one): catch up faster.
                frozen = false;
                recoveryLeft = kRecoverySeconds;
            }
            const float up = recoveryLeft > 0.0 ? kRecoveryUpDbPerSec : kUpDbPerSec;
            const float desired = std::clamp (target - follower.getLufs(), kMinGainDb, kMaxGainDb);
            gainDb = slew (gainDb, desired, up, kDownDbPerSec, dt);
            recoveryLeft = std::max (0.0, recoveryLeft - dt);
        }
        else if (follower.hasProgramme())
        {
            frozen = true;
        }
    }
    else
    {
        gainDb = slew (gainDb, 0.0f, 4.0f, 4.0f, dt);
        frozen = false;
        recoveryLeft = 0.0;
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
void ComparisonMatcher::Side::prepare (double sampleRate) noexcept
{
    stage1.setCoeffs (LoudnessMeter::kWeightingStage1 (sampleRate));
    stage2.setCoeffs (LoudnessMeter::kWeightingStage2 (sampleRate));
    reset();
}

void ComparisonMatcher::Side::reset() noexcept
{
    stage1.reset();
    stage2.reset();
    subBlock = 0.0;
    ring.fill (0.0);
}

void ComparisonMatcher::Side::process (const AudioBlock& block, int numChannels) noexcept FLUB_NONBLOCKING
{
    // As in LoudnessFollower: a -400 dBFS DC keeps the filter states normal
    // in digital silence (the RLB high-pass removes it).
    constexpr double antiDenormal = 1.0e-20;
    const int nch = std::min (block.numChannels, numChannels);
    double sum = 0.0;
    for (int c = 0; c < nch; ++c)
    {
        const double w = bs1770ChannelWeight (c, nch);
        if (w == 0.0)
            continue;
        const float* x = block.channel (c);
        double acc = 0.0;
        for (int i = 0; i < block.numSamples; ++i)
        {
            const double k = stage2.processSample (c, stage1.processSample (c, static_cast<double> (x[i]) + antiDenormal));
            acc += k * k;
        }
        sum += w * acc;
    }
    if (std::isfinite (sum))
    {
        subBlock += sum;
    }
    else
    {
        // A NaN/Inf input would otherwise stay in the filter states; the
        // block is left out of the measurement.
        stage1.reset();
        stage2.reset();
    }
}

double ComparisonMatcher::Side::windowSum() const noexcept
{
    double sum = 0.0;
    for (double e : ring)
        sum += e;
    return sum;
}

void ComparisonMatcher::prepare (double sampleRate, int numChannels)
{
    sr = sampleRate;
    channels = std::clamp (numChannels, 1, kMaxChannels);
    dry.prepare (sampleRate);
    wet.prepare (sampleRate);
    subBlockSamples = std::max<std::int64_t> (1, static_cast<std::int64_t> (kSubBlockSeconds * sampleRate));
    acquireSamples = static_cast<std::int64_t> (kAcquireSeconds * sampleRate);
    sessionEndSamples = static_cast<std::int64_t> (kSessionEndSeconds * sampleRate);
    reset();
}

void ComparisonMatcher::reset() noexcept
{
    dry.reset();
    wet.reset();
    ringPos = ringCount = 0;
    pendingSamples = 0;
    dryTrimDb = wetTrimDb = differenceLu = 0.0f;
    lastWetGain = 1.0f;
    acquiredSamples = offSamples = 0;
    comparing = measured = false;
}

void ComparisonMatcher::measureDry (const AudioBlock& block) noexcept FLUB_NONBLOCKING { dry.process (block, channels); }
void ComparisonMatcher::measureWet (const AudioBlock& block) noexcept FLUB_NONBLOCKING { wet.process (block, channels); }

void ComparisonMatcher::closeSubBlock() noexcept FLUB_NONBLOCKING
{
    // K-weighted mean squares of -60 and -70 LUFS: 10^((L + 0.691) / 10).
    constexpr double kGateMs = 1.1725e-6, kFloorMs = 1.1725e-7;
    const double gate = kGateMs * static_cast<double> (pendingSamples);
    if (dry.subBlock > gate || wet.subBlock > gate)
    {
        dry.ring[static_cast<size_t> (ringPos)] = dry.subBlock;
        wet.ring[static_cast<size_t> (ringPos)] = wet.subBlock;
        ringPos = (ringPos + 1) % kWindowSubBlocks;
        ringCount = std::min (ringCount + 1, kWindowSubBlocks);
        const double d = dry.windowSum(), w = wet.windowSum();
        // Both sides must be programme over the window (-70 LUFS): a side
        // that went silent is not matched down to.
        const double floor = kFloorMs * static_cast<double> (subBlockSamples * ringCount);
        measured = ringCount >= kMinSubBlocks && d > floor && w > floor;
        if (measured)
            differenceLu = static_cast<float> (10.0 * std::log10 (w / d));
    }
    dry.subBlock = wet.subBlock = 0.0;
    pendingSamples = 0;
}

void ComparisonMatcher::update (bool bypassEngaged, bool matching, int numSamples) noexcept FLUB_NONBLOCKING
{
    advance (bypassEngaged, matching, numSamples, true);
}

void ComparisonMatcher::updateUnmeasured (bool bypassEngaged, bool matching, int numSamples) noexcept FLUB_NONBLOCKING
{
    advance (bypassEngaged, matching, numSamples, false);
}

void ComparisonMatcher::advance (bool bypassEngaged, bool matching, int numSamples, bool measuredBlock) noexcept FLUB_NONBLOCKING
{
    if (measuredBlock)
    {
        pendingSamples += numSamples;
        if (pendingSamples >= subBlockSamples)
            closeSubBlock();
    }

    const double dt = numSamples / sr;
    if (! matching)
    {
        comparing = false;
        dryTrimDb = 0.0f;
        wetTrimDb = slew (wetTrimDb, 0.0f, kReleaseDbPerSec, kReleaseDbPerSec, dt);
        return;
    }

    if (bypassEngaged)
    {
        if (! comparing)
        {
            comparing = true; // a new comparison: acquire again
            acquiredSamples = 0;
        }
        offSamples = 0;
    }
    else if (comparing)
    {
        offSamples += numSamples;
        if (offSamples >= sessionEndSamples)
            comparing = false;
    }

    if (! comparing)
    {
        dryTrimDb = 0.0f;
        wetTrimDb = slew (wetTrimDb, 0.0f, kReleaseDbPerSec, kReleaseDbPerSec, dt);
        return;
    }
    // Acquire: follow the live difference for the first kAcquireSeconds of
    // measured programme, then hold it for the whole comparison.
    if (acquiredSamples < acquireSamples && measured)
    {
        const float delta = std::clamp (differenceLu, -kMaxTrimDb, kMaxTrimDb);
        dryTrimDb = std::min (0.0f, delta);  // the reference is louder: turn it down
        wetTrimDb = std::min (0.0f, -delta); // the processed side is louder: turn it down
        acquiredSamples += numSamples;
    }
}

void ComparisonMatcher::applyWetTrim (const AudioBlock& block) noexcept FLUB_NONBLOCKING
{
    const float g = dbToGain (wetTrimDb);
    block.applyGainRamp (lastWetGain, g);
    lastWetGain = g;
}
} // namespace flub
