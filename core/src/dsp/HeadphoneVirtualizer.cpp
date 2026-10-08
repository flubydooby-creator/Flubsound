// Flubsound Pro - multichannel (5.1 / 7.1) to binaural headphone virtualiser.
//
// Signal flow (every speaker path adds into two ear accumulators; the output
// is (make-up * speakers + LFE) * -3 dB trim * fold headroom * swap fade):
//
//   speaker (parametric):  x [-> 6 direction cues] -> rear-cue shelf -> ITD line -+-> Lagrange(D_L) -> shadow_L -> ear L
//                                                                                 +-> Lagrange(D_R) -> shadow_R -> ear R
//   speaker (HRIR):        x -> history -> dot(h_L) -> ear L,  dot(h_R) -> ear R
//   LFE:                   x -> 4th-order Butterworth LP 120 Hz -> lfeGain -> both ears
//                          (LfeFold, shared with the chain's BS.775 fold)
//   room:                  sum of speaker inputs -> HP 200 Hz -> LP 5 kHz -> 6 taps
//                          (4 .. 19 ms, alternating ears) * roomAmount
//   level match (E28a):    D = BS.775 matrix of the speaker inputs (no LFE);
//                          K-weighted powers of D and of the ear sums -> make-up
//
// Level match (docs/11 E28a). The chain's alternative to this module is the
// BS.775 downmix 0.7071 * D, and this module's trim is the same 0.7071, so
// matching the ear sums to D matches virt on to virt off. Both are K-weighted
// (BS.1770: the +4 dB shelf and the 38 Hz high-pass, as SVFs on four lanes,
// run on every other sample) and summed over both channels, as a loudness
// meter does. The loudness ratio depends on the
// content: a single speaker reaches both ears here and one side there (+3 dB
// here); uncorrelated speakers add in power in both folds; correlated ones add
// in amplitude in both, but here with ITD and head-shadow differences that
// comb the highs. So a fixed gain cannot match both ends, and the gain is:
//   diffuse = sqrt(P_D / P_ears) for uncorrelated pink noise on every speaker,
//             computed from the running design at a layout swap: a 1/3-octave
//             grid (20 Hz .. 20 kHz) of |K|^2 times |shadow|^2 |shelf|^2 per
//             ear path (or |HRIR|^2), plus the reflections' power;
//   make-up  = sqrt(<P_D> / <P_ears>), the powers averaged over 3 s (one-pole,
//             per 16-sample period, only periods of D above -70 LUFS: silence
//             freezes it), clamped to diffuse +-4 dB, moving at most 6 dB/s,
//             and ramped linearly across each period. Before any content it
//             is the diffuse gain.
// It is feed-forward (the render is measured before the make-up), so it
// cannot oscillate. The LFE is outside it: it is the same LfeFold in both
// folds. The averages survive reset() (the chain resets the module whenever
// virt comes back on, and the content has not changed); prepare() and a
// layout swap start them again. levelMatch off glides the make-up to unity.
//
// Fold headroom (E28a). Correlated content on every speaker adds up: 7.1 with
// the same full-scale signal everywhere reaches about +10 dBFS. A linked peak
// gain on the output (before the swap fade) holds it at 0 dBFS: when a sample
// would exceed 0 dBFS the gain drops at once to exactly 0 dBFS / peak, holds
// for 10 ms and then recovers with a 150 ms one-pole. It has no look-ahead
// (the module has zero latency), so the first rising edge of an over is
// flattened; below 0 dBFS it does nothing. foldHeadroom off lets it recover.
//
// Geometry (Brown & Duda 1998 spherical head, radius a, c = 343 m/s): for a
// source at azimuth az and an ear at -90 (left) / +90 (right) degrees,
// theta = angle between the source and the ear axis (0 .. 180 deg):
//   delay  D = (a/c)(1 - cos theta) * fs             theta < 90 deg
//            = (a/c)(1 + theta - pi/2) * fs          theta >= 90 deg
//   shadow H(s) = (1 + alpha s / (2 w0)) / (1 + s / (2 w0)),  w0 = c/a,
//          alpha = 1.05 + 0.95 cos(theta * 180/150 deg)   (+6 dB .. -20 dB at HF)
// The delay is realised with a 4-tap (3rd-order) Lagrange interpolator. The
// centre speaker therefore reaches both ears after a/c (~12 samples at 48 kHz):
// that common path delay is part of the acoustic model, so the module
// reports zero latency (no look-ahead, no block delay). Enhanced references
// the delays to the nearer ear instead (below).
//
// Direction cues (docs/11 E28, VirtualizerParams::renderer). Classic, the
// default and the v1 renderer, has one front/back cue: the -4 dB rear shelf,
// switched at |az| = 90 deg (a 10 ms glide). Enhanced replaces it with six
// sections per speaker, ahead of the shelf and common to both ears (so the
// ITD and the ILD of the sphere are untouched), whose gains are continuous
// in the angle from the front, phi = |az| (c = cos phi):
//   timbre     high shelf 3 kHz, Q 0.5: -0.6 * 10 log10 (P(az) / <P>), P the
//              sphere's both-ear HF power alpha(theta_L)^2 + alpha(theta_R)^2
//              and <P> = 2.5992 its mean over the circle: the centre (both
//              ears in the head's shadow, +2.1 dB) and the sides (-1.1 dB at
//              100 deg) move 60 % of the way to the same brightness;
//   front      bell 4 kHz, Q 2.5, +5 dB * c (a cut behind), and high shelf
//              13.5 kHz, +6 dB * max(0, c) (Blauert's frontal bands);
//   rear       bells 1 kHz, Q 1.4, +4 dB, and 10 kHz, Q 2, +6 dB, both
//              * max(0, -c) (the rear bands);
//   pinna      bell -10 dB, Q 2, at 7.6 kHz * 2^(-0.1 (1 - c) / 2) (7.6 kHz
//              in front, 7.1 kHz behind).
// The four band gains are scaled by frontBack / 0.5 (0: none, 1: twice the
// nominal contrast). Enhanced also references each speaker's two Woodworth
// delays to its nearer ear: e times the near ear's delay comes off both ears,
// so at e = 1 every speaker reaches its near ear at sample 0 (exactly: the
// Lagrange taps of a zero delay are 1, 0, 0, 0) and its far ear after its own
// ITD (docs/11 E28, the 4-8 kHz comb row). The ITD and the ILD of every
// speaker are the model's, as in Classic (only the far ear is interpolated
// now). Head-centred delays make the speakers of one side and the centre
// reach that side's ear up to a/c apart (SL 0.19, BL 5.2, FL 6.1, FC 12.2
// samples at 48 kHz), so correlated content on several speakers combs the
// near ear, where the level is: SL against FL + BL notches at 4.4 kHz. Aligned,
// those paths add in phase at the near ear and the far ear (10 - 20 dB lower
// up there) carries the interaural lags; both ears' power of any correlated
// pair combs no deeper than with head-centred delays (the far ear of a
// same-side pair combs deeper, lower down). The renderer switch glides:
// the Enhanced share e moves 0 <-> 1 (one-pole 30 ms at the control rate),
// every cue gain is e times its Enhanced value, the rear shelf's gain (1 - e)
// times its Classic value and the near-ear reference e times its delay,
// redesigned on the control ticks and interpolated per sample as for the
// geometry. The sections of four speakers run side by side (one SSE register
// per section, renderCues). At e = 0 they are not run at all, so Classic is
// the v1 code path bit for bit. The level match's diffuse-field gain includes
// the cues and is recomputed when a glide ends. The HRIR renderer has its
// own cues (the measured set) and ignores both settings.
//
// Control rate: every kControlInterval samples, counted in absolute stream
// time, a "busy" virtualiser advances its geometry smoothers (angles and head
// radius, one-pole 30 ms; rear-cue shelf gains, one-pole 10 ms), redesigns
// delays and filters, and steps the layout-swap state machine. Across the
// following control period delays and coefficients are interpolated per
// sample from the previous design to the new one, so a parameter sweep is a
// chain of short linear ramps (no zipper noise, no clicks). When nothing is
// busy the block is one segment; the output is sample-identical either way,
// so the result does not depend on the host block size. Room and LFE levels
// ramp linearly per sample (20 ms).
//
// Discrete changes: a layout change alters what each input channel means (and
// may switch renderer), so it is not glided. The output fades to silence over
// ~5 ms, roles / renderer / geometry are swapped and the per-channel state is
// cleared at silence. The new configuration then runs silently for a short
// pre-roll (2 ms; up to 10 ms for an HRIR) so its delay lines hold real input
// before the ~5 ms fade-in: fading in while a path is still empty would make
// the signal arrive as a step at a non-zero gain (a small click). The
// reflection line is not cleared, so content common to both layouts keeps
// its reflections continuous.
//
// Renderer selection: the HRIR renderer runs when a well-formed HrirSet was
// set before prepare(), its sampleRate equals the session rate (within
// 0.5 Hz) and its layout equals the running layout. Otherwise the parametric
// renderer runs (a mismatched set is ignored, never resampled here). The LFE
// path, the reflections and the trim are shared by both renderers.
//
// Channel handling: inputs beyond the layout are ignored; layout channels
// missing from a block are silent; a 1-channel block receives the mono
// fold-down (L + R) / 2 of the binaural pair.
#include "flub/dsp/HeadphoneVirtualizer.h"

#include "flub/common/Math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>

#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86_FP)
    #include <xmmintrin.h>
    #define FLUB_VIRT_SSE 1
#else
    #define FLUB_VIRT_SSE 0
#endif

namespace flub
{
namespace
{
constexpr double kSpeedOfSound = 343.0; // m/s

constexpr float kMinFrontDeg = 22.0f, kMaxFrontDeg = 45.0f;
constexpr float kMinSideDeg = 80.0f, kMaxSideDeg = 120.0f;
constexpr float kMinRearDeg = 120.0f, kMaxRearDeg = 165.0f;
constexpr float kMinHeadMm = 70.0f, kMaxHeadMm = 105.0f;
constexpr float kMinLfeDb = LfeFold::kMinDb, kMaxLfeDb = LfeFold::kMaxDb;

// -3 dB headroom trim: a phantom source that hits both ears in phase (centre
// speaker plus reflections plus LFE) must not overload the rest of the chain.
constexpr float kTrim = 0.70794578f; // 10^(-3/20)

// Rear cue: the pinna shadows sources behind the head above ~3 kHz. A sphere
// is front/back symmetric, so without this the rear speakers fold forward.
constexpr double kRearShelfHz = 4000.0;
constexpr double kRearShelfQ = 0.70710678;
constexpr float kRearShelfDb = -4.0f;

// Direction cues of the Enhanced renderer (docs/11 E28, see the file comment).
constexpr double kTimbreHz = 3000.0, kTimbreQ = 0.5, kTimbreShare = 0.6;
constexpr double kLateralPowerMean = 2.599239; // <alpha(theta_L)^2 + alpha(theta_R)^2> over the circle
constexpr double kFrontBandHz = 4000.0, kFrontBandQ = 2.5, kFrontBandDb = 5.0;
constexpr double kFrontShelfHz = 13500.0, kFrontShelfQ = 0.70710678, kFrontShelfDb = 6.0;
constexpr double kRearLowHz = 1000.0, kRearLowQ = 1.4, kRearLowDb = 4.0;
constexpr double kRearHighHz = 10000.0, kRearHighQ = 2.0, kRearHighDb = 6.0;
constexpr double kPinnaHz = 7600.0, kPinnaQ = 2.0, kPinnaDb = -10.0, kPinnaShiftOctaves = -0.1;
constexpr float kNominalFrontBack = 0.5f; // frontBack at which the bands have their nominal gains

// Early reflections: 6 taps, alternating ears (even -> left, odd -> right),
// weights paired so both ears receive the same reflected energy
// (0.5^2 + 0.4^2 + 0.3^2 = 0.5, i.e. -3 dB re the direct path at room = 1).
// The delays are mutually non-harmonic to avoid a periodic comb. The bus is
// band-limited: walls absorb the top end (low-pass) and low-frequency
// reflections would only add boom and comb the bass (high-pass).
constexpr std::array<float, 6> kReflectionMs { 4.0f, 5.3f, 8.9f, 11.7f, 15.4f, 19.0f };
constexpr std::array<float, 6> kReflectionWeight { 0.5f, 0.5f, 0.4f, 0.4f, 0.3f, 0.3f };
constexpr double kReflectionHpHz = 200.0;
constexpr double kReflectionLpHz = 5000.0;

constexpr float kGeometrySmoothingMs = 30.0f; // angles and head radius (one-pole)
constexpr float kRendererSmoothingMs = 30.0f; // Classic <-> Enhanced share and frontBack (one-pole)
constexpr float kShelfSmoothingMs = 10.0f;    // rear-cue shelf gain in dB (one-pole)
constexpr float kGainRampMs = 20.0f;          // room and LFE levels (linear)
constexpr float kFadeMs = 5.0f;               // layout swap: fade out, swap, fade in
constexpr float kPrerollMs = 2.0f;            // silent pre-roll after a swap (parametric)
constexpr float kMaxHrirPrerollMs = 10.0f;    // ... and the cap for an HRIR pre-roll

// Level match and fold headroom (docs/11 E28a, see the file comment).
constexpr double kKShelfHz = 1681.974450955533; // BS.1770 stage 1 (high shelf)
constexpr double kKShelfQ = 0.7071752369554196;
constexpr double kKShelfDb = 3.999843853973347;
constexpr double kKHighPassHz = 38.13547087602444; // stage 2 (RLB high-pass)
constexpr double kKHighPassQ = 0.5003270373238773;
constexpr float kMakeupAverageMs = 3000.0f; // power averages (one-pole)
constexpr float kMakeupSlewDbPerS = 6.0f;
constexpr float kMakeupRangeDb = 4.0f;      // around the diffuse-field gain
// Gate: K-weighted mean square of D summed over both channels >= -70 LUFS.
constexpr double kMakeupGate = 1.1695e-7;   // 10^((-70 + 0.691) / 10)
constexpr float kHeadroomCeiling = 1.0f;    // 0 dBFS
constexpr float kHeadroomHoldMs = 10.0f;
constexpr float kHeadroomReleaseMs = 150.0f;

// Longest HRIR kept by the direct-form convolver (21 ms at 48 kHz: every
// anechoic HRIR set fits). Direct form costs O(taps) per sample and ear: 1024
// taps for 7.1 at 48 kHz is already ~16 % of a core, so longer sets (BRIRs
// with a room tail) are truncated with a short half-cosine fade-out rather
// than risking dropouts. A partitioned FFT convolver is on the roadmap.
constexpr int kMaxHrirTaps = 1024;
constexpr int kHrirTruncationFade = 64;

// Filter state below -300 dB re full scale is flushed to zero at the end of a
// segment so decaying IIR tails never crawl through subnormals when the host
// did not enable FTZ.
constexpr float kStateFloor = 1.0e-15f;

/** NaN -> fallback; everything else (including +-inf) clamps into range. */
float clampOr (float v, float lo, float hi, float fallback) noexcept
{
    return std::isnan (v) ? fallback : std::clamp (v, lo, hi);
}

ChannelLayout sanitiseLayout (ChannelLayout l) noexcept
{
    return static_cast<int> (l) > static_cast<int> (ChannelLayout::Surround71) ? ChannelLayout::Surround71 : l;
}

VirtualizerParams sanitise (const VirtualizerParams& p) noexcept
{
    const VirtualizerParams d;
    VirtualizerParams s;
    s.layout = sanitiseLayout (p.layout);
    s.frontAngleDeg = clampOr (p.frontAngleDeg, kMinFrontDeg, kMaxFrontDeg, d.frontAngleDeg);
    s.sideAngleDeg = clampOr (p.sideAngleDeg, kMinSideDeg, kMaxSideDeg, d.sideAngleDeg);
    s.rearAngleDeg = clampOr (p.rearAngleDeg, kMinRearDeg, kMaxRearDeg, d.rearAngleDeg);
    s.headRadiusMm = clampOr (p.headRadiusMm, kMinHeadMm, kMaxHeadMm, d.headRadiusMm);
    s.roomAmount = clampOr (p.roomAmount, 0.0f, 1.0f, d.roomAmount);
    s.lfeGainDb = clampOr (p.lfeGainDb, kMinLfeDb, kMaxLfeDb, d.lfeGainDb);
    s.lfeOn = p.lfeOn;
    s.levelMatch = p.levelMatch;
    s.foldHeadroom = p.foldHeadroom;
    s.renderer = p.renderer == VirtualizerRenderer::Enhanced ? VirtualizerRenderer::Enhanced : VirtualizerRenderer::Classic;
    s.frontBack = clampOr (p.frontBack, 0.0f, 1.0f, d.frontBack);
    return s;
}

bool hasLfe (ChannelLayout l) noexcept { return l != ChannelLayout::Stereo; }

/** The BS.775 matrix D (Bs775Fold without its overall gain and the LFE):
    channel c's weight into the left and right output. */
std::array<float, 2> downmixWeights (int c) noexcept
{
    constexpr float k = Bs775Fold::kMatrixGain;
    if (c == 0)
        return { 1.0f, 0.0f };
    if (c == 1)
        return { 0.0f, 1.0f };
    if (c == 2)
        return { k, k };
    return c % 2 == 0 ? std::array<float, 2> { k, 0.0f } : std::array<float, 2> { 0.0f, k };
}

/** The 1/3-octave grid of the diffuse-field gain, 20 Hz .. 20 kHz (pink noise
    has equal power per band, so every point weighs the same). */
constexpr int kGridFirst = -17, kGridLast = 13;
double gridFrequency (int k) noexcept { return 1000.0 * std::pow (2.0, k / 3.0); }

/** Four floats that the level match's K-weighting runs in parallel (D left /
    right, ears left / right): one SSE register where there is one, else
    plain floats. The arithmetic is the same either way. */
#if FLUB_VIRT_SSE
struct Lane4
{
    __m128 v;
};
inline Lane4 lane4 (float a, float b, float c, float d) noexcept { return { _mm_setr_ps (a, b, c, d) }; }
inline Lane4 lane4 (const std::array<float, 4>& a) noexcept { return { _mm_loadu_ps (a.data()) }; }
inline Lane4 splat (float a) noexcept { return { _mm_set1_ps (a) }; }
inline Lane4 operator+ (Lane4 a, Lane4 b) noexcept { return { _mm_add_ps (a.v, b.v) }; }
inline Lane4 operator- (Lane4 a, Lane4 b) noexcept { return { _mm_sub_ps (a.v, b.v) }; }
inline Lane4 operator* (Lane4 a, Lane4 b) noexcept { return { _mm_mul_ps (a.v, b.v) }; }
inline void store (Lane4 a, std::array<float, 4>& out) noexcept { _mm_storeu_ps (out.data(), a.v); }
#else
struct Lane4
{
    std::array<float, 4> v;
};
inline Lane4 lane4 (float a, float b, float c, float d) noexcept { return { { a, b, c, d } }; }
inline Lane4 lane4 (const std::array<float, 4>& a) noexcept { return { a }; }
inline Lane4 splat (float a) noexcept { return { { a, a, a, a } }; }
template <typename Op>
inline Lane4 each (Lane4 a, Lane4 b, Op op) noexcept
{
    return { { op (a.v[0], b.v[0]), op (a.v[1], b.v[1]), op (a.v[2], b.v[2]), op (a.v[3], b.v[3]) } };
}
inline Lane4 operator+ (Lane4 a, Lane4 b) noexcept { return each (a, b, [] (float x, float y) { return x + y; }); }
inline Lane4 operator- (Lane4 a, Lane4 b) noexcept { return each (a, b, [] (float x, float y) { return x - y; }); }
inline Lane4 operator* (Lane4 a, Lane4 b) noexcept { return each (a, b, [] (float x, float y) { return x * y; }); }
inline void store (Lane4 a, std::array<float, 4>& out) noexcept { out = a.v; }
#endif

/** SVF coefficients broadcast to four lanes. */
struct SvfLanes
{
    Lane4 a1, a2, a3, m0, m1, m2;
    explicit SvfLanes (const SvfCoeffs& c) noexcept
        : a1 (splat (c.a1)), a2 (splat (c.a2)), a3 (splat (c.a3)), m0 (splat (c.m0)), m1 (splat (c.m1)), m2 (splat (c.m2))
    {
    }
};

/** One SVF tick on four lanes (the arithmetic of svfTick). */
inline Lane4 svfLanes (const SvfLanes& c, Lane4& ic1, Lane4& ic2, Lane4 v0) noexcept
{
    const Lane4 v3 = v0 - ic2;
    const Lane4 v1 = c.a1 * ic1 + c.a2 * v3;
    const Lane4 v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
    ic1 = (v1 + v1) - ic1;
    ic2 = (v2 + v2) - ic2;
    return c.m0 * v0 + c.m1 * v1 + c.m2 * v2;
}

/** Woodworth path length to one ear in units of the head radius (delay = value * a/c).
    theta is the angle between the source and the ear axis in radians (0 .. pi). */
double woodworth (double theta) noexcept
{
    return theta < 0.5 * kPi ? 1.0 - std::cos (theta) : 1.0 + theta - 0.5 * kPi;
}

/** Brown-Duda head-shadow HF gain: 2.0 (+6 dB) facing the ear, minimum 0.1
    (-20 dB) at 150 deg and a partial recovery (the "bright spot") at 180 deg. */
double shadowAlpha (double thetaDeg) noexcept
{
    return 1.05 + 0.95 * std::cos (thetaDeg * (180.0 / 150.0) * (kPi / 180.0));
}

/** Angle (deg, 0 .. 180) between a source at azimuth azDeg and the axis of
    ear 0 (left, -90 deg) or 1 (right, +90 deg). */
double earAngleDeg (double azDeg, size_t ear) noexcept
{
    return std::abs (std::remainder (azDeg - (ear == 0 ? -90.0 : 90.0), 360.0));
}

/** What comes off both ears' Woodworth delays (samples) of one speaker: the
    Enhanced share times the nearer ear's delay (docs/11 E28: Enhanced
    references the delays to the near ear; Classic, share 0, takes off
    exactly 0, so its delays are the head-centred ones bit for bit). */
float nearEarLead (const std::array<float, 2>& delays, float share) noexcept
{
    return share * std::min (delays[0], delays[1]);
}

/** The Enhanced renderer's six direction-cue sections for a speaker at
    azimuth azDeg (see the file comment), with the Enhanced share `share`
    (0 .. 1: every gain times it) and the band scale `scale` (frontBack /
    0.5). At share 0 every section is an exact pass-through. */
std::array<SvfCoeffs, 6> directionCues (double azDeg, double share, double scale, double fs) noexcept
{
    const double phi = std::abs (std::remainder (azDeg, 360.0)); // angle from the front
    const double c = std::cos (phi * (kPi / 180.0));
    const double front = std::max (0.0, c), rear = std::max (0.0, -c);
    const double alphaL = shadowAlpha (std::abs (std::remainder (azDeg + 90.0, 360.0)));
    const double alphaR = shadowAlpha (std::abs (std::remainder (azDeg - 90.0, 360.0)));
    const double timbreDb = -kTimbreShare * 10.0 * std::log10 ((alphaL * alphaL + alphaR * alphaR) / kLateralPowerMean);
    const double bands = share * scale;
    return { SvfCoeffs::make (FilterType::HighShelf, kTimbreHz, kTimbreQ, share * timbreDb, fs),
             SvfCoeffs::make (FilterType::Bell, kFrontBandHz, kFrontBandQ, bands * kFrontBandDb * c, fs),
             SvfCoeffs::make (FilterType::HighShelf, kFrontShelfHz, kFrontShelfQ, bands * kFrontShelfDb * front, fs),
             SvfCoeffs::make (FilterType::Bell, kRearLowHz, kRearLowQ, bands * kRearLowDb * rear, fs),
             SvfCoeffs::make (FilterType::Bell, kRearHighHz, kRearHighQ, bands * kRearHighDb * rear, fs),
             SvfCoeffs::make (FilterType::Bell, kPinnaHz * std::pow (2.0, kPinnaShiftOctaves * 0.5 * (1.0 - c)), kPinnaQ,
                              share * kPinnaDb, fs) };
}

/** 3rd-order (4-tap) Lagrange interpolator for a delay of `delay` samples,
    applied as sum h[k] x[n - base - k]. The integer base keeps the fractional
    position d = delay - base in [1, 2), the centred and best-behaved range of
    a 4-tap Lagrange filter; delays below 1 sample use d in [0, 1). The taps
    are continuous in `delay` (exact integer delays select a single tap). */
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

/** Lagrange read ending at line index p (p = write index - base). Power-of-two
    mask; negative indices wrap correctly in two's complement. */
float readLagrange (const float* line, int p, int mask, const std::array<float, 4>& h) noexcept
{
    return h[0] * line[p & mask] + h[1] * line[(p - 1) & mask] + h[2] * line[(p - 2) & mask] + h[3] * line[(p - 3) & mask];
}

/** Coefficient interpolation with exact end points (t = 1 gives b bit-exactly). */
SvfCoeffs mixSvf (const SvfCoeffs& a, const SvfCoeffs& b, float t) noexcept
{
    const float u = 1.0f - t;
    SvfCoeffs c = b;
    c.a1 = u * a.a1 + t * b.a1;
    c.a2 = u * a.a2 + t * b.a2;
    c.a3 = u * a.a3 + t * b.a3;
    c.m0 = u * a.m0 + t * b.m0;
    c.m1 = u * a.m1 + t * b.m1;
    c.m2 = u * a.m2 + t * b.m2;
    return c;
}

/** Same for the first-order head-shadow sections (b2 = a2 = 0). Interpolating
    a1 between two stable poles keeps the pole inside the unit circle. */
BiquadCoeffs mixFirstOrder (const BiquadCoeffs& a, const BiquadCoeffs& b, double t) noexcept
{
    const double u = 1.0 - t;
    BiquadCoeffs c;
    c.b0 = u * a.b0 + t * b.b0;
    c.b1 = u * a.b1 + t * b.b1;
    c.a1 = u * a.a1 + t * b.a1;
    return c;
}

// Also resets non-finite state: one NaN / Inf input sample would otherwise
// latch in the recursive filters forever. The delay lines are FIR and flush
// themselves once overwritten, so the module recovers within one line length.
float flushed (float v) noexcept { return std::abs (v) < kStateFloor || ! std::isfinite (v) ? 0.0f : v; }
double flushed (double v) noexcept { return std::abs (v) < static_cast<double> (kStateFloor) || ! std::isfinite (v) ? 0.0 : v; }

void storeState (SvfState& state, const SvfState& s) noexcept
{
    state.ic1 = flushed (s.ic1);
    state.ic2 = flushed (s.ic2);
}

/** Both ears' dot products over one contiguous history window. Four partial
    sums per ear break the add dependency chain (and let the compiler use
    SIMD) while keeping a fixed, block-size independent summation order. */
void dotPair (const float* x, const float* hl, const float* hr, int n, float& outL, float& outR) noexcept
{
    float l0 = 0.0f, l1 = 0.0f, l2 = 0.0f, l3 = 0.0f;
    float r0 = 0.0f, r1 = 0.0f, r2 = 0.0f, r3 = 0.0f;
    int k = 0;
    for (; k + 4 <= n; k += 4)
    {
        l0 += x[k] * hl[k];
        l1 += x[k + 1] * hl[k + 1];
        l2 += x[k + 2] * hl[k + 2];
        l3 += x[k + 3] * hl[k + 3];
        r0 += x[k] * hr[k];
        r1 += x[k + 1] * hr[k + 1];
        r2 += x[k + 2] * hr[k + 2];
        r3 += x[k + 3] * hr[k + 3];
    }
    for (; k < n; ++k)
    {
        l0 += x[k] * hl[k];
        r0 += x[k] * hr[k];
    }
    outL = (l0 + l1) + (l2 + l3);
    outR = (r0 + r1) + (r2 + r3);
}
} // namespace

//==============================================================================
float HeadphoneVirtualizer::speakerAzimuthDeg (ChannelLayout layout, int channel, const VirtualizerParams& p) noexcept
{
    const VirtualizerParams s = sanitise (p);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float front = s.frontAngleDeg, side = s.sideAngleDeg, rear = s.rearAngleDeg;

    switch (sanitiseLayout (layout))
    {
        case ChannelLayout::Stereo:
        {
            const float az[] = { -front, front };
            return channel >= 0 && channel < 2 ? az[channel] : nan;
        }
        case ChannelLayout::Surround51:
        {
            // FL FR FC LFE SL SR
            const float az[] = { -front, front, 0.0f, nan, -side, side };
            return channel >= 0 && channel < 6 ? az[channel] : nan;
        }
        case ChannelLayout::Surround71:
        {
            // FL FR FC LFE BL BR SL SR
            const float az[] = { -front, front, 0.0f, nan, -rear, rear, -side, side };
            return channel >= 0 && channel < 8 ? az[channel] : nan;
        }
    }
    return nan;
}

void HeadphoneVirtualizer::parametricHrir (float azimuthDeg, float headRadiusMm, double sampleRate, int length, std::vector<float>& left,
                                           std::vector<float>& right, VirtualizerRenderer renderer, float frontBack)
{
    // The design of updateGeometry() (snap) and the per-sample path of
    // renderParametric<false>() for one speaker: (direction cues ->) shelf
    // -> ITD line (Lagrange read) -> head shadow, then the output trim
    // (make-up 1, no LFE).
    const auto n = static_cast<size_t> (std::max (1, length));
    left.assign (n, 0.0f);
    right.assign (n, 0.0f);
    const double fs = sampleRate > 0.0 && std::isfinite (sampleRate) ? std::clamp (sampleRate, 8000.0, 768000.0) : 48000.0;
    const float az = std::isfinite (azimuthDeg) ? static_cast<float> (std::remainder (static_cast<double> (azimuthDeg), 360.0)) : 0.0f;
    const double radius = static_cast<double> (clampOr (headRadiusMm, kMinHeadMm, kMaxHeadMm, VirtualizerParams {}.headRadiusMm)) * 0.001;
    const double headDelay = radius / kSpeedOfSound * fs;
    const double w0 = kSpeedOfSound / radius;

    const bool enhanced = renderer == VirtualizerRenderer::Enhanced;
    const float share = enhanced ? 1.0f : 0.0f;
    const auto shelf = SvfCoeffs::make (FilterType::HighShelf, kRearShelfHz, kRearShelfQ,
                                        static_cast<double> ((std::abs (az) > 90.0f ? kRearShelfDb : 0.0f) * (1.0f - share)), fs);
    const float scale = clampOr (frontBack, 0.0f, 1.0f, VirtualizerParams {}.frontBack) / kNominalFrontBack;
    const auto cues = directionCues (static_cast<double> (az), static_cast<double> (share), static_cast<double> (scale), fs);
    std::vector<float> line (n, 0.0f);
    SvfState shelfState;
    std::array<SvfState, kNumCues> cueState {};
    for (size_t i = 0; i < n; ++i)
    {
        float v = i == 0 ? 1.0f : 0.0f;
        if (enhanced)
            for (size_t k = 0; k < cues.size(); ++k)
                v = svfTick (cues[k], cueState[k], v);
        line[i] = svfTick (shelf, shelfState, v);
    }

    std::array<double, 2> thetaDeg {};
    std::array<float, 2> delays {};
    for (size_t e = 0; e < 2; ++e)
    {
        thetaDeg[e] = earAngleDeg (static_cast<double> (az), e);
        delays[e] = static_cast<float> (woodworth (thetaDeg[e] * (kPi / 180.0)) * headDelay);
    }
    const float lead = nearEarLead (delays, share);
    for (size_t e = 0; e < 2; ++e)
    {
        const float delay = delays[e] - lead;
        int base = 0;
        std::array<float, 4> taps {};
        lagrangeTaps (delay, base, taps);
        const auto shadow = BiquadCoeffs::fromAnalogFirstOrder (1.0, shadowAlpha (thetaDeg[e]) / (2.0 * w0), 1.0, 1.0 / (2.0 * w0), fs);
        BiquadState state;
        auto& out = e == 0 ? left : right;
        for (size_t i = 0; i < n; ++i)
        {
            float x = 0.0f;
            for (size_t k = 0; k < taps.size(); ++k)
            {
                const auto back = static_cast<size_t> (base) + k;
                x += i >= back ? taps[k] * line[i - back] : 0.0f;
            }
            out[i] = kTrim * static_cast<float> (biquadTick (shadow, state, static_cast<double> (x)));
        }
    }
}

//==============================================================================
bool HeadphoneVirtualizer::loadHrir()
{
    for (auto& path : hrirPaths)
    {
        path.history = {};
        path.reversed[0] = {};
        path.reversed[1] = {};
        path.present = false;
    }
    hrirLength = 0;
    hrirWrite = 0;

    if (hrir == nullptr)
        return false;

    const HrirSet& set = *hrir;
    if (static_cast<int> (set.layout) > static_cast<int> (ChannelLayout::Surround71) || set.length < 1)
        return false;

    // Measured responses are only valid at their own sample rate. Resampling
    // belongs to the (background) loader; a mismatched set falls back to the
    // parametric renderer instead of playing at the wrong pitch/geometry.
    if (! (std::abs (set.sampleRate - spec.sampleRate) <= 0.5))
        return false;

    // Entries follow the layout's channel order. The LFE entry of 5.1 / 7.1
    // may be present (ignored: the LFE always takes the low-pass path) or
    // omitted (then the entries are the layout's speakers without the LFE).
    const int numLayoutChannels = channelCount (set.layout);
    const int numEntries = static_cast<int> (set.left.size());
    const bool lfeOmitted = hasLfe (set.layout) && numEntries == numLayoutChannels - 1;
    if (static_cast<int> (set.right.size()) != numEntries || (numEntries != numLayoutChannels && ! lfeOmitted))
        return false;

    const int length = std::min (set.length, kMaxHrirTaps);
    const auto entryFor = [lfeOmitted] (int channel) { return lfeOmitted && channel > 3 ? channel - 1 : channel; };

    for (int c = 0; c < numLayoutChannels; ++c)
    {
        if (hasLfe (set.layout) && c == 3)
            continue;
        const auto e = static_cast<size_t> (entryFor (c));
        for (const auto* ir : { &set.left[e], &set.right[e] })
        {
            if (static_cast<int> (ir->size()) < length)
                return false;
            for (int k = 0; k < length; ++k)
                if (! std::isfinite ((*ir)[static_cast<size_t> (k)]))
                    return false;
        }
    }

    hrirLength = length;
    hrirLayout = set.layout;
    for (int c = 0; c < numLayoutChannels; ++c)
    {
        if (hasLfe (set.layout) && c == 3)
            continue;
        const auto e = static_cast<size_t> (entryFor (c));
        auto& path = hrirPaths[static_cast<size_t> (c)];
        const std::vector<float>* irs[2] = { &set.left[e], &set.right[e] };
        for (size_t ear = 0; ear < 2; ++ear)
        {
            // Time-reversed so the convolution is a forward dot product with
            // the (oldest-first) history window.
            path.reversed[ear].assign (static_cast<size_t> (length), 0.0f);
            for (int k = 0; k < length; ++k)
            {
                float w = 1.0f;
                if (set.length > length && k >= length - kHrirTruncationFade)
                    w = 0.5f + 0.5f * std::cos (static_cast<float> (kPi) * static_cast<float> (k - (length - kHrirTruncationFade) + 1) / static_cast<float> (kHrirTruncationFade));
                path.reversed[ear][static_cast<size_t> (length - 1 - k)] = w * (*irs[ear])[static_cast<size_t> (k)];
            }
        }
        path.history.assign (2 * static_cast<size_t> (length), 0.0f);
        path.present = true;

        // Level match: both ears' K-weighted power on the diffuse-field grid
        // (a DFT of the IR at each grid frequency).
        path.diffusePower = 0.0;
        for (int g = kGridFirst; g <= kGridLast; ++g)
        {
            const double f = gridFrequency (g);
            if (f >= 0.45 * spec.sampleRate)
                break;
            const std::complex<double> step = std::polar (1.0, -kTwoPi * f / spec.sampleRate);
            for (const auto& rev : path.reversed)
            {
                std::complex<double> h (0.0, 0.0), z (1.0, 0.0);
                for (int k = 0; k < length; ++k, z *= step)
                    h += static_cast<double> (rev[static_cast<size_t> (length - 1 - k)]) * z;
                path.diffusePower += pathWeight (f) * std::norm (h);
            }
        }
    }
    return true;
}

void HeadphoneVirtualizer::prepare (const ProcessSpec& newSpec)
{
    spec = newSpec;
    spec.numChannels = std::clamp (spec.numChannels, 1, kMaxChannels);
    spec.maxBlockSize = std::max (1, spec.maxBlockSize);
    // Non-finite or absurd rates would overflow the delay-line sizes (and hang
    // nextPowerOfTwo) or give an empty SVF frequency range.
    if (! (spec.sampleRate > 0.0) || ! std::isfinite (spec.sampleRate))
        spec.sampleRate = 48000.0;
    spec.sampleRate = std::clamp (spec.sampleRate, 8000.0, 768000.0);
    const double fs = spec.sampleRate;

    // ITD lines: the longest Woodworth delay (a = 105 mm, theta = 180 deg) is
    // (a/c)(1 + pi/2) = 787 us = 151 samples at 192 kHz. Room for the 4
    // Lagrange taps plus margin, rounded up to a power of two (256), so the
    // read index is a mask instead of a branch.
    const double maxTau = static_cast<double> (kMaxHeadMm) * 0.001 / kSpeedOfSound * (1.0 + 0.5 * kPi);
    const int maxDelay = static_cast<int> (std::ceil (maxTau * std::max (fs, 192000.0)));
    const int itdSize = nextPowerOfTwo (maxDelay + 8);
    itdMask = itdSize - 1;
    maxDelaySamples = static_cast<float> (itdSize - 5); // safety clamp only; never reached below 192 kHz
    for (auto& line : itdLines)
        line.assign (static_cast<size_t> (itdSize), 0.0f);
    itdWrite = 0;

    // Early reflections (integer taps: static delays need no interpolation).
    for (size_t k = 0; k < reflDelay.size(); ++k)
        reflDelay[k] = std::max (1, msToSamples (kReflectionMs[k], fs));
    const int reflSize = nextPowerOfTwo (*std::max_element (reflDelay.begin(), reflDelay.end()) + 1);
    reflMask = reflSize - 1;
    reflLine.assign (static_cast<size_t> (reflSize), 0.0f);
    reflWrite = 0;
    reflHpCoeffs = SvfCoeffs::make (FilterType::HighPass, kReflectionHpHz, butterworthQ (1, 0), 0.0, fs);
    reflLpCoeffs = SvfCoeffs::make (FilterType::LowPass, kReflectionLpHz, butterworthQ (1, 0), 0.0, fs);

    // LFE: 4th-order Butterworth = two SVF low-pass sections (LfeFold).
    lfe.prepare (fs, LfeFold::gainFor (params.lfeOn, params.lfeGainDb));

    const auto scratch = static_cast<size_t> (spec.maxBlockSize);
    for (auto* v : { &accL, &accR, &bus, &lfeBus, &refL, &refR })
        v->assign (scratch, 0.0f);
    for (auto& v : cueOut)
        v.assign (scratch, 0.0f);

    // Level match and fold headroom (E28a).
    // The servo's K-weighting runs on every other sample (renderSegment), so
    // it is designed for fs / 2; the diffuse-field grid uses the full rate.
    kShelf = SvfCoeffs::make (FilterType::HighShelf, kKShelfHz, kKShelfQ, kKShelfDb, 0.5 * fs);
    kHighPass = SvfCoeffs::make (FilterType::HighPass, kKHighPassHz, kKHighPassQ, 0.0, 0.5 * fs);
    averageCoeff = 1.0 - std::exp (-kControlInterval / (static_cast<double> (kMakeupAverageMs) * 0.001 * fs));
    makeupSlew = dbToGain (kMakeupSlewDbPerS * static_cast<float> (kControlInterval / fs));
    headroomHoldSamples = msToSamples (kHeadroomHoldMs, fs);
    headroomRelease = static_cast<float> (1.0 - std::exp (-1.0 / (static_cast<double> (kHeadroomReleaseMs) * 0.001 * fs)));
    servoSeeded = false;

    // Geometry smoothers step once per control period, so their coefficient
    // is computed for the control rate.
    const double controlRate = fs / kControlInterval;
    frontAngle.reset (controlRate, kGeometrySmoothingMs, params.frontAngleDeg);
    sideAngle.reset (controlRate, kGeometrySmoothingMs, params.sideAngleDeg);
    rearAngle.reset (controlRate, kGeometrySmoothingMs, params.rearAngleDeg);
    headRadius.reset (controlRate, kGeometrySmoothingMs, params.headRadiusMm);
    enhanced.reset (controlRate, kRendererSmoothingMs, params.renderer == VirtualizerRenderer::Enhanced ? 1.0f : 0.0f);
    contrast.reset (controlRate, kRendererSmoothingMs, params.frontBack / kNominalFrontBack);
    for (auto& sp : speakers)
        sp.shelfDb.reset (controlRate, kShelfSmoothingMs, 0.0f);
    roomGain.reset (fs, kGainRampMs, params.roomAmount);

    // The swap fade is a whole number of control periods, so a fade that
    // starts on a tick lands exactly on 0 / 1 on a later tick.
    const long periods = std::lround (static_cast<double> (kFadeMs) * 0.001 * fs / kControlInterval);
    fadeSamples = std::max (1, static_cast<int> (periods)) * kControlInterval;

    hrirValid = loadHrir();

    // Pre-roll after a swap, rounded up to whole control periods: at least the
    // longest ITD path plus the Lagrange taps (151 + 3 samples at 192 kHz, far
    // below 2 ms) and the shelf / shadow start-up. An HRIR needs its length
    // (capped: late taps are small, and the fade-in covers them).
    const auto periodsFor = [] (double samples)
    { return std::max (1, static_cast<int> (std::ceil (samples / kControlInterval))) * kControlInterval; };
    holdParametric = periodsFor (static_cast<double> (kPrerollMs) * 0.001 * fs);
    holdHrir = std::max (holdParametric, periodsFor (std::min (static_cast<double> (hrirLength), static_cast<double> (kMaxHrirPrerollMs) * 0.001 * fs)));

    reset();
}

void HeadphoneVirtualizer::reset() noexcept FLUB_NONBLOCKING
{
    // The level match's averages and make-up are kept (see the file comment);
    // its filters, its period and the fold headroom restart.
    for (auto* st : { &kShelf1, &kShelf2, &kHp1, &kHp2 })
        st->fill (0.0f);
    periodRef = periodBin = 0.0;
    servoPhase = 0;
    makeupFrom = makeupTo;
    headroomGain = 1.0f;
    headroomHold = 0;
    swapLayout();
    reflHpState.reset();
    reflLpState.reset();
    std::fill (reflLine.begin(), reflLine.end(), 0.0f);
    fadePos = fadeSamples;
    fadeDir = 0;
    holdRemaining = 0;
    lfe.reset();
    roomGain.setImmediate (roomGain.getTarget());
    samplesToTick = 0;
    rampPos = 0;
    ramping = false;
    busy = false;
}

//==============================================================================
void HeadphoneVirtualizer::setParams (const VirtualizerParams& p) noexcept FLUB_NONBLOCKING
{
    const VirtualizerParams s = sanitise (p);
    if (s == params)
        return; // the chain pushes parameters every block: unchanged is free

    params = s;
    frontAngle.setTarget (s.frontAngleDeg);
    sideAngle.setTarget (s.sideAngleDeg);
    rearAngle.setTarget (s.rearAngleDeg);
    headRadius.setTarget (s.headRadiusMm);
    enhanced.setTarget (s.renderer == VirtualizerRenderer::Enhanced ? 1.0f : 0.0f);
    contrast.setTarget (s.frontBack / kNominalFrontBack);
    lfe.setGain (LfeFold::gainFor (s.lfeOn, s.lfeGainDb));
    roomGain.setTarget (s.roomAmount);
    // Geometry glides and layout swaps run on the control ticks, which sit at
    // fixed positions in the stream.
    busy = true;
}

//==============================================================================
void HeadphoneVirtualizer::updateGeometry (bool snap) noexcept
{
    VirtualizerParams p = params;
    p.frontAngleDeg = frontAngle.getCurrent();
    p.sideAngleDeg = sideAngle.getCurrent();
    p.rearAngleDeg = rearAngle.getCurrent();

    const double fs = spec.sampleRate;
    const double radius = static_cast<double> (headRadius.getCurrent()) * 0.001;
    const double headDelay = radius / kSpeedOfSound * fs; // a/c in samples
    const double w0 = kSpeedOfSound / radius;

    for (int c = 0; c < kMaxChannels; ++c)
    {
        auto& sp = speakers[static_cast<size_t> (c)];
        if (sp.role != Role::Speaker)
            continue;

        const float az = speakerAzimuthDeg (runningLayout, c, p);
        sp.azimuth = az;
        // Angle between the source and each ear's axis (ears at -90 / +90
        // deg) and the Woodworth delays; Enhanced takes its share of the near
        // ear's delay off both (see the file comment).
        std::array<double, 2> thetaDeg {};
        std::array<float, 2> delays {};
        for (size_t e = 0; e < 2; ++e)
        {
            thetaDeg[e] = earAngleDeg (static_cast<double> (az), e);
            delays[e] = static_cast<float> (woodworth (thetaDeg[e] * (kPi / 180.0)) * headDelay);
        }
        const float lead = nearEarLead (delays, enhanced.getCurrent());
        for (size_t e = 0; e < 2; ++e)
        {
            auto& ear = sp.ears[e];
            ear.delay = std::clamp (delays[e] - lead, 0.0f, maxDelaySamples);
            lagrangeTaps (ear.delay, ear.base, ear.taps);
            ear.shadow = BiquadCoeffs::fromAnalogFirstOrder (1.0, shadowAlpha (thetaDeg[e]) / (2.0 * w0), 1.0, 1.0 / (2.0 * w0), fs);
            if (snap)
            {
                ear.prevDelay = ear.delay;
                ear.prevShadow = ear.shadow;
            }
        }

        const float shelfTarget = std::abs (az) > 90.0f ? kRearShelfDb : 0.0f;
        if (cuesActive)
            designCues (sp);
        else if (snap) // not run: exact pass-throughs, the start point of a later glide to Enhanced
            sp.cue = directionCues (static_cast<double> (az), 0.0, static_cast<double> (contrast.getCurrent()), fs);
        if (snap)
        {
            sp.shelfDb.setImmediate (shelfTarget);
            sp.shelf = SvfCoeffs::make (FilterType::HighShelf, kRearShelfHz, kRearShelfQ,
                                        static_cast<double> (shelfTarget * (1.0f - enhanced.getCurrent())), fs);
            sp.prevShelf = sp.shelf;
            sp.prevCue = sp.cue;
        }
        else
        {
            // Crossing 90 deg is a discrete change of the cue: the shelf gain
            // glides (10 ms) instead of stepping.
            sp.shelfDb.setTarget (shelfTarget);
        }
    }
}

void HeadphoneVirtualizer::designCues (Speaker& sp) noexcept
{
    // Classic (share 0 and settled): the sections are not run; skip the design.
    if (! cuesActive)
        return;
    sp.cue = directionCues (static_cast<double> (sp.azimuth), static_cast<double> (enhanced.getCurrent()),
                            static_cast<double> (contrast.getCurrent()), spec.sampleRate);
}

void HeadphoneVirtualizer::clearChannel (int channel) noexcept
{
    auto& sp = speakers[static_cast<size_t> (channel)];
    sp.shelfState.reset();
    for (auto& st : sp.cueState)
        st.reset();
    for (auto& ear : sp.ears)
        ear.state.reset();
    auto& line = itdLines[static_cast<size_t> (channel)];
    std::fill (line.begin(), line.end(), 0.0f);
    auto& path = hrirPaths[static_cast<size_t> (channel)];
    std::fill (path.history.begin(), path.history.end(), 0.0f);
    if (sp.role == Role::Lfe)
        lfe.clearState();
    sp.clean = true;
}

void HeadphoneVirtualizer::clearState() noexcept
{
    // Per-channel paths and the LFE only. The reflection line is fed by the
    // mono speaker sum and stays continuous across a swap (reset() clears it).
    for (int c = 0; c < kMaxChannels; ++c)
        clearChannel (c);
    lfe.clearState();
}

void HeadphoneVirtualizer::swapLayout() noexcept
{
    // Only called while the output is silent (swap fade at 0) or from reset(),
    // so everything may jump: new roles and renderer, geometry at its targets,
    // clean per-channel state. A silent pre-roll then lets the new paths fill
    // before the fade-in, which hides the rest of the filters' start-up.
    const bool layoutChanged = params.layout != runningLayout;
    runningLayout = params.layout;
    const bool withLfe = hasLfe (runningLayout);
    const int numLayoutChannels = channelCount (runningLayout);
    for (int c = 0; c < kMaxChannels; ++c)
    {
        auto& sp = speakers[static_cast<size_t> (c)];
        sp.role = c >= numLayoutChannels ? Role::None : (withLfe && c == 3 ? Role::Lfe : Role::Speaker);
    }
    useHrir = hrirValid && hrirLayout == runningLayout;

    frontAngle.setImmediate (frontAngle.getTarget());
    sideAngle.setImmediate (sideAngle.getTarget());
    rearAngle.setImmediate (rearAngle.getTarget());
    headRadius.setImmediate (headRadius.getTarget());
    enhanced.setImmediate (enhanced.getTarget());
    contrast.setImmediate (contrast.getTarget());
    cuesActive = enhanced.getCurrent() > 0.0f;
    updateGeometry (true);
    clearState();
    fadeDir = 0;
    holdRemaining = useHrir ? holdHrir : holdParametric;

    // Level match: the new design's diffuse-field gain. A new layout (or the
    // first swap after prepare()) starts the averages again at it; the
    // output is silent here, so the make-up may jump.
    diffuseGain = diffuseGainFor();
    if (layoutChanged || ! servoSeeded)
    {
        avgRef = avgBin = 0.0;
        learned = false;
        makeupFrom = makeupTo = params.levelMatch ? diffuseGain : 1.0f;
        servoSeeded = true;
    }
}

double HeadphoneVirtualizer::pathWeight (double freqHz) const noexcept
{
    // |K|^2 at the session rate (the servo's copy runs at fs / 2).
    const double fs = spec.sampleRate;
    const auto shelf = SvfCoeffs::make (FilterType::HighShelf, kKShelfHz, kKShelfQ, kKShelfDb, fs);
    const auto highPass = SvfCoeffs::make (FilterType::HighPass, kKHighPassHz, kKHighPassQ, 0.0, fs);
    return std::norm (shelf.response (freqHz, fs)) * std::norm (highPass.response (freqHz, fs));
}

float HeadphoneVirtualizer::diffuseGainFor() const noexcept
{
    // Uncorrelated pink noise of equal power on every speaker: the K-weighted
    // power of D over both channels against that of the ear sums (see the
    // file comment). Runs at a layout swap (a few hundred filter responses).
    const double fs = spec.sampleRate;
    double ref = 0.0, ears = 0.0;
    int numSpeakers = 0;
    for (int c = 0; c < kMaxChannels; ++c)
    {
        if (speakers[static_cast<size_t> (c)].role != Role::Speaker)
            continue;
        ++numSpeakers;
        if (useHrir && hrirPaths[static_cast<size_t> (c)].present)
            ears += hrirPaths[static_cast<size_t> (c)].diffusePower;
    }
    const double room = static_cast<double> (roomGain.getTarget());
    for (int g = kGridFirst; g <= kGridLast; ++g)
    {
        const double f = gridFrequency (g);
        if (f >= 0.45 * fs)
            break;
        const double w = pathWeight (f);
        for (int c = 0; c < kMaxChannels; ++c)
        {
            const auto& sp = speakers[static_cast<size_t> (c)];
            if (sp.role != Role::Speaker)
                continue;
            const auto d = downmixWeights (c);
            ref += w * static_cast<double> (d[0] * d[0] + d[1] * d[1]);
            if (! useHrir)
            {
                double shelf = std::norm (sp.shelf.response (f, fs));
                if (cuesActive)
                    for (const auto& cue : sp.cue)
                        shelf *= std::norm (cue.response (f, fs));
                for (const auto& ear : sp.ears)
                    ears += w * shelf * std::norm (ear.shadow.response (f, fs));
            }
        }
        // Reflections: the bus carries every speaker (power numSpeakers), each
        // ear three taps of summed squared weight 0.5.
        const double band = std::norm (reflHpCoeffs.response (f, fs) * reflLpCoeffs.response (f, fs));
        ears += w * room * room * numSpeakers * band;
    }
    if (! (ref > 0.0) || ! (ears > 0.0) || ! std::isfinite (ref / ears))
        return 1.0f;
    return static_cast<float> (std::sqrt (ref / ears));
}

void HeadphoneVirtualizer::tick() noexcept
{
    // A coefficient ramp spans exactly one control period, which ends here:
    // the current design becomes the start point of any new ramp.
    for (auto& sp : speakers)
    {
        sp.prevShelf = sp.shelf;
        sp.prevCue = sp.cue;
        for (auto& ear : sp.ears)
        {
            ear.prevDelay = ear.delay;
            ear.prevShadow = ear.shadow;
        }
    }
    ramping = false;
    rampPos = 0;

    // 1. Layout: a discrete change (channel meaning, renderer). Fade out, swap
    //    at silence, pre-roll silently, fade back in. Returning to the running
    //    layout while the fade-out is still going simply fades back in; a new
    //    change during the pre-roll swaps again at once (still silent).
    if (params.layout != runningLayout)
    {
        if (fadePos == 0)
            swapLayout();
        else
            fadeDir = -1;
    }
    else if (holdRemaining > 0)
    {
        fadeDir = 0;
    }
    else
    {
        fadeDir = fadePos < fadeSamples ? 1 : 0;
    }

    // 2. The renderer's direction cues (E28): glide the Enhanced share and
    //    the band scale. In Classic with nothing to glide the scale just
    //    follows its target (the cues are not run), so a frontBack change
    //    leaves the Classic render untouched.
    const bool shareWasOn = cuesActive;
    if (! enhanced.isSmoothing() && enhanced.getCurrent() == 0.0f)
        contrast.setImmediate (contrast.getTarget());
    const bool cuesMoving = enhanced.isSmoothing() || contrast.isSmoothing();
    if (cuesMoving)
    {
        enhanced.next();
        contrast.next();
    }
    // The cues run while the share is above 0, and for the one period that
    // ramps them down to the pass-through.
    cuesActive = enhanced.getCurrent() > 0.0f || enhanced.isSmoothing() || (shareWasOn && cuesMoving);

    // 3. Continuous geometry: glide angles and head radius, redesign delays,
    //    head-shadow filters and cues, ramp to them across the coming control
    //    period.
    const bool geometryMoving = frontAngle.isSmoothing() || sideAngle.isSmoothing() || rearAngle.isSmoothing() || headRadius.isSmoothing();
    if (geometryMoving)
    {
        frontAngle.next();
        sideAngle.next();
        rearAngle.next();
        headRadius.next();
        updateGeometry (false);
        ramping = true;
    }
    else if (cuesMoving)
    {
        // The cues and, as the share moves, Enhanced's near-ear reference of
        // the delays (the geometry itself is where it was).
        updateGeometry (false);
        ramping = true;
    }

    // 4. Rear-cue shelves (targets set by updateGeometry(); the gain is the
    //    Classic share of it).
    bool shelvesMoving = false;
    for (auto& sp : speakers)
    {
        if (sp.role != Role::Speaker || ! (sp.shelfDb.isSmoothing() || cuesMoving))
            continue;
        const float db = sp.shelfDb.isSmoothing() ? sp.shelfDb.next() : sp.shelfDb.getCurrent();
        sp.shelf = SvfCoeffs::make (FilterType::HighShelf, kRearShelfHz, kRearShelfQ, static_cast<double> (db * (1.0f - enhanced.getCurrent())),
                                    spec.sampleRate);
        ramping = true;
        shelvesMoving = shelvesMoving || sp.shelfDb.isSmoothing();
    }

    if (cuesMoving && ! enhanced.isSmoothing() && ! contrast.isSmoothing())
    {
        // The glide has landed: the level match starts from the new design's
        // diffuse-field gain (the make-up still moves at most 6 dB/s).
        diffuseGain = diffuseGainFor();
    }
    if (shareWasOn && ! cuesActive)
        for (auto& sp : speakers)
            for (auto& st : sp.cueState)
                st.reset(); // no longer run (the last ramp reached the pass-through); start clean next time

    busy = ramping || shelvesMoving || fadeDir != 0 || holdRemaining > 0 || params.layout != runningLayout || frontAngle.isSmoothing()
           || sideAngle.isSmoothing() || rearAngle.isSmoothing() || headRadius.isSmoothing() || enhanced.isSmoothing()
           || contrast.isSmoothing();
}

//==============================================================================
template <bool Ramp>
void HeadphoneVirtualizer::renderParametric (Speaker& sp, float* line, const float* x, int length) noexcept
{
    // One fused per-sample loop: the ITD line (256 samples) is shorter than a
    // block, so each sample must be read back before the line wraps.
    float* const outL = accL.data();
    float* const outR = accR.data();
    const int mask = itdMask;
    const int write0 = itdWrite;
    EarPath& el = sp.ears[0];
    EarPath& er = sp.ears[1];

    // Local copies keep coefficients and state in registers (the output
    // pointers could otherwise alias them).
    SvfCoeffs shelf = sp.shelf;
    SvfState shelfState = sp.shelfState;
    BiquadCoeffs cl = el.shadow, cr = er.shadow;
    BiquadState sl = el.state, sr = er.state;
    std::array<float, 4> hl = el.taps, hr = er.taps;
    int bl = el.base, br = er.base;

    for (int i = 0; i < length; ++i)
    {
        if constexpr (Ramp)
        {
            // Position inside the control period -> exact end point at 16.
            const float t = static_cast<float> (std::min (rampPos + i + 1, kControlInterval)) * (1.0f / kControlInterval);
            shelf = mixSvf (sp.prevShelf, sp.shelf, t);
            lagrangeTaps ((1.0f - t) * el.prevDelay + t * el.delay, bl, hl);
            lagrangeTaps ((1.0f - t) * er.prevDelay + t * er.delay, br, hr);
            cl = mixFirstOrder (el.prevShadow, el.shadow, static_cast<double> (t));
            cr = mixFirstOrder (er.prevShadow, er.shadow, static_cast<double> (t));
        }

        const int w = write0 + i;
        line[w & mask] = svfTick (shelf, shelfState, x[i]);
        outL[i] += static_cast<float> (biquadTick (cl, sl, static_cast<double> (readLagrange (line, w - bl, mask, hl))));
        outR[i] += static_cast<float> (biquadTick (cr, sr, static_cast<double> (readLagrange (line, w - br, mask, hr))));
    }

    storeState (sp.shelfState, shelfState);
    el.state.z1 = flushed (sl.z1);
    er.state.z1 = flushed (sr.z1);
}

void HeadphoneVirtualizer::renderCues (const AudioBlock& block, int start, int length, int numInputs) noexcept
{
    // The six cue sections of up to four speakers run side by side on the
    // four lanes of Lane4 (the arithmetic of svfTick per lane); while
    // ramping, each lane's coefficients are interpolated per sample as
    // mixSvf does. Speaker channels missing from the block are skipped (the
    // main loop does not render them); an unused lane runs on silence with
    // pass-through coefficients and is not stored.
    std::array<int, kMaxChannels> channels {};
    int count = 0;
    for (int c = 0; c < std::min (numInputs, kMaxChannels); ++c)
        if (speakers[static_cast<size_t> (c)].role == Role::Speaker)
            channels[static_cast<size_t> (count++)] = c;

    const SvfCoeffs identity = SvfCoeffs::make (FilterType::Bell, 1000.0, 1.0, 0.0, spec.sampleRate);
    const SvfState silent;
    for (int g = 0; g < count; g += 4)
    {
        std::array<Speaker*, 4> sp {};
        std::array<const float*, 4> in {};
        std::array<float*, 4> out {};
        for (size_t l = 0; l < 4; ++l)
        {
            const int k = g + static_cast<int> (l);
            if (k < count)
            {
                const int c = channels[static_cast<size_t> (k)];
                sp[l] = &speakers[static_cast<size_t> (c)];
                in[l] = block.channel (c) + start;
                out[l] = cueOut[static_cast<size_t> (c)].data();
            }
        }
        const auto coeff = [&] (size_t l, size_t k, bool prev) -> const SvfCoeffs&
        { return sp[l] == nullptr ? identity : (prev ? sp[l]->prevCue[k] : sp[l]->cue[k]); };
        const auto lanes = [&] (size_t k, bool prev, float SvfCoeffs::*m)
        { return lane4 (coeff (0, k, prev).*m, coeff (1, k, prev).*m, coeff (2, k, prev).*m, coeff (3, k, prev).*m); };

        struct Section
        {
            Lane4 a1, a2, a3, m0, m1, m2;
        };
        std::array<Section, kNumCues> cur {}, prv {};
        std::array<Lane4, kNumCues> ic1 {}, ic2 {};
        for (size_t k = 0; k < cur.size(); ++k)
        {
            for (auto* set : { &cur, &prv })
            {
                const bool prev = set == &prv;
                (*set)[k] = { lanes (k, prev, &SvfCoeffs::a1), lanes (k, prev, &SvfCoeffs::a2), lanes (k, prev, &SvfCoeffs::a3),
                              lanes (k, prev, &SvfCoeffs::m0), lanes (k, prev, &SvfCoeffs::m1), lanes (k, prev, &SvfCoeffs::m2) };
            }
            const auto& s0 = sp[0] != nullptr ? sp[0]->cueState[k] : silent;
            const auto& s1 = sp[1] != nullptr ? sp[1]->cueState[k] : silent;
            const auto& s2 = sp[2] != nullptr ? sp[2]->cueState[k] : silent;
            const auto& s3 = sp[3] != nullptr ? sp[3]->cueState[k] : silent;
            ic1[k] = lane4 (s0.ic1, s1.ic1, s2.ic1, s3.ic1);
            ic2[k] = lane4 (s0.ic2, s1.ic2, s2.ic2, s3.ic2);
        }

        std::array<float, 4> y {};
        std::array<Section, kNumCues> run = cur;
        for (int i = 0; i < length; ++i)
        {
            if (ramping)
            {
                const float t = static_cast<float> (std::min (rampPos + i + 1, kControlInterval)) * (1.0f / kControlInterval);
                const Lane4 lt = splat (t), lu = splat (1.0f - t);
                for (size_t k = 0; k < cur.size(); ++k)
                {
                    const auto& a = prv[k];
                    const auto& b = cur[k];
                    run[k] = { lu * a.a1 + lt * b.a1, lu * a.a2 + lt * b.a2, lu * a.a3 + lt * b.a3,
                               lu * a.m0 + lt * b.m0, lu * a.m1 + lt * b.m1, lu * a.m2 + lt * b.m2 };
                }
            }
            Lane4 v = lane4 (in[0] != nullptr ? in[0][i] : 0.0f, in[1] != nullptr ? in[1][i] : 0.0f, in[2] != nullptr ? in[2][i] : 0.0f,
                             in[3] != nullptr ? in[3][i] : 0.0f);
            for (size_t k = 0; k < cur.size(); ++k)
            {
                const auto& c = run[k];
                const Lane4 v3 = v - ic2[k];
                const Lane4 v1 = c.a1 * ic1[k] + c.a2 * v3;
                const Lane4 v2 = ic2[k] + c.a2 * ic1[k] + c.a3 * v3;
                ic1[k] = (v1 + v1) - ic1[k];
                ic2[k] = (v2 + v2) - ic2[k];
                v = c.m0 * v + c.m1 * v1 + c.m2 * v2;
            }
            store (v, y);
            for (size_t l = 0; l < 4; ++l)
                if (out[l] != nullptr)
                    out[l][i] = y[l];
        }

        std::array<float, 4> s1 {}, s2 {};
        for (size_t k = 0; k < cur.size(); ++k)
        {
            store (ic1[k], s1);
            store (ic2[k], s2);
            for (size_t l = 0; l < 4; ++l)
                if (sp[l] != nullptr)
                {
                    sp[l]->cueState[k].ic1 = flushed (s1[l]);
                    sp[l]->cueState[k].ic2 = flushed (s2[l]);
                }
        }
    }
}

void HeadphoneVirtualizer::renderHrir (HrirPath& path, const float* x, int length) noexcept
{
    // Direct-form convolution. history[p + 1 .. p + n] always holds the newest
    // n inputs oldest-first (every sample is written twice, n apart), so each
    // output is one contiguous dot product with the time-reversed HRIR.
    const int n = hrirLength;
    float* const hist = path.history.data();
    const float* const hl = path.reversed[0].data();
    const float* const hr = path.reversed[1].data();
    float* const outL = accL.data();
    float* const outR = accR.data();

    int p = hrirWrite;
    for (int i = 0; i < length; ++i)
    {
        hist[p] = x[i];
        hist[p + n] = x[i];
        float yl = 0.0f, yr = 0.0f;
        dotPair (hist + p + 1, hl, hr, n, yl, yr);
        outL[i] += yl;
        outR[i] += yr;
        if (++p == n)
            p = 0;
    }
}

void HeadphoneVirtualizer::renderLfe (const float* x, int length) noexcept
{
    lfe.addToMono (x, lfeBus.data(), length); // the same in both ears
}

void HeadphoneVirtualizer::updateMakeup() noexcept
{
    // End of a 16-sample period (stream time): fold its energies into the
    // averages, then aim the next period's ramp at the new make-up.
    if (params.levelMatch && periodRef >= kMakeupGate * kControlInterval && std::isfinite (periodRef) && std::isfinite (periodBin))
    {
        avgRef += averageCoeff * (periodRef - avgRef);
        avgBin += averageCoeff * (periodBin - avgBin);
        learned = true;
    }
    periodRef = periodBin = 0.0;

    float target = 1.0f;
    if (params.levelMatch)
    {
        target = diffuseGain;
        if (learned && avgBin > 1.0e-30)
        {
            const float range = dbToGain (kMakeupRangeDb);
            target = std::clamp (static_cast<float> (std::sqrt (avgRef / avgBin)), diffuseGain / range, diffuseGain * range);
        }
    }
    makeupFrom = makeupTo;
    makeupTo = std::clamp (target, makeupFrom / makeupSlew, makeupFrom * makeupSlew);
}

void HeadphoneVirtualizer::renderReflections (int length) noexcept
{
    const SvfCoeffs hp = reflHpCoeffs, lp = reflLpCoeffs;
    SvfState hs = reflHpState, ls = reflLpState;
    const float* const src = bus.data();
    float* const line = reflLine.data();
    float* const outL = accL.data();
    float* const outR = accR.data();
    const int mask = reflMask;
    int w = reflWrite;

    // The line is always fed (cheap) so raising the room level later starts
    // from real history rather than stale samples.
    if (! roomGain.isSmoothing() && roomGain.getCurrent() == 0.0f)
    {
        for (int i = 0; i < length; ++i, w = (w + 1) & mask)
            line[w] = svfTick (lp, ls, svfTick (hp, hs, src[i]));
    }
    else
    {
        const std::array<int, kNumReflections> d = reflDelay;
        constexpr auto g = kReflectionWeight;
        for (int i = 0; i < length; ++i, w = (w + 1) & mask)
        {
            line[w] = svfTick (lp, ls, svfTick (hp, hs, src[i]));
            const float room = roomGain.next();
            const float yl = g[0] * line[(w - d[0]) & mask] + g[2] * line[(w - d[2]) & mask] + g[4] * line[(w - d[4]) & mask];
            const float yr = g[1] * line[(w - d[1]) & mask] + g[3] * line[(w - d[3]) & mask] + g[5] * line[(w - d[5]) & mask];
            outL[i] += room * yl;
            outR[i] += room * yr;
        }
    }

    reflWrite = w;
    storeState (reflHpState, hs);
    storeState (reflLpState, ls);
}

void HeadphoneVirtualizer::renderSegment (const AudioBlock& block, int start, int length, int numInputs) noexcept
{
    float* const l = accL.data();
    float* const r = accR.data();
    float* const sum = bus.data();
    float* const lfeOut = lfeBus.data();
    float* const dl = refL.data();
    float* const dr = refR.data();
    std::fill_n (l, length, 0.0f);
    std::fill_n (r, length, 0.0f);
    std::fill_n (sum, length, 0.0f);
    std::fill_n (lfeOut, length, 0.0f);
    // The level match measures D while it is on (off, the make-up only
    // glides to unity).
    const bool measure = params.levelMatch;
    if (measure)
    {
        std::fill_n (dl, length, 0.0f);
        std::fill_n (dr, length, 0.0f);
    }

    // Enhanced's direction cues, four speakers at a time, into cueOut.
    if (cuesActive && ! useHrir)
        renderCues (block, start, length, numInputs);

    // Every input is read (into the accumulators) before any output is
    // written, so in-place processing is safe although ch 0/1 are both
    // speaker inputs and ear outputs.
    bool lfeRendered = false;
    for (int c = 0; c < kMaxChannels; ++c)
    {
        auto& sp = speakers[static_cast<size_t> (c)];
        if (sp.role == Role::None)
            continue;
        if (c >= numInputs)
        {
            // Channel missing from this block: silence. Clear its history once
            // so it restarts cleanly if it comes back.
            if (! sp.clean)
                clearChannel (c);
            continue;
        }

        sp.clean = false;
        const float* x = block.channel (c) + start;
        if (sp.role == Role::Lfe)
        {
            renderLfe (x, length);
            lfeRendered = true;
            continue;
        }

        for (int i = 0; i < length; ++i)
            sum[i] += x[i];
        if (measure)
        {
            const auto d = downmixWeights (c);
            for (size_t side = 0; side < 2; ++side)
            {
                float* const ref = side == 0 ? dl : dr;
                if (d[side] != 0.0f) // only the centre feeds both sides
                    for (int i = 0; i < length; ++i)
                        ref[i] += d[side] * x[i];
            }
        }

        if (useHrir)
        {
            if (hrirPaths[static_cast<size_t> (c)].present)
                renderHrir (hrirPaths[static_cast<size_t> (c)], x, length);
        }
        else
        {
            // Enhanced: the speaker path starts from the cue-filtered input.
            const float* xs = cuesActive ? cueOut[static_cast<size_t> (c)].data() : x;
            if (ramping)
                renderParametric<true> (sp, itdLines[static_cast<size_t> (c)].data(), xs, length);
            else
                renderParametric<false> (sp, itdLines[static_cast<size_t> (c)].data(), xs, length);
        }
    }
    if (! lfeRendered)
        lfe.skip (length); // keep the ramp in stream time

    renderReflections (length);

    itdWrite = (itdWrite + length) & itdMask;
    if (hrirLength > 0)
        hrirWrite = (hrirWrite + length) % hrirLength;

    // Output: (make-up * speakers + LFE) * trim * fold headroom * swap fade,
    // binaural in ch 0/1, the rest cleared. Per sample, so the make-up ramp
    // and the headroom gain are the same for any block partition.
    const bool fading = fadeDir != 0 || fadePos != fadeSamples;
    const float invFade = 1.0f / static_cast<float> (fadeSamples);
    const float invPeriod = 1.0f / static_cast<float> (kControlInterval);
    const bool limit = params.foldHeadroom;
    float* const outL = block.channel (0) + start;
    float* const outR = numInputs >= 2 ? block.channel (1) + start : nullptr;
    if (measure)
    {
        // K-weighted power, in place: dl = D (both channels), dr = the ear
        // sums before the make-up. Measured on every other sample (even
        // stream positions; the others read 0): the mean square of the
        // subsampled signal is that of the signal, and at fs / 2 the
        // K-weighting still covers the band where it differs (see prepare()).
        // Half the cost; the servo only needs the ratio of two powers.
        const SvfLanes ks (kShelf), kh (kHighPass);
        Lane4 s1 = lane4 (kShelf1), s2 = lane4 (kShelf2), h1 = lane4 (kHp1), h2 = lane4 (kHp2);
        std::array<float, 4> y {};
        const int first = servoPhase & 1; // first even stream position
        for (int i = first; i < length; i += 2)
        {
            const Lane4 k = svfLanes (kh, h1, h2, svfLanes (ks, s1, s2, lane4 (dl[i], dr[i], l[i], r[i])));
            store (k * k, y);
            dl[i] = y[0] + y[1];
            dr[i] = y[2] + y[3];
        }
        for (int i = 1 - first; i < length; i += 2)
            dl[i] = dr[i] = 0.0f;
        store (s1, kShelf1);
        store (s2, kShelf2);
        store (h1, kHp1);
        store (h2, kHp2);
        for (auto* st : { &kShelf1, &kShelf2, &kHp1, &kHp2 })
            for (auto& x : *st)
                x = flushed (x);
    }
    double eRef = periodRef, eBin = periodBin;
    for (int i = 0; i < length; ++i)
    {
        if (measure)
        {
            eRef += static_cast<double> (dl[i]);
            eBin += static_cast<double> (dr[i]);
        }

        const int phase = ++servoPhase;
        const float m = makeupFrom + (makeupTo - makeupFrom) * (static_cast<float> (phase) * invPeriod);
        float yl = kTrim * (m * l[i] + lfeOut[i]);
        float yr = kTrim * (m * r[i] + lfeOut[i]);

        // Fold headroom: hold, then release; an over drops the gain at once
        // to exactly the ceiling (and restarts the hold).
        if (headroomHold > 0)
        {
            --headroomHold;
        }
        else if (headroomGain < 1.0f)
        {
            headroomGain += (1.0f - headroomGain) * headroomRelease;
            if (headroomGain > 0.999999f)
                headroomGain = 1.0f;
        }
        const float peak = std::max (std::abs (yl), std::abs (yr));
        if (limit && peak * headroomGain > kHeadroomCeiling && std::isfinite (peak))
        {
            headroomGain = kHeadroomCeiling / peak;
            headroomHold = headroomHoldSamples;
        }
        float g = headroomGain;
        if (fading)
        {
            // Integer position -> exact 0 / 1 end points.
            const int pos = std::clamp (fadePos + fadeDir * (i + 1), 0, fadeSamples);
            g *= static_cast<float> (pos) * invFade;
        }
        yl *= g;
        yr *= g;

        if (outR != nullptr)
        {
            outL[i] = yl;
            outR[i] = yr;
        }
        else
        {
            outL[i] = 0.5f * (yl + yr); // mono host bus: the mono fold-down of the pair
        }

        if (phase == kControlInterval)
        {
            periodRef = eRef;
            periodBin = eBin;
            updateMakeup();
            eRef = eBin = 0.0;
            servoPhase = 0;
        }
    }
    periodRef = eRef;
    periodBin = eBin;

    for (int c = 2; c < numInputs; ++c)
        std::fill_n (block.channel (c) + start, length, 0.0f);
}

void HeadphoneVirtualizer::process (const AudioBlock& block) noexcept FLUB_NONBLOCKING
{
    const int numSamples = block.numSamples;
    const int numInputs = std::min (block.numChannels, kMaxChannels);
    const int scratch = static_cast<int> (accL.size());
    if (numSamples <= 0 || numInputs <= 0 || scratch == 0)
        return;

    int pos = 0;
    while (pos < numSamples)
    {
        if (samplesToTick == 0)
        {
            if (busy)
                tick();
            samplesToTick = kControlInterval;
        }

        // While nothing is busy every tick would be a no-op, so the rest of the
        // block is one segment (identical output, fewer loop set-ups).
        const int len = std::min (busy ? std::min (samplesToTick, numSamples - pos) : numSamples - pos, scratch);
        renderSegment (block, pos, len, numInputs);

        if (ramping)
            rampPos = std::min (rampPos + len, kControlInterval);
        if (fadeDir != 0)
            fadePos = std::clamp (fadePos + fadeDir * len, 0, fadeSamples);
        else if (holdRemaining > 0)
            holdRemaining = std::max (0, holdRemaining - len);

        // Keep the tick grid aligned to absolute stream time.
        samplesToTick = ((samplesToTick - len) % kControlInterval + kControlInterval) % kControlInterval;
        pos += len;
    }
}
} // namespace flub
