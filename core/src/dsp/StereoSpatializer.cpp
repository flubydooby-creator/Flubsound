// Flubsound Pro - mono-compatible stereo widener / spatializer.
//
// Per sample (stereo blocks only):
//
//   M  = (L + R) / 2,   S = (L - R) / 2
//   S1 = min (w, 1) * (S + guard_w * (HS_w (S) - S))             width
//        HS_w: 2nd-order high shelf (Cytomic SVF, Q 1/sqrt 2) at the low cut,
//        0 dB below it, 20 log10 (max (w, 1)) dB above, half of that at the cut
//        guard_w: width polarity guard (below), 1 while S stays below M
//   S2 = Bell (S1), 3 kHz, Q 0.5, +3 dB * focus                  positional focus
//        (0 dB at output rates <= 32 kHz: Bluetooth hands-free / speech links)
//   S3 = S2 + 0.5 * space * AP (z^-P Dip (HP_300 (M)))           space
//   S4 = S3 - 0.6 * crossfeed * LP1_700 (S3)                     Mono-safe crossfeed
//   L3 = M + S4,  R3 = M - S4        (L and R are left untouched when S4 == S)
//   L' = L3 - (1 - n0) LP_fc (L3) + g z^-D LP_fc (R3)            Bs2b / Meier crossfeed
//   R' = R3 - (1 - n0) LP_fc (R3) + g z^-D LP_fc (L3)
//
// Mono guarantee: L' + R' = 2M = L + R for every setting of width, focus,
// space and the Mono-safe crossfeed, up to float rounding (a few ulp),
// because nothing there touches M. The Bs2b / Meier crossfeed is the
// exception: it is a real L / R crossfeed with an interaural delay, and
// L' + R' = (N + X) (L + R) with N, X the near- and far-ear filters.
//
// Width. The contract's S' = S_low * min (w, 1) + S_high * w is realised with
// a complementary split (S_low + S_high = S exactly): for w <= 1 the width is
// the plain gain w * S, for w > 1 it is S + (w - 1) * S_high, which is a
// minimum-phase high shelf. Its magnitude transition matches an in-phase LR4
// band sum (w = 2: +0.4 dB at half the cut, +3.0 dB at the cut, +5.6 dB at
// twice the cut; LR4: +0.5 / +3.5 / +5.8 dB), but its phase never strays more
// than 28 degrees from M. A literal LR4 pair sums to a 2nd-order all-pass
// that reaches -180 degrees at the crossover: on S alone that would invert the
// side signal against the untouched M around the low cut (a left-panned
// 180 Hz source would image right) and width 1 could not be transparent.
// The shelf is re-derived per sample only while width or low cut glide
// (two square roots and one division), so a glide has no control-rate steps.
// At w = 1 its mix coefficients are exactly (1, 0, 0): bit-exact identity.
//
// Width polarity guard (docs/11 E12 Phase A). Raising S against an untouched
// M writes M - S' < 0 into the quieter ear once S' > M: for a hard-panned
// source (S = M) any widening puts an anti-phase copy in the silent ear (at
// w = 2 at -6 dB re the source, ILD 9.5 dB instead of infinite). The side
// signal the shelf adds, d = HS_w (S) - S, is therefore applied in the share
//   guard_w = clamp ((e_M - e_S) / e_d, 0, 1)
// where e_M, e_S are peak envelopes (30 ms release) of M and S high-passed at
// the low cut (Q 1/sqrt 2) and e_d that of d (instant attack, 50 ms release,
// as the focus guard). A hard-panned source (e_M = e_S) is not widened and
// its far ear stays exactly silent; a partially panned one is widened until
// its far ear reaches silence (R = L / 2: 6 -> 14 dB ILD at w = 2); a pure
// side signal (no M) is not widened. The guard reads the band mix, not each
// source (docs/03 section 7.9). At w <= 1, d = 0 exactly and the guard has no
// effect.
//
// Positional focus: the Cytomic bell of Svf.h (k = 1 / (Q A), m1 = k (A^2 - 1))
// with fixed g; the gain glides in dB and the three a-coefficients are
// re-derived per sample only while it moves. Focus 0 gives m1 = 0 exactly.
// Polarity guard: raising S by G against an untouched M flips the far ear
// (M - G S) once G S > M. For a hard-panned source (S = M) any lift does,
// which would put an anti-phase copy in the silent ear (the ILD would fall
// from infinite to ~10 dB). Band-pass envelopes of M and S at the bell's own
// centre and Q (peak hold, 30 ms release) bound the lift to what keeps the
// quieter ear's polarity: the applied share of the bell's added signal is
//   guard = clamp ((e_M - e_S) / (e_S (G - 1)), 0, 1),  G = A^2 (3 kHz gain)
// (instant attack, 50 ms release). Partially panned sources (e_M > e_S) still
// gain ILD, a hard-panned one keeps it infinite, a pure side signal (no M)
// is not lifted, and M is still never touched (mono sum exact).
//
// Space: HP_300 (2nd-order Butterworth) of M, then a presence dip (bell,
// -7 dB at 2 kHz, Q 0.4: -5 dB at 1 and 4 kHz), delayed by P = 10 ms, then
// three nested Schroeder all-passes (g = 0.5): the 7.3 ms outer section
// carries the 4.7 ms section inside its delay path, which in turn carries
// the 3.1 ms one.
//   v = x + g w,  y = w - g v,  w = inner (v delayed)
// An all-pass has a flat magnitude, so the ambience has exactly the spectrum
// of Dip (HP (M)); it lives only in S, so it cancels in mono. For a centred
// source, L' / R' = (1 + A) / (1 - A) with A the ambience response: the ear
// levels differ wherever A is in phase with M. A Schroeder section passes -g
// of its input with no delay; without the pre-delay that tap would put
// -0.25 * space * HP (M) straight into S, i.e. pan the centre sideways
// (4.4 dB ILD at space = 1). Delayed, it becomes a lateral early reflection
// (a Lauridsen-type complementary comb) whose ripple averages out within a
// 1/3 octave once the band is wider than about 3.5 / P: with P = 10 ms the
// ILD of a centred impulse stays below 1 dB in every 1/3 octave from 100 Hz
// (5 ms: 1.9 dB at 500 Hz). On a steady tone the ripple does not average:
// its ILD is up to 20 log10 ((1 + |A|) / (1 - |A|)), 9.5 dB at |A| = 0.5,
// which the presence dip lowers to 3.9-4.9 dB over 1-4 kHz, where a voice
// carries its presence (docs/11 E12; bounding it further needs a lower |A|
// or a per-partial decorrelator, see docs/03 section 7.9).
//
// Mono-safe crossfeed: S4 = S3 - 0.6 c LP1 (S3), with a first-order TPT
// low-pass at 700 Hz. Subtracting it from S is a smooth low shelf on S only
// (c = 1: -7.9 dB at DC, -7.5 dB at 100 Hz, -2.4 dB at 700 Hz, -0.2 dB at
// 3 kHz), monotonic, no resonance, no delay. This was the only crossfeed
// before docs/11 E12: its interaural delay is only that of the low-pass
// (0.24 samples by the E12 audit, 1.4 samples by the white-noise
// cross-correlation of tests/test_spatializer.cpp).
//
// Bs2b / Meier crossfeed (docs/11 E12 Phase A). With r = c * 10^(-feed / 20)
// the far / near feed ratio at DC (feed 4.5 dB bs2b, 9.5 dB Meier), n0 =
// 1 / sqrt (1 + r^2) and g = r n0, and LP the first-order TPT low-pass at fc
// (700 / 650 Hz):
//   far ear  X = g z^-D LP        (head shadow, interaural delay)
//   near ear N = 1 - (1 - n0) LP  (a first-order shelf, n0 at DC, 1 at HF)
// |N|^2 + |X|^2 = (n0^2 + g^2 + W^2) / (1 + W^2) = 1 for every frequency W
// (bilinear, so exact in the digital domain too; the Lagrange read droops
// only where LP has already removed the signal): a hard-panned source and
// uncorrelated L / R keep their power at every frequency. D = 0.235 ms, read
// with a 3rd-order Lagrange interpolator (as the virtualiser's); the
// low-pass adds its own delay, so the far ear lags the near one by 0.27 ms
// by cross-correlation at 44.1 / 48 kHz (0.25 ms at 96 / 192 kHz), about the
// Woodworth ITD of a speaker at +-30 degrees (0.26 ms, head radius
// 8.75 cm; the pre-E12 crossfeed: 0.03 ms). A centred source
// sums coherently, |N + X| = (1 + r) / sqrt (1 + r^2) at DC: +2.7 dB (bs2b)
// / +2.0 dB (Meier) at crossfeed 1, falling to 0 dB above a few kHz - the
// low-frequency build-up a stereo speaker pair gives its phantom centre. The
// low-passes and delay lines always run, so the crossfeed fades in from live
// state; r and fc glide (20 ms), and a type change cross-fades through the
// two gain smoothers.
//
// Auto mono safety: mean products L'R', L'^2, R'^2 of the OUTPUT (one-pole,
// 300 ms, double precision), rho = <L'R'> / sqrt (<L'^2> <R'^2>) (1 below
// -100 dBFS). Every 32 samples of stream time a safety amount s (0 .. 1)
// integrates the error e = minCorrelation - rho:
//   e > 0      : s += (32 / 300 ms) * min (1, e / 0.1)   full pull in 300 ms
//   e < -h     : s -= 32 / 3 s                            slow release
//   otherwise  : hold (hysteresis band, so the loop parks instead of hunting)
//   h = min (0.05, (1 - minCorrelation) / 2), so release stays reachable
//   when minCorrelation is close to 1
//   output below -100 dBFS (rho undefined) : hold
//   safety off or width <= 1 : s -= 32 / 300 ms
//   w_eff = w > 1 ? 1 + (w - 1)(1 - s) : w
// Only widening is pulled back: a narrowed image (w < 1) is never widened by
// the safety. w_eff is smoothed by the 20 ms width smoother, which also
// removes the 32-sample steps of s.
//
// Smoothing: width / focus dB / space gain / crossfeed gain one-pole 20 ms,
// ln (low cut) one-pole 50 ms, all per sample. The control countdown runs in
// stream time, every smoother is per sample and the state hygiene (denormal
// flush, non-finite recovery) runs on the control tick, so the output is
// bit-identical for any host block size.
//
// CPU per stereo sample: 8 SVF ticks (shelf, the width guard's two
// high-passes, bell, the focus guard's two band-passes, HP, dip), three
// one-poles, 6 delay-line writes and 12 reads (8 of them the crossfeed's
// Lagrange reads, only while it is on) and 3 double-precision MACs;
// coefficient math only while a parameter glides.
#include "flub/dsp/StereoSpatializer.h"

#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>

namespace flub
{
namespace
{
constexpr float kMaxWidth = 2.0f;
constexpr float kMinLowCutHz = 60.0f, kMaxLowCutHz = 500.0f;

constexpr float kShelfK = 1.41421356f; // 1 / Q, Q = 1 / sqrt 2 (no overshoot)

constexpr double kFocusHz = 3000.0;
constexpr float kFocusQ = 0.5f;
// +3 dB on S at 3 kHz: about +2.9 dB of added ILD on a source 6 dB to one
// side (docs/11 E24 caps the added ILD at 3 dB; 6 dB added 7.9 dB).
constexpr float kFocusMaxDb = 3.0f;
// At and below this rate the output is a Bluetooth hands-free / speech link
// (8 / 16 / 32 kHz, mono and narrowband), where an ILD emphasis near 3 kHz
// has nothing to sharpen: focus is off (docs/11 E24, E17; the chain turns the
// gaming footsteps band off at the same rates).
constexpr double kSpeechLinkMaxRate = 32000.0;
constexpr float kDbToLnA = 0.0575646273f; // ln (10) / 40: dB -> ln A of the bell
constexpr float kFocusEnvReleaseMs = 30.0f;   // polarity guard: band envelope release
constexpr float kFocusGuardReleaseMs = 50.0f; // polarity guard: recovery of the lift

constexpr double kSpaceHpHz = 300.0;
constexpr double kSpaceHpQ = 0.70710678;
// Presence dip on the ambience feed (docs/11 E12): -5 dB at 1 and 4 kHz,
// -7 dB at 2 kHz, where a centred voice carries its presence.
constexpr double kSpaceDipHz = 2000.0;
constexpr double kSpaceDipQ = 0.4;
constexpr double kSpaceDipDb = -7.0;
constexpr float kSpaceScale = 0.5f;
constexpr float kAllPassG = 0.5f;
// 10 ms: the direct all-pass tap's ripple averages out within every 1/3
// octave from 100 Hz (ILD of a centred impulse < 1 dB; 5 ms gave 1.9 dB).
constexpr float kPreDelayMs = 10.0f;
constexpr float kOuterMs = 7.3f, kMiddleMs = 4.7f, kInnerMs = 3.1f;

// Mono-safe crossfeed (the pre-E12 M / S shelf).
constexpr double kCrossfeedHz = 700.0;
constexpr float kCrossfeedScale = 0.6f;

// Bs2b / Meier crossfeed. Far-ear delay: with the head shadow's own delay
// the cross-correlation ITD is 0.27 ms at 44.1 / 48 kHz, about the Woodworth
// ITD of a speaker at +-30 degrees (a / c (theta + sin theta), a = 8.75 cm,
// c = 343 m/s: 0.26 ms); docs/11 E12 asks for 0.22-0.30 ms.
constexpr double kCrossfeedItdMs = 0.235;
struct CrossfeedModel
{
    double hz;    // head-shadow corner
    float feedDb; // far ear below near ear at DC, crossfeed 1
};
constexpr CrossfeedModel kBs2b { 700.0, 4.5f };
constexpr CrossfeedModel kMeier { 650.0, 9.5f };

CrossfeedModel crossfeedModel (CrossfeedType type) noexcept
{
    return type == CrossfeedType::Meier ? kMeier : kBs2b;
}

/** Far / near feed ratio at DC of the Bs2b / Meier crossfeed (0 when off or Mono-safe). */
float crossfeedRatio (const SpatializerParams& p) noexcept
{
    if (p.crossfeedType == CrossfeedType::MonoSafe)
        return 0.0f;
    return p.crossfeed * std::pow (10.0f, -0.05f * crossfeedModel (p.crossfeedType).feedDb);
}

/** Gain of the Mono-safe shelf (0 unless that type is selected). */
float monoSafeCrossfeedGain (const SpatializerParams& p) noexcept
{
    return p.crossfeedType == CrossfeedType::MonoSafe ? kCrossfeedScale * p.crossfeed : 0.0f;
}

constexpr double kCorrelationMs = 300.0;
// Product of the two mean squares below which the correlation is undefined
// (geometric-mean level -100 dBFS): the meter then reads 1 and the mono
// safety holds its current pull.
constexpr double kCorrelationFloor = 1.0e-20;

constexpr float kParamSmoothMs = 20.0f;
constexpr float kLowCutSmoothMs = 50.0f;

constexpr float kSafetyAttackMs = 300.0f;   // s: 0 -> 1 at full error
constexpr float kSafetyReleaseMs = 3000.0f; // s: 1 -> 0 once correlation recovered
constexpr float kSafetyOffMs = 300.0f;      // s: 1 -> 0 when the safety is disabled
constexpr float kSafetyErrorRange = 0.1f;   // error at which the pull rate saturates
constexpr float kSafetyHysteresis = 0.05f;

// Filter / delay-line values below -300 dB re full scale are flushed to zero
// so decaying tails never crawl through subnormals when the host did not
// enable FTZ.
constexpr float kStateFloor = 1.0e-15f;

float flushed (float v) noexcept { return std::abs (v) < kStateFloor ? 0.0f : v; }

void flush (SvfState& s) noexcept
{
    s.ic1 = flushed (s.ic1);
    s.ic2 = flushed (s.ic2);
}

/** NaN -> fallback (the last valid value); everything else, +-inf included, clamps. */
float clampOr (float v, float lo, float hi, float fallback) noexcept
{
    return std::isnan (v) ? fallback : std::clamp (v, lo, hi);
}

SpatializerParams sanitise (const SpatializerParams& p, const SpatializerParams& previous) noexcept
{
    SpatializerParams s;
    s.width = clampOr (p.width, 0.0f, kMaxWidth, previous.width);
    s.widthLowCutHz = clampOr (p.widthLowCutHz, kMinLowCutHz, kMaxLowCutHz, previous.widthLowCutHz);
    s.positionalFocus = clampOr (p.positionalFocus, 0.0f, 1.0f, previous.positionalFocus);
    s.space = clampOr (p.space, 0.0f, 1.0f, previous.space);
    s.crossfeed = clampOr (p.crossfeed, 0.0f, 1.0f, previous.crossfeed);
    const int type = static_cast<int> (p.crossfeedType);
    s.crossfeedType = type >= 0 && type <= static_cast<int> (CrossfeedType::MonoSafe) ? p.crossfeedType : previous.crossfeedType;
    s.autoMonoSafety = p.autoMonoSafety;
    s.minCorrelation = clampOr (p.minCorrelation, -1.0f, 1.0f, previous.minCorrelation);
    return s;
}

void setA (SvfCoeffs& c, float g, float k) noexcept
{
    c.a1 = 1.0f / (1.0f + g * (g + k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    c.g = static_cast<double> (g);
    c.k = static_cast<double> (k);
}

/** 3rd-order (4-tap) Lagrange interpolator for a delay of `delay` samples,
    applied as sum h[k] x[n - base - k] (as HeadphoneVirtualizer's): the
    fractional position d = delay - base stays in [1, 2), the centred range
    of a 4-tap Lagrange filter (delays below 1 sample use d in [0, 1)). */
void lagrangeTaps (float delay, int& base, std::array<float, 4>& h) noexcept
{
    base = std::max (0, static_cast<int> (std::floor (delay)) - 1);
    const float d = delay - static_cast<float> (base);
    const float d1 = d - 1.0f, d2 = d - 2.0f, d3 = d - 3.0f;
    h[0] = -d1 * d2 * d3 * (1.0f / 6.0f);
    h[1] = d * d2 * d3 * 0.5f;
    h[2] = -d * d1 * d3 * 0.5f;
    h[3] = d * d1 * d2 * (1.0f / 6.0f);
}
} // namespace

//==============================================================================
void StereoSpatializer::DelayBuffer::allocate (int maxDelay)
{
    const int size = nextPowerOfTwo (std::max (2, maxDelay + 1));
    data.assign (static_cast<size_t> (size), 0.0f);
    mask = size - 1;
}

//==============================================================================
void StereoSpatializer::prepare (const ProcessSpec& newSpec)
{
    spec = newSpec;
    spec.numChannels = std::clamp (spec.numChannels, 1, kMaxChannels);
    spec.maxBlockSize = std::max (1, spec.maxBlockSize);
    if (! (spec.sampleRate > 0.0))
        spec.sampleRate = 48000.0;
    sr = spec.sampleRate;
    focusMaxDb = sr > kSpeechLinkMaxRate ? kFocusMaxDb : 0.0f;

    // Fixed designs.
    focusG = static_cast<float> (std::tan (kPi * SvfCoeffs::clampFrequency (kFocusHz, sr) / sr));
    focusDetectCoeffs = SvfCoeffs::make (FilterType::BandPass, kFocusHz, kFocusQ, 0.0, sr);
    envRelease = static_cast<float> (std::exp (-1.0 / (kFocusEnvReleaseMs * 0.001 * sr)));
    guardRelease = static_cast<float> (std::exp (-1.0 / (kFocusGuardReleaseMs * 0.001 * sr)));
    spaceHpCoeffs = SvfCoeffs::make (FilterType::HighPass, kSpaceHpHz, kSpaceHpQ, 0.0, sr);
    spaceDipCoeffs = SvfCoeffs::make (FilterType::Bell, kSpaceDipHz, kSpaceDipQ, kSpaceDipDb, sr);
    crossfeedG = onePoleG (static_cast<float> (kCrossfeedHz));
    corrCoeff = std::exp (-1.0 / (kCorrelationMs * 0.001 * sr));

    const auto perTick = [this] (float ms) { return static_cast<float> (kControlInterval / (static_cast<double> (ms) * 0.001 * sr)); };
    safetyAttackStep = perTick (kSafetyAttackMs);
    safetyReleaseStep = perTick (kSafetyReleaseMs);
    safetyOffStep = perTick (kSafetyOffMs);

    // Ambience network and crossfeed ITD lines. Every line is a power of two
    // no longer than the longest one, so a single wrapped write position
    // serves all of them.
    preDelaySamples = std::max (1, msToSamples (kPreDelayMs, sr));
    outerDelay = std::max (1, msToSamples (kOuterMs, sr));
    middleDelay = std::max (1, msToSamples (kMiddleMs, sr));
    innerDelay = std::max (1, msToSamples (kInnerMs, sr));
    preDelayLine.allocate (preDelaySamples);
    outerLine.allocate (outerDelay);
    middleLine.allocate (middleDelay);
    innerLine.allocate (innerDelay);
    lagrangeTaps (static_cast<float> (kCrossfeedItdMs * 0.001 * sr), xfeedBase, xfeedTaps);
    xfeedLineL.allocate (xfeedBase + 3);
    xfeedLineR.allocate (xfeedBase + 3);
    writeMask = std::max ({ preDelayLine.mask, outerLine.mask, middleLine.mask, innerLine.mask, xfeedLineL.mask });

    widthSmoother.reset (sr, kParamSmoothMs, params.width);
    lowCutLogHz.reset (sr, kLowCutSmoothMs, std::log (params.widthLowCutHz));
    focusDb.reset (sr, kParamSmoothMs, focusMaxDb * params.positionalFocus);
    spaceGain.reset (sr, kParamSmoothMs, kSpaceScale * params.space);
    crossfeedGain.reset (sr, kParamSmoothMs, monoSafeCrossfeedGain (params));
    xfeedRatio.reset (sr, kParamSmoothMs, crossfeedRatio (params));
    xfeedLogHz.reset (sr, kParamSmoothMs, static_cast<float> (std::log (crossfeedModel (params.crossfeedType).hz)));

    prepared = true;
    reset();
}

void StereoSpatializer::reset() noexcept FLUB_NONBLOCKING
{
    clearState();

    // Start from the current settings with no glide.
    safety = 0.0f;
    widthSmoother.setImmediate (params.width);
    lowCutLogHz.setImmediate (std::log (params.widthLowCutHz));
    focusDb.setImmediate (focusMaxDb * params.positionalFocus);
    spaceGain.setImmediate (kSpaceScale * params.space);
    crossfeedGain.setImmediate (monoSafeCrossfeedGain (params));
    xfeedRatio.setImmediate (crossfeedRatio (params));
    xfeedLogHz.setImmediate (static_cast<float> (std::log (crossfeedModel (params.crossfeedType).hz)));

    lowCutG0 = prewarp (std::exp (lowCutLogHz.getCurrent()));
    designShelf (widthSmoother.getCurrent(), lowCutG0);
    designWidthDetect (lowCutG0);
    designFocus (focusDb.getCurrent());
    xfeedG = onePoleG (std::exp (xfeedLogHz.getCurrent()));
    designCrossfeed (xfeedRatio.getCurrent());

    controlCountdown = kControlInterval;
    correlation.store (1.0f, std::memory_order_relaxed);
    effectiveWidth.store (widthSmoother.getCurrent(), std::memory_order_relaxed);
}

void StereoSpatializer::clearState() noexcept
{
    shelfState.reset();
    widthMidState.reset();
    widthSideState.reset();
    envWidthMid = envWidthSide = envWidthAdd = 0.0f;
    widthGuard = 1.0f;
    focusState.reset();
    detectMidState.reset();
    detectSideState.reset();
    envMid = envSide = 0.0f;
    focusGuard = 1.0f;
    spaceHpState.reset();
    spaceDipState.reset();
    preDelayLine.clear();
    outerLine.clear();
    middleLine.clear();
    innerLine.clear();
    xfeedLineL.clear();
    xfeedLineR.clear();
    writePos = 0;
    lastAmbience = 0.0f;
    crossfeedState = 0.0f;
    xfeedStateL = xfeedStateR = 0.0f;
    corrLR = corrLL = corrRR = 0.0;
}

void StereoSpatializer::setParams (const SpatializerParams& p) noexcept FLUB_NONBLOCKING
{
    const SpatializerParams s = sanitise (p, params);
    if (s == params)
        return; // the chain pushes parameters every block: unchanged is free

    params = s;
    updateWidthTarget();
    lowCutLogHz.setTarget (std::log (s.widthLowCutHz));
    focusDb.setTarget (focusMaxDb * s.positionalFocus);
    spaceGain.setTarget (kSpaceScale * s.space);
    // A type change cross-fades: the gain of the old type glides to 0 while
    // that of the new one glides up (and the head-shadow corner glides).
    crossfeedGain.setTarget (monoSafeCrossfeedGain (s));
    xfeedRatio.setTarget (crossfeedRatio (s));
    if (s.crossfeedType != CrossfeedType::MonoSafe)
        xfeedLogHz.setTarget (static_cast<float> (std::log (crossfeedModel (s.crossfeedType).hz)));
}

//==============================================================================
float StereoSpatializer::prewarp (float hz) const noexcept
{
    const double f = SvfCoeffs::clampFrequency (static_cast<double> (hz), sr);
    return static_cast<float> (std::tan (kPi * f / sr));
}

float StereoSpatializer::onePoleG (float hz) const noexcept
{
    const double g = std::tan (kPi * SvfCoeffs::clampFrequency (static_cast<double> (hz), sr) / sr);
    return static_cast<float> (g / (1.0 + g));
}

void StereoSpatializer::designShelf (float width, float g0) noexcept
{
    // Cytomic high shelf with gain A^2 = max (w, 1): g = g0 sqrt (A),
    // m = (A^2, k (1 - A) A, 1 - A^2). Written with wHi instead of A^2 so the
    // LF gain m0 + m2 is exactly 1 and w = 1 is exactly (1, 0, 0).
    const float wHi = std::max (width, 1.0f);
    const float a = std::sqrt (wHi);
    setA (shelfCoeffs, g0 * std::sqrt (a), kShelfK);
    shelfCoeffs.m0 = wHi;
    shelfCoeffs.m1 = kShelfK * (1.0f - a) * a;
    shelfCoeffs.m2 = 1.0f - wHi;
    shelfWidth = width;
    shelfG0 = g0;
}

void StereoSpatializer::designWidthDetect (float g0) noexcept
{
    // High-pass (Q 1/sqrt 2) at the low cut: the band the shelf widens.
    setA (widthDetectCoeffs, g0, kShelfK);
    widthDetectCoeffs.m0 = 1.0f;
    widthDetectCoeffs.m1 = -kShelfK;
    widthDetectCoeffs.m2 = -1.0f;
}

void StereoSpatializer::designFocus (float gainDb) noexcept
{
    // Cytomic bell (as SvfCoeffs::make): k = 1 / (Q A), m1 = k (A^2 - 1).
    const float a = std::exp (gainDb * kDbToLnA);
    const float k = 1.0f / (kFocusQ * a);
    setA (focusCoeffs, focusG, k);
    focusCoeffs.m0 = 1.0f;
    focusCoeffs.m1 = k * (a * a - 1.0f);
    focusCoeffs.m2 = 0.0f;
    focusCentreGain = a * a;
}

void StereoSpatializer::designCrossfeed (float ratio) noexcept
{
    // Power-complementary near / far pair: n0^2 + g^2 = 1, g / n0 = ratio.
    const float n0 = 1.0f / std::sqrt (1.0f + ratio * ratio);
    xfeedNearCut = 1.0f - n0;
    xfeedFar = ratio * n0;
    xfeedDesigned = ratio;
}

void StereoSpatializer::updateWidthTarget() noexcept
{
    // Only widening is pulled back; for w in (1, 2] and s = 0 the expression
    // is exactly w (w - 1 and 1 + (w - 1) are exact in float).
    const float w = params.width;
    widthSmoother.setTarget (w > 1.0f ? 1.0f + (w - 1.0f) * (1.0f - safety) : w);
}

float StereoSpatializer::correlationEstimate() const noexcept
{
    const double den = corrLL * corrRR;
    if (! (den > kCorrelationFloor))
        return 1.0f;
    return static_cast<float> (std::clamp (corrLR / std::sqrt (den), -1.0, 1.0));
}

void StereoSpatializer::controlTick() noexcept
{
    // State hygiene runs here, in stream time, rather than once per host
    // block: the flush points (and so every output bit) are then the same for
    // any block size, a subnormal tail can crawl for at most 32 samples even
    // with 4096-sample blocks, and a NaN is contained within 32 samples.
    sanitiseState();

    if (corrLL + corrRR < 1.0e-30)
        corrLR = corrLL = corrRR = 0.0; // long silence: no subnormal doubles

    if (! params.autoMonoSafety || params.width <= 1.0f)
    {
        safety -= safetyOffStep;
    }
    else if (corrLL * corrRR > kCorrelationFloor)
    {
        // The release threshold may not exceed a correlation of 1: with
        // minCorrelation near 1 a fixed 0.05 band would make release
        // unreachable, and the width could never recover even on content
        // that is back above the target.
        const float hysteresis = std::min (kSafetyHysteresis, 0.5f * (1.0f - params.minCorrelation));
        const float err = params.minCorrelation - correlationEstimate();
        if (err > 0.0f)
            safety += safetyAttackStep * std::min (1.0f, err / kSafetyErrorRange);
        else if (err < -hysteresis)
            safety -= safetyReleaseStep;
    }
    // else: (near) silence says nothing about the material - hold the pull,
    // so a game's pauses do not pump the width back up between events.
    safety = std::clamp (safety, 0.0f, 1.0f);
    updateWidthTarget();
}

float StereoSpatializer::ambience (float mid, int pos) noexcept
{
    const float g = kAllPassG;

    preDelayLine.write (pos, svfTick (spaceDipCoeffs, spaceDipState, svfTick (spaceHpCoeffs, spaceHpState, mid)));
    const float x = preDelayLine.read (pos, preDelaySamples);

    // Nested Schroeder all-passes: each section's delayed path runs through
    // the next section (v = in + g w, y = w - g v). All reads precede the
    // writes (every delay >= 1 sample).
    const float u1 = outerLine.read (pos, outerDelay);
    const float u2 = middleLine.read (pos, middleDelay);
    const float u3 = innerLine.read (pos, innerDelay);

    const float v3 = flushed (u2 + g * u3);
    const float y3 = u3 - g * v3;
    const float v2 = flushed (u1 + g * y3);
    const float y2 = y3 - g * v2;
    const float v1 = flushed (x + g * y2);
    const float y1 = y2 - g * v1;

    innerLine.write (pos, v3);
    middleLine.write (pos, v2);
    outerLine.write (pos, v1);
    lastAmbience = y1;
    return y1;
}

void StereoSpatializer::sanitiseState() noexcept
{
    // A non-finite input sample (normally caught by the chain's protection)
    // must not poison the recursive states for good: start clean instead.
    // Any non-finite output also latches into the correlation accumulators,
    // so a NaN / inf that only lives in a delay line is caught once it
    // reaches the output.
    const float sum = shelfState.ic1 + shelfState.ic2 + widthMidState.ic1 + widthMidState.ic2 + widthSideState.ic1
                      + widthSideState.ic2 + envWidthMid + envWidthSide + envWidthAdd + focusState.ic1 + focusState.ic2
                      + detectMidState.ic1 + detectMidState.ic2 + detectSideState.ic1 + detectSideState.ic2 + envMid + envSide
                      + spaceHpState.ic1 + spaceHpState.ic2 + spaceDipState.ic1 + spaceDipState.ic2 + crossfeedState
                      + xfeedStateL + xfeedStateR + lastAmbience;
    if (! std::isfinite (sum) || ! std::isfinite (corrLR + corrLL + corrRR))
    {
        clearState();
        return;
    }

    flush (shelfState);
    flush (widthMidState);
    flush (widthSideState);
    envWidthMid = flushed (envWidthMid);
    envWidthSide = flushed (envWidthSide);
    envWidthAdd = flushed (envWidthAdd);
    flush (focusState);
    flush (detectMidState);
    flush (detectSideState);
    envMid = flushed (envMid);
    envSide = flushed (envSide);
    flush (spaceHpState);
    flush (spaceDipState);
    crossfeedState = flushed (crossfeedState);
    xfeedStateL = flushed (xfeedStateL);
    xfeedStateR = flushed (xfeedStateR);
}

//==============================================================================
void StereoSpatializer::process (const AudioBlock& block) noexcept FLUB_NONBLOCKING
{
    if (! prepared || block.numChannels != 2)
        return; // stereo only: any other channel count passes through untouched

    float* left = block.channel (0);
    float* right = block.channel (1);

    for (int i = 0; i < block.numSamples; ++i)
    {
        if (--controlCountdown <= 0)
        {
            controlCountdown = kControlInterval;
            controlTick();
        }

        const int pos = writePos;
        writePos = (pos + 1) & writeMask;

        const float l = left[i], r = right[i];
        const float mid = 0.5f * (l + r);
        const float side = 0.5f * (l - r);

        // ---- width ----
        if (lowCutLogHz.isSmoothing())
        {
            lowCutG0 = prewarp (std::exp (lowCutLogHz.next()));
            designWidthDetect (lowCutG0);
        }
        const float w = widthSmoother.next();
        if (w != shelfWidth || lowCutG0 != shelfG0)
            designShelf (w, lowCutG0);
        float s = side;
        {
            // Polarity guard: the added side signal may raise S up to M in the
            // band above the low cut, never past it (w <= 1: added == 0).
            const float added = svfTick (shelfCoeffs, shelfState, side) - side;
            envWidthMid = std::max (std::abs (svfTick (widthDetectCoeffs, widthMidState, mid)), envRelease * envWidthMid);
            envWidthSide = std::max (std::abs (svfTick (widthDetectCoeffs, widthSideState, side)), envRelease * envWidthSide);
            envWidthAdd = std::max (std::abs (added), envRelease * envWidthAdd);
            const float room = envWidthMid - envWidthSide;
            const float target = envWidthAdd <= room ? 1.0f : (room > 0.0f ? room / envWidthAdd : 0.0f);
            widthGuard = target < widthGuard ? target : target + guardRelease * (widthGuard - target);
            s = (side + widthGuard * added) * std::min (w, 1.0f); // w = 1: exactly side
        }

        // ---- positional focus ----
        if (focusDb.isSmoothing())
            designFocus (focusDb.next());
        {
            const float focused = svfTick (focusCoeffs, focusState, s);
            envMid = std::max (std::abs (svfTick (focusDetectCoeffs, detectMidState, mid)), envRelease * envMid);
            envSide = std::max (std::abs (svfTick (focusDetectCoeffs, detectSideState, s)), envRelease * envSide);
            const float lift = envSide * (focusCentreGain - 1.0f);
            const float target = lift <= envMid - envSide ? 1.0f : std::max (0.0f, (envMid - envSide) / lift);
            focusGuard = target < focusGuard ? target : target + guardRelease * (focusGuard - target);
            s += focusGuard * (focused - s); // focus 0: focused == s, exact
        }

        // ---- space ---- (the network always runs, so raising space never
        // starts from an empty reverb)
        s += spaceGain.next() * ambience (mid, pos);

        // ---- Mono-safe crossfeed ----
        {
            const float v = (s - crossfeedState) * crossfeedG;
            const float lp = v + crossfeedState;
            crossfeedState = lp + v;
            s -= crossfeedGain.next() * lp;
        }

        // Neutral settings leave S bit-exact: keep L / R bit-exact too
        // (M + S does not round back to L in general).
        float lo = l, ro = r;
        bool changed = false;
        if (s != side)
        {
            lo = mid + s;
            ro = mid - s;
            changed = true;
        }

        // ---- Bs2b / Meier crossfeed ---- (the head-shadow filters and ITD
        // lines always run, so the crossfeed fades in from live state)
        {
            if (xfeedLogHz.isSmoothing())
                xfeedG = onePoleG (std::exp (xfeedLogHz.next()));
            const float ratio = xfeedRatio.next();
            if (ratio != xfeedDesigned)
                designCrossfeed (ratio);

            const float vl = (lo - xfeedStateL) * xfeedG;
            const float shadowL = vl + xfeedStateL;
            xfeedStateL = shadowL + vl;
            const float vr = (ro - xfeedStateR) * xfeedG;
            const float shadowR = vr + xfeedStateR;
            xfeedStateR = shadowR + vr;
            xfeedLineL.write (pos, flushed (shadowL));
            xfeedLineR.write (pos, flushed (shadowR));

            if (ratio != 0.0f)
            {
                float farL = 0.0f, farR = 0.0f; // the other channel, delayed by the ITD
                for (int k = 0; k < 4; ++k)
                {
                    const float h = xfeedTaps[static_cast<size_t> (k)];
                    farL += h * xfeedLineR.read (pos, xfeedBase + k);
                    farR += h * xfeedLineL.read (pos, xfeedBase + k);
                }
                lo = lo - xfeedNearCut * shadowL + xfeedFar * farL;
                ro = ro - xfeedNearCut * shadowR + xfeedFar * farR;
                changed = true;
            }
        }

        if (changed)
        {
            left[i] = lo;
            right[i] = ro;
        }

        const double dl = static_cast<double> (lo), dr = static_cast<double> (ro);
        corrLR = dl * dr + corrCoeff * (corrLR - dl * dr);
        corrLL = dl * dl + corrCoeff * (corrLL - dl * dl);
        corrRR = dr * dr + corrCoeff * (corrRR - dr * dr);
    }

    correlation.store (correlationEstimate(), std::memory_order_relaxed);
    effectiveWidth.store (widthSmoother.getCurrent(), std::memory_order_relaxed);
}
} // namespace flub
