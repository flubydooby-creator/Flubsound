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
//                          peak bed RMS + 24 dB) for 2.5 s with an explosion
//                          at 5.5 s (45 Hz + low noise, peak bed RMS + 26 dB)
//   ambush  7.5 - 12 s  : the bed and the steps again, after the event
//
// The bed and the steps set the level (their integrated loudness); the
// combat is turned down where it would pass -1 dBFS (at -14 LUFS). Metrics (both
// channels' power, output vs input, windows from the pinned timeline):
//
//   cue SNR gain     step-only power in the 3.2 kHz band (step windows minus
//                    the bed) over the rest of the bed (full-band minus
//                    3.2 kHz band power), out minus in: how far a step
//                    stands out of everything else (a static EQ reads its
//                    gain on the steps minus its gain on the rest, a
//                    broadband gain 0)
//   contrast change  step-only over bed power, both in the 3.2 kHz band
//   bed lift         full-band bed power, 150-330 ms after each step onset
//   bed drift        bed lift at 4-5 s minus at 1.5-2.5 s
//   event change     full-band power over the combat, out vs in
//   onset jump       gain of the first shot minus the mean gain of the shots
//                    at 6.5-7.5 s (first 30 ms of each): E21's "first combat
//                    event after quiet <= steady-state events + 1 dB"
//   hole             bed lift before the event (3-5 s) minus 1-2 s after it
//   recovery         seconds after the event until the bed lift stays within
//                    1 dB of its pre-event value (pairs of bed windows)
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

/** Two cascaded one-pole high-passes (12 dB/oct). */
std::vector<float> highPass (std::vector<float> x, double fc)
{
    for (int stage = 0; stage < 2; ++stage)
    {
        const auto lp = lowPass (x, fc);
        for (size_t i = 0; i < x.size(); ++i)
            x[i] -= lp[i];
    }
    return x;
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
    double combatDb = 0.0; // combat level re its nominal (<= 0: turned down to stay under -1 dBFS)
};

/** The 12 s programme (see the header): the bed and steps at `lufs`
    integrated, the combat over them. */
Scene makeScene (double lufs)
{
    const int n = samplesOf (kLength);
    constexpr float kBedRms = 0.05f;
    // Ambience: two pink noises, high-passed at 40 Hz (the generator's pink
    // reaches a few Hz; a default 20 Hz subsonic filter would otherwise take
    // 1 dB off the "bed lift"), the right channel partly correlated with the
    // left (a diffuse but not fully decorrelated bed).
    const auto bedA = highPass (pinkNoise (n, kBedRms, 1357), 40.0);
    const auto bedB = highPass (pinkNoise (n, kBedRms, 2468), 40.0);
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

    // Loudness: the bed and the steps (the programme without the combat)
    // measure `lufs` integrated.
    Channels c { std::move (left), std::move (right) };
    const auto report = analyse (c, kFs);
    REQUIRE (report.integratedLufs > -100.0f);
    const double g = std::pow (10.0, (lufs - report.integratedLufs) / 20.0);
    for (auto& ch : c)
        for (auto& v : ch)
            v = static_cast<float> (v * g);

    // Combat: automatic fire (peak bed + 24 dB) and an explosion (45 Hz +
    // low-passed noise, peak bed + 26 dB), turned down as a whole where the
    // sum would pass -1 dBFS: in a loud programme the events sit closer to
    // the bed, as in a mastered game mix (at -14 LUFS the bed alone peaks at
    // -2.6 dBFS, so there is hardly any combat left).
    const int n2 = samplesOf (kLength);
    Channels combat (2, std::vector<float> (static_cast<size_t> (n2), 0.0f));
    const double bedRms = kBedRms * g;
    FastRandom rng (8642);
    const double shotPeak = bedRms * std::pow (10.0, 24.0 / 20.0);
    for (double t = kCombatStart; t < kCombatEnd - 0.05; t += kShotEvery)
    {
        const int onset = samplesOf (t);
        for (int i = 0; i < samplesOf (0.1); ++i)
        {
            const double env = shotPeak * std::exp (-i / (0.015 * kFs));
            combat[0][static_cast<size_t> (onset + i)] += static_cast<float> (env * rng.nextBipolar());
            combat[1][static_cast<size_t> (onset + i)] += static_cast<float> (env * rng.nextBipolar());
        }
        s.shots.push_back ({ onset, onset + samplesOf (0.03) });
        s.shotOnsets.push_back (t);
    }
    {
        const int onset = samplesOf (kExplosionAt), len = samplesOf (1.5);
        const auto rumble = lowPass (lowPass (whiteNoise (len, 1.0f, 9753), 150.0), 150.0);
        const double rumbleRms = rms (rumble.data(), len);
        const double peak = bedRms * std::pow (10.0, 26.0 / 20.0);
        for (int i = 0; i < len; ++i)
        {
            const double t = i / kFs;
            const double v = peak * std::exp (-t / 0.35) * (0.7 * std::sin (kTwoPi * 45.0 * t) + 0.3 * rumble[static_cast<size_t> (i)] / rumbleRms);
            combat[0][static_cast<size_t> (onset + i)] += static_cast<float> (v);
            combat[1][static_cast<size_t> (onset + i)] += static_cast<float> (v);
        }
    }
    const double limit = std::pow (10.0, -1.0 / 20.0);
    double combatScale = 1.0;
    for (int pass = 0; pass < 32; ++pass)
    {
        double peak = 0.0;
        for (size_t ch = 0; ch < 2; ++ch)
            for (size_t i = 0; i < c[ch].size(); ++i)
                peak = std::max (peak, std::abs (c[ch][i] + combatScale * combat[ch][i]));
        if (peak <= limit)
            break;
        combatScale *= 0.97 * limit / peak;
    }
    s.combatDb = 20.0 * std::log10 (combatScale);
    for (size_t ch = 0; ch < 2; ++ch)
        for (size_t i = 0; i < c[ch].size(); ++i)
        {
            c[ch][i] = static_cast<float> (c[ch][i] + combatScale * combat[ch][i]);
            s.inputPeak = std::max (s.inputPeak, static_cast<double> (std::abs (c[ch][i])));
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
    // The rest of the programme: the bed's full-band power minus its power
    // in the step band.
    const double inRest = meanPower (in, qb) - meanPower (inBand, qb), outRest = meanPower (out, qb) - meanPower (outBand, qb);
    r.cueSnrGainDb = (powerDb (outStep) - powerDb (outRest)) - (powerDb (inStep) - powerDb (inRest));
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
    // Recovery: bed windows in consecutive pairs (0.35 s apart, 0.36 s of
    // bed), because single 180 ms windows of an upward-compressed pink bed
    // scatter by +-0.7 dB with no event at all.
    r.recoveryS = 0.0;
    const auto post = stepsBetween (s, kCombatEnd, kLength);
    for (size_t i = 0; i + 1 < post.size(); ++i)
        if (std::abs (gain (out, in, { post[i].bed, post[i + 1].bed }) - pre) > 1.0)
            r.recoveryS = post[i + 1].onset + kBedWindowTo - kCombatEnd; // the last pair still off by more than 1 dB
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
        measured ("scene combat level re nominal at " + levelName (l), s.combatDb, "dB");
        CHECK_LE (s.inputPeak, std::pow (10.0, -1.0 / 20.0));
    }
    CHECK (a.shots.size() == 25u);
    CHECK (stepsBetween (a, 1.5, kCombatStart).size() == 9u);
    CHECK (stepsBetween (a, kCombatEnd + 1.0, kCombatEnd + 2.0).size() == 3u);
    for (const auto& st : a.steps)
        CHECK (st.step.second <= st.bed.first); // bed windows hold no step
}

TEST_CASE ("Scenes: metric validation - bypass reads 0, a static step-band bell reads its gain on the steps over the rest, a 20 dB pumping compressor fails the recovery metric (E60)")
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
    // A static +6 dB bell at the steps' centre and Q (3.2 kHz, Q 1),
    // everything else off. The metric reads the bell's gain on the steps
    // minus its gain on the rest of the bed: from the two filter responses
    // (the steps are Q 1 band noise, wider than the bell's top) +4.77 dB on
    // the steps and +0.50 dB on the rest, so +4.27 dB. A 6 dB broadband cut
    // reads 0 (no cue stands out more) and -6 dB of bed.
    auto bell = resolve (RenderOptions {});
    onlyModules (bell, { EqOn });
    setValue (bell, eq (0, EqFieldOn), 1.0f);
    setValue (bell, eq (0, EqFieldType), 0.0f); // Bell
    setValue (bell, eq (0, EqFieldFreq), 3200.0f);
    setValue (bell, eq (0, EqFieldGain), 6.0f);
    setValue (bell, eq (0, EqFieldQ), 1.0f);
    const auto rb = measureScene (scene, render (scene.input, bell));
    print ("static +6 dB Q 1 bell at 3.2 kHz", rb);
    CHECK_NEAR (rb.cueSnrGainDb, 4.77 - 0.50, 0.3);
    CHECK_LE (std::abs (rb.holeDb), 0.1);
    CHECK_LE (rb.recoveryS, 0.0);
    auto cut = resolve (RenderOptions {});
    onlyModules (cut, {});
    setValue (cut, OutputGainDb, -6.0f);
    const auto rc = measureScene (scene, render (scene.input, cut));
    print ("broadband -6 dB", rc);
    CHECK_NEAR (rc.cueSnrGainDb, 0.0, 0.1);
    CHECK_NEAR (rc.contrastChangeDb, 0.0, 0.1);
    CHECK_NEAR (rc.bedLiftDb, -6.0, 0.1);

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
    CHECK_GE (rp.recoveryS, 1.2); // fails docs/11 E21's "within 1 dB 1 s after the event"
}

TEST_CASE ("Scenes: every gaming and night preset at -14 / -24 / -40 LUFS (E60 stage 1 matrix, E59 ratchet), and docs/11 E19's Done-when on the cue enhancer, Competitive FPS and Battle Royale")
{
    // The ratchet: every metric of every configuration pinned at today's
    // value (0.3 dB, recovery 0.4 s: compiler / FMA differences only). A
    // change that moves one must say why and re-pin it. Recorded after the
    // docs/11 E19 redesign (Footsteps = the CueLift bands 4 / 5) and the
    // gaming preset retune; the values before it are in docs/11 E19 / E60.
    //                      cue SNR, contrast, bed, drift, event, onset jump, hole, recovery (s), step after
    struct Pinned
    {
        const char* file; // nullptr: Gaming mode, Footsteps 100, everything else default (the module)
        double v[3][9];   // [-14 / -24 / -40 LUFS][metric]
    };
    static const Pinned pinned[] = {
        { nullptr,
          { { 5.96, 5.86, -0.03, -0.09, 0.66, -0.11, -0.13, 0.00, 0.31 },
            { 6.14, 6.04, 0.03, 0.03, 0.75, -0.07, -0.03, 0.00, 0.07 },
            { 6.14, 6.04, 0.03, 0.03, 0.74, -0.23, -0.03, 0.00, 0.07 } } },
        { "gaming-competitive-fps.json",
          { { 5.51, 4.69, 0.03, -0.08, 0.15, 0.10, -0.17, 0.00, 0.38 },
            { 6.16, 4.98, 0.37, -0.01, -0.56, 0.15, -0.05, 0.00, 0.10 },
            { 6.99, 5.07, 0.82, -0.03, -0.01, 0.01, -0.04, 0.00, -0.03 } } },
        { "gaming-battle-royale.json",
          { { 4.35, 3.70, -2.46, -0.12, -2.70, 0.28, -0.33, 0.00, 0.33 },
            { 5.25, 4.36, 0.13, -0.05, -2.99, 0.58, -0.22, 0.00, 0.29 },
            { 6.25, 4.83, 0.75, -0.01, 0.07, 0.05, -0.04, 0.00, 0.00 } } },
        { "gaming-night-mode.json",
          { { 1.99, 1.04, -7.86, -0.24, -8.86, 0.65, -0.47, 0.00, 0.07 },
            { 1.95, 0.95, 1.92, 0.31, -6.54, 3.39, 0.79, 1.38, -0.90 },
            { 3.49, 1.61, 9.79, 1.89, 2.52, 0.92, -1.80, 4.18, 2.21 } } },
        { "gaming-horror-detail.json",
          { { 3.18, 2.68, 0.40, -0.09, 0.15, 0.32, -0.25, 0.00, 0.24 },
            { 4.07, 3.20, 2.04, -0.01, 0.00, 0.45, -0.08, 0.00, 0.15 },
            { 5.07, 3.22, 2.68, -0.04, 1.91, 0.10, -0.02, 0.00, -0.05 } } },
        { "gaming-7-1-headphone-surround.json",
          { { 2.52, 2.01, -0.89, -0.10, -1.42, 0.35, -0.27, 0.00, 0.24 },
            { 3.39, 2.51, 2.18, -0.07, -1.34, 0.83, -0.19, 0.00, 0.24 },
            { 4.78, 2.84, 3.16, -0.04, 1.97, 0.06, -0.02, 0.00, -0.07 } } },
        { "gaming-cinematic-adventure.json",
          { { 0.56, 0.16, 0.87, -0.13, 0.27, 1.05, -0.17, 0.00, 0.17 },
            { 1.53, 0.32, 3.11, -0.05, 0.86, 1.03, 0.02, 0.00, 0.02 },
            { 2.84, 0.20, 4.02, -0.10, 4.51, 0.52, -0.01, 0.00, -0.19 } } },
        { "gaming-moba-strategy.json",
          { { -1.29, -0.98, -1.46, -0.04, -2.25, 0.30, -0.19, 0.00, -0.35 },
            { -0.72, -2.25, 2.83, -0.03, -1.30, 1.38, -0.04, 0.00, -0.56 },
            { 2.90, -0.44, 4.24, -0.11, 2.35, 0.18, -0.04, 0.00, -0.43 } } },
        { "gaming-racing.json",
          { { -0.34, -0.78, -0.74, 0.03, -1.76, 0.30, 0.02, 0.00, 0.31 },
            { 0.35, -0.69, 3.29, 0.02, -0.76, 1.56, 0.03, 0.00, 0.27 },
            { 1.89, -0.22, 4.16, -0.05, 4.09, 0.15, -0.01, 0.00, -0.22 } } },
        { "gaming-tournament-clean.json",
          { { 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00 },
            { 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00 },
            { 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00 } } },
        { "music-late-night-low-volume.json",
          { { -0.33, -0.22, -8.80, -0.12, -9.63, 1.08, -0.03, 0.00, -0.20 },
            { -0.41, -0.33, 0.84, 0.71, -5.23, 6.03, 1.86, 4.18, -1.85 },
            { 0.14, -0.31, 7.30, 2.15, 4.11, 3.15, -1.68, 4.18, 2.31 } } },
    };
    for (int l = 0; l < 3; ++l)
    {
        const auto scene = makeScene (kLevels[l]);
        for (const auto& p : pinned)
        {
            RenderOptions o;
            if (p.file == nullptr)
            {
                o.mode = ModeValue::Gaming;
                o.macros.push_back ({ "footsteps", 100.0f });
            }
            else
                o = factoryPreset (p.file);
            const std::string what = std::string (p.file == nullptr ? "Footsteps 100" : p.file) + " at " + levelName (l);
            const auto r = measureScene (scene, render (scene.input, resolve (o)));
            print (what, r);
            const double got[9] = { r.cueSnrGainDb, r.contrastChangeDb, r.bedLiftDb, r.bedDriftDb, r.eventChangeDb,
                                    r.onsetJumpDb, r.holeDb, r.recoveryS, r.stepAfterDb };
            for (int m = 0; m < 9; ++m)
                CHECK_NEAR (got[m], p.v[l][m], m == 7 ? 0.4 : 0.3);

            // docs/11 E19 Done-when: the module and the Competitive FPS /
            // Battle Royale presets lift the bed <= +1 dB and raise the
            // step / bed contrast >= +3 dB, at every level (before the
            // redesign, at -24 / -40 LUFS: module bed +2.44 / +2.44 dB,
            // contrast +0.77 / +0.77 dB; Competitive FPS +2.50 / +3.63 and
            // +0.50 / +0.35 dB; Battle Royale +3.92 / +5.48 and +0.06 /
            // +0.31 dB).
            const bool judged = p.file == nullptr || std::string (p.file) == "gaming-competitive-fps.json"
                                || std::string (p.file) == "gaming-battle-royale.json";
            if (judged)
            {
                CHECK_LE (r.bedLiftDb, 1.0);
                CHECK_GE (r.contrastChangeDb, 3.0);
            }
        }
    }
    // The module's lift is the same law at every level: within +-1 dB
    // across -14 / -24 / -40 LUFS (docs/11 E19 Done-when).
    for (int l = 1; l < 3; ++l)
        CHECK_NEAR (pinned[0].v[l][0], pinned[0].v[0][0], 1.0);
}
