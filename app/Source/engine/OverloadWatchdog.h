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
        double lastLoad = 0.0;        // load of the latest poll
        double peakLoad = 0.0;        // highest load of the current / last overload
        int hotStreak = 0, calmStreak = 0;
    };

    static constexpr int kMaxWindowPolls = 64;

    OverloadWatchdog() = default;
    explicit OverloadWatchdog (const Config& c) : config (sanitise (c)) {}

    const Config& getConfig() const noexcept { return config; }
    const State& getState() const noexcept { return state; }
    bool isOverloaded() const noexcept { return state.overloaded; }

    /** One poll. Returns the transition this sample caused, if any. */
    Event update (const Sample& s) noexcept
    {
        if (! s.running)
        {
            baseline = -1;
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
        int64_t fresh = 0;
        if (s.glitchCount >= 0)
        {
            if (baseline >= 0)
                fresh = s.glitchCount >= baseline ? s.glitchCount - baseline : s.glitchCount;
            baseline = s.glitchCount;
        }
        state.glitches += static_cast<uint64_t> (fresh);
        pushWindow (fresh);

        const double load = std::clamp (s.load, 0.0, 1.0);
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
        baseline = -1;
        clearWindow();
    }

private:
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
    int64_t baseline = -1;
    std::array<int64_t, kMaxWindowPolls> window {};
    int64_t windowSum = 0;
    size_t windowPos = 0;
};
} // namespace flub::app
