// Flubsound Pro - auto-generated editor for a set of parameters.
//
// Builds one control per parameter id from flub::param::layout():
//   Toggle -> switch, Choice -> combo box with caption, everything else ->
//   small ParamKnob. Banded parameters ("eq.<n>.*", "dyneq.<n>.*") are
//   grouped into one titled row per band ("Band 3", "Dynamic band 2") with
//   the band prefix removed from the captions. The layout flows left to
//   right and wraps; getHeightForWidth() lets a Viewport size it.
// Controls are bound through the owner's ParameterBinder (and unbound again
// when the grid is destroyed); each has its ParamHints tooltip (docs/11 E39).
#pragma once

#include "ParamKnob.h"
#include "ParameterBinding.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace flub::app::ui
{
class ParamGrid : public juce::Component
{
public:
    ParamGrid (ParameterBinder& binder, const std::vector<int>& paramIds);
    ~ParamGrid() override;

    /** All parameter ids of a layout group (Info::group), in layout order. */
    static std::vector<int> idsForGroup (const juce::String& group);

    int getHeightForWidth (int width) const;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    struct Cell
    {
        int paramId = -1;
        std::unique_ptr<juce::Component> control; // ParamKnob, ToggleButton or ComboBox
        juce::String caption;                      // combo boxes: drawn above
        int width = 68;
    };

    struct Section
    {
        juce::String title;
        std::vector<Cell> cells;
        juce::Rectangle<int> titleArea;
    };

    int layoutSections (int width, bool apply);

    ParameterBinder& binder;
    std::vector<Section> sections;
};
} // namespace flub::app::ui
