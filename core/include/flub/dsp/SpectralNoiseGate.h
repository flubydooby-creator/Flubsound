// Flubsound Pro - spectral noise gate / light noise reduction (STFT).
//
// Frame: fftSize (default 512 @ 48 kHz), hop = fftSize/4, sqrt-Hann analysis
//   and synthesis windows (75% overlap, constant overlap-add), latency =
//   fftSize samples.
// Per bin k (power P_k, smoothed over time with ~10 ms):
//   noise floor N_k : minimum tracking - follows P_k instantly downwards,
//                     rises at most floorRiseDbPerSec upwards (speech/music
//                     never "becomes" the floor, slow changes in hum/hiss do).
//   gate target     : open (1) when P_k > N_k * 10^(thresholdDb/10), else
//                     G_min = 10^(-reductionDb/20); a 6 dB soft transition.
//   smoothing       : per-bin attack/release (open fast, close slow) and a
//                     3-bin frequency average -> suppresses "musical noise".
// "Light" by design: reductionDb defaults to 12 dB. Intended for music
// playback, noisy captures and batch restoration; the Gaming profile removes
// it from the chain (latency) - voice-chat denoise is a separate future
// neural module (see docs/09).
#pragma once

#include "Fft.h"
#include "Processor.h"

namespace flub
{
struct NoiseGateParams
{
    float thresholdDb = 6.0f;      // 0 .. 20 dB above the tracked floor
    float reductionDb = 12.0f;     // 0 .. 40
    float attackMs = 5.0f;         // 1 .. 50
    float releaseMs = 80.0f;       // 10 .. 500
    float floorRiseDbPerSec = 3.0f;// 0.5 .. 20
    bool freezeFloor = false;      // hold the learned noise profile

    bool operator== (const NoiseGateParams&) const = default;
};

class SpectralNoiseGate final : public Processor
{
public:
    /** Structural: call before prepare(). Power of two 256 .. 4096. */
    void setFftSize (int size) noexcept { fftSize = size; }

    void prepare (const ProcessSpec& spec) override;
    void reset() noexcept override;
    void process (const AudioBlock& block) noexcept override;
    int latencySamples() const noexcept override;
    const char* name() const noexcept override { return "Spectral Noise Gate"; }

    void setParams (const NoiseGateParams& p) noexcept;
    const NoiseGateParams& getParams() const noexcept { return params; }

private:
    // ---- implementation-defined below this line ----
    int fftSize = 512;
    ProcessSpec spec;
    NoiseGateParams params;
};
} // namespace flub
