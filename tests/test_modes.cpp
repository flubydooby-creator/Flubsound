// Gaming mode (R2.9) through the full ProcessingChain: what each Gaming macro
// does to the effective parameters (MacroMap's kGamingTable) and to the sound
// (the internal dynamic-EQ mode bands of configureModeBands and the mode /
// binaural policy in ProcessingChain::applyParameters).
//
// Audible checks measure the steady-state level of test tones in the last
// 0.5 s of a 1 s render (integer periods of every tone used), always against
// the same chain with the macro at 0. Where a macro engages several modules,
// the GUI's audition bypass holds the others off so one mechanism is
// measured at a time.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/common/Denormals.h"
#include "flub/engine/ProcessingChain.h"

#include <array>
#include <functional>
#include <initializer_list>

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

    /** Level (dBFS) of the component at `freq` in output channel `ch`. */
    double toneDb (int ch, double freq) const
    {
        return toDb (toneAmplitude (out.ch[static_cast<size_t> (ch)].data() + out.numSamples() - kMeasure, kMeasure, freq, kFs));
    }
    float eff (int id) const { return effective[static_cast<size_t> (id)]; }
};

/** Renders `in` through a fresh chain in Gaming mode. `setup` adjusts the
    store before prepare(); `held` modules are held off with the audition
    bypass. */
Render renderGaming (const Setup& setup, Planar in, std::initializer_list<int> held = {})
{
    ParameterStore store;
    store.set (Mode, static_cast<float> (ModeValue::Gaming));
    setup (store);
    ProcessingChain chain (store);
    chain.prepare ({ kFs, kBlock, in.numChannels() });
    for (int id : held)
        chain.setAuditionBypass (id, true);
    {
        ScopedNoDenormals noDenormals;
        const int n = in.numSamples();
        for (int pos = 0; pos < n; pos += kBlock)
            chain.process (in.block (pos, std::min (kBlock, n - pos)));
    }
    Render r { std::move (in), {}, 0.0f, 0.0f, {} };
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

/** Stereo sine at `dbfs` on the left; the right channel is `right` x left. */
Planar tone (double freq, float dbfs, float right = 1.0f)
{
    Planar p (2, kLen);
    const auto s = sine (freq, kFs, kLen, dbToGain (dbfs));
    for (int i = 0; i < kLen; ++i)
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
} // namespace

// ---------------------------------------------------------------------------
TEST_CASE ("Gaming Footsteps (M1): mode band 4 lifts quiet 3.2 kHz detail by its gain law; loud steps are not boosted")
{
    // Mode band 4: BoostBelow bell at 3.2 kHz, threshold -42 dB, ratio 3,
    // range 7 dB x Footsteps (upward compression of quiet high detail).
    CHECK (ProcessingChain::modeBandFrequency (ModeValue::Gaming, 4) == 3200.0f);
    CHECK (ProcessingChain::modeBandFrequency (ModeValue::Gaming, 5) == 260.0f);
    CHECK (ProcessingChain::modeBandFrequency (ModeValue::Gaming, 6) == 90.0f);
    CHECK (ProcessingChain::modeBandFrequency (ModeValue::Gaming, 7) == 2000.0f);
    CHECK (ProcessingChain::modeBandFrequency (ModeValue::Gaming, 3) == 0.0f); // a user band
    CHECK (ProcessingChain::modeBandFrequency (ModeValue::Gaming, 8) == 0.0f);
    const double f = ProcessingChain::modeBandFrequency (ModeValue::Gaming, 4);

    // Footsteps engages the dynamic EQ and the compressor (upward, max
    // 3 dB x smoothstep(0.3, 1)) even when both are stored off.
    const auto footsteps = [] (float amount) { return macroOnly (Macro1, amount, { DynEqOn, CompressorOn }); };
    {
        const auto e0 = effectiveAfterPrepare (footsteps (0.0f));
        CHECK (e0[DynEqOn] < 0.5f);
        CHECK (e0[CompressorOn] < 0.5f);
        const auto e5 = effectiveAfterPrepare (footsteps (0.5f));
        CHECK (e5[DynEqOn] >= 0.5f);
        CHECK (e5[CompressorOn] >= 0.5f);
        CHECK_NEAR (e5[CompUpMaxGainDb], 3.0 * 0.198251, 1e-4);
        const auto e1 = effectiveAfterPrepare (footsteps (1.0f));
        CHECK_NEAR (e1[CompUpMaxGainDb], 3.0, 1e-4);
    }

    // The band on its own (compressor held off): lift of the 3.2 kHz tone
    // against Footsteps at 0.
    const auto lift = [&] (float amount, float levelDb, std::initializer_list<int> held, Render* keep = nullptr) {
        const auto ref = renderGaming (footsteps (0.0f), tone (f, levelDb), held);
        auto r = renderGaming (footsteps (amount), tone (f, levelDb), held);
        const double l = r.toneDb (0, f) - ref.toneDb (0, f);
        CHECK_NEAR (r.toneDb (1, f) - ref.toneDb (1, f), l, 0.01); // stereo-linked: both ears alike
        if (keep != nullptr)
            *keep = std::move (r);
        return l;
    };
    Render r { Planar (2, 1), {}, 0.0f, 0.0f, {} };
    // -50 dBFS: 8 dB under the threshold -> (1 - 1/3) x 8 = +5.3 dB.
    CHECK_NEAR (lift (1.0f, -50.0f, { CompressorOn }, &r), 8.0 * (1.0 - 1.0 / 3.0), 0.3);
    CHECK_NEAR (r.dynEqDb[4], 8.0 * (1.0 - 1.0 / 3.0), 0.3);
    // -60 dBFS: the law asks for +12 dB, the range (7 dB x Footsteps) caps it.
    CHECK_NEAR (lift (1.0f, -60.0f, { CompressorOn }), 7.0, 0.2);
    CHECK_NEAR (lift (0.5f, -60.0f, { CompressorOn }), 3.5, 0.2);
    // A loud step (-10 dBFS, far above the threshold) is not boosted.
    CHECK_LE (std::abs (lift (1.0f, -10.0f, { CompressorOn }, &r)), 0.05);
    CHECK_LE (r.dynEqDb[4], 0.01f);

    // The whole macro: the compressor's upward gain (3 dB at 100 %) adds to
    // the band's 7 dB on a -60 dBFS cue.
    CHECK_NEAR (lift (1.0f, -60.0f, {}, &r), 10.0, 0.3);
    CHECK_NEAR (r.compUpwardDb, 3.0, 0.2);
}

TEST_CASE ("Gaming Footsteps (M1): anti-masking band 6 tames a very loud 90 Hz rumble; normal bass is untouched")
{
    // Mode band 6: CutAbove low shelf at 90 Hz, threshold -22 dB, ratio 3,
    // range 6 dB x Footsteps. The compressor Footsteps also engages is held
    // off so only the band is measured.
    const double f = ProcessingChain::modeBandFrequency (ModeValue::Gaming, 6);
    const auto render = [f] (float amount, float levelDb) {
        return renderGaming (macroOnly (Macro1, amount, { CompressorOn }), tone (f, levelDb), { CompressorOn });
    };
    // -6 dBFS explosion rumble: the full 6 dB cut, which a shelf applies half
    // of at its corner frequency.
    const auto loudRef = render (0.0f, -6.0f), loud = render (1.0f, -6.0f);
    CHECK_NEAR (loud.dynEqDb[6], -6.0, 0.1);
    CHECK_NEAR (loud.toneDb (0, f) - loudRef.toneDb (0, f), -3.0, 0.3);
    CHECK_NEAR (loud.toneDb (1, f) - loudRef.toneDb (1, f), -3.0, 0.3);
    // -30 dBFS bass line: below the threshold, so no cut at all.
    const auto normalRef = render (0.0f, -30.0f), normal = render (1.0f, -30.0f);
    CHECK_GE (normal.dynEqDb[6], -0.01f);
    CHECK_LE (std::abs (normal.toneDb (0, f) - normalRef.toneDb (0, f)), 0.05);
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
    // the spatializer scales S by width and by its focus bell (+6 dB x focus
    // at 3 kHz) and leaves M alone.
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
    CHECK_GE (ild[1], ild[0] + 2.0);
    CHECK_GE (ild[2], ild[1] + 5.0);
    const double sGain = 1.25 * dbToGain (6.0f * 0.9f);
    CHECK_NEAR (ild[2], toDb ((0.75 + 0.25 * sGain) / (0.75 - 0.25 * sGain)), 0.5);
    // Mid/side only: the mono fold-down is exactly what it was.
    CHECK_NEAR (monoSumDb[1], monoSumDb[0], 0.01);
    CHECK_NEAR (monoSumDb[2], monoSumDb[0], 0.01);

    // A hard-left source stays clearly on its side. (Raising S necessarily
    // puts an anti-phase copy in the far ear, so its ILD drops from infinite
    // to about 10 dB at 100 %; it must not collapse towards the centre.)
    const auto hard = renderGaming (positional (1.0f), tone (3000.0, -20.0f, 0.0f));
    CHECK_GE (hard.toneDb (0, 3000.0) - hard.toneDb (1, 3000.0), 6.0);
    CHECK_GE (hard.toneDb (0, 3000.0), -20.0);
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

TEST_CASE ("Gaming Detail (M4): upward compression lifts quiet cues by its law (up to 8 dB); loud ones are not lifted")
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
    const auto quietRef = renderGaming (detail (0.0f), tone (1000.0, -60.0f));
    const auto quiet = renderGaming (detail (1.0f), tone (1000.0, -60.0f));
    CHECK_NEAR (quiet.toneDb (0, 1000.0) - quietRef.toneDb (0, 1000.0), 7.5, 0.3);
    CHECK_NEAR (quiet.compUpwardDb, 7.5, 0.3);
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

TEST_CASE ("Gaming: binaural lock on a 7.1 strip - width 1 and space 0 whatever the store asks, positional focus still applies")
{
    // With the virtualiser on, the stereo after it is binaural: widening,
    // ambience and crossfeed would corrupt its interaural cues, so the chain
    // forces width 1 / space 0 / crossfeed 0 into the spatializer (focus is
    // allowed). The effective values still show what the store and macros
    // ask for; the spatializer's own width meter shows what it applies.
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
    CHECK (wide.eff (SpatialWidth) == 2.0f); // asked for (clamped to the range)
    CHECK (wide.eff (SpatialSpace) == 1.0f);
    CHECK_NEAR (wide.effectiveWidth, 1.0, 1e-4); // applied
    CHECK (maxAbsDiff (wide.out, plain.out) == 0.0);
    CHECK (rms (plain.out.ch[0].data(), kLen) > 0.01);

    // Focus is not locked: Positional still changes the binaural output.
    const auto noFocus = render (true, 1.0f, 0.0f, 0.0f, 0.0f);
    CHECK (plain.eff (SpatialFocus) > 0.8f);
    CHECK_GE (rmsDiff (plain.out, noFocus.out), 0.01 * rms (plain.out.ch[0].data(), kLen));

    // Contrast: the BS.775 downmix (virtualiser off) is plain stereo, and the
    // same store widens it and adds space.
    const auto wideDownmix = render (false, 2.0f, 1.0f, 1.0f, 1.0f);
    const auto plainDownmix = render (false, 1.0f, 0.0f, 0.0f, 1.0f);
    CHECK_NEAR (wideDownmix.effectiveWidth, 2.0, 1e-3);
    CHECK_GE (rmsDiff (wideDownmix.out, plainDownmix.out), 0.1 * rms (plainDownmix.out.ch[0].data(), kLen));
}
