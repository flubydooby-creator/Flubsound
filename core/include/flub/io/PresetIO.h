// Flubsound Pro - preset model and (de)serialisation.
//
// File format (UTF-8 JSON, *.flubpreset.json):
// {
//   "format": "flubsound-preset", "version": 2,
//   "name": "Competitive FPS", "category": "Gaming", "author": "Flubsound",
//   "description": "...", "tags": ["fps", "footsteps"],
//   "params": { "mode": "Gaming", "boost": 0.4, "eq.0.freq": 90.0, ... }
// }
// * Parameters are keyed by stable string keys (param::Info::key); unknown
//   keys are ignored (forward compatible) and missing keys keep defaults
//   (backward compatible).
// * Choice parameters are written as their label ("Gaming") and accept either
//   a label or an index when read. Toggles are written as true/false.
// * Only values that differ from defaults are written unless `full` is set.
// * Schema version 2 (docs/11 E01): missing keys keep TODAY's defaults. A
//   version-1 file (or one without "version") was written sparse against the
//   version-1 defaults, so its missing keys load those: PresetIO.cpp keeps a
//   frozen table of every default changed since (today `virt.lfe`, 0 dB in
//   version 1, +6 dB now). Files newer than version 2 are rejected; toJson()
//   writes version 2.
// * App state (isAppState(): `bypass`, `bypass.matched`, `latency.profile`)
//   shares the parameter table but belongs to the application, not to the
//   sound of a preset (docs/11 E40). Factory presets carry none of it.
//   Loading rule:
//     - applyPresetToStore() (a preset loaded into a strip) never writes it.
//       A `latency.profile` in "params" (files from before E40, or a user
//       preset saved from the store) is migrated to suggestedLatencyProfile
//       and not applied: a preset never re-prepares the engine or changes
//       another strip's latency through the MixEngine padding.
//     - applyToStore() (saved strip state, plug-in state) writes an app-state
//       value only when the JSON carried its key; a missing key leaves the
//       bank's value as it is instead of resetting it to the default.
//   A preset may name the profile it was made for in the top-level
//   "suggestedLatencyProfile" label ("Quality", "Balanced", "Low Latency").
//   It is metadata for a suggestion prompt (docs/11 E42a) and is never
//   applied on load.
#pragma once

#include "flub/engine/Parameters.h"
#include "flub/io/Json.h"

#include <optional>
#include <string>
#include <vector>

namespace flub::preset
{
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
};

/** True for parameters that are application state, not preset sound:
    bypass, bypass.matched and latency.profile. */
bool isAppState (int paramId) noexcept;

/** Preset with every parameter at its default. */
Preset makeDefault();

bool fromJson (const json::Value& v, Preset& out, std::string& error);
json::Value toJson (const Preset& p, bool full = false);

bool load (const std::string& path, Preset& out, std::string& error);
bool save (const std::string& path, const Preset& p, std::string& error, bool full = false);

/** Store <-> preset (bank). Non-RT but lock-free (store writes are atomic).
    applyToStore restores saved state: every value, except app state the JSON
    did not carry. applyPresetToStore loads a preset: every value except app
    state, which keeps the bank's value. */
void applyToStore (const Preset& p, param::ParameterStore& store, param::Bank bank);
void applyPresetToStore (const Preset& p, param::ParameterStore& store, param::Bank bank);
Preset captureFromStore (const param::ParameterStore& store, param::Bank bank);
} // namespace flub::preset
