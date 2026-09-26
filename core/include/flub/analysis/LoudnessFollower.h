// Flubsound Pro - lightweight running loudness estimate (for control loops).
//
// K-weighted, channel-weighted mean square integrated by a one-pole with a
// configurable time constant (default 3 s ~ "short-term"). Unlike
// LoudnessMeter it has no gating or histograms: it is the cheap signal used
// by AutoGain (input levelling), loudness-matched A/B and the SafetyGovernor.
// A silence gate reports isActive() == false below -60 LUFS so control
// loops freeze instead of chasing silence.
#pragma once

#include "LoudnessMeter.h"
#include "flub/dsp/EnvelopeFollower.h"

namespace flub
{
class LoudnessFollower
{
public:
    void prepare (double sampleRate, int numChannels, float timeConstantMs = 3000.0f)
    {
        channels = std::min (numChannels, kMaxChannels);
        const auto s1 = LoudnessMeter::kWeightingStage1 (sampleRate);
        const auto s2 = LoudnessMeter::kWeightingStage2 (sampleRate);
        stage1.setCoeffs (s1);
        stage2.setCoeffs (s2);
        ms.prepare (sampleRate, timeConstantMs);
        reset();
    }

    void reset() noexcept
    {
        stage1.reset();
        stage2.reset();
        ms.reset();
    }

    void process (const AudioBlock& block) noexcept
    {
        // Two tiny offsets keep every recursive state normal during digital
        // silence even without FTZ/DAZ: a -400 dBFS DC at the filter input (the
        // RLB high-pass removes it, but the biquad states settle at ~1e-20
        // instead of decaying into subnormals), and a -600 dB floor on the mean
        // square (far below the 1e-20 reporting floor of getLufs()).
        constexpr double antiDenormal = 1.0e-20;
        constexpr double msFloor = 1.0e-60;

        const int nch = std::min (block.numChannels, channels);
        for (int i = 0; i < block.numSamples; ++i)
        {
            double sum = msFloor;
            for (int c = 0; c < nch; ++c)
            {
                if (nch >= 6 && c == 3)
                    continue; // LFE
                const double w = (nch >= 6 && c >= 4) ? 1.41 : 1.0;
                const double x = static_cast<double> (block.channel (c)[i]) + antiDenormal;
                const double k = stage2.processSample (c, stage1.processSample (c, x));
                sum += w * k * k;
            }
            ms.process (sum);
        }

        // A NaN/Inf input sample would otherwise leave the filter states and
        // the mean square NaN forever (getLufs() stuck at -inf, the control
        // loops frozen). Any non-finite value reaches ms within the same
        // sample, so one check per block is enough to restart cleanly.
        if (! std::isfinite (ms.get()))
            reset();
    }

    float getLufs() const noexcept
    {
        const double e = ms.get();
        return e > 1.0e-20 ? static_cast<float> (-0.691 + 10.0 * std::log10 (e)) : kMinusInfDb;
    }

    bool isActive() const noexcept { return getLufs() > -60.0f; }

private:
    int channels = 2;
    Biquad stage1, stage2;
    MeanSquareFollower ms;
};
} // namespace flub
