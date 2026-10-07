#include "NoticeBanners.h"

#include "FlubLookAndFeel.h"
#include "Theme.h"

#include <algorithm>

namespace flub::app::ui
{
using namespace flub::param;

namespace
{
/** Warning triangle with an exclamation mark. */
void drawWarning (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour colour)
{
    const auto s = juce::jmin (r.getWidth(), r.getHeight());
    const auto b = r.withSizeKeepingCentre (s, s);
    juce::Path triangle;
    triangle.addTriangle (b.getCentreX(), b.getY() + s * 0.08f, b.getRight() - s * 0.04f, b.getBottom() - s * 0.1f, b.getX() + s * 0.04f,
                          b.getBottom() - s * 0.1f);
    g.setColour (colour);
    g.strokePath (triangle, juce::PathStrokeType (s * 0.09f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.fillRoundedRectangle (juce::Rectangle<float> (s * 0.1f, s * 0.3f).withCentre ({ b.getCentreX(), b.getY() + s * 0.5f }), s * 0.05f);
    g.fillEllipse (juce::Rectangle<float> (s * 0.12f, s * 0.12f).withCentre ({ b.getCentreX(), b.getY() + s * 0.74f }));
}

/** The slim banner body every notice here shares (DeviceAdviceBanner's look). */
void drawBannerBody (juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour accent)
{
    g.setColour (Palette::panelRaised);
    g.fillRoundedRectangle (bounds, Theme::kControlRadius);
    g.setColour (accent.withAlpha (0.45f));
    g.drawRoundedRectangle (bounds, Theme::kControlRadius, 1.0f);
    g.setColour (accent);
    g.fillRoundedRectangle (bounds.withWidth (4.0f), 2.0f);
}

/** Headline in bold, then the message in muted text, cut at `right`. */
void drawBannerText (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& headline, const juce::String& message)
{
    const auto headFont = Theme::font (13.0f, true);
    const int headW = juce::jmin (r.getWidth(), juce::roundToInt (juce::GlyphArrangement::getStringWidth (headFont, headline)) + 2);
    g.setFont (headFont);
    g.setColour (Palette::text);
    g.drawText (headline, r.removeFromLeft (headW), juce::Justification::centredLeft, true);
    if (message.isNotEmpty() && r.getWidth() > 60)
    {
        r.removeFromLeft (12);
        g.setFont (Theme::font (13.0f));
        g.setColour (Palette::muted);
        g.drawText (message, r, juce::Justification::centredLeft, true);
    }
}

int buttonWidth (const juce::TextButton& b)
{
    return juce::jlimit (60, 220, juce::roundToInt (juce::GlyphArrangement::getStringWidth (Theme::font (13.0f), b.getButtonText())) + 26);
}
} // namespace

// =============================================================================
// DeviceErrorBanner
// =============================================================================
DeviceErrorBanner::DeviceErrorBanner (EngineController& c) : controller (c)
{
    setTitle ("Audio device problem");

    retryButton.setTooltip ("Open the audio device again (or check the input / output pair again)");
    retryButton.onClick = [this] { retry(); };
    chooseButton.setTooltip ("Choose another output device (Settings > Audio)");
    chooseButton.onClick = [this]
    {
        if (onChooseOutput)
            onChooseOutput();
    };
    soundButton.setTooltip ("Open the system's sound settings");
    soundButton.onClick = [this]
    {
        if (onOpenSoundSettings)
            onOpenSoundSettings();
    };
    for (auto* b : { &retryButton, &chooseButton, &soundButton })
        addAndMakeVisible (b);
    setVisible (false);
}

juce::String DeviceErrorBanner::headlineFor (const DeviceSafetyState& state)
{
    switch (state.kind)
    {
        case DeviceSafetyState::Kind::LoopbackPair: return "Output muted: feedback loop";
        case DeviceSafetyState::Kind::DeviceError: return state.outputMuted ? "Output muted: audio device error" : "Audio device error";
        case DeviceSafetyState::Kind::None: break;
    }
    if (state.outputFallback)
        return state.safeSpeakerProfile ? "Output fallback: safe speaker profile" : "Output fallback";
    return {};
}

juce::String DeviceErrorBanner::messageFor (const DeviceSafetyState& state)
{
    if (state.kind == DeviceSafetyState::Kind::None)
        return state.outputFallback ? state.fallbackMessage : juce::String();
    // The host's loopback text starts with what the headline already says.
    if (const juce::String prefix ("Output muted: "); state.kind == DeviceSafetyState::Kind::LoopbackPair && state.message.startsWith (prefix))
        return state.message.substring (prefix.length());
    if (state.message.isNotEmpty())
        return state.message;
    return state.kind == DeviceSafetyState::Kind::LoopbackPair ? "The output device feeds the input back into Flubsound. Choose another output device."
                                                                : "The audio device reported an error. Retry, or choose another output device.";
}

bool DeviceErrorBanner::refresh()
{
    const auto state = controller.getDeviceSafetyState();
    const bool want = state.kind != DeviceSafetyState::Kind::None || state.outputFallback;
    warnOnly = state.kind == DeviceSafetyState::Kind::None; // a fallback that plays: amber, not the hot error colour
    headline = headlineFor (state);
    message = messageFor (state);
    if (! want)
        retryError = {};
    if (retryError.isNotEmpty() && retryError != message)
        message << " (Retry: " << retryError << ")";

    setTooltip (message);
    setDescription (headline + ". " + message);

    const bool changed = want != showing;
    showing = want;
    setVisible (want);
    resized();
    repaint();
    return changed;
}

void DeviceErrorBanner::retry()
{
    // A successful re-open clears the state and broadcasts Change::Device,
    // which refreshes (and hides) the banner; a failure keeps it up.
    retryError = controller.retryDevice();
    if (refresh() && getParentComponent() != nullptr)
        getParentComponent()->resized();
}

void DeviceErrorBanner::resized()
{
    auto r = getLocalBounds().reduced (6, 4);
    for (auto* b : { &soundButton, &chooseButton, &retryButton })
    {
        b->setBounds (r.removeFromRight (buttonWidth (*b)));
        r.removeFromRight (6);
    }
}

void DeviceErrorBanner::paint (juce::Graphics& g)
{
    const auto colours = Theme::statusColours (*this);
    const auto accent = warnOnly ? colours.warn : colours.hot;
    drawBannerBody (g, getLocalBounds().toFloat().reduced (0.5f), accent);

    auto r = getLocalBounds().reduced (10, 0);
    drawWarning (g, r.removeFromLeft (22).toFloat().reduced (0.0f, 8.0f), accent);
    r.removeFromLeft (8);
    r.setRight (retryButton.getX() - 10);
    drawBannerText (g, r, headline, message);
}

// =============================================================================
// NoticeBar
// =============================================================================
NoticeBar::NoticeBar()
{
    setTitle ("Notices");
    actionButton.onClick = [this]
    {
        if (entries.empty())
            return;
        // The action may post or dismiss notices itself: run a copy.
        const auto action = entries.front().notice.action;
        const auto key = entries.front().notice.key;
        dismiss (key);
        if (action)
            action();
    };
    dismissButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xc3\x97"))); // multiplication sign
    dismissButton.setTitle ("Dismiss notice");
    dismissButton.setTooltip ("Dismiss this notice");
    dismissButton.onClick = [this]
    {
        if (! entries.empty())
            dismiss (entries.front().notice.key);
    };
    addChildComponent (actionButton);
    addAndMakeVisible (dismissButton);
    setVisible (false);
}

NoticeBar::~NoticeBar()
{
    stopTimer();
}

void NoticeBar::post (Notice notice)
{
    const bool wasShowing = shouldShow();
    entries.erase (std::remove_if (entries.begin(), entries.end(), [&] (const Entry& e) { return e.notice.key == notice.key; }), entries.end());
    const double expires = notice.seconds > 0.0 ? juce::Time::getMillisecondCounterHiRes() + notice.seconds * 1000.0 : 0.0;
    entries.push_front ({ std::move (notice), expires });
    while (entries.size() > EngineController::kMaxPendingNotices)
        entries.pop_back();
    update (wasShowing);
}

void NoticeBar::dismiss (const juce::String& key)
{
    const bool wasShowing = shouldShow();
    const auto before = entries.size();
    entries.erase (std::remove_if (entries.begin(), entries.end(), [&] (const Entry& e) { return e.notice.key == key; }), entries.end());
    if (entries.size() != before)
        update (wasShowing);
}

void NoticeBar::clear()
{
    const bool wasShowing = shouldShow();
    entries.clear();
    update (wasShowing);
}

bool NoticeBar::hasNotice (const juce::String& key) const
{
    return std::any_of (entries.begin(), entries.end(), [&] (const Entry& e) { return e.notice.key == key; });
}

void NoticeBar::expire (double nowMs)
{
    const bool wasShowing = shouldShow();
    const auto before = entries.size();
    entries.erase (std::remove_if (entries.begin(), entries.end(), [nowMs] (const Entry& e) { return e.expiresMs > 0.0 && nowMs >= e.expiresMs; }),
                   entries.end());
    if (entries.size() != before)
        update (wasShowing);
}

void NoticeBar::timerCallback()
{
    expire (juce::Time::getMillisecondCounterHiRes());
}

void NoticeBar::update (bool wasShowing)
{
    const auto* notice = current();
    actionButton.setVisible (notice != nullptr && notice->actionLabel.isNotEmpty() && notice->action != nullptr);
    if (notice != nullptr)
    {
        actionButton.setButtonText (notice->actionLabel);
        setTooltip (notice->detail.isNotEmpty() ? notice->detail : notice->text);
        setDescription (notice->text + (notice->detail.isNotEmpty() ? ". " + notice->detail : juce::String()));
    }

    // Only timed notices need the clock.
    const bool timed = std::any_of (entries.begin(), entries.end(), [] (const Entry& e) { return e.expiresMs > 0.0; });
    if (timed && ! isTimerRunning())
        startTimerHz (4);
    else if (! timed)
        stopTimer();

    setVisible (shouldShow());
    resized();
    repaint();
    if (wasShowing != shouldShow() && onVisibilityChanged)
        onVisibilityChanged();
}

void NoticeBar::resized()
{
    auto r = getLocalBounds().reduced (6, 4);
    dismissButton.setBounds (r.removeFromRight (r.getHeight()));
    if (actionButton.isVisible())
    {
        r.removeFromRight (6);
        actionButton.setBounds (r.removeFromRight (buttonWidth (actionButton)));
    }
}

void NoticeBar::paint (juce::Graphics& g)
{
    const auto* notice = current();
    if (notice == nullptr)
        return;

    const auto accent = notice->kind == Notice::Kind::Warning ? Palette::amber : Theme::accent (*this);
    drawBannerBody (g, getLocalBounds().toFloat().reduced (0.5f), accent);

    auto r = getLocalBounds().reduced (10, 0);
    if (notice->kind == Notice::Kind::Warning)
        drawWarning (g, r.removeFromLeft (22).toFloat().reduced (0.0f, 8.0f), accent);
    else
    {
        // "i" in a circle.
        auto icon = r.removeFromLeft (22).toFloat().reduced (2.0f, 9.0f);
        icon = icon.withSizeKeepingCentre (icon.getHeight(), icon.getHeight());
        g.setColour (accent);
        g.drawEllipse (icon.reduced (0.75f), 1.5f);
        g.fillRect (juce::Rectangle<float> (1.6f, icon.getHeight() * 0.36f).withCentre ({ icon.getCentreX(), icon.getCentreY() + icon.getHeight() * 0.1f }));
        g.fillEllipse (juce::Rectangle<float> (2.2f, 2.2f).withCentre ({ icon.getCentreX(), icon.getY() + icon.getHeight() * 0.27f }));
    }
    r.removeFromLeft (8);
    r.setRight ((actionButton.isVisible() ? actionButton.getX() : dismissButton.getX()) - 10);

    // "(2 more)" when older notices wait behind this one.
    juce::String more;
    if (entries.size() > 1)
        more << "+" << static_cast<int> (entries.size() - 1) << " more";
    if (more.isNotEmpty())
    {
        g.setFont (Theme::font (11.5f));
        g.setColour (Palette::faint);
        const int w = juce::roundToInt (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), more)) + 4;
        g.drawText (more, r.removeFromRight (w), juce::Justification::centredRight, false);
        r.removeFromRight (8);
    }
    g.setFont (Theme::font (13.0f));
    g.setColour (Palette::text);
    g.drawText (notice->text, r, juce::Justification::centredLeft, true);
}

// ---- The app's notices ---------------------------------------------------------------------
juce::String NoticeBar::profileName (LatencyProfileValue profile)
{
    const auto& choices = layout()[static_cast<size_t> (LatencyProfile)].choices;
    const auto i = static_cast<size_t> (profile);
    return i < choices.size() ? juce::String (choices[i]) : juce::String (static_cast<int> (profile));
}

NoticeBar::Notice NoticeBar::presetWarningsNotice (const EngineController::PresetWarnings& w)
{
    Notice n;
    n.key = kPresetWarningsKey;
    n.kind = Notice::Kind::Warning;
    n.seconds = kPresetWarningSeconds;
    n.text << "Preset \"" << w.presetName << "\": " << (w.warnings.isEmpty() ? juce::String ("loaded with warnings") : w.warnings[0]);
    if (w.warnings.size() > 1)
        n.text << " (+" << (w.warnings.size() - 1) << (w.warnings.size() == 2 ? " other warning)" : " other warnings)");
    n.detail << "The preset file has settings Flubsound adjusted or ignored while loading it:";
    for (const auto& line : w.warnings)
        n.detail << "\n- " << line;
    n.detail << "\nSave the preset again to write a clean file.";
    return n;
}

NoticeBar::Notice NoticeBar::recoveryNotice (const AppSettings::Recovery& recovery)
{
    Notice n;
    if (! recovery.wasDamaged())
        return n;
    n.key = kRecoveryKey;
    n.kind = Notice::Kind::Warning;
    n.text = recovery.restoredFromBackup > 0
                 ? "The settings file was damaged: restored from the backup of an earlier start (" + juce::String (recovery.restoredFromBackup) + ")."
                 : juce::String ("The settings file was damaged and no backup was usable: Flubsound started with default settings.");
    n.detail << "The damaged file was kept as " << recovery.quarantined.getFullPathName() << ".";
    if (recovery.restoredFromBackup > 0)
        n.detail << " Changes made after that backup (routing, rules, hotkeys) may be missing.";
    return n;
}

NoticeBar::Notice NoticeBar::latencyNotice (const EngineController::LatencySuggestion& s, std::function<void()> accept)
{
    Notice n;
    n.key = kLatencyKey;
    n.kind = Notice::Kind::Prompt;
    n.text << "\"" << s.presetName << "\" was made for " << profileName (s.suggested) << "; the engine runs " << profileName (s.current) << ".";
    n.detail = "The latency profile is one setting for every strip, so loading a preset never changes it. Switching restarts the engine "
               "briefly; you can change it back in Settings > Processing.";
    n.actionLabel = "Use " + profileName (s.suggested);
    n.action = std::move (accept);
    n.seconds = 30.0;
    return n;
}

NoticeBar::Notice NoticeBar::hotkeyNotice (const std::vector<HotkeyManager::Failure>& failures, std::function<void()> openSettings)
{
    Notice n;
    if (failures.empty())
        return n;
    n.key = kHotkeysKey;
    n.kind = Notice::Kind::Warning;
    n.text << (failures.size() == 1 ? "Hotkey not active: " : "Hotkeys not active: ") << HotkeyManager::describeFailure (failures.front()) << ".";
    if (failures.size() > 1)
        n.text << " (+" << static_cast<int> (failures.size() - 1) << (failures.size() == 2 ? " other)" : " others)");
    n.detail << "These system-wide shortcuts do nothing until they get a combination of their own:";
    for (const auto& failure : failures)
        n.detail << "\n- " << HotkeyManager::describeFailure (failure) << ".";
    n.detail << "\nSettings > Hotkeys can pick a free combination, or record a new one when you press it.";
    n.actionLabel = "Fix in Settings";
    n.action = std::move (openSettings);
    return n;
}
} // namespace flub::app::ui
