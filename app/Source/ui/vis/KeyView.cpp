#include "KeyView.h"

#include "VisCommon.h"

#include "../Theme.h"

#include <cmath>

namespace flub::app::ui::vis
{
KeyView::KeyView()
{
    setOpaque (false);
    setInterceptsMouseClicks (false, false);
    listener.setSampleRate (48000.0);
}

void KeyView::setSampleRate (double sampleRate)
{
    listener.setSampleRate (sampleRate);
}

void KeyView::reset()
{
    listener.reset();
    repaint();
}

void KeyView::pushPost (const float* mid, const float*, int numSamples)
{
    listener.push (mid, numSamples);
}

void KeyView::advance (const FrameContext& frame)
{
    listener.advance (frame.dtSeconds);
    const auto& key = listener.key;
    const auto progress = static_cast<float> (std::min (1.0, key.getSignalSeconds() / music::KeyDetector::kFullConfidenceSeconds));
    if (key.getKey() != paintedKey || std::abs (key.getConfidence() - paintedConfidence) > 0.005f
        || (key.getKey() < 0 && std::abs (progress - paintedProgress) > 0.01f))
    {
        paintedKey = key.getKey();
        paintedConfidence = key.getConfidence();
        paintedProgress = progress;
        repaint();
    }
}

void KeyView::paint (juce::Graphics& g)
{
    const auto accent = Theme::accent (*this);
    const auto bounds = getLocalBounds().toFloat();
    drawWell (g, bounds);
    auto r = bounds.reduced (10.0f, 0.0f);
    Theme::drawCaption (g, "KEY", r.removeFromLeft (40.0f), Palette::muted);

    const auto& detector = listener.key;
    const int key = detector.getKey();
    if (key < 0)
    {
        g.setFont (Theme::font (12.5f));
        g.setColour (Palette::faint);
        const auto text = listener.estimator.hasSignal() || detector.getSignalSeconds() > 0.0
                              ? "listening" + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa6"))
                              : juce::String ("no signal");
        g.drawText (text, r, juce::Justification::centredLeft, false);
        return;
    }

    // Name, confidence bar and percentage.
    const auto nameFont = Theme::font (17.0f, true);
    const auto name = music::keyName (key);
    g.setFont (nameFont);
    g.setColour (accent);
    g.drawText (name, r.removeFromLeft (juce::GlyphArrangement::getStringWidth (nameFont, name) + 14.0f), juce::Justification::centredLeft, false);

    const float confidence = detector.getConfidence();
    const auto bar = r.removeFromLeft (juce::jmin (90.0f, r.getWidth() * 0.2f)).withSizeKeepingCentre (juce::jmin (90.0f, r.getWidth() * 0.2f), 5.0f);
    g.setColour (Palette::track.withAlpha (0.7f));
    g.fillRoundedRectangle (bar, 2.5f);
    g.setColour (accent.withAlpha (0.4f + 0.6f * confidence));
    g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * confidence), 2.5f);
    r.removeFromLeft (8.0f);
    g.setFont (Theme::numeric (12.5f));
    g.setColour (Palette::text);
    g.drawText (juce::String (juce::roundToInt (confidence * 100.0f)) + " %", r.removeFromLeft (44.0f), juce::Justification::centredLeft, false);

    // The scale (tonic first), then the relative key at the right.
    const bool flats = music::keyPrefersFlats (key);
    const uint16_t scale = music::scaleMask (key);
    const auto smallFont = Theme::font (12.0f);
    const auto relative = "relative " + music::keyName (music::relativeKey (key));
    const float relW = juce::GlyphArrangement::getStringWidth (smallFont, relative) + 6.0f;
    if (r.getWidth() > relW + 200.0f)
    {
        g.setFont (smallFont);
        g.setColour (labelColour());
        g.drawText (relative, r.removeFromRight (relW), juce::Justification::centredRight, false);
    }
    r.removeFromLeft (10.0f);
    const auto noteFont = Theme::font (12.5f, true);
    g.setFont (noteFont);
    for (int i = 0; i < 12 && r.getWidth() > 24.0f; ++i)
    {
        const int pc = (key % 12 + i) % 12;
        if ((scale & (1u << pc)) == 0)
            continue;
        const juce::String note (music::pitchClassName (pc, flats));
        g.setColour (i == 0 ? accent : Palette::text.withAlpha (0.75f));
        g.drawText (note, r.removeFromLeft (juce::GlyphArrangement::getStringWidth (noteFont, note) + 10.0f), juce::Justification::centredLeft, false);
    }
}
} // namespace flub::app::ui::vis
