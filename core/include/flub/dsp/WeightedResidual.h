// Flubsound Pro - loudness-weighted, masking-limited residual of a processing span (RT-safe).
//
// DistortionEstimator.h and ParallelDistortion.h measure one stage around
// its own curve with a scalar (or two-reference) least-squares fit. This
// measures a whole span of the chain - linear filters, gain riding and
// nonlinear stages together - from its input x (the reference, delayed by
// the span's latency by the caller) and its output y, both mono (the mid
// channel), and says how much of the output no linear time-invariant
// response of the input explains, and how much of that is audible
// (docs/11 E06 Phase 3, docs/03 §14.5):
//
//   Both sides first pass the same 8th-order Butterworth high-pass at
//   40 Hz, and bins below 40 Hz are not analysed (a linear filter on both
//   leaves any linear relation between them as it is): otherwise the
//   reference's subsonic content, which a span with a subsonic filter
//   removes, leaks through the window's main lobe (+-23 Hz) into the lowest
//   bins and reads as residual (pink noise through the bass engine's 20 Hz
//   high-pass read -12 dB).
//   Frames of N samples (N = the power of two at or above 80 ms: 4096 at
//   44.1 / 48 kHz), Hann window, hops drawn in [N/4, N/2] from a fixed
//   pseudo-random sequence (restarted by reset()): a hop that stayed a whole
//   number of periods of a steady tone would let a harmonic keep its phase
//   against the tone's window leakage, and the fit below would take it for
//   a linear response.
//   Per bin k, exponential averages over the frames (tau = kAverageSeconds)
//   of Sxx = |X|^2, Syy = |Y|^2 and Sxy = Y conj(X). The best linear
//   response per bin is H = Sxy / Sxx; what it leaves is the incoherent power
//     N_k = Syy - |Sxy|^2 / Sxx          (0 for any LTI span, exactly),
//   harmonics, intermodulation, noise and fast gain modulation; C_k = Syy -
//   N_k is the coherent (linearly processed programme) power. A bin whose
//   reference is below 1e-12 of the frame's mean is all incoherent, and no
//   bin may claim a linear power gain more than 6 dB over the largest band
//   gain |sum Sxy|^2 / (sum Sxx)^2 of the bands that carry the reference's
//   programme (within 30 dB of its strongest band): a harmonic a stage adds
//   on top of a faint copy already in its reference, phase-locked to the
//   same fundamental (the bass harmonics ahead of the saturator), would
//   otherwise fit as a gain of that copy (a saturator's 3rd harmonic of
//   50 Hz read 13 dB low).
//   Bins from 40 Hz to min (20 kHz, 0.49 fs) are summed into 25 critical
//   bands (Zwicker's Bark edges 100, 200, ..., 12000, 15500 Hz, the last
//   band up to 20 kHz), each bin weighted by the BS.1770 K-weighting's
//   power response w_k (the loudness weighting the product meters with):
//     N_b = sum w_k N_k,  C_b = sum w_k C_k,  Y_b = sum w_k Syy_k.
//   Masking, within the band only and only by the programme's own coherent
//   content (no spreading across bands, so a tone does not mask the
//   harmonics a stage adds above it, and generated content does not mask
//   itself): threshold T_b = C_b 10^(-O_b / 10) with Johnston's offset
//   O_b = alpha (14.5 + b) + (1 - alpha) 5.5 dB, alpha = min (1, SFM / -60 dB)
//   the tonality from the spectral flatness of the band's averaged coherent
//   power spectrum (a noise-like band masks within 5.5 dB, a tone only
//   14.5 dB and more under it); audible part
//     A_b = N_b^2 / (N_b + T_b)          (N_b far above T_b: all of it;
//                                         far below: N_b^2 / T_b).
//   Results (dB re the K-weighted output power sum_b Y_b):
//     getResidualDb() = 10 log10 (sum N_b / sum Y_b)   (loudness-weighted residual)
//     getWeightedDb() = 10 log10 (sum A_b / sum Y_b)   (its audible part)
//   A span without output reads kMinusInfDb. No spreading makes it
//   conservative: masking from neighbouring bands is not credited.
//
// Cost: one complex FFT of N (x and y packed as real and imaginary part)
// and O(N) bookkeeping per hop, every 21-43 ms at 48 kHz. process() is
// allocation-free; prepare() allocates.
#pragma once

#include "Biquad.h"
#include "Fft.h"
#include "flub/common/Math.h"
#include "flub/common/Realtime.h"

#include <array>
#include <complex>
#include <cstdint>
#include <vector>

namespace flub
{
class WeightedResidual
{
public:
    static constexpr double kFrameSeconds = 0.080;  // N = next power of two
    static constexpr double kAverageSeconds = 0.3;  // spectral averages
    static constexpr double kNoiseOffsetDb = 5.5;   // masking threshold under a noise-like band's coherent power
    static constexpr double kTonalOffsetDb = 14.5;  // ... under a tonal one: 14.5 + band index dB
    static constexpr int kNumBands = 25;

    /** Non-RT: allocates the frame buffers and the FFT. */
    void prepare (double sampleRate);
    void reset() noexcept FLUB_NONBLOCKING;

    /** Adds n aligned samples of the span's input (reference) and output;
        analyses every frame that completes on the way. */
    void process (const float* reference, const float* output, int n) noexcept FLUB_NONBLOCKING;

    /** True once a frame was analysed since reset(). */
    bool hasReading() const noexcept { return frames > 0; }
    std::int64_t getFramesAnalysed() const noexcept { return frames; }
    /** Loudness-weighted residual (dB re the output; kMinusInfDb when none). */
    float getResidualDb() const noexcept { return residualDb; }
    /** The residual without the K-weighting (dB re the output power from
        20 Hz to 20 kHz): the span's THD+N and IMD as a flat meter reads them. */
    float getPlainResidualDb() const noexcept { return plainResidualDb; }
    /** Its audible part (masking-limited, dB re the output). */
    float getWeightedDb() const noexcept { return weightedDb; }
    /** Per band (0 .. kNumBands - 1), K-weighted: incoherent and output power (dB, arbitrary reference). */
    float getBandResidualDb (int band) const noexcept { return bandResidualDb[static_cast<size_t> (band)]; }
    float getBandOutputDb (int band) const noexcept { return bandOutputDb[static_cast<size_t> (band)]; }
    /** Upper edge of band b in Hz (the last band ends at 20 kHz or 0.49 fs). */
    static float bandUpperHz (int band) noexcept;
    int getFrameSize() const noexcept { return size; }

private:
    void analyse() noexcept FLUB_NONBLOCKING;
    int nextHop() noexcept FLUB_NONBLOCKING;

    double sr = 48000.0;
    int size = 4096, writePos = 0, untilFrame = 4096, firstBin = 1, lastBin = 2047;
    float alpha = 0.1f;
    std::uint32_t rng = 1;
    std::int64_t frames = 0, filled = 0;
    std::vector<float> xRing, yRing, window, weight;
    std::vector<Fft::Complex> frame;
    std::vector<std::complex<double>> spectrumX, spectrumY;
    std::vector<double> sxx, syy, sxyRe, sxyIm;
    std::vector<int> bandOfBin;
    Fft fft;
    std::array<Biquad, 4> preHighPass; // channel 0: reference, 1: output
    float residualDb = kMinusInfDb, weightedDb = kMinusInfDb, plainResidualDb = kMinusInfDb;
    std::array<float, kNumBands> bandResidualDb {}, bandOutputDb {};
};
} // namespace flub
