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

#include "flub/analysis/LoudnessMeter.h"
#include "flub/analysis/SceneEvents.h"
#include "flub/common/Math.h"
#include "flub/dsp/Biquad.h"
#include "flub/dsp/Compressor.h"
#include "flub/dsp/DynamicEq.h"
#include "flub/engine/Parameters.h"
#include "flub/engine/StartleGuard.h"
#include "flub/io/PresetIO.h"
#include "flub/io/WavFile.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
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

// ---- stage 1 remainder: dialogue over effects, speech -> music -> silence,
// ---- and a quiet -> loud track change (docs/11 E60) --------------------------
double uniform (FastRandom& rng, double lo, double hi) { return lo + (hi - lo) * 0.5 * (static_cast<double> (rng.nextBipolar()) + 1.0); }

/** Two-pole resonator (unity gain at its centre, roughly), run in double. */
struct Resonator
{
    double a1 = 0.0, a2 = 0.0, g = 0.0, y1 = 0.0, y2 = 0.0;
    void set (double hz, double bandwidthHz)
    {
        const double r = std::exp (-kPi * bandwidthHz / kFs);
        a1 = 2.0 * r * std::cos (kTwoPi * hz / kFs);
        a2 = -r * r;
        g = 1.0 - r;
    }
    double process (double x)
    {
        const double y = g * x + a1 * y1 + a2 * y2;
        y2 = y1;
        y1 = y;
        return y;
    }
};

/** A speech-like mono signal in [from, to): phrases of 4-7 syllables (an
    impulse train at 100-170 Hz through three vowel formants; every fourth
    syllable a 4.5 kHz fricative), 30-70 ms between syllables and 0.4-0.6 s
    between phrases. `syllables` / `pauses` are measurement windows: the
    syllables' middles, and the phrase pauses less 50 ms at each end. */
struct Speech
{
    std::vector<float> x;
    std::vector<Window> syllables, pauses;
};

Speech makeSpeech (int n, double from, double to, uint32_t seed)
{
    static const double vowels[5][2] = { { 730, 1090 }, { 270, 2290 }, { 300, 870 }, { 530, 1840 }, { 570, 840 } };
    Speech s;
    s.x.assign (static_cast<size_t> (n), 0.0f);
    FastRandom rng (seed);
    const auto hiss = bandPass (whiteNoise (n, 1.0f, seed + 1), 4500.0, 1.5);
    double t = from;
    int k = 0;
    while (t < to - 0.8)
    {
        const int syllables = 4 + static_cast<int> (rng.nextU32() % 4u);
        const double f0 = uniform (rng, 100.0, 170.0);
        for (int j = 0; j < syllables && t < to - 0.3; ++j, ++k)
        {
            const double dur = uniform (rng, 0.12, 0.24);
            const int onset = samplesOf (t), len = samplesOf (dur);
            const int edge = samplesOf (0.02);
            const auto envelope = [&] (int i) {
                const double a = i < edge ? 0.5 - 0.5 * std::cos (kPi * i / edge) : 1.0;
                const double b = len - i < edge ? 0.5 - 0.5 * std::cos (kPi * (len - i) / edge) : 1.0;
                return a * b;
            };
            if (k % 4 == 3)
            {
                for (int i = 0; i < len; ++i)
                    s.x[static_cast<size_t> (onset + i)] += static_cast<float> (0.25 * envelope (i) * hiss[static_cast<size_t> (onset + i)]);
            }
            else
            {
                const auto& v = vowels[rng.nextU32() % 5u];
                Resonator r1, r2, r3;
                r1.set (v[0], 90.0);
                r2.set (v[1], 110.0);
                r3.set (2500.0, 150.0);
                double phase = 1.0;
                const double pitch = f0 * uniform (rng, 0.9, 1.1);
                for (int i = 0; i < len; ++i)
                {
                    phase += (pitch * (1.0 - 0.1 * i / len)) / kFs; // a slight fall over the syllable
                    const double pulse = phase >= 1.0 ? 1.0 : 0.0;
                    if (phase >= 1.0)
                        phase -= 1.0;
                    const double y = r1.process (pulse) + 0.6 * r2.process (pulse) + 0.25 * r3.process (pulse);
                    s.x[static_cast<size_t> (onset + i)] += static_cast<float> (envelope (i) * y);
                }
            }
            s.syllables.push_back ({ onset + samplesOf (0.02), onset + len - samplesOf (0.02) });
            t += dur + uniform (rng, 0.03, 0.07);
        }
        const double pause = uniform (rng, 0.4, 0.6);
        if (t + pause < to)
            s.pauses.push_back ({ samplesOf (t + 0.05), samplesOf (t + pause - 0.05) });
        t += pause;
    }
    return s;
}

/** A stereo music-like signal: kick on every beat, snare on 2 and 4, hats on
    the eighths, a bass note and a three-note pad (four harmonics each,
    detuned between the channels) per bar. */
Channels makeMusic (int n, double bpm, uint32_t seed)
{
    Channels m (2, std::vector<float> (static_cast<size_t> (n), 0.0f));
    const auto noise = whiteNoise (n, 1.0f, seed);
    const auto snareNoise = highPass (noise, 1000.0), hatNoise = highPass (highPass (noise, 7000.0), 7000.0);
    const double beat = 60.0 / bpm;
    static const double roots[4] = { 55.0, 43.65, 65.41, 49.0 };   // A1 F1 C2 G1
    static const double chords[4][3] = { { 220.0, 261.63, 329.63 }, { 174.61, 220.0, 261.63 }, { 261.63, 329.63, 392.0 }, { 196.0, 246.94, 293.66 } };
    for (int b = 0; b * beat < n / kFs; ++b)
    {
        const int onset = samplesOf (b * beat), bar = (b / 4) % 4;
        const int len = std::min (samplesOf (beat), n - onset);
        for (int i = 0; i < len; ++i)
        {
            const double t = i / kFs, tb = (b * beat) + t;
            double mono = 0.9 * std::exp (-t / 0.12) * std::sin (kTwoPi * (45.0 * t + 10.0 * 0.03 * (1.0 - std::exp (-t / 0.03))));
            if (b % 2 == 1)
                mono += 0.45 * std::exp (-t / 0.08) * snareNoise[static_cast<size_t> (onset + i)];
            const double bass = 0.35 * std::exp (-t / 0.3) * (std::sin (kTwoPi * roots[bar] * tb) + 0.3 * std::sin (kTwoPi * 2.0 * roots[bar] * tb));
            for (int ch = 0; ch < 2; ++ch)
            {
                double pad = 0.0;
                for (double f : chords[bar])
                    for (int h = 1; h <= 4; ++h)
                        pad += std::sin (kTwoPi * f * h * (ch == 0 ? 0.997 : 1.003) * tb) / h;
                const double hatGain = (i < samplesOf (beat / 2) ? 1.0 : 0.7) * (ch == 0 ? 0.8 : 1.2);
                const int half = i % samplesOf (beat / 2);
                const double hat = 0.12 * hatGain * std::exp (-half / (0.02 * kFs)) * hatNoise[static_cast<size_t> (onset + i)];
                m[static_cast<size_t> (ch)][static_cast<size_t> (onset + i)] += static_cast<float> (mono + bass + 0.05 * pad + hat);
            }
        }
    }
    return m;
}

double integratedOf (const Channels& c) { return analyse (c, kFs).integratedLufs; }

/** The signal through the BS.1770 K-weighting: the new scenes' level metrics
    are loudness-weighted, so a preset's low-shelf or high-pass does not read
    as turning music (kick and bass) down against speech. */
Channels kWeighted (const Channels& c)
{
    Biquad s1, s2;
    s1.setCoeffs (LoudnessMeter::kWeightingStage1 (kFs));
    s2.setCoeffs (LoudnessMeter::kWeightingStage2 (kFs));
    Channels k (c.size());
    for (size_t ch = 0; ch < c.size(); ++ch)
    {
        k[ch].resize (c[ch].size());
        for (size_t i = 0; i < c[ch].size(); ++i)
            k[ch][i] = static_cast<float> (s2.processSample (static_cast<int> (ch), s1.processSample (static_cast<int> (ch), c[ch][i])));
    }
    return k;
}

void scaleTo (Channels& c, double lufs)
{
    const double g = std::pow (10.0, (lufs - integratedOf (c)) / 20.0);
    for (auto& ch : c)
        for (auto& v : ch)
            v = static_cast<float> (v * g);
}

/** Turns the whole programme down where it would pass -1 dBFS; returns the
    change in dB (<= 0). */
double keepUnderFullScale (Channels& c)
{
    double peak = 0.0;
    for (const auto& ch : c)
        peak = std::max (peak, peakAbs (ch.data(), static_cast<int> (ch.size())));
    const double limit = std::pow (10.0, -1.0 / 20.0);
    if (peak <= limit)
        return 0.0;
    const double g = limit / peak;
    for (auto& ch : c)
        for (auto& v : ch)
            v = static_cast<float> (v * g);
    return 20.0 * std::log10 (g);
}

io::AudioFileData fileOf (Channels c)
{
    io::AudioFileData f;
    f.sampleRate = kFs;
    f.numChannels = 2;
    f.channels = std::move (c);
    return f;
}

double gainDb (const Channels& out, const Channels& in, const std::vector<Window>& w) { return powerDb (meanPower (out, w)) - powerDb (meanPower (in, w)); }

Window span (double from, double to) { return { samplesOf (from), samplesOf (to) }; }

// Dialogue over effects: 6 s of a stationary effects bed (the pink ambience
// of the core scenes plus a 120 Hz engine rumble 3 dB under it) with centred
// dialogue from 0.8 s at the same loudness as the effects (0 LU).
struct DialogueScene
{
    io::AudioFileData input;
    Channels dialogue; // the dialogue alone, as mixed into the input
    Speech speech;
    double fullScaleDb = 0.0;
};

DialogueScene makeDialogueScene (double lufs)
{
    const int n = samplesOf (6.0);
    DialogueScene s;
    const auto bedA = highPass (pinkNoise (n, 0.05f, 1357), 40.0), bedB = highPass (pinkNoise (n, 0.05f, 2468), 40.0);
    auto rumble = lowPass (lowPass (whiteNoise (n, 1.0f, 97531), 120.0), 120.0);
    const double rumbleGain = 0.05 * std::pow (10.0, -3.0 / 20.0) / rms (rumble.data(), n);
    Channels fx (2, std::vector<float> (static_cast<size_t> (n)));
    for (size_t i = 0; i < static_cast<size_t> (n); ++i)
    {
        const double r = rumbleGain * rumble[i];
        fx[0][i] = static_cast<float> (bedA[i] + r);
        fx[1][i] = static_cast<float> (0.7 * bedA[i] + 0.71414284 * bedB[i] + r);
    }
    s.speech = makeSpeech (n, 0.8, 6.0, 777);
    Channels voice { s.speech.x, s.speech.x };
    scaleTo (voice, integratedOf (fx));
    Channels c (2, std::vector<float> (static_cast<size_t> (n)));
    for (size_t ch = 0; ch < 2; ++ch)
        for (size_t i = 0; i < static_cast<size_t> (n); ++i)
            c[ch][i] = fx[ch][i] + voice[ch][i];
    const double g = std::pow (10.0, (lufs - integratedOf (c)) / 20.0);
    for (size_t ch = 0; ch < 2; ++ch)
        for (size_t i = 0; i < static_cast<size_t> (n); ++i)
        {
            c[ch][i] = static_cast<float> (c[ch][i] * g);
            voice[ch][i] = static_cast<float> (voice[ch][i] * g);
        }
    s.fullScaleDb = keepUnderFullScale (c);
    const double k = std::pow (10.0, s.fullScaleDb / 20.0);
    for (auto& ch : voice)
        for (auto& v : ch)
            v = static_cast<float> (v * k);
    s.dialogue = std::move (voice);
    s.input = fileOf (std::move (c));
    return s;
}

struct DialogueResult
{
    double snrGainDb = 0, dialogueLiftDb = 0, effectsLiftDb = 0;
};

/** Measured from 1.2 s (every tracker settled on the effects):
      dialogue SNR gain  dialogue-only power over effects power in the
                         speech band (2 kHz, Q 0.7: about 1-4 kHz), out minus
                         in; the dialogue-only power is the syllables minus
                         the pauses (the effects are stationary);
      dialogue lift      the same dialogue-only power, K-weighted, out vs in;
      effects lift       K-weighted power in the pauses, out vs in. */
DialogueResult measureDialogue (const DialogueScene& s, const Channels& out)
{
    const auto& in = s.input.channels;
    std::vector<Window> syl, pau;
    for (const auto& w : s.speech.syllables)
        if (w.first >= samplesOf (1.2))
            syl.push_back (w);
    for (const auto& w : s.speech.pauses)
        if (w.first >= samplesOf (1.2))
            pau.push_back (w);
    const auto band = [] (const Channels& c) { return Channels { bandPass (c[0], 2000.0, 0.7), bandPass (c[1], 2000.0, 0.7) }; };
    const auto inB = band (in), outB = band (out);
    const double inDlg = meanPower (inB, syl) - meanPower (inB, pau), outDlg = meanPower (outB, syl) - meanPower (outB, pau);
    DialogueResult r;
    r.snrGainDb = (powerDb (outDlg) - powerDb (meanPower (outB, pau))) - (powerDb (inDlg) - powerDb (meanPower (inB, pau)));
    const auto inK = kWeighted (in), outK = kWeighted (out);
    r.dialogueLiftDb = powerDb (meanPower (outK, syl) - meanPower (outK, pau)) - powerDb (meanPower (inK, syl) - meanPower (inK, pau));
    r.effectsLiftDb = gainDb (outK, inK, pau);
    return r;
}

// Speech -> music -> silence: speech 0 - 4 s, music 4 - 6.5 s (each at the
// programme level on its own), 1.5 s of silence (only the -80 dBFS hiss that
// runs under the whole programme), speech again 8 - 9.5 s. The first speech
// is long enough for Auto Level (3 dB/s, +6 dB cap) to settle on it.
constexpr double kMusicFrom = 4.0, kSilenceFrom = 6.5, kSpeechAgain = 8.0, kSmsLength = 9.5;

struct SmsScene
{
    io::AudioFileData input;
    double fullScaleDb = 0.0;
};

SmsScene makeSpeechMusicSilence (double lufs)
{
    const int n = samplesOf (kSmsLength);
    const auto speech1 = makeSpeech (samplesOf (kMusicFrom), 0.1, kMusicFrom, 4711).x;
    const auto speech2 = makeSpeech (samplesOf (kSmsLength - kSpeechAgain), 0.0, kSmsLength - kSpeechAgain, 4712).x;
    Channels sp1 { speech1, speech1 }, sp2 { speech2, speech2 };
    auto music = makeMusic (samplesOf (kSilenceFrom - kMusicFrom), 120.0, 99);
    scaleTo (sp1, lufs);
    scaleTo (sp2, lufs);
    scaleTo (music, lufs);
    Channels c (2, std::vector<float> (static_cast<size_t> (n), 0.0f));
    const auto place = [&] (const Channels& part, double at) {
        for (size_t ch = 0; ch < 2; ++ch)
            std::copy (part[ch].begin(), part[ch].end(), c[ch].begin() + samplesOf (at));
    };
    place (sp1, 0.0);
    place (music, kMusicFrom);
    place (sp2, kSpeechAgain);
    SmsScene s;
    s.fullScaleDb = keepUnderFullScale (c);
    const float hiss = static_cast<float> (std::pow (10.0, -80.0 / 20.0) * std::sqrt (3.0));
    const auto h0 = whiteNoise (n, hiss, 31), h1 = whiteNoise (n, hiss, 32);
    for (size_t i = 0; i < static_cast<size_t> (n); ++i)
    {
        c[0][i] += h0[i];
        c[1][i] += h1[i];
    }
    s.input = fileOf (std::move (c));
    return s;
}

struct SmsResult
{
    double balanceChangeDb = 0, musicOnsetJumpDb = 0, silenceLiftDb = 0, silenceOutDbfs = 0, speechReturnDb = 0;
};

/** All K-weighted gains, out vs in:
    balance change   music gain (the last 1.5 s of it) minus speech gain (the
                     2 s before the music): how far the chain moves music
                     against speech;
    music onset jump gain over the music's first 300 ms minus its last 1.5 s;
    silence lift     gain on the hiss 0.3 - 1.4 s into the silence minus the
                     speech gain: whether the noise floor comes up against
                     the programme (and the hiss's output level, dBFS);
    speech return    gain over the returning speech minus the speech before. */
SmsResult measureSms (const SmsScene& s, const Channels& outRaw)
{
    const auto in = kWeighted (s.input.channels), out = kWeighted (outRaw);
    SmsResult r;
    const double speechGain = gainDb (out, in, { span (kMusicFrom - 2.0, kMusicFrom) });
    const double musicGain = gainDb (out, in, { span (kSilenceFrom - 1.5, kSilenceFrom) });
    r.balanceChangeDb = musicGain - speechGain;
    r.musicOnsetJumpDb = gainDb (out, in, { span (kMusicFrom, kMusicFrom + 0.3) }) - musicGain;
    r.silenceLiftDb = gainDb (out, in, { span (kSilenceFrom + 0.3, kSpeechAgain - 0.1) }) - speechGain;
    r.silenceOutDbfs = powerDb (meanPower (outRaw, { span (kSilenceFrom + 0.3, kSpeechAgain - 0.1) }));
    r.speechReturnDb = gainDb (out, in, { span (kSpeechAgain, kSmsLength) }) - speechGain;
    return r;
}

// Track change: a quiet track (100 BPM, 15 LU under the programme level)
// for 3 s, then a loud one (128 BPM, at the programme level) to 8 s.
constexpr double kTrackChange = 3.0, kTrackLength = 8.0, kTrackSteady = 5.5;

struct TrackScene
{
    io::AudioFileData input;
    double fullScaleDb = 0.0;
};

TrackScene makeTrackChange (double lufs)
{
    auto quiet = makeMusic (samplesOf (kTrackChange), 100.0, 5);
    auto loud = makeMusic (samplesOf (kTrackLength - kTrackChange), 128.0, 6);
    scaleTo (quiet, lufs - 15.0);
    scaleTo (loud, lufs);
    Channels c (2);
    for (size_t ch = 0; ch < 2; ++ch)
    {
        c[ch] = quiet[ch];
        c[ch].insert (c[ch].end(), loud[ch].begin(), loud[ch].end());
    }
    TrackScene s;
    s.fullScaleDb = keepUnderFullScale (c);
    s.input = fileOf (std::move (c));
    return s;
}

struct TrackResult
{
    double stepChangeDb = 0, overshootDb = 0, settleS = 0, peakDbfs = 0;
};

/** K-weighted gains, out vs in:
    step change  gain on the loud track (its last 2.5 s) minus on the quiet
                 one (its last 2 s): how far the chain narrows (< 0) the step;
    overshoot    gain over the loud track's first 500 ms minus its last 2.5 s;
    settle       seconds after the change until the gain over two-beat
                 windows (one beat apart) stays within 1 dB of the last
                 2.5 s, read over the 2.5 s after the change (2.34 s, the
                 last window's end: not settled);
    peak         the output's sample peak over the loud track's first second. */
TrackResult measureTrack (const TrackScene& s, const Channels& outRaw)
{
    const auto in = kWeighted (s.input.channels), out = kWeighted (outRaw);
    TrackResult r;
    const double steady = gainDb (out, in, { span (kTrackSteady, kTrackLength) });
    r.stepChangeDb = steady - gainDb (out, in, { span (kTrackChange - 2.0, kTrackChange) });
    r.overshootDb = gainDb (out, in, { span (kTrackChange, kTrackChange + 0.5) }) - steady;
    const double beat = 60.0 / 128.0;
    for (double t = kTrackChange; t + 2.0 * beat <= kTrackSteady; t += beat)
        if (std::abs (gainDb (out, in, { span (t, t + 2.0 * beat) }) - steady) > 1.0)
            r.settleS = t + 2.0 * beat - kTrackChange;
    double peak = 0.0;
    for (const auto& ch : outRaw)
        for (int i = samplesOf (kTrackChange); i < samplesOf (kTrackChange + 1.0); ++i)
            peak = std::max (peak, static_cast<double> (std::abs (ch[static_cast<size_t> (i)])));
    r.peakDbfs = toDb (peak);
    return r;
}
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
    // Re-pinned by docs/11 E04 step 4 and E20 (Phase 3 batch 5), every
    // gaming row: Footsteps and Detail add the Clarity shaper's high-band
    // attack (+2 dB each at 100 %), Impact's attack moved to its low band
    // and its static bass boost and harmonics became the bass engine's
    // event-keyed punch. Moved by more than the tolerance: the combat's
    // event change at -40 LUFS (its HF onsets get the high-band attack too;
    // the cue enhancer's loud cap does not apply to the shaper): Footsteps
    // 100 0.74 -> 1.08 dB, Competitive FPS 0.02 -> 0.46, Horror Detail 1.91
    // -> 2.31; Racing's 4.13 -> 3.82 (Impact 0.35's static boost gone);
    // Cinematic Adventure (Impact 0.5: its 1.3 dB full-band attack lifted
    // the steps' onsets too, now only the low band's) cue SNR gain 0.56 /
    // 1.53 / 2.84 -> 0.11 / 1.16 / 2.41 dB, contrast 0.16 / 0.32 / 0.20 ->
    // -0.45 / -0.26 / -0.40 dB, event change at -40 LUFS 4.51 -> 4.09 dB,
    // onset jump 0.52 -> 0.06 dB. Everything else within 0.3 dB (cue SNR and
    // contrast of the other rows within 0.15 dB, bed lift within 0.1 dB).
    //                      cue SNR, contrast, bed, drift, event, onset jump, hole, recovery (s), step after
    struct Pinned
    {
        const char* file; // nullptr: Gaming mode, Footsteps 100, everything else default (the module)
        double v[3][9];   // [-14 / -24 / -40 LUFS][metric]
    };
    static const Pinned pinned[] = {
        { nullptr,
          { { 6.01, 5.92, -0.01, -0.04, 0.72, -0.09, -0.10, 0.00, 0.27 },
            { 6.18, 6.09, 0.03, 0.03, 0.98, -0.07, -0.03, 0.00, 0.05 },
            { 6.18, 6.09, 0.03, 0.03, 1.08, -0.26, -0.03, 0.00, 0.06 } } },
        { "gaming-competitive-fps.json",
          { { 5.43, 4.61, 0.07, -0.07, 0.22, 0.14, -0.12, 0.00, 0.27 },
            { 6.02, 4.85, 0.36, -0.02, -0.24, 0.08, -0.04, 0.00, 0.08 },
            { 6.85, 4.94, 0.81, -0.04, 0.46, -0.13, -0.04, 0.00, -0.04 } } },
        { "gaming-battle-royale.json",
          { { 4.28, 3.58, -2.49, -0.12, -2.84, 0.28, -0.35, 0.00, 0.35 },
            { 5.18, 4.24, 0.08, -0.04, -2.91, 0.70, -0.23, 0.00, 0.29 },
            { 6.16, 4.69, 0.70, -0.02, 0.33, -0.03, -0.04, 0.00, -0.01 } } },
        // Night Mode: re-pinned by docs/11 E21 Phase 3's retune (Auto Level
        // -20 -> -14 LUFS with the compressor's +6 dB make-up removed and its
        // thresholds, the upward section's and dyneq.0's moved up 6 dB with
        // it; the Startle Guard at 20 LU). Before: bed lift -7.86 / 1.92 /
        // 9.79, event change -8.86 / -6.54 / 2.52, onset jump 0.65 / 3.39 /
        // 0.92, hole -0.47 / 0.79 / -1.80, recovery 0.00 / 1.38 / 4.18. The
        // -24 LUFS hole is now negative (the bed after the event 1.1 dB
        // louder, not quieter): at -24 LUFS the new target asks Auto Level
        // for +10 dB, so it is still rising to its +6 dB cap (1 dB/s) through
        // this 12 s scene. Re-pinned by docs/11 E21's time constants (Phase 3
        // batch 3: Auto Level's 15 s measure, its 10 ms onset gate and 300 ms
        // hold; the Startle Guard's sustained detector): event change -9.03 /
        // -7.44 / -0.05 -> -8.97 / -7.12 / -0.29, onset jump 0.65 / 1.80 /
        // -0.07 -> 0.57 / 1.20 / 0.28, step lift after at -24 LUFS 0.78 ->
        // 1.05 dB, the rest within 0.05 dB.
        { "gaming-night-mode.json",
          { { 1.74, 1.03, -7.91, -0.20, -9.09, 0.56, -0.43, 0.00, 0.01 },
            { 2.01, 1.03, -0.15, 0.75, -7.25, 1.47, -1.19, 2.43, 1.06 },
            { 3.83, 1.96, 3.82, 1.94, -0.33, 0.47, -1.86, 4.18, 2.80 } } },
        { "gaming-horror-detail.json",
          { { 3.07, 2.56, 0.39, -0.08, 0.06, 0.27, -0.25, 0.00, 0.23 },
            { 3.97, 3.09, 2.02, 0.00, -0.06, 0.69, -0.08, 0.00, 0.13 },
            { 4.96, 3.10, 2.66, -0.03, 2.31, -0.02, -0.02, 0.00, -0.05 } } },
        { "gaming-7-1-headphone-surround.json",
          { { 2.49, 1.89, -0.95, -0.09, -1.52, 0.32, -0.27, 0.00, 0.23 },
            { 3.40, 2.41, 2.09, -0.06, -1.36, 0.86, -0.20, 0.00, 0.23 },
            { 4.76, 2.72, 3.07, -0.04, 1.88, 0.00, -0.03, 0.00, -0.07 } } },
        { "gaming-cinematic-adventure.json",
          { { 0.11, -0.45, 0.78, -0.05, 0.16, 0.69, -0.14, 0.00, 0.01 },
            { 1.16, -0.26, 2.91, -0.01, 0.83, 0.95, 0.00, 0.00, -0.11 },
            { 2.41, -0.40, 3.83, -0.07, 4.09, 0.06, -0.02, 0.00, -0.35 } } },
        { "gaming-moba-strategy.json",
          { { -1.30, -1.01, -1.49, -0.03, -2.28, 0.29, -0.19, 0.00, -0.39 },
            { -0.72, -2.28, 2.80, -0.03, -1.34, 1.37, -0.04, 0.00, -0.60 },
            { 2.88, -0.48, 4.22, -0.11, 2.28, 0.13, -0.05, 0.00, -0.46 } } },
        // Racing: re-pinned by docs/11 E04 step 2 (Phase 3 batch 2; its
        // Tighten 0.2 no longer cuts onsets and is applied as a one-pole
        // shelf). Before: onset jump 0.30 / 1.56 / 0.15, step lift after vs
        // before 0.31 / 0.27 / -0.22, cue SNR gain -0.34 / 0.35 / 1.89.
        { "gaming-racing.json",
          { { -0.37, -0.93, -0.81, -0.06, -1.78, 0.57, -0.19, 0.00, -0.17 },
            { 0.35, -0.83, 3.17, -0.04, -0.77, 1.51, -0.06, 0.00, -0.19 },
            { 1.77, -0.47, 4.04, -0.06, 3.82, -0.03, -0.03, 0.00, -0.35 } } },
        { "gaming-tournament-clean.json",
          { { 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00 },
            { 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00 },
            { 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00 } } },
        // Late Night: re-pinned by docs/11 E21 (Phase 3 batch 3): its retune
        // for -20 LUFS (Auto Level -26 LUFS, 10:1 compressor with +11.8 dB
        // make-up, a stronger upward section) and the Auto Level time
        // constants. A leveller now: the -40 LUFS bed is brought up to the
        // target (+7.30 -> +22.56 dB), the -24 LUFS onset jump 6.03 -> 1.76 dB,
        // hole 1.86 -> -0.89 dB, recovery 4.18 -> 2.43 s.
        { "music-late-night-low-volume.json",
          { { -0.02, -0.11, -5.91, 0.42, -6.34, 0.42, -0.30, 0.00, -0.04 },
            { -0.02, -0.16, 4.45, -0.15, -4.53, 1.76, -0.89, 2.43, -0.06 },
            { 0.33, -0.13, 22.56, 0.12, 6.79, 2.56, 0.07, 1.03, -1.37 } } },
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

namespace
{
// The core scene at -60 LUFS (docs/11 E19's quiet material): Horror
// Detail (Detail 0.6) and Competitive FPS, as shipped and with the upward
// section off (upward threshold -80 dB). Before the floor followed the
// background (fixed -75 dBFS): bed lift 7.73 / 2.50 dB against 2.62 /
// 0.77 dB without the upward section, contrast 3.49 / 5.20 dB; at
// -50 LUFS 4.73 / 2.27 dB against 2.75 / 0.86 dB, contrast 2.33 / 4.40 dB
// (FPS 5.25 without it). At -14 / -24 / -40 LUFS the upward section is
// idle on this scene (its threshold is under the bed), so the matrix
// above does not move.
void checkQuietSceneUpward (const char* file, double bedDb, double contrastDb)
{
    const auto scene = makeScene (-60.0);
    const auto shipped = resolve (factoryPreset (file));
    auto noUp = shipped;
    setValue (noUp, CompUpThresholdDb, -80.0f);
    const auto a = measureScene (scene, render (scene.input, shipped));
    const auto b = measureScene (scene, render (scene.input, noUp));
    measured (std::string (file) + " at -60 LUFS: bed lift", a.bedLiftDb, "dB");
    measured (std::string (file) + " at -60 LUFS: bed lift, upward section off", b.bedLiftDb, "dB");
    measured (std::string (file) + " at -60 LUFS: contrast change", a.contrastChangeDb, "dB");
    measured (std::string (file) + " at -60 LUFS: hole 1-2 s after", a.holeDb, "dB");
    CHECK_NEAR (a.bedLiftDb, b.bedLiftDb, 0.1); // the upward section adds nothing to the bed
    CHECK_GE (a.contrastChangeDb, b.contrastChangeDb - 0.1);
    CHECK_NEAR (a.bedLiftDb, bedDb, 0.3);
    CHECK_NEAR (a.contrastChangeDb, contrastDb, 0.3);
    CHECK_LE (std::abs (a.holeDb), 0.3);
}
} // namespace

TEST_CASE ("Scenes: Detail's upward compressor does not lift the bed of the quiet scene - its floor follows the background: Horror Detail (docs/11 E19 step 2)")
{
    checkQuietSceneUpward ("gaming-horror-detail.json", 2.62, 3.60);
}

TEST_CASE ("Scenes: Detail's upward compressor does not lift the bed of the quiet scene - its floor follows the background: Competitive FPS (docs/11 E19 step 2)")
{
    checkQuietSceneUpward ("gaming-competitive-fps.json", 0.77, 5.28);
}

// =============================================================================
// docs/11 E60 stage 1 remainder: dialogue over effects, speech -> music ->
// silence, a quiet -> loud track change. Each pins its presets' values at
// -14 / -24 / -40 LUFS (0.3 dB, 0.5 s for times) and checks the expectation
// its scene is for; where a preset still fails it, the value is pinned and a
// KNOWN_GAP comment names the target.
namespace
{
std::vector<float> presetValues (const char* file)
{
    if (file == nullptr)
    {
        auto v = resolve (RenderOptions {});
        setValue (v, BypassAll, 1.0f);
        return v;
    }
    return resolve (factoryPreset (file));
}
} // namespace

namespace
{
void checkDialogueScene (int l)
{
    // Expectation: no preset makes the dialogue harder to pick out of the
    // effects than bypass does (dialogue SNR gain >= 0). Competitive FPS
    // meets it (its cue enhancer lifts the syllables' onsets in the 3.2 kHz
    // band). KNOWN_GAP: target dialogue SNR gain >= 0 dB (docs/11 E60 stage 1
    // finding; no Done-when yet). MOBA / Strategy (Voice & Score 0.6) and
    // Night Mode lower it by up to 5-6 dB: their downward compressor (1.8:1
    // engaged by Detail in MOBA, 3:1 in Night Mode; Night Mode before docs/11
    // E21 Phase 3's retune -5.86 / -6.18 / -1.31 dB) turns the dialogue down
    // against the stationary effects, and Voice & Score's band 7 - an
    // upward compressor at 2 kHz (BoostBelow) - lifts the effects between
    // the phrases more than the dialogue (MOBA at Voice 0: -4.44 / -3.12 /
    // -0.20 dB; at Detail 0: -1.86 / -2.86 / -1.98 dB).
    //                     dialogue SNR gain, dialogue lift, effects lift
    struct Pinned
    {
        const char* file; // nullptr: bypass
        bool knownGap;
        double v[3][3];
    };
    static const Pinned pinned[] = {
        { nullptr, false, { { 0.00, -0.03, -0.03 }, { 0.00, -0.03, -0.03 }, { 0.00, -0.03, -0.03 } } },
        { "gaming-competitive-fps.json", false, { { 2.90, 2.57, -0.10 }, { 3.00, 3.18, 0.49 }, { 3.15, 3.87, 0.90 } } },
        { "gaming-moba-strategy.json", true, { { -5.30, -4.85, -0.67 }, { -4.07, 0.29, 3.31 }, { -1.98, 3.86, 4.60 } } },
        // Night Mode at -14 LUFS re-pinned by docs/11 E21's time constants
        // (Phase 3 batch 3): -5.75 / -10.91 / -6.19 -> -5.30 / -10.64 / -6.29.
        { "gaming-night-mode.json", true, { { -5.30, -10.64, -6.29 }, { -5.44, -2.95, 1.56 }, { -0.32, 4.92, 4.54 } } },
    };
    const auto scene = makeDialogueScene (kLevels[l]);
    measured ("dialogue scene turned down to stay under -1 dBFS at " + levelName (l), scene.fullScaleDb, "dB");
    for (const auto& p : pinned)
    {
        const auto r = measureDialogue (scene, render (scene.input, presetValues (p.file)));
        const std::string what = std::string (p.file == nullptr ? "bypass" : p.file) + " at " + levelName (l);
        measured (what + ": dialogue SNR gain", r.snrGainDb, "dB");
        measured (what + ": dialogue lift", r.dialogueLiftDb, "dB");
        measured (what + ": effects lift", r.effectsLiftDb, "dB");
        const double got[3] = { r.snrGainDb, r.dialogueLiftDb, r.effectsLiftDb };
        for (int m = 0; m < 3; ++m)
            CHECK_NEAR (got[m], p.v[l][m], p.file == nullptr ? 0.1 : 0.3);
        if (! p.knownGap)
            CHECK_GE (r.snrGainDb, -0.1);
    }
}
} // namespace

TEST_CASE ("Scenes: dialogue over effects - dialogue SNR gain, dialogue and effects lift at -14 LUFS (E60; KnownGap: Voice & Score presets lower the dialogue-to-effects ratio)")
{
    // Metric validation, no render: the input with its dialogue doubled
    // reads +6.02 dB of dialogue SNR gain and dialogue lift and no effects
    // lift; bypass reads 0 (below).
    {
        const auto s = makeDialogueScene (-24.0);
        auto louder = s.input.channels;
        for (size_t ch = 0; ch < 2; ++ch)
            for (size_t i = 0; i < louder[ch].size(); ++i)
                louder[ch][i] += s.dialogue[ch][i];
        const auto r = measureDialogue (s, louder);
        measured ("dialogue doubled: dialogue SNR gain", r.snrGainDb, "dB");
        CHECK_NEAR (r.snrGainDb, 6.02, 0.1);
        CHECK_NEAR (r.dialogueLiftDb, 6.02, 0.1);
        CHECK_NEAR (r.effectsLiftDb, 0.0, 0.01);
    }

    checkDialogueScene (0);
}

TEST_CASE ("Scenes: dialogue over effects - dialogue SNR gain, dialogue and effects lift at -24 LUFS (E60; KnownGap: Voice & Score presets lower the dialogue-to-effects ratio)")
{
    checkDialogueScene (1);
}

TEST_CASE ("Scenes: dialogue over effects - dialogue SNR gain, dialogue and effects lift at -40 LUFS (E60; KnownGap: Voice & Score presets lower the dialogue-to-effects ratio)")
{
    checkDialogueScene (2);
}

namespace
{
void checkSpeechMusicSilence (int l)
{
    // Expectations, docs/11 E21's Done-when read on this scene: the first
    // event after quiet at most 1 dB over the steady state (music onset jump
    // <= +1 dB: met everywhere), back within 1 dB of the level before the
    // event (speech return within +-1 dB), and the ambience lift budget of
    // +6 dB applied to the noise floor (silence lift re speech <= +6 dB).
    // The balance change has no target (pinned only).
    // KNOWN_GAP: speech return within +-1 dB per docs/11 E21 - Night Mode
    // +2.68 dB and Late Night +2.79 dB at -40 LUFS (Auto Level still rising
    // on the quiet speech, 3 dB/s; Night Mode +1.78 dB before docs/11 E21
    // Phase 3's retune, whose 3:1 compressor with 6 dB of make-up took part
    // of the rise back; Late Night +2.45 dB before its Phase 3 batch 3
    // retune, whose stronger upward section lifts the returning speech
    // more). Late Night at -24 LUFS closed by that retune: +1.01 -> +0.02 dB.
    // KNOWN_GAP: silence lift <= +6 dB per docs/11 E21 - Night Mode +8.1 dB
    // at -14 LUFS and Late Night +6.5 dB at -24 LUFS: the downward
    // compressor turns the speech down while the level after it (Night
    // Mode's Auto Level target, Late Night's make-up) lifts the hiss in the
    // pause. (E19's relative floor keeps the upward section off the -80 dBFS
    // hiss.) Night Mode at -24 LUFS closed by the E21 Phase 3 retune: +8.60
    // -> +5.83 dB; Late Night at -14 LUFS by its batch 3 retune and the Auto
    // Level time constants: +7.47 -> +5.81 dB (before: +7.47 / +8.35 dB at
    // -14 / -24 LUFS). Night Mode at -14 LUFS moved with those time
    // constants: silence lift 8.37 -> 8.12 dB, speech return 0.01 -> 0.11 dB.
    //                     balance change, music onset jump, silence lift, silence out (dBFS), speech return
    struct Pinned
    {
        const char* file; // nullptr: bypass
        bool returnGap[3], silenceGap[3];
        double v[3][5];
    };
    static const Pinned pinned[] = {
        { nullptr, { false, false, false }, { false, false, false },
          { { 0.00, 0.00, 0.00, -79.98, 0.00 }, { 0.00, 0.00, 0.00, -79.98, 0.00 }, { 0.00, 0.00, 0.00, -79.98, 0.00 } } },
        { "gaming-night-mode.json", { false, false, true }, { true, false, false },
          { { -1.42, -0.13, 8.12, -79.35, 0.11 }, { -1.08, -1.05, 5.83, -75.74, 0.97 }, { -2.42, -2.52, 0.90, -75.81, 2.68 } } },
        { "music-late-night-low-volume.json", { false, false, true }, { false, true, false },
          { { 1.25, 0.35, 5.81, -79.22, -0.48 }, { 1.76, 0.08, 6.53, -70.42, 0.02 }, { 7.86, -5.96, 2.91, -62.34, 2.79 } } },
    };
    const auto scene = makeSpeechMusicSilence (kLevels[l]);
    measured ("speech -> music -> silence turned down to stay under -1 dBFS at " + levelName (l), scene.fullScaleDb, "dB");
    for (const auto& p : pinned)
    {
        const auto r = measureSms (scene, render (scene.input, presetValues (p.file)));
        const std::string what = std::string (p.file == nullptr ? "bypass" : p.file) + " at " + levelName (l);
        measured (what + ": music vs speech balance change", r.balanceChangeDb, "dB");
        measured (what + ": music onset jump", r.musicOnsetJumpDb, "dB");
        measured (what + ": silence lift re speech", r.silenceLiftDb, "dB");
        measured (what + ": silence output", r.silenceOutDbfs, "dBFS");
        measured (what + ": speech return", r.speechReturnDb, "dB");
        const double got[5] = { r.balanceChangeDb, r.musicOnsetJumpDb, r.silenceLiftDb, r.silenceOutDbfs, r.speechReturnDb };
        for (int m = 0; m < 5; ++m)
            CHECK_NEAR (got[m], p.v[l][m], p.file == nullptr ? 0.1 : 0.3);
        CHECK_LE (r.musicOnsetJumpDb, 1.0);
        if (! p.returnGap[l])
            CHECK_LE (std::abs (r.speechReturnDb), 1.0);
        if (! p.silenceGap[l])
            CHECK_LE (r.silenceLiftDb, 6.0);
    }
}
} // namespace

TEST_CASE ("Scenes: speech -> music -> silence - balance, music onset, silence and speech return at -14 LUFS (E60; KnownGap: E21 onset / return / silence targets)")
{
    checkSpeechMusicSilence (0);
}

TEST_CASE ("Scenes: speech -> music -> silence - balance, music onset, silence and speech return at -24 LUFS (E60; KnownGap: E21 onset / return / silence targets)")
{
    checkSpeechMusicSilence (1);
}

TEST_CASE ("Scenes: speech -> music -> silence - balance, music onset, silence and speech return at -40 LUFS (E60; KnownGap: E21 onset / return / silence targets)")
{
    checkSpeechMusicSilence (2);
}

namespace
{
void checkTrackChange (int l)
{
    // Expectations, docs/11 E21's Done-when read on this scene: the loud
    // track's first 500 ms at most 1 dB over its steady state (overshoot
    // <= +1 dB) and within 1 dB of it after 1 s (settle <= 1 s); the output
    // stays under full scale (peak <= -0.5 dBFS: met everywhere). The step
    // change has no target (pinned only). Night Mode meets all of it at
    // every level, Late Night at -14 and -40 LUFS.
    // KNOWN_GAP: overshoot <= +1 dB and settle <= 1 s per docs/11 E21 - Late
    // Night at -24 LUFS settles after 1.88 s (0 s before docs/11 E21's time
    // constants, Phase 3 batch 3: Auto Level's upper gate now holds the whole
    // loud track out of its measure, so the compressor takes the 15 LU step;
    // the 3 s measure's leak used to take part of it). Late Night's retune
    // (Phase 3 batch 3) closed -14 LUFS (+3.70 dB / 1.41 s -> +0.98 dB / 0 s;
    // flagged all the same, 0.02 dB under the limit, so that a compiler's
    // rounding cannot fail it) and -40 LUFS (-2.56 dB / 2.34 s -> +0.91 dB /
    // 0 s). Night Mode closed by docs/11 E21 Phase 3's retune (before: +2.00
    // dB / 1.88 s at -14 LUFS, -3.64 dB / 2.34 s at -40 LUFS): its
    // compressor now works on the level Auto Level delivers instead of
    // adding 6 dB after it; the time constants moved its step change -4.66 /
    // -0.88 -> -5.39 / -2.32 dB and overshoot -0.16 / -0.95 -> +0.45 / +0.36
    // dB at -14 / -24 LUFS (settle 0.94 -> 0 s at -24).
    //                     step change, overshoot, settle (s), peak (dBFS)
    struct Pinned
    {
        const char* file; // nullptr: bypass
        bool knownGap[3];
        double v[3][4];
    };
    static const Pinned pinned[] = {
        { nullptr, { false, false, false }, { { -0.01, 0.00, 0.00, -2.51 }, { 0.00, 0.00, 0.00, -8.41 }, { 0.00, 0.00, 0.00, -24.40 } } },
        { "gaming-night-mode.json", { false, false, false }, { { -5.39, 0.45, 0.00, -9.50 }, { -2.32, 0.36, 0.00, -12.14 }, { 2.00, -1.21, 0.00, -24.82 } } },
        { "music-late-night-low-volume.json", { true, true, false }, { { -14.83, 0.98, 0.00, -2.20 }, { -15.79, 0.81, 1.88, -2.89 }, { -1.26, 0.91, 0.00, -2.52 } } },
    };
    const auto scene = makeTrackChange (kLevels[l]);
    measured ("track change turned down to stay under -1 dBFS at " + levelName (l), scene.fullScaleDb, "dB");
    for (const auto& p : pinned)
    {
        const auto r = measureTrack (scene, render (scene.input, presetValues (p.file)));
        const std::string what = std::string (p.file == nullptr ? "bypass" : p.file) + " at " + levelName (l);
        measured (what + ": step change", r.stepChangeDb, "dB");
        measured (what + ": overshoot", r.overshootDb, "dB");
        measured (what + ": settle", r.settleS, "s");
        measured (what + ": peak", r.peakDbfs, "dBFS");
        const double got[4] = { r.stepChangeDb, r.overshootDb, r.settleS, r.peakDbfs };
        for (int m = 0; m < 4; ++m)
            CHECK_NEAR (got[m], p.v[l][m], m == 2 ? 0.5 : p.file == nullptr ? 0.1 : 0.3);
        CHECK_LE (r.peakDbfs, -0.5);
        if (! p.knownGap[l])
        {
            CHECK_LE (r.overshootDb, 1.0);
            CHECK_LE (r.settleS, 1.0);
        }
    }
}
} // namespace

TEST_CASE ("Scenes: quiet -> loud track change - step change, overshoot, settling and peak at -14 LUFS (E60; KnownGap: E21 overshoot / settle targets)")
{
    checkTrackChange (0);
}

TEST_CASE ("Scenes: quiet -> loud track change - step change, overshoot, settling and peak at -24 LUFS (E60; KnownGap: E21 overshoot / settle targets)")
{
    checkTrackChange (1);
}

TEST_CASE ("Scenes: quiet -> loud track change - step change, overshoot, settling and peak at -40 LUFS (E60; KnownGap: E21 overshoot / settle targets)")
{
    checkTrackChange (2);
}

// =============================================================================
// docs/11 E21 Phase 3: the Startle Guard (guard.range, StartleGuard.h) on a
// competitive preset. Its Done-when: with the guard on, the event over the
// ambience <= N + 1 LU and the step band within 1 dB of its pre-event gain
// after 1 s; and the first combat event after the quiet <= the steady-state
// events + 1 dB. Event over ambience: the loudest 400 ms K-weighted window
// over the combat (every 50 ms, to 400 ms after it) minus the K-weighted
// power of the ambience and its steps at 3 - 5 s, of the output.
namespace
{
double eventOverAmbienceLu (const Channels& c)
{
    const auto k = kWeighted (c);
    double loudest = 0.0;
    for (double t = kCombatStart; t + 0.4 <= kCombatEnd + 0.4; t += 0.05)
        loudest = std::max (loudest, meanPower (k, { span (t, t + 0.4) }));
    return powerDb (loudest) - powerDb (meanPower (k, { span (3.0, kCombatStart) }));
}

void checkGuardOnCompetitive (int l, std::initializer_list<GuardRangeValue> ranges)
{
    const auto scene = makeScene (kLevels[l]);
    const auto shipped = resolve (factoryPreset ("gaming-competitive-fps.json"));
    const auto off = measureScene (scene, render (scene.input, shipped));
    measured ("event over ambience, input, at " + levelName (l), eventOverAmbienceLu (scene.input.channels), "LU");
    for (auto range : ranges)
    {
        auto values = shipped;
        setValue (values, GuardRange, static_cast<float> (range));
        const auto out = render (scene.input, values);
        const auto r = measureScene (scene, out);
        const float ceiling = StartleGuard::ceilingLuFor (static_cast<int> (range));
        const std::string what = "Competitive FPS, guard " + std::to_string (static_cast<int> (ceiling)) + " LU, at " + levelName (l);
        const double event = eventOverAmbienceLu (out);
        measured (what + ": event over ambience", event, "LU");
        measured (what + ": event change", r.eventChangeDb, "dB");
        measured (what + ": onset jump", r.onsetJumpDb, "dB");
        measured (what + ": step lift after vs before", r.stepAfterDb, "dB");
        measured (what + ": recovery", r.recoveryS, "s");
        CHECK_LE (event, ceiling + 1.0);
        CHECK_LE (r.onsetJumpDb, 1.0);
        CHECK_LE (std::abs (r.stepAfterDb), 1.0);
        CHECK_LE (r.recoveryS, 1.0);
        // The ambience and its steps are not the guard's business.
        CHECK_NEAR (r.bedLiftDb, off.bedLiftDb, 0.05);
        CHECK_NEAR (r.contrastChangeDb, off.contrastChangeDb, 0.05);
        CHECK_LE (r.eventChangeDb, off.eventChangeDb + 0.05);
    }
}
} // namespace

TEST_CASE ("Scenes: the Startle Guard (10 / 6 LU) on Competitive FPS at -24 LUFS - event over ambience <= N + 1 LU, onset jump <= 1 dB, the step band within 1 dB after 1 s, the ambience untouched (docs/11 E21)")
{
    checkGuardOnCompetitive (1, { GuardRangeValue::Lu10Balanced, GuardRangeValue::Lu6Shield });
}

TEST_CASE ("Scenes: the Startle Guard (15 / 10 LU) on Competitive FPS at -40 LUFS - event over ambience <= N + 1 LU, onset jump <= 1 dB, the step band within 1 dB after 1 s, the ambience untouched (docs/11 E21)")
{
    checkGuardOnCompetitive (2, { GuardRangeValue::Lu15, GuardRangeValue::Lu10Balanced });
}

TEST_CASE ("Scenes: the Startle Guard (6 LU) on Competitive FPS at -40 LUFS - event over ambience <= N + 1 LU, onset jump <= 1 dB, the step band within 1 dB after 1 s, the ambience untouched (docs/11 E21)")
{
    checkGuardOnCompetitive (2, { GuardRangeValue::Lu6Shield });
}

TEST_CASE ("SceneEvents: the background tracker's law, and the detector finds the steps, the combat, the silence and the track change of the scenes (E60)")
{
    // BackgroundTracker (flub/dsp/BackgroundTracker.h), stepped at 1 kHz:
    // learns a steady level within 300 ms, rises 5 dB/s, falls with 400 ms,
    // never under its floor; a 40 ms cue moves it by at most 0.2 dB.
    BackgroundTracker t;
    t.prepare (1000.0);
    for (int i = 0; i < 300; ++i)
        t.update (-60.0f, -90.0f);
    CHECK (! t.isLearning());
    CHECK_NEAR (t.get(), -60.0, 1e-3);
    for (int i = 0; i < 40; ++i)
        t.update (-40.0f, -90.0f);
    CHECK_NEAR (t.get(), -59.8, 1e-3);
    for (int i = 0; i < 960; ++i)
        t.update (-40.0f, -90.0f);
    CHECK_NEAR (t.get(), -55.0, 1e-2); // 1 s at 5 dB/s
    for (int i = 0; i < 400; ++i)
        t.update (-60.0f, -90.0f);
    CHECK_NEAR (t.get(), -60.0 + 5.0 * std::exp (-1.0), 0.05); // one 400 ms time constant
    for (int i = 0; i < 5000; ++i)
        t.update (-160.0f, -90.0f);
    CHECK_NEAR (t.get(), -90.0, 1e-6);
    t.update (std::numeric_limits<float>::quiet_NaN(), -90.0f);
    CHECK_NEAR (t.get(), -90.0, 1e-6);

    const auto count = [] (const SceneAnalysis& a, SceneEventType type) {
        return std::count_if (a.events.begin(), a.events.end(), [type] (const SceneEvent& e) { return e.type == type; });
    };
    const auto first = [] (const SceneAnalysis& a, SceneEventType type) {
        return *std::find_if (a.events.begin(), a.events.end(), [type] (const SceneEvent& e) { return e.type == type; });
    };

    // The core scene in the steps' band (onsets 3 dB over the background):
    // the steps (4 dB over the bed in the band) as onsets within a frame or
    // two of their start, and the combat as one loud event (the background
    // is held through it, so the steps right after it are found too).
    {
        const auto scene = makeScene (-24.0);
        SceneEventSettings band;
        band.bandHz = kStepBandHz;
        band.bandQ = kStepBandQ;
        band.onsetDb = 3.0f;
        const auto a = analyseSceneEvents (scene.input.channels, kFs, band);
        int found = 0, spurious = 0;
        for (const auto& e : a.events)
        {
            if (e.type != SceneEventType::Onset)
                continue;
            const bool step = std::any_of (scene.steps.begin(), scene.steps.end(), [&] (const StepWindow& st) { return std::abs (e.startSeconds - st.onset) <= 0.03; });
            (step ? found : spurious) += 1;
        }
        measured ("detector: steps found as onsets (of " + std::to_string (scene.steps.size()) + ")", found, "steps");
        measured ("detector: onsets that are not steps", spurious, "onsets");
        CHECK_GE (found, 18); // 20 of 23: three steps rise under 3 dB over the bed in this band
        CHECK_LE (spurious, 1); // the end of the combat's last shot
        REQUIRE (count (a, SceneEventType::Loud) == 1);
        const auto loud = first (a, SceneEventType::Loud);
        measured ("detector: combat from", loud.startSeconds, "s");
        measured ("detector: combat to", loud.endSeconds, "s");
        CHECK_NEAR (loud.startSeconds, kCombatStart, 0.02);
        CHECK_NEAR (loud.endSeconds, kCombatEnd - 0.1, 0.1);
        CHECK (count (a, SceneEventType::LevelChange) == 0);
    }
    // Speech -> music -> silence: the 1.5 s silence (the -80 dBFS hiss).
    {
        const auto a = analyseSceneEvents (makeSpeechMusicSilence (-24.0).input.channels, kFs);
        const auto silence = std::find_if (a.events.begin(), a.events.end(), [] (const SceneEvent& e) {
            return e.type == SceneEventType::Silence && std::abs (e.startSeconds - kSilenceFrom) < 0.05;
        });
        REQUIRE (silence != a.events.end());
        CHECK_NEAR (silence->endSeconds, kSpeechAgain, 0.05);
        CHECK_NEAR (silence->levelDb, -79.7, 0.3);
    }
    // Track change: one level change, at the change, about the 15 LU step
    // (the median frame levels of the two tracks differ by 17.3 dB).
    {
        const auto a = analyseSceneEvents (makeTrackChange (-24.0).input.channels, kFs);
        REQUIRE (count (a, SceneEventType::LevelChange) == 1);
        const auto change = first (a, SceneEventType::LevelChange);
        measured ("detector: track change at", change.startSeconds, "s");
        measured ("detector: track change step", change.overBackgroundDb, "dB");
        CHECK_NEAR (change.startSeconds, kTrackChange, 0.1);
        CHECK_NEAR (change.overBackgroundDb, 17.3, 0.5);
    }
    // Dialogue over stationary effects at 0 LU: nothing stands out by 6 dB,
    // nothing changes level.
    CHECK (analyseSceneEvents (makeDialogueScene (-24.0).input.channels, kFs).events.empty());
}

// =============================================================================
// docs/11 E19 step 4: the cue enhancer (DynamicEq's CueLift bands) and the
// upward compressor's relative floor keep their backgrounds, and the cue
// enhancer its loud cap, in the terms of the level before Auto Level's gain
// (setReferenceOffsetDb, which ProcessingChain::process hands over every
// block), and the cue gate's optional onset-flux key.
namespace
{
constexpr int kBlock = 512;

/** The first 5 s of the core scene (the bed and the steps, before the combat). */
Channels quietPart (const Scene& s)
{
    Channels c = s.input.channels;
    for (auto& ch : c)
        ch.resize (static_cast<size_t> (samplesOf (kCombatStart)));
    return c;
}

/** Cue SNR gain (see the header) over the quiet part's steps from 1.5 s. */
double cueSnrGainDb (const Scene& s, const Channels& in, const Channels& out)
{
    const auto quiet = stepsBetween (s, 1.5, kCombatStart);
    const auto qs = stepWindows (quiet), qb = bedWindows (quiet);
    const auto inBand = bandPass (in), outBand = bandPass (out);
    const double inStep = meanPower (inBand, qs) - meanPower (inBand, qb), outStep = meanPower (outBand, qs) - meanPower (outBand, qb);
    const double inRest = meanPower (in, qb) - meanPower (inBand, qb), outRest = meanPower (out, qb) - meanPower (outBand, qb);
    return (powerDb (outStep) - powerDb (outRest)) - (powerDb (inStep) - powerDb (inRest));
}

/** Gaming mode's Footsteps bands at 100 % (ProcessingChain configureModeBands). */
void prepareCueBands (DynamicEq& d, bool onsetFlux)
{
    d.prepare (ProcessSpec { kFs, kBlock, 2 });
    DynEqBandParams p;
    p.enabled = true;
    p.mode = DynEqMode::CueLift;
    p.frequency = 3200.0f;
    p.q = 0.9f;
    p.rangeDb = 7.0f;
    p.attackMs = 1.0f;
    p.releaseMs = 40.0f;
    p.noiseFloorDb = -75.0f;
    p.cueOnsetFlux = onsetFlux;
    d.setBand (4, p);
    p.frequency = 260.0f;
    p.q = 1.2f;
    p.rangeDb = 3.0f;
    p.attackMs = 2.0f;
    p.releaseMs = 60.0f;
    d.setBand (5, p);
    d.reset();
}

/** Runs `m` over `in` times an upstream gain of `stepDb` from `stepAt`
    seconds on (block-aligned, as Auto Level's per-block gain), handing the
    gain over with setReferenceOffsetDb if `handOver`. `perBlock` is called
    after each block with its start time. */
template <class Module, class PerBlock>
Channels runUpstream (Module& m, const Channels& in, double stepAt, double stepDb, bool handOver, PerBlock perBlock)
{
    const int n = static_cast<int> (in[0].size());
    Planar buf (2, n);
    buf.ch[0] = in[0];
    buf.ch[1] = in[1];
    buf.rebind();
    const float g = static_cast<float> (std::pow (10.0, stepDb / 20.0));
    for (int pos = 0; pos < n; pos += kBlock)
    {
        const int len = std::min (kBlock, n - pos);
        const bool after = pos >= samplesOf (stepAt);
        if (after)
            for (auto& ch : buf.ch)
                for (int i = pos; i < pos + len; ++i)
                    ch[static_cast<size_t> (i)] *= g;
        m.setReferenceOffsetDb (handOver && after ? static_cast<float> (stepDb) : 0.0f);
        m.process (buf.block (pos, len));
        perBlock (pos / kFs);
    }
    return buf.ch;
}

/** Band power (3.2 kHz) of `out` over windows, minus `gainDb`, re `ref`'s. */
double bandDeviationDb (const Channels& out, double gainDb, const Channels& ref, const std::vector<Window>& w)
{
    return powerDb (meanPower (bandPass (out), w)) - gainDb - powerDb (meanPower (bandPass (ref), w));
}
} // namespace

TEST_CASE ("Scenes: an upstream gain step does not move the cue enhancer when it is handed over - the steps keep their lift and the bed stays unlifted (docs/11 E19 step 4)")
{
    // The quiet part of the core scene at -40 LUFS through the Footsteps 100
    // bands alone, with a +10 / -10 dB broadband gain from 2 s on (an
    // upstream gain move far faster than Auto Level's 3 / 4 dB/s, so the
    // difference shows). Deviation = the steps' / the bed's 3.2 kHz band
    // power over the second after the move, minus the gain, re the same
    // bands without it. Handed over (the background and the loud cap in the
    // terms of the level before the gain) it stays within 0.05 dB; not
    // handed over, +10 dB reads as the bed rising out of its background
    // (the background climbs at 5 dB/s, so the bed is lifted for about 2 s)
    // and -10 dB sinks the steps under it (they lose their lift until the
    // background has fallen, 400 ms time constant).
    const auto scene = makeScene (-40.0);
    const auto in = quietPart (scene);
    const auto after = stepsBetween (scene, 2.0, 3.0);
    const auto none = [] (double) {};
    DynamicEq ref;
    prepareCueBands (ref, false);
    const auto plain = runUpstream (ref, in, 2.0, 0.0, false, none);
    for (const double stepDb : { 10.0, -10.0 })
    {
        DynamicEq a, b;
        prepareCueBands (a, false);
        prepareCueBands (b, false);
        const auto handed = runUpstream (a, in, 2.0, stepDb, true, none);
        const auto notHanded = runUpstream (b, in, 2.0, stepDb, false, none);
        const std::string what = std::string (stepDb > 0.0 ? "+10" : "-10") + " dB upstream at 2 s, ";
        const double hStep = bandDeviationDb (handed, stepDb, plain, stepWindows (after));
        const double hBed = bandDeviationDb (handed, stepDb, plain, bedWindows (after));
        const double nStep = bandDeviationDb (notHanded, stepDb, plain, stepWindows (after));
        const double nBed = bandDeviationDb (notHanded, stepDb, plain, bedWindows (after));
        measured (what + "handed over: step deviation", hStep, "dB");
        measured (what + "handed over: bed deviation", hBed, "dB");
        measured (what + "not handed over: step deviation", nStep, "dB");
        measured (what + "not handed over: bed deviation", nBed, "dB");
        CHECK_LE (std::abs (hStep), 0.05);
        CHECK_LE (std::abs (hBed), 0.05);
        if (stepDb > 0.0)
            CHECK_GE (nBed, 1.0);
        else
            CHECK_LE (nStep, -1.0);
    }
}

TEST_CASE ("Scenes: an upstream gain step does not move the upward compressor's relative floor when it is handed over (docs/11 E19 step 4)")
{
    // A -50 dBFS pink bed through the upward section alone (relative floor,
    // threshold -20 dB, 3:1, up to 6 dB, no downward section), +10 dB
    // broadband from 2 s on. Handed over, the background moves with the gain
    // and the bed stays unlifted; not handed over it reads as 10 dB out of
    // its background and gets the upward lift until the background catches up.
    const int n = samplesOf (4.0);
    const auto bed = highPass (pinkNoise (n, std::pow (10.0f, -50.0f / 20.0f), 1357), 40.0);
    const Channels in { bed, bed };
    CompressorParams p;
    p.thresholdDb = 0.0f;
    p.ratio = 1.0f;
    p.upThresholdDb = -20.0f;
    p.upRatio = 3.0f;
    p.upMaxGainDb = 6.0f;
    p.upRelativeFloor = true;
    const auto run = [&] (double stepDb, bool handOver, double& maxUpDb) {
        Compressor c;
        c.setParams (p);
        c.prepare (ProcessSpec { kFs, kBlock, 2 });
        maxUpDb = 0.0;
        const auto out = runUpstream (c, in, 2.0, stepDb, handOver, [&] (double t) {
            if (t >= 2.0)
                maxUpDb = std::max (maxUpDb, static_cast<double> (c.getUpwardGainDb()));
        });
        return out;
    };
    double plainUp = 0.0, handedUp = 0.0, notHandedUp = 0.0;
    const auto plain = run (0.0, false, plainUp);
    const auto handed = run (10.0, true, handedUp);
    const auto notHanded = run (10.0, false, notHandedUp);
    const std::vector<Window> w { span (2.05, 3.0) }; // after the 2 ms look-ahead
    const double hBed = powerDb (meanPower (handed, w)) - 10.0 - powerDb (meanPower (plain, w));
    const double nBed = powerDb (meanPower (notHanded, w)) - 10.0 - powerDb (meanPower (plain, w));
    measured ("upward lift, no gain step: largest after 2 s", plainUp, "dB");
    measured ("+10 dB upstream at 2 s, handed over: largest upward lift", handedUp, "dB");
    measured ("+10 dB upstream at 2 s, handed over: bed deviation", hBed, "dB");
    measured ("+10 dB upstream at 2 s, not handed over: largest upward lift", notHandedUp, "dB");
    measured ("+10 dB upstream at 2 s, not handed over: bed deviation", nBed, "dB");
    CHECK_NEAR (handedUp, plainUp, 0.1);
    CHECK_LE (std::abs (hBed), 0.05);
    CHECK_GE (notHandedUp, plainUp + 2.0);
    CHECK_GE (nBed, 1.0);
}

namespace
{
// The cue enhancer's own lift in a preset: the cue SNR gain over the quiet
// part of the core scene (steps 1.5 - 5 s) as shipped minus with Footsteps
// 0, so the rest of the chain cancels (Night Mode's 3:1 compressor squeezes
// the steps of the loud programme: its whole cue SNR gain is 1.73 / 2.03 /
// 3.87 dB, 1.73 / 2.66 / 4.21 dB with Auto Level off). docs/11 E19
// Done-when: within +-1 dB across -14 / -24 / -40 LUFS, as for the module
// (the matrix above), with Auto Level's gain handed over.
void checkOwnLiftAcrossLevels (const char* name, const char* file, bool firstRunWithAutoLevel, const double (&pinnedDb)[3])
{
    double lift[3] = {};
    for (int l = 0; l < 3; ++l)
    {
        const auto scene = makeScene (kLevels[l]);
        const auto in = quietPart (scene);
        auto shipped = resolve (factoryPreset (file));
        if (firstRunWithAutoLevel)
        {
            setValue (shipped, BoostIntensity, std::min (shipped[static_cast<size_t> (BoostIntensity)], 0.20f));
            setValue (shipped, AutoLevelOn, 1.0f);
            setValue (shipped, AutoLevelTargetLufs, -14.0f);
        }
        auto without = shipped;
        setValue (without, Macro1, 0.0f);
        lift[l] = cueSnrGainDb (scene, in, render (fileOf (in), shipped)) - cueSnrGainDb (scene, in, render (fileOf (in), without));
        measured (std::string (name) + ": cue enhancer's own lift at " + levelName (l), lift[l], "dB");
        CHECK_NEAR (lift[l], pinnedDb[l], 0.3);
    }
    for (int l = 1; l < 3; ++l)
        CHECK_NEAR (lift[l], lift[0], 1.0);
}
} // namespace

TEST_CASE ("Scenes: the cue enhancer's own lift is the same at -14 / -24 / -40 LUFS in Night Mode, with Auto Level's gain handed over (docs/11 E19 step 4)")
{
    // Before the hand-over (Auto Level's gain seen by the backgrounds): 2.08 / 2.11 / 2.37 dB.
    checkOwnLiftAcrossLevels ("Night Mode", "gaming-night-mode.json", false, { 2.08, 2.08, 2.33 });
}

TEST_CASE ("Scenes: the cue enhancer's own lift is the same at -14 / -24 / -40 LUFS in First Run - Game with Auto Level on, with its gain handed over (docs/11 E19 step 4)")
{
    // "First Run - Game" is the app's Game strip default (Competitive FPS,
    // Boost capped at 0.20, EngineController.cpp). It has no Auto Level (4.89
    // / 4.97 / 5.06 dB, bit-identical), so here it runs with Auto Level on at
    // -14 LUFS. Before the hand-over: 4.90 / 5.14 / 5.21 dB.
    checkOwnLiftAcrossLevels ("First Run - Game, Auto Level on", "gaming-competitive-fps.json", true, { 4.91, 5.08, 5.14 });
}

TEST_CASE ("Scenes: the cue gate's onset-flux key - a slow swell out of the bed is not lifted, the steps keep their lift (docs/11 E19)")
{
    // cueOnsetFlux (DynamicEq.h; off by default, and in every preset): an
    // event must climb from 2 to 4.5 dB over the background within 15 ms of
    // crossing 2 dB after 30 ms under it. The Footsteps 100 bands alone.
    // Swell: 3.2 kHz band noise in a -45 dBFS pink bed (the core scene's,
    // without steps), from 30 dB under the bed's band power up to 10 dB over
    // it at 20 dB/s from 1 s, then held: its lift = the 3.2 kHz band power
    // over 2.5 - 4.5 s (10 dB out of the bed and more), out vs in, and the
    // largest applied gain from 1 s. Steps: the quiet part of the core scene
    // (cue SNR gain) at -14 / -24 / -40 LUFS, and 20 / 40 / 80 ms bursts of
    // the same band noise at -60 dBFS out of digital silence (lift = band
    // power over the bursts, out vs in).
    const int n = samplesOf (4.5);
    const float bedRms = std::pow (10.0f, -45.0f / 20.0f);
    Channels swell { highPass (pinkNoise (n, bedRms, 1357), 40.0), highPass (pinkNoise (n, bedRms, 2468), 40.0) };
    const double bedBand = std::sqrt (meanPower (bandPass (swell), { span (0.5, 1.0) }));
    const auto band = bandPass (whiteNoise (n, 1.0f, 777), kStepBandHz, kStepBandQ);
    const double bandRms = rms (band.data(), n);
    for (int i = samplesOf (1.0); i < n; ++i)
    {
        const double db = std::min (-30.0 + 20.0 * (i / kFs - 1.0), 10.0);
        const double v = bedBand / bandRms * std::pow (10.0, db / 20.0) * band[static_cast<size_t> (i)];
        swell[0][static_cast<size_t> (i)] += static_cast<float> (v);
        swell[1][static_cast<size_t> (i)] += static_cast<float> (v);
    }
    Channels bursts (2, std::vector<float> (static_cast<size_t> (n), 0.0f));
    std::vector<Window> burstWindows[3];
    const double burstGain = std::pow (10.0, -60.0 / 20.0) / bandRms / std::sqrt (3.0 / 8.0);
    for (int k = 0; k < 9; ++k)
    {
        const int onset = samplesOf (0.5 + 0.4 * k), len = samplesOf (k % 3 == 0 ? 0.02 : k % 3 == 1 ? 0.04 : 0.08);
        for (int i = 0; i < len; ++i)
        {
            const float v = static_cast<float> (burstGain * (0.5 - 0.5 * std::cos (kTwoPi * i / len)) * band[static_cast<size_t> (onset + i)]);
            bursts[0][static_cast<size_t> (onset + i)] = v;
            bursts[1][static_cast<size_t> (onset + i)] = v;
        }
        burstWindows[k % 3].push_back ({ onset, onset + len });
    }

    const auto none = [] (double) {};
    double swellLift[2] = {}, maxGain[2] = {}, snr[2][3] = {}, burstLift[2][3] = {};
    for (int flux = 0; flux < 2; ++flux)
    {
        DynamicEq d;
        prepareCueBands (d, flux == 1);
        const auto out = runUpstream (d, swell, 0.0, 0.0, false, [&] (double t) {
            if (t >= 1.0)
                maxGain[flux] = std::max (maxGain[flux], static_cast<double> (d.getBandGainDb (4)));
        });
        swellLift[flux] = bandDeviationDb (out, 0.0, swell, { span (2.5, 4.5) });
        for (int l = 0; l < 3; ++l)
        {
            const auto scene = makeScene (kLevels[l]);
            const auto in = quietPart (scene);
            DynamicEq e;
            prepareCueBands (e, flux == 1);
            snr[flux][l] = cueSnrGainDb (scene, in, runUpstream (e, in, 0.0, 0.0, false, none));
        }
        DynamicEq b;
        prepareCueBands (b, flux == 1);
        const auto burstOut = runUpstream (b, bursts, 0.0, 0.0, false, none);
        for (int d3 = 0; d3 < 3; ++d3)
            burstLift[flux][d3] = bandDeviationDb (burstOut, 0.0, bursts, burstWindows[d3]);
    }
    const char* gate[2] = { "gate on level only", "onset flux" };
    for (int flux = 0; flux < 2; ++flux)
    {
        measured (std::string ("20 dB/s swell out of the bed, 3.2 kHz lift over 2.5 - 4.5 s: ") + gate[flux], swellLift[flux], "dB");
        measured (std::string ("20 dB/s swell out of the bed, largest applied 3.2 kHz gain: ") + gate[flux], maxGain[flux], "dB");
        for (int l = 0; l < 3; ++l)
            measured ("steps under the bed at " + levelName (l) + ", cue SNR gain: " + gate[flux], snr[flux][l], "dB");
        measured (std::string ("-60 dBFS bursts out of silence, lift 20 / 40 / 80 ms: ") + gate[flux] + ", 20 ms", burstLift[flux][0], "dB");
        measured (std::string ("-60 dBFS bursts out of silence, lift 20 / 40 / 80 ms: ") + gate[flux] + ", 40 ms", burstLift[flux][1], "dB");
        measured (std::string ("-60 dBFS bursts out of silence, lift 20 / 40 / 80 ms: ") + gate[flux] + ", 80 ms", burstLift[flux][2], "dB");
    }
    CHECK_GE (swellLift[0], 2.0);
    CHECK_LE (swellLift[1], 0.4);
    CHECK_LE (maxGain[1], 2.5); // the first 15 ms of a rise get the ordinary gate's lift
    for (int l = 0; l < 3; ++l)
        CHECK_NEAR (snr[1][l], snr[0][l], 0.2);
    for (int d3 = 0; d3 < 3; ++d3)
        CHECK_NEAR (burstLift[1][d3], burstLift[0][d3], 0.1);
}
