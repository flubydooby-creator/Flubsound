#include "flub/neural/VoiceCleanupRunner.h"

#include <algorithm>
#include <cmath>

namespace flub
{
VoiceCleanupRunner::VoiceCleanupRunner (std::shared_ptr<VoiceCleanupTelemetry> t)
    : VoiceCleanupRunner (voiceCleanupModelData(), voiceCleanupModelSize(), std::move (t))
{
}

VoiceCleanupRunner::VoiceCleanupRunner (const void* modelData, std::size_t modelBytes, std::shared_ptr<VoiceCleanupTelemetry> t)
    : telemetry (std::move (t))
{
    std::string error;
    if (! net.load (modelData, modelBytes, error))
    {
        loadError = "voice cleanup model: " + error;
        return;
    }
    const nn::ModelInfo& info = net.info();
    if (info.featureSet != kFeatureSet || info.sampleRate != static_cast<uint32_t> (kSampleRate) || info.frameSize != static_cast<uint32_t> (kFrameSize)
        || net.numInputs() != kNumFeatures || net.numOutputs() != 2 || net.outputSize (0) != kNumBands || net.outputSize (1) != 1)
    {
        loadError = "voice cleanup model: the model does not fit this front end (feature set 1, 48 kHz, 240-sample frames, "
                    "23 features in, 22 band gains and a voice activity out)";
        return;
    }

    fft.prepare (kFftSize);
    bandMap.build (kBandCentresHz, kNumBands, kFftSize, kSampleRate);
    window.resize (2 * kFrameSize);
    for (int n = 0; n < 2 * kFrameSize; ++n)
        window[static_cast<size_t> (n)] = bandgains::vorbisWindow (n, 2 * kFrameSize);
    previousHop.assign (kFrameSize, 0.0f);
    fftBuffer.assign (kFftSize, 0.0f);
    bins.assign (kFftSize / 2 + 1, Fft::Complex {});
    power.assign (kFftSize / 2 + 1, 0.0f);
    energy.assign (kNumBands, 0.0f);
    features.assign (kNumFeatures, 0.0f);
    previousGains.assign (kNumBands, 1.0f);
    history.assign (kPitchHistory, 0.0f);
    energySum.assign (kPitchHistory + 1, 0.0);
    valid = true;
}

ModelDescription VoiceCleanupRunner::describe() const
{
    ModelDescription d;
    if (! valid)
        return d; // frameSize 0: invalid, the processor stays inert
    d.frameSize = kFrameSize;
    d.numInputChannels = 1;
    d.numControls = kNumBands;
    d.controlKind = ControlKind::BandGains;
    d.sampleRate = kSampleRate;
    d.fftSize = kFftSize;
    d.bandCentresHz = kBandCentresHz;
    return d;
}

void VoiceCleanupRunner::reset()
{
    if (! valid)
        return;
    net.reset();
    std::fill (previousHop.begin(), previousHop.end(), 0.0f);
    std::fill (history.begin(), history.end(), 0.0f);
    std::fill (previousGains.begin(), previousGains.end(), 1.0f); // the suppression fades in from unity
    lastVad = 0.0f;
}

bool VoiceCleanupRunner::run (const float* inFrame, float* outControls)
{
    if (! valid)
        return false;
    processFrame (inFrame, outControls);
    return true;
}

void VoiceCleanupRunner::computeFeatures (const float* hop) noexcept
{
    // Band log-energies of the window [previous hop, this hop].
    for (int n = 0; n < kFrameSize; ++n)
    {
        const auto i = static_cast<size_t> (n);
        fftBuffer[i] = window[i] * previousHop[i];
        fftBuffer[i + kFrameSize] = window[i + kFrameSize] * hop[n];
    }
    std::fill (fftBuffer.begin() + 2 * kFrameSize, fftBuffer.end(), 0.0f);
    fft.forwardReal (fftBuffer.data(), bins.data());
    for (size_t k = 0; k < bins.size(); ++k)
        power[k] = std::norm (bins[k]);
    bandMap.binsToBands (power.data(), energy.data());
    for (int b = 0; b < kNumBands; ++b)
        features[static_cast<size_t> (b)] = std::log10 (energy[static_cast<size_t> (b)] + kEnergyFloor);

    // Voicing: shift in this hop, box-decimated by 4.
    constexpr int perHop = kFrameSize / kDecimation;
    std::copy (history.begin() + perHop, history.end(), history.begin());
    for (int m = 0; m < perHop; ++m)
    {
        const float* q = hop + m * kDecimation;
        history[static_cast<size_t> (kPitchHistory - perHop + m)] = (q[0] + q[1] + q[2] + q[3]) * 0.25f;
    }
    energySum[0] = 0.0;
    for (int i = 0; i < kPitchHistory; ++i)
    {
        const double v = history[static_cast<size_t> (i)];
        energySum[static_cast<size_t> (i + 1)] = energySum[static_cast<size_t> (i)] + v * v;
    }
    const float* seg = history.data() + kLagMax;
    const double segEnergy = energySum[kPitchHistory] - energySum[kLagMax];
    float best = 0.0f;
    for (int lag = kLagMin; lag <= kLagMax; ++lag)
    {
        const int m = kLagMax - lag;
        const float* ref = history.data() + m;
        float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
        for (int n = 0; n < kPitchSegment; n += 4)
        {
            s0 += seg[n] * ref[n];
            s1 += seg[n + 1] * ref[n + 1];
            s2 += seg[n + 2] * ref[n + 2];
            s3 += seg[n + 3] * ref[n + 3];
        }
        const double num = (s0 + s1) + (s2 + s3);
        const double refEnergy = std::max (0.0, energySum[static_cast<size_t> (m + kPitchSegment)] - energySum[static_cast<size_t> (m)]);
        const auto r = static_cast<float> (num / std::sqrt (segEnergy * refEnergy + 1.0e-9));
        best = std::max (best, r);
    }
    features[kNumBands] = best;
}

void VoiceCleanupRunner::processFrame (const float* hop, float* gains) noexcept FLUB_NONBLOCKING
{
    computeFeatures (hop);
    net.run (features.data());
    const float* g = net.output (0);
    lastVad = net.output (1)[0];

    double before = 0.0, after = 0.0;
    for (int b = 0; b < kNumBands; ++b)
    {
        const auto i = static_cast<size_t> (b);
        float v = std::max (g[b], kGainFloor);
        v = std::max (v, kRelease * previousGains[i]);
        previousGains[i] = v;
        gains[b] = v;
        before += energy[i];
        after += static_cast<double> (energy[i]) * v * v;
    }
    std::copy (hop, hop + kFrameSize, previousHop.begin());

    if (telemetry != nullptr)
    {
        telemetry->voiceActivity.store (lastVad, std::memory_order_relaxed);
        const auto reduction = before > 1.0e-12 ? static_cast<float> (10.0 * std::log10 (std::max (after, 1.0e-30) / before)) : 0.0f;
        telemetry->reductionDb.store (std::min (0.0f, reduction), std::memory_order_relaxed);
        telemetry->frames.fetch_add (1, std::memory_order_relaxed);
    }
}
} // namespace flub
