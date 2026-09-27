// KNOWN_GAP metrics: the docs/11 E59 slice (#e59, §3.2). Each test pins a
// stimulus and a metric definition, renders through the CLI's offline
// renderer (the real-time chain, latency compensated, primed) and asserts
// TODAY's value with a small tolerance - not the target. Each assertion
// carries a "KNOWN_GAP: target ... per docs/11 E##" comment.
//
// So a change that moves a metric, on purpose or not, fails here: the fix
// that closes a gap updates the expectation to its target on purpose (and
// flips it to a CHECK_LE / CHECK_GE bound); a change that moves it without
// meaning to is caught. Tolerances cover compiler and FMA differences only
// (0.05-0.5 dB, 2 % for time shares), not tuning; gcc 13 and clang 18
// (Linux x86-64, Release) measure identical values to 0.01 dB.
//
//   * pumping: a 2 kHz tone under 55 Hz kicks at Boost 100 (E05, E02)
//   * 60 Hz and 1 kHz THD+N of a -6 dBFS sine at 12 dB maximizer drive (E05)
//   * 7.1 LFE fold, virtualiser off and on, against one main channel (E01;
//     the LFE is folded since E01, the +10 dB target is still open)
//   * stereo content in an 8-channel container: centre notch and FL-only
//     separation of the fold (E27; closed by the stereo passthrough fold)
//   * burst-footstep lift on isolated 20 / 40 / 80 ms bursts, and step/bed
//     contrast at -14 / -24 / -40 LUFS (and -50 LUFS) (E19; closed by the
//     cue enhancer, kept as its regression tests; tests/test_scenes.cpp
//     has the E60 scenes)
//   * the Night Mode ambush scene: bed lift and post-event hole (E21; the
//     hole closed by the AutoLevel slice, the bed lift still open)
//   * kick onset: Punch 100 lift at 0-10 ms against 10-30 ms (E04, E05)
//   * 30 Hz audible-band (>= 120 Hz) energy of the laptop preset (E03; closed
//     by the preset slice, kept as its regression test)
//   * 3 kHz ILD added by Positional Focus (E24; closed by its 3 dB cap)
//   * 3.2 kHz lift at Bluetooth hands-free rates, 8 / 16 / 32 kHz (E17)
//   * protection (E06 slice, E10 Phase 1): integrated-loudness spread of
//     governed macros over host block sizes 64..4096 (closed); disturbance
//     of a single 1e30 sample and of a NaN burst (closed); DC after the
//     maximizer at 24 dB drive per latency profile; 50 Hz THD+N of all Music
//     macros at protection strength Off / Normal / Strict
//
// Every test prints its measured values ("    measured ...") so a tuning
// session reads the numbers from one run of `flub_tests KnownGap`. The last
// tests check the metric code itself (injected artefacts) and the CLI's
// `render.stats` against a hand computation.
#include "TestFramework.h"
#include "TestSignals.h"

#include "Analysis.h"
#include "CliOptions.h"
#include "Commands.h"
#include "OfflineRenderer.h"

#include "flub/common/Denormals.h"
#include "flub/common/Math.h"
#include "flub/engine/Parameters.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/engine/Protection.h"
#include "flub/io/PresetIO.h"
#include "flub/io/WavFile.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
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

// ---- parameters and rendering -----------------------------------------------
/** Base values exactly as `flubsound-cli process` resolves these options. */
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

/** A factory preset in the latency profile it suggests, as `process -p <file>
    --profile <suggestedLatencyProfile>` resolves it: presets no longer set
    the profile (docs/11 E40), and these metrics were baselined in it. */
RenderOptions factoryPreset (const char* file)
{
    RenderOptions o;
    o.presetSpec = std::string (FLUB_PRESET_DIR) + "/" + file;
    preset::Preset p;
    std::string error;
    if (preset::load (o.presetSpec, p, error))
        o.profile = p.suggestedLatencyProfile;
    return o;
}

RenderOptions boosted (ModeValue mode, float percent)
{
    RenderOptions o;
    o.mode = mode;
    o.boostPercent = percent;
    return o;
}

void setValue (std::vector<float>& v, int id, float x) { v[static_cast<size_t> (id)] = x; }

/** Switches every module off except `keep` (Auto Level stays at its value). */
void onlyModules (std::vector<float>& v, std::initializer_list<int> keep)
{
    for (int id : { GateOn, EqOn, DynEqOn, BassOn, ClarityOn, SaturationOn, SpatialOn, VirtualizerOn, CompressorOn, MaximizerOn })
        setValue (v, id, std::find (keep.begin(), keep.end(), id) != keep.end() ? 1.0f : 0.0f);
}

io::AudioFileData fileOf (Channels channels)
{
    io::AudioFileData d;
    d.sampleRate = kFs;
    d.numChannels = static_cast<int> (channels.size());
    d.channels = std::move (channels);
    return d;
}

io::AudioFileData stereoOf (const std::vector<float>& mono) { return fileOf ({ mono, mono }); }

/** One latency-compensated pass at 512-sample blocks: 2 channels of input length. */
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

// ---- measurement ------------------------------------------------------------
std::vector<float> midOf (const Channels& c)
{
    std::vector<float> m (c[0].size());
    for (size_t i = 0; i < m.size(); ++i)
        m[i] = 0.5f * (c[0][i] + c[1][i]);
    return m;
}

/** Mean square over the windows. */
double meanPower (const std::vector<float>& x, const std::vector<Window>& windows)
{
    double acc = 0.0;
    int64_t n = 0;
    for (const auto& w : windows)
        for (int i = w.first; i < w.second; ++i)
        {
            acc += static_cast<double> (x[static_cast<size_t> (i)]) * x[static_cast<size_t> (i)];
            ++n;
        }
    return n > 0 ? acc / static_cast<double> (n) : 0.0;
}

/** Mean square of both channels over the windows. */
double meanPower (const Channels& c, const std::vector<Window>& windows) { return 0.5 * (meanPower (c[0], windows) + meanPower (c[1], windows)); }

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

/** Residual power after removing DC and the components at `harmonics` x f0,
    relative to the window's total power (dB). The window must hold an integer
    number of periods of f0, so the projections are exact. */
double residualAfterHarmonics (const float* x, int n, double f0, int harmonics, double& totalPower)
{
    double total = 0.0, mean = 0.0;
    for (int i = 0; i < n; ++i)
    {
        total += static_cast<double> (x[i]) * x[i];
        mean += x[i];
    }
    mean /= n;
    double removed = mean * mean * n;
    for (int k = 1; k <= harmonics; ++k)
    {
        double re = 0.0, im = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double a = kTwoPi * f0 * k * i / kFs;
            re += x[i] * std::cos (a);
            im += x[i] * std::sin (a);
        }
        removed += 2.0 * (re * re + im * im) / n; // (A^2 / 2) * n with A = 2 |X| / n
    }
    totalPower = total / n;
    return std::max (0.0, total - removed) / n;
}

/** THD+N (dB re total) of a sine at f0 over [begin, begin + n): the CLI's
    `quality` definition (Analysis.h sineThdnDb), as are the tone gain track
    (toneGainTrack, 20 ms Hann windows every 5 ms) and percentile(). */
double thdPlusNoiseDb (const std::vector<float>& x, int begin, int n, double f0) { return sineThdnDb (x.data() + begin, n, kFs, f0); }

// ---- stimuli ----------------------------------------------------------------
/** 55 Hz kicks (exp decay, tau 100 ms, 350 ms long, peak `kickPeak`) every
    500 ms from 250 ms on, under a steady tone. */
std::vector<float> tonesUnderKicks (double seconds, double toneHz, float toneAmp, float kickPeak)
{
    const int n = samplesOf (seconds);
    std::vector<float> x (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kFs;
        const double beat = std::fmod (t + 0.25, 0.5);
        const double kick = t >= 0.25 && beat < 0.35 ? kickPeak * std::exp (-beat / 0.1) * std::sin (kTwoPi * 55.0 * beat) : 0.0;
        x[static_cast<size_t> (i)] = static_cast<float> (toneAmp * std::sin (kTwoPi * toneHz * t) + kick);
    }
    return x;
}

/** Scales a stereo scene so its integrated loudness is `lufs`. */
void normaliseLoudness (Channels& c, double lufs)
{
    const auto report = analyse (c, kFs);
    REQUIRE (report.integratedLufs > -100.0f);
    const auto g = static_cast<float> (std::pow (10.0, (lufs - report.integratedLufs) / 20.0));
    for (auto& ch : c)
        for (auto& s : ch)
            s *= g;
}

// ---- E19: burst-footstep scenes --------------------------------------------
/** Footstep-like bursts: seeded white noise band-passed at 3.2 kHz (Q 1, the
    Gaming footsteps band), Hann-shaped. From 1 s: six 20 ms, six 40 ms and six
    80 ms bursts, one every 400 ms, then one 1 s "steady" burst and 0.6 s of
    tail. `stepRms` is each burst's RMS over its length; `bedRms` a pink bed
    under all of it (0 = digital silence). */
struct BurstScene
{
    io::AudioFileData input;
    std::vector<Window> bursts[3]; // 20 / 40 / 80 ms
    Window steady;                 // last 500 ms of the 1 s burst
    std::vector<Window> bed;       // 200..400 ms after each short burst's onset
};

BurstScene buildBurstScene (float bedRms, double stepRms)
{
    const int n = samplesOf (1.0 + 18 * 0.4 + 1.0 + 0.6);
    auto x = pinkNoise (n, bedRms, 777);
    const auto band = bandPass (whiteNoise (n, 1.0f, 4242), 3200.0, 1.0);
    const double bandRms = rms (band.data(), n);
    const double hannRms = std::sqrt (3.0 / 8.0);

    BurstScene s;
    auto addBurst = [&] (int start, int len, bool shaped) {
        const double g = stepRms / bandRms / (shaped ? hannRms : 1.0);
        for (int i = 0; i < len; ++i)
        {
            const double w = shaped ? 0.5 - 0.5 * std::cos (kTwoPi * i / len) : 1.0;
            x[static_cast<size_t> (start + i)] += static_cast<float> (g * w * band[static_cast<size_t> (start + i)]);
        }
    };
    const int durations[3] = { samplesOf (0.020), samplesOf (0.040), samplesOf (0.080) };
    for (int k = 0; k < 18; ++k)
    {
        const int onset = samplesOf (1.0 + 0.4 * k), len = durations[k / 6];
        addBurst (onset, len, true);
        s.bursts[k / 6].push_back ({ onset, onset + len });
        s.bed.push_back ({ onset + samplesOf (0.2), onset + samplesOf (0.4) });
    }
    const int steadyOnset = samplesOf (1.0 + 18 * 0.4);
    addBurst (steadyOnset, samplesOf (1.0), false);
    s.steady = { steadyOnset + samplesOf (0.5), steadyOnset + samplesOf (1.0) };
    s.input = stereoOf (x);
    return s;
}

/** Steps 6 dB under the bed's full-band RMS (about 4 dB over it in the
    3.2 kHz band), the whole scene normalised to `lufs` integrated. */
BurstScene makeBurstScene (double lufs)
{
    auto s = buildBurstScene (0.05f, 0.025);
    normaliseLoudness (s.input.channels, lufs);
    return s;
}

struct BurstResult
{
    double burstLiftDb[3]; // step-only power in the 3.2 kHz band, out vs in
    double steadyLiftDb;
    double bedLiftDb;      // full-band bed, out vs in
    double contrastChangeDb[3]; // (step / bed in the band) out minus in
};

BurstResult measureBursts (const BurstScene& s, const std::vector<float>& values)
{
    const auto out = render (s.input, values);
    const auto inBand = bandPass (midOf (s.input.channels), 3200.0, 1.0);
    const auto outBand = bandPass (midOf (out), 3200.0, 1.0);
    const double inBed = meanPower (inBand, s.bed), outBed = meanPower (outBand, s.bed);

    BurstResult r {};
    for (int d = 0; d < 3; ++d)
    {
        const double inStep = meanPower (inBand, s.bursts[d]) - inBed;
        const double outStep = meanPower (outBand, s.bursts[d]) - outBed;
        r.burstLiftDb[d] = powerDb (outStep) - powerDb (inStep);
        r.contrastChangeDb[d] = (powerDb (outStep) - powerDb (outBed)) - (powerDb (inStep) - powerDb (inBed));
    }
    r.steadyLiftDb = powerDb (meanPower (outBand, { s.steady }) - outBed) - powerDb (meanPower (inBand, { s.steady }) - inBed);
    r.bedLiftDb = powerDb (meanPower (out, s.bed)) - powerDb (meanPower (s.input.channels, s.bed));
    return r;
}
} // namespace

// =============================================================================
TEST_CASE ("KnownGap: pumping - a 2 kHz tone under 55 Hz kicks moves with every kick at Boost 100 (E05 / E02)")
{
    // Stimulus: 2 kHz at -20 dBFS peak plus 55 Hz kicks (peak -6 dBFS, tau
    // 100 ms) every 500 ms, 6 s, Music mode. Metric: the tone's gain in 20 ms
    // Hann windows every 5 ms over 1..6 s (2 kHz is no harmonic of 55 Hz, so
    // limiter distortion of the kick does not leak into it); spread p95 - p5,
    // dip = median - min, lift = max - median, fraction of windows > 1 dB
    // below the median.
    const auto x = tonesUnderKicks (6.0, 2000.0, 0.1f, 0.5f);
    const auto input = stereoOf (x);
    struct Row
    {
        float boost;
        double spread, dip, lift, down;
    } rows[2] = { { 0.0f, 0, 0, 0, 0 }, { 100.0f, 0, 0, 0, 0 } };
    for (auto& r : rows)
    {
        const auto out = render (input, resolve (boosted (ModeValue::Music, r.boost)));
        const auto g = toneGainTrack (out[0], x, kFs, 2000.0, samplesOf (1.0), samplesOf (6.0));
        const double med = percentile (g, 0.5);
        r.spread = percentile (g, 0.95) - percentile (g, 0.05);
        r.dip = med - *std::min_element (g.begin(), g.end());
        r.lift = *std::max_element (g.begin(), g.end()) - med;
        r.down = static_cast<double> (std::count_if (g.begin(), g.end(), [med] (double v) { return v < med - 1.0; })) / static_cast<double> (g.size());
        const std::string tag = "pumping boost " + std::to_string (static_cast<int> (r.boost));
        measured (tag + " p95-p5", r.spread, "dB");
        measured (tag + " max dip", r.dip, "dB");
        measured (tag + " max lift", r.lift, "dB");
        measured (tag + " time > 1 dB down", 100.0 * r.down, "%");
    }
    // Boost 0 is not a gap: the tone must not move at all.
    CHECK_LE (rows[0].spread, 0.2);
    CHECK_LE (rows[0].dip, 0.3);

    // Re-baselined by the docs/11 E06 slice (the governor ticks on its 10 ms
    // grid and the chain's segments end there): spread 4.92 -> 3.60 dB, dip
    // 2.97 -> 2.41, lift 3.33 -> 3.56, time > 1 dB down 32.2 -> 2.7 %. Not a
    // pumping fix: Boost 100 governs here, and the governor's trajectory -
    // hence this metric - depended on the host block before (CLI, same
    // scene: 4.37 / 3.03 / 3.31 dB / 32.2 % at 64-sample blocks, 3.76 / 2.25 /
    // 3.58 dB / 3.2 % at 480) and still differs between 64-256 and >= 480
    // samples (the 25 ms THD+N windows close on the host's block grid). The
    // time-down share flips between about 32 % and 3 % with small changes of
    // the trajectory.
    // KNOWN_GAP: target max dip <= 6 dB (<= 3 dB only with multiband) per docs/11 E05 Done-when.
    CHECK_NEAR (rows[1].dip, 2.41, 0.3);
    // KNOWN_GAP: E59 reports p95 - p5 and lift / dip separately; docs/11 E05 / E02 set no target for them yet.
    CHECK_NEAR (rows[1].spread, 3.60, 0.3);
    CHECK_NEAR (rows[1].lift, 3.56, 0.3);
    CHECK_NEAR (100.0 * rows[1].down, 2.7, 2.0);
}

TEST_CASE ("KnownGap: 60 Hz and 1 kHz THD+N of a -6 dBFS sine at 12 dB maximizer drive (E05)")
{
    // Maximizer alone (every other module off) at its defaults (Balanced,
    // clipper share 50 %, ceiling -1 dBTP) with max.drive 12 dB; 2 s sines,
    // THD+N over 1..2 s (an integer number of periods): the residual after
    // removing DC and the fundamental, dB re the total. docs/11 E05 measured
    // -16.0 dB on the round-7 build.
    auto values = resolve (RenderOptions {});
    onlyModules (values, { MaximizerOn });
    setValue (values, MaxDriveDb, 12.0f);
    double thd[2] = {};
    const double freqs[2] = { 60.0, 1000.0 };
    for (int k = 0; k < 2; ++k)
    {
        const auto out = render (stereoOf (sine (freqs[k], kFs, samplesOf (2.0), 0.5f)), values);
        thd[k] = thdPlusNoiseDb (out[0], samplesOf (1.0), samplesOf (1.0), freqs[k]);
        measured (std::string ("THD+N at ") + (k == 0 ? "60 Hz" : "1 kHz"), thd[k], "dB");
    }
    // KNOWN_GAP: target <= -30 dB at 60 Hz per docs/11 E05 Done-when.
    CHECK_NEAR (thd[0], -16.11, 0.5);
    // KNOWN_GAP: target <= -30 dB at 1 kHz per docs/11 E05 Done-when.
    CHECK_NEAR (thd[1], -16.21, 0.5);
}

TEST_CASE ("KnownGap: 7.1 LFE - an LFE-only 50 Hz tone folds at +6 dB re one main channel with the virtualiser off and on (E01)")
{
    // 8-channel input (FL FR FC LFE BL BR SL SR), 50 Hz at -12 dBFS peak on
    // one channel, maximizer and bass engine off (docs/11 E01's setup), the
    // surround fold forced (virt.input: FL-only input would otherwise switch
    // to the stereo passthrough after 2 s, docs/11 E27).
    // Level: the 50 Hz amplitude of the left output over 1..2 s, dBFS.
    auto level = [] (int channel, bool virt) {
        auto values = resolve (RenderOptions {});
        setValue (values, MaximizerOn, 0.0f);
        setValue (values, BassOn, 0.0f);
        setValue (values, VirtualizerOn, virt ? 1.0f : 0.0f);
        setValue (values, VirtInputMode, static_cast<float> (InputModeValue::ForceSurround));
        Channels c (8, std::vector<float> (static_cast<size_t> (samplesOf (2.0)), 0.0f));
        c[static_cast<size_t> (channel)] = sine (50.0, kFs, samplesOf (2.0), 0.25f);
        const auto out = render (fileOf (std::move (c)), values);
        return toDb (toneAmplitude (out[0].data() + samplesOf (1.0), samplesOf (1.0), 50.0, kFs));
    };
    const double lfeOff = level (3, false), lfeOn = level (3, true), mainOff = level (0, false), mainOn = level (0, true);
    measured ("LFE only, virt off", lfeOff, "dBFS");
    measured ("LFE only, virt on", lfeOn, "dBFS");
    measured ("FL only, virt off", mainOff, "dBFS");
    measured ("FL only, virt on", mainOn, "dBFS");
    measured ("LFE re FL, virt off", lfeOff - mainOff, "dB");
    measured ("LFE re FL, virt on", lfeOn - mainOn, "dB");

    // The FL reference (not a gap): the BS.775 fold and the virtualiser both pass one main at about -3 dB.
    CHECK_NEAR (mainOff, -15.05, 0.3);
    CHECK_NEAR (mainOn, -15.01, 0.3);
    // Fixed by E01 (shared Bs775Fold / LfeFold, virt.lfe default +6 dB):
    // before, the LFE was dropped with the virtualiser off (-240 dBFS, exact
    // silence) and sat at -0.03 dB re FL with it on. Now both folds put it at
    // virt.lfe re one main and within 1 dB of each other (E01 Done-when).
    CHECK_NEAR (lfeOff - mainOff, 6.00, 0.3);
    CHECK_NEAR (lfeOn - mainOn, 5.97, 0.3);
    CHECK_LE (std::abs (lfeOn - lfeOff), 1.0);
    // KNOWN_GAP: target LFE = FL + 10 dB (+-1 dB) per docs/11 E01 Done-when;
    // the default stays +6 dB until E05's LF-safe limiter envelope lands.
}

TEST_CASE ("KnownGap closed: stereo in an 8-channel container - FL/FR-only content switches to the stereo passthrough fold: no centre notch, full separation (E27)")
{
    // A stereo game in the 8-channel Game container (FL/FR only), default
    // settings with every module but the virtualiser off: the fold alone.
    // Before (the surround fold, forced): FL = FR through the two virtual
    // speakers at +-30 deg, and FL-only through its virtual speaker. After
    // (Auto): 2 s of FL/FR-only content switch to the passthrough fold
    // (400 ms ramp); measured over 3..4 s.
    // Centre: 152 tones, 100 Hz .. 8 kHz, 1/24 octave apart, plus 1400 ..
    // 1440 Hz in 2 Hz steps around the fold's deepest notch (integer Hz, so
    // 1 s windows are orthogonal), 0.005 each at spread phases on FL and FR;
    // gain = left output / FL input per tone, dB.
    const int n = samplesOf (4.0), from = samplesOf (3.0), len = samplesOf (1.0);
    std::vector<double> freqs;
    for (int k = 0; k < 152; ++k)
        freqs.push_back (std::round (100.0 * std::pow (2.0, k / 24.0)));
    for (double f = 1400.0; f <= 1440.0; f += 2.0)
        if (std::find (freqs.begin(), freqs.end(), f) == freqs.end())
            freqs.push_back (f);
    std::vector<float> probe (static_cast<size_t> (n), 0.0f);
    for (size_t k = 0; k < freqs.size(); ++k)
    {
        const auto s = sine (freqs[k], kFs, n, 0.005f, 2.39996 * static_cast<double> (k));
        for (int i = 0; i < n; ++i)
            probe[static_cast<size_t> (i)] += s[static_cast<size_t> (i)];
    }
    const auto noise = whiteNoise (n, 0.1f, 2701);
    auto container = [n] (const std::vector<float>& fl, const std::vector<float>& fr) {
        Channels c (8, std::vector<float> (static_cast<size_t> (n), 0.0f));
        c[0] = fl;
        c[1] = fr;
        return fileOf (std::move (c));
    };
    auto valuesFor = [] (InputModeValue mode) {
        auto v = resolve (RenderOptions {});
        onlyModules (v, { VirtualizerOn });
        setValue (v, VirtInputMode, static_cast<float> (mode));
        return v;
    };
    struct Result
    {
        double minGain, maxGain, notchAt, separation;
    };
    auto measure = [&] (InputModeValue mode) {
        const auto values = valuesFor (mode);
        const auto centre = render (container (probe, probe), values);
        Result r { 1e9, -1e9, 0.0, 0.0 };
        for (double f : freqs)
        {
            const double g = toDb (toneAmplitude (centre[0].data() + from, len, f, kFs) / toneAmplitude (probe.data() + from, len, f, kFs));
            if (g < r.minGain)
            {
                r.minGain = g;
                r.notchAt = f;
            }
            r.maxGain = std::max (r.maxGain, g);
        }
        const auto left = render (container (noise, std::vector<float> (static_cast<size_t> (n), 0.0f)), values);
        const double l = rms (left[0].data() + from, len), rr = rms (left[1].data() + from, len);
        r.separation = rr > 0.0 ? toDb (l / rr) : 200.0; // exact silence reads 200 dB
        return r;
    };
    const Result before = measure (InputModeValue::ForceSurround), after = measure (InputModeValue::Auto);
    measured ("FL = FR through the surround fold: deepest gain", before.minGain, "dB");
    measured ("FL = FR through the surround fold: deepest gain at", before.notchAt, "Hz");
    measured ("FL = FR through the surround fold: highest gain", before.maxGain, "dB");
    measured ("FL only through the surround fold: L re R", before.separation, "dB");
    measured ("FL = FR, Auto after 3 s: deepest gain", after.minGain, "dB");
    measured ("FL = FR, Auto after 3 s: highest gain", after.maxGain, "dB");
    measured ("FL only, Auto after 3 s: L re R", after.separation, "dB");

    // Before: the fold this content got before E27 (still what Force Surround
    // gives): -16.78 dB at 1412 Hz and +3.34 dB at the top, re the input,
    // and 10.74 dB separation (docs/11 E27, on its own stimulus: a -15.4 dB
    // notch at 1418 Hz, +2.6 dB at 250 Hz and 10.8 dB separation).
    CHECK_NEAR (before.minGain, -16.78, 0.5);
    CHECK_NEAR (before.maxGain, 3.34, 0.3);
    CHECK_NEAR (before.separation, 10.74, 0.3);
    // Closed by E27 Phase 1 (target per docs/11 E27 Done-when: no notch deeper
    // than 0.5 dB, full separation; the passthrough equals a 2-channel render).
    CHECK_LE (std::abs (after.minGain), 0.01);
    CHECK_LE (std::abs (after.maxGain), 0.01);
    CHECK_GE (after.separation, 120.0);
}

TEST_CASE ("KnownGap closed: burst footsteps - Footsteps 100 gives isolated 20 / 40 / 80 ms steps out of digital silence >= 90 % of its steady lift (E19)")
{
    // buildBurstScene() with no bed, each burst at -60 dBFS RMS (inside the
    // -45..-60 dBFS range of docs/11 E19's Done-when). Lift = output vs input
    // power in the 3.2 kHz band over the bursts of one length; steady = the
    // last 500 ms of the 1 s burst. Gaming mode, Footsteps 100, everything
    // else default. docs/11 E19 measured +0.3 / +0.6 / +1.1 dB against
    // +3.4 dB steady on its own stimulus.
    //
    // Before the E19 redesign (the interim static bell) the bursts got
    // 0.52 / 0.93 / 1.65 dB against 5.71 dB steady (9 / 16 / 29 %): between
    // the steps the band sat in digital silence, under its hiss floor, and a
    // burst raised the gain at the band's 120 ms release. The cue enhancer
    // (CueLift, DynamicEq.h) holds the background at the hiss floor there,
    // so a step stands 15 dB out of it and gets the lift within 1-2 ms:
    // 5.34 / 5.63 / 5.67 dB against 5.68 dB (94 / 99 / 100 %).
    RenderOptions footsteps;
    footsteps.mode = ModeValue::Gaming;
    footsteps.macros.push_back ({ "footsteps", 100.0f });
    const auto r = measureBursts (buildBurstScene (0.0f, std::pow (10.0, -60.0 / 20.0)), resolve (footsteps));
    measured ("isolated -60 dBFS bursts, Footsteps 100 lift: 20 ms", r.burstLiftDb[0], "dB");
    measured ("isolated -60 dBFS bursts, Footsteps 100 lift: 40 ms", r.burstLiftDb[1], "dB");
    measured ("isolated -60 dBFS bursts, Footsteps 100 lift: 80 ms", r.burstLiftDb[2], "dB");
    measured ("isolated -60 dBFS bursts, Footsteps 100 lift: steady", r.steadyLiftDb, "dB");

    // Target (docs/11 E19 Done-when): 20-50 ms bursts >= 80 % of the steady lift (the interim's 20 ms >= 90 % too).
    CHECK_GE (r.burstLiftDb[0], 0.9 * r.steadyLiftDb);
    CHECK_GE (r.burstLiftDb[1], 0.9 * r.steadyLiftDb);
    CHECK_NEAR (r.burstLiftDb[0], 5.34, 0.3);
    CHECK_NEAR (r.burstLiftDb[1], 5.63, 0.3);
    CHECK_NEAR (r.burstLiftDb[2], 5.67, 0.3);
    CHECK_NEAR (r.steadyLiftDb, 5.68, 0.3);
}

TEST_CASE ("KnownGap closed: step/bed contrast - the Footsteps 100 cue enhancer lifts 20-80 ms steps under a bed by the same law at -14..-50 LUFS and raises their contrast; Competitive FPS keeps the bed within +1 dB (E19)")
{
    // makeBurstScene() (steps 6 dB under a pink bed) at -14 / -24 / -40 LUFS,
    // plus -50 LUFS (docs/11 E19's quiet-material case). Metrics (mid channel):
    //   burst lift  = step-only power in the 3.2 kHz band (burst windows minus
    //                 the bed), out vs in, per burst length;
    //   steady lift = the same over the last 500 ms of a 1 s burst;
    //   bed lift    = full-band power 200..400 ms after each burst onset;
    //   contrast change = (step / bed in the band) out minus in.
    // The reference is the same scene through a static +7 dB bell at 3.2 kHz,
    // Q 0.9 (band 4's shape as a user EQ band, every other module off): a
    // static EQ moves the contrast only by +0.77 dB, because the steps sit
    // closer to its centre than the pink bed does.
    RenderOptions footsteps;
    footsteps.mode = ModeValue::Gaming;
    footsteps.macros.push_back ({ "footsteps", 100.0f });
    const auto footstepValues = resolve (footsteps);
    const auto fpsValues = resolve (factoryPreset ("gaming-competitive-fps.json"));
    auto bellValues = resolve (RenderOptions {});
    onlyModules (bellValues, { EqOn });
    setValue (bellValues, eq (0, EqFieldOn), 1.0f);
    setValue (bellValues, eq (0, EqFieldType), 0.0f); // EqBandType::Bell
    setValue (bellValues, eq (0, EqFieldFreq), 3200.0f);
    setValue (bellValues, eq (0, EqFieldGain), 7.0f);
    setValue (bellValues, eq (0, EqFieldQ), 0.9f);

    constexpr int kLevels = 4;
    const double levels[kLevels] = { -14.0, -24.0, -40.0, -50.0 };
    BurstResult fs[kLevels], fps[kLevels], bell[kLevels];
    for (int l = 0; l < kLevels; ++l)
    {
        const auto scene = makeBurstScene (levels[l]);
        fs[l] = measureBursts (scene, footstepValues);
        fps[l] = measureBursts (scene, fpsValues);
        bell[l] = measureBursts (scene, bellValues);
        const std::string at = " at " + std::to_string (static_cast<int> (levels[l])) + " LUFS: ";
        measured ("Footsteps 100 step lift" + at + "20 ms", fs[l].burstLiftDb[0], "dB");
        measured ("Footsteps 100 step lift" + at + "40 ms", fs[l].burstLiftDb[1], "dB");
        measured ("Footsteps 100 step lift" + at + "80 ms", fs[l].burstLiftDb[2], "dB");
        measured ("Footsteps 100 step lift" + at + "steady", fs[l].steadyLiftDb, "dB");
        measured ("Footsteps 100 bed lift" + at.substr (0, at.size() - 2), fs[l].bedLiftDb, "dB");
        measured ("Footsteps 100 contrast change" + at + "40 ms", fs[l].contrastChangeDb[1], "dB");
        measured ("static +7 dB bell contrast change" + at + "40 ms", bell[l].contrastChangeDb[1], "dB");
        measured ("Competitive FPS bed lift" + at.substr (0, at.size() - 2), fps[l].bedLiftDb, "dB");
        measured ("Competitive FPS contrast change" + at + "20 ms", fps[l].contrastChangeDb[0], "dB");
        measured ("Competitive FPS contrast change" + at + "40 ms", fps[l].contrastChangeDb[1], "dB");
        measured ("Competitive FPS contrast change" + at + "80 ms", fps[l].contrastChangeDb[2], "dB");
    }

    // Footsteps 100. Before the E19 redesign (the interim static bell): lift
    // 20 / 40 / 80 ms / steady 3.81 / 3.18 / 2.57 / 2.30 dB at -14 LUFS (the
    // steps' peaks reached its loud roll-off) and 5.76 / 5.77 / 5.82 /
    // 5.70 dB below; contrast change at 40 ms -1.44 dB at -14 LUFS, +0.76 dB
    // (the static bell's own) below. After it (CueLift): 5.24 / 6.03 /
    // 6.53 dB at every level; a 1 s steady burst becomes background after
    // about 0.5 s (1.00 dB over its last 500 ms); contrast change +5.99 dB;
    // the bed -1.13 dB (the default 20 Hz subsonic filter on the generator's
    // infrasonic pink; the cue bands add nothing).
    for (int l = 0; l < kLevels; ++l)
    {
        // docs/11 E19 Done-when: lift within +-1 dB across the levels, 20-50 ms
        // steps >= 80 % of the law (the 80 ms lift), contrast >= +3 dB, and
        // well above a static bell's.
        for (int d = 0; d < 3; ++d)
            CHECK_NEAR (fs[l].burstLiftDb[d], fs[1].burstLiftDb[d], 1.0);
        CHECK_GE (fs[l].burstLiftDb[0], 0.8 * fs[l].burstLiftDb[2]);
        CHECK_GE (fs[l].contrastChangeDb[1], 3.0);
        CHECK_GE (fs[l].contrastChangeDb[1], bell[l].contrastChangeDb[1] + 3.0);
        CHECK_NEAR (fs[l].burstLiftDb[0], 5.24, 0.3);
        CHECK_NEAR (fs[l].burstLiftDb[1], 6.03, 0.3);
        CHECK_NEAR (fs[l].burstLiftDb[2], 6.53, 0.3);
        CHECK_NEAR (fs[l].steadyLiftDb, 1.00, 0.3);
        CHECK_NEAR (fs[l].contrastChangeDb[1], 5.99, 0.3);
        CHECK_NEAR (fs[l].bedLiftDb, -1.13, 0.3);
        CHECK_NEAR (bell[l].contrastChangeDb[1], 0.77, 0.1);
    }

    // Competitive FPS. Before the redesign: bed -0.06 / 0.66 / 1.66 / 4.37 dB;
    // contrast change 20 / 40 / 80 ms 0.05 / -0.53 / -1.11, 0.83 / 0.70 /
    // 0.52, 0.82 / 0.75 / 0.70 and -0.89 / -1.66 / -1.67 dB (docs/11 E19
    // measured +10.9 dB and -6.0 dB on its own stimulus).
    const double fpsBed[kLevels] = { -1.86, -1.30, -0.94, 0.69 };
    const double fpsContrast[kLevels][3] = { { 4.39, 4.93, 5.17 }, { 4.49, 5.04, 5.26 }, { 4.58, 5.21, 5.67 }, { 4.55, 4.72, 5.21 } }; // 20 / 40 / 80 ms
    for (int l = 0; l < kLevels; ++l)
    {
        // docs/11 E19 Done-when: Competitive FPS bed <= +1 dB and step/bed contrast change >= +3 dB.
        CHECK_LE (fps[l].bedLiftDb, 1.0);
        CHECK_NEAR (fps[l].bedLiftDb, fpsBed[l], 0.3);
        for (int d = 0; d < 3; ++d)
        {
            CHECK_GE (fps[l].contrastChangeDb[d], 3.0);
            CHECK_NEAR (fps[l].contrastChangeDb[d], fpsContrast[l][d], 0.3);
        }
    }
}

TEST_CASE ("E19: the cue enhancer's loud cap keeps gunfire nearly unlifted in the gaming presets")
{
    // docs/11 E19 shipped its interim bell "only with a preset check that the
    // loud cap still protects gunfire"; the redesign keeps the check. Scene:
    // -40 dBFS white-noise bed, then ten shots (seeded white noise, tau 15 ms,
    // peak -3 dBFS) 100 ms apart from 1 s. Metric: 3.2 kHz band power (mid,
    // Q 1) over the first 30 ms of each shot, the preset as shipped vs the
    // same preset with Footsteps 0. Interim bell (7 dB x Footsteps, rolled
    // off on the shots): +1.13 / +0.94 / +0.58 dB (Competitive FPS, Battle
    // Royale, Night Mode); the cue enhancer (peak 26..36 dB over the
    // background withdraws the lift within 0.5 ms): +0.22 / +0.25 / +0.05 dB.
    // Output peaks stay under full scale.
    const int n = samplesOf (3.0);
    auto x = whiteNoise (n, std::pow (10.0f, -40.0f / 20.0f) * std::sqrt (3.0f), 97);
    FastRandom rng (1234);
    std::vector<Window> shots;
    for (int shot = 0; shot < 10; ++shot)
    {
        const int onset = samplesOf (1.0 + 0.1 * shot);
        for (int i = 0; i < samplesOf (0.1); ++i)
            x[static_cast<size_t> (onset + i)] += static_cast<float> (std::pow (10.0, -3.0 / 20.0) * std::exp (-i / (0.015 * kFs)) * rng.nextBipolar());
        shots.push_back ({ onset, onset + samplesOf (0.03) });
    }
    const auto input = stereoOf (x);
    const struct
    {
        const char* file;
        double shotLiftDb;
    } presets[] = { { "gaming-competitive-fps.json", 0.22 }, { "gaming-battle-royale.json", 0.25 }, { "gaming-night-mode.json", 0.05 } };
    for (const auto& p : presets)
    {
        auto opts = factoryPreset (p.file);
        const auto shipped = render (input, resolve (opts));
        opts.macros.push_back ({ "footsteps", 0.0f });
        const auto without = render (input, resolve (opts));
        const double lift = powerDb (meanPower (bandPass (midOf (shipped), 3200.0, 1.0), shots))
                            - powerDb (meanPower (bandPass (midOf (without), 3200.0, 1.0), shots));
        measured (std::string (p.file) + " shot lift in the 3.2 kHz band vs Footsteps 0", lift, "dB");
        CHECK_LE (lift, 0.5);
        CHECK_NEAR (lift, p.shotLiftDb, 0.3);
        for (const auto& ch : shipped)
            CHECK_LE (peakAbs (ch.data(), n), 1.0);
    }
}

TEST_CASE ("KnownGap: Night Mode ambush - no hole after the event, but the bed is still lifted +12 dB (E21)")
{
    // Scene: 12 s of -50 dBFS-RMS pink ambience, then 3 s of automatic fire
    // (10 shots/s; each seeded white noise, tau 15 ms, peak -12 dBFS) over the
    // ambience, then 8 s of ambience alone. Levels are full-band power of both
    // channels, out vs in: bed before (8..12 s), event (12..15 s), bed 1..2 s
    // after the event (16..17 s) and 5..7.5 s after it (20..22.5 s). This is
    // the re-baselined, pinned scene docs/11 E21 asks for (its +16.6 dB lift
    // and 6.6 dB hole predate d096a4d and used another event). The same
    // scene with 10 s of fire (22..30 s after it) checks a long event.
    auto scene = [] (double fireSeconds) {
        const int n = samplesOf (12.0 + fireSeconds + 8.0);
        auto x = pinkNoise (n, std::pow (10.0f, -50.0f / 20.0f), 1357);
        FastRandom rng (2468);
        for (int shot = 0; shot < static_cast<int> (std::lround (fireSeconds * 10.0)); ++shot)
        {
            const int onset = samplesOf (12.0 + 0.1 * shot);
            for (int i = 0; i < samplesOf (0.1); ++i)
                x[static_cast<size_t> (onset + i)] += static_cast<float> (0.25 * std::exp (-i / (0.015 * kFs)) * rng.nextBipolar());
        }
        return stereoOf (x);
    };
    const auto shipped = resolve (factoryPreset ("gaming-night-mode.json"));
    auto noAutoLevel = shipped;
    setValue (noAutoLevel, AutoLevelOn, 0.0f);
    auto lift = [] (const io::AudioFileData& input, const Channels& out, double from, double to) {
        const std::vector<Window> w { { samplesOf (from), samplesOf (to) } };
        return powerDb (meanPower (out, w)) - powerDb (meanPower (input.channels, w));
    };

    const auto input = scene (3.0);
    const auto out = render (input, shipped);
    const double before = lift (input, out, 8.0, 12.0), event = lift (input, out, 12.0, 15.0);
    const double after1 = lift (input, out, 16.0, 17.0), after5 = lift (input, out, 20.0, 22.5);
    const double staticLift = lift (input, render (input, noAutoLevel), 8.0, 12.0);
    measured ("Night Mode bed lift before the event", before, "dB");
    measured ("Night Mode event change", event, "dB");
    measured ("Night Mode bed lift 1-2 s after", after1, "dB");
    measured ("Night Mode bed lift 5-7.5 s after", after5, "dB");
    measured ("Night Mode hole 1-2 s after", before - after1, "dB");
    measured ("Night Mode hole 5-7.5 s after", before - after5, "dB");
    measured ("Night Mode bed lift with Auto Level off", staticLift, "dB");

    const auto longInput = scene (10.0);
    const auto longOut = render (longInput, shipped);
    const double longHole1 = lift (longInput, longOut, 8.0, 12.0) - lift (longInput, longOut, 23.0, 24.0);
    const double longHole5 = lift (longInput, longOut, 8.0, 12.0) - lift (longInput, longOut, 27.0, 29.5);
    measured ("Night Mode hole 1-2 s after a 10 s event", longHole1, "dB");
    measured ("Night Mode hole 5-7.5 s after a 10 s event", longHole5, "dB");

    // E21 slice (AutoLevel: upper gate, +6 dB cap, 3 dB/s recovery). Before
    // it: bed before 15.77 dB, hole 5.23 dB (1-2 s) / 2.36 dB (5-7.5 s),
    // event 1.76 dB; 10 s event: hole 6.89 / 4.01 dB. The gunfire is held out
    // of Auto Level's measure, so its gain stays at +6 dB through the event
    // and there is no hole; the bed lift fell by 3.8 dB (Auto Level +12 ->
    // +6 dB, partly made up by the upward compressor on a quieter bed).
    CHECK_LE (std::abs (before - after1), 1.0); // docs/11 E21 Done-when: within 1 dB 1 s after the event
    CHECK_LE (std::abs (before - after5), 1.0);
    CHECK_NEAR (before - after1, -0.10, 0.3);
    CHECK_NEAR (event, 1.24, 0.3);
    // Auto Level's own share of the bed lift is at its +6 dB cap.
    CHECK_LE (before - staticLift, AutoLevel::kMaxGainDb + 0.1);
    // KNOWN_GAP: target ambience lift <= +6 dB per docs/11 E21 Done-when. The
    // rest is the preset's own: 6.72 dB with Auto Level off (compressor
    // make-up 6 dB, upward compression) - a preset retune. The E19 redesign
    // took the Footsteps bell's share out of it (11.98 / 6.98 -> 11.13 /
    // 6.72 dB): the cue enhancer does not lift the bed.
    CHECK_NEAR (before, 11.13, 0.3);
    CHECK_NEAR (staticLift, 6.72, 0.3);
    // A 10 s event: the upper gate's 5 s release counts only the blocks in
    // which the 100 ms measure also reads above the gate, so whether this
    // intermittent fire becomes a new level depended on the host block (a
    // 3.25 dB hole 1-2 s after it at 512-sample blocks, none at 64 or 480;
    // the CLI measured 2.93 dB at 256 / 512 / 1024 and -0.48 dB at 64 / 480).
    // Since the chain's segments end on the governor's 10 ms grid (docs/11
    // E06) the count is the same at every block size: no new level, no hole
    // (KNOWN_GAP: no Done-when for long events in docs/11 E21).
    CHECK_NEAR (longHole1, -0.31, 0.3);
    CHECK_LE (std::abs (longHole5), 1.0);
}

TEST_CASE ("KnownGap: kick onset - Punch 100 lifts the kick's first 10 ms only slightly more than its body, Tighten and Boost 100 cut the onset (E04 / E05)")
{
    // Synthetic kick (50 Hz + 80 Hz chirp, e^-18t, peak -6 dBFS) every
    // 500 ms for 6 s, Music mode, maximizer off unless stated. Lift = output
    // vs input power summed over the kicks from 1 s on, in 0-10 / 10-30 /
    // 40-60 ms after each onset.
    const int n = samplesOf (6.0);
    std::vector<float> x (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kFs, beat = std::fmod (t, 0.5);
        x[static_cast<size_t> (i)] = static_cast<float> (0.5 * std::exp (-beat * 18.0) * std::sin (kTwoPi * (50.0 + 80.0 * std::exp (-beat * 30.0)) * beat));
    }
    const auto input = stereoOf (x);
    auto lifts = [&] (const std::vector<float>& values, double& onset, double& body, double& late) {
        const auto out = render (input, values);
        std::vector<Window> w0, w1, w2;
        for (double k = 1.0; k < 5.9; k += 0.5)
        {
            const int s = samplesOf (k);
            w0.push_back ({ s, s + samplesOf (0.010) });
            w1.push_back ({ s + samplesOf (0.010), s + samplesOf (0.030) });
            w2.push_back ({ s + samplesOf (0.040), s + samplesOf (0.060) });
        }
        onset = powerDb (meanPower (out, w0)) - powerDb (meanPower (input.channels, w0));
        body = powerDb (meanPower (out, w1)) - powerDb (meanPower (input.channels, w1));
        late = powerDb (meanPower (out, w2)) - powerDb (meanPower (input.channels, w2));
    };

    RenderOptions punch;
    punch.mode = ModeValue::Music;
    punch.macros.push_back ({ "punch", 100.0f });
    auto punchValues = resolve (punch);
    setValue (punchValues, MaximizerOn, 0.0f);
    auto tightenValues = resolve (RenderOptions {});
    setValue (tightenValues, MaximizerOn, 0.0f);
    setValue (tightenValues, BassTighten, 0.5f);
    const auto boostValues = resolve (boosted (ModeValue::Music, 100.0f)); // maximizer on (Boost engages it)

    double p0 = 0, p1 = 0, p2 = 0, t0 = 0, t1 = 0, t2 = 0, b0 = 0, b1 = 0, b2 = 0;
    lifts (punchValues, p0, p1, p2);
    lifts (tightenValues, t0, t1, t2);
    lifts (boostValues, b0, b1, b2);
    measured ("Punch 100 lift 0-10 ms", p0, "dB");
    measured ("Punch 100 lift 10-30 ms", p1, "dB");
    measured ("Punch 100 lift 40-60 ms", p2, "dB");
    measured ("Tighten 0.5 lift 0-10 ms", t0, "dB");
    measured ("Tighten 0.5 lift 10-30 ms", t1, "dB");
    measured ("Boost 100 onset (0-10) minus body (10-30)", b0 - b1, "dB");

    // KNOWN_GAP: target Punch 100 0-10 ms lift >= 10-30 ms lift + 2 dB per docs/11 E04 Done-when.
    // docs/11 E04 step (1) took BassTighten out of the Punch macro: 0-10 /
    // 10-30 ms 3.92 / 5.47 -> 5.38 / 4.70 dB, onset minus body -1.55 ->
    // +0.68 dB. The rest is the full-band shaper's own smear (E04 steps 2-5).
    CHECK_NEAR (p0, 5.38, 0.3);
    CHECK_NEAR (p1, 4.70, 0.3);
    CHECK_NEAR (p0 - p1, 0.68, 0.3);
    CHECK_GE (p0, p1); // the onset no longer gets less than the body
    // KNOWN_GAP: target Tighten 0.5 0-10 ms change >= -0.5 dB per docs/11 E04 Done-when.
    CHECK_NEAR (t0, -2.04, 0.3);
    // KNOWN_GAP: target Boost 100 kick onset / body >= 0 dB per docs/11 E05 Done-when.
    CHECK_NEAR (b0 - b1, -1.73, 0.3);
}

TEST_CASE ("KnownGap closed: 30 Hz audible-band energy - the laptop preset keeps the harmonics of a 30 Hz tone (E03)")
{
    // A 30 Hz sine at -12 dBFS peak for 3 s through Laptop Speakers, as
    // shipped and with bass.subsonic 0. Audible band = everything >= 120 Hz:
    // the output is periodic, so over 2..3 s (30 periods) it is the power left
    // after DC and the 30 / 60 / 90 Hz components, in dB re the input's 30 Hz
    // power.
    const auto input = stereoOf (sine (30.0, kFs, samplesOf (3.0), 0.25f));
    const double inPower = 0.25 * 0.25 / 2.0;
    auto audible = [&] (std::vector<float> values) {
        const auto out = render (input, values);
        double total = 0.0;
        const double residual = residualAfterHarmonics (out[0].data() + samplesOf (2.0), samplesOf (1.0), 30.0, 3, total);
        return powerDb (residual) - powerDb (inPower);
    };
    auto shipped = resolve (factoryPreset ("device-laptop-speakers.json"));
    auto noSubsonic = shipped;
    setValue (noSubsonic, BassSubsonic, 0.0f);
    const double asShipped = audible (shipped), withoutSubsonic = audible (noSubsonic);
    measured ("Laptop Speakers 30 Hz audible band, as shipped", asShipped, "dB");
    measured ("Laptop Speakers 30 Hz audible band, bass.subsonic 0", withoutSubsonic, "dB");

    // Closed by the E03 preset slice: the preset's bass.subsonic went from 40 Hz
    // to the 20 Hz default (stage 1 runs before the harmonic generator), which
    // moved the shipped value from -23.80 to -14.78 dB. Target >= -15.8 dB per
    // docs/11 E03 Done-when (">= -19 dB" there, on its own stimulus, where
    // shipped / subsonic 0 read -27.0 / -17.8 dB: the target is 1.2 dB under
    // subsonic 0, and the subsonic-0 render here was the same 9.2 dB above the
    // old shipped one).
    CHECK_GE (asShipped, -15.8);
    CHECK_NEAR (asShipped, -14.78, 0.3);
    CHECK_NEAR (withoutSubsonic, -14.60, 0.3);
    CHECK_LE (withoutSubsonic - asShipped, 0.5); // the 20 Hz HP4 costs a 30 Hz tone about 0.2 dB
}

TEST_CASE ("KnownGap closed: focus ILD - Positional Focus at 100 % adds at most 3 dB of ILD at 3 kHz (E24)")
{
    // 3 kHz tone, left -20 dBFS, right -26.02 dBFS (6.02 dB ILD). Added ILD =
    // output ILD minus input ILD, amplitudes at 3 kHz over 1..2 s. Spatial
    // alone at spatial.focus 0 and 1, and Competitive FPS as shipped (M2).
    // The docs/11 E24 slice capped the focus bell at +3 dB (was +6 dB): added
    // ILD at 100 % 7.91 -> 2.86 dB, Competitive FPS 5.53 -> 2.53 dB.
    const int n = samplesOf (2.0);
    const auto input = fileOf ({ sine (3000.0, kFs, n, 0.1f), sine (3000.0, kFs, n, 0.05f) });
    const double inIld = toDb (0.1 / 0.05);
    auto addedIld = [&] (const std::vector<float>& values) {
        const auto out = render (input, values);
        const double l = toneAmplitude (out[0].data() + samplesOf (1.0), samplesOf (1.0), 3000.0, kFs);
        const double r = toneAmplitude (out[1].data() + samplesOf (1.0), samplesOf (1.0), 3000.0, kFs);
        return toDb (l / r) - inIld;
    };
    auto spatialOnly = resolve (RenderOptions {});
    onlyModules (spatialOnly, { SpatialOn });
    auto focusMax = spatialOnly;
    setValue (focusMax, SpatialFocus, 1.0f);
    const double off = addedIld (spatialOnly), full = addedIld (focusMax);
    const double fps = addedIld (resolve (factoryPreset ("gaming-competitive-fps.json")));
    measured ("added ILD, focus 0", off, "dB");
    measured ("added ILD, focus 100 %", full, "dB");
    measured ("added ILD, Competitive FPS", fps, "dB");

    CHECK_NEAR (off, 0.0, 0.05); // focus 0 is neutral
    // Target (docs/11 E24 Done-when): capped focus adds <= 3 dB ILD at 3 kHz.
    CHECK_LE (full, 3.0);
    CHECK_LE (fps, 3.0);
    CHECK_NEAR (full, 2.86, 0.2);
    CHECK_NEAR (fps, 2.53, 0.2);
}

TEST_CASE ("KnownGap: hands-free rates - at 8 / 16 / 32 kHz Footsteps 100 no longer lifts 3.2 kHz above the rest of the band; Competitive FPS still does (E17)")
{
    // A -60 dBFS tone at 3.2 kHz and one at 1 kHz, 2 s each, at 8 / 16 / 32
    // (Bluetooth hands-free) and 48 kHz. Lift = output / input amplitude at the
    // tone over 1..2 s; the metric is the 3.2 kHz lift minus the 1 kHz lift
    // (broadband upward compression lifts both alike and is not the E17
    // defect). Before the docs/11 E17 band-4 clamp, Footsteps 100 lifted
    // 3.2 kHz by +10.0 dB at every rate (+7.0 dB over 1 kHz at 8 kHz) and
    // Competitive FPS by +14.3 dB at 8 kHz (+8.0 dB over 1 kHz).
    auto lift = [] (const std::vector<float>& values, double fs, double freq) {
        const int n = static_cast<int> (2.0 * fs), half = n / 2;
        io::AudioFileData in;
        in.sampleRate = fs;
        in.numChannels = 2;
        const auto x = sine (freq, fs, n, std::pow (10.0f, -60.0f / 20.0f));
        in.channels = { x, x };
        const auto out = render (in, values);
        return toDb (toneAmplitude (out[0].data() + half, half, freq, fs) / toneAmplitude (x.data() + half, half, freq, fs));
    };
    RenderOptions footsteps;
    footsteps.mode = ModeValue::Gaming;
    footsteps.macros.push_back ({ "footsteps", 100.0f });
    const auto fsValues = resolve (footsteps);
    const auto fpsValues = resolve (factoryPreset ("gaming-competitive-fps.json"));
    const double rates[4] = { 8000.0, 16000.0, 32000.0, 48000.0 };
    double fsExcess[4] {}, fpsExcess[4] {};
    for (int k = 0; k < 4; ++k)
    {
        fsExcess[k] = lift (fsValues, rates[k], 3200.0) - lift (fsValues, rates[k], 1000.0);
        fpsExcess[k] = lift (fpsValues, rates[k], 3200.0) - lift (fpsValues, rates[k], 1000.0);
        const std::string at = " at " + std::to_string (static_cast<int> (rates[k] / 1000.0)) + " kHz";
        measured ("Footsteps 100 3.2 kHz lift over 1 kHz" + at, fsExcess[k], "dB");
        measured ("Competitive FPS 3.2 kHz lift over 1 kHz" + at, fpsExcess[k], "dB");
    }
    // Target (docs/11 E17 Done-when): 3.2 kHz lift from Footsteps 100 <= +1 dB at 8 / 16 / 32 kHz.
    for (int k = 0; k < 3; ++k)
    {
        CHECK_LE (fsExcess[k], 1.0);
        CHECK_NEAR (fsExcess[k], 0.0, 0.1);
    }
    // 48 kHz: the band is on, but since docs/11 E19 it is the cue enhancer,
    // which leaves a steady tone alone (the interim bell gave it 6.43 dB);
    // test_modes.cpp checks its lift on cues at 44.1 kHz and on hands-free rates.
    CHECK_NEAR (fsExcess[3], 0.0, 0.1);
    // KNOWN_GAP: target 3.2 kHz lift from Competitive FPS <= +1 dB at 8 / 16 / 32 kHz per docs/11 E17 Done-when. The
    // rest is Clarity presence (centred at 3.2 kHz, raised by Boost and Voice & Score) and the voice band at 2 kHz;
    // E17's hands-free runtime override (Footsteps, Spatial, Bass and virtualiser off) is not built yet.
    // The E19 retune lowered the preset's Voice & Score (0.35 -> 0.2): 2.42 / 2.53 / 2.54 -> 1.75 / 1.79 / 1.79 dB.
    const double fpsToday[3] = { 1.75, 1.79, 1.79 };
    for (int k = 0; k < 3; ++k)
        CHECK_NEAR (fpsExcess[k], fpsToday[k], 0.3);
}

TEST_CASE ("KnownGap metrics: each metric reads an injected artefact at its injected value (meta-validation, docs/11 E59)")
{
    // THD+N: a 60 Hz sine plus a 1 % third harmonic reads -40.0 dB.
    {
        auto x = sine (60.0, kFs, samplesOf (1.0), 0.5f);
        const auto h = sine (180.0, kFs, samplesOf (1.0), 0.005f);
        for (size_t i = 0; i < x.size(); ++i)
            x[i] += h[i];
        CHECK_NEAR (thdPlusNoiseDb (x, 0, samplesOf (1.0), 60.0), 20.0 * std::log10 (0.01 / std::sqrt (1.0001)), 0.05);
    }
    // Tone envelope: a 6 dB, 2 Hz square gain modulation reads a 6 dB spread.
    {
        const auto in = sine (2000.0, kFs, samplesOf (3.0), 0.1f);
        auto out = in;
        for (size_t i = 0; i < out.size(); ++i)
            if (std::fmod (static_cast<double> (i) / kFs, 0.5) >= 0.25)
                out[i] *= 0.5f;
        const auto g = toneGainTrack (out, in, kFs, 2000.0, samplesOf (1.0), samplesOf (3.0));
        CHECK_NEAR (percentile (g, 0.95) - percentile (g, 0.05), 6.02, 0.1);
        CHECK_NEAR (*std::max_element (g.begin(), g.end()) - *std::min_element (g.begin(), g.end()), 6.02, 0.1);
    }
    // Audible band: a 30 Hz sine with a 150 Hz partial 20 dB down reads -20 dB.
    {
        auto x = sine (30.0, kFs, samplesOf (1.0), 0.25f);
        const auto h = sine (150.0, kFs, samplesOf (1.0), 0.025f);
        for (size_t i = 0; i < x.size(); ++i)
            x[i] += h[i];
        double total = 0.0;
        CHECK_NEAR (powerDb (residualAfterHarmonics (x.data(), samplesOf (1.0), 30.0, 3, total)) - powerDb (0.25 * 0.25 / 2.0), -20.0, 0.05);
    }
    // The burst metrics through the render path: every module off reads 0 dB
    // everywhere, a -6 dB output gain reads -6 dB lifts and no contrast change.
    {
        const auto scene = makeBurstScene (-24.0);
        auto values = resolve (RenderOptions {});
        onlyModules (values, {});
        const auto flat = measureBursts (scene, values);
        setValue (values, OutputGainDb, -6.0f);
        const auto gained = measureBursts (scene, values);
        for (int d = 0; d < 3; ++d)
        {
            CHECK_NEAR (flat.burstLiftDb[d], 0.0, 0.01);
            CHECK_NEAR (flat.contrastChangeDb[d], 0.0, 0.01);
            CHECK_NEAR (gained.burstLiftDb[d], -6.0, 0.05);
            CHECK_NEAR (gained.contrastChangeDb[d], 0.0, 0.05);
        }
        CHECK_NEAR (flat.bedLiftDb, 0.0, 0.01);
        CHECK_NEAR (gained.bedLiftDb, -6.0, 0.05);
        CHECK_NEAR (gained.steadyLiftDb, -6.0, 0.05);
    }
}

TEST_CASE ("KnownGap metrics: the quality suite reads a 1 % cubic, a 6 dB 2 Hz square gain modulation and a 3 ms delay injected into a pass-through render at their injected values (meta-validation, docs/11 E59)")
{
    // docs/11 E59 Done-when: each metric reports the injected value within
    // +-10 % (of the power ratio for dB metrics: +-0.41 dB). The suite is
    // `flubsound-cli quality` (Commands.h measureQuality); every module is
    // off, so the chain passes the stimuli through and only the injected
    // artefact shows.
    auto values = resolve (RenderOptions {});
    onlyModules (values, {});
    QualityReport clean, cubic, modulated, delayed;
    std::string error;
    REQUIRE (measureQuality (values, 512, clean, error));
    for (const auto& t : clean.thdn)
        CHECK (t.db < -100.0);
    CHECK (clean.bassImdDb < -100.0);
    CHECK (clean.smpteImdDb < -100.0);
    for (const auto& m : clean.mtnd)
        CHECK (m.db < -100.0);
    for (const auto& d : clean.ducking)
        CHECK_NEAR (d.track.spreadDb, 0.0, 0.01);
    CHECK_NEAR (clean.kickOnsetLiftDb - clean.kickBodyLiftDb, 0.0, 0.01);
    CHECK_NEAR (clean.kickCentroidShiftMs, 0.0, 0.01);
    CHECK_NEAR (clean.pinkOutLufs, clean.pinkInLufs, 0.01);

    // 1 % cubic: y = x + c x^3 with c = 0.16, so the -6 dBFS (A = 0.5) sine's
    // 3rd harmonic is 1 % of A. Expected values from the expansion of
    // (sum a_i cos w_i t)^3: harmonic c A^3 / 4, fundamental A + 3 c A^3 / 4
    // (+ 3 c A B^2 / 2 per other tone of amplitude B), 2f1 +- f2 products
    // 3 c A^2 B / 4.
    constexpr double c = 0.16;
    REQUIRE (measureQuality (values, 512, cubic, error, [] (Channels& out) {
        for (auto& ch : out)
            for (auto& s : ch)
                s = static_cast<float> (s + c * s * s * s);
    }));
    const double a = 0.5, h3 = c * a * a * a / 4.0, fund = a + 3.0 * c * a * a * a / 4.0;
    const double thdExpected = 20.0 * std::log10 (h3 / std::sqrt (fund * fund + h3 * h3));
    for (const auto& t : cubic.thdn)
        CHECK_NEAR (t.db, thdExpected, 0.41);
    const double b = 0.25, product = 0.75 * c * b * b * b, tone = b + c * b * b * b * (0.75 + 1.5);
    CHECK_NEAR (cubic.bassImdDb, 10.0 * std::log10 (4.0 * product * product / (2.0 * tone * tone)), 0.41);
    const double lo = 0.4, hi = 0.1, side = 0.75 * c * lo * lo * hi, hiOut = hi + c * (0.75 * hi * hi * hi + 1.5 * lo * lo * hi);
    CHECK_NEAR (cubic.smpteImdDb, 10.0 * std::log10 (2.0 * side * side / (hiOut * hiOut)), 0.41);
    measured ("meta: 1 % cubic THD+N (expected " + std::to_string (thdExpected) + ")", cubic.thdn[0].db, "dB");
    measured ("meta: 1 % cubic bass IMD", cubic.bassImdDb, "dB");
    measured ("meta: 1 % cubic SMPTE IMD", cubic.smpteImdDb, "dB");

    // 6 dB 2 Hz square gain modulation: every probe's gain track spreads
    // 6.02 dB, and its kick-rate component is the square wave's fundamental,
    // (4 / pi) x 3.01 dB (3rd harmonic a third of that, even ones 0).
    REQUIRE (measureQuality (values, 512, modulated, error, [] (Channels& out) {
        for (auto& ch : out)
            for (size_t i = 0; i < ch.size(); ++i)
                if (std::fmod (static_cast<double> (i) / kFs, 0.5) >= 0.25)
                    ch[i] *= 0.5f;
    }));
    const double squareFundamental = 4.0 / kPi * 0.5 * toDb (2.0);
    for (const auto& d : modulated.ducking)
    {
        CHECK_NEAR (d.track.spreadDb, 6.02, 0.602);
        CHECK_NEAR (d.track.modulationDb[0], squareFundamental, 0.1 * squareFundamental);
        CHECK_NEAR (d.track.modulationDb[2], squareFundamental / 3.0, 0.1 * squareFundamental / 3.0);
        CHECK (d.track.modulationDb[1] < 0.05);
        CHECK_NEAR (d.track.downPercent, 50.0, 5.0);
    }
    measured ("meta: 6 dB 2 Hz square, 2 kHz probe 2 Hz modulation", modulated.ducking[1].track.modulationDb[0], "dB");

    // 3 ms delay: the kick's energy centroid moves 3 ms.
    REQUIRE (measureQuality (values, 512, delayed, error, [] (Channels& out) {
        const auto d = static_cast<size_t> (samplesOf (0.003));
        for (auto& ch : out)
        {
            ch.insert (ch.begin(), d, 0.0f);
            ch.resize (ch.size() - d);
        }
    }));
    CHECK_NEAR (delayed.kickCentroidShiftMs, 3.0, 0.3);
    measured ("meta: 3 ms delay, kick centroid shift", delayed.kickCentroidShiftMs, "ms");

    // MTND: noise 40 dB under a multitone reads -40 dB (the residual after
    // the excited tones), a pure multitone reads nothing.
    {
        std::vector<double> tones { 50.0, 110.0, 230.0, 470.0, 1010.0, 2030.0, 4070.0, 8110.0 };
        const int n = samplesOf (1.0);
        std::vector<float> x (static_cast<size_t> (n));
        double power = 0.0;
        for (size_t k = 0; k < tones.size(); ++k)
        {
            const auto s = sine (tones[k], kFs, n, 0.1f);
            for (int i = 0; i < n; ++i)
                x[static_cast<size_t> (i)] += s[static_cast<size_t> (i)];
            power += 0.5 * 0.01;
        }
        CHECK (multitoneResidualDb (x.data(), n, kFs, tones) < -100.0);
        const auto noise = whiteNoise (n, 1.0f, 99);
        const double noiseRms = rms (noise.data(), n);
        const auto g = static_cast<float> (std::sqrt (power) * 0.01 / noiseRms);
        for (int i = 0; i < n; ++i)
            x[static_cast<size_t> (i)] += g * noise[static_cast<size_t> (i)];
        CHECK_NEAR (multitoneResidualDb (x.data(), n, kFs, tones), -40.0, 0.41);
    }
}

TEST_CASE ("CLI: render.stats reads the limiter GR of a steady sine as the hand-computed ceiling - (peak + drive), and all-zero on a chain with every module off")
{
    // 1 kHz at -6.02 dBFS peak (48 samples per cycle: the true peak is the
    // sample peak within 0.02 dB), maximizer alone with clipper share 0 and
    // glue 0, drive 9 dB, ceiling -1 dBTP: the limiter must take
    // -1 - (-6.02 + 9) = -3.98 dB on every sample once it has engaged.
    const auto input = stereoOf (sine (1000.0, kFs, samplesOf (2.0), 0.5f));
    auto values = resolve (RenderOptions {});
    onlyModules (values, { MaximizerOn });
    setValue (values, MaxClipAmount, 0.0f);
    setValue (values, MaxGlue, 0.0f);
    setValue (values, MaxDriveDb, 9.0f);
    setValue (values, MaxCeilingDb, -1.0f);
    RenderResult rr;
    std::string error;
    REQUIRE (renderFile (input, values, RenderSettings {}, rr, error));
    const double expected = -1.0 - (toDb (0.5) + 9.0);
    const auto& st = rr.stats;
    CHECK (st.frames == samplesOf (2.0));
    CHECK_NEAR (st.limiterGrMeanDb, expected, 0.1);
    CHECK_NEAR (st.limiterGrMaxDb, expected, 0.1);
    CHECK_NEAR (st.limiterOver3DbPercent, 100.0, 1.0);
    CHECK_NEAR (st.glueGrMaxDb, 0.0, 1.0e-6);
    CHECK (st.clipEnergyMaxDb <= kMinusInfDb + 0.5f);
    CHECK (st.safetyClips == 0);
    CHECK_NEAR (rr.outputReport.samplePeakDbfs, -1.0, 0.1);

    // JSON: the same numbers under render.stats, rounded to 0.01.
    const auto json = renderStatsToJson (st);
    REQUIRE (json["limiter"]["grMeanDb"].isNumber());
    CHECK_NEAR (json["limiter"]["grMeanDb"].asNumber(), st.limiterGrMeanDb, 0.006);
    CHECK_NEAR (json["frames"].asNumber(), samplesOf (2.0), 0.0);
    CHECK (json["clipper"]["energyMaxDb"].isNull()); // no measurement: null
    CHECK (json["governor"]["scaleMin"].isNumber());
    REQUIRE (json["modeBands"].asArray().size() == 4);
    CHECK_NEAR (json["modeBands"].asArray()[0]["band"].asNumber(), 4.0, 0.0);

    // Every module off: nothing acts.
    onlyModules (values, {});
    REQUIRE (renderFile (input, values, RenderSettings {}, rr, error));
    CHECK_NEAR (rr.stats.limiterGrMaxDb, 0.0, 1.0e-6);
    CHECK_NEAR (rr.stats.compGrMaxDb, 0.0, 1.0e-6);
    CHECK_NEAR (rr.stats.governorScaleMin, 1.0, 1.0e-6);
    CHECK_NEAR (rr.stats.limiterOver1DbPercent, 0.0, 1.0e-6);
    for (size_t b = 0; b < 4; ++b)
        CHECK_NEAR (rr.stats.modeBandMaxDb[b], 0.0, 1.0e-6);
}

// =============================================================================
// Protection metrics (docs/11 E06 slice, E10 Phase 1)
// =============================================================================
namespace
{
/** As render(), straight through a ProcessingChain at 512-sample blocks with
    the host's protection strength set (the CLI has no switch for it): primed
    the same way, latency compensated. */
struct GovernorReading
{
    double scale = 1.0, distortionDb = -160.0; // at the end of the render
};

Channels renderAtStrength (const io::AudioFileData& input, const std::vector<float>& values, ProtectionStrength strength,
                           GovernorReading* reading = nullptr)
{
    ParameterStore store;
    for (int id = 0; id < kNumParams; ++id)
        store.set (id, values[static_cast<size_t> (id)]);
    constexpr int kBlock = 512;
    ProcessingChain chain (store);
    chain.prepare ({ kFs, kBlock, 2 });
    chain.setProtectionStrength (strength);
    const int latency = chain.getLatencySamples();
    const int n = static_cast<int> (input.channels[0].size());
    Planar buf (2, n + latency);
    for (int c = 0; c < 2; ++c)
        std::copy (input.channels[static_cast<size_t> (c)].begin(), input.channels[static_cast<size_t> (c)].end(), buf.ch[static_cast<size_t> (c)].begin());
    ScopedNoDenormals noDenormals;
    Planar prime (2, kBlock);
    chain.process (prime.block (0, kBlock));
    chain.reset();
    for (int pos = 0; pos < n + latency; pos += kBlock)
        chain.process (buf.block (pos, std::min (kBlock, n + latency - pos)));
    if (reading != nullptr)
        *reading = { chain.meters().governorScale.load(), chain.meters().governorDistortionDb.load() };
    Channels out (2);
    for (int c = 0; c < 2; ++c)
        out[static_cast<size_t> (c)].assign (buf.ch[static_cast<size_t> (c)].begin() + latency, buf.ch[static_cast<size_t> (c)].end());
    return out;
}

/** Pink bed at -24 dBFS RMS plus 55 Hz kicks (peak 0.7, tau 100 ms) every 500 ms. */
io::AudioFileData kickProgramme (double seconds)
{
    const int n = samplesOf (seconds);
    auto l = pinkNoise (n, std::pow (10.0f, -24.0f / 20.0f), 707);
    auto r = pinkNoise (n, std::pow (10.0f, -24.0f / 20.0f), 808);
    for (int i = 0; i < n; ++i)
    {
        const double beat = std::fmod (i / kFs, 0.5);
        const auto kick = static_cast<float> (0.7 * std::exp (-beat / 0.1) * std::sin (kTwoPi * 55.0 * beat));
        l[static_cast<size_t> (i)] += kick;
        r[static_cast<size_t> (i)] += kick;
    }
    return fileOf ({ l, r });
}
} // namespace

TEST_CASE ("KnownGap closed: governed macros at 64..4096-sample blocks - Boost 100 + Loudness 100 on kick-heavy programme lands within 0.05 LU integrated (E06)")
{
    // The governor ticks on the maximizer's 10 ms GR-window grid and the chain
    // ends its processing segments on that grid, so the scale's trajectory -
    // the only block-size dependent part of the chain on this programme -
    // is nearly the same at every host block size. Before the E06 slice (a
    // per-block tick with dt = block length, the new scale taking effect at
    // the next host block) this scene spread 0.25 LU (-10.40 LUFS at 256 to
    // -10.65 at 4096 samples); now 0.047 LU. What is left is the 25 ms
    // THD+N windows of the saturator and the clipper, which close at the
    // first segment boundary after 25 ms (1216 samples at 64-sample blocks,
    // 1440 at 480 and above).
    const auto input = kickProgramme (20.0);
    RenderOptions o = boosted (ModeValue::Music, 100.0f);
    o.macros.push_back ({ "loudness", 100.0f });
    const auto values = resolve (o);
    double lo = 1.0e9, hi = -1.0e9;
    for (const int block : { 64, 256, 512, 1024, 2048, 4096 })
    {
        Channels out;
        int latency = 0;
        std::string error;
        REQUIRE (renderPass (input, values, block, out, latency, error));
        const double lufs = analyse (out, kFs).integratedLufs;
        measured ("Boost 100 + Loudness 100 integrated at " + std::to_string (block) + "-sample blocks", lufs, "LUFS");
        lo = std::min (lo, lufs);
        hi = std::max (hi, lufs);
    }
    measured ("Boost 100 + Loudness 100 block-size spread", hi - lo, "LU");
    CHECK_LE (hi - lo, 0.05); // docs/11 E06 Done-when
}

TEST_CASE ("KnownGap closed: a single 1e30 sample disturbs the output for under 50 ms and leaves the level alone; a NaN burst does not regress (E10)")
{
    // -20 dBFS pink through the Signature preset, one corrupt sample at 2 s.
    // Before the sanitiser a finite 1e30 passed the non-finite guard: about
    // 100 ms of silence, then +13.7 dB at 2.1-2.5 s and -2.1 dB at 4-5 s, a
    // disturbance above -60 dBFS for 6 s (CLI, same scene). It is now muted
    // and its block is hidden from the control loops.
    const int n = samplesOf (6.0);
    const auto input = fileOf ({ pinkNoise (n, 0.1f, 31), pinkNoise (n, 0.1f, 32) });
    const auto values = resolve (factoryPreset ("music-flubsound-signature.json"));
    const auto clean = render (input, values);
    const int at = samplesOf (2.0);

    struct Disturbance
    {
        double spanMs, worstChangeDb;
    };
    const auto disturbance = [&] (const io::AudioFileData& spiked) {
        const auto out = render (spiked, values);
        int first = -1, last = -1;
        for (int i = 0; i < n; ++i)
        {
            const float d = std::max (std::abs (out[0][static_cast<size_t> (i)] - clean[0][static_cast<size_t> (i)]),
                                      std::abs (out[1][static_cast<size_t> (i)] - clean[1][static_cast<size_t> (i)]));
            if (d > 1.0e-3f) // -60 dBFS
            {
                first = first < 0 ? i : first;
                last = i;
            }
        }
        Disturbance r { first < 0 ? 0.0 : 1000.0 * (last - first + 1) / kFs, 0.0 };
        // Level change from 100 ms after the spike on (docs/11 E10 Done-when).
        for (const auto& w : { Window { at + samplesOf (0.1), at + samplesOf (0.5) }, Window { at + samplesOf (0.5), at + samplesOf (1.0) },
                               Window { at + samplesOf (1.0), at + samplesOf (2.0) }, Window { at + samplesOf (2.0), n } })
            r.worstChangeDb = std::max (r.worstChangeDb, std::abs (powerDb (meanPower (out, { w })) - powerDb (meanPower (clean, { w }))));
        return r;
    };

    auto spiked = input;
    spiked.channels[0][static_cast<size_t> (at)] = 1.0e30f;
    const auto spike = disturbance (spiked);
    auto both = input;
    both.channels[0][static_cast<size_t> (at)] = both.channels[1][static_cast<size_t> (at)] = -1.0e30f;
    const auto spikeBoth = disturbance (both);
    auto nan = input;
    for (int i = at; i < at + 480; ++i)
        nan.channels[0][static_cast<size_t> (i)] = std::numeric_limits<float>::quiet_NaN();
    const auto burst = disturbance (nan);
    measured ("1e30 spike disturbance above -60 dBFS", spike.spanMs, "ms");
    measured ("1e30 spike level change after 100 ms", spike.worstChangeDb, "dB");
    measured ("-1e30 on both channels disturbance above -60 dBFS", spikeBoth.spanMs, "ms");
    measured ("NaN burst (10 ms) disturbance above -60 dBFS", burst.spanMs, "ms");
    measured ("NaN burst level change after 100 ms", burst.worstChangeDb, "dB");

    // docs/11 E10 Done-when: <= 50 ms above -60 dBFS, <= 0.3 dB after 100 ms.
    CHECK_LE (spike.spanMs, 50.0);
    CHECK_LE (spike.worstChangeDb, 0.3);
    CHECK_LE (spikeBoth.spanMs, 50.0);
    CHECK_LE (spikeBoth.worstChangeDb, 0.3);
    // The NaN burst drops its blocks (silence) and restarts the signal path;
    // the control loops keep their state. Unchanged by E10 Phase 1. Re-based
    // by docs/11 E12 Phase A, 799.8 -> 441.0 ms: Signature's width 1.23 no
    // longer widens this uncorrelated pink noise (the width polarity guard
    // caps S at M), so the mono safety's pull, which the restart clears,
    // no longer shapes the output (799.8 ms with the guard disabled).
    CHECK_NEAR (burst.spanMs, 441.0, 20.0);
    CHECK_LE (burst.worstChangeDb, 0.3);
}

TEST_CASE ("KnownGap: DC after the maximizer - an asymmetric 100 + 200 Hz signal at 24 dB drive leaves <= -60 dBFS DC in Quality and Balanced; Low Latency and the limiter alone still leave some (E10)")
{
    // 100 Hz + 200 Hz (+90 degrees), 0.35 each: no DC in, a strongly
    // asymmetric waveform. Default chain with max.drive 24, 2..4 s. Before
    // the residual-path DC blocker on the clipper's correction the output
    // held -15.8 dBFS DC in Balanced (-22.0 dBFS at drive 12; CLI).
    const int n = samplesOf (4.0);
    std::vector<float> x (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
        x[static_cast<size_t> (i)] = static_cast<float> (0.35 * std::sin (kTwoPi * 100.0 * i / kFs) + 0.35 * std::cos (kTwoPi * 200.0 * i / kFs));
    const auto input = stereoOf (x);
    const auto dcDb = [&] (const Channels& out) {
        double sum = 0.0;
        for (const auto& c : out)
            for (int i = samplesOf (2.0); i < n; ++i)
                sum += c[static_cast<size_t> (i)];
        return toDb (std::abs (sum) / (2.0 * (n - samplesOf (2.0))));
    };
    struct Row
    {
        LatencyProfileValue profile;
        const char* name;
        double chainDb, limiterOnlyDb; // measured after E10 Phase 1
    };
    for (const auto& r : { Row { LatencyProfileValue::Quality, "Quality", -73.55, -51.42 }, Row { LatencyProfileValue::Balanced, "Balanced", -68.81, -49.02 },
                           Row { LatencyProfileValue::LowLatency, "Low Latency", -58.70, -42.22 } })
    {
        RenderOptions o;
        o.profile = r.profile;
        auto values = resolve (o);
        setValue (values, MaxDriveDb, 24.0f);
        const double dc = dcDb (render (input, values));
        setValue (values, MaxClipAmount, 0.0f);
        const double limiterOnly = dcDb (render (input, values));
        measured (std::string ("DC at max.drive 24, ") + r.name, dc, "dBFS");
        measured (std::string ("DC at max.drive 24 with the clipper off, ") + r.name, limiterOnly, "dBFS");
        // docs/11 E10 Done-when: <= -60 dBFS in every profile. What is left
        // comes from the true-peak limiter's gain modulation of the
        // asymmetric waveform (the clipper-off rows; faster in Low Latency's
        // 0.5 ms look-ahead). A residual high-pass there would move peaks
        // past the ceiling (KNOWN_GAP for Low Latency and the limiter alone).
        if (r.profile != LatencyProfileValue::LowLatency)
            CHECK_LE (dc, -60.0);
        CHECK_NEAR (dc, r.chainDb, 1.0);
        CHECK_NEAR (limiterOnly, r.limiterOnlyDb, 1.0);
    }
}

TEST_CASE ("KnownGap: all Music macros at 100 on a 50 Hz sine - THD+N at protection strength Off, Normal and Strict, with and without driven base settings (E06)")
{
    // -12 dBFS 50 Hz, Music, Boost 100 and macros 1-5 at 100 %, THD+N over
    // 6..10 s. Off governs the macro amounts only (as before E06); Normal
    // also scales the base max.drive, sat.drive and bass.harmonics; Strict
    // does that and lets the scale fall to 0 instead of 0.3. Scene b adds
    // driven base settings (max.drive 12, Tape saturation at 12 dB).
    const auto input = stereoOf (sine (50.0, kFs, samplesOf (10.0), std::pow (10.0f, -12.0f / 20.0f)));
    RenderOptions o = boosted (ModeValue::Music, 100.0f);
    for (const char* m : { "1", "2", "3", "4", "5" })
        o.macros.push_back ({ m, 100.0f });
    const auto macros = resolve (o);
    auto driven = macros;
    setValue (driven, MaxDriveDb, 12.0f);
    setValue (driven, SaturationOn, 1.0f);
    setValue (driven, SatDriveDb, 12.0f);
    double thd[2][3] = {}, governed[2][3] = {};
    for (int scene = 0; scene < 2; ++scene)
        for (const auto s : { ProtectionStrength::Off, ProtectionStrength::Normal, ProtectionStrength::Strict })
        {
            GovernorReading gr;
            const auto out = renderAtStrength (input, scene == 0 ? macros : driven, s, &gr);
            const auto k = static_cast<size_t> (s);
            thd[scene][k] = thdPlusNoiseDb (out[0], samplesOf (6.0), samplesOf (4.0), 50.0);
            governed[scene][k] = gr.distortionDb;
            const std::string tag = std::string (scene == 0 ? "all Music macros 100" : "... with max.drive 12 + sat.drive 12")
                                    + ", strength " + std::to_string (static_cast<int> (s));
            measured (tag + ": 50 Hz THD+N", thd[scene][k], "dB");
            measured (tag + ": 50 Hz THD+N (percent)", 100.0 * std::pow (10.0, thd[scene][k] / 20.0), "%");
            measured (tag + ": governor scale at 10 s", gr.scale, "");
            measured (tag + ": governor THD+N input (3 s average)", gr.distortionDb, "dB");
        }
    // Off is the behaviour before E06 (the CLI measures 20.3 % on scene a).
    CHECK_NEAR (thd[0][0], -13.85, 0.3);
    // KNOWN_GAP: target <= 3 % (-30.5 dB) at Normal per docs/11 E06 Done-when.
    // Music's base drives are 0 dB: every drive on scene a is a governed
    // macro amount, already scaled at Off, and the scale sits at its 0.3
    // floor, so Normal changes nothing here. Strict (floor 0) removes every
    // governed amount and still measures 5.3 %: the rest is ungoverned (the
    // governor's own input stays over its -30 dB budget at scale 0, and the
    // limiter's LF distortion is not measured at all - docs/11 E06 step 1,
    // E05's LF envelope).
    CHECK_NEAR (thd[0][1], thd[0][0], 0.05);
    CHECK_NEAR (thd[0][2], -25.51, 0.3);
    CHECK_GE (governed[0][2], SafetyGovernor::kDistortionBudgetDb);
    // Driven base settings: Normal governs them too, and the governor's
    // measured input (saturator + clipper) falls 6 dB; the output's THD+N
    // does not, because less clipper drive hands the 50 Hz peaks to the
    // limiter, whose distortion the governor does not see (KNOWN_GAP, E06
    // step 1).
    CHECK_NEAR (thd[1][0], -15.27, 0.3);
    CHECK_NEAR (thd[1][1], -14.11, 0.3);
    CHECK_NEAR (thd[1][2], -20.83, 0.3);
    CHECK_LE (governed[1][1], governed[1][0] - 3.0);
}
