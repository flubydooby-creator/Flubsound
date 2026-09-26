// Flubsound Pro - mono-compatible stereo widener / spatializer.
//
// Everything here operates on the SIDE signal only:
//   M = (L + R) / 2,  S = (L - R) / 2,  L' = M + S',  R' = M - S'
// so the mono fold-down L' + R' = 2M is preserved EXACTLY for every setting.
// That is the core mono-compatibility guarantee (tested), and it also means
// no comb filtering on mono playback, laptop speakers, or phone Bluetooth.
//
//   width     : S' = S_low * min(width, 1) + S_high * width, split with an
//               LR4 at widthLowCutHz so the low end never gets wider.
//   positionalFocus (gaming): +0..6 dB bell on S at 3 kHz (Q 0.5, ~1-6 kHz).
//               Interaural level differences in this region are the main
//               lateral localisation cue for broadband transients
//               (footsteps, reloads); emphasising S there sharpens the
//               perceived direction without touching the centre (M).
//   space     : S += space * 0.5 * D(HP_300Hz(M)), D = 3 nested Schroeder
//               all-passes (3.1/4.7/7.3 ms, g = 0.5): decorrelated ambience
//               derived from the centre. In mono it cancels (lives in S).
//   crossfeed : S' -= crossfeed * 0.6 * LP_700Hz(S): reduces low-frequency
//               separation on headphones (bs2b-like comfort) without
//               colouring M - also mono-exact.
//   autoMonoSafety: running L/R correlation (300 ms); if it drops below
//               minCorrelation the effective width is pulled back towards 1.
// Stereo only: blocks with numChannels != 2 pass through untouched.
// Zero latency.
#pragma once

#include "Processor.h"
#include "Svf.h"
#include "flub/common/SmoothedValue.h"

#include <algorithm>
#include <atomic>
#include <vector>

namespace flub
{
struct SpatializerParams
{
    float width = 1.0f;            // 0 (mono) .. 2
    float widthLowCutHz = 180.0f;  // 60 .. 500 Hz
    float positionalFocus = 0.0f;  // 0 .. 1
    float space = 0.0f;            // 0 .. 1
    float crossfeed = 0.0f;        // 0 .. 1
    bool autoMonoSafety = true;
    float minCorrelation = 0.0f;   // -1 .. 1

    bool operator== (const SpatializerParams&) const = default;
};

class StereoSpatializer final : public Processor
{
public:
    void prepare (const ProcessSpec& spec) override;
    void reset() noexcept override;
    void process (const AudioBlock& block) noexcept override;
    const char* name() const noexcept override { return "Stereo & Space"; }

    void setParams (const SpatializerParams& p) noexcept;
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
    void designFocus (float gainDb) noexcept;
    float prewarp (float hz) const noexcept;
    float ambience (float mid) noexcept;
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

    // Positional focus: 3 kHz bell on S.
    OnePoleSmoother focusDb;
    float focusG = 0.0f;
    SvfCoeffs focusCoeffs;
    SvfState focusState;

    // Space: HP 300 Hz (M) -> pre-delay -> nested all-pass network -> S.
    SvfCoeffs spaceHpCoeffs;
    SvfState spaceHpState;
    DelayBuffer preDelayLine, outerLine, middleLine, innerLine;
    int preDelaySamples = 1, outerDelay = 1, middleDelay = 1, innerDelay = 1;
    int writePos = 0, writeMask = 0;
    float lastAmbience = 0.0f;
    OnePoleSmoother spaceGain;

    // Crossfeed: first-order TPT low-pass on S.
    float crossfeedG = 0.0f, crossfeedState = 0.0f;
    OnePoleSmoother crossfeedGain;

    // Output correlation (300 ms mean products) and the mono-safety pull.
    double corrCoeff = 0.0, corrLR = 0.0, corrLL = 0.0, corrRR = 0.0;
    float safety = 0.0f; // 0 = user width .. 1 = width pulled to 1
    float safetyAttackStep = 0.0f, safetyReleaseStep = 0.0f, safetyOffStep = 0.0f;
    int controlCountdown = kControlInterval;
};
} // namespace flub
