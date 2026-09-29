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

bool isMeasured (float lufs) noexcept { return std::isfinite (lufs) && lufs > kMinusInfDb + 0.5f; }

std::string formatFloat (const char* format, double v)
{
    char buf[64];
    std::snprintf (buf, sizeof (buf), format, v);
    return buf;
}

/** Builds RenderStats from one MeterBus reading per block (weighted by the
    block's programme frames). */
class StatsAccumulator
{
public:
    explicit StatsAccumulator (const MeterBus& bus) noexcept
        : m (bus), safetyClipsAtStart (bus.safetyClipCount.load (std::memory_order_relaxed))
    {
    }

    void add (int numFrames) noexcept
    {
        constexpr auto rl = std::memory_order_relaxed;
        const double w = numFrames;
        const bool first = total <= 0.0;
        total += w;

        const float gr = m.maxGainReductionDb.load (rl);
        s.limiterGrMaxDb = std::min (s.limiterGrMaxDb, gr);
        grSum += w * gr;
        over1 += gr < -1.0f ? w : 0.0;
        over3 += gr < -3.0f ? w : 0.0;

        const float glue = m.glueGainReductionDb.load (rl);
        s.glueGrMaxDb = std::min (s.glueGrMaxDb, glue);
        glueSum += w * glue;

        const float clip = m.clipEnergyRatioDb.load (rl);
        s.clipEnergyMaxDb = std::max (s.clipEnergyMaxDb, clip);
        clipActive += clip > -60.0f ? w : 0.0;

        const float thd = m.distortionDb.load (rl);
        s.distortionMaxDb = std::max (s.distortionMaxDb, thd);
        thdPower += w * (thd > kMinusInfDb ? std::pow (10.0, 0.1 * thd) : 0.0);

        const float harm = m.harmonicsDb.load (rl);
        s.harmonicsMaxDb = std::max (s.harmonicsMaxDb, harm);
        harmPower += w * (harm > kMinusInfDb ? std::pow (10.0, 0.1 * harm) : 0.0);

        const float comp = m.compGainReductionDb.load (rl);
        s.compGrMaxDb = std::min (s.compGrMaxDb, comp);
        compSum += w * comp;
        s.compUpwardMaxDb = std::max (s.compUpwardMaxDb, m.compUpwardGainDb.load (rl));
        s.bassProtectionMaxDb = std::max (s.bassProtectionMaxDb, m.bassProtectionDb.load (rl));

        for (size_t b = 0; b < s.modeBandMinDb.size(); ++b)
        {
            const float g = m.dynEqGainDb[static_cast<size_t> (ProcessingChain::kFirstModeBand) + b].load (rl);
            s.modeBandMinDb[b] = first ? g : std::min (s.modeBandMinDb[b], g);
            s.modeBandMaxDb[b] = first ? g : std::max (s.modeBandMaxDb[b], g);
            bandSum[b] += w * g;
        }

        const float scale = m.governorScale.load (rl);
        s.governorScaleMin = std::min (s.governorScaleMin, scale);
        scaleSum += w * scale;
        backoff += scale < 0.99f ? w : 0.0;
        const int state = m.governorState.load (rl);
        if (state >= 0 && state < static_cast<int> (stateFrames.size()))
            stateFrames[static_cast<size_t> (state)] += w;
        const uint32_t reason = m.governorReason.load (rl);
        limiterReason += (reason & SafetyGovernor::kReasonLimiter) != 0 ? w : 0.0;
        distortionReason += (reason & SafetyGovernor::kReasonDistortion) != 0 ? w : 0.0;
        s.governorScaleEnd = scale;
        s.governorStateEnd = state;
        s.governorReasonEnd = reason;
        // The measured loop (Normal / Strict, docs/11 E06 batch 2).
        dynamicsReason += (reason & SafetyGovernor::kReasonDynamics) != 0 ? w : 0.0;
        harmonicsReason += (reason & SafetyGovernor::kReasonHarmonics) != 0 ? w : 0.0;
        tonalReason += (reason & SafetyGovernor::kReasonTonal) != 0 ? w : 0.0;
        s.governorStrength = m.governorStrength.load (rl);
        const float harmonicsScale = m.governorHarmonicsScale.load (rl), tonalScale = m.governorTonalScale.load (rl);
        s.governorHarmonicsScaleMin = std::min (s.governorHarmonicsScaleMin, harmonicsScale);
        s.governorHarmonicsScaleEnd = harmonicsScale;
        s.governorTonalScaleMin = std::min (s.governorTonalScaleMin, tonalScale);
        s.governorTonalScaleEnd = tonalScale;
        const float drive = m.governorDriveResidualDb.load (rl), harmonics = m.governorHarmonicsResidualDb.load (rl);
        s.governorDriveResidualMaxDb = std::max (s.governorDriveResidualMaxDb, drive);
        s.governorDriveResidualEndDb = drive;
        if (drive > kMinusInfDb)
        {
            residualPower += w * std::pow (10.0, 0.1 * drive);
            residualFrames += w;
        }
        s.governorHarmonicsResidualMaxDb = std::max (s.governorHarmonicsResidualMaxDb, harmonics);
        s.governorHarmonicsResidualEndDb = harmonics;
        s.governorBassResidualEndDb = m.governorBassResidualDb.load (rl);
        s.governorResidualBudgetDb = m.governorResidualBudgetDb.load (rl);
        const float plr = m.governorPlrDb.load (rl);
        s.governorPlrMinDb = std::min (s.governorPlrMinDb, plr);
        s.governorPlrEndDb = plr;
        s.governorPlrBudgetDb = m.governorPlrBudgetDb.load (rl);
        for (size_t b = 0; b < s.tonalLiftEndDb.size(); ++b)
        {
            const float lift = m.tonalLiftDb[b].load (rl);
            s.tonalLiftMaxDb[b] = std::max (s.tonalLiftMaxDb[b], lift);
            s.tonalLiftEndDb[b] = lift;
            s.tonalBudgetDb[b] = m.tonalBudgetDb[b].load (rl);
        }
        const float smoothCut = m.smoothnessCutDb.load (rl);
        s.smoothnessCutMaxDb = std::min (s.smoothnessCutMaxDb, smoothCut);
        smoothActive += smoothCut < -0.5f ? w : 0.0;

        const float level = m.autoLevelGainDb.load (rl);
        s.autoLevelMinDb = first ? level : std::min (s.autoLevelMinDb, level);
        s.autoLevelMaxDb = first ? level : std::max (s.autoLevelMaxDb, level);
        s.autoDriveMaxDb = std::min (s.autoDriveMaxDb, m.autoDriveDb.load (rl));
    }

    RenderStats finish() const noexcept
    {
        RenderStats r = s;
        r.frames = static_cast<int64_t> (total);
        r.safetyClips = m.safetyClipCount.load (std::memory_order_relaxed) - safetyClipsAtStart;
        if (total <= 0.0)
            return r;
        const auto mean = [this] (double sum) { return static_cast<float> (sum / total); };
        const auto percent = [this] (double frames) { return static_cast<float> (100.0 * frames / total); };
        r.limiterGrMeanDb = mean (grSum);
        r.limiterOver1DbPercent = percent (over1);
        r.limiterOver3DbPercent = percent (over3);
        r.glueGrMeanDb = mean (glueSum);
        r.clipActivePercent = percent (clipActive);
        r.distortionMeanDb = thdPower > 0.0 ? std::max (kMinusInfDb, static_cast<float> (10.0 * std::log10 (thdPower / total))) : kMinusInfDb;
        r.harmonicsMeanDb = harmPower > 0.0 ? std::max (kMinusInfDb, static_cast<float> (10.0 * std::log10 (harmPower / total))) : kMinusInfDb;
        r.compGrMeanDb = mean (compSum);
        for (size_t b = 0; b < r.modeBandMeanDb.size(); ++b)
            r.modeBandMeanDb[b] = mean (bandSum[b]);
        r.governorScaleMean = mean (scaleSum);
        r.governorBackoffPercent = percent (backoff);
        for (size_t k = 0; k < stateFrames.size(); ++k)
            r.governorStatePercent[k] = percent (stateFrames[k]);
        r.governorLimiterReasonPercent = percent (limiterReason);
        r.governorDistortionReasonPercent = percent (distortionReason);
        r.governorDynamicsReasonPercent = percent (dynamicsReason);
        r.governorHarmonicsReasonPercent = percent (harmonicsReason);
        r.governorTonalReasonPercent = percent (tonalReason);
        r.smoothnessActivePercent = percent (smoothActive);
        r.governorDriveResidualMeanDb = residualPower > 0.0 ? std::max (kMinusInfDb, static_cast<float> (10.0 * std::log10 (residualPower / residualFrames))) : kMinusInfDb;
        return r;
    }

private:
    const MeterBus& m;
    const uint64_t safetyClipsAtStart;
    RenderStats s;
    double total = 0.0, grSum = 0.0, over1 = 0.0, over3 = 0.0, glueSum = 0.0, clipActive = 0.0, thdPower = 0.0, compSum = 0.0;
    double scaleSum = 0.0, backoff = 0.0, harmPower = 0.0, limiterReason = 0.0, distortionReason = 0.0;
    double dynamicsReason = 0.0, harmonicsReason = 0.0, tonalReason = 0.0, residualPower = 0.0, residualFrames = 0.0;
    double smoothActive = 0.0;
    std::array<double, 4> stateFrames {};
    std::array<double, 4> bandSum {};
};
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
                 std::vector<std::vector<float>>& outStereo, int& latencySamples, std::string& error,
                 const std::atomic<bool>* abort, RenderStats* stats, ProtectionStrength protection)
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
    chain->setProtectionStrength (protection);
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
    StatsAccumulator accumulator (chain->meters());

    // Input followed by `latency` samples of silence; output sample k of the
    // chain belongs to input frame k - latency.
    const int64_t totalFrames = numFrames + latency;
    for (int64_t pos = 0; pos < totalFrames; pos += blockSize)
    {
        if (abort != nullptr && abort->load (std::memory_order_relaxed))
        {
            error = kAbortedError;
            return false;
        }

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
        if (stats != nullptr && available > 0)
            accumulator.add (available);

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
    if (stats != nullptr)
        *stats = accumulator.finish();
    return true;
}

bool renderFile (const io::AudioFileData& input, const std::vector<float>& baseValues, const RenderSettings& settings,
                 RenderResult& result, std::string& error)
{
    result = RenderResult();
    result.chainInputChannels = input.numChannels == 1 ? 2 : input.numChannels;
    if (baseValues.size() != static_cast<size_t> (kNumParams))
    {
        error = "internal error: parameter table has the wrong size";
        return false;
    }

    std::vector<float> values = baseValues;
    auto value = [&values] (int id) -> float& { return values[static_cast<size_t> (id)]; };
    auto rangeOf = [] (int id) -> const Info& { return layout()[static_cast<size_t> (id)]; };
    const bool maximizerOn = value (MaximizerOn) >= 0.5f;

    using Clock = std::chrono::steady_clock;
    double renderSeconds = 0.0;

    RenderStats currentStats, bestStats;
    auto runPass = [&] (std::vector<std::vector<float>>& out, LoudnessReport& report) {
        const auto t0 = Clock::now();
        int latency = 0;
        const bool ok = renderPass (input, values, settings.blockSize, out, latency, error, settings.abort, &currentStats, settings.protection);
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

    float bestDrive = value (MaxDriveDb), bestInGain = value (InputGainDb), bestOutGain = value (OutputGainDb);
    auto keepAsBest = [&] {
        best.swap (current);
        bestReport = currentReport;
        bestStats = currentStats;
        bestDrive = value (MaxDriveDb);
        bestInGain = value (InputGainDb);
        bestOutGain = value (OutputGainDb);
    };

    bool iterated = false; // the loudness loop ran (its result is reported below)
    if (! settings.targetLufs)
    {
        keepAsBest();
    }
    else if (value (BypassAll) >= 0.5f)
    {
        // The bypassed output is the (loudness-matched) dry signal: no gain
        // stage of the chain can move it, so iterating would only waste passes.
        result.targetReached = false;
        result.notes.push_back ("warning: loudness target skipped: bypass=on delivers the dry signal");
        keepAsBest();
    }
    else if (! isMeasured (currentReport.integratedLufs))
    {
        result.targetReached = false;
        result.notes.push_back ("warning: loudness target skipped: the output has no measurable integrated loudness "
                                "(silence, or shorter than one 400 ms gating block)");
        keepAsBest();
    }
    else
    {
        const float target = *settings.targetLufs;
        const float tolerance = settings.toleranceLu;
        float bestError = std::abs (currentReport.integratedLufs - target);
        keepAsBest(); // `current` is empty after the swap; bestReport == currentReport

        // Gain stages the loop may move, in order of preference:
        //   louder : max.drive (up to 24 dB) -> input.gain (ahead of the
        //            maximizer, so its ceiling still holds)
        //   quieter: max.drive (down to 0 dB) -> output.gain (post-maximizer
        //            attenuation, which can only lower the true peak) ->
        //            input.gain (ahead of the maximizer)
        // With the maximizer explicitly off output.gain moves first, then
        // input.gain (and --ceiling was already reported as not guaranteed).
        struct Point
        {
            int knob;
            float drive, inGain, outGain, lufs;
        };
        std::optional<Point> previous;
        bool inputNoteAdded = false, outputNoteAdded = false;

        for (int iteration = 0; iteration < settings.maxIterations; ++iteration)
        {
            const float lufs = currentReport.integratedLufs;
            const float err = target - lufs;
            if (! isMeasured (lufs) || std::abs (err) <= tolerance)
                break;

            auto canMove = [&] (int id, bool up) {
                const float v = value (id);
                return up ? v < rangeOf (id).maxValue - 0.005f : v > rangeOf (id).minValue + 0.005f;
            };
            const bool louder = err > 0.0f;
            int knob = -1;
            if (maximizerOn)
            {
                if (canMove (MaxDriveDb, louder))
                    knob = MaxDriveDb;
                else if (louder && canMove (InputGainDb, true))
                    knob = InputGainDb;
                else if (! louder && canMove (OutputGainDb, false))
                    knob = OutputGainDb;
                else if (! louder && canMove (InputGainDb, false))
                    knob = InputGainDb;
            }
            else if (canMove (OutputGainDb, louder))
            {
                knob = OutputGainDb;
            }
            else if (canMove (InputGainDb, louder))
            {
                knob = InputGainDb;
            }

            if (knob < 0)
            {
                std::string why;
                if (louder)
                    why = maximizerOn ? "maximizer drive and input.gain are at their maximum"
                                      : "output.gain and input.gain are at their maximum (the maximizer is off)";
                else
                    why = maximizerOn ? "maximizer drive is 0 dB and output.gain and input.gain are at their minimum"
                                      : "output.gain and input.gain are at their minimum (the maximizer is off)";
                result.notes.push_back ("warning: loudness target not reachable: " + why);
                break;
            }

            // Secant step once two passes moved the same stage (with the other
            // stages unchanged): a limiter makes loudness grow by less than
            // 1 LU per dB as it works harder.
            const float drive = value (MaxDriveDb), inGain = value (InputGainDb), outGain = value (OutputGainDb);
            float slope = 1.0f;
            if (previous && previous->knob == knob)
            {
                const float x0 = knob == MaxDriveDb ? previous->drive : (knob == InputGainDb ? previous->inGain : previous->outGain);
                const float x1 = value (knob);
                const bool othersEqual = (knob == MaxDriveDb || previous->drive == drive) && (knob == InputGainDb || previous->inGain == inGain)
                                         && (knob == OutputGainDb || previous->outGain == outGain);
                if (othersEqual && std::abs (x1 - x0) > 0.05f)
                {
                    const float s = (lufs - previous->lufs) / (x1 - x0);
                    slope = std::isfinite (s) ? std::clamp (s, 0.2f, 1.5f) : 1.0f;
                }
            }
            previous = Point { knob, drive, inGain, outGain, lufs };

            const auto& info = rangeOf (knob);
            float& v = value (knob);
            v = std::clamp (v + err / slope, info.minValue, info.maxValue);

            if (knob == InputGainDb && ! inputNoteAdded)
            {
                std::string note;
                if (louder)
                    note = maximizerOn ? "maximizer drive is at its " + formatFloat ("%.0f", rangeOf (MaxDriveDb).maxValue)
                                             + " dB maximum: input.gain raised to reach the target"
                                       : std::string ("input.gain raised to reach the target (the maximizer is off)");
                else
                    note = maximizerOn ? "maximizer drive is 0 dB and output.gain is at its minimum: input.gain lowered to reach the target"
                                       : std::string ("output.gain is at its minimum: input.gain lowered to reach the target (the maximizer is off)");
                result.notes.push_back (note);
                inputNoteAdded = true;
            }
            if (knob == OutputGainDb && maximizerOn && ! outputNoteAdded)
            {
                result.notes.push_back ("maximizer drive is already 0 dB: output.gain lowered to reach the target");
                outputNoteAdded = true;
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
        iterated = true;
    }

    // Ceiling guarantee for the delivered file. The maximizer's true-peak
    // limiter can overshoot by a few hundredths of a dB when driven very hard;
    // with the maximizer on, a static trim (offline, after the chain) brings
    // the file back under the ceiling that was asked for.
    if (settings.verifyCeilingDb && isMeasured (bestReport.truePeakDbtp))
    {
        const float ceiling = *settings.verifyCeilingDb;
        if (maximizerOn && bestReport.truePeakDbtp > ceiling)
        {
            const float trimDb = ceiling - bestReport.truePeakDbtp - 0.01f;
            const float g = dbToGain (trimDb);
            for (auto& channel : best)
                for (auto& s : channel)
                    s *= g;
            bestReport = analyse (best, input.sampleRate);
            result.ceilingTrimDb = trimDb;
            result.notes.push_back ("true peak held at the ceiling: output trimmed by " + formatFloat ("%.2f", trimDb)
                                    + " dB (maximizer overshoot)");
            if (iterated && isMeasured (bestReport.integratedLufs))
                result.targetReached = std::abs (bestReport.integratedLufs - *settings.targetLufs) <= settings.toleranceLu;
        }
        if (bestReport.truePeakDbtp > ceiling + 0.1f)
            result.notes.push_back ("warning: true peak " + formatFloat ("%.2f", bestReport.truePeakDbtp) + " dBTP exceeds the "
                                    + formatFloat ("%.2f", ceiling) + " dBTP ceiling");
    }

    if (iterated && ! result.targetReached)
        result.notes.push_back ("warning: loudness target missed by "
                                + formatFloat ("%.2f", std::abs (bestReport.integratedLufs - *settings.targetLufs)) + " LU after "
                                + std::to_string (result.passes) + (result.passes == 1 ? " pass" : " passes"));

    result.output.sampleRate = input.sampleRate;
    result.output.numChannels = 2;
    result.output.sourceFormat = io::SampleFormat::Float32;
    result.output.channels = std::move (best);
    result.outputReport = bestReport;
    result.driveDb = bestDrive;
    result.inputGainDb = bestInGain;
    result.outputGainDb = bestOutGain;
    result.stats = bestStats;
    result.renderSeconds = renderSeconds;
    return true;
}

bool writeRender (const std::string& path, io::SampleFormat format, RenderResult& result, std::string& error)
{
    if (! io::writeWav (path, result.output, format, error))
        return false;
    if (format == io::SampleFormat::Float32)
        return true; // float32 round trips bit-exactly: outputReport already describes the file

    // PCM: report what was delivered - the quantised, dithered samples - not
    // the float render (the dither is seeded per file, so this is repeatable).
    io::AudioFileData written;
    std::string readError;
    if (! io::readWav (path, written, readError))
    {
        error = "cannot read back " + path + ": " + readError;
        return false;
    }
    result.outputReport = analyse (written.channels, written.sampleRate);
    return true;
}
} // namespace flub::cli
