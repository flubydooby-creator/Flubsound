// Flubsound Pro - parameter smoothing (zipper-noise prevention).
#pragma once

#include <algorithm>
#include <cmath>

namespace flub
{
/** Linear ramp to a target over a fixed time. Deterministic end point, so it is
    used for gains and crossfades where "reaches exactly 1.0" matters. */
class LinearSmoothedValue
{
public:
    void reset (double sampleRate, float rampMs, float initial) noexcept
    {
        rampSamples = std::max (1, static_cast<int> (sampleRate * rampMs * 0.001));
        current = target = initial;
        remaining = 0;
    }

    void setTarget (float newTarget) noexcept
    {
        if (newTarget == target)
            return;
        target = newTarget;
        remaining = rampSamples;
        step = (target - current) / static_cast<float> (rampSamples);
    }

    void setImmediate (float v) noexcept
    {
        current = target = v;
        remaining = 0;
    }

    float next() noexcept
    {
        if (remaining > 0)
        {
            current = (--remaining == 0) ? target : current + step;
        }
        return current;
    }

    /** Advance n samples and return the value at the end (for control-rate updates). */
    float skip (int n) noexcept
    {
        if (remaining <= 0)
            return current;
        if (n >= remaining)
        {
            remaining = 0;
            current = target;
        }
        else
        {
            remaining -= n;
            current += step * static_cast<float> (n);
        }
        return current;
    }

    bool isSmoothing() const noexcept { return remaining > 0; }
    float getCurrent() const noexcept { return current; }
    float getTarget() const noexcept { return target; }

private:
    float current = 0.0f, target = 0.0f, step = 0.0f;
    int remaining = 0, rampSamples = 1;
};

/** Exponential (one-pole) smoother; natural for frequencies (smooth in log domain)
    and for values where a soft landing is preferable to a fixed ramp. */
class OnePoleSmoother
{
public:
    void reset (double sampleRate, float timeMs, float initial) noexcept
    {
        coeff = timeMs <= 0.0f ? 0.0f : static_cast<float> (std::exp (-1.0 / (timeMs * 0.001 * sampleRate)));
        current = target = initial;
    }

    void setTarget (float t) noexcept { target = t; }
    void setImmediate (float v) noexcept { current = target = v; }

    float next() noexcept
    {
        const float previous = current;
        current = target + coeff * (current - target);
        snapIfSettled (previous);
        return current;
    }

    /** Advance n samples at once (exact for a one-pole). */
    float skip (int n) noexcept
    {
        const float previous = current;
        current = target + std::pow (coeff, static_cast<float> (n)) * (current - target);
        snapIfSettled (previous);
        return current;
    }

    bool isSmoothing() const noexcept { return current != target; }
    float getCurrent() const noexcept { return current; }
    float getTarget() const noexcept { return target; }

private:
    /** In float the recursion stalls ~0.5 ulp / (1 - coeff) short of a non-zero
        target (it stops moving before any relative threshold is met), so a
        step that no longer changes the value also counts as settled. */
    void snapIfSettled (float previous) noexcept
    {
        if (current == previous || std::abs (current - target) < 1.0e-6f * (1.0f + std::abs (target)))
            current = target;
    }

    float coeff = 0.0f, current = 0.0f, target = 0.0f;
};
} // namespace flub
