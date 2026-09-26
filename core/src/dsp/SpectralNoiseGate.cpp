#include "flub/dsp/SpectralNoiseGate.h"

#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace flub
{
namespace
{
constexpr int kMinFftSize = 256;
constexpr int kMaxFftSize = 4096;
constexpr int kDefaultFftSize = 512;

// P_k is smoothed over ~10 ms: long enough to take the edge off the frame to
// frame variance of a single periodogram, short enough that transients still
// open the gate on time. With long hops (fftSize >= 2048 at 48 kHz) 10 ms would
// be less than one hop, i.e. no smoothing at all, and the raw periodogram of
// plain noise (exponentially distributed, 2 degrees of freedom) would keep
// poking through the threshold; there the time constant is two hops, still
// only half the frame length that already limits the time resolution.
constexpr double kPowerTimeSeconds = 0.010;
constexpr double kPowerTimeHops = 2.0;

// After the analysis window becomes valid (reset, or the end of a digital
// silence) the floor waits ~3 power time constants (P_k within 5 %) before it
// adapts, so it never learns the half-settled power.
constexpr double kWarmupTimeConstants = 3.0;

// The first profile is a single snapshot of P_k: per bin it is off by several dB
// (P_k of noise still fluctuates). The minimum tracker corrects a floor that is
// too high at once, one that is too low only at floorRiseDbPerSec, so the
// snapshot is bias-scaled like every later candidate and starts high. It then
// needs a few hundred ms of material to settle onto the lower envelope; for
// that long the floor keeps adapting even with freezeFloor on, otherwise a
// gate that is frozen from the start would hold the raw snapshot for good
// (permanently open "musical noise" bins, or a floor B dB too high).
constexpr double kLearnSeconds = 1.0;

// thresholdDb / reductionDb glide over 20 ms (hop-rate one-pole), so moving
// either never steps every bin's gain at once.
constexpr double kParamSmoothSeconds = 0.020;

// Width of the soft gate transition, centred on the threshold: the gain goes
// from G_min at (threshold - 3 dB) to unity at (threshold + 3 dB), linear in dB.
constexpr float kTransitionDb = 6.0f;

// A hop whose samples all stay below -140 dBFS (under the 24-bit LSB) is
// digital silence: gaps between tracks or a stopped stream must not wipe the
// learned noise profile, so the floor holds while any such hop is inside the
// analysis window.
constexpr float kSilenceLevel = 1.0e-7f;

// Keeps every power strictly positive (-200 dB, far below any real noise floor)
// so ratios and logs stay finite and the smoothers never go subnormal.
constexpr float kPowerOffset = 1.0e-20f;
// A frame whose power is not below this (NaN / Inf / absurd input) is skipped,
// so the per-bin state can neither overflow nor be poisoned.
constexpr float kMaxPower = 1.0e30f;
// P_k / N_k is clamped to +-200 dB: far beyond both ends of the gate curve.
constexpr float kMinRatio = 1.0e-20f;
constexpr float kMaxRatio = 1.0e20f;

// Gains closer than this to their target snap onto it: an open bin reaches
// exactly 0 dB (unity), keeping the transparent case bit-accurate.
constexpr float kGainSnapDb = 1.0e-3f;
constexpr float kParamSnap = 1.0e-4f;

constexpr float kDbToLnGain = 0.11512925464970228f; // ln(10) / 20

// ---- noise-floor bias compensation ------------------------------------------
// A minimum tracker ("instantly down, at most r dB/s up") on a fluctuating power
// estimate settles on the LOWER envelope of that estimate, not on its mean. How
// far below depends on how much P_k fluctuates (its equivalent degrees of
// freedom, set by the hop length relative to the power time constant) and on
// how fast the floor may climb back after a dip. Uncompensated, the "floor" would
// sit 2 dB (short hops, fast rise) to 18 dB (long hops, slow rise) under the
// actual noise, and "threshold 6 dB above the floor" would open the gate on
// plain noise.
//
// The table holds that offset (dB) for stationary Gaussian white noise,
// measured by simulating exactly this estimator (sqrt-Hann, hop = N/4, P_k with
// the time constant powerTimeSeconds(), the tracker below) for 400 s (100 s
// discarded) and averaging 10 log10 (tracked minimum / true mean power) over
// all bins and frames. Only the hop length and the rise rate matter: the
// per-bin statistics of white noise do not depend on N itself.
// Rows: hop length 0.3125 .. 40 ms (log2 grid anchored at 5 ms, where the power
// time constant switches from 10 ms to two hops, so the kink lies on a row).
// Columns: floorRiseDbPerSec 0.5, 1, 2, 4, 8, 16, 32 (log2 grid). Real settings
// span hop 0.33 ms (256 @ 192 kHz) .. 23.2 ms (4096 @ 44.1 kHz) and rise
// 0.5 .. 20 dB/s; values in between are bilinear in (log2 hop, log2 rise).
// Scaling the tracker input by this offset makes N_k an estimate of the mean
// noise power, so thresholdDb means "dB above the noise" for every
// fftSize / sample-rate / rise-rate combination.
constexpr int kBiasHopSteps = 8;
constexpr int kBiasRiseSteps = 7;
constexpr double kBiasFirstHopSeconds = 0.3125e-3;
constexpr float kBiasFirstRise = 0.5f;
constexpr float kFloorBiasDb[kBiasHopSteps][kBiasRiseSteps] = {
    { 2.34f, 2.14f, 1.92f, 1.67f, 1.41f, 1.14f, 0.85f }, // 0.3125 ms
    { 3.35f, 3.08f, 2.78f, 2.45f, 2.10f, 1.72f, 1.33f }, // 0.625 ms
    { 4.76f, 4.38f, 3.98f, 3.53f, 3.06f, 2.54f, 2.00f }, // 1.25 ms
    { 6.66f, 6.15f, 5.60f, 5.00f, 4.35f, 3.66f, 2.92f }, // 2.5 ms
    { 9.14f, 8.44f, 7.70f, 6.89f, 6.01f, 5.07f, 4.09f }, // 5 ms
    { 8.43f, 7.68f, 6.87f, 6.00f, 5.07f, 4.08f, 3.09f }, // 10 ms
    { 7.69f, 6.88f, 6.01f, 5.07f, 4.09f, 3.09f, 2.17f }, // 20 ms
    { 6.91f, 6.02f, 5.08f, 4.09f, 3.09f, 2.17f, 1.43f }, // 40 ms
};

float floorBiasDb (double hopSec, float riseDbPerSec) noexcept
{
    const float x = std::clamp (static_cast<float> (std::log2 (hopSec / kBiasFirstHopSeconds)),
                                0.0f, static_cast<float> (kBiasHopSteps - 1));
    const float y = std::clamp (std::log2 (riseDbPerSec / kBiasFirstRise), 0.0f, static_cast<float> (kBiasRiseSteps - 1));
    const int x0 = std::min (static_cast<int> (x), kBiasHopSteps - 2);
    const int y0 = std::min (static_cast<int> (y), kBiasRiseSteps - 2);
    const float fx = x - static_cast<float> (x0);
    const float fy = y - static_cast<float> (y0);
    const float lo = lerp (kFloorBiasDb[x0][y0], kFloorBiasDb[x0][y0 + 1], fy);
    const float hi = lerp (kFloorBiasDb[x0 + 1][y0], kFloorBiasDb[x0 + 1][y0 + 1], fy);
    return lerp (lo, hi, fx);
}

double powerTimeSeconds (double hopSec) noexcept
{
    return std::max (kPowerTimeSeconds, kPowerTimeHops * hopSec);
}

int validFftSize (int size) noexcept
{
    return isPowerOfTwo (size) && size >= kMinFftSize && size <= kMaxFftSize ? size : kDefaultFftSize;
}

float sanitise (float v, float lo, float hi, float fallback) noexcept
{
    return std::isfinite (v) ? std::clamp (v, lo, hi) : fallback;
}

/** Per-hop one-pole coefficient for a time constant in seconds. */
float hopCoeff (double hopSec, double timeSec) noexcept
{
    return timeSec > 0.0 ? static_cast<float> (std::exp (-hopSec / timeSec)) : 0.0f;
}

float smoothTowards (float current, float target, float coeff, float snap) noexcept
{
    const float v = target + coeff * (current - target);
    return std::abs (v - target) < snap ? target : v;
}
} // namespace

//==============================================================================
void SpectralNoiseGate::prepare (const ProcessSpec& newSpec)
{
    spec = newSpec;
    spec.numChannels = std::clamp (spec.numChannels, 1, kMaxChannels);
    if (! (spec.sampleRate > 0.0))
        spec.sampleRate = 48000.0;

    frameSize = validFftSize (fftSize);
    hopSize = frameSize / 4;
    numBins = frameSize / 2 + 1;
    numChannels = spec.numChannels;
    hopSeconds = static_cast<double> (hopSize) / spec.sampleRate;

    const size_t n = static_cast<size_t> (frameSize);
    const size_t nb = static_cast<size_t> (numBins);

    fft.prepare (frameSize);

    // sqrt-Hann (periodic) = sin(pi n / N). Analysis * synthesis = Hann, and four
    // Hann windows at hop N/4 sum to exactly 2, so the synthesis window carries
    // the 1/2 that makes unity gain a perfect reconstruction.
    analysisWindow.resize (n);
    synthesisWindow.resize (n);
    for (size_t i = 0; i < n; ++i)
    {
        const double w = std::sin (kPi * static_cast<double> (i) / static_cast<double> (frameSize));
        analysisWindow[i] = static_cast<float> (w);
        synthesisWindow[i] = static_cast<float> (0.5 * w);
    }
    powerNorm = 2.0f / static_cast<float> (frameSize); // sum of sin^2 over the frame = N / 2

    frame.assign (n, 0.0f);
    gains.assign (nb, 1.0f);
    snr.assign (nb, 1.0f);
    bins.assign (nb, Fft::Complex {});

    for (int c = 0; c < kMaxChannels; ++c)
    {
        auto& st = channels[static_cast<size_t> (c)];
        const bool used = c < numChannels;
        st.input.assign (used ? n : 0u, 0.0f);
        st.output.assign (used ? n : 0u, 0.0f);
        st.power.assign (used ? nb : 0u, 0.0f);
        st.noiseFloor.assign (used ? nb : 0u, 1.0f);
        st.gainDb.assign (used ? nb : 0u, 0.0f);
    }

    const double powerTime = powerTimeSeconds (hopSeconds);
    powerCoeff = hopCoeff (hopSeconds, powerTime);
    paramCoeff = hopCoeff (hopSeconds, kParamSmoothSeconds);
    warmupFrames = std::max (1, static_cast<int> (std::ceil (kWarmupTimeConstants * powerTime / hopSeconds)));
    learnPeriodFrames = std::max (1, static_cast<int> (std::ceil (kLearnSeconds / hopSeconds)));

    prepared = true;
    updateCoefficients();
    reset();
}

void SpectralNoiseGate::reset() noexcept
{
    if (! prepared)
        return;
    for (int c = 0; c < numChannels; ++c)
        clearChannel (channels[static_cast<size_t> (c)]);
    activeChannels = numChannels;
    hopPos = 0;
    thresholdCur = params.thresholdDb;
    reductionCur = params.reductionDb;
}

int SpectralNoiseGate::latencySamples() const noexcept
{
    // An input sample is covered by four frames; the last of them is analysed
    // at most fftSize - 1 samples later. Each output sample is read from the
    // accumulator before the incoming sample is analysed, which adds one more:
    // the delay is exactly fftSize for every sample, independent of hop phase.
    return prepared ? frameSize : validFftSize (fftSize);
}

//==============================================================================
void SpectralNoiseGate::setParams (const NoiseGateParams& p) noexcept
{
    NoiseGateParams q = p;
    q.thresholdDb = sanitise (q.thresholdDb, 0.0f, 20.0f, params.thresholdDb);
    q.reductionDb = sanitise (q.reductionDb, 0.0f, 40.0f, params.reductionDb);
    q.attackMs = sanitise (q.attackMs, 1.0f, 50.0f, params.attackMs);
    q.releaseMs = sanitise (q.releaseMs, 10.0f, 500.0f, params.releaseMs);
    q.floorRiseDbPerSec = sanitise (q.floorRiseDbPerSec, 0.5f, 20.0f, params.floorRiseDbPerSec);

    if (q == params)
        return; // the chain pushes every block; unchanged values cost nothing

    params = q;
    if (prepared)
        updateCoefficients();
    else
    {
        thresholdCur = params.thresholdDb;
        reductionCur = params.reductionDb;
    }
}

void SpectralNoiseGate::updateCoefficients() noexcept
{
    // Everything below runs once per hop, so every time constant is converted
    // with the hop length, not the sample period.
    attackCoeff = hopCoeff (hopSeconds, static_cast<double> (params.attackMs) * 0.001);
    releaseCoeff = hopCoeff (hopSeconds, static_cast<double> (params.releaseMs) * 0.001);

    // Upward floor rate: r dB/s -> r * hop dB per frame, as a power factor.
    riseFactor = static_cast<float> (std::pow (10.0, static_cast<double> (params.floorRiseDbPerSec) * hopSeconds * 0.1));
    floorBias = std::pow (10.0f, 0.1f * floorBiasDb (hopSeconds, params.floorRiseDbPerSec));
}

void SpectralNoiseGate::clearChannel (ChannelState& st) noexcept
{
    std::fill (st.input.begin(), st.input.end(), 0.0f);
    std::fill (st.output.begin(), st.output.end(), 0.0f);
    std::fill (st.power.begin(), st.power.end(), kPowerOffset);
    std::fill (st.noiseFloor.begin(), st.noiseFloor.end(), 1.0f);
    std::fill (st.gainDb.begin(), st.gainDb.end(), 0.0f); // start open: nothing learned yet
    st.hopPeak = 0.0f;
    st.silentHops = 0xFu; // the samples before the reset are unknown (zeros in the FIFO)
    st.holdFrames = warmupFrames;
    st.learnFrames = learnPeriodFrames;
    st.floorValid = false;
}

//==============================================================================
void SpectralNoiseGate::process (const AudioBlock& block) noexcept
{
    const int nch = std::min (block.numChannels, numChannels);
    if (! prepared || nch <= 0)
        return;

    // A channel that was absent from previous blocks has stale FIFOs; it
    // restarts from a clean state (its history is silence to us).
    for (int c = activeChannels; c < nch; ++c)
        clearChannel (channels[static_cast<size_t> (c)]);
    activeChannels = nch;

    // Stream through the FIFOs one hop segment at a time. Frames are processed
    // at absolute hop boundaries, so the result is identical for any host
    // block size.
    int pos = 0;
    while (pos < block.numSamples)
    {
        const int n = std::min (block.numSamples - pos, hopSize - hopPos);
        const size_t inOffset = static_cast<size_t> (frameSize - hopSize + hopPos);
        const size_t outOffset = static_cast<size_t> (hopPos);

        for (int c = 0; c < nch; ++c)
        {
            auto& st = channels[static_cast<size_t> (c)];
            float* io = block.channel (c) + pos;
            float* in = st.input.data() + inOffset;
            const float* out = st.output.data() + outOffset;
            float peak = st.hopPeak;
            for (int i = 0; i < n; ++i)
            {
                const float x = io[i];
                in[i] = x;
                peak = std::max (peak, std::abs (x));
                io[i] = out[i];
            }
            st.hopPeak = peak;
        }

        hopPos += n;
        pos += n;

        if (hopPos == hopSize)
        {
            hopPos = 0;
            thresholdCur = smoothTowards (thresholdCur, params.thresholdDb, paramCoeff, kParamSnap);
            reductionCur = smoothTowards (reductionCur, params.reductionDb, paramCoeff, kParamSnap);
            for (int c = 0; c < nch; ++c)
                processFrame (channels[static_cast<size_t> (c)]);
        }
    }
}

void SpectralNoiseGate::processFrame (ChannelState& st) noexcept
{
    const int n = frameSize;
    const int hop = hopSize;
    const size_t tail = static_cast<size_t> (n - hop);

    // ---- window validity / floor adaptation gate ----
    st.silentHops = ((st.silentHops << 1) | (st.hopPeak < kSilenceLevel ? 1u : 0u)) & 0xFu;
    st.hopPeak = 0.0f;

    bool adaptFloor = false;
    if (st.silentHops != 0)
        st.holdFrames = warmupFrames; // silence (or pre-reset zeros) inside the window
    else if (st.holdFrames > 0)
        --st.holdFrames;               // P_k still settling on the new material
    else
        adaptFloor = ! params.freezeFloor || st.learnFrames > 0; // freeze holds the profile once learned

    const bool firstProfile = adaptFloor && ! st.floorValid;
    if (adaptFloor)
    {
        st.floorValid = true;
        if (st.learnFrames > 0)
            --st.learnFrames;
    }
    const bool gating = st.floorValid;

    // ---- analysis ----
    float* fr = frame.data();
    const float* in = st.input.data();
    const float* wa = analysisWindow.data();
    for (int i = 0; i < n; ++i)
        fr[i] = in[i] * wa[i];
    std::memmove (st.input.data(), st.input.data() + hop, sizeof (float) * tail);

    fft.forwardReal (fr, bins.data());

    // ---- per-bin statistics ----
    float* pw = st.power.data();
    float* nf = st.noiseFloor.data();
    float* sn = snr.data();

    for (int k = 0; k < numBins; ++k)
    {
        // A frame poisoned by NaN / Inf (or absurdly large) input leaves the
        // statistics as they were instead of pinning P_k at a huge value that
        // would take ~1 s to decay (gate wide open) and drag the floor up.
        float& pk = pw[k];
        const float p = std::norm (bins[static_cast<size_t> (k)]) * powerNorm + kPowerOffset;
        if (p < kMaxPower)
            pk = p + powerCoeff * (pk - p);

        // Noise floor: minimum tracking of the (bias-scaled) power. It drops to
        // any lower value at once, but climbs at most riseFactor per hop, so a
        // note or a voice never "becomes" the floor while slow hum / hiss drift
        // is followed. The first profile is the (bias-scaled) candidate itself:
        // starting high, the instant downward path settles it within the
        // learning period.
        float& nk = nf[k];
        if (adaptFloor)
        {
            const float candidate = pk * floorBias;
            nk = firstProfile || candidate < nk ? candidate : std::min (candidate, nk * riseFactor);
        }

        sn[k] = std::clamp (pk / nk, kMinRatio, kMaxRatio);
    }

    // ---- gate gain ----
    // The spectrum of a real frame is Hermitian, so the neighbours of DC and
    // Nyquist are their mirror images (bins 1 and N/2 - 1).
    const int last = numBins - 1;
    const float lowerDb = thresholdCur - 0.5f * kTransitionDb;
    const float red = reductionCur;
    float* gd = st.gainDb.data();
    float* gl = gains.data();

    for (int k = 0; k <= last; ++k)
    {
        // Soft gate on the 3-bin average of P_k / N_k: G_min below
        // (threshold - 3 dB), unity above (threshold + 3 dB), linear in dB.
        // Averaging the ratio (not the power) keeps a stationary hum line from
        // holding its neighbours open, while cutting the variance of plain
        // noise by ~3x so far fewer noise bins poke through the threshold.
        float targetDb = 0.0f;
        if (gating)
        {
            const float ratio = (sn[k == 0 ? 1 : k - 1] + sn[k] + sn[k == last ? last - 1 : k + 1]) / 3.0f;
            const float levelDb = 10.0f * std::log10 (ratio);
            const float t = std::clamp ((levelDb - lowerDb) * (1.0f / kTransitionDb), 0.0f, 1.0f);
            targetDb = -red * (1.0f - t);
        }

        // Opening (gain rising) uses the attack time, closing the release time:
        // bins open fast on onsets and fade out slowly, which masks the random
        // on/off flicker of noise bins ("musical noise").
        float& g = gd[k];
        g = smoothTowards (g, targetDb, targetDb > g ? attackCoeff : releaseCoeff, kGainSnapDb);
        gl[k] = std::exp (g * kDbToLnGain);
    }

    // ---- 3-bin average of the applied gain ----
    // A jagged gain curve is a long filter kernel that wraps around the frame
    // (time aliasing); averaging neighbouring gains keeps it short and spreads
    // any isolated open bin, which removes the "tonal" character of musical
    // noise. Unity everywhere stays exactly unity ((1 + 1 + 1) / 3 == 1).
    Fft::Complex* x = bins.data();
    for (int k = 0; k <= last; ++k)
    {
        const float below = gl[k == 0 ? 1 : k - 1];
        const float above = gl[k == last ? last - 1 : k + 1];
        x[k] *= (below + gl[k] + above) / 3.0f;
    }

    // ---- synthesis and overlap-add ----
    fft.inverseReal (x, fr);

    float* acc = st.output.data();
    std::memmove (acc, acc + hop, sizeof (float) * tail);
    std::fill (acc + tail, acc + n, 0.0f);
    const float* ws = synthesisWindow.data();
    for (int i = 0; i < n; ++i)
        acc[i] += fr[i] * ws[i];
}
} // namespace flub
