#include "flub/dsp/Fft.h"

#include "flub/common/Math.h"

#include <cassert>
#include <utility>

namespace flub
{
void Fft::prepare (int size)
{
    assert (isPowerOfTwo (size) && size >= 2);
    n = size;
    int bits = 0;
    while ((1 << bits) < n)
        ++bits;

    bitrev.resize (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            r |= ((i >> b) & 1) << (bits - 1 - b);
        bitrev[static_cast<size_t> (i)] = r;
    }

    twiddles.resize (static_cast<size_t> (n / 2));
    for (int k = 0; k < n / 2; ++k)
    {
        const double a = -kTwoPi * k / n;
        twiddles[static_cast<size_t> (k)] = Complex (static_cast<float> (std::cos (a)), static_cast<float> (std::sin (a)));
    }
    scratch.assign (static_cast<size_t> (n), Complex {});
}

void Fft::transform (Complex* data, bool inverseDir) const noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const int j = bitrev[static_cast<size_t> (i)];
        if (j > i)
            std::swap (data[i], data[j]);
    }

    for (int len = 2; len <= n; len <<= 1)
    {
        const int half = len >> 1;
        const int step = n / len;
        for (int start = 0; start < n; start += len)
        {
            for (int k = 0; k < half; ++k)
            {
                Complex w = twiddles[static_cast<size_t> (k * step)];
                if (inverseDir)
                    w = std::conj (w);
                const Complex a = data[start + k];
                const Complex b = data[start + k + half] * w;
                data[start + k] = a + b;
                data[start + k + half] = a - b;
            }
        }
    }
}

void Fft::forward (Complex* data) const noexcept { transform (data, false); }

void Fft::inverse (Complex* data) const noexcept
{
    transform (data, true);
    const float scale = 1.0f / static_cast<float> (n);
    for (int i = 0; i < n; ++i)
        data[i] *= scale;
}

void Fft::forwardReal (const float* in, Complex* outBins) noexcept
{
    for (int i = 0; i < n; ++i)
        scratch[static_cast<size_t> (i)] = Complex (in[i], 0.0f);
    transform (scratch.data(), false);
    for (int k = 0; k <= n / 2; ++k)
        outBins[k] = scratch[static_cast<size_t> (k)];
}

void Fft::inverseReal (const Complex* bins, float* out) noexcept
{
    scratch[0] = Complex (bins[0].real(), 0.0f);
    scratch[static_cast<size_t> (n / 2)] = Complex (bins[n / 2].real(), 0.0f);
    for (int k = 1; k < n / 2; ++k)
    {
        scratch[static_cast<size_t> (k)] = bins[k];
        scratch[static_cast<size_t> (n - k)] = std::conj (bins[k]);
    }
    transform (scratch.data(), true);
    const float scale = 1.0f / static_cast<float> (n);
    for (int i = 0; i < n; ++i)
        out[i] = scratch[static_cast<size_t> (i)].real() * scale;
}
} // namespace flub
