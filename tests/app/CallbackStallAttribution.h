// Flubsound Pro - telling the runner's stalls from the engine's own work in
// an idle device window (docs/11 E48, R1.2: the PipeWire device's xrun case
// in test_app_pipewire.cpp).
//
// The xrun case records, at the end of each cycle's device callbacks on the
// node's data thread, the wall clock, the driver's cycle start, the thread's
// CPU time and context switches, the device's xrun count and the CPU it ran
// on. A sampler thread reads the runner's steal time per CPU (/proc/stat),
// the data thread's run-queue wait (schedstat) and its CPU migrations
// (se.nr_migrations in /proc/self/task/<tid>/sched) about every millisecond.
// While idle, every xrun, late interval and callback that finished after
// its cycle must be a scheduling stall: the runner keeping the thread off
// the CPU, not the engine's work or a wait of its own. This header holds
// that judgement. It has no platform code, so its rules are unit-tested
// with recorded CI numbers on every OS (test_app_stall_attribution.cpp).
//
// The CPU time a guest kernel counts for a thread can include time the
// hypervisor took from its vCPU: a kernel with no steal clock cannot see it
// (Hyper-V gives Linux none: GitHub's runners report 0 steal), and one with
// a steal clock but without CONFIG_PARAVIRT_TIME_ACCOUNTING (or booted with
// no-steal-acc) reports it in /proc/stat but still charges it to the
// running thread. A cycle whose CPU time reached the period counts as the
// runner's only when the runner's measured share explains it; otherwise it
// stays the engine's own work and the case fails. The one measure that can
// is Hyper-V's: each vCPU's run time (HV_X64_MSR_VP_RUNTIME, sampled by a
// root helper: tools/scripts/hv-vp-runtime.py). A thread really runs only
// while its vCPU runs, so a cycle charged C of CPU time, over a span in
// which the vCPUs it can have run on ran V in all, had at least C - V taken
// by the hypervisor. That holds only when V covers every vCPU the thread
// ran on: the CPUs seen at the cycle's two ends when its migrations prove
// there was no other (none, or one between two different CPUs), otherwise
// every vCPU (a weaker bound, but one that always holds), and when the
// samples' reads bracket the span.
//
// /proc/stat's steal explains no CPU time; it is printed around every event.
// It counts in USER_HZ ticks (10 ms), and the kernel adds a CPU's steal at
// its next tick: d new ticks between two reads mean more than (d - 1) ticks
// of steal accounted between them (one tick can be nanoseconds crossing a
// tick boundary), which happened from a tick before the first read to the
// second. Over a cycle of a few milliseconds, and the margins needed to see
// its steal at all, that never leaves a lower bound above zero for steal
// inside the cycle (2026-10-25 review: one tick used to excuse up to 20 ms).
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
    int64_t wallNs = 0;                  // steady clock at the recorder, before its reads
    int64_t doneNs = 0;                  // steady clock after its reads (CPU time, switches, CPU; 0: not recorded)
    int64_t stampNs = 0;                 // the driver's cycle start the block was stamped with (0: none)
    int64_t cpuNs = 0;                   // the data thread's CPU time
    long voluntary = 0, involuntary = 0; // its context switches: waits of its own, the kernel's preemptions
    int xruns = 0;                       // the device's count, up to the previous cycle
    int cpu = -1;                        // the CPU it ran on at the recorder (-1: not known)
};

/** Whether steal that /proc/stat reports is also in the CPU time of the
    thread that was running (CLOCK_THREAD_CPUTIME_ID). For the runner's
    description only: steal explains no CPU time (see the top). */
enum class StealInCpuTime
{
    Unknown,
    Yes, // no CONFIG_PARAVIRT_TIME_ACCOUNTING, or booted with no-steal-acc
    No   // the scheduler takes reported steal out of the thread's CPU time
};

inline const char* describe (StealInCpuTime s)
{
    return s == StealInCpuTime::Yes ? "yes" : s == StealInCpuTime::No ? "no" : "unknown";
}

/** The runner's counters sampled about every millisecond by a thread of
    its own (Linux: /proc/stat, /proc/self/task/<tid>/schedstat and sched). */
struct RunnerSamples
{
    int numCpus = 0;
    // Each sample has two clock stamps: before its reads (wallNs) and after
    // them (doneNs). The sampler runs at idle priority and can be held
    // between the two, so its reads happened somewhere in between.
    std::vector<int64_t> wallNs, doneNs; // per sample, ascending, the recorder's clock
    std::vector<int64_t> stealTotal;     // ticks, all CPUs (information)
    std::vector<int64_t> stealPerCpu;    // ticks, numCpus per sample (information)
    std::vector<int64_t> waitNs;         // the data thread's run-queue wait (cumulative ns; -1: not read)
    std::vector<int64_t> migrations;     // the data thread's CPU migrations (cumulative; -1: not read)
    // Information only (counts, not time): the interrupts and softirqs each
    // CPU handled (/proc/interrupts, /proc/softirqs), numCpus per sample.
    // Without CONFIG_IRQ_TIME_ACCOUNTING their time is in the CPU time of
    // the thread they interrupted.
    std::vector<int64_t> irqsPerCpu, softirqsPerCpu;
    // Hyper-V's run time per vCPU (HV_X64_MSR_VP_RUNTIME, sampled by a root
    // helper: tools/scripts/hv-vp-runtime.py), ns, numCpus per sample
    // indexed by CPU id (-1: that CPU not sampled); empty when not sampled.
    // Stamped before (vpWallNs) and after (vpDoneNs) its reads, as above.
    std::vector<int64_t> vpWallNs, vpDoneNs, vpRunNs;

    size_t size() const { return wallNs.size(); }
    int64_t vpRun (size_t sample, int cpu) const { return vpRunNs[at (sample, cpu)]; }
    void addVp (int64_t wall, int64_t done, const std::vector<int64_t>& runNs)
    {
        vpWallNs.push_back (wall);
        vpDoneNs.push_back (done);
        for (int c = 0; c < numCpus; ++c)
            vpRunNs.push_back (c < static_cast<int> (runNs.size()) ? runNs[static_cast<size_t> (c)] : -1);
    }
    size_t at (size_t sample, int cpu) const { return sample * static_cast<size_t> (numCpus) + static_cast<size_t> (cpu); }
    int64_t steal (size_t sample, int cpu) const { return stealPerCpu[at (sample, cpu)]; }
    int64_t irqs (size_t sample, int cpu) const { return irqsPerCpu[at (sample, cpu)]; }
    int64_t softirqs (size_t sample, int cpu) const { return softirqsPerCpu[at (sample, cpu)]; }
    /** One sample: stamped at 'wall', its reads done at 'done' (-1: at 'wall'). */
    void add (int64_t wall, int64_t total, const std::vector<int64_t>& perCpu, int64_t wait, const std::vector<int64_t>& irqCounts = {},
              const std::vector<int64_t>& softirqCounts = {}, int64_t migrationCount = -1, int64_t done = -1)
    {
        const auto value = [] (const std::vector<int64_t>& v, int c) { return c < static_cast<int> (v.size()) ? v[static_cast<size_t> (c)] : 0; };
        wallNs.push_back (wall);
        doneNs.push_back (done >= wall ? done : wall);
        stealTotal.push_back (total);
        for (int c = 0; c < numCpus; ++c)
        {
            stealPerCpu.push_back (value (perCpu, c));
            irqsPerCpu.push_back (value (irqCounts, c));
            softirqsPerCpu.push_back (value (softirqCounts, c));
        }
        waitNs.push_back (wait);
        migrations.push_back (migrationCount);
    }
    /** Drops the samples before 'keepFrom' (the /proc samples, not the run time ones). */
    void dropBefore (size_t keepFrom)
    {
        keepFrom = std::min (keepFrom, size());
        const auto drop = [] (std::vector<int64_t>& v, size_t n) { v.erase (v.begin(), v.begin() + static_cast<std::ptrdiff_t> (std::min (n, v.size()))); };
        const auto perCpu = keepFrom * static_cast<size_t> (numCpus);
        for (auto* v : { &wallNs, &doneNs, &stealTotal, &waitNs, &migrations })
            drop (*v, keepFrom);
        for (auto* v : { &stealPerCpu, &irqsPerCpu, &softirqsPerCpu })
            drop (*v, perCpu);
    }
};

/** What the runner measurably did during one cycle (from the callback
    before to this one). */
struct Runner
{
    bool measured = false;    // the samples bracket the cycle
    int cpuBefore = -1;       // the CPU the thread ended the previous cycle on
    int cpuAfter = -1;        // and this one
    int64_t migrations = -1;  // the thread's CPU migrations over the cycle (and the samples' margins; -1: not read)
    bool cpusProven = false;  // it ran on no CPU but cpuBefore and cpuAfter (migrations 0, or 1 between the two)
    int64_t stealTicks = 0;   // steal on those CPUs (all CPUs when neither is known), up to the accounting lag after the cycle (information)
    int64_t stealTotal = 0;   // steal on all CPUs in the same span (information)
    int64_t waitNs = -1;      // the thread's run-queue wait in the cycle (-1: not read)
    int64_t irqs = -1, softirqs = -1; // interrupts and softirqs on the thread's CPU(s) in the cycle (information; -1: not read)
    int vpCpus = 0;           // how many vCPUs' run time V adds up: those it ran on (proven) or all; 0: not bounded
    int64_t vpRunNs = -1;     // how long those vCPUs really ran over the cycle's span (Hyper-V; -1: not sampled)
    int64_t hypervisorNs = 0; // the cycle's CPU time less that: charged to the thread while no vCPU of its ran it
    int64_t explainedNs = 0;  // CPU time the runner measurably took from the cycle: the hypervisor's share
};

/** The latest sample stamped at or before 't' (size() when none). */
inline size_t sampleAtOrBefore (const RunnerSamples& s, int64_t t)
{
    const auto it = std::upper_bound (s.wallNs.begin(), s.wallNs.end(), t);
    return it == s.wallNs.begin() ? s.size() : static_cast<size_t> (it - s.wallNs.begin()) - 1;
}

/** The first sample stamped at or after 't' (size() when none). */
inline size_t sampleAtOrAfter (const RunnerSamples& s, int64_t t)
{
    return static_cast<size_t> (std::lower_bound (s.wallNs.begin(), s.wallNs.end(), t) - s.wallNs.begin());
}

/** The last sample whose reads were done by 't', from the stamps after the
    reads (doneNs.size() when none). */
inline size_t sampleReadBy (const std::vector<int64_t>& doneNs, int64_t t)
{
    const auto it = std::upper_bound (doneNs.begin(), doneNs.end(), t);
    return it == doneNs.begin() ? doneNs.size() : static_cast<size_t> (it - doneNs.begin()) - 1;
}

/** The first sample whose reads began at 't' or later, from the stamps
    before the reads (wallNs.size() when none). */
inline size_t sampleReadFrom (const std::vector<int64_t>& wallNs, int64_t t)
{
    return static_cast<size_t> (std::lower_bound (wallNs.begin(), wallNs.end(), t) - wallNs.begin());
}

/** The runner per cycle for callbacks from .. to - 1 (index 0 has no cycle
    before it). A cycle's CPU time is read between the recorder's two
    stamps, so the span that holds it runs from the recorder before (its
    first stamp) to the end of this one's reads (its second). Its vCPUs'
    run time (Hyper-V) and the thread's migrations are read over samples
    whose reads bracket that span: the sample before must have finished its
    reads by the span's start, the sample after must have begun them at its
    end or later. A sample stamped before the span but read inside it would
    leave run time (or a migration) out and make the hypervisor's share
    look larger. Samples around the span can only add run time of the
    neighbouring cycles: the hypervisor's share is a lower bound. Steal and
    the interrupt counts are read up to 'lagNs' later, because the kernel
    adds a CPU's steal to /proc/stat at that CPU's next tick (information). */
inline std::vector<Runner> runnerPerCycle (const std::vector<Callback>& callbacks, const RunnerSamples& samples, size_t from, size_t to, int64_t lagNs)
{
    std::vector<Runner> runner (callbacks.size());
    to = std::min (to, callbacks.size());
    const auto vpSamples = samples.vpWallNs.size();
    const auto known = [&] (int cpu) { return cpu >= 0 && cpu < samples.numCpus; };
    for (size_t k = std::max<size_t> (from, 1); k < to; ++k)
    {
        auto& r = runner[k];
        const auto begin = callbacks[k - 1].wallNs, end = callbacks[k].wallNs;
        const auto readEnd = std::max (end, callbacks[k].doneNs);
        r.cpuBefore = callbacks[k - 1].cpu;
        r.cpuAfter = callbacks[k].cpu;

        // The CPUs it ran on: proven only by its migrations. None means one
        // CPU throughout; one, between two different CPUs, means those two.
        // Any more (a wake-up on a third CPU, then a pull to the last one)
        // and the CPUs at the two ends are not all of them.
        const auto m0 = sampleReadBy (samples.doneNs, begin), m1 = sampleReadFrom (samples.wallNs, readEnd);
        if (m0 < samples.size() && m1 < samples.size() && samples.migrations[m0] >= 0 && samples.migrations[m1] >= 0)
            r.migrations = samples.migrations[m1] - samples.migrations[m0];
        r.cpusProven = known (r.cpuBefore) && known (r.cpuAfter) && r.migrations >= 0 && r.migrations <= (r.cpuBefore != r.cpuAfter ? 1 : 0);

        // Hyper-V: how long the vCPUs it can have run on ran. Those it ran
        // on when proven, otherwise all (the thread runs on one vCPU at a
        // time, so it never really ran longer than all of them together).
        if (vpSamples > 0)
        {
            const auto before = sampleReadBy (samples.vpDoneNs, begin), after = sampleReadFrom (samples.vpWallNs, readEnd);
            if (before < vpSamples && after < vpSamples)
            {
                int64_t ran = 0;
                int counted = 0;
                bool sampled = true;
                for (int c = 0; c < samples.numCpus; ++c)
                {
                    if (r.cpusProven && c != r.cpuBefore && c != r.cpuAfter)
                        continue;
                    // A counter that went backwards (a VM save / restore resets it) or a damaged record proves nothing:
                    // leave the cycle unbounded rather than excuse it with a negative run time.
                    if (samples.vpRun (before, c) < 0 || samples.vpRun (after, c) < 0 || samples.vpRun (after, c) < samples.vpRun (before, c))
                        sampled = false;
                    ran += samples.vpRun (after, c) - samples.vpRun (before, c);
                    ++counted;
                }
                if (sampled && counted > 0)
                {
                    r.measured = true;
                    r.vpCpus = counted;
                    r.vpRunNs = ran;
                    r.hypervisorNs = std::max<int64_t> (0, (callbacks[k].cpuNs - callbacks[k - 1].cpuNs) - ran);
                }
            }
        }

        // /proc/stat and the rest, for the description: a counter over the
        // thread's CPUs between two samples (each distinct CPU once, all
        // CPUs when neither is known).
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
        const auto s0 = sampleAtOrBefore (samples, begin), s1 = sampleAtOrAfter (samples, end + lagNs);
        if (s0 < samples.size() && s1 < samples.size())
        {
            r.measured = true;
            r.stealTicks = onThreadCpus ([&] (size_t i, int c) { return samples.steal (i, c); }, s0, s1);
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
        }
        r.explainedNs = r.hypervisorNs;
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
    end while the kernel preempted it or while the hypervisor took enough
    of its time to explain that: without the hypervisor's share it would
    have finished within its cycle. */
inline bool isStall (const Window& w, size_t k, long usualWaits)
{
    if (! w.unsplit || k < 1 || k >= std::min (w.recorded, w.callbacks.size()))
        return false;
    if (waitsOf (w, k) > usualWaits || static_cast<double> (ownCpu (w, k)) >= w.periodNs)
        return false; // a wait of its own, or its own work over the period
    if (static_cast<double> (wallGap (w, k)) > kLateFactor * w.periodNs)
        return true;
    return finishedLate (w, k) && (preemptionsOf (w, k) > 0 || static_cast<double> (doneAfter (w, k) - explainedNs (w, k)) < w.periodNs);
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
    int cpuOverRunner = 0;   // of those, explained by the runner (the hypervisor's measured share)
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
    if (r->migrations >= 0)
        s << ", " << r->migrations << " migrations";
    else
        s << ", migrations not read";
    if (r->waitNs >= 0)
        s << ", run-queue wait " << toMs (r->waitNs) << " ms";
    if (r->irqs >= 0)
        s << ", " << r->irqs << " interrupts and " << r->softirqs << " softirqs on its CPU";
    if (r->vpRunNs >= 0)
    {
        if (r->cpusProven && r->cpuBefore == r->cpuAfter)
            s << ", its vCPU ran ";
        else if (r->cpusProven)
            s << ", its vCPUs " << r->cpuBefore << " / " << r->cpuAfter << " ran ";
        else
            s << ", all " << r->vpCpus << " vCPUs ran (its own not proven) ";
        s << toMs (r->vpRunNs) << " ms around it (Hyper-V: at least " << toMs (r->hypervisorNs) << " ms of its CPU time the hypervisor's)";
    }
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
