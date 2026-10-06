#include "GainReductionTrace.h"

#include "../MeterSnapshot.h"
#include "../Theme.h"

#include <cmath>

namespace flub::app::ui::vis
{
GainReductionTrace::GainReductionTrace()
{
    setOpaque (false);
    setInterceptsMouseClicks (false, false);
    for (auto& p : lines)
        p.preallocateSpace (3 * (kSlots + 8) * 2);
    limiterFill.preallocateSpace (3 * (kSlots + 8) * 3);
}

const char* GainReductionTrace::stageName (int stage) noexcept
{
    switch (stage)
    {
        case Compressor: return "Comp";
        case Limiter: return "Limiter";
        case Glue: return "Glue";
        case BassProtect: return "Bass";
        case Master: return "Master";
        default: return "";
    }
}

juce::Colour GainReductionTrace::stageColour (int stage) const
{
    const auto status = Theme::statusColours (*this);
    switch (stage)
    {
        case Compressor: return Theme::accent (*this);
        case Limiter: return status.hot;
        case Glue: return status.warn;
        case BassProtect: return Palette::eqBands[6];
        default: return Palette::text.withAlpha (0.85f);
    }
}

void GainReductionTrace::reset()
{
    history.clear();
    current.fill (0.0f);
    rangeDb = 12.0f;
    rebuildPaths();
    repaint();
}

void GainReductionTrace::advance (const FrameContext& frame)
{
    const auto& m = frame.meters;
    const auto gr = [&m] (float v) { return m.active && std::isfinite (v) ? juce::jlimit (-60.0f, 0.0f, v) : 0.0f; };
    current = { gr (m.compGainReductionDb), gr (m.maxGainReductionDb), gr (m.glueGainReductionDb), gr (m.bassProtectionDb),
                gr (m.masterGainReductionDb) };
    history.add (current.data(), frame.dtSeconds);

    float deepest = 0.0f;
    for (int age = 0; age < history.getFilled(); ++age)
        for (int k = 0; k < kNumStages; ++k)
            deepest = juce::jmin (deepest, history.get (age, k));
    rangeDb = deepest < -11.0f ? 24.0f : 12.0f;

    if (isVisible())
    {
        rebuildPaths();
        repaint();
    }
}

void GainReductionTrace::resized()
{
    well = getLocalBounds().toFloat();
    plot = well;
    plot.removeFromLeft (36.0f);
    plot.removeFromRight (10.0f);
    plot.removeFromTop (28.0f);
    plot.removeFromBottom (20.0f);
    rebuildPaths();
}

float GainReductionTrace::yForDb (float db) const noexcept
{
    return plot.getY() + juce::jlimit (0.0f, 1.0f, -db / rangeDb) * plot.getHeight();
}

void GainReductionTrace::rebuildPaths()
{
    for (auto& p : lines)
        p.clear();
    limiterFill.clear();
    if (plot.isEmpty())
        return;

    const float dx = plot.getWidth() / static_cast<float> (kSlots);
    const float frac = history.getSlotFraction();
    const int filled = history.getFilled();
    const auto xFor = [&] (int age) { return age < 0 ? plot.getRight() : plot.getRight() - (static_cast<float> (age) + frac) * dx; };
    const auto valueAt = [this] (int age, int k) { return age < 0 ? history.getRunning (k) : history.get (age, k); };

    for (int k = 0; k < kNumStages; ++k)
    {
        auto& line = lines[static_cast<size_t> (k)];
        const bool fill = k == Limiter;
        bool drawing = false;
        float lastX = 0.0f;
        for (int age = -1; age < filled; ++age)
        {
            const float x = juce::jmax (plot.getX(), xFor (age));
            const float v = age < 0 && filled == 0 ? 0.0f : valueAt (age, k);
            const bool visible = v < kVisibleDb;
            if (visible)
            {
                const float y = yForDb (v);
                if (! drawing)
                {
                    // Start from the 0 dB line so a reduction rises out of it.
                    line.startNewSubPath (x, plot.getY());
                    if (fill)
                        limiterFill.startNewSubPath (x, plot.getY());
                    drawing = true;
                }
                line.lineTo (x, y);
                if (fill)
                    limiterFill.lineTo (x, y);
                lastX = x;
            }
            if (drawing && (! visible || x <= plot.getX()))
            {
                line.lineTo (lastX, plot.getY());
                if (fill)
                {
                    limiterFill.lineTo (lastX, plot.getY());
                    limiterFill.closeSubPath();
                }
                drawing = false;
            }
            if (x <= plot.getX())
                break;
        }
        if (drawing && fill)
        {
            limiterFill.lineTo (lastX, plot.getY());
            limiterFill.closeSubPath();
        }
    }
}

void GainReductionTrace::paint (juce::Graphics& g)
{
    drawWell (g, well);

    // Grid: every 3 dB (6 dB on the deeper scale), labelled.
    g.setFont (labelFont());
    const float step = rangeDb > 12.0f ? 6.0f : 3.0f;
    for (float db = 0.0f; db >= -rangeDb; db -= step)
    {
        const float y = std::round (yForDb (db)) + 0.5f;
        g.setColour (db == 0.0f ? Palette::gridMajor : Palette::grid.interpolatedWith (Palette::gridMinor, 0.5f));
        g.drawHorizontalLine (static_cast<int> (y), plot.getX(), plot.getRight());
        g.setColour (labelColour());
        g.drawText (juce::String (juce::roundToInt (db)), juce::Rectangle<float> (well.getX(), y - 7.0f, 30.0f, 14.0f),
                    juce::Justification::centredRight, false);
    }
    for (int s = 0; s <= static_cast<int> (kSeconds); s += 5)
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
        g.reduceClipRegion (plot.expanded (0.0f, 2.0f).getSmallestIntegerContainer());
        g.setColour (stageColour (Limiter).withAlpha (0.16f));
        g.fillPath (limiterFill);
        // The master and bass first, the compressor and limiter on top.
        for (const int k : { static_cast<int> (Master), static_cast<int> (BassProtect), static_cast<int> (Glue), static_cast<int> (Compressor),
                             static_cast<int> (Limiter) })
        {
            g.setColour (stageColour (k));
            g.strokePath (lines[static_cast<size_t> (k)], juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }

    // Legend with the current values.
    auto legend = juce::Rectangle<float> (plot.getX(), well.getY() + 6.0f, plot.getWidth(), 16.0f);
    const auto font = Theme::font (11.0f);
    g.setFont (font);
    for (int k = 0; k < kNumStages; ++k)
    {
        const auto text = juce::String (stageName (k)) + " " + juce::String (current[static_cast<size_t> (k)], 1);
        const float w = juce::GlyphArrangement::getStringWidth (font, text);
        if (14.0f + w + 12.0f > legend.getWidth())
            break;
        g.setColour (stageColour (k));
        g.fillRoundedRectangle (legend.removeFromLeft (14.0f).withSizeKeepingCentre (12.0f, 3.0f), 1.5f);
        legend.removeFromLeft (4.0f);
        g.setColour (current[static_cast<size_t> (k)] < kVisibleDb ? Palette::text : Palette::muted);
        g.drawText (text, legend.removeFromLeft (w + 2.0f), juce::Justification::centredLeft, false);
        legend.removeFromLeft (12.0f);
    }
}
} // namespace flub::app::ui::vis
