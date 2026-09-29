// Flubsound Pro - Startle Guard: a fast, programme-relative ceiling for
// sudden loud events (docs/11 E21, with E20's Tame).
//
// The "Dynamic Range" control (guard.range: Off / 20 / 15 / 10 / 6 LU; 10 LU
// is E21's "Balanced", 6 LU its "Shield") sets how far an event may rise
// over the recent programme. A night-time listener sets the volume for the
// ambience and the dialogue; an explosion or a burst of gunfire 20-30 LU
// above them is what wakes the house. The guard turns such an event down to
// the ceiling and lets everything at or under it through untouched.
//
//   sidechain  x through a 2nd-order kSidechainHpHz high-pass (an explosion's
//              low end and the rumble are the Tame stage's, below) and a
//              kCueDipDb bell at 3.2 kHz, Q 0.7, over the 2-5 kHz cue band
//              (docs/11 E19), K-weighted, so a loud footstep or a reload does
//              not duck the mix.
//   reference  the recent programme level: a 3 s mean square of the
//              sidechain, corrected for its cold start (the weighted mean
//              over the admitted time so far, like GatedLoudness in
//              Protection.h), advanced only on the programme itself: not in
//              digital silence (< -70 LUFS), not while it is more than
//              kQuietRestartLu under the reference (a pause, a quieter scene:
//              after kQuietRestartSeconds of that the reference restarts on
//              it), and not while an event is on - the detector (below)
//              more than kEventGateLu over it, and for kEventHoldMs after -
//              so a fight does not raise it and the ambience after it is
//              judged against the ambience before it.
//              The gate acts per sample: a slower gate (AutoLevel's 400 ms
//              upper gate) let the first 80 ms of gunfire into the reference,
//              which then climbed with the fight and let it through.
//              Programme that stays over the gate for kNewLevelSeconds is a
//              new level: it is admitted from then on, so the reference rises
//              (3 s) and a long battle or louder music is released smoothly.
//              Not valid (no guarding) until kMinReferenceMs of programme.
//              It is kept in the terms of the level before AutoLevel
//              (setLevelOffsetDb), so AutoLevel's slow moves shift the
//              ceiling with the programme.
//   detector   max (momentary, fast - kCrestAllowanceDb) in the power domain:
//              the BS.1770 momentary loudness (a 400 ms rectangular window, in
//              10 ms steps plus the samples so far, so an event is forgotten
//              400 ms after it ends; a 200 ms one-pole took 0.7 s to fall
//              15 dB, which kept the gain down past the 1 s Done-when)
//              and a 1 ms one-pole mean square (the onset of a shot), the
//              latter allowed kCrestAllowanceDb over the ceiling because a
//              transient reads that much higher on 1 ms than its loudness.
//   computer   target = min (0, reference + ceiling - detector) dB; held for
//              kHoldMs (a 10 shots/s burst is one event, not ten), then
//              released towards the target with a kReleaseMs time constant
//              (15 dB of reduction is back within 1 dB about 0.9 s after the
//              event: at most 400 ms for the momentary to forget it, then
//              the release). The gain falls to a lower target with a one-pole
//              attack of a third of the look-ahead (at least kMinAttackMs).
//              Programme that stays over the ceiling (loud music) is
//              gain-modulated at its transients (the 1 ms detector) until it
//              becomes the new level.
//   look-ahead none of its own: the chain measures the signal ahead of the
//              compressor slot and applies the gains to what leaves it (the
//              slot's latency, 0.5 / 1 / 3 ms, is spent anyway, bypassed or
//              not), so the gain is already falling when an onset arrives and
//              the chain latency does not change.
//
// Tame (docs/11 E20): the same control keys the Gaming mode's anti-masking
// band 6 in the dynamic EQ, ahead of the guard (a CutAbove low shelf at
// 90 Hz): its range is tameAmountFor() x kTameMaxRangeDb at kTameRatio
// (it was 6 dB x Footsteps at 3:1 before E20 took it off Footsteps), and its
// threshold follows the reference, min (-22 dBFS, reference +
// kTameOverReferenceDb), so an explosion's low end is taken down at any
// listening level while the guard's broadband gain stays off the cues that
// follow it. Off keeps the band at range 0 and -22 dBFS, as since E20.
//
// The gain is broadband and stereo-linked. Off: a running gain is released
// as usual, then the stage idles (no measurement, the audio untouched) and
// starts again from a fresh reference when it is switched back on.
#pragma once

#include "flub/common/AudioBlock.h"
#include "flub/common/Realtime.h"
#include "flub/dsp/Biquad.h"

#include <array>
#include <atomic>
#include <vector>

namespace flub
{
class StartleGuard
{
public:
    static constexpr float kSidechainHpHz = 150.0f;
    static constexpr float kCueDipHz = 3200.0f, kCueDipQ = 0.7f, kCueDipDb = -12.0f;
    static constexpr float kFastMs = 1.0f, kMomentaryMs = 400.0f, kMomentaryStepMs = 10.0f;
    static constexpr float kCrestAllowanceDb = 8.0f;
    static constexpr float kHoldMs = 150.0f, kReleaseMs = 200.0f;
    static constexpr float kMinAttackMs = 0.25f;
    static constexpr float kReferenceMs = 3000.0f, kMinReferenceMs = 100.0f, kSilenceLufs = -70.0f;
    static constexpr float kEventGateLu = 6.0f, kEventHoldMs = 300.0f, kNewLevelSeconds = 5.0f;
    static constexpr float kQuietRestartLu = 20.0f, kQuietRestartSeconds = 3.0f;
    /** Tame (docs/11 E20): band 6's range at full Tame, its threshold over the
        reference, and the highest threshold (the band's fixed one before). */
    static constexpr float kTameMaxRangeDb = 18.0f, kTameRatio = 4.0f, kTameOverReferenceDb = 6.0f, kTameMaxThresholdDb = -22.0f;

    /** The guard.range choices (Parameters.h GuardRangeValue): the ceiling
        over the reference in LU (0 = off) and the Tame amount (0..1). */
    static float ceilingLuFor (int rangeChoice) noexcept;
    static float tameAmountFor (int rangeChoice) noexcept;

    /** Non-RT. lookaheadSamples: the latency between measure() and apply()
        (the compressor slot's); allocates the per-sample gain buffer. */
    void prepare (double sampleRate, int maxBlockSize, int lookaheadSamples);
    void reset() noexcept FLUB_NONBLOCKING;

    /** The ceiling over the reference (LU); 0 or less switches the guard off. */
    void setCeilingLu (float lu) noexcept FLUB_NONBLOCKING;
    /** The gain (dB) an upstream broadband stage applies whose moves the
        reference follows (AutoLevel's), for the next measure(). */
    void setLevelOffsetDb (float db) noexcept FLUB_NONBLOCKING;

    /** Measures one stereo block (numSamples <= maxBlockSize) and computes its
        gains. unmeasured: a block hidden from the control loops (docs/11 E10)
        - the gain holds and nothing is learnt from it. */
    void measure (const AudioBlock& block, bool unmeasured) noexcept FLUB_NONBLOCKING;
    /** Applies the gains of the last measure() to a block of the same length
        (the one that left the look-ahead stage). */
    void apply (const AudioBlock& block) noexcept FLUB_NONBLOCKING;

    /** The gain at the end of the last block (dB <= 0). */
    float getGainDb() const noexcept { return gainDb; }
    /** Deepest gain of the last block (dB <= 0), any thread. */
    float getBlockGainDb() const noexcept { return blockGainDb.load (std::memory_order_relaxed); }
    /** The reference (LUFS through the sidechain weighting, at the current
        level offset); kMinusInfDb while the guard idles or the reference is
        not valid. */
    float getReferenceLufs() const noexcept;
    /** True while the guard measures or its gain is still below unity. */
    bool isRunning() const noexcept { return running; }

private:
    void restartReference() noexcept;

    double sr = 48000.0;
    int maxBlock = 0;
    float ceilingLu = 0.0f, levelOffsetDb = 0.0f;
    bool running = false, gainIdle = true;

    BiquadCoeffs hpCoeffs, dipCoeffs, k1Coeffs, k2Coeffs;
    std::array<BiquadState, 2> hpState {}, dipState {}, k1State {}, k2State {};
    std::vector<float> gains; // per-sample linear gains of the last measure()
    int pendingSamples = 0;   // gains waiting for apply()

    // Detector and reference (mean squares of the K-weighted sidechain).
    // The momentary loudness: the sums of the last kMomentarySteps - 1 whole
    // steps (a ring) and of the samples of the current one.
    static constexpr int kMomentarySteps = static_cast<int> (kMomentaryMs / kMomentaryStepMs);
    std::array<double, kMomentarySteps - 1> stepSums {};
    double stepsSum = 0.0, partialSum = 0.0;
    int stepLength = 480, partialLength = 0, stepCount = 0, stepPos = 0;
    double fastMs = 0.0, slowMs = 0.0, slowFill = 0.0;
    double fastCoeff = 0.0, slowCoeff = 0.0, crestFactor = 1.0;
    double minFill = 0.0, silencePower = 0.0, eventGateFactor = 1.0, quietFactor = 1.0;
    int eventHoldSamples = 14400, newLevelSamples = 240000, quietRestartSamples = 144000, holdSamples = 7200;
    int eventHoldLeft = 0, eventRun = 0, quietRun = 0, holdLeft = 0;
    float attackCoeff = 0.0f, releaseCoeff = 0.0f;
    float heldTargetDb = 0.0f, gainDb = 0.0f, lastLinear = 1.0f;
    std::atomic<float> blockGainDb { 0.0f };
};
} // namespace flub
