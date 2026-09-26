// Flubsound Pro - Linkwitz-Riley 4th-order (24 dB/oct) crossovers.
//
// LR4 = two cascaded 2nd-order Butterworth sections (Q = 1/sqrt 2):
//   LP4 = 1/(s^2+sqrt2 s+1)^2,  HP4 = s^4/(s^2+sqrt2 s+1)^2
//   LP4 + HP4 = (s^2 - sqrt2 s + 1)/(s^2 + sqrt2 s + 1)   (2nd-order all-pass)
// so the bands sum to a flat magnitude response. Multi-band splitters add an
// all-pass with the same phase to the lower band so every band sums flat.
#pragma once

#include "Svf.h"

#include <array>

namespace flub
{
class LinkwitzRiley4
{
public:
    void prepare (double sampleRate) noexcept
    {
        sr = sampleRate;
        setFrequency (freq);
        reset();
    }

    /** RT-safe; recomputes coefficients. Prefer rare changes (crossover
        points are not modulated in Flubsound). */
    void setFrequency (double hz) noexcept
    {
        freq = hz;
        coeffs = SvfCoeffs::make (FilterType::LowPass, hz, 1.0 / std::sqrt (2.0), 0.0, sr);
    }

    double getFrequency() const noexcept { return freq; }

    void reset() noexcept
    {
        for (auto& s : st)
            s = {};
    }

    void processSample (int ch, float x, float& low, float& high) noexcept
    {
        auto& s = st[static_cast<size_t> (ch)];
        float v1, v2;
        svfTickRaw (coeffs, s.split, x, v1, v2);
        const float lp1 = v2;
        const float hp1 = x - static_cast<float> (coeffs.k) * v1 - v2;
        svfTickRaw (coeffs, s.low2, lp1, v1, v2);
        low = v2;
        svfTickRaw (coeffs, s.high2, hp1, v1, v2);
        high = hp1 - static_cast<float> (coeffs.k) * v1 - v2;
    }

private:
    struct ChannelState
    {
        SvfState split, low2, high2;
    };

    double sr = 48000.0, freq = 120.0;
    SvfCoeffs coeffs;
    std::array<ChannelState, kMaxChannels> st {};
};

/** The all-pass that matches an LR4 crossover's phase at the same frequency. */
class LinkwitzRileyAllPass
{
public:
    void prepare (double sampleRate, double hz) noexcept
    {
        coeffs = SvfCoeffs::make (FilterType::AllPass, hz, 1.0 / std::sqrt (2.0), 0.0, sampleRate);
        reset();
    }

    void reset() noexcept
    {
        for (auto& s : st)
            s.reset();
    }

    float processSample (int ch, float x) noexcept { return svfTick (coeffs, st[static_cast<size_t> (ch)], x); }

private:
    SvfCoeffs coeffs;
    std::array<SvfState, kMaxChannels> st {};
};

/** Three phase-coherent bands (low | mid | high) that sum to an all-pass. */
class ThreeBandSplitter
{
public:
    void prepare (double sampleRate, double lowMidHz, double midHighHz) noexcept
    {
        lowSplit.prepare (sampleRate);
        lowSplit.setFrequency (lowMidHz);
        highSplit.prepare (sampleRate);
        highSplit.setFrequency (midHighHz);
        lowAlign.prepare (sampleRate, midHighHz);
    }

    void reset() noexcept
    {
        lowSplit.reset();
        highSplit.reset();
        lowAlign.reset();
    }

    void processSample (int ch, float x, float& low, float& mid, float& high) noexcept
    {
        float rest;
        lowSplit.processSample (ch, x, low, rest);
        highSplit.processSample (ch, rest, mid, high);
        low = lowAlign.processSample (ch, low);
    }

private:
    LinkwitzRiley4 lowSplit, highSplit;
    LinkwitzRileyAllPass lowAlign;
};
} // namespace flub
