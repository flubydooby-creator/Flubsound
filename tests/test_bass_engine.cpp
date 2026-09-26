// Tests for the bass engine: subsonic filter, mono bass, adaptive low shelf
// with headroom protection, psychoacoustic harmonics, replace-fundamental,
// tighten, click-free switching, real-time safety, robustness and
// block-size invariance.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/dsp/BassEngine.h"

#include <algorithm>
#include <cmath>
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

void prepareBass (BassEngine& be, double fs = kFs, int channels = 2, int maxBlock = 512)
{
    ProcessSpec spec;
    spec.sampleRate = fs;
    spec.maxBlockSize = maxBlock;
    spec.numChannels = channels;
    be.prepare (spec);
}

/** Every feature off (the defaults enable the 20 Hz subsonic filter). */
BassEngineParams allOff()
{
    BassEngineParams p;
    p.subsonicHz = 0.0f;
    return p;
}

BassEngineParams allOn()
{
    BassEngineParams p;
    p.boostDb = 10.0f;
    p.boostFrequency = 80.0f;
    p.protectThresholdDb = -6.0f;
    p.harmonicsAmount = 0.8f;
    p.harmonicsCutoff = 110.0f;
    p.harmonicsCharacter = 0.3f;
    p.replaceFundamental = true;
    p.tighten = 0.7f;
    p.monoBelowHz = 120.0f;
    p.subsonicHz = 25.0f;
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

/** Processes left / right (right empty = mono copy of left) from a reset. */
Planar runStereo (BassEngine& be, const std::vector<float>& left, const std::vector<float>& right = {}, int blockSize = 256)
{
    const int n = static_cast<int> (left.size());
    Planar buf (2, n);
    setChannel (buf, 0, left);
    setChannel (buf, 1, right.empty() ? left : right);
    be.reset();
    processInBlocks (be, buf, blockSize);
    return buf;
}

/** Amplitude (dB re amplitude ref) of the component at freq over [from, from + len). */
double levelDb (const std::vector<float>& y, int from, int len, double freq, double ref = 1.0, double fs = kFs)
{
    return toDb (toneAmplitude (y.data() + from, len, freq, fs) / ref);
}

std::vector<float> decayingTone (int n, double freq, float amplitude, double tauMs, double fs = kFs)
{
    std::vector<float> v (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        const double t = static_cast<double> (i) / fs;
        v[static_cast<size_t> (i)] = static_cast<float> (amplitude * std::exp (-t / (tauMs * 0.001)) * std::sin (kTwoPi * freq * t));
    }
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
} // namespace

//==============================================================================
TEST_CASE ("BassEngine: silence in gives exactly silence out; no DC")
{
    const int n = ms (1000);
    for (int channels : { 1, 2, 6 })
    {
        for (const auto& p : { BassEngineParams {}, allOff(), allOn() })
        {
            BassEngine be;
            prepareBass (be, kFs, channels);
            be.setParams (p);
            be.reset();
            Planar buf (channels, n);
            processInBlocks (be, buf, 256);
            double peak = 0.0;
            for (const auto& c : buf.ch)
                peak = std::max (peak, peakAbs (c.data(), n));
            CHECK (peak == 0.0);

            // ... also right after loud material and a reset.
            Planar loud (channels, n);
            for (int c = 0; c < channels; ++c)
                setChannel (loud, c, sine (45.0 + 10.0 * c, kFs, n, 0.9f));
            processInBlocks (be, loud, 256);
            be.reset();
            Planar quiet (channels, ms (200));
            processInBlocks (be, quiet, 100);
            for (const auto& c : quiet.ch)
                CHECK (peakAbs (c.data(), quiet.numSamples()) == 0.0);
        }
    }

    // DC is removed by the (default) subsonic filter, with everything else on too.
    for (const auto& p : { BassEngineParams {}, allOn() })
    {
        BassEngine be;
        prepareBass (be);
        be.setParams (p);
        const std::vector<float> dc (static_cast<size_t> (ms (2000)), 0.5f);
        const auto out = runStereo (be, dc);
        const int tail = ms (200);
        for (const auto& c : out.ch)
        {
            double mean = 0.0;
            for (int i = ms (2000) - tail; i < ms (2000); ++i)
                mean += c[static_cast<size_t> (i)];
            CHECK_LE (std::abs (mean / tail), 1.0e-4);
        }
    }

    // The harmonics generator adds no DC of its own (no subsonic filter here).
    BassEngine be;
    prepareBass (be);
    auto p = allOff();
    p.harmonicsAmount = 1.0f;
    p.harmonicsCharacter = 0.0f; // most even-order content (T2, T4 carry the constant terms)
    be.setParams (p);
    const auto out = runStereo (be, sine (50.0, kFs, ms (2000), 0.5f));
    double mean = 0.0;
    for (int i = ms (1000); i < ms (2000); ++i) // 50 whole periods
        mean += out.ch[0][static_cast<size_t> (i)];
    CHECK_LE (std::abs (mean / ms (1000)), 1.0e-4);
}

TEST_CASE ("BassEngine: the low shelf boosts a quiet tone by its designed amount")
{
    const float amp = dbfs (-40.0);
    const int settle = ms (1000), measure = ms (500); // whole periods of 20 and 40 Hz
    auto p = allOff();
    p.boostDb = 9.0f;
    p.boostFrequency = 60.0f;

    BassEngine be;
    prepareBass (be);
    be.setParams (p);
    const auto shelf = SvfCoeffs::make (FilterType::LowShelf, 60.0, 0.7, 9.0, kFs);

    for (double f : { 20.0, 40.0, 60.0, 150.0, 1000.0 })
    {
        const auto out = runStereo (be, sine (f, kFs, settle + measure, amp));
        CHECK_NEAR (levelDb (out.ch[0], settle, measure, f, amp), shelf.magnitudeDb (f, kFs), 0.1);
        CHECK (be.getProtectionDb() == 0.0f);
    }

    // Deep in the shelf a quiet tone gets the full boost (+9 dB within 0.5 dB).
    // Note: at 40 Hz a Q 0.7 shelf whose half-gain point is 60 Hz gives ~7.3 dB,
    // which the analytic check above verifies.
    {
        const auto out = runStereo (be, sine (20.0, kFs, settle + measure, amp));
        CHECK_NEAR (levelDb (out.ch[0], settle, measure, 20.0, amp), 9.0, 0.5);
    }
    p.boostFrequency = 120.0f;
    be.setParams (p);
    {
        const auto out = runStereo (be, sine (40.0, kFs, settle + measure, amp));
        CHECK_NEAR (levelDb (out.ch[0], settle, measure, 40.0, amp), 9.0, 0.5);
    }
}

TEST_CASE ("BassEngine: headroom protection withdraws the boost on loud low frequencies")
{
    const int settle = ms (1000), measure = ms (500);

    // Cap at 0 dBFS, +12 dB boost, 40 Hz at -6 dBFS: the boost is cut back so
    // the LF level stays at the cap (the prediction assumes the full shelf gain,
    // so the real output lands a little below).
    {
        auto p = allOff();
        p.boostDb = 12.0f;
        p.boostFrequency = 60.0f;
        p.protectThresholdDb = 0.0f;
        BassEngine be;
        prepareBass (be);
        be.setParams (p);
        const float amp = dbfs (-6.0);
        const auto out = runStereo (be, sine (40.0, kFs, settle + measure, amp));
        const double outPeakDb = toDb (peakAbs (out.ch[0].data() + settle, measure));
        CHECK_LE (outPeakDb, 0.0 + 1.0);
        CHECK_GE (outPeakDb, -6.0 + 2.0); // still boosted, just not beyond the cap
        CHECK_GE (be.getProtectionDb(), 4.0f);
        CHECK_LE (be.getProtectionDb(), 12.0f);
    }

    // A tone already above the cap: the whole boost is withdrawn, never a cut.
    {
        auto p = allOff();
        p.boostDb = 9.0f;
        p.boostFrequency = 60.0f;
        p.protectThresholdDb = -12.0f;
        BassEngine be;
        prepareBass (be);
        be.setParams (p);
        const float amp = dbfs (-3.0);
        const auto out = runStereo (be, sine (40.0, kFs, settle + measure, amp));
        CHECK_NEAR (be.getProtectionDb(), 9.0, 0.01);
        CHECK_NEAR (levelDb (out.ch[0], settle, measure, 40.0, amp), 0.0, 0.1);
        CHECK_LE (toDb (peakAbs (out.ch[0].data() + settle, measure)), -3.0 + 0.1);

        // Quiet material keeps the full boost ...
        const auto quiet = runStereo (be, sine (40.0, kFs, settle + measure, dbfs (-40.0)));
        CHECK (be.getProtectionDb() == 0.0f);
        const auto shelf = SvfCoeffs::make (FilterType::LowShelf, 60.0, 0.7, 9.0, kFs);
        CHECK_NEAR (levelDb (quiet.ch[0], settle, measure, 40.0, dbfs (-40.0)), shelf.magnitudeDb (40.0, kFs), 0.1);

        // ... and the protection recovers after a loud passage (150 ms release).
        const int n = ms (3000);
        auto x = sine (40.0, kFs, n, dbfs (-3.0));
        for (int i = ms (1000); i < n; ++i)
            x[static_cast<size_t> (i)] *= dbfs (-37.0);
        Planar buf (2, n);
        setChannel (buf, 0, x);
        setChannel (buf, 1, x);
        be.reset();
        float protAtSwitch = 0.0f;
        for (int pos = 0; pos < n; pos += 480)
        {
            be.process (buf.block (pos, std::min (480, n - pos)));
            if (pos + 480 == ms (1000))
                protAtSwitch = be.getProtectionDb();
        }
        CHECK_GE (protAtSwitch, 8.9f);
        CHECK (be.getProtectionDb() == 0.0f);
    }

    // The detector is linked: a loud left channel protects the quiet right one too.
    {
        auto p = allOff();
        p.boostDb = 9.0f;
        p.boostFrequency = 60.0f;
        p.protectThresholdDb = -12.0f;
        BassEngine be;
        prepareBass (be);
        be.setParams (p);
        const auto out = runStereo (be, sine (40.0, kFs, settle + measure, dbfs (-3.0)), sine (40.0, kFs, settle + measure, dbfs (-40.0)));
        CHECK_NEAR (levelDb (out.ch[1], settle, measure, 40.0, dbfs (-40.0)), 0.0, 0.1);
    }
}

TEST_CASE ("BassEngine: harmonics create clear 2nd and 3rd harmonics of a low tone")
{
    const int settle = ms (500), measure = ms (500);
    const float amp = dbfs (-20.0);
    auto p = allOff();
    p.harmonicsAmount = 1.0f;
    p.harmonicsCutoff = 120.0f;
    p.harmonicsCharacter = 0.5f;
    BassEngine be;
    prepareBass (be);
    be.setParams (p);

    const auto out = runStereo (be, sine (50.0, kFs, settle + measure, amp));
    const double fund = levelDb (out.ch[0], settle, measure, 50.0, amp);
    const double h2 = levelDb (out.ch[0], settle, measure, 100.0, amp);
    const double h3 = levelDb (out.ch[0], settle, measure, 150.0, amp);
    CHECK_NEAR (fund, 0.0, 0.5); // the fundamental itself is untouched
    CHECK_GE (h2, -12.0);
    CHECK_GE (h3, -12.0);
    CHECK_LE (std::max (h2, h3), 6.0);
    // Same on the other channel (added equally to all channels).
    CHECK_NEAR (levelDb (out.ch[1], settle, measure, 150.0, amp), h3, 1.0e-3);

    // The harmonic level tracks the fundamental linearly (envelope-normalised).
    const auto quiet = runStereo (be, sine (50.0, kFs, settle + measure, amp * 0.01f));
    CHECK_NEAR (levelDb (quiet.ch[0], settle, measure, 150.0, amp * 0.01), h3, 0.5);
    CHECK_NEAR (levelDb (quiet.ch[0], settle, measure, 100.0, amp * 0.01), h2, 0.5);

    // Nothing is generated at amount 0 (the path is idle).
    p.harmonicsAmount = 0.0f;
    be.setParams (p);
    const auto dry = runStereo (be, sine (50.0, kFs, settle + measure, amp));
    CHECK_LE (levelDb (dry.ch[0], settle, measure, 100.0, amp), -100.0);
    CHECK_LE (levelDb (dry.ch[0], settle, measure, 150.0, amp), -100.0);
}

TEST_CASE ("BassEngine: harmonics character flips the 2nd / 3rd harmonic balance")
{
    const int settle = ms (500), measure = ms (500);
    const float amp = dbfs (-20.0);
    auto ratioDb = [&] (float character)
    {
        auto p = allOff();
        p.harmonicsAmount = 1.0f;
        p.harmonicsCutoff = 120.0f;
        p.harmonicsCharacter = character;
        BassEngine be;
        prepareBass (be);
        be.setParams (p);
        const auto out = runStereo (be, sine (50.0, kFs, settle + measure, amp));
        return levelDb (out.ch[0], settle, measure, 100.0) - levelDb (out.ch[0], settle, measure, 150.0);
    };
    const double even = ratioDb (0.0f);
    const double odd = ratioDb (1.0f);
    CHECK_GE (even, 3.0);  // warm: 2nd harmonic dominates
    CHECK_LE (odd, -3.0);  // punchy: 3rd harmonic dominates
    CHECK_GE (even - odd, 12.0);
}

TEST_CASE ("BassEngine: replace-fundamental removes the original below the cutoff")
{
    const int settle = ms (500), measure = ms (500);
    const float amp = dbfs (-12.0);
    for (float amount : { 0.0f, 1.0f })
    {
        auto p = allOff();
        p.harmonicsAmount = amount;
        p.harmonicsCutoff = 120.0f;
        p.replaceFundamental = true;
        BassEngine be;
        prepareBass (be);
        be.setParams (p);
        const auto out = runStereo (be, sine (50.0, kFs, settle + measure, amp));
        CHECK_LE (levelDb (out.ch[0], settle, measure, 50.0, amp), -12.0);
        if (amount > 0.0f)
            CHECK_GE (levelDb (out.ch[0], settle, measure, 150.0, amp), -12.0); // the harmonics carry the pitch

        // Content well above the cutoff passes.
        if (amount == 0.0f)
        {
            const auto hi = runStereo (be, sine (400.0, kFs, settle + measure, amp));
            CHECK_NEAR (levelDb (hi.ch[0], settle, measure, 400.0, amp), 0.0, 0.1);
        }
    }
}

TEST_CASE ("BassEngine: mono bass cancels antiphase lows and leaves antiphase highs")
{
    const int settle = ms (500), measure = ms (500);
    const float amp = dbfs (-12.0);
    auto p = allOff();
    p.monoBelowHz = 120.0f;
    BassEngine be;
    prepareBass (be);
    be.setParams (p);

    auto antiphase = [&] (double f)
    {
        auto l = sine (f, kFs, settle + measure, amp);
        auto r = l;
        for (auto& v : r)
            v = -v;
        return runStereo (be, l, r);
    };

    const auto low = antiphase (50.0);
    CHECK_LE (levelDb (low.ch[0], settle, measure, 50.0, amp), -20.0);
    CHECK_LE (levelDb (low.ch[1], settle, measure, 50.0, amp), -20.0);

    const auto high = antiphase (1000.0);
    CHECK_NEAR (levelDb (high.ch[0], settle, measure, 1000.0, amp), 0.0, 0.1);
    CHECK_NEAR (levelDb (high.ch[1], settle, measure, 1000.0, amp), 0.0, 0.1);

    // In-phase (already mono) bass is untouched in level.
    const auto mono = runStereo (be, sine (50.0, kFs, settle + measure, amp));
    CHECK_NEAR (levelDb (mono.ch[0], settle, measure, 50.0, amp), 0.0, 0.05);

    // Stereo only: with 3 channels the setting has no effect.
    BassEngine surround;
    prepareBass (surround, kFs, 3);
    surround.setParams (p);
    Planar buf (3, settle + measure);
    setChannel (buf, 0, sine (50.0, kFs, settle + measure, amp));
    setChannel (buf, 1, sine (50.0, kFs, settle + measure, -amp));
    const Planar in = clone (buf);
    surround.reset();
    processInBlocks (surround, buf, 256);
    CHECK (buf.ch == in.ch);
}

TEST_CASE ("BassEngine: subsonic filter removes rumble and keeps the bass")
{
    BassEngine be;
    prepareBass (be);
    be.setParams (BassEngineParams {}); // defaults: subsonic 20 Hz, everything else off
    const int settle = ms (1000), measure = ms (1000);
    const float amp = dbfs (-12.0);
    const auto rumble = runStereo (be, sine (10.0, kFs, settle + measure, amp));
    CHECK_LE (levelDb (rumble.ch[0], settle, measure, 10.0, amp), -20.0);
    const auto bass = runStereo (be, sine (100.0, kFs, settle + measure, amp));
    CHECK_NEAR (levelDb (bass.ch[0], settle, measure, 100.0, amp), 0.0, 0.5);

    // Higher corner, measured against the Butterworth design.
    auto p = BassEngineParams {};
    p.subsonicHz = 40.0f;
    be.setParams (p);
    const auto at40 = runStereo (be, sine (40.0, kFs, settle + measure, amp));
    CHECK_NEAR (levelDb (at40.ch[0], settle, measure, 40.0, amp), -3.01, 0.1);
}

TEST_CASE ("BassEngine: tighten shortens low-frequency decays and leaves highs alone")
{
    const int n = ms (1200);
    auto run = [&] (float tighten, double freq)
    {
        auto p = allOff();
        p.tighten = tighten;
        BassEngine be;
        prepareBass (be);
        be.setParams (p);
        std::vector<float> x (static_cast<size_t> (ms (100)), 0.0f);
        const auto tone = decayingTone (n - ms (100), freq, 0.5f, 250.0);
        x.insert (x.end(), tone.begin(), tone.end());
        return runStereo (be, x).ch[0];
    };
    auto energyDb = [] (const std::vector<float>& y, int from, int to) { return toDb (rms (y.data() + from, to - from)); };

    const auto dry = run (0.0f, 60.0);
    const auto tight = run (1.0f, 60.0);
    const int onset = ms (100);
    CHECK_NEAR (energyDb (tight, onset, onset + ms (25)) - energyDb (dry, onset, onset + ms (25)), 0.0, 1.5);
    CHECK_LE (energyDb (tight, onset + ms (300), onset + ms (900)) - energyDb (dry, onset + ms (300), onset + ms (900)), -6.0);

    const auto dryHigh = run (0.0f, 1000.0);
    const auto tightHigh = run (1.0f, 1000.0);
    CHECK_NEAR (energyDb (tightHigh, onset + ms (300), onset + ms (900)) - energyDb (dryHigh, onset + ms (300), onset + ms (900)), 0.0, 0.3);
}

TEST_CASE ("BassEngine: switching features on and off is click-free")
{
    // Two low tones, different on L and R (so mono bass has work to do).
    const int n = ms (4000);
    auto l = sine (80.0, kFs, n, 0.15f);
    auto r = sine (55.0, kFs, n, 0.15f, 1.0);
    for (size_t i = 0; i < l.size(); ++i)
    {
        l[i] += 0.1f * r[i];
        r[i] -= 0.1f * l[i];
    }

    auto run = [&] (bool toggle)
    {
        BassEngine be;
        prepareBass (be);
        auto p = allOn();
        be.setParams (p);
        be.reset();
        Planar buf (2, n);
        setChannel (buf, 0, l);
        setChannel (buf, 1, r);
        const int block = 240;
        for (int pos = 0, k = 0; pos < n; pos += block, ++k)
        {
            if (toggle && k % 25 == 12) // every 125 ms, mid-note
            {
                const int step = k / 25;
                switch (step % 7)
                {
                    case 0: p.monoBelowHz = p.monoBelowHz > 0.0f ? 0.0f : 150.0f; break;
                    case 1: p.replaceFundamental = ! p.replaceFundamental; break;
                    case 2: p.subsonicHz = p.subsonicHz > 0.0f ? 0.0f : 35.0f; break;
                    case 3: p.tighten = p.tighten > 0.0f ? 0.0f : 1.0f; break;
                    case 4: p.harmonicsAmount = p.harmonicsAmount > 0.0f ? 0.0f : 1.0f; break;
                    case 5: p.boostDb = p.boostDb > 0.0f ? 0.0f : 15.0f; break;
                    default: p.harmonicsCutoff = p.harmonicsCutoff > 100.0f ? 60.0f : 200.0f; break;
                }
                be.setParams (p);
            }
            be.process (buf.block (pos, std::min (block, n - pos)));
        }
        return buf;
    };

    // A click is broadband: measure what lands above 3 kHz (all the program
    // content, harmonics included, is below ~1.2 kHz).
    auto hfPeak = [&] (const Planar& y)
    {
        double peak = 0.0;
        const auto hp = SvfCoeffs::make (FilterType::HighPass, 3000.0, 0.7071, 0.0, kFs);
        for (const auto& c : y.ch)
        {
            SvfState s1, s2;
            for (size_t i = 0; i < c.size(); ++i)
            {
                const float v = svfTick (hp, s2, svfTick (hp, s1, c[i]));
                if (i >= static_cast<size_t> (ms (100))) // skip the start-up of signal and meter
                    peak = std::max (peak, static_cast<double> (std::abs (v)));
            }
        }
        return peak;
    };
    const double steady = hfPeak (run (false));
    const double toggled = hfPeak (run (true));
    CHECK_LE (toggled, 1.0e-3); // -60 dBFS, program at about -14 dBFS
    CHECK_LE (toggled, std::max (4.0 * steady, 2.0e-4));
}

TEST_CASE ("BassEngine: zero latency - an impulse is not delayed")
{
    BassEngine be;
    prepareBass (be);
    CHECK (be.latencySamples() == 0);
    for (const auto& p : { BassEngineParams {}, allOn() })
    {
        be.setParams (p);
        Planar buf (2, 2048);
        buf.ch[0][100] = 1.0e-3f;
        buf.ch[1][100] = 1.0e-3f;
        be.reset();
        processInBlocks (be, buf, 64);
        for (int i = 0; i < 100; ++i)
            CHECK (buf.ch[0][static_cast<size_t> (i)] == 0.0f);
        CHECK (buf.ch[0][100] != 0.0f);
    }
    // The subsonic high-pass passes the impulse's leading edge practically unchanged.
    be.setParams (BassEngineParams {});
    Planar buf (2, 2048);
    buf.ch[0][100] = 1.0e-3f;
    be.reset();
    processInBlocks (be, buf, 64);
    int peakAt = 0;
    for (int i = 1; i < 2048; ++i)
        if (std::abs (buf.ch[0][static_cast<size_t> (i)]) > std::abs (buf.ch[0][static_cast<size_t> (peakAt)]))
            peakAt = i;
    CHECK (peakAt == 100 + be.latencySamples());
    CHECK_NEAR (buf.ch[0][100], 1.0e-3, 1.0e-5);
}

TEST_CASE ("BassEngine: process, reset and setters do not allocate")
{
    BassEngine be;
    prepareBass (be, kFs, 2, 512);
    Planar buf (2, 512);
    setChannel (buf, 0, sine (60.0, kFs, 512, 0.7f));
    setChannel (buf, 1, whiteNoise (512, 0.5f, 3));

    flubtest::AllocationGuard guard;
    be.reset();
    auto p = allOn();
    for (int i = 0; i < 60; ++i)
    {
        p.boostDb = static_cast<float> (i % 16);
        p.boostFrequency = 30.0f + static_cast<float> (i * 7 % 170);
        p.harmonicsAmount = static_cast<float> (i % 3) * 0.5f;
        p.harmonicsCutoff = 40.0f + static_cast<float> (i * 13 % 210);
        p.replaceFundamental = i % 4 < 2;
        p.tighten = static_cast<float> (i % 5) * 0.25f;
        p.monoBelowHz = i % 6 < 3 ? 0.0f : 90.0f;
        p.subsonicHz = i % 7 < 2 ? 0.0f : 30.0f;
        be.setParams (p);
        be.process (buf.block (0, 1 + (i * 97) % 512).firstChannels (1 + i % 2));
        (void) be.getProtectionDb();
        (void) be.getParams();
    }
    CHECK (guard.allocations() == 0);
}

TEST_CASE ("BassEngine: robustness - silence, DC, full-scale noise, impulses, extreme settings, all rates")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    BassEngineParams maxi = allOn();
    maxi.boostDb = 15.0f;
    maxi.boostFrequency = 200.0f;
    maxi.protectThresholdDb = 0.0f;
    maxi.harmonicsAmount = 1.0f;
    maxi.harmonicsCutoff = 250.0f;
    maxi.harmonicsCharacter = 1.0f;
    maxi.tighten = 1.0f;
    maxi.monoBelowHz = 250.0f;
    maxi.subsonicHz = 40.0f;
    BassEngineParams mini = maxi;
    mini.boostFrequency = 30.0f;
    mini.protectThresholdDb = -30.0f;
    mini.harmonicsCutoff = 40.0f;
    mini.harmonicsCharacter = 0.0f;
    mini.monoBelowHz = 40.0f;
    mini.subsonicHz = 10.0f;
    BassEngineParams wild;
    wild.boostDb = inf;
    wild.boostFrequency = -5.0f;
    wild.protectThresholdDb = 1.0e9f;
    wild.harmonicsAmount = 1.0e9f;
    wild.harmonicsCutoff = nan;
    wild.harmonicsCharacter = -inf;
    wild.replaceFundamental = true;
    wild.tighten = inf;
    wild.monoBelowHz = 1.0e9f;
    wild.subsonicHz = 3.0f;
    const BassEngineParams settings[] = { BassEngineParams {}, maxi, mini, wild };

    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const int n = static_cast<int> (fs * 0.4);
        for (const auto& p : settings)
        {
            for (int channels : { 1, 2, 6 })
            {
                BassEngine be;
                prepareBass (be, fs, channels, 480);
                be.setParams (p);
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
                                for (int i = 0; i < n; i += 997)
                                    x[static_cast<size_t> (i)] = 1.0f;
                                break;
                            default: setChannel (buf, c, sine (30.0 + 7.0 * c, fs, n, 1.0f)); break;
                        }
                    }
                    be.reset();
                    processInBlocks (be, buf, 480);
                    // Worst case: +15 dB shelf, +6 dB harmonics, filter overshoot.
                    CHECK (allFinite (buf, 64.0));
                    if (sig == 0)
                        CHECK (peakAbs (buf.ch[0].data(), n) == 0.0);
                    CHECK (std::isfinite (be.getProtectionDb()) && be.getProtectionDb() >= 0.0f && be.getProtectionDb() <= 15.0f);
                }
            }
        }

        // Sanitised parameters.
        BassEngine be;
        prepareBass (be, fs);
        be.setParams (wild);
        const auto& q = be.getParams();
        CHECK (q.boostDb == 15.0f);
        CHECK (q.boostFrequency == 30.0f);
        CHECK (q.protectThresholdDb == 0.0f);
        CHECK (q.harmonicsAmount == 1.0f);
        CHECK (q.harmonicsCutoff == BassEngineParams {}.harmonicsCutoff); // NaN keeps the previous value
        CHECK (q.harmonicsCharacter == 0.0f);
        CHECK (q.tighten == 1.0f);
        CHECK (q.monoBelowHz == 250.0f);
        CHECK (q.subsonicHz == 10.0f);
        CHECK (std::string (be.name()) == "Bass Engine");

        // Random automation every block (sizes 1..700, 1 or 2 channels) on full-scale noise.
        prepareBass (be, fs, 2, 4096);
        FastRandom rng (123);
        auto rnd = [&rng] (float lo, float hi) { return lo + (hi - lo) * 0.5f * (rng.nextBipolar() + 1.0f); };
        Planar buf (2, n);
        setChannel (buf, 0, whiteNoise (n, 1.0f, 5));
        setChannel (buf, 1, whiteNoise (n, 1.0f, 6));
        for (int pos = 0; pos < n;)
        {
            const int len = std::min (n - pos, 1 + static_cast<int> (rng.nextU32() % 700u));
            BassEngineParams r;
            r.boostDb = rnd (0.0f, 15.0f);
            r.boostFrequency = rnd (30.0f, 200.0f);
            r.protectThresholdDb = rnd (-30.0f, 0.0f);
            r.harmonicsAmount = rng.nextU32() % 3u == 0u ? 0.0f : rnd (0.0f, 1.0f);
            r.harmonicsCutoff = rnd (40.0f, 250.0f);
            r.harmonicsCharacter = rnd (0.0f, 1.0f);
            r.replaceFundamental = rng.nextU32() % 2u == 0u;
            r.tighten = rng.nextU32() % 3u == 0u ? 0.0f : rnd (0.0f, 1.0f);
            r.monoBelowHz = rng.nextU32() % 2u == 0u ? 0.0f : rnd (40.0f, 250.0f);
            r.subsonicHz = rng.nextU32() % 2u == 0u ? 0.0f : rnd (10.0f, 40.0f);
            be.setParams (r);
            be.process (buf.block (pos, len).firstChannels (1 + static_cast<int> (rng.nextU32() % 2u)));
            pos += len;
        }
        CHECK (allFinite (buf, 64.0));

        // A NaN / Inf input does not latch.
        be.setParams (allOn());
        Planar poison (2, 64);
        poison.ch[0][10] = nan;
        poison.ch[1][20] = inf;
        be.process (poison.block());
        Planar after (2, n);
        setChannel (after, 0, sine (70.0, fs, n, 0.5f));
        setChannel (after, 1, sine (90.0, fs, n, 0.5f));
        processInBlocks (be, after, 256);
        Planar tail (2, ms (50, fs));
        for (int c = 0; c < 2; ++c)
            std::copy (after.ch[static_cast<size_t> (c)].end() - tail.numSamples(), after.ch[static_cast<size_t> (c)].end(), tail.ch[static_cast<size_t> (c)].begin());
        CHECK (allFinite (tail, 64.0));
        CHECK (peakAbs (tail.ch[0].data(), tail.numSamples()) > 0.01);
    }
}

TEST_CASE ("BassEngine: output is independent of the host block size")
{
    const int n = 3584 * 6; // 3584 = 7 * 512: the parameter change lands on a block edge for every size
    Planar input (2, n);
    {
        const auto noise = whiteNoise (n, 0.2f, 42);
        const auto kick = decayingTone (n, 55.0, 1.0f, 120.0);
        const auto tone = sine (95.0, kFs, n, 0.3f);
        for (int i = 0; i < n; ++i)
        {
            const size_t k = static_cast<size_t> (i);
            const float gate = (i / 2500) % 2 == 0 ? 1.0f : 0.1f;
            input.ch[0][k] = noise[k] + gate * kick[static_cast<size_t> (i % 9000)] + tone[k];
            input.ch[1][k] = 0.5f * noise[k] - gate * kick[static_cast<size_t> (i % 9000)] + 0.2f * tone[k];
        }
    }

    auto run = [&] (int blockSize)
    {
        BassEngine be;
        prepareBass (be, kFs, 2, 512);
        auto p = allOn();
        p.protectThresholdDb = -18.0f; // make the protection work
        be.setParams (p);
        be.reset();
        Planar buf = clone (input);
        for (int pos = 0; pos < n; pos += blockSize)
        {
            if (pos == 7168)
            {
                // glides, stage switches and a new harmonics character mid-stream
                p.boostFrequency = 140.0f;
                p.monoBelowHz = 0.0f;
                p.replaceFundamental = false;
                p.harmonicsCharacter = 0.9f;
                p.harmonicsCutoff = 180.0f;
                p.tighten = 0.0f;
                p.subsonicHz = 40.0f;
                be.setParams (p);
            }
            be.process (buf.block (pos, std::min (blockSize, n - pos)));
        }
        return buf;
    };

    const auto ref = run (512);
    for (int bs : { 1, 7, 64 })
        CHECK_LE (maxAbsDiff (run (bs), ref), 1.0e-6);
}
