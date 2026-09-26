// Flubsound Pro - double-precision transposed direct-form II biquad.
//
// Used where coefficients are static and precision matters more than
// modulation behaviour: BS.1770 K-weighting (metering accuracy), the
// Brown-Duda head-shadow sections in the headphone virtualiser, fixed
// de-emphasis networks. For modulated filters use the SVF.
#pragma once

#include "flub/common/AudioBlock.h"
#include "flub/common/Math.h"

#include <array>
#include <complex>

namespace flub
{
struct BiquadCoeffs
{
    // Normalised so that a0 == 1.
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

    static BiquadCoeffs identity() noexcept { return {}; }

    /** Bilinear transform of a first-order analog section
        H(s) = (B0 + B1 s) / (A0 + A1 s), with s = K (1 - z^-1) / (1 + z^-1). */
    static BiquadCoeffs fromAnalogFirstOrder (double B0, double B1, double A0, double A1, double sampleRate) noexcept
    {
        const double K = 2.0 * sampleRate;
        const double a0 = A0 + A1 * K;
        BiquadCoeffs c;
        c.b0 = (B0 + B1 * K) / a0;
        c.b1 = (B0 - B1 * K) / a0;
        c.b2 = 0.0;
        c.a1 = (A0 - A1 * K) / a0;
        c.a2 = 0.0;
        return c;
    }

    std::complex<double> response (double freqHz, double sampleRate) const noexcept
    {
        const std::complex<double> z1 = std::polar (1.0, -kTwoPi * freqHz / sampleRate);
        const std::complex<double> z2 = z1 * z1;
        return (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
    }
};

struct BiquadState
{
    double z1 = 0.0, z2 = 0.0;
    void reset() noexcept { z1 = z2 = 0.0; }
};

inline double biquadTick (const BiquadCoeffs& c, BiquadState& s, double x) noexcept
{
    const double y = c.b0 * x + s.z1;
    s.z1 = c.b1 * x - c.a1 * y + s.z2;
    s.z2 = c.b2 * x - c.a2 * y;
    return y;
}

class Biquad
{
public:
    void setCoeffs (const BiquadCoeffs& c) noexcept { coeffs = c; }
    const BiquadCoeffs& getCoeffs() const noexcept { return coeffs; }

    void reset() noexcept
    {
        for (auto& s : state)
            s.reset();
    }

    double processSample (int ch, double x) noexcept { return biquadTick (coeffs, state[static_cast<size_t> (ch)], x); }

    void process (const AudioBlock& block) noexcept
    {
        for (int c = 0; c < block.numChannels; ++c)
        {
            float* d = block.channel (c);
            auto& s = state[static_cast<size_t> (c)];
            for (int i = 0; i < block.numSamples; ++i)
                d[i] = static_cast<float> (biquadTick (coeffs, s, d[i]));
        }
    }

private:
    BiquadCoeffs coeffs;
    std::array<BiquadState, kMaxChannels> state {};
};
} // namespace flub
