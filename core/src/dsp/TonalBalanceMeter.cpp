#include "flub/dsp/TonalBalanceMeter.h"

#include <algorithm>
#include <cmath>

namespace flub
{
void TonalBalanceMeter::prepare (double sampleRate)
{
    sr = sampleRate;
    for (int b = 0; b < kNumBands; ++b)
    {
        const auto i = static_cast<size_t> (b);
        measured[i] = kHighHz[i] <= 0.45 * sr;
        for (int s = 0; s < 2; ++s)
        {
            highPass[i][static_cast<size_t> (s)] = SvfCoeffs::make (FilterType::HighPass, kLowHz[i], butterworthQ (2, s), 0.0, sr);
            lowPass[i][static_cast<size_t> (s)] = SvfCoeffs::make (FilterType::LowPass, std::min (kHighHz[i], 0.45 * sr), butterworthQ (2, s), 0.0, sr);
        }
    }
    minSamples = static_cast<std::int64_t> (kMinSeconds * sr);
    reset();
}

void TonalBalanceMeter::reset() noexcept FLUB_NONBLOCKING
{
    for (Side* side : { &reference, &output })
    {
        for (auto& bandStates : side->states)
            for (auto& channel : bandStates)
                for (auto& s : channel)
                    s.reset();
        side->window.fill (0.0);
        side->average.fill (0.0);
    }
    windowSamples = averagedSamples = 0;
}

void TonalBalanceMeter::accumulate (const AudioBlock& block, Side& side) noexcept FLUB_NONBLOCKING
{
    const int nch = std::min (block.numChannels, 2);
    for (int b = 0; b < kNumBands; ++b)
    {
        const auto bi = static_cast<size_t> (b);
        if (! measured[bi])
            continue;
        const auto& hp = highPass[bi];
        const auto& lp = lowPass[bi];
        double sum = 0.0;
        for (int c = 0; c < nch; ++c)
        {
            auto& st = side.states[bi][static_cast<size_t> (c)];
            const float* x = block.channel (c);
            for (int i = 0; i < block.numSamples; ++i)
            {
                const float h = svfTick (hp[1], st[1], svfTick (hp[0], st[0], x[i]));
                const float y = svfTick (lp[1], st[3], svfTick (lp[0], st[2], h));
                sum += static_cast<double> (y) * y;
            }
        }
        side.window[bi] += sum;
    }
}

void TonalBalanceMeter::tick() noexcept FLUB_NONBLOCKING
{
    const std::int64_t n = windowSamples;
    windowSamples = 0;
    if (n <= 0)
        return;
    const double mean = reference.window[static_cast<size_t> (Mids)] / static_cast<double> (n);
    if (mean > kSilencePower)
    {
        // A one-pole in samples: a window of n samples moves it by 1 - a^n.
        const double keep = std::exp (-static_cast<double> (n) / (kAverageSeconds * sr));
        for (Side* side : { &reference, &output })
            for (int b = 0; b < kNumBands; ++b)
            {
                const auto bi = static_cast<size_t> (b);
                side->average[bi] = keep * side->average[bi] + (1.0 - keep) * side->window[bi] / static_cast<double> (n);
            }
        averagedSamples += n;
    }
    reference.window.fill (0.0);
    output.window.fill (0.0);
}

float TonalBalanceMeter::getLiftDb (int band) const noexcept FLUB_NONBLOCKING
{
    if (band <= Mids || band >= kNumBands || ! hasReading())
        return kNoReading;
    const auto bi = static_cast<size_t> (band);
    const double refMids = reference.average[static_cast<size_t> (Mids)], outMids = output.average[static_cast<size_t> (Mids)];
    const double refBand = reference.average[bi], outBand = output.average[bi];
    const double range = std::pow (10.0, -0.1 * kBandRangeDb);
    if (! measured[bi] || ! (refMids > 0.0) || ! (outMids > 0.0) || refBand < range * refMids || ! (outBand > 0.0))
        return kNoReading;
    return static_cast<float> (10.0 * std::log10 ((outBand / refBand) / (outMids / refMids)));
}
} // namespace flub
