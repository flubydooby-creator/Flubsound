// Music Warmth (docs/11 E14, owner feedback): the level-compensated tone
// tilt (flub/dsp/ToneTilt.h), the gentle Tube colour through MacroMap's
// override row, warmth.tapeGrit's v1 rows (Lo-Fi Chill, Warm Vinyl), and
// every Done-when row of E14's Warmth remap through the whole chain:
//
//   * a -6 dBFS 1 kHz sine at Warmth 50 / 100 (maximizer off): THD+N
//     <= 0.5 %, H2 > H3, no inharmonic product above -80 dBc (44.1 / 48 kHz)
//   * pink transfer (Warmth X re Warmth 0, third-octave bands): +3.5 / -2.7
//     dB at 200 Hz / 10 kHz at 100 %, half of that at 50 % (+-0.3 dB)
//   * integrated loudness Warmth 0 <-> 50 / 100 within 0.3 LU on pink noise
//     and on the drum-and-bass music programme (render diff's "music"), and
//     within 0.5 LU with the automatic preamp on
//   * a sweep 0 -> 100 -> 0 over 2 s is click-free; Warmth 0 is untouched
//
// Every test prints its values ("    measured ...").
#include "TestFramework.h"
#include "TestSignals.h"

#include "Analysis.h"

#include "flub/analysis/LoudnessMeter.h"
#include "flub/common/Denormals.h"
#include "flub/common/Math.h"
#include "flub/dsp/Fft.h"
#include "flub/dsp/ToneTilt.h"
#include "flub/engine/MacroMap.h"
#include "flub/engine/Parameters.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/io/PresetIO.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace flub;
using namespace flub::param;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;
constexpr int kBlock = 512;

void measured (const std::string& name, double value, const char* unit)
{
    char buf[64];
    std::snprintf (buf, sizeof (buf), "%.2f", value);
    std::cout << "    measured " << name << " = " << buf << " " << unit << "\n";
}

std::vector<float> defaults()
{
    std::vector<float> v (static_cast<size_t> (kNumParams));
    for (int i = 0; i < kNumParams; ++i)
        v[static_cast<size_t> (i)] = layout()[static_cast<size_t> (i)].defaultValue;
    return v;
}

/** The render diff's "music" programme (tests' makeProgramme at 0.25): chirp
    kicks every 500 ms, noise hats, a 55 Hz bass line, a 440 / 660 Hz pad. */
Planar musicProgramme (int n)
{
    Planar p (2, n);
    FastRandom rng (1234);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kFs;
        const double beat = std::fmod (t, 0.5);
        const double kick = std::exp (-beat * 18.0) * std::sin (kTwoPi * (50.0 + 80.0 * std::exp (-beat * 30.0)) * beat);
        const double hat = (std::fmod (t + 0.25, 0.5) < 0.03 ? 0.3 : 0.0) * rng.nextBipolar();
        const double bass = 0.4 * std::sin (kTwoPi * 55.0 * t);
        const double pad = 0.15 * std::sin (kTwoPi * 440.0 * t) + 0.1 * std::sin (kTwoPi * 660.0 * t + 0.3);
        p.ch[0][static_cast<size_t> (i)] = 0.25f * static_cast<float> (kick + hat + bass + pad);
        p.ch[1][static_cast<size_t> (i)] = 0.25f * static_cast<float> (kick + 0.8 * hat + bass + 0.7 * pad);
    }
    return p;
}

Planar pinkProgramme (int n, float rms = 0.1f)
{
    Planar p (2, n);
    const auto noise = pinkNoise (n, rms, 4711);
    for (auto& c : p.ch)
        std::copy (noise.begin(), noise.end(), c.begin());
    return p;
}

struct Setting
{
    float warmth = 0.0f;
    bool maximizer = true, tapeGrit = false;
    double fs = kFs;
};

/** A store and the chain that reads it (the chain keeps a reference). */
struct Rig
{
    ParameterStore store;
    ProcessingChain chain { store };
};

/** The default Music chain at `s`, run over `buf` in place. */
std::unique_ptr<Rig> render (const Setting& s, Planar& buf)
{
    auto rig = std::make_unique<Rig>();
    rig->store.set (Mode, static_cast<float> (ModeValue::Music));
    rig->store.set (Macro5, s.warmth);
    rig->store.set (MaximizerOn, s.maximizer ? 1.0f : 0.0f);
    rig->store.set (WarmthTapeGrit, s.tapeGrit ? 1.0f : 0.0f);
    rig->chain.prepare ({ s.fs, kBlock, 2 });
    ScopedNoDenormals noDenormals;
    const int n = buf.numSamples();
    for (int pos = 0; pos < n; pos += kBlock)
        rig->chain.process (buf.block (pos, std::min (kBlock, n - pos)));
    return rig;
}

double integratedLufs (Planar& buf)
{
    LoudnessMeter m;
    m.prepare (kFs, 2);
    m.process (buf.block());
    return m.getIntegratedLufs();
}

/** Welch power of channel 0 in the third-octave band around hz (Hann, 8192,
    50 % overlap) from `from` on. */
double thirdOctaveDb (const Planar& buf, int from, double hz)
{
    constexpr int kN = 8192;
    Fft fft;
    fft.prepare (kN);
    std::vector<float> seg (kN);
    std::vector<std::complex<float>> bins (kN / 2 + 1);
    const int lo = static_cast<int> (std::ceil (hz * std::pow (2.0, -1.0 / 6.0) * kN / kFs));
    const int hi = static_cast<int> (std::floor (hz * std::pow (2.0, 1.0 / 6.0) * kN / kFs));
    double power = 0.0;
    for (int start = from; start + kN <= buf.numSamples(); start += kN / 2)
    {
        for (int i = 0; i < kN; ++i)
            seg[static_cast<size_t> (i)] = buf.ch[0][static_cast<size_t> (start + i)]
                                           * static_cast<float> (0.5 - 0.5 * std::cos (kTwoPi * i / kN));
        fft.forwardReal (seg.data(), bins.data());
        for (int k = lo; k <= hi; ++k)
            power += std::norm (bins[static_cast<size_t> (k)]);
    }
    return 10.0 * std::log10 (std::max (power, 1.0e-30));
}
} // namespace

// ---------------------------------------------------------------------------
TEST_CASE ("Warmth: the new parameters are appended in layout version 5, off by default")
{
    for (int id : { WarmthTone, WarmthTapeGrit })
    {
        const auto& info = layout()[static_cast<size_t> (id)];
        CHECK (info.sinceVersion == 5);
        CHECK (info.defaultValue == 0.0f);
    }
    CHECK (findByKey ("warmth.tone") == WarmthTone);
    CHECK (findByKey ("warmth.tapeGrit") == WarmthTapeGrit);
    CHECK (WarmthTapeGrit == kNumScalarParams - 1);
}

TEST_CASE ("Warmth: MacroMap - Music Warmth drives the tilt and a gentle Tube colour; tapeGrit is the v1 rows; Gaming's Voice & Score is untouched")
{
    auto base = defaults();
    std::vector<float> eff (static_cast<size_t> (kNumParams)), half (static_cast<size_t> (kNumParams));
    base[Mode] = static_cast<float> (ModeValue::Music);

    // Warmth 0: nothing moves (the tilt idles, see below).
    MacroMap::apply (base.data(), eff.data(), 1.0f);
    CHECK (eff == base);

    for (float w : { 0.5f, 1.0f })
    {
        base[Macro5] = w;
        MacroMap::apply (base.data(), eff.data(), 1.0f);
        const float c = smoothstep (0.0f, 1.0f, w);
        CHECK (eff[WarmthTone] == c);
        CHECK (eff[SaturationOn] == 1.0f);
        CHECK (eff[SatType] == 1.0f); // Tube: sat.type left at its default
        CHECK_NEAR (eff[SatDriveDb], 0.9 * c, 1e-6);
        CHECK (eff[BassBoostDb] == 0.0f); // the v1 bass rows are gone
        CHECK (eff[BassHarmonics] == 0.0f);
        MacroMap::apply (base.data(), half.data(), 0.5f);
        CHECK_NEAR (half[SatDriveDb], 0.45 * c, 1e-6); // governed
        CHECK (half[WarmthTone] == c);                 // the tilt is not
    }
    // A chosen type stays, and so does the type of a saturator the user or
    // preset switched on (even Tape, the default).
    base[SatType] = 2.0f;
    MacroMap::apply (base.data(), eff.data(), 1.0f);
    CHECK (eff[SatType] == 2.0f);
    base[SatType] = 0.0f;
    base[SaturationOn] = 1.0f;
    MacroMap::apply (base.data(), eff.data(), 1.0f);
    CHECK (eff[SatType] == 0.0f);
    CHECK_NEAR (eff[SatDriveDb], 0.9, 1e-6); // the gentle drive on the user's saturator
    base[SaturationOn] = 0.0f;

    // warmth.tapeGrit: the v1 Warmth, contribution for contribution.
    base[WarmthTapeGrit] = 1.0f;
    base[Macro5] = 0.5f;
    MacroMap::apply (base.data(), eff.data(), 1.0f);
    CHECK (eff[WarmthTone] == 0.0f);
    CHECK (eff[SatType] == 0.0f);
    CHECK (eff[SaturationOn] == 1.0f && eff[BassOn] == 1.0f);
    CHECK (eff[SatDriveDb] == 9.0f * smoothstep (0.0f, 1.0f, 0.5f));
    CHECK (eff[BassHarmonics] == 0.2f * smoothstep (0.4f, 1.0f, 0.5f));
    CHECK (eff[BassBoostDb] == 2.0f * smoothstep (0.3f, 1.0f, 0.5f));
    MacroMap::apply (base.data(), half.data(), 0.5f);
    CHECK (half[SatDriveDb] == 9.0f * smoothstep (0.0f, 1.0f, 0.5f) * 0.5f);

    // Gaming: M5 is Voice & Score, with or without tapeGrit.
    base[Mode] = static_cast<float> (ModeValue::Gaming);
    for (float grit : { 0.0f, 1.0f })
    {
        base[WarmthTapeGrit] = grit;
        MacroMap::apply (base.data(), eff.data(), 1.0f);
        CHECK (eff[WarmthTone] == 0.0f);
        CHECK (eff[SatType] == 0.0f && eff[SaturationOn] == 0.0f && eff[SatDriveDb] == 0.0f);
        CHECK_NEAR (eff[ClarityPresence], 0.35, 1e-6);
        CHECK (eff[ClarityOn] == 1.0f && eff[DynEqOn] == 1.0f);
    }
}

TEST_CASE ("Warmth: Lo-Fi Chill and Warm Vinyl keep the v1 Tape grit - their effective values are bit-identical to the v1 Warmth")
{
#ifdef FLUB_PRESET_DIR
    struct V1Row
    {
        int id;
        float amount, start, end;
    };
    // MacroMap.cpp's v1 M5 rows (the ones before docs/11 E14's remap).
    const V1Row v1[] = { { SaturationOn, 1.0f, 0.0f, 0.02f }, { SatDriveDb, 9.0f, 0.0f, 1.0f }, { BassHarmonics, 0.2f, 0.4f, 1.0f },
                         { BassBoostDb, 2.0f, 0.3f, 1.0f },   { BassOn, 1.0f, 0.0f, 0.02f } };
    for (const char* file : { "music-lo-fi-chill.json", "music-warm-vinyl.json" })
    {
        preset::Preset p;
        std::string error;
        REQUIRE (preset::load (std::string (FLUB_PRESET_DIR) + "/" + file, p, error));
        CHECK (p.values[static_cast<size_t> (WarmthTapeGrit)] == 1.0f);
        const float w = p.values[static_cast<size_t> (Macro5)];
        CHECK (w > 0.0f);
        std::vector<float> eff (static_cast<size_t> (kNumParams)), expected (static_cast<size_t> (kNumParams));
        MacroMap::apply (p.values.data(), eff.data(), 1.0f);
        // Every other contribution, then the v1 Warmth rows in their order.
        auto noWarmth = p.values;
        noWarmth[static_cast<size_t> (Macro5)] = 0.0f;
        MacroMap::apply (noWarmth.data(), expected.data(), 1.0f);
        expected[static_cast<size_t> (Macro5)] = w;
        for (const auto& r : v1)
            expected[static_cast<size_t> (r.id)] = layout()[static_cast<size_t> (r.id)].clamp (
                expected[static_cast<size_t> (r.id)] + r.amount * smoothstep (r.start, r.end, w) * 1.0f);
        int differing = 0;
        for (int i = 0; i < kNumParams; ++i)
            differing += eff[static_cast<size_t> (i)] != expected[static_cast<size_t> (i)] ? 1 : 0;
        measured (std::string (file) + ": effective values differing from v1", differing, "");
        CHECK (differing == 0);
    }
#endif
}

TEST_CASE ("Warmth: with warmth.tapeGrit the governor scales the v1 Warmth drive through the chain as before")
{
    // test_distortion.cpp's hot case (Tape at 12 dB plus Warmth 100 on a
    // loud drum programme, maximizer off) with warmth.tapeGrit on: the
    // effective drive is the base plus the v1 +9 dB times the governor's
    // scale of the block before, and the governor does back off.
    ParameterStore store;
    store.set (Mode, static_cast<float> (ModeValue::Music));
    store.set (MaximizerOn, 0.0f);
    store.set (SaturationOn, 1.0f);
    store.set (SatDriveDb, 12.0f);
    store.set (Macro5, 1.0f);
    store.set (WarmthTapeGrit, 1.0f);
    constexpr int kTick = 480; // 10 ms blocks: one governor tick each
    ProcessingChain chain (store);
    chain.prepare ({ kFs, kTick, 2 });
    const int n = static_cast<int> (kFs * 4.0);
    Planar prog = musicProgramme (n);
    for (auto& c : prog.ch)
        for (auto& v : c)
            v *= 1.4f; // about the level of test_distortion's 0.35 programme
    float prevScale = 1.0f, minScale = 1.0f;
    double maxError = 0.0;
    ScopedNoDenormals noDenormals;
    for (int pos = 0; pos < n; pos += kTick)
    {
        chain.process (prog.block (pos, std::min (kTick, n - pos)));
        const float expected = std::clamp (12.0f + 9.0f * prevScale, 0.0f, 24.0f);
        maxError = std::max (maxError, static_cast<double> (std::abs (chain.effectiveValue (SatDriveDb) - expected)));
        CHECK (chain.effectiveValue (WarmthTone) == 0.0f);
        CHECK (chain.effectiveValue (SatType) == 0.0f);
        prevScale = chain.meters().governorScale.load();
        minScale = std::min (minScale, prevScale);
    }
    measured ("tapeGrit, lowest governor scale", minScale, "");
    CHECK_LE (maxError, 1e-4);
    CHECK (minScale < 1.0f);
}

TEST_CASE ("Warmth: a -6 dBFS 1 kHz sine at Warmth 50 / 100 (maximizer off) - THD+N <= 0.5 %, H2 > H3, no inharmonic above -80 dBc")
{
    // Before (v1: Tape, +9 dB drive): Warmth 50 H2 -146 / H3 -30.5 dB,
    // THD+N 2.99 %; Tube alone 3.96 % (50) / 9.84 % (100) (docs/11 E14).
    for (float w : { 0.5f, 1.0f })
    {
        const int n = static_cast<int> (2 * kFs);
        Planar buf (2, n);
        const auto s = sine (1000.0, kFs, n, 0.5f);
        for (auto& c : buf.ch)
            std::copy (s.begin(), s.end(), c.begin());
        render ({ w, false }, buf);
        const float* x = buf.ch[0].data() + static_cast<int> (kFs);
        const int m = static_cast<int> (kFs);
        const double thdnDb = cli::sineThdnDb (x, m, kFs, 1000.0);
        const double a1 = toneAmplitude (x, m, 1000.0, kFs);
        const double h2 = toDb (toneAmplitude (x, m, 2000.0, kFs) / a1), h3 = toDb (toneAmplitude (x, m, 3000.0, kFs) / a1);
        const std::string at = "Warmth " + std::to_string (static_cast<int> (w * 100.0f));
        measured (at + ", THD+N", 100.0 * std::pow (10.0, thdnDb / 20.0), "%");
        measured (at + ", H2", h2, "dBc");
        measured (at + ", H3", h3, "dBc");
        CHECK_LE (thdnDb, 20.0 * std::log10 (0.005));
        CHECK (h2 > h3);
    }
    // Inharmonic products: a tone on an odd bin near 1 kHz, so every folded
    // harmonic lands between the harmonics (worstAliasDbc, Analysis.h).
    constexpr int kN = 65536;
    for (double fs : { 44100.0, 48000.0 })
        for (float w : { 0.5f, 1.0f })
        {
            const int bin = cli::aliasToneBin (1000.0, fs, kN);
            const double f0 = bin * fs / kN;
            const int n = static_cast<int> (fs / 2) + kN;
            Planar buf (2, n);
            for (int i = 0; i < n; ++i)
                buf.ch[0][static_cast<size_t> (i)] = buf.ch[1][static_cast<size_t> (i)] = static_cast<float> (0.5 * std::sin (kTwoPi * f0 * i / fs));
            render ({ w, false, false, fs }, buf);
            const double dbc = cli::worstAliasDbc (buf.ch[0].data() + (n - kN), kN, fs, bin);
            measured ("worst inharmonic, Warmth " + std::to_string (static_cast<int> (w * 100.0f)) + " at " + std::to_string (static_cast<int> (fs)) + " Hz", dbc, "dBc");
            CHECK_LE (dbc, -80.0);
        }
}

TEST_CASE ("Warmth: pink transfer re Warmth 0 - +3.5 dB at 200 Hz and -2.7 dB at 10 kHz at 100 %, half at 50 % (+-0.3 dB)")
{
    const int n = static_cast<int> (4 * kFs);
    const int from = static_cast<int> (kFs);
    Planar ref = pinkProgramme (n);
    render ({ 0.0f }, ref);
    const double ref200 = thirdOctaveDb (ref, from, 200.0), ref10k = thirdOctaveDb (ref, from, 10000.0), ref1k = thirdOctaveDb (ref, from, 1000.0);
    for (float w : { 0.5f, 1.0f })
    {
        Planar buf = pinkProgramme (n);
        render ({ w }, buf);
        const double d200 = thirdOctaveDb (buf, from, 200.0) - ref200, d10k = thirdOctaveDb (buf, from, 10000.0) - ref10k,
                     d1k = thirdOctaveDb (buf, from, 1000.0) - ref1k;
        const std::string at = "pink transfer, Warmth " + std::to_string (static_cast<int> (w * 100.0f));
        measured (at + ", 200 Hz", d200, "dB");
        measured (at + ", 1 kHz", d1k, "dB");
        measured (at + ", 10 kHz", d10k, "dB");
        CHECK_NEAR (d200, 3.5 * w, 0.3);
        CHECK_NEAR (d10k, -2.7 * w, 0.3);
        CHECK_NEAR (d1k, 0.0, 0.4);
    }
}

namespace
{
/** Integrated loudness of `prog` at Warmth 50 / 100 re Warmth 0 (the default Music chain). */
void checkLoudnessChange (const char* name, bool music)
{
    const int n = static_cast<int> (5 * kFs);
    const auto make = [music] (int length) { return music ? musicProgramme (length) : pinkProgramme (length); };
    Planar ref = make (n);
    render ({ 0.0f }, ref);
    const double l0 = integratedLufs (ref);
    for (float w : { 0.5f, 1.0f })
    {
        Planar buf = make (n);
        const auto rig = render ({ w }, buf);
        const double d = integratedLufs (buf) - l0;
        const std::string at = std::string (name) + ", Warmth " + std::to_string (static_cast<int> (w * 100.0f));
        measured (at + ", loudness re Warmth 0", d, "LU");
        measured (at + ", level compensation", rig->chain.getWarmthTilt().getCompensationDb(), "dB");
        CHECK_LE (std::abs (d), 0.3);
    }
}
} // namespace

TEST_CASE ("Warmth: switching Warmth 0 <-> 50 / 100 changes integrated loudness by <= 0.3 LU on pink noise")
{
    checkLoudnessChange ("pink", false);
}

TEST_CASE ("Warmth: switching Warmth 0 <-> 50 / 100 changes integrated loudness by <= 0.3 LU on the music programme")
{
    checkLoudnessChange ("music", true);
}

TEST_CASE ("Warmth: the level compensation follows the programme, not the beat, and the static-boost model counts the sections")
{
    // Music programme at Warmth 100: the compensation over 2 .. 6 s (kicks
    // every 500 ms) holds within 0.1 dB.
    const int n = static_cast<int> (6 * kFs);
    Planar buf = musicProgramme (n);
    ParameterStore store;
    store.set (Mode, static_cast<float> (ModeValue::Music));
    store.set (Macro5, 1.0f);
    ProcessingChain chain (store);
    chain.prepare ({ kFs, kBlock, 2 });
    double lo = 1.0e9, hi = -1.0e9;
    {
        ScopedNoDenormals noDenormals;
        for (int pos = 0; pos < n; pos += kBlock)
        {
            chain.process (buf.block (pos, std::min (kBlock, n - pos)));
            if (pos >= 2 * kFs)
            {
                const double c = chain.getWarmthTilt().getCompensationDb();
                lo = std::min (lo, c);
                hi = std::max (hi, c);
            }
        }
    }
    measured ("music, Warmth 100, compensation", 0.5 * (lo + hi), "dB");
    measured ("music, Warmth 100, compensation spread over 2..6 s", hi - lo, "dB");
    measured ("music, full tilt's loudness change (L1)", chain.getWarmthTilt().getFullTiltLoudnessDb(), "dB");
    CHECK_LE (hi - lo, 0.1);
    CHECK (chain.getWarmthTilt().getCompensationDb() < -0.4f); // the body bell's lift on the bass line comes back off

    // The automatic preamp's model (docs/11 E11) sees the two sections.
    auto base = defaults();
    std::vector<float> eff (static_cast<size_t> (kNumParams));
    base[Mode] = static_cast<float> (ModeValue::Music);
    base[Macro5] = 1.0f;
    MacroMap::apply (base.data(), eff.data(), 1.0f);
    ProcessingChain::StaticBoostModel model;
    ProcessingChain::buildStaticBoostModel (eff.data(), kFs, false, model);
    SvfCoeffs body, high;
    ToneTilt::sections (1.0f, kFs, body, high);
    for (double hz : { 60.0, 200.0, 10000.0 })
    {
        const double lift = model.responseDb (hz) - model.responseDb (1000.0);
        const double expected = body.magnitudeDb (hz, kFs) + high.magnitudeDb (hz, kFs) - body.magnitudeDb (1000.0, kFs) - high.magnitudeDb (1000.0, kFs);
        measured ("static-boost model, " + std::to_string (static_cast<int> (hz)) + " Hz re 1 kHz at Warmth 100", lift, "dB");
        CHECK_NEAR (lift, expected, 0.01);
    }
}

TEST_CASE ("Warmth: with the automatic preamp on, Warmth 0 <-> 100 stays within 0.5 LU (the preamp counts the tilt's trim)")
{
    // The automatic preamp (docs/11 E11) takes back static boosts over its
    // allowance. The tilt's own trim already takes its lift back on this
    // bass-heavy programme; counting only the sections turned Warmth 100
    // into -2.8 LU here (verifier, docs/11 E14 Status).
    const int n = static_cast<int> (5 * kFs);
    const auto run = [n] (float warmth, float& preampDb) {
        Planar buf = musicProgramme (n);
        ParameterStore store;
        store.set (Mode, static_cast<float> (ModeValue::Music));
        store.set (Macro5, warmth);
        store.set (AutoPreampOn, 1.0f);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, kBlock, 2 });
        ScopedNoDenormals noDenormals;
        for (int pos = 0; pos < n; pos += kBlock)
            chain.process (buf.block (pos, std::min (kBlock, n - pos)));
        preampDb = chain.getAutoPreampDb();
        return integratedLufs (buf);
    };
    float p0 = 0.0f, p1 = 0.0f;
    const double l0 = run (0.0f, p0), l1 = run (1.0f, p1);
    measured ("music, auto preamp on, Warmth 100 loudness re Warmth 0", l1 - l0, "LU");
    measured ("music, auto preamp on, preamp at Warmth 0", p0, "dB");
    measured ("music, auto preamp on, preamp at Warmth 100", p1, "dB");
    CHECK_LE (std::abs (l1 - l0), 0.5);
}

TEST_CASE ("Warmth: a sweep 0 -> 100 -> 0 over 2 s is click-free")
{
    // The default Music chain on 50 Hz + 300 Hz (0.07 each; the body bell
    // moves them by 0.5 / 2.6 dB, so the level compensation cannot cancel
    // both): the Warmth macro moved every block, 0 -> 1 over 1 s and back
    // over 1 s. Anything above 2 kHz in these tones is a click (a
    // coefficient, trim, type or engage step).
    ParameterStore store;
    store.set (Mode, static_cast<float> (ModeValue::Music));
    ProcessingChain chain (store);
    chain.prepare ({ kFs, kBlock, 2 });
    const int n = static_cast<int> (kFs * 3.0);
    const int lead = static_cast<int> (kFs * 0.5);
    Planar buf (2, n);
    const auto lo = sine (50.0, kFs, n, 0.07f), mid = sine (300.0, kFs, n, 0.07f);
    for (auto& c : buf.ch)
        for (size_t i = 0; i < c.size(); ++i)
            c[i] = lo[i] + mid[i];
    bool reachedFull = false;
    {
        ScopedNoDenormals noDenormals;
        for (int pos = 0; pos < n; pos += kBlock)
        {
            const double t = static_cast<double> (pos - lead) / kFs;
            store.set (Macro5, static_cast<float> (std::clamp (t < 1.0 ? t : 2.0 - t, 0.0, 1.0)));
            chain.process (buf.block (pos, std::min (kBlock, n - pos)));
            reachedFull = reachedFull || chain.getWarmthTilt().getAppliedAmount() > 0.95f;
        }
    }
    CHECK (reachedFull);
    CHECK (! chain.getWarmthTilt().isRunning()); // back at 0: idle again
    const int period = static_cast<int> (kFs / 50.0);
    double maxStepDb = 0.0, last = 0.0;
    for (int p0 = lead - period; p0 + period <= n; p0 += period)
    {
        const double peak = toDb (peakAbs (buf.ch[0].data() + p0, period));
        if (p0 >= lead)
            maxStepDb = std::max (maxStepDb, std::abs (peak - last));
        last = peak;
    }
    std::vector<float> y (buf.ch[0]);
    float* yp[] = { y.data() };
    for (int s = 0; s < 4; ++s)
    {
        SvfFilter hp;
        hp.set (FilterType::HighPass, 2000.0, butterworthQ (4, s), 0.0, kFs);
        hp.process (AudioBlock (yp, 1, n));
    }
    const double hfDb = toDb (peakAbs (y.data() + lead, n - lead)) - toDb (0.14);
    measured ("largest change between 20 ms periods", maxStepDb, "dB");
    measured ("peak above 2 kHz re the tones", hfDb, "dB");
    CHECK (maxStepDb <= 0.5);
    CHECK (hfDb < -70.0);
}

TEST_CASE ("Warmth: at 0 the tilt does not touch the signal (bit-exact), and after a trip to 100 it glides back and idles bit-exact")
{
    ToneTilt tilt;
    tilt.prepare ({ kFs, kBlock, 2 });
    tilt.setParams ({ 0.0f });
    const int n = static_cast<int> (kFs);
    Planar buf = musicProgramme (n);
    const Planar input = buf;
    processInBlocks (tilt, buf, kBlock);
    CHECK (buf.ch == input.ch);
    CHECK (! tilt.isRunning());

    tilt.setParams ({ 1.0f });
    Planar on = musicProgramme (n);
    processInBlocks (tilt, on, kBlock);
    CHECK (tilt.getAppliedAmount() == 1.0f);
    CHECK (on.ch != input.ch);
    tilt.setParams ({ 0.0f });
    Planar back = musicProgramme (n);
    processInBlocks (tilt, back, kBlock); // 200 ms glide, then idle
    CHECK (! tilt.isRunning());
    const int after = static_cast<int> (0.25 * kFs);
    bool exact = true;
    for (int c = 0; c < 2; ++c)
        for (int i = after; i < n; ++i)
            exact = exact && back.ch[static_cast<size_t> (c)][static_cast<size_t> (i)] == input.ch[static_cast<size_t> (c)][static_cast<size_t> (i)];
    CHECK (exact);

    // In the chain at Warmth 0 the tilt never starts.
    Planar prog = musicProgramme (n);
    const auto rig = render ({ 0.0f }, prog);
    CHECK (! rig->chain.getWarmthTilt().isRunning());
    CHECK (rig->chain.effectiveValue (WarmthTone) == 0.0f);
}
