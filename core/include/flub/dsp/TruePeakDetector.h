// Flubsound Pro - 4x polyphase inter-sample (true) peak detector.
//
// ITU-R BS.1770-4 Annex 2 estimates true peak by 4x oversampling. We use our
// own Kaiser-windowed sinc interpolator (97-tap prototype, 24 taps per phase)
// whose passband is flat to ~19 kHz at 48 kHz, which meets the Annex 2
// accuracy intent; its constant delay is exactly kDelay base samples.
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
    static constexpr int kTapsPerPhase = 24;
    static constexpr int kDelay = kTapsPerPhase / 2; // = 12 base samples

    /** Allocates. */
    void prepare (int numChannels)
    {
        constexpr int length = kPhases * kTapsPerPhase + 1; // 97, centre = 48
        constexpr int centre = (length - 1) / 2;
        constexpr double beta = 8.0;
        for (int p = 1; p < kPhases; ++p)
        {
            double sum = 0.0;
            auto& taps = phaseTaps[static_cast<size_t> (p)];
            for (int j = 0; j < kTapsPerPhase; ++j)
            {
                const int k = kPhases * j + p;
                const double v = fir::sinc (static_cast<double> (k - centre) / kPhases) * fir::kaiser (k, length, beta);
                taps[static_cast<size_t> (j)] = static_cast<float> (v);
                sum += v;
            }
            for (auto& t : taps)
                t = static_cast<float> (t / sum); // unity DC gain per phase
        }
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
    std::array<std::array<float, kTapsPerPhase>, kPhases> phaseTaps {};
    std::array<std::vector<float>, kMaxChannels> history;
    std::array<int, kMaxChannels> pos {};
};
} // namespace flub
