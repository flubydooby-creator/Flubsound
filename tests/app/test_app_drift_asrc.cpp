// App-level tests: docs/11 E50 Phase A, the capture FIFO's band-limited
// resampler (DriftCompensatedFifo::Kernel, a polyphase Kaiser-windowed sinc).
//   * The kernel is flat within +-0.1 dB to 20 kHz at fractional phases 0,
//     0.25, 0.5 and 0.75, and its group delay is fixedDelayFrames() +- 0.5.
//   * Resampling at 1 +- 200 ppm: THD+N (images included) <= -100 dB for
//     1 kHz and 15 kHz at -1 dBFS, measured against the exact sine at every
//     output position.
//   * Through the FIFO (a capture 200 ppm fast, 441-frame packets, 128-frame
//     blocks): 10 kHz and 20 kHz keep their level within 0.1 dB, pull() does
//     not allocate, and the delay is reported.
// The 4-point Catmull-Rom interpolator the kernel replaced is evaluated
// alongside for the before -> after numbers.
#include "AppTestSupport.h"

#include "engine/DriftCompensatedFifo.h"

#include "flub/common/Math.h"

#include <array>
#include <cmath>
#include <complex>
#include <iostream>
#include <vector>

using flub::app::DriftCompensatedFifo;
using Kernel = DriftCompensatedFifo::Kernel;

namespace
{
constexpr double kFs = 48000.0;

std::vector<double> kernelAt (double fraction)
{
    std::array<float, Kernel::kTaps> c {};
    Kernel::coefficients (Kernel::table(), static_cast<float> (fraction), c.data());
    return { c.begin(), c.end() };
}

/** The Catmull-Rom interpolator before E50: taps for h0..h3 (oldest first),
    output between h1 and h2 at `t`. */
std::vector<double> hermiteAt (double t)
{
    const double t2 = t * t, t3 = t2 * t;
    return { -0.5 * t + t2 - 0.5 * t3, 1.0 - 2.5 * t2 + 1.5 * t3, 0.5 * t + 2.0 * t2 - 1.5 * t3, -0.5 * t2 + 0.5 * t3 };
}

/** Frequency response of taps c (oldest first; the last one on the newest sample). */
std::complex<double> responseAt (const std::vector<double>& c, double hz)
{
    const double w = flub::kTwoPi * hz / kFs;
    std::complex<double> h {};
    const auto n = c.size();
    for (size_t k = 0; k < n; ++k)
        h += c[k] * std::polar (1.0, -w * static_cast<double> (n - 1 - k));
    return h;
}

double magnitudeDb (const std::vector<double>& c, double hz)
{
    return 20.0 * std::log10 (std::abs (responseAt (c, hz)));
}

/** Group delay in frames behind the newest input sample. */
double groupDelay (const std::vector<double>& c, double hz)
{
    constexpr double dHz = 1.0;
    const double a = std::arg (responseAt (c, hz - dHz)), b = std::arg (responseAt (c, hz + dHz));
    double d = b - a;
    while (d > flub::kPi)
        d -= flub::kTwoPi;
    while (d < -flub::kPi)
        d += flub::kTwoPi;
    return -d / (flub::kTwoPi * 2.0 * dHz / kFs);
}

/** Resamples a -1 dBFS sine at `ratio` input frames per output frame with
    taps from `taps (fraction)` (output between window[n/2 - 1] and
    window[n/2]) and returns THD+N in dB: every error against the exact sine
    at the output's input position (distortion, images, noise). */
template <typename Taps>
double resampledThdN (double hz, double ratio, Taps&& taps, int numTaps)
{
    const double amp = std::pow (10.0, -1.0 / 20.0);
    const double w = flub::kTwoPi * hz / kFs;
    constexpr int kOut = 24000;
    const int half = numTaps / 2;
    std::vector<float> x (static_cast<size_t> (kOut * ratio) + static_cast<size_t> (numTaps) + 64);
    for (size_t n = 0; n < x.size(); ++n)
        x[n] = static_cast<float> (amp * std::sin (w * static_cast<double> (n) + 0.3));

    double err = 0.0, sig = 0.0;
    for (int m = 0; m < kOut; ++m)
    {
        const double position = static_cast<double> (numTaps) + static_cast<double> (m) * ratio;
        const auto n0 = static_cast<size_t> (position);
        const double fraction = position - static_cast<double> (n0);
        const auto c = taps (fraction);
        double y = 0.0;
        for (int k = 0; k < numTaps; ++k)
            y += c[static_cast<size_t> (k)] * static_cast<double> (x[n0 - static_cast<size_t> (half - 1) + static_cast<size_t> (k)]);
        const double ideal = amp * std::sin (w * position + 0.3);
        err += (y - ideal) * (y - ideal);
        sig += ideal * ideal;
    }
    return 10.0 * std::log10 (err / sig);
}
} // namespace

TEST_CASE ("App: E50 resampler kernel: flat to 20 kHz at every phase, group delay = fixedDelayFrames +- 0.5")
{
    double worstDev = 0.0, hermiteWorst = 0.0;
    for (const double t : { 0.0, 0.25, 0.5, 0.75 })
    {
        const auto c = kernelAt (t);
        double sum = 0.0;
        for (const double v : c)
            sum += v;
        CHECK_NEAR (sum, 1.0, 1.0e-5);
        for (double hz = 0.0; hz <= 20000.0; hz += 250.0)
        {
            const double db = magnitudeDb (c, std::max (hz, 1.0));
            worstDev = std::max (worstDev, std::abs (db));
            CHECK_LE (std::abs (db), 0.1);
            hermiteWorst = std::max (hermiteWorst, std::abs (magnitudeDb (hermiteAt (t), std::max (hz, 1.0))));
        }
        for (const double hz : { 1000.0, 10000.0, 20000.0 })
        {
            const double delay = groupDelay (c, hz);
            CHECK_LE (std::abs (delay - DriftCompensatedFifo::fixedDelayFrames()), 0.5 + 1.0e-6);
            CHECK_NEAR (delay, 0.5 * Kernel::kTaps - t, 0.01);
        }
    }
    std::cerr << "    20 kHz at phase 0.25 / 0.5: Catmull-Rom " << magnitudeDb (hermiteAt (0.25), 20000.0) << " / "
              << magnitudeDb (hermiteAt (0.5), 20000.0) << " dB -> kernel " << magnitudeDb (kernelAt (0.25), 20000.0) << " / "
              << magnitudeDb (kernelAt (0.5), 20000.0) << " dB; worst deviation to 20 kHz " << hermiteWorst << " -> " << worstDev << " dB\n";
    CHECK_LE (worstDev, 0.01);

    // Phase 0 is the identity: a stream at a ratio of exactly 1 passes bit-exact
    // apart from the delay.
    const auto identity = kernelAt (0.0);
    for (int k = 0; k < Kernel::kTaps; ++k)
        CHECK_NEAR (identity[static_cast<size_t> (k)], k == Kernel::kTaps / 2 - 1 ? 1.0 : 0.0, 1.0e-6);

    // Images: the continuous kernel the phases sample (spacing 1 / kPhases)
    // must reject 28 kHz and up, where the images of anything up to 20 kHz
    // fall (fs - 20 kHz), before they alias back next to the tone.
    const float* rows = Kernel::table();
    double stopband = -1000.0;
    for (double hz = 28000.0; hz <= 200000.0; hz += 250.0)
    {
        std::complex<double> g {};
        for (int p = 0; p < Kernel::kPhases; ++p)
            for (int k = 0; k < Kernel::kTaps; ++k)
            {
                const double x = static_cast<double> (k) - (0.5 * Kernel::kTaps - 1.0) - static_cast<double> (p) / Kernel::kPhases;
                g += static_cast<double> (rows[p * Kernel::kTaps + k]) * std::polar (1.0, -flub::kTwoPi * hz * x / kFs);
            }
        stopband = std::max (stopband, 20.0 * std::log10 (std::abs (g) / Kernel::kPhases + 1.0e-30));
    }
    std::cerr << "    kernel stopband (28-200 kHz, where images land): " << stopband << " dB\n";
    CHECK_LE (stopband, -100.0);
}

TEST_CASE ("App: E50 resampling at 1 +- 200 ppm: THD+N <= -100 dB at 1 and 15 kHz, -1 dBFS")
{
    for (const double hz : { 1000.0, 15000.0 })
        for (const double ppm : { -200.0, 200.0 })
        {
            const double ratio = 1.0 + ppm * 1.0e-6;
            const double after = resampledThdN (hz, ratio, kernelAt, Kernel::kTaps);
            const double before = resampledThdN (hz, ratio, hermiteAt, 4);
            std::cerr << "    " << hz << " Hz at " << ppm << " ppm: THD+N " << before << " dB (Catmull-Rom) -> " << after << " dB\n";
            CHECK_LE (after, -100.0);
        }
}

TEST_CASE ("App: E50 through the FIFO: 10 and 20 kHz keep their level at +200 ppm, no allocation, delay reported")
{
    for (const double hz : { 10000.0, 20000.0 })
    {
        DriftCompensatedFifo fifo;
        fifo.prepare (2, kFs, kFs, 128);
        const auto stats0 = fifo.getStats();
        CHECK_NEAR (stats0.resamplerDelayMs, 1000.0 * DriftCompensatedFifo::fixedDelayFrames() / kFs, 1.0e-4);

        const double amp = std::pow (10.0, -1.0 / 20.0);
        const double w = flub::kTwoPi * hz / kFs;
        constexpr double drift = 200.0e-6; // the capture clock runs fast
        constexpr int kPacket = 441, kBlock = 128;
        constexpr double kSeconds = 3.0;
        std::vector<float> packet (static_cast<size_t> (kPacket * 2));
        std::array<std::vector<float>, 2> out { std::vector<float> (kBlock), std::vector<float> (kBlock) };
        const std::array<float*, 2> dest { out[0].data(), out[1].data() };
        int64_t produced = 0;
        double inSq = 0.0, outSq = 0.0;
        int64_t measured = 0, allocations = 0;
        const auto blocks = static_cast<int> (kSeconds * kFs / kBlock);
        for (int b = 0; b < blocks; ++b)
        {
            // Everything the capture clock has produced by the end of this block.
            const auto due = static_cast<int64_t> (static_cast<double> ((b + 1) * kBlock) * (1.0 + drift));
            while (produced + kPacket <= due)
            {
                for (int i = 0; i < kPacket; ++i)
                {
                    const auto v = static_cast<float> (amp * std::sin (w * static_cast<double> (produced + i)));
                    packet[static_cast<size_t> (2 * i)] = v;
                    packet[static_cast<size_t> (2 * i + 1)] = v;
                }
                fifo.push (packet.data(), kPacket, 2);
                produced += kPacket;
            }
            flubapptest::RealtimeProbe probe;
            fifo.pull (dest.data(), 2, kBlock, false);
            allocations += probe.allocations();
            if (b >= blocks / 2)
                for (int i = 0; i < kBlock; ++i)
                {
                    outSq += static_cast<double> (out[0][static_cast<size_t> (i)]) * static_cast<double> (out[0][static_cast<size_t> (i)]);
                    inSq += 0.5 * amp * amp;
                    ++measured;
                }
        }
        const auto stats = fifo.getStats();
        const double levelDb = 10.0 * std::log10 (outSq / inSq);
        std::cerr << "    " << hz << " Hz through the FIFO at +200 ppm: level " << levelDb << " dB, correction " << stats.correctionPpm
                  << " ppm, underruns " << stats.underruns << "\n";
        CHECK (measured > 0);
        CHECK (stats.streaming);
        CHECK (stats.underruns == 0);
        CHECK_LE (std::abs (levelDb), 0.1);
        CHECK (allocations == 0);
    }
}
