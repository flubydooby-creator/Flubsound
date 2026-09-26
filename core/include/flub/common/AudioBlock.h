// Flubsound Pro - non-owning multichannel audio view plus an owning buffer.
//
// All DSP processes in place on an AudioBlock. AudioBlock is a small value type
// (array of channel pointers), so sub-blocks can be taken without allocation.
// AudioBuffer owns memory and may only be resized from prepare().
#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <vector>

namespace flub
{
/** Hard upper bound on channels anywhere in the engine (7.1 = 8). */
inline constexpr int kMaxChannels = 8;

struct AudioBlock
{
    std::array<float*, kMaxChannels> ch {};
    int numChannels = 0;
    int numSamples = 0;

    AudioBlock() = default;

    AudioBlock (float* const* data, int channels, int samples, int offset = 0) noexcept
        : numChannels (std::min (channels, kMaxChannels)), numSamples (samples)
    {
        assert (channels <= kMaxChannels);
        for (int c = 0; c < numChannels; ++c)
            ch[static_cast<size_t> (c)] = data[c] + offset;
    }

    float* channel (int c) const noexcept { return ch[static_cast<size_t> (c)]; }

    AudioBlock subBlock (int offset, int length) const noexcept
    {
        assert (offset >= 0 && offset + length <= numSamples);
        AudioBlock b;
        b.numChannels = numChannels;
        b.numSamples = length;
        for (int c = 0; c < numChannels; ++c)
            b.ch[static_cast<size_t> (c)] = ch[static_cast<size_t> (c)] + offset;
        return b;
    }

    /** View restricted to the first n channels. */
    AudioBlock firstChannels (int n) const noexcept
    {
        AudioBlock b = *this;
        b.numChannels = std::min (n, numChannels);
        return b;
    }

    void clear() const noexcept
    {
        for (int c = 0; c < numChannels; ++c)
            std::memset (channel (c), 0, sizeof (float) * static_cast<size_t> (numSamples));
    }

    void copyFrom (const AudioBlock& src) const noexcept
    {
        const int n = std::min (numSamples, src.numSamples);
        for (int c = 0; c < std::min (numChannels, src.numChannels); ++c)
            if (channel (c) != src.channel (c))
                std::memcpy (channel (c), src.channel (c), sizeof (float) * static_cast<size_t> (n));
    }

    void applyGain (float g) const noexcept
    {
        for (int c = 0; c < numChannels; ++c)
        {
            float* d = channel (c);
            for (int i = 0; i < numSamples; ++i)
                d[i] *= g;
        }
    }

    /** Linear gain ramp from g0 to g1 across the block (click-free gain change). */
    void applyGainRamp (float g0, float g1) const noexcept
    {
        if (g0 == g1)
        {
            if (g0 != 1.0f)
                applyGain (g0);
            return;
        }
        const float step = (g1 - g0) / static_cast<float> (std::max (1, numSamples));
        for (int c = 0; c < numChannels; ++c)
        {
            float* d = channel (c);
            float g = g0;
            for (int i = 0; i < numSamples; ++i, g += step)
                d[i] *= g;
        }
    }
};

class AudioBuffer
{
public:
    AudioBuffer() = default;
    AudioBuffer (int channels, int samples) { setSize (channels, samples); }

    /** Allocates. Never call from the audio thread. */
    void setSize (int channels, int samples)
    {
        assert (channels <= kMaxChannels);
        numChannels = channels;
        numSamples = samples;
        storage.assign (static_cast<size_t> (channels) * static_cast<size_t> (samples), 0.0f);
        for (int c = 0; c < kMaxChannels; ++c)
            pointers[static_cast<size_t> (c)] = c < channels ? storage.data() + static_cast<size_t> (c) * static_cast<size_t> (samples) : nullptr;
    }

    int getNumChannels() const noexcept { return numChannels; }
    int getNumSamples() const noexcept { return numSamples; }
    float* channel (int c) noexcept { return pointers[static_cast<size_t> (c)]; }
    const float* channel (int c) const noexcept { return pointers[static_cast<size_t> (c)]; }

    AudioBlock block() noexcept { return AudioBlock (pointers.data(), numChannels, numSamples); }
    AudioBlock block (int channels, int samples) noexcept
    {
        assert (channels <= numChannels && samples <= numSamples);
        return AudioBlock (pointers.data(), channels, samples);
    }

    void clear() noexcept { std::fill (storage.begin(), storage.end(), 0.0f); }

private:
    std::vector<float> storage;
    std::array<float*, kMaxChannels> pointers {};
    int numChannels = 0, numSamples = 0;
};
} // namespace flub
