// Flubsound Pro - protection & loudness control loops (all RT-safe).
//
// SafetyGovernor (THD / over-processing protection):
//   Inputs per block: maximizer limiter GR (dB), clipper energy ratio (dB).
//   Budget: limiter GR averaged over ~3 s must stay above -6 dB, clip energy
//   ratio above -30 dB (~3 % "THD" contribution). When over budget, scale
//   falls at 15 %/s (min 0.3); when under budget minus 1.5 dB hysteresis it
//   recovers at 3 %/s. The scale multiplies every "governed" macro amount.
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

class SafetyGovernor
{
public:
    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void update (float limiterGrDb, float clipEnergyRatioDb, int numSamples) noexcept;
    float getScale() const noexcept { return scale; }

private:
    double sr = 48000.0;
    float avgGrDb = 0.0f, avgClipDb = -160.0f, scale = 1.0f;
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
