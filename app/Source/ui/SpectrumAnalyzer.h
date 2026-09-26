// Flubsound Pro - real-time spectrum analyser (pre vs post processing).
//
// Data path (message thread only):
//   AnalyzerFeed -> push() : mono mid samples of the selected strip's pre and
//                            post analyser taps are written into two
//                            4096-sample history rings.
//   advance()              : every 1024 new samples (75 % overlap) a Hann-
//                            windowed 4096-point FFT (juce::dsp::FFT) runs;
//                            the power spectrum is averaged into 1/6-octave
//                            bands at log-spaced display points (20 Hz -
//                            20 kHz). Once per display frame the values move
//                            towards the latest analysis with attack /
//                            release ballistics, peak-hold traces are
//                            updated and the paths are rebuilt, so paint()
//                            only strokes / fills ready-made paths over a
//                            cached grid image.
// Levels are 1/6-octave band levels referred to 1 kHz: the averaged power
// density plus the bandwidth of a 1/6-octave band at 1 kHz, so pink noise
// reads its band level at the tilt pivot and a sine reads close to its dBFS
// value there. Display: log frequency axis, dB axis, optional
// +4.5 dB/octave tilt around 1 kHz (music looks "flat"), pre = grey fill,
// post = accent line with a soft glow, peak hold = thin accent line.
//
// The plot geometry is shared with EqCurveEditor, which is laid over this
// component with identical bounds.
#pragma once

#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <vector>

namespace flub::app::ui
{
class SpectrumAnalyzer : public juce::Component
{
public:
    static constexpr int kFftOrder = 12;
    static constexpr int kFftSize = 1 << kFftOrder; // 4096
    static constexpr int kHop = kFftSize / 4;       // 75 % overlap
    static constexpr int kNumPoints = 420;          // log-spaced display points
    static constexpr float kMinDb = -84.0f, kMaxDb = 0.0f;
    static constexpr float kMinHz = 20.0f, kMaxHz = 20000.0f;

    // Plot insets inside the component (left: dB labels, right: EQ gain labels).
    static constexpr float kLeftInset = 34.0f, kRightInset = 34.0f, kTopInset = 10.0f, kBottomInset = 22.0f;

    SpectrumAnalyzer();

    // ---- Data -------------------------------------------------------------------------
    void setSampleRate (double newSampleRate);
    void push (bool post, const float* samples, int numSamples);
    /** Clears history and display (strip switch, engine rebuilt). */
    void reset();
    /** FFT hops, ballistics, path rebuild; repaints if anything moved. */
    void advance (double dtSeconds);

    // ---- Options -------------------------------------------------------------------------
    void setShowPre (bool shouldShow);
    void setShowPost (bool shouldShow);
    void setTiltEnabled (bool shouldTilt);
    void setPeakHoldEnabled (bool shouldHold);
    bool isTiltEnabled() const noexcept { return tilt; }

    // ---- Geometry (component coordinates) ----------------------------------------------------
    juce::Rectangle<float> getPlotArea() const noexcept { return plot; }
    float xForFrequency (double hz) const noexcept;
    double frequencyForX (float x) const noexcept;
    float yForDb (float db) const noexcept;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void moved() override;
    void lookAndFeelChanged() override;

private:
    struct Stream
    {
        std::vector<float> history;               // kFftSize ring
        int writePos = 0, sinceHop = 0, filled = 0;
        std::vector<float> analysisDb, displayDb, peakDb, peakAge;
        double idleSeconds = 0.0;
        bool fresh = false;
    };

    struct Band
    {
        int lo = 0, hi = 0; // averaged bin range (inclusive) when hi >= lo
        float frac = 0.0f;  // interpolation position between lo and lo + 1 when hi < lo
    };

    void analyse (Stream& s);
    void rebuildBands();
    void rebuildPaths();
    void renderGrid (float scale);
    void buildTrace (const Stream& s, const std::vector<float>& values, juce::Path& line, juce::Path* fill) const;

    juce::dsp::FFT fft { kFftOrder };
    std::vector<float> window, fftData;
    std::array<Stream, 2> streams; // 0 = pre, 1 = post
    std::vector<Band> bands;
    std::vector<float> pointHz, pointX, tiltDb;
    double sampleRate = 48000.0;
    float calibrationDb = 0.0f; // sine calibration + 1/6-octave bandwidth at 1 kHz
    bool showPre = true, showPost = true, tilt = true, peakHold = true;

    juce::Rectangle<float> plot;
    juce::Image gridImage;
    float gridScale = 0.0f;
    juce::Path preLine, preFill, postLine, postFill, peakLine;
    bool anyData = false;
};
} // namespace flub::app::ui
