// StreamTap (docs/11 E53, the real-device soak): the copy of the app's final
// device output that the soak's analysis reads on another thread. Chunks
// carry their stream position, a full ring drops whole chunks (counted, and
// visible to the reader as a jump), write() never allocates, and a writer
// and a reader on two threads lose and reorder nothing.
#include "TestFramework.h"

#include "flub/analysis/StreamTap.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using namespace flub;

TEST_CASE ("StreamTap: chunks carry their stream position; mono is copied to both sides, no channels are silence")
{
    StreamTap tap;
    tap.prepare (4096);
    REQUIRE (tap.isPrepared());

    std::vector<float> l (300), r (300);
    for (size_t i = 0; i < l.size(); ++i)
    {
        l[i] = static_cast<float> (i);
        r[i] = -static_cast<float> (i);
    }
    const std::array<const float*, 2> stereo { l.data(), r.data() };
    tap.write (stereo.data(), 2, 300); // 128 + 128 + 44
    const std::array<const float*, 1> mono { l.data() };
    tap.write (mono.data(), 1, 10);
    tap.write (nullptr, 0, 5);
    CHECK (tap.framesWritten() == 315);
    CHECK (tap.framesDropped() == 0);

    StreamTap::Chunk chunk;
    int64_t expected = 0;
    std::vector<std::array<float, 2>> frames;
    while (tap.read (chunk))
    {
        CHECK (chunk.frame == expected);
        CHECK (chunk.numFrames >= 1 && chunk.numFrames <= StreamTap::kChunkFrames);
        expected += chunk.numFrames;
        for (int i = 0; i < chunk.numFrames; ++i)
            frames.push_back ({ chunk.samples[2 * i], chunk.samples[2 * i + 1] });
    }
    REQUIRE (frames.size() == 315);
    CHECK (frames[0][0] == 0.0f && frames[0][1] == 0.0f);
    CHECK (frames[299][0] == 299.0f && frames[299][1] == -299.0f);
    CHECK (frames[300][0] == 0.0f && frames[300][1] == 0.0f);     // mono: both sides
    CHECK (frames[309][0] == 9.0f && frames[309][1] == 9.0f);
    CHECK (frames[310][0] == 0.0f && frames[314][1] == 0.0f);     // silence
}

TEST_CASE ("StreamTap: a full ring drops whole chunks, counted, and the reader sees the gap as a jump in position")
{
    StreamTap tap;
    tap.prepare (StreamTap::kChunkFrames); // the smallest ring: a few chunks
    const int capacity = tap.getCapacityChunks();
    REQUIRE (capacity >= 2);

    std::vector<float> block (StreamTap::kChunkFrames, 0.25f);
    const std::array<const float*, 2> ch { block.data(), block.data() };
    const int writes = capacity + 5; // nothing is read meanwhile
    for (int w = 0; w < writes; ++w)
        tap.write (ch.data(), 2, StreamTap::kChunkFrames);

    const int64_t total = static_cast<int64_t> (writes) * StreamTap::kChunkFrames;
    CHECK (tap.framesWritten() == total);
    CHECK (tap.framesDropped() > 0);

    // The ring keeps the oldest chunks; after a read the writer goes on, and
    // the next chunk's position jumps over everything dropped.
    StreamTap::Chunk chunk;
    int64_t read = 0, expected = 0;
    while (tap.read (chunk))
    {
        CHECK (chunk.frame == expected);
        expected += chunk.numFrames;
        read += chunk.numFrames;
    }
    CHECK (read + tap.framesDropped() == total);
    tap.write (ch.data(), 2, 16);
    REQUIRE (tap.read (chunk));
    CHECK (chunk.frame == total);
    CHECK (chunk.frame - expected == tap.framesDropped()); // the jump is the gap

    tap.reset();
    CHECK (tap.framesWritten() == 0);
    CHECK (tap.framesDropped() == 0);
    CHECK (! tap.read (chunk));
}

TEST_CASE ("StreamTap: write() allocates nothing (an unprepared tap drops everything)")
{
    StreamTap tap;
    tap.prepare (48000);
    std::vector<float> block (1024, 0.1f);
    const std::array<const float*, 2> ch { block.data(), block.data() };

    flubtest::AllocationGuard guard;
    for (int i = 0; i < 64; ++i)
        tap.write (ch.data(), 2, 1024);
    tap.write (ch.data(), 2, 0);
    tap.write (nullptr, 0, 512);
    CHECK (guard.allocations() == 0);

    StreamTap unprepared;
    unprepared.write (ch.data(), 2, 100);
    CHECK (unprepared.framesWritten() == 100);
    CHECK (unprepared.framesDropped() == 100);
    StreamTap::Chunk chunk;
    CHECK (! unprepared.read (chunk));
}

TEST_CASE ("StreamTap: a writer and a reader on two threads: every frame read or counted dropped, in order, intact")
{
    StreamTap tap;
    tap.prepare (8192);
    constexpr int kBlock = 300; // not a multiple of the chunk size
    constexpr int kBlocks = 4000; // 1.2 M frames

    std::atomic<bool> done { false };
    std::thread writer ([&]
    {
        std::vector<float> l (kBlock), r (kBlock);
        int64_t pos = 0;
        for (int b = 0; b < kBlocks; ++b)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                // Exactly representable positions (< 2^24) on the left, a tag on the right.
                l[static_cast<size_t> (i)] = static_cast<float> ((pos + i) % (1 << 23));
                r[static_cast<size_t> (i)] = 1.0f;
            }
            const std::array<const float*, 2> ch { l.data(), r.data() };
            tap.write (ch.data(), 2, kBlock);
            pos += kBlock;
            if ((b & 31) == 0)
                std::this_thread::yield();
        }
        done.store (true, std::memory_order_release);
    });

    int64_t read = 0, last = -1, gaps = 0;
    bool intact = true, ordered = true;
    StreamTap::Chunk chunk;
    for (;;)
    {
        const bool finished = done.load (std::memory_order_acquire);
        bool any = false;
        while (tap.read (chunk))
        {
            any = true;
            if (chunk.frame < last)
                ordered = false;
            if (last >= 0 && chunk.frame != last)
                ++gaps;
            for (int i = 0; i < chunk.numFrames; ++i)
                if (chunk.samples[2 * i] != static_cast<float> ((chunk.frame + i) % (1 << 23)) || chunk.samples[2 * i + 1] != 1.0f)
                    intact = false;
            read += chunk.numFrames;
            last = chunk.frame + chunk.numFrames;
        }
        if (finished && ! any)
            break;
        if (! any)
            std::this_thread::yield();
    }
    writer.join();

    const int64_t total = static_cast<int64_t> (kBlock) * kBlocks;
    CHECK (tap.framesWritten() == total);
    CHECK (read + tap.framesDropped() == total);
    CHECK (ordered);
    CHECK (intact);
    CHECK (gaps == 0 || tap.framesDropped() > 0); // a jump only where frames were dropped
}
