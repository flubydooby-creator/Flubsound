// Tests for the clarity enhancer: neutral exactness, transient shaping,
// de-mud, dynamic presence, air exciter (harmonics and freedom from
// aliasing at 44.1 kHz), click-free parameter changes, real-time safety,
// robustness and block-size invariance.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/dsp/ClarityEnhancer.h"
#include "flub/dsp/Fft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <string>
#include <vector>

using namespace flub;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;

float dbfs (double db) { return static_cast<float> (std::pow (10.0, db / 20.0)); }
int ms (double milliseconds, double fs = kFs) { return static_cast<int> (fs * milliseconds * 0.001); }

void prepareClarity (ClarityEnhancer& ce, double fs = kFs, int channels = 2, int maxBlock = 512)
{
    ProcessSpec spec;
    spec.sampleRate = fs;
    spec.maxBlockSize = maxBlock;
    spec.numChannels = channels;
    ce.prepare (spec);
}

ClarityParams allOn()
{
    ClarityParams p;
    p.attackDb = 6.0f;
    p.sustainDb = -4.0f;
    p.presence = 0.8f;
    p.presenceFrequency = 2500.0f;
    p.air = 0.7f;
    p.deMud = 0.6f;
    return p;
}

/** Copies into the existing channel storage (keeps Planar::ptrs valid). */
void setChannel (Planar& p, int c, const std::vector<float>& v)
{
    auto& dst = p.ch[static_cast<size_t> (c)];
    std::copy_n (v.begin(), std::min (v.size(), dst.size()), dst.begin());
}

/** Deep copy whose channel pointers refer to its own storage. */
Planar clone (const Planar& src)
{
    Planar p (src.numChannels(), src.numSamples());
    p.ch = src.ch;
    p.ptrs.clear();
    for (auto& c : p.ch)
        p.ptrs.push_back (c.data());
    return p;
}

/** Same signal on both channels, processed from a reset. */
Planar runStereo (ClarityEnhancer& ce, const std::vector<float>& x, int blockSize = 256)
{
    Planar buf (2, static_cast<int> (x.size()));
    setChannel (buf, 0, x);
    setChannel (buf, 1, x);
    ce.reset();
    processInBlocks (ce, buf, blockSize);
    return buf;
}

double levelDb (const std::vector<float>& y, int from, int len, double freq, double ref = 1.0, double fs = kFs)
{
    return toDb (toneAmplitude (y.data() + from, len, freq, fs) / ref);
}

std::vector<float> gatedNoise (int n, float amplitude, double burstMs, double periodMs, uint32_t seed = 5)
{
    auto v = whiteNoise (n, amplitude, seed);
    const int burst = ms (burstMs), period = ms (periodMs);
    for (int i = 0; i < n; ++i)
        if (i % period >= burst)
            v[static_cast<size_t> (i)] = 0.0f;
    return v;
}

bool allFinite (const Planar& p, double bound)
{
    for (const auto& c : p.ch)
        for (float v : c)
            if (! std::isfinite (v) || std::abs (v) > bound)
                return false;
    return true;
}

double maxAbsDiff (const Planar& a, const Planar& b)
{
    double m = 0.0;
    for (size_t c = 0; c < a.ch.size(); ++c)
        for (size_t i = 0; i < a.ch[c].size(); ++i)
            m = std::max (m, static_cast<double> (std::abs (a.ch[c][i] - b.ch[c][i])));
    return m;
}

/** Windowed magnitude spectrum (dB, 4-term Blackman-Harris: sidelobes < -92 dB). */
std::vector<double> spectrumDb (const float* x, int n)
{
    Fft fft;
    fft.prepare (n);
    std::vector<float> w (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        const double t = kTwoPi * i / n;
        const double win = 0.35875 - 0.48829 * std::cos (t) + 0.14128 * std::cos (2.0 * t) - 0.01168 * std::cos (3.0 * t);
        w[static_cast<size_t> (i)] = static_cast<float> (x[i] * win);
    }
    std::vector<Fft::Complex> bins (static_cast<size_t> (n / 2 + 1));
    fft.forwardReal (w.data(), bins.data());
    std::vector<double> db (bins.size());
    for (size_t k = 0; k < bins.size(); ++k)
        db[k] = toDb (std::abs (bins[k]));
    return db;
}
} // namespace

//==============================================================================
TEST_CASE ("Clarity: neutral parameters are an exact pass-through")
{
    ClarityEnhancer ce;
    prepareClarity (ce);
    CHECK (ce.getParams() == ClarityParams {});
    CHECK (ce.latencySamples() == 0);

    const int n = ms (600);
    Planar buf (2, n);
    setChannel (buf, 0, gatedNoise (n, 0.9f, 30.0, 110.0, 3));
    setChannel (buf, 1, sine (3200.0, kFs, n, 0.5f));
    buf.ch[1][777] = 1.0f;
    const Planar ref = clone (buf);
    processInBlocks (ce, buf, 128);
    CHECK (buf.ch == ref.ch);

    // Every stage switched on and off again: once everything has landed the
    // output is bit-exact again (the stages stop, the shaper returns 1).
    ce.setParams (allOn());
    Planar on = clone (ref);
    processInBlocks (ce, on, 128);
    CHECK (on.ch != ref.ch);
    ce.setParams (ClarityParams {});
    Planar settle = clone (ref);
    processInBlocks (ce, settle, 128); // longer than every release (150 ms) and fade
    Planar settle2 = clone (ref);
    processInBlocks (ce, settle2, 128);
    Planar again = clone (ref);
    processInBlocks (ce, again, 128);
    CHECK (again.ch == ref.ch);
}

TEST_CASE ("Clarity: transient attack lifts onsets (linked, full band)")
{
    const int n = ms (1500);
    const auto in = gatedNoise (n, 0.25f, 200.0, 500.0, 17);
    ClarityEnhancer ce;
    prepareClarity (ce);
    ClarityParams p;
    p.attackDb = 12.0f;
    ce.setParams (p);
    Planar buf (2, n);
    setChannel (buf, 0, in);
    for (int i = 0; i < n; ++i)
        buf.ch[1][static_cast<size_t> (i)] = 0.1f * in[static_cast<size_t> (i)];
    ce.reset();
    processInBlocks (ce, buf, 200);

    for (int start = ms (500); start + ms (200) <= n; start += ms (500))
    {
        const double onset = toDb (rms (buf.ch[0].data() + start, ms (10)) / rms (in.data() + start, ms (10)));
        const double sustained = toDb (rms (buf.ch[0].data() + start + ms (100), ms (100)) / rms (in.data() + start + ms (100), ms (100)));
        CHECK_GE (onset - sustained, 6.0);
    }
    // Linked: the quiet channel receives exactly the same gain.
    int mismatches = 0;
    for (int i = 0; i < n; ++i)
    {
        const size_t k = static_cast<size_t> (i);
        if (std::abs (in[k]) > 1.0e-3f)
            mismatches += std::abs (buf.ch[1][k] / (0.1f * in[k]) - buf.ch[0][k] / in[k]) > 1.0e-4f ? 1 : 0;
    }
    CHECK (mismatches == 0);
}

TEST_CASE ("Clarity: de-mud reduces a dominant 250 Hz tone and ignores balanced material")
{
    const int settle = ms (800), measure = ms (400);
    const float amp = dbfs (-12.0);
    auto measure250 = [&] (float deMud, const std::vector<float>& x, double ref)
    {
        ClarityEnhancer ce;
        prepareClarity (ce);
        ClarityParams p;
        p.deMud = deMud;
        ce.setParams (p);
        const auto out = runStereo (ce, x);
        return levelDb (out.ch[0], settle, measure, 250.0, ref);
    };

    const auto tone = sine (250.0, kFs, settle + measure, amp);
    CHECK_NEAR (measure250 (1.0f, tone, amp), -4.0, 0.5);
    CHECK_NEAR (measure250 (0.5f, tone, amp), -2.0, 0.5);
    CHECK_NEAR (measure250 (0.0f, tone, amp), 0.0, 1.0e-3);

    // A quiet 250 Hz component inside loud broadband material is not "mud".
    auto mix = whiteNoise (settle + measure, 0.3f, 99);
    const auto quietTone = sine (250.0, kFs, settle + measure, dbfs (-40.0));
    for (size_t i = 0; i < mix.size(); ++i)
        mix[i] += quietTone[i];
    ClarityEnhancer dry;
    prepareClarity (dry);
    const auto ref = runStereo (dry, mix);
    CHECK_NEAR (measure250 (1.0f, mix, 1.0) - levelDb (ref.ch[0], settle, measure, 250.0), 0.0, 0.3);
}

TEST_CASE ("Clarity: dynamic presence lifts quiet content and leaves loud content")
{
    const int settle = ms (800), measure = ms (400);
    auto presenceGainDb = [&] (float presence, float freqHz, double toneHz, double levelDb_)
    {
        ClarityEnhancer ce;
        prepareClarity (ce);
        ClarityParams p;
        p.presence = presence;
        p.presenceFrequency = freqHz;
        ce.setParams (p);
        const float amp = dbfs (levelDb_);
        const auto out = runStereo (ce, sine (toneHz, kFs, settle + measure, amp));
        return levelDb (out.ch[0], settle, measure, toneHz, amp);
    };

    CHECK_NEAR (presenceGainDb (1.0f, 3200.0f, 3200.0, -50.0), 6.0, 0.5);
    CHECK_LE (presenceGainDb (1.0f, 3200.0f, 3200.0, -6.0), 1.0);
    CHECK_NEAR (presenceGainDb (0.5f, 3200.0f, 3200.0, -50.0), 3.0, 0.5);
    // In between: a partial lift (inverse level).
    const double mid = presenceGainDb (1.0f, 3200.0f, 3200.0, -27.0);
    CHECK (mid > 1.0 && mid < 5.0);
    // Hiss-level content is not lifted (noise-floor taper).
    CHECK_NEAR (presenceGainDb (1.0f, 3200.0f, 3200.0, -95.0), 0.0, 0.3);
    // The bell follows presenceFrequency; far from it nothing happens.
    CHECK_NEAR (presenceGainDb (1.0f, 1000.0f, 1000.0, -50.0), 6.0, 0.5);
    CHECK_NEAR (presenceGainDb (1.0f, 6000.0f, 200.0, -50.0), 0.0, 0.1);
}

TEST_CASE ("Clarity: air creates 2nd and 3rd harmonics of upper-mid content")
{
    const int settle = ms (500), measure = ms (200);
    const float amp = dbfs (-12.0);
    ClarityEnhancer ce;
    prepareClarity (ce);
    ClarityParams p;
    p.air = 1.0f;
    ce.setParams (p);
    const auto out = runStereo (ce, sine (4000.0, kFs, settle + measure, amp));
    const double h2 = levelDb (out.ch[0], settle, measure, 8000.0, amp);
    const double h3 = levelDb (out.ch[0], settle, measure, 12000.0, amp);
    CHECK_GE (h2, -30.0);
    CHECK_GE (h3, -30.0);
    CHECK_LE (std::max (h2, h3), -6.0);
    CHECK_NEAR (levelDb (out.ch[0], settle, measure, 4000.0, amp), 0.0, 1.0);
    CHECK_LE (levelDb (out.ch[0], settle, measure, 16000.0, amp), -100.0); // order <= 3: no 4th harmonic

    // The harmonics track the input level linearly (envelope-normalised).
    const auto quiet = runStereo (ce, sine (4000.0, kFs, settle + measure, amp * 0.01f));
    CHECK_NEAR (levelDb (quiet.ch[0], settle, measure, 8000.0, amp * 0.01), h2, 0.5);

    // Air 0: nothing is generated.
    ce.setParams (ClarityParams {});
    const auto dry = runStereo (ce, sine (4000.0, kFs, settle + measure, amp));
    CHECK_LE (levelDb (dry.ch[0], settle, measure, 8000.0, amp), -120.0);
}

TEST_CASE ("Clarity: air produces no aliasing at 44.1 kHz")
{
    constexpr double fs = 44100.0;
    constexpr double f0 = 6500.0; // 3rd harmonic 19.5 kHz: just below Nyquist
    const int settle = ms (500, fs), n = 16384;
    ClarityEnhancer ce;
    prepareClarity (ce, fs, 1);
    ClarityParams p;
    p.air = 1.0f;
    ce.setParams (p);
    ce.reset();
    Planar buf (1, settle + n);
    setChannel (buf, 0, sine (f0, fs, settle + n, dbfs (-12.0)));
    processInBlocks (ce, buf, 441);

    const auto db = spectrumDb (buf.ch[0].data() + settle, n);
    const double binHz = fs / n;
    auto peakNear = [&] (double f)
    {
        const int k = static_cast<int> (std::lround (f / binHz));
        double m = -300.0;
        for (int j = k - 3; j <= k + 3; ++j)
            m = std::max (m, db[static_cast<size_t> (j)]);
        return m;
    };
    const double ref = peakNear (f0);
    CHECK_GE (peakNear (2.0 * f0) - ref, -40.0); // the wanted harmonics are there
    CHECK_GE (peakNear (3.0 * f0) - ref, -40.0);

    double worst = -300.0, worstHz = 0.0;
    for (size_t k = 4; k < db.size(); ++k)
    {
        const double f = static_cast<double> (k) * binHz;
        bool harmonic = false;
        for (int m = 1; m <= 3; ++m)
            harmonic = harmonic || std::abs (f - m * f0) < 12.0 * binHz;
        if (! harmonic && db[k] - ref > worst)
        {
            worst = db[k] - ref;
            worstHz = f;
        }
    }
    CHECK_LE (worst, -80.0);
    if (worst > -80.0)
        std::cerr << "    worst non-harmonic component " << worst << " dB at " << worstHz << " Hz\n";
}

TEST_CASE ("Clarity: parameter changes are click-free")
{
    // 300 Hz + 2 kHz: everything the enhancer generates from this stays below
    // ~8 kHz, so energy above 15 kHz can only come from a discontinuity.
    const int n = ms (3000);
    const auto a = sine (300.0, kFs, n, 0.25f);
    const auto b = sine (2000.0, kFs, n, 0.1f);
    std::vector<float> x (a.size());
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = a[i] + b[i];

    auto run = [&] (bool toggle)
    {
        ClarityEnhancer ce;
        prepareClarity (ce);
        auto p = allOn();
        ce.setParams (p);
        ce.reset();
        Planar buf (2, n);
        setChannel (buf, 0, x);
        setChannel (buf, 1, x);
        const int block = 240;
        for (int pos = 0, k = 0; pos < n; pos += block, ++k)
        {
            if (toggle && k % 25 == 12)
            {
                switch ((k / 25) % 6)
                {
                    case 0: p.presence = p.presence > 0.0f ? 0.0f : 1.0f; break;
                    case 1: p.presenceFrequency = p.presenceFrequency > 2000.0f ? 1000.0f : 6000.0f; break;
                    case 2: p.deMud = p.deMud > 0.0f ? 0.0f : 1.0f; break;
                    case 3: p.air = p.air > 0.0f ? 0.0f : 1.0f; break;
                    case 4: p.attackDb = p.attackDb > 0.0f ? -12.0f : 12.0f; break;
                    default: p.sustainDb = p.sustainDb < 0.0f ? 12.0f : -12.0f; break;
                }
                ce.setParams (p);
            }
            ce.process (buf.block (pos, std::min (block, n - pos)));
        }
        return buf;
    };
    auto hfPeak = [] (const Planar& y)
    {
        double peak = 0.0;
        const auto hp = SvfCoeffs::make (FilterType::HighPass, 15000.0, 0.7071, 0.0, kFs);
        for (const auto& c : y.ch)
        {
            SvfState s1, s2;
            for (size_t i = 0; i < c.size(); ++i)
            {
                const float v = svfTick (hp, s2, svfTick (hp, s1, c[i]));
                if (i >= static_cast<size_t> (ms (100)))
                    peak = std::max (peak, static_cast<double> (std::abs (v)));
            }
        }
        return peak;
    };
    const double steady = hfPeak (run (false));
    const double toggled = hfPeak (run (true));
    CHECK_LE (toggled, 1.0e-4); // -80 dBFS; program at about -9 dBFS
    CHECK_LE (toggled, std::max (4.0 * steady, 2.0e-5));
}

TEST_CASE ("Clarity: zero latency - an impulse is not delayed")
{
    ClarityEnhancer ce;
    prepareClarity (ce);
    CHECK (ce.latencySamples() == 0);
    ce.setParams (allOn());
    Planar buf (2, 2048);
    buf.ch[0][100] = 1.0e-3f;
    buf.ch[1][100] = 1.0e-3f;
    ce.reset();
    processInBlocks (ce, buf, 64);
    for (int i = 0; i < 100; ++i)
        CHECK (buf.ch[0][static_cast<size_t> (i)] == 0.0f);
    int peakAt = 0;
    for (int i = 1; i < 2048; ++i)
        if (std::abs (buf.ch[0][static_cast<size_t> (i)]) > std::abs (buf.ch[0][static_cast<size_t> (peakAt)]))
            peakAt = i;
    CHECK (peakAt == 100 + ce.latencySamples());
}

TEST_CASE ("Clarity: process, reset and setters do not allocate")
{
    ClarityEnhancer ce;
    prepareClarity (ce, kFs, 8, 512);
    Planar buf (8, 512);
    for (int c = 0; c < 8; ++c)
        setChannel (buf, c, whiteNoise (512, 0.5f, static_cast<uint32_t> (c + 3)));

    flubtest::AllocationGuard guard;
    ce.reset();
    for (int i = 0; i < 60; ++i)
    {
        ClarityParams p;
        p.attackDb = static_cast<float> (i % 25) - 12.0f;
        p.sustainDb = 12.0f - static_cast<float> (i % 25);
        p.presence = static_cast<float> (i % 3) * 0.5f;
        p.presenceFrequency = 1000.0f + static_cast<float> (i * 311 % 5000);
        p.air = static_cast<float> (i % 4) * 0.33f;
        p.deMud = static_cast<float> (i % 5) * 0.25f;
        ce.setParams (p);
        ce.process (buf.block (0, 1 + (i * 97) % 512).firstChannels (1 + i % 8));
        (void) ce.getParams();
    }
    CHECK (guard.allocations() == 0);
}

TEST_CASE ("Clarity: robustness - silence, DC, full-scale noise, impulses, extreme settings, all rates")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    ClarityParams maxi;
    maxi.attackDb = 12.0f;
    maxi.sustainDb = 12.0f;
    maxi.presence = 1.0f;
    maxi.presenceFrequency = 6000.0f;
    maxi.air = 1.0f;
    maxi.deMud = 1.0f;
    ClarityParams mini = maxi;
    mini.attackDb = -12.0f;
    mini.sustainDb = -12.0f;
    mini.presenceFrequency = 1000.0f;
    ClarityParams wild;
    wild.attackDb = 1.0e9f;
    wild.sustainDb = -inf;
    wild.presence = nan;
    wild.presenceFrequency = 1.0e9f;
    wild.air = -3.0f;
    wild.deMud = inf;
    const ClarityParams settings[] = { ClarityParams {}, maxi, mini, wild };

    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const int n = static_cast<int> (fs * 0.3);
        for (const auto& p : settings)
        {
            for (int channels : { 1, 2, 6 })
            {
                ClarityEnhancer ce;
                prepareClarity (ce, fs, channels, 480);
                ce.setParams (p);
                for (int sig = 0; sig < 5; ++sig)
                {
                    Planar buf (channels, n);
                    for (int c = 0; c < channels; ++c)
                    {
                        auto& x = buf.ch[static_cast<size_t> (c)];
                        switch (sig)
                        {
                            case 0: break;
                            case 1: std::fill (x.begin(), x.end(), c % 2 == 0 ? 1.0f : -1.0f); break;
                            case 2: setChannel (buf, c, whiteNoise (n, 1.0f, static_cast<uint32_t> (11 + c))); break;
                            case 3:
                                for (int i = 0; i < n; i += 1009)
                                    x[static_cast<size_t> (i)] = 1.0f;
                                break;
                            default: setChannel (buf, c, sine (fs * 0.45, fs, n, 1.0f)); break;
                        }
                    }
                    ce.reset();
                    processInBlocks (ce, buf, 480);
                    // Worst case: +24 dB transient gain, +6 dB presence, +2 dB shelf, exciter.
                    CHECK (allFinite (buf, 64.0));
                    if (sig == 0)
                        CHECK (peakAbs (buf.ch[0].data(), n) == 0.0);
                }
            }
        }

        // Sanitised parameters.
        ClarityEnhancer ce;
        prepareClarity (ce, fs);
        ce.setParams (wild);
        const auto& q = ce.getParams();
        CHECK (q.attackDb == 12.0f);
        CHECK (q.sustainDb == -12.0f);
        CHECK (q.presence == ClarityParams {}.presence); // NaN keeps the previous value
        CHECK (q.presenceFrequency == 6000.0f);
        CHECK (q.air == 0.0f);
        CHECK (q.deMud == 1.0f);
        CHECK (std::string (ce.name()) == "Clarity");

        // Random automation every block on full-scale noise.
        prepareClarity (ce, fs, 2, 4096);
        FastRandom rng (321);
        auto rnd = [&rng] (float lo, float hi) { return lo + (hi - lo) * 0.5f * (rng.nextBipolar() + 1.0f); };
        Planar buf (2, n);
        setChannel (buf, 0, whiteNoise (n, 1.0f, 5));
        setChannel (buf, 1, whiteNoise (n, 1.0f, 6));
        for (int pos = 0; pos < n;)
        {
            const int len = std::min (n - pos, 1 + static_cast<int> (rng.nextU32() % 700u));
            ClarityParams r;
            r.attackDb = rnd (-12.0f, 12.0f);
            r.sustainDb = rnd (-12.0f, 12.0f);
            r.presence = rng.nextU32() % 3u == 0u ? 0.0f : rnd (0.0f, 1.0f);
            r.presenceFrequency = rnd (1000.0f, 6000.0f);
            r.air = rng.nextU32() % 3u == 0u ? 0.0f : rnd (0.0f, 1.0f);
            r.deMud = rng.nextU32() % 3u == 0u ? 0.0f : rnd (0.0f, 1.0f);
            ce.setParams (r);
            ce.process (buf.block (pos, len).firstChannels (1 + static_cast<int> (rng.nextU32() % 2u)));
            pos += len;
        }
        CHECK (allFinite (buf, 64.0));

        // A NaN / Inf input does not latch.
        ce.setParams (maxi);
        Planar poison (2, 64);
        poison.ch[0][10] = nan;
        poison.ch[1][20] = inf;
        ce.process (poison.block());
        Planar after (2, n);
        setChannel (after, 0, sine (3000.0, fs, n, 0.5f));
        setChannel (after, 1, sine (250.0, fs, n, 0.5f));
        processInBlocks (ce, after, 256);
        Planar tail (2, ms (50, fs));
        for (int c = 0; c < 2; ++c)
            std::copy (after.ch[static_cast<size_t> (c)].end() - tail.numSamples(), after.ch[static_cast<size_t> (c)].end(), tail.ch[static_cast<size_t> (c)].begin());
        CHECK (allFinite (tail, 64.0));
        CHECK (peakAbs (tail.ch[0].data(), tail.numSamples()) > 0.1);
    }
}

TEST_CASE ("Clarity: output is independent of the host block size")
{
    const int n = 3584 * 6; // 3584 = 7 * 512: the parameter change lands on a block edge for every size
    Planar input (2, n);
    {
        const auto noise = gatedNoise (n, 0.3f, 40.0, 130.0, 42);
        const auto low = sine (250.0, kFs, n, 0.4f);
        const auto high = sine (4100.0, kFs, n, 0.1f);
        for (int i = 0; i < n; ++i)
        {
            const size_t k = static_cast<size_t> (i);
            input.ch[0][k] = noise[k] + low[k] + high[k];
            input.ch[1][k] = 0.5f * noise[k] - low[k] + 0.3f * high[k];
        }
    }

    auto run = [&] (int blockSize)
    {
        ClarityEnhancer ce;
        prepareClarity (ce, kFs, 2, 512);
        auto p = allOn();
        ce.setParams (p);
        ce.reset();
        Planar buf = clone (input);
        for (int pos = 0; pos < n; pos += blockSize)
        {
            if (pos == 7168)
            {
                p.presenceFrequency = 5000.0f;
                p.air = 0.0f;
                p.deMud = 1.0f;
                p.attackDb = -8.0f;
                ce.setParams (p);
            }
            ce.process (buf.block (pos, std::min (blockSize, n - pos)));
        }
        return buf;
    };

    const auto ref = run (512);
    for (int bs : { 1, 7, 64 })
        CHECK_LE (maxAbsDiff (run (bs), ref), 1.0e-6);
}
