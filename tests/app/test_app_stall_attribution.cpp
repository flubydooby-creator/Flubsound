// Flubsound Pro - the PipeWire xrun case's judgement (CallbackStallAttribution.h,
// docs/11 E48, R1.2) on recorded CI numbers, on every OS: which idle xruns
// are the runner's (a scheduling stall, or time the hypervisor measurably
// took that the kernel charged to the data thread) and which are the
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
    window is 2 .. n - 5 and the recorder ran 4 more (as in the case). The
    recorder's reads take 5 us. */
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
        c.doneNs = c.wallNs + 5000;
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
    was already there), done 1.92 ms after its start, 0.54 ms of CPU.
    'cpuFrom48' is the CPU of callback 48 and those after it. */
std::vector<Spec> failingRun (int cpuFrom48 = 1)
{
    std::vector<Spec> specs (kCycles);
    for (size_t k = 2 + 48; k < specs.size(); ++k)
        specs[k].cpu = cpuFrom48;
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

/** A sample stamped at 'start' that did its reads only at 'done' (held in
    between: the samplers run at idle priority). */
struct HeldSample
{
    int64_t start = -1, done = -1;
};

struct StealAt
{
    int cpu;
    int64_t t;
    int64_t ticks;
};

/** What the /proc sampler sees: steal ticks on a CPU from a time on, the
    data thread's migrations (the times it moved to another CPU; not read
    when /proc/self/task/<tid>/sched cannot be read) and one held sample. */
struct SamplerSpec
{
    std::vector<StealAt> steal;
    std::vector<int64_t> migratedAt;
    bool migrationsRead = true;
    HeldSample held;
};

/** The /proc sampler every millisecond over the window, its reads taking
    20 us (the counters as at their end): the steal and migrations of
    'spec', the thread waiting 0.01 ms on the run queue per millisecond, 2
    interrupts and 1 softirq per millisecond on CPU 1 (the thread's) and 50
    interrupts on CPU 3. */
stalls::RunnerSamples samplesFor (const stalls::Window& w, const SamplerSpec& spec = {})
{
    stalls::RunnerSamples s;
    s.numCpus = 4;
    const auto begin = w.callbacks.front().wallNs - 1000000, end = w.callbacks.back().wallNs + 20000000;
    std::vector<std::pair<int64_t, int64_t>> stamps; // start, done
    for (int64_t t = begin; t <= end; t += 1000000)
        if (spec.held.start < 0 || t < spec.held.start || t > spec.held.done)
            stamps.emplace_back (t, t + 20000);
    if (spec.held.start >= 0)
        stamps.emplace_back (spec.held.start, spec.held.done);
    std::sort (stamps.begin(), stamps.end());
    for (const auto& stamp : stamps)
    {
        const int64_t readAt = stamp.second;
        const auto ms = (readAt - begin) / 1000000;
        std::vector<int64_t> perCpu (4, 0);
        int64_t total = 0;
        for (int c = 0; c < 4; ++c)
        {
            for (const auto& e : spec.steal)
                if (e.cpu == c && readAt >= e.t)
                    perCpu[static_cast<size_t> (c)] += e.ticks;
            total += perCpu[static_cast<size_t> (c)];
        }
        const auto moved = static_cast<int64_t> (std::count_if (spec.migratedAt.begin(), spec.migratedAt.end(), [readAt] (int64_t t) { return t <= readAt; }));
        s.add (stamp.first, total, perCpu, 10000 * ms, { 0, 2 * ms, 0, 50 * ms }, { 0, ms, 0, 0 }, spec.migrationsRead ? moved : -1, readAt);
    }
    return s;
}

/** Hyper-V's run time per vCPU every 0.5 ms (what tools/scripts/hv-vp-runtime.py
    samples, its reads taking 20 us): every vCPU runs 'background' of the
    time, except that in from .. to vCPU c runs 'during[c]' of each
    millisecond. With a 'held' sample, the samples it held up are not
    taken. */
struct VpRun
{
    int64_t from = 0, to = 0;
    std::vector<double> during; // per vCPU, ms per ms in from .. to (below 0: background)
    double background = 0.25;
    HeldSample held;
};

void addVpRuntime (stalls::RunnerSamples& s, const stalls::Window& w, const VpRun& run)
{
    const auto begin = w.callbacks.front().wallNs - 1000000, end = w.callbacks.back().wallNs + 20000000;
    const auto ranUpTo = [&] (int c, int64_t t)
    {
        const auto inside = static_cast<double> (std::max<int64_t> (0, std::min (t, run.to) - std::max (begin, run.from)));
        const auto rate = c < static_cast<int> (run.during.size()) && run.during[static_cast<size_t> (c)] >= 0.0 ? run.during[static_cast<size_t> (c)] : run.background;
        return static_cast<int64_t> (std::llround (run.background * static_cast<double> (t - begin) + (rate - run.background) * inside));
    };
    std::vector<std::pair<int64_t, int64_t>> stamps; // start, done
    for (int64_t t = begin; t <= end; t += 500000)
        if (run.held.start < 0 || t < run.held.start || t > run.held.done)
            stamps.emplace_back (t, t + 20000);
    if (run.held.start >= 0)
        stamps.emplace_back (run.held.start, run.held.done);
    std::sort (stamps.begin(), stamps.end());
    for (const auto& stamp : stamps) // read at the end of its reads
        s.addVp (stamp.first, stamp.second, { ranUpTo (0, stamp.second), ranUpTo (1, stamp.second), ranUpTo (2, stamp.second), ranUpTo (3, stamp.second) });
}

/** The run time of vCPUs 'cpus' over the samples that bracket callback k's
    span by their reads (as the attribution takes them), whatever the
    thread's migrations: what a bound with those vCPUs alone reads. */
int64_t vpRunOver (const stalls::RunnerSamples& s, const stalls::Window& w, size_t k, std::vector<int> cpus)
{
    const auto before = stalls::sampleReadBy (s.vpDoneNs, w.callbacks[k - 1].wallNs);
    const auto after = stalls::sampleReadFrom (s.vpWallNs, std::max (w.callbacks[k].wallNs, w.callbacks[k].doneNs));
    int64_t ran = 0;
    for (const auto c : cpus)
        ran += s.vpRun (after, c) - s.vpRun (before, c);
    return ran;
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

/** The failing run's window judged as on CI (1 xrun, no late interval, 1
    engine callback over the period). */
std::vector<std::string> judgeFailingRun (const char* what, const stalls::Window& w)
{
    const auto a = stalls::attribute (w);
    const auto failed = stalls::judge (a, w, 1, 0, 1);
    print (what, a, failed);
    return failed;
}
} // namespace

TEST_CASE ("App: PipeWire xrun attribution - an idle window passes; run 38030655885's slow cycle (3.98 ms of CPU, no steal) is the engine's own work and fails (E48, R1.2)")
{
    // An idle window as on CI: 225 callbacks, nothing late.
    auto idle = makeWindow (std::vector<Spec> (kCycles));
    withRunner (idle, samplesFor (idle));
    const auto quiet = stalls::attribute (idle);
    CHECK (quiet.cycles == 225);
    CHECK (quiet.usualWaits == 1);
    CHECK (quiet.xrunsSeen + quiet.lateSeen + quiet.finishesSeen == 0);
    CHECK (quiet.cpuOverPeriod == 0);
    CHECK_NEAR (quiet.wallMeanNs, kPeriodNs, 1.0);
    CHECK (stalls::judge (quiet, idle, 0, 0, 0).empty());
    const auto& r = idle.runner[at (idle, 100)];
    CHECK (r.measured);
    CHECK (r.stealTicks == 0);
    CHECK (r.migrations == 0);
    CHECK (r.cpusProven);
    CHECK (r.vpRunNs == -1); // no run time sampled
    CHECK (r.waitNs >= 10000); // about 3 samples' worth of run-queue wait
    // Interrupts and softirqs are counted on the thread's CPU only (3 to 4
    // milliseconds a cycle: CPU 3's 50 per millisecond are not in them).
    CHECK (r.irqs >= 4);
    CHECK (r.irqs <= 10);
    CHECK (r.softirqs >= 2);
    CHECK (r.softirqs <= 5);

    // The failing loop's numbers: nothing measured from the runner (as in
    // that run), and the runner sampled without steal or run time (what the
    // case reads on a runner without the run time helper): the same verdict.
    for (const bool sampled : { false, true })
    {
        auto w = makeWindow (failingRun());
        if (sampled)
            withRunner (w, samplesFor (w));
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

TEST_CASE ("App: PipeWire xrun attribution - /proc/stat's steal excuses nothing: one or two ticks on the slow cycle's CPU, on another CPU or with its CPU unknown, run 38030655885's slow cycle stays the engine's (E48, R1.2)")
{
    // /proc/stat counts steal in 10 ms ticks, added at a CPU's next tick: one
    // new tick can be nanoseconds of steal crossing a tick boundary, and the
    // slow cycle's 6.04 ms span (with the margins needed to see its steal)
    // cannot hold the two ticks a lower bound needs. Until the 2026-10-25
    // review one tick on the thread's CPU excused up to 20 ms of CPU time
    // and these windows passed.
    const auto reference = makeWindow (failingRun());
    const auto during = reference.callbacks[at (reference, 47)].wallNs + 2000000; // inside the slow cycle's span
    struct Case
    {
        const char* what;
        std::vector<StealAt> steal;
        int cpu;               // the thread's CPU (-1: not known)
        int64_t ticksOnThread; // what the description reports on its CPU (all CPUs when not known)
    };
    const std::vector<Case> cases {
        { "one steal tick on the thread's CPU 1", { { 1, during, 1 } }, 1, 1 },
        { "two steal ticks on the thread's CPU 1", { { 1, during, 2 } }, 1, 2 },
        { "one steal tick on CPU 3, the thread on CPU 1", { { 3, during, 1 } }, 1, 0 },
        { "one steal tick on CPU 3, the thread's CPU not known", { { 3, during, 1 } }, -1, 1 },
    };
    for (const auto& c : cases)
    {
        auto w = makeWindow ([&] {
            auto specs = failingRun();
            for (auto& s : specs)
                s.cpu = c.cpu;
            return specs;
        }());
        withRunner (w, samplesFor (w, { c.steal, {}, true, {} }));
        const auto& r = w.runner[at (w, 48)];
        const auto failed = judgeFailingRun (c.what, w);
        CHECK (r.measured);
        CHECK (r.stealTicks == c.ticksOnThread);
        CHECK (r.stealTotal == c.steal.front().ticks);
        CHECK (r.explainedNs == 0);
        CHECK (! stalls::isStall (w, at (w, 48), stalls::attribute (w).usualWaits));
        CHECK (failed.size() == 3);
        if (c.cpu == 1)
            CHECK (stalls::attribute (w).events.find ("steal " + std::to_string (c.ticksOnThread) + " ticks on CPU 1") != std::string::npos);
    }
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
    withRunner (w, samplesFor (w));
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
    withRunner (w, samplesFor (w));
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
    withRunner (w, samplesFor (w));
    a = stalls::attribute (w);
    failed = stalls::judge (a, w, 1, 0, 0);
    print ("4 ms of injected work", a, failed);
    CHECK ((a.engineCycles == std::vector<size_t> { at (w, 60) }));
    CHECK (a.xrunsStalled == 0);
    CHECK (! failed.empty());
}

TEST_CASE ("App: PipeWire xrun attribution - on Hyper-V the vCPU's run time bounds the hypervisor's share: run 38030655885's slow cycle on one vCPU, no migration, passes when its vCPU ran 0.7 ms of it, fails when it ran 4.2 ms or its migrations were not read (E48, R1.2)")
{
    // The slow cycle spans 6.04 ms from the callback before; its thread was
    // charged 3.98 ms of CPU time on CPU 1, with no migration (0 in the
    // counter): its vCPU's run time bounds it. The /proc sampler reads no
    // steal, as on a Hyper-V runner.
    auto w = makeWindow (failingRun());
    const auto slowFrom = w.callbacks[at (w, 47)].wallNs, slowTo = w.callbacks[at (w, 48)].wallNs;
    const auto span = static_cast<double> (slowTo - slowFrom) / 1.0e6;
    for (const double ranMs : { 0.7, 4.2 })
    {
        auto s = samplesFor (w);
        addVpRuntime (s, w, { slowFrom, slowTo, { -1.0, ranMs / span }, 0.25, {} });
        withRunner (w, s);
        const auto& r = w.runner[at (w, 48)];
        const auto a = stalls::attribute (w);
        const auto failed = stalls::judge (a, w, 1, 0, 1);
        print (ranMs < 1.0 ? "vCPU ran 0.7 ms of the slow cycle, no migration" : "vCPU ran 4.2 ms of the slow cycle, no migration", a, failed);
        REQUIRE (r.measured);
        CHECK (r.stealTicks == 0);
        CHECK (r.migrations == 0);
        CHECK (r.cpusProven);
        CHECK (r.vpCpus == 1);
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

    // The same 0.7 ms with the migrations not read (no /proc/self/task/<tid>/sched),
    // or the thread's CPU not known: nothing proves the thread stayed on
    // vCPU 1, so all four vCPUs' run time bounds the cycle (each of the
    // other three ran a quarter of the span), and it fails.
    for (const bool cpuKnown : { true, false })
    {
        auto u = makeWindow ([&] {
            auto specs = failingRun();
            for (auto& spec : specs)
                spec.cpu = cpuKnown ? 1 : -1;
            return specs;
        }());
        auto s = samplesFor (u, { {}, {}, ! cpuKnown, {} });
        addVpRuntime (s, u, { slowFrom, slowTo, { -1.0, 0.7 / span }, 0.25, {} });
        withRunner (u, s);
        const auto& r = u.runner[at (u, 48)];
        const auto failed = judgeFailingRun (cpuKnown ? "vCPU 1 ran 0.7 ms, migrations not read: all vCPUs bound it" : "vCPU 1 ran 0.7 ms, the thread's CPU not known: all vCPUs bound it", u);
        REQUIRE (r.measured);
        CHECK (! r.cpusProven);
        CHECK (r.vpCpus == 4);
        CHECK (r.vpRunNs >= 700000 + 3 * static_cast<int64_t> (0.25 * span * 1.0e6));
        CHECK (r.hypervisorNs == 0);
        CHECK (r.explainedNs == 0);
        CHECK (! failed.empty());
        CHECK (stalls::attribute (u).events.find ("all 4 vCPUs ran") != std::string::npos);
    }

    // The /proc samples start only 1 ms into the slow cycle: its migrations
    // are not bracketed, so all vCPUs bound it, and it fails.
    {
        auto s = samplesFor (w);
        s.dropBefore (stalls::sampleAtOrAfter (s, slowFrom + 1000000));
        addVpRuntime (s, w, { slowFrom, slowTo, { -1.0, 0.7 / span }, 0.25, {} });
        withRunner (w, s);
        const auto& r = w.runner[at (w, 48)];
        const auto failed = judgeFailingRun ("vCPU 1 ran 0.7 ms, the /proc samples start inside the slow cycle", w);
        REQUIRE (r.measured);
        CHECK (r.migrations == -1);
        CHECK (! r.cpusProven);
        CHECK (r.vpCpus == 4);
        CHECK (r.hypervisorNs == 0);
        CHECK (! failed.empty());
        CHECK (w.runner[at (w, 52)].migrations == 0); // bracketed a few cycles on
        CHECK (w.runner[at (w, 52)].vpCpus == 1);
    }
}

TEST_CASE ("App: PipeWire xrun attribution - on Hyper-V a slow cycle whose thread migrated more than its two ends show is bounded with every vCPU: 3.4 ms of real work on a third vCPU stays the engine's (E48, R1.2)")
{
    // 2026-10-25 review: the thread ends the cycle before on CPU 1 and the
    // slow cycle on CPU 2, but after its preemption it did the engine's work
    // on CPU 3: 0.1 ms on CPU 1, a wake-up on CPU 3 (3.4 ms there), a pull to
    // CPU 2 (0.48 ms). All 3.98 ms were real work; outside the slow cycle
    // each vCPU runs a quarter of the time. Bounded with CPUs 1 and 2 alone
    // (the vCPUs "ran" about 0.6 - 1.1 ms with the samples' margins), about
    // 3 ms looked like the hypervisor's and the window passed. Its 2
    // migrations prove nothing about its CPUs, so all four vCPUs bound it:
    // no hypervisor's share, the engine's own work.
    auto w = makeWindow (failingRun (2));
    const auto slowFrom = w.callbacks[at (w, 47)].wallNs, slowTo = w.callbacks[at (w, 48)].wallNs;
    const auto span = static_cast<double> (slowTo - slowFrom) / 1.0e6;
    const std::vector<double> realWork { 0.25, 0.10 / span, 0.48 / span, 3.4 / span }; // vCPU 0 at a quarter, the others the thread's
    const std::vector<int64_t> moves { slowFrom + 200000, slowFrom + 3800000 }; // 1 -> 3, 3 -> 2
    {
        auto s = samplesFor (w, { {}, moves, true, {} });
        addVpRuntime (s, w, { slowFrom, slowTo, realWork, 0.25, {} });
        withRunner (w, s);
        const auto& r = w.runner[at (w, 48)];
        const auto failed = judgeFailingRun ("3.4 ms of work on vCPU 3, 2 migrations", w);
        const auto twoEnds = vpRunOver (s, w, at (w, 48), { 1, 2 });
        std::cout << "    with vCPUs 1 and 2 alone the bound would read " << static_cast<double> (twoEnds) / 1.0e6 << " ms of run time, a hypervisor's share of "
                  << static_cast<double> (stalls::cycleCpu (w, at (w, 48)) - twoEnds) / 1.0e6 << " ms; with all four " << static_cast<double> (r.vpRunNs) / 1.0e6
                  << " ms, a share of " << static_cast<double> (r.hypervisorNs) / 1.0e6 << " ms\n";
        REQUIRE (r.measured);
        CHECK (r.cpuBefore == 1);
        CHECK (r.cpuAfter == 2);
        CHECK (r.migrations == 2);
        CHECK (! r.cpusProven);
        CHECK (r.vpCpus == 4);
        CHECK (twoEnds < kPeriod); // the trap: its own work by CPUs 1 and 2 alone (CPU time less a share of CPU time - twoEnds), under the period: it passed
        CHECK (r.vpRunNs >= 3980000);
        CHECK (r.hypervisorNs == 0);
        CHECK ((stalls::attribute (w).engineCycles == std::vector<size_t> { at (w, 48) }));
        CHECK (failed.size() == 3);
    }

    // The same with the migrations not read: all four vCPUs, the same verdict.
    {
        auto s = samplesFor (w, { {}, moves, false, {} });
        addVpRuntime (s, w, { slowFrom, slowTo, realWork, 0.25, {} });
        withRunner (w, s);
        const auto& r = w.runner[at (w, 48)];
        const auto failed = judgeFailingRun ("3.4 ms of work on vCPU 3, migrations not read", w);
        CHECK (r.migrations == -1);
        CHECK (r.vpCpus == 4);
        CHECK (r.hypervisorNs == 0);
        CHECK (failed.size() == 3);
    }

    // One migration, 1 -> 2: the thread ran on vCPUs 1 and 2 only, which ran
    // 0.1 and 0.58 ms of the span; the hypervisor took the rest of its
    // 3.98 ms of CPU time (vCPU 3 runs its quarter for someone else).
    // Bounded with those two, at least 2.5 ms was the hypervisor's: the
    // window passes.
    {
        auto s = samplesFor (w, { {}, { slowFrom + 200000 }, true, {} });
        addVpRuntime (s, w, { slowFrom, slowTo, { -1.0, 0.10 / span, 0.58 / span }, 0.25, {} });
        withRunner (w, s);
        const auto& r = w.runner[at (w, 48)];
        const auto failed = judgeFailingRun ("vCPUs 1 and 2 ran 0.68 ms, 1 migration", w);
        REQUIRE (r.measured);
        CHECK (r.migrations == 1);
        CHECK (r.cpusProven);
        CHECK (r.vpCpus == 2);
        CHECK (r.hypervisorNs >= 2500000);
        CHECK (failed.empty());
        CHECK (stalls::attribute (w).events.find ("its vCPUs 1 / 2 ran") != std::string::npos);
    }

    // The /proc sampler held between its stamp and its reads: a sample
    // stamped 10 us before the span, read 1 ms into it, after the move to
    // CPU 3. Bracketed by its stamp it would count 1 migration (3 -> 2) and
    // "prove" CPUs 1 and 2; its reads were not done by the span's start, so
    // the sample before it brackets the span: 2 migrations, all vCPUs.
    {
        auto s = samplesFor (w, { {}, moves, true, { slowFrom - 10000, slowFrom + 1000000 } });
        addVpRuntime (s, w, { slowFrom, slowTo, realWork, 0.25, {} });
        withRunner (w, s);
        const auto& r = w.runner[at (w, 48)];
        const auto failed = judgeFailingRun ("3.4 ms of work on vCPU 3, a held /proc sample", w);
        const auto byStamp = s.migrations[stalls::sampleReadFrom (s.wallNs, w.callbacks[at (w, 48)].doneNs)]
                             - s.migrations[stalls::sampleAtOrBefore (s, slowFrom)];
        std::cout << "    bracketed by its stamp the held sample counts " << byStamp << " migration; by its reads " << r.migrations << "\n";
        CHECK (byStamp == 1);
        CHECK (r.migrations == 2);
        CHECK (! r.cpusProven);
        CHECK (r.hypervisorNs == 0);
        CHECK (failed.size() == 3);
    }
}

TEST_CASE ("App: PipeWire xrun attribution - a late finish with no preemption is excused only when the hypervisor's share covers its overrun (E48, R1.2)")
{
    // Callback 80 done 3.2 ms after its cycle's start (0.53 ms over the
    // period), 2.0 ms of CPU time, no preemption, one wait; the callback
    // before it done late too (1.9 ms), so the interval stays under 1.5
    // periods (3.97 ms) and only the late finish speaks. Its vCPU ran 2.0 ms
    // less 'shareMs' of the span: a share of 0.15 - 0.4 ms (the samples'
    // margins at a quarter rate) leaves it finishing late, a stall only if
    // any share counted (before the 2026-10-25 review); a share of 0.75 -
    // 1.0 ms covers the overrun.
    std::vector<Spec> specs (kCycles);
    specs[2 + 79].doneAfterNs = 1900000;
    specs[2 + 80].doneAfterNs = 3200000;
    specs[2 + 80].cpuNs = 2000000;
    specs[2 + 80].xrun = true;
    specs[2 + 81].doneAfterNs = 3200000 - kPeriod + 540000;
    specs[2 + 81].cpuNs = 540000;
    specs[2 + 81].waits = 0;
    for (const double shareMs : { 0.4, 1.0 })
    {
        auto w = makeWindow (specs);
        const auto from = w.callbacks[at (w, 79)].wallNs, to = w.callbacks[at (w, 80)].wallNs;
        const auto span = static_cast<double> (to - from) / 1.0e6;
        auto s = samplesFor (w);
        addVpRuntime (s, w, { from, to, { -1.0, (2.0 - shareMs) / span }, 0.25, {} });
        withRunner (w, s);
        const auto& r = w.runner[at (w, 80)];
        const auto a = stalls::attribute (w);
        const auto failed = stalls::judge (a, w, 1, 0, 0);
        print (shareMs < 0.5 ? "a late finish, a hypervisor's share under its overrun" : "a late finish, a hypervisor's share over its overrun", a, failed);
        std::cout << "      callback 80: done " << static_cast<double> (stalls::doneAfter (w, at (w, 80))) / 1.0e6 << " ms after its start, CPU "
                  << static_cast<double> (stalls::cycleCpu (w, at (w, 80))) / 1.0e6 << " ms, its vCPU ran " << static_cast<double> (r.vpRunNs) / 1.0e6
                  << " ms around it, a hypervisor's share of " << static_cast<double> (r.hypervisorNs) / 1.0e6 << " ms\n";
        REQUIRE (r.measured);
        CHECK (r.cpusProven);
        CHECK (stalls::wallGap (w, at (w, 80)) < static_cast<int64_t> (1.5 * kPeriodNs));
        CHECK (stalls::preemptionsOf (w, at (w, 80)) == 0);
        CHECK (stalls::finishedLate (w, at (w, 80)));
        CHECK (a.finishesSeen == 1);
        CHECK (a.engineCycles.empty()); // 2 ms of CPU: under the period either way
        if (shareMs < 0.5)
        {
            CHECK (r.hypervisorNs > 100000); // a share, but under the 0.53 ms overrun
            CHECK (r.hypervisorNs < 450000);
            CHECK (! stalls::isStall (w, at (w, 80), a.usualWaits));
            CHECK (a.finishesStalled == 0);
            CHECK (! failed.empty());
        }
        else
        {
            CHECK (r.hypervisorNs > 700000);
            CHECK (stalls::isStall (w, at (w, 80), a.usualWaits));
            CHECK (a.finishesStalled == 1);
            CHECK (failed.empty());
        }
    }
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
    // The thread stayed on CPU 1 (0 migrations), so vCPU 1 alone bounds it:
    // the other vCPUs' run time cannot hide a wrong bracket.
    auto w = makeWindow (failingRun());
    const auto slowFrom = w.callbacks[at (w, 47)].wallNs, slowTo = w.callbacks[at (w, 48)].wallNs;
    const auto span = static_cast<double> (slowTo - slowFrom) / 1.0e6;
    auto s = samplesFor (w);
    addVpRuntime (s, w, { slowFrom, slowTo, { -1.0, 4.2 / span }, 0.25, { slowFrom - 10000, slowTo - 100000 } });
    withRunner (w, s);
    const auto& r = w.runner[at (w, 48)];
    const auto a = stalls::attribute (w);
    const auto failed = stalls::judge (a, w, 1, 0, 1);
    print ("a held sample before the slow cycle", a, failed);
    REQUIRE (r.measured);
    CHECK (r.cpusProven);
    CHECK (r.vpCpus == 1);
    CHECK (r.vpRunNs >= 4200000); // the work is inside the bracket
    CHECK (r.hypervisorNs == 0);
    CHECK ((a.engineCycles == std::vector<size_t> { at (w, 48) }));
    CHECK (! failed.empty());

    // What a bracket by first stamps alone would have read: from the held
    // sample to the first one stamped at the span's end or later.
    const auto heldAt = static_cast<size_t> (std::find (s.vpWallNs.begin(), s.vpWallNs.end(), slowFrom - 10000) - s.vpWallNs.begin());
    const auto afterAt = stalls::sampleReadFrom (s.vpWallNs, w.callbacks[at (w, 48)].doneNs);
    REQUIRE (heldAt < s.vpWallNs.size());
    REQUIRE (afterAt < s.vpWallNs.size());
    const auto heldRun = s.vpRun (afterAt, 1) - s.vpRun (heldAt, 1);
    std::cout << "    bracketed by first stamps: the vCPU ran " << static_cast<double> (heldRun) / 1.0e6 << " ms, a hypervisor's share of "
              << static_cast<double> (stalls::cycleCpu (w, at (w, 48)) - heldRun) / 1.0e6 << " ms; by the reads: ran " << static_cast<double> (r.vpRunNs) / 1.0e6
              << " ms, share " << static_cast<double> (r.hypervisorNs) / 1.0e6 << " ms\n";
    CHECK (heldRun < 300000);
    CHECK (stalls::cycleCpu (w, at (w, 48)) - heldRun >= 3600000);
}
