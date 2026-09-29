// Flubsound Pro CLI - `demo`: the by-ear demo pack (docs/11 E37 / E59
// tooling; the listening guide builds on it).
//
// For every macro of both modes, Boost 0 -> 50 / 100 in both modes and each
// user-facing module switch (Smoothness, crossfeed, virtualiser, loudness
// contour, Startle Guard, Night, the four maximizer styles), `demo` renders a
// before / after pair of WAV files through the processing chain (renderFile,
// exactly what `process` runs) and writes index.txt, which says per pair how
// both sides were set (as `flubsound-cli process --set` options), what
// changed in numbers and what to listen for.
//
//   * Programmes: built-in synthetic ones (48 kHz, generated here from fixed
//     seeds): "music" (drums, bass line, chord pad, a sung lead), "speech"
//     (a formant voice with sibilants and pauses), "game" (ambience,
//     footsteps moving left to right, gunshots, two explosions, a voice line,
//     a quiet score) and "game-7.1" (the same scene as an 8-channel bed, for
//     the virtualiser). --input replaces them with the user's file; only the
//     virtualiser pair keeps the built-in 7.1 scene when that file is not
//     5.1 / 7.1 (the virtualiser does not run on stereo).
//   * Loudness matching: by the E37 rule, only the louder side of a pair is
//     turned down to the quieter's integrated loudness (EBU R128), so a pair
//     is not won by the louder side and nothing is raised into clipping.
//     Level features - the Loudness macro, Startle Guard and Night, whose
//     job is the level itself - are left unmatched; the index says so and
//     gives the difference.
//   * Numbers: each written file is read back and measured as `analyze
//     --bands` measures it (integrated loudness, true peak, octave bands of
//     the mean of the channels); "band delta" is after minus before, per
//     octave band. The chain's render statistics of both sides (limiter,
//     compressor and Smoothness readouts) are given too.
//   * Deterministic: the same options give byte-identical files and index,
//     whatever the number of worker threads (every render has its own
//     parameter store and chain).
#pragma once

#include "Analysis.h"
#include "CliOptions.h"
#include "OfflineRenderer.h"

#include "flub/io/WavFile.h"

#include <functional>
#include <string>
#include <vector>

namespace flub::cli
{
struct DemoOptions
{
    std::string input;           // the user's WAV file; empty = the built-in programmes
    std::string outDir = "flubsound-demo";
    double seconds = 10.0;       // length of each built-in programme
    int jobs = 0;                // worker threads (0 = number of CPU cores)
    io::SampleFormat format = io::SampleFormat::Pcm24;
    std::string presetDir;       // factory preset folder (Night reads Night Mode Gaming's dynamics)
    int blockSize = 512;
};

/** One pair of the pack, as listed (the same for every run). */
struct DemoPairSpec
{
    std::string slug;           // file stem: "music-punch"
    std::string title;          // "Music - Punch 0 -> 100 %"
    std::string programme;      // "music", "speech", "game", "game-7.1"
    std::vector<ParamSetting> before, after; // --set options of each side
    bool levelFeature = false;  // not loudness-matched
    std::string listenFor;
};

/** The pair list (Night's dynamics are read from the factory preset folder
    `presetDir` resolves to; `note` says when its built-in copy was used). */
std::vector<DemoPairSpec> demoPairs (const std::string& presetDir, std::string* note = nullptr);

struct DemoPairResult
{
    DemoPairSpec spec;
    std::string programmeUsed;       // "music (built-in)", "your file (song.wav)", ...
    std::string beforeFile, afterFile; // names inside the pack folder
    bool matched = false;            // loudness-matched (false for level features / no measurement)
    float beforeTrimDb = 0.0f, afterTrimDb = 0.0f; // gain applied before writing (<= 0)
    LoudnessReport beforeReport, afterReport;       // of the written files
    std::vector<BandLevel> beforeBands, afterBands; // of the written files (analyze --bands)
    RenderStats beforeStats, afterStats;
};

struct DemoResult
{
    std::vector<DemoPairResult> pairs;
    int renders = 0;                 // distinct renders (sides shared by pairs are rendered once)
    std::string indexPath;
    std::vector<std::string> notes;
};

/** Renders and writes the pack into options.outDir (created if needed).
    `progress` (optional) receives one line per finished render. Returns
    false with a message on failure. Non-RT, allocates. */
bool makeDemoPack (const DemoOptions& options, DemoResult& result, std::string& error,
                   const std::function<void (const std::string&)>& progress = {});

/** The index.txt text of a finished pack. */
std::string formatDemoIndex (const DemoOptions& options, const DemoResult& result);

/** `flubsound-cli demo`. */
int runDemo (const CliOptions& options);
} // namespace flub::cli
