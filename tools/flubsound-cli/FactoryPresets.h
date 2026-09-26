// Flubsound Pro CLI - locating, listing and resolving factory presets.
//
// Factory presets are the presets/factory/**/*.json files of the source tree
// (the app embeds the same files as BinaryData). The CLI finds the folder at
// run time, first match wins:
//   1. an explicit folder (--dir for `presets`, --preset-dir for process/batch)
//   2. $FLUBSOUND_PRESET_DIR
//   3. <exe dir>/presets/factory, then the same below the executable's parent
//      folders, up to four levels (build trees: build/tools/flubsound-cli/../../..)
//   4. <exe dir>/../share/flubsound/presets/factory (installed layout) and
//      <exe dir>/../Resources/presets/factory (macOS bundle)
//   5. the source tree the binary was compiled from (FLUB_SOURCE_PRESET_DIR)
//
// `--preset <x>` accepts a path to a preset file, or a factory preset name:
// exact name (case-insensitive), file name, then a "loose" match ignoring
// case/punctuation, then a unique prefix/substring. Ambiguity is an error
// that lists the candidates.
#pragma once

#include "flub/io/PresetIO.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace flub::cli
{
struct FactoryPresetEntry
{
    std::string name, category, author, description, mode;
    std::vector<std::string> tags;
    std::filesystem::path file;
};

/** Absolute path of the running executable (empty if it cannot be found). */
std::filesystem::path executablePath();

/** Every folder that is tried, in order (for error messages). */
std::vector<std::filesystem::path> presetSearchPath (const std::string& explicitDir);

/** First existing folder of presetSearchPath(). An explicit folder that does
    not exist is NOT skipped silently: it is returned as nullopt. */
std::optional<std::filesystem::path> findFactoryPresetDir (const std::string& explicitDir);

/** Parses every *.json below dir (recursively), sorted by category then
    name. Files that fail to parse are reported in `problems`. */
std::vector<FactoryPresetEntry> scanPresetDir (const std::filesystem::path& dir, std::vector<std::string>& problems);

/** Loads `spec` (file path or factory preset name) into `out`.
    `resolvedFrom` receives a short description ("factory preset 'X' (file)").
    Returns false with a helpful message in `error`. */
bool resolvePreset (const std::string& spec, const std::string& explicitPresetDir, preset::Preset& out, std::string& resolvedFrom,
                    std::string& error);
} // namespace flub::cli
