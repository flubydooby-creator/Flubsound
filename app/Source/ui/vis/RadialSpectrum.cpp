#include "RadialSpectrum.h"

#include "VisCommon.h"

#include "../MeterSnapshot.h"
#include "../SpectrumAnalyzer.h"
#include "../Theme.h"

#include <cmath>

namespace flub::app::ui::vis
{
namespace
{
constexpr float kFloorDb = -200.0f;
constexpr float kPi = juce::MathConstants<float>::pi;
constexpr float kMinTopDb = -45.0f, kMaxTopDb = 6.0f, kTopReleaseSeconds = 3.0f;
constexpr double kBassHz[] = { 40.0, 50.0, 63.0, 80.0, 100.0 };

juce::Colour farColourFor (juce::Colour accent)
{
    return accent == Palette::magenta ? Palette::teal : Palette::magenta;
}
} // namespace

RadialSpectrum::RadialSpectrum()
{
    for (int b = 0; b < kBins; ++b)
    {
        const auto i = static_cast<size_t> (b);
        hz[i] = binHz (b);
        tiltDb[i] = kTiltDbPerOctave * static_cast<float> (std::log2 (hz[i] / 1000.0));
    }
    for (auto& p : rays)
        p.preallocateSpace (2 * 5 * 3 * (kBins / kColourGroups + 2));
    outline.preallocateSpace (3 * (2 * kBins + 4));
    peakDots.preallocateSpace (2 * kBins * 20);
    ringDots.preallocateSpace (kDots * 20);
    setOpaque (false);
    setInterceptsMouseClicks (false, false);
}

// =============================================================================
// Mapping
// =============================================================================
double RadialSpectrum::binHz (int bin) noexcept
{
    const double t = static_cast<double> (juce::jlimit (0, kBins - 1, bin)) / (kBins - 1);
    return kLowHz * std::pow (kHighHz / kLowHz, t);
}

float RadialSpectrum::angleFor (double frequency) noexcept
{
    const double t = std::log (juce::jlimit (kLowHz, kHighHz, frequency) / kLowHz) / std::log (kHighHz / kLowHz);
    // A small gap at the top and bottom keeps the two halves' rays apart.
    constexpr double kGap = 0.015;
    return static_cast<float> ((kGap + t * (1.0 - 2.0 * kGap)) * juce::MathConstants<double>::pi);
}

juce::Point<float> RadialSpectrum::polar (juce::Point<float> c, float angle, float radius) noexcept
{
    return { c.x + radius * std::sin (angle), c.y - radius * std::cos (angle) };
}

float RadialSpectrum::radiusFor (float level, float inner, float outer) noexcept
{
    return inner + juce::jlimit (0.0f, 1.0f, level) * (outer - inner);
}

float RadialSpectrum::levelFor (float db, float top) noexcept
{
    const float t = juce::jlimit (0.0f, 1.0f, (db - (top - kRangeDb)) / kRangeDb);
    return std::pow (t, 1.4f);
}

float RadialSpectrum::ringGap() const noexcept
{
    return juce::jlimit (6.0f, 16.0f, outerRadius * 0.05f);
}

float RadialSpectrum::getInnerRadius() const noexcept
{
    return baseInner * (1.0f + 0.12f * pulse);
}

// =============================================================================
// Data
// =============================================================================
void RadialSpectrum::reset()
{
    levels.fill (0.0f);
    peaks.fill (0.0f);
    peakAge.fill (0.0f);
    topDb = -30.0f;
    pulse = 0.0f;
    bassFast = bassSlow = -66.0f;
    anyData = false;
    rebuildPaths();
    repaint();
}

void RadialSpectrum::advance (const FrameContext& frame)
{
    const auto dt = static_cast<float> (juce::jlimit (0.0, 0.25, frame.dtSeconds));
    const auto* spectrum = frame.spectrum;
    anyData = spectrum != nullptr && spectrum->hasData();
    active = frame.meters.active;
    loudness = frame.meters.momentaryLufs;

    std::array<float, kBins> db {};
    float frameMax = kFloorDb;
    for (size_t b = 0; b < db.size(); ++b)
    {
        db[b] = anyData ? spectrum->getDisplayLevelDb (true, hz[b]) + tiltDb[b] : kFloorDb;
        frameMax = juce::jmax (frameMax, db[b]);
    }
    if (frameMax > -150.0f)
    {
        const float target = juce::jlimit (kMinTopDb, kMaxTopDb, frameMax + 3.0f);
        topDb = target > topDb ? target : topDb + (target - topDb) * (1.0f - std::exp (-dt / kTopReleaseSeconds));
    }

    const float release = 1.0f - std::exp (-dt / kReleaseSeconds);
    float energy = 0.0f;
    for (size_t b = 0; b < db.size(); ++b)
    {
        const float target = levelFor (db[b], topDb);
        auto& l = levels[b];
        l = target > l ? target : l + (target - l) * release;
        energy += l;
        auto& p = peaks[b];
        if (l >= p)
        {
            p = l;
            peakAge[b] = 0.0f;
        }
        else
        {
            peakAge[b] += dt;
            if (peakAge[b] > kPeakHoldSeconds)
                p = juce::jmax (l, p - kPeakFallDbPerSecond / kRangeDb * dt);
        }
    }
    energy /= static_cast<float> (kBins);

    // Bass pulse: the 40 - 100 Hz level (untilted) over its own 1 s average.
    float bass = kFloorDb;
    if (anyData)
        for (const double f : kBassHz)
            bass = juce::jmax (bass, spectrum->getDisplayLevelDb (true, f));
    bass = juce::jmax (bass, -66.0f); // below this nothing pulses (leakage, hiss)
    const bool rising = bass > bassFast + 0.5f;
    bassFast = bass > bassFast ? bass : bassFast + (bass - bassFast) * (1.0f - std::exp (-dt / 0.12f));
    bassSlow += (bass - bassSlow) * (1.0f - std::exp (-dt / 0.3f));
    const float kick = juce::jlimit (0.0f, 1.0f, (bassFast - bassSlow - 1.0f) / 6.0f);
    // A hit sets the pulse while the bass rises; then it decays (held notes do not keep it up).
    pulse = pulse * std::exp (-dt / 0.18f);
    if (rising)
        pulse = juce::jmax (pulse, kick);

    rotation = std::fmod (rotation + dt * (0.06f + 0.5f * energy), 2.0f * kPi);

    if (isVisible())
    {
        rebuildPaths();
        repaint();
    }
}

// =============================================================================
// Drawing
// =============================================================================
void RadialSpectrum::rebuildPaths()
{
    for (auto& p : rays)
        p.clear();
    outline.clear();
    peakDots.clear();
    ringDots.clear();
    if (well.isEmpty())
        return;

    const float inner = getInnerRadius();
    const float base = inner + 3.0f;
    const float halfWidth = kPi / static_cast<float> (kBins) * 0.42f;
    const float dot = juce::jlimit (1.6f, 3.2f, outerRadius * 0.012f);
    for (int b = 0; b < kBins; ++b)
    {
        const auto i = static_cast<size_t> (b);
        const float a = angleFor (hz[i]);
        const float r = juce::jmax (base + 1.5f, radiusFor (levels[i], base, outerRadius));
        auto& path = rays[static_cast<size_t> (b * kColourGroups / kBins)];
        for (const float side : { 1.0f, -1.0f })
        {
            const float w = halfWidth * juce::jmin (1.0f, base / r * 1.6f); // a little narrower at the tip: rays, not wedges
            path.addQuadrilateral (polar (centre, side * (a - halfWidth), base).x, polar (centre, side * (a - halfWidth), base).y,
                                   polar (centre, side * (a + halfWidth), base).x, polar (centre, side * (a + halfWidth), base).y,
                                   polar (centre, side * (a + w), r).x, polar (centre, side * (a + w), r).y,
                                   polar (centre, side * (a - w), r).x, polar (centre, side * (a - w), r).y);
            const auto pk = polar (centre, side * a, radiusFor (peaks[i], base, outerRadius) + dot * 1.8f);
            peakDots.addEllipse (pk.x - dot * 0.5f, pk.y - dot * 0.5f, dot, dot);
        }
    }
    // The outline through the tips: right half top to bottom, then the left half back up.
    for (int k = 0; k < 2 * kBins; ++k)
    {
        const bool right = k < kBins;
        const int b = right ? k : 2 * kBins - 1 - k;
        const auto i = static_cast<size_t> (b);
        const float a = right ? angleFor (hz[i]) : -angleFor (hz[i]);
        const auto p = polar (centre, a, juce::jmax (base + 1.5f, radiusFor (levels[i], base, outerRadius)) + 1.5f);
        if (k == 0)
            outline.startNewSubPath (p);
        else
            outline.lineTo (p);
    }
    outline.closeSubPath();

    const float ringRadius = outerRadius + ringGap();
    for (int k = 0; k < kDots; ++k)
    {
        const float a = rotation + static_cast<float> (k) * 2.0f * kPi / static_cast<float> (kDots);
        const float size = (k % 6 == 0 ? 2.6f : 1.5f) * juce::jlimit (1.0f, 1.8f, outerRadius / 220.0f);
        const auto p = polar (centre, a, ringRadius);
        ringDots.addEllipse (p.x - size * 0.5f, p.y - size * 0.5f, size, size);
    }
}

void RadialSpectrum::resized()
{
    well = getLocalBounds().toFloat();
    centre = well.getCentre();
    const float size = juce::jmin (well.getWidth(), well.getHeight());
    outerRadius = juce::jmax (20.0f, size * 0.5f - (size < 400.0f ? 22.0f : 34.0f));
    baseInner = outerRadius * 0.30f;
    renderHalo();
    rebuildPaths();
}

void RadialSpectrum::lookAndFeelChanged()
{
    accent = Theme::accent (*this);
    farColour = farColourFor (accent);
    for (int k = 0; k < kColourGroups; ++k)
    {
        const float t = static_cast<float> (k) / static_cast<float> (kColourGroups - 1);
        // accent -> a saturated violet in between -> the other mode's colour.
        const auto mid = accent.interpolatedWith (farColour, 0.5f).withMultipliedSaturation (1.35f).withMultipliedBrightness (1.1f);
        rayColours[static_cast<size_t> (k)] = t < 0.5f ? accent.interpolatedWith (mid, t * 2.0f) : mid.interpolatedWith (farColour, (t - 0.5f) * 2.0f);
    }
    renderHalo();
    repaint();
}

void RadialSpectrum::renderHalo()
{
    const int d = juce::jmax (8, juce::roundToInt (outerRadius * 2.0f));
    if (d < 8 || well.isEmpty())
    {
        halo = {};
        return;
    }
    halo = juce::Image (juce::Image::ARGB, d, d, true);
    juce::Graphics g (halo);
    const juce::Point<float> c { d * 0.5f, d * 0.5f };
    juce::ColourGradient glow (accent.withAlpha (0.40f), c, farColour.withAlpha (0.0f), c.translated (d * 0.5f, 0.0f), true);
    glow.addColour (0.30, accent.withAlpha (0.30f));
    glow.addColour (0.62, farColour.withAlpha (0.12f));
    g.setGradientFill (glow);
    g.fillEllipse (0.0f, 0.0f, static_cast<float> (d), static_cast<float> (d));
}

void RadialSpectrum::paint (juce::Graphics& g)
{
    drawWell (g, well);
    if (well.isEmpty())
        return;

    // The halo swells with the bass pulse.
    if (halo.isValid())
    {
        juce::Graphics::ScopedSaveState state (g);
        g.setOpacity (0.55f + 0.45f * pulse);
        const float s = (outerRadius * 2.0f / static_cast<float> (halo.getWidth())) * (1.0f + 0.10f * pulse);
        g.drawImageTransformed (halo, juce::AffineTransform::translation (-halo.getWidth() * 0.5f, -halo.getHeight() * 0.5f)
                                          .scaled (s)
                                          .translated (centre.x, centre.y));
    }

    const float inner = getInnerRadius();
    g.setColour (Palette::gridMinor);
    for (const float f : { 0.5f, 1.0f })
    {
        const float r = inner + 3.0f + f * (outerRadius - inner - 3.0f);
        g.drawEllipse (juce::Rectangle<float> (2.0f * r, 2.0f * r).withCentre (centre), 1.0f);
    }

    g.setColour (accent.withAlpha (0.35f));
    g.fillPath (ringDots);

    for (size_t k = 0; k < rays.size(); ++k)
    {
        g.setColour (rayColours[k].withAlpha (0.88f));
        g.fillPath (rays[k]);
    }
    const auto glowColour = accent.interpolatedWith (farColour, 0.35f);
    g.setColour (glowColour.withAlpha (0.14f));
    g.strokePath (outline, juce::PathStrokeType (7.0f, juce::PathStrokeType::curved));
    g.setColour (glowColour.withAlpha (0.30f));
    g.strokePath (outline, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved));
    g.setColour (Palette::text.withAlpha (0.75f));
    g.strokePath (outline, juce::PathStrokeType (1.1f, juce::PathStrokeType::curved));
    g.setColour (Palette::text.withAlpha (0.65f));
    g.fillPath (peakDots);

    // The centre: a dark disc ringed in the accent, brighter on a kick, with the loudness.
    const auto disc = juce::Rectangle<float> (2.0f * inner, 2.0f * inner).withCentre (centre);
    g.setColour (Palette::well.darker (0.3f));
    g.fillEllipse (disc);
    g.setColour (accent.withAlpha (0.25f + 0.25f * pulse));
    g.drawEllipse (disc.expanded (2.0f + 3.0f * pulse), 2.0f + 4.0f * pulse);
    g.setColour (accent.withAlpha (0.55f + 0.45f * pulse));
    g.drawEllipse (disc, 1.5f + 1.5f * pulse);

    if (active && loudness > -70.0f)
    {
        // The number a little above the centre, "LUFS" under it (the number alone in a small ring).
        const float big = juce::jlimit (11.0f, 76.0f, inner * 0.5f);
        const bool caption = inner >= 34.0f;
        g.setColour (Palette::text);
        g.setFont (Theme::numeric (big));
        g.drawText (juce::String (loudness, 1),
                    juce::Rectangle<float> (disc.getWidth(), big * 1.2f).withCentre (centre.translated (0.0f, caption ? -big * 0.2f : 0.0f)),
                    juce::Justification::centred, false);
        if (caption)
        {
            const float captionHeight = juce::jlimit (9.0f, 18.0f, big * 0.3f);
            g.setColour (Palette::muted);
            g.setFont (Theme::caption (captionHeight));
            g.drawText ("LUFS", juce::Rectangle<float> (disc.getWidth(), captionHeight * 1.4f).withCentre (centre.translated (0.0f, big * 0.62f)),
                        juce::Justification::centred, false);
        }
    }
    else if (! anyData)
    {
        g.setColour (Palette::muted);
        g.setFont (labelFont());
        g.drawText ("No signal", disc, juce::Justification::centred, false);
    }

    // Where the frequencies are: lowest at the top, highest at the bottom.
    g.setFont (labelFont());
    g.setColour (labelColour());
    const auto top = polar (centre, 0.0f, outerRadius + ringGap() + 11.0f);
    const auto bottom = polar (centre, kPi, outerRadius + ringGap() + 11.0f);
    g.drawText ("30 Hz", juce::Rectangle<float> (60.0f, 14.0f).withCentre (top), juce::Justification::centred, false);
    g.drawText ("16 kHz", juce::Rectangle<float> (60.0f, 14.0f).withCentre (bottom), juce::Justification::centred, false);
}
} // namespace flub::app::ui::vis
