#include "StereoField.h"

#include "VisCommon.h"

#include "../Theme.h"

#include <cmath>

namespace flub::app::ui::vis
{
StereoField::StereoField()
{
    window.resize (static_cast<size_t> (kFftSize));
    for (int i = 0; i < kFftSize; ++i)
        window[static_cast<size_t> (i)] =
            0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * static_cast<float> (i) / static_cast<float> (kFftSize));
    midHistory.assign (static_cast<size_t> (kFftSize), 0.0f);
    sideHistory.assign (static_cast<size_t> (kFftSize), 0.0f);
    midFft.assign (static_cast<size_t> (2 * kFftSize), 0.0f);
    sideFft.assign (static_cast<size_t> (2 * kFftSize), 0.0f);
    rebuildBins();
    setOpaque (false);
    setInterceptsMouseClicks (false, false);
}

double StereoField::bandCentreHz (int band) noexcept
{
    return 1000.0 * std::pow (2.0, (band - 16) / 3.0);
}

int StereoField::bandIndexFor (double hz) noexcept
{
    const double i = 16.0 + 3.0 * std::log2 (juce::jmax (hz, 1.0) / 1000.0);
    return juce::jlimit (0, kNumBands - 1, static_cast<int> (std::lround (i)));
}

StereoField::Band StereoField::analyse (double midPower, double sidePower, double cross) noexcept
{
    Band b;
    const double total = midPower + sidePower;
    if (! (total > 1.0e-30))
        return b;
    const double pl = juce::jmax (0.0, total + 2.0 * cross), pr = juce::jmax (0.0, total - 2.0 * cross);
    b.pan = static_cast<float> (juce::jlimit (-1.0, 1.0, (pr - pl) / (pl + pr)));
    if (juce::jmin (pl, pr) <= 1.0e-6 * juce::jmax (pl, pr))
        b.correlation = 1.0f; // one side silent: a point source at that side
    else
        b.correlation = static_cast<float> (juce::jlimit (-1.0, 1.0, (midPower - sidePower) / std::sqrt (pl * pr)));
    b.levelDb = static_cast<float> (10.0 * std::log10 (total));
    return b;
}

void StereoField::setSampleRate (double newSampleRate)
{
    if (newSampleRate <= 0.0 || newSampleRate == sampleRate)
        return;
    sampleRate = newSampleRate;
    rebuildBins();
    reset();
}

void StereoField::rebuildBins()
{
    const double binHz = sampleRate / kFftSize;
    const int top = kFftSize / 2 - 1;
    for (int i = 0; i < kNumBands; ++i)
    {
        const double fc = bandCentreHz (i);
        int lo = static_cast<int> (std::ceil (fc * std::pow (2.0, -1.0 / 6.0) / binHz));
        int hi = static_cast<int> (std::floor (fc * std::pow (2.0, 1.0 / 6.0) / binHz));
        if (hi < lo)
            lo = hi = static_cast<int> (std::lround (fc / binHz));
        lo = juce::jmax (1, lo);
        hi = juce::jmin (top, hi);
        binLo[static_cast<size_t> (i)] = lo;
        binHi[static_cast<size_t> (i)] = hi; // hi < lo: above Nyquist, the band stays empty
    }
}

void StereoField::reset()
{
    std::fill (midHistory.begin(), midHistory.end(), 0.0f);
    std::fill (sideHistory.begin(), sideHistory.end(), 0.0f);
    writePos = sinceHop = filled = 0;
    pm.fill (0.0);
    ps.fill (0.0);
    cr.fill (0.0);
    bands.fill (Band {});
    primed = false;
    repaint();
}

void StereoField::pushPost (const float* mid, const float* side, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
    {
        midHistory[static_cast<size_t> (writePos)] = mid[i];
        sideHistory[static_cast<size_t> (writePos)] = side[i];
        writePos = (writePos + 1) % kFftSize;
    }
    sinceHop += numSamples;
    filled = juce::jmin (kFftSize, filled + numSamples);
}

void StereoField::advance (const FrameContext&)
{
    if (filled < kFftSize || sinceHop < kHop)
        return;
    const double seconds = static_cast<double> (sinceHop) / sampleRate;
    sinceHop = 0;
    analyseNow (seconds);
    repaint();
}

void StereoField::analyseNow (double seconds)
{
    for (int i = 0; i < kFftSize; ++i)
    {
        const auto src = static_cast<size_t> ((writePos + i) % kFftSize);
        midFft[static_cast<size_t> (i)] = midHistory[src] * window[static_cast<size_t> (i)];
        sideFft[static_cast<size_t> (i)] = sideHistory[src] * window[static_cast<size_t> (i)];
    }
    std::fill (midFft.begin() + kFftSize, midFft.end(), 0.0f);
    std::fill (sideFft.begin() + kFftSize, sideFft.end(), 0.0f);
    fft.performRealOnlyForwardTransform (midFft.data(), true);
    fft.performRealOnlyForwardTransform (sideFft.data(), true);

    // Scale so a full-scale sine in one band reads about 0 dB (Hann: |X| = A N / 4).
    const double norm = 1.0 / (0.25 * kFftSize * 0.25 * kFftSize);
    const double alpha = primed ? 1.0 - std::exp (-seconds / kSmoothingSeconds) : 1.0;
    primed = true;
    ++analyses;
    for (size_t b = 0; b < static_cast<size_t> (kNumBands); ++b)
    {
        double m2 = 0.0, s2 = 0.0, c = 0.0;
        for (int k = binLo[b]; k <= binHi[b]; ++k)
        {
            const double mr = midFft[static_cast<size_t> (2 * k)], mi = midFft[static_cast<size_t> (2 * k + 1)];
            const double sr = sideFft[static_cast<size_t> (2 * k)], si = sideFft[static_cast<size_t> (2 * k + 1)];
            m2 += mr * mr + mi * mi;
            s2 += sr * sr + si * si;
            c += mr * sr + mi * si;
        }
        pm[b] += alpha * (m2 * norm - pm[b]);
        ps[b] += alpha * (s2 * norm - ps[b]);
        cr[b] += alpha * (c * norm - cr[b]);
        bands[b] = analyse (pm[b], ps[b], cr[b]);
    }
}

// =============================================================================
// Display
// =============================================================================
void StereoField::resized()
{
    well = getLocalBounds().toFloat();
    plot = well;
    plot.removeFromLeft (40.0f);
    plot.removeFromRight (18.0f);
    plot.removeFromTop (12.0f);
    plot.removeFromBottom (22.0f);
}

float StereoField::xForPan (float pan) const noexcept
{
    return plot.getCentreX() + juce::jlimit (-1.0f, 1.0f, pan) * plot.getWidth() * 0.5f;
}

float StereoField::yForBand (int band) const noexcept
{
    return plot.getBottom() - (static_cast<float> (band) + 0.5f) / static_cast<float> (kNumBands) * plot.getHeight();
}

void StereoField::paint (juce::Graphics& g)
{
    drawWell (g, well);

    // Grid: centre, half-left / half-right, and the decades.
    g.setColour (Palette::gridMinor.brighter (0.1f));
    for (const float p : { -0.5f, 0.5f })
        g.drawVerticalLine (juce::roundToInt (xForPan (p)), plot.getY(), plot.getBottom());
    for (const double f : { 100.0, 1000.0, 10000.0 })
        g.drawHorizontalLine (juce::roundToInt (yForBand (bandIndexFor (f))), plot.getX(), plot.getRight());
    g.setColour (Palette::gridMajor);
    g.drawVerticalLine (juce::roundToInt (xForPan (0.0f)), plot.getY(), plot.getBottom());

    g.setFont (labelFont());
    g.setColour (labelColour());
    float lastLabelY = 1.0e9f;
    for (const double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
    {
        const float y = yForBand (bandIndexFor (f));
        if (lastLabelY - y < 13.0f) // short plots: no overlapping labels
            continue;
        lastLabelY = y;
        const auto text = f >= 1000.0 ? juce::String (juce::roundToInt (f / 1000.0)) + "k" : juce::String (juce::roundToInt (f));
        g.drawText (text, juce::Rectangle<float> (well.getX(), y - 7.0f, 34.0f, 14.0f), juce::Justification::centredRight, false);
    }
    g.setFont (Theme::font (11.0f, true));
    g.setColour (Palette::muted);
    const float labelY = plot.getBottom() + 4.0f;
    g.drawText ("L", juce::Rectangle<float> (plot.getX() - 6.0f, labelY, 14.0f, 14.0f), juce::Justification::centredLeft, false);
    g.drawText ("C", juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ plot.getCentreX(), labelY + 7.0f }), juce::Justification::centred, false);
    g.drawText ("R", juce::Rectangle<float> (plot.getRight() - 8.0f, labelY, 14.0f, 14.0f), juce::Justification::centredRight, false);

    float loudest = kGateDb;
    for (const auto& b : bands)
        loudest = juce::jmax (loudest, b.levelDb);
    if (loudest <= kGateDb)
        return;

    const float floorDb = loudest - kRangeDb;
    const float halfWidth = plot.getWidth() * 0.5f;
    const float rowHeight = plot.getHeight() / static_cast<float> (kNumBands);
    const float baseRadius = juce::jlimit (1.5f, 4.5f, rowHeight * 0.6f);
    const float barHalfHeight = juce::jlimit (1.0f, 2.5f, rowHeight * 0.35f);
    for (int i = 0; i < kNumBands; ++i)
    {
        const auto& b = bands[static_cast<size_t> (i)];
        if (b.levelDb <= juce::jmax (floorDb, kGateDb))
            continue;
        const float t = juce::jlimit (0.0f, 1.0f, (b.levelDb - floorDb) / kRangeDb);
        const auto colour = frequencyColour (bandCentreHz (i));
        const float x = xForPan (b.pan), y = yForBand (i);

        // Width: a bar centred on the pan position.
        if (b.correlation < 0.98f)
        {
            const float half = (1.0f - b.correlation) * 0.5f * halfWidth;
            const auto bar = juce::Rectangle<float>::leftTopRightBottom (juce::jmax (plot.getX(), x - half), y - barHalfHeight,
                                                                       juce::jmin (plot.getRight(), x + half), y + barHalfHeight);
            g.setColour (colour.withAlpha (0.12f + 0.38f * t));
            g.fillRoundedRectangle (bar, 2.5f);
            if (b.correlation < 0.0f)
            {
                g.setColour (Palette::red.withAlpha (0.35f + 0.4f * t));
                g.drawRoundedRectangle (bar, 2.5f, 1.0f);
            }
        }
        const float radius = baseRadius * (0.6f + 0.7f * t);
        g.setColour (colour.withAlpha (0.16f * t));
        g.fillEllipse (juce::Rectangle<float> (radius * 4.4f, radius * 4.4f).withCentre ({ x, y }));
        g.setColour (colour.withAlpha (0.35f + 0.65f * t));
        g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre ({ x, y }));
    }
}
} // namespace flub::app::ui::vis
