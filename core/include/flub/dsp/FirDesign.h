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
} // namespace flub::fir
