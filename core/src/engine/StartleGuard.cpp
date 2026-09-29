#include "flub/engine/StartleGuard.h"

#include "flub/analysis/LoudnessMeter.h"
#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace flub
{
namespace
{
// Audio EQ Cookbook (RBJ) sections, normalised to a0 = 1.
BiquadCoeffs highPass (double hz, double sampleRate) noexcept
{
    const double w = kTwoPi * std::min (hz, 0.45 * sampleRate) / sampleRate;
    const double cw = std::cos (w), alpha = std::sin (w) / std::sqrt (2.0), a0 = 1.0 + alpha;
    BiquadCoeffs c;
    c.b0 = 0.5 * (1.0 + cw) / a0;
    c.b1 = -(1.0 + cw) / a0;
    c.b2 = c.b0;
    c.a1 = -2.0 * cw / a0;
    c.a2 = (1.0 - alpha) / a0;
    return c;
}

BiquadCoeffs peaking (double hz, double q, double gainDb, double sampleRate) noexcept
{
    const double w = kTwoPi * std::min (hz, 0.45 * sampleRate) / sampleRate;
    const double cw = std::cos (w), alpha = std::sin (w) / (2.0 * q), A = std::pow (10.0, gainDb / 40.0);
    const double a0 = 1.0 + alpha / A;
    BiquadCoeffs c;
    c.b0 = (1.0 + alpha * A) / a0;
    c.b1 = -2.0 * cw / a0;
    c.b2 = (1.0 - alpha * A) / a0;
    c.a1 = c.b1;
    c.a2 = (1.0 - alpha / A) / a0;
    return c;
}

// guard.range: Off, 20 LU, 15 LU, 10 LU (Balanced), 6 LU (Shield).
constexpr float kCeilings[] = { 0.0f, 20.0f, 15.0f, 10.0f, 6.0f };
constexpr float kTame[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
constexpr float kIdleDb = -0.005f; // a gain above this is unity (and the stage may idle)
} // namespace

float StartleGuard::ceilingLuFor (int rangeChoice) noexcept
{
    return rangeChoice >= 0 && rangeChoice < static_cast<int> (std::size (kCeilings)) ? kCeilings[rangeChoice] : 0.0f;
}

float StartleGuard::tameAmountFor (int rangeChoice) noexcept
{
    return rangeChoice >= 0 && rangeChoice < static_cast<int> (std::size (kTame)) ? kTame[rangeChoice] : 0.0f;
}

void StartleGuard::prepare (double sampleRate, int maxBlockSize, int lookaheadSamples)
{
    sr = sampleRate;
    maxBlock = std::max (1, maxBlockSize);
    hpCoeffs = highPass (kSidechainHpHz, sr);
    dipCoeffs = peaking (kCueDipHz, kCueDipQ, kCueDipDb, sr);
    k1Coeffs = LoudnessMeter::kWeightingStage1 (sr);
    k2Coeffs = LoudnessMeter::kWeightingStage2 (sr);
    gains.assign (static_cast<size_t> (maxBlock), 1.0f);
    fastCoeff = static_cast<double> (onePoleCoeff (kFastMs, sr));
    stepLength = std::max (1, msToSamples (kMomentaryStepMs, sr));
    slowCoeff = std::exp (-1.0 / (0.001 * kReferenceMs * sr));
    minFill = 1.0 - std::exp (-static_cast<double> (kMinReferenceMs) / kReferenceMs);
    crestFactor = std::pow (10.0, -0.1 * static_cast<double> (kCrestAllowanceDb));
    silencePower = std::pow (10.0, 0.1 * (static_cast<double> (kSilenceLufs) + 0.691));
    eventGateFactor = std::pow (10.0, 0.1 * static_cast<double> (kEventGateLu));
    quietFactor = std::pow (10.0, -0.1 * static_cast<double> (kQuietRestartLu));
    eventHoldSamples = msToSamples (kEventHoldMs, sr);
    newLevelSamples = msToSamples (1000.0f * kNewLevelSeconds, sr);
    quietRestartSamples = msToSamples (1000.0f * kQuietRestartSeconds, sr);
    attackCoeff = onePoleCoeff (std::max (kMinAttackMs, static_cast<float> (1000.0 * lookaheadSamples / (3.0 * sr))), sr);
    releaseCoeff = onePoleCoeff (kReleaseMs, sr);
    holdSamples = msToSamples (kHoldMs, sr);
    reset();
}

void StartleGuard::restartReference() noexcept
{
    slowMs = slowFill = 0.0;
    eventHoldLeft = eventRun = quietRun = 0;
}

void StartleGuard::reset() noexcept FLUB_NONBLOCKING
{
    for (auto* s : { &hpState, &dipState, &k1State, &k2State })
        for (auto& st : *s)
            st.reset();
    stepSums.fill (0.0);
    stepsSum = partialSum = 0.0;
    partialLength = stepCount = stepPos = 0;
    fastMs = 0.0;
    restartReference();
    holdLeft = 0;
    heldTargetDb = gainDb = 0.0f;
    lastLinear = 1.0f;
    pendingSamples = 0;
    gainIdle = true;
    running = false;
    blockGainDb.store (0.0f, std::memory_order_relaxed);
}

void StartleGuard::setCeilingLu (float lu) noexcept FLUB_NONBLOCKING
{
    ceilingLu = std::isfinite (lu) ? std::max (0.0f, lu) : 0.0f;
}

void StartleGuard::setLevelOffsetDb (float db) noexcept FLUB_NONBLOCKING
{
    levelOffsetDb = std::isfinite (db) ? db : 0.0f;
}

float StartleGuard::getReferenceLufs() const noexcept
{
    if (! running || slowFill < minFill || ! (slowMs > 0.0))
        return kMinusInfDb;
    return static_cast<float> (10.0 * std::log10 (slowMs / slowFill) - 0.691) + levelOffsetDb;
}

void StartleGuard::measure (const AudioBlock& block, bool unmeasured) noexcept FLUB_NONBLOCKING
{
    const int n = std::min (block.numSamples, maxBlock);
    const bool on = ceilingLu > 0.0f;
    pendingSamples = n;
    if (! on && gainIdle)
    {
        // Off and released: idle (the audio untouched, nothing measured). The
        // next switch-on starts from a fresh reference.
        running = false;
        gainDb = heldTargetDb = 0.0f;
        blockGainDb.store (0.0f, std::memory_order_relaxed);
        return;
    }
    if (! running)
    {
        reset();
        pendingSamples = n;
        running = true;
    }

    float deepest = gainDb;
    if (unmeasured)
    {
        // Hidden from the control loops (docs/11 E10): the gain holds.
        std::fill (gains.begin(), gains.begin() + n, lastLinear);
        blockGainDb.store (deepest, std::memory_order_relaxed);
        return;
    }

    const double ceilingFactor = std::pow (10.0, 0.1 * static_cast<double> (ceilingLu));
    // The reference is kept in the terms of the level before the upstream
    // gain (AutoLevel's): it moves with that gain, frozen or not.
    const double offset = std::pow (10.0, 0.1 * static_cast<double> (levelOffsetDb));
    // A -400 dBFS DC keeps the filter states normal in digital silence (the
    // high-pass removes it), as in LoudnessFollower.
    constexpr double antiDenormal = 1.0e-20;
    const int nch = std::min (block.numChannels, 2);
    for (int i = 0; i < n; ++i)
    {
        double power = 0.0;
        for (int c = 0; c < nch; ++c)
        {
            const auto ch = static_cast<size_t> (c);
            const double y = biquadTick (dipCoeffs, dipState[ch], biquadTick (hpCoeffs, hpState[ch], static_cast<double> (block.channel (c)[i]) + antiDenormal));
            const double k = biquadTick (k2Coeffs, k2State[ch], biquadTick (k1Coeffs, k1State[ch], y));
            power += k * k;
        }
        fastMs = power + fastCoeff * (fastMs - power);
        // The momentary loudness (400 ms).
        partialSum += power;
        ++partialLength;
        const double momentaryMs = (stepsSum + partialSum) / static_cast<double> (stepCount * stepLength + partialLength);
        if (partialLength == stepLength)
        {
            stepSums[static_cast<size_t> (stepPos)] = partialSum;
            stepPos = (stepPos + 1) % (kMomentarySteps - 1);
            stepCount = std::min (stepCount + 1, kMomentarySteps - 1);
            stepsSum = 0.0; // summed afresh: no drift from running subtraction
            for (int k = 0; k < stepCount; ++k)
                stepsSum += stepSums[static_cast<size_t> (k)];
            partialSum = 0.0;
            partialLength = 0;
        }
        const double detector = std::max (momentaryMs, fastMs * crestFactor);

        // The reference (see the header comment): gated per sample.
        const bool valid = slowFill >= minFill;
        const double reference = valid ? offset * slowMs / slowFill : 0.0;
        const bool silent = momentaryMs < silencePower;
        const bool quiet = valid && ! silent && momentaryMs < reference * quietFactor;
        if (valid && detector > reference * eventGateFactor)
        {
            eventHoldLeft = eventHoldSamples;
            eventRun = std::min (eventRun + 1, newLevelSamples);
        }
        else if (eventHoldLeft > 0 && --eventHoldLeft == 0)
        {
            eventRun = 0;
        }
        if (! silent && ! quiet && (eventHoldLeft == 0 || eventRun >= newLevelSamples))
        {
            const double p = power / offset;
            slowMs = p + slowCoeff * (slowMs - p);
            slowFill = 1.0 + slowCoeff * (slowFill - 1.0);
        }
        if (quiet)
        {
            if (++quietRun >= quietRestartSamples)
                restartReference();
        }
        else
        {
            quietRun = 0;
        }

        float target = 0.0f;
        if (on && valid)
        {
            const double cap = reference * ceilingFactor;
            if (detector > cap)
                target = static_cast<float> (10.0 * std::log10 (cap / detector));
        }
        if (target <= heldTargetDb)
        {
            heldTargetDb = target;
            holdLeft = holdSamples;
        }
        else if (holdLeft > 0)
        {
            --holdLeft;
        }
        else
        {
            heldTargetDb = target + releaseCoeff * (heldTargetDb - target);
        }
        gainDb = heldTargetDb < gainDb ? heldTargetDb + attackCoeff * (gainDb - heldTargetDb) : heldTargetDb;
        deepest = std::min (deepest, gainDb);
        lastLinear = gainDb < kIdleDb ? dbToGain (gainDb) : 1.0f;
        gains[static_cast<size_t> (i)] = lastLinear;
    }

    // A non-finite sidechain (the chain drops non-finite blocks before this,
    // so only a filter blow-up could cause one) restarts the detector.
    if (! std::isfinite (fastMs) || ! std::isfinite (stepsSum + partialSum) || ! std::isfinite (slowMs) || ! std::isfinite (gainDb))
    {
        reset();
        running = true;
        pendingSamples = n;
        std::fill (gains.begin(), gains.begin() + n, 1.0f);
        return;
    }
    gainIdle = ! on && gainDb >= kIdleDb && heldTargetDb >= kIdleDb;
    blockGainDb.store (deepest, std::memory_order_relaxed);
}

void StartleGuard::apply (const AudioBlock& block) noexcept FLUB_NONBLOCKING
{
    if (! running)
        return;
    const int n = std::min (block.numSamples, pendingSamples);
    for (int c = 0; c < block.numChannels; ++c)
    {
        float* x = block.channel (c);
        for (int i = 0; i < n; ++i)
            x[i] *= gains[static_cast<size_t> (i)];
    }
}
} // namespace flub
