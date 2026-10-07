// Flubsound Pro - the neural voice cleanup model (experimental; docs/03 §16,
// docs/09 §1.1, docs/11 E35): an RNNoise-style speech-in-noise suppressor for
// the Chat strip, trained in-house on synthetic data (tools/neural/) and run by
// the TinyNet runtime (flub/neural/TinyNet.h) on AsyncModelProcessor's worker.
//
// Per model frame (5 ms at 48 kHz, the mono downmix of the strip):
//   features (feature set 1, tools/neural/fvdsp.py is the reference):
//     the 10 ms window [previous hop, this hop] (Vorbis), zero-padded to a
//     512-point FFT; 22 band energies with the BandGains layout below
//     (flub/neural/BandGains.h); log10 (energy + 1e-10); plus a voicing
//     strength: the largest normalised autocorrelation, at lags of 2.5 - 15 ms,
//     of the last 20 ms of the signal box-decimated 4x (12 kHz);
//   network (the embedded model, ~51 k parameters, int8 weights): a causal
//     convolution over 3 frames, two GRUs, a voice-activity output and 22
//     sigmoid band gains;
//   post-processing: each gain at least kGainFloor (-30 dB: never a hard gate)
//     and at least kRelease times its previous value, so a cut deepens by at
//     most 2.2 dB per frame (RNNoise's 0.6 per 10 ms) and lets go at once.
// Controls: ControlKind::BandGains, 22 bands, fftSize 512. With the
// processor's default safety frame count the chain reports
// L = 240 * (2 + safetyFrames) samples (960 = 20 ms with two safety frames,
// within Balanced's budget; see Eligibility.h).
//
// Only 48 kHz: at any other rate the description's sample rate keeps the
// model out of the chain (NeuralSlotState::SampleRateMismatch).
#pragma once

#include "flub/common/Realtime.h"
#include "flub/dsp/Fft.h"
#include "flub/neural/BandGains.h"
#include "flub/neural/ModelRunner.h"
#include "flub/neural/TinyNet.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace flub
{
/** What the runner reports while it runs (relaxed atomics; any thread). */
struct VoiceCleanupTelemetry
{
    std::atomic<float> voiceActivity { 0.0f }; // the model's voice-activity output of the last frame, 0..1
    std::atomic<float> reductionDb { 0.0f };   // level change of the last frame from its band gains (dB <= 0)
    std::atomic<uint64_t> frames { 0 };        // frames run
};

/** The embedded model (core/src/neural/VoiceCleanupModelData.cpp, generated
    from presets/neural/voice-cleanup.fnn by tools/neural/train_voice_cleanup.py). */
const unsigned char* voiceCleanupModelData() noexcept;
std::size_t voiceCleanupModelSize() noexcept;

class VoiceCleanupRunner final : public ModelRunner
{
public:
    static constexpr int kFrameSize = 240;
    static constexpr int kFftSize = 512;
    static constexpr int kNumBands = 22;
    static constexpr int kNumFeatures = kNumBands + 1;
    static constexpr uint32_t kFeatureSet = 1;
    static constexpr double kSampleRate = 48000.0;
    static constexpr float kGainFloor = 0.03f; // -30.5 dB
    static constexpr float kRelease = 0.775f;  // per frame: 0.6 per 10 ms
    static constexpr float kEnergyFloor = 1.0e-10f;
    // Voicing (decimated domain, 12 kHz).
    static constexpr int kDecimation = 4, kPitchSegment = 240, kLagMin = 30, kLagMax = 180;
    static constexpr int kPitchHistory = kPitchSegment + kLagMax;
    static constexpr float kBandCentresHz[kNumBands] = { 0.0f, 200.0f, 400.0f, 600.0f, 800.0f, 1000.0f, 1200.0f, 1400.0f,
                                                         1600.0f, 2000.0f, 2400.0f, 2800.0f, 3200.0f, 4000.0f, 4800.0f,
                                                         5600.0f, 6800.0f, 8000.0f, 9600.0f, 12000.0f, 15600.0f, 20000.0f };

    /** Non-RT: loads the embedded model and allocates everything. */
    explicit VoiceCleanupRunner (std::shared_ptr<VoiceCleanupTelemetry> telemetry = {});
    /** Non-RT: loads a model file held in memory (tests, experiments). */
    VoiceCleanupRunner (const void* modelData, std::size_t modelBytes, std::shared_ptr<VoiceCleanupTelemetry> telemetry = {});

    /** False when the model did not load or does not fit this front end;
        describe() is then invalid (frameSize 0), so the processor stays inert
        and the chain reports InvalidModel. */
    bool isValid() const noexcept { return valid; }
    const std::string& getLoadError() const noexcept { return loadError; }
    const nn::ModelInfo& getModelInfo() const noexcept { return net.info(); }

    ModelDescription describe() const override;
    void reset() override;
    bool run (const float* inFrame, float* outControls) override;

    /** What run() does, without the virtual call: hop = kFrameSize samples,
        gains = kNumBands floats. Allocation-free; resets nothing. */
    void processFrame (const float* hop, float* gains) noexcept FLUB_NONBLOCKING;

    /** The features and voice activity of the last frame (tests, diagnostics). */
    const float* getLastFeatures() const noexcept { return features.data(); }
    float getLastVoiceActivity() const noexcept { return lastVad; }

private:
    void computeFeatures (const float* hop) noexcept;

    std::shared_ptr<VoiceCleanupTelemetry> telemetry;
    nn::TinyNet net;
    bool valid = false;
    std::string loadError;

    Fft fft;
    bandgains::BandMap bandMap;
    std::vector<float> window, previousHop, fftBuffer, power, energy, features, previousGains;
    std::vector<Fft::Complex> bins;
    std::vector<float> history;   // kPitchHistory decimated samples, oldest first
    std::vector<double> energySum; // running sum of history^2 (kPitchHistory + 1)
    float lastVad = 0.0f;
};
} // namespace flub
