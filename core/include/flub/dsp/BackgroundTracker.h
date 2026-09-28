// Flubsound Pro - background level tracker (docs/11 E19).
//
// A slow estimate of a level's background, in dB. It rises at most 5 dB/s
// and falls with a 400 ms time constant, so it sits in the lower part of a
// stationary level's spread (a minimum-statistics floor): a bed, rain or a
// held tone becomes background within seconds, a 20-80 ms cue or a burst of
// fire barely moves it, and it drops back to a quieter bed within about a
// second. It never goes below the caller's floor (out of digital silence the
// background IS the floor). For 300 ms after a reset it follows the level
// (40 ms) to learn it. update() and reset() are real-time safe.
//
// The law is the one the DynamicEq CueLift band tracks its band with. The
// Compressor's relative upward floor steps one at control rate, and
// analyseSceneEvents (flub/analysis/SceneEvents.h) one per 10 ms frame.
#pragma once

#include "flub/common/Math.h"
#include "flub/common/Realtime.h"

#include <algorithm>
#include <cmath>

namespace flub
{
class BackgroundTracker
{
public:
    static constexpr double kRiseDbPerSecond = 5.0;
    static constexpr double kFallMs = 400.0;
    static constexpr double kLearnMs = 300.0;
    static constexpr double kLearnFollowMs = 40.0;

    /** updateRateHz: how often update() is called (the sample rate, or a
        control rate). Resets; does not allocate. */
    void prepare (double updateRateHz) noexcept
    {
        const double rate = updateRateHz > 0.0 && std::isfinite (updateRateHz) ? updateRateHz : 48000.0;
        riseDbPerUpdate = static_cast<float> (kRiseDbPerSecond / rate);
        fallCoeff = static_cast<float> (1.0 - std::exp (-1000.0 / (kFallMs * rate)));
        learnCoeff = static_cast<float> (1.0 - std::exp (-1000.0 / (kLearnFollowMs * rate)));
        learnUpdates = std::max (1, static_cast<int> (std::lround (kLearnMs * 0.001 * rate)));
        reset();
    }

    /** Starts learning again (the next update() starts from its level). */
    void reset() noexcept FLUB_NONBLOCKING
    {
        learnLeft = learnUpdates;
        started = false;
    }

    /** One step: `levelDb` is the current level, `floorDb` the lowest the
        background may go. Returns the new background. Non-finite levels are
        read as silence. */
    float update (float levelDb, float floorDb) noexcept FLUB_NONBLOCKING
    {
        const float x = std::isfinite (levelDb) ? std::max (levelDb, kMinusInfDb) : kMinusInfDb;
        if (! started)
        {
            backgroundDb = x;
            started = true;
        }
        else if (learnLeft > 0)
            backgroundDb += (x - backgroundDb) * learnCoeff;
        else if (x > backgroundDb)
            backgroundDb += std::min (x - backgroundDb, riseDbPerUpdate);
        else
            backgroundDb += (x - backgroundDb) * fallCoeff;
        if (learnLeft > 0)
            --learnLeft;
        if (! std::isfinite (backgroundDb))
            backgroundDb = floorDb;
        backgroundDb = std::max (backgroundDb, floorDb);
        return backgroundDb;
    }

    float get() const noexcept { return backgroundDb; }
    bool isLearning() const noexcept { return learnLeft > 0; }

private:
    float backgroundDb = kMinusInfDb;
    float riseDbPerUpdate = 0.0f, fallCoeff = 0.0f, learnCoeff = 0.0f;
    int learnUpdates = 1, learnLeft = 1;
    bool started = false;
};
} // namespace flub
