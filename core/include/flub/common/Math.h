// Flubsound Pro - small, allocation-free math helpers shared by every DSP module.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace flub
{
inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 2.0 * kPi;
inline constexpr float kPiF = static_cast<float> (kPi);

/** Silence floor used when converting to decibels (-160 dB ~ below 32-bit float noise). */
inline constexpr float kMinusInfDb = -160.0f;

inline float dbToGain (float db) noexcept
{
    return db <= kMinusInfDb ? 0.0f : std::pow (10.0f, db * 0.05f);
}

inline float gainToDb (float gain, float floorDb = kMinusInfDb) noexcept
{
    return gain > 0.0f ? std::max (floorDb, 20.0f * std::log10 (gain)) : floorDb;
}

/** Power (mean-square) to dB. */
inline float powerToDb (float power, float floorDb = kMinusInfDb) noexcept
{
    return power > 0.0f ? std::max (floorDb, 10.0f * std::log10 (power)) : floorDb;
}

template <typename T>
constexpr T lerp (T a, T b, T t) noexcept
{
    return a + (b - a) * t;
}

/** Smoothstep in [edge0, edge1] -> [0, 1]; used by macro curves. */
inline float smoothstep (float edge0, float edge1, float x) noexcept
{
    if (edge1 <= edge0)
        return x >= edge1 ? 1.0f : 0.0f;
    const float t = std::clamp ((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/** One-pole coefficient for a time constant in milliseconds (63% rise time).
    y += (1 - a) * (x - y). Returns 0 (instant) for non-positive times. */
inline float onePoleCoeff (float timeMs, double sampleRate) noexcept
{
    if (timeMs <= 0.0f || sampleRate <= 0.0)
        return 0.0f;
    return static_cast<float> (std::exp (-1.0 / (static_cast<double> (timeMs) * 0.001 * sampleRate)));
}

inline int msToSamples (float ms, double sampleRate) noexcept
{
    return static_cast<int> (std::lround (static_cast<double> (ms) * 0.001 * sampleRate));
}

inline bool isPowerOfTwo (int x) noexcept { return x > 0 && (x & (x - 1)) == 0; }

inline int nextPowerOfTwo (int x) noexcept
{
    int p = 1;
    while (p < x)
        p <<= 1;
    return p;
}

/** Cheap RT-safe PRNG (xorshift32) for dither / decorrelation. */
class FastRandom
{
public:
    explicit FastRandom (uint32_t seed = 0x9E3779B9u) noexcept : state (seed ? seed : 1u) {}

    uint32_t nextU32() noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    /** Uniform in [-1, 1). */
    float nextBipolar() noexcept
    {
        return static_cast<float> (nextU32() >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }

private:
    uint32_t state;
};
} // namespace flub
