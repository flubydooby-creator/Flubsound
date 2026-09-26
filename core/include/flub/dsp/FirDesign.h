// Flubsound Pro - windowed-sinc FIR design helpers (prepare-time only).
#pragma once

#include "flub/common/Math.h"

#include <cmath>
#include <vector>

namespace flub::fir
{
/** Zeroth-order modified Bessel function of the first kind (series). */
inline double besselI0 (double x) noexcept
{
    double sum = 1.0, term = 1.0;
    const double halfX = 0.5 * x;
    for (int k = 1; k < 64; ++k)
    {
        term *= (halfX / k) * (halfX / k);
        sum += term;
        if (term < 1.0e-14 * sum)
            break;
    }
    return sum;
}

inline double kaiser (int n, int length, double beta) noexcept
{
    if (length <= 1)
        return 1.0;
    const double r = 2.0 * n / (length - 1) - 1.0;
    return besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / besselI0 (beta);
}

inline double sinc (double x) noexcept
{
    return std::abs (x) < 1.0e-12 ? 1.0 : std::sin (kPi * x) / (kPi * x);
}

/** Linear-phase low-pass, odd length, cutoff in cycles/sample (0..0.5),
    unity DC gain. Allocates: prepare-time only. */
inline std::vector<double> lowpass (int length, double cutoff, double kaiserBeta)
{
    std::vector<double> h (static_cast<size_t> (length));
    const int centre = (length - 1) / 2;
    double sum = 0.0;
    for (int n = 0; n < length; ++n)
    {
        h[static_cast<size_t> (n)] = 2.0 * cutoff * sinc (2.0 * cutoff * (n - centre)) * kaiser (n, length, kaiserBeta);
        sum += h[static_cast<size_t> (n)];
    }
    for (auto& v : h)
        v /= sum;
    return h;
}
} // namespace flub::fir
