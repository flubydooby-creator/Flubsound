// Flubsound Pro - the band layout shared by ControlKind::BandGains models and
// their renderer (docs/09 §1.1, docs/03 §16).
//
// A BandGains model controls the gain of numBands frequency bands. Both sides
// use the same short-time Fourier transform, so a model's features and the
// gains it returns refer to the same spectra:
//   * hop = the model's frameSize N; window = 2N samples, the Vorbis power-
//     complementary window w[n] = sin (pi/2 sin^2 (pi (n + 0.5) / 2N)), which
//     satisfies w[n]^2 + w[n + N]^2 = 1, so analysis and synthesis with it and
//     unity gains reconstruct the input exactly (overlap-add, 50 %);
//   * the 2N windowed samples zero-padded to fftSize (a power of two);
//   * bin k (frequency k * sampleRate / fftSize) lies between two band centres
//     c[lo] <= f < c[lo + 1]; it takes the weights (1 - frac, frac) of bands lo
//     and lo + 1, frac = (f - c[lo]) / (c[lo + 1] - c[lo]); below the first
//     centre it belongs to band 0, at or above the last to the last band.
// The same weights map band gains to bin gains (bandsToBins: linear
// interpolation) and bin powers to band energies (binsToBands: triangular
// bands, RNNoise's layout), so the two maps are transposes of each other.
// tools/neural/fvdsp.py is the numpy twin the models are trained with.
#pragma once

#include <cmath>
#include <vector>

namespace flub::bandgains
{
/** Window value n of a Vorbis window of `length` samples (length = 2 * hop). */
inline float vorbisWindow (int n, int length) noexcept
{
    const double s = std::sin (3.14159265358979323846 * (n + 0.5) / length);
    return static_cast<float> (std::sin (0.5 * 3.14159265358979323846 * s * s));
}

/** Bin <-> band weights for one layout, FFT size and sample rate. */
class BandMap
{
public:
    /** Non-RT (allocates). centresHz: bandCount >= 2 strictly increasing values. */
    void build (const float* centresHz, int bandCount, int fftSize, double sampleRate)
    {
        bands = bandCount;
        bins = fftSize / 2 + 1;
        lo.assign (static_cast<size_t> (bins), 0);
        frac.assign (static_cast<size_t> (bins), 0.0f);
        for (int k = 0; k < bins; ++k)
        {
            const double f = k * (sampleRate / fftSize);
            int b = 0;
            double w = 0.0;
            if (f >= centresHz[bandCount - 1])
            {
                b = bandCount - 2;
                w = 1.0;
            }
            else if (f > centresHz[0])
            {
                while (b + 1 < bandCount && centresHz[b + 1] <= f)
                    ++b;
                w = (f - centresHz[b]) / (static_cast<double> (centresHz[b + 1]) - centresHz[b]);
            }
            lo[static_cast<size_t> (k)] = b;
            frac[static_cast<size_t> (k)] = static_cast<float> (w);
        }
    }

    int numBands() const noexcept { return bands; }
    int numBins() const noexcept { return bins; }

    /** binGains[k] = (1 - frac) g[lo] + frac g[lo + 1] for every bin. */
    void bandsToBins (const float* bandGains, float* binGains) const noexcept
    {
        for (int k = 0; k < bins; ++k)
        {
            const auto i = static_cast<size_t> (k);
            const int b = lo[i];
            binGains[k] = (1.0f - frac[i]) * bandGains[b] + frac[i] * bandGains[b + 1];
        }
    }

    /** energy[b] = sum over bins of power[k] times the bin's weight for band b. */
    void binsToBands (const float* binPower, float* energy) const noexcept
    {
        for (int b = 0; b < bands; ++b)
            energy[b] = 0.0f;
        for (int k = 0; k < bins; ++k)
        {
            const auto i = static_cast<size_t> (k);
            const int b = lo[i];
            energy[b] += (1.0f - frac[i]) * binPower[k];
            energy[b + 1] += frac[i] * binPower[k];
        }
    }

private:
    int bands = 0, bins = 0;
    std::vector<int> lo;
    std::vector<float> frac;
};
} // namespace flub::bandgains
