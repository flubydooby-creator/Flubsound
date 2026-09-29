#include "flub/dsp/VoiceActivity.h"

#include <algorithm>
#include <cmath>

namespace flub
{
namespace
{
constexpr int kNever = 1 << 20;
constexpr float kSilentDb = -200.0f;

int framesOf (float ms) noexcept
{
    return std::max (1, static_cast<int> (std::lround (ms / VoiceActivity::kFrameMs)));
}
} // namespace

void VoiceActivity::prepare (double sr) noexcept
{
    sampleRate = sr;
    frameLength = std::max (1, static_cast<int> (std::lround (kFrameMs * 0.001 * sr)));
    peakFrames = std::min (kMaxPeakFrames, framesOf (kPeakWindowMs));
    evidenceFrames = framesOf (kEvidenceMs);
    hangoverFrames = framesOf (kHangoverMs);
    hp = SvfCoeffs::make (FilterType::HighPass, kBandLowHz, 0.70710678, 0.0, sr);
    lp = SvfCoeffs::make (FilterType::LowPass, kBandHighHz, 0.70710678, 0.0, sr);
    // Constant-Q band-passes at the centres of six equal log-width slices of
    // 300 - 3400 Hz (about 0.58 octave each: Q 2.5).
    const double span = std::log (static_cast<double> (kBandHighHz / kBandLowHz));
    for (int b = 0; b < kNumFlatnessBands; ++b)
    {
        const double centre = kBandLowHz * std::exp (span * (b + 0.5) / kNumFlatnessBands);
        bandCoeffs[static_cast<size_t> (b)] = SvfCoeffs::make (FilterType::BandPass, centre, 2.5, 0.0, sr);
    }
    reset();
}

void VoiceActivity::reset() noexcept FLUB_NONBLOCKING
{
    hpState.reset();
    lpState.reset();
    for (auto& s : bandStates)
        s.reset();
    wholeSum = bandSum = 0.0;
    flatSums.fill (0.0);
    history.fill (kSilentDb);
    historyPos = frameFill = 0;
    sinceVoiced = sinceSyllable = sinceSpeech = kNever;
    frame = {};
    active = false;
    frames = activeFrames = 0;
    activeFlag.store (false, std::memory_order_relaxed);
}

void VoiceActivity::seedActive() noexcept FLUB_NONBLOCKING
{
    sinceSpeech = 0;
    active = true;
    activeFlag.store (true, std::memory_order_relaxed);
}

void VoiceActivity::process (const AudioBlock& in) noexcept FLUB_NONBLOCKING
{
    if (in.numChannels <= 0)
    {
        processSilence (in.numSamples);
        return;
    }
    const float* l = in.channel (0);
    const float* r = in.numChannels > 1 ? in.channel (1) : l;
    for (int k = 0; k < in.numSamples; ++k)
    {
        float x = 0.5f * (l[k] + r[k]);
        if (! std::isfinite (x)) // a broken driver block: heard as nothing
            x = 0.0f;
        const float band = svfTick (lp, lpState, svfTick (hp, hpState, x));
        wholeSum += static_cast<double> (x) * x;
        bandSum += static_cast<double> (band) * band;
        for (size_t b = 0; b < static_cast<size_t> (kNumFlatnessBands); ++b)
        {
            const float y = svfTick (bandCoeffs[b], bandStates[b], x);
            flatSums[b] += static_cast<double> (y) * y;
        }
        if (++frameFill == frameLength)
            endFrame();
    }
}

void VoiceActivity::processSilence (int numSamples) noexcept FLUB_NONBLOCKING
{
    // The filters ring out within a frame of silence; their states are
    // cleared at once instead (what they would hold after a few frames).
    hpState.reset();
    lpState.reset();
    for (auto& s : bandStates)
        s.reset();
    for (int left = numSamples; left > 0;)
    {
        const int take = std::min (left, frameLength - frameFill);
        frameFill += take;
        left -= take;
        if (frameFill == frameLength)
            endFrame();
    }
}

void VoiceActivity::endFrame() noexcept FLUB_NONBLOCKING
{
    const double n = static_cast<double> (frameLength);
    const double bandMs = bandSum / n, wholeMs = wholeSum / n;
    frame.bandDb = bandMs > 1.0e-20 ? static_cast<float> (10.0 * std::log10 (bandMs)) : kSilentDb;
    frame.ratio = wholeMs > 1.0e-20 ? static_cast<float> (bandMs / wholeMs) : 0.0f;
    double logSum = 0.0, sum = 0.0;
    for (const double s : flatSums)
    {
        const double ms = s / n + 1.0e-20;
        logSum += std::log (ms);
        sum += ms;
    }
    frame.flatness = static_cast<float> (std::exp (logSum / kNumFlatnessBands) / (sum / kNumFlatnessBands));
    frame.voiced = frame.bandDb > kMinLevelDb && frame.ratio > kMinBandRatio && frame.flatness < kMaxFlatness;

    // A syllable boundary: the band has fallen kSyllableDipDb under its
    // recent maximum, and that maximum was voice.
    float recentMax = kSilentDb;
    for (int i = 0; i < peakFrames; ++i)
        recentMax = std::max (recentMax, history[static_cast<size_t> (i)]);
    sinceVoiced = frame.voiced ? 0 : std::min (kNever, sinceVoiced + 1);
    const bool voicedRecently = sinceVoiced <= peakFrames;
    if (recentMax - frame.bandDb >= kSyllableDipDb && recentMax > kMinLevelDb && voicedRecently)
        sinceSyllable = 0;
    else
        sinceSyllable = std::min (kNever, sinceSyllable + 1);
    // Only voiced frames enter the maximum: a loud noise burst or a drum
    // hit out of the voice band does not become a "syllable".
    history[static_cast<size_t> (historyPos)] = frame.voiced ? frame.bandDb : kSilentDb;
    historyPos = historyPos + 1 == peakFrames ? 0 : historyPos + 1;

    frame.speech = frame.voiced && sinceSyllable <= evidenceFrames;
    sinceSpeech = frame.speech ? 0 : std::min (kNever, sinceSpeech + 1);
    active = sinceSpeech <= hangoverFrames;
    activeFlag.store (active, std::memory_order_relaxed);
    ++frames;
    activeFrames += active ? 1u : 0u;

    wholeSum = bandSum = 0.0;
    flatSums.fill (0.0);
    frameFill = 0;
}
} // namespace flub
