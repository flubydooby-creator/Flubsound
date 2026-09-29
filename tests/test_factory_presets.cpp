// Factory preset validation (presets/factory/*.json).
//
// Every factory preset must:
//   * parse, carry complete metadata (name, category, author, description,
//     tags) and live in a file named "<category>-<slug>.json",
//   * use only known parameter keys, valid choice labels, true/false toggles
//     and in-range numbers, and store ONLY non-default values,
//   * store no app state (`bypass`, `bypass.matched`, `latency.profile`):
//     loading a preset never changes the latency profile (docs/11 E40); the
//     profile a preset is made for is the "suggestedLatencyProfile" label,
//   * keep the product's output protection: maximizer engaged, output gain
//     <= 0 dB (it is applied after the limiter), ceiling <= -1 dBTP
//     (<= -2 dBTP for the Bluetooth device preset),
//   * render a hot drum/bass/pad programme through the full ProcessingChain
//     in Balanced, Low Latency and the suggested profile (the user, not the
//     preset, chooses it) with finite output, sample peaks below the ceiling
//     and no safety clips.
//
// FLUB_PRESET_DIR (and FLUB_DEVICE_PROFILES) are compile definitions from
// tests/CMakeLists.txt; builds without them compile this file to nothing.
#include "TestFramework.h"
#include "TestSignals.h"

#ifdef FLUB_PRESET_DIR

    #include "Analysis.h"
    #include "OfflineRenderer.h"

    #include "flub/analysis/PeakMeters.h"
    #include "flub/common/Denormals.h"
    #include "flub/dsp/TruePeakLimiter.h"
    #include "flub/engine/MacroMap.h"
    #include "flub/engine/ProcessingChain.h"
    #include "flub/io/Json.h"
    #include "flub/io/PresetIO.h"

    #include <algorithm>
    #include <cctype>
    #include <cstdint>
    #include <cstdio>
    #include <cstdlib>
    #include <filesystem>
    #include <fstream>
    #include <iostream>
    #include <set>
    #include <sstream>
    #include <string>
    #include <vector>

using namespace flub;
using namespace flub::param;
using namespace flubtest;

namespace
{
namespace fs = std::filesystem;

constexpr double kFs = 48000.0;
constexpr int kBlockSize = 512;
constexpr int kRenderSamples = static_cast<int> (kFs * 4.0); // 4 s programme

/** Upper bounds per latency profile (Quality, Balanced, Low Latency) in ms,
    from the ProcessingChain contract (~28.2 / 4.0 / ~2.1 ms at 48 kHz). */
constexpr double kMaxLatencyMs[3] = { 40.0, 5.0, 2.5 };

/** The limiter works on its own 4x true-peak estimate; an independent 4x
    meter may read slightly higher between samples (same margin as the
    engine-level ceiling test in test_engine.cpp). */
constexpr float kTruePeakToleranceDb = 0.15f;

struct FactoryFile
{
    fs::path path;
    std::string text;
};

std::string toLower (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    return s;
}

bool contains (const std::string& haystack, const std::string& needle)
{
    return toLower (haystack).find (toLower (needle)) != std::string::npos;
}

/** All *.json files of the factory folder, sorted by file name (stable output). */
std::vector<FactoryFile> factoryFiles()
{
    std::vector<FactoryFile> files;
    std::error_code ec;
    for (fs::directory_iterator it (FLUB_PRESET_DIR, ec), end; ! ec && it != end; it.increment (ec))
    {
        if (! it->is_regular_file() || toLower (it->path().extension().string()) != ".json")
            continue;
        std::ifstream in (it->path(), std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        files.push_back ({ it->path(), ss.str() });
    }
    std::sort (files.begin(), files.end(),
               [] (const FactoryFile& a, const FactoryFile& b) { return a.path.filename() < b.path.filename(); });
    return files;
}

bool isBluetoothPreset (const preset::Preset& p)
{
    if (contains (p.name, "bluetooth"))
        return true;
    return std::any_of (p.tags.begin(), p.tags.end(), [] (const std::string& t) { return contains (t, "bluetooth"); });
}

bool isSurroundPreset (const preset::Preset& p)
{
    if (contains (p.name, "7.1"))
        return true;
    return std::any_of (p.tags.begin(), p.tags.end(), [] (const std::string& t) { return t == "7.1"; });
}

int choiceIndex (const preset::Preset& p, int id)
{
    return static_cast<int> (std::lround (p.values[static_cast<size_t> (id)]));
}

constexpr LatencyProfileValue kProfiles[] = { LatencyProfileValue::Quality, LatencyProfileValue::Balanced, LatencyProfileValue::LowLatency };

const std::string& profileName (LatencyProfileValue profile)
{
    return layout()[static_cast<size_t> (LatencyProfile)].choices[static_cast<size_t> (profile)];
}

/** The profiles a preset is rendered in: the user, not the preset, picks the
    profile, so Balanced (the default) and Low Latency (the shortest limiter
    look-ahead) always, and Quality (the longest look-aheads, the gate and HQ
    oversampling; by far the slowest to render) when the preset suggests it. */
std::vector<LatencyProfileValue> renderProfiles (const preset::Preset& p)
{
    std::vector<LatencyProfileValue> profiles { LatencyProfileValue::Balanced, LatencyProfileValue::LowLatency };
    if (p.suggestedLatencyProfile == LatencyProfileValue::Quality)
        profiles.push_back (LatencyProfileValue::Quality);
    return profiles;
}

/** Hot drum / bass / pad programme (peaks ~ -0.3 dBFS, i.e. a loud master):
    120 BPM kick with pitch drop, snare on 2 and 4, 8th-note hats, a moving
    bass line and a detuned stereo pad. 2 channels, or 8 channels in 7.1
    order (FL FR FC LFE BL BR SL SR) with the elements spread around. */
Planar makeProgramme (int numChannels)
{
    Planar p (numChannels, kRenderSamples);
    FastRandom rng (2024);
    float prevNoise = 0.0f;
    const double bassNotes[4] = { 55.0, 55.0, 43.65, 49.0 };
    for (int i = 0; i < kRenderSamples; ++i)
    {
        const double t = i / kFs;
        const double beat = std::fmod (t, 0.5);
        const double kick = 0.9 * std::exp (-beat * 9.0) * std::sin (kTwoPi * (48.0 * beat + 2.8 * (1.0 - std::exp (-beat * 25.0))));
        const double snT = std::fmod (t, 1.0) - 0.5;
        const float noise = rng.nextBipolar();
        const double snare =
            snT >= 0.0 ? 0.45 * noise * std::exp (-snT * 22.0) + 0.3 * std::sin (kTwoPi * 190.0 * snT) * std::exp (-snT * 30.0) : 0.0;
        const double hat = 0.12 * (noise - prevNoise) * std::exp (-std::fmod (t, 0.25) * 60.0);
        prevNoise = noise;
        const double f0 = bassNotes[static_cast<int> (std::fmod (t / 2.0, 4.0))];
        const double env = 0.35 + 0.65 * std::exp (-std::fmod (t, 0.25) * 6.0);
        const double bass =
            0.42 * env * (std::sin (kTwoPi * f0 * t) + 0.35 * std::sin (kTwoPi * 2.0 * f0 * t) + 0.15 * std::sin (kTwoPi * 3.0 * f0 * t));
        const double padL = 0.07 * (std::sin (kTwoPi * 220.0 * t) + std::sin (kTwoPi * 277.18 * t) + std::sin (kTwoPi * 329.63 * t));
        const double padR =
            0.07 * (std::sin (kTwoPi * 220.6 * t + 0.4) + std::sin (kTwoPi * 277.9 * t + 1.1) + std::sin (kTwoPi * 330.4 * t + 2.0));
        const auto s = static_cast<size_t> (i);
        if (numChannels == 2)
        {
            p.ch[0][s] = static_cast<float> (kick + snare + 0.8 * hat + bass + padL);
            p.ch[1][s] = static_cast<float> (kick + snare + 1.2 * hat + bass + padR);
        }
        else
        {
            p.ch[0][s] = static_cast<float> (0.5 * kick + padL);
            p.ch[1][s] = static_cast<float> (0.5 * kick + padR);
            p.ch[2][s] = static_cast<float> (snare + 0.5 * bass);
            p.ch[3][s] = static_cast<float> (0.5 * kick + 0.5 * bass);
            p.ch[4][s] = static_cast<float> (0.7 * padL);
            p.ch[5][s] = static_cast<float> (0.7 * padR);
            p.ch[6][s] = static_cast<float> (1.5 * hat);
            p.ch[7][s] = static_cast<float> (-1.5 * hat);
        }
    }

    double peak = 0.0;
    for (const auto& c : p.ch)
        peak = std::max (peak, peakAbs (c.data(), kRenderSamples));
    const auto gain = static_cast<float> (dbToGain (-0.3f) / peak);
    for (auto& c : p.ch)
        for (auto& v : c)
            v *= gain;
    return p;
}

/** Why a parameter value in the JSON is not acceptable ("" = fine). */
std::string checkParamValue (const Info& info, const json::Value& value)
{
    switch (info.unit)
    {
        case Unit::Choice:
        {
            if (! value.isString())
                return "choice must be written as a label";
            const auto it = std::find (info.choices.begin(), info.choices.end(), value.asString());
            if (it == info.choices.end())
                return "unknown choice label '" + value.asString() + "'";
            if (static_cast<float> (it - info.choices.begin()) == info.defaultValue)
                return "default value stored (factory presets store only non-default values)";
            return {};
        }
        case Unit::Toggle:
            if (! value.isBool())
                return "toggle must be true/false";
            if ((value.asBool() ? 1.0f : 0.0f) == info.defaultValue)
                return "default value stored (factory presets store only non-default values)";
            return {};
        default:
        {
            if (! value.isNumber())
                return "number expected";
            const double v = value.asNumber();
            if (! std::isfinite (v) || v < info.minValue || v > info.maxValue)
                return "out of range [" + std::to_string (info.minValue) + ", " + std::to_string (info.maxValue) + "]";
            if (static_cast<float> (v) == info.defaultValue)
                return "default value stored (factory presets store only non-default values)";
            return {};
        }
    }
}

void fail (const FactoryFile& f, const std::string& what)
{
    reportFailure (__FILE__, __LINE__, f.path.filename().string() + ": " + what);
}
} // namespace

//==============================================================================
TEST_CASE ("Factory presets: library is complete, uniquely named and loadable")
{
    // Only a lower bound: the README tells contributors how to add presets, so
    // the library is expected to grow.
    const auto files = factoryFiles();
    REQUIRE (files.size() >= 20);

    std::set<std::string> names;
    std::set<std::string> categories;
    for (const auto& f : files)
    {
        preset::Preset p;
        std::string error;
        if (! preset::load (f.path.string(), p, error))
        {
            fail (f, "does not load: " + error);
            continue;
        }
        if (! names.insert (toLower (p.name)).second)
            fail (f, "duplicate preset name '" + p.name + "'");
        categories.insert (p.category);

        // "<category>-<slug>.json": lower-case, the category as the prefix.
        const std::string file = f.path.filename().string();
        if (file != toLower (file) || file.rfind (toLower (p.category) + "-", 0) != 0)
            fail (f, "file name must be <category>-<slug>.json in lower case");
    }
    CHECK (categories == (std::set<std::string> { "Device", "Gaming", "Music" }));

    // Every consumer (the app's BinaryData glob, the CLI, the install rule and
    // this file) reads the top level only, so a preset in a sub-folder would
    // silently never ship: keep the folder flat.
    std::error_code ec;
    for (fs::directory_iterator it (FLUB_PRESET_DIR, ec), end; ! ec && it != end; it.increment (ec))
        if (it->is_directory())
            reportFailure (__FILE__, __LINE__, "presets/factory must not have sub-folders: " + it->path().filename().string());
    CHECK (! ec);

    // Presets that code and docs refer to by name must exist.
    for (const char* required : { "Flubsound Signature", "Competitive FPS", "Bluetooth Headphones", "Tournament Clean",
                                  "7.1 Headphone Surround", "Cinematic Adventure", "Laptop Speakers" })
        if (names.count (toLower (required)) == 0)
            reportFailure (__FILE__, __LINE__, std::string ("missing factory preset '") + required + "'");

    #ifdef FLUB_DEVICE_PROFILES
    // Every preset a headset profile suggests must be a factory preset.
    std::ifstream in (FLUB_DEVICE_PROFILES, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    json::Value profiles;
    std::string error;
    REQUIRE (json::parse (ss.str(), profiles, error));
    for (const auto& profile : profiles["profiles"].asArray())
        for (const char* key : { "musicPreset", "gamingPreset" })
        {
            const std::string& suggested = profile[key].asString();
            if (! suggested.empty() && names.count (toLower (suggested)) == 0)
                reportFailure (__FILE__, __LINE__,
                               "device profile '" + profile["id"].asString() + "' suggests unknown preset '" + suggested + "'");
        }
    #endif
}

TEST_CASE ("Factory presets: metadata, keys, labels and output protection are valid")
{
    const auto& table = layout();
    std::set<std::string> uuids;
    for (const auto& f : factoryFiles())
    {
        json::Value root;
        std::string error;
        if (! json::parse (f.text, root, error))
        {
            fail (f, "invalid JSON: " + error);
            continue;
        }
        if (root["format"].asString() != "flubsound-preset" || root["version"].asNumber (0.0) != 1.0)
            fail (f, "format must be \"flubsound-preset\", version 1");

        preset::Preset p;
        if (! preset::fromJson (root, p, error))
        {
            fail (f, "rejected by preset::fromJson: " + error);
            continue;
        }

        // ---- Metadata ----
        if (p.name.empty() || p.description.empty())
            fail (f, "name and description must not be empty");
        if (p.category != "Music" && p.category != "Gaming" && p.category != "Device")
            fail (f, "category must be Music, Gaming or Device, not '" + p.category + "'");
        if (p.author != "Flubsound (Flubes & Claude)")
            fail (f, "author must be \"Flubsound (Flubes & Claude)\"");
        if (p.tags.empty() || root["tags"].asArray().size() != p.tags.size())
            fail (f, "tags must be a non-empty array of strings");
        // Stable identity (docs/11 E52): rules, hotkeys and packs refer to it.
        if (! preset::isValidUuid (p.uuid) || root["uuid"].asString() != p.uuid)
            fail (f, "needs a lower-case RFC 4122 \"uuid\" (python3 -c 'import uuid; print(uuid.uuid4())')");
        else if (! uuids.insert (p.uuid).second)
            fail (f, "uuid " + p.uuid + " is used by another factory preset");
        if (! p.warnings.empty())
            fail (f, "fromJson warning: " + p.warnings.front());

        // ---- Parameters: known keys, valid values, no duplicates or defaults ----
        const auto& params = root["params"];
        if (! params.isObject() || params.asObject().empty())
            fail (f, "params must be a non-empty object");
        std::set<std::string> seen;
        for (const auto& [key, value] : params.asObject())
        {
            const int id = findByKey (key);
            if (id < 0)
            {
                fail (f, "unknown parameter key '" + key + "'");
                continue;
            }
            if (! seen.insert (key).second)
                fail (f, "duplicate parameter key '" + key + "'");
            if (preset::isAppState (id))
                fail (f, key + " is app state and never part of a preset (use \"suggestedLatencyProfile\" for the profile)");
            const std::string problem = checkParamValue (table[static_cast<size_t> (id)], value);
            if (! problem.empty())
                fail (f, key + ": " + problem);
        }

        // ---- Mode matches the category ----
        const int mode = choiceIndex (p, Mode);
        if (p.category == "Gaming" && mode != static_cast<int> (ModeValue::Gaming))
            fail (f, "Gaming presets must set \"mode\": \"Gaming\"");
        if (p.category == "Music" && mode != static_cast<int> (ModeValue::Music))
            fail (f, "Music presets must use Music mode");

        // ---- Output protection ----
        const float ceiling = p.values[static_cast<size_t> (MaxCeilingDb)];
        const float maxCeiling = isBluetoothPreset (p) ? -2.0f : -1.0f;
        if (ceiling > maxCeiling)
            fail (f, "ceiling " + std::to_string (ceiling) + " dBTP is above " + std::to_string (maxCeiling) + " dBTP");
        if (p.values[static_cast<size_t> (MaximizerOn)] < 0.5f)
            fail (f, "the maximizer (true-peak limiter) must stay enabled");
        if (p.values[static_cast<size_t> (OutputGainDb)] > 0.0f)
            fail (f, "output gain is applied after the limiter and must be <= 0 dB");

        // ---- Suggested latency profile: metadata only, a valid label ----
        const auto& suggestion = root["suggestedLatencyProfile"];
        if (! suggestion.isNull()
            && (! suggestion.isString() || ! p.suggestedLatencyProfile || profileName (*p.suggestedLatencyProfile) != suggestion.asString()))
            fail (f, "suggestedLatencyProfile must be one of the latency.profile labels");
    }
}

TEST_CASE ("Factory presets: macros stack sanely and gaming presets keep positional cues")
{
    const auto& table = layout();
    for (const auto& f : factoryFiles())
    {
        preset::Preset p;
        std::string error;
        if (! preset::load (f.path.string(), p, error))
        {
            fail (f, "does not load: " + error);
            continue;
        }
        const auto value = [&p] (int id) { return p.values[static_cast<size_t> (id)]; };
        const bool gaming = choiceIndex (p, Mode) == static_cast<int> (ModeValue::Gaming);

        // ---- Macro stacking ----
        // Boost Intensity and the macros ADD to the base values. With all six
        // at 100 % (governor not yet reacting), the drive / bass / harmonic
        // parameters must not be pinned at the top of their range: a base
        // value that eats the macro headroom makes the top of the knob dead
        // and is the "bass 15 + harmonics 1 + drive 24" combination the
        // governor was never meant to rescue.
        std::vector<float> base (p.values), eff (static_cast<size_t> (kNumParams));
        for (int id : { BoostIntensity, Macro1, Macro2, Macro3, Macro4, Macro5 })
            base[static_cast<size_t> (id)] = 1.0f;
        MacroMap::apply (base.data(), eff.data(), 1.0f);
        for (int id : { BassBoostDb, BassHarmonics, MaxDriveDb, SatDriveDb })
        {
            const auto& info = table[static_cast<size_t> (id)];
            if (eff[static_cast<size_t> (id)] >= info.maxValue)
                fail (f, info.key + " is pinned at its maximum with Boost Intensity and all macros at 100 %");
        }
        // Fixed drive is never governed; loudness must come from Boost / Loudness.
        if (value (MaxDriveDb) > 0.0f)
            fail (f, "max.drive must stay 0 (fixed drive bypasses the SafetyGovernor)");

        // ---- Gaming: positional cues ----
        if (gaming)
        {
            // The chain forces crossfeed to 0 in Gaming mode; a stored value would
            // only show a misleading number in the UI.
            if (value (SpatialCrossfeed) > 0.0f)
                fail (f, "Gaming presets must not store headphone crossfeed");
            // Conservative width / ambience: decorrelation smears direction.
            if (value (SpatialWidth) > 1.25f || value (SpatialSpace) > 0.2f)
                fail (f, "Gaming presets must keep width <= 1.25 and space <= 0.2");
            // The compressor is switched on by Boost / Footsteps / Detail, so its
            // downward settings must be chosen on purpose, not left at 2.5:1 @ -18 dB.
            MacroMap::apply (p.values.data(), eff.data(), 1.0f);
            const json::Value stored = preset::toJson (p, false); // non-default values only
            const auto& params = stored["params"];
            if (eff[static_cast<size_t> (CompressorOn)] >= 0.5f && params["comp.ratio"].isNull() && params["comp.threshold"].isNull())
                fail (f, "the compressor is engaged but its ratio / threshold are left at the defaults");
        }

        // ---- Suggested latency profile matches the "low-latency" tag ----
        const bool lowLatencyTag = std::find (p.tags.begin(), p.tags.end(), "low-latency") != p.tags.end();
        const bool lowLatency = p.suggestedLatencyProfile == LatencyProfileValue::LowLatency;
        if (lowLatencyTag != lowLatency)
            fail (f, "the \"low-latency\" tag and a suggested Low Latency profile must go together");
        if (gaming && (contains (p.name, "competitive") || contains (p.name, "tournament")) && ! lowLatency)
            fail (f, "competitive / tournament presets must suggest the Low Latency profile");

        // ---- Small-speaker mode keeps the fundamentals its harmonics come from ----
        // The subsonic high-pass (bass engine stage 1) runs before the harmonic
        // generator (stage 4), and replaceFundamental removes everything below
        // the cutoff afterwards, so the harmonics are all that is left of the
        // lowest octave. A subsonic above the 20 Hz default removed it: for a
        // 30 Hz tone, Laptop Speakers at 40 Hz gave 9.2 dB less audible-band
        // (>= 120 Hz) energy than at 0 (docs/11 E03; KnownGap test in
        // test_known_gaps.cpp).
        if (value (BassReplaceFundamental) >= 0.5f && value (BassSubsonic) > 20.0f)
            fail (f, "with bass.replaceFundamental the subsonic filter must stay <= 20 Hz (it runs before the harmonics)");
    }
}

TEST_CASE ("Factory presets: loading one leaves the latency profile and bypass state alone and needs no re-prepare (E40)")
{
    // A strip on any profile, bypass engaged and matched bypass off (all
    // non-default), loads each preset through both store paths: the preset
    // path, and applyToStore, which the app's PresetManager still used when
    // the factory presets stopped carrying latency.profile.
    for (const auto& f : factoryFiles())
    {
        preset::Preset p;
        std::string error;
        if (! preset::load (f.path.string(), p, error))
        {
            fail (f, "does not load: " + error);
            continue;
        }
        for (const auto profile : kProfiles)
            for (const bool presetPath : { true, false })
            {
                ParameterStore store;
                store.set (Bank::A, LatencyProfile, static_cast<float> (static_cast<int> (profile)));
                store.set (Bank::A, BypassAll, 1.0f);
                store.set (Bank::A, LoudnessMatchBypass, 0.0f);
                ProcessingChain chain (store);
                chain.prepare ({ kFs, kBlockSize, 2 });

                if (presetPath)
                    preset::applyPresetToStore (p, store, Bank::A);
                else
                    preset::applyToStore (p, store, Bank::A);
                const std::string in = std::string (presetPath ? " (applyPresetToStore, " : " (applyToStore, ") + profileName (profile) + ")";
                if (store.get (Bank::A, LatencyProfile) != static_cast<float> (static_cast<int> (profile)))
                    fail (f, "loading changed latency.profile" + in);
                if (store.get (Bank::A, BypassAll) != 1.0f || store.get (Bank::A, LoudnessMatchBypass) != 0.0f)
                    fail (f, "loading changed bypass / bypass.matched" + in);
                if (chain.needsReprepare())
                    fail (f, "loading requires a re-prepare" + in);
            }
    }
}

TEST_CASE ("Factory presets: each renders a hot programme cleanly below its ceiling in Balanced, Low Latency and its suggested profile")
{
    const Planar stereo = makeProgramme (2);
    const Planar surround = makeProgramme (8);

    for (const auto& f : factoryFiles())
    {
        preset::Preset p;
        std::string error;
        if (! preset::load (f.path.string(), p, error))
        {
            fail (f, "does not load: " + error);
            continue;
        }

        // The profile is the user's choice (app state), not the preset's.
        for (const auto profile : renderProfiles (p))
        {
            const std::string in = " (" + profileName (profile) + ")";
            ParameterStore store;
            store.set (Bank::A, LatencyProfile, static_cast<float> (static_cast<int> (profile)));
            preset::applyPresetToStore (p, store, Bank::A);
            ProcessingChain chain (store);
            const int channels = isSurroundPreset (p) ? 8 : 2;
            chain.prepare ({ kFs, kBlockSize, channels });
            CHECK (! chain.needsReprepare());

            const double latencyMs = 1000.0 * chain.getLatencySamples() / kFs;
            if (latencyMs > kMaxLatencyMs[static_cast<int> (profile)])
                fail (f, "latency " + std::to_string (latencyMs) + " ms exceeds its profile's bound" + in);

            Planar buf = channels == 8 ? surround : stereo;
            {
                ScopedNoDenormals noDenormals;
                for (int pos = 0; pos < kRenderSamples; pos += kBlockSize)
                    chain.process (buf.block (pos, std::min (kBlockSize, kRenderSamples - pos)));
            }

            bool finite = true;
            for (const auto& c : buf.ch)
                finite = finite && std::all_of (c.begin(), c.end(), [] (float v) { return std::isfinite (v); });
            if (! finite)
            {
                fail (f, "non-finite output" + in);
                continue;
            }

            const float ceilingDb = p.values[static_cast<size_t> (MaxCeilingDb)];
            const double ceiling = dbToGain (ceilingDb);
            for (int c = 0; c < 2; ++c)
            {
                const double peak = peakAbs (buf.ch[static_cast<size_t> (c)].data(), kRenderSamples);
                if (peak > ceiling + 1.0e-6)
                    fail (f, "sample peak " + std::to_string (toDb (peak)) + " dBFS above the " + std::to_string (ceilingDb) + " dBTP ceiling" + in);
            }
            for (int c = 2; c < channels; ++c)
                if (peakAbs (buf.ch[static_cast<size_t> (c)].data(), kRenderSamples) != 0.0)
                    fail (f, "channels above the stereo pair must be cleared" + in);

            // True peak (4x interpolated) within the inter-sample tolerance.
            TruePeakMeter truePeak;
            truePeak.prepare (2);
            truePeak.process (buf.block().firstChannels (2));
            if (truePeak.getMaxDbAllChannels() > ceilingDb + kTruePeakToleranceDb)
                fail (f, "true peak " + std::to_string (truePeak.getMaxDbAllChannels()) + " dBTP above the ceiling" + in);

            if (chain.meters().safetyClipCount.load() != 0)
                fail (f, "the limiter's safety clamp engaged" + in);

            // The preset must not mute or gut the programme (last 2 s, after settling).
            const int tail = kRenderSamples / 2;
            if (rms (buf.ch[0].data() + tail, tail) < dbToGain (-40.0f))
                fail (f, "output is (nearly) silent" + in);
        }
    }
}

TEST_CASE ("Factory presets: Boost Intensity and all macros at 100 % stay safe")
{
    // Worst case a user can reach from any factory preset in two moves: every
    // macro turned fully up. Only the first 2 s are rendered - that is where
    // the SafetyGovernor has not reacted yet, so the limiter alone must hold.
    constexpr int kStressSamples = kRenderSamples / 2;
    const Planar stereo = makeProgramme (2);
    const Planar surround = makeProgramme (8);

    for (const auto& f : factoryFiles())
    {
        preset::Preset p;
        std::string error;
        if (! preset::load (f.path.string(), p, error))
        {
            fail (f, "does not load: " + error);
            continue;
        }

        for (const auto profile : renderProfiles (p))
        {
            const std::string in = " (" + profileName (profile) + ")";
            ParameterStore store;
            store.set (Bank::A, LatencyProfile, static_cast<float> (static_cast<int> (profile)));
            preset::applyPresetToStore (p, store, Bank::A);
            for (int id : { BoostIntensity, Macro1, Macro2, Macro3, Macro4, Macro5 })
                store.set (Bank::A, id, 1.0f);
            ProcessingChain chain (store);
            const int channels = isSurroundPreset (p) ? 8 : 2;
            chain.prepare ({ kFs, kBlockSize, channels });

            Planar buf = channels == 8 ? surround : stereo;
            {
                ScopedNoDenormals noDenormals;
                for (int pos = 0; pos < kStressSamples; pos += kBlockSize)
                    chain.process (buf.block (pos, std::min (kBlockSize, kStressSamples - pos)));
            }

            bool finite = true;
            for (int c = 0; c < 2; ++c)
                finite = finite && std::all_of (buf.ch[static_cast<size_t> (c)].begin(), buf.ch[static_cast<size_t> (c)].begin() + kStressSamples,
                                                [] (float v) { return std::isfinite (v); });
            if (! finite)
            {
                fail (f, "non-finite output at full macros" + in);
                continue;
            }

            const float ceilingDb = p.values[static_cast<size_t> (MaxCeilingDb)];
            for (int c = 0; c < 2; ++c)
            {
                const double peak = peakAbs (buf.ch[static_cast<size_t> (c)].data(), kStressSamples);
                if (peak > dbToGain (ceilingDb) + 1.0e-6)
                    fail (f, "sample peak " + std::to_string (toDb (peak)) + " dBFS above the ceiling at full macros" + in);
            }
            TruePeakMeter truePeak;
            truePeak.prepare (2);
            truePeak.process (buf.block (0, kStressSamples).firstChannels (2));
            if (truePeak.getMaxDbAllChannels() > ceilingDb + kTruePeakToleranceDb)
                fail (f, "true peak " + std::to_string (truePeak.getMaxDbAllChannels()) + " dBTP above the ceiling at full macros" + in);
            if (chain.meters().safetyClipCount.load() != 0)
                fail (f, "the limiter's safety clamp engaged at full macros" + in);
        }
    }
}

//==============================================================================
// Intent blocks (docs/11 E14 step 2; presets/README.md "Intent blocks")
//==============================================================================
//
// Every factory preset carries an "intent" object (preset::Intent): what it
// is meant to do to the sound, as bounds on measured renders. One case per
// preset renders it with the offline renderer (the CLI's renderPass: primed,
// latency compensated, 512-sample blocks, protection Off) in the profile it
// suggests (Balanced when it names none), as tools/scripts/preset-render-diff.py
// does, on programmes built like that script's:
//   * pink: the `flubsound-cli quality` pink (Kellet pink, seed 5959,
//     -18 dBFS RMS, mono on both channels), 4 s, measured over 1-4 s;
//   * music: the render diff's "music" programme (kicks, hats, 55 Hz bass,
//     440 / 660 Hz pad, about -17 LUFS) for 6 s, continued for 6 s more at
//     -8 dB (6-9 s) and -4 dB (9-12 s) with 10 ms ramps: its first 6 s are
//     the render diff's programme (tone, loudness), the whole 12 s give the
//     loudness range (about 5 LU) and the THD+N;
//   * game (Gaming presets): the render diff's "game-quiet" programme, a
//     -50 dBFS-RMS pink bed with a 40 ms 3.2 kHz step every 400 ms.
// Metrics (the tolerances are the block's own):
//   * tone: each octave band's level change (Analysis.h octaveBands, of
//     (L + R) / 2) re the input, minus the integrated-loudness change, so a
//     pure gain reads 0 everywhere; null bands are not asserted (the music
//     programme has next to nothing at 31.5 Hz and in the top octaves);
//   * loudnessOffsetLu: the integrated-loudness change;
//   * lraLossMaxLu: loudness range of the input minus that of the output
//     (EBU Tech 3342) on the 12 s music, at most this;
//   * thdnMaxDb: the deepest-reaching measured THD+N of saturator and
//     clipper (render.stats distortion.thdnMaxDb) on the 12 s music;
//   * stepBedContrastDb: in the 3.2 kHz band (RBJ band-pass, Q 1, of the
//     mid), (step power over the bed / bed power) out minus in: step windows
//     are the 40 ms steps, bed windows 200-360 ms after each onset (the
//     tests/test_known_gaps.cpp E19 contrast).
// FLUB_INTENT_PRINT=1 prints each preset's measured block as JSON, the way
// to write a block for a new preset or after a deliberate re-voicing.
namespace
{
constexpr int kIntentBlock = 512;

int intentSamples (double seconds) { return static_cast<int> (std::lround (seconds * kFs)); }

/** tools/scripts/preset-render-diff.py's XorShift32 (flub::FastRandom's sequence). */
class XorShift32
{
public:
    explicit XorShift32 (uint32_t seed) : rng (seed) {}
    double bipolar() { return static_cast<double> (rng.nextBipolar()); } // exact in float
private:
    FastRandom rng;
};

/** The script's pink(): Kellet's refined filter in double, scaled to rmsLevel. */
std::vector<double> scriptPink (int n, double rmsLevel, uint32_t seed)
{
    XorShift32 rng (seed);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, acc = 0;
    std::vector<double> out (static_cast<size_t> (n));
    for (auto& v : out)
    {
        const double w = rng.bipolar();
        b0 = 0.99886 * b0 + w * 0.0555179;
        b1 = 0.99332 * b1 + w * 0.0750759;
        b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856;
        b4 = 0.55000 * b4 + w * 0.5329522;
        b5 = -0.7616 * b5 - w * 0.0168980;
        v = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
        b6 = w * 0.115926;
        acc += v * v;
    }
    const double g = acc > 0.0 ? rmsLevel / std::sqrt (acc / n) : 0.0;
    for (auto& v : out)
        v *= g;
    return out;
}

/** RBJ band-pass, 0 dB peak (the script's band_pass). */
std::vector<double> bandPassD (const std::vector<double>& x, double f0, double q)
{
    const double w0 = kTwoPi * f0 / kFs, alpha = std::sin (w0) / (2.0 * q), a0 = 1.0 + alpha;
    const double b0 = alpha / a0, a1 = -2.0 * std::cos (w0) / a0, a2 = (1.0 - alpha) / a0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    std::vector<double> y (x.size());
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double o = b0 * (x[i] - x2) - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x[i];
        y2 = y1;
        y1 = o;
        y[i] = o;
    }
    return y;
}

io::AudioFileData fileOf (const std::vector<double>& left, const std::vector<double>& right)
{
    io::AudioFileData d;
    d.sampleRate = kFs;
    d.numChannels = 2;
    for (const auto* c : { &left, &right })
    {
        d.channels.emplace_back (c->size());
        std::transform (c->begin(), c->end(), d.channels.back().begin(), [] (double v) { return static_cast<float> (v); });
    }
    return d;
}

/** The render diff's "music" programme for `seconds` (programme_music). */
void scriptMusic (int n, std::vector<double>& left, std::vector<double>& right)
{
    XorShift32 rng (1234);
    left.assign (static_cast<size_t> (n), 0.0);
    right.assign (static_cast<size_t> (n), 0.0);
    constexpr double level = 0.25;
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kFs, beat = std::fmod (t, 0.5);
        const double k = std::exp (-beat * 18.0) * std::sin (kTwoPi * (50.0 + 80.0 * std::exp (-beat * 30.0)) * beat);
        const double hat = (std::fmod (t + 0.25, 0.5) < 0.03 ? 0.3 : 0.0) * rng.bipolar();
        const double bass = 0.4 * std::sin (kTwoPi * 55.0 * t);
        const double pad = 0.15 * std::sin (kTwoPi * 440.0 * t) + 0.1 * std::sin (kTwoPi * 660.0 * t + 0.3);
        left[static_cast<size_t> (i)] = level * (k + hat + bass + pad);
        right[static_cast<size_t> (i)] = level * (k + 0.8 * hat + bass + 0.7 * pad);
    }
}

/** Section gains (dB) of the 12 s dynamic music, from 6 s on, with 10 ms ramps. */
void applySections (std::vector<double>& left, std::vector<double>& right, const std::vector<std::pair<double, double>>& sections)
{
    const int ramp = intentSamples (0.010);
    double previousDb = 0.0;
    for (size_t s = 0; s < sections.size(); ++s)
    {
        const int from = intentSamples (sections[s].first);
        const int to = s + 1 < sections.size() ? intentSamples (sections[s + 1].first) : static_cast<int> (left.size());
        for (int i = from; i < to; ++i)
        {
            const double r = std::min (1.0, (i - from) / static_cast<double> (ramp));
            const double g = std::pow (10.0, (previousDb + r * (sections[s].second - previousDb)) / 20.0);
            left[static_cast<size_t> (i)] *= g;
            right[static_cast<size_t> (i)] *= g;
        }
        previousDb = sections[s].second;
    }
}

const io::AudioFileData& intentPink()
{
    static const io::AudioFileData pink = [] {
        // Commands.cpp's quality pink: FastRandom white noise through the same filter.
        const auto x = scriptPink (intentSamples (4.0), std::pow (10.0, -18.0 / 20.0), 5959);
        return fileOf (x, x);
    }();
    return pink;
}

const io::AudioFileData& intentMusic()
{
    static const io::AudioFileData music = [] {
        std::vector<double> left, right;
        scriptMusic (intentSamples (12.0), left, right);
        applySections (left, right, { { 6.0, -8.0 }, { 9.0, -4.0 } });
        return fileOf (left, right);
    }();
    return music;
}

/** The render diff's "game-quiet" programme and its step / bed windows. */
struct GameScene
{
    io::AudioFileData input;
    std::vector<std::pair<int, int>> steps, bed;
};

const GameScene& intentGame()
{
    static const GameScene scene = [] {
        const int n = intentSamples (6.0);
        auto x = scriptPink (n, std::pow (10.0, -50.0 / 20.0), 777);
        XorShift32 rng (4242);
        std::vector<double> noise (static_cast<size_t> (n));
        for (auto& v : noise)
            v = rng.bipolar();
        const auto band = bandPassD (noise, 3200.0, 1.0);
        double acc = 0.0;
        for (const double v : band)
            acc += v * v;
        const int length = intentSamples (0.040);
        const double g = std::pow (10.0, -56.0 / 20.0) / std::sqrt (acc / n) / std::sqrt (3.0 / 8.0);
        GameScene s;
        for (int onset = intentSamples (0.5); onset + length < n; onset += intentSamples (0.4))
        {
            for (int i = 0; i < length; ++i)
                x[static_cast<size_t> (onset + i)] += g * (0.5 - 0.5 * std::cos (kTwoPi * i / length)) * band[static_cast<size_t> (onset + i)];
            s.steps.push_back ({ onset, onset + length });
            if (onset + intentSamples (0.36) <= n)
                s.bed.push_back ({ onset + intentSamples (0.2), onset + intentSamples (0.36) });
        }
        s.input = fileOf (x, x);
        return s;
    }();
    return scene;
}

std::vector<std::vector<float>> slice (const std::vector<std::vector<float>>& c, int from, int to)
{
    std::vector<std::vector<float>> out;
    for (const auto& ch : c)
        out.emplace_back (ch.begin() + from, ch.begin() + to);
    return out;
}

/** The chain's values for a preset: as stored, in its suggested profile. */
std::vector<float> intentValues (const preset::Preset& p)
{
    auto values = p.values;
    values[static_cast<size_t> (LatencyProfile)] =
        static_cast<float> (static_cast<int> (p.suggestedLatencyProfile.value_or (LatencyProfileValue::Balanced)));
    return values;
}

bool renderIntent (const io::AudioFileData& input, const std::vector<float>& values, std::vector<std::vector<float>>& out,
                   cli::RenderStats* stats = nullptr)
{
    int latency = 0;
    std::string error;
    const bool ok = cli::renderPass (input, values, kIntentBlock, out, latency, error, nullptr, stats);
    if (! ok)
        reportFailure (__FILE__, __LINE__, "render failed: " + error);
    return ok;
}

/** Tone (band change minus loudness change) and loudness change of `out` re `in`. */
void toneOf (const std::vector<std::vector<float>>& in, const std::vector<std::vector<float>>& out, double& loudnessLu,
             std::array<std::optional<double>, preset::kIntentBandsHz.size()>& tone, bool assertAll)
{
    const auto inReport = cli::analyse (in, kFs), outReport = cli::analyse (out, kFs);
    loudnessLu = static_cast<double> (outReport.integratedLufs) - static_cast<double> (inReport.integratedLufs);
    const auto inBands = cli::octaveBands (in, kFs), outBands = cli::octaveBands (out, kFs);
    REQUIRE (inBands.size() == tone.size() && outBands.size() == tone.size());
    float loudest = -160.0f;
    for (const auto& b : inBands)
        loudest = std::max (loudest, b.levelDb);
    for (size_t b = 0; b < tone.size(); ++b)
    {
        // A band 40 dB under the programme's loudest carries next to nothing
        // of it: what the preset adds there (harmonics, noise) is not tone.
        if (! assertAll && inBands[b].levelDb < loudest - 40.0f)
            tone[b].reset();
        else
            tone[b] = static_cast<double> (outBands[b].levelDb) - static_cast<double> (inBands[b].levelDb) - loudnessLu;
    }
}

double meanPower (const std::vector<double>& x, const std::vector<std::pair<int, int>>& windows)
{
    double acc = 0.0;
    int count = 0;
    for (const auto& [from, to] : windows)
        for (int i = from; i < to; ++i, ++count)
            acc += x[static_cast<size_t> (i)] * x[static_cast<size_t> (i)];
    return count > 0 ? acc / count : 0.0;
}

double stepBedContrastChangeDb (const GameScene& s, const std::vector<std::vector<float>>& out)
{
    auto midBand = [] (const std::vector<std::vector<float>>& c) {
        std::vector<double> mid (c[0].size());
        for (size_t i = 0; i < mid.size(); ++i)
            mid[i] = 0.5 * (static_cast<double> (c[0][i]) + static_cast<double> (c[1][i]));
        return bandPassD (mid, 3200.0, 1.0);
    };
    const auto inBand = midBand (s.input.channels), outBand = midBand (out);
    const double inBed = meanPower (inBand, s.bed), outBed = meanPower (outBand, s.bed);
    const double inStep = std::max (meanPower (inBand, s.steps) - inBed, 1.0e-30);
    const double outStep = std::max (meanPower (outBand, s.steps) - outBed, 1.0e-30);
    return 10.0 * std::log10 ((outStep / std::max (outBed, 1.0e-30)) / (inStep / std::max (inBed, 1.0e-30)));
}

/** FLUB_INTENT_PRINT is set (and not "0"). */
bool intentPrintRequested()
{
    #if defined(_MSC_VER)
    char* value = nullptr;
    size_t length = 0;
    const bool set = _dupenv_s (&value, &length, "FLUB_INTENT_PRINT") == 0 && value != nullptr && value[0] != '\0' && value[0] != '0';
    std::free (value);
    return set;
    #else
    const char* value = std::getenv ("FLUB_INTENT_PRINT");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
    #endif
}

/** What an intent block describes, measured. */
struct IntentMeasurement
{
    std::array<std::optional<double>, preset::kIntentBandsHz.size()> tonePink {}, toneMusic {};
    double loudnessPinkLu = 0.0, loudnessMusicLu = 0.0;
    double lraInLu = 0.0, lraOutLu = 0.0;
    double thdnMaxDb = -160.0;
    std::optional<double> stepBedContrastDb;
};

bool measureIntent (const preset::Preset& p, IntentMeasurement& m)
{
    const auto values = intentValues (p);
    std::vector<std::vector<float>> out;

    // Pink, 1-4 s.
    const auto& pink = intentPink();
    if (! renderIntent (pink, values, out))
        return false;
    const int pinkFrom = intentSamples (1.0), pinkTo = static_cast<int> (pink.numFrames());
    toneOf (slice (pink.channels, pinkFrom, pinkTo), slice (out, pinkFrom, pinkTo), m.loudnessPinkLu, m.tonePink, true);

    // Music: tone and loudness over the first 6 s, range and THD+N over 12 s.
    const auto& music = intentMusic();
    cli::RenderStats stats;
    if (! renderIntent (music, values, out, &stats))
        return false;
    const int musicTo = intentSamples (6.0);
    toneOf (slice (music.channels, 0, musicTo), slice (out, 0, musicTo), m.loudnessMusicLu, m.toneMusic, false);
    m.lraInLu = static_cast<double> (cli::analyse (music.channels, kFs).loudnessRangeLu);
    m.lraOutLu = static_cast<double> (cli::analyse (out, kFs).loudnessRangeLu);
    m.thdnMaxDb = static_cast<double> (stats.distortionMaxDb);

    if (choiceIndex (p, Mode) == static_cast<int> (ModeValue::Gaming))
    {
        const auto& game = intentGame();
        if (! renderIntent (game.input, values, out))
            return false;
        m.stepBedContrastDb = stepBedContrastChangeDb (game, out);
    }
    return true;
}

/** "1.5", "-0.3", "0.0" (never "-0.0"). */
std::string oneDecimal (double v)
{
    char text[32];
    std::snprintf (text, sizeof (text), "%.1f", std::abs (v) < 0.05 ? 0.0 : v);
    return text;
}

/** The measured block as it is written in a preset file (two-space indent,
    after "params"), with this file's default tolerances: tone +-1 dB on pink
    and +-1.5 dB on music, loudness +-1 LU, the measured range loss + 0.5 LU,
    the measured THD+N + 3 dB (-100 dB when nothing was measured), contrast
    +-1 dB. FLUB_INTENT_PRINT=1 prints it. */
std::string measuredBlockText (const IntentMeasurement& m)
{
    auto bands = [] (const std::array<std::optional<double>, preset::kIntentBandsHz.size()>& tone) {
        std::string t = "[";
        for (size_t b = 0; b < tone.size(); ++b)
            t += (b > 0 ? ", " : "") + (tone[b] ? oneDecimal (*tone[b]) : std::string ("null"));
        return t + "]";
    };
    const double lraLoss = std::ceil ((m.lraInLu - m.lraOutLu + 0.5) * 10.0) / 10.0;
    const double thdn = m.thdnMaxDb < -100.0 ? -100.0 : std::ceil (m.thdnMaxDb + 3.0);
    std::string t = "  \"intent\": {\n    \"tone\": {\n";
    t += "      \"pink\": { \"db\": " + bands (m.tonePink) + ", \"toleranceDb\": 1.0 },\n";
    t += "      \"music\": { \"db\": " + bands (m.toneMusic) + ", \"toleranceDb\": 1.5 }\n    },\n";
    t += "    \"loudnessOffsetLu\": { \"pink\": " + oneDecimal (m.loudnessPinkLu) + ", \"music\": " + oneDecimal (m.loudnessMusicLu)
         + ", \"toleranceLu\": 1.0 },\n";
    t += "    \"lraLossMaxLu\": " + oneDecimal (lraLoss) + ",\n";
    t += "    \"thdnMaxDb\": " + std::to_string (static_cast<int> (thdn));
    if (m.stepBedContrastDb)
        t += ",\n    \"stepBedContrastDb\": { \"value\": " + oneDecimal (*m.stepBedContrastDb) + ", \"toleranceDb\": 1.0 }";
    return t + "\n  }";
}

void checkTone (const FactoryFile& f, const char* programme, const std::optional<preset::IntentTone>& want,
                const std::array<std::optional<double>, preset::kIntentBandsHz.size()>& got)
{
    if (! want)
    {
        fail (f, std::string ("intent.tone.") + programme + " is missing");
        return;
    }
    for (size_t b = 0; b < got.size(); ++b)
    {
        if (! want->db[b])
            continue;
        char what[160];
        if (! got[b])
            std::snprintf (what, sizeof (what), "intent.tone.%s asserts %g Hz, a band the programme does not carry", programme,
                           preset::kIntentBandsHz[b]);
        else if (std::abs (*got[b] - *want->db[b]) > want->toleranceDb)
            std::snprintf (what, sizeof (what), "intent.tone.%s at %g Hz: measured %+.2f dB, intent %+.2f +- %.2f dB", programme,
                           preset::kIntentBandsHz[b], *got[b], *want->db[b], want->toleranceDb);
        else
            continue;
        fail (f, what);
    }
}

void checkIntent (const FactoryFile& f)
{
    preset::Preset p;
    std::string error;
    if (! preset::load (f.path.string(), p, error))
    {
        fail (f, "does not load: " + error);
        return;
    }
    IntentMeasurement m;
    if (! measureIntent (p, m))
        return;
    if (intentPrintRequested())
        std::cout << "    measured intent of " << f.path.filename().string() << " (loudness range " << m.lraInLu << " -> " << m.lraOutLu
                  << " LU, THD+N " << m.thdnMaxDb << " dB):\n"
                  << measuredBlockText (m) << "\n";
    if (! p.intent)
    {
        fail (f, "has no valid \"intent\" block (FLUB_INTENT_PRINT=1 prints the measured one)");
        return;
    }
    const auto& want = *p.intent;
    checkTone (f, "pink", want.tonePink, m.tonePink);
    checkTone (f, "music", want.toneMusic, m.toneMusic);

    char what[200];
    auto checkLoudness = [&] (const char* programme, const std::optional<double>& intended, double measured) {
        if (! intended)
            fail (f, std::string ("intent.loudnessOffsetLu.") + programme + " is missing");
        else if (std::abs (measured - *intended) > want.loudnessToleranceLu)
        {
            std::snprintf (what, sizeof (what), "intent.loudnessOffsetLu.%s: measured %+.2f LU, intent %+.2f +- %.2f LU", programme,
                           measured, *intended, want.loudnessToleranceLu);
            fail (f, what);
        }
    };
    checkLoudness ("pink", want.loudnessOffsetPinkLu, m.loudnessPinkLu);
    checkLoudness ("music", want.loudnessOffsetMusicLu, m.loudnessMusicLu);

    if (! want.lraLossMaxLu)
        fail (f, "intent.lraLossMaxLu is missing");
    else if (m.lraInLu - m.lraOutLu > *want.lraLossMaxLu)
    {
        std::snprintf (what, sizeof (what), "intent.lraLossMaxLu: the music's range %.2f -> %.2f LU loses %.2f LU, more than %.2f LU",
                       m.lraInLu, m.lraOutLu, m.lraInLu - m.lraOutLu, *want.lraLossMaxLu);
        fail (f, what);
    }
    if (! want.thdnMaxDb)
        fail (f, "intent.thdnMaxDb is missing");
    else if (m.thdnMaxDb > *want.thdnMaxDb)
    {
        std::snprintf (what, sizeof (what), "intent.thdnMaxDb: measured THD+N %.1f dB on the music, above %.1f dB", m.thdnMaxDb, *want.thdnMaxDb);
        fail (f, what);
    }

    // Gaming presets state their step / bed contrast; the others have none to state.
    if (m.stepBedContrastDb.has_value() != want.stepBedContrastDb.has_value())
        fail (f, m.stepBedContrastDb ? "a Gaming preset needs intent.stepBedContrastDb" : "intent.stepBedContrastDb is for Gaming presets only");
    else if (m.stepBedContrastDb && std::abs (*m.stepBedContrastDb - *want.stepBedContrastDb) > want.stepBedToleranceDb)
    {
        std::snprintf (what, sizeof (what), "intent.stepBedContrastDb: measured %+.2f dB, intent %+.2f +- %.2f dB", *m.stepBedContrastDb,
                       *want.stepBedContrastDb, want.stepBedToleranceDb);
        fail (f, what);
    }
}

/** One case per factory preset ("Factory presets: intent - <category> - <name>"),
    so each stays under the suite's 2 s per case and a preset added to the
    folder is checked without a new case. */
bool registerIntentCases()
{
    for (const auto& f : factoryFiles())
    {
        json::Value root;
        std::string error;
        std::string label = f.path.stem().string();
        if (json::parse (f.text, root, error))
            label = root["category"].asString() + " - " + root["name"].asString();
        const auto path = f.path;
        const std::string name = "Factory presets: intent - " + label + " (docs/11 E14)";
        ::flubtest::Registrar (name.c_str(),
                               [path] {
                                   for (const auto& g : factoryFiles())
                                       if (g.path == path)
                                           checkIntent (g);
                               },
                               __FILE__, __LINE__);
    }
    return true;
}

[[maybe_unused]] const bool kIntentCasesRegistered = registerIntentCases();
} // namespace

TEST_CASE ("Factory presets: every preset carries a complete intent block (docs/11 E14 step 2)")
{
    int withIntent = 0;
    const auto files = factoryFiles();
    for (const auto& f : files)
    {
        preset::Preset p;
        std::string error;
        if (! preset::load (f.path.string(), p, error))
        {
            fail (f, "does not load: " + error);
            continue;
        }
        if (! p.intent)
        {
            fail (f, "has no valid \"intent\" block");
            continue;
        }
        const auto& i = *p.intent;
        const bool gaming = choiceIndex (p, Mode) == static_cast<int> (ModeValue::Gaming);
        if (! i.tonePink || ! i.toneMusic || ! i.loudnessOffsetPinkLu || ! i.loudnessOffsetMusicLu || ! i.lraLossMaxLu || ! i.thdnMaxDb
            || i.stepBedContrastDb.has_value() != gaming)
            fail (f, "the intent block must state tone (pink, music), loudnessOffsetLu (pink, music), lraLossMaxLu, thdnMaxDb and, for Gaming only, stepBedContrastDb");
        else
            ++withIntent;
        // Not part of the sound: the same parameters without the block hash the same.
        auto bare = p;
        bare.intent.reset();
        CHECK (preset::contentHash (bare) == preset::contentHash (p));
    }
    CHECK (withIntent == static_cast<int> (files.size()));
}


// ---- Classical & Jazz on a hot master (docs/11 E14 Done-when, E11) ----------
namespace
{
void measured (const std::string& name, double value, const char* unit)
{
    std::printf ("    measured %s: %.2f %s\n", name.c_str(), value, unit);
}

/** 16 s of the render diff's music in pop-like sections (0 / -6 / 0 / -4 dB,
    4 s each, 10 ms ramps; about 5 LU of loudness range), mastered as a hot
    pop master is: gain to -10.5 LUFS integrated through a true-peak limiter
    at -0.3 dBTP (flub::TruePeakLimiter, 1.5 ms look-ahead, 80 ms release),
    the gain found again after the limiter (docs/11 E11's "-10.54 LUFS pop
    master"; E11's hot masters are -10 to -9 LUFS at -0.3 dBTP). */
const io::AudioFileData& hotMaster()
{
    static const io::AudioFileData master = [] {
        std::vector<double> left, right;
        scriptMusic (intentSamples (16.0), left, right);
        applySections (left, right, { { 4.0, -6.0 }, { 8.0, 0.0 }, { 12.0, -4.0 } });
        const auto original = fileOf (left, right);
        auto c = original;
        double gainDb = -10.5 - static_cast<double> (cli::analyse (original.channels, kFs).integratedLufs);
        for (int pass = 0; pass < 6; ++pass)
        {
            c = original;
            const auto g = static_cast<float> (std::pow (10.0, gainDb / 20.0));
            for (auto& ch : c.channels)
                for (auto& v : ch)
                    v *= g;
            TruePeakLimiter limiter;
            limiter.setLookaheadMs (1.5f);
            limiter.setTruePeakDetection (true);
            limiter.prepare ({ kFs, kBlockSize, 2 });
            limiter.setParams ({ -0.3f, 80.0f, true });
            const int n = static_cast<int> (c.numFrames());
            for (int pos = 0; pos < n; pos += kBlockSize)
            {
                float* ptrs[2] = { c.channels[0].data() + pos, c.channels[1].data() + pos };
                limiter.process (AudioBlock (ptrs, 2, std::min (kBlockSize, n - pos)));
            }
            const double error = -10.5 - static_cast<double> (cli::analyse (c.channels, kFs).integratedLufs);
            if (std::abs (error) < 0.02)
                break;
            gainDb += error;
        }
        return c;
    }();
    return master;
}

/** Loudness range of Classical & Jazz Dynamic's render of the hot master
    (its suggested profile, Quality), with the automatic preamp as asked. */
double classicalHotMasterLraLu (bool autoPreamp, bool preampHot, cli::LoudnessReport* in = nullptr)
{
    preset::Preset p;
    std::string error;
    REQUIRE (preset::load ((fs::path (FLUB_PRESET_DIR) / "music-classical-jazz-dynamic.json").string(), p, error));
    auto values = intentValues (p);
    values[static_cast<size_t> (AutoPreampOn)] = autoPreamp ? 1.0f : 0.0f;
    values[static_cast<size_t> (AutoPreampHot)] = preampHot ? 1.0f : 0.0f;
    std::vector<std::vector<float>> out;
    cli::RenderStats stats;
    REQUIRE (renderIntent (hotMaster(), values, out, &stats));
    const auto report = cli::analyse (out, kFs);
    if (in != nullptr)
        *in = cli::analyse (hotMaster().channels, kFs);
    const std::string what = std::string ("Classical & Jazz, ") + (autoPreamp ? (preampHot ? "auto.preamp + auto.preampHot" : "auto.preamp") : "no preamp");
    measured (what + ": integrated", report.integratedLufs, "LUFS");
    measured (what + ": loudness range", report.loudnessRangeLu, "LU");
    measured (what + ": limiter deeper than 1 dB", stats.limiterOver1DbPercent, "%");
    measured (what + ": limiter mean GR", stats.limiterGrMeanDb, "dB");
    return static_cast<double> (report.loudnessRangeLu);
}
} // namespace

// KNOWN_GAP: Classical & Jazz loses <= 0.3 LU of the loudness range of a
// -10.5 LUFS master with E11's automatic preamp on, per docs/11 E14's
// Done-when. Measured (gcc 13, Release): the master (-10.51 LUFS, -0.35 dBTP,
// LRA 4.56 LU) loses 1.42 LU (4.56 -> 3.15 LU; -11.13 LUFS out, the limiter
// deeper than 1 dB 59 % of the time, mean GR -1.26 dB). Cause: the master's
// peaks sit 0.65 dB over the preset's -1 dBTP ceiling and the preset adds
// +1.6 LU (crossfeed and the 1.2 dB bass shelf on centred low end), so the
// limiter holds the loud sections down and not the quiet ones.
// `auto.preamp` takes off only the static boosts past its 1 dB allowance
// (about nothing here: 1.48 LU lost without it); `auto.preampHot` also takes
// back the allowance while the input's peaks leave no room, 0.97 LU (the
// last case), still over the row: it only reacts once the loud section's
// peaks arrive and releases at 1 dB/s. Closing it needs a preamp that knows
// the track's peak ahead (E11's intro / loud-section row, look-ahead or a
// learned per-track peak) or the preset's own headroom (a re-voicing).
TEST_CASE ("KnownGap: Classical & Jazz Dynamic on a -10.5 LUFS pop master with auto.preamp on loses 1.42 LU of loudness range (E14 row <= 0.3 LU)")
{
    cli::LoudnessReport in;
    const double lra = classicalHotMasterLraLu (true, false, &in);
    measured ("the -10.5 LUFS master: integrated", in.integratedLufs, "LUFS");
    measured ("the -10.5 LUFS master: true peak", in.truePeakDbtp, "dBTP");
    measured ("the -10.5 LUFS master: loudness range", in.loudnessRangeLu, "LU");
    measured ("Classical & Jazz, auto.preamp: loudness range lost", in.loudnessRangeLu - lra, "LU");
    CHECK_NEAR (in.integratedLufs, -10.5, 0.05);
    CHECK_NEAR (in.truePeakDbtp, -0.35, 0.1);
    CHECK_NEAR (in.loudnessRangeLu - lra, 1.42, 0.1); // KNOWN_GAP: target <= 0.3 LU per docs/11 E14
}

TEST_CASE ("KnownGap: Classical & Jazz Dynamic on the -10.5 LUFS pop master without a preamp loses 1.48 LU of loudness range (E14 / E11)")
{
    cli::LoudnessReport in;
    const double off = classicalHotMasterLraLu (false, false, &in);
    measured ("Classical & Jazz, no preamp: loudness range lost", in.loudnessRangeLu - off, "LU");
    CHECK_NEAR (in.loudnessRangeLu - off, 1.48, 0.1); // KNOWN_GAP: target <= 0.3 LU per docs/11 E14
}

TEST_CASE ("KnownGap: Classical & Jazz Dynamic on the -10.5 LUFS pop master with auto.preamp + auto.preampHot loses 0.97 LU of loudness range (E14 / E11)")
{
    cli::LoudnessReport in;
    const double hot = classicalHotMasterLraLu (true, true, &in);
    measured ("Classical & Jazz, auto.preamp + auto.preampHot: loudness range lost", in.loudnessRangeLu - hot, "LU");
    CHECK_NEAR (in.loudnessRangeLu - hot, 0.97, 0.1); // KNOWN_GAP: target <= 0.3 LU per docs/11 E14
}

#endif // FLUB_PRESET_DIR
