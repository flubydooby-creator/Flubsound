#include "PitchEstimator.h"

#include <algorithm>
#include <cmath>

namespace flub::app::ui::vis
{
namespace
{
constexpr double kPi = juce::MathConstants<double>::pi;

float toDb (float magnitude) noexcept
{
    return 20.0f * std::log10 (magnitude + 1.0e-20f);
}
} // namespace

PitchEstimator::PitchEstimator()
{
    window.resize (static_cast<size_t> (kWindow));
    double sum = 0.0;
    for (int i = 0; i < kWindow; ++i)
    {
        const double x = static_cast<double> (i) / (kWindow - 1);
        const double w = 0.42 - 0.5 * std::cos (2.0 * kPi * x) + 0.08 * std::cos (4.0 * kPi * x); // Blackman
        window[static_cast<size_t> (i)] = static_cast<float> (w);
        sum += w;
    }
    amplitudeScale = static_cast<float> (2.0 / sum); // a sine of amplitude a peaks at a x sum / 2
    history.assign (static_cast<size_t> (kWindow), 0.0f);
    fftData.assign (static_cast<size_t> (2 * kFftSize), 0.0f);
    sampleRate = 0.0;
    setSampleRate (48000.0);
}

double PitchEstimator::midiToHz (double midi) noexcept
{
    return 440.0 * std::pow (2.0, (midi - 69.0) / 12.0);
}

void PitchEstimator::setSampleRate (double newRate)
{
    if (newRate <= 0.0 || std::abs (newRate - sampleRate) < 0.5)
        return;
    sampleRate = newRate;
    decimation = std::max (1, static_cast<int> (std::lround (sampleRate / kTargetRate)));
    decimatedRate = sampleRate / decimation;
    maxHz = 0.28 * decimatedRate;

    // 8th-order Butterworth low-pass at 0.3 x the decimated rate (4 RBJ biquads).
    const double w0 = 2.0 * kPi * std::min (0.3 * decimatedRate, 0.45 * sampleRate) / sampleRate;
    for (size_t s = 0; s < 4; ++s)
    {
        const double q = 1.0 / (2.0 * std::cos ((2.0 * static_cast<double> (s) + 1.0) * kPi / 16.0));
        const double alpha = std::sin (w0) / (2.0 * q);
        const double cw = std::cos (w0);
        const double a0 = 1.0 + alpha;
        coeffs[5 * s + 0] = (1.0 - cw) * 0.5 / a0;
        coeffs[5 * s + 1] = (1.0 - cw) / a0;
        coeffs[5 * s + 2] = (1.0 - cw) * 0.5 / a0;
        coeffs[5 * s + 3] = -2.0 * cw / a0;
        coeffs[5 * s + 4] = (1.0 - alpha) / a0;
    }
    reset();
}

void PitchEstimator::clearResult() noexcept
{
    numPeaks = numNotes = 0;
    chroma.fill (0.0f);
    signal = false;
}

void PitchEstimator::reset() noexcept
{
    std::fill (history.begin(), history.end(), 0.0f);
    state.fill (0.0);
    phase = writePos = filled = sinceHop = 0;
    idleSeconds = 0.0;
    pending = false;
    clearResult();
}

void PitchEstimator::push (const float* samples, int numSamples) noexcept
{
    for (int i = 0; i < numSamples; ++i)
    {
        double y = samples[i];
        if (decimation > 1)
        {
            for (size_t b = 0; b < 4; ++b)
            {
                const double* c = &coeffs[5 * b];
                double* z = &state[2 * b];
                const double out = c[0] * y + z[0];
                z[0] = c[1] * y - c[3] * out + z[1];
                z[1] = c[2] * y - c[4] * out;
                y = out;
            }
            if (++phase < decimation)
                continue;
            phase = 0;
        }
        history[static_cast<size_t> (writePos)] = static_cast<float> (y);
        writePos = (writePos + 1) % kWindow;
        filled = std::min (kWindow, filled + 1);
        if (++sinceHop >= kHop)
            pending = true;
    }
    for (auto& z : state)
        if (std::abs (z) < 1.0e-30)
            z = 0.0;
    if (numSamples > 0)
        idleSeconds = 0.0;
}

bool PitchEstimator::update (double dtSeconds) noexcept
{
    if (pending && filled >= kWindow / 2)
    {
        pending = false;
        sinceHop = 0; // stale hops are skipped: only the latest frame matters
        analyse();
        return true;
    }
    idleSeconds += std::max (0.0, dtSeconds);
    if (idleSeconds > kIdleSeconds && (numPeaks > 0 || signal))
    {
        clearResult();
        return true;
    }
    return false;
}

void PitchEstimator::analyse() noexcept
{
    for (int i = 0; i < kWindow; ++i)
        fftData[static_cast<size_t> (i)] = history[static_cast<size_t> ((writePos + i) % kWindow)] * window[static_cast<size_t> (i)];
    std::fill (fftData.begin() + kWindow, fftData.end(), 0.0f);
    fft.performFrequencyOnlyForwardTransform (fftData.data(), true);

    numPeaks = findPeaks (fftData.data(), kFftSize / 2, decimatedRate / kFftSize, amplitudeScale, kMinHz, maxHz, peaks.data(), kMaxPeaks);
    numNotes = estimateNotes (peaks.data(), numPeaks, maxHz, notes.data(), kMaxNotes);
    signal = foldChroma (peaks.data(), numPeaks, chroma.data());
}

// =============================================================================
// Pure helpers
// =============================================================================
int PitchEstimator::findPeaks (const float* magnitude, int numBins, double binHz, float scale, double minHz, double maxFreq,
                               SpectralPeak* out, int maxPeaks) noexcept
{
    if (binHz <= 0.0 || maxPeaks <= 0)
        return 0;
    const int lo = std::max (1, static_cast<int> (std::ceil (minHz / binHz)));
    const int hi = std::min (numBins - 2, static_cast<int> (std::floor (maxFreq / binHz)));
    if (hi <= lo)
        return 0;

    float loudest = 0.0f;
    for (int k = lo; k <= hi; ++k)
        loudest = std::max (loudest, magnitude[k]);
    const float thresholdDb = std::max (kFloorDb, toDb (loudest * scale) - kRangeDb);

    int count = 0;
    for (int k = lo; k <= hi; ++k)
    {
        const float m = magnitude[k];
        if (! (m > magnitude[k - 1] && m >= magnitude[k + 1]))
            continue;
        const float b = toDb (m * scale);
        if (b < thresholdDb)
            continue;
        const float a = toDb (magnitude[k - 1] * scale), c = toDb (magnitude[k + 1] * scale);
        const float denominator = a - 2.0f * b + c;
        const float p = denominator < -1.0e-6f ? std::clamp (0.5f * (a - c) / denominator, -0.5f, 0.5f) : 0.0f;
        SpectralPeak peak;
        peak.hz = static_cast<float> ((static_cast<double> (k) + p) * binHz);
        peak.amplitude = std::pow (10.0f, (b - 0.25f * (a - c) * p) / 20.0f);

        if (count < maxPeaks)
        {
            out[count++] = peak;
            continue;
        }
        // Full: the new peak replaces the weakest if it is stronger.
        int weakest = 0;
        for (int j = 1; j < count; ++j)
            if (out[j].amplitude < out[weakest].amplitude)
                weakest = j;
        if (out[weakest].amplitude < peak.amplitude)
        {
            for (int j = weakest; j + 1 < count; ++j) // keep the order by frequency
                out[j] = out[j + 1];
            out[count - 1] = peak;
        }
    }
    return count;
}

int PitchEstimator::estimateNotes (const SpectralPeak* peakList, int peakCount, double maxFreq, EstimatedNote* out, int maxNotes) noexcept
{
    peakCount = std::min (peakCount, kMaxPeaks);
    if (peakCount <= 0 || maxNotes <= 0)
        return 0;

    float ref = 0.0f;
    std::array<float, kMaxPeaks> residual {};
    for (int j = 0; j < peakCount; ++j)
    {
        residual[static_cast<size_t> (j)] = peakList[j].amplitude;
        ref = std::max (ref, peakList[j].amplitude);
    }
    if (ref <= 0.0f)
        return 0;
    const auto compress = [ref] (float a) noexcept
    {
        if (a <= 0.0f)
            return 0.0f;
        return std::clamp ((20.0f * std::log10 (a / ref) + kRangeDb) / kRangeDb, 0.0f, 1.0f);
    };
    // A peak counts with its level on the compressed scale times the share of it
    // no note found so far explains (residual / amplitude).
    std::array<float, kMaxPeaks> level {};
    for (int j = 0; j < peakCount; ++j)
        level[static_cast<size_t> (j)] = compress (peakList[j].amplitude);

    // The nearest peak to `hz` within the tolerance, -1 if none.
    const double tolerance = std::pow (2.0, kToleranceCents / 1200.0);
    const auto match = [&] (double hz) noexcept
    {
        int lo = 0, hi = peakCount; // first peak >= hz
        while (lo < hi)
        {
            const int mid = (lo + hi) / 2;
            if (peakList[mid].hz < hz)
                lo = mid + 1;
            else
                hi = mid;
        }
        int best = -1;
        double bestRatio = tolerance;
        for (const int j : { lo - 1, lo })
        {
            if (j < 0 || j >= peakCount)
                continue;
            const double ratio = std::max (peakList[j].hz / hz, hz / peakList[j].hz);
            if (ratio <= bestRatio)
            {
                bestRatio = ratio;
                best = j;
            }
        }
        return best;
    };

    // Matched peak per candidate and harmonic (fixed for all iterations).
    std::array<std::array<int16_t, kMaxHarmonics>, kNumCandidates> table {};
    std::array<int, kNumCandidates> harmonics {};
    std::array<float, kMaxHarmonics> weight {};
    for (int h = 0; h < kMaxHarmonics; ++h)
        weight[static_cast<size_t> (h)] = 1.0f / std::sqrt (static_cast<float> (h + 1));
    for (int c = 0; c < kNumCandidates; ++c)
    {
        const double f0 = midiToHz (kLowMidi + c);
        auto& row = table[static_cast<size_t> (c)];
        int n = 0;
        for (int h = 1; h <= kMaxHarmonics && h * f0 <= maxFreq * tolerance; ++h)
            row[static_cast<size_t> (n++)] = static_cast<int16_t> (match (h * f0));
        harmonics[static_cast<size_t> (c)] = n;
    }

    std::array<bool, kNumCandidates> taken {};
    bool bassTaken = false;
    int count = 0;
    float first = 0.0f;
    while (count < maxNotes)
    {
        int best = -1;
        float bestSalience = 0.0f;
        for (int c = 0; c < kNumCandidates; ++c)
        {
            const auto& row = table[static_cast<size_t> (c)];
            if (taken[static_cast<size_t> (c)] || harmonics[static_cast<size_t> (c)] == 0 || row[0] < 0
                || (bassTaken && kLowMidi + c < kBassSplitMidi))
                continue;
            const int f = row[0];
            if (compress (peakList[f].amplitude) < kMinFundamental || residual[static_cast<size_t> (f)] < kMinFundamentalShare * peakList[f].amplitude)
                continue;
            float s = 0.0f;
            for (int h = 0; h < harmonics[static_cast<size_t> (c)]; ++h)
                if (const int j = row[static_cast<size_t> (h)]; j >= 0)
                    s += weight[static_cast<size_t> (h)] * level[static_cast<size_t> (j)] * residual[static_cast<size_t> (j)] / peakList[j].amplitude;
            if (s > bestSalience)
            {
                bestSalience = s;
                best = c;
            }
        }
        if (best < 0 || bestSalience < kMinSalience || (count > 0 && bestSalience < kRelativeSalience * first))
            break;
        if (count == 0)
            first = bestSalience;

        const auto& row = table[static_cast<size_t> (best)];
        out[count].midi = kLowMidi + best;
        out[count].salience = bestSalience;
        out[count].level = compress (peakList[row[0]].amplitude);
        ++count;
        taken[static_cast<size_t> (best)] = true;
        bassTaken = bassTaken || kLowMidi + best < kBassSplitMidi;

        // Subtract the note: per harmonic at most the mean of it and its matched neighbours.
        const int n = harmonics[static_cast<size_t> (best)];
        std::array<float, kMaxHarmonics> remove {};
        for (int h = 0; h < n; ++h)
        {
            const int j = row[static_cast<size_t> (h)];
            if (j < 0)
                continue;
            float sum = 0.0f;
            int k = 0;
            for (int d = -1; d <= 1; ++d)
                if (h + d >= 0 && h + d < n)
                    if (const int jj = row[static_cast<size_t> (h + d)]; jj >= 0)
                    {
                        sum += residual[static_cast<size_t> (jj)];
                        ++k;
                    }
            remove[static_cast<size_t> (h)] = std::min (residual[static_cast<size_t> (j)], sum / static_cast<float> (k));
        }
        for (int h = 0; h < n; ++h)
            if (const int j = row[static_cast<size_t> (h)]; j >= 0)
                residual[static_cast<size_t> (j)] = std::max (0.0f, residual[static_cast<size_t> (j)] - remove[static_cast<size_t> (h)]);
    }

    std::sort (out, out + count, [] (const EstimatedNote& a, const EstimatedNote& b) { return a.midi < b.midi; });
    return count;
}

bool PitchEstimator::foldChroma (const SpectralPeak* peakList, int peakCount, float* out) noexcept
{
    std::fill (out, out + 12, 0.0f);
    float ref = 0.0f;
    for (int j = 0; j < peakCount; ++j)
        if (peakList[j].hz >= kChromaMinHz)
            ref = std::max (ref, peakList[j].amplitude);
    if (ref <= 0.0f)
        return false;
    for (int j = 0; j < peakCount; ++j)
    {
        const auto& p = peakList[j];
        if (p.hz < kChromaMinHz || p.amplitude <= 0.0f)
            continue;
        const float above = std::clamp ((20.0f * std::log10 (p.amplitude / ref) + kChromaRangeDb) / kChromaRangeDb, 0.0f, 1.0f);
        const float v = above * above;
        if (v <= 0.0f)
            continue;
        const double semis = 12.0 * std::log2 (p.hz / 440.0) + 9.0; // C = 0
        const double nearest = std::round (semis);
        const auto d = static_cast<float> (semis - nearest); // -0.5 .. 0.5
        const int pc = ((static_cast<int> (nearest) % 12) + 12) % 12;
        const int other = (pc + (d >= 0.0f ? 1 : 11)) % 12;
        out[pc] += v * (1.0f - std::abs (d));
        out[other] += v * std::abs (d);
    }
    float top = 0.0f;
    for (int i = 0; i < 12; ++i)
        top = std::max (top, out[i]);
    if (top <= 0.0f)
        return false;
    for (int i = 0; i < 12; ++i)
        out[i] /= top;
    return true;
}
} // namespace flub::app::ui::vis
