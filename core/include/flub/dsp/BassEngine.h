// Flubsound Pro - bass engine: adaptive boost + psychoacoustic harmonics.
//
// Signal flow (per block, zero latency, all IIR):
//   1. Subsonic high-pass (Butterworth 24 dB/oct at subsonicHz; 0 = off)
//      removes DC and inaudible rumble that would waste limiter headroom.
//   2. Mono-bass (stereo only): LR4 split at monoBelowHz; the low band is
//      replaced by its mid (L+R)/2 -> tight, centred low end, no phasey sub.
//   3. Adaptive low boost: low-shelf (Q 0.7) at boostFrequency whose gain is
//      boostDb minus a protection term. A detector (LP at ~150 Hz, linked,
//      10 ms / 150 ms envelope) predicts the boosted LF level; if
//      level + boost exceeds protectThresholdDb the boost is withdrawn by the
//      excess (soft knee 6 dB). This is what keeps "+12 dB bass" from turning
//      into limiter pumping on bass-heavy material and explosions.
//   4. Psychoacoustic bass ("missing fundamental"): the mid signal is band
//      limited to [~25 Hz, harmonicsCutoff]; an amplitude-normalised
//      Chebyshev waveshaper generates exact harmonics of a sinusoid:
//        xn = clamp(x / env, -1, 1)
//        h  = w2 T2(xn) + w3 T3(xn) + w4 T4(xn) + w5 T5(xn)
//        T2 = 2x^2-1, T3 = 4x^3-3x, T4 = 8x^4-8x^2+1, T5 = 16x^5-20x^3+5x
//        y  = h * env  (harmonic level tracks the fundamental linearly)
//      weights morph from even/warm (w2=1, w3=.35, w4=.3, w5=.1) to
//      odd/punchy (w2=.35, w3=1, w4=.1, w5=.3) with harmonicsCharacter.
//      The harmonics are band-passed to [cutoff, 6*cutoff] and added equally
//      to all channels. With replaceFundamental (small speakers / laptops)
//      the original content below the cutoff is high-passed away, reclaiming
//      headroom the transducer cannot use anyway.
//   5. Tighten: TransientShaper on the LR4 low band (< 150 Hz) with negative
//      sustain = shorter, drier bass decay ("punchy" rather than "boomy").
#pragma once

#include "Processor.h"
#include "TransientShaper.h"

#include <atomic>

namespace flub
{
struct BassEngineParams
{
    float boostDb = 0.0f;              // 0 .. 15
    float boostFrequency = 70.0f;      // 30 .. 200 Hz
    float protectThresholdDb = -12.0f; // -30 .. 0 dBFS predicted LF level cap
    float harmonicsAmount = 0.0f;      // 0 .. 1 (mix of generated harmonics, up to +6 dB)
    float harmonicsCutoff = 120.0f;    // 40 .. 250 Hz (device low-frequency limit)
    float harmonicsCharacter = 0.5f;   // 0 = even/warm .. 1 = odd/punchy
    bool replaceFundamental = false;   // small-speaker mode
    float tighten = 0.0f;              // 0 .. 1 -> 0 .. -12 dB low-band sustain
    float monoBelowHz = 0.0f;          // 0 = off, else 40 .. 250 Hz
    float subsonicHz = 20.0f;          // 0 = off, else 10 .. 40 Hz

    bool operator== (const BassEngineParams&) const = default;
};

class BassEngine final : public Processor
{
public:
    void prepare (const ProcessSpec& spec) override;
    void reset() noexcept override;
    void process (const AudioBlock& block) noexcept override;
    const char* name() const noexcept override { return "Bass Engine"; }

    void setParams (const BassEngineParams& p) noexcept;
    const BassEngineParams& getParams() const noexcept { return params; }

    /** Boost currently withdrawn by the protection stage (dB >= 0), for the GUI. */
    float getProtectionDb() const noexcept { return protectionDb.load (std::memory_order_relaxed); }

private:
    // ---- implementation-defined below this line ----
    ProcessSpec spec;
    BassEngineParams params;
    std::atomic<float> protectionDb { 0.0f };
};
} // namespace flub
