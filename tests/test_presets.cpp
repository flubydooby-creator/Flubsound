// PresetIO: app state (bypass, bypass.matched, latency.profile) is not part
// of a preset's sound (docs/11 E40). The rule, from flub/io/PresetIO.h:
//   * applyPresetToStore never writes app state; a `latency.profile` in a
//     preset's "params" (files from before E40, presets saved from the store)
//     becomes Preset::suggestedLatencyProfile instead;
//   * applyToStore (saved strip state) writes app state only when the JSON
//     carried its key;
//   * "suggestedLatencyProfile" is metadata: it round-trips, is never applied.
#include "TestFramework.h"

#include "flub/engine/ProcessingChain.h"
#include "flub/io/Json.h"
#include "flub/io/PresetIO.h"

#include <string>

using namespace flub;
using namespace flub::param;

namespace
{
preset::Preset parsePreset (const std::string& text)
{
    json::Value v;
    std::string error;
    REQUIRE (json::parse (text, v, error));
    preset::Preset p;
    REQUIRE (preset::fromJson (v, p, error));
    return p;
}

float profileValue (LatencyProfileValue profile) { return static_cast<float> (static_cast<int> (profile)); }

/** A pre-E40 preset: Quality profile, bypass and matched bypass in "params". */
const char* const kLegacyQualityPreset = R"({
  "format": "flubsound-preset", "version": 1,
  "name": "Old Warm Vinyl", "category": "Music", "author": "Me",
  "params": { "boost": 0.25, "latency.profile": "Quality", "bypass": true, "bypass.matched": false }
})";
} // namespace

TEST_CASE ("PresetIO: loading a preset that stores latency.profile Quality does not change the latency profile (E40)")
{
    const auto p = parsePreset (kLegacyQualityPreset);
    REQUIRE (p.suggestedLatencyProfile.has_value());
    CHECK (*p.suggestedLatencyProfile == LatencyProfileValue::Quality); // migrated to a suggestion

    ParameterStore store;
    store.set (Bank::A, LatencyProfile, profileValue (LatencyProfileValue::LowLatency));
    ProcessingChain chain (store);
    chain.prepare ({ 48000.0, 256, 2 });

    preset::applyPresetToStore (p, store, Bank::A);
    CHECK (store.get (Bank::A, BoostIntensity) == 0.25f); // the sound is loaded
    CHECK (store.get (Bank::A, LatencyProfile) == profileValue (LatencyProfileValue::LowLatency));
    CHECK (store.get (Bank::A, BypassAll) == 0.0f);
    CHECK (store.get (Bank::A, LoudnessMatchBypass) == 1.0f);
    CHECK (! chain.needsReprepare());
}

TEST_CASE ("PresetIO: a preset without app-state keys leaves the bank's values on both store paths")
{
    // Loading used to reset the profile to Balanced (the default) whenever the
    // file did not name one, so a strip on Low Latency moved to Balanced.
    const auto p = parsePreset (R"({ "format": "flubsound-preset", "version": 1, "name": "N", "params": { "boost": 0.5 } })");
    CHECK (p.unsetAppState.size() == 3);
    CHECK (! p.suggestedLatencyProfile.has_value());

    for (const bool presetPath : { true, false })
    {
        ParameterStore store;
        store.set (Bank::B, LatencyProfile, profileValue (LatencyProfileValue::LowLatency));
        store.set (Bank::B, BypassAll, 1.0f);
        store.set (Bank::B, LoudnessMatchBypass, 0.0f);
        if (presetPath)
            preset::applyPresetToStore (p, store, Bank::B);
        else
            preset::applyToStore (p, store, Bank::B);
        CHECK (store.get (Bank::B, BoostIntensity) == 0.5f);
        CHECK (store.get (Bank::B, LatencyProfile) == profileValue (LatencyProfileValue::LowLatency));
        CHECK (store.get (Bank::B, BypassAll) == 1.0f);
        CHECK (store.get (Bank::B, LoudnessMatchBypass) == 0.0f);
    }
}

TEST_CASE ("PresetIO: saved strip state still restores the latency profile and bypass it carries")
{
    // Strip state is captureFromStore -> toJson -> fromJson -> applyToStore.
    ParameterStore saved;
    saved.set (Bank::A, LatencyProfile, profileValue (LatencyProfileValue::Quality));
    saved.set (Bank::A, BypassAll, 1.0f);
    saved.set (Bank::A, MaxDriveDb, 3.0f);
    const std::string text = json::write (preset::toJson (preset::captureFromStore (saved, Bank::A), false));

    ParameterStore restored;
    preset::applyToStore (parsePreset (text), restored, Bank::A);
    CHECK (restored.get (Bank::A, LatencyProfile) == profileValue (LatencyProfileValue::Quality));
    CHECK (restored.get (Bank::A, BypassAll) == 1.0f);
    CHECK (restored.get (Bank::A, MaxDriveDb) == 3.0f);

    // A default (Balanced) profile is not written, and restoring onto a
    // fresh store gives Balanced.
    ParameterStore balanced;
    ParameterStore fresh;
    preset::applyToStore (parsePreset (json::write (preset::toJson (preset::captureFromStore (balanced, Bank::A), false))), fresh, Bank::A);
    CHECK (fresh.get (Bank::A, LatencyProfile) == profileValue (LatencyProfileValue::Balanced));
}

TEST_CASE ("PresetIO: suggestedLatencyProfile is read, written and never applied")
{
    const auto p = parsePreset (R"({ "format": "flubsound-preset", "version": 1, "name": "S",
                                     "suggestedLatencyProfile": "Low Latency", "params": { "mode": "Gaming" } })");
    REQUIRE (p.suggestedLatencyProfile.has_value());
    CHECK (*p.suggestedLatencyProfile == LatencyProfileValue::LowLatency);

    // Round trip: the label is written at the top level, and unset app state
    // stays out of "params" even with `full`.
    const auto j = preset::toJson (p, true);
    CHECK (j["suggestedLatencyProfile"].asString() == "Low Latency");
    CHECK (j["params"]["latency.profile"].isNull());
    CHECK (j["params"]["bypass"].isNull());
    CHECK (j["params"]["bypass.matched"].isNull());
    CHECK (! j["params"]["mode"].isNull());
    CHECK (parsePreset (json::write (j)).suggestedLatencyProfile == LatencyProfileValue::LowLatency);

    ParameterStore store;
    preset::applyToStore (p, store, Bank::A);
    CHECK (store.get (Bank::A, LatencyProfile) == profileValue (LatencyProfileValue::Balanced));

    // An explicit suggestion wins over a legacy params value; an unknown
    // label or an out-of-range index is ignored.
    CHECK (parsePreset (R"({ "format": "flubsound-preset", "suggestedLatencyProfile": "Balanced",
                             "params": { "latency.profile": "Quality" } })")
               .suggestedLatencyProfile
           == LatencyProfileValue::Balanced);
    CHECK (! parsePreset (R"({ "format": "flubsound-preset", "suggestedLatencyProfile": "Ultra" })").suggestedLatencyProfile.has_value());
    CHECK (! parsePreset (R"({ "format": "flubsound-preset", "suggestedLatencyProfile": 7 })").suggestedLatencyProfile.has_value());
    CHECK (parsePreset (R"({ "format": "flubsound-preset", "suggestedLatencyProfile": 0 })").suggestedLatencyProfile == LatencyProfileValue::Quality);
}

TEST_CASE ("PresetIO: isAppState names exactly bypass, bypass.matched and latency.profile")
{
    int count = 0;
    for (int id = 0; id < kNumParams; ++id)
        count += preset::isAppState (id) ? 1 : 0;
    CHECK (count == 3);
    CHECK (preset::isAppState (BypassAll));
    CHECK (preset::isAppState (LoudnessMatchBypass));
    CHECK (preset::isAppState (LatencyProfile));
    CHECK (! preset::isAppState (Mode));
}
