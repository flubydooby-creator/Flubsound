// Flubsound Pro - true-peak, sample-peak, RMS and stereo correlation meters.
//
// TruePeakMeter : per-channel max of the 4x TruePeakDetector since reset,
//                 plus the max over the most recent block (for bar meters).
// LevelMeter    : per channel - sample peak (last block), RMS with 300 ms
//                 integration (IEC 60268-10-like ballistics), and for the
//                 first two channels the phase correlation
//                 r = E[LR] / sqrt(E[L^2] E[R^2]) over 300 ms (+1 mono,
//                 0 uncorrelated/wide, -1 out of phase).
// Both only read the block.
#pragma once

#include "flub/common/AudioBlock.h"
#include "flub/common/Math.h"
#include "flub/dsp/EnvelopeFollower.h"
#include "flub/dsp/TruePeakDetector.h"

#include <array>

namespace flub
{
class TruePeakMeter
{
public:
    void prepare (int numChannels)
    {
        channels = std::min (numChannels, kMaxChannels);
        detector.prepare (channels);
        reset();
    }

    void reset() noexcept
    {
        detector.reset();
        maxHold.fill (0.0f);
        lastBlock.fill (0.0f);
    }

    void process (const AudioBlock& block) noexcept
    {
        const int nch = std::min (block.numChannels, channels);
        for (int c = 0; c < nch; ++c)
        {
            const float* d = block.channel (c);
            float blockMax = 0.0f;
            for (int i = 0; i < block.numSamples; ++i)
                blockMax = std::max (blockMax, detector.processSample (c, d[i]));
            lastBlock[static_cast<size_t> (c)] = blockMax;
            maxHold[static_cast<size_t> (c)] = std::max (maxHold[static_cast<size_t> (c)], blockMax);
        }
    }

    float getMaxDb (int ch) const noexcept { return gainToDb (maxHold[static_cast<size_t> (ch)]); }
    float getBlockDb (int ch) const noexcept { return gainToDb (lastBlock[static_cast<size_t> (ch)]); }

    float getMaxDbAllChannels() const noexcept
    {
        float m = 0.0f;
        for (int c = 0; c < channels; ++c)
            m = std::max (m, maxHold[static_cast<size_t> (c)]);
        return gainToDb (m);
    }

private:
    int channels = 2;
    TruePeakDetector detector;
    std::array<float, kMaxChannels> maxHold {}, lastBlock {};
};

class LevelMeter
{
public:
    void prepare (double sampleRate, int numChannels)
    {
        channels = std::min (numChannels, kMaxChannels);
        for (auto& f : rms)
            f.prepare (sampleRate, 300.0f);
        corrLR.prepare (sampleRate, 300.0f);
        corrLL.prepare (sampleRate, 300.0f);
        corrRR.prepare (sampleRate, 300.0f);
        reset();
    }

    void reset() noexcept
    {
        for (auto& f : rms)
            f.reset();
        corrLR.reset();
        corrLL.reset();
        corrRR.reset();
        peak.fill (0.0f);
    }

    void process (const AudioBlock& block) noexcept
    {
        const int nch = std::min (block.numChannels, channels);
        for (int c = 0; c < nch; ++c)
        {
            const float* d = block.channel (c);
            float pk = 0.0f;
            for (int i = 0; i < block.numSamples; ++i)
            {
                pk = std::max (pk, std::abs (d[i]));
                rms[static_cast<size_t> (c)].process (static_cast<double> (d[i]) * d[i]);
            }
            peak[static_cast<size_t> (c)] = pk;
        }
        if (nch >= 2)
        {
            const float* l = block.channel (0);
            const float* r = block.channel (1);
            for (int i = 0; i < block.numSamples; ++i)
            {
                corrLR.process (static_cast<double> (l[i]) * r[i]);
                corrLL.process (static_cast<double> (l[i]) * l[i]);
                corrRR.process (static_cast<double> (r[i]) * r[i]);
            }
        }
    }

    float getPeakDb (int ch) const noexcept { return gainToDb (peak[static_cast<size_t> (ch)]); }
    float getRmsDb (int ch) const noexcept { return powerToDb (static_cast<float> (rms[static_cast<size_t> (ch)].get())); }

    float getCorrelation() const noexcept
    {
        const double den = std::sqrt (corrLL.get() * corrRR.get());
        return den > 1.0e-12 ? static_cast<float> (std::clamp (corrLR.get() / den, -1.0, 1.0)) : 1.0f;
    }

private:
    int channels = 2;
    std::array<MeanSquareFollower, kMaxChannels> rms;
    MeanSquareFollower corrLR, corrLL, corrRR;
    std::array<float, kMaxChannels> peak {};
};
} // namespace flub
