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
//   * pumping: a 2 kHz tone under 55 Hz kicks at Boost 100 (E05, E02; closed
//     by E05 step 5's LF-first limiter)
//   * 60 Hz and 1 kHz THD+N of a -6 dBFS sine at 12 dB maximizer drive (E05;
//     closed by E05 stage 1)
//   * the E59 quality suite (`flubsound-cli quality`, Commands.h
//     measureQuality: THD+N, two-tone / SMPTE IMD, multitone MTND against
//     output loudness, ducking spectrum of 1-8 kHz probes under kicks, kick
//     onset and timing, pink loudness) on the maximizer alone at 12 dB drive
//     and on Music Boost 100 (E05 stage 1), and its meta-validation
//   * 7.1 LFE fold, virtualiser off and on, against one main channel (E01;
//     the LFE is folded since E01, the +10 dB target is still open)
//   * stereo content in an 8-channel container: centre notch and FL-only
//     separation of the fold (E27; closed by the stereo passthrough fold)
//   * burst-footstep lift on isolated 20 / 40 / 80 ms bursts, and step/bed
//     contrast at -14 / -24 / -40 LUFS (and -50 LUFS) (E19; closed by the
//     cue enhancer, kept as its regression tests; tests/test_scenes.cpp
//     has the E60 scenes)
//   * the Night Mode ambush scene: bed lift and post-event hole (E21; the
//     hole closed by the AutoLevel slice, the bed lift by the Phase 3 retune)
//   * kick onset: Punch 100 lift at 0-10 ms against 10-30 ms, Tighten 0.5's
//     change of the first 10 ms (closed by E04 step 2), onset against body
//     at Boost 80 / 100 (E04, E05)
//   * bass-line pumping: a 32 Hz line under 55 Hz kicks through Bass Head,
//     as shipped and with split-band protection (E02 (a)); the subsonic
//     slice: Bass Head's 40 Hz group delay, the gaming presets' subsonic
//     cost at 28 Hz (E02)
//   * 30 Hz audible-band (>= 120 Hz) energy of the laptop preset (E03; closed
//     by the preset slice, kept as its regression test)
//   * 3 kHz ILD added by Positional Focus (E24; closed by its 3 dB cap)
//   * 3.2 kHz lift at Bluetooth hands-free rates, 8 / 16 / 32 kHz (E17)
//   * protection (E06 slice, E10 Phase 1): integrated-loudness spread of
//     governed macros over host block sizes 64..4096 (closed); disturbance
//     of a single 1e30 sample and of a NaN burst (closed); DC after the
//     maximizer at 24 dB drive per latency profile (closed in every profile
//     by E05 stage 1); 50 Hz THD+N of all Music
//     macros at protection strength Off / Normal / Strict (closed at Normal
//     by E06 Phase 3's measured loop)
//   * hot master (-9.2 LUFS, -0.35 dBTP): limiter time > 1 dB and clip energy
//     of Signature and Punchy Pop with and without the automatic preamp (E11)
//   * high-frequency harshness (E07): sibilance over voice of a sung-vocal
//     stand-in at Music Boost 100 + Clarity 100 with and without Smoothness;
//     the Gaming full stack's 2-5 kHz tilt at protection strength Off and
//     Normal (closed at Normal by the tonal-balance rule); presence at -45
//     against -12 dBFS (closed by clarity.presenceMode Relative, E07 step
//     3; Absolute, the default, pinned), and E59's presence-invariance row
//     over 30 dB
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
#include "flub/dsp/Fft.h"
#include "flub/dsp/Svf.h"
#include "flub/engine/Parameters.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/engine/Protection.h"
#include "flub/io/PresetIO.h"
#include "flub/io/WavFile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
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
TEST_CASE ("KnownGap closed: pumping - a 2 kHz tone under 55 Hz kicks dips <= 3 dB at Boost 100 (E05 step 5 / E02)")
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
    // Re-baselined by docs/11 E05 stage 1 (crest-gated clipper, LF-safe
    // limiter envelope): spread 3.60 -> 4.86 dB, dip 2.41 -> 5.52, lift 3.56
    // -> 1.74, time > 1 dB down 2.7 -> 14.0 %, integrated -10.26 -> -10.19
    // LUFS (CLI, same scene). The governor used to back Boost off to a mean
    // scale of 0.64 because the clipper squared the kicks off (THD+N mean
    // -30.4 dB, over the -30 dB budget); the crest-gated clipper leaves the
    // kick's body alone (-32.7 dB), the scale stays at 1 and the limiter
    // takes the kicks instead (GR mean -0.24 -> -2.68 dB). With the drive
    // held fixed the same stage halves the dip: maximizer alone at 12 dB,
    // `quality` ducking 2 kHz probe dip 8.9 -> 3.7 dB, p95 - p5 6.3 -> 4.6.
    // docs/11 E05 step 5: Boost's LF-first limiter (max.lfLimit, from Boost
    // 50 %) limits the kicks in the low band before the bands are summed, so
    // the wideband limiter no longer ducks the tone with them: spread 4.86
    // -> 1.25 dB, dip 5.52 -> 2.49, lift 1.74 -> 0.97, time > 1 dB down 14.0
    // -> 6.0 %, integrated -10.19 -> -9.35 LUFS (CLI). docs/11 E05
    // Done-when: max dip <= 6 dB met, and the <= 3 dB it expected only from
    // a multiband mode.
    // docs/11 E05 step 6 (Boost's transient coupling, up to +1 dB of
    // Clarity attack from the limiter's transient GR; the LF-first limiter
    // unchanged at 100 %): dip 2.49 -> 2.10 dB, spread 1.25 -> 0.99, time
    // > 1 dB down 6.0 -> 4.0 %, lift 0.97 -> 1.46 dB (the tone rides up a
    // little more between the kicks, whose onsets now take slightly more GR).
    CHECK_NEAR (rows[1].dip, 2.10, 0.3);
    CHECK_LE (rows[1].dip, 3.0);
    // E59 reports p95 - p5 and lift / dip separately; docs/11 E05 / E02 set no target for them.
    CHECK_NEAR (rows[1].spread, 0.99, 0.3);
    CHECK_NEAR (rows[1].lift, 1.46, 0.3);
    CHECK_NEAR (100.0 * rows[1].down, 4.0, 3.0);
}

TEST_CASE ("KnownGap closed: 60 Hz and 1 kHz THD+N of a -6 dBFS sine at 12 dB maximizer drive (E05)")
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
    // Closed by docs/11 E05 stage 1: -16.11 / -16.21 -> -131.2 / -111.3 dB.
    // The crest gate leaves a steady sine (crest 3 dB, under the 6 dB gate)
    // unclipped, so the limiter takes the drive, and its period hold keeps
    // the gain flat from one 60 Hz half-cycle peak to the next.
    CHECK_LE (thd[0], -30.0); // docs/11 E05 Done-when: <= -30 dB at 60 Hz
    CHECK_LE (thd[1], -30.0); // ... and at 1 kHz
    CHECK_LE (thd[0], -100.0);
    CHECK_LE (thd[1], -100.0);
}

namespace
{
/** `flubsound-cli quality --set max.drive=12` with every other module off
    (Commands.h measureQuality; stimuli and metrics defined there), the
    clipper at its default share or off (`clipperOff`); prints the report. */
QualityReport maximizerQualityAt12 (bool clipperOff)
{
    auto values = resolve (RenderOptions {});
    onlyModules (values, { MaximizerOn });
    setValue (values, MaxDriveDb, 12.0f);
    if (clipperOff)
        setValue (values, MaxClipAmount, 0.0f);
    QualityReport report;
    std::string error;
    REQUIRE (measureQuality (values, 512, report, error));
    const auto* r = &report;
    const std::string tag = ! clipperOff ? "max.drive 12: " : "max.drive 12, clipper off: ";
    for (const auto& t : r->thdn)
        measured (tag + "THD+N at " + std::to_string (static_cast<int> (t.hz)) + " Hz", t.db, "dB");
    measured (tag + "IMD 50 + 63 Hz", r->bassImdDb, "dB");
    measured (tag + "IMD SMPTE", r->smpteImdDb, "dB");
    for (const auto& m : r->mtnd)
    {
        measured (tag + "MTND at " + std::to_string (static_cast<int> (m.inputRmsDbfs)) + " dBFS", m.db, "dB");
        measured (tag + "  output loudness", m.outputLufs, "LUFS");
    }
    measured (tag + "2 kHz probe under kicks: max dip", r->ducking[1].track.dipDb, "dB");
    measured (tag + "2 kHz probe under kicks: p95-p5", r->ducking[1].track.spreadDb, "dB");
    measured (tag + "2 kHz probe under kicks: 2 Hz modulation", r->ducking[1].track.modulationDb[0], "dB");
    measured (tag + "kick onset minus body", r->kickOnsetLiftDb - r->kickBodyLiftDb, "dB");
    measured (tag + "kick energy centroid shift", r->kickCentroidShiftMs, "ms");
    measured (tag + "pink -18 dBFS out", r->pinkOutLufs, "LUFS");
    return report;
}
} // namespace

// One case per clipper setting (each a whole quality-suite run), so each
// stays under the suite's 2 s per case. Values before docs/11 E05 stage 1
// in the comments (same suite, pre-change build).
TEST_CASE ("KnownGap: the maximizer at 12 dB drive on the E59 quality suite - THD+N, IMD, MTND, ducking of 1-8 kHz probes under kicks, kick onset and loudness (E05 stage 1) - clipper off")
{
    const auto limiterOnly = maximizerQualityAt12 (true);
    // docs/11 E05 Done-when: 40 Hz at about 7 dB GR <= -45 dB in Balanced
    // (limiter alone, before -32.5).
    for (const auto* r : { &limiterOnly })
        for (const auto& t : r->thdn)
            CHECK_LE (t.db, -100.0);
    // SMPTE 60 Hz + 7 kHz, the limiter alone -37.8 -> -107.8 dB.
    CHECK_LE (limiterOnly.smpteImdDb, -90.0);
    // Loudness of -18 dBFS pink, limiter alone -7.32 -> -8.31 LUFS.
    CHECK_NEAR (limiterOnly.pinkOutLufs, -8.31, 0.2);
}

TEST_CASE ("KnownGap: the maximizer at 12 dB drive on the E59 quality suite - THD+N, IMD, MTND, ducking of 1-8 kHz probes under kicks, kick onset and loudness (E05 stage 1) - clipper at its default share")
{
    const auto q = maximizerQualityAt12 (false);
    // docs/11 E05 Done-when: sines at 12 dB drive <= -30 dB THD+N at 60 Hz
    // and 1 kHz (before -16.3 / -16.2).
    for (const auto* r : { &q })
        for (const auto& t : r->thdn)
            CHECK_LE (t.db, -100.0);
    // IMD: bass third -15.3 -> -33.5 dB, SMPTE 60 Hz + 7 kHz -5.1 -> -45.2 dB
    // (the old clipper chopped the 7 kHz riding on the clipped 60 Hz).
    CHECK_NEAR (q.bassImdDb, -33.46, 1.0);
    CHECK_NEAR (q.smpteImdDb, -45.21, 1.0);
    // MTND at -18 / -12 dBFS RMS in: -23.6 / -13.4 -> -30.1 / -21.6 dB, at
    // -7.8 / -4.4 -> -8.3 / -7.7 LUFS out (the clipper's loudness came with
    // its distortion; at matched loudness, -8 LUFS, about 6 dB cleaner).
    CHECK_NEAR (q.mtnd[1].db, -30.09, 0.5);
    CHECK_NEAR (q.mtnd[2].db, -21.63, 0.5);
    CHECK_NEAR (q.mtnd[1].outputLufs, -8.27, 0.2);
    CHECK_NEAR (q.mtnd[2].outputLufs, -7.67, 0.2);
    // Ducking of the probes under 55 Hz kicks at a fixed 12 dB drive (the
    // wideband limiter's pumping): max dip 8.9 -> 3.7 dB (docs/11 E05
    // Done-when <= 6 dB), p95 - p5 6.3 -> 4.6 dB, 2 Hz modulation 2.07 ->
    // 1.35 dB, the same on every probe from 1 to 8 kHz.
    for (const auto& d : q.ducking)
    {
        CHECK_LE (d.track.dipDb, 6.0);
        CHECK_NEAR (d.track.dipDb, 3.74, 0.4);
        CHECK_NEAR (d.track.spreadDb, 4.56, 0.4);
        CHECK_NEAR (d.track.modulationDb[0], 1.35, 0.2);
    }
    // Kick onset minus body -1.64 -> -0.51 dB (KNOWN_GAP: >= 0 per docs/11
    // E05 Done-when at Boost 100; E04's attack coupling). The energy
    // centroid moves 5.8 -> 8.6 ms later: the held gain lifts the kick's
    // decay (KNOWN_GAP: < 2 ms per docs/11 E59 kick alignment).
    CHECK_NEAR (q.kickOnsetLiftDb - q.kickBodyLiftDb, -0.51, 0.3);
    CHECK_NEAR (q.kickCentroidShiftMs, 8.62, 0.5);
    // Loudness of -18 dBFS pink: -6.28 -> -6.90 LUFS (limiter alone: the case above).
    CHECK_NEAR (q.pinkOutLufs, -6.90, 0.2);
}

TEST_CASE ("KnownGap closed: Music Boost 100 on the E59 quality suite - loudness within 1.5 LU of the pre-E05 maximizer, probes under kicks dip <= 3 dB, kick onset >= body (E05 stage 1, step 5)")
{
    // `flubsound-cli quality --mode music --boost 100` (the whole chain,
    // governed). docs/11 E05 Done-when: integrated loudness at max Boost
    // within 1.5 LU of today: pink -8.02 -> -8.71 LUFS.
    QualityReport q;
    std::string error;
    REQUIRE (measureQuality (resolve (boosted (ModeValue::Music, 100.0f)), 512, q, error));
    measured ("Music Boost 100: pink -18 dBFS out", q.pinkOutLufs, "LUFS");
    measured ("Music Boost 100: 2 kHz probe under kicks: max dip", q.ducking[1].track.dipDb, "dB");
    measured ("Music Boost 100: 2 kHz probe under kicks: p95-p5", q.ducking[1].track.spreadDb, "dB");
    measured ("Music Boost 100: kick onset minus body", q.kickOnsetLiftDb - q.kickBodyLiftDb, "dB");
    measured ("Music Boost 100: THD+N at 1 kHz", q.thdn[3].db, "dB");
    CHECK_GE (q.pinkOutLufs, -8.02 - 1.5);
    CHECK_NEAR (q.pinkOutLufs, -8.71, 0.2);
    // The probes: dip 2.4 -> 5.4 dB, p95 - p5 3.5 -> 5.0 dB, lift 3.3 -> 1.6
    // dB. Before, the old clipper's distortion of the kicks (THD+N over the
    // -30 dB budget) made the governor hold Boost at ~0.64 of its amount;
    // now the full governed drive reaches the limiter (see the pumping
    // KnownGap above). docs/11 E05 step 5 (Boost's LF-first limiter,
    // max.lfLimit): dip 5.37 -> 2.55 dB, p95 - p5 4.99 -> 1.09 dB - under the
    // <= 3 dB the item expected only from a multiband mode; pink loudness
    // unchanged. The cost: the kick's low band is held 3 dB under the
    // ceiling (CLI, the pumping scene: its < 150 Hz lift +4.0 -> +1.3 dB,
    // the 2 kHz tone +5.8 -> +7.5 dB).
    // docs/11 E05 step 6 (Boost's transient coupling): dip 2.55 -> 2.23 dB,
    // p95 - p5 1.09 -> 1.03 dB.
    CHECK_LE (q.ducking[1].track.dipDb, 3.0);
    CHECK_NEAR (q.ducking[1].track.dipDb, 2.23, 0.4);
    CHECK_NEAR (q.ducking[1].track.spreadDb, 1.03, 0.4);
    // Kick onset minus body -1.80 -> -1.18 dB (stage 1) -> +0.21 dB (step 5;
    // docs/11 E05 Done-when >= 0 met) -> +0.30 dB (step 6).
    CHECK_NEAR (q.kickOnsetLiftDb - q.kickBodyLiftDb, 0.30, 0.3);
    CHECK_GE (q.kickOnsetLiftDb - q.kickBodyLiftDb, 0.0);
    // A steady 1 kHz sine at Boost 100: -50.3 -> -94.8 dB THD+N.
    CHECK_LE (q.thdn[3].db, -80.0);
}

TEST_CASE ("KnownGap closed: 7.1 LFE - an LFE-only 50 Hz tone folds at +10 dB re one main channel with the virtualiser off and on (E01)")
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

    // The FL reference (not a gap): the BS.775 fold passes one main at -3 dB
    // to one side. The virtualiser sends it to both ears and, since its level
    // match (docs/11 E28a), at the downmix's loudness: 3 dB lower at each ear
    // (-15.01 -> -18.04 dBFS), while the LFE keeps its level.
    CHECK_NEAR (mainOff, -15.05, 0.3);
    CHECK_NEAR (mainOn, -18.04, 0.3);
    // Fixed by E01 (shared Bs775Fold / LfeFold, virt.lfe default +6 dB, then
    // +10 dB with the fold's headroom on E05's LF-first limiter): before, the
    // LFE was dropped with the virtualiser off (-240 dBFS, exact silence) and
    // sat at -0.03 dB re FL with it on; at +6 dB 6.00 / 8.99 dB. Now both
    // folds put it at +10 dB re one main (E01 Done-when: within 1 dB of +10
    // and of each other).
    CHECK_NEAR (lfeOff - mainOff, 10.00, 0.3);
    CHECK_NEAR (lfeOn - mainOn, 12.99, 0.3); // per ear; the loudness ratio is the downmix's (E28a), +10 dB
    CHECK_LE (std::abs (lfeOn - lfeOff), 1.0);
    CHECK_NEAR (lfeOff - mainOff, 10.0, 1.0);
}

TEST_CASE ("KnownGap closed: 5.1 pink - the band-limited LFE sits +10 dB re one main in the BS.775 fold and the virtualiser (E01)")
{
    // docs/11 E01 Done-when's pink reference: independent pink on the five
    // mains at -30 dBFS RMS, pink low-passed at 120 Hz on the LFE at the same
    // RMS. Every module but the virtualiser (on or off) off, the surround fold
    // forced. Ratio: the output's power over 25..100 Hz, both channels, 1..4 s,
    // with only the LFE playing over that with only FL playing its own 120 Hz
    // band-limited pink (the LFE's twin). The LFE reaches both sides and FL
    // one (BS.775) or both at -3 dB each (the virtualiser's level match), so
    // the ratio is virt.lfe + 3.01 dB in both folds, less the fold's own
    // 120 Hz low-pass on the already band-limited LFE. The host's capture fold
    // (AudioEngineHost) runs the same Bs775Fold at the strip's virt.lfe
    // (tests/app/test_app_host_io.cpp, tones).
    const int n = samplesOf (4.0);
    auto lowPassed = [n] (uint32_t seed) {
        auto x = pinkNoise (n, dbToGain (-30.0f), seed);
        for (int s = 0; s < 2; ++s)
        {
            SvfFilter lp;
            lp.set (FilterType::LowPass, 120.0, butterworthQ (2, s), 0.0, kFs);
            float* p[] = { x.data() };
            lp.process (AudioBlock (p, 1, n));
        }
        return x;
    };
    const auto lfe = lowPassed (5101), main = lowPassed (5102);
    auto bandPower = [&] (int channel, const std::vector<float>& signal, bool virt) {
        auto values = resolve (RenderOptions {});
        onlyModules (values, { VirtualizerOn });
        setValue (values, VirtualizerOn, virt ? 1.0f : 0.0f);
        setValue (values, VirtInputMode, static_cast<float> (InputModeValue::ForceSurround));
        Channels c (6, std::vector<float> (static_cast<size_t> (n), 0.0f));
        c[static_cast<size_t> (channel)] = signal;
        const auto out = render (fileOf (std::move (c)), values);
        double power = 0.0;
        for (int ch = 0; ch < 2; ++ch)
        {
            std::vector<float> y (out[static_cast<size_t> (ch)].begin() + samplesOf (1.0), out[static_cast<size_t> (ch)].end());
            float* p[] = { y.data() };
            SvfFilter hp, lp;
            hp.set (FilterType::HighPass, 25.0, 0.7071, 0.0, kFs);
            lp.set (FilterType::LowPass, 100.0, 0.7071, 0.0, kFs);
            hp.process (AudioBlock (p, 1, static_cast<int> (y.size())));
            lp.process (AudioBlock (p, 1, static_cast<int> (y.size())));
            power += rms (y.data(), static_cast<int> (y.size())) * rms (y.data(), static_cast<int> (y.size()));
        }
        return 10.0 * std::log10 (std::max (power, 1.0e-30));
    };
    const double bs775 = bandPower (3, lfe, false) - bandPower (0, main, false);
    const double virt = bandPower (3, lfe, true) - bandPower (0, main, true);
    measured ("LFE re one main, 25-100 Hz, BS.775 fold", bs775, "dB");
    measured ("LFE re one main, 25-100 Hz, virtualiser", virt, "dB");
    // Default virt.lfe +10 dB: 12.74 / 12.75 dB (+6 dB: 8.74 / 8.75; before
    // E01: the LFE dropped / -0.03 dB). The Done-when: the ratio within 1 dB
    // in both folds, at the +10 dB convention.
    CHECK_NEAR (bs775, 10.0 + 3.01, 1.0);
    CHECK_NEAR (virt, 10.0 + 3.01, 1.0);
    CHECK_LE (std::abs (bs775 - virt), 1.0);
}

TEST_CASE ("KnownGap closed: the LFE fold's headroom - +10 dB LFE explosions ride the LF-first limiter, not the whole mix (E01)")
{
    // 7.1, defaults (Music, maximizer on, virtualiser off: the BS.775 fold,
    // forced): -30 dBFS RMS pink on the mains, -26 on FC, and four
    // explosions 1.5 s apart - a 35 Hz decaying sine at -3 dBFS peak on the
    // LFE (tau 250 ms) with -12 dBFS noise bursts on FL FR BL BR (tau 80 ms).
    // Measured: the maximizer limiter's gain reduction per block (the
    // broadband limiter that ducks the dialogue) and the output's 1 kHz band.
    const int n = samplesOf (6.0);
    Channels c (8);
    for (int ch = 0; ch < 8; ++ch)
        c[static_cast<size_t> (ch)] = ch == 3 ? std::vector<float> (static_cast<size_t> (n), 0.0f)
                                              : pinkNoise (n, dbToGain (ch == 2 ? -26.0f : -30.0f), static_cast<uint32_t> (7000 + ch));
    FastRandom rng (4242);
    for (int k = 0; k < 4; ++k)
    {
        const int onset = samplesOf (0.5 + 1.5 * k);
        for (int i = 0; i < samplesOf (0.8); ++i)
        {
            const double t = i / kFs;
            c[3][static_cast<size_t> (onset + i)] += static_cast<float> (0.7 * std::exp (-t / 0.25) * std::sin (kTwoPi * (35.0 + 25.0 * std::exp (-t / 0.05)) * t));
            const float burst = static_cast<float> (0.25 * std::exp (-t / 0.08)) * rng.nextBipolar();
            for (int ch : { 0, 1, 4, 5 })
                c[static_cast<size_t> (ch)][static_cast<size_t> (onset + i)] += burst;
        }
    }
    const auto input = fileOf (std::move (c));
    struct Result
    {
        double overOneDbPercent, deepestDb;
    };
    auto measure = [&] (float lfeDb) {
        auto values = resolve (RenderOptions {});
        setValue (values, VirtualizerOn, 0.0f);
        setValue (values, VirtInputMode, static_cast<float> (InputModeValue::ForceSurround));
        setValue (values, VirtLfeGainDb, lfeDb);
        ParameterStore store;
        for (int id = 0; id < kNumParams; ++id)
            store.set (id, values[static_cast<size_t> (id)]);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, 480, 8 });
        Channels io = input.channels;
        std::vector<float*> ptrs;
        for (auto& ch : io)
            ptrs.push_back (ch.data());
        Result r { 0.0, 0.0 };
        int blocks = 0, over = 0;
        for (int pos = 0; pos + 480 <= n; pos += 480)
        {
            chain.process (AudioBlock (ptrs.data(), 8, 480, pos));
            const double gr = chain.meters().maxGainReductionDb.load();
            r.deepestDb = std::min (r.deepestDb, gr);
            over += gr < -1.0 ? 1 : 0;
            ++blocks;
        }
        r.overOneDbPercent = 100.0 * over / blocks;
        return r;
    };
    const Result six = measure (6.0f), ten = measure (10.0f);
    measured ("limiter time over 1 dB, LFE +6 dB", six.overOneDbPercent, "%");
    measured ("limiter time over 1 dB, LFE +10 dB (default)", ten.overOneDbPercent, "%");
    measured ("deepest limiter GR, LFE +6 dB", six.deepestDb, "dB");
    measured ("deepest limiter GR, LFE +10 dB", ten.deepestDb, "dB");
    // +10 dB alone (before the fold armed the LF-first limiter; the same
    // scene through flubsound-cli) held the limiter over 1 dB 63.5 % of the
    // time, 6.3 dB deep, against 7.3 % / 3.2 dB at +6 dB: the dialogue
    // ducked under every explosion. With the LF-first limiter above +6 dB
    // it is back to the +6 dB figures.
    CHECK_LE (ten.overOneDbPercent, six.overOneDbPercent + 2.0);
    CHECK_GE (ten.deepestDb, six.deepestDb - 0.5);
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
    // notch at 1418 Hz, +2.6 dB at 250 Hz and 10.8 dB separation). Since the
    // virtualiser's level match (docs/11 E28a) the whole curve sits 2.9 dB
    // lower, at the downmix's loudness: -19.70 dB and +0.48 dB.
    CHECK_NEAR (before.minGain, -19.70, 0.5);
    CHECK_NEAR (before.maxGain, 0.48, 0.3);
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
    // Re-based by docs/11 E04 step 4 (Footsteps also offsets the Clarity
    // shaper's high band, +2 dB at 100 %: the steps' onsets above 4 kHz get
    // it) 5.34 / 5.63 / 5.67 -> 5.85 / 5.82 / 5.71 dB, steady unchanged.
    CHECK_NEAR (r.burstLiftDb[0], 5.85, 0.3);
    CHECK_NEAR (r.burstLiftDb[1], 5.82, 0.3);
    CHECK_NEAR (r.burstLiftDb[2], 5.67, 0.3);
    CHECK_NEAR (r.steadyLiftDb, 5.68, 0.3);
}

namespace
{
// "KnownGap closed: step/bed contrast" (E19): makeBurstScene() (steps 6 dB
// under a pink bed) at -14 / -24 / -40 LUFS, plus -50 LUFS (docs/11 E19's
// quiet-material case). Metrics (mid channel):
//   burst lift  = step-only power in the 3.2 kHz band (burst windows minus
//                 the bed), out vs in, per burst length;
//   steady lift = the same over the last 500 ms of a 1 s burst;
//   bed lift    = full-band power 200..400 ms after each burst onset;
//   contrast change = (step / bed in the band) out minus in.
// The reference is the same scene through a static +7 dB bell at 3.2 kHz,
// Q 0.9 (band 4's shape as a user EQ band, every other module off): a
// static EQ moves the contrast only by +0.77 dB, because the steps sit
// closer to its centre than the pink bed does.
// One case per level for Footsteps 100 (with the bell) and one per level for
// Competitive FPS (registerStepBedContrastCases), so each stays under 2 s.
constexpr int kStepBedLevels = 4;
constexpr double kStepBedLufs[kStepBedLevels] = { -14.0, -24.0, -40.0, -50.0 };

std::string stepBedAt (int l)
{
    return " at " + std::to_string (static_cast<int> (kStepBedLufs[l])) + " LUFS: ";
}

/** Footsteps 100 at level `l`, rendered (and printed) once: the other
    levels' cases compare their lift with the -24 LUFS one. */
const BurstResult& stepBedFootsteps (int l)
{
    static std::array<std::optional<BurstResult>, kStepBedLevels> memo;
    auto& slot = memo[static_cast<size_t> (l)];
    if (! slot)
    {
        RenderOptions footsteps;
        footsteps.mode = ModeValue::Gaming;
        footsteps.macros.push_back ({ "footsteps", 100.0f });
        const auto r = measureBursts (makeBurstScene (kStepBedLufs[l]), resolve (footsteps));
        const std::string at = stepBedAt (l);
        measured ("Footsteps 100 step lift" + at + "20 ms", r.burstLiftDb[0], "dB");
        measured ("Footsteps 100 step lift" + at + "40 ms", r.burstLiftDb[1], "dB");
        measured ("Footsteps 100 step lift" + at + "80 ms", r.burstLiftDb[2], "dB");
        measured ("Footsteps 100 step lift" + at + "steady", r.steadyLiftDb, "dB");
        measured ("Footsteps 100 bed lift" + at.substr (0, at.size() - 2), r.bedLiftDb, "dB");
        measured ("Footsteps 100 contrast change" + at + "40 ms", r.contrastChangeDb[1], "dB");
        slot = r;
    }
    return *slot;
}

void checkStepBedFootsteps (int l)
{
    auto bellValues = resolve (RenderOptions {});
    onlyModules (bellValues, { EqOn });
    setValue (bellValues, eq (0, EqFieldOn), 1.0f);
    setValue (bellValues, eq (0, EqFieldType), 0.0f); // EqBandType::Bell
    setValue (bellValues, eq (0, EqFieldFreq), 3200.0f);
    setValue (bellValues, eq (0, EqFieldGain), 7.0f);
    setValue (bellValues, eq (0, EqFieldQ), 0.9f);
    // Indexed as in the original single case: this level's fs[l] and bell[l],
    // and fs[1] (-24 LUFS) for the cross-level check.
    BurstResult fs[kStepBedLevels] {}, bell[kStepBedLevels] {};
    fs[l] = stepBedFootsteps (l);
    fs[1] = stepBedFootsteps (1);
    bell[l] = measureBursts (makeBurstScene (kStepBedLufs[l]), bellValues);
    measured ("static +7 dB bell contrast change" + stepBedAt (l) + "40 ms", bell[l].contrastChangeDb[1], "dB");

    // Footsteps 100. Before the E19 redesign (the interim static bell): lift
    // 20 / 40 / 80 ms / steady 3.81 / 3.18 / 2.57 / 2.30 dB at -14 LUFS (the
    // steps' peaks reached its loud roll-off) and 5.76 / 5.77 / 5.82 /
    // 5.70 dB below; contrast change at 40 ms -1.44 dB at -14 LUFS, +0.76 dB
    // (the static bell's own) below. After it (CueLift): 5.24 / 6.03 /
    // 6.53 dB at every level; a 1 s steady burst becomes background after
    // about 0.5 s (1.00 dB over its last 500 ms); contrast change +5.99 dB;
    // the bed -1.13 dB (the default 20 Hz subsonic filter on the generator's
    // infrasonic pink; the cue bands add nothing).
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

void checkStepBedCompetitiveFps (int l)
{
    BurstResult fps[kStepBedLevels] {}; // indexed as in the original single case
    fps[l] = measureBursts (makeBurstScene (kStepBedLufs[l]), resolve (factoryPreset ("gaming-competitive-fps.json")));
    const std::string at = stepBedAt (l);
    measured ("Competitive FPS bed lift" + at.substr (0, at.size() - 2), fps[l].bedLiftDb, "dB");
    measured ("Competitive FPS contrast change" + at + "20 ms", fps[l].contrastChangeDb[0], "dB");
    measured ("Competitive FPS contrast change" + at + "40 ms", fps[l].contrastChangeDb[1], "dB");
    measured ("Competitive FPS contrast change" + at + "80 ms", fps[l].contrastChangeDb[2], "dB");

    // Competitive FPS. Before the redesign: bed -0.06 / 0.66 / 1.66 / 4.37 dB;
    // contrast change 20 / 40 / 80 ms 0.05 / -0.53 / -1.11, 0.83 / 0.70 /
    // 0.52, 0.82 / 0.75 / 0.70 and -0.89 / -1.66 / -1.67 dB (docs/11 E19
    // measured +10.9 dB and -6.0 dB on its own stimulus). At -50 LUFS the
    // Detail upward compressor lifted the bed until its floor followed the
    // background (E19 step 2): bed 0.69 -> -0.93 dB, contrast 4.55 / 4.72 /
    // 5.21 -> 4.63 / 5.35 / 5.81 dB.
    const double fpsBed[kStepBedLevels] = { -1.86, -1.30, -0.94, -0.93 };
    const double fpsContrast[kStepBedLevels][3] = { { 4.39, 4.93, 5.17 }, { 4.49, 5.04, 5.26 }, { 4.58, 5.21, 5.67 }, { 4.63, 5.35, 5.81 } }; // 20 / 40 / 80 ms
    // docs/11 E19 Done-when: Competitive FPS bed <= +1 dB and step/bed contrast change >= +3 dB.
    CHECK_LE (fps[l].bedLiftDb, 1.0);
    CHECK_NEAR (fps[l].bedLiftDb, fpsBed[l], 0.3);
    for (int d = 0; d < 3; ++d)
    {
        CHECK_GE (fps[l].contrastChangeDb[d], 3.0);
        CHECK_NEAR (fps[l].contrastChangeDb[d], fpsContrast[l][d], 0.3);
    }
}

bool registerStepBedContrastCases()
{
    const std::string base = "KnownGap closed: step/bed contrast - the Footsteps 100 cue enhancer lifts 20-80 ms steps under a bed by the same law at -14..-50 LUFS and raises their contrast; Competitive FPS keeps the bed within +1 dB (E19)";
    for (int l = 0; l < kStepBedLevels; ++l)
    {
        const std::string level = std::to_string (static_cast<int> (kStepBedLufs[l])) + " LUFS";
        ::flubtest::Registrar ((base + " - Footsteps 100 and the static bell at " + level).c_str(), [l] { checkStepBedFootsteps (l); }, __FILE__, __LINE__);
    }
    for (int l = 0; l < kStepBedLevels; ++l)
    {
        const std::string level = std::to_string (static_cast<int> (kStepBedLufs[l])) + " LUFS";
        ::flubtest::Registrar ((base + " - Competitive FPS at " + level).c_str(), [l] { checkStepBedCompetitiveFps (l); }, __FILE__, __LINE__);
    }
    return true;
}

[[maybe_unused]] const bool kStepBedContrastCasesRegistered = registerStepBedContrastCases();
} // namespace

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
    } presets[] = { { "gaming-competitive-fps.json", -0.18 }, { "gaming-battle-royale.json", 0.32 }, { "gaming-night-mode.json", -0.44 } };
    // Re-based by docs/11 E04 step 4, +0.22 / +0.25 / +0.05 -> -0.18 / +0.32
    // / -0.44 dB: Footsteps also offsets the shaper's high band, which lifts
    // the shots' onsets above 4 kHz, and the limiter (in Night Mode also the
    // Startle Guard) then holds the shipped shots a little lower in this
    // 3.2 kHz band.
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

namespace
{
// The Night Mode ambush scene (docs/11 E21): 12 s of -50 dBFS-RMS pink
// ambience, then `fireSeconds` of automatic fire (10 shots/s; each seeded
// white noise, tau 15 ms, peak -12 dBFS) over the ambience, then 8 s of
// ambience alone. Levels are full-band power of both channels, out vs in.
// This is the re-baselined, pinned scene docs/11 E21 asks for (its +16.6 dB
// lift and 6.6 dB hole predate d096a4d and used another event).
io::AudioFileData nightAmbushScene (double fireSeconds)
{
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
}

double nightAmbushLiftDb (const io::AudioFileData& input, const Channels& out, double from, double to)
{
    const std::vector<Window> w { { samplesOf (from), samplesOf (to) } };
    return powerDb (meanPower (out, w)) - powerDb (meanPower (input.channels, w));
}

// Night Mode's bed lift before the event as shipped (pinned below): the
// Auto Level case compares with it without rendering it again.
constexpr double kNightBedLiftDb = 5.10;
} // namespace

TEST_CASE ("KnownGap closed: Night Mode ambush - no hole after the event, the bed lifted <= +6 dB and the fire held by the Startle Guard (E21)")
{
    // 3 s of fire; bed before (8..12 s), event (12..15 s), bed 1..2 s after
    // the event (16..17 s) and 5..7.5 s after it (20..22.5 s).
    const auto input = nightAmbushScene (3.0);
    const auto out = render (input, resolve (factoryPreset ("gaming-night-mode.json")));
    const double before = nightAmbushLiftDb (input, out, 8.0, 12.0), event = nightAmbushLiftDb (input, out, 12.0, 15.0);
    const double after1 = nightAmbushLiftDb (input, out, 16.0, 17.0), after5 = nightAmbushLiftDb (input, out, 20.0, 22.5);
    measured ("Night Mode bed lift before the event", before, "dB");
    measured ("Night Mode event change", event, "dB");
    measured ("Night Mode bed lift 1-2 s after", after1, "dB");
    measured ("Night Mode bed lift 5-7.5 s after", after5, "dB");
    measured ("Night Mode hole 1-2 s after", before - after1, "dB");
    measured ("Night Mode hole 5-7.5 s after", before - after5, "dB");

    // E21 slice (AutoLevel: upper gate, +6 dB cap, 3 dB/s recovery). Before
    // it: bed before 15.77 dB, hole 5.23 dB (1-2 s) / 2.36 dB (5-7.5 s),
    // event 1.76 dB; 10 s event: hole 6.89 / 4.01 dB. The gunfire is held out
    // of Auto Level's measure, so its gain stays at +6 dB through the event
    // and there is no hole; the bed lift fell by 3.8 dB (Auto Level +12 ->
    // +6 dB, partly made up by the upward compressor on a quieter bed).
    CHECK_LE (std::abs (before - after1), 1.0); // docs/11 E21 Done-when: within 1 dB 1 s after the event
    CHECK_LE (std::abs (before - after5), 1.0);
    CHECK_NEAR (before - after1, -0.05, 0.3);
    // Closed by the E21 Phase 3 retune: docs/11 E21 Done-when, ambience lift
    // <= +6 dB. What was left over Auto Level's +6 dB was the preset's own
    // (5.10 dB with Auto Level off, mostly the compressor's 6 dB make-up;
    // the E19 redesign and its step 2 had already taken the Footsteps
    // bell's and the upward compressor's shares out: 11.98 / 6.98 -> 11.13 /
    // 5.10 dB). The make-up moved in front of the compressor as Auto Level's
    // target (-20 -> -14 LUFS, the compressor's, the upward section's and
    // dyneq.0's thresholds up 6 dB with it), so loud programme meets the
    // same compression and a quiet bed is lifted by Auto Level's cap alone:
    // 11.10 -> 5.10 dB (the next case: -0.90 dB with Auto Level off, the
    // -3 dB shelf at 90 Hz). The Startle Guard (guard.range 20 LU) holds the
    // fire, about 30 LU over the bed, to 20 LU over it: event change
    // +1.18 -> -10.31 dB. Re-based by docs/11 E04 step 4, -10.31 -> -11.10
    // dB (Footsteps and Detail offset the shaper's high band: the fire's HF
    // onsets get it and the guard holds them to the same 20 LU).
    CHECK_LE (before, 6.0);
    CHECK_NEAR (before, kNightBedLiftDb, 0.3);
    CHECK_NEAR (event, -11.10, 0.3);
}

TEST_CASE ("KnownGap closed: Night Mode ambush - with Auto Level off the preset itself leaves the bed where it was, so the lift is Auto Level's +6 dB cap alone (E21)")
{
    // The scene of the case above, Auto Level off: the bed lift before the
    // event (8..12 s) is the preset's own (split out of that case to keep
    // each case under 2 s).
    const auto input = nightAmbushScene (3.0);
    auto noAutoLevel = resolve (factoryPreset ("gaming-night-mode.json"));
    setValue (noAutoLevel, AutoLevelOn, 0.0f);
    const double staticLift = nightAmbushLiftDb (input, render (input, noAutoLevel), 8.0, 12.0);
    measured ("Night Mode bed lift with Auto Level off", staticLift, "dB");
    // Auto Level's own share of the bed lift is at its +6 dB cap.
    CHECK_LE (kNightBedLiftDb - staticLift, AutoLevel::kMaxGainDb + 0.1);
    CHECK_NEAR (staticLift, -0.90, 0.3);
}

TEST_CASE ("KnownGap: Night Mode ambush, 10 s of fire - no hole after it at any block size (E21; no Done-when for long events)")
{
    const auto longInput = nightAmbushScene (10.0);
    const auto longOut = render (longInput, resolve (factoryPreset ("gaming-night-mode.json")));
    const double longHole1 = nightAmbushLiftDb (longInput, longOut, 8.0, 12.0) - nightAmbushLiftDb (longInput, longOut, 23.0, 24.0);
    const double longHole5 = nightAmbushLiftDb (longInput, longOut, 8.0, 12.0) - nightAmbushLiftDb (longInput, longOut, 27.0, 29.5);
    measured ("Night Mode hole 1-2 s after a 10 s event", longHole1, "dB");
    measured ("Night Mode hole 5-7.5 s after a 10 s event", longHole5, "dB");
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

TEST_CASE ("KnownGap closed: kick onset - Punch 100 lifts the kick's first 10 ms >= 2 dB more than its body (E04 step 4); Tighten no longer cuts the onset, Boost 80 and 100 keep it over the body (E04 / E05)")
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
    auto plainValues = resolve (RenderOptions {});
    setValue (plainValues, MaximizerOn, 0.0f);
    auto tightenValues = plainValues;
    setValue (tightenValues, BassTighten, 0.5f);
    const auto boostValues = resolve (boosted (ModeValue::Music, 100.0f)); // maximizer on (Boost engages it)
    const auto boost80Values = resolve (boosted (ModeValue::Music, 80.0f));

    double p0 = 0, p1 = 0, p2 = 0, t0 = 0, t1 = 0, t2 = 0, n0 = 0, n1 = 0, n2 = 0, b0 = 0, b1 = 0, b2 = 0, c0 = 0, c1 = 0, c2 = 0;
    lifts (punchValues, p0, p1, p2);
    lifts (tightenValues, t0, t1, t2);
    lifts (plainValues, n0, n1, n2);
    lifts (boostValues, b0, b1, b2);
    lifts (boost80Values, c0, c1, c2);
    measured ("Punch 100 lift 0-10 ms", p0, "dB");
    measured ("Punch 100 lift 10-30 ms", p1, "dB");
    measured ("Punch 100 lift 40-60 ms", p2, "dB");
    measured ("Tighten 0.5 lift 0-10 ms", t0, "dB");
    measured ("Tighten 0.5 lift 10-30 ms", t1, "dB");
    measured ("Tighten 0.5 change re Tighten 0, 0-10 ms", t0 - n0, "dB");
    measured ("Tighten 0.5 change re Tighten 0, 40-60 ms", t2 - n2, "dB");
    measured ("Boost 100 onset (0-10) minus body (10-30)", b0 - b1, "dB");
    measured ("Boost 80 onset (0-10) minus body (10-30)", c0 - c1, "dB");

    // docs/11 E04 Done-when, Punch 100: 0-10 ms lift >= 10-30 ms lift + 2 dB.
    // Step (1) took BassTighten out of the Punch macro: 0-10 / 10-30 ms
    // 3.92 / 5.47 -> 5.38 / 4.70 dB, onset minus body -1.55 -> +0.68 dB; the
    // rest was the full-band shaper's own smear. Step 4: Punch also offsets
    // the shaper's high band (clarity.attackHigh +2.5 dB), which runs the
    // 3-band path: the kick is in the low band, whose timing (10 ms slow
    // attack, fast release) keeps the lift on the onset, and whose gain is
    // applied to a one-pole low band of the signal (the step 3 path's LR4
    // band sum was an all-pass that took 2.4 dB off the kick's first 10 ms
    // at any setting), read by an LR2 detector: 5.38 / 4.70 -> 5.43 / 2.52
    // dB, onset minus body +0.68 -> +2.91 dB.
    CHECK_NEAR (p0, 5.43, 0.3);
    CHECK_NEAR (p1, 2.52, 0.3);
    CHECK_NEAR (p0 - p1, 2.91, 0.3);
    CHECK_GE (p0 - p1, 2.0);
    // docs/11 E04 Done-when, Tighten 0.5: 0-10 ms change >= -0.5 dB - met by
    // step 2: the lift -2.04 -> -0.55 dB, i.e. -1.69 -> -0.20 dB re Tighten 0
    // (whose chain, the 20 Hz subsonic filter, reads -0.35 dB). Two causes:
    // the sustain cut still read the previous kick's decay through the new
    // one's first 2-3 ms (now gated by the onset, and a cut the gate lifts
    // returns in 0.5 ms), and the output was the 150 Hz LR4 split's band
    // sum, an all-pass whose ~3 ms of group delay under 100 Hz took 1.5 dB
    // off the first 10 ms at any Tighten > 0 (now x + (g - 1) LP1 (x):
    // exactly x at unity gain). The tail is still cut (60-150 / 150-300 ms
    // -1.79 / -4.98 -> -1.65 / -5.11 dB re Tighten 0, CLI).
    CHECK_NEAR (t0, -0.55, 0.3);
    CHECK_GE (t0 - n0, -0.5);
    CHECK_NEAR (t1 - n1, 0.0, 0.3);
    // docs/11 E05 Done-when, Boost 100 kick onset / body >= 0 dB: stage 1
    // (crest-gated clipper, LF-safe limiter envelope) -1.80 -> -1.18 dB;
    // met by step 5, Boost's LF-first limiter (max.lfLimit from Boost 50 %):
    // the kick is limited in the low band 3 dB under the ceiling, so the
    // wideband limiter and the clipper no longer take its onset: +0.21 dB.
    // Step 6 (the LF-first limiter full from 75 %, and Boost's transient
    // coupling): Boost 100 +0.21 -> +0.30 dB, Boost 80 -0.89 -> +0.23 dB.
    CHECK_NEAR (b0 - b1, 0.30, 0.3);
    CHECK_GE (b0 - b1, 0.0);
    CHECK_NEAR (c0 - c1, 0.23, 0.3);
    CHECK_GE (c0 - c1, 0.0);
}

TEST_CASE ("KnownGap closed: an HF step under an explosion's tail keeps its isolated lift within 1 dB at Gaming Boost 100 + Impact 100 (E04 step 4)")
{
    // docs/11 E04 Done-when. An explosion (a 45 Hz sine and noise under
    // 250 Hz, 0.6 / 0.4, peak -12 dBFS, 2 ms rise, 400 ms decay) from 0.5 s
    // and a step 150 ms into its tail: 20 ms of noise above 3 kHz, peak
    // -36 dBFS, 1 ms rise, 6 ms decay. Lift = the power above 3 kHz (four
    // one-pole high-passes) over the step (2 ms before it to its end), the
    // output's against the input's: the step alone, and under the tail as
    // (step + explosion) out minus explosion out. Before step 4 Impact's
    // attack went to the full-band shaper, whose detector the explosion's
    // tail holds up, so the step got the attack only in isolation: 11.27 /
    // 6.05 dB, -5.22 dB (Boost 100 alone, the full-band shaper: 7.74 / 6.22,
    // -1.52 dB). Impact's attack now goes to the low band and runs the
    // 3-band path; the high band reads the step on its own: 7.67 / 7.18 dB,
    // -0.49 dB (the maximizer's gain reduction on the tail is the rest).
    const int n = samplesOf (2.0), onset = samplesOf (0.5), at = samplesOf (0.65), len = samplesOf (0.02);
    const auto onePoles = [] (std::vector<float> x, double hz, bool highPass) {
        const double g = std::tan (kPi * hz / kFs), G = g / (1.0 + g);
        for (int stage = 0; stage < 4; ++stage)
        {
            double z = 0.0;
            for (auto& v : x)
            {
                const double w = (v - z) * G, lp = w + z;
                z = lp + w;
                v = static_cast<float> (highPass ? v - lp : lp);
            }
        }
        return x;
    };
    const auto normalised = [] (std::vector<float> x, int from, int to) {
        double acc = 0.0;
        for (int i = from; i < to; ++i)
            acc += static_cast<double> (x[static_cast<size_t> (i)]) * x[static_cast<size_t> (i)];
        const auto g = static_cast<float> (1.0 / std::sqrt (std::max (1.0e-30, acc / (to - from))));
        for (auto& v : x)
            v *= g;
        return x;
    };
    const auto noise = normalised (onePoles (whiteNoise (n, 1.0f, 451), 250.0, false), onset, n);
    const auto click = normalised (onePoles (whiteNoise (n, 1.0f, 452), 3000.0, true), at, at + len);
    std::vector<float> explosion (static_cast<size_t> (n), 0.0f), step (static_cast<size_t> (n), 0.0f);
    double explosionPeak = 0.0, stepPeak = 0.0;
    for (int i = onset; i < n; ++i)
    {
        const double t = (i - onset) / kFs;
        explosion[static_cast<size_t> (i)] = static_cast<float> (std::min (1.0, t / 0.002) * std::exp (-t / 0.4)
                                                                 * (0.6 * std::sin (kTwoPi * 45.0 * t) + 0.4 * noise[static_cast<size_t> (i)]));
        explosionPeak = std::max (explosionPeak, static_cast<double> (std::abs (explosion[static_cast<size_t> (i)])));
    }
    for (int i = 0; i < len; ++i)
    {
        step[static_cast<size_t> (at + i)] = static_cast<float> (std::min (1.0, i / (0.001 * kFs)) * std::exp (-i / (0.006 * kFs)) * click[static_cast<size_t> (at + i)]);
        stepPeak = std::max (stepPeak, static_cast<double> (std::abs (step[static_cast<size_t> (at + i)])));
    }
    std::vector<float> both (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        explosion[static_cast<size_t> (i)] *= static_cast<float> (std::pow (10.0, -12.0 / 20.0) / explosionPeak);
        step[static_cast<size_t> (i)] *= static_cast<float> (std::pow (10.0, -36.0 / 20.0) / stepPeak);
        both[static_cast<size_t> (i)] = explosion[static_cast<size_t> (i)] + step[static_cast<size_t> (i)];
    }
    const std::vector<Window> window { { at - samplesOf (0.002), at + len } };
    const auto hfDb = [&] (const std::vector<float>& x) { return powerDb (meanPower (onePoles (x, 3000.0, true), window)); };
    const double stepDb = hfDb (step);
    const auto lifts = [&] (const std::vector<float>& values, double& isolated, double& underTail) {
        const auto s = render (stereoOf (step), values), b = render (stereoOf (both), values), e = render (stereoOf (explosion), values);
        std::vector<float> diff (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
            diff[static_cast<size_t> (i)] = b[0][static_cast<size_t> (i)] - e[0][static_cast<size_t> (i)];
        isolated = hfDb (s[0]) - stepDb;
        underTail = hfDb (diff) - stepDb;
    };
    RenderOptions impact = boosted (ModeValue::Gaming, 100.0f);
    impact.macros.push_back ({ "impact", 100.0f });
    double isolated = 0.0, underTail = 0.0, boostIsolated = 0.0, boostUnderTail = 0.0;
    lifts (resolve (impact), isolated, underTail);
    lifts (resolve (boosted (ModeValue::Gaming, 100.0f)), boostIsolated, boostUnderTail);
    measured ("Gaming Boost 100 + Impact 100: HF step lift, isolated", isolated, "dB");
    measured ("Gaming Boost 100 + Impact 100: HF step lift, 150 ms into an explosion's tail", underTail, "dB");
    measured ("Gaming Boost 100 alone: HF step lift, isolated", boostIsolated, "dB");
    measured ("Gaming Boost 100 alone: HF step lift, under the tail", boostUnderTail, "dB");
    CHECK_LE (std::abs (underTail - isolated), 1.0);
    CHECK_NEAR (isolated, 7.67, 0.3);
    CHECK_NEAR (underTail - isolated, -0.49, 0.3);
}

namespace
{
/** 55 Hz kicks (peak 0.5, tau 100 ms, 350 ms) every 0.5 + 1/128 s under a
    32 Hz line: the line turns a quarter cycle per kick period, so averaging
    four periods cancels the kicks' own 32 Hz content (lineGainSpreadDb). */
constexpr double kLineKickPeriod = 0.5 + 1.0 / 128.0;

std::vector<float> lineUnderKicks (double lineAmp, int n)
{
    std::vector<float> x (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kFs, beat = std::fmod (t, kLineKickPeriod);
        const double kick = beat < 0.35 ? 0.5 * std::exp (-beat / 0.1) * std::sin (kTwoPi * 55.0 * beat) : 0.0;
        x[static_cast<size_t> (i)] = static_cast<float> (lineAmp * std::sin (kTwoPi * 32.0 * t) + kick);
    }
    return x;
}

/** The 32 Hz line's level over one kick period (every 0.5 ms, dB): demodulated
    at 32 Hz, averaged over one line cycle and four kick periods from `from`. */
std::vector<double> lineLevelTrack (const std::vector<float>& y, double from)
{
    const int period = static_cast<int> (std::lround (kLineKickPeriod * kFs)), cycle = static_cast<int> (std::lround (kFs / 32.0));
    const int s0 = samplesOf (from);
    std::vector<double> re (y.size()), im (y.size());
    double accRe = 0.0, accIm = 0.0;
    for (size_t i = 0; i < y.size(); ++i)
    {
        const double a = kTwoPi * 32.0 * static_cast<double> (i) / kFs;
        accRe += y[i] * std::cos (a);
        accIm -= y[i] * std::sin (a);
        if (i >= static_cast<size_t> (cycle))
        {
            const size_t j = i - static_cast<size_t> (cycle);
            const double b = kTwoPi * 32.0 * static_cast<double> (j) / kFs;
            accRe -= y[j] * std::cos (b);
            accIm += y[j] * std::sin (b);
        }
        re[i] = accRe;
        im[i] = accIm;
    }
    std::vector<double> track;
    for (int tau = 0; tau < period; tau += 24)
    {
        double r = 0.0, q = 0.0;
        for (int k = 0; k < 4; ++k)
        {
            r += re[static_cast<size_t> (s0 + k * period + tau)];
            q += im[static_cast<size_t> (s0 + k * period + tau)];
        }
        track.push_back (20.0 * std::log10 (std::max (1.0e-12, std::hypot (r, q))));
    }
    return track;
}

/** Peak-to-peak (dB) of the line's gain (out track - in track) over the kick period. */
double lineGainSpreadDb (const std::vector<float>& in, const std::vector<float>& out)
{
    const auto a = lineLevelTrack (in, 3.5), b = lineLevelTrack (out, 3.5);
    double lo = 1.0e9, hi = -1.0e9;
    for (size_t k = 0; k < a.size(); ++k)
    {
        lo = std::min (lo, b[k] - a[k]);
        hi = std::max (hi, b[k] - a[k]);
    }
    return hi - lo;
}

/** Complex gain of the rendered left channel at f over 2..3 s for a sine at `amp`. */
std::pair<double, double> toneResponse (const std::vector<float>& values, double f, double amp)
{
    const auto x = sine (f, kFs, samplesOf (3.0), static_cast<float> (amp));
    const auto y = render (stereoOf (x), values)[0];
    double yr = 0.0, yi = 0.0, xr = 0.0, xi = 0.0;
    for (int i = samplesOf (2.0); i < samplesOf (3.0); ++i)
    {
        const double c = std::cos (kTwoPi * f * i / kFs), s = std::sin (kTwoPi * f * i / kFs);
        yr += y[static_cast<size_t> (i)] * c;
        yi -= y[static_cast<size_t> (i)] * s;
        xr += x[static_cast<size_t> (i)] * c;
        xi -= x[static_cast<size_t> (i)] * s;
    }
    return { 20.0 * std::log10 (std::hypot (yr, yi) / std::hypot (xr, xi)), std::atan2 (yi, yr) - std::atan2 (xi, xr) };
}
} // namespace

TEST_CASE ("KnownGap: bass-line pumping - a 32 Hz line under 55 Hz kicks through Bass Head moves with the kicks; split-band protection holds it (E02 (a))")
{
    // A 32 Hz line at -18 dBFS under 55 Hz kicks at -6 dBFS (~500 ms):
    // the peak-to-peak of the line's gain over the kick period, Bass Head as
    // shipped and with bass.splitProtect (docs/11 E02 (a), off by default:
    // turning it on in a factory preset is a voicing decision).
    const int n = samplesOf (5.8);
    const auto x = lineUnderKicks (0.125, n);
    auto shipped = resolve (factoryPreset ("music-bass-head.json"));
    auto split = shipped;
    setValue (split, BassSplitProtect, 1.0f);
    const double asShipped = lineGainSpreadDb (x, render (stereoOf (x), shipped)[0]);
    const double withSplit = lineGainSpreadDb (x, render (stereoOf (x), split)[0]);
    measured ("Bass Head 32 Hz line modulation under 55 Hz kicks, as shipped", asShipped, "dB");
    measured ("Bass Head 32 Hz line modulation under 55 Hz kicks, bass.splitProtect on", withSplit, "dB");
    // KNOWN_GAP: target <= 1 dB per docs/11 E02 (Bass Head settings). The
    // bass engine's protection alone goes 3.7 -> 0.0 dB with the split
    // (test_bass_engine.cpp) and Bass Head's whole engine 3.4 -> 1.8 dB; what
    // is left comes from Tighten (E04) and the harmonics generator (E03),
    // about 1 dB each, and from the chain after them (Punch's transient
    // shaper, the glue and the maximizer at Boost 45 %). Re-based by docs/11
    // E04 step 4 (Bass Head's Punch 0.25 runs the shaper's 3-band path,
    // whose low band lifts the kicks' onsets, not the line): with the split
    // 2.95 -> 2.26 dB, as shipped 5.07 -> 5.02 dB.
    CHECK_NEAR (asShipped, 5.02, 0.3);
    CHECK_NEAR (withSplit, 2.26, 0.3);
    CHECK_LE (withSplit, asShipped - 1.0);
}

TEST_CASE ("KnownGap: subsonic slice - Bass Head's 40 Hz group delay, and the gaming presets lose <= 3 dB at 28 Hz to their subsonic filter (E02)")
{
    // Group delay at 40 Hz from the phase of 39 / 41 Hz sines at -40 dBFS
    // (rendered latency-compensated, so the chain's own latency is not in
    // it). Before the slice Bass Head ran a 4th-order subsonic at 25 Hz:
    // 18.68 ms; with the 2nd order at the 20 Hz default 13.82 ms. KNOWN_GAP:
    // target <= 11 ms per docs/11 E02: the rest is the mono-bass LR4 at
    // 110 Hz (4.6 ms) and the glue split's LR4 at 120 Hz (Boost 45 %), which
    // only a shared crossover (E02 (b)) or arming the split with glue alone
    // (E05's open decision) would remove.
    const auto head = resolve (factoryPreset ("music-bass-head.json"));
    const auto [g39, p39] = toneResponse (head, 39.0, 0.01);
    const auto [g41, p41] = toneResponse (head, 41.0, 0.01);
    const double gd = -std::remainder (p41 - p39, kTwoPi) / (kTwoPi * 2.0) * 1000.0;
    measured ("Bass Head 40 Hz group delay", gd, "ms");
    CHECK_NEAR (gd, 13.82, 0.3);
    (void) g39;
    (void) g41;

    // 28 Hz: what the subsonic filter itself costs (the preset against the
    // same preset with bass.subsonic 0; their EQ is not the filter's).
    // Closed by the slice: 4th order at 28 / 30 Hz -3.0 / -4.4 dB -> 2nd
    // order at 20 Hz -1.0 dB (docs/11 E02: within 3 dB).
    for (const char* file : { "gaming-battle-royale.json", "gaming-competitive-fps.json" })
    {
        const auto preset = resolve (factoryPreset (file));
        auto noSubsonic = preset;
        setValue (noSubsonic, BassSubsonic, 0.0f);
        const double cost = toneResponse (preset, 28.0, 0.01).first - toneResponse (noSubsonic, 28.0, 0.01).first;
        measured (std::string (file) + " subsonic cost at 28 Hz", cost, "dB");
        CHECK_GE (cost, -3.0);
        CHECK_NEAR (cost, -1.0, 0.3);
    }
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

// docs/11 E59 Done-when: each metric reports the injected value within
// +-10 % (of the power ratio for dB metrics: +-0.41 dB). The suite is
// `flubsound-cli quality` (Commands.h measureQuality); every module is
// off, so the chain passes the stimuli through and only the injected
// artefact shows. One case per quality-suite run, so each stays under 2 s.
namespace
{
std::vector<float> passThroughValues()
{
    auto values = resolve (RenderOptions {});
    onlyModules (values, {});
    return values;
}
} // namespace

TEST_CASE ("KnownGap metrics: the quality suite reads a 1 % cubic, a 6 dB 2 Hz square gain modulation and a 3 ms delay injected into a pass-through render at their injected values (meta-validation, docs/11 E59) - clean pass-through reads nothing")
{
    const auto values = passThroughValues();
    QualityReport clean;
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
}

TEST_CASE ("KnownGap metrics: the quality suite reads a 1 % cubic, a 6 dB 2 Hz square gain modulation and a 3 ms delay injected into a pass-through render at their injected values (meta-validation, docs/11 E59) - 1 % cubic")
{
    const auto values = passThroughValues();
    QualityReport cubic;
    std::string error;
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
}

TEST_CASE ("KnownGap metrics: the quality suite reads a 1 % cubic, a 6 dB 2 Hz square gain modulation and a 3 ms delay injected into a pass-through render at their injected values (meta-validation, docs/11 E59) - 6 dB 2 Hz square gain modulation")
{
    const auto values = passThroughValues();
    QualityReport modulated;
    std::string error;
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
}

TEST_CASE ("KnownGap metrics: the quality suite reads a 1 % cubic, a 6 dB 2 Hz square gain modulation and a 3 ms delay injected into a pass-through render at their injected values (meta-validation, docs/11 E59) - 3 ms delay, and the MTND residual")
{
    const auto values = passThroughValues();
    QualityReport delayed;
    std::string error;
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

TEST_CASE ("CLI: `quality` takes the chain options but no files, and its JSON and render.stats carry the E59 statistics")
{
    CliOptions o;
    std::string error;
    REQUIRE (parseCommandLine ({ "quality", "--mode", "music", "--boost", "100", "--set", "max.drive=6", "--profile", "quality", "--json" }, o, error));
    CHECK (o.command == Command::Quality);
    CHECK (o.json);
    CHECK (o.render.boostPercent.has_value());
    CHECK (o.render.sets.size() == 1u);
    CHECK (! parseCommandLine ({ "quality", "-i", "in.wav" }, o, error));
    CHECK (! parseCommandLine ({ "quality", "--target-lufs", "-14" }, o, error));
    CHECK (! parseCommandLine ({ "quality", "song.wav" }, o, error));

    // Every family is in the JSON; the pass-through values are exact.
    auto values = resolve (RenderOptions {});
    onlyModules (values, {});
    QualityReport q;
    REQUIRE (measureQuality (values, 512, q, error));
    const auto json = qualityToJson (q);
    REQUIRE (json["thdn"].asArray().size() == 4u);
    CHECK (json["imd"]["smpteDb"].isNumber());
    REQUIRE (json["mtnd"].asArray().size() == 3u);
    REQUIRE (json["ducking"]["probes"].asArray().size() == 4u);
    CHECK (json["ducking"]["probes"].asArray()[1]["modulationDb"].asArray().size() == 4u);
    CHECK_NEAR (json["kick"]["centroidShiftMs"].asNumber(), 0.0, 0.01);
    CHECK_NEAR (json["loudness"]["pinkOutLufs"].asNumber(), json["loudness"]["pinkInLufs"].asNumber(), 0.01);
    CHECK (formatQuality (q).find ("Ducking") != std::string::npos);

    // render.stats: the harmonics reading and the governor's state / reason shares.
    RenderResult rr;
    REQUIRE (renderFile (stereoOf (sine (50.0, kFs, samplesOf (4.0), 0.5f)), resolve (boosted (ModeValue::Music, 100.0f)), RenderSettings {}, rr, error));
    const auto st = renderStatsToJson (rr.stats);
    CHECK (st["harmonics"]["maxDb"].isNumber()); // Boost 100 raises bass.harmonics
    double shares = 0.0;
    for (const char* k : { "idle", "backingOff", "holding", "recovering" })
        shares += st["governor"]["statePercent"][k].isNumber() ? st["governor"]["statePercent"][k].asNumber() : 0.0;
    CHECK_NEAR (shares, 100.0, 0.1);
    CHECK (st["governor"]["distortionReasonPercent"].isNumber() || st["governor"]["distortionReasonPercent"].isNull());
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
    double harmonicsDb = -160.0;                // the intentional harmonic generators' share (bass harmonics, air)
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
        *reading = { chain.meters().governorScale.load(), chain.meters().governorDistortionDb.load(), chain.meters().harmonicsDb.load() };
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

// "KnownGap closed: governed macros at 64..4096-sample blocks" (E06).
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
// One case renders each block size (registerGovernedBlockCases), so each
// stays under 2 s; the spread case reads their memo (and renders whatever
// a filtered run left out).
constexpr int kGovernedBlocks[] = { 64, 256, 512, 1024, 2048, 4096 };
constexpr size_t kNumGovernedBlocks = std::size (kGovernedBlocks);

/** Integrated loudness of Boost 100 + Loudness 100 on the kick programme at
    the k-th block size, rendered (and printed) once. */
double governedLufsAt (size_t k)
{
    static std::array<std::optional<double>, kNumGovernedBlocks> memo;
    if (! memo[k])
    {
        static const auto input = kickProgramme (20.0);
        RenderOptions o = boosted (ModeValue::Music, 100.0f);
        o.macros.push_back ({ "loudness", 100.0f });
        const auto values = resolve (o);
        const int block = kGovernedBlocks[k];
        Channels out;
        int latency = 0;
        std::string error;
        REQUIRE (renderPass (input, values, block, out, latency, error));
        const double lufs = analyse (out, kFs).integratedLufs;
        measured ("Boost 100 + Loudness 100 integrated at " + std::to_string (block) + "-sample blocks", lufs, "LUFS");
        memo[k] = lufs;
    }
    return *memo[k];
}

const char* const kGovernedBlocksCase = "KnownGap closed: governed macros at 64..4096-sample blocks - Boost 100 + Loudness 100 on kick-heavy programme lands within 0.05 LU integrated (E06)";

bool registerGovernedBlockCases()
{
    for (size_t k = 0; k < kNumGovernedBlocks; ++k)
        ::flubtest::Registrar ((std::string (kGovernedBlocksCase) + " - render at " + std::to_string (kGovernedBlocks[k]) + "-sample blocks").c_str(),
                               [k] { governedLufsAt (k); }, __FILE__, __LINE__);
    ::flubtest::Registrar ((std::string (kGovernedBlocksCase) + " - spread over the block sizes").c_str(), [] {
        double lo = 1.0e9, hi = -1.0e9;
        for (size_t k = 0; k < kNumGovernedBlocks; ++k)
        {
            const double lufs = governedLufsAt (k);
            lo = std::min (lo, lufs);
            hi = std::max (hi, lufs);
        }
        measured ("Boost 100 + Loudness 100 block-size spread", hi - lo, "LU");
        CHECK_LE (hi - lo, 0.05); // docs/11 E06 Done-when
    }, __FILE__, __LINE__);
    return true;
}

[[maybe_unused]] const bool kGovernedBlockCasesRegistered = registerGovernedBlockCases();
} // namespace

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
    // Re-based by docs/11 E04 step 2, 441.0 -> 510.1 ms (the 1e30 spikes
    // 7.2 -> 3.9 ms): Signature's Tighten 0.1 is gated by the onset, which
    // reads the restarted programme as one, and is applied through a
    // one-pole shelf whose state restarts with it. Re-based by docs/11 E04
    // step 4, 510.1 -> 457.5 ms: Signature's Punch 0.2 runs the Clarity
    // shaper's 3-band path, whose shapers restart with the path too.
    CHECK_NEAR (burst.spanMs, 457.5, 20.0);
    CHECK_LE (burst.worstChangeDb, 0.3);
}

TEST_CASE ("KnownGap closed: DC after the maximizer - an asymmetric 100 + 200 Hz signal at 24 dB drive leaves <= -60 dBFS DC in every profile, with and without the clipper (E10, E05)")
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
        double chainDb, limiterOnlyDb; // measured after docs/11 E05 stage 1
    };
    // After E10 Phase 1: chain / limiter alone -73.55 / -51.42 (Quality),
    // -68.81 / -49.02 (Balanced), -58.70 / -42.22 dBFS (Low Latency).
    for (const auto& r : { Row { LatencyProfileValue::Quality, "Quality", -142.03, -141.68 }, Row { LatencyProfileValue::Balanced, "Balanced", -142.03, -141.68 },
                           Row { LatencyProfileValue::LowLatency, "Low Latency", -141.17, -141.83 } })
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
        // docs/11 E10 Done-when: <= -60 dBFS in every profile. What was left
        // after E10 Phase 1 came from the true-peak limiter's gain
        // modulation of the asymmetric waveform: the gain dipped at the big
        // peak and released before the next one. docs/11 E05 stage 1's
        // period hold keeps it flat from peak to peak, so the product of
        // gain and waveform carries no DC (float rounding is left), and the
        // crest gate no longer clips this steady waveform.
        CHECK_LE (dc, -60.0);
        CHECK_LE (limiterOnly, -60.0);
        // Rounding-level values (about -141 dBFS): bounded, not pinned.
        CHECK_LE (dc, r.chainDb + 40.0);
        CHECK_LE (limiterOnly, r.limiterOnlyDb + 40.0);
    }
}

namespace
{
// -12 dBFS 50 Hz, Music, Boost 100 and macros 1-5 at 100 %, THD+N over
// 6..10 s, at one protection strength: scene 0 as it is, scene 1 with
// driven base settings (max.drive 12, Tape saturation at 12 dB). Off
// governs the macro amounts only (as before E06), with the stepwise loop;
// Normal also scales the base max.drive, sat.drive and bass.harmonics, and
// since docs/11 E06 Phase 3 runs the measured loop (the audible residuals
// of the bass engine and of the saturator .. maximizer span, a harmonics
// scale of its own, PLR, feed-forward); Strict does that with stricter
// budgets and a floor of 0. One case per strength keeps each under 2 s.
struct AllMacros50Hz
{
    double thd[2] {}, tracked[2] {}, harmonics[2] {};
};

/** THD+N of a steady tone whose level drifts slowly (a governor's recovery
    or probe, ~1 dB/s): the residual of a sine fit per 100 ms window (whole
    periods of f0), summed over the windows against their power. A level
    ramp is taken out, as a tracking-notch analyser takes it out; harmonics
    and noise are not (at Off it reads within 0.5 dB of the plain fit). */
double trackedThdPlusNoiseDb (const std::vector<float>& x, int begin, int n, double f0)
{
    const int w = samplesOf (0.1);
    double residual = 0.0, total = 0.0;
    for (int p = begin; p + w <= begin + n; p += w)
    {
        double power = 0.0;
        for (int i = p; i < p + w; ++i)
            power += static_cast<double> (x[static_cast<size_t> (i)]) * x[static_cast<size_t> (i)];
        residual += power * std::pow (10.0, thdPlusNoiseDb (x, p, w, f0) / 10.0);
        total += power;
    }
    return total > 0.0 ? 10.0 * std::log10 (std::max (residual, 1.0e-30) / total) : -160.0;
}

AllMacros50Hz allMusicMacros50Hz (ProtectionStrength s)
{
    const auto input = stereoOf (sine (50.0, kFs, samplesOf (10.0), std::pow (10.0f, -12.0f / 20.0f)));
    RenderOptions o = boosted (ModeValue::Music, 100.0f);
    for (const char* m : { "1", "2", "3", "4", "5" })
        o.macros.push_back ({ m, 100.0f });
    const auto macros = resolve (o);
    auto driven = macros;
    setValue (driven, MaxDriveDb, 12.0f);
    setValue (driven, SaturationOn, 1.0f);
    setValue (driven, SatDriveDb, 12.0f);
    AllMacros50Hz r;
    for (int scene = 0; scene < 2; ++scene)
    {
        GovernorReading gr;
        const auto out = renderAtStrength (input, scene == 0 ? macros : driven, s, &gr);
        const auto k = static_cast<size_t> (scene);
        r.thd[k] = thdPlusNoiseDb (out[0], samplesOf (6.0), samplesOf (4.0), 50.0);
        r.tracked[k] = trackedThdPlusNoiseDb (out[0], samplesOf (6.0), samplesOf (4.0), 50.0);
        r.harmonics[k] = gr.harmonicsDb;
        const std::string tag = std::string (scene == 0 ? "all Music macros 100" : "... with max.drive 12 + sat.drive 12")
                                + ", strength " + std::to_string (static_cast<int> (s));
        measured (tag + ": 50 Hz THD+N", r.thd[k], "dB");
        measured (tag + ": 50 Hz THD+N (percent)", 100.0 * std::pow (10.0, r.thd[k] / 20.0), "%");
        measured (tag + ": 50 Hz THD+N, level drift tracked (100 ms fits)", r.tracked[k], "dB");
        measured (tag + ": governor scale at 10 s", gr.scale, "");
        measured (tag + ": governor THD+N input (3 s average)", gr.distortionDb, "dB");
        measured (tag + ": bass harmonics + air exciter share", gr.harmonicsDb, "dB");
    }
    return r;
}

// At Off, as pinned below: THD+N of the two scenes and the bass harmonics +
// air exciter share of scene 0 (the Normal and Strict cases compare with them).
// Re-based for docs/11 E14's Warmth remap (Phase 3 batch 2 review): Warmth
// is now a level-compensated tilt with a gentle Tube colour instead of +9 dB
// of Tape drive, bass boost and harmonics (v1: -13.96 / -12.96 / -13.58 dB).
constexpr double kAllMacrosOffThdDb[2] = { -16.99, -17.21 }, kAllMacrosOffHarmonicsDb = -16.11;
} // namespace

TEST_CASE ("KnownGap closed: all Music macros at 100 on a 50 Hz sine - THD+N unchanged at protection strength Off, with and without driven base settings (E06)")
{
    const auto r = allMusicMacros50Hz (ProtectionStrength::Off);
    // Off is the behaviour before E06 (14.2 % on scene 0; 20.0 % with the v1
    // Warmth), bit for bit: the measured loop and its analysers do not run at
    // Off. (Re-based for docs/11 E04 and E05 step 5 in Phase 2, and for E14's
    // Warmth remap in Phase 3 batch 2.)
    CHECK_NEAR (r.thd[0], kAllMacrosOffThdDb[0], 0.3);
    CHECK_NEAR (r.thd[1], kAllMacrosOffThdDb[1], 0.3);
    CHECK_NEAR (r.harmonics[0], kAllMacrosOffHarmonicsDb, 0.3);
}

TEST_CASE ("KnownGap closed: all Music macros at 100 on a 50 Hz sine - THD+N <= 3 % at protection strength Normal, with and without driven base settings (E06)")
{
    const auto r = allMusicMacros50Hz (ProtectionStrength::Normal);
    // The Done-when row: <= 3 % (-30.46 dB) at Normal. Before Phase 3 Normal
    // read 20.0 % (-13.96 dB): the bass harmonics generator's intended
    // harmonics (-13.6 dB of the output) at the 0.3 floor, which no budget
    // counted. Now the harmonics are budgeted on their own scale by what the
    // programme leaves audible (a steady tone masks none of them), and the
    // drive span's residual (saturator, glue, limiter) on the drive scale.
    //
    // Measured with the level drift tracked (Phase 3 batch 2 review): on this
    // steady sine the governor reaches its 0.3 floor by 3 s and then probes
    // (it recovers at 1 dB/s from about 7 s and backs off again near 10 s,
    // the probe memory's first 4 s hold), and a single sine fit over 6..10 s
    // counts that ramp as THD+N: -25.99 dB plain against -41.7 dB tracked
    // (the v1 Warmth read -36.28 plain here, -28.1 over 2..6 s: the plain
    // fit only measured where the probe fell). Where the ramp falls is not
    // distortion, so the row reads the tracked fit; the plain one is printed.
    CHECK_LE (r.tracked[0], -30.46);
    CHECK_LE (r.harmonics[0], kAllMacrosOffHarmonicsDb - 20.0); // the harmonics were taken down
    // Driven base settings: Normal governs them too (before: 19.9 %).
    CHECK_LE (r.tracked[1], -30.46);
}

TEST_CASE ("KnownGap closed: all Music macros at 100 on a 50 Hz sine - THD+N <= 3 % at protection strength Strict; with driven base settings 10 dB under Off (E06)")
{
    const auto r = allMusicMacros50Hz (ProtectionStrength::Strict);
    CHECK_LE (r.tracked[0], -30.46); // it read 6.0 % before Phase 3
    // Strict, with its 6 dB lower budget, is still moving over 6..10 s on
    // the driven scene (plain fit 7.0 %, 9.6 % before Phase 3; tracked
    // -50.6 dB, see the Normal case).
    CHECK_LE (r.tracked[1], kAllMacrosOffThdDb[1] - 10.0);
}

TEST_CASE ("Chain (E11) Done-when: a -17 LUFS classical stand-in (LRA about 20 LU, -0.5 dBTP) through Classical & Jazz Dynamic keeps its loudness range within 0.1 LU with auto.preamp on")
{
    // Classical programme: wide-band noise (partly correlated stereo) in 3 s
    // sections spanning 27 dB (250 ms raised-cosine joins), at -17 LUFS
    // integrated. The automatic preamp follows the loud parts' level (it does
    // not ride the sections), so it must not change the loudness range.
    const int section = samplesOf (3.0), ramp = samplesOf (0.25);
    const double gains[] = { 0.0, -9.0, -18.0, -27.0, -13.0, -4.0, -23.0 };
    const int n = section * static_cast<int> (std::size (gains));
    const auto a = pinkNoise (n, 0.1f, 1701), b = pinkNoise (n, 0.1f, 1702);
    Channels x (2, std::vector<float> (static_cast<size_t> (n)));
    for (int i = 0; i < n; ++i)
    {
        const int k = i / section, j = i % section;
        double g = gains[k];
        if (j < ramp && k > 0)
            g = gains[k - 1] + (gains[k] - gains[k - 1]) * 0.5 * (1.0 - std::cos (kPi * j / ramp));
        const auto v = static_cast<float> (std::pow (10.0, g / 20.0));
        x[0][static_cast<size_t> (i)] = v * (0.8f * a[static_cast<size_t> (i)] + 0.6f * b[static_cast<size_t> (i)]);
        x[1][static_cast<size_t> (i)] = v * (0.8f * a[static_cast<size_t> (i)] - 0.6f * b[static_cast<size_t> (i)]);
    }
    const double scale = std::pow (10.0, (-17.0 - static_cast<double> (analyse (x, kFs).integratedLufs)) / 20.0);
    for (auto& ch : x)
        for (auto& v : ch)
            v = static_cast<float> (v * scale);
    const auto input = fileOf (x);
    const auto in = analyse (input.channels, kFs);
    measured ("classical stand-in integrated", in.integratedLufs, "LUFS");
    measured ("classical stand-in loudness range", in.loudnessRangeLu, "LU");
    measured ("classical stand-in true peak", in.truePeakDbtp, "dBTP");
    CHECK_NEAR (in.integratedLufs, -17.0, 0.1);
    CHECK_GE (in.loudnessRangeLu, 18.0f);
    CHECK_LE (in.truePeakDbtp, -0.5f);

    auto values = resolve (factoryPreset ("music-classical-jazz-dynamic.json"));
    RenderResult off, on;
    std::string error;
    REQUIRE (renderFile (input, values, RenderSettings {}, off, error));
    setValue (values, AutoPreampOn, 1.0f);
    REQUIRE (renderFile (input, values, RenderSettings {}, on, error));
    measured ("Classical & Jazz loudness range, preamp off", off.outputReport.loudnessRangeLu, "LU");
    measured ("Classical & Jazz loudness range, preamp on", on.outputReport.loudnessRangeLu, "LU");
    measured ("Classical & Jazz integrated, preamp off", off.outputReport.integratedLufs, "LUFS");
    measured ("Classical & Jazz integrated, preamp on", on.outputReport.integratedLufs, "LUFS");
    measured ("Classical & Jazz limiter > 1 dB, preamp on", on.stats.limiterOver1DbPercent, "%");
    CHECK_NEAR (on.outputReport.loudnessRangeLu, in.loudnessRangeLu, 0.1);  // the Done-when row
    CHECK_NEAR (on.outputReport.loudnessRangeLu, off.outputReport.loudnessRangeLu, 0.1);
}

TEST_CASE ("KnownGap closed: hot master - the automatic preamp (auto.preamp, allowance 1 dB) takes the chain's static boost off the limiter; with auto.preampHot also the allowance, the drive and half the Punch attack: Signature and Punchy Pop limit > 1 dB <= 2 % of the time (E11)")
{
    // A hot master: -20 dBFS-RMS pink noise with 55 Hz kicks (-6 dBFS peak)
    // every 500 ms, through the maximizer alone at 10 dB drive, full clipper
    // share and a -0.3 dBTP ceiling: about -9.5 LUFS, -0.35 dBTP.
    const auto kicks = tonesUnderKicks (4.0, 1000.0, 0.0f, 0.5f);
    const auto noiseL = pinkNoise (samplesOf (4.0), 0.1f, 311), noiseR = pinkNoise (samplesOf (4.0), 0.1f, 312);
    Channels raw (2, kicks);
    for (size_t i = 0; i < kicks.size(); ++i)
    {
        raw[0][i] += noiseL[i];
        raw[1][i] += noiseR[i];
    }
    auto mastering = resolve (RenderOptions {});
    onlyModules (mastering, { MaximizerOn });
    setValue (mastering, MaxDriveDb, 10.0f);
    setValue (mastering, MaxCeilingDb, -0.3f);
    setValue (mastering, MaxClipAmount, 1.0f);
    const auto hot = fileOf (render (fileOf (raw), mastering));
    const auto master = analyse (hot.channels, kFs);
    measured ("hot master integrated loudness", master.integratedLufs, "LUFS");
    measured ("hot master true peak", master.truePeakDbtp, "dBTP");

    struct Row
    {
        const char* file;
        double over1Off, over1On, clipOff, clipOn; // pinned: limiter time > 1 dB (%), loudest clip energy (dB)
    };
    // Measured when auto.preamp landed (docs/11 E11). Re-based by docs/11
    // E04 step 2 (both presets use Tighten, 0.1 / 0.2, which no longer
    // cuts the kicks' onsets, so the limiter sees them): Signature 17.87 /
    // 7.47 % / -44.37 / -53.05 dB -> the values below, Punchy Pop 49.33 /
    // 6.93 % / -36.47 / -48.75 dB -> 50.13 / 7.47 % / -38.65 / -54.87 dB.
    // Re-based by docs/11 E04 step 4 (Punch adds clarity.attackHigh, so both
    // presets run the shaper's 3-band path; the preamp and the prediction
    // are unchanged, the hot-programme rows still <= 2 %): Signature 18.93 /
    // 6.93 % / -41.53 / -50.78 dB -> the values below, Punchy Pop 50.13 /
    // 7.47 % / -38.65 / -54.87 dB -> the values below (confirmed in review
    // on a tree with only the E04 / E20 macro rows reverted: the old values).
    const Row rows[] = {
        { "music-flubsound-signature.json", 17.33, 7.20, -40.91, -50.20 },
        { "music-punchy-pop.json", 43.47, 6.93, -36.47, -50.17 },
    };
    for (const auto& row : rows)
    {
        auto values = resolve (factoryPreset (row.file));
        RenderResult off, on, hotOn;
        std::string error;
        REQUIRE (renderFile (hot, values, RenderSettings {}, off, error));
        setValue (values, AutoPreampOn, 1.0f);
        REQUIRE (renderFile (hot, values, RenderSettings {}, on, error));
        auto hotValues = values;
        setValue (hotValues, AutoPreampHot, 1.0f);
        REQUIRE (renderFile (hot, hotValues, RenderSettings {}, hotOn, error));
        const std::string name = std::string (row.file).substr (6, std::string (row.file).size() - 11);
        {
            // The prediction and the preamp behind the "on" render.
            ParameterStore store;
            for (int id = 0; id < kNumParams; ++id)
                store.set (id, values[static_cast<size_t> (id)]);
            ProcessingChain chain (store);
            chain.prepare ({ kFs, 64, 2 });
            std::vector<float> l (64), r (64);
            float* ch[2] = { l.data(), r.data() };
            chain.process (AudioBlock (ch, 2, 64));
            measured (name + " predicted static boost", chain.getPredictedBoostDb(), "dB");
            measured (name + " automatic preamp", chain.getAutoPreampDb(), "dB");
        }
        measured (name + " limiter > 1 dB, preamp off", off.stats.limiterOver1DbPercent, "%");
        measured (name + " limiter > 1 dB, preamp on", on.stats.limiterOver1DbPercent, "%");
        measured (name + " limiter mean GR, preamp off", off.stats.limiterGrMeanDb, "dB");
        measured (name + " limiter mean GR, preamp on", on.stats.limiterGrMeanDb, "dB");
        measured (name + " clip energy max, preamp off", off.stats.clipEnergyMaxDb, "dB");
        measured (name + " clip energy max, preamp on", on.stats.clipEnergyMaxDb, "dB");
        measured (name + " integrated loudness, preamp off", off.outputReport.integratedLufs, "LUFS");
        measured (name + " integrated loudness, preamp on", on.outputReport.integratedLufs, "LUFS");
        measured (name + " limiter > 1 dB, preamp on + hot programme", hotOn.stats.limiterOver1DbPercent, "%");
        measured (name + " limiter mean GR, preamp on + hot programme", hotOn.stats.limiterGrMeanDb, "dB");
        measured (name + " integrated loudness, preamp on + hot programme", hotOn.outputReport.integratedLufs, "LUFS");
        CHECK_NEAR (off.stats.limiterOver1DbPercent, row.over1Off, 0.5);
        CHECK_NEAR (on.stats.limiterOver1DbPercent, row.over1On, 0.5);
        CHECK_NEAR (off.stats.clipEnergyMaxDb, row.clipOff, 0.5);
        CHECK_NEAR (on.stats.clipEnergyMaxDb, row.clipOn, 0.5);
        CHECK_LE (on.stats.limiterOver1DbPercent, off.stats.limiterOver1DbPercent);
        CHECK_LE (on.stats.clipEnergyMaxDb, off.stats.clipEnergyMaxDb);
        // The docs/11 E11 Done-when row: limiter active (> 1 dB) <= 2 % on a
        // hot master for Signature and Punchy Pop. auto.preamp alone leaves
        // their maximizer drive (Boost, loudness on purpose), the 1 dB
        // allowance and Punch's onset lift; auto.preampHot takes them back
        // while the input's peaks reach the ceiling (ProcessingChain.h),
        // at the cost of that loudness.
        CHECK_LE (hotOn.stats.limiterOver1DbPercent, 2.0);
        CHECK_LE (hotOn.outputReport.integratedLufs, on.outputReport.integratedLufs);
    }
}

// =============================================================================
// High-frequency harshness (docs/11 E07): the Smoothness stage and the
// tonal-balance rule
// =============================================================================
namespace
{
/** Band power (Hann-windowed 4096-point FFT, bins in [lo, hi)) of x[begin, begin + n), n <= 4096. */
double fftBandPower (const std::vector<float>& x, int begin, int n, double lo, double hi)
{
    constexpr int kN = 4096;
    static Fft fft;
    if (fft.getSize() != kN)
        fft.prepare (kN);
    std::vector<float> frame (kN, 0.0f);
    for (int i = 0; i < n; ++i)
        frame[static_cast<size_t> (i)] = x[static_cast<size_t> (begin + i)] * static_cast<float> (0.5 - 0.5 * std::cos (kTwoPi * i / n));
    std::vector<Fft::Complex> bins (kN / 2 + 1);
    fft.forwardReal (frame.data(), bins.data());
    double sum = 0.0;
    for (int k = 0; k <= kN / 2; ++k)
        if (const double f = k * kFs / kN; f >= lo && f < hi)
            sum += std::norm (bins[static_cast<size_t> (k)]);
    return sum;
}

/** The report's sung-vocal stand-in (docs/11 E07): a 180 Hz "vowel" (22
    harmonics at 1/k, 5 Hz vibrato) at -18 dBFS RMS, and a 120 ms Hann-gated
    "s" (white noise band-limited to 5 - 10 kHz by 4th-order Butterworths,
    -24 dBFS RMS) every 500 ms from 250 ms on, the vowel ducked by 80 %
    under it. 3 s, `gainDb` over those levels. */
struct VocalScene
{
    io::AudioFileData input;
    std::vector<int> onsets;
};

VocalScene vocalScene (float gainDb)
{
    constexpr double seconds = 3.0;
    const int n = samplesOf (seconds);
    std::vector<double> vowel (static_cast<size_t> (n));
    double phase = 0.0, power = 0.0;
    for (int i = 0; i < n; ++i)
    {
        phase += kTwoPi * 180.0 * (1.0 + 0.01 * std::sin (kTwoPi * 5.0 * i / kFs)) / kFs;
        double v = 0.0;
        for (int k = 1; k <= 22; ++k)
            v += std::sin (k * phase) / k;
        vowel[static_cast<size_t> (i)] = v;
        power += v * v;
    }
    auto s = whiteNoise (n, 1.0f, 99);
    for (int stage = 0; stage < 2; ++stage)
        for (const auto type : { FilterType::HighPass, FilterType::LowPass })
        {
            SvfFilter f;
            f.set (type, type == FilterType::HighPass ? 5000.0 : 10000.0, butterworthQ (2, stage), 0.0, kFs);
            for (auto& v : s)
                v = f.processSample (0, v);
        }
    const double vowelGain = std::pow (10.0, -18.0 / 20.0) / std::sqrt (power / n);
    const double sGain = std::pow (10.0, -24.0 / 20.0) / rms (s.data(), n);
    std::vector<double> gate (static_cast<size_t> (n), 0.0);
    VocalScene scene;
    const int len = samplesOf (0.12);
    for (double t = 0.25; t + 0.3 < seconds; t += 0.5)
    {
        const int a = samplesOf (t);
        scene.onsets.push_back (a);
        for (int i = 0; i < len; ++i)
            gate[static_cast<size_t> (a + i)] = 0.5 - 0.5 * std::cos (kTwoPi * i / len);
    }
    const double g = std::pow (10.0, gainDb / 20.0);
    std::vector<float> x (static_cast<size_t> (n));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float> (g * (vowelGain * vowel[i] * (1.0 - 0.8 * gate[i]) + sGain * s[i] * gate[i]));
    scene.input = stereoOf (x);
    return scene;
}

/** Sibilance (5 - 10 kHz, 30..90 ms into each "s") over voice (150 Hz -
    2 kHz, 200..260 ms after each onset) of the left channel, dB; the first
    "s" is skipped (the report's measure, enh-clarity/t1b). */
double sibilanceOverVoiceDb (const std::vector<float>& y, const std::vector<int>& onsets)
{
    double s = 0.0, v = 0.0;
    for (size_t k = 1; k < onsets.size(); ++k)
    {
        s += fftBandPower (y, onsets[k] + samplesOf (0.03), samplesOf (0.06), 5000.0, 10000.0);
        v += fftBandPower (y, onsets[k] + samplesOf (0.2), samplesOf (0.06), 150.0, 2000.0);
    }
    return powerDb (s) - powerDb (v);
}

/** Mean band power over consecutive 4096-sample frames of [begin, end). */
double fftBandPowerOver (const std::vector<float>& x, int begin, int end, double lo, double hi)
{
    double sum = 0.0;
    int frames = 0;
    for (int p = begin; p + 4096 <= end; p += 4096, ++frames)
        sum += fftBandPower (x, p, 4096, lo, hi);
    return frames > 0 ? sum / frames : 0.0;
}
} // namespace

TEST_CASE ("KnownGap closed: sibilance over voice - a sung vocal at Music Boost 100 + Clarity 100 rises +2.6 dB; Smoothness 100 holds it to <= +0.5 dB (<= +1 dB at +8 dB input), the maximizer after the stage included (E07)")
{
    // Change of sibilance / voice against the input (bypass), at 0 and +8 dB
    // input. Smoothness (smooth.amount, SmoothnessGuard.h) sits after the
    // saturator and before the compressor and the maximizer, and holds each
    // "s" to the lift of the voice around it at the chain's output (the
    // maximizer's ducking of the loud vowels included; before Phase 3
    // batch 2 it took the band back to the balance under the "s" at the
    // dynamic EQ's input: +1.16 / +2.00 dB at Boost 100 + Clarity 100).
    struct Setting
    {
        const char* name;
        float boost, clarity, smooth, gainDb;
    };
    const Setting settings[] = {
        { "Boost 100 + Clarity 100, Smoothness 0, 0 dB", 100.0f, 100.0f, 0.0f, 0.0f },
        { "Boost 100 + Clarity 100, Smoothness 100, 0 dB", 100.0f, 100.0f, 1.0f, 0.0f },
        { "Boost 100 + Clarity 100, Smoothness 0, +8 dB", 100.0f, 100.0f, 0.0f, 8.0f },
        { "Boost 100 + Clarity 100, Smoothness 100, +8 dB", 100.0f, 100.0f, 1.0f, 8.0f },
        { "Boost 70 + Clarity 70, Smoothness 100, 0 dB", 70.0f, 70.0f, 1.0f, 0.0f },
        { "Clarity 100, Smoothness 100, 0 dB", 0.0f, 100.0f, 1.0f, 0.0f },
        { "Boost 100 + Clarity 100, Smoothness 100, -10 dB", 100.0f, 100.0f, 1.0f, -10.0f },
    };
    double change[std::size (settings)] = {};
    for (size_t k = 0; k < std::size (settings); ++k)
    {
        const auto& s = settings[k];
        const auto scene = vocalScene (s.gainDb);
        RenderOptions o = boosted (ModeValue::Music, s.boost);
        o.macros.push_back ({ "3", s.clarity });
        auto values = resolve (o);
        setValue (values, SmoothAmount, s.smooth);
        const auto out = render (scene.input, values);
        change[k] = sibilanceOverVoiceDb (out[0], scene.onsets) - sibilanceOverVoiceDb (scene.input.channels[0], scene.onsets);
        measured (std::string ("sibilance / voice change, ") + s.name, change[k], "dB");
    }
    // Smoothness 0 is the chain before E07, bit for bit.
    CHECK_NEAR (change[0], 2.64, 0.1);
    CHECK_NEAR (change[2], 3.61, 0.1);
    // The enhancement's own share is taken back: Clarity 100 alone (no
    // maximizer) and Boost 100 + Clarity 100 at -10 dB (the maximizer idle).
    CHECK_LE (change[5], 0.5);
    CHECK_LE (change[6], 0.5);
    // The docs/11 E07 Done-when rows: <= +0.5 dB at 0 dB input and <= +1 dB
    // at +8 dB. The maximizer ducks the loud vowels 0.6 dB more than the
    // quieter "s" (the voice in the vowel frames against the voice under
    // the "s"); the stage reads that at the output. Boost 70 + Clarity 70 is
    // the fatigue session's setting.
    CHECK_LE (change[1], 0.5);
    CHECK_LE (change[3], 1.0);
    CHECK_LE (change[4], 0.5);
}

TEST_CASE ("KnownGap closed at protection strength Normal: Gaming full stack on -30 dBFS pink - 2-5 kHz lift minus 200 Hz-1 kHz lift <= +2 dB (+4.1 dB at Off) (E07)")
{
    // Gaming, Boost 100 and every macro at 100, -30 dBFS pink, 10 s; the
    // tilt over 7..10 s (the tonal-balance rule averages over ~1 s and
    // settles in about 6 s). Off (the default) has no tonal rule: its tilt
    // is the chain before E07. (Strict: tests/test_protection_tonal.cpp.)
    const auto input = stereoOf (pinkNoise (samplesOf (10.0), std::pow (10.0f, -30.0f / 20.0f), 2024));
    RenderOptions o = boosted (ModeValue::Gaming, 100.0f);
    for (const char* m : { "1", "2", "3", "4", "5" })
        o.macros.push_back ({ m, 100.0f });
    const auto values = resolve (o);
    double tilt[2] = {}, harsh[2] = {};
    for (const auto s : { ProtectionStrength::Off, ProtectionStrength::Normal })
    {
        const auto out = renderAtStrength (input, values, s);
        const int a = samplesOf (7.0), b = samplesOf (10.0);
        const auto lift = [&] (double lo, double hi) {
            return powerDb (fftBandPowerOver (out[0], a, b, lo, hi)) - powerDb (fftBandPowerOver (input.channels[0], a, b, lo, hi));
        };
        const auto k = static_cast<size_t> (s);
        tilt[k] = lift (2000.0, 5000.0) - lift (200.0, 1000.0);
        harsh[k] = lift (5000.0, 10000.0) - lift (200.0, 1000.0);
        measured ("Gaming full stack, strength " + std::to_string (static_cast<int> (s)) + ": 2-5 kHz minus 200 Hz-1 kHz lift", tilt[k], "dB");
        measured ("Gaming full stack, strength " + std::to_string (static_cast<int> (s)) + ": 5-10 kHz minus 200 Hz-1 kHz lift", harsh[k], "dB");
    }
    // KNOWN_GAP at Off (the default): target <= +2 dB per docs/11 E07
    // Done-when; the rule runs at protection strength Normal / Strict.
    // Re-based by docs/11 E20, 4.06 -> 4.39 dB: Impact 100's static 6 dB
    // shelf at 70 Hz and harmonics became an event-keyed burst, and their
    // skirt had lifted the reference band, 200 Hz - 1 kHz (CLI, same
    // stack: 2-5 kHz lift 10.84 -> 10.78 dB, 200 Hz - 1 kHz 6.47 -> 6.00).
    CHECK_NEAR (tilt[0], 4.39, 0.2);
    // The Done-when row at Normal.
    CHECK_LE (tilt[1], 2.0);
    CHECK_LE (harsh[1], 2.0);
}

TEST_CASE ("KnownGap: presence against programme level, clarity.presenceMode Absolute (the default) - pink at -45 and -12 dBFS: clarity.presence 1 lifts 5.2 dB more at -45; Music Boost 100 + Clarity 100 at protection strength Normal within 3.2 dB (6.7 at Off) (E07)")
{
    // 2.5-4 kHz lift of pink. (a) clarity.presence 1 as a base value,
    // maximizer off (the report's measure), 3 s, over 1..3 s: the Clarity
    // presence's inverse-level law (ClarityEnhancer.cpp presenceGainDb,
    // kPresenceThresholdDb); the tonal-balance rule never scales base values
    // (tests/test_protection_tonal.cpp). (b) Music Boost 100 + Clarity 100,
    // the lift over the 200 Hz - 1 kHz lift, 10 s, over 7..10 s, where the
    // rule scales the macros' presence at Normal.
    const auto pink = pinkNoise (samplesOf (10.0), 1.0f, 55);
    auto base = resolve (RenderOptions {});
    setValue (base, MaximizerOn, 0.0f);
    setValue (base, ClarityPresence, 1.0f);
    RenderOptions o = boosted (ModeValue::Music, 100.0f);
    o.macros.push_back ({ "3", 100.0f });
    const auto macros = resolve (o);
    const auto lift = [&pink] (const std::vector<float>& values, ProtectionStrength s, float levelDb, double seconds, bool relative) {
        const int n = samplesOf (seconds), a = samplesOf (seconds - 3.0 + (seconds < 5.0 ? 1.0 : 0.0));
        std::vector<float> x (pink.begin(), pink.begin() + n);
        for (auto& v : x)
            v *= std::pow (10.0f, levelDb / 20.0f);
        const auto out = renderAtStrength (stereoOf (x), values, s);
        const auto band = [&] (double lo, double hi) {
            return powerDb (fftBandPowerOver (out[0], a, n, lo, hi)) - powerDb (fftBandPowerOver (x, a, n, lo, hi));
        };
        return band (2500.0, 4000.0) - (relative ? band (200.0, 1000.0) : 0.0);
    };
    const double baseSpread = lift (base, ProtectionStrength::Off, -45.0f, 3.0, false) - lift (base, ProtectionStrength::Off, -12.0f, 3.0, false);
    const double off = lift (macros, ProtectionStrength::Off, -45.0f, 3.0, true) - lift (macros, ProtectionStrength::Off, -12.0f, 3.0, true);
    const double normal = lift (macros, ProtectionStrength::Normal, -45.0f, 10.0, true) - lift (macros, ProtectionStrength::Normal, -12.0f, 10.0, true);
    measured ("clarity.presence 1: 2.5-4 kHz lift at -45 minus at -12 dBFS", baseSpread, "dB");
    measured ("Music Boost 100 + Clarity 100, Off: relative presence lift at -45 minus at -12 dBFS", off, "dB");
    measured ("Music Boost 100 + Clarity 100, Normal: relative presence lift at -45 minus at -12 dBFS", normal, "dB");
    // KNOWN_GAP in Absolute, the default: target within 1.5 dB per docs/11
    // E07 Done-when, met by clarity.presenceMode Relative (approach step 3;
    // the next case). Making Relative the default re-voices every preset
    // that lifts presence: an owner decision. The tonal-balance rule at
    // Normal takes the quiet programme's lift down to its budget.
    CHECK_NEAR (baseSpread, 5.2, 0.2);
    CHECK_NEAR (off, 6.67, 0.3);
    CHECK_NEAR (normal, 3.2, 0.3);
    CHECK_LE (normal, off - 2.5);
}

namespace
{
/** docs/11 E07 / E59 presence invariance: the 2.5 - 4 kHz lift of pink at
    `quietDb` minus at `loudDb` (dBFS RMS), over the lift of 200 Hz - 1 kHz
    when `relative`; `seconds` of programme, read over its last 2 s (3 s
    once the tonal rule runs, which settles in about 6 s). */
double presenceSpreadDb (const std::vector<float>& values, ProtectionStrength s, float quietDb, float loudDb, double seconds, bool relative)
{
    static const auto pink = pinkNoise (samplesOf (10.0), 1.0f, 55);
    const auto lift = [&] (float levelDb) {
        const int n = samplesOf (seconds), a = samplesOf (seconds - (seconds < 5.0 ? 2.0 : 3.0));
        std::vector<float> x (pink.begin(), pink.begin() + n);
        for (auto& v : x)
            v *= std::pow (10.0f, levelDb / 20.0f);
        const auto out = renderAtStrength (stereoOf (x), values, s);
        const auto band = [&] (double lo, double hi) {
            return powerDb (fftBandPowerOver (out[0], a, n, lo, hi)) - powerDb (fftBandPowerOver (x, a, n, lo, hi));
        };
        return band (2500.0, 4000.0) - (relative ? band (200.0, 1000.0) : 0.0);
    };
    return lift (quietDb) - lift (loudDb);
}

std::vector<float> relativePresence (std::vector<float> values)
{
    setValue (values, ClarityPresenceMode, static_cast<float> (PresenceModeValue::Relative));
    return values;
}
} // namespace

TEST_CASE ("KnownGap closed with clarity.presenceMode Relative for clarity.presence 1 - pink at -45 and -12 dBFS within 1.5 dB (5.2 dB in Absolute); Music Boost 100 + Clarity 100 at protection strength Normal 2.0 dB (3.2 in Absolute): the de-harsh band's fixed threshold (E07 step 3)")
{
    // The Done-when rows of docs/11 E07 on the previous case's stimuli, with
    // the presence read against the programme's own 200 Hz - 1 kHz body
    // (ClarityEnhancer.cpp). (a) clarity.presence 1 as a base value,
    // maximizer off, 3 s. (b) Music Boost 100 + Clarity 100, relative to the
    // 200 Hz - 1 kHz lift, 10 s at Normal.
    auto base = resolve (RenderOptions {});
    setValue (base, MaximizerOn, 0.0f);
    setValue (base, ClarityPresence, 1.0f);
    RenderOptions o = boosted (ModeValue::Music, 100.0f);
    o.macros.push_back ({ "3", 100.0f });
    const double baseSpread = presenceSpreadDb (relativePresence (base), ProtectionStrength::Off, -45.0f, -12.0f, 3.0, false);
    const double normal = presenceSpreadDb (relativePresence (resolve (o)), ProtectionStrength::Normal, -45.0f, -12.0f, 10.0, true);
    measured ("Relative, clarity.presence 1: 2.5-4 kHz lift at -45 minus at -12 dBFS", baseSpread, "dB");
    measured ("Relative, Music Boost 100 + Clarity 100, Normal: relative presence lift at -45 minus at -12 dBFS", normal, "dB");
    // The Done-when row for the presence itself (Absolute: 5.2 dB).
    CHECK_LE (std::abs (baseSpread), 1.5);
    // KNOWN_GAP: target within 1.5 dB. What is left is not the presence:
    // the Clarity macro's de-harsh band (the Music mode band at 3.5 kHz in
    // the dynamic EQ, CutAbove over a fixed -22 dB, up to 3 dB) cuts the
    // -12 dBFS pink and not the -45 dBFS one (Clarity 100 alone, Relative,
    // Off: 2.2 dB; Boost 100 alone -0.5 dB). It needs that band relative
    // too (E07 approach step 1, ProcessingChain.cpp configureModeBands).
    CHECK_NEAR (normal, 2.0, 0.3);
    CHECK_LE (normal, 3.17 - 1.0);
}

TEST_CASE ("Quality metric (E59): presence invariance over 30 dB - the 2.5-4 kHz lift over the 200 Hz - 1 kHz lift at -45 against -15 dBFS pink, Absolute against Relative presence, Music Boost 50 and Gaming Boost 100 + Voice & Score 100")
{
    // docs/11 E59's presence-invariance row: how much more a setting lifts
    // the presence of quiet programme than of loud (0 = invariant). 3 s of
    // pink, read over the last 2 s, protection strength Off (the default).
    // Every row is pinned; the Relative rows are held to E07's 1.5 dB
    // where nothing else level-dependent lifts the band.
    struct Setting
    {
        const char* name;
        ModeValue mode;
        float boost;
        const char* macro;
    };
    const Setting settings[] = { { "Music Boost 50", ModeValue::Music, 50.0f, nullptr },
                                 { "Gaming Boost 100 + Voice & Score 100", ModeValue::Gaming, 100.0f, "5" } };
    double spread[2][2] = {};
    for (size_t k = 0; k < std::size (settings); ++k)
    {
        RenderOptions o = boosted (settings[k].mode, settings[k].boost);
        if (settings[k].macro != nullptr)
            o.macros.push_back ({ settings[k].macro, 100.0f });
        const auto values = resolve (o);
        spread[k][0] = presenceSpreadDb (values, ProtectionStrength::Off, -45.0f, -15.0f, 3.0, true);
        spread[k][1] = presenceSpreadDb (relativePresence (values), ProtectionStrength::Off, -45.0f, -15.0f, 3.0, true);
        measured (std::string ("presence invariance, ") + settings[k].name + ", Absolute", spread[k][0], "dB");
        measured (std::string ("presence invariance, ") + settings[k].name + ", Relative", spread[k][1], "dB");
    }
    // Pinned (the ratchet for later changes): Absolute Music Boost 50 1.48,
    // Gaming 6.00 dB; Relative -0.02 and 1.46 dB.
    CHECK_NEAR (spread[0][0], 1.48, 0.3);
    CHECK_NEAR (spread[1][0], 6.00, 0.3);
    CHECK_LE (std::abs (spread[0][1]), 1.5);
    // KNOWN_GAP (at the 1.5 dB row's edge): with the presence relative, the
    // Gaming Voice & Score band (the dynamic EQ's boost-below mode band over
    // a fixed -36 dB, up to 4 dB) still lifts quiet programme more.
    CHECK_NEAR (spread[1][1], 1.46, 0.3);
    for (size_t k = 0; k < std::size (settings); ++k)
        CHECK_GE (spread[k][0], spread[k][1]);
}

TEST_CASE ("E07: the footstep cue lift survives Smoothness 100 and the tonal-balance rule - Gaming Footsteps 100 + Boost 100 steps under a bed at protection strength Normal (risk P4)")
{
    // The E19 burst scene at -24 LUFS; the cue enhancer's bands are never
    // scaled by the rule and sit ahead of the Smoothness stage's reference,
    // Boost's presence is. Lift of the steps in the 3.2 kHz band, out vs in.
    const auto scene = makeBurstScene (-24.0);
    RenderOptions o = boosted (ModeValue::Gaming, 100.0f);
    o.macros.push_back ({ "footsteps", 100.0f });
    const auto values = resolve (o);
    auto smooth = values;
    setValue (smooth, SmoothAmount, 1.0f);
    const auto lifts = [&scene] (const std::vector<float>& v, ProtectionStrength s) {
        const auto out = renderAtStrength (scene.input, v, s);
        const auto inBand = bandPass (midOf (scene.input.channels), 3200.0, 1.0);
        const auto outBand = bandPass (midOf (out), 3200.0, 1.0);
        const double inBed = meanPower (inBand, scene.bed), outBed = meanPower (outBand, scene.bed);
        double lift = 0.0;
        for (int d = 0; d < 3; ++d)
            lift += (powerDb (meanPower (outBand, scene.bursts[d]) - outBed) - powerDb (meanPower (inBand, scene.bursts[d]) - inBed)) / 3.0;
        return lift;
    };
    const double off = lifts (values, ProtectionStrength::Off);
    const double guarded = lifts (smooth, ProtectionStrength::Normal);
    measured ("Footsteps 100 + Boost 100, Off, Smoothness 0: mean step lift", off, "dB");
    measured ("Footsteps 100 + Boost 100, Normal, Smoothness 100: mean step lift", guarded, "dB");
    CHECK (guarded >= off - 1.0);
}
