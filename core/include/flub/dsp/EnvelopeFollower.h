// Flubsound Pro - envelope detection building blocks.
#pragma once

#include "flub/common/Math.h"

#include <cmath>

namespace flub
{
/** Branching peak follower on a non-negative input (|x| or a power).
    Attack when the input rises above the envelope, release otherwise. */
class EnvelopeFollower
{
public:
    void prepare (double sampleRate, float attackMs, float releaseMs) noexcept
    {
        sr = sampleRate;
        setTimes (attackMs, releaseMs);
        env = 0.0f;
    }

    void setTimes (float attackMs, float releaseMs) noexcept
    {
        attack = onePoleCoeff (attackMs, sr);
        release = onePoleCoeff (releaseMs, sr);
    }

    void reset (float value = 0.0f) noexcept { env = value; }

    float process (float x) noexcept
    {
        const float c = x > env ? attack : release;
        env = x + c * (env - x);
        return env;
    }

    float get() const noexcept { return env; }

private:
    double sr = 48000.0;
    float attack = 0.0f, release = 0.0f, env = 0.0f;
};

/** Smoothing in the gain (dB) domain with separate attack/release, the
    "decoupled, branching" topology from Giannoulis, Massberg & Reiss (2012).

    Attack/release follow the usual dynamics convention: "attack" is the
    response to the detector level RISING, "release" to it falling.
      * Compressors / upward compressors (level up -> gain down): a falling
        gain uses attack, a rising gain uses release (expanderMode = false).
      * Expanders / gates / "boost above threshold" (level up -> gain up):
        a rising gain uses attack (expanderMode = true). */
class GainSmoother
{
public:
    void prepare (double sampleRate, float attackMs, float releaseMs, bool isExpander = false) noexcept
    {
        sr = sampleRate;
        expanderMode = isExpander;
        setTimes (attackMs, releaseMs);
        state = 0.0f;
    }

    void setTimes (float attackMs, float releaseMs) noexcept
    {
        attack = onePoleCoeff (attackMs, sr);
        release = onePoleCoeff (releaseMs, sr);
    }

    void setExpanderMode (bool isExpander) noexcept { expanderMode = isExpander; }

    void reset (float db = 0.0f) noexcept { state = db; }

    float process (float targetDb) noexcept
    {
        const bool gainFalling = targetDb < state;
        const bool levelRising = expanderMode ? ! gainFalling : gainFalling;
        const float c = levelRising ? attack : release;
        state = targetDb + c * (state - targetDb);
        return state;
    }

    float get() const noexcept { return state; }

private:
    double sr = 48000.0;
    float attack = 0.0f, release = 0.0f, state = 0.0f;
    bool expanderMode = false;
};

/** Exponentially weighted mean square (RMS^2) follower. */
class MeanSquareFollower
{
public:
    void prepare (double sampleRate, float timeMs) noexcept
    {
        coeff = onePoleCoeff (timeMs, sampleRate);
        ms = 0.0;
    }

    void reset() noexcept { ms = 0.0; }

    double process (double xSquared) noexcept
    {
        ms = xSquared + coeff * (ms - xSquared);
        return ms;
    }

    double get() const noexcept { return ms; }

private:
    double ms = 0.0;
    float coeff = 0.0f;
};
} // namespace flub
