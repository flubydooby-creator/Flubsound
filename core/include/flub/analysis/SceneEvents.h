// Flubsound Pro - programme background and scene events (docs/11 E19, E60).
//
// BackgroundTracker (real-time safe): a slow estimate of a level's
// background, in dB. It rises at most 5 dB/s and falls with a 400 ms time
// constant, so it sits in the lower part of a stationary level's spread (a
// minimum-statistics floor): a bed, rain or a held tone becomes background
// within seconds, a 20-80 ms cue or a burst of fire barely moves it, and it
// drops back to a quieter bed within about a second. It never goes below
// the caller's floor (out of digital silence the background IS the floor).
// For 300 ms after a reset it follows the level (40 ms) to learn it. The law
// is the one the DynamicEq CueLift band tracks its band with; the
// Compressor's relative upward floor uses this class.
//
// analyseSceneEvents (offline; allocates, never on the audio thread): frames
// a multichannel buffer (10 ms), optionally in one band (RBJ band-pass), and
// reports per frame the mean-square level over all channels, the sample peak
// and the tracked background, plus a list of events:
//   Onset       the level rises onsetDb over the background and falls back
//               within maxOnsetMs (a step, a click, a short cue);
//   Loud        the peak passes loudOverBackgroundDb over the background, or
//               loudAbsoluteDb (gunfire, explosions);
//   Silence     the level stays under silenceDb for at least minSilenceMs;
//   LevelChange the mean level over changeWindowMs after a frame differs by
//               at least changeDb from the window before it (a scene or
//               track change; one event per change, at its largest step).
// The CLI's `analyze --events` and the E60 scene tests (tests/test_scenes.cpp)
// read programmes with it.
#pragma once

#include "flub/common/Math.h"
#include "flub/common/Realtime.h"

#include <algorithm>
#include <cmath>
#include <vector>

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

//==============================================================================
enum class SceneEventType
{
    Onset,
    Loud,
    Silence,
    LevelChange
};

const char* sceneEventName (SceneEventType type) noexcept;

struct SceneEventSettings
{
    double frameMs = 10.0;
    double bandHz = 0.0;              // 0 = full band, else an RBJ band-pass at this centre ...
    double bandQ = 1.0;               // ... and Q
    float floorDb = -90.0f;           // the background never goes below this
    float onsetDb = 6.0f;             // Onset: level over the background ...
    double maxOnsetMs = 300.0;        // ... for at most this long
    float loudOverBackgroundDb = 20.0f; // Loud: peak over the background ...
    float loudAbsoluteDb = -6.0f;     // ... or over this (dBFS)
    float silenceDb = -70.0f;         // Silence: level under this ...
    double minSilenceMs = 300.0;      // ... for at least this long
    float changeDb = 6.0f;            // LevelChange: mean level after vs before ...
    double changeWindowMs = 2000.0;   // ... over this long on each side
};

struct SceneFrame
{
    float levelDb = kMinusInfDb;      // mean square over the frame and all channels
    float peakDb = kMinusInfDb;       // sample peak over the frame and all channels
    float backgroundDb = kMinusInfDb; // BackgroundTracker on levelDb
};

struct SceneEvent
{
    SceneEventType type = SceneEventType::Onset;
    double startSeconds = 0.0, endSeconds = 0.0;
    float levelDb = kMinusInfDb;      // Onset / Silence: highest frame level; Loud: highest peak;
                                      // LevelChange: mean level after the change
    float overBackgroundDb = 0.0f;    // Onset / Loud: highest level (peak) over the background;
                                      // LevelChange: after minus before; Silence: 0
};

struct SceneAnalysis
{
    double frameSeconds = 0.01;
    std::vector<SceneFrame> frames;
    std::vector<SceneEvent> events; // in order of their start
};

/** channels: planar [channel][frame], equal lengths. */
SceneAnalysis analyseSceneEvents (const std::vector<std::vector<float>>& channels, double sampleRate,
                                  const SceneEventSettings& settings = {});
} // namespace flub
