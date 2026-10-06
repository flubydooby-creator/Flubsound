// Flubsound Pro - song key (visualiser "key", a strip).
//
// The estimated key of what is playing ("F minor · 82 %"): the output's
// chroma (PitchEstimator) accumulated over a ~15 s leaky window and matched
// against the Krumhansl-Kessler major / minor profiles (music::KeyDetector).
// Shows the key, its confidence as a bar and a percentage, the relative key
// and the scale's notes (tonic in the accent); "listening" until there is
// enough signal. Starts afresh on reset() (strip switch, engine rebuilt).
// Kept fed while hidden (keepsHistory): the window carries on.
#pragma once

#include "MusicTheory.h"
#include "Visualiser.h"

namespace flub::app::ui::vis
{
class KeyView : public Visualiser
{
public:
    KeyView();

    void setSampleRate (double sampleRate) override;
    void reset() override;
    void pushPost (const float* mid, const float* side, int numSamples) override;
    void advance (const FrameContext& frame) override;
    bool keepsHistory() const override { return true; }
    int getStripHeight() const override { return 34; }

    const music::KeyDetector& getDetector() const noexcept { return listener.key; }

    void paint (juce::Graphics& g) override;

private:
    music::MusicListener listener;
    int paintedKey = -2;
    float paintedConfidence = -1.0f, paintedProgress = -1.0f;
};
} // namespace flub::app::ui::vis
