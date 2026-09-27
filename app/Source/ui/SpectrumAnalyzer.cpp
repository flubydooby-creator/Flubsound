#include "SpectrumAnalyzer.h"

#include "Theme.h"

#include <cmath>

namespace flub::app::ui
{
namespace
{
constexpr float kFloorDb = -140.0f;
constexpr float kAttackSeconds = 0.012f;  // rise time constant
constexpr float kReleaseSeconds = 0.30f;  // fall time constant
constexpr float kPeakHoldSeconds = 1.2f;
constexpr float kPeakFallDbPerSecond = 14.0f;
constexpr float kTiltDbPerOctave = 4.5f;
constexpr double kIdleSeconds = 0.35; // no samples for this long: let the trace fall
} // namespace

SpectrumAnalyzer::SpectrumAnalyzer()
{
    // Opaque (fills with the panel colour) so 60 Hz repaints never have to
    // repaint the parent panel behind it.
    setOpaque (true);
    setInterceptsMouseClicks (false, false);
    setTitle ("Spectrum analyser");
    setDescription ("Input and output spectrum of the selected strip, 20 Hz to 20 kHz");

    // Periodic Hann window (exact 75 % overlap-add).
    window.resize (static_cast<size_t> (kFftSize));
    for (int i = 0; i < kFftSize; ++i)
        window[static_cast<size_t> (i)] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * static_cast<float> (i) / kFftSize);
    fftData.assign (static_cast<size_t> (2 * kFftSize), 0.0f);

    pointHz.resize (static_cast<size_t> (kNumPoints));
    tiltDb.resize (static_cast<size_t> (kNumPoints));
    for (int i = 0; i < kNumPoints; ++i)
    {
        const double t = static_cast<double> (i) / (kNumPoints - 1);
        const double hz = kMinHz * std::pow (static_cast<double> (kMaxHz / kMinHz), t);
        pointHz[static_cast<size_t> (i)] = static_cast<float> (hz);
        tiltDb[static_cast<size_t> (i)] = kTiltDbPerOctave * static_cast<float> (std::log2 (hz / 1000.0));
    }

    for (auto& s : streams)
    {
        s.history.assign (static_cast<size_t> (kFftSize), 0.0f);
        s.analysisDb.assign (static_cast<size_t> (kNumPoints), kFloorDb);
        s.displayDb.assign (static_cast<size_t> (kNumPoints), kFloorDb);
        s.peakDb.assign (static_cast<size_t> (kNumPoints), kFloorDb);
        s.peakAge.assign (static_cast<size_t> (kNumPoints), 0.0f);
    }
    rebuildBands();
}

// =============================================================================
// Data
// =============================================================================
void SpectrumAnalyzer::setSampleRate (double newSampleRate)
{
    if (newSampleRate <= 0.0 || std::abs (newSampleRate - sampleRate) < 0.5)
        return;
    sampleRate = newSampleRate;
    rebuildBands();
    reset();
}

void SpectrumAnalyzer::rebuildBands()
{
    // 1/6 octave: band edges at fc * 2^(+-1/12). Where the band is narrower
    // than two bins (low frequencies) the magnitude is interpolated instead.
    const double binHz = sampleRate / kFftSize;
    const double edge = std::pow (2.0, 1.0 / 12.0);
    const int maxBin = kFftSize / 2 - 1;

    // dBFS scale (a full-scale sine reads 0 dB, noise its band power on the
    // same scale):
    //   20 log10 (4 / N)          Hann coherent gain 0.5: amplitude = 4 |X| / N
    //   - 10 log10 (1.5)          Hann equivalent noise bandwidth (bins): a tone's
    //                             |X|^2 summed over the band is 1.5x its peak bin's
    //   + 10 log10 (B1k / binHz)  mean bin power -> power of a 1/6-octave band
    //                             at 1 kHz (bandwidth B1k)
    // Bands hold whole bins, so a band's width only approximates its nominal
    // width: a 0 dBFS 1 kHz sine reads +0.3 dB at 44.1 kHz and +0.4 dB at
    // 48 kHz (its band spans 9 bins = 105 Hz instead of 115.6 Hz).
    const double bandwidthAt1k = 1000.0 * (edge - 1.0 / edge);
    const double hannEnbwBins = 1.5;
    calibrationDb = static_cast<float> (20.0 * std::log10 (4.0 / kFftSize) - 10.0 * std::log10 (hannEnbwBins)
                                        + 10.0 * std::log10 (bandwidthAt1k / binHz));

    bands.resize (static_cast<size_t> (kNumPoints));
    for (int i = 0; i < kNumPoints; ++i)
    {
        const double fc = pointHz[static_cast<size_t> (i)];
        const double lo = fc / edge / binHz, hi = fc * edge / binHz;
        auto& b = bands[static_cast<size_t> (i)];
        if (hi - lo < 2.0)
        {
            const double pos = juce::jlimit (0.0, static_cast<double> (maxBin - 1), fc / binHz);
            b.lo = static_cast<int> (std::floor (pos));
            b.frac = static_cast<float> (pos - b.lo);
            b.hi = b.lo - 1; // marks "interpolate"
        }
        else
        {
            b.lo = juce::jlimit (1, maxBin, static_cast<int> (std::ceil (lo)));
            b.hi = juce::jlimit (b.lo, maxBin, static_cast<int> (std::floor (hi)));
        }
    }
}

void SpectrumAnalyzer::push (bool post, const float* samples, int numSamples)
{
    auto& s = streams[post ? 1 : 0];
    for (int i = 0; i < numSamples; ++i)
    {
        s.history[static_cast<size_t> (s.writePos)] = samples[i];
        s.writePos = (s.writePos + 1) & (kFftSize - 1);
    }
    s.sinceHop += numSamples;
    s.filled = juce::jmin (kFftSize, s.filled + numSamples);
    s.idleSeconds = 0.0;
    s.fresh = true;
}

void SpectrumAnalyzer::reset()
{
    for (auto& s : streams)
    {
        std::fill (s.history.begin(), s.history.end(), 0.0f);
        std::fill (s.analysisDb.begin(), s.analysisDb.end(), kFloorDb);
        std::fill (s.displayDb.begin(), s.displayDb.end(), kFloorDb);
        std::fill (s.peakDb.begin(), s.peakDb.end(), kFloorDb);
        std::fill (s.peakAge.begin(), s.peakAge.end(), 0.0f);
        s.writePos = s.sinceHop = s.filled = 0;
        s.idleSeconds = 0.0;
        s.fresh = false;
    }
    anyData = false;
    rebuildPaths();
    repaint();
}

void SpectrumAnalyzer::analyse (Stream& s)
{
    // Oldest -> newest, windowed.
    for (int i = 0; i < kFftSize; ++i)
        fftData[static_cast<size_t> (i)] = s.history[static_cast<size_t> ((s.writePos + i) & (kFftSize - 1))] * window[static_cast<size_t> (i)];
    std::fill (fftData.begin() + kFftSize, fftData.end(), 0.0f);
    fft.performFrequencyOnlyForwardTransform (fftData.data(), true);

    for (int i = 0; i < kNumPoints; ++i)
    {
        const auto& b = bands[static_cast<size_t> (i)];
        double power = 0.0;
        if (b.hi < b.lo)
        {
            const double m = fftData[static_cast<size_t> (b.lo)] + (fftData[static_cast<size_t> (b.lo + 1)] - fftData[static_cast<size_t> (b.lo)]) * b.frac;
            power = m * m;
        }
        else
        {
            for (int k = b.lo; k <= b.hi; ++k)
                power += static_cast<double> (fftData[static_cast<size_t> (k)]) * fftData[static_cast<size_t> (k)];
            power /= (b.hi - b.lo + 1);
        }
        s.analysisDb[static_cast<size_t> (i)] = juce::jmax (kFloorDb, static_cast<float> (10.0 * std::log10 (power + 1.0e-24)) + calibrationDb);
    }
}

float SpectrumAnalyzer::getBandLevelDb (bool post, double hz) const noexcept
{
    // Display points are log-spaced: the nearest one in log frequency.
    const double t = std::log (juce::jlimit (static_cast<double> (kMinHz), static_cast<double> (kMaxHz), hz) / kMinHz)
                     / std::log (static_cast<double> (kMaxHz / kMinHz));
    const auto i = static_cast<size_t> (juce::jlimit (0, kNumPoints - 1, juce::roundToInt (t * (kNumPoints - 1))));
    return streams[post ? 1 : 0].analysisDb[i];
}

void SpectrumAnalyzer::advance (double dtSeconds)
{
    const auto dt = static_cast<float> (juce::jlimit (0.0, 0.25, dtSeconds));
    const float attack = 1.0f - std::exp (-dt / kAttackSeconds);
    const float release = 1.0f - std::exp (-dt / kReleaseSeconds);
    bool moved = false;

    for (auto& s : streams)
    {
        // Run every pending hop (normally 0 or 1 per frame; bounded after stalls).
        if (s.filled >= kFftSize / 2 && s.sinceHop >= kHop)
        {
            analyse (s);
            s.sinceHop = juce::jmin (s.sinceHop - kHop, kHop - 1);
            anyData = true;
        }

        s.idleSeconds += dt;
        if (s.idleSeconds > kIdleSeconds)
            std::fill (s.analysisDb.begin(), s.analysisDb.end(), kFloorDb); // no signal: fall away

        for (size_t i = 0; i < static_cast<size_t> (kNumPoints); ++i)
        {
            auto& d = s.displayDb[i];
            const float target = s.analysisDb[i];
            const float before = d;
            d += (target - d) * (target > d ? attack : release);

            auto& peak = s.peakDb[i];
            auto& age = s.peakAge[i];
            if (d >= peak)
            {
                peak = d;
                age = 0.0f;
            }
            else
            {
                age += dt;
                if (age > kPeakHoldSeconds)
                    peak = juce::jmax (d, peak - kPeakFallDbPerSecond * dt);
            }
            moved = moved || std::abs (d - before) > 0.01f;
        }
    }

    if (moved)
    {
        rebuildPaths();
        repaint (plot.expanded (2.0f).getSmallestIntegerContainer());
    }
}

// =============================================================================
// Options
// =============================================================================
void SpectrumAnalyzer::setShowPre (bool shouldShow)
{
    showPre = shouldShow;
    rebuildPaths();
    repaint();
}

void SpectrumAnalyzer::setShowPost (bool shouldShow)
{
    showPost = shouldShow;
    rebuildPaths();
    repaint();
}

void SpectrumAnalyzer::setTiltEnabled (bool shouldTilt)
{
    tilt = shouldTilt;
    rebuildPaths();
    repaint();
}

void SpectrumAnalyzer::setPeakHoldEnabled (bool shouldHold)
{
    peakHold = shouldHold;
    rebuildPaths();
    repaint();
}

// =============================================================================
// Geometry / paths
// =============================================================================
float SpectrumAnalyzer::xForFrequency (double hz) const noexcept
{
    const double t = std::log (juce::jmax (1.0, hz) / kMinHz) / std::log (static_cast<double> (kMaxHz / kMinHz));
    return plot.getX() + static_cast<float> (t) * plot.getWidth();
}

double SpectrumAnalyzer::frequencyForX (float x) const noexcept
{
    const double t = plot.getWidth() > 0.0f ? (x - plot.getX()) / plot.getWidth() : 0.0;
    return kMinHz * std::pow (static_cast<double> (kMaxHz / kMinHz), t);
}

float SpectrumAnalyzer::yForDb (float db) const noexcept
{
    const float t = (juce::jlimit (kMinDb - 12.0f, kMaxDb + 6.0f, db) - kMinDb) / (kMaxDb - kMinDb);
    return plot.getBottom() - t * plot.getHeight();
}

void SpectrumAnalyzer::buildTrace (const Stream&, const std::vector<float>& values, juce::Path& line, juce::Path* fill) const
{
    line.clear();
    if (fill != nullptr)
        fill->clear();
    if (! anyData || plot.isEmpty())
        return;

    const float bottom = plot.getBottom();
    for (size_t i = 0; i < static_cast<size_t> (kNumPoints); ++i)
    {
        const float db = values[i] + (tilt ? tiltDb[i] : 0.0f);
        const float y = juce::jmin (bottom, yForDb (db));
        if (i == 0)
            line.startNewSubPath (pointX[i], y);
        else
            line.lineTo (pointX[i], y);
    }

    if (fill != nullptr)
    {
        *fill = line;
        fill->lineTo (pointX.back(), bottom);
        fill->lineTo (pointX.front(), bottom);
        fill->closeSubPath();
    }
}

void SpectrumAnalyzer::rebuildPaths()
{
    if (showPre)
        buildTrace (streams[0], streams[0].displayDb, preLine, &preFill);
    else
    {
        preLine.clear();
        preFill.clear();
    }

    if (showPost)
    {
        buildTrace (streams[1], streams[1].displayDb, postLine, &postFill);
        if (peakHold)
            buildTrace (streams[1], streams[1].peakDb, peakLine, nullptr);
        else
            peakLine.clear();
    }
    else
    {
        postLine.clear();
        postFill.clear();
        peakLine.clear();
    }
}

void SpectrumAnalyzer::resized()
{
    auto r = getLocalBounds().toFloat();
    plot = juce::Rectangle<float> (r.getX() + kLeftInset, r.getY() + kTopInset, r.getWidth() - kLeftInset - kRightInset,
                                   r.getHeight() - kTopInset - kBottomInset);
    pointX.resize (static_cast<size_t> (kNumPoints));
    for (size_t i = 0; i < pointX.size(); ++i)
        pointX[i] = xForFrequency (pointHz[i]);
    gridImage = {};
    rebuildPaths();
}

void SpectrumAnalyzer::moved()
{
    gridImage = {}; // the panel gradient behind it depends on the position
}

void SpectrumAnalyzer::lookAndFeelChanged()
{
    gridImage = {};
    repaint();
}

void SpectrumAnalyzer::renderGrid (float scale)
{
    const int w = juce::roundToInt (static_cast<float> (getWidth()) * scale);
    const int h = juce::roundToInt (static_cast<float> (getHeight()) * scale);
    gridImage = juce::Image (juce::Image::ARGB, juce::jmax (1, w), juce::jmax (1, h), true);
    gridScale = scale;

    juce::Graphics g (gridImage);
    g.addTransform (juce::AffineTransform::scale (scale));

    // The parent panel's fill behind the axis labels (the component is opaque), then the plot well.
    if (auto* parent = getParentComponent())
        g.setGradientFill (Theme::panelFill (getLocalArea (parent, parent->getLocalBounds()).toFloat()));
    else
        g.setColour (Palette::panel);
    g.fillAll();
    g.setColour (Palette::well);
    g.fillRoundedRectangle (plot.expanded (1.0f), 6.0f);

    // Frequency grid: faint minor lines, brighter decades, labelled majors.
    for (double decade = 10.0; decade <= 10000.0; decade *= 10.0)
    {
        for (int m = 1; m <= 9; ++m)
        {
            const double f = decade * m;
            if (f < kMinHz || f > kMaxHz)
                continue;
            const float x = std::round (xForFrequency (f)) + 0.5f;
            g.setColour (m == 1 ? Palette::gridMajor : Palette::gridMinor);
            g.drawVerticalLine (static_cast<int> (x), plot.getY(), plot.getBottom());
        }
    }

    g.setFont (Theme::font (10.5f));
    g.setColour (Palette::faint.brighter (0.25f));
    for (const double f : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
    {
        const float x = xForFrequency (f);
        const auto text = f >= 1000.0 ? juce::String (juce::roundToInt (f / 1000.0)) + "k" : juce::String (juce::roundToInt (f));
        auto labelArea = juce::Rectangle<float> (x - 22.0f, plot.getBottom() + 4.0f, 44.0f, 14.0f);
        if (f == 20.0)
            labelArea = labelArea.withX (x - 2.0f);
        else if (f == 20000.0)
            labelArea = labelArea.withRightX (x + 2.0f);
        g.drawText (text, labelArea, f == 20.0 ? juce::Justification::centredLeft
                                                : (f == 20000.0 ? juce::Justification::centredRight : juce::Justification::centred));
    }

    // Level grid (left axis): 12 dB steps, coarser when the plot is short.
    const float pixelsPer12Db = plot.getHeight() * 12.0f / (kMaxDb - kMinDb);
    const float step = pixelsPer12Db >= 16.0f ? 12.0f : (pixelsPer12Db >= 8.0f ? 24.0f : 42.0f);
    for (float db = kMaxDb; db >= kMinDb; db -= step)
    {
        const float y = std::round (yForDb (db)) + 0.5f;
        g.setColour (Palette::grid.interpolatedWith (Palette::gridMinor, 0.5f));
        g.drawHorizontalLine (static_cast<int> (y), plot.getX(), plot.getRight());
        g.setColour (Palette::faint);
        g.drawText (juce::String (juce::roundToInt (db)), juce::Rectangle<float> (plot.getX() - kLeftInset, y - 7.0f, kLeftInset - 6.0f, 14.0f),
                    juce::Justification::centredRight);
    }
}

// =============================================================================
void SpectrumAnalyzer::paint (juce::Graphics& g)
{
    const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
    if (gridImage.isNull() || std::abs (scale - gridScale) > 0.01f)
        renderGrid (scale);
    g.drawImageTransformed (gridImage, juce::AffineTransform::scale (1.0f / gridScale));

    if (! anyData)
    {
        g.setColour (Palette::faint);
        g.setFont (Theme::font (12.0f));
        g.drawText ("Waiting for audio on this strip", plot.withHeight (juce::jmin (plot.getHeight() * 0.4f, 60.0f)), juce::Justification::centred);
        return;
    }

    g.saveState();
    g.reduceClipRegion (plot.getSmallestIntegerContainer());

    const auto accent = Theme::accent (*this);

    if (! preFill.isEmpty())
    {
        g.setGradientFill (juce::ColourGradient (Palette::muted.withAlpha (0.22f), 0.0f, plot.getY(), Palette::muted.withAlpha (0.03f), 0.0f,
                                                 plot.getBottom(), false));
        g.fillPath (preFill);
        g.setColour (Palette::muted.withAlpha (0.45f));
        g.strokePath (preLine, juce::PathStrokeType (1.0f));
    }

    if (! postLine.isEmpty())
    {
        g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.20f), 0.0f, plot.getY(), accent.withAlpha (0.0f), 0.0f, plot.getBottom(),
                                                 false));
        g.fillPath (postFill);

        // Glow: a wide translucent stroke under the crisp line.
        g.setColour (accent.withAlpha (0.15f));
        g.strokePath (postLine, juce::PathStrokeType (4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (accent);
        g.strokePath (postLine, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    if (! peakLine.isEmpty())
    {
        g.setColour (accent.withAlpha (0.38f));
        g.strokePath (peakLine, juce::PathStrokeType (1.0f));
    }

    g.restoreState();
}
} // namespace flub::app::ui
