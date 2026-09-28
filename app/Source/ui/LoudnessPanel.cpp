#include "LoudnessPanel.h"

#include "Theme.h"

#include "flub/engine/Protection.h"

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
    shown.inShortTerm = finiteOr (s.inShortTermLufs, -160.0f);
    shown.preamp = finiteOr (s.autoPreampDb, 0.0f);
    {
        // Time share with the maximizer's limiter more than 1 dB down: an
        // exponential average of "active" over about kLimiterActiveSeconds,
        // on the raw (unheld) reading of each frame.
        const bool limiting = s.active && finiteOr (s.maxGainReductionDb, 0.0f) < kLimiterActiveThresholdDb;
        shown.limiterActive += ((limiting ? 1.0f : 0.0f) - shown.limiterActive) * (1.0f - std::exp (-dt / kLimiterActiveSeconds));
    }

    followGr (shown.comp, s.active ? s.compGainReductionDb : 0.0f);
    followGr (shown.limiter, s.active ? s.maxGainReductionDb : 0.0f);
    followGr (shown.glue, s.active ? s.glueGainReductionDb : 0.0f);
    followGr (shown.bass, s.active ? s.bassProtectionDb : 0.0f);
    followGr (shown.master, s.active ? s.masterGainReductionDb : 0.0f);
    shown.compUp = juce::jmax (s.active ? finiteOr (s.compUpwardGainDb, 0.0f) : 0.0f, shown.compUp - release);
    // What the Safety Governor weighs: measured THD+N of the saturator and the
    // clipper, floored by the clipper's clip-energy ratio (03 §14.5).
    const float distortion = juce::jmax (finiteOr (s.distortionDb, -160.0f), finiteOr (s.clipEnergyRatioDb, -160.0f));
    shown.clip = juce::jmax (s.active ? distortion : -160.0f, shown.clip - release * 2.0f);
    shown.harmonics = juce::jmax (s.active ? finiteOr (s.harmonicsDb, -160.0f) : -160.0f, shown.harmonics - release * 2.0f);

    const float smooth = 1.0f - std::exp (-dt / 0.15f);
    shown.correlation += (juce::jlimit (-1.0f, 1.0f, finiteOr (s.correlation, 1.0f)) - shown.correlation) * smooth;
    shown.width += (juce::jlimit (0.0f, 3.0f, finiteOr (s.effectiveWidth, 1.0f)) - shown.width) * smooth;

    const bool changed = shown.active != painted.active || differs (shown.momentary, painted.momentary, 0.05f)
                         || differs (shown.shortTerm, painted.shortTerm, 0.05f) || differs (shown.integrated, painted.integrated, 0.05f)
                         || differs (shown.range, painted.range, 0.05f) || differs (shown.truePeakMax, painted.truePeakMax, 0.05f)
                         || differs (shown.autoLevel, painted.autoLevel, 0.05f) || differs (shown.inShortTerm, painted.inShortTerm, 0.05f)
                         || differs (shown.limiterActive, painted.limiterActive, 0.005f) || differs (shown.preamp, painted.preamp, 0.05f)
                         || differs (shown.harmonics, painted.harmonics, 0.1f) || differs (shown.comp, painted.comp, 0.02f)
                         || differs (shown.compUp, painted.compUp, 0.02f) || differs (shown.limiter, painted.limiter, 0.02f)
                         || differs (shown.glue, painted.glue, 0.02f) || differs (shown.clip, painted.clip, 0.1f)
                         || differs (shown.bass, painted.bass, 0.02f) || differs (shown.master, painted.master, 0.02f)
                         || differs (shown.correlation, painted.correlation, 0.005f) || differs (shown.width, painted.width, 0.005f);
    // Loudness readouts are read by eye: 20 Hz is plenty and halves the paint cost.
    sinceRepaint += dt;
    if (changed && sinceRepaint >= 0.05f)
    {
        sinceRepaint = 0.0f;
        repaint();
    }
}

void LoudnessPanel::reset()
{
    shown = {};
    painted = {};
    repaint();
}

juce::String LoudnessPanel::formatInOutDelta (float inLufs, float outLufs)
{
    if (! std::isfinite (inLufs) || ! std::isfinite (outLufs) || inLufs <= -70.0f || outLufs <= -70.0f)
        return "--";
    return Theme::formatSignedDb (outLufs - inLufs, 1) + " LU";
}

void LoudnessPanel::drawLevelRow (juce::Graphics& g, juce::Rectangle<float> row, const juce::String& name, float levelDb, float budgetDb,
                                  juce::Colour colour)
{
    const float nameWidth = juce::jmin (84.0f, row.getWidth() * 0.36f);
    g.setColour (Palette::muted);
    g.setFont (Theme::font (11.5f));
    g.drawText (name, row.removeFromLeft (nameWidth), juce::Justification::centredLeft, true);
    auto valueArea = row.removeFromRight (46.0f);
    auto bar = row.withSizeKeepingCentre (row.getWidth(), juce::jmin (6.0f, row.getHeight() - 6.0f));
    g.setColour (Palette::well);
    g.fillRoundedRectangle (bar, 2.0f);
    const float amount = juce::jlimit (0.0f, 1.0f, (levelDb + 60.0f) / 50.0f); // -60 .. -10 dB
    const bool hasBudget = budgetDb > -60.0f;
    if (amount > 0.001f)
    {
        g.setColour (hasBudget && levelDb > budgetDb ? Theme::statusColours (*this).hot : colour);
        g.fillRoundedRectangle (bar.withWidth (juce::jmax (2.0f, bar.getWidth() * amount)), 2.0f);
    }
    if (hasBudget)
    {
        const float budgetX = bar.getX() + bar.getWidth() * (budgetDb + 60.0f) / 50.0f;
        g.setColour (Palette::muted.withAlpha (0.7f));
        g.fillRect (budgetX - 0.5f, bar.getY() - 2.0f, 1.0f, bar.getHeight() + 4.0f);
    }
    g.setFont (Theme::numeric (11.5f, false));
    g.setColour (levelDb > -60.0f ? Palette::text : Palette::faint);
    g.drawText (levelDb > -60.0f ? juce::String (juce::roundToInt (levelDb)) : juce::String ("-inf"), valueArea, juce::Justification::centredRight,
                false);
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
        g.setColour (Theme::statusColours (*this).warn);
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
    const auto status = Theme::statusColours (*this); // follows the meter palette

    auto r = getLocalBounds().toFloat().reduced (14.0f, 12.0f);
    const float fixed = 18.0f + 60.0f + 20.0f + 20.0f + 10.0f + 18.0f + 10.0f + 18.0f; // captions / readouts / gaps
    const int grRows = 7, stereoRows = 2;
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
    auto item = [&g] (juce::Rectangle<float>& row, float colW, const juce::String& name, const juce::String& value, juce::Colour colour)
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
    {
        auto row = r.removeFromTop (20.0f);
        const float colW = row.getWidth() / 3.0f;
        const auto plain = Palette::text.withAlpha (0.9f);
        item (row, colW, "LRA", juce::String (shown.range, 1), plain);
        item (row, colW, "TP", Theme::formatDb (shown.truePeakMax, 1), shown.truePeakMax > -1.0f ? status.hot : plain);
        item (row, colW, "AUTO", Theme::formatSignedDb (shown.autoLevel, 1), plain);
    }
    {
        // What the strip does to loudness: short-term out - in, the limiter's
        // active share and the automatic preamp (docs/11 E38 / E11).
        auto row = r.removeFromTop (20.0f);
        const float colW = row.getWidth() / 3.0f;
        const auto plain = Palette::text.withAlpha (0.9f);
        const auto delta = formatInOutDelta (shown.active ? shown.inShortTerm : -160.0f, shown.shortTerm);
        item (row, colW * 1.25f, "IN>OUT", delta, delta == "--" ? Palette::faint : plain);
        const int limitPct = juce::roundToInt (shown.limiterActive * 100.0f);
        item (row, colW * 0.85f, "LIM", juce::String (limitPct) + "%", limitPct > 10 ? status.warn : plain);
        item (row, colW * 0.9f, "PRE", shown.preamp < -0.05f ? Theme::formatSignedDb (shown.preamp, 1) : juce::String ("off"),
              shown.preamp < -0.05f ? plain : Palette::faint);
    }
    // ---- Dynamics ----
    r.removeFromTop (10.0f);
    Theme::drawCaption (g, "GAIN REDUCTION", r.removeFromTop (18.0f));
    drawGainReductionRow (g, r.removeFromTop (rowH), "Compressor", shown.comp, 12.0f, shown.compUp);
    drawGainReductionRow (g, r.removeFromTop (rowH), "Limiter", shown.limiter, 12.0f);
    drawGainReductionRow (g, r.removeFromTop (rowH), "Glue", shown.glue, 12.0f);
    drawGainReductionRow (g, r.removeFromTop (rowH), "Bass protect", shown.bass, 12.0f);
    drawGainReductionRow (g, r.removeFromTop (rowH), "Master", shown.master, 12.0f);
    // Distortion: measured THD+N of saturator + clipper (floored by the
    // clip-energy ratio), against the Safety Governor's budget; harmonics:
    // what the bass harmonics and the air exciter add on purpose.
    drawLevelRow (g, r.removeFromTop (rowH), "Distortion", shown.clip, flub::SafetyGovernor::kDistortionBudgetDb, status.warn.withAlpha (0.8f));
    drawLevelRow (g, r.removeFromTop (rowH), "Harmonics", shown.harmonics, -160.0f, accent.withAlpha (0.8f));

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
        const auto colour = shown.correlation < 0.0f ? status.hot : (shown.correlation < 0.3f ? status.warn : status.safe);
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
