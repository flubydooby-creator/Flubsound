// CallbackTiming (docs/11 E45): the audio callback's duration and interval
// histograms. Deterministic: the tests feed the nanosecond values a callback
// would measure, so no clock is involved.
#include "TestFramework.h"

#include "flub/analysis/CallbackTiming.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>

using namespace flub;

namespace
{
constexpr uint64_t periodNs (int samples, double rate)
{
    return static_cast<uint64_t> (static_cast<double> (samples) * 1.0e9 / rate + 0.5);
}
} // namespace

TEST_CASE ("CallbackTiming: histogram buckets tile the range at 1/8 octave (a percentile is within 12.5 % of the value)")
{
    using H = TimingHistogram;
    CHECK (H::bucketFor (0) == 0);
    CHECK (H::bucketFor (H::kBaseNs - 1) == 0);
    CHECK (H::bucketFor (H::kBaseNs) == 1);
    CHECK (H::bucketUpperNs (0) == H::kBaseNs);
    CHECK (H::bucketFor (UINT64_MAX) == H::kNumBuckets - 1);

    bool tiled = true, narrow = true;
    for (int b = 1; b < H::kNumBuckets - 1; ++b)
    {
        const uint64_t lo = H::bucketLowerNs (b), hi = H::bucketUpperNs (b);
        tiled = tiled && hi > lo && H::bucketFor (lo) == b && H::bucketFor (hi - 1) == b && H::bucketLowerNs (b + 1) == hi;
        narrow = narrow && static_cast<double> (hi - lo) <= 0.125 * static_cast<double> (lo) + 0.5;
    }
    CHECK (tiled);
    CHECK (narrow);
    // The top bucket starts at ~4.3 s (any real callback fits below it) and
    // has no upper edge.
    CHECK (H::bucketLowerNs (H::kNumBuckets - 1) == H::kBaseNs << H::kOctaves);
    CHECK (H::bucketFor ((H::kBaseNs << H::kOctaves) - 1) == H::kNumBuckets - 2);
    CHECK (H::bucketUpperNs (H::kNumBuckets - 1) == UINT64_MAX);

    // A 5 s stall lands there, and the percentile and window max stay the value.
    TimingHistogram h;
    const auto before = h.snapshot();
    h.add (5000000000u);
    const auto window = h.snapshot().since (before);
    CHECK (window.maxNs == 5000000000u);
    CHECK (window.percentileNs (1.0) == 5.0e9);
}

TEST_CASE ("CallbackTiming: mean, percentiles and max of a known distribution")
{
    TimingHistogram h;
    // 10 000 values evenly spread over 100..200 us, plus one 900 us outlier.
    for (int i = 0; i < 10000; ++i)
        h.add (100000u + static_cast<uint64_t> (i) * 10u);
    h.add (900000u);
    const auto s = h.snapshot();
    CHECK (s.count() == 10001);
    CHECK (s.maxNs == 900000u);
    CHECK_NEAR (s.meanNs(), 150075.0, 100.0);
    CHECK_NEAR (s.percentileNs (0.5), 150000.0, 0.09 * 150000.0);
    CHECK_NEAR (s.percentileNs (0.99), 199000.0, 0.09 * 199000.0);
    CHECK (s.percentileNs (1.0) == 900000.0); // the top is clamped to the true max
    CHECK (s.percentileNs (0.0) >= 90000.0);
    CHECK (TimingHistogram().snapshot().percentileNs (0.5) == 0.0);
}

TEST_CASE ("CallbackTiming: a 3x spike every 500 ms is caught by p99.9 and max although the average load stays low (E45)")
{
    // docs/11 E45 Done-when: "A 3x spike every 500 ms with glitchCount = -1 is
    // caught." Balanced at 48 kHz / 128: 375 callbacks per second, each
    // using 40 % of its 2.667 ms period, and every 188th (500 ms) three times
    // as long, 120 %: over budget. The 2 Hz average the watchdog sees today
    // stays near 41 %, far below its 90 % threshold, and the device reports
    // no glitch count.
    const uint64_t period = periodNs (128, 48000.0);
    const auto normal = static_cast<uint64_t> (0.4 * static_cast<double> (period));
    CallbackTiming timing;
    auto previous = timing.snapshot();
    uint64_t stamp = 1000000000u;
    int spikes = 0;
    double worstWindowP999 = 0.0, worstWindowPeak = 0.0, worstWindowMean = 0.0;
    for (int i = 0; i < 375 * 10; ++i) // 10 s
    {
        const bool spike = i % 188 == 187;
        spikes += spike ? 1 : 0;
        timing.record (stamp, spike ? 3 * normal : normal, period);
        stamp += period;

        if (i % 375 == 374) // a 1 s diagnostics window
        {
            const auto now = timing.snapshot();
            const auto window = now.since (previous);
            previous = now;
            CHECK (window.callbacks == 375);
            worstWindowP999 = std::max (worstWindowP999, window.loadAt (0.999));
            worstWindowPeak = std::max (worstWindowPeak, window.loadAt (1.0));
            worstWindowMean = std::max (worstWindowMean, window.meanLoad());
        }
    }

    const auto all = timing.snapshot();
    CHECK (all.callbacks == 3750);
    CHECK (all.periodNs == period);
    CHECK (all.overBudget == static_cast<uint64_t> (spikes)); // every spike, nothing else
    CHECK (spikes == 19);
    CHECK (all.late == 0);                                    // the cadence itself was regular
    CHECK_NEAR (all.loadAt (1.0), 1.2, 0.01);                 // the exact max
    CHECK_GE (worstWindowPeak, 1.19);                         // a window's max: the true one (1.2 less rounding) ...
    CHECK_LE (worstWindowPeak, 1.2 * 1.125);                  // ... or at most one bucket above it
    CHECK_GE (worstWindowP999, 1.0);                          // p99.9 of a window sees the spike
    CHECK_LE (worstWindowMean, 0.42);                         // what an average shows
    CHECK_NEAR (all.loadAt (0.5), 0.4, 0.4 * 0.09);
}

TEST_CASE ("CallbackTiming: late callbacks from the interval; a restart or a clock step back is not a gap")
{
    const uint64_t period = periodNs (256, 48000.0);
    CallbackTiming timing;
    uint64_t stamp = 5000000u;
    for (int i = 0; i < 100; ++i, stamp += period)
        timing.record (stamp, 1000, period);
    auto s = timing.snapshot();
    CHECK (s.callbacks == 100);
    CHECK (s.interval.count() == 99); // the first callback has no interval
    CHECK (s.late == 0);
    CHECK_NEAR (s.interval.percentileNs (0.5), static_cast<double> (period), 0.09 * static_cast<double> (period));

    // A stall: the next callback arrives 3 periods after the last one.
    stamp += 2 * period;
    timing.record (stamp, 1000, period);
    s = timing.snapshot();
    CHECK (s.late == 1);
    CHECK_GE (static_cast<double> (s.interval.maxNs), 3.0 * static_cast<double> (period));

    // Just under 1.5 periods is not late.
    stamp += period + period / 2 - 1000;
    timing.record (stamp, 1000, period);
    CHECK (timing.snapshot().late == 1);

    // A device restart (restartIntervals) and a timestamp that went
    // backwards start a new run: no interval, nothing late.
    timing.restartIntervals();
    stamp += 1000u * period;
    timing.record (stamp, 1000, period);
    timing.record (stamp - 10u * period, 1000, period);
    s = timing.snapshot();
    CHECK (s.late == 1);
    CHECK (s.interval.count() == 101);
    CHECK (s.callbacks == 104);
}

TEST_CASE ("CallbackTiming: windows from since(); record() allocates nothing")
{
    CallbackTiming timing;
    const uint64_t period = periodNs (64, 48000.0);
    for (int i = 0; i < 50; ++i)
        timing.record (static_cast<uint64_t> (i) * period, 500000u, period); // 37.5 % load
    const auto first = timing.snapshot();

    flubtest::AllocationGuard guard;
    for (int i = 50; i < 80; ++i)
        timing.record (static_cast<uint64_t> (i) * period, 100000u, period);
    CHECK (guard.allocations() == 0);

    const auto window = timing.snapshot().since (first);
    CHECK (window.callbacks == 30);
    CHECK (window.duration.count() == 30);
    CHECK (window.interval.count() == 30);
    CHECK (window.duration.sumNs == 30u * 100000u);
    // Its max is the window's own (100 us, within a bucket), not the earlier 500 us.
    CHECK_GE (window.duration.maxNs, 100000u);
    CHECK_LE (static_cast<double> (window.duration.maxNs), 100000.0 * 1.125);
    CHECK (first.duration.maxNs == 500000u);
    const auto empty = first.since (first);
    CHECK (empty.callbacks == 0 && empty.duration.maxNs == 0 && empty.loadAt (0.99) == 0.0);
}

TEST_CASE ("CallbackTiming: a reader thread sees monotonic counts while the writer records")
{
    auto timing = std::make_unique<CallbackTiming>();
    constexpr int kCallbacks = 200000;
    std::atomic<bool> done { false };
    std::thread writer ([&]
    {
        for (int i = 0; i < kCallbacks; ++i)
            timing->record (static_cast<uint64_t> (i) * 1000u, static_cast<uint64_t> (1000 + i % 5000), 1000u);
        done.store (true);
    });

    uint64_t lastCallbacks = 0, lastCount = 0;
    bool monotonic = true;
    int reads = 0;
    while (! done.load() || reads == 0)
    {
        const auto s = timing->snapshot();
        monotonic = monotonic && s.callbacks >= lastCallbacks && s.duration.count() >= lastCount;
        lastCallbacks = s.callbacks;
        lastCount = s.duration.count();
        ++reads;
    }
    writer.join();
    const auto s = timing->snapshot();
    CHECK (monotonic);
    CHECK (s.callbacks == kCallbacks);
    CHECK (s.duration.count() == kCallbacks);
    CHECK (s.interval.count() == kCallbacks - 1);
    CHECK (s.duration.maxNs == 5999u);
}
