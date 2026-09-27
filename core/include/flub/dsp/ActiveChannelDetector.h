// Flubsound Pro - which channels of a 5.1 / 7.1 input carry content, and
// whether the strip should fold it as surround or as stereo (docs/11 E27).
//
// Windows process loopback and the Game endpoint hand the Game strip an
// 8-channel container; a game rendering for a stereo headset fills only
// FL/FR. Folding that as surround (two virtual speakers at +-30 deg, or the
// BS.775 matrix at -3 dB) colours and narrows it, so the chain switches such
// content to its stereo passthrough fold.
//
// Measure: a one-pole mean-square follower per channel (50 ms), after the
// input gain and AutoLevel. front = max(FL, FR), rest = max(channels 2..N-1),
// the LFE included. Per block:
//   surround evidence : rest > front - 50 dB and rest > -90 dBFS
//   stereo evidence   : front > -70 dBFS and rest < front - 60 dB
//   (anything else resets the stereo run; digital silence holds both runs)
// Decisions:
//   * 250 ms of unbroken surround evidence -> Surround, and surround is then
//     CONFIRMED: latched until reset() (a new device or routed process), so a
//     long rear-silent stretch in a real 7.1 match cannot flip the fold.
//     250 ms (the report's approach says 300 ms) keeps the decision within
//     the Done-when's 300 ms at host blocks up to 2048 samples at 48 kHz.
//   * 2 s of unbroken stereo evidence while unconfirmed -> Stereo.
// Start (and after reset()): Surround, unconfirmed - a real 7.1 stream is
// never folded as stereo while the detector makes up its mind.
//
// Real-time: process() is allocation- and lock-free; timers are in samples,
// decisions are taken once per block.
#pragma once

#include "EnvelopeFollower.h"
#include "flub/common/AudioBlock.h"
#include "flub/common/Math.h"
#include "flub/common/Realtime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace flub
{
class ActiveChannelDetector
{
public:
    enum class Fold : uint8_t
    {
        Surround = 0,
        Stereo = 1
    };

    static constexpr float kFollowerMs = 50.0f;
    static constexpr float kSurroundOnDb = -50.0f;    // rest re front: surround evidence above
    static constexpr float kStereoBelowDb = -60.0f;   // rest re front: stereo evidence below
    static constexpr float kFrontFloorDbfs = -70.0f;  // fronts quieter than this: no stereo evidence
    static constexpr float kChannelFloorDbfs = -90.0f; // a channel quieter than this is silent
    static constexpr float kSurroundHoldMs = 250.0f;
    static constexpr float kStereoHoldMs = 2000.0f;

    void prepare (double sampleRate, int numChannels) noexcept
    {
        channels = std::clamp (numChannels, 1, kMaxChannels);
        for (auto& f : followers)
            f.prepare (sampleRate, kFollowerMs);
        surroundHold = std::max (1, msToSamples (kSurroundHoldMs, sampleRate));
        stereoHold = std::max (1, msToSamples (kStereoHoldMs, sampleRate));
        reset();
    }

    /** Back to "Surround, unconfirmed" with empty followers. */
    void reset() noexcept FLUB_NONBLOCKING
    {
        for (auto& f : followers)
            f.reset();
        ms.fill (0.0f);
        surroundRun = stereoRun = 0;
        fold = Fold::Surround;
        confirmed = false;
        mask = 0;
    }

    void process (const AudioBlock& in) noexcept FLUB_NONBLOCKING
    {
        const int n = in.numSamples;
        const int nch = std::min (in.numChannels, channels);
        if (n <= 0 || nch <= 0)
            return;
        for (int c = 0; c < nch; ++c)
        {
            const float* x = in.channel (c);
            auto& f = followers[static_cast<size_t> (c)];
            for (int i = 0; i < n; ++i)
                f.process (static_cast<double> (x[i]) * x[i]);
            // Below -200 dB (or a non-finite reading): exact zero, so a long
            // silence never decays into subnormals.
            if (! (f.get() > 1.0e-20 && std::isfinite (f.get())))
                f.reset();
            ms[static_cast<size_t> (c)] = static_cast<float> (f.get());
        }

        const float front = std::max (ms[0], nch > 1 ? ms[1] : 0.0f);
        float rest = 0.0f;
        for (int c = 2; c < nch; ++c)
            rest = std::max (rest, ms[static_cast<size_t> (c)]);

        // Mean-square (power) forms of the dB constants above.
        constexpr float channelFloor = 1.0e-9f, frontFloor = 1.0e-7f;
        constexpr float surroundRatio = 1.0e-5f, stereoRatio = 1.0e-6f;
        uint32_t m = 0;
        for (int c = 0; c < nch; ++c)
        {
            const float v = ms[static_cast<size_t> (c)];
            if (v > channelFloor && (c < 2 || v > front * surroundRatio))
                m |= 1u << static_cast<uint32_t> (c);
        }
        mask = m;

        const bool surroundEvidence = rest > channelFloor && rest > front * surroundRatio;
        const bool stereoEvidence = front > frontFloor && rest < front * stereoRatio;
        surroundRun = surroundEvidence ? std::min (surroundRun + n, surroundHold) : 0;
        if (stereoEvidence)
            stereoRun = std::min (stereoRun + n, stereoHold);
        else if (surroundEvidence || front > frontFloor)
            stereoRun = 0; // content that is not stereo-only; silence holds the run

        if (surroundRun >= surroundHold)
        {
            fold = Fold::Surround;
            confirmed = true;
        }
        else if (! confirmed && stereoRun >= stereoHold)
        {
            fold = Fold::Stereo;
        }
    }

    Fold getFold() const noexcept { return fold; }
    bool isSurroundConfirmed() const noexcept { return confirmed; }
    /** Bit c set: channel c carries content (FL/FR above -90 dBFS; the others
        also above front - 50 dB). */
    uint32_t getActiveMask() const noexcept { return mask; }

private:
    std::array<MeanSquareFollower, kMaxChannels> followers {};
    std::array<float, kMaxChannels> ms {}; // followers' readings at the end of the last block
    int channels = 2;
    int surroundRun = 0, stereoRun = 0, surroundHold = 1, stereoHold = 1;
    Fold fold = Fold::Surround;
    bool confirmed = false;
    uint32_t mask = 0;
};
} // namespace flub
