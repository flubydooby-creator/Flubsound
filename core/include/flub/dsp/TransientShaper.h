// Flubsound Pro - level-independent transient shaper (building block).
//
// Two envelope pairs on the (stereo-linked) rectified signal:
//   attack indicator  : dB(env fast-attack) - dB(env slow-attack)   >= 0 at onsets
//                       (attack 0.5 ms vs 20 ms, equal ~60 ms release)
//   sustain indicator : dB(env slow-release) - dB(env fast-release) >= 0 in decays
//                       (release 400 ms vs 40 ms, equal ~1 ms attack)
//   gainDb = attackDb * clamp(attackInd / 6 dB, 0, 1)
//          + sustainDb * clamp(sustainInd / 6 dB, 0, 1)
// The result depends on envelope *shape*, not absolute level, so the same
// setting behaves consistently on quiet and loud material. Gain is smoothed
// (~1 ms) and applied identically to all channels (image-stable).
// Used by ClarityEnhancer (full band "punch / detail") and BassEngine
// (low-band "tighten" = negative sustain). Zero latency.
#pragma once

#include "EnvelopeFollower.h"
#include "flub/common/AudioBlock.h"

namespace flub
{
class TransientShaper
{
public:
    void prepare (double sampleRate) noexcept;
    void reset() noexcept;

    /** -12 .. +12 dB each. RT-safe. */
    void setAttackDb (float db) noexcept;
    void setSustainDb (float db) noexcept;

    /** Returns the linear gain to apply to the current sample, given the
        linked detector input max_c |x_c[n]|. Call exactly once per sample. */
    float computeGain (float linkedAbs) noexcept;

    /** Convenience: full-band in-place processing with linked detection. */
    void process (const AudioBlock& block) noexcept;

    bool isNeutral() const noexcept { return attackDb == 0.0f && sustainDb == 0.0f; }

private:
    // ---- implementation-defined below this line ----
    double sr = 48000.0;
    float attackDb = 0.0f, sustainDb = 0.0f;
};
} // namespace flub
