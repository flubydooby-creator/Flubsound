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
#include "flub/io/FilePath.h"
#include "flub/io/WavFile.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
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

TEST_CASE ("CLI demo: every pair written, matched unless a level feature, index band deltas as analyze, deterministic")
{
    // The listed pairs: every macro of both modes, Boost 50 / 100 in both
    // modes, the module switches and the four maximizer styles.
    const auto specs = demoPairs ({});
    std::vector<std::string> slugs;
    for (const auto& s : specs)
        slugs.push_back (s.slug);
    const std::vector<std::string> expected {
        "music-punch", "music-width", "music-clarity", "music-loudness", "music-warmth", "gaming-footsteps", "gaming-positional",
        "gaming-impact", "gaming-detail", "gaming-voice-score", "music-boost-50", "music-boost-100", "gaming-boost-50",
        "gaming-boost-100", "smoothness", "crossfeed", "virtualiser", "contour", "startle-guard", "night", "max-style-transparent",
        "max-style-punchy", "max-style-aggressive", "max-style-safe"
    };
    CHECK (slugs == expected);
    for (const auto& s : specs)
        CHECK (s.levelFeature == (s.slug == "music-loudness" || s.slug == "startle-guard" || s.slug == "night"));

    DemoTempDir a ("a"), b ("b");
    DemoOptions o;
    o.seconds = 0.5;
    o.outDir = a.utf8();
    o.jobs = 2;
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

TEST_CASE ("CLI demo: --input renders every pair on the user's file (the virtualiser keeps the 7.1 scene for stereo)")
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
    DemoResult r;
    REQUIRE (makeDemoPack (o, r, error));
    REQUIRE (! r.pairs.empty());
    for (const auto& p : r.pairs)
    {
        if (p.spec.slug == "virtualiser")
            CHECK (p.programmeUsed.rfind ("game-7.1", 0) == 0);
        else
            CHECK (p.programmeUsed == "your file (my song.wav)");
        CHECK_NEAR (p.afterReport.sampleRate, p.spec.slug == "virtualiser" ? 48000.0 : 44100.0, 0.0);
    }
    CHECK (r.notes.size() == 1);
    CHECK (! r.notes.empty() && r.notes[0].find ("7.1") != std::string::npos);
    checkPack (dir.path / "pack", r);

    // A file that cannot be read is an error, not an empty pack.
    o.input = io::pathToUtf8 (dir.path / "missing.wav");
    DemoResult missing;
    CHECK (! makeDemoPack (o, missing, error));
    CHECK (! error.empty());
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
