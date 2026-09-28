// Flubsound Pro - scene events (docs/11 E60).
//
// analyseSceneEvents (offline; allocates, never on the audio thread): frames
// a multichannel buffer (10 ms), optionally in one band (RBJ band-pass), and
// reports per frame the mean-square level over all channels, the sample peak
// and the background (flub/dsp/BackgroundTracker.h on the frame level, held
// while a loud event runs, for up to 3 s, so a burst of fire does not become
// ambience), plus a list of events:
//   Onset       the level rises onsetDb over the background and falls back
//               within maxOnsetMs (a step, a click, a short cue);
//   Loud        the peak passes loudOverBackgroundDb over the background, or
//               loudAbsoluteDb (gunfire, explosions); frames under 150 ms
//               apart form one event (a burst of fire);
//   Silence     the level stays under silenceDb for at least minSilenceMs;
//   LevelChange the median frame level over changeWindowMs after a frame
//               differs by at least changeDb from the median over the window
//               before it (a scene or track change; one event per change, at
//               its largest step; medians, so fire or speech pauses do not
//               read as a new level).
// The CLI's `analyze --events` and the E60 scene tests (tests/test_scenes.cpp)
// read programmes with it.
#pragma once

#include "flub/common/Math.h"
#include "flub/dsp/BackgroundTracker.h"

#include <vector>

namespace flub
{
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
