#include "Waterfall3D.h"

#include "VisCommon.h"

#include "../SpectrumAnalyzer.h"
#include "../Theme.h"

#include <algorithm>
#include <cmath>

namespace flub::app::ui::vis
{
namespace
{
constexpr float kFloorDb = -200.0f;
constexpr double kLowHz = 20.0, kHighHz = 20000.0;
constexpr float kHorizon = 0.30f;    // horizon height in the plot (from the top)
constexpr float kPeakHeight = 0.56f; // a full-height front ridge rises this share of the plot
constexpr float kBackWidth = 0.42f;  // extra convergence of the width: x scale = kBackWidth + (1 - kBackWidth) * s

/** The other mode's colour: the far end of the depth fade. */
juce::Colour farColourFor (juce::Colour accent)
{
    return accent == Palette::magenta ? Palette::teal : Palette::magenta;
}
} // namespace

Waterfall3D::Waterfall3D()
{
    rows.assign (static_cast<size_t> (kRows * kColumns), kFloorDb);
    for (int c = 0; c < kColumns; ++c)
    {
        hz[static_cast<size_t> (c)] = columnHz (c);
        tiltDb[static_cast<size_t> (c)] = kTiltDbPerOctave * static_cast<float> (std::log2 (columnHz (c) / 1000.0));
    }
    live.fill (kFloorDb);
    running.fill (kFloorDb);
    // Every ridge: kColumns points plus the baseline; preallocated so a frame never grows a path.
    for (size_t i = 0; i < lines.size(); ++i)
    {
        lines[i].preallocateSpace (3 * (kColumns + 4));
        fills[i].preallocateSpace (3 * (kColumns + 8));
    }
    setOpaque (false);
    setInterceptsMouseClicks (false, false);
}

// =============================================================================
// Mapping
// =============================================================================
double Waterfall3D::columnHz (int column) noexcept
{
    const double t = static_cast<double> (juce::jlimit (0, kColumns - 1, column)) / (kColumns - 1);
    return kLowHz * std::pow (kHighHz / kLowHz, t);
}

int Waterfall3D::columnFor (double frequency) noexcept
{
    const double t = std::log (juce::jlimit (kLowHz, kHighHz, frequency) / kLowHz) / std::log (kHighHz / kLowHz);
    return juce::jlimit (0, kColumns - 1, juce::roundToInt (t * (kColumns - 1)));
}

float Waterfall3D::heightFor (float db, float top) noexcept
{
    const float t = juce::jlimit (0.0f, 1.0f, (db - (top - kRangeDb)) / kRangeDb);
    return std::pow (t, 1.6f); // the floor flattens out, peaks stand up
}

juce::Point<float> Waterfall3D::project (juce::Rectangle<float> area, float u, float depth, float height) noexcept
{
    const float z = 1.0f + juce::jlimit (0.0f, 1.0f, depth) * (kFarDepth - 1.0f);
    const float s = 1.0f / z;
    const float horizon = area.getY() + area.getHeight() * kHorizon;
    const float ground = horizon + (area.getBottom() - horizon) * s;
    const float sx = kBackWidth + (1.0f - kBackWidth) * s;
    const float x = area.getCentreX() + (u - 0.5f) * area.getWidth() * sx;
    return { x, ground - height * area.getHeight() * kPeakHeight * s };
}

float Waterfall3D::getLevel (int age, int column) const noexcept
{
    if (age < 0 || age >= filled || column < 0 || column >= kColumns)
        return kFloorDb;
    const int row = ((newest - age) % kRows + kRows) % kRows;
    return rows[static_cast<size_t> (row * kColumns + column)];
}

// =============================================================================
// Data
// =============================================================================
void Waterfall3D::setSampleRate (double) {}

void Waterfall3D::reset()
{
    std::fill (rows.begin(), rows.end(), kFloorDb);
    live.fill (kFloorDb);
    running.fill (kFloorDb);
    newest = -1;
    filled = 0;
    elapsed = 0.0;
    runningEmpty = true;
    anyData = false;
    topDb = -30.0f;
    rebuildPaths();
    repaint();
}

void Waterfall3D::advance (const FrameContext& frame)
{
    const double dt = juce::jlimit (0.0, 0.25, frame.dtSeconds);
    const auto* spectrum = frame.spectrum;
    anyData = spectrum != nullptr && spectrum->hasData();
    float frameMax = kFloorDb;
    for (size_t c = 0; c < live.size(); ++c)
    {
        live[c] = anyData ? spectrum->getDisplayLevelDb (true, hz[c]) + tiltDb[c] : kFloorDb;
        frameMax = juce::jmax (frameMax, live[c]);
        running[c] = runningEmpty ? live[c] : juce::jmax (running[c], live[c]);
    }
    runningEmpty = false;

    elapsed += dt;
    for (int guard = 0; elapsed >= kRowSeconds && guard < kRows; ++guard)
    {
        newest = (newest + 1) % kRows;
        std::copy (running.begin(), running.end(), rows.begin() + newest * kColumns);
        filled = juce::jmin (kRows, filled + 1);
        elapsed -= kRowSeconds;
        running = live;
    }
    if (elapsed >= kRowSeconds)
        elapsed = std::fmod (elapsed, kRowSeconds);

    // Automatic range: the loudest band near the top, at once going up, slowly coming down.
    if (frameMax > -150.0f)
    {
        const float target = juce::jlimit (kMinTopDb, kMaxTopDb, frameMax + 3.0f);
        topDb = target > topDb ? target : topDb + (target - topDb) * (1.0f - std::exp (-static_cast<float> (dt) / kTopReleaseSeconds));
    }

    if (isVisible())
    {
        rebuildPaths();
        repaint();
    }
}

// =============================================================================
// Drawing
// =============================================================================
void Waterfall3D::buildRidge (juce::Path& line, juce::Path& fill, const float* levels, float depth) const
{
    line.clear();
    fill.clear();
    if (plot.isEmpty())
        return;
    const float step = 1.0f / static_cast<float> (kColumns - 1);
    for (int c = 0; c < kColumns; ++c)
    {
        const auto p = project (plot, static_cast<float> (c) * step, depth, heightFor (levels[c], topDb));
        if (c == 0)
        {
            line.startNewSubPath (p);
            const auto base = project (plot, 0.0f, depth, 0.0f);
            fill.startNewSubPath (base.x, base.y + 1.0f);
            fill.lineTo (p);
        }
        else
        {
            line.lineTo (p);
            fill.lineTo (p);
        }
    }
    const auto baseRight = project (plot, 1.0f, depth, 0.0f);
    fill.lineTo (baseRight.x, baseRight.y + 1.0f);
    fill.closeSubPath();
}

void Waterfall3D::rebuildPaths()
{
    const float frac = static_cast<float> (elapsed / kRowSeconds);
    buildRidge (lines[0], fills[0], live.data(), 0.0f);
    depths[0] = 0.0f;
    numDrawn = 1;
    for (int age = 0; age < filled; ++age)
    {
        const float depth = (static_cast<float> (age) + 1.0f + frac) / static_cast<float> (kRows);
        if (depth > 1.0f)
            break;
        const int row = ((newest - age) % kRows + kRows) % kRows;
        const auto i = static_cast<size_t> (numDrawn);
        buildRidge (lines[i], fills[i], rows.data() + row * kColumns, depth);
        depths[i] = depth;
        ++numDrawn;
    }
}

void Waterfall3D::resized()
{
    well = getLocalBounds().toFloat();
    plot = well.reduced (14.0f, 0.0f).withTrimmedTop (8.0f).withTrimmedBottom (24.0f);

    // The floor grid: decade lines running to the horizon, second lines across.
    floorGrid.clear();
    for (const double f : { 100.0, 1000.0, 10000.0 })
    {
        const auto u = static_cast<float> (std::log (f / kLowHz) / std::log (kHighHz / kLowHz));
        floorGrid.startNewSubPath (project (plot, u, 0.0f, 0.0f));
        floorGrid.lineTo (project (plot, u, 1.0f, 0.0f));
    }
    const double total = kRows * kRowSeconds;
    for (int second = 1; second <= static_cast<int> (total); ++second)
    {
        const auto d = static_cast<float> (second / total);
        floorGrid.startNewSubPath (project (plot, 0.0f, d, 0.0f));
        floorGrid.lineTo (project (plot, 1.0f, d, 0.0f));
    }
    renderBackdrop();
    rebuildPaths();
}

void Waterfall3D::lookAndFeelChanged()
{
    accent = Theme::accent (*this);
    farColour = farColourFor (accent);
    haze = Palette::well.interpolatedWith (farColour, 0.16f);
    renderBackdrop();
    repaint();
}

void Waterfall3D::renderBackdrop()
{
    if (well.isEmpty())
    {
        backdrop = {};
        return;
    }
    // Rendered once per size / colour change: the sky's gradients and the sun.
    const float scale = juce::jlimit (1.0f, 3.0f, juce::Component::getApproximateScaleFactorForComponent (this));
    const int w = juce::jmax (1, juce::roundToInt (well.getWidth() * scale));
    const int h = juce::jmax (1, juce::roundToInt (well.getHeight() * scale));
    if (! backdrop.isValid() || backdrop.getWidth() != w || backdrop.getHeight() != h)
        backdrop = juce::Image (juce::Image::ARGB, w, h, true);
    else
        backdrop.clear (backdrop.getBounds());

    juce::Graphics g (backdrop);
    g.addTransform (juce::AffineTransform::translation (-well.getX(), -well.getY()).scaled (scale));
    juce::Path shape;
    shape.addRoundedRectangle (well, kWellRadius);
    g.reduceClipRegion (shape);

    const float farGround = project (plot, 0.5f, 1.0f, 0.0f).y;

    // Sky: the well at the top, a haze of the far colour at the horizon; the floor darker towards the front.
    g.setGradientFill (juce::ColourGradient (Palette::well, 0.0f, well.getY(), haze, 0.0f, farGround, false));
    g.fillRect (well.withBottom (farGround));
    g.setGradientFill (juce::ColourGradient (haze, 0.0f, farGround, Palette::well.darker (0.25f), 0.0f, well.getBottom(), false));
    g.fillRect (well.withTop (farGround));

    // A low sun on the horizon (behind the landscape), striped in its lower half.
    const float radius = plot.getHeight() * 0.19f;
    const juce::Point<float> centre { plot.getCentreX(), farGround };
    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (juce::Rectangle<float> (well.getX(), well.getY(), well.getWidth(), farGround - well.getY()).toNearestInt());
        juce::ColourGradient glow (farColour.withAlpha (0.20f), centre, farColour.withAlpha (0.0f), centre.translated (radius * 2.6f, 0.0f), true);
        g.setGradientFill (glow);
        g.fillEllipse (juce::Rectangle<float> (radius * 5.2f, radius * 5.2f).withCentre (centre));
        // Amber at the top to magenta at the horizon in either mode (the synthwave sun).
        juce::ColourGradient sun (Palette::amber.interpolatedWith (Palette::magenta, 0.15f).withAlpha (0.62f), centre.x, centre.y - radius, Palette::magenta.withAlpha (0.42f), centre.x,
                                  centre.y, false);
        juce::Path disc;
        disc.addEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre));
        g.reduceClipRegion (disc);
        // Stripes cut out of the lower half, wider towards the horizon.
        float y = centre.y - radius * 0.45f;
        for (int i = 0; i < 6; ++i)
        {
            const float gap = radius * (0.03f + 0.022f * static_cast<float> (i));
            g.excludeClipRegion (juce::Rectangle<float> (centre.x - radius, y, radius * 2.0f, gap).toNearestInt());
            y += gap + radius * (0.11f - 0.012f * static_cast<float> (i));
        }
        g.setGradientFill (sun);
        g.fillPath (disc);
    }

    // Horizon line glow.
    {
        juce::ColourGradient line (farColour.withAlpha (0.0f), plot.getX(), farGround, farColour.withAlpha (0.0f), plot.getRight(), farGround, false);
        line.addColour (0.5, farColour.withAlpha (0.55f));
        g.setGradientFill (line);
        g.fillRect (juce::Rectangle<float> (plot.getX(), farGround - 0.75f, plot.getWidth(), 1.5f));
    }
}

void Waterfall3D::paint (juce::Graphics& g)
{
    if (backdrop.isValid())
        g.drawImage (backdrop, well, juce::RectanglePlacement::stretchToFit);
    else
        drawWell (g, well);

    juce::Graphics::ScopedSaveState state (g);
    g.reduceClipRegion (well.toNearestInt());

    g.setColour (Palette::gridMinor.interpolatedWith (farColour, 0.25f).withMultipliedAlpha (0.8f));
    g.strokePath (floorGrid, juce::PathStrokeType (1.0f));

    // Back to front: each ridge hides what lies behind it.
    const auto terrainFront = Palette::well.darker (0.35f);
    for (int i = numDrawn - 1; i >= 1; --i)
    {
        const auto k = static_cast<size_t> (i);
        const float d = depths[k];
        const float fadeOut = juce::jlimit (0.0f, 1.0f, (1.0f - d) * static_cast<float> (kRows) * 0.5f);
        g.setColour (terrainFront.interpolatedWith (haze, std::pow (d, 0.7f)).withAlpha (fadeOut));
        g.fillPath (fills[k]);
        const auto colour = accent.interpolatedWith (farColour, std::pow (d, 0.75f));
        g.setColour (colour.withAlpha ((0.95f - 0.75f * d) * fadeOut));
        g.strokePath (lines[k], juce::PathStrokeType (1.0f + 0.7f * (1.0f - d), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // The live front ridge, with a glow.
    g.setColour (terrainFront);
    g.fillPath (fills[0]);
    g.setColour (accent.withAlpha (0.12f));
    g.strokePath (lines[0], juce::PathStrokeType (7.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (accent.withAlpha (0.28f));
    g.strokePath (lines[0], juce::PathStrokeType (3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (accent.interpolatedWith (Palette::text, 0.35f));
    g.strokePath (lines[0], juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Labels: frequency under the front row, seconds at the right-hand edge.
    g.setFont (labelFont());
    g.setColour (labelColour());
    static constexpr std::pair<double, const char*> freqs[] = { { 50.0, "50" }, { 100.0, "100" }, { 200.0, "200" }, { 500.0, "500" },
                                                               { 1000.0, "1k" }, { 2000.0, "2k" }, { 5000.0, "5k" }, { 10000.0, "10k" } };
    for (const auto& [f, text] : freqs)
    {
        const auto u = static_cast<float> (std::log (f / kLowHz) / std::log (kHighHz / kLowHz));
        const auto p = project (plot, u, 0.0f, 0.0f);
        g.drawText (text, juce::Rectangle<float> (40.0f, 14.0f).withCentre ({ p.x, p.y + 12.0f }), juce::Justification::centred, false);
    }
    const double total = kRows * kRowSeconds;
    for (int second = 2; second <= static_cast<int> (total); second += 2)
    {
        // In the empty margin right of the landscape, level with that second's floor line.
        const auto p = project (plot, 1.0f, static_cast<float> (second / total), 0.0f);
        g.drawText ("-" + juce::String (second) + " s", juce::Rectangle<float> (plot.getRight() - 36.0f, p.y - 7.0f, 36.0f, 14.0f),
                    juce::Justification::centredRight, false);
    }
    if (! anyData)
    {
        g.setColour (Palette::muted);
        g.drawText ("No signal", well.withHeight (22.0f).translated (0.0f, 6.0f), juce::Justification::centred, false);
    }
}
} // namespace flub::app::ui::vis
