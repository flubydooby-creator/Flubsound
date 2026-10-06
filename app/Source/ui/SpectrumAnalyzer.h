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
// density (Hann coherent gain and 1.5-bin noise bandwidth removed) plus the
// bandwidth of a 1/6-octave band at 1 kHz, so pink noise reads its band
// level at the tilt pivot and a sine reads its dBFS value there (within
// +0.4 dB at 48 kHz, bands being whole bins). Display: log frequency axis,
// dB axis, optional +4.5 dB/octave tilt around 1 kHz (music looks "flat"),
// pre = grey fill, post = accent line with a soft glow, peak hold = thin
// accent line.
//
// The plot geometry is shared with EqCurveEditor, which is laid over this
// component with identical bounds.
//
// Optional views (all off by default; with them off the analyser draws and
// analyses exactly as described above). docs/06 §6.4 has the details:
//   Difference      post minus pre per display point (ballistic values,
//                   smoothed over +-6 points, gated below -100 dB), drawn
//                   against the EQ gain axis (0 dB = no change), green.
//   Sharper lows    a second analysis per stream of the signal decimated to
//                   ~8 kHz (4th-order Butterworth anti-alias filter):
//                   4096-sample Hann window (24576 input samples, 0.51 s, at
//                   48 kHz), zero-padded to an 8192-point FFT, one hop per
//                   kHop / decimation decimated samples (about the main
//                   FFT's 1024 input samples). Below 220 Hz the display points read it
//                   (interpolated, no band averaging), crossfaded in dB to
//                   the main analysis up to 300 Hz. Same density scale.
//   Stereo width    side (L - R) / 2 band level against the post mid band
//                   level: width = S / (M + S) in power, 0 mono, 0.5 wide
//                   (uncorrelated), 1 anti-phase; a translucent area in the
//                   bottom fifth of the plot, in the other mode's accent.
//   Spectrogram     the post analysis written as one image row per hop
//                   (Spectrogram), newest on top, instead of the traces.
//   Freeze          a dashed copy of the current post (and pre) traces.
//   Piano keys      a keyboard C1 .. C8 along the bottom of the plot (inside
//                   it: the plot never changes size), the hovered key lit.
// The hover readout itself is drawn by EqCurveEditor (it gets the mouse);
// the analyser provides the levels and the note naming (describeFrequency).
#pragma once

#include "Spectrogram.h"

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

    // Sharper lows: the decimated long analysis (see above).
    static constexpr int kLongWindow = 4096;                 // decimated samples
    static constexpr int kLongFftOrder = 13;                 // zero-padded x2
    static constexpr int kLongFftSize = 1 << kLongFftOrder;  // 8192
    static constexpr double kLongTargetRate = 8000.0;        // decimation factor = round (fs / this)
    static constexpr double kCrossoverLoHz = 220.0, kCrossoverHiHz = 300.0;

    // Spectrogram history: one row per hop (1024 samples: ~5.5 s at 48 kHz).
    static constexpr int kSpectrogramRows = 256;

    // Plot insets inside the component (left: dB labels, right: EQ gain labels).
    static constexpr float kLeftInset = 34.0f, kRightInset = 34.0f, kTopInset = 10.0f, kBottomInset = 22.0f;
    // Piano keys: height of the strip at the bottom of the plot; lowest / highest C (MIDI).
    static constexpr float kKeysHeight = 11.0f;
    static constexpr int kKeysLowMidi = 24, kKeysHighMidi = 108; // C1 .. C8

    SpectrumAnalyzer();

    // ---- Data -------------------------------------------------------------------------
    void setSampleRate (double newSampleRate);
    void push (bool post, const float* samples, int numSamples);
    /** The post tap's side signal (L - R) / 2, for the stereo-width view. */
    void pushSide (const float* samples, int numSamples);
    /** Clears history and display (strip switch, engine rebuilt). A frozen
        trace is kept (it is a reference the user captured). */
    void reset();
    /** FFT hops, ballistics, path rebuild; repaints if anything moved. */
    void advance (double dtSeconds);
    /** Latest analysed 1/6-octave band level (dB, before ballistics and
        tilt) at the display point nearest `hz`; pre or post stream. With
        Sharper lows on, the long analysis below the crossover. */
    float getBandLevelDb (bool post, double hz) const noexcept;
    /** The level the trace shows (after ballistics, before tilt) at the
        display point nearest `hz`; the hover readout's value. */
    float getDisplayLevelDb (bool post, double hz) const noexcept;
    /** Difference view: smoothed post minus pre (dB) at the point nearest `hz`
        (computed while the view is on). */
    float getDifferenceDb (double hz) const noexcept;
    /** Stereo width view: the analysed S / (M + S) at the point nearest `hz` (0..1). */
    float getStereoWidth (double hz) const noexcept;
    /** True once the data arrived for the analysis the display uses at `hz`. */
    bool hasData() const noexcept { return anyData; }

    /** Resolution bandwidth (Hz) of the long analysis (Hann ENBW, 1.5 of its
        bins): a sine below the crossover reads its level plus
        10 log10 (B1k / this), like noise it reads on the density scale. */
    double getLowResolutionBandwidthHz() const noexcept;
    /** Decimation factor of the long analysis at the current sample rate. */
    int getDecimation() const noexcept { return decimation; }

    // ---- Pure helpers (tested) ---------------------------------------------------------
    /** Nearest equal-tempered note (A4 = 440 Hz) and the offset in cents,
        e.g. 55.4 Hz -> "A1 +13c"; "" outside 8 Hz .. 30 kHz. */
    static juce::String noteName (double hz);
    /** "55.4 Hz", "440 Hz", "1.25 kHz", "12.5 kHz". */
    static juce::String frequencyText (double hz);
    /** The readout's first line: "A1 +3c · 55.4 Hz". */
    static juce::String describeFrequency (double hz);
    /** The difference trace: out[i] = mean over the points j within
        +-halfWidth of i (and inside [0, n)) where max (pre[j], post[j]) >
        gateDb of post[j] - pre[j]; 0 where no point qualifies. */
    static void computeDifference (const float* pre, const float* post, float* out, int n, int halfWidth = 6, float gateDb = -100.0f);
    /** S / (M + S) from the mid and side levels (dB); 0 where both are below gateDb. */
    static float widthFromLevels (float midDb, float sideDb, float gateDb = -100.0f) noexcept;

    // ---- Options -------------------------------------------------------------------------
    void setShowPre (bool shouldShow);
    void setShowPost (bool shouldShow);
    void setTiltEnabled (bool shouldTilt);
    void setPeakHoldEnabled (bool shouldHold);
    bool isTiltEnabled() const noexcept { return tilt; }
    bool isShowingPre() const noexcept { return showPre; }
    bool isShowingPost() const noexcept { return showPost; }
    void setDifferenceEnabled (bool shouldShow);
    void setSharpLowsEnabled (bool shouldUse);
    void setWidthEnabled (bool shouldShow);
    void setSpectrogramEnabled (bool shouldShow);
    void setPianoKeysEnabled (bool shouldShow);
    bool isSpectrogramEnabled() const noexcept { return spectrogramOn; }
    bool isDifferenceEnabled() const noexcept { return differenceOn; }
    /** The EQ display range (the difference trace uses the EQ gain axis). */
    void setEqRangeDb (float rangeDb);

    /** Freeze: captures the post trace (and the pre trace if shown) as a
        dashed reference; a second call re-captures. */
    void freeze();
    void clearFreeze();
    bool isFrozen() const noexcept { return frozen; }

    /** The hovered frequency (<= 0: none): lights its piano key. */
    void setHoverFrequency (double hz);

    const Spectrogram& getSpectrogram() const noexcept { return spectrogram; }

    // ---- Geometry (component coordinates) ----------------------------------------------------
    juce::Rectangle<float> getPlotArea() const noexcept { return plot; }
    float xForFrequency (double hz) const noexcept;
    double frequencyForX (float x) const noexcept;
    float yForDb (float db) const noexcept;
    /** The EQ gain axis (right-hand labels): EqCurveEditor and the difference trace. */
    float yForGain (float db, float rangeDb) const noexcept;
    /** y of the trace (tilt included) at the point nearest `hz`. */
    float traceY (bool post, double hz) const noexcept;
    /** Piano keys: the key of MIDI note `midi` (C1 .. C8, else empty). A
        white key reaches to the middle of a neighbouring black key (else to
        the semitone edge, E-F and B-C); a black key spans its semitone and
        62 % of the strip's height. */
    juce::Rectangle<float> getKeyBounds (int midi) const noexcept;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void moved() override;
    void lookAndFeelChanged() override;

private:
    struct Stream
    {
        std::vector<float> history;               // kFftSize ring
        int writePos = 0, sinceHop = 0, filled = 0;
        std::vector<float> shortDb;               // main (4096) analysis
        std::vector<float> analysisDb, displayDb, peakDb, peakAge;
        double idleSeconds = 0.0;
        bool fresh = false;

        // Sharper lows: decimated history and its analysis.
        std::vector<float> longHistory, longDb;   // kLongWindow ring; per display point
        int longWrite = 0, longSinceHop = 0, longFilled = 0, decimPhase = 0;
        std::array<double, 4> lpState {};         // two biquads (DF2T)
        bool longReady = false;
    };

    struct Band
    {
        int lo = 0, hi = 0; // averaged bin range (inclusive) when hi >= lo
        float frac = 0.0f;  // interpolation position between lo and lo + 1 when hi < lo
    };

    void analyse (Stream& s);
    void analyseLong (Stream& s);
    void combine (Stream& s);
    void pushInto (Stream& s, const float* samples, int numSamples, bool decimate);
    void rebuildBands();
    void rebuildPaths();
    void rebuildFrozenPaths();
    void rebuildKeys();
    void renderGrid (float scale);
    void buildTrace (const std::vector<float>& values, juce::Path& line, juce::Path* fill) const;
    void updateSpectrogramColours();
    void paintKeys (juce::Graphics& g, juce::Colour accent) const;
    size_t pointIndex (double hz) const noexcept;

    juce::dsp::FFT fft { kFftOrder };
    juce::dsp::FFT longFft { kLongFftOrder };
    std::vector<float> window, fftData, longWindow, longData;
    std::array<Stream, 2> streams; // 0 = pre, 1 = post
    Stream side;                   // post side (L - R) / 2: main analysis only
    std::vector<Band> bands;
    std::vector<float> pointHz, pointX, tiltDb, longWeight;
    int longPoints = 0; // display points below kCrossoverHiHz
    double sampleRate = 48000.0;
    float calibrationDb = 0.0f; // sine calibration - Hann noise bandwidth + 1/6-octave bandwidth at 1 kHz
    float longCalibrationDb = 0.0f;
    int decimation = 6;
    int longHop = 170; // decimated samples: kHop / decimation (the main analysis' update rate)
    std::array<double, 10> lpCoeffs {}; // b0 b1 b2 a1 a2 per biquad
    bool showPre = true, showPost = true, tilt = true, peakHold = true;
    bool differenceOn = false, sharpLows = false, widthOn = false, spectrogramOn = false, keysOn = false;
    float eqRangeDb = 12.0f;

    std::vector<float> differenceDb, widthTarget, widthDisplay, frozenPre, frozenPost, rowScratch;
    bool frozen = false, frozenHasPre = false, frozenPathsDirty = true;
    double hoverHz = 0.0;

    Spectrogram spectrogram { kNumPoints, kSpectrogramRows };
    juce::Colour spectrogramAccent;

    juce::Rectangle<float> plot;
    juce::Image gridImage;
    float gridScale = 0.0f;
    bool gridForSpectrogram = false;
    juce::Path preLine, preFill, postLine, postFill, peakLine, diffLine, diffFill, widthFill, widthLine, frozenPreDashes, frozenPostDashes;
    juce::Path whiteKeys, blackKeys, keySeparators;
    bool anyData = false;
};
} // namespace flub::app::ui
