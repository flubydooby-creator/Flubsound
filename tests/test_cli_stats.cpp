// CLI render statistics and chain options added for docs/11 E06 (6) / E59:
// the SafetyGovernor's state and reasons in `render.stats` (time shares and
// the reading at the end of the programme), the harmonics reading of the
// bass harmonics generator / air exciter, `--protection` (the chain's
// protection strength, a host setting) and `quality --rate` (the hygiene
// family's sample rate) and `--smart` with a preset's "smart" flag (docs/11
// E34). Each render is a few seconds of 48 kHz audio.
#include "TestFramework.h"
#include "TestSignals.h"

#include "CliOptions.h"
#include "Commands.h"
#include "OfflineRenderer.h"

#include "flub/common/Math.h"
#include "flub/engine/Protection.h"
#include "flub/io/PresetIO.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

using namespace flub;
using namespace flub::cli;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;

io::AudioFileData stereoOf (const std::vector<float>& mono)
{
    io::AudioFileData d;
    d.sampleRate = kFs;
    d.numChannels = 2;
    d.channels = { mono, mono };
    return d;
}

/** Base values as `flubsound-cli process <args>` resolves them. */
std::vector<float> resolveArgs (std::vector<std::string> args)
{
    args.insert (args.begin(), { "process", "-i", "in.wav", "-o", "out.wav" });
    CliOptions o;
    std::string error;
    REQUIRE (parseCommandLine (args, o, error));
    ResolvedParameters p;
    REQUIRE (buildParameters (o.render, p, error));
    return p.values;
}

RenderResult renderWith (const io::AudioFileData& input, const std::vector<float>& values, ProtectionStrength protection = ProtectionStrength::Off)
{
    RenderSettings s;
    s.protection = protection;
    RenderResult rr;
    std::string error;
    REQUIRE (renderFile (input, values, s, rr, error));
    return rr;
}

std::vector<std::string> reasonsOf (const json::Value& stats)
{
    std::vector<std::string> r;
    for (const auto& v : stats["governor"]["end"]["reasons"].asArray())
        r.push_back (v.asString());
    return r;
}
} // namespace

TEST_CASE ("CLI: --protection off|normal|strict is a chain option (default off) and --rate belongs to `quality`")
{
    CliOptions o;
    std::string error;
    REQUIRE (parseCommandLine ({ "process", "-i", "a.wav", "-o", "b.wav" }, o, error));
    CHECK (o.render.protection == ProtectionStrength::Off);
    REQUIRE (parseCommandLine ({ "process", "-i", "a.wav", "-o", "b.wav", "--protection", "strict" }, o, error));
    CHECK (o.render.protection == ProtectionStrength::Strict);
    CHECK (makeRenderSettings (o.render, ResolvedParameters {}).protection == ProtectionStrength::Strict);
    REQUIRE (parseCommandLine ({ "quality", "--protection", "Normal", "--rate", "44100" }, o, error));
    CHECK (o.render.protection == ProtectionStrength::Normal);
    CHECK (o.rate == 44100.0);
    REQUIRE (parseCommandLine ({ "batch", "-i", "in", "-o", "out", "--protection", "off" }, o, error));

    CHECK (! parseCommandLine ({ "process", "-i", "a.wav", "-o", "b.wav", "--protection", "max" }, o, error));
    CHECK (error.find ("--protection expects off, normal or strict") != std::string::npos);
    CHECK (! parseCommandLine ({ "process", "-i", "a.wav", "-o", "b.wav", "--rate", "44100" }, o, error)); // quality only
    CHECK (! parseCommandLine ({ "quality", "--rate", "1000" }, o, error));
    CHECK (! parseCommandLine ({ "quality", "--rate", "44100.5" }, o, error));
    CHECK (! parseCommandLine ({ "analyze", "-i", "a.wav", "--protection", "normal" }, o, error)); // no chain
}

TEST_CASE ("CLI render.stats: the governor's state and reasons - limiter-bound and THD+N-bound scenes, at the end and as time shares (docs/11 E06 (6))")
{
    // Limiter-bound: -18 dBFS pink at Loudness 100 + Boost 100 with the base
    // drive at 24 dB and the clipper off, so only the limiter works (GR mean
    // about -10 dB, over the -6 dB budget; the budget is a ~3 s average, so
    // the governor starts backing off after about 2 s).
    const auto pink = stereoOf (pinkNoise (static_cast<int> (6.0 * kFs), 0.126f, 5959));
    const auto limiterBound = renderWith (pink, resolveArgs ({ "--mode", "music", "--boost", "100", "--macro", "4=100", "--set", "max.drive=24", "max.clip=0" }));
    auto st = renderStatsToJson (limiterBound.stats);
    CHECK (st["governor"]["end"]["state"].asString() == "backingOff");
    CHECK (reasonsOf (st) == std::vector<std::string> { "limiter" });
    CHECK_LE (st["governor"]["end"]["scale"].asNumber(), 0.6);
    CHECK_GE (st["governor"]["limiterReasonPercent"].asNumber(), 50.0);
    CHECK_NEAR (st["governor"]["distortionReasonPercent"].asNumber (0.0), 0.0, 1.0e-9);
    CHECK_GE (st["governor"]["statePercent"]["backingOff"].asNumber(), 50.0);
    CHECK (formatStats (limiterBound.stats).find ("at the end backingOff") != std::string::npos);

    // THD+N-bound: a -6 dBFS 50 Hz sine with every Music macro at 100.
    const auto sine50 = stereoOf (sine (50.0, kFs, static_cast<int> (3.0 * kFs), 0.5f));
    const auto allMacros = resolveArgs ({ "--mode", "music", "--boost", "100", "--macro", "1=100", "2=100", "3=100", "4=100", "5=100" });
    const auto thdBound = renderWith (sine50, allMacros);
    st = renderStatsToJson (thdBound.stats);
    CHECK (reasonsOf (st) == std::vector<std::string> { "distortion" });
    CHECK_GE (st["governor"]["distortionReasonPercent"].asNumber(), 50.0);
    CHECK_NEAR (st["governor"]["limiterReasonPercent"].asNumber (0.0), 0.0, 1.0e-9);
    // Every Music macro raises the bass harmonics: the harmonics reading is there.
    CHECK_GE (st["harmonics"]["maxDb"].asNumber (-160.0), -20.0);
    CHECK (formatStats (thdBound.stats).find ("harmonics max") != std::string::npos);

    // Nothing to govern: idle at the end, no reasons, the full scale.
    const auto quiet = renderWith (stereoOf (sine (1000.0, kFs, static_cast<int> (1.0 * kFs), 0.01f)), resolveArgs ({}));
    st = renderStatsToJson (quiet.stats);
    CHECK (st["governor"]["end"]["state"].asString() == "idle");
    CHECK (reasonsOf (st).empty());
    CHECK_NEAR (st["governor"]["end"]["scale"].asNumber(), 1.0, 1.0e-9);
}

TEST_CASE ("CLI --protection reaches the chain: Normal governs the base drives (lower THD+N than Off), Strict lets the scale fall below 0.3")
{
    // The docs/11 E06 scene: all Music macros 100 on a -6 dBFS 50 Hz sine,
    // with base max.drive 12 + sat.drive 12 (governed only at Normal /
    // Strict), 6 s so the scale reaches its floor.
    const auto sine50 = stereoOf (sine (50.0, kFs, static_cast<int> (6.0 * kFs), 0.5f));
    const auto values = resolveArgs ({ "--mode", "music", "--boost", "100", "--macro", "1=100", "2=100", "3=100", "4=100", "5=100", "--set", "max.drive=12", "sat.drive=12" });
    const auto off = renderWith (sine50, values, ProtectionStrength::Off);
    const auto normal = renderWith (sine50, values, ProtectionStrength::Normal);
    const auto strict = renderWith (sine50, values, ProtectionStrength::Strict);
    // The saturator + clipper THD+N the governor measures drops once the base drives are governed.
    CHECK_LE (normal.stats.distortionMeanDb, off.stats.distortionMeanDb - 0.5f);
    CHECK_GE (off.stats.governorScaleMin, 0.3f - 1.0e-6f);
    CHECK_LE (strict.stats.governorScaleMin, 0.3f - 0.05f);
}

TEST_CASE ("CLI Smart macros (docs/11 E34): a preset's \"smart\" flag reaches the render, --smart on|off overrides it, and quality takes it too")
{
    namespace fs = std::filesystem;
    std::random_device rd;
    const fs::path file = fs::temp_directory_path() / ("flub_smart_" + std::to_string (rd()) + ".json");
    struct Remove
    {
        fs::path p;
        ~Remove() { std::error_code ec; fs::remove (p, ec); }
    } const cleanup { file };

    // A preset with Smart macros: Music, Boost 100, Punch 100, Loudness 60.
    std::vector<std::string> base { "--mode", "music", "--boost", "100", "--macro", "punch=100", "loudness=60" };
    auto smartPreset = preset::makeDefault();
    smartPreset.name = "Smart test";
    smartPreset.values = resolveArgs (base);
    smartPreset.smart = true;
    std::string error;
    REQUIRE (preset::save (file.string(), smartPreset, error));

    const auto resolve = [] (std::vector<std::string> args) {
        args.insert (args.begin(), { "process", "-i", "in.wav", "-o", "out.wav" });
        CliOptions o;
        std::string e;
        REQUIRE (parseCommandLine (args, o, e));
        ResolvedParameters p;
        REQUIRE (buildParameters (o.render, p, e));
        return std::make_pair (o, p);
    };
    const auto [plainOptions, plain] = resolve (base);
    const auto [fromPresetOptions, fromPreset] = resolve ({ "--preset", file.string() });
    const auto [forcedOffOptions, forcedOff] = resolve ({ "--preset", file.string(), "--smart", "off" });
    const auto [forcedOnOptions, forcedOn] = resolve ({ "--smart", "on", "--mode", "music", "--boost", "100", "--macro", "punch=100", "loudness=60" });
    CHECK (! plain.smart);
    CHECK (fromPreset.smart);
    CHECK (! forcedOff.smart);
    CHECK (forcedOn.smart);
    CHECK (fromPreset.values == plain.values);
    CHECK (makeRenderSettings (fromPresetOptions.render, fromPreset).smartMacros);
    CHECK (! makeRenderSettings (forcedOffOptions.render, forcedOff).smartMacros);

    CliOptions bad;
    CHECK (! parseCommandLine ({ "process", "-i", "a.wav", "-o", "b.wav", "--smart", "maybe" }, bad, error));
    CHECK (error.find ("--smart expects on or off") != std::string::npos);
    REQUIRE (parseCommandLine ({ "quality", "--smart", "on" }, bad, error));
    CHECK (bad.render.smart == std::optional<bool> (true));

    // A limited master (pink through a hard tanh, PLR well under 7.5 LU): Smart
    // takes back attack and drive, so the preset's render differs from the
    // static one and matches --smart on bit for bit.
    auto mono = pinkNoise (static_cast<int> (2.0 * kFs), 0.25f, 77);
    for (auto& x : mono)
        x = 0.9f * std::tanh (6.0f * x);
    const auto input = stereoOf (mono);
    const auto render = [&input] (const std::vector<float>& values, const RenderSettings& s) {
        RenderResult rr;
        std::string e;
        REQUIRE (renderFile (input, values, s, rr, e));
        return rr.output.channels;
    };
    const auto staticOut = render (plain.values, makeRenderSettings (plainOptions.render, plain));
    const auto presetOut = render (fromPreset.values, makeRenderSettings (fromPresetOptions.render, fromPreset));
    const auto onOut = render (forcedOn.values, makeRenderSettings (forcedOnOptions.render, forcedOn));
    const auto offOut = render (forcedOff.values, makeRenderSettings (forcedOffOptions.render, forcedOff));
    CHECK (presetOut == onOut);
    CHECK (offOut == staticOut);
    double diff = 0.0;
    for (size_t i = static_cast<size_t> (kFs); i < staticOut[0].size(); ++i) // after Smart's 0.5 s warm-up
        diff = std::max (diff, static_cast<double> (std::abs (presetOut[0][i] - staticOut[0][i])));
    CHECK_GE (diff, 1.0e-3);
}
