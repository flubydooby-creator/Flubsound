// Flubsound Pro - protection & loudness control loops (all RT-safe).
//
// DistortionMonitor (measured THD+N of the chain's nonlinear stages):
//   Per block it power-sums the latest THD+N readings (25 ms windows) of the
//   stages in series (the saturator and the maximizer's soft clipper, each
//   measured around its own curve, see flub/dsp/DistortionEstimator.h); a
//   stage that is fully bypassed feeds -160 dB. A power-domain one-pole with tau = 300 ms
//   smooths the block value for the meters (MeterBus::distortionDb).
//   The intentional harmonic generators (the bass engine's harmonics and the
//   clarity air exciter) are measured the same way, but their readings are
//   the share of the harmonics they add on purpose: updateHarmonics() sums
//   and smooths them separately, and they are neither in the THD+N above nor
//   a SafetyGovernor input (docs/03-dsp-design.md §14.5 has the numbers).
//
// SafetyGovernor (THD / over-processing protection):
//   Inputs per block: maximizer limiter GR (dB) and distortion (dB): the
//   measured THD+N of the saturator power-summed with the clipper's share,
//   which is its measured THD+N floored at its clip energy ratio over the
//   same 25 ms window (the former proxy, which reads higher on a steady
//   tone), so the governor does not act later on clipping than it did on
//   the proxy alone. Budget: limiter GR averaged over ~3 s must stay above
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

namespace flub
{
class GatedLoudness
{
public:
    void prepare (double sampleRate, int numChannels)
    {
        channels = numChannels;
        momentary.prepare (sampleRate, numChannels, 100.0f);
        slow.prepare (sampleRate, numChannels, 3000.0f);
        reset();
    }

    void reset() noexcept
    {
        momentary.reset();
        slow.reset();
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
        gateOpen = ! silent && m > -50.0f && (! slow.isActive() || m > slow.getLufs() - 20.0f);
        if (gateOpen)
            slow.process (block);
    }

    /** Gated slow loudness (LUFS). */
    float getLufs() const noexcept { return slow.getLufs(); }
    /** True while programme is present and the slow measure is valid. */
    bool isActive() const noexcept { return gateOpen && slow.isActive(); }

private:
    LoudnessFollower momentary, slow;
    int channels = 2;
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
