// Flubsound Pro CLI - soak: the processing chain on generated programme with
// parameter automation, watched by the discontinuity detector (docs/11 E53
// step 4, the offline Linux slice).
//
// soakChain streams, so a run of hours needs no more memory than a few blocks:
//
//   * Programme (SoakProgramme): seeded and licence-free, a cycle of 6 s
//     scenes - music (bass, chords, a 997 Hz lead with vibrato, kicks),
//     game (a quiet low-passed bed, 3.2 kHz steps, shots and an explosion),
//     speech-like syllables (a 110-180 Hz harmonic voice), a loud section
//     that drives the maximizer to the ceiling, and a fade into 1 s of
//     digital silence and back. Stereo, peaks under -1 dBFS. Every envelope
//     is a raised-cosine ramp of 2 ms or more and the noise is low-passed, so
//     the programme itself is smooth and tonal: the detector (flub/analysis/
//     Discontinuity.h) reads it at its full sensitivity and finds nothing on
//     it (the input is watched too, as a self-check).
//   * Automation: every interval (+-50 %, seeded) one host action, applied
//     between blocks as the app's GUI thread would: Boost or a macro to a
//     random value, another continuous parameter (gains, EQ band gains,
//     bass, clarity, width, compressor, maximizer drive, saturation) to a
//     random value in its range, a module toggled, the mode flipped, a
//     factory preset loaded (app state and structural values kept), bypass
//     toggled, or an A/B bank switch. `All` sets any non-structural
//     parameter to a random value instead (a parameter fuzz). Structural
//     parameters (latency profile) never change: they re-prepare the chain.
//   * Watched: the chain's stereo output (and its input) through a
//     DiscontinuityDetector; each detection is reported with the most recent
//     automation action before it. Also: the output's sample peak, and the
//     wall time per block against its real-time budget.
//
// Everything but the wall times is deterministic for a seed.
#pragma once

#include "flub/analysis/Discontinuity.h"
#include "flub/engine/Protection.h"
#include "flub/io/Json.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace flub::cli
{
enum class SoakAutomation
{
    Off,
    User, // host actions a user takes (see the header comment)
    All   // any non-structural parameter to a random value
};

struct SoakSettings
{
    double seconds = 600.0;
    double sampleRate = 48000.0;
    int blockSize = 512;
    uint32_t seed = 1;
    SoakAutomation automation = SoakAutomation::User;
    double intervalMs = 250.0; // mean time between automation actions
    ProtectionStrength protection = ProtectionStrength::Off;
    DiscontinuitySettings detector;
    // Factory preset tables (param::kNumParams values each) the automation
    // may load, with their names; empty = no preset switches.
    std::vector<std::vector<float>> presets;
    std::vector<std::string> presetNames;
    // Test hook: called on the chain's output block (2 channels, frames
    // [startFrame, startFrame + numFrames) of the stream) before it is
    // watched, to inject a known glitch.
    std::function<void (int64_t startFrame, float* const* stereo, int numFrames)> inject;
    // Optional: called every 10 s of programme with the seconds done.
    std::function<void (double seconds)> progress;
};

struct SoakDetection
{
    Discontinuity event;
    double seconds = 0.0;
    std::string lastAction; // the automation action before it ("" if none)
    double lastActionAgeMs = 0.0;
    bool bypassed = false;  // the global bypass was engaged (the output is the bypass reference)
};

struct SoakReport
{
    double seconds = 0.0, sampleRate = 48000.0;
    int blockSize = 512, latencySamples = 0;
    uint32_t seed = 1;
    SoakAutomation automation = SoakAutomation::User;
    int64_t frames = 0;
    int64_t actions = 0;
    std::vector<std::pair<std::string, int64_t>> actionCounts; // per kind ("macro", "toggle", ...)
    std::array<int64_t, kNumDiscontinuityTypes> output {}, input {}; // detections per type
    std::vector<SoakDetection> detections;                           // output, up to detector.maxReported
    int64_t outputKinks = 0, outputRecurring = 0, outputBandLimited = 0; // click candidates set aside (DiscontinuityDetector::kinks / recurring / bandLimited)
    float outputPeakDbfs = -160.0f;
    double wallSeconds = 0.0, maxBlockMs = 0.0, meanBlockMs = 0.0, blockBudgetMs = 0.0;
    int64_t blocksOverBudget = 0;

    int64_t outputTotal() const noexcept;
    int64_t inputTotal() const noexcept;
};

/** Seeded programme generator (see the header comment), stereo, streaming. */
class SoakProgramme
{
public:
    SoakProgramme (double sampleRate, uint32_t seed);
    ~SoakProgramme();
    SoakProgramme (const SoakProgramme&) = delete;
    SoakProgramme& operator= (const SoakProgramme&) = delete;

    /** The next numFrames frames into left / right. */
    void render (float* left, float* right, int numFrames);

    static constexpr double kSceneSeconds = 6.0;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/** Runs the soak with `baseValues` (param::kNumParams) as the starting
    settings. Non-RT, allocates. False with `error` on bad settings. */
bool soakChain (const std::vector<float>& baseValues, const SoakSettings& settings, SoakReport& report, std::string& error);

/** "off", "user", "all". */
const char* soakAutomationName (SoakAutomation a) noexcept;

/** The `soak --json` object (dB rounded to 0.01, times to 0.1 ms). */
json::Value soakToJson (const SoakReport& report);

/** Human-readable multi-line report. */
std::string formatSoak (const SoakReport& report);
} // namespace flub::cli
