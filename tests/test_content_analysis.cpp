// docs/11 E34: the content analysis tap (ContentAnalysis.h) and Smart macro
// scaling (MacroMap::smartModulation, ProcessingChain::setSmartMacros).
//  * the readings on known signals: pink / white noise tilt, a sub-bass
//    share, a limited master's PLR, correlation and M/S width, onsets;
//  * frames close on the sample count, whatever the host blocks;
//  * the Done-when rows on synthetic programme: on a limited master
//    (PLR < 8 LU) Punch 100 lowers the integrated loudness by at most 0.1 LU
//    and raises the true peak by at most 0.5 dB with Smart on; open material
//    (PLR 12+) renders within 0.2 LU of Smart off; renders are bit-identical
//    twice and Smart off is bit-identical to a chain that never had it.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/analysis/ContentAnalysis.h"
#include "flub/analysis/LoudnessMeter.h"
#include "flub/analysis/PeakMeters.h"
#include "flub/common/Denormals.h"
#include "flub/engine/MacroMap.h"
#include "flub/engine/Parameters.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/io/PresetIO.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

using namespace flub;
using namespace flub::param;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;

AnalysisState analyse (Planar& p)
{
    std::vector<const float*> ptrs;
    for (auto& c : p.ch)
        ptrs.push_back (c.data());
    return ContentAnalysis::analyseWhole (ptrs.data(), p.numChannels(), p.numSamples(), kFs);
}

Planar stereo (const std::vector<float>& l, const std::vector<float>& r)
{
    Planar p (2, static_cast<int> (l.size()));
    std::copy (l.begin(), l.end(), p.ch[0].begin());
    std::copy (r.begin(), r.end(), p.ch[1].begin());
    return p;
}

/** Music-like programme: a 55 Hz kick every 500 ms, noise hats on the
    off-beats and a pink bed, partly correlated between the sides. About
    -18 LUFS with 12 - 13 LU of PLR. */
Planar musicProgramme (double seconds, uint32_t seed = 7)
{
    const int n = static_cast<int> (seconds * kFs);
    const auto common = pinkNoise (n, 0.1f, seed);
    const auto sideL = pinkNoise (n, 0.04f, seed + 1), sideR = pinkNoise (n, 0.04f, seed + 2);
    const auto hatNoise = whiteNoise (n, 1.0f, seed + 3);
    Planar p (2, n);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kFs;
        const double beat = std::fmod (t, 0.5), off = std::fmod (t + 0.25, 0.5);
        const double kick = 0.5 * std::exp (-beat / 0.1) * std::sin (kTwoPi * 55.0 * beat);
        const double hat = 0.25 * std::exp (-off / 0.02) * hatNoise[static_cast<size_t> (i)];
        const auto k = static_cast<size_t> (i);
        p.ch[0][k] = static_cast<float> (0.7 * (kick + hat + common[k] + sideL[k]));
        p.ch[1][k] = static_cast<float> (0.7 * (kick + hat + common[k] + sideR[k]));
    }
    return p;
}

void run (ProcessingChain& chain, Planar& buf, int blockSize)
{
    ScopedNoDenormals noDenormals;
    const int n = buf.numSamples();
    for (int pos = 0; pos < n; pos += blockSize)
        chain.process (buf.block (pos, std::min (blockSize, n - pos)));
}

/** A chain as the offline renderer runs one: prepared, primed with a silent
    block and reset, so it starts at its settings. */
Planar render (const Planar& input, const std::function<void (ParameterStore&)>& settings, bool smart, int blockSize = 512)
{
    ParameterStore store;
    settings (store);
    ProcessingChain chain (store);
    chain.setSmartMacros (smart);
    chain.prepare ({ kFs, blockSize, 2 });
    Planar prime (2, blockSize);
    run (chain, prime, blockSize);
    chain.reset();
    Planar out = input;
    run (chain, out, blockSize);
    return out;
}

/** The limited master: the programme through the maximizer alone at a high
    drive, clipper first, -0.3 dBFS ceiling. */
Planar limitedMaster (const Planar& raw)
{
    return render (
        raw,
        [] (ParameterStore& s) {
            for (int id : { EqOn, DynEqOn, BassOn, ClarityOn, SaturationOn, SpatialOn, VirtualizerOn, CompressorOn })
                s.set (id, 0.0f);
            s.set (MaxDriveDb, 24.0f);
            s.set (MaxCeilingDb, -0.3f);
            s.set (MaxClipAmount, 1.0f);
        },
        false);
}

struct Levels
{
    double integratedLufs = -160.0, truePeakDb = -160.0;
};

Levels measure (Planar& p, int skipSamples = 0)
{
    LoudnessMeter meter;
    meter.prepare (kFs, 2);
    TruePeakMeter tp;
    tp.prepare (2);
    auto b = p.block (skipSamples, p.numSamples() - skipSamples);
    meter.process (b);
    tp.process (b);
    return { meter.getIntegratedLufs(), tp.getMaxDbAllChannels() };
}

/** Music mode at Punch `punch` (0..1); everything else at its default. */
std::function<void (ParameterStore&)> punchAt (float punch)
{
    return [punch] (ParameterStore& s) { s.set (Macro1, punch); };
}

bool identical (const Planar& a, const Planar& b)
{
    return a.ch == b.ch;
}

bool probe()
{
    #if defined(_MSC_VER)
    char* value = nullptr;
    size_t length = 0;
    const bool set = _dupenv_s (&value, &length, "FLUB_E34_PROBE") == 0 && value != nullptr;
    std::free (value);
    return set;
    #else
    return std::getenv ("FLUB_E34_PROBE") != nullptr;
    #endif
}
} // namespace

//==============================================================================
TEST_CASE ("ContentAnalysis (E34): pink noise reads a flat tilt and high tilt, white noise +3 dB per octave, its sub-100 Hz share the pink share")
{
    const int n = static_cast<int> (4.0 * kFs);
    Planar pink = stereo (pinkNoise (n, 0.1f, 11), pinkNoise (n, 0.1f, 12));
    Planar white = stereo (whiteNoise (n, 0.1f, 13), whiteNoise (n, 0.1f, 14));
    const auto p = analyse (pink);
    const auto w = analyse (white);
    if (probe())
        std::printf ("pink tilt %.2f high %.2f low %.2f corr %.3f side %.2f | white tilt %.2f high %.2f low %.2f\n", p.tiltDbPerOctave,
                     p.highTiltDb, p.lowShareDb, p.correlation, p.sideDb, w.tiltDbPerOctave, w.highTiltDb, w.lowShareDb);
    CHECK (p.valid);
    CHECK_NEAR (p.tiltDbPerOctave, 0.0, 0.3);
    CHECK_NEAR (p.highTiltDb, 0.0, 1.0);
    CHECK_NEAR (w.tiltDbPerOctave, 3.0, 0.3);
    // The test's pink noise keeps its -3 dB / octave down to about 10 Hz:
    // 3.3 of 11.2 octaves below 100 Hz (-5.3 dB), a little more in practice.
    CHECK_NEAR (p.lowShareDb, -4.6, 1.0);
    CHECK_LE (w.lowShareDb, -20.0);
    // Two independent channels: uncorrelated, side as strong as mid.
    CHECK_NEAR (p.correlation, 0.0, 0.05);
    CHECK_NEAR (p.sideDb, 0.0, 0.3);
}

TEST_CASE ("ContentAnalysis (E34): correlation and M/S width of mono, wide and inverted programme; a sine's crest 3 dB")
{
    const int n = static_cast<int> (2.0 * kFs);
    const auto a = pinkNoise (n, 0.1f, 21);
    std::vector<float> inv (a.size());
    std::transform (a.begin(), a.end(), inv.begin(), [] (float x) { return -x; });
    Planar mono = stereo (a, a), inverted = stereo (a, inv);
    const auto m = analyse (mono), v = analyse (inverted);
    CHECK_NEAR (m.correlation, 1.0, 1.0e-3);
    CHECK_LE (m.sideDb, -100.0);
    CHECK_NEAR (v.correlation, -1.0, 1.0e-3);
    const auto tone = sine (1000.0, kFs, n, 0.5f);
    Planar s = stereo (tone, tone);
    const auto t = analyse (s);
    CHECK_NEAR (t.crestDb, 3.01, 0.05);
    CHECK_NEAR (t.peakDbfs, -6.02, 0.05);
}

TEST_CASE ("ContentAnalysis (E34): a limited master reads PLR < 8 LU, the programme it came from 12+; the kicks read as onsets, pink noise as none")
{
    Planar raw = musicProgramme (6.0);
    Planar hot = limitedMaster (raw);
    const auto r = analyse (raw), h = analyse (hot);
    const int n = static_cast<int> (4.0 * kFs);
    Planar pink = stereo (pinkNoise (n, 0.1f, 31), pinkNoise (n, 0.1f, 32));
    const auto p = analyse (pink);
    if (probe())
        std::printf ("raw PLR %.2f crest %.2f L %.2f onsets %.2f flux %.2f low %.2f | hot PLR %.2f crest %.2f L %.2f onsets %.2f | pink onsets %.2f flux %.2f\n",
                     r.plrDb, r.crestDb, r.loudnessLufs, r.onsetsPerSecond, r.fluxDb, r.lowShareDb, h.plrDb, h.crestDb, h.loudnessLufs,
                     h.onsetsPerSecond, p.onsetsPerSecond, p.fluxDb);
    CHECK_GE (r.plrDb, 12.0);
    CHECK_LE (h.plrDb, 8.0);
    CHECK_GE (h.loudnessLufs, r.loudnessLufs + 6.0);
    // Two kicks and two hats a second (the first kick has no history).
    CHECK_NEAR (r.onsetsPerSecond, 4.0, 0.5);
    CHECK_LE (p.onsetsPerSecond, 1.0);
    CHECK_GE (r.fluxDb, p.fluxDb);
}

TEST_CASE ("ContentAnalysis (E34): frames close on the sample count - the state after 64-sample and 4096-sample blocks is the same, silence holds it")
{
    Planar x = musicProgramme (3.0, 41);
    const int n = x.numSamples();
    auto stateWith = [&x, n] (int blockSize, int silenceSamples) {
        ContentAnalysis a;
        a.prepare (kFs);
        for (int pos = 0; pos < n; pos += blockSize)
            a.process (x.block (pos, std::min (blockSize, n - pos)));
        Planar quiet (2, silenceSamples);
        for (int pos = 0; pos < silenceSamples; pos += blockSize)
            a.process (quiet.block (pos, std::min (blockSize, silenceSamples - pos)));
        return a.getState();
    };
    const auto s64 = stateWith (64, 0), s4096 = stateWith (4096, 0), held = stateWith (512, static_cast<int> (2.0 * kFs));
    CHECK (s64.valid);
    CHECK (s64.frames == 30u); // 3 s of 100 ms frames
    CHECK (s64.frames == s4096.frames);
    CHECK (s64.plrDb == s4096.plrDb);
    CHECK (s64.tiltDbPerOctave == s4096.tiltDbPerOctave);
    CHECK (s64.onsetsPerSecond == s4096.onsetsPerSecond);
    // Two seconds of digital silence add no frame and change nothing.
    CHECK (held.frames == s64.frames);
    CHECK (held.plrDb == s64.plrDb);
    CHECK (held.lowShareDb == s64.lowShareDb);
}

TEST_CASE ("MacroMap (E34): Smart modulation - identity while not valid; attack and drive shrink below 10.5 LU of PLR; bass on LF excess, air on HF tilt; all 1 is bit-identical")
{
    AnalysisState s;
    CHECK (MacroMap::smartModulation (s).isIdentity());
    s.valid = true;
    s.plrDb = 14.0f;
    s.lowShareDb = -8.0f;
    s.highTiltDb = -10.0f;
    CHECK (MacroMap::smartModulation (s).isIdentity());
    s.plrDb = 6.0f;
    auto m = MacroMap::smartModulation (s);
    CHECK (m.attack == 0.0f);
    CHECK (m.drive == MacroMap::kSmartDriveFloor);
    s.plrDb = 0.5f * (MacroMap::kSmartPlrZero + MacroMap::kSmartPlrFull);
    m = MacroMap::smartModulation (s);
    CHECK_NEAR (m.attack, 0.5, 1.0e-6);
    s.lowShareDb = 0.5f;
    s.highTiltDb = 1.0f;
    m = MacroMap::smartModulation (s);
    CHECK_NEAR (m.bass, 1.0 - MacroMap::kSmartBassCut, 1.0e-6);
    CHECK_NEAR (m.air, 1.0 - MacroMap::kSmartAirCut, 1.0e-6);
    CHECK (m.forParam (ClarityAttackDb) == m.attack && m.forParam (ClarityAttackHighDb) == m.attack);
    CHECK (m.forParam (MaxDriveDb) == m.drive && m.forParam (SatDriveDb) == m.drive);
    CHECK (m.forParam (BassBoostDb) == m.bass && m.forParam (ClarityAir) == m.air);
    CHECK (m.forParam (ClarityPresence) == 1.0f);

    // Boost 100 and every macro at 100 in both modes: the identity changes nothing.
    for (float mode : { 0.0f, 1.0f })
    {
        std::vector<float> base (static_cast<size_t> (kNumParams));
        for (int id = 0; id < kNumParams; ++id)
            base[static_cast<size_t> (id)] = layout()[static_cast<size_t> (id)].defaultValue;
        base[static_cast<size_t> (Mode)] = mode;
        for (int id : { BoostIntensity, Macro1, Macro2, Macro3, Macro4, Macro5 })
            base[static_cast<size_t> (id)] = 1.0f;
        std::vector<float> a (base.size()), b (base.size()), c (base.size());
        const MacroModulation identity;
        MacroMap::apply (base.data(), a.data(), 0.7f);
        MacroMap::apply (base.data(), b.data(), 0.7f, 0.0f, &identity);
        CHECK (a == b);
        MacroModulation half;
        half.attack = half.drive = 0.5f;
        MacroMap::apply (base.data(), c.data(), 0.7f, 0.0f, &half);
        CHECK (c[static_cast<size_t> (ClarityAttackDb)] < a[static_cast<size_t> (ClarityAttackDb)]);
        CHECK (c[static_cast<size_t> (MaxDriveDb)] < a[static_cast<size_t> (MaxDriveDb)]);
        CHECK (c[static_cast<size_t> (ClarityPresence)] == a[static_cast<size_t> (ClarityPresence)]);
    }
}

TEST_CASE ("Chain (E34): Smart off is bit-identical to a chain that never had it; Smart on renders bit-identical twice")
{
    Planar raw = musicProgramme (3.0, 51);
    const auto settings = [] (ParameterStore& s) {
        s.set (BoostIntensity, 0.4f);
        s.set (Macro1, 1.0f);
    };
    const Planar plain = render (raw, settings, false);
    {
        // Smart switched on and off again before the programme: nothing moved.
        ParameterStore store;
        settings (store);
        ProcessingChain chain (store);
        chain.setSmartMacros (true);
        chain.setSmartMacros (false);
        chain.prepare ({ kFs, 512, 2 });
        Planar prime (2, 512);
        run (chain, prime, 512);
        chain.reset();
        Planar out = raw;
        run (chain, out, 512);
        CHECK (identical (out, plain));
    }
    const Planar a = render (raw, settings, true), b = render (raw, settings, true);
    CHECK (identical (a, b));
}

TEST_CASE ("Chain (E34): the Smart render does not depend on the host block size (64 vs 1024 samples)")
{
    Planar hot = limitedMaster (musicProgramme (3.0, 61));
    const Planar a = render (hot, punchAt (1.0f), true, 64), b = render (hot, punchAt (1.0f), true, 1024);
    double maxDiff = 0.0;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < a.numSamples(); ++i)
            maxDiff = std::max (maxDiff, static_cast<double> (std::abs (a.ch[static_cast<size_t> (c)][static_cast<size_t> (i)]
                                                                        - b.ch[static_cast<size_t> (c)][static_cast<size_t> (i)])));
    if (probe())
        std::printf ("block size diff %.3g\n", maxDiff);
    CHECK_LE (maxDiff, 1.0e-4);
}

namespace
{
/** The Done-when's limited master, made once (6 s, PLR about 7.5 LU). */
Planar& doneWhenMaster()
{
    static Planar hot = limitedMaster (musicProgramme (6.0, 71));
    return hot;
}

/** Punch 0 against Punch 100 over `base` on the limited master, Smart on
    and off: integrated loudness and true peak. */
void checkPunchOnLimitedMaster (const std::function<void (ParameterStore&)>& base)
{
    Planar& hot = doneWhenMaster();
    const auto h = analyse (hot);
    REQUIRE (h.plrDb < 8.0f);
    const auto with = [&base] (float punch) {
        return [&base, punch] (ParameterStore& s) {
            base (s);
            s.set (Macro1, punch);
        };
    };
    Planar p0 = render (hot, with (0.0f), true), p100 = render (hot, with (1.0f), true);
    Planar s0 = render (hot, with (0.0f), false), s100 = render (hot, with (1.0f), false);
    const auto l0 = measure (p0), l100 = measure (p100), m0 = measure (s0), m100 = measure (s100);
    if (probe())
        std::printf ("hot PLR %.2f | smart P0 %.3f / %.2f  P100 %.3f / %.2f | static P0 %.3f / %.2f  P100 %.3f / %.2f\n", h.plrDb,
                     l0.integratedLufs, l0.truePeakDb, l100.integratedLufs, l100.truePeakDb, m0.integratedLufs, m0.truePeakDb,
                     m100.integratedLufs, m100.truePeakDb);
    CHECK_GE (l100.integratedLufs, l0.integratedLufs - 0.1);
    CHECK_LE (l100.truePeakDb, l0.truePeakDb + 0.5);
    // The static macro on the same master: the loss Smart takes away.
    CHECK_LE (m100.integratedLufs, m0.integratedLufs - 0.1);
}
} // namespace

TEST_CASE ("Chain (E34) Done-when: on a limited master (PLR < 8) Smart Punch 100 costs <= 0.1 LU and <= 0.5 dB of true peak; static Punch costs more")
{
    checkPunchOnLimitedMaster ([] (ParameterStore&) {});
}

TEST_CASE ("Chain (E34) Done-when: the same over Boost 40 + Loudness 60 (the maximizer driven), where static Punch costs 0.5 LU")
{
    checkPunchOnLimitedMaster ([] (ParameterStore& s) {
        s.set (BoostIntensity, 0.4f);
        s.set (Macro4, 0.6f);
    });
}

TEST_CASE ("Chain (E34) Done-when: open programme (PLR 12+) with Smart renders within 0.2 LU of static, Punch 100 and Boost 60 + Loudness 60")
{
    Planar raw = musicProgramme (5.0, 81);
    REQUIRE (analyse (raw).plrDb >= 12.0f);
    const std::function<void (ParameterStore&)> cases[] = {
        punchAt (1.0f),
        [] (ParameterStore& s) {
            s.set (BoostIntensity, 0.6f);
            s.set (Macro4, 0.6f);
        },
    };
    for (const auto& settings : cases)
    {
        Planar a = render (raw, settings, true), b = render (raw, settings, false);
        const auto la = measure (a), lb = measure (b);
        if (probe())
            std::printf ("open smart %.3f static %.3f\n", la.integratedLufs, lb.integratedLufs);
        CHECK_NEAR (la.integratedLufs, lb.integratedLufs, 0.2);
    }
}

TEST_CASE ("Chain (E34): the tap publishes its state and the multipliers; off, they glide back to 1 and the modulation is dropped")
{
    Planar hot = limitedMaster (musicProgramme (4.0, 91));
    ParameterStore store;
    store.set (Macro1, 1.0f);
    ProcessingChain chain (store);
    AnalysisState s;
    chain.prepare ({ kFs, 480, 2 });
    CHECK (! chain.getContentAnalysis (s));
    chain.setSmartMacros (true);
    run (chain, hot, 480);
    REQUIRE (chain.getContentAnalysis (s));
    CHECK (s.valid);
    CHECK_LE (s.plrDb, 8.0);
    const auto m = chain.getSmartModulation();
    CHECK_LE (m.attack, 0.02);
    CHECK_LE (chain.effectiveValue (ClarityAttackDb), 0.1);
    // Off: 1 s later the attack is on its way back, after 2 s it is all back.
    chain.setSmartMacros (false);
    Planar more = limitedMaster (musicProgramme (1.0, 92));
    run (chain, more, 480);
    const auto mid = chain.getSmartModulation();
    CHECK (mid.attack > 0.3f && mid.attack < 0.7f);
    Planar rest = limitedMaster (musicProgramme (1.1, 93));
    run (chain, rest, 480);
    CHECK (chain.getSmartModulation().isIdentity());
    CHECK_NEAR (chain.effectiveValue (ClarityAttackDb), 6.0, 1.0e-6);
}

TEST_CASE ("PresetIO (E34): \"smart\" reads and writes, is in contentHash only when true, and a wrong type is a warning")
{
    preset::Preset p = preset::makeDefault();
    const std::string plainHash = preset::contentHash (p);
    CHECK (preset::toJson (p)["smart"].isNull());
    p.smart = true;
    CHECK (preset::contentHash (p) != plainHash);
    const auto v = preset::toJson (p);
    CHECK (v["smart"].isBool() && v["smart"].asBool());
    preset::Preset q;
    std::string error;
    REQUIRE (preset::fromJson (v, q, error));
    CHECK (q.smart);
    CHECK (preset::contentHash (q) == preset::contentHash (p));
    auto bad = v;
    bad.set ("smart", "yes");
    REQUIRE (preset::fromJson (bad, q, error));
    CHECK (! q.smart);
    CHECK (! q.warnings.empty());
}
