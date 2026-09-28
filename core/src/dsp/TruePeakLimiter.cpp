#include "flub/dsp/TruePeakLimiter.h"

#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>

namespace flub
{
namespace
{
// Required gain is computed against a threshold 0.05 dB below the ceiling.
// It absorbs the residual error of the 4x interpolator (a peak that falls
// between two of its phases is under-read by a few hundredths of a dB for
// programme material) and float rounding in the gain envelope.
constexpr float kMarginLin = 0.99426007f; // 10^(-0.05 / 20)

// True-peak mode holds the reduced gain for this many extra samples on each
// side of a peak (Kh below), so the gain is flat under the central part of
// the interpolator that reads the peak. Without it the attack ramp is still
// falling across the kernel's +-20 taps when the peak arrives, and on dense
// clipped programme the output's true peak (read by the same interpolator)
// came out up to ~0.05 dB above the ceiling. 8 taps each side carry
// practically all of the kernel's weight: with Kh = 8 the worst case measured
// on hard-clipped noise + tones at 44.1 / 48 / 96 kHz and 0.5 .. 2 ms
// look-ahead is 0.04 dB below the ceiling. Kh never exceeds L / 3, so the
// attack ramp (L - Kh + 1 samples) keeps at least two thirds of the look-ahead.
constexpr int kTruePeakHold = 8;

// Look-ahead is structural; 10 ms is far beyond any profile (0.5 .. 2 ms) and
// bounds the ring sizes (1921 samples at 192 kHz).
constexpr float kMaxLookaheadMs = 10.0f;

// The ceiling glides linearly in dB, so a preset change or a macro move of
// the ceiling while limiting is a smooth gain change, not a step.
constexpr float kCeilingSmoothMs = 50.0f;

// Auto release: an isolated peak releases at releaseMs * kFastFraction; once
// the current limiting run has spanned kBlendEndMs the release is releaseMs.
// Between kBlendStartMs and kBlendEndMs the one-pole coefficient is blended,
// so the release speed never switches abruptly.
constexpr float kFastFraction = 0.2f;
constexpr float kBlendStartMs = 25.0f;
constexpr float kBlendEndMs = 50.0f;

// Overs closer together than this belong to one continuous run: 25 ms is
// half a period of 20 Hz, so a bass note that pokes over the ceiling on
// every half-cycle counts as continuous limiting (slow, low-distortion
// release) rather than a string of isolated peaks (fast release that would
// modulate the waveform).
constexpr float kRunGapMs = 25.0f;

// A run is forgotten only once the gain has recovered to within 0.1 dB, so
// dense material that never fully recovers keeps the slow release.
constexpr float kRecoveredGain = 0.98855309f; // 10^(-0.1 / 20)

// The released gain lands exactly on the envelope once this close, so a
// recovered limiter is bit-transparent (y = x delayed).
constexpr double kLand = 1.0e-7;

// LF-safe envelope (docs/11 E05 stage 1, LimiterEnvelope). Period hold:
// the gain holds after each peak for the spacing of the recent peaks (+1/8),
// so a periodic waveform's gain stays flat from one peak to the next instead
// of releasing in between (at least half a period of the lowest frequency
// that sets the peaks), up to kMaxHoldMs (half a period of 20 Hz). A peak is
// a run of overs (r < 1) that starts more than kPeakGapMs after the last
// over; an isolated peak (none within kMaxHoldMs before it) gets no hold,
// so transients and dense broadband programme are not held (a fixed 10 ms
// hold on every peak, docs/11 E05's first proposal, cost 1-2 LU on
// kick-heavy programme and lengthened every duck).
constexpr float kMaxHoldMs = 25.0f;
constexpr float kPeakGapMs = 0.5f;
constexpr float kProgramAttackMs = 150.0f;
constexpr float kProgramReleaseMs = 800.0f;

float sanitise (float v, float lo, float hi, float fallback) noexcept
{
    return std::isfinite (v) ? std::clamp (v, lo, hi) : fallback;
}
} // namespace

//==============================================================================
void TruePeakLimiter::RefinedPeakDetector::prepare (int numChannels)
{
    // The very same interpolator as TruePeakDetector (and so as the meters):
    // shared design function, same taps, same kDelay. Phase 0 is the delayed
    // input sample itself.
    TruePeakDetector::designPhaseTaps (phaseTaps);
    for (int c = 0; c < kMaxChannels; ++c)
        history[static_cast<size_t> (c)].assign (c < numChannels ? static_cast<size_t> (2 * kTaps) : 0u, 0.0f);
    reset();
}

void TruePeakLimiter::RefinedPeakDetector::reset() noexcept
{
    for (auto& h : history)
        std::fill (h.begin(), h.end(), 0.0f);
    pos.fill (0);
    lastPhase.fill (0.0f);
}

float TruePeakLimiter::RefinedPeakDetector::processSample (int ch, float x) noexcept
{
    const auto c = static_cast<size_t> (ch);
    auto& h = history[c];
    int& p = pos[c];
    p = (p == 0 ? kTaps - 1 : p - 1);
    h[static_cast<size_t> (p)] = x;
    h[static_cast<size_t> (p + kTaps)] = x;
    const float* w = h.data() + p; // w[j] = x[n - j]

    // |z| on the 4x grid around the interval: a[0] = n-D-1/4 (last call's
    // phase 3), a[1..4] = n-D + {0, 1/4, 1/2, 3/4}, a[5] = n-D+1 (= x[n-D+1],
    // already in the history, so refining costs no extra delay).
    std::array<float, 6> a {};
    a[0] = lastPhase[c];
    a[1] = std::abs (w[kDelay]);
    for (int ph = 1; ph < kPhases; ++ph)
    {
        const float* t = phaseTaps[static_cast<size_t> (ph)].data();
        float acc = 0.0f;
        for (int j = 0; j < kTaps; ++j)
            acc += t[j] * w[j];
        a[static_cast<size_t> (ph + 1)] = std::abs (acc);
    }
    a[5] = std::abs (w[kDelay - 1]);
    lastPhase[c] = a[4];

    // Plain grid maximum, then the parabolic vertex of every local maximum.
    // For a local maximum |a0 - a2| <= -den, so the correction is bounded
    // (<= a1 / 4) and can only raise the estimate (conservative).
    float peak = std::max ({ a[1], a[2], a[3], a[4] });
    for (size_t i = 1; i <= 4; ++i)
    {
        const float a0 = a[i - 1], a1 = a[i], a2 = a[i + 1];
        const float den = a0 - 2.0f * a1 + a2;
        if (a1 >= a0 && a1 >= a2 && den < 0.0f)
        {
            const float d = a0 - a2;
            peak = std::max (peak, a1 - 0.125f * d * d / den);
        }
    }
    return peak;
}

//==============================================================================
LimiterParams TruePeakLimiter::sanitised (const LimiterParams& in, const LimiterParams& fallback) noexcept
{
    LimiterParams p = in;
    p.ceilingDb = sanitise (p.ceilingDb, -12.0f, 0.0f, fallback.ceilingDb);
    p.releaseMs = sanitise (p.releaseMs, 5.0f, 1000.0f, fallback.releaseMs);
    return p;
}

void TruePeakLimiter::updateReleaseCoeffs() noexcept
{
    auto coeff = [this] (double ms) { return std::exp (-1.0 / (ms * 0.001 * spec.sampleRate)); };
    slowCoeff = coeff (static_cast<double> (params.releaseMs));
    fastCoeff = params.autoRelease ? coeff (static_cast<double> (params.releaseMs * kFastFraction)) : slowCoeff;
}

void TruePeakLimiter::updateCeiling (float ceilingDb) noexcept
{
    ceilingLin = dbToGain (ceilingDb);
    thresholdLin = ceilingLin * kMarginLin;
}

//==============================================================================
void TruePeakLimiter::prepare (const ProcessSpec& newSpec)
{
    spec = newSpec;
    if (! (spec.sampleRate > 0.0) || ! std::isfinite (spec.sampleRate))
        spec.sampleRate = 48000.0;
    spec.numChannels = std::clamp (spec.numChannels, 1, kMaxChannels);
    spec.maxBlockSize = std::max (1, spec.maxBlockSize);

    // Structural settings are latched here and nowhere else.
    detectTruePeak = truePeak;
    const float la = std::isfinite (lookaheadMs) ? std::clamp (lookaheadMs, 0.0f, kMaxLookaheadMs) : 1.5f;
    lookahead = msToSamples (la, spec.sampleRate);
    detectorDelay = detectTruePeak ? TruePeakDetector::kDelay : 0;

    hold = detectTruePeak ? std::min (kTruePeakHold, lookahead / 3) : 0;
    envelope = envelopeRequest;

    detector.prepare (spec.numChannels);
    audioDelay.prepare (spec.numChannels, lookahead + detectorDelay);

    // Gain hold after the peak (periodHold): H extends the sliding minimum's window.
    holdMax = envelope.periodHold ? static_cast<uint32_t> (msToSamples (kMaxHoldMs, spec.sampleRate)) : 0u;
    peakGap = static_cast<uint32_t> (std::max (1, msToSamples (kPeakGapMs, spec.sampleRate)));

    // The deque holds at most L + Kh + 2 + H live entries plus the one that
    // expires on the current sample.
    baseWindow = static_cast<uint32_t> (lookahead + hold + 2);
    const int capacity = nextPowerOfTwo (lookahead + hold + 3 + static_cast<int> (holdMax));
    dequeValue.assign (static_cast<size_t> (capacity), 1.0f);
    dequeIndex.assign (static_cast<size_t> (capacity), 0u);
    dequeMask = static_cast<uint32_t> (capacity - 1);

    // Attack: one box of L - Kh + 1 samples, or (smoothAttack) two cascaded
    // boxes over the same support (lengths M1 + M2 - 1 = L - Kh + 1).
    const int support = lookahead - hold + 1;
    ringSize = support;
    ring2Size = 1;
    if (envelope.smoothAttack && support >= 3)
    {
        ringSize = (support + 2) / 2;
        ring2Size = support + 1 - ringSize;
    }
    boxRing.assign (static_cast<size_t> (ringSize), 1.0f);
    boxLength = static_cast<double> (ringSize);
    box2Ring.assign (static_cast<size_t> (ring2Size), 1.0);
    box2Length = static_cast<double> (ring2Size);
    ceilingRing.assign (static_cast<size_t> (lookahead + 1), 1.0f);

    programAttack = std::exp (-1.0 / (static_cast<double> (kProgramAttackMs) * 0.001 * spec.sampleRate));
    programRelease = std::exp (-1.0 / (static_cast<double> (kProgramReleaseMs) * 0.001 * spec.sampleRate));

    gapSamples = std::max (1, msToSamples (kRunGapMs, spec.sampleRate));
    blendStart = std::max (1, msToSamples (kBlendStartMs, spec.sampleRate));
    blendEnd = std::max (blendStart + 1, msToSamples (kBlendEndMs, spec.sampleRate));
    blendScale = 1.0 / static_cast<double> (blendEnd - blendStart);

    safetyClips.store (0, std::memory_order_relaxed);
    prepared = true;
    reset();
}

void TruePeakLimiter::reset() noexcept FLUB_NONBLOCKING
{
    detector.reset();
    audioDelay.reset();

    dequeFront = dequeBack = 0;
    sampleIndex = 0;

    std::fill (boxRing.begin(), boxRing.end(), 1.0f);
    boxSum = static_cast<double> (ringSize);
    ringPos = 0;
    std::fill (box2Ring.begin(), box2Ring.end(), 1.0);
    box2Sum = static_cast<double> (ring2Size);
    ring2Pos = 0;
    ceilingPos = 0;

    window = baseWindow;
    holdSamples = spacingLast = spacingPrev = 0;
    sincePeakStart = sinceOverSample = 2 * holdMax + peakGap; // no recent peak
    programGain = 1.0;
    holdMs.store (0.0f, std::memory_order_relaxed);

    // No previous output to click against: the ceiling starts at its target.
    ceilingDbS.reset (spec.sampleRate, kCeilingSmoothMs, params.ceilingDb);
    updateCeiling (params.ceilingDb);
    std::fill (ceilingRing.begin(), ceilingRing.end(), ceilingLin);

    gain = 1.0;
    sinceOver = gapSamples + 1;
    runAge = runSpan = 0;
    updateReleaseCoeffs();

    grDb.store (0.0f, std::memory_order_relaxed);
    fresh = true;
}

int TruePeakLimiter::latencySamples() const noexcept
{
    return lookahead + detectorDelay;
}

void TruePeakLimiter::setParams (const LimiterParams& newParams) noexcept FLUB_NONBLOCKING
{
    const LimiterParams p = sanitised (newParams, params);
    if (p == params)
        return; // pushed every block by the chain; unchanged values cost nothing

    const bool releaseChanged = p.releaseMs != params.releaseMs || p.autoRelease != params.autoRelease;
    params = p;
    if (fresh)
    {
        // Nothing has been output since prepare()/reset(), so there is no
        // gain to click against: start directly at the requested ceiling
        // (the chain pushes its parameters right after preparing).
        ceilingDbS.setImmediate (p.ceilingDb);
        updateCeiling (p.ceilingDb);
        std::fill (ceilingRing.begin(), ceilingRing.end(), ceilingLin);
    }
    else
    {
        ceilingDbS.setTarget (p.ceilingDb);
    }
    if (releaseChanged)
        updateReleaseCoeffs();
}

//==============================================================================
void TruePeakLimiter::process (const AudioBlock& block) noexcept FLUB_NONBLOCKING
{
    const int numSamples = block.numSamples;
    const int numCh = std::min ({ block.numChannels, spec.numChannels, kMaxChannels });
    if (! prepared || numSamples <= 0 || numCh <= 0)
        return;
    fresh = false;

    // Prepared channels that are missing from this block are fed silence, so
    // their detector history and look-ahead line never hold stale audio that
    // could be released unlimited when a wider block comes back.
    const int numPrepared = spec.numChannels;
    std::array<float*, kMaxChannels> data {};
    for (int c = 0; c < numCh; ++c)
        data[static_cast<size_t> (c)] = block.channel (c);

    float* const dqValue = dequeValue.data();
    uint32_t* const dqIndex = dequeIndex.data();
    float* const box = boxRing.data();
    float* const ceilHist = ceilingRing.data();

    float minGain = 1.0f;
    uint64_t clips = 0;
    const bool periodHold = envelope.periodHold;
    const bool program = envelope.programEnvelope;
    const uint32_t counterCap = 2 * holdMax + peakGap;

    // Strictly per sample with all state carried across calls, so the output
    // is bit-identical for any host block size.
    for (int i = 0; i < numSamples; ++i)
    {
        if (ceilingDbS.isSmoothing())
            updateCeiling (ceilingDbS.next());

        // ---- 1) linked peak: one gain for all channels (keeps the image) --------
        // std::max (peak, NaN) keeps peak, so a NaN sample cannot blind the
        // detector; it is dealt with by the output guard below.
        float peak = 0.0f;
        if (detectTruePeak)
        {
            for (int c = 0; c < numCh; ++c)
                peak = std::max (peak, detector.processSample (c, data[static_cast<size_t> (c)][i]));
            for (int c = numCh; c < numPrepared; ++c)
                peak = std::max (peak, detector.processSample (c, 0.0f));
        }
        else
        {
            for (int c = 0; c < numCh; ++c)
                peak = std::max (peak, std::abs (data[static_cast<size_t> (c)][i]));
        }

        // ---- 2) required gain r = min (1, threshold / p) --------------------------
        // Written as a comparison so +Inf gives 0 and NaN gives 1.
        const float r = peak > thresholdLin ? thresholdLin / peak : 1.0f;

        // ---- 2b) period hold: H = the recent peak spacing (periodHold) --------
        // A peak starts when r < 1 after more than peakGap samples without an
        // over; the spacing of peak starts sets the hold (max of the last two,
        // so alternating big / small peaks are covered). The window may
        // shrink by many samples at once: the expiry below is a loop.
        if (periodHold)
        {
            if (r < 1.0f)
            {
                if (sinceOverSample > peakGap)
                {
                    const uint32_t spacing = sincePeakStart;
                    spacingPrev = spacing <= holdMax ? spacingLast : 0u;
                    spacingLast = spacing <= holdMax ? spacing : 0u;
                    const uint32_t recent = std::max (spacingLast, spacingPrev);
                    holdSamples = std::min (holdMax, recent + recent / 8u);
                    window = baseWindow + holdSamples;
                    sincePeakStart = 0;
                }
                sinceOverSample = 0;
            }
            sincePeakStart = std::min (sincePeakStart + 1u, counterCap);
            sinceOverSample = std::min (sinceOverSample + 1u, counterCap);
        }

        // ---- 3) sliding minimum over r[n-L-Kh-1 .. n] (monotonic deque) -----------
        // Entries increase from front to back; anything at the back that is
        // not smaller than r can never be the minimum again. Each entry is
        // pushed and popped once, so this is O(1) amortised and at most
        // 2 * numSamples + L + 2 operations per block.
        while (dequeBack != dequeFront && dqValue[(dequeBack - 1u) & dequeMask] >= r)
            --dequeBack;
        dqValue[dequeBack & dequeMask] = r;
        dqIndex[dequeBack & dequeMask] = sampleIndex;
        ++dequeBack;
        // Without the adaptive hold exactly one index leaves the window per
        // sample; if it is still in the deque it is the oldest entry, i.e. the
        // front. A shrinking hold can expire several at once. The newest
        // entry (age 0) never expires, so the deque is never empty here.
        while (sampleIndex - dqIndex[dequeFront & dequeMask] >= window)
            ++dequeFront;
        const float m = dqValue[dequeFront & dequeMask];
        ++sampleIndex;

        // ---- 4) box filter: mean of m over the last L - Kh + 1 samples ------------
        // Every m[k], k in [n-L+Kh, n], has r[j] in its window for every j in
        // [n-L-1-Kh, n-L+Kh], so the mean is <= all of them: the gain is down
        // to the required value Kh samples before the peak (and the
        // inter-sample interval before it) leaves the look-ahead delay, via a
        // linear ramp of L - Kh + 1 samples, and stays there until Kh samples
        // after it.
        const float oldM = box[ringPos];
        box[ringPos] = m;
        boxSum += static_cast<double> (m) - static_cast<double> (oldM);
        ceilHist[ceilingPos] = ceilingLin;
        if (++ceilingPos > lookahead)
            ceilingPos = 0;
        if (++ringPos == ringSize)
        {
            ringPos = 0;
            // Re-sum once per ring cycle (O(1) amortised) so the running sum
            // can never drift, however long the stream runs.
            double s = 0.0;
            for (int k = 0; k < ringSize; ++k)
                s += static_cast<double> (box[k]);
            boxSum = s;
        }
        // The oldest ceiling in its ring (L + 1 long) is the one r[n-L] was
        // computed with: the clamp level this output sample was limited to.
        const float clampLin = ceilHist[ceilingPos];
        // (A division, so that a window full of 1.0 gives exactly 1.0.)
        double env = std::clamp (boxSum / boxLength, 0.0, 1.0);
        if (ring2Size > 1)
        {
            // smoothAttack: the second box of the cascade (triangular kernel).
            box2Sum += env - box2Ring[static_cast<size_t> (ring2Pos)];
            box2Ring[static_cast<size_t> (ring2Pos)] = env;
            if (++ring2Pos == ring2Size)
            {
                ring2Pos = 0;
                double s2 = 0.0;
                for (int k = 0; k < ring2Size; ++k)
                    s2 += box2Ring[static_cast<size_t> (k)];
                box2Sum = s2;
            }
            env = std::clamp (box2Sum / box2Length, 0.0, 1.0);
        }

        // ---- 5) program-dependent release ----------------------------------------
        // runSpan = time from the first to the latest over of the current run
        // (overs < kRunGapMs apart); the run ends once the gain has recovered.
        if (r < 1.0f)
        {
            sinceOver = 0;
            runSpan = runAge;
        }
        else if (sinceOver <= gapSamples)
        {
            ++sinceOver;
        }
        if (sinceOver > gapSamples && gain >= kRecoveredGain)
            runAge = runSpan = 0;
        else if (runAge < blendEnd)
            ++runAge;

        // g = min (a, release (g)): attack follows the envelope exactly (it is
        // already a linear ramp), release is a one-pole towards it.
        if (env <= gain)
        {
            gain = env;
        }
        else
        {
            const double blend = std::clamp (static_cast<double> (runSpan - blendStart) * blendScale, 0.0, 1.0);
            const double coeff = fastCoeff + (slowCoeff - fastCoeff) * blend;
            gain = env + coeff * (gain - env);
            if (env - gain < kLand)
                gain = env;
        }
        double outGain = gain;
        if (program)
        {
            // Program envelope: a slow follower of g; the output takes the
            // lower of the two, so it can only reduce further.
            if (gain < programGain)
            {
                programGain = gain + programAttack * (programGain - gain);
            }
            else
            {
                programGain = gain + programRelease * (programGain - gain);
                if (gain - programGain < kLand)
                    programGain = gain;
            }
            outGain = std::min (gain, programGain);
        }
        const float g = static_cast<float> (outGain);
        minGain = std::min (minGain, g);

        // ---- 6) delayed audio, gain, final safety clamp --------------------------
        // |x[n-L-D]| <= p[n-L], so |y| <= threshold < clampLin by construction;
        // the clamp is a counted last resort (and turns NaN into silence).
        for (int c = 0; c < numCh; ++c)
        {
            float* d = data[static_cast<size_t> (c)];
            float y = audioDelay.processSample (c, d[i]) * g;
            if (! (std::abs (y) <= clampLin))
            {
                y = std::isnan (y) ? 0.0f : std::copysign (clampLin, y);
                ++clips;
            }
            d[i] = y;
        }
        for (int c = numCh; c < numPrepared; ++c)
            audioDelay.processSample (c, 0.0f);
        audioDelay.advance();
    }

    if (periodHold)
        holdMs.store (static_cast<float> (1000.0 * holdSamples / spec.sampleRate), std::memory_order_relaxed);
    grDb.store (gainToDb (minGain), std::memory_order_relaxed);
    if (clips > 0)
        safetyClips.fetch_add (clips, std::memory_order_relaxed);
}
} // namespace flub
