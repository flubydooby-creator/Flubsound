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
#include <cmath>

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

    /** See ThreeBandSplitter::flushStates. */
    float flushStates (int numChannels, float tiny) noexcept
    {
        float sum = 0.0f;
        for (int c = 0; c < numChannels && c < kMaxChannels; ++c)
        {
            auto& s = st[static_cast<size_t> (c)];
            sum += flushSvf (s.split, tiny) + flushSvf (s.low2, tiny) + flushSvf (s.high2, tiny);
        }
        return sum;
    }

    /** Zeroes a state whose integrators are both below `tiny`. */
    static float flushSvf (SvfState& s, float tiny) noexcept
    {
        if (std::abs (s.ic1) < tiny && std::abs (s.ic2) < tiny)
            s.ic1 = s.ic2 = 0.0f;
        return s.ic1 + s.ic2;
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

    /** See ThreeBandSplitter::flushStates. */
    float flushStates (int numChannels, float tiny) noexcept
    {
        float sum = 0.0f;
        for (int c = 0; c < numChannels && c < kMaxChannels; ++c)
            sum += LinkwitzRiley4::flushSvf (st[static_cast<size_t> (c)], tiny);
        return sum;
    }

private:
    SvfCoeffs coeffs;
    std::array<SvfState, kMaxChannels> st {};
};

/** One Linkwitz-Riley band, per channel: an LR4 high-pass at lowHz into an
    LR4 low-pass at highHz (24 dB/oct skirts; the detector band of
    BassEngine's Impact punch, docs/11 E20). */
class LinkwitzRileyBand
{
public:
    void prepare (double sampleRate, double lowHz, double highHz) noexcept
    {
        const double q = 1.0 / std::sqrt (2.0);
        highPass = SvfCoeffs::make (FilterType::HighPass, lowHz, q, 0.0, sampleRate);
        lowPass = SvfCoeffs::make (FilterType::LowPass, highHz, q, 0.0, sampleRate);
        reset();
    }

    void reset() noexcept
    {
        for (auto& ch : st)
            for (auto& s : ch)
                s.reset();
    }

    float processSample (int ch, float x) noexcept
    {
        auto& s = st[static_cast<size_t> (ch)];
        const float h = svfTick (highPass, s[1], svfTick (highPass, s[0], x));
        return svfTick (lowPass, s[3], svfTick (lowPass, s[2], h));
    }

    /** See ThreeBandSplitter::flushStates. */
    float flushStates (int numChannels, float tiny) noexcept
    {
        float sum = 0.0f;
        for (int c = 0; c < numChannels && c < kMaxChannels; ++c)
            for (auto& s : st[static_cast<size_t> (c)])
                sum += LinkwitzRiley4::flushSvf (s, tiny);
        return sum;
    }

private:
    SvfCoeffs highPass, lowPass;
    std::array<std::array<SvfState, 4>, kMaxChannels> st {};
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

    /** RT-safe; moves the low / mid split (coefficients only, the state is
        kept). Glide it in small steps (ClarityEnhancer moves it once per
        control interval). */
    void setLowMidFrequency (double hz) noexcept { lowSplit.setFrequency (hz); }
    double getLowMidFrequency() const noexcept { return lowSplit.getFrequency(); }

    /** Flushes filter states below `tiny` to 0 on the first numChannels
        channels (so a decay reaches exact silence); returns the sum of the
        states kept, non-finite if any state is. */
    float flushStates (int numChannels, float tiny) noexcept
    {
        return lowSplit.flushStates (numChannels, tiny) + highSplit.flushStates (numChannels, tiny)
             + lowAlign.flushStates (numChannels, tiny);
    }

private:
    LinkwitzRiley4 lowSplit, highSplit;
    LinkwitzRileyAllPass lowAlign;
};
} // namespace flub
