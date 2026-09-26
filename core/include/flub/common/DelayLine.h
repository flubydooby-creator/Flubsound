// Flubsound Pro - fixed multichannel delay line.
//
// Used for look-ahead (limiter/compressor), dry-path latency compensation in
// bypass crossfades, and the global bypass path. The delay is fixed at
// prepare() time: changing latency at runtime would shift audio in time and
// click, so latency-affecting settings are "structural" (see ProcessingChain).
#pragma once

#include "AudioBlock.h"

#include <array>
#include <vector>

namespace flub
{
class DelayLine
{
public:
    /** Allocates. delaySamples may be 0 (pass-through). */
    void prepare (int numChannels, int delaySamples)
    {
        channels = numChannels;
        delay = std::max (0, delaySamples);
        size = delay + 1;
        for (int c = 0; c < kMaxChannels; ++c)
            lines[static_cast<size_t> (c)].assign (c < numChannels ? static_cast<size_t> (size) : 0u, 0.0f);
        writePos = 0;
    }

    void reset() noexcept
    {
        for (auto& l : lines)
            std::fill (l.begin(), l.end(), 0.0f);
        writePos = 0;
    }

    int getDelay() const noexcept { return delay; }

    /** Delay the block in place. */
    void process (const AudioBlock& block) noexcept
    {
        if (delay == 0)
            return;
        const int nch = std::min (block.numChannels, channels);
        int pos = writePos;
        for (int c = 0; c < nch; ++c)
        {
            float* line = lines[static_cast<size_t> (c)].data();
            float* d = block.channel (c);
            pos = writePos;
            for (int i = 0; i < block.numSamples; ++i)
            {
                line[pos] = d[i];
                int readPos = pos - delay;
                if (readPos < 0)
                    readPos += size;
                d[i] = line[readPos];
                if (++pos == size)
                    pos = 0;
            }
        }
        writePos = pos;
    }

    /** Single-sample access for per-sample algorithms: push x, return x delayed. */
    float processSample (int ch, float x) noexcept
    {
        if (delay == 0)
            return x;
        float* line = lines[static_cast<size_t> (ch)].data();
        line[writePos] = x;
        int readPos = writePos - delay;
        if (readPos < 0)
            readPos += size;
        return line[readPos];
    }

    /** Advance the shared write head after calling processSample() on every channel. */
    void advance() noexcept
    {
        if (delay > 0 && ++writePos == size)
            writePos = 0;
    }

private:
    std::array<std::vector<float>, kMaxChannels> lines;
    int channels = 0, delay = 0, size = 1, writePos = 0;
};
} // namespace flub
