// Flubsound Pro - the PipeWire xrun case's judgement (CallbackStallAttribution.h,
// docs/11 E48, R1.2) on recorded CI numbers, on every OS: which idle xruns
// are the runner's (a scheduling stall, or time the hypervisor took that the
// kernel charged to the data thread, seen as steal) and which are the
// engine's own (work over the period, a wait of its own).

#include "AppTestSupport.h"
#include "CallbackStallAttribution.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace flubtest;
namespace stalls = flubapptest::stalls;

namespace
{
constexpr double kPeriodNs = 1.0e9 * 128.0 / 48000.0; // the CI graph: quantum 128 at 48 kHz, 2.67 ms
const int64_t kPeriod = static_cast<int64_t> (std::llround (kPeriodNs));

/** One cycle of a synthetic window: an idle cycle of the CI runner by
    default (done 0.67 ms after its start with 0.62 ms of CPU time and one
    wait, for its trigger). */
struct Spec
{
    int64_t doneAfterNs = 670000;
    int64_t cpuNs = 620000;
    long waits = 1, preemptions = 0;
    int missedBefore = 0; // driver cycles the node missed before this one
    bool xrun = false;    // the node counted this cycle as an xrun (seen at the next callback)
    int cpu = 1;
};

/** 'specs' as recorded callbacks: index 0 is the one before the window, the
    window is 2 .. n - 5 and the recorder ran 4 more (as in the case). */
stalls::Window makeWindow (const std::vector<Spec>& specs)
{
    stalls::Window w;
    w.periodNs = kPeriodNs;
    int64_t stamp = 1000000000, cpu = 0;
    long voluntary = 0, involuntary = 0;
    int xruns = 0;
    for (size_t k = 0; k < specs.size(); ++k)
    {
        const auto& s = specs[k];
        if (k > 0)
            stamp += (1 + s.missedBefore) * kPeriod;
        cpu += s.cpuNs;
        voluntary += s.waits;
        involuntary += s.preemptions;
        stalls::Callback c;
        c.stampNs = stamp;
        c.wallNs = stamp + s.doneAfterNs;
        c.cpuNs = cpu;
        c.voluntary = voluntary;
        c.involuntary = involuntary;
        c.xruns = xruns;
        c.cpu = s.cpu;
        w.callbacks.push_back (c);
        if (s.xrun)
            ++xruns;
    }
    w.first = 2;
    w.last = specs.size() - 4;
    w.recorded = specs.size();
    return w;
}

constexpr size_t kCycles = 231; // 225 in the window, as on CI

/** The window index of the window's callback 'n' (as the CI log counts). */
size_t at (const stalls::Window& w, size_t n) { return w.first + n; }

/** Run 38030655885 attempt 1, loop 2: window callback 48 took 3.98 ms of
    CPU time with one preemption and one wait and ended 4.04 ms after its
    cycle's start; callback 49 followed 0.54 ms later (no wait: its trigger
    was already there), done 1.92 ms after its start, 0.54 ms of CPU. */
std::vector<Spec> failingRun()
{
    std::vector<Spec> specs (kCycles);
    auto& slow = specs[2 + 48];
    slow.doneAfterNs = 4042220;
    slow.cpuNs = 3980590;
    slow.preemptions = 1;
    slow.xrun = true;
    auto& next = specs[2 + 49];
    next.doneAfterNs = 1918720;
    next.cpuNs = 542513;
    next.waits = 0;
    return specs;
}

/** Samples every millisecond over the window, no steal, the thread waiting
    0.01 ms on the run queue per sample, 2 interrupts and 1 softirq per
    sample on CPU 1 (the thread's) and 50 interrupts on CPU 3; 'steal' adds
    ticks on 'cpu' from time 't' on. */
struct StealAt
{
    int cpu;
    int64_t t;
    int64_t ticks;
};
stalls::RunnerSamples samplesFor (const stalls::Window& w, const std::vector<StealAt>& steal, stalls::StealInCpuTime counted)
{
    stalls::RunnerSamples s;
    s.numCpus = 4;
    s.stealInCpuTime = counted;
    std::vector<int64_t> perCpu (4, 0), irqs (4, 0), softirqs (4, 0);
    const auto begin = w.callbacks.front().wallNs - 1000000, end = w.callbacks.back().wallNs + 20000000;
    int64_t wait = 0;
    for (int64_t t = begin; t <= end; t += 1000000)
    {
        int64_t total = 0;
        for (int c = 0; c < 4; ++c)
        {
            perCpu[static_cast<size_t> (c)] = 0;
            for (const auto& e : steal)
                if (e.cpu == c && t >= e.t)
                    perCpu[static_cast<size_t> (c)] += e.ticks;
            total += perCpu[static_cast<size_t> (c)];
        }
        wait += 10000;
        irqs[1] += 2;
        irqs[3] += 50;
        softirqs[1] += 1;
        s.add (t, total, perCpu, wait, irqs, softirqs);
    }
    return s;
}

/** A sample of the run time helper stamped at 'start' that did its reads
    only at 'done' (preempted in between: it runs at idle priority). */
struct HeldSample
{
    int64_t start = -1, done = -1;
};

/** Hyper-V's run time per vCPU every 0.5 ms (what tools/scripts/hv-vp-runtime.py
    samples, its reads taking 20 us): CPU 1 (the thread's) runs a quarter of
    the time, except 'slowRunPerMs' of each millisecond from 'slowFrom' to
    'slowTo'; the other vCPUs a quarter of the time. With a 'held' sample,
    the samples it held up are not taken. */
void addVpRuntime (stalls::RunnerSamples& s, const stalls::Window& w, int64_t slowFrom, int64_t slowTo, double slowRunPerMs, HeldSample held = {})
{
    const auto begin = w.callbacks.front().wallNs - 1000000, end = w.callbacks.back().wallNs + 20000000;
    const auto quarter = [begin] (int64_t t) { return static_cast<int64_t> (std::llround (0.25 * static_cast<double> (t - begin))); };
    const auto cpu1 = [&] (int64_t t) // CPU 1's run time up to t
    {
        const auto slow = std::max<int64_t> (0, std::min (t, slowTo) - std::max (begin, slowFrom));
        return quarter (t) + static_cast<int64_t> (std::llround ((slowRunPerMs - 0.25) * static_cast<double> (slow)));
    };
    std::vector<std::pair<int64_t, int64_t>> stamps; // start, done
    for (int64_t t = begin; t <= end; t += 500000)
        if (held.start < 0 || t < held.start || t > held.done)
            stamps.emplace_back (t, t + 20000);
    if (held.start >= 0)
        stamps.emplace_back (held.start, held.done);
    std::sort (stamps.begin(), stamps.end());
    for (const auto& [start, done] : stamps)
        s.addVp (start, done, { quarter (done), cpu1 (done), quarter (done), quarter (done) }); // read at the end of its reads
}

/** The case's own lag: steal is read up to 2 periods + 4 ms after a cycle. */
const int64_t kLagNs = 2 * kPeriod + 4000000;

void withRunner (stalls::Window& w, const stalls::RunnerSamples& s) { w.runner = stalls::runnerPerCycle (w.callbacks, s, w.first, w.recorded, kLagNs); }

void print (const char* what, const stalls::Attribution& a, const std::vector<std::string>& failed)
{
    std::cout << "    " << what << ": " << a.xrunsStalled << " of " << a.xrunsSeen << " xruns, " << a.lateStalled << " of " << a.lateSeen << " late intervals, "
              << a.finishesStalled << " of " << a.finishesSeen << " late finishes a scheduling stall; " << a.cpuOverPeriod << " cycles at the period ("
              << a.cpuOverRunner << " the runner's)" << a.overs << " -> " << (failed.empty() ? "passes" : "fails: " + failed.front()) << "\n";
}
} // namespace

TEST_CASE ("App: PipeWire xrun attribution - an idle window passes; run 38030655885's slow cycle (3.98 ms of CPU, no steal) is the engine's own work and fails (E48, R1.2)")
{
    // An idle window as on CI: 225 callbacks, nothing late.
    auto idle = makeWindow (std::vector<Spec> (kCycles));
    withRunner (idle, samplesFor (idle, {}, stalls::StealInCpuTime::Yes));
    const auto quiet = stalls::attribute (idle);
    CHECK (quiet.cycles == 225);
    CHECK (quiet.usualWaits == 1);
    CHECK (quiet.xrunsSeen + quiet.lateSeen + quiet.finishesSeen == 0);
    CHECK (quiet.cpuOverPeriod == 0);
    CHECK_NEAR (quiet.wallMeanNs, kPeriodNs, 1.0);
    CHECK (stalls::judge (quiet, idle, 0, 0, 0).empty());
    CHECK (idle.runner[at (idle, 100)].measured);
    CHECK (idle.runner[at (idle, 100)].stealTicks == 0);
    CHECK (idle.runner[at (idle, 100)].waitNs >= 10000); // about 3 samples' worth of run-queue wait
    // Interrupts and softirqs are counted on the thread's CPU only (3 to 4
    // samples a cycle: CPU 3's 50 per sample are not in them).
    CHECK (idle.runner[at (idle, 100)].irqs >= 4);
    CHECK (idle.runner[at (idle, 100)].irqs <= 10);
    CHECK (idle.runner[at (idle, 100)].softirqs >= 2);
    CHECK (idle.runner[at (idle, 100)].softirqs <= 5);

    // The failing loop's numbers: nothing measured from the runner (as in
    // that run), and the runner sampled without steal (what the new case
    // reads on a runner whose kernel has no steal clock): the same verdict.
    for (const bool sampled : { false, true })
    {
        auto w = makeWindow (failingRun());
        if (sampled)
            withRunner (w, samplesFor (w, {}, stalls::StealInCpuTime::Yes));
        const auto a = stalls::attribute (w);
        const auto failed = stalls::judge (a, w, 1, 0, 1); // 1 xrun, no late interval, 1 engine callback over the period (E45)
        print (sampled ? "38030655885, sampled, no steal" : "38030655885, not sampled", a, failed);
        CHECK (a.xrunsSeen == 1);
        CHECK (a.xrunsStalled == 0);
        CHECK (a.finishesSeen == 1);
        CHECK (a.finishesStalled == 0);
        CHECK (a.lateSeen == 0);
        CHECK (a.cpuOverPeriod == 1);
        CHECK (a.cpuOverRunner == 0);
        CHECK ((a.engineCycles == std::vector<size_t> { at (w, 48) }));
        CHECK_NEAR (a.maxCpuNs, 3980590, 1);
        CHECK (! stalls::isStall (w, at (w, 48), a.usualWaits));
        CHECK (failed.size() == 3); // the xrun, the late finish, the own work
        CHECK (a.events.find ("NO scheduling stall around it; callback 48") != std::string::npos);
    }
}

TEST_CASE ("App: PipeWire xrun attribution - the same slow cycle with steal on its CPU, charged to the thread by the kernel, is the runner's and passes (E48, R1.2)")
{
    // One /proc/stat steal tick on CPU 1 (the thread's) while the slow cycle
    // ran: steal under 20 ms explains its 3.98 ms of CPU time.
    auto w = makeWindow (failingRun());
    const auto during = w.callbacks[at (w, 47)].wallNs + 2000000;
    for (const auto counted : { stalls::StealInCpuTime::Yes, stalls::StealInCpuTime::Unknown })
    {
        withRunner (w, samplesFor (w, { { 1, during, 1 } }, counted));
        const auto& r = w.runner[at (w, 48)];
        CHECK (r.measured);
        CHECK (r.stealTicks == 1);
        CHECK (r.stealTotal == 1);
        CHECK (r.explainedNs == 20000000);
        const auto a = stalls::attribute (w);
        const auto failed = stalls::judge (a, w, 1, 0, 1);
        print (counted == stalls::StealInCpuTime::Yes ? "38030655885 with steal" : "38030655885 with steal, accounting unknown", a, failed);
        CHECK (stalls::isStall (w, at (w, 48), a.usualWaits));
        CHECK (a.xrunsStalled == a.xrunsSeen);
        CHECK (a.finishesStalled == a.finishesSeen);
        CHECK (a.cpuOverPeriod == 1);
        CHECK (a.cpuOverRunner == 1);
        CHECK (a.engineCycles.empty());
        CHECK (a.maxOwnCpuNs < kPeriod); // the rest of the window's cycles
        CHECK (failed.empty());
        CHECK (a.events.find ("a scheduling stall at callback 48") != std::string::npos);
        CHECK (a.events.find ("steal 1 ticks on CPU 1") != std::string::npos);
    }

    // Two ticks in a cycle bound its steal below 30 ms.
    withRunner (w, samplesFor (w, { { 1, during, 2 } }, stalls::StealInCpuTime::Yes));
    CHECK (w.runner[at (w, 48)].explainedNs == 30000000);
}

TEST_CASE ("App: PipeWire xrun attribution - steal on another CPU, steal the kernel takes out of the thread's CPU time, or steal in another cycle does not excuse a slow cycle (E48, R1.2)")
{
    const auto check = [] (const char* what, stalls::Window w, bool runners)
    {
        const auto a = stalls::attribute (w);
        const auto failed = stalls::judge (a, w, 1, 0, 1);
        print (what, a, failed);
        CHECK (failed.empty() == runners);
        CHECK (a.cpuOverRunner == (runners ? 1 : 0));
    };

    // The thread ran on CPU 1; the steal was on CPU 3.
    auto w = makeWindow (failingRun());
    const auto during = w.callbacks[at (w, 47)].wallNs + 2000000;
    withRunner (w, samplesFor (w, { { 3, during, 1 } }, stalls::StealInCpuTime::Yes));
    CHECK (w.runner[at (w, 48)].stealTicks == 0);
    CHECK (w.runner[at (w, 48)].stealTotal == 1);
    check ("steal on CPU 3, the thread on CPU 1", w, false);

    // When the thread's CPU is not known, steal anywhere counts.
    auto unknown = makeWindow ([] {
        auto specs = failingRun();
        for (auto& s : specs)
            s.cpu = -1;
        return specs;
    }());
    withRunner (unknown, samplesFor (unknown, { { 3, during, 1 } }, stalls::StealInCpuTime::Yes));
    CHECK (unknown.runner[at (unknown, 48)].stealTicks == 1);
    CHECK (unknown.runner[at (unknown, 48)].irqs >= 4 * 52); // and every CPU's interrupts
    check ("steal on CPU 3, the thread's CPU not known", unknown, true);

    // A kernel that takes steal out of the thread's CPU time
    // (CONFIG_PARAVIRT_TIME_ACCOUNTING): 3.98 ms of CPU time were the
    // thread's own, whatever the steal.
    withRunner (w, samplesFor (w, { { 1, during, 1 } }, stalls::StealInCpuTime::No));
    CHECK (w.runner[at (w, 48)].stealTicks == 1);
    CHECK (w.runner[at (w, 48)].explainedNs == 0);
    check ("steal on CPU 1, out of the thread's CPU time", w, false);

    // Steal 20 cycles before the slow one does not reach it: the slow
    // cycle's span (and the 9.3 ms after it) saw none.
    withRunner (w, samplesFor (w, { { 1, w.callbacks[at (w, 27)].wallNs + 1000000, 1 } }, stalls::StealInCpuTime::Yes));
    CHECK (w.runner[at (w, 48)].stealTicks == 0);
    CHECK (w.runner[at (w, 28)].stealTicks == 1);
    check ("steal 20 cycles earlier", w, false);

    // A cycle the samples do not bracket is not measured: no excuse.
    auto late = makeWindow (failingRun());
    auto samples = samplesFor (late, { { 1, during, 1 } }, stalls::StealInCpuTime::Yes);
    const auto keepFrom = stalls::sampleAtOrAfter (samples, late.callbacks[at (late, 48)].wallNs);
    samples.wallNs.erase (samples.wallNs.begin(), samples.wallNs.begin() + static_cast<std::ptrdiff_t> (keepFrom));
    samples.stealTotal.erase (samples.stealTotal.begin(), samples.stealTotal.begin() + static_cast<std::ptrdiff_t> (keepFrom));
    samples.stealPerCpu.erase (samples.stealPerCpu.begin(), samples.stealPerCpu.begin() + static_cast<std::ptrdiff_t> (keepFrom * 4));
    samples.waitNs.erase (samples.waitNs.begin(), samples.waitNs.begin() + static_cast<std::ptrdiff_t> (keepFrom));
    withRunner (late, samples);
    CHECK (! late.runner[at (late, 48)].measured);
    CHECK (late.runner[at (late, 52)].measured);
    check ("the samples start after the slow cycle began", late, false);
}

TEST_CASE ("App: PipeWire xrun attribution - a missed cycle and a held callback whose CPU time fit the period stay scheduling stalls; a wait of its own or injected work does not (E48, R1.2)")
{
    // Run 38030655885 attempt 2, loop 9: window callback 134 came 2 periods
    // after the one before on the driver clock (the graph ran a cycle
    // without the node), done 1.01 ms after its cycle's start with 0.46 ms
    // of CPU, no preemption, one wait; the xrun showed at callback 135.
    std::vector<Spec> missed (kCycles);
    missed[2 + 134].missedBefore = 1;
    missed[2 + 134].doneAfterNs = 1011800;
    missed[2 + 134].cpuNs = 460507;
    missed[2 + 134].xrun = true;
    auto w = makeWindow (missed);
    withRunner (w, samplesFor (w, {}, stalls::StealInCpuTime::Yes));
    auto a = stalls::attribute (w);
    auto failed = stalls::judge (a, w, 1, 1, 0);
    print ("a missed cycle (38030655885 attempt 2)", a, failed);
    CHECK (a.xrunsSeen == 1);
    CHECK (a.lateSeen == 1);
    CHECK (a.xrunsStalled == 1);
    CHECK (a.lateStalled == 1);
    CHECK (failed.empty());

    // Run 37640705514, loop 7: one callback done 2.87 ms after its cycle's
    // start, 4.96 ms after the one before, an xrun (that run did not read the
    // CPU time; here 0.5 ms, held by one preemption).
    std::vector<Spec> held (kCycles);
    held[2 + 100].doneAfterNs = 2870000;
    held[2 + 100].cpuNs = 500000;
    held[2 + 100].preemptions = 1;
    held[2 + 100].xrun = true;
    w = makeWindow (held);
    a = stalls::attribute (w);
    failed = stalls::judge (a, w, 1, 0, 1);
    print ("a held callback (37640705514)", a, failed);
    CHECK (a.finishesSeen == 1);
    CHECK (a.finishesStalled == 1);
    CHECK (a.xrunsStalled == 1);
    CHECK (failed.empty());

    // The same lateness from a wait of its own (a second voluntary switch:
    // a lock or a sleep in the engine) is the engine's.
    held[2 + 100].preemptions = 0;
    held[2 + 100].waits = 2;
    w = makeWindow (held);
    withRunner (w, samplesFor (w, {}, stalls::StealInCpuTime::Yes));
    a = stalls::attribute (w);
    failed = stalls::judge (a, w, 1, 0, 1);
    print ("a wait of its own", a, failed);
    CHECK (! stalls::isStall (w, at (w, 100), a.usualWaits));
    CHECK (a.xrunsStalled == 0);
    CHECK (a.finishesStalled == 0);
    CHECK (! failed.empty());

    // 4 ms of work in one cycle with no steal anywhere (what the PipeWire
    // case injects): the engine's own, though it looks like the failing run.
    std::vector<Spec> work (kCycles);
    work[2 + 60].cpuNs = 4600000;
    work[2 + 60].doneAfterNs = 4670000;
    work[2 + 60].xrun = true;
    work[2 + 61].doneAfterNs = 4670000 - kPeriod + 540000; // right after it, its trigger already there
    work[2 + 61].cpuNs = 540000;
    work[2 + 61].waits = 0;
    w = makeWindow (work);
    withRunner (w, samplesFor (w, {}, stalls::StealInCpuTime::Yes));
    a = stalls::attribute (w);
    failed = stalls::judge (a, w, 1, 0, 0);
    print ("4 ms of injected work", a, failed);
    CHECK ((a.engineCycles == std::vector<size_t> { at (w, 60) }));
    CHECK (a.xrunsStalled == 0);
    CHECK (! failed.empty());
}

TEST_CASE ("App: PipeWire xrun attribution - on Hyper-V the vCPU's run time bounds the hypervisor's share: run 38030655885's slow cycle passes when its vCPU ran 0.7 ms of it, fails when it ran 4.2 ms (E48, R1.2)")
{
    // The slow cycle spans 6.04 ms from the callback before; its thread was
    // charged 3.98 ms of CPU time. The vCPU's run time is sampled alone
    // (no /proc/stat), as on a runner where steal reads 0.
    auto w = makeWindow (failingRun());
    const auto slowFrom = w.callbacks[at (w, 47)].wallNs, slowTo = w.callbacks[at (w, 48)].wallNs;
    const auto span = static_cast<double> (slowTo - slowFrom) / 1.0e6;
    for (const double ranMs : { 0.7, 4.2 })
    {
        stalls::RunnerSamples s;
        s.numCpus = 4;
        addVpRuntime (s, w, slowFrom, slowTo, ranMs / span);
        withRunner (w, s);
        const auto& r = w.runner[at (w, 48)];
        const auto a = stalls::attribute (w);
        const auto failed = stalls::judge (a, w, 1, 0, 1);
        print (ranMs < 1.0 ? "vCPU ran 0.7 ms of the slow cycle" : "vCPU ran 4.2 ms of the slow cycle", a, failed);
        CHECK (r.measured);
        CHECK (r.stealTicks == 0);
        // The samples around the span add at most 0.5 ms at a quarter rate.
        CHECK (r.vpRunNs >= static_cast<int64_t> (ranMs * 1.0e6) - 100000);
        CHECK (r.vpRunNs <= static_cast<int64_t> (ranMs * 1.0e6) + 300000);
        if (ranMs < 1.0)
        {
            CHECK (r.hypervisorNs >= 2900000); // at least 3.98 - 0.95 ms
            CHECK (r.explainedNs == r.hypervisorNs);
            CHECK (stalls::isStall (w, at (w, 48), a.usualWaits));
            CHECK (failed.empty());
            CHECK (a.events.find ("its vCPU ran") != std::string::npos);
        }
        else
        {
            CHECK (r.hypervisorNs == 0);
            CHECK ((a.engineCycles == std::vector<size_t> { at (w, 48) }));
            CHECK (! failed.empty());
        }
        // An ordinary cycle: its vCPU ran at least its 0.62 ms of CPU time.
        CHECK (w.runner[at (w, 100)].vpRunNs >= 620000);
        CHECK (w.runner[at (w, 100)].hypervisorNs == 0);
    }

    // With the thread's CPU not known, the vCPUs' run time bounds nothing.
    auto unknown = makeWindow ([] {
        auto specs = failingRun();
        for (auto& spec : specs)
            spec.cpu = -1;
        return specs;
    }());
    stalls::RunnerSamples s;
    s.numCpus = 4;
    addVpRuntime (s, unknown, slowFrom, slowTo, 0.7 / span);
    withRunner (unknown, s);
    CHECK (unknown.runner[at (unknown, 48)].vpRunNs == -1);
    CHECK (unknown.runner[at (unknown, 48)].explainedNs == 0);
}

TEST_CASE ("App: PipeWire xrun attribution - a run time sample stamped before the slow cycle but read inside it (the helper preempted) does not bound it: the slow cycle stays the engine's (E48, R1.2)")
{
    // The helper stamps a sample, then reads the MSRs; at idle priority it
    // can be preempted in between. Here it stamped 10 us before the slow
    // cycle's span and read only 0.1 ms before its end, after the vCPU had
    // run 4.2 ms of it (real work). Taken as the sample before the span, it
    // would leave that run time out (the vCPU "ran" about 0.2 ms) and call
    // 3.8 ms of the cycle's 3.98 ms the hypervisor's. Its reads were not
    // done by the span's start, so the sample before it bounds the cycle.
    auto w = makeWindow (failingRun());
    const auto slowFrom = w.callbacks[at (w, 47)].wallNs, slowTo = w.callbacks[at (w, 48)].wallNs;
    const auto span = static_cast<double> (slowTo - slowFrom) / 1.0e6;
    stalls::RunnerSamples s;
    s.numCpus = 4;
    addVpRuntime (s, w, slowFrom, slowTo, 4.2 / span, { slowFrom - 10000, slowTo - 100000 });
    withRunner (w, s);
    const auto& r = w.runner[at (w, 48)];
    const auto a = stalls::attribute (w);
    const auto failed = stalls::judge (a, w, 1, 0, 1);
    print ("a held sample before the slow cycle", a, failed);
    REQUIRE (r.measured);
    CHECK (r.vpRunNs >= 4200000); // the work is inside the bracket
    CHECK (r.hypervisorNs == 0);
    CHECK ((a.engineCycles == std::vector<size_t> { at (w, 48) }));
    CHECK (! failed.empty());

    // What a bracket by first stamps alone would have read: from the held
    // sample to the first one stamped at the span's end or later.
    const auto heldAt = static_cast<size_t> (std::find (s.vpWallNs.begin(), s.vpWallNs.end(), slowFrom - 10000) - s.vpWallNs.begin());
    const auto afterAt = static_cast<size_t> (std::lower_bound (s.vpWallNs.begin(), s.vpWallNs.end(), slowTo) - s.vpWallNs.begin());
    REQUIRE (heldAt < s.vpWallNs.size());
    REQUIRE (afterAt < s.vpWallNs.size());
    const auto heldRun = s.vpRun (afterAt, 1) - s.vpRun (heldAt, 1);
    std::cout << "    bracketed by first stamps: the vCPU ran " << static_cast<double> (heldRun) / 1.0e6 << " ms, a hypervisor's share of "
              << static_cast<double> (stalls::cycleCpu (w, at (w, 48)) - heldRun) / 1.0e6 << " ms; by the reads: ran " << static_cast<double> (r.vpRunNs) / 1.0e6
              << " ms, share " << static_cast<double> (r.hypervisorNs) / 1.0e6 << " ms\n";
    CHECK (heldRun < 300000);
    CHECK (stalls::cycleCpu (w, at (w, 48)) - heldRun >= 3600000);
}
