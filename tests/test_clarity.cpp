// Tests for the clarity enhancer: neutral exactness, transient shaping,
// de-mud, dynamic presence, air exciter (harmonics and freedom from
// aliasing at 44.1 kHz), click-free parameter changes, real-time safety,
// robustness and block-size invariance.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/dsp/ClarityEnhancer.h"
#include "flub/dsp/Crossover.h"
#include "flub/dsp/Fft.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
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

    // Tones in the band's upper skirt (above 7 kHz) are not shaped at full
    // depth, so their 2nd / 3rd harmonics, which would fold back into the
    // audible range, stay low.
    for (const auto& [tone, alias, limitDb] : { std::array<double, 3> { 9000.0, fs - 3.0 * 9000.0, -40.0 },
                                                std::array<double, 3> { 10000.0, fs - 3.0 * 10000.0, -50.0 },
                                                std::array<double, 3> { 12000.0, fs - 2.0 * 12000.0, -50.0 } })
    {
        ce.reset();
        Planar skirt (1, settle + n);
        setChannel (skirt, 0, sine (tone, fs, settle + n, dbfs (-12.0)));
        processInBlocks (ce, skirt, 441);
        CHECK_LE (levelDb (skirt.ch[0], settle, n, alias, dbfs (-12.0), fs), limitDb);
    }
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
        p.presenceMode = i % 7 < 4 ? PresenceMode::Relative : PresenceMode::Absolute; // docs/11 E07 step 3
        // docs/11 E04 step 3: the 3-band path switching on, gliding its split and off.
        p.attackLowDb = i % 11 < 6 ? static_cast<float> (i % 5) * 3.0f : 0.0f;
        p.attackHighDb = i % 13 < 4 ? -6.0f : 0.0f;
        p.lowSplitHz = 60.0f + static_cast<float> (i * 37 % 140);
        p.transientSpeed = 0.5f + 0.5f * static_cast<float> (i % 4);
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
    maxi.attackLowDb = 12.0f; // docs/11 E04 step 3: the 3-band path
    maxi.attackHighDb = 12.0f;
    maxi.lowSplitHz = 200.0f;
    maxi.transientSpeed = 2.0f;
    maxi.presence = 1.0f;
    maxi.presenceFrequency = 6000.0f;
    maxi.air = 1.0f;
    maxi.deMud = 1.0f;
    ClarityParams mini = maxi;
    mini.attackDb = -12.0f;
    mini.sustainDb = -12.0f;
    mini.presenceFrequency = 1000.0f;
    mini.attackLowDb = 0.0f; // full band (maxi and wild run the bands; the test stays under 2 s)
    mini.attackHighDb = 0.0f;
    mini.lowSplitHz = 60.0f;
    mini.transientSpeed = 0.5f;
    ClarityParams wild;
    wild.attackDb = 1.0e9f;
    wild.sustainDb = -inf;
    wild.presence = nan;
    wild.presenceFrequency = 1.0e9f;
    wild.air = -3.0f;
    wild.deMud = inf;
    wild.attackLowDb = nan;
    wild.attackHighDb = inf;
    wild.lowSplitHz = -5.0f;
    wild.transientSpeed = 1.0e9f;
    const ClarityParams settings[] = { ClarityParams {}, maxi, mini, wild };

    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const int n = static_cast<int> (fs * 0.2);
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
        CHECK (q.attackLowDb == 0.0f); // NaN keeps the previous value
        CHECK (q.attackHighDb == 12.0f);
        CHECK (q.lowSplitHz == 60.0f);
        CHECK (q.transientSpeed == 2.0f);
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
            r.attackLowDb = rng.nextU32() % 2u == 0u ? 0.0f : rnd (-12.0f, 12.0f);
            r.attackHighDb = rng.nextU32() % 2u == 0u ? 0.0f : rnd (-12.0f, 12.0f);
            r.lowSplitHz = rnd (60.0f, 200.0f);
            r.transientSpeed = rnd (0.5f, 2.0f);
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

// ---- adversarial review tests ----

TEST_CASE ("Clarity (review): after loud material the output decays to exact silence without a reset")
{
    for (int channels : { 1, 2, 6 })
    {
        ClarityEnhancer ce;
        prepareClarity (ce, kFs, channels);
        ClarityParams p = allOn();
        p.attackDb = 12.0f;
        p.sustainDb = 12.0f;
        p.air = 1.0f;
        ce.setParams (p);
        ce.reset();
        const int loud = ms (500), n = loud + ms (3000);
        Planar buf (channels, n);
        for (int c = 0; c < channels; ++c)
            setChannel (buf, c, whiteNoise (loud, 0.8f, static_cast<uint32_t> (c + 9)));
        processInBlocks (ce, buf, 256);
        for (const auto& c : buf.ch)
            CHECK (peakAbs (c.data() + n - ms (500), ms (500)) == 0.0);
    }
}

TEST_CASE ("Clarity (review): every block size gives bit-identical output")
{
    const int n = 4096 * 5;
    Planar input (2, n);
    {
        const auto noise = gatedNoise (n, 0.4f, 25.0, 90.0, 4);
        const auto mud = sine (240.0, kFs, n, 0.5f);
        const auto hiss = whiteNoise (n, 0.01f, 5);
        for (int i = 0; i < n; ++i)
        {
            const size_t k = static_cast<size_t> (i);
            input.ch[0][k] = noise[k] + mud[k] + hiss[k];
            input.ch[1][k] = 0.2f * noise[k] - mud[k];
        }
    }
    auto run = [&] (int blockSize)
    {
        ClarityEnhancer ce;
        prepareClarity (ce, kFs, 2, 4096);
        auto p = allOn();
        ce.setParams (p);
        ce.reset();
        Planar buf = clone (input);
        for (int pos = 0; pos < n; pos += blockSize)
        {
            if (pos == 12288) // common boundary of every size below
            {
                p.presence = 0.0f;
                p.air = 1.0f;
                p.sustainDb = 12.0f;
                p.presenceFrequency = 1200.0f;
                ce.setParams (p);
            }
            ce.process (buf.block (pos, std::min (blockSize, n - pos)));
        }
        return buf;
    };
    const auto ref = run (4096);
    for (int bs : { 1, 3, 12, 16, 48, 256, 1024 }) // all divide 12288
        CHECK (maxAbsDiff (run (bs), ref) == 0.0);
}

TEST_CASE ("Clarity (review): steady tones through shaper, de-mud and presence stay free of modulation products")
{
    for (double fs : { 44100.0, 192000.0 })
    {
        ClarityEnhancer ce;
        prepareClarity (ce, fs);
        ClarityParams p;
        p.attackDb = 12.0f;
        p.sustainDb = -12.0f;
        p.presence = 1.0f;
        p.deMud = 1.0f;
        ce.setParams (p);
        ce.reset();
        const int n = static_cast<int> (fs * 2.0);
        auto x = sine (250.0, fs, n, 0.3f);
        const auto y = sine (1000.0, fs, n, 0.05f);
        for (size_t i = 0; i < x.size(); ++i)
            x[i] += y[i];
        Planar buf (2, n);
        setChannel (buf, 0, x);
        setChannel (buf, 1, x);
        processInBlocks (ce, buf, 256);
        const int from = static_cast<int> (fs), len = static_cast<int> (fs);
        const double a = toneAmplitude (buf.ch[0].data() + from, len, 250.0, fs);
        CHECK_NEAR (toDb (a / 0.3), -4.0, 0.3); // the de-mud cut is there
        for (double f : { 500.0, 750.0, 1250.0, 1500.0, 2000.0 })
            CHECK_LE (toDb (toneAmplitude (buf.ch[0].data() + from, len, f, fs) / a), -90.0);
    }
}

TEST_CASE ("Clarity (review): presence and de-mud behave the same at every sample rate")
{
    const float quiet = dbfs (-50.0), loud = dbfs (-12.0);
    for (double fs : { 44100.0, 96000.0, 192000.0 })
    {
        const int settle = static_cast<int> (fs * 0.8), measure = static_cast<int> (fs * 0.4);
        ClarityEnhancer ce;
        prepareClarity (ce, fs);
        ClarityParams p;
        p.presence = 1.0f;
        ce.setParams (p);
        Planar buf (2, settle + measure);
        setChannel (buf, 0, sine (3200.0, fs, settle + measure, quiet));
        setChannel (buf, 1, sine (3200.0, fs, settle + measure, quiet));
        ce.reset();
        processInBlocks (ce, buf, 256);
        CHECK_NEAR (toDb (toneAmplitude (buf.ch[0].data() + settle, measure, 3200.0, fs) / quiet), 6.0, 0.5);

        p = ClarityParams {};
        p.deMud = 1.0f;
        ce.setParams (p);
        Planar mud (2, settle + measure);
        setChannel (mud, 0, sine (250.0, fs, settle + measure, loud));
        setChannel (mud, 1, sine (250.0, fs, settle + measure, loud));
        ce.reset();
        processInBlocks (ce, mud, 256);
        CHECK_NEAR (toDb (toneAmplitude (mud.ch[0].data() + settle, measure, 250.0, fs) / loud), -4.0, 0.5);
    }
}

// ---- relative presence (docs/11 E07 step 3) ----

namespace
{
void measured (const std::string& name, double value, const char* unit)
{
    std::printf ("    measured %s = %.2f %s\n", name.c_str(), value, unit);
}

/** Power of x[from, from + len) in [lo, hi] Hz: Hann frames of 4096, summed (dB). */
double bandPowerDb (const std::vector<float>& x, int from, int len, double lo, double hi, double fs = kFs)
{
    constexpr int kFrame = 4096;
    Fft fft;
    fft.prepare (kFrame);
    std::vector<float> w (kFrame);
    std::vector<Fft::Complex> bins (kFrame / 2 + 1);
    double sum = 0.0;
    for (int start = from; start + kFrame <= from + len; start += kFrame / 2)
    {
        for (int i = 0; i < kFrame; ++i)
            w[static_cast<size_t> (i)] = static_cast<float> (x[static_cast<size_t> (start + i)] * (0.5 - 0.5 * std::cos (kTwoPi * i / kFrame)));
        fft.forwardReal (w.data(), bins.data());
        for (size_t k = 0; k < bins.size(); ++k)
        {
            const double f = static_cast<double> (k) * fs / kFrame;
            if (f >= lo && f <= hi)
                sum += std::norm (bins[k]);
        }
    }
    return 10.0 * std::log10 (std::max (sum, 1.0e-30));
}

/** 2.5 - 4 kHz lift of a stereo run of x (same on both channels) over its last 2 s (dB). */
double presenceLiftDb (const ClarityParams& p, const std::vector<float>& x)
{
    ClarityEnhancer ce;
    prepareClarity (ce);
    ce.setParams (p);
    const auto out = runStereo (ce, x);
    const int n = static_cast<int> (x.size()), from = n - ms (2000);
    return bandPowerDb (out.ch[0], from, ms (2000), 2500.0, 4000.0) - bandPowerDb (x, from, ms (2000), 2500.0, 4000.0);
}

std::vector<float> scaled (std::vector<float> x, double db)
{
    for (auto& v : x)
        v *= dbfs (db);
    return x;
}

/** x through a 2nd-order Butterworth section (Svf.h). */
std::vector<float> filtered (std::vector<float> x, FilterType type, double hz)
{
    const auto c = SvfCoeffs::make (type, hz, 0.70710678, 0.0, kFs);
    SvfState s;
    for (auto& v : x)
        v = svfTick (c, s, v);
    return x;
}

ClarityParams presenceOnly (PresenceMode mode)
{
    ClarityParams p;
    p.presence = 1.0f;
    p.presenceMode = mode;
    return p;
}
} // namespace

TEST_CASE ("Clarity (E07 step 3): Relative presence lifts pink the same at -45 and -12 dBFS (Absolute: 5 dB more at -45); at -18 dBFS both laws agree")
{
    // docs/11 E07 Done-when: pink at -45 and -12 dBFS, presence lifts within
    // 1.5 dB. Presence 1 at 3.2 kHz, the 2.5 - 4 kHz lift over the last 2 s
    // of 3 s.
    const auto pink = pinkNoise (ms (3000), 1.0f, 55);
    double lift[2][3] = {};
    const double levels[] = { -45.0, -18.0, -12.0 };
    for (int m = 0; m < 2; ++m)
        for (int l = 0; l < 3; ++l)
        {
            lift[m][l] = presenceLiftDb (presenceOnly (static_cast<PresenceMode> (m)), scaled (pink, levels[l]));
            measured (std::string (m == 0 ? "Absolute" : "Relative") + " presence 1, pink at " + std::to_string (static_cast<int> (levels[l]))
                                    + " dBFS: 2.5-4 kHz lift",
                                lift[m][l], "dB");
        }
    // The absolute law: 5 dB more on the quiet pink (the KnownGap).
    CHECK_GE (lift[0][0] - lift[0][2], 4.0);
    // The relative law: the Done-when row, with a margin.
    CHECK_LE (std::abs (lift[1][0] - lift[1][2]), 0.5);
    CHECK_GE (lift[1][1], 1.0);
    // Calibrated at the chain's nominal level (-18 dBFS RMS, AutoLevel's
    // default target): there the two laws give pink the same lift.
    CHECK_NEAR (lift[1][1], lift[0][1], 0.3);
}

TEST_CASE ("Clarity (E07 step 3): Relative presence follows the programme's balance - a dark programme gets the full lift, a bright one none, and a band that jumps over the body is not lifted")
{
    const auto pink = scaled (pinkNoise (ms (3000), 1.0f, 77), -24.0);
    const auto rel = presenceOnly (PresenceMode::Relative);
    const double neutral = presenceLiftDb (rel, pink);
    // Dark: pink through two low-pass sections at 800 Hz (the presence band
    // ~20 dB further under the body). Bright: two high-passes at 1.5 kHz.
    const double dark = presenceLiftDb (rel, filtered (filtered (pink, FilterType::LowPass, 800.0), FilterType::LowPass, 800.0));
    const double bright = presenceLiftDb (rel, filtered (filtered (pink, FilterType::HighPass, 1500.0), FilterType::HighPass, 1500.0));
    measured ("Relative presence 1: lift of pink / dark / bright programme", neutral, "dB");
    measured ("  dark (LP 800 Hz x2)", dark, "dB");
    measured ("  bright (HP 1.5 kHz x2)", bright, "dB");
    CHECK_GE (dark, neutral + 2.0);
    CHECK_GE (dark, 4.5); // the full 6 dB bell reads about 5 dB over 2.5 - 4 kHz
    CHECK_LE (bright, 0.3);

    // A 3.2 kHz noise burst 12 dB over the pink's band, 40 ms every 800 ms:
    // the fast balance withdraws the lift within the burst (as the absolute
    // law does on a loud band); between the bursts the programme keeps most
    // of it (the bursts brighten its slow balance by about 2.5 dB).
    const int len = ms (4800);
    auto x = scaled (pinkNoise (len, 1.0f, 78), -24.0);
    const auto burst = filtered (filtered (whiteNoise (len, 1.0f, 9), FilterType::HighPass, 2800.0), FilterType::LowPass, 3600.0);
    const double gain = std::pow (10.0, (bandPowerDb (x, 0, len, 2500.0, 4000.0) - bandPowerDb (burst, 0, len, 2500.0, 4000.0) + 12.0) / 20.0);
    for (size_t i = 0; i < x.size(); ++i)
        if (static_cast<int> (i) % ms (800) < ms (40))
            x[i] += static_cast<float> (gain) * burst[i];
    ClarityEnhancer ce;
    prepareClarity (ce);
    ce.setParams (rel);
    const auto out = runStereo (ce, x);
    double inBurst = 0.0, outBurst = 0.0, between = 0.0;
    int windows = 0;
    for (int start = ms (1600); start + ms (800) <= len; start += ms (800), ++windows)
    {
        // 15 .. 40 ms of each burst (the 5 ms withdrawal is over).
        for (int i = start + ms (15); i < start + ms (40); ++i)
        {
            inBurst += static_cast<double> (x[static_cast<size_t> (i)]) * x[static_cast<size_t> (i)];
            outBurst += static_cast<double> (out.ch[0][static_cast<size_t> (i)]) * out.ch[0][static_cast<size_t> (i)];
        }
        between += bandPowerDb (out.ch[0], start + ms (200), ms (500), 2500.0, 4000.0) - bandPowerDb (x, start + ms (200), ms (500), 2500.0, 4000.0);
    }
    const double burstLift = 10.0 * std::log10 (outBurst / inBurst);
    between /= windows;
    measured ("Relative presence 1: broadband lift inside a 3.2 kHz burst 12 dB over the band", burstLift, "dB");
    measured ("  2.5-4 kHz lift between the bursts", between, "dB");
    CHECK_LE (burstLift, 0.3);
    CHECK_GE (between, 0.7);
}

TEST_CASE ("Clarity (E07 step 3): switching presenceMode glides without a click (also during the warm-up); Relative at presence 0 is an exact pass-through; the output does not depend on the block size")
{
    // The parameter-change programme of "Clarity: parameter changes are
    // click-free": the two laws ask for about 3 dB apart on its 2 kHz tone.
    const int n = ms (3000);
    const auto a = sine (300.0, kFs, n, 0.25f);
    const auto b = sine (2000.0, kFs, n, 0.1f);
    std::vector<float> x (a.size());
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = a[i] + b[i];
    const auto hp = SvfCoeffs::make (FilterType::HighPass, 15000.0, 0.7071, 0.0, kFs);
    auto hfPeak = [&hp] (const Planar& y) {
        double peak = 0.0;
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
    auto run = [&] (int togglePeriodBlocks, double* toneDb) {
        ClarityEnhancer ce;
        prepareClarity (ce);
        auto p = presenceOnly (PresenceMode::Absolute);
        ce.setParams (p);
        ce.reset();
        Planar buf (2, n);
        setChannel (buf, 0, x);
        setChannel (buf, 1, x);
        const int block = 240; // 5 ms: a period of 3 blocks switches within the 60 ms warm-up
        for (int pos = 0, k = 0; pos < n; pos += block, ++k)
        {
            if (togglePeriodBlocks > 0 && k % togglePeriodBlocks == togglePeriodBlocks / 2)
            {
                p.presenceMode = p.presenceMode == PresenceMode::Absolute ? PresenceMode::Relative : PresenceMode::Absolute;
                ce.setParams (p);
            }
            ce.process (buf.block (pos, std::min (block, n - pos)));
        }
        if (toneDb != nullptr)
            *toneDb = levelDb (buf.ch[0], n - ms (500), ms (500), 2000.0, 0.1);
        return buf;
    };
    double absoluteDb = 0.0, relativeDb = 0.0;
    const double steady = hfPeak (run (0, &absoluteDb));
    {
        ClarityEnhancer ce;
        prepareClarity (ce);
        ce.setParams (presenceOnly (PresenceMode::Relative));
        relativeDb = levelDb (runStereo (ce, x, 240).ch[0], n - ms (500), ms (500), 2000.0, 0.1);
    }
    measured ("300 Hz + 2 kHz programme, 2 kHz gain: Absolute / Relative", absoluteDb, "dB");
    measured ("  Relative", relativeDb, "dB");
    CHECK_GE (std::abs (relativeDb - absoluteDb), 1.5); // the switch moves the bell
    for (int period : { 50, 3 })
    {
        const double toggled = hfPeak (run (period, nullptr));
        CHECK_LE (toggled, 1.0e-4); // -80 dBFS; programme at about -9 dBFS
        CHECK_LE (toggled, std::max (4.0 * steady, 2.0e-5));
    }

    // Relative with presence 0: nothing runs.
    {
        ClarityEnhancer ce;
        prepareClarity (ce);
        ClarityParams p;
        p.presenceMode = PresenceMode::Relative;
        ce.setParams (p);
        const auto out = runStereo (ce, x);
        bool exact = true;
        for (size_t i = 0; i < x.size(); ++i)
            exact = exact && out.ch[0][i] == x[i];
        CHECK (exact);
    }

    // Block-size invariance, a switch to Relative and back on a common boundary.
    const int len = 4096 * 5;
    Planar input (2, len);
    {
        const auto pink = pinkNoise (len, 0.05f, 12);
        const auto noise = gatedNoise (len, 0.2f, 25.0, 90.0, 4);
        for (int i = 0; i < len; ++i)
        {
            const size_t k = static_cast<size_t> (i);
            input.ch[0][k] = pink[k] + noise[k];
            input.ch[1][k] = pink[k] - 0.5f * noise[k];
        }
    }
    auto runBlocks = [&] (int blockSize) {
        ClarityEnhancer ce;
        prepareClarity (ce, kFs, 2, 4096);
        auto p = allOn();
        p.presenceMode = PresenceMode::Relative;
        ce.setParams (p);
        ce.reset();
        Planar buf = clone (input);
        for (int pos = 0; pos < len; pos += blockSize)
        {
            if (pos == 6144 || pos == 12288)
            {
                p.presenceMode = pos == 6144 ? PresenceMode::Absolute : PresenceMode::Relative;
                ce.setParams (p);
            }
            ce.process (buf.block (pos, std::min (blockSize, len - pos)));
        }
        return buf;
    };
    const auto ref = runBlocks (4096 / 2);
    for (int bs : { 1, 3, 12, 16, 48, 256, 1024 }) // all divide 6144
        CHECK (maxAbsDiff (runBlocks (bs), ref) == 0.0);
}

// ---- docs/11 E04 step 3: the 3-band path ----

namespace
{
/** Hits every periodMs decaying with tau (kind 0: a 60 Hz kick, 1: a 1 kHz
    pluck, 2: near-Gaussian noise differentiated twice, i.e. mostly above
    4 kHz). */
std::vector<float> bandHits (int kind, int n, double periodMs, double tauMs)
{
    std::vector<float> v (static_cast<size_t> (n));
    FastRandom rng (3);
    const int period = ms (periodMs);
    for (int i = 0; i < n; ++i)
    {
        const double t = static_cast<double> (i % period) / kFs;
        const double env = std::exp (-t / (tauMs * 0.001));
        double x;
        if (kind == 0)
            x = 0.5 * env * std::sin (kTwoPi * 60.0 * t);
        else if (kind == 1)
            x = 0.4 * env * std::sin (kTwoPi * 1000.0 * t);
        else
            x = 0.15 * env * static_cast<double> (rng.nextBipolar() + rng.nextBipolar() + rng.nextBipolar() + rng.nextBipolar());
        v[static_cast<size_t> (i)] = static_cast<float> (x);
    }
    if (kind == 2)
    {
        float p1 = 0.0f, p2 = 0.0f;
        for (auto& x : v)
        {
            const float y = 0.25f * (x - 2.0f * p1 + p2);
            p2 = p1;
            p1 = x;
            x = y;
        }
    }
    return v;
}

double liftDb (const std::vector<float>& y, const std::vector<float>& ref, int from, int to)
{
    return toDb (rms (y.data() + from, to - from) / std::max (1.0e-30, rms (ref.data() + from, to - from)));
}

/** Runs x on both channels from a reset; returns channel 0. */
std::vector<float> runParams (const ClarityParams& p, const std::vector<float>& x, int blockSize = 256)
{
    ClarityEnhancer ce;
    prepareClarity (ce);
    ce.setParams (p);
    return runStereo (ce, x, blockSize).ch[0];
}
} // namespace

TEST_CASE ("Clarity (E04 step 3): each band keeps +12 dB of attack at >= 9 dB on hits 75 ms apart (full band: 3.9 - 7.5 dB) and leaves no bump > 1 dB at 40-60 ms (full band: 3.3 - 3.5 dB); the other bands stay at unity")
{
    // docs/11 E04 Done-when, through the module: the kick in the low band,
    // the pluck in the mid band, the click in the high band, +12 dB in the
    // band under test and 0 in the others. Lifts are the RMS of the first
    // 10 ms of each hit (and of 40 - 60 ms after isolated hits) against the
    // input (the path at unity gains is the input exactly, step 4); the
    // full-band shaper at +12 dB shows what the bands fix.
    const double taus[] = { 30.0, 15.0, 8.0 };
    for (int kind = 0; kind < 3; ++kind)
    {
        ClarityParams bands;
        bands.attackDb = kind == 1 ? 12.0f : 0.0f;
        bands.attackLowDb = kind == 0 ? 12.0f : (kind == 1 ? -12.0f : 0.0f);
        bands.attackHighDb = kind == 2 ? 12.0f : (kind == 1 ? -12.0f : 0.0f);
        ClarityParams full;
        full.attackDb = 12.0f;

        double fastLift = 0.0, fullFastLift = 0.0, bump = -99.0, fullBump = -99.0;
        for (double period : { 75.0, 500.0 })
        {
            const int n = ms (period * 11.5);
            const auto x = bandHits (kind, n, period, taus[kind]);
            const auto& ref = x;
            const auto y = runParams (bands, x);
            const auto yFull = runParams (full, x);
            double lift = 0.0, liftFull = 0.0;
            for (int h = 3; h <= 10; ++h)
            {
                const int on = h * ms (period);
                lift += liftDb (y, ref, on, on + ms (10)) / 8.0;
                liftFull += liftDb (yFull, x, on, on + ms (10)) / 8.0;
                if (period > 100.0)
                {
                    bump = std::max (bump, liftDb (y, ref, on + ms (40), on + ms (60)));
                    fullBump = std::max (fullBump, liftDb (yFull, x, on + ms (40), on + ms (60)));
                }
            }
            if (period < 100.0)
            {
                fastLift = lift;
                fullFastLift = liftFull;
            }
        }
        std::printf ("  E04 Clarity band %d: 0-10 ms lift 75 ms apart %.2f dB (full band %.2f), 40-60 ms %.2f dB (full band %.2f)\n", kind,
                     fastLift, fullFastLift, bump, fullBump);
        CHECK_GE (fastLift, 9.0);
        CHECK_LE (bump, 1.0);
        CHECK_LE (fullFastLift, 8.0);
        CHECK_GE (fullBump, 3.0);
    }

    // The bands at 0 dB leave their content nearly alone: the pluck with
    // only the low band at +12 dB comes out within 0.75 dB. The level-
    // independent low band reads the pluck onset's own low-frequency skirt
    // as an onset, and its lift reaches 1 kHz through the one-pole low band
    // it is applied to (-18.5 dB, near quadrature): 0.60 dB (0.10 dB when
    // step 3 applied it to the LR4 band, whose all-pass took 2.4 dB off a
    // kick's first 10 ms instead; docs/11 E04 step 4).
    const int n = ms (500 * 3);
    const auto pluck = bandHits (1, n, 500.0, 15.0);
    ClarityParams lowOnly;
    lowOnly.attackLowDb = 12.0f;
    const auto y = runParams (lowOnly, pluck);
    const double leak = liftDb (y, pluck, ms (500), n);
    std::printf ("  E04 1 kHz pluck, low band +12 dB: %.2f dB\n", leak);
    CHECK_GE (leak, -0.05);
    CHECK_LE (leak, 0.75);
}

TEST_CASE ("Clarity (E04 step 3): a steady 40 Hz note moves <= 0.1 dB with every band at +-12 dB attack and sustain, and gains no sidebands")
{
    // docs/11 E04 Done-when. Read once the note has settled: in its first
    // 2.5 s a +12 dB sustain also lifts the mid band's share of the note
    // (its LR4 slope passes 40 Hz at -38 dB) while it settles from the
    // high-pass's onset transient, which moves the note by 0.31 dB (a -12 dB
    // sustain 0.08 dB), once, like any decay the sustain acts on.
    const int n = ms (3500);
    const auto x = sine (40.0, kFs, n, 0.25f);
    const float combos[][4] = { // attack, sustain, low offset, high offset
        { 12, 12, 12, 12 }, { -12, -12, -12, -12 }, { 12, -12, 12, 12 }, { -12, 12, -12, -12 }, { 0, 12, 12, -12 }, { 0, -12, -12, 12 } };
    double worst = 0.0;
    for (const auto& c : combos)
    {
        ClarityParams p;
        p.attackDb = c[0];
        p.sustainDb = c[1];
        p.attackLowDb = c[2];
        p.attackHighDb = c[3];
        const auto y = runParams (p, x);
        double lo = 1.0e9, hi = -1.0e9;
        for (int w = ms (2500); w + ms (25) <= n; w += ms (5))
        {
            const double db = toDb (rms (y.data() + w, ms (25)));
            lo = std::min (lo, db);
            hi = std::max (hi, db);
        }
        CHECK_LE (hi - lo, 0.1);
        worst = std::max (worst, hi - lo);
        const double a1 = toneAmplitude (y.data() + ms (2500), ms (1000), 40.0, kFs);
        CHECK_NEAR (toDb (a1 / 0.25), 0.0, 0.05);
        for (int k = 2; k <= 4; ++k)
            CHECK_LE (toDb (toneAmplitude (y.data() + ms (2500), ms (1000), 40.0 * k, kFs) / a1), -80.0);
    }
    std::printf ("  E04 steady 40 Hz, every band at +-12 dB: level range %.4f dB\n", worst);
}

TEST_CASE ("Clarity (E04 step 3): with both offsets at 0 the shaper stays full band; an offset starts the 3-band path, which leaves the output alone while it warms up, then crossfades in and back out without a click, and stops")
{
    const int n = ms (2000);
    std::vector<float> x (static_cast<size_t> (n));
    {
        const auto hitsNoise = gatedNoise (n, 0.3f, 30.0, 120.0, 8);
        const auto low = sine (70.0, kFs, n, 0.3f);
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = hitsNoise[i] + low[i];
    }
    ClarityParams p;
    p.attackDb = 6.0f;
    p.sustainDb = -4.0f;

    ClarityEnhancer ref, detour;
    prepareClarity (ref);
    prepareClarity (detour);
    ref.setParams (p);
    detour.setParams (p);
    Planar a (2, n), b (2, n);
    for (auto* buf : { &a, &b })
    {
        setChannel (*buf, 0, x);
        setChannel (*buf, 1, x);
    }
    ref.reset();
    detour.reset();
    CHECK (! detour.isBandPathActive());
    const int block = 240, on = ms (500), off = ms (1000);
    for (int pos = 0; pos < n; pos += block)
    {
        if (pos == on)
        {
            auto q = p;
            q.attackLowDb = 6.0f;
            detour.setParams (q);
            CHECK (detour.isBandPathActive());
        }
        if (pos == off)
            detour.setParams (p);
        ref.process (a.block (pos, block));
        detour.process (b.block (pos, block));
        if (pos == ms (1200))
            CHECK (! detour.isBandPathActive()); // faded out and stopped
    }
    // The warm-up (5 x the low band's 10 ms slow attack) leaves the output
    // bit-identical; the crossfade then changes it; once stopped, the full-
    // band shaper (which never stopped) is bit-identical again.
    auto same = [&] (int from, int to)
    {
        for (int c = 0; c < 2; ++c)
            for (int i = from; i < to; ++i)
                if (a.ch[static_cast<size_t> (c)][static_cast<size_t> (i)] != b.ch[static_cast<size_t> (c)][static_cast<size_t> (i)])
                    return false;
        return true;
    };
    CHECK (same (0, on + ms (49)));
    CHECK (! same (on + ms (50), off));
    CHECK (same (ms (1200), n));

    // Neutral with a detour through the offsets: exact pass-through again.
    ClarityEnhancer ce;
    prepareClarity (ce);
    ClarityParams q;
    q.attackHighDb = -9.0f;
    ce.setParams (q);
    Planar warm (2, ms (300));
    setChannel (warm, 0, whiteNoise (ms (300), 0.3f, 4));
    processInBlocks (ce, warm, 128);
    ce.setParams (ClarityParams {});
    Planar settle (2, ms (300));
    setChannel (settle, 0, whiteNoise (ms (300), 0.3f, 5));
    processInBlocks (ce, settle, 128);
    CHECK (! ce.isBandPathActive());
    Planar again (2, ms (300));
    setChannel (again, 0, whiteNoise (ms (300), 0.3f, 6));
    setChannel (again, 1, sine (1000.0, kFs, ms (300), 0.3f));
    const Planar dry = clone (again);
    processInBlocks (ce, again, 128);
    CHECK (again.ch == dry.ch);
}

TEST_CASE ("Clarity (E04 step 3): switching the band offsets, the split and the speed is click-free; the split decides which band a note is in; the output does not depend on the block size and decays to exact silence")
{
    // As "Clarity: parameter changes are click-free": 300 Hz + 2 kHz, so
    // energy above 15 kHz can only come from a discontinuity. Air is off:
    // while the band sum's phase moves (the crossfade, a split glide) the
    // 2 kHz tone's level in the exciter's band dips and recovers, and the
    // exciter clips its normalised input for the 0.5 ms its envelope takes
    // to follow a rise, as on any fast rise in a programme (-78 dBFS above
    // 15 kHz on this -9 dBFS programme, 21 x its steady level).
    const int n = ms (3000);
    std::vector<float> x (static_cast<size_t> (n));
    {
        const auto a = sine (300.0, kFs, n, 0.25f);
        const auto b = sine (2000.0, kFs, n, 0.1f);
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = a[i] + b[i];
    }
    auto run = [&] (bool toggle, int blockSize)
    {
        ClarityEnhancer ce;
        prepareClarity (ce, kFs, 2, 512);
        ClarityParams p = allOn();
        p.air = 0.0f;
        ce.setParams (p);
        ce.reset();
        Planar buf (2, n);
        setChannel (buf, 0, x);
        setChannel (buf, 1, x);
        for (int pos = 0; pos < n; pos += blockSize)
        {
            if (toggle && pos % 6000 == 2400) // a common boundary of every block size used
            {
                switch ((pos / 6000) % 5)
                {
                    case 0: p.attackLowDb = p.attackLowDb != 0.0f ? 0.0f : 12.0f; break;
                    case 1: p.attackHighDb = p.attackHighDb != 0.0f ? 0.0f : -12.0f; break;
                    case 2: p.lowSplitHz = p.lowSplitHz > 100.0f ? 60.0f : 200.0f; break;
                    case 3: p.transientSpeed = p.transientSpeed > 1.0f ? 0.5f : 2.0f; break;
                    default: p.attackDb = p.attackDb > 0.0f ? -12.0f : 12.0f; break;
                }
                ce.setParams (p);
            }
            ce.process (buf.block (pos, std::min (blockSize, n - pos)));
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
    const Planar toggled = run (true, 240);
    const double steady = hfPeak (run (false, 240));
    CHECK_LE (hfPeak (toggled), 1.0e-4); // -80 dBFS; programme at about -9 dBFS
    CHECK_LE (hfPeak (toggled), std::max (4.0 * steady, 2.0e-5));
    for (int bs : { 1, 16, 600 }) // all divide 2400 and 6000, as 240 does
        CHECK (maxAbsDiff (run (true, bs), toggled) == 0.0);

    // The split: a 150 Hz pluck is in the low band at a 200 Hz split (lifted
    // with the low band at +12 dB) and in the mid band at 60 Hz (not lifted).
    const auto pluck150 = [] {
        std::vector<float> v (static_cast<size_t> (ms (1500)));
        for (size_t i = 0; i < v.size(); ++i)
        {
            const double t = static_cast<double> (static_cast<int> (i) % ms (500)) / kFs;
            v[i] = static_cast<float> (0.4 * std::exp (-t / 0.03) * std::sin (kTwoPi * 150.0 * t));
        }
        return v;
    }();
    auto onsetLift = [&] (float splitHz)
    {
        ClarityParams p;
        p.attackLowDb = 12.0f;
        p.lowSplitHz = splitHz;
        const auto y = runParams (p, pluck150);
        return liftDb (y, pluck150, ms (1000), ms (1010));
    };
    // (At 60 Hz the low band still passes 150 Hz at -16 dB, and a level-
    // independent shaper lifts that as much as a full-level onset.)
    const double at200 = onsetLift (200.0f), at60 = onsetLift (60.0f);
    std::printf ("  E04 150 Hz pluck, low band +12 dB: first 10 ms %.2f dB at a 200 Hz split, %.2f dB at 60 Hz\n", at200, at60);
    CHECK_GE (at200, 6.0);
    CHECK_LE (at60, at200 - 3.0);

    // Loud material, then silence: exact zeros without a reset.
    ClarityEnhancer ce;
    prepareClarity (ce, kFs, 2);
    ClarityParams p = allOn();
    p.attackLowDb = 12.0f;
    p.attackHighDb = 12.0f;
    p.sustainDb = 12.0f;
    ce.setParams (p);
    ce.reset();
    const int loud = ms (500), total = loud + ms (3000);
    Planar buf (2, total);
    setChannel (buf, 0, whiteNoise (loud, 0.8f, 9));
    setChannel (buf, 1, whiteNoise (loud, 0.8f, 10));
    processInBlocks (ce, buf, 256);
    CHECK (ce.isBandPathActive());
    for (const auto& c : buf.ch)
        CHECK (peakAbs (c.data() + total - ms (500), ms (500)) == 0.0);
}

TEST_CASE ("Clarity (E04 step 5): a look-ahead is the module's latency - neutral it delays the input bit for bit - and an onset's lift is in place when it arrives, in both paths; it allocates nothing and does not depend on the block size")
{
    // setLookaheadMs is meant for the Quality profile (the chain does not
    // set it yet: the Quality totals it would raise are pinned elsewhere,
    // docs/11 E04 Status). 1 ms at 48 kHz = 48 samples.
    const int n = ms (1000);
    const auto runWith = [] (float lookaheadMs, const ClarityParams& p, const std::vector<float>& x, int blockSize) {
        ClarityEnhancer ce;
        ce.setLookaheadMs (lookaheadMs);
        prepareClarity (ce);
        ce.setParams (p);
        return std::make_pair (runStereo (ce, x, blockSize), ce.latencySamples());
    };

    // Neutral: the input, 48 samples later, exactly.
    const auto noise = whiteNoise (n, 0.3f, 17);
    const auto [neutral, latency] = runWith (1.0f, ClarityParams {}, noise, 256);
    CHECK (latency == 48);
    bool exact = true;
    for (int i = 0; i < n; ++i)
        exact = exact && neutral.ch[0][static_cast<size_t> (i)] == (i < latency ? 0.0f : noise[static_cast<size_t> (i - latency)]);
    CHECK (exact);
    ClarityEnhancer plain;
    prepareClarity (plain);
    CHECK (plain.latencySamples() == 0);

    // +12 dB attack on isolated 1 kHz plucks (full band) and high-band
    // clicks (the 3-band path): the lift over each onset's first 1 ms and
    // first 10 ms, against the input (the output read `latency` later).
    const auto firstLift = [&] (const Planar& y, const std::vector<float>& x, int delay, double toMs) {
        double lift = 0.0;
        for (int h = 3; h <= 10; ++h)
        {
            const int on = h * ms (100);
            double yy = 0.0, xx = 0.0;
            for (int i = on; i < on + ms (toMs); ++i)
            {
                yy += static_cast<double> (y.ch[0][static_cast<size_t> (i + delay)]) * y.ch[0][static_cast<size_t> (i + delay)];
                xx += static_cast<double> (x[static_cast<size_t> (i)]) * x[static_cast<size_t> (i)];
            }
            lift += 10.0 * std::log10 (yy / xx) / 8.0;
        }
        return lift;
    };
    for (int kind : { 1, 2 })
    {
        const auto x = bandHits (kind, ms (1100), 100.0, kind == 1 ? 15.0 : 8.0);
        ClarityParams p;
        p.attackDb = kind == 1 ? 12.0f : 0.0f;
        p.attackHighDb = kind == 2 ? 12.0f : 0.0f;
        const auto [y0, l0] = runWith (0.0f, p, x, 256);
        const auto [y1, l1] = runWith (1.0f, p, x, 256);
        const double before1 = firstLift (y0, x, l0, 1.0), after1 = firstLift (y1, x, l1, 1.0);
        const double before10 = firstLift (y0, x, l0, 10.0), after10 = firstLift (y1, x, l1, 10.0);
        std::printf ("  E04 step 5, %s: first 1 ms lift %.2f -> %.2f dB, first 10 ms %.2f -> %.2f dB\n",
                     kind == 1 ? "full band, 1 kHz pluck" : "high band, click", before1, after1, before10, after10);
        // Measured: full band 4.18 -> 8.90 dB (its 20 ms slow attack lets the
        // gain reach its peak about 2 ms into an onset), high band 9.50 ->
        // 11.85 dB; over 10 ms 10.19 -> 10.72 and 9.87 -> 9.77 dB.
        CHECK_GE (after1, before1 + 2.0);
        CHECK_GE (after1, 8.5);
        CHECK_GE (after10, before10 - 0.1);

        // Block-size independent.
        const auto [y2, l2] = runWith (1.0f, p, x, 61);
        CHECK (maxAbsDiff (y1, y2) == 0.0);
    }

    // No allocation with the delay line running (both paths).
    ClarityEnhancer ce;
    ce.setLookaheadMs (2.0f);
    prepareClarity (ce, kFs, 2, 512);
    Planar buf (2, 512);
    setChannel (buf, 0, whiteNoise (512, 0.5f, 3));
    setChannel (buf, 1, whiteNoise (512, 0.5f, 4));
    flubtest::AllocationGuard guard;
    ce.reset();
    for (int i = 0; i < 40; ++i)
    {
        ClarityParams p;
        p.attackDb = static_cast<float> (i % 7) - 3.0f;
        p.attackHighDb = i % 3 == 0 ? 6.0f : 0.0f;
        ce.setParams (p);
        ce.process (buf.block (0, 1 + i * 53 % 512));
    }
    CHECK (guard.allocations() == 0);
}
