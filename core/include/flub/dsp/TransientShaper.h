// Flubsound Pro - level-independent transient shaper (building block).
//
// Two envelope pairs on the (stereo-linked) rectified signal:
//   attack indicator  : dB(env fast-attack) - dB(env slow-attack)   >= 0 at onsets
//                       (attack 0.5 ms vs 20 ms, equal ~60 ms release)
//   sustain indicator : dB(env slow-release) - dB(env fast-release) >= 0 in decays
//                       (release 400 ms vs 40 ms, equal ~1 ms attack)
//   gainDb = attackDb * clamp(attackInd / 6 dB, 0, 1)
//          + sustainDb * clamp(sustainInd / 6 dB, 0, 1) [* (1 - onset weight), gated]
// The result depends on envelope *shape*, not absolute level, so the same
// setting behaves consistently on quiet and loud material. Gain is smoothed
// (~1 ms) and applied identically to all channels (image-stable).
// Used by ClarityEnhancer (full band "punch / detail", and one per band of
// its 3-band path) and BassEngine (low-band "tighten" = negative sustain).
// Zero latency.
//
// Timing (docs/11 E04 step 3): the hold, the slow attack, the attack pair's
// release and the gain smoothing can be set per instance; the defaults are
// the full-band timing above. A band shaper can also release its attack
// pair program-dependently (fast after a real decay or gap, slow on a
// sustained sound's shallow dips) and scale its timing with a speed.
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
    /** Detector timing (docs/11 E04 step 3). The defaults are the full-band
        shaper's timing, which Clarity without band offsets and BassEngine's
        Tighten run (bit-exact with the shaper before the struct existed). */
    struct Timing
    {
        double holdMs = 25.0;          // peak-hold window: >= half the rectified period of the lowest steady note
        float slowAttackMs = 20.0f;    // A_slow's attack: how long an onset reads as one
        float attackReleaseMs = 60.0f; // the attack pair's release
        /** > 0: program-dependent release. The attack pair releases at this
            time constant once the held level has fallen 6 dB under A_slow (a
            decaying hit, a gap), at attackReleaseMs on shallower dips (the
            level changes of a sustained sound), interpolated in between. */
        float fastReleaseMs = 0.0f;
        float gainSmoothMs = 1.0f;     // symmetric smoothing of the gain in dB

        /** The 3-band path of ClarityEnhancer (docs/11 E04 step 3): the band
            below the 60 - 200 Hz split (25 ms anti-ripple hold), the band up
            to 4 kHz (a hold covering a third of the split frequency, the
            lowest content its LR4 slope still passes at -38 dB) and the band
            above (3 ms hold). */
        static Timing lowBand() noexcept;
        static Timing midBand (double splitHz) noexcept;
        static Timing highBand() noexcept;
    };

    /** Not RT-safe in general (the hold is re-sized): call before prepare()
        or re-prepare. prepare() keeps the timing. */
    void setTiming (const Timing& t) noexcept { timing = t; }
    const Timing& getTiming() const noexcept { return timing; }

    void prepare (double sampleRate) noexcept;
    void reset() noexcept FLUB_NONBLOCKING;

    /** Scales the slow attack and both releases by 1 / speed (0.5 .. 2;
        1 = the timing as set). NaN is ignored. RT-safe; the envelopes keep
        their state, so a change never steps the gain. */
    void setSpeed (float speed) noexcept FLUB_NONBLOCKING;
    /** Re-sizes the hold window without clearing it (the held value never
        steps; the new window applies as buckets close). RT-safe. */
    void setHoldMs (double holdMs) noexcept FLUB_NONBLOCKING;

    /** -12 .. +12 dB each. RT-safe. */
    void setAttackDb (float db) noexcept FLUB_NONBLOCKING;
    void setSustainDb (float db) noexcept FLUB_NONBLOCKING;
    /** Sustain gated by the onset: the sustain gain is scaled by (1 - the
        held level's rise over the slow attack envelope, 0..1 over 6 dB), so
        it never acts during an onset (Tighten, docs/11 E04 step 2). Default
        off (Clarity's sustain as before). */
    void setSustainGatedByAttack (bool gated) noexcept FLUB_NONBLOCKING { sustainGated = gated; }

    /** Returns the linear gain to apply to the current sample, given the
        linked detector input max_c |x_c[n]|. Call exactly once per sample. */
    float computeGain (float linkedAbs) noexcept;

    /** The attack indicator alone (0 .. 1: the fast attack envelope's rise
        over the slow one, full at 6 dB), for a detector that keys something
        else on onsets (BassEngine's Impact punch, docs/11 E20). Runs the
        hold and the attack pair only; call it instead of computeGain(),
        once per sample. RT-safe. */
    float computeOnset (float linkedAbs) noexcept FLUB_NONBLOCKING;

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

        /** Changes the window without clearing it: the buckets already
            closed keep their maxima, so the held value never steps. */
        void resize (double sampleRate, double minWindowMs) noexcept
        {
            const double samples = std::max (1.0, sampleRate * minWindowMs * 0.001);
            length = std::max (1, static_cast<int> (std::ceil (samples / static_cast<double> (kBuckets - 1))));
            countdown = std::min (countdown, length);
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

    void updateTimes() noexcept;
    /** The hold and the attack pair for one detector sample (both paths). */
    void updateAttackPair (float held, float& aFast, float& aSlow) noexcept;

    double sr = 48000.0;
    Timing timing;
    float speed = 1.0f;
    float attackDb = 0.0f, sustainDb = 0.0f;          // targets (what isNeutral() reports)
    bool sustainGated = false;                        // sustain weight x (1 - onset weight)
    PeakHold hold;                                    // ~25 ms: ripple-free level down to 20 Hz
    EnvelopeFollower attackFast, attackSlow;          // 0.5 / 20 ms attack, 60 ms release
    EnvelopeFollower sustainSlow, sustainFast;        // 1 ms attack, 400 / 40 ms release
    OnePoleSmoother attackAmount, sustainAmount;      // smoothed attackDb / sustainDb
    float gainCoeff = 0.0f, gainDbState = 0.0f;       // ~1 ms gain smoothing (dB domain)
    float gatedReturnCoeff = 0.0f;                    // 0.5 ms: a gated sustain cut returning
    // Program-dependent release (timing.fastReleaseMs > 0): the attack pair
    // as plain one-poles, since the release coefficient moves per sample.
    bool programRelease = false;
    float progFast = 0.0f, progSlow = 0.0f;           // A_fast / A_slow
    float progFastAttack = 0.0f, progSlowAttack = 0.0f, progSlowRelease = 0.0f, progFastRelease = 0.0f;
};
} // namespace flub
