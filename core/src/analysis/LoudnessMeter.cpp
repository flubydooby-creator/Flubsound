// Flubsound Pro - ITU-R BS.1770-4 / EBU R128 loudness meter.
//
// Signal flow, per channel:
//   x -> K-weighting stage 1 (high shelf) -> stage 2 (RLB high-pass) -> y^2
// The squares are summed per channel over a 100 ms sub-block. At every
// sub-block boundary the per-channel sums are combined with the BS.1770
// channel weights into one energy value that enters a 30-slot ring (3 s).
// Every window the meter reports is a sum of whole sub-blocks from that ring,
// so all readings update at exactly 10 Hz, on a sample-count grid that does
// not depend on how the host slices the stream into blocks.
//
// Gated measurements (integrated loudness, LRA) keep only histograms: the
// bins hold exact energy sums and counts, which makes the absolute-gated mean
// (and so the relative gate) exact, and quantises nothing but the single
// 0.01 LU bin that straddles the relative gate. Memory is O(1) for any
// programme length.
#include "flub/analysis/LoudnessMeter.h"

#include "flub/analysis/ChannelWeights.h"

#include <algorithm>
#include <cmath>

namespace flub
{
namespace
{
constexpr double kLoudnessOffset = -0.691; // cancels the K-weighting gain at 997 Hz
constexpr double kAbsoluteGateLufs = -70.0;
constexpr double kIntegratedRelativeGateLu = -10.0; // BS.1770-4 / EBU R128
constexpr double kRangeRelativeGateLu = -20.0;      // EBU Tech 3342
constexpr double kRangeLowFraction = 0.10;          // P10
constexpr double kRangeHighFraction = 0.95;         // P95

// Histogram: from the absolute gate (-70 LUFS) up to +30 LUFS, in 0.1 LU
// coarse bins that are each split into ten 0.01 LU fine bins. Nothing real is
// louder than ~+13 LUFS (full-scale square waves in 7.1), but float signals
// can exceed 0 dBFS; such blocks land in the top bin with their true energy,
// so the gated mean stays exact and only LRA resolution saturates.
constexpr int kFinePerCoarse = 10;
constexpr int kNumCoarseBins = 1000;
constexpr int kNumFineBins = kNumCoarseBins * kFinePerCoarse;
constexpr double kFineBinsPerLu = 100.0;

// The K-weighting designs are valid for any fs > 2 * 1682 Hz; below a sane
// minimum the prewarping tan() would fold over and give an unstable filter.
constexpr double kMinDesignRate = 8000.0;
constexpr double kMaxSampleRate = 1.0e7;

// A constant -400 dBFS offset added at the filter input. The RLB high-pass has
// a double zero at DC, so it never reaches the measured energy, but it keeps
// every recursive state at a normal, non-zero steady-state value during
// digital silence: the filters cannot decay into subnormals even when the
// caller forgot to enable FTZ/DAZ.
constexpr double kAntiDenormal = 1.0e-20;

double energyToLufs (double energy) noexcept
{
    return kLoudnessOffset + 10.0 * std::log10 (energy);
}

float toReading (double energy) noexcept
{
    if (! (energy > 0.0))
        return kMinusInfDb;
    return static_cast<float> (std::max (static_cast<double> (kMinusInfDb), energyToLufs (energy)));
}

/** Fine (0.01 LU) bin of a loudness; the coarse bin is index / kFinePerCoarse. */
int fineBinIndex (double lufs) noexcept
{
    const double pos = std::floor ((lufs - kAbsoluteGateLufs) * kFineBinsPerLu);
    if (! (pos >= 0.0))
        return 0; // also catches NaN, whose float -> int conversion would be UB
    return static_cast<int> (std::min (pos, static_cast<double> (kNumFineBins - 1)));
}

/** Rate used for the K-weighting designs: non-finite rates fall back to 48 kHz
    (tan() of NaN, or of 0 for an infinite rate, would give NaN coefficients or
    a double pole on the unit circle), tiny ones are raised to the design minimum. */
double designRate (double fsHz) noexcept
{
    if (! std::isfinite (fsHz))
        return 48000.0;
    return std::max (fsHz, kMinDesignRate);
}
} // namespace

//==============================================================================
BiquadCoeffs LoudnessMeter::kWeightingStage1 (double fsHz) noexcept
{
    // "Pre-filter": a second-order high shelf (+4 dB above ~1.7 kHz) that models
    // the acoustic effect of the head. Derived from its analog prototype through
    // a bilinear transform prewarped at f0 (the libebur128 derivation), so every
    // sample rate gets the same analog response; at 48 kHz it reproduces the
    // BS.1770 coefficient table to ~1e-14.
    constexpr double f0 = 1681.974450955533;
    constexpr double gainDb = 3.999843853973347;
    constexpr double q = 0.7071752369554196;

    const double k = std::tan (kPi * f0 / designRate (fsHz));
    const double vh = std::pow (10.0, gainDb / 20.0);
    const double vb = std::pow (vh, 0.4996667741545416);
    const double a0 = 1.0 + k / q + k * k;

    BiquadCoeffs c;
    c.b0 = (vh + vb * k / q + k * k) / a0;
    c.b1 = 2.0 * (k * k - vh) / a0;
    c.b2 = (vh - vb * k / q + k * k) / a0;
    c.a1 = 2.0 * (k * k - 1.0) / a0;
    c.a2 = (1.0 - k / q + k * k) / a0;
    return c;
}

BiquadCoeffs LoudnessMeter::kWeightingStage2 (double fsHz) noexcept
{
    // "RLB" (revised low-frequency B-curve): a second-order high-pass at ~38 Hz.
    // The numerator is the unnormalised [1, -2, 1] exactly as in the BS.1770
    // table (its ~+0.04 dB passband gain is part of the standard and is covered
    // by the -0.691 dB offset).
    constexpr double f0 = 38.13547087602444;
    constexpr double q = 0.5003270373238773;

    const double k = std::tan (kPi * f0 / designRate (fsHz));
    const double d = 1.0 + k / q + k * k;

    BiquadCoeffs c;
    c.b0 = 1.0;
    c.b1 = -2.0;
    c.b2 = 1.0;
    c.a1 = 2.0 * (k * k - 1.0) / d;
    c.a2 = (1.0 - k / q + k * k) / d;
    return c;
}

//==============================================================================
void LoudnessMeter::prepare (double newSampleRate, int numChannels)
{
    // Nonsense rates (<= 0, NaN, Inf) fall back to 48 kHz; the upper clamp keeps
    // the sub-block length (an int) meaningful for any finite input.
    fs = std::isfinite (newSampleRate) && newSampleRate > 0.0 ? std::min (newSampleRate, kMaxSampleRate) : 48000.0;
    channels = std::clamp (numChannels, 1, kMaxChannels);
    stage1Coeffs = kWeightingStage1 (fs);
    stage2Coeffs = kWeightingStage2 (fs);

    // 100 ms sub-blocks: 4410 / 4800 / 9600 / 19200 samples at the standard
    // rates (exact). All windows and the gating overlap (75 % of 400 ms) are
    // whole multiples of it.
    subBlockLength = std::max (1, static_cast<int> (std::lround (0.1 * fs)));

    // BS.1770-4 channel weights (see ChannelWeights.h): only a >= 6 channel
    // stream is read as 5.1 / 7.1; the LFE is excluded and the side
    // surrounds get +1.5 dB (1.41), the 7.1 back pair 1.0.
    for (int c = 0; c < kMaxChannels; ++c)
        channelWeight[static_cast<size_t> (c)] = bs1770ChannelWeight (c, channels);

    integratedHistogram.prepare();
    rangeHistogram.prepare();
    reset();
}

void LoudnessMeter::reset() noexcept
{
    for (auto& s : stage1State)
        s.reset();
    for (auto& s : stage2State)
        s.reset();
    channelEnergy.fill (0.0);
    subBlockEnergy.fill (0.0);
    subBlockPos = 0;
    ringPos = 0;
    validSubBlocks = 0;
    momentaryLufs = kMinusInfDb;
    shortTermLufs = kMinusInfDb;
    resetMeasurement();
    measuredSubBlocks = 0;
}

void LoudnessMeter::resetIntegrated() noexcept
{
    resetMeasurement();
    // The new measurement only takes windows that lie entirely after the reset.
    // A partly accumulated sub-block still holds audio from before it, so it
    // is skipped (-1) - momentary/short-term themselves keep running.
    measuredSubBlocks = subBlockPos > 0 ? -1 : 0;
}

void LoudnessMeter::resetMeasurement() noexcept
{
    integratedHistogram.reset();
    rangeHistogram.reset();
    integratedLufs = kMinusInfDb;
    maxMomentaryLufs = kMinusInfDb;
    maxShortTermLufs = kMinusInfDb;
    loudnessRangeLu = 0.0f;
}

//==============================================================================
void LoudnessMeter::process (const AudioBlock& block) noexcept
{
    if (subBlockLength <= 0)
        return; // not prepared

    const int nch = std::min (block.numChannels, channels);
    int pos = 0;

    while (pos < block.numSamples)
    {
        // Run up to the next sub-block boundary. Each channel keeps one running
        // sum of squares that is extended sample by sample in stream order, so
        // the result is bit-identical for any host block size.
        const int n = std::min (block.numSamples - pos, subBlockLength - subBlockPos);

        for (int c = 0; c < nch; ++c)
        {
            const auto ci = static_cast<size_t> (c);
            if (channelWeight[ci] <= 0.0)
                continue; // LFE: not part of the measurement at all

            const float* x = block.channel (c) + pos;
            auto& s1 = stage1State[ci];
            auto& s2 = stage2State[ci];
            double acc = channelEnergy[ci];
            for (int i = 0; i < n; ++i)
            {
                const double k1 = biquadTick (stage1Coeffs, s1, static_cast<double> (x[i]) + kAntiDenormal);
                const double k2 = biquadTick (stage2Coeffs, s2, k1);
                acc += k2 * k2;
            }
            channelEnergy[ci] = acc;
        }

        pos += n;
        subBlockPos += n;
        if (subBlockPos >= subBlockLength)
            completeSubBlock();
    }
}

void LoudnessMeter::completeSubBlock() noexcept
{
    double weighted = 0.0;
    for (int c = 0; c < channels; ++c)
    {
        const auto ci = static_cast<size_t> (c);
        double e = channelEnergy[ci];
        if (! std::isfinite (e))
        {
            // NaN/Inf reached the input: drop this channel's sub-block and
            // restart its filters, so one bad sample cannot poison the meter
            // (the recursive states would otherwise stay NaN forever).
            e = 0.0;
            stage1State[ci].reset();
            stage2State[ci].reset();
        }
        weighted += channelWeight[ci] * e;
        channelEnergy[ci] = 0.0;
    }

    subBlockEnergy[static_cast<size_t> (ringPos)] = weighted;
    ringPos = (ringPos + 1) % kShortTermSubBlocks;
    subBlockPos = 0;
    validSubBlocks = std::min (validSubBlocks + 1, kShortTermSubBlocks);
    measuredSubBlocks = std::min (measuredSubBlocks + 1, kShortTermSubBlocks);

    // Mean square of the newest `count` sub-blocks (oldest first, fixed order).
    const auto windowEnergy = [this] (int count) noexcept
    {
        double sum = 0.0;
        for (int i = count; i >= 1; --i)
            sum += subBlockEnergy[static_cast<size_t> ((ringPos - i + kShortTermSubBlocks) % kShortTermSubBlocks)];
        return sum / (static_cast<double> (count) * static_cast<double> (subBlockLength));
    };

    // Momentary (400 ms). Every momentary window is also a BS.1770 gating block:
    // 400 ms long, a new one every 100 ms = 75 % overlap.
    if (validSubBlocks >= kMomentarySubBlocks)
    {
        const double e = windowEnergy (kMomentarySubBlocks);
        momentaryLufs = toReading (e);

        if (measuredSubBlocks >= kMomentarySubBlocks)
        {
            maxMomentaryLufs = std::max (maxMomentaryLufs, momentaryLufs);
            integratedHistogram.add (e);
            integratedLufs = toReading (integratedHistogram.gatedMeanEnergy (kIntegratedRelativeGateLu));
        }
    }

    // Short-term (3 s). LRA takes every short-term value, i.e. 3 s windows
    // with a 100 ms hop (Tech 3342 needs overlapping windows; libebur128 hops
    // 1 s - the finer hop only gives the statistics more samples).
    if (validSubBlocks >= kShortTermSubBlocks)
    {
        const double e = windowEnergy (kShortTermSubBlocks);
        shortTermLufs = toReading (e);

        if (measuredSubBlocks >= kShortTermSubBlocks)
        {
            maxShortTermLufs = std::max (maxShortTermLufs, shortTermLufs);
            rangeHistogram.add (e);
            double low = 0.0, high = 0.0;
            if (rangeHistogram.gatedPercentiles (kRangeRelativeGateLu, kRangeLowFraction, kRangeHighFraction, low, high))
                loudnessRangeLu = static_cast<float> (std::max (0.0, high - low));
        }
    }
}

//==============================================================================
float LoudnessMeter::getMomentaryLufs() const noexcept { return momentaryLufs; }
float LoudnessMeter::getShortTermLufs() const noexcept { return shortTermLufs; }
float LoudnessMeter::getIntegratedLufs() const noexcept { return integratedLufs; }
float LoudnessMeter::getLoudnessRangeLu() const noexcept { return loudnessRangeLu; }
float LoudnessMeter::getMaxMomentaryLufs() const noexcept { return maxMomentaryLufs; }
float LoudnessMeter::getMaxShortTermLufs() const noexcept { return maxShortTermLufs; }

//==============================================================================
void LoudnessMeter::GatingHistogram::prepare()
{
    coarse.assign (static_cast<size_t> (kNumCoarseBins), Bin {});
    fine.assign (static_cast<size_t> (kNumFineBins), Bin {});
    totalEnergy = 0.0;
    totalCount = 0;
    highestBin = -1;
}

void LoudnessMeter::GatingHistogram::reset() noexcept
{
    // Only the occupied range can be non-zero, which keeps a reset from the
    // audio thread cheap.
    const auto used = std::min (static_cast<size_t> (highestBin + 1), coarse.size());
    std::fill (coarse.begin(), coarse.begin() + static_cast<std::ptrdiff_t> (used), Bin {});
    std::fill (fine.begin(), fine.begin() + static_cast<std::ptrdiff_t> (used * kFinePerCoarse), Bin {});
    totalEnergy = 0.0;
    totalCount = 0;
    highestBin = -1;
}

void LoudnessMeter::GatingHistogram::add (double energy) noexcept
{
    if (coarse.empty() || ! (energy > 0.0))
        return;
    const double lufs = energyToLufs (energy);
    if (! (lufs > kAbsoluteGateLufs))
        return; // absolute gate

    const int f = fineBinIndex (lufs);
    const int c = f / kFinePerCoarse;
    auto& fb = fine[static_cast<size_t> (f)];
    auto& cb = coarse[static_cast<size_t> (c)];
    fb.energy += energy;
    ++fb.count;
    cb.energy += energy;
    ++cb.count;
    totalEnergy += energy;
    ++totalCount;
    highestBin = std::max (highestBin, c);
}

int LoudnessMeter::GatingHistogram::firstGatedBin (double relativeGateLu) const noexcept
{
    if (totalCount <= 0)
        return -1;

    // Relative gate in the energy domain: L > mean_L + gate  <=>  E > mean_E * 10^(gate/10).
    const double threshold = totalEnergy / static_cast<double> (totalCount) * std::pow (10.0, relativeGateLu / 10.0);
    const int t = fineBinIndex (energyToLufs (threshold));
    if (t / kFinePerCoarse > highestBin)
        return -1;

    // Every fine bin above t lies wholly above the threshold. Bin t straddles
    // it and is kept or dropped as a whole, decided by its exact mean energy:
    // only blocks within 0.01 LU of the gate can be misclassified (and for
    // steady material, whose blocks all sit at that mean, none are).
    const auto& b = fine[static_cast<size_t> (t)];
    if (b.count > 0 && b.energy / static_cast<double> (b.count) > threshold)
        return t;
    return (t + 1) / kFinePerCoarse <= highestBin ? t + 1 : -1;
}

LoudnessMeter::GatingHistogram::Bin LoudnessMeter::GatingHistogram::partialCoarse (int first) const noexcept
{
    Bin sum;
    const int end = (first / kFinePerCoarse + 1) * kFinePerCoarse;
    for (int f = first; f < end; ++f)
    {
        sum.energy += fine[static_cast<size_t> (f)].energy;
        sum.count += fine[static_cast<size_t> (f)].count;
    }
    return sum;
}

double LoudnessMeter::GatingHistogram::gatedMeanEnergy (double relativeGateLu) const noexcept
{
    const int first = firstGatedBin (relativeGateLu);
    if (first < 0)
        return 0.0;

    // The gated part of the straddling coarse bin from its fine bins, then
    // every coarse bin above it whole.
    Bin sum = partialCoarse (first);
    for (int c = first / kFinePerCoarse + 1; c <= highestBin; ++c)
    {
        sum.energy += coarse[static_cast<size_t> (c)].energy;
        sum.count += coarse[static_cast<size_t> (c)].count;
    }
    return sum.count > 0 ? sum.energy / static_cast<double> (sum.count) : 0.0;
}

bool LoudnessMeter::GatingHistogram::gatedPercentiles (double relativeGateLu, double lowFraction, double highFraction,
                                                       double& lowLufs, double& highLufs) const noexcept
{
    const int first = firstGatedBin (relativeGateLu);
    if (first < 0)
        return false;

    const int firstCoarse = first / kFinePerCoarse;
    const std::int64_t firstCount = partialCoarse (first).count;
    std::int64_t n = firstCount;
    for (int c = firstCoarse + 1; c <= highestBin; ++c)
        n += coarse[static_cast<size_t> (c)].count;
    if (n <= 0)
        return false;

    // Tech 3342 reference: value = sorted[round ((n - 1) * p)] (0-based). The
    // histogram is the sorted list in 0.01 LU steps; a rank is resolved to the
    // fine bin it falls into, represented by that bin's exact mean loudness.
    // Coarse bins are skipped whole until one contains the next rank.
    const double last = static_cast<double> (n - 1);
    const std::int64_t ranks[2] = { static_cast<std::int64_t> (std::llround (last * lowFraction)),
                                    static_cast<std::int64_t> (std::llround (last * highFraction)) };
    double* const outputs[2] = { &lowLufs, &highLufs };
    int next = 0;
    std::int64_t seen = 0;

    for (int c = firstCoarse; c <= highestBin && next < 2; ++c)
    {
        const std::int64_t count = c == firstCoarse ? firstCount : coarse[static_cast<size_t> (c)].count;
        if (seen + count <= ranks[next])
        {
            seen += count;
            continue;
        }

        const int end = (c + 1) * kFinePerCoarse;
        for (int f = c == firstCoarse ? first : c * kFinePerCoarse; f < end && next < 2; ++f)
        {
            const auto& b = fine[static_cast<size_t> (f)];
            if (b.count == 0)
                continue;
            seen += b.count;
            // log10 only for the (at most two) bins that resolve a rank.
            while (next < 2 && ranks[next] < seen)
                *outputs[next++] = energyToLufs (b.energy / static_cast<double> (b.count));
        }
    }
    return next == 2;
}
} // namespace flub
