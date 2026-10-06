#include "ChordView.h"

#include "VisCommon.h"

#include "../Theme.h"

#include <cmath>
#include <cstring>

namespace flub::app::ui::vis
{
namespace
{
constexpr int kWhitePcs[] = { 0, 2, 4, 5, 7, 9, 11 };
constexpr int kBlackPcs[] = { 1, 3, 6, 8, 10 };
constexpr float kBlackAfterWhite[] = { 1.0f, 2.0f, 4.0f, 5.0f, 6.0f }; // boundary (in white keys) each black key sits on

juce::String dot()
{
    return juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 "));
}

juce::String keyLine (const music::KeyDetector& key)
{
    if (key.getKey() < 0)
        return "Key: listening" + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa6"));
    return "Key: " + music::keyName (key.getKey()) + dot() + juce::String (juce::roundToInt (key.getConfidence() * 100.0f)) + " %";
}
} // namespace

ChordView::ChordView()
{
    setOpaque (false);
    setInterceptsMouseClicks (false, false);
    listener.setSampleRate (48000.0);
}

void ChordView::setSampleRate (double sampleRate)
{
    listener.setSampleRate (sampleRate);
}

void ChordView::reset()
{
    listener.reset();
    tracker.reset();
    std::memcpy (text, "N.C.", 5);
    previousText[0] = '\0';
    fade = 1.0f;
    repaint();
}

void ChordView::pushPost (const float* mid, const float*, int numSamples)
{
    listener.push (mid, numSamples);
}

void ChordView::advance (const FrameContext& frame)
{
    listener.advance (frame.dtSeconds);
    const auto& est = listener.estimator;
    bool dirty = false;
    if (tracker.update (&est.getNote (0), est.getNumNotes(), frame.dtSeconds))
    {
        std::memcpy (previousText, text, sizeof (text));
        fade = 0.0f;
        dirty = true;
    }
    // The spelling follows the key: re-format when it changes.
    if (flats() != paintedFlats || dirty)
    {
        paintedFlats = flats();
        music::formatChord (tracker.getChord(), paintedFlats, text, sizeof (text));
        dirty = true;
    }
    if (fade < 1.0f)
    {
        fade = std::min (1.0f, fade + static_cast<float> (frame.dtSeconds / kFadeSeconds));
        dirty = true;
    }
    for (int pc = 0; pc < 12; ++pc)
    {
        auto& p = paintedPresence[static_cast<size_t> (pc)];
        if (std::abs (tracker.getPresence (pc) - p) > 0.02f)
        {
            p = tracker.getPresence (pc);
            dirty = true;
        }
    }
    if (listener.key.getKey() != paintedKey || std::abs (listener.key.getConfidence() - paintedConfidence) > 0.01f)
    {
        paintedKey = listener.key.getKey();
        paintedConfidence = listener.key.getConfidence();
        dirty = true;
    }
    if (dirty)
        repaint();
}

void ChordView::paintKeyboard (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour accent) const
{
    const auto& chord = tracker.getChord();
    const float whiteW = area.getWidth() / 7.0f;
    const auto keyColour = [&] (int pc, juce::Colour base)
    {
        const float p = tracker.getPresence (pc);
        if (p <= 0.01f)
            return base;
        const bool root = chord.isChord() && pc == chord.root;
        return base.interpolatedWith (root ? accent.brighter (0.35f) : accent, juce::jlimit (0.0f, 1.0f, (root ? 0.35f : 0.2f) + 0.65f * p));
    };
    const auto bassDot = [&] (juce::Rectangle<float> key, int pc, juce::Colour colour)
    {
        if (chord.isChord() && chord.quality != music::Chord::kSingleNote && pc == chord.bass)
        {
            g.setColour (colour);
            g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ key.getCentreX(), key.getBottom() - 6.0f }));
        }
    };

    g.setFont (Theme::font (10.0f));
    for (int i = 0; i < 7; ++i)
    {
        const int pc = kWhitePcs[i];
        const auto key = juce::Rectangle<float> (area.getX() + whiteW * static_cast<float> (i), area.getY(), whiteW, area.getHeight()).reduced (1.0f, 0.0f);
        g.setColour (keyColour (pc, Palette::muted.withAlpha (0.35f)));
        g.fillRoundedRectangle (key, 3.0f);
        const bool lit = tracker.getPresence (pc) > 0.5f;
        g.setColour (lit ? Palette::well : labelColour());
        g.drawText (music::pitchClassName (pc, false), key.withTrimmedTop (key.getHeight() * 0.55f).withTrimmedBottom (8.0f),
                    juce::Justification::centred, false);
        bassDot (key, pc, lit ? Palette::well : Palette::text);
    }
    for (int i = 0; i < 5; ++i)
    {
        const int pc = kBlackPcs[i];
        const float cx = area.getX() + whiteW * kBlackAfterWhite[i];
        const auto key = juce::Rectangle<float> (whiteW * 0.62f, area.getHeight() * 0.58f).withCentre ({ cx, area.getY() + area.getHeight() * 0.29f });
        g.setColour (Palette::well);
        g.fillRoundedRectangle (key.expanded (1.5f, 0.0f).withTrimmedTop (-1.0f), 3.0f);
        g.setColour (keyColour (pc, Palette::panelRaised));
        g.fillRoundedRectangle (key, 2.5f);
        bassDot (key, pc, Palette::text);
    }
}

void ChordView::paintStrip (juce::Graphics& g, juce::Colour accent)
{
    auto r = getLocalBounds().toFloat().reduced (10.0f, 0.0f);
    Theme::drawCaption (g, "CHORD", r.removeFromLeft (52.0f), Palette::muted);
    const auto& chord = tracker.getChord();

    g.setFont (Theme::font (12.0f));
    g.setColour (Palette::muted);
    const auto keyText = keyLine (listener.key);
    const float keyW = juce::GlyphArrangement::getStringWidth (Theme::font (12.0f), keyText) + 4.0f;
    if (r.getWidth() > keyW + 160.0f)
        g.drawText (keyText, r.removeFromRight (keyW), juce::Justification::centredRight, false);

    const auto nameFont = Theme::font (19.0f, true);
    const juce::String name (text);
    g.setFont (nameFont);
    g.setColour (chord.isChord() ? accent : Palette::faint);
    const float nameW = juce::GlyphArrangement::getStringWidth (nameFont, name) + 14.0f;
    g.drawText (name, r.removeFromLeft (nameW), juce::Justification::centredLeft, false);

    if (chord.isChord())
    {
        juce::String notes;
        for (int i = 0; i < 12; ++i)
            if (const int pc = (chord.root + i) % 12; (chord.mask & (1u << pc)) != 0)
                notes << (notes.isEmpty() ? "" : "  ") << music::pitchClassName (pc, paintedFlats);
        g.setFont (Theme::font (12.5f));
        g.setColour (Palette::text.withAlpha (0.8f));
        g.drawText (notes + "   " + juce::String (music::chordDescription (chord)), r, juce::Justification::centredLeft, true);
    }
}

void ChordView::paint (juce::Graphics& g)
{
    const auto accent = Theme::accent (*this);
    const auto bounds = getLocalBounds().toFloat();
    drawWell (g, bounds);
    if (getHeight() < 60)
    {
        paintStrip (g, accent);
        return;
    }

    auto r = bounds.reduced (14.0f, 10.0f);
    const auto& chord = tracker.getChord();

    // Key, top right; the last chords, bottom.
    auto top = r.removeFromTop (18.0f);
    g.setFont (Theme::font (12.0f));
    g.setColour (listener.key.getKey() >= 0 ? Palette::text.withAlpha (0.85f) : Palette::faint);
    g.drawText (keyLine (listener.key), top, juce::Justification::centredRight, false);
    g.setColour (labelColour());
    if (! listener.estimator.hasSignal())
        g.drawText ("No signal", top, juce::Justification::centredLeft, false);

    const float h = r.getHeight();
    const bool showKeyboard = h >= 170.0f, showPills = h >= 140.0f, showHistory = h >= 110.0f;

    if (showHistory)
    {
        auto historyRow = r.removeFromBottom (22.0f);
        const auto font = Theme::font (13.0f, true);
        g.setFont (font);
        const int count = std::min (tracker.getHistorySize(), 6);
        juce::String items[7];
        int n = 0;
        for (int age = count - 1; age >= 0; --age)
            items[n++] = music::chordText (tracker.getHistory (age), paintedFlats);
        items[n++] = juce::String (text);
        const juce::String arrow (juce::CharPointer_UTF8 ("  \xe2\x80\xba  "));
        float total = 0.0f;
        for (int i = 0; i < n; ++i)
            total += juce::GlyphArrangement::getStringWidth (font, items[i]) + (i + 1 < n ? juce::GlyphArrangement::getStringWidth (font, arrow) : 0.0f);
        float x = historyRow.getCentreX() - total * 0.5f;
        for (int i = 0; i < n; ++i)
        {
            const bool current = i == n - 1;
            const float w = juce::GlyphArrangement::getStringWidth (font, items[i]);
            g.setColour (current ? (chord.isChord() ? accent : Palette::faint)
                                 : Palette::text.withAlpha (0.25f + 0.5f * static_cast<float> (i + 1) / static_cast<float> (n)));
            g.drawText (items[i], juce::Rectangle<float> (x, historyRow.getY(), w + 2.0f, historyRow.getHeight()), juce::Justification::centredLeft, false);
            x += w;
            if (! current)
            {
                const float aw = juce::GlyphArrangement::getStringWidth (font, arrow);
                g.setColour (Palette::faint);
                g.drawText (arrow, juce::Rectangle<float> (x, historyRow.getY(), aw + 2.0f, historyRow.getHeight()), juce::Justification::centredLeft, false);
                x += aw;
            }
        }
        r.removeFromBottom (8.0f);
    }

    if (r.getWidth() >= 600.0f && r.getHeight() >= 70.0f)
    {
        // Wide: the keyboard at the right, the estimated notes (with their octaves) at the left.
        const float sideW = juce::jmin (280.0f, r.getWidth() * 0.3f);
        auto right = r.removeFromRight (sideW);
        auto left = r.removeFromLeft (sideW);
        const float kh = juce::jlimit (36.0f, 64.0f, right.getHeight() * 0.45f);
        const float kw = juce::jmin (right.getWidth() - 16.0f, kh * 4.6f);
        const auto keys = right.withSizeKeepingCentre (kw, kh).withX (right.getRight() - kw - 4.0f);
        paintKeyboard (g, keys, accent);

        const auto& est = listener.estimator;
        auto notesArea = left.withSizeKeepingCentre (left.getWidth(), kh + 20.0f);
        Theme::drawCaption (g, "NOTES", notesArea.removeFromTop (16.0f).withTrimmedLeft (4.0f), Palette::muted);
        juce::String notes;
        for (int i = 0; i < est.getNumNotes(); ++i)
            notes << (i > 0 ? "  " : "") << music::noteName (est.getNote (i).midi, paintedFlats);
        g.setFont (Theme::font (13.0f, true));
        g.setColour (notes.isEmpty() ? Palette::faint : Palette::text.withAlpha (0.8f));
        g.drawFittedText (notes.isEmpty() ? juce::String ("--") : notes, notesArea.withTrimmedLeft (4.0f).toNearestInt(),
                          juce::Justification::topLeft, 3, 1.0f);
    }
    else if (showKeyboard)
    {
        const float kh = juce::jlimit (36.0f, 58.0f, h * 0.2f);
        const float kw = juce::jmin (r.getWidth() * 0.7f, kh * 6.2f);
        paintKeyboard (g, r.removeFromBottom (kh).withSizeKeepingCentre (kw, kh), accent);
        r.removeFromBottom (10.0f);
    }

    if (showPills && chord.isChord())
    {
        auto row = r.removeFromBottom (24.0f);
        const auto font = Theme::font (13.0f, true);
        constexpr float kPillW = 40.0f, kGap = 8.0f;
        int count = 0;
        for (int pc = 0; pc < 12; ++pc)
            count += (chord.mask >> pc) & 1;
        float x = row.getCentreX() - (static_cast<float> (count) * (kPillW + kGap) - kGap) * 0.5f;
        g.setFont (font);
        for (int i = 0; i < 12; ++i)
        {
            const int pc = (chord.root + i) % 12;
            if ((chord.mask & (1u << pc)) == 0)
                continue;
            const auto pill = juce::Rectangle<float> (x, row.getY(), kPillW, row.getHeight());
            const bool root = i == 0;
            g.setColour (root ? accent : accent.withAlpha (0.14f));
            g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
            g.setColour (root ? accent : accent.withAlpha (0.55f));
            g.drawRoundedRectangle (pill.reduced (0.5f), pill.getHeight() * 0.5f, 1.0f);
            g.setColour (root ? Palette::well : Palette::text);
            g.drawText (music::pitchClassName (pc, paintedFlats), pill, juce::Justification::centred, false);
            x += kPillW + kGap;
        }
        r.removeFromBottom (8.0f);
    }

    // What it is, under the name.
    auto description = r.removeFromBottom (18.0f);
    g.setFont (Theme::font (13.0f));
    g.setColour (Palette::muted);
    juce::String what = chord.isChord() ? juce::String (music::chordDescription (chord)) : juce::String ("no chord");
    if (chord.isChord() && chord.quality != music::Chord::kSingleNote && chord.bass != chord.root)
        what << dot() << "bass " << music::pitchClassName (chord.bass, paintedFlats);
    g.drawText (what, description, juce::Justification::centred, false);

    // The symbol, cross-fading (and rising a little) on a change.
    const float size = juce::jlimit (26.0f, 88.0f, r.getHeight() * 0.8f);
    const auto nameFont = Theme::font (size, true);
    const float eased = fade * fade * (3.0f - 2.0f * fade);
    const auto drawName = [&] (const char* s, float alpha, float dy, juce::Colour colour)
    {
        if (alpha <= 0.0f || s[0] == '\0')
            return;
        const auto area = r.translated (0.0f, dy);
        g.setFont (nameFont);
        g.setColour (colour.withAlpha (alpha * 0.18f)); // soft halo
        for (const float o : { -2.0f, 2.0f })
            g.drawText (s, area.translated (o, 0.0f), juce::Justification::centred, false);
        g.setColour (colour.withAlpha (alpha));
        g.drawText (s, area, juce::Justification::centred, false);
    };
    const bool previousWasChord = std::strcmp (previousText, "N.C.") != 0;
    drawName (previousText, 1.0f - eased, -8.0f * eased, previousWasChord ? accent : Palette::faint);
    drawName (text, eased, 8.0f * (1.0f - eased), chord.isChord() ? accent : Palette::faint);
}
} // namespace flub::app::ui::vis
