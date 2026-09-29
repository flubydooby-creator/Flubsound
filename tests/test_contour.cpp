// Relative loudness contour (docs/11 E32; flub/dsp/LoudnessContour.h): the
// ISO 226:2023 formula, the four-section design, and the stage in the chain
// driven by the listening level (contour.level + the host's endpoint offset,
// ProcessingChain::setListeningLevelDb). Every test prints its values
// ("    measured ...").
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/common/Denormals.h"
#include "flub/common/Math.h"
#include "flub/dsp/LoudnessContour.h"
#include "flub/engine/Parameters.h"
#include "flub/engine/ProcessingChain.h"

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
constexpr int kBlock = 256;

void measured (const std::string& name, double value, const char* unit)
{
    char buf[64];
    std::snprintf (buf, sizeof (buf), "%.2f", value);
    std::cout << "    measured " << name << " = " << buf << " " << unit << "\n";
}

int isoIndex (double hz)
{
    for (int i = 0; i < iso226::kNumFrequencies; ++i)
        if (iso226::kFrequencies[static_cast<size_t> (i)] == hz)
            return i;
    return -1;
}

void runChain (ProcessingChain& chain, Planar& buf, int from, int to)
{
    ScopedNoDenormals noDenormals;
    for (int pos = from; pos < to; pos += kBlock)
        chain.process (buf.block (pos, std::min (kBlock, to - pos)));
}

void bypassAllModules (ParameterStore& s)
{
    for (int id : { GateOn, EqOn, DynEqOn, BassOn, ClarityOn, SaturationOn, SpatialOn, VirtualizerOn, CompressorOn, MaximizerOn })
        s.set (id, 0.0f);
}

/** 50 Hz and 1 kHz at 0.03 each on both channels. */
Planar twoTone (int n)
{
    Planar p (2, n);
    const auto lo = sine (50.0, kFs, n, 0.03f), hi = sine (1000.0, kFs, n, 0.03f);
    for (auto& c : p.ch)
        for (size_t i = 0; i < c.size(); ++i)
            c[i] = lo[i] + hi[i];
    return p;
}
} // namespace

// ---------------------------------------------------------------------------
TEST_CASE ("Contour: ISO 226:2023 - 1 kHz is the loudness level, 2.4 phon is the threshold of hearing, and the formula matches an independent evaluation")
{
    const int k1 = isoIndex (1000.0);
    REQUIRE (k1 >= 0);
    for (double phon = 20.0; phon <= 100.0; phon += 10.0)
        CHECK_NEAR (iso226::splDb (k1, phon), phon, 1.0e-9);
    // At 2.4 phon the level term vanishes (10^(0.03 x 2.4) = 10^0.072): the
    // contour is the threshold of hearing T_f of Table 1.
    const double tf[] = { 78.1, 68.7, 59.5, 51.1, 44.0, 37.5, 31.5, 26.5, 22.1, 17.9, 14.4, 11.4, 8.6, 6.2, 4.4,
                          3.0,  2.2,  2.4,  3.5,  1.7,  -1.3, -4.2, -6.0, -5.4, -1.5, 6.0,  12.6, 13.9, 12.3 };
    for (int i = 0; i < iso226::kNumFrequencies; ++i)
        CHECK_NEAR (iso226::splDb (i, 2.4), tf[i], 1.0e-9);
    // Formula (1) evaluated independently (Python, double precision) with the
    // same Table 1 parameters, at 20 / 40 / 60 / 80 / 100 phon.
    struct Point
    {
        double hz, phon, spl;
    };
    const Point points[] = { { 20.0, 20.0, 89.5445 },     { 50.0, 40.0, 77.7687 },  { 100.0, 60.0, 78.5040 }, { 3150.0, 80.0, 77.1566 },
                             { 12500.0, 100.0, 102.6749 }, { 125.0, 40.0, 60.4395 }, { 8000.0, 40.0, 51.2275 }, { 50.0, 80.0, 101.8096 } };
    for (const auto& p : points)
    {
        const double got = iso226::splDb (isoIndex (p.hz), p.phon);
        measured ("ISO 226 " + std::to_string (static_cast<int> (p.hz)) + " Hz at " + std::to_string (static_cast<int> (p.phon)) + " phon", got, "dB SPL");
        CHECK_NEAR (got, p.spl, 0.001);
    }
    // The relative gain is 0 at 1 kHz and grows towards the bass as the level falls.
    CHECK_NEAR (iso226::relativeGainDb (k1, 80.0, -30.0), 0.0, 1.0e-9);
    const double g50 = iso226::relativeGainDb (isoIndex (50.0), 80.0, -30.0);
    measured ("G (50 Hz), 80 phon, -30 dB", g50, "dB");
    CHECK_NEAR (g50, 12.1039, 0.001);
    CHECK (iso226::relativeGainDb (isoIndex (50.0), 80.0, -40.0) > g50);
    CHECK (iso226::relativeGainDb (isoIndex (3150.0), 80.0, -30.0) < 0.0);
}

TEST_CASE ("Contour: the four sections fit G within 0.85 dB from 20 Hz to 12.5 kHz and within 0.5 dB at 50 Hz from 0 to -40 dB; the cap holds")
{
    LoudnessContour c;
    c.prepare ({ kFs, 512, 2 });
    double worst = 0.0, worst50 = 0.0;
    for (float level = 0.0f; level >= -40.0f; level -= 5.0f)
    {
        LoudnessContourParams p;
        p.enabled = true;
        p.levelDb = level;
        p.maxLiftDb = 24.0f;
        c.setParams (p);
        // A design waits for kDesignIntervalMs after the previous one: let it run.
        Planar idle (2, static_cast<int> (LoudnessContour::kDesignIntervalMs * 0.001 * kFs) + 1);
        c.process (idle.block());
        for (int i = 0; i < iso226::kNumFrequencies; ++i)
        {
            const double want = std::min (iso226::relativeGainDb (i, 80.0, level), 24.0);
            const double err = std::abs (c.targetLiftDb (iso226::kFrequencies[static_cast<size_t> (i)]) - want);
            worst = std::max (worst, err);
            if (iso226::kFrequencies[static_cast<size_t> (i)] == 50.0)
                worst50 = std::max (worst50, err);
        }
    }
    measured ("worst fit error 20 Hz .. 12.5 kHz", worst, "dB");
    measured ("worst fit error at 50 Hz", worst50, "dB");
    CHECK (worst <= 0.85);
    CHECK (worst50 <= 0.5);

    // The cap: no lift above maxLiftDb (+ the fit's error) at any level.
    LoudnessContourParams p;
    p.enabled = true;
    p.levelDb = -60.0f;
    p.maxLiftDb = 12.0f;
    LoudnessContour capped;
    capped.prepare ({ kFs, 512, 2 });
    capped.setParams (p);
    double maxLift = -100.0;
    for (double f = 20.0; f < 20000.0; f *= 1.05)
        maxLift = std::max (maxLift, capped.targetLiftDb (f));
    measured ("max lift, cap 12 dB, -60 dB", maxLift, "dB");
    CHECK (maxLift < 13.1); // the cap plus the fit's error
}

TEST_CASE ("Contour: in the chain the 50 Hz gain re 1 kHz tracks the listening level within 1 dB from 0 to -40 dB")
{
    const int settle = static_cast<int> (kFs * 0.6), window = static_cast<int> (kFs * 0.2);
    // Level of 50 Hz re 1 kHz at the output, contour on or off.
    const auto ratioDb = [&] (bool contourOn, float hostLevelDb) {
        ParameterStore store;
        bypassAllModules (store);
        store.set (ContourOn, contourOn ? 1.0f : 0.0f);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, kBlock, 2 });
        chain.setListeningLevelDb (hostLevelDb);
        Planar buf = twoTone (settle + window);
        runChain (chain, buf, 0, settle + window);
        const float* y = buf.ch[0].data() + settle;
        return toDb (toneAmplitude (y, window, 50.0, kFs)) - toDb (toneAmplitude (y, window, 1000.0, kFs));
    };
    const double off = ratioDb (false, 0.0f);
    double worst = 0.0;
    for (float level : { 0.0f, -10.0f, -20.0f, -30.0f, -40.0f })
    {
        const double lift = ratioDb (true, level) - off;
        const double want = iso226::relativeGainDb (isoIndex (50.0), 80.0, level);
        measured ("50 Hz re 1 kHz at " + std::to_string (static_cast<int> (level)) + " dB (ISO " + std::to_string (want).substr (0, 5) + ")", lift, "dB");
        worst = std::max (worst, std::abs (lift - want));
    }
    measured ("worst tracking error", worst, "dB");
    CHECK (worst <= 1.0);

    // contour.level (the plug-in's route) and the host's offset add up.
    ParameterStore store;
    bypassAllModules (store);
    store.set (ContourOn, 1.0f);
    store.set (ContourLevelDb, -10.0f);
    ProcessingChain chain (store);
    chain.prepare ({ kFs, kBlock, 2 });
    chain.setListeningLevelDb (-20.0f);
    Planar buf = twoTone (settle);
    runChain (chain, buf, 0, settle);
    measured ("lift at 50 Hz, contour.level -10 + host -20", chain.getContourLiftAt50HzDb(), "dB");
    CHECK_NEAR (chain.getLoudnessContour().targetLiftDb (50.0), iso226::relativeGainDb (isoIndex (50.0), 80.0, -30.0), 0.6);
}

TEST_CASE ("Contour: a volume ramp from 0 to -40 dB in 1 dB steps glides: no step, no click")
{
    ParameterStore store;
    bypassAllModules (store);
    store.set (ContourOn, 1.0f);
    ProcessingChain chain (store);
    chain.prepare ({ kFs, kBlock, 2 });
    const int n = static_cast<int> (kFs * 2.5);
    Planar buf (2, n);
    const auto tone = sine (50.0, kFs, n, 0.05f);
    for (auto& c : buf.ch)
        std::copy (tone.begin(), tone.end(), c.begin());
    // A user dragging the OS volume: -1 dB every 25 ms, as the host polls it.
    const int stepEvery = static_cast<int> (kFs * 0.025);
    ScopedNoDenormals noDenormals;
    for (int pos = 0; pos < n; pos += kBlock)
    {
        const int dbStep = std::min (40, std::max (0, (pos - static_cast<int> (kFs * 0.25)) / stepEvery));
        chain.setListeningLevelDb (-static_cast<float> (dbStep));
        chain.process (buf.block (pos, std::min (kBlock, n - pos)));
    }
    // Peak per 20 ms period of the 50 Hz output: consecutive periods may
    // differ by what the glide allows, never by a step.
    const int period = static_cast<int> (kFs / 50.0);
    double maxStepDb = 0.0, first = 0.0, last = 0.0;
    bool started = false;
    for (int p0 = period; p0 + period <= n; p0 += period)
    {
        const double peak = toDb (peakAbs (buf.ch[0].data() + p0, period));
        if (started)
            maxStepDb = std::max (maxStepDb, std::abs (peak - last));
        else
            first = peak;
        started = true;
        last = peak;
    }
    // Anything above 2 kHz in a 50 Hz tone is a click (a coefficient or
    // trim step): an 8th-order Butterworth high-pass (50 Hz at -256 dB).
    std::vector<float> y (buf.ch[0]);
    float* yp[] = { y.data() };
    for (int s = 0; s < 4; ++s)
    {
        SvfFilter hp;
        hp.set (FilterType::HighPass, 2000.0, butterworthQ (4, s), 0.0, kFs);
        hp.process (AudioBlock (yp, 1, n));
    }
    const double hfDb = toDb (peakAbs (y.data() + period, n - period)) - toDb (0.05);
    measured ("50 Hz output change over the ramp", last - first, "dB");
    measured ("largest change between 20 ms periods", maxStepDb, "dB");
    measured ("peak above 2 kHz re the tone", hfDb, "dB");
    CHECK (maxStepDb <= 0.5);
    // Updating the coefficients in 0.04 dB steps without interpolation read
    // -47 dB here; the interpolated glide's corners read about -77 dB (a
    // shelf re-designed at every sample reads -83 dB on the same ramp).
    CHECK (hfDb < -70.0);
    CHECK (last - first > 1.0); // the lift did follow (net of the trim)
}

TEST_CASE ("Contour: off by default and untouched; switched off it glides flat, then idles bit-exact")
{
    CHECK (layout()[static_cast<size_t> (ContourOn)].defaultValue == 0.0f);
    for (int id : { ContourOn, ContourReferencePhon, ContourLevelDb, ContourMaxLiftDb })
        CHECK (layout()[static_cast<size_t> (id)].sinceVersion == 4);

    ParameterStore store;
    bypassAllModules (store);
    ProcessingChain chain (store);
    chain.prepare ({ kFs, kBlock, 2 });
    const int n = static_cast<int> (kFs * 3.0);
    Planar buf (2, n);
    const auto noise = pinkNoise (n, 0.1f, 99);
    for (auto& c : buf.ch)
        std::copy (noise.begin(), noise.end(), c.begin());
    const Planar input = buf;
    const int lat = chain.getLatencySamples();
    // Off (default) with an endpoint offset: the chain is the identity.
    chain.setListeningLevelDb (-30.0f);
    runChain (chain, buf, 0, n / 3);
    double err = 0.0;
    for (int i = lat; i < n / 3; ++i)
        err = std::max (err, static_cast<double> (std::abs (buf.ch[0][static_cast<size_t> (i)] - input.ch[0][static_cast<size_t> (i - lat)])));
    CHECK (err == 0.0);
    CHECK (! chain.getLoudnessContour().isRunning());
    // On for a second, then off: flat again within the glide (18 dB at 60 dB/s < 0.5 s).
    store.set (ContourOn, 1.0f);
    runChain (chain, buf, n / 3, 2 * n / 3);
    CHECK (chain.getContourLiftAt50HzDb() > 10.0f);
    CHECK (chain.getContourTrimDb() < 0.0f);
    store.set (ContourOn, 0.0f);
    runChain (chain, buf, 2 * n / 3, n);
    CHECK (! chain.getLoudnessContour().isRunning());
    err = 0.0;
    for (int i = 2 * n / 3 + static_cast<int> (0.5 * kFs); i < n; ++i)
        err = std::max (err, static_cast<double> (std::abs (buf.ch[0][static_cast<size_t> (i)] - input.ch[0][static_cast<size_t> (i - lat)])));
    measured ("error after switching off", err, "");
    CHECK (err == 0.0);
}

TEST_CASE ("Contour: pink noise with the contour at -30 dB keeps the maximizer's limiter under 1 dB")
{
    // Music defaults (maximizer on, ceiling -1 dBTP), pink at -20 / -14 /
    // -10 dBFS RMS (the last one limited already without the contour), the
    // contour on at -30 dB: the lift's programme-weighted excess over the
    // allowance comes off the level, the rest goes to the LF-first limiter.
    // The contour never adds limiting.
    for (float rmsDb : { -20.0f, -14.0f, -10.0f })
    {
        double limiterGr[2] = {}, lfLimit[2] = {};
        for (int onIndex = 0; onIndex < 2; ++onIndex)
        {
            ParameterStore store;
            store.set (ContourOn, onIndex == 1 ? 1.0f : 0.0f);
            ProcessingChain chain (store);
            chain.prepare ({ kFs, kBlock, 2 });
            chain.setListeningLevelDb (-30.0f);
            const int n = static_cast<int> (kFs * 4.0);
            Planar buf (2, n);
            const auto l = pinkNoise (n, dbToGain (rmsDb), 11), r = pinkNoise (n, dbToGain (rmsDb), 12);
            std::copy (l.begin(), l.end(), buf.ch[0].begin());
            std::copy (r.begin(), r.end(), buf.ch[1].begin());
            ScopedNoDenormals noDenormals;
            double worst = 0.0;
            for (int pos = 0; pos < n; pos += kBlock)
            {
                chain.process (buf.block (pos, std::min (kBlock, n - pos)));
                if (pos > static_cast<int> (kFs))
                    worst = std::min (worst, static_cast<double> (chain.meters().maxGainReductionDb.load()));
            }
            limiterGr[onIndex] = worst;
            lfLimit[onIndex] = chain.effectiveValue (MaxLfLimit);
            if (onIndex == 1)
            {
                measured ("contour lift at 50 Hz", chain.getContourLiftAt50HzDb(), "dB");
                measured ("contour trim", chain.getContourTrimDb(), "dB");
            }
        }
        const std::string at = " (pink " + std::to_string (static_cast<int> (rmsDb)) + " dBFS)";
        measured ("limiter GR, contour off" + at, limiterGr[0], "dB");
        measured ("limiter GR, contour on at -30 dB" + at, limiterGr[1], "dB");
        measured ("max.lfLimit applied, contour on" + at, lfLimit[1], "");
        CHECK (limiterGr[1] > -1.0);
        CHECK (limiterGr[1] >= limiterGr[0] - 0.05);
        CHECK (lfLimit[0] == 0.0);
        CHECK (lfLimit[1] > 0.5);
    }
}

TEST_CASE ("Contour: the automatic preamp counts the contour's lift net of its trim; no allocation while the level moves")
{
    ParameterStore store;
    bypassAllModules (store);
    store.set (ContourOn, 1.0f);
    store.set (AutoPreampOn, 1.0f);
    store.set (AutoPreampAllowanceDb, 0.0f);
    ProcessingChain chain (store);
    chain.prepare ({ kFs, kBlock, 2 });
    chain.setListeningLevelDb (-30.0f);
    Planar buf (2, static_cast<int> (kFs * 0.5));
    runChain (chain, buf, 0, buf.numSamples());
    const auto& c = chain.getLoudnessContour();
    measured ("contour predicted lift (programme weighted)", c.getPredictedLiftDb(), "dB");
    measured ("contour trim", c.getTargetTrimDb(), "dB");
    measured ("chain predicted boost", chain.getPredictedBoostDb(), "dB");
    measured ("automatic preamp", chain.getAutoPreampDb(), "dB");
    CHECK (c.getPredictedLiftDb() > 10.0f);
    CHECK_NEAR (c.getTargetTrimDb(), 3.0f - c.getPredictedLiftDb(), 0.01);
    // What the contour leaves in is its allowance (3 dB): the preamp takes it.
    CHECK_NEAR (chain.getPredictedBoostDb(), 3.0, 0.1);
    CHECK_NEAR (chain.getAutoPreampDb(), -3.0, 0.1);

    // Moving the level re-designs on the audio thread without allocating.
    Planar more (2, kBlock);
    AllocationGuard guard;
    for (int i = 0; i < 200; ++i)
    {
        chain.setListeningLevelDb (-30.0f + 0.1f * static_cast<float> (i % 50));
        chain.process (more.block());
    }
    CHECK (guard.allocations() == 0);
}
