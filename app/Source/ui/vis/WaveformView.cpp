#include "WaveformView.h"

#include "VisCommon.h"

#include "../Theme.h"

#include <cmath>

namespace flub::app::ui::vis
{
WaveformView::WaveformView()
{
    for (auto& s : streams)
        s.columns.assign (static_cast<size_t> (kColumns), Column {});
    for (auto* p : { &preShape, &postShape })
        p->preallocateSpace (3 * (2 * kColumns + 8));
    setOpaque (false);
    setInterceptsMouseClicks (false, false);
    setSampleRate (48000.0);
}

void WaveformView::setSampleRate (double sampleRate)
{
    if (sampleRate <= 0.0)
        return;
    const int spc = juce::jmax (1, static_cast<int> (std::lround (sampleRate * kSeconds / kColumns)));
    if (spc != samplesPerColumn)
    {
        samplesPerColumn = spc;
        reset();
    }
}

void WaveformView::reset()
{
    for (auto& s : streams)
    {
        std::fill (s.columns.begin(), s.columns.end(), Column {});
        s.running = {};
        s.count = s.filled = 0;
        s.newest = -1;
        s.changed = true;
    }
    rebuildPaths();
    repaint();
}

void WaveformView::push (Stream& s, const float* samples, int numSamples) noexcept
{
    for (int i = 0; i < numSamples; ++i)
    {
        const float x = std::isfinite (samples[i]) ? samples[i] : 0.0f;
        if (s.count == 0)
            s.running = { x, x };
        else
        {
            s.running.lo = juce::jmin (s.running.lo, x);
            s.running.hi = juce::jmax (s.running.hi, x);
        }
        if (++s.count >= samplesPerColumn)
        {
            s.newest = (s.newest + 1) % kColumns;
            s.columns[static_cast<size_t> (s.newest)] = s.running;
            s.filled = juce::jmin (kColumns, s.filled + 1);
            s.count = 0;
            s.changed = true;
        }
    }
}

void WaveformView::pushPre (const float* mid, int numSamples)
{
    push (streams[0], mid, numSamples);
}

void WaveformView::pushPost (const float* mid, const float*, int numSamples)
{
    push (streams[1], mid, numSamples);
}

const WaveformView::Column& WaveformView::getColumn (bool post, int age) const noexcept
{
    static const Column empty {};
    const auto& s = streams[post ? 1 : 0];
    if (age < 0 || age >= s.filled)
        return empty;
    return s.columns[static_cast<size_t> ((s.newest - age + kColumns) % kColumns)];
}

void WaveformView::advance (const FrameContext&)
{
    if (! streams[0].changed && ! streams[1].changed)
        return;
    streams[0].changed = streams[1].changed = false;
    if (isVisible())
    {
        rebuildPaths();
        repaint();
    }
}

void WaveformView::resized()
{
    well = getLocalBounds().toFloat();
    plot = well;
    plot.removeFromLeft (36.0f);
    plot.removeFromRight (10.0f);
    plot.removeFromTop (26.0f);
    plot.removeFromBottom (20.0f);
    rebuildPaths();
}

float WaveformView::yFor (float amplitude) const noexcept
{
    return plot.getCentreY() - juce::jlimit (-1.0f, 1.0f, amplitude) * plot.getHeight() * 0.5f;
}

void WaveformView::buildShape (const Stream& s, juce::Path& shape) const
{
    shape.clear();
    if (s.filled < 2 || plot.isEmpty())
        return;
    const float dx = plot.getWidth() / static_cast<float> (kColumns - 1);
    const int n = s.filled;
    const auto column = [&s] (int age) { return s.columns[static_cast<size_t> ((s.newest - age + kColumns) % kColumns)]; };
    // Top edge right to left (newest at the right), then the bottom edge back.
    for (int age = 0; age < n; ++age)
    {
        const float x = plot.getRight() - static_cast<float> (age) * dx;
        const float y = yFor (column (age).hi);
        if (age == 0)
            shape.startNewSubPath (x, y);
        else
            shape.lineTo (x, y);
    }
    for (int age = n - 1; age >= 0; --age)
        shape.lineTo (plot.getRight() - static_cast<float> (age) * dx, yFor (column (age).lo));
    shape.closeSubPath();
}

void WaveformView::rebuildPaths()
{
    buildShape (streams[0], preShape);
    buildShape (streams[1], postShape);
}

void WaveformView::paint (juce::Graphics& g)
{
    drawWell (g, well);
    const auto accent = Theme::accent (*this);

    // Amplitude grid: 0 dBFS (+-1), -6 dB (+-0.5), the centre line.
    g.setFont (labelFont());
    for (const float a : { 1.0f, 0.5f, 0.0f, -0.5f, -1.0f })
    {
        const float y = std::round (yFor (a)) + 0.5f;
        g.setColour (a == 0.0f ? Palette::gridMajor : Palette::grid.interpolatedWith (Palette::gridMinor, 0.5f));
        g.drawHorizontalLine (static_cast<int> (y), plot.getX(), plot.getRight());
        if (a <= 0.0f)
            continue;
        g.setColour (labelColour());
        const auto text = a == 1.0f ? juce::String ("0 dB") : juce::String ("-6");
        g.drawText (text, juce::Rectangle<float> (well.getX(), y - 7.0f, 32.0f, 14.0f), juce::Justification::centredRight, false);
    }
    for (int s = 0; s <= static_cast<int> (kSeconds); ++s)
    {
        const float x = plot.getRight() - static_cast<float> (s / kSeconds) * plot.getWidth();
        g.setColour (Palette::gridMinor);
        g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
        g.setColour (labelColour());
        const auto text = s == 0 ? juce::String ("now") : "-" + juce::String (s) + " s";
        auto area = juce::Rectangle<float> (x - 24.0f, plot.getBottom() + 3.0f, 48.0f, 14.0f);
        auto just = juce::Justification::centred;
        if (s == 0)
        {
            area = area.withRightX (x + 2.0f);
            just = juce::Justification::centredRight;
        }
        else if (s == static_cast<int> (kSeconds))
        {
            area = area.withX (x - 2.0f);
            just = juce::Justification::centredLeft;
        }
        g.drawText (text, area, just, false);
    }

    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plot.getSmallestIntegerContainer());
        g.setColour (Palette::muted.withAlpha (0.38f));
        g.fillPath (preShape);
        g.setColour (accent.withAlpha (0.30f));
        g.fillPath (postShape);
        g.setColour (accent.withAlpha (0.9f));
        g.strokePath (postShape, juce::PathStrokeType (1.0f));
        // The input's outline on top, so it stays visible where the output covers it.
        g.setColour (Palette::text.withAlpha (0.5f));
        g.strokePath (preShape, juce::PathStrokeType (0.8f));
    }

    drawLegend (g, { plot.getX(), well.getY() + 6.0f, plot.getWidth(), 16.0f },
                { { Palette::muted, "Input (before)" }, { accent, "Output (after)" } });
}
} // namespace flub::app::ui::vis
