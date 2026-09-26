// Tests for the mono-compatible stereo widener / spatializer (width shelf on
// S, positional-focus bell, all-pass "space", crossfeed, auto mono safety).
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/dsp/StereoSpatializer.h"
#include "flub/dsp/Svf.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <vector>

using namespace flub;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;
constexpr double kRates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };

/** Every stage neutral and the mono safety off (it would react to the
    antiphase test tones otherwise). */
SpatializerParams neutral()
{
    SpatializerParams p;
    p.autoMonoSafety = false;
    return p;
}

/** Prepares, applies params and resets (no glide from the defaults). */
void setUp (StereoSpatializer& sp, const SpatializerParams& p, double fs = kFs, int maxBlock = 512, int channels = 2)
{
    sp.setParams (p);
    sp.prepare ({ fs, maxBlock, channels });
    sp.reset();
}

/** Stereo planar buffer from two signals (copied: Planar keeps raw pointers). */
Planar stereo (const std::vector<float>& l, const std::vector<float>& r)
{
    Planar buf (2, static_cast<int> (l.size()));
    std::copy (l.begin(), l.end(), buf.ch[0].begin());
    std::copy (r.begin(), r.end(), buf.ch[1].begin());
    return buf;
}

std::vector<float> scaled (const std::vector<float>& x, float g)
{
    std::vector<float> y (x);
    for (auto& v : y)
        v *= g;
    return y;
}

std::vector<float> mix (const std::vector<float>& a, float ga, const std::vector<float>& b, float gb)
{
    std::vector<float> y (a.size());
    for (size_t i = 0; i < a.size(); ++i)
        y[i] = ga * a[i] + gb * b[i];
    return y;
}

std::vector<float> sideOf (const Planar& buf, int start, int length)
{
    std::vector<float> s (static_cast<size_t> (length));
    for (int i = 0; i < length; ++i)
        s[static_cast<size_t> (i)] = 0.5f * (buf.ch[0][static_cast<size_t> (start + i)] - buf.ch[1][static_cast<size_t> (start + i)]);
    return s;
}

std::vector<float> midOf (const Planar& buf, int start, int length)
{
    std::vector<float> m (static_cast<size_t> (length));
    for (int i = 0; i < length; ++i)
        m[static_cast<size_t> (i)] = 0.5f * (buf.ch[0][static_cast<size_t> (start + i)] + buf.ch[1][static_cast<size_t> (start + i)]);
    return m;
}

/** Steady-state gain (dB) of the side path for a pure side sine (L = x, R = -x),
    measured over a whole number of periods. */
double sideGainDb (const SpatializerParams& p, double freq, double fs = kFs)
{
    StereoSpatializer sp;
    setUp (sp, p, fs);
    const int settle = static_cast<int> (fs * 0.25);
    const int periods = std::max (1, static_cast<int> (std::floor (0.5 * freq)));
    const int measure = static_cast<int> (std::lround (periods * fs / freq));
    const int total = settle + measure;
    const float amp = 0.25f;
    const auto x = sine (freq, fs, total, amp);
    Planar buf = stereo (x, scaled (x, -1.0f));
    processInBlocks (sp, buf, 256);
    const auto s = sideOf (buf, settle, measure);
    return toDb (toneAmplitude (s.data(), measure, freq, fs) / amp);
}

double maxMonoSumError (const Planar& in, const Planar& out)
{
    double err = 0.0;
    for (size_t i = 0; i < in.ch[0].size(); ++i)
    {
        const double a = static_cast<double> (in.ch[0][i]) + static_cast<double> (in.ch[1][i]);
        const double b = static_cast<double> (out.ch[0][i]) + static_cast<double> (out.ch[1][i]);
        err = std::max (err, std::abs (a - b));
    }
    return err;
}

bool allFinite (const Planar& buf)
{
    for (const auto& c : buf.ch)
        for (float v : c)
            if (! std::isfinite (v))
                return false;
    return true;
}

bool identical (const Planar& a, const Planar& b)
{
    return a.ch == b.ch;
}

double maxAbsDiff (const Planar& a, const Planar& b)
{
    double d = 0.0;
    for (size_t c = 0; c < a.ch.size(); ++c)
        for (size_t i = 0; i < a.ch[c].size(); ++i)
            d = std::max (d, std::abs (static_cast<double> (a.ch[c][i]) - static_cast<double> (b.ch[c][i])));
    return d;
}

/** Largest |second difference| of x over [start, start + length). A sine of
    amplitude A has at most A w^2 (w in rad / sample); a step (click) of size
    d adds d, so this is far more sensitive than the first difference. */
double maxCurvature (const std::vector<float>& x, int start, int length)
{
    double m = 0.0;
    for (int i = std::max (2, start); i < start + length; ++i)
    {
        const auto k = static_cast<size_t> (i);
        m = std::max (m, std::abs (static_cast<double> (x[k]) - 2.0 * x[k - 1] + static_cast<double> (x[k - 2])));
    }
    return m;
}

/** Mono noise with a music-like (falling) spectrum: one-pole low-pass ~800 Hz. */
std::vector<float> darkNoise (int n, float amplitude, uint32_t seed)
{
    auto x = whiteNoise (n, amplitude, seed);
    float y = 0.0f;
    for (auto& v : x)
    {
        y += 0.1f * (v - y);
        v = 3.0f * y;
    }
    return x;
}

double rmsDb (const std::vector<float>& x) { return toDb (rms (x.data(), static_cast<int> (x.size()))); }

/** Crossfeed side response, written independently of the implementation:
    1 - 0.6 c / (1 + s / wc) with the bilinear (prewarped) frequency mapping. */
double crossfeedDb (double c, double freq, double fs)
{
    const double ratio = std::tan (kPi * freq / fs) / std::tan (kPi * 700.0 / fs);
    const std::complex<double> lp = 1.0 / std::complex<double> (1.0, ratio);
    return 20.0 * std::log10 (std::abs (1.0 - 0.6 * c * lp));
}
} // namespace

//==============================================================================
TEST_CASE ("StereoSpatializer: L'+R' == L+R for random stereo noise under random settings")
{
    FastRandom rng (0xC0FFEEu);
    const auto rnd = [&rng] (float lo, float hi) { return lo + (hi - lo) * 0.5f * (rng.nextBipolar() + 1.0f); };
    const auto randomParams = [&] {
        SpatializerParams p;
        p.width = rnd (0.0f, 2.0f);
        p.widthLowCutHz = rnd (60.0f, 500.0f);
        p.positionalFocus = rnd (0.0f, 1.0f);
        p.space = rnd (0.0f, 1.0f);
        p.crossfeed = rnd (0.0f, 1.0f);
        p.autoMonoSafety = rng.nextBipolar() > 0.0f;
        p.minCorrelation = rnd (-1.0f, 1.0f);
        return p;
    };

    double worst = 0.0;
    bool finite = true;
    for (int trial = 0; trial < 40; ++trial)
    {
        const double fs = kRates[trial % 4];
        const int n = 12000;
        // Random blend of common, independent and antiphase content.
        const auto a = whiteNoise (n, 1.0f, 11u + static_cast<uint32_t> (trial));
        const auto b = whiteNoise (n, 1.0f, 911u + static_cast<uint32_t> (trial));
        const float common = rnd (0.0f, 0.6f), indep = rnd (0.0f, 0.4f), anti = rnd (0.0f, 0.6f);
        std::vector<float> l (static_cast<size_t> (n)), r (static_cast<size_t> (n));
        for (size_t i = 0; i < l.size(); ++i)
        {
            l[i] = common * a[i] + anti * b[i] + indep * a[(i * 7) % l.size()];
            r[i] = common * a[i] - anti * b[i] + indep * b[(i * 13) % l.size()];
        }
        const Planar in = stereo (l, r);
        Planar out = stereo (l, r);

        StereoSpatializer sp;
        setUp (sp, randomParams(), fs);
        // Random block sizes and parameter changes mid-stream (glides active).
        int pos = 0;
        while (pos < n)
        {
            const int len = std::min (n - pos, 1 + static_cast<int> (rng.nextU32() % 512u));
            if (rng.nextBipolar() > 0.6f)
                sp.setParams (randomParams());
            sp.process (out.block (pos, len));
            pos += len;
        }
        worst = std::max (worst, maxMonoSumError (in, out));
        finite = finite && allFinite (out);
    }
    CHECK (finite);
    CHECK_LE (worst, 1.0e-5);
}

TEST_CASE ("StereoSpatializer: width 1 with everything else neutral is a bit-exact pass-through")
{
    const int n = 24000;
    const auto a = whiteNoise (n, 0.8f, 5u);
    const auto b = whiteNoise (n, 0.8f, 6u);
    // Antiphase-heavy content with the safety ON (default params): the
    // safety never touches a width of 1.
    const auto l = mix (a, 0.3f, b, 0.7f);
    const auto r = mix (a, 0.3f, b, -0.7f);

    StereoSpatializer sp;
    setUp (sp, SpatializerParams {});
    Planar out = stereo (l, r);
    processInBlocks (sp, out, 333);
    CHECK (identical (out, stereo (l, r)));
    CHECK_NEAR (sp.getEffectiveWidth(), 1.0, 0.0);

    // After a detour through every stage and back, it lands bit-exactly again.
    SpatializerParams busy;
    busy.width = 2.0f;
    busy.positionalFocus = 1.0f;
    busy.space = 1.0f;
    busy.crossfeed = 1.0f;
    sp.setParams (busy);
    Planar detour = stereo (l, r);
    processInBlocks (sp, detour, 256);
    CHECK (! identical (detour, stereo (l, r)));
    sp.setParams (SpatializerParams {});
    Planar settle = stereo (l, r);
    processInBlocks (sp, settle, 256); // 0.5 s: every smoother has landed
    Planar after = stereo (l, r);
    processInBlocks (sp, after, 256);
    CHECK (identical (after, stereo (l, r)));
}

TEST_CASE ("StereoSpatializer: width 0 folds to mono, L' == R' == (L + R) / 2")
{
    const int n = 8192;
    const auto l = whiteNoise (n, 0.9f, 21u);
    const auto r = mix (whiteNoise (n, 0.9f, 22u), 1.0f, l, -0.4f);
    SpatializerParams p = neutral();
    p.width = 0.0f;
    StereoSpatializer sp;
    setUp (sp, p);
    Planar out = stereo (l, r);
    processInBlocks (sp, out, 100);
    bool equal = true, isMid = true;
    for (size_t i = 0; i < l.size(); ++i)
    {
        equal = equal && out.ch[0][i] == out.ch[1][i];
        isMid = isMid && out.ch[0][i] == 0.5f * (l[i] + r[i]);
    }
    CHECK (equal);
    CHECK (isMid);
    CHECK_NEAR (sp.getEffectiveWidth(), 0.0, 0.0);
}

TEST_CASE ("StereoSpatializer: width 2 lifts S by 6 dB above the low cut and not below; M untouched")
{
    SpatializerParams p = neutral();
    p.width = 2.0f;
    p.widthLowCutHz = 180.0f;
    CHECK_LE (std::abs (sideGainDb (p, 40.0)), 0.1);
    CHECK_LE (sideGainDb (p, 90.0), 0.6); // half the cut: +0.4 dB (an LR4 sum: +0.5 dB)
    CHECK_NEAR (sideGainDb (p, 2000.0), 6.02, 0.1);
    CHECK_NEAR (sideGainDb (p, 10000.0), 6.02, 0.1);

    // The response is exactly the 2nd-order (Q 1/sqrt 2) high shelf of Svf.h.
    const auto shelf = SvfCoeffs::make (FilterType::HighShelf, 180.0, 0.70710678, 20.0 * std::log10 (2.0), kFs);
    for (double f : { 60.0, 180.0, 400.0, 1000.0 })
        CHECK_NEAR (sideGainDb (p, f), shelf.magnitudeDb (f, kFs), 0.05);

    // Higher low cut moves the whole transition up.
    p.widthLowCutHz = 500.0f;
    CHECK_LE (std::abs (sideGainDb (p, 150.0)), 0.1);
    CHECK_NEAR (sideGainDb (p, 6000.0), 6.02, 0.1);

    // Same at the other sample rates.
    p.widthLowCutHz = 180.0f;
    for (double fs : { 44100.0, 192000.0 })
    {
        CHECK_LE (std::abs (sideGainDb (p, 40.0, fs)), 0.1);
        CHECK_NEAR (sideGainDb (p, 5000.0, fs), 6.02, 0.1);
    }

    // Narrowing is broadband.
    p.width = 0.5f;
    CHECK_NEAR (sideGainDb (p, 40.0), -6.02, 0.05);
    CHECK_NEAR (sideGainDb (p, 5000.0), -6.02, 0.05);

    // A mid-only signal (L == R) is returned bit-exactly.
    p.width = 2.0f;
    StereoSpatializer sp;
    setUp (sp, p);
    const auto x = whiteNoise (4096, 0.7f, 3u);
    Planar out = stereo (x, x);
    processInBlocks (sp, out, 128);
    CHECK (identical (out, stereo (x, x)));
}

TEST_CASE ("StereoSpatializer: positional focus lifts S around 3 kHz only; M untouched")
{
    SpatializerParams p = neutral();
    p.positionalFocus = 1.0f;
    CHECK_NEAR (sideGainDb (p, 3000.0), 6.0, 0.05);
    CHECK_LE (std::abs (sideGainDb (p, 100.0)), 0.1);
    CHECK_LE (sideGainDb (p, 1000.0), 4.0);
    CHECK_LE (sideGainDb (p, 15000.0), 1.5);
    const auto bell = SvfCoeffs::make (FilterType::Bell, 3000.0, 0.5, 6.0, kFs);
    for (double f : { 300.0, 1000.0, 2000.0, 6000.0, 12000.0 })
        CHECK_NEAR (sideGainDb (p, f), bell.magnitudeDb (f, kFs), 0.05);

    p.positionalFocus = 0.5f;
    CHECK_NEAR (sideGainDb (p, 3000.0), 3.0, 0.05);
    CHECK_NEAR (sideGainDb (p, 3000.0, 44100.0), 3.0, 0.05);

    p.positionalFocus = 1.0f;
    StereoSpatializer sp;
    setUp (sp, p);
    const auto x = whiteNoise (4096, 0.7f, 4u);
    Planar out = stereo (x, x);
    processInBlocks (sp, out, 128);
    CHECK (identical (out, stereo (x, x)));
}

TEST_CASE ("StereoSpatializer: space adds decorrelated S to a mono input; mono sum stays exact")
{
    const int n = 96000;
    const auto x = whiteNoise (n, 0.5f, 7u);
    SpatializerParams p;
    p.space = 1.0f; // safety on (defaults): the output stays positively correlated
    StereoSpatializer sp;
    setUp (sp, p);
    const Planar in = stereo (x, x);
    Planar out = stereo (x, x);
    processInBlocks (sp, out, 512);

    CHECK_LE (maxMonoSumError (in, out), 1.0e-5);
    const int start = n / 2, len = n / 2;
    const auto s = sideOf (out, start, len);
    const auto m = midOf (out, start, len);
    // S = 0.5 * allpass (HP_300 (M)): about -6 dB re M for white noise.
    const double ratioDb = rmsDb (s) - rmsDb (m);
    CHECK_NEAR (ratioDb, -6.1, 0.5);

    // Uncorrelated with M at lag 0 (the pre-delay keeps the Schroeder
    // direct tap out of it) ...
    double sm = 0.0, ss = 0.0, mm = 0.0;
    for (size_t i = 0; i < s.size(); ++i)
    {
        sm += static_cast<double> (s[i]) * m[i];
        ss += static_cast<double> (s[i]) * s[i];
        mm += static_cast<double> (m[i]) * m[i];
    }
    CHECK_LE (std::abs (sm / std::sqrt (ss * mm)), 0.05);
    // ... so the centre does not move: L and R carry the same level.
    const std::vector<float> lOut (out.ch[0].begin() + start, out.ch[0].end());
    const std::vector<float> rOut (out.ch[1].begin() + start, out.ch[1].end());
    CHECK_NEAR (rmsDb (lOut) - rmsDb (rOut), 0.0, 0.2);
    // Same for music-like content, whose neighbouring samples are strongly
    // correlated (an undelayed Schroeder direct tap would shift it ~4 dB).
    {
        StereoSpatializer dark;
        setUp (dark, p);
        const auto y = darkNoise (n, 0.5f, 17u);
        Planar outDark = stereo (y, y);
        processInBlocks (dark, outDark, 512);
        const std::vector<float> lDark (outDark.ch[0].begin() + start, outDark.ch[0].end());
        const std::vector<float> rDark (outDark.ch[1].begin() + start, outDark.ch[1].end());
        CHECK_NEAR (rmsDb (lDark) - rmsDb (rDark), 0.0, 0.2);
        CHECK_GE (rmsDb (sideOf (outDark, start, len)) - rmsDb (midOf (outDark, start, len)), -20.0);
    }
    // Output correlation (1 - 0.25) / (1 + 0.25) ~ 0.6 is published.
    CHECK_NEAR (sp.getCorrelation(), 0.6, 0.1);
    CHECK_NEAR (sp.getEffectiveWidth(), 1.0, 0.0);

    // Steady mono sines: the all-pass network is flat, so S / M is exactly
    // 0.5 |HP_300 (f)| - -6.02 dB well above 300 Hz, < -25 dB at 80 Hz.
    const auto monoSineSideDb = [&p] (double freq) {
        StereoSpatializer sine1;
        setUp (sine1, p);
        const int total = 72000, from = 24000;
        const auto y = sine (freq, kFs, total, 0.4f);
        Planar o = stereo (y, y);
        processInBlocks (sine1, o, 512);
        const auto sOut = sideOf (o, from, total - from);
        return toDb (toneAmplitude (sOut.data(), total - from, freq, kFs) / 0.4);
    };
    CHECK_NEAR (monoSineSideDb (3000.0), -6.02, 0.05);
    CHECK_NEAR (monoSineSideDb (9000.0), -6.02, 0.05);
    CHECK_LE (monoSineSideDb (80.0), -25.0);

    // Half the amount -> half the ambience amplitude.
    p.space = 0.5f;
    StereoSpatializer half;
    setUp (half, p);
    Planar outHalf = stereo (x, x);
    processInBlocks (half, outHalf, 512);
    CHECK_NEAR (rmsDb (sideOf (outHalf, start, len)) - rmsDb (s), -6.02, 0.1);
}

TEST_CASE ("StereoSpatializer: crossfeed reduces low-frequency S only; M untouched")
{
    SpatializerParams p = neutral();
    p.crossfeed = 1.0f;
    CHECK_NEAR (sideGainDb (p, 100.0), crossfeedDb (1.0, 100.0, kFs), 0.05);
    CHECK_LE (sideGainDb (p, 100.0), -7.0);
    CHECK_GE (sideGainDb (p, 8000.0), -0.1);
    CHECK_NEAR (sideGainDb (p, 700.0), crossfeedDb (1.0, 700.0, kFs), 0.05);
    p.crossfeed = 0.5f;
    CHECK_NEAR (sideGainDb (p, 100.0), crossfeedDb (0.5, 100.0, kFs), 0.05);
    CHECK_NEAR (sideGainDb (p, 100.0, 96000.0), crossfeedDb (0.5, 100.0, 96000.0), 0.05);

    p.crossfeed = 1.0f;
    StereoSpatializer sp;
    setUp (sp, p);
    const auto x = whiteNoise (4096, 0.7f, 8u);
    Planar out = stereo (x, x);
    processInBlocks (sp, out, 128);
    CHECK (identical (out, stereo (x, x)));
}

TEST_CASE ("StereoSpatializer: auto mono safety pulls the width back for antiphase-heavy content")
{
    const int n = static_cast<int> (kFs * 2.0);
    const auto a = whiteNoise (n, 0.5f, 31u);
    const auto b = whiteNoise (n, 0.05f, 32u);
    const auto c = whiteNoise (n, 0.05f, 33u);
    const auto antiL = mix (a, 1.0f, b, 1.0f); // correlation ~ -0.98
    const auto antiR = mix (a, -1.0f, c, 1.0f);

    SpatializerParams p;
    p.width = 2.0f;
    p.autoMonoSafety = true;
    p.minCorrelation = 0.0f;
    StereoSpatializer sp;
    setUp (sp, p);
    CHECK_NEAR (sp.getEffectiveWidth(), 2.0, 0.0);

    Planar buf = stereo (antiL, antiR);
    std::vector<float> trace;
    for (int pos = 0; pos < n; pos += 256)
    {
        sp.process (buf.block (pos, std::min (256, n - pos)));
        trace.push_back (sp.getEffectiveWidth());
    }
    CHECK_NEAR (trace.back(), 1.0, 1.0e-3);
    CHECK_LE (sp.getCorrelation(), -0.5);
    // Smooth pull (no jumps) and never outside [1, 2].
    double maxJump = 0.0;
    for (size_t i = 1; i < trace.size(); ++i)
        maxJump = std::max (maxJump, static_cast<double> (std::abs (trace[i] - trace[i - 1])));
    CHECK_LE (maxJump, 0.05);
    CHECK_GE (*std::min_element (trace.begin(), trace.end()), 1.0f);
    CHECK_LE (*std::max_element (trace.begin(), trace.end()), 2.0f);
    // Pulled within ~0.6 s (300 ms detector + 300 ms pull).
    CHECK_LE (trace[static_cast<size_t> (kFs * 0.8 / 256)], 1.1f);

    // Silence says nothing about the material: the pull is held (a game's
    // pauses must not pump the width back up). For the first seconds the
    // 300 ms memory still holds the old correlation; once the output is
    // below -100 dBFS the correlation is undefined (meter reads 1) and the
    // pull is still held.
    Planar pause (2, n);
    processInBlocks (sp, pause, 256);
    CHECK_NEAR (sp.getEffectiveWidth(), 1.0, 1.0e-3);
    CHECK_LE (sp.getCorrelation(), -0.5);
    for (int k = 0; k < 3; ++k)
    {
        Planar silence (2, n);
        processInBlocks (sp, silence, 256);
    }
    CHECK_NEAR (sp.getCorrelation(), 1.0, 0.0);
    CHECK_NEAR (sp.getEffectiveWidth(), 1.0, 1.0e-3);

    // Recovery once the content is well correlated again (width 2 keeps the
    // output correlation at ~0.7 for this material).
    const auto corrL = mix (a, 1.0f, b, 3.0f);
    const auto corrR = mix (a, 1.0f, c, 3.0f);
    for (int k = 0; k < 3; ++k)
    {
        Planar more = stereo (corrL, corrR);
        processInBlocks (sp, more, 256);
    }
    CHECK_NEAR (sp.getEffectiveWidth(), 2.0, 1.0e-3);
    CHECK_GE (sp.getCorrelation(), 0.2);

    // Safety off: the width stays where the user put it.
    p.autoMonoSafety = false;
    StereoSpatializer off;
    setUp (off, p);
    Planar bufOff = stereo (antiL, antiR);
    processInBlocks (off, bufOff, 256);
    CHECK_NEAR (off.getEffectiveWidth(), 2.0, 0.0);
    CHECK_LE (off.getCorrelation(), -0.9);

    // A narrowed image is never widened by the safety.
    p.autoMonoSafety = true;
    p.width = 0.5f;
    StereoSpatializer narrow;
    setUp (narrow, p);
    Planar bufNarrow = stereo (antiL, antiR);
    processInBlocks (narrow, bufNarrow, 256);
    CHECK_NEAR (narrow.getEffectiveWidth(), 0.5, 0.0);

    // minCorrelation -1 never engages.
    p.width = 2.0f;
    p.minCorrelation = -1.0f;
    StereoSpatializer never;
    setUp (never, p);
    Planar bufNever = stereo (antiL, antiR);
    processInBlocks (never, bufNever, 256);
    CHECK_NEAR (never.getEffectiveWidth(), 2.0, 0.0);
}

TEST_CASE ("StereoSpatializer: published correlation tracks the output")
{
    const int n = 48000;
    const auto a = whiteNoise (n, 0.5f, 41u);
    const auto b = whiteNoise (n, 0.5f, 42u);
    StereoSpatializer sp;
    setUp (sp, neutral());
    Planar same = stereo (a, a);
    processInBlocks (sp, same, 512);
    CHECK_NEAR (sp.getCorrelation(), 1.0, 1.0e-4);
    sp.reset();
    Planar anti = stereo (a, scaled (a, -1.0f));
    processInBlocks (sp, anti, 512);
    CHECK_NEAR (sp.getCorrelation(), -1.0, 1.0e-4);
    sp.reset();
    Planar indep = stereo (a, b);
    processInBlocks (sp, indep, 512);
    CHECK_LE (std::abs (sp.getCorrelation()), 0.1);
    sp.reset();
    CHECK_NEAR (sp.getCorrelation(), 1.0, 0.0);
}

TEST_CASE ("StereoSpatializer: 1-channel and 6-channel blocks pass through untouched")
{
    SpatializerParams p;
    p.width = 2.0f;
    p.positionalFocus = 1.0f;
    p.space = 1.0f;
    p.crossfeed = 1.0f;
    StereoSpatializer sp;
    setUp (sp, p, kFs, 512, 6);

    const int n = 2048;
    Planar six (6, n);
    for (int c = 0; c < 6; ++c)
    {
        const auto x = whiteNoise (n, 0.8f, 50u + static_cast<uint32_t> (c));
        std::copy (x.begin(), x.end(), six.ch[static_cast<size_t> (c)].begin());
    }
    Planar ref = six;
    // Planar's copy keeps pointers into `six`: rebuild them for the copy.
    for (size_t c = 0; c < ref.ch.size(); ++c)
        ref.ptrs[c] = ref.ch[c].data();

    processInBlocks (sp, six, 512);
    CHECK (identical (six, ref));
    for (int pos = 0; pos < n; pos += 256)
        sp.process (six.block (pos, 256).firstChannels (1));
    CHECK (identical (six, ref));

    // Sanity: a stereo block of the same stream is processed.
    for (int pos = 0; pos < n; pos += 256)
        sp.process (six.block (pos, 256).firstChannels (2));
    CHECK (six.ch[0] != ref.ch[0]);
    CHECK (six.ch[2] == ref.ch[2]);
}

TEST_CASE ("StereoSpatializer: no allocation in reset / setParams / process")
{
    StereoSpatializer sp;
    setUp (sp, SpatializerParams {}, kFs, 512, 2);
    const auto l = whiteNoise (2048, 0.8f, 61u);
    const auto r = whiteNoise (2048, 0.8f, 62u);
    Planar buf = stereo (l, r);
    SpatializerParams busy;
    busy.width = 1.7f;
    busy.widthLowCutHz = 320.0f;
    busy.positionalFocus = 0.8f;
    busy.space = 0.9f;
    busy.crossfeed = 0.4f;
    busy.minCorrelation = 0.5f;
    SpatializerParams nan;
    nan.width = std::numeric_limits<float>::quiet_NaN();

    AllocationGuard guard;
    sp.reset();
    sp.setParams (busy);
    for (int pos = 0; pos < 2048; pos += 512)
        sp.process (buf.block (pos, 512));
    sp.setParams (nan);
    sp.setParams (SpatializerParams {});
    for (int pos = 0; pos < 2048; pos += 64)
        sp.process (buf.block (pos, 64));
    sp.process (buf.block (0, 512).firstChannels (1));
    sp.reset();
    sp.process (buf.block (0, 1));
    [[maybe_unused]] const float c = sp.getCorrelation() + sp.getEffectiveWidth();
    CHECK (guard.allocations() == 0);
}

TEST_CASE ("StereoSpatializer: robustness - silence, DC, full-scale noise, impulses, extreme params, all rates")
{
    const float inf = std::numeric_limits<float>::infinity();
    const float nanV = std::numeric_limits<float>::quiet_NaN();

    std::vector<SpatializerParams> settings;
    {
        SpatializerParams maxed;
        maxed.width = 2.0f;
        maxed.widthLowCutHz = 60.0f;
        maxed.positionalFocus = 1.0f;
        maxed.space = 1.0f;
        maxed.crossfeed = 1.0f;
        maxed.minCorrelation = 1.0f;
        settings.push_back (maxed);
        SpatializerParams absurd;
        absurd.width = 1.0e9f;
        absurd.widthLowCutHz = 1.0e9f;
        absurd.positionalFocus = inf;
        absurd.space = 1.0e6f;
        absurd.crossfeed = -1.0e6f;
        absurd.minCorrelation = -inf;
        settings.push_back (absurd);
        SpatializerParams negative;
        negative.width = -5.0f;
        negative.widthLowCutHz = -1.0f;
        negative.positionalFocus = -1.0f;
        negative.space = -inf;
        negative.crossfeed = inf;
        negative.minCorrelation = inf;
        settings.push_back (negative);
        SpatializerParams nanP;
        nanP.width = nanV;
        nanP.widthLowCutHz = nanV;
        nanP.positionalFocus = nanV;
        nanP.space = nanV;
        nanP.crossfeed = nanV;
        nanP.minCorrelation = nanV;
        settings.push_back (nanP);
    }

    // Parameter sanitising: clamped, NaN keeps the previous value.
    {
        StereoSpatializer sp;
        sp.setParams (settings[1]);
        CHECK_NEAR (sp.getParams().width, 2.0, 0.0);
        CHECK_NEAR (sp.getParams().widthLowCutHz, 500.0, 0.0);
        CHECK_NEAR (sp.getParams().positionalFocus, 1.0, 0.0);
        CHECK_NEAR (sp.getParams().space, 1.0, 0.0);
        CHECK_NEAR (sp.getParams().crossfeed, 0.0, 0.0);
        CHECK_NEAR (sp.getParams().minCorrelation, -1.0, 0.0);
        sp.setParams (settings[3]);
        CHECK_NEAR (sp.getParams().width, 2.0, 0.0);
        CHECK_NEAR (sp.getParams().widthLowCutHz, 500.0, 0.0);
        CHECK_NEAR (sp.getParams().space, 1.0, 0.0);
    }

    for (double fs : kRates)
    {
        const int n = static_cast<int> (fs * 0.25);
        std::vector<std::pair<std::vector<float>, std::vector<float>>> inputs;
        inputs.push_back ({ std::vector<float> (static_cast<size_t> (n), 0.0f), std::vector<float> (static_cast<size_t> (n), 0.0f) });
        inputs.push_back ({ std::vector<float> (static_cast<size_t> (n), 1.0f), std::vector<float> (static_cast<size_t> (n), -1.0f) });
        inputs.push_back ({ std::vector<float> (static_cast<size_t> (n), 1.0f), std::vector<float> (static_cast<size_t> (n), 1.0f) });
        inputs.push_back ({ whiteNoise (n, 1.0f, 71u), whiteNoise (n, 1.0f, 72u) });
        {
            std::vector<float> imp (static_cast<size_t> (n), 0.0f);
            for (size_t i = 3; i < imp.size(); i += 997)
                imp[i] = 1.0f;
            inputs.push_back ({ imp, std::vector<float> (static_cast<size_t> (n), 0.0f) });
            inputs.push_back ({ imp, scaled (imp, -1.0f) });
        }

        for (const auto& s : settings)
            for (size_t k = 0; k < inputs.size(); ++k)
            {
                StereoSpatializer sp;
                setUp (sp, settings[0], fs, 512);
                sp.setParams (s); // glide from maxed into the extreme setting
                Planar buf = stereo (inputs[k].first, inputs[k].second);
                processInBlocks (sp, buf, 173);
                CHECK (allFinite (buf));
                CHECK_LE (std::max (peakAbs (buf.ch[0].data(), n), peakAbs (buf.ch[1].data(), n)), 16.0);
                CHECK (std::isfinite (sp.getCorrelation()) && std::isfinite (sp.getEffectiveWidth()));
                if (k == 0)
                    CHECK_NEAR (std::max (peakAbs (buf.ch[0].data(), n), peakAbs (buf.ch[1].data(), n)), 0.0, 0.0);
            }
    }

    // Tails decay to exact zero (no subnormal crawl) and a NaN input sample
    // does not poison the state for good.
    for (double fs : { 44100.0, 192000.0 })
    {
        StereoSpatializer sp;
        setUp (sp, settings[0], fs, 512);
        const int n = static_cast<int> (fs * 0.5);
        Planar noise = stereo (whiteNoise (n, 1.0f, 81u), whiteNoise (n, 1.0f, 82u));
        processInBlocks (sp, noise, 512);
        const int quiet = static_cast<int> (fs * 3.0);
        Planar silence (2, quiet);
        processInBlocks (sp, silence, 512);
        const int last = 4096;
        CHECK_NEAR (std::max (peakAbs (silence.ch[0].data() + quiet - last, last), peakAbs (silence.ch[1].data() + quiet - last, last)), 0.0, 0.0);

        Planar bad = stereo (whiteNoise (512, 0.5f, 83u), whiteNoise (512, 0.5f, 84u));
        bad.ch[0][100] = nanV;
        sp.process (bad.block (0, 512));
        Planar good = stereo (whiteNoise (4096, 0.5f, 85u), whiteNoise (4096, 0.5f, 86u));
        processInBlocks (sp, good, 512);
        CHECK (allFinite (good));
        CHECK (std::isfinite (sp.getCorrelation()));
    }
}

TEST_CASE ("StereoSpatializer: output is independent of the host block size")
{
    const int n = 7168 * 6;
    const auto a = whiteNoise (n, 0.6f, 91u);
    const auto b = whiteNoise (n, 0.6f, 92u);
    // First half antiphase-heavy (the mono safety moves), second half correlated.
    std::vector<float> l (static_cast<size_t> (n)), r (static_cast<size_t> (n));
    for (size_t i = 0; i < l.size(); ++i)
    {
        const bool anti = i < l.size() / 2;
        l[i] = a[i] + 0.2f * b[i];
        r[i] = (anti ? -a[i] : a[i]) + 0.2f * b[(i * 3) % l.size()];
    }

    SpatializerParams p;
    p.width = 1.8f;
    p.widthLowCutHz = 150.0f;
    p.positionalFocus = 0.7f;
    p.space = 0.6f;
    p.crossfeed = 0.4f;
    p.autoMonoSafety = true;
    p.minCorrelation = 0.2f;
    SpatializerParams q = p; // applied at a position every block size hits
    q.widthLowCutHz = 420.0f;
    q.positionalFocus = 0.1f;
    q.space = 0.2f;
    q.crossfeed = 0.9f;
    q.width = 0.6f;
    const int changeAt = 7168 * 4;

    const auto run = [&] (int blockSize, float& finalWidth) {
        StereoSpatializer sp;
        setUp (sp, p);
        Planar buf = stereo (l, r);
        for (int pos = 0; pos < n; pos += blockSize)
        {
            if (pos == changeAt)
                sp.setParams (q);
            sp.process (buf.block (pos, std::min (blockSize, n - pos)));
        }
        finalWidth = sp.getEffectiveWidth();
        return buf;
    };

    float w1 = 0.0f;
    const Planar ref = run (1, w1);
    for (int bs : { 7, 64, 512 })
    {
        float w = 0.0f;
        const Planar out = run (bs, w);
        CHECK_LE (maxAbsDiff (ref, out), 1.0e-6);
        CHECK_NEAR (w, w1, 1.0e-6);
    }
}

TEST_CASE ("StereoSpatializer: zero latency - an impulse comes out at its own sample")
{
    StereoSpatializer sp;
    CHECK (sp.latencySamples() == 0);

    const int n = 1024, at = 100;
    std::vector<float> imp (static_cast<size_t> (n), 0.0f);
    imp[static_cast<size_t> (at)] = 1.0f;
    const std::vector<float> zero (static_cast<size_t> (n), 0.0f);

    // Neutral: the impulse comes back bit-exactly.
    setUp (sp, neutral());
    Planar neutralOut = stereo (imp, zero);
    processInBlocks (sp, neutralOut, 64);
    CHECK (identical (neutralOut, stereo (imp, zero)));

    // Every stage busy: nothing before the impulse, the peak at the impulse.
    SpatializerParams p;
    p.width = 2.0f;
    p.positionalFocus = 1.0f;
    p.space = 1.0f;
    p.crossfeed = 0.5f;
    setUp (sp, p);
    Planar out = stereo (imp, zero);
    processInBlocks (sp, out, 64);
    CHECK_NEAR (peakAbs (out.ch[0].data(), at) + peakAbs (out.ch[1].data(), at), 0.0, 0.0);
    int peakIndex = 0;
    for (int i = 0; i < n; ++i)
        if (std::abs (out.ch[0][static_cast<size_t> (i)]) > std::abs (out.ch[0][static_cast<size_t> (peakIndex)]))
            peakIndex = i;
    CHECK (peakIndex == at);
    CHECK_GE (out.ch[0][static_cast<size_t> (at)], 0.5f);
    // M = 0.5 at the impulse and nowhere else: the mono sum is the impulse.
    CHECK_NEAR (out.ch[0][static_cast<size_t> (at)] + out.ch[1][static_cast<size_t> (at)], 1.0, 1.0e-6);
}

TEST_CASE ("StereoSpatializer: parameter jumps are click-free")
{
    const int n = 24000, change = 12000;
    // Side sine at its peak when the parameter jumps: an unsmoothed step of
    // width / focus / crossfeed / low cut would show up as a curvature spike
    // (the size of the step: 0.1 .. 0.4 here) far above the sine's own A w^2.
    // A one-pole glide only has a slope corner (~ step / 960 samples), which
    // the factor 2 below allows for.
    const auto check = [&] (const SpatializerParams& from, const SpatializerParams& to, double freq) {
        const double phase = 0.5 * kPi - kTwoPi * freq * change / kFs;
        const auto x = sine (freq, kFs, n, 0.25f, phase);
        const auto render = [&] (const SpatializerParams& first, const SpatializerParams& second) {
            StereoSpatializer sp;
            setUp (sp, first);
            Planar buf = stereo (x, scaled (x, -1.0f));
            for (int pos = 0; pos < n; pos += 480) // 12000 is a block boundary
            {
                if (pos == change)
                    sp.setParams (second);
                sp.process (buf.block (pos, 480));
            }
            return buf;
        };
        const Planar steadyFrom = render (from, from);
        const Planar steadyTo = render (to, to);
        const Planar jump = render (from, to);
        const double bound = std::max (maxCurvature (steadyFrom.ch[0], 6000, 6000), maxCurvature (steadyTo.ch[0], 18000, 6000));
        CHECK_LE (maxCurvature (jump.ch[0], change - 100, 9600), 2.0 * bound);
    };

    SpatializerParams a = neutral(), b = neutral();
    a.width = 0.3f;
    b.width = 2.0f;
    check (a, b, 1000.0);
    check (b, a, 1000.0);
    a = b = neutral();
    b.positionalFocus = 1.0f;
    check (a, b, 1500.0);
    check (b, a, 1500.0);
    a = b = neutral();
    b.crossfeed = 1.0f;
    check (a, b, 200.0);
    check (b, a, 200.0);
    a = b = neutral();
    a.width = b.width = 2.0f;
    a.widthLowCutHz = 60.0f;
    b.widthLowCutHz = 500.0f;
    check (a, b, 250.0);
    check (b, a, 250.0);

    // Space: the ambience fades in, it does not switch on.
    StereoSpatializer sp;
    setUp (sp, neutral());
    const auto x = whiteNoise (n, 0.5f, 99u);
    SpatializerParams on = neutral();
    on.space = 1.0f;
    Planar buf = stereo (x, x);
    for (int pos = 0; pos < n; pos += 480)
    {
        if (pos == change)
            sp.setParams (on);
        sp.process (buf.block (pos, 480));
    }
    const auto s = sideOf (buf, 0, n);
    const double early = peakAbs (s.data() + change, 24); // first 0.5 ms
    const double settled = peakAbs (s.data() + change + 4800, 4800);
    CHECK_LE (early, 0.1 * settled);
    CHECK_GE (settled, 0.05);
}

// ---- adversarial review tests ----

TEST_CASE ("StereoSpatializer (review): bit-identical for any block size, including tails and max-size blocks")
{
    // Noise with the safety moving, a parameter change, then a long silent
    // tail (where the denormal flush acts). Every host block size - including
    // the 4096 maximum - must give the same bits, and the same telemetry.
    for (double fs : { 44100.0, 192000.0 })
    {
        const int n = static_cast<int> (fs * 3.0);
        const int loud = n / 6; // then 2.5 s of silence (the ambience decays ~135 dB/s)
        const auto a = whiteNoise (loud, 0.7f, 101u);
        const auto b = whiteNoise (loud, 0.7f, 102u);
        std::vector<float> l (static_cast<size_t> (n), 0.0f), r (static_cast<size_t> (n), 0.0f);
        for (size_t i = 0; i < static_cast<size_t> (loud); ++i)
        {
            l[i] = a[i] + 0.1f * b[i];
            r[i] = -a[i] + 0.1f * b[(i * 5) % b.size()];
        }
        SpatializerParams p;
        p.width = 2.0f;
        p.widthLowCutHz = 60.0f;
        p.positionalFocus = 1.0f;
        p.space = 1.0f;
        p.crossfeed = 1.0f;
        p.minCorrelation = 0.3f;
        SpatializerParams q = p;
        q.widthLowCutHz = 500.0f;
        q.space = 0.4f;
        const int changeAt = 8192; // blocks are cut there, as a host would

        const auto run = [&] (int blockSize, float& corr, float& width) {
            StereoSpatializer sp;
            setUp (sp, p, fs, 4096);
            Planar buf = stereo (l, r);
            for (int pos = 0; pos < n;)
            {
                if (pos == changeAt)
                    sp.setParams (q);
                const int len = std::min ({ blockSize, n - pos, pos < changeAt ? changeAt - pos : n });
                sp.process (buf.block (pos, len));
                pos += len;
            }
            corr = sp.getCorrelation();
            width = sp.getEffectiveWidth();
            return buf;
        };
        float c1 = 0.0f, w1 = 0.0f;
        const Planar ref = run (1, c1, w1);
        for (int bs : { 13, 1024, 4096 })
        {
            float c = 0.0f, w = 0.0f;
            const Planar out = run (bs, c, w);
            CHECK (identical (ref, out));
            CHECK (c == c1);
            CHECK (w == w1);
        }
        // The tail ends in exact zeros (no subnormal crawl) even with 4096 blocks.
        CHECK_NEAR (std::max (peakAbs (ref.ch[0].data() + n - 4096, 4096), peakAbs (ref.ch[1].data() + n - 4096, 4096)), 0.0, 0.0);
    }
}

TEST_CASE ("StereoSpatializer (review): mono safety releases with minCorrelation close to 1")
{
    // minCorrelation 0.97: a fixed 0.05 release band would demand a
    // correlation above 1.02, so a pulled width could never come back.
    const int n = static_cast<int> (kFs * 2.0);
    const auto a = whiteNoise (n, 0.5f, 111u);
    const auto b = whiteNoise (n, 0.5f, 112u);
    const auto c = whiteNoise (n, 0.5f, 113u);
    SpatializerParams p;
    p.width = 2.0f;
    p.minCorrelation = 0.97f;
    StereoSpatializer sp;
    setUp (sp, p);

    Planar pull = stereo (mix (a, 1.0f, b, 0.3f), mix (a, 1.0f, c, 0.3f)); // output rho ~0.7 at width 2
    processInBlocks (sp, pull, 256);
    CHECK_LE (sp.getEffectiveWidth(), 1.01f);

    // Nearly mono material: at width 2 the output correlation is ~0.996,
    // comfortably above the target, so the width must be restored.
    for (int k = 0; k < 4; ++k)
    {
        Planar near = stereo (mix (a, 1.0f, b, 0.03f), mix (a, 1.0f, c, 0.03f));
        processInBlocks (sp, near, 256);
    }
    CHECK_GE (sp.getCorrelation(), 0.985f);
    CHECK_NEAR (sp.getEffectiveWidth(), 2.0, 1.0e-3);
}

TEST_CASE ("StereoSpatializer (review): mono safety timing does not depend on the sample rate")
{
    // Seconds until the effective width first drops below 1.1 on antiphase
    // content, and back above 1.9 after it turns well correlated.
    const auto times = [] (double fs, double& pullS, double& recoverS) {
        const int n = static_cast<int> (fs * 6.0);
        const auto a = whiteNoise (n, 0.5f, 121u);
        const auto b = whiteNoise (n, 0.1f, 122u);
        SpatializerParams p;
        p.width = 2.0f;
        StereoSpatializer sp;
        setUp (sp, p, fs);
        pullS = recoverS = -1.0;
        const int half = n / 3;
        std::vector<float> l (static_cast<size_t> (n)), r (static_cast<size_t> (n));
        for (size_t i = 0; i < l.size(); ++i)
        {
            l[i] = a[i] + b[i];
            r[i] = (static_cast<int> (i) < half ? -a[i] : a[i]) + b[(i * 7) % b.size()];
        }
        Planar buf = stereo (l, r);
        for (int pos = 0; pos < n; pos += 64)
        {
            sp.process (buf.block (pos, std::min (64, n - pos)));
            const double t = (pos + 64) / fs;
            if (pullS < 0.0 && sp.getEffectiveWidth() < 1.1f)
                pullS = t;
            if (pos >= half && recoverS < 0.0 && sp.getEffectiveWidth() > 1.9f)
                recoverS = t - half / fs;
        }
    };
    double p44 = 0.0, r44 = 0.0, p192 = 0.0, r192 = 0.0;
    times (44100.0, p44, r44);
    times (192000.0, p192, r192);
    CHECK (p44 > 0.0 && p192 > 0.0 && r44 > 0.0 && r192 > 0.0);
    CHECK_NEAR (p192, p44, 0.05);
    CHECK_NEAR (r192, r44, 0.1);
    CHECK_LE (p44, 0.8);
    CHECK_LE (r44, 4.5);
}

TEST_CASE ("StereoSpatializer (review): widening never mirrors a panned source around the low cut")
{
    // A hard-left tone at and around the low cut must stay on the left at
    // width 2 (an LR4 band sum on S would rotate S by -180 degrees against M
    // at the cut and move it to the right). L' / R' = |1 + H| / |1 - H|.
    for (double f : { 60.0, 90.0, 180.0, 360.0, 1000.0 })
    {
        SpatializerParams p = neutral();
        p.width = 2.0f;
        p.widthLowCutHz = 180.0f;
        StereoSpatializer sp;
        setUp (sp, p);
        const int total = 96000, from = 48000;
        const auto x = sine (f, kFs, total, 0.5f);
        Planar buf = stereo (x, std::vector<float> (x.size(), 0.0f));
        processInBlocks (sp, buf, 512);
        const double lAmp = toneAmplitude (buf.ch[0].data() + from, total - from, f, kFs);
        const double rAmp = toneAmplitude (buf.ch[1].data() + from, total - from, f, kFs);
        CHECK_GE (lAmp, 2.5 * rAmp);
        // Mono sum is still exactly the input.
        CHECK_NEAR (toneAmplitude (midOf (buf, from, total - from).data(), total - from, f, kFs), 0.25, 1.0e-4);
    }
}

TEST_CASE ("StereoSpatializer (review): NaN / inf inputs are contained within one control interval")
{
    const float nanV = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (float bad : { nanV, inf, -inf })
    {
        SpatializerParams p;
        p.width = 1.8f;
        p.positionalFocus = 1.0f;
        p.space = 1.0f;
        p.crossfeed = 1.0f;
        StereoSpatializer sp;
        setUp (sp, p, kFs, 4096);
        Planar buf = stereo (whiteNoise (4096, 0.5f, 131u), whiteNoise (4096, 0.5f, 132u));
        buf.ch[1][10] = bad; // right channel only
        sp.process (buf.block (0, 4096));
        // The state is cleared at the next 32-sample control tick, not at the
        // end of a 4096-sample block.
        bool finiteAfter = true;
        for (int c = 0; c < 2; ++c)
            for (size_t i = 10 + 2 * 32; i < 4096; ++i)
                finiteAfter = finiteAfter && std::isfinite (buf.ch[static_cast<size_t> (c)][i]);
        CHECK (finiteAfter);
        CHECK (std::isfinite (sp.getCorrelation()) && std::isfinite (sp.getEffectiveWidth()));
        Planar next = stereo (whiteNoise (4096, 0.5f, 133u), whiteNoise (4096, 0.5f, 134u));
        processInBlocks (sp, next, 512);
        CHECK (allFinite (next));
    }
}

TEST_CASE ("StereoSpatializer (review): per-sample parameter thrash stays finite, bounded and mono-exact")
{
    FastRandom rng (0xBADF00Du);
    for (double fs : kRates)
    {
        const int n = static_cast<int> (fs * 0.3);
        const Planar in = stereo (whiteNoise (n, 1.0f, 141u), whiteNoise (n, 1.0f, 142u));
        Planar out = stereo (in.ch[0], in.ch[1]);
        StereoSpatializer sp;
        setUp (sp, SpatializerParams {}, fs, 1);
        float minW = 10.0f, maxW = -10.0f;
        for (int i = 0; i < n; ++i)
        {
            // Hammer every parameter between its extremes, one-sample blocks.
            SpatializerParams p;
            p.width = rng.nextBipolar() > 0.0f ? 2.0f : 0.0f;
            p.widthLowCutHz = rng.nextBipolar() > 0.0f ? 500.0f : 60.0f;
            p.positionalFocus = rng.nextBipolar() > 0.0f ? 1.0f : 0.0f;
            p.space = rng.nextBipolar() > 0.0f ? 1.0f : 0.0f;
            p.crossfeed = rng.nextBipolar() > 0.0f ? 1.0f : 0.0f;
            p.autoMonoSafety = rng.nextBipolar() > 0.0f;
            p.minCorrelation = rng.nextBipolar();
            sp.setParams (p);
            sp.process (out.block (i, 1));
            minW = std::min (minW, sp.getEffectiveWidth());
            maxW = std::max (maxW, sp.getEffectiveWidth());
        }
        CHECK (allFinite (out));
        CHECK_LE (maxMonoSumError (in, out), 1.0e-5);
        CHECK_GE (minW, 0.0f);
        CHECK_LE (maxW, 2.0f);
        CHECK_LE (std::max (peakAbs (out.ch[0].data(), n), peakAbs (out.ch[1].data(), n)), 16.0);
    }
}

TEST_CASE ("StereoSpatializer (review): re-prepare at another rate, empty blocks, silence start")
{
    SpatializerParams p;
    p.width = 2.0f;
    p.space = 1.0f;
    StereoSpatializer sp;
    setUp (sp, p, 44100.0, 256);

    // Long digital silence first: the safety must not drift (correlation is
    // undefined, so it holds at "no pull").
    Planar silence (2, 44100 * 2);
    processInBlocks (sp, silence, 256);
    CHECK_NEAR (sp.getEffectiveWidth(), 2.0, 0.0);
    CHECK_NEAR (sp.getCorrelation(), 1.0, 0.0);

    // Zero-length blocks are harmless.
    Planar tiny (2, 4);
    sp.process (tiny.block (0, 0));
    CHECK (allFinite (tiny));

    // Re-prepare at 192 kHz with larger blocks: the ambience keeps its level
    // (delay lines re-sized, S / M of a mono noise input still ~ -6 dB).
    sp.prepare ({ 192000.0, 4096, 2 });
    const int n = 192000;
    const auto x = whiteNoise (n, 0.5f, 151u);
    Planar buf = stereo (x, x);
    processInBlocks (sp, buf, 4096);
    CHECK (allFinite (buf));
    const double ratioDb = rmsDb (sideOf (buf, n / 2, n / 2)) - rmsDb (midOf (buf, n / 2, n / 2));
    CHECK_NEAR (ratioDb, -6.0, 0.6);
    CHECK_LE (maxMonoSumError (stereo (x, x), buf), 1.0e-5);
}
