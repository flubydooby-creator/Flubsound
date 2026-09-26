// Flubsound Pro - preset model and (de)serialisation.
//
// File format (UTF-8 JSON, *.flubpreset.json):
// {
//   "format": "flubsound-preset", "version": 1,
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
#pragma once

#include "flub/engine/Parameters.h"
#include "flub/io/Json.h"

#include <string>
#include <vector>

namespace flub::preset
{
struct Preset
{
    std::string name, category, author, description;
    std::vector<std::string> tags;
    std::vector<float> values; // param::kNumParams, full table
};

/** Preset with every parameter at its default. */
Preset makeDefault();

bool fromJson (const json::Value& v, Preset& out, std::string& error);
json::Value toJson (const Preset& p, bool full = false);

bool load (const std::string& path, Preset& out, std::string& error);
bool save (const std::string& path, const Preset& p, std::string& error, bool full = false);

/** Store <-> preset (bank). Non-RT but lock-free (store writes are atomic). */
void applyToStore (const Preset& p, param::ParameterStore& store, param::Bank bank);
Preset captureFromStore (const param::ParameterStore& store, param::Bank bank);
} // namespace flub::preset
