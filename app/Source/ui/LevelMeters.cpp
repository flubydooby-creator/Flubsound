#include "LevelMeters.h"

#include "Theme.h"

#include <cmath>

namespace flub::app::ui
{
namespace
{
constexpr float kPeakFallDbPerSecond = 24.0f;
constexpr float kHoldSeconds = 1.5f;
constexpr float kHoldFallDbPerSecond = 20.0f;
constexpr float kRmsSeconds = 0.06f;
} // namespace

LevelMeters::LevelMeters()
{
    setTitle ("Level meters");
    setDescription ("Input and output peak / RMS levels with peak hold and clip indicators, and the true-peak maximum.");
    setOpaque (false);
}

float LevelMeters::deflection (float db) noexcept
{
    float d;
    if (db < -70.0f)
        d = 0.0f;
    else if (db < -60.0f)
        d = (db + 70.0f) * 0.25f;
    else if (db < -50.0f)
        d = (db + 60.0f) * 0.5f + 2.5f;
    else if (db < -40.0f)
        d = (db + 50.0f) * 0.75f + 7.5f;
    else if (db < -30.0f)
        d = (db + 40.0f) * 1.5f + 15.0f;
    else if (db < -20.0f)
        d = (db + 30.0f) * 2.0f + 30.0f;
    else if (db < 0.0f)
        d = (db + 20.0f) * 2.5f + 50.0f;
    else
        d = 100.0f;
    return d * 0.01f;
}

void LevelMeters::updateChannel (Channel& c, float peakDb, float rmsDb, float dt) noexcept
{
    if (! std::isfinite (peakDb))
        peakDb = -100.0f;
    if (! std::isfinite (rmsDb))
        rmsDb = -100.0f;

    c.peak = juce::jmax (peakDb, c.peak - kPeakFallDbPerSecond * dt);
    c.rms += (rmsDb - c.rms) * (1.0f - std::exp (-dt / kRmsSeconds));
    c.rms = juce::jmin (c.rms, c.peak);

    if (c.peak >= c.hold)
    {
        c.hold = c.peak;
        c.holdAge = 0.0f;
    }
    else
    {
        c.holdAge += dt;
        if (c.holdAge > kHoldSeconds)
            c.hold = juce::jmax (c.peak, c.hold - kHoldFallDbPerSecond * dt);
    }
}

void LevelMeters::update (const MeterSnapshot& s, double dtSeconds)
{
    const auto dt = static_cast<float> (juce::jlimit (0.0, 0.25, dtSeconds));
    const float silent = -100.0f;
    for (size_t c = 0; c < 2; ++c)
    {
        updateChannel (in[c], s.active ? s.inPeakDb[c] : silent, s.active ? s.inRmsDb[c] : silent, dt);
        updateChannel (out[c], s.active ? s.outPeakDb[c] : silent, s.active ? s.outRmsDb[c] : silent, dt);
        if (s.active && s.inPeakDb[c] > -0.05f)
            inClip = true;
        if (s.active && s.outPeakDb[c] > -0.05f)
            outClip = true;
    }
    truePeakNow = s.outTruePeakDb;
    if (s.active && truePeakNow > 0.0f)
        outClip = true;

    const float newMax = s.outTruePeakMaxDb;
    const bool changed = std::abs (newMax - truePeakMax) > 0.05f || active != s.active;
    truePeakMax = newMax;
    active = s.active;

    if (changed || s.active || in[0].hold > -99.0f || out[0].hold > -99.0f)
        repaint();
}

void LevelMeters::reset()
{
    for (auto* pair : { &in, &out })
        for (auto& c : *pair)
            c = {};
    inClip = outClip = false;
    truePeakMax = -160.0f;
    repaint();
}

void LevelMeters::resized()
{
    auto r = getLocalBounds().toFloat().reduced (14.0f, 12.0f);
    r.removeFromTop (22.0f); // caption

    auto bars = r.removeFromLeft (juce::jmin (112.0f, r.getWidth() * 0.5f));
    barsArea = bars;
    bars.removeFromLeft (24.0f); // scale labels
    const float pairWidth = (bars.getWidth() - 12.0f) * 0.5f;
    inArea = bars.removeFromLeft (pairWidth);
    bars.removeFromLeft (12.0f);
    outArea = bars.removeFromLeft (pairWidth);
    inClipArea = inArea.withHeight (6.0f);
    outClipArea = outArea.withHeight (6.0f);

    r.removeFromLeft (14.0f);
    truePeakArea = r;
}

void LevelMeters::drawPair (juce::Graphics& g, juce::Rectangle<float> area, const Channel& l, const Channel& r, bool clipped,
                            const juce::String& label)
{
    const auto colours = Theme::meterColours (*this);

    // Clip LED
    auto led = area.removeFromTop (6.0f);
    g.setColour (clipped ? colours.hot : Palette::well);
    g.fillRoundedRectangle (led, 2.0f);
    if (! clipped)
    {
        g.setColour (Palette::border);
        g.drawRoundedRectangle (led.reduced (0.5f), 2.0f, 1.0f);
    }
    area.removeFromTop (4.0f);

    auto labelArea = area.removeFromBottom (16.0f);
    area.removeFromBottom (2.0f);

    const float barWidth = (area.getWidth() - 3.0f) * 0.5f;
    const float bottom = area.getBottom(), height = area.getHeight();
    auto gradient = juce::ColourGradient (colours.safe, 0.0f, bottom, colours.hot, 0.0f, area.getY(), false);
    gradient.clearColours();
    gradient.addColour (0.0, colours.safe);
    gradient.addColour (deflection (-14.0f), colours.safe);
    gradient.addColour (deflection (-11.0f), colours.warn);
    gradient.addColour (deflection (-4.0f), colours.warn);
    gradient.addColour (deflection (-2.5f), colours.hot);
    gradient.addColour (1.0, colours.hot);

    float x = area.getX();
    for (const auto* c : { &l, &r })
    {
        const auto bar = juce::Rectangle<float> (x, area.getY(), barWidth, height);
        g.setColour (Palette::well);
        g.fillRoundedRectangle (bar, 2.5f);

        const float peakTop = bottom - deflection (c->peak) * height;
        const float rmsTop = bottom - deflection (c->rms) * height;
        g.setGradientFill (gradient);
        g.setOpacity (0.35f);
        g.fillRect (bar.withTop (peakTop));
        g.setOpacity (1.0f);
        g.fillRect (bar.withTop (rmsTop));

        if (c->hold > -69.0f)
        {
            const float holdY = bottom - deflection (c->hold) * height;
            g.setColour (Theme::meterColourForDb (colours, c->hold).brighter (0.2f));
            g.fillRect (bar.getX(), holdY - 1.0f, bar.getWidth(), 2.0f);
        }
        x += barWidth + 3.0f;
    }

    g.setColour (Palette::muted);
    g.setFont (Theme::caption (10.0f));
    g.drawText (label, labelArea, juce::Justification::centred, false);
}

void LevelMeters::paint (juce::Graphics& g)
{
    Theme::drawPanel (g, getLocalBounds().toFloat());
    auto header = getLocalBounds().toFloat().reduced (14.0f, 12.0f).removeFromTop (18.0f);
    Theme::drawCaption (g, "LEVELS", header);
    if (! active)
    {
        g.setColour (Palette::faint);
        g.setFont (Theme::font (11.0f));
        g.drawText ("no signal", header, juce::Justification::centredRight, false);
    }

    // ---- dB scale (aligned with the bar bodies) ----
    const auto body = inArea.withTrimmedTop (10.0f).withTrimmedBottom (18.0f);
    g.setFont (Theme::font (9.5f));
    for (const float db : { 0.0f, -3.0f, -6.0f, -12.0f, -18.0f, -24.0f, -36.0f, -48.0f, -60.0f })
    {
        const float y = body.getBottom() - deflection (db) * body.getHeight();
        g.setColour (Palette::faint);
        g.drawText (juce::String (juce::roundToInt (db)), juce::Rectangle<float> (barsArea.getX(), y - 6.0f, 20.0f, 12.0f),
                    juce::Justification::centredRight, false);
        g.setColour (Palette::grid);
        g.drawHorizontalLine (juce::roundToInt (y), barsArea.getX() + 22.0f, outArea.getRight());
    }

    drawPair (g, inArea, in[0], in[1], inClip, "IN");
    drawPair (g, outArea, out[0], out[1], outClip, "OUT");

    // ---- True peak + numeric readouts ----
    auto r = truePeakArea;
    const auto colours = Theme::meterColours (*this);
    Theme::drawCaption (g, "TRUE PEAK", r.removeFromTop (14.0f), Palette::muted);
    r.removeFromTop (2.0f);
    {
        auto valueRow = r.removeFromTop (30.0f);
        const bool over = truePeakMax > -1.0f;
        g.setColour (truePeakMax <= -99.0f ? Palette::faint : (over ? colours.hot : Palette::text));
        g.setFont (Theme::numeric (24.0f));
        const auto text = Theme::formatDb (truePeakMax, 1);
        const float tw = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text);
        g.drawText (text, valueRow.removeFromLeft (tw + 4.0f), juce::Justification::centredLeft, false);
        g.setColour (Palette::muted);
        g.setFont (Theme::font (11.0f));
        g.drawText ("dBTP", valueRow.withTrimmedTop (8.0f), juce::Justification::centredLeft, false);
    }
    g.setColour (Palette::faint);
    g.setFont (Theme::font (10.5f));
    g.drawText ("max hold - click to reset", r.removeFromTop (14.0f), juce::Justification::centredLeft, true);
    r.removeFromTop (10.0f);

    auto readout = [&] (const juce::String& name, float left, float right)
    {
        if (r.getHeight() < 30.0f)
            return;
        Theme::drawCaption (g, name, r.removeFromTop (14.0f), Palette::faint);
        auto row = r.removeFromTop (18.0f);
        g.setFont (Theme::numeric (13.0f, false));
        const float half = row.getWidth() * 0.5f;
        const float values[] = { left, right };
        for (int i = 0; i < 2; ++i)
        {
            const float v = values[i];
            const auto cell = row.removeFromLeft (half);
            g.setColour (v <= -99.0f ? Palette::faint : Palette::text.withAlpha (0.9f));
            g.drawText (Theme::formatDb (v, 1), cell, juce::Justification::centredLeft, false);
        }
        r.removeFromTop (6.0f);
    };
    readout ("OUT PEAK  L / R", out[0].hold, out[1].hold);
    readout ("OUT RMS  L / R", out[0].rms, out[1].rms);
    readout ("IN PEAK  L / R", in[0].hold, in[1].hold);
}

void LevelMeters::mouseMove (const juce::MouseEvent& e)
{
    juce::String tip;
    if (truePeakArea.contains (e.position))
        tip = "Click to reset the true-peak hold and the integrated loudness";
    else if (inArea.contains (e.position) || outArea.contains (e.position))
        tip = "Bars: RMS (solid) and peak (translucent) with peak hold. Click to clear the clip indicators.";
    if (tip != lastTip)
    {
        lastTip = tip;
        setTooltip (tip);
    }
}

void LevelMeters::mouseUp (const juce::MouseEvent& e)
{
    if (truePeakArea.contains (e.position))
    {
        truePeakMax = -160.0f;
        for (auto* pair : { &in, &out })
            for (auto& c : *pair)
                c.hold = c.peak;
        inClip = outClip = false;
        if (onResetRequested != nullptr)
            onResetRequested();
        repaint();
    }
    else if (barsArea.contains (e.position))
    {
        inClip = outClip = false;
        repaint();
    }
}
} // namespace flub::app::ui
