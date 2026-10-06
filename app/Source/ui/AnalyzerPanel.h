// Flubsound Pro - the analyser panel: SpectrumAnalyzer with the
// EqCurveEditor stacked on top (identical bounds, shared plot geometry) and a
// header row with the display options (pre / post / tilt / peak hold) and the
// EQ display range.
//
// The optional views (difference, sharper lows, stereo width, piano keys,
// spectrogram, freeze; all off by default) live in the "View" chip's menu.
// Where the header has room beside the legend, Diff and Freeze also get
// chips of their own (Diff first); the legend is never pushed out by them.
//
// Visualisers (docs/06 §6.4.2, vis/Visualiser.h): the View menu's
// "Visualiser" section picks one view from vis::registry() to show in place
// of the spectrum plot (the header's spectrum controls hide with it) or,
// with "Beside the spectrum" on and room for both, to its right; and
// "Strip" picks a thin view (the correlation meter) under whichever is
// shown. "Spectrum + EQ" (the default) and no strip leave the panel exactly
// as it always was. The VisualiserHost owns the views and MainComponent
// feeds it (pushPre / pushPost from the AnalyzerFeed, advanceVisualisers
// once per frame).
#pragma once

#include "EqCurveEditor.h"
#include "MeterSnapshot.h"
#include "SpectrumAnalyzer.h"
#include "Widgets.h"
#include "vis/VisualiserHost.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace flub::app::ui
{
class AnalyzerPanel : public juce::Component
{
public:
    struct Options
    {
        bool showPre = true, showPost = true, tilt = true, peakHold = true;
        float eqRangeDb = 12.0f;
        // Optional views (docs/06 §6.4), all off by default.
        bool difference = false, sharpLows = false, width = false, pianoKeys = false, spectrogram = false;
        // Visualisers (vis::registry() ids): the main view (kSpectrum = none),
        // the strip under it (kNone) and whether the main view sits beside the
        // spectrum instead of replacing it.
        juce::String visualiser { kSpectrum }, strip { kNone };
        bool beside = false;

        static constexpr const char* kSpectrum = "spectrum";
        static constexpr const char* kNone = "none";

        /** The `ui.analyzer` preference:
            "pre,post,tilt,hold,range,diff,lows,width,keys,spectrogram,visualiser,strip,beside". */
        juce::String toString() const;
        /** Reads toString()'s format, or the older 10- and 5-field ones (the
            newer fields at their defaults). An id that is not (or no longer)
            registered, or registered for the other place, reads as the
            default. False if malformed. */
        static bool fromString (const juce::String& text, Options& options);
    };

    explicit AnalyzerPanel (EqCurveEditor::StoreProvider storeProvider);

    SpectrumAnalyzer& getAnalyzer() noexcept { return analyzer; }
    EqCurveEditor& getEqEditor() noexcept { return eqEditor; }

    void setOptions (const Options& options);
    const Options& getOptions() const noexcept { return options; }
    /** Called when the user changed a display option (for persistence). */
    std::function<void (const Options&)> onOptionsChanged;

    /** Freeze (re-)captures the traces; clear removes them (not persisted). */
    void freeze();
    void clearFreeze();

    /** The visualiser views (created on first use) and the selected ones (nullptr: none). */
    vis::VisualiserHost& getVisualisers() noexcept { return visualisers; }
    vis::Visualiser* getMainVisualiser() const noexcept { return mainView; }
    vis::Visualiser* getStripVisualiser() const noexcept { return stripView; }
    /** True while a main visualiser replaces the spectrum (not beside it). */
    bool isSpectrumReplaced() const noexcept;
    /** Once per display frame (after the feed's pushes): advances the fed views. */
    void advanceVisualisers (const MeterSnapshot& meters, double dtSeconds, double sampleRate);
    /** The loudness target the loudness history draws: the maximizer's
        automatic drive target while the maximizer and its automatic drive are on, else the auto-level target
        while auto level is on; NaN otherwise. */
    static float loudnessTarget (const flub::param::ParameterStore& store, juce::String& name);

    juce::Button& getViewButton() noexcept { return viewButton; }
    juce::Button& getDiffButton() noexcept { return diffButton; }
    juce::Button& getFreezeButton() noexcept { return freezeButton; }

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void applyOptions();
    void changed();
    void showViewMenu();
    float legendWidth() const;
    juce::String caption() const;
    /** Applies a View menu item of the Visualiser section; false for other items. */
    bool applyVisualiserMenuItem (int item);

    SpectrumAnalyzer analyzer;
    EqCurveEditor eqEditor;
    juce::TextButton preButton { "In" }, postButton { "Out" }, tiltButton { "Tilt" }, holdButton { "Hold" };
    juce::TextButton viewButton { "View" }, diffButton { "Diff" }, freezeButton { "Freeze" };
    juce::ComboBox rangeBox;
    Options options;
    juce::Rectangle<int> headerArea;
    EqCurveEditor::StoreProvider stores;
    vis::VisualiserHost visualisers;
    vis::Visualiser* mainView = nullptr;
    vis::Visualiser* stripView = nullptr;
};
} // namespace flub::app::ui
