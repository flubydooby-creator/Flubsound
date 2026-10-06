#include "ChromagramView.h"

#include "VisCommon.h"

#include "../Theme.h"

#include <algorithm>
#include <cmath>

namespace flub::app::ui::vis
{
namespace
{
juce::Colour heat (float v, juce::Colour accent)
{
    // Dark well -> accent -> near white at the top.
    v = juce::jlimit (0.0f, 1.0f, v);
    if (v < 0.75f)
        return accent.withAlpha (std::pow (v / 0.75f, 1.4f) * 0.92f);
    return accent.interpolatedWith (juce::Colours::white, (v - 0.75f) / 0.25f * 0.55f);
}
} // namespace

ChromagramView::ChromagramView()
    : image (juce::Image::ARGB, kColumns, 12, true, juce::SoftwareImageType())
{
    setOpaque (false);
    setInterceptsMouseClicks (false, false);
    values.assign (static_cast<size_t> (kColumns * 12), 0.0f);
    listener.setSampleRate (48000.0);
    lutAccent = Palette::teal;
    for (size_t i = 0; i < lut.size(); ++i)
        lut[i] = heat (static_cast<float> (i) / 255.0f, lutAccent).getPixelARGB();
}

void ChromagramView::setSampleRate (double sampleRate)
{
    listener.setSampleRate (sampleRate);
}

void ChromagramView::reset()
{
    listener.reset();
    target.fill (0.0f);
    bars.fill (0.0f);
    column.fill (0.0f);
    std::fill (values.begin(), values.end(), 0.0f);
    newest = -1;
    filled = 0;
    columnElapsed = 0.0;
    image.clear (image.getBounds());
    repaint();
}

void ChromagramView::pushPost (const float* mid, const float*, int numSamples)
{
    listener.push (mid, numSamples);
}

float ChromagramView::getHistory (int age, int pc) const noexcept
{
    if (age < 0 || age >= filled)
        return 0.0f;
    const int c = ((newest - age) % kColumns + kColumns) % kColumns;
    return values[static_cast<size_t> (c * 12 + ((pc % 12) + 12) % 12)];
}

void ChromagramView::renderColumn (int c) noexcept
{
    juce::Image::BitmapData data (image, c, 0, 1, 12, juce::Image::BitmapData::writeOnly);
    for (int pc = 0; pc < 12; ++pc)
    {
        const float v = juce::jlimit (0.0f, 1.0f, values[static_cast<size_t> (c * 12 + pc)]);
        *reinterpret_cast<juce::PixelARGB*> (data.getPixelPointer (0, 11 - pc)) = lut[static_cast<size_t> (v * 255.0f)];
    }
}

void ChromagramView::advance (const FrameContext& frame)
{
    const double dt = juce::jlimit (0.0, 0.1, frame.dtSeconds);
    if (listener.advance (dt))
        target = listener.estimator.getChroma();

    bool dirty = false;
    const auto up = static_cast<float> (1.0 - std::exp (-dt / kAttackSeconds));
    const auto down = static_cast<float> (1.0 - std::exp (-dt / kReleaseSeconds));
    for (size_t pc = 0; pc < 12; ++pc)
    {
        bars[pc] += (target[pc] - bars[pc]) * (target[pc] > bars[pc] ? up : down);
        if (bars[pc] < 1.0e-3f)
            bars[pc] = 0.0f;
        column[pc] = std::max (column[pc], bars[pc]);
        if (std::abs (bars[pc] - painted[pc]) > 0.003f)
        {
            painted[pc] = bars[pc];
            dirty = true;
        }
    }

    columnElapsed += dt;
    for (int guard = 0; columnElapsed >= kColumnSeconds && guard < 4; ++guard)
    {
        columnElapsed -= kColumnSeconds;
        newest = (newest + 1) % kColumns;
        filled = std::min (kColumns, filled + 1);
        std::copy (column.begin(), column.end(), values.begin() + newest * 12);
        renderColumn (newest);
        column = bars;
        dirty = true;
    }
    if (columnElapsed >= kColumnSeconds)
        columnElapsed = std::fmod (columnElapsed, kColumnSeconds);

    if (listener.key.getKey() != paintedKey || std::abs (listener.key.getConfidence() - paintedConfidence) > 0.01f)
    {
        paintedKey = listener.key.getKey();
        paintedConfidence = listener.key.getConfidence();
        dirty = true;
    }
    if (dirty)
        repaint();
}

void ChromagramView::lookAndFeelChanged()
{
    repaint(); // paint() rebuilds the colour table for the new accent
}

void ChromagramView::paintHeader (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour accent) const
{
    const auto& key = listener.key;
    auto r = area;
    Theme::drawCaption (g, "KEY", r.removeFromLeft (34.0f), Palette::muted);
    if (key.getKey() < 0)
    {
        g.setFont (Theme::font (13.0f));
        g.setColour (Palette::faint);
        g.drawText ("listening" + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa6")), r, juce::Justification::centredLeft, false);
        return;
    }
    const auto nameFont = Theme::font (15.0f, true);
    const auto name = music::keyName (key.getKey());
    const float w = juce::GlyphArrangement::getStringWidth (nameFont, name) + 12.0f;
    g.setFont (nameFont);
    g.setColour (accent);
    g.drawText (name, r.removeFromLeft (w), juce::Justification::centredLeft, false);

    // Confidence: a short bar and the percentage.
    const auto bar = r.removeFromLeft (56.0f).withSizeKeepingCentre (56.0f, 4.0f);
    g.setColour (Palette::track.withAlpha (0.7f));
    g.fillRoundedRectangle (bar, 2.0f);
    g.setColour (accent.withAlpha (0.85f));
    g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * key.getConfidence()), 2.0f);
    r.removeFromLeft (6.0f);
    g.setFont (Theme::numeric (12.0f, false));
    g.setColour (Palette::text.withAlpha (0.8f));
    g.drawText (juce::String (juce::roundToInt (key.getConfidence() * 100.0f)) + " %", r.removeFromLeft (40.0f), juce::Justification::centredLeft,
                false);

    if (r.getWidth() > 150.0f)
    {
        g.setFont (Theme::font (11.5f));
        g.setColour (labelColour());
        g.drawText ("relative " + music::keyName (music::relativeKey (key.getKey())), r, juce::Justification::centredRight, false);
    }
}

void ChromagramView::paintBars (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour accent) const
{
    const int key = listener.key.getKey();
    const uint16_t scale = music::scaleMask (key);
    const bool flats = music::keyPrefersFlats (key);
    auto labels = area.removeFromBottom (18.0f);
    const float slot = area.getWidth() / 12.0f;
    const float barW = juce::jmin (slot * 0.72f, 34.0f);
    g.setFont (Theme::font (11.0f, true));
    for (int pc = 0; pc < 12; ++pc)
    {
        const float cx = area.getX() + slot * (static_cast<float> (pc) + 0.5f);
        const auto track = juce::Rectangle<float> (cx - barW * 0.5f, area.getY(), barW, area.getHeight());
        g.setColour (Palette::track.withAlpha (0.28f));
        g.fillRoundedRectangle (track, 3.0f);

        const float v = juce::jlimit (0.0f, 1.0f, bars[static_cast<size_t> (pc)]);
        if (v > 0.002f)
        {
            const auto bar = track.withTop (track.getBottom() - track.getHeight() * v);
            g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.95f), bar.getX(), track.getY(), accent.withAlpha (0.35f), bar.getX(),
                                                     track.getBottom(), false));
            g.fillRoundedRectangle (bar, 3.0f);
            g.setColour (accent.brighter (0.6f).withAlpha (0.4f + 0.6f * v));
            g.fillRoundedRectangle (bar.withHeight (2.5f), 1.25f);
        }

        const bool inScale = (scale & (1u << pc)) != 0;
        const bool tonic = key >= 0 && pc == key % 12;
        g.setColour (tonic ? accent : (inScale ? Palette::text : labelColour()));
        const auto label = juce::Rectangle<float> (slot, labels.getHeight()).withCentre ({ cx, labels.getCentreY() + 1.0f });
        g.drawText (music::pitchClassName (pc, flats), label, juce::Justification::centred, false);
        if (inScale)
        {
            g.setColour (tonic ? accent : Palette::muted.withAlpha (0.7f));
            g.fillEllipse (juce::Rectangle<float> (3.0f, 3.0f).withCentre ({ cx, labels.getBottom() + 1.0f }));
        }
    }
}

void ChromagramView::paintHistory (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour accent)
{
    juce::ignoreUnused (accent);
    const bool flats = music::keyPrefersFlats (listener.key.getKey());
    auto labels = area.removeFromLeft (22.0f);
    g.setColour (Palette::track.withAlpha (0.22f));
    g.fillRoundedRectangle (area, 4.0f);

    const float rowH = area.getHeight() / 12.0f;
    g.setFont (Theme::font (9.5f));
    for (int pc = 0; pc < 12; ++pc)
    {
        const float y = area.getBottom() - rowH * static_cast<float> (pc + 1);
        if (rowH >= 9.0f || pc % 2 == 0)
        {
            g.setColour (labelColour());
            g.drawText (music::pitchClassName (pc, flats), juce::Rectangle<float> (labels.getX(), y, labels.getWidth() - 4.0f, rowH),
                        juce::Justification::centredRight, false);
        }
    }

    if (filled > 0)
    {
        const float colW = area.getWidth() / static_cast<float> (kColumns);
        const auto drawPart = [&] (int firstColumn, int count, float x)
        {
            const int x0 = juce::roundToInt (x), x1 = juce::roundToInt (x + colW * static_cast<float> (count));
            if (x1 > x0)
                g.drawImage (image, x0, juce::roundToInt (area.getY()), x1 - x0, juce::roundToInt (area.getHeight()), firstColumn, 0, count, 12);
        };
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        if (filled < kColumns)
        {
            drawPart (0, filled, area.getRight() - colW * static_cast<float> (filled));
        }
        else
        {
            const int oldest = (newest + 1) % kColumns;
            const int first = kColumns - oldest;
            drawPart (oldest, first, area.getX());
            if (newest + 1 > 0 && oldest != 0)
                drawPart (0, newest + 1, area.getX() + colW * static_cast<float> (first));
        }
    }

    // Row separators and a seconds scale.
    g.setColour (Palette::well.withAlpha (0.55f));
    for (int pc = 1; pc < 12; ++pc)
        g.fillRect (juce::Rectangle<float> (area.getX(), area.getBottom() - rowH * static_cast<float> (pc) - 0.5f, area.getWidth(), 1.0f));
    g.setColour (labelColour());
    g.setFont (Theme::font (9.5f));
    for (int s = 5; s < static_cast<int> (kHistorySeconds); s += 5)
    {
        const float x = area.getRight() - area.getWidth() * static_cast<float> (s / kHistorySeconds);
        g.setColour (Palette::well.withAlpha (0.7f));
        g.fillRect (juce::Rectangle<float> (x - 0.5f, area.getY(), 1.0f, area.getHeight()));
    }
}

void ChromagramView::paint (juce::Graphics& g)
{
    const auto accent = Theme::accent (*this);
    if (accent != lutAccent)
    {
        lutAccent = accent;
        for (size_t i = 0; i < lut.size(); ++i)
            lut[i] = heat (static_cast<float> (i) / 255.0f, accent).getPixelARGB();
        for (int c = 0; c < filled; ++c)
            renderColumn (((newest - c) % kColumns + kColumns) % kColumns);
    }

    const auto bounds = getLocalBounds().toFloat();
    drawWell (g, bounds);

    if (getHeight() < 60)
    {
        // Strip: caption, 12 cells, the key.
        auto r = bounds.reduced (10.0f, 0.0f);
        Theme::drawCaption (g, "CHROMA", r.removeFromLeft (60.0f), Palette::muted);
        const int key = listener.key.getKey();
        const auto keyText = key < 0 ? juce::String ("Key ") + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa6"))
                                      : music::keyName (key) + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 "))
                                            + juce::String (juce::roundToInt (listener.key.getConfidence() * 100.0f)) + " %";
        g.setFont (Theme::font (12.5f, key >= 0));
        g.setColour (key >= 0 ? accent : Palette::faint);
        g.drawText (keyText, r.removeFromRight (150.0f), juce::Justification::centredRight, false);
        r.removeFromRight (10.0f);
        const auto cells = r.withSizeKeepingCentre (r.getWidth(), 20.0f);
        const float w = cells.getWidth() / 12.0f;
        const bool flats = music::keyPrefersFlats (key);
        g.setFont (Theme::font (10.5f, true));
        for (int pc = 0; pc < 12; ++pc)
        {
            const auto cell = juce::Rectangle<float> (cells.getX() + w * static_cast<float> (pc), cells.getY(), w, cells.getHeight()).reduced (1.5f, 0.0f);
            const float v = bars[static_cast<size_t> (pc)];
            g.setColour (Palette::track.withAlpha (0.3f));
            g.fillRoundedRectangle (cell, 3.0f);
            g.setColour (heat (v, accent));
            g.fillRoundedRectangle (cell, 3.0f);
            g.setColour (v > 0.6f ? Palette::well : Palette::muted);
            g.drawText (music::pitchClassName (pc, flats), cell, juce::Justification::centred, false);
        }
        return;
    }

    auto r = bounds.reduced (12.0f, 8.0f);
    paintHeader (g, r.removeFromTop (22.0f), accent);
    r.removeFromTop (8.0f);

    if (r.getWidth() >= 520.0f)
    {
        auto barsArea = r.removeFromLeft (juce::jlimit (260.0f, 420.0f, r.getWidth() * 0.38f));
        paintBars (g, barsArea.withTrimmedBottom (4.0f), accent);
        r.removeFromLeft (16.0f);
        auto scale = r.removeFromBottom (18.0f).withTrimmedLeft (22.0f);
        paintHistory (g, r.withTrimmedBottom (4.0f), accent);
        g.setFont (Theme::font (9.5f));
        g.setColour (labelColour());
        g.drawText ("-" + juce::String (static_cast<int> (kHistorySeconds)) + " s", scale, juce::Justification::centredLeft, false);
        g.drawText ("now", scale, juce::Justification::centredRight, false);
    }
    else
    {
        const float barsH = juce::jmax (60.0f, r.getHeight() * 0.5f);
        paintBars (g, r.removeFromTop (barsH), accent);
        r.removeFromTop (12.0f);
        paintHistory (g, r, accent);
    }
}
} // namespace flub::app::ui::vis
