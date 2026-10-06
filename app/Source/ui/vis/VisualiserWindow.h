// Flubsound Pro - the visualiser window ("Pop out", docs/06 §6.4.2).
//
// One resizable top-level window that shows any main view on its own: the
// spectrum or spectrogram (read-only mirrors of the analyser, SpectrumMirror)
// or any registered visualiser that can replace the spectrum (the 3D
// waterfall, radial spectrum, goniometer, ...). It can sit on a second
// monitor, be maximised, or go borderless full screen on the display it is
// on (F11, a double-click, the Full screen button or the right-click menu;
// Esc or F11 leaves). In full screen the header and the mouse pointer hide
// after kIdleHideMs without mouse movement and come back when it moves.
// Left / Right arrows step through the views.
//
// The window owns its own view instance (a view can show in the panel and
// here at once), fed by the panel's VisualiserHost as its "extra" view: the
// same pushes and the same FrameContext, so there is no second analysis.
// Closing it does not touch the panel's view. MainComponent owns it,
// persists its State (the view, the normal bounds and monitor, maximised and
// full screen) in the `ui.visualiserWindow` preference, and closes it while
// Tournament mode is on (docs/11 E55: no extra windows over a game).
//
// Message thread only. onDesktop = false (tests) keeps the window off the
// desktop: full screen then only changes the flags and the content's layout.
#pragma once

#include "Visualiser.h"

#include <functional>
#include <memory>
#include <vector>

namespace flub::app::ui::vis
{
class VisualiserWindow final : public juce::DocumentWindow
{
public:
    static constexpr int kMinWidth = 360, kMinHeight = 240;
    static constexpr int kDefaultWidth = 1100, kDefaultHeight = 680;
    static constexpr int kIdleHideMs = 2500;
    static constexpr const char* kDefaultView = "spectrum";

    /** What is persisted: "view,x,y,w,h,maximised,fullscreen". */
    struct State
    {
        juce::String view { kDefaultView };
        juce::Rectangle<int> bounds; // the normal (not maximised, not full screen) bounds; empty = default
        bool maximised = false, fullScreen = false;

        juce::String toString() const;
        /** False (state unchanged) if malformed; an unknown view reads as the default. */
        static bool fromString (const juce::String& text, State& state);
    };

    /** A view the window can show, in menu order. */
    struct Choice
    {
        juce::String id, name, caption;
    };
    /** "spectrum", "spectrogram", then every registry entry that can be a main view. */
    static const std::vector<Choice>& choices();
    static const Choice* findChoice (const juce::String& id);
    static std::unique_ptr<Visualiser> createView (const juce::String& id);

    explicit VisualiserWindow (const juce::String& viewId, bool onDesktop = true);
    ~VisualiserWindow() override;

    /** Shows another view (an unknown id shows the default). onViewChanged
        runs with the new view before the old one is deleted. */
    void setView (const juce::String& id);
    const juce::String& getViewId() const noexcept { return viewId; }
    Visualiser* getView() const noexcept { return view.get(); }
    /** Steps to the next (+1) or previous (-1) view in choices(). */
    void stepView (int direction);

    /** Borderless full screen on the display the window is on. */
    void setFullScreenMode (bool shouldBeFullScreen);
    bool isFullScreenMode() const noexcept { return fullScreenMode; }
    /** True while the header is shown (always outside full screen). */
    bool isHeaderShown() const noexcept;

    State getState() const;
    /** Bounds (kept on a connected display), maximised, full screen and view. */
    void applyState (const State& state);

    /** The new view (fed from now on); called before the old view is deleted. */
    std::function<void (Visualiser*)> onViewChanged;
    /** The close button: the owner deletes the window (asynchronously). */
    std::function<void()> onCloseRequested;
    /** View, full screen or maximised changed: persist getState(). */
    std::function<void()> onStateChanged;

    void closeButtonPressed() override;
    juce::BorderSize<int> getBorderThickness() const override;
    void moved() override;
    void resized() override;
    void lookAndFeelChanged() override;

private:
    class Content;
    void rememberBounds();
    void notifyState();

    Content* content = nullptr; // owned through setContentOwned
    std::unique_ptr<Visualiser> view;
    juce::String viewId;
    juce::Rectangle<int> normalBounds;
    bool desktop = true, fullScreenMode = false, wasMaximised = false;
};
} // namespace flub::app::ui::vis
