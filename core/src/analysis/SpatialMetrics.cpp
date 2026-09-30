#include "flub/analysis/SpatialMetrics.h"

#include "flub/common/AudioBlock.h"
#include "flub/common/Denormals.h"
#include "flub/common/Math.h"
#include "flub/dsp/Fft.h"
#include "flub/dsp/StereoSpatializer.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <utility>

namespace flub
{
namespace
{
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

// Nominal 1/3-octave labels for k = -17 .. 13 (exact centre 1 kHz x 2^(k/3)).
constexpr int kFirstThird = -17;
constexpr std::array<double, 31> kNominalThirds { 20.0,   25.0,   31.5,   40.0,   50.0,   63.0,   80.0,   100.0,  125.0,  160.0,  200.0,
                                                  250.0,  315.0,  400.0,  500.0,  630.0,  800.0,  1000.0, 1250.0, 1600.0, 2000.0, 2500.0,
                                                  3150.0, 4000.0, 5000.0, 6300.0, 8000.0, 10000.0, 12500.0, 16000.0, 20000.0 };

double exactThird (size_t index) noexcept { return 1000.0 * std::pow (2.0, (static_cast<double> (index) + kFirstThird) / 3.0); }

constexpr double kOnsetFraction = 0.1;   // -20 dB re the peak
constexpr double kEarlyMs = 80.0;        // IACC early / late split
constexpr double kPreOnsetMs = 1.0;      // the early window starts this far before the onset
constexpr double kDirectPreMs = 0.5;     // DRR direct window [onset - 0.5, onset + 2.5) ms
constexpr double kDirectPostMs = 2.5;
constexpr double kMaxLagMs = 1.0;        // IACC lag range
constexpr double kEmptyWindow = 1.0e-6;   // an IACC window below -60 dB re the whole response is empty
constexpr double kNoReverb = 1.0e-10;     // DRR: after the direct sound below -100 dB re it is no reverberant part
constexpr int kWelchSize = 8192;
constexpr double kFocusSettleSeconds = 0.25; // skipped by focusIldDeviation (guard and envelopes settle)

int msToIndex (double ms, double sampleRate) noexcept { return static_cast<int> (std::lround (ms * 0.001 * sampleRate)); }

/** |X[k]|^2, k = 0 .. n/2, of x zero-padded (or cut) to n points (n a power of two). */
std::vector<double> powerSpectrum (const float* x, size_t length, int n, const float* window = nullptr)
{
    Fft fft;
    fft.prepare (n);
    std::vector<float> buf (static_cast<size_t> (n), 0.0f);
    const size_t m = std::min (length, static_cast<size_t> (n));
    for (size_t i = 0; i < m; ++i)
        buf[i] = window != nullptr ? x[i] * window[i] : x[i];
    std::vector<std::complex<float>> bins (static_cast<size_t> (n / 2 + 1));
    fft.forwardReal (buf.data(), bins.data());
    std::vector<double> p (bins.size());
    for (size_t k = 0; k < bins.size(); ++k)
        p[k] = std::norm (std::complex<double> (bins[k]));
    return p;
}

/** First and one-past-last bin whose centre lies in the 1/3-octave band. */
std::pair<size_t, size_t> bandBins (double nominalHz, double sampleRate, int n) noexcept
{
    const double fc = thirdOctaveExactHz (nominalHz);
    const double binHz = sampleRate / n;
    const auto lo = static_cast<size_t> (std::ceil (fc * std::pow (2.0, -1.0 / 6.0) / binHz));
    auto hi = static_cast<size_t> (std::ceil (fc * std::pow (2.0, 1.0 / 6.0) / binHz));
    hi = std::min (hi, static_cast<size_t> (n / 2 + 1));
    return { std::min (lo, hi), std::max (hi, std::min (lo, hi) + 1) }; // at least one bin
}

/** 2nd-order RBJ section in double (Butterworth band edges for the IACC octaves). */
struct Section
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;

    double tick (double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

Section rbjPass (bool highPass, double hz, double q, double sampleRate) noexcept
{
    const double w0 = kTwoPi * hz / sampleRate, cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * q), a0 = 1.0 + alpha;
    Section s;
    s.b0 = (highPass ? (1.0 + cw) : (1.0 - cw)) * 0.5 / a0;
    s.b1 = (highPass ? -(1.0 + cw) : (1.0 - cw)) / a0;
    s.b2 = s.b0;
    s.a1 = -2.0 * cw / a0;
    s.a2 = (1.0 - alpha) / a0;
    return s;
}

/** x through an octave band: 4th-order Butterworth high-pass at fc / sqrt 2 and low-pass at fc sqrt 2. */
std::vector<float> octaveBand (const std::vector<float>& x, double fc, double sampleRate)
{
    constexpr std::array<double, 2> kQ { 0.54119610014619698, 1.3065629648763766 }; // 4th-order Butterworth
    std::array<Section, 4> s { rbjPass (true, fc / std::sqrt (2.0), kQ[0], sampleRate), rbjPass (true, fc / std::sqrt (2.0), kQ[1], sampleRate),
                               rbjPass (false, fc * std::sqrt (2.0), kQ[0], sampleRate), rbjPass (false, fc * std::sqrt (2.0), kQ[1], sampleRate) };
    std::vector<float> y (x.size());
    for (size_t i = 0; i < x.size(); ++i)
    {
        double v = x[i];
        for (auto& sec : s)
            v = sec.tick (v);
        y[i] = static_cast<float> (v);
    }
    return y;
}

double energy (const std::vector<float>& x, int begin, int end) noexcept
{
    double e = 0.0;
    for (int i = std::max (0, begin); i < std::min (end, static_cast<int> (x.size())); ++i)
        e += static_cast<double> (x[static_cast<size_t> (i)]) * x[static_cast<size_t> (i)];
    return e;
}

/** IACC of a window, NaN when it holds less than kEmptyWindow of the total energy. */
double windowIacc (const std::vector<float>& l, const std::vector<float>& r, int begin, int end, int maxLag, double total, int* lag = nullptr)
{
    const double e = energy (l, begin, end) + energy (r, begin, end);
    if (! (total > 0.0) || e < kEmptyWindow * total)
        return kNaN;
    return interauralCrossCorrelation (l, r, begin, end, maxLag, lag);
}
} // namespace

std::vector<double> thirdOctaveBands (double sampleRate, double loHz, double hiHz)
{
    std::vector<double> bands;
    for (size_t k = 0; k < kNominalThirds.size(); ++k)
        if (kNominalThirds[k] >= loHz * 0.999 && kNominalThirds[k] <= hiHz * 1.001 && exactThird (k) * std::pow (2.0, 1.0 / 6.0) < 0.5 * sampleRate)
            bands.push_back (kNominalThirds[k]);
    return bands;
}

double thirdOctaveExactHz (double nominalHz) noexcept
{
    size_t best = 0;
    for (size_t k = 1; k < kNominalThirds.size(); ++k)
        if (std::abs (std::log (kNominalThirds[k] / nominalHz)) < std::abs (std::log (kNominalThirds[best] / nominalHz)))
            best = k;
    return exactThird (best);
}

int directOnset (const BinauralIr& ir) noexcept
{
    const size_t n = std::min (ir.left.size(), ir.right.size());
    float peak = 0.0f;
    for (size_t i = 0; i < n; ++i)
        peak = std::max ({ peak, std::abs (ir.left[i]), std::abs (ir.right[i]) });
    if (! (peak > 0.0f) || ! std::isfinite (peak))
        return -1;
    const auto threshold = static_cast<float> (kOnsetFraction) * peak;
    for (size_t i = 0; i < n; ++i)
        if (std::max (std::abs (ir.left[i]), std::abs (ir.right[i])) >= threshold)
            return static_cast<int> (i);
    return -1;
}

double interauralCrossCorrelation (const std::vector<float>& l, const std::vector<float>& r, int begin, int end, int maxLag, int* lagOut)
{
    const int n = static_cast<int> (std::min (l.size(), r.size()));
    begin = std::max (0, begin);
    end = std::min (end, n);
    const double el = energy (l, begin, end), er = energy (r, begin, end);
    if (lagOut != nullptr)
        *lagOut = 0;
    if (! (el > 0.0) || ! (er > 0.0) || ! std::isfinite (el * er))
        return kNaN;
    double best = -1.0;
    int bestLag = 0;
    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        double acc = 0.0;
        for (int i = std::max (begin, -lag); i < std::min (end, n - lag); ++i)
            acc += static_cast<double> (l[static_cast<size_t> (i)]) * r[static_cast<size_t> (i + lag)];
        // Near-ties go to the smaller |lag| (a centred source reads lag 0).
        const double v = std::abs (acc);
        const bool tie = std::abs (v - best) <= 1.0e-9 * best;
        if ((v > best && ! tie) || (tie && std::abs (lag) < std::abs (bestLag)))
        {
            best = std::max (best, v);
            bestLag = lag;
        }
    }
    if (lagOut != nullptr)
        *lagOut = bestLag;
    return std::min (1.0, best / std::sqrt (el * er));
}

std::vector<double> thirdOctaveLevelsDb (const std::vector<float>& ir, double sampleRate, const std::vector<double>& bandsHz)
{
    const int n = nextPowerOfTwo (std::max ({ static_cast<int> (ir.size()), static_cast<int> (std::ceil (0.5 * sampleRate)), 2 }));
    const auto p = powerSpectrum (ir.data(), ir.size(), n);
    std::vector<double> levels;
    levels.reserve (bandsHz.size());
    for (double hz : bandsHz)
    {
        const auto [lo, hi] = bandBins (hz, sampleRate, n);
        double sum = 0.0;
        for (size_t k = lo; k < hi && k < p.size(); ++k)
            sum += p[k];
        const double mean = sum / static_cast<double> (hi - lo);
        levels.push_back (mean > 0.0 ? 10.0 * std::log10 (mean) : static_cast<double> (kMinusInfDb));
    }
    return levels;
}

BinauralIrMetrics analyseBinauralIr (const BinauralIr& ir, double sampleRate)
{
    BinauralIrMetrics m;
    m.iaccEarly = m.iaccLate = m.itdMs = kNaN;
    m.iaccEarlyBands.fill (kNaN);
    m.iaccLateBands.fill (kNaN);
    const int onset = directOnset (ir);
    if (onset < 0 || ! (sampleRate > 0.0))
        return m;
    m.valid = true;
    ScopedNoDenormals noDenormals;

    const int n = static_cast<int> (std::min (ir.left.size(), ir.right.size()));
    const int earlyBegin = onset - msToIndex (kPreOnsetMs, sampleRate);
    const int split = onset + msToIndex (kEarlyMs, sampleRate);
    const int maxLag = std::max (1, msToIndex (kMaxLagMs, sampleRate));
    m.onsetSeconds = onset / sampleRate;

    const double total = energy (ir.left, 0, n) + energy (ir.right, 0, n);
    int lag = 0;
    m.iaccEarly = windowIacc (ir.left, ir.right, earlyBegin, split, maxLag, total, &lag);
    m.itdMs = std::isnan (m.iaccEarly) ? kNaN : 1000.0 * lag / sampleRate;
    m.iaccLate = windowIacc (ir.left, ir.right, split, n, maxLag, total);
    for (size_t b = 0; b < kIaccOctavesHz.size(); ++b)
    {
        const double fc = kIaccOctavesHz[b];
        if (fc * std::sqrt (2.0) >= 0.45 * sampleRate)
            continue;
        const auto bl = octaveBand (ir.left, fc, sampleRate), br = octaveBand (ir.right, fc, sampleRate);
        const double bandTotal = energy (bl, 0, n) + energy (br, 0, n);
        m.iaccEarlyBands[b] = windowIacc (bl, br, earlyBegin, split, maxLag, bandTotal);
        m.iaccLateBands[b] = windowIacc (bl, br, split, n, maxLag, bandTotal);
    }

    const int directEnd = onset + msToIndex (kDirectPostMs, sampleRate);
    const double direct = energy (ir.left, onset - msToIndex (kDirectPreMs, sampleRate), directEnd)
                          + energy (ir.right, onset - msToIndex (kDirectPreMs, sampleRate), directEnd);
    const double rest = energy (ir.left, directEnd, n) + energy (ir.right, directEnd, n);
    m.drrDb = rest < kNoReverb * direct ? kInf : 10.0 * std::log10 (direct / rest);

    m.bandsHz = thirdOctaveBands (sampleRate);
    m.leftDb = thirdOctaveLevelsDb (ir.left, sampleRate, m.bandsHz);
    m.rightDb = thirdOctaveLevelsDb (ir.right, sampleRate, m.bandsHz);
    return m;
}

DiffuseField diffuseField (const std::vector<BinauralIr>& irs, double sampleRate, double loHz, double hiHz)
{
    DiffuseField d;
    d.bandsHz = thirdOctaveBands (sampleRate, loHz, hiHz);
    if (irs.empty() || d.bandsHz.empty())
        return d;
    std::vector<double> power (d.bandsHz.size(), 0.0);
    for (const auto& ir : irs)
        for (const auto* ear : { &ir.left, &ir.right })
        {
            const auto levels = thirdOctaveLevelsDb (*ear, sampleRate, d.bandsHz);
            for (size_t b = 0; b < levels.size(); ++b)
                power[b] += std::pow (10.0, 0.1 * levels[b]);
        }
    double mean = 0.0;
    for (double& p : power)
    {
        p /= 2.0 * static_cast<double> (irs.size());
        d.levelDb.push_back (p > 0.0 ? 10.0 * std::log10 (p) : static_cast<double> (kMinusInfDb));
        mean += d.levelDb.back();
    }
    mean /= static_cast<double> (d.levelDb.size());
    double lo = kInf, hi = -kInf, squares = 0.0;
    for (double v : d.levelDb)
    {
        d.deviationDb.push_back (v - mean);
        lo = std::min (lo, v - mean);
        hi = std::max (hi, v - mean);
        squares += (v - mean) * (v - mean);
    }
    d.rangeDb = hi - lo;
    d.rmsDeviationDb = std::sqrt (squares / static_cast<double> (d.deviationDb.size()));
    return d;
}

double peakToNotchDb (const std::vector<float>& ir, double sampleRate, double loHz, double hiHz)
{
    const int n = nextPowerOfTwo (std::max (static_cast<int> (ir.size()), 8192));
    const auto p = powerSpectrum (ir.data(), ir.size(), n);
    double lo = kInf, hi = -kInf;
    for (size_t k = 0; k < p.size(); ++k)
    {
        const double f = static_cast<double> (k) * sampleRate / n;
        if (f < loHz || f > hiHz)
            continue;
        const double db = 10.0 * std::log10 (std::max (p[k], 1.0e-30));
        lo = std::min (lo, db);
        hi = std::max (hi, db);
    }
    return hi >= lo ? hi - lo : 0.0;
}

std::vector<BinauralIr> splitImpulses (const std::vector<float>& left, const std::vector<float>& right, double sampleRate,
                                       double minSpacingSeconds, std::vector<double>* onsetSeconds)
{
    std::vector<BinauralIr> irs;
    if (onsetSeconds != nullptr)
        onsetSeconds->clear();
    const size_t n = std::min (left.size(), right.size());
    if (n == 0 || ! (sampleRate > 0.0))
        return irs;
    float peak = 0.0f;
    for (size_t i = 0; i < n; ++i)
        if (std::isfinite (left[i]) && std::isfinite (right[i]))
            peak = std::max ({ peak, std::abs (left[i]), std::abs (right[i]) });
    if (! (peak > 0.0f))
        return irs;

    const auto threshold = static_cast<float> (kOnsetFraction) * peak;
    const auto spacing = static_cast<size_t> (std::max (1L, std::lround (minSpacingSeconds * sampleRate)));
    std::vector<size_t> onsets;
    for (size_t i = 0; i < n; ++i)
        if (std::max (std::abs (left[i]), std::abs (right[i])) >= threshold && (onsets.empty() || i - onsets.back() >= spacing))
            onsets.push_back (i);

    const auto pre = static_cast<size_t> (std::max (0, msToIndex (kPreOnsetMs, sampleRate)));
    for (size_t k = 0; k < onsets.size(); ++k)
    {
        const size_t begin = onsets[k] >= pre ? onsets[k] - pre : 0;
        const size_t end = k + 1 < onsets.size() ? onsets[k + 1] - pre : n;
        BinauralIr ir;
        ir.left.assign (left.begin() + static_cast<std::ptrdiff_t> (begin), left.begin() + static_cast<std::ptrdiff_t> (end));
        ir.right.assign (right.begin() + static_cast<std::ptrdiff_t> (begin), right.begin() + static_cast<std::ptrdiff_t> (end));
        irs.push_back (std::move (ir));
        if (onsetSeconds != nullptr)
            onsetSeconds->push_back (static_cast<double> (onsets[k]) / sampleRate);
    }
    return irs;
}

BinauralIr virtualizerResponse (const VirtualizerParams& params, uint32_t channelMask, double sampleRate, int length)
{
    VirtualizerParams p = params;
    p.levelMatch = false;
    p.foldHeadroom = false;
    HeadphoneVirtualizer v;
    v.setParams (p);
    constexpr int kBlock = 512;
    v.prepare ({ sampleRate, kBlock, kMaxChannels });

    const int numLayout = channelCount (p.layout);
    const bool withLfe = p.layout != ChannelLayout::Stereo;
    length = std::max (1, length);
    AudioBuffer buffer (kMaxChannels, length);
    for (int c = 0; c < numLayout; ++c)
        if (((channelMask >> c) & 1u) != 0 && ! (withLfe && c == 3))
            buffer.channel (c)[0] = 1.0f;
    for (int pos = 0; pos < length; pos += kBlock)
    {
        std::array<float*, kMaxChannels> ptrs {};
        for (int c = 0; c < kMaxChannels; ++c)
            ptrs[static_cast<size_t> (c)] = buffer.channel (c) + pos;
        v.process (AudioBlock (ptrs.data(), kMaxChannels, std::min (kBlock, length - pos)));
    }
    BinauralIr ir;
    ir.left.assign (buffer.channel (0), buffer.channel (0) + length);
    ir.right.assign (buffer.channel (1), buffer.channel (1) + length);
    return ir;
}

// ---- HRTF-rendered ILD (docs/11 E24) ---------------------------------------

std::vector<float> focusIldAzimuths()
{
    std::vector<float> az;
    for (int a = 0; a <= 180; a += 15)
        az.push_back (static_cast<float> (a));
    return az;
}

std::vector<std::vector<float>> hrtfRenderedSource (const std::vector<float>& mono, float azimuthDeg, double sampleRate, float headRadiusMm,
                                                    int hrirLength)
{
    std::vector<float> hl, hr;
    HeadphoneVirtualizer::parametricHrir (azimuthDeg, headRadiusMm, sampleRate, hrirLength, hl, hr);
    // The tail of the model's responses is negligible: convolve only up to
    // the last tap above -140 dB re the larger ear's peak.
    float peak = 0.0f;
    for (size_t i = 0; i < hl.size(); ++i)
        peak = std::max ({ peak, std::abs (hl[i]), std::abs (hr[i]) });
    size_t taps = hl.size();
    while (taps > 1 && std::abs (hl[taps - 1]) < 1.0e-7f * peak && std::abs (hr[taps - 1]) < 1.0e-7f * peak)
        --taps;

    std::vector<std::vector<float>> out (2, std::vector<float> (mono.size(), 0.0f));
    for (size_t i = 0; i < mono.size(); ++i)
    {
        double yl = 0.0, yr = 0.0;
        for (size_t k = 0; k < taps && k <= i; ++k)
        {
            yl += static_cast<double> (hl[k]) * mono[i - k];
            yr += static_cast<double> (hr[k]) * mono[i - k];
        }
        out[0][i] = static_cast<float> (yl);
        out[1][i] = static_cast<float> (yr);
    }
    return out;
}

std::vector<double> bandIldDb (const std::vector<float>& left, const std::vector<float>& right, double sampleRate,
                               const std::vector<double>& bandsHz)
{
    const size_t n = std::min (left.size(), right.size());
    std::vector<float> window (static_cast<size_t> (kWelchSize));
    for (size_t i = 0; i < window.size(); ++i)
        window[i] = static_cast<float> (0.5 - 0.5 * std::cos (kTwoPi * static_cast<double> (i) / kWelchSize));
    std::vector<double> pl (static_cast<size_t> (kWelchSize / 2 + 1), 0.0), pr (pl.size(), 0.0);
    constexpr size_t kHop = kWelchSize / 2;
    for (size_t start = 0; start == 0 || start + static_cast<size_t> (kWelchSize) <= n; start += kHop)
    {
        const size_t len = std::min (static_cast<size_t> (kWelchSize), n - std::min (n, start));
        const auto sl = powerSpectrum (left.data() + start, len, kWelchSize, window.data());
        const auto sr = powerSpectrum (right.data() + start, len, kWelchSize, window.data());
        for (size_t k = 0; k < pl.size(); ++k)
        {
            pl[k] += sl[k];
            pr[k] += sr[k];
        }
        if (n < static_cast<size_t> (kWelchSize))
            break;
    }
    std::vector<double> ild;
    ild.reserve (bandsHz.size());
    for (double hz : bandsHz)
    {
        const auto [lo, hi] = bandBins (hz, sampleRate, kWelchSize);
        double a = 0.0, b = 0.0;
        for (size_t k = lo; k < hi && k < pl.size(); ++k)
        {
            a += pl[k];
            b += pr[k];
        }
        ild.push_back (a > 0.0 && b > 0.0 ? 10.0 * std::log10 (a / b) : kNaN);
    }
    return ild;
}

IldDeviation ildDeviation (const std::vector<std::vector<float>>& reference, const std::vector<std::vector<float>>& measured, double sampleRate,
                           double loHz, double hiHz, double summaryLoHz, double summaryHiHz)
{
    IldDeviation d;
    if (reference.size() < 2 || measured.size() < 2)
        return d;
    d.bandsHz = thirdOctaveBands (sampleRate, loHz, hiHz);
    d.referenceDb = bandIldDb (reference[0], reference[1], sampleRate, d.bandsHz);
    d.measuredDb = bandIldDb (measured[0], measured[1], sampleRate, d.bandsHz);
    double sum = 0.0;
    int count = 0;
    for (size_t b = 0; b < d.bandsHz.size(); ++b)
    {
        const double dev = d.measuredDb[b] - d.referenceDb[b];
        d.deviationDb.push_back (dev);
        if (d.bandsHz[b] < summaryLoHz * 0.999 || d.bandsHz[b] > summaryHiHz * 1.001 || std::isnan (dev))
            continue;
        sum += dev;
        ++count;
        if (std::abs (dev) > d.maxAbsDb)
        {
            d.maxAbsDb = std::abs (dev);
            d.maxAbsHz = d.bandsHz[b];
        }
    }
    d.meanDb = count > 0 ? sum / count : 0.0;
    return d;
}

std::vector<std::vector<float>> applyPositionalFocus (const std::vector<std::vector<float>>& stereo, float focus, double sampleRate)
{
    std::vector<std::vector<float>> out = stereo;
    if (out.size() < 2)
        return out;
    out.resize (2);
    const int n = static_cast<int> (std::min (out[0].size(), out[1].size()));
    StereoSpatializer s;
    SpatializerParams p;
    p.width = 1.0f;
    p.space = 0.0f;
    p.crossfeed = 0.0f;
    p.positionalFocus = std::clamp (focus, 0.0f, 1.0f);
    constexpr int kBlock = 512;
    s.prepare ({ sampleRate, kBlock, 2 });
    s.setParams (p);
    s.reset(); // start at the targets (no glide from the defaults)
    for (int pos = 0; pos < n; pos += kBlock)
    {
        std::array<float*, 2> ptrs { out[0].data() + pos, out[1].data() + pos };
        s.process (AudioBlock (ptrs.data(), 2, std::min (kBlock, n - pos)));
    }
    return out;
}

std::array<IldDeviation, 3> focusIldDeviation (const std::vector<std::vector<float>>& source, double sampleRate)
{
    std::array<IldDeviation, 3> r;
    if (source.size() < 2)
        return r;
    // Both measured after the first 250 ms, where the focus guard's
    // envelopes and the band-pass detectors have settled.
    const size_t n = std::min (source[0].size(), source[1].size());
    const auto skip = static_cast<std::ptrdiff_t> (std::min (n / 2, static_cast<size_t> (kFocusSettleSeconds * sampleRate)));
    const auto end = static_cast<std::ptrdiff_t> (n);
    const auto tail = [skip, end] (const std::vector<std::vector<float>>& x) {
        return std::vector<std::vector<float>> { std::vector<float> (x[0].begin() + skip, x[0].begin() + end),
                                                 std::vector<float> (x[1].begin() + skip, x[1].begin() + end) };
    };
    const auto reference = tail (source);
    for (size_t k = 0; k < kFocusIldAmounts.size(); ++k)
        r[k] = ildDeviation (reference, tail (applyPositionalFocus (source, kFocusIldAmounts[k], sampleRate)), sampleRate);
    return r;
}
} // namespace flub
