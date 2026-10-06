// Flubsound Pro - chord name (visualiser "chord", main view or strip).
//
// The chord that is playing, from the estimated fundamentals of the output
// (PitchEstimator) steadied by music::ChordTracker (a name shows once it has
// held for 0.18 s, "N.C." after 0.8 s without one): a big chord symbol
// ("Am7", "C/E", "Fsus2", "N.C.") that cross-fades on a change, what it is
// ("minor seventh · bass G"), its notes from the root as pills, a one-octave
// keyboard lighting every pitch class that sounds (the root brightest, a dot
// on the bass) and the last chords ("Am › F › C › G"). The key estimate
// (music::KeyDetector) chooses sharps or flats and is shown top right.
// As a strip: the symbol, its notes and the key on one line.
#pragma once

#include "MusicTheory.h"
#include "Visualiser.h"

namespace flub::app::ui::vis
{
class ChordView : public Visualiser
{
public:
    static constexpr double kFadeSeconds = 0.18;

    ChordView();

    void setSampleRate (double sampleRate) override;
    void reset() override;
    void pushPost (const float* mid, const float* side, int numSamples) override;
    void advance (const FrameContext& frame) override;
    int getStripHeight() const override { return 34; }

    const music::Chord& getChord() const noexcept { return tracker.getChord(); }
    const music::ChordTracker& getTracker() const noexcept { return tracker; }
    const music::MusicListener& getListener() const noexcept { return listener; }
    /** The shown symbol, e.g. "Am7" (no allocation: a fixed buffer). */
    const char* getChordText() const noexcept { return text; }

    void paint (juce::Graphics& g) override;

private:
    void paintStrip (juce::Graphics& g, juce::Colour accent);
    void paintKeyboard (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour accent) const;
    bool flats() const noexcept { return music::keyPrefersFlats (listener.key.getKey()); }

    music::MusicListener listener;
    music::ChordTracker tracker;
    char text[32] = "N.C.", previousText[32] = "";
    float fade = 1.0f;
    std::array<float, 12> paintedPresence {};
    int paintedKey = -1;
    float paintedConfidence = 0.0f;
    bool paintedFlats = false;
};
} // namespace flub::app::ui::vis
