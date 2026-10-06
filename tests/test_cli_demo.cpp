// `flubsound-cli demo` (tools/flubsound-cli/Demo.h): the by-ear demo pack.
// On short programmes (0.5 s): every listed pair is written; a pair is
// loudness-matched within 0.5 LU unless index.txt calls it a level feature;
// the index's band deltas are what `analyze --bands` reads from the two files
// (within 0.1 dB); two runs with different worker counts write identical
// files; --input replaces the built-in programmes.
#include "TestFramework.h"

#include "Analysis.h"
#include "CliOptions.h"
#include "Demo.h"

#include "flub/common/Math.h"
#include "flub/engine/MacroMap.h"
#include "flub/io/FilePath.h"
#include "flub/io/WavFile.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace flub;
using namespace flub::cli;

namespace
{
namespace fs = std::filesystem;

/** A fresh folder below the system temp path, removed on destruction. */
struct DemoTempDir
{
    explicit DemoTempDir (const std::string& name)
    {
        static std::atomic<int> counter { 0 };
        for (int attempt = 0;; ++attempt)
        {
            const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
            path = fs::temp_directory_path()
                   / ("flub_test_cli_demo_" + name + "_" + std::to_string (ticks) + "_" + std::to_string (std::random_device {}()) + "_"
                      + std::to_string (++counter));
            std::error_code ec;
            if (fs::create_directory (path, ec))
                break;
            REQUIRE (attempt < 100);
        }
    }

    ~DemoTempDir()
    {
        std::error_code ec;
        fs::remove_all (path, ec);
    }

    DemoTempDir (const DemoTempDir&) = delete;
    DemoTempDir& operator= (const DemoTempDir&) = delete;

    std::string utf8() const { return io::pathToUtf8 (path); }

    fs::path path;
};

std::string readText (const fs::path& path)
{
    std::ifstream in (path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

/** One pair as index.txt lists it. */
struct IndexEntry
{
    std::string title, beforeFile, afterFile, programme, level;
    std::vector<std::string> bandDelta; // the value tokens ("+0.3", "n/a")
};

std::string field (const std::string& line, const char* name)
{
    const std::string prefix = std::string ("  ") + name;
    if (line.rfind (prefix, 0) != 0)
        return {};
    const auto colon = line.find (": ");
    return colon == std::string::npos ? std::string {} : line.substr (colon + 2);
}

std::vector<IndexEntry> parseIndex (const std::string& text)
{
    std::vector<IndexEntry> entries;
    std::istringstream in (text);
    std::string line;
    while (std::getline (in, line))
    {
        if (line.size() > 5 && line[0] == '[' && line[3] == ']')
        {
            entries.push_back ({});
            entries.back().title = line.substr (5);
            continue;
        }
        if (entries.empty())
            continue;
        auto& e = entries.back();
        if (const auto files = field (line, "Files"); ! files.empty())
        {
            const auto bar = files.find (" | ");
            e.beforeFile = files.substr (0, bar);
            e.afterFile = bar == std::string::npos ? std::string {} : files.substr (bar + 3);
        }
        else if (const auto programme = field (line, "Programme"); ! programme.empty())
            e.programme = programme;
        else if (const auto level = field (line, "Level"); ! level.empty())
            e.level = level;
        else if (const auto delta = field (line, "Band delta"); ! delta.empty())
        {
            std::istringstream words (delta);
            std::string label, value;
            while (words >> label >> value)
                e.bandDelta.push_back (value);
        }
    }
    return entries;
}

struct Measured
{
    LoudnessReport report;
    std::vector<BandLevel> bands;
};

bool measureFile (const fs::path& path, Measured& m)
{
    io::AudioFileData d;
    std::string error;
    if (! io::readWav (io::pathToUtf8 (path), d, error))
        return false;
    m.report = analyse (d.channels, d.sampleRate);
    m.bands = octaveBands (d.channels, d.sampleRate);
    return true;
}

/** The pack's pairs checked against index.txt and the files themselves. */
void checkPack (const fs::path& dir, const DemoResult& result)
{
    const auto entries = parseIndex (readText (dir / "index.txt"));
    REQUIRE (entries.size() == result.pairs.size());
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const auto& e = entries[i];
        const auto& spec = result.pairs[i].spec;
        CHECK (e.title == spec.title);
        CHECK (e.beforeFile == result.pairs[i].beforeFile);
        CHECK (e.afterFile == result.pairs[i].afterFile);
        Measured before, after;
        REQUIRE (measureFile (dir / e.beforeFile, before));
        REQUIRE (measureFile (dir / e.afterFile, after));
        REQUIRE (before.report.integratedLufs > kMinusInfDb);
        REQUIRE (after.report.integratedLufs > kMinusInfDb);

        // Loudness-matched within 0.5 LU, unless the index calls it a level feature.
        if (spec.levelFeature)
            CHECK (e.level.rfind ("not matched (a level feature)", 0) == 0);
        else
        {
            CHECK (e.level.rfind ("matched", 0) == 0);
            if (std::abs (after.report.integratedLufs - before.report.integratedLufs) > 0.5f)
                std::cerr << "    " << spec.slug << ": " << before.report.integratedLufs << " vs " << after.report.integratedLufs << " LUFS\n";
            CHECK_LE (std::abs (after.report.integratedLufs - before.report.integratedLufs), 0.5);
        }

        // Band deltas: after minus before, as `analyze --bands` reads the two files.
        REQUIRE (e.bandDelta.size() == before.bands.size());
        REQUIRE (after.bands.size() == before.bands.size());
        for (size_t b = 0; b < before.bands.size(); ++b)
        {
            if (before.bands[b].levelDb <= kMinusInfDb || after.bands[b].levelDb <= kMinusInfDb)
            {
                CHECK (e.bandDelta[b] == "n/a");
                continue;
            }
            REQUIRE (e.bandDelta[b] != "n/a");
            CHECK_NEAR (std::stod (e.bandDelta[b]), after.bands[b].levelDb - before.bands[b].levelDb, 0.1);
        }
    }
}
} // namespace

// The pair list: every macro of both modes, Boost 50 / 100 in both modes,
// the module switches and the four maximizer styles (batch 3), then the
// batch 4 - 5 features, the module cards, the genre and voice presets and
// the app's own settings (docs/12-feature-guide.md names a pair for each).
namespace
{
const std::vector<std::string> kFirstPairs {
    "music-punch", "music-width", "music-clarity", "music-loudness", "music-warmth", "gaming-footsteps", "gaming-positional",
    "gaming-impact", "gaming-detail", "gaming-voice-score", "music-boost-50", "music-boost-100", "gaming-boost-50",
    "gaming-boost-100", "smoothness", "crossfeed", "virtualiser", "contour", "startle-guard", "night", "max-style-transparent",
    "max-style-punchy", "max-style-aggressive", "max-style-safe"
};
const std::vector<std::string> kChainPairs {
    "music-punch-boost-100", "gaming-impact-boost-100", "attack-low", "attack-high", "relative-presence", "crossfeed-meier",
    "crossfeed-mono-safe", "enhanced-renderer", "virt-front-back", "preset-rock-metal", "preset-orchestral-film",
    "preset-acoustic-singer-songwriter", "preset-rnb-vocal", "preset-electronic-ambient", "preset-synthwave", "preset-late-night",
    "preset-podcast-voice", "preset-voice-chat", "noise-gate", "eq-bell", "dynamic-eq", "bass-boost", "bass-harmonics", "bass-tighten", "saturation",
    "tape-grit", "compressor", "auto-preamp", "latency-profile", "protection-normal"
};
const std::vector<std::string> kEnginePairs { "smart-macros", "onboard-cap",  "safe-speaker-cap", "device-correction",
                                              "per-ear",      "hearing-cap", "chat-duck",        "chatmix" };
const std::vector<std::string> kLevelFeatures { "music-loudness",       "startle-guard",   "night",       "preset-late-night",
                                                "preset-podcast-voice", "preset-voice-chat", "hearing-cap", "chat-duck",
                                                "chatmix" };

bool contains (const std::vector<std::string>& list, const std::string& s)
{
    return std::find (list.begin(), list.end(), s) != list.end();
}
} // namespace

TEST_CASE ("CLI demo: the pair list - every feature pair, level features, app settings only on engine pairs")
{
    const auto specs = demoPairs ({});
    std::vector<std::string> slugs, expected = kFirstPairs;
    expected.insert (expected.end(), kChainPairs.begin(), kChainPairs.end());
    expected.insert (expected.end(), kEnginePairs.begin(), kEnginePairs.end());
    for (const auto& s : specs)
        slugs.push_back (s.slug);
    CHECK (slugs == expected);
    for (const auto& s : specs)
    {
        CHECK (s.levelFeature == contains (kLevelFeatures, s.slug));
        // Both sides of an engine pair go through the mix engine; no other pair sets an app setting but protection.
        const bool engine = contains (kEnginePairs, s.slug);
        CHECK (s.beforeHost.engine == engine);
        CHECK (s.afterHost.engine == engine);
        if (! engine)
        {
            CHECK (s.beforeHost.describe().empty());
            CHECK (s.afterHost.describe().empty() == (s.slug != "protection-normal"));
        }
        else
            CHECK (s.beforeHost.describe() != s.afterHost.describe());
        CHECK (s.beforePreset.empty());
        CHECK (s.afterPreset.empty() == (s.slug.rfind ("preset-", 0) != 0));
    }
}

TEST_CASE ("CLI demo: every pair written, matched unless a level feature, index band deltas as analyze, deterministic")
{
    DemoTempDir a ("a"), b ("b");
    DemoOptions o;
    o.seconds = 0.5;
    o.outDir = a.utf8();
    o.jobs = 2;
    o.only = kFirstPairs; // the rest in the cases below (each case under 2 s)
    const auto& expected = kFirstPairs;
    DemoResult ra;
    std::string error;
    REQUIRE (makeDemoPack (o, ra, error));
    CHECK (error.empty());
    REQUIRE (ra.pairs.size() == expected.size());
    CHECK (ra.renders < static_cast<int> (2 * expected.size())); // shared sides are rendered once
    CHECK (ra.notes.empty());
    // The virtualiser pair runs on the 7.1 scene; Night copies Night Mode Gaming's dynamics.
    for (const auto& p : ra.pairs)
        if (p.spec.slug == "virtualiser")
            CHECK (p.programmeUsed.rfind ("game-7.1", 0) == 0);
        else if (p.spec.slug == "night")
            CHECK (std::any_of (p.spec.after.begin(), p.spec.after.end(),
                                [] (const ParamSetting& s) { return s.key == "autolevel.target" && s.value == "-14"; }));
    checkPack (a.path, ra);

    // Deterministic, whatever the number of workers.
    o.outDir = b.utf8();
    o.jobs = 3;
    DemoResult rb;
    REQUIRE (makeDemoPack (o, rb, error));
    size_t files = 0;
    for (const auto& entry : fs::directory_iterator (a.path))
    {
        ++files;
        CHECK (readText (entry.path()) == readText (b.path / entry.path().filename()));
    }
    CHECK (files == 2 * expected.size() + 1);
}

TEST_CASE ("CLI demo: the chain pairs - presets, module cards and the batch 4 - 5 keys, matched as the first ones")
{
    DemoTempDir dir ("chain");
    DemoOptions o;
    o.seconds = 0.5;
    o.outDir = dir.utf8();
    o.jobs = 2;
    o.only = kChainPairs;
    DemoResult r;
    std::string error;
    REQUIRE (makeDemoPack (o, r, error));
    REQUIRE (r.pairs.size() == kChainPairs.size());
    CHECK (r.notes.empty());
    for (const auto& p : r.pairs)
    {
        CHECK (p.beforeEngine.empty());
        CHECK (p.afterEngine.empty());
        if (p.spec.slug == "noise-gate")
            CHECK (p.programmeUsed.rfind ("speech-hiss", 0) == 0);
        else if (p.spec.slug == "enhanced-renderer" || p.spec.slug == "virt-front-back")
            CHECK (p.programmeUsed.rfind ("game-7.1", 0) == 0);
        else if (p.spec.slug == "protection-normal")
            CHECK (p.afterStats.governorStrength == static_cast<int> (ProtectionStrength::Normal));
    }
    checkPack (dir.path, r);
    // A preset side is named in the index as `process` takes it.
    const auto index = readText (dir.path / "index.txt");
    CHECK (index.find ("After      : --preset music-rock-metal\n") != std::string::npos);

    o.only = { "no-such-pair" };
    DemoResult unknown;
    CHECK (! makeDemoPack (o, unknown, error));
    CHECK (error.find ("no-such-pair") != std::string::npos);
}

namespace
{
/** One channel's octave bands of a pack file. */
std::vector<BandLevel> channelBands (const fs::path& path, size_t channel)
{
    io::AudioFileData d;
    std::string error;
    REQUIRE (io::readWav (io::pathToUtf8 (path), d, error));
    return octaveBands ({ d.channels[channel] }, d.sampleRate);
}

float bandAt (const std::vector<BandLevel>& bands, float hz)
{
    for (const auto& b : bands)
        if (std::abs (b.centreHz - hz) < 0.1f * hz)
            return b.levelDb;
    return kMinusInfDb;
}

/** The engine pairs `only` rendered on 2 s programmes (Smart's state is
    valid after 0.5 s; the chat voice talks from 0.5 to 1.5 s). */
struct EnginePack
{
    explicit EnginePack (const char* name, std::vector<std::string> only) : dir (name)
    {
        DemoOptions o;
        o.seconds = 2.0;
        o.outDir = dir.utf8();
        o.jobs = 2;
        o.format = io::SampleFormat::Float32;
        o.only = std::move (only);
        std::string error;
        REQUIRE (makeDemoPack (o, result, error));
        REQUIRE (result.pairs.size() == o.only.size());
        checkPack (dir.path, result);
        for (const auto& p : result.pairs)
        {
            bySlug[p.spec.slug] = &p;
            CHECK (! p.beforeEngine.empty());
            CHECK (! p.afterEngine.empty());
            CHECK_NEAR (p.afterReport.sampleRate, 48000.0, 0.0);
        }
    }

    bool has (const char* slug, bool after, const char* text) const
    {
        const auto* p = bySlug.at (slug);
        return (after ? p->afterEngine : p->beforeEngine).find (text) != std::string::npos;
    }

    float level (const char* slug, bool after) const
    {
        const auto* p = bySlug.at (slug);
        return (after ? p->afterReport : p->beforeReport).integratedLufs;
    }

    DemoTempDir dir;
    DemoResult result;
    std::map<std::string, const DemoPairResult*> bySlug;
};
} // namespace

TEST_CASE ("CLI demo: the app's chain settings through the mix engine - Smart, the two caps, the correction")
{
    const EnginePack pack ("engine1", { "smart-macros", "onboard-cap", "safe-speaker-cap", "device-correction" });
    auto has = [&pack] (const char* slug, bool after, const char* text) { return pack.has (slug, after, text); };
    const auto& bySlug = pack.bySlug;

    // Smart on the loud master takes attack back; off leaves the multipliers at 1.
    CHECK (has ("smart-macros", false, "attack 1.00, drive 1.00"));
    CHECK (! has ("smart-macros", true, "attack 1.00"));
    // The headset enhancement cap shows CAPPED; the safe speaker cap holds the bass lift at +3 dB.
    CHECK (has ("onboard-cap", false, "CAPPED off"));
    CHECK (has ("onboard-cap", true, "CAPPED on"));
    CHECK (has ("safe-speaker-cap", false, "bass boost as applied +14.0 dB"));
    CHECK (has ("safe-speaker-cap", true, "bass boost as applied +3.0 dB"));
    // The headphone correction's example curve: more low end, less at 2 - 4 kHz, re the rest.
    {
        const auto* p = bySlug.at ("device-correction");
        CHECK (p->afterBands[1].levelDb - p->beforeBands[1].levelDb > p->afterBands[7].levelDb - p->beforeBands[7].levelDb + 3.0f);
    }
}

TEST_CASE ("CLI demo: the app's mix settings through the mix engine - the per-ear profile, the hearing cap, the duck, ChatMix")
{
    const EnginePack pack ("engine2", { "per-ear", "hearing-cap", "chat-duck", "chatmix" });
    auto has = [&pack] (const char* slug, bool after, const char* text) { return pack.has (slug, after, text); };
    auto level = [&pack] (const char* slug, bool after) { return pack.level (slug, after); };
    const auto& bySlug = pack.bySlug;
    const auto& dir = pack.dir;

    // The per-ear profile: the right ear brighter than the left at 8 kHz, by far more than before.
    {
        const auto* p = bySlug.at ("per-ear");
        const auto beforeL = channelBands (dir.path / p->beforeFile, 0), beforeR = channelBands (dir.path / p->beforeFile, 1);
        const auto afterL = channelBands (dir.path / p->afterFile, 0), afterR = channelBands (dir.path / p->afterFile, 1);
        const float before = bandAt (beforeR, 8000.0f) - bandAt (beforeL, 8000.0f), after = bandAt (afterR, 8000.0f) - bandAt (afterL, 8000.0f);
        CHECK_NEAR (before, 0.0, 0.5);
        CHECK (after - before > 6.0f);
        CHECK (has ("per-ear", true, "reservation -"));
    }
    // The hearing guard: an estimate on both sides; the cap takes the level down.
    CHECK (has ("hearing-cap", false, "estimate: loudest 5 s"));
    CHECK (has ("hearing-cap", true, "cap gain min -"));
    CHECK (level ("hearing-cap", true) < level ("hearing-cap", false) - 3.0f);
    // The duck acts while the voice talks; ChatMix towards Chat turns the game (its low end: no voice there) down 6 dB.
    CHECK (has ("chat-duck", false, "duck depth max 0 %"));
    CHECK (has ("chat-duck", true, "duck depth max 100 %"));
    CHECK (! has ("chat-duck", true, "voice held 0 %"));
    {
        const auto* p = bySlug.at ("chatmix");
        CHECK_NEAR (p->afterBands[1].levelDb - p->beforeBands[1].levelDb, -6.02, 0.3); // 63 Hz
    }
}

TEST_CASE ("CLI demo: --input renders the pairs on the user's file (the virtualiser keeps the 7.1 scene for stereo, chat its scene)")
{
    DemoTempDir dir ("input");
    io::AudioFileData song;
    song.sampleRate = 44100.0;
    song.numChannels = 2;
    song.channels.assign (2, std::vector<float> (22050));
    for (size_t i = 0; i < song.channels[0].size(); ++i)
    {
        const double t = static_cast<double> (i) / song.sampleRate;
        song.channels[0][i] = static_cast<float> (0.3 * std::sin (kTwoPi * 110.0 * t) + 0.1 * std::sin (kTwoPi * 3300.0 * t));
        song.channels[1][i] = static_cast<float> (0.3 * std::sin (kTwoPi * 110.0 * t + 0.4) + 0.1 * std::sin (kTwoPi * 2200.0 * t));
    }
    const std::string input = io::pathToUtf8 (dir.path / "my song.wav");
    std::string error;
    REQUIRE (io::writeWav (input, song, io::SampleFormat::Float32, error));

    DemoOptions o;
    o.input = input;
    o.outDir = io::pathToUtf8 (dir.path / "pack");
    o.seconds = 0.5;
    o.jobs = 2;
    o.format = io::SampleFormat::Float32;
    o.only = { "music-punch", "virtualiser", "smart-macros", "preset-rock-metal", "noise-gate", "chat-duck", "night" };
    DemoResult r;
    REQUIRE (makeDemoPack (o, r, error));
    REQUIRE (r.pairs.size() == o.only.size());
    for (const auto& p : r.pairs)
    {
        const bool builtIn = p.spec.slug == "virtualiser" || p.spec.slug == "chat-duck";
        if (p.spec.slug == "virtualiser")
            CHECK (p.programmeUsed.rfind ("game-7.1", 0) == 0);
        else if (p.spec.slug == "chat-duck")
            CHECK (p.programmeUsed.rfind ("chat-scene", 0) == 0);
        else
            CHECK (p.programmeUsed == "your file (my song.wav)");
        CHECK_NEAR (p.afterReport.sampleRate, builtIn ? 48000.0 : 44100.0, 0.0);
    }
    REQUIRE (r.notes.size() == 2);
    CHECK (r.notes[0].find ("7.1") != std::string::npos);
    CHECK (r.notes[1].find ("chat pairs") != std::string::npos);
    checkPack (dir.path / "pack", r);

    // A file that cannot be read is an error, not an empty pack.
    o.input = io::pathToUtf8 (dir.path / "missing.wav");
    DemoResult missing;
    CHECK (! makeDemoPack (o, missing, error));
    CHECK (! error.empty());
}

TEST_CASE ("CLI demo: the feature guide (docs/12) names an existing pair in every entry and covers every pair, macro, card, page and command")
{
    // docs/ is next to presets/factory in the source tree.
    const fs::path guidePath = fs::path (FLUB_PRESET_DIR).parent_path().parent_path() / "docs" / "12-feature-guide.md";
    const std::string guide = readText (guidePath);
    REQUIRE (! guide.empty());

    std::vector<std::string> slugs;
    for (const auto& s : demoPairs ({}))
        slugs.push_back (s.slug);
    auto backticked = [] (const std::string& text) {
        std::vector<std::string> tokens;
        for (size_t open = text.find ('`'); open != std::string::npos; open = text.find ('`', open + 1))
        {
            const size_t close = text.find ('`', open + 1);
            if (close == std::string::npos)
                break;
            tokens.push_back (text.substr (open + 1, close - open - 1));
            open = close;
        }
        return tokens;
    };

    // Every entry (a "### " heading) has exactly one "Demo pairs" line: existing
    // pairs in backticks, or "none (why)" for a control without a sound of its own.
    std::istringstream in (guide);
    std::string line, entry;
    std::vector<std::string> headings, named, checklist;
    std::map<std::string, int> demoLines;
    bool inChecklist = false;
    const std::string label = "- **Demo pairs:** ";
    while (std::getline (in, line))
    {
        if (line.rfind ("## ", 0) == 0)
        {
            inChecklist = line.rfind ("## 12.", 0) == 0;
            entry.clear();
        }
        else if (line.rfind ("### ", 0) == 0)
        {
            entry = line.substr (4);
            headings.push_back (entry);
            demoLines[entry] = 0;
        }
        else if (line.rfind (label, 0) == 0)
        {
            REQUIRE (! entry.empty());
            ++demoLines[entry];
            const std::string rest = line.substr (label.size());
            const auto tokens = backticked (rest);
            if (rest.rfind ("none (", 0) == 0)
                CHECK (tokens.empty());
            else
            {
                CHECK (! tokens.empty());
                for (const auto& t : tokens)
                {
                    if (! contains (slugs, t))
                        std::cerr << "    " << entry << ": unknown demo pair `" << t << "`\n";
                    CHECK (contains (slugs, t));
                    named.push_back (t);
                }
            }
        }
        else if (inChecklist && line.rfind ("| ", 0) == 0 && line.rfind ("| #", 0) != 0 && line.rfind ("|---", 0) != 0)
        {
            // | # | Feature | Demo pair | ...: an existing pair, or "—" for an app-only row.
            size_t column = 0; // the third column starts after the third bar
            for (int bar = 0; bar < 3 && column != std::string::npos; ++bar)
                column = line.find ('|', bar == 0 ? 0 : column + 1);
            REQUIRE (column != std::string::npos);
            const std::string cell = line.substr (column + 1, line.find ('|', column + 1) - column - 1);
            const auto tokens = backticked (cell);
            if (! tokens.empty())
            {
                REQUIRE (tokens.size() == 1);
                CHECK (contains (slugs, tokens[0]));
                checklist.push_back (tokens[0]);
            }
        }
    }
    REQUIRE (headings.size() > 60);
    for (const auto& [heading, count] : demoLines)
    {
        if (count != 1)
            std::cerr << "    " << heading << ": " << count << " demo-pair lines\n";
        CHECK (count == 1);
    }
    // Every pair is named by an entry and has a checklist row.
    for (const auto& s : slugs)
    {
        if (! contains (named, s) || ! contains (checklist, s))
            std::cerr << "    pair " << s << " is not in the guide's entries or checklist\n";
        CHECK (contains (named, s));
        CHECK (contains (checklist, s));
    }

    // Every macro of both modes, every module card, every Settings page and
    // every CLI command has its own entry.
    auto hasHeading = [&headings] (const std::string& text) {
        return std::any_of (headings.begin(), headings.end(), [&text] (const std::string& h) { return h.find (text) != std::string::npos; });
    };
    std::vector<std::string> required { "Boost Intensity", "Dynamic Range", "Smoothness", "Smart macros" };
    for (int mode = 0; mode < 2; ++mode)
        for (int i = 0; i < 5; ++i)
            required.push_back (MacroMap::macroName (static_cast<param::ModeValue> (mode), i));
    for (const char* card : { "Noise Gate card", "Parametric EQ card", "Dynamic EQ card", "Bass Engine card", "Clarity card",
                              "Saturation card", "Stereo & Space card", "Headphone Virtualizer card", "Compressor card",
                              "Loudness Maximizer card", "Loudness contour", "Protection strength", "Automatic preamp" })
        required.push_back (card);
    for (const char* page : { "Audio", "Correction", "Processing", "Hearing", "Hotkeys", "General", "Diagnostics" })
        required.push_back (std::string ("Settings › ") + page);
    for (const char* command : { "`flubsound-cli process`", "`batch`", "`flubsound-cli analyze`", "`flubsound-cli quality`", "`soak`",
                                 "`flubsound-cli demo`", "`flubsound-cli latency-probe`", "`flubsound-cli params`", "`presets`", "`help`",
                                 "`--version`", "`flubsound-cli ctl`" })
        required.push_back (command);
    for (const auto& r : required)
    {
        if (! hasHeading (r))
            std::cerr << "    no guide entry for " << r << "\n";
        CHECK (hasHeading (r));
    }
}

TEST_CASE ("CLI demo: command line")
{
    CliOptions o;
    std::string error;
    REQUIRE (parseCommandLine ({ "demo", "--input", "song.wav", "--out", "pack", "--seconds", "4", "--jobs", "2" }, o, error));
    CHECK (o.command == Command::Demo);
    CHECK (o.input == "song.wav");
    CHECK (o.output == "pack");
    CHECK_NEAR (o.demoSeconds, 4.0, 0.0);
    CHECK (o.jobs == 2);
    CHECK (! o.demoFormatSet);

    REQUIRE (parseCommandLine ({ "demo", "song.wav", "pack", "--format", "f32" }, o, error));
    CHECK (o.input == "song.wav");
    CHECK (o.output == "pack");
    CHECK (o.demoFormatSet);
    CHECK (o.render.format == io::SampleFormat::Float32);

    REQUIRE (parseCommandLine ({ "demo" }, o, error));
    CHECK (o.input.empty());
    CHECK_NEAR (o.demoSeconds, 10.0, 0.0);

    CHECK (! parseCommandLine ({ "demo", "--seconds", "0.1" }, o, error));
    CHECK (error.find ("--seconds") != std::string::npos);
    CHECK (! parseCommandLine ({ "demo", "--boost", "50" }, o, error)); // the pairs set the chain
    CHECK (! parseCommandLine ({ "process", "--out", "x.wav" }, o, error));
}
