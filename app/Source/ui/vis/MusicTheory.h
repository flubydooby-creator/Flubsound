// Flubsound Pro - music theory for the music views (docs/06 §6.4.2).
//
//   nameChord / formatChord: a set of pitch classes (and the bass) as a chord
//     symbol: triads (maj, m, dim, aug), sus2 / sus4, power chords (5), 6ths,
//     7ths (7, maj7, m7, m7b5, dim7, m(maj7), 7sus4), add9 / m(add9), 9ths
//     (9, maj9, m9), 7ths and triads without their fifth, inversions as slash
//     chords ("C/E"); a lone note is its name, nothing "N.C.". When several
//     roots fit the same set (C6 = Am7) the one in the bass wins, then the
//     simpler chord. A set no template fits is named without its least
//     needed notes (one, then two, never the bass), else "N.C.".
//   ChordTracker: per-frame estimated notes -> a steady chord. Every pitch
//     class's presence follows its notes' salience (full from half the
//     strongest note's) with kAttackSeconds / kReleaseSeconds, so a note the
//     drums make up in one frame and not the next stays low. The present
//     pitch classes (> kActive) are named strongest first: the most of them
//     (up to 6) that make a chord, so a stray note is left out rather than
//     spoiling the name. A new name is shown once it has held for
//     kHoldSeconds; "N.C." once nothing sounds for kSilenceHoldSeconds, or
//     after kUnnamedHoldSeconds of notes no chord fits (until then the last
//     chord stays). Keeps the last kHistory chords.
//   KeyDetector: Krumhansl-Schmuckler key finding. The chroma is accumulated
//     in a leaky window (time constant kWindowSeconds) and correlated
//     (Pearson) with the 24 rotated Krumhansl-Kessler major / minor profiles;
//     the best one is the key (a new key must beat the shown one by
//     kSwitchMargin). Confidence: the best correlation x how far it stands
//     above the runner-up x how much signal the window holds.
// No allocation except in the juce::String helpers (paint / tests only).
#pragma once

#include "PitchEstimator.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace flub::app::ui::vis::music
{
/** "C", "C#" / "Db", ... for a pitch class (wrapped to 0..11). */
const char* pitchClassName (int pc, bool flats) noexcept;
/** "A3" for MIDI 57. */
juce::String noteName (int midi, bool flats);

struct Chord
{
    static constexpr int kNone = -1, kSingleNote = -2;
    int root = -1;      // pitch class; -1: no chord ("N.C.")
    int bass = -1;      // pitch class of the lowest note; a slash chord when != root
    int quality = kNone; // index into the chord table, kSingleNote, or kNone
    uint16_t mask = 0;  // the pitch classes the name covers (bit pc)

    bool isChord() const noexcept { return root >= 0; }
    bool operator== (const Chord& other) const noexcept
    {
        return root == other.root && bass == other.bass && quality == other.quality && mask == other.mask;
    }
    bool operator!= (const Chord& other) const noexcept { return ! (*this == other); }
};

uint16_t maskOf (std::initializer_list<int> pitchClasses) noexcept;
/** allowDropping: when no chord has exactly these notes, try without one, then two (never the bass). */
Chord nameChord (uint16_t mask, int bassPc, bool allowDropping = true) noexcept;
/** "Am7/G", "N.C.": at most size - 1 characters, no allocation. */
void formatChord (const Chord& chord, bool flats, char* out, size_t size) noexcept;
juce::String chordText (const Chord& chord, bool flats = false);
/** Shortcut for tests: chordText (nameChord (maskOf (pcs), bass)). */
juce::String chordText (std::initializer_list<int> pitchClasses, int bassPc, bool flats = false);
/** "minor seventh", "major triad", "single note", "no chord". */
const char* chordDescription (const Chord& chord) noexcept;

// ---- ChordTracker ----------------------------------------------------------------------
class ChordTracker
{
public:
    static constexpr double kAttackSeconds = 0.12, kReleaseSeconds = 0.25;
    static constexpr double kHoldSeconds = 0.25, kSilenceHoldSeconds = 0.8, kUnnamedHoldSeconds = 3.0;
    static constexpr float kActive = 0.4f;
    static constexpr int kHistory = 8;

    void reset() noexcept;
    /** One display frame's notes (lowest first); true when the shown chord changed. */
    bool update (const EstimatedNote* notes, int numNotes, double dtSeconds) noexcept;

    const Chord& getChord() const noexcept { return shown; }
    /** 0..1 per pitch class. */
    float getPresence (int pc) const noexcept { return presence[static_cast<size_t> (((pc % 12) + 12) % 12)]; }
    /** Chords shown before the current one (0 = the previous one). */
    int getHistorySize() const noexcept { return historySize; }
    const Chord& getHistory (int age) const noexcept;

private:
    std::array<float, 12> presence {}, bassPresence {};
    Chord shown, candidate;
    double candidateSeconds = 0.0;
    std::array<Chord, kHistory> history {};
    int historyNewest = -1, historySize = 0;
};

// ---- KeyDetector ---------------------------------------------------------------------------
class KeyDetector
{
public:
    static constexpr double kWindowSeconds = 15.0;
    static constexpr double kFullConfidenceSeconds = 6.0; // signal needed for full confidence
    static constexpr float kSwitchMargin = 0.015f;

    void reset() noexcept;
    /** One analysis: the chroma (12, any scale) while there is signal, over dtSeconds. */
    void add (const float* chroma, bool signal, double dtSeconds) noexcept;

    /** -1: none yet; 0..11: major key on that tonic; 12..23: minor key on tonic key - 12. */
    int getKey() const noexcept { return key; }
    float getConfidence() const noexcept { return confidence; }
    float getScore (int k) const noexcept { return scores[static_cast<size_t> (k)]; }
    /** Seconds of signal in the window (decaying). */
    double getSignalSeconds() const noexcept { return weight; }

    /** Pearson correlation of a chroma with the 24 key profiles (pure). */
    static void correlate (const double* chroma, float* scores24) noexcept;

private:
    std::array<double, 12> accumulated {};
    std::array<float, 24> scores {};
    double weight = 0.0;
    int key = -1;
    float confidence = 0.0f;
};

/** What the music views share: the estimator fed with the post mid signal and
    a key detector fed with its chroma after every analysis. */
struct MusicListener
{
    PitchEstimator estimator;
    KeyDetector key;
    double sinceAnalysis = 0.0;

    void setSampleRate (double rate) { estimator.setSampleRate (rate); }
    void reset() noexcept
    {
        estimator.reset();
        key.reset();
        sinceAnalysis = 0.0;
    }
    void push (const float* mid, int numSamples) noexcept { estimator.push (mid, numSamples); }
    /** Once per frame; true when a new analysis (or the idle clear) came in. */
    bool advance (double dtSeconds) noexcept
    {
        sinceAnalysis += dtSeconds;
        if (! estimator.update (dtSeconds))
            return false;
        key.add (estimator.getChroma().data(), estimator.hasSignal(), sinceAnalysis);
        sinceAnalysis = 0.0;
        return true;
    }
};

/** "A minor", "Eb major"; "" for -1. */
juce::String keyName (int key);
/** Short form: "Am", "Eb". */
juce::String keyShortName (int key);
/** Flat keys (F, Bb, Eb, Ab, Db major; D, G, C, F, Bb, Eb minor) spell with flats. */
bool keyPrefersFlats (int key) noexcept;
/** The key's scale (major / natural minor) as a pitch-class mask; 0 for -1. */
uint16_t scaleMask (int key) noexcept;
/** The relative major / minor. */
int relativeKey (int key) noexcept;
} // namespace flub::app::ui::vis::music
