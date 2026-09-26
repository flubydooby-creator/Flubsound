// Engine-level tests: parameter layout/store, macros, protection loops,
// module slot, full processing chain, mixer and presets.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/analysis/PeakMeters.h"
#include "flub/common/Denormals.h"
#include "flub/engine/MixEngine.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/io/PresetIO.h"

#include <limits>
#include <set>

using namespace flub;
using namespace flub::param;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;

/** Drum-like test programme: kick-ish low bursts + noise hats + a bass line. */
Planar makeProgramme (int numSamples, float level = 0.5f, uint32_t seed = 7)
{
    Planar p (2, numSamples);
    FastRandom rng (seed);
    for (int i = 0; i < numSamples; ++i)
    {
        const double t = i / kFs;
        const double beat = std::fmod (t, 0.5);
        const double kick = std::exp (-beat * 18.0) * std::sin (kTwoPi * (50.0 + 80.0 * std::exp (-beat * 30.0)) * beat);
        const double hat = (std::fmod (t + 0.25, 0.5) < 0.03 ? 0.3 : 0.0) * rng.nextBipolar();
        const double bassLine = 0.4 * std::sin (kTwoPi * 55.0 * t);
        const double pad = 0.15 * std::sin (kTwoPi * 440.0 * t) + 0.1 * std::sin (kTwoPi * 660.0 * t + 0.3);
        const float l = level * static_cast<float> (kick + hat + bassLine + pad);
        const float r = level * static_cast<float> (kick + 0.8 * hat + bassLine + 0.7 * pad);
        p.ch[0][static_cast<size_t> (i)] = l;
        p.ch[1][static_cast<size_t> (i)] = r;
    }
    return p;
}

void runChain (ProcessingChain& chain, Planar& buf, int blockSize)
{
    ScopedNoDenormals noDenormals;
    const int n = buf.numSamples();
    for (int pos = 0; pos < n; pos += blockSize)
        chain.process (buf.block (pos, std::min (blockSize, n - pos)));
}

void bypassAllModules (ParameterStore& s)
{
    for (int id : { GateOn, EqOn, DynEqOn, BassOn, ClarityOn, SaturationOn, SpatialOn, VirtualizerOn, CompressorOn, MaximizerOn })
        s.set (id, 0.0f);
}
} // namespace

// ---------------------------------------------------------------------------
TEST_CASE ("Parameters: layout is complete, keys unique, defaults in range")
{
    const auto& t = layout();
    REQUIRE (static_cast<int> (t.size()) == kNumParams);
    std::set<std::string> keys;
    std::set<int> versions;
    for (const auto& i : t)
    {
        CHECK (! i.key.empty());
        CHECK (! i.name.empty());
        CHECK (i.minValue <= i.defaultValue && i.defaultValue <= i.maxValue);
        if (i.unit == Unit::Choice)
            CHECK (static_cast<int> (i.choices.size()) == static_cast<int> (i.maxValue) + 1);
        CHECK (i.sinceVersion >= 1); // plug-in version hint; 0 would mean "unversioned" to JUCE
        keys.insert (i.key);
        versions.insert (i.sinceVersion);
    }
    CHECK (static_cast<int> (keys.size()) == kNumParams);
    // Every release that adds parameters takes the next version: 1..N, no gaps.
    CHECK (! versions.empty() && *versions.begin() == 1 && *versions.rbegin() == static_cast<int> (versions.size()));
    CHECK (findByKey ("eq.3.freq") == eq (3, EqFieldFreq));
    CHECK (findByKey ("dyneq.1.threshold") == dyn (1, DynFieldThreshold));
    CHECK (findByKey ("nope") == -1);
    CHECK (t[static_cast<size_t> (LatencyProfile)].structural);
    CHECK (kEqBands >= 10); // requirement: fully parametric EQ with at least 10 bands
}

TEST_CASE ("ParameterStore: clamping, banks, snapshot, version")
{
    ParameterStore s;
    const auto v0 = s.version();
    s.set (MaxDriveDb, 99.0f);
    CHECK (s.get (MaxDriveDb) == 24.0f);
    CHECK (s.version() > v0);
    s.set (Bank::B, MaxDriveDb, 3.0f);
    CHECK (s.get (Bank::A, MaxDriveDb) == 24.0f);
    s.setActiveBank (Bank::B);
    CHECK (s.get (MaxDriveDb) == 3.0f);
    s.copyBank (Bank::A, Bank::B);
    CHECK (s.get (MaxDriveDb) == 24.0f);
    std::vector<float> snap (static_cast<size_t> (kNumParams));
    AllocationGuard guard;
    s.snapshot (snap.data());
    CHECK (guard.allocations() == 0);
    CHECK (snap[static_cast<size_t> (MaxDriveDb)] == 24.0f);
}

TEST_CASE ("ParameterStore: NaN is ignored, infinities clamp, and the chain stays finite")
{
    ParameterStore store;
    store.set (OutputGainDb, -3.0f);
    store.set (OutputGainDb, std::numeric_limits<float>::quiet_NaN());
    CHECK (store.get (OutputGainDb) == -3.0f);
    store.set (InputGainDb, std::numeric_limits<float>::infinity());
    CHECK (store.get (InputGainDb) == layout()[static_cast<size_t> (InputGainDb)].maxValue);
    store.set (InputGainDb, -std::numeric_limits<float>::infinity());
    CHECK (store.get (InputGainDb) == layout()[static_cast<size_t> (InputGainDb)].minValue);
    CHECK (layout()[static_cast<size_t> (MaxDriveDb)].clamp (std::numeric_limits<float>::quiet_NaN()) == layout()[static_cast<size_t> (MaxDriveDb)].defaultValue);

    // Every parameter hit with NaN: the chain output stays finite.
    ParameterStore s2;
    for (int i = 0; i < kNumParams; ++i)
        s2.set (i, std::numeric_limits<float>::quiet_NaN());
    ProcessingChain chain (s2);
    chain.prepare ({ kFs, 512, 2 });
    auto buf = makeProgramme (48000, 0.5f);
    runChain (chain, buf, 512);
    for (auto& c : buf.ch)
        for (float v : c)
            REQUIRE (std::isfinite (v));
}

TEST_CASE ("MacroMap: zero macros leave base values untouched")
{
    std::vector<float> base (static_cast<size_t> (kNumParams)), eff (static_cast<size_t> (kNumParams));
    for (int i = 0; i < kNumParams; ++i)
        base[static_cast<size_t> (i)] = layout()[static_cast<size_t> (i)].defaultValue;
    for (int mode : { 0, 1 })
    {
        base[Mode] = static_cast<float> (mode);
        MacroMap::apply (base.data(), eff.data(), 1.0f);
        for (int i = 0; i < kNumParams; ++i)
            CHECK (eff[static_cast<size_t> (i)] == base[static_cast<size_t> (i)]);
    }
}

TEST_CASE ("MacroMap: Boost Intensity is staged and governed")
{
    std::vector<float> base (static_cast<size_t> (kNumParams)), eff (static_cast<size_t> (kNumParams)), half (static_cast<size_t> (kNumParams));
    for (int i = 0; i < kNumParams; ++i)
        base[static_cast<size_t> (i)] = layout()[static_cast<size_t> (i)].defaultValue;

    base[BoostIntensity] = 0.2f;
    MacroMap::apply (base.data(), eff.data(), 1.0f);
    CHECK (eff[ClarityPresence] > 0.0f);  // clarity arrives early
    CHECK (eff[MaxDriveDb] == 0.0f);      // loudness does not yet
    base[BoostIntensity] = 1.0f;
    MacroMap::apply (base.data(), eff.data(), 1.0f);
    CHECK_NEAR (eff[MaxDriveDb], 8.0, 1e-4);
    CHECK_NEAR (eff[BassBoostDb], 5.0, 1e-4);
    MacroMap::apply (base.data(), half.data(), 0.5f);
    CHECK_NEAR (half[MaxDriveDb], 4.0, 1e-4);           // governed
    CHECK_NEAR (half[ClarityPresence], eff[ClarityPresence], 1e-6); // not governed
    for (int i = 0; i < kNumParams; ++i)
    {
        const auto& info = layout()[static_cast<size_t> (i)];
        CHECK (eff[static_cast<size_t> (i)] >= info.minValue && eff[static_cast<size_t> (i)] <= info.maxValue);
    }
}

TEST_CASE ("MacroMap: glue is armed only while a source that can raise it is off zero")
{
    std::vector<float> base (static_cast<size_t> (kNumParams));
    for (int i = 0; i < kNumParams; ++i)
        base[static_cast<size_t> (i)] = layout()[static_cast<size_t> (i)].defaultValue;
    base[Mode] = static_cast<float> (ModeValue::Music);
    CHECK (! MacroMap::isArmed (base.data(), MaxGlue));
    base[BoostIntensity] = 0.2f; // below the glue start point (40 %), but armed
    CHECK (MacroMap::isArmed (base.data(), MaxGlue));
    base[BoostIntensity] = 0.0f;
    base[Macro4] = 0.1f;         // Loudness
    CHECK (MacroMap::isArmed (base.data(), MaxGlue));
    base[Macro4] = 0.0f;
    base[Macro1] = 1.0f;         // Punch never raises glue
    CHECK (! MacroMap::isArmed (base.data(), MaxGlue));
}

TEST_CASE ("MacroMap: every mode has five named macros that engage modules")
{
    for (auto mode : { ModeValue::Music, ModeValue::Gaming })
    {
        for (int m = 0; m < 5; ++m)
            CHECK (std::string (MacroMap::macroName (mode, m)).size() > 0);
        std::vector<float> base (static_cast<size_t> (kNumParams)), eff (static_cast<size_t> (kNumParams));
        for (int i = 0; i < kNumParams; ++i)
            base[static_cast<size_t> (i)] = layout()[static_cast<size_t> (i)].defaultValue;
        base[Mode] = static_cast<float> (mode);
        base[SaturationOn] = 0.0f;
        base[Macro5] = 0.5f;
        MacroMap::apply (base.data(), eff.data(), 1.0f);
        if (mode == ModeValue::Music)
            CHECK (eff[SaturationOn] >= 0.5f); // Warmth engages saturation
        else
            CHECK (eff[ClarityPresence] > 0.0f); // Voice & Score raises presence
    }
}

TEST_CASE ("SafetyGovernor: backs off under sustained over-limiting and recovers")
{
    SafetyGovernor g;
    g.prepare (kFs);
    for (int i = 0; i < 48000 * 6 / 480; ++i)
        g.update (-12.0f, -40.0f, 480);
    CHECK (g.getScale() < 0.6f);
    CHECK (g.getScale() >= 0.3f);
    const float low = g.getScale();
    for (int i = 0; i < 48000 * 10 / 480; ++i)
        g.update (-1.0f, -60.0f, 480);
    CHECK (g.getScale() > low);
}

TEST_CASE ("AutoLevel: brings a quiet source towards the target, slew limited, frozen in silence")
{
    AutoLevel al;
    al.prepare (kFs, 2);
    al.setEnabled (true);
    al.setTargetLufs (-18.0f);
    // -30 dBFS stereo 1 kHz sine is ~ -30 LUFS: needs +12 dB.
    const int block = 480;
    for (int b = 0; b < 100; ++b) // 1 s: slew limit is +1 dB/s
    {
        Planar p (2, block);
        auto s = sine (1000.0, kFs, block, dbToGain (-30.0f), b * block * kTwoPi * 1000.0 / kFs);
        p.ch[0] = s;
        p.ch[1] = s;
        al.process (p.block());
    }
    CHECK (al.getGainDb() > 0.0f);
    CHECK (al.getGainDb() <= 1.05f);
    const float held = al.getGainDb();
    for (int b = 0; b < 400; ++b) // 4 s of silence: frozen
    {
        Planar p (2, block);
        al.process (p.block());
    }
    CHECK_NEAR (al.getGainDb(), held, 0.3);
}

TEST_CASE ("ModuleSlot: bypassed slot is a pure latency-compensated delay and toggling is click-free")
{
    TruePeakLimiter lim; // has latency
    ModuleSlot slot;
    slot.prepare (lim, { kFs, 256, 2 }, 20.0f, true);
    const int lat = slot.latencySamples();
    REQUIRE (lat > 0);

    const int n = 48000;
    Planar buf (2, n);
    auto s = sine (440.0, kFs, n, 0.25f);
    buf.ch[0] = s;
    buf.ch[1] = s;
    for (int pos = 0, b = 0; pos < n; pos += 256, ++b)
    {
        slot.setActive (b < 40 || b > 120); // off in the middle
        slot.process (buf.block (pos, std::min (256, n - pos)));
    }
    // Low-level sine is untouched by the limiter, so wet == dry and the whole
    // output (after the first `lat` samples) must be the delayed input.
    double maxErr = 0.0;
    for (int i = lat; i < n; ++i)
        maxErr = std::max (maxErr, static_cast<double> (std::abs (buf.ch[0][static_cast<size_t> (i)] - s[static_cast<size_t> (i - lat)])));
    CHECK_LE (maxErr, 1e-5);
}

// ---------------------------------------------------------------------------
TEST_CASE ("Chain: latency per profile and constant under module bypass")
{
    ParameterStore store;
    ProcessingChain chain (store);
    int latencies[3] {};
    for (int profile = 0; profile < 3; ++profile)
    {
        store.set (LatencyProfile, static_cast<float> (profile));
        chain.prepare ({ kFs, 512, 2 });
        latencies[profile] = chain.getLatencySamples();
        CHECK (! chain.needsReprepare());
    }
    CHECK (latencies[0] > latencies[1]);
    CHECK (latencies[1] > latencies[2]);
    CHECK (latencies[1] * 1000.0 / kFs < 5.0);  // Balanced: < 5 ms algorithmic
    CHECK (latencies[2] * 1000.0 / kFs < 2.5);  // Low Latency: < 2.5 ms
    store.set (LatencyProfile, 0.0f);
    CHECK (chain.needsReprepare());
}

TEST_CASE ("Chain: everything bypassed = input delayed by the chain latency (bit-transparent path)")
{
    ParameterStore store;
    bypassAllModules (store);
    ProcessingChain chain (store);
    chain.prepare ({ kFs, 256, 2 });
    const int lat = chain.getLatencySamples();
    auto prog = makeProgramme (48000, 0.3f);
    Planar buf = prog;
    buf.ptrs.clear();
    for (auto& c : buf.ch)
        buf.ptrs.push_back (c.data());
    runChain (chain, buf, 256);
    double maxErr = 0.0;
    for (int c = 0; c < 2; ++c)
        for (int i = lat; i < 48000; ++i)
            maxErr = std::max (maxErr, static_cast<double> (std::abs (buf.ch[static_cast<size_t> (c)][static_cast<size_t> (i)] - prog.ch[static_cast<size_t> (c)][static_cast<size_t> (i - lat)])));
    CHECK_LE (maxErr, 1e-6);
}

TEST_CASE ("Chain: with glue disarmed the maximizer passes hot flat-topped material untouched")
{
    // The glue's 3-band splitter is an all-pass that raises the crest factor
    // of flat-topped material (+3.4 dB on this square). With no glue in the
    // preset and no macro that could raise it, the splitter must be out of the
    // path: the default maximizer (drive 0, clipper not reached, ceiling
    // -1 dBTP) must return the square exactly, delayed. At -5 dBFS its true
    // peak (Gibbs ringing at the edges) is -2.9 dBTP; through the splitter it
    // would read about +0.5 dBTP and be limited.
    ParameterStore store;
    bypassAllModules (store);
    store.set (MaximizerOn, 1.0f);
    ProcessingChain chain (store);
    chain.prepare ({ kFs, 512, 2 });
    const int lat = chain.getLatencySamples();
    const int n = 48000;
    Planar in (2, n);
    for (int i = 0; i < n; ++i)
        for (auto& c : in.ch)
            c[static_cast<size_t> (i)] = ((i / 240) % 2 == 0 ? 1.0f : -1.0f) * dbToGain (-5.0f); // 100 Hz
    Planar buf = in;
    buf.ptrs.clear();
    for (auto& c : buf.ch)
        buf.ptrs.push_back (c.data());
    runChain (chain, buf, 512);
    double maxErr = 0.0;
    for (int c = 0; c < 2; ++c)
        for (int i = lat; i < n; ++i)
            maxErr = std::max (maxErr, static_cast<double> (std::abs (buf.ch[static_cast<size_t> (c)][static_cast<size_t> (i)] - in.ch[static_cast<size_t> (c)][static_cast<size_t> (i - lat)])));
    CHECK_LE (maxErr, 1e-6);
    CHECK (chain.meters().maxGainReductionDb.load() == 0.0f);
}

TEST_CASE ("Chain: audition bypass holds a macro-engaged module off (hold-to-bypass A/B)")
{
    // Punch (Music macro 1) switches Clarity on although its own enable
    // parameter is off. The GUI's "listen without this module" must still
    // take it out: the output becomes the input, delayed, after the fade.
    auto render = [] (bool audition) {
        ParameterStore store;
        bypassAllModules (store);
        store.set (Mode, static_cast<float> (ModeValue::Music));
        store.set (Macro1, 1.0f);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, 256, 2 });
        CHECK (chain.effectiveValue (ClarityOn) >= 0.5f); // engaged by the macro
        chain.setAuditionBypass (ClarityOn, audition);
        CHECK (chain.isAuditionBypassed (ClarityOn) == audition);
        auto prog = makeProgramme (48000, 0.3f);
        Planar buf = prog;
        buf.ptrs.clear();
        for (auto& c : buf.ch)
            buf.ptrs.push_back (c.data());
        runChain (chain, buf, 256);
        const int lat = chain.getLatencySamples();
        double maxErr = 0.0;
        for (int c = 0; c < 2; ++c)
            for (int i = 4800 + lat; i < 48000; ++i) // after the 20 ms bypass fade
                maxErr = std::max (maxErr, static_cast<double> (std::abs (buf.ch[static_cast<size_t> (c)][static_cast<size_t> (i)] - prog.ch[static_cast<size_t> (c)][static_cast<size_t> (i - lat)])));
        return maxErr;
    };
    CHECK (render (false) > 1e-3);  // Clarity audibly active
    CHECK_LE (render (true), 1e-6); // held off despite the macro
    ParameterStore store;
    ProcessingChain chain (store);
    chain.setAuditionBypass (InputGainDb, true); // not a module: ignored
    CHECK (! chain.isAuditionBypassed (InputGainDb));
}

TEST_CASE ("Chain: full Music boost on a hot programme never exceeds the ceiling")
{
    for (int mode : { 0, 1 })
    {
        ParameterStore store;
        store.set (Mode, static_cast<float> (mode));
        store.set (BoostIntensity, 1.0f);
        for (int m = Macro1; m <= Macro5; ++m)
            store.set (m, 1.0f);
        store.set (MaxCeilingDb, -1.0f);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, 512, 2 });
        auto buf = makeProgramme (48000 * 4, 0.9f);
        runChain (chain, buf, 512);

        TruePeakMeter tp;
        tp.prepare (2);
        tp.process (buf.block());
        CHECK_LE (tp.getMaxDbAllChannels(), -1.0 + 0.15);
        CHECK_LE (peakAbs (buf.ch[0].data(), buf.numSamples()), dbToGain (-1.0f) + 1e-6);
        CHECK (chain.meters().safetyClipCount.load() == 0);
        for (auto& c : buf.ch)
            for (float v : c)
                REQUIRE (std::isfinite (v));
    }
}

TEST_CASE ("Chain: process() is allocation-free in every mode and profile")
{
    for (int profile = 0; profile < 3; ++profile)
    {
        ParameterStore store;
        store.set (LatencyProfile, static_cast<float> (profile));
        store.set (GateOn, 1.0f);
        store.set (CompressorOn, 1.0f);
        store.set (SaturationOn, 1.0f);
        store.set (BoostIntensity, 0.7f);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, 512, 2 });
        auto buf = makeProgramme (512 * 20, 0.5f);
        AllocationGuard guard;
        for (int b = 0; b < 20; ++b)
        {
            if (b == 10)
            {
                store.set (Mode, 1.0f);           // mode switch mid-stream
                store.setActiveBank (Bank::B);    // A/B switch mid-stream
                store.set (BypassAll, 1.0f);
            }
            chain.process (buf.block (b * 512, 512));
        }
        CHECK (guard.allocations() == 0);
    }
}

TEST_CASE ("Chain: 7.1 input is virtualised (or downmixed) to stereo; extra channels cleared")
{
    for (bool virt : { true, false })
    {
        ParameterStore store;
        store.set (VirtualizerOn, virt ? 1.0f : 0.0f);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, 256, 8 });
        Planar buf (8, 256 * 40);
        auto s = sine (1000.0, kFs, buf.numSamples(), 0.2f);
        buf.ch[6] = s; // side-left only
        runChain (chain, buf, 256);
        const int tail = buf.numSamples() / 2;
        const double left = rms (buf.ch[0].data() + tail, tail);
        const double right = rms (buf.ch[1].data() + tail, tail);
        CHECK (left > 0.01);
        CHECK (left > right); // image stays on the left
        for (int c = 2; c < 8; ++c)
            CHECK (peakAbs (buf.ch[static_cast<size_t> (c)].data(), buf.numSamples()) == 0.0);
    }
}

TEST_CASE ("Chain: toggling the virtualiser on a 7.1 strip crossfades (no step in the output)")
{
    // The binaural render and the BS.775 downmix differ in level and timing,
    // so a hard switch would step the waveform. Measure the largest
    // sample-to-sample change around each switch against steady state.
    ParameterStore store;
    bypassAllModules (store);
    store.set (VirtualizerOn, 1.0f);
    ProcessingChain chain (store);
    constexpr int kBlock = 256;
    chain.prepare ({ kFs, kBlock, 8 });
    const int n = kBlock * 600; // 3.2 s
    Planar buf (8, n);
    for (int c = 0; c < 8; ++c)
        if (c != 3)
        {
            const auto s = sine (220.0 * (1.0 + 0.1 * c), kFs, n, 0.1f);
            std::copy (s.begin(), s.end(), buf.ch[static_cast<size_t> (c)].begin()); // keep the block pointers valid
        }
    const int toggleOff = kBlock * 200, toggleOn = kBlock * 400;
    for (int pos = 0; pos < n; pos += kBlock)
    {
        if (pos == toggleOff)
            store.set (VirtualizerOn, 0.0f);
        if (pos == toggleOn)
            store.set (VirtualizerOn, 1.0f);
        chain.process (buf.block (pos, kBlock));
    }
    auto maxStep = [&] (int from, int to) {
        double m = 0.0;
        for (int c = 0; c < 2; ++c)
            for (int i = from + 1; i < to; ++i)
                m = std::max (m, static_cast<double> (std::abs (buf.ch[static_cast<size_t> (c)][static_cast<size_t> (i)] - buf.ch[static_cast<size_t> (c)][static_cast<size_t> (i - 1)])));
        return m;
    };
    const double steadyOn = maxStep (kBlock * 100, toggleOff);
    const double steadyOff = maxStep (toggleOff + kBlock * 20, toggleOn);
    const double steady = std::max (steadyOn, steadyOff);
    CHECK (steady > 0.0);
    CHECK_LE (maxStep (toggleOff - kBlock, toggleOff + kBlock * 8), 1.25 * steady);
    CHECK_LE (maxStep (toggleOn - kBlock, toggleOn + kBlock * 8), 1.25 * steady);
}

TEST_CASE ("Chain: loudness-matched global bypass tracks the processed loudness without exceeding the ceiling")
{
    ParameterStore store;
    store.set (Macro4, 1.0f); // music loudness: output clearly louder than input
    ProcessingChain chain (store);
    chain.prepare ({ kFs, 512, 2 });
    auto warm = makeProgramme (48000 * 6, 0.2f);
    runChain (chain, warm, 512); // let loudness followers settle
    store.set (BypassAll, 1.0f);
    auto buf = makeProgramme (48000 * 2, 0.2f, 99);
    runChain (chain, buf, 512);
    CHECK (chain.meters().shortTermLufs.load() > -60.0f);
    CHECK_LE (peakAbs (buf.ch[0].data() + 48000, 48000), dbToGain (-1.0f) + 1e-3);
}

TEST_CASE ("MixEngine: strips are summed, padded to equal latency and master-limited")
{
    MixEngine mix;
    mix.configure ({ { "Game", 8, 0.0f, false }, { "Music", 2, 0.0f, false } }, kFs, 256);
    mix.params (0).set (Mode, 1.0f);
    mix.params (1).set (LatencyProfile, 0.0f); // Quality profile on music - needs reprepare
    CHECK (mix.needsReprepare());
    mix.configure ({ { "Game", 8, 0.0f, false }, { "Music", 2, 0.0f, false } }, kFs, 256);
    CHECK (! mix.needsReprepare());
    CHECK (mix.params (1).get (LatencyProfile) == 0.0f); // store preserved across configure

    Planar game (8, 256), music (2, 256), out (2, 256);
    const AudioBlock gb = game.block(), mb = music.block();
    const AudioBlock* inputs[] = { &gb, &mb };
    AllocationGuard guard;
    for (int b = 0; b < 50; ++b)
    {
        for (int i = 0; i < 256; ++i)
        {
            game.ch[0][static_cast<size_t> (i)] = 0.9f * static_cast<float> (std::sin (kTwoPi * 100.0 * (b * 256 + i) / kFs));
            music.ch[0][static_cast<size_t> (i)] = music.ch[1][static_cast<size_t> (i)] = 0.9f * static_cast<float> (std::sin (kTwoPi * 60.0 * (b * 256 + i) / kFs));
        }
        mix.process (inputs, out.block());
        CHECK_LE (peakAbs (out.ch[0].data(), 256), dbToGain (-1.0f) + 1e-6);
    }
    CHECK (guard.allocations() == 0);
    CHECK (mix.getLatencySamples() > 0);
}

TEST_CASE ("Presets: JSON round trip, labels for choices, unknown keys ignored")
{
    preset::Preset p = preset::makeDefault();
    p.name = "Round Trip";
    p.category = "Gaming";
    p.tags = { "fps", "test" };
    p.values[static_cast<size_t> (Mode)] = 1.0f;
    p.values[static_cast<size_t> (MaxDriveDb)] = 7.5f;
    p.values[static_cast<size_t> (eq (2, EqFieldType))] = 3.0f;

    const auto j = preset::toJson (p);
    CHECK (j["params"]["mode"].asString() == "Gaming");
    CHECK (j["params"]["eq.2.type"].asString() == "Low Cut");
    const std::string text = json::write (j);

    json::Value parsed;
    std::string err;
    REQUIRE (json::parse (text, parsed, err));
    preset::Preset back;
    REQUIRE (preset::fromJson (parsed, back, err));
    CHECK (back.name == "Round Trip");
    CHECK (back.tags.size() == 2);
    for (int i = 0; i < kNumParams; ++i)
        CHECK (back.values[static_cast<size_t> (i)] == p.values[static_cast<size_t> (i)]);

    json::Value extra = parsed;
    json::Value params = extra["params"];
    params.set ("future.param", 1.0);
    params.set ("max.drive", 1000.0); // clamped
    extra.set ("params", params);
    REQUIRE (preset::fromJson (extra, back, err));
    CHECK (back.values[static_cast<size_t> (MaxDriveDb)] == 24.0f);

    ParameterStore store;
    preset::applyToStore (back, store, Bank::B);
    CHECK (store.get (Bank::B, MaxDriveDb) == 24.0f);
    CHECK (preset::captureFromStore (store, Bank::B).values[static_cast<size_t> (Mode)] == 1.0f);
}

TEST_CASE ("Chain: a NaN/Inf input block is dropped and the chain recovers")
{
    ParameterStore store;
    store.set (BoostIntensity, 0.5f);
    ProcessingChain chain (store);
    chain.prepare ({ kFs, 256, 2 });
    auto buf = makeProgramme (256 * 20, 0.3f);
    buf.ch[0][256 * 5 + 17] = std::numeric_limits<float>::quiet_NaN();
    buf.ch[1][256 * 9 + 3] = std::numeric_limits<float>::infinity();
    runChain (chain, buf, 256);
    for (auto& c : buf.ch)
        for (float v : c)
            REQUIRE (std::isfinite (v));
    CHECK (rms (buf.ch[0].data() + 256 * 15, 256 * 5) > 1e-3); // audio resumed
}

TEST_CASE ("Chain: runs at every sample rate a headset may use (8 kHz hands-free .. 192 kHz)")
{
    // Bluetooth hands-free (HFP) endpoints run at 8 / 16 kHz, some USB headsets
    // at 22.05 / 24 / 32 kHz; wired and USB audio classes at 44.1 .. 192 kHz.
    for (double sr : { 8000.0, 16000.0, 22050.0, 24000.0, 32000.0, 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
        for (int mode : { 0, 1 })
        {
            ParameterStore store;
            store.set (Mode, static_cast<float> (mode));
            store.set (BoostIntensity, 1.0f);
            for (int m = Macro1; m <= Macro5; ++m)
                store.set (m, 1.0f);
            store.set (CompressorOn, 1.0f);
            store.set (SaturationOn, 1.0f);
            ProcessingChain chain (store);
            chain.prepare ({ sr, 256, 2 });
            // FIR oversampling latency is fixed in samples, so it grows in ms at
            // narrowband rates; those are Bluetooth hands-free links that add
            // 100+ ms themselves. Everything >= 22.05 kHz stays under 6 ms.
            CHECK (chain.getLatencySamples() * 1000.0 / sr < (sr >= 22050.0 ? 6.0 : 12.0));

            const int n = static_cast<int> (sr * 1.5);
            Planar buf (2, n);
            FastRandom rng (11);
            for (int i = 0; i < n; ++i)
            {
                const double t = i / sr;
                const float v = 0.8f * static_cast<float> (std::sin (kTwoPi * 110.0 * t) * (std::fmod (t, 0.25) < 0.05 ? 1.0 : 0.3))
                                + 0.2f * rng.nextBipolar();
                buf.ch[0][static_cast<size_t> (i)] = v;
                buf.ch[1][static_cast<size_t> (i)] = 0.9f * v;
            }
            runChain (chain, buf, 256);
            for (auto& c : buf.ch)
                for (float v : c)
                    REQUIRE (std::isfinite (v));
            CHECK_LE (peakAbs (buf.ch[0].data(), n), dbToGain (-1.0f) + 1e-6);
            CHECK (rms (buf.ch[0].data() + n / 2, n / 2) > 1e-3);
        }
}
