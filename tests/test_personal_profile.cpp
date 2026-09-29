// The personal hearing profile (docs/11 E33 slice; PersonalProfile.h): the
// per-ear gain, balance and 8-band EQ at the audiometric frequencies, stored
// outside ParameterStore and applied by a per-ear stage in every strip's
// chain; both placements measured (before the compressor with a headroom
// reservation - the product's - and after the maximizer with its own per-ear
// limiters), the file, the hand-over and the RT properties.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/analysis/PeakMeters.h"
#include "flub/common/Denormals.h"
#include "flub/dsp/Svf.h"
#include "flub/engine/PersonalProfile.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/io/FilePath.h"
#include "flub/io/PresetIO.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace flub;
using namespace flub::param;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;
constexpr int kBlock = 256;
constexpr int kHfFirstBand = 3; // 2 kHz .. 8 kHz

/** +db at 2 - 8 kHz on the right ear (docs/11 E33's "right-ear HF profile"). */
PersonalProfile rightHf (float db)
{
    PersonalProfile p;
    p.enabled = true;
    for (int b = kHfFirstBand; b < PersonalProfile::kNumBands; ++b)
        p.bandDb[1][static_cast<size_t> (b)] = db;
    return p;
}

/** Profiles with typical and awkward shapes, all inside the ranges. */
std::vector<PersonalProfile> shapes()
{
    std::vector<PersonalProfile> out;
    PersonalProfile slope; // a sloping high-frequency loss, worse on the right
    slope.enabled = true;
    slope.bandDb[0] = { 0, 0, 0, 5, 5, 10, 10, 10 };
    slope.bandDb[1] = { 0, 0, 5, 10, 15, 15, 15, 15 };
    out.push_back (slope);
    PersonalProfile cookie; // a mid-frequency ("cookie bite") shape, symmetric
    cookie.enabled = true;
    cookie.bandDb[0] = cookie.bandDb[1] = { 0, 5, 10, 15, 10, 5, 0, 0 };
    out.push_back (cookie);
    PersonalProfile mixed; // broadband gains, a balance and cuts
    mixed.enabled = true;
    mixed.gainDb = { 3.0f, -2.0f };
    mixed.balanceDb = 4.0f;
    mixed.bandDb[0] = { -6, -3, 0, 4, 8, 6, 2, -4 };
    mixed.bandDb[1] = { 2, 2, -5, 0, 7, 12, 9, 3 };
    out.push_back (mixed);
    PersonalProfile notch; // a 4 kHz notch-shaped loss (noise exposure)
    notch.enabled = true;
    notch.bandDb[0] = notch.bandDb[1] = { 0, 0, 0, 3, 10, 15, 8, 3 };
    out.push_back (notch);
    return out;
}

/** 4th-order Butterworth 2 - 8 kHz band-pass on a buffer (in place). */
void bandPass (std::vector<float>& x, double fs)
{
    SvfFilter hp1, hp2, lp1, lp2;
    hp1.set (FilterType::HighPass, 2000.0, butterworthQ (2, 0), 0.0, fs);
    hp2.set (FilterType::HighPass, 2000.0, butterworthQ (2, 1), 0.0, fs);
    lp1.set (FilterType::LowPass, 8000.0, butterworthQ (2, 0), 0.0, fs);
    lp2.set (FilterType::LowPass, 8000.0, butterworthQ (2, 1), 0.0, fs);
    for (auto& v : x)
        v = lp2.processSample (0, lp1.processSample (0, hp2.processSample (0, hp1.processSample (0, v))));
}

double energyDb (const std::vector<float>& x, int from)
{
    double acc = 0.0;
    for (size_t i = static_cast<size_t> (from); i < x.size(); ++i)
        acc += static_cast<double> (x[i]) * x[i];
    return 10.0 * std::log10 (std::max (acc, 1.0e-30));
}

/** A hard-panned high-frequency source: 2 - 8 kHz noise in the right channel
    with its peak at `peakDb` dBFS, and the same source 20 dB down in the left
    (the far ear's head shadow of a binaural render at 4 kHz). */
Planar hardPannedSource (int numSamples, double peakDb)
{
    auto s = whiteNoise (numSamples, 1.0f, 77);
    bandPass (s, kFs);
    const double g = std::pow (10.0, peakDb / 20.0) / peakAbs (s.data(), numSamples);
    Planar p (2, numSamples);
    for (size_t i = 0; i < s.size(); ++i)
    {
        p.ch[1][i] = static_cast<float> (g * s[i]);
        p.ch[0][i] = static_cast<float> (0.1 * g * s[i]);
    }
    return p;
}

void loadFactory (ParameterStore& store, const char* file)
{
    preset::Preset p;
    std::string error;
    const bool ok = preset::load (std::string (FLUB_PRESET_DIR) + "/" + file, p, error);
    if (! ok)
        std::cerr << "    preset load failed: " << error << "\n";
    REQUIRE (ok);
    preset::applyPresetToStore (p, store, Bank::A);
}

void run (ProcessingChain& chain, Planar& buf)
{
    ScopedNoDenormals noDenormals;
    const int n = buf.numSamples();
    for (int pos = 0; pos < n; pos += kBlock)
        chain.process (buf.block (pos, std::min (kBlock, n - pos)));
}

/** The factory preset at Boost 100 (and optionally protection Normal) with a
    profile (or none) at a placement, over `input`. */
Planar renderChain (const char* file, const PersonalProfile* profile, PersonalPlacement placement, const Planar& input,
                    ProtectionStrength strength = ProtectionStrength::Off)
{
    ParameterStore store;
    loadFactory (store, file);
    store.set (BoostIntensity, 1.0f);
    ProcessingChain chain (store);
    chain.setPersonalPlacement (placement);
    chain.setProtectionStrength (strength);
    if (profile != nullptr)
        chain.setPersonalProfile (*profile);
    chain.prepare ({ kFs, kBlock, 2 });
    Planar buf = input;
    run (chain, buf);
    return buf;
}

/** Every module off: the chain is linear, so what the stage does is all it does. */
void neutralStore (ParameterStore& store)
{
    for (int id : { AutoLevelOn, AutoPreampOn, GateOn, EqOn, DynEqOn, BassOn, ClarityOn, SaturationOn, SpatialOn, VirtualizerOn, CompressorOn,
                    MaximizerOn })
        store.set (id, 0.0f);
    store.set (BoostIntensity, 0.0f);
}

/** Per-ear gain (dB) of the stage alone at each band frequency, measured with sines. */
std::array<std::array<double, PersonalProfile::kNumBands>, 2> measureStage (const PersonalProfile& profile, double fs)
{
    std::array<std::array<double, PersonalProfile::kNumBands>, 2> out {};
    for (size_t b = 0; b < static_cast<size_t> (PersonalProfile::kNumBands); ++b)
    {
        PersonalEarStage stage;
        stage.setProfileNow (profile);
        stage.prepare ({ fs, kBlock, 2 }, PersonalPlacement::BeforeCompressor);
        const double f = PersonalProfile::kBandHz[b];
        const int settle = static_cast<int> (0.2 * fs), measure = static_cast<int> (0.3 * fs);
        const auto s = sine (f, fs, settle + measure, 0.01f);
        Planar buf (2, settle + measure);
        buf.ch[0] = buf.ch[1] = s;
        buf.rebind();
        for (int pos = 0; pos < buf.numSamples(); pos += kBlock)
            stage.process (buf.block (pos, std::min (kBlock, buf.numSamples() - pos)));
        for (size_t e = 0; e < 2; ++e)
            out[e][b] = toDb (toneAmplitude (buf.ch[e].data() + settle, measure, f, fs) / 0.01);
    }
    return out;
}
} // namespace

TEST_CASE ("PersonalProfile: ranges, balance and the 12 dB cap on the per-ear difference")
{
    PersonalProfile p;
    p.bandDb[1][5] = 10.0f;
    CHECK (p.isNeutral()); // disabled
    CHECK (p.targetDb (1, 5) == 0.0f);
    p.enabled = true;
    CHECK (! p.isNeutral());
    CHECK (p.targetDb (1, 5) == 10.0f);
    CHECK (p.targetDb (0, 5) == 0.0f);

    // Ranges; NaN and Inf read 0.
    PersonalProfile wild;
    wild.enabled = true;
    wild.gainDb = { 40.0f, std::nanf ("") };
    wild.balanceDb = -50.0f;
    wild.bandDb[0][0] = std::numeric_limits<float>::infinity();
    wild.bandDb[1][7] = 99.0f;
    const auto s = wild.sanitised();
    CHECK (s.gainDb[0] == PersonalProfile::kGainRangeDb);
    CHECK (s.gainDb[1] == 0.0f);
    CHECK (s.balanceDb == -PersonalProfile::kBalanceRangeDb);
    CHECK (s.bandDb[0][0] == 0.0f);
    CHECK (s.bandDb[1][7] == PersonalProfile::kBandRangeDb);

    // A balance only turns an ear down.
    PersonalProfile bal;
    bal.enabled = true;
    bal.balanceDb = 6.0f;
    CHECK (bal.broadbandDb (0) == -6.0f);
    CHECK (bal.broadbandDb (1) == 0.0f);
    bal.balanceDb = -4.0f;
    CHECK (bal.broadbandDb (0) == 0.0f);
    CHECK (bal.broadbandDb (1) == -4.0f);

    // Each ear's target is capped at +15 dB, and the ears differ by at most
    // 12 dB: the higher one is lowered, the lower one never raised.
    PersonalProfile cap;
    cap.enabled = true;
    cap.gainDb = { 12.0f, -12.0f };
    CHECK (cap.broadbandDb (0) == 0.0f); // 12 over -12 is 24: the left comes down to 0
    CHECK (cap.broadbandDb (1) == -12.0f);
    cap.gainDb = { 10.0f, 0.0f };
    cap.bandDb[0][6] = 15.0f;
    CHECK (cap.targetDb (0, 6) == 12.0f); // 25 -> 15 (boost cap) -> 12 (difference cap)
    CHECK (cap.targetDb (1, 6) == 0.0f);
    for (const auto& shape : shapes())
        for (int b = 0; b < PersonalProfile::kNumBands; ++b)
        {
            CHECK_LE (std::abs (shape.targetDb (1, b) - shape.targetDb (0, b)), PersonalProfile::kMaxEarDifferenceDb);
            CHECK_LE (shape.targetDb (0, b), PersonalProfile::kMaxBoostDb);
        }
}

TEST_CASE ("PersonalProfile: per-ear magnitude within 1 dB of target at the audiometric frequencies")
{
    // The design (exact digital response) and the stage as measured with
    // sines, at 44.1, 48 and 96 kHz; the reservation is common to both ears
    // and is taken out.
    double worstDesign = 0.0, worstMeasured = 0.0;
    for (double fs : { 44100.0, 48000.0, 96000.0 })
        for (const auto& profile : shapes())
        {
            const auto curve = designPersonalCurve (profile, fs);
            for (int e = 0; e < 2; ++e)
                for (int b = 0; b < PersonalProfile::kNumBands; ++b)
                    worstDesign = std::max (worstDesign, std::abs (curve.responseDb (e, PersonalProfile::kBandHz[static_cast<size_t> (b)], fs)
                                                                   - profile.targetDb (e, b)));
            if (fs == 96000.0)
                continue; // the measured check at 44.1 and 48 kHz (runtime)
            const auto measured = measureStage (profile, fs);
            const double reservation = personalReservationDb (curve, fs);
            for (int e = 0; e < 2; ++e)
                for (int b = 0; b < PersonalProfile::kNumBands; ++b)
                    worstMeasured = std::max (worstMeasured, std::abs (measured[static_cast<size_t> (e)][static_cast<size_t> (b)] - reservation
                                                                       - profile.targetDb (e, b)));
        }
    std::cout << "    per-ear magnitude: design worst " << worstDesign << " dB, measured worst " << worstMeasured << " dB\n";
    CHECK_LE (worstDesign, 0.01);
    CHECK_LE (worstMeasured, 1.0);

    // Outside 250 Hz .. 8 kHz the response returns to the broadband gain.
    const auto slope = shapes()[0];
    const auto curve = designPersonalCurve (slope, kFs);
    CHECK_NEAR (curve.responseDb (1, 40.0, kFs), 0.0, 0.5);
    CHECK_LE (curve.responseDb (1, 18000.0, kFs), 6.0);
    // The reservation is the louder ear's programme-weighted maximum.
    const float reservation = personalReservationDb (curve, kFs);
    std::cout << "    sloping loss (right +15 dB from 3 kHz): reservation " << reservation << " dB\n";
    CHECK (reservation < 0.0f);
    CHECK (reservation >= -PersonalProfile::kMaxBoostDb);

    // At a hands-free rate the bands at or above 0.4 fs are left out.
    const auto narrow = designPersonalCurve (slope, 16000.0);
    for (int i = 0; i < narrow.numFilters; ++i)
        CHECK (narrow.filters[static_cast<size_t> (i)].frequency < 0.4f * 16000.0f);
    CHECK_NEAR (narrow.responseDb (1, 4000.0, 16000.0), slope.targetDb (1, 5), 0.01);
}

TEST_CASE ("PersonalProfile: the chain applies the stage per ear (every module off)")
{
    // Sines at the band frequencies through a chain with every module off:
    // right over left follows the targets, both ears carry the reservation.
    const auto profile = shapes()[0];
    const int settle = static_cast<int> (0.2 * kFs), measure = static_cast<int> (0.3 * kFs);
    double worst = 0.0;
    for (size_t b = 0; b < static_cast<size_t> (PersonalProfile::kNumBands); ++b)
    {
        const double f = PersonalProfile::kBandHz[b];
        const auto s = sine (f, kFs, settle + measure, 0.05f);
        std::array<std::array<double, 2>, 2> level {}; // [with profile][ear]
        for (int withProfile = 0; withProfile < 2; ++withProfile)
        {
            ParameterStore store;
            neutralStore (store);
            ProcessingChain chain (store);
            if (withProfile == 1)
                chain.setPersonalProfile (profile);
            chain.prepare ({ kFs, kBlock, 2 });
            Planar buf (2, settle + measure);
            buf.ch[0] = buf.ch[1] = s;
            buf.rebind();
            run (chain, buf);
            for (size_t e = 0; e < 2; ++e)
                level[static_cast<size_t> (withProfile)][e] = toDb (toneAmplitude (buf.ch[e].data() + settle, measure, f, kFs));
        }
        ParameterStore probe;
        ProcessingChain probeChain (probe);
        probeChain.setPersonalProfile (profile);
        const double reservation = probeChain.getPersonalReservationDb();
        for (int e = 0; e < 2; ++e)
            worst = std::max (worst, std::abs (level[1][static_cast<size_t> (e)] - level[0][static_cast<size_t> (e)] - reservation
                                               - profile.targetDb (e, static_cast<int> (b))));
    }
    std::cout << "    through the chain: worst per-ear error " << worst << " dB\n";
    CHECK_LE (worst, 1.0);
}

TEST_CASE ("PersonalProfile: a hard-panned source keeps its ILD through the whole chain at Boost 100 (before the compressor; not after the maximizer)")
{
    // The ILD row (docs/11 E33): a +12 dB right-ear HF profile, a 2 - 8 kHz
    // source hard-panned right (the left ear 20 dB down) at -40 / -20 / -6
    // dBFS peak, a Gaming and a Music preset at Boost 100. ILD = right minus
    // left band energy over the last 1.5 s; its change with the profile must
    // stay within 1 dB of the static per-ear gain (the same change through
    // the stage alone).
    const int n = static_cast<int> (2.5 * kFs), from = static_cast<int> (1.0 * kFs);
    const auto profile = rightHf (12.0f);

    const auto ild = [&] (Planar out) {
        bandPass (out.ch[0], kFs);
        bandPass (out.ch[1], kFs);
        return std::array<double, 3> { energyDb (out.ch[1], from) - energyDb (out.ch[0], from), energyDb (out.ch[0], from), energyDb (out.ch[1], from) };
    };

    // The static per-ear gain on this source: the stage alone.
    double staticIld = 0.0;
    {
        Planar src = hardPannedSource (n, -20.0);
        Planar staged = src;
        PersonalEarStage stage;
        stage.setProfileNow (profile);
        stage.prepare ({ kFs, kBlock, 2 }, PersonalPlacement::BeforeCompressor);
        for (int pos = 0; pos < n; pos += kBlock)
            stage.process (staged.block (pos, std::min (kBlock, n - pos)));
        staticIld = ild (staged)[0] - ild (src)[0];
    }
    std::cout << "    static per-ear gain on the source: " << staticIld << " dB\n";
    CHECK_NEAR (staticIld, 12.0, 1.0);

    double worstBefore = 0.0, worstAfter = 0.0;
    for (const char* file : { "gaming-competitive-fps.json", "music-flubsound-signature.json" })
        for (double level : { -40.0, -20.0, -6.0 })
        {
            const Planar src = hardPannedSource (n, level);
            const auto none = ild (renderChain (file, nullptr, PersonalPlacement::BeforeCompressor, src));
            const auto before = ild (renderChain (file, &profile, PersonalPlacement::BeforeCompressor, src));
            const auto after = ild (renderChain (file, &profile, PersonalPlacement::AfterMaximizer, src));
            const double dBefore = before[0] - none[0] - staticIld, dAfter = after[0] - none[0] - staticIld;
            worstBefore = std::max (worstBefore, std::abs (dBefore));
            worstAfter = std::max (worstAfter, std::abs (dAfter));
            std::cout << "    " << file << " at " << level << " dBFS: ILD change - static: before the compressor " << dBefore
                      << " dB (left ear " << before[1] - none[1] << " dB), after the maximizer " << dAfter << " dB (left ear "
                      << after[1] - none[1] << " dB)\n";
        }
    CHECK_LE (worstBefore, 1.0);
    // The measured alternative misses the row: it stays documented, not used.
    CHECK (worstAfter > 1.0);
}

TEST_CASE ("PersonalProfile: true peak stays under the ceiling on dense material with a +15 dB profile (both placements)")
{
    // Dense material: independent full-band pink noise in each ear at -10
    // dBFS RMS (peaks near 0 dBFS), Boost 100; +15 dB on 2 - 8 kHz in both
    // ears, and +15 dB everywhere (+12 broadband, +3 per band).
    const int n = static_cast<int> (2.0 * kFs), from = static_cast<int> (0.5 * kFs);
    Planar dense (2, n);
    dense.ch[0] = pinkNoise (n, 0.316f, 21);
    dense.ch[1] = pinkNoise (n, 0.316f, 22);
    dense.rebind();
    PersonalProfile hf;
    hf.enabled = true;
    for (int b = kHfFirstBand; b < PersonalProfile::kNumBands; ++b)
        hf.bandDb[0][static_cast<size_t> (b)] = hf.bandDb[1][static_cast<size_t> (b)] = 15.0f;
    PersonalProfile all;
    all.enabled = true;
    all.gainDb = { 12.0f, 12.0f };
    for (auto& ear : all.bandDb)
        ear.fill (3.0f);
    constexpr float kTruePeakToleranceDb = 0.15f; // as tests/test_factory_presets.cpp: an independent 4x meter
    float worst = -100.0f;
    for (const char* file : { "gaming-competitive-fps.json", "music-club-loud.json" })
        for (const auto* profile : { &hf, &all })
            for (auto placement : { PersonalPlacement::BeforeCompressor, PersonalPlacement::AfterMaximizer })
            {
                ParameterStore store;
                loadFactory (store, file);
                const float ceilingDb = store.get (MaxCeilingDb);
                Planar out = renderChain (file, profile, placement, dense);
                TruePeakMeter tp;
                tp.prepare (2);
                tp.process (out.block (from, n - from));
                const float over = tp.getMaxDbAllChannels() - ceilingDb;
                worst = std::max (worst, over);
                CHECK_LE (over, kTruePeakToleranceDb);
            }
    std::cout << "    +15 dB profile, dense material, Boost 100: true peak at most " << worst << " dB re the ceiling\n";
}

TEST_CASE ("PersonalProfile: the chain's measures read the output with the stage undone (governor, tonal rule)")
{
    // At protection Normal the governor's drive span, the output PLR and the
    // tonal-balance rule's output read the chain's output with the per-ear
    // stage inverted: a +12 dB right-ear HF profile neither reads as
    // distortion nor as presence the chain adds.
    const int n = static_cast<int> (4.0 * kFs);
    Planar game (2, n);
    game.ch[0] = pinkNoise (n, 0.05f, 31);
    game.ch[1] = pinkNoise (n, 0.05f, 32);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kFs, inStep = std::fmod (t, 0.4);
        const double step = inStep < 0.04 ? 0.1 * std::sin (kPi * inStep / 0.04) * std::sin (kTwoPi * 3200.0 * t) : 0.0;
        game.ch[0][static_cast<size_t> (i)] += static_cast<float> (0.5 * step);
        game.ch[1][static_cast<size_t> (i)] += static_cast<float> (step);
    }
    game.rebind();
    const auto profile = rightHf (12.0f);
    struct Reading
    {
        float residual, presence, tonalScale, harsh;
    };
    const auto measure = [&] (const PersonalProfile* p) {
        ParameterStore store;
        loadFactory (store, "gaming-competitive-fps.json");
        store.set (BoostIntensity, 1.0f);
        store.set (SmoothAmount, 0.5f); // the Smoothness stage's downstream view too
        ProcessingChain chain (store);
        chain.setProtectionStrength (ProtectionStrength::Normal);
        if (p != nullptr)
            chain.setPersonalProfile (*p);
        chain.prepare ({ kFs, kBlock, 2 });
        Planar buf = game;
        run (chain, buf);
        return Reading { chain.getDriveResidualDb(), chain.getTonalLiftDb (TonalBalanceMeter::Presence), chain.getGovernorTonalScale(),
                         chain.getTonalLiftDb (TonalBalanceMeter::Harsh) };
    };
    const Reading none = measure (nullptr), with = measure (&profile);
    std::cout << "    drive residual " << none.residual << " -> " << with.residual << " dB, presence lift " << none.presence << " -> "
              << with.presence << " dB, harsh lift " << none.harsh << " -> " << with.harsh << " dB, tonal scale " << none.tonalScale
              << " -> " << with.tonalScale << "\n";
    CHECK (none.residual > kMinusInfDb && none.presence != TonalBalanceMeter::kNoReading);
    // What remains is the dynamics' gain riding on the stage's output (a
    // gain that moves does not commute exactly with the filters): far under
    // the governor's -35 dB budget. Read on the output itself the drive
    // residual was -10.5 dB, the presence lift 13.1 dB and the harsh lift
    // 8.5 dB (the tonal scale 0.30).
    CHECK_LE (with.residual, -60.0f);
    CHECK_NEAR (with.presence, none.presence, 0.5);
    CHECK_NEAR (with.harsh, none.harsh, 0.5);
    CHECK_NEAR (with.tonalScale, none.tonalScale, 0.05);
}

TEST_CASE ("PersonalProfile: no profile is bit-identical; a cleared profile glides back to untouched")
{
    // Chains at Boost 100, protection Normal and Smoothness on (every path
    // the stage touches runs): no profile, an enabled neutral one, a
    // disabled one with values and a real one replaced by a neutral one
    // before prepare() give the same output bit for bit.
    const int n = static_cast<int> (1.0 * kFs);
    Planar src (2, n);
    src.ch[0] = pinkNoise (n, 0.1f, 41);
    src.ch[1] = pinkNoise (n, 0.1f, 42);
    src.rebind();
    PersonalProfile neutral;
    neutral.enabled = true;
    PersonalProfile disabled = rightHf (12.0f);
    disabled.enabled = false;
    const auto render = [&] (const std::vector<PersonalProfile>& sequence) {
        ParameterStore store;
        loadFactory (store, "music-flubsound-signature.json");
        store.set (BoostIntensity, 1.0f);
        store.set (SmoothAmount, 0.5f);
        ProcessingChain chain (store);
        chain.setProtectionStrength (ProtectionStrength::Normal);
        for (const auto& p : sequence)
            chain.setPersonalProfile (p);
        chain.prepare ({ kFs, kBlock, 2 });
        Planar buf = src;
        run (chain, buf);
        return buf;
    };
    const Planar reference = render ({});
    CHECK (render ({ neutral }).ch == reference.ch);
    CHECK (render ({ disabled }).ch == reference.ch);
    CHECK (render ({ rightHf (12.0f), neutral }).ch == reference.ch);
    CHECK (render ({ rightHf (12.0f) }).ch != reference.ch);
    CHECK (designPersonalCurve (neutral, kFs).isEmpty() && designPersonalCurve (disabled, kFs).isEmpty());

    // The stage alone: a profile, then a neutral one handed over while it
    // runs; after the crossfade the output is the input, bit for bit.
    PersonalEarStage stage;
    stage.setProfileNow (rightHf (12.0f));
    stage.prepare ({ kFs, kBlock, 2 }, PersonalPlacement::BeforeCompressor);
    Planar buf = src;
    const int half = (n / 2 / kBlock) * kBlock; // on a block boundary
    for (int pos = 0; pos < n; pos += kBlock)
    {
        if (pos == half)
            CHECK (stage.setProfile (neutral));
        stage.process (buf.block (pos, std::min (kBlock, n - pos)));
    }
    CHECK (buf.ch[1][static_cast<size_t> (half - 1)] != src.ch[1][static_cast<size_t> (half - 1)]); // the profile ran
    const int fade = static_cast<int> (std::ceil (DeviceCorrection::kCrossfadeMs * 0.001 * kFs)) + kBlock;
    bool untouched = true;
    for (size_t c = 0; c < 2; ++c)
        for (size_t i = static_cast<size_t> (half + fade); i < static_cast<size_t> (n); ++i)
            untouched = untouched && buf.ch[c][i] == src.ch[c][i];
    CHECK (untouched);
}

TEST_CASE ("PersonalProfile: a change is a crossfade, never a click; the hand-over ring and its retry")
{
    // A 2 kHz sine through the stage; the profile jumps from -12 dB on both
    // ears to +12 dB on the right ear's 2 - 8 kHz (with its reservation) while
    // it runs: no sample-to-sample step exceeds that of the louder steady
    // sine after the change.
    const int n = static_cast<int> (0.5 * kFs), change = (n / 2 / kBlock) * kBlock;
    Planar buf (2, n);
    buf.ch[0] = buf.ch[1] = sine (2000.0, kFs, n, 0.05f);
    buf.rebind();
    PersonalProfile down;
    down.enabled = true;
    down.gainDb = { -12.0f, -12.0f };
    PersonalEarStage stage;
    stage.setProfileNow (down);
    stage.prepare ({ kFs, kBlock, 2 }, PersonalPlacement::BeforeCompressor);
    CHECK (stage.getReservationDb() == 0.0f); // cuts need none
    for (int pos = 0; pos < n; pos += kBlock)
    {
        if (pos == change)
            CHECK (stage.setProfile (rightHf (12.0f)));
        stage.process (buf.block (pos, std::min (kBlock, n - pos)));
    }
    const float reservation = stage.getReservationDb();
    double changeStep = 0.0, finalStep = 0.0;
    for (size_t i = static_cast<size_t> (change); i < static_cast<size_t> (n); ++i)
    {
        const double d = std::abs (static_cast<double> (buf.ch[1][i]) - buf.ch[1][i - 1]);
        double& step = i < static_cast<size_t> (n - 4800) ? changeStep : finalStep;
        step = std::max (step, d);
    }
    std::cout << "    24 dB profile change: largest step " << changeStep << " (steady after it " << finalStep << ")\n";
    CHECK_LE (changeStep, finalStep * 1.01);
    // ... and it did change: the right ear from -12 dB to +12 dB with the reservation.
    const double end = toneAmplitude (buf.ch[1].data() + n - 4800, 4800, 2000.0, kFs), start = toneAmplitude (buf.ch[1].data() + 4800, 4800, 2000.0, kFs);
    CHECK_NEAR (toDb (end / start), 12.0 + reservation + 12.0, 0.05);

    // More changes than the ring holds while the audio thread does not run:
    // the newest waits, retryPending() hands it over once there is room.
    PersonalEarStage idle;
    idle.prepare ({ kFs, kBlock, 2 }, PersonalPlacement::BeforeCompressor);
    bool full = false;
    for (int k = 1; k <= 8 && ! full; ++k)
        full = ! idle.setProfile (rightHf (static_cast<float> (k)));
    CHECK (full);
    CHECK (! idle.retryPending());
    Planar one (2, kBlock);
    for (int k = 0; k < 8; ++k)
        idle.process (one.block());
    CHECK (idle.retryPending());
}

TEST_CASE ("PersonalProfile: the profile survives 5 preset loads, a re-prepare, an engine swap and its file")
{
    // Two chains follow the same 5 preset loads (as the app loads them:
    // applyPresetToStore, then prepare() when the store asks for it; the
    // latency profile changes on the way); the one with the profile keeps
    // it - the same design, and the ILD row on the hard-panned source.
    const auto profile = rightHf (12.0f);
    const auto dir = std::filesystem::temp_directory_path() / "flub-personal-profile-test";
    std::filesystem::create_directories (dir);
    const std::string path = io::pathToUtf8 (dir / "Personal \xc3\xa9\xc3\xa0.json");
    std::string error;
    REQUIRE (personal::save (path, profile, error));

    ParameterStore withStore, noneStore;
    ProcessingChain with (withStore), none (noneStore);
    with.setPersonalProfile (profile);
    with.prepare ({ kFs, kBlock, 2 });
    none.prepare ({ kFs, kBlock, 2 });
    const int n = static_cast<int> (0.8 * kFs), from = static_cast<int> (0.4 * kFs);
    const Planar src = hardPannedSource (n, -20.0);
    const char* files[] = { "gaming-competitive-fps.json", "music-bass-head.json", "gaming-night-mode.json", "music-classical-jazz-dynamic.json",
                            "gaming-horror-detail.json" };
    int load = 0;
    for (const char* file : files)
    {
        for (auto* s : { &withStore, &noneStore })
        {
            loadFactory (*s, file);
            s->set (LatencyProfile, static_cast<float> (load % 2 == 0 ? LatencyProfileValue::Balanced : LatencyProfileValue::LowLatency));
        }
        for (auto* c : { &with, &none })
            if (c->needsReprepare())
                c->prepare ({ kFs, kBlock, 2 });
        ++load;
        CHECK (with.getPersonalProfile() == profile);
        CHECK (with.getPersonalCurve() == designPersonalCurve (profile, kFs));
        Planar a = src, b = src;
        run (with, a);
        run (none, b);
        for (auto* p : { &a, &b })
        {
            bandPass (p->ch[0], kFs);
            bandPass (p->ch[1], kFs);
        }
        const double change = (energyDb (a.ch[1], from) - energyDb (a.ch[0], from)) - (energyDb (b.ch[1], from) - energyDb (b.ch[0], from));
        std::cout << "    after loading " << file << ": ILD change " << change << " dB\n";
        CHECK_NEAR (change, 11.35, 1.0);
    }

    // A swapped-in engine's chain takes it over (MixEngine::configureFrom),
    // re-designed at its own rate; the file still holds it.
    ParameterStore swapStore;
    ProcessingChain swapped (swapStore);
    swapped.prepare ({ 44100.0, kBlock, 2 });
    swapped.adoptGovernorState (with);
    CHECK (swapped.getPersonalProfile() == profile);
    CHECK (swapped.getPersonalCurve() == designPersonalCurve (profile, 44100.0));
    PersonalProfile reloaded;
    REQUIRE (personal::load (path, reloaded, error));
    CHECK (reloaded == profile);
    std::filesystem::remove_all (dir);
}

TEST_CASE ("PersonalProfile: the file round-trips, rejects what it cannot read and clamps what is out of range")
{
    PersonalProfile p;
    p.enabled = true;
    p.gainDb = { -3.5f, 2.25f };
    p.balanceDb = 1.5f;
    p.bandDb[0] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    p.bandDb[1] = { -1, -2, 0, 5.5f, 10, 12.5f, 15, -15 };
    std::string error;
    PersonalProfile back;
    REQUIRE (personal::fromJson (personal::toJson (p), back, error));
    CHECK (back == p);
    const auto text = json::write (personal::toJson (p), 2);
    CHECK (text.find ("\"format\": \"flubsound-personal-profile\"") != std::string::npos);

    const auto parse = [] (const std::string& s, PersonalProfile& out, std::string& err) {
        json::Value v;
        return json::parse (s, v, err) && personal::fromJson (v, out, err);
    };
    // Missing keys read 0 / false; values out of range load clamped.
    PersonalProfile sparse;
    REQUIRE (parse (R"({"format":"flubsound-personal-profile","version":1,"right":{"bandsDb":[0,0,0,0,0,0,0,40]}})", sparse, error));
    CHECK (! sparse.enabled);
    CHECK (sparse.bandDb[1][7] == PersonalProfile::kBandRangeDb);
    CHECK (sparse.gainDb[0] == 0.0f);
    // What it cannot read leaves the profile untouched and says why.
    const PersonalProfile keep = p;
    PersonalProfile out = keep;
    for (const char* bad : { R"({"format":"flubsound-preset","version":1})",
                             R"({"format":"flubsound-personal-profile","version":2})",
                             R"({"format":"flubsound-personal-profile","version":1,"bandHz":[250,500,1000,2000,3000,4000,6000]})",
                             R"({"format":"flubsound-personal-profile","version":1,"left":{"bandsDb":[0,0,0]}})",
                             R"({"format":"flubsound-personal-profile","version":1,"left":{"gainDb":"loud"}})",
                             R"({"format":"flubsound-personal-profile","version":1,"enabled":1})" })
    {
        error.clear();
        CHECK (! parse (bad, out, error));
        CHECK (! error.empty());
        CHECK (out == keep);
    }

    // save() replaces an existing file through a temporary one it leaves no trace of.
    const auto dir = std::filesystem::temp_directory_path() / "flub-personal-profile-io";
    std::filesystem::create_directories (dir);
    const std::string path = io::pathToUtf8 (dir / "profile.json");
    REQUIRE (personal::save (path, keep, error));
    REQUIRE (personal::save (path, sparse, error));
    PersonalProfile loaded;
    REQUIRE (personal::load (path, loaded, error));
    CHECK (loaded == sparse);
    CHECK (! std::filesystem::exists (dir / "profile.json.tmp"));
    CHECK (! personal::load (io::pathToUtf8 (dir / "missing.json"), loaded, error));
    std::filesystem::remove_all (dir);
}
