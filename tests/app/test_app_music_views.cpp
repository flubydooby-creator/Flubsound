// App-level tests: the music views (docs/06 §6.4.2): the multi-pitch
// estimator (fundamentals, not overtones), chord naming, the chord tracker,
// chroma folding, key detection, the analyser's "fundamentals only" piano
// keys and its persistence, and the chord / chromagram / key views fed with
// synthetic harmonic tones.
#include "AppTestSupport.h"

#include "ui/AnalyzerPanel.h"
#include "ui/MeterSnapshot.h"
#include "ui/SpectrumAnalyzer.h"
#include "ui/vis/ChordView.h"
#include "ui/vis/ChromagramView.h"
#include "ui/vis/KeyView.h"
#include "ui/vis/MusicTheory.h"
#include "ui/vis/PitchEstimator.h"
#include "engine/TestSignalGenerator.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

using namespace flub::app;
namespace vis = flub::app::ui::vis;
namespace music = flub::app::ui::vis::music;

#define MUSIC_LOG(expr)                                         \
    do                                                          \
    {                                                           \
        std::ostringstream flubMusicS_;                         \
        flubMusicS_ << expr;                                    \
        std::printf ("  %s\n", flubMusicS_.str().c_str());      \
    } while (0)

namespace
{
constexpr double kPi = juce::MathConstants<double>::pi;
constexpr double kRate = 48000.0;
constexpr int kFrame = 800; // one 60 Hz display frame
constexpr double kDt = kFrame / kRate;

/** Notes (MIDI) with `harmonics` harmonics each: amplitude `level` / h^tilt. */
struct Voicing
{
    std::vector<int> midis;
    int harmonics = 6;
    double level = 0.1, tilt = 1.0;
};

/** Renders `seconds` of the voicing starting at sample `start` (phase continuous across calls). */
std::vector<float> render (const Voicing& v, double seconds, int64_t start = 0)
{
    std::vector<float> out (static_cast<size_t> (seconds * kRate), 0.0f);
    for (const int m : v.midis)
    {
        const double f0 = vis::PitchEstimator::midiToHz (m);
        for (int h = 1; h <= v.harmonics; ++h)
        {
            const double a = v.level / std::pow (static_cast<double> (h), v.tilt);
            const double w = 2.0 * kPi * f0 * h / kRate;
            const double phase0 = 0.7 * h + 0.3 * m; // not all partials in phase
            for (size_t i = 0; i < out.size(); ++i)
                out[i] += static_cast<float> (a * std::sin (w * static_cast<double> (start + static_cast<int64_t> (i)) + phase0));
        }
    }
    return out;
}

std::vector<int> estimate (const Voicing& v)
{
    vis::PitchEstimator e;
    e.setSampleRate (kRate);
    const auto x = render (v, 0.8);
    e.push (x.data(), static_cast<int> (x.size()));
    REQUIRE (e.update (0.0));
    std::vector<int> notes;
    for (int i = 0; i < e.getNumNotes(); ++i)
        notes.push_back (e.getNote (i).midi);
    return notes;
}

std::string describe (const std::vector<int>& notes)
{
    std::string s;
    for (const int m : notes)
        s += music::noteName (m, false).toStdString() + " ";
    return s;
}

/** Feeds a chord progression (each chord `seconds`, `loops` times) frame by frame into a listener. */
void play (music::MusicListener& listener, const std::vector<std::vector<int>>& chords, double seconds, int loops)
{
    int64_t n = 0;
    for (int loop = 0; loop < loops; ++loop)
        for (const auto& chord : chords)
        {
            Voicing v;
            v.midis = chord;
            v.harmonics = 4;
            v.level = 0.08;
            const auto x = render (v, seconds, n);
            n += static_cast<int64_t> (x.size());
            for (size_t i = 0; i + kFrame <= x.size(); i += kFrame)
            {
                listener.push (x.data() + i, kFrame);
                listener.advance (kDt);
            }
        }
}
} // namespace

// =============================================================================
// Estimator
// =============================================================================
TEST_CASE ("App: pitch estimator names single harmonic notes by their fundamentals, not their overtones")
{
    for (const int midi : { 36, 57, 76 }) // C2, A3, E5
    {
        Voicing v;
        v.midis = { midi };
        v.harmonics = 6;
        v.tilt = 0.0; // six equally loud harmonics: the hardest case for octave errors
        const auto notes = estimate (v);
        MUSIC_LOG ("note " << midi << ": " << describe (notes));
        REQUIRE (notes.size() == 1);
        CHECK (notes[0] == midi);
    }
}

TEST_CASE ("App: pitch estimator finds the notes of a C major triad and an A minor 7 chord")
{
    Voicing triad;
    triad.midis = { 60, 64, 67 }; // C4 E4 G4
    const auto c = estimate (triad);
    MUSIC_LOG ("C major: " << describe (c));
    CHECK ((c == std::vector<int> { 60, 64, 67 }));

    Voicing am7;
    am7.midis = { 57, 60, 64, 67 }; // A3 C4 E4 G4
    const auto a = estimate (am7);
    MUSIC_LOG ("Am7: " << describe (a));
    CHECK ((a == std::vector<int> { 57, 60, 64, 67 }));

    // With a bass note two octaves down.
    Voicing withBass;
    withBass.midis = { 45, 60, 64, 67 }; // A2 + C major triad = Am7 voicing
    const auto b = estimate (withBass);
    MUSIC_LOG ("A2 + C4 E4 G4: " << describe (b));
    CHECK ((b == std::vector<int> { 45, 60, 64, 67 }));
}

TEST_CASE ("App: pitch estimator reads silence as no notes and clears after the stream stops")
{
    vis::PitchEstimator e;
    e.setSampleRate (kRate);
    std::vector<float> silence (static_cast<size_t> (kRate), 0.0f);
    e.push (silence.data(), static_cast<int> (silence.size()));
    e.update (0.0);
    CHECK (e.getNumNotes() == 0);
    CHECK (! e.hasSignal());

    Voicing v;
    v.midis = { 57 };
    const auto x = render (v, 0.6);
    e.push (x.data(), static_cast<int> (x.size()));
    CHECK (e.update (0.0));
    CHECK (e.getNumNotes() == 1);
    for (int i = 0; i < 30; ++i) // no more samples: cleared after kIdleSeconds
        e.update (kDt);
    CHECK (e.getNumNotes() == 0);
    CHECK (! e.hasSignal());
}

TEST_CASE ("App: chroma folds octaves onto one pitch class (440 Hz and 880 Hz are A)")
{
    vis::PitchEstimator e;
    e.setSampleRate (kRate);
    std::vector<float> x (static_cast<size_t> (0.8 * kRate));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float> (0.3 * std::sin (2.0 * kPi * 440.0 * static_cast<double> (i) / kRate)
                                   + 0.2 * std::sin (2.0 * kPi * 880.0 * static_cast<double> (i) / kRate));
    e.push (x.data(), static_cast<int> (x.size()));
    REQUIRE (e.update (0.0));
    REQUIRE (e.hasSignal());
    const auto& chroma = e.getChroma();
    CHECK (chroma[9] == 1.0f);
    for (int pc = 0; pc < 12; ++pc)
        if (pc != 9)
        {
            CHECK (chroma[static_cast<size_t> (pc)] < 0.05f);
        }

    // The pure fold: a peak a quarter tone above A splits between A and A#.
    const vis::SpectralPeak peaks[] = { { 440.0f * std::pow (2.0f, 0.5f / 12.0f), 1.0f } };
    float folded[12] {};
    REQUIRE (vis::PitchEstimator::foldChroma (peaks, 1, folded));
    CHECK (std::abs (folded[9] - 1.0f) < 1.0e-4f);
    CHECK (std::abs (folded[10] - 1.0f) < 1.0e-3f);
    CHECK (folded[8] == 0.0f);
    CHECK (! vis::PitchEstimator::foldChroma (peaks, 0, folded));
}

// =============================================================================
// Chord names
// =============================================================================
TEST_CASE ("App: chord names from pitch-class sets, slash chords for inversions")
{
    using music::chordText;
    // Triads, sus, power chords.
    CHECK (chordText ({ 0, 4, 7 }, 0) == "C");
    CHECK (chordText ({ 9, 0, 4 }, 9) == "Am");
    CHECK (chordText ({ 11, 2, 5 }, 11) == "Bdim");
    CHECK (chordText ({ 0, 4, 8 }, 0) == "Caug");
    CHECK (chordText ({ 2, 7, 9 }, 2) == "Dsus4");
    CHECK (chordText ({ 2, 4, 9 }, 2) == "Dsus2");
    CHECK (chordText ({ 4, 11 }, 4) == "E5");
    // Sevenths, sixths, add9, ninths.
    CHECK (chordText ({ 7, 11, 2, 5 }, 7) == "G7");
    CHECK (chordText ({ 0, 4, 7, 11 }, 0) == "Cmaj7");
    CHECK (chordText ({ 9, 0, 4, 7 }, 9) == "Am7");
    CHECK (chordText ({ 11, 2, 5, 9 }, 11) == "Bm7b5");
    CHECK (chordText ({ 0, 3, 6, 9 }, 0) == "Cdim7");
    CHECK (chordText ({ 0, 4, 7, 2 }, 0) == "Cadd9");
    CHECK (chordText ({ 9, 0, 4, 11 }, 9) == "Am(add9)");
    CHECK (chordText ({ 7, 0, 2, 5 }, 7) == "G7sus4");
    CHECK (chordText ({ 0, 4, 7, 10, 2 }, 0) == "C9");
    CHECK (chordText ({ 0, 4, 7, 10 }, 0) == "C7");
    CHECK (chordText ({ 0, 4, 10 }, 0) == "C7"); // no fifth
    // The bass decides between names of the same notes: C6 or Am7.
    CHECK (chordText ({ 0, 4, 7, 9 }, 0) == "C6");
    CHECK (chordText ({ 0, 4, 7, 9 }, 9) == "Am7");
    // Inversions as slash chords.
    CHECK (chordText ({ 0, 4, 7 }, 4) == "C/E");
    CHECK (chordText ({ 0, 4, 7 }, 7) == "C/G");
    CHECK (chordText ({ 9, 0, 4, 7 }, 7) == "Am7/G");
    CHECK (chordText ({ 5, 9, 0 }, 9) == "F/A");
    // Without a bass: the plain chord.
    CHECK (chordText ({ 0, 4, 7 }, -1) == "C");
    // Flats.
    CHECK (chordText ({ 10, 2, 5 }, 10, true) == "Bb");
    CHECK (chordText ({ 3, 6, 10 }, 6, true) == "Ebm/Gb");
    CHECK (chordText ({ 10, 2, 5 }, 10, false) == "A#");
    // A lone note, nothing, and a cluster named without its odd note.
    CHECK (chordText ({ 9 }, 9) == "A");
    CHECK (chordText ({}, -1) == "N.C.");
    CHECK (chordText ({ 0, 4, 7, 1 }, 0) == "C"); // C major + a stray C#
    CHECK (chordText ({ 0, 1 }, 0) == "N.C.");
    CHECK (music::nameChord (0, -1).isChord() == false);
    CHECK (juce::String (music::chordDescription (music::nameChord (music::maskOf ({ 9, 0, 4, 7 }), 9))) == "minor seventh");
}

TEST_CASE ("App: chord tracker holds a chord through short dropouts and names a change only once it held")
{
    music::ChordTracker t;
    const auto notes = [] (std::initializer_list<int> midis)
    {
        std::vector<vis::EstimatedNote> v;
        for (const int m : midis)
            v.push_back ({ m, 1.0f, 1.0f });
        return v;
    };
    const auto am = notes ({ 45, 60, 64, 69 });
    const auto f = notes ({ 41, 60, 65, 69 });
    for (int i = 0; i < 30; ++i)
        t.update (am.data(), static_cast<int> (am.size()), kDt);
    CHECK (music::chordText (t.getChord()) == "Am");

    // Two frames of nothing (a dropout) change nothing.
    t.update (nullptr, 0, kDt);
    t.update (nullptr, 0, kDt);
    for (int i = 0; i < 5; ++i)
        t.update (am.data(), static_cast<int> (am.size()), kDt);
    CHECK (music::chordText (t.getChord()) == "Am");
    CHECK (t.getHistorySize() == 0);

    // F: not after 3 frames, yes after 2/3 of a second; Am goes to the history.
    for (int i = 0; i < 3; ++i)
        t.update (f.data(), static_cast<int> (f.size()), kDt);
    CHECK (music::chordText (t.getChord()) == "Am");
    for (int i = 0; i < 40; ++i)
        t.update (f.data(), static_cast<int> (f.size()), kDt);
    CHECK (music::chordText (t.getChord()) == "F");
    REQUIRE (t.getHistorySize() == 1);
    CHECK (music::chordText (t.getHistory (0)) == "Am");

    // Silence: N.C. after the longer hold.
    for (int i = 0; i < 20; ++i)
        t.update (nullptr, 0, kDt);
    CHECK (music::chordText (t.getChord()) == "F");
    for (int i = 0; i < 60; ++i)
        t.update (nullptr, 0, kDt);
    CHECK (music::chordText (t.getChord()) == "N.C.");
    t.reset();
    CHECK (t.getHistorySize() == 0);
}

// =============================================================================
// Key
// =============================================================================
TEST_CASE ("App: key detection: C major I-IV-V-I")
{
    music::MusicListener listener;
    listener.setSampleRate (kRate);
    play (listener, { { 48, 60, 64, 67 }, { 41, 60, 65, 69 }, { 43, 59, 62, 67 }, { 48, 60, 64, 67 } }, 1.0, 2);
    MUSIC_LOG ("key " << music::keyName (listener.key.getKey()) << " confidence " << listener.key.getConfidence());
    CHECK (listener.key.getKey() == 0);
    CHECK (listener.key.getConfidence() > 0.5f);
    CHECK (music::keyName (listener.key.getKey()) == "C major");
    listener.reset();
    CHECK (listener.key.getKey() == -1);
}

TEST_CASE ("App: key detection: A minor i-iv-v")
{
    music::MusicListener listener;
    listener.setSampleRate (kRate);
    play (listener, { { 45, 57, 60, 64 }, { 50, 57, 62, 65 }, { 52, 55, 59, 64 } }, 1.0, 3);
    MUSIC_LOG ("key " << music::keyName (listener.key.getKey()) << " confidence " << listener.key.getConfidence());
    CHECK (listener.key.getKey() == 12 + 9);
    CHECK (music::keyName (listener.key.getKey()) == "A minor");
    CHECK (music::keyShortName (listener.key.getKey()) == "Am");
}

TEST_CASE ("App: key helpers: profiles, names, scales, relative keys and flat spelling")
{
    // A chroma that is exactly the C major profile correlates 1 with C major.
    const double cMajor[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    float scores[24] {};
    music::KeyDetector::correlate (cMajor, scores);
    CHECK (std::abs (scores[0] - 1.0f) < 1.0e-5f);
    for (int k = 1; k < 24; ++k)
        CHECK (scores[k] < 0.9f);

    CHECK (music::keyName (5 + 12) == "F minor");
    CHECK (music::keyName (3) == "Eb major");
    CHECK (music::keyName (-1).isEmpty());
    CHECK (music::relativeKey (0) == 21);
    CHECK (music::relativeKey (21) == 0);
    CHECK (music::scaleMask (0) == music::maskOf ({ 0, 2, 4, 5, 7, 9, 11 }));
    CHECK (music::scaleMask (21) == music::maskOf ({ 9, 11, 0, 2, 4, 5, 7 }));
    CHECK (music::scaleMask (7) == music::maskOf ({ 7, 9, 11, 0, 2, 4, 6 }));
    CHECK (music::keyPrefersFlats (5 + 12));
    CHECK (! music::keyPrefersFlats (9 + 12));
    CHECK (! music::keyPrefersFlats (-1));
}

// =============================================================================
// Analyser: fundamentals-only piano keys, persistence
// =============================================================================
TEST_CASE ("App: analyser piano keys light only the fundamental with Fundamentals only")
{
    ui::SpectrumAnalyzer a;
    a.setBounds (0, 0, 900, 300);
    a.setSampleRate (kRate);
    a.setPianoKeysEnabled (true);
    a.setFundamentalsOnly (true);
    REQUIRE (a.getPitchEstimator() != nullptr);

    Voicing v;
    v.midis = { 36 }; // C2 with 6 harmonics: C3, G3, C4, E4, G4 are overtones
    v.tilt = 0.5;
    const auto x = render (v, 1.0);
    for (size_t i = 0; i + kFrame <= x.size(); i += kFrame)
    {
        a.push (true, x.data() + i, kFrame);
        a.push (false, x.data() + i, kFrame);
        a.advance (kDt);
    }
    CHECK (a.getKeyGlow (36) > 0.5f);
    for (const int overtone : { 48, 55, 60, 64, 67 })
    {
        CHECK (a.getKeyGlow (overtone) == 0.0f);
    }
}

TEST_CASE ("App: analyser options persist Fundamentals only in the piano keys field, older strings read it off")
{
    using O = ui::AnalyzerPanel::Options;
    O o;
    o.pianoKeys = true;
    o.fundamentals = true;
    CHECK (o.toString() == "1,1,1,1,12,0,0,0,3,0,spectrum,none,0");
    O back;
    REQUIRE (O::fromString (o.toString(), back));
    CHECK (back.pianoKeys);
    CHECK (back.fundamentals);

    // Off but remembered.
    o.pianoKeys = false;
    REQUIRE (O::fromString (o.toString(), back));
    CHECK (! back.pianoKeys);
    CHECK (back.fundamentals);
    CHECK (back.toString() == "1,1,1,1,12,0,0,0,2,0,spectrum,none,0");

    // The strings of the previous versions: keys on, fundamentals off.
    REQUIRE (O::fromString ("1,1,1,1,12,0,0,0,1,0,spectrum,none,0", back));
    CHECK (back.pianoKeys);
    CHECK (! back.fundamentals);
    REQUIRE (O::fromString ("1,1,1,1,12,0,0,0,1,0", back));
    CHECK (back.pianoKeys);
    CHECK (! back.fundamentals);
    REQUIRE (O::fromString ("1,1,1,1,12", back));
    CHECK (! back.pianoKeys);
    CHECK (! back.fundamentals);
}

TEST_CASE ("App: analyser panel applies Fundamentals only to the analyser")
{
    ui::AnalyzerPanel panel ([] { return nullptr; });
    auto o = panel.getOptions();
    o.pianoKeys = true;
    o.fundamentals = true;
    panel.setOptions (o);
    CHECK (panel.getAnalyzer().isFundamentalsOnly());
    o.fundamentals = false;
    panel.setOptions (o);
    CHECK (! panel.getAnalyzer().isFundamentalsOnly());
}

// =============================================================================
// Views
// =============================================================================
TEST_CASE ("App: chord, chromagram and key views follow a synthetic Am7")
{
    ui::MeterSnapshot m;
    vis::ChordView chord;
    vis::ChromagramView chroma;
    vis::KeyView key;
    Voicing v;
    v.midis = { 45, 60, 64, 67 }; // A2 C4 E4 G4
    v.harmonics = 5;
    const auto x = render (v, 1.2);
    std::vector<float> side (kFrame, 0.0f);
    for (auto* view : std::initializer_list<vis::Visualiser*> { &chord, &chroma, &key })
    {
        view->setSampleRate (kRate);
        view->setBounds (0, 0, 800, 260);
        for (size_t i = 0; i + kFrame <= x.size(); i += kFrame)
        {
            view->pushPost (x.data() + i, side.data(), kFrame);
            view->advance (vis::FrameContext { m, kDt, kRate });
        }
    }
    CHECK (std::string (chord.getChordText()) == "Am7");
    CHECK (chord.getChord().bass == 9);

    // A is the strongest pitch class in the chroma (bass + overtones), the B-flat next to it nearly empty.
    int strongest = 0;
    for (int pc = 1; pc < 12; ++pc)
        if (chroma.getBar (pc) > chroma.getBar (strongest))
            strongest = pc;
    CHECK ((strongest == 9 || strongest == 4)); // A or E (the fifth's overtones add up)
    CHECK (chroma.getBar (10) < 0.2f);
    CHECK (chroma.getHistoryFilled() > 10);
    CHECK (chroma.getHistory (0, 9) > 0.5f);

    CHECK (key.getDetector().getSignalSeconds() > 0.5);
    key.reset();
    CHECK (key.getDetector().getKey() == -1);
    chord.reset();
    CHECK (std::string (chord.getChordText()) == "N.C.");
}

TEST_CASE ("App: the app's own test music (Am - F - C - G with drums) reads its chords and A minor")
{
    // The screenshot scene's programme: kick, snare (a 185 Hz tone), hats, a
    // bass line with octaves and fifths, and a quiet pad playing the chords.
    const int n = static_cast<int> (8.5 * kRate);
    std::vector<float> l (static_cast<size_t> (n)), r (static_cast<size_t> (n)), mid (static_cast<size_t> (n));
    float* chans[2] = { l.data(), r.data() };
    flub::app::TestSignalGenerator gen (kRate);
    gen.setProgramme (0, flub::app::TestSignalGenerator::Programme::Music);
    REQUIRE (gen.renderStrip (0, flub::AudioBlock (chans, 2, n)));
    for (size_t i = 0; i < mid.size(); ++i)
        mid[i] = 0.5f * (l[i] + r[i]);

    music::MusicListener listener;
    listener.setSampleRate (kRate);
    music::ChordTracker tracker;
    std::string shown;
    for (int i = 0; i + kFrame <= n; i += kFrame)
    {
        listener.push (mid.data() + i, kFrame);
        listener.advance (kDt);
        const auto& e = listener.estimator;
        if (tracker.update (&e.getNote (0), e.getNumNotes(), kDt))
            shown += music::chordText (tracker.getChord()).toStdString() + " ";
    }
    MUSIC_LOG ("chords shown: " << shown << "| key " << music::keyName (listener.key.getKey()) << " " << listener.key.getConfidence());
    // The pad's chords in order (passing names such as Am6 or Fmaj7 may come between).
    const auto at = [&shown] (const char* chord, size_t from) { return shown.find (std::string (chord) + " ", from); };
    const auto am = at ("Am", 0);
    REQUIRE (am != std::string::npos);
    const auto f = at ("F", am);
    REQUIRE (f != std::string::npos);
    CHECK (at ("C", f) != std::string::npos);
    CHECK (listener.key.getKey() == 12 + 9);
    CHECK (listener.key.getConfidence() > 0.5f);
}
