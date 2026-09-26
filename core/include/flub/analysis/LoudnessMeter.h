// Flubsound Pro - ITU-R BS.1770-4 / EBU R128 loudness meter.
//
// K-weighting: two biquads (high-shelf "pre-filter" + RLB high-pass), with
// coefficients derived from the analog prototype so any sample rate is
// exact (matches the BS.1770 48 kHz table to ~1e-14):
//   stage 1: f0 = 1681.974450955533 Hz, G = 3.999843853973347 dB,
//            Q = 0.7071752369554196
//   stage 2: f0 = 38.13547087602444 Hz, Q = 0.5003270373238773
// Loudness = -0.691 + 10 log10( sum_i G_i * meanSquare_i )
//   G per ChannelWeights.h: 1.0 (L, R, C, 7.1 back pair), 1.41 (side
//   surrounds), LFE excluded (index 3 for 5.1/7.1).
// Windows are built from 100 ms sub-blocks of channel-weighted energy:
//   momentary  = 400 ms (4 sub-blocks), short-term = 3 s (30 sub-blocks)
//   integrated = gated (EBU R128): 400 ms blocks, 75 % overlap; absolute gate
//                -70 LUFS; relative gate -10 LU below the absolute-gated
//                mean. Implemented with a two-level histogram (0.1 LU coarse,
//                0.01 LU fine bins, each with its energy sum + count) -> O(1)
//                memory for arbitrarily long programmes.
//   LRA (EBU Tech 3342): short-term values with absolute gate -70 and relative
//                gate -20 LU; LRA = P95 - P10, same histogram technique.
// process() only reads the block (const input), never modifies it.
#pragma once

#include "flub/common/AudioBlock.h"
#include "flub/dsp/Biquad.h"

#include <array>
#include <vector>

namespace flub
{
class LoudnessMeter
{
public:
    static BiquadCoeffs kWeightingStage1 (double sampleRate) noexcept;
    static BiquadCoeffs kWeightingStage2 (double sampleRate) noexcept;

    /** Allocates. numChannels 1..8; channels 3 (LFE) and 4+ (surround weight
        1.41) are interpreted as 5.1/7.1 only when numChannels >= 6. */
    void prepare (double sampleRate, int numChannels);
    void reset() noexcept;              // everything
    void resetIntegrated() noexcept;    // integrated + LRA + max values only

    void process (const AudioBlock& block) noexcept;

    float getMomentaryLufs() const noexcept;   // -inf -> kMinusInfDb
    float getShortTermLufs() const noexcept;
    float getIntegratedLufs() const noexcept;  // gated
    float getLoudnessRangeLu() const noexcept;
    float getMaxMomentaryLufs() const noexcept;
    float getMaxShortTermLufs() const noexcept;

private:
    // ---- implementation-defined below this line ----
    static constexpr int kMomentarySubBlocks = 4;   // 400 ms
    static constexpr int kShortTermSubBlocks = 30;  // 3 s (also the ring length)

    /** Gating histogram from -70 LUFS (the absolute gate) to +30 LUFS, louder
        blocks clamped into the top bin. Two levels: 0.1 LU coarse bins keep
        the scans short (<= 1000 bins), and 0.01 LU fine bins resolve the bin
        that straddles the relative gate and the bins that hold the LRA
        percentile ranks. Every bin keeps the exact energy sum and count of its
        blocks, so the gated mean is exact except for blocks within 0.01 LU of
        the relative gate, and memory stays constant for any programme length. */
    class GatingHistogram
    {
    public:
        void prepare();
        void reset() noexcept;

        /** Adds one block (mean-square energy already channel-weighted);
            blocks at or below the -70 LUFS absolute gate are discarded. */
        void add (double energy) noexcept;

        /** Mean energy of the blocks that pass the absolute gate and the
            relative gate (relativeGateLu below the absolute-gated mean);
            0 when nothing passes. */
        double gatedMeanEnergy (double relativeGateLu) const noexcept;

        /** EBU Tech 3342 percentiles (in LUFS) of the gated distribution.
            Returns false when no block passes the gates. */
        bool gatedPercentiles (double relativeGateLu, double lowFraction, double highFraction,
                               double& lowLufs, double& highLufs) const noexcept;

    private:
        struct Bin
        {
            double energy = 0.0;
            std::int64_t count = 0;
        };

        /** First fine bin that passes the relative gate (-1 = nothing passes). */
        int firstGatedBin (double relativeGateLu) const noexcept;

        /** Energy / count of the gated part of the coarse bin that contains
            fine bin `first` (fine bins from `first` to the coarse bin's end). */
        Bin partialCoarse (int first) const noexcept;

        std::vector<Bin> coarse, fine;
        double totalEnergy = 0.0; // absolute-gated sums (for the relative gate)
        std::int64_t totalCount = 0;
        int highestBin = -1;      // highest occupied coarse bin: bounds the scans
    };

    void completeSubBlock() noexcept;
    void resetMeasurement() noexcept;

    double fs = 48000.0;
    int channels = 2;
    int subBlockLength = 0;       // samples per 100 ms sub-block (0 = not prepared)
    int subBlockPos = 0;          // samples accumulated in the current sub-block

    BiquadCoeffs stage1Coeffs, stage2Coeffs;
    std::array<BiquadState, kMaxChannels> stage1State {}, stage2State {};
    std::array<double, kMaxChannels> channelWeight {};
    std::array<double, kMaxChannels> channelEnergy {}; // running sum of squares, current sub-block

    std::array<double, kShortTermSubBlocks> subBlockEnergy {}; // ring of weighted sub-block energies
    int ringPos = 0;
    int validSubBlocks = 0;       // complete sub-blocks since reset(), capped at the ring length
    int measuredSubBlocks = 0;    // ... since the measurement (re)started, capped likewise

    GatingHistogram integratedHistogram; // 400 ms blocks, 75 % overlap
    GatingHistogram rangeHistogram;      // 3 s short-term values every 100 ms

    float momentaryLufs = kMinusInfDb, shortTermLufs = kMinusInfDb, integratedLufs = kMinusInfDb;
    float maxMomentaryLufs = kMinusInfDb, maxShortTermLufs = kMinusInfDb;
    float loudnessRangeLu = 0.0f;
};
} // namespace flub
