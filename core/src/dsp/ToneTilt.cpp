#include "flub/dsp/ToneTilt.h"

#include "flub/analysis/LoudnessMeter.h"
#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>

namespace flub
{
namespace
{
// K-weighted mean square (both channels summed) below which a chunk is
// silence and leaves the level measure alone (-80 dB).
constexpr double kSilence = 1.0e-8;
// Keeps the K-weighting's recursive states normal during digital silence
// without FTZ / DAZ (as LoudnessFollower): the RLB high-pass removes it.
constexpr double kAntiDenormal = 1.0e-20;

bool sameCoeffs (const SvfCoeffs& a, const SvfCoeffs& b) noexcept
{
    return a.a1 == b.a1 && a.a2 == b.a2 && a.a3 == b.a3 && a.m0 == b.m0 && a.m1 == b.m1 && a.m2 == b.m2;
}

SvfCoeffs lerpCoeffs (const SvfCoeffs& a, const SvfCoeffs& b, float t) noexcept
{
    SvfCoeffs m = b;
    m.a1 = a.a1 + t * (b.a1 - a.a1);
    m.a2 = a.a2 + t * (b.a2 - a.a2);
    m.a3 = a.a3 + t * (b.a3 - a.a3);
    m.m0 = a.m0 + t * (b.m0 - a.m0);
    m.m1 = a.m1 + t * (b.m1 - a.m1);
    m.m2 = a.m2 + t * (b.m2 - a.m2);
    return m;
}
} // namespace

void ToneTilt::sections (float amt, double fs, SvfCoeffs& bodyBell, SvfCoeffs& highShelf) noexcept FLUB_NONBLOCKING
{
    const double a = std::clamp (static_cast<double> (amt), 0.0, 1.0);
    bodyBell = SvfCoeffs::make (FilterType::Bell, kBodyHz, kBodyQ, kBodyMaxDb * a, fs);
    highShelf = SvfCoeffs::make (FilterType::HighShelf, kHighShelfHz, kHighShelfQ, kHighShelfMaxDb * a, fs);
}

void ToneTilt::prepare (const ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;
    numChannels = std::clamp (spec.numChannels, 1, kMaxChannels);
    maxStep = static_cast<float> (kGlidePerSecond * kUpdateSamples / sampleRate);
    compAlpha = 1.0 - std::exp (-static_cast<double> (kUpdateSamples) / (0.001 * kCompTimeConstantMs * sampleRate));
    kStage1.setCoeffs (LoudnessMeter::kWeightingStage1 (sampleRate));
    kStage2.setCoeffs (LoudnessMeter::kWeightingStage2 (sampleRate));
    sections (1.0f, sampleRate, fullBody, fullHigh);
    target = 0.0f;
    reset();
    snapPending = true; // the first setParams() after prepare() applies without a glide
}

void ToneTilt::startMeasure() noexcept
{
    kStage1.reset();
    kStage2.reset();
    for (auto& s : fullBodyState)
        s.reset();
    for (auto& s : fullHighState)
        s.reset();
    chunkIn = chunkFull = 0.0;
    msIn = msFull = 0.0;
    measuredUpdates = 0;
    loudnessDb = 0.0f;
}

void ToneTilt::reset() noexcept FLUB_NONBLOCKING
{
    for (auto& s : bodyState)
        s.reset();
    for (auto& s : highState)
        s.reset();
    startMeasure();
    amount = target;
    sections (amount, sampleRate, body, high);
    bodyFrom = body;
    highFrom = high;
    trimDb = trimFromDb = 0.0f;
    untilUpdate = 0;
    running = target > 0.0f;
    appliedAmount.store (amount, std::memory_order_relaxed);
    appliedTrimDb.store (0.0f, std::memory_order_relaxed);
    fullTiltDb.store (0.0f, std::memory_order_relaxed);
}

void ToneTilt::setParams (const ToneTiltParams& p) noexcept FLUB_NONBLOCKING
{
    target = p.amount > 0.0f ? std::min (p.amount, 1.0f) : 0.0f; // NaN -> 0
    if (snapPending)
    {
        snapPending = false;
        reset();
        return;
    }
    if (target > 0.0f && ! running)
    {
        running = true; // from cleared filters and a fresh measure (see process())
        untilUpdate = 0;
    }
}

void ToneTilt::update() noexcept
{
    // The level measure: the chunk since the last update (kUpdateSamples;
    // nothing yet on the first update after a start, which reads as silence).
    const double in = chunkIn / kUpdateSamples, full = chunkFull / kUpdateSamples;
    chunkIn = chunkFull = 0.0;
    if (in > kSilence)
    {
        ++measuredUpdates;
        const double w = std::max (1.0 / measuredUpdates, compAlpha); // a running mean until the time constant
        msIn += w * (in - msIn);
        msFull += w * (full - msFull);
        if (std::isfinite (msIn) && std::isfinite (msFull) && msIn > 0.0 && msFull > 0.0)
            loudnessDb = static_cast<float> (std::clamp (10.0 * std::log10 (msFull / msIn), kHighShelfMaxDb, kBodyMaxDb));
        else
            startMeasure(); // a non-finite input: start over
    }

    const float previous = amount;
    amount = std::abs (target - amount) <= maxStep ? target : amount + std::copysign (maxStep, target - amount);
    bodyFrom = body;
    highFrom = high;
    trimFromDb = trimDb;
    if (amount != previous)
        sections (amount, sampleRate, body, high);
    trimDb = -amount * loudnessDb;
}

void ToneTilt::process (const AudioBlock& block) noexcept FLUB_NONBLOCKING
{
    if (! running)
        return;

    const int n = block.numSamples;
    const int channels = std::min (block.numChannels, numChannels);
    int start = 0;
    while (start < n)
    {
        if (untilUpdate <= 0)
        {
            update();
            untilUpdate = kUpdateSamples;
        }
        // Coefficients and trim move linearly from the previous update's
        // values to this one's over the kUpdateSamples, sample by sample.
        const int len = std::min (n - start, untilUpdate);
        const int done = kUpdateSamples - untilUpdate;
        const bool gliding = ! sameCoeffs (bodyFrom, body) || ! sameCoeffs (highFrom, high);
        const float g0 = dbToGain (trimFromDb), g1 = dbToGain (trimDb);
        for (int i = 0; i < len; ++i)
        {
            const float t = static_cast<float> (done + i + 1) / static_cast<float> (kUpdateSamples);
            const SvfCoeffs l = gliding ? lerpCoeffs (bodyFrom, body, t) : body;
            const SvfCoeffs h = gliding ? lerpCoeffs (highFrom, high, t) : high;
            const float g = g0 + t * (g1 - g0);
            for (int ch = 0; ch < channels; ++ch)
            {
                float* x = block.channel (ch) + start + i;
                const float v = *x;
                // The level measure reads the input only (open loop).
                const double k = kStage2.processSample (ch, kStage1.processSample (ch, static_cast<double> (v) + kAntiDenormal));
                const float kf = svfTick (fullHigh, fullHighState[static_cast<size_t> (ch)],
                                          svfTick (fullBody, fullBodyState[static_cast<size_t> (ch)], static_cast<float> (k)));
                chunkIn += k * k;
                chunkFull += static_cast<double> (kf) * static_cast<double> (kf);
                const float y = svfTick (h, highState[static_cast<size_t> (ch)], svfTick (l, bodyState[static_cast<size_t> (ch)], v));
                *x = y * g;
            }
        }
        untilUpdate -= len;
        start += len;
    }
    appliedAmount.store (amount, std::memory_order_relaxed);
    appliedTrimDb.store (trimDb, std::memory_order_relaxed);
    fullTiltDb.store (loudnessDb, std::memory_order_relaxed);

    if (target == 0.0f && amount == 0.0f && trimDb == 0.0f && trimFromDb == 0.0f && sameCoeffs (bodyFrom, body) && sameCoeffs (highFrom, high))
    {
        running = false; // flat: idle from here on (see the header comment)
        for (auto& s : bodyState)
            s.reset();
        for (auto& s : highState)
            s.reset();
        startMeasure();
    }
}
} // namespace flub
