#include "flub/dsp/SmoothnessGuard.h"

#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <initializer_list>

namespace flub
{
namespace
{
constexpr float kParamSmoothMs = 20.0f;
constexpr double kSilencePower = 1.0e-10; // -100 dB: no reference, no cut
constexpr float kSettledDb = 1.0e-4f;     // a cut this close to 0 dB is 0 dB
} // namespace

void SmoothnessGuard::prepare (const ProcessSpec& spec)
{
    sr = spec.sampleRate;
    enabledForRate = sr >= kMinSampleRate;
    band = SvfCoeffs::make (FilterType::BandPass, kBandHz, kBandQ, 0.0, sr);
    constexpr double butterworth = 0.70710678118654752;
    bodyHp = SvfCoeffs::make (FilterType::HighPass, kBodyLowHz, butterworth, 0.0, sr);
    bodyLp = SvfCoeffs::make (FilterType::LowPass, kBodyHighHz, butterworth, 0.0, sr);
    // BandPass mixes m1 = k: the unity-peak band-pass is k v1.
    bandK = band.m1;
    // G per band cut (see the header comment): the mean of |1 + (G - 1) BP|^2
    // over 5 - 10 kHz (flat power, 64 points), bisected in dB of G.
    std::array<std::complex<double>, 64> bp {};
    const double top = std::min (10000.0, 0.45 * sr);
    for (size_t i = 0; i < bp.size(); ++i)
        bp[i] = band.response (5000.0 + (top - 5000.0) * (static_cast<double> (i) + 0.5) / static_cast<double> (bp.size()), sr);
    const auto bandCutDb = [&bp] (double g) {
        double sum = 0.0;
        for (const auto& h : bp)
            sum += std::norm (1.0 + (g - 1.0) * h);
        return -10.0 * std::log10 (sum / static_cast<double> (bp.size()));
    };
    for (int i = 0; i < kTableSize; ++i)
    {
        const double want = static_cast<double> (kTableStepDb) * i;
        double lo = kMinGainDb, hi = 0.0; // dB of G; the band cut grows as G falls
        for (int it = 0; it < 40; ++it)
        {
            const double mid = 0.5 * (lo + hi);
            (bandCutDb (std::pow (10.0, mid / 20.0)) < want ? hi : lo) = mid;
        }
        gainTable[static_cast<size_t> (i)] = static_cast<float> (std::pow (10.0, 0.5 * (lo + hi) / 20.0));
    }
    gainTable[0] = 1.0f;
    refDelay.prepare (spec.numChannels, referenceDelay);
    stagePower.assign (static_cast<size_t> (std::max (1, spec.maxBlockSize) + downstreamDelay + 1), 0.0f);
    powerCoeff = 1.0 - static_cast<double> (onePoleCoeff (kDetectorMs, sr));
    longCoeff = 1.0 - static_cast<double> (onePoleCoeff (kBodyLongMs, sr));
    const double controlRate = sr / kControlInterval;
    amount.reset (controlRate, kParamSmoothMs, params.amount);
    cut.prepare (controlRate, kAttackMs, kReleaseMs, false);
    reset();
}

void SmoothnessGuard::reset() noexcept FLUB_NONBLOCKING
{
    for (auto* states : { &xBand, &rBand, &yBand, &oBand })
        for (auto& s : *states)
            s.reset();
    for (auto* states : { &xBody, &rBody, &oBody })
        for (auto& channel : *states)
            for (auto& s : channel)
                s.reset();
    refDelay.reset();
    pBand = pBody = rBandPow = rBodyPow = 0.0;
    std::fill (stagePower.begin(), stagePower.end(), 0.0f);
    stageWrite = lastBlock = 0;
    hasDownstream = false;
    qStage = qOut = oBodyLong = rBodyLong = 0.0;
    downstreamGainDb = 0.0f;
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

float SmoothnessGuard::gainForBandCut (float bandCutDb) const noexcept FLUB_NONBLOCKING
{
    const float x = std::clamp (bandCutDb / kTableStepDb, 0.0f, static_cast<float> (kTableSize - 1));
    const int i = std::min (static_cast<int> (x), kTableSize - 2);
    const float t = x - static_cast<float> (i);
    return gainTable[static_cast<size_t> (i)] + t * (gainTable[static_cast<size_t> (i + 1)] - gainTable[static_cast<size_t> (i)]);
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
        float excess = ratioX - std::max (ratioR, threshold);
        if (hasDownstream && oBodyLong > kSilencePower && rBodyLong > kSilencePower)
        {
            // Held to the programme's body lift at the output (header comment).
            const float liftH = static_cast<float> (10.0 * std::log10 ((pBand + tiny) / (rBandPow + tiny)));
            const float liftB = static_cast<float> (10.0 * std::log10 (oBodyLong / rBodyLong));
            excess = std::min (ratioX - threshold, liftH + downstreamGainDb - liftB);
        }
        targetDb = -std::min (maxCut, a * std::max (0.0f, excess));
    }
    cutDb = cut.process (targetDb);
    if (targetDb == 0.0f && cutDb > -kSettledDb)
    {
        cut.reset (0.0f);
        cutDb = 0.0f;
    }
    // G glides linearly from its value now to the new one across the next interval.
    gainStart = gain;
    gainStep = (gainForBandCut (-cutDb) - gain) / static_cast<float> (kControlInterval);
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
            auto& rb2 = rBody[static_cast<size_t> (c)];
            const float b = svfTick (bodyLp, rb2[1], svfTick (bodyHp, rb2[0], r));
            rh += static_cast<double> (h) * h;
            rb += static_cast<double> (b) * b;
        }
        if (rch > 0)
            refDelay.advance();
        rBandPow += powerCoeff * (rh - rBandPow);
        rBodyPow += powerCoeff * (rb - rBodyPow);
        rBodyLong += longCoeff * (rb - rBodyLong);

        // Input: detectors, and the cut on its sibilant band (the same band-pass).
        gain = gainStart + gainStep * static_cast<float> (controlPhase + 1);
        const float g1 = gain - 1.0f;
        double xh = 0.0, xb = 0.0, yh = 0.0;
        for (int c = 0; c < nch; ++c)
        {
            float* d = block.channel (c);
            auto& xb2 = xBody[static_cast<size_t> (c)];
            const float b = svfTick (bodyLp, xb2[1], svfTick (bodyHp, xb2[0], d[i]));
            float v1 = 0.0f, v2 = 0.0f;
            svfTickRaw (band, xBand[static_cast<size_t> (c)], d[i], v1, v2);
            const float h = bandK * v1;
            xh += static_cast<double> (h) * h;
            xb += static_cast<double> (b) * b;
            if (g1 != 0.0f)
                d[i] += g1 * h;
            float w1 = 0.0f, w2 = 0.0f;
            svfTickRaw (band, yBand[static_cast<size_t> (c)], d[i], w1, w2);
            yh += static_cast<double> (bandK * w1) * (bandK * w1);
        }
        pBand += powerCoeff * (xh - pBand);
        pBody += powerCoeff * (xb - pBody);
        stagePower[static_cast<size_t> (stageWrite)] = static_cast<float> (yh);
        if (++stageWrite == static_cast<int> (stagePower.size()))
            stageWrite = 0;
        deepest = std::min (deepest, cutDb);

        ++controlPhase;
        if (--controlCountdown == 0)
        {
            controlCountdown = kControlInterval;
            controlPhase = 0;
            controlTick();
        }
    }
    blockCutDb = deepest;
    lastBlock = n;
}

void SmoothnessGuard::processDownstream (const AudioBlock& out) noexcept FLUB_NONBLOCKING
{
    const int size = static_cast<int> (stagePower.size());
    const int n = std::min ({ out.numSamples, lastBlock, size - 1 - downstreamDelay });
    const int nch = std::min (out.numChannels, 2);
    lastBlock = 0;
    if (n <= 0 || nch <= 0 || ! enabledForRate)
        return;
    // The stage's output for these samples was written in the last
    // process(); the output's sample i is its sample i - downstreamDelay.
    int read = stageWrite - n - downstreamDelay;
    while (read < 0)
        read += size;
    for (int i = 0; i < n; ++i)
    {
        double oh = 0.0, ob = 0.0;
        for (int c = 0; c < nch; ++c)
        {
            const float x = out.channel (c)[i];
            float v1 = 0.0f, v2 = 0.0f;
            svfTickRaw (band, oBand[static_cast<size_t> (c)], x, v1, v2);
            const float h = bandK * v1;
            auto& ob2 = oBody[static_cast<size_t> (c)];
            const float b = svfTick (bodyLp, ob2[1], svfTick (bodyHp, ob2[0], x));
            oh += static_cast<double> (h) * h;
            ob += static_cast<double> (b) * b;
        }
        qStage += powerCoeff * (static_cast<double> (stagePower[static_cast<size_t> (read)]) - qStage);
        qOut += powerCoeff * (oh - qOut);
        oBodyLong += longCoeff * (ob - oBodyLong);
        if (++read == size)
            read = 0;
    }
    hasDownstream = true;
    downstreamGainDb = qStage > kSilencePower && qOut > kSilencePower
                           ? std::clamp (static_cast<float> (10.0 * std::log10 (qOut / qStage)), -kDownstreamRangeDb, kDownstreamRangeDb)
                           : 0.0f;
}
} // namespace flub
