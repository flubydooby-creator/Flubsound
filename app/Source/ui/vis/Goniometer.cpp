#include "Goniometer.h"

#include "../MeterSnapshot.h"
#include "../Theme.h"

#include <cmath>

namespace flub::app::ui::vis
{
Goniometer::Goniometer()
{
    mids.assign (static_cast<size_t> (kMaxSamplesPerFrame), 0.0f);
    sides.assign (static_cast<size_t> (kMaxSamplesPerFrame), 0.0f);
    setOpaque (false);
    setInterceptsMouseClicks (false, false);
}

juce::Point<float> Goniometer::scopePoint (float mid, float side, float g) noexcept
{
    return { -side * g, mid * g };
}

void Goniometer::setSampleRate (double newSampleRate)
{
    if (newSampleRate > 0.0)
        sampleRate = newSampleRate;
}

void Goniometer::reset()
{
    count = writePos = 0;
    phosphor.clear();
    phosphor.render();
    peak = 0.0f;
    gain = 1.0f;
    lit = false;
    repaint();
}

void Goniometer::pushPost (const float* mid, const float* side, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
    {
        mids[static_cast<size_t> (writePos)] = mid[i];
        sides[static_cast<size_t> (writePos)] = side[i];
        writePos = (writePos + 1) % kMaxSamplesPerFrame;
    }
    count = juce::jmin (kMaxSamplesPerFrame, count + numSamples);
}

void Goniometer::advance (const FrameContext& frame)
{
    const auto dt = static_cast<float> (juce::jmax (1.0e-4, frame.dtSeconds));
    correlation = frame.meters.correlation;
    active = frame.meters.active;

    // Automatic gain: peak of |mid|, |side| with a slow release; the gain
    // falls at once (nothing leaves the scope) and rises gently.
    float framePeak = 0.0f;
    for (int k = 0; k < count; ++k)
    {
        const int i = (writePos - count + k + kMaxSamplesPerFrame) % kMaxSamplesPerFrame;
        framePeak = juce::jmax (framePeak, std::abs (mids[static_cast<size_t> (i)]), std::abs (sides[static_cast<size_t> (i)]));
    }
    peak = framePeak >= peak ? framePeak : juce::jmax (framePeak, peak * std::exp (-dt / kGainReleaseSeconds));
    const float maxGain = std::pow (10.0f, kMaxGainDb / 20.0f);
    const float target = peak > 1.0e-6f ? juce::jlimit (1.0f, maxGain, kTargetPeak / peak) : gain;
    gain = target < gain ? target : gain * std::pow (target / gain, juce::jmin (1.0f, dt * 4.0f));

    const bool hadLight = lit;
    phosphor.fade (std::exp (-dt / kAfterglowSeconds));
    const float half = 0.5f * static_cast<float> (kCells - 1);
    const float amount = 0.05f * static_cast<float> (48000.0 / sampleRate);
    for (int k = 0; k < count; ++k)
    {
        const int i = (writePos - count + k + kMaxSamplesPerFrame) % kMaxSamplesPerFrame;
        const auto p = scopePoint (mids[static_cast<size_t> (i)], sides[static_cast<size_t> (i)], gain);
        phosphor.splat (half + p.x * half, half - p.y * half, amount);
    }
    lit = count > 0 || (hadLight && peak > 1.0e-6f);
    count = 0;
    if (lit || hadLight)
    {
        phosphor.render();
        repaint (scope.getSmallestIntegerContainer().expanded (2));
        repaint (well.withTop (well.getBottom() - 22.0f).getSmallestIntegerContainer());
    }
}

void Goniometer::resized()
{
    well = getLocalBounds().toFloat();
    const float side = juce::jmax (10.0f, juce::jmin (well.getWidth(), well.getHeight()) - 44.0f);
    scope = well.withSizeKeepingCentre (side, side).translated (0.0f, -4.0f);
}

void Goniometer::lookAndFeelChanged()
{
    phosphor.setColour (Theme::accent (*this), Palette::text);
    phosphor.render();
    repaint();
}

void Goniometer::paint (juce::Graphics& g)
{
    drawWell (g, well);
    const auto c = scope.getCentre();
    const float r = scope.getWidth() * 0.5f;

    // Grid: full-scale circle, half circle, the M / S axes and the L / R diagonals.
    g.setColour (Palette::gridMajor);
    g.drawEllipse (scope, 1.0f);
    g.setColour (Palette::gridMinor.brighter (0.15f));
    g.drawEllipse (scope.reduced (r * 0.5f), 1.0f);
    g.setColour (Palette::gridMajor);
    g.drawLine (c.x, scope.getY(), c.x, scope.getBottom(), 1.0f);
    g.drawLine (scope.getX(), c.y, scope.getRight(), c.y, 1.0f);
    const float d = r * 0.7071f;
    const float dashes[] = { 3.0f, 4.0f };
    g.setColour (Palette::grid.brighter (0.2f));
    g.drawDashedLine ({ c.x - d, c.y - d, c.x + d, c.y + d }, dashes, 2, 1.0f);
    g.drawDashedLine ({ c.x + d, c.y - d, c.x - d, c.y + d }, dashes, 2, 1.0f);

    {
        juce::Graphics::ScopedSaveState state (g);
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        const float s = scope.getWidth() / static_cast<float> (kCells);
        g.drawImageTransformed (phosphor.getImage(), juce::AffineTransform::scale (s).translated (scope.getX(), scope.getY()));
    }

    // Axis labels just outside the circle.
    g.setFont (Theme::font (11.0f, true));
    g.setColour (Palette::muted);
    const auto label = [&g] (const char* text, juce::Point<float> at)
    { g.drawText (text, juce::Rectangle<float> (16.0f, 14.0f).withCentre (at), juce::Justification::centred, false); };
    label ("M", { c.x, scope.getY() - 9.0f });
    label ("L", { c.x - d - 9.0f, c.y - d - 9.0f });
    label ("R", { c.x + d + 9.0f, c.y - d - 9.0f });
    label ("S", { scope.getX() - 10.0f, c.y });
    label ("S", { scope.getRight() + 10.0f, c.y });

    // Readouts: the automatic gain and the correlation.
    auto bottom = well.reduced (10.0f, 6.0f).removeFromBottom (14.0f);
    g.setFont (labelFont());
    g.setColour (labelColour());
    const float gainDb = 20.0f * std::log10 (gain);
    const int shownDb = juce::roundToInt (gainDb);
    g.drawText ("Auto gain " + juce::String (shownDb >= 0 ? "+" : "") + juce::String (shownDb) + " dB", bottom, juce::Justification::centredLeft, false);
    g.drawText (active ? "Corr " + juce::String (correlation >= 0.0f ? "+" : "") + juce::String (correlation, 2) : juce::String ("No signal"), bottom,
                juce::Justification::centredRight, false);
}
} // namespace flub::app::ui::vis
