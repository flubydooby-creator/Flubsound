#include "flub/engine/Protection.h"

#include "flub/analysis/ChannelWeights.h"
#include "flub/analysis/LoudnessMeter.h"
#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

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
    // purpose, so their share is not in the THD+N budget (at Normal / Strict
    // the governor's harmonics loop budgets the bass share by what the
    // programme leaves exposed, docs/11 E06 Phase 3).
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
void DriveFeedForward::reset() noexcept FLUB_NONBLOCKING
{
    peaks.fill (0.0f);
    peaksDb.fill (kMinusInfDb);
    count = pos = 0;
    attack = static_cast<float> (std::exp (-kTickSeconds / kProgramAttackSeconds));
    release = static_cast<float> (std::exp (-kTickSeconds / kProgramReleaseSeconds));
}

void DriveFeedForward::pushTick (float peak, float rms) noexcept FLUB_NONBLOCKING
{
    const float p = std::isfinite (peak) ? std::max (0.0f, peak) : 0.0f;
    peaks[static_cast<size_t> (pos)] = p;
    peaksDb[static_cast<size_t> (pos)] = gainToDb (p);
    rmsLevels[static_cast<size_t> (pos)] = std::isfinite (rms) ? std::clamp (rms, 0.0f, p) : 0.0f;
    pos = (pos + 1) % kTicks;
    count = std::min (count + 1, kTicks);
}

void DriveFeedForward::setClipper (float headroomDb, float crestDb, float depthDb) noexcept FLUB_NONBLOCKING
{
    clipHeadroomDb = std::isnan (headroomDb) ? 1000.0f : std::min (headroomDb, 1000.0f);
    clipCrestDb = std::isfinite (crestDb) ? std::max (0.0f, crestDb) : 0.0f;
    clipDepthDb = std::isfinite (depthDb) ? std::clamp (depthDb, 0.0f, 24.0f) : 24.0f;
}

namespace
{
constexpr float kFeedForwardProgrammeDb = -70.0f; // a tick whose peak is below this is not programme
}

float DriveFeedForward::meanReductionDb (float ceilingDb, float driveDb, int programmeTicks) const noexcept FLUB_NONBLOCKING
{
    // In units of the input before the drive: the ceiling k, the clipper's
    // threshold t, its crest gate and depth cap (a peak over the threshold
    // comes out at the threshold, but loses at most the depth: the soft
    // knee is not modelled). The ticks in chronological order (oldest
    // first): the first pass only warms the envelope up on the window's last
    // kWarmTicks (1.25 release time constants), the second accumulates. The
    // product of the applied gains is taken in runs (their logs summed), so
    // a log per tick is not needed and the product cannot underflow.
    const double k = std::pow (10.0, static_cast<double> (ceilingDb - driveDb) / 20.0);
    const bool clipper = clipHeadroomDb < 100.0f;
    const double t = clipper ? k * std::pow (10.0, static_cast<double> (clipHeadroomDb) / 20.0) : 0.0;
    const double crest = clipCrestDb > 0.0f ? std::pow (10.0, static_cast<double> (clipCrestDb) / 20.0) : 0.0;
    const double depth = std::pow (10.0, -static_cast<double> (clipDepthDb) / 20.0);
    const int first = count == kTicks ? pos : 0;
    double env = 1.0, logSum = 0.0, product = 1.0;
    for (int pass = 0; pass < 2; ++pass)
        for (int j = pass == 0 ? std::max (0, count - kWarmTicks) : 0; j < count; ++j)
        {
            const auto i = static_cast<size_t> ((first + j) % kTicks);
            double peak = static_cast<double> (peaks[i]);
            if (clipper)
            {
                const double threshold = std::max (t, crest * static_cast<double> (rmsLevels[i]));
                if (peak > threshold)
                    peak = std::max (threshold, depth * peak);
            }
            const double g = peak > k ? k / peak : 1.0;
            env = g + static_cast<double> (g < env ? attack : release) * (env - g);
            if (pass == 1 && peaksDb[i] > kFeedForwardProgrammeDb)
            {
                product *= std::min (g, env);
                if (product < 1.0e-200)
                {
                    logSum += std::log (product);
                    product = 1.0;
                }
            }
        }
    logSum += std::log (product);
    return static_cast<float> (-20.0 / std::log (10.0) * logSum / programmeTicks);
}

float DriveFeedForward::driveForBudget (float ceilingDb, float grBudgetDb) const noexcept FLUB_NONBLOCKING
{
    // The glue and the LF-first limiter (which take part of the peaks
    // before the limiter) are not modelled, so with them on the prediction
    // reads deep: the governor's PI trim takes the rest, and its limiter
    // loop yields while the limiter is idle.
    int n = 0;
    for (int i = 0; i < count; ++i)
        n += peaksDb[static_cast<size_t> (i)] > kFeedForwardProgrammeDb ? 1 : 0;
    if (n < kMinTicks)
        return std::numeric_limits<float>::infinity();
    const float target = -grBudgetDb;
    float lo = -48.0f, hi = 72.0f;
    if (meanReductionDb (ceilingDb, hi, n) < target)
        return std::numeric_limits<float>::infinity();
    for (int it = 0; it < 13; ++it) // 120 dB / 2^13: 0.015 dB
    {
        const float mid = 0.5f * (lo + hi);
        (meanReductionDb (ceilingDb, mid, n) < target ? lo : hi) = mid;
    }
    return lo;
}

bool DriveFeedForward::hasReading() const noexcept FLUB_NONBLOCKING
{
    int n = 0;
    for (int i = 0; i < count && n < kMinTicks; ++i)
        n += peaksDb[static_cast<size_t> (i)] > kFeedForwardProgrammeDb ? 1 : 0;
    return n >= kMinTicks;
}

// ---------------------------------------------------------------------------
void PlrMeter::prepare (double sampleRate, int numChannels)
{
    loudness.prepare (sampleRate, numChannels, 3000.0f);
    logPole = std::log (static_cast<double> (onePoleCoeff (3000.0f, sampleRate)));
    reset();
}

void PlrMeter::reset() noexcept FLUB_NONBLOCKING
{
    loudness.reset();
    peaks.fill (0.0f);
    pos = count = 0;
    tickPeak = 0.0f;
    samples = 0;
}

void PlrMeter::process (const AudioBlock& block) noexcept FLUB_NONBLOCKING
{
    loudness.process (block);
    samples += block.numSamples;
    for (int c = 0; c < block.numChannels; ++c)
        for (int i = 0; i < block.numSamples; ++i)
            tickPeak = std::max (tickPeak, std::abs (block.channel (c)[i]));
}

void PlrMeter::tick() noexcept FLUB_NONBLOCKING
{
    peaks[static_cast<size_t> (pos)] = tickPeak;
    pos = (pos + 1) % DriveFeedForward::kTicks;
    count = std::min (count + 1, DriveFeedForward::kTicks);
    tickPeak = 0.0f;
}

float PlrMeter::getPlrDb() const noexcept FLUB_NONBLOCKING
{
    const float raw = loudness.getLufs();
    const double residual = std::exp (logPole * static_cast<double> (samples));
    const float lufs = residual < 0.999 ? raw - static_cast<float> (10.0 * std::log10 (1.0 - residual)) : raw;
    if (count < 100 || ! (lufs > -50.0f))
        return kNoReading;
    float peak = 0.0f;
    for (int i = 0; i < count; ++i)
        peak = std::max (peak, peaks[static_cast<size_t> (i)]);
    return gainToDb (peak) - lufs;
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
    tickSeconds = static_cast<float> (dt);
    grFastCoeff = static_cast<float> (std::exp (-dt / kGrAverageSeconds));
    programAttack = static_cast<float> (std::exp (-dt / kProgramAttackSeconds));
    programRelease = static_cast<float> (std::exp (-dt / kProgramReleaseSeconds));
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
    measuredRunning = false;
    grFastDb = driveDb = harmonicsDb = tonalDb = 0.0f;
    harmonicsScale = tonalScale = 1.0f;
    grLoop = residualLoop = plrLoop = harmonicsLoop = tonalLoop = {};
    programDepthDb = sagLagDb = 0.0f;
    pending = {};
    pendingProbes = {};
    restartHold = false;
    restartHoldLeft = 0.0f;
}

void SafetyGovernor::restart() noexcept FLUB_NONBLOCKING
{
    if (strength == ProtectionStrength::Off)
    {
        reset();
        return;
    }
    const Memory m = getMemory();
    reset();
    restoreMemory (m);
}

SafetyGovernor::Memory SafetyGovernor::getMemory() const noexcept FLUB_NONBLOCKING
{
    Memory m;
    m.strength = strength;
    m.scale = scale;
    m.harmonicsScale = harmonicsScale;
    m.tonalScale = tonalScale;
    m.reason = reason;
    if (measuredRunning)
    {
        const Loop* loops[] = { &grLoop, &residualLoop, &plrLoop, &harmonicsLoop, &tonalLoop };
        for (size_t k = 0; k < m.probes.size(); ++k)
            m.probes[k] = loops[k]->probe();
    }
    else if (restartHold)
    {
        m.probes = pendingProbes; // still holding after an earlier restore
    }
    m.valid = true;
    return m;
}

void SafetyGovernor::restoreMemory (const Memory& memory) noexcept FLUB_NONBLOCKING
{
    // At the strength it was learned at: now, so the next block already runs
    // on the restored scales. Otherwise at the first tick (the host may set
    // the strength in between).
    pending = memory;
    if (memory.strength == strength)
        applyPendingMemory();
}

void SafetyGovernor::applyPendingMemory() noexcept FLUB_NONBLOCKING
{
    if (! pending.valid)
        return;
    const Memory m = pending;
    pending.valid = false;
    if (m.strength != strength) // learned at another strength: not this loop's state
        return;
    const auto unit = [] (float v) { return std::isfinite (v) ? std::clamp (v, 0.0f, 1.0f) : 1.0f; };
    if (unit (m.scale) >= 1.0f && unit (m.harmonicsScale) >= 1.0f && unit (m.tonalScale) >= 1.0f)
        return; // nothing learned: start as after reset() (a render's priming, a fresh chain)
    scale = std::max (minScale, unit (m.scale));
    if (strength != ProtectionStrength::Off)
    {
        harmonicsScale = unit (m.harmonicsScale);
        tonalScale = unit (m.tonalScale);
        pendingProbes = m.probes;
        measuredRunning = false;
        restartHold = true;
        restartHoldLeft = kRestartHoldSeconds;
    }
    const bool governing = scale < 1.0f || harmonicsScale < 1.0f || tonalScale < 1.0f;
    state = governing ? State::Holding : State::Idle;
    reason = governing ? m.reason : 0u;
}

void SafetyGovernor::setStrength (ProtectionStrength s) noexcept FLUB_NONBLOCKING
{
    minScale = s == ProtectionStrength::Strict ? kStrictMinScale : kMinScale;
    scale = std::max (scale, minScale);
    if (s != strength)
    {
        // The measured loop (re)starts from the scales as they are; Off has
        // no harmonics or tonal scale.
        measuredRunning = false;
        if (s == ProtectionStrength::Off)
            harmonicsScale = tonalScale = 1.0f;
    }
    strength = s;
}

SafetyGovernor::Budgets SafetyGovernor::budgetsFor (ProtectionStrength s, bool music) noexcept
{
    // Provisional values (docs/11 E06 Phase 3), until the E60 listening
    // panel: Music about 5 dB stricter than Gaming on the audible residual,
    // a dynamics budget in Music only (games need their quiet cues loud),
    // Strict 6 dB stricter on the residual, 2 dB on the limiter, 2 dB more
    // PLR.
    Budgets b;
    b.grDb = kGrBudgetDb;
    b.residualDb = music ? -35.0f : -30.0f;
    b.plrDb = music ? 8.0f : 0.0f;
    // Tonal balance (docs/11 E07): Gaming's presence budget is its Done-when
    // (2-5 kHz lift minus 200 Hz - 1 kHz lift <= +2 dB); Music may be a
    // little brighter, air more than the bands where harshness lives.
    b.presenceDb = music ? 3.0f : 2.0f;
    b.harshDb = music ? 3.0f : 2.0f;
    b.airDb = music ? 4.0f : 3.0f;
    if (s == ProtectionStrength::Strict)
    {
        b.grDb += 2.0f;
        b.residualDb -= 6.0f;
        b.plrDb = music ? 10.0f : 0.0f;
        b.presenceDb -= 1.5f;
        b.harshDb -= 1.5f;
        b.airDb -= 1.5f;
    }
    return b;
}

void SafetyGovernor::update (float limiterGrDb, float distortionDb, int numSamples) noexcept FLUB_NONBLOCKING
{
    pendingSamples += numSamples;
    while (pendingSamples >= tickSamples)
    {
        pendingSamples -= tickSamples;
        applyPendingMemory();
        tick (limiterGrDb, distortionDb);
    }
}

void SafetyGovernor::updateMeasured (const Readings& r, int numSamples) noexcept FLUB_NONBLOCKING
{
    pendingSamples += numSamples;
    while (pendingSamples >= tickSamples)
    {
        pendingSamples -= tickSamples;
        applyPendingMemory();
        if (strength == ProtectionStrength::Off)
            tick (r.limiterGrDb, r.distortionDb);
        else
            measuredTick (r);
    }
}

float SafetyGovernor::Loop::step (float e, float dt, float offsetDb, float gain, float holdBandDb) noexcept FLUB_NONBLOCKING
{
    // A reading far from its set point (or none, -160 dB) is taken as 12 dB
    // off, so a loop that was idle does not kick when a reading appears.
    constexpr float kMaxErrorDb = 12.0f;
    lastError = std::clamp (e, -kMaxErrorDb, kMaxErrorDb);
    // Over the set point: PI. Within holdBandDb (kHoldBandDb) under it: hold. Further
    // under: the integral recovers (no proportional term, so the scale does
    // not jump up when the reading drops). The band keeps the loop off the
    // edge of stages that switch in at a threshold (the maximizer's glue on
    // a steady bass tone read 13 dB more residual per dB of drive), where a
    // plain PI would hunt.
    // Over the set point the integral runs on the error plus kApproachDb, so
    // it reaches the set point in finite time instead of creeping up to it.
    const float cap = holdLeft > 0.0f ? ceiling : 0.0f;
    if (verifyLeft > 0.0f)
    {
        // Just stepped to the probe cap: wait for the readings to show it.
        verifyLeft -= dt;
        integral = std::clamp (integral, -120.0f, cap - offsetDb);
        return std::min (cap, offsetDb + integral);
    }
    float p = 0.0f;
    if (lastError > 0.0f)
    {
        integral -= gain * kIntGain * (lastError + kApproachDb) * dt;
        p = gain * kPropGain * lastError;
    }
    else if (lastError < -holdBandDb)
    {
        integral -= gain * kIntGain * (lastError + holdBandDb) * dt;
    }
    integral = std::clamp (integral, -120.0f, cap - offsetDb);
    return std::min (cap, offsetDb + integral - p);
}

void SafetyGovernor::Loop::track (float candidate, float applied, float dt) noexcept FLUB_NONBLOCKING
{
    if (candidate < applied)
        integral += applied - candidate;
    // Probe memory: a stage that switches in at a threshold (the glue on a
    // steady bass tone) reads far under the budget just below it, so plain
    // recovery would climb back into it every few seconds.
    const bool over = lastError > 0.0f;
    if (over && ! wasOver)
    {
        holdTime = std::abs (lastApplied - lastOnset) <= 1.0f ? std::min (2.0f * holdTime, kMaxProbeHoldSeconds) : kProbeHoldSeconds;
        lastOnset = lastApplied;
        ceiling = lastApplied - kProbeMarginDb;
        holdLeft = holdTime;
        // Step to the cap (from where this tick's candidate is, if lower) and
        // wait for the readings - unless the back-off starts far over (the
        // programme changed, not a probe that went a little too far).
        integral += std::min (0.0f, ceiling - candidate);
        verifyLeft = lastError < kProbeErrorDb ? kVerifySeconds : 0.0f;
    }
    else if (! over && holdLeft > 0.0f)
    {
        holdLeft -= dt;
    }
    wasOver = over;
    lastApplied = applied;
}

void SafetyGovernor::measuredTick (const Readings& r) noexcept FLUB_NONBLOCKING
{
    const float dt = tickSeconds;
    // The meters' ~3 s averages, as in tick().
    const float a = tickAverage;
    avgGrDb = a * avgGrDb + (1.0f - a) * r.limiterGrDb;
    avgDistortionDb = powerToDb (a * dbToPower (avgDistortionDb) + (1.0f - a) * dbToPower (r.distortionDb));
    grFastDb = grFastCoeff * grFastDb + (1.0f - grFastCoeff) * r.limiterGrDb;

    const auto b = budgetsFor (strength, musicMode);
    const float ffDb = gainToDb (std::clamp (r.feedForwardScale, 1.0e-3f, 1.0f));
    const float floorDb = minScale > 0.0f ? gainToDb (minScale) : kFloorDb;

    // Release tie: the limiter's programme envelope, modelled on the
    // window's deepest GR (one-pole in the linear gain, as in the limiter).
    const float windowDepthDb = std::max (0.0f, -r.limiterGrDb);
    float roomDb = 0.0f;
    {
        const float p = dbToGain (-programDepthDb), w = dbToGain (-windowDepthDb);
        const float depthBefore = programDepthDb;
        programDepthDb = std::max (0.0f, -gainToDb (w + (w < p ? programAttack : programRelease) * (p - w)));
        // The attack's room: how far the window's GR is under the envelope
        // while the envelope is still attacking (deepening by more than
        // kAttackingDbPerTick); in steady limiting the window's deepest peak
        // always reads a little under it, and that is no room.
        if (programDepthDb - depthBefore > kAttackingDbPerTick)
            roomDb = std::max (0.0f, windowDepthDb - programDepthDb);
        // The dip releases as the envelope does, at 8.686 (10^(L/20) - 1) / tau dB/s
        // at a dip of L dB, and cannot exceed the reduction the envelope holds.
        constexpr float kDbPerNeper = 8.685889638f;
        sagLagDb -= kDbPerNeper * (dbToGain (sagLagDb) - 1.0f) * dt / kProgramReleaseSeconds;
        sagLagDb = std::clamp (sagLagDb, 0.0f, programDepthDb);
    }

    if (! measuredRunning && restartHold)
    {
        // Learned state taken over: hold the scales until the readings the
        // loop starts from are back (see kRestartHoldSeconds); the time only
        // counts once some are (silence holds).
        const bool plrReady = b.plrDb <= 0.0f || (r.plrDb < PlrMeter::kNoReading && r.inputPlrDb < PlrMeter::kNoReading);
        if (! (r.feedForwardValid && plrReady) && restartHoldLeft > 0.0f)
        {
            if (r.feedForwardValid || r.plrDb < PlrMeter::kNoReading)
                restartHoldLeft -= dt;
            return;
        }
    }
    if (! measuredRunning)
    {
        // Start from the scales as they are (bumpless).
        driveDb = scale > 0.0f ? std::max (kFloorDb, gainToDb (scale)) : kFloorDb;
        harmonicsDb = harmonicsScale > 0.0f ? std::max (kFloorDb, gainToDb (harmonicsScale)) : kFloorDb;
        tonalDb = tonalScale > 0.0f ? std::max (kFloorDb, gainToDb (tonalScale)) : kFloorDb;
        grLoop = residualLoop = plrLoop = harmonicsLoop = tonalLoop = {};
        grLoop.integral = driveDb - ffDb;
        residualLoop.integral = plrLoop.integral = driveDb;
        harmonicsLoop.integral = harmonicsDb;
        tonalLoop.integral = tonalDb;
        grLoop.lastApplied = residualLoop.lastApplied = plrLoop.lastApplied = driveDb;
        harmonicsLoop.lastApplied = harmonicsDb;
        tonalLoop.lastApplied = tonalDb;
        if (restartHold)
        {
            // ... with the probe memory it had.
            Loop* loops[] = { &grLoop, &residualLoop, &plrLoop, &harmonicsLoop, &tonalLoop };
            for (size_t k = 0; k < pendingProbes.size(); ++k)
                loops[k]->setProbe (pendingProbes[k]);
            restartHold = false;
        }
        measuredRunning = true;
    }

    // Drive scale: the lowest of three loops, each a PI on its error in dB
    // (> 0 = over its set point, kSetPointMarginDb inside the budget); the
    // limiter loop is a trim around the feed-forward.
    const float eGr = (b.grDb + kSetPointMarginDb) - grFastDb;
    const float eResidual = r.driveResidualDb - (b.residualDb - kSetPointMarginDb);
    // The dynamics budget is what the chain may take away: programme that
    // arrives under the budget may lose kPlrAllowanceDb more.
    const bool plrBudget = b.plrDb > 0.0f && r.plrDb < PlrMeter::kNoReading && r.inputPlrDb < PlrMeter::kNoReading;
    // (The set point is kPlrMarginDb over the budget, but never above the
    // input's PLR less the allowance: a steady tone's PLR cannot be raised
    // by any drive.)
    const float plrSetPoint = plrBudget ? std::min (b.plrDb + kPlrMarginDb, r.inputPlrDb - kPlrAllowanceDb) : 0.0f;
    const float ePlr = plrBudget ? plrSetPoint - r.plrDb : -12.0f;
    float uGr = grLoop.step (eGr, dt, ffDb, kTrimGain);
    // The feed-forward is an upper bound on the limiter's GR (stages ahead
    // of it - the glue, the LF-first limiter - take peaks it does not
    // model): while the limiter is all but idle it holds nothing down.
    if (grFastDb > -kLimiterIdleDb)
        uGr = std::max (uGr, driveDb + kRiseDbPerSec * dt);
    const float uResidual = residualLoop.step (eResidual, dt, 0.0f);
    const float uPlr = plrLoop.step (ePlr, dt, 0.0f);
    const float target = std::max (floorDb, std::min ({ uGr, uResidual, uPlr }));
    const float before = driveDb;
    // Fall limit. Release tie (see kSagAllowanceDb): while the output is
    // limiter-bound and the limiter's programme envelope holds more than the
    // allowance, the drive may fall (in dB of drive) by what the envelope's
    // attack has not yet taken plus what keeps the modelled dip under the
    // allowance - unless the audible residual is far over its set point,
    // where the faster of the two applies.
    float fallFloor = driveDb - kFallDbPerSec * dt;
    const bool limiterBound = uGr <= std::min (uResidual, uPlr) || programDepthDb > -(b.grDb + kSetPointMarginDb);
    const bool tie = r.driveAtFullScaleDb > 0.0f && programDepthDb + roomDb > kSagAllowanceDb && limiterBound;
    if (tie)
    {
        const float driveNow = std::min (r.driveMaxDb, r.driveAtFullScaleDb * dbToGain (driveDb));
        const float allowed = roomDb + std::max (0.0f, kSagAllowanceDb - sagLagDb);
        const float tieFloor = driveNow - allowed > 0.0f ? gainToDb ((driveNow - allowed) / r.driveAtFullScaleDb) : kFloorDb;
        fallFloor = eResidual > kProbeErrorDb ? std::min (fallFloor, tieFloor) : tieFloor;
    }
    driveDb = std::clamp (target, std::min (driveDb, fallFloor), driveDb + kRiseDbPerSec * dt);
    if (tie && driveDb < before)
    {
        // What the drive fell beyond the attack's room is a dip until released.
        const float fellDb = std::min (r.driveMaxDb, r.driveAtFullScaleDb * dbToGain (before)) - std::min (r.driveMaxDb, r.driveAtFullScaleDb * dbToGain (driveDb));
        sagLagDb = std::min (programDepthDb, sagLagDb + std::max (0.0f, fellDb - roomDb));
    }
    grLoop.track (uGr, driveDb, dt);
    residualLoop.track (uResidual, driveDb, dt);
    plrLoop.track (uPlr, driveDb, dt);
    scale = driveDb <= kFloorDb ? 0.0f : std::max (minScale, dbToGain (driveDb));

    // Harmonics scale: its own loop on the bass harmonics' audible residual.
    // Small Speaker Mode's harmonics replace the fundamental: Normal leaves them.
    const bool harmonicsGoverned = r.harmonicsResidualDb > kMinusInfDb
                                   && ! (strength == ProtectionStrength::Normal && harmonicsReplace);
    const float eHarmonics = harmonicsGoverned ? r.harmonicsResidualDb - (b.residualDb - kSetPointMarginDb) : -12.0f;
    const float uHarmonics = std::max (kFloorDb, harmonicsLoop.step (eHarmonics, dt, 0.0f));
    const float harmonicsBefore = harmonicsDb;
    harmonicsDb = std::clamp (uHarmonics, harmonicsDb - kHarmonicsFallDbPerSec * dt, harmonicsDb + kRiseDbPerSec * dt);
    harmonicsLoop.track (uHarmonics, harmonicsDb, dt);
    harmonicsScale = harmonicsDb <= kFloorDb ? 0.0f : dbToGain (harmonicsDb);

    // Tonal-balance rule (docs/11 E07): the highest band's lift over its
    // set point. The meter's readings hold through pauses; before its first
    // reading (0.5 s of programme) the loop sees nothing over budget.
    float eTonal = -12.0f;
    for (const auto& [lift, budget] : { std::pair { r.presenceLiftDb, b.presenceDb }, std::pair { r.harshLiftDb, b.harshDb },
                                       std::pair { r.airLiftDb, b.airDb } })
        if (lift > kMinusInfDb)
            eTonal = std::max (eTonal, lift - (budget - kTonalMarginDb));
    const float uTonal = std::max (kFloorDb, tonalLoop.step (eTonal, dt, 0.0f, kTonalGain, kTonalHoldBandDb));
    const float tonalBefore = tonalDb;
    tonalDb = std::clamp (uTonal, tonalDb - kTonalFallDbPerSec * dt, tonalDb + kRiseDbPerSec * dt);
    tonalLoop.track (uTonal, tonalDb, dt);
    tonalScale = tonalDb <= kFloorDb ? 0.0f : dbToGain (tonalDb);

    // State and reasons, as the Off loop reports them.
    constexpr float kStep = 1.0e-4f;
    const bool driveFell = driveDb < before - kStep, harmonicsFell = harmonicsDb < harmonicsBefore - kStep;
    const bool tonalFell = tonalDb < tonalBefore - kStep;
    const bool driveOver = eGr > 0.0f || eResidual > 0.0f || (plrBudget && ePlr > 0.0f);
    const bool harmonicsOver = harmonicsGoverned && eHarmonics > 0.0f;
    const bool tonalOver = eTonal > 0.0f;
    const bool atFloor = driveDb <= floorDb + kStep, harmonicsAtFloor = harmonicsDb <= kFloorDb + kStep;
    const bool tonalAtFloor = tonalDb <= kFloorDb + kStep;
    if (driveFell || harmonicsFell || tonalFell || (driveOver && atFloor) || (harmonicsOver && harmonicsAtFloor)
        || (tonalOver && tonalAtFloor))
    {
        reason |= (eGr > 0.0f ? kReasonLimiter : 0u) | (eResidual > 0.0f ? kReasonDistortion : 0u)
                  | (plrBudget && ePlr > 0.0f ? kReasonDynamics : 0u) | (harmonicsOver ? kReasonHarmonics : 0u)
                  | (tonalOver ? kReasonTonal : 0u);
        state = State::BackingOff;
    }
    else if (driveDb >= -kStep && harmonicsDb >= -kStep && tonalDb >= -kStep)
        state = State::Idle;
    else if (driveDb > before + kStep || harmonicsDb > harmonicsBefore + kStep || tonalDb > tonalBefore + kStep)
        state = State::Recovering;
    else
        state = State::Holding;
    if (state == State::Idle)
        reason = 0;
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
    follower.setSlowTimeMs (kMeasureMs);
    follower.prepare (sampleRate, numChannels);
    settleSamples = static_cast<std::int64_t> (kRestartSettleSeconds * sampleRate);
    gapSamples = static_cast<std::int64_t> (kGapSeconds * sampleRate);
    reset();
}

void AutoLevel::reset() noexcept
{
    follower.reset();
    gainDb = 0.0f;
    lastLinear = 1.0f;
    recoveryLeft = 0.0;
    restartsSeen = 0;
    pauseSamples = 0;
    direction = 0;
    frozen = settling = false;
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
    {
        follower.process (block); // measured BEFORE our gain: open-loop, unconditionally stable
        pauseSamples = follower.hasProgramme() ? 0 : pauseSamples + block.numSamples;
        // A new level measured for less than the settle time is not carried
        // over a pause (a track that ended just after it was taken as the
        // level): the next programme starts the measure afresh.
        if (settling && pauseSamples >= gapSamples && follower.measuredSamples() > 0)
            follower.restart();
    }
    const double dt = block.numSamples / sr;
    if (follower.restartCount() != restartsSeen)
    {
        // The measure restarted on a new level: its first reading is the mean
        // of a beat or a phrase, so the gain waits for kRestartSettleSeconds
        // of it instead of following that and coming back.
        restartsSeen = follower.restartCount();
        settling = true;
        direction = 0;
    }
    if (settling && follower.measuredSamples() >= settleSamples)
        settling = false;

    if (enabled)
    {
        if (! measure)
        {
            // Hidden from the loop (docs/11 E10): the gain holds, as in a pause.
        }
        else if (follower.isActive() && settling)
        {
            frozen = true; // the gain holds; it catches up at the recovery rate
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
            float desired = std::clamp (target - follower.getLufs(), kMinGainDb, kMaxGainDb);
            if ((direction < 0 && desired > gainDb && desired - gainDb <= kReversalDb) || (direction > 0 && desired < gainDb && gainDb - desired <= kReversalDb))
                desired = gainDb; // a small reversal: hold
            const float before = gainDb;
            gainDb = slew (gainDb, desired, up, kDownDbPerSec, dt);
            direction = gainDb > before ? 1 : gainDb < before ? -1 : direction;
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
