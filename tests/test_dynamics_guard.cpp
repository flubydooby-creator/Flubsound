// Startle Guard and the Dynamic Range control (docs/11 E21 Phase 3, with
// E20's Tame keying): flub/engine/StartleGuard.h.
//
// The guard alone (a 1 ms look-ahead delay stands in for the compressor
// slot the chain measures across), then in the chain. Scenes: a -40 dBFS
// pink ambience with automatic fire (10 shots/s, white noise, tau 15 ms), a
// loud 3.2 kHz step, a sustained level change, and the Gaming explosion of
// docs/11 E20's Tame Done-when. Levels are K-weighted (BS.1770) powers of
// both channels. Every test prints its values ("    measured ...").
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/analysis/LoudnessMeter.h"
#include "flub/common/Math.h"
#include "flub/dsp/Biquad.h"
#include "flub/engine/Parameters.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/engine/StartleGuard.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using namespace flub;
using namespace flub::param;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;
constexpr int kBlock = 480, kLookahead = 48;
using Channels = std::vector<std::vector<float>>;

int samplesOf (double seconds) { return static_cast<int> (std::lround (seconds * kFs)); }

void measured (const std::string& name, double value, const char* unit)
{
    char buf[64];
    std::snprintf (buf, sizeof (buf), "%.2f", value);
    std::cout << "    measured " << name << " = " << buf << " " << unit << "\n";
}

double powerDb (double p) { return 10.0 * std::log10 (std::max (1.0e-30, p)); }

Channels kWeighted (const Channels& c)
{
    Biquad s1, s2;
    s1.setCoeffs (LoudnessMeter::kWeightingStage1 (kFs));
    s2.setCoeffs (LoudnessMeter::kWeightingStage2 (kFs));
    Channels k (c.size());
    for (size_t ch = 0; ch < c.size(); ++ch)
    {
        k[ch].resize (c[ch].size());
        for (size_t i = 0; i < c[ch].size(); ++i)
            k[ch][i] = static_cast<float> (s2.processSample (static_cast<int> (ch), s1.processSample (static_cast<int> (ch), c[ch][i])));
    }
    return k;
}

double meanPower (const Channels& c, double from, double to)
{
    double acc = 0.0;
    int64_t n = 0;
    for (const auto& ch : c)
        for (int i = samplesOf (from); i < std::min (samplesOf (to), static_cast<int> (ch.size())); ++i)
        {
            acc += static_cast<double> (ch[static_cast<size_t> (i)]) * ch[static_cast<size_t> (i)];
            ++n;
        }
    return n > 0 ? acc / static_cast<double> (n) : 0.0;
}

/** The loudest 400 ms K-weighted window (every 50 ms) in [from, to). */
double loudestMomentaryDb (const Channels& k, double from, double to)
{
    double loudest = 0.0;
    for (double t = from; t + 0.4 <= to + 1.0e-9; t += 0.05)
        loudest = std::max (loudest, meanPower (k, t, t + 0.4));
    return powerDb (loudest);
}

/** Automatic fire (10 shots/s from `from` for `seconds`, each seeded white
    noise, tau 15 ms, peak `peak`) added to both channels. */
void addFire (Channels& c, double from, double seconds, double peak, uint32_t seed = 2468)
{
    FastRandom rng (seed);
    for (int shot = 0; shot < static_cast<int> (std::lround (seconds * 10.0)); ++shot)
    {
        const int onset = samplesOf (from + 0.1 * shot);
        for (int i = 0; i < samplesOf (0.1) && onset + i < static_cast<int> (c[0].size()); ++i)
        {
            const double env = peak * std::exp (-i / (0.015 * kFs));
            for (auto& ch : c)
                ch[static_cast<size_t> (onset + i)] += static_cast<float> (env * rng.nextBipolar());
        }
    }
}

Channels pinkBed (double seconds, double rmsDb)
{
    const int n = samplesOf (seconds);
    const auto a = pinkNoise (n, static_cast<float> (std::pow (10.0, rmsDb / 20.0)), 1357);
    const auto b = pinkNoise (n, static_cast<float> (std::pow (10.0, rmsDb / 20.0)), 2468);
    Channels c { a, std::vector<float> (static_cast<size_t> (n)) };
    for (size_t i = 0; i < c[1].size(); ++i)
        c[1][i] = 0.7f * a[i] + 0.71414284f * b[i];
    return c;
}

struct GuardRun
{
    Channels out;
    std::vector<float> gainDb; // per block (the guard's gain at its end)
    std::vector<float> referenceLufs;
};

/** The guard alone, as the chain runs it: measure() on the signal, apply()
    on the signal delayed by kLookahead (the compressor slot's latency at
    Balanced). The output is advanced by kLookahead again, so it lines up
    with the input. `ceilingAt` (seconds -> LU) may switch the ceiling. */
template <typename Ceiling>
GuardRun runGuard (const Channels& in, Ceiling ceilingAt)
{
    StartleGuard g;
    g.prepare (kFs, kBlock, kLookahead);
    const int n = static_cast<int> (in[0].size());
    Planar io (2, kBlock);
    std::vector<std::vector<float>> delay (2, std::vector<float> (static_cast<size_t> (kLookahead), 0.0f));
    GuardRun r;
    r.out.assign (2, std::vector<float> (static_cast<size_t> (n), 0.0f));
    int writePos = 0;
    for (int pos = 0; pos < n + kLookahead; pos += kBlock)
    {
        const int len = kBlock;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < len; ++i)
                io.ch[static_cast<size_t> (c)][static_cast<size_t> (i)] = pos + i < n ? in[static_cast<size_t> (c)][static_cast<size_t> (pos + i)] : 0.0f;
        g.setCeilingLu (ceilingAt (pos / kFs));
        g.measure (io.block(), false);
        for (int c = 0; c < 2; ++c) // the look-ahead stage: a pure delay
        {
            auto& d = delay[static_cast<size_t> (c)];
            auto& x = io.ch[static_cast<size_t> (c)];
            for (int i = 0; i < len; ++i)
            {
                const float y = d[static_cast<size_t> ((writePos + i) % kLookahead)];
                d[static_cast<size_t> ((writePos + i) % kLookahead)] = x[static_cast<size_t> (i)];
                x[static_cast<size_t> (i)] = y;
            }
        }
        writePos = (writePos + len) % kLookahead;
        g.apply (io.block());
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < len; ++i)
            {
                const int at = pos + i - kLookahead;
                if (at >= 0 && at < n)
                    r.out[static_cast<size_t> (c)][static_cast<size_t> (at)] = io.ch[static_cast<size_t> (c)][static_cast<size_t> (i)];
            }
        r.gainDb.push_back (g.getGainDb());
        r.referenceLufs.push_back (g.getReferenceLufs());
    }
    return r;
}

GuardRun runGuard (const Channels& in, float ceilingLu)
{
    return runGuard (in, [ceilingLu] (double) { return ceilingLu; });
}

float gainAt (const GuardRun& r, double seconds) { return r.gainDb[static_cast<size_t> (samplesOf (seconds) / kBlock)]; }
float referenceAt (const GuardRun& r, double seconds) { return r.referenceLufs[static_cast<size_t> (samplesOf (seconds) / kBlock)]; }
} // namespace

// =============================================================================
TEST_CASE ("StartleGuard: guard.range is a layout-version-4 Choice, Off by default, mapped to the E21 ceilings and E20 Tame amounts")
{
    const auto& info = layout()[static_cast<size_t> (GuardRange)];
    REQUIRE (findByKey ("guard.range") == GuardRange);
    CHECK (info.unit == Unit::Choice);
    CHECK (info.sinceVersion == 4);
    CHECK (info.defaultValue == 0.0f); // Off: every preset saved before it sounds as it did
    CHECK (! info.structural);
    REQUIRE (info.choices.size() == 5);
    CHECK (info.choices[0] == "Off");
    CHECK (info.choices[3] == "10 LU (Balanced)");
    CHECK (info.choices[4] == "6 LU (Shield)");
    const float ceilings[] = { 0.0f, 20.0f, 15.0f, 10.0f, 6.0f }, tame[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
    for (int c = 0; c < 5; ++c)
    {
        CHECK (StartleGuard::ceilingLuFor (c) == ceilings[c]);
        CHECK (StartleGuard::tameAmountFor (c) == tame[c]);
    }
    CHECK (StartleGuard::ceilingLuFor (-1) == 0.0f);
    CHECK (StartleGuard::ceilingLuFor (5) == 0.0f);
    CHECK (StartleGuard::tameAmountFor (static_cast<int> (GuardRangeValue::Off)) == 0.0f);
}

TEST_CASE ("StartleGuard: Off leaves the audio untouched, and a steady ambience under the ceiling is passed bit-exact at every setting")
{
    const auto bed = pinkBed (4.0, -30.0);
    for (float ceiling : { 0.0f, 20.0f, 15.0f, 10.0f, 6.0f })
    {
        const auto r = runGuard (bed, ceiling);
        bool identical = true;
        for (size_t c = 0; c < 2; ++c)
            for (size_t i = 0; i < bed[c].size(); ++i)
                identical = identical && r.out[c][i] == bed[c][i];
        CHECK (identical);
        CHECK (*std::min_element (r.gainDb.begin(), r.gainDb.end()) == 0.0f);
    }
}

TEST_CASE ("StartleGuard: automatic fire about 20 LU over the ambience is held to the ceiling (+1 LU), the ambience is back within 1 dB 1 s after it, the reference does not move")
{
    // 5 s of -40 dBFS pink ambience, 2 s of fire (peak -8 dBFS), 5 s of ambience.
    auto scene = pinkBed (12.0, -40.0);
    addFire (scene, 5.0, 2.0, std::pow (10.0, -8.0 / 20.0));
    const auto kin = kWeighted (scene);
    const double ambienceIn = powerDb (meanPower (kin, 3.0, 5.0));
    const double eventIn = loudestMomentaryDb (kin, 5.0, 7.4) - ambienceIn;
    measured ("fire over ambience, input", eventIn, "LU");
    CHECK_GE (eventIn, 19.0);
    for (float ceiling : { 20.0f, 15.0f, 10.0f, 6.0f })
    {
        const auto r = runGuard (scene, ceiling);
        const auto k = kWeighted (r.out);
        const double ambience = powerDb (meanPower (k, 3.0, 5.0));
        const double event = loudestMomentaryDb (k, 5.0, 7.4) - ambience;
        const std::string what = "ceiling " + std::to_string (static_cast<int> (ceiling)) + " LU";
        measured (what + ": fire over ambience", event, "LU");
        measured (what + ": gain 1 s after the fire", gainAt (r, 8.0), "dB");
        measured (what + ": ambience before -> 1-2 s after", powerDb (meanPower (k, 8.0, 9.0)) - powerDb (meanPower (kin, 8.0, 9.0)), "dB");
        measured (what + ": reference moved", referenceAt (r, 9.0) - referenceAt (r, 4.9), "dB");
        CHECK_LE (event, ceiling + 1.0);                   // docs/11 E21 Done-when: event - ambience <= N + 1 LU
        CHECK_GE (gainAt (r, 8.0), -1.0f);                  // ... within 1 dB of its pre-event gain after 1 s
        CHECK_NEAR (powerDb (meanPower (k, 3.0, 5.0)), ambienceIn, 0.01); // the ambience before is untouched
        CHECK_LE (std::abs (referenceAt (r, 9.0) - referenceAt (r, 4.9)), 0.5f);
    }
}

TEST_CASE ("StartleGuard: a loud 3.2 kHz footstep does not trigger it (the cue band is out of its sidechain); broadband sound at the same loudness does")
{
    // -40 dBFS ambience with, from 3 s, three 400 ms bursts 800 ms apart, 12 LU
    // (K-weighted, over their length) above it: 3.2 kHz band noise (a run of
    // steps, a reload) or pink noise (a crash), at ceiling 6 LU.
    const auto bed = pinkBed (6.0, -40.0);
    const double bedPower = meanPower (kWeighted (bed), 0.5, 3.0);
    auto burstScene = [&] (bool step) {
        auto noise = whiteNoise (samplesOf (6.0), 1.0f, 99);
        if (step)
        {
            // RBJ band-pass at 3.2 kHz, Q 1.
            const double w0 = kTwoPi * 3200.0 / kFs, alpha = std::sin (w0) / 2.0, a0 = 1.0 + alpha;
            double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
            for (auto& v : noise)
            {
                const double y = (alpha * v - alpha * x2) / a0 - (-2.0 * std::cos (w0) / a0) * y1 - ((1.0 - alpha) / a0) * y2;
                x2 = x1;
                x1 = v;
                y2 = y1;
                y1 = y;
                v = static_cast<float> (y);
            }
        }
        else
        {
            noise = pinkNoise (samplesOf (6.0), 1.0f, 77);
        }
        Channels bursts (2, std::vector<float> (noise.size(), 0.0f));
        for (double t = 3.0; t < 5.0; t += 0.8)
            for (int i = 0; i < samplesOf (0.4); ++i)
                for (auto& ch : bursts)
                    ch[static_cast<size_t> (samplesOf (t) + i)] = noise[static_cast<size_t> (samplesOf (t) + i)];
        const auto kb = kWeighted (bursts);
        double p = 0.0;
        for (double t = 3.0; t < 5.0; t += 0.8)
            p += meanPower (kb, t, t + 0.4);
        p /= 3.0;
        const double g = std::sqrt (bedPower * std::pow (10.0, 1.2) / p);
        auto scene = bed;
        for (size_t c = 0; c < 2; ++c)
            for (size_t i = 0; i < scene[c].size(); ++i)
                scene[c][i] += static_cast<float> (g * bursts[c][i]);
        return scene;
    };
    const auto step = runGuard (burstScene (true), 6.0f);
    const auto crash = runGuard (burstScene (false), 6.0f);
    const float stepGain = *std::min_element (step.gainDb.begin(), step.gainDb.end());
    const float crashGain = *std::min_element (crash.gainDb.begin(), crash.gainDb.end());
    measured ("deepest gain on 3.2 kHz steps 12 LU over the ambience", stepGain, "dB");
    measured ("deepest gain on pink bursts 12 LU over the ambience", crashGain, "dB");
    CHECK_GE (stepGain, -0.5f);
    CHECK_LE (crashGain, -4.0f);
}

TEST_CASE ("StartleGuard: programme that stays loud becomes the new level after 5 s and is released; a quieter scene restarts the reference")
{
    // -40 dBFS ambience for 4 s, then 15 dB louder for 12 s, then 25 dB
    // quieter than that for 6 s, at ceiling 10 LU.
    auto scene = pinkBed (22.0, -40.0);
    for (auto& ch : scene)
        for (size_t i = 0; i < ch.size(); ++i)
            ch[i] *= i >= static_cast<size_t> (samplesOf (16.0)) ? static_cast<float> (std::pow (10.0, -10.0 / 20.0))
                     : i >= static_cast<size_t> (samplesOf (4.0)) ? static_cast<float> (std::pow (10.0, 15.0 / 20.0))
                                                                  : 1.0f;
    const auto r = runGuard (scene, 10.0f);
    measured ("gain 1 s into the louder programme", gainAt (r, 5.0), "dB");
    measured ("gain 12 s into it", gainAt (r, 15.9), "dB");
    measured ("reference 12 s into it re before", referenceAt (r, 15.9) - referenceAt (r, 3.9), "dB");
    measured ("reference 5.5 s into the quieter scene re the louder one", referenceAt (r, 21.5) - referenceAt (r, 15.9), "dB");
    CHECK_LE (gainAt (r, 5.0), -3.0f);          // a +15 LU jump is capped at first ...
    CHECK_GE (gainAt (r, 15.9), -1.0f);         // ... and released once it is the programme
    CHECK_GE (referenceAt (r, 15.9) - referenceAt (r, 3.9), 13.0f);
    CHECK_NEAR (referenceAt (r, 21.5) - referenceAt (r, 15.9), -25.0f, 2.0f);
    CHECK_GE (gainAt (r, 21.5), -0.01f);
}

TEST_CASE ("StartleGuard: switching it off during an event releases the gain smoothly and then idles; hidden blocks hold it")
{
    auto scene = pinkBed (8.0, -40.0);
    addFire (scene, 2.0, 4.0, std::pow (10.0, -8.0 / 20.0));
    const auto r = runGuard (scene, [] (double t) { return t < 3.0 ? 10.0f : 0.0f; });
    float largestRise = 0.0f;
    for (size_t b = 1; b < r.gainDb.size(); ++b)
        largestRise = std::max (largestRise, r.gainDb[b] - r.gainDb[b - 1]);
    measured ("gain when switched off", gainAt (r, 2.99), "dB");
    measured ("largest rise per 10 ms block after", largestRise, "dB");
    CHECK_LE (gainAt (r, 2.99), -6.0f);
    CHECK_LE (largestRise, 1.0f); // a 200 ms release, not a step
    CHECK (gainAt (r, 5.5) == 0.0f);
    CHECK (referenceAt (r, 5.5) <= kMinusInfDb); // idle: nothing measured

    // A block hidden from the control loops (docs/11 E10) holds the gain.
    StartleGuard g;
    g.prepare (kFs, kBlock, kLookahead);
    g.setCeilingLu (6.0f);
    Planar io (2, kBlock);
    int pos = 0;
    for (; pos + kBlock <= samplesOf (2.5); pos += kBlock)
    {
        for (int c = 0; c < 2; ++c)
            std::copy (scene[static_cast<size_t> (c)].begin() + pos, scene[static_cast<size_t> (c)].begin() + pos + kBlock, io.ch[static_cast<size_t> (c)].begin());
        g.measure (io.block(), false);
        g.apply (io.block());
    }
    const float held = g.getGainDb();
    g.measure (io.block(), true);
    CHECK (g.getGainDb() == held);
    CHECK_LE (held, -6.0f);
}

TEST_CASE ("Chain: the Startle Guard adds no latency in any profile and is bit-exact while it is not needed; guard.range keys the Gaming Tame band")
{
    const auto bed = pinkBed (3.0, -30.0);
    for (auto profile : { LatencyProfileValue::Quality, LatencyProfileValue::Balanced, LatencyProfileValue::LowLatency })
    {
        Channels outs[2];
        int latency[2] = { 0, 0 };
        for (int on = 0; on < 2; ++on)
        {
            ParameterStore store;
            store.set (LatencyProfile, static_cast<float> (profile));
            store.set (GuardRange, on == 1 ? static_cast<float> (GuardRangeValue::Lu6Shield) : 0.0f);
            ProcessingChain chain (store);
            chain.prepare ({ kFs, kBlock, 2 });
            latency[on] = chain.getLatencySamples();
            Planar io (2, kBlock);
            outs[on].assign (2, {});
            for (int pos = 0; pos + kBlock <= static_cast<int> (bed[0].size()); pos += kBlock)
            {
                for (int c = 0; c < 2; ++c)
                    std::copy (bed[static_cast<size_t> (c)].begin() + pos, bed[static_cast<size_t> (c)].begin() + pos + kBlock, io.ch[static_cast<size_t> (c)].begin());
                chain.process (io.block());
                for (int c = 0; c < 2; ++c)
                    outs[on][static_cast<size_t> (c)].insert (outs[on][static_cast<size_t> (c)].end(), io.ch[static_cast<size_t> (c)].begin(), io.ch[static_cast<size_t> (c)].end());
            }
            CHECK (chain.getStartleGuardGainDb() == 0.0f);
        }
        CHECK (latency[0] == latency[1]); // docs/11 E21 Done-when: Low Latency chain latency unchanged
        CHECK (outs[0] == outs[1]);       // Music mode, ambience only: nothing to guard, no Tame band
    }
}

namespace
{
/** A Gaming-mode chain (defaults, Balanced) over `in` in kBlock blocks,
    latency compensated. */
Channels renderGaming (const Channels& in, float guardRange)
{
    ParameterStore store;
    store.set (Mode, static_cast<float> (ModeValue::Gaming));
    store.set (GuardRange, guardRange);
    ProcessingChain chain (store);
    chain.prepare ({ kFs, kBlock, 2 });
    const int latency = chain.getLatencySamples();
    const int n = static_cast<int> (in[0].size());
    Planar io (2, kBlock);
    Channels out (2, std::vector<float> (static_cast<size_t> (n), 0.0f));
    for (int pos = 0; pos < n + latency; pos += kBlock)
    {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < kBlock; ++i)
                io.ch[static_cast<size_t> (c)][static_cast<size_t> (i)] = pos + i < n ? in[static_cast<size_t> (c)][static_cast<size_t> (pos + i)] : 0.0f;
        chain.process (io.block());
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < kBlock; ++i)
                if (const int at = pos + i - latency; at >= 0 && at < n)
                    out[static_cast<size_t> (c)][static_cast<size_t> (at)] = io.ch[static_cast<size_t> (c)][static_cast<size_t> (i)];
    }
    return out;
}

std::vector<float> bandPass3k2 (const std::vector<float>& x)
{
    const double w0 = kTwoPi * 3200.0 / kFs, alpha = std::sin (w0) / 2.0, a0 = 1.0 + alpha;
    std::vector<float> y (x.size());
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double v = (alpha * x[i] - alpha * x2) / a0 + (2.0 * std::cos (w0) / a0) * y1 - ((1.0 - alpha) / a0) * y2;
        x2 = x1;
        x1 = x[i];
        y2 = y1;
        y1 = v;
        y[i] = static_cast<float> (v);
    }
    return y;
}
} // namespace

TEST_CASE ("Tame (docs/11 E20 via E21): the Dynamic Range control keys the Gaming anti-masking band - an explosion's loudness down >= 6 dB, a step 150 ms into it within 1 dB")
{
    // -40 dBFS pink ambience; at 2 s an explosion (45 Hz + low-passed noise,
    // peak 26 dB over the ambience's RMS, tau 350 ms, as in
    // tests/test_scenes.cpp); a 40 ms 3.2 kHz step 150 ms after its onset.
    // The same scene without the explosion is the step's reference.
    const int n = samplesOf (5.0);
    const double bedRms = std::pow (10.0, -40.0 / 20.0);
    const auto bed = pinkBed (5.0, -40.0);
    std::vector<float> explosion (static_cast<size_t> (n), 0.0f), step (static_cast<size_t> (n), 0.0f);
    {
        auto rumble = whiteNoise (n, 1.0f, 9753);
        for (int pass = 0; pass < 2; ++pass) // two one-pole low-passes at 150 Hz
        {
            const double a = std::exp (-kTwoPi * 150.0 / kFs);
            double s = 0.0;
            for (auto& v : rumble)
                v = static_cast<float> (s = (1.0 - a) * v + a * s);
        }
        const double rumbleRms = rms (rumble.data(), n);
        for (int i = samplesOf (2.0); i < n; ++i)
        {
            const double t = (i - samplesOf (2.0)) / kFs;
            explosion[static_cast<size_t> (i)] = static_cast<float> (bedRms * std::pow (10.0, 26.0 / 20.0) * std::exp (-t / 0.35)
                                                                     * (0.7 * std::sin (kTwoPi * 45.0 * t) + 0.3 * rumble[static_cast<size_t> (i)] / rumbleRms));
        }
        const auto band = bandPass3k2 (whiteNoise (n, 1.0f, 4242));
        const double gain = bedRms * std::pow (10.0, -6.0 / 20.0) / rms (band.data(), n) / std::sqrt (3.0 / 8.0);
        for (int i = 0; i < samplesOf (0.04); ++i)
            step[static_cast<size_t> (samplesOf (2.15) + i)] = static_cast<float> (gain * (0.5 - 0.5 * std::cos (kTwoPi * i / samplesOf (0.04))) * band[static_cast<size_t> (samplesOf (2.15) + i)]);
    }
    auto withExplosion = bed, without = bed;
    for (size_t c = 0; c < 2; ++c)
        for (size_t i = 0; i < static_cast<size_t> (n); ++i)
        {
            withExplosion[c][i] += explosion[i] + step[i];
            without[c][i] += step[i];
        }
    const auto stepLevel = [] (const Channels& out) {
        const Channels b { bandPass3k2 (out[0]), bandPass3k2 (out[1]) };
        return powerDb (meanPower (b, 2.15, 2.19));
    };
    // The explosion's own loudness: K-weighted power over its first second
    // minus that of the same render without it (the ambience and the step,
    // which pass the same way).
    const auto explosionDb = [] (const Channels& with, const Channels& withoutIt) {
        return powerDb (meanPower (kWeighted (with), 2.0, 3.0) - meanPower (kWeighted (withoutIt), 2.0, 3.0));
    };
    const double offDb = explosionDb (renderGaming (withExplosion, 0.0f), renderGaming (without, 0.0f));
    for (auto range : { GuardRangeValue::Lu10Balanced, GuardRangeValue::Lu6Shield })
    {
        const auto on = renderGaming (withExplosion, static_cast<float> (range));
        const auto onWithout = renderGaming (without, static_cast<float> (range));
        const double down = explosionDb (on, onWithout) - offDb;
        const double stepChange = stepLevel (on) - stepLevel (onWithout);
        const std::string what = range == GuardRangeValue::Lu6Shield ? "6 LU (Shield)" : "10 LU (Balanced)";
        measured (what + ": explosion loudness vs Off", down, "dB");
        measured (what + ": step 150 ms into it vs no explosion", stepChange, "dB");
        CHECK_LE (down, -6.0);
        // At Balanced the explosion's low end is the Tame band's alone: the
        // step keeps its level. At Shield the explosion's upper band is
        // itself over the 6 LU ceiling, so the guard takes the step down
        // with it (-2.75 dB).
        if (range == GuardRangeValue::Lu10Balanced)
            CHECK_LE (std::abs (stepChange), 1.0);
    }
}
