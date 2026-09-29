// Flubsound Pro - protection & loudness control loops (all RT-safe).
//
// DistortionMonitor (measured THD+N of the chain's nonlinear stages):
//   Per block it power-sums the latest THD+N readings (windows of at least
//   25 ms) of the stages in series (the saturator and the maximizer's soft
//   clipper, each measured around its own curve, see
//   flub/dsp/DistortionEstimator.h); a stage that is fully bypassed feeds
//   -160 dB. A power-domain one-pole with tau = 300 ms smooths the block
//   value for the meters (MeterBus::distortionDb).
//   The intentional harmonic generators (the bass engine's harmonics and the
//   clarity air exciter) are measured the same way, but their readings are
//   the share of the harmonics they add on purpose: updateHarmonics() sums
//   and smooths them separately, and they are neither in the THD+N above nor
//   a SafetyGovernor input (docs/03-dsp-design.md §14.5 has the numbers).
//
// SafetyGovernor (THD / over-processing protection):
//   Inputs per block: maximizer limiter GR (dB; the deepest per fixed 10 ms
//   window, so it does not depend on the host block size) and distortion
//   (dB): the measured THD+N of the saturator power-summed with the
//   clipper's share, which is its measured THD+N floored at its clip energy
//   ratio over the same analysis window (the former proxy, which reads
//   higher on a steady tone), so the governor does not act later on
//   clipping than it did on the proxy alone. At protection strength Normal
//   and Strict the chain raises the maximizer's share to its whole-stage
//   residual when that reads higher (the clipper and the limiter together,
//   the limiter's gain-modulation IMD included; docs/11 E06 step 1). Budget: limiter GR averaged over ~3 s must stay above
//   -6 dB, distortion (power average over ~3 s) below -30 dB (~3.2 % RMS of
//   the output). When over budget, scale falls at 15 %/s (min 0.3); when
//   under budget minus 1.5 dB hysteresis it recovers at 3 %/s. The scale
//   multiplies every "governed" macro amount.
//   The loop ticks on a fixed 10 ms grid counted from reset() - the grid the
//   maximizer's limiter-GR windows close on - with dt = 10 ms, however the
//   host splits the audio into blocks (docs/11 E06 slice): a call that
//   closes no window leaves the scale alone, one that closes k windows ticks
//   k times on its readings. ProcessingChain ends its processing segments on
//   this grid (samplesToNextTick()), so each tick reads the window that just
//   closed and its scale takes effect at the same sample for any host block
//   size. A per-block tick (dt = block length, the new scale applied from the
//   next host block) spread Boost 100 + Loudness 100 by 0.25 LU over blocks
//   64-4096 on kick-heavy programme; now 0.05 LU.
//   Protection strength (ProtectionStrength, a host setting, not a preset
//   value): Off (the default) never touches base values and runs the
//   stepwise loop above, exactly as before; Normal also scales the base
//   max.drive, sat.drive and bass.harmonics by the same scale
//   (ProcessingChain::applyParameters) and runs the measured loop below;
//   Strict does that with stricter budgets and lets the scale fall to 0
//   instead of 0.3.
//   Measured loop (Normal / Strict, docs/11 E06 Phase 3; updateMeasured()),
//   on the same 10 ms ticks. The chain measures two spans with
//   WeightedResidual (flub/dsp/WeightedResidual.h: the loudness-weighted,
//   masking-limited residual of a span against its latency-aligned input):
//   the saturator .. maximizer ("drive span") and the bass engine. Two
//   scales, in dB (u = 20 log10 scale), each moved by PI loops on dB errors
//   (> 0 = over the set point, kSetPointMarginDb inside the budget):
//     * the drive scale (getScale(): the governed macro amounts and the base
//       drives, as above) is the lowest of three loops - the limiter's GR
//       (0.5 s average) as a slow trim (kTrimGain) around a feed-forward
//       (DriveFeedForward: the drive at which the maximizer input's peaks
//       of the last 3 s would hold the limiter at its budget, as a share of
//       the drive at the full scale), the drive span's audible residual plus
//       the bass engine's non-harmonic share (its protection riding the
//       boost shelf, which the drive scale governs), and in Music the
//       dynamics budget: the output's PLR (PlrMeter, ~3 s) may not fall
//       under the budget, nor more than kPlrAllowanceDb under the input's
//       own PLR (a steady tone cannot be given more);
//     * the harmonics scale (getHarmonicsScale(), multiplied into
//       bass.harmonics on top of the drive scale): the harmonics policy.
//       The generator's harmonics are intended, so they never count against
//       the drive; but harmonics added on top of a fundamental the output
//       still carries are budgeted by what the programme leaves audible (a
//       steady bass tone masks none of them; dense programme most): the bass
//       engine's audible residual in the share its generator's own meter
//       gives the harmonics. Small Speaker Mode's harmonics stand in for a
//       removed fundamental: Normal leaves them alone, Strict governs them.
//   The PI loops run on the error plus kApproachDb while over the set point
//   (so they reach it), hold within kHoldBandDb under it, and recover (no
//   proportional term, kRiseDbPerSec at most) further under; falls are
//   limited to kFallDbPerSec (drive) / kHarmonicsFallDbPerSec. Probe
//   memory: at the onset of a back-off a loop steps kProbeMarginDb under
//   the level that went over and caps its recovery there for a hold that
//   doubles (4 s .. 64 s) while back-offs keep starting at the same level,
//   and it waits kVerifySeconds (the readings' lag) before backing off
//   further unless the onset is far over: stages that switch in at a
//   threshold (the maximizer's glue on a steady bass tone read 13 dB more
//   residual per dB of drive) would otherwise be probed every few seconds.
//   Budgets (budgetsFor(), provisional until the E60 listening panel):
//   limiter GR -6 dB (Strict -4), audible residual -35 dB Music / -30 dB
//   Gaming (Strict 6 dB lower), PLR 8 dB in Music (Strict 10), none in
//   Gaming. The ~3 s averages of the GR and the stage THD+N above are still
//   kept for the meters.
//   Tonal-balance rule (Normal / Strict, docs/11 E07; getTonalScale()): a
//   third scale, on the lifts the macros add ungoverned - Clarity presence
//   and air, the Gaming Voice & Score band and the Music air band (never the
//   user's EQ, the footstep cue bands or anything in base values). The
//   chain measures its net tonal balance (TonalBalanceMeter: long-term
//   presence 2-5 kHz, harsh 5-10 kHz and air 10-16 kHz lifts over the
//   200 Hz - 1 kHz lift, output against the dynamic EQ's input); the loop
//   (a PI as above at kTonalGain, set point kTonalMarginDb inside the
//   budget, falls at most kTonalFallDbPerSec) takes the scale down while
//   any band's lift is over its budget, whatever made it bright, and lets
//   it recover when all are comfortably under. Budgets: presence / harsh /
//   air +3 / +3 / +4 dB in Music, +2 / +2 / +3 dB in Gaming (its Done-when:
//   2-5 kHz lift minus 200 Hz - 1 kHz lift <= +2 dB), Strict 1.5 dB lower.
//   It does not touch the drive scale.
//   getState() / getReason() say what the loop is doing and which budget
//   made it back off (published on MeterBus with the scale and both
//   averages); the measured loop adds kReasonDynamics, kReasonHarmonics and
//   kReasonTonal.
//
// GatedLoudness (AutoLevel and AutoDrive):
//   A 3 s K-weighted "slow" loudness that is only advanced while programme is
//   present. The gate is open when the block is not (near) digital silence
//   (RMS above -70 dBFS), a fast 100 ms follower reads above -50 LUFS, and it
//   is no more than 20 LU below the slow measure (the EBU R128 idea of
//   absolute + relative gating). Pauses, track gaps and fade-outs therefore
//   neither decay the measurement nor drive the control loops.
//   The slow measure is only fed while the gate is open, so the relative
//   gate alone could hold a programme 20 LU quieter than the last one out
//   forever. It therefore has a release: after 3 s of programme kept out by
//   the relative criterion alone (silence and the absolute gate pause the
//   count, an open gate clears it) the slow measure restarts and acquires
//   the new level.
//   The slow one-pole starts from zero after every reset and restart, so it
//   reads 10 log10(1 - exp(-t / 3 s)) dB low after t seconds of open gate
//   (8 dB at 0.5 s, 3 dB at 2 s). getLufs() adds that bias back: the reading
//   is the exponentially weighted mean over the open-gate time so far,
//   unbiased from the first block on.
//
// AutoLevel (LUFS input levelling / loudness normalisation):
//   GatedLoudness on the input with its upper gate on (below); gain =
//   target - measured, limited to -12..+6 dB (a quiet bed is lifted at most
//   6 dB), slew-limited to +1 dB/s (up) and -4 dB/s (down), adapted only
//   while the measure is advanced: frozen in silence, pauses and fade-outs,
//   while programme is kept out by the relative gate, and during loud events
//   the upper gate holds. For 2 s after such a freeze of programme the
//   upward slew is 3 dB/s, so what a long event took away comes back fast.
//   Applied as a per-block linear ramp (click-free).
//   Upper gate (docs/11 E21): a 400 ms K-weighted momentary loudness runs
//   beside the slow measure; while it reads more than 8 LU above the slow
//   measure, the block is left out of the slow measure and the gain holds, so
//   an explosion or a burst of gunfire neither pulls the gain down nor leaves
//   a hole in the ambience after it. Programme that stays that loud is a new
//   level, not an event: after 5 s of it (counted while the 100 ms follower
//   also reads above the gate, so the 400 ms measure's decay after an event
//   does not count) the slow measure restarts on it.
//
// AutoDrive (maximizer loudness target):
//   GatedLoudness on the post-chain output; a slow integrating loop (0.5 LU
//   dead band, <= 2 dB/s) produces a drive REDUCTION in [-requested drive, 0]
//   dB (never past 0 dB of drive, so recovery is immediate). It can
//   stop over-limiting but can never make things louder than the user/macros
//   asked for.
//
// ComparisonMatcher (fair A/B through the loudness-matched bypass, docs/11
//   E37 Phase 1): the dry reference and the processed output are K-weighted
//   all the time, in 100 ms sub-blocks. A sub-block is admitted when either
//   side reads above -60 LUFS; the difference is the energy ratio over the
//   last 30 admitted sub-blocks (3 s of programme, pauses left out), valid
//   from 4 on while both sides read above -70 LUFS over them. A sliding
//   window forgets a change completely after 3 s (a one-pole average took
//   16 s to settle within 0.1 dB after a 7 dB drop). It only ever
//   attenuates the louder side: dry trim = min (0, wet - dry), wet trim =
//   min (0, dry - wet), limited to -20 dB. A raise would need headroom the
//   dry reference does not have (the processed side got its loudness from
//   limiting), so the former capped, raise-only match read 2-3 LU short on
//   hot programme. A comparison starts when the global bypass is engaged
//   with matching on and lasts until the bypass has been off for 10 s;
//   during it the processed side keeps its trim, so every flip back and
//   forth is matched (the first flip still hears the processed side at its
//   own level). The trims follow the live difference for the first 1 s of
//   measured programme of a comparison (acquire) and are then frozen until
//   it ends, so the reference does not ride the programme. When it ends,
//   the wet trim returns to 0 dB at 2 dB/s. Matching off: the dry trim is
//   0 dB at once and the wet trim returns at the same rate.
#pragma once

#include "flub/analysis/LoudnessFollower.h"
#include "flub/common/Realtime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace flub
{
class GatedLoudness
{
public:
    static constexpr float kSlowTimeMs = 3000.0f;
    /** Programme kept out by the relative gate alone for this long restarts
        the slow measure (see the header comment). */
    static constexpr double kRelativeReleaseSeconds = 3.0;
    /** Upper gate (AutoLevel only, see the AutoLevel header comment). */
    static constexpr float kUpperMomentaryMs = 400.0f;
    static constexpr float kUpperGateLu = 8.0f;
    static constexpr double kUpperReleaseSeconds = 5.0;

    /** Turns the upper gate on or off; call before prepare(). */
    void setUpperGate (bool on) noexcept { upperGate = on; }

    void prepare (double sampleRate, int numChannels)
    {
        channels = numChannels;
        momentary.prepare (sampleRate, numChannels, 100.0f);
        slow.prepare (sampleRate, numChannels, kSlowTimeMs);
        upper.prepare (sampleRate, numChannels, kUpperMomentaryMs);
        slowPole = static_cast<double> (onePoleCoeff (kSlowTimeMs, sampleRate));
        releaseSamples = static_cast<std::int64_t> (kRelativeReleaseSeconds * sampleRate);
        upperReleaseSamples = static_cast<std::int64_t> (kUpperReleaseSeconds * sampleRate);
        reset();
    }

    void reset() noexcept
    {
        momentary.reset();
        upper.reset();
        restartSlow();
        gateOpen = held = programme = false;
    }

    void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING
    {
        // Instant close on (near) digital silence: track gaps, paused players.
        double sumSquares = 0.0;
        const int nch = std::min (block.numChannels, channels);
        for (int c = 0; c < nch; ++c)
            for (int i = 0; i < block.numSamples; ++i)
                sumSquares += static_cast<double> (block.channel (c)[i]) * block.channel (c)[i];
        const bool silent = sumSquares <= 1.0e-7 * std::max (1, nch * block.numSamples); // < -70 dBFS RMS

        momentary.process (block);
        if (upperGate)
            upper.process (block);
        const float m = momentary.getLufs();
        programme = ! silent && m > -50.0f;
        const float s = getLufs();
        gateOpen = programme && (! (s > -60.0f) || m > s - 20.0f);

        // Upper gate: a loud event is held out of the slow measure; one that
        // lasts kUpperReleaseSeconds is a new level and restarts it.
        held = upperGate && gateOpen && s > -60.0f && upper.getLufs() > s + kUpperGateLu;
        if (held)
        {
            if (m > s + kUpperGateLu)
                upperHeldSamples += block.numSamples;
            if (upperHeldSamples >= upperReleaseSamples)
            {
                restartSlow();
                held = false;
            }
        }
        else
        {
            upperHeldSamples = 0;
        }

        if (gateOpen && ! held)
        {
            relativeGatedSamples = 0;
            slow.process (block);
            openSamples += block.numSamples;
            if (biasDb > 0.0f)
            {
                // The one-pole started from zero has filled to 1 - pole^n of
                // its input level after n samples; divide that back out.
                const double residual = std::pow (slowPole, static_cast<double> (openSamples));
                biasDb = residual > 1.0e-7 ? static_cast<float> (-10.0 * std::log10 (1.0 - residual)) : 0.0f;
            }
        }
        else if (programme && ! gateOpen)
        {
            // Programme is present but more than 20 LU below the slow
            // measure, which cannot move while it is kept out: after
            // kRelativeReleaseSeconds, restart the measure on the new level.
            relativeGatedSamples += block.numSamples;
            if (relativeGatedSamples >= releaseSamples)
                restartSlow();
        }
    }

    /** Gated slow loudness (LUFS), corrected for the cold start of its one-pole. */
    float getLufs() const noexcept
    {
        const float raw = slow.getLufs();
        return raw > kMinusInfDb ? raw + biasDb : raw;
    }
    /** True while the slow measure is valid and was advanced by the last
        block: programme present, inside the gates, not held by the upper gate. */
    bool isActive() const noexcept { return gateOpen && ! held && getLufs() > -60.0f; }
    /** True while the upper gate holds the last block out (a loud event). */
    bool isHeld() const noexcept { return held; }
    /** True when the last block was programme (not silence, above the
        absolute gate), whether or not it was admitted. */
    bool hasProgramme() const noexcept { return programme; }

private:
    void restartSlow() noexcept
    {
        slow.reset();
        openSamples = 0;
        relativeGatedSamples = 0;
        upperHeldSamples = 0;
        biasDb = 1.0f; // any value > 0: recomputed on the next open block
    }

    LoudnessFollower momentary, slow, upper;
    int channels = 2;
    double slowPole = 0.0;
    std::int64_t releaseSamples = 144000, upperReleaseSamples = 240000;
    std::int64_t openSamples = 0, relativeGatedSamples = 0, upperHeldSamples = 0;
    float biasDb = 0.0f;
    bool upperGate = false, gateOpen = false, held = false, programme = false;
};

class DistortionMonitor
{
public:
    static constexpr float kMeterTauSeconds = 0.3f;

    void prepare (double sampleRate) noexcept;
    void reset() noexcept FLUB_NONBLOCKING;
    /** Combines this block's stage readings (dB; -160 = none) and returns the
        block's THD+N (power sum). Advances the meter smoothing by numSamples. */
    float update (float saturatorDb, float clipperDb, int numSamples) noexcept FLUB_NONBLOCKING;
    /** Power sum of two ratios in dB (-160 = none): stages in series. */
    static float combineDb (float aDb, float bDb) noexcept FLUB_NONBLOCKING;
    float getBlockDb() const noexcept { return blockDb; }
    /** Block THD+N smoothed in the power domain (tau = kMeterTauSeconds). */
    float getSmoothedDb() const noexcept { return smoothedDb; }

    /** The harmonic generators' readings this block (bass harmonics, air
        exciter; dB re their output, -160 = none): power-summed and smoothed
        like the THD+N, but kept apart from it (see the header comment).
        Returns the block's power sum. */
    float updateHarmonics (float bassDb, float airDb, int numSamples) noexcept FLUB_NONBLOCKING;
    float getHarmonicsBlockDb() const noexcept { return harmonicsBlockDb; }
    /** Block harmonics reading smoothed in the power domain (tau = kMeterTauSeconds). */
    float getSmoothedHarmonicsDb() const noexcept { return harmonicsSmoothedDb; }

private:
    /** Advances a power-domain one-pole (tau = kMeterTauSeconds) by numSamples. */
    float smooth (float& statePow, float blockPow, int numSamples) const noexcept FLUB_NONBLOCKING;

    double sr = 48000.0;
    float blockDb = -160.0f, smoothedDb = -160.0f, smoothedPow = 0.0f;
    float harmonicsBlockDb = -160.0f, harmonicsSmoothedDb = -160.0f, harmonicsSmoothedPow = 0.0f;
};

/** How far the SafetyGovernor reaches (see the header comment). */
enum class ProtectionStrength : int
{
    Off = 0,    // governed macro amounts only; base values untouched (the default)
    Normal = 1, // also the base max.drive, sat.drive and bass.harmonics
    Strict = 2  // as Normal, and the scale may fall to 0
};

/** Pre-maximizer peak statistics for the governor's feed-forward (docs/11
    E06 Phase 3, see the header comment): the maximizer input's peak per
    10 ms tick over the last 3 s, and the drive that would keep the
    limiter's average gain reduction at a budget. */
class DriveFeedForward
{
public:
    static constexpr int kTicks = 300; // 3 s of 10 ms ticks

    void reset() noexcept FLUB_NONBLOCKING;
    /** One tick's peak of the maximizer input (linear, before the drive). */
    void push (float peak) noexcept FLUB_NONBLOCKING;
    /** The drive (dB) at which the mean over the ticks of max (0, peak +
        drive - ceiling) equals -grBudgetDb; +inf while fewer than 50 ticks
        (0.5 s) of programme (peaks above -70 dBFS) are held. */
    float driveForBudget (float ceilingDb, float grBudgetDb) const noexcept FLUB_NONBLOCKING;

private:
    std::array<float, kTicks> peaksDb {};
    int count = 0, pos = 0;
};

/** Peak-to-loudness ratio of the chain's output over ~3 s (docs/11 E06
    Phase 3): the highest sample peak of the last 3 s of 10 ms ticks
    against a 3 s K-weighted loudness (its one-pole's cold start corrected,
    as in GatedLoudness). */
class PlrMeter
{
public:
    void prepare (double sampleRate, int numChannels);
    void reset() noexcept FLUB_NONBLOCKING;
    void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING;
    /** Closes a 10 ms tick: the peak since the last one joins the window. */
    void tick() noexcept FLUB_NONBLOCKING;
    /** PLR in dB; kNoReading while the loudness is below -50 LUFS or the
        window is not yet 1 s long. */
    float getPlrDb() const noexcept FLUB_NONBLOCKING;
    static constexpr float kNoReading = 1000.0f;

private:
    LoudnessFollower loudness;
    std::array<float, DriveFeedForward::kTicks> peaks {};
    int pos = 0, count = 0;
    float tickPeak = 0.0f;
    double logPole = 0.0;       // ln of the follower's one-pole coefficient
    std::int64_t samples = 0;   // since reset (the cold-start correction; an integer, so the reading does not depend on the blocks)
};

class SafetyGovernor
{
public:
    static constexpr float kGrBudgetDb = -6.0f;          // sustained limiting deeper than this = over-driven
    static constexpr float kDistortionBudgetDb = -30.0f; // THD+N above ~3.2 % RMS = audible distortion
    static constexpr float kHysteresisDb = 1.5f;
    static constexpr float kFallPerSec = 0.15f;
    static constexpr float kRisePerSec = 0.03f;
    static constexpr float kMinScale = 0.3f;       // floor at Off and Normal
    static constexpr float kStrictMinScale = 0.0f; // floor at Strict
    /** Tick length: the LoudnessMaximizer's limiter-GR window (the chain
        checks that the two agree). */
    static constexpr float kTickMs = 10.0f;

    /** What the loop did at its last tick. */
    enum class State : int
    {
        Idle = 0,       // scale 1, within budget
        BackingOff = 1, // over budget: the scale falls (or sits at its floor)
        Holding = 2,    // below 1, inside the hysteresis band: the scale holds
        Recovering = 3  // below 1, comfortably under budget: the scale rises
    };
    /** Bits of getReason(): the budgets that made the scale fall. */
    static constexpr uint32_t kReasonLimiter = 1u;    // ~3 s limiter GR average deeper than kGrBudgetDb
    static constexpr uint32_t kReasonDistortion = 2u; // ~3 s THD+N average above kDistortionBudgetDb
    static constexpr uint32_t kReasonDynamics = 4u;   // Normal / Strict: output PLR under its budget
    static constexpr uint32_t kReasonHarmonics = 8u;  // Normal / Strict: exposed bass harmonics over the budget
    static constexpr uint32_t kReasonTonal = 16u;     // Normal / Strict: the net tonal balance over its budget (docs/11 E07)

    /** The measured loop's budgets (Normal / Strict, docs/11 E06 Phase 3). */
    struct Budgets
    {
        float grDb = -6.0f;        // limiter GR, ~0.5 s average (dB <= 0)
        float residualDb = -35.0f; // audible span residual (WeightedResidual, dB re the output)
        float plrDb = 8.0f;        // output peak-to-loudness ratio; 0 = no dynamics budget
        // Net tonal balance (docs/11 E07): lift over the 200 Hz - 1 kHz lift (dB).
        float presenceDb = 3.0f;   // 2 - 5 kHz
        float harshDb = 3.0f;      // 5 - 10 kHz
        float airDb = 4.0f;        // 10 - 16 kHz
    };
    /** Programme that arrives with less PLR than the budget may lose at most
        this much more (a steady tone has about 3-5 dB; a mastered track 6-8). */
    static constexpr float kPlrAllowanceDb = 1.0f;
    /** The dynamics loop's set point over its budget (smaller than
        kSetPointMarginDb: much of a chain's PLR reduction is not the drive's). */
    static constexpr float kPlrMarginDb = 0.5f;
    /** Per strength and mode (music = Music mode, else Gaming). Off has no
        measured loop; it returns Normal's. */
    static Budgets budgetsFor (ProtectionStrength s, bool music) noexcept;

    /** One tick's readings for the measured loop (Normal / Strict). */
    struct Readings
    {
        float limiterGrDb = 0.0f;                 // deepest limiter GR in the window (dB <= 0)
        float distortionDb = -160.0f;             // the stage THD+N the Off loop uses (averaged for the meters)
        float driveResidualDb = -160.0f;          // audible residual of the drive span (saturator .. maximizer)
        float harmonicsResidualDb = -160.0f;      // audible residual of the bass engine's harmonics (-160: none)
        float plrDb = PlrMeter::kNoReading;       // output PLR over ~3 s
        float inputPlrDb = PlrMeter::kNoReading;  // the span input's PLR over ~3 s
        float feedForwardScale = 1.0f;            // drive scale the pre-maximizer peaks predict for the GR budget
        // The chain's net tonal lifts over its 200 Hz - 1 kHz lift (TonalBalanceMeter; -160: no reading).
        float presenceLiftDb = -160.0f, harshLiftDb = -160.0f, airLiftDb = -160.0f;
    };
    /** Measured-loop constants: PI gains on dB errors (u = 20 log10 scale),
        the set point below each budget, and the slew limits of u. */
    static constexpr float kPropGain = 0.3f;       // dB of u per dB of error
    static constexpr float kIntGain = 2.0f;        // dB of u per dB of error and second
    static constexpr float kTrimGain = 0.25f;      // the limiter loop's PI, a trim around the feed-forward
    static constexpr float kSetPointMarginDb = 1.5f;
    static constexpr float kHoldBandDb = 3.0f;     // under the set point: hold; further under: recover
    static constexpr float kApproachDb = 1.0f;     // added to an error over the set point (finite-time approach)
    static constexpr float kProbeMarginDb = 1.0f;  // recovery cap under the level of the last back-off ...
    static constexpr float kProbeHoldSeconds = 4.0f;    // ... held this long after it,
    static constexpr float kMaxProbeHoldSeconds = 64.0f; // doubling to this while probes fail at the same level
    static constexpr float kVerifySeconds = 0.5f;  // wait after stepping to the cap, before integrating on
    static constexpr float kProbeErrorDb = 6.0f;   // a back-off starting further over is not a probe
    static constexpr float kFallDbPerSec = 6.0f;            // drive scale (about the limiter's programme release)
    static constexpr float kHarmonicsFallDbPerSec = 12.0f;  // harmonics scale
    static constexpr float kRiseDbPerSec = 1.0f;
    static constexpr float kGrAverageSeconds = 0.5f;
    static constexpr float kFloorDb = -60.0f;      // u at which a floor-0 scale is taken as 0
    /** The tonal-balance rule (docs/11 E07): PI gain (a slow loop: its
        readings average over TonalBalanceMeter::kAverageSeconds), set
        point under the budget, fall limit of its u. */
    static constexpr float kTonalGain = 0.2f;
    static constexpr float kTonalMarginDb = 0.5f;
    static constexpr float kTonalFallDbPerSec = 3.0f;

    void prepare (double sampleRate) noexcept;
    void reset() noexcept FLUB_NONBLOCKING;
    /** Restarts the tick grid (not the loop state), for when the maximizer's
        window grid restarts without a full reset (a dropped block). */
    void restartTickGrid() noexcept FLUB_NONBLOCKING { pendingSamples = 0; }
    /** Samples until the next tick (1 .. one window): the chain ends its
        processing segments there, so a new scale takes effect on the grid. */
    int samplesToNextTick() const noexcept FLUB_NONBLOCKING { return tickSamples - pendingSamples; }
    /** Sets the scale's floor (Strict: 0, else kMinScale); any time on the
        audio thread. A scale below a raised floor is lifted to it. */
    void setStrength (ProtectionStrength s) noexcept FLUB_NONBLOCKING;
    /** Advances the tick grid by numSamples and ticks once per 10 ms window
        closed, on these readings. distortionDb: the distortion of the
        nonlinear stages this block (see the header comment;
        ProcessingChain::process). The stepwise loop at every strength;
        the chain calls updateMeasured() at Normal / Strict. */
    void update (float limiterGrDb, float distortionDb, int numSamples) noexcept FLUB_NONBLOCKING;
    /** As update(), with the measured loop's readings (Off uses only
        limiterGrDb and distortionDb, as above). */
    void updateMeasured (const Readings& readings, int numSamples) noexcept FLUB_NONBLOCKING;
    /** Selects the measured loop's budgets (Music or Gaming); any time on the
        audio thread. */
    void setMusicMode (bool music) noexcept FLUB_NONBLOCKING { musicMode = music; }
    /** Small Speaker Mode: the bass harmonics stand in for a fundamental the
        output no longer carries, so Normal leaves them alone (Strict still
        governs them). */
    void setHarmonicsReplaceFundamental (bool replacing) noexcept FLUB_NONBLOCKING { harmonicsReplace = replacing; }
    /** Advances the tick grid by numSamples without measuring: the windows
        closed here leave the averages and the scale as they are (a block
        hidden from the control loops, docs/11 E10). */
    void skip (int numSamples) noexcept FLUB_NONBLOCKING;
    float getScale() const noexcept { return scale; }
    /** Extra scale on the bass harmonics (Normal / Strict; 1 at Off). */
    float getHarmonicsScale() const noexcept { return harmonicsScale; }
    /** Scale on the ungoverned tonal lifts of the macros (docs/11 E07;
        Normal / Strict; 1 at Off). */
    float getTonalScale() const noexcept { return tonalScale; }
    /** Whether the last tick ran the measured loop (strength Normal / Strict). */
    bool isMeasuredLoop() const noexcept { return strength != ProtectionStrength::Off; }
    /** The ~3 s power average of the distortion input (what the budget is compared with). */
    float getAverageDistortionDb() const noexcept { return avgDistortionDb; }
    /** The ~3 s average of the limiter GR input (dB <= 0). */
    float getAverageGainReductionDb() const noexcept { return avgGrDb; }
    State getState() const noexcept { return state; }
    /** kReason* bits of the budgets that were over since the scale last left
        1; 0 while the scale is 1. */
    uint32_t getReason() const noexcept { return reason; }

private:
    /** One PI loop of the measured control (u in dB of scale). */
    struct Loop
    {
        float integral = 0.0f;
        /** Advances by one tick on error e (dB, > 0 = over its set point) and
            returns the candidate u (<= 0). offsetDb shifts the loop (the
            feed-forward); gain scales its PI gains (a trim around a
            feed-forward runs at kTrimGain). */
        float step (float e, float dt, float offsetDb, float gain = 1.0f) noexcept FLUB_NONBLOCKING;
        /** Anti-windup (a loop that asked for less than was applied starts
            the next tick from what was applied) and the probe memory: at the
            onset of a back-off, recovery is capped kProbeMarginDb under the
            level that went over, for a hold that doubles (up to
            kMaxProbeHoldSeconds) while back-offs keep starting within 1 dB of
            the last one, and restarts at kProbeHoldSeconds otherwise. At the
            onset the loop steps to that cap and waits kVerifySeconds (about
            the readings' lag) before it integrates further, so a failed
            probe costs kProbeMarginDb, not what the lag would add. */
        void track (float candidate, float applied, float dt) noexcept FLUB_NONBLOCKING;
        float lastError = 0.0f, lastApplied = 0.0f;
        float ceiling = 0.0f, holdLeft = 0.0f, holdTime = kProbeHoldSeconds, lastOnset = 1.0f, verifyLeft = 0.0f;
        bool wasOver = false;
    };

    void tick (float limiterGrDb, float distortionDb) noexcept FLUB_NONBLOCKING;
    void measuredTick (const Readings& r) noexcept FLUB_NONBLOCKING;
    /** Restarts the measured loop from the current scales. */
    void startMeasured() noexcept FLUB_NONBLOCKING;

    double sr = 48000.0;
    int tickSamples = 480, pendingSamples = 0;
    float tickAverage = 0.0f, tickFall = 0.0f, tickRise = 0.0f; // per-tick constants (prepare)
    float tickSeconds = 0.01f, grFastCoeff = 0.0f;
    float minScale = kMinScale;
    float avgGrDb = 0.0f, avgDistortionDb = -160.0f, scale = 1.0f;
    // Measured loop (Normal / Strict).
    ProtectionStrength strength = ProtectionStrength::Off;
    bool musicMode = true, harmonicsReplace = false, measuredRunning = false;
    float grFastDb = 0.0f, driveDb = 0.0f, harmonicsDb = 0.0f, harmonicsScale = 1.0f, tonalDb = 0.0f, tonalScale = 1.0f;
    Loop grLoop, residualLoop, plrLoop, harmonicsLoop, tonalLoop;
    State state = State::Idle;
    uint32_t reason = 0;
};

class AutoLevel
{
public:
    static constexpr float kMaxGainDb = 6.0f;   // upward cap (docs/11 E21; was +12)
    static constexpr float kMinGainDb = -12.0f;
    static constexpr float kUpDbPerSec = 1.0f;
    static constexpr float kDownDbPerSec = 4.0f;
    static constexpr float kRecoveryUpDbPerSec = 3.0f; // for kRecoverySeconds after a freeze
    static constexpr double kRecoverySeconds = 2.0;

    void prepare (double sampleRate, int numChannels);
    void reset() noexcept;
    void setTargetLufs (float lufs) noexcept { target = lufs; }
    void setEnabled (bool on) noexcept { enabled = on; }

    /** Measures and applies the levelling gain in place. */
    void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING;
    /** For a block hidden from the control loops (docs/11 E10): not
        measured, the gain holds (as in a pause) and is applied in place. */
    void processUnmeasured (const AudioBlock& block) noexcept FLUB_NONBLOCKING;
    float getGainDb() const noexcept { return gainDb; }
    /** True while the upper gate holds the gain through a loud event. */
    bool isHeld() const noexcept { return follower.isHeld(); }

private:
    void run (const AudioBlock& block, bool measure) noexcept FLUB_NONBLOCKING;

    GatedLoudness follower;
    double sr = 48000.0, recoveryLeft = 0.0;
    float target = -18.0f, gainDb = 0.0f, lastLinear = 1.0f;
    bool enabled = false, frozen = false;
};

class AutoDrive
{
public:
    void prepare (double sampleRate, int numChannels);
    void reset() noexcept;
    /** Feed the post-chain output; returns the drive reduction (dB <= 0) to
        apply at the next block. The reduction never goes below
        -requestedDriveDb: past that the drive is already 0 dB and further
        "reduction" would change nothing audible while delaying recovery. */
    float update (const AudioBlock& output, float targetLufs, bool enabled, float requestedDriveDb) noexcept;
    float getReductionDb() const noexcept { return reductionDb; }

private:
    GatedLoudness follower;
    double sr = 48000.0;
    float reductionDb = 0.0f;
};

class ComparisonMatcher
{
public:
    static constexpr double kAcquireSeconds = 1.0;
    static constexpr double kSessionEndSeconds = 10.0; // bypass off this long ends a comparison
    static constexpr float kReleaseDbPerSec = 2.0f;
    static constexpr float kMaxTrimDb = 20.0f;
    static constexpr double kSubBlockSeconds = 0.1;
    static constexpr int kWindowSubBlocks = 30; // 3 s of programme
    static constexpr int kMinSubBlocks = 4;     // measure valid after 0.4 s of programme

    void prepare (double sampleRate, int numChannels);
    void reset() noexcept;
    void measureDry (const AudioBlock& dry) noexcept FLUB_NONBLOCKING;
    void measureWet (const AudioBlock& wet) noexcept FLUB_NONBLOCKING;
    /** Closes the block both sides were measured on and advances the
        comparison by its numSamples: bypassEngaged = the global bypass is on,
        matching = loudness matching is on. */
    void update (bool bypassEngaged, bool matching, int numSamples) noexcept FLUB_NONBLOCKING;
    /** As update(), for a block neither side was measured on (hidden from
        the control loops, docs/11 E10): it does not count towards the
        current 100 ms sub-block. */
    void updateUnmeasured (bool bypassEngaged, bool matching, int numSamples) noexcept FLUB_NONBLOCKING;
    /** Applies the wet trim to the processed block in place, as a linear
        ramp from the last block's value (click-free). */
    void applyWetTrim (const AudioBlock& wet) noexcept FLUB_NONBLOCKING;

    /** Trim (dB, <= 0) for the dry reference. */
    float getDryTrimDb() const noexcept { return dryTrimDb; }
    /** Trim (dB, <= 0) for the processed output. */
    float getWetTrimDb() const noexcept { return wetTrimDb; }
    /** Processed minus dry loudness over the window (LU); valid only while
        hasMeasurement(). */
    float getDifferenceLu() const noexcept { return differenceLu; }
    bool hasMeasurement() const noexcept { return measured; }
    bool isComparing() const noexcept { return comparing; }
    /** True once the trims of the current comparison are frozen. */
    bool isFrozen() const noexcept { return comparing && acquiredSamples >= acquireSamples; }

private:
    /** K-weighted, channel-weighted sum of squares of one side (BS.1770). */
    struct Side
    {
        Biquad stage1, stage2;
        double subBlock = 0.0;
        std::array<double, kWindowSubBlocks> ring {};
        void prepare (double sampleRate) noexcept;
        void reset() noexcept;
        void process (const AudioBlock& block, int channels) noexcept FLUB_NONBLOCKING;
        double windowSum() const noexcept;
    };
    void closeSubBlock() noexcept FLUB_NONBLOCKING;
    void advance (bool bypassEngaged, bool matching, int numSamples, bool measuredBlock) noexcept FLUB_NONBLOCKING;

    Side dry, wet;
    double sr = 48000.0;
    int channels = 2, ringPos = 0, ringCount = 0;
    std::int64_t subBlockSamples = 4800, pendingSamples = 0;
    std::int64_t acquireSamples = 48000, sessionEndSamples = 480000;
    std::int64_t acquiredSamples = 0, offSamples = 0;
    float dryTrimDb = 0.0f, wetTrimDb = 0.0f, lastWetGain = 1.0f, differenceLu = 0.0f;
    bool comparing = false, measured = false;
};
} // namespace flub
