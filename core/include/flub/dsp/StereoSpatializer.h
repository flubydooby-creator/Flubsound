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

#include <atomic>

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
    ProcessSpec spec;
    SpatializerParams params;
    std::atomic<float> correlation { 1.0f }, effectiveWidth { 1.0f };
};
} // namespace flub
