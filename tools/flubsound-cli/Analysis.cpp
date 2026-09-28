#include "Analysis.h"

#include "flub/analysis/LoudnessMeter.h"
#include "flub/analysis/PeakMeters.h"
#include "flub/common/Denormals.h"
#include "flub/common/Math.h"
#include "flub/dsp/Fft.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdio>
#include <sstream>
#include <utility>

namespace flub::cli
{
namespace
{
constexpr int kAnalysisBlock = 4096;

/** Values at (or below) this are "no measurement". */
constexpr float kNoMeasurement = kMinusInfDb + 0.5f;

bool isMeasured (float v) noexcept { return std::isfinite (v) && v > kNoMeasurement; }

/** Rounds for JSON so reports stay readable ("-23.02", not "-23.0199947357"). */
json::Value jsonNumber (float v, int decimals)
{
    if (! isMeasured (v))
        return json::Value();
    const double scale = std::pow (10.0, decimals);
    return json::Value (std::round (static_cast<double> (v) * scale) / scale);
}

json::Value jsonArray (const std::vector<float>& values, int decimals)
{
    json::Value a { json::Value::Array {} };
    for (float v : values)
        a.push (jsonNumber (v, decimals));
    return a;
}
} // namespace

const char* sampleFormatName (io::SampleFormat format) noexcept
{
    switch (format)
    {
        case io::SampleFormat::Pcm16: return "pcm16";
        case io::SampleFormat::Pcm24: return "pcm24";
        case io::SampleFormat::Pcm32: return "pcm32";
        case io::SampleFormat::Float32: return "float32";
        case io::SampleFormat::Float64: return "float64";
    }
    return "unknown";
}

std::string formatDb (float value, int decimals)
{
    if (! isMeasured (value))
        return "-inf";
    char buf[32];
    std::snprintf (buf, sizeof (buf), "%.*f", decimals, static_cast<double> (value));
    return buf;
}

LoudnessReport analyse (const std::vector<std::vector<float>>& channels, double sampleRate)
{
    LoudnessReport r;
    r.sampleRate = sampleRate;
    r.numChannels = std::min (static_cast<int> (channels.size()), kMaxChannels);
    r.numFrames = r.numChannels > 0 ? static_cast<int64_t> (channels[0].size()) : 0;
    r.durationSeconds = sampleRate > 0.0 ? static_cast<double> (r.numFrames) / sampleRate : 0.0;

    const int nch = r.numChannels;
    r.channelTruePeakDbtp.assign (static_cast<size_t> (nch), kMinusInfDb);
    r.channelSamplePeakDbfs.assign (static_cast<size_t> (nch), kMinusInfDb);
    r.channelRmsDbfs.assign (static_cast<size_t> (nch), kMinusInfDb);
    if (nch == 0 || sampleRate <= 0.0)
        return r;

    ScopedNoDenormals noDenormals;

    LoudnessMeter loudness;
    loudness.prepare (sampleRate, nch);
    TruePeakMeter truePeak;
    truePeak.prepare (nch);

    std::vector<double> sumSquares (static_cast<size_t> (nch), 0.0);
    std::vector<float> samplePeak (static_cast<size_t> (nch), 0.0f);

    // The meters take an AudioBlock (mutable channel pointers) but only ever
    // read it (documented in LoudnessMeter.h / PeakMeters.h), so viewing the
    // caller's const data through it is safe.
    std::array<float*, kMaxChannels> pointers {};
    for (int c = 0; c < nch; ++c)
        pointers[static_cast<size_t> (c)] = const_cast<float*> (channels[static_cast<size_t> (c)].data());

    for (int64_t pos = 0; pos < r.numFrames; pos += kAnalysisBlock)
    {
        const int n = static_cast<int> (std::min<int64_t> (kAnalysisBlock, r.numFrames - pos));
        // Built by hand: the AudioBlock offset constructor takes an int, and
        // long programmes can exceed 2^31 frames.
        AudioBlock view;
        view.numChannels = nch;
        view.numSamples = n;
        for (int c = 0; c < nch; ++c)
            view.ch[static_cast<size_t> (c)] = pointers[static_cast<size_t> (c)] + pos;

        loudness.process (view);
        truePeak.process (view);

        for (int c = 0; c < nch; ++c)
        {
            const float* d = view.channel (c);
            double ss = 0.0;
            float pk = samplePeak[static_cast<size_t> (c)];
            for (int i = 0; i < n; ++i)
            {
                ss += static_cast<double> (d[i]) * static_cast<double> (d[i]);
                pk = std::max (pk, std::abs (d[i]));
            }
            sumSquares[static_cast<size_t> (c)] += ss;
            samplePeak[static_cast<size_t> (c)] = pk;
        }
    }

    // Flush the true-peak interpolator (it runs kDelay samples behind and
    // needs half its kernel of look-ahead for the last inter-sample peaks).
    {
        AudioBuffer silence (nch, TruePeakDetector::kTapsPerPhase);
        truePeak.process (silence.block());
    }

    r.integratedLufs = loudness.getIntegratedLufs();
    r.loudnessRangeLu = loudness.getLoudnessRangeLu();
    r.maxMomentaryLufs = loudness.getMaxMomentaryLufs();
    r.maxShortTermLufs = loudness.getMaxShortTermLufs();

    double totalSquares = 0.0;
    float overallPeak = 0.0f;
    for (int c = 0; c < nch; ++c)
    {
        const auto cs = static_cast<size_t> (c);
        r.channelTruePeakDbtp[cs] = truePeak.getMaxDb (c);
        r.channelSamplePeakDbfs[cs] = gainToDb (samplePeak[cs]);
        const double meanSquare = r.numFrames > 0 ? sumSquares[cs] / static_cast<double> (r.numFrames) : 0.0;
        r.channelRmsDbfs[cs] = powerToDb (static_cast<float> (meanSquare));
        totalSquares += sumSquares[cs];
        overallPeak = std::max (overallPeak, samplePeak[cs]);
    }
    r.truePeakDbtp = truePeak.getMaxDbAllChannels();
    r.samplePeakDbfs = gainToDb (overallPeak);
    const double totalCount = static_cast<double> (r.numFrames) * nch;
    r.rmsDbfs = powerToDb (static_cast<float> (totalCount > 0.0 ? totalSquares / totalCount : 0.0));
    return r;
}

std::string formatReport (const LoudnessReport& r, const std::string& title, const std::string& sourceFormat)
{
    std::ostringstream s;
    char buf[160];

    s << title << "\n";
    std::snprintf (buf, sizeof (buf), "  Format            : %d ch, %.0f Hz, %s, %lld frames (%.3f s)\n", r.numChannels, r.sampleRate,
                   sourceFormat.c_str(), static_cast<long long> (r.numFrames), r.durationSeconds);
    s << buf;
    s << "  Integrated        : " << formatDb (r.integratedLufs, 2) << " LUFS\n";
    s << "  Loudness range    : " << formatDb (r.loudnessRangeLu, 2) << " LU\n";
    s << "  Momentary max     : " << formatDb (r.maxMomentaryLufs, 2) << " LUFS\n";
    s << "  Short-term max    : " << formatDb (r.maxShortTermLufs, 2) << " LUFS\n";
    s << "  True peak         : " << formatDb (r.truePeakDbtp, 2) << " dBTP\n";
    s << "  Sample peak       : " << formatDb (r.samplePeakDbfs, 2) << " dBFS\n";
    s << "  RMS               : " << formatDb (r.rmsDbfs, 2) << " dBFS\n";
    if (isMeasured (r.truePeakDbtp) && isMeasured (r.integratedLufs))
        s << "  PLR (TP - I)      : " << formatDb (r.truePeakDbtp - r.integratedLufs, 2) << " LU\n";

    if (r.numChannels > 1)
    {
        s << "  Per channel       :  ch   true peak   sample peak       RMS\n";
        for (int c = 0; c < r.numChannels; ++c)
        {
            const auto cs = static_cast<size_t> (c);
            std::snprintf (buf, sizeof (buf), "                      %2d  %8s dBTP  %8s dBFS  %8s dBFS\n", c + 1,
                           formatDb (r.channelTruePeakDbtp[cs], 2).c_str(), formatDb (r.channelSamplePeakDbfs[cs], 2).c_str(),
                           formatDb (r.channelRmsDbfs[cs], 2).c_str());
            s << buf;
        }
    }
    return s.str();
}

json::Value reportToJson (const LoudnessReport& r, const std::string& file, const std::string& sourceFormat)
{
    json::Value v;
    v.set ("file", file);
    v.set ("sampleRate", r.sampleRate);
    v.set ("channels", r.numChannels);
    v.set ("frames", static_cast<double> (r.numFrames));
    v.set ("durationSeconds", std::round (r.durationSeconds * 1000.0) / 1000.0);
    v.set ("sourceFormat", sourceFormat);
    v.set ("integratedLufs", jsonNumber (r.integratedLufs, 2));
    v.set ("loudnessRangeLu", jsonNumber (r.loudnessRangeLu, 2));
    v.set ("maxMomentaryLufs", jsonNumber (r.maxMomentaryLufs, 2));
    v.set ("maxShortTermLufs", jsonNumber (r.maxShortTermLufs, 2));
    v.set ("truePeakDbtp", jsonNumber (r.truePeakDbtp, 2));
    v.set ("samplePeakDbfs", jsonNumber (r.samplePeakDbfs, 2));
    v.set ("rmsDbfs", jsonNumber (r.rmsDbfs, 2));
    if (isMeasured (r.truePeakDbtp) && isMeasured (r.integratedLufs))
        v.set ("plrLu", jsonNumber (r.truePeakDbtp - r.integratedLufs, 2));
    else
        v.set ("plrLu", json::Value());

    json::Value perChannel;
    perChannel.set ("truePeakDbtp", jsonArray (r.channelTruePeakDbtp, 2));
    perChannel.set ("samplePeakDbfs", jsonArray (r.channelSamplePeakDbfs, 2));
    perChannel.set ("rmsDbfs", jsonArray (r.channelRmsDbfs, 2));
    v.set ("perChannel", std::move (perChannel));
    return v;
}
std::vector<BandLevel> octaveBands (const std::vector<std::vector<float>>& channels, double sampleRate)
{
    std::vector<BandLevel> bands;
    if (channels.empty() || channels[0].empty() || ! (sampleRate > 0.0))
        return bands;
    const size_t n = channels[0].size();
    std::vector<double> mid (n, 0.0);
    for (const auto& c : channels)
        for (size_t i = 0; i < n && i < c.size(); ++i)
            mid[i] += c[i];
    for (auto& v : mid)
        v /= static_cast<double> (channels.size());

    // Exact octave centres 1 kHz x 2^k, labelled with their nominal values.
    constexpr double kQ = 1.41421356237309505; // ~1 octave between the -3 dB points
    constexpr std::array<float, 10> kNominalHz { 31.5f, 63.0f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f };
    for (size_t k = 0; k < kNominalHz.size(); ++k)
    {
        const double centre = 1000.0 * std::pow (2.0, static_cast<double> (k) - 5.0);
        if (centre >= 0.4 * sampleRate)
            break;
        const double w0 = kTwoPi * centre / sampleRate, alpha = std::sin (w0) / (2.0 * kQ), a0 = 1.0 + alpha;
        const double b0 = alpha / a0, a1 = -2.0 * std::cos (w0) / a0, a2 = (1.0 - alpha) / a0;
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0, acc = 0.0;
        for (double x : mid)
        {
            const double y = b0 * (x - x2) - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;
            acc += y * y;
        }
        const double ms = acc / static_cast<double> (n);
        bands.push_back ({ kNominalHz[k], ms > 0.0 ? std::max (kMinusInfDb, static_cast<float> (10.0 * std::log10 (ms))) : kMinusInfDb });
    }
    return bands;
}

json::Value bandsToJson (const std::vector<BandLevel>& bands)
{
    json::Value a { json::Value::Array {} };
    for (const auto& b : bands)
    {
        json::Value v;
        v.set ("hz", static_cast<double> (b.centreHz));
        v.set ("db", jsonNumber (b.levelDb, 2));
        a.push (std::move (v));
    }
    return a;
}

std::string formatBands (const std::vector<BandLevel>& bands)
{
    std::string s;
    for (const auto& b : bands)
    {
        char buf[48];
        if (b.centreHz >= 1000.0f)
            std::snprintf (buf, sizeof (buf), "%gk %s", static_cast<double> (b.centreHz) / 1000.0, formatDb (b.levelDb, 1).c_str());
        else
            std::snprintf (buf, sizeof (buf), "%g %s", static_cast<double> (b.centreHz), formatDb (b.levelDb, 1).c_str());
        s += (s.empty() ? "" : "  ") + std::string (buf);
    }
    return s + " (octave band Hz: dBFS)";
}

// ===========================================================================
// Sound-quality metrics (docs/11 E59)
// ===========================================================================
namespace
{
/** sum x e^{-j w i} over x[0, n): (re, im) of the DFT projection at freqHz. */
std::pair<double, double> project (const float* x, int n, double sampleRate, double freqHz)
{
    double re = 0.0, im = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double a = kTwoPi * freqHz * i / sampleRate;
        re += x[i] * std::cos (a);
        im += x[i] * std::sin (a);
    }
    return { re, im };
}

/** Power (A^2 / 2) of the component at freqHz over an integer number of its periods. */
double tonePower (const float* x, int n, double sampleRate, double freqHz)
{
    const auto [re, im] = project (x, n, sampleRate, freqHz);
    return 2.0 * (re * re + im * im) / (static_cast<double> (n) * n);
}

/** Mean square and mean of x[0, n). */
std::pair<double, double> meanSquareAndMean (const float* x, int n)
{
    double total = 0.0, mean = 0.0;
    for (int i = 0; i < n; ++i)
    {
        total += static_cast<double> (x[i]) * x[i];
        mean += x[i];
    }
    return { total / n, mean / n };
}

double ratioDb (double num, double den) { return 10.0 * std::log10 (std::max (1.0e-30, num)) - 10.0 * std::log10 (std::max (1.0e-30, den)); }

bool nearHarmonic (double f, double f0) noexcept
{
    const double h = std::round (f / f0);
    return h >= 1.0 && std::abs (f - h * f0) < 1.0e-6;
}
} // namespace

double sineThdnDb (const float* x, int n, double sampleRate, double f0)
{
    if (n <= 0 || ! (sampleRate > 0.0))
        return 0.0;
    const auto [total, mean] = meanSquareAndMean (x, n);
    const double residual = std::max (0.0, total - mean * mean - tonePower (x, n, sampleRate, f0));
    return ratioDb (residual, total);
}

double multitoneResidualDb (const float* x, int n, double sampleRate, const std::vector<double>& tones)
{
    if (n <= 0 || ! (sampleRate > 0.0) || tones.empty())
        return 0.0;
    const auto [total, mean] = meanSquareAndMean (x, n);
    double excited = 0.0;
    for (double f : tones)
        excited += tonePower (x, n, sampleRate, f);
    return ratioDb (std::max (0.0, total - mean * mean - excited), excited);
}

double twoToneImdDb (const float* x, int n, double sampleRate, double f1, double f2, int maxOrder)
{
    if (n <= 0 || ! (sampleRate > 0.0))
        return 0.0;
    std::vector<double> products;
    for (int m = 1; m < maxOrder; ++m)
        for (int k = 1; m + k <= maxOrder; ++k)
            for (double f : { m * f1 + k * f2, std::abs (m * f1 - k * f2) })
            {
                if (! (f > 0.0) || f >= 0.5 * sampleRate || nearHarmonic (f, f1) || nearHarmonic (f, f2))
                    continue;
                if (std::none_of (products.begin(), products.end(), [f] (double p) { return std::abs (p - f) < 1.0e-6; }))
                    products.push_back (f);
            }
    double imd = 0.0;
    for (double f : products)
        imd += tonePower (x, n, sampleRate, f);
    return ratioDb (imd, tonePower (x, n, sampleRate, f1) + tonePower (x, n, sampleRate, f2));
}

double smpteImdDb (const float* x, int n, double sampleRate, double fLow, double fHigh, int sidebands)
{
    if (n <= 0 || ! (sampleRate > 0.0))
        return 0.0;
    double side = 0.0;
    for (int k = 1; k <= sidebands; ++k)
        for (double f : { fHigh - k * fLow, fHigh + k * fLow })
            if (f > 0.0 && f < 0.5 * sampleRate)
                side += tonePower (x, n, sampleRate, f);
    return ratioDb (side, tonePower (x, n, sampleRate, fHigh));
}

std::vector<double> toneGainTrack (const std::vector<float>& out, const std::vector<float>& in, double sampleRate, double freqHz,
                                   int begin, int end)
{
    const int len = static_cast<int> (std::lround (0.020 * sampleRate)), hop = static_cast<int> (std::lround (0.005 * sampleRate));
    std::vector<double> gains;
    if (len <= 0 || hop <= 0)
        return gains;
    end = std::min ({ end, static_cast<int> (out.size()), static_cast<int> (in.size()) });
    // Hann-weighted projection; the phase reference is the window start (the
    // magnitude does not depend on it), so one table serves every window.
    std::vector<double> wc (static_cast<size_t> (len)), ws (static_cast<size_t> (len));
    for (int i = 0; i < len; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (kTwoPi * i / len), a = kTwoPi * freqHz * i / sampleRate;
        wc[static_cast<size_t> (i)] = w * std::cos (a);
        ws[static_cast<size_t> (i)] = w * std::sin (a);
    }
    const auto amplitude = [&] (const std::vector<float>& x, int start) {
        double re = 0.0, im = 0.0;
        for (int i = 0; i < len; ++i)
        {
            const double v = x[static_cast<size_t> (start + i)];
            re += v * wc[static_cast<size_t> (i)];
            im += v * ws[static_cast<size_t> (i)];
        }
        return std::sqrt (re * re + im * im);
    };
    for (int start = std::max (0, begin); start + len <= end; start += hop)
        gains.push_back (20.0 * std::log10 (std::max (1.0e-12, amplitude (out, start)) / std::max (1.0e-12, amplitude (in, start))));
    return gains;
}

// ---- signal hygiene (docs/11 E10) ----------------------------------------
namespace
{
/** |X[k]|, k = 0..n/2, of x[0, n) under a 4-term Blackman-Harris window (sidelobes -92 dB). */
std::vector<double> windowedMagnitudes (const float* x, int n)
{
    Fft fft;
    fft.prepare (n);
    std::vector<float> w (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        const double t = kTwoPi * i / n;
        const double win = 0.35875 - 0.48829 * std::cos (t) + 0.14128 * std::cos (2.0 * t) - 0.01168 * std::cos (3.0 * t);
        w[static_cast<size_t> (i)] = static_cast<float> (x[i] * win);
    }
    std::vector<std::complex<float>> bins (static_cast<size_t> (n / 2 + 1));
    fft.forwardReal (w.data(), bins.data());
    std::vector<double> m (bins.size());
    for (size_t k = 0; k < bins.size(); ++k)
        m[k] = std::abs (bins[k]);
    return m;
}

bool isPowerOfTwo (int n) noexcept { return n >= 2 && (n & (n - 1)) == 0; }
} // namespace

int aliasToneBin (double freqHz, double sampleRate, int n) noexcept
{
    int bin = static_cast<int> (std::lround (freqHz / sampleRate * n));
    if (bin % 2 == 0)
        bin += 1;
    return std::clamp (bin, 1, n / 2 - 1);
}

double worstAliasDbc (const float* x, int n, double sampleRate, int bin0, double bandHz)
{
    if (! isPowerOfTwo (n) || ! (sampleRate > 0.0) || bin0 < 1 || bin0 >= n / 2)
        return 0.0;
    const auto m = windowedMagnitudes (x, n);
    const int lo = static_cast<int> (std::ceil (20.0 / sampleRate * n));
    const int hi = std::min (n / 2, static_cast<int> (std::floor (bandHz / sampleRate * n)));
    double worst = 0.0;
    for (int b = std::max (1, lo); b <= hi; ++b)
    {
        const int r = b % bin0;
        if (std::min (r, bin0 - r) <= 4) // the main lobe (+-2 bins) of a harmonic, and margin
            continue;
        worst = std::max (worst, m[static_cast<size_t> (b)]);
    }
    return 20.0 * std::log10 (std::max (1.0e-30, worst)) - 20.0 * std::log10 (std::max (1.0e-30, m[static_cast<size_t> (bin0)]));
}

double powerShareAboveDb (const float* x, int n, double sampleRate, double fromHz, int windowSize)
{
    if (! isPowerOfTwo (windowSize) || n < windowSize || ! (sampleRate > 0.0))
        return 0.0;
    const int first = static_cast<int> (std::ceil (fromHz / sampleRate * windowSize));
    double total = 0.0, above = 0.0;
    for (int start = 0; start + windowSize <= n; start += windowSize / 2)
    {
        const auto m = windowedMagnitudes (x + start, windowSize);
        for (int k = 1; k < static_cast<int> (m.size()); ++k)
        {
            const double p = m[static_cast<size_t> (k)] * m[static_cast<size_t> (k)];
            total += p;
            above += k >= first ? p : 0.0;
        }
    }
    return ratioDb (above, total);
}

double dcDbfs (const float* x, int n)
{
    if (n <= 0)
        return kMinusInfDb;
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
        sum += x[i];
    return std::max (static_cast<double> (kMinusInfDb), 20.0 * std::log10 (std::max (1.0e-30, std::abs (sum) / n)));
}

double percentile (std::vector<double> v, double p)
{
    if (v.empty())
        return 0.0;
    std::sort (v.begin(), v.end());
    const auto idx = static_cast<size_t> (std::clamp (p, 0.0, 1.0) * static_cast<double> (v.size() - 1) + 0.5); // nearest rank
    return v[idx];
}

GainTrackStats summariseGainTrack (const std::vector<double>& g, double rateHz)
{
    GainTrackStats s;
    if (g.empty())
        return s;
    const double med = percentile (g, 0.5);
    s.spreadDb = percentile (g, 0.95) - percentile (g, 0.05);
    s.dipDb = med - *std::min_element (g.begin(), g.end());
    s.liftDb = *std::max_element (g.begin(), g.end()) - med;
    s.downPercent = 100.0 * static_cast<double> (std::count_if (g.begin(), g.end(), [med] (double v) { return v < med - 1.0; }))
                    / static_cast<double> (g.size());

    // Modulation spectrum over the whole periods of rateHz the track holds
    // (the track has one value per 5 ms, i.e. 200 per second).
    constexpr double kTrackRate = 200.0;
    if (rateHz > 0.0)
    {
        const double period = kTrackRate / rateHz;
        const auto m = static_cast<int> (std::floor (static_cast<double> (g.size()) / period) * period + 1.0e-9);
        if (m >= 2)
        {
            double mean = 0.0;
            for (int i = 0; i < m; ++i)
                mean += g[static_cast<size_t> (i)];
            mean /= m;
            for (size_t k = 0; k < s.modulationDb.size(); ++k)
            {
                double re = 0.0, im = 0.0;
                for (int i = 0; i < m; ++i)
                {
                    const double a = kTwoPi * static_cast<double> (k + 1) * rateHz * i / kTrackRate;
                    re += (g[static_cast<size_t> (i)] - mean) * std::cos (a);
                    im += (g[static_cast<size_t> (i)] - mean) * std::sin (a);
                }
                s.modulationDb[k] = 2.0 * std::sqrt (re * re + im * im) / m;
            }
        }
    }
    return s;
}

double energyCentroidMs (const std::vector<float>& x, int begin, int n, double sampleRate)
{
    double e = 0.0, te = 0.0;
    for (int i = 0; i < n && begin + i < static_cast<int> (x.size()); ++i)
    {
        const double v = static_cast<double> (x[static_cast<size_t> (begin + i)]);
        e += v * v;
        te += i * v * v;
    }
    return e > 0.0 && sampleRate > 0.0 ? 1000.0 * te / e / sampleRate : 0.0;
}
// ===========================================================================
// Scene events and band tracks (docs/11 E60)
// ===========================================================================
namespace
{
std::array<int, 4> countEvents (const std::vector<SceneEvent>& events)
{
    std::array<int, 4> c {};
    for (const auto& e : events)
        ++c[static_cast<size_t> (e.type)];
    return c;
}

json::Value countsToJson (const std::array<int, 4>& counts)
{
    json::Value v;
    for (int t = 0; t < 4; ++t)
        v.set (sceneEventName (static_cast<SceneEventType> (t)), counts[static_cast<size_t> (t)]);
    return v;
}

json::Value sceneEventToJson (const SceneEvent& e)
{
    json::Value v;
    v.set ("type", sceneEventName (e.type));
    v.set ("start", std::round (e.startSeconds * 1000.0) / 1000.0);
    v.set ("end", std::round (e.endSeconds * 1000.0) / 1000.0);
    v.set ("levelDb", jsonNumber (e.levelDb, 2));
    v.set ("overBackgroundDb", jsonNumber (e.overBackgroundDb, 2));
    return v;
}

std::string countsText (const std::array<int, 4>& counts)
{
    std::string s;
    for (int t = 0; t < 4; ++t)
        s += (t == 0 ? "" : ", ") + std::to_string (counts[static_cast<size_t> (t)]) + " " + sceneEventName (static_cast<SceneEventType> (t));
    return s;
}

std::string hzLabel (double hz)
{
    char buf[32];
    if (hz >= 1000.0)
        std::snprintf (buf, sizeof (buf), "%gk", hz / 1000.0);
    else
        std::snprintf (buf, sizeof (buf), "%g", hz);
    return buf;
}

/** Nearest-rank percentile of dB values (p in 0..1), -160 for none. */
float percentileDb (std::vector<float> v, double p)
{
    if (v.empty())
        return kMinusInfDb;
    const auto k = static_cast<size_t> (std::clamp (std::ceil (p * static_cast<double> (v.size())) - 1.0, 0.0, static_cast<double> (v.size() - 1)));
    std::nth_element (v.begin(), v.begin() + static_cast<std::ptrdiff_t> (k), v.end());
    return v[k];
}
} // namespace

EventsReport sceneEvents (const std::vector<std::vector<float>>& channels, double sampleRate, double bandHz)
{
    EventsReport r;
    SceneEventSettings settings;
    settings.bandHz = bandHz;
    const SceneAnalysis a = analyseSceneEvents (channels, sampleRate, settings);
    r.frameSeconds = a.frameSeconds;
    r.bandHz = bandHz;
    r.events = a.events;
    r.counts = countEvents (a.events);
    std::vector<float> backgrounds;
    backgrounds.reserve (a.frames.size());
    for (const auto& f : a.frames)
        backgrounds.push_back (f.backgroundDb);
    r.backgroundMedianDb = percentileDb (std::move (backgrounds), 0.5);
    return r;
}

std::vector<BandTrack> bandTracks (const std::vector<std::vector<float>>& channels, double sampleRate)
{
    std::vector<BandTrack> tracks;
    if (channels.empty() || channels[0].empty() || ! (sampleRate > 0.0))
        return tracks;
    // The mid, as octaveBands() reads it.
    const size_t n = channels[0].size();
    std::vector<std::vector<float>> mid (1, std::vector<float> (n, 0.0f));
    for (size_t i = 0; i < n; ++i)
    {
        double acc = 0.0;
        for (const auto& c : channels)
            acc += i < c.size() ? static_cast<double> (c[i]) : 0.0;
        mid[0][i] = static_cast<float> (acc / static_cast<double> (channels.size()));
    }

    constexpr std::array<float, 10> kNominalHz { 31.5f, 63.0f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f };
    for (size_t k = 0; k < kNominalHz.size(); ++k)
    {
        const double centre = 1000.0 * std::pow (2.0, static_cast<double> (k) - 5.0);
        if (centre >= 0.4 * sampleRate)
            break;
        SceneEventSettings settings;
        settings.bandHz = centre;
        settings.bandQ = 1.41421356237309505; // as octaveBands()
        const SceneAnalysis a = analyseSceneEvents (mid, sampleRate, settings);
        BandTrack t;
        t.centreHz = kNominalHz[k];
        const int per = std::max (1, static_cast<int> (std::lround (kTrackSeconds / a.frameSeconds)));
        for (size_t f = 0; f + static_cast<size_t> (per) <= a.frames.size(); f += static_cast<size_t> (per))
        {
            double power = 0.0;
            for (int j = 0; j < per; ++j)
                power += std::pow (10.0, 0.1 * a.frames[f + static_cast<size_t> (j)].levelDb);
            t.levelDb.push_back (static_cast<float> (std::max (static_cast<double> (kMinusInfDb), 10.0 * std::log10 (std::max (power / per, 1.0e-16)))));
        }
        t.p10Db = percentileDb (t.levelDb, 0.1);
        t.medianDb = percentileDb (t.levelDb, 0.5);
        t.p90Db = percentileDb (t.levelDb, 0.9);
        t.events = a.events;
        t.counts = countEvents (a.events);
        tracks.push_back (std::move (t));
    }
    return tracks;
}

json::Value eventsToJson (const EventsReport& r)
{
    json::Value v;
    v.set ("frameSeconds", r.frameSeconds);
    v.set ("bandHz", r.bandHz > 0.0 ? json::Value (r.bandHz) : json::Value());
    v.set ("backgroundMedianDb", jsonNumber (r.backgroundMedianDb, 2));
    v.set ("counts", countsToJson (r.counts));
    json::Value list { json::Value::Array {} };
    for (const auto& e : r.events)
        list.push (sceneEventToJson (e));
    v.set ("list", std::move (list));
    return v;
}

json::Value bandTracksToJson (const std::vector<BandTrack>& tracks)
{
    json::Value v;
    v.set ("frameSeconds", kTrackSeconds);
    json::Value bands { json::Value::Array {} };
    for (const auto& t : tracks)
    {
        json::Value b;
        b.set ("hz", static_cast<double> (t.centreHz));
        b.set ("p10Db", jsonNumber (t.p10Db, 2));
        b.set ("medianDb", jsonNumber (t.medianDb, 2));
        b.set ("p90Db", jsonNumber (t.p90Db, 2));
        b.set ("counts", countsToJson (t.counts));
        b.set ("levelsDb", jsonArray (t.levelDb, 1));
        json::Value list { json::Value::Array {} };
        for (const auto& e : t.events)
            list.push (sceneEventToJson (e));
        b.set ("events", std::move (list));
        bands.push (std::move (b));
    }
    v.set ("bands", std::move (bands));
    return v;
}

std::string formatEvents (const EventsReport& r, size_t maxLines)
{
    std::string s = "Events  : " + countsText (r.counts) + " (" + (r.bandHz > 0.0 ? hzLabel (r.bandHz) + " Hz band" : std::string ("full band"))
                    + ", background median " + formatDb (r.backgroundMedianDb, 1) + " dB)\n";
    char buf[160];
    for (size_t i = 0; i < r.events.size() && i < maxLines; ++i)
    {
        const auto& e = r.events[i];
        std::snprintf (buf, sizeof (buf), "  %9.3f - %9.3f s  %-12s %7s dB", e.startSeconds, e.endSeconds, sceneEventName (e.type),
                       formatDb (e.levelDb, 1).c_str());
        s += buf;
        if (e.type == SceneEventType::Onset || e.type == SceneEventType::Loud)
            std::snprintf (buf, sizeof (buf), "  %+.1f dB over the background\n", static_cast<double> (e.overBackgroundDb));
        else if (e.type == SceneEventType::LevelChange)
            std::snprintf (buf, sizeof (buf), "  %+.1f dB step\n", static_cast<double> (e.overBackgroundDb));
        else
            std::snprintf (buf, sizeof (buf), "\n");
        s += buf;
    }
    if (r.events.size() > maxLines)
        s += "  ... (" + std::to_string (r.events.size() - maxLines) + " more; --json lists all)\n";
    return s;
}

std::string formatBandTracks (const std::vector<BandTrack>& tracks)
{
    std::string s = "Band tracks (octave bands of the mid, 100 ms frames; dBFS):\n       Hz     p10  median     p90  onset  loud  silence  change\n";
    char buf[160];
    for (const auto& t : tracks)
    {
        std::snprintf (buf, sizeof (buf), "  %7s  %6s  %6s  %6s  %5d  %4d  %7d  %6d\n", hzLabel (t.centreHz).c_str(), formatDb (t.p10Db, 1).c_str(),
                       formatDb (t.medianDb, 1).c_str(), formatDb (t.p90Db, 1).c_str(), t.counts[0], t.counts[1], t.counts[2], t.counts[3]);
        s += buf;
    }
    return s;
}

// ===========================================================================
// Glitches (docs/11 E53)
// ===========================================================================
GlitchReport detectGlitches (const std::vector<std::vector<float>>& channels, double sampleRate)
{
    GlitchReport r;
    r.sampleRate = sampleRate;
    if (channels.empty())
        return r;
    DiscontinuityDetector d;
    d.prepare (sampleRate, static_cast<int> (channels.size()));
    size_t length = channels[0].size();
    for (const auto& c : channels)
        length = std::min (length, c.size());
    std::vector<const float*> ptrs (channels.size());
    for (size_t pos = 0; pos < length; pos += kAnalysisBlock)
    {
        for (size_t c = 0; c < channels.size(); ++c)
            ptrs[c] = channels[c].data() + pos;
        d.process (ptrs.data(), static_cast<int> (std::min<size_t> (kAnalysisBlock, length - pos)));
    }
    d.finish();
    for (int t = 0; t < kNumDiscontinuityTypes; ++t)
        r.counts[static_cast<size_t> (t)] = d.count (static_cast<DiscontinuityType> (t));
    r.events = d.events();
    return r;
}

json::Value glitchesToJson (const GlitchReport& r)
{
    json::Value v;
    int64_t total = 0;
    for (int t = 0; t < kNumDiscontinuityTypes; ++t)
    {
        v.set (discontinuityName (static_cast<DiscontinuityType> (t)), static_cast<double> (r.counts[static_cast<size_t> (t)]));
        total += r.counts[static_cast<size_t> (t)];
    }
    v.set ("total", static_cast<double> (total));
    json::Value list { json::Value::Array {} };
    for (const auto& e : r.events)
    {
        json::Value g;
        g.set ("type", discontinuityName (e.type));
        g.set ("channel", e.channel);
        g.set ("seconds", std::round (static_cast<double> (e.frame) / r.sampleRate * 10000.0) / 10000.0);
        if (e.length > 0)
            g.set ("lengthMs", std::round (1000.0 * static_cast<double> (e.length) / r.sampleRate * 100.0) / 100.0);
        g.set ("levelDb", jsonNumber (e.levelDb, 2));
        if (e.type == DiscontinuityType::Click)
            g.set ("overDb", jsonNumber (e.overDb, 2));
        list.push (std::move (g));
    }
    v.set ("list", std::move (list));
    return v;
}

std::string formatGlitches (const GlitchReport& r, size_t maxLines)
{
    std::string s = "Glitches: ";
    int64_t total = 0;
    for (int t = 0; t < kNumDiscontinuityTypes; ++t)
    {
        s += (t == 0 ? "" : ", ") + std::to_string (r.counts[static_cast<size_t> (t)]) + " " + discontinuityName (static_cast<DiscontinuityType> (t));
        total += r.counts[static_cast<size_t> (t)];
    }
    s += "\n";
    char buf[160];
    for (size_t i = 0; i < r.events.size() && i < maxLines; ++i)
    {
        const auto& e = r.events[i];
        std::snprintf (buf, sizeof (buf), "  %10.4f s  ch %d  %-10s %7.1f dB", static_cast<double> (e.frame) / r.sampleRate, e.channel,
                       discontinuityName (e.type), static_cast<double> (e.levelDb));
        s += buf;
        if (e.type == DiscontinuityType::Click)
        {
            std::snprintf (buf, sizeof (buf), " (+%.1f dB over the residual)", static_cast<double> (e.overDb));
            s += buf;
        }
        if (e.length > 0)
        {
            std::snprintf (buf, sizeof (buf), " %.2f ms", 1000.0 * static_cast<double> (e.length) / r.sampleRate);
            s += buf;
        }
        s += "\n";
    }
    if (total > static_cast<int64_t> (std::min (r.events.size(), maxLines)))
        s += "  ... (" + std::to_string (total - static_cast<int64_t> (std::min (r.events.size(), maxLines))) + " more)\n";
    return s;
}
} // namespace flub::cli
