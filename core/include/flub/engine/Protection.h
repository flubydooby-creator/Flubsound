// Flubsound Pro - protection & loudness control loops (all RT-safe).
//
// SafetyGovernor (THD / over-processing protection):
//   Inputs per block: maximizer limiter GR (dB), clipper energy ratio (dB).
//   Budget: limiter GR averaged over ~3 s must stay above -6 dB, clip energy
//   ratio above -30 dB (~3 % "THD" contribution). When over budget, scale
//   falls at 15 %/s (min 0.3); when under budget minus 1.5 dB hysteresis it
//   recovers at 3 %/s. The scale multiplies every "governed" macro amount.
//
// AutoLevel (LUFS input levelling / loudness normalisation):
//   LoudnessFollower (3 s) on the input; gain = target - measured, limited to
//   +-12 dB, slew-limited to +1 dB/s (up) and -4 dB/s (down), frozen while the
//   input is silent (< -60 LUFS) so pauses/track gaps are not "pumped up".
//   Applied as a per-block linear ramp (click-free).
//
// AutoDrive (maximizer loudness target):
//   Measures post-chain short-term loudness; the maximizer drive actually
//   used is min(driveParam, driveParam + (target - measured)) with the same
//   slow slew; it only ever REDUCES drive (never exceeds the user/macro drive)
//   so it can stop over-limiting but can never make things louder than asked.
//
// LoudnessMatch (fair A/B): two LoudnessFollowers (dry, wet); gainDb for the
//   dry path = wet - dry, clamped +-12 dB, slewed 3 dB/s.
#pragma once

#include "flub/analysis/LoudnessFollower.h"

namespace flub
{
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
    LoudnessFollower follower;
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
        apply at the next block. */
    float update (const AudioBlock& output, float targetLufs, bool enabled) noexcept;
    float getReductionDb() const noexcept { return reductionDb; }

private:
    LoudnessFollower follower;
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
    LoudnessFollower dryF, wetF;
    double sr = 48000.0;
    float gainDb = 0.0f;
};
} // namespace flub
