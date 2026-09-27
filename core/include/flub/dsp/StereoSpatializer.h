// Flubsound Pro - mono-compatible stereo widener / spatializer.
//
// Width, focus and space operate on the SIDE signal only:
//   M = (L + R) / 2,  S = (L - R) / 2,  L' = M + S',  R' = M - S'
// so the mono fold-down L' + R' = 2M is preserved EXACTLY for them. That is
// the core mono-compatibility guarantee (tested), and it also means no comb
// filtering on mono playback, laptop speakers, or phone Bluetooth. The one
// exception is the bs2b / Meier headphone crossfeed (below): a real
// crossfeed delays one ear against the other, which no M / S-only
// processor can do, so it trades the exact mono sum for a real interaural
// delay. Its Mono-safe type keeps the exact guarantee.
//
//   width     : S' = S_low * min(width, 1) + S_high * width, split at
//               widthLowCutHz so the low end never gets wider. The split is
//               complementary (S_low + S_high = S, a 2nd-order Q 0.707 shelf
//               transition with LR4-like magnitude): an LR4 pair would sum to
//               an all-pass that turns S by 180 degrees against M at the cut.
//               Width polarity guard (docs/11 E12): the added side signal
//               S_high * (width - 1) is applied only in the share that keeps
//               S' below M (band envelopes above the low cut), so widening
//               never writes an anti-phase copy into the far ear: a
//               hard-panned source stays hard-panned at any width.
//   positionalFocus (gaming): +0..3 dB bell on S at 3 kHz (Q 0.5, ~1-6 kHz).
//               Interaural level differences in this region are the main
//               lateral localisation cue for broadband transients
//               (footsteps, reloads); emphasising S there sharpens the
//               perceived direction without touching the centre (M). At
//               100 % a source 6 dB to one side gains about 2.9 dB of ILD
//               at 3 kHz. Off at sample rates <= 32 kHz (Bluetooth
//               hands-free / speech links, mono and narrowband).
//   space     : S += space * 0.5 * D(z^-10ms W(M)), W = HP 300 Hz and a
//               presence dip (-7 dB at 2 kHz, Q 0.4: -5..-7 dB over
//               1-4 kHz), D = 3 nested Schroeder all-passes (3.1/4.7/7.3 ms,
//               g = 0.5): decorrelated ambience derived from the centre. The
//               10 ms pre-delay keeps the all-pass direct tap (-g x) from
//               panning the centre and bounds the ILD it gives a centred
//               source to < 1 dB per 1/3 octave. In mono it cancels (lives
//               in S).
//   crossfeed : headphone crossfeed, 0 .. 1, of the type crossfeedType:
//               Bs2b / Meier (default Bs2b): energy-preserving L/R
//               crossfeed. Each ear receives the other channel through a
//               first-order head-shadow low-pass (bs2b 700 Hz, Meier 650 Hz)
//               delayed by 0.235 ms (3rd-order Lagrange; with the low-pass
//               the cross-correlation ITD is 0.27 ms at 44.1 / 48 kHz, about
//               the 0.26 ms of a speaker at +-30 degrees), and its own
//               channel through the complementary near-ear shelf, so
//               |near|^2 + |far|^2 = 1 at every frequency. At crossfeed 1
//               the far ear is 4.5 dB (bs2b) / 9.5 dB (Meier) below the
//               near ear at low frequencies; the knob scales that feed
//               ratio linearly.
//               MonoSafe: the former M / S shelf, S' -= crossfeed * 0.6 *
//               LP1_700Hz(S): narrows the low end without any delay, mono
//               exact.
//   autoMonoSafety: running L/R correlation (300 ms); if it drops below
//               minCorrelation the effective width is pulled back towards 1
//               (widths above 1 only: a narrowed image is never widened).
// Stereo only: blocks with numChannels != 2 pass through untouched.
// Zero latency.
#pragma once

#include "Processor.h"
#include "Svf.h"
#include "flub/common/SmoothedValue.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <vector>

namespace flub
{
/** Headphone crossfeed model (docs/11 E12 Phase A). */
enum class CrossfeedType
{
    Bs2b,    // energy-preserving L/R crossfeed, 700 Hz head shadow, 4.5 dB feed, 0.27 ms ITD
    Meier,   // same structure, 650 Hz, 9.5 dB feed (subtler)
    MonoSafe // M / S low shelf on S only: no delay, mono sum exact (the pre-E12 crossfeed)
};

struct SpatializerParams
{
    float width = 1.0f;            // 0 (mono) .. 2
    float widthLowCutHz = 180.0f;  // 60 .. 500 Hz
    float positionalFocus = 0.0f;  // 0 .. 1
    float space = 0.0f;            // 0 .. 1
    float crossfeed = 0.0f;        // 0 .. 1
    CrossfeedType crossfeedType = CrossfeedType::Bs2b;
    bool autoMonoSafety = true;
    float minCorrelation = 0.0f;   // -1 .. 1

    bool operator== (const SpatializerParams&) const = default;
};

class StereoSpatializer final : public Processor
{
public:
    void prepare (const ProcessSpec& spec) override;
    void reset() noexcept FLUB_NONBLOCKING override;
    void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING override;
    const char* name() const noexcept override { return "Stereo & Space"; }

    void setParams (const SpatializerParams& p) noexcept FLUB_NONBLOCKING;
    const SpatializerParams& getParams() const noexcept { return params; }

    /** Output L/R correlation (-1..1) and the width actually applied. */
    float getCorrelation() const noexcept { return correlation.load (std::memory_order_relaxed); }
    float getEffectiveWidth() const noexcept { return effectiveWidth.load (std::memory_order_relaxed); }

private:
    // ---- implementation-defined below this line ----
    //
    // Width realisation note: S_low / S_high are a COMPLEMENTARY split
    // (S_low + S_high = S exactly), so for w <= 1 the width is a plain gain
    // w * S and for w > 1 it is S + (w - 1) * S_high, i.e. a 2nd-order
    // minimum-phase high shelf on S (Q 1/sqrt 2, 0 dB below widthLowCutHz,
    // 20 log10 w above, half of that at the cut - the same magnitude
    // transition as an in-phase LR4 sum). A literal LR4 pair sums to an
    // all-pass; applied to S alone (M must stay untouched for the mono
    // guarantee) it would rotate S by -180 degrees against M at the low cut,
    // mirroring the image there, and width 1 could never be transparent.
    // See StereoSpatializer.cpp for the full signal flow.
    static constexpr int kControlInterval = 32;

    /** Power-of-two circular buffer; all lines share one write position. */
    struct DelayBuffer
    {
        void allocate (int maxDelay);
        void clear() noexcept { std::fill (data.begin(), data.end(), 0.0f); }
        float read (int pos, int delay) const noexcept { return data[static_cast<size_t> ((pos - delay) & mask)]; }
        void write (int pos, float x) noexcept { data[static_cast<size_t> (pos & mask)] = x; }

        std::vector<float> data;
        int mask = 0;
    };

    void clearState() noexcept;
    void sanitiseState() noexcept;
    void controlTick() noexcept;
    void updateWidthTarget() noexcept;
    void designShelf (float width, float g0) noexcept;
    void designWidthDetect (float g0) noexcept;
    void designFocus (float gainDb) noexcept;
    void designCrossfeed (float ratio) noexcept;
    float prewarp (float hz) const noexcept;
    float onePoleG (float hz) const noexcept;
    float ambience (float mid, int pos) noexcept;
    float correlationEstimate() const noexcept;

    ProcessSpec spec;
    SpatializerParams params;
    std::atomic<float> correlation { 1.0f }, effectiveWidth { 1.0f };
    bool prepared = false;
    double sr = 48000.0;

    // Width: complementary high shelf on S, re-derived only while width or
    // low cut glide (per sample, so there are no control-rate steps).
    OnePoleSmoother widthSmoother;     // effective width (user width x mono safety)
    OnePoleSmoother lowCutLogHz;       // ln (widthLowCutHz)
    float lowCutG0 = 0.0f;             // tan (pi fc / fs) of the current low cut
    float shelfWidth = 1.0f, shelfG0 = 0.0f; // design the shelf coefficients hold
    SvfCoeffs shelfCoeffs;
    SvfState shelfState;
    // Width polarity guard: envelopes of HP (M), HP (S) at the low cut and of
    // the side signal the shelf adds; share of that signal that may be applied.
    SvfCoeffs widthDetectCoeffs;
    SvfState widthMidState, widthSideState;
    float envWidthMid = 0.0f, envWidthSide = 0.0f, envWidthAdd = 0.0f, widthGuard = 1.0f;
    bool widthGuardLive = false; // detectors run only while the width is above 1

    // Positional focus: 3 kHz bell on S.
    OnePoleSmoother focusDb;
    float focusMaxDb = 0.0f; // bell gain at focus 1 (0 at speech-link rates)
    float focusG = 0.0f;
    SvfCoeffs focusCoeffs;
    SvfState focusState;
    float focusCentreGain = 1.0f; // A^2 of the current bell (linear gain at 3 kHz)
    // Polarity guard: band envelopes of M and S around the bell, and the share
    // of the bell's lift that may be applied (see StereoSpatializer.cpp).
    SvfCoeffs focusDetectCoeffs;
    SvfState detectMidState, detectSideState;
    float envMid = 0.0f, envSide = 0.0f, focusGuard = 1.0f;
    float envRelease = 0.0f, guardRelease = 0.0f;

    // Space: HP 300 Hz (M) -> presence dip -> pre-delay -> nested all-pass network -> S.
    SvfCoeffs spaceHpCoeffs, spaceDipCoeffs;
    SvfState spaceHpState, spaceDipState;
    DelayBuffer preDelayLine, outerLine, middleLine, innerLine;
    int preDelaySamples = 1, outerDelay = 1, middleDelay = 1, innerDelay = 1;
    int writePos = 0, writeMask = 0;
    float lastAmbience = 0.0f;
    OnePoleSmoother spaceGain;

    // Mono-safe crossfeed: first-order TPT low-pass on S.
    float crossfeedG = 0.0f, crossfeedState = 0.0f;
    OnePoleSmoother crossfeedGain;

    // Bs2b / Meier crossfeed: head-shadow TPT low-pass per output channel,
    // ITD lines on their outputs (3rd-order Lagrange read), near-ear shelf.
    OnePoleSmoother xfeedRatio;        // far / near feed ratio at DC (0 = off)
    OnePoleSmoother xfeedLogHz;        // ln (head-shadow corner)
    float xfeedG = 0.0f;               // TPT coefficient of the current corner
    float xfeedDesigned = 0.0f;        // ratio the two gains below hold
    float xfeedNearCut = 0.0f;         // 1 - n0: near-ear shelf depth
    float xfeedFar = 0.0f;             // g: far-ear gain at DC
    float xfeedStateL = 0.0f, xfeedStateR = 0.0f;
    DelayBuffer xfeedLineL, xfeedLineR;
    bool xfeedLive = false;            // filters / lines run only while on or fading
    int xfeedBase = 0;                 // integer part of the Lagrange read
    std::array<float, 4> xfeedTaps {};

    // Output correlation (300 ms mean products) and the mono-safety pull.
    double corrCoeff = 0.0, corrLR = 0.0, corrLL = 0.0, corrRR = 0.0;
    float safety = 0.0f; // 0 = user width .. 1 = width pulled to 1
    float safetyAttackStep = 0.0f, safetyReleaseStep = 0.0f, safetyOffStep = 0.0f;
    int controlCountdown = kControlInterval;
};
} // namespace flub
