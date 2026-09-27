// Golden checks for presets (docs/11 E52 Phase A / B): nothing may change how
// a stored preset sounds without someone noticing.
//
//   * tests/golden/parameter-defaults.json: the default of every parameter.
//     Factory and user presets are sparse (a missing key means the default of
//     the file's schema major), so changing a default re-voices every preset
//     that leaves the key out. A changed default fails here until the schema
//     major is bumped with a migration that fills the old value into older
//     files (PresetIO.cpp: kV1Defaults, migrations()) and the file is
//     re-recorded. A new or removed parameter also asks for a re-record.
//   * tests/golden/factory-presets.json, per factory preset:
//       - uuid and contentHash (preset::contentHash of the loaded preset):
//         checked on every platform. Any change to a factory file, to a
//         default or to the parameter table shows up here, deterministically;
//       - a golden render ("Golden renders:" test): integrated LUFS and 1/3-
//         octave band levels (25 Hz .. 20 kHz) of a 3 s pink-noise-and-kick
//         programme rendered through the full chain (the CLI's offline
//         renderer, the preset's values, Balanced), within +-0.05 dB. Float
//         results differ slightly between compilers and CPUs, so the render
//         is compared on ONE reference platform only: Linux x86-64, gcc
//         Release (CI's core job, gcc leg, with FLUB_GOLDEN_REFERENCE=1).
//         Elsewhere the test renders nothing and passes.
//
// Re-recording (only for an intended change; say which in the change):
//     FLUB_GOLDEN_UPDATE=1 build/tests/flub_tests "Golden"
// on the reference platform rewrites both files in the source tree.
#include "TestFramework.h"
#include "TestSignals.h"

#ifdef FLUB_PRESET_DIR

    #include "OfflineRenderer.h"

    #include "flub/analysis/LoudnessMeter.h"
    #include "flub/dsp/Fft.h"
    #include "flub/io/Json.h"
    #include "flub/io/PresetIO.h"

    #include <algorithm>
    #include <cstdlib>
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

const std::string kGoldenDir = (fs::path (FLUB_PRESET_DIR) / ".." / ".." / "tests" / "golden").lexically_normal().string();
const std::string kDefaultsFile = (fs::path (kGoldenDir) / "parameter-defaults.json").string();
const std::string kPresetsFile = (fs::path (kGoldenDir) / "factory-presets.json").string();

constexpr double kFs = 48000.0;
constexpr int kProgrammeSamples = static_cast<int> (kFs * 3.0);
constexpr double kRenderToleranceDb = 0.05;
constexpr int kFftSize = 16384;

bool envSet (const char* name)
{
    #if defined(_MSC_VER)
    char* value = nullptr;
    size_t length = 0;
    const bool set = _dupenv_s (&value, &length, name) == 0 && value != nullptr && value[0] != '\0' && value[0] != '0';
    std::free (value);
    return set;
    #else
    const char* value = std::getenv (name);
    return value != nullptr && value[0] != '\0' && value[0] != '0';
    #endif
}

bool updating() { return envSet ("FLUB_GOLDEN_UPDATE"); }

bool readJson (const std::string& path, json::Value& out)
{
    std::ifstream in (path, std::ios::binary);
    if (! in)
        return false;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string error;
    return json::parse (ss.str(), out, error);
}

void writeJson (const std::string& path, const json::Value& v)
{
    std::ofstream out (path, std::ios::binary | std::ios::trunc);
    out << json::write (v, 2) << "\n";
    std::cout << "    [golden] wrote " << path << "\n";
}

double round3 (double v) { return std::round (v * 1000.0) / 1000.0; }

struct FactoryPreset
{
    std::string stem;
    preset::Preset preset;
};

std::vector<FactoryPreset> loadFactoryPresets()
{
    std::vector<FactoryPreset> presets;
    std::error_code ec;
    for (fs::directory_iterator it (FLUB_PRESET_DIR, ec), end; ! ec && it != end; it.increment (ec))
    {
        if (! it->is_regular_file() || it->path().extension() != ".json")
            continue;
        FactoryPreset f;
        f.stem = it->path().stem().string();
        std::string error;
        if (! preset::load (it->path().string(), f.preset, error))
        {
            reportFailure (__FILE__, __LINE__, f.stem + ": " + error);
            continue;
        }
        presets.push_back (std::move (f));
    }
    std::sort (presets.begin(), presets.end(), [] (const FactoryPreset& a, const FactoryPreset& b) { return a.stem < b.stem; });
    return presets;
}

/** 3 s stereo programme: pink noise (-22 dBFS RMS, mostly centred) for every
    band, under 55 Hz kicks (-6 dBFS peak, every 500 ms) that move the
    dynamics stages. */
io::AudioFileData goldenProgramme()
{
    io::AudioFileData in;
    in.sampleRate = kFs;
    in.numChannels = 2;
    const auto common = pinkNoise (kProgrammeSamples, 0.07f, 5201);
    const auto sideL = pinkNoise (kProgrammeSamples, 0.025f, 5202);
    const auto sideR = pinkNoise (kProgrammeSamples, 0.025f, 5203);
    in.channels.assign (2, std::vector<float> (static_cast<size_t> (kProgrammeSamples)));
    for (int i = 0; i < kProgrammeSamples; ++i)
    {
        const double beat = std::fmod (i / kFs, 0.5);
        const double kick = beat < 0.35 ? 0.5 * std::exp (-beat / 0.1) * std::sin (kTwoPi * 55.0 * beat) : 0.0;
        const auto s = static_cast<size_t> (i);
        in.channels[0][s] = common[s] + sideL[s] + static_cast<float> (kick);
        in.channels[1][s] = common[s] + sideR[s] + static_cast<float> (kick);
    }
    return in;
}

/** IEC 61260 1/3-octave centres (base-10 exact, nominal labels) 25 Hz .. 20 kHz. */
std::vector<double> thirdOctaveCentres()
{
    std::vector<double> centres;
    for (int n = 14; n <= 43; ++n)
        centres.push_back (std::pow (10.0, n / 10.0));
    return centres;
}

std::string bandLabel (double centre)
{
    // Nominal labels: 25, 31.5, 40, ... 1k -> "1000", 20000.
    static const double nominal[] = { 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630,
                                      800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000 };
    const auto* best = std::min_element (std::begin (nominal), std::end (nominal),
                                         [centre] (double a, double b) { return std::abs (a - centre) < std::abs (b - centre); });
    std::ostringstream os;
    os << *best;
    return os.str();
}

/** 1/3-octave band levels (dBFS, mean square of the mid signal (L + R) / 2)
    from a Hann-windowed Welch average, 50 % overlap. */
std::vector<double> thirdOctaveLevels (const std::vector<std::vector<float>>& stereo)
{
    Fft fft;
    fft.prepare (kFftSize);
    const size_t n = stereo[0].size();
    std::vector<double> window (kFftSize), power (kFftSize / 2 + 1, 0.0);
    double windowPower = 0.0;
    for (int i = 0; i < kFftSize; ++i)
    {
        window[static_cast<size_t> (i)] = 0.5 - 0.5 * std::cos (kTwoPi * i / kFftSize);
        windowPower += window[static_cast<size_t> (i)] * window[static_cast<size_t> (i)];
    }
    std::vector<float> frame (kFftSize);
    std::vector<Fft::Complex> bins (kFftSize / 2 + 1);
    int frames = 0;
    for (size_t start = 0; start + kFftSize <= n; start += kFftSize / 2)
    {
        for (size_t i = 0; i < static_cast<size_t> (kFftSize); ++i)
            frame[i] = static_cast<float> (0.5 * (stereo[0][start + i] + stereo[1][start + i]) * window[i]);
        fft.forwardReal (frame.data(), bins.data());
        for (size_t k = 0; k < bins.size(); ++k)
            power[k] += std::norm (bins[k]);
        ++frames;
    }
    // Parseval: mean square = sum over the one-sided spectrum / (N * sum w^2).
    const double scale = 1.0 / (static_cast<double> (frames) * kFftSize * windowPower);
    std::vector<double> levels;
    for (const double fc : thirdOctaveCentres())
    {
        const double lo = fc * std::pow (2.0, -1.0 / 6.0), hi = fc * std::pow (2.0, 1.0 / 6.0);
        double acc = 0.0;
        for (size_t k = 1; k + 1 < power.size(); ++k)
        {
            const double f = static_cast<double> (k) * kFs / kFftSize;
            if (f >= lo && f < hi)
                acc += 2.0 * power[k];
        }
        levels.push_back (10.0 * std::log10 (std::max (acc * scale, 1.0e-20)));
    }
    return levels;
}

json::Value renderMetrics (const preset::Preset& p, const io::AudioFileData& programme, std::string& error)
{
    std::vector<std::vector<float>> out;
    int latency = 0;
    if (! cli::renderPass (programme, p.values, 512, out, latency, error))
        return {};

    LoudnessMeter meter;
    meter.prepare (kFs, 2);
    std::vector<float*> pointers { out[0].data(), out[1].data() };
    meter.process (AudioBlock (pointers.data(), 2, static_cast<int> (out[0].size())));

    json::Value metrics { json::Value::Object {} };
    metrics.set ("integratedLufs", round3 (static_cast<double> (meter.getIntegratedLufs())));
    json::Value bands { json::Value::Object {} };
    const auto centres = thirdOctaveCentres();
    const auto levels = thirdOctaveLevels (out);
    for (size_t b = 0; b < centres.size(); ++b)
        bands.set (bandLabel (centres[b]), round3 (levels[b]));
    metrics.set ("thirdOctaveDb", std::move (bands));
    return metrics;
}

void fail (const std::string& what) { reportFailure (__FILE__, __LINE__, what); }
} // namespace

//==============================================================================
TEST_CASE ("Golden: every parameter default matches tests/golden/parameter-defaults.json (a changed default needs a schema major and a migration)")
{
    const auto& table = layout();
    if (updating())
    {
        json::Value root { json::Value::Object {} };
        root.set ("format", "flubsound-golden-parameter-defaults");
        root.set ("schemaMajor", preset::kSchemaVersion.majorVersion);
        json::Value defaults { json::Value::Object {} };
        for (const auto& info : table)
            defaults.set (info.key, static_cast<double> (info.defaultValue));
        root.set ("defaults", std::move (defaults));
        writeJson (kDefaultsFile, root);
        return;
    }

    json::Value golden;
    REQUIRE (readJson (kDefaultsFile, golden));
    const int recordedMajor = static_cast<int> (golden["schemaMajor"].asNumber (0.0));
    REQUIRE (recordedMajor >= 1);
    CHECK (recordedMajor <= preset::kSchemaVersion.majorVersion);

    std::set<std::string> recordedKeys;
    for (const auto& [key, value] : golden["defaults"].asObject())
    {
        recordedKeys.insert (key);
        const int id = findByKey (key);
        if (id < 0)
        {
            fail ("parameter \"" + key + "\" was removed: add a migration that maps it (alias) or drops it, then re-record "
                  "(FLUB_GOLDEN_UPDATE=1)");
            continue;
        }
        const float recorded = static_cast<float> (value.asNumber());
        const float now = table[static_cast<size_t> (id)].defaultValue;
        if (recorded == now)
            continue;
        if (recordedMajor == preset::kSchemaVersion.majorVersion)
        {
            fail ("default of \"" + key + "\" changed from " + std::to_string (recorded) + " to " + std::to_string (now)
                  + ": every sparse preset that omits it would be re-voiced. Bump the preset schema major (PresetIO.h "
                    "kSchemaVersion), freeze the old value in a kV" + std::to_string (recordedMajor) + "Defaults table with a "
                  + std::to_string (recordedMajor) + " -> " + std::to_string (recordedMajor + 1)
                  + " migration (PresetIO.cpp), then re-record (FLUB_GOLDEN_UPDATE=1)");
            continue;
        }
        // The major was bumped: the migration from the recorded major must fill the old value.
        const auto frozen = preset::frozenDefaults (recordedMajor);
        const auto it = std::find_if (frozen.begin(), frozen.end(), [&wanted = key] (const auto& d) { return d.first == wanted; });
        if (it == frozen.end() || it->second != recorded)
            fail ("default of \"" + key + "\" changed but frozenDefaults(" + std::to_string (recordedMajor)
                  + ") does not restore " + std::to_string (recorded) + " for older files");
    }
    if (recordedMajor != preset::kSchemaVersion.majorVersion)
        fail ("tests/golden/parameter-defaults.json was recorded for schema " + std::to_string (recordedMajor)
              + ": re-record it (FLUB_GOLDEN_UPDATE=1) once the migration is in place");
    for (const auto& info : table)
        if (recordedKeys.count (info.key) == 0)
            fail ("new parameter \"" + info.key + "\" is not recorded: re-record (FLUB_GOLDEN_UPDATE=1)");
}

TEST_CASE ("Golden: factory presets keep their uuid and contentHash (tests/golden/factory-presets.json)")
{
    const auto presets = loadFactoryPresets();
    REQUIRE (presets.size() >= 20);
    if (updating())
        return; // the render test below writes the file (uuid, hash and renders together)

    json::Value golden;
    REQUIRE (readJson (kPresetsFile, golden));
    const auto& recorded = golden["presets"];
    std::set<std::string> uuids;
    for (const auto& f : presets)
    {
        if (! preset::isValidUuid (f.preset.uuid))
            fail (f.stem + ": factory presets must carry a valid \"uuid\"");
        else if (! uuids.insert (f.preset.uuid).second)
            fail (f.stem + ": duplicate uuid " + f.preset.uuid);

        const auto& entry = recorded[f.stem];
        if (! entry.isObject())
        {
            fail (f.stem + ": not recorded in tests/golden/factory-presets.json: re-record (FLUB_GOLDEN_UPDATE=1)");
            continue;
        }
        if (entry["uuid"].asString() != f.preset.uuid)
            fail (f.stem + ": uuid changed from " + entry["uuid"].asString() + " (a factory preset's uuid is permanent)");
        const auto hash = preset::contentHash (f.preset);
        if (entry["contentHash"].asString() != hash)
            fail (f.stem + ": contentHash " + entry["contentHash"].asString() + " -> " + hash
                  + ": the resolved values changed (the file, a default or the parameter table). If intended, re-record "
                    "(FLUB_GOLDEN_UPDATE=1) and name the change");
    }
    CHECK (recorded.asObject().size() == presets.size());
}

TEST_CASE ("Golden renders: every factory preset renders within 0.05 dB of tests/golden/factory-presets.json (reference platform only)")
{
    if (! updating() && ! envSet ("FLUB_GOLDEN_REFERENCE"))
    {
        std::cout << "    [golden] skipped: set FLUB_GOLDEN_REFERENCE=1 on the reference platform (Linux x86-64, gcc Release)\n";
        return;
    }
    const auto presets = loadFactoryPresets();
    REQUIRE (presets.size() >= 20);
    const auto programme = goldenProgramme();

    if (updating())
    {
        json::Value root { json::Value::Object {} };
        root.set ("format", "flubsound-golden-factory-presets");
        root.set ("reference", "Linux x86-64, gcc Release; tests/test_presets_golden.cpp (3 s pink noise + 55 Hz kicks, Balanced)");
        json::Value entries { json::Value::Object {} };
        for (const auto& f : presets)
        {
            std::string error;
            json::Value entry { json::Value::Object {} };
            entry.set ("uuid", f.preset.uuid);
            entry.set ("contentHash", preset::contentHash (f.preset));
            entry.set ("render", renderMetrics (f.preset, programme, error));
            REQUIRE (error.empty());
            entries.set (f.stem, std::move (entry));
        }
        root.set ("presets", std::move (entries));
        writeJson (kPresetsFile, root);
        return;
    }

    json::Value golden;
    REQUIRE (readJson (kPresetsFile, golden));
    for (const auto& f : presets)
    {
        const auto& recorded = golden["presets"][f.stem]["render"];
        if (! recorded.isObject())
        {
            fail (f.stem + ": no golden render recorded");
            continue;
        }
        std::string error;
        const auto now = renderMetrics (f.preset, programme, error);
        if (! error.empty())
        {
            fail (f.stem + ": render failed: " + error);
            continue;
        }
        auto compare = [&] (const std::string& what, const json::Value& before, const json::Value& after) {
            const double b = before.asNumber (-999.0), a = after.asNumber (-999.0);
            if (! (std::abs (a - b) <= kRenderToleranceDb))
                fail (f.stem + ": " + what + " moved " + std::to_string (b) + " -> " + std::to_string (a) + " dB");
        };
        compare ("integrated LUFS", recorded["integratedLufs"], now["integratedLufs"]);
        for (const auto& [band, level] : now["thirdOctaveDb"].asObject())
            compare (band + " Hz band", recorded["thirdOctaveDb"][band], level);
    }
}

#endif
