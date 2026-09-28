// Tests for the look-ahead compressor: static curve (downward soft knee and
// upward with floor taper, and the background-relative upward floor of
// docs/11 E19), steady-state gains, stereo linking, sidechain
// high-pass, look-ahead, parallel mix, auto makeup / release, latency,
// click-free parameter changes, real-time safety, robustness and block-size
// invariance.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/dsp/Compressor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace flub;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;

float dbfs (double db) { return static_cast<float> (std::pow (10.0, db / 20.0)); }

/** Copies into an existing channel. (Assigning a new vector to Planar::ch
    would reallocate it behind the block pointers Planar caches.) */
void setChannel (Planar& buf, int c, const std::vector<float>& v)
{
    auto& dst = buf.ch[static_cast<size_t> (c)];
    std::copy_n (v.begin(), std::min (v.size(), dst.size()), dst.begin());
}

void prepareComp (Compressor& comp, double fs = kFs, int channels = 2, int maxBlock = 512, float lookaheadMs = 2.0f)
{
    comp.setLookaheadMs (lookaheadMs);
    ProcessSpec spec;
    spec.sampleRate = fs;
    spec.maxBlockSize = maxBlock;
    spec.numChannels = channels;
    comp.prepare (spec);
}

/** Plain downward compressor, hard knee, no upward, no makeup. */
CompressorParams downParams (float thresholdDb, float ratio)
{
    CompressorParams p;
    p.thresholdDb = thresholdDb;
    p.ratio = ratio;
    p.kneeDb = 0.0f;
    p.attackMs = 5.0f;
    p.releaseMs = 100.0f;
    p.makeupDb = 0.0f;
    p.autoMakeup = false;
    p.autoRelease = false;
    p.sidechainHpHz = 80.0f;
    p.mix = 1.0f;
    p.upMaxGainDb = 0.0f;
    return p;
}

/** Steady sine on all channels from a fresh reset; returns the output gain of
    the tone (channel 0, dB) over `measure` seconds after `settle` seconds.
    Both spans are integer numbers of periods for every frequency used here. */
double toneGainDb (Compressor& comp, double freq, float amplitude, double settle = 0.4, double measure = 0.2,
                   int channels = 2, double fs = kFs)
{
    const int ns = static_cast<int> (std::lround (settle * fs));
    const int nm = static_cast<int> (std::lround (measure * fs));
    Planar buf (channels, ns + nm);
    const auto s = sine (freq, fs, ns + nm, amplitude);
    for (auto& c : buf.ch)
        std::copy (s.begin(), s.end(), c.begin());
    comp.reset();
    processInBlocks (comp, buf, 256);
    return toDb (toneAmplitude (buf.ch[0].data() + ns, nm, freq, fs) / amplitude);
}

bool allFinite (const Planar& buf)
{
    for (const auto& c : buf.ch)
        for (float v : c)
            if (! std::isfinite (v))
                return false;
    return true;
}

double peakOf (const Planar& buf)
{
    double p = 0.0;
    for (const auto& c : buf.ch)
        p = std::max (p, peakAbs (c.data(), static_cast<int> (c.size())));
    return p;
}

/** Largest change (dB) of the applied gain out[n] / in[n - latency] between
    consecutive measurable samples (|delayed input| >= minLevel). */
double maxGainStepDb (const std::vector<float>& in, const std::vector<float>& out, int latency, float minLevel, int from)
{
    double prev = 0.0, maxStep = 0.0;
    bool havePrev = false;
    for (int n = std::max (from, latency); n < static_cast<int> (out.size()); ++n)
    {
        const float x = in[static_cast<size_t> (n - latency)];
        if (std::abs (x) < minLevel)
            continue;
        const double g = toDb (std::abs (static_cast<double> (out[static_cast<size_t> (n)]) / x));
        if (havePrev)
            maxStep = std::max (maxStep, std::abs (g - prev));
        prev = g;
        havePrev = true;
    }
    return maxStep;
}
} // namespace

//==============================================================================
TEST_CASE ("Compressor: static curve is 0 below the knee, continuous inside it and has slope 1/R above it")
{
    CompressorParams p = downParams (-20.0f, 4.0f);
    p.kneeDb = 10.0f; // knee spans -25 .. -15 dB

    // Below the knee: exactly no gain change.
    CHECK_NEAR (Compressor::computeGainDb (p, -80.0f), 0.0, 1e-9);
    CHECK_NEAR (Compressor::computeGainDb (p, -40.0f), 0.0, 1e-9);
    CHECK_NEAR (Compressor::computeGainDb (p, -25.0f), 0.0, 1e-9);

    // Inside the knee: the quadratic (1/R - 1)(x - T + W/2)^2 / (2W).
    CHECK_NEAR (Compressor::computeGainDb (p, -20.0f), -0.75 * 25.0 / 20.0, 1e-5); // at T: -(1-1/R) W / 8
    CHECK_NEAR (Compressor::computeGainDb (p, -18.0f), -0.75 * 49.0 / 20.0, 1e-5);

    // Continuity of value and slope at both knee edges.
    const float eps = 1.0e-3f;
    for (float edge : { -25.0f, -15.0f })
    {
        const double below = Compressor::computeGainDb (p, edge - eps);
        const double at = Compressor::computeGainDb (p, edge);
        const double above = Compressor::computeGainDb (p, edge + eps);
        CHECK_NEAR (below, at, 1e-3);
        CHECK_NEAR (above, at, 1e-3);
        CHECK_NEAR ((above - at) / eps, (at - below) / eps, 0.02); // no kink
    }
    CHECK_NEAR (Compressor::computeGainDb (p, -15.0f), -0.75 * 5.0, 1e-5);

    // Fine sweep: monotonic non-increasing gain, never steeper than -(1 - 1/R),
    // so the curve has no jump anywhere.
    double prev = Compressor::computeGainDb (p, -40.0f);
    for (int i = 1; i <= 4000; ++i)
    {
        const float x = -40.0f + 0.01f * static_cast<float> (i);
        const double g = Compressor::computeGainDb (p, x);
        CHECK_LE (g, prev + 1e-6);
        CHECK_GE (g - prev, -0.75 * 0.01 - 1e-4);
        prev = g;
    }

    // Far above: output level y = x + g has slope 1/R.
    const double y0 = 0.0 + Compressor::computeGainDb (p, 0.0f);
    const double y1 = 10.0 + Compressor::computeGainDb (p, 10.0f);
    CHECK_NEAR ((y1 - y0) / 10.0, 0.25, 1e-5);
    CHECK_NEAR (Compressor::computeGainDb (p, 0.0f), -15.0, 1e-4); // T + (x - T)/R - x

    // Hard knee (W = 0) must not divide by zero.
    p.kneeDb = 0.0f;
    CHECK_NEAR (Compressor::computeGainDb (p, -20.0f), 0.0, 1e-9);
    CHECK_NEAR (Compressor::computeGainDb (p, -12.0f), -6.0, 1e-5);
    CHECK_NEAR (Compressor::computeGainDb (p, -20.5f), 0.0, 1e-9);

    // Ratio 1 is a straight wire.
    p.ratio = 1.0f;
    p.kneeDb = 6.0f;
    CHECK_NEAR (Compressor::computeGainDb (p, 0.0f), 0.0, 1e-9);
    CHECK_NEAR (Compressor::computeGainDb (p, -20.0f), 0.0, 1e-9);
}

TEST_CASE ("Compressor: upward curve lifts below its threshold, caps at upMax and tapers to 0 at the floor")
{
    CompressorParams p = downParams (-18.0f, 2.5f);
    p.upThresholdDb = -40.0f;
    p.upRatio = 2.0f;
    p.upMaxGainDb = 10.0f;
    p.upFloorDb = -75.0f;

    CHECK_NEAR (Compressor::computeGainDb (p, -30.0f), 0.0, 1e-9);  // above upThreshold
    CHECK_NEAR (Compressor::computeGainDb (p, -40.0f), 0.0, 1e-9);  // continuous at upThreshold
    CHECK_NEAR (Compressor::computeGainDb (p, -45.0f), 2.5, 1e-5);
    CHECK_NEAR (Compressor::computeGainDb (p, -50.0f), 5.0, 1e-5);
    CHECK_NEAR (Compressor::computeGainDb (p, -60.0f), 10.0, 1e-5); // capped (would be 10)
    CHECK_NEAR (Compressor::computeGainDb (p, -62.0f), 10.0, 1e-5); // capped (would be 11), taper still 1 (at -63)
    CHECK_NEAR (Compressor::computeGainDb (p, -69.0f), 10.0 * 6.0 / 12.0, 1e-4); // half-way down the taper
    CHECK_NEAR (Compressor::computeGainDb (p, -75.0f), 0.0, 1e-9);  // at the floor
    CHECK_NEAR (Compressor::computeGainDb (p, -120.0f), 0.0, 1e-9); // silence is never lifted

    // upMax = 0 switches the upward part off entirely.
    p.upMaxGainDb = 0.0f;
    CHECK_NEAR (Compressor::computeGainDb (p, -50.0f), 0.0, 1e-9);

    // Upward and downward parts add.
    p.upMaxGainDb = 10.0f;
    p.upThresholdDb = -10.0f;
    p.thresholdDb = -30.0f;
    p.ratio = 2.0f;
    // x = -20: gDown = -(1/2)(10) = -5, gUp = (10)(1/2) = 5 -> 0.
    CHECK_NEAR (Compressor::computeGainDb (p, -20.0f), 0.0, 1e-5);
}

TEST_CASE ("Compressor: static curve sanitises out-of-range and non-finite input")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    CompressorParams p;
    p.ratio = 0.0f;        // -> 1
    p.kneeDb = -5.0f;      // -> 0
    p.thresholdDb = nan;   // -> default
    CHECK (std::isfinite (Compressor::computeGainDb (p, 0.0f)));
    CHECK_NEAR (Compressor::computeGainDb (p, 0.0f), 0.0, 1e-9); // ratio 1

    CompressorParams q;
    q.ratio = 1000.0f;     // -> 20
    q.thresholdDb = -500.0f; // -> -60
    q.upMaxGainDb = 100.0f;  // -> 18
    q.upThresholdDb = inf;   // -> default
    for (float x : { -inf, -1000.0f, -160.0f, -60.0f, 0.0f, 50.0f, 1000.0f, inf, nan })
        CHECK (std::isfinite (Compressor::computeGainDb (q, x)));
    CHECK_NEAR (Compressor::computeGainDb (q, 0.0f), -(60.0 * 0.95), 1e-3);

    // setParams keeps the last valid value for non-finite fields and clamps the rest.
    Compressor comp;
    prepareComp (comp);
    CompressorParams r = downParams (-24.0f, 3.0f);
    comp.setParams (r);
    r.thresholdDb = nan;
    r.ratio = 50.0f;
    r.sidechainHpHz = 5.0f;  // on, clamped to 20 Hz
    r.mix = 2.0f;
    r.attackMs = 0.0f;
    comp.setParams (r);
    CHECK_NEAR (comp.getParams().thresholdDb, -24.0, 1e-9);
    CHECK_NEAR (comp.getParams().ratio, 20.0, 1e-9);
    CHECK_NEAR (comp.getParams().sidechainHpHz, 20.0, 1e-9);
    CHECK_NEAR (comp.getParams().mix, 1.0, 1e-9);
    CHECK_NEAR (comp.getParams().attackMs, 0.1, 1e-6);
    r.sidechainHpHz = -3.0f; // off
    comp.setParams (r);
    CHECK_NEAR (comp.getParams().sidechainHpHz, 0.0, 1e-9);
}

//==============================================================================
TEST_CASE ("Compressor: 1 kHz tone at -8 dBFS, threshold -20, ratio 4, hard knee settles at -9 dB")
{
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        Compressor comp;
        comp.setParams (downParams (-20.0f, 4.0f));
        prepareComp (comp, fs);
        const double g = toneGainDb (comp, 1000.0, dbfs (-8.0), 0.3, 0.2, 2, fs);
        CHECK_NEAR (g, -9.0, 0.3);
        CHECK_NEAR (comp.getGainReductionDb(), -9.0, 0.3);
        CHECK_NEAR (comp.getUpwardGainDb(), 0.0, 1e-9);
    }

    // A tone below the threshold passes untouched.
    Compressor comp;
    comp.setParams (downParams (-20.0f, 4.0f));
    prepareComp (comp);
    CHECK_NEAR (toneGainDb (comp, 1000.0, dbfs (-30.0)), 0.0, 0.01);
    CHECK_NEAR (comp.getGainReductionDb(), 0.0, 1e-9);
}

TEST_CASE ("Compressor: upward compression lifts a -50 dBFS tone by ~5 dB and leaves the floor alone")
{
    CompressorParams p = downParams (-18.0f, 2.5f);
    p.upThresholdDb = -40.0f;
    p.upRatio = 2.0f;
    p.upMaxGainDb = 10.0f;
    p.upFloorDb = -75.0f;

    Compressor comp;
    comp.setParams (p);
    prepareComp (comp);

    // The lift rises with the release time (100 ms), so allow ~1 s to settle.
    CHECK_NEAR (toneGainDb (comp, 1000.0, dbfs (-50.0), 1.0, 0.2), 5.0, 0.3);
    CHECK_NEAR (comp.getUpwardGainDb(), 5.0, 0.3);
    CHECK_NEAR (comp.getGainReductionDb(), 0.0, 1e-9);

    // Inside the floor taper: min(10, 15) * (-70 + 75) / 12.
    CHECK_NEAR (toneGainDb (comp, 1000.0, dbfs (-70.0), 1.0, 0.2), 10.0 * 5.0 / 12.0, 0.3);

    // Below the floor: no lift at all.
    CHECK_NEAR (toneGainDb (comp, 1000.0, dbfs (-90.0), 1.0, 0.2), 0.0, 0.05);
    CHECK_NEAR (comp.getUpwardGainDb(), 0.0, 0.05);

    // Loud material is not lifted either (the tone is above upThreshold).
    CHECK_NEAR (toneGainDb (comp, 1000.0, dbfs (-30.0), 0.5, 0.2), 0.0, 0.05);
}

TEST_CASE ("Compressor: relative upward floor - the static curve fades the lift from 3 to 9 dB over the background; with the background at the floor it only adds that fade (docs/11 E19)")
{
    CompressorParams p = downParams (-18.0f, 2.5f);
    p.upThresholdDb = -40.0f;
    p.upRatio = 2.0f;
    p.upMaxGainDb = 10.0f;
    p.upFloorDb = -75.0f;
    p.upRelativeFloor = true;

    // A -50 dB level: the fixed law gives (1 - 1/2) x 10 = 5 dB.
    CHECK_NEAR (Compressor::computeGainDb (p, -50.0f, -75.0f), 5.0, 1e-5); // background at the floor: 22 dB over it
    CHECK_NEAR (Compressor::computeGainDb (p, -50.0f, -59.0f), 5.0, 1e-5); // 9 dB over: full
    CHECK_NEAR (Compressor::computeGainDb (p, -50.0f, -56.0f), 2.5, 1e-5); // 6 dB over: half
    CHECK_NEAR (Compressor::computeGainDb (p, -50.0f, -53.0f), 0.0, 1e-9); // 3 dB over: none
    CHECK_NEAR (Compressor::computeGainDb (p, -50.0f, -50.0f), 0.0, 1e-9); // the background itself
    CHECK_NEAR (Compressor::computeGainDb (p, -50.0f, -45.0f), 0.0, 1e-9); // under it

    // Without a background the curve is taken out of silence (background at
    // the floor): the fixed curve times the relative fade near the floor.
    CHECK_NEAR (Compressor::computeGainDb (p, -50.0f), 5.0, 1e-5);
    CHECK_NEAR (Compressor::computeGainDb (p, -69.0f), 10.0 * (6.0 / 12.0) * (3.0 / 6.0), 1e-4);
    CHECK_NEAR (Compressor::computeGainDb (p, -50.0f, std::numeric_limits<float>::quiet_NaN()), 5.0, 1e-5);
    CHECK_NEAR (Compressor::computeGainDb (p, -50.0f, -120.0f), 5.0, 1e-5); // never under the floor

    // The downward part and a fixed floor ignore the background.
    CHECK_NEAR (Compressor::computeGainDb (p, -8.0f, -10.0f), Compressor::computeGainDb (p, -8.0f), 1e-6);
    p.upRelativeFloor = false;
    CHECK_NEAR (Compressor::computeGainDb (p, -50.0f, -50.0f), 5.0, 1e-5);
}

TEST_CASE ("Compressor: with the relative upward floor a steady bed is not lifted, a quiet sound 10 dB out of it is, the bed stays unlifted after it, and switching is click-free (docs/11 E19)")
{
    // 4 s of a -60 dBFS-RMS stereo pink bed with a -40 dBFS 1 kHz tone at
    // 2.5 - 3.0 s. Upward threshold -30 dB, ratio 2, max 10 dB, floor -75,
    // no downward section, release 100 ms. Gains are out vs the delayed
    // input, full-band power over both channels.
    CompressorParams p = downParams (-10.0f, 1.0f);
    p.upThresholdDb = -30.0f;
    p.upRatio = 2.0f;
    p.upMaxGainDb = 10.0f;
    p.upFloorDb = -75.0f;
    const int n = static_cast<int> (4.0 * kFs);
    Planar src (2, n);
    setChannel (src, 0, pinkNoise (n, dbfs (-60.0), 11));
    setChannel (src, 1, pinkNoise (n, dbfs (-60.0), 22));
    const auto tone = sine (1000.0, kFs, n, dbfs (-40.0));
    for (int i = static_cast<int> (2.5 * kFs); i < static_cast<int> (3.0 * kFs); ++i)
        for (auto& c : src.ch)
            c[static_cast<size_t> (i)] += tone[static_cast<size_t> (i)];

    struct Result
    {
        double bedDb, toneDb, afterDb;
        float backgroundDb;
    };
    const auto run = [&] (bool relative) {
        auto q = p;
        q.upRelativeFloor = relative;
        Compressor comp;
        comp.setParams (q);
        prepareComp (comp);
        const int lat = comp.latencySamples();
        Planar buf (2, n);
        for (int c = 0; c < 2; ++c)
            setChannel (buf, c, src.ch[static_cast<size_t> (c)]);
        processInBlocks (comp, buf, 256);
        const auto gain = [&] (double from, double to) {
            double po = 0.0, pi = 0.0;
            for (int c = 0; c < 2; ++c)
                for (int i = static_cast<int> (from * kFs); i < std::min (static_cast<int> (to * kFs), n - lat); ++i)
                {
                    const double o = buf.ch[static_cast<size_t> (c)][static_cast<size_t> (i + lat)];
                    const double x = src.ch[static_cast<size_t> (c)][static_cast<size_t> (i)];
                    po += o * o;
                    pi += x * x;
                }
            return 10.0 * std::log10 (po / pi);
        };
        // The background of the bed, read just before the tone (block end).
        Compressor probe;
        probe.setParams (q);
        prepareComp (probe);
        Planar head (2, static_cast<int> (2.5 * kFs));
        for (int c = 0; c < 2; ++c)
            setChannel (head, c, src.ch[static_cast<size_t> (c)]);
        processInBlocks (probe, head, 256);
        return Result { gain (1.5, 2.5), gain (2.8, 3.0), gain (3.5, 4.0), probe.getUpwardBackgroundDb() };
    };
    const auto fixed = run (false), rel = run (true);
    std::printf ("    measured fixed floor: bed %+.2f dB, tone %+.2f dB, bed after %+.2f dB\n", fixed.bedDb, fixed.toneDb, fixed.afterDb);
    std::printf ("    measured relative floor: bed %+.2f dB, tone %+.2f dB, bed after %+.2f dB, background %.2f dB\n", rel.bedDb, rel.toneDb,
                 rel.afterDb, static_cast<double> (rel.backgroundDb));
    CHECK_GE (fixed.bedDb, 6.0); // the fixed floor lifts the whole bed
    CHECK_LE (std::abs (rel.bedDb), 0.5);
    CHECK_LE (std::abs (rel.afterDb), 0.5);
    CHECK_GE (rel.toneDb, 3.5); // the law gives 5 dB; the lift rises at the 100 ms release
    CHECK_NEAR (rel.toneDb, fixed.toneDb, 1.0);
    // The background sits in the lower part of the bed's held-peak spread,
    // well above the floor.
    CHECK (rel.backgroundDb > -60.0f && rel.backgroundDb < -40.0f);

    // Switching the relative floor on and off under a steady quiet signal
    // (-50 dBFS DC + a -54 dBFS tone: never near zero, so the applied gain can
    // be read at every sample) glides: 10 dB of lift goes and comes back
    // without a step.
    auto q = p;
    q.attackMs = 0.1f; // fastest attack: an unsmoothed switch would show directly
    Compressor comp;
    comp.setParams (q);
    prepareComp (comp);
    const int lat = comp.latencySamples();
    const int seg = static_cast<int> (1.0 * kFs);
    Planar buf (1, 3 * seg);
    const auto t = sine (1000.0, kFs, 3 * seg, dbfs (-54.0));
    for (int i = 0; i < 3 * seg; ++i)
        buf.ch[0][static_cast<size_t> (i)] = dbfs (-50.0) + t[static_cast<size_t> (i)];
    const auto in = buf.ch[0];
    for (int s = 0; s < 3; ++s)
    {
        q.upRelativeFloor = s == 1;
        comp.setParams (q);
        for (int pos = s * seg; pos < (s + 1) * seg; pos += 480)
            comp.process (buf.block (pos, 480));
        if (s == 1)
            CHECK_LE (comp.getUpwardGainDb(), 0.05f); // the steady signal is its own background
    }
    CHECK_NEAR (comp.getUpwardGainDb(), 10.0, 0.3); // fixed floor again: the full law (capped)
    CHECK_LE (maxGainStepDb (in, buf.ch[0], lat, 0.001f, seg / 2), 0.1);
}

TEST_CASE ("Compressor: makeup is manual, or auto = half the reduction at 0 dBFS (knee included)")
{
    Compressor comp;
    CompressorParams p = downParams (-20.0f, 4.0f);
    p.autoMakeup = true; // gDown(0 dBFS) = -15 -> +7.5 dB
    comp.setParams (p);
    prepareComp (comp);
    CHECK_NEAR (toneGainDb (comp, 1000.0, dbfs (-8.0)), -9.0 + 7.5, 0.3);
    CHECK_NEAR (comp.getGainReductionDb(), -9.0, 0.3); // the meter excludes makeup

    // Soft knee that reaches 0 dBFS: T = -10, W = 24, R = 4 ->
    // gDown(0) = -0.75 (10 + 12)^2 / 48; a quiet tone only sees the makeup.
    p.thresholdDb = -10.0f;
    p.kneeDb = 24.0f;
    comp.setParams (p);
    comp.reset();
    const double expectedMakeup = 0.5 * 0.75 * 22.0 * 22.0 / 48.0;
    CHECK_NEAR (toneGainDb (comp, 1000.0, dbfs (-40.0)), expectedMakeup, 0.02);

    // Manual makeup.
    p.autoMakeup = false;
    p.makeupDb = 6.0f;
    comp.setParams (p);
    comp.reset();
    CHECK_NEAR (toneGainDb (comp, 1000.0, dbfs (-40.0)), 6.0, 0.02);

    // Auto makeup is capped to the makeup range (+24 dB).
    p.autoMakeup = true;
    p.thresholdDb = -60.0f;
    p.ratio = 20.0f;
    p.kneeDb = 0.0f;
    comp.setParams (p);
    comp.reset();
    CHECK_NEAR (toneGainDb (comp, 1000.0, dbfs (-70.0)), 24.0, 0.05);
}

TEST_CASE ("Compressor: linked detection gives both channels identical gain when only one is loud")
{
    Compressor comp;
    comp.setParams (downParams (-30.0f, 4.0f));
    prepareComp (comp);
    const int lat = comp.latencySamples();

    const int n = 24000;
    Planar buf (2, n);
    setChannel (buf, 0, sine (1000.0, kFs, n, dbfs (-6.0)));
    setChannel (buf, 1, sine (440.0, kFs, n, dbfs (-40.0)));
    const Planar in = buf;
    processInBlocks (comp, buf, 128);

    int compared = 0;
    double worst = 0.0, minGainDb = 0.0;
    for (int i = n / 4; i < n; ++i)
    {
        const float x0 = in.ch[0][static_cast<size_t> (i - lat)];
        const float x1 = in.ch[1][static_cast<size_t> (i - lat)];
        if (std::abs (x0) < 0.05f || std::abs (x1) < 0.002f)
            continue;
        const double g0 = buf.ch[0][static_cast<size_t> (i)] / static_cast<double> (x0);
        const double g1 = buf.ch[1][static_cast<size_t> (i)] / static_cast<double> (x1);
        worst = std::max (worst, std::abs (g0 - g1) / g0);
        minGainDb = std::min (minGainDb, toDb (g0));
        ++compared;
    }
    CHECK (compared > 1000);
    CHECK_LE (worst, 1e-5);
    CHECK_LE (minGainDb, -15.0); // the loud channel really is compressed (~ -18 dB)

    // The quiet channel on its own is not compressed: its gain above came from the loud one.
    Compressor alone;
    alone.setParams (downParams (-30.0f, 4.0f));
    prepareComp (alone);
    CHECK_NEAR (toneGainDb (alone, 440.0, dbfs (-40.0)), 0.0, 0.01);
}

TEST_CASE ("Compressor: sidechain high-pass stops a loud 30 Hz tone from causing much reduction")
{
    CompressorParams p = downParams (-30.0f, 4.0f);
    p.sidechainHpHz = 80.0f;
    Compressor comp;
    comp.setParams (p);
    prepareComp (comp);

    const double g1k = toneGainDb (comp, 1000.0, dbfs (-6.0));
    const double gr1k = comp.getGainReductionDb();
    const double g30 = toneGainDb (comp, 30.0, dbfs (-6.0), 0.4, 0.2);
    const double gr30 = comp.getGainReductionDb();
    CHECK_NEAR (g1k, -18.0, 0.3);
    CHECK_GE (g30, g1k + 10.0); // 30 Hz reads ~17 dB lower in the sidechain
    CHECK_GE (gr30, gr1k + 10.0);

    // Without the high-pass, equal levels are compressed equally.
    p.sidechainHpHz = 0.0f;
    comp.setParams (p);
    comp.reset();
    CHECK_NEAR (toneGainDb (comp, 30.0, dbfs (-6.0), 0.4, 0.2), -18.0, 0.5);
    CHECK_NEAR (toneGainDb (comp, 1000.0, dbfs (-6.0)), -18.0, 0.3);
}

TEST_CASE ("Compressor: look-ahead attenuates an isolated burst from its very first sample")
{
    CompressorParams p = downParams (-30.0f, 20.0f);
    p.attackMs = 1.0f;
    p.releaseMs = 100.0f;

    auto firstSampleGainDb = [&] (float lookaheadMs, double& outputPeak)
    {
        Compressor comp;
        comp.setParams (p);
        prepareComp (comp, kFs, 2, 512, lookaheadMs);
        const int lat = comp.latencySamples();
        const int onset = 4800;
        const int n = onset + 4800 + lat;
        Planar buf (2, n);
        for (int i = onset; i < onset + 4800; ++i)
        {
            // Cosine burst at 0 dBFS: the very first sample is full scale.
            const float v = static_cast<float> (std::cos (kTwoPi * 1000.0 * (i - onset) / kFs));
            buf.ch[0][static_cast<size_t> (i)] = v;
            buf.ch[1][static_cast<size_t> (i)] = v;
        }
        processInBlocks (comp, buf, 64);
        outputPeak = peakOf (buf);
        CHECK_NEAR (buf.ch[0][static_cast<size_t> (onset + lat - 1)], 0.0, 1e-12); // nothing before it
        return toDb (std::abs (buf.ch[0][static_cast<size_t> (onset + lat)]));
    };

    double peakWith = 0.0, peakWithout = 0.0;
    // Target: (0 + 30)(1/20 - 1) = -28.5 dB; 5 ms look-ahead = 5 attack time constants.
    const double withLookahead = firstSampleGainDb (5.0f, peakWith);
    const double without = firstSampleGainDb (0.0f, peakWithout);
    CHECK_LE (withLookahead, -26.0);
    CHECK_LE (peakWith, dbfs (-25.0)); // no overshoot anywhere in the burst
    CHECK_GE (without, -1.0);          // without look-ahead the onset passes almost untouched
    CHECK_GE (peakWithout, dbfs (-1.0));
}

TEST_CASE ("Compressor: look-ahead withdraws the upward lift before a loud transient arrives")
{
    // Night mode: a quiet passage is lifted by +5 dB, then a gunshot-like
    // burst arrives. The lift must be gone (attack) by the time the burst is
    // heard, otherwise the loudest moment would be boosted.
    CompressorParams p = downParams (-18.0f, 2.5f);
    p.attackMs = 1.0f;
    p.upThresholdDb = -40.0f;
    p.upRatio = 2.0f;
    p.upMaxGainDb = 10.0f;

    Compressor comp;
    comp.setParams (p);
    prepareComp (comp, kFs, 2, 512, 3.0f);
    const int lat = comp.latencySamples();

    const int onset = 48000; // 1 s of quiet tone: the lift settles at +5 dB
    const int n = onset + 4800;
    Planar buf (2, n);
    const auto quiet = sine (1000.0, kFs, onset, dbfs (-50.0));
    for (auto& c : buf.ch)
    {
        std::copy (quiet.begin(), quiet.end(), c.begin());
        for (int i = onset; i < n; ++i)
            c[static_cast<size_t> (i)] = dbfs (-6.0) * static_cast<float> (std::cos (kTwoPi * 1000.0 * (i - onset) / kFs));
    }
    processInBlocks (comp, buf, 128);

    // Until the sidechain sees the burst (output sample `onset`, i.e. `lat`
    // samples before the burst is heard) the quiet tone is still lifted ...
    CHECK_NEAR (toDb (toneAmplitude (buf.ch[0].data() + onset - 2400, 2400, 1000.0, kFs) / dbfs (-50.0)), 5.0, 0.3);
    // ... and the burst's first sample is already compressed, not boosted:
    // target (-6 + 18)(1/2.5 - 1) = -7.2 dB, 3 ms of look-ahead = 3 attack constants.
    const double first = toDb (std::abs (buf.ch[0][static_cast<size_t> (onset + lat)]) / dbfs (-6.0));
    CHECK_LE (first, -5.0);
}

TEST_CASE ("Compressor: mix 0 is exactly the input delayed by the latency; mix 0.5 blends aligned paths")
{
    CompressorParams p = downParams (-40.0f, 20.0f);
    p.attackMs = 0.1f;
    p.makeupDb = 12.0f;
    p.upMaxGainDb = 18.0f;
    p.mix = 0.0f;

    Compressor comp;
    comp.setParams (p);
    prepareComp (comp, kFs, 2, 512, 3.0f);
    const int lat = comp.latencySamples();
    CHECK (lat == 144);

    const int n = 20000;
    Planar buf (2, n);
    setChannel (buf, 0, whiteNoise (n, 0.9f, 7));
    setChannel (buf, 1, sine (1234.0, kFs, n, 0.8f));
    const Planar in = buf;
    processInBlocks (comp, buf, 100);

    double worst = 0.0;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i)
        {
            const float expected = i >= lat ? in.ch[static_cast<size_t> (c)][static_cast<size_t> (i - lat)] : 0.0f;
            worst = std::max (worst, static_cast<double> (std::abs (buf.ch[static_cast<size_t> (c)][static_cast<size_t> (i)] - expected)));
        }
    CHECK_LE (worst, 1e-6);
    CHECK_LE (comp.getGainReductionDb(), -10.0); // the wet path was working all along

    // mix 0.5 with -9 dB of reduction: 20 log10(0.5 + 0.5 * 10^(-9/20)).
    Compressor par;
    CompressorParams q = downParams (-20.0f, 4.0f);
    q.mix = 0.5f;
    par.setParams (q);
    prepareComp (par);
    const double expected = toDb (0.5 + 0.5 * std::pow (10.0, -9.0 / 20.0));
    CHECK_NEAR (toneGainDb (par, 1000.0, dbfs (-8.0)), expected, 0.2);
}

TEST_CASE ("Compressor: auto release recovers faster after a short burst than after a long one")
{
    // Returns the time (ms) from the end of the burst until the block's
    // gain reduction is back above -3 dB.
    auto recoveryMs = [] (bool autoRelease, double burstMs)
    {
        CompressorParams p = downParams (-30.0f, 10.0f);
        p.attackMs = 1.0f;
        p.releaseMs = 200.0f;
        p.autoRelease = autoRelease;
        Compressor comp;
        comp.setParams (p);
        prepareComp (comp);

        const int blockSize = 48; // 1 ms
        const int burst = static_cast<int> (burstMs * kFs / 1000.0);
        const int n = burst + static_cast<int> (kFs * 2.0);
        Planar buf (2, n);
        const auto tone = sine (1000.0, kFs, burst, dbfs (-6.0));
        for (auto& c : buf.ch)
            std::copy (tone.begin(), tone.end(), c.begin());

        double deepest = 0.0;
        for (int pos = 0; pos < n; pos += blockSize)
        {
            comp.process (buf.block (pos, std::min (blockSize, n - pos)));
            const double gr = comp.getGainReductionDb();
            deepest = std::min (deepest, gr);
            if (pos >= burst && gr > -3.0)
            {
                CHECK_LE (deepest, -20.0); // the burst was fully compressed (target -21.6 dB)
                return static_cast<double> (pos - burst) * 1000.0 / kFs;
            }
        }
        return 1.0e9;
    };

    const double shortAuto = recoveryMs (true, 20.0);
    const double longAuto = recoveryMs (true, 500.0);
    const double shortManual = recoveryMs (false, 20.0);
    const double longManual = recoveryMs (false, 500.0);

    CHECK_LE (shortAuto, 0.6 * longAuto);
    // Sustained reduction releases at the full releaseMs, like manual mode.
    CHECK_NEAR (longAuto, longManual, 0.05 * longManual);
    // Without auto release the burst length does not matter.
    CHECK_NEAR (shortManual, longManual, 0.05 * longManual);
    // The fast release is never faster than releaseMs / 4 (plus the hold).
    CHECK_GE (shortAuto, 0.25 * longManual - 5.0);
}

TEST_CASE ("Compressor: latencySamples() = round(lookahead * fs) and a quiet impulse arrives exactly that late")
{
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        for (float la : { 0.0f, 0.5f, 1.0f, 2.0f, 3.0f, 10.0f })
        {
            Compressor comp;
            prepareComp (comp, fs, 2, 512, la);
            const int lat = comp.latencySamples();
            CHECK (lat == static_cast<int> (std::lround (static_cast<double> (la) * fs / 1000.0)));

            const int n = lat + 600;
            Planar buf (2, n);
            const int at = 37;
            buf.ch[0][static_cast<size_t> (at)] = 0.001f;  // -60 dBFS: far below threshold
            buf.ch[1][static_cast<size_t> (at + 5)] = -0.002f;
            processInBlocks (comp, buf, 97);
            for (int i = 0; i < n; ++i)
            {
                const float e0 = i == at + lat ? 0.001f : 0.0f;
                const float e1 = i == at + 5 + lat ? -0.002f : 0.0f;
                CHECK (buf.ch[0][static_cast<size_t> (i)] == e0);
                CHECK (buf.ch[1][static_cast<size_t> (i)] == e1);
            }
        }
    }

    // A quiet tone is delayed without any other change.
    Compressor comp;
    prepareComp (comp, kFs, 1, 512, 2.0f);
    CHECK (comp.latencySamples() == 96);
    const int n = 4800;
    Planar buf (1, n);
    setChannel (buf, 0, sine (440.0, kFs, n, dbfs (-40.0)));
    const Planar in = buf;
    processInBlocks (comp, buf, 256);
    double worst = 0.0;
    for (int i = 96; i < n; ++i)
        worst = std::max (worst, static_cast<double> (std::abs (buf.ch[0][static_cast<size_t> (i)] - in.ch[0][static_cast<size_t> (i - 96)])));
    CHECK_LE (worst, 1e-9);

    // Structural: the look-ahead only changes on the next prepare(), and is limited to 10 ms.
    comp.setLookaheadMs (5.0f);
    CHECK (comp.latencySamples() == 96);
    comp.setLookaheadMs (50.0f);
    prepareComp (comp, kFs, 1, 512, 50.0f);
    CHECK (comp.latencySamples() == 480);
    comp.setLookaheadMs (std::numeric_limits<float>::quiet_NaN());
    prepareComp (comp, kFs, 1, 512, std::numeric_limits<float>::quiet_NaN());
    CHECK (comp.latencySamples() == 0);
}

TEST_CASE ("Compressor: parameter changes during a loud tone are click-free")
{
    CompressorParams p = downParams (-10.0f, 2.0f);
    p.attackMs = 0.1f; // fastest attack: any unsmoothed step would show directly
    p.releaseMs = 10.0f;
    p.kneeDb = 0.0f;

    Compressor comp;
    comp.setParams (p);
    prepareComp (comp);
    const int lat = comp.latencySamples();

    const int seg = 9600; // 200 ms per step
    std::vector<CompressorParams> steps;
    auto q = p;
    q.thresholdDb = -40.0f;
    q.ratio = 10.0f;
    steps.push_back (q); // 1: big threshold + ratio jump (~27 dB more reduction)
    q.kneeDb = 24.0f;
    q.makeupDb = 12.0f;
    steps.push_back (q); // 2: knee + makeup
    q.mix = 0.2f;
    steps.push_back (q); // 3: parallel
    q.autoMakeup = true;
    q.mix = 1.0f;
    steps.push_back (q); // 4: auto makeup on (makeup 12 -> 18 dB) and mix back to 1
    q.sidechainHpHz = 0.0f;
    steps.push_back (q); // 5: sidechain HP off (the DC part now counts: +7 dB of level)
    q.sidechainHpHz = 300.0f;
    steps.push_back (q); // 6: HP back on at another corner
    q.upMaxGainDb = 18.0f;
    q.upThresholdDb = -10.0f;
    q.autoRelease = true;
    steps.push_back (q); // 7: upward on, auto release
    q = p;
    steps.push_back (q); // 8: everything back

    // DC + tone never comes near zero, so the applied gain out[n] / in[n - L]
    // can be read at every single sample. The sidechain high-pass removes the
    // DC from the detector (peak 0.4) or not (peak 0.9), so toggling it is a
    // real change of the detected level.
    const int n = seg * static_cast<int> (steps.size() + 1);
    Planar buf (1, n);
    const auto tone = sine (1000.0, kFs, n, 0.4f);
    for (int i = 0; i < n; ++i)
        buf.ch[0][static_cast<size_t> (i)] = 0.5f + tone[static_cast<size_t> (i)];
    const auto in = buf.ch[0];

    for (int s = 0; s <= static_cast<int> (steps.size()); ++s)
    {
        if (s > 0)
            comp.setParams (steps[static_cast<size_t> (s - 1)]);
        for (int pos = s * seg; pos < (s + 1) * seg; pos += 480)
            comp.process (buf.block (pos, 480));
    }

    CHECK (allFinite (buf));
    // The applied gain never moves by more than a small fraction of a dB from
    // one sample to the next (an unsmoothed threshold jump with this 0.1 ms
    // attack would move it by ~5 dB in one sample).
    CHECK_LE (maxGainStepDb (in, buf.ch[0], lat, 0.05f, seg / 2), 0.1);
}

//==============================================================================
TEST_CASE ("Compressor: reset, setParams and process do not allocate")
{
    Compressor comp;
    prepareComp (comp, kFs, 8, 512, 3.0f);
    Planar buf (8, 512);
    for (auto& c : buf.ch)
        std::copy_n (whiteNoise (512, 0.8f, 99).begin(), 512, c.begin());

    flubtest::AllocationGuard guard;
    comp.reset();
    CompressorParams p = downParams (-30.0f, 6.0f);
    p.autoRelease = true;
    p.autoMakeup = true;
    p.upMaxGainDb = 12.0f;
    comp.setParams (p);
    for (int i = 0; i < 12; ++i)
    {
        p.thresholdDb -= 1.0f;
        p.sidechainHpHz = (i % 3 == 0) ? 0.0f : 60.0f + 20.0f * static_cast<float> (i);
        p.mix = (i % 2 == 0) ? 1.0f : 0.5f;
        comp.setParams (p);
        comp.process (buf.block (0, 512));
        comp.process (buf.block (0, 1));
        comp.process (buf.block (5, 77));
        comp.process (buf.block().firstChannels (3));
    }
    comp.reset();
    comp.process (buf.block());
    (void) comp.getGainReductionDb();
    (void) comp.getUpwardGainDb();
    (void) Compressor::computeGainDb (p, -12.0f);
    CHECK (guard.allocations() == 0);
}

TEST_CASE ("Compressor: silence, DC, full-scale noise, impulses and extreme settings stay finite and bounded")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();

    std::vector<CompressorParams> sets;
    sets.push_back (CompressorParams {});
    {
        CompressorParams p; // everything at the "hot" end
        p.thresholdDb = -60.0f;
        p.ratio = 20.0f;
        p.kneeDb = 24.0f;
        p.attackMs = 0.1f;
        p.releaseMs = 10.0f;
        p.autoRelease = true;
        p.makeupDb = 24.0f;
        p.sidechainHpHz = 300.0f;
        p.upThresholdDb = -10.0f;
        p.upRatio = 10.0f;
        p.upMaxGainDb = 18.0f;
        p.upFloorDb = -100.0f;
        sets.push_back (p);
    }
    {
        CompressorParams p; // the other end
        p.thresholdDb = 0.0f;
        p.ratio = 1.0f;
        p.kneeDb = 0.0f;
        p.attackMs = 200.0f;
        p.releaseMs = 2000.0f;
        p.autoMakeup = true;
        p.makeupDb = -12.0f;
        p.sidechainHpHz = 0.0f;
        p.mix = 0.0f;
        p.upThresholdDb = -80.0f;
        p.upRatio = 1.0f;
        p.upFloorDb = -40.0f;
        sets.push_back (p);
    }
    {
        CompressorParams p; // garbage in
        p.thresholdDb = 1.0e9f;
        p.ratio = -3.0f;
        p.kneeDb = nan;
        p.attackMs = -1.0f;
        p.releaseMs = 1.0e12f;
        p.makeupDb = 1000.0f;
        p.sidechainHpHz = 1.0e6f;
        p.mix = nan;
        p.upThresholdDb = -1.0e9f;
        p.upRatio = nan;
        p.upMaxGainDb = 1.0e4f;
        p.upFloorDb = 1.0e4f;
        sets.push_back (p);
    }
    {
        CompressorParams p = sets[1]; // hot, auto makeup, parallel
        p.autoMakeup = true;
        p.mix = 0.5f;
        sets.push_back (p);
    }

    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const int n = static_cast<int> (fs * 0.15);
        std::vector<std::vector<float>> signals;
        signals.emplace_back (static_cast<size_t> (n), 0.0f);                // silence
        signals.emplace_back (static_cast<size_t> (n), 1.0f);                // DC
        signals.emplace_back (static_cast<size_t> (n), -1.0f);               // negative DC
        signals.push_back (whiteNoise (n, 1.0f, 4242));                      // full-scale noise
        signals.emplace_back (static_cast<size_t> (n), 0.0f);                // impulses
        for (int i = 0; i < n; i += 2011)
            signals.back()[static_cast<size_t> (i)] = (i / 2011) % 2 == 0 ? 1.0f : -1.0f;
        signals.push_back (sine (20.0, fs, n, 1.0f));                        // full-scale sub
        signals.emplace_back (static_cast<size_t> (n), 1.0e-38f);            // subnormal-ish floor

        for (const auto& params : sets)
        {
            Compressor comp;
            comp.setParams (params);
            prepareComp (comp, fs, 2, 4096, 10.0f);
            for (const auto& sig : signals)
            {
                Planar buf (2, n);
                setChannel (buf, 0, sig);
                setChannel (buf, 1, sig);
                std::reverse (buf.ch[1].begin(), buf.ch[1].end());
                comp.reset();
                processInBlocks (comp, buf, 1000);
                CHECK (allFinite (buf));
                // Max possible gain: +18 dB upward + 24 dB makeup.
                CHECK_LE (peakOf (buf), 126.0);
                CHECK (std::isfinite (comp.getGainReductionDb()));
                CHECK (std::isfinite (comp.getUpwardGainDb()));
            }
        }
    }

    // Default settings never make a full-scale signal louder than its input.
    Compressor comp;
    prepareComp (comp);
    Planar buf (2, 48000);
    setChannel (buf, 0, whiteNoise (48000, 1.0f, 5));
    setChannel (buf, 1, whiteNoise (48000, 1.0f, 6));
    processInBlocks (comp, buf, 512);
    CHECK_LE (peakOf (buf), 1.0);
}

TEST_CASE ("Compressor: recovers from NaN / Inf input samples")
{
    Compressor comp;
    comp.setParams (downParams (-20.0f, 4.0f));
    prepareComp (comp);
    const int lat = comp.latencySamples();
    const int n = 96000;
    Planar buf (2, n);
    setChannel (buf, 0, sine (1000.0, kFs, n, 0.5f));
    setChannel (buf, 1, sine (700.0, kFs, n, 0.5f));
    buf.ch[0][1000] = std::numeric_limits<float>::quiet_NaN();
    buf.ch[1][3000] = std::numeric_limits<float>::infinity();
    buf.ch[0][3001] = -std::numeric_limits<float>::infinity();
    processInBlocks (comp, buf, 256);
    int nonFinite = 0;
    for (int c = 0; c < 2; ++c)
        for (int i = 3001 + lat + 1; i < n; ++i)
            nonFinite += std::isfinite (buf.ch[static_cast<size_t> (c)][static_cast<size_t> (i)]) ? 0 : 1;
    CHECK (nonFinite == 0);
    CHECK (std::isfinite (comp.getGainReductionDb()));
    // The Inf reads as a +100 dBFS peak (deep reduction, then hold and release);
    // after that the -6 dBFS tone is compressed normally again: (-6 + 20)(1/4 - 1).
    CHECK_NEAR (toDb (toneAmplitude (buf.ch[0].data() + n - 4800, 4800, 1000.0, kFs) / 0.5), -10.5, 0.1);
}

TEST_CASE ("Compressor: output is independent of the host block size (1, 7, 64, 512)")
{
    CompressorParams p = downParams (-28.0f, 5.0f);
    p.kneeDb = 8.0f;
    p.attackMs = 2.0f;
    p.releaseMs = 80.0f;
    p.autoRelease = true;
    p.autoMakeup = true;
    p.sidechainHpHz = 120.0f;
    p.mix = 0.8f;
    p.upThresholdDb = -45.0f;
    p.upRatio = 3.0f;
    p.upMaxGainDb = 9.0f;
    p.upFloorDb = -80.0f;

    auto q = p; // pushed after prepare: every smoother is gliding from the start
    q.thresholdDb = -22.0f;
    q.ratio = 3.0f;
    q.sidechainHpHz = 0.0f;
    q.mix = 0.6f;
    q.upMaxGainDb = 12.0f;

    const int n = 30000;
    Planar src (2, n);
    const auto noise = whiteNoise (n, 1.0f, 321);
    const auto tone = sine (220.0, kFs, n, 1.0f);
    for (int i = 0; i < n; ++i)
    {
        // Loud / quiet / very quiet sections, so downward, release and upward all run.
        const int section = (i / 3000) % 4;
        const float level = section == 0 ? 0.9f : section == 1 ? 0.004f : section == 2 ? 0.3f : 0.0005f;
        src.ch[0][static_cast<size_t> (i)] = level * (0.6f * tone[static_cast<size_t> (i)] + 0.4f * noise[static_cast<size_t> (i)]);
        src.ch[1][static_cast<size_t> (i)] = level * noise[static_cast<size_t> (n - 1 - i)];
    }

    // The relative upward floor (docs/11 E19) too, gliding on after prepare:
    // its background tracker steps on the global sample phase.
    auto run = [&] (int blockSize, bool relative)
    {
        Compressor comp;
        comp.setParams (p);
        prepareComp (comp, kFs, 2, 512, 1.5f);
        auto r = q;
        r.upRelativeFloor = relative;
        comp.setParams (r);
        Planar buf (2, n); // (a copied Planar would still point at src's channels)
        for (int c = 0; c < 2; ++c)
            setChannel (buf, c, src.ch[static_cast<size_t> (c)]);
        processInBlocks (comp, buf, blockSize);
        return buf.ch;
    };

    for (bool relative : { false, true })
    {
        const auto ref = run (1, relative);
        const int lat = static_cast<int> (std::lround (1.5 * kFs / 1000.0));
        double changed = 0.0; // the gain really moves (not just a delayed copy)
        for (int i = lat; i < n; ++i)
            changed = std::max (changed, static_cast<double> (std::abs (ref[0][static_cast<size_t> (i)] - src.ch[0][static_cast<size_t> (i - lat)])));
        CHECK_GE (changed, 0.05);

        for (int bs : { 7, 64, 512 })
        {
            const auto out = run (bs, relative);
            double worst = 0.0;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i)
                    worst = std::max (worst, static_cast<double> (std::abs (out[static_cast<size_t> (c)][static_cast<size_t> (i)]
                                                                            - ref[static_cast<size_t> (c)][static_cast<size_t> (i)])));
            CHECK_LE (worst, 1e-6);
        }
    }
}

TEST_CASE ("Compressor: 1..8 channels, all linked, and blocks narrower than the prepared width")
{
    for (int channels = 1; channels <= kMaxChannels; ++channels)
    {
        Compressor comp;
        comp.setParams (downParams (-30.0f, 4.0f));
        prepareComp (comp, kFs, channels);
        const int lat = comp.latencySamples();
        const int n = 9600;
        Planar buf (channels, n);
        const int loud = channels - 1; // only the last channel is loud
        for (int c = 0; c < channels; ++c)
            setChannel (buf, c, sine (500.0 + 100.0 * c, kFs, n, c == loud ? dbfs (-6.0) : dbfs (-50.0)));
        const Planar in = buf;
        processInBlocks (comp, buf, 333);
        CHECK (allFinite (buf));

        // Every channel gets the loud channel's gain (~ -18 dB).
        for (int c = 0; c < channels; ++c)
        {
            const auto& x = in.ch[static_cast<size_t> (c)];
            const auto& y = buf.ch[static_cast<size_t> (c)];
            const int from = n / 2;
            const double g = toneAmplitude (y.data() + from, n - from, 500.0 + 100.0 * c, kFs)
                             / toneAmplitude (x.data() + from - lat, n - from, 500.0 + 100.0 * c, kFs);
            CHECK_NEAR (toDb (g), -18.0, 0.3);
        }
    }

    // A stereo-prepared compressor fed a mono block processes just that channel.
    Compressor comp;
    comp.setParams (downParams (-20.0f, 4.0f));
    prepareComp (comp, kFs, 2);
    const int n = 19200;
    Planar buf (2, n);
    setChannel (buf, 0, sine (1000.0, kFs, n, dbfs (-8.0)));
    setChannel (buf, 1, sine (1000.0, kFs, n, dbfs (-8.0)));
    const std::vector<float> original = buf.ch[1];
    for (int pos = 0; pos < n; pos += 480)
        comp.process (buf.block (pos, 480).firstChannels (1));
    CHECK (allFinite (buf));
    CHECK (buf.ch[1] == original);
    CHECK_NEAR (toDb (toneAmplitude (buf.ch[0].data() + n - 4800, 4800, 1000.0, kFs) / dbfs (-8.0)), -9.0, 0.3);
}

// ---- adversarial review tests ----
namespace
{
/** A 1 kHz tone whose level steps through `segments` (seconds, dBFS). */
std::vector<float> steppedTone (double fs, const std::vector<std::pair<double, double>>& segments)
{
    std::vector<float> out;
    int start = 0;
    for (const auto& [seconds, db] : segments)
    {
        const int len = static_cast<int> (std::lround (seconds * fs));
        const float amp = db <= -200.0 ? 0.0f : dbfs (db);
        for (int i = 0; i < len; ++i)
            out.push_back (amp * static_cast<float> (std::sin (kTwoPi * 1000.0 * (start + i) / fs)));
        start += len;
    }
    return out;
}

/** Runs `comp` over a mono signal in blocks of `blockSize`, returning the
    smoothed gain the meter reports after every block (block size 1 gives the
    gain of every sample). */
std::vector<float> gainTrace (Compressor& comp, std::vector<float> signal, int blockSize)
{
    std::vector<float> trace;
    float* ptr = signal.data();
    const int n = static_cast<int> (signal.size());
    for (int pos = 0; pos < n; pos += blockSize)
    {
        comp.process (AudioBlock (&ptr, 1, std::min (blockSize, n - pos), pos));
        trace.push_back (std::min (comp.getGainReductionDb(), 0.0f) + comp.getUpwardGainDb());
    }
    return trace;
}

/** First index >= from where the trace crosses `level` in the given direction. */
double crossingIndex (const std::vector<float>& trace, int from, float level, bool falling)
{
    for (int i = std::max (1, from); i < static_cast<int> (trace.size()); ++i)
    {
        const float a = trace[static_cast<size_t> (i - 1)], b = trace[static_cast<size_t> (i)];
        if (falling ? (a > level && b <= level) : (a < level && b >= level))
            return (i - 1) + static_cast<double> ((level - a) / (b - a)); // linear interpolation
    }
    return -1.0;
}
} // namespace

TEST_CASE ("Compressor [adversarial]: the gain lands on the static curve even with the slowest times at 192 kHz")
{
    // A float one-pole y = t + c (y - t) stalls once c (y - t) rounds back to
    // y - t: with c = 1 - 2.6e-6 (2000 ms at 192 kHz) and |t| ~ 40 dB that is
    // up to ~0.7 dB short of the target, indefinitely.
    const double fs = 192000.0;
    CompressorParams p = downParams (-60.0f, 20.0f);
    p.sidechainHpHz = 0.0f;
    p.attackMs = 200.0f;
    p.releaseMs = 2000.0f;
    Compressor comp;
    comp.setParams (p);
    prepareComp (comp, fs, 1, 4096, 0.0f);

    const double expected = Compressor::computeGainDb (p, -18.0f); // -39.9 dB

    // Attack side: -30 dBFS (-28.5 dB), then -18 dBFS; 3 s = 15 attack time constants.
    auto trace = gainTrace (comp, steppedTone (fs, { { 3.0, -30.0 }, { 3.0, -18.0 } }), 4096);
    CHECK_NEAR (trace.back(), expected, 0.01);

    // Release side: -6 dBFS (-51.3 dB), then back to -18 dBFS for 8 release time constants.
    trace = gainTrace (comp, steppedTone (fs, { { 1.0, -6.0 }, { 16.0, -18.0 } }), 4096);
    CHECK_NEAR (trace.back(), expected, 0.01);
}

TEST_CASE ("Compressor [adversarial]: after a parameter glide the output matches a compressor set up with the final values")
{
    // Parameter smoothers must actually land. A smoother that stalls a few
    // ulps short keeps the curve (and the sidechain corner) off target forever
    // and never lets the module go idle.
    for (double fs : { 48000.0, 192000.0 })
    {
        CompressorParams a = downParams (-20.0f, 3.0f);
        a.kneeDb = 4.0f;
        a.makeupDb = 3.0f;
        a.sidechainHpHz = 80.0f;
        a.upThresholdDb = -50.0f;
        a.upMaxGainDb = 6.0f;
        CompressorParams b = a;
        b.thresholdDb = -37.3f;
        b.ratio = 6.0f;
        b.kneeDb = 9.0f;
        b.makeupDb = 7.3f;
        b.mix = 0.7f;
        b.sidechainHpHz = 300.0f;
        b.upThresholdDb = -61.0f;
        b.upMaxGainDb = 9.0f;
        b.upFloorDb = -83.0f;

        const auto signal = steppedTone (fs, { { 3.0, -12.0 } });
        const int n = static_cast<int> (signal.size());

        Compressor glide, fresh;
        glide.setParams (a);
        prepareComp (glide, fs, 1);
        glide.setParams (b);
        fresh.setParams (b);
        prepareComp (fresh, fs, 1);

        Planar x (1, n), y (1, n);
        setChannel (x, 0, signal);
        setChannel (y, 0, signal);
        processInBlocks (glide, x, 512);
        processInBlocks (fresh, y, 512);

        double worst = 0.0, peak = 0.0;
        for (int i = n - n / 10; i < n; ++i)
        {
            worst = std::max (worst, static_cast<double> (std::abs (x.ch[0][static_cast<size_t> (i)] - y.ch[0][static_cast<size_t> (i)])));
            peak = std::max (peak, static_cast<double> (std::abs (y.ch[0][static_cast<size_t> (i)])));
        }
        CHECK (peak > 0.05);
        CHECK_LE (worst / peak, 1e-6);
    }
}

TEST_CASE ("Compressor [adversarial]: attack and release time constants are exact and sample-rate independent")
{
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        CompressorParams p = downParams (-40.0f, 20.0f);
        p.sidechainHpHz = 0.0f;
        p.attackMs = 5.0f;
        p.releaseMs = 100.0f;
        Compressor comp;
        comp.setParams (p);
        prepareComp (comp, fs, 1, 512, 0.0f);

        const float target = Compressor::computeGainDb (p, -6.0f); // -32.3 dB
        const auto trace = gainTrace (comp, steppedTone (fs, { { 0.5, -6.0 }, { 1.0, -300.0 } }), 1);

        // Attack: from -10 dB, one time constant covers (1 - 1/e) of the rest.
        const float a0 = -10.0f, a1 = target + (a0 - target) / static_cast<float> (std::exp (1.0));
        const double attackMs = (crossingIndex (trace, 0, a1, true) - crossingIndex (trace, 0, a0, true)) * 1000.0 / fs;
        CHECK_NEAR (attackMs, 5.0, 0.05);

        // Release (after the hold): from -20 dB to -20 / e dB is one time constant.
        const int end = static_cast<int> (0.5 * fs);
        const float r0 = -20.0f, r1 = r0 / static_cast<float> (std::exp (1.0));
        const double releaseMs = (crossingIndex (trace, end, r1, false) - crossingIndex (trace, end, r0, false)) * 1000.0 / fs;
        CHECK_NEAR (releaseMs, 100.0, 1.0);

        // The hold before the release starts is bounded: half a period of
        // 20 Hz to a full one (HP off), never less than the look-ahead.
        const double holdMs = (crossingIndex (trace, end, target + 0.5f, false) - end) * 1000.0 / fs;
        CHECK_GE (holdMs, 24.0);
        CHECK_LE (holdMs, 51.0);
    }
}

TEST_CASE ("Compressor [adversarial]: auto release after a 5 ms transient runs at ~releaseMs/4, whatever the hold phase")
{
    // The peak hold keeps the detected level up for 25..50 ms (HP off) after
    // the transient. That hold must not be counted as "persistence", or a
    // gunshot would release at up to ~0.5 releaseMs instead of ~0.25.
    auto recoverySamples = [] (bool autoRelease, float releaseMs, int offset)
    {
        CompressorParams p = downParams (-30.0f, 10.0f);
        p.sidechainHpHz = 0.0f;
        p.attackMs = 0.5f;
        p.releaseMs = releaseMs;
        p.autoRelease = autoRelease;
        Compressor comp;
        comp.setParams (p);
        prepareComp (comp, kFs, 1, 512, 0.0f);
        const auto trace = gainTrace (comp, steppedTone (kFs, { { offset / kFs, -300.0 }, { 0.005, -6.0 }, { 1.0, -300.0 } }), 1);
        const int end = offset + 240;
        CHECK_LE (*std::min_element (trace.begin(), trace.end()), -15.0f); // the transient was caught
        return crossingIndex (trace, end, -3.0f, false) - end;
    };

    for (int offset : { 1, 311, 600, 911, 1199 })
    {
        const double autoTime = recoverySamples (true, 200.0f, offset);
        const double quarterTime = recoverySamples (false, 50.0f, offset); // manual releaseMs / 4
        CHECK_GE (autoTime, quarterTime - 1.0);
        CHECK_LE (autoTime, 1.2 * quarterTime);
    }
}

TEST_CASE ("Compressor [adversarial]: a channel that rejoins after narrower blocks does not replay stale audio")
{
    Compressor comp;
    comp.setParams (downParams (-20.0f, 4.0f));
    prepareComp (comp, kFs, 2, 512, 3.0f);
    const int lat = comp.latencySamples();

    Planar buf (2, 3 * 480);
    setChannel (buf, 0, sine (1000.0, kFs, 3 * 480, 0.3f));
    setChannel (buf, 1, whiteNoise (480, 0.9f, 17)); // loud noise on channel 1, then silence
    comp.process (buf.block (0, 480));
    comp.process (buf.block (480, 480).firstChannels (1)); // channel 1 dropped
    comp.process (buf.block (960, 480));                   // ... and back, silent

    double stale = 0.0;
    for (int i = 960; i < 960 + lat; ++i)
        stale = std::max (stale, static_cast<double> (std::abs (buf.ch[1][static_cast<size_t> (i)])));
    CHECK_LE (stale, 0.0);
}

TEST_CASE ("Compressor [adversarial]: a NaN on one channel does not disturb the detector of the others")
{
    // Only the poisoned channel's sidechain filter is restarted: restarting a
    // healthy one mid-signal passes its full (unfiltered) level for a few ms.
    CompressorParams p = downParams (-20.0f, 4.0f);
    p.sidechainHpHz = 80.0f;
    Compressor comp;
    comp.setParams (p);
    prepareComp (comp, kFs, 2, 512, 2.0f);

    const int n = 48000;
    Planar buf (2, n);
    setChannel (buf, 0, sine (30.0, kFs, n, dbfs (-6.0))); // reads ~ -23 dB through the HP: no reduction
    setChannel (buf, 1, sine (1000.0, kFs, n, dbfs (-40.0)));
    buf.ch[1][24000 + 7] = std::numeric_limits<float>::quiet_NaN();

    std::vector<float> trace;
    for (int pos = 0; pos < n; pos += 16)
    {
        comp.process (buf.block (pos, 16));
        trace.push_back (comp.getGainReductionDb());
    }
    CHECK_GE (*std::min_element (trace.begin() + 1000, trace.end()), -0.5f);
}

TEST_CASE ("Compressor [adversarial]: the hold covers the full 10 ms look-ahead even with a 300 Hz sidechain corner")
{
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        CompressorParams p = downParams (-30.0f, 20.0f);
        p.sidechainHpHz = 300.0f; // half-period hold only 3.3 ms < look-ahead
        p.attackMs = 1.0f;
        p.releaseMs = 10.0f;
        Compressor comp;
        comp.setParams (p);
        prepareComp (comp, fs, 1, 512, 10.0f);
        const int lat = comp.latencySamples();

        const int n = lat + 2000;
        Planar buf (1, n);
        const int at = 700;
        for (int i = at; i < at + 8; ++i)
            buf.ch[0][static_cast<size_t> (i)] = (i % 2 == 0) ? 1.0f : -1.0f; // HF click, passes the HP
        processInBlocks (comp, buf, 64);
        // Every sample of the click is played with (almost) the full reduction.
        for (int i = at; i < at + 8; ++i)
            CHECK_LE (toDb (std::abs (buf.ch[0][static_cast<size_t> (i + lat)])), -24.0);
    }
}

TEST_CASE ("Compressor [adversarial]: random automation, block sizes and signals never produce NaN/Inf or out-of-range meters")
{
    FastRandom rng (0xC0FFEEu);
    auto uni = [&] (float lo, float hi) { return lo + (hi - lo) * 0.5f * (rng.nextBipolar() + 1.0f); };

    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        for (int channels : { 1, 2, 8 })
        {
            Compressor comp;
            prepareComp (comp, fs, channels, 4096, uni (0.0f, 10.0f));
            const int n = static_cast<int> (fs * 0.5);
            Planar buf (channels, n);
            for (int c = 0; c < channels; ++c)
            {
                auto noise = whiteNoise (n, 1.0f, static_cast<uint32_t> (11 + c));
                for (int i = 0; i < n; ++i)
                {
                    // Bursts of full-scale noise, quiet passages and silence.
                    const int section = (i / 5000 + c) % 4;
                    const float g = section == 0 ? 1.0f : section == 1 ? 1.0e-3f : section == 2 ? 0.0f : 0.05f;
                    buf.ch[static_cast<size_t> (c)][static_cast<size_t> (i)] = g * noise[static_cast<size_t> (i)];
                }
            }
            int pos = 0;
            while (pos < n)
            {
                CompressorParams p;
                p.thresholdDb = uni (-70.0f, 5.0f);
                p.ratio = uni (0.5f, 25.0f);
                p.kneeDb = uni (-2.0f, 30.0f);
                p.attackMs = uni (0.0f, 250.0f);
                p.releaseMs = uni (0.0f, 2500.0f);
                p.autoRelease = rng.nextBipolar() > 0.0f;
                p.makeupDb = uni (-15.0f, 30.0f);
                p.autoMakeup = rng.nextBipolar() > 0.3f;
                p.sidechainHpHz = rng.nextBipolar() > 0.0f ? uni (-10.0f, 400.0f) : 0.0f;
                p.mix = uni (-0.2f, 1.2f);
                p.upThresholdDb = uni (-90.0f, 0.0f);
                p.upRatio = uni (0.5f, 12.0f);
                p.upMaxGainDb = uni (-2.0f, 20.0f);
                p.upFloorDb = uni (-110.0f, -30.0f);
                p.upRelativeFloor = rng.nextBipolar() > 0.0f;
                comp.setParams (p);

                const int len = std::min (n - pos, 1 + static_cast<int> (rng.nextU32() % 700u));
                const int nch = 1 + static_cast<int> (rng.nextU32() % static_cast<uint32_t> (channels));
                comp.process (buf.block (pos, len).firstChannels (nch));
                CHECK_LE (comp.getGainReductionDb(), 0.0f);
                CHECK_GE (comp.getGainReductionDb(), -160.0f);
                CHECK_GE (comp.getUpwardGainDb(), 0.0f);
                CHECK_LE (comp.getUpwardGainDb(), 18.0f + 1.0e-3f);
                CHECK (std::isfinite (comp.getUpwardBackgroundDb()));
                CHECK_GE (comp.getUpwardBackgroundDb(), -110.0f); // never under the (smoothed) floor
                pos += len;
            }
            CHECK (allFinite (buf));
            CHECK_LE (peakOf (buf), 126.0); // +18 dB up + 24 dB makeup on a full-scale input
        }
    }
}

TEST_CASE ("Compressor [adversarial]: parameter changes at the same sample positions give identical output for any block split")
{
    const int n = 48000, seg = 480;
    Planar src (2, n);
    setChannel (src, 0, whiteNoise (n, 0.8f, 3));
    setChannel (src, 1, sine (95.0, kFs, n, 0.7f));

    auto run = [&] (uint32_t seed)
    {
        FastRandom rng (seed), prm (77); // prm: the same automation for every split
        Compressor comp;
        prepareComp (comp, kFs, 2, 512, 1.0f);
        Planar buf (2, n);
        for (int c = 0; c < 2; ++c)
            setChannel (buf, c, src.ch[static_cast<size_t> (c)]);
        for (int s = 0; s < n; s += seg)
        {
            CompressorParams p = downParams (-40.0f + 30.0f * std::abs (prm.nextBipolar()), 2.0f + 8.0f * std::abs (prm.nextBipolar()));
            p.autoRelease = prm.nextBipolar() > 0.0f;
            p.autoMakeup = prm.nextBipolar() > 0.0f;
            p.sidechainHpHz = prm.nextBipolar() > 0.2f ? 0.0f : 150.0f;
            p.mix = std::abs (prm.nextBipolar());
            p.upMaxGainDb = 10.0f * std::abs (prm.nextBipolar());
            comp.setParams (p);
            int pos = s;
            while (pos < s + seg)
            {
                const int len = seed == 0 ? seg : std::min (s + seg - pos, 1 + static_cast<int> (rng.nextU32() % 97u));
                comp.process (buf.block (pos, len));
                pos += len;
            }
        }
        return buf.ch;
    };

    const auto ref = run (0);
    for (uint32_t seed : { 1u, 2u, 3u })
    {
        const auto out = run (seed);
        CHECK (out == ref);
    }
}
