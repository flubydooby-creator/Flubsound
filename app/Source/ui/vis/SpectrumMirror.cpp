#include "SpectrumMirror.h"

#include "VisCommon.h"

#include "../SpectrumAnalyzer.h"
#include "../Theme.h"

#include <cmath>

namespace flub::app::ui::vis
{
namespace
{
constexpr float kFloorDb = -200.0f;
constexpr double kLowHz = 20.0, kHighHz = 20000.0;
} // namespace

SpectrumMirror::SpectrumMirror (Mode m) : mode (m)
{
    for (int i = 0; i < kPoints; ++i)
    {
        const auto k = static_cast<size_t> (i);
        hz[k] = pointHz (i);
        tiltDb[k] = 4.5f * static_cast<float> (std::log2 (hz[k] / 1000.0));
    }
    pre.fill (kFloorDb);
    post.fill (kFloorDb);
    for (auto* p : { &preFill, &postFill, &postLine })
        p->preallocateSpace (3 * (kPoints + 6));
    spectrogram.clear();
    setOpaque (false);
    setInterceptsMouseClicks (false, false);
}

double SpectrumMirror::pointHz (int point) noexcept
{
    const double t = static_cast<double> (juce::jlimit (0, kPoints - 1, point)) / (kPoints - 1);
    return kLowHz * std::pow (kHighHz / kLowHz, t);
}

void SpectrumMirror::reset()
{
    pre.fill (kFloorDb);
    post.fill (kFloorDb);
    spectrogram.clear();
    rowElapsed = 0.0;
    rebuildPaths();
    repaint();
}

void SpectrumMirror::advance (const FrameContext& frame)
{
    const auto* spectrum = frame.spectrum;
    anyData = spectrum != nullptr && spectrum->hasData();
    tilt = spectrum == nullptr || spectrum->isTiltEnabled();
    if (mode == Mode::Spectrum)
    {
        for (size_t i = 0; i < hz.size(); ++i)
        {
            const float t = tilt ? tiltDb[i] : 0.0f;
            pre[i] = anyData ? spectrum->getDisplayLevelDb (false, hz[i]) + t : kFloorDb;
            post[i] = anyData ? spectrum->getDisplayLevelDb (true, hz[i]) + t : kFloorDb;
        }
        if (isVisible())
            rebuildPaths();
    }
    else
    {
        // A row per 1/60 s of the analysed levels (no ballistics: sharp in time).
        rowElapsed += juce::jlimit (0.0, 0.25, frame.dtSeconds);
        for (int guard = 0; rowElapsed >= kRowSeconds && guard < 16; ++guard)
        {
            rowElapsed -= kRowSeconds;
            for (size_t i = 0; i < hz.size(); ++i)
            {
                post[i] = anyData ? spectrum->getBandLevelDb (true, hz[i]) + (tilt ? tiltDb[i] : 0.0f) : kFloorDb;
                row[i] = post[i];
            }
            spectrogram.writeRow (row.data(), kPoints, kSpectrogramFloorDb, kSpectrogramTopDb);
        }
        if (rowElapsed >= kRowSeconds)
            rowElapsed = std::fmod (rowElapsed, kRowSeconds);
    }
    if (isVisible())
        repaint();
}

float SpectrumMirror::yFor (float db) const noexcept
{
    const float t = juce::jlimit (0.0f, 1.0f, (db - kMinDb) / (kMaxDb - kMinDb));
    return plot.getBottom() - t * plot.getHeight();
}

float SpectrumMirror::xFor (int point) const noexcept
{
    return plot.getX() + plot.getWidth() * static_cast<float> (point) / static_cast<float> (kPoints - 1);
}

void SpectrumMirror::rebuildPaths()
{
    preFill.clear();
    postFill.clear();
    postLine.clear();
    if (plot.isEmpty() || mode != Mode::Spectrum)
        return;
    const float bottom = plot.getBottom();
    preFill.startNewSubPath (plot.getX(), bottom);
    postFill.startNewSubPath (plot.getX(), bottom);
    for (int i = 0; i < kPoints; ++i)
    {
        const auto k = static_cast<size_t> (i);
        const float x = xFor (i);
        preFill.lineTo (x, yFor (pre[k]));
        const float y = yFor (post[k]);
        postFill.lineTo (x, y);
        if (i == 0)
            postLine.startNewSubPath (x, y);
        else
            postLine.lineTo (x, y);
    }
    preFill.lineTo (plot.getRight(), bottom);
    preFill.closeSubPath();
    postFill.lineTo (plot.getRight(), bottom);
    postFill.closeSubPath();
}

void SpectrumMirror::resized()
{
    well = getLocalBounds().toFloat();
    plot = well.withTrimmedLeft (38.0f).withTrimmedRight (14.0f).withTrimmedTop (12.0f).withTrimmedBottom (24.0f);
    rebuildPaths();
}

void SpectrumMirror::lookAndFeelChanged()
{
    accent = Theme::accent (*this);
    spectrogram.setColours (Palette::well, accent, Palette::text);
    repaint();
}

void SpectrumMirror::paint (juce::Graphics& g)
{
    drawWell (g, well);
    if (plot.isEmpty())
        return;

    const auto xForHz = [this] (double f)
    { return plot.getX() + plot.getWidth() * static_cast<float> (std::log (f / kLowHz) / std::log (kHighHz / kLowHz)); };

    if (mode == Mode::Spectrogram)
    {
        spectrogram.draw (g, plot);
    }
    else
    {
        // dB lines every 12 dB.
        g.setFont (labelFont());
        for (float db = kMaxDb - 12.0f; db > kMinDb; db -= 12.0f)
        {
            const float y = yFor (db);
            g.setColour (Palette::gridMinor);
            g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
            g.setColour (labelColour());
            g.drawText (juce::String (juce::roundToInt (db)), juce::Rectangle<float> (well.getX() + 2.0f, y - 7.0f, 32.0f, 14.0f),
                        juce::Justification::centredRight, false);
        }
    }
    // Frequency lines (decades bold) and labels.
    static constexpr double lines[] = { 30, 40, 50, 60, 70, 80, 90, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000,
                                        2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 10000 };
    for (const double f : lines)
    {
        const bool decade = f == 100.0 || f == 1000.0 || f == 10000.0;
        g.setColour ((decade ? Palette::gridMajor : Palette::gridMinor).withMultipliedAlpha (mode == Mode::Spectrogram ? 0.5f : 1.0f));
        g.drawVerticalLine (juce::roundToInt (xForHz (f)), plot.getY(), plot.getBottom());
    }
    g.setFont (labelFont());
    g.setColour (labelColour());
    static constexpr std::pair<double, const char*> labels[] = { { 50.0, "50" }, { 100.0, "100" }, { 200.0, "200" }, { 500.0, "500" },
                                                                { 1000.0, "1k" }, { 2000.0, "2k" }, { 5000.0, "5k" }, { 10000.0, "10k" },
                                                                { 20000.0, "20k" } };
    for (const auto& [f, text] : labels)
        g.drawText (text, juce::Rectangle<float> (40.0f, 14.0f).withCentre ({ juce::jmin (xForHz (f), plot.getRight() - 12.0f), plot.getBottom() + 12.0f }),
                    juce::Justification::centred, false);

    if (mode == Mode::Spectrum)
    {
        g.setColour (accent.withAlpha (0.09f));
        g.fillPath (postFill);
        g.setColour (Palette::muted.withAlpha (0.16f));
        g.fillPath (preFill);
        g.setColour (accent.withAlpha (0.14f));
        g.strokePath (postLine, juce::PathStrokeType (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (accent.withAlpha (0.30f));
        g.strokePath (postLine, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (accent.interpolatedWith (Palette::text, 0.2f));
        g.strokePath (postLine, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    else
    {
        g.setColour (labelColour());
        g.drawText ("now", juce::Rectangle<float> (well.getX() + 2.0f, plot.getY(), 32.0f, 14.0f), juce::Justification::centredRight, false);
        g.drawText ("-6 s", juce::Rectangle<float> (well.getX() + 2.0f, plot.getBottom() - 14.0f, 32.0f, 14.0f), juce::Justification::centredRight,
                    false);
    }
    if (! anyData)
    {
        g.setColour (Palette::muted);
        g.drawText ("No signal", plot.withHeight (22.0f).translated (0.0f, 8.0f), juce::Justification::centred, false);
    }
}
} // namespace flub::app::ui::vis
