// Tests for the level-independent transient shaper: neutral exactness,
// attack / sustain shaping, level independence, stereo linking, smooth
// parameter changes, real-time safety, robustness and block-size invariance.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/dsp/TransientShaper.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace flub;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;

int ms (double milliseconds, double fs = kFs) { return static_cast<int> (fs * milliseconds * 0.001); }

/** White-noise bursts of burstMs every periodMs (silence in between). */
std::vector<float> gatedNoise (int n, float amplitude, double burstMs, double periodMs, uint32_t seed = 5, double fs = kFs)
{
    auto v = whiteNoise (n, amplitude, seed);
    const int burst = ms (burstMs, fs), period = ms (periodMs, fs);
    for (int i = 0; i < n; ++i)
        if (i % period >= burst)
            v[static_cast<size_t> (i)] = 0.0f;
    return v;
}

/** Sine bursts that start every periodMs and decay exponentially (tau ms). */
std::vector<float> decayingTone (int n, double freq, float amplitude, double tauMs, double periodMs, double fs = kFs)
{
    std::vector<float> v (static_cast<size_t> (n));
    const int period = ms (periodMs, fs);
    for (int i = 0; i < n; ++i)
    {
        const double t = static_cast<double> (i % period) / fs;
        v[static_cast<size_t> (i)] = static_cast<float> (amplitude * std::exp (-t / (tauMs * 0.001)) * std::sin (kTwoPi * freq * t));
    }
    return v;
}

void runShaper (TransientShaper& ts, Planar& buf, int blockSize)
{
    const int n = buf.numSamples();
    for (int pos = 0; pos < n; pos += blockSize)
        ts.process (buf.block (pos, std::min (blockSize, n - pos)));
}

/** Copies into the existing channel storage (assigning a new vector to
    Planar::ch[c] would leave Planar::ptrs dangling). */
void setChannel (Planar& p, int c, const std::vector<float>& v)
{
    auto& dst = p.ch[static_cast<size_t> (c)];
    std::copy_n (v.begin(), std::min (v.size(), dst.size()), dst.begin());
}

/** Deep copy whose channel pointers refer to its own storage (a plain copy
    of a Planar keeps pointing at the source's samples). */
Planar clone (const Planar& src)
{
    Planar p (src.numChannels(), src.numSamples());
    p.ch = src.ch;
    p.ptrs.clear();
    for (auto& c : p.ch)
        p.ptrs.push_back (c.data());
    return p;
}

Planar monoBuffer (const std::vector<float>& x)
{
    Planar p (1, static_cast<int> (x.size()));
    p.ch[0] = x;
    return clone (p);
}

double rmsDb (const std::vector<float>& x, int from, int to)
{
    return toDb (rms (x.data() + from, to - from));
}

/** Output / input RMS gain (dB) over [from, to). */
double gainDb (const std::vector<float>& out, const std::vector<float>& in, int from, int to)
{
    return rmsDb (out, from, to) - rmsDb (in, from, to);
}

bool allFinite (const Planar& p, double bound)
{
    for (const auto& c : p.ch)
        for (float v : c)
            if (! std::isfinite (v) || std::abs (v) > bound)
                return false;
    return true;
}
} // namespace

//==============================================================================
TEST_CASE ("TransientShaper: neutral settings are an exact pass-through")
{
    TransientShaper ts;
    ts.prepare (kFs);
    CHECK (ts.isNeutral());

    const int n = ms (500);
    Planar buf (2, n);
    setChannel (buf, 0, gatedNoise (n, 0.8f, 40.0, 150.0, 3));
    setChannel (buf, 1, decayingTone (n, 97.0, 0.9f, 60.0, 250.0));
    buf.ch[1][1000] = 1.0f;
    const Planar ref = clone (buf);
    runShaper (ts, buf, 64);
    CHECK (buf.ch == ref.ch);

    // After a detour through non-neutral settings the gain lands exactly on 1.
    ts.setAttackDb (9.0f);
    ts.setSustainDb (-6.0f);
    CHECK (! ts.isNeutral());
    Planar shaped = clone (ref);
    runShaper (ts, shaped, 64);
    CHECK (shaped.ch != ref.ch);
    ts.setAttackDb (0.0f);
    ts.setSustainDb (0.0f);
    CHECK (ts.isNeutral());
    Planar settle = clone (ref);
    runShaper (ts, settle, 64); // 500 ms: far longer than the 20 ms / 1 ms smoothing
    Planar again = clone (ref);
    runShaper (ts, again, 64);
    CHECK (again.ch == ref.ch);
    for (int i = 0; i < 1000; ++i)
        CHECK (ts.computeGain (0.5f * static_cast<float> (i % 7)) == 1.0f);
}

TEST_CASE ("TransientShaper: +12 dB attack lifts the onset of noise bursts relative to their sustained part")
{
    const int n = ms (2000);
    const auto in = gatedNoise (n, 0.25f, 200.0, 500.0);

    auto relativeOnsetDb = [&] (float attackDb)
    {
        TransientShaper ts;
        ts.prepare (kFs);
        ts.setAttackDb (attackDb);
        ts.reset();
        Planar buf = monoBuffer (in);
        runShaper (ts, buf, 128);
        double sum = 0.0;
        int bursts = 0;
        for (int start = ms (500); start + ms (200) <= n; start += ms (500), ++bursts)
        {
            const double onset = gainDb (buf.ch[0], in, start, start + ms (10));
            const double sustained = gainDb (buf.ch[0], in, start + ms (100), start + ms (200));
            CHECK_NEAR (sustained, 0.0, 1.0); // steady noise is (almost) untouched
            sum += onset - sustained;
        }
        return sum / bursts;
    };

    const double boosted = relativeOnsetDb (12.0f);
    const double softened = relativeOnsetDb (-12.0f);
    CHECK_GE (boosted, 6.0);
    CHECK_LE (softened, -6.0);
    CHECK_NEAR (relativeOnsetDb (0.0f), 0.0, 1.0e-9);
}

TEST_CASE ("TransientShaper: negative sustain shortens a decaying tone's tail, positive sustain lengthens it")
{
    const int n = ms (2000);
    const auto in = decayingTone (n, 1000.0, 0.5f, 150.0, 1000.0);

    auto process = [&] (float sustainDb)
    {
        TransientShaper ts;
        ts.prepare (kFs);
        ts.setSustainDb (sustainDb);
        ts.reset();
        Planar buf = monoBuffer (in);
        runShaper (ts, buf, 256);
        return buf.ch[0];
    };
    const auto dry = process (0.0f);
    const auto shortened = process (-12.0f);
    const auto lengthened = process (12.0f);
    CHECK (dry == in);

    for (int start : { 0, ms (1000) })
    {
        // The attack portion stays, the tail (250 .. 800 ms) loses >= 6 dB.
        CHECK_NEAR (gainDb (shortened, in, start, start + ms (20)), 0.0, 1.0);
        CHECK_LE (gainDb (shortened, in, start + ms (250), start + ms (800)), -6.0);
        CHECK_GE (gainDb (lengthened, in, start + ms (250), start + ms (800)), 6.0);

        // Time for the tone to fall 30 dB below its initial level (10 ms RMS windows).
        auto decayTime = [&] (const std::vector<float>& y)
        {
            const double ref = rmsDb (y, start, start + ms (10));
            for (int t = start; t + ms (10) < start + ms (1000); t += ms (5))
                if (rmsDb (y, t, t + ms (10)) < ref - 30.0)
                    return t - start;
            return ms (1000);
        };
        CHECK_LE (decayTime (shortened), decayTime (dry) * 3 / 4);
        CHECK_GE (decayTime (lengthened), decayTime (dry) * 5 / 4);
    }
}

TEST_CASE ("TransientShaper: the shaping is independent of the absolute level")
{
    // Gated noise + decaying tone bursts: onsets and decays drive both pairs.
    const int n = ms (1500);
    const auto noise = gatedNoise (n, 1.0f, 120.0, 300.0, 9);
    const auto tone = decayingTone (n, 180.0, 1.0f, 120.0, 400.0);
    std::vector<float> shape (static_cast<size_t> (n));
    for (size_t i = 0; i < shape.size(); ++i)
        shape[i] = 0.5f * noise[i] + 0.5f * tone[i];
    const double peak = peakAbs (shape.data(), n);

    auto gains = [&] (double levelDb)
    {
        TransientShaper ts;
        ts.prepare (kFs);
        ts.setAttackDb (12.0f);
        ts.setSustainDb (-12.0f);
        ts.reset();
        const float scale = static_cast<float> (std::pow (10.0, levelDb / 20.0) / peak);
        std::vector<double> g (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
            g[static_cast<size_t> (i)] = gainToDb (ts.computeGain (std::abs (scale * shape[static_cast<size_t> (i)])));
        return g;
    };
    const auto quiet = gains (-30.0);
    const auto loud = gains (-6.0);

    // Compare wherever the signal is within 30 dB of its peak (in the silent
    // gaps the gain multiplies nothing, and the -100 dBFS floor legitimately
    // makes very quiet material shape a little less).
    double maxDiff = 0.0, maxGain = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const size_t k = static_cast<size_t> (i);
        if (std::abs (shape[k]) < peak * 0.0316)
            continue;
        maxDiff = std::max (maxDiff, std::abs (quiet[k] - loud[k]));
        maxGain = std::max (maxGain, std::abs (loud[k]));
    }
    CHECK_LE (maxDiff, 0.5);
    CHECK_GE (maxGain, 6.0); // the settings really do shape this material
}

TEST_CASE ("TransientShaper: detection is linked - every channel gets the same gain")
{
    const int n = ms (800);
    Planar buf (3, n);
    setChannel (buf, 0, gatedNoise (n, 0.5f, 100.0, 250.0, 21)); // loud, drives the detector
    setChannel (buf, 1, whiteNoise (n, 0.01f, 22));              // quiet, continuous
    setChannel (buf, 2, decayingTone (n, 300.0, 0.02f, 50.0, 200.0));
    const Planar in = clone (buf);

    TransientShaper ts;
    ts.prepare (kFs);
    ts.setAttackDb (10.0f);
    ts.setSustainDb (-8.0f);
    ts.reset();
    runShaper (ts, buf, 100);

    // Reference gain from a second shaper fed the linked detector signal.
    TransientShaper ref;
    ref.prepare (kFs);
    ref.setAttackDb (10.0f);
    ref.setSustainDb (-8.0f);
    ref.reset();
    int mismatches = 0;
    bool anyShaping = false;
    for (int i = 0; i < n; ++i)
    {
        const size_t k = static_cast<size_t> (i);
        float linked = 0.0f;
        for (int c = 0; c < 3; ++c)
            linked = std::max (linked, std::abs (in.ch[static_cast<size_t> (c)][k]));
        const float g = ref.computeGain (linked);
        anyShaping = anyShaping || std::abs (g - 1.0f) > 0.1f;
        for (int c = 0; c < 3; ++c)
            mismatches += buf.ch[static_cast<size_t> (c)][k] != (g == 1.0f ? in.ch[static_cast<size_t> (c)][k] : in.ch[static_cast<size_t> (c)][k] * g) ? 1 : 0;
    }
    CHECK (mismatches == 0);
    CHECK (anyShaping);
}

TEST_CASE ("TransientShaper: parameter changes and onsets move the gain smoothly")
{
    TransientShaper ts;
    ts.prepare (kFs);
    ts.reset();
    const int n = ms (1200);
    const auto x = gatedNoise (n, 0.9f, 150.0, 300.0, 13);
    double prevDb = 0.0, maxStepDb = 0.0;
    for (int i = 0; i < n; ++i)
    {
        // Parameter jumps while the detector is in the middle of an onset / decay.
        if (i == ms (301))
            ts.setAttackDb (12.0f);
        if (i == ms (460))
            ts.setSustainDb (-12.0f);
        if (i == ms (605))
            ts.setAttackDb (-12.0f);
        if (i == ms (910))
            ts.setSustainDb (12.0f);
        const double gDb = gainToDb (ts.computeGain (std::abs (x[static_cast<size_t> (i)])));
        CHECK (std::isfinite (gDb));
        maxStepDb = std::max (maxStepDb, std::abs (gDb - prevDb));
        prevDb = gDb;
    }
    // 1 ms gain smoothing at 48 kHz: a full 24 dB swing moves <= 0.5 dB per sample.
    CHECK_LE (maxStepDb, 0.55);
    CHECK_GE (maxStepDb, 0.01);
}

TEST_CASE ("TransientShaper: zero latency - an impulse is not delayed")
{
    TransientShaper ts;
    ts.prepare (kFs);
    ts.setAttackDb (6.0f);
    ts.reset();
    Planar buf (2, 1024);
    buf.ch[0][100] = 0.01f;
    buf.ch[1][100] = -0.01f;
    runShaper (ts, buf, 64);
    int nonZero = 0;
    for (int i = 0; i < 1024; ++i)
        nonZero += buf.ch[0][static_cast<size_t> (i)] != 0.0f ? 1 : 0;
    CHECK (nonZero == 1);
    CHECK (buf.ch[0][100] != 0.0f);
    CHECK (buf.ch[1][100] == -buf.ch[0][100]);
}

TEST_CASE ("TransientShaper: process, reset and setters do not allocate")
{
    TransientShaper ts;
    ts.prepare (kFs);
    Planar buf (8, 512);
    for (int c = 0; c < 8; ++c)
        setChannel (buf, c, gatedNoise (512, 0.5f, 3.0, 7.0, static_cast<uint32_t> (c + 1)));

    flubtest::AllocationGuard guard;
    ts.reset();
    for (int i = 0; i < 40; ++i)
    {
        ts.setAttackDb (static_cast<float> (i % 25) - 12.0f);
        ts.setSustainDb (12.0f - static_cast<float> (i % 25));
        ts.process (buf.block (0, 1 + (i * 131) % 512).firstChannels (1 + i % 8));
        (void) ts.computeGain (0.3f);
        (void) ts.isNeutral();
    }
    CHECK (guard.allocations() == 0);
}

TEST_CASE ("TransientShaper: robustness - silence, DC, full-scale noise, impulses, extreme settings, all rates")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const int n = static_cast<int> (fs * 0.3);
        const float settings[][2] = { { 12.0f, 12.0f }, { -12.0f, -12.0f }, { 12.0f, -12.0f }, { 1.0e9f, -1.0e9f }, { inf, -inf } };
        for (const auto& s : settings)
        {
            for (int sig = 0; sig < 5; ++sig)
            {
                TransientShaper ts;
                ts.prepare (fs);
                ts.setAttackDb (s[0]);
                ts.setSustainDb (s[1]);
                ts.setAttackDb (nan); // ignored: keeps the last valid value
                ts.reset();
                Planar buf (2, n);
                for (int c = 0; c < 2; ++c)
                {
                    auto& x = buf.ch[static_cast<size_t> (c)];
                    switch (sig)
                    {
                        case 0: break;
                        case 1: std::fill (x.begin(), x.end(), c == 0 ? 1.0f : -1.0f); break;
                        case 2: setChannel (buf, c, whiteNoise (n, 1.0f, static_cast<uint32_t> (31 + c))); break;
                        case 3:
                            for (int i = 0; i < n; i += 1999)
                                x[static_cast<size_t> (i)] = 1.0f;
                            break;
                        default: setChannel (buf, c, gatedNoise (n, 1.0f, 2.0, 50.0, static_cast<uint32_t> (41 + c), fs)); break;
                    }
                }
                const Planar in = clone (buf);
                runShaper (ts, buf, 480);
                // Gain is bounded by +-24 dB (attack + sustain at their limits).
                CHECK (allFinite (buf, 15.9));
                bool bounded = true;
                for (size_t c = 0; c < 2; ++c)
                    for (int i = 0; i < n; ++i)
                        bounded = bounded && std::abs (buf.ch[c][static_cast<size_t> (i)]) <= 15.85f * std::abs (in.ch[c][static_cast<size_t> (i)]) + 1.0e-12f;
                CHECK (bounded);
                if (sig == 0)
                    CHECK (peakAbs (buf.ch[0].data(), n) == 0.0);
                // DC settles to a steady gain: no transient or sustain left after 200 ms.
                if (sig == 1)
                    CHECK_NEAR (buf.ch[0][static_cast<size_t> (n - 1)], 1.0, 1.0e-3);
            }
        }

        // A NaN detector sample in a steady signal changes nothing (it reads as
        // silence for one sample, which the peak hold bridges); +-inf and huge
        // values saturate the detector, stay finite and recover after reset().
        TransientShaper ts;
        ts.prepare (fs);
        ts.setAttackDb (12.0f);
        ts.setSustainDb (-12.0f);
        ts.reset();
        float g = 0.0f;
        for (int i = 0; i < n; ++i)
            g = ts.computeGain (i == n / 2 ? nan : 0.5f);
        // (float one-pole followers settle to within ~1e-4 of their input at 192 kHz)
        CHECK_NEAR (g, 1.0, 2.0e-3);
        for (float bad : { nan, inf, -inf, -1.0f, 1.0e30f })
            CHECK (std::isfinite (ts.computeGain (bad)));
        for (int i = 0; i < n; ++i)
            CHECK (std::isfinite (ts.computeGain (0.5f)));
        ts.reset();
        for (int i = 0; i < n; ++i)
            g = ts.computeGain (0.5f);
        CHECK_NEAR (g, 1.0, 2.0e-3);
    }
}

TEST_CASE ("TransientShaper: output is independent of the host block size")
{
    const int n = ms (700);
    Planar input (2, n);
    setChannel (input, 0, gatedNoise (n, 0.7f, 60.0, 170.0, 77));
    setChannel (input, 1, decayingTone (n, 220.0, 0.6f, 80.0, 230.0));

    auto run = [&] (int blockSize)
    {
        TransientShaper ts;
        ts.prepare (kFs);
        ts.setAttackDb (9.0f);
        ts.setSustainDb (-7.0f);
        ts.reset();
        Planar buf = clone (input);
        runShaper (ts, buf, blockSize);
        return buf;
    };

    const auto ref = run (512);
    for (int bs : { 1, 7, 64 })
    {
        const auto out = run (bs);
        double maxDiff = 0.0;
        for (size_t c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
                maxDiff = std::max (maxDiff, static_cast<double> (std::abs (out.ch[c][static_cast<size_t> (i)] - ref.ch[c][static_cast<size_t> (i)])));
        CHECK (maxDiff == 0.0); // per-sample algorithm: exactly identical
    }
}

// ---- adversarial review tests ----

TEST_CASE ("TransientShaper (review): steady low notes are not gain-modulated at any rate")
{
    // The peak hold must make a sustained bass note read as a constant level:
    // any residual ripple in the gain would put harmonics / sidebands on it.
    for (double fs : { 44100.0, 48000.0, 192000.0 })
    {
        for (double f : { 25.0, 40.0, 70.0 })
        {
            TransientShaper ts;
            ts.prepare (fs);
            ts.setAttackDb (12.0f);
            ts.setSustainDb (-12.0f);
            ts.reset();
            const int n = static_cast<int> (fs * 1.5);
            Planar buf = monoBuffer (sine (f, fs, n, 0.5f));
            runShaper (ts, buf, 256);
            const int from = static_cast<int> (fs * 0.5), len = static_cast<int> (fs);
            const double a1 = toneAmplitude (buf.ch[0].data() + from, len, f, fs);
            CHECK_NEAR (toDb (a1 / 0.5), 0.0, 0.01);
            for (int k = 2; k <= 4; ++k)
                CHECK_LE (toDb (toneAmplitude (buf.ch[0].data() + from, len, k * f, fs) / a1), -80.0);
        }
    }
}

TEST_CASE ("TransientShaper (review): gain is bounded and slews smoothly at every sample rate")
{
    const float inf = std::numeric_limits<float>::infinity();
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        TransientShaper ts;
        ts.prepare (fs);
        ts.reset();
        FastRandom rng (7);
        const int n = static_cast<int> (fs * 2.0);
        double prev = 0.0, maxStep = 0.0, maxAbs = 0.0;
        for (int i = 0; i < n; ++i)
        {
            if (i % 997 == 0)
            {
                ts.setAttackDb (rng.nextBipolar() > 0.0f ? 1.0e9f : -inf);
                ts.setSustainDb (12.0f * rng.nextBipolar());
            }
            // Clicks, bursts, silence and garbage on the detector input.
            const float x = (i % 4000 < 30) ? 1.0f : ((i / 3000) % 2 == 0 ? 0.3f * rng.nextBipolar() : 0.0f);
            const float g = ts.computeGain (i % 5003 == 0 ? inf : x);
            REQUIRE (std::isfinite (g) && g > 0.0f);
            const double db = gainToDb (g);
            maxAbs = std::max (maxAbs, std::abs (db));
            maxStep = std::max (maxStep, std::abs (db - prev));
            prev = db;
        }
        CHECK_LE (maxAbs, 24.0 + 1.0e-3);
        // 1 ms one-pole on a target inside +-24 dB: at most 48 dB * (1 - e^(-1 / (fs * 1 ms))) per sample.
        CHECK_LE (maxStep, 48.0 * (1.0 - std::exp (-1000.0 / fs)) + 1.0e-3);
    }
}

TEST_CASE ("TransientShaper (review): neutral is bit-exact again after garbage input at every rate")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (double fs : { 44100.0, 96000.0, 192000.0 })
    {
        TransientShaper ts;
        ts.prepare (fs);
        ts.setAttackDb (12.0f);
        ts.setSustainDb (-12.0f);
        for (float bad : { nan, std::numeric_limits<float>::infinity(), 1.0e30f, -5.0f })
            (void) ts.computeGain (bad);
        ts.setAttackDb (0.0f);
        ts.setSustainDb (0.0f);
        const int n = static_cast<int> (fs * 0.3);
        const auto x = gatedNoise (n, 0.9f, 20.0, 60.0, 3, fs);
        Planar settle = monoBuffer (x);
        runShaper (ts, settle, 64);
        Planar again = monoBuffer (x);
        runShaper (ts, again, 64);
        CHECK (again.ch[0] == x);
    }
}
