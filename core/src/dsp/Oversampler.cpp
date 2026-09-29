#include "flub/dsp/Oversampler.h"

#include "flub/dsp/FirDesign.h"

#include <algorithm>
#include <cassert>

namespace flub
{
namespace
{
/** sum a[k] b[k] over k < n in four interleaved partial sums: without
    -ffast-math a single running sum is one serial add chain the compiler
    may not reorder, which made these FIRs most of the saturator's cost at
    8x (docs/11 E10 Phase 2); four chains vectorise. */
inline float dot (const float* a, const float* b, int n) noexcept
{
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    int k = 0;
    for (; k + 4 <= n; k += 4)
    {
        s0 += a[k] * b[k];
        s1 += a[k + 1] * b[k + 1];
        s2 += a[k + 2] * b[k + 2];
        s3 += a[k + 3] * b[k + 3];
    }
    for (; k < n; ++k)
        s0 += a[k] * b[k];
    return (s0 + s1) + (s2 + s3);
}
} // namespace

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
        out[2 * i] = w[d];                             // phase 0: pure delay (centre tap * 2 = 1)
        out[2 * i + 1] = 2.0f * dot (taps, w, numTaps); // phase 1: interpolated
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
        out[i] = 0.5f * even.window()[d] + dot (taps, odd.window(), numTaps);
        odd.push (in[2 * i + 1]);
    }
}

void DecimatorStage::prepare (int numChannels, int newM, double cutoff, double kaiserBeta, double advance)
{
    m = std::max (1, newM);
    const int length = 4 * m + 1;
    // Centred `advance` higher-rate samples early (docs/11 E10 Phase 2: the
    // ADAA curve's half-sample delay); the window moves with the sinc and
    // keeps its half-width 2m, so its last tap falls just outside (weight 0).
    const double centre = 2 * m - std::clamp (advance, 0.0, 1.0);
    const double i0Beta = fir::besselI0 (kaiserBeta);
    std::vector<double> h (static_cast<size_t> (length));
    double sum = 0.0;
    for (int n = 0; n < length; ++n)
    {
        const double r = (n - centre) / (2.0 * m);
        const double window = fir::besselI0 (kaiserBeta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0Beta;
        const double v = 2.0 * cutoff * fir::sinc (2.0 * cutoff * (n - centre)) * (std::abs (r) <= 1.0 ? window : 0.0);
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
        out[i] = dot (t, h.window(), length);
        h.push (in[2 * i + 1]);
    }
}

Oversampler::Design Oversampler::design (int factor, Quality quality) noexcept
{
    const bool hq = quality == Quality::High;
    Design d;
    d.factor = factor;
    d.d1 = hq ? 16 : 8;
    d.d2 = hq ? 4 : 3;
    d.beta1 = hq ? 9.0 : 4.5;
    d.beta2 = hq ? 8.0 : 6.0;
    return d;
}

Oversampler::Design Oversampler::forProfile (Profile profile, double sampleRate) noexcept
{
    const bool quality = profile == Profile::Quality;
    // Every row runs its curve with ADAA (docs/11 E10 Phase 2), so stage 1
    // always downsamples through a DecimatorStage, which takes the ADAA's
    // half sample back. Latency as the fixed 2x designs had at every rate:
    // Quality 32, Balanced / Low Latency 16 base-rate samples.
    Design d;
    d.adaa = true;
    if (sampleRate >= 176400.0)
    {
        // 2x: harmonics that fold at 2x come from far above the audible band;
        // the fixed designs' half-bands with a decimator of the same length
        // (quality: 16 + 16; else 8 + 8 at a 0.22 cutoff, below the half-band's
        // 0.25, whose transition would otherwise reach the image of 20 kHz).
        d.factor = 2;
        d.d1 = quality ? 16 : 8;
        d.d2 = quality ? 4 : 3;
        d.beta1 = quality ? 9.0 : 6.0;
        d.beta2 = quality ? 8.0 : 6.0;
        d.m1 = d.d1;
        d.cutoff1 = quality ? 0.25 : 0.22;
        d.betaDown1 = quality ? 9.0 : 7.0;
        return d;
    }
    // Below: short half-bands and a stage-1 decimator whose transition ends
    // near the image of 20 kHz (0.23 / 0.19 of the 2x rate is the -6 dB
    // point): 4x, 8 + 18 + 6 = 32 and 4 + 9 + 3 = 16 samples. Quality up to
    // 48 kHz runs 8x, the only way to keep 24 dB of drive below -70 dBc at
    // 44.1 kHz (4x with ADAA: -54 dBc on Tape), at twice the curve's CPU: the
    // third half-band (9 taps) takes its one sample from stage 2 (8 + 18 +
    // 5 + 1).
    const bool eight = quality && sampleRate < 88200.0;
    d.factor = eight ? 8 : 4;
    d.d1 = quality ? 8 : 4;
    d.d2 = quality ? (eight ? 5 : 6) : 3;
    d.beta1 = quality ? 8.0 : 6.0;
    d.beta2 = quality ? 9.0 : 7.0;
    d.m1 = quality ? 18 : 9;
    d.cutoff1 = quality ? 0.23 : 0.19;
    d.betaDown1 = quality ? 9.0 : 7.0;
    d.d3 = 2;
    d.beta3 = 5.0;
    return d;
}

Oversampler::Design Oversampler::forClipper (Profile profile, double sampleRate) noexcept
{
    // A hard clip's harmonics fall only 6 dB per octave. At 2x Low (Low
    // Latency's old design) those above the 2x Nyquist fold straight back
    // into the audible band: 24 dB of drive with the crest gate off and the
    // depth uncapped read -19.0 / -22.5 dBc at 44.1 / 48 kHz, 12 dB -34.0 /
    // -47.1 dBc. The 16-sample 4x design gets -42.0 / -44.0 and -57.3 /
    // -64.6 dBc, as Balanced's 4x High (36 samples). From 88.2 kHz 2x Low
    // keeps 12 dB below -60 dBc and stays (4x there costs 46 % more).
    // Quality and Balanced keep 4x High: 8x in the same 36 samples reaches
    // -52.7 / -82.8 dBc at 44.1 kHz, for 60 % more of the maximizer's CPU,
    // and the default crest gate keeps a steady tone out of the clipper
    // anyway. The clipper's curve has no closed-form antiderivative with
    // its depth cap, so there is no ADAA here.
    if (profile != Profile::LowLatency)
        return design (4, Quality::High);
    if (sampleRate >= 88200.0)
        return design (2, Quality::Low);
    Design d = forProfile (Profile::LowLatency, sampleRate);
    d.adaa = false;
    return d;
}

void Oversampler::prepare (int numChannels, int maxBlockSize, int newFactor, Quality quality)
{
    prepare (numChannels, maxBlockSize, design (newFactor, quality));
}

void Oversampler::prepare (int numChannels, int maxBlockSize, const Design& d)
{
    assert (d.factor == 1 || d.factor == 2 || d.factor == 4 || d.factor == 8);
    assert (d.d1 >= 1 && d.d2 >= 1);
    assert (d.factor != 8 || (d.d3 >= 2 && d.d3 % 2 == 0));
    assert (! d.adaa || d.m1 > 0);
    factor = d.factor;
    channels = numChannels;
    maxBlock = maxBlockSize;

    stage1.prepare (numChannels, std::max (1, d.d1), d.beta1);
    stage2.prepare (numChannels, std::max (1, d.d2), d.beta2);
    // d3 even: the 4x <-> 8x round trip (2 d3 samples at 4x) is d3 / 2 base samples.
    const int d3 = std::max (2, d.d3 + d.d3 % 2);
    stage3.prepare (numChannels, factor == 8 ? d3 : 1, d.beta3);
    useDecimator = d.m1 > 0 && factor > 1;
    if (useDecimator)
        down1.prepare (numChannels, d.m1, d.cutoff1, d.betaDown1, d.adaa ? 1.0 / factor : 0.0);

    // Stage 1: d1 base samples up + d1 (half-band) or m1 (decimator) down.
    const int stage1Latency = std::max (1, d.d1) + (useDecimator ? down1.delay() : std::max (1, d.d1));
    latency = factor == 1 ? 0 : (factor == 2 ? stage1Latency : stage1Latency + stage2.roundTripLatency() / 2);
    if (factor == 8)
        latency += d3 / 2;

    midStride = 2 * maxBlockSize;
    highStride = 4 * maxBlockSize;
    topStride = 8 * maxBlockSize;
    mid.assign (static_cast<size_t> (numChannels * midStride), 0.0f);
    high.assign (static_cast<size_t> (factor >= 4 ? numChannels * highStride : 0), 0.0f);
    top.assign (static_cast<size_t> (factor == 8 ? numChannels * topStride : 0), 0.0f);
    for (int c = 0; c < kMaxChannels; ++c)
    {
        midPtrs[static_cast<size_t> (c)] = c < numChannels ? mid.data() + c * midStride : nullptr;
        highPtrs[static_cast<size_t> (c)] = (c < numChannels && factor >= 4) ? high.data() + c * highStride : nullptr;
        topPtrs[static_cast<size_t> (c)] = (c < numChannels && factor == 8) ? top.data() + c * topStride : nullptr;
    }
}

void Oversampler::reset() noexcept
{
    stage1.reset();
    stage2.reset();
    stage3.reset();
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
    if (factor == 4)
        return AudioBlock (highPtrs.data(), nch, 4 * n);

    for (int c = 0; c < nch; ++c)
        stage3.upsample (c, highPtrs[static_cast<size_t> (c)], topPtrs[static_cast<size_t> (c)], 4 * n);
    return AudioBlock (topPtrs.data(), nch, 8 * n);
}

void Oversampler::downsample (const AudioBlock& out) noexcept
{
    if (factor == 1)
        return;

    const int nch = std::min (out.numChannels, channels);
    const int n = std::min (out.numSamples, maxBlock);
    if (factor == 8)
        for (int c = 0; c < nch; ++c)
            stage3.downsample (c, topPtrs[static_cast<size_t> (c)], highPtrs[static_cast<size_t> (c)], 4 * n);
    if (factor >= 4)
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
