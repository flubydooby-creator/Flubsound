// Flubsound Pro - automatic profile switching by foreground application
// (roadmap 2.5).
//
// The user writes rules "while <application> is in the foreground, the
// <strip> strip plays <preset> (optionally in Music / Gaming mode)" and
// chooses per rule whether the strip's previous state is put back when the
// application leaves the foreground ("restore on exit", default off: the
// preset stays). EngineController polls platform::ForegroundApp at 2 Hz on
// the message thread and feeds every sample to AutoProfileSwitcher, which
// answers with what to do; the controller carries it out like a preset load
// (parameter writes on the message thread, nothing on the audio thread).
//
// This is the decision logic only: no platform calls, no threads, no clock
// (time is counted in polls).
//
//   * Matching: a rule's executable matches like the app routing map
//     (AppRouting::executablesMatch: case-insensitive, no directory, no
//     ".exe"), or equals the macOS bundle id. The FIRST matching rule wins.
//   * Hysteresis: the foreground must have matched the same rule (or no
//     rule) for switchAfterPolls consecutive polls (2 = 1 s at 2 Hz) before
//     anything happens, so alt-tabbing through a game or a notification
//     stealing the focus for a moment does not flap presets.
//   * One switch at a time: at most one rule is active. A newly stable rule
//     first ends the active one (End, then Apply).
//   * Flubsound's own window in the foreground holds the current state (the
//     user is editing the auto-loaded preset: alt-tabbing from the game to
//     Flubsound must not restore), and so do samples without an answer
//     (no focused window, the desktop, a process that cannot be inspected).
//   * A preset chosen by hand on the rule's strip while the rule is active
//     (cancelStrip) cancels it: nothing is restored later, and the rule is
//     not applied again until another application (or none) has been
//     stable in the foreground.
//   * Tournament mode (docs/11 E55) holds everything: samples are ignored,
//     the active rule stays applied (the sound does not change mid-match)
//     and nothing is applied or ended until it is switched off; the caller
//     stops polling the foreground meanwhile (isTournamentMode).
//   * Rules edited while one is active: the active rule stays if an identical
//     rule is still in the list, otherwise it ends WITHOUT restoring (the
//     strip keeps what it plays). Switching the feature off does the same.
//     A rule cancelled by hand stays cancelled if it is still in the list.
#pragma once

#include "AppRouting.h"
#include "settings/AppSettings.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace flub::app
{
class AutoProfileSwitcher
{
public:
    struct Config
    {
        int switchAfterPolls = 2; // consecutive polls a new foreground must last (2 = 1 s at 2 Hz)
    };

    /** One foreground poll (platform::ForegroundAppInfo, as JUCE strings). */
    struct Sample
    {
        bool valid = false;         // false: no answer (holds the current state)
        bool isThisProcess = false; // Flubsound itself is in front (holds the current state)
        uint32_t processId = 0;
        juce::String executable;    // path or file name
        juce::String bundleId;      // macOS
    };

    struct Action
    {
        enum class Kind
        {
            Apply, // load rule.presetId (+ mode) on rule.stripName; with restoreOnExit keep the strip's state first
            End    // the rule's application left: with restoreOnExit put the kept state back, otherwise just forget it
        };
        Kind kind = Kind::Apply;
        AutoProfileRule rule;
        bool restore = false; // End only: put the kept state back (rule.restoreOnExit and not ended by an edit / switch-off)
    };

    AutoProfileSwitcher() = default;
    explicit AutoProfileSwitcher (const Config& c) : config (sanitise (c)) {}

    const Config& getConfig() const noexcept { return config; }
    const std::vector<AutoProfileRule>& getRules() const noexcept { return rules; }
    bool isEnabled() const noexcept { return enabled; }

    /** The rule currently applied, nullptr if none. */
    const AutoProfileRule* getActiveRule() const noexcept
    {
        return active >= 0 && active < static_cast<int> (rules.size()) ? &rules[static_cast<size_t> (active)] : nullptr;
    }

    static bool matches (const AutoProfileRule& rule, const Sample& s)
    {
        const auto wanted = rule.executable.trim();
        if (wanted.isEmpty())
            return false;
        return (s.executable.isNotEmpty() && AppRouting::executablesMatch (wanted, s.executable))
               || (s.bundleId.isNotEmpty() && wanted.equalsIgnoreCase (s.bundleId));
    }

    /** Index of the first rule matching the sample; -1 for none. */
    int findRule (const Sample& s) const
    {
        for (size_t i = 0; i < rules.size(); ++i)
            if (matches (rules[i], s))
                return static_cast<int> (i);
        return -1;
    }

    /** Replaces the rules. May end the active rule (without restoring). */
    std::vector<Action> setRules (std::vector<AutoProfileRule> newRules)
    {
        std::vector<Action> actions;
        const auto indexIn = [&newRules] (int index, const std::vector<AutoProfileRule>& from)
        {
            if (index < 0 || index >= static_cast<int> (from.size()))
                return -1;
            const auto same = std::find (newRules.begin(), newRules.end(), from[static_cast<size_t> (index)]);
            return same != newRules.end() ? static_cast<int> (same - newRules.begin()) : -1;
        };
        const int newActive = indexIn (active, rules);
        if (const auto* current = getActiveRule(); current != nullptr && newActive < 0)
            actions.push_back ({ Action::Kind::End, *current, false });
        // A rule cancelled by hand stays cancelled through edits of other
        // rules (adding or removing one must not re-apply it).
        const int newSuppressed = indexIn (suppressed, rules);
        rules = std::move (newRules);
        active = newActive;
        suppressed = newSuppressed;
        candidate = kUnknown;
        candidatePolls = 0;
        return actions;
    }

    /** Off: ends the active rule (without restoring) and ignores samples. */
    std::vector<Action> setEnabled (bool shouldBeEnabled)
    {
        std::vector<Action> actions;
        if (enabled == shouldBeEnabled)
            return actions;
        enabled = shouldBeEnabled;
        if (! enabled)
        {
            if (const auto* current = getActiveRule())
                actions.push_back ({ Action::Kind::End, *current, false });
            active = -1;
        }
        suppressed = -1;
        candidate = kUnknown;
        candidatePolls = 0;
        return actions;
    }

    /** Tournament mode (docs/11 E55): holds the current state and ignores
        samples until switched off; the stability count starts over then. No
        actions either way. */
    void setTournamentMode (bool shouldHold) noexcept
    {
        tournament = shouldHold;
        candidate = kUnknown;
        candidatePolls = 0;
    }
    bool isTournamentMode() const noexcept { return tournament; }

    /** A preset was chosen by hand on this strip: an active rule for it is
        cancelled (no restore) and not re-applied while its app stays in front.
        Returns true if a rule was cancelled. */
    bool cancelStrip (const juce::String& stripName)
    {
        const auto* current = getActiveRule();
        if (current == nullptr || ! current->stripName.equalsIgnoreCase (stripName))
            return false;
        suppressed = active;
        active = -1;
        return true;
    }

    /** One poll. Returns the actions to carry out, in order (at most an End
        followed by an Apply). */
    std::vector<Action> update (const Sample& s)
    {
        std::vector<Action> actions;
        if (! enabled || tournament || ! s.valid || s.isThisProcess)
            return actions; // hold

        const int target = findRule (s);
        if (target != candidate)
        {
            candidate = target;
            candidatePolls = 1;
        }
        else if (candidatePolls < std::numeric_limits<int>::max())
        {
            ++candidatePolls;
        }

        if (candidatePolls < config.switchAfterPolls)
            return actions;

        // Stable. A rule cancelled by hand stays off until something else
        // (another application, or none) was stable in front.
        if (suppressed >= 0 && target != suppressed)
            suppressed = -1;
        if (target == active || (suppressed >= 0 && target == suppressed)) // (-1 is "no rule", not "suppressed")
            return actions;

        if (const auto* current = getActiveRule())
            actions.push_back ({ Action::Kind::End, *current, current->restoreOnExit });
        active = target;
        if (const auto* next = getActiveRule())
            actions.push_back ({ Action::Kind::Apply, *next, false });
        return actions;
    }

private:
    static constexpr int kUnknown = -2; // no candidate yet (-1 = "no rule matches")

    static Config sanitise (Config c)
    {
        c.switchAfterPolls = std::max (1, c.switchAfterPolls);
        return c;
    }

    Config config;
    std::vector<AutoProfileRule> rules;
    bool enabled = true;
    bool tournament = false; // docs/11 E55: hold everything
    int active = -1;     // index into rules, -1 = none
    int suppressed = -1; // rule cancelled by hand while its app stays in front
    int candidate = kUnknown;
    int candidatePolls = 0;
};
} // namespace flub::app
