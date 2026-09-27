// Flubsound Pro - signal generators and measurement helpers for tests.
#pragma once

#include "flub/common/AudioBlock.h"
#include "flub/common/Math.h"
#include "flub/dsp/Processor.h"

#include <algorithm>
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

/** Pink (-3 dB/octave) noise, Paul Kellet's refined filter on seeded white
    noise, scaled to the given RMS over the whole buffer. Deterministic. */
inline std::vector<float> pinkNoise (int numSamples, float rmsLevel, uint32_t seed = 4321)
{
    flub::FastRandom rng (seed);
    std::vector<float> v (static_cast<size_t> (numSamples));
    double b0 = 0.0, b1 = 0.0, b2 = 0.0, b3 = 0.0, b4 = 0.0, b5 = 0.0, b6 = 0.0, acc = 0.0;
    for (auto& s : v)
    {
        const double w = rng.nextBipolar();
        b0 = 0.99886 * b0 + w * 0.0555179;
        b1 = 0.99332 * b1 + w * 0.0750759;
        b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856;
        b4 = 0.55000 * b4 + w * 0.5329522;
        b5 = -0.7616 * b5 - w * 0.0168980;
        const double p = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
        b6 = w * 0.115926;
        s = static_cast<float> (p);
        acc += p * p;
    }
    const double g = numSamples > 0 && acc > 0.0 ? rmsLevel / std::sqrt (acc / numSamples) : 0.0;
    for (auto& s : v)
        s = static_cast<float> (s * g);
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
        rebind();
    }

    // Copies must point at their OWN storage (a defaulted copy would keep the
    // source's channel pointers). Moves keep the vectors' buffers, so the
    // pointers stay valid.
    Planar (const Planar& other) : ch (other.ch) { rebind(); }
    Planar& operator= (const Planar& other)
    {
        if (this != &other)
        {
            ch = other.ch;
            rebind();
        }
        return *this;
    }
    Planar (Planar&&) noexcept = default;
    Planar& operator= (Planar&&) noexcept = default;

    void rebind()
    {
        ptrs.clear();
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
        std::copy (s.begin(), s.end(), c.begin()); // keep the storage Planar points at
    p.reset();
    processInBlocks (p, buf, 256);
    // Measure at the requested frequency over an integer number of periods where possible.
    const double outAmp = toneAmplitude (buf.ch[0].data() + settle, measure, freq, sampleRate);
    return toDb (outAmp / amplitude);
}
} // namespace flubtest
