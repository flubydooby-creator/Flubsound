#include "flub/dsp/WeightedResidual.h"

#include "flub/analysis/LoudnessMeter.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace flub
{
namespace
{
// Upper edges of the critical bands (Zwicker); the last one is capped at 20 kHz / 0.49 fs.
constexpr std::array<float, WeightedResidual::kNumBands> kBarkUpperHz {
    100.0f, 200.0f, 300.0f, 400.0f, 510.0f, 630.0f, 770.0f, 920.0f, 1080.0f, 1270.0f, 1480.0f, 1720.0f, 2000.0f,
    2320.0f, 2700.0f, 3150.0f, 3700.0f, 4400.0f, 5300.0f, 6400.0f, 7700.0f, 9500.0f, 12000.0f, 15500.0f, 20000.0f
};
constexpr double kLowestHz = 40.0;
constexpr std::int64_t kMinFrames = 8; // readings start once the averages hold this many frames
constexpr double kReferenceFloor = 1.0e-12; // a bin's reference below this x the mean is all incoherent
constexpr double kRidingShare = 0.5;        // a band is gain-normalised when its predicted output is at least this share
constexpr double kMaxGainOverSpan = 4.0;    // a bin's linear power gain at most 6 dB over the span's largest band gain

constexpr double kPreHighPassHz = 40.0;

/** RBJ high-pass section. */
BiquadCoeffs highPass (double f, double q, double sampleRate) noexcept
{
    const double w = kTwoPi * f / sampleRate, cw = std::cos (w), alpha = std::sin (w) / (2.0 * q), a0 = 1.0 + alpha;
    BiquadCoeffs c;
    c.b0 = 0.5 * (1.0 + cw) / a0;
    c.b1 = -(1.0 + cw) / a0;
    c.b2 = c.b0;
    c.a1 = -2.0 * cw / a0;
    c.a2 = (1.0 - alpha) / a0;
    return c;
}

/** |H(e^{j w})|^2 of a normalised biquad. */
double powerResponse (const BiquadCoeffs& c, double w) noexcept
{
    const std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
    const auto h = (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
    return std::norm (h);
}

float ratioDb (double num, double den) noexcept
{
    if (! (num > 0.0) || ! (den > 0.0))
        return kMinusInfDb;
    return static_cast<float> (std::max (static_cast<double> (kMinusInfDb), 10.0 * std::log10 (num / den)));
}
} // namespace

float WeightedResidual::bandUpperHz (int band) noexcept
{
    return kBarkUpperHz[static_cast<size_t> (std::clamp (band, 0, kNumBands - 1))];
}

void WeightedResidual::prepare (double sampleRate)
{
    sr = sampleRate;
    size = 1;
    while (size < kFrameSeconds * sampleRate)
        size *= 2;
    fft.prepare (size);
    xRing.assign (static_cast<size_t> (size), 0.0f);
    yRing.assign (static_cast<size_t> (size), 0.0f);
    frame.assign (static_cast<size_t> (size), {});
    window.resize (static_cast<size_t> (size));
    for (int i = 0; i < size; ++i)
        window[static_cast<size_t> (i)] = static_cast<float> (0.5 - 0.5 * std::cos (kTwoPi * i / size));

    const int bins = size / 2 + 1;
    sxx.assign (static_cast<size_t> (bins), 0.0);
    syy.assign (static_cast<size_t> (bins), 0.0);
    sxyRe.assign (static_cast<size_t> (bins), 0.0);
    sxyIm.assign (static_cast<size_t> (bins), 0.0);
    spectrumX.assign (static_cast<size_t> (bins), {});
    spectrumY.assign (static_cast<size_t> (bins), {});
    weight.assign (static_cast<size_t> (bins), 0.0f);
    bandOfBin.assign (static_cast<size_t> (bins), -1);
    const double top = std::min (20000.0, 0.49 * sampleRate);
    firstBin = std::max (1, static_cast<int> (std::ceil (kLowestHz * size / sampleRate)));
    lastBin = std::min (bins - 2, static_cast<int> (std::floor (top * size / sampleRate)));
    // 8th-order Butterworth high-pass on both sides (see the header comment).
    constexpr std::array<double, 4> kButterworthQ { 0.5097956, 0.6013449, 0.8999762, 2.5629154 };
    for (size_t i = 0; i < preHighPass.size(); ++i)
        preHighPass[i].setCoeffs (highPass (kPreHighPassHz, kButterworthQ[i], sampleRate));
    const auto k1 = LoudnessMeter::kWeightingStage1 (sampleRate), k2 = LoudnessMeter::kWeightingStage2 (sampleRate);
    for (int k = firstBin; k <= lastBin; ++k)
    {
        const double f = k * sampleRate / size, w = kTwoPi * k / size;
        int b = 0;
        while (b < kNumBands - 1 && f >= kBarkUpperHz[static_cast<size_t> (b)])
            ++b;
        bandOfBin[static_cast<size_t> (k)] = b;
        weight[static_cast<size_t> (k)] = static_cast<float> (powerResponse (k1, w) * powerResponse (k2, w));
    }
    reset();
}

void WeightedResidual::reset() noexcept FLUB_NONBLOCKING
{
    for (auto& section : preHighPass)
        section.reset();
    std::fill (xRing.begin(), xRing.end(), 0.0f);
    std::fill (yRing.begin(), yRing.end(), 0.0f);
    std::fill (sxx.begin(), sxx.end(), 0.0);
    std::fill (syy.begin(), syy.end(), 0.0);
    std::fill (sxyRe.begin(), sxyRe.end(), 0.0);
    std::fill (sxyIm.begin(), sxyIm.end(), 0.0);
    writePos = 0;
    filled = frames = 0;
    rng = 0x2545f491u;
    untilFrame = size; // the first frame is a full one
    alpha = 1.0f;
    residualDb = weightedDb = plainResidualDb = kMinusInfDb;
    bandResidualDb.fill (kMinusInfDb);
    bandOutputDb.fill (kMinusInfDb);
}

int WeightedResidual::nextHop() noexcept FLUB_NONBLOCKING
{
    // xorshift32: a fixed sequence from reset(), so readings do not depend on
    // how the host splits the audio.
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    const int quarter = size / 4;
    return quarter + static_cast<int> (rng % static_cast<std::uint32_t> (quarter + 1));
}

void WeightedResidual::process (const float* reference, const float* output, int n) noexcept FLUB_NONBLOCKING
{
    for (int i = 0; i < n;)
    {
        const int run = std::min (n - i, untilFrame);
        for (int j = 0; j < run; ++j)
        {
            double x = reference[i + j], y = output[i + j];
            for (auto& section : preHighPass)
            {
                x = section.processSample (0, x);
                y = section.processSample (1, y);
            }
            xRing[static_cast<size_t> (writePos)] = static_cast<float> (x);
            yRing[static_cast<size_t> (writePos)] = static_cast<float> (y);
            if (++writePos == size)
                writePos = 0;
        }
        i += run;
        untilFrame -= run;
        if (untilFrame == 0)
        {
            analyse();
            const int hop = nextHop();
            untilFrame = hop;
            alpha = static_cast<float> (1.0 - std::exp (-hop / (kAverageSeconds * sr)));
        }
    }
}

void WeightedResidual::analyse() noexcept FLUB_NONBLOCKING
{
    // x + j y through one complex FFT; the oldest sample is at writePos.
    for (int i = 0, p = writePos; i < size; ++i)
    {
        const float w = window[static_cast<size_t> (i)];
        frame[static_cast<size_t> (i)] = { w * xRing[static_cast<size_t> (p)], w * yRing[static_cast<size_t> (p)] };
        if (++p == size)
            p = 0;
    }
    fft.forward (frame.data());

    // Split the packed transform into the reference's and the output's bins.
    for (int k = firstBin; k <= lastBin; ++k)
    {
        const auto zk = std::complex<double> (frame[static_cast<size_t> (k)]);
        const auto zn = std::conj (std::complex<double> (frame[static_cast<size_t> (size - k)]));
        spectrumX[static_cast<size_t> (k)] = 0.5 * (zk + zn);
        spectrumY[static_cast<size_t> (k)] = std::complex<double> (0.0, -0.5) * (zk - zn);
    }
    // Slow gain riding (a limiter's programme envelope, a governor, a bass
    // protection shelf) is not distortion, but the averaged fit below would
    // read it as incoherence (a 3 dB/s ramp read -19 dB). Each band of the
    // frame is therefore first brought back to the running response: g_b =
    // the least-squares real gain of Y against H X (H = Sxy / Sxx so far),
    // g_b within +-12 dB; 1 while the band's predicted output is under half
    // its output (mostly content the reference does not have: its fit is
    // not a gain) or before the averages hold kMinFrames frames.
    std::array<double, kNumBands> num {}, den {}, out {};
    if (frames >= kMinFrames)
        for (int k = firstBin; k <= lastBin; ++k)
        {
            const auto kk = static_cast<size_t> (k);
            if (! (sxx[kk] > 0.0))
                continue;
            const std::complex<double> h (sxyRe[kk] / sxx[kk], sxyIm[kk] / sxx[kk]);
            const auto p = h * spectrumX[kk];
            const auto b = static_cast<size_t> (bandOfBin[kk]);
            num[b] += (spectrumY[kk] * std::conj (p)).real();
            den[b] += std::norm (p);
            out[b] += std::norm (spectrumY[kk]);
        }
    std::array<double, kNumBands> inverseGain {};
    for (size_t b = 0; b < static_cast<size_t> (kNumBands); ++b)
    {
        const double g = den[b] > kRidingShare * out[b] && den[b] > 0.0 ? num[b] / den[b] : 1.0;
        inverseGain[b] = g > 0.25 && g < 4.0 ? 1.0 / g : 1.0;
    }

    const double a = alpha, keep = 1.0 - a;
    for (int k = firstBin; k <= lastBin; ++k)
    {
        const auto kk = static_cast<size_t> (k);
        const auto xk = spectrumX[kk];
        const auto yk = inverseGain[static_cast<size_t> (bandOfBin[kk])] * spectrumY[kk];
        const auto xy = yk * std::conj (xk);
        sxx[kk] = keep * sxx[kk] + a * std::norm (xk);
        syy[kk] = keep * syy[kk] + a * std::norm (yk);
        sxyRe[kk] = keep * sxyRe[kk] + a * xy.real();
        sxyIm[kk] = keep * sxyIm[kk] + a * xy.imag();
    }
    ++frames;
    if (frames < kMinFrames)
        return;

    double meanXx = 0.0;
    std::array<double, kNumBands> bandXx {}, bandXyRe {}, bandXyIm {};
    for (int k = firstBin; k <= lastBin; ++k)
    {
        const auto kk = static_cast<size_t> (k);
        const auto b = static_cast<size_t> (bandOfBin[kk]);
        meanXx += sxx[kk];
        bandXx[b] += sxx[kk];
        bandXyRe[b] += sxyRe[kk];
        bandXyIm[b] += sxyIm[kk];
    }
    meanXx /= std::max (1, lastBin - firstBin + 1);
    const double floorXx = kReferenceFloor * meanXx;
    // The largest linear gain a bin may claim: kMaxGainOverSpan over the
    // largest band gain of the bands where the reference has programme
    // (within 30 dB of its strongest band). Without it, a harmonic the span
    // adds on top of a faint copy already in its reference, phase-locked to
    // the same fundamental (bass harmonics ahead of a saturator), fits as a
    // linear gain of that copy and is not counted.
    double strongest = 0.0, maxGain = 0.0;
    for (size_t b = 0; b < static_cast<size_t> (kNumBands); ++b)
        strongest = std::max (strongest, bandXx[b]);
    for (size_t b = 0; b < static_cast<size_t> (kNumBands); ++b)
        if (bandXx[b] > 1.0e-3 * strongest && bandXx[b] > 0.0)
            maxGain = std::max (maxGain, (bandXyRe[b] * bandXyRe[b] + bandXyIm[b] * bandXyIm[b]) / (bandXx[b] * bandXx[b]));
    const double gainCap = kMaxGainOverSpan * maxGain;

    std::array<double, kNumBands> nb {}, cb {}, yb {}, logSum {}, flatSum {};
    std::array<int, kNumBands> bins {};
    double plainN = 0.0, plainY = 0.0;
    for (int k = firstBin; k <= lastBin; ++k)
    {
        const auto kk = static_cast<size_t> (k);
        const double w = weight[kk], yy = syy[kk];
        double incoherent = yy;
        if (sxx[kk] > floorXx && sxx[kk] > 0.0)
        {
            const double coherent = std::min ((sxyRe[kk] * sxyRe[kk] + sxyIm[kk] * sxyIm[kk]) / sxx[kk], gainCap * sxx[kk]);
            incoherent = std::clamp (yy - coherent, 0.0, yy);
        }
        const auto b = static_cast<size_t> (bandOfBin[kk]);
        plainN += incoherent;
        plainY += yy;
        // Tonality of the band's coherent programme (the masker): spectral
        // flatness of its averaged power spectrum.
        const double c = std::max (yy - incoherent, 1.0e-30);
        logSum[b] += std::log (c);
        flatSum[b] += c;
        ++bins[b];
        nb[b] += w * incoherent;
        cb[b] += w * (yy - incoherent);
        yb[b] += w * yy;
    }
    double nSum = 0.0, aSum = 0.0, ySum = 0.0;
    for (size_t b = 0; b < static_cast<size_t> (kNumBands); ++b)
    {
        // Johnston's offset: tonality alpha = min (1, SFM_dB / -60) from the
        // flatness (a band of one or two bins counts as tonal).
        double tonality = 1.0;
        if (bins[b] > 2 && flatSum[b] > 0.0)
        {
            const double sfmDb = 10.0 / std::log (10.0) * (logSum[b] / bins[b] - std::log (flatSum[b] / bins[b]));
            tonality = std::clamp (sfmDb / -60.0, 0.0, 1.0);
        }
        const double offsetDb = tonality * (kTonalOffsetDb + static_cast<double> (b)) + (1.0 - tonality) * kNoiseOffsetDb;
        const double t = std::pow (10.0, -0.1 * offsetDb) * cb[b];
        nSum += nb[b];
        aSum += nb[b] > 0.0 ? nb[b] * nb[b] / (nb[b] + t) : 0.0;
        ySum += yb[b];
        bandResidualDb[b] = ratioDb (nb[b], 1.0);
        bandOutputDb[b] = ratioDb (yb[b], 1.0);
    }
    plainResidualDb = ratioDb (plainN, plainY);
    residualDb = ratioDb (nSum, ySum);
    weightedDb = ratioDb (aSum, ySum);
}
} // namespace flub
