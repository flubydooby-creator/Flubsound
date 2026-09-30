#include "flub/analysis/ContentAnalysis.h"

#include "flub/analysis/LoudnessMeter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace flub
{
namespace
{
// Keeps every recursive state normal through digital silence without
// FTZ / DAZ (as LoudnessFollower): the high-passes and band-passes remove it.
constexpr double kAntiDenormal = 1.0e-20;
constexpr double kBandQ = 1.4142135623730951; // about one octave (RBJ, constant 0 dB peak)
constexpr float kOnsetRiseDb = 6.0f, kOnsetFloorDb = -70.0f;
constexpr int kOnsetRefractory = 4; // sub-frames after an onset (50 ms with it)
constexpr size_t kOnsetHighBand = 6; // 4 kHz

BiquadCoeffs bandPass (double f0, double fs) noexcept
{
    const double w0 = kTwoPi * f0 / fs;
    const double alpha = std::sin (w0) / (2.0 * kBandQ);
    const double a0 = 1.0 + alpha;
    BiquadCoeffs c;
    c.b0 = alpha / a0;
    c.b1 = 0.0;
    c.b2 = -alpha / a0;
    c.a1 = -2.0 * std::cos (w0) / a0;
    c.a2 = (1.0 - alpha) / a0;
    return c;
}

BiquadCoeffs lowPassQ (double f0, double q, double fs) noexcept
{
    const double w0 = kTwoPi * f0 / fs;
    const double alpha = std::sin (w0) / (2.0 * q);
    const double cw = std::cos (w0);
    const double a0 = 1.0 + alpha;
    BiquadCoeffs c;
    c.b0 = 0.5 * (1.0 - cw) / a0;
    c.b1 = (1.0 - cw) / a0;
    c.b2 = c.b0;
    c.a1 = -2.0 * cw / a0;
    c.a2 = (1.0 - alpha) / a0;
    return c;
}

/** The band-pass's power gain on pink noise, re the analog prototype's
    (pi / (2 Q) per unit of ln f): integrated over ln f from 2 Hz to
    Nyquist, 48 points per octave. */
double pinkPowerGain (const BiquadCoeffs& c, double fs) noexcept
{
    constexpr int kPerOctave = 48;
    const double step = std::log (2.0) / kPerOctave;
    double sum = 0.0;
    for (double lf = std::log (2.0); lf < std::log (0.5 * fs); lf += step)
        sum += std::norm (c.response (std::exp (lf), fs)) * step;
    return sum / (kPi / (2.0 * kBandQ));
}

float powerDb (double power) noexcept { return power > 1.0e-30 ? static_cast<float> (10.0 * std::log10 (power)) : kMinusInfDb; }
} // namespace

void ContentAnalysis::prepare (double fs, int frameSamples)
{
    sampleRate = fs;
    frameLength = frameSamples > 0 ? frameSamples : std::max (1, msToSamples (kFrameMs, fs));
    subLength = std::max (1, frameLength / 10);
    kStage1 = LoudnessMeter::kWeightingStage1 (fs);
    kStage2 = LoudnessMeter::kWeightingStage2 (fs);
    lowPass[0] = lowPassQ (100.0, 0.5411961001461969, fs);
    lowPass[1] = lowPassQ (100.0, 1.3065629648763766, fs);
    numBands = 0;
    for (int b = 0; b < kNumBands; ++b)
    {
        const auto i = static_cast<size_t> (b);
        if (kBandHz[i] >= 0.4 * fs)
            break;
        bandCoeffs[i] = bandPass (kBandHz[i], fs);
        bandNorm[i] = 1.0 / std::max (1.0e-6, pinkPowerGain (bandCoeffs[i], fs));
        numBands = b + 1;
    }
    reset();
}

void ContentAnalysis::reset() noexcept FLUB_NONBLOCKING
{
    for (auto* states : { &kState1, &kState2, &lowState })
        for (auto& s : *states)
            s.reset();
    for (auto& s : bandState)
        s.reset();
    current = {};
    previousBandDb.fill (0.0f);
    previousBandsValid = false;
    subEnergy = {};
    subCount = 0;
    subHistory = {};
    subHistoryCount = subHistoryPos = refractory = 0;
    ringCount = ringPos = 0;
    whole = {};
    wholeFrames = 0;
    wholeFlux = 0.0;
    state = {};
}

void ContentAnalysis::analyseSample (double l, double r) noexcept FLUB_NONBLOCKING
{
    l += kAntiDenormal;
    r += kAntiDenormal;
    const double kl = biquadTick (kStage2, kState2[0], biquadTick (kStage1, kState1[0], l));
    const double kr = biquadTick (kStage2, kState2[1], biquadTick (kStage1, kState1[1], r));
    Frame& f = current;
    f.kPower += kl * kl + kr * kr;
    f.plainPower += l * l + r * r;
    f.peak = std::max (f.peak, static_cast<float> (std::max (std::abs (l), std::abs (r))));
    const double m = 0.5 * (l + r), s = 0.5 * (l - r);
    f.mm += m * m;
    f.ss += s * s;
    f.ll += l * l;
    f.rr += r * r;
    f.lr += l * r;
    const double low = biquadTick (lowPass[1], lowState[1], biquadTick (lowPass[0], lowState[0], m));
    f.low += low * low;
    double high = 0.0;
    for (int b = 0; b < numBands; ++b)
    {
        const auto i = static_cast<size_t> (b);
        const double y = biquadTick (bandCoeffs[i], bandState[i], m);
        f.band[i] += y * y;
        if (i >= kOnsetHighBand)
            high += y * y;
    }

    // Onsets on 10 ms sub-frames: the mid's energy (kicks, bass notes) or
    // that of its bands from 4 kHz up (hats, consonants) rising out of the
    // 50 ms before it.
    subEnergy[0] += m * m;
    subEnergy[1] += high;
    if (++subCount == subLength)
    {
        bool onset = false;
        for (size_t k = 0; k < 2; ++k)
        {
            const double e = subEnergy[k] / subLength;
            if (subHistoryCount == static_cast<int> (kOnsetHistory))
            {
                double mean = 0.0;
                for (size_t h = 0; h < kOnsetHistory; ++h)
                    mean += subHistory[k][h];
                const float eDb = powerDb (e);
                onset = onset || (eDb > kOnsetFloorDb && eDb > powerDb (mean / static_cast<double> (kOnsetHistory)) + kOnsetRiseDb);
            }
            subHistory[k][static_cast<size_t> (subHistoryPos)] = e;
            subEnergy[k] = 0.0;
        }
        if (refractory > 0)
            --refractory;
        else if (onset)
        {
            ++f.onsets;
            refractory = kOnsetRefractory;
        }
        subHistoryPos = (subHistoryPos + 1) % static_cast<int> (kOnsetHistory);
        subHistoryCount = std::min (subHistoryCount + 1, static_cast<int> (kOnsetHistory));
        subCount = 0;
    }

    if (++f.samples == frameLength)
        closeFrame();
}

void ContentAnalysis::process (const AudioBlock& block, bool measure) noexcept FLUB_NONBLOCKING
{
    const int n = block.numSamples;
    const int nch = block.numChannels;
    if (! measure || nch <= 0)
    {
        // Only the frame clock moves; what the frame holds so far is kept.
        for (int left = n; left > 0;)
        {
            const int step = std::min (left, frameLength - current.samples);
            current.samples += step;
            left -= step;
            if (current.samples == frameLength)
                closeFrame();
        }
        return;
    }
    constexpr double kMinus3 = 0.7071067811865476;
    for (int i = 0; i < n; ++i)
    {
        double l = block.channel (0)[i];
        double r = nch > 1 ? static_cast<double> (block.channel (1)[i]) : l;
        if (nch > 2)
        {
            // FL FR FC LFE [BL BR] SL SR: centre and surrounds at -3 dB, LFE at -6 dB.
            const double c = block.channel (2)[i];
            const double lfe = nch > 3 ? 0.5 * static_cast<double> (block.channel (3)[i]) : 0.0;
            l += kMinus3 * c + lfe;
            r += kMinus3 * c + lfe;
            for (int s = 4; s + 1 < nch; s += 2)
            {
                l += kMinus3 * static_cast<double> (block.channel (s)[i]);
                r += kMinus3 * static_cast<double> (block.channel (s + 1)[i]);
            }
        }
        analyseSample (l, r);
    }
}

void ContentAnalysis::closeFrame() noexcept FLUB_NONBLOCKING
{
    Frame& f = current;
    const double len = std::max (1, f.samples);
    // The gate reads the plain level: the K-weighting's ring-out after the
    // programme stops would count the first frame of a pause.
    const bool programme = powerDb (f.plainPower / (2.0 * len)) > kSilenceDb;
    if (programme)
    {
        std::array<float, kNumBands> bandDb {};
        float flux = 0.0f;
        for (int b = 0; b < numBands; ++b)
        {
            const auto i = static_cast<size_t> (b);
            bandDb[i] = std::max (-120.0f, powerDb (f.band[i] * bandNorm[i] / len));
            if (previousBandsValid)
                flux += std::max (0.0f, bandDb[i] - previousBandDb[i]);
        }
        f.flux = numBands > 0 ? flux / static_cast<float> (numBands) : 0.0f;
        previousBandDb = bandDb;
        previousBandsValid = true;
        ++state.frames;
        if (wholeProgramme)
        {
            whole.kPower += f.kPower;
            whole.plainPower += f.plainPower;
            whole.mm += f.mm;
            whole.ss += f.ss;
            whole.ll += f.ll;
            whole.rr += f.rr;
            whole.lr += f.lr;
            whole.low += f.low;
            for (size_t i = 0; i < f.band.size(); ++i)
                whole.band[i] += f.band[i];
            whole.peak = std::max (whole.peak, f.peak);
            whole.onsets += f.onsets;
            whole.samples += f.samples;
            wholeFlux += f.flux;
            ++wholeFrames;
        }
        else
        {
            ring[static_cast<size_t> (ringPos)] = f;
            ringPos = (ringPos + 1) % kWindowFrames;
            ringCount = std::min (ringCount + 1, kWindowFrames);
        }
        updateState();
    }
    else
    {
        previousBandsValid = false; // no flux out of a pause
    }
    current = {};
}

void ContentAnalysis::updateState() noexcept FLUB_NONBLOCKING
{
    Frame sum;
    double fluxSum = 0.0;
    int count = 0;
    if (wholeProgramme)
    {
        sum = whole;
        fluxSum = wholeFlux;
        count = wholeFrames;
    }
    else
    {
        for (int k = 0; k < ringCount; ++k)
        {
            const Frame& f = ring[static_cast<size_t> (k)];
            sum.kPower += f.kPower;
            sum.plainPower += f.plainPower;
            sum.mm += f.mm;
            sum.ss += f.ss;
            sum.ll += f.ll;
            sum.rr += f.rr;
            sum.lr += f.lr;
            sum.low += f.low;
            for (size_t i = 0; i < f.band.size(); ++i)
                sum.band[i] += f.band[i];
            sum.peak = std::max (sum.peak, f.peak);
            sum.onsets += f.onsets;
            sum.samples += f.samples;
            fluxSum += f.flux;
        }
        count = ringCount;
    }
    if (count <= 0 || sum.samples <= 0)
        return;

    const double len = sum.samples;
    AnalysisState& s = state;
    s.valid = count >= kMinFrames;
    s.windowSeconds = static_cast<float> (len / sampleRate);
    s.loudnessLufs = -0.691f + powerDb (sum.kPower / len);
    s.peakDbfs = gainToDb (sum.peak);
    s.plrDb = s.peakDbfs - s.loudnessLufs;
    s.crestDb = s.peakDbfs - powerDb (sum.plainPower / (2.0 * len));

    std::array<float, kNumBands> bandDb {};
    for (int b = 0; b < numBands; ++b)
    {
        const auto i = static_cast<size_t> (b);
        bandDb[i] = std::max (-120.0f, powerDb (sum.band[i] * bandNorm[i] / len));
    }
    // Tilt: least squares over the bands 125 Hz .. 8 kHz this rate has.
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    int nb = 0;
    for (int b = 0; b < numBands; ++b)
    {
        const auto i = static_cast<size_t> (b);
        if (kBandHz[i] < 125.0 || kBandHz[i] > 8000.0)
            continue;
        const double x = std::log2 (kBandHz[i] / 1000.0), y = bandDb[i];
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
        ++nb;
    }
    const double den = nb * sxx - sx * sx;
    s.tiltDbPerOctave = nb >= 3 && den > 0.0 ? static_cast<float> ((nb * sxy - sx * sy) / den) : 0.0f;
    // High tilt: the 8 kHz (and 16 kHz) bands over 500 Hz .. 2 kHz.
    constexpr size_t k500 = 3, k8k = 7;
    if (static_cast<size_t> (numBands) > k8k)
    {
        float hi = 0.0f;
        for (size_t b = k8k; b < static_cast<size_t> (numBands); ++b)
            hi += bandDb[b];
        hi /= static_cast<float> (static_cast<size_t> (numBands) - k8k);
        const float mid = (bandDb[k500] + bandDb[k500 + 1] + bandDb[k500 + 2]) / 3.0f;
        s.highTiltDb = hi - mid;
    }
    else
    {
        s.highTiltDb = 0.0f;
    }
    s.lowShareDb = sum.mm > 1.0e-30 ? powerDb (sum.low / sum.mm) : kMinusInfDb;
    s.fluxDb = static_cast<float> (fluxSum / count);
    s.onsetsPerSecond = static_cast<float> (sum.onsets / (len / sampleRate));
    const double lrDen = std::sqrt (sum.ll * sum.rr);
    s.correlation = lrDen > 1.0e-30 ? static_cast<float> (std::clamp (sum.lr / lrDen, -1.0, 1.0)) : 1.0f;
    s.sideDb = sum.mm > 1.0e-30 ? powerDb (sum.ss / sum.mm) : kMinusInfDb;
}

AnalysisState ContentAnalysis::analyseWhole (const float* const* channels, int numChannels, int64_t numSamples, double fs)
{
    ContentAnalysis a;
    a.prepare (fs);
    a.wholeProgramme = true;
    std::array<float*, kMaxChannels> ptrs {};
    const int nch = std::clamp (numChannels, 1, kMaxChannels);
    constexpr int kChunk = 4096;
    for (int64_t pos = 0; pos < numSamples; pos += kChunk)
    {
        const int n = static_cast<int> (std::min<int64_t> (kChunk, numSamples - pos));
        for (int c = 0; c < nch; ++c)
            ptrs[static_cast<size_t> (c)] = const_cast<float*> (channels[c] + pos); // process() only reads
        a.process (AudioBlock (ptrs.data(), nch, n));
    }
    // The last, partial frame counts too.
    if (a.current.samples > 0)
        a.closeFrame();
    return a.state;
}

void AnalysisSnapshot::publish (const AnalysisState& s) noexcept FLUB_NONBLOCKING
{
    std::array<uint32_t, kWords> w {};
    std::memcpy (w.data(), &s, sizeof (s));
    const uint32_t seq = sequence.load (std::memory_order_relaxed);
    sequence.store (seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence (std::memory_order_release);
    for (size_t i = 0; i < kWords; ++i)
        words[i].store (w[i], std::memory_order_relaxed);
    sequence.store (seq + 2, std::memory_order_release);
}

bool AnalysisSnapshot::read (AnalysisState& s) const noexcept
{
    for (int attempt = 0; attempt < 64; ++attempt)
    {
        const uint32_t s0 = sequence.load (std::memory_order_acquire);
        if ((s0 & 1u) != 0u)
            continue;
        std::array<uint32_t, kWords> w {};
        for (size_t i = 0; i < kWords; ++i)
            w[i] = words[i].load (std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_acquire);
        if (sequence.load (std::memory_order_relaxed) == s0)
        {
            if (s0 == 0u)
                return false;
            std::memcpy (static_cast<void*> (&s), w.data(), sizeof (s));
            return true;
        }
    }
    return false;
}

void AnalysisSnapshot::clear() noexcept
{
    // Readers see "nothing published" until the next publish().
    sequence.store (0, std::memory_order_release);
}
} // namespace flub
