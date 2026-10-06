// Flubsound Pro - stereo field by frequency (visualiser "stereo-field").
//
// Where each third-octave band of the output sits between left and right,
// and how wide it is. The post mid / side pairs are analysed together
// (4096-point Hann FFT of each, every 2048 samples); per band the mid and
// side powers PM, PS and their cross term C = sum Re (M conj (S)) give
//     PL = PM + PS + 2C,  PR = PM + PS - 2C        (L = M + S, R = M - S)
//     pan         = (PR - PL) / (PL + PR) = -2C / (PM + PS)   (-1 left .. +1 right)
//     correlation = (PM - PS) / sqrt (PL PR)                   (+1 point source,
//                   0 unrelated (wide), -1 out of phase; +1 when one side is silent)
//     level       = 10 log10 (PM + PS)
// PM, PS and C are averaged over about kSmoothingSeconds. Display: frequency
// up the plot (25 Hz at the bottom, 20 kHz at the top), pan across (L at
// the left), one dot per band in frequencyColour; a band that is wide
// (correlation below 1) also gets a bar whose half length is
// (1 - correlation) / 2 of the half width; brightness and size follow the
// level relative to the loudest band (kRangeDb). Message thread only; FFT
// buffers are allocated in the constructor.
#pragma once

#include "Visualiser.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <cstdint>
#include <vector>

namespace flub::app::ui::vis
{
class StereoField : public Visualiser
{
public:
    static constexpr int kFftOrder = 12;
    static constexpr int kFftSize = 1 << kFftOrder; // 4096
    static constexpr int kHop = kFftSize / 2;
    static constexpr int kNumBands = 30;            // ISO third octaves 25 Hz .. 20 kHz
    static constexpr double kSmoothingSeconds = 0.3;
    static constexpr float kRangeDb = 48.0f;        // shown below the loudest band
    static constexpr float kGateDb = -100.0f;       // bands below read as empty

    struct Band
    {
        float pan = 0.0f, correlation = 1.0f, levelDb = -200.0f;
    };

    StereoField();

    void setSampleRate (double sampleRate) override;
    void reset() override;
    void pushPost (const float* mid, const float* side, int numSamples) override;
    void advance (const FrameContext& frame) override;

    /** Pan, correlation and level of a band from its mid / side powers and
        cross term (see above). */
    static Band analyse (double midPower, double sidePower, double cross) noexcept;
    /** Centre frequency of band i (1000 * 2^((i - 16) / 3) Hz). */
    static double bandCentreHz (int band) noexcept;
    /** The band whose centre is nearest `hz`. */
    static int bandIndexFor (double hz) noexcept;

    const Band& getBand (int band) const noexcept { return bands[static_cast<size_t> (band)]; }
    /** Analyses run so far. */
    int64_t getAnalyses() const noexcept { return analyses; }

    /** Plot geometry (component coordinates). */
    juce::Rectangle<float> getPlotArea() const noexcept { return plot; }
    float xForPan (float pan) const noexcept;
    float yForBand (int band) const noexcept;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void analyseNow (double seconds);
    void rebuildBins();

    juce::dsp::FFT fft { kFftOrder };
    std::vector<float> window, midHistory, sideHistory, midFft, sideFft;
    int writePos = 0, sinceHop = 0, filled = 0;
    double sampleRate = 48000.0;
    std::array<int, kNumBands> binLo {}, binHi {};
    std::array<double, kNumBands> pm {}, ps {}, cr {};
    std::array<Band, kNumBands> bands {};
    int64_t analyses = 0;
    bool primed = false;

    juce::Rectangle<float> well, plot;
};
} // namespace flub::app::ui::vis
