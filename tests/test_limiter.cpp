// Tests for the look-ahead true-peak limiter: the ceiling guarantee (sample
// peak, plus an INDEPENDENT band-limited true-peak measurement) for driven
// noise, square-ish waves, sparse impulses and sines at several rates, no
// overshoot on steps, exact latency and transparency below the ceiling,
// release time constants (fixed and program-dependent), stereo linking,
// click-free ceiling changes, real-time safety, robustness and block-size
// invariance.
//
// What the true-peak guarantee covers: the limiter's 4x detector
// (TruePeakDetector) is flat to 0.39 fs and rolls off above it, so the
// "true peak <= ceiling + 0.1 dB" check is made on programme whose spectrum
// is inside that band (band-limited noise and squares, sines, impulses) and
// is judged by the ideal (full-band sinc) reconstruction. Raw full-band
// synthetic signals (white noise, aliased squares) still get the hard
// sample-peak guarantee; their ideal-reconstruction peak is only bounded
// loosely (documented limitation of the 4x detector).
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/dsp/TruePeakLimiter.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <map>
#include <vector>

using namespace flub;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;
const double kTpTolerance = std::pow (10.0, 0.1 / 20.0); // +0.1 dB

float dbfs (double db) { return static_cast<float> (std::pow (10.0, db / 20.0)); }

//==============================================================================
// Independent true-peak meter (deliberately shares no code with the limiter's
// TruePeakDetector): ideal band-limited (sinc) reconstruction of the whole
// finite signal, obtained by zero-padding its spectrum 8x, then parabolic
// refinement at every local maximum of the 8x grid. The signal is padded with
// silence to at least twice its length first, so the FFT's circular wrap only
// ever sees zeros. Double precision throughout.
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

/** True peak of one or two channels (two real signals share one complex FFT:
    the interpolation is linear and maps real signals to real signals, so the
    real part is channel a and the imaginary part channel b). */
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
    Y[N / 2] = 0.5 * X[N / 2]; // split the Nyquist bin between +fs/2 and -fs/2
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

double independentTruePeak (const std::vector<float>& x) { return independentTruePeak (x.data(), nullptr, static_cast<int> (x.size())); }

/** Linear-phase Blackman-windowed-sinc low-pass (161 taps, cutoff 0.41 fs:
    flat to ~0.39 fs, below -70 dB from ~0.44 fs). The full convolution is
    returned, so the result (onset and decay included) is band-limited. */
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

void prepareLimiter (TruePeakLimiter& lim, double fs = kFs, int channels = 2, int maxBlock = 512, float lookaheadMs = 1.5f,
                     bool truePeak = true)
{
    lim.setLookaheadMs (lookaheadMs);
    lim.setTruePeakDetection (truePeak);
    ProcessSpec spec;
    spec.sampleRate = fs;
    spec.maxBlockSize = maxBlock;
    spec.numChannels = channels;
    lim.prepare (spec);
}

LimiterParams limiterParams (float ceilingDb = -1.0f, float releaseMs = 80.0f, bool autoRelease = true)
{
    LimiterParams p;
    p.ceilingDb = ceilingDb;
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

//==============================================================================
// Programme that stresses the ceiling. Every signal is `n` samples long and
// ends in a silent tail, so the limiter's output ends naturally and the
// abrupt end of the input is itself part of the test.
std::vector<float> drivenNoise (int n, int tail, float gain, uint32_t seed)
{
    auto v = whiteNoise (n, gain, seed);
    std::fill (v.end() - tail, v.end(), 0.0f);
    return v;
}

/** White noise band-limited to the detector's band (see bandLimit()). The
    unit-variance input is scaled so the result has the given sample peak. */
std::vector<float> bandLimitedNoise (int n, int tail, float peak, uint32_t seed)
{
    auto v = bandLimit (whiteNoise (n - tail, 1.0f, seed));
    v.resize (static_cast<size_t> (n), 0.0f);
    const float scale = peak / static_cast<float> (peakAbs (v.data(), n));
    for (auto& s : v)
        s *= scale;
    return v;
}

/** Naive (aliased) square: full-scale steps, the classic inter-sample-over generator. */
std::vector<float> naiveSquare (double freq, double fs, int n, int tail, float amp)
{
    std::vector<float> v (static_cast<size_t> (n), 0.0f);
    for (int i = 0; i < n - tail; ++i)
        v[static_cast<size_t> (i)] = std::fmod (freq * i / fs, 1.0) < 0.5 ? amp : -amp;
    return v;
}

/** Hard-driven tanh "square-ish" wave (sampled directly, so it aliases). */
std::vector<float> tanhSquare (double freq, double fs, int n, int tail, float amp)
{
    std::vector<float> v (static_cast<size_t> (n), 0.0f);
    const double norm = std::tanh (6.0);
    for (int i = 0; i < n - tail; ++i)
        v[static_cast<size_t> (i)] = amp * static_cast<float> (std::tanh (6.0 * std::sin (kTwoPi * freq * i / fs)) / norm);
    return v;
}

/** Sparse single-sample impulses of alternating sign, pseudo-random spacing
    (sometimes adjacent pairs, whose inter-sample peak is 4/pi = +2.1 dB). */
std::vector<float> sparseImpulses (int n, int tail, float amp, uint32_t seed)
{
    std::vector<float> v (static_cast<size_t> (n), 0.0f);
    FastRandom rng (seed);
    int pos = 100;
    float sign = 1.0f;
    while (pos < n - tail - 2)
    {
        v[static_cast<size_t> (pos)] = sign * amp;
        if ((rng.nextU32() & 3u) == 0u)
            v[static_cast<size_t> (pos + 1)] = sign * amp; // doublet
        sign = -sign;
        pos += 150 + static_cast<int> (rng.nextU32() % 3000u);
    }
    return v;
}

/** Steady sine with 5 ms raised-cosine fades: the test is about the tone's
    inter-sample peaks, not about the broadband splatter of a hard onset
    (hard onsets are covered by the noise, square, impulse and step cases). */
std::vector<float> toneWithTail (double freq, double fs, int n, int tail, float amp, double phase)
{
    auto v = sine (freq, fs, n, amp, phase);
    std::fill (v.end() - tail, v.end(), 0.0f);
    const int fade = static_cast<int> (0.005 * fs);
    for (int i = 0; i < fade; ++i)
    {
        const float w = static_cast<float> (0.5 - 0.5 * std::cos (kPi * i / fade));
        v[static_cast<size_t> (i)] *= w;
        v[static_cast<size_t> (n - tail - 1 - i)] *= w;
    }
    return v;
}

struct CeilingResult
{
    double samplePeak = 0.0, truePeak = 0.0;
    uint64_t safetyClips = 0;
};

CeilingResult runCeilingCase (const std::vector<float>& left, const std::vector<float>& right, double fs, float ceilingDb,
                              float lookaheadMs = 1.5f, int blockSize = 256)
{
    TruePeakLimiter lim;
    prepareLimiter (lim, fs, 2, 512, lookaheadMs);
    lim.setParams (limiterParams (ceilingDb, 80.0f, true));
    Planar buf (2, static_cast<int> (left.size()));
    setChannel (buf, 0, left);
    setChannel (buf, 1, right);
    processInBlocks (lim, buf, blockSize);
    CeilingResult r;
    r.samplePeak = planarPeak (buf);
    r.truePeak = planarTruePeak (buf);
    r.safetyClips = lim.getSafetyClipCount();
    return r;
}

struct Program
{
    const char* name;
    std::vector<float> l, r;
};

void reportIfFailed (bool ok, const Program& prog, double fs, float ceilingDb, const CeilingResult& res)
{
    if (! ok)
        std::cerr << "    case: " << prog.name << " @ " << fs << " Hz, ceiling " << ceilingDb << " dB: sample peak "
                  << toDb (res.samplePeak) << " dB, true peak " << toDb (res.truePeak) << " dB, clips " << res.safetyClips << "\n";
}
} // namespace

//==============================================================================
TEST_CASE ("TruePeakLimiter: independent true-peak meter is accurate (self-test)")
{
    // A sine's true peak is its amplitude whatever its phase against the
    // sample grid (the sample peak of an fs/4 sine at 45 degrees is 3 dB low).
    const int n = 8192;
    for (double f : { 997.0, 11025.0, 19000.0 })
    {
        auto s = sine (f, 44100.0, n, 0.5f, 0.785398);
        // Taper the ends so the finite signal's own spectrum is clean.
        for (int i = 0; i < 1024; ++i)
        {
            const float w = static_cast<float> (0.5 - 0.5 * std::cos (kPi * i / 1024.0));
            s[static_cast<size_t> (i)] *= w;
            s[static_cast<size_t> (n - 1 - i)] *= w;
        }
        CHECK_NEAR (independentTruePeak (s), 0.5, 0.5 * 0.002); // within 0.02 dB
    }
    CHECK_LE (peakAbs (sine (11025.0, 44100.0, 64, 1.0f, 0.785398).data(), 64), 0.7072);

    // Doublet: 2 sinc (0.5) = 4 / pi at the midpoint.
    std::vector<float> q (1024, 0.0f);
    q[500] = 1.0f;
    q[501] = 1.0f;
    CHECK_NEAR (independentTruePeak (q), 4.0 / kPi, 0.002);

    // The band-limiting helper keeps its passband and removes the top of the band.
    auto lo = bandLimit (sine (0.38 * 48000.0, 48000.0, 8000, 0.5f));
    auto hi = bandLimit (sine (0.45 * 48000.0, 48000.0, 8000, 0.5f));
    CHECK_NEAR (toneAmplitude (lo.data() + 2000, 4000, 0.38 * 48000.0, 48000.0), 0.5, 0.005);
    CHECK_LE (toneAmplitude (hi.data() + 2000, 4000, 0.45 * 48000.0, 48000.0), 0.5 * 0.001);
}

TEST_CASE ("TruePeakLimiter: latencySamples() = lookahead + detector delay, and a quiet impulse arrives exactly that late")
{
    struct Case
    {
        double fs;
        float lookaheadMs;
        bool truePeak;
        int expected;
    };
    const Case cases[] = { { 48000.0, 1.5f, true, 72 + TruePeakDetector::kDelay }, { 48000.0, 1.5f, false, 72 },  { 44100.0, 2.0f, true, 88 + TruePeakDetector::kDelay },
                           { 96000.0, 0.5f, true, 48 + TruePeakDetector::kDelay }, { 192000.0, 1.0f, true, 192 + TruePeakDetector::kDelay }, { 48000.0, 0.0f, true, TruePeakDetector::kDelay } };
    for (const auto& tc : cases)
    {
        TruePeakLimiter lim;
        prepareLimiter (lim, tc.fs, 2, 256, tc.lookaheadMs, tc.truePeak);
        CHECK (lim.latencySamples() == tc.expected);

        const int n = 2048;
        Planar buf (2, n);
        buf.ch[0][100] = dbfs (-30.0);
        buf.ch[1][100] = -dbfs (-40.0);
        processInBlocks (lim, buf, 64);
        const int lat = lim.latencySamples();
        for (int i = 0; i < n; ++i)
        {
            const float e0 = i == 100 + lat ? dbfs (-30.0) : 0.0f;
            const float e1 = i == 100 + lat ? -dbfs (-40.0) : 0.0f;
            if (buf.ch[0][static_cast<size_t> (i)] != e0 || buf.ch[1][static_cast<size_t> (i)] != e1)
            {
                CHECK (false);
                break;
            }
        }
    }

    // Structural setters only take effect at prepare().
    TruePeakLimiter lim;
    prepareLimiter (lim, kFs, 2, 256, 1.5f, true);
    lim.setLookaheadMs (5.0f);
    lim.setTruePeakDetection (false);
    CHECK (lim.latencySamples() == 72 + TruePeakDetector::kDelay);
}

TEST_CASE ("TruePeakLimiter: a -30 dBFS signal passes bit-exactly, only delayed by latencySamples()")
{
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        TruePeakLimiter lim;
        prepareLimiter (lim, fs);
        lim.setParams (limiterParams (-1.0f));
        const int n = static_cast<int> (fs * 0.25);
        const auto l = whiteNoise (n, dbfs (-30.0), 11);
        const auto r = sine (3000.0, fs, n, dbfs (-30.0));
        Planar buf (2, n);
        setChannel (buf, 0, l);
        setChannel (buf, 1, r);
        processInBlocks (lim, buf, 128);
        const int lat = lim.latencySamples();
        bool exact = true;
        for (int i = 0; i < n; ++i)
        {
            const float e0 = i >= lat ? l[static_cast<size_t> (i - lat)] : 0.0f;
            const float e1 = i >= lat ? r[static_cast<size_t> (i - lat)] : 0.0f;
            exact = exact && buf.ch[0][static_cast<size_t> (i)] == e0 && buf.ch[1][static_cast<size_t> (i)] == e1;
        }
        CHECK (exact);
        CHECK (lim.getGainReductionDb() == 0.0f);
        CHECK (lim.getSafetyClipCount() == 0u);
    }
}

TEST_CASE ("TruePeakLimiter: ceiling holds in sample peak and independent true peak (<= +0.1 dB) at 44.1/48/96 kHz")
{
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        const int n = static_cast<int> (fs * 0.15);
        const int tail = static_cast<int> (fs * 0.02);
        Program hardOnset { "hard-onset 11 kHz +6 dB (no fades)", sine (11000.0, fs, n, 2.0f, 0.1), sine (11000.0, fs, n, 2.0f, 2.1) };
        std::fill (hardOnset.l.end() - tail, hardOnset.l.end(), 0.0f);
        std::fill (hardOnset.r.end() - tail, hardOnset.r.end(), 0.0f);
        const Program programs[] = {
            hardOnset,
            { "band-limited noise +20 dB", bandLimitedNoise (n, tail, 10.0f, 1), bandLimitedNoise (n, tail, 10.0f, 2) },
            { "band-limited noise +6 dB", bandLimitedNoise (n, tail, 2.0f, 3), bandLimitedNoise (n, tail, 2.0f, 4) },
            { "band-limited naive square +12 dB", bandLimit (naiveSquare (1234.0, fs, n - 200, tail, 4.0f)),
              bandLimit (naiveSquare (110.0, fs, n - 200, tail, 4.0f)) },
            { "band-limited tanh square 0 dBFS", bandLimit (tanhSquare (441.0, fs, n - 200, tail, 1.0f)),
              bandLimit (tanhSquare (3001.0, fs, n - 200, tail, 1.0f)) },
            { "sparse impulses", sparseImpulses (n, tail, 1.0f, 5), sparseImpulses (n, tail, 3.0f, 6) },
            { "997 Hz +6 dB", toneWithTail (997.0, fs, n, tail, 2.0f, 0.3), toneWithTail (997.0, fs, n, tail, 1.0f, 1.3) },
            { "11 kHz +6 dB", toneWithTail (11000.0, fs, n, tail, 2.0f, 0.1), toneWithTail (11000.0, fs, n, tail, 2.0f, 0.9) },
            { "11 kHz 0 dBFS", toneWithTail (11000.0, fs, n, tail, 1.0f, 0.5), toneWithTail (11025.0, fs, n, tail, 1.0f, 0.0) },
        };
        for (const auto& prog : programs)
        {
            for (float ceilingDb : { -1.0f, -0.1f })
            {
                const auto res = runCeilingCase (prog.l, prog.r, fs, ceilingDb);
                const double ceil = dbfs (ceilingDb);
                reportIfFailed (res.samplePeak <= ceil && res.truePeak <= ceil * kTpTolerance && res.safetyClips == 0u, prog, fs,
                                ceilingDb, res);
                CHECK_LE (res.samplePeak, ceil);
                CHECK_LE (res.truePeak, ceil * kTpTolerance);
                CHECK (res.safetyClips == 0u);
                // ...and it is a limiter, not a mute: the programme lands near the ceiling.
                CHECK_GE (res.truePeak, ceil * dbfs (-0.5));
            }
        }
    }
}

TEST_CASE ("TruePeakLimiter: full-band synthetic signals hold the sample ceiling exactly (true peak bounded loosely)")
{
    // Raw white noise and aliased squares carry full-level content up to
    // fs/2. The sample-peak guarantee is exact; their ideal-reconstruction
    // peak can exceed the ceiling because the 4x detector rolls off above
    // 0.39 fs (-1.7 dB at 0.45 fs). This bounds that known limitation.
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        const int n = static_cast<int> (fs * 0.15);
        const int tail = static_cast<int> (fs * 0.02);
        const Program programs[] = {
            { "white noise +20 dB", drivenNoise (n, tail, 10.0f, 1), drivenNoise (n, tail, 10.0f, 2) },
            { "white noise +6 dB", drivenNoise (n, tail, 2.0f, 3), drivenNoise (n, tail, 2.0f, 4) },
            { "naive square 0 dBFS", naiveSquare (110.0, fs, n, tail, 1.0f), naiveSquare (220.0, fs, n, tail, 1.0f) },
            { "naive square +12 dB", naiveSquare (1234.0, fs, n, tail, 4.0f), naiveSquare (777.0, fs, n, tail, 4.0f) },
            { "tanh square 0 dBFS", tanhSquare (441.0, fs, n, tail, 1.0f), tanhSquare (3001.0, fs, n, tail, 1.0f) },
        };
        for (const auto& prog : programs)
        {
            const float ceilingDb = -1.0f;
            const auto res = runCeilingCase (prog.l, prog.r, fs, ceilingDb);
            const double ceil = dbfs (ceilingDb);
            reportIfFailed (res.samplePeak <= ceil && res.truePeak <= ceil * dbfs (2.0) && res.safetyClips == 0u, prog, fs, ceilingDb,
                            res);
            CHECK_LE (res.samplePeak, ceil);
            CHECK (res.safetyClips == 0u);
            CHECK_LE (res.truePeak, ceil * dbfs (2.0));
        }
    }
}

TEST_CASE ("TruePeakLimiter: other ceilings and look-aheads hold the ceiling too")
{
    const int n = 12000, tail = 1000;
    const auto noise = bandLimitedNoise (n, tail, 12.0f, 21);
    const auto square = bandLimit (naiveSquare (3000.0, kFs, n - 200, tail, 3.0f));
    for (float lookaheadMs : { 0.0f, 0.5f, 1.0f, 5.0f })
        for (float ceilingDb : { -12.0f, -6.0f, 0.0f })
        {
            TruePeakLimiter lim;
            prepareLimiter (lim, kFs, 2, 512, lookaheadMs);
            lim.setParams (limiterParams (ceilingDb, 30.0f, true));
            Planar buf (2, n);
            setChannel (buf, 0, noise);
            setChannel (buf, 1, square);
            processInBlocks (lim, buf, 100);
            CHECK_LE (planarPeak (buf), dbfs (ceilingDb));
            CHECK (lim.getSafetyClipCount() == 0u);
            // With no look-ahead at all the gain switches instantly: the
            // sample ceiling still holds, the true-peak claim needs a ramp.
            if (lookaheadMs > 0.0f)
                CHECK_LE (planarTruePeak (buf), dbfs (ceilingDb) * kTpTolerance);
        }
}

TEST_CASE ("TruePeakLimiter: a sudden +12 dB step never overshoots and is anticipated by the look-ahead")
{
    const int n = 24000, step = 8000, end = 22000;
    std::vector<float> x (static_cast<size_t> (n), 0.0f);
    for (int i = 0; i < end; ++i) // step 1/3 of a period away from a zero crossing, then a hard stop
        x[static_cast<size_t> (i)] = static_cast<float> (std::sin (kTwoPi * 440.0 * i / kFs + 1.0)) * (i < step ? dbfs (-6.0) : dbfs (6.0));

    TruePeakLimiter lim;
    prepareLimiter (lim, kFs, 1, 512);
    lim.setParams (limiterParams (-1.0f));
    Planar buf (1, n);
    setChannel (buf, 0, x);
    // Block size 1: getGainReductionDb() then reads the per-sample gain.
    std::vector<float> gainDb (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        lim.process (buf.block (i, 1));
        gainDb[static_cast<size_t> (i)] = lim.getGainReductionDb();
    }
    const int lat = lim.latencySamples();
    CHECK_LE (planarPeak (buf), dbfs (-1.0));
    CHECK_LE (independentTruePeak (buf.ch[0]), dbfs (-1.0) * kTpTolerance);
    CHECK (lim.getSafetyClipCount() == 0u);

    // Before the step reaches the output nothing happens (-6 dBFS < ceiling)...
    CHECK (gainDb[static_cast<size_t> (step + lat - 80)] == 0.0f);
    // ...but the gain is already ramping down when the first loud sample leaves the delay.
    CHECK_LE (gainDb[static_cast<size_t> (step + lat - 1)], -1.0);
    // Steady state: 6.02 dB over, ceiling -1 dB, margin 0.05 dB.
    CHECK_NEAR (gainDb[static_cast<size_t> (end - 1)], -(6.02 + 1.0 + 0.05), 0.1);
}

TEST_CASE ("TruePeakLimiter: release follows releaseMs (fixed) and releaseMs / 5 after an isolated peak (auto)")
{
    // Effective time constant from two points of the release, after the
    // look-ahead ramp has finished: tau = dt / ln (r1 / r2), r = 1 - g.
    auto measureTau = [] (bool autoRelease, float releaseMs, int burstLength) {
        TruePeakLimiter lim;
        prepareLimiter (lim, kFs, 1, 64);
        lim.setParams (limiterParams (-1.0f, releaseMs, autoRelease));
        const int n = static_cast<int> (kFs * 1.5);
        Planar buf (1, n);
        for (int i = 0; i < burstLength; ++i)
            buf.ch[0][static_cast<size_t> (1000 + i)] = static_cast<float> (4.0 * std::sin (kTwoPi * 60.0 * i / kFs + 1.0));
        if (burstLength == 1)
            buf.ch[0][1000] = 4.0f;
        std::vector<double> g (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
        {
            lim.process (buf.block (i, 1));
            g[static_cast<size_t> (i)] = std::pow (10.0, lim.getGainReductionDb() / 20.0);
        }
        // Release start: the last sample at the minimum after the burst left the delay.
        const int releaseStart = 1000 + burstLength + lim.latencySamples() + 2 * static_cast<int> (0.0015 * kFs);
        const int t1 = releaseStart + static_cast<int> (0.002 * kFs);
        const int t2 = t1 + static_cast<int> (0.5 * releaseMs * 0.001 * kFs * (autoRelease && burstLength < 100 ? 0.2 : 1.0));
        const double r1 = 1.0 - g[static_cast<size_t> (t1)], r2 = 1.0 - g[static_cast<size_t> (t2)];
        CHECK (r1 > 0.05);
        const double tauSamples = (t2 - t1) / std::log (r1 / std::max (1e-12, r2));
        return 1000.0 * tauSamples / kFs;
    };

    CHECK_NEAR (measureTau (false, 100.0f, 1), 100.0, 5.0);
    CHECK_NEAR (measureTau (false, 300.0f, 1), 300.0, 15.0);
    CHECK_NEAR (measureTau (true, 100.0f, 1), 20.0, 2.0);  // isolated: fast
    CHECK_NEAR (measureTau (true, 100.0f, static_cast<int> (0.3 * kFs)), 100.0, 5.0); // 300 ms of limiting: slow

    // And the gain really does come back: 1 s after an isolated peak it is at 0 dB.
    TruePeakLimiter lim;
    prepareLimiter (lim, kFs, 1, 512);
    lim.setParams (limiterParams (-1.0f, 80.0f, true));
    Planar buf (1, 48000);
    buf.ch[0][10] = 8.0f;
    processInBlocks (lim, buf, 512);
    CHECK (lim.getGainReductionDb() == 0.0f);
}

TEST_CASE ("TruePeakLimiter: detection is linked - a quiet channel gets the loud channel's gain")
{
    TruePeakLimiter lim;
    prepareLimiter (lim, kFs, 2, 256);
    lim.setParams (limiterParams (-1.0f));
    const int n = 24000;
    const auto loud = sine (997.0, kFs, n, 2.0f);
    const auto quiet = sine (300.0, kFs, n, 0.01f);
    Planar buf (2, n);
    setChannel (buf, 0, loud);
    setChannel (buf, 1, quiet);
    processInBlocks (lim, buf, 256);
    const int lat = lim.latencySamples();
    // Steady state: gain on the quiet channel = gain on the loud channel.
    const double gLoud = toneAmplitude (buf.ch[0].data() + 12000, 9600, 997.0, kFs) / 2.0;
    const double gQuiet = toneAmplitude (buf.ch[1].data() + 12000, 9600, 300.0, kFs) / 0.01;
    CHECK_NEAR (toDb (gLoud), toDb (gQuiet), 0.02);
    CHECK_NEAR (toDb (gQuiet), -7.07, 0.1);
    CHECK (lat == 72 + TruePeakDetector::kDelay);
}

TEST_CASE ("TruePeakLimiter: ceiling and release changes while limiting are click-free and never trip the safety clamp")
{
    const int n = 48000;
    const auto x = sine (997.0, kFs, n, 2.0f);
    TruePeakLimiter lim;
    prepareLimiter (lim, kFs, 1, 64);
    lim.setParams (limiterParams (-1.0f, 80.0f, true));
    Planar buf (1, n);
    setChannel (buf, 0, x);
    std::vector<double> g (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        if (i == 12000)
            lim.setParams (limiterParams (-12.0f, 80.0f, true)); // -11 dB ceiling jump
        if (i == 24000)
            lim.setParams (limiterParams (0.0f, 30.0f, false)); // +12 dB, fixed release
        if (i == 36000)
            lim.setParams (limiterParams (-6.0f, 5.0f, true));
        lim.process (buf.block (i, 1));
        g[static_cast<size_t> (i)] = std::pow (10.0, lim.getGainReductionDb() / 20.0);
    }
    CHECK (lim.getSafetyClipCount() == 0u);
    // The ceiling glides over 50 ms: the largest per-sample gain step is tiny.
    double maxStep = 0.0;
    for (int i = 6000; i < n; ++i)
        maxStep = std::max (maxStep, std::abs (g[static_cast<size_t> (i)] - g[static_cast<size_t> (i - 1)]));
    CHECK_LE (maxStep, 2.0e-3);
    // Each new ceiling is reached (output peaks ~ ceiling - margin).
    CHECK_NEAR (toDb (peakAbs (buf.ch[0].data() + 20000, 4000)), -12.05, 0.1);
    CHECK_NEAR (toDb (peakAbs (buf.ch[0].data() + 32000, 4000)), -0.05, 0.1);
    CHECK_NEAR (toDb (peakAbs (buf.ch[0].data() + 44000, 4000)), -6.05, 0.1);
}

TEST_CASE ("TruePeakLimiter: sample-peak mode (true peak off) holds the sample ceiling with latency = lookahead")
{
    TruePeakLimiter lim;
    prepareLimiter (lim, kFs, 2, 512, 1.5f, false);
    lim.setParams (limiterParams (-1.0f));
    CHECK (lim.latencySamples() == 72);
    const int n = 24000;
    Planar buf (2, n);
    setChannel (buf, 0, whiteNoise (n, 10.0f, 7));
    setChannel (buf, 1, naiveSquare (1000.0, kFs, n, 0, 3.0f));
    processInBlocks (lim, buf, 512);
    CHECK_LE (planarPeak (buf), dbfs (-1.0));
    CHECK (lim.getSafetyClipCount() == 0u);
}

TEST_CASE ("TruePeakLimiter: reset, setParams and process do not allocate")
{
    TruePeakLimiter lim;
    prepareLimiter (lim, kFs, 2, 512);
    Planar buf (2, 512);
    setChannel (buf, 0, whiteNoise (512, 8.0f, 1));
    setChannel (buf, 1, whiteNoise (512, 8.0f, 2));

    AllocationGuard guard;
    lim.reset();
    for (int b = 0; b < 16; ++b)
    {
        lim.setParams (limiterParams (b % 2 == 0 ? -1.0f : -6.0f, b % 3 == 0 ? 20.0f : 200.0f, b % 4 != 0));
        lim.process (buf.block());
        lim.process (buf.block (0, 7));
        (void) lim.getGainReductionDb();
        (void) lim.getSafetyClipCount();
    }
    lim.reset();
    CHECK (guard.allocations() == 0);
}

TEST_CASE ("TruePeakLimiter: silence, DC, full-scale noise, impulses and extreme settings stay finite and under the ceiling")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        for (int channels : { 1, 2, 8 })
        {
            TruePeakLimiter lim;
            prepareLimiter (lim, fs, channels, 1024);
            const int n = 8192;
            for (int sig = 0; sig < 6; ++sig)
            {
                Planar buf (channels, n);
                for (int c = 0; c < channels; ++c)
                {
                    auto& d = buf.ch[static_cast<size_t> (c)];
                    switch (sig)
                    {
                        case 0: break; // silence
                        case 1: std::fill (d.begin(), d.end(), c % 2 == 0 ? 1.0f : -5.0f); break; // DC
                        case 2: d = whiteNoise (n, 1.0f, static_cast<uint32_t> (c + 1)); break;
                        case 3: d = whiteNoise (n, 1000.0f, static_cast<uint32_t> (c + 9)); break; // +60 dB
                        case 4: d[100] = 1.0f; d[101] = -1.0f; d[4000] = 50.0f; break;              // impulses
                        default: d = sine (fs * 0.49, fs, n, 3.0f); break;                          // near Nyquist
                    }
                }
                buf.ptrs.clear();
                for (auto& c : buf.ch)
                    buf.ptrs.push_back (c.data());

                const float ceilings[] = { -12.0f, 0.0f, -100.0f, 100.0f, nan };
                const float releases[] = { 5.0f, 1000.0f, 0.0f, 1.0e9f, nan };
                const int k = sig % 5;
                lim.setParams (limiterParams (ceilings[k], releases[(k + sig) % 5], sig % 2 == 0));
                processInBlocks (lim, buf, sig % 2 == 0 ? 1024 : 333);
                CHECK (allFinite (buf));
                CHECK_LE (planarPeak (buf), 1.0); // the ceiling is clamped to <= 0 dBFS
                CHECK (std::isfinite (lim.getGainReductionDb()));
                CHECK (lim.getGainReductionDb() <= 0.0f);
            }
            CHECK (lim.getSafetyClipCount() == 0u);
        }
    }

    // Extreme look-ahead values are clamped.
    for (float la : { -5.0f, 1000.0f, nan })
    {
        TruePeakLimiter lim;
        prepareLimiter (lim, kFs, 2, 256, la);
        CHECK (lim.latencySamples() >= TruePeakDetector::kDelay);
        CHECK (lim.latencySamples() <= TruePeakDetector::kDelay + 480);
        Planar buf (2, 4096);
        setChannel (buf, 0, whiteNoise (4096, 4.0f, 3));
        processInBlocks (lim, buf, 256);
        CHECK (allFinite (buf));
        CHECK_LE (planarPeak (buf), dbfs (-1.0));
    }
}

TEST_CASE ("TruePeakLimiter: NaN / Inf input never reaches the output and the limiter recovers")
{
    TruePeakLimiter lim;
    prepareLimiter (lim, kFs, 2, 256);
    lim.setParams (limiterParams (-1.0f));
    const int n = 24000;
    Planar buf (2, n);
    setChannel (buf, 0, sine (1000.0, kFs, n, 0.25f));
    setChannel (buf, 1, sine (1000.0, kFs, n, 0.25f));
    buf.ch[0][1000] = std::numeric_limits<float>::quiet_NaN();
    buf.ch[1][2000] = std::numeric_limits<float>::infinity();
    buf.ch[0][3000] = -std::numeric_limits<float>::infinity();
    processInBlocks (lim, buf, 256);
    CHECK (allFinite (buf));
    CHECK_LE (planarPeak (buf), dbfs (-1.0));
    // A second later the quiet tone passes untouched again.
    const int lat = lim.latencySamples();
    const auto ref = sine (1000.0, kFs, n, 0.25f);
    double err = 0.0;
    for (int i = 20000; i < n; ++i)
        err = std::max (err, static_cast<double> (std::abs (buf.ch[0][static_cast<size_t> (i)] - ref[static_cast<size_t> (i - lat)])));
    CHECK (err == 0.0);
}

TEST_CASE ("TruePeakLimiter: output is independent of the host block size (1, 7, 64, 512)")
{
    const int n = 20000;
    std::vector<float> l = whiteNoise (n, 6.0f, 31), r = naiveSquare (500.0, kFs, n, 0, 2.0f);
    for (int i = 0; i < n; ++i) // bursts with gaps: exercises attack, release and auto release
        if ((i / 3000) % 2 == 1)
        {
            l[static_cast<size_t> (i)] *= 0.05f;
            r[static_cast<size_t> (i)] *= 0.05f;
        }

    std::vector<std::vector<float>> outputs;
    for (int bs : { 1, 7, 64, 512 })
    {
        TruePeakLimiter lim;
        prepareLimiter (lim, kFs, 2, 512);
        lim.setParams (limiterParams (-1.0f, 50.0f, true));
        Planar buf (2, n);
        setChannel (buf, 0, l);
        setChannel (buf, 1, r);
        processInBlocks (lim, buf, bs);
        std::vector<float> both (buf.ch[0]);
        both.insert (both.end(), buf.ch[1].begin(), buf.ch[1].end());
        outputs.push_back (both);
    }
    for (size_t k = 1; k < outputs.size(); ++k)
    {
        double maxDiff = 0.0;
        for (size_t i = 0; i < outputs[0].size(); ++i)
            maxDiff = std::max (maxDiff, static_cast<double> (std::abs (outputs[k][i] - outputs[0][i])));
        CHECK (maxDiff == 0.0); // strictly per-sample algorithm: bit-identical
    }
}

TEST_CASE ("TruePeakLimiter: blocks narrower than the prepared channel count and long runs stay stable")
{
    TruePeakLimiter lim;
    prepareLimiter (lim, kFs, 8, 512);
    lim.setParams (limiterParams (-1.0f, 40.0f, true));
    Planar mono (1, 512);
    Planar wide (8, 512);
    FastRandom rng (5);
    // ~10 s of alternating loud / quiet noise, mono and 8-channel blocks.
    for (int b = 0; b < 1000; ++b)
    {
        Planar& buf = (b % 3 == 0) ? mono : wide;
        const float amp = (b / 20) % 2 == 0 ? 8.0f : 0.1f;
        for (auto& c : buf.ch)
            for (auto& v : c)
                v = amp * rng.nextBipolar();
        lim.process (buf.block());
        CHECK_LE (planarPeak (buf), dbfs (-1.0));
    }
    CHECK (allFinite (wide));
    CHECK (lim.getSafetyClipCount() == 0u);
}

// ---- adversarial review tests ----
namespace
{
/** Brute-force reference of the header's gain computer in sample-peak mode
    (p = max |x|, D = 0), in double: r, the sliding minimum over L + 2 and the
    mean over L + 1, straight from the definitions (no deque, no running sum). */
std::vector<double> referenceEnvelope (const std::vector<float>& x, int L, double thr)
{
    const int n = static_cast<int> (x.size());
    auto r = [&] (int k) {
        if (k < 0)
            return 1.0;
        const double p = std::abs (static_cast<double> (x[static_cast<size_t> (k)]));
        return p > thr ? thr / p : 1.0;
    };
    std::vector<double> m (static_cast<size_t> (n)), a (static_cast<size_t> (n));
    for (int k = 0; k < n; ++k)
    {
        double mn = 1.0;
        for (int j = k - L - 1; j <= k; ++j)
            mn = std::min (mn, r (j));
        m[static_cast<size_t> (k)] = mn;
    }
    for (int k = 0; k < n; ++k)
    {
        double s = 0.0;
        for (int j = k - L; j <= k; ++j)
            s += j < 0 ? 1.0 : m[static_cast<size_t> (j)];
        a[static_cast<size_t> (k)] = s / (L + 1);
    }
    return a;
}
} // namespace

TEST_CASE ("TruePeakLimiter [adversarial]: gain matches a brute-force model of the header (deque, box filter, attack bound)")
{
    // Sample-peak mode makes p[n] exactly max |x|, so the whole gain computer
    // can be checked sample by sample against the definitions. Look-aheads
    // include L + 3 = 32 and 64 (deque ring exactly full) and L = 0.
    // Programme: never-zero random-sign samples under an envelope with
    // isolated spikes, dense bursts, and long monotonic ramps up and down
    // (a slowly falling peak level fills the deque to L + 2 entries).
    const int n = 12000;
    std::vector<float> x (static_cast<size_t> (n));
    FastRandom rng (77);
    for (int i = 0; i < n; ++i)
    {
        double env = 0.3;
        if (i >= 2000 && i < 2400)
            env = 4.0 - 3.5 * (i - 2000) / 400.0; // falling peaks: r rises every sample
        else if (i >= 3000 && i < 3400)
            env = 0.5 + 3.5 * (i - 3000) / 400.0; // rising peaks
        else if (i >= 5000 && i < 7000)
            env = 1.0 + 3.0 * (0.5 + 0.5 * rng.nextBipolar()); // dense random overs
        else if ((rng.nextU32() & 127u) == 0u)
            env = 1.0 + 7.0 * (0.5 + 0.5 * rng.nextBipolar()); // isolated spikes
        const float sign = (rng.nextU32() & 1u) != 0u ? 1.0f : -1.0f;
        x[static_cast<size_t> (i)] = sign * static_cast<float> (env * (0.6 + 0.4 * (0.5 + 0.5 * rng.nextBipolar())));
    }

    for (int L : { 0, 1, 29, 61, 72 })
    {
        TruePeakLimiter lim;
        prepareLimiter (lim, kFs, 1, 300, static_cast<float> (L * 1000.0 / kFs), false);
        REQUIRE (lim.latencySamples() == L);
        lim.setParams (limiterParams (-1.0f, 50.0f, true));
        Planar buf (1, n);
        setChannel (buf, 0, x);
        // Irregular host blocks (1 .. 300 samples).
        for (int pos = 0; pos < n;)
        {
            const int len = std::min (n - pos, 1 + static_cast<int> (rng.nextU32() % 300u));
            lim.process (buf.block (pos, len));
            pos += len;
        }
        const double thr = std::pow (10.0, (-1.0 - 0.05) / 20.0);
        const auto a = referenceEnvelope (x, L, thr);
        int boundFails = 0, attackFails = 0, releaseFails = 0;
        double prevG = 1.0;
        for (int i = L; i < n; ++i)
        {
            const double xin = x[static_cast<size_t> (i - L)];
            const double y = buf.ch[0][static_cast<size_t> (i)];
            const double g = y / xin; // x never 0
            const double ai = a[static_cast<size_t> (i)];
            const double r0 = i - L - 1 >= 0 ? std::min (1.0, thr / std::abs (static_cast<double> (x[static_cast<size_t> (i - L - 1)]))) : 1.0;
            const double r1 = std::min (1.0, thr / std::abs (xin));
            // g <= a[n] <= min (r[n-L-1], r[n-L]); |y| <= threshold.
            if (g > ai * (1.0 + 1e-6) || g > std::min (r0, r1) * (1.0 + 1e-6) || std::abs (y) > thr * (1.0 + 1e-6))
                ++boundFails;
            // Attack: the gain follows the box-filtered envelope exactly.
            if (ai < prevG * (1.0 - 1e-5) && std::abs (g - ai) > 1e-6 * ai)
                ++attackFails;
            // Release: monotonic recovery, never above the envelope.
            if (ai > prevG * (1.0 + 1e-5) && g < prevG * (1.0 - 1e-6))
                ++releaseFails;
            prevG = g;
        }
        if (boundFails + attackFails + releaseFails > 0)
            std::cerr << "    L = " << L << ": bound " << boundFails << ", attack " << attackFails << ", release " << releaseFails << "\n";
        CHECK (boundFails == 0);
        CHECK (attackFails == 0);
        CHECK (releaseFails == 0);
        CHECK (lim.getSafetyClipCount() == 0u);
    }
}

TEST_CASE ("TruePeakLimiter [adversarial]: 192 kHz and the latency-profile look-aheads hold the ceiling (sample + true peak)")
{
    struct Case
    {
        double fs;
        float lookaheadMs;
    };
    // 0.5 ms = LowLatency profile, 2 ms = Quality profile, 1 ms = MixEngine master.
    const Case cases[] = { { 192000.0, 1.5f }, { 192000.0, 0.5f }, { 44100.0, 0.5f }, { 48000.0, 0.5f }, { 96000.0, 2.0f }, { 44100.0, 1.0f } };
    for (const auto& tc : cases)
    {
        const double fs = tc.fs;
        const int n = static_cast<int> (fs * 0.12);
        const int tail = static_cast<int> (fs * 0.02);
        Program hardOnset { "hard-onset 11 kHz +6 dB", sine (11000.0, fs, n, 2.0f, 0.1), sine (7000.0, fs, n, 2.0f, 2.1) };
        std::fill (hardOnset.l.end() - tail, hardOnset.l.end(), 0.0f);
        std::fill (hardOnset.r.end() - tail, hardOnset.r.end(), 0.0f);
        const Program programs[] = {
            hardOnset,
            { "band-limited noise +20 dB", bandLimitedNoise (n, tail, 10.0f, 51), bandLimitedNoise (n, tail, 10.0f, 52) },
            { "sparse impulses + doublets", sparseImpulses (n, tail, 2.0f, 53), sparseImpulses (n, tail, 1.0f, 54) },
            { "band-limited square +12 dB", bandLimit (naiveSquare (2500.0, fs, n - 200, tail, 4.0f)),
              bandLimit (naiveSquare (60.0, fs, n - 200, tail, 4.0f)) },
        };
        for (const auto& prog : programs)
        {
            const float ceilingDb = -1.0f;
            const auto res = runCeilingCase (prog.l, prog.r, fs, ceilingDb, tc.lookaheadMs, 97);
            const double ceil = dbfs (ceilingDb);
            reportIfFailed (res.samplePeak <= ceil && res.truePeak <= ceil * kTpTolerance && res.safetyClips == 0u, prog, fs, ceilingDb, res);
            CHECK_LE (res.samplePeak, ceil);
            CHECK_LE (res.truePeak, ceil * kTpTolerance);
            CHECK (res.safetyClips == 0u);
        }
    }
}

TEST_CASE ("TruePeakLimiter [adversarial]: ceiling automation while limiting never trips the safety clamp")
{
    // The ceiling is moved at random (-12 .. 0 dB) every 10 .. 40 ms under
    // dense +12 dB programme. The gain path alone must hold every ceiling in
    // force (the clamp compares against the ceiling each output sample was
    // limited with), and the result is identical for any block size.
    const int n = 48000;
    const auto l = bandLimitedNoise (n, 1000, 4.0f, 61);
    const auto r = bandLimit (naiveSquare (330.0, kFs, n - 200, 1000, 4.0f));
    std::vector<std::pair<int, float>> automation;
    FastRandom rng (62);
    for (int pos = 0; pos < n;)
    {
        automation.push_back ({ pos, -12.0f * (0.5f + 0.5f * rng.nextBipolar()) });
        pos += 480 * (1 + static_cast<int> (rng.nextU32() % 4u));
    }
    std::vector<float> first;
    for (int bs : { 1, 48, 480 })
    {
        TruePeakLimiter lim;
        prepareLimiter (lim, kFs, 2, 512);
        lim.setParams (limiterParams (-1.0f, 40.0f, true));
        Planar buf (2, n);
        setChannel (buf, 0, l);
        setChannel (buf, 1, r);
        size_t next = 0;
        for (int pos = 0; pos < n; pos += bs)
        {
            while (next < automation.size() && automation[next].first <= pos)
                lim.setParams (limiterParams (automation[next++].second, 40.0f, true));
            lim.process (buf.block (pos, std::min (bs, n - pos)));
        }
        CHECK (lim.getSafetyClipCount() == 0u);
        CHECK_LE (planarPeak (buf), 1.0);
        CHECK (allFinite (buf));
        if (first.empty())
            first = buf.ch[0];
        else
            CHECK (buf.ch[0] == first); // automation points are multiples of every block size
    }

    // Harsher: a new random ceiling every millisecond (the glide never
    // finishes, and reverses direction while the gain is still ramping).
    TruePeakLimiter lim;
    prepareLimiter (lim, kFs, 2, 64);
    Planar buf (2, n);
    setChannel (buf, 0, l);
    setChannel (buf, 1, r);
    for (int pos = 0; pos < n; pos += 48)
    {
        lim.setParams (limiterParams (-12.0f * (0.5f + 0.5f * rng.nextBipolar()), 5.0f, (pos / 48) % 2 == 0));
        lim.process (buf.block (pos, std::min (48, n - pos)));
    }
    CHECK (lim.getSafetyClipCount() == 0u);
    CHECK_LE (planarPeak (buf), 1.0);
}

TEST_CASE ("TruePeakLimiter [adversarial]: after 20 s of dense limiting with a 1 s release the gain lands exactly on 0 dB")
{
    // Guards the running sum (re-summed per ring cycle), the double gain
    // state and the landing rule: once recovered, the limiter is again a
    // bit-exact delay.
    TruePeakLimiter lim;
    prepareLimiter (lim, kFs, 2, 1024, 10.0f); // largest look-ahead: largest ring
    lim.setParams (limiterParams (-3.0f, 1000.0f, false));
    Planar buf (2, 1024);
    FastRandom rng (71);
    const int loudBlocks = static_cast<int> (20.0 * kFs / 1024);
    for (int b = 0; b < loudBlocks; ++b)
    {
        for (auto& c : buf.ch)
            for (auto& v : c)
                v = 6.0f * rng.nextBipolar();
        lim.process (buf.block());
        if (b == loudBlocks - 1)
            CHECK_LE (lim.getGainReductionDb(), -10.0f);
    }
    CHECK (lim.getSafetyClipCount() == 0u);

    // 25 s of quiet noise (a 1 s release needs ~17 s from -18 dB to 1e-7).
    const int lat = lim.latencySamples();
    std::vector<float> history;
    bool exact = true;
    for (int b = 0; b < static_cast<int> (25.0 * kFs / 1024); ++b)
    {
        std::vector<float> in (1024);
        for (auto& v : in)
            v = 0.1f * rng.nextBipolar();
        std::copy (in.begin(), in.end(), buf.ch[0].begin());
        std::copy (in.begin(), in.end(), buf.ch[1].begin());
        history.insert (history.end(), in.begin(), in.end());
        lim.process (buf.block());
        if (b >= static_cast<int> (24.0 * kFs / 1024))
            for (int i = 0; i < 1024; ++i)
            {
                const size_t k = history.size() - 1024 + static_cast<size_t> (i) - static_cast<size_t> (lat);
                exact = exact && buf.ch[0][static_cast<size_t> (i)] == history[k];
            }
    }
    CHECK (exact);
    CHECK (lim.getGainReductionDb() == 0.0f);
}

TEST_CASE ("TruePeakLimiter [adversarial]: a channel that leaves and rejoins never replays stale audio")
{
    for (bool truePeak : { true, false })
    {
        TruePeakLimiter lim;
        prepareLimiter (lim, kFs, 2, 256, 1.5f, truePeak);
        lim.setParams (limiterParams (-1.0f));
        Planar st (2, 256);
        std::fill (st.ch[1].begin(), st.ch[1].end(), 0.5f); // right channel: DC, then the channel goes away
        lim.process (st.block());
        Planar mono (1, 256);
        for (int b = 0; b < 4; ++b)
            lim.process (mono.block());
        Planar back (2, 256); // silence on both channels
        lim.process (back.block());
        CHECK (peakAbs (back.ch[1].data(), 256) == 0.0);
        CHECK (peakAbs (back.ch[0].data(), 256) == 0.0);
    }
}

TEST_CASE ("TruePeakLimiter [adversarial]: reset() mid-limit forgets the gain; setParams() right after applies instantly")
{
    TruePeakLimiter lim;
    prepareLimiter (lim, kFs, 1, 512);
    lim.setParams (limiterParams (-1.0f, 1000.0f, false));
    Planar loud (1, 4096);
    setChannel (loud, 0, sine (440.0, kFs, 4096, 8.0f));
    processInBlocks (lim, loud, 512);
    CHECK_LE (lim.getGainReductionDb(), -15.0f);

    lim.reset();
    lim.setParams (limiterParams (-6.0f, 1000.0f, false)); // fresh: no 50 ms glide
    const int n = 4096;
    const auto quiet = sine (440.0, kFs, n, dbfs (-7.0)); // below -6 dB: must pass untouched
    Planar buf (1, n);
    setChannel (buf, 0, quiet);
    processInBlocks (lim, buf, 512);
    const int lat = lim.latencySamples();
    bool exact = true;
    for (int i = 0; i < n; ++i)
        exact = exact && buf.ch[0][static_cast<size_t> (i)] == (i >= lat ? quiet[static_cast<size_t> (i - lat)] : 0.0f);
    CHECK (exact);
    CHECK (lim.getGainReductionDb() == 0.0f);

    // ...and the new ceiling is in force from the first sample: a -3 dBFS
    // burst straight after another reset is held at -6 dB.
    lim.reset();
    lim.setParams (limiterParams (-2.0f));
    lim.setParams (limiterParams (-6.0f));
    Planar burst (1, 2048);
    setChannel (burst, 0, sine (440.0, kFs, 2048, dbfs (-3.0)));
    processInBlocks (lim, burst, 64);
    CHECK_LE (peakAbs (burst.ch[0].data(), 2048), dbfs (-6.0));
    CHECK (lim.getSafetyClipCount() == 0u);
}

TEST_CASE ("TruePeakLimiter [adversarial]: getGainReductionDb() reports the deepest gain of the block")
{
    // One spike of +12 dB (over a -1 dB ceiling) in an otherwise quiet
    // stream, sample-peak mode: the deepest gain is threshold / 4 exactly.
    TruePeakLimiter lim;
    prepareLimiter (lim, kFs, 2, 4096, 1.5f, false);
    lim.setParams (limiterParams (-1.0f, 80.0f, true));
    Planar buf (2, 4096);
    buf.ch[1][1000] = -4.0f;
    lim.process (buf.block());
    CHECK_NEAR (lim.getGainReductionDb(), -1.05 - toDb (4.0), 0.001);
    CHECK_NEAR (std::abs (buf.ch[1][static_cast<size_t> (1000 + lim.latencySamples())]), dbfs (-1.05), 1e-5);
    // A later quiet block (after recovery) reports 0 dB again.
    Planar quiet (2, 4096);
    for (int b = 0; b < 12; ++b)
        lim.process (quiet.block());
    CHECK (lim.getGainReductionDb() == 0.0f);
}

TEST_CASE ("TruePeakLimiter [adversarial]: CD-band programme (flat to 0.45 fs) stays within the documented true-peak bound")
{
    // Documents (and pins, against regressions) the detector-band limitation
    // from the header: full-level noise that fills a CD-style passband up to
    // 0.45 fs (19.8 kHz at 44.1 kHz), driven +12 dB. The sample peak is
    // exact; the ideal-reconstruction peak is measured at up to ~+0.25 dB.
    auto lowPass045 = [] (const std::vector<float>& x) {
        constexpr int taps = 401, centre = taps / 2;
        constexpr double fc = 0.45;
        std::vector<double> h (taps);
        double sum = 0.0;
        for (int k = 0; k < taps; ++k)
        {
            const double t = k - centre;
            const double s = t == 0.0 ? 1.0 : std::sin (kTwoPi * fc * t) / (kTwoPi * fc * t);
            h[static_cast<size_t> (k)] = s * (0.42 - 0.5 * std::cos (kTwoPi * k / (taps - 1)) + 0.08 * std::cos (2.0 * kTwoPi * k / (taps - 1)));
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
    };
    double worst = -100.0;
    for (double fs : { 44100.0, 48000.0 })
        for (uint32_t seed : { 1u, 2u, 3u })
        {
            const int n = static_cast<int> (fs * 0.12), tail = static_cast<int> (fs * 0.02);
            std::vector<float> ch[2];
            for (int c = 0; c < 2; ++c)
            {
                ch[c] = lowPass045 (whiteNoise (n - tail - 400, 1.0f, seed + 10u * static_cast<uint32_t> (c)));
                ch[c].resize (static_cast<size_t> (n), 0.0f);
                const float s = 4.0f / static_cast<float> (peakAbs (ch[c].data(), n));
                for (auto& v : ch[c])
                    v *= s;
            }
            const auto res = runCeilingCase (ch[0], ch[1], fs, -1.0f);
            CHECK_LE (res.samplePeak, dbfs (-1.0));
            CHECK (res.safetyClips == 0u);
            worst = std::max (worst, toDb (res.truePeak) + 1.0);
        }
    CHECK_LE (worst, 0.35);
}
