// Flubsound Pro - loudness-matched comparisons (docs/11 E37): the header's
// A/B, a module card's "listen without it" (the virtualiser's too) and the
// blind A/B/X test.
//
// One rule for all of them (the ComparisonMatcher rule of the bypass): only
// the LOUDER side is turned down, to the quieter one, never the other way
// round, through EngineController::setComparisonTrimDb (a gain in the mix
// after the chain, click-free). The offsets come from two sources:
//   * PresetLoudnessEstimator: the chain gain of a set of values on a
//     reference programme at the level the strip's input has (about 0.2 s of
//     background work, cached), so a comparison is matched from its first
//     second;
//   * the strip's own meters (Phase 2, "refined"): while a bank plays, its
//     gain on the real programme is short-term out minus in (with the input
//     gain and Auto Level taken off again, as the estimates see the input),
//     averaged over every poll once the bank has played unchanged for
//     kLiveSettleSeconds. When both banks have such a reading at about the
//     same level, they replace the estimates.
//
// BankComparison: per strip, the gain of bank A and of bank B. A flip (the
// header's A/B, or any setActiveBank / toggleAB the poll notices) applies
// the trim of the bank now playing at once; kLiveSettleSeconds later it is
// refined once from the live readings, then frozen until the next flip, so
// an edit is heard at its own loudness. Identical banks get no trim. A
// preset loaded into the strip ends the comparison: its trim is released
// (a loaded preset plays at its own level) until the next flip. Off
// (AppSettings compare.matched) releases every trim.
//
// ListenMatch: the module card's ear. The gain of the strip's values with
// the module audition-bypassed against without; the estimate is requested
// when the pointer enters the ear, so the first hold is matched. While the
// ear is held, the "without" side is trimmed when it is the louder one;
// when it is the quieter one, the "with" side is trimmed from the release
// until kSessionEndSeconds after the last hold (as the bypass matcher's
// comparison session), then the trim goes back to 0 dB at kReleaseDbPerSec.
//
// AbxTest: the blind test of the two banks. Each trial hides a random bank
// behind X; the listener plays A, B and X as often as they like (matched by
// BankComparison) and says which one X is. The result is the number right
// out of the trials and the one-sided binomial p-value (the chance of doing
// as well by guessing). The panel (AbxPanel) covers the window, so neither
// the A/B buttons nor the trim readout gives X away.
//
// Message thread only.
#pragma once

#include "PresetAudition.h"
#include "engine/EngineController.h"

#include <juce_events/juce_events.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace flub::app::ui
{
using EstimatorProvider = std::function<std::shared_ptr<PresetLoudnessEstimator>()>;

/** Only the louder side is turned down, to the quieter one: the trims of
    side 0 and side 1 (both <= 0 dB, at most kMaxComparisonTrimDb). Non-finite
    gains give no trim. */
struct MatchTrims
{
    float first = 0.0f, second = 0.0f;
};
MatchTrims matchTrims (float firstGainLu, float secondGainLu) noexcept;

/** A strip's current values as the comparisons estimate them: the active or
    given bank, with the master bypass and the loudness-matched bypass
    (app state that does not change the processed sound) at 0. */
std::vector<float> comparisonValues (const flub::param::ParameterStore& store, flub::param::Bank bank);

/** The level the strip's programme arrives at (LUFS, before the input gain
    and Auto Level, as the estimates see it); nullopt while the input is
    below -60 LUFS. */
std::optional<float> programmeLevel (EngineController& controller, int strip);

// =============================================================================
class BankComparison final : private juce::Timer, private EngineController::Listener
{
public:
    static constexpr double kLiveSettleSeconds = 3.5; // the 3 s short-term window, plus margin
    static constexpr float kLiveLevelToleranceLu = 3.0f;
    static constexpr int kPollHz = 10;

    BankComparison (EngineController& controller, EstimatorProvider estimator);
    ~BankComparison() override;

    /** compare.matched (persisted by the owner). Off releases every trim. */
    void setEnabled (bool shouldMatch);
    bool isEnabled() const noexcept { return enabled; }

    /** The seconds clock (default: wall time); tests pass rendered time. */
    void setClock (std::function<double()> clock);

    /** One poll: notices flips, reads the live gains, refines, releases.
        The timer calls it at kPollHz; tests call it directly. */
    void poll();

    struct Status
    {
        bool banksDiffer = false;  // A and B do not hold the same sound
        bool comparing = false;    // a flip happened since the strip's last preset load
        bool pending = false;      // waiting for an estimate
        bool live = false;         // the trim comes from the strip's own meters
        float trimDb = 0.0f;       // the trim on the bank playing (<= 0)
        float gapLu = 0.0f;        // playing minus the other bank (LU), when known
        bool gapKnown = false;
        flub::param::Bank playing = flub::param::Bank::A;
    };
    Status getStatus (int strip) const;
    /** "B plays 3.1 dB down: it is 3.1 LU louder than A (estimate)", "A and B
        are matched (A is the quieter)", ... for the header's tooltip. */
    static juce::String describe (const Status& status);
    /** The header's short readout: "B -3.1 dB", "matched", "" when nothing applies. */
    static juce::String shortText (const Status& status);

    /** After a status change (trim applied / released, estimate arrived). */
    std::function<void()> onStatusChanged;

    /** The gain the comparison uses for a bank (LU), and whether it is live. */
    std::optional<float> gainOf (int strip, flub::param::Bank bank, bool* live = nullptr) const;

private:
    struct Live
    {
        uint64_t key = 0;
        float gainLu = 0.0f;    // mean over the readings
        float levelLufs = 0.0f; // mean programme level over them
        double gainSum = 0.0, levelSum = 0.0;
        int readings = 0;
        bool valid = false;
    };
    struct StripState
    {
        flub::param::Bank playing = flub::param::Bank::A;
        bool known = false, comparing = false, refined = false, pending = false, live = false;
        std::array<uint64_t, 2> key {};
        std::array<double, 2> unchangedSince {};
        std::array<Live, 2> liveGain {};
        double flippedAt = 0.0;
        float levelLufs = PresetLoudnessEstimator::kDefaultLevelLufs;
        bool levelKnown = false;
        float gapLu = 0.0f;
        bool gapKnown = false;
        juce::String presetId;
    };

    void timerCallback() override { poll(); }
    void engineControllerChanged (EngineController::Change change) override;
    void flipped (int strip, flub::param::Bank bank);
    void applyMatch (int strip, bool allowLive);
    void release (int strip);
    void releaseAll();
    std::shared_ptr<PresetLoudnessEstimator> estimator();
    double now() const;

    EngineController& controller;
    EstimatorProvider estimatorProvider;
    std::shared_ptr<PresetLoudnessEstimator> listened; // the estimator we listen to
    int listenerToken = 0;
    std::function<double()> clock;
    std::array<StripState, AudioEngineHost::kMaxStrips> strips {};
    bool enabled = true;
    uint32_t generation = 0;

    JUCE_DECLARE_NON_COPYABLE (BankComparison)
};

// =============================================================================
class ListenMatch final : private juce::Timer
{
public:
    static constexpr double kSessionEndSeconds = 10.0;
    static constexpr float kReleaseDbPerSec = 2.0f;

    ListenMatch (EngineController& controller, EstimatorProvider estimator);
    ~ListenMatch() override;

    void setEnabled (bool shouldMatch);
    bool isEnabled() const noexcept { return enabled; }
    void setClock (std::function<double()> clock);

    /** The pointer is over a module's ear: estimate both sides now. */
    void prepare (int strip, int enableParamId);
    /** The ear is held (true) or released (false). */
    void listen (int strip, int enableParamId, bool held);
    /** Ends the session at once (strip switch, engine rebuilt): trim to 0 dB. */
    void reset();

    /** The trim applied now (dB <= 0) and the gap it closes (without minus
        with, LU) when known. */
    float getTrimDb() const noexcept { return appliedTrim; }
    std::optional<float> getGapLu() const noexcept { return gap; }

    /** One step of the session (the timer calls it at 10 Hz). */
    void poll();

private:
    void timerCallback() override { poll(); }
    void update();
    std::shared_ptr<PresetLoudnessEstimator> estimator();
    double now() const;

    EngineController& controller;
    EstimatorProvider estimatorProvider;
    std::shared_ptr<PresetLoudnessEstimator> listened;
    int listenerToken = 0;
    std::function<double()> clock;
    bool enabled = true, held = false, active = false;
    int strip = -1, moduleId = -1;
    double releasedAt = 0.0;
    float preparedLevel = PresetLoudnessEstimator::kDefaultLevelLufs, sessionLevel = PresetLoudnessEstimator::kDefaultLevelLufs;
    float appliedTrim = 0.0f;
    std::optional<float> gap;

    JUCE_DECLARE_NON_COPYABLE (ListenMatch)
};

// =============================================================================
class AbxTest
{
public:
    enum class Choice
    {
        A,
        B,
        X
    };

    /** A test of `trials` trials on `strip`; the hidden banks come from `seed`. */
    AbxTest (EngineController& controller, int strip, int trials, int64_t seed);
    /** Puts the bank back that played before the test. */
    ~AbxTest();

    int getStrip() const noexcept { return strip; }
    int getTrials() const noexcept { return trials; }
    /** The trial being answered (0-based); == getTrials() when finished. */
    int getTrial() const noexcept { return static_cast<int> (answers.size()); }
    bool isFinished() const noexcept { return getTrial() >= trials; }
    int getCorrect() const noexcept { return correct; }

    /** Plays A, B or the current trial's X (a bank switch: click-free). */
    void play (Choice choice);
    /** What plays now (A, B or X; X until another is chosen). */
    Choice getPlaying() const noexcept { return playing; }
    /** The listener's answer for the current trial: X is A (true) or B. */
    void answer (bool xIsA);

    /** The one-sided binomial p-value of `correct` out of `trials` at 50 %. */
    static double pValue (int correct, int trials);
    /** "7 of 10 right: p = 0.17, no reliable difference" etc. */
    juce::String describeResult() const;

    /** Tests: the hidden bank of trial `trial`. */
    flub::param::Bank hiddenBank (int trial) const;

private:
    EngineController& controller;
    const int strip, trials;
    flub::param::Bank bankBefore = flub::param::Bank::A;
    std::vector<flub::param::Bank> hidden;
    std::vector<bool> answers;
    int correct = 0;
    Choice playing = Choice::A;
};
} // namespace flub::app::ui
