// Flubsound Pro - linear-phase polyphase oversampling (1x / 2x / 4x / 8x).
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
// Rate-aware designs (docs/11 E10 step 3, forProfile(), used by the
// saturator): the half-band's transition is centred on the base Nyquist, so
// at 44.1 kHz a harmonic at 25 kHz folds to 19.1 kHz almost unattenuated,
// and at 2x every harmonic above the 2x Nyquist folds inside the oversampled
// domain. Below 176.4 kHz the saturator therefore runs 4x with short
// half-bands and downsamples stage 1 through a DecimatorStage, a general
// low-pass whose transition ends near the image of 20 kHz, in the latency
// the fixed 2x designs had (Quality 32, Balanced / Low Latency 16 base-rate
// samples, so every chain latency is unchanged). Worst in-band alias of a
// -6 dBFS 1 / 5 / 7 / 10 kHz sine through Tape at 9 dB drive (Warmth 100):
// 44.1 kHz -32.2 -> -78.5 dBc (Quality), -32.3 -> -74.2 dBc (Balanced / Low
// Latency); 48 kHz -41.2 -> -83.2 / -41.1 -> -78.0 dBc (test_signal_hygiene.cpp).
// From 176.4 kHz up the fixed 2x designs stay.
//
// Harder drive (docs/11 E10 Phase 2): a hard-driven curve is close to a
// square wave, whose harmonics fall only 6 dB per octave, so at 24 dB drive
// the 17th / 19th harmonics of 10 kHz fold straight into the audible band at
// 4x (-28 dBc). Two tools, both in Design: factor 8 (a third half-band stage,
// 9 taps: at 8x it only has to reject what would fold onto 0 - 20 kHz, so
// its transition runs from 20 kHz to the image band) and `adaa`, for a
// curve evaluated with first-order antiderivative anti-aliasing (the
// Saturator's curves). ADAA1 is the curve followed by a one-sample box
// filter, whose sinc response nulls every multiple of the oversampled rate,
// i.e. exactly where the harmonics that fold near 0 Hz come from (-28 ->
// about -50 dBc at 4x, and below -70 dBc at 8x); it delays the curve by half
// an oversampled sample, which stage 1's decimator takes back: with `adaa`
// its taps are centred 1 / factor of a 2x-rate sample early (a Kaiser
// windowed sinc at a fractional centre), so the deviation stays aligned
// with the exactly delayed dry path and the latency is unchanged.
//
// The maximizer's clipper has a table of its own (forClipper): the latency
// of its fixed designs, with Low Latency below 88.2 kHz on the 16-sample
// 4x design instead of 2x Low (24 dB of drive, crest gate off, 44.1 kHz:
// -19.0 -> -42.0 dBc), no ADAA (its depth-capped curve has no closed-form
// antiderivative).
//
// A minimum-phase / polyphase-IIR variant (lower latency, non-linear phase)
// is the planned alternative for the "Competitive" latency profile.
#pragma once

#include "flub/common/AudioBlock.h"

#include <algorithm>
#include <array>
#include <vector>

namespace flub
{
/** Mirrored FIR history: a contiguous newest-first window at window(). */
struct FirHistory
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
    int d = 0;
    std::vector<float> oddTaps; // h[2j+1], j = 0 .. 2d-1 (DC-normalised: sum = 0.5)
    std::array<FirHistory, kMaxChannels> upHist, downEven, downOdd;
};

/** Stage-1 downsampler (2x -> base) of the rate-aware designs: a general
    Kaiser-windowed-sinc low-pass of 4m + 1 taps at the higher rate, so the
    transition band can sit below the base Nyquist (a half-band's is centred
    on it). Delay m base-rate samples, less `advance` higher-rate samples. */
class DecimatorStage
{
public:
    /** Allocates. cutoff = -6 dB point as a fraction of the higher rate;
        advance (0 .. 1) = higher-rate samples by which the taps are centred
        early, to take back a fractional delay of what feeds the stage. */
    void prepare (int numChannels, int m, double cutoff, double kaiserBeta, double advance = 0.0);
    void reset() noexcept;

    /** in: 2n samples at the higher rate; out: n samples at the lower rate. */
    void downsample (int ch, const float* in, float* out, int n) noexcept;

    int delay() const noexcept { return m; }

private:
    int m = 0;
    std::vector<float> taps; // 4m + 1, DC gain 1
    std::array<FirHistory, kMaxChannels> hist;
};

class Oversampler
{
public:
    enum class Quality { Low, High };

    /** Latency profile of a rate-aware design (forProfile). */
    enum class Profile { Quality, Balanced, LowLatency };

    /** Factor and stage designs: stage 1 (base <-> 2x) has 4 d1 + 1 taps,
        stage 2 (2x <-> 4x) 4 d2 + 1, stage 3 (4x <-> 8x) 4 d3 + 1, each a
        Kaiser-windowed half-band sinc with the given beta. With m1 > 0
        stage 1 downsamples through a DecimatorStage of 4 m1 + 1 taps
        (cutoff1 re the 2x rate) instead of its half-band. Round trip in
        base-rate samples: d1 + (m1 or d1), plus d2 at factor 4 and 8, plus
        d3 / 2 at factor 8 (d3 even). adaa: the curve between upsample()
        and downsample() is first-order ADAA (half an oversampled sample
        late); stage 1's decimator (m1 > 0 required) takes that back. */
    struct Design
    {
        int factor = 2;      // 1, 2, 4 or 8
        int d1 = 16, d2 = 4; // half-band half-orders
        double beta1 = 9.0, beta2 = 8.0;
        int m1 = 0;          // 0: stage 1 downsamples with its half-band
        double cutoff1 = 0.25, betaDown1 = 9.0;
        int d3 = 2;          // factor 8 only
        double beta3 = 5.0;
        bool adaa = false;

        bool operator== (const Design&) const = default;
    };

    /** The fixed half-band designs: High (d1 16, d2 4, beta 9 / 8), Low (8, 3, 4.5 / 6). */
    static Design design (int factor, Quality quality) noexcept;

    /** The rate-aware saturator table (docs/11 E10 step 3, see the header
        comment): the same latency in samples as the fixed 2x designs it
        replaced (Quality 32, Balanced / Low Latency 16) at every rate. */
    static Design forProfile (Profile profile, double sampleRate) noexcept;

    /** The maximizer clipper's table (docs/11 E10): the latency of the
        fixed designs it had (Quality / Balanced 4x High, 36 samples; Low
        Latency 16), with Low Latency below 88.2 kHz running 4x (the
        saturator's 16-sample design, without ADAA) instead of 2x Low. */
    static Design forClipper (Profile profile, double sampleRate) noexcept;

    /** Allocates. factor must be 1, 2 or 4. */
    void prepare (int numChannels, int maxBlockSize, int factor, Quality quality = Quality::High);
    /** Allocates. design.factor must be 1, 2, 4 or 8, d1 / d2 >= 1, d3 >= 2 and even. */
    void prepare (int numChannels, int maxBlockSize, const Design& design);
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
    HalfbandStage stage1, stage2, stage3;
    DecimatorStage down1; // stage-1 downsampler when the design has m1 > 0
    bool useDecimator = false;
    std::vector<float> mid, high, top; // per channel planar: [ch * stride] at 2x, 4x, 8x
    int midStride = 0, highStride = 0, topStride = 0;
    std::array<float*, kMaxChannels> midPtrs {}, highPtrs {}, topPtrs {};
    int lastNumSamples = 0;
};
} // namespace flub
