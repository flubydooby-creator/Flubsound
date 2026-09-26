#include "Analysis.h"

#include "flub/analysis/LoudnessMeter.h"
#include "flub/analysis/PeakMeters.h"
#include "flub/common/Denormals.h"
#include "flub/common/Math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <sstream>

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
} // namespace flub::cli
