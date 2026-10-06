#include "MusicTheory.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace flub::app::ui::vis::music
{
namespace
{
const char* const kSharps[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
const char* const kFlats[] = { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };

constexpr uint16_t bits (std::initializer_list<int> intervals) noexcept
{
    uint16_t m = 0;
    for (const int i : intervals)
        m = static_cast<uint16_t> (m | (1u << i));
    return m;
}

struct ChordType
{
    uint16_t intervals;
    const char* suffix;
    const char* description;
};

// In order of preference where two names fit the same notes with the same root choice.
const ChordType kChordTypes[] = {
    { bits ({ 0, 4, 7 }), "", "major triad" },
    { bits ({ 0, 3, 7 }), "m", "minor triad" },
    { bits ({ 0, 4, 7, 10 }), "7", "dominant seventh" },
    { bits ({ 0, 4, 7, 11 }), "maj7", "major seventh" },
    { bits ({ 0, 3, 7, 10 }), "m7", "minor seventh" },
    { bits ({ 0, 7 }), "5", "power chord" },
    { bits ({ 0, 5, 7 }), "sus4", "suspended fourth" },
    { bits ({ 0, 2, 7 }), "sus2", "suspended second" },
    { bits ({ 0, 3, 6 }), "dim", "diminished triad" },
    { bits ({ 0, 4, 8 }), "aug", "augmented triad" },
    { bits ({ 0, 3, 6, 10 }), "m7b5", "half-diminished seventh" },
    { bits ({ 0, 3, 6, 9 }), "dim7", "diminished seventh" },
    { bits ({ 0, 2, 4, 7 }), "add9", "major add nine" },
    { bits ({ 0, 2, 3, 7 }), "m(add9)", "minor add nine" },
    { bits ({ 0, 4, 7, 9 }), "6", "major sixth" },
    { bits ({ 0, 3, 7, 9 }), "m6", "minor sixth" },
    { bits ({ 0, 5, 7, 10 }), "7sus4", "seventh, suspended fourth" },
    { bits ({ 0, 3, 7, 11 }), "m(maj7)", "minor-major seventh" },
    { bits ({ 0, 2, 4, 7, 10 }), "9", "dominant ninth" },
    { bits ({ 0, 2, 4, 7, 11 }), "maj9", "major ninth" },
    { bits ({ 0, 2, 3, 7, 10 }), "m9", "minor ninth" },
    { bits ({ 0, 4, 10 }), "7", "dominant seventh (no fifth)" },
    { bits ({ 0, 4, 11 }), "maj7", "major seventh (no fifth)" },
    { bits ({ 0, 3, 10 }), "m7", "minor seventh (no fifth)" },
    { bits ({ 0, 4 }), "", "major (no fifth)" },
    { bits ({ 0, 3 }), "m", "minor (no fifth)" },
};
constexpr int kNumChordTypes = static_cast<int> (sizeof (kChordTypes) / sizeof (kChordTypes[0]));
constexpr int kNotInBassCost = 100; // a root in the bass beats any template order
constexpr int kDroppedNoteCost = 1000;

int popcount (uint16_t m) noexcept
{
    int n = 0;
    for (; m != 0; m = static_cast<uint16_t> (m & (m - 1)))
        ++n;
    return n;
}

uint16_t rotate (uint16_t mask, int root) noexcept
{
    const unsigned m = mask & 0xfffu;
    return static_cast<uint16_t> (((m >> root) | (m << (12 - root))) & 0xfffu);
}

int wrap (int pc) noexcept
{
    return ((pc % 12) + 12) % 12;
}

/** The best (root, type) for exactly these notes; updates best / bestCost. */
void tryMask (uint16_t mask, int bass, int penalty, Chord& best, int& bestCost) noexcept
{
    for (int root = 0; root < 12; ++root)
    {
        if ((mask & (1u << root)) == 0)
            continue;
        const uint16_t rel = rotate (mask, root);
        for (int t = 0; t < kNumChordTypes; ++t)
        {
            if (kChordTypes[t].intervals != rel)
                continue;
            const int cost = penalty + t + (bass < 0 || root == bass ? 0 : kNotInBassCost);
            if (cost < bestCost)
            {
                bestCost = cost;
                best.root = root;
                best.quality = t;
                best.mask = mask;
            }
        }
    }
}

// Krumhansl-Kessler key profiles (tonic first).
constexpr double kMajorProfile[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
constexpr double kMinorProfile[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
} // namespace

const char* pitchClassName (int pc, bool flats) noexcept
{
    return (flats ? kFlats : kSharps)[wrap (pc)];
}

juce::String noteName (int midi, bool flats)
{
    return juce::String (pitchClassName (midi, flats)) + juce::String (midi / 12 - 1);
}

uint16_t maskOf (std::initializer_list<int> pitchClasses) noexcept
{
    uint16_t m = 0;
    for (const int pc : pitchClasses)
        m = static_cast<uint16_t> (m | (1u << wrap (pc)));
    return m;
}

Chord nameChord (uint16_t mask, int bassPc, bool allowDropping) noexcept
{
    mask = static_cast<uint16_t> (mask & 0xfffu);
    Chord chord;
    if (mask == 0)
        return chord;
    const int bass = bassPc >= 0 && (mask & (1u << wrap (bassPc))) != 0 ? wrap (bassPc) : -1;
    const int count = popcount (mask);
    if (count == 1)
    {
        for (int pc = 0; pc < 12; ++pc)
            if ((mask & (1u << pc)) != 0)
                chord.root = chord.bass = pc;
        chord.quality = Chord::kSingleNote;
        chord.mask = mask;
        return chord;
    }

    int bestCost = INT_MAX;
    tryMask (mask, bass, 0, chord, bestCost);
    // Nothing fits: leave out one note (never the bass), then two.
    for (int drop = 1; allowDropping && drop <= 2 && bestCost == INT_MAX && count - drop >= 2; ++drop)
    {
        for (int a = 0; a < 12; ++a)
        {
            if ((mask & (1u << a)) == 0 || a == bass)
                continue;
            const auto without = static_cast<uint16_t> (mask & ~(1u << a));
            if (drop == 1)
            {
                tryMask (without, bass, kDroppedNoteCost, chord, bestCost);
                continue;
            }
            for (int b = a + 1; b < 12; ++b)
                if ((without & (1u << b)) != 0 && b != bass)
                    tryMask (static_cast<uint16_t> (without & ~(1u << b)), bass, 2 * kDroppedNoteCost, chord, bestCost);
        }
    }
    if (bestCost == INT_MAX)
        return {};
    chord.bass = bass < 0 ? chord.root : bass;
    return chord;
}

void formatChord (const Chord& chord, bool flats, char* out, size_t size) noexcept
{
    if (size == 0)
        return;
    out[0] = '\0';
    const auto append = [&] (const char* text)
    {
        const size_t used = std::strlen (out);
        size_t i = 0;
        for (; text[i] != '\0' && used + i + 1 < size; ++i)
            out[used + i] = text[i];
        out[used + i] = '\0';
    };
    if (! chord.isChord())
    {
        append ("N.C.");
        return;
    }
    append (pitchClassName (chord.root, flats));
    if (chord.quality >= 0 && chord.quality < kNumChordTypes)
        append (kChordTypes[chord.quality].suffix);
    if (chord.quality != Chord::kSingleNote && chord.bass >= 0 && chord.bass != chord.root)
    {
        append ("/");
        append (pitchClassName (chord.bass, flats));
    }
}

juce::String chordText (const Chord& chord, bool flats)
{
    char text[32];
    formatChord (chord, flats, text, sizeof (text));
    return juce::String (text);
}

juce::String chordText (std::initializer_list<int> pitchClasses, int bassPc, bool flats)
{
    return chordText (nameChord (maskOf (pitchClasses), bassPc), flats);
}

const char* chordDescription (const Chord& chord) noexcept
{
    if (chord.quality == Chord::kSingleNote)
        return "single note";
    if (chord.quality >= 0 && chord.quality < kNumChordTypes)
        return kChordTypes[chord.quality].description;
    return "no chord";
}

// =============================================================================
// ChordTracker
// =============================================================================
void ChordTracker::reset() noexcept
{
    presence.fill (0.0f);
    bassPresence.fill (0.0f);
    shown = candidate = Chord {};
    candidateSeconds = 0.0;
    history.fill (Chord {});
    historyNewest = -1;
    historySize = 0;
}

const Chord& ChordTracker::getHistory (int age) const noexcept
{
    static const Chord none;
    if (age < 0 || age >= historySize)
        return none;
    return history[static_cast<size_t> (((historyNewest - age) % kHistory + kHistory) % kHistory)];
}

bool ChordTracker::update (const EstimatedNote* notes, int numNotes, double dtSeconds) noexcept
{
    const double dt = std::clamp (dtSeconds, 0.0, 0.25);
    std::array<float, 12> target {}, bassTarget {};
    float strongest = 0.0f;
    for (int i = 0; i < numNotes; ++i)
        strongest = std::max (strongest, notes[i].salience);
    for (int i = 0; i < numNotes; ++i)
    {
        auto& t = target[static_cast<size_t> (wrap (notes[i].midi))];
        t = std::max (t, std::clamp (notes[i].salience / (0.5f * strongest), 0.0f, 1.0f));
    }
    if (numNotes > 0)
        bassTarget[static_cast<size_t> (wrap (notes[0].midi))] = 1.0f;

    const auto attack = static_cast<float> (1.0 - std::exp (-dt / kAttackSeconds));
    const auto release = static_cast<float> (1.0 - std::exp (-dt / kReleaseSeconds));
    std::array<int, 12> order {};
    int active = 0;
    for (size_t pc = 0; pc < 12; ++pc)
    {
        presence[pc] += (target[pc] - presence[pc]) * (target[pc] > presence[pc] ? attack : release);
        bassPresence[pc] += (bassTarget[pc] - bassPresence[pc]) * (bassTarget[pc] > bassPresence[pc] ? attack : release);
        if (presence[pc] > kActive)
            order[static_cast<size_t> (active++)] = static_cast<int> (pc);
    }
    // Strongest first.
    std::sort (order.begin(), order.begin() + active,
               [this] (int a, int b) { return presence[static_cast<size_t> (a)] > presence[static_cast<size_t> (b)]; });

    // The most of the strongest pitch classes that make a chord.
    Chord now;
    for (int k = std::min (active, 6); k >= 1 && ! now.isChord(); --k)
    {
        uint16_t mask = 0;
        for (int i = 0; i < k; ++i)
            mask = static_cast<uint16_t> (mask | (1u << order[static_cast<size_t> (i)]));
        int bass = -1;
        float bassBest = 0.25f;
        for (int pc = 0; pc < 12; ++pc)
            if ((mask & (1u << pc)) != 0 && bassPresence[static_cast<size_t> (pc)] > bassBest)
            {
                bassBest = bassPresence[static_cast<size_t> (pc)];
                bass = pc;
            }
        now = nameChord (mask, bass, false);
    }
    if (now == shown)
    {
        candidate = now;
        candidateSeconds = 0.0;
        return false;
    }
    if (now != candidate)
    {
        candidate = now;
        candidateSeconds = 0.0;
    }
    candidateSeconds += dt;
    const double hold = now.isChord() ? kHoldSeconds : (active == 0 ? kSilenceHoldSeconds : kUnnamedHoldSeconds);
    if (candidateSeconds + 1.0e-9 < hold)
        return false;
    if (shown.isChord())
    {
        historyNewest = (historyNewest + 1) % kHistory;
        history[static_cast<size_t> (historyNewest)] = shown;
        historySize = std::min (kHistory, historySize + 1);
    }
    shown = now;
    candidateSeconds = 0.0;
    return true;
}

// =============================================================================
// KeyDetector
// =============================================================================
void KeyDetector::reset() noexcept
{
    accumulated.fill (0.0);
    scores.fill (0.0f);
    weight = 0.0;
    key = -1;
    confidence = 0.0f;
}

void KeyDetector::correlate (const double* chroma, float* out) noexcept
{
    double mean = 0.0;
    for (int i = 0; i < 12; ++i)
        mean += chroma[i];
    mean /= 12.0;
    double var = 0.0;
    for (int i = 0; i < 12; ++i)
        var += (chroma[i] - mean) * (chroma[i] - mean);

    for (int mode = 0; mode < 2; ++mode)
    {
        const double* profile = mode == 0 ? kMajorProfile : kMinorProfile;
        double pMean = 0.0;
        for (int i = 0; i < 12; ++i)
            pMean += profile[i];
        pMean /= 12.0;
        double pVar = 0.0;
        for (int i = 0; i < 12; ++i)
            pVar += (profile[i] - pMean) * (profile[i] - pMean);
        for (int tonic = 0; tonic < 12; ++tonic)
        {
            double cov = 0.0;
            for (int i = 0; i < 12; ++i)
                cov += (chroma[(tonic + i) % 12] - mean) * (profile[i] - pMean);
            const double denominator = std::sqrt (var * pVar);
            out[mode * 12 + tonic] = denominator > 1.0e-12 ? static_cast<float> (cov / denominator) : 0.0f;
        }
    }
}

void KeyDetector::add (const float* chroma, bool signal, double dtSeconds) noexcept
{
    const double dt = std::clamp (dtSeconds, 0.0, 0.5);
    const double decay = std::exp (-dt / kWindowSeconds);
    for (auto& a : accumulated)
        a *= decay;
    weight *= decay;
    if (signal)
    {
        for (size_t i = 0; i < 12; ++i)
            accumulated[i] += static_cast<double> (std::max (0.0f, chroma[i])) * dt;
        weight += dt;
    }

    correlate (accumulated.data(), scores.data());
    int best = -1, second = -1;
    for (int k = 0; k < 24; ++k)
    {
        if (best < 0 || scores[static_cast<size_t> (k)] > scores[static_cast<size_t> (best)])
        {
            second = best;
            best = k;
        }
        else if (second < 0 || scores[static_cast<size_t> (k)] > scores[static_cast<size_t> (second)])
        {
            second = k;
        }
    }
    if (weight < 0.25 || scores[static_cast<size_t> (best)] <= 0.0f)
    {
        key = -1;
        confidence = 0.0f;
        return;
    }
    if (key < 0 || scores[static_cast<size_t> (best)] > scores[static_cast<size_t> (key)] + kSwitchMargin)
        key = best;
    // The runner-up of the shown key.
    float runnerUp = -1.0f;
    for (int k = 0; k < 24; ++k)
        if (k != key)
            runnerUp = std::max (runnerUp, scores[static_cast<size_t> (k)]);
    const float r = scores[static_cast<size_t> (key)];
    const float margin = std::clamp ((r - runnerUp) / 0.1f, 0.0f, 1.0f);
    const auto amount = static_cast<float> (std::min (1.0, weight / kFullConfidenceSeconds));
    confidence = std::clamp (r, 0.0f, 1.0f) * (0.55f + 0.45f * margin) * amount;
}

juce::String keyName (int k)
{
    if (k < 0 || k >= 24)
        return {};
    return juce::String (pitchClassName (k % 12, keyPrefersFlats (k))) + (k < 12 ? " major" : " minor");
}

juce::String keyShortName (int k)
{
    if (k < 0 || k >= 24)
        return {};
    return juce::String (pitchClassName (k % 12, keyPrefersFlats (k))) + (k < 12 ? "" : "m");
}

bool keyPrefersFlats (int k) noexcept
{
    if (k < 0 || k >= 24)
        return false;
    const int tonic = k % 12;
    if (k < 12)
        return tonic == 5 || tonic == 10 || tonic == 3 || tonic == 8 || tonic == 1; // F Bb Eb Ab Db
    return tonic == 2 || tonic == 7 || tonic == 0 || tonic == 5 || tonic == 10 || tonic == 3; // D G C F Bb Eb minor
}

uint16_t scaleMask (int k) noexcept
{
    if (k < 0 || k >= 24)
        return 0;
    const uint16_t intervals = k < 12 ? bits ({ 0, 2, 4, 5, 7, 9, 11 }) : bits ({ 0, 2, 3, 5, 7, 8, 10 });
    return rotate (intervals, (12 - k % 12) % 12);
}

int relativeKey (int k) noexcept
{
    if (k < 0 || k >= 24)
        return -1;
    return k < 12 ? 12 + (k + 9) % 12 : (k - 12 + 3) % 12;
}
} // namespace flub::app::ui::vis::music
