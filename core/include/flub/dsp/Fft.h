// Flubsound Pro - small radix-2 complex FFT (RT-safe after prepare()).
//
// Reference implementation so the core has zero third-party dependencies.
// Production builds should swap in PFFFT (BSD) or the platform FFT
// (vDSP / IPP) behind this same interface. The STFT noise gate is the only
// realtime user today; the HRIR convolver is direct-form, and a partitioned
// FFT convolver for longer responses is roadmap.
#pragma once

#include <complex>
#include <vector>

namespace flub
{
class Fft
{
public:
    using Complex = std::complex<float>;

    /** Allocates. size must be a power of two >= 2. */
    void prepare (int size);
    int getSize() const noexcept { return n; }

    /** In-place forward transform (no scaling). */
    void forward (Complex* data) const noexcept;
    /** In-place inverse transform, scaled by 1/N. */
    void inverse (Complex* data) const noexcept;

    /** Real-input forward: writes N/2+1 bins. Uses internal scratch (not
        re-entrant across threads; one Fft instance per thread). */
    void forwardReal (const float* in, Complex* outBins) noexcept;
    /** Hermitian N/2+1 bins -> N real samples (scaled 1/N). */
    void inverseReal (const Complex* bins, float* out) noexcept;

private:
    void transform (Complex* data, bool inverseDir) const noexcept;

    int n = 0;
    std::vector<int> bitrev;
    std::vector<Complex> twiddles; // e^{-j 2 pi k / N}, k < N/2
    std::vector<Complex> scratch;
};
} // namespace flub
