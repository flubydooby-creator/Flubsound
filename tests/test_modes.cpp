// Gaming mode (R2.9) through the full ProcessingChain: what each Gaming macro
// does to the effective parameters (MacroMap's kGamingTable) and to the sound
// (the internal dynamic-EQ mode bands of configureModeBands and the mode /
// binaural policy in ProcessingChain::applyParameters).
//
// Audible checks measure the steady-state level of test tones in the last
// 0.5 s of a 1 s render (integer periods of every tone used) - or, for the
// Footsteps cue enhancer, a tone burst rising out of a pink bed - always
// against the same chain with the macro at 0. Where a macro engages several modules,
// the GUI's audition bypass holds the others off so one mechanism is
// measured at a time.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/common/Denormals.h"
#include "flub/dsp/BassEngine.h"
#include "flub/dsp/Crossover.h"
#include "flub/dsp/DynamicEq.h"
#include "flub/engine/MacroMap.h"
#include "flub/engine/ProcessingChain.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>

using namespace flub;
using namespace flub::param;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;
constexpr int kBlock = 256;
constexpr int kLen = 48000;      // 1 s per render
constexpr int kMeasure = 24000;  // measured over the last 0.5 s

using Setup = std::function<void (ParameterStore&)>;

/** What a Gaming-mode render reports: the output, a few meters as of the
    last block, and the effective (post-macro) parameter values. */
struct Render
{
    Planar out;
    std::array<float, MeterBus::kMaxDynBands> dynEqDb {};
    float effectiveWidth = 0.0f, compUpwardDb = 0.0f;
    std::vector<float> effective;
    double fs = kFs;
    float autoPreampDb = 0.0f, predictedBoostDb = 0.0f;

    /** Level (dBFS) of the component at `freq` in output channel `ch`, over
        the last 0.5 s. */
    double toneDb (int ch, double freq) const
    {
        const int n = static_cast<int> (fs / 2.0);
        return toDb (toneAmplitude (out.ch[static_cast<size_t> (ch)].data() + out.numSamples() - n, n, freq, fs));
    }
    float eff (int id) const { return effective[static_cast<size_t> (id)]; }
};

/** Renders `in` through a fresh chain in Gaming mode at `fs`. `setup`
    adjusts the store before prepare(); `held` modules are held off with the
    audition bypass. */
Render renderGaming (const Setup& setup, Planar in, std::initializer_list<int> held = {}, double fs = kFs)
{
    ParameterStore store;
    store.set (Mode, static_cast<float> (ModeValue::Gaming));
    setup (store);
    ProcessingChain chain (store);
    chain.prepare ({ fs, kBlock, in.numChannels() });
    for (int id : held)
        chain.setAuditionBypass (id, true);
    {
        ScopedNoDenormals noDenormals;
        const int n = in.numSamples();
        for (int pos = 0; pos < n; pos += kBlock)
            chain.process (in.block (pos, std::min (kBlock, n - pos)));
    }
    Render r { std::move (in), {}, 0.0f, 0.0f, {}, fs };
    for (size_t b = 0; b < r.dynEqDb.size(); ++b)
        r.dynEqDb[b] = chain.meters().dynEqGainDb[b].load();
    r.effectiveWidth = chain.meters().effectiveWidth.load();
    r.compUpwardDb = chain.meters().compUpwardGainDb.load();
    r.autoPreampDb = chain.getAutoPreampDb();
    r.predictedBoostDb = chain.getPredictedBoostDb();
    for (int i = 0; i < kNumParams; ++i)
        r.effective.push_back (chain.effectiveValue (i));
    return r;
}

/** Effective values a Gaming-mode chain publishes after prepare() (the macro
    staging law, governor at 1). */
std::vector<float> effectiveAfterPrepare (const Setup& setup)
{
    ParameterStore store;
    store.set (Mode, static_cast<float> (ModeValue::Gaming));
    setup (store);
    ProcessingChain chain (store);
    chain.prepare ({ kFs, kBlock, 2 });
    std::vector<float> e;
    for (int i = 0; i < kNumParams; ++i)
        e.push_back (chain.effectiveValue (i));
    return e;
}

/** 1 s stereo sine at `dbfs` on the left; the right channel is `right` x left. */
Planar tone (double freq, float dbfs, float right = 1.0f, double fs = kFs)
{
    const int len = static_cast<int> (fs);
    Planar p (2, len);
    const auto s = sine (freq, fs, len, dbToGain (dbfs));
    for (int i = 0; i < len; ++i)
    {
        p.ch[0][static_cast<size_t> (i)] = s[static_cast<size_t> (i)];
        p.ch[1][static_cast<size_t> (i)] = right * s[static_cast<size_t> (i)];
    }
    return p;
}

/** Store setup: the given module toggles stored OFF (so any engagement is
    the macro's doing) and `macroId` at `amount`. */
Setup macroOnly (int macroId, float amount, std::initializer_list<int> storedOff)
{
    std::vector<int> off (storedOff);
    return [macroId, amount, off] (ParameterStore& s) {
        for (int id : off)
            s.set (id, 0.0f);
        s.set (macroId, amount);
    };
}

double maxAbsDiff (const Planar& a, const Planar& b)
{
    double m = 0.0;
    for (size_t c = 0; c < 2; ++c)
        for (size_t i = 0; i < a.ch[c].size(); ++i)
            m = std::max (m, static_cast<double> (std::abs (a.ch[c][i] - b.ch[c][i])));
    return m;
}

double rmsDiff (const Planar& a, const Planar& b)
{
    double acc = 0.0;
    for (size_t c = 0; c < 2; ++c)
        for (size_t i = 0; i < a.ch[c].size(); ++i)
        {
            const double d = static_cast<double> (a.ch[c][i]) - b.ch[c][i];
            acc += d * d;
        }
    return std::sqrt (acc / (2.0 * static_cast<double> (a.ch[0].size())));
}

/** A 1.2 s stereo pink bed at `bedDb` dBFS RMS (none below -150) with a
    tone burst at `freq`: peak `burstDb` dBFS, 80 ms from 0.8 s (after the
    cue bands have learnt the bed), 2 ms raised-cosine edges. */
Planar burstOverBed (double freq, float burstDb, float bedDb, double fs = kFs)
{
    const int len = static_cast<int> (1.2 * fs), onset = static_cast<int> (0.8 * fs), n = static_cast<int> (0.08 * fs);
    const int edge = static_cast<int> (0.002 * fs);
    Planar p (2, len);
    if (bedDb > -150.0f)
    {
        const auto bed = pinkNoise (len, dbToGain (bedDb), 97);
        for (auto& c : p.ch)
            std::copy (bed.begin(), bed.end(), c.begin()); // keep the storage Planar points at
    }
    const double a = dbToGain (burstDb);
    for (int i = 0; i < n; ++i)
    {
        const double w = i < edge ? 0.5 - 0.5 * std::cos (kPi * i / edge) : (i >= n - edge ? 0.5 - 0.5 * std::cos (kPi * (n - i) / edge) : 1.0);
        const auto v = static_cast<float> (a * w * std::sin (kTwoPi * freq * (onset + i) / fs));
        p.ch[0][static_cast<size_t> (onset + i)] += v;
        p.ch[1][static_cast<size_t> (onset + i)] += v;
    }
    return p;
}

/** The latency (samples) of a Gaming-mode chain set up by `setup`. */
int chainLatency (const Setup& setup, double fs)
{
    ParameterStore store;
    store.set (Mode, static_cast<float> (ModeValue::Gaming));
    setup (store);
    ProcessingChain chain (store);
    chain.prepare ({ fs, kBlock, 2 });
    return chain.getLatencySamples();
}

/** Lift (dB) of the burst at `freq` over [fromMs, toMs) after its onset:
    output amplitude with `on` against the same chain with `off`. `onRender`
    (optional) receives the `on` render. */
double burstLift (const Setup& on, const Setup& off, const Planar& in, double freq, double fromMs, double toMs,
                  std::initializer_list<int> held = {}, double fs = kFs, Render* onRender = nullptr)
{
    // Windows in the output, i.e. after each chain's latency.
    const int a = static_cast<int> ((0.8 + fromMs * 0.001) * fs), n = static_cast<int> ((toMs - fromMs) * 0.001 * fs);
    const int a1 = a + chainLatency (on, fs), a0 = a + chainLatency (off, fs);
    auto r1 = renderGaming (on, in, held, fs);
    const auto r0 = renderGaming (off, in, held, fs);
    const double lift = toDb (toneAmplitude (r1.out.ch[0].data() + a1, n, freq, fs) / toneAmplitude (r0.out.ch[0].data() + a0, n, freq, fs));
    CHECK_NEAR (toDb (toneAmplitude (r1.out.ch[1].data() + a1, n, freq, fs) / toneAmplitude (r0.out.ch[1].data() + a0, n, freq, fs)), lift, 0.01); // stereo-linked
    if (onRender != nullptr)
        *onRender = std::move (r1);
    return lift;
}
} // namespace

// ---------------------------------------------------------------------------
TEST_CASE ("Gaming Footsteps (M1): mode band 4 is the cue enhancer - a 3.2 kHz cue rising out of the bed gets 7 dB x Footsteps from its first milliseconds at any level; a steady sound, loud cues and hiss get none")
{
    // Mode band 4: DynEqMode::CueLift bell at 3.2 kHz, Q 0.9, range 7 dB x
    // Footsteps, hiss floor -75 dB (docs/11 E19, see DynamicEq.h): the lift
    // is keyed to how far the band stands out of its own slow background,
    // not to a fixed threshold.
    CHECK (ProcessingChain::modeBandFrequency (ModeValue::Gaming, 4) == 3200.0f);
    CHECK (ProcessingChain::modeBandFrequency (ModeValue::Gaming, 5) == 260.0f);
    CHECK (ProcessingChain::modeBandFrequency (ModeValue::Gaming, 6) == 90.0f);
    CHECK (ProcessingChain::modeBandFrequency (ModeValue::Gaming, 7) == 2000.0f);
    CHECK (ProcessingChain::modeBandFrequency (ModeValue::Gaming, 3) == 0.0f); // a user band
    CHECK (ProcessingChain::modeBandFrequency (ModeValue::Gaming, 8) == 0.0f);
    const double f = ProcessingChain::modeBandFrequency (ModeValue::Gaming, 4);

    // Footsteps engages the dynamic EQ only: the broadband upward compressor
    // it used to add lifted the bed with the steps (docs/11 E19).
    const auto footsteps = [] (float amount) { return macroOnly (Macro1, amount, { DynEqOn, CompressorOn }); };
    {
        const auto e0 = effectiveAfterPrepare (footsteps (0.0f));
        CHECK (e0[DynEqOn] < 0.5f);
        const auto e1 = effectiveAfterPrepare (footsteps (1.0f));
        CHECK (e1[DynEqOn] >= 0.5f);
        CHECK (e1[CompressorOn] < 0.5f);
        CHECK (e1[CompUpMaxGainDb] == 0.0f);
    }

    Render r { Planar (2, 1), {}, 0.0f, 0.0f, {}, kFs };
    // A cue as loud as the bed's full-band RMS (about 9 dB over the bed in
    // the band) gets the full range at -60, -40 and -20 dBFS alike, within
    // 2-12 ms of its onset nearly all of it, and none once it has ended.
    for (const float level : { -60.0f, -40.0f, -20.0f })
    {
        const auto in = burstOverBed (f, level, level);
        CHECK_NEAR (burstLift (footsteps (1.0f), footsteps (0.0f), in, f, 10.0, 70.0, {}, kFs, &r), 7.0, 0.3);
        CHECK_LE (std::abs (r.dynEqDb[4]), 0.01f); // released 0.3 s after the cue
        CHECK_GE (burstLift (footsteps (1.0f), footsteps (0.0f), in, f, 2.0, 12.0), 6.0);
    }
    CHECK_NEAR (burstLift (footsteps (0.5f), footsteps (0.0f), burstOverBed (f, -40.0f, -40.0f), f, 10.0, 70.0), 3.5, 0.2);
    // Out of digital silence (the background is the hiss floor): the full range.
    CHECK_NEAR (burstLift (footsteps (1.0f), footsteps (0.0f), burstOverBed (f, -60.0f, -200.0f), f, 10.0, 70.0), 7.0, 0.3);
    // Under the -75 dB hiss floor nothing is lifted.
    CHECK_LE (std::abs (burstLift (footsteps (1.0f), footsteps (0.0f), burstOverBed (f, -90.0f, -200.0f), f, 10.0, 70.0)), 0.05);
    // A loud cue (gunfire level, -3 dBFS over a -50 dBFS bed) hits the loud
    // cap: at most a trace of lift.
    CHECK_LE (burstLift (footsteps (1.0f), footsteps (0.0f), burstOverBed (f, -3.0f, -50.0f), f, 10.0, 70.0), 0.3);
    // A steady tone is the background itself: not lifted, at any level.
    for (const float level : { -60.0f, -40.0f, -20.0f })
    {
        const auto ref = renderGaming (footsteps (0.0f), tone (f, level));
        r = renderGaming (footsteps (1.0f), tone (f, level));
        CHECK_LE (std::abs (r.toneDb (0, f) - ref.toneDb (0, f)), 0.05);
        CHECK_LE (std::abs (r.dynEqDb[4]), 0.01f);
    }

    // Speech-link rates (docs/11 E17): at 32 kHz and below the output is a
    // Bluetooth hands-free link, where 3.2 kHz is 0.2..0.8 x Nyquist of a
    // narrowband channel; the band is off there and on again at 44.1 kHz.
    for (const double fs : { 8000.0, 16000.0, 32000.0 })
    {
        CHECK_LE (std::abs (burstLift (footsteps (1.0f), footsteps (0.0f), burstOverBed (f, -40.0f, -40.0f, fs), f, 10.0, 70.0, {}, fs, &r)), 0.05);
        CHECK_LE (std::abs (r.dynEqDb[4]), 0.01f);
    }
    CHECK_NEAR (burstLift (footsteps (1.0f), footsteps (0.0f), burstOverBed (f, -40.0f, -40.0f, 44100.0), f, 10.0, 70.0, {}, 44100.0), 7.0, 0.3);
}

TEST_CASE ("Gaming Footsteps (M1): mode band 5 is the footstep-body cue enhancer at 260 Hz (3 dB x Footsteps); a steady low-mid and loud hits are not lifted")
{
    // Mode band 5: CueLift bell at 260 Hz, Q 1.2, range 3 dB x Footsteps
    // (heel impact), level averaged over four periods (15 ms).
    const double f = ProcessingChain::modeBandFrequency (ModeValue::Gaming, 5);
    const auto footsteps = [] (float amount) { return macroOnly (Macro1, amount, { DynEqOn, CompressorOn }); };
    Render r { Planar (2, 1), {}, 0.0f, 0.0f, {}, kFs };
    for (const float level : { -50.0f, -30.0f })
    {
        CHECK_NEAR (burstLift (footsteps (1.0f), footsteps (0.0f), burstOverBed (f, level, level), f, 15.0, 75.0, {}, kFs, &r), 3.0, 0.3);
        CHECK_LE (std::abs (r.dynEqDb[5]), 0.05f); // released (the slower low-band level leaves a 0.03 dB tail)
    }
    CHECK_NEAR (burstLift (footsteps (0.5f), footsteps (0.0f), burstOverBed (f, -40.0f, -40.0f), f, 15.0, 75.0), 1.5, 0.2);
    CHECK_LE (burstLift (footsteps (1.0f), footsteps (0.0f), burstOverBed (f, -3.0f, -50.0f), f, 15.0, 75.0), 0.3);
    const auto ref = renderGaming (footsteps (0.0f), tone (f, -30.0f));
    r = renderGaming (footsteps (1.0f), tone (f, -30.0f));
    CHECK_LE (std::abs (r.toneDb (0, f) - ref.toneDb (0, f)), 0.05);
    CHECK_LE (std::abs (r.dynEqDb[5]), 0.01f);
}

TEST_CASE ("Gaming cue enhancer (DynamicEq CueLift): bit-identical for any host block split, an exact identity on a steady tone, and NaN input is flushed")
{
    // Band 4's settings at Footsteps 100, on the DynamicEq alone.
    DynEqBandParams cue;
    cue.enabled = true;
    cue.mode = DynEqMode::CueLift;
    cue.frequency = 3200.0f;
    cue.q = 0.9f;
    cue.rangeDb = 7.0f;
    cue.attackMs = 1.0f;
    cue.releaseMs = 40.0f;
    cue.noiseFloorDb = -75.0f;
    const auto run = [&cue] (Planar buf, const std::vector<int>& blocks) {
        DynamicEq d;
        d.prepare ({ kFs, 1024, 2 });
        d.setBand (4, cue);
        d.reset();
        ScopedNoDenormals noDenormals;
        size_t k = 0;
        for (int pos = 0; pos < buf.numSamples();)
        {
            const int len = std::min (blocks[k++ % blocks.size()], buf.numSamples() - pos);
            d.process (buf.block (pos, len));
            pos += len;
        }
        return std::make_pair (std::move (buf), d.getBandGainDb (4));
    };

    // Bursts over a bed, in 512-sample blocks and in odd, varying ones.
    const auto in = burstOverBed (3200.0, -40.0f, -40.0f);
    const auto a = run (in, { 512 }).first, b = run (in, { 1, 37, 256, 5, 999, 16, 17 }).first;
    CHECK (a.ch == b.ch);
    CHECK_GE (toDb (toneAmplitude (a.ch[0].data() + static_cast<int> (0.81 * kFs), static_cast<int> (0.06 * kFs), 3200.0, kFs)) + 40.0, 6.5);

    // A steady tone: once learnt, the band sits at exactly 0 dB, and the EQ
    // is an exact identity (the last 0.3 s bit-identical to the input).
    const auto toneIn = tone (3200.0, -30.0f);
    const auto [toneOut, gainDb] = run (toneIn, { 256 });
    CHECK (gainDb == 0.0f);
    const auto tail = static_cast<std::ptrdiff_t> (0.7 * kFs);
    CHECK (std::equal (toneOut.ch[0].begin() + tail, toneOut.ch[0].end(), toneIn.ch[0].begin() + tail));

    // NaN / Inf for one block: flushed within two control intervals, and the
    // band acts on the next burst again.
    auto nan = burstOverBed (3200.0, -40.0f, -40.0f);
    for (int i = static_cast<int> (0.5 * kFs); i < static_cast<int> (0.5 * kFs) + 256; ++i)
        nan.ch[0][static_cast<size_t> (i)] = (i % 2 == 0) ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    const auto [nanOut, nanGain] = run (nan, { 256 });
    bool finite = true;
    for (const auto& c : nanOut.ch)
        for (size_t i = static_cast<size_t> (0.5 * kFs + 256 + 2 * DynamicEq::kControlInterval); i < c.size(); ++i)
            finite = finite && std::isfinite (c[i]);
    CHECK (finite);
    CHECK (std::isfinite (nanGain));
    CHECK_GE (toDb (toneAmplitude (nanOut.ch[0].data() + static_cast<int> (0.81 * kFs), static_cast<int> (0.06 * kFs), 3200.0, kFs)) + 40.0, 6.0);
}

TEST_CASE ("Gaming: anti-masking band 6 no longer follows Footsteps - Footsteps 100 leaves a loud 90 Hz rumble alone; the presets carry the band as a user band (E20)")
{
    // docs/11 E20 Done-when: "Footsteps 100 no longer changes the explosion
    // level". Mode band 6 (the CutAbove low shelf at 90 Hz, 6 dB x Footsteps
    // before) is off in the Gaming mode policy until E21's Tame amount
    // exists to key it to.
    const double f = ProcessingChain::modeBandFrequency (ModeValue::Gaming, 6);
    const auto footsteps = [] (float amount) { return macroOnly (Macro1, amount, { CompressorOn }); };
    const auto loudRef = renderGaming (footsteps (0.0f), tone (f, -6.0f)), loud = renderGaming (footsteps (1.0f), tone (f, -6.0f));
    CHECK_LE (std::abs (loud.toneDb (0, f) - loudRef.toneDb (0, f)), 0.05);
    CHECK (loud.dynEqDb[6] == 0.0f);

    // The presets that tamed loud LF (Competitive FPS: 6 dB x its Footsteps
    // 0.8) store the same band as user band 0, so their explosion handling is
    // unchanged: -6 dBFS rumble is cut by the full 4.8 dB (half of it at the
    // shelf's corner), a -30 dBFS bass line not at all.
    const auto antiMasking = [] (bool bandOn) {
        return [bandOn] (ParameterStore& s) {
            s.set (dyn (0, DynFieldOn), bandOn ? 1.0f : 0.0f);
            s.set (dyn (0, DynFieldMode), 0.0f);  // Cut Above
            s.set (dyn (0, DynFieldShape), 1.0f); // Low Shelf
            s.set (dyn (0, DynFieldFreq), 90.0f);
            s.set (dyn (0, DynFieldQ), 0.7f);
            s.set (dyn (0, DynFieldThreshold), -22.0f);
            s.set (dyn (0, DynFieldRatio), 3.0f);
            s.set (dyn (0, DynFieldRange), 4.8f);
            s.set (dyn (0, DynFieldAttack), 10.0f);
            s.set (dyn (0, DynFieldRelease), 250.0f);
        };
    };
    const auto cutRef = renderGaming (antiMasking (false), tone (f, -6.0f)), cut = renderGaming (antiMasking (true), tone (f, -6.0f));
    CHECK_NEAR (cut.dynEqDb[0], -4.8, 0.1);
    CHECK_NEAR (cut.toneDb (0, f) - cutRef.toneDb (0, f), -2.4, 0.3);
    const auto quietRef = renderGaming (antiMasking (false), tone (f, -30.0f)), quiet = renderGaming (antiMasking (true), tone (f, -30.0f));
    CHECK_GE (quiet.dynEqDb[0], -0.01f);
    CHECK_LE (std::abs (quiet.toneDb (0, f) - quietRef.toneDb (0, f)), 0.05);
}

TEST_CASE ("Gaming Positional (M2): focus + width raise the ILD of an off-centre 3 kHz source; the mono sum is untouched")
{
    // Positional engages Stereo & Space and raises focus (0.9 x
    // smoothstep(0, 1)) and width (+0.25 x smoothstep(0.3, 1)).
    const auto positional = [] (float amount) { return macroOnly (Macro2, amount, { SpatialOn }); };
    {
        const auto e0 = effectiveAfterPrepare (positional (0.0f));
        CHECK (e0[SpatialOn] < 0.5f);
        CHECK (e0[SpatialFocus] == 0.0f);
        CHECK (e0[SpatialWidth] == 1.0f);
        const auto e5 = effectiveAfterPrepare (positional (0.5f));
        CHECK (e5[SpatialOn] >= 0.5f);
        CHECK_NEAR (e5[SpatialFocus], 0.45, 1e-5);
        CHECK_NEAR (e5[SpatialWidth], 1.0 + 0.25 * 0.198251, 1e-5);
        const auto e1 = effectiveAfterPrepare (positional (1.0f));
        CHECK_NEAR (e1[SpatialFocus], 0.9, 1e-5);
        CHECK_NEAR (e1[SpatialWidth], 1.25, 1e-5);
    }

    // A 3 kHz source panned left (R = L / 2: ILD 6.0 dB). M = 0.75, S = 0.25;
    // the spatializer scales S by width and by its focus bell (+3 dB x focus
    // at 3 kHz, docs/11 E24's cap) and leaves M alone.
    double ild[3] {}, monoSumDb[3] {};
    const float amounts[3] = { 0.0f, 0.5f, 1.0f };
    for (int k = 0; k < 3; ++k)
    {
        const auto r = renderGaming (positional (amounts[k]), tone (3000.0, -20.0f, 0.5f));
        ild[k] = r.toneDb (0, 3000.0) - r.toneDb (1, 3000.0);
        std::vector<float> sum (static_cast<size_t> (kMeasure));
        for (int i = 0; i < kMeasure; ++i)
            sum[static_cast<size_t> (i)] = r.out.ch[0][static_cast<size_t> (kLen - kMeasure + i)] + r.out.ch[1][static_cast<size_t> (kLen - kMeasure + i)];
        monoSumDb[k] = toDb (toneAmplitude (sum.data(), kMeasure, 3000.0, kFs));
        if (k == 2)
            CHECK_NEAR (r.effectiveWidth, 1.25, 1e-3); // no mono-safety pull on a correlated source
    }
    CHECK_NEAR (ild[0], toDb (2.0), 0.02); // macro at 0: transparent
    CHECK_GE (ild[1], ild[0] + 1.0);
    CHECK_GE (ild[2], ild[1] + 3.0);
    const double sGain = 1.25 * dbToGain (3.0f * 0.9f);
    CHECK_NEAR (ild[2], toDb ((0.75 + 0.25 * sGain) / (0.75 - 0.25 * sGain)), 0.5);
    // Mid/side only: the mono fold-down is exactly what it was.
    CHECK_NEAR (monoSumDb[1], monoSumDb[0], 0.01);
    CHECK_NEAR (monoSumDb[2], monoSumDb[0], 0.01);

    // A hard-left source stays hard-left: the focus's polarity guard never
    // lifts S beyond M, so no anti-phase copy reaches the far ear (without it
    // the ILD fell from infinite to about 10 dB at 100 %).
    const auto hard = renderGaming (positional (1.0f), tone (3000.0, -20.0f, 0.0f));
    CHECK_GE (hard.toneDb (0, 3000.0) - hard.toneDb (1, 3000.0), 60.0);
    CHECK_GE (hard.toneDb (0, 3000.0), -20.5);
}

namespace
{
void measured (const std::string& name, double value, const char* unit)
{
    std::printf ("    measured %s = %.2f %s\n", name.c_str(), value, unit);
}

/** docs/11 E20's scene, 3 s stereo: a steady LF noise rumble (the 40 - 150 Hz
    LR4 band of white noise, `rumbleDb` dBFS RMS) and from 1.5 s an explosion
    (a 45 Hz sine and noise under 250 Hz, 0.6 / 0.4, peak `explosionDb` dBFS,
    2 ms rise, 400 ms decay); explosionDb below -150: the rumble alone. */
Planar rumbleScene (float rumbleDb, float explosionDb)
{
    const int n = static_cast<int> (3.0 * kFs), onset = static_cast<int> (1.5 * kFs);
    auto rumble = whiteNoise (n, 1.0f, 77);
    auto noise = whiteNoise (n, 1.0f, 78);
    LinkwitzRileyBand band;
    band.prepare (kFs, 40.0, 150.0);
    LinkwitzRiley4 low;
    low.prepare (kFs);
    low.setFrequency (250.0);
    for (int i = 0; i < n; ++i)
    {
        float lo, hi;
        rumble[static_cast<size_t> (i)] = band.processSample (0, rumble[static_cast<size_t> (i)]);
        low.processSample (0, noise[static_cast<size_t> (i)], lo, hi);
        noise[static_cast<size_t> (i)] = lo;
    }
    const double rumbleGain = dbToGain (rumbleDb) / std::max (1.0e-12, rms (rumble.data(), n));
    const double noiseRms = std::max (1.0e-12, rms (noise.data() + onset, n - onset));
    std::vector<double> explosion (static_cast<size_t> (n), 0.0);
    double peak = 1.0e-12;
    for (int i = onset; i < n; ++i)
    {
        const double t = (i - onset) / kFs;
        const double env = std::min (1.0, t / 0.002) * std::exp (-t / 0.4);
        explosion[static_cast<size_t> (i)] = env * (0.6 * std::sin (kTwoPi * 45.0 * t) + 0.4 * noise[static_cast<size_t> (i)] / noiseRms);
        peak = std::max (peak, std::abs (explosion[static_cast<size_t> (i)]));
    }
    const double explosionGain = explosionDb > -150.0f ? dbToGain (explosionDb) / peak : 0.0;
    Planar p (2, n);
    for (int i = 0; i < n; ++i)
    {
        const auto v = static_cast<float> (rumbleGain * rumble[static_cast<size_t> (i)] + explosionGain * explosion[static_cast<size_t> (i)]);
        p.ch[0][static_cast<size_t> (i)] = p.ch[1][static_cast<size_t> (i)] = v;
    }
    return p;
}

/** Power (dB) of channel 0's 40 - 150 Hz LR4 band over [fromS, toS) of the
    input's time line (the output read `delay` samples later). */
double lfBandDb (const Planar& p, double fromS, double toS, int delay)
{
    LinkwitzRileyBand band;
    band.prepare (kFs, 40.0, 150.0);
    const int a = static_cast<int> (fromS * kFs) + delay, b = std::min (p.numSamples(), static_cast<int> (toS * kFs) + delay);
    double acc = 0.0;
    for (int i = 0; i < b; ++i)
    {
        const double y = band.processSample (0, p.ch[0][static_cast<size_t> (i)]);
        if (i >= a)
            acc += y * y;
    }
    return 10.0 * std::log10 (std::max (1.0e-30, acc / std::max (1, b - a)));
}
} // namespace

TEST_CASE ("Gaming Impact (M3): no static bass boost or harmonics; the attack goes to the shaper's low band and the bass engine's event-keyed punch (docs/11 E04 step 4, E20)")
{
    // Before (docs/11 E20 Why): Impact was a governed 6 dB low shelf x
    // smoothstep (0, 1), harmonics 0.25 x smoothstep (0.4, 1) and a full-band
    // attack of 4 dB x smoothstep (0.2, 1), which lifted a quiet rumble more
    // than the explosion. Now it engages the bass engine for its punch (the
    // chain sets BassEngineParams::impactPunch from Impact, governed) and
    // Clarity for the low band's attack.
    const auto impact = [] (float amount) { return macroOnly (Macro3, amount, { BassOn, ClarityOn }); };
    const auto e0 = effectiveAfterPrepare (impact (0.0f));
    CHECK (e0[BassOn] < 0.5f);
    CHECK (e0[ClarityOn] < 0.5f);
    for (float amount : { 0.5f, 1.0f })
    {
        const auto e = effectiveAfterPrepare (impact (amount));
        CHECK (e[BassOn] >= 0.5f);
        CHECK (e[ClarityOn] >= 0.5f);
        CHECK (e[BassBoostDb] == 0.0f);
        CHECK (e[BassHarmonics] == 0.0f);
        CHECK (e[ClarityAttackDb] == 0.0f);
        CHECK (e[ClarityAttackHighDb] == 0.0f);
    }
    CHECK_NEAR (effectiveAfterPrepare (impact (0.5f))[ClarityAttackLowDb], 4.0 * 0.316406, 1e-4); // x smoothstep (0.2, 1)
    CHECK_NEAR (effectiveAfterPrepare (impact (1.0f))[ClarityAttackLowDb], 4.0, 1e-4);

    // Footsteps (M1) and Detail (M4) reach the shaper's high band (docs/11
    // E04 step 4): 2 dB each at 100 %, from any amount; Footsteps engages
    // Clarity for it.
    const auto fs = effectiveAfterPrepare (macroOnly (Macro1, 1.0f, { ClarityOn }));
    CHECK (fs[ClarityOn] >= 0.5f);
    CHECK_NEAR (fs[ClarityAttackHighDb], 2.0, 1e-5);
    CHECK (fs[ClarityAttackDb] == 0.0f);
    CHECK_NEAR (effectiveAfterPrepare (macroOnly (Macro4, 1.0f, { ClarityOn }))[ClarityAttackHighDb], 2.0, 1e-5);
    CHECK_NEAR (effectiveAfterPrepare (macroOnly (Macro4, 0.5f, { ClarityOn }))[ClarityAttackHighDb], 1.0, 1e-5);
}

TEST_CASE ("Gaming Impact (M3) Done-when (docs/11 E20): an explosion's onset window gains >= 3 dB of LF, a steady rumble stays within 0.5 dB, the lift is gone in the tail, and a hot explosion stays under the ceiling")
{
    // Impact 100 against Impact 0, everything else default, on the scene
    // above: a -40 dBFS RMS rumble, an explosion peaking at -20 dBFS. The
    // 40 - 150 Hz band over the explosion's first 150 ms (onset window), over
    // 0.5 - 1.4 s (rumble only) and 300 - 800 ms after the onset (tail).
    // Before (the static shelf, CLI, same scene): rumble +2.40 dB, onset
    // window +6.15 dB, tail +4.09 dB - the rumble was lifted nearly as much
    // as the explosion.
    const auto impact = [] (float amount) { return macroOnly (Macro3, amount, {}); };
    const int delay = chainLatency (impact (1.0f), kFs);
    CHECK (delay == chainLatency (impact (0.0f), kFs));
    const auto in = rumbleScene (-40.0f, -20.0f);
    const auto off = renderGaming (impact (0.0f), in), on = renderGaming (impact (1.0f), in);
    const double onsetLift = lfBandDb (on.out, 1.5, 1.65, delay) - lfBandDb (off.out, 1.5, 1.65, delay);
    const double rumbleLift = lfBandDb (on.out, 0.5, 1.4, delay) - lfBandDb (off.out, 0.5, 1.4, delay);
    const double tailLift = lfBandDb (on.out, 1.8, 2.3, delay) - lfBandDb (off.out, 1.8, 2.3, delay);
    measured ("Impact 100, explosion onset window (0-150 ms) LF lift", onsetLift, "dB");
    measured ("Impact 100, rumble-only LF lift", rumbleLift, "dB");
    measured ("Impact 100, explosion tail (300-800 ms) LF lift", tailLift, "dB");
    CHECK_GE (onsetLift, 3.0);
    CHECK_LE (std::abs (rumbleLift), 0.5);
    CHECK_LE (tailLift, 1.0);
    CHECK_NEAR (onsetLift, 4.46, 0.3);

    // Headroom-reserved: an explosion peaking at -12 dBFS, its LF near
    // bass.protect (-12 dBFS), gets less of the burst while its peak lasts
    // (the next case reads the cap on the bass engine alone: the chain adds
    // the shaper's low-band attack, which is not reserved).
    const auto loudIn = rumbleScene (-36.0f, -12.0f);
    const auto loudOff = renderGaming (impact (0.0f), loudIn), loudOn = renderGaming (impact (1.0f), loudIn);
    const double loudLift = lfBandDb (loudOn.out, 1.5, 1.65, delay) - lfBandDb (loudOff.out, 1.5, 1.65, delay);
    measured ("Impact 100, -12 dBFS explosion onset window LF lift", loudLift, "dB");
    CHECK_LE (loudLift, onsetLift);

    // Ceiling held: Boost 50 (the maximizer on, ceiling -1 dBTP) and Impact
    // 100 on an explosion peaking at -1 dBFS; the output's sample peak stays
    // under the ceiling, as it does at Impact 0.
    const auto boosted = [] (float amount) {
        return [amount] (ParameterStore& s) {
            s.set (BoostIntensity, 0.5f);
            s.set (Macro3, amount);
        };
    };
    const auto hot = rumbleScene (-30.0f, -1.0f);
    const auto hotOff = renderGaming (boosted (0.0f), hot), hotOn = renderGaming (boosted (1.0f), hot);
    const double peakOff = toDb (std::max (peakAbs (hotOff.out.ch[0].data(), hot.numSamples()), peakAbs (hotOff.out.ch[1].data(), hot.numSamples())));
    const double peakOn = toDb (std::max (peakAbs (hotOn.out.ch[0].data(), hot.numSamples()), peakAbs (hotOn.out.ch[1].data(), hot.numSamples())));
    measured ("Boost 50 + Impact 0 / 100, -1 dBFS explosion: output peak (Impact 0)", peakOff, "dBFS");
    measured ("Boost 50 + Impact 0 / 100, -1 dBFS explosion: output peak (Impact 100)", peakOn, "dBFS");
    CHECK_LE (peakOn, -1.0 + 0.05);
    CHECK_LE (peakOff, -1.0 + 0.05);
}

TEST_CASE ("Gaming Impact (M3) and the automatic preamp (docs/11 E20): the preamp makes room for the burst - its model counts the 77.5 Hz bell and the burst's harmonics at a quiet programme's size, the steady prediction does not, and the onset's lift comes back by what the preamp takes")
{
    // Before: the preamp's model did not know the burst (Impact 100 against
    // 0, allowance 0: the same preamp, a change of 0.00 dB), so the
    // explosion's onset window kept its full +4.51 dB LF lift on top of a
    // preamp sized for the steady boosts. After: -6.22 dB, and -1.71 dB.
    const auto impact = [] (float amount, bool preamp) {
        return [amount, preamp] (ParameterStore& s) {
            s.set (Macro3, amount);
            s.set (AutoPreampOn, preamp ? 1.0f : 0.0f);
            s.set (AutoPreampAllowanceDb, 0.0f);
        };
    };
    const int delay = chainLatency (impact (1.0f, true), kFs);
    const auto in = rumbleScene (-40.0f, -20.0f);
    const auto off = renderGaming (impact (0.0f, true), in), on = renderGaming (impact (1.0f, true), in);
    const double preampDb = on.autoPreampDb - off.autoPreampDb;
    measured ("Impact 100 against 0: the automatic preamp's change", preampDb, "dB");
    measured ("Impact 100 against 0: the steady prediction's change", on.predictedBoostDb - off.predictedBoostDb, "dB");
    CHECK_LE (preampDb, -3.0);
    CHECK_GE (preampDb, -6.5); // the burst's 6 dB and its harmonics
    CHECK_NEAR (on.predictedBoostDb, off.predictedBoostDb, 0.01); // a steady programme never gets the burst
    // Impact 50 counts smoothstep (0.5) of the burst: less.
    const auto half = renderGaming (impact (0.5f, true), rumbleScene (-40.0f, -20.0f));
    CHECK_NEAR (half.autoPreampDb - off.autoPreampDb, 0.5 * preampDb, 0.3);

    // The onset window's LF lift with the preamp on is the lift without it
    // plus the preamp's change: the burst itself is unchanged.
    const double lift = lfBandDb (on.out, 1.5, 1.65, delay) - lfBandDb (off.out, 1.5, 1.65, delay);
    const auto bareOff = renderGaming (impact (0.0f, false), in), bareOn = renderGaming (impact (1.0f, false), in);
    const double bareLift = lfBandDb (bareOn.out, 1.5, 1.65, delay) - lfBandDb (bareOff.out, 1.5, 1.65, delay);
    measured ("Impact 100, explosion onset window LF lift, preamp on", lift, "dB");
    measured ("Impact 100, explosion onset window LF lift, preamp off", bareLift, "dB");
    CHECK_NEAR (lift, bareLift + preampDb, 0.5);

    // The model directly: only Gaming's M3 is a burst, and only the onset
    // model (the preamp's) counts it - a bell of 6 dB at 77.5 Hz on a quiet
    // programme, nothing at Music or with onsets off.
    ParameterStore store;
    for (int id : { GateOn, EqOn, DynEqOn, ClarityOn, SaturationOn, SpatialOn, VirtualizerOn, CompressorOn, MaximizerOn })
        store.set (id, 0.0f);
    store.set (BassOn, 1.0f);
    store.set (Macro3, 1.0f);
    std::vector<float> e (static_cast<size_t> (kNumParams));
    const auto modelAt77 = [&] (ModeValue mode, bool onsets) {
        store.set (Mode, static_cast<float> (mode));
        for (int i = 0; i < kNumParams; ++i)
            e[static_cast<size_t> (i)] = store.get (i);
        ProcessingChain::StaticBoostModel m;
        ProcessingChain::BoostModelContext ctx;
        ctx.programmeDb = ProcessingChain::kPreampQuietProgrammeDb;
        ctx.onsets = onsets;
        ProcessingChain::buildStaticBoostModel (e.data(), kFs, false, m, ctx);
        return m.responseDb (77.5);
    };
    measured ("the onset model at 77.5 Hz, Gaming Impact 100", modelAt77 (ModeValue::Gaming, true), "dB");
    CHECK_NEAR (modelAt77 (ModeValue::Gaming, true), 6.0, 0.3); // the bell, plus a little harmonics power
    CHECK_NEAR (modelAt77 (ModeValue::Gaming, false), 0.0, 0.01); // the default subsonic filter's skirt
    CHECK_NEAR (modelAt77 (ModeValue::Music, true), 0.0, 0.01);
}

TEST_CASE ("Gaming Impact (M3) and Smart macros (docs/11 E20, E34): on bass-heavy programme the burst takes the Smart bass multiplier like the bass rows, and so does the automatic preamp's model")
{
    // Before: Smart's bass multiplier scaled the macros' bass boost and
    // harmonics rows but not Impact's burst (it has no row), so on LF-heavy
    // programme Smart took nothing off it (this scene, Smart on: lift 4.50 dB,
    // preamp -6.22 dB, as static). The scene follows 3 s of the
    // rumble alone, so the analysis is valid and the multiplier has glided.
    struct Result
    {
        Planar out;
        float preampDb = 0.0f, bass = 1.0f;
    };
    const auto render = [] (float impact, bool smart, bool preamp) {
        ParameterStore store;
        store.set (Mode, static_cast<float> (ModeValue::Gaming));
        store.set (Macro3, impact);
        store.set (AutoPreampOn, preamp ? 1.0f : 0.0f);
        store.set (AutoPreampAllowanceDb, 0.0f);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, kBlock, 2 });
        chain.setSmartMacros (smart);
        ScopedNoDenormals noDenormals;
        Planar lead = rumbleScene (-40.0f, -150.0f);
        for (int pos = 0; pos < lead.numSamples(); pos += kBlock)
            chain.process (lead.block (pos, std::min (kBlock, lead.numSamples() - pos)));
        Result r { rumbleScene (-40.0f, -20.0f), 0.0f, 1.0f };
        for (int pos = 0; pos < r.out.numSamples(); pos += kBlock)
            chain.process (r.out.block (pos, std::min (kBlock, r.out.numSamples() - pos)));
        r.preampDb = chain.getAutoPreampDb();
        r.bass = chain.getSmartModulation().bass;
        return r;
    };
    const int delay = chainLatency (macroOnly (Macro3, 1.0f, {}), kFs);
    const auto lift = [delay] (const Result& on, const Result& off) {
        return lfBandDb (on.out, 1.5, 1.65, delay) - lfBandDb (off.out, 1.5, 1.65, delay);
    };
    const auto staticOn = render (1.0f, false, false), staticOff = render (0.0f, false, false);
    const auto smartOn = render (1.0f, true, false), smartOff = render (0.0f, true, false);
    measured ("Smart bass multiplier on the LF-heavy scene", smartOn.bass, "");
    CHECK_LE (smartOn.bass, 0.85f); // LF-heavy: Smart takes some of the macros' bass
    CHECK_GE (smartOn.bass, 1.0f - MacroMap::kSmartBassCut - 0.01f);
    const double staticLift = lift (staticOn, staticOff), smartLift = lift (smartOn, smartOff);
    measured ("Impact 100, onset window LF lift, static", staticLift, "dB");
    measured ("Impact 100, onset window LF lift, Smart", smartLift, "dB");
    CHECK_GE (staticLift, 3.0);
    CHECK_LE (smartLift, staticLift - 0.15);
    CHECK_GE (smartLift, 3.0);

    // The preamp's model counts the burst at the same multiplier: the bell
    // of 6 dB x the multiplier, and a little harmonics power.
    const double preampDb = render (1.0f, true, true).preampDb - render (0.0f, true, true).preampDb;
    measured ("Impact 100 against 0 with Smart: the automatic preamp's change", preampDb, "dB");
    CHECK_NEAR (preampDb, -(6.0 * smartOn.bass + 0.2), 0.3);
}

TEST_CASE ("BassEngine Impact punch (docs/11 E20): switching it is click-free, the burst keys on onsets only and stays under bass.protect, the output does not depend on the block size, a NaN burst does not stick, and nothing allocates")
{
    // A 60 Hz tone at -40 dBFS (steady: no onset) with an explosion-like hit
    // at 0.5 s (from the scene above, peak -20 dBFS).
    const auto hit = rumbleScene (-150.0f, -20.0f);
    const int n = static_cast<int> (1.5 * kFs);
    Planar in (2, n);
    const auto tone60 = sine (60.0, kFs, n, dbToGain (-40.0f));
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i)
            in.ch[static_cast<size_t> (c)][static_cast<size_t> (i)] = tone60[static_cast<size_t> (i)] + hit.ch[0][static_cast<size_t> (i + static_cast<int> (1.0 * kFs))];

    const auto run = [&] (const Planar& x, int blockSize, auto&& perBlock, float* maxImpactDb = nullptr) {
        BassEngine be;
        be.prepare ({ kFs, 512, 2 });
        Planar y = x;
        float maxDb = 0.0f;
        for (int pos = 0, b = 0; pos < n; pos += blockSize, ++b)
        {
            perBlock (be, pos);
            be.process (y.block (pos, std::min (blockSize, n - pos)));
            maxDb = std::max (maxDb, be.getImpactDb());
        }
        if (maxImpactDb != nullptr)
            *maxImpactDb = maxDb;
        return y;
    };
    const auto withPunch = [] (float punch) {
        return [punch] (BassEngine& be, int) {
            BassEngineParams p;
            p.impactPunch = punch;
            be.setParams (p);
        };
    };

    // The hit keys a burst (up to 6 dB); the steady tone alone does not.
    float burstDb = 0.0f, steadyDb = 0.0f;
    const auto a = run (in, 256, withPunch (1.0f), &burstDb);
    Planar toneOnly (2, n);
    for (int c = 0; c < 2; ++c)
        toneOnly.ch[static_cast<size_t> (c)] = tone60;
    run (toneOnly, 256, withPunch (1.0f), &steadyDb);
    measured ("Impact punch: largest lift on the hit / on the steady tone", burstDb, "dB");
    measured ("  on the steady tone", steadyDb, "dB");
    CHECK_GE (burstDb, 5.0f);
    CHECK_LE (burstDb, 6.0f);
    CHECK_LE (steadyDb, 0.05f);

    // Block-size independent (the stage runs per sample).
    const auto b = run (in, 61, withPunch (1.0f));
    CHECK (maxAbsDiff (a, b) == 0.0);

    // Nothing above 3 kHz (the harmonics end at 6 x 120 Hz), measured
    // through four one-pole high-passes: switching Impact on and off on the
    // steady tone is click-free, and the burst on the hit (a second-order
    // rise of the lift and of the harmonics mix) leaves only the harmonics
    // generator's own splatter, 45 dB under the hit.
    const auto hfPeakDb = [&] (const Planar& y) {
        double hf = 0.0;
        std::array<double, 4> z {};
        const double g = std::tan (kPi * 3000.0 / kFs), G = g / (1.0 + g);
        for (int i = 0; i < n; ++i)
        {
            double v = y.ch[0][static_cast<size_t> (i)];
            for (auto& s : z)
            {
                const double w = (v - s) * G, lp = w + s;
                s = lp + w;
                v -= lp;
            }
            if (i > static_cast<int> (0.05 * kFs))
                hf = std::max (hf, std::abs (v));
        }
        return toDb (hf);
    };
    const auto toggle = [] (BassEngine& be, int pos) {
        BassEngineParams p;
        p.impactPunch = pos < static_cast<int> (0.3 * kFs) ? 0.0f : (pos < static_cast<int> (0.7 * kFs) ? 1.0f : (pos < static_cast<int> (1.0 * kFs) ? 0.0f : 0.6f));
        be.setParams (p);
    };
    const double switchedDb = hfPeakDb (run (toneOnly, 256, toggle)), burstHfDb = hfPeakDb (run (in, 256, toggle));
    measured ("Impact punch switched on the steady tone: largest content above 3 kHz", switchedDb, "dBFS");
    measured ("Impact punch switched with a burst on the hit: largest content above 3 kHz", burstHfDb, "dBFS");
    CHECK_LE (switchedDb, -100.0);
    CHECK_LE (burstHfDb, -65.0);

    // Headroom: the same hit 20 dB louder (its LF over bass.protect's
    // -12 dBFS) gets almost none of the lift.
    Planar loud = in;
    for (auto& c : loud.ch)
        for (auto& v : c)
            v *= 10.0f;
    float loudDb = 0.0f;
    run (loud, 256, withPunch (1.0f), &loudDb);
    measured ("Impact punch: largest lift on the hit 20 dB louder", loudDb, "dB");
    CHECK_LE (loudDb, 1.0f);

    // A NaN burst resets the stage (the engine's non-finite guard) and the
    // output is finite and back to the undisturbed render 0.5 s later.
    Planar nan = in;
    for (int i = static_cast<int> (0.2 * kFs); i < static_cast<int> (0.2 * kFs) + 64; ++i)
        nan.ch[0][static_cast<size_t> (i)] = std::numeric_limits<float>::quiet_NaN();
    const auto c = run (nan, 256, withPunch (1.0f));
    bool finite = true;
    for (int i = static_cast<int> (0.3 * kFs); i < n; ++i)
        finite = finite && std::isfinite (c.ch[0][static_cast<size_t> (i)]) && std::isfinite (c.ch[1][static_cast<size_t> (i)]);
    CHECK (finite);

    // process / setParams / reset allocate nothing.
    BassEngine be;
    be.prepare ({ kFs, 512, 2 });
    Planar buf = in;
    flubtest::AllocationGuard guard;
    be.reset();
    for (int i = 0; i < 40; ++i)
    {
        BassEngineParams p;
        p.impactPunch = static_cast<float> (i % 4) / 3.0f;
        p.harmonicsAmount = i % 5 == 0 ? 0.3f : 0.0f;
        be.setParams (p);
        be.process (buf.block (i * 512 % (n - 512), 1 + i * 37 % 512));
    }
    CHECK (guard.allocations() == 0);
}

TEST_CASE ("Gaming Detail (M4): upward compression lifts quiet cues by its law (up to 8 dB) over the programme's background; a steady sound and loud ones are not lifted")
{
    const auto detail = [] (float amount) { return macroOnly (Macro4, amount, { CompressorOn, ClarityOn }); };
    {
        const auto e0 = effectiveAfterPrepare (detail (0.0f));
        CHECK (e0[CompressorOn] < 0.5f);
        CHECK (e0[CompUpMaxGainDb] == 0.0f);
        const auto e5 = effectiveAfterPrepare (detail (0.5f));
        CHECK (e5[CompressorOn] >= 0.5f);
        CHECK (e5[ClarityOn] >= 0.5f);
        CHECK_NEAR (e5[CompUpMaxGainDb], 4.0, 1e-4);        // 8 dB x smoothstep(0, 1)
        CHECK_NEAR (e5[ClarityAir], 0.4 * 0.316406, 1e-5);  // x smoothstep(0.2, 1)
        const auto e1 = effectiveAfterPrepare (detail (1.0f));
        CHECK_NEAR (e1[CompUpMaxGainDb], 8.0, 1e-4);
        CHECK_NEAR (e1[ClarityAir], 0.4, 1e-5);
    }
    // Upward threshold -45 dB, ratio 2: a -60 dBFS cue is lifted by
    // (1 - 1/2) x 15 = 7.5 dB (under the 8 dB cap); a -10 dBFS one is not.
    // In Gaming the upward floor follows the programme's background (docs/11
    // E19, Compressor upRelativeFloor): the cue here rises out of 0.4 s of
    // silence (after the background's 300 ms learning time), where the
    // background is the -75 dB floor, and a tone this far over it only
    // becomes background after about 1.2 s (5 dB/s). The lift rises at the
    // 120 ms release, so the last 0.5 s average under the law (about 6.7 dB);
    // the meter (last block) reads it.
    auto cue = tone (1000.0, -60.0f);
    for (auto& c : cue.ch)
        std::fill_n (c.begin(), kLen * 2 / 5, 0.0f);
    const auto quietRef = renderGaming (detail (0.0f), cue);
    const auto quiet = renderGaming (detail (1.0f), cue);
    CHECK_NEAR (quiet.toneDb (0, 1000.0) - quietRef.toneDb (0, 1000.0), 6.7, 0.3);
    CHECK_NEAR (quiet.compUpwardDb, 7.5, 0.3);
    // The same tone from the first sample is its own background: not lifted
    // (the fixed -75 dB floor lifted it by the full 7.5 dB).
    const auto steadyRef = renderGaming (detail (0.0f), tone (1000.0, -60.0f));
    const auto steady = renderGaming (detail (1.0f), tone (1000.0, -60.0f));
    CHECK_NEAR (steady.toneDb (0, 1000.0) - steadyRef.toneDb (0, 1000.0), 0.0, 0.3);
    CHECK_LE (steady.compUpwardDb, 0.3f);
    // Music mode keeps the fixed floor (Late Night lifts quiet passages).
    const auto music = renderGaming ([] (ParameterStore& s) {
        s.set (Mode, static_cast<float> (ModeValue::Music));
        s.set (CompressorOn, 1.0f);
        s.set (CompRatio, 1.0f);
        s.set (CompUpMaxGainDb, 8.0f);
    }, tone (1000.0, -60.0f), { DynEqOn, ClarityOn, BassOn, SpatialOn, SaturationOn, MaximizerOn });
    CHECK_NEAR (music.compUpwardDb, 7.5, 0.3);
    const auto loudRef = renderGaming (detail (0.0f), tone (1000.0, -10.0f));
    const auto loud = renderGaming (detail (1.0f), tone (1000.0, -10.0f));
    CHECK_LE (loud.toneDb (0, 1000.0) - loudRef.toneDb (0, 1000.0), 0.05);
    CHECK_LE (loud.compUpwardDb, 0.01f);
}

TEST_CASE ("Gaming Voice & Score (M5): mode band 7 lifts quiet 2 kHz dialogue by up to 4 dB; presence adds on top")
{
    // Mode band 7: BoostBelow bell at 2 kHz, threshold -36 dB, ratio 2,
    // range 4 dB x Voice. Clarity (presence / de-mud) is held off to measure
    // the band alone.
    const auto voice = [] (float amount) { return macroOnly (Macro5, amount, { DynEqOn, ClarityOn }); };
    {
        const auto e0 = effectiveAfterPrepare (voice (0.0f));
        CHECK (e0[DynEqOn] < 0.5f);
        CHECK (e0[ClarityOn] < 0.5f);
        const auto e5 = effectiveAfterPrepare (voice (0.5f));
        CHECK (e5[DynEqOn] >= 0.5f);
        CHECK (e5[ClarityOn] >= 0.5f);
        CHECK_NEAR (e5[ClarityPresence], 0.35, 1e-5);
        CHECK_NEAR (e5[ClarityDeMud], 0.4 * 0.316406, 1e-5);
        const auto e1 = effectiveAfterPrepare (voice (1.0f));
        CHECK_NEAR (e1[ClarityPresence], 0.7, 1e-5);
        CHECK_NEAR (e1[ClarityDeMud], 0.4, 1e-5);
    }
    const double f = ProcessingChain::modeBandFrequency (ModeValue::Gaming, 7);
    // -45 dBFS: 9 dB under the threshold asks for 4.5 dB, the range caps it at 4.
    const auto quietRef = renderGaming (voice (0.0f), tone (f, -45.0f), { ClarityOn });
    const auto quiet = renderGaming (voice (1.0f), tone (f, -45.0f), { ClarityOn });
    const double bandLift = quiet.toneDb (0, f) - quietRef.toneDb (0, f);
    CHECK_NEAR (bandLift, 4.0, 0.2);
    CHECK_NEAR (quiet.dynEqDb[7], 4.0, 0.1);
    // Loud dialogue (-12 dBFS) is left alone.
    const auto loudRef = renderGaming (voice (0.0f), tone (f, -12.0f), { ClarityOn });
    const auto loud = renderGaming (voice (1.0f), tone (f, -12.0f), { ClarityOn });
    CHECK_LE (std::abs (loud.toneDb (0, f) - loudRef.toneDb (0, f)), 0.05);
    CHECK_LE (loud.dynEqDb[7], 0.01f);
    // The whole macro: Clarity's presence lift adds to the band.
    const auto full = renderGaming (voice (1.0f), tone (f, -45.0f));
    CHECK_GE (full.toneDb (0, f) - quietRef.toneDb (0, f), bandLift + 1.0);
}

TEST_CASE ("Gaming: crossfeed is forced off - a hard-left source never leaks into the right ear, whatever the store says")
{
    // Crossfeed feeds low-passed S back (bs2b-like), which blurs lateral
    // cues; the chain zeroes it in Gaming mode. Hard-left 300 Hz, crossfeed
    // stored at 100 %.
    const auto render = [] (ModeValue mode, float crossfeed, float positional) {
        return renderGaming (
            [=] (ParameterStore& s) {
                s.set (Mode, static_cast<float> (mode));
                s.set (SpatialCrossfeed, crossfeed);
                s.set (Macro2, positional);
            },
            tone (300.0, -20.0f, 0.0f));
    };
    // Music honours it: the right ear hears the left source.
    const auto music = render (ModeValue::Music, 1.0f, 0.0f);
    CHECK_GE (music.toneDb (1, 300.0), -40.0);
    // Gaming: the right ear stays silent and the left is untouched.
    const auto gaming = render (ModeValue::Gaming, 1.0f, 0.0f);
    CHECK_LE (peakAbs (gaming.out.ch[1].data(), kLen), 1e-6);
    CHECK_NEAR (gaming.toneDb (0, 300.0), -20.0, 0.05);
    // With spatial processing active (Positional) the right ear carries only
    // what width / focus put there: identical to crossfeed stored at 0.
    const auto withCrossfeed = render (ModeValue::Gaming, 1.0f, 1.0f);
    const auto withoutCrossfeed = render (ModeValue::Gaming, 0.0f, 1.0f);
    CHECK (maxAbsDiff (withCrossfeed.out, withoutCrossfeed.out) == 0.0);
}

TEST_CASE ("Gaming: binaural lock on a 7.1 strip - width 1, space 0 and focus 0 whatever the store asks")
{
    // With the virtualiser on, the stereo after it is binaural: widening,
    // ambience, crossfeed and the focus ILD bell would corrupt its interaural
    // cues, so the chain forces width 1 / space 0 / crossfeed 0 / focus 0
    // into the spatializer (docs/11 E24 (i); focus was allowed before). The
    // published effective values and the spatializer's own width meter both
    // show what is applied.
    Planar in (8, kLen);
    const double freqs[8] = { 440.0, 550.0, 700.0, 0.0, 1300.0, 2300.0, 3100.0, 1700.0 }; // FL FR FC LFE BL BR SL SR
    const float levels[8] = { 0.10f, 0.08f, 0.10f, 0.0f, 0.05f, 0.05f, 0.10f, 0.07f };
    for (size_t c = 0; c < 8; ++c)
        if (freqs[c] > 0.0)
        {
            const auto s = sine (freqs[c], kFs, kLen, levels[c]);
            std::copy (s.begin(), s.end(), in.ch[c].begin());
        }
    const auto render = [&in] (bool virt, float width, float space, float crossfeed, float positional) {
        return renderGaming (
            [=] (ParameterStore& s) {
                s.set (VirtualizerOn, virt ? 1.0f : 0.0f);
                s.set (SpatialWidth, width);
                s.set (SpatialSpace, space);
                s.set (SpatialCrossfeed, crossfeed);
                s.set (SpatialMonoSafety, 0.0f);
                s.set (Macro2, positional);
            },
            in);
    };

    const auto wide = render (true, 2.0f, 1.0f, 1.0f, 1.0f);
    const auto plain = render (true, 1.0f, 0.0f, 0.0f, 1.0f);
    // The published effective values show what is applied, not what the store
    // asked for (the GUI's post-macro markers must not show a locked width).
    CHECK (wide.eff (SpatialWidth) == 1.0f);
    CHECK (wide.eff (SpatialSpace) == 0.0f);
    CHECK (wide.eff (SpatialCrossfeed) == 0.0f);
    CHECK_NEAR (wide.effectiveWidth, 1.0, 1e-4); // applied
    CHECK (maxAbsDiff (wide.out, plain.out) == 0.0);
    CHECK (rms (plain.out.ch[0].data(), kLen) > 0.01);

    // Focus is locked too: Positional 100 % (focus 0.9 from the macro) no
    // longer changes the binaural output at all.
    const auto noFocus = render (true, 1.0f, 0.0f, 0.0f, 0.0f);
    CHECK (plain.eff (SpatialFocus) == 0.0f);
    CHECK (maxAbsDiff (plain.out, noFocus.out) == 0.0);

    // Contrast: the BS.775 downmix (virtualiser off) is plain stereo, and the
    // same store widens it and adds space.
    const auto wideDownmix = render (false, 2.0f, 1.0f, 1.0f, 1.0f);
    const auto plainDownmix = render (false, 1.0f, 0.0f, 0.0f, 1.0f);
    CHECK_NEAR (wideDownmix.effectiveWidth, 2.0, 1e-3);
    CHECK_GE (rmsDiff (wideDownmix.out, plainDownmix.out), 0.1 * rms (plainDownmix.out.ch[0].data(), kLen));
    CHECK (plainDownmix.eff (SpatialFocus) > 0.8f); // no lock: focus applies
}

TEST_CASE ("Gaming: a compressor switched on only by a macro is upward-only - loud sounds keep their dynamics unless a ratio was chosen")
{
    // Detail switches the compressor on for its upward section (the only
    // Gaming macro that still does, docs/11 E19). With
    // comp.ratio left at its default the downward section stays off, so a
    // loud -10 dBFS "gunshot" tone passes at its own level; a preset that
    // chose a ratio (1.5:1 here) keeps it, and so does a compressor the user
    // switched on.
    const std::initializer_list<int> held { DynEqOn, ClarityOn, BassOn, SpatialOn, SaturationOn, MaximizerOn };
    const auto loud = tone (1000.0, -10.0f);
    const auto off = renderGaming (macroOnly (Macro4, 0.0f, { CompressorOn }), loud, held);
    const auto macro = renderGaming (macroOnly (Macro4, 1.0f, { CompressorOn }), loud, held);
    CHECK (macro.eff (CompressorOn) >= 0.5f);
    CHECK (macro.eff (CompRatio) == 1.0f);
    CHECK_NEAR (macro.toneDb (0, 1000.0), off.toneDb (0, 1000.0), 0.2);

    const auto chosen = renderGaming ([] (ParameterStore& s) {
        macroOnly (Macro4, 1.0f, { CompressorOn }) (s);
        s.set (CompRatio, 1.5f);
    }, loud, held);
    CHECK (chosen.eff (CompRatio) == 1.5f);
    CHECK (chosen.toneDb (0, 1000.0) < off.toneDb (0, 1000.0) - 1.0); // downward 1.5:1 above -18 dB

    // Rendered (not just prepared), so the chain's per-block override has run.
    const auto userOn = renderGaming ([] (ParameterStore& s) {
        s.set (CompressorOn, 1.0f);
        s.set (Macro4, 1.0f);
    }, loud, held);
    CHECK (userOn.eff (CompRatio) == layout()[static_cast<size_t> (CompRatio)].defaultValue);
    CHECK (userOn.toneDb (0, 1000.0) < off.toneDb (0, 1000.0) - 1.0); // the user's 2.5:1 compresses
}
