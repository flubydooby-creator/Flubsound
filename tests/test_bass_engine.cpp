// Tests for the bass engine: subsonic filter, mono bass, adaptive low shelf
// with headroom protection, psychoacoustic harmonics, replace-fundamental,
// tighten, click-free switching, real-time safety, robustness and
// block-size invariance.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/dsp/BassEngine.h"
#include "flub/dsp/LoudnessMaximizer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

TEST_CASE ("BassEngine: tighten keeps a kick's first 10 ms, still shortens its tail and never lifts a frequency (docs/11 E04 step 2)")
{
    // Kicks (50 Hz + 80 Hz chirp, e^-18t, -6 dBFS) every 500 ms. docs/11
    // E04 Done-when: Tighten 0.5 changes the first 10 ms by >= -0.5 dB
    // (before: -1.69 dB re Tighten 0 - its sustain cut on the onset and the
    // LR4 split's all-pass as the output).
    const int n = ms (4000), period = ms (500);
    std::vector<float> x (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        const double beat = static_cast<double> (i % period) / kFs;
        x[static_cast<size_t> (i)] = static_cast<float> (0.5 * std::exp (-beat * 18.0) * std::sin (kTwoPi * (50.0 + 80.0 * std::exp (-beat * 30.0)) * beat));
    }
    auto run = [&] (float tighten, const std::vector<float>& in)
    {
        auto p = allOff();
        p.tighten = tighten;
        BassEngine be;
        prepareBass (be);
        be.setParams (p);
        return runStereo (be, in).ch[0];
    };
    auto windowDb = [&] (const std::vector<float>& y, const std::vector<float>& ref, int from, int to) {
        double a = 0.0, b = 0.0;
        for (int start = period; start + period <= n; start += period)
            for (int i = start + from; i < start + to; ++i)
            {
                a += static_cast<double> (y[static_cast<size_t> (i)]) * y[static_cast<size_t> (i)];
                b += static_cast<double> (ref[static_cast<size_t> (i)]) * ref[static_cast<size_t> (i)];
            }
        return 10.0 * std::log10 (a / b);
    };
    const auto dry = run (0.0f, x), tight = run (0.5f, x);
    const double onset = windowDb (tight, dry, 0, ms (10)), tail = windowDb (tight, dry, ms (150), ms (300));
    std::printf ("    measured tighten 0.5 on kicks: 0-10 ms %.2f dB, 150-300 ms %.2f dB\n", onset, tail);
    CHECK_GE (onset, -0.5);
    CHECK_LE (tail, -3.0);

    // The shelf x + (g - 1) LP1 (x) with g <= 1 only ever cuts: decaying
    // tones from 40 Hz to 4 kHz never come out louder in any window.
    for (double f : { 40.0, 150.0, 400.0, 1000.0, 4000.0 })
    {
        std::vector<float> tone (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
        {
            const double t = static_cast<double> (i % period) / kFs;
            tone[static_cast<size_t> (i)] = static_cast<float> (0.5 * std::exp (-t / 0.08) * std::sin (kTwoPi * f * t));
        }
        const auto a = run (0.0f, tone), b = run (1.0f, tone);
        for (int from = 0; from < ms (400); from += ms (20))
            CHECK_LE (windowDb (b, a, from, from + ms (20)), 0.02);
    }
}

TEST_CASE ("BassEngine: switching features on and off is click-free")
{
    // A click is broadband: measure what lands above 3 kHz (all the program
    // content, harmonics included, is below ~1.5 kHz).
    auto hfPeak = [] (const Planar& y)
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

    const int n = ms (4000);
    const int block = 240;

    // 1. Linear features (subsonic, mono, shelf / protection, replace, tighten)
    //    toggled every 125 ms, mid-note, on two different low tones on L / R.
    {
        auto l = sine (80.0, kFs, n, 0.15f);
        auto r = sine (55.0, kFs, n, 0.15f, 1.0);
        for (size_t i = 0; i < l.size(); ++i)
        {
            l[i] += 0.1f * r[i];
            r[i] -= 0.1f * l[i];
        }
        BassEngine be;
        prepareBass (be);
        auto p = allOn();
        p.harmonicsAmount = 0.0f;
        be.setParams (p);
        be.reset();
        Planar buf (2, n);
        setChannel (buf, 0, l);
        setChannel (buf, 1, r);
        for (int pos = 0, k = 0; pos < n; pos += block, ++k)
        {
            if (k % 25 == 12)
            {
                switch ((k / 25) % 6)
                {
                    case 0: p.monoBelowHz = p.monoBelowHz > 0.0f ? 0.0f : 150.0f; break;
                    case 1: p.replaceFundamental = ! p.replaceFundamental; break;
                    case 2: p.subsonicHz = p.subsonicHz > 0.0f ? 0.0f : 35.0f; break;
                    case 3: p.tighten = p.tighten > 0.0f ? 0.0f : 1.0f; break;
                    case 4: p.boostDb = p.boostDb > 0.0f ? 0.0f : 15.0f; break;
                    default: p.harmonicsCutoff = p.harmonicsCutoff > 100.0f ? 60.0f : 200.0f; break;
                }
                be.setParams (p);
            }
            be.process (buf.block (pos, std::min (block, n - pos)));
        }
        CHECK_LE (hfPeak (buf), 1.0e-4); // -80 dBFS; program at about -12 dBFS
    }

    // 2. Harmonics amount / cutoff / character toggled on a steady tone with
    //    every other feature on. What remains is the shaper's own brief
    //    clamping while the band level rises (cutoff sweeping up), far below
    //    anything a switching discontinuity would produce.
    {
        const auto x = sine (60.0, kFs, n, 0.25f);
        auto run = [&] (bool toggle)
        {
            BassEngine be;
            prepareBass (be);
            auto p = allOn();
            be.setParams (p);
            be.reset();
            Planar buf (2, n);
            setChannel (buf, 0, x);
            setChannel (buf, 1, x);
            for (int pos = 0, k = 0; pos < n; pos += block, ++k)
            {
                if (toggle && k % 25 == 12)
                {
                    switch ((k / 25) % 3)
                    {
                        case 0: p.harmonicsAmount = p.harmonicsAmount > 0.0f ? 0.0f : 1.0f; break;
                        case 1: p.harmonicsCutoff = p.harmonicsCutoff > 100.0f ? 60.0f : 200.0f; break;
                        default: p.harmonicsCharacter = p.harmonicsCharacter > 0.5f ? 0.0f : 1.0f; break;
                    }
                    be.setParams (p);
                }
                be.process (buf.block (pos, std::min (block, n - pos)));
            }
            return buf;
        };
        CHECK_LE (hfPeak (run (false)), 1.0e-4);
        CHECK_LE (hfPeak (run (true)), 1.0e-3); // -60 dBFS; program at about -6 dBFS
    }
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
        const int n = static_cast<int> (fs * 0.25);
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

// ---- adversarial review tests ----

TEST_CASE ("BassEngine (review): protection telemetry does not flash while the boost is automated")
{
    // Quiet material never needs protection, so the GUI readout must stay at
    // exactly 0 while boostDb moves (the shelf gain lags the boost by its 5 ms
    // smoothing; that lag is not "withdrawn by the protection").
    BassEngine be;
    prepareBass (be);
    auto p = allOff();
    p.boostFrequency = 60.0f;
    be.setParams (p);
    be.reset();
    const int n = ms (1500);
    Planar buf (2, n);
    setChannel (buf, 0, sine (40.0, kFs, n, dbfs (-60.0)));
    setChannel (buf, 1, sine (40.0, kFs, n, dbfs (-60.0)));
    float maxReading = 0.0f;
    for (int pos = 0, k = 0; pos < n; pos += 64, ++k)
    {
        if (k % 150 == 20)
        {
            p.boostDb = p.boostDb > 0.0f ? 0.0f : 15.0f;
            be.setParams (p);
        }
        be.process (buf.block (pos, std::min (64, n - pos)));
        maxReading = std::max (maxReading, be.getProtectionDb());
    }
    CHECK (maxReading == 0.0f);
}

TEST_CASE ("BassEngine (review): the protection cap holds for every shelf frequency and tone")
{
    // Tones just under / over the cap, shelf anywhere in 30..200 Hz: the
    // steady output never exceeds max (cap, input) by more than 0.5 dB.
    const int settle = ms (500), measure = ms (500);
    for (float boostHz : { 30.0f, 80.0f, 150.0f, 200.0f })
    {
        for (double f : { 30.0, 60.0, 120.0, 180.0, 250.0 })
        {
            for (float thr : { 0.0f, -12.0f })
            {
                for (float rel : { -2.0f, 3.0f })
                {
                    auto p = allOff();
                    p.boostDb = 15.0f;
                    p.boostFrequency = boostHz;
                    p.protectThresholdDb = thr;
                    BassEngine be;
                    prepareBass (be);
                    be.setParams (p);
                    const double inDb = thr + rel;
                    const auto out = runStereo (be, sine (f, kFs, settle + measure, dbfs (inDb)));
                    const double outDb = toDb (peakAbs (out.ch[0].data() + settle, measure));
                    CHECK_LE (outDb, std::max (static_cast<double> (thr), inDb) + 0.5);
                    CHECK (be.getProtectionDb() > 0.0f);
                }
            }
        }
    }

    // The spec's case literally: a tone near 0 dBFS, cap 0 dBFS.
    for (double f : { 40.0, 60.0, 100.0 })
    {
        auto p = allOff();
        p.boostDb = 12.0f;
        p.boostFrequency = 60.0f;
        p.protectThresholdDb = 0.0f;
        BassEngine be;
        prepareBass (be);
        be.setParams (p);
        const auto out = runStereo (be, sine (f, kFs, settle + measure, dbfs (-1.0)));
        CHECK_LE (toDb (peakAbs (out.ch[0].data() + settle, measure)), 0.0 + 1.0);
        CHECK_GE (be.getProtectionDb(), 6.0f);
    }
}

TEST_CASE ("BassEngine (review): after loud material the output decays to exact silence without a reset")
{
    for (int channels : { 1, 2, 6 })
    {
        BassEngine be;
        prepareBass (be, kFs, channels);
        be.setParams (allOn());
        be.reset();
        const int loud = ms (500), n = loud + ms (2500);
        Planar buf (channels, n);
        for (int c = 0; c < channels; ++c)
        {
            auto x = sine (40.0 + 13.0 * c, kFs, loud, 0.9f);
            const auto noise = whiteNoise (loud, 0.3f, static_cast<uint32_t> (c + 1));
            for (int i = 0; i < loud; ++i)
                buf.ch[static_cast<size_t> (c)][static_cast<size_t> (i)] = x[static_cast<size_t> (i)] + noise[static_cast<size_t> (i)];
        }
        processInBlocks (be, buf, 256);
        // State hygiene flushes every recursive state: the last 500 ms are exactly 0.
        for (const auto& c : buf.ch)
            CHECK (peakAbs (c.data() + n - ms (500), ms (500)) == 0.0);
    }
}

TEST_CASE ("BassEngine (review): switching everything off lands on a bit-exact pass-through")
{
    BassEngine be;
    prepareBass (be);
    be.setParams (allOn());
    be.reset();
    const int n = ms (700);
    Planar in (2, n);
    setChannel (in, 0, whiteNoise (n, 0.4f, 8));
    setChannel (in, 1, sine (70.0, kFs, n, 0.6f));
    Planar warm = clone (in);
    processInBlocks (be, warm, 128);
    be.setParams (allOff()); // no reset: every stage has to glide / fade out and stop
    for (int round = 0; round < 3; ++round)
    {
        Planar buf = clone (in);
        processInBlocks (be, buf, 128);
        if (round == 2)
            CHECK (buf.ch == in.ch);
    }
    CHECK (be.getProtectionDb() == 0.0f);
}

TEST_CASE ("BassEngine (review): every block size gives bit-identical output")
{
    const int n = 4096 * 5;
    Planar input (2, n);
    {
        const auto noise = whiteNoise (n, 0.3f, 17);
        const auto kick = decayingTone (9000, 50.0, 0.9f, 90.0);
        for (int i = 0; i < n; ++i)
        {
            const size_t k = static_cast<size_t> (i);
            const float hit = kick[static_cast<size_t> (i % 9000)];
            input.ch[0][k] = noise[k] + hit;
            input.ch[1][k] = 0.3f * noise[k] - hit;
        }
    }
    auto run = [&] (int blockSize)
    {
        BassEngine be;
        prepareBass (be, kFs, 2, 4096);
        auto p = allOn();
        p.protectThresholdDb = -20.0f;
        be.setParams (p);
        be.reset();
        Planar buf = clone (input);
        for (int pos = 0; pos < n; pos += blockSize)
        {
            if (pos == 12288) // common boundary of every size below
            {
                p.tighten = 0.0f;
                p.monoBelowHz = 60.0f;
                p.boostFrequency = 190.0f;
                p.harmonicsAmount = 0.0f;
                be.setParams (p);
            }
            be.process (buf.block (pos, std::min (blockSize, n - pos)));
        }
        return buf;
    };
    const auto ref = run (4096);
    for (int bs : { 1, 3, 12, 16, 48, 256, 1024 }) // all divide 12288
        CHECK (maxAbsDiff (run (bs), ref) == 0.0);
}

TEST_CASE ("BassEngine (review): steady bass through protection, tighten, mono and subsonic stays clean")
{
    // Protection in its knee (gain depends on level), tighten on, mono on:
    // every dynamic stage sees a constant level, so no harmonics appear.
    for (double fs : { 44100.0, 48000.0, 192000.0 })
    {
        for (double f : { 30.0, 55.0, 90.0 })
        {
            BassEngine be;
            prepareBass (be, fs);
            auto p = allOff();
            p.subsonicHz = 20.0f;
            p.boostDb = 12.0f;
            p.boostFrequency = 80.0f;
            p.protectThresholdDb = -6.0f;
            p.tighten = 1.0f;
            p.monoBelowHz = 120.0f;
            be.setParams (p);
            const int n = static_cast<int> (fs * 2.0);
            const auto out = runStereo (be, sine (f, fs, n, dbfs (-14.0)));
            const int from = static_cast<int> (fs), len = static_cast<int> (fs);
            const double a1 = toneAmplitude (out.ch[0].data() + from, len, f, fs);
            CHECK (be.getProtectionDb() > 0.5f);
            CHECK (be.getProtectionDb() < 11.5f);
            for (int k = 2; k <= 5; ++k)
                CHECK_LE (toDb (toneAmplitude (out.ch[0].data() + from, len, k * f, fs) / a1), -70.0);
        }
    }
}

//==============================================================================
// docs/11 E02: split-band protection (a) and the subsonic slope.
namespace
{
/** 55 Hz kicks (peak 0.5, tau 100 ms, 350 ms long) every 0.5 + 1/128 s: over
    one kick period a 32 Hz line turns by a quarter cycle, so averaging four
    periods cancels the kick at 32 Hz (lineGainTrack). */
constexpr double kKickPeriod = 0.5 + 1.0 / 128.0;

std::vector<float> lineUnderKicks (double lineAmp, double kickHz, int n)
{
    std::vector<float> x (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kFs, beat = std::fmod (t, kKickPeriod);
        const double kick = beat < 0.35 ? 0.5 * std::exp (-beat / 0.1) * std::sin (kTwoPi * kickHz * beat) : 0.0;
        x[static_cast<size_t> (i)] = static_cast<float> (lineAmp * std::sin (kTwoPi * 32.0 * t) + kick);
    }
    return x;
}

/** The 32 Hz line's level over one kick period (every 0.5 ms, dB): the
    signal demodulated at 32 Hz, averaged over one line cycle and over four
    kick periods from `from` (where the kicks' own 32 Hz content cancels). */
std::vector<double> lineLevelTrack (const std::vector<float>& y, double from)
{
    const int period = static_cast<int> (std::lround (kKickPeriod * kFs)), cycle = static_cast<int> (std::lround (kFs / 32.0));
    const int s0 = static_cast<int> (from * kFs);
    std::vector<double> re (y.size()), im (y.size());
    double accRe = 0.0, accIm = 0.0;
    for (size_t i = 0; i < y.size(); ++i)
    {
        const double a = kTwoPi * 32.0 * static_cast<double> (i) / kFs;
        accRe += y[i] * std::cos (a);
        accIm -= y[i] * std::sin (a);
        if (i >= static_cast<size_t> (cycle))
        {
            const double b = kTwoPi * 32.0 * static_cast<double> (i - static_cast<size_t> (cycle)) / kFs;
            accRe -= y[i - static_cast<size_t> (cycle)] * std::cos (b);
            accIm += y[i - static_cast<size_t> (cycle)] * std::sin (b);
        }
        re[i] = accRe;
        im[i] = accIm;
    }
    std::vector<double> track;
    for (int tau = 0; tau < period; tau += 24)
    {
        double r = 0.0, q = 0.0;
        for (int p = 0; p < 4; ++p)
        {
            r += re[static_cast<size_t> (s0 + p * period + tau)];
            q += im[static_cast<size_t> (s0 + p * period + tau)];
        }
        track.push_back (toDb (std::hypot (r, q) / (4.0 * cycle) * 2.0));
    }
    return track;
}

struct LineResult
{
    double modulationDb = 0.0, meanGainDb = 0.0;
};

/** The line's gain through `p` (the input's own track as reference): its
    peak-to-peak modulation over the kick period and its mean. */
LineResult lineModulation (const BassEngineParams& p, double lineAmp, double kickHz = 55.0, float lookaheadMs = 0.0f)
{
    const int n = static_cast<int> (5.8 * kFs);
    const auto x = lineUnderKicks (lineAmp, kickHz, n);
    BassEngine be;
    be.setLookaheadMs (lookaheadMs);
    prepareBass (be);
    be.setParams (p);
    const auto out = runStereo (be, x);
    // Latency-aligned with the input.
    std::vector<float> aligned (out.ch[0].begin() + be.latencySamples(), out.ch[0].end());
    aligned.resize (out.ch[0].size(), 0.0f);
    const auto in = lineLevelTrack (x, 3.5), got = lineLevelTrack (aligned, 3.5);
    double lo = 1.0e9, hi = -1.0e9, sum = 0.0;
    for (size_t k = 0; k < in.size(); ++k)
    {
        const double g = got[k] - in[k];
        lo = std::min (lo, g);
        hi = std::max (hi, g);
        sum += g;
    }
    return { hi - lo, sum / static_cast<double> (in.size()) };
}

/** Music Bass Head's bass engine (presets/factory/music-bass-head.json). */
BassEngineParams bassHead()
{
    BassEngineParams p;
    p.boostDb = 6.0f;
    p.boostFrequency = 55.0f;
    p.protectThresholdDb = -4.0f;
    p.harmonicsAmount = 0.2f;
    p.harmonicsCutoff = 90.0f;
    p.harmonicsCharacter = 0.35f;
    p.tighten = 0.1f;
    p.monoBelowHz = 110.0f;
    p.subsonicHz = 25.0f;
    return p;
}
} // namespace

TEST_CASE ("BassEngine: split-band protection holds a 32 Hz line steady under 55 Hz kicks at Bass Head's settings (docs/11 E02 (a))")
{
    // The Done-when stimulus: a 32 Hz line (-24 / -18 / -12 dBFS) under 55 Hz
    // kicks at -6 dBFS every ~500 ms. The protection alone (tighten and
    // harmonics off) moved the line with every kick; the split detectors'
    // program-dependent release hold one gain through the pattern. The rest
    // of Bass Head's engine still moves it: tighten (E04) and the harmonics
    // generator (E03) each by about 1 dB, measured below.
    for (const double lineAmp : { 0.0625, 0.125, 0.25 })
    {
        auto protectionOnly = bassHead();
        protectionOnly.tighten = 0.0f;
        protectionOnly.harmonicsAmount = 0.0f;
        auto split = protectionOnly;
        split.splitProtection = true;
        const auto before = lineModulation (protectionOnly, lineAmp), after = lineModulation (split, lineAmp);
        auto head = bassHead();
        const auto headBefore = lineModulation (head, lineAmp);
        head.splitProtection = true;
        const auto headAfter = lineModulation (head, lineAmp);
        std::printf ("    measured 32 Hz line at %.1f dBFS: protection alone %.2f -> %.2f dB modulation (mean gain %+.2f -> %+.2f dB); "
                     "Bass Head's engine %.2f -> %.2f dB\n",
                     toDb (lineAmp), before.modulationDb, after.modulationDb, before.meanGainDb, after.meanGainDb, headBefore.modulationDb,
                     headAfter.modulationDb);
        CHECK_GE (before.modulationDb, 3.0); // the stimulus does pump the classic protection
        CHECK_LE (after.modulationDb, 0.2);
        CHECK_LE (headAfter.modulationDb, headBefore.modulationDb - 1.0);
    }
}

TEST_CASE ("BassEngine: split-band protection - kicks above 60 Hz leave the sub boost, an isolated hit releases as before, and the cap holds for every shelf and tone (docs/11 E02 (a))")
{
    // 100 Hz kicks over the 32 Hz line with a 55 Hz shelf: the classic
    // detector predicts the full boost for the kick and withdraws the whole
    // shelf with each one; the split one takes the kick's excess in the punch
    // band, so the line keeps about the classic's mean boost, steadily.
    {
        auto p = bassHead();
        p.tighten = 0.0f;
        p.harmonicsAmount = 0.0f;
        const auto classic = lineModulation (p, 0.125, 100.0);
        p.splitProtection = true;
        const auto split = lineModulation (p, 0.125, 100.0);
        std::printf ("    measured 32 Hz line under 100 Hz kicks: modulation %.2f -> %.2f dB, mean gain %+.2f -> %+.2f dB\n", classic.modulationDb,
                     split.modulationDb, classic.meanGainDb, split.meanGainDb);
        CHECK_LE (split.modulationDb, 0.5);
        CHECK_GE (split.meanGainDb, classic.meanGainDb - 0.5); // steady at about the classic mean (55 Hz kicks cost 2.5 - 3.3 dB)
    }

    // One loud 40 Hz burst, then quiet: no pattern, so no hold; the boost
    // returns as fast as the classic protection's.
    {
        const int n = ms (2000);
        auto x = sine (40.0, kFs, n, dbfs (-3.0));
        for (int i = ms (300); i < n; ++i)
            x[static_cast<size_t> (i)] *= dbfs (-37.0);
        float released[2] {};
        for (int mode = 0; mode < 2; ++mode)
        {
            auto p = allOff();
            p.boostDb = 9.0f;
            p.boostFrequency = 60.0f;
            p.protectThresholdDb = -12.0f;
            p.splitProtection = mode == 1;
            BassEngine be;
            prepareBass (be);
            be.setParams (p);
            Planar buf (2, n);
            setChannel (buf, 0, x);
            setChannel (buf, 1, x);
            float atBurst = 0.0f;
            for (int pos = 0; pos < n; pos += 480)
            {
                be.process (buf.block (pos, std::min (480, n - pos)));
                if (pos + 480 == ms (300))
                    atBurst = be.getProtectionDb();
                if (pos + 480 == ms (800))
                    released[mode] = be.getProtectionDb();
            }
            CHECK_GE (atBurst, 8.0f);
        }
        CHECK_LE (released[1], released[0] + 0.1f);
        CHECK_LE (released[1], 0.5f);
    }

    // The cap: steady tones just under / over it, shelf anywhere in
    // 30 .. 200 Hz, tones across the sub / punch split: never more than
    // 0.5 dB over max (cap, input), as the classic protection.
    const int settle = ms (500), measure = ms (400);
    for (float boostHz : { 30.0f, 80.0f, 150.0f, 200.0f })
        for (double f : { 30.0, 55.0, 70.0, 80.0, 120.0, 180.0 })
            for (float thr : { 0.0f, -12.0f })
                for (float rel : { -2.0f, 3.0f })
                {
                    auto p = allOff();
                    p.boostDb = 15.0f;
                    p.boostFrequency = boostHz;
                    p.protectThresholdDb = thr;
                    p.splitProtection = true;
                    BassEngine be;
                    prepareBass (be);
                    be.setParams (p);
                    const double inDb = thr + rel;
                    const auto out = runStereo (be, sine (f, kFs, settle + measure, dbfs (inDb)));
                    CHECK_LE (toDb (peakAbs (out.ch[0].data() + settle, measure)), std::max (static_cast<double> (thr), inDb) + 0.5);
                    CHECK (be.getProtectionDb() > 0.0f);
                }
}

TEST_CASE ("BassEngine: the 2nd-order subsonic filter keeps 28 Hz within 3 dB and halves the group delay at 40 Hz (docs/11 E02 subsonic slice)")
{
    // Group delay at 40 Hz from the phase of 39 / 41 Hz tones (a plain
    // delay reads its length). HP4 at 25 Hz (Bass Head's) is the 8.2 ms of
    // docs/11 E02; HP4 at 28 / 30 Hz (the gaming presets') lose 3.0 / 4.4 dB
    // at 28 Hz.
    const auto measure = [] (float hz, int order) {
        auto p = allOff();
        p.subsonicHz = hz;
        p.subsonicOrder = order;
        const int n = ms (1500), from = ms (1000), len = ms (500);
        double phase[2] {};
        for (int k = 0; k < 2; ++k)
        {
            const double f = k == 0 ? 39.0 : 41.0;
            BassEngine be;
            prepareBass (be);
            be.setParams (p);
            const auto x = sine (f, kFs, n, 0.1f);
            const auto y = runStereo (be, x);
            double re = 0.0, im = 0.0;
            for (int i = from; i < from + len; ++i)
            {
                re += y.ch[0][static_cast<size_t> (i)] * std::cos (kTwoPi * f * i / kFs);
                im -= y.ch[0][static_cast<size_t> (i)] * std::sin (kTwoPi * f * i / kFs);
            }
            phase[k] = std::atan2 (im, re) + 0.5 * kPi; // re the input sine
        }
        const double dphi = std::remainder (phase[1] - phase[0], kTwoPi);
        BassEngine be;
        prepareBass (be);
        be.setParams (p);
        const auto y28 = runStereo (be, sine (28.0, kFs, n, 0.1f));
        return std::pair { -dphi / (kTwoPi * 2.0) * 1000.0, levelDb (y28.ch[0], from, len, 28.0, 0.1) };
    };
    const auto [gd4At25, at28For4At25] = measure (25.0f, 4);
    const auto [gd4At20, at28For4At20] = measure (20.0f, 4);
    const auto [gd2At20, at28For2At20] = measure (20.0f, 2);
    const auto [gd4At30, at28For4At30] = measure (30.0f, 4);
    std::printf ("    measured 40 Hz group delay: HP4 25 Hz %.2f ms, HP4 20 Hz %.2f ms, HP2 20 Hz %.2f ms; 28 Hz: HP4 30 Hz %.2f dB, HP2 20 Hz %.2f dB\n",
                 gd4At25, gd4At20, gd2At20, at28For4At30, at28For2At20);
    CHECK_NEAR (gd4At25, 8.2, 0.3);
    CHECK_NEAR (gd2At20, 3.3, 0.3);
    CHECK_LE (gd2At20, 0.5 * gd4At25);
    CHECK_GE (at28For2At20, -3.0);
    CHECK_LE (at28For4At30, -3.0); // what the gaming presets' 30 Hz HP4 does
    CHECK_GE (at28For4At20, -0.5);
    (void) at28For4At25;
}

TEST_CASE ("BassEngine: switching split-band protection and the subsonic slope is click-free and every block size gives the same output (docs/11 E02)")
{
    // A 32 Hz line under kicks (the protection working) with a 25 Hz
    // rumble: split protection on / off and the slope 4 / 2 toggle every
    // 300 ms. No output step beyond the steady signal's, and bit-identical
    // output for every host block size.
    const int n = ms (2400);
    auto x = lineUnderKicks (0.125, 55.0, n);
    const auto rumble = sine (25.0, kFs, n, 0.1f);
    for (size_t i = 0; i < x.size(); ++i)
        x[i] += rumble[i];
    std::vector<Planar> outs;
    for (int blockSize : { 480, 1, 37 })
    {
        auto p = bassHead();
        p.subsonicHz = 20.0f;
        BassEngine be;
        prepareBass (be);
        be.setParams (p);
        Planar buf (2, n);
        setChannel (buf, 0, x);
        setChannel (buf, 1, x);
        for (int pos = 0; pos < n;)
        {
            // Changes land at the same sample for every block size.
            const int step = pos / ms (300), nextChange = (step + 1) * ms (300);
            p.splitProtection = step % 2 == 1;
            p.subsonicOrder = (step / 2) % 2 == 1 ? 2 : 4;
            be.setParams (p);
            const int len = std::min ({ blockSize, n - pos, nextChange - pos });
            be.process (buf.block (pos, len));
            pos += len;
        }
        CHECK (allFinite (buf, 4.0));
        outs.push_back (clone (buf));
    }
    for (size_t k = 1; k < outs.size(); ++k)
        CHECK (maxAbsDiff (outs[0], outs[k]) == 0.0);

    // Steps: the largest sample-to-sample change stays that of a run
    // without toggles (the kicks' own attack is the largest step).
    auto p = bassHead();
    p.subsonicHz = 20.0f;
    BassEngine steady;
    prepareBass (steady);
    steady.setParams (p);
    const auto ref = runStereo (steady, x);
    double maxStep = 0.0, refStep = 0.0;
    for (int i = 1; i < n; ++i)
    {
        maxStep = std::max (maxStep, static_cast<double> (std::abs (outs[0].ch[0][static_cast<size_t> (i)] - outs[0].ch[0][static_cast<size_t> (i - 1)])));
        refStep = std::max (refStep, static_cast<double> (std::abs (ref.ch[0][static_cast<size_t> (i)] - ref.ch[0][static_cast<size_t> (i - 1)])));
    }
    CHECK_LE (maxStep, 1.1 * refStep);
}

// =============================================================================
// Split-band protection look-ahead (docs/11 E02 (a): Quality only, 2 ms)
// =============================================================================
namespace
{
struct OnsetResult
{
    double onsetPeakDb = 0.0, onsetGrDb = 0.0, laterGrDb = 0.0;
};

/** x (from 1 s on) through the bass engine at boost / corner / cap, then
    the maximizer as the Quality profile runs it (2 ms look-ahead, true-peak
    detection, drive 0, ceiling -1 dBTP): the bass output's peak and the
    limiter's deepest gain reduction over the onset's first 50 ms, and its
    deepest reduction over 200 - 400 ms. */
OnsetResult onsetThroughMaximizer (const std::vector<float>& x, bool split, float lookaheadMs, float boostDb, float boostHz, float capDb)
{
    constexpr int block = 32;
    const int n = static_cast<int> (x.size()), onset = ms (1000);
    BassEngine be;
    be.setLookaheadMs (lookaheadMs);
    prepareBass (be, kFs, 2, block);
    auto p = allOff();
    p.boostDb = boostDb;
    p.boostFrequency = boostHz;
    p.protectThresholdDb = capDb;
    p.subsonicHz = 20.0f;
    p.subsonicOrder = 2;
    p.splitProtection = split;
    be.setParams (p);
    be.reset();
    LoudnessMaximizer mx;
    mx.setLookaheadMs (2.0f);
    mx.setTruePeakDetection (true);
    mx.prepare ({ kFs, block, 2 });
    mx.setParams (MaximizerParams {});
    mx.reset();

    Planar buf (2, n);
    setChannel (buf, 0, x);
    setChannel (buf, 1, x);
    OnsetResult r;
    double peak = 0.0;
    const int bassLatency = be.latencySamples(), latency = bassLatency + mx.latencySamples();
    for (int pos = 0; pos + block <= n; pos += block)
    {
        const auto b = buf.block (pos, block);
        be.process (b);
        for (int i = 0; i < block; ++i)
            if (const int t = pos + i - bassLatency - onset; t >= 0 && t < ms (50))
                peak = std::max (peak, static_cast<double> (std::abs (buf.ch[0][static_cast<size_t> (pos + i)])));
        mx.process (b);
        const int newest = pos + block - 1 - latency - onset; // the latest input sample the limiter has seen
        const double gr = -mx.getGainReductionDb();
        if (newest >= 0 && newest < ms (50))
            r.onsetGrDb = std::max (r.onsetGrDb, gr);
        if (newest >= ms (200) && newest < ms (400))
            r.laterGrDb = std::max (r.laterGrDb, gr);
    }
    r.onsetPeakDb = toDb (peak);
    return r;
}
} // namespace

TEST_CASE ("BassEngine: the split-protection look-ahead meets a sudden bass onset with its withdrawal - the maximizer's onset GR >= 2 dB lower (docs/11 E02 (a))")
{
    // docs/03 4.3.3's onset: a 40 Hz tone at -6 dBFS starts at 1 s into
    // +12 dB at 70 Hz with the cap at 0 dBFS. The classic detector's 10 ms
    // attack lets it peak at +3.7 .. +4 dBFS for 12 - 40 ms (docs/03 4.9);
    // with the maximizer on that is limiter gain reduction. The split
    // detectors with Quality's 2 ms look-ahead (1 ms attack, withdrawals
    // not smoothed) have the boost withdrawn when the onset arrives.
    // Classic / split alone / split with look-ahead: onset GR 4.58 / 4.72 /
    // 0.69 dB, onset peak +3.89 / +3.98 / -0.36 dBFS.
    const int n = ms (1500);
    std::vector<float> tone (static_cast<size_t> (n), 0.0f);
    for (int i = ms (1000); i < n; ++i)
        tone[static_cast<size_t> (i)] = static_cast<float> (dbfs (-6.0) * std::sin (kTwoPi * 40.0 * (i - ms (1000)) / kFs));
    const auto classic = onsetThroughMaximizer (tone, false, 0.0f, 12.0f, 70.0f, 0.0f);
    const auto splitOnly = onsetThroughMaximizer (tone, true, 0.0f, 12.0f, 70.0f, 0.0f);
    const auto ahead = onsetThroughMaximizer (tone, true, 2.0f, 12.0f, 70.0f, 0.0f);
    const auto classicAhead = onsetThroughMaximizer (tone, false, 2.0f, 12.0f, 70.0f, 0.0f);
    std::printf ("    measured 40 Hz onset, maximizer on: onset GR classic %.2f / split %.2f / split + 2 ms look-ahead %.2f dB "
                 "(onset peak %+.2f / %+.2f / %+.2f dBFS); GR at 200 - 400 ms %.2f / %.2f / %.2f dB\n",
                 classic.onsetGrDb, splitOnly.onsetGrDb, ahead.onsetGrDb, classic.onsetPeakDb, splitOnly.onsetPeakDb, ahead.onsetPeakDb,
                 classic.laterGrDb, splitOnly.laterGrDb, ahead.laterGrDb);
    CHECK_GE (classic.onsetGrDb, 3.0); // the stimulus does overshoot
    CHECK_LE (ahead.onsetGrDb, classic.onsetGrDb - 2.0);
    CHECK_LE (ahead.onsetPeakDb, 0.5); // the cap, within the protection's steady tolerance
    CHECK_LE (ahead.laterGrDb, classic.laterGrDb);
    // The classic protection ignores the look-ahead: its timing is kept.
    CHECK_NEAR (classicAhead.onsetGrDb, classic.onsetGrDb, 0.05);
    CHECK_NEAR (classicAhead.onsetPeakDb, classic.onsetPeakDb, 0.05);

    // An explosion (as tests/test_scenes.cpp: 45 Hz + low-passed noise, tau
    // 350 ms) at -6 dBFS, same settings: the noise's own peaks are not
    // predictable from the level; classic 1.43 dB, split with look-ahead 0.
    std::vector<float> boom (static_cast<size_t> (n), 0.0f);
    {
        auto rumble = whiteNoise (n, 1.0f, 9753);
        for (int pass = 0; pass < 2; ++pass)
        {
            const double a = std::exp (-kTwoPi * 150.0 / kFs);
            double z = 0.0;
            for (auto& v : rumble)
                v = static_cast<float> (z = (1.0 - a) * v + a * z);
        }
        double power = 0.0;
        for (float v : rumble)
            power += static_cast<double> (v) * v;
        const double rumbleRms = std::sqrt (power / n);
        for (int i = ms (1000); i < n; ++i)
        {
            const double t = (i - ms (1000)) / kFs;
            boom[static_cast<size_t> (i)] = static_cast<float> (dbfs (-6.0) * std::exp (-t / 0.35)
                                                                * (0.7 * std::sin (kTwoPi * 45.0 * t) + 0.3 * rumble[static_cast<size_t> (i)] / rumbleRms));
        }
    }
    const auto boomClassic = onsetThroughMaximizer (boom, false, 0.0f, 12.0f, 70.0f, 0.0f);
    const auto boomAhead = onsetThroughMaximizer (boom, true, 2.0f, 12.0f, 70.0f, 0.0f);
    std::printf ("    measured explosion onset, maximizer on: onset GR classic %.2f / split + look-ahead %.2f dB (onset peak %+.2f / %+.2f dBFS)\n",
                 boomClassic.onsetGrDb, boomAhead.onsetGrDb, boomClassic.onsetPeakDb, boomAhead.onsetPeakDb);
    CHECK_LE (boomAhead.onsetGrDb, boomClassic.onsetGrDb - 1.0);
}

TEST_CASE ("BassEngine: the look-ahead is the latency, keeps the line steady and the cap, switches click-free and is block-size independent (docs/11 E02 (a))")
{
    // Latency: 2 ms in samples at each rate; the default is 0.
    for (const double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        BassEngine be;
        be.setLookaheadMs (2.0f);
        prepareBass (be, fs);
        CHECK (be.latencySamples() == static_cast<int> (std::lround (0.002 * fs)));
    }

    // With the split detectors off it only delays: the protection, mono,
    // subsonic and shelf, 96 samples later, within -80 dBFS (-93.9 dBFS
    // measured: the peak holds' bucket grid does not move with the delay,
    // so a held peak can close a few samples earlier or later).
    {
        const int n = ms (1500);
        auto x = whiteNoise (n, 0.3f, 77);
        const auto lf = sine (45.0, kFs, n, 0.5f);
        for (size_t i = 0; i < x.size(); ++i)
            x[i] += lf[i] * (i > static_cast<size_t> (ms (700)) ? 1.0f : 0.1f);
        auto p = allOff();
        p.boostDb = 9.0f;
        p.boostFrequency = 60.0f;
        p.protectThresholdDb = -12.0f;
        p.monoBelowHz = 100.0f;
        p.subsonicHz = 20.0f;
        BassEngine plain, ahead;
        ahead.setLookaheadMs (2.0f);
        prepareBass (plain);
        prepareBass (ahead);
        plain.setParams (p);
        ahead.setParams (p);
        const auto a = runStereo (plain, x, whiteNoise (n, 0.3f, 78)), b = runStereo (ahead, x, whiteNoise (n, 0.3f, 78));
        double diff = 0.0;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i + 96 < n; ++i)
                diff = std::max (diff, static_cast<double> (std::abs (a.ch[static_cast<size_t> (c)][static_cast<size_t> (i)]
                                                                      - b.ch[static_cast<size_t> (c)][static_cast<size_t> (i + 96)])));
        std::printf ("    measured look-ahead vs none, split off, 96 samples apart: largest difference %.1f dBFS\n", toDb (diff));
        CHECK_LE (diff, 1.0e-4);
        CHECK (plain.getProtectionDb() > 1.0f);
    }

    // The line under kicks stays as steady as without the look-ahead.
    for (const double lineAmp : { 0.0625, 0.125, 0.25 })
    {
        auto p = bassHead();
        p.tighten = 0.0f;
        p.harmonicsAmount = 0.0f;
        p.splitProtection = true;
        const auto without = lineModulation (p, lineAmp), with = lineModulation (p, lineAmp, 55.0, 2.0f);
        std::printf ("    measured 32 Hz line at %.1f dBFS, protection alone, split: %.2f dB modulation (mean %+.2f dB); with 2 ms look-ahead %.2f dB (mean %+.2f dB)\n",
                     toDb (lineAmp), without.modulationDb, without.meanGainDb, with.modulationDb, with.meanGainDb);
        CHECK_LE (with.modulationDb, 0.2);
        CHECK_NEAR (with.meanGainDb, without.meanGainDb, 1.0);
    }

    // The cap for steady tones, as without the look-ahead.
    const int settle = ms (500), measure = ms (400);
    for (float boostHz : { 30.0f, 80.0f, 200.0f })
        for (double f : { 30.0, 55.0, 80.0, 120.0 })
            for (float rel : { -2.0f, 3.0f })
            {
                auto p = allOff();
                p.boostDb = 15.0f;
                p.boostFrequency = boostHz;
                p.protectThresholdDb = -6.0f;
                p.splitProtection = true;
                BassEngine be;
                be.setLookaheadMs (2.0f);
                prepareBass (be);
                be.setParams (p);
                const double inDb = -6.0 + rel;
                const auto out = runStereo (be, sine (f, kFs, settle + measure, dbfs (inDb)));
                CHECK_LE (toDb (peakAbs (out.ch[0].data() + settle, measure)), std::max (-6.0, inDb) + 0.5);
            }

    // Split protection toggled every 300 ms under kicks: bit-identical for
    // every block size, no step beyond the kicks' own.
    const int n = ms (2400);
    const auto x = lineUnderKicks (0.125, 55.0, n);
    std::vector<Planar> outs;
    for (int blockSize : { 480, 1, 37 })
    {
        auto p = bassHead();
        BassEngine be;
        be.setLookaheadMs (2.0f);
        prepareBass (be);
        be.setParams (p);
        Planar buf (2, n);
        setChannel (buf, 0, x);
        setChannel (buf, 1, x);
        for (int pos = 0; pos < n;)
        {
            const int step = pos / ms (300), nextChange = (step + 1) * ms (300);
            p.splitProtection = step % 2 == 1;
            be.setParams (p);
            const int len = std::min ({ blockSize, n - pos, nextChange - pos });
            be.process (buf.block (pos, len));
            pos += len;
        }
        CHECK (allFinite (buf, 4.0));
        outs.push_back (clone (buf));
    }
    for (size_t k = 1; k < outs.size(); ++k)
        CHECK (maxAbsDiff (outs[0], outs[k]) == 0.0);
    BassEngine steady;
    steady.setLookaheadMs (2.0f);
    prepareBass (steady);
    steady.setParams (bassHead());
    const auto ref = runStereo (steady, x);
    double maxStep = 0.0, refStep = 0.0;
    for (int i = 1; i < n; ++i)
    {
        maxStep = std::max (maxStep, static_cast<double> (std::abs (outs[0].ch[0][static_cast<size_t> (i)] - outs[0].ch[0][static_cast<size_t> (i - 1)])));
        refStep = std::max (refStep, static_cast<double> (std::abs (ref.ch[0][static_cast<size_t> (i)] - ref.ch[0][static_cast<size_t> (i - 1)])));
    }
    CHECK_LE (maxStep, 1.1 * refStep);

    // Real-time safety with the delay line.
    {
        BassEngine be;
        be.setLookaheadMs (2.0f);
        prepareBass (be, kFs, 2, 512);
        Planar buf (2, 512);
        setChannel (buf, 0, sine (60.0, kFs, 512, 0.7f));
        setChannel (buf, 1, whiteNoise (512, 0.5f, 3));
        flubtest::AllocationGuard guard;
        be.reset();
        auto q = allOn();
        for (int i = 0; i < 40; ++i)
        {
            q.splitProtection = i % 3 != 0;
            q.boostDb = static_cast<float> (i % 16);
            be.setParams (q);
            be.process (buf.block (0, 1 + (i * 97) % 512).firstChannels (1 + i % 2));
        }
        CHECK (guard.allocations() == 0);
    }
}
