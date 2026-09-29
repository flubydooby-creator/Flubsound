// Flubsound Pro - the chain's net tonal balance, for the tonal-balance guard (docs/11 E07).
//
// The SafetyGovernor's tonal rule (Protection.h) needs to know how much
// brighter the chain has made the programme, whatever made it so: the
// presence bell, the air exciter and shelf, the Gaming voice band, the
// footstep cue bands, the saturator's and the clipper's harmonics. This
// compares long-term band powers of a reference (the chain's signal before
// the mode bands and the enhancement: the dynamic EQ's input, so the user's
// parametric EQ is part of the reference and never counts as a lift) and of
// the chain's output:
//
//   Bands (8th order: a 4th-order Butterworth high-pass at the lower edge
//   and a low-pass at the upper, so a presence bell's skirt does not read
//   in the next band): mids 200 Hz - 1 kHz, presence 2 - 5 kHz,
//   harsh 5 - 10 kHz, air 10 - 16 kHz (the air band only where 16 kHz is
//   under 0.45 fs). Both signals stereo, squares summed over the channels.
//   The chain accumulates both sides per sample and closes a window on the
//   governor's 10 ms tick (tick()): each band's window mean joins a power
//   one-pole with tau = kAverageSeconds (in samples, so a short window after
//   a restarted grid weighs less). A window whose reference mids are under
//   kSilencePower (a pause) is not averaged: the readings hold.
//   Reading, per band b over the mids:
//     lift_b = 10 log10 ((out_b / ref_b) / (out_mids / ref_mids))   (dB)
//   the tilt the chain added (a broadband gain reads 0 in every band).
//   Valid (hasReading()) after kMinSeconds of averaged programme; a band
//   whose reference is kBandRangeDb under the mids' reads no lift (noise).
//
// Cost: 16 SVF sections per sample and channel on each side, only at
// protection strength Normal / Strict. RT-safe: process*() and tick()
// neither allocate nor lock.
#pragma once

#include "Svf.h"
#include "flub/common/AudioBlock.h"
#include "flub/common/Math.h"
#include "flub/common/Realtime.h"

#include <array>
#include <cstdint>

namespace flub
{
class TonalBalanceMeter
{
public:
    enum Band : int { Mids = 0, Presence, Harsh, Air, kNumBands };
    static constexpr std::array<double, kNumBands> kLowHz { 200.0, 2000.0, 5000.0, 10000.0 };
    static constexpr std::array<double, kNumBands> kHighHz { 1000.0, 5000.0, 10000.0, 16000.0 };
    static constexpr double kAverageSeconds = 1.0;
    static constexpr double kMinSeconds = 0.5;
    static constexpr double kSilencePower = 1.0e-9; // reference mids under -90 dB
    static constexpr float kBandRangeDb = 60.0f;
    /** getLiftDb() of a band without a reading. */
    static constexpr float kNoReading = kMinusInfDb;

    void prepare (double sampleRate);
    void reset() noexcept FLUB_NONBLOCKING;
    /** Accumulate the reference / the output (stereo; the same samples on both sides). */
    void processReference (const AudioBlock& block) noexcept FLUB_NONBLOCKING
    {
        accumulate (block, reference);
        windowSamples += block.numSamples;
    }
    void processOutput (const AudioBlock& block) noexcept FLUB_NONBLOCKING { accumulate (block, output); }
    /** Closes the window accumulated since the last tick. */
    void tick() noexcept FLUB_NONBLOCKING;

    bool hasReading() const noexcept { return averagedSamples >= minSamples; }
    /** lift_b (dB, see the header comment) of Presence, Harsh or Air;
        kNoReading while !hasReading(), for the mids, or for a band that is
        not measured at this rate or has no reference content. */
    float getLiftDb (int band) const noexcept FLUB_NONBLOCKING;

private:
    struct Side
    {
        std::array<std::array<std::array<SvfState, 4>, 2>, kNumBands> states {}; // [band][channel][hp, hp, lp, lp]
        std::array<double, kNumBands> window {}, average {};
    };
    void accumulate (const AudioBlock& block, Side& side) noexcept FLUB_NONBLOCKING;

    double sr = 48000.0;
    std::array<std::array<SvfCoeffs, 2>, kNumBands> highPass {}, lowPass {}; // two Butterworth sections each
    std::array<bool, kNumBands> measured {};
    Side reference, output;
    std::int64_t windowSamples = 0, averagedSamples = 0, minSamples = 24000;
};
} // namespace flub
