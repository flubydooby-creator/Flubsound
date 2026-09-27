// Direct tests for protection, A/B and macro sub-parts that were previously
// only covered indirectly (see docs/TRACEABILITY.md R2.1, R2.8, R2.10, R3.5,
// R6.1):
//   * the SafetyGovernor's clip-energy branch, on its own and end to end
//     through the chain with governed Boost drive into heavy clipping;
//   * the desktop master limiter at the device-profile ceiling caps
//     (Bluetooth A2DP -2 dBTP, hands-free -3 dBTP) on hot inter-sample-peak
//     material, and the air exciter's sample-rate cut-off (< 42 kHz);
//   * the ComparisonMatcher (loudness-matched bypass) as a unit, and its
//     accuracy through the chain on hot programme (docs/11 E37);
//   * click-free A/B bank switches and global bypass toggles;
//   * the Music Width and Clarity macros (MacroMap values and their audible
//     direction through the chain);
//   * GatedLoudness's relative-gate release and cold-start correction (through
//     AutoLevel and AutoDrive), the control loops across a dropped NaN block,
//     the meters of slots switched off mid-stream, and the governor's GR input
//     across block sizes.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/analysis/LoudnessMeter.h"
#include "flub/analysis/PeakMeters.h"
#include "flub/common/Denormals.h"
#include "flub/engine/DeviceProfiles.h"
#include "flub/engine/MixEngine.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/engine/Protection.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace flub;
using namespace flub::param;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;

std::vector<float> defaultBase()
{
    std::vector<float> base (static_cast<size_t> (kNumParams));
    for (int i = 0; i < kNumParams; ++i)
        base[static_cast<size_t> (i)] = layout()[static_cast<size_t> (i)].defaultValue;
    return base;
}

void bypassAllModules (ParameterStore& s)
{
    for (int id : { GateOn, EqOn, DynEqOn, BassOn, ClarityOn, SaturationOn, SpatialOn, VirtualizerOn, CompressorOn, MaximizerOn })
        s.set (id, 0.0f);
}

void runChain (ProcessingChain& chain, Planar& buf, int blockSize)
{
    ScopedNoDenormals noDenormals;
    const int n = buf.numSamples();
    for (int pos = 0; pos < n; pos += blockSize)
        chain.process (buf.block (pos, std::min (blockSize, n - pos)));
}

/** Drum-like programme (kicks, noise hats, bass line, pad), as in test_engine. */
Planar makeProgramme (int numSamples, float level, uint32_t seed, double sampleRate = kFs)
{
    Planar p (2, numSamples);
    FastRandom rng (seed);
    for (int i = 0; i < numSamples; ++i)
    {
        const double t = i / sampleRate;
        const double beat = std::fmod (t, 0.5);
        const double kick = std::exp (-beat * 18.0) * std::sin (kTwoPi * (50.0 + 80.0 * std::exp (-beat * 30.0)) * beat);
        const double hat = (std::fmod (t + 0.25, 0.5) < 0.03 ? 0.3 : 0.0) * rng.nextBipolar();
        const double bassLine = 0.4 * std::sin (kTwoPi * 55.0 * t);
        const double pad = 0.15 * std::sin (kTwoPi * 440.0 * t) + 0.1 * std::sin (kTwoPi * 660.0 * t + 0.3);
        p.ch[0][static_cast<size_t> (i)] = level * static_cast<float> (kick + hat + bassLine + pad);
        p.ch[1][static_cast<size_t> (i)] = level * static_cast<float> (kick + 0.8 * hat + bassLine + 0.7 * pad);
    }
    return p;
}

/** Largest sample-to-sample step of x over [from, to). */
double maxStep (const std::vector<float>& x, int from, int to)
{
    double m = 0.0;
    for (int i = std::max (1, from); i < to; ++i)
        m = std::max (m, static_cast<double> (std::abs (x[static_cast<size_t> (i)] - x[static_cast<size_t> (i - 1)])));
    return m;
}

//==============================================================================
// Independent 4x true-peak meter (shares no code with TruePeakDetector): a
// 64-taps-per-phase Kaiser (beta 8) windowed sinc evaluated in double at the
// three fractional positions n + 1/4, 1/2, 3/4 plus the sample itself, the
// BS.1770-4 Annex 2 method with a longer interpolator. Samples outside the
// signal are zero.
double independentTruePeakDb (const std::vector<float>& x, int from, int to)
{
    constexpr int kHalf = 32;
    constexpr double kBeta = 8.0;
    const auto besselI0 = [] (double v) {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 50; ++k)
        {
            term *= (v / (2.0 * k)) * (v / (2.0 * k));
            sum += term;
        }
        return sum;
    };
    double taps[3][2 * kHalf];
    for (int p = 1; p <= 3; ++p)
        for (int k = 0; k < 2 * kHalf; ++k)
        {
            // Tap for x[n + 1 - kHalf + k] at position n + p/4.
            const double d = p / 4.0 - (1 - kHalf + k);
            const double sinc = std::abs (d) < 1e-12 ? 1.0 : std::sin (kPi * d) / (kPi * d);
            const double r = d / (kHalf + 0.5);
            const double w = std::abs (r) < 1.0 ? besselI0 (kBeta * std::sqrt (1.0 - r * r)) / besselI0 (kBeta) : 0.0;
            taps[p - 1][k] = sinc * w;
        }
    const int n = static_cast<int> (x.size());
    double peak = 0.0;
    for (int i = from; i < to; ++i)
    {
        peak = std::max (peak, static_cast<double> (std::abs (x[static_cast<size_t> (i)])));
        for (int p = 0; p < 3; ++p)
        {
            double acc = 0.0;
            for (int k = 0; k < 2 * kHalf; ++k)
            {
                const int j = i + 1 - kHalf + k;
                if (j >= 0 && j < n)
                    acc += taps[p][k] * x[static_cast<size_t> (j)];
            }
            peak = std::max (peak, std::abs (acc));
        }
    }
    return toDb (peak);
}

/** Hot programme full of inter-sample peaks, band-limited to 0.4 fs: an fs/4
    sine at 45 degrees (every sample 3 dB below its true peak) whose level
    swings at 3 Hz, over noise low-passed at 0.4 fs. */
Planar makeHotIspProgramme (int numSamples, double sampleRate, uint32_t seed)
{
    // Windowed-sinc low-pass at 0.4 fs (Blackman, 127 taps) for the noise.
    constexpr int kTaps = 127;
    std::vector<double> lp (kTaps);
    for (int k = 0; k < kTaps; ++k)
    {
        const double m = k - (kTaps - 1) / 2.0;
        const double sinc = m == 0.0 ? 0.8 : std::sin (kPi * 0.8 * m) / (kPi * m);
        const double w = 0.42 - 0.5 * std::cos (kTwoPi * k / (kTaps - 1)) + 0.08 * std::cos (2.0 * kTwoPi * k / (kTaps - 1));
        lp[static_cast<size_t> (k)] = sinc * w;
    }
    Planar p (2, numSamples);
    for (size_t c = 0; c < 2; ++c)
    {
        const auto white = whiteNoise (numSamples + kTaps, 1.0f, seed + static_cast<uint32_t> (c));
        for (int i = 0; i < numSamples; ++i)
        {
            double noise = 0.0;
            for (int k = 0; k < kTaps; ++k)
                noise += lp[static_cast<size_t> (k)] * white[static_cast<size_t> (i + k)];
            const double t = i / sampleRate;
            const double env = 0.6 + 0.4 * std::sin (kTwoPi * 3.0 * t + static_cast<double> (c));
            const double isp = env * std::sin (kTwoPi * 0.25 * i + 0.25 * kPi);
            p.ch[c][static_cast<size_t> (i)] = static_cast<float> (0.75 * isp + 0.35 * noise);
        }
    }
    return p;
}
} // namespace

//==============================================================================
// R3.5 - SafetyGovernor clip-energy branch
//==============================================================================
TEST_CASE ("Protection: the SafetyGovernor's clip-energy branch alone backs off at 15 %/s, holds inside its hysteresis and recovers at 3 %/s")
{
    // The limiter GR input stays at 0 dB throughout, so every move of the
    // scale here comes from the clip-energy ratio (budget -30 dB, recovery
    // below -31.5 dB, power-domain 3 s average).
    constexpr int kBlock = 480; // 10 ms at 48 kHz
    constexpr int kPerSecond = 100;
    SafetyGovernor g;
    g.prepare (kFs);
    const auto feed = [&] (float clipDb, int blocks) {
        for (int b = 0; b < blocks; ++b)
            g.update (0.0f, clipDb, kBlock);
    };

    // Well inside the budget: nothing happens.
    feed (-40.0f, 10 * kPerSecond);
    CHECK (g.getScale() == 1.0f);

    // 10 dB over the budget: the power average crosses -30 dB after ~0.3 s,
    // then the scale falls at 0.15 per second down to the 0.3 floor.
    feed (-20.0f, 1 * kPerSecond);
    const float at1 = g.getScale();
    feed (-20.0f, 1 * kPerSecond);
    const float at2 = g.getScale();
    CHECK (at1 < 1.0f);
    CHECK_NEAR (at1 - at2, 0.15, 0.002);
    feed (-20.0f, 6 * kPerSecond);
    CHECK (g.getScale() == 0.3f);

    // Clean again: the average needs ~8 s to fall below -31.5 dB (from
    // -20 dB with a 3 s time constant); then the scale rises at 0.03 per second.
    feed (-160.0f, 7 * kPerSecond);
    CHECK (g.getScale() == 0.3f);
    feed (-160.0f, 3 * kPerSecond);
    const float r1 = g.getScale();
    feed (-160.0f, 2 * kPerSecond);
    const float r2 = g.getScale();
    CHECK (r1 > 0.3f);
    CHECK_NEAR (r2 - r1, 0.06, 0.002);

    // -31 dB sits inside the 1.5 dB hysteresis: once the rising average has
    // passed -31.5 dB (after < 6 s) the scale neither falls nor rises.
    feed (-31.0f, 8 * kPerSecond);
    const float held = g.getScale();
    CHECK (held > r2); // it was still recovering while the average was below -31.5 dB
    feed (-31.0f, 20 * kPerSecond);
    CHECK (g.getScale() == held);

    // The average is taken in the power domain (bursts are not
    // under-weighted), so one 10 ms block with half of its energy clipped
    // (-3 dB) already trips it, while a -15 dB block does not; reset()
    // restores 1.
    feed (-160.0f, 30 * kPerSecond);
    const float before = g.getScale();
    feed (-15.0f, 1);
    CHECK (g.getScale() >= before);
    feed (-160.0f, 30 * kPerSecond);
    const float before2 = g.getScale();
    feed (-3.0f, 1);
    CHECK (g.getScale() < before2);
    g.reset();
    CHECK (g.getScale() == 1.0f);
}

TEST_CASE ("Protection: governed Boost drive into heavy clipping trips the clip-energy budget; the governor scales only the governed contributions, never the base values, and releases when the signal calms")
{
    // Base drive 10 dB + Boost 100 % (+8 dB governed) with the clipper at its
    // maximum share (threshold 0.3 dB above the ceiling): the clipper does
    // nearly all the work, so the limiter's GR average stays inside its
    // -6 dB budget and only the clip-energy branch can trip the governor.
    ParameterStore store;
    store.set (Mode, static_cast<float> (ModeValue::Music));
    store.set (BoostIntensity, 1.0f);
    store.set (MaxClipAmount, 1.0f);
    store.set (MaxDriveDb, 10.0f);
    store.set (BassBoostDb, 1.0f);
    store.set (SatDriveDb, 2.0f);
    store.set (BassHarmonics, 0.1f);
    store.set (ClarityPresence, 0.2f);
    std::vector<float> baseBefore (static_cast<size_t> (kNumParams)), baseAfter (static_cast<size_t> (kNumParams));
    store.snapshot (baseBefore.data());

    // 10 ms blocks: each is one processing segment that ends on a governor
    // tick (docs/11 E06), so the effective values of a block were computed
    // with the scale published after the previous one.
    constexpr int kBlock = 480;
    ProcessingChain chain (store);
    chain.prepare ({ kFs, kBlock, 2 });

    const int hotLen = static_cast<int> (kFs * 8.0);
    const int calmLen = static_cast<int> (kFs * 20.0);
    auto hot = makeProgramme (hotLen, 0.5f, 3);
    auto calm = makeProgramme (calmLen, 0.02f, 4);

    struct Governed
    {
        int id;
        float amount; // Boost 100 % amount (curve = 1 at 100 %)
    };
    const Governed governed[] = { { MaxDriveDb, 8.0f }, { BassBoostDb, 5.0f }, { BassHarmonics, 0.3f }, { SatDriveDb, 4.0f } };

    // Mirror of the governor's inputs (published per block after its update).
    const double a = std::exp (-kBlock / kFs / 3.0);
    double avgGr = 0.0, avgClipPow = 0.0, worstAvgGr = 0.0, peakAvgClipDb = -200.0;
    float prevScale = 1.0f, minScale = 1.0f, scaleEndHot = 1.0f;
    double maxContributionError = 0.0;
    bool ungovernedStable = true;

    ScopedNoDenormals noDenormals;
    const auto runPart = [&] (Planar& buf, bool isHot) {
        for (int pos = 0; pos < buf.numSamples(); pos += kBlock)
        {
            const int len = std::min (kBlock, buf.numSamples() - pos);
            chain.process (buf.block (pos, len));
            // The effective values of this block were computed with the scale
            // the governor left after the previous block.
            for (const auto& gv : governed)
            {
                const float expected = std::clamp (baseBefore[static_cast<size_t> (gv.id)] + gv.amount * prevScale,
                                                   layout()[static_cast<size_t> (gv.id)].minValue, layout()[static_cast<size_t> (gv.id)].maxValue);
                maxContributionError = std::max (maxContributionError, static_cast<double> (std::abs (chain.effectiveValue (gv.id) - expected)));
            }
            // Ungoverned Boost entries never move with the scale.
            ungovernedStable = ungovernedStable && std::abs (chain.effectiveValue (ClarityPresence) - 0.55f) < 1e-6f
                               && std::abs (chain.effectiveValue (SpatialWidth) - 1.2f) < 1e-6f
                               && std::abs (chain.effectiveValue (MaxGlue) - 0.3f) < 1e-6f;

            const auto& m = chain.meters();
            const float scale = m.governorScale.load();
            const double grDb = m.maxGainReductionDb.load();
            const double clipDb = m.clipEnergyRatioDb.load();
            avgGr = a * avgGr + (1.0 - a) * grDb;
            avgClipPow = a * avgClipPow + (1.0 - a) * (clipDb <= -160.0 ? 0.0 : std::pow (10.0, clipDb / 10.0));
            if (isHot)
            {
                worstAvgGr = std::min (worstAvgGr, avgGr);
                peakAvgClipDb = std::max (peakAvgClipDb, 10.0 * std::log10 (std::max (1e-30, avgClipPow)));
            }
            minScale = std::min (minScale, scale);
            prevScale = scale;
        }
    };

    runPart (hot, true);
    scaleEndHot = prevScale;

    CHECK_GE (worstAvgGr, -6.0 + 1.5); // the GR branch was never over budget (nor inside its hysteresis)...
    CHECK_GE (peakAvgClipDb, -30.0 + 6.0); // ...the clip-energy branch was, by a wide margin
    CHECK_LE (scaleEndHot, 0.35f);         // 8 s of heavy clipping: the scale reached (nearly) its 0.3 floor
    CHECK_GE (minScale, 0.3f);
    CHECK_LE (chain.effectiveValue (MaxDriveDb), 10.0f + 8.0f * 0.35f + 1e-4f);

    runPart (calm, false);
    CHECK_LE (maxContributionError, 1e-4);
    CHECK (ungovernedStable);
    // Calm programme: no clipping, no limiting. The clip average (about
    // -11 dB at its peak) needs roughly 12 s to fall 1.5 dB under the
    // budget, then the scale climbs at 3 %/s (about 0.55 after 20 s).
    CHECK_GE (prevScale, scaleEndHot + 0.1f);
    CHECK_GE (chain.effectiveValue (MaxDriveDb), 10.0f + 8.0f * (scaleEndHot + 0.1f) - 1e-3f);

    // The governor only ever touched effective values: the store is intact.
    store.snapshot (baseAfter.data());
    CHECK (baseAfter == baseBefore);
    CHECK (chain.meters().safetyClipCount.load() == 0);
}

//==============================================================================
// R6.1 - device-profile ceiling caps on the master limiter; air cut-off
//==============================================================================
TEST_CASE ("Headset: the master limiter at the Bluetooth -2 dBTP and hands-free -3 dBTP caps holds the 4x true peak of hot inter-sample-peak material")
{
    device::Database db;
    std::string err;
    REQUIRE (db.loadBuiltIn (err));

    struct Case
    {
        const char* endpoint;
        double sampleRate;
        int channels;
        device::Connection hint;
        float expectedCap;
    };
    const Case cases[] = {
        { "Headphones (Stealth 600 Gen 3)", 48000.0, 2, device::Connection::Bluetooth, -2.0f },
        { "Headphones (Stealth 600 Gen 3)", 44100.0, 2, device::Connection::Bluetooth, -2.0f },
        { "Headset (Stealth 600 Gen 3 Hands-Free AG Audio)", 16000.0, 1, device::Connection::Unknown, -3.0f },
        { "Headset (Stealth 600 Gen 3 Hands-Free AG Audio)", 8000.0, 1, device::Connection::Unknown, -3.0f },
    };

    for (const auto& tc : cases)
    {
        // What the app does: advice for the matched endpoint -> master ceiling.
        const auto advice = device::adviceFor (db.match (tc.endpoint, tc.sampleRate, tc.channels, tc.hint), tc.sampleRate, false);
        REQUIRE (advice.ceilingDbTp == tc.expectedCap);

        constexpr int kBlock = 256;
        MixEngine mix;
        const std::vector<StripConfig> layout { { "Game", 2, 0.0f, false }, { "Music", 2, 0.0f, false } };
        mix.configure (layout, tc.sampleRate, kBlock);
        // Strips as pure delays: the hot material reaches the master unchanged.
        for (int s = 0; s < mix.getNumStrips(); ++s)
            bypassAllModules (mix.params (s));
        mix.configure (layout, tc.sampleRate, kBlock);
        mix.setMasterCeilingDb (advice.ceilingDbTp);

        const int total = static_cast<int> (tc.sampleRate * 3.0) / kBlock * kBlock;
        const auto a = makeHotIspProgramme (total, tc.sampleRate, 17);
        const auto b = makeHotIspProgramme (total, tc.sampleRate, 29);
        // The un-limited sum, for reference: far over the cap in true peak.
        std::vector<float> sumL (static_cast<size_t> (total));
        for (int i = 0; i < total; ++i)
            sumL[static_cast<size_t> (i)] = a.ch[0][static_cast<size_t> (i)] + b.ch[0][static_cast<size_t> (i)];
        const double inputTp = independentTruePeakDb (sumL, 0, total);
        CHECK_GE (inputTp, advice.ceilingDbTp + 6.0);

        Planar game (2, kBlock), music (2, kBlock), out (2, total);
        const AudioBlock gb = game.block(), mb = music.block();
        const AudioBlock* inputs[] = { &gb, &mb };
        float deepestGr = 0.0f;
        {
            ScopedNoDenormals noDenormals;
            for (int pos = 0; pos < total; pos += kBlock)
            {
                for (size_t c = 0; c < 2; ++c)
                {
                    std::copy_n (a.ch[c].begin() + pos, kBlock, game.ch[c].begin());
                    std::copy_n (b.ch[c].begin() + pos, kBlock, music.ch[c].begin());
                }
                mix.process (inputs, out.block (pos, kBlock));
                deepestGr = std::min (deepestGr, mix.getMasterGainReductionDb());
            }
        }
        CHECK_LE (deepestGr, -6.0); // the master really was limiting hard

        TruePeakMeter tp;
        tp.prepare (2);
        tp.process (out.block());
        double independentTp = -200.0;
        for (const auto& c : out.ch)
            independentTp = std::max (independentTp, independentTruePeakDb (c, 0, total));
        CHECK_LE (tp.getMaxDbAllChannels(), advice.ceilingDbTp + 0.1);
        CHECK_LE (independentTp, advice.ceilingDbTp + 0.1);
        for (const auto& c : out.ch)
            CHECK_LE (peakAbs (c.data(), total), dbToGain (advice.ceilingDbTp) + 1e-6);
        CHECK (mix.getMasterSafetyClipCount() == 0);
        // The output is still hot (limited, not muted): within 3 dB of the cap.
        CHECK_GE (toDb (peakAbs (out.ch[0].data(), total)), advice.ceilingDbTp - 3.0);
    }
}

TEST_CASE ("Headset: below 42 kHz (hands-free 8 / 16 kHz, USB 32 kHz) the air exciter is cut off - nothing is added above the input band; at 44.1 / 48 kHz the same setting adds its harmonics and shelf")
{
    // Only Clarity runs, with nothing but Air at 100 % (plus the Clarity
    // macro, which raises air too). The input is a tone inside the exciter's
    // 3.5 - 7 kHz band where the rate allows, plus a quiet tone for the
    // 10 kHz shelf where it fits under Nyquist.
    for (double sr : { 8000.0, 16000.0, 32000.0, 44100.0, 48000.0 })
    {
        const bool airAllowed = sr >= 42000.0;
        const double f0 = std::min (5000.0, 0.45 * sr); // 3.6 kHz at 8 kHz, 5 kHz elsewhere
        const double fShelf = 12000.0;
        const bool shelfTone = fShelf < 0.45 * sr;

        ParameterStore store;
        bypassAllModules (store);
        store.set (ClarityOn, 1.0f);
        store.set (ClarityAir, 1.0f);
        ProcessingChain chain (store);
        chain.prepare ({ sr, 256, 2 });
        const int lat = chain.getLatencySamples();

        const int n = static_cast<int> (sr * 1.0);
        Planar in (2, n);
        const auto tone = sine (f0, sr, n, 0.25f);
        const auto high = shelfTone ? sine (fShelf, sr, n, 0.01f) : std::vector<float> (static_cast<size_t> (n), 0.0f);
        for (auto& c : in.ch)
            for (int i = 0; i < n; ++i)
                c[static_cast<size_t> (i)] = tone[static_cast<size_t> (i)] + high[static_cast<size_t> (i)];
        Planar buf = in;
        runChain (chain, buf, 256);

        CHECK (chain.effectiveValue (ClarityAir) == (airAllowed ? 1.0f : 0.0f));
        // The macro route is cut off the same way.
        store.set (Macro3, 1.0f);
        Planar one (2, 256);
        chain.process (one.block());
        CHECK (chain.effectiveValue (ClarityAir) == (airAllowed ? 1.0f : 0.0f));

        const int from = lat + n / 2, len = n - from;
        double maxErr = 0.0;
        for (size_t c = 0; c < 2; ++c)
            for (int i = from; i < n; ++i)
                maxErr = std::max (maxErr, static_cast<double> (std::abs (buf.ch[c][static_cast<size_t> (i)] - in.ch[c][static_cast<size_t> (i - lat)])));
        if (! airAllowed)
        {
            // Clarity with air forced to 0 is an exact pass-through: the output
            // is the delayed input, so no harmonic, alias or shelf energy is
            // added anywhere in the spectrum.
            CHECK_LE (maxErr, 1e-6);
        }
        else
        {
            CHECK_GE (maxErr, 1e-3);
            const double h2 = toDb (toneAmplitude (buf.ch[0].data() + from, len, 2.0 * f0, sr) / 0.25);
            CHECK_GE (h2, -30.0); // the 2nd harmonic of the band tone is there
            const double shelfDb = toDb (toneAmplitude (buf.ch[0].data() + from, len, fShelf, sr) / 0.01);
            CHECK_GE (shelfDb, 1.0); // +2 dB shelf at 10 kHz lifts the 12 kHz tone
        }
    }
}

//==============================================================================
// R2.1 / R2.10 - ComparisonMatcher (loudness-matched bypass) as a unit
//==============================================================================
namespace
{
struct Trims
{
    float dry = 0.0f, wet = 0.0f;
};

/** Feeds `seconds` of a noisy 1 kHz programme at `dryLevel` to the dry side
    and the same programme `wetDb` louder (or silence, with wetSilent) to the
    wet side, in 10 ms blocks, with the bypass and matching flags given;
    returns the trims after every block. */
std::vector<Trims> runMatch (ComparisonMatcher& cm, double seconds, float dryLevel, float wetDb, bool bypass, uint32_t seed,
                             bool wetSilent = false, bool matching = true)
{
    constexpr int kBlock = 480;
    std::vector<Trims> trims;
    FastRandom rng (seed);
    Planar dry (2, kBlock), wet (2, kBlock);
    int64_t clock = 0;
    const float wetGain = wetSilent ? 0.0f : dbToGain (wetDb);
    const int blocks = static_cast<int> (std::lround (seconds * kFs / kBlock));
    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < kBlock; ++i, ++clock)
        {
            const float x = dryLevel * (0.7f * static_cast<float> (std::sin (kTwoPi * 1000.0 * static_cast<double> (clock) / kFs)) + 0.3f * rng.nextBipolar());
            for (size_t c = 0; c < 2; ++c)
            {
                dry.ch[c][static_cast<size_t> (i)] = x;
                wet.ch[c][static_cast<size_t> (i)] = wetGain * x;
            }
        }
        cm.measureDry (dry.block());
        cm.measureWet (wet.block());
        cm.update (bypass, matching, kBlock);
        trims.push_back ({ cm.getDryTrimDb(), cm.getWetTrimDb() });
    }
    return trims;
}
} // namespace

TEST_CASE ("ComparisonMatcher: only the louder side is turned down, by the measured difference, and only in a comparison")
{
    // docs/11 E37: the matched bypass used to raise the dry reference (+-12 dB,
    // 3 dB/s), capped at the ceiling, which left it up to 3 LU short on hot
    // programme. Now the louder side is attenuated and nothing is raised.
    for (float diffDb : { 6.0f, -9.0f, 0.0f, 11.5f, 25.0f, -25.0f })
    {
        ComparisonMatcher cm;
        cm.prepare (kFs, 2);
        // 4 s of programme with the bypass off: no comparison, no trim.
        for (const auto& t : runMatch (cm, 4.0, 0.1f, diffDb, false, 21))
            CHECK (t.dry == 0.0f && t.wet == 0.0f);
        CHECK (! cm.isComparing());

        // Bypass engaged: the trims are there from the first block (the
        // measures have been running) and never positive.
        const auto trims = runMatch (cm, 3.0, 0.1f, diffDb, true, 22);
        const float expected = std::clamp (diffDb, -ComparisonMatcher::kMaxTrimDb, ComparisonMatcher::kMaxTrimDb);
        for (const auto& t : trims)
        {
            CHECK (t.dry <= 0.0f);
            CHECK (t.wet <= 0.0f);
        }
        CHECK (cm.isComparing());
        CHECK (cm.isFrozen());
        // Identical spectra: K-weighting cancels, the trim is the difference.
        CHECK_NEAR (trims.front().wet, -std::max (0.0f, expected), 0.05);
        CHECK_NEAR (trims.front().dry, std::min (0.0f, expected), 0.05);
        CHECK_NEAR (trims.back().wet, -std::max (0.0f, expected), 0.05);
        CHECK_NEAR (trims.back().dry, std::min (0.0f, expected), 0.05);
    }
}

TEST_CASE ("ComparisonMatcher: acquires in 1 s of programme, holds still for a whole comparison, ends 10 s after the last bypass-off and releases at 2 dB/s")
{
    {
        // A processed side that is silent while the reference plays is not
        // matched down to (it is not programme).
        ComparisonMatcher silentWet;
        silentWet.prepare (kFs, 2);
        for (const auto& t : runMatch (silentWet, 3.0, 0.1f, 0.0f, true, 8, true))
            CHECK (t.dry == 0.0f && t.wet == 0.0f);
        CHECK (! silentWet.hasMeasurement());
    }

    ComparisonMatcher cm;
    cm.prepare (kFs, 2);
    // A comparison that starts in silence waits for programme.
    for (const auto& t : runMatch (cm, 2.0, 0.0f, 0.0f, true, 9))
        CHECK (t.dry == 0.0f && t.wet == 0.0f);
    CHECK (! cm.isFrozen());

    // Processed +6 dB: the window is valid after 0.4 s of programme, the
    // trim follows it for 1 s, then it is frozen.
    auto trims = runMatch (cm, 1.3, 0.1f, 6.0f, true, 9);
    CHECK (! cm.isFrozen());
    trims = runMatch (cm, 0.2, 0.1f, 6.0f, true, 10);
    CHECK (cm.isFrozen());
    const float frozen = cm.getWetTrimDb();
    CHECK_NEAR (frozen, -6.0, 0.1);
    CHECK (cm.getDryTrimDb() == 0.0f);

    // A 10 s comparison: flips every 2 s while the processed side changes
    // (+6 -> +10 -> +2 dB) - the trims do not move at all (a tracking match
    // audibly rode the reference level).
    float lowest = frozen, highest = frozen;
    for (int flip = 0; flip < 5; ++flip)
    {
        const float wetDb = flip < 2 ? 10.0f : 2.0f;
        for (const auto& t : runMatch (cm, 2.0, 0.1f, wetDb, flip % 2 == 1, static_cast<uint32_t> (30 + flip)))
        {
            lowest = std::min (lowest, t.wet);
            highest = std::max (highest, t.wet);
            CHECK (t.dry == 0.0f);
        }
    }
    CHECK_LE (highest - lowest, 0.2); // docs/11 E37 Done-when: trim variance <= 0.2 dB
    CHECK (highest == frozen && lowest == frozen);

    // The last flip left the bypass off at 10 s; the comparison ends after
    // 10 s of bypass off in all (8 s more here), and the processed side
    // then returns to its own level at 2 dB/s (0.02 dB per 10 ms block).
    trims = runMatch (cm, 7.9, 0.1f, 2.0f, false, 40);
    CHECK (cm.isComparing());
    CHECK (trims.back().wet == frozen);
    trims = runMatch (cm, 4.0, 0.1f, 2.0f, false, 41);
    CHECK (! cm.isComparing());
    float prev = frozen, largestStep = 0.0f;
    for (const auto& t : trims)
    {
        CHECK (t.wet >= prev);
        largestStep = std::max (largestStep, t.wet - prev);
        prev = t.wet;
    }
    CHECK_LE (largestStep, 0.02 + 1e-5);
    CHECK (trims.back().wet == 0.0f);

    // A new comparison acquires the difference as it is now (+2 dB).
    runMatch (cm, 1.5, 0.1f, 2.0f, true, 42);
    CHECK_NEAR (cm.getWetTrimDb(), -2.0, 0.1);

    // Matching switched off: the dry trim goes at once, the wet trim returns
    // at 2 dB/s; reset() clears everything.
    runMatch (cm, 1.5, 0.1f, -5.0f, false, 43, false, false);
    CHECK (cm.getDryTrimDb() == 0.0f);
    CHECK (! cm.isComparing());
    CHECK (cm.getWetTrimDb() == 0.0f);
    // The measures keep running outside a comparison, so one that starts
    // after the programme settled (processed now 5 dB quieter) is right at once.
    runMatch (cm, 8.0, 0.1f, -5.0f, false, 44);
    trims = runMatch (cm, 1.5, 0.1f, -5.0f, true, 45);
    CHECK_NEAR (trims.front().dry, -5.0, 0.1);
    CHECK_NEAR (trims.back().dry, -5.0, 0.1);
    CHECK (trims.back().wet == 0.0f);
    cm.reset();
    CHECK (cm.getDryTrimDb() == 0.0f);
    CHECK (cm.getWetTrimDb() == 0.0f);
    CHECK (! cm.isComparing());
}

TEST_CASE ("Chain: on hot programme the loudness-matched bypass and the processed side match within 0.5 LU through a 10 s comparison, the reference below the ceiling")
{
    // docs/11 E37 Done-when. Music mode, Loudness macro 100 % (the processed
    // side is limited at the -1 dBTP ceiling), two programmes: dense pink
    // noise (-15.8 LUFS, peaks -5.3 dBFS) and 55 Hz kicks over a dull bed
    // (-18.2 LUFS, peaks -3.4 dBFS). 6 s processed, then bypass on / off /
    // on / off / on in 2 s steps, then off. Loudness is the gated integrated
    // loudness of each step (from 0.2 s after the flip).
    // Before E37 (the dry reference raised, capped at ceiling - held dry
    // peak, then limited): bypass read 1.96 to 2.31 LU (dense) and 3.04 to
    // 3.07 LU (kicks) under the processed side, which played at its own
    // level. Now the processed side is turned down to the reference for the
    // comparison: every step reads within 0.1 LU of every other.
    constexpr int kBlock = 512;
    const int n = static_cast<int> (kFs * 18.0);
    const auto at = [] (double seconds) { return static_cast<int> (seconds * kFs); };
    const auto loudness = [&] (const Planar& p, double from, double to) {
        LoudnessMeter meter;
        meter.prepare (kFs, 2);
        const int a = at (from), len = at (to) - a;
        Planar seg (2, len);
        for (size_t c = 0; c < 2; ++c)
            std::copy (p.ch[c].begin() + a, p.ch[c].begin() + a + len, seg.ch[c].begin());
        meter.process (seg.block());
        return static_cast<double> (meter.getIntegratedLufs());
    };

    for (int programme = 0; programme < 2; ++programme)
    {
        Planar in (2, n);
        if (programme == 0)
        {
            in.ch[0] = pinkNoise (n, 0.12f, 11);
            in.ch[1] = pinkNoise (n, 0.12f, 12);
        }
        else
        {
            FastRandom rng (5);
            float bed = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                const double beat = std::fmod (i / kFs, 0.5);
                bed += 0.05f * (rng.nextBipolar() - bed);
                const double kick = 0.7 * std::exp (-beat * 12.0) * std::sin (kTwoPi * 55.0 * beat);
                for (auto& c : in.ch)
                    c[static_cast<size_t> (i)] = static_cast<float> (0.05 * bed + kick);
            }
        }
        ParameterStore store;
        store.set (Mode, static_cast<float> (ModeValue::Music));
        store.set (Macro4, 1.0f);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, kBlock, 2 });
        Planar buf = in;
        {
            ScopedNoDenormals noDenormals;
            for (int pos = 0; pos < n; pos += kBlock)
            {
                const double t = pos / kFs;
                const bool bypass = (t >= 6.0 && t < 8.0) || (t >= 10.0 && t < 12.0) || (t >= 14.0 && t < 16.0);
                store.set (BypassAll, bypass ? 1.0f : 0.0f);
                chain.process (buf.block (pos, std::min (kBlock, n - pos)));
            }
        }

        const double processed = loudness (buf, 3.0, 6.0), input = loudness (in, 3.0, 6.0);
        CHECK_GE (processed - input, 5.0); // processing is clearly louder
        std::vector<double> steps;
        for (double from = 6.0; from < 17.0; from += 2.0)
            steps.push_back (loudness (buf, from + 0.2, from + 2.0));
        const auto [lo, hi] = std::minmax_element (steps.begin(), steps.end());
        CHECK_LE (*hi - *lo, 0.5);
        CHECK_NEAR (*lo, input, 0.5); // matched at the reference (the input), not raised

        // The reference stays below the ceiling (sample and true peak).
        TruePeakMeter tp;
        tp.prepare (2);
        for (double from : { 6.1, 10.1, 14.1 })
            tp.process (buf.block (at (from), at (1.9)));
        CHECK_LE (tp.getMaxDbAllChannels(), -1.0 + 0.15);
    }
}

//==============================================================================
// R2.10 - click-free A/B bank switches and global bypass
//==============================================================================
TEST_CASE ("A/B: bank switches and global bypass toggles (matched and unmatched) are click-free on a sine")
{
    // Bank A: Music with Boost 50 %. Bank B differs audibly: +9 dB at 1 kHz,
    // more presence, saturation switched on, wider, +3 dB drive. The switch
    // is one atomic; every difference must glide or crossfade. Criterion: the
    // largest sample-to-sample step around each event is no larger than the
    // steady signal's own largest step (before or after) times 1.1.
    constexpr int kBlock = 256;
    const int second = static_cast<int> (kFs);
    const int total = 5 * second;
    Planar in (2, total);
    for (int i = 0; i < total; ++i)
    {
        in.ch[0][static_cast<size_t> (i)] = 0.2f * static_cast<float> (std::sin (kTwoPi * 1000.0 * i / kFs));
        in.ch[1][static_cast<size_t> (i)] = 0.2f * static_cast<float> (std::sin (kTwoPi * 1000.0 * i / kFs + 0.5));
    }

    for (bool matched : { true, false })
    {
        ParameterStore store;
        store.set (Bank::A, BoostIntensity, 0.5f);
        store.set (Bank::A, LoudnessMatchBypass, matched ? 1.0f : 0.0f);
        store.copyBank (Bank::A, Bank::B);
        store.set (Bank::B, eq (5, EqFieldGain), 9.0f); // band 5 = 1 kHz bell
        store.set (Bank::B, ClarityPresence, 0.8f);
        store.set (Bank::B, SaturationOn, 1.0f);
        store.set (Bank::B, SatDriveDb, 6.0f);
        store.set (Bank::B, SpatialWidth, 1.5f);
        store.set (Bank::B, MaxDriveDb, 3.0f);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, kBlock, 2 });

        // Events on block boundaries at ~1 s (bypass on), ~2 s (bypass off),
        // ~3 s (A -> B) and ~4 s (B -> A). Bypass comes first so the matched
        // gain has only ever seen bank A (its 3 s loudness memory would
        // otherwise still be settling after the louder bank B).
        const int events[] = { second / kBlock * kBlock, 2 * second / kBlock * kBlock, 3 * second / kBlock * kBlock, 4 * second / kBlock * kBlock };
        Planar buf = in;
        {
            ScopedNoDenormals noDenormals;
            for (int pos = 0; pos < total; pos += kBlock)
            {
                if (pos == events[0])
                    store.set (BypassAll, 1.0f);
                if (pos == events[1])
                    store.set (BypassAll, 0.0f);
                if (pos == events[2])
                    store.setActiveBank (Bank::B);
                if (pos == events[3])
                    store.setActiveBank (Bank::A);
                chain.process (buf.block (pos, std::min (kBlock, total - pos)));
            }
        }

        const int pre = static_cast<int> (0.6 * kFs), guard = static_cast<int> (0.05 * kFs), settle = static_cast<int> (0.3 * kFs);
        for (int e : events)
            for (size_t c = 0; c < 2; ++c)
            {
                const auto& y = buf.ch[c];
                const double own = std::max (maxStep (y, e - pre, e - guard), maxStep (y, e + settle, e + settle + pre - guard));
                const double around = maxStep (y, e - guard, e + settle);
                CHECK (own > 0.01);
                CHECK_LE (around, 1.1 * own);
            }
        // The banks really differ: bank B's steady output moves at least
        // twice as fast (+9 dB at 1 kHz) as bank A's.
        CHECK_GE (maxStep (buf.ch[0], events[2] + settle, events[3] - guard), 2.0 * maxStep (buf.ch[0], events[1] + settle, events[2] - guard));
    }
}

//==============================================================================
// R2.8 - Music Width and Clarity macros
//==============================================================================
TEST_CASE ("Macros: Music Width engages Stereo and raises width 1 -> 1.6 and space 0 -> 0.35 (from 40 %) monotonically, ungoverned, clamped at 2")
{
    auto base = defaultBase();
    base[Mode] = static_cast<float> (ModeValue::Music);
    base[SpatialOn] = 0.0f;
    std::vector<float> eff (static_cast<size_t> (kNumParams)), gov (static_cast<size_t> (kNumParams));
    float prevWidth = 1.0f, prevSpace = 0.0f;
    for (int step = 0; step <= 20; ++step)
    {
        const float v = static_cast<float> (step) / 20.0f;
        base[Macro2] = v;
        MacroMap::apply (base.data(), eff.data(), 1.0f);
        MacroMap::apply (base.data(), gov.data(), 0.3f);
        CHECK (eff[SpatialWidth] >= prevWidth);
        CHECK (eff[SpatialSpace] >= prevSpace);
        CHECK (eff[SpatialWidth] == gov[SpatialWidth]); // not governed
        CHECK (eff[SpatialSpace] == gov[SpatialSpace]);
        CHECK ((eff[SpatialOn] >= 0.5f) == (v >= 0.01f));
        if (v <= 0.4f)
            CHECK (eff[SpatialSpace] == 0.0f); // space starts at 40 %
        // Width touches nothing but the stereo module.
        for (int id : { ClarityPresence, ClarityAir, MaxDriveDb, BassBoostDb, SatDriveDb })
            CHECK (eff[static_cast<size_t> (id)] == base[static_cast<size_t> (id)]);
        prevWidth = eff[SpatialWidth];
        prevSpace = eff[SpatialSpace];
    }
    CHECK_NEAR (prevWidth, 1.6, 1e-5);
    CHECK_NEAR (prevSpace, 0.35, 1e-5);
    base[Macro2] = 0.5f;
    MacroMap::apply (base.data(), eff.data(), 1.0f);
    CHECK_NEAR (eff[SpatialWidth], 1.3, 1e-5); // smoothstep(0, 1, 0.5) = 0.5
    // With Boost 100 % (+0.2) the documented maximum is 1.8; a wide base clamps at 2.
    base[Macro2] = 1.0f;
    base[BoostIntensity] = 1.0f;
    MacroMap::apply (base.data(), eff.data(), 0.3f);
    CHECK_NEAR (eff[SpatialWidth], 1.8, 1e-5);
    base[SpatialWidth] = 1.9f;
    MacroMap::apply (base.data(), eff.data(), 1.0f);
    CHECK (eff[SpatialWidth] == 2.0f);
}

TEST_CASE ("Macros: Music Clarity engages Clarity and Dynamic EQ and raises presence (+0.8), air (+0.7 from 20 %) and de-mud (+0.5 by 70 %) monotonically, ungoverned")
{
    auto base = defaultBase();
    base[Mode] = static_cast<float> (ModeValue::Music);
    base[ClarityOn] = 0.0f;
    base[DynEqOn] = 0.0f;
    std::vector<float> eff (static_cast<size_t> (kNumParams)), gov (static_cast<size_t> (kNumParams));
    float prev[3] = { 0.0f, 0.0f, 0.0f };
    const int ids[3] = { ClarityPresence, ClarityAir, ClarityDeMud };
    for (int step = 0; step <= 20; ++step)
    {
        const float v = static_cast<float> (step) / 20.0f;
        base[Macro3] = v;
        MacroMap::apply (base.data(), eff.data(), 1.0f);
        MacroMap::apply (base.data(), gov.data(), 0.3f);
        for (size_t k = 0; k < 3; ++k)
        {
            CHECK (eff[static_cast<size_t> (ids[k])] >= prev[k]);
            CHECK (eff[static_cast<size_t> (ids[k])] == gov[static_cast<size_t> (ids[k])]); // not governed
            prev[k] = eff[static_cast<size_t> (ids[k])];
        }
        CHECK ((eff[ClarityOn] >= 0.5f) == (v >= 0.01f));
        CHECK ((eff[DynEqOn] >= 0.5f) == (v >= 0.01f));
        if (v <= 0.2f)
            CHECK (eff[ClarityAir] == 0.0f);
        if (v >= 0.7f)
            CHECK_NEAR (eff[ClarityDeMud], 0.5, 1e-5);
        for (int id : { SpatialWidth, MaxDriveDb, BassBoostDb, SatDriveDb, ClarityAttackDb })
            CHECK (eff[static_cast<size_t> (id)] == base[static_cast<size_t> (id)]);
    }
    CHECK_NEAR (prev[0], 0.8, 1e-5);
    CHECK_NEAR (prev[1], 0.7, 1e-5);
    CHECK_NEAR (prev[2], 0.5, 1e-5);
    // With Boost 100 % presence would reach 1.15 and clamps at 1; air reaches 1.0.
    base[BoostIntensity] = 1.0f;
    MacroMap::apply (base.data(), eff.data(), 1.0f);
    CHECK (eff[ClarityPresence] == 1.0f);
    CHECK_NEAR (eff[ClarityAir], 1.0, 1e-5);
}

TEST_CASE ("Macros: through the chain, Width raises the side / mid ratio and Clarity lifts quiet presence-band content, both in proportion to the macro")
{
    // Width: a partially correlated 1 kHz / 1.5 kHz pair (above the 180 Hz
    // width low cut); side / mid energy ratio of the output.
    const int n = static_cast<int> (kFs * 1.0);
    Planar stereo (2, n);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kFs;
        const double mid = 0.2 * std::sin (kTwoPi * 1000.0 * t), side = 0.05 * std::sin (kTwoPi * 1500.0 * t);
        stereo.ch[0][static_cast<size_t> (i)] = static_cast<float> (mid + side);
        stereo.ch[1][static_cast<size_t> (i)] = static_cast<float> (mid - side);
    }
    const auto sideToMidDb = [] (const Planar& p) {
        double s = 0.0, m = 0.0;
        for (int i = p.numSamples() / 2; i < p.numSamples(); ++i)
        {
            const double l = p.ch[0][static_cast<size_t> (i)], r = p.ch[1][static_cast<size_t> (i)];
            m += 0.25 * (l + r) * (l + r);
            s += 0.25 * (l - r) * (l - r);
        }
        return 10.0 * std::log10 (s / m);
    };
    double prevRatio = -1000.0, ratio0 = 0.0;
    for (float v : { 0.0f, 0.5f, 1.0f })
    {
        ParameterStore store;
        bypassAllModules (store);
        store.set (Macro2, v);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, 256, 2 });
        Planar buf = stereo;
        runChain (chain, buf, 256);
        const double r = sideToMidDb (buf);
        if (v == 0.0f)
            ratio0 = r;
        CHECK (r > prevRatio + (v == 0.0f ? 0.0 : 1.0));
        prevRatio = r;
    }
    CHECK_GE (prevRatio - ratio0, 3.0); // width 1.6: side up by about 4 dB relative to mid

    // Clarity: dynamic presence (up to +6 dB x presence when the band is
    // quiet) on a quiet 3.2 kHz tone; a loud one gets less.
    const auto presenceGainDb = [] (float macro, float amplitude) {
        ParameterStore store;
        bypassAllModules (store);
        store.set (Macro3, macro);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, 256, 2 });
        const int len = static_cast<int> (kFs * 1.0);
        Planar buf (2, len);
        const auto s = sine (3200.0, kFs, len, amplitude);
        buf.ch[0] = s;
        buf.ch[1] = s;
        buf.rebind();
        runChain (chain, buf, 256);
        return toDb (toneAmplitude (buf.ch[0].data() + len / 2, len / 2, 3200.0, kFs) / amplitude);
    };
    const double quiet0 = presenceGainDb (0.0f, 0.01f), quietHalf = presenceGainDb (0.5f, 0.01f), quietFull = presenceGainDb (1.0f, 0.01f);
    const double loudFull = presenceGainDb (1.0f, 0.5f);
    CHECK_NEAR (quiet0, 0.0, 0.01);
    CHECK_GE (quietHalf, quiet0 + 1.0);
    CHECK_GE (quietFull, quietHalf + 1.0);
    CHECK_GE (quietFull, 3.0);
    CHECK_LE (loudFull, quietFull - 2.0);
}

//==============================================================================
// GatedLoudness: relative-gate release and cold start; the chain's control
// loops across a dropped block; meters of bypassed slots; the governor's GR
// input across block sizes
//==============================================================================
namespace
{
/** Feeds `seconds` of a stereo 1 kHz sine of peak `amplitude` (at 1 kHz the
    K-weighting is ~0 dB, so a peak of X dBFS reads about X LUFS) to
    `perBlock` in 480-sample blocks; `phase` carries on across calls. */
template <typename Fn>
void feedTone (double seconds, float amplitude, double& phase, Fn&& perBlock)
{
    constexpr int kBlock = 480;
    Planar p (2, kBlock);
    const int blocks = static_cast<int> (std::lround (seconds * kFs / kBlock));
    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const float v = amplitude * static_cast<float> (std::sin (phase));
            p.ch[0][static_cast<size_t> (i)] = v;
            p.ch[1][static_cast<size_t> (i)] = v;
            phase = std::fmod (phase + kTwoPi * 1000.0 / kFs, kTwoPi);
        }
        perBlock (p.block());
    }
}
} // namespace

TEST_CASE ("GatedLoudness: programme more than 20 LU below the last one is held out for 3 s, then the slow measure restarts, so AutoLevel converges on it and AutoDrive releases its reduction")
{
    // Loud (-8 LUFS) then quiet (-30 LUFS, 22 LU below): the relative gate
    // closes on the quiet programme, and the slow measure it compares with
    // only moves while the gate is open. Without the release it stayed
    // closed for good (AutoLevel frozen near -9.4 dB, AutoDrive at its floor).
    AutoLevel al;
    al.prepare (kFs, 2);
    al.setEnabled (true);
    al.setTargetLufs (-18.0f);
    AutoDrive ad;
    ad.prepare (kFs, 2);
    constexpr float kDriveTarget = -14.0f, kRequestedDrive = 12.0f;
    const auto both = [&] (const AudioBlock& b) {
        ad.update (b, kDriveTarget, true, kRequestedDrive);
        al.process (b); // in place: AutoDrive saw the block first
    };

    double phase = 0.0;
    feedTone (20.0, dbToGain (-8.0f), phase, both);
    CHECK_NEAR (al.getGainDb(), -10.0, 0.3); // -18 - (-8)
    CHECK (ad.getReductionDb() == -kRequestedDrive); // 6 LU over: at its floor

    // The momentary follower needs about 0.7 s to fall 20 LU (the gate is
    // open that long); from then on both loops hold...
    feedTone (1.0, dbToGain (-30.0f), phase, both);
    const float heldGain = al.getGainDb(), heldReduction = ad.getReductionDb();
    CHECK_LE (heldGain, -9.0f);
    feedTone (1.5, dbToGain (-30.0f), phase, both); // 2.5 s of quiet programme in all
    CHECK (al.getGainDb() == heldGain);
    CHECK (ad.getReductionDb() == heldReduction);
    // ...and a pause (digital silence) neither releases nor restarts the count.
    feedTone (5.0, 0.0f, phase, both);
    CHECK (al.getGainDb() == heldGain);
    CHECK (ad.getReductionDb() == heldReduction);

    // After 3 s of quiet programme the slow measure restarts on it: AutoLevel
    // climbs to +6 dB (-18 - (-30) = +12, capped at +6 since docs/11 E21),
    // at 3 dB/s for the first 2 s after the freeze and 1 dB/s after that
    // (it climbed at 1 dB/s to +12 dB before E21), and AutoDrive (16 LU
    // under its target) gives the drive back at 2 dB/s.
    std::vector<float> gains;
    feedTone (30.0, dbToGain (-30.0f), phase, [&] (const AudioBlock& b) {
        both (b);
        gains.push_back (al.getGainDb());
    });
    CHECK_NEAR (al.getGainDb(), AutoLevel::kMaxGainDb, 0.01);
    CHECK (ad.getReductionDb() == 0.0f);
    float prev = heldGain, largestStep = 0.0f, largestLateStep = 0.0f;
    size_t firstMove = gains.size();
    for (size_t i = 0; i < gains.size(); ++i)
    {
        const float g = gains[i];
        CHECK (g >= prev - 1e-4f); // only ever up (float noise of the reading once settled)
        if (g > prev + 1e-4f && firstMove == gains.size())
            firstMove = i;
        largestStep = std::max (largestStep, g - prev);
        if (firstMove < gains.size() && i >= firstMove + 200)
            largestLateStep = std::max (largestLateStep, g - prev);
        prev = g;
    }
    REQUIRE (firstMove + 300 < gains.size());
    CHECK_LE (largestStep, 0.03f + 1e-5f);     // 3 dB/s (0.03 dB per 10 ms block) for 2 s
    CHECK_LE (largestLateStep, 0.01f + 1e-5f); // then 1 dB/s
    CHECK_NEAR (gains[firstMove + 199] - heldGain, 6.0, 0.05);
}

TEST_CASE ("GatedLoudness: the slow measure is corrected for its cold start, so AutoLevel leaves a source at its target alone and never moves the wrong way")
{
    // Reference: the settled reading of a -20 dBFS tone.
    GatedLoudness settled;
    settled.prepare (kFs, 2);
    double phase = 0.0;
    feedTone (20.0, dbToGain (-20.0f), phase, [&] (const AudioBlock& b) { settled.process (b); });
    const float ref = settled.getLufs();
    CHECK_NEAR (ref, -20.0, 0.2);

    // A fresh measure reads the same level from its first open block on (the
    // uncorrected one-pole read 8 dB low after 0.5 s and 3 dB low after 2 s).
    GatedLoudness fresh;
    fresh.prepare (kFs, 2);
    phase = 0.0;
    int block = 0;
    double worst = 0.0;
    feedTone (6.0, dbToGain (-20.0f), phase, [&] (const AudioBlock& b) {
        fresh.process (b);
        if (block++ > 0)
        {
            CHECK (fresh.isActive());
            worst = std::max (worst, static_cast<double> (std::abs (fresh.getLufs() - ref)));
        }
    });
    CHECK_LE (worst, 0.1);

    // AutoLevel from reset: a source at the target stays at 0 dB (it used to
    // climb to +2.4 dB), one 6 dB over it only ever goes down (it used to rise
    // to +0.5 dB first) and reaches -6 dB at 4 dB/s.
    for (float levelDb : { -18.0f, -12.0f })
    {
        AutoLevel al;
        al.prepare (kFs, 2);
        al.setEnabled (true);
        al.setTargetLufs (-18.0f);
        phase = 0.0;
        float highest = -100.0f, lowest = 100.0f;
        std::vector<float> gains;
        feedTone (8.0, dbToGain (levelDb), phase, [&] (const AudioBlock& b) {
            al.process (b);
            highest = std::max (highest, al.getGainDb());
            lowest = std::min (lowest, al.getGainDb());
            gains.push_back (al.getGainDb());
        });
        if (levelDb == -18.0f)
        {
            CHECK_LE (highest, 0.15f);
            CHECK_GE (lowest, -0.15f);
        }
        else
        {
            CHECK_LE (highest, 0.0f);
            CHECK_NEAR (gains[149], -6.0, 0.15); // after 1.5 s
            CHECK_NEAR (gains.back(), -6.0, 0.15);
        }
    }
}

TEST_CASE ("Chain: a dropped NaN/Inf block resets the signal path but keeps the converged governor, AutoLevel and AutoDrive state")
{
    // Hot, heavily clipped programme (Boost 100 %, clipper at its maximum
    // share) with AutoLevel on: the governor backs off and AutoLevel settles
    // on a cut. None of the control loops sees the non-finite block, so their
    // state must survive it (a full reset used to snap the scale back to 1
    // and the AutoLevel gain to 0 dB, i.e. seconds of louder, harder-driven
    // audio after a single NaN).
    ParameterStore store;
    store.set (Mode, static_cast<float> (ModeValue::Music));
    store.set (BoostIntensity, 1.0f);
    store.set (MaxClipAmount, 1.0f);
    store.set (MaxDriveDb, 10.0f);
    store.set (AutoLevelOn, 1.0f);
    store.set (AutoLevelTargetLufs, -24.0f);
    constexpr int kBlock = 512;
    ProcessingChain chain (store);
    chain.prepare ({ kFs, kBlock, 2 });
    auto hot = makeProgramme (static_cast<int> (kFs * 8.0), 0.5f, 3);
    runChain (chain, hot, kBlock);

    const auto& m = chain.meters();
    const float scaleBefore = m.governorScale.load(), gainBefore = m.autoLevelGainDb.load();
    REQUIRE (scaleBefore < 0.6f);
    REQUIRE (gainBefore < -3.0f);

    auto more = makeProgramme (kBlock * 4, 0.5f, 5);
    more.ch[0][static_cast<size_t> (kBlock + 17)] = std::numeric_limits<float>::quiet_NaN();
    runChain (chain, more, kBlock);
    for (auto& c : more.ch)
        for (int i = kBlock; i < 2 * kBlock; ++i)
            REQUIRE (c[static_cast<size_t> (i)] == 0.0f); // the dropped block is silence
    // Three processed blocks since: each loop moved at most its own slew.
    const double dt = 3.0 * kBlock / kFs;
    CHECK_LE (std::abs (m.governorScale.load() - scaleBefore), 0.15 * dt + 1e-5);
    CHECK_LE (std::abs (m.autoLevelGainDb.load() - gainBefore), 4.0 * dt + 1e-4);
    for (auto& c : more.ch)
        for (float v : c)
            REQUIRE (std::isfinite (v));
    CHECK (rms (more.ch[0].data() + 3 * kBlock, kBlock) > 1e-3); // audio resumed
}

TEST_CASE ("Chain: a maximizer, compressor, bass engine or dynamic EQ switched off mid-stream publishes no gain reduction and no clip energy (its last readings are not held)")
{
    ParameterStore store;
    bypassAllModules (store);
    store.set (MaximizerOn, 1.0f);
    store.set (MaxDriveDb, 20.0f);
    store.set (MaxClipAmount, 0.5f);
    store.set (CompressorOn, 1.0f);
    // A bass boost the headroom protection has to withdraw, and one dynamic
    // EQ band cutting hard at the kick.
    store.set (BassOn, 1.0f);
    store.set (BassBoostDb, 12.0f);
    store.set (BassProtectDb, -30.0f);
    store.set (DynEqOn, 1.0f);
    store.set (dyn (0, DynFieldOn), 1.0f);
    store.set (dyn (0, DynFieldFreq), 60.0f);
    store.set (dyn (0, DynFieldThreshold), -50.0f);
    constexpr int kBlock = 512;
    ProcessingChain chain (store);
    chain.prepare ({ kFs, kBlock, 2 });
    auto hot = makeProgramme (static_cast<int> (kFs * 2.0), 0.5f, 3);
    runChain (chain, hot, kBlock);
    const auto& m = chain.meters();
    REQUIRE (m.clipEnergyRatioDb.load() > -40.0f); // clipping hard...
    REQUIRE (m.maxGainReductionDb.load() < -0.5f); // ...and limiting
    REQUIRE (m.compGainReductionDb.load() < -0.5f);
    REQUIRE (m.bassProtectionDb.load() > 0.5f);
    REQUIRE (std::abs (m.dynEqGainDb[0].load()) > 0.5f);

    // Off (the slot fades out over 20 ms, then stops processing).
    for (int id : { MaximizerOn, CompressorOn, BassOn, DynEqOn })
        store.set (id, 0.0f);
    auto more = makeProgramme (static_cast<int> (kFs * 0.5), 0.5f, 4);
    runChain (chain, more, kBlock);
    CHECK (m.clipEnergyRatioDb.load() == kMinusInfDb);
    CHECK (m.maxGainReductionDb.load() == 0.0f);
    CHECK (m.glueGainReductionDb.load() == 0.0f);
    CHECK (m.compGainReductionDb.load() == 0.0f);
    CHECK (m.compUpwardGainDb.load() == 0.0f);
    CHECK (m.bassProtectionDb.load() == 0.0f);
    for (const auto& g : m.dynEqGainDb)
        CHECK (g.load() == 0.0f);
}

TEST_CASE ("Protection: the governor's limiter-GR input is taken per fixed 10 ms window, so the same programme and drive govern the same at 64- and 4096-sample blocks")
{
    // Maximizer alone, no clipper, Boost 0 (the scale changes no audio): the
    // output and the limiter's gain are the same at every block size. With a
    // per-block GR minimum the budget tripped by block size (min scale 1.0
    // at 64, 0.69 at 512, 0.3 at 4096 samples for this drive).
    const auto minScale = [] (int blockSize) {
        ParameterStore store;
        bypassAllModules (store);
        store.set (MaximizerOn, 1.0f);
        store.set (MaxClipAmount, 0.0f);
        store.set (MaxDriveDb, 14.0f);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, blockSize, 2 });
        auto buf = makeProgramme (static_cast<int> (kFs * 12.0), 0.5f, 3);
        float lowest = 1.0f;
        ScopedNoDenormals noDenormals;
        for (int pos = 0; pos < buf.numSamples(); pos += blockSize)
        {
            chain.process (buf.block (pos, std::min (blockSize, buf.numSamples() - pos)));
            lowest = std::min (lowest, chain.meters().governorScale.load());
        }
        return lowest;
    };
    const float s64 = minScale (64), s512 = minScale (512), s4096 = minScale (4096);
    CHECK_LE (s512, 0.9f); // over budget: the governor acts (measured 0.74-0.76)
    CHECK_GE (s512, 0.5f);
    CHECK_NEAR (s64, s512, 0.05);
    CHECK_NEAR (s4096, s512, 0.05);
}
