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
//   clipping than it did on the proxy alone. Budget: limiter GR averaged over ~3 s must stay above
//   -6 dB, distortion (power average over ~3 s) below -30 dB (~3.2 % RMS of
//   the output). When over budget, scale falls at 15 %/s (min 0.3); when
//   under budget minus 1.5 dB hysteresis it recovers at 3 %/s. The scale
//   multiplies every "governed" macro amount.
//   The loop ticks on a fixed 10 ms grid counted from reset() - the grid the
//   maximizer's limiter-GR windows close on - with dt = 10 ms, however the
//   host splits the audio into blocks (docs/11 E06 slice): a block that
//   closes no window leaves the scale alone, one that closes k windows ticks
//   k times on its readings. A per-block tick (dt = block length) moved the
//   scale on the block grid, which alone spread Boost 100 + Loudness 100 by
//   0.29 LU over blocks 64-4096 on kick-heavy programme.
//   Protection strength (ProtectionStrength, a host setting, not a preset
//   value): Off (the default) never touches base values, as before; Normal
//   also scales the base max.drive, sat.drive and bass.harmonics by the same
//   scale (ProcessingChain::applyParameters); Strict does that and lets the
//   scale fall to 0 instead of 0.3.
//   getState() / getReason() say what the loop is doing and which budget
//   made it back off (published on MeterBus for the UI and the CLI).
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

    void prepare (double sampleRate) noexcept;
    void reset() noexcept FLUB_NONBLOCKING;
    /** Restarts the tick grid (not the loop state), for when the maximizer's
        window grid restarts without a full reset (a dropped block). */
    void restartTickGrid() noexcept FLUB_NONBLOCKING { pendingSamples = 0; }
    /** Sets the scale's floor (Strict: 0, else kMinScale); any time on the
        audio thread. A scale below a raised floor is lifted to it. */
    void setStrength (ProtectionStrength s) noexcept FLUB_NONBLOCKING;
    /** Advances the tick grid by numSamples and ticks once per 10 ms window
        closed, on these readings. distortionDb: the distortion of the
        nonlinear stages this block (see the header comment;
        ProcessingChain::process). */
    void update (float limiterGrDb, float distortionDb, int numSamples) noexcept FLUB_NONBLOCKING;
    /** Advances the tick grid by numSamples without measuring: the windows
        closed here leave the averages and the scale as they are (a block
        hidden from the control loops, docs/11 E10). */
    void skip (int numSamples) noexcept FLUB_NONBLOCKING;
    float getScale() const noexcept { return scale; }
    /** The ~3 s power average of the distortion input (what the budget is compared with). */
    float getAverageDistortionDb() const noexcept { return avgDistortionDb; }
    /** The ~3 s average of the limiter GR input (dB <= 0). */
    float getAverageGainReductionDb() const noexcept { return avgGrDb; }
    State getState() const noexcept { return state; }
    /** kReason* bits of the budgets that were over since the scale last left
        1; 0 while the scale is 1. */
    uint32_t getReason() const noexcept { return reason; }

private:
    void tick (float limiterGrDb, float distortionDb) noexcept FLUB_NONBLOCKING;

    double sr = 48000.0;
    int tickSamples = 480, pendingSamples = 0;
    float tickAverage = 0.0f, tickFall = 0.0f, tickRise = 0.0f; // per-tick constants (prepare)
    float minScale = kMinScale;
    float avgGrDb = 0.0f, avgDistortionDb = -160.0f, scale = 1.0f;
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
