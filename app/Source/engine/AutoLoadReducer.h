// Flubsound Pro - opt-in automatic response to sustained CPU overload.
//
// Settings > Processing > "Reduce processing load automatically when the CPU
// overloads" (AppSettings::getReduceLoadOnOverload, default OFF). When it is
// on and the OverloadWatchdog reports an overload that lasts, the engine is
// stepped down ONE level of this ladder at a time:
//
//   Quality  --> Balanced  --> Low Latency   (bottom: nothing more to drop)
//
// The latency profile is the engine's only load switch: it selects the
// spectral noise gate (Quality only) and the oversampling of the saturator
// and of the maximizer's clipper (2x HQ + 4x HQ / 2x LQ + 4x HQ / 2x LQ +
// 2x LQ) together with the look-ahead times (flub/engine/ProcessingChain.h).
// There is no separate "HQ oversampling" switch, so the ladder is the
// profile order, and Low Latency, the cheapest profile, is the bottom.
//
// This is the decision logic only: plain C++, no JUCE, no threads, no clock
// (time is counted in polls; EngineController polls at 2 Hz). The controller
// applies a step exactly like a user profile change (every strip, both banks,
// on the message thread; AudioEngineHost re-prepares) and reports it.
//
//   * A step needs an overload that has lasted stepAfterPolls polls while the
//     setting is on (6 polls = 3 s at 2 Hz, on top of the watchdog's own 2 s
//     to declare it), and ...
//   * ... at least minPollsBetweenSteps polls (60 = 30 s) since the previous
//     step or manual profile change, so the re-prepared engine (and its own
//     brief dropout) gets time to show whether the step was enough.
//   * Never steps back up (a recovered load would otherwise oscillate):
//     "Restore" is manual (EngineController::restoreLatencyProfile).
//   * A profile the user chose by hand (Settings, or any write that does not
//     match the last step) resets the ladder: that profile is the new
//     starting point and nothing is offered for restoring.
#pragma once

#include "flub/engine/Parameters.h"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace flub::app
{
class AutoLoadReducer
{
public:
    using Profile = flub::param::LatencyProfileValue;

    struct Config
    {
        int stepAfterPolls = 6;        // consecutive overloaded polls before a step
        int minPollsBetweenSteps = 60; // rate limit: polls since the last step / manual change
    };

    struct State
    {
        int steps = 0;                              // automatic steps since the user last chose a profile
        Profile restoreProfile = Profile::Balanced; // steps > 0: the user's profile before the first step
        Profile lastFrom = Profile::Balanced;       // steps > 0: the latest step ...
        Profile lastTo = Profile::Balanced;         // ... from -> to
        uint64_t sessionSteps = 0;                  // every step this session (never reset; tray bubble)
        int overloadPolls = 0;                      // consecutive overloaded polls while enabled
        int pollsSinceChange = 0;                   // polls since the last step / manual change (saturates)
    };

    AutoLoadReducer() { state.pollsSinceChange = config.minPollsBetweenSteps; }
    explicit AutoLoadReducer (const Config& c) : config (sanitise (c)) { state.pollsSinceChange = config.minPollsBetweenSteps; }

    const Config& getConfig() const noexcept { return config; }
    const State& getState() const noexcept { return state; }
    /** True after an automatic step, until the user chooses a profile. */
    bool hasReduced() const noexcept { return state.steps > 0; }

    /** The next cheaper profile of the ladder; nothing at the bottom. */
    static std::optional<Profile> nextLower (Profile p) noexcept
    {
        switch (p)
        {
            case Profile::Quality: return Profile::Balanced;
            case Profile::Balanced: return Profile::LowLatency;
            case Profile::LowLatency: break;
        }
        return std::nullopt;
    }

    /** One poll. `current` is the profile the strips have now. Returns the
        profile to switch to when a step is due (the caller applies it). */
    std::optional<Profile> update (bool enabled, bool overloaded, Profile current) noexcept
    {
        // Someone else changed the profile since our last step: a manual choice.
        if (state.steps > 0 && current != state.lastTo)
            profileChangedByUser();

        state.pollsSinceChange = std::min (state.pollsSinceChange + 1, config.minPollsBetweenSteps);
        state.overloadPolls = enabled && overloaded ? std::min (state.overloadPolls + 1, config.stepAfterPolls) : 0;

        if (state.overloadPolls < config.stepAfterPolls || state.pollsSinceChange < config.minPollsBetweenSteps)
            return std::nullopt;
        const auto next = nextLower (current);
        if (! next.has_value())
            return std::nullopt; // at the bottom: keep reporting the overload, nothing to drop

        if (state.steps == 0)
            state.restoreProfile = current;
        ++state.steps;
        ++state.sessionSteps;
        state.lastFrom = current;
        state.lastTo = *next;
        state.overloadPolls = 0;
        state.pollsSinceChange = 0;
        return next;
    }

    /** The user chose a profile (or restored one): forget the ladder, and
        give that choice the rate limit's time before any further step. */
    void profileChangedByUser() noexcept
    {
        state.steps = 0;
        state.overloadPolls = 0;
        state.pollsSinceChange = 0;
    }

private:
    static Config sanitise (Config c) noexcept
    {
        c.stepAfterPolls = std::max (1, c.stepAfterPolls);
        c.minPollsBetweenSteps = std::max (1, c.minPollsBetweenSteps);
        return c;
    }

    Config config;
    State state;
};
} // namespace flub::app
