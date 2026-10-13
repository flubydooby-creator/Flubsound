// Flubsound Pro - telling the runner's stalls from the engine's own work in
// an idle device window (docs/11 E48, R1.2: the PipeWire device's xrun case
// in test_app_pipewire.cpp).
//
// The xrun case records, at the end of each cycle's device callbacks on the
// node's data thread, the wall clock, the driver's cycle start, the thread's
// CPU time and context switches, the device's xrun count and the CPU it ran
// on. A sampler thread reads the runner's steal time per CPU (/proc/stat) and
// the data thread's run-queue wait (schedstat) about every millisecond. While
// idle, every xrun, late interval and callback that finished after its cycle
// must be a scheduling stall: the runner keeping the thread off the CPU, not
// the engine's work or a wait of its own. This header holds that judgement.
// It has no platform code, so its rules are unit-tested with recorded CI
// numbers on every OS (test_app_stall_attribution.cpp).
//
// The CPU time a guest kernel counts for a thread can include time the
// hypervisor took from its vCPU: a kernel with no steal clock (Hyper-V gives
// Linux none) cannot see it, and one with a steal clock but without
// CONFIG_PARAVIRT_TIME_ACCOUNTING (or booted with no-steal-acc) reports it
// in /proc/stat but still charges it to the running thread. A cycle whose CPU
// time reached the period counts as the runner's only when steal on the CPU
// the thread ran on explains it; with no steal it stays the engine's own work
// and the case fails. /proc/stat counts steal in USER_HZ ticks (10 ms), so
// d ticks in a cycle's window bound its steal below (d + 1) ticks: one tick
// explains any overrun under 20 ms, no tick explains none.
#pragma once

#include "flub/analysis/CallbackTiming.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace flubapptest::stalls
{
/** One recorder call: the end of one cycle's device callbacks on the data
    thread. The counters are cumulative. */
struct Callback
{
    int64_t wallNs = 0;                  // steady clock at the recorder
    int64_t stampNs = 0;                 // the driver's cycle start the block was stamped with (0: none)
    int64_t cpuNs = 0;                   // the data thread's CPU time
    long voluntary = 0, involuntary = 0; // its context switches: waits of its own, the kernel's preemptions
    int xruns = 0;                       // the device's count, up to the previous cycle
    int cpu = -1;                        // the CPU it ran on at the recorder (-1: not known)
};

/** Whether steal that /proc/stat reports is also in the CPU time of the
    thread that was running (CLOCK_THREAD_CPUTIME_ID). A kernel without a
    steal clock reports none, and the hypervisor's time is in the thread's
    CPU time unseen. */
enum class StealInCpuTime
{
    Unknown, // treated as Yes: steal seen on the thread's CPU may explain its CPU time
    Yes,     // no CONFIG_PARAVIRT_TIME_ACCOUNTING, or booted with no-steal-acc
    No       // the scheduler takes reported steal out of the thread's CPU time: it explains no CPU time
};

inline const char* describe (StealInCpuTime s)
{
    return s == StealInCpuTime::Yes ? "yes" : s == StealInCpuTime::No ? "no" : "unknown (taken as yes)";
}

/** The runner's counters sampled about every millisecond by a thread of
    its own (Linux: /proc/stat and /proc/self/task/<tid>/schedstat). */
struct RunnerSamples
{
    int numCpus = 0;
    int64_t tickNs = 10000000; // /proc/stat's unit (USER_HZ = 100)
    StealInCpuTime stealInCpuTime = StealInCpuTime::Unknown;
    std::vector<int64_t> wallNs;      // per sample, ascending, the recorder's clock
    std::vector<int64_t> stealTotal;  // ticks, all CPUs
    std::vector<int64_t> stealPerCpu; // ticks, numCpus per sample
    std::vector<int64_t> waitNs;      // the data thread's run-queue wait (cumulative ns; -1: not read)
    // Information only (counts, not time): the interrupts and softirqs each
    // CPU handled (/proc/interrupts, /proc/softirqs), numCpus per sample.
    // Without CONFIG_IRQ_TIME_ACCOUNTING their time is in the CPU time of
    // the thread they interrupted.
    std::vector<int64_t> irqsPerCpu, softirqsPerCpu;

    size_t size() const { return wallNs.size(); }
    size_t at (size_t sample, int cpu) const { return sample * static_cast<size_t> (numCpus) + static_cast<size_t> (cpu); }
    int64_t steal (size_t sample, int cpu) const { return stealPerCpu[at (sample, cpu)]; }
    int64_t irqs (size_t sample, int cpu) const { return irqsPerCpu[at (sample, cpu)]; }
    int64_t softirqs (size_t sample, int cpu) const { return softirqsPerCpu[at (sample, cpu)]; }
    void add (int64_t wall, int64_t total, const std::vector<int64_t>& perCpu, int64_t wait, const std::vector<int64_t>& irqCounts = {},
              const std::vector<int64_t>& softirqCounts = {})
    {
        const auto value = [] (const std::vector<int64_t>& v, int c) { return c < static_cast<int> (v.size()) ? v[static_cast<size_t> (c)] : 0; };
        wallNs.push_back (wall);
        stealTotal.push_back (total);
        for (int c = 0; c < numCpus; ++c)
        {
            stealPerCpu.push_back (value (perCpu, c));
            irqsPerCpu.push_back (value (irqCounts, c));
            softirqsPerCpu.push_back (value (softirqCounts, c));
        }
        waitNs.push_back (wait);
    }
};

/** What the runner measurably did during one cycle (from the callback
    before to this one). */
struct Runner
{
    bool measured = false;    // the samples bracket the cycle
    int cpuBefore = -1;       // the CPU the thread ended the previous cycle on
    int cpuAfter = -1;        // and this one
    int64_t stealTicks = 0;   // steal on those CPUs (all CPUs when neither is known), up to the accounting lag after the cycle
    int64_t stealTotal = 0;   // steal on all CPUs in the same span
    int64_t waitNs = -1;      // the thread's run-queue wait in the cycle (-1: not read)
    int64_t irqs = 0, softirqs = 0; // interrupts and softirqs on the thread's CPU(s) in the cycle (information)
    int64_t explainedNs = 0;  // CPU time the runner explains: (ticks + 1) x tick when steal was seen and counts as CPU time
};

/** The latest sample at or before 't' (size() when none). */
inline size_t sampleAtOrBefore (const RunnerSamples& s, int64_t t)
{
    const auto it = std::upper_bound (s.wallNs.begin(), s.wallNs.end(), t);
    return it == s.wallNs.begin() ? s.size() : static_cast<size_t> (it - s.wallNs.begin()) - 1;
}

/** The first sample at or after 't' (size() when none). */
inline size_t sampleAtOrAfter (const RunnerSamples& s, int64_t t)
{
    return static_cast<size_t> (std::lower_bound (s.wallNs.begin(), s.wallNs.end(), t) - s.wallNs.begin());
}

/** The runner per cycle for callbacks from .. to - 1 (index 0 has no cycle
    before it). A cycle spans the callback before to this one; its steal is
    read up to 'lagNs' later, because the kernel adds a CPU's steal to
    /proc/stat at that CPU's next tick (or when it leaves idle). */
inline std::vector<Runner> runnerPerCycle (const std::vector<Callback>& callbacks, const RunnerSamples& samples, size_t from, size_t to, int64_t lagNs)
{
    std::vector<Runner> runner (callbacks.size());
    to = std::min (to, callbacks.size());
    for (size_t k = std::max<size_t> (from, 1); k < to; ++k)
    {
        auto& r = runner[k];
        const auto begin = callbacks[k - 1].wallNs, end = callbacks[k].wallNs;
        const auto s0 = sampleAtOrBefore (samples, begin), s1 = sampleAtOrAfter (samples, end + lagNs);
        if (s0 >= samples.size() || s1 >= samples.size())
            continue; // not bracketed: not measured
        r.measured = true;
        r.cpuBefore = callbacks[k - 1].cpu;
        r.cpuAfter = callbacks[k].cpu;
        const auto known = [&] (int cpu) { return cpu >= 0 && cpu < samples.numCpus; };
        // A counter over the thread's CPUs between two samples: each distinct
        // CPU once (the thread usually wakes where it slept), all CPUs when
        // neither is known.
        const auto onThreadCpus = [&] (auto counter, size_t sampleFrom, size_t sampleTo)
        {
            int64_t sum = 0;
            if (known (r.cpuBefore))
                sum += counter (sampleTo, r.cpuBefore) - counter (sampleFrom, r.cpuBefore);
            if (known (r.cpuAfter) && r.cpuAfter != r.cpuBefore)
                sum += counter (sampleTo, r.cpuAfter) - counter (sampleFrom, r.cpuAfter);
            if (! known (r.cpuBefore) && ! known (r.cpuAfter))
                for (int c = 0; c < samples.numCpus; ++c)
                    sum += counter (sampleTo, c) - counter (sampleFrom, c);
            return sum;
        };
        if (known (r.cpuBefore) || known (r.cpuAfter))
            r.stealTicks = onThreadCpus ([&] (size_t i, int c) { return samples.steal (i, c); }, s0, s1);
        else
            r.stealTicks = samples.stealTotal[s1] - samples.stealTotal[s0];
        r.stealTotal = samples.stealTotal[s1] - samples.stealTotal[s0];
        const auto w1 = sampleAtOrAfter (samples, end); // counted when they happen: no lag
        if (w1 < samples.size())
        {
            if (samples.waitNs[s0] >= 0 && samples.waitNs[w1] >= 0)
                r.waitNs = samples.waitNs[w1] - samples.waitNs[s0];
            if (! samples.irqsPerCpu.empty())
            {
                r.irqs = onThreadCpus ([&] (size_t i, int c) { return samples.irqs (i, c); }, s0, w1);
                r.softirqs = onThreadCpus ([&] (size_t i, int c) { return samples.softirqs (i, c); }, s0, w1);
            }
        }
        if (r.stealTicks > 0 && samples.stealInCpuTime != StealInCpuTime::No)
            r.explainedNs = (r.stealTicks + 1) * samples.tickNs;
    }
    return runner;
}

/** An idle window: the callbacks first .. last - 1 measured, the recorder
    running until recorded - 1 (an xrun of the window's last cycle shows a
    callback later). */
struct Window
{
    std::vector<Callback> callbacks;
    std::vector<Runner> runner; // per callback; empty when nothing was sampled
    size_t first = 0, last = 0, recorded = 0;
    double periodNs = 0.0;
    bool unsplit = true; // one callback per cycle (block = quantum): the stamps are cycle starts
};

/** E45's "late": an interval longer than this many periods. */
constexpr double kLateFactor = flub::CallbackTiming::kLateFactor;

/** At most this many events of a kind in an idle window. */
constexpr int kToleratedStalls = 2;

inline int64_t wallGap (const Window& w, size_t k) { return w.callbacks[k].wallNs - w.callbacks[k - 1].wallNs; }
inline int64_t cycleCpu (const Window& w, size_t k) { return w.callbacks[k].cpuNs - w.callbacks[k - 1].cpuNs; }
inline int64_t doneAfter (const Window& w, size_t k) { return w.callbacks[k].wallNs - w.callbacks[k].stampNs; }
inline long waitsOf (const Window& w, size_t k) { return w.callbacks[k].voluntary - w.callbacks[k - 1].voluntary; }
inline long preemptionsOf (const Window& w, size_t k) { return w.callbacks[k].involuntary - w.callbacks[k - 1].involuntary; }
inline const Runner* runnerOf (const Window& w, size_t k) { return k < w.runner.size() && w.runner[k].measured ? &w.runner[k] : nullptr; }
inline int64_t explainedNs (const Window& w, size_t k)
{
    const auto* r = runnerOf (w, k);
    return r != nullptr ? r->explainedNs : 0;
}
/** The cycle's CPU time less what the runner measurably took. */
inline int64_t ownCpu (const Window& w, size_t k) { return std::max<int64_t> (0, cycleCpu (w, k) - explainedNs (w, k)); }
inline bool finishedLate (const Window& w, size_t k)
{
    return w.callbacks[k].stampNs != 0 && static_cast<double> (doneAfter (w, k)) >= w.periodNs;
}

/** A scheduling stall at callback k: a cycle whose own work fit the period
    and that waited for nothing but its trigger (a voluntary switch more than
    'usualWaits' is a lock or a wait in the engine), yet ran late: more than
    1.5 periods after the callback before it, or finished after its cycle's
    end while the kernel preempted it or the hypervisor took time from it. */
inline bool isStall (const Window& w, size_t k, long usualWaits)
{
    if (! w.unsplit || k < 1 || k >= std::min (w.recorded, w.callbacks.size()))
        return false;
    if (waitsOf (w, k) > usualWaits || static_cast<double> (ownCpu (w, k)) >= w.periodNs)
        return false; // a wait of its own, or its own work over the period
    return static_cast<double> (wallGap (w, k)) > kLateFactor * w.periodNs
           || (finishedLate (w, k) && (preemptionsOf (w, k) > 0 || explainedNs (w, k) > 0));
}

struct Attribution
{
    // The window's cycles, first .. last - 1.
    int cycles = 0;
    double wallMeanNs = 0.0;
    int64_t maxIntervalNs = 0, medianDelayNs = 0, maxDelayNs = 0, medianCpuNs = 0, maxCpuNs = 0, maxOwnCpuNs = 0;
    int wideIntervals = 0;   // wake-ups more than 1.5 periods after the one before
    int preemptedCycles = 0; // cycles in which the kernel preempted the data thread
    long usualWaits = 0;     // the median voluntary switches per cycle: its wait for the trigger
    int cpuOverPeriod = 0;   // cycles whose CPU time reached the period
    int cpuOverRunner = 0;   // of those, explained by the runner (steal)
    std::vector<size_t> engineCycles; // cycles whose own work reached the period
    std::string overs;       // those cycles, described

    // The events, first .. last: each located, and a scheduling stall or not.
    int xrunsSeen = 0, xrunsStalled = 0, lateSeen = 0, lateStalled = 0, finishesSeen = 0, finishesStalled = 0;
    std::string events;
};

inline double toMs (int64_t ns) { return static_cast<double> (ns) / 1.0e6; }

/** The runner's numbers for callback k, for a description. */
inline std::string describeRunner (const Window& w, size_t k)
{
    std::ostringstream s;
    const auto* r = runnerOf (w, k);
    if (r == nullptr)
    {
        s << ", runner not sampled";
        return s.str();
    }
    s << ", steal " << r->stealTicks << " ticks on CPU ";
    if (r->cpuBefore >= 0 && r->cpuAfter >= 0 && r->cpuBefore != r->cpuAfter)
        s << r->cpuBefore << " / " << r->cpuAfter;
    else if (r->cpuAfter >= 0 || r->cpuBefore >= 0)
        s << (r->cpuAfter >= 0 ? r->cpuAfter : r->cpuBefore);
    else
        s << "? (all CPUs)";
    s << " (" << r->stealTotal << " on all CPUs)";
    if (r->waitNs >= 0)
        s << ", run-queue wait " << toMs (r->waitNs) << " ms";
    s << ", " << r->irqs << " interrupts and " << r->softirqs << " softirqs on its CPU";
    if (r->explainedNs > 0)
        s << ", the runner's up to " << toMs (r->explainedNs) << " ms of its CPU time";
    return s.str();
}

/** Every event of the window and the cycles around it, judged. */
inline Attribution attribute (const Window& w)
{
    Attribution a;
    const auto recorded = std::min (w.recorded, w.callbacks.size());
    if (w.first < 1 || w.last <= w.first || w.last + 1 > recorded)
        return a; // the caller checks the window's shape first
    const auto& c = w.callbacks;
    const auto median = [] (auto values) // a copy, sorted here
    {
        std::sort (values.begin(), values.end());
        return values[values.size() / 2];
    };
    std::vector<int64_t> delays, cpus;
    std::vector<long> waits;
    a.maxDelayNs = std::numeric_limits<int64_t>::min();
    for (size_t i = w.first; i < w.last; ++i)
    {
        if (i > w.first)
        {
            a.maxIntervalNs = std::max (a.maxIntervalNs, wallGap (w, i));
            a.wideIntervals += static_cast<double> (wallGap (w, i)) > kLateFactor * w.periodNs ? 1 : 0;
        }
        if (c[i].stampNs != 0)
        {
            delays.push_back (doneAfter (w, i));
            a.maxDelayNs = std::max (a.maxDelayNs, delays.back());
        }
        cpus.push_back (cycleCpu (w, i));
        a.maxCpuNs = std::max (a.maxCpuNs, cpus.back());
        a.maxOwnCpuNs = std::max (a.maxOwnCpuNs, ownCpu (w, i));
        waits.push_back (waitsOf (w, i));
        a.preemptedCycles += preemptionsOf (w, i) > 0 ? 1 : 0;
    }
    a.cycles = static_cast<int> (w.last - w.first);
    a.wallMeanNs = static_cast<double> (c[w.last - 1].wallNs - c[w.first].wallNs) / static_cast<double> (w.last - 1 - w.first);
    a.medianDelayNs = delays.empty() ? 0 : median (delays);
    if (delays.empty())
        a.maxDelayNs = 0;
    a.medianCpuNs = median (cpus);
    a.usualWaits = median (waits);

    std::ostringstream overs;
    for (size_t i = w.first; i < w.last; ++i)
    {
        if (static_cast<double> (cycleCpu (w, i)) < w.periodNs)
            continue;
        ++a.cpuOverPeriod;
        const bool runners = static_cast<double> (ownCpu (w, i)) < w.periodNs;
        a.cpuOverRunner += runners ? 1 : 0;
        if (! runners)
            a.engineCycles.push_back (i);
        overs << "; callback " << i - w.first << ": CPU " << toMs (cycleCpu (w, i)) << " ms (" << static_cast<double> (cycleCpu (w, i)) / w.periodNs
              << " periods), " << preemptionsOf (w, i) << " preemptions, " << waitsOf (w, i) << " waits" << describeRunner (w, i) << ": "
              << (runners ? "the runner's" : "the engine's own work");
    }
    a.overs = overs.str();

    const auto stallIn = [&] (size_t from, size_t to) // the first stalled callback in from .. to; 0 when none
    {
        for (size_t k = std::max<size_t> (from, 1); k <= std::min (to, recorded - 1); ++k)
            if (isStall (w, k, a.usualWaits))
                return k;
        return size_t { 0 };
    };
    std::ostringstream events;
    const auto describeEvent = [&] (const char* what, size_t i, size_t at)
    {
        const auto k = at != 0 ? at : i; // the stall, or the event's own callback
        events << "; " << what << " at callback " << i - w.first << " (" << toMs (c[i].wallNs - c[w.first].wallNs) << " ms into the window): "
               << (at != 0 ? "a scheduling stall at callback " : "NO scheduling stall around it; callback ")
               << static_cast<long long> (k) - static_cast<long long> (w.first) << " ended " << toMs (wallGap (w, k)) << " ms after the one before ("
               << static_cast<double> (wallGap (w, k)) / w.periodNs << " periods; driver clock "
               << (c[k].stampNs != 0 && c[k - 1].stampNs != 0 ? toMs (c[k].stampNs - c[k - 1].stampNs) : 0.0) << " ms), done "
               << (c[k].stampNs != 0 ? toMs (doneAfter (w, k)) : 0.0) << " ms after its cycle's start, CPU " << toMs (cycleCpu (w, k)) << " ms, "
               << preemptionsOf (w, k) << " preemptions, " << waitsOf (w, k) << " waits" << describeRunner (w, k);
    };
    for (size_t i = w.first; i <= w.last; ++i)
    {
        // The device's count, up to the previous cycle: an xrun of a missed
        // cycle shows one callback after the gap that skipped it, one of a
        // cycle finished late one callback after that cycle.
        for (int x = c[i - 1].xruns; x < c[i].xruns; ++x)
        {
            const auto at = stallIn (i >= 3 ? i - 3 : 1, i + 1);
            ++a.xrunsSeen;
            a.xrunsStalled += at != 0 ? 1 : 0;
            describeEvent ("xrun seen", i, at);
        }
        if (c[i].stampNs != 0 && c[i - 1].stampNs != 0 && static_cast<double> (c[i].stampNs - c[i - 1].stampNs) > kLateFactor * w.periodNs)
        {
            const auto at = stallIn (i - 1, i + 1);
            ++a.lateSeen;
            a.lateStalled += at != 0 ? 1 : 0;
            describeEvent ("late interval", i, at);
        }
        if (w.unsplit && finishedLate (w, i))
        {
            const auto at = stallIn (i - 1, i);
            ++a.finishesSeen;
            a.finishesStalled += at != 0 ? 1 : 0;
            describeEvent ("finished after its cycle", i, at);
        }
    }
    a.events = events.str();
    return a;
}

/** The xrun case's verdict on a window: every condition it failed (none:
    the window passes). 'deviceXruns' is the device's count over the window,
    'lateIntervals' and 'overBudget' E45's counts for the engine's callback. */
inline std::vector<std::string> judge (const Attribution& a, const Window& w, int deviceXruns, uint64_t lateIntervals, uint64_t overBudget)
{
    std::vector<std::string> failed;
    std::ostringstream s;
    const auto fail = [&failed] (const std::string& why) { failed.push_back (why); };
    if (deviceXruns < 0)
        fail ("the device's xrun count went down");
    if (deviceXruns > kToleratedStalls)
        fail (std::to_string (deviceXruns) + " xruns in the window (at most " + std::to_string (kToleratedStalls) + ")");
    if (lateIntervals > static_cast<uint64_t> (kToleratedStalls))
        fail (std::to_string (lateIntervals) + " late intervals (at most " + std::to_string (kToleratedStalls) + ")");
    if (a.finishesSeen > kToleratedStalls)
        fail (std::to_string (a.finishesSeen) + " callbacks finished after their cycle (at most " + std::to_string (kToleratedStalls) + ")");
    if (a.xrunsSeen < deviceXruns)
        fail ("the recorder located " + std::to_string (a.xrunsSeen) + " of the device's " + std::to_string (deviceXruns) + " xruns");
    if (a.lateSeen < static_cast<int> (lateIntervals))
        fail ("the recorder located " + std::to_string (a.lateSeen) + " of E45's " + std::to_string (lateIntervals) + " late intervals");
    if (w.unsplit && a.finishesSeen < static_cast<int> (overBudget))
        fail ("the recorder located " + std::to_string (a.finishesSeen) + " late finishes for E45's " + std::to_string (overBudget) + " engine callbacks over the period");
    if (a.xrunsStalled != a.xrunsSeen)
        fail (std::to_string (a.xrunsSeen - a.xrunsStalled) + " xruns with no scheduling stall around them");
    if (a.lateStalled != a.lateSeen)
        fail (std::to_string (a.lateSeen - a.lateStalled) + " late intervals with no scheduling stall around them");
    if (a.finishesStalled != a.finishesSeen)
        fail (std::to_string (a.finishesSeen - a.finishesStalled) + " late finishes with no scheduling stall around them");
    if (! a.engineCycles.empty())
    {
        s << a.engineCycles.size() << " cycles whose own work reached the period (CPU time at most " << toMs (a.maxCpuNs) << " ms, "
          << toMs (a.maxOwnCpuNs) << " ms not explained by the runner; period " << w.periodNs / 1.0e6 << " ms)";
        fail (s.str());
    }
    return failed;
}
} // namespace flubapptest::stalls
