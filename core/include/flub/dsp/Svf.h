// Flubsound Pro - topology-preserving-transform state variable filter.
//
// Andrew Simper's (Cytomic) linear trapezoidal SVF. Chosen as the workhorse
// filter because, unlike direct-form biquads, it stays well behaved when its
// coefficients are modulated every few samples (dynamic EQ, smoothed sweeps)
// and has good numerical behaviour for low cutoffs at high sample rates.
//
//   g = tan(pi * fc / fs), k = 1 / Q
//   a1 = 1 / (1 + g (g + k)), a2 = g a1, a3 = g a2
//   per sample:
//     v3 = v0 - ic2;  v1 = a1 ic1 + a2 v3;  v2 = ic2 + a2 ic1 + a3 v3
//     ic1 = 2 v1 - ic1;  ic2 = 2 v2 - ic2
//     y = m0 v0 + m1 v1 + m2 v2
// where v1 is the band-pass output s/(s^2+ks+1) and v2 the low-pass 1/(s^2+ks+1).
// The mix coefficients (m0, m1, m2) select the response. The bilinear mapping
// s = j tan(w/2) / g gives the exact digital response used by response().
#pragma once

#include "flub/common/AudioBlock.h"
#include "flub/common/Math.h"

#include <array>
#include <complex>
#include <cstdint>

namespace flub
{
enum class FilterType : uint8_t
{
    Bell = 0,
    LowShelf,
    HighShelf,
    LowPass,
    HighPass,
    BandPass, // unity gain at centre
    Notch,
    AllPass
};

struct SvfCoeffs
{
    float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f;
    float m0 = 1.0f, m1 = 0.0f, m2 = 0.0f;
    double g = 0.0, k = 1.0; // kept (double) for exact response evaluation

    static double clampFrequency (double freq, double sampleRate) noexcept
    {
        return std::clamp (freq, 5.0, 0.49 * sampleRate);
    }

    static SvfCoeffs make (FilterType type, double freq, double q, double gainDb, double sampleRate) noexcept
    {
        SvfCoeffs c;
        freq = clampFrequency (freq, sampleRate);
        q = std::clamp (q, 0.025, 40.0);
        const double w = std::tan (kPi * freq / sampleRate);
        const double A = std::pow (10.0, gainDb / 40.0);
        double g = w, k = 1.0 / q;
        double m0 = 0.0, m1 = 0.0, m2 = 0.0;

        switch (type)
        {
            case FilterType::Bell:
                k = 1.0 / (q * A);
                m0 = 1.0; m1 = k * (A * A - 1.0); m2 = 0.0;
                break;
            case FilterType::LowShelf:
                g = w / std::sqrt (A);
                m0 = 1.0; m1 = k * (A - 1.0); m2 = A * A - 1.0;
                break;
            case FilterType::HighShelf:
                g = w * std::sqrt (A);
                m0 = A * A; m1 = k * (1.0 - A) * A; m2 = 1.0 - A * A;
                break;
            case FilterType::LowPass:  m0 = 0.0; m1 = 0.0; m2 = 1.0; break;
            case FilterType::HighPass: m0 = 1.0; m1 = -k; m2 = -1.0; break;
            case FilterType::BandPass: m0 = 0.0; m1 = k; m2 = 0.0; break;
            case FilterType::Notch:    m0 = 1.0; m1 = -k; m2 = 0.0; break;
            case FilterType::AllPass:  m0 = 1.0; m1 = -2.0 * k; m2 = 0.0; break;
        }

        const double a1 = 1.0 / (1.0 + g * (g + k));
        const double a2 = g * a1;
        const double a3 = g * a2;
        c.a1 = static_cast<float> (a1);
        c.a2 = static_cast<float> (a2);
        c.a3 = static_cast<float> (a3);
        c.m0 = static_cast<float> (m0);
        c.m1 = static_cast<float> (m1);
        c.m2 = static_cast<float> (m2);
        c.g = g;
        c.k = k;
        return c;
    }

    /** Exact complex response of the digital filter at freqHz. Thread-safe, pure. */
    std::complex<double> response (double freqHz, double sampleRate) const noexcept
    {
        const double omega = std::tan (kPi * std::clamp (freqHz, 0.0, 0.4999 * sampleRate) / sampleRate) / g;
        const std::complex<double> s (0.0, omega);
        const std::complex<double> lp = 1.0 / (s * s + k * s + 1.0);
        const std::complex<double> bp = s * lp;
        return static_cast<double> (m0) + static_cast<double> (m1) * bp + static_cast<double> (m2) * lp;
    }

    double magnitudeDb (double freqHz, double sampleRate) const noexcept
    {
        return 20.0 * std::log10 (std::max (1.0e-12, std::abs (response (freqHz, sampleRate))));
    }
};

struct SvfState
{
    float ic1 = 0.0f, ic2 = 0.0f;
    void reset() noexcept { ic1 = ic2 = 0.0f; }
};

/** Raw tick. Returns the mixed output; lowOut/bandOut optionally receive v2/v1. */
inline float svfTick (const SvfCoeffs& c, SvfState& s, float v0) noexcept
{
    const float v3 = v0 - s.ic2;
    const float v1 = c.a1 * s.ic1 + c.a2 * v3;
    const float v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
    s.ic1 = 2.0f * v1 - s.ic1;
    s.ic2 = 2.0f * v2 - s.ic2;
    return c.m0 * v0 + c.m1 * v1 + c.m2 * v2;
}

/** Tick that returns the raw low-pass (v2) and band-pass (v1) outputs; the
    high-pass is v0 - k*v1 - v2. Used by crossovers that need LP and HP from
    one state. */
inline void svfTickRaw (const SvfCoeffs& c, SvfState& s, float v0, float& v1Out, float& v2Out) noexcept
{
    const float v3 = v0 - s.ic2;
    const float v1 = c.a1 * s.ic1 + c.a2 * v3;
    const float v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
    s.ic1 = 2.0f * v1 - s.ic1;
    s.ic2 = 2.0f * v2 - s.ic2;
    v1Out = v1;
    v2Out = v2;
}

/** Multichannel SVF with shared coefficients. */
class SvfFilter
{
public:
    void setCoeffs (const SvfCoeffs& c) noexcept { coeffs = c; }
    void set (FilterType t, double f, double q, double gainDb, double sr) noexcept { coeffs = SvfCoeffs::make (t, f, q, gainDb, sr); }
    const SvfCoeffs& getCoeffs() const noexcept { return coeffs; }

    void reset() noexcept
    {
        for (auto& s : state)
            s.reset();
    }

    float processSample (int ch, float x) noexcept { return svfTick (coeffs, state[static_cast<size_t> (ch)], x); }

    void process (const AudioBlock& block) noexcept
    {
        for (int c = 0; c < block.numChannels; ++c)
        {
            float* d = block.channel (c);
            auto& s = state[static_cast<size_t> (c)];
            for (int i = 0; i < block.numSamples; ++i)
                d[i] = svfTick (coeffs, s, d[i]);
        }
    }

private:
    SvfCoeffs coeffs;
    std::array<SvfState, kMaxChannels> state {};
};

/** Butterworth Q values for cascades of 2nd-order sections: order 2N filter. */
inline double butterworthQ (int numSections, int sectionIndex) noexcept
{
    // Poles of an order-2N Butterworth: Q_k = 1 / (2 sin((2k+1) pi / (4N)))
    const int order = 2 * numSections;
    return 1.0 / (2.0 * std::sin ((2.0 * sectionIndex + 1.0) * kPi / (2.0 * order)));
}
} // namespace flub
