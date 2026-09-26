// Factory preset validation (presets/factory/*.json).
//
// Every factory preset must:
//   * parse, carry complete metadata (name, category, author, description,
//     tags) and live in a file named "<category>-<slug>.json",
//   * use only known parameter keys, valid choice labels, true/false toggles
//     and in-range numbers, and store ONLY non-default values,
//   * keep the product's output protection: maximizer engaged, output gain
//     <= 0 dB (it is applied after the limiter), ceiling <= -1 dBTP
//     (<= -2 dBTP for the Bluetooth device preset),
//   * render a hot drum/bass/pad programme through the full ProcessingChain
//     (prepared AFTER loading, so the latency profile is honoured) with
//     finite output, sample peaks below the ceiling and no safety clips.
//
// FLUB_PRESET_DIR (and FLUB_DEVICE_PROFILES) are compile definitions from
// tests/CMakeLists.txt; builds without them compile this file to nothing.
#include "TestFramework.h"
#include "TestSignals.h"

#ifdef FLUB_PRESET_DIR

    #include "flub/analysis/PeakMeters.h"
    #include "flub/common/Denormals.h"
    #include "flub/engine/ProcessingChain.h"
    #include "flub/io/Json.h"
    #include "flub/io/PresetIO.h"

    #include <algorithm>
    #include <cctype>
    #include <filesystem>
    #include <fstream>
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
    const auto files = factoryFiles();
    REQUIRE (files.size() >= 20);
    CHECK (files.size() <= 24);

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
        if (p.author != "Flubsound")
            fail (f, "author must be \"Flubsound\"");
        if (p.tags.empty() || root["tags"].asArray().size() != p.tags.size())
            fail (f, "tags must be a non-empty array of strings");

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
        if (p.values[static_cast<size_t> (BypassAll)] >= 0.5f)
            fail (f, "a factory preset must not load in global bypass");
    }
}

TEST_CASE ("Factory presets: each renders a hot programme cleanly below its ceiling")
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

        ParameterStore store;
        preset::applyToStore (p, store, Bank::A);
        ProcessingChain chain (store);
        const int channels = isSurroundPreset (p) ? 8 : 2;
        chain.prepare ({ kFs, kBlockSize, channels }); // after loading: the latency profile is structural
        CHECK (! chain.needsReprepare());

        const int profile = std::clamp (choiceIndex (p, LatencyProfile), 0, 2);
        const double latencyMs = 1000.0 * chain.getLatencySamples() / kFs;
        if (latencyMs > kMaxLatencyMs[profile])
            fail (f, "latency " + std::to_string (latencyMs) + " ms exceeds its profile's bound");

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
            fail (f, "non-finite output");
            continue;
        }

        const float ceilingDb = p.values[static_cast<size_t> (MaxCeilingDb)];
        const double ceiling = dbToGain (ceilingDb);
        for (int c = 0; c < 2; ++c)
        {
            const double peak = peakAbs (buf.ch[static_cast<size_t> (c)].data(), kRenderSamples);
            if (peak > ceiling + 1.0e-6)
                fail (f, "sample peak " + std::to_string (toDb (peak)) + " dBFS above the " + std::to_string (ceilingDb) + " dBTP ceiling");
        }
        for (int c = 2; c < channels; ++c)
            if (peakAbs (buf.ch[static_cast<size_t> (c)].data(), kRenderSamples) != 0.0)
                fail (f, "channels above the stereo pair must be cleared");

        // True peak (4x interpolated) within the inter-sample tolerance.
        TruePeakMeter truePeak;
        truePeak.prepare (2);
        truePeak.process (buf.block().firstChannels (2));
        if (truePeak.getMaxDbAllChannels() > ceilingDb + kTruePeakToleranceDb)
            fail (f, "true peak " + std::to_string (truePeak.getMaxDbAllChannels()) + " dBTP above the ceiling");

        if (chain.meters().safetyClipCount.load() != 0)
            fail (f, "the limiter's safety clamp engaged");

        // The preset must not mute or gut the programme (last 2 s, after settling).
        const int tail = kRenderSamples / 2;
        if (rms (buf.ch[0].data() + tail, tail) < dbToGain (-40.0f))
            fail (f, "output is (nearly) silent");
    }
}

#endif // FLUB_PRESET_DIR
