#include "flub/analysis/StreamTap.h"

#include <algorithm>
#include <cstddef>

namespace flub
{
void StreamTap::prepare (int capacityFrames)
{
    // Every write() may end in a partial chunk, so a ring of whole chunks
    // holds at least capacityFrames frames when each chunk is half full.
    const int frames = std::max (capacityFrames, kChunkFrames);
    ring.allocate (static_cast<size_t> (2 * ((frames + kChunkFrames - 1) / kChunkFrames) + 2));
    capacityChunks = static_cast<int> (ring.capacity());
    reset();
}

void StreamTap::reset() noexcept
{
    Chunk discard;
    while (ring.pop (discard))
    {
    }
    position = 0;
    written.store (0, std::memory_order_release);
    dropped.store (0, std::memory_order_release);
}

void StreamTap::write (const float* const* channels, int numChannels, int numFrames) noexcept FLUB_NONBLOCKING
{
    if (numFrames <= 0)
        return;

    const float* left = channels != nullptr && numChannels > 0 ? channels[0] : nullptr;
    const float* right = channels != nullptr && numChannels > 1 ? channels[1] : left;

    int64_t lost = 0;
    Chunk chunk;
    for (int start = 0; start < numFrames; start += kChunkFrames)
    {
        const int n = std::min (kChunkFrames, numFrames - start);
        chunk.frame = position;
        chunk.numFrames = n;
        for (int i = 0; i < n; ++i)
        {
            chunk.samples[2 * i] = left != nullptr ? left[start + i] : 0.0f;
            chunk.samples[2 * i + 1] = right != nullptr ? right[start + i] : 0.0f;
        }
        position += n;
        if (capacityChunks <= 0 || ! ring.push (chunk))
            lost += n; // the reader is behind: the gap shows in the positions
    }

    // Single writer: a relaxed load + store is enough.
    if (lost > 0)
        dropped.store (dropped.load (std::memory_order_relaxed) + lost, std::memory_order_release);
    written.store (position, std::memory_order_release);
}

bool StreamTap::read (Chunk& chunk) noexcept
{
    return capacityChunks > 0 && ring.pop (chunk);
}
} // namespace flub
