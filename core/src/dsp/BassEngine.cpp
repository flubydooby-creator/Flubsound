// Flubsound Pro - bass engine: adaptive boost + psychoacoustic harmonics.
//
// Per sample, in this order (all IIR, zero latency unless a look-ahead is set):
//
//   1. Subsonic HP   : Butterworth 4th order (2 SVF sections) at subsonicHz.
//   2. Mono bass     : stereo only. Per channel LR4 split at monoBelowHz;
//                      out_c = (low_L + low_R) / 2 + high_c.
//   3. Adaptive shelf: detector = linked max_c |LP2_150Hz (x_c)| -> 25 ms peak
//                      hold -> 10 / 150 ms peak follower -> level L (dBFS).
//                      excess = L + boostDb - protectThresholdDb
//                      withdraw = clamp (softKnee6dB (excess), 0, boostDb);
//                      low shelf (Q 0.7, half gain at boostFrequency) with
//                      gain smooth_5ms (boostDb - withdraw); getProtectionDb()
//                      publishes smooth_5ms (withdraw). The detector LP sits
//                      at max (150 Hz, 1.5 boostFrequency).
//                      splitProtection (docs/11 E02 (a)): sub = LR4 low
//                      at 85 Hz -> 25 ms hold -> 10 / 150 ms follower;
//                      punch = the detector LP above with an 8 ms hold.
//                      The shelf withdraws the sub's excess, a bell (Q 0.5
//                      at 0.8 sqrt (60 Hz x detector LP)) cuts what the
//                      punch prediction exceeds beyond that (at most the
//                      shelf's boost at the bell), each through a
//                      program-dependent hold (BandProtector::update).
//                      Look-ahead L (setLookaheadMs, default 0): the
//                      split detectors (1 ms attack) read x[n], everything
//                      from the classic detector on reads x[n - L], and
//                      the split withdrawals apply without the 5 ms
//                      smoothing (their releases keep it).
//   3a. Punch        : (docs/11 E20; before the protection detector)
//                      band_c = LR4 HP 40 Hz -> LR4 LP 150 Hz (x_c);
//                      w = TransientShaper::computeOnset (max_c |band_c|)
//                      (low-band timing: 25 ms hold, 10 ms slow attack,
//                      program-dependent release); o = clamp ((w - 0.4) /
//                      0.6, 0, 1) (under 2.4 dB of rise is not an onset:
//                      the held level of LF noise rumbles by about that);
//                      env: rises to o at once, held 80 ms after its last
//                      rise, then * 60 ms one-pole (flushed under 1e-4),
//                      nothing for 50 ms after a switch-on or reset (the
//                      detector learns the programme first);
//                      L = 25 ms peak hold of max_c |band_c| (dBFS);
//                      lift = min (6 dB * punch * env, max (0, threshold -
//                      L)) through a 2 ms rise / 20 ms fall one-pole (dB);
//                      out_c = x_c + (10^(lift/20) - 1) BP (x_c), BP the
//                      unity band-pass at 77.5 Hz, Q 0.7 (a bell of the
//                      lift; exactly x_c at 0 dB). The harmonics
//                      generator's mix gains punch * env while it runs.
//   4. Harmonics     : mid = mean of the channels -> HP2 25 Hz -> LP4 cutoff
//                      -> envelope-normalised Chebyshev waveshaper (header)
//                      -> HP2 cutoff -> LP4 6 * cutoff -> * 2 * amount,
//                      added to every channel after the optional
//                      replace-fundamental HP4 at cutoff on the original.
//                      Telemetry at that sum, per channel: the dry path,
//                      the shaper input through the same HP2 / LP4 and mix
//                      (the linear branch) and the harmonics
//                      (ParallelDistortion.h).
//   5. Tighten       : LR4 split at 150 Hz, TransientShaper (sustain =
//                      -12 dB * tighten, gated by the onset so it
//                      never cuts an onset) detecting max_c |low_c|; its
//                      gain g is applied as a dynamic low shelf, out_c = x_c
//                      + (g - 1) LP1_150Hz (x_c) (docs/11 E04 step 2). The
//                      LR4 bands only feed the detector: their sum is an
//                      all-pass (about 3 ms of group delay under 100 Hz),
//                      which as the output (g low + high, before) took
//                      1.5 dB off a kick's first 10 ms at any setting.
//                      The one-pole's lag bounds the cut: -6 dB (tighten
//                      0.5) reads -4.9 / -3.2 / -2.0 dB at 50 / 100 / 150 Hz,
//                      and never lifts anything.
//
// Control rate: every kControlInterval samples of absolute stream time (the
// counter survives across process() calls, so the output does not depend on
// the host block size) the smoothers advance, the protection is computed and
// filter designs are refreshed. Across the following interval the filters
// glide per sample to the new designs: the shelf interpolates (g, k, m0..m2)
// (TransientShaper::SvfGlide), the fixed-Q LP / HP / LR4 sections interpolate
// their prewarped frequency g (GGlide); a1..a3 are re-derived per sample, so
// neither gain changes nor corner sweeps produce 16-sample steps.
//
// Switching subsonic / mono / replace / tighten in or out uses ParkedStage
// (see header). Engaging: the stage starts with its corner parked at 10 Hz
// (subsonic: 5 Hz), where the processed path differs from the dry one
// essentially only below the audible band, crossfades in over 20 ms, and
// only then glides (25 ms one-pole in log frequency) to its target.
// Disengaging runs the same sequence backwards (the tighten stage also waits
// for its shaper gain to return to exactly 1). Corner changes of an engaged
// stage are plain glides.
//
// Detector envelopes use a peak hold so that a steady bass note gives a
// constant level: no ripple, hence no gain modulation distortion from the
// protection and exact harmonics from the waveshaper.
//
// State hygiene: every control tick filter states below 1e-20 are flushed to
// zero and a non-finite state (NaN / Inf input) resets all states.
#include "flub/dsp/BassEngine.h"

#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>

namespace flub
{
namespace
{
constexpr float kParamSmoothMs = 20.0f;     // dB values, amounts, character
constexpr float kFreqGlideMs = 25.0f;       // one-pole in log frequency
constexpr float kBlendMs = 20.0f;           // parked-stage crossfade
constexpr float kSplitParkHz = 10.0f;       // LR4 splits, replace-fundamental HP
constexpr float kSubsonicParkHz = 5.0f;     // lowest frequency an SVF is designed for
constexpr float kParkLogTolerance = 0.05f;  // within 5 % of the park frequency

constexpr double kButterworthQ2 = 0.70710678118654752;

// 3. Adaptive boost.
constexpr double kShelfQ = 0.7;
constexpr double kDetectorHz = 150.0;
constexpr double kDetectorTrackRatio = 1.5; // detector LP >= 1.5 x boostFrequency (see designDetector)
constexpr float kDetectorAttackMs = 10.0f;
constexpr float kDetectorReleaseMs = 150.0f;
constexpr float kShelfGainSmoothMs = 5.0f;
constexpr float kProtectionKneeDb = 6.0f;
constexpr double kHoldMs = 25.0; // half a period of 20 Hz

// 4. Harmonics.
constexpr double kHarmonicsLowHz = 25.0;
constexpr double kHarmonicsUpperRatio = 6.0;
constexpr float kHarmEnvAttackMs = 0.5f;
constexpr float kHarmEnvReleaseMs = 50.0f;
constexpr float kHarmonicsMaxGain = 2.0f; // +6 dB at amount 1
constexpr std::array<float, 4> kEvenWeights { 1.0f, 0.35f, 0.3f, 0.1f };  // w2..w5, character 0
constexpr std::array<float, 4> kOddWeights { 0.35f, 1.0f, 0.1f, 0.3f };   // w2..w5, character 1

// 3b. Split-band protection (docs/11 E02 (a)).
constexpr double kSplitHz = 60.0;          // sub | punch
constexpr double kSubDetectorHz = 85.0;    // the sub detector's LR4 low band: 60 Hz x sqrt 2, reads -1.9 dB at 60 Hz
constexpr double kSubHoldMs = 25.0;        // as the classic detector
constexpr double kPunchHoldMs = 8.0;
constexpr double kBellQ = 0.5;             // wide: covers 60 .. 150 Hz ...
constexpr double kBellCentre = 0.8;        // ... centred 0.8 x sqrt (60 Hz x detector LP), 76 Hz at 150 Hz;
                                           // a tone anywhere in the band then stays <= 0.5 dB over the cap
constexpr float kProgramMaxSpacingMs = 1200.0f; // onsets further apart are isolated
constexpr float kProgramReleaseMs = 150.0f;     // after the hold, towards the raw withdrawal
constexpr double kValleyRiseMs = 300.0;         // onset detector's valley: rise time constant
constexpr float kLookaheadAttackMs = 1.0f;      // split detectors' attack with a look-ahead (setLookaheadMs)

// 3a. Impact's punch (docs/11 E20).
constexpr double kImpactLowHz = 40.0, kImpactHighHz = 150.0; // the detector band (LR4 each side)
constexpr double kImpactBellHz = 77.5;                      // sqrt (40 x 150)
constexpr double kImpactBellQ = 0.7;
constexpr float kImpactMaxDb = 6.0f;          // the lift at punch 1 (the old Impact shelf's 6 dB, on onsets only)
constexpr float kImpactHarmonicsMix = 1.0f;   // harmonics mix at punch 1 (the old Impact row's 0.25 x 2 was 0.5; bursts only)
constexpr float kImpactOnsetFloor = 0.4f;     // indicator weight read as no onset (2.4 of its 6 dB)
constexpr float kImpactHoldMs = 80.0f;
constexpr float kImpactReleaseMs = 60.0f;
constexpr float kImpactRiseMs = 2.0f, kImpactFallMs = 20.0f;
constexpr double kImpactLevelHoldMs = 25.0;
constexpr float kImpactWarmMs = 50.0f;        // 5 slow attacks of the detector: its envelopes within 1 % of the programme
constexpr float kImpactEnvFloor = 1.0e-4f;

// 5. Tighten.
constexpr float kTightenHz = 150.0f;
constexpr float kTightenMaxDb = -12.0f;

// Parameter ranges (header).
constexpr float kMaxBoostDb = 15.0f;
constexpr float kMinBoostHz = 30.0f, kMaxBoostHz = 200.0f;
constexpr float kMinProtectDb = -30.0f, kMaxProtectDb = 0.0f;
constexpr float kMinCutoffHz = 40.0f, kMaxCutoffHz = 250.0f;
constexpr float kMinMonoHz = 40.0f, kMaxMonoHz = 250.0f;
constexpr float kMinSubsonicHz = 10.0f, kMaxSubsonicHz = 40.0f;

// Recursive states below this are flushed (no subnormal crawl without FTZ).
constexpr float kStateFlush = 1.0e-20f;
constexpr float kEnvFlush = 1.0e-15f;

/** NaN -> fallback; everything else (including +-inf) clamps into range. */
float clampOr (float v, float lo, float hi, float fallback) noexcept
{
    return std::isnan (v) ? fallback : std::clamp (v, lo, hi);
}

/** "0 = off, else lo..hi": values <= 0 switch the stage off. */
float offOrRange (float v, float lo, float hi, float fallback) noexcept
{
    if (std::isnan (v))
        return fallback;
    return v <= 0.0f ? 0.0f : std::clamp (v, lo, hi);
}

BassEngineParams sanitise (const BassEngineParams& in, const BassEngineParams& prev) noexcept
{
    BassEngineParams p = in;
    p.boostDb = clampOr (in.boostDb, 0.0f, kMaxBoostDb, prev.boostDb);
    p.boostFrequency = clampOr (in.boostFrequency, kMinBoostHz, kMaxBoostHz, prev.boostFrequency);
    p.protectThresholdDb = clampOr (in.protectThresholdDb, kMinProtectDb, kMaxProtectDb, prev.protectThresholdDb);
    p.harmonicsAmount = clampOr (in.harmonicsAmount, 0.0f, 1.0f, prev.harmonicsAmount);
    p.harmonicsCutoff = clampOr (in.harmonicsCutoff, kMinCutoffHz, kMaxCutoffHz, prev.harmonicsCutoff);
    p.harmonicsCharacter = clampOr (in.harmonicsCharacter, 0.0f, 1.0f, prev.harmonicsCharacter);
    p.tighten = clampOr (in.tighten, 0.0f, 1.0f, prev.tighten);
    p.monoBelowHz = offOrRange (in.monoBelowHz, kMinMonoHz, kMaxMonoHz, prev.monoBelowHz);
    p.subsonicHz = offOrRange (in.subsonicHz, kMinSubsonicHz, kMaxSubsonicHz, prev.subsonicHz);
    p.subsonicOrder = in.subsonicOrder <= 2 ? 2 : 4;
    p.punch = clampOr (in.punch, 0.0f, 1.0f, prev.punch);
    return p;
}

/** Withdrawal (dB) for a predicted excess over the cap: 6 dB soft knee
    centred on the threshold, slope 1 above it (C1-continuous). */
float softKnee (float excessDb) noexcept
{
    constexpr float halfKnee = 0.5f * kProtectionKneeDb;
    if (excessDb <= -halfKnee)
        return 0.0f;
    if (excessDb >= halfKnee)
        return excessDb;
    const float t = excessDb + halfKnee;
    return t * t / (2.0f * kProtectionKneeDb);
}

/** Protection detector LP: ~150 Hz as specified, but never below 1.5 x the
    shelf frequency. The prediction assumes the full boost on what the
    detector sees; with a shelf at 150 - 200 Hz a fixed 150 Hz LP reads
    content around the shelf corner (which still gets about half the boost)
    several dB low, and the output overshot the cap by up to 2.6 dB. At
    1.5 x the corner the prediction stays conservative (<= 0.2 dB over the cap
    for any tone, any setting). For shelves at or below 100 Hz nothing changes. */
SvfCoeffs designDetector (double boostHz, double sampleRate) noexcept
{
    return SvfCoeffs::make (FilterType::LowPass, std::max (kDetectorHz, kDetectorTrackRatio * boostHz), kButterworthQ2, 0.0, sampleRate);
}

void designHighPass4 (std::array<SvfCoeffs, 2>& c, double hz, double sampleRate) noexcept
{
    c[0] = SvfCoeffs::make (FilterType::HighPass, hz, butterworthQ (2, 0), 0.0, sampleRate);
    c[1] = SvfCoeffs::make (FilterType::HighPass, hz, butterworthQ (2, 1), 0.0, sampleRate);
}

/** One Butterworth LP design; lr4Split() derives both LR4 bands from it. */
SvfCoeffs designLr4 (double hz, double sampleRate) noexcept
{
    return SvfCoeffs::make (FilterType::LowPass, hz, kButterworthQ2, 0.0, sampleRate);
}

/** Same topology as LinkwitzRiley4::processSample (LP4 + HP4 = 2nd-order
    all-pass), with the state owned here so it can be flushed. */
inline void lr4Split (const SvfCoeffs& c, std::array<SvfState, 3>& s, float x, float& low, float& high) noexcept
{
    const float k = static_cast<float> (c.k);
    float v1, v2;
    svfTickRaw (c, s[0], x, v1, v2);
    const float lp1 = v2;
    const float hp1 = x - k * v1 - v2;
    svfTickRaw (c, s[1], lp1, v1, v2);
    low = v2;
    svfTickRaw (c, s[2], hp1, v1, v2);
    high = hp1 - k * v1 - v2;
}

/** An LP / HP design with its prewarped frequency g replaced (k, m unchanged). */
inline SvfCoeffs withG (SvfCoeffs c, float g) noexcept
{
    const float k = static_cast<float> (c.k);
    c.a1 = 1.0f / (1.0f + g * (g + k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    return c;
}

inline float blendTo (float dry, float wet, float amount) noexcept
{
    return amount == 1.0f ? wet : dry + amount * (wet - dry);
}

/** Flushes near-zero SVF states; returns their sum for a cheap finiteness check. */
float flushTiny (SvfState& s) noexcept
{
    if (std::abs (s.ic1) < kStateFlush)
        s.ic1 = 0.0f;
    if (std::abs (s.ic2) < kStateFlush)
        s.ic2 = 0.0f;
    return s.ic1 + s.ic2;
}
} // namespace

//==============================================================================
void BassEngine::BandProtector::reset (float envLevel) noexcept
{
    hold.reset();
    env.reset (envLevel);
    held = 0.0f;
    valleyDb = gainToDb (envLevel);
    holdTicks = countdown = spacingLast = spacingPrev = 0;
    sinceOnset = maxTicks + 1; // no onset yet
    armed = true;
}

float BassEngine::BandProtector::update (float levelDb, float raw) noexcept
{
    // Onsets on the band's level: a rise of 3 dB over its recent valley (it
    // follows the level down at once and up with a 300 ms time constant,
    // never more than 30 dB below it), re-armed once the level is back
    // within 1.5 dB of it, so every hit of a repeated pattern counts however
    // much it happens to withdraw, also over a loud sustained line. Their spacing (the larger of the last
    // two, + 1/8) is how long a withdrawal is held: kicks 500 ms apart keep
    // one steady gain between them. An isolated onset (none in the last
    // 1.2 s) holds nothing.
    sinceOnset = std::min (sinceOnset + 1, maxTicks + 1);
    valleyDb = std::min (levelDb, std::max (levelDb - 30.0f, levelDb + valleyCoeff * (valleyDb - levelDb)));
    if (armed && levelDb > valleyDb + 3.0f)
    {
        armed = false;
        const int spacing = sinceOnset <= maxTicks ? sinceOnset : 0;
        spacingPrev = spacing > 0 ? spacingLast : 0;
        spacingLast = spacing;
        const int recent = std::max (spacingLast, spacingPrev);
        holdTicks = std::min (maxTicks, recent + recent / 8);
        countdown = holdTicks;
        sinceOnset = 0;
    }
    else if (! armed && levelDb < valleyDb + 1.5f)
    {
        armed = true;
    }

    if (raw >= held)
    {
        held = raw;
        countdown = std::max (countdown, holdTicks);
    }
    else if (countdown > 0)
    {
        --countdown;
    }
    else
    {
        held = holdTicks == 0 ? raw : raw + releaseCoeff * (held - raw);
    }
    return held;
}

//==============================================================================
void BassEngine::prepare (const ProcessSpec& newSpec)
{
    spec = newSpec;
    spec.numChannels = std::clamp (spec.numChannels, 1, kMaxChannels);
    if (! (spec.sampleRate > 0.0))
        spec.sampleRate = 48000.0;
    controlRate = spec.sampleRate / static_cast<double> (kControlInterval);
    const double sr = spec.sampleRate;

    detectorHold.prepare (sr, kHoldMs);
    detectorEnv.prepare (sr, kDetectorAttackMs, kDetectorReleaseMs);
    subProtect.hold.prepare (sr, kSubHoldMs);
    punchProtect.hold.prepare (sr, kPunchHoldMs);
    for (auto* band : { &subProtect, &punchProtect })
    {
        band->env.prepare (sr, kDetectorAttackMs, kDetectorReleaseMs);
        band->maxTicks = std::max (1, static_cast<int> (kProgramMaxSpacingMs * 0.001 * controlRate));
        band->releaseCoeff = static_cast<float> (std::exp (-1.0 / (kProgramReleaseMs * 0.001 * controlRate)));
        band->valleyCoeff = static_cast<float> (std::exp (-1.0 / (kValleyRiseMs * 0.001 * controlRate)));
    }
    splitXo = designLr4 (kSubDetectorHz, sr);
    // Look-ahead: the split detectors' attack fits inside it, so a
    // withdrawal is nearly complete when the onset reaches the shelf.
    lookahead = static_cast<int> (std::lround (lookaheadMs * 0.001 * sr));
    lookaheadBuf.assign (static_cast<size_t> (lookahead * spec.numChannels), 0.0f);
    if (lookahead > 0)
        for (auto* band : { &subProtect, &punchProtect })
            band->env.prepare (sr, kLookaheadAttackMs, kDetectorReleaseMs);
    orderBlend.reset (sr, kBlendMs, params.subsonicOrder == 2 ? 1.0f : 0.0f);

    harmPreHp = SvfCoeffs::make (FilterType::HighPass, kHarmonicsLowHz, kButterworthQ2, 0.0, sr);
    harmHold.prepare (sr, kHoldMs);
    harmEnv.prepare (sr, kHarmEnvAttackMs, kHarmEnvReleaseMs);
    harmonicsMix.reset (sr, kParamSmoothMs, 0.0f);

    tightShaper.prepare (sr);
    tightShaper.setSustainGatedByAttack (true); // never on the onset (docs/11 E04 step 2)
    distortionWindow.prepare (sr);

    punchBand.prepare (sr, kImpactLowHz, kImpactHighHz);
    punchDetector.setTiming (TransientShaper::Timing::lowBand());
    punchDetector.prepare (sr);
    punchLevel.prepare (sr, kImpactLevelHoldMs);
    punchBell = SvfCoeffs::make (FilterType::BandPass, kImpactBellHz, kImpactBellQ, 0.0, sr);
    punchHoldSamples = std::max (1, msToSamples (kImpactHoldMs, sr));
    punchReleaseCoeff = onePoleCoeff (kImpactReleaseMs, sr);
    punchRiseCoeff = onePoleCoeff (kImpactRiseMs, sr);
    punchFallCoeff = onePoleCoeff (kImpactFallMs, sr);

    auto initStage = [sr] (ParkedStage& stage, float parkHz)
    {
        stage.parkHz = parkHz;
        stage.logPark = std::log (parkHz);
        stage.blend.reset (sr, kBlendMs, 0.0f);
    };
    initStage (subsonic, kSubsonicParkHz);
    initStage (mono, kSplitParkHz);
    initStage (replace, kSplitParkHz);
    initStage (tight, kSplitParkHz);

    updateTargets();
    reset();
}

void BassEngine::reset() noexcept FLUB_NONBLOCKING
{
    controlCountdown = kControlInterval;
    const double sr = spec.sampleRate;

    // After a reset there is no previous output to click against: every
    // parameter starts at its target and every stage fully engaged.
    boostSmoothed.reset (controlRate, kParamSmoothMs, params.boostDb);
    logBoostHz.reset (controlRate, kFreqGlideMs, std::log (params.boostFrequency));
    thresholdSmoothed.reset (controlRate, kParamSmoothMs, params.protectThresholdDb);
    shelfGainSmoothed.reset (controlRate, kShelfGainSmoothMs, params.boostDb);
    withdrawSmoothed.reset (controlRate, kShelfGainSmoothMs, 0.0f);
    characterSmoothed.reset (controlRate, kParamSmoothMs, params.harmonicsCharacter);
    logCutoff.reset (controlRate, kFreqGlideMs, std::log (params.harmonicsCutoff));
    boostHz = params.boostFrequency;
    cutoffHz = params.harmonicsCutoff;

    harmonicsMix.setImmediate (params.harmonicsAmount * kHarmonicsMaxGain);
    harmonicsActive = params.harmonicsAmount > 0.0f || params.punch > 0.0f;
    punchAmount.reset (sr, kParamSmoothMs, params.punch);
    punchActive = params.punch > 0.0f;
    updateHarmonicWeights();
    updateHarmonicFilters();
    cutoffGlide.active = upperGlide.active = false;

    auto resetStage = [this] (ParkedStage& stage)
    {
        stage.active = stage.wanted;
        stage.hz = stage.wanted ? stage.targetHz : stage.parkHz;
        stage.logHz.reset (controlRate, kFreqGlideMs, stage.wanted ? stage.logTarget : stage.logPark);
        stage.blend.setImmediate (stage.wanted ? 1.0f : 0.0f);
        stage.glide.active = false;
    };
    resetStage (subsonic);
    resetStage (mono);
    resetStage (replace);
    resetStage (tight);
    designHighPass4 (subsonicHp, subsonic.hz, sr);
    subsonicHp2 = SvfCoeffs::make (FilterType::HighPass, subsonic.hz, kButterworthQ2, 0.0, sr);
    monoXo = designLr4 (mono.hz, sr);
    designHighPass4 (replaceHp, replace.hz, sr);
    tightXo = designLr4 (tight.hz, sr);

    detectorLp = designDetector (boostHz, sr);
    shelfGainDb = params.boostDb;
    shelf.setImmediate (SvfCoeffs::make (FilterType::LowShelf, boostHz, kShelfQ, shelfGainDb, sr));
    shelfActive = ! shelf.isIdentity();
    orderBlend.setImmediate (params.subsonicOrder == 2 ? 1.0f : 0.0f);
    splitRunning = params.splitProtection;
    updateSplitDesigns();
    bellCutSmoothed.reset (controlRate, kShelfGainSmoothMs, 0.0f);
    bellCutDb = 0.0f;
    bell.setImmediate (SvfCoeffs::make (FilterType::Bell, bellHz, kBellQ, 0.0, sr));
    bellActive = false;

    clearAllStates();
    protectionDb.store (0.0f, std::memory_order_relaxed);
    distortionWindow.reset();
    distortionDb.store (kMinusInfDb, std::memory_order_relaxed);
}

//==============================================================================
void BassEngine::setParams (const BassEngineParams& newParams) noexcept FLUB_NONBLOCKING
{
    const BassEngineParams p = sanitise (newParams, params);
    if (p == params)
        return; // the chain pushes every block; unchanged values cost nothing
    params = p;
    updateTargets();
}

void BassEngine::updateTargets() noexcept
{
    const double sr = spec.sampleRate;

    boostSmoothed.setTarget (params.boostDb);
    logBoostHz.setTarget (std::log (params.boostFrequency));
    thresholdSmoothed.setTarget (params.protectThresholdDb);
    characterSmoothed.setTarget (params.harmonicsCharacter);
    logCutoff.setTarget (std::log (params.harmonicsCutoff));

    harmonicsMix.setTarget (params.harmonicsAmount * kHarmonicsMaxGain);
    if ((params.harmonicsAmount > 0.0f || params.punch > 0.0f) && ! harmonicsActive)
    {
        // Idle harmonics path: start from clean state, the mix ramps from 0
        // (Impact's punch bursts it from 0).
        clearHarmonics();
        harmonicsActive = true;
    }

    // Impact's punch (docs/11 E20): an idle stage starts from clean state,
    // its amount ramping from 0, and keys nothing until it has warmed up.
    punchAmount.setTarget (params.punch);
    if (params.punch > 0.0f && ! punchActive)
    {
        punchActive = true;
        punchAmount.setImmediate (0.0f);
        punchAmount.setTarget (params.punch);
        startPunch();
    }

    tightShaper.setSustainDb (kTightenMaxDb * params.tighten);

    if (setStage (subsonic, params.subsonicHz > 0.0f, params.subsonicHz))
    {
        subsonicState.fill ({});
        subsonic2State.fill ({});
        designHighPass4 (subsonicHp, subsonic.hz, sr);
        subsonicHp2 = SvfCoeffs::make (FilterType::HighPass, subsonic.hz, kButterworthQ2, 0.0, sr);
    }
    orderBlend.setTarget (params.subsonicOrder == 2 ? 1.0f : 0.0f);

    if (params.splitProtection && ! splitRunning)
    {
        // Start the split detectors from the classic one's level: at worst
        // they withdraw a little more for a moment, never less.
        splitRunning = true;
        splitState.fill ({});
        lookaheadLpState.fill ({});
        subProtect.reset (detectorEnv.get());
        punchProtect.reset (detectorEnv.get());
    }
    if (setStage (mono, params.monoBelowHz > 0.0f && spec.numChannels == 2, params.monoBelowHz))
    {
        monoState.fill ({});
        monoXo = designLr4 (mono.hz, sr);
    }
    if (setStage (replace, params.replaceFundamental, params.harmonicsCutoff))
    {
        replaceState.fill ({});
        designHighPass4 (replaceHp, replace.hz, sr);
    }
    if (setStage (tight, params.tighten > 0.0f, kTightenHz))
    {
        tightState.fill ({});
        tightLp.fill (0.0f);
        tightXo = designLr4 (tight.hz, sr);
        tightShaper.reset();
    }
}

void BassEngine::updateSplitDesigns() noexcept
{
    // The bell sits a little below the geometric centre of the punch band
    // (60 Hz to the detector LP); what it may take back is the shelf's own
    // boost there, bellShelfRatio dB per dB of boost at the current corner.
    const double sr = spec.sampleRate;
    const double detectorHz = std::max (kDetectorHz, kDetectorTrackRatio * boostHz);
    bellHz = kBellCentre * std::sqrt (kSplitHz * detectorHz);
    constexpr double refDb = 6.0;
    bellShelfRatio = static_cast<float> (SvfCoeffs::make (FilterType::LowShelf, boostHz, kShelfQ, refDb, sr).magnitudeDb (bellHz, sr) / refDb);
}

bool BassEngine::setStage (ParkedStage& stage, bool wanted, float hz) noexcept
{
    stage.wanted = wanted;
    if (wanted)
    {
        stage.targetHz = hz;
        stage.logTarget = std::log (hz);
    }
    if (! wanted || stage.active)
        return false;

    // Engage parked: here the processed path differs from dry only in the
    // subsonic range, so the crossfade in cannot notch audible content.
    stage.active = true;
    stage.hz = stage.parkHz;
    stage.logHz.setImmediate (stage.logPark);
    stage.blend.setImmediate (0.0f);
    stage.blend.setTarget (1.0f);
    stage.glide.active = false;
    return true;
}

bool BassEngine::tickStage (ParkedStage& stage, bool effectSettled) noexcept
{
    stage.glide.active = false; // the caller starts a new glide if the corner moved
    if (! stage.active)
        return false;

    if (stage.wanted)
    {
        // Crossfade in completely at the park frequency before gliding up:
        // the crossfade then only ever mixes dry with a practically identical
        // processed signal.
        stage.blend.setTarget (1.0f);
        stage.logHz.setTarget (stage.blend.getCurrent() == 1.0f ? stage.logTarget : stage.logPark);
    }
    else
    {
        // Glide to the park frequency; crossfade out once there (and once the
        // stage's own effect, e.g. the tighten gain, has returned to unity).
        stage.logHz.setTarget (stage.logPark);
        if (effectSettled && std::abs (stage.logHz.getCurrent() - stage.logPark) < kParkLogTolerance)
            stage.blend.setTarget (0.0f);
        if (stage.blend.getCurrent() == 0.0f && ! stage.blend.isSmoothing())
        {
            stage.active = false;
            return false;
        }
    }

    if (! stage.logHz.isSmoothing())
        return false;
    stage.hz = std::exp (stage.logHz.next());
    return true;
}

void BassEngine::startPunch() noexcept
{
    punchBand.reset();
    punchDetector.reset();
    punchLevel.reset();
    punchBellState.fill ({});
    punchEnv = punchGainDb = 0.0f;
    punchHoldLeft = 0;
    punchWarm = std::max (1, msToSamples (kImpactWarmMs, spec.sampleRate));
}

float BassEngine::processPunch (std::array<float, kMaxChannels>& x, int numCh) noexcept
{
    float peak = 0.0f;
    for (int c = 0; c < numCh; ++c)
        peak = std::max (peak, std::abs (punchBand.processSample (c, x[static_cast<size_t> (c)])));
    const float onset = punchDetector.computeOnset (peak);
    const float heldDb = gainToDb (punchLevel.process (peak));

    // The burst envelope: an onset raises it at once, it holds, then releases.
    float o = std::clamp ((onset - kImpactOnsetFloor) / (1.0f - kImpactOnsetFloor), 0.0f, 1.0f);
    if (punchWarm > 0)
    {
        --punchWarm;
        o = 0.0f;
    }
    if (o > 0.0f && o >= punchEnv)
    {
        punchEnv = o;
        punchHoldLeft = punchHoldSamples;
    }
    else if (punchHoldLeft > 0)
    {
        --punchHoldLeft;
    }
    else
    {
        punchEnv *= punchReleaseCoeff;
        if (punchEnv < kImpactEnvFloor)
            punchEnv = 0.0f;
    }

    // The lift, reserved inside the band's headroom under the protection
    // threshold, then smoothed in dB (landing exactly on 0).
    const float amount = punchAmount.next();
    const float roomDb = std::max (0.0f, thresholdSmoothed.getCurrent() - heldDb);
    const float targetDb = std::min (kImpactMaxDb * amount * punchEnv, roomDb);
    punchGainDb = targetDb + (targetDb > punchGainDb ? punchRiseCoeff : punchFallCoeff) * (punchGainDb - targetDb);
    if (std::abs (punchGainDb - targetDb) < 1.0e-6f)
        punchGainDb = targetDb;

    const float g1 = punchGainDb == 0.0f ? 0.0f : dbToGain (punchGainDb) - 1.0f;
    for (int c = 0; c < numCh; ++c)
    {
        const size_t ch = static_cast<size_t> (c);
        const float bp = svfTick (punchBell, punchBellState[ch], x[ch]);
        if (g1 != 0.0f)
            x[ch] += g1 * bp;
    }
    return kImpactHarmonicsMix * amount * punchEnv; // the harmonics generator's burst mix
}

void BassEngine::updateHarmonicFilters() noexcept
{
    const double sr = spec.sampleRate;
    const double fc = cutoffHz;
    const double oldCutG = harmPostHp.g, oldUpperG = harmPostLp[0].g;
    harmPreLp[0] = SvfCoeffs::make (FilterType::LowPass, fc, butterworthQ (2, 0), 0.0, sr);
    harmPreLp[1] = SvfCoeffs::make (FilterType::LowPass, fc, butterworthQ (2, 1), 0.0, sr);
    harmPostHp = SvfCoeffs::make (FilterType::HighPass, fc, kButterworthQ2, 0.0, sr);
    harmPostLp[0] = SvfCoeffs::make (FilterType::LowPass, fc * kHarmonicsUpperRatio, butterworthQ (2, 0), 0.0, sr);
    harmPostLp[1] = SvfCoeffs::make (FilterType::LowPass, fc * kHarmonicsUpperRatio, butterworthQ (2, 1), 0.0, sr);
    cutoffGlide.start (oldCutG, harmPostHp.g);
    upperGlide.start (oldUpperG, harmPostLp[0].g);
}

void BassEngine::updateHarmonicWeights() noexcept
{
    const float c = characterSmoothed.getCurrent();
    for (size_t j = 0; j < weights.size(); ++j)
        weights[j] = lerp (kEvenWeights[j], kOddWeights[j], c);
}

void BassEngine::clearHarmonics() noexcept
{
    harmState.fill ({});
    harmHold.reset();
    harmEnv.reset (0.0f);
}

void BassEngine::clearAllStates() noexcept
{
    subsonicState.fill ({});
    subsonic2State.fill ({});
    monoState.fill ({});
    shelfState.fill ({});
    detectorState.fill ({});
    splitState.fill ({});
    bellState.fill ({});
    lookaheadLpState.fill ({});
    std::fill (lookaheadBuf.begin(), lookaheadBuf.end(), 0.0f);
    lookaheadPos = 0;
    subProtect.reset (0.0f);
    punchProtect.reset (0.0f);
    replaceState.fill ({});
    startPunch();
    tightState.fill ({});
    tightLp.fill (0.0f);
    detectorHold.reset();
    detectorEnv.reset (0.0f);
    clearHarmonics();
    tightShaper.reset();
}

float BassEngine::flushStates() noexcept
{
    float sum = 0.0f;
    for (int c = 0; c < spec.numChannels; ++c)
    {
        const size_t ch = static_cast<size_t> (c);
        for (auto& s : subsonicState[ch])
            sum += flushTiny (s);
        sum += flushTiny (subsonic2State[ch]);
        for (auto& s : splitState[ch])
            sum += flushTiny (s);
        sum += flushTiny (bellState[ch]) + flushTiny (lookaheadLpState[ch]);
        for (auto& s : monoState[ch])
            sum += flushTiny (s);
        for (auto& s : replaceState[ch])
            sum += flushTiny (s);
        for (auto& s : tightState[ch])
            sum += flushTiny (s);
        if (std::abs (tightLp[ch]) < kStateFlush)
            tightLp[ch] = 0.0f;
        sum += tightLp[ch];
        sum += flushTiny (shelfState[ch]) + flushTiny (detectorState[ch]) + flushTiny (punchBellState[ch]);
    }
    for (auto& s : harmState)
        sum += flushTiny (s);
    sum += punchBand.flushStates (spec.numChannels, kStateFlush);
    return sum;
}

//==============================================================================
void BassEngine::controlTick() noexcept
{
    const double sr = spec.sampleRate;

    // ---- switchable filter stages -----------------------------------------
    // A moved corner is redesigned here and reached by a per-sample g glide
    // across the next control interval.
    if (tickStage (subsonic, true))
    {
        const double g0 = subsonicHp[0].g;
        designHighPass4 (subsonicHp, subsonic.hz, sr);
        subsonicHp2 = SvfCoeffs::make (FilterType::HighPass, subsonic.hz, kButterworthQ2, 0.0, sr);
        subsonic.glide.start (g0, subsonicHp[0].g);
    }
    if (tickStage (mono, true))
    {
        const double g0 = monoXo.g;
        monoXo = designLr4 (mono.hz, sr);
        mono.glide.start (g0, monoXo.g);
    }
    if (tickStage (replace, true))
    {
        const double g0 = replaceHp[0].g;
        designHighPass4 (replaceHp, replace.hz, sr);
        replace.glide.start (g0, replaceHp[0].g);
    }
    if (tickStage (tight, tightShaper.isSettled()))
    {
        const double g0 = tightXo.g;
        tightXo = designLr4 (tight.hz, sr);
        tight.glide.start (g0, tightXo.g);
    }

    // ---- adaptive low shelf with headroom protection -----------------------
    const float boost = boostSmoothed.next();
    const float threshold = thresholdSmoothed.next();
    const bool shelfMoved = logBoostHz.isSmoothing();
    if (shelfMoved)
    {
        boostHz = std::exp (logBoostHz.next());
        detectorLp = designDetector (boostHz, sr);
        updateSplitDesigns();
    }

    // Predicted LF peak after the boost vs the cap; the boost is withdrawn by
    // the excess (never beyond the whole boost: this stage never cuts). The
    // net shelf gain is what gets smoothed (5 ms), so a boost change and the
    // protection's reaction to it can never pull the gain in opposite
    // directions for a moment.
    const float levelDb = gainToDb (detectorEnv.get());
    float withdraw = std::clamp (softKnee (levelDb + boost - threshold), 0.0f, boost);
    float bellCut = 0.0f;
    if (splitRunning)
    {
        // Split-band protection (docs/11 E02 (a)): the sub band withdraws the
        // shelf; what the whole LF band (the classic prediction, 8 ms hold)
        // still exceeds after that is taken by the 60-150 Hz bell, at most
        // as deep as the shelf's boost at its centre. Each goes through its
        // program-dependent hold.
        const float subDb = gainToDb (subProtect.env.get()), punchDb = gainToDb (punchProtect.env.get());
        const float subWithdraw = subProtect.update (subDb, std::clamp (softKnee (subDb + boost - threshold), 0.0f, boost));
        const float fullWithdraw = std::clamp (softKnee (punchDb + boost - threshold), 0.0f, boost);
        bellCut = punchProtect.update (punchDb, std::max (0.0f, fullWithdraw - subWithdraw));
        if (params.splitProtection)
        {
            // The bell never takes more than the boost the shelf still
            // gives at its centre, so no frequency ends up below flat.
            withdraw = std::min (subWithdraw, boost);
            bellCut = std::min (bellCut, bellShelfRatio * (boost - withdraw));
        }
        else
        {
            bellCut = 0.0f; // switched off: back to the classic detector (gains glide)
            if (bellCutDb == 0.0f && ! bellCutSmoothed.isSmoothing())
                splitRunning = false;
        }
    }
    // (While the boost itself falls, the smoothed gain may trail it by a
    // fraction of a dB; clamping it to the boost would put a kink in it.)
    shelfGainSmoothed.setTarget (boost - withdraw);
    // With a look-ahead the split withdrawals are already ramped by their
    // detectors' 1 ms attack and lead the audio: they apply at once (the
    // shelf and bell still glide per sample across the control interval),
    // so an onset meets them in place; only releases keep the 5 ms
    // smoothing (40 Hz at -6 dBFS into +12 dB, cap 0 dBFS: onset peak
    // +4.0 -> -0.4 dBFS; smoothed like the releases, about +1.6).
    const bool leadWithdraw = lookahead > 0 && splitRunning && params.splitProtection;
    if (leadWithdraw && boost - withdraw < shelfGainSmoothed.getCurrent())
        shelfGainSmoothed.setImmediate (boost - withdraw);
    const float gainDb = std::max (0.0f, shelfGainSmoothed.next());

    // Telemetry: the withdrawal itself, smoothed like the gain. (boost - gainDb
    // would also report the 5 ms lag of the gain behind a boost change as
    // "protection": a 2+ dB flash on quiet material whenever boostDb moves.)
    withdrawSmoothed.setTarget (std::max (withdraw, bellCut));
    protectionDb.store (std::clamp (withdrawSmoothed.next(), 0.0f, boost), std::memory_order_relaxed);

    // The punch band's bell (a cut of bellCut dB, smoothed like the shelf gain).
    bellCutSmoothed.setTarget (bellCut);
    if (leadWithdraw && bellCut > bellCutSmoothed.getCurrent())
        bellCutSmoothed.setImmediate (bellCut);
    const float cut = std::max (0.0f, bellCutSmoothed.next());
    if (cut != bellCutDb || (shelfMoved && cut != 0.0f))
    {
        if (! bellActive)
        {
            bellState.fill ({});
            bellActive = true;
        }
        bellCutDb = cut;
        bell.glideTo (SvfCoeffs::make (FilterType::Bell, bellHz, kBellQ, -cut, sr));
    }
    else
    {
        bell.ramping = false;
        if (bell.isIdentity())
            bellActive = false;
    }

    if (gainDb != shelfGainDb || (shelfMoved && gainDb != 0.0f))
    {
        if (! shelfActive)
        {
            // Resume from identity with clean state; the glide starts at 0 dB.
            shelfState.fill ({});
            shelfActive = true;
        }
        shelfGainDb = gainDb;
        shelf.glideTo (SvfCoeffs::make (FilterType::LowShelf, boostHz, kShelfQ, gainDb, sr));
    }
    else
    {
        shelf.ramping = false;
        if (shelf.isIdentity())
            shelfActive = false; // exact 0 dB: skip it until the gain moves
    }

    // ---- harmonics ---------------------------------------------------------
    if (characterSmoothed.isSmoothing())
    {
        characterSmoothed.next();
        updateHarmonicWeights();
    }
    if (logCutoff.isSmoothing())
    {
        cutoffHz = std::exp (logCutoff.next());
        updateHarmonicFilters();
    }
    else
    {
        cutoffGlide.active = upperGlide.active = false;
    }
    // Impact's punch stops once its amount and its lift have landed on 0.
    if (punchActive && params.punch == 0.0f && ! punchAmount.isSmoothing() && punchAmount.getCurrent() == 0.0f && punchGainDb == 0.0f)
        punchActive = false;
    if (harmonicsActive && harmonicsMix.getTarget() == 0.0f && ! harmonicsMix.isSmoothing() && ! punchActive)
        harmonicsActive = false;

    // ---- state hygiene -----------------------------------------------------
    if (detectorEnv.get() < kEnvFlush)
        detectorEnv.reset (0.0f);
    for (auto* band : { &subProtect, &punchProtect })
        if (band->env.get() < kEnvFlush)
            band->env.reset (0.0f);
    if (harmEnv.get() < kEnvFlush)
        harmEnv.reset (0.0f);
    if (! std::isfinite (flushStates() + detectorEnv.get() + harmEnv.get() + subProtect.env.get() + punchProtect.env.get()))
        clearAllStates();
}

//==============================================================================
void BassEngine::process (const AudioBlock& block) noexcept FLUB_NONBLOCKING
{
    const int numSamples = block.numSamples;
    const int numCh = std::min ({ block.numChannels, spec.numChannels, kMaxChannels });
    if (numSamples <= 0 || numCh <= 0)
        return;

    for (int pos = 0; pos < numSamples;)
    {
        const int len = std::min (numSamples - pos, controlCountdown);
        processSegment (block, numCh, pos, len);
        pos += len;
        controlCountdown -= len;
        if (controlCountdown == 0)
        {
            controlCountdown = kControlInterval;
            controlTick();
        }
    }

    // Harmonics telemetry: a window without harmonics (all sums 0) reads -160 dB.
    if (float db = kMinusInfDb; distortionWindow.advance (numSamples, db))
        distortionDb.store (db, std::memory_order_relaxed);
}

void BassEngine::splitDetect (const std::array<float, kMaxChannels>& x, int numCh, float lfPeak) noexcept
{
    // Split-band detectors, linked like the classic one: sub = the LR4 low
    // band at 85 Hz (overlapping the punch band, so a tone near 60 Hz is not
    // under-read), punch = the classic detector's signal (lfPeak) with the
    // shorter hold.
    float subPeak = 0.0f;
    for (int c = 0; c < numCh; ++c)
    {
        const size_t ch = static_cast<size_t> (c);
        float lo = 0.0f, hi = 0.0f;
        lr4Split (splitXo, splitState[ch], x[ch], lo, hi);
        subPeak = std::max (subPeak, std::abs (lo));
    }
    subProtect.env.process (subProtect.hold.process (subPeak));
    punchProtect.env.process (punchProtect.hold.process (lfPeak));
}

void BassEngine::processSegment (const AudioBlock& block, int numCh, int pos, int len) noexcept
{
    const bool doMono = mono.active && numCh == 2;
    const int phase = kControlInterval - controlCountdown; // samples since the last tick
    const float invCh = 1.0f / static_cast<float> (numCh);
    const float w2 = weights[0], w3 = weights[1], w4 = weights[2], w5 = weights[3];
    constexpr float invInterval = 1.0f / static_cast<float> (kControlInterval);

    std::array<float, kMaxChannels> x {}, low {}, high {};
    std::array<ParallelDistortionSums, kMaxChannels> sums {}; // harmonics telemetry of this segment

    for (int i = 0; i < len; ++i)
    {
        const int n = pos + i;
        const float t = static_cast<float> (phase + i + 1) * invInterval; // glide position
        for (int c = 0; c < numCh; ++c)
            x[static_cast<size_t> (c)] = block.channel (c)[n];

        // 1. Subsonic high-pass -----------------------------------------------
        if (subsonic.active)
        {
            const float b = subsonic.blend.next();
            SvfCoeffs hp0 = subsonicHp[0], hp1 = subsonicHp[1];
            if (subsonic.glide.active)
            {
                const float g = subsonic.glide.at (t);
                hp0 = withG (hp0, g);
                hp1 = withG (hp1, g);
            }
            // 4th order, 2nd order, or a crossfade of the two (order change).
            const float order = orderBlend.next();
            SvfCoeffs hp2 = subsonicHp2;
            if (subsonic.glide.active)
                hp2 = withG (hp2, subsonic.glide.at (t));
            for (int c = 0; c < numCh; ++c)
            {
                const size_t ch = static_cast<size_t> (c);
                auto& s = subsonicState[ch];
                const float y4 = svfTick (hp1, s[1], svfTick (hp0, s[0], x[ch]));
                const float y2 = svfTick (hp2, subsonic2State[ch], x[ch]);
                x[ch] = blendTo (x[ch], order == 0.0f ? y4 : blendTo (y4, y2, order), b);
            }
        }
        else
        {
            orderBlend.next();
        }

        // 2. Mono bass: both channels get the mid of the low band -------------
        if (doMono)
        {
            const float b = mono.blend.next();
            const SvfCoeffs xo = mono.glide.active ? withG (monoXo, mono.glide.at (t)) : monoXo;
            lr4Split (xo, monoState[0], x[0], low[0], high[0]);
            lr4Split (xo, monoState[1], x[1], low[1], high[1]);
            const float midLow = 0.5f * (low[0] + low[1]);
            x[0] = blendTo (x[0], midLow + high[0], b);
            x[1] = blendTo (x[1], midLow + high[1], b);
        }

        // 3. Protection detector (pre-boost, linked) + low shelf ---------------
        if (lookahead > 0)
        {
            // Look-ahead: the split detectors read this sample, the rest of
            // the engine (the classic detector too, so its timing is
            // unchanged) the one `lookahead` samples earlier.
            if (splitRunning)
            {
                float punchPeak = 0.0f;
                for (int c = 0; c < numCh; ++c)
                {
                    const size_t ch = static_cast<size_t> (c);
                    punchPeak = std::max (punchPeak, std::abs (svfTick (detectorLp, lookaheadLpState[ch], x[ch])));
                }
                splitDetect (x, numCh, punchPeak);
            }
            for (int c = 0; c < numCh; ++c)
            {
                float& slot = lookaheadBuf[static_cast<size_t> (c * lookahead + lookaheadPos)];
                const float delayed = slot;
                slot = x[static_cast<size_t> (c)];
                x[static_cast<size_t> (c)] = delayed;
            }
            lookaheadPos = lookaheadPos + 1 == lookahead ? 0 : lookaheadPos + 1;
        }
        // 3a. Impact's punch (docs/11 E20): the LF burst, and the harmonics' burst mix.
        const float punchHarmonics = punchActive ? processPunch (x, numCh) : 0.0f;

        float lfPeak = 0.0f;
        for (int c = 0; c < numCh; ++c)
        {
            const size_t ch = static_cast<size_t> (c);
            lfPeak = std::max (lfPeak, std::abs (svfTick (detectorLp, detectorState[ch], x[ch])));
        }
        detectorEnv.process (detectorHold.process (lfPeak));
        if (splitRunning && lookahead == 0)
            splitDetect (x, numCh, lfPeak);

        if (shelfActive)
        {
            const SvfCoeffs sc = shelf.ramping ? shelf.at (t) : shelf.end;
            for (int c = 0; c < numCh; ++c)
            {
                const size_t ch = static_cast<size_t> (c);
                x[ch] = svfTick (sc, shelfState[ch], x[ch]);
            }
        }
        if (bellActive)
        {
            const SvfCoeffs bc = bell.ramping ? bell.at (t) : bell.end;
            for (int c = 0; c < numCh; ++c)
            {
                const size_t ch = static_cast<size_t> (c);
                x[ch] = svfTick (bc, bellState[ch], x[ch]);
            }
        }

        // 4. Psychoacoustic harmonics from the mid signal ------------------------
        float harmonics = 0.0f, linearBranch = 0.0f;
        if (harmonicsActive)
        {
            float mid = 0.0f;
            for (int c = 0; c < numCh; ++c)
                mid += x[static_cast<size_t> (c)];
            mid *= invCh;

            SvfCoeffs lp0 = harmPreLp[0], lp1 = harmPreLp[1], postHp = harmPostHp;
            SvfCoeffs postLp0 = harmPostLp[0], postLp1 = harmPostLp[1];
            if (cutoffGlide.active)
            {
                const float g = cutoffGlide.at (t);
                lp0 = withG (lp0, g);
                lp1 = withG (lp1, g);
                postHp = withG (postHp, g);
            }
            if (upperGlide.active)
            {
                const float g = upperGlide.at (t);
                postLp0 = withG (postLp0, g);
                postLp1 = withG (postLp1, g);
            }

            float b = svfTick (harmPreHp, harmState[0], mid);
            b = svfTick (lp0, harmState[1], b);
            b = svfTick (lp1, harmState[2], b);

            // Amplitude-normalised Chebyshev shaper: for a sinusoid the output
            // is exactly w2 cos 2t + w3 cos 3t + ... scaled by its amplitude.
            const float env = harmEnv.process (harmHold.process (std::abs (b)));
            const float xn = env > 0.0f ? std::clamp (b / env, -1.0f, 1.0f) : 0.0f;
            const float x2 = xn * xn;
            const float t2 = 2.0f * x2 - 1.0f;
            const float t3 = xn * (4.0f * x2 - 3.0f);
            const float t4 = 8.0f * x2 * (x2 - 1.0f) + 1.0f;
            const float t5 = xn * (x2 * (16.0f * x2 - 20.0f) + 5.0f);
            float y = (w2 * t2 + w3 * t3 + w4 * t4 + w5 * t5) * env;

            // Band-pass [cutoff (12 dB/oct), 6 cutoff (24 dB/oct)]: the steep
            // upper slope also removes the splatter of the envelope's attacks.
            y = svfTick (postHp, harmState[3], y);
            y = svfTick (postLp0, harmState[4], y);
            y = svfTick (postLp1, harmState[5], y);

            // Telemetry reference: the shaper's input through the same band
            // pass. Whatever part of the harmonics is proportional to b (odd
            // terms below full scale) is linear filtering, not generated.
            float lin = svfTick (postHp, harmState[6], b);
            lin = svfTick (postLp0, harmState[7], lin);
            lin = svfTick (postLp1, harmState[8], lin);
            const float mix = harmonicsMix.next() + punchHarmonics;
            harmonics = y * mix;
            linearBranch = lin * mix;
        }

        if (replace.active)
        {
            const float b = replace.blend.next();
            SvfCoeffs hp0 = replaceHp[0], hp1 = replaceHp[1];
            if (replace.glide.active)
            {
                const float g = replace.glide.at (t);
                hp0 = withG (hp0, g);
                hp1 = withG (hp1, g);
            }
            for (int c = 0; c < numCh; ++c)
            {
                const size_t ch = static_cast<size_t> (c);
                auto& s = replaceState[ch];
                const float y = svfTick (hp1, s[1], svfTick (hp0, s[0], x[ch]));
                x[ch] = blendTo (x[ch], y, b);
            }
        }

        if (harmonicsActive)
            for (int c = 0; c < numCh; ++c)
            {
                // Telemetry: the linear references here against what the shaper adds.
                const size_t ch = static_cast<size_t> (c);
                sums[ch].add (x[ch], linearBranch, harmonics);
                x[ch] += harmonics;
            }

        // 5. Tighten: shorter low-band decay --------------------------------
        if (tight.active)
        {
            const float b = tight.blend.next();
            const SvfCoeffs xo = tight.glide.active ? withG (tightXo, tight.glide.at (t)) : tightXo;
            float lowPeak = 0.0f;
            for (int c = 0; c < numCh; ++c)
            {
                const size_t ch = static_cast<size_t> (c);
                lr4Split (xo, tightState[ch], x[ch], low[ch], high[ch]);
                lowPeak = std::max (lowPeak, std::abs (low[ch]));
            }
            const float g = tightShaper.computeGain (lowPeak);
            // Applied as x + (g - 1) LP1 (x): exactly x at unity gain, so the
            // split's all-pass never reaches the output (see the header).
            const float lpG = static_cast<float> (xo.g / (1.0 + xo.g));
            for (int c = 0; c < numCh; ++c)
            {
                const size_t ch = static_cast<size_t> (c);
                float& z = tightLp[ch];
                const float v = (x[ch] - z) * lpG;
                const float lp = v + z;
                z = lp + v;
                x[ch] += b * (g - 1.0f) * lp;
            }
        }

        for (int c = 0; c < numCh; ++c)
            block.channel (c)[n] = x[static_cast<size_t> (c)];
    }

    if (harmonicsActive)
        for (int c = 0; c < numCh; ++c)
            if (const auto& s = sums[static_cast<size_t> (c)]; s.isFinite())
                distortionWindow.channel (c).merge (s);
}
} // namespace flub
