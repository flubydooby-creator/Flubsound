#include "flub/dsp/SmoothnessGuard.h"

#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>

namespace flub
{
namespace
{
constexpr float kParamSmoothMs = 20.0f;
constexpr double kSilencePower = 1.0e-10; // -100 dB: no reference, no cut
constexpr float kSettledDb = 1.0e-4f;     // a cut this close to 0 dB is 0 dB

float softKnee (float overDb) noexcept
{
    constexpr float half = 0.5f * SmoothnessGuard::kKneeDb;
    if (overDb <= -half)
        return 0.0f;
    if (overDb >= half)
        return overDb;
    const float t = overDb + half;
    return t * t / (2.0f * SmoothnessGuard::kKneeDb);
}
} // namespace

void SmoothnessGuard::prepare (const ProcessSpec& spec)
{
    sr = spec.sampleRate;
    enabledForRate = sr >= kMinSampleRate;
    band = SvfCoeffs::make (FilterType::BandPass, kBandHz, kBandQ, 0.0, sr);
    body = SvfCoeffs::make (FilterType::BandPass, kBodyHz, kBodyQ, 0.0, sr);
    // BandPass mixes m1 = k: the unity-peak band-pass is k v1.
    bandK = band.m1;
    bodyK = body.m1;
    refDelay.prepare (spec.numChannels, referenceDelay);
    powerCoeff = 1.0 - static_cast<double> (onePoleCoeff (kDetectorMs, sr));
    const double controlRate = sr / kControlInterval;
    amount.reset (controlRate, kParamSmoothMs, params.amount);
    cut.prepare (controlRate, kAttackMs, kReleaseMs, false);
    reset();
}

void SmoothnessGuard::reset() noexcept FLUB_NONBLOCKING
{
    for (auto* states : { &xBand, &xBody, &rBand, &rBody })
        for (auto& s : *states)
            s.reset();
    refDelay.reset();
    pBand = pBody = rBandPow = rBodyPow = 0.0;
    amount.setImmediate (amount.getTarget());
    cut.reset (0.0f);
    gainStart = gain = 1.0f;
    gainStep = 0.0f;
    controlCountdown = kControlInterval;
    controlPhase = 0;
    blockCutDb = 0.0f;
    hasReference = false;
}

void SmoothnessGuard::setParams (const SmoothnessParams& p) noexcept FLUB_NONBLOCKING
{
    params.amount = std::isnan (p.amount) ? params.amount : std::clamp (p.amount, 0.0f, 1.0f);
    params.gaming = p.gaming;
    amount.setTarget (params.amount);
}

void SmoothnessGuard::controlTick() noexcept FLUB_NONBLOCKING
{
    const float a = amount.next();
    float targetDb = 0.0f;
    if (enabledForRate && a > 0.0f && rBandPow + rBodyPow > kSilencePower && pBand + pBody > kSilencePower)
    {
        constexpr double tiny = 1.0e-20;
        const float ratioX = static_cast<float> (10.0 * std::log10 ((pBand + tiny) / (pBody + tiny)));
        const float ratioR = static_cast<float> (10.0 * std::log10 ((rBandPow + tiny) / (rBodyPow + tiny)));
        const float threshold = params.gaming ? kGamingThresholdDb : kThresholdDb;
        const float maxCut = params.gaming ? kGamingMaxCutDb : kMaxCutDb;
        targetDb = -std::min (maxCut, a * softKnee (ratioX - std::max (ratioR, threshold)));
    }
    float db = cut.process (targetDb);
    if (targetDb == 0.0f && db > -kSettledDb)
    {
        cut.reset (0.0f);
        db = 0.0f;
    }
    // G glides linearly from its value now to the new one across the next interval.
    gainStart = gain;
    gainStep = (dbToGain (db) - gain) / static_cast<float> (kControlInterval);
}

void SmoothnessGuard::process (const AudioBlock& block) noexcept FLUB_NONBLOCKING
{
    const int n = block.numSamples;
    const int nch = std::min (block.numChannels, 2);
    const bool withReference = hasReference && reference.numSamples >= n && reference.numChannels >= 1;
    const int rch = withReference ? std::min (reference.numChannels, nch) : 0;
    hasReference = false;
    float deepest = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        // Reference: delayed to line up with the input, then its detectors.
        double rh = 0.0, rb = 0.0;
        for (int c = 0; c < rch; ++c)
        {
            const float r = refDelay.processSample (c, reference.channel (c)[i]);
            float v1 = 0.0f, v2 = 0.0f;
            svfTickRaw (band, rBand[static_cast<size_t> (c)], r, v1, v2);
            const float h = bandK * v1;
            svfTickRaw (body, rBody[static_cast<size_t> (c)], r, v1, v2);
            const float b = bodyK * v1;
            rh += static_cast<double> (h) * h;
            rb += static_cast<double> (b) * b;
        }
        if (rch > 0)
            refDelay.advance();
        rBandPow += powerCoeff * (rh - rBandPow);
        rBodyPow += powerCoeff * (rb - rBodyPow);

        // Input: detectors, and the cut on its sibilant band (the same band-pass).
        gain = gainStart + gainStep * static_cast<float> (controlPhase + 1);
        const float g1 = gain - 1.0f;
        double xh = 0.0, xb = 0.0;
        for (int c = 0; c < nch; ++c)
        {
            float* d = block.channel (c);
            float v1 = 0.0f, v2 = 0.0f;
            svfTickRaw (body, xBody[static_cast<size_t> (c)], d[i], v1, v2);
            const float b = bodyK * v1;
            svfTickRaw (band, xBand[static_cast<size_t> (c)], d[i], v1, v2);
            const float h = bandK * v1;
            xh += static_cast<double> (h) * h;
            xb += static_cast<double> (b) * b;
            if (g1 != 0.0f)
                d[i] += g1 * h;
        }
        pBand += powerCoeff * (xh - pBand);
        pBody += powerCoeff * (xb - pBody);
        deepest = std::min (deepest, g1);

        ++controlPhase;
        if (--controlCountdown == 0)
        {
            controlCountdown = kControlInterval;
            controlPhase = 0;
            controlTick();
        }
    }
    blockCutDb = deepest < 0.0f ? gainToDb (1.0f + deepest) : 0.0f;
}
} // namespace flub
