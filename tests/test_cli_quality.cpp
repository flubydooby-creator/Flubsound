// Sound-quality targets and the KNOWN_GAP ratchet (docs/11 E59):
// tests/quality_targets.json names settings (`flubsound-cli quality` chain
// options) and rows (a metric path into the `quality --json` object, its
// target and, for a known gap, the value recorded per compiler). Here the
// per-PR rows are measured with the same code the CLI runs (Commands.h
// measureQuality / measureHygiene) and checked: a row without `recorded`
// must meet its target, a known gap must not be worse than its recorded
// value by more than its margin. tools/scripts/quality-report.py evaluates
// every row (the nightly profile x rate matrix) through the CLI with the
// same rules.
#include "TestFramework.h"
#include "TestSignals.h"

#include "Analysis.h"
#include "CliOptions.h"
#include "Commands.h"
#include "OfflineRenderer.h"

#include "flub/common/Math.h"
#include "flub/io/Json.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace flub;
using namespace flub::cli;

namespace
{
const char* compilerKey() noexcept
{
#if defined(_MSC_VER) && ! defined(__clang__)
    return "msvc";
#elif defined(__apple_build_version__)
    return "appleclang";
#elif defined(__clang__)
    return "clang";
#elif defined(__GNUC__)
    return "gcc";
#else
    return "other";
#endif
}

json::Value loadTargets()
{
    const std::string path = std::string (FLUB_PRESET_DIR) + "/../../tests/quality_targets.json";
    std::ifstream in (path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    json::Value v;
    std::string error;
    const bool ok = in.good() || in.eof();
    if (! ok || ! json::parse (text.str(), v, error))
        std::cerr << "    cannot read " << path << ": " << error << "\n";
    REQUIRE (v.isObject());
    return v;
}

/** One row of the targets file at one profile / rate. */
struct Row
{
    std::string id, setting, profile, metric, op, tier;
    double rate = 48000.0, target = 0.0, margin = 0.0;
    std::optional<double> recorded; // for this compiler (margin already widened for a fallback)
};

std::vector<Row> expandRows (const json::Value& targets)
{
    std::vector<Row> rows;
    for (const auto& r : targets["rows"].asArray())
    {
        std::vector<std::string> profiles;
        for (const auto& p : r["profiles"].asArray())
            profiles.push_back (p.asString());
        if (profiles.empty())
            profiles.push_back ("");
        std::vector<double> rates;
        for (const auto& x : r["rates"].asArray())
            rates.push_back (x.asNumber());
        if (rates.empty())
            rates.push_back (48000.0);
        for (const auto& profile : profiles)
            for (double rate : rates)
            {
                Row row;
                row.id = r["id"].asString();
                row.setting = r["setting"].asString();
                row.profile = profile;
                row.metric = r["metric"].asString();
                row.op = r["op"].asString();
                row.tier = r["tier"].asString();
                row.rate = rate;
                row.target = r["target"].asNumber();
                if (! profile.empty() && r["byProfile"][profile].isNumber())
                    row.target = r["byProfile"][profile].asNumber();
                row.margin = r["margin"].asNumber (0.5);
                const auto& recorded = r["recorded"];
                if (recorded[compilerKey()].isNumber())
                    row.recorded = recorded[compilerKey()].asNumber();
                else if (recorded["gcc"].isNumber())
                {
                    row.recorded = recorded["gcc"].asNumber(); // the reference compiler, twice the margin
                    row.margin *= 2.0;
                }
                rows.push_back (row);
            }
    }
    return rows;
}

/** A dotted path into a JSON object; numeric parts index arrays. */
std::optional<double> metricAt (const json::Value& root, const std::string& path)
{
    const json::Value* v = &root;
    std::stringstream parts (path);
    std::string part;
    while (std::getline (parts, part, '.'))
    {
        if (! part.empty() && part.find_first_not_of ("0123456789") == std::string::npos)
        {
            const auto& a = v->asArray();
            const size_t i = static_cast<size_t> (std::stoul (part));
            if (i >= a.size())
                return std::nullopt;
            v = &a[i];
        }
        else
            v = &(*v)[part];
    }
    return v->isNumber() ? std::optional<double> (v->asNumber()) : std::nullopt;
}

struct Verdict
{
    bool ok = false, metTarget = false;
};

Verdict evaluate (const Row& row, double value)
{
    const bool lowerIsBetter = row.op == "<=";
    Verdict v;
    v.metTarget = lowerIsBetter ? value <= row.target : value >= row.target;
    if (v.metTarget)
        v.ok = true;
    else if (row.recorded)
        v.ok = lowerIsBetter ? value <= *row.recorded + row.margin : value >= *row.recorded - row.margin;
    return v;
}

/** The `quality` options of a setting (and a row's profile) resolved to parameter values. */
CliOptions settingOptions (const json::Value& targets, const std::string& setting, const std::string& profile)
{
    std::vector<std::string> args { "quality" };
    for (const auto& a : targets["settings"][setting]["args"].asArray())
        args.push_back (a.asString());
    if (! profile.empty())
        args.insert (args.end(), { "--profile", profile });
    CliOptions o;
    std::string error;
    const bool ok = parseCommandLine (args, o, error);
    if (! ok)
        std::cerr << "    setting '" << setting << "': " << error << "\n";
    REQUIRE (ok);
    return o;
}

std::vector<float> valuesOf (const CliOptions& o)
{
    ResolvedParameters p;
    std::string error;
    REQUIRE (buildParameters (o.render, p, error));
    return p.values;
}

bool isHygiene (const Row& r) { return r.metric.rfind ("hygiene.", 0) == 0; }

/** Checks every per-PR row of (setting, profile, rate) against `quality` (a
    qualityToJson object, with "hygiene" when those rows need it); returns
    the number of rows checked. */
int checkRows (const std::vector<Row>& rows, const std::string& setting, const std::string& profile, double rate, const json::Value& quality)
{
    int n = 0;
    for (const auto& r : rows)
    {
        if (r.tier != "per-pr" || r.setting != setting || r.profile != profile || (isHygiene (r) && r.rate != rate))
            continue;
        const auto value = metricAt (quality, r.metric);
        REQUIRE (value.has_value());
        const auto v = evaluate (r, *value);
        char buf[256];
        std::snprintf (buf, sizeof (buf), "    measured %s = %.2f (target %s %.2f%s)\n", r.id.c_str(), *value, r.op.c_str(), r.target,
                       r.recorded ? (v.metTarget ? "; KNOWN_GAP met: drop `recorded`" : "; KNOWN_GAP, not worse than recorded") : "");
        std::cout << buf;
        if (! v.ok)
            std::cerr << "    " << r.id << ": " << *value << " fails " << r.op << " " << r.target
                      << (r.recorded ? " and is worse than the recorded " + std::to_string (*r.recorded) : std::string()) << "\n";
        CHECK (v.ok);
        ++n;
    }
    return n;
}

/** A value just past what a row accepts (worse than its target, or than its recorded value + margin). */
double regressed (const Row& r)
{
    const double sign = r.op == "<=" ? 1.0 : -1.0;
    return (r.recorded ? *r.recorded + sign * r.margin : r.target) + sign * 0.1;
}

// The (setting, profile, rate) combinations the test cases below measure.
struct Combo
{
    const char* setting;
    const char* profile;
    double rate;
};
constexpr Combo kPerPrCombos[] = { { "maximizer-12", "", 48000.0 }, { "music-boost-100", "", 48000.0 }, { "warmth-100", "balanced", 44100.0 } };
} // namespace

TEST_CASE ("Quality targets: tests/quality_targets.json is well-formed, every setting parses as `quality` options, every per-PR row is measured below")
{
    const auto targets = loadTargets();
    CHECK (targets["format"].asString() == "flubsound-quality-targets");
    CHECK (targets["version"].asNumber() == 1.0);
    const auto rows = expandRows (targets);
    CHECK (rows.size() >= 20u);
    for (const auto& r : rows)
    {
        CHECK (! r.id.empty());
        CHECK (targets["settings"][r.setting]["args"].isArray());
        CHECK (r.op == "<=" || r.op == ">=");
        CHECK (r.tier == "per-pr" || r.tier == "nightly");
        CHECK (r.margin >= (r.recorded ? 0.5 : 0.0)); // ratchet margins (>= 0.5 dB) cover compilers, not tuning
        settingOptions (targets, r.setting, r.profile);
        if (r.tier != "per-pr")
            continue;
        bool covered = false;
        for (const auto& c : kPerPrCombos)
            covered = covered || (r.setting == c.setting && r.profile == c.profile && (! isHygiene (r) || r.rate == c.rate));
        if (! covered)
            std::cerr << "    per-PR row " << r.id << " has no test case (add one, or make it nightly)\n";
        CHECK (covered);
    }

    // The ratchet itself: a value just past what a row accepts fails it,
    // one just inside passes. A known gap that meets its target passes.
    for (const auto& r : rows)
    {
        CHECK (! evaluate (r, regressed (r)).ok);
        const double sign = r.op == "<=" ? 1.0 : -1.0;
        CHECK (evaluate (r, (r.recorded ? *r.recorded + sign * r.margin : r.target) - sign * 0.01).ok);
        CHECK (evaluate (r, r.target - sign * 0.01).metTarget);
    }
}

TEST_CASE ("Quality ratchet: the maximizer at 12 dB drive meets its targets and no known gap got worse (per-PR rows)")
{
    const auto targets = loadTargets();
    const auto rows = expandRows (targets);
    QualityReport q;
    std::string error;
    REQUIRE (measureQuality (valuesOf (settingOptions (targets, "maximizer-12", "")), 512, q, error));
    CHECK (checkRows (rows, "maximizer-12", "", 48000.0, qualityToJson (q)) >= 5);
}

TEST_CASE ("Quality ratchet: Music Boost 100 meets its targets and no known gap got worse (per-PR rows)")
{
    const auto targets = loadTargets();
    const auto rows = expandRows (targets);
    QualityReport q;
    std::string error;
    REQUIRE (measureQuality (valuesOf (settingOptions (targets, "music-boost-100", "")), 512, q, error));
    CHECK (checkRows (rows, "music-boost-100", "", 48000.0, qualityToJson (q)) >= 5);
}

TEST_CASE ("Quality targets: Warmth 100 hygiene at 44.1 kHz, Balanced (per-PR rows)")
{
    const auto targets = loadTargets();
    const auto rows = expandRows (targets);
    const auto o = settingOptions (targets, "warmth-100", "balanced");
    HygieneReport h;
    std::string error;
    REQUIRE (measureHygiene (valuesOf (o), 44100.0, 512, h, error));
    json::Value quality;
    quality.set ("hygiene", hygieneToJson (h));
    CHECK (checkRows (rows, "warmth-100", "balanced", 44100.0, quality) >= 1);
}

TEST_CASE ("Quality ratchet: a deliberately regressed render (a 3 ms delay injected into the maximizer's output) fails the kick-centroid row")
{
    const auto targets = loadTargets();
    const auto rows = expandRows (targets);
    QualityReport q;
    std::string error;
    REQUIRE (measureQuality (valuesOf (settingOptions (targets, "maximizer-12", "")), 512, q, error, [] (std::vector<std::vector<float>>& out) {
        const size_t d = 144; // 3 ms at 48 kHz
        for (auto& c : out)
        {
            c.insert (c.begin(), d, 0.0f);
            c.resize (c.size() - d);
        }
    }));
    const auto json = qualityToJson (q);
    int failed = 0;
    for (const auto& r : rows)
        if (r.id == "E59.max12.kickCentroid")
        {
            const auto value = metricAt (json, r.metric);
            REQUIRE (value.has_value());
            std::cout << "    measured regressed kick centroid = " << *value << " ms\n";
            failed += evaluate (r, *value).ok ? 0 : 1;
        }
    CHECK (failed == 1);
}

TEST_CASE ("Quality hygiene metrics read injected artefacts at their injected values (meta-validation)")
{
    // worstAliasDbc: a sine on its odd bin plus a line at -80 dBc on a bin
    // that is no harmonic's; dcDbfs of a -60 dBFS offset; powerShareAboveDb
    // of a 1 kHz tone and a 30 kHz tone 40 dB down at 96 kHz.
    constexpr int n = 65536;
    constexpr double fs = 48000.0;
    const int bin = aliasToneBin (1000.0, fs, n);
    CHECK (bin % 2 == 1);
    std::vector<float> x (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
        x[static_cast<size_t> (i)] = static_cast<float> (0.5 * std::sin (kTwoPi * bin * i / n) + 0.5e-4 * std::sin (kTwoPi * (bin * 3 + 101) * i / n) + 0.001);
    CHECK_NEAR (worstAliasDbc (x.data(), n, fs, bin), -80.0, 0.1);
    CHECK_NEAR (dcDbfs (x.data(), n), -60.0, 0.01);

    constexpr double hi = 96000.0;
    std::vector<float> y (static_cast<size_t> (hi));
    for (size_t i = 0; i < y.size(); ++i)
        y[i] = static_cast<float> (0.5 * std::sin (kTwoPi * 1000.0 * static_cast<double> (i) / hi) + 0.005 * std::sin (kTwoPi * 30000.0 * static_cast<double> (i) / hi));
    CHECK_NEAR (powerShareAboveDb (y.data(), static_cast<int> (y.size()), hi, 22000.0), -40.0, 0.1);
}
