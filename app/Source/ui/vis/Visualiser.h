// Flubsound Pro - the analyser panel's visualiser views (docs/06 §6.4.2).
//
// A visualiser is one optional view the analyser panel can show in place of
// the spectrum plot (or beside it), or as a thin strip under it. The
// Spectrum + EQ view stays the default and is not a visualiser: with no
// visualiser chosen the panel draws exactly what it always did.
//
// Contract (message thread only, like every view):
//   * A visualiser is a juce::Component. It paints itself inside its own
//     bounds (transparent outside its plot well: the panel's fill shows
//     through) and repaints itself from advance() when something moved.
//   * Data arrives through the four calls below, in this order every display
//     frame: pushPre / pushPost (zero or more times: whatever the analyser
//     taps delivered since the last frame), then advance (exactly once).
//     pushPre / pushPost only copy samples into the view's own preallocated
//     buffers; analysis belongs in advance (it runs once per frame and only
//     for the views that are fed, see keepsHistory).
//   * No allocation per frame: buffers, images and paths are sized in the
//     constructor, setSampleRate or resized (juce::Path::clear keeps its
//     storage). tests/app/test_app_visualisers.cpp checks pushPost + advance
//     for every registered view.
//   * Colours come from Palette:: / Theme:: (Theme::accent (*this) follows the
//     strip's mode: teal for Music, magenta for Gaming); a mode or theme
//     switch arrives as lookAndFeelChanged().
//   * Give the component a title and description (accessibility) and a
//     tooltip where a hover explains something; VisualiserHost sets the
//     title and description from the registry entry when it creates the view.
//   * Most views ignore the mouse (setInterceptsMouseClicks (false, false)).
//     One that takes clicks (the brain: drag to turn) still gets the
//     visualiser window's menu, double-click and idle handling: the window
//     listens to its mouse events and adds addMenuItems() to its menu.
//
// To add a view: write vis/MyView.{h,cpp} (a class derived from Visualiser),
// then add one line for it to the list in vis/VisualiserRegistry.cpp. The
// View menu, persistence (ui.analyzer), the screenshot driver's
// "vis-<id>" / "vis-strip-<id>" states and the framework tests pick it up
// from there. app/CMakeLists.txt globs vis/*.cpp.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <memory>

namespace flub::app::ui
{
struct MeterSnapshot;
class SpectrumAnalyzer;
} // namespace flub::app::ui

namespace flub::app::ui::vis
{
/** What a visualiser gets once per display frame (valid during advance() only). */
struct FrameContext
{
    explicit FrameContext (const MeterSnapshot& snapshot, double dt = 1.0 / 60.0, double rate = 48000.0,
                           const SpectrumAnalyzer* analyser = nullptr) noexcept
        : meters (snapshot), dtSeconds (dt), sampleRate (rate), spectrum (analyser)
    {
    }

    /** The selected strip's telemetry of this frame (MeterBus via MeterSnapshot:
        levels, loudness, correlation, gain reductions, ...). */
    const MeterSnapshot& meters;
    /** Seconds since the previous frame (0 .. 0.1). */
    double dtSeconds = 1.0 / 60.0;
    double sampleRate = 48000.0;
    /** The spectrum analyser's latest analysis (getBandLevelDb, getDisplayLevelDb,
        getStereoWidth, xForFrequency ...); nullptr in tests that have none. */
    const SpectrumAnalyzer* spectrum = nullptr;
    /** The loudness target in force, NaN when there is none (see AnalyzerPanel::loudnessTarget). */
    float targetLufs = std::nanf ("");
    /** What the target is, e.g. "Auto level target". */
    juce::String targetName;
};

class Visualiser : public juce::Component
{
public:
    ~Visualiser() override = default;

    /** The stream's sample rate changed (also called once before the first push). */
    virtual void setSampleRate (double /*sampleRate*/) {}
    /** Strip switch / engine rebuilt: forget the history. */
    virtual void reset() {}
    /** New samples of the pre-processing tap (mono mid (L + R) / 2). */
    virtual void pushPre (const float* /*mid*/, int /*numSamples*/) {}
    /** New samples of the post-processing tap: mid (L + R) / 2 and side
        (L - R) / 2, aligned (mid[i] and side[i] are the same instant; so
        L = mid + side, R = mid - side). */
    virtual void pushPost (const float* /*mid*/, const float* /*side*/, int /*numSamples*/) {}
    /** Once per display frame, after the pushes: analyse, update, repaint. */
    virtual void advance (const FrameContext& frame) = 0;

    /** True for views with a history worth keeping (loudness, waveform, gain
        reduction): once created they are fed and advanced while hidden too,
        so switching to them shows the last minute, not an empty plot. Views
        that only show "now" return false and cost nothing while hidden. */
    virtual bool keepsHistory() const { return false; }

    /** Height of the view as a strip under the spectrum (Descriptor::canBeStrip). */
    virtual int getStripHeight() const { return 34; }

    /** The view's own right-click items (e.g. the brain's "Stop turning"), added
        with their actions (PopupMenu::addItem (text, enabled, ticked, action)).
        The visualiser window puts them at the top of its menu; a view that takes
        mouse clicks shows them itself in the analyser panel. Default: none. */
    virtual void addMenuItems (juce::PopupMenu& /*menu*/) {}
};

/** One registry entry (vis/VisualiserRegistry.cpp). */
struct Descriptor
{
    /** Persisted in ui.analyzer: lower case letters, digits and '-' only;
        never renamed or reused for something else. */
    const char* id;
    /** The View menu's item, e.g. "Goniometer (vectorscope)". */
    const char* menuName;
    /** The panel caption while it replaces the spectrum, e.g. "GONIOMETER". */
    const char* caption;
    /** Accessibility description and tooltip: what it shows, how to read it. */
    const char* description;
    /** Can replace the spectrum plot (or stand beside it) / can be a strip under it. */
    bool canBeMain, canBeStrip;
    std::unique_ptr<Visualiser> (*create)();
};

/** Helper for the registry: `&vis::make<MyView>`. */
template <typename View>
std::unique_ptr<Visualiser> make()
{
    return std::make_unique<View>();
}
} // namespace flub::app::ui::vis
