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
#include "flub/dsp/DynamicEq.h"
#include "flub/engine/ProcessingChain.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <limits>
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

TEST_CASE ("Gaming Impact (M3): governed bass boost + harmonics and transient attack; a sub-bass hit gains level and harmonics")
{
    const auto impact = [] (float amount) { return macroOnly (Macro3, amount, { BassOn, ClarityOn }); };
    {
        const auto e0 = effectiveAfterPrepare (impact (0.0f));
        CHECK (e0[BassOn] < 0.5f);
        CHECK (e0[ClarityOn] < 0.5f);
        const auto e5 = effectiveAfterPrepare (impact (0.5f));
        CHECK (e5[BassOn] >= 0.5f);
        CHECK (e5[ClarityOn] >= 0.5f);
        CHECK_NEAR (e5[BassBoostDb], 3.0, 1e-4);                 // 6 dB x smoothstep(0, 1)
        CHECK_NEAR (e5[BassHarmonics], 0.25 * 0.0740741, 1e-5);  // x smoothstep(0.4, 1)
        CHECK_NEAR (e5[ClarityAttackDb], 4.0 * 0.316406, 1e-4);  // x smoothstep(0.2, 1)
        const auto e1 = effectiveAfterPrepare (impact (1.0f));
        CHECK_NEAR (e1[BassBoostDb], 6.0, 1e-4);
        CHECK_NEAR (e1[BassHarmonics], 0.25, 1e-5);
        CHECK_NEAR (e1[ClarityAttackDb], 4.0, 1e-4);
    }
    // A -30 dBFS 50 Hz rumble: the low shelf (corner 70 Hz) lifts it by most
    // of its 6 dB, and the harmonic generator adds its 3rd harmonic (150 Hz)
    // where there was none.
    const auto off = renderGaming (impact (0.0f), tone (50.0, -30.0f));
    const auto on = renderGaming (impact (1.0f), tone (50.0, -30.0f));
    const double boost = on.toneDb (0, 50.0) - off.toneDb (0, 50.0);
    CHECK_GE (boost, 3.5);
    CHECK_LE (boost, 6.05);
    CHECK_LE (off.toneDb (0, 150.0), -130.0);
    CHECK_GE (on.toneDb (0, 150.0) - on.toneDb (0, 50.0), -20.0);
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
