// The on-board enhancement cap (docs/11 E16): "Headset enhancement is ON"
// clamps Gaming Footsteps and Detail to 30 % and holds the virtualiser off
// (MacroMap::apply, ProcessingChain::setOnboardEnhancementCap), without
// touching the preset or the store, and glides in and out click-free.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/common/Denormals.h"
#include "flub/engine/MacroMap.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/io/PresetIO.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace flub;
using namespace flub::param;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;
constexpr int kBlock = 256;

/** A game-like stereo programme: a pink bed at about -30 dBFS, 40 ms
    footstep-like 3.2 kHz / 260 Hz bursts every 400 ms, quiet high ticks
    (Detail's upward compressor and air) and a loud noise burst at 1.5 s. */
Planar makeGameProgramme (int numSamples)
{
    Planar p (2, numSamples);
    const auto bedL = pinkNoise (numSamples, 0.03f, 11);
    const auto bedR = pinkNoise (numSamples, 0.03f, 12);
    FastRandom rng (5);
    for (int i = 0; i < numSamples; ++i)
    {
        const double t = i / kFs;
        const double inStep = std::fmod (t, 0.4);
        const double env = inStep < 0.04 ? std::sin (kPi * inStep / 0.04) : 0.0;
        const double step = env * (0.08 * std::sin (kTwoPi * 3200.0 * t) + 0.05 * std::sin (kTwoPi * 260.0 * t));
        const double tick = std::fmod (t + 0.2, 0.25) < 0.004 ? 0.01 * rng.nextBipolar() : 0.0;
        const double blast = t > 1.5 && t < 1.8 ? 0.5 * rng.nextBipolar() * std::exp (-(t - 1.5) * 8.0) : 0.0;
        const auto s = static_cast<size_t> (i);
        p.ch[0][s] = bedL[s] + static_cast<float> (step + tick + blast);
        p.ch[1][s] = bedR[s] + static_cast<float> (0.7 * step + tick + blast);
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

/** Level of a - b relative to b, dB (-inf as -300). */
double nullDb (const Planar& a, const Planar& b)
{
    double diff = 0.0, ref = 0.0;
    for (size_t c = 0; c < 2; ++c)
        for (size_t i = 0; i < a.ch[c].size(); ++i)
        {
            const double d = static_cast<double> (a.ch[c][i]) - static_cast<double> (b.ch[c][i]);
            diff += d * d;
            ref += static_cast<double> (b.ch[c][i]) * static_cast<double> (b.ch[c][i]);
        }
    return diff <= 0.0 ? -300.0 : 10.0 * std::log10 (diff / std::max (ref, 1.0e-30));
}

/** Renders `file` at the given Footsteps / Detail and virt.on, cap on or off
    (set before prepare(), as the app does for a device it already knows). */
Planar render (const char* file, float footsteps, float detail, bool virtOn, bool cap, const Planar& input)
{
    ParameterStore store;
    loadFactory (store, file);
    store.set (Macro1, footsteps);
    store.set (Macro4, detail);
    store.set (VirtualizerOn, virtOn ? 1.0f : 0.0f);
    ProcessingChain chain (store);
    chain.setOnboardEnhancementCap (cap);
    chain.prepare ({ kFs, kBlock, 2 });
    Planar buf = input;
    run (chain, buf);
    return buf;
}
} // namespace

TEST_CASE ("OnboardCap: MacroMap clamps Footsteps and Detail to 30 % and holds virt.on off; cap 0 is bit-identical; base untouched (E16)")
{
    ParameterStore store;
    store.set (Mode, static_cast<float> (ModeValue::Gaming));
    store.set (BoostIntensity, 0.6f);
    store.set (VirtualizerOn, 1.0f);
    for (float m : { 0.0f, 0.2f, 0.3f, 0.7f, 1.0f })
    {
        for (int id : { Macro1, Macro2, Macro3, Macro4, Macro5 })
            store.set (id, m);
        std::vector<float> base (static_cast<size_t> (kNumParams)), before (base.size()), plain (base.size()), off (base.size()), capped (base.size()),
            ref (base.size()), refBase;
        store.snapshot (base.data());
        before = base;
        MacroMap::apply (base.data(), plain.data(), 0.8f);
        MacroMap::apply (base.data(), off.data(), 0.8f, 0.0f);
        CHECK (plain == off); // cap 0: exactly the old result
        MacroMap::apply (base.data(), capped.data(), 0.8f, 1.0f);
        CHECK (base == before); // the preset / store values are never written
        // Capped == the same base with Footsteps and Detail at min(m, 0.30), virt.on off.
        refBase = base;
        refBase[static_cast<size_t> (Macro1)] = std::min (m, MacroMap::kOnboardCapMacroLimit);
        refBase[static_cast<size_t> (Macro4)] = std::min (m, MacroMap::kOnboardCapMacroLimit);
        refBase[static_cast<size_t> (VirtualizerOn)] = 0.0f;
        MacroMap::apply (refBase.data(), ref.data(), 0.8f);
        CHECK (capped == ref);
        CHECK (capped[static_cast<size_t> (Macro2)] == m); // Positional, Impact, Voice & Score keep theirs
        CHECK (capped[static_cast<size_t> (Macro3)] == m);
        CHECK (capped[static_cast<size_t> (Macro5)] == m);
        // Half way through the glide the limit is half way, 0.65.
        MacroMap::apply (base.data(), capped.data(), 0.8f, 0.5f);
        CHECK (std::abs (capped[static_cast<size_t> (Macro1)] - std::min (m, 0.65f)) < 1e-6f);
        CHECK (capped[static_cast<size_t> (VirtualizerOn)] == 0.0f);
    }
    // Music: the macros mean Punch .. Warmth and are not capped; the virtualiser still is.
    store.set (Mode, static_cast<float> (ModeValue::Music));
    std::vector<float> base (static_cast<size_t> (kNumParams)), eff (base.size()), plain (base.size());
    store.snapshot (base.data());
    MacroMap::apply (base.data(), plain.data(), 1.0f);
    MacroMap::apply (base.data(), eff.data(), 1.0f, 1.0f);
    CHECK (eff[static_cast<size_t> (Macro1)] == 1.0f);
    CHECK (eff[static_cast<size_t> (Macro4)] == 1.0f);
    CHECK (eff[static_cast<size_t> (VirtualizerOn)] == 0.0f);
    plain[static_cast<size_t> (VirtualizerOn)] = 0.0f;
    CHECK (eff == plain);
}

TEST_CASE ("OnboardCap: Competitive FPS at Footsteps / Detail 100 with the cap nulls against Footsteps / Detail 30 without it; the virtualiser is bypassed (E16 Done-when)")
{
    const int n = static_cast<int> (kFs * 2.5);
    const Planar input = makeGameProgramme (n);
    const Planar capped = render ("gaming-competitive-fps.json", 1.0f, 1.0f, true, true, input);
    const Planar reference = render ("gaming-competitive-fps.json", 0.30f, 0.30f, false, false, input);
    const Planar uncapped = render ("gaming-competitive-fps.json", 1.0f, 1.0f, false, false, input);
    const double capNull = nullDb (capped, reference);
    const double openNull = nullDb (uncapped, reference);
    std::cout << "    Footsteps / Detail 100 + cap (virt.on) vs 30: " << capNull << " dB; without the cap: " << openNull << " dB\n";
    CHECK (capNull <= -90.0);
    CHECK (openNull > -40.0); // the cap is what makes them equal: 100 and 30 differ audibly
}

TEST_CASE ("OnboardCap: the chain glides the cap in and out over 250 ms, reports it on the MeterBus and never writes the store (E16)")
{
    ParameterStore store;
    loadFactory (store, "gaming-competitive-fps.json");
    store.set (Macro1, 1.0f);
    store.set (Macro4, 0.8f);
    store.set (VirtualizerOn, 1.0f);
    ProcessingChain chain (store);
    chain.prepare ({ kFs, kBlock, 2 });
    Planar buf (2, kBlock);
    const auto block = [&] {
        chain.process (buf.block (0, kBlock));
    };
    block();
    CHECK (! chain.meters().onboardCapActive.load());
    CHECK (chain.effectiveValue (Macro1) == 1.0f);
    CHECK (chain.effectiveValue (VirtualizerOn) == 1.0f);

    chain.setOnboardEnhancementCap (true);
    CHECK (chain.getOnboardEnhancementCap());
    block();
    CHECK (chain.meters().onboardCapActive.load());
    CHECK (chain.effectiveValue (VirtualizerOn) == 0.0f);   // at once (the virtualiser's own 20 ms crossfade)
    const float firstStep = chain.effectiveValue (Macro1); // one block into the 250 ms glide
    CHECK (firstStep < 1.0f);
    CHECK (firstStep > 0.95f);
    const int glideBlocks = static_cast<int> (std::ceil (0.001 * ProcessingChain::kOnboardCapGlideMs * kFs / kBlock));
    for (int b = 1; b < glideBlocks; ++b)
        block();
    CHECK (chain.effectiveValue (Macro1) == MacroMap::kOnboardCapMacroLimit);
    CHECK (chain.effectiveValue (Macro4) == MacroMap::kOnboardCapMacroLimit);
    CHECK (store.get (Macro1) == 1.0f); // the store keeps the user's value
    CHECK (store.get (Macro4) == 0.8f);
    CHECK (store.get (VirtualizerOn) == 1.0f);

    chain.setOnboardEnhancementCap (false);
    block();
    CHECK (chain.meters().onboardCapActive.load()); // still gliding out
    CHECK (chain.effectiveValue (VirtualizerOn) == 0.0f);
    for (int b = 1; b < glideBlocks; ++b)
        block();
    CHECK (! chain.meters().onboardCapActive.load());
    CHECK (chain.effectiveValue (Macro1) == 1.0f);
    CHECK (chain.effectiveValue (Macro4) == 0.8f);
    CHECK (chain.effectiveValue (VirtualizerOn) == 1.0f);

    // A chain swapped in (MixEngine::configureFrom -> adoptGovernorState)
    // takes the cap over, at once on its first block.
    chain.setOnboardEnhancementCap (true);
    ProcessingChain next (store);
    next.prepare ({ kFs, kBlock, 2 });
    next.adoptGovernorState (chain);
    CHECK (next.getOnboardEnhancementCap());
    next.process (buf.block (0, kBlock));
    CHECK (next.effectiveValue (Macro1) == MacroMap::kOnboardCapMacroLimit);
    CHECK (next.meters().onboardCapActive.load());
}

TEST_CASE ("OnboardCap: switching the cap on and off while playing is click-free - stereo Footsteps / Detail 100 and a 7.1 strip with the virtualiser (E16)")
{
    // The criterion of "Chain: toggling the virtualiser on a 7.1 strip
    // crossfades" (test_engine.cpp): the largest sample-to-sample step
    // around each switch against steady state.
    struct Case
    {
        int channels;
        const char* file;
    };
    for (const Case& c : { Case { 2, "gaming-competitive-fps.json" }, Case { 8, "gaming-7-1-headphone-surround.json" } })
    {
        ParameterStore store;
        loadFactory (store, c.file);
        store.set (Macro1, 1.0f);
        store.set (Macro4, 1.0f);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, kBlock, c.channels });
        const int n = kBlock * 500; // 2.7 s
        Planar buf (c.channels, n);
        for (int ch = 0; ch < c.channels; ++ch)
            if (ch != 3)
            {
                const auto s = sine (220.0 * (1.0 + 0.1 * ch), kFs, n, 0.1f);
                std::copy (s.begin(), s.end(), buf.ch[static_cast<size_t> (ch)].begin());
            }
        const int toggleOn = kBlock * 150, toggleOff = kBlock * 330;
        {
            ScopedNoDenormals noDenormals;
            for (int pos = 0; pos < n; pos += kBlock)
            {
                if (pos == toggleOn)
                    chain.setOnboardEnhancementCap (true);
                if (pos == toggleOff)
                    chain.setOnboardEnhancementCap (false);
                chain.process (buf.block (pos, kBlock));
            }
        }
        auto maxStep = [&] (int from, int to) {
            double m = 0.0;
            for (size_t ch = 0; ch < 2; ++ch)
                for (int i = from + 1; i < to; ++i)
                    m = std::max (m, static_cast<double> (std::abs (buf.ch[ch][static_cast<size_t> (i)] - buf.ch[ch][static_cast<size_t> (i - 1)])));
            return m;
        };
        const double steadyOff = maxStep (kBlock * 60, toggleOn);
        const double steadyOn = maxStep (toggleOn + kBlock * 80, toggleOff);
        const double steady = std::max (steadyOn, steadyOff);
        const double atOn = maxStep (toggleOn - kBlock, toggleOn + kBlock * 60);
        const double atOff = maxStep (toggleOff - kBlock, toggleOff + kBlock * 60);
        std::cout << "    " << c.channels << " ch: max step at cap on " << atOn / steady << ", off " << atOff / steady << " x steady\n";
        CHECK (steady > 0.0);
        CHECK_LE (atOn, 1.25 * steady);
        CHECK_LE (atOff, 1.25 * steady);
    }
}
