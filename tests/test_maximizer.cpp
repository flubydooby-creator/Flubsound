// Tests for the loudness maximizer: the soft-clip transfer curve, exact
// latency and transparency at low level, the ceiling guarantee (sample peak
// and an INDEPENDENT band-limited true-peak measurement) with heavy drive on
// noise and drum-like programme, clipper on/off and its THD telemetry, the
// 3-band glue, click-free parameter changes, real-time safety, robustness
// and block-size invariance.
#include "TestFramework.h"
#include "TestSignals.h"

#include "Analysis.h"

#include "flub/common/Math.h"
#include "flub/dsp/LoudnessMaximizer.h"
#include "flub/engine/Parameters.h"
#include "flub/engine/ProcessingChain.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <iostream>
#include <limits>
#include <map>
#include <utility>
#include <vector>

using namespace flub;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;
const double kTpTolerance = std::pow (10.0, 0.1 / 20.0); // +0.1 dB

float dbfs (double db) { return static_cast<float> (std::pow (10.0, db / 20.0)); }

//==============================================================================
// Independent true-peak meter (shares no code with the limiter's detector):
// ideal band-limited (sinc) reconstruction of the whole finite signal via an
// 8x zero-padded spectrum, parabolic refinement of every local maximum.
// Two channels share one complex FFT (real part = a, imaginary part = b).
void fftInPlace (std::vector<std::complex<double>>& a, bool inverse)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap (a[i], a[j]);
    }
    // Forward twiddles e^{-j 2 pi k / n}, cached per size (test code only).
    static std::map<size_t, std::vector<std::complex<double>>> cache;
    auto& tw = cache[n];
    if (tw.empty())
    {
        tw.resize (n / 2);
        for (size_t k = 0; k < n / 2; ++k)
        {
            const double ang = -2.0 * kPi * static_cast<double> (k) / static_cast<double> (n);
            tw[k] = std::complex<double> (std::cos (ang), std::sin (ang));
        }
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const size_t half = len >> 1, step = n / len;
        for (size_t start = 0; start < n; start += len)
            for (size_t k = 0; k < half; ++k)
            {
                const std::complex<double> w = inverse ? std::conj (tw[k * step]) : tw[k * step];
                const std::complex<double> u = a[start + k];
                const std::complex<double> v = a[start + k + half] * w;
                a[start + k] = u + v;
                a[start + k + half] = u - v;
            }
    }
}

double independentTruePeak (const float* a, const float* b, int n)
{
    constexpr size_t os = 8;
    size_t N = 2;
    while (N < 2 * static_cast<size_t> (n))
        N <<= 1;
    std::vector<std::complex<double>> X (N);
    for (int i = 0; i < n; ++i)
        X[static_cast<size_t> (i)] = std::complex<double> (a[i], b != nullptr ? b[i] : 0.0f);
    fftInPlace (X, false);

    const size_t M = N * os;
    std::vector<std::complex<double>> Y (M);
    for (size_t k = 0; k < N / 2; ++k)
        Y[k] = X[k];
    for (size_t k = 1; k < N / 2; ++k)
        Y[M - k] = X[N - k];
    Y[N / 2] = 0.5 * X[N / 2];
    Y[M - N / 2] = 0.5 * X[N / 2];
    fftInPlace (Y, true);

    const double scale = 1.0 / static_cast<double> (N);
    double peak = 0.0;
    for (int part = 0; part < (b != nullptr ? 2 : 1); ++part)
    {
        auto at = [&] (size_t m) { return std::abs (part == 0 ? Y[m].real() : Y[m].imag()) * scale; };
        for (size_t m = 1; m + 1 < M; ++m)
        {
            const double y1 = at (m);
            if (y1 <= peak)
                continue;
            peak = y1;
            const double y0 = at (m - 1), y2 = at (m + 1);
            const double den = y0 - 2.0 * y1 + y2;
            if (y1 >= y0 && y1 >= y2 && den < 0.0)
            {
                const double p = 0.5 * (y0 - y2) / den;
                peak = std::max (peak, y1 - 0.25 * (y0 - y2) * p);
            }
        }
    }
    return peak;
}

/** Linear-phase Blackman-windowed-sinc low-pass (161 taps, cutoff 0.41 fs);
    the full convolution is returned, so the result is band-limited. */
std::vector<float> bandLimit (const std::vector<float>& x)
{
    constexpr int taps = 161, centre = taps / 2;
    constexpr double fc = 0.41;
    std::vector<double> h (taps);
    double sum = 0.0;
    for (int k = 0; k < taps; ++k)
    {
        const double t = k - centre;
        const double sinc = t == 0.0 ? 1.0 : std::sin (2.0 * kPi * fc * t) / (2.0 * kPi * fc * t);
        const double w = 0.42 - 0.5 * std::cos (kTwoPi * k / (taps - 1)) + 0.08 * std::cos (2.0 * kTwoPi * k / (taps - 1));
        h[static_cast<size_t> (k)] = sinc * w;
        sum += h[static_cast<size_t> (k)];
    }
    std::vector<float> y (x.size() + taps - 1, 0.0f);
    for (size_t i = 0; i < y.size(); ++i)
    {
        double acc = 0.0;
        for (int k = 0; k < taps; ++k)
            if (i >= static_cast<size_t> (k) && i - static_cast<size_t> (k) < x.size())
                acc += h[static_cast<size_t> (k)] / sum * x[i - static_cast<size_t> (k)];
        y[i] = static_cast<float> (acc);
    }
    return y;
}

//==============================================================================
void setChannel (Planar& buf, int c, const std::vector<float>& v)
{
    auto& dst = buf.ch[static_cast<size_t> (c)];
    std::fill (dst.begin(), dst.end(), 0.0f);
    std::copy_n (v.begin(), std::min (v.size(), dst.size()), dst.begin());
}

void prepareMax (LoudnessMaximizer& m, double fs = kFs, int channels = 2, int maxBlock = 512, int osFactor = 4,
                 Oversampler::Quality q = Oversampler::Quality::High, float lookaheadMs = 1.5f, bool truePeak = true)
{
    m.setClipOversampling (osFactor, q);
    m.setLookaheadMs (lookaheadMs);
    m.setTruePeakDetection (truePeak);
    ProcessSpec spec;
    spec.sampleRate = fs;
    spec.maxBlockSize = maxBlock;
    spec.numChannels = channels;
    m.prepare (spec);
}

MaximizerParams maxParams (float driveDb = 0.0f, float ceilingDb = -1.0f, float clipAmount = 0.5f, float glue = 0.0f,
                           float clipKnee = 0.5f, float releaseMs = 60.0f, bool autoRelease = true)
{
    MaximizerParams p;
    p.driveDb = driveDb;
    p.ceilingDb = ceilingDb;
    p.clipAmount = clipAmount;
    p.clipKnee = clipKnee;
    p.glue = glue;
    p.releaseMs = releaseMs;
    p.autoRelease = autoRelease;
    return p;
}

bool allFinite (const Planar& buf)
{
    for (const auto& c : buf.ch)
        for (float v : c)
            if (! std::isfinite (v))
                return false;
    return true;
}

double planarPeak (const Planar& buf)
{
    double p = 0.0;
    for (const auto& c : buf.ch)
        p = std::max (p, peakAbs (c.data(), static_cast<int> (c.size())));
    return p;
}

double planarTruePeak (const Planar& buf)
{
    double p = 0.0;
    for (size_t c = 0; c < buf.ch.size(); c += 2)
        p = std::max (p, independentTruePeak (buf.ch[c].data(), c + 1 < buf.ch.size() ? buf.ch[c + 1].data() : nullptr,
                                              buf.numSamples()));
    return p;
}

/** Drum-machine-like pattern at ~0 dBFS peaks: a pitch-dropping kick (120 ->
    45 Hz, 150 ms decay), a snare (noise + 190 Hz body, 60 ms) and closed hats
    (short noise ticks) on a 240 bpm 16th-note grid, plus a bass line. */
std::vector<float> drumPattern (double fs, int n, uint32_t seed)
{
    std::vector<float> v (static_cast<size_t> (n), 0.0f);
    FastRandom rng (seed);
    const int sixteenth = static_cast<int> (fs * 0.0625);
    for (int step = 0; step * sixteenth < n; ++step)
    {
        const int start = step * sixteenth;
        const bool kick = step % 4 == 0, snare = step % 8 == 4;
        const int len = std::min (n - start, static_cast<int> (fs * 0.4));
        double phase = 0.0;
        for (int i = 0; i < len; ++i)
        {
            const double t = i / fs;
            double s = 0.0;
            if (kick)
            {
                const double f = 45.0 + 75.0 * std::exp (-t / 0.03);
                phase += kTwoPi * f / fs;
                s += 0.9 * std::sin (phase) * std::exp (-t / 0.15);
            }
            if (snare)
                s += (0.6 * rng.nextBipolar() + 0.4 * std::sin (kTwoPi * 190.0 * t)) * std::exp (-t / 0.06);
            s += 0.25 * rng.nextBipolar() * std::exp (-t / 0.008); // hat
            v[static_cast<size_t> (start + i)] += static_cast<float> (s);
        }
    }
    for (int i = 0; i < n; ++i)
        v[static_cast<size_t> (i)] += 0.3f * static_cast<float> (std::sin (kTwoPi * 55.0 * i / fs));
    return v;
}

/** Low-level multi-tone well inside the band (for transparency checks). */
std::vector<float> multiTone (double fs, int n, float peak)
{
    std::vector<float> v (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        const double t = i / fs;
        v[static_cast<size_t> (i)] = static_cast<float> (0.25 * peak
                                                         * (std::sin (kTwoPi * 97.0 * t) + std::sin (kTwoPi * 1003.0 * t + 1.0)
                                                            + std::sin (kTwoPi * 5100.0 * t + 2.0) + std::sin (kTwoPi * 12000.0 * t + 0.5)));
    }
    return v;
}

/** Runs the maximizer over a whole buffer and returns the mean of the
    per-block limiter gain reduction (dB) over the second half. */
double meanLimiterGrDb (LoudnessMaximizer& m, Planar& buf, int blockSize)
{
    const int n = buf.numSamples();
    double sum = 0.0;
    int count = 0;
    for (int pos = 0; pos < n; pos += blockSize)
    {
        const int len = std::min (blockSize, n - pos);
        m.process (buf.block (pos, len));
        if (pos >= n / 2)
        {
            sum += m.getGainReductionDb();
            ++count;
        }
    }
    return count > 0 ? sum / count : 0.0;
}
} // namespace

//==============================================================================
TEST_CASE ("LoudnessMaximizer: softClip is odd, continuous, monotonic, identity below the knee and never exceeds t")
{
    for (float t : { 0.05f, 0.5f, 1.0f, 1.9f })
        for (float knee : { 0.0f, 0.1f, 0.5f, 1.0f })
        {
            const float ks = t * (1.0f - 0.5f * knee);
            float prevX = -10.0f * t;
            float prev = LoudnessMaximizer::softClip (prevX, t, knee);
            bool monotonic = true, bounded = true, odd = true, identity = true;
            double maxSlope = 0.0;
            const int steps = 20000;
            for (int k = 1; k <= steps; ++k)
            {
                const float x = -10.0f * t + 20.0f * t * static_cast<float> (k) / steps;
                const float y = LoudnessMaximizer::softClip (x, t, knee);
                monotonic = monotonic && y >= prev;
                bounded = bounded && std::abs (y) <= t;
                odd = odd && LoudnessMaximizer::softClip (-x, t, knee) == -y;
                if (std::abs (x) <= ks)
                    identity = identity && y == x;
                maxSlope = std::max (maxSlope, static_cast<double> (y - prev) / static_cast<double> (x - prevX));
                prev = y;
                prevX = x;
            }
            CHECK (monotonic);
            CHECK (bounded);
            CHECK (odd);
            CHECK (identity);
            // Continuity: the slope never exceeds 1 (Lipschitz), so there are no jumps.
            CHECK_LE (maxSlope, 1.0 + 1e-4);

            // Asymptote and extreme inputs.
            CHECK (LoudnessMaximizer::softClip (1.0e30f, t, knee) <= t);
            CHECK (LoudnessMaximizer::softClip (std::numeric_limits<float>::infinity(), t, knee) == t);
            CHECK (LoudnessMaximizer::softClip (-std::numeric_limits<float>::infinity(), t, knee) == -t);
            if (knee > 0.0f)
                CHECK_GE (LoudnessMaximizer::softClip (3.0f * t, t, knee), ks + 0.95f * (t - ks)); // approaches t
        }
    // Slope 1 at the knee start (C1 continuity): tanh'(0) = 1.
    const float t = 1.0f, knee = 0.5f, ks = 0.75f, h = 1.0e-3f;
    const float slopeAbove = (LoudnessMaximizer::softClip (ks + h, t, knee) - LoudnessMaximizer::softClip (ks, t, knee)) / h;
    CHECK_NEAR (slopeAbove, 1.0, 0.01);
    // Knee 0 is a hard clip, knee 1 starts at t / 2.
    CHECK (LoudnessMaximizer::softClip (1.5f, 1.0f, 0.0f) == 1.0f);
    CHECK (LoudnessMaximizer::softClip (0.999f, 1.0f, 0.0f) == 0.999f);
    CHECK (LoudnessMaximizer::softClip (0.5f, 1.0f, 1.0f) == 0.5f);
    CHECK (LoudnessMaximizer::softClip (0.6f, 1.0f, 1.0f) < 0.6f);
    // Degenerate threshold / knee values are safe.
    CHECK (LoudnessMaximizer::softClip (0.3f, 0.0f, 0.5f) == 0.0f);
    CHECK (LoudnessMaximizer::softClip (0.3f, -1.0f, 0.5f) == 0.0f);
    CHECK (std::abs (LoudnessMaximizer::softClip (5.0f, 1.0f, std::numeric_limits<float>::quiet_NaN())) <= 1.0f);
    CHECK (std::abs (LoudnessMaximizer::softClip (5.0f, 1.0f, 7.0f)) <= 1.0f);
}

TEST_CASE ("LoudnessMaximizer: latencySamples() = clip oversampler + limiter latency, constant with the clipper off")
{
    struct Case
    {
        int factor;
        Oversampler::Quality q;
        float lookaheadMs;
        bool truePeak;
        int expected;
    };
    const Case cases[] = {
        { 4, Oversampler::Quality::High, 1.5f, true, 36 + 72 + TruePeakDetector::kDelay },
        { 4, Oversampler::Quality::High, 2.0f, true, 36 + 96 + TruePeakDetector::kDelay },
        { 2, Oversampler::Quality::Low, 0.5f, true, 16 + 24 + TruePeakDetector::kDelay },
        { 1, Oversampler::Quality::High, 1.5f, true, 0 + 72 + TruePeakDetector::kDelay },
        { 2, Oversampler::Quality::High, 1.0f, false, 32 + 48 },
    };
    for (const auto& tc : cases)
    {
        for (float clipAmount : { 0.0f, 0.7f })
        {
            LoudnessMaximizer m;
            prepareMax (m, kFs, 2, 256, tc.factor, tc.q, tc.lookaheadMs, tc.truePeak);
            CHECK (m.latencySamples() == tc.expected);
            m.setParams (maxParams (0.0f, -1.0f, clipAmount));
            CHECK (m.latencySamples() == tc.expected);

            // A quiet impulse: with the clipper off it arrives exactly (bit-exact)
            // latencySamples() later; with the oversampled clipper on, the
            // linear-phase round trip is symmetric about the same sample.
            const int n = 1024;
            Planar buf (2, n);
            buf.ch[0][200] = dbfs (-40.0);
            buf.ch[1][300] = -dbfs (-40.0);
            processInBlocks (m, buf, 64);
            const int lat = m.latencySamples();
            if (clipAmount == 0.0f || tc.factor == 1)
            {
                CHECK (buf.ch[0][static_cast<size_t> (200 + lat)] == dbfs (-40.0));
                CHECK (buf.ch[1][static_cast<size_t> (300 + lat)] == -dbfs (-40.0));
                CHECK_NEAR (peakAbs (buf.ch[0].data(), n), dbfs (-40.0), 0.0);
            }
            else
            {
                int argMax = 0;
                for (int i = 0; i < n; ++i)
                    if (std::abs (buf.ch[0][static_cast<size_t> (i)]) > std::abs (buf.ch[0][static_cast<size_t> (argMax)]))
                        argMax = i;
                CHECK (argMax == 200 + lat);
                // (A full-band impulse loses its top octave in the round trip.)
                CHECK_NEAR (buf.ch[0][static_cast<size_t> (200 + lat)], dbfs (-40.0), dbfs (-40.0) * 0.06);
                // Symmetric (linear-phase) response around the delayed impulse.
                for (int k = 1; k < 30; ++k)
                    CHECK_NEAR (buf.ch[0][static_cast<size_t> (200 + lat - k)], buf.ch[0][static_cast<size_t> (200 + lat + k)], 1e-6);
            }
        }
    }
}

TEST_CASE ("LoudnessMaximizer: drive 0 and a -20 dBFS signal pass unchanged, only delayed by latencySamples()")
{
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        for (float clipAmount : { 0.5f, 1.0f, 0.0f })
        {
            LoudnessMaximizer m;
            prepareMax (m, fs);
            m.setParams (maxParams (0.0f, -1.0f, clipAmount));
            const int n = static_cast<int> (fs * 0.2);
            const auto x = multiTone (fs, n, dbfs (-20.0));
            Planar buf (2, n);
            setChannel (buf, 0, x);
            setChannel (buf, 1, x);
            processInBlocks (m, buf, 256);
            const int lat = m.latencySamples();
            double maxErr = 0.0;
            for (int i = lat + 200; i < n; ++i)
                maxErr = std::max (maxErr, static_cast<double> (std::abs (buf.ch[1][static_cast<size_t> (i)] - x[static_cast<size_t> (i - lat)])));
            if (clipAmount == 0.0f)
                CHECK (maxErr == 0.0); // no oversampler in the path: bit-exact
            else
                CHECK_LE (maxErr, 1e-4);
            CHECK (m.getGainReductionDb() == 0.0f);
            CHECK (m.getGlueReductionDb() == 0.0f);
            CHECK (m.getClipEnergyRatioDb() == -160.0f);
        }
    }
}

TEST_CASE ("LoudnessMaximizer: with 18 dB drive the ceiling holds on noise and drum-like programme (sample and true peak)")
{
    // Programme as it arrives from any recording chain: band-limited by the
    // ADC / resampler anti-alias filter (here: flat to 0.39 fs, gone above
    // 0.44 fs), at mastered-music level (noise at about -20 dBFS RMS, drums
    // peaking near 0 dBFS), 18 dB of drive, clipper off / light / default /
    // full, glue on and off. Judged in sample peak (always exact) and by the
    // ideal reconstruction (independent 8x FFT meter): <= +0.1 dB while the
    // clipper's own THD telemetry stays below -12 dB (it removes < 6 % of the
    // energy - already 18 dB beyond the SafetyGovernor's -30 dB budget).
    // Harder clipping creates intermodulation right up to fs/2, above the
    // limiter's 4x detector band (flat to 0.4535 fs); there the ideal-
    // reconstruction peak may exceed the ceiling slightly (measured
    // <= +0.3 dB), which is bounded here as a documented limitation.
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        const int n = static_cast<int> (fs * 0.3);
        const int tail = static_cast<int> (fs * 0.02);
        const auto noiseL = bandLimit (whiteNoise (n - tail - 200, 0.17f, 11));
        const auto noiseR = bandLimit (whiteNoise (n - tail - 200, 0.17f, 12));
        const auto drumsL = bandLimit (drumPattern (fs, n - tail - 200, 13));
        const auto drumsR = bandLimit (drumPattern (fs, n - tail - 200, 14));
        struct Prog
        {
            const char* name;
            const std::vector<float>*l, *r;
        };
        const Prog progs[] = { { "noise", &noiseL, &noiseR }, { "drums", &drumsL, &drumsR } };
        for (const auto& prog : progs)
        {
            for (int variant = 0; variant < 5; ++variant)
            {
                const float clipAmounts[] = { 0.0f, 0.25f, 0.5f, 0.5f, 1.0f };
                const float glues[] = { 0.7f, 0.0f, 0.0f, 0.7f, 0.3f };
                const float ceilings[] = { -1.0f, -1.0f, -1.0f, -0.3f, -1.0f };
                const float knees[] = { 0.5f, 1.0f, 0.5f, 0.2f, 0.0f };
                const auto v = static_cast<size_t> (variant);
                const float ceilingDb = ceilings[v];
                LoudnessMaximizer m;
                prepareMax (m, fs);
                m.setParams (maxParams (18.0f, ceilingDb, clipAmounts[v], glues[v], knees[v]));
                Planar buf (2, n);
                setChannel (buf, 0, *prog.l);
                setChannel (buf, 1, *prog.r);
                double clipEnergy = 0.0;
                int blocks = 0;
                for (int pos = 0; pos < n; pos += 256, ++blocks)
                {
                    m.process (buf.block (pos, std::min (256, n - pos)));
                    clipEnergy += std::pow (10.0, m.getClipEnergyRatioDb() / 10.0);
                }
                const double clipDb = 10.0 * std::log10 (clipEnergy / blocks + 1e-30);
                const double sp = planarPeak (buf), tp = planarTruePeak (buf), ceil = dbfs (ceilingDb);
                const double tol = clipDb <= -12.0 ? kTpTolerance : dbfs (0.5);
                if (! (sp <= ceil && tp <= ceil * tol))
                    std::cerr << "    case: " << prog.name << " @ " << fs << " Hz, variant " << variant << ": sample peak " << toDb (sp)
                              << " dB, true peak " << toDb (tp) << " dB (ceiling " << ceilingDb << "), clip energy " << clipDb << " dB\n";
                CHECK_LE (sp, ceil);
                CHECK_LE (tp, ceil * tol);
                CHECK (allFinite (buf));
                // Loud: the drive really went into the stages.
                CHECK_GE (tp, ceil * dbfs (-0.5));
                CHECK_LE (m.getGainReductionDb(), 0.0f);
            }
        }
    }
}

TEST_CASE ("LoudnessMaximizer: extreme and full-band input still holds the sample ceiling exactly")
{
    // Synthetic full-band content (raw white noise, hats made of raw noise)
    // and absurd clipping (18 dB into dense noise that already sits at
    // -11 dBFS RMS: the clipper removes a quarter of the energy) put strong
    // content between 0.41 fs and fs/2, where the limiter's 4x detector
    // under-reads. The sample-peak ceiling is still exact; the ideal-
    // reconstruction peak is only bounded loosely (documented limitation:
    // measured <= +0.3 dB for band-limited input, up to ~+1.7 dB for raw
    // white noise).
    for (double fs : { 44100.0, 48000.0 })
    {
        const int n = static_cast<int> (fs * 0.2);
        const int tail = static_cast<int> (fs * 0.02);
        const auto rawL = whiteNoise (n - tail, 0.5f, 15);
        const auto rawR = whiteNoise (n - tail, 0.5f, 16);
        const auto drumsL = drumPattern (fs, n - tail, 17);
        const auto drumsR = drumPattern (fs, n - tail, 18);
        const auto denseL = bandLimit (whiteNoise (n - tail - 200, 0.5f, 19));
        const auto denseR = bandLimit (whiteNoise (n - tail - 200, 0.5f, 20));
        const std::vector<float>* progs[3][2] = { { &rawL, &rawR }, { &drumsL, &drumsR }, { &denseL, &denseR } };
        for (const auto& prog : progs)
            for (float clipAmount : { 0.0f, 0.5f, 1.0f })
            {
                LoudnessMaximizer m;
                prepareMax (m, fs);
                m.setParams (maxParams (18.0f, -1.0f, clipAmount, 0.5f, clipAmount == 1.0f ? 0.0f : 0.5f));
                Planar buf (2, n);
                setChannel (buf, 0, *prog[0]);
                setChannel (buf, 1, *prog[1]);
                processInBlocks (m, buf, 256);
                CHECK_LE (planarPeak (buf), dbfs (-1.0));
                CHECK_LE (planarTruePeak (buf), dbfs (-1.0 + 2.0));
                CHECK (allFinite (buf));
            }
    }
}

TEST_CASE ("LoudnessMaximizer: clipAmount 0 disables the clipper (telemetry -160 dB); clipping shows up in the telemetry")
{
    const int n = 24000;
    const auto x = sine (220.0, kFs, n, 0.5f);

    // Off: -160 dB even when driven hard (that the path is then a bit-exact
    // delay is checked by the transparency test above).
    {
        LoudnessMaximizer m;
        prepareMax (m);
        m.setParams (maxParams (18.0f, -1.0f, 0.0f));
        Planar buf (2, n);
        setChannel (buf, 0, x);
        setChannel (buf, 1, x);
        for (int pos = 0; pos < n; pos += 256)
        {
            m.process (buf.block (pos, std::min (256, n - pos)));
            CHECK (m.getClipEnergyRatioDb() == -160.0f);
        }
        CHECK_LE (m.getGainReductionDb(), -10.0); // the limiter does all the work
    }

    // On (crest gate off, depth uncapped: the plain curve clips the steady
    // sine): the energy ratio tracks how hard the clipper works.
    double ratio[3] {};
    const float amounts[] = { 0.2f, 0.6f, 1.0f };
    for (int k = 0; k < 3; ++k)
    {
        LoudnessMaximizer m;
        prepareMax (m);
        auto p = maxParams (18.0f, -1.0f, amounts[k]);
        p.clipCrestDb = 0.0f;
        p.clipMaxDepthDb = 24.0f;
        m.setParams (p);
        Planar buf (2, n);
        setChannel (buf, 0, x);
        setChannel (buf, 1, x);
        processInBlocks (m, buf, 256);
        ratio[k] = m.getClipEnergyRatioDb();
        CHECK (ratio[k] > -160.0);
        CHECK (ratio[k] < 0.0);
    }
    CHECK (ratio[0] < ratio[1]);
    CHECK (ratio[1] < ratio[2]);

    // Defaults (docs/11 E05 crest gate): the same steady sine at 18 dB drive
    // never stands 6 dB out of its own RMS, so it is not clipped at all.
    {
        LoudnessMaximizer m;
        prepareMax (m);
        m.setParams (maxParams (18.0f, -1.0f, 1.0f));
        Planar buf (2, n);
        setChannel (buf, 0, x);
        setChannel (buf, 1, x);
        processInBlocks (m, buf, 256);
        CHECK (m.getClipEnergyRatioDb() == -160.0f);
        CHECK_LE (m.getGainReductionDb(), -10.0);
    }

    // Quiet input with the clipper on: nothing is clipped.
    LoudnessMaximizer m;
    prepareMax (m);
    m.setParams (maxParams (0.0f, -1.0f, 1.0f));
    Planar buf (2, 4096);
    setChannel (buf, 0, sine (220.0, kFs, 4096, 0.1f));
    processInBlocks (m, buf, 512);
    CHECK (m.getClipEnergyRatioDb() == -160.0f);
}

TEST_CASE ("LoudnessMaximizer: the clipper shaves transients so the limiter reduces less")
{
    // Drum programme: sharp transients on top of a denser body.
    const int n = static_cast<int> (kFs * 1.0);
    const auto drums = drumPattern (kFs, n, 21);
    double gr[2] {};
    for (int k = 0; k < 2; ++k)
    {
        LoudnessMaximizer m;
        prepareMax (m);
        m.setParams (maxParams (12.0f, -1.0f, k == 0 ? 0.0f : 1.0f));
        Planar buf (2, n);
        setChannel (buf, 0, drums);
        setChannel (buf, 1, drums);
        gr[k] = meanLimiterGrDb (m, buf, 256);
    }
    CHECK_GE (gr[1], gr[0] + 0.5); // less limiter gain reduction with the clipper
}

TEST_CASE ("LoudnessMaximizer: glue > 0 reduces limiter gain reduction on a bass-heavy signal")
{
    const int n = static_cast<int> (kFs * 1.0);
    std::vector<float> x (static_cast<size_t> (n));
    FastRandom rng (3);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kFs;
        x[static_cast<size_t> (i)] = static_cast<float> (0.8 * std::sin (kTwoPi * 50.0 * t) + 0.08 * std::sin (kTwoPi * 800.0 * t)
                                                         + 0.03 * rng.nextBipolar());
    }
    double gr[2] {}, glueGr[2] {};
    for (int k = 0; k < 2; ++k)
    {
        LoudnessMaximizer m;
        prepareMax (m);
        // Clipper off, so the limiter alone handles what the glue leaves.
        m.setParams (maxParams (12.0f, -1.0f, 0.0f, k == 0 ? 0.0f : 1.0f));
        Planar buf (2, n);
        setChannel (buf, 0, x);
        setChannel (buf, 1, x);
        gr[k] = meanLimiterGrDb (m, buf, 256);
        glueGr[k] = m.getGlueReductionDb();
    }
    CHECK (gr[1] > gr[0] + 3.0);
    CHECK (glueGr[0] == 0.0);
    CHECK (glueGr[1] < -1.0);
}

TEST_CASE ("LoudnessMaximizer: parameter changes and stage on/off switches are click-free")
{
    // A 100 Hz tone: its second difference is tiny (A (2 pi f / fs)^2), so any
    // step or kink in the output shows up as a spike.
    const int n = static_cast<int> (kFs * 2.0);
    const auto x = sine (100.0, kFs, n, 0.35f);
    LoudnessMaximizer m;
    prepareMax (m, kFs, 2, 512);
    m.setParams (maxParams (6.0f, -1.0f, 0.5f, 0.0f));
    Planar buf (2, n);
    setChannel (buf, 0, x);
    setChannel (buf, 1, x);
    const int block = 128;
    for (int pos = 0, b = 0; pos < n; pos += block, ++b)
    {
        MaximizerParams p = maxParams (6.0f, -1.0f, 0.5f, 0.0f);
        const int phase = b / 60; // ~160 ms per stage
        if (phase % 2 == 1)
            p.glue = 0.8f;       // glue on / off
        if (phase % 3 == 1)
            p.clipAmount = 0.0f; // clipper off / on
        if (phase % 4 == 2)
            p.driveDb = 14.0f;   // drive jump
        if (phase % 5 == 3)
            p.ceilingDb = -6.0f; // ceiling jump
        if (phase % 7 == 5)
            p.clipKnee = 0.0f;
        m.setParams (p);
        m.process (buf.block (pos, std::min (block, n - pos)));
    }
    CHECK (allFinite (buf));
    const double baseline = 0.35 * std::pow (10.0, 14.0 / 20.0) * std::pow (kTwoPi * 100.0 / kFs, 2.0);
    double maxD2 = 0.0;
    const auto& y = buf.ch[0];
    for (int i = 2000; i < n; ++i)
        maxD2 = std::max (maxD2, static_cast<double> (std::abs (y[static_cast<size_t> (i)] - 2.0f * y[static_cast<size_t> (i - 1)]
                                                                + y[static_cast<size_t> (i - 2)])));
    // 14x (12x until docs/11 E05 stage 1; measured 9.8x before, 12.7x
    // after): during the 50 ms glide to 14 dB drive the crest-gated clipper's
    // RMS lags the rising tone and shaves a few peaks, and the limiter's
    // program envelope hands over to its fast gain through a min(); both
    // bend the waveform a little harder, neither steps it (a 0.25 % gain
    // step on this tone alone reads 14x).
    CHECK_LE (maxD2, 14.0 * baseline);
    CHECK_LE (planarPeak (buf), 1.0);
}

TEST_CASE ("LoudnessMaximizer: the clip crest gate and depth cap (max.clipCrest / max.clipMaxDb) glide - no step when they move, the new setting once the glide is over (docs/11 E05 step 1)")
{
    // A 100 Hz tone clipped hard (crest gate off, 14 dB drive), then the depth
    // cap and the crest gate jump: 3 -> 12 dB and 0 -> 6 dB and back, every
    // 200 ms. They used to take effect at once (the cap's gain stepped).
    const int n = static_cast<int> (kFs * 1.2);
    const auto x = sine (100.0, kFs, n, 0.35f);
    const auto run = [&] (bool jumps, float crestDb, float depthDb) {
        LoudnessMaximizer m;
        prepareMax (m, kFs, 2, 512);
        Planar buf (2, n);
        setChannel (buf, 0, x);
        setChannel (buf, 1, x);
        const int block = 128;
        for (int pos = 0, b = 0; pos < n; pos += block, ++b)
        {
            MaximizerParams p = maxParams (14.0f, -1.0f, 1.0f, 0.0f, 0.2f);
            p.clipCrestDb = crestDb;
            p.clipMaxDepthDb = depthDb;
            if (jumps && (b / 75) % 2 == 1) // 200 ms per state
            {
                p.clipCrestDb = 6.0f;
                p.clipMaxDepthDb = 12.0f;
            }
            m.setParams (p);
            m.process (buf.block (pos, std::min (block, n - pos)));
        }
        return buf;
    };
    const Planar moved = run (true, 0.0f, 3.0f);
    CHECK (allFinite (moved));
    // The largest second difference while the settings move, against the
    // same tone held at either setting (its own bends: the clipper's knee).
    const auto maxD2 = [] (const Planar& b, int from, int to) {
        double d = 0.0;
        const auto& y = b.ch[0];
        for (int i = std::max (from, 2); i < to; ++i)
            d = std::max (d, static_cast<double> (std::abs (y[static_cast<size_t> (i)] - 2.0f * y[static_cast<size_t> (i - 1)]
                                                          + y[static_cast<size_t> (i - 2)])));
        return d;
    };
    const Planar heldA = run (false, 0.0f, 3.0f), heldB = run (false, 6.0f, 12.0f);
    const double steady = std::max (maxD2 (heldA, 2000, n), maxD2 (heldB, 2000, n));
    const double moving = maxD2 (moved, 2000, n);
    std::printf ("    measured second difference moving / held = %.2f\n", moving / steady);
    CHECK_LE (moving, 1.5 * steady);
    // The last change (to 6 / 12 dB) is at 1.0 s; 150 ms later the output is
    // that held setting's: the glide ends exactly on it.
    const int from = static_cast<int> (kFs * 1.15);
    double diff = 0.0;
    for (int i = from; i < n; ++i)
        diff = std::max (diff, static_cast<double> (std::abs (moved.ch[0][static_cast<size_t> (i)] - heldB.ch[0][static_cast<size_t> (i)])));
    std::printf ("    measured largest difference to the held setting 150 ms after the change = %.2g\n", diff);
    CHECK_LE (diff, 1.0e-3);
}

TEST_CASE ("LoudnessMaximizer: release and ceiling are passed to the limiter")
{
    auto grAfterBurst = [] (float releaseMs) {
        LoudnessMaximizer m;
        prepareMax (m);
        m.setParams (maxParams (0.0f, -1.0f, 0.0f, 0.0f, 0.5f, releaseMs, false));
        const int n = 24000;
        Planar buf (2, n);
        for (int i = 1000; i < 3000; ++i)
            buf.ch[0][static_cast<size_t> (i)] = 3.0f * static_cast<float> (std::sin (kTwoPi * 440.0 * i / kFs));
        // The last block (0.34 .. 0.44 s after the burst) reports the gain
        // that has recovered least in it.
        processInBlocks (m, buf, 4800);
        return m.getGainReductionDb();
    };
    CHECK (grAfterBurst (500.0f) < grAfterBurst (20.0f) - 1.0f);

    LoudnessMaximizer m;
    prepareMax (m);
    m.setParams (maxParams (0.0f, -6.0f, 0.0f));
    Planar buf (2, 9600);
    setChannel (buf, 0, sine (1000.0, kFs, 9600, 1.0f));
    processInBlocks (m, buf, 512);
    CHECK_NEAR (toDb (peakAbs (buf.ch[0].data() + 4800, 4800)), -6.05, 0.05);
}

TEST_CASE ("LoudnessMaximizer: reset, setParams and process do not allocate")
{
    LoudnessMaximizer m;
    prepareMax (m, kFs, 2, 512);
    Planar buf (2, 512);
    setChannel (buf, 0, whiteNoise (512, 0.8f, 1));
    setChannel (buf, 1, whiteNoise (512, 0.8f, 2));

    AllocationGuard guard;
    m.reset();
    for (int b = 0; b < 24; ++b)
    {
        m.setParams (maxParams (static_cast<float> (b % 4) * 6.0f, b % 2 == 0 ? -1.0f : -3.0f, b % 3 == 0 ? 0.0f : 0.8f,
                                b % 5 < 2 ? 0.0f : 0.6f, 0.3f, b % 2 == 0 ? 30.0f : 300.0f, b % 4 != 0));
        m.process (buf.block());
        m.process (buf.block (0, 7));
        (void) m.getGainReductionDb();
        (void) m.getGlueReductionDb();
        (void) m.getClipEnergyRatioDb();
    }
    m.reset();
    CHECK (guard.allocations() == 0);
}

TEST_CASE ("LoudnessMaximizer: silence, DC, full-scale noise, impulses and extreme settings stay finite and under the ceiling")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        for (int channels : { 1, 2, 8 })
        {
            LoudnessMaximizer m;
            prepareMax (m, fs, channels, 1024, channels == 8 ? 2 : 4);
            const int n = 6000;
            for (int sig = 0; sig < 6; ++sig)
            {
                Planar buf (channels, n);
                for (int c = 0; c < channels; ++c)
                {
                    auto& d = buf.ch[static_cast<size_t> (c)];
                    switch (sig)
                    {
                        case 0: break;                                                             // silence
                        case 1: std::fill (d.begin(), d.end(), c % 2 == 0 ? 1.0f : -0.5f); break; // DC
                        case 2: d = whiteNoise (n, 1.0f, static_cast<uint32_t> (c + 1)); break;   // full-scale noise
                        case 3: d = whiteNoise (n, 100.0f, static_cast<uint32_t> (c + 9)); break; // +40 dB
                        case 4: d[10] = 1.0f; d[11] = -1.0f; d[3000] = 30.0f; break;              // impulses
                        default: d = sine (fs * 0.49, fs, n, 2.0f); break;                        // near Nyquist
                    }
                }
                buf.ptrs.clear();
                for (auto& c : buf.ch)
                    buf.ptrs.push_back (c.data());

                MaximizerParams p;
                switch (sig % 3)
                {
                    case 0: p = maxParams (24.0f, 0.0f, 1.0f, 1.0f, 0.0f, 5.0f, true); break;
                    case 1: p = maxParams (100.0f, -100.0f, 7.0f, -3.0f, 9.0f, 1.0e9f, false); break; // clamped
                    default: p = maxParams (nan, nan, nan, nan, nan, nan, true); break;               // ignored
                }
                m.setParams (p);
                processInBlocks (m, buf, sig % 2 == 0 ? 1024 : 333);
                CHECK (allFinite (buf));
                CHECK_LE (planarPeak (buf), 1.0); // the ceiling never exceeds 0 dBFS
                CHECK (std::isfinite (m.getGainReductionDb()));
                CHECK (std::isfinite (m.getGlueReductionDb()));
                CHECK (std::isfinite (m.getClipEnergyRatioDb()));
                CHECK (m.getClipEnergyRatioDb() <= 0.0f);
            }
        }
    }
}

TEST_CASE ("LoudnessMaximizer: NaN / Inf input is contained and the maximizer recovers")
{
    LoudnessMaximizer m;
    prepareMax (m);
    m.setParams (maxParams (6.0f, -1.0f, 0.5f, 0.8f));
    const int n = 48000;
    Planar buf (2, n);
    setChannel (buf, 0, sine (300.0, kFs, n, 0.3f));
    setChannel (buf, 1, sine (300.0, kFs, n, 0.3f));
    buf.ch[0][2000] = std::numeric_limits<float>::quiet_NaN();
    buf.ch[1][5000] = std::numeric_limits<float>::infinity();
    processInBlocks (m, buf, 256);
    CHECK (allFinite (buf));
    CHECK_LE (planarPeak (buf), dbfs (-1.0));
    // Recovered: the tail carries the tone again at a sensible level.
    CHECK_GE (toneAmplitude (buf.ch[0].data() + 38400, 9600, 300.0, kFs), 0.3);
}

TEST_CASE ("LoudnessMaximizer: output is independent of the host block size (1, 7, 64, 512)")
{
    const int n = 12000;
    const auto l = drumPattern (kFs, n, 41);
    const auto r = whiteNoise (n, 0.4f, 42);
    std::vector<std::vector<float>> outputs;
    for (int bs : { 1, 7, 64, 512 })
    {
        LoudnessMaximizer m;
        prepareMax (m, kFs, 2, 512);
        m.setParams (maxParams (12.0f, -1.0f, 0.7f, 0.6f, 0.4f, 40.0f, true));
        Planar buf (2, n);
        setChannel (buf, 0, l);
        setChannel (buf, 1, r);
        processInBlocks (m, buf, bs);
        std::vector<float> both (buf.ch[0]);
        both.insert (both.end(), buf.ch[1].begin(), buf.ch[1].end());
        outputs.push_back (both);
    }
    for (size_t k = 1; k < outputs.size(); ++k)
    {
        double maxDiff = 0.0;
        for (size_t i = 0; i < outputs[0].size(); ++i)
            maxDiff = std::max (maxDiff, static_cast<double> (std::abs (outputs[k][i] - outputs[0][i])));
        CHECK_LE (maxDiff, 1e-5);
    }
}

// ---- adversarial review tests ----
TEST_CASE ("LoudnessMaximizer [adversarial]: a channel that leaves and rejoins never replays stale audio")
{
    // Stereo DC on the right, then 100 ms of mono blocks, then stereo
    // silence: the right channel must come back silent (it used to release
    // 0.5 of stale DC from the dry delay / oversampler lanes that stood still
    // meanwhile). The gap is long enough for the glue splitter's own IIR
    // ringing to have decayed below the tolerance.
    for (float clipAmount : { 0.0f, 0.5f })
        for (float glue : { 0.0f, 0.6f })
        {
            LoudnessMaximizer m;
            prepareMax (m, kFs, 2, 256);
            m.setParams (maxParams (0.0f, -1.0f, clipAmount, glue));
            Planar st (2, 256);
            std::fill (st.ch[1].begin(), st.ch[1].end(), 0.5f);
            m.process (st.block());
            Planar mono (1, 256);
            for (int b = 0; b < 20; ++b)
            {
                std::fill (mono.ch[0].begin(), mono.ch[0].end(), 0.0f);
                m.process (mono.block());
            }
            Planar back (2, 256);
            m.process (back.block());
            CHECK_LE (peakAbs (back.ch[1].data(), 256), 1e-6);
            CHECK_LE (peakAbs (back.ch[0].data(), 256), 1e-6);
        }
}

TEST_CASE ("LoudnessMaximizer [adversarial]: a NaN in one channel does not glitch the other channel")
{
    // Glue on (shared splitter), clipper on: a corrupt sample in the left
    // channel used to reset the splitter of every channel, dropping one
    // sample of the healthy right channel and restarting its filters.
    auto run = [] (bool inject) {
        LoudnessMaximizer m;
        prepareMax (m);
        m.setParams (maxParams (0.0f, -1.0f, 0.5f, 0.8f));
        const int n = 9600;
        Planar buf (2, n);
        setChannel (buf, 0, sine (300.0, kFs, n, 0.05f));
        setChannel (buf, 1, sine (300.0, kFs, n, 0.2f));
        if (inject)
        {
            buf.ch[0][4000] = std::numeric_limits<float>::quiet_NaN();
            buf.ch[0][6000] = std::numeric_limits<float>::infinity();
        }
        processInBlocks (m, buf, 256);
        return buf;
    };
    const auto ref = run (false);
    const auto hit = run (true);
    CHECK (allFinite (hit));
    double errRight = 0.0;
    for (int i = 0; i < hit.numSamples(); ++i)
        errRight = std::max (errRight, static_cast<double> (std::abs (hit.ch[1][static_cast<size_t> (i)] - ref.ch[1][static_cast<size_t> (i)])));
    CHECK_LE (errRight, 1e-3); // (linked glue level: the left lane is 12 dB quieter)
}

TEST_CASE ("LoudnessMaximizer [adversarial]: clip-energy telemetry equals the header formula")
{
    // 1x oversampling, glue off, parameters applied instantly: the clipper
    // input is exactly x * drive, so the telemetry can be recomputed,
    // including the crest gate (t' = max (t, 10^(crest/20) sqrt (P)), P the
    // linked max-channel power through two cascaded 5 ms one-poles, updated
    // before each sample is clipped) and the depth cap (softClipCapped).
    const float driveDb = 12.0f, ceilingDb = -2.0f, clipAmount = 0.7f, knee = 0.4f;
    LoudnessMaximizer m;
    prepareMax (m, kFs, 2, 512, 1);
    auto params = maxParams (driveDb, ceilingDb, clipAmount, 0.0f, knee);
    params.clipCrestDb = 3.0f;    // low enough for noise peaks to clip, and
    params.clipMaxDepthDb = 2.0f; // a cap they reach
    m.setParams (params);
    const int n = 4096;
    const auto l = sine (220.0, kFs, n, 0.3f);
    const auto r = whiteNoise (n, 0.2f, 91);
    Planar buf (2, n);
    setChannel (buf, 0, l);
    setChannel (buf, 1, r);
    const float drive = dbToGain (driveDb);
    const float t = dbToGain (ceilingDb + lerp (6.0f, 0.3f, clipAmount));
    const float crest = dbToGain (params.clipCrestDb), depth = dbToGain (-params.clipMaxDepthDb);
    const float powerCoeff = 1.0f - onePoleCoeff (5.0f, kFs);
    float powerFast = 0.0f, power = 0.0f;
    std::vector<float> tEff (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        const float a = l[static_cast<size_t> (i)] * drive, b = r[static_cast<size_t> (i)] * drive;
        powerFast += powerCoeff * (std::max (a * a, b * b) - powerFast);
        power += powerCoeff * (powerFast - power);
        tEff[static_cast<size_t> (i)] = std::max (t, crest * std::sqrt (power));
    }
    int clippedBlocks = 0;
    for (int pos = 0; pos < n; pos += 512)
    {
        m.process (buf.block (pos, 512));
        double diff = 0.0, in = 0.0;
        for (const auto* src : { &l, &r })
            for (int i = pos; i < pos + 512; ++i)
            {
                const float x = (*src)[static_cast<size_t> (i)] * drive;
                const double d = static_cast<double> (x - LoudnessMaximizer::softClipCapped (x, tEff[static_cast<size_t> (i)], knee, depth));
                diff += d * d;
                in += static_cast<double> (x) * static_cast<double> (x);
            }
        if (diff > 0.0)
        {
            ++clippedBlocks;
            CHECK_NEAR (m.getClipEnergyRatioDb(), 10.0 * std::log10 (diff / in), 0.01);
        }
        else
        {
            CHECK (m.getClipEnergyRatioDb() == -160.0f);
        }
    }
    CHECK_GE (clippedBlocks, 4); // the noise channel's peaks stand out of its RMS
}

TEST_CASE ("LoudnessMaximizer [adversarial]: glue is 2:1 above ceiling - 6 dB and an all-pass (no gain) below it")
{
    // Below threshold: every band gain is 1, so the output is the splitter's
    // all-pass sum: flat magnitude at any frequency, zero glue reduction.
    for (double f : { 50.0, 120.0, 1000.0, 4000.0, 12000.0 })
    {
        LoudnessMaximizer m;
        prepareMax (m);
        m.setParams (maxParams (0.0f, -1.0f, 0.0f, 1.0f));
        const int n = 24000;
        Planar buf (2, n);
        setChannel (buf, 0, sine (f, kFs, n, dbfs (-20.0)));
        setChannel (buf, 1, sine (f, kFs, n, dbfs (-20.0)));
        processInBlocks (m, buf, 256);
        CHECK_NEAR (toDb (toneAmplitude (buf.ch[0].data() + 12000, 12000, f, kFs)), -20.0, 0.05);
        CHECK (m.getGlueReductionDb() == 0.0f);
    }

    // Above threshold, a 1 kHz tone sits in the mid band: out = sqrt (T * A).
    // Ceiling 0 dB -> T = -6.02 dBFS; A = 0.8 -> out -3.98 dBFS, gain -2.04 dB.
    LoudnessMaximizer m;
    prepareMax (m);
    m.setParams (maxParams (0.0f, 0.0f, 0.0f, 1.0f));
    const int n = 48000;
    Planar buf (2, n);
    setChannel (buf, 0, sine (1000.0, kFs, n, 0.8f));
    setChannel (buf, 1, sine (1000.0, kFs, n, 0.8f));
    processInBlocks (m, buf, 480);
    const double expected = 0.5 * (toDb (0.8) + (-6.0206));
    CHECK_NEAR (toDb (toneAmplitude (buf.ch[0].data() + 24000, 24000, 1000.0, kFs)), expected, 0.15);
    CHECK_NEAR (m.getGlueReductionDb(), expected - toDb (0.8), 0.15);
    CHECK (m.getGainReductionDb() == 0.0f); // -3.98 dBFS: the limiter has nothing to do
}

TEST_CASE ("LoudnessMaximizer [adversarial]: automation of every parameter and stage switch is block-size independent")
{
    // Parameter changes at multiples of 512 samples (so every host block
    // size sees them at the same sample): drive, ceiling, clip on/off, glue
    // on/off, knee, release. Everything runs per sample, so the outputs must
    // agree to float rounding for block sizes 1, 64 and 512.
    const int n = 48000;
    const auto l = drumPattern (kFs, n, 81);
    const auto r = bandLimit (whiteNoise (n, 0.3f, 82));
    std::vector<MaximizerParams> sched;
    FastRandom rng (83);
    for (int k = 0; k * 512 < n; ++k)
    {
        MaximizerParams p = maxParams (12.0f, -1.0f, 0.5f, 0.3f);
        if ((k / 6) % 2 == 1)
            p.glue = 0.0f;
        if ((k / 5) % 3 == 1)
            p.clipAmount = 0.0f;
        if ((k / 7) % 2 == 1)
        {
            p.driveDb = 20.0f * (0.5f + 0.5f * rng.nextBipolar());
            p.ceilingDb = -6.0f * (0.5f + 0.5f * rng.nextBipolar());
            p.clipKnee = 0.5f + 0.5f * rng.nextBipolar();
            p.releaseMs = 200.0f;
            p.autoRelease = false;
        }
        sched.push_back (p);
    }
    std::vector<std::vector<float>> outs;
    for (int bs : { 1, 64, 512 })
    {
        LoudnessMaximizer m;
        prepareMax (m, kFs, 2, 512);
        Planar buf (2, n);
        setChannel (buf, 0, l);
        setChannel (buf, 1, r);
        for (int pos = 0; pos < n; pos += bs)
        {
            if (pos % 512 == 0)
                m.setParams (sched[static_cast<size_t> (pos / 512)]);
            m.process (buf.block (pos, std::min (bs, n - pos)));
        }
        CHECK (allFinite (buf));
        CHECK_LE (planarPeak (buf), 1.0);
        std::vector<float> both (buf.ch[0]);
        both.insert (both.end(), buf.ch[1].begin(), buf.ch[1].end());
        outs.push_back (both);
    }
    for (size_t k = 1; k < outs.size(); ++k)
    {
        double maxDiff = 0.0;
        for (size_t i = 0; i < outs[0].size(); ++i)
            maxDiff = std::max (maxDiff, static_cast<double> (std::abs (outs[k][i] - outs[0][i])));
        CHECK_LE (maxDiff, 1e-6);
    }
}

TEST_CASE ("LoudnessMaximizer [adversarial]: 192 kHz and the latency profiles hold the ceiling with 18 dB drive")
{
    struct Case
    {
        double fs;
        int factor;
        Oversampler::Quality q;
        float lookaheadMs;
    };
    // Quality (4x HQ, 2 ms), LowLatency (2x Low, 0.5 ms), Balanced (4x HQ, 1.5 ms).
    const Case cases[] = { { 192000.0, 4, Oversampler::Quality::High, 1.5f }, { 44100.0, 2, Oversampler::Quality::Low, 0.5f },
                           { 48000.0, 4, Oversampler::Quality::High, 2.0f }, { 96000.0, 2, Oversampler::Quality::Low, 0.5f } };
    for (const auto& tc : cases)
    {
        const int n = static_cast<int> (tc.fs * 0.25);
        const int tail = static_cast<int> (tc.fs * 0.02);
        const auto dl = bandLimit (drumPattern (tc.fs, n - tail - 200, 85));
        const auto dr = bandLimit (drumPattern (tc.fs, n - tail - 200, 86));
        for (float clipAmount : { 0.0f, 0.4f })
        {
            LoudnessMaximizer m;
            prepareMax (m, tc.fs, 2, 512, tc.factor, tc.q, tc.lookaheadMs);
            m.setParams (maxParams (18.0f, -1.0f, clipAmount, 0.5f));
            Planar buf (2, n);
            setChannel (buf, 0, dl);
            setChannel (buf, 1, dr);
            double clipEnergy = 0.0;
            int blocks = 0;
            for (int pos = 0; pos < n; pos += 512, ++blocks)
            {
                m.process (buf.block (pos, std::min (512, n - pos)));
                clipEnergy += std::pow (10.0, m.getClipEnergyRatioDb() / 10.0);
            }
            const double clipDb = 10.0 * std::log10 (clipEnergy / blocks + 1e-30);
            const double sp = planarPeak (buf), tp = planarTruePeak (buf), ceil = dbfs (-1.0);
            if (! (sp <= ceil && tp <= ceil * kTpTolerance))
                std::cerr << "    fs " << tc.fs << " x" << tc.factor << " clip " << clipAmount << ": sp " << toDb (sp) << " tp " << toDb (tp)
                          << " clip energy " << clipDb << "\n";
            CHECK_LE (sp, ceil);
            CHECK_LE (tp, ceil * (clipDb <= -12.0 ? kTpTolerance : dbfs (0.5)));
            CHECK_GE (tp, ceil * dbfs (-0.5));
        }
    }
}

TEST_CASE ("LoudnessMaximizer: with the crest-gated clipper and the LF-safe limiter the ceiling holds on the 11-rate matrix at 18 dB drive (docs/11 E05 stage 1)")
{
    // 8 .. 192 kHz, drums plus a 45 Hz bass line (the period hold engages)
    // in the Balanced structure (4x HQ clipper, 1.5 ms look-ahead), clipper
    // at its default share: sample peak exactly under the ceiling, true
    // peak within the documented 0.1 dB, no safety clamp.
    for (double fs : { 8000.0, 11025.0, 16000.0, 22050.0, 32000.0, 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
    {
        const int n = static_cast<int> (fs * 0.5);
        const int tail = static_cast<int> (fs * 0.02);
        auto dl = drumPattern (fs, n - tail - 200, 31), dr = drumPattern (fs, n - tail - 200, 32);
        for (size_t i = 0; i < dl.size(); ++i)
        {
            const auto bass = static_cast<float> (0.5 * std::sin (kTwoPi * 45.0 * static_cast<double> (i) / fs));
            dl[i] += bass;
            dr[i] += bass;
        }
        dl = bandLimit (dl);
        dr = bandLimit (dr);
        LoudnessMaximizer m;
        prepareMax (m, fs, 2, 512);
        m.setParams (maxParams (18.0f, -1.0f, 0.5f, 0.0f));
        Planar buf (2, n);
        setChannel (buf, 0, dl);
        setChannel (buf, 1, dr);
        processInBlocks (m, buf, 512);
        const double sp = planarPeak (buf), tp = planarTruePeak (buf), ceil = dbfs (-1.0);
        if (! (sp <= ceil && tp <= ceil * kTpTolerance))
            std::cerr << "    fs " << fs << ": sp " << toDb (sp) << " tp " << toDb (tp) << "\n";
        CHECK_LE (sp, ceil);
        CHECK_LE (tp, ceil * kTpTolerance);
        CHECK (m.getSafetyClipCount() == 0u);
        CHECK_GE (tp, ceil * dbfs (-0.5)); // limited, not muted
    }
}

//==============================================================================
// docs/11 E05 step 5: the LF-first limiter in the glue path (max.lfLimit).
namespace
{
/** 55 Hz kicks (peak 0.5, tau 100 ms) every 500 ms under a 2 kHz tone at
    0.1: the E59 ducking scene. */
std::vector<float> kicksUnderTone (int n)
{
    std::vector<float> x (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kFs, beat = std::fmod (t + 0.25, 0.5);
        const double kick = t >= 0.25 && beat < 0.35 ? 0.5 * std::exp (-beat / 0.1) * std::sin (kTwoPi * 55.0 * beat) : 0.0;
        x[static_cast<size_t> (i)] = static_cast<float> (0.1 * std::sin (kTwoPi * 2000.0 * t) + kick);
    }
    return x;
}

/** Renders mono x on both channels (latency removed) through a fresh maximizer. */
std::vector<float> renderMax (const std::vector<float>& x, const MaximizerParams& p, int block = 512, LoudnessMaximizer* keep = nullptr)
{
    LoudnessMaximizer local;
    LoudnessMaximizer& m = keep != nullptr ? *keep : local;
    prepareMax (m, kFs, 2, 512);
    m.setParams (p);
    const int lat = m.latencySamples(), n = static_cast<int> (x.size());
    Planar buf (2, n + lat);
    setChannel (buf, 0, x);
    setChannel (buf, 1, x);
    processInBlocks (m, buf, block);
    return std::vector<float> (buf.ch[0].begin() + lat, buf.ch[0].end());
}

/** Max dip (median - min, dB) of the 2 kHz tone's gain in 20 ms windows over 1..n. */
double toneDipDb (const std::vector<float>& out, const std::vector<float>& in)
{
    const auto g = cli::toneGainTrack (out, in, kFs, 2000.0, static_cast<int> (kFs), static_cast<int> (in.size()));
    return cli::summariseGainTrack (g, 2.0).dipDb;
}
} // namespace

TEST_CASE ("LoudnessMaximizer: the LF-first limiter takes kicks down in the low band, so a 2 kHz tone ducks less, and the ceiling holds (docs/11 E05 step 5)")
{
    // 12 dB drive, glue 0: with lfLimit 0 the wideband limiter ducks the
    // tone at every kick; with lfLimit 1 the low band is limited 3 dB under
    // the ceiling first. Measured: dip 3.83 -> 1.92 dB (CLI `quality`, the
    // same scene over 1..6 s: 3.74 -> 2.18 dB).
    const int n = static_cast<int> (kFs * 4.0);
    const auto x = kicksUnderTone (n);
    auto p = maxParams (12.0f, -1.0f, 0.5f, 0.0f);
    LoudnessMaximizer plain, lf;
    const auto a = renderMax (x, p, 512, &plain);
    p.lfLimit = 1.0f;
    const auto b = renderMax (x, p, 512, &lf);
    const double dipA = toneDipDb (a, x), dipB = toneDipDb (b, x);
    std::cout << "    measured 2 kHz dip under kicks: lfLimit 0 " << dipA << " dB, 1 " << dipB << " dB, LF GR " << lf.getLfReductionDb() << " dB\n";
    CHECK_LE (dipB, dipA - 1.0);
    CHECK_LE (dipB, 2.6);
    CHECK_LE (peakAbs (b.data(), n), dbfs (-1.0));
    CHECK (lf.getSafetyClipCount() == 0u);
    // The reduction is the low band's: the tone keeps more of its drive.
    CHECK (toneAmplitude (b.data() + n / 2, n / 2, 2000.0, kFs) > toneAmplitude (a.data() + n / 2, n / 2, 2000.0, kFs));
}

TEST_CASE ("LoudnessMaximizer: the LF-first limiter's held-peak detector leaves a steady bass tone clean, and its amount glides click-free and block-size independent (docs/11 E05 step 5)")
{
    // 60 Hz at 0.5 through 12 dB drive: the low band sits 13 dB over its
    // threshold, the gain holds flat over the half-period-of-30 Hz peak hold.
    const int n = static_cast<int> (kFs * 4.0);
    const auto tone = sine (60.0, kFs, n, 0.5f);
    auto p = maxParams (12.0f, -1.0f, 0.5f, 0.0f);
    p.lfLimit = 1.0f;
    const auto y = renderMax (tone, p);
    const double thd = cli::sineThdnDb (y.data() + 3 * static_cast<int> (kFs), static_cast<int> (kFs), kFs, 60.0);
    std::cout << "    measured 60 Hz THD+N with lfLimit 1 at 12 dB drive = " << thd << " dB\n";
    CHECK_LE (thd, -45.0); // docs/11 E05 Done-when: <= -30 dB

    // lfLimit 0 <-> 1 every 200 ms on a 60 Hz tone the low band limits:
    // no step in the waveform (second difference against the held settings').
    const auto maxD2 = [] (const std::vector<float>& v) {
        double d = 0.0;
        for (size_t i = 4800; i < v.size(); ++i)
            d = std::max (d, static_cast<double> (std::abs (v[i] - 2.0f * v[i - 1] + v[i - 2])));
        return d;
    };
    const auto x = sine (60.0, kFs, static_cast<int> (kFs * 1.2), 0.35f);
    const auto run = [&] (bool jumps, float held, int block) {
        LoudnessMaximizer m;
        prepareMax (m, kFs, 2, 512);
        // A little glue keeps the band stage running, as Boost's glue floor
        // does in the chain (starting and stopping the stage is the glue
        // switch's crossfade, tested above).
        auto q = maxParams (8.0f, -1.0f, 0.5f, 0.1f);
        q.lfLimit = held;
        m.setParams (q);
        Planar buf (2, static_cast<int> (x.size()));
        setChannel (buf, 0, x);
        setChannel (buf, 1, x);
        for (int pos = 0; pos < buf.numSamples(); pos += block)
        {
            if (jumps)
            {
                q.lfLimit = (pos / static_cast<int> (0.2 * kFs)) % 2 == 1 ? 1.0f : 0.0f;
                m.setParams (q);
            }
            m.process (buf.block (pos, std::min (block, buf.numSamples() - pos)));
        }
        return buf.ch[0];
    };
    const double held = std::max (maxD2 (run (false, 0.0f, 240)), maxD2 (run (false, 1.0f, 240)));
    const auto moving = run (true, 0.0f, 240);
    std::cout << "    measured second difference moving / held = " << maxD2 (moving) / held << "\n";
    // Measured 2.4x: the low band's gain glides 2.8 dB in 50 ms. The
    // parameter-change test above allows 14x a pure tone's for its stage
    // switches; a gain step of 0.25 % alone reads 14x on such a tone.
    CHECK_LE (maxD2 (moving), 3.0 * held);
    // Block-size independence with the stage moving (the per-sample path).
    const auto moving7 = run (true, 0.0f, 240); // same schedule, same blocks: deterministic
    CHECK (moving7 == moving);
    const auto blockA = renderMax (x, p, 7), blockB = renderMax (x, p, 512);
    double maxDiff = 0.0;
    for (size_t i = 0; i < blockA.size(); ++i)
        maxDiff = std::max (maxDiff, static_cast<double> (std::abs (blockA[i] - blockB[i])));
    CHECK_LE (maxDiff, 1e-5);
}

//==============================================================================
// docs/11 E06 step 1: the whole-stage residual (clipper + limiter).
TEST_CASE ("LoudnessMaximizer: the whole-stage residual is the least-squares THD+N of the output against the aligned clipper input per 30 ms grid window, and reads the limiter's gain modulation the clipper's own THD+N cannot (docs/11 E06 step 1)")
{
    // Two tones 50 + 63 Hz at 0.25 each, 12 dB drive, clipper off: only the
    // limiter acts (its gain follows the 13 Hz beat), so the clipper's
    // reading is -160 dB while the output carries IMD.
    const int window = 3 * static_cast<int> (kFs * 0.01); // the 10 ms grid point at or after 25 ms
    const int n = 40 * window;
    std::vector<float> x (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
        x[static_cast<size_t> (i)] = static_cast<float> (0.25 * std::sin (kTwoPi * 50.0 * i / kFs) + 0.25 * std::sin (kTwoPi * 63.0 * i / kFs));
    const auto p = maxParams (12.0f, -1.0f, 0.0f, 0.0f);
    LoudnessMaximizer m;
    prepareMax (m, kFs, 2, 2048);
    m.setParams (p);
    const int lat = m.latencySamples();
    Planar buf (2, n);
    setChannel (buf, 0, x);
    setChannel (buf, 1, x);
    const float drive = dbfs (12.0);
    int checked = 0;
    for (int w = 0; w < n / window; ++w)
    {
        m.process (buf.block (w * window, window));
        if (w < 10)
            continue;
        // Offline: the same window, y against drive x delayed by the latency.
        DistortionSums s;
        for (int i = w * window; i < (w + 1) * window; ++i)
        {
            const float ref = drive * x[static_cast<size_t> (i - lat)];
            s.add (ref, buf.ch[0][static_cast<size_t> (i)] - ref);
        }
        DistortionEnergy e;
        e.add (s);
        CHECK_NEAR (m.getResidualDistortionDb(), e.ratioDb(), 0.05);
        CHECK (m.getDistortionDb() <= -159.0f);
        ++checked;
    }
    CHECK (checked == 30);
    std::cout << "    measured limiter-only residual on 50 + 63 Hz at 12 dB drive = " << m.getResidualDistortionDb() << " dB\n";
    CHECK_GE (m.getResidualDistortionDb(), -45.0f);

    // Nothing to limit: the output is the input delayed, the residual reads clean.
    LoudnessMaximizer quiet;
    prepareMax (quiet, kFs, 2, 512);
    quiet.setParams (maxParams (0.0f, -1.0f, 0.5f, 0.0f));
    Planar low (2, n);
    const auto s1k = sine (1000.0, kFs, n, 0.3f);
    setChannel (low, 0, s1k);
    setChannel (low, 1, s1k);
    processInBlocks (quiet, low, 512);
    CHECK_LE (quiet.getResidualDistortionDb(), -120.0f);

    // Its windows (and the clipper's) close on the fixed grid, so the
    // reading after the same audio does not depend on the host blocks.
    const auto finalReading = [&] (std::initializer_list<int> pattern) {
        LoudnessMaximizer r;
        prepareMax (r, kFs, 2, 4096);
        r.setParams (maxParams (12.0f, -1.0f, 0.5f, 0.0f));
        Planar b (2, n);
        setChannel (b, 0, x);
        setChannel (b, 1, x);
        int pos = 0;
        while (pos < n)
            for (int len : pattern)
            {
                const int l = std::min (len, n - pos);
                if (l <= 0)
                    break;
                r.process (b.block (pos, l));
                pos += l;
            }
        return std::make_pair (r.getResidualDistortionDb(), r.getDistortionDb());
    };
    const auto a = finalReading ({ 480 }), b = finalReading ({ 4096 }), c = finalReading ({ 1000, 37, 4096, 5 });
    CHECK_NEAR (a.first, b.first, 0.01);
    CHECK_NEAR (a.first, c.first, 0.01);
    CHECK_NEAR (a.second, b.second, 0.01);
    CHECK_NEAR (a.second, c.second, 0.01);
}

//==============================================================================
// docs/11 E19 step 3: the bed-lift budget (max.bedLift).
TEST_CASE ("LoudnessMaximizer: the bed-lift budget lifts quiet programme by at most the budget, upstream lift included, and leaves loud programme at the full drive (docs/11 E19 step 3)")
{
    const int n = static_cast<int> (kFs * 4.0);
    const auto gainDb = [&] (const std::vector<float>& out, const std::vector<float>& in) {
        const int a = static_cast<int> (kFs * 2.0);
        return toDb (rms (out.data() + a, n - a) / rms (in.data() + a, n - a));
    };
    const auto quiet = pinkNoise (n, dbfs (-40.0), 71), loud = pinkNoise (n, dbfs (-10.0), 72);
    auto p = maxParams (6.0f, -1.0f, 0.5f, 0.0f);
    const double quietFull = gainDb (renderMax (quiet, p), quiet), loudFull = gainDb (renderMax (loud, p), loud);
    p.bedLiftDb = 1.0f;
    const double quietBudget = gainDb (renderMax (quiet, p), quiet), loudBudget = gainDb (renderMax (loud, p), loud);
    LoudnessMaximizer upstream;
    prepareMax (upstream, kFs, 2, 512);
    upstream.setUpstreamLiftDb (3.0f); // the chain ahead already lifts 3 dB
    const double quietUpstream = gainDb (renderMax (quiet, p, 512, &upstream), quiet);
    std::cout << "    measured quiet pink: drive 6 dB " << quietFull << " dB, budget 1 dB " << quietBudget << " dB, with 3 dB upstream "
              << quietUpstream << " dB; loud pink " << loudFull << " -> " << loudBudget << " dB\n";
    CHECK_NEAR (quietFull, 6.0, 0.05);
    CHECK_NEAR (quietBudget, 1.0, 0.05);
    CHECK_NEAR (quietUpstream, 1.0 - 3.0, 0.05);
    CHECK_NEAR (loudBudget, loudFull, 0.05); // the drive reaches the ceiling: all of it stays

    // Quiet -> loud -> quiet: the drive glides (no step), the ceiling holds.
    std::vector<float> scene (quiet);
    for (int i = n / 3; i < 2 * n / 3; ++i)
        scene[static_cast<size_t> (i)] = loud[static_cast<size_t> (i)];
    LoudnessMaximizer m;
    const auto y = renderMax (scene, p, 512, &m);
    CHECK_LE (peakAbs (y.data(), n), dbfs (-1.0));
    CHECK (m.getSafetyClipCount() == 0u);
}

TEST_CASE ("LoudnessMaximizer: switching the bed-lift budget on after the drive moved glides from that drive (no step from an old cap)")
{
    // Budget on at 3 dB drive, off (the cap releases up to 3 dB), drive 3 ->
    // 12 dB without a budget, budget on again: the cap must start from the
    // 12 dB it replaces and glide down, not jump to the 3 dB it last had.
    LoudnessMaximizer m;
    prepareMax (m, kFs, 1, 64);
    const int seg = static_cast<int> (kFs * 0.5);
    std::vector<float> x (static_cast<size_t> (seg), 0.005f), y;
    auto p = maxParams (3.0f, -1.0f, 0.5f, 0.0f);
    const auto run = [&] (float driveDb, float bedLiftDb) {
        p.driveDb = driveDb;
        p.bedLiftDb = bedLiftDb;
        m.setParams (p);
        std::vector<float> b (x);
        for (int pos = 0; pos < seg; pos += 64)
        {
            float* ch[] = { b.data() + pos };
            m.process (AudioBlock { ch, 1, std::min (64, seg - pos) });
        }
        y.insert (y.end(), b.begin(), b.end());
    };
    run (3.0f, 1.0f);
    run (3.0f, 24.0f);
    run (12.0f, 24.0f);
    const float before = y.back();
    run (12.0f, 1.0f);
    double maxStep = 0.0;
    for (size_t i = static_cast<size_t> (3 * seg); i < y.size(); ++i)
        maxStep = std::max (maxStep, static_cast<double> (std::abs (y[i] - y[i - 1])));
    std::cout << "    measured: level before " << before << ", after " << y.back() << ", largest step " << maxStep / before << " of the level\n";
    CHECK_NEAR (toDb (y.back() / 0.005), 1.0, 0.05); // the budget holds
    CHECK_LE (maxStep / before, 0.01);                 // a 9 dB jump would be 0.65
}

TEST_CASE ("Chain: Boost's transient coupling adds at most 1 dB of Clarity attack on kicks above Boost 50 % in Music, nothing on a steady tone, in Gaming or below 50 %, and restarts on reset (docs/11 E05 step 6)")
{
    using namespace flub::param;
    const int n = static_cast<int> (4.0 * kFs);
    std::vector<float> kicks (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        const double beat = std::fmod (i / kFs, 0.5);
        kicks[static_cast<size_t> (i)] = static_cast<float> (0.5 * std::exp (-beat * 10.0) * std::sin (kTwoPi * 55.0 * beat)
                                                            + 0.05 * std::sin (kTwoPi * 2000.0 * i / kFs));
    }
    const auto tone = sine (60.0, kFs, n, 0.5f);
    // The attack the coupling adds after 4 s: the effective clarity.attack
    // minus Boost's own row (+2 dB from 60 %, both modes).
    const auto coupled = [&] (ModeValue mode, float boost, const std::vector<float>& x, bool resetAfter) {
        ParameterStore store;
        store.set (Mode, static_cast<float> (mode));
        store.set (BoostIntensity, boost);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, 512, 2 });
        std::vector<float> l (512), r (512);
        for (int p = 0; p < n; p += 512)
        {
            std::copy_n (x.begin() + p, 512, l.begin());
            std::copy_n (x.begin() + p, 512, r.begin());
            float* ch[2] = { l.data(), r.data() };
            chain.process (AudioBlock (ch, 2, 512));
        }
        if (resetAfter)
        {
            chain.reset();
            std::fill (l.begin(), l.end(), 0.0f);
            std::fill (r.begin(), r.end(), 0.0f);
            float* ch[2] = { l.data(), r.data() };
            chain.process (AudioBlock (ch, 2, 64));
        }
        return static_cast<double> (chain.effectiveValue (ClarityAttackDb) - 2.0f); // Boost's row is full from 60 % (Music) / 70 % (Gaming)
    };
    const double music100 = coupled (ModeValue::Music, 1.0f, kicks, false);
    const double music80 = coupled (ModeValue::Music, 0.8f, kicks, false);
    const double steady = coupled (ModeValue::Music, 1.0f, tone, false);
    const double gaming = coupled (ModeValue::Gaming, 1.0f, kicks, false);
    const double afterReset = coupled (ModeValue::Music, 1.0f, kicks, true);
    std::printf ("    measured coupled attack: Music Boost 100 / 80 kicks %.2f / %.2f dB, steady 60 Hz %.2f dB, Gaming %.2f dB, after reset %.2f dB\n",
                 music100, music80, steady, gaming, afterReset);
    CHECK_GE (music100, 0.5);
    CHECK_LE (music100, 1.0 + 1e-6);
    CHECK_GE (music80, 0.1);
    CHECK_LE (music80, music100 + 1e-6);
    CHECK_LE (std::abs (steady), 0.1);
    CHECK (gaming == 0.0);
    CHECK (afterReset == 0.0);
    // Below Boost 50 % nothing: the effective attack is Boost's row alone.
    ParameterStore store;
    store.set (Mode, 0.0f);
    store.set (BoostIntensity, 0.45f);
    ProcessingChain chain (store);
    chain.prepare ({ kFs, 512, 2 });
    std::vector<float> l (kicks.begin(), kicks.end()), r = l;
    for (int p = 0; p + 512 <= n; p += 512)
    {
        float* ch[2] = { l.data() + p, r.data() + p };
        chain.process (AudioBlock (ch, 2, 512));
    }
    CHECK_NEAR (chain.effectiveValue (ClarityAttackDb), 2.0f * smoothstep (0.1f, 0.6f, 0.45f), 1e-5);
}
