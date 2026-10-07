// Flubsound Pro CLI - `demo`: the by-ear demo pack (docs/11 E37 / E59
// tooling; the listening guide builds on it).
//
// For every macro of both modes, Boost 0 -> 50 / 100 in both modes, each
// user-facing module switch (Smoothness, crossfeed and its types, virtualiser
// and its renderer, loudness contour, Startle Guard, Night, the four
// maximizer styles, the module cards), the genre and voice presets and the
// app's own settings (below), `demo` renders a
// before / after pair of WAV files through the processing chain (renderFile,
// exactly what `process` runs) and writes index.txt, which says per pair how
// both sides were set (as `flubsound-cli process --set` options), what
// changed in numbers and what to listen for.
//
//   * Programmes: built-in synthetic ones (48 kHz, generated here from fixed
//     seeds): "music" (drums, bass line, chord pad, a sung lead), "speech"
//     (a formant voice with sibilants and pauses), "game" (ambience,
//     footsteps moving left to right, gunshots, two explosions, a voice line,
//     a quiet score), "game-7.1" (the same scene as an 8-channel bed, for
//     the virtualiser), "music-loud" (the music mastered loud: soft-clipped,
//     PLR under 7.5 LU, for Smart macros), "speech-hiss" (the voice over a
//     hiss floor, for the noise gate), "speech-noisy" (the voice over a fan
//     with mains hum and keyboard typing, for the neural voice cleanup) and
//     "chat-scene" (the game scene on the
//     Game strip with a teammate's voice on a Chat strip). --input replaces
//     them with the user's file; the virtualiser pairs keep the built-in 7.1
//     scene when that file is not 5.1 / 7.1 (the virtualiser does not run on
//     stereo), the chat pairs always keep the built-in scene and voice, and
//     the neural voice cleanup pair its noisy voice (a 48 kHz speech model).
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
//   * The app's own settings (docs/12-feature-guide.md names a pair for
//     each): a side can also load a factory preset (`--preset`) and carry
//     host settings that the app keeps outside the parameters (DemoHost:
//     Smart macros, the headset enhancement cap, the safe speaker bass cap,
//     a headphone correction, the per-ear profile, the hearing guard, the
//     chat duck and ChatMix). Such a pair renders both sides through a
//     MixEngine, as the app runs it (the strip named after the mode, a Chat
//     strip fed a built-in voice where the pair needs one, the device
//     correction, the master limiter at -1 dBTP and the hearing guard),
//     instead of `process`'s single chain; its index gives the engine's
//     readouts instead of render.stats.
#pragma once

#include "Analysis.h"
#include "CliOptions.h"
#include "OfflineRenderer.h"

#include "flub/engine/PersonalProfile.h"
#include "flub/engine/Protection.h"
#include "flub/io/WavFile.h"

#include <functional>
#include <limits>
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
    std::vector<std::string> only; // pair slugs to render (empty = every pair), in list order
};

/** A side's settings that are not parameters: what the app sets on its
    engine (see the header comment). The defaults are "not set". */
struct DemoHost
{
    bool engine = false;            // render through a MixEngine (set on both sides of a pair)
    ProtectionStrength protection = ProtectionStrength::Off; // ProcessingChain::setProtectionStrength (docs/11 E06)
    bool smartMacros = false;       // ProcessingChain::setSmartMacros (E34)
    bool onboardCap = false;        // ProcessingChain::setOnboardEnhancementCap (E16)
    float safeSpeakerBassCapDb = std::numeric_limits<float>::infinity(); // setSafeSpeakerBassCapDb (E51)
    std::string correctionText;     // an AutoEQ / APO ParametricEQ text for the device correction (E15)
    PersonalProfile personal;       // ProcessingChain::setPersonalProfile (E33); disabled = none
    float sensitivityDbSpl = std::numeric_limits<float>::quiet_NaN(); // HearingGuard (E32 (c)); NaN = unknown
    float endpointVolumeDb = 0.0f;  // the system volume the guard counts
    bool capOn = false;             // the listening-level cap
    float capDbA = 85.0f;
    bool chatStrip = false;         // a Chat strip beside the main one, fed the built-in voice (E22)
    bool chatDuck = false;          // MixEngine::setChatDuck
    float chatDuckDepthDb = 4.5f;
    float chatMix = 0.0f;           // MixEngine::setChatMix, -1 (Game) .. +1 (Chat)
    bool neuralVoiceCleanup = false; // the voice cleanup model in the chain's neural slot (RenderSettings, docs/03 §16)

    /** "" when nothing is set; else the settings in words ("Smart macros on,
        ..."), unique per distinct setting (a render is shared by it). */
    std::string describe() const;
};

/** One pair of the pack, as listed (the same for every run). */
struct DemoPairSpec
{
    std::string slug;           // file stem: "music-punch"
    std::string title;          // "Music - Punch 0 -> 100 %"
    std::string programme;      // "music", "music-loud", "speech", "speech-hiss", "speech-noisy", "game", "game-7.1", "chat-scene"
    std::vector<ParamSetting> before, after; // --set options of each side
    bool levelFeature = false;  // not loudness-matched
    std::string listenFor;
    std::string beforePreset, afterPreset; // factory preset of each side (--preset; empty = the defaults)
    DemoHost beforeHost, afterHost;        // the app's settings of each side
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
    RenderStats beforeStats, afterStats;             // of `process`'s chain (not set for engine pairs)
    std::string beforeEngine, afterEngine;           // the engine's readouts (engine pairs only)
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
