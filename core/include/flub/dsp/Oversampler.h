// Flubsound Pro - linear-phase polyphase oversampling (1x / 2x / 4x).
//
// Every nonlinearity (soft clipper, saturation) runs oversampled so that the
// harmonics it creates above the original Nyquist are filtered out instead of
// aliasing back into the audible band.
//
// Design: cascaded half-band FIR stages (Kaiser-windowed sinc). A half-band
// filter has every other tap equal to zero, so each 2x stage costs 2d (~N/2)
// multiply-adds per lower-rate sample per direction (tap symmetry is not
// exploited), and phase 0 of the interpolator is a pure delay. Stage lengths are chosen so the round-trip latency is a
// whole number of base-rate samples (required for latency compensation):
//
//   stage 1 (base <-> 2x): 4*d1+1 taps, round trip = 2*d1 base samples
//   stage 2 (2x <-> 4x)  : 4*d2+1 taps, round trip = d2 base samples
//
//   Quality::High : d1 = 16, d2 = 4 -> 4x latency 36 smp
//   Quality::Low  : d1 = 8,  d2 = 3 -> 4x latency 19 smp (gaming low latency)
// Image rejection is ~90 dB (High) / ~50 dB (Low) up to ~0.375 fs, falling to
// 67 / 40 dB at 20 kHz / 48 kHz and 25 / 18 dB at 20 kHz / 44.1 kHz. The
// passband droops near fs/2 (20 kHz / 44.1 kHz: -1.0 dB High, -2.3 dB Low), so
// the nonlinear stages use DELTA oversampling: only the deviation f(x^) - x^
// is band-limited and added to the exactly delayed input.
//
// A minimum-phase / polyphase-IIR variant (lower latency, non-linear phase)
// is the planned alternative for the "Competitive" latency profile.
#pragma once

#include "flub/common/AudioBlock.h"

#include <array>
#include <vector>

namespace flub
{
class HalfbandStage
{
public:
    /** Allocates. d = half-order parameter (taps = 4d + 1). */
    void prepare (int numChannels, int d, double kaiserBeta);
    void reset() noexcept;

    /** in: n samples at the lower rate; out: 2n samples at the higher rate. */
    void upsample (int ch, const float* in, float* out, int n) noexcept;
    /** in: 2n samples at the higher rate; out: n samples at the lower rate. */
    void downsample (int ch, const float* in, float* out, int n) noexcept;

    /** Round-trip (up + down) latency in samples at the LOWER rate of this stage. */
    int roundTripLatency() const noexcept { return 2 * d; }

private:
    // Mirrored history: contiguous newest-first window at &buf[pos].
    struct History
    {
        std::vector<float> buf;
        int len = 0, pos = 0;
        void prepare (int length)
        {
            len = length;
            buf.assign (static_cast<size_t> (2 * length), 0.0f);
            pos = 0;
        }
        void reset() noexcept { std::fill (buf.begin(), buf.end(), 0.0f); pos = 0; }
        void push (float x) noexcept
        {
            pos = (pos == 0 ? len - 1 : pos - 1);
            buf[static_cast<size_t> (pos)] = x;
            buf[static_cast<size_t> (pos + len)] = x;
        }
        const float* window() const noexcept { return buf.data() + pos; }
    };

    int d = 0;
    std::vector<float> oddTaps; // h[2j+1], j = 0 .. 2d-1 (DC-normalised: sum = 0.5)
    std::array<History, kMaxChannels> upHist, downEven, downOdd;
};

class Oversampler
{
public:
    enum class Quality { Low, High };

    /** Allocates. factor must be 1, 2 or 4. */
    void prepare (int numChannels, int maxBlockSize, int factor, Quality quality = Quality::High);
    void reset() noexcept;

    int getFactor() const noexcept { return factor; }

    /** Round-trip latency (up + down) in base-rate samples. Integer by design. */
    int latencySamples() const noexcept { return latency; }

    /** Upsamples `in` and returns a view of the internal oversampled buffer.
        For factor 1 it returns `in` itself (process in place). */
    AudioBlock upsample (const AudioBlock& in) noexcept;

    /** Downsamples the internal buffer into `out` (no-op for factor 1). */
    void downsample (const AudioBlock& out) noexcept;

private:
    int factor = 1, channels = 0, maxBlock = 0, latency = 0;
    HalfbandStage stage1, stage2;
    std::vector<float> mid, high; // per channel planar: [ch * stride]
    int midStride = 0, highStride = 0;
    std::array<float*, kMaxChannels> midPtrs {}, highPtrs {};
    int lastNumSamples = 0;
};
} // namespace flub
