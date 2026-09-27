// App-level tests: the opt-in automatic response to sustained CPU overload.
//
// * AutoLoadReducer is the pure decision logic (no JUCE, no threads, no
//   clock): one call per watchdog poll with "enabled", "overloaded" and the
//   strips' current latency profile; it answers with the profile to step to.
// * EngineController::updateOverloadWatchdog is the 2 Hz poll; the tests feed
//   it chosen statuses (no timers, no waiting) with the setting on and off,
//   then check the strips' parameters, the re-prepared chains, the
//   Change::Device notification, the header tooltip / tray text and the
//   Settings > Processing controls (switch, latency box, Restore button).
#include "AppTestSupport.h"

#include "engine/AutoLoadReducer.h"
#include "engine/EngineController.h"
#include "ui/HeaderBar.h"
#include "ui/SettingsDialog.h"

#include <functional>

using namespace flub::app;
using flub::param::Bank;
using flub::param::LatencyProfileValue;
using Profile = LatencyProfileValue;

namespace
{
/** Polls `r` until it asks for a step (or `limit` polls); returns the number
    of polls it took, 0 if it never stepped. */
int pollsUntilStep (AutoLoadReducer& r, bool enabled, bool overloaded, Profile current, int limit, Profile* stepTo = nullptr)
{
    for (int i = 1; i <= limit; ++i)
    {
        if (const auto next = r.update (enabled, overloaded, current))
        {
            if (stepTo != nullptr)
                *stepTo = *next;
            return i;
        }
    }
    return 0;
}

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

EngineStatus runningStatus (double load)
{
    EngineStatus st;
    st.deviceOpen = true;
    st.running = true;
    st.deviceName = "Fake Device";
    st.deviceTypeName = "Fake type";
    st.cpuLoad = load;
    st.xruns = -1;
    return st;
}

struct ChangeCounter final : EngineController::Listener
{
    void engineControllerChanged (EngineController::Change change) override
    {
        if (change == EngineController::Change::Device)
            ++device;
        if (change == EngineController::Change::Settings)
            ++settings;
    }
    int device = 0, settings = 0;
};

/** Every strip, both banks, carries `profile`. */
bool allStripsAt (EngineController& controller, Profile profile)
{
    const float v = static_cast<float> (static_cast<int> (profile));
    for (int s = 0; s < controller.getNumStrips(); ++s)
        for (const auto bank : { Bank::A, Bank::B })
            if (controller.getParams (s).get (bank, flub::param::LatencyProfile) != v)
                return false;
    return true;
}

/** Depth-first search of `root`'s children for the first T matching `match`. */
template <typename T>
T* findChild (juce::Component& root, const std::function<bool (T&)>& match)
{
    for (auto* child : root.getChildren())
    {
        if (auto* c = dynamic_cast<T*> (child); c != nullptr && match (*c))
            return c;
        if (auto* found = findChild<T> (*child, match))
            return found;
    }
    return nullptr;
}

ui::HotkeyHooks noHotkeys()
{
    ui::HotkeyHooks hooks;
    hooks.isSupported = [] { return false; };
    hooks.getFailures = [] { return juce::StringArray(); };
    hooks.reRegister = [] {};
    return hooks;
}
} // namespace

// =============================================================================
// AutoLoadReducer (pure)
// =============================================================================
TEST_CASE ("App: AutoLoadReducer steps one level after 6 overloaded polls along Quality, Balanced, Low Latency and stops at the bottom")
{
    // The ladder.
    CHECK (AutoLoadReducer::nextLower (Profile::Quality) == Profile::Balanced);
    CHECK (AutoLoadReducer::nextLower (Profile::Balanced) == Profile::LowLatency);
    CHECK (! AutoLoadReducer::nextLower (Profile::LowLatency).has_value());

    AutoLoadReducer r; // 6 polls, 60 polls between steps
    CHECK (r.getConfig().stepAfterPolls == 6);
    CHECK (r.getConfig().minPollsBetweenSteps == 60);
    CHECK (! r.hasReduced());

    // Five overloaded polls are not enough; the sixth steps Quality -> Balanced.
    for (int i = 0; i < 5; ++i)
        CHECK (! r.update (true, true, Profile::Quality).has_value());
    CHECK (r.update (true, true, Profile::Quality) == Profile::Balanced);
    CHECK (r.hasReduced());
    CHECK (r.getState().steps == 1);
    CHECK (r.getState().sessionSteps == 1);
    CHECK (r.getState().restoreProfile == Profile::Quality);
    CHECK (r.getState().lastFrom == Profile::Quality);
    CHECK (r.getState().lastTo == Profile::Balanced);

    // A calm poll restarts the count: an overload must last again.
    Profile to = Profile::Quality;
    CHECK (pollsUntilStep (r, true, true, Profile::Balanced, 59) == 0); // rate limit (below)
    CHECK (! r.update (true, false, Profile::Balanced).has_value());
    CHECK (r.getState().overloadPolls == 0);
    CHECK (pollsUntilStep (r, true, true, Profile::Balanced, 100, &to) == 6);
    CHECK (to == Profile::LowLatency);
    CHECK (r.getState().steps == 2);
    CHECK (r.getState().restoreProfile == Profile::Quality); // still the user's choice
    CHECK (r.getState().lastFrom == Profile::Balanced);

    // Low Latency is the bottom: a never-ending overload changes nothing more.
    CHECK (pollsUntilStep (r, true, true, Profile::LowLatency, 1000) == 0);
    CHECK (r.getState().steps == 2);
    CHECK (r.getState().sessionSteps == 2);
    CHECK (r.hasReduced()); // Restore stays on offer
}

TEST_CASE ("App: AutoLoadReducer steps at most once per 60 polls and never steps back up")
{
    AutoLoadReducer r;
    CHECK (pollsUntilStep (r, true, true, Profile::Quality, 10) == 6);

    // Still overloaded on Balanced: the next step waits for 60 polls after
    // the first one, however long the overload has lasted.
    CHECK (pollsUntilStep (r, true, true, Profile::Balanced, 59) == 0);
    CHECK (r.update (true, true, Profile::Balanced) == Profile::LowLatency);

    // A smaller rate limit than the step delay: the step delay rules.
    AutoLoadReducer::Config c;
    c.stepAfterPolls = 4;
    c.minPollsBetweenSteps = 2;
    AutoLoadReducer fast (c);
    CHECK (pollsUntilStep (fast, true, true, Profile::Quality, 10) == 4);
    CHECK (pollsUntilStep (fast, true, true, Profile::Balanced, 10) == 4);

    // Recovered: however long the load stays low, nothing steps back up.
    AutoLoadReducer calm;
    CHECK (pollsUntilStep (calm, true, true, Profile::Quality, 10) == 6);
    CHECK (pollsUntilStep (calm, true, false, Profile::Balanced, 10000) == 0);
    CHECK (calm.getState().lastTo == Profile::Balanced);
    CHECK (calm.hasReduced());

    // Nonsense configurations are sanitised (at least one poll each).
    AutoLoadReducer::Config zero;
    zero.stepAfterPolls = 0;
    zero.minPollsBetweenSteps = -5;
    AutoLoadReducer sane (zero);
    CHECK (sane.getConfig().stepAfterPolls == 1);
    CHECK (sane.getConfig().minPollsBetweenSteps == 1);
    CHECK (! sane.update (true, false, Profile::Quality).has_value());
    CHECK (sane.update (true, true, Profile::Quality) == Profile::Balanced);
}

TEST_CASE ("App: AutoLoadReducer never acts while disabled, and an overload counts only from when it is enabled")
{
    AutoLoadReducer r;
    for (const auto profile : { Profile::Quality, Profile::Balanced, Profile::LowLatency })
        CHECK (pollsUntilStep (r, false, true, profile, 1000) == 0);
    CHECK (! r.hasReduced());
    CHECK (r.getState().sessionSteps == 0);
    CHECK (r.getState().overloadPolls == 0);

    // Switched on in the middle of an overload: six more polls, not one.
    CHECK (pollsUntilStep (r, true, true, Profile::Quality, 10) == 6);

    // Switched off again after a step: no further step, the Restore offer stays.
    CHECK (pollsUntilStep (r, false, true, Profile::Balanced, 1000) == 0);
    CHECK (r.getState().steps == 1);
    CHECK (r.hasReduced());
}

TEST_CASE ("App: AutoLoadReducer resets its ladder when the profile is changed by hand and holds off for the rate limit")
{
    // Explicit: the controller calls profileChangedByUser() for Settings
    // changes and for Restore.
    AutoLoadReducer r;
    CHECK (pollsUntilStep (r, true, true, Profile::Quality, 10) == 6);
    CHECK (r.hasReduced());
    r.profileChangedByUser();
    CHECK (! r.hasReduced());
    CHECK (r.getState().steps == 0);
    CHECK (r.getState().sessionSteps == 1); // the session count is not a ladder
    // The user's new choice (Quality again) gets 60 polls before a new step,
    // which then starts a new ladder from it.
    CHECK (pollsUntilStep (r, true, true, Profile::Quality, 59) == 0);
    CHECK (r.update (true, true, Profile::Quality) == Profile::Balanced);
    CHECK (r.getState().steps == 1);
    CHECK (r.getState().restoreProfile == Profile::Quality);

    // Implicit: a profile that does not match the last step was written by
    // someone else (a manual change that bypassed the controller).
    AutoLoadReducer s;
    CHECK (pollsUntilStep (s, true, true, Profile::Quality, 10) == 6); // -> Balanced
    CHECK (! s.update (true, false, Profile::Balanced).has_value());
    CHECK (s.hasReduced());
    CHECK (! s.update (true, true, Profile::LowLatency).has_value()); // the user picked Low Latency
    CHECK (! s.hasReduced());
    CHECK (s.getState().steps == 0);
    CHECK (pollsUntilStep (s, true, true, Profile::LowLatency, 1000) == 0); // and that is the bottom
}

// =============================================================================
// EngineController + header / tray / Settings
// =============================================================================
TEST_CASE ("App: overload response on: EngineController steps the latency profile down and the header, tray text and Settings show it")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    ChangeCounter changes;
    controller.addListener (&changes);
    auto& host = controller.getHost();

    ui::SettingsDialog settings (controller, noHotkeys(), [] (ui::MeterPalette) {}, ui::MeterPalette::Standard);
    settings.setSize (ui::SettingsDialog::kMinWidth, ui::SettingsDialog::kMinHeight);
    settings.showPage (ui::SettingsDialog::Page::Processing);
    auto* toggle = findChild<juce::ToggleButton> (settings, [] (juce::ToggleButton& b)
                                                  { return b.getButtonText() == "Reduce processing load automatically when the CPU overloads"; });
    auto* restore = findChild<juce::TextButton> (settings, [] (juce::TextButton& b) { return b.getButtonText() == "Restore"; });
    auto* latencyBox = findChild<juce::ComboBox> (settings, [] (juce::ComboBox& b) { return b.getTitle() == "Latency profile"; });
    REQUIRE (toggle != nullptr);
    REQUIRE (restore != nullptr);
    REQUIRE (latencyBox != nullptr);

    // Default: off, nothing reduced, Restore disabled.
    CHECK (! controller.getReduceLoadOnOverload());
    CHECK (! toggle->getToggleState());
    CHECK (! restore->isEnabled());
    CHECK (controller.describeLoadReduction().isEmpty());

    // The user picks Quality in Settings (the same path as ever: every strip,
    // both banks; the engine re-prepares on the message thread).
    latencyBox->setSelectedId (static_cast<int> (Profile::Quality) + 1, juce::sendNotificationSync);
    CHECK (controller.getLatencyProfile() == Profile::Quality);
    CHECK (allStripsAt (controller, Profile::Quality));
    CHECK (host.needsReprepare());
    host.reconfigure();
    CHECK (controller.getChain (0).getLatencyProfile() == Profile::Quality);

    // ... and switches the response on.
    const int settingsBefore = changes.settings;
    toggle->setToggleState (true, juce::sendNotificationSync);
    CHECK (controller.getReduceLoadOnOverload());
    CHECK (controller.getSettings().getReduceLoadOnOverload());
    CHECK (changes.settings == settingsBefore + 1);

    // A sustained 97 % load: the watchdog declares the overload on poll 4
    // (one Change::Device), and the step waits for the rate limit that the
    // manual choice started (60 polls = 30 s at 2 Hz).
    const auto hot = runningStatus (0.97);
    for (int i = 0; i < 59; ++i)
        controller.updateOverloadWatchdog (hot);
    CHECK (controller.getOverloadState().overloaded);
    CHECK (changes.device == 1);
    CHECK (allStripsAt (controller, Profile::Quality));
    CHECK (! controller.hasReducedLoad());

    controller.updateOverloadWatchdog (hot);
    CHECK (changes.device == 2); // the step is broadcast like an overload edge
    CHECK (controller.hasReducedLoad());
    CHECK (controller.getLatencyProfile() == Profile::Balanced);
    CHECK (allStripsAt (controller, Profile::Balanced));
    CHECK (controller.getLoadReductionState().sessionSteps == 1);

    // Only parameters were written: the chains change when AudioEngineHost's
    // message-thread poll re-prepares the engine (here: called directly).
    CHECK (controller.getChain (0).getLatencyProfile() == Profile::Quality);
    CHECK (host.needsReprepare());
    host.reconfigure();
    CHECK (! host.needsReprepare());
    for (int s = 0; s < controller.getNumStrips(); ++s)
        CHECK (controller.getChain (s).getLatencyProfile() == Profile::Balanced);

    // What was changed: header tooltip, tray bubble text, Settings.
    const auto text = controller.describeLoadReduction();
    CHECK (text == "Processing load reduced automatically after a sustained CPU overload: latency profile Quality -> Balanced. "
                   "Restore Quality in Settings > Processing.");
    const auto tip = ui::HeaderBar::describeCpu (hot, controller.getOverloadState(), text);
    CHECK (tip.contains ("Overload:"));
    CHECK (tip.endsWith ("\n" + text));
    CHECK (ui::HeaderBar::describeCpu (EngineStatus {}, controller.getOverloadState(), text) == "CPU: no audio device open\n" + text);
    CHECK (ui::HeaderBar::describeCpu (hot, controller.getOverloadState()) == tip.upToLastOccurrenceOf ("\n", false, false));
    settings.showPage (ui::SettingsDialog::Page::Processing); // refreshes the page
    CHECK (restore->isEnabled());
    CHECK (latencyBox->getSelectedId() == static_cast<int> (Profile::Balanced) + 1);
    CHECK (toggle->getToggleState());

    // Still overloaded: one level per 60 polls, then the bottom.
    for (int i = 0; i < 59; ++i)
        controller.updateOverloadWatchdog (hot);
    CHECK (controller.getLatencyProfile() == Profile::Balanced);
    controller.updateOverloadWatchdog (hot);
    CHECK (controller.getLatencyProfile() == Profile::LowLatency);
    CHECK (changes.device == 3);
    CHECK (controller.describeLoadReduction().contains ("latency profile Quality -> Balanced -> Low Latency. Restore Quality"));
    for (int i = 0; i < 300; ++i)
        controller.updateOverloadWatchdog (hot);
    CHECK (controller.getLatencyProfile() == Profile::LowLatency);
    CHECK (changes.device == 3);
    CHECK (controller.getLoadReductionState().sessionSteps == 2);

    // The load recovers: the overload ends, the profile stays (no step back up).
    const auto calm = runningStatus (0.3);
    for (int i = 0; i < 200; ++i)
        controller.updateOverloadWatchdog (calm);
    CHECK (! controller.getOverloadState().overloaded);
    CHECK (changes.device == 4);
    CHECK (controller.getLatencyProfile() == Profile::LowLatency);
    CHECK (controller.hasReducedLoad());

    // Restore (manual): back to the user's Quality, the ladder is reset.
    REQUIRE (restore->onClick != nullptr);
    restore->onClick();
    CHECK (controller.getLatencyProfile() == Profile::Quality);
    CHECK (allStripsAt (controller, Profile::Quality));
    CHECK (! controller.hasReducedLoad());
    CHECK (controller.describeLoadReduction().isEmpty());
    CHECK (! restore->isEnabled());
    CHECK (latencyBox->getSelectedId() == static_cast<int> (Profile::Quality) + 1);

    controller.removeListener (&changes);
}

TEST_CASE ("App: overload response: a profile chosen by hand resets the ladder, and with the setting off nothing changes")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    ChangeCounter changes;
    controller.addListener (&changes);
    const auto hot = runningStatus (0.97);

    // Off (the default): an overload that lasts minutes changes nothing but
    // the readout (two Change::Device edges).
    controller.setLatencyProfile (Profile::Quality);
    for (int i = 0; i < 1000; ++i)
        controller.updateOverloadWatchdog (hot);
    CHECK (controller.getOverloadState().overloaded);
    CHECK (changes.device == 1);
    CHECK (allStripsAt (controller, Profile::Quality));
    CHECK (! controller.hasReducedLoad());
    CHECK (controller.getLoadReductionState().sessionSteps == 0);
    CHECK (controller.describeLoadReduction().isEmpty());
    CHECK (! ui::HeaderBar::describeCpu (hot, controller.getOverloadState(), controller.describeLoadReduction()).contains ("reduced"));

    // On: the running overload now leads to a step after 6 polls (the rate
    // limit of the manual choice above has long passed).
    controller.setReduceLoadOnOverload (true);
    for (int i = 0; i < 5; ++i)
        controller.updateOverloadWatchdog (hot);
    CHECK (allStripsAt (controller, Profile::Quality));
    controller.updateOverloadWatchdog (hot);
    CHECK (allStripsAt (controller, Profile::Balanced));
    CHECK (controller.hasReducedLoad());

    // The user picks Quality again by hand: the ladder resets (nothing to
    // restore) and that choice is kept for the rate limit, then a new ladder
    // starts from it.
    controller.setLatencyProfile (Profile::Quality);
    CHECK (! controller.hasReducedLoad());
    CHECK (controller.describeLoadReduction().isEmpty());
    for (int i = 0; i < 59; ++i)
        controller.updateOverloadWatchdog (hot);
    CHECK (allStripsAt (controller, Profile::Quality));
    controller.updateOverloadWatchdog (hot);
    CHECK (allStripsAt (controller, Profile::Balanced));
    CHECK (controller.getLoadReductionState().restoreProfile == Profile::Quality);

    // A write that bypasses the controller (any other editor of the stores)
    // is also a manual choice: the ladder resets on the next poll.
    for (int s = 0; s < controller.getNumStrips(); ++s)
        for (const auto bank : { Bank::A, Bank::B })
            controller.getParams (s).set (bank, flub::param::LatencyProfile, static_cast<float> (static_cast<int> (Profile::LowLatency)));
    controller.updateOverloadWatchdog (hot);
    CHECK (! controller.hasReducedLoad());
    CHECK (allStripsAt (controller, Profile::LowLatency));

    // Off again: whatever the overload, nothing moves.
    controller.setReduceLoadOnOverload (false);
    controller.setLatencyProfile (Profile::Quality);
    const auto stepsBefore = controller.getLoadReductionState().sessionSteps;
    for (int i = 0; i < 1000; ++i)
        controller.updateOverloadWatchdog (hot);
    CHECK (allStripsAt (controller, Profile::Quality));
    CHECK (controller.getLoadReductionState().sessionSteps == stepsBefore);

    controller.removeListener (&changes);
}

TEST_CASE ("App: overload response: one xrun burst at low load starts an overload but never steps; glitching or hot polls that go on still do")
{
    // Regression (hardening, watchdog): the reducer used to count the
    // watchdog's LATCHED overload, which lasts at least 10 polls after any
    // start, so the "6 more polls" filter never filtered: 3 glitches at 30 %
    // load stepped Quality -> Balanced 5 polls later.
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    controller.setLatencyProfile (Profile::Quality);
    controller.setReduceLoadOnOverload (true);

    auto status = runningStatus (0.30);
    status.glitches = 0;
    const auto poll = [&controller, &status] (int newGlitches)
    {
        status.glitches += newGlitches;
        controller.updateOverloadWatchdog (status);
    };

    // Let the rate limit of the manual choice above pass (60 calm polls).
    for (int i = 0; i < 60; ++i)
        poll (0);
    CHECK (! controller.getOverloadState().overloaded);

    // A disk / driver hiccup: 3 glitches in one poll at 30 % load.
    poll (3);
    CHECK (controller.getOverloadState().overloaded); // reported, as before
    for (int i = 0; i < 9; ++i)
    {
        poll (0);
        CHECK (controller.getOverloadState().overloaded); // latched for 10 calm polls ...
        CHECK (allStripsAt (controller, Profile::Quality)); // ... but no step
    }
    poll (0);
    CHECK (! controller.getOverloadState().overloaded);
    for (int i = 0; i < 100; ++i)
        poll (0);
    CHECK (allStripsAt (controller, Profile::Quality));
    CHECK (! controller.hasReducedLoad());
    CHECK (controller.getLoadReductionState().sessionSteps == 0);

    // Glitches that keep coming at low load: each poll is stressed, and after
    // the start poll plus 5 more the profile steps down.
    poll (3);
    CHECK (controller.getOverloadState().overloaded);
    for (int i = 0; i < 4; ++i)
        poll (1);
    CHECK (allStripsAt (controller, Profile::Quality));
    poll (1);
    CHECK (allStripsAt (controller, Profile::Balanced));
    CHECK (controller.getLoadReductionState().sessionSteps == 1);

    // A calm poll in the middle of an episode restarts the count.
    controller.setLatencyProfile (Profile::Quality);
    for (int i = 0; i < 60; ++i)
        poll (0); // ends the episode and lets the rate limit pass
    CHECK (! controller.getOverloadState().overloaded);
    status.cpuLoad = 0.97;
    for (int i = 0; i < 4; ++i)
        poll (0); // hot: the overload starts on the 4th poll (stressed 1)
    CHECK (controller.getOverloadState().overloaded);
    for (int i = 0; i < 4; ++i)
        poll (0); // stressed 2..5
    status.cpuLoad = 0.30;
    poll (0); // calm: still overloaded, count back to 0
    CHECK (controller.getOverloadState().overloaded);
    status.cpuLoad = 0.97;
    for (int i = 0; i < 5; ++i)
        poll (0);
    CHECK (allStripsAt (controller, Profile::Quality));
    poll (0);
    CHECK (allStripsAt (controller, Profile::Balanced));

    // The pure pieces agree: isStressed() is the latched state AND a poll
    // that was not calm.
    OverloadWatchdog watchdog;
    OverloadWatchdog::Sample sample;
    sample.running = true;
    sample.load = 0.3;
    sample.glitchCount = 0;
    watchdog.update (sample);
    sample.glitchCount = 3;
    CHECK (watchdog.update (sample) == OverloadWatchdog::Event::OverloadStarted);
    CHECK (watchdog.isStressed());
    watchdog.update (sample);
    CHECK (watchdog.isOverloaded());
    CHECK (! watchdog.isStressed());
    sample.load = 0.8; // between the thresholds: not calm
    watchdog.update (sample);
    CHECK (watchdog.isStressed());
}
