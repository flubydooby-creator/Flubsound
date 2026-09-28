// Flubsound Pro - per-callback timing histograms for the audio thread
// (docs/11 E45).
//
// The device's CPU load, polled at 2 Hz, is an average: it hides callbacks
// that run 3-9x longer than the rest (Balanced / 128 samples: mean 83.9 us,
// max 544.6 us), and a backend that reports no xrun count (-1) leaves those
// spikes invisible. CallbackTiming records EVERY callback into two lock-free
// histograms that another thread reads:
//   duration : wall-clock time spent inside the callback, start to end. It
//              includes any preemption of the audio thread, which is what the
//              device sees when the callback is late.
//   interval : time from the previous callback (the device's cadence; the
//              backend's host timestamp when it gives one). A gap in the
//              stream shows here even when the backend counts no xrun.
// plus counters against the callback's period (numSamples / sampleRate):
//   overBudget : duration > period (the callback could not keep up)
//   late       : interval > kLateFactor x period (a stall or discontinuity)
//
// Buckets are log-spaced, kSubBuckets per octave from kBaseNs (1.024 us;
// bucket 0 holds everything below), so a percentile is within one bucket
// (1/8 octave, 9 %) of the true value; the top bucket holds everything from
// ~4.3 s up. Memory: 2 x 177 counters, fixed.
//
// Threads: ONE writer (the audio thread: record()) and any number of readers
// (snapshot(); restartIntervals() from any thread). Every counter is a
// relaxed atomic written only by the writer: a snapshot taken during a
// record() may see that callback in some fields and not yet in others, but
// never a torn value. A reader that wants a window (the last second, since
// its previous poll) keeps its previous snapshot and calls since(): nothing
// is reset, so readers never disturb each other or the writer.
//
// Real-time: record() and add() are wait-free (no allocation, locks, system
// calls or loops over the buckets); snapshot() is O(buckets), reader side.
#pragma once

#include "flub/common/Realtime.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace flub
{
/** Lock-free log-bucketed histogram of durations in nanoseconds. */
class TimingHistogram
{
public:
    static constexpr int kSubBuckets = 8;       // per octave (3 bits)
    static constexpr int kOctaves = 22;         // 1.024 us .. ~4.3 s
    static constexpr int kNumBuckets = 1 + kSubBuckets * kOctaves;
    static constexpr uint64_t kBaseNs = 1024;   // lower edge of bucket 1

    /** The bucket a value falls in (0 = below kBaseNs, the last = from its lower edge up). */
    static int bucketFor (uint64_t ns) noexcept FLUB_NONBLOCKING;
    /** A bucket's lower edge (inclusive) and upper edge (exclusive; the last
        bucket's nominal edge, kBaseNs << kOctaves). */
    static uint64_t bucketLowerNs (int bucket) noexcept;
    static uint64_t bucketUpperNs (int bucket) noexcept;

    struct Snapshot
    {
        std::array<uint64_t, kNumBuckets> counts {};
        uint64_t sumNs = 0;
        /** The largest value recorded. In a window from since(): the upper
            edge of the window's highest non-empty bucket, at most the
            all-time largest, i.e. within one bucket above the true value. */
        uint64_t maxNs = 0;

        uint64_t count() const noexcept;
        double meanNs() const noexcept;
        /** The value below which a fraction q (0..1) of the values lie,
            interpolated linearly inside its bucket and at most maxNs; 0 when
            empty. */
        double percentileNs (double q) const noexcept;
        /** What was recorded after `earlier` (a snapshot of the same histogram). */
        Snapshot since (const Snapshot& earlier) const noexcept;
    };

    /** Writer thread. */
    void add (uint64_t ns) noexcept FLUB_NONBLOCKING;
    /** Any thread. */
    Snapshot snapshot() const noexcept;

private:
    std::array<std::atomic<uint64_t>, kNumBuckets> counts {};
    std::atomic<uint64_t> sumNs { 0 }, maxNs { 0 };
};

/** Duration and interval histograms of an audio callback (see above). */
class CallbackTiming
{
public:
    /** An interval longer than this many periods counts as late. */
    static constexpr double kLateFactor = 1.5;

    struct Snapshot
    {
        TimingHistogram::Snapshot duration, interval;
        uint64_t callbacks = 0;  // recorded callbacks
        uint64_t overBudget = 0; // callbacks whose duration exceeded their period
        uint64_t late = 0;       // intervals longer than kLateFactor x period
        uint64_t periodNs = 0;   // the latest callback's period, 0 before the first

        /** What was recorded after `earlier`; periodNs is the latest. */
        Snapshot since (const Snapshot& earlier) const noexcept;
        /** A duration percentile over the period (1.0 = the callback used its
            whole period); 0 when nothing was recorded. q = 1 is the peak. */
        double loadAt (double q) const noexcept;
        double meanLoad() const noexcept;
    };

    /** Audio thread, once per callback: `stampNs` is the callback's timestamp
        on any monotonic clock (the backend's host time when it gives one, else
        the callback's start), `durationNs` the time spent inside it,
        `periodNs` numSamples / sampleRate. The first callback, and the first
        after restartIntervals() or a timestamp that went backwards, records
        no interval. */
    void record (uint64_t stampNs, uint64_t durationNs, uint64_t periodNs) noexcept FLUB_NONBLOCKING;

    /** Any thread: the next record() starts a new run of intervals (after a
        device restart, or callbacks the writer did not time), so that gap
        is neither an interval nor late. */
    void restartIntervals() noexcept FLUB_NONBLOCKING { restartRequested.store (true, std::memory_order_release); }

    /** Any thread. */
    Snapshot snapshot() const noexcept;

private:
    TimingHistogram duration, interval;
    std::atomic<uint64_t> callbacks { 0 }, overBudget { 0 }, late { 0 }, lastPeriodNs { 0 };
    std::atomic<bool> restartRequested { true };
    uint64_t lastStampNs = 0; // writer only
};
} // namespace flub
