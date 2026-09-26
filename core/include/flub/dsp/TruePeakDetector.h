// Flubsound Pro - 4x polyphase inter-sample (true) peak detector.
//
// ITU-R BS.1770-4 Annex 2 estimates true peak by 4x oversampling. We use our
// own Kaiser-windowed sinc interpolator: a 161-tap prototype (40 taps per
// phase, Kaiser beta 5). Every fractional-delay phase is flat within 0.02 dB
// up to 0.4535 fs (20 kHz at 44.1 kHz, 21.8 kHz at 48 kHz) and over-reads by
// at most +0.04 dB, so the detector never materially under-reads full-band
// programme. The earlier 24-tap design dipped by up to 2 dB near 20 kHz.
// Its constant delay is exactly kDelay base samples.
//
// Each call consumes x[n] and returns max |.| over the four positions
// { n-kDelay, n-kDelay+1/4, n-kDelay+1/2, n-kDelay+3/4 }.  Phase 0 of a
// Nyquist (M-th band) interpolator reproduces the original sample exactly.
#pragma once

#include "FirDesign.h"
#include "flub/common/AudioBlock.h"

#include <array>
#include <cmath>
#include <vector>

namespace flub
{
class TruePeakDetector
{
public:
    static constexpr int kPhases = 4;
    static constexpr int kTapsPerPhase = 40;
    static constexpr int kDelay = kTapsPerPhase / 2; // = 20 base samples

    static constexpr double kKaiserBeta = 5.0; // see the passband analysis above

    using PhaseTaps = std::array<std::array<float, kTapsPerPhase>, kPhases>;

    /** The interpolator design, shared with TruePeakLimiter's detector so the
        limiter and the meters can never read different peaks. Phase 0 is the
        delayed input sample itself and is left untouched (all zero). */
    static void designPhaseTaps (PhaseTaps& phaseTaps) noexcept
    {
        constexpr int length = kPhases * kTapsPerPhase + 1; // 161, centre = 80
        constexpr int centre = (length - 1) / 2;
        for (int p = 1; p < kPhases; ++p)
        {
            double sum = 0.0;
            auto& taps = phaseTaps[static_cast<size_t> (p)];
            for (int j = 0; j < kTapsPerPhase; ++j)
            {
                const int k = kPhases * j + p;
                const double v = fir::sinc (static_cast<double> (k - centre) / kPhases) * fir::kaiser (k, length, kKaiserBeta);
                taps[static_cast<size_t> (j)] = static_cast<float> (v);
                sum += v;
            }
            for (auto& t : taps)
                t = static_cast<float> (t / sum); // unity DC gain per phase
        }
    }

    /** Allocates. */
    void prepare (int numChannels)
    {
        designPhaseTaps (phaseTaps);
        for (int c = 0; c < kMaxChannels; ++c)
            history[static_cast<size_t> (c)].assign (c < numChannels ? 2 * kTapsPerPhase : 0, 0.0f);
        pos.fill (0);
    }

    void reset() noexcept
    {
        for (auto& h : history)
            std::fill (h.begin(), h.end(), 0.0f);
        pos.fill (0);
    }

    float processSample (int ch, float x) noexcept
    {
        auto& h = history[static_cast<size_t> (ch)];
        int& p = pos[static_cast<size_t> (ch)];
        p = (p == 0 ? kTapsPerPhase - 1 : p - 1);
        h[static_cast<size_t> (p)] = x;
        h[static_cast<size_t> (p + kTapsPerPhase)] = x;
        const float* w = h.data() + p; // w[j] = x[n - j]

        float peak = std::abs (w[kDelay]); // phase 0: original sample
        for (int ph = 1; ph < kPhases; ++ph)
        {
            const float* t = phaseTaps[static_cast<size_t> (ph)].data();
            float acc = 0.0f;
            for (int j = 0; j < kTapsPerPhase; ++j)
                acc += t[j] * w[j];
            peak = std::max (peak, std::abs (acc));
        }
        return peak;
    }

private:
    PhaseTaps phaseTaps {};
    std::array<std::vector<float>, kMaxChannels> history;
    std::array<int, kMaxChannels> pos {};
};
} // namespace flub
