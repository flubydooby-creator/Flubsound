// Engine-level tests: parameter layout/store, macros, protection loops,
// module slot, full processing chain, mixer and presets.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/analysis/PeakMeters.h"
#include "flub/common/Denormals.h"
#include "flub/engine/MixEngine.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/io/PresetIO.h"

#include <functional>
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

TEST_CASE ("Chain: Quality runs as Balanced below 32 kHz, without a re-prepare (docs/11 E42a)")
{
    // Before: Quality at 8 kHz was 1152 samples (144 ms; the 1024-sample gate
    // frame alone was 128 ms), 1192 (74.5 ms) at 16 kHz.
    for (const double sr : { 8000.0, 16000.0, 22050.0 })
    {
        ParameterStore store;
        ProcessingChain chain (store);
        store.set (LatencyProfile, static_cast<float> (LatencyProfileValue::Balanced));
        chain.prepare ({ sr, 512, 2 });
        const int balanced = chain.getLatencySamples();
        store.set (LatencyProfile, static_cast<float> (LatencyProfileValue::Quality));
        CHECK (chain.needsReprepare());
        chain.prepare ({ sr, 512, 2 });
        CHECK (chain.getLatencySamples() == balanced);
        CHECK (chain.getLatencyProfile() == LatencyProfileValue::Balanced);
        CHECK (chain.getRequestedLatencyProfile() == LatencyProfileValue::Quality);
        CHECK (! chain.needsReprepare()); // the clamp is not a pending change
    }
    // From 32 kHz up Quality is Quality.
    ParameterStore store;
    ProcessingChain chain (store);
    store.set (LatencyProfile, static_cast<float> (LatencyProfileValue::Balanced));
    chain.prepare ({ 32000.0, 512, 2 });
    const int balanced = chain.getLatencySamples();
    store.set (LatencyProfile, static_cast<float> (LatencyProfileValue::Quality));
    chain.prepare ({ 32000.0, 512, 2 });
    CHECK (chain.getLatencyProfile() == LatencyProfileValue::Quality);
    CHECK (chain.getRequestedLatencyProfile() == LatencyProfileValue::Quality);
    CHECK (chain.getLatencySamples() > balanced);
}

TEST_CASE ("Chain: each profile's latency in ms is about the same from 44.1 to 192 kHz (docs/11 E42a)")
{
    // Look-aheads and the gate's STFT frame are defined in ms; only the
    // oversampler FIRs and the 20-sample true-peak detector are in samples,
    // and those get shorter in ms as the rate rises. Before, the gate frame
    // was 1024 samples at every rate: Quality was 30.2 ms at 44.1 kHz but
    // 16.6 ms at 96 kHz and 10.8 ms at 192 kHz.
    const auto latencyMs = [] (LatencyProfileValue p, double sr) {
        ParameterStore store;
        store.set (LatencyProfile, static_cast<float> (p));
        ProcessingChain chain (store);
        chain.prepare ({ sr, 512, 2 });
        return 1000.0 * chain.getLatencySamples() / sr;
    };
    CHECK (latencyMs (LatencyProfileValue::Quality, 48000.0) == 1000.0 * 1400 / 48000.0); // 48 kHz: 1400 + Clarity's 1 ms look-ahead (docs/11 E04 step 5)
    for (const double sr : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
    {
        const double q = latencyMs (LatencyProfileValue::Quality, sr);
        const double b = latencyMs (LatencyProfileValue::Balanced, sr);
        const double l = latencyMs (LatencyProfileValue::LowLatency, sr);
        std::cerr << "    " << sr << " Hz: Quality " << q << " ms, Balanced " << b << " ms, Low Latency " << l << " ms\n";
        CHECK (q >= 27.0); // with Clarity's 1 ms look-ahead (docs/11 E04 step 5)
        CHECK (q <= 31.5);
        // Look-aheads: Balanced 2.5 ms, Low Latency 1 ms, plus FIRs that are at
        // most their 48 kHz length in ms.
        CHECK (b >= 2.5);
        CHECK (b <= 4.2);
        CHECK (l >= 1.0);
        CHECK (l <= 2.2);
    }
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

TEST_CASE ("Chain: matched bypass never overshoots the ceiling when a louder dry peak arrives")
{
    // The processed output (Loudness macro) is far louder than the input, so
    // the matched bypass lifts the dry reference. Kicks that keep getting
    // louder then meet a match gain chosen for the quieter past: the
    // reference must still stay at the ceiling (sample and true peak).
    for (int profile = 0; profile < 3; ++profile)
    {
        ParameterStore store;
        store.set (LatencyProfile, static_cast<float> (profile));
        store.set (Macro4, 1.0f);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, 512, 2 });
        const int n = static_cast<int> (kFs * 10.0);
        Planar buf (2, n);
        FastRandom rng (5);
        float lp = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const double t = i / kFs;
            lp += 0.05f * (rng.nextBipolar() - lp);                        // dull noise bed
            const double beat = std::fmod (t, 0.5);
            const double kickLevel = std::pow (10.0, (-26.0 + 2.6 * t) / 20.0); // -26 dBFS rising to 0 dBFS
            const double kick = kickLevel * std::exp (-beat * 12.0) * std::sin (kTwoPi * 55.0 * beat);
            const double burst = std::fmod (t, 1.7) < 0.02 ? 0.5 * kickLevel * rng.nextBipolar() : 0.0;
            for (auto& c : buf.ch)
                c[static_cast<size_t> (i)] = static_cast<float> (0.08 * lp + kick + burst);
        }
        const int bypassFrom = static_cast<int> (kFs * 4.0);
        for (int pos = 0; pos < n; pos += 512)
        {
            if (pos == bypassFrom)
                store.set (BypassAll, 1.0f);
            chain.process (buf.block (pos, std::min (512, n - pos)));
        }
        const int from = bypassFrom + static_cast<int> (kFs * 0.1);
        TruePeakMeter tp;
        tp.prepare (2);
        tp.process (buf.block (from, n - from));
        CHECK_LE (tp.getMaxDbAllChannels(), -1.0 + 0.15);
        for (auto& c : buf.ch)
            CHECK_LE (peakAbs (c.data() + from, n - from), dbToGain (-1.0f) + 1e-6);
    }
}

namespace
{
/** Runs `buf` through `chain` in blocks; `afterBlock (endSample)` is called
    after every block (to sample meters or change parameters). */
void runChainWatching (ProcessingChain& chain, Planar& buf, int blockSize, const std::function<void (int)>& afterBlock)
{
    ScopedNoDenormals noDenormals;
    const int n = buf.numSamples();
    for (int pos = 0; pos < n; pos += blockSize)
    {
        const int len = std::min (blockSize, n - pos);
        chain.process (buf.block (pos, len));
        afterBlock (pos + len);
    }
}

/** Largest |a[i] - b[i - delay]| over both channels for i in [from, to). */
double maxDelayedError (const Planar& a, const Planar& b, int delay, int from, int to)
{
    double m = 0.0;
    for (size_t c = 0; c < 2; ++c)
        for (int i = std::max (from, delay); i < to; ++i)
            m = std::max (m, static_cast<double> (std::abs (a.ch[c][static_cast<size_t> (i)] - b.ch[c][static_cast<size_t> (i - delay)])));
    return m;
}

/** Maximizer-only strip (LowLatency profile keeps the long AutoDrive runs cheap). */
void maximizerOnly (ParameterStore& s, float driveDb, bool autoDrive, float targetLufs)
{
    bypassAllModules (s);
    s.set (LatencyProfile, static_cast<float> (LatencyProfileValue::LowLatency));
    s.set (MaximizerOn, 1.0f);
    s.set (MaxDriveDb, driveDb);
    s.set (MaxAutoDrive, autoDrive ? 1.0f : 0.0f);
    s.set (MaxTargetLufs, targetLufs);
}
} // namespace

TEST_CASE ("Chain: AutoDrive pulls a hot drive down to the loudness target at <= 2 dB/s and settles near, not above, it")
{
    // +16 dB of maximizer drive on a programme at about -26 LUFS puts the
    // output near -11 LUFS, 7 LU over a -18 LUFS target. AutoDrive
    // (Protection.h) integrates the error of a gated 3 s loudness of the
    // output at min(2, 0.5 |error|) dB/s with a 0.5 LU dead band, as a drive
    // REDUCTION in [-24, 0] dB (drive = max(0, requested + reduction)).
    constexpr float kDrive = 16.0f, kTarget = -18.0f;
    constexpr int kBlockSize = 512;
    const double blockSec = kBlockSize / kFs;
    {
        ParameterStore store; // reference: AutoDrive off stays far over the target
        maximizerOnly (store, kDrive, false, kTarget);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, kBlockSize, 2 });
        auto buf = makeProgramme (48000 * 4, 0.15f);
        runChain (chain, buf, kBlockSize);
        CHECK_GE (chain.meters().shortTermLufs.load(), kTarget + 5.0f);
        CHECK (chain.meters().autoDriveDb.load() == 0.0f);
    }

    ParameterStore store;
    maximizerOnly (store, kDrive, true, kTarget);
    ProcessingChain chain (store);
    chain.prepare ({ kFs, kBlockSize, 2 });
    // 30 s: behind the 3 s follower the loop is underdamped - it first
    // undershoots the target by about 3 LU (around 10 s), then settles
    // inside its dead band by about 20 s.
    auto buf = makeProgramme (48000 * 30, 0.15f);
    std::vector<float> reduction, shortTerm; // per block
    runChainWatching (chain, buf, kBlockSize, [&] (int) {
        reduction.push_back (chain.meters().autoDriveDb.load());
        shortTerm.push_back (chain.meters().shortTermLufs.load());
    });

    // Never raises the drive, never beyond its range, never faster than 2 dB/s.
    float prev = 0.0f;
    double fastest = 0.0;
    for (float r : reduction)
    {
        CHECK (r <= 0.0f);
        CHECK (r >= -24.0f);
        fastest = std::max (fastest, std::abs (r - prev) / blockSec);
        prev = r;
    }
    CHECK_LE (fastest, 2.0 + 1e-3);
    // Far over the target (error > 4 LU) the loop runs at exactly that limit
    // (the gated loudness reads the output level from its first open block,
    // so from the start until the error falls under 4 LU at about 4 s)...
    const auto reductionAt = [&] (double seconds) { return reduction[static_cast<size_t> (seconds / blockSec)]; };
    CHECK_NEAR (reductionAt (4.0) - reductionAt (1.0), -6.0, 0.3);
    // ...and it settles inside its dead band, with drive to spare (the target
    // is reached by the loop, not by the 0 dB drive floor).
    CHECK_GE (shortTerm.back(), kTarget - 1.0f);
    CHECK_LE (shortTerm.back(), kTarget + 0.75f);
    CHECK_GE (kDrive + reduction.back(), 3.0f);
    // Once the output has come down to the target it never climbs back over it
    // (beyond the dead band and the meter's 3 s window).
    size_t reached = 0;
    while (reached < shortTerm.size() && ! (shortTerm[reached] > -100.0f && shortTerm[reached] <= kTarget + 0.75f))
        ++reached;
    REQUIRE (reached < shortTerm.size());
    CHECK_LE (static_cast<double> (reached) * blockSec, 10.0);
    for (size_t i = reached; i < shortTerm.size(); ++i)
        CHECK_LE (shortTerm[i], kTarget + 0.75f);
}

TEST_CASE ("Chain: AutoDrive's reduction stops at the requested drive, so it recovers at once")
{
    // An unreachable target (far below the undriven loudness): once the drive
    // is back at 0 dB further "reduction" changes nothing, so it must stop at
    // -requested drive instead of running on to -24 dB and delaying recovery.
    ParameterStore store;
    bypassAllModules (store);
    store.set (MaximizerOn, 1.0f);
    store.set (MaxDriveDb, 6.0f);
    store.set (MaxAutoDrive, 1.0f);
    store.set (MaxTargetLufs, -24.0f);
    ProcessingChain chain (store);
    chain.prepare ({ kFs, 512, 2 });
    auto buf = makeProgramme (static_cast<int> (kFs * 12.0), 0.6f);
    runChain (chain, buf, 512);
    const float reduction = chain.meters().autoDriveDb.load();
    CHECK_NEAR (reduction, -6.0, 1e-3);
    // Lowering the requested drive clamps the reduction immediately.
    store.set (MaxDriveDb, 2.0f);
    auto more = makeProgramme (512 * 4, 0.6f);
    runChain (chain, more, 512);
    CHECK_GE (chain.meters().autoDriveDb.load(), -2.0f - 1e-3f);
}

TEST_CASE ("Chain: AutoDrive never raises the drive: below the target it is inert, and it stops at 0 dB drive (the input itself)")
{
    constexpr int kBlockSize = 512;
    // 1. A quiet programme far below the target: the reduction stays exactly
    //    0 and the output is bit-identical to AutoDrive off (it can only take
    //    drive away, never add any).
    Planar out[2] = { makeProgramme (48000 * 4, 0.05f), makeProgramme (48000 * 4, 0.05f) };
    for (int autoDrive = 0; autoDrive < 2; ++autoDrive)
    {
        ParameterStore store;
        maximizerOnly (store, 6.0f, autoDrive == 1, -14.0f);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, kBlockSize, 2 });
        float deepest = 0.0f, highest = -100.0f;
        runChainWatching (chain, out[autoDrive], kBlockSize, [&] (int) {
            deepest = std::min (deepest, chain.meters().autoDriveDb.load());
            highest = std::max (highest, chain.meters().autoDriveDb.load());
        });
        CHECK (deepest == 0.0f);
        CHECK (highest == 0.0f);
        CHECK (chain.meters().shortTermLufs.load() < -24.0f); // far below the -14 LUFS target
    }
    CHECK (maxDelayedError (out[1], out[0], 0, 0, out[0].numSamples()) == 0.0);

    // 2. A target the programme exceeds even undriven (-20 LUFS vs -24): the
    //    reduction reaches the requested 6 dB and stops there, so the applied
    //    drive is 0 dB and the output is the untouched (delayed) input -
    //    AutoDrive never attenuates below the unprocessed level.
    ParameterStore store;
    maximizerOnly (store, 6.0f, true, -24.0f);
    ProcessingChain chain (store);
    chain.prepare ({ kFs, kBlockSize, 2 });
    const auto in = makeProgramme (48000 * 8, 0.3f);
    Planar buf = in;
    float deepest = 0.0f;
    runChainWatching (chain, buf, kBlockSize, [&] (int) { deepest = std::min (deepest, chain.meters().autoDriveDb.load()); });
    CHECK_LE (chain.meters().autoDriveDb.load(), -6.0f);
    CHECK_GE (deepest, -24.0f);
    CHECK_LE (maxDelayedError (buf, in, chain.getLatencySamples(), 48000 * 6, buf.numSamples()), 1e-6);
}

TEST_CASE ("Chain: matched bypass plays the input as it is and turns the processed side down to it within 0.5 LU; unmatched bypass is the input itself")
{
    // +10 dB maximizer drive on a steady, low-crest programme (peaks -25 dBFS):
    // the processed output is 10 LU louder than the input and is never
    // limited. docs/11 E37: the match only ever turns the louder side down,
    // so in a comparison the reference is the input itself and the processed
    // side, once the bypass is off again, plays 10 dB down (it used to raise
    // the reference by 10 dB instead, and fell short where that needed
    // headroom; see test_protection_gaps.cpp for hot programme).
    constexpr int kBlockSize = 512;
    constexpr int kBypassAt = kBlockSize * 470;   // ~5.0 s: measures settled
    constexpr int kBypassOffAt = kBlockSize * 800; // ~8.5 s: 3.5 s of bypass (> the 3 s short-term window)
    constexpr int kTotal = kBlockSize * 1130;      // ~3.5 s of the processed side again, still in the comparison
    Planar in (2, kTotal);
    for (int i = 0; i < kTotal; ++i)
    {
        const double t = i / kFs;
        const double env = 0.8 + 0.2 * std::sin (kTwoPi * 0.7 * t);
        const double v = env * (0.5 * std::sin (kTwoPi * 110.0 * t) + 0.3 * std::sin (kTwoPi * 440.0 * t + 1.0) + 0.2 * std::sin (kTwoPi * 1760.0 * t + 2.0));
        in.ch[0][static_cast<size_t> (i)] = static_cast<float> (0.06 * v);
        in.ch[1][static_cast<size_t> (i)] = static_cast<float> (0.05 * v);
    }

    for (bool matched : { true, false })
    {
        ParameterStore store;
        store.set (MaxDriveDb, 10.0f);
        store.set (LoudnessMatchBypass, matched ? 1.0f : 0.0f);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, kBlockSize, 2 });
        Planar buf = in;
        float wetLufs = 0.0f, inLufs = 0.0f, refLufs = 0.0f, inAtOff = 0.0f;
        runChainWatching (chain, buf, kBlockSize, [&] (int end) {
            if (end == kBypassAt)
            {
                wetLufs = chain.meters().shortTermLufs.load();
                inLufs = chain.meters().inShortTermLufs.load();
                store.set (BypassAll, 1.0f);
            }
            if (end == kBypassOffAt)
            {
                refLufs = chain.meters().shortTermLufs.load();
                inAtOff = chain.meters().inShortTermLufs.load();
                store.set (BypassAll, 0.0f);
            }
        });
        const float backLufs = chain.meters().shortTermLufs.load(), inAtEnd = chain.meters().inShortTermLufs.load();
        CHECK_GE (wetLufs - inLufs, 8.0f); // processing is clearly louder than the input

        // The reference is the input, sample for sample (delayed by the
        // chain latency), matched or not: the input is the quieter side.
        const int lat = chain.getLatencySamples();
        const int from = kBypassOffAt - 48000 * 2; // the last 2 s of bypass, well after the 30 ms crossfade
        CHECK_LE (maxDelayedError (buf, in, lat, from, kBypassOffAt), 1e-6);
        CHECK_NEAR (refLufs, inAtOff, 0.5);
        // Back on the processed side within the comparison: matched, it is
        // 10 LU down, at the input's loudness; unmatched, 10 LU over it.
        CHECK_NEAR (backLufs - inAtEnd, matched ? 0.0 : 10.0, 0.5);
    }
}

TEST_CASE ("MixEngine: strips are summed, padded to equal latency and master-limited")
{
    // One sync group: the two strips are padded to the slower one.
    const std::vector<StripConfig> layout { { "Game", 8, 0.0f, false, 0 }, { "Music", 2, 0.0f, false, 0 } };
    MixEngine mix;
    mix.configure (layout, kFs, 256);
    mix.params (0).set (Mode, 1.0f);
    mix.params (1).set (LatencyProfile, 0.0f); // Quality profile on music - needs reprepare
    CHECK (mix.needsReprepare());
    mix.configure (layout, kFs, 256);
    CHECK (! mix.needsReprepare());
    CHECK (mix.params (1).get (LatencyProfile) == 0.0f); // store preserved across configure
    CHECK (mix.getStripPaddingSamples (1) == 0);
    CHECK (mix.getStripPaddingSamples (0) == mix.chain (1).getLatencySamples() - mix.chain (0).getLatencySamples());
    CHECK (mix.getStripLatencySamples (0) == mix.getLatencySamples());
    CHECK (mix.getStripLatencySamples (1) == mix.getLatencySamples());

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

TEST_CASE ("MixEngine: master look-ahead follows the strips' latency profiles (0.5 ms only when all are Low Latency)")
{
    // One sync group, so every strip is padded to the slowest one.
    const std::vector<StripConfig> layout { { "Game", 8, 0.0f, false, 0 }, { "Music", 2, 0.0f, false, 0 }, { "Chat", 2, 0.0f, false, 0 } };
    const auto setAll = [] (MixEngine& m, LatencyProfileValue p) {
        for (int s = 0; s < m.getNumStrips(); ++s)
            m.params (s).set (LatencyProfile, static_cast<float> (p));
    };

    // Reported latency = where an impulse actually comes out (every module
    // bypassed, so the strips and the master limiter are pure delays).
    const auto impulsePosition = [] (MixEngine& m) {
        const int n = 1024;
        Planar game (8, n), music (2, n), chat (2, n), out (2, n);
        game.ch[0][0] = game.ch[1][0] = 0.25f;
        const AudioBlock gb = game.block(), mb = music.block(), cb = chat.block();
        const AudioBlock* inputs[] = { &gb, &mb, &cb };
        m.process (inputs, out.block());
        const auto it = std::max_element (out.ch[0].begin(), out.ch[0].end(), [] (float a, float b) { return std::abs (a) < std::abs (b); });
        return static_cast<int> (it - out.ch[0].begin());
    };

    MixEngine mix;
    mix.configure (layout, kFs, 1024);
    for (int s = 0; s < mix.getNumStrips(); ++s)
        bypassAllModules (mix.params (s));

    // Default (Balanced everywhere): 192 + master 1 ms (48) + detector 20.
    setAll (mix, LatencyProfileValue::Balanced);
    mix.configure (layout, kFs, 1024);
    CHECK (mix.chain (0).getLatencySamples() == 192);
    CHECK (mix.getLatencySamples() == 192 + 48 + 20);
    CHECK (impulsePosition (mix) == mix.getLatencySamples());

    // All strips on Low Latency (what the app does): 100 + 0.5 ms (24) + 20
    // = 144 samples = 3.0 ms instead of 168.
    setAll (mix, LatencyProfileValue::LowLatency);
    CHECK (mix.needsReprepare());
    mix.configure (layout, kFs, 1024);
    CHECK (! mix.needsReprepare());
    CHECK (mix.chain (0).getLatencySamples() == 100);
    CHECK (mix.getLatencySamples() == 100 + 24 + 20);
    CHECK (impulsePosition (mix) == mix.getLatencySamples());

    // One strip back on Balanced: that is a structural change the host sees
    // through needsReprepare(), and the re-configure pads every strip to 192
    // and puts the master back on 1 ms.
    mix.params (2).set (LatencyProfile, static_cast<float> (LatencyProfileValue::Balanced));
    CHECK (mix.needsReprepare());
    mix.configure (layout, kFs, 1024);
    CHECK (mix.getLatencySamples() == 192 + 48 + 20);
    CHECK (impulsePosition (mix) == mix.getLatencySamples());

    // Quality anywhere: 1400 + 68.
    mix.params (1).set (LatencyProfile, static_cast<float> (LatencyProfileValue::Quality));
    CHECK (mix.needsReprepare());
    mix.configure (layout, kFs, 1024);
    CHECK (mix.getLatencySamples() == 1400 + 48 + 20);
}

TEST_CASE ("MixEngine: with all strips on Low Latency the 0.5 ms master still holds the ceiling")
{
    MixEngine mix;
    const std::vector<StripConfig> layout { { "Game", 2, 0.0f, false }, { "Music", 2, 0.0f, false } };
    mix.configure (layout, kFs, 256);
    for (int s = 0; s < 2; ++s)
    {
        mix.params (s).set (LatencyProfile, static_cast<float> (LatencyProfileValue::LowLatency));
        mix.params (s).set (BoostIntensity, 1.0f);
    }
    mix.configure (layout, kFs, 256);
    REQUIRE (mix.getLatencySamples() == 100 + 24 + 20);

    // Two hot, individually limited strips: their sum overshoots the -1 dBTP
    // master ceiling by several dB, so the master works continuously.
    const int total = 48000 * 3;
    const auto a = makeProgramme (total, 0.9f, 7);
    const auto b = makeProgramme (total, 0.9f, 99);
    Planar game (2, 256), music (2, 256), out (2, total);
    const AudioBlock gb = game.block(), mb = music.block();
    const AudioBlock* inputs[] = { &gb, &mb };
    float deepestGr = 0.0f;
    AllocationGuard guard;
    for (int pos = 0; pos + 256 <= total; pos += 256)
    {
        for (int c = 0; c < 2; ++c)
        {
            std::copy_n (a.ch[static_cast<size_t> (c)].begin() + pos, 256, game.ch[static_cast<size_t> (c)].begin());
            std::copy_n (b.ch[static_cast<size_t> (c)].begin() + pos, 256, music.ch[static_cast<size_t> (c)].begin());
        }
        mix.process (inputs, out.block (pos, 256));
        deepestGr = std::min (deepestGr, mix.getMasterGainReductionDb());
    }
    CHECK (guard.allocations() == 0);

    CHECK_LE (deepestGr, -2.0); // the master really was limiting
    TruePeakMeter tp;
    tp.prepare (2);
    tp.process (out.block());
    CHECK_LE (tp.getMaxDbAllChannels(), -1.0 + 0.15);
    for (int c = 0; c < 2; ++c)
        CHECK_LE (peakAbs (out.ch[static_cast<size_t> (c)].data(), total), dbToGain (-1.0f) + 1e-6);
    CHECK (mix.getMasterSafetyClipCount() == 0);
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

namespace
{
/** "Chain: runs at every sample rate a headset may use": one rate, both modes. */
void checkChainAtSampleRate (double sr)
{
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
        for (const auto& c : buf.ch) // both output channels: ceiling held, audio present
        {
            CHECK_LE (peakAbs (c.data(), n), dbToGain (-1.0f) + 1e-6);
            CHECK (rms (c.data() + n / 2, n / 2) > 1e-3);
        }
    }
}

/** One case per rate ("Chain: runs at every sample rate a headset may use
    (8 kHz hands-free .. 192 kHz) - <rate> Hz"), so each stays under 2 s.
    Bluetooth hands-free (HFP) endpoints run at 8 / 16 kHz, some USB headsets
    at 22.05 / 24 / 32 kHz; wired and USB audio classes at 44.1 .. 192 kHz. */
bool registerChainSampleRateCases()
{
    for (double sr : { 8000.0, 16000.0, 22050.0, 24000.0, 32000.0, 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
        ::flubtest::Registrar ((std::string ("Chain: runs at every sample rate a headset may use (8 kHz hands-free .. 192 kHz) - ")
                                + std::to_string (static_cast<int> (sr)) + " Hz")
                                   .c_str(),
                               [sr] { checkChainAtSampleRate (sr); }, __FILE__, __LINE__);
    return true;
}

[[maybe_unused]] const bool kChainSampleRateCasesRegistered = registerChainSampleRateCases();
} // namespace

TEST_CASE ("MixEngine: padding only within sync groups, per-strip latency reported (docs/11 E40 part 3 / E42a)")
{
    // Reported per-strip latency = where an impulse on that strip comes out
    // (every module bypassed: chains, pads and the master are pure delays).
    const auto impulsePosition = [] (MixEngine& m, int strip) {
        const int n = 2048;
        Planar a (8, n), b (2, n), c (2, n), out (2, n);
        Planar* ins[] = { &a, &b, &c };
        ins[strip]->ch[0][0] = ins[strip]->ch[1][0] = 0.25f;
        const AudioBlock ab = a.block(), bb = b.block(), cb = c.block();
        const AudioBlock* inputs[] = { &ab, &bb, &cb };
        m.process (inputs, out.block());
        const auto it = std::max_element (out.ch[0].begin(), out.ch[0].end(), [] (float x, float y) { return std::abs (x) < std::abs (y); });
        return static_cast<int> (it - out.ch[0].begin());
    };
    const auto build = [] (MixEngine& m, const std::vector<StripConfig>& layout) {
        m.configure (layout, kFs, 2048);
        for (int s = 0; s < m.getNumStrips(); ++s)
            bypassAllModules (m.params (s));
        m.params (0).set (LatencyProfile, static_cast<float> (LatencyProfileValue::LowLatency)); // Game
        m.params (1).set (LatencyProfile, static_cast<float> (LatencyProfileValue::Quality));    // Music
        m.params (2).set (LatencyProfile, static_cast<float> (LatencyProfileValue::Balanced));   // Chat
        m.configure (layout, kFs, 2048);
    };

    // Default: every strip in a group of its own. A Quality preset on Music
    // no longer delays the Low Latency Game strip: 100 + master 1 ms (48 + 20)
    // instead of 1400 + 68 (before this change every strip was padded).
    {
        MixEngine mix;
        build (mix, { { "Game", 8, 0.0f, false }, { "Music", 2, 0.0f, false }, { "Chat", 2, 0.0f, false } });
        CHECK (mix.getMasterLatencySamples() == 48 + 20);
        CHECK (mix.getStripPaddingSamples (0) == 0);
        CHECK (mix.getStripPaddingSamples (1) == 0);
        CHECK (mix.getStripPaddingSamples (2) == 0);
        CHECK (mix.getStripLatencySamples (0) == 100 + 68);
        CHECK (mix.getStripLatencySamples (1) == 1400 + 68);
        CHECK (mix.getStripLatencySamples (2) == 192 + 68);
        CHECK (mix.getLatencySamples() == 1400 + 68); // the slowest strip
        for (int s = 0; s < 3; ++s)
            CHECK (impulsePosition (mix, s) == mix.getStripLatencySamples (s));
    }
    // Game and Chat share a group (e.g. a game and its voice channel): Game is
    // padded to Chat's 192, Music stays alone.
    {
        MixEngine mix;
        build (mix, { { "Game", 8, 0.0f, false, 1 }, { "Music", 2, 0.0f, false }, { "Chat", 2, 0.0f, false, 1 } });
        CHECK (mix.getStripPaddingSamples (0) == 92);
        CHECK (mix.getStripPaddingSamples (1) == 0);
        CHECK (mix.getStripPaddingSamples (2) == 0);
        CHECK (mix.getStripLatencySamples (0) == 192 + 68);
        CHECK (mix.getStripLatencySamples (2) == 192 + 68);
        CHECK (mix.getStripLatencySamples (1) == 1400 + 68);
        for (int s = 0; s < 3; ++s)
            CHECK (impulsePosition (mix, s) == mix.getStripLatencySamples (s));
    }
    // Out-of-range strips report nothing.
    MixEngine empty;
    CHECK (empty.getStripLatencySamples (0) == 0);
    CHECK (empty.getStripPaddingSamples (-1) == 0);
}

TEST_CASE ("MixEngine: configureFrom builds a second engine that shares the running engine's parameter stores")
{
    // What a host's crossfaded engine swap relies on: the new engine is built
    // beside the old one, which keeps processing exactly as before.
    const std::vector<StripConfig> oldLayout { { "Game", 8, 0.0f, false }, { "Music", 2, 0.0f, false } };
    MixEngine running, reference;
    running.configure (oldLayout, kFs, 256);
    reference.configure (oldLayout, kFs, 256);
    running.params (1).set (BoostIntensity, 0.7f);
    reference.params (1).set (BoostIntensity, 0.7f);

    Planar game (8, 256), music (2, 256), out (2, 256), refGame (8, 256), refMusic (2, 256), refOut (2, 256);
    const AudioBlock gb = game.block(), mb = music.block(), rgb = refGame.block(), rmb = refMusic.block();
    const AudioBlock* inputs[] = { &gb, &mb };
    const AudioBlock* refInputs[] = { &rgb, &rmb };
    int64_t t = 0;
    float maxDiff = 0.0f;
    const auto run = [&] (int blocks)
    {
        for (int b = 0; b < blocks; ++b, t += 256)
        {
            for (int i = 0; i < 256; ++i)
            {
                const float v = 0.3f * static_cast<float> (std::sin (kTwoPi * 220.0 * static_cast<double> (t + i) / kFs));
                for (auto* p : { &game, &refGame })
                    for (auto& c : p->ch)
                        c[static_cast<size_t> (i)] = v;
                for (auto* p : { &music, &refMusic })
                    for (auto& c : p->ch)
                        c[static_cast<size_t> (i)] = 0.5f * v;
            }
            running.process (inputs, out.block());
            reference.process (refInputs, refOut.block());
            for (size_t c = 0; c < 2; ++c)
                for (size_t i = 0; i < 256; ++i)
                    maxDiff = std::max (maxDiff, std::abs (out.ch[c][i] - refOut.ch[c][i]));
        }
    };
    run (20);

    // A structural change (Quality on Music) and a third strip: a new engine.
    running.params (1).set (LatencyProfile, 0.0f);
    reference.params (1).set (LatencyProfile, 0.0f);
    CHECK (running.needsReprepare());
    auto next = std::make_unique<MixEngine>();
    next->configureFrom (running, { { "Game", 8, 0.0f, false }, { "Music", 2, 0.0f, false }, { "Chat", 2, 0.0f, false } }, kFs, 256);
    REQUIRE (next->getNumStrips() == 3);
    CHECK (&next->params (0) == &running.params (0)); // the same stores: profiles carry over
    CHECK (&next->params (1) == &running.params (1));
    CHECK (next->params (1).get (BoostIntensity) == 0.7f);
    CHECK (next->params (2).get (BoostIntensity) == ParameterStore().get (BoostIntensity)); // a new strip: defaults
    CHECK (! next->needsReprepare());
    CHECK (running.needsReprepare()); // the running engine was not re-prepared ...
    CHECK (next->getLatencySamples() > running.getLatencySamples());

    // ... and keeps processing sample for sample as if nothing happened.
    run (20);
    CHECK (maxDiff == 0.0f);

    // A parameter written through either engine reaches both; the stores
    // outlive the engine they were created in.
    next->params (1).set (BoostIntensity, 0.2f);
    CHECK (running.params (1).get (BoostIntensity) == 0.2f);
    MixEngine third;
    third.configureFrom (*next, { { "Game", 8, 0.0f, false } }, kFs, 256);
    next.reset();
    CHECK (third.params (0).get (Mode) == running.params (0).get (Mode));
    third.params (0).set (Mode, 1.0f);
    CHECK (running.params (0).get (Mode) == 1.0f);
}

// ---------------------------------------------------------------------------
// 5.1 / 7.1 input folds (docs/11 E01 LFE, E27 active channels, E24 (i)).
namespace
{
/** An 8-channel stream fed block by block through a chain; channel levels
    can change between calls. The chain's fold state is sampled per block. */
struct SurroundFeed
{
    explicit SurroundFeed (ProcessingChain& c, int block = 512) : chain (c), blockSize (block) {}

    ProcessingChain& chain;
    int blockSize;
    int64_t t = 0;
    std::vector<int> foldPerBlock; // MeterBus::inputFold after each block

    /** `seconds` of sines (a different frequency per channel) at the given
        peak amplitudes, FL/FR as noise when `noiseFronts`. Returns the output. */
    Planar run (const std::array<float, 8>& amps, double seconds, bool noiseFronts = true)
    {
        const int n = static_cast<int> (std::lround (seconds * kFs / blockSize)) * blockSize;
        Planar buf (8, n);
        for (int c = 0; c < 8; ++c)
        {
            const auto amp = amps[static_cast<size_t> (c)];
            if (amp == 0.0f)
                continue;
            auto& ch = buf.ch[static_cast<size_t> (c)];
            if (c < 2 && noiseFronts)
            {
                const auto x = whiteNoise (n, amp, static_cast<uint32_t> (77 + c + t));
                std::copy (x.begin(), x.end(), ch.begin());
            }
            else
            {
                for (int i = 0; i < n; ++i)
                    ch[static_cast<size_t> (i)] = amp * static_cast<float> (std::sin (kTwoPi * (310.0 + 90.0 * c) * static_cast<double> (t + i) / kFs));
            }
        }
        const Planar in = buf;
        std::vector<int> folds (static_cast<size_t> (n / blockSize));
        {
            AllocationGuard guard;
            for (int pos = 0, b = 0; pos < n; pos += blockSize, ++b)
            {
                chain.process (buf.block (pos, blockSize));
                folds[static_cast<size_t> (b)] = chain.meters().inputFold.load();
            }
            allocations += guard.allocations();
        }
        foldPerBlock.insert (foldPerBlock.end(), folds.begin(), folds.end());
        t += n;
        inputs.push_back (in);
        return buf;
    }

    /** Seconds from `fromBlock` until the fold first read `fold`; -1 if never. */
    double timeTo (int fold, size_t fromBlock) const
    {
        for (size_t b = fromBlock; b < foldPerBlock.size(); ++b)
            if (foldPerBlock[b] == fold)
                return static_cast<double> (b + 1 - fromBlock) * blockSize / kFs;
        return -1.0;
    }

    std::vector<Planar> inputs;
    int64_t allocations = 0; // inside chain.process()
};

std::array<float, 8> frontsOnly (float amp) { return { amp, amp, 0, 0, 0, 0, 0, 0 }; }
} // namespace

TEST_CASE ("Chain: a stereo game in an 8-channel container switches to the passthrough fold within 2.5 s without a level step, then equals the 2-channel render (E27)")
{
    for (bool virt : { false, true })
    {
        ParameterStore s8, s2;
        for (auto* s : { &s8, &s2 })
        {
            bypassAllModules (*s);
            s->set (VirtualizerOn, virt ? 1.0f : 0.0f);
        }
        ProcessingChain c8 (s8), c2 (s2);
        c8.prepare ({ kFs, 512, 8 });
        c2.prepare ({ kFs, 512, 2 });
        SurroundFeed feed (c8);
        const Planar out8 = feed.run (frontsOnly (0.1f), 4.0);
        CHECK (feed.allocations == 0);
        const Planar& in8 = feed.inputs.front();
        Planar ref (2, in8.numSamples());
        std::copy (in8.ch[0].begin(), in8.ch[0].end(), ref.ch[0].begin());
        std::copy (in8.ch[1].begin(), in8.ch[1].end(), ref.ch[1].begin());
        runChain (c2, ref, 512);

        // Decision after 2 s of FL/FR-only content, then a 400 ms ramp.
        const double decided = feed.timeTo (1, 0);
        CHECK_GE (decided, 2.0);
        CHECK_LE (decided + 0.4, 2.5);
        CHECK (c8.meters().activeChannelMask.load() == 0x3u);
        CHECK (! c8.meters().surroundConfirmed.load());

        // Then exactly the 2-channel render (all modules bypassed: both are
        // the input delayed by the same latency).
        const int settled = static_cast<int> (2.5 * kFs);
        double err = 0.0;
        for (int c = 0; c < 2; ++c)
            for (int i = settled; i < out8.numSamples(); ++i)
                err = std::max (err, static_cast<double> (std::abs (out8.ch[static_cast<size_t> (c)][static_cast<size_t> (i)] - ref.ch[static_cast<size_t> (c)][static_cast<size_t> (i)])));
        CHECK_LE (err, 1e-6);

        // The ramp: output / 2-channel reference level in 20 ms windows moves
        // by well under 1 dB between neighbours across the switch (1.9..3 s)
        // and never dips below where it started (no hole mid-fade: the
        // linear fade is RMS-compensated, next test). The BS.775 fold starts
        // 3.01 dB below, the virtualiser about 0.8 dB, whose short-term level
        // also wanders with the noise's spectrum in the steady fold (0.5..1.9 s).
        // (The virtualiser started 0.8 dB below until its level match, docs/11
        // E28a, put it at the downmix's loudness: now 2.9 dB.)
        const int w = static_cast<int> (0.02 * kFs);
        double lowest = 0.0;
        auto track = [&] (double from, double to, double& first, double& last) {
            double prev = 0.0, maxStep = 0.0;
            lowest = 0.0;
            for (int i = static_cast<int> (from * kFs), k = 0; i + w <= static_cast<int> (to * kFs); i += w, ++k)
            {
                const double num = rms (out8.ch[0].data() + i, w) + rms (out8.ch[1].data() + i, w);
                const double den = rms (ref.ch[0].data() + i, w) + rms (ref.ch[1].data() + i, w);
                const double g = toDb (num / den);
                lowest = std::min (lowest, g);
                if (k == 0)
                    first = g;
                else
                    maxStep = std::max (maxStep, std::abs (g - prev));
                prev = g;
            }
            last = prev;
            return maxStep;
        };
        double first = 0.0, last = 0.0, unused = 0.0;
        const double steadyStep = track (0.5, 1.9, unused, unused);
        const double switchStep = track (1.9, 3.0, first, last);
        std::cout << "    measured " << (virt ? "virtualiser" : "BS.775") << " -> passthrough: start " << first << " dB, largest 20 ms step "
                  << switchStep << " dB (steady fold: " << steadyStep << " dB)\n";
        CHECK_NEAR (first, virt ? -2.9 : -3.01, virt ? 0.5 : 0.01);
        CHECK_NEAR (last, 0.0, 0.01);
        CHECK_LE (switchStep, virt ? 0.5 : 0.2);
        CHECK_GE (lowest, first - (virt ? 0.3 : 0.01));
    }
}

TEST_CASE ("Chain: the passthrough ramp is power-compensated - centred pink noise glides from the virtualiser to passthrough without a swell (E27)")
{
    // FL = FR pink noise (a centred, low-heavy source) in an 8-channel
    // container, virtualiser on, every module off. The binaural render and
    // the passthrough are strongly correlated in the lows, so an equal-power
    // ramp between them swelled 1.2-1.5 dB above both folds mid-fade; the
    // compensated linear ramp stays between the two folds' levels. Level:
    // output / input power (both channels) in 50 ms windows, latency aligned.
    ParameterStore store;
    bypassAllModules (store);
    store.set (VirtualizerOn, 1.0f);
    ProcessingChain chain (store);
    chain.prepare ({ kFs, 512, 8 });
    const int n = static_cast<int> (4.0 * kFs / 512) * 512;
    Planar buf (8, n);
    const auto pink = pinkNoise (n, 0.05f, 5);
    buf.ch[0] = pink;
    buf.ch[1] = pink;
    runChain (chain, buf, 512);
    const int latency = chain.getLatencySamples(), w = static_cast<int> (0.05 * kFs);
    auto level = [&] (int from) {
        double out = 0.0, in = 0.0;
        for (int i = from; i < from + w; ++i)
        {
            const auto x = static_cast<double> (pink[static_cast<size_t> (i)]);
            in += 2.0 * x * x;
            for (int c = 0; c < 2; ++c)
            {
                const auto y = static_cast<double> (buf.ch[static_cast<size_t> (c)][static_cast<size_t> (i + latency)]);
                out += y * y;
            }
        }
        return 10.0 * std::log10 (out / in);
    };
    // The virtualiser fold was up to 2.6 dB above the passthrough; since its
    // level match (docs/11 E28a) it sits about 3 dB below it (the downmix's
    // loudness), so the ramp must stay between the two folds either way.
    double virtMax = -100.0, virtMin = 100.0, passMin = 100.0, passMax = -100.0, fadeMax = -100.0, fadeMin = 100.0;
    for (int i = static_cast<int> (1.0 * kFs); i + w <= static_cast<int> (1.9 * kFs); i += w / 4)
    {
        virtMax = std::max (virtMax, level (i)); // the virtualiser fold, before the decision
        virtMin = std::min (virtMin, level (i));
    }
    for (int i = static_cast<int> (1.9 * kFs); i + w <= static_cast<int> (2.6 * kFs); i += w / 4)
    {
        fadeMax = std::max (fadeMax, level (i));
        fadeMin = std::min (fadeMin, level (i));
    }
    for (int i = static_cast<int> (2.6 * kFs); i + w + latency <= n; i += w / 4)
    {
        passMin = std::min (passMin, level (i)); // passthrough: 0 dB
        passMax = std::max (passMax, level (i));
    }
    std::cout << "    measured centred pink: virtualiser fold up to " << virtMax << " dB, during the ramp " << fadeMin << " .. " << fadeMax
              << " dB, passthrough from " << passMin << " dB\n";
    CHECK_NEAR (passMin, 0.0, 0.01);
    CHECK_LE (fadeMax, std::max (virtMax, passMax) + 0.3);
    CHECK_GE (fadeMin, std::min (virtMin, passMin) - 0.3);
}

TEST_CASE ("Chain: rear content switches back to surround within 300 ms and latches through 30 s of rear silence; -45 dB ambience never leaves surround; redetect starts over (E27)")
{
    ParameterStore store;
    bypassAllModules (store);
    store.set (VirtualizerOn, 1.0f);
    ProcessingChain chain (store);
    chain.prepare ({ kFs, 1024, 8 });
    SurroundFeed feed (chain, 1024);
    feed.run (frontsOnly (0.1f), 2.5);
    CHECK (feed.foldPerBlock.back() == 1);

    // A side-left sound 30 dB under the fronts.
    auto rear = frontsOnly (0.1f);
    rear[6] = 0.1f * dbToGain (-30.0f);
    const size_t mark = feed.foldPerBlock.size();
    feed.run (rear, 0.5);
    const double back = feed.timeTo (0, mark);
    CHECK (back > 0.0);
    CHECK_LE (back, 0.30);
    CHECK (chain.meters().surroundConfirmed.load());
    CHECK (chain.meters().activeChannelMask.load() == ((1u << 6) | 0x3u));

    // 30 s with silent rears: stays surround (the latch).
    const size_t quiet = feed.foldPerBlock.size();
    feed.run (frontsOnly (0.1f), 30.0);
    CHECK (feed.timeTo (1, quiet) < 0.0);

    // A new routed process: detection starts over.
    chain.redetectInputChannels();
    const size_t again = feed.foldPerBlock.size();
    feed.run (frontsOnly (0.1f), 2.5);
    CHECK (feed.timeTo (1, again) > 0.0);
    CHECK (feed.allocations == 0);

    // -45 dB rear ambience under the fronts is surround content.
    chain.prepare ({ kFs, 1024, 8 });
    SurroundFeed amb (chain, 1024);
    auto ambience = frontsOnly (0.1f);
    ambience[4] = ambience[5] = 0.1f * dbToGain (-45.0f);
    amb.run (ambience, 5.0);
    CHECK (amb.timeTo (1, 0) < 0.0);
    CHECK (chain.meters().surroundConfirmed.load());
}

TEST_CASE ("Chain: Force Stereo / Force Surround and the own-HRTF switch (E27); focus 0 under the binaural lock (E24 (i))")
{
    auto full = frontsOnly (0.1f);
    full[2] = full[4] = full[5] = full[6] = full[7] = 0.05f;
    // Force Stereo: the passthrough fold from the first block, whatever the
    // content, and no binaural lock on the (virtualiser-on) store's width.
    {
        ParameterStore store;
        store.set (VirtInputMode, static_cast<float> (InputModeValue::ForceStereo));
        store.set (SpatialWidth, 1.5f);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, 512, 8 });
        SurroundFeed feed (chain);
        feed.run (full, 0.5);
        CHECK (feed.timeTo (0, 0) < 0.0);
        CHECK (chain.effectiveValue (SpatialWidth) == 1.5f);
    }
    // Force Surround: FL/FR-only content never leaves the surround fold, and
    // the binaural lock holds width 1 and focus 0 (Positional 100 %).
    {
        ParameterStore store;
        store.set (Mode, 1.0f);
        store.set (VirtInputMode, static_cast<float> (InputModeValue::ForceSurround));
        store.set (SpatialWidth, 1.5f);
        store.set (Macro2, 1.0f);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, 512, 8 });
        SurroundFeed feed (chain);
        feed.run (frontsOnly (0.1f), 3.0);
        CHECK (feed.timeTo (1, 0) < 0.0);
        CHECK (chain.effectiveValue (SpatialWidth) == 1.0f);
        CHECK (chain.effectiveValue (SpatialFocus) == 0.0f);
        // Auto on the same content: after the switch there is no binaural
        // render, so no lock: width and focus apply again.
        store.set (VirtInputMode, static_cast<float> (InputModeValue::Auto));
        feed.run (frontsOnly (0.1f), 2.5);
        CHECK (feed.foldPerBlock.back() == 1);
        CHECK (chain.effectiveValue (SpatialWidth) > 1.0f);
        CHECK (chain.effectiveValue (SpatialFocus) > 0.8f);
    }
    // The game renders its own HRTF: virtualiser off, width 1, focus 0,
    // crossfeed 0, space 0 on any strip; a 7.1 strip folds as stereo at once
    // unless surround is forced.
    for (int channels : { 2, 8 })
        for (bool forceSurround : { false, true })
        {
            ParameterStore store;
            store.set (VirtOwnHrtf, 1.0f);
            store.set (SpatialWidth, 1.8f);
            store.set (SpatialFocus, 0.7f);
            store.set (SpatialSpace, 0.5f);
            store.set (SpatialCrossfeed, 0.5f); // Music mode: crossfeed is otherwise allowed
            if (forceSurround)
                store.set (VirtInputMode, static_cast<float> (InputModeValue::ForceSurround));
            ProcessingChain chain (store);
            chain.prepare ({ kFs, 512, channels });
            Planar buf (channels, 512 * 20);
            runChain (chain, buf, 512);
            CHECK (chain.effectiveValue (VirtualizerOn) == 0.0f);
            CHECK (chain.effectiveValue (SpatialWidth) == 1.0f);
            CHECK (chain.effectiveValue (SpatialFocus) == 0.0f);
            CHECK (chain.effectiveValue (SpatialSpace) == 0.0f);
            CHECK (chain.effectiveValue (SpatialCrossfeed) == 0.0f);
            CHECK (chain.meters().inputFold.load() == (channels > 2 && ! forceSurround ? 1 : 0));
        }
}

TEST_CASE ("Chain: the LFE folds at virt.lfe re one main in both folds; virt.lfeFold off is the v1 downmix bit for bit (E01)")
{
    // LFE-only vs FL-only 50 Hz, all modules bypassed, surround fold forced.
    auto level = [] (int channel, bool virt, float lfeDb) {
        ParameterStore store;
        bypassAllModules (store);
        store.set (VirtualizerOn, virt ? 1.0f : 0.0f);
        store.set (VirtRoom, 0.0f);
        store.set (VirtLfeGainDb, lfeDb);
        store.set (VirtInputMode, static_cast<float> (InputModeValue::ForceSurround));
        ProcessingChain chain (store);
        chain.prepare ({ kFs, 512, 8 });
        const int n = 48000;
        Planar buf (8, n);
        const auto s = sine (50.0, kFs, n, 0.25f);
        std::copy (s.begin(), s.end(), buf.ch[static_cast<size_t> (channel)].begin());
        runChain (chain, buf, 512);
        return toDb (toneAmplitude (buf.ch[0].data() + n / 2, n / 2, 50.0, kFs));
    };
    CHECK (layout()[static_cast<size_t> (VirtLfeGainDb)].defaultValue == 10.0f); // +6 before preset schema 3
    for (float lfeDb : { 0.0f, 6.0f, 10.0f })
    {
        const double off = level (3, false, lfeDb) - level (0, false, lfeDb);
        const double on = level (3, true, lfeDb) - level (0, true, lfeDb);
        CHECK_NEAR (off, lfeDb, 0.05);
        // The virtualiser sends FL to both ears at the downmix's loudness
        // (level match, docs/11 E28a): 3 dB lower at each ear; the LFE is not
        // scaled, so it is virt.lfe above one main in loudness in both folds.
        CHECK_NEAR (on, lfeDb + 3.01, 0.1);
    }

    // LFE fold off: the v1 BS.775 downmix (LFE dropped, -3 dB), delayed by the
    // chain latency, bit for bit.
    ParameterStore store;
    bypassAllModules (store);
    store.set (VirtLfeFold, 0.0f);
    store.set (VirtInputMode, static_cast<float> (InputModeValue::ForceSurround));
    ProcessingChain chain (store);
    chain.prepare ({ kFs, 256, 8 });
    const int lat = chain.getLatencySamples();
    const int n = 24000;
    Planar buf (8, n);
    for (int c = 0; c < 8; ++c)
    {
        const auto x = whiteNoise (n, 0.1f, static_cast<uint32_t> (500 + c));
        std::copy (x.begin(), x.end(), buf.ch[static_cast<size_t> (c)].begin());
    }
    std::vector<float> refL (static_cast<size_t> (n)), refR (static_cast<size_t> (n));
    constexpr float k = 0.70710678f;
    for (size_t i = 0; i < static_cast<size_t> (n); ++i)
    {
        float lo = buf.ch[0][i], ro = buf.ch[1][i];
        lo += k * buf.ch[2][i];
        ro += k * buf.ch[2][i];
        for (size_t c = 4; c + 1 < 8; c += 2)
        {
            lo += k * buf.ch[c][i];
            ro += k * buf.ch[c + 1][i];
        }
        refL[i] = k * lo;
        refR[i] = k * ro;
    }
    runChain (chain, buf, 256);
    double err = 0.0;
    for (int i = lat; i < n; ++i)
    {
        err = std::max (err, static_cast<double> (std::abs (buf.ch[0][static_cast<size_t> (i)] - refL[static_cast<size_t> (i - lat)])));
        err = std::max (err, static_cast<double> (std::abs (buf.ch[1][static_cast<size_t> (i)] - refR[static_cast<size_t> (i - lat)])));
    }
    CHECK (err == 0.0);
}

TEST_CASE ("Presets: schema versions 2 / 3 - a preset without virt.lfe keeps its version's default (version 1: 0 dB, 2: +6 dB), a version-3 one gets today's (+10 dB)")
{
    auto parse = [] (const char* text, preset::Preset& p, std::string& err) {
        json::Value v;
        REQUIRE (json::parse (text, v, err));
        return preset::fromJson (v, p, err);
    };
    preset::Preset p;
    std::string err;
    REQUIRE (parse (R"({ "format": "flubsound-preset", "version": 1, "name": "Old", "params": { "boost": 0.5 } })", p, err));
    CHECK (p.values[static_cast<size_t> (VirtLfeGainDb)] == 0.0f);
    REQUIRE (parse (R"({ "format": "flubsound-preset", "name": "No version", "params": {} })", p, err));
    CHECK (p.values[static_cast<size_t> (VirtLfeGainDb)] == 0.0f);
    REQUIRE (parse (R"({ "format": "flubsound-preset", "version": 1, "name": "Set", "params": { "virt.lfe": -4 } })", p, err));
    CHECK (p.values[static_cast<size_t> (VirtLfeGainDb)] == -4.0f);
    REQUIRE (parse (R"({ "format": "flubsound-preset", "version": 2, "name": "New", "params": {} })", p, err));
    CHECK (p.values[static_cast<size_t> (VirtLfeGainDb)] == 6.0f);
    CHECK (p.values[static_cast<size_t> (VirtLfeFold)] == 1.0f);
    REQUIRE (parse (R"({ "format": "flubsound-preset", "version": 3, "name": "Newer", "params": {} })", p, err));
    CHECK (p.values[static_cast<size_t> (VirtLfeGainDb)] == 10.0f);
    CHECK (! parse (R"({ "format": "flubsound-preset", "version": 4, "name": "Future", "params": {} })", p, err));

    // Saving writes version 3, so a value equal to today's default may be
    // omitted and still round-trips; a version-1 value of 0 dB is written.
    preset::Preset old;
    REQUIRE (parse (R"({ "format": "flubsound-preset", "version": 1, "name": "Old", "params": {} })", old, err));
    const auto j = preset::toJson (old);
    CHECK (j["version"].asNumber() == 3.0);
    CHECK (j["params"]["virt.lfe"].asNumber (99.0) == 0.0);
    preset::Preset back;
    REQUIRE (preset::fromJson (j, back, err));
    CHECK (back.values[static_cast<size_t> (VirtLfeGainDb)] == 0.0f);
    const auto fresh = preset::toJson (preset::makeDefault());
    CHECK (fresh["params"]["virt.lfe"].isNull());
    REQUIRE (preset::fromJson (fresh, back, err));
    CHECK (back.values[static_cast<size_t> (VirtLfeGainDb)] == 10.0f);
}
