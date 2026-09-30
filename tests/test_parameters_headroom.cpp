// docs/11 E11 (automatic preamp) and E05 steps 1 and 4 (the clipper's crest
// gate and depth cap as parameters, named maximizer styles): the layout
// version 3 parameters, the allocation-free static-boost predictor, the
// chain's static-boost model against the modules' own responses, the
// automatic preamp in the chain (level, glide, AutoLevel, bypass) and the
// styles. Phase 3 batch 5: the model's level-dependent terms (presence in
// both laws, the crossfeed's centred sum, the compressor, the dynamic EQ,
// the bass protection and harmonics) against the modules on pink noise, the
// programme level the chain measures for them, and the Done-when row - the
// prediction within 1 dB of the rendered pink transfer maximum for every
// factory preset at Boost 0 / 50 / 100 (one case per preset).
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/common/Denormals.h"
#include "flub/dsp/Bs775Fold.h"
#include "flub/dsp/DeviceCorrection.h"
#include "flub/dsp/Fft.h"
#include "flub/dsp/ParametricEq.h"
#include "flub/dsp/Svf.h"
#include "flub/engine/Parameters.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/io/PresetIO.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace flub;
using namespace flub::param;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;

void allModulesOff (ParameterStore& s)
{
    for (int id : { GateOn, EqOn, DynEqOn, BassOn, ClarityOn, SaturationOn, SpatialOn, VirtualizerOn, CompressorOn, MaximizerOn })
        s.set (id, 0.0f);
}

void run (ProcessingChain& chain, Planar& buf, int blockSize)
{
    ScopedNoDenormals noDenormals;
    const int n = buf.numSamples();
    for (int pos = 0; pos < n; pos += blockSize)
        chain.process (buf.block (pos, std::min (blockSize, n - pos)));
}

Planar stereoOf (const std::vector<float>& x)
{
    Planar p (2, static_cast<int> (x.size()));
    std::copy (x.begin(), x.end(), p.ch[0].begin());
    std::copy (x.begin(), x.end(), p.ch[1].begin());
    return p;
}

std::vector<float> effectiveOf (const ProcessingChain& chain)
{
    std::vector<float> e (static_cast<size_t> (kNumParams));
    for (int i = 0; i < kNumParams; ++i)
        e[static_cast<size_t> (i)] = chain.effectiveValue (i);
    return e;
}

/** Gain (dB) of a steady sine through the chain, measured on its last 200 ms. */
double sineGainDb (ParameterStore& store, double freq, float amplitude)
{
    ProcessingChain chain (store);
    chain.prepare ({ kFs, 512, 2 });
    const int n = static_cast<int> (kFs * 0.8), m = static_cast<int> (kFs * 0.2);
    Planar buf = stereoOf (sine (freq, kFs, n, amplitude));
    run (chain, buf, 512);
    return toDb (toneAmplitude (buf.ch[0].data() + n - m, m, freq, kFs) / amplitude);
}

/** The static-boost model of the chain's current effective values, for a
    programme at programmeDb (the level-dependent terms: docs/11 E11 p3b5). */
ProcessingChain::StaticBoostModel modelOf (ParameterStore& store, double programmeDb)
{
    ProcessingChain chain (store);
    chain.prepare ({ kFs, 512, 2 });
    Planar buf (2, 64);
    run (chain, buf, 64); // one block publishes the effective values
    const auto e = effectiveOf (chain);
    ProcessingChain::StaticBoostModel m;
    ProcessingChain::BoostModelContext ctx;
    ctx.programmeDb = programmeDb;
    ProcessingChain::buildStaticBoostModel (e.data(), kFs, false, m, ctx);
    return m;
}
} // namespace

//==============================================================================
TEST_CASE ("Parameters (E11 / E05 / E19): auto.preamp, auto.preampAllowance, max.clipCrest, max.clipMaxDb, max.style, max.lfLimit and max.bedLift - layout version 3, defaults that keep every older preset's sound")
{
    const auto& t = layout();
    struct Expect
    {
        const char* key;
        int id;
        float def, lo, hi;
    };
    const Expect expected[] = {
        { "auto.preamp", AutoPreampOn, 0.0f, 0.0f, 1.0f },
        { "auto.preampAllowance", AutoPreampAllowanceDb, 1.0f, 0.0f, 12.0f },
        { "max.clipCrest", MaxClipCrestDb, 6.0f, 0.0f, 24.0f },   // the stage 1 clipper's MaximizerParams::clipCrestDb
        { "max.clipMaxDb", MaxClipMaxDb, 3.0f, 0.5f, 24.0f },     // ... and clipMaxDepthDb
        { "max.style", MaxStyle, 0.0f, 0.0f, 4.0f },               // Custom
        { "max.lfLimit", MaxLfLimit, 0.0f, 0.0f, 1.0f },           // off: MaximizerParams::lfLimit (docs/11 E05 step 5)
        { "max.bedLift", MaxBedLiftDb, 24.0f, 0.0f, 24.0f },       // 24 = no budget (docs/11 E19 step 3)
    };
    for (const auto& x : expected)
    {
        REQUIRE (findByKey (x.key) == x.id);
        const auto& info = t[static_cast<size_t> (x.id)];
        CHECK (info.sinceVersion == 3);
        CHECK (info.defaultValue == x.def);
        CHECK (info.minValue == x.lo);
        CHECK (info.maxValue == x.hi);
        CHECK (! info.structural);
    }
    const MaximizerParams stage1;
    CHECK (t[static_cast<size_t> (MaxClipCrestDb)].defaultValue == stage1.clipCrestDb);
    CHECK (t[static_cast<size_t> (MaxClipMaxDb)].defaultValue == stage1.clipMaxDepthDb);
    CHECK (t[static_cast<size_t> (MaxLfLimit)].defaultValue == stage1.lfLimit);
    CHECK (t[static_cast<size_t> (MaxBedLiftDb)].defaultValue == stage1.bedLiftDb);
    CHECK (t[static_cast<size_t> (MaxStyle)].choices.size() == 5);
    CHECK (t[static_cast<size_t> (MaxStyle)].choices[0] == "Custom");

    // Custom owns nothing; every named style gives all six of its controls a
    // value inside their parameter ranges.
    CHECK (maxStyleValues (MaxStyleValue::Custom) == nullptr);
    CHECK (maxStyleValues (static_cast<MaxStyleValue> (5)) == nullptr);
    for (auto s : { MaxStyleValue::Transparent, MaxStyleValue::Punchy, MaxStyleValue::Aggressive, MaxStyleValue::Safe })
    {
        const MaxStyleValues* v = maxStyleValues (s);
        REQUIRE (v != nullptr);
        const auto inRange = [&t] (int id, float x) { return t[static_cast<size_t> (id)].clamp (x) == x; };
        CHECK (inRange (MaxClipAmount, v->clipAmount));
        CHECK (inRange (MaxClipKnee, v->clipKnee));
        CHECK (inRange (MaxClipCrestDb, v->clipCrestDb));
        CHECK (inRange (MaxClipMaxDb, v->clipMaxDb));
        CHECK (inRange (MaxReleaseMs, v->releaseMs));
    }
    CHECK (maxStyleValues (MaxStyleValue::Safe)->clipAmount == 0.0f); // limiter only
}

//==============================================================================
TEST_CASE ("Headroom: predictMaxBoostWith finds exactly what predictMaxBoost finds, without allocating (the chain's predictor, docs/11 E11)")
{
    CorrectionCurve curve;
    curve.add ({ CorrectionFilterType::Peak, 1000.0f, 6.0f, 1.0f });
    curve.add ({ CorrectionFilterType::Peak, 5300.0f, 4.0f, 12.0f });
    curve.add ({ CorrectionFilterType::LowShelf, 90.0f, 3.5f, 0.7f });
    curve.add ({ CorrectionFilterType::HighShelf, 9000.0f, -2.0f, 0.7f });
    const auto response = [&curve] (double f) { return curve.responseDb (0, f, kFs); };
    for (auto w : { headroom::Weighting::Flat, headroom::Weighting::Programme })
        for (const auto& range : { std::pair { 20.0, 20000.0 }, std::pair { 50.0, 8000.0 }, std::pair { 1000.0, 1000.0 } })
        {
            const auto a = headroom::predictMaxBoost (response, w, range.first, range.second);
            headroom::Prediction b;
            {
                AllocationGuard guard;
                b = headroom::predictMaxBoostWith (response, w, range.first, range.second);
                CHECK (guard.allocations() == 0);
            }
            CHECK (a.maxBoostDb == b.maxBoostDb);
            CHECK (a.atHz == b.atHz);
        }
}

//==============================================================================
TEST_CASE ("Headroom: the chain's static-boost model is the modules' own response - EQ exactly, and a quiet sine through dynamic-EQ static gain, bass shelf + subsonic, presence, air and saturation make-up within 0.05 dB (air 0.1)")
{
    // EQ: every band type, against ParametricEq::responseDb (the GUI curve).
    {
        ParameterStore s;
        allModulesOff (s);
        s.set (EqOn, 1.0f);
        const float types[] = { 0, 1, 2, 3, 4, 5, 6, 0, 0, 2 };
        const float gains[] = { 5.0f, 3.0f, -4.0f, 0.0f, 0.0f, 0.0f, 0.0f, -8.0f, 12.0f, 2.5f };
        const float freqs[] = { 180.0f, 60.0f, 7000.0f, 25.0f, 18000.0f, 3000.0f, 1000.0f, 400.0f, 2500.0f, 12000.0f };
        std::vector<EqBandParams> bands;
        for (int b = 0; b < kEqBands; ++b)
        {
            s.set (eq (b, EqFieldType), types[b]);
            s.set (eq (b, EqFieldGain), gains[b]);
            s.set (eq (b, EqFieldFreq), freqs[b]);
            s.set (eq (b, EqFieldQ), 0.5f + 0.3f * static_cast<float> (b));
            s.set (eq (b, EqFieldSlope), static_cast<float> (b % 4));
            EqBandParams p;
            p.enabled = true;
            p.type = static_cast<EqBandType> (static_cast<int> (types[b]));
            p.gainDb = gains[b];
            p.frequency = freqs[b];
            p.q = 0.5f + 0.3f * static_cast<float> (b);
            p.slopeDbPerOct = 12 * (b % 4 + 1);
            bands.push_back (p);
        }
        s.set (EqOutputGainDb, -1.5f);
        const auto m = modelOf (s, ProcessingChain::kNominalProgrammeDb);
        double worst = 0.0;
        for (double f = 20.0; f < 20000.0; f *= 1.03)
            worst = std::max (worst, std::abs (m.responseDb (f) - (ParametricEq::responseDb (bands.data(), kEqBands, f, kFs) - 1.5)));
        CHECK_LE (worst, 1.0e-6);
    }

    // The other stages, each alone, against a rendered -40 dBFS sine (every
    // one of them is at its full boost down there; the model at a -60 dBFS
    // programme, where presence lifts fully and nothing withdraws).
    struct Case
    {
        const char* what;
        double freq, tolerance;
        void (*setup) (ParameterStore&);
    };
    const Case cases[] = {
        { "dynamic-EQ static gain", 2000.0, 0.05, [] (ParameterStore& s) {
              s.set (DynEqOn, 1.0f);
              s.set (dyn (1, DynFieldOn), 1.0f);
              s.set (dyn (1, DynFieldFreq), 2000.0f);
              s.set (dyn (1, DynFieldStaticGain), 4.0f);
          } },
        { "bass shelf + subsonic", 30.0, 0.05, [] (ParameterStore& s) {
              s.set (BassOn, 1.0f);
              s.set (BassBoostDb, 6.0f);
              s.set (BassBoostFreq, 80.0f);
              s.set (BassSubsonic, 25.0f);
          } },
        { "presence (full lift)", 3200.0, 0.05, [] (ParameterStore& s) {
              s.set (ClarityOn, 1.0f);
              s.set (ClarityPresence, 0.5f);
          } },
        // The air exciter's cubic has a linear term: its -12 dB branch adds a
        // little of the band's (LP4 7 kHz) fundamental, +0.05 dB at 14 kHz.
        { "air shelf", 14000.0, 0.1, [] (ParameterStore& s) {
              s.set (ClarityOn, 1.0f);
              s.set (ClarityAir, 1.0f);
          } },
        { "saturation make-up", 1000.0, 0.05, [] (ParameterStore& s) {
              s.set (SaturationOn, 1.0f);
              s.set (SatType, 2.0f); // Digital
              s.set (SatMix, 0.5f);
              s.set (SatOutputDb, 3.0f);
          } },
    };
    for (const auto& c : cases)
    {
        ParameterStore s;
        allModulesOff (s);
        c.setup (s);
        const double predicted = modelOf (s, -60.0).responseDb (c.freq);
        const double rendered = sineGainDb (s, c.freq, 0.01f);
        std::printf ("    measured %s at %.0f Hz: model %+.3f dB, rendered %+.3f dB\n", c.what, c.freq, predicted, rendered);
        CHECK (std::abs (predicted) > 1.0); // the case does boost
        CHECK_NEAR (predicted, rendered, c.tolerance);
    }

    // The surround folds' -3 dB trim counts against the boosts.
    ParameterStore s;
    allModulesOff (s);
    std::vector<float> e (static_cast<size_t> (kNumParams));
    s.snapshot (e.data());
    ProcessingChain::StaticBoostModel m;
    ProcessingChain::buildStaticBoostModel (e.data(), kFs, true, m);
    CHECK_NEAR (m.responseDb (1000.0), 20.0 * std::log10 (static_cast<double> (Bs775Fold::kMatrixGain)), 1.0e-9);
    ProcessingChain::buildStaticBoostModel (e.data(), kFs, false, m);
    CHECK (m.responseDb (1000.0) == 0.0);
}

//==============================================================================
TEST_CASE ("Chain (E11): auto.preamp takes the predicted boost minus the allowance off - a sine at the predicted maximum leaves at input + allowance; a boost where programme is rare costs nothing")
{
    ParameterStore s;
    allModulesOff (s);
    s.set (EqOn, 1.0f);
    s.set (eq (5, EqFieldGain), 6.0f); // 1 kHz bell, Q 1
    s.set (AutoPreampOn, 1.0f);
    s.set (AutoPreampAllowanceDb, 1.0f);
    ProcessingChain chain (s);
    chain.prepare ({ kFs, 512, 2 });
    const int n = static_cast<int> (kFs * 0.5), m = static_cast<int> (kFs * 0.2);
    Planar buf = stereoOf (sine (1000.0, kFs, n, 0.1f));
    run (chain, buf, 256);
    CHECK_NEAR (chain.getPredictedBoostDb(), 6.0, 0.001); // programme weighting is 0 dB at 1 kHz
    CHECK_NEAR (chain.getPredictedBoostHz(), 1000.0, 5.0);
    CHECK_NEAR (chain.getAutoPreampDb(), -5.0, 0.001);
    // From the first block on: prepare() starts the preamp at its value.
    CHECK_NEAR (toDb (toneAmplitude (buf.ch[0].data() + n - m, m, 1000.0, kFs) / 0.1), 1.0, 0.01);
    CHECK_NEAR (toDb (toneAmplitude (buf.ch[0].data() + 2400, 4800, 1000.0, kFs) / 0.1), 1.0, 0.02);

    // A new boost is predicted within kHeadroomUpdateMs (10 ms) and glides in.
    s.set (eq (5, EqFieldGain), 9.0f);
    Planar more = stereoOf (sine (1000.0, kFs, n, 0.1f));
    run (chain, more, 64);
    CHECK_NEAR (chain.getAutoPreampDb(), -8.0, 0.001);
    CHECK_NEAR (toDb (toneAmplitude (more.ch[0].data() + n - m, m, 1000.0, kFs) / 0.1), 1.0, 0.01);

    // The same bell at 8 kHz sits 9 dB under the programme envelope: no preamp.
    s.set (eq (5, EqFieldGain), 0.0f);
    s.set (eq (8, EqFieldGain), 6.0f);
    Planar hf = stereoOf (sine (8000.0, kFs, n, 0.1f));
    run (chain, hf, 512);
    CHECK_LE (chain.getPredictedBoostDb(), 0.5); // the bell's skirt over the flat programme region
    CHECK (chain.getAutoPreampDb() == 0.0f);

    // Off: the prediction still runs (for a display), the preamp is 0 dB.
    s.set (eq (8, EqFieldGain), 0.0f);
    s.set (eq (5, EqFieldGain), 6.0f);
    s.set (AutoPreampOn, 0.0f);
    run (chain, hf, 512);
    CHECK_NEAR (chain.getPredictedBoostDb(), 6.0, 0.001);
    CHECK (chain.getAutoPreampDb() == 0.0f);
}

//==============================================================================
TEST_CASE ("Chain (E11): auto.preamp off (or at 0 dB) is bit-identical; switching it glides without a step; AutoLevel does not cancel it; the bypass reference does not carry it")
{
    const int n = static_cast<int> (kFs * 1.0);
    const auto prog = pinkNoise (n, 0.05f, 91);
    const auto render = [&] (ParameterStore& s, const std::vector<float>& x, int block) {
        ProcessingChain chain (s);
        chain.prepare ({ kFs, 512, 2 });
        Planar buf = stereoOf (x);
        run (chain, buf, block);
        return buf;
    };

    // Default chain vs. auto.preamp on with nothing boosted (preamp 0 dB) and
    // vs. a changed allowance with the preamp off: the same samples.
    {
        ParameterStore a, b, c;
        b.set (AutoPreampOn, 1.0f);
        c.set (AutoPreampAllowanceDb, 7.0f);
        const Planar ya = render (a, prog, 480), yb = render (b, prog, 480), yc = render (c, prog, 480);
        CHECK (ya.ch == yb.ch);
        CHECK (ya.ch == yc.ch);
    }

    // Switching on mid-signal: a 100 Hz tone (tiny second difference) through
    // an EQ that boosts it 6 dB; the preamp's -5 dB glides over 20 ms.
    {
        ParameterStore s;
        allModulesOff (s);
        s.set (EqOn, 1.0f);
        s.set (eq (2, EqFieldFreq), 100.0f);
        s.set (eq (2, EqFieldGain), 6.0f);
        ProcessingChain chain (s);
        chain.prepare ({ kFs, 512, 2 });
        Planar buf = stereoOf (sine (100.0, kFs, n, 0.2f));
        ScopedNoDenormals noDenormals;
        for (int pos = 0; pos < n; pos += 128)
        {
            if (pos >= n / 2)
                s.set (AutoPreampOn, 1.0f);
            chain.process (buf.block (pos, std::min (128, n - pos)));
        }
        const auto& y = buf.ch[0];
        double steady = 0.0, around = 0.0;
        for (int i = 2000; i < n; ++i)
        {
            const double d2 = std::abs (static_cast<double> (y[static_cast<size_t> (i)]) - 2.0 * y[static_cast<size_t> (i - 1)] + y[static_cast<size_t> (i - 2)]);
            double& worst = i < n / 2 ? steady : around;
            worst = std::max (worst, d2);
        }
        std::printf ("    measured second difference while the preamp switches / steady = %.3f\n", around / steady);
        // A linear 20 ms ramp bends the tone at its two corners (1.75x the
        // tone's own second difference, like input.gain's ramp); a gain step
        // of -5 dB would read about 2500x.
        CHECK_LE (around, 2.5 * steady);
        const int m = static_cast<int> (kFs * 0.1);
        CHECK_NEAR (toDb (toneAmplitude (y.data() + n / 2 - m, m, 100.0, kFs) / 0.2), 6.0, 0.02);
        CHECK_NEAR (toDb (toneAmplitude (y.data() + n - m, m, 100.0, kFs) / 0.2), 1.0, 0.02);
    }

    // AutoLevel measures before the preamp: the same AutoLevel gain with the
    // preamp on or off, so the output is exactly the preamp lower.
    {
        ParameterStore off, on;
        for (auto* s : { &off, &on })
        {
            allModulesOff (*s);
            s->set (EqOn, 1.0f);
            s->set (eq (5, EqFieldGain), 6.0f);
            s->set (AutoLevelOn, 1.0f);
        }
        on.set (AutoPreampOn, 1.0f);
        const auto x = pinkNoise (static_cast<int> (kFs * 3.0), 0.02f, 92);
        const Planar yo = render (off, x, 512), yn = render (on, x, 512);
        const int len = static_cast<int> (kFs * 1.0), from = static_cast<int> (x.size()) - len;
        const double d = toDb (rms (yn.ch[0].data() + from, len) / rms (yo.ch[0].data() + from, len));
        std::printf ("    measured AutoLevel on: preamp on - off = %+.3f dB (preamp -5 dB)\n", d);
        CHECK_NEAR (d, -5.0, 0.01);
    }

    // Global bypass (not loudness matched): the reference is the unprocessed
    // input, preamp or not.
    {
        ParameterStore off, on;
        for (auto* s : { &off, &on })
        {
            allModulesOff (*s);
            s->set (EqOn, 1.0f);
            s->set (eq (5, EqFieldGain), 6.0f);
            s->set (BypassAll, 1.0f);
            s->set (LoudnessMatchBypass, 0.0f);
        }
        on.set (AutoPreampOn, 1.0f);
        const Planar yo = render (off, prog, 512), yn = render (on, prog, 512);
        double diff = 0.0; // the mix w + b (dry - w) at b = 1 rounds w away
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
                diff = std::max (diff, static_cast<double> (std::abs (yo.ch[static_cast<size_t> (c)][static_cast<size_t> (i)]
                                                                     - yn.ch[static_cast<size_t> (c)][static_cast<size_t> (i)])));
        CHECK_LE (diff, 1.0e-7);
    }
}

//==============================================================================
TEST_CASE ("Chain (E05): a named maximizer style sets its six controls, Custom leaves them to the store; style, preamp and EQ changes do not allocate")
{
    ParameterStore s;
    s.set (MaxClipAmount, 0.3f);
    s.set (MaxClipMaxDb, 5.0f);
    s.set (MaxReleaseMs, 200.0f);
    ProcessingChain chain (s);
    chain.prepare ({ kFs, 512, 2 });
    Planar buf = stereoOf (pinkNoise (512 * 40, 0.1f, 93));
    const auto block = [&] (int k) { chain.process (buf.block (512 * k, 512)); };
    block (0);
    CHECK (chain.effectiveValue (MaxClipAmount) == 0.3f);
    CHECK (chain.effectiveValue (MaxClipMaxDb) == 5.0f);
    CHECK (chain.effectiveValue (MaxReleaseMs) == 200.0f);
    CHECK (chain.effectiveValue (MaxClipCrestDb) == 6.0f);

    AllocationGuard guard;
    int k = 1;
    for (auto style : { MaxStyleValue::Transparent, MaxStyleValue::Punchy, MaxStyleValue::Aggressive, MaxStyleValue::Safe })
    {
        s.set (MaxStyle, static_cast<float> (style));
        s.set (AutoPreampOn, static_cast<float> (k % 2));
        s.set (eq (3, EqFieldGain), static_cast<float> (k));
        block (k++);
        const MaxStyleValues* v = maxStyleValues (style);
        CHECK (chain.effectiveValue (MaxClipAmount) == v->clipAmount);
        CHECK (chain.effectiveValue (MaxClipKnee) == v->clipKnee);
        CHECK (chain.effectiveValue (MaxClipCrestDb) == v->clipCrestDb);
        CHECK (chain.effectiveValue (MaxClipMaxDb) == v->clipMaxDb);
        CHECK (chain.effectiveValue (MaxReleaseMs) == v->releaseMs);
        CHECK (chain.effectiveValue (MaxAutoRelease) == (v->autoRelease ? 1.0f : 0.0f));
        for (int j = 0; j < 4; ++j)
            block (k++);
    }
    s.set (MaxStyle, 0.0f);
    block (k++);
    CHECK (guard.allocations() == 0);
    CHECK (chain.effectiveValue (MaxClipAmount) == 0.3f);
    CHECK (chain.effectiveValue (MaxReleaseMs) == 200.0f);
}

//==============================================================================
// docs/11 E11 (Phase 3 batch 5): the model's level-dependent terms and the
// Done-when row. The programme is pink noise high-passed at 20 Hz (4th order):
// the model's pink (pinkPowerDb in ProcessingChain.cpp), whose sub-20 Hz
// content would otherwise drive the peak detectors of chains without a
// subsonic filter.
namespace
{
std::vector<float> programmePink (double seconds, double rmsDb, uint32_t seed)
{
    auto x = pinkNoise (static_cast<int> (seconds * kFs), 1.0f, seed);
    const SvfCoeffs h1 = SvfCoeffs::make (FilterType::HighPass, 20.0, butterworthQ (2, 0), 0.0, kFs);
    const SvfCoeffs h2 = SvfCoeffs::make (FilterType::HighPass, 20.0, butterworthQ (2, 1), 0.0, kFs);
    SvfState s1, s2;
    for (auto& v : x)
        v = svfTick (h2, s2, svfTick (h1, s1, v));
    const double g = std::pow (10.0, rmsDb / 20.0) / rms (x.data(), static_cast<int> (x.size()));
    for (auto& v : x)
        v = static_cast<float> (v * g);
    return x;
}

/** Power per 1/6-octave band (25 Hz .. 16 kHz, centres in `centres`) of both
    channels, Hann frames of 16384 from `skip` on. */
std::vector<double> bandPowers (const Planar& p, int skip, std::vector<double>* centres = nullptr)
{
    constexpr int N = 16384;
    static thread_local Fft fft;
    fft.prepare (N);
    std::vector<double> spec (N / 2 + 1, 0.0);
    std::vector<Fft::Complex> bins (spec.size());
    std::vector<float> frame (N);
    const int n = p.numSamples();
    for (int start = skip; start + N <= n; start += N / 2)
        for (const auto& ch : p.ch)
        {
            for (int i = 0; i < N; ++i)
                frame[static_cast<size_t> (i)] = ch[static_cast<size_t> (start + i)] * static_cast<float> (0.5 - 0.5 * std::cos (kTwoPi * i / N));
            fft.forwardReal (frame.data(), bins.data());
            for (size_t k = 0; k < spec.size(); ++k)
                spec[k] += std::norm (bins[k]);
        }
    std::vector<double> out;
    if (centres != nullptr)
        centres->clear();
    for (double c = 25.0; c <= 16000.0; c *= std::exp2 (1.0 / 6.0))
    {
        const double lo = c * std::exp2 (-1.0 / 12.0), hi = c * std::exp2 (1.0 / 12.0);
        double sum = 0.0;
        for (size_t k = 0; k < spec.size(); ++k)
            if (const double f = static_cast<double> (k) * kFs / N; f >= lo && f < hi)
                sum += spec[k];
        out.push_back (sum);
        if (centres != nullptr)
            centres->push_back (c);
    }
    return out;
}

struct Transfer
{
    std::vector<double> centres, db; // the rendered pink transfer per band
    ProcessingChain::StaticBoostModel model; // the chain's model at the programme level
};

/** Renders 3 s of `x` (2 identical channels, 512-sample blocks) through a
    chain on `store` with the maximizer held out, and returns the transfer
    measured from 1 s on (latency-compensated) with the chain's model at
    programmeDb. */
Transfer transferOf (ParameterStore& store, const std::vector<float>& x, double programmeDb)
{
    ProcessingChain chain (store);
    chain.setAuditionBypass (MaximizerOn, true);
    chain.prepare ({ kFs, 512, 2 });
    Planar buf = stereoOf (x);
    run (chain, buf, 512);
    const int lat = chain.getLatencySamples(), n = buf.numSamples();
    Planar out (2, n);
    for (size_t c = 0; c < 2; ++c)
        std::copy (buf.ch[c].begin() + lat, buf.ch[c].end(), out.ch[c].begin());
    Transfer t;
    const auto in = bandPowers (stereoOf (x), static_cast<int> (kFs), &t.centres);
    const auto o = bandPowers (out, static_cast<int> (kFs));
    for (size_t i = 0; i < in.size(); ++i)
        t.db.push_back (10.0 * std::log10 (o[i] / in[i]));
    chain.buildHeadroomModel (t.model, programmeDb);
    return t;
}

double transferAt (const Transfer& t, double hz)
{
    size_t best = 0;
    for (size_t i = 1; i < t.centres.size(); ++i)
        if (std::abs (std::log (t.centres[i] / hz)) < std::abs (std::log (t.centres[best] / hz)))
            best = i;
    return t.db[best];
}
} // namespace

TEST_CASE ("Headroom (E11): the model's level-dependent terms against each module alone on pink at -30 and -18 dBFS - presence in both laws, the crossfeed's centred sum, the compressor, a dynamic-EQ cut, the bass protection and harmonics")
{
    struct Case
    {
        const char* what;
        double hz, tolerance;
        void (*setup) (ParameterStore&);
    };
    const Case cases[] = {
        { "presence, Absolute", 3200.0, 0.3, [] (ParameterStore& s) {
              s.set (ClarityOn, 1.0f);
              s.set (ClarityPresence, 1.0f);
          } },
        { "presence, Relative", 3200.0, 0.3, [] (ParameterStore& s) {
              s.set (ClarityOn, 1.0f);
              s.set (ClarityPresence, 1.0f);
              s.set (ClarityPresenceMode, static_cast<float> (PresenceModeValue::Relative));
          } },
        { "crossfeed (Bs2b 1), centred", 40.0, 0.2, [] (ParameterStore& s) {
              s.set (SpatialOn, 1.0f);
              s.set (SpatialCrossfeed, 1.0f);
          } },
        { "compressor (-20 dB, 3:1, +6 dB make-up)", 1000.0, 0.4, [] (ParameterStore& s) {
              s.set (CompressorOn, 1.0f);
              s.set (CompThresholdDb, -20.0f);
              s.set (CompRatio, 3.0f);
              s.set (CompMakeupDb, 6.0f);
          } },
        { "dynamic-EQ cut (bell 5 kHz Q 1.5, -28 dB, 2.5:1, 4 dB)", 5000.0, 0.4, [] (ParameterStore& s) {
              s.set (DynEqOn, 1.0f);
              s.set (dyn (1, DynFieldOn), 1.0f);
              s.set (dyn (1, DynFieldFreq), 5000.0f);
              s.set (dyn (1, DynFieldQ), 1.5f);
              s.set (dyn (1, DynFieldThreshold), -28.0f);
              s.set (dyn (1, DynFieldRatio), 2.5f);
              s.set (dyn (1, DynFieldRange), 4.0f);
          } },
        { "bass shelf 9 dB at 60 Hz, protected at -12 dB", 31.5, 0.5, [] (ParameterStore& s) {
              s.set (BassOn, 1.0f);
              s.set (BassBoostDb, 9.0f);
              s.set (BassBoostFreq, 60.0f);
              s.set (BassHarmonics, 0.0f);
              s.set (BassTighten, 0.0f);
          } },
        { "bass harmonics 0.4 above a 150 Hz cutoff", 180.0, 0.6, [] (ParameterStore& s) {
              s.set (BassOn, 1.0f);
              s.set (BassBoostDb, 0.0f);
              s.set (BassHarmonics, 0.4f);
              s.set (BassHarmonicsCutoff, 150.0f);
              s.set (BassTighten, 0.0f);
          } },
    };
    for (double level : { -30.0, -18.0 })
    {
        const auto x = programmePink (3.0, level, 7);
        for (const auto& c : cases)
        {
            ParameterStore s;
            allModulesOff (s);
            c.setup (s);
            const auto t = transferOf (s, x, level);
            const double rendered = transferAt (t, c.hz), predicted = t.model.responseDb (c.hz);
            std::printf ("    measured %s at %.0f dBFS, %.0f Hz: model %+.2f dB, rendered %+.2f dB\n", c.what, level, c.hz, predicted, rendered);
            CHECK_NEAR (predicted, rendered, c.tolerance);
        }
    }
}

TEST_CASE ("Chain (E11): the model's programme level is the loudness of the input's loud parts - pink at -24 dBFS reads -24 after its 3 s warm-up, a quieter passage and a reset() hold it, prepare() starts again at the nominal -18; the published prediction is the model's at that level")
{
    ParameterStore s;
    allModulesOff (s);
    s.set (ClarityOn, 1.0f);
    s.set (ClarityPresence, 1.0f); // Absolute: the lift depends on the level
    s.set (ClarityPresenceFreq, 1000.0f); // where the programme weighting is 0 dB
    s.set (AutoPreampOn, 1.0f);
    ProcessingChain chain (s);
    chain.prepare ({ kFs, 512, 2 });
    CHECK (chain.getModelProgrammeDb() == static_cast<float> (ProcessingChain::kNominalProgrammeDb));
    const auto feed = [&] (double seconds, double level, uint32_t seed) {
        Planar buf = stereoOf (programmePink (seconds, level, seed));
        run (chain, buf, 512);
    };
    feed (2.5, -24.0, 11);
    CHECK (chain.getModelProgrammeDb() == static_cast<float> (ProcessingChain::kNominalProgrammeDb)); // still warming up
    feed (2.5, -24.0, 12);
    const float loud = chain.getModelProgrammeDb();
    std::printf ("    measured the model's programme level on -24 dBFS pink: %.2f dB\n", loud);
    CHECK_NEAR (loud, -24.0, 0.5);
    ProcessingChain::StaticBoostModel m;
    chain.buildHeadroomModel (m, loud);
    const auto p = ProcessingChain::predictStaticBoost (m, headroom::Weighting::Programme);
    std::printf ("    measured the prediction at that level: %+.2f dB at %.0f Hz\n", p.maxBoostDb, p.atHz);
    CHECK_NEAR (chain.getPredictedBoostDb(), p.maxBoostDb, 1.0e-4);
    CHECK_NEAR (p.maxBoostDb, 3.3, 0.3); // presence's lift on -24 dBFS pink (the level terms' case above)
    // The preamp counts presence at a quiet programme's full lift (onsets get
    // it before the detector withdraws it): the model at kPreampQuietProgrammeDb.
    chain.buildHeadroomModel (m, ProcessingChain::kPreampQuietProgrammeDb);
    const auto quiet = ProcessingChain::predictStaticBoost (m, headroom::Weighting::Programme);
    CHECK_NEAR (quiet.maxBoostDb, 6.0, 0.05);
    CHECK_NEAR (chain.getAutoPreampDb(), headroom::preampDb (quiet, 1.0), 1.0e-4);

    feed (3.0, -36.0, 13); // a quiet passage: the loud parts' level holds
    CHECK (chain.getModelProgrammeDb() == loud);
    chain.reset(); // a transport jump keeps it (and warms up again)
    feed (1.0, -36.0, 14);
    CHECK (chain.getModelProgrammeDb() == loud);
    chain.prepare ({ kFs, 512, 2 });
    CHECK (chain.getModelProgrammeDb() == static_cast<float> (ProcessingChain::kNominalProgrammeDb));
}

namespace
{
/** docs/11 E11's Done-when row for one factory preset: the flat-weighted
    prediction of its chain's model at the programme's level within 1 dB of
    the maximum of the rendered transfer of -24 dBFS pink (1/6-octave bands,
    25 Hz .. 16 kHz; maximizer held out, Auto Level off, Balanced) at Boost
    0 / 50 / 100. */
void checkPredictionAgainstRender (const std::filesystem::path& file)
{
    preset::Preset p;
    std::string error;
    REQUIRE (preset::load (file.string(), p, error));
    constexpr double level = -24.0;
    const auto x = programmePink (3.0, level, 7);
    for (float boost : { 0.0f, 0.5f, 1.0f })
    {
        ParameterStore s;
        preset::applyToStore (p, s, Bank::A);
        s.set (BoostIntensity, boost);
        s.set (MaximizerOn, 0.0f);
        s.set (LatencyProfile, static_cast<float> (LatencyProfileValue::Balanced));
        s.set (AutoLevelOn, 0.0f);
        const auto t = transferOf (s, x, level);
        size_t at = 0;
        for (size_t i = 1; i < t.db.size(); ++i)
            if (t.db[i] > t.db[at])
                at = i;
        const auto predicted = ProcessingChain::predictStaticBoost (t.model, headroom::Weighting::Flat);
        std::printf ("    measured %s, Boost %.0f: predicted %+.2f dB at %.0f Hz, rendered %+.2f dB at %.0f Hz\n", file.stem().string().c_str(),
                     boost * 100.0f, predicted.maxBoostDb, predicted.atHz, t.db[at], t.centres[at]);
        CHECK_NEAR (predicted.maxBoostDb, t.db[at], 1.0);
    }
}

#ifdef FLUB_PRESET_DIR
bool registerPredictionCases()
{
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator (FLUB_PRESET_DIR))
        if (entry.path().extension() == ".json")
            files.push_back (entry.path());
    std::sort (files.begin(), files.end());
    for (const auto& file : files)
    {
        const std::string name = "Headroom (E11) Done-when: the predicted maximum boost is within 1 dB of the rendered pink transfer maximum at Boost 0 / 50 / 100 - "
                                 + file.stem().string();
        ::flubtest::Registrar (name.c_str(), [file] { checkPredictionAgainstRender (file); }, __FILE__, __LINE__);
    }
    return true;
}

[[maybe_unused]] const bool kPredictionCasesRegistered = registerPredictionCases();
#endif
} // namespace
