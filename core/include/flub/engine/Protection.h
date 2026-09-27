// Flubsound Pro - protection & loudness control loops (all RT-safe).
//
// DistortionMonitor (measured THD+N of the chain's nonlinear stages):
//   Per block it power-sums the latest THD+N readings (windows of at least
//   25 ms) of the stages in series (the saturator and the maximizer's soft
//   clipper, each measured around its own curve, see
//   flub/dsp/DistortionEstimator.h); a stage that is fully bypassed feeds
//   -160 dB. A power-domain one-pole with tau = 300 ms smooths the block
//   value for the meters (MeterBus::distortionDb).
//   The intentional harmonic generators (the bass engine's harmonics and the
//   clarity air exciter) are measured the same way, but their readings are
//   the share of the harmonics they add on purpose: updateHarmonics() sums
//   and smooths them separately, and they are neither in the THD+N above nor
//   a SafetyGovernor input (docs/03-dsp-design.md §14.5 has the numbers).
//
// SafetyGovernor (THD / over-processing protection):
//   Inputs per block: maximizer limiter GR (dB; the deepest per fixed 10 ms
//   window, so it does not depend on the host block size) and distortion
//   (dB): the measured THD+N of the saturator power-summed with the
//   clipper's share, which is its measured THD+N floored at its clip energy
//   ratio over the same analysis window (the former proxy, which reads
//   higher on a steady tone), so the governor does not act later on
//   clipping than it did on the proxy alone. Budget: limiter GR averaged over ~3 s must stay above
//   -6 dB, distortion (power average over ~3 s) below -30 dB (~3.2 % RMS of
//   the output). When over budget, scale falls at 15 %/s (min 0.3); when
//   under budget minus 1.5 dB hysteresis it recovers at 3 %/s. The scale
//   multiplies every "governed" macro amount; base values are never touched.
//
// GatedLoudness (shared by the three loops below):
//   A 3 s K-weighted "slow" loudness that is only advanced while programme is
//   present. The gate is open when the block is not (near) digital silence
//   (RMS above -70 dBFS), a fast 100 ms follower reads above -50 LUFS, and it
//   is no more than 20 LU below the slow measure (the EBU R128 idea of
//   absolute + relative gating). Pauses, track gaps and fade-outs therefore
//   neither decay the measurement nor drive the control loops.
//   The slow measure is only fed while the gate is open, so the relative
//   gate alone could hold a programme 20 LU quieter than the last one out
//   forever. It therefore has a release: after 3 s of programme kept out by
//   the relative criterion alone (silence and the absolute gate pause the
//   count, an open gate clears it) the slow measure restarts and acquires
//   the new level.
//   The slow one-pole starts from zero after every reset and restart, so it
//   reads 10 log10(1 - exp(-t / 3 s)) dB low after t seconds of open gate
//   (8 dB at 0.5 s, 3 dB at 2 s). getLufs() adds that bias back: the reading
//   is the exponentially weighted mean over the open-gate time so far,
//   unbiased from the first block on.
//
// AutoLevel (LUFS input levelling / loudness normalisation):
//   GatedLoudness on the input; gain = target - measured, limited to +-12 dB,
//   slew-limited to +1 dB/s (up) and -4 dB/s (down), adapted only while the
//   gate is open. Applied as a per-block linear ramp (click-free).
//
// AutoDrive (maximizer loudness target):
//   GatedLoudness on the post-chain output; a slow integrating loop (0.5 LU
//   dead band, <= 2 dB/s) produces a drive REDUCTION in [-requested drive, 0]
//   dB (never past 0 dB of drive, so recovery is immediate). It can
//   stop over-limiting but can never make things louder than the user/macros
//   asked for.
//
// LoudnessMatch (fair A/B): two GatedLoudness measures (dry, wet); gainDb for
//   the dry path = wet - dry, clamped +-12 dB, slewed 3 dB/s.
#pragma once

#include "flub/analysis/LoudnessFollower.h"
#include "flub/common/Realtime.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace flub
{
class GatedLoudness
{
public:
    static constexpr float kSlowTimeMs = 3000.0f;
    /** Programme kept out by the relative gate alone for this long restarts
        the slow measure (see the header comment). */
    static constexpr double kRelativeReleaseSeconds = 3.0;

    void prepare (double sampleRate, int numChannels)
    {
        channels = numChannels;
        momentary.prepare (sampleRate, numChannels, 100.0f);
        slow.prepare (sampleRate, numChannels, kSlowTimeMs);
        slowPole = static_cast<double> (onePoleCoeff (kSlowTimeMs, sampleRate));
        releaseSamples = static_cast<std::int64_t> (kRelativeReleaseSeconds * sampleRate);
        reset();
    }

    void reset() noexcept
    {
        momentary.reset();
        restartSlow();
        gateOpen = false;
    }

    void process (const AudioBlock& block) noexcept
    {
        // Instant close on (near) digital silence: track gaps, paused players.
        double sumSquares = 0.0;
        const int nch = std::min (block.numChannels, channels);
        for (int c = 0; c < nch; ++c)
            for (int i = 0; i < block.numSamples; ++i)
                sumSquares += static_cast<double> (block.channel (c)[i]) * block.channel (c)[i];
        const bool silent = sumSquares <= 1.0e-7 * std::max (1, nch * block.numSamples); // < -70 dBFS RMS

        momentary.process (block);
        const float m = momentary.getLufs();
        const bool absoluteOpen = ! silent && m > -50.0f;
        const float s = getLufs();
        gateOpen = absoluteOpen && (! (s > -60.0f) || m > s - 20.0f);
        if (gateOpen)
        {
            relativeGatedSamples = 0;
            slow.process (block);
            openSamples += block.numSamples;
            if (biasDb > 0.0f)
            {
                // The one-pole started from zero has filled to 1 - pole^n of
                // its input level after n samples; divide that back out.
                const double residual = std::pow (slowPole, static_cast<double> (openSamples));
                biasDb = residual > 1.0e-7 ? static_cast<float> (-10.0 * std::log10 (1.0 - residual)) : 0.0f;
            }
        }
        else if (absoluteOpen)
        {
            // Programme is present but more than 20 LU below the slow
            // measure, which cannot move while it is kept out: after
            // kRelativeReleaseSeconds, restart the measure on the new level.
            relativeGatedSamples += block.numSamples;
            if (relativeGatedSamples >= releaseSamples)
                restartSlow();
        }
    }

    /** Gated slow loudness (LUFS), corrected for the cold start of its one-pole. */
    float getLufs() const noexcept
    {
        const float raw = slow.getLufs();
        return raw > kMinusInfDb ? raw + biasDb : raw;
    }
    /** True while programme is present and the slow measure is valid. */
    bool isActive() const noexcept { return gateOpen && getLufs() > -60.0f; }

private:
    void restartSlow() noexcept
    {
        slow.reset();
        openSamples = 0;
        relativeGatedSamples = 0;
        biasDb = 1.0f; // any value > 0: recomputed on the next open block
    }

    LoudnessFollower momentary, slow;
    int channels = 2;
    double slowPole = 0.0;
    std::int64_t releaseSamples = 144000, openSamples = 0, relativeGatedSamples = 0;
    float biasDb = 0.0f;
    bool gateOpen = false;
};

class DistortionMonitor
{
public:
    static constexpr float kMeterTauSeconds = 0.3f;

    void prepare (double sampleRate) noexcept;
    void reset() noexcept FLUB_NONBLOCKING;
    /** Combines this block's stage readings (dB; -160 = none) and returns the
        block's THD+N (power sum). Advances the meter smoothing by numSamples. */
    float update (float saturatorDb, float clipperDb, int numSamples) noexcept FLUB_NONBLOCKING;
    /** Power sum of two ratios in dB (-160 = none): stages in series. */
    static float combineDb (float aDb, float bDb) noexcept FLUB_NONBLOCKING;
    float getBlockDb() const noexcept { return blockDb; }
    /** Block THD+N smoothed in the power domain (tau = kMeterTauSeconds). */
    float getSmoothedDb() const noexcept { return smoothedDb; }

    /** The harmonic generators' readings this block (bass harmonics, air
        exciter; dB re their output, -160 = none): power-summed and smoothed
        like the THD+N, but kept apart from it (see the header comment).
        Returns the block's power sum. */
    float updateHarmonics (float bassDb, float airDb, int numSamples) noexcept FLUB_NONBLOCKING;
    float getHarmonicsBlockDb() const noexcept { return harmonicsBlockDb; }
    /** Block harmonics reading smoothed in the power domain (tau = kMeterTauSeconds). */
    float getSmoothedHarmonicsDb() const noexcept { return harmonicsSmoothedDb; }

private:
    /** Advances a power-domain one-pole (tau = kMeterTauSeconds) by numSamples. */
    float smooth (float& statePow, float blockPow, int numSamples) const noexcept FLUB_NONBLOCKING;

    double sr = 48000.0;
    float blockDb = -160.0f, smoothedDb = -160.0f, smoothedPow = 0.0f;
    float harmonicsBlockDb = -160.0f, harmonicsSmoothedDb = -160.0f, harmonicsSmoothedPow = 0.0f;
};

class SafetyGovernor
{
public:
    static constexpr float kGrBudgetDb = -6.0f;          // sustained limiting deeper than this = over-driven
    static constexpr float kDistortionBudgetDb = -30.0f; // THD+N above ~3.2 % RMS = audible distortion

    void prepare (double sampleRate) noexcept;
    void reset() noexcept FLUB_NONBLOCKING;
    /** distortionDb: the distortion of the nonlinear stages this block (see
        the header comment; ProcessingChain::process). */
    void update (float limiterGrDb, float distortionDb, int numSamples) noexcept FLUB_NONBLOCKING;
    float getScale() const noexcept { return scale; }
    /** The ~3 s power average of the distortion input (what the budget is compared with). */
    float getAverageDistortionDb() const noexcept { return avgDistortionDb; }

private:
    double sr = 48000.0;
    float avgGrDb = 0.0f, avgDistortionDb = -160.0f, scale = 1.0f;
};

class AutoLevel
{
public:
    void prepare (double sampleRate, int numChannels);
    void reset() noexcept;
    void setTargetLufs (float lufs) noexcept { target = lufs; }
    void setEnabled (bool on) noexcept { enabled = on; }

    /** Measures and applies the levelling gain in place. */
    void process (const AudioBlock& block) noexcept;
    float getGainDb() const noexcept { return gainDb; }

private:
    GatedLoudness follower;
    double sr = 48000.0;
    float target = -18.0f, gainDb = 0.0f, lastLinear = 1.0f;
    bool enabled = false;
};

class AutoDrive
{
public:
    void prepare (double sampleRate, int numChannels);
    void reset() noexcept;
    /** Feed the post-chain output; returns the drive reduction (dB <= 0) to
        apply at the next block. The reduction never goes below
        -requestedDriveDb: past that the drive is already 0 dB and further
        "reduction" would change nothing audible while delaying recovery. */
    float update (const AudioBlock& output, float targetLufs, bool enabled, float requestedDriveDb) noexcept;
    float getReductionDb() const noexcept { return reductionDb; }

private:
    GatedLoudness follower;
    double sr = 48000.0;
    float reductionDb = 0.0f;
};

class LoudnessMatch
{
public:
    void prepare (double sampleRate, int numChannels);
    void reset() noexcept;
    void measureDry (const AudioBlock& dry) noexcept;
    void measureWet (const AudioBlock& wet) noexcept;
    /** Gain (dB) to apply to the dry path so it matches the wet loudness. */
    float getDryGainDb (int numSamples) noexcept;

private:
    GatedLoudness dryF, wetF;
    double sr = 48000.0;
    float gainDb = 0.0f;
};
} // namespace flub
