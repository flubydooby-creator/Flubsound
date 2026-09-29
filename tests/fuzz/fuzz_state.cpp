// Flubsound Pro - libFuzzer target: saved settings and state (docs/11 E53).
//
// The core half of what the app and the plug-in read back from their saved
// settings, all from one JSON document:
//   * a strip's saved A/B state ("flubsound-strip-state", the JSON the app
//     keeps per strip in its settings file): the same steps as
//     EngineController's stripStateFromJson - preset::fromJson of "A" and
//     "B", applyToStore into both banks, the active bank - then the state is
//     captured and written again, and must read back to the same values;
//   * a user device-profile file (device::Database::load, then match() and
//     adviceFor() on a few endpoint names);
//   * plug-in / host state recall (preset::resolveSavedState) from the
//     document's "params" object, or the root object, as (key, number) pairs:
//     every value comes back finite and inside its range.
// The settings file itself (juce::PropertiesFile XML) and the plug-in's
// ValueTree are JUCE's parsers and are not fuzzed here.

#include "FuzzCheck.h"

#include "flub/engine/DeviceProfiles.h"
#include "flub/engine/Parameters.h"
#include "flub/io/Json.h"
#include "flub/io/PresetIO.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace flub;

constexpr const char* kStateFormat = "flubsound-strip-state"; // app/Source/engine/EngineController.cpp

json::Value stripStateToJson (const param::ParameterStore& store)
{
    json::Value root;
    root.set ("format", kStateFormat);
    root.set ("version", 1);
    root.set ("activeBank", store.getActiveBank() == param::Bank::A ? "A" : "B");
    root.set ("A", preset::toJson (preset::captureFromStore (store, param::Bank::A), false));
    root.set ("B", preset::toJson (preset::captureFromStore (store, param::Bank::B), false));
    return root;
}

bool stripStateFromJson (const json::Value& root, param::ParameterStore& store)
{
    if (root["format"].asString() != kStateFormat)
        return false;
    bool any = false;
    std::string error;
    for (const auto bank : { param::Bank::A, param::Bank::B })
    {
        const auto& v = root[bank == param::Bank::A ? "A" : "B"];
        preset::Preset p;
        if (v.isObject() && preset::fromJson (v, p, error))
        {
            preset::applyToStore (p, store, bank);
            any = true;
        }
    }
    if (any)
        store.setActiveBank (root["activeBank"].asString() == "B" ? param::Bank::B : param::Bank::A);
    return any;
}

void checkStripState (const json::Value& root)
{
    param::ParameterStore store;
    if (! stripStateFromJson (root, store))
        return;

    const auto& table = param::layout();
    std::vector<float> a (table.size()), b (table.size());
    for (int id = 0; id < param::kNumParams; ++id)
    {
        a[static_cast<size_t> (id)] = store.get (param::Bank::A, id);
        b[static_cast<size_t> (id)] = store.get (param::Bank::B, id);
        for (const float v : { a[static_cast<size_t> (id)], b[static_cast<size_t> (id)] })
            FUZZ_CHECK (std::isfinite (v) && v >= table[static_cast<size_t> (id)].minValue && v <= table[static_cast<size_t> (id)].maxValue);
    }

    // Saved again (what the app writes on exit) and restored into a new store.
    const auto text = json::write (stripStateToJson (store), 0);
    json::Value again;
    std::string error;
    FUZZ_CHECK (json::parse (text, again, error));
    param::ParameterStore restored;
    FUZZ_CHECK (stripStateFromJson (again, restored));
    FUZZ_CHECK (restored.getActiveBank() == store.getActiveBank());
    for (int id = 0; id < param::kNumParams; ++id)
    {
        FUZZ_CHECK (restored.get (param::Bank::A, id) == a[static_cast<size_t> (id)]);
        FUZZ_CHECK (restored.get (param::Bank::B, id) == b[static_cast<size_t> (id)]);
    }
}

void checkDeviceProfiles (const json::Value& root)
{
    device::Database db;
    std::string error;
    if (! db.load (root, error))
    {
        FUZZ_CHECK (! error.empty());
        return;
    }
    for (const char* endpoint : { "Headphones (Stealth 700 Gen 2 MAX)", "Speakers (Realtek(R) Audio)", "", "arctis nova 7" })
        for (const bool gaming : { false, true })
        {
            const auto match = db.match (endpoint, 48000.0, 2);
            const auto advice = device::adviceFor (match, 48000.0, gaming);
            (void) advice;
        }
    for (const auto& profile : db.profiles())
        (void) db.match (profile.displayName, 44100.0, 8, device::Connection::Bluetooth);
}

void checkSavedState (const json::Value& root)
{
    const auto& object = root["params"].isObject() ? root["params"].asObject() : root.asObject();
    std::vector<std::pair<std::string, float>> saved;
    for (const auto& [key, value] : object)
        if (value.isNumber())
            saved.emplace_back (key, static_cast<float> (value.asNumber()));

    std::vector<std::string> warnings;
    const auto values = preset::resolveSavedState (saved, &warnings);
    const auto& table = param::layout();
    FUZZ_CHECK (values.size() == table.size());
    for (size_t id = 0; id < table.size(); ++id)
        FUZZ_CHECK (std::isfinite (values[id]) && values[id] >= table[id].minValue && values[id] <= table[id].maxValue);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput (const uint8_t* data, size_t size)
{
    json::Value root;
    std::string error;
    if (! json::parse (std::string (reinterpret_cast<const char*> (data), size), root, error))
        return 0;

    checkStripState (root);
    checkDeviceProfiles (root);
    checkSavedState (root);
    return 0;
}
