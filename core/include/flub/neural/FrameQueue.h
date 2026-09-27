// Flubsound Pro - wait-free single-producer / single-consumer queue of
// fixed-size float frames, each with a small header.
//
// SpscRing carries a stream of trivially copyable items; a model frame is
// thousands of floats that must travel as one unit together with its sequence
// number, so AsyncModelProcessor uses this instead: the producer fills a slot
// in place (beginWrite / commitWrite), the consumer reads it in place
// (peek / release), and no frame is ever split or copied twice. Capacity is
// rounded up to a power of two; head and tail are free-running counters on
// separate cache lines.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace flub
{
#if defined(_MSC_VER)
    #pragma warning(push)
    #pragma warning(disable : 4324) // padding from alignas(64) is the point: head/tail on separate cache lines
#endif

class FrameQueue
{
public:
    struct Header
    {
        uint64_t seq = 0;          // frame number (monotonic for the queue's owner)
        bool ok = true;            // result frames: the model succeeded
        bool discontinuity = false;// input frames: first frame after prepare() / reset()
    };

    /** Allocates; call before the queue is shared between threads. */
    void allocate (size_t minFrames, size_t floatsPerFrame)
    {
        size_t cap = 1;
        while (cap < minFrames)
            cap <<= 1;
        frameFloats = floatsPerFrame;
        mask = cap - 1;
        data.assign (cap * floatsPerFrame, 0.0f);
        headers.assign (cap, Header {});
        head.store (0, std::memory_order_relaxed);
        tail.store (0, std::memory_order_relaxed);
    }

    size_t capacity() const noexcept { return headers.size(); }
    size_t floatsPerFrame() const noexcept { return frameFloats; }

    // ---- producer -----------------------------------------------------------
    /** The slot to fill next, or nullptr when the queue is full. */
    float* beginWrite() noexcept
    {
        const size_t w = head.load (std::memory_order_relaxed);
        if (w - tail.load (std::memory_order_acquire) >= headers.size())
            return nullptr;
        return data.data() + (w & mask) * frameFloats;
    }

    /** Publishes the slot returned by the last successful beginWrite(). */
    void commitWrite (const Header& h) noexcept
    {
        const size_t w = head.load (std::memory_order_relaxed);
        headers[w & mask] = h;
        head.store (w + 1, std::memory_order_release);
    }

    // ---- consumer -----------------------------------------------------------
    /** The oldest frame (and its header), or nullptr when empty. Valid until release(). */
    const float* peek (Header& h) const noexcept
    {
        const size_t r = tail.load (std::memory_order_relaxed);
        if (head.load (std::memory_order_acquire) == r)
            return nullptr;
        h = headers[r & mask];
        return data.data() + (r & mask) * frameFloats;
    }

    /** Drops the frame returned by the last successful peek(). */
    void release() noexcept { tail.store (tail.load (std::memory_order_relaxed) + 1, std::memory_order_release); }

    /** Snapshot of the fill level: an upper bound from the producer thread
        (the consumer may have released since), a lower bound from the
        consumer thread (the producer may have committed since), approximate
        elsewhere. */
    size_t size() const noexcept
    {
        const size_t r = tail.load (std::memory_order_acquire); // tail first: head only grows, so this never underflows
        return head.load (std::memory_order_acquire) - r;
    }

private:
    std::vector<float> data;
    std::vector<Header> headers;
    size_t frameFloats = 0, mask = 0;
    alignas (64) std::atomic<size_t> head { 0 };
    alignas (64) std::atomic<size_t> tail { 0 };
};

#if defined(_MSC_VER)
    #pragma warning(pop)
#endif
} // namespace flub
