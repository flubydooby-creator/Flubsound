// Flubsound Pro - CPU-overload watchdog (R1.5).
//
// Decides, from periodic samples of the device callback's CPU load and of its
// glitch counter (device xruns plus callbacks that overran their buffer
// period), whether the engine is in SUSTAINED overload. It is the decision
// logic only: plain C++, no JUCE, no threads, no clock. EngineController
// feeds it from its message-thread timer (2 Hz) and reacts to the events.
//
//   Normal --(enterPolls consecutive polls at load >= enterLoad,
//             or >= enterGlitches new glitches within glitchWindowPolls)--> Overloaded
//   Overloaded --(exitPolls consecutive calm polls: load < exitLoad and
//                 no new glitch)--> Normal
//
// Peak callbacks (docs/11 E45). The load a poll is judged by is the higher of
// the average and, when the host times its callbacks (flub::CallbackTiming),
// the p99.9 callback duration of the poll's window over the period: a 2 Hz
// average hides callbacks 3-9x the mean, and one that uses 90 % of its
// period is a callback away from a dropout. The timing also counts glitches
// that no counter reports: callbacks longer than their period (only while
// the device reports no glitch count at all, glitchCount = -1: JUCE's own
// count holds the same callbacks) and late callbacks, discontinuities in the
// host's timestamps (the controller passes those only for a device that
// counts no xruns itself, which would count the same stalls).
//
// Hysteresis in level (0.9 in, 0.75 out) and in time (2 s in, 5 s out at
// 2 Hz) keeps a load hovering around the threshold from flapping between the
// two states. A poll without a running device clears the state (and re-bases
// the glitch counter, as does a counter that went backwards: JUCE restarts it
// with the device).
//
// Policy (docs/01-architecture.md §7): NOTIFY by default. An overload is
// shown in the header's CPU readout (and its tooltip, with the recommended
// action) and counted per session; the engine is not changed. Automatic
// degradation is OPT-IN (Settings > Processing, default off) because every
// step is a structural re-prepare with an audible dropout of its own: then
// AutoLoadReducer steps the latency profile down one level on a lasting
// overload (rate limited, never back up by itself; see AutoLoadReducer.h).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace flub::app
{
class OverloadWatchdog
{
public:
    struct Config
    {
        double enterLoad = 0.9;     // a poll at or above this load is "hot"
        double exitLoad = 0.75;     // recovery needs every poll below this
        int enterPolls = 4;         // consecutive hot polls that start an overload
        int enterGlitches = 3;      // or this many new glitches ...
        int glitchWindowPolls = 10; // ... within this many polls
        int exitPolls = 10;         // consecutive calm polls that end an overload
    };

    struct Sample
    {
        bool running = false;     // device open and callback attached
        double load = 0.0;        // 0..1, the callback's CPU load
        int64_t glitchCount = -1; // cumulative glitch counter, -1 = not reported
        /** The p99.9 callback duration of this poll's window over the period
            (CallbackTiming::Snapshot::loadAt (0.999) of since()); < 0 = not
            measured. Not clamped at 1: a callback can overrun. */
        double peakLoad = -1.0;
        /** Cumulative callback timing counts (CallbackTiming overBudget and
            late); -1 = not measured / not to be counted (see above). */
        int64_t overBudget = -1, late = -1;
    };

    enum class Event
    {
        None,
        OverloadStarted,
        OverloadCleared
    };

    struct State
    {
        bool overloaded = false;
        uint64_t episodes = 0;        // overloads started this session
        uint64_t glitches = 0;        // glitches seen this session (sum of counter increments)
        uint64_t episodeGlitches = 0; // glitches during the current / last overload
        double lastLoad = 0.0;        // load of the latest poll (the higher of the average and the peak)
        double lastPeakLoad = -1.0;   // p99.9 callback of the latest poll's window; < 0 = not measured
        double peakLoad = 0.0;        // highest load of the current / last overload
        int hotStreak = 0, calmStreak = 0;
    };

    static constexpr int kMaxWindowPolls = 64;

    OverloadWatchdog() = default;
    explicit OverloadWatchdog (const Config& c) : config (sanitise (c)) {}

    const Config& getConfig() const noexcept { return config; }
    const State& getState() const noexcept { return state; }
    bool isOverloaded() const noexcept { return state.overloaded; }
    /** Overloaded AND the latest poll was not calm (load >= exitLoad, or a
        fresh glitch). isOverloaded() is the latched hysteresis state, which
        stays true for at least exitPolls polls after any start, including a
        single glitch burst at low load; "how long has the overload lasted"
        (AutoLoadReducer) must count only these polls. */
    bool isStressed() const noexcept { return state.overloaded && state.calmStreak == 0; }

    /** One poll. Returns the transition this sample caused, if any. */
    Event update (const Sample& s) noexcept
    {
        if (! s.running)
        {
            baseline = overBudgetBaseline = lateBaseline = -1;
            state.lastPeakLoad = -1.0;
            clearWindow();
            state.hotStreak = state.calmStreak = 0;
            state.lastLoad = 0.0;
            if (! state.overloaded)
                return Event::None;
            state.overloaded = false;
            return Event::OverloadCleared;
        }

        // New glitches since the previous poll. A counter that went backwards
        // was restarted with the device: everything it shows now is new.
        int64_t fresh = freshSince (s.glitchCount, baseline);
        // The callback timing's own counts (docs/11 E45, see above).
        fresh += freshSince (s.glitchCount < 0 ? s.overBudget : -1, overBudgetBaseline);
        fresh += freshSince (s.late, lateBaseline);
        state.glitches += static_cast<uint64_t> (fresh);
        pushWindow (fresh);

        state.lastPeakLoad = s.peakLoad >= 0.0 ? s.peakLoad : -1.0;
        const double load = std::clamp (std::max (s.load, s.peakLoad), 0.0, 1.0);
        state.lastLoad = load;
        const bool hot = load >= config.enterLoad;
        const bool calm = load < config.exitLoad && fresh == 0;
        state.hotStreak = hot ? state.hotStreak + 1 : 0;
        state.calmStreak = calm ? state.calmStreak + 1 : 0;

        if (! state.overloaded)
        {
            if (state.hotStreak >= config.enterPolls || windowSum >= config.enterGlitches)
            {
                state.overloaded = true;
                ++state.episodes;
                state.episodeGlitches = static_cast<uint64_t> (windowSum);
                state.peakLoad = load;
                state.calmStreak = 0;
                return Event::OverloadStarted;
            }
            return Event::None;
        }

        state.episodeGlitches += static_cast<uint64_t> (fresh);
        state.peakLoad = std::max (state.peakLoad, load);
        if (state.calmStreak >= config.exitPolls)
        {
            state.overloaded = false;
            state.hotStreak = 0;
            clearWindow(); // the glitches that started this episode must not start the next one
            return Event::OverloadCleared;
        }
        return Event::None;
    }

    /** Forgets everything, including the session counters. */
    void reset() noexcept
    {
        state = {};
        baseline = overBudgetBaseline = lateBaseline = -1;
        clearWindow();
    }

private:
    /** New counts of a cumulative counter since its previous reading (none
        for the first reading or a counter that is not reported, < 0). A
        counter that went backwards was restarted: everything it shows now is
        new. */
    static int64_t freshSince (int64_t count, int64_t& previous) noexcept
    {
        if (count < 0)
        {
            previous = -1;
            return 0;
        }
        const int64_t fresh = previous < 0 ? 0 : (count >= previous ? count - previous : count);
        previous = count;
        return fresh;
    }

    static Config sanitise (Config c) noexcept
    {
        c.enterPolls = std::max (1, c.enterPolls);
        c.exitPolls = std::max (1, c.exitPolls);
        c.enterGlitches = std::max (1, c.enterGlitches);
        c.glitchWindowPolls = std::clamp (c.glitchWindowPolls, 1, kMaxWindowPolls);
        c.exitLoad = std::min (c.exitLoad, c.enterLoad);
        return c;
    }

    void pushWindow (int64_t fresh) noexcept
    {
        const auto n = static_cast<size_t> (config.glitchWindowPolls);
        windowSum -= window[windowPos];
        window[windowPos] = fresh;
        windowSum += fresh;
        windowPos = (windowPos + 1) % n;
    }

    void clearWindow() noexcept
    {
        window.fill (0);
        windowSum = 0;
        windowPos = 0;
    }

    Config config;
    State state;
    int64_t baseline = -1, overBudgetBaseline = -1, lateBaseline = -1;
    std::array<int64_t, kMaxWindowPolls> window {};
    int64_t windowSum = 0;
    size_t windowPos = 0;
};
} // namespace flub::app
