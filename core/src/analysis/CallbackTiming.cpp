#include "flub/analysis/CallbackTiming.h"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace flub
{
namespace
{
// Single writer: a relaxed load + store is enough (and cheaper than an RMW).
inline void bump (std::atomic<uint64_t>& counter, uint64_t by = 1) noexcept
{
    counter.store (counter.load (std::memory_order_relaxed) + by, std::memory_order_relaxed);
}

constexpr int kSubBits = static_cast<int> (std::bit_width (static_cast<unsigned> (TimingHistogram::kSubBuckets))) - 1; // 3
constexpr int kBaseBits = static_cast<int> (std::bit_width (TimingHistogram::kBaseNs)) - 1;                            // 10
static_assert ((1 << kSubBits) == TimingHistogram::kSubBuckets && (uint64_t { 1 } << kBaseBits) == TimingHistogram::kBaseNs);
} // namespace

// =============================================================================
// TimingHistogram
// =============================================================================
int TimingHistogram::bucketFor (uint64_t ns) noexcept FLUB_NONBLOCKING
{
    if (ns < kBaseNs)
        return 0;
    const int msb = static_cast<int> (std::bit_width (ns)) - 1; // >= kBaseBits
    const int octave = msb - kBaseBits;
    if (octave >= kOctaves)
        return kNumBuckets - 1;
    // The kSubBits bits below the leading one pick the sub-bucket.
    const int sub = static_cast<int> ((ns >> (msb - kSubBits)) & static_cast<uint64_t> (kSubBuckets - 1));
    return 1 + octave * kSubBuckets + sub;
}

uint64_t TimingHistogram::bucketLowerNs (int bucket) noexcept
{
    if (bucket <= 0)
        return 0;
    bucket = std::min (bucket, kNumBuckets - 1);
    const int octave = (bucket - 1) / kSubBuckets;
    const int sub = (bucket - 1) % kSubBuckets;
    // (1 + sub / kSubBuckets) x kBaseNs x 2^octave
    return static_cast<uint64_t> (kSubBuckets + sub) << (octave + kBaseBits - kSubBits);
}

uint64_t TimingHistogram::bucketUpperNs (int bucket) noexcept
{
    if (bucket >= kNumBuckets - 1)
        return UINT64_MAX;
    return bucketLowerNs (std::max (bucket, 0) + 1);
}

void TimingHistogram::add (uint64_t ns) noexcept FLUB_NONBLOCKING
{
    bump (counts[static_cast<size_t> (bucketFor (ns))]);
    bump (sumNs, ns);
    if (ns > maxNs.load (std::memory_order_relaxed))
        maxNs.store (ns, std::memory_order_relaxed);
}

TimingHistogram::Snapshot TimingHistogram::snapshot() const noexcept
{
    Snapshot s;
    for (size_t b = 0; b < counts.size(); ++b)
        s.counts[b] = counts[b].load (std::memory_order_relaxed);
    s.sumNs = sumNs.load (std::memory_order_relaxed);
    s.maxNs = maxNs.load (std::memory_order_relaxed);
    return s;
}

uint64_t TimingHistogram::Snapshot::count() const noexcept
{
    uint64_t n = 0;
    for (const auto c : counts)
        n += c;
    return n;
}

double TimingHistogram::Snapshot::meanNs() const noexcept
{
    const auto n = count();
    return n > 0 ? static_cast<double> (sumNs) / static_cast<double> (n) : 0.0;
}

double TimingHistogram::Snapshot::percentileNs (double q) const noexcept
{
    const auto n = count();
    if (n == 0)
        return 0.0;
    const double rank = std::clamp (q, 0.0, 1.0) * static_cast<double> (n);
    double below = 0.0;
    for (int b = 0; b < kNumBuckets; ++b)
    {
        const auto inBucket = static_cast<double> (counts[static_cast<size_t> (b)]);
        if (inBucket <= 0.0)
            continue;
        if (below + inBucket >= rank)
        {
            const double lower = static_cast<double> (bucketLowerNs (b));
            // The top bucket has no upper edge: up to the largest value.
            const double upper = b == kNumBuckets - 1 ? std::max (lower, static_cast<double> (maxNs))
                                                      : static_cast<double> (bucketUpperNs (b));
            const double value = lower + (upper - lower) * std::clamp ((rank - below) / inBucket, 0.0, 1.0);
            return std::min (value, static_cast<double> (maxNs));
        }
        below += inBucket;
    }
    return static_cast<double> (maxNs);
}

TimingHistogram::Snapshot TimingHistogram::Snapshot::since (const Snapshot& earlier) const noexcept
{
    Snapshot w;
    int highest = -1;
    for (size_t b = 0; b < counts.size(); ++b)
    {
        w.counts[b] = counts[b] - std::min (counts[b], earlier.counts[b]);
        if (w.counts[b] > 0)
            highest = static_cast<int> (b);
    }
    w.sumNs = sumNs - std::min (sumNs, earlier.sumNs);
    // The window's largest value is in its highest non-empty bucket.
    w.maxNs = highest < 0 ? 0 : std::min (maxNs, bucketUpperNs (highest));
    return w;
}

// =============================================================================
// CallbackTiming
// =============================================================================
void CallbackTiming::record (uint64_t stampNs, uint64_t durationNs, uint64_t periodNs) noexcept FLUB_NONBLOCKING
{
    duration.add (durationNs);
    if (periodNs > 0 && durationNs > periodNs)
        bump (overBudget);

    const bool restart = restartRequested.load (std::memory_order_acquire)
                      && restartRequested.exchange (false, std::memory_order_acq_rel);
    if (! restart && stampNs >= lastStampNs)
    {
        const uint64_t gap = stampNs - lastStampNs;
        interval.add (gap);
        if (periodNs > 0 && static_cast<double> (gap) > kLateFactor * static_cast<double> (periodNs))
            bump (late);
    }
    lastStampNs = stampNs;

    lastPeriodNs.store (periodNs, std::memory_order_relaxed);
    bump (callbacks);
}

CallbackTiming::Snapshot CallbackTiming::snapshot() const noexcept
{
    Snapshot s;
    s.callbacks = callbacks.load (std::memory_order_relaxed);
    s.overBudget = overBudget.load (std::memory_order_relaxed);
    s.late = late.load (std::memory_order_relaxed);
    s.periodNs = lastPeriodNs.load (std::memory_order_relaxed);
    s.duration = duration.snapshot();
    s.interval = interval.snapshot();
    return s;
}

CallbackTiming::Snapshot CallbackTiming::Snapshot::since (const Snapshot& earlier) const noexcept
{
    Snapshot w;
    w.duration = duration.since (earlier.duration);
    w.interval = interval.since (earlier.interval);
    w.callbacks = callbacks - std::min (callbacks, earlier.callbacks);
    w.overBudget = overBudget - std::min (overBudget, earlier.overBudget);
    w.late = late - std::min (late, earlier.late);
    w.periodNs = periodNs;
    return w;
}

double CallbackTiming::Snapshot::loadAt (double q) const noexcept
{
    return periodNs > 0 ? duration.percentileNs (q) / static_cast<double> (periodNs) : 0.0;
}

double CallbackTiming::Snapshot::meanLoad() const noexcept
{
    return periodNs > 0 ? duration.meanNs() / static_cast<double> (periodNs) : 0.0;
}
} // namespace flub
