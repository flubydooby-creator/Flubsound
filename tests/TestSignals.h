// Flubsound Pro - signal generators and measurement helpers for tests.
#pragma once

#include "flub/common/AudioBlock.h"
#include "flub/common/Math.h"
#include "flub/dsp/Processor.h"

#include <cmath>
#include <functional>
#include <vector>

namespace flubtest
{
inline std::vector<float> sine (double freq, double sampleRate, int numSamples, float amplitude = 1.0f, double phase = 0.0)
{
    std::vector<float> v (static_cast<size_t> (numSamples));
    for (int i = 0; i < numSamples; ++i)
        v[static_cast<size_t> (i)] = amplitude * static_cast<float> (std::sin (flub::kTwoPi * freq * i / sampleRate + phase));
    return v;
}

inline std::vector<float> whiteNoise (int numSamples, float amplitude = 1.0f, uint32_t seed = 1234)
{
    flub::FastRandom rng (seed);
    std::vector<float> v (static_cast<size_t> (numSamples));
    for (auto& s : v)
        s = amplitude * rng.nextBipolar();
    return v;
}

inline double rms (const float* x, int n)
{
    double acc = 0.0;
    for (int i = 0; i < n; ++i)
        acc += static_cast<double> (x[i]) * x[i];
    return n > 0 ? std::sqrt (acc / n) : 0.0;
}

inline double peakAbs (const float* x, int n)
{
    double p = 0.0;
    for (int i = 0; i < n; ++i)
        p = std::max (p, static_cast<double> (std::abs (x[i])));
    return p;
}

inline double toDb (double linear) { return 20.0 * std::log10 (std::max (1.0e-12, linear)); }

/** Amplitude of the component at `freq` (single-bin DFT / Goertzel). */
inline double toneAmplitude (const float* x, int n, double freq, double sampleRate)
{
    double re = 0.0, im = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double a = flub::kTwoPi * freq * i / sampleRate;
        re += x[i] * std::cos (a);
        im -= x[i] * std::sin (a);
    }
    return 2.0 * std::sqrt (re * re + im * im) / n;
}

/** Stereo/multichannel planar buffer helper. */
struct Planar
{
    std::vector<std::vector<float>> ch;
    std::vector<float*> ptrs;

    Planar (int numChannels, int numSamples)
        : ch (static_cast<size_t> (numChannels), std::vector<float> (static_cast<size_t> (numSamples), 0.0f))
    {
        for (auto& c : ch)
            ptrs.push_back (c.data());
    }

    int numChannels() const { return static_cast<int> (ch.size()); }
    int numSamples() const { return ch.empty() ? 0 : static_cast<int> (ch[0].size()); }
    flub::AudioBlock block() { return flub::AudioBlock (ptrs.data(), numChannels(), numSamples()); }
    flub::AudioBlock block (int offset, int length) { return flub::AudioBlock (ptrs.data(), numChannels(), length, offset); }
};

/** Runs a processor over a whole planar buffer in blocks of blockSize. */
inline void processInBlocks (flub::Processor& p, Planar& buf, int blockSize)
{
    const int n = buf.numSamples();
    for (int pos = 0; pos < n; pos += blockSize)
    {
        const int len = std::min (blockSize, n - pos);
        p.process (buf.block (pos, len));
    }
}

/** Steady-state gain (dB) of a processor for a sine at `freq` (mono or all
    channels fed identically), measured after a settling time. */
inline double measureGainDb (flub::Processor& p, double freq, double sampleRate, int numChannels = 2, float amplitude = 0.1f)
{
    const int settle = static_cast<int> (sampleRate * 0.5);
    const int measure = static_cast<int> (sampleRate * 0.5);
    const int total = settle + measure;
    Planar buf (numChannels, total);
    auto s = sine (freq, sampleRate, total, amplitude);
    for (auto& c : buf.ch)
        c = s;
    p.reset();
    processInBlocks (p, buf, 256);
    // Measure at the requested frequency over an integer number of periods where possible.
    const double outAmp = toneAmplitude (buf.ch[0].data() + settle, measure, freq, sampleRate);
    return toDb (outAmp / amplitude);
}
} // namespace flubtest
