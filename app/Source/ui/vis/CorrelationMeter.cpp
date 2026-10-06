#include "CorrelationMeter.h"

#include "VisCommon.h"

#include "../Theme.h"

#include <cmath>

namespace flub::app::ui::vis
{
CorrelationMeter::CorrelationMeter()
{
    setOpaque (false);
    setInterceptsMouseClicks (false, false);
    setSampleRate (48000.0);
}

void CorrelationMeter::setSampleRate (double newSampleRate)
{
    if (newSampleRate > 0.0)
        sampleRate = newSampleRate;
    coefficient = std::exp (-1.0 / (kIntegrationSeconds * sampleRate));
}

void CorrelationMeter::reset()
{
    lr = ll = rr = 0.0;
    correlation = hold = 1.0f;
    holdAge = 0.0;
    signal = false;
    repaint();
}

void CorrelationMeter::pushPost (const float* mid, const float* side, int numSamples)
{
    const double a = coefficient, b = 1.0 - coefficient;
    for (int i = 0; i < numSamples; ++i)
    {
        const double l = static_cast<double> (mid[i]) + side[i];
        const double r = static_cast<double> (mid[i]) - side[i];
        lr = a * lr + b * l * r;
        ll = a * ll + b * l * l;
        rr = a * rr + b * r * r;
    }
}

void CorrelationMeter::advance (const FrameContext& frame)
{
    const double gate = std::pow (10.0, kGateDb / 10.0);
    signal = 0.5 * (ll + rr) > gate;
    if (signal)
    {
        const double denominator = std::sqrt (ll * rr);
        // One channel silent: a point source at that side (correlation +1 by convention).
        correlation = denominator > 1.0e-9 * (ll + rr) ? static_cast<float> (juce::jlimit (-1.0, 1.0, lr / denominator)) : 1.0f;
    }

    if (signal && correlation <= hold)
    {
        hold = correlation;
        holdAge = 0.0;
    }
    else
    {
        holdAge += frame.dtSeconds;
        if (holdAge > kHoldSeconds)
            hold = juce::jmin (signal ? correlation : 1.0f, hold + kHoldReleasePerSecond * static_cast<float> (frame.dtSeconds));
    }

    if (std::abs (correlation - painted) > 0.002f || std::abs (hold - paintedHold) > 0.002f || signal != paintedSignal)
    {
        painted = correlation;
        paintedHold = hold;
        paintedSignal = signal;
        repaint();
    }
}

void CorrelationMeter::resized()
{
    well = getLocalBounds().toFloat();
    auto r = well.reduced (10.0f, 0.0f);
    r.removeFromLeft (48.0f); // caption
    readout = r.removeFromRight (44.0f);
    r.removeFromRight (8.0f);
    bar = r.withSizeKeepingCentre (r.getWidth() - 12.0f, 8.0f).translated (0.0f, -3.0f);
}

float CorrelationMeter::xForCorrelation (float value) const noexcept
{
    return bar.getX() + (juce::jlimit (-1.0f, 1.0f, value) + 1.0f) * 0.5f * bar.getWidth();
}

void CorrelationMeter::paint (juce::Graphics& g)
{
    drawWell (g, well);
    const auto status = Theme::statusColours (*this);

    // Caption.
    Theme::drawCaption (g, "CORR", well.reduced (10.0f, 0.0f).withWidth (44.0f), Palette::muted);

    // Track with the red zone below 0 and a faint good zone towards +1.
    g.setColour (Palette::track.withAlpha (0.6f));
    g.fillRoundedRectangle (bar, 4.0f);
    const float zero = xForCorrelation (0.0f);
    g.setColour (status.hot.withAlpha (0.22f));
    g.fillRoundedRectangle (bar.withRight (zero), 4.0f);
    g.setColour (status.safe.withAlpha (0.08f));
    g.fillRoundedRectangle (bar.withLeft (xForCorrelation (0.3f)), 4.0f);

    // Ticks and labels under the bar.
    g.setFont (Theme::font (9.5f));
    for (const float v : { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f })
    {
        const float x = xForCorrelation (v);
        g.setColour (v == 0.0f ? Palette::borderStrong.brighter (0.3f) : Palette::borderStrong);
        g.fillRect (juce::Rectangle<float> (x - 0.5f, bar.getY() - 2.0f, 1.0f, bar.getHeight() + 4.0f));
        if (v == -0.5f || v == 0.5f)
            continue;
        g.setColour (labelColour());
        const auto text = v < 0.0f ? juce::String ("-1") : (v > 0.0f ? juce::String ("+1") : juce::String ("0"));
        g.drawText (text, juce::Rectangle<float> (24.0f, 10.0f).withCentre ({ x, bar.getBottom() + 8.0f }), juce::Justification::centred, false);
    }

    const auto colour = correlation < 0.0f ? status.hot : (correlation < 0.3f ? status.warn : status.safe);
    const float alpha = signal ? 1.0f : 0.35f;
    const float x = xForCorrelation (correlation);

    // Fill from 0 to the value, then the needle with a soft glow.
    g.setColour (colour.withAlpha (0.4f * alpha));
    g.fillRect (juce::Rectangle<float>::leftTopRightBottom (juce::jmin (zero, x), bar.getY() + 1.0f, juce::jmax (zero, x), bar.getBottom() - 1.0f));
    g.setColour (colour.withAlpha (0.25f * alpha));
    g.fillRoundedRectangle (juce::Rectangle<float> (8.0f, bar.getHeight() + 8.0f).withCentre ({ x, bar.getCentreY() }), 3.0f);
    g.setColour (colour.withAlpha (alpha));
    g.fillRoundedRectangle (juce::Rectangle<float> (3.0f, bar.getHeight() + 6.0f).withCentre ({ x, bar.getCentreY() }), 1.5f);

    // Hold marker: a small triangle above the bar.
    if (hold < 0.995f)
    {
        const float hx = xForCorrelation (hold);
        juce::Path tri;
        tri.addTriangle (hx - 3.5f, bar.getY() - 6.0f, hx + 3.5f, bar.getY() - 6.0f, hx, bar.getY() - 1.0f);
        g.setColour ((hold < 0.0f ? status.hot : Palette::text).withAlpha (0.85f));
        g.fillPath (tri);
    }

    // Readout.
    g.setFont (Theme::numeric (12.5f));
    g.setColour (signal ? colour : Palette::faint);
    const auto text = signal ? (correlation >= 0.0f ? "+" : "") + juce::String (correlation, 2) : juce::String ("--");
    g.drawText (text, readout, juce::Justification::centredRight, false);
}
} // namespace flub::app::ui::vis
