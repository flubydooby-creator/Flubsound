// Gaming scenes: docs/11 E60 stage 1 core (#e60) - seeded, licence-free
// scenario programmes rendered through the CLI's offline renderer (the
// real-time chain, latency compensated, primed) at -14 / -24 / -40 LUFS for
// every gaming and night preset, with the metrics docs/11 E19 and E21 are
// judged by. One 12 s programme holds the three core scenes:
//
//   burst   0 - 5 s     : a stereo ambience bed (pink, partly correlated)
//                          and panned footsteps (3.2 kHz band noise, Hann,
//                          20 / 30 / 40 / 50 ms, one every 350 ms) 6 dB under
//                          the bed's full-band RMS (about 4 dB over it in the
//                          3.2 kHz band, like test_known_gaps.cpp's scene)
//   quiet -> combat 5 s : automatic fire (10 shots/s, white noise, tau 15 ms,
//                          peak bed + 24 dB) for 2.5 s with an explosion at
//                          5.5 s (45 Hz + low noise, peak bed + 26 dB)
//   ambush  7.5 - 12 s  : the bed and the steps again, after the event
//
// The whole programme is scaled to its integrated loudness. Metrics (both
// channels' power, output vs input, windows from the pinned timeline):
//
//   cue SNR gain     step-only power in the 3.2 kHz band (step windows minus
//                    the bed) over the FULL-BAND bed power, out minus in:
//                    what a player hears of a step against the whole bed
//                    (a static +6 dB step-band EQ reads about +6 dB)
//   contrast change  step-only over bed power, both in the 3.2 kHz band
//   bed lift         full-band bed power, 150-330 ms after each step onset
//   bed drift        bed lift at 4-5 s minus at 1.5-2.5 s
//   event change     full-band power over the combat, out vs in
//   onset jump       gain of the first shot minus the mean gain of the shots
//                    at 6.5-7.5 s (first 30 ms of each): E21's "first combat
//                    event after quiet <= steady-state events + 1 dB"
//   hole             bed lift before the event (3-5 s) minus 1-2 s after it
//   recovery         seconds after the event until the bed lift stays within
//                    1 dB of its pre-event value (0.25 s bed windows)
//   step after       the steps' cue-band lift 1-2 s after the event minus
//                    their lift before it (E21: within 1 dB after 1 s)
//
// The first tests validate the metrics (bypass, a static step-band EQ, a
// pumping compressor); the last pin every preset's values (the E59 ratchet:
// a change that moves one fails here and must say why) and check docs/11
// E19's Done-when on the module and the Competitive FPS / Battle Royale
// presets. Every test prints its values ("    measured ...").
#include "TestFramework.h"
#include "TestSignals.h"

#include "Analysis.h"
#include "CliOptions.h"
#include "OfflineRenderer.h"

#include "flub/common/Math.h"
#include "flub/engine/Parameters.h"
#include "flub/io/PresetIO.h"
#include "flub/io/WavFile.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace flub;
using namespace flub::cli;
using namespace flub::param;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;
using Channels = std::vector<std::vector<float>>;
using Window = std::pair<int, int>; // [begin, end) in samples

int samplesOf (double seconds) { return static_cast<int> (std::lround (seconds * kFs)); }

void measured (const std::string& name, double value, const char* unit)
{
    char buf[64];
    std::snprintf (buf, sizeof (buf), "%.2f", value);
    std::cout << "    measured " << name << " = " << buf << " " << unit << "\n";
}

// ---- timeline ----------------------------------------------------------------
constexpr double kLength = 12.0;
constexpr double kFirstStep = 0.5, kStepEvery = 0.35;
constexpr double kCombatStart = 5.0, kCombatEnd = 7.5, kExplosionAt = 5.5;
constexpr double kShotEvery = 0.1;
constexpr double kStepBandHz = 3200.0, kStepBandQ = 1.0;
constexpr double kBedWindowFrom = 0.15, kBedWindowTo = 0.33; // after a step onset

// ---- parameters and rendering -------------------------------------------------
std::vector<float> resolve (const RenderOptions& options)
{
    ResolvedParameters p;
    std::string error;
    const bool ok = buildParameters (options, p, error);
    if (! ok)
        std::cerr << "    " << error << "\n";
    REQUIRE (ok);
    return p.values;
}

/** A factory preset in its suggested latency profile (as test_known_gaps.cpp). */
RenderOptions factoryPreset (const std::string& file)
{
    RenderOptions o;
    o.presetSpec = std::string (FLUB_PRESET_DIR) + "/" + file;
    preset::Preset p;
    std::string error;
    if (preset::load (o.presetSpec, p, error))
        o.profile = p.suggestedLatencyProfile;
    return o;
}

void setValue (std::vector<float>& v, int id, float x) { v[static_cast<size_t> (id)] = x; }

void onlyModules (std::vector<float>& v, std::initializer_list<int> keep)
{
    for (int id : { GateOn, EqOn, DynEqOn, BassOn, ClarityOn, SaturationOn, SpatialOn, VirtualizerOn, CompressorOn, MaximizerOn })
        setValue (v, id, std::find (keep.begin(), keep.end(), id) != keep.end() ? 1.0f : 0.0f);
}

Channels render (const io::AudioFileData& input, const std::vector<float>& values)
{
    Channels out;
    int latency = 0;
    std::string error;
    const bool ok = renderPass (input, values, 512, out, latency, error);
    if (! ok)
        std::cerr << "    " << error << "\n";
    REQUIRE (ok);
    return out;
}

// ---- measurement ----------------------------------------------------------------
double meanPower (const Channels& c, const std::vector<Window>& windows)
{
    double acc = 0.0;
    int64_t n = 0;
    for (const auto& ch : c)
        for (const auto& w : windows)
            for (int i = w.first; i < w.second; ++i)
            {
                acc += static_cast<double> (ch[static_cast<size_t> (i)]) * ch[static_cast<size_t> (i)];
                ++n;
            }
    return n > 0 ? acc / static_cast<double> (n) : 0.0;
}

double powerDb (double p) { return 10.0 * std::log10 (std::max (1.0e-30, p)); }

/** RBJ band-pass (0 dB peak), run in double. */
std::vector<float> bandPass (const std::vector<float>& x, double f0, double q)
{
    const double w0 = kTwoPi * f0 / kFs, alpha = std::sin (w0) / (2.0 * q), a0 = 1.0 + alpha;
    const double b0 = alpha / a0, b2 = -alpha / a0, a1 = -2.0 * std::cos (w0) / a0, a2 = (1.0 - alpha) / a0;
    std::vector<float> y (x.size());
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double in = x[i];
        const double out = b0 * in + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = in;
        y2 = y1;
        y1 = out;
        y[i] = static_cast<float> (out);
    }
    return y;
}

Channels bandPass (const Channels& c) { return { bandPass (c[0], kStepBandHz, kStepBandQ), bandPass (c[1], kStepBandHz, kStepBandQ) }; }

/** One-pole low-pass, run in double. */
std::vector<float> lowPass (const std::vector<float>& x, double fc)
{
    const double a = std::exp (-kTwoPi * fc / kFs);
    std::vector<float> y (x.size());
    double s = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        s = (1.0 - a) * x[i] + a * s;
        y[i] = static_cast<float> (s);
    }
    return y;
}

// ---- the scene ------------------------------------------------------------------
struct StepWindow
{
    Window step, bed;
    double onset; // seconds
};

struct Scene
{
    io::AudioFileData input;
    std::vector<StepWindow> steps; // outside the combat
    std::vector<Window> shots;     // first 30 ms of each shot
    std::vector<double> shotOnsets;
    double inputPeak = 0.0;
};

/** The 12 s programme (see the header), scaled to `lufs` integrated. */
Scene makeScene (double lufs)
{
    const int n = samplesOf (kLength);
    constexpr float kBedRms = 0.05f;
    // Ambience: two pink noises, the right channel partly correlated with
    // the left (a diffuse but not fully decorrelated bed).
    const auto bedA = pinkNoise (n, kBedRms, 1357);
    const auto bedB = pinkNoise (n, kBedRms, 2468);
    std::vector<float> left (bedA), right (static_cast<size_t> (n));
    for (size_t i = 0; i < right.size(); ++i)
        right[i] = 0.7f * bedA[i] + 0.71414284f * bedB[i];

    Scene s;
    // Footsteps: 3.2 kHz band noise (the Gaming footsteps band), Hann-shaped,
    // each at kBedRms - 6 dB RMS over its length, panned with a constant-
    // power law at 30 / 60 degrees (left-heavy / right-heavy) in turn.
    const auto band = bandPass (whiteNoise (n, 1.0f, 4242), kStepBandHz, kStepBandQ);
    const double bandRms = rms (band.data(), n), hannRms = std::sqrt (3.0 / 8.0);
    const double stepGain = kBedRms * std::pow (10.0, -6.0 / 20.0) / bandRms / hannRms;
    const double durations[4] = { 0.020, 0.030, 0.040, 0.050 };
    int k = 0;
    for (double t = kFirstStep; t + 0.4 < kLength; t += kStepEvery, ++k)
    {
        if (t > kCombatStart - 0.35 && t < kCombatEnd + 0.1)
            continue; // no steps during the combat
        const int onset = samplesOf (t), len = samplesOf (durations[k % 4]);
        const double angle = (k % 2 == 0 ? 30.0 : 60.0) * kPi / 180.0;
        const double gl = std::cos (angle) * std::sqrt (2.0), gr = std::sin (angle) * std::sqrt (2.0);
        for (int i = 0; i < len; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos (kTwoPi * i / len);
            const double v = stepGain * w * band[static_cast<size_t> (onset + i)];
            left[static_cast<size_t> (onset + i)] += static_cast<float> (gl * v);
            right[static_cast<size_t> (onset + i)] += static_cast<float> (gr * v);
        }
        s.steps.push_back ({ { onset, onset + len }, { onset + samplesOf (kBedWindowFrom), onset + samplesOf (kBedWindowTo) }, t });
    }

    // Combat: automatic fire, and an explosion (45 Hz + low-passed noise).
    FastRandom rng (8642);
    const double shotPeak = kBedRms * std::pow (10.0, 24.0 / 20.0);
    for (double t = kCombatStart; t < kCombatEnd - 0.05; t += kShotEvery)
    {
        const int onset = samplesOf (t);
        for (int i = 0; i < samplesOf (0.1); ++i)
        {
            const double env = shotPeak * std::exp (-i / (0.015 * kFs));
            left[static_cast<size_t> (onset + i)] += static_cast<float> (env * rng.nextBipolar());
            right[static_cast<size_t> (onset + i)] += static_cast<float> (env * rng.nextBipolar());
        }
        s.shots.push_back ({ onset, onset + samplesOf (0.03) });
        s.shotOnsets.push_back (t);
    }
    {
        const int onset = samplesOf (kExplosionAt), len = samplesOf (1.5);
        const auto rumble = lowPass (lowPass (whiteNoise (len, 1.0f, 9753), 150.0), 150.0);
        const double rumbleRms = rms (rumble.data(), len);
        const double peak = kBedRms * std::pow (10.0, 26.0 / 20.0);
        for (int i = 0; i < len; ++i)
        {
            const double t = i / kFs;
            const double v = peak * std::exp (-t / 0.35) * (0.7 * std::sin (kTwoPi * 45.0 * t) + 0.3 * rumble[static_cast<size_t> (i)] / rumbleRms);
            left[static_cast<size_t> (onset + i)] += static_cast<float> (v);
            right[static_cast<size_t> (onset + i)] += static_cast<float> (v);
        }
    }

    Channels c { std::move (left), std::move (right) };
    const auto report = analyse (c, kFs);
    REQUIRE (report.integratedLufs > -100.0f);
    const auto g = static_cast<float> (std::pow (10.0, (lufs - report.integratedLufs) / 20.0));
    for (auto& ch : c)
        for (auto& v : ch)
        {
            v *= g;
            s.inputPeak = std::max (s.inputPeak, static_cast<double> (std::abs (v)));
        }
    s.input.sampleRate = kFs;
    s.input.numChannels = 2;
    s.input.channels = std::move (c);
    return s;
}

struct SceneResult
{
    double cueSnrGainDb = 0, contrastChangeDb = 0, bedLiftDb = 0, bedDriftDb = 0;
    double eventChangeDb = 0, onsetJumpDb = 0, holeDb = 0, recoveryS = 0, stepAfterDb = 0;
};

std::vector<StepWindow> stepsBetween (const Scene& s, double from, double to)
{
    std::vector<StepWindow> w;
    for (const auto& st : s.steps)
        if (st.onset >= from && st.onset < to)
            w.push_back (st);
    return w;
}

std::vector<Window> stepWindows (const std::vector<StepWindow>& w)
{
    std::vector<Window> r;
    for (const auto& st : w)
        r.push_back (st.step);
    return r;
}

std::vector<Window> bedWindows (const std::vector<StepWindow>& w)
{
    std::vector<Window> r;
    for (const auto& st : w)
        r.push_back (st.bed);
    return r;
}

SceneResult measureScene (const Scene& s, const Channels& out)
{
    const auto& in = s.input.channels;
    const auto inBand = bandPass (in), outBand = bandPass (out);
    const auto gain = [] (const Channels& o, const Channels& i, const std::vector<Window>& w) { return powerDb (meanPower (o, w)) - powerDb (meanPower (i, w)); };

    SceneResult r;
    // Burst: the quiet part (1.5 - 5 s: after every tracker has settled).
    const auto quiet = stepsBetween (s, 1.5, kCombatStart);
    const auto qs = stepWindows (quiet), qb = bedWindows (quiet);
    const double inStep = meanPower (inBand, qs) - meanPower (inBand, qb);
    const double outStep = meanPower (outBand, qs) - meanPower (outBand, qb);
    r.cueSnrGainDb = (powerDb (outStep) - powerDb (meanPower (out, qb))) - (powerDb (inStep) - powerDb (meanPower (in, qb)));
    r.contrastChangeDb = (powerDb (outStep) - powerDb (meanPower (outBand, qb))) - (powerDb (inStep) - powerDb (meanPower (inBand, qb)));
    r.bedLiftDb = gain (out, in, qb);
    r.bedDriftDb = gain (out, in, bedWindows (stepsBetween (s, 4.0, 5.0))) - gain (out, in, bedWindows (stepsBetween (s, 1.5, 2.5)));

    // Quiet -> combat.
    r.eventChangeDb = gain (out, in, { { samplesOf (kCombatStart), samplesOf (kCombatEnd) } });
    std::vector<Window> steadyShots;
    for (size_t i = 0; i < s.shots.size(); ++i)
        if (s.shotOnsets[i] >= 6.5)
            steadyShots.push_back (s.shots[i]);
    r.onsetJumpDb = gain (out, in, { s.shots.front() }) - gain (out, in, steadyShots);

    // Ambush: the bed and the steps after the event.
    const double pre = gain (out, in, bedWindows (stepsBetween (s, 3.0, kCombatStart)));
    r.holeDb = pre - gain (out, in, bedWindows (stepsBetween (s, kCombatEnd + 1.0, kCombatEnd + 2.0)));
    r.recoveryS = 0.0;
    for (const auto& st : stepsBetween (s, kCombatEnd, kLength))
        if (std::abs (gain (out, in, { st.bed }) - pre) > 1.0)
            r.recoveryS = st.onset + kBedWindowTo - kCombatEnd; // the last bed window still off by more than 1 dB
    const auto after = stepsBetween (s, kCombatEnd + 1.0, kCombatEnd + 2.0);
    const auto as = stepWindows (after), ab = bedWindows (after);
    const double inAfter = meanPower (inBand, as) - meanPower (inBand, ab);
    const double outAfter = meanPower (outBand, as) - meanPower (outBand, ab);
    r.stepAfterDb = (powerDb (outAfter) - powerDb (inAfter)) - (powerDb (outStep) - powerDb (inStep));
    return r;
}

void print (const std::string& what, const SceneResult& r)
{
    measured (what + ": cue SNR gain", r.cueSnrGainDb, "dB");
    measured (what + ": contrast change", r.contrastChangeDb, "dB");
    measured (what + ": bed lift", r.bedLiftDb, "dB");
    measured (what + ": bed drift", r.bedDriftDb, "dB");
    measured (what + ": event change", r.eventChangeDb, "dB");
    measured (what + ": onset jump", r.onsetJumpDb, "dB");
    measured (what + ": hole 1-2 s after", r.holeDb, "dB");
    measured (what + ": recovery", r.recoveryS, "s");
    measured (what + ": step lift after vs before", r.stepAfterDb, "dB");
}

constexpr double kLevels[3] = { -14.0, -24.0, -40.0 };

std::string levelName (int l) { return std::to_string (static_cast<int> (kLevels[l])) + " LUFS"; }
} // namespace

// =============================================================================
TEST_CASE ("Scenes: the scene is deterministic, its input stays under full scale, and its windows are where the timeline says")
{
    const auto a = makeScene (-24.0), b = makeScene (-24.0);
    CHECK (a.input.channels == b.input.channels);
    for (int l = 0; l < 3; ++l)
    {
        const auto s = makeScene (kLevels[l]);
        measured ("scene input peak at " + levelName (l), toDb (s.inputPeak), "dBFS");
        CHECK_LE (s.inputPeak, 1.0);
        CHECK_NEAR (analyse (s.input.channels, kFs).integratedLufs, kLevels[l], 0.05);
    }
    CHECK (a.shots.size() == 25u);
    CHECK (stepsBetween (a, 1.5, kCombatStart).size() == 9u);
    CHECK (stepsBetween (a, kCombatEnd + 1.0, kCombatEnd + 2.0).size() == 3u);
    for (const auto& st : a.steps)
        CHECK (st.step.second <= st.bed.first); // bed windows hold no step
}

TEST_CASE ("Scenes: metric validation - bypass reads 0, a static +6 dB step-band EQ reads about +6 dB, a 20 dB pumping compressor fails the recovery metric (E60)")
{
    // docs/11 E60 Done-when: "Bypass: cue SNR gain 0 +- 0.1 dB; a +6 dB
    // step-band EQ reads about +6 dB; a 20 dB pumping compressor fails the
    // recovery metric".
    for (int l = 0; l < 3; ++l)
    {
        const auto scene = makeScene (kLevels[l]);
        auto bypass = resolve (RenderOptions {});
        setValue (bypass, BypassAll, 1.0f);
        const auto r = measureScene (scene, render (scene.input, bypass));
        print ("bypass at " + levelName (l), r);
        CHECK_NEAR (r.cueSnrGainDb, 0.0, 0.1);
        CHECK_NEAR (r.contrastChangeDb, 0.0, 0.1);
        CHECK_NEAR (r.bedLiftDb, 0.0, 0.1);
        CHECK_NEAR (r.eventChangeDb, 0.0, 0.1);
        CHECK_NEAR (r.onsetJumpDb, 0.0, 0.1);
        CHECK_NEAR (r.holeDb, 0.0, 0.1);
        CHECK_LE (r.recoveryS, 0.0);
        CHECK_NEAR (r.stepAfterDb, 0.0, 0.1);
    }

    const auto scene = makeScene (-24.0);
    // A static +6 dB bell on the step band (3.2 kHz, Q 1), everything else off.
    auto bell = resolve (RenderOptions {});
    onlyModules (bell, { EqOn });
    setValue (bell, eq (0, EqFieldOn), 1.0f);
    setValue (bell, eq (0, EqFieldType), 0.0f); // Bell
    setValue (bell, eq (0, EqFieldFreq), 3200.0f);
    setValue (bell, eq (0, EqFieldGain), 6.0f);
    setValue (bell, eq (0, EqFieldQ), 1.0f);
    const auto rb = measureScene (scene, render (scene.input, bell));
    print ("static +6 dB step-band bell", rb);
    CHECK_NEAR (rb.cueSnrGainDb, 6.0, 1.0);
    CHECK_LE (std::abs (rb.holeDb), 0.1);
    CHECK_LE (rb.recoveryS, 0.0);

    // A pumping compressor: 20:1 from 2 dB over the bed, 1 ms attack, 2 s
    // release, no make-up - about 20 dB of gain reduction through the combat
    // that recovers over seconds.
    auto pump = resolve (RenderOptions {});
    onlyModules (pump, { CompressorOn });
    const double bedDb = powerDb (meanPower (scene.input.channels, bedWindows (stepsBetween (scene, 1.5, kCombatStart))));
    setValue (pump, CompThresholdDb, static_cast<float> (bedDb + 2.0));
    setValue (pump, CompRatio, 20.0f);
    setValue (pump, CompKneeDb, 0.0f);
    setValue (pump, CompAttackMs, 1.0f);
    setValue (pump, CompReleaseMs, 2000.0f);
    setValue (pump, CompAutoRelease, 0.0f);
    setValue (pump, CompMakeupDb, 0.0f);
    setValue (pump, CompUpMaxGainDb, 0.0f);
    const auto rp = measureScene (scene, render (scene.input, pump));
    print ("20:1 pumping compressor", rp);
    CHECK_LE (rp.eventChangeDb, -12.0);
    CHECK_GE (rp.holeDb, 3.0);
    CHECK_GE (rp.recoveryS, 2.0); // fails a 1 s recovery target
}

TEST_CASE ("Scenes: every gaming and night preset at -14 / -24 / -40 LUFS (E60 stage 1 matrix, E59 ratchet)")
{
    const char* files[] = { "gaming-competitive-fps.json", "gaming-battle-royale.json", "gaming-night-mode.json",
                            "gaming-horror-detail.json", "gaming-7-1-headphone-surround.json", "gaming-cinematic-adventure.json",
                            "gaming-moba-strategy.json", "gaming-racing.json", "gaming-tournament-clean.json",
                            "music-late-night-low-volume.json" };
    RenderOptions footsteps;
    footsteps.mode = ModeValue::Gaming;
    footsteps.macros.push_back ({ "footsteps", 100.0f });
    for (int l = 0; l < 3; ++l)
    {
        const auto scene = makeScene (kLevels[l]);
        print ("Footsteps 100 at " + levelName (l), measureScene (scene, render (scene.input, resolve (footsteps))));
        for (const char* f : files)
            print (std::string (f) + " at " + levelName (l), measureScene (scene, render (scene.input, resolve (factoryPreset (f)))));
    }
}
