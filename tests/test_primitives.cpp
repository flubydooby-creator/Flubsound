// Tests for the shared DSP primitives (SVF, biquad, LR4, oversampler,
// true-peak detector, FFT, ring buffer, delay line).
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/common/DelayLine.h"
#include "flub/common/SmoothedValue.h"
#include "flub/common/SpscRing.h"
#include "flub/dsp/Biquad.h"
#include "flub/dsp/Crossover.h"
#include "flub/dsp/Fft.h"
#include "flub/dsp/Oversampler.h"
#include "flub/dsp/Svf.h"
#include "flub/dsp/TruePeakDetector.h"

#include <complex>

using namespace flub;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;

double svfMeasuredDb (const SvfCoeffs& c, double freq)
{
    SvfState st;
    const int n = 48000;
    auto s = sine (freq, kFs, n, 0.25f);
    for (auto& v : s)
        v = svfTick (c, st, v);
    return toDb (toneAmplitude (s.data() + n / 2, n / 2, freq, kFs) / 0.25);
}
} // namespace

TEST_CASE ("Svf: bell reaches its gain at the centre frequency and is flat far away")
{
    const auto c = SvfCoeffs::make (FilterType::Bell, 1000.0, 1.0, 6.0, kFs);
    CHECK_NEAR (svfMeasuredDb (c, 1000.0), 6.0, 0.05);
    CHECK_NEAR (svfMeasuredDb (c, 30.0), 0.0, 0.1);
    CHECK_NEAR (c.magnitudeDb (1000.0, kFs), 6.0, 1.0e-6);
    const auto cut = SvfCoeffs::make (FilterType::Bell, 2500.0, 2.0, -9.0, kFs);
    CHECK_NEAR (svfMeasuredDb (cut, 2500.0), -9.0, 0.05);
}

TEST_CASE ("Svf: shelves reach their gain at DC / Nyquist side")
{
    const auto ls = SvfCoeffs::make (FilterType::LowShelf, 200.0, 0.7071, 8.0, kFs);
    CHECK_NEAR (ls.magnitudeDb (5.0, kFs), 8.0, 0.05);
    CHECK_NEAR (ls.magnitudeDb (15000.0, kFs), 0.0, 0.05);
    CHECK_NEAR (ls.magnitudeDb (200.0, kFs), 4.0, 0.1); // half gain at the corner
    const auto hs = SvfCoeffs::make (FilterType::HighShelf, 8000.0, 0.7071, -6.0, kFs);
    CHECK_NEAR (hs.magnitudeDb (20.0, kFs), 0.0, 0.05);
    CHECK_NEAR (hs.magnitudeDb (23000.0, kFs), -6.0, 0.2);
}

TEST_CASE ("Svf: analytic response() matches the running filter for every type")
{
    const FilterType types[] = { FilterType::Bell, FilterType::LowShelf, FilterType::HighShelf, FilterType::LowPass,
                                 FilterType::HighPass, FilterType::BandPass, FilterType::Notch, FilterType::AllPass };
    for (auto t : types)
    {
        const auto c = SvfCoeffs::make (t, 1200.0, 0.9, 5.0, kFs);
        for (double f : { 100.0, 700.0, 3000.0, 9000.0 })
        {
            const double predicted = c.magnitudeDb (f, kFs);
            if (predicted < -40.0)
                continue; // below measurement precision
            CHECK_NEAR (svfMeasuredDb (c, f), predicted, 0.05);
        }
    }
}

TEST_CASE ("Svf: Butterworth Q table")
{
    CHECK_NEAR (butterworthQ (1, 0), 0.70711, 1e-4);
    CHECK_NEAR (butterworthQ (2, 0), 1.30656, 1e-4);
    CHECK_NEAR (butterworthQ (2, 1), 0.54120, 1e-4);
    CHECK_NEAR (butterworthQ (4, 0), 2.56292, 1e-4);
    CHECK_NEAR (butterworthQ (4, 3), 0.50980, 1e-4);
}

TEST_CASE ("LR4: bands sum to a flat magnitude; each band is -6 dB at the crossover")
{
    LinkwitzRiley4 xo;
    xo.prepare (kFs);
    xo.setFrequency (1000.0);
    for (double f : { 50.0, 400.0, 1000.0, 2500.0, 12000.0 })
    {
        xo.reset();
        const int n = 48000;
        auto s = sine (f, kFs, n, 0.5f);
        std::vector<float> lo (n), hi (n), sum (n);
        for (int i = 0; i < n; ++i)
        {
            xo.processSample (0, s[static_cast<size_t> (i)], lo[static_cast<size_t> (i)], hi[static_cast<size_t> (i)]);
            sum[static_cast<size_t> (i)] = lo[static_cast<size_t> (i)] + hi[static_cast<size_t> (i)];
        }
        CHECK_NEAR (toDb (toneAmplitude (sum.data() + n / 2, n / 2, f, kFs) / 0.5), 0.0, 0.02);
        if (f == 1000.0)
        {
            CHECK_NEAR (toDb (toneAmplitude (lo.data() + n / 2, n / 2, f, kFs) / 0.5), -6.02, 0.05);
            CHECK_NEAR (toDb (toneAmplitude (hi.data() + n / 2, n / 2, f, kFs) / 0.5), -6.02, 0.05);
        }
    }
}

TEST_CASE ("ThreeBandSplitter: low + mid + high is flat")
{
    ThreeBandSplitter sp;
    sp.prepare (kFs, 120.0, 4000.0);
    for (double f : { 40.0, 120.0, 700.0, 4000.0, 15000.0 })
    {
        sp.reset();
        const int n = 48000;
        auto s = sine (f, kFs, n, 0.5f);
        std::vector<float> sum (n);
        for (int i = 0; i < n; ++i)
        {
            float l, m, h;
            sp.processSample (0, s[static_cast<size_t> (i)], l, m, h);
            sum[static_cast<size_t> (i)] = l + m + h;
        }
        CHECK_NEAR (toDb (toneAmplitude (sum.data() + n / 2, n / 2, f, kFs) / 0.5), 0.0, 0.02);
    }
}

TEST_CASE ("Biquad: first-order analog BLT keeps DC and HF gains")
{
    // H(s) = (1 + 2 s/w) / (1 + s/w): DC gain 1, HF gain 2 (+6 dB)
    const double w = 2.0 * kPi * 1000.0;
    const auto c = BiquadCoeffs::fromAnalogFirstOrder (1.0, 2.0 / w, 1.0, 1.0 / w, kFs);
    CHECK_NEAR (std::abs (c.response (1.0, kFs)), 1.0, 1e-4);
    CHECK_NEAR (std::abs (c.response (23900.0, kFs)), 2.0, 0.01);
}

TEST_CASE ("Oversampler: round trip reproduces the input delayed by the reported latency")
{
    for (int factor : { 2, 4 })
        for (auto q : { Oversampler::Quality::High, Oversampler::Quality::Low })
        {
            Oversampler os;
            const int block = 128;
            os.prepare (2, block, factor, q);
            const int lat = os.latencySamples();
            CHECK (lat > 0);
            const int n = 16384;
            auto in = sine (997.0, kFs, n, 0.5f);
            Planar buf (2, n);
            buf.ch[0] = in;
            buf.ch[1] = in;
            for (int pos = 0; pos < n; pos += block)
            {
                auto b = buf.block (pos, block);
                auto up = os.upsample (b);
                CHECK (up.numSamples == block * factor);
                os.downsample (b);
            }
            double errPow = 0.0, sigPow = 0.0;
            for (int i = 4096; i < n; ++i)
            {
                const double e = buf.ch[0][static_cast<size_t> (i)] - in[static_cast<size_t> (i - lat)];
                errPow += e * e;
                sigPow += static_cast<double> (in[static_cast<size_t> (i - lat)]) * in[static_cast<size_t> (i - lat)];
            }
            const double snrDb = 10.0 * std::log10 (sigPow / std::max (1e-30, errPow));
            CHECK_GE (snrDb, q == Oversampler::Quality::High ? 80.0 : 45.0);
        }
}

TEST_CASE ("Oversampler: latency values per quality")
{
    Oversampler os;
    os.prepare (2, 64, 4, Oversampler::Quality::High);
    CHECK (os.latencySamples() == 36);
    os.prepare (2, 64, 2, Oversampler::Quality::High);
    CHECK (os.latencySamples() == 32);
    os.prepare (2, 64, 4, Oversampler::Quality::Low);
    CHECK (os.latencySamples() == 19);
    os.prepare (2, 64, 2, Oversampler::Quality::Low);
    CHECK (os.latencySamples() == 16);
    os.prepare (2, 64, 1);
    CHECK (os.latencySamples() == 0);
}

TEST_CASE ("TruePeakDetector: finds the inter-sample peak of an fs/4 sine at 45 degrees")
{
    TruePeakDetector tp;
    tp.prepare (1);
    // Samples of sin(pi/2 n + pi/4) are +-0.7071: sample peak -3 dB, true peak 0 dB.
    const int n = 4800;
    auto s = sine (kFs / 4.0, kFs, n, 1.0f, kPi / 4.0);
    float samplePeak = 0.0f, truePeak = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        samplePeak = std::max (samplePeak, std::abs (s[static_cast<size_t> (i)]));
        const float p = tp.processSample (0, s[static_cast<size_t> (i)]);
        if (i > 100)
            truePeak = std::max (truePeak, p);
    }
    CHECK_NEAR (toDb (samplePeak), -3.01, 0.01);
    CHECK_NEAR (toDb (truePeak), 0.0, 0.15);
}

TEST_CASE ("TruePeakDetector: phase 0 reproduces the input with kDelay samples of delay")
{
    TruePeakDetector tp;
    tp.prepare (1);
    std::vector<float> outs;
    for (int i = 0; i < 64; ++i)
        outs.push_back (tp.processSample (0, i == 5 ? 0.5f : 0.0f));
    // The impulse at n=5 must be reported (via phase 0 or neighbours) at n=5+kDelay.
    CHECK_NEAR (outs[static_cast<size_t> (5 + TruePeakDetector::kDelay)], 0.5, 1e-6);
}

TEST_CASE ("Fft: forward/inverse round trip and a pure tone lands in one bin")
{
    Fft fft;
    fft.prepare (1024);
    std::vector<float> x = sine (48000.0 * 10.0 / 1024.0, kFs, 1024, 1.0f);
    std::vector<std::complex<float>> bins (513);
    fft.forwardReal (x.data(), bins.data());
    CHECK_NEAR (std::abs (bins[10]), 512.0, 1e-2);
    CHECK_LE (std::abs (bins[40]), 1e-2);
    std::vector<float> y (1024);
    fft.inverseReal (bins.data(), y.data());
    double err = 0.0;
    for (int i = 0; i < 1024; ++i)
        err = std::max (err, static_cast<double> (std::abs (y[static_cast<size_t> (i)] - x[static_cast<size_t> (i)])));
    CHECK_LE (err, 1e-5);
}

TEST_CASE ("SpscRing: FIFO order, capacity and overflow drop")
{
    SpscRing<int> ring (8);
    CHECK (ring.capacity() >= 8);
    int data[20];
    for (int i = 0; i < 20; ++i)
        data[i] = i;
    const size_t written = ring.push (data, 20);
    CHECK (written == ring.capacity());
    int out[20] = {};
    const size_t read = ring.pop (out, 20);
    CHECK (read == written);
    for (size_t i = 0; i < read; ++i)
        CHECK (out[i] == static_cast<int> (i));
    CHECK (ring.available() == 0);
}

TEST_CASE ("DelayLine: block delay equals per-sample delay")
{
    DelayLine dl;
    dl.prepare (2, 7);
    Planar buf (2, 64);
    for (int i = 0; i < 64; ++i)
        buf.ch[0][static_cast<size_t> (i)] = buf.ch[1][static_cast<size_t> (i)] = static_cast<float> (i + 1);
    dl.process (buf.block (0, 30));
    dl.process (buf.block (30, 34));
    for (int i = 0; i < 64; ++i)
    {
        const float expected = i < 7 ? 0.0f : static_cast<float> (i - 7 + 1);
        CHECK (buf.ch[0][static_cast<size_t> (i)] == expected);
        CHECK (buf.ch[1][static_cast<size_t> (i)] == expected);
    }
}

TEST_CASE ("OnePoleSmoother: glides converge exactly to non-zero targets (no float stall)")
{
    for (double controlRate : { 44100.0 / 16, 48000.0 / 16, 48000.0, 192000.0 })
        for (float target : { 1.5f, 6.0f, 9.97f, 14.29f, 24.0f, -3.3f, 1000.0f })
        {
            OnePoleSmoother s;
            s.reset (controlRate, 20.0f, 0.0f);
            s.setTarget (target);
            int steps = 0;
            while (s.isSmoothing() && steps < 1000000)
            {
                s.next();
                ++steps;
            }
            CHECK (s.getCurrent() == target);
            CHECK (! s.isSmoothing());
        }
    OnePoleSmoother k;
    k.reset (48000.0, 20.0f, 0.0f);
    k.setTarget (12.0f);
    for (int i = 0; i < 100000 && k.isSmoothing(); ++i)
        k.skip (16);
    CHECK (k.getCurrent() == 12.0f);
}
