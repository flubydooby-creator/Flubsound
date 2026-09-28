#include "flub/dsp/Oversampler.h"

#include "flub/dsp/FirDesign.h"

#include <algorithm>
#include <cassert>

namespace flub
{
void HalfbandStage::prepare (int numChannels, int halfOrder, double kaiserBeta)
{
    d = halfOrder;
    const int length = 4 * d + 1;
    const int centre = 2 * d;

    // Half-band prototype: cutoff at a quarter of the higher rate. Taps at even
    // offsets from the centre are exactly zero; the centre tap is exactly 0.5.
    oddTaps.assign (static_cast<size_t> (2 * d), 0.0f);
    double oddSum = 0.0;
    std::vector<double> odd (static_cast<size_t> (2 * d));
    for (int j = 0; j < 2 * d; ++j)
    {
        const int n = 2 * j + 1;
        const double v = 0.5 * fir::sinc (0.5 * (n - centre)) * fir::kaiser (n, length, kaiserBeta);
        odd[static_cast<size_t> (j)] = v;
        oddSum += v;
    }
    // Normalise so centre (0.5) + odd taps (0.5) = unity DC gain.
    for (int j = 0; j < 2 * d; ++j)
        oddTaps[static_cast<size_t> (j)] = static_cast<float> (odd[static_cast<size_t> (j)] * (0.5 / oddSum));

    for (int c = 0; c < kMaxChannels; ++c)
    {
        const bool used = c < numChannels;
        upHist[static_cast<size_t> (c)].prepare (used ? 2 * d : 1);
        downEven[static_cast<size_t> (c)].prepare (used ? d + 1 : 1);
        downOdd[static_cast<size_t> (c)].prepare (used ? 2 * d : 1);
    }
}

void HalfbandStage::reset() noexcept
{
    for (auto& h : upHist)
        h.reset();
    for (auto& h : downEven)
        h.reset();
    for (auto& h : downOdd)
        h.reset();
}

void HalfbandStage::upsample (int ch, const float* in, float* out, int n) noexcept
{
    auto& hist = upHist[static_cast<size_t> (ch)];
    const float* taps = oddTaps.data();
    const int numTaps = 2 * d;
    for (int i = 0; i < n; ++i)
    {
        hist.push (in[i]);
        const float* w = hist.window();
        float acc = 0.0f;
        for (int j = 0; j < numTaps; ++j)
            acc += taps[j] * w[j];
        out[2 * i] = w[d];            // phase 0: pure delay (centre tap * 2 = 1)
        out[2 * i + 1] = 2.0f * acc;  // phase 1: interpolated
    }
}

void HalfbandStage::downsample (int ch, const float* in, float* out, int n) noexcept
{
    auto& even = downEven[static_cast<size_t> (ch)];
    auto& odd = downOdd[static_cast<size_t> (ch)];
    const float* taps = oddTaps.data();
    const int numTaps = 2 * d;
    for (int i = 0; i < n; ++i)
    {
        even.push (in[2 * i]);
        const float* wo = odd.window();
        float acc = 0.0f;
        for (int j = 0; j < numTaps; ++j)
            acc += taps[j] * wo[j];
        out[i] = 0.5f * even.window()[d] + acc;
        odd.push (in[2 * i + 1]);
    }
}

void DecimatorStage::prepare (int numChannels, int newM, double cutoff, double kaiserBeta)
{
    m = std::max (1, newM);
    const int length = 4 * m + 1;
    const int centre = 2 * m;
    std::vector<double> h (static_cast<size_t> (length));
    double sum = 0.0;
    for (int n = 0; n < length; ++n)
    {
        const double v = 2.0 * cutoff * fir::sinc (2.0 * cutoff * (n - centre)) * fir::kaiser (n, length, kaiserBeta);
        h[static_cast<size_t> (n)] = v;
        sum += v;
    }
    taps.resize (static_cast<size_t> (length));
    for (int n = 0; n < length; ++n)
        taps[static_cast<size_t> (n)] = static_cast<float> (h[static_cast<size_t> (n)] / sum); // unity DC gain
    for (int c = 0; c < kMaxChannels; ++c)
        hist[static_cast<size_t> (c)].prepare (c < numChannels ? length : 1);
}

void DecimatorStage::reset() noexcept
{
    for (auto& h : hist)
        h.reset();
}

void DecimatorStage::downsample (int ch, const float* in, float* out, int n) noexcept
{
    auto& h = hist[static_cast<size_t> (ch)];
    const float* t = taps.data();
    const int length = 4 * m + 1;
    for (int i = 0; i < n; ++i)
    {
        // Centred on in[2i - 2m] (the newest sample in the window is in[2i]),
        // so the delay is exactly m base-rate samples, like a half-band stage.
        h.push (in[2 * i]);
        const float* w = h.window();
        float acc = 0.0f;
        for (int k = 0; k < length; ++k)
            acc += t[k] * w[k];
        out[i] = acc;
        h.push (in[2 * i + 1]);
    }
}

Oversampler::Design Oversampler::design (int factor, Quality quality) noexcept
{
    const bool hq = quality == Quality::High;
    return { factor, hq ? 16 : 8, hq ? 4 : 3, hq ? 9.0 : 4.5, hq ? 8.0 : 6.0 };
}

Oversampler::Design Oversampler::forProfile (Profile profile, double sampleRate) noexcept
{
    const bool quality = profile == Profile::Quality;
    // 176.4 kHz and up: harmonics that fold at 2x are far above the audible
    // band, so the fixed 2x designs stay (4x would cost four times the 48 kHz CPU).
    if (sampleRate >= 176400.0)
        return design (2, quality ? Quality::High : Quality::Low);
    // Below: 4x with short half-bands and a stage-1 decimator whose transition
    // ends near the image of 20 kHz (0.23 / 0.19 of the 2x rate is the -6 dB
    // point), in the latency the 2x designs had: 8 + 18 + 6 = 32 and
    // 4 + 9 + 3 = 16 base-rate samples.
    if (quality)
        return { 4, 8, 6, 8.0, 9.0, 18, 0.23, 9.0 };
    return { 4, 4, 3, 6.0, 7.0, 9, 0.19, 7.0 };
}

void Oversampler::prepare (int numChannels, int maxBlockSize, int newFactor, Quality quality)
{
    prepare (numChannels, maxBlockSize, design (newFactor, quality));
}

void Oversampler::prepare (int numChannels, int maxBlockSize, const Design& d)
{
    assert (d.factor == 1 || d.factor == 2 || d.factor == 4);
    assert (d.d1 >= 1 && d.d2 >= 1);
    factor = d.factor;
    channels = numChannels;
    maxBlock = maxBlockSize;

    stage1.prepare (numChannels, std::max (1, d.d1), d.beta1);
    stage2.prepare (numChannels, std::max (1, d.d2), d.beta2);
    useDecimator = d.m1 > 0 && factor > 1;
    if (useDecimator)
        down1.prepare (numChannels, d.m1, d.cutoff1, d.betaDown1);

    // Stage 1: d1 base samples up + d1 (half-band) or m1 (decimator) down.
    const int stage1Latency = std::max (1, d.d1) + (useDecimator ? down1.delay() : std::max (1, d.d1));
    latency = factor == 1 ? 0 : (factor == 2 ? stage1Latency : stage1Latency + stage2.roundTripLatency() / 2);

    midStride = 2 * maxBlockSize;
    highStride = 4 * maxBlockSize;
    mid.assign (static_cast<size_t> (numChannels * midStride), 0.0f);
    high.assign (static_cast<size_t> (factor == 4 ? numChannels * highStride : 0), 0.0f);
    for (int c = 0; c < kMaxChannels; ++c)
    {
        midPtrs[static_cast<size_t> (c)] = c < numChannels ? mid.data() + c * midStride : nullptr;
        highPtrs[static_cast<size_t> (c)] = (c < numChannels && factor == 4) ? high.data() + c * highStride : nullptr;
    }
}

void Oversampler::reset() noexcept
{
    stage1.reset();
    stage2.reset();
    down1.reset();
}

AudioBlock Oversampler::upsample (const AudioBlock& in) noexcept
{
    lastNumSamples = in.numSamples;
    if (factor == 1)
        return in;

    const int nch = std::min (in.numChannels, channels);
    const int n = std::min (in.numSamples, maxBlock);
    for (int c = 0; c < nch; ++c)
        stage1.upsample (c, in.channel (c), midPtrs[static_cast<size_t> (c)], n);

    if (factor == 2)
        return AudioBlock (midPtrs.data(), nch, 2 * n);

    for (int c = 0; c < nch; ++c)
        stage2.upsample (c, midPtrs[static_cast<size_t> (c)], highPtrs[static_cast<size_t> (c)], 2 * n);
    return AudioBlock (highPtrs.data(), nch, 4 * n);
}

void Oversampler::downsample (const AudioBlock& out) noexcept
{
    if (factor == 1)
        return;

    const int nch = std::min (out.numChannels, channels);
    const int n = std::min (out.numSamples, maxBlock);
    if (factor == 4)
        for (int c = 0; c < nch; ++c)
            stage2.downsample (c, highPtrs[static_cast<size_t> (c)], midPtrs[static_cast<size_t> (c)], 2 * n);

    for (int c = 0; c < nch; ++c)
    {
        if (useDecimator)
            down1.downsample (c, midPtrs[static_cast<size_t> (c)], out.channel (c), n);
        else
            stage1.downsample (c, midPtrs[static_cast<size_t> (c)], out.channel (c), n);
    }
}
} // namespace flub
