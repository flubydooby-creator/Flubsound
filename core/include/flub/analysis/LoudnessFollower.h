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
        const int nch = std::min (block.numChannels, channels);
        for (int i = 0; i < block.numSamples; ++i)
        {
            double sum = 0.0;
            for (int c = 0; c < nch; ++c)
            {
                if (nch >= 6 && c == 3)
                    continue; // LFE
                const double w = (nch >= 6 && c >= 4) ? 1.41 : 1.0;
                const double k = stage2.processSample (c, stage1.processSample (c, block.channel (c)[i]));
                sum += w * k * k;
            }
            ms.process (sum);
        }
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
