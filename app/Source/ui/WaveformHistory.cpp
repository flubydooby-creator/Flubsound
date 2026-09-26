#include "WaveformHistory.h"

#include "Theme.h"

#include <cmath>

namespace flub::app::ui
{
namespace
{
constexpr float kLufsTop = 0.0f, kLufsBottom = -40.0f;
} // namespace

WaveformHistory::WaveformHistory()
{
    setTitle ("Output history");
    setDescription ("Scrolling waveform of the processed output with the short-term loudness trace.");
    setOpaque (false);
    columns.resize (static_cast<size_t> (kColumns));
    setSampleRate (48000.0);
}

void WaveformHistory::setSampleRate (double newSampleRate)
{
    if (newSampleRate <= 0.0)
        return;
    sampleRate = newSampleRate;
    samplesPerColumn = juce::jmax (1, static_cast<int> (std::lround (sampleRate * kSeconds / kColumns)));
}

void WaveformHistory::reset()
{
    std::fill (columns.begin(), columns.end(), Column {});
    writeIndex = samplesInColumn = 0;
    runningLo = runningHi = 0.0f;
    currentLufs = latestLufs = -160.0f;
    hasAudio = false;
    dirty = true;
    rebuildPaths();
    repaint();
}

void WaveformHistory::push (const float* samples, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
    {
        const float s = samples[i];
        runningLo = juce::jmin (runningLo, s);
        runningHi = juce::jmax (runningHi, s);
        if (++samplesInColumn >= samplesPerColumn)
        {
            auto& c = columns[static_cast<size_t> (writeIndex)];
            c.lo = runningLo;
            c.hi = runningHi;
            c.lufs = latestLufs;
            writeIndex = (writeIndex + 1) % kColumns;
            samplesInColumn = 0;
            runningLo = runningHi = 0.0f;
            dirty = true;
        }
    }
    if (numSamples > 0)
        hasAudio = true;
}

void WaveformHistory::setLoudness (float shortTermLufs)
{
    latestLufs = std::isfinite (shortTermLufs) ? shortTermLufs : -160.0f;
    if (std::abs (latestLufs - currentLufs) > 0.05f)
    {
        currentLufs = latestLufs;
        repaint (getLocalBounds().removeFromTop (30));
    }
}

void WaveformHistory::advance()
{
    if (! dirty)
        return;
    dirty = false;
    rebuildPaths();
    repaint (plot.expanded (2.0f).getSmallestIntegerContainer());
}

void WaveformHistory::resized()
{
    auto r = getLocalBounds().toFloat().reduced (12.0f, 8.0f);
    r.removeFromTop (20.0f);
    r.removeFromRight (30.0f); // LUFS axis labels
    plot = r;
    rebuildPaths();
}

void WaveformHistory::rebuildPaths()
{
    envelope.clear();
    loudnessTrace.clear();
    if (plot.isEmpty() || ! hasAudio)
        return;

    const float midY = plot.getCentreY();
    const float halfH = plot.getHeight() * 0.5f - 1.0f;
    const float dx = plot.getWidth() / static_cast<float> (kColumns - 1);

    // Oldest column at the left edge, newest at the right.
    auto columnAt = [this] (int i) -> const Column& { return columns[static_cast<size_t> ((writeIndex + i) % kColumns)]; };

    // Soft-knee display scaling so quiet passages stay visible (sqrt-like).
    auto shape = [] (float v)
    {
        const float a = juce::jmin (1.0f, std::abs (v));
        return std::copysign (std::sqrt (a), v);
    };

    envelope.startNewSubPath (plot.getX(), midY - shape (columnAt (0).hi) * halfH);
    for (int i = 1; i < kColumns; ++i)
        envelope.lineTo (plot.getX() + dx * static_cast<float> (i), midY - shape (columnAt (i).hi) * halfH);
    for (int i = kColumns - 1; i >= 0; --i)
        envelope.lineTo (plot.getX() + dx * static_cast<float> (i), midY - shape (columnAt (i).lo) * halfH);
    envelope.closeSubPath();

    bool drawing = false;
    for (int i = 0; i < kColumns; i += 2)
    {
        const float lufs = columnAt (i).lufs;
        if (lufs <= kLufsBottom - 20.0f)
        {
            drawing = false;
            continue;
        }
        const float t = (juce::jlimit (kLufsBottom, kLufsTop, lufs) - kLufsBottom) / (kLufsTop - kLufsBottom);
        const float x = plot.getX() + dx * static_cast<float> (i);
        const float y = plot.getBottom() - t * plot.getHeight();
        if (drawing)
            loudnessTrace.lineTo (x, y);
        else
            loudnessTrace.startNewSubPath (x, y);
        drawing = true;
    }
}

void WaveformHistory::paint (juce::Graphics& g)
{
    Theme::drawPanel (g, getLocalBounds().toFloat());
    const auto accent = Theme::accent (*this);

    auto header = getLocalBounds().toFloat().reduced (12.0f, 8.0f).removeFromTop (16.0f);
    const float w = Theme::drawCaption (g, "OUTPUT HISTORY", header);
    {
        auto h = header.withTrimmedLeft (w + 16.0f);
        g.setFont (Theme::font (11.0f));
        g.setColour (Palette::faint);
        g.drawText (juce::String (juce::roundToInt (kSeconds)) + " s", h.removeFromLeft (40.0f), juce::Justification::centredLeft, false);

        // Legend + current short-term value on the right.
        auto right = header.removeFromRight (220.0f);
        g.setColour (Palette::text.withAlpha (0.9f));
        g.setFont (Theme::numeric (12.0f));
        g.drawText (Theme::formatLufs (currentLufs) + " LUFS", right.removeFromRight (86.0f), juce::Justification::centredRight, false);
        g.setColour (Palette::muted);
        g.setFont (Theme::font (11.0f));
        g.drawText ("short-term", right.removeFromRight (64.0f), juce::Justification::centredRight, false);
        auto swatch = right.removeFromRight (22.0f).withSizeKeepingCentre (14.0f, 2.0f);
        g.setColour (Palette::text.withAlpha (0.8f));
        g.fillRect (swatch);
    }

    // Plot well + guides
    g.setColour (Palette::well);
    g.fillRoundedRectangle (plot.expanded (2.0f, 2.0f), 5.0f);
    g.setFont (Theme::font (9.5f));
    for (const float lufs : { -10.0f, -20.0f, -30.0f })
    {
        const float t = (lufs - kLufsBottom) / (kLufsTop - kLufsBottom);
        const float y = std::round (plot.getBottom() - t * plot.getHeight()) + 0.5f;
        g.setColour (Palette::grid);
        g.drawHorizontalLine (static_cast<int> (y), plot.getX(), plot.getRight());
        g.setColour (Palette::faint);
        g.drawText (juce::String (juce::roundToInt (lufs)), juce::Rectangle<float> (plot.getRight() + 4.0f, y - 6.0f, 26.0f, 12.0f),
                    juce::Justification::centredLeft, false);
    }

    if (envelope.isEmpty())
    {
        g.setColour (Palette::faint);
        g.setFont (Theme::font (11.5f));
        g.drawText ("No output yet", plot, juce::Justification::centred, false);
        return;
    }

    g.saveState();
    g.reduceClipRegion (plot.getSmallestIntegerContainer());
    // Brighter at the peaks, darker around the centre line.
    auto gradient = juce::ColourGradient (accent.withAlpha (0.85f), 0.0f, plot.getY(), accent.withAlpha (0.85f), 0.0f, plot.getBottom(), false);
    gradient.addColour (0.5, accent.withAlpha (0.35f));
    g.setGradientFill (gradient);
    g.fillPath (envelope);

    // Fade the oldest part so the history "scrolls in" from the left.
    g.setGradientFill (juce::ColourGradient (Palette::well, plot.getX(), 0.0f, Palette::well.withAlpha (0.0f), plot.getX() + plot.getWidth() * 0.12f,
                                             0.0f, false));
    g.fillRect (plot.withWidth (plot.getWidth() * 0.12f));

    if (! loudnessTrace.isEmpty())
    {
        g.setColour (Palette::background.withAlpha (0.6f));
        g.strokePath (loudnessTrace, juce::PathStrokeType (3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (Palette::text.withAlpha (0.9f));
        g.strokePath (loudnessTrace, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    g.restoreState();
}
} // namespace flub::app::ui
