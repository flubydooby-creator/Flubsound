// Flubsound Pro - the analyser panel: SpectrumAnalyzer with the
// EqCurveEditor stacked on top (identical bounds, shared plot geometry) and a
// header row with the display options (pre / post / tilt / peak hold) and the
// EQ display range.
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
    };

    explicit AnalyzerPanel (EqCurveEditor::StoreProvider storeProvider);

    SpectrumAnalyzer& getAnalyzer() noexcept { return analyzer; }
    EqCurveEditor& getEqEditor() noexcept { return eqEditor; }

    void setOptions (const Options& options);
    const Options& getOptions() const noexcept { return options; }
    /** Called when the user changed a display option (for persistence). */
    std::function<void (const Options&)> onOptionsChanged;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void applyOptions();

    SpectrumAnalyzer analyzer;
    EqCurveEditor eqEditor;
    juce::TextButton preButton { "In" }, postButton { "Out" }, tiltButton { "Tilt" }, holdButton { "Hold" };
    juce::ComboBox rangeBox;
    Options options;
    juce::Rectangle<int> headerArea;
};
} // namespace flub::app::ui
