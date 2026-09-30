// Flubsound Pro CLI - command-line model, parsing and parameter resolution.
//
// Parsing is strict: unknown options, options that do not belong to the
// command, malformed numbers and out-of-range values are usage errors
// (exit code 2) with a message that says what was expected.
//
// Parameter precedence (later wins):
//   defaults -> --preset -> --mode -> --boost / --macro / --profile /
//   --ceiling -> --set key=value
// "Bypass All" is application state, never preset state (same rule as the
// app's PresetManager): a preset cannot bypass a render, only --set bypass=on.
#pragma once

#include "flub/engine/Parameters.h"
#include "flub/engine/Protection.h"
#include "flub/io/WavFile.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace flub::cli
{
enum class Command
{
    None,
    Process,
    Batch,
    Analyze,
    Quality,
    Soak,
    Params,
    Presets,
    Demo,
    Help,
    Version
};

struct MacroSetting
{
    std::string which; // "1".."5" or a macro name of the active mode ("punch", "footsteps", ...)
    float percent = 0.0f;
};

struct ParamSetting
{
    std::string key, value;
};

struct RenderOptions
{
    std::string presetSpec; // file path or factory preset name
    std::string presetDir;  // explicit factory preset folder
    std::optional<param::ModeValue> mode;
    std::optional<float> boostPercent;
    std::vector<MacroSetting> macros;
    std::vector<ParamSetting> sets;
    std::optional<float> targetLufs;
    std::optional<float> ceilingDb;
    std::optional<param::LatencyProfileValue> profile;
    // SafetyGovernor reach (--protection): a host setting of the chain, not
    // a parameter (ProcessingChain::setProtectionStrength); Off like the app.
    ProtectionStrength protection = ProtectionStrength::Off;
    io::SampleFormat format = io::SampleFormat::Float32;
    int blockSize = 512;
};

struct CliOptions
{
    Command command = Command::None;
    std::string commandName;  // as typed
    std::string helpTopic;    // `help <command>` / `<command> --help`
    std::string input, output;
    std::string dir;          // `presets --dir`
    bool json = false;
    bool quiet = false;
    bool recursive = false;   // `batch --recursive`
    bool bands = false;       // `process` / `analyze --bands`: octave-band levels
    int jobs = 0;             // `batch --jobs` (0 = hardware concurrency)
    double rate = 48000.0;    // `quality --rate`: sample rate of the hygiene family; `soak --rate`
    // `analyze --events / --event-band / --glitches` (docs/11 E60, E53)
    bool events = false;      // scene events (with --bands: per octave band, and level tracks)
    double eventBandHz = 0.0; // the events list in one band (0 = full band)
    bool glitches = false;    // discontinuity detector
    bool spatial = false;     // `analyze --spatial`: IACC / DRR / diffuse field of binaural impulses (docs/11 E60)
    bool focusIld = false;    // `analyze --focus-ild`: ILD through the positional focus off / 50 / 100 % (docs/11 E24)
    // `soak` (docs/11 E53)
    double soakSeconds = 600.0;
    uint32_t seed = 1;
    std::string automation = "user"; // off | user | all
    double intervalMs = 250.0;
    // `demo` (Demo.h): --seconds (built-in programme length), and whether
    // --format was given (the pack defaults to pcm24, not f32)
    double demoSeconds = 10.0;
    bool demoFormatSet = false;
    RenderOptions render;
};

/** args excludes argv[0]. Returns false with a usage error message. */
bool parseCommandLine (const std::vector<std::string>& args, CliOptions& out, std::string& error);

struct ResolvedParameters
{
    std::vector<float> values;      // param::kNumParams base values for the store
    std::string presetDescription;  // "defaults", "factory preset 'X' (...)", ...
    std::vector<std::string> notes; // informational adjustments worth printing
};

/** Builds the base parameter table for a render (resolves the preset once;
    batch jobs copy the result). Returns false with a message on failure. */
bool buildParameters (const RenderOptions& options, ResolvedParameters& out, std::string& error);

/** Parses a --set value for parameter `id`: numbers (optionally with the
    parameter's unit, "k" / "kHz" for frequencies, "%" for percent values),
    choice labels or indices, and on/off/true/false/yes/no/1/0 for toggles.
    Out-of-range values are errors (not silently clamped). */
bool parseParameterValue (int id, const std::string& text, float& value, std::string& error);

/** Display form of a stored value ("-3.5 dB", "60 %", "Gaming", "on"). */
std::string formatParameterValue (int id, float value);

/** Short unit label ("dB", "Hz", "%", ...). */
const char* unitLabel (param::Unit unit) noexcept;
} // namespace flub::cli
