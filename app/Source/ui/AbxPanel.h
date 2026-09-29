// Flubsound Pro - the blind A/B/X test of a strip's two settings banks
// (docs/11 E37), over the whole window.
//
//   +----------------------------------------------------------+
//   | Blind test (A/B/X) - Music strip                   [x]   |
//   | X is A or B, picked at random for each trial ...         |
//   |      [ A ]      [ B ]      [ X ]     (what plays is lit) |
//   |      [ X is A ]            [ X is B ]                    |
//   | Trial 3 of 10   o o * . . . . . . .                      |
//   | (when finished) 9 of 10 right: p = 0.011. You can ...    |
//   |                               [Start again]   [Close]    |
//   +----------------------------------------------------------+
//
// The panel covers the header too: the A/B buttons and the matched-A/B
// readout would otherwise show which bank X is. A and B are loudness
// matched by the header's BankComparison (the louder bank is turned down to
// the quieter one), so a louder setting cannot win the test. Keys: A, B, X
// play; 1 / 2 answer "X is A" / "X is B"; Escape closes. Closing puts the
// bank back that played before the test (AbxTest's destructor).
#pragma once

#include "Comparison.h"
#include "Widgets.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace flub::app::ui
{
class AbxPanel final : public juce::Component
{
public:
    static constexpr int kDefaultTrials = 10;

    /** `matched`: whether the A/B loudness match is on (the panel warns when not). */
    AbxPanel (EngineController& controller, int strip, bool matched, int64_t seed = juce::Random::getSystemRandom().nextInt64());
    ~AbxPanel() override;

    std::function<void()> onClose;

    /** Nullptr when A and B hold the same sound (nothing to test). */
    AbxTest* getTest() noexcept { return test.get(); }
    void restart();

    juce::Button& getPlayButton (AbxTest::Choice choice) noexcept;
    juce::Button& getAnswerButton (bool xIsA) noexcept { return xIsA ? answerA : answerB; }
    juce::Button& getCloseButton() noexcept { return closeButton; }
    juce::String getStatusText() const;

    void paint (juce::Graphics& g) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;
    void parentSizeChanged() override;
    void mouseDown (const juce::MouseEvent& e) override;

    /** The card for a window of `area`. */
    static juce::Rectangle<int> cardBounds (juce::Rectangle<int> area);

private:
    void refresh();
    void close();
    void playChoice (AbxTest::Choice choice);
    void answerX (bool xIsA);

    EngineController& controller;
    const int strip;
    const bool matched;
    juce::Random random;
    std::unique_ptr<AbxTest> test;
    juce::TextButton playA { "A" }, playB { "B" }, playX { "X" };
    juce::TextButton answerA { "X is A" }, answerB { "X is B" };
    juce::TextButton againButton { "Start again" }, closeButton { "Close" };
    juce::Rectangle<int> card, textArea, progressArea, resultArea;
};
} // namespace flub::app::ui
