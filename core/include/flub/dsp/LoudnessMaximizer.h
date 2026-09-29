// Flubsound Pro - intelligent true-peak loudness maximizer + soft clipper.
//
//   x * drive
//     Bed-lift budget (bedLiftDb, docs/11 E19 step 3): the drive is loudness
//     only where it pushes the programme into the limiter; on quiet
//     programme the chain's static lifts (EQ, compressor make-up, drive)
//     just raise everything - a game's ambience bed with its cues. With a
//     budget B, programme that stays well clear of the ceiling is lifted at
//     most B dB in all: the drive becomes D - w max (0, U + D - B), where U
//     is the lift upstream of the maximizer (setUpstreamLiftDb(), measured
//     by the chain) and w follows the input's headroom h = ceiling - P, P =
//     its peak (instant attack, 400 ms release): 0 where D reaches the
//     ceiling (h <= D), 1 from 6 dB of spare headroom on, linear between.
//     The drive may go down to D - 12 dB; it glides with 20 ms. Loud
//     programme (combat) keeps D. 24 dB = no budget (the default).
//     -> [glue] 3-band pre-compression (ThreeBandSplitter 120 Hz / 4 kHz,
//        linked per band, ratio 2:1, threshold ceiling - 6 dB, 5/80 ms,
//        amount = glue). Balances the band levels so one band (usually the
//        bass) does not dominate the limiter: louder AND less pumping.
//        LF-first limiter (lfLimit, docs/11 E05 step 5): the low band alone
//        is limited 3 dB under the ceiling before the bands are summed, from
//        its own detector (the band's held peak over half a period of 30 Hz,
//        1 ms attack, 250 ms release), so a kick's body is taken down in the
//        low band instead of by the wideband limiter, which would duck the
//        vocals and pads with it. The held peak keeps the gain flat on
//        steady bass; gain = 1 + lfLimit (min (1, T / env) - 1). The stage
//        runs while glue or lfLimit is above 0.
//     -> [clip] oversampled soft clipper (clipOversampling x, default 4x):
//        threshold t = ceilingLin * 10^(clipHeadroomDb/20), with
//        clipHeadroomDb = lerp(+6 dB, +0.3 dB, clipAmount); soft knee:
//          |x| <= ks          : y = x                   ks = t (1 - 0.5 knee)
//          |x| >  ks          : y = sign * (ks + (t - ks) tanh((|x| - ks)/(t - ks)))
//        Shaves sub-millisecond transients (snare/gunshot crack) cheaply, so
//        the limiter only handles the longer peaks -> less audible pumping.
//        clipAmount = 0 disables the clipper entirely (limiter-only).
//        Crest gate and depth cap (docs/11 E05 stage 1): the threshold is
//        raised to clipCrestDb above the signal's short-term RMS (linked
//        max-channel power, two cascaded 5 ms one-poles), t' = max(t,
//        10^(crest/20) rms), so steady tones and bass (crest 3 dB) are never
//        clipped and only transients that stand out of their own level are;
//        and each sample loses at most clipMaxDepthDb:
//          |y| = smoothmax(|clip(x)|, 10^(-depth/20) |x|)
//        (a C1 quadratic blend over +-0.45 (1 - 10^(-depth/20)) t'), so drive
//        beyond it becomes limiter gain reduction instead of clip depth.
//        Delta design: out = x + HP5(clip(x^) - x^), the correction band-
//        limited by the downsampler and passed through a 5 Hz 1st-order
//        high-pass (docs/11 E10), so clipping an asymmetric waveform leaves
//        no DC on the driver.
//     -> [limit] TruePeakLimiter at the ceiling (look-ahead, true peak),
//        with the LF-safe envelope (LimiterEnvelope: a gain hold over the
//        peak spacing of periodic waveforms, S-shaped attack, program
//        envelope; setLimiterEnvelope(), all on).
// Telemetry: clipEnergyRatioDb = 10 log10(sum (x - clip(x))^2 / sum x^2) over
//   the last block (how hard the clipper works), the limiter's gain
//   reduction, and distortionDb: the clipper's THD+N over the last analysis
//   window (closed on the 10 ms GR-window grid at or after 25 ms: 30 ms),
//   measured at the oversampled rate around the curve itself, where input
//   and clipped output are aligned (DistortionEstimator.h: residual after the
//   least-squares gain, relative to the output energy). The chain feeds the
//   limiter GR (deepest per 10 ms window, getWindowGainReductionDb()) and the combined THD+N of the nonlinear stages (this one
//   floored at the clip energy ratio over the same window,
//   getWindowClipEnergyDb()) to the SafetyGovernor, which backs the
//   governed macro contributions off when either exceeds its budget.
//   Whole-stage residual (docs/11 E06 step 1): the output of the clipper
//   and the limiter against their input (after the drive and the glue
//   stage), delayed by the stage's latency, over the same >= 25 ms windows:
//   the least-squares gain per window and channel takes the ceiling and
//   the average gain reduction, the residual is what the clipper and the
//   limiter's gain modulation (IMD) add - the limiter's share that the
//   clipper's own THD+N cannot see (getResidualDistortionDb()).
// latency = clipper oversampler latency + limiter latency.
#pragma once

#include "Crossover.h"
#include "DistortionEstimator.h"
#include "Oversampler.h"
#include "Processor.h"
#include "TruePeakLimiter.h"
#include "flub/common/DelayLine.h"
#include "flub/common/SmoothedValue.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

namespace flub
{
struct MaximizerParams
{
    float driveDb = 0.0f;    // 0 .. 24
    float ceilingDb = -1.0f; // -12 .. 0 dBTP
    float clipAmount = 0.5f; // 0 .. 1
    float clipKnee = 0.5f;   // 0 .. 1
    float glue = 0.0f;       // 0 .. 1
    float releaseMs = 60.0f; // 5 .. 1000
    bool autoRelease = true;
    float clipCrestDb = 6.0f;    // 0 .. 24; 0 = no crest gate (clip at t)      (max.clipCrest)
    float clipMaxDepthDb = 3.0f; // 0.5 .. 24; 24 = depth not capped           (max.clipMaxDb)
    float lfLimit = 0.0f;        // 0 .. 1; LF-first limiter in the glue path   (max.lfLimit)
    float bedLiftDb = 24.0f;     // 0 .. 24; drive budget below the ceiling  (max.bedLift; 24 = none)

    bool operator== (const MaximizerParams&) const = default;
};

class LoudnessMaximizer final : public Processor
{
public:
    /** Structural: call before prepare(). A fixed half-band design, factor 1, 2 or 4. */
    void setClipOversampling (int factor, Oversampler::Quality q = Oversampler::Quality::High) noexcept
    {
        clipOsDesign = Oversampler::design (factor <= 1 ? 1 : (factor <= 2 ? 2 : 4), q);
    }
    /** Structural: an explicit design (factor 1, 2, 4 or 8, no ADAA:
        Oversampler::forClipper, docs/11 E10). */
    void setClipOversampling (const Oversampler::Design& design) noexcept
    {
        clipOsDesign = design;
        clipOsDesign.adaa = false;
    }
    void setLookaheadMs (float ms) noexcept { lookaheadMs = ms; }
    void setTruePeakDetection (bool enabled) noexcept { truePeak = enabled; }
    /** The final limiter's LF-safe envelope stages (default: all on). */
    void setLimiterEnvelope (const LimiterEnvelope& e) noexcept { limiterEnvelope = e; }

    void prepare (const ProcessSpec& spec) override;
    void reset() noexcept FLUB_NONBLOCKING override;
    void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING override;
    int latencySamples() const noexcept override;
    const char* name() const noexcept override { return "Loudness Maximizer"; }

    void setParams (const MaximizerParams& p) noexcept FLUB_NONBLOCKING;
    /** The static lift ahead of the maximizer (dB, output re input of the
        chain's modules), for the bed-lift budget; the chain sets it per block. */
    void setUpstreamLiftDb (float db) noexcept FLUB_NONBLOCKING { upstreamLiftDb = std::isfinite (db) ? std::clamp (db, -24.0f, 24.0f) : 0.0f; }
    /** The budget's weight at the end of the last block: 1 on quiet
        programme, 0 where the drive reaches the ceiling (audio thread). */
    float getBedQuietWeight() const noexcept { return bedQuiet; }
    const MaximizerParams& getParams() const noexcept { return params; }

    /** Soft-clip transfer curve (threshold t, knee 0..1), exposed for tests/GUI. */
    static float softClip (float x, float threshold, float knee) noexcept;
    /** softClip with the depth cap: |y| >= depthGain |x| (C1 blend), depthGain
        = 10^(-clipMaxDepthDb / 20); depthGain 0 = uncapped (softClip itself). */
    static float softClipCapped (float x, float threshold, float knee, float depthGain) noexcept;

    /** Deepest limiter gain reduction in the last block (dB <= 0; the meter). */
    float getGainReductionDb() const noexcept { return limiterGrDb.load (std::memory_order_relaxed); }
    /** Deepest limiter gain reduction per fixed kGrWindowMs window (dB <= 0),
        the mean over the windows that closed in the last block that closed
        any: the SafetyGovernor's GR input, independent of the host block size. */
    float getWindowGainReductionDb() const noexcept { return windowGrDb.load (std::memory_order_relaxed); }
    static constexpr float kGrWindowMs = 10.0f;
    /** Engagements of the limiter's final safety clamp since prepare() (0 in normal operation). */
    uint64_t getSafetyClipCount() const noexcept { return limiter.getSafetyClipCount(); }
    float getGlueReductionDb() const noexcept { return glueGrDb.load (std::memory_order_relaxed); }
    /** Deepest gain reduction of the LF-first limiter in the last block (dB <= 0, its amount applied). */
    float getLfReductionDb() const noexcept { return lfGrDb.load (std::memory_order_relaxed); }
    float getClipEnergyRatioDb() const noexcept { return clipRatioDb.load (std::memory_order_relaxed); }
    /** The clipper's input energy behind that ratio (sum x^2 at the
        oversampled rate, last block; 0 while the clipper is off), so a caller
        that splits a block can combine the ratios exactly. */
    float getClipInputEnergy() const noexcept { return clipInputEnergy.load (std::memory_order_relaxed); }
    /** THD+N of the soft clipper over the last 25 ms analysis window (dB re its output; -160 = clean or off). */
    float getDistortionDb() const noexcept { return distortionDb.load (std::memory_order_relaxed); }
    /** The clip energy ratio over that same window (the chain floors the
        governor's clipper input with it; getClipEnergyRatioDb() is per block). */
    float getWindowClipEnergyDb() const noexcept { return windowClipDb.load (std::memory_order_relaxed); }
    /** THD+N of the clipper and the limiter together over the last analysis
        window: the output against the latency-aligned input of the clipper
        (see the header comment; dB re the output, -160 = clean). */
    float getResidualDistortionDb() const noexcept { return residualDb.load (std::memory_order_relaxed); }

private:
    // ---- implementation-defined below this line ----
    // Notes (details in LoudnessMaximizer.cpp):
    //  * Continuous parameters glide over 50 ms; the glue and clipper stages
    //    are switched on/off by crossfading against a latency-aligned dry path
    //    (after a short warm-up of the stage), so on/off is click-free and the
    //    latency never changes.
    //  * Glue detector per band: linked |x| -> two-bucket peak hold (half a
    //    period of the band's lowest frequency) -> 5/80 ms branching follower
    //    -> ratio-2 gain sqrt(T / env) above T.
    //  * The final limiter holds the sample ceiling exactly. Its true-peak
    //    accuracy (<= +0.1 dB) needs the signal inside the 4x detector band
    //    (flat to 0.4535 fs, see TruePeakDetector.h); very hard clipping
    //    creates intermodulation up to fs/2, where every 4x interpolator
    //    rolls off, so an ideal reconstruction can read slightly higher
    //    (measured: every factory preset at full macros stays below the
    //    ceiling on the 4x meter, worst -1.04 dBTP for a -1 dBTP ceiling).

    /** Linked level detector of one glue band. */
    struct BandDetector
    {
        float bucket = 0.0f, prevBucket = 0.0f, env = 0.0f;
        int countdown = 1, holdLength = 1;
        float attack = 0.0f, release = 0.0f;
    };

    static constexpr int kNumBands = 3;

    static MaximizerParams sanitised (const MaximizerParams& p, const MaximizerParams& fallback) noexcept;
    /** The glue stage (splitter) runs for the glue and for the LF-first limiter. */
    static bool bandStageWanted (const MaximizerParams& p) noexcept { return p.glue > 0.0f || p.lfLimit > 0.0f; }
    void applyParamsImmediately() noexcept;
    void startGlue (bool immediate) noexcept;
    void startClipper (bool immediate) noexcept;
    void updateCeiling (float newCeilingDb) noexcept;
    void updateClipThreshold() noexcept;
    void updateClipShape() noexcept;
    void processSegment (const AudioBlock& seg, double& clipDiffEnergy, double& clipInEnergy, float& glueMinGain,
                         float& lfMinGain) noexcept;

    Oversampler::Design clipOsDesign = Oversampler::design (4, Oversampler::Quality::High);
    float lookaheadMs = 1.5f;
    bool truePeak = true;
    LimiterEnvelope limiterEnvelope { true, true, true };
    ProcessSpec spec;
    MaximizerParams params;
    std::atomic<float> limiterGrDb { 0.0f }, glueGrDb { 0.0f }, lfGrDb { 0.0f }, clipRatioDb { -160.0f }, distortionDb { -160.0f },
        windowClipDb { -160.0f }, windowGrDb { 0.0f }, clipInputEnergy { 0.0f }, residualDb { -160.0f };
    int grWindowLength = 480, grWindowCount = 0; // limiter GR window (kGrWindowMs), samples
    float grWindowMin = 0.0f;                    // deepest GR in the open window (dB)
    int analysisPending = 0;                     // samples since the last grid point (analysis windows)
    DistortionWindow distortionWindow; // clipper THD+N sums over a window of at least 25 ms (closes on the GR-window grid)
    // Whole-stage residual: the clipper's input, delayed by the clipper and
    // limiter latency, against the limiter's output, over the same windows.
    DistortionWindow residualWindow;
    DelayLine residualDelay;
    AudioBuffer residualRef;

    bool prepared = false;
    bool fresh = true;           // nothing processed since prepare()/reset(): setParams() applies instantly

    // Silence for prepared channels that a block leaves out: every stage (dry
    // delay, oversampler, splitter, limiter) keeps running on all prepared
    // channels, so no stale audio is released when a wider block returns.
    AudioBuffer padBuffer;

    // Clipper: oversampled soft clip, crossfaded against a dry path delayed by
    // exactly the oversampler round trip.
    int osFactor = 1;
    Oversampler oversampler;
    DelayLine dryDelay;
    AudioBuffer dryBuffer;
    std::vector<float> thresholdBuf, kneeBuf, clipMixBuf, depthBuf; // per-sample clip controls (maxBlockSize)
    bool clipRunning = false;
    int clipWarmup = 0, clipWarmupLength = 1;
    // Residual-path DC blocker on the clipper's correction (docs/11 E10):
    // TPT one-pole low-pass state per channel (double) and its coefficient.
    std::array<double, kMaxChannels> clipDcLp {};
    double clipDcG = 0.0;
    // Crest gate: short-term linked power of the clipper input and its
    // coefficient; crest and depth gains from the params (0 = off), gliding
    // linearly (in gain) over kParamSmoothMs, so moving either is click-free.
    float clipPowerFast = 0.0f, clipPower = 0.0f, clipPowerCoeff = 0.0f;
    LinearSmoothedValue crestGainS, depthGainS;

    // Glue: 3-band split, per-band linked compressor, crossfaded against the input.
    ThreeBandSplitter splitter;
    std::array<BandDetector, kNumBands> bands {};
    bool glueRunning = false;
    int glueWarmup = 0, glueWarmupLength = 1;
    float antiDenormal = 0.0f;   // alternates 1e-20 / 0 on the splitter input
    // Bed-lift budget: the input's peak follower, the drive cap (dB) and
    // their coefficients.
    float bedPeak = 0.0f, bedCapDb = 24.0f, bedPeakRelease = 0.0f, bedCapCoeff = 0.0f, upstreamLiftDb = 0.0f;
    float bedQuiet = 1.0f; // w: 1 = quiet programme (the budget applies in full), 0 = the drive reaches the ceiling
    bool bedCapActive = false;
    // LF-first limiter: its follower of the low band's held peak (after the
    // glue gain), coefficients, threshold and smoothed amount.
    float lfEnv = 0.0f, lfAttack = 0.0f, lfRelease = 0.0f, lfThreshold = 1.0f;
    LinearSmoothedValue lfLimitS;

    TruePeakLimiter limiter;

    // Smoothed controls.
    LinearSmoothedValue driveDbS, ceilingDbS, clipAmountS, clipKneeS, glueS, glueMixS, clipMixS;
    float driveGain = 1.0f, ceilingDb = -1.0f, ceilingLin = 1.0f, glueThreshold = 0.5f, clipThreshold = 1.0f;
};
} // namespace flub
