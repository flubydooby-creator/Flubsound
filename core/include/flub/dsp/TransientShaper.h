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
#include "Svf.h"
#include "flub/common/AudioBlock.h"
#include "flub/common/Realtime.h"
#include "flub/common/SmoothedValue.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace flub
{
class TransientShaper
{
public:
    void prepare (double sampleRate) noexcept;
    void reset() noexcept FLUB_NONBLOCKING;

    /** -12 .. +12 dB each. RT-safe. */
    void setAttackDb (float db) noexcept FLUB_NONBLOCKING;
    void setSustainDb (float db) noexcept FLUB_NONBLOCKING;

    /** Returns the linear gain to apply to the current sample, given the
        linked detector input max_c |x_c[n]|. Call exactly once per sample. */
    float computeGain (float linkedAbs) noexcept;

    /** Convenience: full-band in-place processing with linked detection. */
    void process (const AudioBlock& block) noexcept;

    bool isNeutral() const noexcept { return attackDb == 0.0f && sustainDb == 0.0f; }

private:
    // ---- implementation-defined below this line ----
    // BassEngine and ClarityEnhancer embed a TransientShaper and reuse the two
    // small helpers below for their own detectors and dynamic filters.
    friend class BassEngine;
    friend class ClarityEnhancer;

    /** Sliding-window peak hold, O(1) per sample: returns the maximum input over
        the last (kBuckets - 1) * length + 1 .. kBuckets * length samples.
        A plain peak follower decays between waveform peaks and so ripples at
        twice the signal frequency; any gain derived from it then modulates the
        audio (audible distortion on bass notes). When the window spans at least
        half a period of the lowest frequency of interest, a steady tone always
        has a waveform peak inside it and the held level is ripple free, while
        a rising level still passes instantly. */
    struct PeakHold
    {
        static constexpr int kBuckets = 4;

        /** The held window always covers at least minWindowMs. */
        void prepare (double sampleRate, double minWindowMs) noexcept
        {
            const double samples = std::max (1.0, sampleRate * minWindowMs * 0.001);
            length = std::max (1, static_cast<int> (std::ceil (samples / static_cast<double> (kBuckets - 1))));
            reset();
        }

        void reset() noexcept
        {
            closed.fill (0.0f);
            current = closedMax = 0.0f;
            countdown = length;
            oldest = 0;
        }

        /** x >= 0. A NaN input is ignored (std::max keeps the first argument). */
        float process (float x) noexcept
        {
            current = std::max (current, x);
            const float held = std::max (current, closedMax);
            if (--countdown == 0)
            {
                closed[static_cast<size_t> (oldest)] = current;
                oldest = oldest + 1 == kBuckets - 1 ? 0 : oldest + 1;
                closedMax = std::max ({ closed[0], closed[1], closed[2] });
                current = 0.0f;
                countdown = length;
            }
            return held;
        }

        std::array<float, kBuckets - 1> closed {};
        float current = 0.0f, closedMax = 0.0f;
        int length = 1, countdown = 1, oldest = 0;
    };
    static_assert (PeakHold::kBuckets == 4, "PeakHold::process() unrolls three closed buckets");

    /** SVF coefficients that glide linearly (g, k, m0..m2 interpolated per
        sample, a1..a3 re-derived) from the previous design to a new one across
        one control interval. Every intermediate set is a valid, stable SVF, so
        dynamic gains move without control-rate zipper noise. */
    struct SvfGlide
    {
        struct Point
        {
            float g = 0.0f, k = 0.0f, m0 = 0.0f, m1 = 0.0f, m2 = 0.0f;
        };

        void setImmediate (const SvfCoeffs& c) noexcept
        {
            end = c;
            ramping = false;
        }

        void glideTo (const SvfCoeffs& c) noexcept
        {
            ramping = c.g != end.g || c.k != end.k || c.m0 != end.m0 || c.m1 != end.m1 || c.m2 != end.m2;
            if (ramping)
            {
                start = { static_cast<float> (end.g), static_cast<float> (end.k), end.m0, end.m1, end.m2 };
                delta = { static_cast<float> (c.g - end.g), static_cast<float> (c.k - end.k),
                          c.m0 - end.m0, c.m1 - end.m1, c.m2 - end.m2 };
            }
            end = c;
        }

        /** Output == input whatever the filter state (0 dB bell / shelf). */
        bool isIdentity() const noexcept { return ! ramping && end.m0 == 1.0f && end.m1 == 0.0f && end.m2 == 0.0f; }

        /** Coefficients at fraction t (0 < t <= 1) of the glide. */
        SvfCoeffs at (float t) const noexcept
        {
            const float g = start.g + delta.g * t;
            const float k = start.k + delta.k * t;
            SvfCoeffs c;
            c.a1 = 1.0f / (1.0f + g * (g + k));
            c.a2 = g * c.a1;
            c.a3 = g * c.a2;
            c.m0 = start.m0 + delta.m0 * t;
            c.m1 = start.m1 + delta.m1 * t;
            c.m2 = start.m2 + delta.m2 * t;
            return c;
        }

        SvfCoeffs end;
        Point start, delta;
        bool ramping = false;
    };

    /** Targets neutral and every smoother landed: computeGain() returns exactly 1. */
    bool isSettled() const noexcept
    {
        return isNeutral() && attackAmount.getCurrent() == 0.0f && sustainAmount.getCurrent() == 0.0f && gainDbState == 0.0f;
    }

    double sr = 48000.0;
    float attackDb = 0.0f, sustainDb = 0.0f;          // targets (what isNeutral() reports)
    PeakHold hold;                                    // ~25 ms: ripple-free level down to 20 Hz
    EnvelopeFollower attackFast, attackSlow;          // 0.5 / 20 ms attack, 60 ms release
    EnvelopeFollower sustainSlow, sustainFast;        // 1 ms attack, 400 / 40 ms release
    OnePoleSmoother attackAmount, sustainAmount;      // smoothed attackDb / sustainDb
    float gainCoeff = 0.0f, gainDbState = 0.0f;       // ~1 ms gain smoothing (dB domain)
};
} // namespace flub
