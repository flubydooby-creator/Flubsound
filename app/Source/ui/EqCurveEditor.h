// Flubsound Pro - interactive parametric EQ curve, drawn over the analyser.
//
// * The combined response of the 10 bands (+ EQ output gain) is computed on
//   the UI thread with flub::ParametricEq::responseDb from the selected
//   strip's ParameterStore values, only when the store changed.
// * Band nodes (one colour per band):
//     drag            frequency / gain (gain only for bell and shelf types;
//                     Shift = fine)
//     mouse wheel     Q
//     double-click    reset the band
//     right-click     type / slope / enable / reset menu
//     keyboard        Left/Right frequency, Up/Down gain, +/- Q, Delete =
//                     enable/disable, Tab / [ ] = select band
// * The live dynamic-EQ gains (MeterBus::dynEqGainDb: 4 user bands + the 4
//   internal mode bands) are shown as "ghost" markers.
// Geometry (frequency axis, plot area) comes from the SpectrumAnalyzer this
// editor is stacked on; the right-hand axis shows EQ gain (+-6 / 12 / 24 dB).
#pragma once

#include "SpectrumAnalyzer.h"
#include "flub/dsp/ParametricEq.h"
#include "flub/engine/Parameters.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>

namespace flub::app::ui
{
class EqCurveEditor : public juce::Component
{
public:
    using StoreProvider = std::function<flub::param::ParameterStore*()>;
    static constexpr int kNumDynMarkers = 8;

    EqCurveEditor (SpectrumAnalyzer& geometry, StoreProvider storeProvider);

    void setSampleRate (double newSampleRate);
    /** Re-reads the bands when the store (or its version) changed. Cheap; call per frame. */
    void refresh (bool force = false);
    /** Live dynamic-EQ gains (dB) of the 8 dynamic bands; mode picks the internal band frequencies. */
    void setDynamicEqState (const std::array<float, kNumDynMarkers>& gainsDb, flub::param::ModeValue mode);

    void setRangeDb (float rangeDb);
    float getRangeDb() const noexcept { return rangeDb; }

    int getSelectedBand() const noexcept { return selected; }
    void selectBand (int band);
    std::function<void (int band)> onBandSelected;

    static juce::Colour bandColour (int band);

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    bool keyPressed (const juce::KeyPress& key) override;
    void focusGained (FocusChangeType) override { invalidate(); }
    void focusLost (FocusChangeType) override { invalidate(); }
    void lookAndFeelChanged() override;

private:
    static constexpr int kBands = flub::param::kEqBands;

    static bool typeHasGain (flub::EqBandType type) noexcept;
    float yForGain (float db) const noexcept;
    float gainForY (float y) const noexcept;
    juce::Point<float> nodePosition (int band) const;
    int nodeAt (juce::Point<float> p) const;
    void rebuildCurve();
    void set (int band, flub::param::EqField field, float value);
    void resetBand (int band);
    void showBandMenu (int band);
    juce::String describeBand (int band) const;
    void drawBubble (juce::Graphics& g, int band) const;
    void renderLayer (juce::Graphics& g);
    void invalidate();

    SpectrumAnalyzer& geometry;
    StoreProvider storeProvider;
    std::array<flub::EqBandParams, kBands> bands {};
    float outputGainDb = 0.0f;
    bool eqEnabled = true;
    double sampleRate = 48000.0;
    float rangeDb = 12.0f;

    const flub::param::ParameterStore* lastStore = nullptr;
    uint32_t lastVersion = 0;

    juce::Path curve, curveFill, selectedFill;
    juce::Image layer;          // cached curve / nodes / labels
    float layerScale = 1.0f;
    bool layerDirty = true;
    std::array<float, kNumDynMarkers> dynGains {};
    std::array<float, kNumDynMarkers> dynFreqs {};

    int selected = -1, hovered = -1, dragging = -1;
    float dragStartFreq = 0.0f, dragStartGain = 0.0f;
    juce::Point<float> dragStartPos;
};
} // namespace flub::app::ui
