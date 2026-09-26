#include "flub/dsp/Oversampler.h"

#include "flub/dsp/FirDesign.h"

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

void Oversampler::prepare (int numChannels, int maxBlockSize, int newFactor, Quality quality)
{
    assert (newFactor == 1 || newFactor == 2 || newFactor == 4);
    factor = newFactor;
    channels = numChannels;
    maxBlock = maxBlockSize;

    const bool hq = quality == Quality::High;
    const int d1 = hq ? 16 : 8;
    const int d2 = hq ? 4 : 3;
    stage1.prepare (numChannels, d1, hq ? 9.0 : 4.5);
    stage2.prepare (numChannels, d2, hq ? 8.0 : 6.0);

    latency = factor == 1 ? 0 : (factor == 2 ? stage1.roundTripLatency() : stage1.roundTripLatency() + stage2.roundTripLatency() / 2);

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
        stage1.downsample (c, midPtrs[static_cast<size_t> (c)], out.channel (c), n);
}
} // namespace flub
