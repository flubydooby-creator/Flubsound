// Flubsound Pro - preset model and (de)serialisation.
//
// File format (UTF-8 JSON, *.flubpreset.json):
// {
//   "format": "flubsound-preset", "version": 2,
//   "uuid": "6f1c2c1e-3a0b-4d51-9a43-6c8f0e2b7d10",
//   "contentHash": "9c1d0e0f5a2b7c34",
//   "name": "Competitive FPS", "category": "Gaming", "author": "Flubsound",
//   "description": "...", "tags": ["fps", "footsteps"],
//   "params": { "mode": "Gaming", "boost": 0.4, "eq.0.freq": 90.0, ... }
// }
// * Parameters are keyed by stable string keys (param::Info::key); unknown
//   keys are ignored (forward compatible) and missing keys keep defaults
//   (backward compatible). Every key or value fromJson ignores or changes is
//   reported in Preset::warnings (docs/11 E52): unknown keys (with the
//   closest known key: `"bost"` -> did you mean "boost"?), values clamped to
//   the parameter's range, unknown choice labels and values of the wrong
//   type. The CLI and the app show them (stderr / --json, a toast).
// * Choice parameters are written as their label ("Gaming") and accept either
//   a label or an index when read. Toggles are written as true/false.
// * Only values that differ from defaults are written unless `full` is set.
// * Schema version (docs/11 E52): "version" is major.minor, written as a
//   JSON number (2, 2.1) or read from a string ("2.1"); a missing version is
//   1.0. A newer MINOR of a known major loads, with a warning (a minor only
//   adds optional fields and keys, which are ignored). A newer MAJOR is
//   refused (never quarantined or rewritten). An older major is migrated IN
//   MEMORY through the migration registry (migrate(): pure JSON -> JSON
//   steps, one per major); the file itself is never rewritten on load, only
//   an explicit save writes the current version.
// * Schema 2 (docs/11 E01): missing keys keep TODAY's defaults. A version-1
//   file (or one without "version") was written sparse against the version-1
//   defaults: the 1 -> 2 migration fills its missing keys from the frozen
//   version-1 defaults table (PresetIO.cpp kV1Defaults: today `virt.lfe`,
//   0 dB in version 1, +6 dB now). A later default change bumps the major and
//   appends a migration with the old value; tests/test_presets_golden.cpp
//   fails when a default changes without one (tests/golden/parameter-defaults.json).
// * "uuid" (optional, RFC 4122 text form, stored lower case) identifies a
//   preset across renames; auto-profile rules, hotkeys and content packs
//   should reference presets by it (findByUuid). Factory presets carry a
//   fixed one; makeUuid() gives a new random one to a preset saved without.
// * "contentHash" is contentHash() of the preset when it was saved: a hash
//   of every sound parameter's resolved value (app state excluded), so it
//   identifies the sound, not the file. It is written by toJson and kept in
//   Preset::savedContentHash when read; it is not a checksum (a mismatch
//   means the file was edited, or the parameter table grew since).
// * App state (isAppState(): `bypass`, `bypass.matched`, `latency.profile`)
//   shares the parameter table but belongs to the application, not to the
//   sound of a preset (docs/11 E40). Factory presets carry none of it.
//   Loading rule:
//     - applyPresetToStore() (a preset loaded into a strip) never writes it.
//       A `latency.profile` in "params" (files from before E40, or a user
//       preset saved from the store) is migrated to suggestedLatencyProfile
//       and not applied: a preset never re-prepares the engine or changes
//       another strip's latency through the MixEngine padding.
//     - applyToStore() (saved strip state) writes an app-state
//       value only when the JSON carried its key; a missing key leaves the
//       bank's value as it is instead of resetting it to the default.
//   A preset may name the profile it was made for in the top-level
//   "suggestedLatencyProfile" label ("Quality", "Balanced", "Low Latency").
//   It is metadata for a suggestion prompt (docs/11 E42a) and is never
//   applied on load.
// * Plug-in / host state (resolveSavedState): every parameter the saved state
//   does not carry takes its DEFAULT on load, never the value it had before
//   (docs/11 E52 Phase A: deterministic DAW recall).
#pragma once

#include "flub/engine/Parameters.h"
#include "flub/io/Json.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace flub::preset
{
/** Preset schema version "major.minor" (see the header comment). */
struct SchemaVersion
{
    int majorVersion = 1;
    int minorVersion = 0;

    friend bool operator== (SchemaVersion a, SchemaVersion b) noexcept
    {
        return a.majorVersion == b.majorVersion && a.minorVersion == b.minorVersion;
    }
    friend bool operator!= (SchemaVersion a, SchemaVersion b) noexcept { return ! (a == b); }
};

/** The version toJson() writes and the newest this build reads fully. */
inline constexpr SchemaVersion kSchemaVersion { 2, 0 };

/** "2", "2.1" (the form "version" is written in). */
std::string toString (SchemaVersion v);

/** Reads "version": missing -> 1.0; a number (2, 2.1) or a string ("2.1").
    False for anything else (negative, not major.minor, wrong type). */
bool parseSchemaVersion (const json::Value& version, SchemaVersion& out);

struct Preset
{
    std::string name, category, author, description;
    std::vector<std::string> tags;
    std::vector<float> values; // param::kNumParams, full table

    /** App-state ids (isAppState) whose key the JSON did not carry: applyToStore
        and toJson skip them. Empty for presets built in code (makeDefault,
        captureFromStore), which own every value. */
    std::vector<int> unsetAppState;

    /** The profile the preset was made for, if it names one (see above). */
    std::optional<param::LatencyProfileValue> suggestedLatencyProfile;

    /** Stable identity (lower-case RFC 4122 text), empty when the file has none. */
    std::string uuid;

    /** fromJson: the file's "contentHash" (empty when it has none). */
    std::string savedContentHash;

    /** fromJson: the schema version the file was written in (before migration). */
    SchemaVersion loadedVersion = kSchemaVersion;

    /** fromJson: what was ignored or changed while reading (unknown keys,
        clamped values, unknown labels, a newer minor version), one sentence
        each, for the CLI (stderr / --json) and the app (a toast). */
    std::vector<std::string> warnings;
};

/** True for parameters that are application state, not preset sound:
    bypass, bypass.matched and latency.profile. */
bool isAppState (int paramId) noexcept;

/** Preset with every parameter at its default. */
Preset makeDefault();

bool fromJson (const json::Value& v, Preset& out, std::string& error);
json::Value toJson (const Preset& p, bool full = false);

/** One step of the migration registry: rewrites a preset JSON of major
    `fromMajor` into major `fromMajor + 1`. Pure (JSON in, JSON out). */
struct Migration
{
    int fromMajor;
    const char* description;
    json::Value (*apply) (const json::Value& root);
};

/** The registry, one step per major up to kSchemaVersion.majorVersion. */
const std::vector<Migration>& migrations();

/** Brings a preset JSON of an older major to kSchemaVersion's major (a newer
    minor of the current major passes through). False with a message for an
    invalid or newer-major version. `from` receives the version read. */
bool migrate (const json::Value& root, json::Value& out, SchemaVersion& from, std::string& error);

/** The frozen defaults of schema major `majorVersion` that differ from
    today's (key, value); empty for the current major. The 1 -> 2 migration
    fills absent keys from the version-1 table. */
std::vector<std::pair<std::string, float>> frozenDefaults (int majorVersion);

/** Hash of every sound parameter's value (app state excluded): 16 lower-case
    hex digits of 64-bit FNV-1a over (key, float bits) in layout order. Equal
    sounds give equal hashes on every platform; any value change, and a new
    parameter in the table, changes it. */
std::string contentHash (const Preset& p);

/** A new random (version 4) uuid, lower case. Not real-time safe. */
std::string makeUuid();

/** True for the RFC 4122 text form (8-4-4-4-12 hex digits, any case). */
bool isValidUuid (const std::string& text);

/** The preset with this uuid (case-insensitive), nullptr if none. For
    references that must survive renames: auto-profile rules, hotkeys,
    content packs (docs/11 E52). */
const Preset* findByUuid (const std::vector<Preset>& presets, const std::string& uuid);

bool load (const std::string& path, Preset& out, std::string& error);
bool save (const std::string& path, const Preset& p, std::string& error, bool full = false);

/** Store <-> preset (bank). Non-RT but lock-free (store writes are atomic).
    applyToStore restores saved state: every value, except app state the JSON
    did not carry. applyPresetToStore loads a preset: every value except app
    state, which keeps the bank's value. */
void applyToStore (const Preset& p, param::ParameterStore& store, param::Bank bank);
void applyPresetToStore (const Preset& p, param::ParameterStore& store, param::Bank bank);
Preset captureFromStore (const param::ParameterStore& store, param::Bank bank);

/** Saved plug-in / host state recall (docs/11 E52 Phase A): the full value
    table (param::kNumParams values) for the (key, value) pairs a saved state
    carries. A parameter the state does not carry takes its DEFAULT, never the
    value it had before the load; values are clamped to their range; unknown
    keys and non-finite values are ignored and reported in `warnings`. */
std::vector<float> resolveSavedState (const std::vector<std::pair<std::string, float>>& saved,
                                      std::vector<std::string>* warnings = nullptr);
} // namespace flub::preset
