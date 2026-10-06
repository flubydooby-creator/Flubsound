// Flubsound Pro - the spectrum and the spectrogram for the visualiser window.
//
// The analyser panel's Spectrum + EQ view is not a visualiser (it is the
// panel itself), so the pop-out window (VisualiserWindow) shows these two
// read-only mirrors of it instead: "spectrum" draws the analyser's displayed
// input (grey fill) and output (accent fill and glowing line) levels with the
// panel's tilt setting, "spectrogram" writes the analysed output levels into
// a scrolling Spectrogram image (one row per 1/60 s, newest on top). Both
// read the panel's SpectrumAnalyzer through FrameContext::spectrum: no second
// analysis. They are not in the registry (the panel has the real thing).
// Message thread only; no allocation per frame.
#pragma once

#include "../Spectrogram.h"
#include "Visualiser.h"

#include <array>

namespace flub::app::ui::vis
{
class SpectrumMirror : public Visualiser
{
public:
    enum class Mode
    {
        Spectrum,
        Spectrogram
    };

    static constexpr int kPoints = 360;
    static constexpr float kMinDb = -84.0f, kMaxDb = 0.0f;
    static constexpr float kSpectrogramFloorDb = -90.0f, kSpectrogramTopDb = -6.0f;
    static constexpr int kSpectrogramRows = 360; // 6 s at 60 rows/s
    static constexpr double kRowSeconds = 1.0 / 60.0;

    explicit SpectrumMirror (Mode mode);

    void reset() override;
    void advance (const FrameContext& frame) override;

    Mode getMode() const noexcept { return mode; }
    static double pointHz (int point) noexcept;
    /** The output level (dB, tilt as the panel draws it) at point i, as last read. */
    float getPostDb (int point) const noexcept { return post[static_cast<size_t> (point)]; }
    const Spectrogram& getSpectrogram() const noexcept { return spectrogram; }

    void paint (juce::Graphics& g) override;
    void resized() override;
    void lookAndFeelChanged() override;
    void parentHierarchyChanged() override { lookAndFeelChanged(); }

private:
    void rebuildPaths();
    float yFor (float db) const noexcept;
    float xFor (int point) const noexcept;

    Mode mode;
    std::array<double, kPoints> hz {};
    std::array<float, kPoints> tiltDb {}, pre {}, post {}, row {};
    bool tilt = true, anyData = false;
    double rowElapsed = 0.0;
    Spectrogram spectrogram { kPoints, kSpectrogramRows };
    juce::Path preFill, postFill, postLine;
    juce::Colour accent;
    juce::Rectangle<float> well, plot;
};
} // namespace flub::app::ui::vis
