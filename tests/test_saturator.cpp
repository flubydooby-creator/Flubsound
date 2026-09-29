// Tests for the oversampled saturator: static curve properties, unity
// small-signal gain, exact transparency at 0 dB drive / mix 0, harmonic
// character per type, DC removal, aliasing, latency and dry/wet alignment,
// tape emphasis and head bump, click-free parameter / type changes,
// allocation freedom, robustness and block-size invariance.
#include "TestFramework.h"
#include "TestSignals.h"

#include "Analysis.h"

#include "flub/common/Denormals.h"
#include "flub/dsp/Saturator.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

using namespace flub;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;
constexpr std::array<double, 4> kRates { 44100.0, 48000.0, 96000.0, 192000.0 };
constexpr std::array<SaturationType, 3> kTypes { SaturationType::Tape, SaturationType::Tube, SaturationType::Digital };

struct OsConfig
{
    int factor;
    Oversampler::Quality quality;
    int expectedLatency;
};

constexpr std::array<OsConfig, 5> kOsConfigs { {
    { 1, Oversampler::Quality::High, 0 },
    { 2, Oversampler::Quality::High, 32 },
    { 4, Oversampler::Quality::High, 36 },
    { 2, Oversampler::Quality::Low, 16 },
    { 4, Oversampler::Quality::Low, 19 },
} };

SaturatorParams makeParams (SaturationType type, float driveDb, float mix = 1.0f, float outputDb = 0.0f)
{
    SaturatorParams p;
    p.type = type;
    p.driveDb = driveDb;
    p.mix = mix;
    p.outputDb = outputDb;
    return p;
}

std::unique_ptr<Saturator> makeSat (double sampleRate, const SaturatorParams& p, int numChannels = 1, int maxBlock = 4096,
                                    int factor = 2, Oversampler::Quality q = Oversampler::Quality::High)
{
    auto s = std::make_unique<Saturator>();
    s->setOversampling (factor, q);
    s->setParams (p);
    s->prepare ({ sampleRate, maxBlock, numChannels });
    return s;
}

/** Copies into the existing channel storage (keeps Planar's pointers valid). */
void load (Planar& buf, int channel, const std::vector<float>& s)
{
    auto& dst = buf.ch[static_cast<size_t> (channel)];
    std::copy (s.begin(), s.begin() + static_cast<std::ptrdiff_t> (std::min (s.size(), dst.size())), dst.begin());
}

void fillAll (Planar& buf, const std::vector<float>& s)
{
    for (int c = 0; c < buf.numChannels(); ++c)
        load (buf, c, s);
}

std::vector<float> runMono (Saturator& sat, const std::vector<float>& in, int blockSize = 256)
{
    Planar buf (1, static_cast<int> (in.size()));
    load (buf, 0, in);
    processInBlocks (sat, buf, blockSize);
    return buf.ch[0];
}

bool allFinite (const Planar& buf)
{
    for (const auto& c : buf.ch)
        for (float v : c)
            if (! std::isfinite (v))
                return false;
    return true;
}

double peakOf (const Planar& buf, int from, int to)
{
    double p = 0.0;
    for (const auto& c : buf.ch)
        p = std::max (p, peakAbs (c.data() + from, to - from));
    return p;
}

/** max |y[n] - y[n-1]| for n in [from, to). */
double maxStep (const std::vector<float>& y, int from, int to)
{
    double m = 0.0;
    for (int n = std::max (1, from); n < to; ++n)
        m = std::max (m, static_cast<double> (std::abs (y[static_cast<size_t> (n)] - y[static_cast<size_t> (n - 1)])));
    return m;
}

/** max |y[n] - x[n - delay]| over n in [from, to). */
double maxDelayedError (const std::vector<float>& y, const std::vector<float>& x, int delay, int from, int to)
{
    double e = 0.0;
    for (int n = std::max (from, delay); n < to; ++n)
        e = std::max (e, static_cast<double> (std::abs (y[static_cast<size_t> (n)] - x[static_cast<size_t> (n - delay)])));
    return e;
}

/** Harmonic amplitudes h[k] = |k-th harmonic| (k = 1 .. 10) of y over [start, start + n).
    Callers use an integer number of fundamental periods, so there is no leakage. */
std::array<double, 11> harmonics (const std::vector<float>& y, int start, int n, double f0, double sampleRate)
{
    std::array<double, 11> h {};
    for (int k = 1; k <= 10; ++k)
        h[static_cast<size_t> (k)] = toneAmplitude (y.data() + start, n, f0 * k, sampleRate);
    return h;
}

double thd (const std::array<double, 11>& h)
{
    double p = 0.0;
    for (size_t k = 2; k < h.size(); ++k)
        p += h[k] * h[k];
    return std::sqrt (p) / std::max (1.0e-30, h[1]);
}

/** Steady-state 1 kHz (or f) response of a mono saturator: settle 0.2 s,
    measure 0.25 s (an integer number of periods for the frequencies used). */
struct ToneResult
{
    std::vector<float> out;
    int start = 0, length = 0;
};

ToneResult runTone (Saturator& sat, double freq, double sampleRate, float amplitude)
{
    const int settle = static_cast<int> (sampleRate * 0.2);
    const int length = static_cast<int> (sampleRate * 0.25);
    ToneResult r;
    r.out = runMono (sat, sine (freq, sampleRate, settle + length, amplitude));
    r.start = settle;
    r.length = length;
    return r;
}
} // namespace

//==============================================================================
TEST_CASE ("Saturator: shape() has f(0) = 0, unit slope at 0, is bounded and monotonic")
{
    for (auto type : kTypes)
    {
        CHECK (Saturator::shape (type, 0.0f) == 0.0f);

        // f'(0) = 1 by central difference.
        const float h = 1.0e-3f;
        const double slope = (static_cast<double> (Saturator::shape (type, h)) - Saturator::shape (type, -h)) / (2.0 * h);
        CHECK_NEAR (slope, 1.0, 1.0e-3);

        // Bounded (the tube's negative asymptote -1 / (1 - tanh 0.2) = -1.246 is the largest) and finite.
        double peak = 0.0;
        for (float x = -1000.0f; x <= 1000.0f; x += 0.37f)
        {
            const float y = Saturator::shape (type, x);
            CHECK (std::isfinite (y));
            peak = std::max (peak, static_cast<double> (std::abs (y)));
        }
        CHECK_LE (peak, 1.25);
        CHECK (std::isfinite (Saturator::shape (type, std::numeric_limits<float>::max())));
        CHECK (std::isfinite (Saturator::shape (type, -std::numeric_limits<float>::max())));

        // Monotonic non-decreasing on a fine grid. All three functions are; Tape
        // and Digital are exactly monotonic in float too. The tube's rational
        // form can wobble by one float ulp next to its asymptote (~1e-7).
        const float tolerance = type == SaturationType::Tube ? 2.0e-7f : 0.0f;
        float prev = Saturator::shape (type, -20.0f);
        bool monotonic = true;
        for (int i = 1; i <= 40000; ++i)
        {
            const float y = Saturator::shape (type, -20.0f + 1.0e-3f * static_cast<float> (i));
            monotonic = monotonic && y >= prev - tolerance;
            prev = y;
        }
        CHECK (monotonic);
    }

    // Tape = tanh (odd, saturates at +-1).
    for (float x : { 0.1f, 0.5f, 1.0f, 3.0f })
    {
        CHECK_NEAR (Saturator::shape (SaturationType::Tape, x), std::tanh (x), 1.0e-6);
        CHECK (Saturator::shape (SaturationType::Tape, -x) == -Saturator::shape (SaturationType::Tape, x));
    }
    CHECK_NEAR (Saturator::shape (SaturationType::Tape, 50.0f), 1.0, 1.0e-6);

    // Digital = x - 4/27 x^3 up to the knee at 1.5 (value 1, slope 0), hard +-1 beyond.
    CHECK_NEAR (Saturator::shape (SaturationType::Digital, 0.5f), 0.5 - 4.0 / 27.0 * 0.125, 1.0e-6);
    CHECK_NEAR (Saturator::shape (SaturationType::Digital, 1.4999f), 1.0, 1.0e-6);
    CHECK_NEAR (Saturator::shape (SaturationType::Digital, 1.5f), 1.0, 0.0);
    CHECK_NEAR (Saturator::shape (SaturationType::Digital, 7.0f), 1.0, 0.0);
    CHECK_NEAR (Saturator::shape (SaturationType::Digital, -7.0f), -1.0, 0.0);
    const double kneeSlope = (Saturator::shape (SaturationType::Digital, 1.5f) - Saturator::shape (SaturationType::Digital, 1.49f)) / 0.01;
    CHECK_LE (std::abs (kneeSlope), 0.01);
    for (float x : { 0.2f, 0.9f, 1.3f, 2.0f })
        CHECK (Saturator::shape (SaturationType::Digital, -x) == -Saturator::shape (SaturationType::Digital, x));

    // Tube = (tanh(x + b) - tanh b) / (1 - tanh^2 b), b = 0.2, as in the header.
    const double tb = std::tanh (0.2);
    for (double x = -6.0; x <= 6.0; x += 0.05)
    {
        const double expected = (std::tanh (x + 0.2) - tb) / (1.0 - tb * tb);
        CHECK_NEAR (Saturator::shape (SaturationType::Tube, static_cast<float> (x)), expected, 2.0e-6);
    }
    // Asymmetric: the positive half compresses harder -> even harmonics.
    CHECK_NEAR (Saturator::shape (SaturationType::Tube, 30.0f), 1.0 / (1.0 + tb), 1.0e-5);
    CHECK_NEAR (Saturator::shape (SaturationType::Tube, -30.0f), -1.0 / (1.0 - tb), 1.0e-5);
    CHECK (Saturator::shape (SaturationType::Tube, 1.0f) < -Saturator::shape (SaturationType::Tube, -1.0f));

    // Out-of-range enum values behave like Digital (what setParams() maps them to).
    CHECK (Saturator::shape (static_cast<SaturationType> (9), 0.7f) == Saturator::shape (SaturationType::Digital, 0.7f));
}

//==============================================================================
TEST_CASE ("Saturator: small-signal gain is unity (-40 dBFS, 1 kHz, drive 12 dB)")
{
    // Every type and oversampling mode at 48 kHz ...
    for (const auto& os : kOsConfigs)
        for (auto type : kTypes)
        {
            auto sat = makeSat (kFs, makeParams (type, 12.0f), 1, 512, os.factor, os.quality);
            const auto r = runTone (*sat, 1000.0, kFs, 0.01f);
            const double db = toDb (toneAmplitude (r.out.data() + r.start, r.length, 1000.0, kFs) / 0.01);
            CHECK_NEAR (db, 0.0, 0.1);
            if (os.quality == Oversampler::Quality::High)
                CHECK_NEAR (db, 0.0, 0.02);
        }

    // Flat across the band: the tape pre/de-emphasis shelves cancel exactly,
    // the curves are linear at -60 dBFS. (Frequencies with an integer number
    // of periods in 0.25 s; the tape head bump only matters below ~400 Hz.)
    for (auto type : kTypes)
        for (double f : { 1000.0, 3000.0, 6000.0, 10000.0, 16000.0 })
        {
            auto sat = makeSat (kFs, makeParams (type, 24.0f), 1, 512, 2, Oversampler::Quality::High);
            const auto r = runTone (*sat, f, kFs, 0.001f);
            CHECK_NEAR (toDb (toneAmplitude (r.out.data() + r.start, r.length, f, kFs) / 0.001), 0.0, 0.02);
        }

    // ... and the default mode at every rate, also at full drive.
    for (double fs : kRates)
        for (auto type : kTypes)
            for (float drive : { 12.0f, 24.0f })
            {
                auto sat = makeSat (fs, makeParams (type, drive));
                const float amp = drive > 12.0f ? 0.001f : 0.01f; // -60 dBFS at 24 dB drive
                const auto r = runTone (*sat, 1000.0, fs, amp);
                CHECK_NEAR (toDb (toneAmplitude (r.out.data() + r.start, r.length, 1000.0, fs) / amp), 0.0, 0.1);
            }
}

TEST_CASE ("Saturator: drive 0 dB is transparent and mix 0 is the exactly delayed dry signal")
{
    const int n = 8192;
    const auto noise = whiteNoise (n, 1.0f, 99); // full scale: the curves would be far from linear
    for (const auto& os : kOsConfigs)
        for (auto type : kTypes)
        {
            {
                auto sat = makeSat (kFs, makeParams (type, 0.0f, 1.0f, 0.0f), 1, 512, os.factor, os.quality);
                CHECK (sat->latencySamples() == os.expectedLatency);
                const auto y = runMono (*sat, noise, 128);
                CHECK_LE (maxDelayedError (y, noise, os.expectedLatency, 0, n), 1.0e-6);
                CHECK_LE (peakAbs (y.data(), os.expectedLatency), 0.0); // the delay line starts silent
            }
            {
                auto sat = makeSat (kFs, makeParams (type, 24.0f, 0.0f, 12.0f), 1, 512, os.factor, os.quality);
                const auto y = runMono (*sat, noise, 100);
                CHECK_LE (maxDelayedError (y, noise, os.expectedLatency, 0, n), 1.0e-6);
            }
        }
}

TEST_CASE ("Saturator: output gain scales the wet path and the dry/wet mix is latency-aligned")
{
    const int n = 9600;
    const auto x = whiteNoise (n, 0.8f, 3);
    const int lat = 32; // 2x High

    // Drive 0: wet = dry, so output gain and mix are pure gains on the delayed input.
    {
        auto sat = makeSat (kFs, makeParams (SaturationType::Tube, 0.0f, 1.0f, -6.0f));
        const auto y = runMono (*sat, x);
        const float g = dbToGain (-6.0f);
        double err = 0.0;
        for (int i = lat; i < n; ++i)
            err = std::max (err, static_cast<double> (std::abs (y[static_cast<size_t> (i)] - g * x[static_cast<size_t> (i - lat)])));
        CHECK_LE (err, 1.0e-6);
    }
    {
        auto sat = makeSat (kFs, makeParams (SaturationType::Tape, 0.0f, 0.5f, 6.0f));
        const auto y = runMono (*sat, x);
        const float g = 0.5f + 0.5f * dbToGain (6.0f);
        double err = 0.0;
        for (int i = lat; i < n; ++i)
            err = std::max (err, static_cast<double> (std::abs (y[static_cast<size_t> (i)] - g * x[static_cast<size_t> (i - lat)])));
        CHECK_LE (err, 1.0e-6);
    }

    // Driven: mix m is exactly (1 - m) * delayed dry + m * (the mix-1 output).
    for (auto type : kTypes)
    {
        auto wet = makeSat (kFs, makeParams (type, 15.0f, 1.0f, 3.0f));
        auto half = makeSat (kFs, makeParams (type, 15.0f, 0.3f, 3.0f));
        const auto yw = runMono (*wet, x);
        const auto yh = runMono (*half, x);
        double err = 0.0;
        for (int i = lat; i < n; ++i)
        {
            const float expected = 0.7f * x[static_cast<size_t> (i - lat)] + 0.3f * yw[static_cast<size_t> (i)];
            err = std::max (err, static_cast<double> (std::abs (yh[static_cast<size_t> (i)] - expected)));
        }
        CHECK_LE (err, 1.0e-5);
    }

    // Alignment: at low level (linear region) a 50 % mix must reproduce the
    // input delayed by the latency - a misaligned dry path would comb-filter.
    for (const auto& os : kOsConfigs)
    {
        auto sat = makeSat (kFs, makeParams (SaturationType::Digital, 12.0f, 0.5f), 1, 512, os.factor, os.quality);
        const int len = 24000;
        std::vector<float> in (static_cast<size_t> (len));
        for (int i = 0; i < len; ++i)
            in[static_cast<size_t> (i)] = static_cast<float> (0.004 * std::sin (kTwoPi * 440.0 * i / kFs) + 0.003 * std::sin (kTwoPi * 3100.0 * i / kFs)
                                                             + 0.002 * std::sin (kTwoPi * 9700.0 * i / kFs));
        const auto y = runMono (*sat, in);
        double errPow = 0.0, sigPow = 0.0;
        for (int i = 4800; i < len; ++i)
        {
            const double ref = in[static_cast<size_t> (i - os.expectedLatency)];
            const double e = y[static_cast<size_t> (i)] - ref;
            errPow += e * e;
            sigPow += ref * ref;
        }
        const double snrDb = 10.0 * std::log10 (sigPow / std::max (1.0e-30, errPow));
        CHECK_GE (snrDb, os.quality == Oversampler::Quality::High ? 60.0 : 40.0);
    }
}

//==============================================================================
TEST_CASE ("Saturator: THD rises with drive for every type")
{
    for (auto type : kTypes)
    {
        double prev = -1.0;
        for (float drive : { 0.0f, 1.0f, 3.0f, 6.0f, 9.0f, 12.0f, 18.0f, 24.0f })
        {
            auto sat = makeSat (kFs, makeParams (type, drive));
            const auto r = runTone (*sat, 1000.0, kFs, 0.5f);
            const double t = thd (harmonics (r.out, r.start, r.length, 1000.0, kFs));
            if (drive == 0.0f)
                CHECK_LE (t, 1.0e-5); // transparent
            else
                CHECK_GE (t, prev * 1.1);
            prev = t;
        }
        CHECK_GE (prev, 0.2); // heavy saturation at 24 dB drive
    }
}

TEST_CASE ("Saturator: Tube has a strong 2nd harmonic, Tape and Digital are odd")
{
    std::array<std::array<double, 11>, 3> h {};
    for (size_t t = 0; t < kTypes.size(); ++t)
    {
        auto sat = makeSat (kFs, makeParams (kTypes[t], 12.0f));
        const auto r = runTone (*sat, 1000.0, kFs, 0.5f);
        h[t] = harmonics (r.out, r.start, r.length, 1000.0, kFs);
    }
    const auto rel = [] (const std::array<double, 11>& hh, size_t k) { return toDb (hh[k] / hh[1]); };
    const auto& tape = h[0];
    const auto& tube = h[1];
    const auto& digital = h[2];

    CHECK_GE (rel (tube, 2), -30.0);
    CHECK_GE (rel (tube, 2), rel (digital, 2) + 60.0);
    CHECK_GE (rel (tube, 2), rel (tape, 2) + 60.0);
    for (const auto* odd : { &tape, &digital })
    {
        CHECK_GE (rel (*odd, 3), -25.0);
        for (size_t k : { 2u, 4u, 6u })
            CHECK_LE (rel (*odd, k), -100.0);
    }
}

TEST_CASE ("Saturator: Tube output is DC-free after settling")
{
    for (float drive : { 12.0f, 24.0f })
    {
        auto sat = makeSat (kFs, makeParams (SaturationType::Tube, drive));
        const int settle = 24000, len = 24000; // 0.5 s each, 50 periods of 100 Hz
        const auto x = sine (100.0, kFs, settle + len, 0.5f);
        const auto y = runMono (*sat, x);
        double mean = 0.0;
        for (int i = settle; i < settle + len; ++i)
            mean += y[static_cast<size_t> (i)];
        mean /= len;
        const double fund = toneAmplitude (y.data() + settle, len, 100.0, kFs);

        // What the bare curve would leave: f(g x) / g has a large negative mean.
        const float g = dbToGain (drive);
        double rawMean = 0.0;
        for (int i = settle; i < settle + len; ++i)
            rawMean += Saturator::shape (SaturationType::Tube, g * x[static_cast<size_t> (i)]) / g;
        rawMean /= len;

        CHECK_GE (std::abs (rawMean), 0.02 * fund);
        CHECK_LE (std::abs (mean), 1.0e-5 * fund);
    }
}

TEST_CASE ("Saturator: 4x HQ Digital keeps 15 kHz aliasing below -60 dB (1x does not)")
{
    // 15 kHz at 48 kHz: the output is periodic with 16 samples, so every
    // component is a multiple of 3 kHz. In band, only 15 kHz is harmonic; the
    // rest (3, 6, 9, 12, 18, 21 kHz, DC, Nyquist) can only be aliases.
    // Drive 12 dB at -6 dBFS drives the curve 6 dB into its flat top (~20 % THD
    // at 1 kHz). 16384 = 1024 periods of 3 kHz: no leakage.
    const auto worstAliasDb = [] (int factor)
    {
        auto sat = makeSat (kFs, makeParams (SaturationType::Digital, 12.0f), 1, 512, factor, Oversampler::Quality::High);
        const int n = 24000, len = 16384;
        const auto y = runMono (*sat, sine (15000.0, kFs, n, 0.5f));
        const float* w = y.data() + (n - len);
        const double fund = toneAmplitude (w, len, 15000.0, kFs);
        double worst = 0.0;
        for (int k = 0; k <= 8; ++k)
        {
            if (k == 5)
                continue;
            // DC and Nyquist are real-valued bins: the 2/n amplitude scaling counts them twice.
            const double a = toneAmplitude (w, len, 3000.0 * k, kFs) * ((k == 0 || k == 8) ? 0.5 : 1.0);
            worst = std::max (worst, a);
        }
        return toDb (worst / fund);
    };

    CHECK_LE (worstAliasDb (4), -60.0);
    CHECK_GE (worstAliasDb (1), -30.0); // the measurement does see aliasing when it is there
}

//==============================================================================
TEST_CASE ("Saturator: latencySamples() is exact - a low-level impulse appears L samples later")
{
    for (const auto& os : kOsConfigs)
        for (auto type : kTypes)
        {
            auto sat = makeSat (kFs, makeParams (type, 12.0f), 1, 512, os.factor, os.quality);
            const int lat = sat->latencySamples();
            CHECK (lat == os.expectedLatency);

            const int n0 = 300, n = 1024;
            std::vector<float> x (static_cast<size_t> (n), 0.0f);
            x[static_cast<size_t> (n0)] = 1.0e-3f; // -60 dBFS: linear region even at 12 dB drive
            const auto y = runMono (*sat, x, 64);

            int argMax = 0;
            for (int i = 1; i < n; ++i)
                if (std::abs (y[static_cast<size_t> (i)]) > std::abs (y[static_cast<size_t> (argMax)]))
                    argMax = i;
            CHECK (argMax == n0 + lat);
            CHECK_NEAR (y[static_cast<size_t> (n0 + lat)], 1.0e-3, 0.05e-3);
        }

    // Latency is structural: changing the setting after prepare() does nothing
    // until the next prepare(); invalid factors are sanitised.
    Saturator sat;
    sat.setOversampling (4, Oversampler::Quality::High);
    sat.prepare ({ kFs, 256, 2 });
    CHECK (sat.latencySamples() == 36);
    sat.setOversampling (2, Oversampler::Quality::Low);
    CHECK (sat.latencySamples() == 36);
    sat.prepare ({ kFs, 256, 2 });
    CHECK (sat.latencySamples() == 16);
    sat.setOversampling (3);
    sat.prepare ({ kFs, 256, 2 });
    CHECK (sat.latencySamples() == 32);
    sat.setOversampling (16);
    sat.prepare ({ kFs, 256, 2 });
    CHECK (sat.latencySamples() == 36);
    sat.setOversampling (0);
    sat.prepare ({ kFs, 256, 2 });
    CHECK (sat.latencySamples() == 0);
}

//==============================================================================
TEST_CASE ("Saturator: tape head bump follows drive and HF saturates earlier (emphasis)")
{
    // Small-signal (-60 dBFS) gain at 80 Hz: +1 dB * drive / 24 for Tape only.
    // 80 Hz: 0.25 s = 20 periods. The tube's 10 Hz DC blocker costs 0.07 dB here.
    const auto gainAt = [] (SaturationType type, float drive, double freq, float amp)
    {
        auto sat = makeSat (kFs, makeParams (type, drive));
        const auto r = runTone (*sat, freq, kFs, amp);
        return toDb (toneAmplitude (r.out.data() + r.start, r.length, freq, kFs) / amp);
    };
    CHECK_NEAR (gainAt (SaturationType::Tape, 24.0f, 80.0, 0.001f), 1.0, 0.03);
    CHECK_NEAR (gainAt (SaturationType::Tape, 12.0f, 80.0, 0.001f), 0.5, 0.03);
    CHECK_NEAR (gainAt (SaturationType::Tape, 0.0f, 80.0, 0.001f), 0.0, 0.001);
    CHECK_NEAR (gainAt (SaturationType::Digital, 24.0f, 80.0, 0.001f), 0.0, 0.01);
    CHECK_NEAR (gainAt (SaturationType::Tube, 24.0f, 80.0, 0.001f), 20.0 * std::log10 (8.0 / std::sqrt (65.0)), 0.02);

    // Same level, same drive: an 8 kHz tone reaches the tanh ~6 dB hotter than
    // a 250 Hz tone, so its fundamental is compressed noticeably more. Digital
    // (no emphasis) compresses both equally.
    const float amp = 0.25f;
    const double tapeLow = gainAt (SaturationType::Tape, 12.0f, 250.0, amp);
    const double tapeHigh = gainAt (SaturationType::Tape, 12.0f, 8000.0, amp);
    const double digLow = gainAt (SaturationType::Digital, 12.0f, 250.0, amp);
    const double digHigh = gainAt (SaturationType::Digital, 12.0f, 8000.0, amp);
    CHECK_LE (tapeHigh, tapeLow - 1.5);
    CHECK_NEAR (digHigh, digLow, 0.1);
}

//==============================================================================
TEST_CASE ("Saturator: drive / output / mix jumps are click-free")
{
    // The max sample-to-sample step of the output during a transition must not
    // exceed the steady-state steps on either side. All curves have unity
    // slope at 0, so a sine's steepest step is the same at any drive; an
    // unsmoothed jump would add a step of |f(g1 x)/g1 - f(g0 x)/g0| (~0.17 here).
    const int n = static_cast<int> (kFs * 0.6);
    const int change = 9600;
    for (auto type : kTypes)
    {
        struct Case
        {
            SaturatorParams before, after;
            double freq;
            float amp;
        };
        const Case cases[] = {
            { makeParams (type, 0.0f), makeParams (type, 18.0f), 100.0, 0.3f },
            { makeParams (type, 24.0f), makeParams (type, 0.0f), 100.0, 0.3f },
            { makeParams (type, 0.0f, 1.0f, -12.0f), makeParams (type, 0.0f, 1.0f, 12.0f), 200.0, 0.2f },
            { makeParams (type, 18.0f, 1.0f), makeParams (type, 18.0f, 0.0f), 100.0, 0.3f },
            { makeParams (type, 18.0f, 0.0f), makeParams (type, 18.0f, 1.0f), 100.0, 0.3f },
        };
        for (const auto& c : cases)
        {
            auto sat = makeSat (kFs, c.before, 2, 64);
            Planar buf (2, n);
            fillAll (buf, sine (c.freq, kFs, n, c.amp));
            for (int pos = 0; pos < n; pos += 64)
            {
                if (pos == change)
                    sat->setParams (c.after);
                sat->process (buf.block (pos, std::min (64, n - pos)));
            }
            CHECK (allFinite (buf));
            const auto& y = buf.ch[0];
            const double before = maxStep (y, 4800, change);
            const double after = maxStep (y, n - 4800, n);
            CHECK_LE (maxStep (y, change, n), 1.1 * std::max (before, after));

            // ... and the new setting really is reached (same output as a
            // processor that ran with it from the start).
            auto fresh = makeSat (kFs, c.after, 1, 64);
            const auto ref = runMono (*fresh, sine (c.freq, kFs, n, c.amp), 64);
            CHECK_LE (maxDelayedError (y, ref, 0, n - 4800, n), 1.0e-5);
        }
    }
}

TEST_CASE ("Saturator: type changes are crossfaded without clicks")
{
    const int n = static_cast<int> (kFs * 1.0);
    const int seg = 9600;
    const SaturationType sequence[] = { SaturationType::Tape, SaturationType::Digital, SaturationType::Tube,
                                        SaturationType::Tape, SaturationType::Tube };
    const float drive = 18.0f, amp = 0.3f;
    auto sat = makeSat (kFs, makeParams (sequence[0], drive), 2, 32);
    const auto x = sine (100.0, kFs, n, amp);
    Planar buf (2, n);
    fillAll (buf, x);
    for (int pos = 0; pos < n; pos += 32)
    {
        if (pos % seg == 0)
            sat->setParams (makeParams (sequence[pos / seg], drive));
        sat->process (buf.block (pos, std::min (32, n - pos)));
    }
    CHECK (allFinite (buf));

    const auto& y = buf.ch[0];
    double steady = 0.0;
    for (int s = 0; s < 5; ++s)
        steady = std::max (steady, maxStep (y, s * seg + seg / 2, (s + 1) * seg));
    for (int s = 1; s < 5; ++s)
        CHECK_LE (maxStep (y, s * seg - 16, s * seg + seg / 2), 1.1 * steady);

    // The metric is sensitive: switching Tape -> Tube without a crossfade at
    // the sine peak would step by the difference of the two curves.
    const float g = dbToGain (drive);
    const double hardStep = std::abs (Saturator::shape (SaturationType::Tube, g * amp) - Saturator::shape (SaturationType::Tape, g * amp)) / g;
    CHECK_GE (hardStep, 3.0 * steady);
}

TEST_CASE ("Saturator: reversed and queued type changes converge to a fresh instance")
{
    const int n = 38400; // 600 blocks of 64
    const auto x = whiteNoise (n, 0.5f, 11);
    struct Change
    {
        int at;
        SaturationType type;
    };
    // 48 kHz: the crossfade is 960 samples. The first script reverses 320
    // samples into a fade, so it is back on Tape 320 samples later.
    const std::vector<std::vector<Change>> scripts {
        { { 4800, SaturationType::Tube }, { 5120, SaturationType::Tape } },                                      // reverse mid-fade
        { { 4800, SaturationType::Tube }, { 5120, SaturationType::Digital } },                                   // queue a third type
        { { 4800, SaturationType::Digital }, { 5120, SaturationType::Tube }, { 5184, SaturationType::Digital } }, // queue, then cancel
        { { 4800, SaturationType::Tube }, { 5120, SaturationType::Tape }, { 5184, SaturationType::Tube } },      // reverse twice
    };
    for (const auto& script : scripts)
    {
        auto sat = makeSat (kFs, makeParams (SaturationType::Tape, 15.0f, 1.0f, 2.0f), 1, 64);
        Planar buf (1, n);
        load (buf, 0, x);
        SaturationType last = SaturationType::Tape;
        for (int pos = 0; pos < n; pos += 64)
        {
            for (const auto& c : script)
                if (c.at == pos)
                {
                    sat->setParams (makeParams (c.type, 15.0f, 1.0f, 2.0f));
                    last = c.type;
                }
            sat->process (buf.block (pos, 64));
        }
        CHECK (allFinite (buf));
        CHECK (sat->getParams().type == last);

        auto fresh = makeSat (kFs, makeParams (last, 15.0f, 1.0f, 2.0f), 1, 64);
        const auto ref = runMono (*fresh, x, 64);
        CHECK_LE (maxDelayedError (buf.ch[0], ref, 0, n - 4800, n), 1.0e-5);

        if (&script == &scripts.front())
        {
            // A reversal runs back from where the fade was instead of finishing
            // it first: Tape again from 5120 + 320 (+ the oversampler's
            // filter memory). Only the slowly decaying head-bump state still
            // remembers the brief Tube excursion.
            const int back = 5120 + 320 + sat->latencySamples() + 16;
            CHECK_LE (maxDelayedError (buf.ch[0], ref, 0, back, back + 960), 2.0e-3);
        }
    }
}

//==============================================================================
TEST_CASE ("Saturator: process, reset and setParams do not allocate")
{
    for (const auto& os : kOsConfigs)
    {
        auto sat = makeSat (kFs, makeParams (SaturationType::Tape, 6.0f), 2, 512, os.factor, os.quality);
        const int n = 512 * 40;
        Planar buf (2, n);
        fillAll (buf, whiteNoise (n, 0.7f, 5));

        AllocationGuard guard;
        sat->reset();
        int pos = 0, i = 0;
        const int sizes[] = { 512, 1, 77, 256, 3, 511 };
        while (pos < n)
        {
            const int len = std::min (sizes[i % 6], n - pos);
            sat->setParams (makeParams (kTypes[static_cast<size_t> ((i / 3) % 3)], static_cast<float> ((i * 5) % 25),
                                        0.25f * static_cast<float> (i % 5), static_cast<float> (i % 7) - 3.0f));
            sat->process (buf.block (pos, len));
            pos += len;
            ++i;
        }
        sat->reset();
        sat->process (buf.block (0, 512));
        CHECK (guard.allocations() == 0);
        CHECK (allFinite (buf));
    }
}

TEST_CASE ("Saturator: robust to silence, DC, full-scale noise, impulses and extreme parameters at every rate")
{
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    int run = 0;
    for (double fs : kRates)
        for (const auto& os : kOsConfigs)
        {
            const int numChannels = 1 + (run++ % kMaxChannels);
            const int seg = static_cast<int> (fs * 0.04);
            const int n = 5 * seg;
            auto sat = makeSat (fs, makeParams (static_cast<SaturationType> (run % 3), 24.0f, 1.0f, 12.0f), numChannels, 1024,
                                os.factor, os.quality);

            // silence | DC +1 | full-scale noise | impulses | DC -1
            Planar buf (numChannels, n);
            for (int c = 0; c < numChannels; ++c)
            {
                auto& ch = buf.ch[static_cast<size_t> (c)];
                const auto noise = whiteNoise (seg, 1.0f, static_cast<uint32_t> (17 + c));
                for (int i = 0; i < seg; ++i)
                {
                    ch[static_cast<size_t> (seg + i)] = 1.0f;
                    ch[static_cast<size_t> (2 * seg + i)] = noise[static_cast<size_t> (i)];
                    ch[static_cast<size_t> (3 * seg + i)] = (i % 997 == 0) ? (c % 2 == 0 ? 1.0f : -1.0f) : 0.0f;
                    ch[static_cast<size_t> (4 * seg + i)] = -1.0f;
                }
            }

            // Extreme / invalid parameter values, changed every block.
            const SaturatorParams extremes[] = {
                makeParams (SaturationType::Tube, 24.0f, 1.0f, 12.0f),
                makeParams (static_cast<SaturationType> (200), 1.0e9f, nan, inf),
                makeParams (SaturationType::Tape, -inf, 5.0f, -inf),
                makeParams (SaturationType::Digital, nan, -3.0f, nan),
                makeParams (SaturationType::Tape, 24.0f, 1.0f, 12.0f),
                makeParams (SaturationType::Tube, 0.0f, 1.0f, 12.0f),
            };
            int pos = 0, b = 0;
            while (pos < n)
            {
                const int len = std::min (1 + (b * 131) % 1024, n - pos);
                if (pos >= seg) // the silent section runs with the initial parameters
                    sat->setParams (extremes[static_cast<size_t> (b % 6)]);
                sat->process (buf.block (pos, len));
                pos += len;
                ++b;
            }
            CHECK (allFinite (buf));
            CHECK (peakOf (buf, 0, seg) == 0.0); // silence in, silence out
            CHECK_LE (peakOf (buf, 0, n), 6.0);  // +12 dB output on a full-scale dry path is ~4

            const auto& p = sat->getParams();
            CHECK (p.driveDb >= 0.0f && p.driveDb <= 24.0f);
            CHECK (p.mix >= 0.0f && p.mix <= 1.0f);
            CHECK (p.outputDb >= -12.0f && p.outputDb <= 12.0f);
        }

    // Parameter sanitising.
    Saturator sat;
    sat.setParams (makeParams (static_cast<SaturationType> (7), 100.0f, nan, -40.0f));
    CHECK (sat.getParams().type == SaturationType::Digital);
    CHECK (sat.getParams().driveDb == 24.0f);
    CHECK (sat.getParams().mix == 1.0f);
    CHECK (sat.getParams().outputDb == -12.0f);

    // Processing before prepare() is a harmless pass-through.
    Saturator unprepared;
    Planar buf (2, 64);
    fillAll (buf, sine (1000.0, kFs, 64, 0.5f));
    const auto before = buf.ch[0];
    unprepared.process (buf.block());
    CHECK (buf.ch[0] == before);
}

TEST_CASE ("Saturator: channels are independent; fewer channels than prepared is fine")
{
    const int n = 4096;
    auto sat = makeSat (kFs, makeParams (SaturationType::Tube, 12.0f), kMaxChannels, 512);
    Planar buf (kMaxChannels, n);
    const auto x = whiteNoise (n, 0.6f, 21);
    fillAll (buf, x);
    processInBlocks (*sat, buf, 512);
    for (int c = 1; c < kMaxChannels; ++c)
        CHECK (buf.ch[static_cast<size_t> (c)] == buf.ch[0]);

    // The same processor on a 3-channel block, and a mono reference.
    auto sat3 = makeSat (kFs, makeParams (SaturationType::Tube, 12.0f), kMaxChannels, 512);
    Planar three (3, n);
    load (three, 0, x);
    load (three, 2, whiteNoise (n, 0.9f, 22));
    processInBlocks (*sat3, three, 512);
    CHECK (three.ch[0] == buf.ch[0]);
    CHECK (three.ch[1] == std::vector<float> (static_cast<size_t> (n), 0.0f));
}

TEST_CASE ("Saturator: output is independent of the host block size")
{
    // Parameter changes happen at multiples of 3584 = 7 * 512, i.e. at block
    // starts for every block size used. At 192 kHz the 20 ms crossfade (3840
    // samples) is still running at the next change, so the reversal path and
    // segment splitting at the fade end are exercised too.
    for (double fs : { 48000.0, 192000.0 })
        for (const auto& os : kOsConfigs)
        {
            const int step = 3584;
            const int n = 6 * step;
            const SaturatorParams schedule[] = {
                makeParams (SaturationType::Tape, 6.0f),
                makeParams (SaturationType::Tape, 18.0f, 0.7f, 3.0f),
                makeParams (SaturationType::Tube, 18.0f, 0.7f, 3.0f),
                makeParams (SaturationType::Tape, 9.0f, 1.0f, -2.0f),
                makeParams (SaturationType::Digital, 24.0f, 0.4f, 0.0f),
                makeParams (SaturationType::Tube, 3.0f, 1.0f, 6.0f),
            };
            std::vector<float> in0 (static_cast<size_t> (n)), in1 = whiteNoise (n, 0.8f, 42);
            const auto noise = whiteNoise (n, 0.2f, 43);
            for (int i = 0; i < n; ++i)
                in0[static_cast<size_t> (i)] = static_cast<float> (0.5 * std::sin (kTwoPi * 220.0 * i / fs) + 0.2 * std::sin (kTwoPi * 5100.0 * i / fs))
                                               + noise[static_cast<size_t> (i)];

            std::vector<std::vector<float>> outs;
            for (int blockSize : { 512, 1, 7, 64 })
            {
                auto sat = makeSat (fs, schedule[0], 2, 512, os.factor, os.quality);
                Planar buf (2, n);
                load (buf, 0, in0);
                load (buf, 1, in1);
                for (int pos = 0; pos < n; pos += blockSize)
                {
                    if (pos % step == 0)
                        sat->setParams (schedule[pos / step]);
                    sat->process (buf.block (pos, std::min (blockSize, n - pos)));
                }
                CHECK (allFinite (buf));
                outs.push_back (buf.ch[0]);
                outs.back().insert (outs.back().end(), buf.ch[1].begin(), buf.ch[1].end());
            }
            for (size_t k = 1; k < outs.size(); ++k)
            {
                double err = 0.0;
                for (size_t i = 0; i < outs[0].size(); ++i)
                    err = std::max (err, static_cast<double> (std::abs (outs[k][i] - outs[0][i])));
                CHECK_LE (err, 1.0e-5);
            }
        }
}

// ---- adversarial review tests ----

TEST_CASE ("Saturator (review): no subnormal crawl in the tape emphasis after the input stops (FTZ off)")
{
    // Without FTZ the 3 kHz emphasis shelves decayed into subnormals within
    // ~150 samples of silence and stayed there until the end of the segment.
    // At 2x/4x a 4096-sample silent block after loud Tape material cost 30x
    // a loud block. A silent block must not cost more than a loud one (the
    // curve work is identical); 3x leaves room for timer noise. The minimum
    // over several runs rejects scheduler hiccups.
#if defined(FLUB_HAS_SSE_CSR)
    const unsigned int savedCsr = _mm_getcsr();
    _mm_setcsr (savedCsr & ~0x8040u); // FTZ and DAZ off, as on a host that forgot ScopedNoDenormals
    for (int factor : { 1, 2, 4 })
    {
        auto sat = makeSat (44100.0, makeParams (SaturationType::Tape, 12.0f), 2, 4096, factor);
        Planar loud (2, 4096), quiet (2, 4096);
        double loudUs = 1.0e30, quietUs = 1.0e30;
        for (int rep = 0; rep < 7; ++rep)
        {
            fillAll (loud, sine (100.0, 44100.0, 4096, 0.5f));
            fillAll (quiet, std::vector<float> (4096, 0.0f));
            sat->reset();
            const auto t0 = std::chrono::steady_clock::now();
            sat->process (loud.block());
            const auto t1 = std::chrono::steady_clock::now();
            sat->process (quiet.block());
            const auto t2 = std::chrono::steady_clock::now();
            loudUs = std::min (loudUs, std::chrono::duration<double, std::micro> (t1 - t0).count());
            quietUs = std::min (quietUs, std::chrono::duration<double, std::micro> (t2 - t1).count());
        }
        CHECK_LE (quietUs, 3.0 * loudUs + 20.0);
    }
    _mm_setcsr (savedCsr);
#endif

    // Deterministic part: a silent tail really does reach exact zero (every
    // IIR state gets flushed), and no output sample is ever subnormal.
    for (auto type : kTypes)
    {
        auto sat = makeSat (44100.0, makeParams (type, 18.0f), 1, 4096, 4);
        const int n = 44100 * 3;
        std::vector<float> x (static_cast<size_t> (n), 0.0f);
        const auto tone = sine (60.0, 44100.0, 44100, 0.8f);
        std::copy (tone.begin(), tone.end(), x.begin());
        const auto y = runMono (*sat, x, 4096);
        int subnormals = 0;
        for (float v : y)
            subnormals += (v != 0.0f && std::abs (v) < std::numeric_limits<float>::min()) ? 1 : 0;
        CHECK (subnormals == 0);
        CHECK (peakAbs (y.data() + (n - 4096), 4096) == 0.0);
    }
}

TEST_CASE ("Saturator (review): recovers from a NaN / Inf input burst")
{
    // Garbage in must not leave garbage forever: the FIR / delay memories
    // flush themselves and non-finite IIR states are cleared.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (const auto& os : kOsConfigs)
        for (auto type : kTypes)
        {
            auto sat = makeSat (kFs, makeParams (type, 12.0f), 2, 256, os.factor, os.quality);
            const int n = 9600, n0 = 1000, block = 256;
            Planar buf (2, n);
            fillAll (buf, sine (440.0, kFs, n, 0.5f));
            buf.ch[0][n0] = nan;
            buf.ch[1][n0 + 3] = inf;
            buf.ch[1][n0 + 4] = -inf;
            processInBlocks (*sat, buf, block);
            const int recovered = n0 + os.expectedLatency + 2 * block + 64;
            bool finite = true;
            for (const auto& c : buf.ch)
                for (int i = recovered; i < n; ++i)
                    finite = finite && std::isfinite (c[static_cast<size_t> (i)]);
            CHECK (finite);
            CHECK_GE (peakOf (buf, n - 960, n), 0.1); // still producing signal

            // ... and switching to Tape afterwards (whose idle emphasis states
            // may have seen the burst) is clean too.
            sat->setParams (makeParams (SaturationType::Tape, 12.0f));
            Planar more (2, 4800);
            fillAll (more, sine (440.0, kFs, 4800, 0.5f));
            processInBlocks (*sat, more, block);
            CHECK (allFinite (more));
        }
}

TEST_CASE ("Saturator (review): mix 0 stays the exact delayed dry signal during fades and ramps")
{
    // Type, drive and output change every 37-sample block (reversals, queued
    // fades, overlapping ramps): the wet path is busy, but at mix 0 it must be
    // multiplied out exactly.
    for (const auto& os : kOsConfigs)
    {
        auto sat = makeSat (kFs, makeParams (SaturationType::Tape, 0.0f, 0.0f), 2, 512, os.factor, os.quality);
        const int n = 24000;
        const auto x = whiteNoise (n, 1.0f, 3);
        Planar buf (2, n);
        fillAll (buf, x);
        for (int pos = 0, b = 0; pos < n; pos += 37, ++b)
        {
            sat->setParams (makeParams (kTypes[static_cast<size_t> ((b / 5) % 3)], static_cast<float> ((b * 7) % 25), 0.0f,
                                        static_cast<float> (b % 25) - 12.0f));
            sat->process (buf.block (pos, std::min (37, n - pos)));
        }
        CHECK_LE (maxDelayedError (buf.ch[0], x, os.expectedLatency, 0, n), 0.0);
        CHECK_LE (maxDelayedError (buf.ch[1], x, os.expectedLatency, 0, n), 0.0);
    }
}

TEST_CASE ("Saturator (review): maxBlockSize 1 gives the same output as maxBlockSize 4096")
{
    // Internal segmenting at maxBlockSize must not change the result either
    // (drive ramps, type fades and 4x oversampling with 1-sample segments).
    for (const auto& os : kOsConfigs)
    {
        const int n = 12000;
        const auto x = whiteNoise (n, 0.9f, 77);
        std::vector<std::vector<float>> outs;
        for (int maxBlock : { 4096, 1 })
        {
            auto sat = makeSat (96000.0, makeParams (SaturationType::Tube, 3.0f), 1, maxBlock, os.factor, os.quality);
            Planar buf (1, n);
            load (buf, 0, x);
            const int block = maxBlock == 1 ? 1 : 500; // every change lands on a block start
            for (int pos = 0; pos < n; pos += block)
            {
                if (pos == 1000)
                    sat->setParams (makeParams (SaturationType::Tape, 20.0f, 0.8f, -3.0f));
                if (pos == 3000)
                    sat->setParams (makeParams (SaturationType::Digital, 9.0f, 0.8f, 4.0f));
                if (pos == 3500)
                    sat->setParams (makeParams (SaturationType::Tape, 24.0f, 1.0f, 4.0f)); // reversal
                sat->process (buf.block (pos, std::min (block, n - pos)));
            }
            CHECK (allFinite (buf));
            outs.push_back (buf.ch[0]);
        }
        CHECK_LE (maxDelayedError (outs[1], outs[0], 0, 0, n), 1.0e-5);
    }
}

TEST_CASE ("Saturator (review): toggling the type every sample stays click-free")
{
    // Worst case for the fade logic: a reversal at every sample.
    const int n = 48000;
    auto sat = makeSat (kFs, makeParams (SaturationType::Tape, 18.0f), 1, 512);
    const auto x = sine (100.0, kFs, n, 0.3f);
    Planar buf (1, n);
    load (buf, 0, x);
    for (int i = 0; i < n; ++i)
    {
        sat->setParams (makeParams (i % 2 != 0 ? SaturationType::Tube : SaturationType::Digital, 18.0f));
        sat->process (buf.block (i, 1));
    }
    CHECK (allFinite (buf));
    // The steepest step of the input sine; every curve has slope <= 1.
    const double sineStep = 0.3 * kTwoPi * 100.0 / kFs;
    CHECK_LE (maxStep (buf.ch[0], 0, n), 1.1 * sineStep);
}

// ---- first-order ADAA and 8x (docs/11 E10 Phase 2) ----
namespace
{
/** The chain's designs (Oversampler::forProfile, ADAA on) at a rate. */
std::vector<Oversampler::Design> chainDesigns (double fs)
{
    return { Oversampler::forProfile (Oversampler::Profile::Quality, fs), Oversampler::forProfile (Oversampler::Profile::Balanced, fs) };
}

std::unique_ptr<Saturator> makeSat (double sampleRate, const SaturatorParams& p, const Oversampler::Design& design, int numChannels = 1,
                                    int maxBlock = 512)
{
    auto s = std::make_unique<Saturator>();
    s->setOversampling (design);
    s->setParams (p);
    s->prepare ({ sampleRate, maxBlock, numChannels });
    return s;
}
} // namespace

TEST_CASE ("Saturator ADAA: the chain's designs keep unity small-signal gain and an exact latency at every rate")
{
    // The ADAA curve is half an oversampled sample late and the stage-1
    // decimator takes it back, but only on the deviation: the programme still
    // comes from the exact dry delay, so a quiet tone passes at unity and an
    // impulse in the linear region lands exactly on the reported latency.
    for (double fs : kRates)
        for (const auto& design : chainDesigns (fs))
            for (auto type : kTypes)
            {
                CHECK (design.adaa);
                auto sat = makeSat (fs, makeParams (type, 12.0f), design);
                const auto r = runTone (*sat, 1000.0, fs, 0.01f);
                CHECK_NEAR (toDb (toneAmplitude (r.out.data() + r.start, r.length, 1000.0, fs) / 0.01), 0.0, 0.02);

                auto impulse = makeSat (fs, makeParams (type, 12.0f), design);
                const int lat = impulse->latencySamples(), n0 = 300, n = 1024;
                std::vector<float> x (static_cast<size_t> (n), 0.0f);
                x[static_cast<size_t> (n0)] = 1.0e-3f;
                const auto y = runMono (*impulse, x, 64);
                int argMax = 0;
                for (int i = 1; i < n; ++i)
                    if (std::abs (y[static_cast<size_t> (i)]) > std::abs (y[static_cast<size_t> (argMax)]))
                        argMax = i;
                CHECK (argMax == n0 + lat);
                CHECK_NEAR (y[static_cast<size_t> (n0 + lat)], 1.0e-3, 0.05e-3); // Tube's even term adds ~1e-6
            }
}

TEST_CASE ("Saturator ADAA: the deviation stays aligned with the dry path - at 1 / 5 kHz and 12 dB the output's fundamental and 3rd harmonic match the plain curve's, and so does the THD+N telemetry")
{
    // Same design with and without ADAA. The half sample the decimator takes
    // back is what keeps the phases equal: a deviation half an oversampled
    // sample late turns the 5 kHz fundamental by 0.16 rad at 4x (0.08 at 8x).
    // What is left is ADAA1's own error, the curve evaluated on a linear
    // path between samples (O(step^2): -0.04 dB on the 5 kHz fundamental at
    // 4x, -0.1 dB on its 15 kHz harmonic; 4x smaller at 8x), and the
    // aliasing, which only the ADAA side lacks.
    const auto phasor = [] (const ToneResult& r, double f) {
        double re = 0.0, im = 0.0;
        for (int i = 0; i < r.length; ++i)
        {
            const double v = r.out[static_cast<size_t> (r.start + i)];
            re += v * std::cos (kTwoPi * f * i / kFs);
            im -= v * std::sin (kTwoPi * f * i / kFs);
        }
        return std::pair { 2.0 * std::hypot (re, im) / r.length, std::atan2 (im, re) };
    };
    for (const auto& withAdaa : chainDesigns (48000.0))
    {
        auto plain = withAdaa;
        plain.adaa = false;
        for (auto type : kTypes)
            for (double f : { 1000.0, 5000.0 })
            {
                auto a = makeSat (kFs, makeParams (type, 12.0f), withAdaa);
                auto b = makeSat (kFs, makeParams (type, 12.0f), plain);
                const auto ra = runTone (*a, f, kFs, 0.5f);
                const auto rb = runTone (*b, f, kFs, 0.5f);
                for (int h : { 1, 3 })
                {
                    const auto [magA, phaseA] = phasor (ra, h * f);
                    const auto [magB, phaseB] = phasor (rb, h * f);
                    if (magB < 1.0e-4)
                        continue; // (a harmonic the curve barely makes)
                    CHECK_NEAR (toDb (magA / magB), 0.0, 0.15);
                    CHECK_NEAR (std::remainder (phaseA - phaseB, kTwoPi), 0.0, 0.005);
                }
                CHECK_NEAR (a->getDistortionDb(), b->getDistortionDb(), 0.3);
                CHECK_GE (a->getDistortionDb(), -40.0); // it is saturating
            }
    }
}

TEST_CASE ("Saturator ADAA: parameter and type changes are click-free and every host block size gives the same output (4x and 8x)")
{
    for (const auto& [fs, design] : { std::pair { 48000.0, Oversampler::forProfile (Oversampler::Profile::Quality, 48000.0) },
                                      std::pair { 44100.0, Oversampler::forProfile (Oversampler::Profile::Balanced, 44100.0) } })
    {
        const int step = 3584, n = 6 * step;
        const SaturatorParams schedule[] = {
            makeParams (SaturationType::Tape, 6.0f),
            makeParams (SaturationType::Tape, 18.0f, 0.7f, 3.0f),
            makeParams (SaturationType::Tube, 18.0f, 0.7f, 3.0f),
            makeParams (SaturationType::Tape, 9.0f, 1.0f, -2.0f),
            makeParams (SaturationType::Digital, 24.0f, 0.4f, 0.0f),
            makeParams (SaturationType::Tube, 3.0f, 1.0f, 6.0f),
        };
        std::vector<float> in0 (static_cast<size_t> (n)), in1 = whiteNoise (n, 0.8f, 42);
        const auto noise = whiteNoise (n, 0.2f, 43);
        for (int i = 0; i < n; ++i)
            in0[static_cast<size_t> (i)] = static_cast<float> (0.5 * std::sin (kTwoPi * 220.0 * i / fs) + 0.2 * std::sin (kTwoPi * 5100.0 * i / fs))
                                           + noise[static_cast<size_t> (i)];
        std::vector<std::vector<float>> outs;
        for (int blockSize : { 512, 1, 7, 64 })
        {
            auto sat = makeSat (fs, schedule[0], design, 2, 512);
            Planar buf (2, n);
            load (buf, 0, in0);
            load (buf, 1, in1);
            for (int pos = 0; pos < n; pos += blockSize)
            {
                if (pos % step == 0)
                    sat->setParams (schedule[pos / step]);
                sat->process (buf.block (pos, std::min (blockSize, n - pos)));
            }
            CHECK (allFinite (buf));
            outs.push_back (buf.ch[0]);
            outs.back().insert (outs.back().end(), buf.ch[1].begin(), buf.ch[1].end());
        }
        for (size_t k = 1; k < outs.size(); ++k)
        {
            double err = 0.0;
            for (size_t i = 0; i < outs[0].size(); ++i)
                err = std::max (err, static_cast<double> (std::abs (outs[k][i] - outs[0][i])));
            CHECK_LE (err, 1.0e-5);
        }

        // Drive jumps and type changes on a 100 Hz sine: no step larger than
        // the steady state's on either side (as for the plain curves).
        const int m = static_cast<int> (fs * 0.6), change = 9600;
        for (const auto& [before, after] : { std::pair { makeParams (SaturationType::Tape, 0.0f), makeParams (SaturationType::Tape, 18.0f) },
                                             std::pair { makeParams (SaturationType::Tube, 24.0f), makeParams (SaturationType::Tube, 0.0f) },
                                             std::pair { makeParams (SaturationType::Tape, 18.0f), makeParams (SaturationType::Digital, 18.0f) } })
        {
            auto sat = makeSat (fs, before, design, 1, 64);
            Planar buf (1, m);
            load (buf, 0, sine (100.0, fs, m, 0.3f));
            for (int pos = 0; pos < m; pos += 64)
            {
                if (pos == change)
                    sat->setParams (after);
                sat->process (buf.block (pos, std::min (64, m - pos)));
            }
            const auto& y = buf.ch[0];
            CHECK_LE (maxStep (y, change, m), 1.1 * std::max (maxStep (y, 4800, change), maxStep (y, m - 4800, m)));
        }
    }
}

TEST_CASE ("Saturator ADAA: silence, quiet tails, NaN / Inf bursts and +24 dBFS input stay finite and recover (4x and 8x)")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (const auto& design : chainDesigns (44100.0))
        for (auto type : kTypes)
        {
            auto sat = makeSat (44100.0, makeParams (type, 24.0f), design, 2, 256);
            const int n = 12000, n0 = 1000;
            Planar buf (2, n);
            fillAll (buf, sine (440.0, 44100.0, n, 0.5f));
            buf.ch[0][n0] = nan;
            buf.ch[1][n0 + 3] = inf;
            buf.ch[1][n0 + 4] = -inf;
            buf.ch[0][n0 + 2000] = 15.8f; // +24 dBFS
            processInBlocks (*sat, buf, 256);
            bool finite = true;
            for (const auto& c : buf.ch)
                for (int i = n0 + 2000 + 64; i < n; ++i)
                    finite = finite && std::isfinite (c[static_cast<size_t> (i)]);
            CHECK (finite);
            CHECK_GE (peakOf (buf, n - 960, n), 0.01);

            // Silence afterwards decays to exact zero (the ADAA's previous
            // sample and every state are flushed).
            Planar quiet (2, 2 * 44100);
            processInBlocks (*sat, quiet, 256);
            CHECK_LE (peakOf (quiet, 2 * 44100 - 256, 2 * 44100), 0.0);
        }
}

TEST_CASE ("Saturator: an 8x design in Balanced / Low Latency's 16 samples meets the 24 dB alias rows at 44.1 / 48 kHz, at twice the curve's CPU (docs/11 E10: evaluated, not in the table)")
{
    // The E10 rows Balanced / Low Latency miss (pinned in
    // test_signal_hygiene.cpp: Tape 24 dB -54.1 / -59.3 dBc at 44.1 /
    // 48 kHz, 4x with ADAA): 8x fits the same 16 samples (4 + 8 + 3 + 1:
    // a 33-tap stage-1 decimator, the 9-tap third half-band) and meets
    // them. Not adopted: it doubles the curve's cost (saturator alone,
    // stereo, 44.1 kHz: 15.4 -> 29.1 ms per second of audio), and the CLI's
    // realtime factor with the saturator at 9 dB falls 22.7 -> 17.0x
    // (Balanced) and 22.4 -> 14.7x (Low Latency), past docs/11 E10's
    // <= 25 % budget; the design is kept here, measured, for when it is.
    constexpr int kN = 65536;
    auto design = Oversampler::forProfile (Oversampler::Profile::Balanced, 44100.0);
    design.factor = 8;
    design.m1 = 8;
    design.d3 = 2;
    design.beta3 = 5.0;
    for (const double fs : { 44100.0, 48000.0 })
        for (const auto type : { SaturationType::Tape, SaturationType::Tube, SaturationType::Digital })
        {
            double worst = -200.0;
            for (const double hz : { 1000.0, 5000.0, 7000.0, 10000.0 })
            {
                const int bin = cli::aliasToneBin (hz, fs, kN);
                Saturator sat;
                sat.setOversampling (design);
                sat.prepare ({ fs, 512, 1 });
                CHECK (sat.latencySamples() == 16);
                SaturatorParams p;
                p.type = type;
                p.driveDb = 24.0f;
                sat.setParams (p);
                sat.reset();
                const int n = static_cast<int> (fs / 4) + kN;
                Planar buf (1, n);
                const double f0 = bin * fs / kN;
                for (int i = 0; i < n; ++i)
                    buf.ch[0][static_cast<size_t> (i)] = static_cast<float> (0.5 * std::sin (kTwoPi * f0 * i / fs));
                processInBlocks (sat, buf, 512);
                worst = std::max (worst, cli::worstAliasDbc (buf.ch[0].data() + (n - kN), kN, fs, bin));
            }
            std::printf ("    measured saturator alias, 8x in 16 samples, type %d, 24 dB, %.0f Hz: %.1f dBc\n", static_cast<int> (type), fs, worst);
            CHECK_LE (worst, -70.0);
        }
}
