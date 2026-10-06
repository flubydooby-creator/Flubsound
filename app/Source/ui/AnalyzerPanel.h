// Flubsound Pro - the analyser panel: SpectrumAnalyzer with the
// EqCurveEditor stacked on top (identical bounds, shared plot geometry) and a
// header row with the display options (pre / post / tilt / peak hold) and the
// EQ display range.
//
// The optional views (difference, sharper lows, stereo width, piano keys,
// spectrogram, freeze; all off by default) live in the "View" chip's menu.
// Where the header has room beside the legend, Diff and Freeze also get
// chips of their own (Diff first); the legend is never pushed out by them.
#pragma once

#include "EqCurveEditor.h"
#include "SpectrumAnalyzer.h"
#include "Widgets.h"

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

        /** The `ui.analyzer` preference: "pre,post,tilt,hold,range,diff,lows,width,keys,spectrogram". */
        juce::String toString() const;
        /** Reads toString()'s format, or the older 5-field one (the new views off). False if malformed. */
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

    SpectrumAnalyzer analyzer;
    EqCurveEditor eqEditor;
    juce::TextButton preButton { "In" }, postButton { "Out" }, tiltButton { "Tilt" }, holdButton { "Hold" };
    juce::TextButton viewButton { "View" }, diffButton { "Diff" }, freezeButton { "Freeze" };
    juce::ComboBox rangeBox;
    Options options;
    juce::Rectangle<int> headerArea;
};
} // namespace flub::app::ui
