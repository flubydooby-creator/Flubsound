// Flubsound Pro - level-independent transient shaper.
//
// Per sample (detector input d = max_c |x_c|, shared by all channels):
//
//   h   = peak hold of d over ~25..33 ms (see PeakHold in the header)
//   e   = h + 1e-5                      (-100 dBFS floor, see kDetectorFloor)
//   attack pair  : A_fast (0.5 ms att), A_slow (20 ms att), both 60 ms release
//   sustain pair : S_slow (400 ms rel), S_fast (40 ms rel), both 1 ms attack
//   wA  = clamp (20 log10 (A_fast / A_slow) / 6 dB, 0, 1)
//   wS  = clamp (20 log10 (S_slow / S_fast) / 6 dB, 0, 1)
//   gdB = attackDb' * wA + sustainDb' * wS   (' = 20 ms parameter smoothing)
//   gain = 10^(smooth_1ms (gdB) / 20)
//
// Sustain gate (setSustainGatedByAttack, BassEngine's Tighten; docs/11 E04
// step 2): wS is multiplied by (1 - clamp (20 log10 (e / A_slow) / 6 dB, 0,
// 1)), so a negative sustain never lands on an onset. Without it, the
// sustain pair still reads the previous note's decay (S_slow high, S_fast
// near the floor) through a new note's first 2-3 ms, i.e. the whole cut
// sits on the first half-cycle of a kick. The held level e against A_slow
// (still at the previous note's release) opens the gate on the onset's
// first samples - A_fast's 0.5 ms would let the first millisecond through
// cut - and closes it only once A_slow has caught up with the held peak,
// 30-50 ms later, when the decay the sustain pair acts on begins.
//
// Only ratios of envelopes enter the gain, so the shaping follows the shape
// of the envelope, not its level. The peak hold in front of the followers is
// what keeps steady low notes clean: without it the 0.5 ms / 1 ms attack
// followers re-acquire every waveform peak and the pairs disagree by a few
// dB at twice the note frequency, i.e. +-12 dB settings would amplitude-
// modulate a sustained bass note. With the hold a steady tone reads as a
// constant, both envelopes of a pair converge and the indicators are 0.
//
// Neutral (attackDb = sustainDb = 0 and all smoothing landed): the four
// envelopes still run (so re-enabling starts from a consistent state), the
// log / exp work is skipped and computeGain() returns exactly 1.0f, so
// process() leaves the audio bit-exact.
//
// CPU per sample: one max per channel, 4 one-pole followers, at most two
// logs (only while an indicator is inside its 0..6 dB range) and one exp.
#include "flub/dsp/TransientShaper.h"

#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>

namespace flub
{
namespace
{
constexpr float kFastAttackMs = 0.5f;
constexpr float kSlowAttackMs = 20.0f;
constexpr float kAttackPairReleaseMs = 60.0f;
constexpr float kSustainPairAttackMs = 1.0f;
constexpr float kSlowReleaseMs = 400.0f;
constexpr float kFastReleaseMs = 40.0f;

constexpr float kGainSmoothMs = 1.0f;
// Gated sustain: a cut the onset gate lifts returns this fast, so a kick
// arriving while the previous decay's cut is still in place keeps its first
// milliseconds (Tighten 0.5 on kicks, first 10 ms: -0.62 dB with the 1 ms
// smoothing, -0.31 dB with this; 0.2 ms reached -0.13 dB but its sidebands
// broke the -80 dBFS click bound of tests/test_bass_engine.cpp's feature
// toggling). Tighten applies the gain to its low band only.
constexpr float kGatedReturnMs = 0.5f;
constexpr float kParamSmoothMs = 20.0f;
constexpr float kMaxDb = 12.0f;

// The hold must span half a period of the lowest note that should read as
// "steady": 25 ms = 20 Hz (rectified period).
constexpr double kHoldMs = 25.0;

// Added to the detector input (-100 dBFS). Adding a constant to a branching
// one-pole follower adds the same constant to its output, so this equals a
// floor on every envelope: they stay normal floats in silence (no subnormal
// crawl), both envelopes of a pair converge on the floor so fading hiss is
// not read as a decay, and the ratios below never divide by zero.
constexpr float kDetectorFloor = 1.0e-5f;
constexpr float kMaxDetector = 1.0e6f;

// Indicators saturate at 6 dB: ratio 10^(6/20).
constexpr float kIndicatorRangeDb = 6.0f;
constexpr float kFullScaleRatio = 1.99526231f;
constexpr float kLnRatioToWeight = 20.0f / (2.30258509f * kIndicatorRangeDb);
constexpr float kDbToLn = 0.115129255f; // ln(10) / 20

/** clamp (20 log10 (ratio) / 6 dB, 0, 1); the log is only evaluated inside the ramp. */
float indicatorWeight (float ratio) noexcept
{
    if (ratio <= 1.0f)
        return 0.0f;
    if (ratio >= kFullScaleRatio)
        return 1.0f;
    return std::log (ratio) * kLnRatioToWeight;
}
} // namespace

void TransientShaper::prepare (double sampleRate) noexcept
{
    sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    hold.prepare (sr, kHoldMs);
    attackFast.prepare (sr, kFastAttackMs, kAttackPairReleaseMs);
    attackSlow.prepare (sr, kSlowAttackMs, kAttackPairReleaseMs);
    sustainSlow.prepare (sr, kSustainPairAttackMs, kSlowReleaseMs);
    sustainFast.prepare (sr, kSustainPairAttackMs, kFastReleaseMs);
    gainCoeff = onePoleCoeff (kGainSmoothMs, sr);
    gatedReturnCoeff = onePoleCoeff (kGatedReturnMs, sr);
    reset();
}

void TransientShaper::reset() noexcept FLUB_NONBLOCKING
{
    hold.reset();
    attackFast.reset (kDetectorFloor);
    attackSlow.reset (kDetectorFloor);
    sustainSlow.reset (kDetectorFloor);
    sustainFast.reset (kDetectorFloor);
    attackAmount.reset (sr, kParamSmoothMs, attackDb);
    sustainAmount.reset (sr, kParamSmoothMs, sustainDb);
    gainDbState = 0.0f;
}

void TransientShaper::setAttackDb (float db) noexcept FLUB_NONBLOCKING
{
    if (std::isnan (db))
        return; // keep the last valid setting
    attackDb = std::clamp (db, -kMaxDb, kMaxDb);
    attackAmount.setTarget (attackDb);
}

void TransientShaper::setSustainDb (float db) noexcept FLUB_NONBLOCKING
{
    if (std::isnan (db))
        return;
    sustainDb = std::clamp (db, -kMaxDb, kMaxDb);
    sustainAmount.setTarget (sustainDb);
}

float TransientShaper::computeGain (float linkedAbs) noexcept
{
    // NaN reads as silence, +inf / huge values saturate: the envelopes can
    // never be poisoned by a bad input sample.
    float x = 0.0f;
    if (linkedAbs > 0.0f)
        x = std::min (linkedAbs, kMaxDetector);

    const float d = hold.process (x) + kDetectorFloor;
    const float aFast = attackFast.process (d);
    const float aSlow = attackSlow.process (d);
    const float sSlow = sustainSlow.process (d);
    const float sFast = sustainFast.process (d);

    const float atk = attackAmount.next();
    const float sus = sustainAmount.next();

    float targetDb = 0.0f;
    if (atk != 0.0f)
        targetDb += atk * indicatorWeight (aFast / aSlow);
    if (sus != 0.0f)
        targetDb += sus * indicatorWeight (sSlow / sFast) * (sustainGated ? 1.0f - indicatorWeight (d / aSlow) : 1.0f);

    // ~1 ms smoothing of the gain in dB (symmetric, so the envelope shape is
    // not skewed), landing exactly on the target so neutral returns 1.0f.
    // (a gated sustain cut returns faster, see kGatedReturnMs).
    const float coeff = sustainGated && targetDb > gainDbState ? gatedReturnCoeff : gainCoeff;
    gainDbState = targetDb + coeff * (gainDbState - targetDb);
    if (std::abs (gainDbState - targetDb) < 1.0e-6f)
        gainDbState = targetDb;

    return gainDbState == 0.0f ? 1.0f : std::exp (gainDbState * kDbToLn);
}

void TransientShaper::process (const AudioBlock& block) noexcept
{
    const int numCh = std::min (block.numChannels, kMaxChannels);
    if (numCh <= 0)
        return;

    for (int i = 0; i < block.numSamples; ++i)
    {
        float linked = 0.0f;
        for (int c = 0; c < numCh; ++c)
            linked = std::max (linked, std::abs (block.channel (c)[i]));

        const float g = computeGain (linked);
        if (g != 1.0f)
            for (int c = 0; c < numCh; ++c)
                block.channel (c)[i] *= g;
    }
}
} // namespace flub
