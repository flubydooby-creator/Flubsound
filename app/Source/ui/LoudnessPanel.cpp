#include "LoudnessPanel.h"

#include "Theme.h"

#include <cmath>

namespace flub::app::ui
{
namespace
{
constexpr float kGrReleaseDbPerSecond = 18.0f;

float finiteOr (float v, float fallback)
{
    return std::isfinite (v) ? v : fallback;
}

bool differs (float a, float b, float tolerance)
{
    return std::abs (a - b) > tolerance;
}
} // namespace

LoudnessPanel::LoudnessPanel()
{
    setTitle ("Loudness and dynamics");
    setDescription ("LUFS momentary, short-term and integrated loudness, loudness range, gain reduction and stereo correlation.");
    setOpaque (false);
}

void LoudnessPanel::update (const MeterSnapshot& s, double dtSeconds)
{
    const auto dt = static_cast<float> (juce::jlimit (0.0, 0.25, dtSeconds));
    const float release = kGrReleaseDbPerSecond * dt;
    auto followGr = [release] (float& value, float target)
    {
        // Reductions are <= 0: jump down instantly, recover at a fixed rate.
        target = juce::jmin (0.0f, finiteOr (target, 0.0f));
        value = juce::jmin (target, value + release);
    };

    shown.active = s.active;
    shown.momentary = finiteOr (s.momentaryLufs, -160.0f);
    shown.shortTerm = finiteOr (s.shortTermLufs, -160.0f);
    shown.integrated = finiteOr (s.integratedLufs, -160.0f);
    shown.range = finiteOr (s.loudnessRangeLu, 0.0f);
    shown.truePeakMax = finiteOr (s.outTruePeakMaxDb, -160.0f);
    shown.autoLevel = finiteOr (s.autoLevelGainDb, 0.0f);

    followGr (shown.comp, s.active ? s.compGainReductionDb : 0.0f);
    followGr (shown.limiter, s.active ? s.maxGainReductionDb : 0.0f);
    followGr (shown.glue, s.active ? s.glueGainReductionDb : 0.0f);
    followGr (shown.bass, s.active ? s.bassProtectionDb : 0.0f);
    followGr (shown.master, s.active ? s.masterGainReductionDb : 0.0f);
    shown.compUp = juce::jmax (s.active ? finiteOr (s.compUpwardGainDb, 0.0f) : 0.0f, shown.compUp - release);
    shown.clip = juce::jmax (s.active ? finiteOr (s.clipEnergyRatioDb, -160.0f) : -160.0f, shown.clip - release * 2.0f);

    const float smooth = 1.0f - std::exp (-dt / 0.15f);
    shown.correlation += (juce::jlimit (-1.0f, 1.0f, finiteOr (s.correlation, 1.0f)) - shown.correlation) * smooth;
    shown.width += (juce::jlimit (0.0f, 3.0f, finiteOr (s.effectiveWidth, 1.0f)) - shown.width) * smooth;

    const bool changed = shown.active != painted.active || differs (shown.momentary, painted.momentary, 0.05f)
                         || differs (shown.shortTerm, painted.shortTerm, 0.05f) || differs (shown.integrated, painted.integrated, 0.05f)
                         || differs (shown.range, painted.range, 0.05f) || differs (shown.truePeakMax, painted.truePeakMax, 0.05f)
                         || differs (shown.autoLevel, painted.autoLevel, 0.05f) || differs (shown.comp, painted.comp, 0.02f)
                         || differs (shown.compUp, painted.compUp, 0.02f) || differs (shown.limiter, painted.limiter, 0.02f)
                         || differs (shown.glue, painted.glue, 0.02f) || differs (shown.clip, painted.clip, 0.1f)
                         || differs (shown.bass, painted.bass, 0.02f) || differs (shown.master, painted.master, 0.02f)
                         || differs (shown.correlation, painted.correlation, 0.005f) || differs (shown.width, painted.width, 0.005f);
    if (changed)
        repaint();
}

void LoudnessPanel::reset()
{
    shown = {};
    painted = {};
    repaint();
}

void LoudnessPanel::drawGainReductionRow (juce::Graphics& g, juce::Rectangle<float> row, const juce::String& name, float reductionDb,
                                          float rangeDb, float upwardDb)
{
    const float nameWidth = juce::jmin (84.0f, row.getWidth() * 0.36f);
    const float valueWidth = 46.0f;
    g.setColour (Palette::muted);
    g.setFont (Theme::font (11.5f));
    g.drawText (name, row.removeFromLeft (nameWidth), juce::Justification::centredLeft, true);
    auto valueArea = row.removeFromRight (valueWidth);

    auto bar = row.withSizeKeepingCentre (row.getWidth(), juce::jmin (6.0f, row.getHeight() - 6.0f));
    g.setColour (Palette::well);
    g.fillRoundedRectangle (bar, 2.0f);

    const float amount = juce::jlimit (0.0f, 1.0f, -reductionDb / rangeDb);
    if (amount > 0.001f)
    {
        g.setColour (Palette::amber);
        g.fillRoundedRectangle (bar.withWidth (juce::jmax (2.0f, bar.getWidth() * amount)), 2.0f);
    }
    if (upwardDb > 0.05f)
    {
        const float up = juce::jlimit (0.0f, 1.0f, upwardDb / rangeDb);
        g.setColour (Theme::accent (*this).withAlpha (0.85f));
        g.fillRoundedRectangle (bar.withLeft (bar.getRight() - bar.getWidth() * up), 2.0f);
    }

    g.setFont (Theme::numeric (11.5f, false));
    g.setColour (amount > 0.001f ? Palette::text : Palette::faint);
    g.drawText (reductionDb < -0.05f ? juce::String (reductionDb, 1) : juce::String ("0.0"), valueArea, juce::Justification::centredRight, false);
}

void LoudnessPanel::paint (juce::Graphics& g)
{
    painted = shown;
    Theme::drawPanel (g, getLocalBounds().toFloat());
    const auto accent = Theme::accent (*this);

    auto r = getLocalBounds().toFloat().reduced (14.0f, 12.0f);
    const float fixed = 18.0f + 60.0f + 20.0f + 10.0f + 18.0f + 10.0f + 18.0f; // captions / readouts / gaps
    const int grRows = 6, stereoRows = 2;
    const float rowH = juce::jlimit (14.0f, 22.0f, (r.getHeight() - fixed) / static_cast<float> (grRows + stereoRows));

    // ---- Loudness ----
    Theme::drawCaption (g, "LOUDNESS", r.removeFromTop (18.0f));
    {
        auto big = r.removeFromTop (60.0f);
        const float colW = big.getWidth() / 3.0f;
        auto column = [&] (const juce::String& name, float lufs, bool emphasise) -> juce::Rectangle<float>
        {
            auto c = big.removeFromLeft (colW);
            const auto area = c;
            Theme::drawCaption (g, name, c.removeFromTop (14.0f), Palette::faint);
            g.setColour (lufs <= -70.0f ? Palette::faint : (emphasise ? accent : Palette::text));
            g.setFont (Theme::numeric (emphasise ? 25.0f : 21.0f));
            g.drawText (Theme::formatLufs (lufs), c.removeFromTop (30.0f), juce::Justification::centredLeft, false);
            g.setColour (Palette::faint);
            g.setFont (Theme::font (10.0f));
            g.drawText ("LUFS", c, juce::Justification::topLeft, false);
            return area;
        };
        column ("MOMENT.", shown.momentary, false);
        column ("SHORT", shown.shortTerm, true);
        integratedArea = column ("INTEGR.", shown.integrated, false);
    }
    {
        auto row = r.removeFromTop (20.0f);
        const float colW = row.getWidth() / 3.0f;
        auto item = [&] (const juce::String& name, const juce::String& value, juce::Colour colour)
        {
            auto c = row.removeFromLeft (colW);
            g.setColour (Palette::faint);
            g.setFont (Theme::font (10.5f));
            const float nw = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), name) + 4.0f;
            g.drawText (name, c.removeFromLeft (nw), juce::Justification::centredLeft, false);
            g.setColour (colour);
            g.setFont (Theme::numeric (12.0f, false));
            g.drawText (value, c, juce::Justification::centredLeft, true);
        };
        item ("LRA", juce::String (shown.range, 1), Palette::text.withAlpha (0.9f));
        item ("TP", Theme::formatDb (shown.truePeakMax, 1), shown.truePeakMax > -1.0f ? Palette::red : Palette::text.withAlpha (0.9f));
        item ("AUTO", Theme::formatSignedDb (shown.autoLevel, 1), Palette::text.withAlpha (0.9f));
    }

    // ---- Dynamics ----
    r.removeFromTop (10.0f);
    Theme::drawCaption (g, "GAIN REDUCTION", r.removeFromTop (18.0f));
    drawGainReductionRow (g, r.removeFromTop (rowH), "Compressor", shown.comp, 12.0f, shown.compUp);
    drawGainReductionRow (g, r.removeFromTop (rowH), "Limiter", shown.limiter, 12.0f);
    drawGainReductionRow (g, r.removeFromTop (rowH), "Glue", shown.glue, 12.0f);
    drawGainReductionRow (g, r.removeFromTop (rowH), "Bass protect", shown.bass, 12.0f);
    drawGainReductionRow (g, r.removeFromTop (rowH), "Master", shown.master, 12.0f);
    {
        // Clipper: energy of what the clipper removed relative to the signal.
        auto row = r.removeFromTop (rowH);
        const float nameWidth = juce::jmin (84.0f, row.getWidth() * 0.36f);
        g.setColour (Palette::muted);
        g.setFont (Theme::font (11.5f));
        g.drawText ("Clipper", row.removeFromLeft (nameWidth), juce::Justification::centredLeft, true);
        auto valueArea = row.removeFromRight (46.0f);
        auto bar = row.withSizeKeepingCentre (row.getWidth(), juce::jmin (6.0f, row.getHeight() - 6.0f));
        g.setColour (Palette::well);
        g.fillRoundedRectangle (bar, 2.0f);
        const float amount = juce::jlimit (0.0f, 1.0f, (shown.clip + 60.0f) / 50.0f); // -60 .. -10 dB
        const bool overBudget = shown.clip > -30.0f;
        if (amount > 0.001f)
        {
            g.setColour (overBudget ? Palette::red : Palette::amber.withAlpha (0.8f));
            g.fillRoundedRectangle (bar.withWidth (juce::jmax (2.0f, bar.getWidth() * amount)), 2.0f);
        }
        const float budgetX = bar.getX() + bar.getWidth() * 0.6f; // -30 dB budget of the SafetyGovernor
        g.setColour (Palette::muted.withAlpha (0.7f));
        g.fillRect (budgetX - 0.5f, bar.getY() - 2.0f, 1.0f, bar.getHeight() + 4.0f);
        g.setFont (Theme::numeric (11.5f, false));
        g.setColour (shown.clip > -60.0f ? Palette::text : Palette::faint);
        g.drawText (shown.clip > -60.0f ? juce::String (juce::roundToInt (shown.clip)) : juce::String ("-inf"), valueArea,
                    juce::Justification::centredRight, false);
    }

    // ---- Stereo ----
    r.removeFromTop (10.0f);
    Theme::drawCaption (g, "STEREO", r.removeFromTop (18.0f));
    {
        auto row = r.removeFromTop (rowH);
        const float nameWidth = juce::jmin (84.0f, row.getWidth() * 0.36f);
        g.setColour (Palette::muted);
        g.setFont (Theme::font (11.5f));
        g.drawText ("Correlation", row.removeFromLeft (nameWidth), juce::Justification::centredLeft, true);
        auto valueArea = row.removeFromRight (46.0f);
        auto bar = row.withSizeKeepingCentre (row.getWidth(), juce::jmin (8.0f, row.getHeight() - 4.0f));
        g.setColour (Palette::well);
        g.fillRoundedRectangle (bar, 2.0f);
        const float centreX = bar.getCentreX();
        const float x = centreX + shown.correlation * bar.getWidth() * 0.5f;
        const auto colour = shown.correlation < 0.0f ? Palette::red : (shown.correlation < 0.3f ? Palette::amber : Palette::green);
        g.setColour (colour.withAlpha (0.35f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (juce::jmin (centreX, x), bar.getY(), juce::jmax (centreX, x), bar.getBottom()));
        g.setColour (Palette::muted.withAlpha (0.6f));
        g.fillRect (centreX - 0.5f, bar.getY() - 2.0f, 1.0f, bar.getHeight() + 4.0f);
        g.setColour (colour);
        g.fillRoundedRectangle (juce::Rectangle<float> (3.0f, bar.getHeight() + 6.0f).withCentre ({ x, bar.getCentreY() }), 1.5f);
        g.setFont (Theme::numeric (11.5f, false));
        g.setColour (Palette::text);
        g.drawText (juce::String (shown.correlation, 2), valueArea, juce::Justification::centredRight, false);
    }
    {
        auto row = r.removeFromTop (rowH);
        const float nameWidth = juce::jmin (84.0f, row.getWidth() * 0.36f);
        g.setColour (Palette::muted);
        g.setFont (Theme::font (11.5f));
        g.drawText ("Width", row.removeFromLeft (nameWidth), juce::Justification::centredLeft, true);
        auto valueArea = row.removeFromRight (46.0f);
        auto bar = row.withSizeKeepingCentre (row.getWidth(), juce::jmin (6.0f, row.getHeight() - 6.0f));
        g.setColour (Palette::well);
        g.fillRoundedRectangle (bar, 2.0f);
        const float amount = juce::jlimit (0.0f, 1.0f, shown.width / 2.0f); // 0 .. 200 %
        g.setColour (accent.withAlpha (0.85f));
        g.fillRoundedRectangle (bar.withWidth (juce::jmax (2.0f, bar.getWidth() * amount)), 2.0f);
        g.setColour (Palette::muted.withAlpha (0.6f));
        g.fillRect (bar.getCentreX() - 0.5f, bar.getY() - 2.0f, 1.0f, bar.getHeight() + 4.0f); // 100 %
        g.setFont (Theme::numeric (11.5f, false));
        g.setColour (Palette::text);
        g.drawText (juce::String (juce::roundToInt (shown.width * 100.0f)) + "%", valueArea, juce::Justification::centredRight, false);
    }
}

void LoudnessPanel::mouseUp (const juce::MouseEvent& e)
{
    if (integratedArea.contains (e.position) && onResetRequested != nullptr)
        onResetRequested();
}
} // namespace flub::app::ui
