// Flubsound Pro - wait-free single-producer / single-consumer ring buffer.
//
// Used for every audio-thread <-> other-thread hand-off that carries a stream:
//   * audio thread -> GUI       : analyzer / waveform sample taps
//   * capture thread -> render  : per-app loopback streams (with drift control)
//   * GUI -> audio thread       : small command structs (preset swap, meter reset)
// Capacity is rounded up to a power of two. Head/tail live on separate cache
// lines to avoid false sharing between producer and consumer cores.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace flub
{
template <typename T>
class SpscRing
{
    static_assert (std::is_trivially_copyable_v<T>, "SpscRing only carries trivially copyable data");

public:
    SpscRing() = default;
    explicit SpscRing (size_t minCapacity) { allocate (minCapacity); }

    /** Allocates; call before the ring is shared between threads. */
    void allocate (size_t minCapacity)
    {
        size_t cap = 1;
        while (cap < minCapacity + 1)
            cap <<= 1;
        buffer.assign (cap, T {});
        mask = cap - 1;
        head.store (0, std::memory_order_relaxed);
        tail.store (0, std::memory_order_relaxed);
    }

    size_t capacity() const noexcept { return mask; }

    /** Producer side. Returns number of items actually written (drops on overflow). */
    size_t push (const T* items, size_t count) noexcept
    {
        const size_t w = head.load (std::memory_order_relaxed);
        const size_t r = tail.load (std::memory_order_acquire);
        const size_t freeSpace = mask - ((w - r) & mask);
        const size_t n = std::min (count, freeSpace);
        for (size_t i = 0; i < n; ++i)
            buffer[(w + i) & mask] = items[i];
        head.store ((w + n) & mask, std::memory_order_release);
        return n;
    }

    bool push (const T& item) noexcept { return push (&item, 1) == 1; }

    /** Consumer side. Returns number of items read. */
    size_t pop (T* dest, size_t count) noexcept
    {
        const size_t r = tail.load (std::memory_order_relaxed);
        const size_t w = head.load (std::memory_order_acquire);
        const size_t avail = (w - r) & mask;
        const size_t n = std::min (count, avail);
        for (size_t i = 0; i < n; ++i)
            dest[i] = buffer[(r + i) & mask];
        tail.store ((r + n) & mask, std::memory_order_release);
        return n;
    }

    bool pop (T& item) noexcept { return pop (&item, 1) == 1; }

    /** Consumer side: discard items (e.g. to drop stale analyzer data). */
    size_t skip (size_t count) noexcept
    {
        const size_t r = tail.load (std::memory_order_relaxed);
        const size_t w = head.load (std::memory_order_acquire);
        const size_t n = std::min (count, (w - r) & mask);
        tail.store ((r + n) & mask, std::memory_order_release);
        return n;
    }

    /** Approximate fill level; exact when called from either endpoint thread. */
    size_t available() const noexcept
    {
        return (head.load (std::memory_order_acquire) - tail.load (std::memory_order_acquire)) & mask;
    }

private:
    std::vector<T> buffer;
    size_t mask = 0;
    alignas (64) std::atomic<size_t> head { 0 };
    alignas (64) std::atomic<size_t> tail { 0 };
};
} // namespace flub
