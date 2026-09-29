#include "AbxPanel.h"

#include "Theme.h"

namespace flub::app::ui
{
using namespace flub::param;

AbxPanel::AbxPanel (EngineController& c, int s, bool isMatched, int64_t seed)
    : controller (c), strip (s), matched (isMatched), random (seed)
{
    setTitle ("Blind test (A/B/X)");
    setOpaque (true);
    setWantsKeyboardFocus (true);

    for (auto* b : { &playA, &playB, &playX })
    {
        Style::set (*b, "tab");
        b->setClickingTogglesState (false);
        addAndMakeVisible (*b);
    }
    Style::describe (playA, "Play A", "Play settings A (key A)");
    Style::describe (playB, "Play B", "Play settings B (key B)");
    Style::describe (playX, "Play X", "Play X: A or B, hidden (key X)");
    playA.onClick = [this] { playChoice (AbxTest::Choice::A); };
    playB.onClick = [this] { playChoice (AbxTest::Choice::B); };
    playX.onClick = [this] { playChoice (AbxTest::Choice::X); };

    Style::describe (answerA, "X is A", "Answer: X is A (key 1)");
    Style::describe (answerB, "X is B", "Answer: X is B (key 2)");
    answerA.onClick = [this] { answerX (true); };
    answerB.onClick = [this] { answerX (false); };
    addAndMakeVisible (answerA);
    addAndMakeVisible (answerB);

    againButton.onClick = [this] { restart(); };
    closeButton.onClick = [this] { close(); };
    Style::describe (closeButton, "Close", "End the test and go back to the settings you were listening to (Escape)");
    addAndMakeVisible (againButton);
    addAndMakeVisible (closeButton);

    restart();
}

AbxPanel::~AbxPanel() = default;

void AbxPanel::restart()
{
    test.reset(); // puts the bank back first
    const auto& store = controller.getParams (strip);
    if (comparisonValues (store, Bank::A) != comparisonValues (store, Bank::B))
        test = std::make_unique<AbxTest> (controller, strip, kDefaultTrials, random.nextInt64());
    refresh();
}

juce::Button& AbxPanel::getPlayButton (AbxTest::Choice choice) noexcept
{
    return choice == AbxTest::Choice::A ? static_cast<juce::Button&> (playA)
                                        : choice == AbxTest::Choice::B ? static_cast<juce::Button&> (playB) : static_cast<juce::Button&> (playX);
}

juce::String AbxPanel::getStatusText() const
{
    if (test == nullptr)
        return "A and B hold the same sound: change or load something into one of them first.";
    if (test->isFinished())
        return test->describeResult();
    return "Trial " + juce::String (test->getTrial() + 1) + " of " + juce::String (test->getTrials()) + ": which one is X?";
}

void AbxPanel::refresh()
{
    const bool running = test != nullptr && ! test->isFinished();
    for (auto* b : { &playA, &playB, &playX })
        b->setEnabled (running);
    answerA.setEnabled (running);
    answerB.setEnabled (running);
    if (test != nullptr)
    {
        playA.setToggleState (running && test->getPlaying() == AbxTest::Choice::A, juce::dontSendNotification);
        playB.setToggleState (running && test->getPlaying() == AbxTest::Choice::B, juce::dontSendNotification);
        playX.setToggleState (running && test->getPlaying() == AbxTest::Choice::X, juce::dontSendNotification);
    }
    againButton.setVisible (test != nullptr && test->isFinished());
    repaint();
}

void AbxPanel::playChoice (AbxTest::Choice choice)
{
    if (test == nullptr || test->isFinished())
        return;
    test->play (choice);
    refresh();
}

void AbxPanel::answerX (bool xIsA)
{
    if (test == nullptr || test->isFinished())
        return;
    test->answer (xIsA);
    refresh();
}

void AbxPanel::close()
{
    test.reset();
    if (onClose != nullptr)
        onClose();
}

bool AbxPanel::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        close();
        return true;
    }
    auto c = key.getTextCharacter();
    if (c == 0)
        c = static_cast<juce::juce_wchar> (key.getKeyCode());
    switch (juce::CharacterFunctions::toLowerCase (c))
    {
        case 'a': playChoice (AbxTest::Choice::A); return true;
        case 'b': playChoice (AbxTest::Choice::B); return true;
        case 'x': playChoice (AbxTest::Choice::X); return true;
        case '1': answerX (true); return true;
        case '2': answerX (false); return true;
        default: return false;
    }
}

juce::Rectangle<int> AbxPanel::cardBounds (juce::Rectangle<int> area)
{
    const int w = juce::jmin (560, area.getWidth() - 32), h = juce::jmin (360, area.getHeight() - 32);
    return area.withSizeKeepingCentre (juce::jmax (0, w), juce::jmax (0, h));
}

void AbxPanel::parentSizeChanged()
{
    if (auto* parent = getParentComponent())
        setBounds (parent->getLocalBounds());
}

void AbxPanel::mouseDown (const juce::MouseEvent&)
{
    // Clicks on the backdrop do nothing: a test is not ended by accident.
}

void AbxPanel::paint (juce::Graphics& g)
{
    // Opaque: the header's A/B buttons and trim line must not show through.
    g.fillAll (Palette::background);
    Theme::drawPanel (g, card.toFloat());

    auto r = card.reduced (20, 16);
    g.setColour (Palette::text);
    g.setFont (Theme::font (17.0f, true));
    g.drawText ("Blind test (A/B/X) - " + controller.getStripName (strip) + " strip", r.removeFromTop (24), juce::Justification::centredLeft, true);

    g.setColour (Palette::muted);
    g.setFont (Theme::font (12.5f));
    juce::String text = "X is A or B, picked at random for each trial. Listen to A, B and X as often as you like, then say which one X is.";
    text << (matched ? " A and B are loudness matched, so a louder setting cannot win."
                     : " Loudness matching is off (right-click A / B): the louder side may give X away.");
    g.drawFittedText (text, textArea, juce::Justification::topLeft, 3, 1.0f);

    // Progress: one dot per trial (answered: filled; current: ring).
    if (test != nullptr)
    {
        const auto accent = Theme::accent (*this);
        auto p = progressArea.toFloat();
        Theme::drawCaption (g, "TRIAL " + juce::String (juce::jmin (test->getTrial() + 1, test->getTrials())) + " / " + juce::String (test->getTrials()),
                            p.removeFromLeft (110.0f));
        const float d = 9.0f;
        for (int i = 0; i < test->getTrials(); ++i)
        {
            const auto dot = juce::Rectangle<float> (p.getX() + static_cast<float> (i) * (d + 6.0f), p.getCentreY() - d * 0.5f, d, d);
            if (i < test->getTrial())
            {
                g.setColour (accent.withAlpha (0.8f));
                g.fillEllipse (dot);
            }
            else
            {
                g.setColour (i == test->getTrial() ? accent : Palette::border);
                g.drawEllipse (dot.reduced (0.5f), 1.2f);
            }
        }
    }

    g.setColour (test != nullptr && test->isFinished() ? Palette::text : Palette::muted);
    g.setFont (Theme::font (13.0f, test != nullptr && test->isFinished()));
    g.drawFittedText (getStatusText(), resultArea, juce::Justification::centredLeft, 2, 1.0f);
}

void AbxPanel::resized()
{
    card = cardBounds (getLocalBounds());
    auto r = card.reduced (20, 16);
    r.removeFromTop (24 + 6);
    textArea = r.removeFromTop (52);
    r.removeFromTop (8);

    auto plays = r.removeFromTop (44);
    const int playW = juce::jmin (120, (plays.getWidth() - 24) / 3);
    auto row = plays.withSizeKeepingCentre (playW * 3 + 24, 44);
    playA.setBounds (row.removeFromLeft (playW));
    row.removeFromLeft (12);
    playB.setBounds (row.removeFromLeft (playW));
    row.removeFromLeft (12);
    playX.setBounds (row.removeFromLeft (playW));
    r.removeFromTop (10);

    auto answers = r.removeFromTop (36);
    const int answerW = juce::jmin (150, (answers.getWidth() - 16) / 2);
    auto a = answers.withSizeKeepingCentre (answerW * 2 + 16, 36);
    answerA.setBounds (a.removeFromLeft (answerW));
    a.removeFromLeft (16);
    answerB.setBounds (a.removeFromLeft (answerW));
    r.removeFromTop (12);

    progressArea = r.removeFromTop (20);
    auto bottom = r.removeFromBottom (32);
    closeButton.setBounds (bottom.removeFromRight (96));
    bottom.removeFromRight (10);
    againButton.setBounds (bottom.removeFromRight (120));
    r.removeFromBottom (6);
    resultArea = r;
}
} // namespace flub::app::ui
