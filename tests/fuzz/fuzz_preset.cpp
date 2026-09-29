// Flubsound Pro - libFuzzer target: preset files (docs/11 E53).
//
// The input is parsed as JSON, then run through what loading a preset file
// does: the migration registry (preset::migrate) and preset::fromJson, and
// with `"format": "flubsound-preset"` forced in, so the fuzzer reaches the
// parameter loop without having to guess the format tag first. For every
// preset fromJson accepts:
//   * every value is finite and inside its parameter's range;
//   * toJson -> fromJson gives the same values, app-state flags (see
//     checkRoundTrip), uuid, suggested profile and contentHash (sparse and
//     full form);
//   * loading it into a ParameterStore and capturing it back gives the
//     same values (applyToStore) and never touches app state
//     (applyPresetToStore).

#include "FuzzCheck.h"

#include "flub/engine/Parameters.h"
#include "flub/io/Json.h"
#include "flub/io/PresetIO.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace
{
using namespace flub;

void checkRoundTrip (const preset::Preset& p, bool full)
{
    preset::Preset back;
    std::string error;
    FUZZ_CHECK (preset::fromJson (preset::toJson (p, full), back, error));
    for (int id = 0; id < param::kNumParams; ++id)
    {
        const bool unset = std::find (p.unsetAppState.begin(), p.unsetAppState.end(), id) != p.unsetAppState.end();
        if (! unset)
            FUZZ_CHECK (back.values[static_cast<size_t> (id)] == p.values[static_cast<size_t> (id)]);
    }
    // App state is written when it is set, except that the sparse form drops
    // values at their default like any other parameter: those come back unset
    // (applyToStore then keeps the bank's value, which the app's restore into
    // a fresh store makes the default anyway).
    for (const int id : back.unsetAppState)
        FUZZ_CHECK (std::find (p.unsetAppState.begin(), p.unsetAppState.end(), id) != p.unsetAppState.end()
                    || (! full && p.values[static_cast<size_t> (id)] == param::layout()[static_cast<size_t> (id)].defaultValue));
    for (const int id : p.unsetAppState)
        FUZZ_CHECK (std::find (back.unsetAppState.begin(), back.unsetAppState.end(), id) != back.unsetAppState.end());
    FUZZ_CHECK (back.uuid == p.uuid);
    FUZZ_CHECK (back.name == p.name && back.tags == p.tags && back.description == p.description);
    FUZZ_CHECK (back.suggestedLatencyProfile == p.suggestedLatencyProfile);
    FUZZ_CHECK (preset::contentHash (back) == preset::contentHash (p));
    FUZZ_CHECK (back.savedContentHash == preset::contentHash (p));
}

void checkPreset (const preset::Preset& p)
{
    const auto& table = param::layout();
    FUZZ_CHECK (p.values.size() == table.size());
    for (size_t id = 0; id < table.size(); ++id)
    {
        const float v = p.values[id];
        FUZZ_CHECK (std::isfinite (v) && v >= table[id].minValue && v <= table[id].maxValue);
    }

    checkRoundTrip (p, false);
    checkRoundTrip (p, true);

    param::ParameterStore store;
    preset::applyToStore (p, store, param::Bank::B);
    const auto captured = preset::captureFromStore (store, param::Bank::B);
    for (int id = 0; id < param::kNumParams; ++id)
        if (std::find (p.unsetAppState.begin(), p.unsetAppState.end(), id) == p.unsetAppState.end())
            FUZZ_CHECK (captured.values[static_cast<size_t> (id)] == p.values[static_cast<size_t> (id)]);

    param::ParameterStore fresh;
    preset::applyPresetToStore (p, fresh, param::Bank::A);
    for (int id = 0; id < param::kNumParams; ++id)
        if (preset::isAppState (id))
            FUZZ_CHECK (fresh.get (param::Bank::A, id) == table[static_cast<size_t> (id)].defaultValue);
}

void loadAndCheck (const json::Value& root)
{
    json::Value migrated;
    preset::SchemaVersion from;
    std::string error;
    if (preset::migrate (root, migrated, from, error))
        FUZZ_CHECK (preset::parseSchemaVersion (migrated["version"], from) && from.majorVersion == preset::kSchemaVersion.majorVersion);
    else
        FUZZ_CHECK (! error.empty());

    preset::Preset p;
    if (preset::fromJson (root, p, error))
        checkPreset (p);
    else
        FUZZ_CHECK (! error.empty());
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput (const uint8_t* data, size_t size)
{
    json::Value root;
    std::string error;
    if (! json::parse (std::string (reinterpret_cast<const char*> (data), size), root, error))
        return 0;

    loadAndCheck (root);
    if (root.isObject() && root["format"].asString() != "flubsound-preset")
    {
        root.set ("format", "flubsound-preset");
        loadAndCheck (root);
    }
    return 0;
}
