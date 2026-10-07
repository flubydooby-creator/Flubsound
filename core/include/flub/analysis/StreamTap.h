// Flubsound Pro - a copy of a real-time audio stream for an analysing thread
// (docs/11 E53: the real-device soak).
//
// StreamTap carries what a real-time thread produced - the desktop app's
// output as its device callback hands it to the device, after the loopback
// guard and the output trim (what the device then does with it, e.g. an
// underrun after a late callback, is not seen) - to one reader thread, which
// analyses it (the DiscontinuityDetector, levels). Stereo, in chunks of up to kChunkFrames
// frames; every chunk carries its position in the stream (frames since
// prepare / reset, dropped ones included).
//
// The writer never waits. When the reader falls behind and the ring has no
// room for a chunk, the chunk is dropped and counted (framesDropped), and the
// reader sees the gap as a jump in the chunk positions: an analysis restarts
// there instead of reading the splice as a click.
//
// Channels: the first two of the writer's channels (one channel is copied to
// both; none, or a null channel pointer, is silence).
//
// Threads: ONE writer (write()) and ONE reader (read(), the counters from any
// thread). prepare() and reset() only while neither runs. write() is
// wait-free and allocation-free (FLUB_NONBLOCKING, tests/test_rtsan.cpp).
#pragma once

#include "flub/common/Realtime.h"
#include "flub/common/SpscRing.h"

#include <atomic>
#include <cstdint>

namespace flub
{
class StreamTap
{
public:
    static constexpr int kChunkFrames = 128;

    struct Chunk
    {
        int64_t frame = 0;     // stream position of the chunk's first frame
        int32_t numFrames = 0; // 1 .. kChunkFrames
        float samples[2 * kChunkFrames] {}; // interleaved: L R L R ...
    };

    /** Allocates a ring for at least `capacityFrames` frames (written in any
        block sizes) and resets. */
    void prepare (int capacityFrames);

    /** Back to frame 0, counters cleared, the ring emptied. Not while the
        writer or the reader runs. */
    void reset() noexcept;

    /** True once prepare() allocated the ring. */
    bool isPrepared() const noexcept { return capacityChunks > 0; }

    /** Writer: `numFrames` frames of `channels[0 .. numChannels)`. */
    void write (const float* const* channels, int numChannels, int numFrames) noexcept FLUB_NONBLOCKING;

    /** Reader: the next chunk; false when the ring is empty. */
    bool read (Chunk& chunk) noexcept;

    /** Frames the writer was handed (written + dropped): the stream position
        of the next frame. Any thread. */
    int64_t framesWritten() const noexcept { return written.load (std::memory_order_acquire); }
    /** Frames dropped because the ring was full. Any thread. */
    int64_t framesDropped() const noexcept { return dropped.load (std::memory_order_acquire); }
    /** Chunks the ring can hold. */
    int getCapacityChunks() const noexcept { return capacityChunks; }

private:
    SpscRing<Chunk> ring;
    int capacityChunks = 0;
    int64_t position = 0; // writer only: the next frame's stream position
    std::atomic<int64_t> written { 0 }, dropped { 0 };
};
} // namespace flub
