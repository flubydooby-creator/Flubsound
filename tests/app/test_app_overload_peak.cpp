// App-level tests: peak-callback monitoring in the overload watchdog and the
// header (docs/11 E45).
//
// The host times every device callback into a flub::CallbackTiming
// (EngineStatus::callbackTiming). Here a CallbackTiming is fed the durations
// and timestamps a callback would measure (no clock, no device), and its
// snapshots travel the real path: EngineController::updateOverloadWatchdog
// (the 2 Hz poll) -> OverloadWatchdog -> HeaderBar::formatCpuReadout /
// describeCpu. The Done-when case: Balanced / 128 samples at 48 kHz, 40 %
// load, a 3x callback every 500 ms, and no glitch count from the backend.
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "engine/OverloadWatchdog.h"
#include "ui/HeaderBar.h"

#include <cstdint>

using namespace flub::app;
using Event = OverloadWatchdog::Event;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 128;
constexpr uint64_t kPeriodNs = static_cast<uint64_t> (kBlock * 1.0e9 / kRate + 0.5); // 2.667 ms
constexpr uint64_t kPollNs = 500000000;                                               // the controller's 2 Hz

EngineController::Options headlessOptions (const flubapptest::TempFolder& temp)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = false;
    return o;
}

struct ChangeCounter final : EngineController::Listener
{
    void engineControllerChanged (EngineController::Change change) override
    {
        if (change == EngineController::Change::Device)
            ++device;
    }
    int device = 0;
};

/** The device callbacks of one poll period: `load` of the period each, one
    callback of `spike` x that every 500 ms (0 = none), and a stall of
    `stallPeriods` extra periods before the first callback when > 0. */
struct FakeCallbacks
{
    flub::CallbackTiming timing;
    uint64_t now = 0;

    void runPoll (double load, double spike, double stallPeriods = 0.0)
    {
        const uint64_t end = now + kPollNs;
        bool spiked = spike <= 0.0;
        if (stallPeriods > 0.0)
            now += static_cast<uint64_t> (stallPeriods * static_cast<double> (kPeriodNs));
        for (; now < end; now += kPeriodNs)
        {
            double l = load;
            if (! spiked && now + kPeriodNs >= end) // the last callback of the poll
            {
                l = load * spike;
                spiked = true;
            }
            timing.record (now, static_cast<uint64_t> (l * static_cast<double> (kPeriodNs)), kPeriodNs);
        }
    }
};

EngineStatus status (const flub::CallbackTiming::Snapshot& timing, double averageLoad)
{
    EngineStatus st;
    st.deviceOpen = true;
    st.running = true;
    st.deviceName = "Fake Timed Device";
    st.cpuLoad = averageLoad;
    st.xruns = -1;   // the backend counts no xruns ...
    st.glitches = 0; // ... and nothing else saw one
    st.callbackTiming = timing;
    return st;
}
} // namespace

TEST_CASE ("App: E45 a 3x callback every 500 ms with no glitch count (-1) starts an overload; the 2 Hz average alone never does")
{
    FakeCallbacks cb;
    OverloadWatchdog peakAware, averageOnly;
    auto previous = cb.timing.snapshot();
    int startedAt = -1;
    for (int poll = 0; poll < 20; ++poll)
    {
        cb.runPoll (0.4, 3.0);
        const auto snap = cb.timing.snapshot();
        const auto window = snap.since (previous);
        previous = snap;

        OverloadWatchdog::Sample s;
        s.running = true;
        s.load = window.meanLoad(); // what a 2 Hz average sees: 0.40
        s.glitchCount = -1;
        CHECK_LE (s.load, 0.41);
        averageOnly.update (s);

        s.peakLoad = window.loadAt (0.999);
        s.overBudget = static_cast<int64_t> (snap.overBudget);
        s.late = static_cast<int64_t> (snap.late);
        CHECK_GE (s.peakLoad, 1.0); // p99.9 of 188 callbacks is the spike's bucket
        if (peakAware.update (s) == Event::OverloadStarted && startedAt < 0)
            startedAt = poll;
    }
    // Caught within the watchdog's time constants, at the 4th poll (2 s): 4
    // hot polls, and 3 over-budget callbacks after the first reading.
    CHECK (startedAt == 3);
    CHECK (peakAware.isOverloaded());
    CHECK (peakAware.getState().glitches == 19); // every spike after the first reading
    CHECK_GE (peakAware.getState().peakLoad, 1.0);
    CHECK_GE (peakAware.getState().lastPeakLoad, 1.0);
    CHECK (! averageOnly.isOverloaded());
    CHECK (averageOnly.getState().episodes == 0);
    CHECK (averageOnly.getState().lastPeakLoad < 0.0);
}

TEST_CASE ("App: E45 the controller feeds p99.9 into the watchdog, and the header shows the peak")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    ChangeCounter changes;
    controller.addListener (&changes);

    // A steady 40 % with no spike: the peak is the load, nothing is hot.
    FakeCallbacks cb;
    cb.runPoll (0.4, 0.0);
    auto st = status (cb.timing.snapshot(), 0.40);
    controller.updateOverloadWatchdog (st);
    CHECK_NEAR (controller.getOverloadState().lastPeakLoad, 0.4, 0.4 * 0.125);
    auto readout = ui::HeaderBar::formatCpuReadout (st, controller.getOverloadState());
    CHECK (readout.caption == "CPU");
    CHECK (readout.value.startsWith ("40% pk "));
    CHECK (readout.compactValue == "40%");
    CHECK (! readout.warn);
    CHECK (ui::HeaderBar::describeCpu (st, controller.getOverloadState()).contains ("; peak "));

    // A 2.4x callback every 500 ms (96 % of the period: no overrun, so no
    // glitch anywhere): amber at once, OVERLOAD after the 4th hot poll.
    for (int poll = 0; poll < 3; ++poll)
    {
        cb.runPoll (0.4, 2.4);
        st = status (cb.timing.snapshot(), 0.40);
        controller.updateOverloadWatchdog (st);
    }
    readout = ui::HeaderBar::formatCpuReadout (st, controller.getOverloadState());
    CHECK (readout.warn);
    CHECK (! readout.overload);
    CHECK (changes.device == 0);
    CHECK (controller.getOverloadState().glitches == 0);
    cb.runPoll (0.4, 2.4);
    st = status (cb.timing.snapshot(), 0.40);
    controller.updateOverloadWatchdog (st);
    CHECK (controller.getOverloadState().overloaded);
    CHECK (changes.device == 1);
    readout = ui::HeaderBar::formatCpuReadout (st, controller.getOverloadState());
    CHECK (readout.overload);
    CHECK (readout.value.startsWith ("40% pk "));
    const auto tip = ui::HeaderBar::describeCpu (st, controller.getOverloadState());
    CHECK (tip.contains ("peak callbacks of 90 % or more"));

    // Back to steady: ten calm polls clear it.
    for (int poll = 0; poll < 10; ++poll)
    {
        cb.runPoll (0.4, 0.0);
        st = status (cb.timing.snapshot(), 0.40);
        controller.updateOverloadWatchdog (st);
    }
    CHECK (! controller.getOverloadState().overloaded);
    CHECK (changes.device == 2);

    // No callback timing at all (the old path): no peak, the value as before.
    const auto plain = status ({}, 0.42);
    controller.updateOverloadWatchdog (plain);
    CHECK (controller.getOverloadState().lastPeakLoad < 0.0);
    CHECK (ui::HeaderBar::formatCpuReadout (plain, controller.getOverloadState()).value == "42%");

    controller.removeListener (&changes);
}

TEST_CASE ("App: E45 discontinuities in the host timestamps count as dropouts on a device that counts no xruns")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));

    // 30 % load, no spike, but three stalls of two periods within 10 polls:
    // three late callbacks, the watchdog's burst rule.
    FakeCallbacks cb;
    cb.runPoll (0.3, 0.0);
    controller.updateOverloadWatchdog (status (cb.timing.snapshot(), 0.30));
    for (int poll = 0; poll < 3; ++poll)
    {
        CHECK (! controller.getOverloadState().overloaded);
        cb.runPoll (0.3, 0.0, 2.0);
        controller.updateOverloadWatchdog (status (cb.timing.snapshot(), 0.30));
    }
    CHECK (cb.timing.snapshot().late == 3);
    CHECK (controller.getOverloadState().glitches == 3);
    CHECK (controller.getOverloadState().overloaded);

    // A device that counts its own xruns counts the same stalls: the late
    // callbacks are not added on top.
    const flubapptest::TempFolder temp2;
    EngineController counted (headlessOptions (temp2));
    FakeCallbacks cb2;
    auto withXruns = [&] {
        auto st = status (cb2.timing.snapshot(), 0.30);
        st.xruns = 0;
        return st;
    };
    cb2.runPoll (0.3, 0.0);
    counted.updateOverloadWatchdog (withXruns());
    for (int poll = 0; poll < 3; ++poll)
    {
        cb2.runPoll (0.3, 0.0, 2.0);
        counted.updateOverloadWatchdog (withXruns());
    }
    CHECK (counted.getOverloadState().glitches == 0);
    CHECK (! counted.getOverloadState().overloaded);
}
