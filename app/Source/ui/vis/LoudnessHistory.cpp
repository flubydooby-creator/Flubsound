#include "LoudnessHistory.h"

#include "../MeterSnapshot.h"
#include "../Theme.h"

#include <cmath>

namespace flub::app::ui::vis
{
LoudnessHistory::LoudnessHistory()
{
    setOpaque (false);
    setInterceptsMouseClicks (false, false);
    // Room for every point up front: rebuilding never grows the paths' storage.
    for (auto* p : { &momentaryLine, &momentaryFill, &shortLine })
        p->preallocateSpace (3 * (kSlots + 8) * 3);
}

void LoudnessHistory::reset()
{
    history.clear();
    momentary = shortTerm = -160.0f;
    rebuildPaths();
    repaint();
}

void LoudnessHistory::advance (const FrameContext& frame)
{
    const auto finite = [] (float v) { return std::isfinite (v) ? v : -160.0f; };
    momentary = frame.meters.active ? finite (frame.meters.momentaryLufs) : -160.0f;
    shortTerm = frame.meters.active ? finite (frame.meters.shortTermLufs) : -160.0f;
    const float values[] = { momentary, shortTerm };
    history.add (values, frame.dtSeconds);
    targetLufs = frame.targetLufs;
    targetName = frame.targetName;
    if (isVisible())
    {
        rebuildPaths();
        repaint();
    }
}

void LoudnessHistory::resized()
{
    well = getLocalBounds().toFloat();
    plot = well;
    plot.removeFromLeft (36.0f);
    plot.removeFromRight (10.0f);
    plot.removeFromTop (26.0f);
    plot.removeFromBottom (20.0f);
    rebuildPaths();
}

float LoudnessHistory::yForLufs (float lufs) const noexcept
{
    const float t = (juce::jlimit (kMinLufs, kMaxLufs, lufs) - kMinLufs) / (kMaxLufs - kMinLufs);
    return plot.getBottom() - t * plot.getHeight();
}

void LoudnessHistory::rebuildPaths()
{
    momentaryLine.clear();
    momentaryFill.clear();
    shortLine.clear();
    if (plot.isEmpty())
        return;

    const float dx = plot.getWidth() / static_cast<float> (kSlots);
    const float frac = history.getSlotFraction();
    const int filled = history.getFilled();
    // age -1: the running slot at the right edge; then completed slots leftwards.
    const auto xFor = [&] (int age) { return age < 0 ? plot.getRight() : plot.getRight() - (static_cast<float> (age) + frac) * dx; };
    const auto valueAt = [&] (int age, int k) { return age < 0 ? history.getRunning (k) : history.get (age, k); };

    for (int k : { static_cast<int> (Momentary), static_cast<int> (ShortTerm) })
    {
        auto& line = k == Momentary ? momentaryLine : shortLine;
        bool drawing = false;
        float lastX = 0.0f;
        for (int age = -1; age < filled; ++age)
        {
            const float x = juce::jmax (plot.getX(), xFor (age));
            const float v = valueAt (age, k);
            const bool valid = v > kFloorLufs;
            if (valid)
            {
                const float y = yForLufs (v);
                if (! drawing)
                {
                    line.startNewSubPath (x, y);
                    if (k == Momentary)
                    {
                        momentaryFill.startNewSubPath (x, plot.getBottom());
                        momentaryFill.lineTo (x, y);
                    }
                    drawing = true;
                }
                else
                {
                    line.lineTo (x, y);
                    if (k == Momentary)
                        momentaryFill.lineTo (x, y);
                }
                lastX = x;
            }
            if ((! valid || x <= plot.getX()) && drawing)
            {
                if (k == Momentary)
                {
                    momentaryFill.lineTo (lastX, plot.getBottom());
                    momentaryFill.closeSubPath();
                }
                drawing = false;
            }
            if (x <= plot.getX())
                break;
        }
        if (drawing && k == Momentary)
        {
            momentaryFill.lineTo (lastX, plot.getBottom());
            momentaryFill.closeSubPath();
        }
    }
}

void LoudnessHistory::paint (juce::Graphics& g)
{
    drawWell (g, well);
    const auto accent = Theme::accent (*this);

    // Grid: every 6 LU, labelled; time every 10 s.
    g.setFont (labelFont());
    for (float l = -6.0f; l >= kMinLufs; l -= 6.0f)
    {
        const float y = std::round (yForLufs (l)) + 0.5f;
        g.setColour (Palette::grid.interpolatedWith (Palette::gridMinor, 0.5f));
        g.drawHorizontalLine (static_cast<int> (y), plot.getX(), plot.getRight());
        g.setColour (labelColour());
        g.drawText (juce::String (juce::roundToInt (l)), juce::Rectangle<float> (well.getX(), y - 7.0f, 30.0f, 14.0f),
                    juce::Justification::centredRight, false);
    }
    for (int s = 0; s <= static_cast<int> (kSeconds); s += 10)
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

    // Target.
    if (std::isfinite (targetLufs))
    {
        const float y = yForLufs (targetLufs);
        const float dashes[] = { 6.0f, 4.0f };
        g.setColour (Palette::amber.withAlpha (0.85f));
        g.drawDashedLine ({ plot.getX(), y, plot.getRight(), y }, dashes, 2, 1.2f);
        g.setFont (Theme::font (10.5f));
        g.drawText ((targetName.isNotEmpty() ? targetName : juce::String ("Target")) + " " + juce::String (juce::roundToInt (targetLufs)) + " LUFS",
                    juce::Rectangle<float> (plot.getX() + 6.0f, y - 15.0f, plot.getWidth() * 0.6f, 13.0f), juce::Justification::centredLeft, false);
    }

    // Traces (clipped to the plot).
    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plot.getSmallestIntegerContainer());
        g.setColour (accent.withAlpha (0.13f));
        g.fillPath (momentaryFill);
        g.setColour (accent.withAlpha (0.55f));
        g.strokePath (momentaryLine, juce::PathStrokeType (1.0f));
        g.setColour (accent.withAlpha (0.18f));
        g.strokePath (shortLine, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (accent);
        g.strokePath (shortLine, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Legend and the current readings.
    auto top = juce::Rectangle<float> (plot.getX(), well.getY() + 6.0f, plot.getWidth(), 16.0f);
    drawLegend (g, top, { { accent.withAlpha (0.6f), "Momentary" }, { accent, "Short-term" } });
    g.setFont (Theme::numeric (12.0f));
    g.setColour (Palette::text);
    g.drawText ("M " + formatValue (momentary, kFloorLufs) + "   S " + formatValue (shortTerm, kFloorLufs) + "  LUFS", top,
                juce::Justification::centredRight, false);
}
} // namespace flub::app::ui::vis
