#include "OfflineRenderer.h"

#include "flub/common/Denormals.h"
#include "flub/common/Math.h"
#include "flub/engine/Parameters.h"
#include "flub/engine/ProcessingChain.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

namespace flub::cli
{
using namespace param;

namespace
{
constexpr double kMinSampleRate = 8000.0;
constexpr double kMaxSampleRate = 768000.0;
constexpr float kMaxDriveDb = 24.0f;

bool isMeasured (float lufs) noexcept { return std::isfinite (lufs) && lufs > kMinusInfDb + 0.5f; }

std::string formatFloat (const char* format, double v)
{
    char buf[64];
    std::snprintf (buf, sizeof (buf), format, v);
    return buf;
}
} // namespace

bool checkRenderable (const io::AudioFileData& input, std::string& error)
{
    const int nch = input.numChannels;
    if (nch != 1 && nch != 2 && nch != 6 && nch != 8)
    {
        error = "unsupported channel count " + std::to_string (nch) + " (supported: 1 = mono, 2 = stereo, 6 = 5.1, 8 = 7.1)";
        return false;
    }
    if (static_cast<int> (input.channels.size()) != nch)
    {
        error = "internal error: channel data does not match the channel count";
        return false;
    }
    for (const auto& c : input.channels)
        if (c.size() != input.channels[0].size())
        {
            error = "internal error: channels have different lengths";
            return false;
        }
    if (! (input.sampleRate >= kMinSampleRate && input.sampleRate <= kMaxSampleRate))
    {
        error = "unsupported sample rate " + formatFloat ("%.0f", input.sampleRate) + " Hz (supported: 8 kHz .. 768 kHz)";
        return false;
    }
    return true;
}

bool renderPass (const io::AudioFileData& input, const std::vector<float>& values, int blockSize,
                 std::vector<std::vector<float>>& outStereo, int& latencySamples, std::string& error)
{
    if (! checkRenderable (input, error))
        return false;
    if (values.size() != static_cast<size_t> (kNumParams))
    {
        error = "internal error: parameter table has the wrong size";
        return false;
    }

    const int inChannels = input.numChannels;
    const int chainChannels = inChannels == 1 ? 2 : inChannels; // 2, 6 or 8
    const int64_t numFrames = input.numFrames();
    blockSize = std::clamp (blockSize, 16, 16384);

    // Private store + chain: nothing is shared between concurrent batch jobs.
    auto store = std::make_unique<ParameterStore>();
    for (int id = 0; id < kNumParams; ++id)
        store->set (Bank::A, id, values[static_cast<size_t> (id)]);
    store->setActiveBank (Bank::A);

    auto chain = std::make_unique<ProcessingChain> (*store);
    chain->prepare ({ input.sampleRate, blockSize, chainChannels });
    const int latency = chain->getLatencySamples();
    latencySamples = latency;

    AudioBuffer io (chainChannels, blockSize);
    outStereo.assign (2, std::vector<float> (static_cast<size_t> (numFrames), 0.0f));

    ScopedNoDenormals noDenormals;

    // Prime: one silent block delivers every parameter to the modules, then
    // reset() snaps their smoothers onto those targets (no start-up glide).
    io.clear();
    chain->process (io.block (chainChannels, blockSize));
    chain->reset();

    // Input followed by `latency` samples of silence; output sample k of the
    // chain belongs to input frame k - latency.
    const int64_t totalFrames = numFrames + latency;
    for (int64_t pos = 0; pos < totalFrames; pos += blockSize)
    {
        const int n = static_cast<int> (std::min<int64_t> (blockSize, totalFrames - pos));
        const int available = static_cast<int> (std::clamp<int64_t> (numFrames - pos, 0, n)); // real input frames in this block

        for (int c = 0; c < chainChannels; ++c)
        {
            float* dst = io.channel (c);
            const auto& src = input.channels[static_cast<size_t> (inChannels == 1 ? 0 : c)]; // mono -> both sides
            if (available > 0)
                std::memcpy (dst, src.data() + pos, sizeof (float) * static_cast<size_t> (available));
            if (available < n)
                std::memset (dst + available, 0, sizeof (float) * static_cast<size_t> (n - available));
        }

        chain->process (io.block (chainChannels, n));

        // Keep chain output frames [latency, latency + numFrames).
        const int64_t firstOut = pos - latency; // input frame of io[0]
        const int skip = static_cast<int> (std::clamp<int64_t> (-firstOut, 0, n));
        const int64_t dstStart = firstOut + skip;
        const int count = static_cast<int> (std::clamp<int64_t> (std::min<int64_t> (n - skip, numFrames - dstStart), 0, n));
        if (count > 0)
            for (int c = 0; c < 2; ++c)
                std::memcpy (outStereo[static_cast<size_t> (c)].data() + dstStart, io.channel (c) + skip,
                             sizeof (float) * static_cast<size_t> (count));
    }
    return true;
}

bool renderFile (const io::AudioFileData& input, const std::vector<float>& baseValues, const RenderSettings& settings,
                 RenderResult& result, std::string& error)
{
    result = RenderResult();
    result.chainInputChannels = input.numChannels == 1 ? 2 : input.numChannels;

    std::vector<float> values = baseValues;
    auto value = [&values] (int id) -> float& { return values[static_cast<size_t> (id)]; };

    using Clock = std::chrono::steady_clock;
    double renderSeconds = 0.0;

    auto runPass = [&] (std::vector<std::vector<float>>& out, LoudnessReport& report) {
        const auto t0 = Clock::now();
        int latency = 0;
        const bool ok = renderPass (input, values, settings.blockSize, out, latency, error);
        renderSeconds += std::chrono::duration<double> (Clock::now() - t0).count();
        if (! ok)
            return false;
        result.latencySamples = latency;
        ++result.passes;
        report = analyse (out, input.sampleRate);
        return true;
    };

    std::vector<std::vector<float>> current, best;
    LoudnessReport currentReport, bestReport;
    if (! runPass (current, currentReport))
        return false;

    float bestDrive = value (MaxDriveDb), bestGain = value (OutputGainDb);
    auto keepAsBest = [&] {
        best.swap (current);
        bestReport = currentReport;
        bestDrive = value (MaxDriveDb);
        bestGain = value (OutputGainDb);
    };

    if (! settings.targetLufs)
    {
        keepAsBest();
    }
    else
    {
        const float target = *settings.targetLufs;
        const float tolerance = settings.toleranceLu;

        if (! isMeasured (currentReport.integratedLufs))
        {
            result.targetReached = false;
            result.notes.push_back ("loudness target skipped: the output has no measurable integrated loudness "
                                    "(silence, or shorter than one 400 ms gating block)");
            keepAsBest();
        }
        else
        {
            float bestError = std::abs (currentReport.integratedLufs - target);
            keepAsBest();
            // `current` is empty after the swap; bestReport == currentReport.

            struct Point
            {
                float drive, gain, lufs;
            };
            std::optional<Point> previous;
            bool gainNoteAdded = false;

            for (int iteration = 0; iteration < settings.maxIterations; ++iteration)
            {
                const float lufs = currentReport.integratedLufs;
                const float err = target - lufs;
                if (! isMeasured (lufs) || std::abs (err) <= tolerance)
                    break;

                const float drive = value (MaxDriveDb);
                const float gain = value (OutputGainDb);

                // Secant step once two passes at the same output gain exist:
                // the limiter makes loudness grow by less than 1 LU per dB.
                float slope = 1.0f;
                if (previous && previous->gain == gain && std::abs (drive - previous->drive) > 0.05f)
                {
                    const float s = (lufs - previous->lufs) / (drive - previous->drive);
                    slope = std::isfinite (s) ? std::clamp (s, 0.2f, 1.5f) : 1.0f;
                }
                previous = Point { drive, gain, lufs };

                const float newDrive = std::clamp (drive + err / slope, 0.0f, kMaxDriveDb);
                if (std::abs (newDrive - drive) < 0.005f)
                {
                    if (err < 0.0f && drive <= 0.005f)
                    {
                        // Too loud even without drive (e.g. macros add drive, or
                        // the source is already loud): attenuate after the
                        // maximizer, which can only lower the true peak.
                        const auto& gainInfo = layout()[static_cast<size_t> (OutputGainDb)];
                        const float newGain = std::clamp (gain + err, gainInfo.minValue, gain);
                        if (std::abs (newGain - gain) < 0.005f)
                        {
                            result.notes.push_back ("loudness target not reachable: output gain is already at its minimum");
                            break;
                        }
                        value (OutputGainDb) = newGain;
                        if (! gainNoteAdded)
                            result.notes.push_back ("maximizer drive is already 0 dB: output.gain lowered to reach the target");
                        gainNoteAdded = true;
                    }
                    else
                    {
                        result.notes.push_back ("loudness target not reachable: maximizer drive is at its "
                                                + formatFloat ("%.0f", kMaxDriveDb) + " dB maximum");
                        break;
                    }
                }
                else
                {
                    value (MaxDriveDb) = newDrive;
                }

                if (! runPass (current, currentReport))
                    return false;

                const float e = std::abs (currentReport.integratedLufs - target);
                if (isMeasured (currentReport.integratedLufs) && e < bestError)
                {
                    bestError = e;
                    keepAsBest(); // currentReport still describes this pass
                }
            }
            result.targetReached = bestError <= tolerance;
            if (! result.targetReached)
                result.notes.push_back ("loudness target missed by " + formatFloat ("%.2f", bestError) + " LU after "
                                        + std::to_string (result.passes) + " passes");
        }
    }

    // Verify the delivered pass against the ceiling that was asked for.
    if (settings.verifyCeilingDb && bestReport.truePeakDbtp > *settings.verifyCeilingDb + 0.1f)
        result.notes.push_back ("warning: true peak " + formatFloat ("%.2f", bestReport.truePeakDbtp) + " dBTP exceeds the "
                                + formatFloat ("%.2f", *settings.verifyCeilingDb) + " dBTP ceiling");

    result.output.sampleRate = input.sampleRate;
    result.output.numChannels = 2;
    result.output.sourceFormat = io::SampleFormat::Float32;
    result.output.channels = std::move (best);
    result.outputReport = bestReport;
    result.driveDb = bestDrive;
    result.outputGainDb = bestGain;
    result.renderSeconds = renderSeconds;
    return true;
}
} // namespace flub::cli
