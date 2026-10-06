#include "VisualiserWindow.h"

#include "SpectrumMirror.h"
#include "VisualiserRegistry.h"

#include "../Theme.h"
#include "../Widgets.h"

namespace flub::app::ui::vis
{
namespace
{
constexpr int kHeaderHeight = 40;
constexpr int kMenuFullScreen = 1, kMenuView = 100;
} // namespace

// =============================================================================
// State
// =============================================================================
juce::String VisualiserWindow::State::toString() const
{
    return view + "," + juce::String (bounds.getX()) + "," + juce::String (bounds.getY()) + "," + juce::String (bounds.getWidth()) + ","
           + juce::String (bounds.getHeight()) + "," + juce::String (maximised ? 1 : 0) + "," + juce::String (fullScreen ? 1 : 0);
}

bool VisualiserWindow::State::fromString (const juce::String& text, State& state)
{
    const auto tokens = juce::StringArray::fromTokens (text, ",", {});
    if (tokens.size() != 7)
        return false;
    for (int i = 1; i < 7; ++i)
        if (! tokens[i].trim().containsOnly ("-0123456789") || tokens[i].trim().isEmpty())
            return false;
    State s;
    s.view = findChoice (tokens[0].trim()) != nullptr ? tokens[0].trim() : juce::String (kDefaultView);
    const int w = tokens[3].getIntValue(), h = tokens[4].getIntValue();
    if (w >= kMinWidth && h >= kMinHeight)
        s.bounds = { tokens[1].getIntValue(), tokens[2].getIntValue(), w, h };
    s.maximised = tokens[5].getIntValue() == 1;
    s.fullScreen = tokens[6].getIntValue() == 1;
    state = s;
    return true;
}

// =============================================================================
// Views
// =============================================================================
const std::vector<VisualiserWindow::Choice>& VisualiserWindow::choices()
{
    static const std::vector<Choice> list = []
    {
        std::vector<Choice> c;
        c.push_back ({ "spectrum", "Spectrum", "SPECTRUM" });
        c.push_back ({ "spectrogram", "Spectrogram", "SPECTROGRAM" });
        for (const auto& d : registry())
            if (d.canBeMain)
                c.push_back ({ d.id, d.menuName, d.caption });
        return c;
    }();
    return list;
}

const VisualiserWindow::Choice* VisualiserWindow::findChoice (const juce::String& id)
{
    for (const auto& c : choices())
        if (c.id == id)
            return &c;
    return nullptr;
}

std::unique_ptr<Visualiser> VisualiserWindow::createView (const juce::String& id)
{
    std::unique_ptr<Visualiser> v;
    juce::String title, description;
    if (id == "spectrum" || id == "spectrogram")
    {
        const bool spectrum = id == "spectrum";
        v = std::make_unique<SpectrumMirror> (spectrum ? SpectrumMirror::Mode::Spectrum : SpectrumMirror::Mode::Spectrogram);
        title = spectrum ? "Spectrum" : "Spectrogram";
        description = spectrum ? "The analyser's input (grey) and output (accent) spectrum, with the panel's tilt setting."
                               : "The output spectrum over the last 6 seconds, newest at the top; brighter is louder.";
    }
    else if (const auto* d = findDescriptor (id); d != nullptr && d->canBeMain)
    {
        v = d->create();
        title = d->menuName;
        description = d->description;
    }
    if (v != nullptr)
    {
        v->setTitle (title);
        v->setDescription (description);
    }
    return v;
}

// =============================================================================
// Content: the header (caption, view picker, full screen) over the view
// =============================================================================
class VisualiserWindow::Content final : public juce::Component, private juce::Timer
{
public:
    explicit Content (VisualiserWindow& w) : window (w)
    {
        setOpaque (true);
        setWantsKeyboardFocus (true);
        for (size_t i = 0; i < choices().size(); ++i)
            picker.addItem (choices()[i].name, static_cast<int> (i) + 1);
        Style::describe (picker, "Visualiser", "The view this window shows (Left / Right arrows step through them)");
        picker.onChange = [this]
        {
            const int i = picker.getSelectedId() - 1;
            if (i >= 0 && i < static_cast<int> (choices().size()))
                window.setView (choices()[static_cast<size_t> (i)].id);
        };
        addAndMakeVisible (picker);
        Style::set (fullButton, "chip");
        Style::describe (fullButton, "Full screen", "Fill the screen the window is on (F11 or a double-click; Esc leaves)");
        fullButton.onClick = [this] { window.setFullScreenMode (! window.isFullScreenMode()); };
        addAndMakeVisible (fullButton);
        startTimer (200);
    }

    void setView (Visualiser* v)
    {
        view = v;
        if (view != nullptr)
        {
            addAndMakeVisible (*view, 0);
            const auto* c = findChoice (window.getViewId());
            picker.setSelectedId (c != nullptr ? static_cast<int> (c - choices().data()) + 1 : 0, juce::dontSendNotification);
        }
        resized();
        repaint();
    }

    void layoutChanged()
    {
        lastMouseMove = juce::Time::getMillisecondCounter();
        fullButton.setButtonText (window.isFullScreenMode() ? "Exit full screen" : "Full screen");
        updateChrome();
        resized();
        repaint();
    }

    bool headerShown() const noexcept { return ! window.isFullScreenMode() || chromeVisible; }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Palette::background);
        if (window.isFullScreenMode())
            return; // the header floats over the view (paintOverChildren)
        const auto h = header.toFloat();
        const auto* c = findChoice (window.getViewId());
        Theme::drawCaption (g, c != nullptr ? c->caption : juce::String(), h, Palette::muted);
        g.setFont (Theme::font (11.0f));
        g.setColour (Palette::faint);
        g.drawText ("F11 or double-click: full screen", h.withTrimmedLeft (220.0f).withRight (static_cast<float> (picker.getX()) - 12.0f),
                    juce::Justification::centredRight, true);
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        // In full screen the header floats over the view while it is shown.
        if (window.isFullScreenMode() && chromeVisible)
        {
            // Behind the caption and hint only: the picker and the button paint themselves (under this layer).
            const auto h = header.toFloat().withRight (static_cast<float> (picker.getX()) - 6.0f);
            g.setColour (Palette::background.withAlpha (0.82f));
            g.fillRoundedRectangle (h.expanded (8.0f, 4.0f), 8.0f);
            const auto* c = findChoice (window.getViewId());
            Theme::drawCaption (g, c != nullptr ? c->caption : juce::String(), h, Palette::muted);
            g.setFont (Theme::font (11.0f));
            g.setColour (Palette::faint);
            g.drawText ("Esc or F11 leaves full screen", h.withTrimmedRight (6.0f), juce::Justification::centredRight, true);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        const bool full = window.isFullScreenMode();
        header = r.reduced (14, 0).withHeight (kHeaderHeight).withTrimmedTop (full ? 10 : 6);
        auto h = header;
        fullButton.setBounds (h.removeFromRight (full ? 118 : 96).reduced (0, 5));
        h.removeFromRight (8);
        picker.setBounds (h.removeFromRight (juce::jmin (260, juce::jmax (140, h.getWidth() / 2))).reduced (0, 4));
        if (view != nullptr)
            view->setBounds (full ? r.reduced (10) : r.withTrimmedTop (kHeaderHeight + 4).reduced (12, 0).withTrimmedBottom (12));
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::F11Key)
        {
            window.setFullScreenMode (! window.isFullScreenMode());
            return true;
        }
        if (key == juce::KeyPress::escapeKey && window.isFullScreenMode())
        {
            window.setFullScreenMode (false);
            return true;
        }
        if (key == juce::KeyPress::rightKey || key == juce::KeyPress::leftKey)
        {
            window.stepView (key == juce::KeyPress::rightKey ? 1 : -1);
            return true;
        }
        return false;
    }

    void mouseMove (const juce::MouseEvent&) override { wake(); }
    void mouseDrag (const juce::MouseEvent&) override { wake(); }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (! header.contains (e.getPosition()))
            window.setFullScreenMode (! window.isFullScreenMode());
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        wake();
        if (! e.mods.isPopupMenu())
            return;
        juce::PopupMenu menu;
        menu.addItem (kMenuFullScreen, window.isFullScreenMode() ? "Exit full screen (Esc)" : "Full screen (F11)");
        menu.addSeparator();
        for (size_t i = 0; i < choices().size(); ++i)
            menu.addItem (kMenuView + static_cast<int> (i), choices()[i].name, true, choices()[i].id == window.getViewId());
        juce::Component::SafePointer<Content> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea ({ e.getScreenX(), e.getScreenY(), 1, 1 }),
                            [safe] (int result)
                            {
                                if (safe == nullptr || result == 0)
                                    return;
                                if (result == kMenuFullScreen)
                                    safe->window.setFullScreenMode (! safe->window.isFullScreenMode());
                                else if (result >= kMenuView && result < kMenuView + static_cast<int> (choices().size()))
                                    safe->window.setView (choices()[static_cast<size_t> (result - kMenuView)].id);
                            });
    }

private:
    void wake()
    {
        lastMouseMove = juce::Time::getMillisecondCounter();
        updateChrome();
    }

    void timerCallback() override { updateChrome(); }

    void updateChrome()
    {
        const bool full = window.isFullScreenMode();
        const bool recent = juce::Time::getMillisecondCounter() - lastMouseMove < static_cast<juce::uint32> (kIdleHideMs);
        const bool show = ! full || recent;
        if (show != chromeVisible || picker.isVisible() != show)
        {
            chromeVisible = show;
            picker.setVisible (show);
            fullButton.setVisible (show);
            setMouseCursor (show ? juce::MouseCursor::NormalCursor : juce::MouseCursor::NoCursor);
            repaint();
        }
    }

    VisualiserWindow& window;
    Visualiser* view = nullptr;
    juce::ComboBox picker;
    juce::TextButton fullButton { "Full screen" };
    juce::Rectangle<int> header;
    juce::uint32 lastMouseMove = 0;
    bool chromeVisible = true;
};

// =============================================================================
// Window
// =============================================================================
VisualiserWindow::VisualiserWindow (const juce::String& id, bool onDesktop)
    : juce::DocumentWindow ("Flubsound Pro - Visualiser", Palette::background, juce::DocumentWindow::allButtons, onDesktop),
      desktop (onDesktop)
{
    setTitle ("Flubsound Pro visualiser");
    setUsingNativeTitleBar (true);
    content = new Content (*this);
    setContentOwned (content, false);
    setResizable (true, false);
    setResizeLimits (kMinWidth, kMinHeight, 16384, 16384);
    if (desktop)
        centreWithSize (kDefaultWidth, kDefaultHeight);
    else
        setSize (kDefaultWidth, kDefaultHeight);
    normalBounds = getBounds();
    setView (id);
}

VisualiserWindow::~VisualiserWindow()
{
    if (desktop && fullScreenMode)
        setFullScreenMode (false);
    if (view != nullptr && content != nullptr)
        content->removeChildComponent (view.get());
}

void VisualiserWindow::setView (const juce::String& id)
{
    const juce::String wanted = findChoice (id) != nullptr ? id : juce::String (kDefaultView);
    if (wanted == viewId && view != nullptr)
        return;
    auto next = createView (wanted);
    if (next == nullptr)
        return;
    auto old = std::move (view);
    view = std::move (next);
    viewId = wanted;
    if (old != nullptr)
        content->removeChildComponent (old.get());
    content->setView (view.get());
    view->lookAndFeelChanged();
    if (onViewChanged != nullptr)
        onViewChanged (view.get());
    old.reset(); // after the host stopped feeding it
    setName ("Flubsound Pro - " + findChoice (viewId)->name);
    notifyState();
}

void VisualiserWindow::stepView (int direction)
{
    const auto& list = choices();
    const auto* c = findChoice (viewId);
    const int n = static_cast<int> (list.size());
    const int at = c != nullptr ? static_cast<int> (c - list.data()) : 0;
    setView (list[static_cast<size_t> (((at + direction) % n + n) % n)].id);
}

bool VisualiserWindow::isHeaderShown() const noexcept
{
    return content != nullptr && content->headerShown();
}

void VisualiserWindow::setFullScreenMode (bool shouldBeFullScreen)
{
    if (shouldBeFullScreen == fullScreenMode)
        return;
    if (shouldBeFullScreen)
    {
        wasMaximised = isFullScreen(); // native maximise
        juce::Rectangle<int> area = getBounds();
        if (desktop)
            if (const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds()))
                area = display->logicalBounds.getSmallestIntegerContainer();
        fullScreenMode = true;
        // Borderless: no title bar, no frame, no resizing; then cover the display.
        setUsingNativeTitleBar (false);
        setTitleBarHeight (0);
        setResizable (false, false);
        setBounds (area);
        if (desktop && isShowing())
            toFront (true);
    }
    else
    {
        const auto restore = normalBounds; // the frame changes below must not overwrite it
        fullScreenMode = false;
        setTitleBarHeight (26);
        setResizable (true, false);
        setUsingNativeTitleBar (true);
        setBounds (restore);
        normalBounds = restore;
        if (wasMaximised)
            setFullScreen (true);
    }
    content->layoutChanged();
    if (desktop && isShowing())
        content->grabKeyboardFocus();
    notifyState();
}

juce::BorderSize<int> VisualiserWindow::getBorderThickness() const
{
    return fullScreenMode ? juce::BorderSize<int>() : juce::DocumentWindow::getBorderThickness();
}

void VisualiserWindow::rememberBounds()
{
    // Only the normal window: maximised, minimised and full screen keep the last normal bounds.
    if (! fullScreenMode && ! isFullScreen() && ! isMinimised() && getWidth() >= kMinWidth && getHeight() >= kMinHeight)
        normalBounds = getBounds();
}

void VisualiserWindow::moved()
{
    juce::DocumentWindow::moved();
    rememberBounds();
}

void VisualiserWindow::resized()
{
    juce::DocumentWindow::resized();
    rememberBounds();
}

void VisualiserWindow::lookAndFeelChanged()
{
    juce::DocumentWindow::lookAndFeelChanged();
    setBackgroundColour (Palette::background);
}

VisualiserWindow::State VisualiserWindow::getState() const
{
    State s;
    s.view = viewId;
    s.bounds = normalBounds;
    s.maximised = fullScreenMode ? wasMaximised : isFullScreen();
    s.fullScreen = fullScreenMode;
    return s;
}

void VisualiserWindow::applyState (const State& state)
{
    setFullScreenMode (false);
    setView (state.view);
    auto b = state.bounds;
    if (desktop && ! b.isEmpty())
    {
        // Keep it on a connected display: at least 120 x 80 of it visible, else centred on the main one.
        const auto& displays = juce::Desktop::getInstance().getDisplays();
        bool visible = false;
        for (const auto& d : displays.displays)
        {
            const auto shown = d.userBounds.getSmallestIntegerContainer().getIntersection (b);
            visible = visible || (shown.getWidth() >= 120 && shown.getHeight() >= 80);
        }
        if (! visible)
            if (const auto* main = displays.getPrimaryDisplay())
            {
                const auto user = main->userBounds.getSmallestIntegerContainer();
                b = b.withSize (juce::jmin (b.getWidth(), user.getWidth()), juce::jmin (b.getHeight(), user.getHeight())).withCentre (user.getCentre());
            }
    }
    if (! b.isEmpty())
    {
        setBounds (b);
        normalBounds = b;
    }
    if (state.maximised)
        setFullScreen (true);
    if (state.fullScreen)
        setFullScreenMode (true);
    notifyState();
}

void VisualiserWindow::notifyState()
{
    if (onStateChanged != nullptr)
        onStateChanged();
}

void VisualiserWindow::closeButtonPressed()
{
    if (onCloseRequested != nullptr)
        onCloseRequested();
}
} // namespace flub::app::ui::vis
