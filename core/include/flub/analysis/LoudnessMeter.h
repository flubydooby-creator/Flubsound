// Flubsound Pro - ITU-R BS.1770-4 / EBU R128 loudness meter.
//
// K-weighting: two biquads (high-shelf "pre-filter" + RLB high-pass), with
// coefficients derived from the analog prototype so any sample rate is
// exact (matches the BS.1770 48 kHz table to ~1e-8):
//   stage 1: f0 = 1681.974450955533 Hz, G = 3.999843853973347 dB,
//            Q = 0.7071752369554196
//   stage 2: f0 = 38.13547087602444 Hz, Q = 0.5003270373238773
// Loudness = -0.691 + 10 log10( sum_i G_i * meanSquare_i )
//   G = 1.0 (L, R, C), 1.41 (surrounds), LFE excluded (index 3 for 5.1/7.1).
// Windows are built from 100 ms sub-blocks of channel-weighted energy:
//   momentary  = 400 ms (4 sub-blocks), short-term = 3 s (30 sub-blocks)
//   integrated = gated (EBU R128): 400 ms blocks, 75 % overlap; absolute gate
//                -70 LUFS; relative gate -10 LU below the absolute-gated
//                mean. Implemented with a 0.1 LU histogram (bin energy sums +
//                counts) -> O(1) memory for arbitrarily long programmes.
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
    double sampleRate = 48000.0;
    int channels = 2;
};
} // namespace flub
