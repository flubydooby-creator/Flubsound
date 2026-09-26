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
    void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING override;
    int latencySamples() const noexcept override;
    const char* name() const noexcept override { return "Spectral Noise Gate"; }

    void setParams (const NoiseGateParams& p) noexcept;
    const NoiseGateParams& getParams() const noexcept { return params; }

private:
    // ---- implementation-defined below this line ----
    struct ChannelState
    {
        std::vector<float> input;      // analysis FIFO: the last frameSize input samples, oldest first
        std::vector<float> output;     // overlap-add accumulator; [0, hop) is the next hop's output
        std::vector<float> power;      // P_k : bin power smoothed over ~10 ms
        std::vector<float> noiseFloor; // N_k : bias-compensated minimum-tracked floor (~ mean noise power)
        std::vector<float> gainDb;     // per-bin gate gain after attack / release (dB)
        float hopPeak = 0.0f;          // max |x| of the hop being collected (digital-silence detection)
        unsigned silentHops = 0xFu;    // one bit per hop in the analysis window: 1 = silent / before reset
        int holdFrames = 0;            // valid frames left before the floor may adapt (P_k settling)
        int learnFrames = 0;           // adapting frames left in the initial learning period (ignores freeze)
        bool floorValid = false;       // a noise profile has been learned since the last reset
    };

    void updateCoefficients() noexcept;
    void clearChannel (ChannelState& state) noexcept;
    void processFrame (ChannelState& state) noexcept;

    int fftSize = 512;                 // requested by setFftSize(), validated in prepare()
    ProcessSpec spec;
    NoiseGateParams params;

    bool prepared = false;
    int frameSize = 512, hopSize = 128, numBins = 257;
    int numChannels = 0, activeChannels = 0;
    int hopPos = 0;                    // samples collected in the current hop (shared by all channels)
    int warmupFrames = 1;
    int learnPeriodFrames = 1;         // length of the initial learning period (frames)
    double hopSeconds = 128.0 / 48000.0;

    // Per-hop coefficients (derived from params in updateCoefficients()).
    float powerNorm = 2.0f / 512.0f;   // 1 / sum(w^2): white noise of variance s^2 reads P_k = s^2
    float powerCoeff = 0.0f, paramCoeff = 0.0f, attackCoeff = 0.0f, releaseCoeff = 0.0f;
    float riseFactor = 1.0f, floorBias = 1.0f;
    float thresholdCur = 6.0f, reductionCur = 12.0f; // hop-rate smoothed thresholdDb / reductionDb

    Fft fft;
    std::vector<float> analysisWindow, synthesisWindow, frame;
    std::vector<float> snr, gains;     // per-bin scratch: P_k / N_k, linear gate gain
    std::vector<Fft::Complex> bins;
    std::array<ChannelState, kMaxChannels> channels {};
};
} // namespace flub
