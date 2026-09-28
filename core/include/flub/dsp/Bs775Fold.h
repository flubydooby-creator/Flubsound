// Flubsound Pro - the surround-to-stereo fold and the LFE path shared by every
// fold (docs/11 E01, E27).
//
// Channel order (WAVEFORMATEXTENSIBLE): 5.1 FL FR FC LFE SL SR,
//                                       7.1 FL FR FC LFE BL BR SL SR.
//
//   L = g (FL + k FC + k SL [+ k BL] + a LP(LFE))
//   R = g (FR + k FC + k SR [+ k BR] + a LP(LFE))
//
// k = 1/sqrt 2 (-3 dB, ITU-R BS.775). g is the caller's overall gain:
// k for the surround fold (the chain's -3 dB headroom trim, unchanged since
// v1), 1 for the stereo passthrough fold (E27: FL/FR-only content in an
// 8-channel container comes out exactly as the same 2-channel stream).
// a = the LFE level relative to ONE main channel (virt.lfe), LP = LfeFold's
// low-pass. HeadphoneVirtualizer renders its LFE with the same LfeFold: its
// main channels reach the near ear at unity below ~200 Hz and both paths
// share its -3 dB trim, so "a dB above one main" holds in every fold. Its
// level match (docs/11 E28a) then scales the mains by the loudness ratio to
// this fold and leaves the LFE alone, so the LFE keeps its level in both
// folds; a main channel that the virtualiser sends to both ears sits about
// 3 dB lower at each ear than here, at the same loudness.
//
// LFE low-pass: 4th-order Butterworth at 120 Hz (two SVF sections), the
// virtualiser's filter since v1. docs/11 E01 proposes an LR4; an LR4 only
// earns its -6 dB crossover point against a complementary LR4 high-pass whose
// sum is flat, and the mains are not high-passed here, so the LFE needs only
// its band limit. Keeping the Butterworth keeps the virtualiser path
// bit-identical and gives 0.0 dB at 50 Hz (LR4: -0.26 dB).
//
// No fixed attenuation for the LFE + centre sum (E01 step 5): the worst-case
// peak goes through the same budget as every other channel sum (the fold's
// -3 dB, AutoLevel and the maximizer's true-peak limiter).
//
// Real-time: no allocation, no locks; prepare() is the only non-RT call.
#pragma once

#include "Svf.h"
#include "flub/common/AudioBlock.h"
#include "flub/common/Math.h"
#include "flub/common/Realtime.h"
#include "flub/common/SmoothedValue.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace flub
{
/** The LFE path: 120 Hz low-pass times a linearly ramped gain (20 ms). */
class LfeFold
{
public:
    static constexpr double kCutoffHz = 120.0;
    static constexpr float kRampMs = 20.0f;
    /** virt.lfe range: -20 dB .. +16 dB re one main channel (E01 widened the
        top from +10 so the +10 dB in-band convention has headroom above it). */
    static constexpr float kMinDb = -20.0f, kMaxDb = 16.0f;

    /** Linear gain for an LFE level; 0 when the fold is off. NaN -> 0 dB. */
    static float gainFor (bool on, float db) noexcept
    {
        if (! on)
            return 0.0f;
        return dbToGain (std::isnan (db) ? 0.0f : std::clamp (db, kMinDb, kMaxDb));
    }

    void prepare (double sampleRate, float initialGain) noexcept
    {
        for (int s = 0; s < 2; ++s)
            coeffs[static_cast<size_t> (s)] = SvfCoeffs::make (FilterType::LowPass, kCutoffHz, butterworthQ (2, s), 0.0, sampleRate);
        gain.reset (sampleRate, kRampMs, initialGain);
        reset();
    }

    /** Clears the filter and lands the gain ramp on its target. */
    void reset() noexcept FLUB_NONBLOCKING
    {
        clearState();
        gain.setImmediate (gain.getTarget());
    }

    /** Filter state only (the gain keeps ramping). */
    void clearState() noexcept FLUB_NONBLOCKING
    {
        for (auto& s : state)
            s.reset();
    }

    void setGain (float linear) noexcept FLUB_NONBLOCKING { gain.setTarget (linear); }
    float getGain() const noexcept { return gain.getCurrent(); }
    /** The gain is 0 and not ramping: the path contributes nothing. */
    bool isSilent() const noexcept { return ! gain.isSmoothing() && gain.getCurrent() == 0.0f; }

    /** Adds gain * LP(x) to both a and b (n samples). */
    void addTo (const float* x, float* a, float* b, int n) noexcept FLUB_NONBLOCKING
    {
        const SvfCoeffs c0 = coeffs[0], c1 = coeffs[1];
        SvfState s0 = state[0], s1 = state[1];
        for (int i = 0; i < n; ++i)
        {
            const float y = svfTick (c1, s1, svfTick (c0, s0, x[i])) * gain.next();
            a[i] += y;
            b[i] += y;
        }
        store (state[0], s0);
        store (state[1], s1);
    }

    /** Adds gain * LP(x) to a only (n samples): the virtualiser keeps its LFE
        apart so its level match can leave it alone (docs/11 E28a). */
    void addToMono (const float* x, float* a, int n) noexcept FLUB_NONBLOCKING
    {
        const SvfCoeffs c0 = coeffs[0], c1 = coeffs[1];
        SvfState s0 = state[0], s1 = state[1];
        for (int i = 0; i < n; ++i)
            a[i] += svfTick (c1, s1, svfTick (c0, s0, x[i])) * gain.next();
        store (state[0], s0);
        store (state[1], s1);
    }

    /** Advances the gain ramp by n samples without input. Stepped per sample
        (not skip()) so the value is the same for any block partition. */
    void skip (int n) noexcept FLUB_NONBLOCKING
    {
        if (gain.isSmoothing())
            for (int i = 0; i < n; ++i)
                gain.next();
    }

private:
    // State below -300 dB re full scale, or non-finite, is flushed at the end
    // of a block: no subnormal crawl, and one NaN input cannot latch.
    static float flushed (float v) noexcept { return std::abs (v) < 1.0e-15f || ! std::isfinite (v) ? 0.0f : v; }
    static void store (SvfState& dst, const SvfState& s) noexcept
    {
        dst.ic1 = flushed (s.ic1);
        dst.ic2 = flushed (s.ic2);
    }

    std::array<SvfCoeffs, 2> coeffs {};
    std::array<SvfState, 2> state {};
    LinearSmoothedValue gain;
};

/** The BS.775 matrix fold with the shared LFE path (see the file comment). */
class Bs775Fold
{
public:
    static constexpr float kMatrixGain = 0.70710678f; // k: centre and surrounds into each side, and the surround fold's trim

    void prepare (double sampleRate, float lfeGain) noexcept
    {
        lfe.prepare (sampleRate, lfeGain);
        lfeClean = true;
    }

    void reset() noexcept FLUB_NONBLOCKING
    {
        lfe.reset();
        lfeClean = true;
    }

    /** LFE level (LfeFold::gainFor), ramped over 20 ms. */
    void setLfeGain (float linear) noexcept FLUB_NONBLOCKING { lfe.setGain (linear); }
    const LfeFold& getLfe() const noexcept { return lfe; }

    /** Folds io (FL FR [FC LFE SL SR | FC LFE BL BR SL SR]) into channels 0/1
        in place, times `overall` (kMatrixGain: surround fold, 1: stereo
        passthrough). Channels >= 2 are read, not written. With the LFE fold
        off (gain 0, settled) the arithmetic is exactly the v1 downmix. */
    void process (const AudioBlock& io, float overall) noexcept FLUB_NONBLOCKING
    {
        const int nch = io.numChannels;
        const int n = io.numSamples;
        if (nch < 2 || n <= 0)
            return;
        float* l = io.channel (0);
        float* r = io.channel (1);
        if (nch >= 6 && ! lfe.isSilent())
        {
            lfe.addTo (io.channel (3), l, r, n);
            lfeClean = false;
        }
        else if (! lfeClean)
        {
            lfe.clearState(); // silent from here on: restart cleanly when it comes back
            lfeClean = true;
        }

        constexpr float k = kMatrixGain;
        const float* c = nch > 2 ? io.channel (2) : nullptr;
        for (int i = 0; i < n; ++i)
        {
            float lo = l[i], ro = r[i];
            if (c != nullptr)
            {
                lo += k * c[i];
                ro += k * c[i];
            }
            for (int s = 4; s + 1 < nch; s += 2)
            {
                lo += k * io.channel (s)[i];
                ro += k * io.channel (s + 1)[i];
            }
            l[i] = overall * lo;
            r[i] = overall * ro;
        }
    }

private:
    LfeFold lfe;
    bool lfeClean = true;
};
} // namespace flub
