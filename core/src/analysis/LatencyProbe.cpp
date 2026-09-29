#include "flub/analysis/LatencyProbe.h"

#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace flub::latency
{
namespace
{
constexpr double kFadeSeconds = 0.005;
constexpr double kTailMs = 250.0;         // gap beyond maxDelayMs: the path's own tail
constexpr double kPeakGuardBeforeMs = 1.0; // excluded from the noise around the peak
constexpr double kPeakGuardAfterMs = 20.0;
constexpr double kRegularisation = 1.0e-4; // x max |X|^2

int samplesOf (double seconds, double sampleRate)
{
    return static_cast<int> (std::lround (seconds * sampleRate));
}

double effectiveEndHz (const ProbeSettings& s)
{
    return std::min (s.endHz, 0.45 * s.sampleRate);
}

int nextPowerOfTwo (int n)
{
    int size = 2;
    while (size < n)
        size *= 2;
    return size;
}

/** Raised-cosine band window over the sweep's range: rises over its first
    octave, falls over the last 1/3 octave. */
double bandWeight (double hz, double lowHz, double highHz)
{
    if (hz <= lowHz || hz >= highHz)
        return 0.0;
    const double riseEnd = 2.0 * lowHz, fallStart = highHz / std::cbrt (2.0);
    if (hz < riseEnd)
        return 0.5 - 0.5 * std::cos (kPi * std::log (hz / lowHz) / std::log (riseEnd / lowHz));
    if (hz > fallStart)
        return 0.5 - 0.5 * std::cos (kPi * std::log (highHz / hz) / std::log (highHz / fallStart));
    return 1.0;
}

std::string format (const char* pattern, double a, double b = 0.0)
{
    char buf[160];
    std::snprintf (buf, sizeof (buf), pattern, a, b);
    return buf;
}
} // namespace

std::string validate (const ProbeSettings& s)
{
    if (! (s.sampleRate >= 8000.0 && s.sampleRate <= 768000.0))
        return "sample rate must be 8000 .. 768000 Hz";
    if (! (s.sweepSeconds >= 0.1 && s.sweepSeconds <= 10.0))
        return "sweep length must be 0.1 .. 10 s";
    if (! (s.startHz >= 10.0 && s.startHz * 4.0 <= effectiveEndHz (s)))
        return "sweep range must start at 10 Hz or above and span at least two octaves below 0.45 x the sample rate";
    if (! (s.levelDbfs >= -60.0 && s.levelDbfs <= 0.0))
        return "level must be -60 .. 0 dBFS";
    if (! (s.maxDelayMs >= 1.0 && s.maxDelayMs <= 5000.0))
        return "maximum delay must be 1 .. 5000 ms";
    if (! (s.leadInSeconds >= 0.0 && s.leadInSeconds <= 10.0))
        return "lead-in must be 0 .. 10 s";
    if (s.runs < 1 || s.runs > 100)
        return "runs must be 1 .. 100";
    if (! std::isfinite (s.minSnrDb))
        return "minimum SNR must be a number";
    return {};
}

int sweepLength (const ProbeSettings& s)
{
    return samplesOf (s.sweepSeconds, s.sampleRate);
}

int gapLength (const ProbeSettings& s)
{
    return samplesOf ((s.maxDelayMs + kTailMs) * 0.001, s.sampleRate);
}

int64_t runStart (const ProbeSettings& s, int run)
{
    return static_cast<int64_t> (samplesOf (s.leadInSeconds, s.sampleRate))
           + static_cast<int64_t> (run) * (sweepLength (s) + gapLength (s));
}

int64_t probeLength (const ProbeSettings& s)
{
    return runStart (s, s.runs);
}

std::vector<float> makeSweep (const ProbeSettings& s)
{
    const int n = sweepLength (s);
    const double f1 = s.startHz, f2 = effectiveEndHz (s);
    const double T = static_cast<double> (n) / s.sampleRate;
    const double L = T / std::log (f2 / f1); // instantaneous frequency f1 e^(t / L)
    const double amplitude = std::pow (10.0, s.levelDbfs / 20.0);
    const int fade = std::max (1, samplesOf (kFadeSeconds, s.sampleRate));

    std::vector<float> x (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        const double t = i / s.sampleRate;
        double g = amplitude;
        const int edge = std::min (i, n - 1 - i);
        if (edge < fade)
            g *= 0.5 - 0.5 * std::cos (kPi * edge / fade);
        x[static_cast<size_t> (i)] = static_cast<float> (g * std::sin (kTwoPi * f1 * L * (std::exp (t / L) - 1.0)));
    }
    return x;
}

std::vector<float> makeProbe (const ProbeSettings& s)
{
    const auto sweep = makeSweep (s);
    std::vector<float> probe (static_cast<size_t> (probeLength (s)), 0.0f);
    for (int run = 0; run < s.runs; ++run)
        std::copy (sweep.begin(), sweep.end(), probe.begin() + runStart (s, run));
    return probe;
}

// =============================================================================
LatencyProbe::LatencyProbe (const ProbeSettings& s)
    : settings (s)
{
    sweepSamples = sweepLength (s);
    windowLength = gapLength (s);
    segmentLength = sweepSamples + windowLength;
    fftSize = nextPowerOfTwo (segmentLength + sweepSamples);
    fft.prepare (fftSize);

    // Inverse of the sweep's spectrum, regularised where it is weak and
    // limited to the swept band, so the deconvolved response is a smooth
    // band-limited impulse at the path's delay.
    std::vector<float> padded (static_cast<size_t> (fftSize), 0.0f);
    const auto sweep = makeSweep (s);
    std::copy (sweep.begin(), sweep.end(), padded.begin());
    const size_t numBins = static_cast<size_t> (fftSize / 2 + 1);
    std::vector<Fft::Complex> x (numBins);
    fft.forwardReal (padded.data(), x.data());
    double maxPower = 0.0;
    for (const auto& c : x)
        maxPower = std::max (maxPower, static_cast<double> (std::norm (c)));
    const double eps = kRegularisation * maxPower;
    const double lowHz = s.startHz, highHz = effectiveEndHz (s);
    inverse.resize (numBins);
    for (size_t k = 0; k < numBins; ++k)
    {
        const double hz = static_cast<double> (k) * s.sampleRate / fftSize;
        const double w = bandWeight (hz, lowHz, highHz);
        const std::complex<double> xk (x[k].real(), x[k].imag());
        const auto inv = w * std::conj (xk) / (std::norm (xk) + eps);
        inverse[k] = Fft::Complex (static_cast<float> (inv.real()), static_cast<float> (inv.imag()));
    }

    segment.resize (static_cast<size_t> (fftSize));
    response.resize (static_cast<size_t> (fftSize));
    bins.resize (numBins);
}

void LatencyProbe::deconvolve (const float* recording, int64_t numSamples, int64_t start)
{
    std::fill (segment.begin(), segment.end(), 0.0f);
    for (int i = 0; i < segmentLength; ++i)
        if (start + i >= 0 && start + i < numSamples)
            segment[static_cast<size_t> (i)] = recording[start + i];
    fft.forwardReal (segment.data(), bins.data());
    for (size_t k = 0; k < bins.size(); ++k)
        bins[k] *= inverse[k];
    fft.inverseReal (bins.data(), response.data());
}

std::vector<float> LatencyProbe::impulseResponse (const float* recording, int64_t numSamples, int run)
{
    deconvolve (recording, numSamples, runStart (settings, run));
    return { response.begin(), response.begin() + windowLength };
}

Run LatencyProbe::measureRun (const float* recording, int64_t numSamples, int run)
{
    return measureAt (recording, numSamples, run, runStart (settings, run));
}

Run LatencyProbe::measureAt (const float* recording, int64_t numSamples, int run, int64_t start)
{
    deconvolve (recording, numSamples, start);
    Run r;
    r.index = run;

    const int maxLag = std::min (windowLength, samplesOf (settings.maxDelayMs * 0.001, settings.sampleRate) + 1);
    int peak = 0;
    for (int i = 1; i < maxLag; ++i)
        if (std::fabs (response[static_cast<size_t> (i)]) > std::fabs (response[static_cast<size_t> (peak)]))
            peak = i;
    const auto at = [this] (int i) { return static_cast<double> (std::fabs (response[static_cast<size_t> (i)])); };
    const double y0 = at (peak);
    double offset = 0.0;
    if (peak > 0 && peak + 1 < windowLength)
    {
        const double ym = at (peak - 1), yp = at (peak + 1);
        const double denominator = ym - 2.0 * y0 + yp;
        if (denominator < 0.0)
            offset = std::clamp (0.5 * (ym - yp) / denominator, -0.5, 0.5);
    }
    r.delaySamples = peak + offset;
    r.inverted = response[static_cast<size_t> (peak)] < 0.0f;

    // Noise: every lag of the window but the peak's neighbourhood.
    const int before = std::max (1, samplesOf (kPeakGuardBeforeMs * 0.001, settings.sampleRate));
    const int after = std::max (1, samplesOf (kPeakGuardAfterMs * 0.001, settings.sampleRate));
    double sum = 0.0;
    int count = 0;
    for (int i = 0; i < windowLength; ++i)
    {
        if (i >= peak - before && i <= peak + after)
            continue;
        sum += at (i) * at (i);
        ++count;
    }
    const double noise = count > 0 ? std::sqrt (sum / count) : 0.0;
    r.snrDb = y0 <= 0.0 ? 0.0 : (noise > 0.0 ? 20.0 * std::log10 (y0 / noise) : 200.0);
    r.accepted = r.snrDb >= settings.minSnrDb;
    return r;
}

Result LatencyProbe::summarise (std::vector<Run> runs) const
{
    Result result;
    result.runs = std::move (runs);
    if (result.runs.empty())
    {
        result.error = "the recording is shorter than the first sweep (it must start with the probe)";
        return result;
    }

    std::vector<double> delays;
    int inverted = 0;
    double bestSnr = -1.0e9;
    for (const auto& r : result.runs)
    {
        bestSnr = std::max (bestSnr, r.snrDb);
        if (! r.accepted)
            continue;
        delays.push_back (r.delaySamples);
        inverted += r.inverted ? 1 : 0;
    }
    result.acceptedRuns = static_cast<int> (delays.size());
    if (delays.empty() || 2 * delays.size() < result.runs.size())
    {
        result.error = format ("only %.0f of %.0f runs have the response at least ", static_cast<double> (delays.size()),
                               static_cast<double> (result.runs.size()))
                       + format ("%.0f dB above the noise (best %.1f dB): raise the level, check the connection or lower the noise",
                                 settings.minSnrDb, bestSnr);
        return result;
    }
    std::sort (delays.begin(), delays.end());
    const size_t n = delays.size();
    result.delaySamples = n % 2 == 1 ? delays[n / 2] : 0.5 * (delays[n / 2 - 1] + delays[n / 2]);
    result.delayMs = 1000.0 * result.delaySamples / settings.sampleRate;
    result.spreadSamples = delays.back() - delays.front();
    result.inverted = 2 * inverted > result.acceptedRuns;
    result.ok = true;
    return result;
}

Result LatencyProbe::analyse (const float* recording, int64_t numSamples)
{
    std::vector<Run> runs;
    for (int run = 0; run < settings.runs && runStart (settings, run) + sweepSamples <= numSamples; ++run)
        runs.push_back (measureRun (recording, numSamples, run));
    return summarise (std::move (runs));
}

int64_t LatencyProbe::locate (const float* recording, int64_t numSamples) const
{
    // One deconvolution of the whole recording (a coarse position is enough:
    // the runs are then measured precisely around it).
    if (numSamples < sweepSamples)
        return -1;
    const int64_t total = numSamples + sweepSamples;
    if (total > (int64_t { 1 } << 28))
        return -1;
    const int size = nextPowerOfTwo (static_cast<int> (total));
    Fft big;
    big.prepare (size);
    const size_t numBins = static_cast<size_t> (size / 2 + 1);
    std::vector<float> buffer (static_cast<size_t> (size), 0.0f);
    std::vector<Fft::Complex> x (numBins), y (numBins);
    const auto sweep = makeSweep (settings);
    std::copy (sweep.begin(), sweep.end(), buffer.begin());
    big.forwardReal (buffer.data(), x.data());
    std::fill (buffer.begin(), buffer.end(), 0.0f);
    std::copy (recording, recording + numSamples, buffer.begin());
    big.forwardReal (buffer.data(), y.data());
    double maxPower = 0.0;
    for (const auto& c : x)
        maxPower = std::max (maxPower, static_cast<double> (std::norm (c)));
    const double eps = kRegularisation * maxPower;
    for (size_t k = 0; k < numBins; ++k)
    {
        const double w = bandWeight (static_cast<double> (k) * settings.sampleRate / size, settings.startHz, effectiveEndHz (settings));
        y[k] *= static_cast<float> (w / (std::norm (x[k]) + eps)) * std::conj (x[k]);
    }
    big.inverseReal (y.data(), buffer.data());

    // The first arrival at least half as strong as the strongest.
    float strongest = 0.0f;
    for (int64_t i = 0; i < numSamples; ++i)
        strongest = std::max (strongest, std::fabs (buffer[static_cast<size_t> (i)]));
    if (strongest <= 0.0f)
        return -1;
    for (int64_t i = 0; i < numSamples; ++i)
        if (std::fabs (buffer[static_cast<size_t> (i)]) >= 0.5f * strongest)
        {
            int64_t best = i; // the peak of that arrival
            for (int64_t j = i; j < std::min (numSamples, i + sweepSamples / 4); ++j)
                if (std::fabs (buffer[static_cast<size_t> (j)]) > std::fabs (buffer[static_cast<size_t> (best)]))
                    best = j;
            return best;
        }
    return -1;
}

Result LatencyProbe::analyseRelative (const float* recording, const float* reference, int64_t numSamples)
{
    // The probe's position in the reference sets where the runs start, so the
    // recording's start does not have to be locked to playback: the direct
    // path lands 10 ms into each run's window.
    const int64_t arrival = locate (reference, numSamples);
    if (arrival < 0)
    {
        Result failed;
        failed.error = "the probe was not found in the reference channel";
        return failed;
    }
    const int64_t shift = arrival - runStart (settings, 0) - samplesOf (0.010, settings.sampleRate);

    std::vector<Run> runs;
    for (int run = 0; run < settings.runs && runStart (settings, run) + shift + sweepSamples <= numSamples; ++run)
    {
        const int64_t start = runStart (settings, run) + shift;
        const Run measured = measureAt (recording, numSamples, run, start);
        const Run direct = measureAt (reference, numSamples, run, start);
        Run r;
        r.index = run;
        r.delaySamples = measured.delaySamples - direct.delaySamples;
        r.snrDb = std::min (measured.snrDb, direct.snrDb);
        r.inverted = measured.inverted != direct.inverted;
        r.accepted = measured.accepted && direct.accepted;
        runs.push_back (r);
    }
    return summarise (std::move (runs));
}
} // namespace flub::latency
