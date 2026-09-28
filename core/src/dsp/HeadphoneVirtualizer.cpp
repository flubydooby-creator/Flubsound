// Flubsound Pro - multichannel (5.1 / 7.1) to binaural headphone virtualiser.
//
// Signal flow (every speaker path adds into two ear accumulators; the output
// is (make-up * speakers + LFE) * -3 dB trim * fold headroom * swap fade):
//
//   speaker (parametric):  x -> rear-cue shelf -> ITD line -+-> Lagrange(D_L) -> shadow_L -> ear L
//                                                           +-> Lagrange(D_R) -> shadow_R -> ear R
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
// reports zero latency (no look-ahead, no block delay).
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
        for (size_t e = 0; e < 2; ++e)
        {
            // Angle between the source and this ear's axis (ears at -90 / +90 deg).
            const double earAz = e == 0 ? -90.0 : 90.0;
            const double thetaDeg = std::abs (std::remainder (static_cast<double> (az) - earAz, 360.0));
            auto& ear = sp.ears[e];
            ear.delay = std::clamp (static_cast<float> (woodworth (thetaDeg * (kPi / 180.0)) * headDelay), 0.0f, maxDelaySamples);
            lagrangeTaps (ear.delay, ear.base, ear.taps);
            ear.shadow = BiquadCoeffs::fromAnalogFirstOrder (1.0, shadowAlpha (thetaDeg) / (2.0 * w0), 1.0, 1.0 / (2.0 * w0), fs);
            if (snap)
            {
                ear.prevDelay = ear.delay;
                ear.prevShadow = ear.shadow;
            }
        }

        const float shelfTarget = std::abs (az) > 90.0f ? kRearShelfDb : 0.0f;
        if (snap)
        {
            sp.shelfDb.setImmediate (shelfTarget);
            sp.shelf = SvfCoeffs::make (FilterType::HighShelf, kRearShelfHz, kRearShelfQ, static_cast<double> (shelfTarget), fs);
            sp.prevShelf = sp.shelf;
        }
        else
        {
            // Crossing 90 deg is a discrete change of the cue: the shelf gain
            // glides (10 ms) instead of stepping.
            sp.shelfDb.setTarget (shelfTarget);
        }
    }
}

void HeadphoneVirtualizer::clearChannel (int channel) noexcept
{
    auto& sp = speakers[static_cast<size_t> (channel)];
    sp.shelfState.reset();
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
                const double shelf = std::norm (sp.shelf.response (f, fs));
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

    // 2. Continuous geometry: glide angles and head radius, redesign delays and
    //    head-shadow filters, ramp to them across the coming control period.
    if (frontAngle.isSmoothing() || sideAngle.isSmoothing() || rearAngle.isSmoothing() || headRadius.isSmoothing())
    {
        frontAngle.next();
        sideAngle.next();
        rearAngle.next();
        headRadius.next();
        updateGeometry (false);
        ramping = true;
    }

    // 3. Rear-cue shelves (targets set by updateGeometry()).
    bool shelvesMoving = false;
    for (auto& sp : speakers)
    {
        if (sp.role != Role::Speaker || ! sp.shelfDb.isSmoothing())
            continue;
        const float db = sp.shelfDb.next();
        sp.shelf = SvfCoeffs::make (FilterType::HighShelf, kRearShelfHz, kRearShelfQ, static_cast<double> (db), spec.sampleRate);
        ramping = true;
        shelvesMoving = shelvesMoving || sp.shelfDb.isSmoothing();
    }

    busy = ramping || shelvesMoving || fadeDir != 0 || holdRemaining > 0 || params.layout != runningLayout || frontAngle.isSmoothing()
           || sideAngle.isSmoothing() || rearAngle.isSmoothing() || headRadius.isSmoothing();
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
        else if (ramping)
        {
            renderParametric<true> (sp, itdLines[static_cast<size_t> (c)].data(), x, length);
        }
        else
        {
            renderParametric<false> (sp, itdLines[static_cast<size_t> (c)].data(), x, length);
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
