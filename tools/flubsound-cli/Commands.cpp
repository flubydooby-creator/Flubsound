#include "Commands.h"

#include "FactoryPresets.h"
#include "Soak.h"

#include "flub/common/Math.h"
#include "flub/engine/MacroMap.h"
#include "flub/engine/Parameters.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/io/FilePath.h"
#include "flub/io/Json.h"
#include "flub/io/WavFile.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <mutex>
#include <new>
#include <set>
#include <thread>

namespace fs = std::filesystem;

namespace flub::cli
{
namespace
{
// ===========================================================================
// Output helpers
// ===========================================================================
/** Human-readable progress goes to stdout, or to stderr when --json owns stdout. */
class Log
{
public:
    explicit Log (const CliOptions& o) : quiet (o.quiet), json (o.json) {}

    void info (const std::string& text) const
    {
        if (quiet)
            return;
        const std::lock_guard<std::mutex> lock (mutex);
        std::fputs (text.c_str(), json ? stderr : stdout);
        std::fflush (json ? stderr : stdout);
    }

    /** Problems worth seeing even with --quiet (e.g. a missed loudness target). */
    void warning (const std::string& text) const
    {
        if (! quiet)
        {
            info ("Warning : " + text + "\n");
            return;
        }
        const std::lock_guard<std::mutex> lock (mutex);
        std::fputs (("warning: " + text + "\n").c_str(), stderr);
    }

    void error (const std::string& text) const
    {
        const std::lock_guard<std::mutex> lock (mutex);
        std::fputs (("error: " + text + "\n").c_str(), stderr);
    }

private:
    bool quiet, json;
    mutable std::mutex mutex;
};

std::string fmt (const char* format, double v)
{
    char buf[64];
    std::snprintf (buf, sizeof (buf), format, v);
    return buf;
}

bool isUtf8Continuation (char c) { return (static_cast<unsigned char> (c) & 0xC0) == 0x80; }

/** Column width of a UTF-8 string: code points, not bytes (file names and
    preset descriptions may be non-ASCII). */
size_t displayWidth (const std::string& s)
{
    return static_cast<size_t> (std::count_if (s.begin(), s.end(), [] (char c) { return ! isUtf8Continuation (c); }));
}

std::string padRight (std::string s, size_t width)
{
    const size_t w = displayWidth (s);
    if (w < width)
        s.append (width - w, ' ');
    return s;
}

std::string padLeft (std::string s, size_t width)
{
    const size_t w = displayWidth (s);
    if (w < width)
        s.insert (0, width - w, ' ');
    return s;
}

/** Keeps the end of long names ("...ong-file-name.wav"): the last width - 3
    code points, never cutting inside a UTF-8 sequence. */
std::string ellipsize (const std::string& s, size_t width)
{
    if (displayWidth (s) <= width || width < 4)
        return s;
    size_t start = s.size();
    for (size_t kept = 0; kept < width - 3 && start > 0;)
        if (! isUtf8Continuation (s[--start]))
            ++kept;
    return "..." + s.substr (start);
}

void printJson (const json::Value& v)
{
    std::fputs ((json::write (v, 2) + "\n").c_str(), stdout);
    std::fflush (stdout);
}

std::string lowerExtension (const fs::path& p)
{
    std::string e = io::pathToUtf8 (p.extension());
    std::transform (e.begin(), e.end(), e.begin(), [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    return e;
}

/** One-line description of what will be rendered. */
std::string describeSettings (const ResolvedParameters& p, const RenderOptions& o)
{
    using namespace param;
    const auto& v = p.values;
    auto val = [&v] (int id) { return v[static_cast<size_t> (id)]; };
    const auto mode = static_cast<ModeValue> (std::lround (val (Mode)));

    std::string s = "Preset  : " + p.presetDescription + "\n";
    s += "Settings: mode " + formatParameterValue (Mode, val (Mode)) + ", boost " + formatParameterValue (BoostIntensity, val (BoostIntensity));
    for (int i = 0; i < 5; ++i)
        if (val (Macro1 + i) > 0.0f)
            s += std::string (", ") + MacroMap::macroName (mode, i) + " " + formatParameterValue (Macro1 + i, val (Macro1 + i));
    s += ", profile " + formatParameterValue (LatencyProfile, val (LatencyProfile));
    s += ", maximizer " + formatParameterValue (MaximizerOn, val (MaximizerOn));
    s += " (drive " + formatParameterValue (MaxDriveDb, val (MaxDriveDb)) + ", ceiling " + fmt ("%.1f dBTP", val (MaxCeilingDb)) + ")";
    if (p.smart)
        s += ", Smart macros on";
    if (o.neuralVoiceCleanup)
        s += ", neural voice cleanup (experimental)";
    if (o.targetLufs)
        s += ", target " + fmt ("%.1f LUFS", *o.targetLufs);
    s += ", output " + std::string (sampleFormatName (o.format)) + "\n";
    return s;
}

/** Notes starting with "warning: " are problems and survive --quiet. */
void logNotes (const Log& log, const std::vector<std::string>& notes)
{
    const std::string prefix = "warning: ";
    for (const auto& n : notes)
    {
        if (n.rfind (prefix, 0) == 0)
            log.warning (n.substr (prefix.size()));
        else
            log.info ("Note    : " + n + "\n");
    }
}

/** Settings summary plus the parameter notes. */
void logSettings (const Log& log, const ResolvedParameters& p, const RenderOptions& o)
{
    log.info (describeSettings (p, o));
    logNotes (log, p.notes);
}

std::string reportLine (const LoudnessReport& r)
{
    return formatDb (r.integratedLufs, 2) + " LUFS integrated, LRA " + formatDb (r.loudnessRangeLu, 1) + " LU, true peak "
           + formatDb (r.truePeakDbtp, 2) + " dBTP, sample peak " + formatDb (r.samplePeakDbfs, 2) + " dBFS";
}

/** A stat rounded to 0.01 for JSON; null at the -160 dB floor ("no measurement"). */
json::Value statValue (float v)
{
    if (! std::isfinite (v) || v <= kMinusInfDb + 0.5f)
        return json::Value();
    return json::Value (std::round (static_cast<double> (v) * 100.0) / 100.0);
}

json::Value renderInfoJson (const RenderResult& rr, const RenderOptions& o, const ResolvedParameters& p, double sampleRate,
                            double audioSeconds)
{
    json::Value r;
    r.set ("preset", p.presetDescription);
    r.set ("smartMacros", p.smart);
    r.set ("neuralVoiceCleanup", o.neuralVoiceCleanup);
    r.set ("passes", rr.passes);
    r.set ("latencySamples", rr.latencySamples);
    r.set ("latencyMs", std::round (1.0e5 * rr.latencySamples / sampleRate) / 100.0);
    r.set ("chainInputChannels", rr.chainInputChannels);
    r.set ("maxDriveDb", std::round (rr.driveDb * 100.0) / 100.0);
    r.set ("inputGainDb", std::round (rr.inputGainDb * 100.0) / 100.0);
    r.set ("outputGainDb", std::round (rr.outputGainDb * 100.0) / 100.0);
    r.set ("ceilingTrimDb", std::round (rr.ceilingTrimDb * 100.0) / 100.0);
    r.set ("targetLufs", o.targetLufs ? json::Value (static_cast<double> (*o.targetLufs)) : json::Value());
    r.set ("targetReached", rr.targetReached);
    r.set ("renderSeconds", std::round (rr.renderSeconds * 1000.0) / 1000.0);
    r.set ("realtimeFactor", rr.renderSeconds > 0.0 && audioSeconds > 0.0 ? std::round (10.0 * audioSeconds * rr.passes / rr.renderSeconds) / 10.0 : 0.0);
    r.set ("format", sampleFormatName (o.format));
    r.set ("stats", renderStatsToJson (rr.stats));
    json::Value notes { json::Value::Array {} };
    for (const auto& n : p.notes)
        notes.push (n);
    for (const auto& n : rr.notes)
        notes.push (n);
    r.set ("notes", std::move (notes));
    return r;
}

bool sameFile (const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    if (! fs::exists (a, ec) || ! fs::exists (b, ec))
        return false;
    return fs::equivalent (a, b, ec) && ! ec;
}

bool isInside (const fs::path& child, const fs::path& parent)
{
    const auto c = child.lexically_normal(), p = parent.lexically_normal();
    auto ci = c.begin();
    for (auto pi = p.begin(); pi != p.end(); ++pi, ++ci)
    {
        if (pi->empty())
            continue; // trailing separator
        if (ci == c.end() || *ci != *pi)
            return false;
    }
    return true;
}

const char* jsonUnitName (param::Unit u) noexcept
{
    using param::Unit;
    switch (u)
    {
        case Unit::None: return "none";
        case Unit::Db: return "dB";
        case Unit::Hz: return "Hz";
        case Unit::Ms: return "ms";
        case Unit::Percent: return "percent";
        case Unit::Ratio: return "ratio";
        case Unit::Lufs: return "LUFS";
        case Unit::Degrees: return "degrees";
        case Unit::Millimetres: return "mm";
        case Unit::DbPerSec: return "dB/s";
        case Unit::Choice: return "choice";
        case Unit::Toggle: return "toggle";
    }
    return "none";
}
} // namespace

// ===========================================================================
// Shared by process and batch
// ===========================================================================
bool isWavFile (const fs::path& p)
{
    const auto e = lowerExtension (p);
    return e == ".wav" || e == ".wave";
}

RenderSettings makeRenderSettings (const RenderOptions& o, const ResolvedParameters& p)
{
    RenderSettings rs;
    rs.blockSize = o.blockSize;
    rs.targetLufs = o.targetLufs;
    rs.protection = o.protection;
    rs.smartMacros = p.smart;
    rs.neuralVoiceCleanup = o.neuralVoiceCleanup;
    if (o.ceilingDb || o.targetLufs)
        rs.verifyCeilingDb = p.values[static_cast<size_t> (param::MaxCeilingDb)];
    return rs;
}

const char* governorStateName (int state) noexcept
{
    constexpr const char* kStateNames[] = { "idle", "backingOff", "holding", "recovering" };
    return state >= 0 && state < 4 ? kStateNames[state] : "idle";
}

/** The measured loop's reason bits and their names (docs/11 E06 batch 2). */
std::array<std::pair<uint32_t, const char*>, 3> governorMeasuredReasons() noexcept
{
    return { { { SafetyGovernor::kReasonDynamics, "dynamics" }, { SafetyGovernor::kReasonHarmonics, "harmonics" }, { SafetyGovernor::kReasonTonal, "tonal" } } };
}

const char* protectionStrengthName (int strength) noexcept
{
    constexpr const char* kNames[] = { "off", "normal", "strict" };
    return strength >= 0 && strength < 3 ? kNames[strength] : "off";
}

/** PLR readings: 1000 dB (PlrMeter::kNoReading) = none (null). */
json::Value plrValue (float v)
{
    return v < PlrMeter::kNoReading ? statValue (v) : json::Value();
}

/** render.stats governor.measured: the measured loop's readouts at
    protection strength Normal / Strict (MeterBus::governor*, docs/11 E06
    batch 2); scales 1 and readings null at Off. */
json::Value governorMeasuredToJson (const RenderStats& st)
{
    const auto scale = [] (float v) { return json::Value (std::round (v * 1000.0) / 1000.0); };
    json::Value v;
    v.set ("strength", protectionStrengthName (st.governorStrength));
    json::Value hs;
    hs.set ("min", scale (st.governorHarmonicsScaleMin));
    hs.set ("end", scale (st.governorHarmonicsScaleEnd));
    v.set ("harmonicsScale", std::move (hs));
    json::Value ts;
    ts.set ("min", scale (st.governorTonalScaleMin));
    ts.set ("end", scale (st.governorTonalScaleEnd));
    v.set ("tonalScale", std::move (ts));
    json::Value reasons;
    reasons.set ("dynamics", statValue (st.governorDynamicsReasonPercent));
    reasons.set ("harmonics", statValue (st.governorHarmonicsReasonPercent));
    reasons.set ("tonal", statValue (st.governorTonalReasonPercent));
    v.set ("reasonPercent", std::move (reasons));
    json::Value drive;
    drive.set ("maxDb", statValue (st.governorDriveResidualMaxDb));
    drive.set ("meanDb", statValue (st.governorDriveResidualMeanDb));
    drive.set ("endDb", statValue (st.governorDriveResidualEndDb));
    drive.set ("budgetDb", statValue (st.governorResidualBudgetDb));
    v.set ("driveResidual", std::move (drive));
    json::Value harmonics;
    harmonics.set ("maxDb", statValue (st.governorHarmonicsResidualMaxDb));
    harmonics.set ("endDb", statValue (st.governorHarmonicsResidualEndDb));
    harmonics.set ("budgetDb", statValue (st.governorResidualBudgetDb));
    v.set ("harmonicsResidual", std::move (harmonics));
    v.set ("bassResidualEndDb", statValue (st.governorBassResidualEndDb));
    json::Value plr;
    plr.set ("minDb", plrValue (st.governorPlrMinDb));
    plr.set ("endDb", plrValue (st.governorPlrEndDb));
    plr.set ("budgetDb", st.governorPlrBudgetDb > 0.0f ? statValue (st.governorPlrBudgetDb) : json::Value());
    v.set ("plr", std::move (plr));
    json::Value tonal;
    constexpr const char* kBands[] = { "presence", "harsh", "air" };
    for (size_t b = 0; b < st.tonalLiftEndDb.size(); ++b)
    {
        json::Value band;
        band.set ("maxDb", statValue (st.tonalLiftMaxDb[b]));
        band.set ("endDb", statValue (st.tonalLiftEndDb[b]));
        band.set ("budgetDb", statValue (st.tonalBudgetDb[b]));
        tonal.set (kBands[b], std::move (band));
    }
    v.set ("tonalLift", std::move (tonal));
    return v;
}

/** The surround folds' gains (docs/11 E28a): the virtualiser's make-up and the fold headroom. */
json::Value foldStatsToJson (const RenderStats& st)
{
    json::Value fold;
    fold.set ("virtMakeupMinDb", statValue (st.virtMakeupMinDb));
    fold.set ("virtMakeupMaxDb", statValue (st.virtMakeupMaxDb));
    fold.set ("virtMakeupEndDb", statValue (st.virtMakeupEndDb));
    fold.set ("headroomMaxDb", statValue (st.foldHeadroomMaxDb));
    fold.set ("headroomActivePercent", statValue (st.foldHeadroomActivePercent));
    return fold;
}

json::Value renderStatsToJson (const RenderStats& st)
{
    json::Value limiter;
    limiter.set ("grMaxDb", statValue (st.limiterGrMaxDb));
    limiter.set ("grMeanDb", statValue (st.limiterGrMeanDb));
    limiter.set ("over1DbPercent", statValue (st.limiterOver1DbPercent));
    limiter.set ("over3DbPercent", statValue (st.limiterOver3DbPercent));
    limiter.set ("safetyClips", static_cast<double> (st.safetyClips));

    json::Value glue;
    glue.set ("grMaxDb", statValue (st.glueGrMaxDb));
    glue.set ("grMeanDb", statValue (st.glueGrMeanDb));

    json::Value clipper;
    clipper.set ("energyMaxDb", statValue (st.clipEnergyMaxDb));
    clipper.set ("activePercent", statValue (st.clipActivePercent));

    json::Value distortion;
    distortion.set ("thdnMaxDb", statValue (st.distortionMaxDb));
    distortion.set ("thdnMeanDb", statValue (st.distortionMeanDb));

    json::Value harmonics;
    harmonics.set ("maxDb", statValue (st.harmonicsMaxDb));
    harmonics.set ("meanDb", statValue (st.harmonicsMeanDb));

    json::Value compressor;
    compressor.set ("grMaxDb", statValue (st.compGrMaxDb));
    compressor.set ("grMeanDb", statValue (st.compGrMeanDb));
    compressor.set ("upwardMaxDb", statValue (st.compUpwardMaxDb));

    json::Value bands { json::Value::Array {} };
    for (size_t b = 0; b < st.modeBandMinDb.size(); ++b)
    {
        json::Value band;
        band.set ("band", static_cast<int> (b) + ProcessingChain::kFirstModeBand);
        band.set ("minDb", statValue (st.modeBandMinDb[b]));
        band.set ("maxDb", statValue (st.modeBandMaxDb[b]));
        band.set ("meanDb", statValue (st.modeBandMeanDb[b]));
        bands.push (std::move (band));
    }

    json::Value governor;
    governor.set ("scaleMin", std::round (st.governorScaleMin * 1000.0) / 1000.0);
    governor.set ("scaleMean", std::round (st.governorScaleMean * 1000.0) / 1000.0);
    governor.set ("backoffPercent", statValue (st.governorBackoffPercent));
    json::Value states;
    for (size_t k = 0; k < st.governorStatePercent.size(); ++k)
        states.set (governorStateName (static_cast<int> (k)), statValue (st.governorStatePercent[k]));
    governor.set ("statePercent", std::move (states));
    governor.set ("limiterReasonPercent", statValue (st.governorLimiterReasonPercent));
    governor.set ("distortionReasonPercent", statValue (st.governorDistortionReasonPercent));
    // At the end of the programme (docs/11 E06 (6)): scale, state and reasons.
    json::Value end;
    end.set ("scale", std::round (st.governorScaleEnd * 1000.0) / 1000.0);
    end.set ("state", governorStateName (st.governorStateEnd));
    json::Value reasons { json::Value::Array {} };
    if ((st.governorReasonEnd & SafetyGovernor::kReasonLimiter) != 0)
        reasons.push (json::Value ("limiter"));
    if ((st.governorReasonEnd & SafetyGovernor::kReasonDistortion) != 0)
        reasons.push (json::Value ("distortion"));
    for (const auto& [bit, name] : governorMeasuredReasons())
        if ((st.governorReasonEnd & bit) != 0)
            reasons.push (json::Value (name));
    end.set ("reasons", std::move (reasons));
    governor.set ("end", std::move (end));
    governor.set ("measured", governorMeasuredToJson (st)); // docs/11 E06 batch 2

    json::Value smoothness; // docs/11 E07
    smoothness.set ("cutMaxDb", statValue (st.smoothnessCutMaxDb));
    smoothness.set ("activePercent", statValue (st.smoothnessActivePercent));

    json::Value leveller;
    leveller.set ("autoLevelMinDb", statValue (st.autoLevelMinDb));
    leveller.set ("autoLevelMaxDb", statValue (st.autoLevelMaxDb));
    leveller.set ("autoDriveMaxDb", statValue (st.autoDriveMaxDb));

    json::Value v;
    v.set ("frames", static_cast<double> (st.frames));
    v.set ("limiter", std::move (limiter));
    v.set ("glue", std::move (glue));
    v.set ("clipper", std::move (clipper));
    v.set ("distortion", std::move (distortion));
    v.set ("harmonics", std::move (harmonics));
    v.set ("compressor", std::move (compressor));
    v.set ("bassProtectionMaxDb", statValue (st.bassProtectionMaxDb));
    v.set ("modeBands", std::move (bands));
    v.set ("governor", std::move (governor));
    v.set ("smoothness", std::move (smoothness));
    v.set ("leveller", std::move (leveller));
    v.set ("fold", foldStatsToJson (st)); // docs/11 E28a
    return v;
}

std::string formatStats (const RenderStats& st)
{
    std::string s = "limiter GR max " + fmt ("%.1f dB", st.limiterGrMaxDb) + " (mean " + fmt ("%.1f dB", st.limiterGrMeanDb)
                    + fmt (", %.0f %% of the time deeper than 1 dB)", st.limiterOver1DbPercent);
    s += ", THD+N max " + formatDb (st.distortionMaxDb, 1) + " dB";
    s += ", governor min " + fmt ("%.0f %%", 100.0 * st.governorScaleMin);
    if (st.governorBackoffPercent > 0.0f || st.governorStateEnd != 0)
    {
        s += fmt (" (backing off %.0f %%", st.governorStatePercent[1]) + fmt (", holding %.0f %%", st.governorStatePercent[2])
             + fmt ("; limiter %.0f %%", st.governorLimiterReasonPercent) + fmt (", THD+N %.0f %% of the time;", st.governorDistortionReasonPercent)
             + " at the end " + governorStateName (st.governorStateEnd) + ")";
    }
    if (st.governorStrength != 0)
    {
        // The measured loop (docs/11 E06 batch 2).
        s += std::string (", protection ") + protectionStrengthName (st.governorStrength) + fmt (": harmonics scale min %.0f %%", 100.0 * st.governorHarmonicsScaleMin)
             + fmt (", audible residual max %.1f dB", st.governorDriveResidualMaxDb) + fmt (" (budget %.0f)", st.governorResidualBudgetDb);
        if (st.governorPlrMinDb < PlrMeter::kNoReading)
            s += fmt (", PLR min %.1f dB", st.governorPlrMinDb);
        if (st.governorDynamicsReasonPercent > 0.0f || st.governorHarmonicsReasonPercent > 0.0f || st.governorTonalReasonPercent > 0.0f)
            s += fmt ("; dynamics %.0f %%", st.governorDynamicsReasonPercent) + fmt (", harmonics %.0f %%", st.governorHarmonicsReasonPercent)
                 + fmt (", tonal %.0f %% of the time", st.governorTonalReasonPercent);
    }
    if (st.harmonicsMaxDb > kMinusInfDb)
        s += ", harmonics max " + formatDb (st.harmonicsMaxDb, 1) + " dB";
    if (st.compGrMaxDb < 0.0f || st.compUpwardMaxDb > 0.0f)
        s += ", compressor GR max " + fmt ("%.1f dB", st.compGrMaxDb) + " / upward " + fmt ("+%.1f dB", st.compUpwardMaxDb);
    if (st.autoLevelMinDb != 0.0f || st.autoLevelMaxDb != 0.0f)
        s += ", auto level " + fmt ("%+.1f", st.autoLevelMinDb) + ".." + fmt ("%+.1f dB", st.autoLevelMaxDb);
    return s;
}

// ===========================================================================
// process
// ===========================================================================
int runProcess (const CliOptions& o)
{
    const Log log (o);
    std::string error;

    ResolvedParameters params;
    if (! buildParameters (o.render, params, error))
    {
        log.error (error);
        return kExitUsage;
    }
    if (sameFile (io::pathFromUtf8 (o.input), io::pathFromUtf8 (o.output)))
    {
        log.error ("the output file would overwrite the input file (" + o.output + ")");
        return kExitUsage;
    }

    io::AudioFileData input;
    if (! io::readWav (o.input, input, error))
    {
        log.error (error);
        return kExitFailure;
    }
    if (! checkRenderable (input, error))
    {
        log.error (o.input + ": " + error);
        return kExitFailure;
    }

    const std::string inFormat = sampleFormatName (input.sourceFormat);
    logSettings (log, params, o.render);
    log.info ("Input   : " + o.input + fmt ("  (%.0f ch, ", input.numChannels) + fmt ("%.0f Hz, ", input.sampleRate) + inFormat
              + fmt (", %.2f s)\n", static_cast<double> (input.numFrames()) / input.sampleRate));

    const LoudnessReport inReport = analyse (input.channels, input.sampleRate);
    log.info ("          " + reportLine (inReport) + "\n");

    RenderResult rr;
    if (! renderFile (input, params.values, makeRenderSettings (o.render, params), rr, error))
    {
        log.error (o.input + ": " + error);
        return kExitFailure;
    }
    if (! writeRender (o.output, o.render.format, rr, error))
    {
        log.error (error);
        return kExitFailure;
    }

    const double audioSeconds = static_cast<double> (input.numFrames()) / input.sampleRate;
    log.info ("Output  : " + o.output + "  (2 ch, " + sampleFormatName (o.render.format) + ")\n");
    log.info ("          " + reportLine (rr.outputReport) + "\n");
    log.info ("Render  : " + std::to_string (rr.passes) + (rr.passes == 1 ? " pass" : " passes") + ", latency "
              + std::to_string (rr.latencySamples) + " samples (" + fmt ("%.2f ms", 1000.0 * rr.latencySamples / input.sampleRate)
              + ") compensated, max.drive " + fmt ("%.2f dB", rr.driveDb)
              + (rr.inputGainDb != params.values[static_cast<size_t> (param::InputGainDb)] ? ", input.gain " + fmt ("%.2f dB", rr.inputGainDb) : std::string())
              + (rr.outputGainDb != params.values[static_cast<size_t> (param::OutputGainDb)] ? ", output.gain " + fmt ("%.2f dB", rr.outputGainDb) : std::string())
              + (rr.renderSeconds > 0.0 && audioSeconds > 0.0 ? fmt (", %.1fx realtime", audioSeconds * rr.passes / rr.renderSeconds) : std::string()) + "\n");
    log.info ("Stats   : " + formatStats (rr.stats) + "\n");
    logNotes (log, rr.notes);

    std::vector<BandLevel> inBands, outBands;
    if (o.bands)
    {
        inBands = octaveBands (input.channels, input.sampleRate);
        outBands = octaveBands (rr.output.channels, rr.output.sampleRate); // the render (before PCM quantisation)
        log.info ("Bands   : in  " + formatBands (inBands) + "\n");
        log.info ("          out " + formatBands (outBands) + "\n");
    }

    if (o.json)
    {
        json::Value v;
        v.set ("input", reportToJson (inReport, o.input, inFormat));
        v.set ("output", reportToJson (rr.outputReport, o.output, sampleFormatName (o.render.format)));
        if (o.bands)
        {
            v.set ("inputBands", bandsToJson (inBands));
            v.set ("outputBands", bandsToJson (outBands));
        }
        v.set ("render", renderInfoJson (rr, o.render, params, input.sampleRate, audioSeconds));
        printJson (v);
    }
    return kExitOk;
}

// ===========================================================================
// batch
// ===========================================================================
void runBatchJob (const BatchJob& job, const std::vector<float>& values, const RenderSettings& settings, io::SampleFormat format,
                  BatchResult& result)
{
    const auto t0 = std::chrono::steady_clock::now();
    try
    {
        io::AudioFileData input;
        std::string error;
        if (! io::readWav (io::pathToUtf8 (job.input), input, error) || ! checkRenderable (input, error))
        {
            result.error = error;
        }
        else
        {
            result.inFormat = sampleFormatName (input.sourceFormat);
            result.inReport = analyse (input.channels, input.sampleRate);
            if (! renderFile (input, values, settings, result.render, error))
            {
                result.error = error;
            }
            else
            {
                std::error_code ec;
                fs::create_directories (job.output.parent_path(), ec);
                if (! writeRender (io::pathToUtf8 (job.output), format, result.render, error))
                    result.error = error;
                else
                    result.ok = true;
                result.outReport = result.render.outputReport;
            }
        }
    }
    catch (const std::bad_alloc&)
    {
        result.error = "out of memory (try fewer --jobs)";
    }
    catch (const std::exception& e)
    {
        result.error = e.what();
    }
    result.render.output = io::AudioFileData(); // free the audio early
    result.seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
}

int collectBatchJobs (const fs::path& inDir, const fs::path& outDir, bool recursive, std::vector<BatchJob>& jobs,
                      std::vector<std::string>& skipped, std::string& error)
{
    jobs.clear();
    skipped.clear();
    std::error_code ec;

    const fs::path outCanonical = fs::weakly_canonical (outDir, ec);
    // Only an output folder nested in the input folder holds files the
    // recursive scan would pick up (an output folder above the input folder
    // contains every input, so it must not be used as an exclusion).
    const bool outputNestedInInput = recursive && isInside (outCanonical, fs::weakly_canonical (inDir, ec));
    auto consider = [&] (const fs::directory_entry& entry) {
        std::error_code fileEc;
        if (! entry.is_regular_file (fileEc))
            return;
        const fs::path rel = entry.path().lexically_relative (inDir);
        if (! isWavFile (entry.path()))
        {
            skipped.push_back (io::pathToUtf8Generic (rel));
            return;
        }
        if (outputNestedInInput && isInside (fs::weakly_canonical (entry.path(), fileEc), outCanonical))
            return; // output folder nested in the input folder: never re-process our own results
        jobs.push_back ({ entry.path(), outDir / rel, io::pathToUtf8Generic (rel) });
    };

    const auto dirOptions = fs::directory_options::skip_permission_denied;
    if (recursive)
    {
        for (fs::recursive_directory_iterator it (inDir, dirOptions, ec), end; ! ec && it != end; it.increment (ec))
            consider (*it);
    }
    else
    {
        for (fs::directory_iterator it (inDir, dirOptions, ec), end; ! ec && it != end; it.increment (ec))
            consider (*it);
    }
    if (ec)
    {
        error = "cannot list " + io::pathToUtf8 (inDir) + ": " + ec.message();
        return kExitFailure;
    }
    std::sort (jobs.begin(), jobs.end(), [] (const BatchJob& a, const BatchJob& b) { return a.displayName < b.displayName; });

    // An output path must never be one of the inputs. With --recursive and an
    // output folder above the input folder this can happen (in = A/B, out = A:
    // A/B/B/x.wav would be written to A/B/x.wav, itself an input), and a
    // worker could then read a file another worker is overwriting.
    std::set<fs::path> inputs;
    for (const auto& j : jobs)
    {
        std::error_code canonicalEc;
        inputs.insert (fs::weakly_canonical (j.input, canonicalEc));
    }
    for (const auto& j : jobs)
    {
        std::error_code canonicalEc;
        if (inputs.count (fs::weakly_canonical (j.output, canonicalEc)) != 0)
        {
            error = "the output file " + io::pathToUtf8 (j.output) + " would overwrite an input file; choose an output folder "
                    "outside the input folder tree";
            return kExitUsage;
        }
    }
    return kExitOk;
}

size_t batchWorkerCount (size_t numJobs, int requestedJobs)
{
    const unsigned hw = std::max (1u, std::thread::hardware_concurrency());
    return std::min (numJobs, static_cast<size_t> (requestedJobs > 0 ? static_cast<unsigned> (requestedJobs) : hw));
}

void runBatchJobs (const std::vector<BatchJob>& jobs, const std::vector<float>& values, const RenderSettings& settings,
                   io::SampleFormat format, size_t numWorkers, std::vector<BatchResult>& results,
                   const std::function<void (size_t)>& onFinished, const std::function<void (const std::string&)>& onNote)
{
    // Thread pool: workers pull the next job index.
    results.assign (jobs.size(), BatchResult());
    std::atomic<size_t> nextJob { 0 };

    auto worker = [&] {
        for (;;)
        {
            const size_t index = nextJob.fetch_add (1);
            if (index >= jobs.size())
                return;
            runBatchJob (jobs[index], values, settings, format, results[index]);
            if (onFinished)
                onFinished (index);
        }
    };

    std::vector<std::thread> pool;
    pool.reserve (numWorkers);
    try
    {
        for (size_t i = 0; i < numWorkers; ++i)
            pool.emplace_back (worker);
    }
    catch (const std::exception& e)
    {
        // Could not start more threads: the ones running (or this thread) finish the queue.
        if (onNote)
            onNote (std::string ("could only start ") + std::to_string (pool.size()) + " worker(s): " + e.what());
        if (pool.empty())
            worker();
    }
    for (auto& t : pool)
        t.join();
}

std::string formatBatchSummary (const std::vector<BatchJob>& jobs, const std::vector<BatchResult>& results,
                                const std::vector<std::string>& skipped, double wallSeconds)
{
    size_t okCount = 0;
    double audioSeconds = 0.0;
    for (const auto& r : results)
        if (r.ok)
        {
            ++okCount;
            audioSeconds += r.inReport.durationSeconds;
        }
    const size_t failCount = results.size() - okCount;

    size_t nameWidth = 4;
    for (const auto& j : jobs)
        nameWidth = std::max (nameWidth, std::min<size_t> (displayWidth (j.displayName), 48));

    std::string table = "\n" + padRight ("File", nameWidth)
                        + "  In LUFS  Out LUFS  Out dBTP  Out LRA  Passes   Time  Status\n";
    table += std::string (nameWidth + 64, '-') + "\n";
    for (size_t i = 0; i < jobs.size(); ++i)
    {
        const auto& r = results[i];
        table += padRight (ellipsize (jobs[i].displayName, nameWidth), nameWidth);
        if (r.ok)
        {
            table += "  " + padLeft (formatDb (r.inReport.integratedLufs, 1), 7) + "  " + padLeft (formatDb (r.outReport.integratedLufs, 1), 8)
                     + "  " + padLeft (formatDb (r.outReport.truePeakDbtp, 1), 8) + "  " + padLeft (formatDb (r.outReport.loudnessRangeLu, 1), 7)
                     + "  " + padLeft (std::to_string (r.render.passes), 6) + "  " + padLeft (fmt ("%.1fs", r.seconds), 5) + "  "
                     + (r.render.targetReached ? "ok" : "ok (target missed)") + "\n";
        }
        else
        {
            table += "  " + padLeft ("-", 7) + "  " + padLeft ("-", 8) + "  " + padLeft ("-", 8) + "  " + padLeft ("-", 7) + "  "
                     + padLeft ("-", 6) + "  " + padLeft (fmt ("%.1fs", r.seconds), 5) + "  FAILED: " + r.error + "\n";
        }
    }
    table += std::string (nameWidth + 64, '-') + "\n";
    table += std::to_string (okCount) + " ok, " + std::to_string (failCount) + " failed, " + std::to_string (skipped.size())
             + " skipped (non-WAV)" + fmt (" - %.1f s wall", wallSeconds)
             + (wallSeconds > 0.0 && audioSeconds > 0.0 ? fmt (", %.1fx realtime overall", audioSeconds / wallSeconds) : std::string())
             + "\n";
    if (! skipped.empty())
    {
        table += "Skipped : ";
        for (size_t i = 0; i < skipped.size() && i < 8; ++i)
            table += (i > 0 ? ", " : "") + skipped[i];
        if (skipped.size() > 8)
            table += ", ... (" + std::to_string (skipped.size() - 8) + " more)";
        table += "\n";
    }
    return table;
}

json::Value batchResultsToJson (const std::vector<BatchJob>& jobs, const std::vector<BatchResult>& results,
                                const std::vector<std::string>& skipped, size_t numWorkers, double wallSeconds,
                                const RenderOptions& options, const ResolvedParameters& params)
{
    json::Value files { json::Value::Array {} };
    size_t okCount = 0;
    for (size_t i = 0; i < jobs.size(); ++i)
    {
        const auto& r = results[i];
        okCount += r.ok ? 1 : 0;
        json::Value f;
        f.set ("file", io::pathToUtf8 (jobs[i].input));
        f.set ("outputFile", io::pathToUtf8 (jobs[i].output));
        f.set ("status", r.ok ? "ok" : "failed");
        if (! r.ok)
            f.set ("error", r.error);
        if (r.ok)
        {
            f.set ("input", reportToJson (r.inReport, io::pathToUtf8 (jobs[i].input), r.inFormat));
            f.set ("output", reportToJson (r.outReport, io::pathToUtf8 (jobs[i].output), sampleFormatName (options.format)));
            f.set ("render", renderInfoJson (r.render, options, params, r.inReport.sampleRate, r.inReport.durationSeconds));
        }
        f.set ("seconds", std::round (r.seconds * 1000.0) / 1000.0);
        files.push (std::move (f));
    }
    json::Value skippedJson { json::Value::Array {} };
    for (const auto& s : skipped)
        skippedJson.push (s);

    json::Value summary;
    summary.set ("ok", static_cast<int> (okCount));
    summary.set ("failed", static_cast<int> (results.size() - okCount));
    summary.set ("skipped", static_cast<int> (skipped.size()));
    summary.set ("jobs", static_cast<int> (numWorkers));
    summary.set ("wallSeconds", std::round (wallSeconds * 1000.0) / 1000.0);

    json::Value v;
    v.set ("files", std::move (files));
    v.set ("skipped", std::move (skippedJson));
    v.set ("summary", std::move (summary));
    return v;
}

int runBatch (const CliOptions& o)
{
    const Log log (o);
    std::string error;
    std::error_code ec;

    const fs::path inDir = io::pathFromUtf8 (o.input), outDir = io::pathFromUtf8 (o.output);
    if (! fs::is_directory (inDir, ec))
    {
        log.error ("input folder not found: " + o.input);
        return kExitUsage;
    }
    if (fs::exists (outDir, ec) && ! fs::is_directory (outDir, ec))
    {
        log.error ("output path exists and is not a folder: " + o.output);
        return kExitUsage;
    }
    if (sameFile (inDir, outDir))
    {
        log.error ("the output folder must differ from the input folder (files would be overwritten)");
        return kExitUsage;
    }

    ResolvedParameters params;
    if (! buildParameters (o.render, params, error))
    {
        log.error (error);
        return kExitUsage;
    }

    // ---- Collect jobs --------------------------------------------------------
    std::vector<BatchJob> jobs;
    std::vector<std::string> skipped;
    if (const int code = collectBatchJobs (inDir, outDir, o.recursive, jobs, skipped, error); code != kExitOk)
    {
        log.error (error);
        return code;
    }
    if (jobs.empty())
    {
        log.error ("no .wav files found in " + o.input + (o.recursive ? "" : " (use --recursive for sub-folders)"));
        return kExitFailure;
    }

    fs::create_directories (outDir, ec);
    if (! fs::is_directory (outDir, ec))
    {
        log.error ("cannot create output folder " + o.output);
        return kExitFailure;
    }

    const size_t numWorkers = batchWorkerCount (jobs.size(), o.jobs);

    logSettings (log, params, o.render);
    log.info ("Batch   : " + std::to_string (jobs.size()) + " WAV file(s), " + std::to_string (numWorkers) + " job(s)"
              + (skipped.empty() ? std::string() : ", skipping " + std::to_string (skipped.size()) + " non-WAV file(s)") + "\n");

    // ---- Run the jobs, printing one line per finished file ---------------------
    std::vector<BatchResult> results;
    std::atomic<size_t> finished { 0 };
    const auto wallStart = std::chrono::steady_clock::now();
    const size_t countWidth = std::to_string (jobs.size()).size();

    auto onFinished = [&] (size_t index) {
        const BatchResult& r = results[index];
        const size_t done = finished.fetch_add (1) + 1;
        std::string line = "[" + padLeft (std::to_string (done), countWidth) + "/" + std::to_string (jobs.size()) + "] ";
        if (r.ok)
            line += "ok      " + jobs[index].displayName + "  " + formatDb (r.inReport.integratedLufs, 1) + " -> "
                    + formatDb (r.outReport.integratedLufs, 1) + " LUFS, TP " + formatDb (r.outReport.truePeakDbtp, 1) + " dBTP"
                    + fmt (", %.1f s", r.seconds) + (r.render.targetReached ? "" : "  (target missed)") + "\n";
        else
            line += "FAILED  " + jobs[index].displayName + ": " + r.error + "\n";
        log.info (line);
    };
    runBatchJobs (jobs, params.values, makeRenderSettings (o.render, params), o.render.format, numWorkers, results, onFinished,
                  [&log] (const std::string& note) { log.info ("Note    : " + note + "\n"); });
    const double wallSeconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - wallStart).count();

    // ---- Summary ------------------------------------------------------------------
    if (! o.quiet)
        log.info (formatBatchSummary (jobs, results, skipped, wallSeconds));
    if (o.json)
        printJson (batchResultsToJson (jobs, results, skipped, numWorkers, wallSeconds, o.render, params));

    const bool anyFailed = std::any_of (results.begin(), results.end(), [] (const BatchResult& r) { return ! r.ok; });
    return anyFailed ? kExitFailure : kExitOk;
}

// ===========================================================================
// analyze
// ===========================================================================
int runAnalyze (const CliOptions& o)
{
    const Log log (o);
    io::AudioFileData input;
    std::string error;
    if (! io::readWav (o.input, input, error))
    {
        log.error (error);
        return kExitFailure;
    }
    const LoudnessReport r = analyse (input.channels, input.sampleRate);
    const ContentReport content = contentReport (input.channels, input.sampleRate); // docs/11 E34
    const std::string format = sampleFormatName (input.sourceFormat);
    const auto bands = o.bands ? octaveBands (input.channels, input.sampleRate) : std::vector<BandLevel> {};
    const auto events = o.events ? sceneEvents (input.channels, input.sampleRate, o.eventBandHz) : EventsReport {};
    const auto tracks = o.events && o.bands ? bandTracks (input.channels, input.sampleRate) : std::vector<BandTrack> {};
    const auto glitches = o.glitches ? detectGlitches (input.channels, input.sampleRate) : GlitchReport {};
    const auto spatial = o.spatial ? spatialMetrics (input.channels, input.sampleRate) : SpatialReport {};
    const auto focus = o.focusIld ? focusIld (input.channels, input.sampleRate) : FocusIldReport {};
    if ((o.spatial && ! spatial.stereo) || (o.focusIld && ! focus.stereo))
        log.warning ("--spatial / --focus-ild need a stereo (binaural) file");
    if (o.json)
    {
        auto v = reportToJson (r, o.input, format);
        v.set ("content", contentToJson (content));
        v.set ("suggest", suggestToJson (content));
        if (o.bands)
            v.set ("bands", bandsToJson (bands));
        if (o.events)
            v.set ("events", eventsToJson (events));
        if (o.events && o.bands)
            v.set ("bandTracks", bandTracksToJson (tracks));
        if (o.glitches)
            v.set ("glitches", glitchesToJson (glitches));
        if (o.spatial)
            v.set ("spatial", spatialToJson (spatial));
        if (o.focusIld)
            v.set ("focusIld", focusIldToJson (focus));
        printJson (v);
    }
    else
    {
        std::fputs (formatReport (r, o.input, format).c_str(), stdout);
        std::fputs (formatContent (content).c_str(), stdout);
        if (o.bands)
            std::fputs (("Bands   : " + formatBands (bands) + "\n").c_str(), stdout);
        if (o.events)
            std::fputs (formatEvents (events).c_str(), stdout);
        if (o.events && o.bands)
            std::fputs (formatBandTracks (tracks).c_str(), stdout);
        if (o.glitches)
            std::fputs (formatGlitches (glitches).c_str(), stdout);
        if (o.spatial)
            std::fputs (formatSpatial (spatial).c_str(), stdout);
        if (o.focusIld)
            std::fputs (formatFocusIld (focus).c_str(), stdout);
    }
    return kExitOk;
}

// ===========================================================================
// soak (docs/11 E53)
// ===========================================================================
int runSoak (const CliOptions& o)
{
    const Log log (o);
    ResolvedParameters params;
    std::string error;
    if (! buildParameters (o.render, params, error))
    {
        log.error (error);
        return kExitUsage;
    }
    for (const auto& note : params.notes)
        log.warning (note);

    SoakSettings s;
    s.seconds = o.soakSeconds;
    s.sampleRate = o.rate;
    s.blockSize = o.render.blockSize;
    s.seed = o.seed;
    s.automation = o.automation == "off" ? SoakAutomation::Off : o.automation == "all" ? SoakAutomation::All : SoakAutomation::User;
    s.intervalMs = o.intervalMs;
    s.protection = o.render.protection;

    // The factory presets the automation loads (none found: no preset switches).
    if (const auto dir = findFactoryPresetDir (o.render.presetDir))
    {
        std::vector<std::string> problems;
        for (const auto& entry : scanPresetDir (*dir, problems))
        {
            preset::Preset p;
            std::string presetError;
            if (preset::load (io::pathToUtf8 (entry.file), p, presetError) && p.values.size() == static_cast<size_t> (param::kNumParams))
            {
                s.presets.push_back (p.values);
                s.presetNames.push_back (entry.name);
            }
        }
    }
    if (s.presets.empty() && s.automation == SoakAutomation::User)
        log.warning ("no factory presets found: the automation does not switch presets");

    log.info (describeSettings (params, o.render));
    log.info ("Soak    : " + fmt ("%.1f", s.seconds) + " s of generated programme, automation " + soakAutomationName (s.automation) + " every "
              + fmt ("%.0f", s.intervalMs) + " ms (seed " + std::to_string (s.seed) + ", " + std::to_string (s.presets.size())
              + " presets)\n");
    if (! o.quiet)
        s.progress = [&log, &s] (double seconds) {
            log.info ("  " + fmt ("%.0f", seconds) + " / " + fmt ("%.0f", s.seconds) + " s\n");
        };

    SoakReport report;
    if (! soakChain (params.values, s, report, error))
    {
        log.error (error);
        return kExitFailure;
    }
    if (o.json)
        printJson (soakToJson (report));
    else
        std::fputs (formatSoak (report).c_str(), stdout);
    if (report.inputTotal() > 0)
        log.warning ("the generated programme itself read as discontinuous: the run is not valid");
    return report.outputTotal() == 0 && report.inputTotal() == 0 ? kExitOk : kExitFailure;
}

// ===========================================================================
// quality (docs/11 E59)
// ===========================================================================
namespace
{
constexpr double kQualityFs = 48000.0;
using Stereo = std::vector<std::vector<float>>;

int qualitySamples (double seconds) { return static_cast<int> (std::lround (seconds * kQualityFs)); }

io::AudioFileData qualityInput (const std::vector<float>& mono)
{
    io::AudioFileData d;
    d.sampleRate = kQualityFs;
    d.numChannels = 2;
    d.channels = { mono, mono };
    return d;
}

std::vector<float> sumOfSines (double seconds, const std::vector<std::pair<double, double>>& tones /* (Hz, peak) */)
{
    const int n = qualitySamples (seconds);
    std::vector<float> x (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        double v = 0.0;
        for (const auto& [f, a] : tones)
            v += a * std::sin (kTwoPi * f * i / kQualityFs);
        x[static_cast<size_t> (i)] = static_cast<float> (v);
    }
    return x;
}

/** Paul Kellet's refined pink filter on FastRandom white noise, scaled to rmsLevel. */
std::vector<float> pinkNoise (int n, double rmsLevel, uint32_t seed)
{
    FastRandom rng (seed);
    std::vector<float> v (static_cast<size_t> (n));
    double b0 = 0.0, b1 = 0.0, b2 = 0.0, b3 = 0.0, b4 = 0.0, b5 = 0.0, b6 = 0.0, acc = 0.0;
    for (auto& s : v)
    {
        const double w = rng.nextBipolar();
        b0 = 0.99886 * b0 + w * 0.0555179;
        b1 = 0.99332 * b1 + w * 0.0750759;
        b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856;
        b4 = 0.55000 * b4 + w * 0.5329522;
        b5 = -0.7616 * b5 - w * 0.0168980;
        const double p = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
        b6 = w * 0.115926;
        s = static_cast<float> (p);
        acc += p * p;
    }
    const double g = n > 0 && acc > 0.0 ? rmsLevel / std::sqrt (acc / n) : 0.0;
    for (auto& s : v)
        s = static_cast<float> (s * g);
    return v;
}

/** The multitone's frequencies: 31 log-spaced integer frequencies 40 Hz .. 16 kHz. */
std::vector<double> multitoneFrequencies()
{
    std::vector<double> f;
    for (int k = 0; k <= 30; ++k)
    {
        const double hz = std::round (40.0 * std::pow (400.0, k / 30.0));
        if (f.empty() || hz > f.back())
            f.push_back (hz);
    }
    return f;
}

/** Pink amplitudes (1 / sqrt f), seeded phases, scaled to rmsLevel. */
std::vector<float> multitone (double seconds, const std::vector<double>& freqs, double rmsLevel, double sampleRate = kQualityFs)
{
    FastRandom rng (20590);
    std::vector<std::pair<double, double>> parts; // (amplitude, phase)
    double power = 0.0;
    for (double f : freqs)
    {
        const double a = 1.0 / std::sqrt (f);
        parts.push_back ({ a, kPi * rng.nextBipolar() });
        power += 0.5 * a * a;
    }
    const double g = rmsLevel / std::sqrt (power);
    const int n = static_cast<int> (std::lround (seconds * sampleRate));
    std::vector<float> x (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        double v = 0.0;
        for (size_t k = 0; k < freqs.size(); ++k)
            v += parts[k].first * std::sin (kTwoPi * freqs[k] * i / sampleRate + parts[k].second);
        x[static_cast<size_t> (i)] = static_cast<float> (g * v);
    }
    return x;
}

/** 55 Hz kicks (exp decay, tau 100 ms, 350 ms long, peak kickPeak) every 500 ms from 250 ms. */
double kickAt (double t, double kickPeak)
{
    const double beat = std::fmod (t + 0.25, 0.5);
    return t >= 0.25 && beat < 0.35 ? kickPeak * std::exp (-beat / 0.1) * std::sin (kTwoPi * 55.0 * beat) : 0.0;
}

std::vector<float> midOf (const Stereo& c)
{
    std::vector<float> m (c[0].size());
    for (size_t i = 0; i < m.size(); ++i)
        m[i] = 0.5f * (c[0][i] + c[1][i]);
    return m;
}

double windowPower (const std::vector<float>& x, const std::vector<std::pair<int, int>>& windows)
{
    double acc = 0.0;
    int64_t n = 0;
    for (const auto& [b, e] : windows)
        for (int i = b; i < e && i < static_cast<int> (x.size()); ++i, ++n)
            acc += static_cast<double> (x[static_cast<size_t> (i)]) * x[static_cast<size_t> (i)];
    return n > 0 ? acc / static_cast<double> (n) : 0.0;
}

double powerRatioDb (double num, double den) { return 10.0 * std::log10 (std::max (1.0e-30, num)) - 10.0 * std::log10 (std::max (1.0e-30, den)); }

json::Value dbValue (double v) { return json::Value (std::round (v * 100.0) / 100.0); }
} // namespace

bool measureQuality (const std::vector<float>& values, int blockSize, QualityReport& report, std::string& error,
                     const QualityInjector& inject, ProtectionStrength protection, bool smartMacros)
{
    report = QualityReport();
    const auto render = [&] (const std::vector<float>& mono, Stereo& out, RenderStats* stats) {
        int latency = 0;
        if (! renderPass (qualityInput (mono), values, blockSize, out, latency, error, nullptr, stats, protection, smartMacros))
            return false;
        if (inject)
            inject (out);
        return true;
    };
    const int second = qualitySamples (1.0);
    Stereo out;

    // ---- THD+N of single sines ---------------------------------------------
    for (double hz : { 40.0, 60.0, 100.0, 1000.0 })
    {
        if (! render (sumOfSines (2.0, { { hz, 0.5 } }), out, nullptr))
            return false;
        const auto mid = midOf (out);
        report.thdn.push_back ({ hz, sineThdnDb (mid.data() + second, second, kQualityFs, hz) });
    }

    // ---- two-tone IMD ------------------------------------------------------
    if (! render (sumOfSines (2.0, { { 50.0, 0.25 }, { 63.0, 0.25 } }), out, nullptr))
        return false;
    report.bassImdDb = twoToneImdDb (midOf (out).data() + second, second, kQualityFs, 50.0, 63.0, 5);
    if (! render (sumOfSines (2.0, { { 60.0, 0.4 }, { 7000.0, 0.1 } }), out, nullptr))
        return false;
    report.smpteImdDb = smpteImdDb (midOf (out).data() + second, second, kQualityFs, 60.0, 7000.0, 4);

    // ---- multitone MTND against output loudness ------------------------------
    const auto tones = multitoneFrequencies();
    for (double level : { -24.0, -18.0, -12.0 })
    {
        if (! render (multitone (2.0, tones, std::pow (10.0, level / 20.0)), out, nullptr))
            return false;
        const auto mid = midOf (out);
        report.mtnd.push_back ({ level, analyse (out, kQualityFs).integratedLufs, multitoneResidualDb (mid.data() + second, second, kQualityFs, tones) });
    }

    // ---- ducking of probe tones under kicks ---------------------------------
    {
        const std::array<double, 4> probes { 1000.0, 2000.0, 4000.0, 8000.0 };
        const int n = qualitySamples (6.0);
        std::vector<float> x (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
        {
            const double t = i / kQualityFs;
            double v = kickAt (t, 0.5);
            for (double f : probes)
                v += 0.05 * std::sin (kTwoPi * f * t);
            x[static_cast<size_t> (i)] = static_cast<float> (v);
        }
        RenderStats stats;
        if (! render (x, out, &stats))
            return false;
        const auto mid = midOf (out);
        for (double f : probes)
            report.ducking.push_back ({ f, summariseGainTrack (toneGainTrack (mid, x, kQualityFs, f, second, n), 2.0) });
        report.duckingLimiterGrMaxDb = stats.limiterGrMaxDb;
        report.duckingLimiterGrMeanDb = stats.limiterGrMeanDb;
    }

    // ---- kick onset / body and alignment -------------------------------------
    {
        const int n = qualitySamples (6.0);
        std::vector<float> x (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
        {
            const double t = i / kQualityFs, beat = std::fmod (t, 0.5);
            x[static_cast<size_t> (i)] = static_cast<float> (0.5 * std::exp (-beat * 18.0) * std::sin (kTwoPi * (50.0 + 80.0 * std::exp (-beat * 30.0)) * beat));
        }
        if (! render (x, out, nullptr))
            return false;
        const auto mid = midOf (out);
        std::vector<std::pair<int, int>> w0, w1, w2;
        double centroidShift = 0.0;
        int kicks = 0;
        for (int k = 2; k < 12; ++k) // onsets at 1.0 .. 5.5 s
        {
            const int s0 = qualitySamples (0.5 * k);
            w0.push_back ({ s0, s0 + qualitySamples (0.010) });
            w1.push_back ({ s0 + qualitySamples (0.010), s0 + qualitySamples (0.030) });
            w2.push_back ({ s0 + qualitySamples (0.040), s0 + qualitySamples (0.060) });
            centroidShift += energyCentroidMs (mid, s0, qualitySamples (0.150), kQualityFs) - energyCentroidMs (x, s0, qualitySamples (0.150), kQualityFs);
            ++kicks;
        }
        report.kickOnsetLiftDb = powerRatioDb (windowPower (mid, w0), windowPower (x, w0));
        report.kickBodyLiftDb = powerRatioDb (windowPower (mid, w1), windowPower (x, w1));
        report.kickLateLiftDb = powerRatioDb (windowPower (mid, w2), windowPower (x, w2));
        report.kickCentroidShiftMs = centroidShift / kicks;
    }

    // ---- loudness of pink noise ----------------------------------------------
    {
        const auto x = pinkNoise (qualitySamples (6.0), std::pow (10.0, -18.0 / 20.0), 5959);
        if (! render (x, out, &report.pinkStats))
            return false;
        const auto in = analyse (qualityInput (x).channels, kQualityFs);
        const auto o = analyse (out, kQualityFs);
        report.pinkInLufs = in.integratedLufs;
        report.pinkOutLufs = o.integratedLufs;
        report.pinkOutTruePeakDbtp = o.truePeakDbtp;
    }
    return true;
}

// ---- hygiene (docs/11 E10) --------------------------------------------------
bool measureHygiene (const std::vector<float>& values, double sampleRate, int blockSize, HygieneReport& report, std::string& error,
                     ProtectionStrength protection, bool smartMacros)
{
    report = HygieneReport();
    report.sampleRate = sampleRate;
    const auto samples = [sampleRate] (double seconds) { return static_cast<int> (std::lround (seconds * sampleRate)); };
    const auto render = [&] (std::vector<float> mono, Stereo& out) {
        io::AudioFileData d;
        d.sampleRate = sampleRate;
        d.numChannels = 2;
        d.channels = { mono, mono };
        int latency = 0;
        return renderPass (d, values, blockSize, out, latency, error, nullptr, nullptr, protection, smartMacros);
    };
    Stereo out;

    // Aliases of single sines, measured on the last 65536 samples.
    constexpr int kAliasN = 65536;
    for (double hz : { 1000.0, 5000.0, 7000.0, 10000.0 })
    {
        if (hz > 0.45 * sampleRate)
            continue;
        const int bin = aliasToneBin (hz, sampleRate, kAliasN);
        const double f0 = bin * sampleRate / kAliasN;
        const int n = samples (0.5) + kAliasN;
        std::vector<float> x (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
            x[static_cast<size_t> (i)] = static_cast<float> (0.5 * std::sin (kTwoPi * f0 * i / sampleRate));
        if (! render (std::move (x), out))
            return false;
        const auto mid = midOf (out);
        const double dbc = worstAliasDbc (mid.data() + (n - kAliasN), kAliasN, sampleRate, bin);
        report.alias.push_back ({ f0, dbc });
        report.worstAliasDbc = std::max (report.worstAliasDbc, dbc);
    }

    // DC of an asymmetric waveform (the stimulus of the E10 DC KnownGap).
    {
        const int n = samples (4.0);
        std::vector<float> x (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
            x[static_cast<size_t> (i)] = static_cast<float> (0.35 * std::sin (kTwoPi * 100.0 * i / sampleRate) + 0.35 * std::cos (kTwoPi * 200.0 * i / sampleRate));
        if (! render (std::move (x), out))
            return false;
        const auto mid = midOf (out);
        const int from = samples (2.0);
        report.dcDbfs = dcDbfs (mid.data() + from, n - from);
    }

    // Ultrasonic share of an in-band multitone (only where there is an ultrasonic band).
    if (sampleRate >= 88200.0)
    {
        const int n = samples (2.0);
        auto x = multitone (2.0, multitoneFrequencies(), std::pow (10.0, -18.0 / 20.0), sampleRate);
        if (! render (std::move (x), out))
            return false;
        const auto mid = midOf (out);
        const int from = samples (1.0);
        report.ultrasonicDb = powerShareAboveDb (mid.data() + from, n - from, sampleRate, 22000.0);
    }
    return true;
}

json::Value hygieneToJson (const HygieneReport& r)
{
    json::Value alias { json::Value::Array {} };
    for (const auto& a : r.alias)
    {
        json::Value v;
        v.set ("hz", std::round (a.hz * 100.0) / 100.0);
        v.set ("dbc", dbValue (a.dbc));
        alias.push (std::move (v));
    }
    json::Value v;
    v.set ("sampleRate", r.sampleRate);
    v.set ("alias", std::move (alias));
    v.set ("worstAliasDbc", dbValue (r.worstAliasDbc));
    v.set ("dcDbfs", dbValue (r.dcDbfs));
    v.set ("ultrasonicDb", r.ultrasonicDb ? dbValue (*r.ultrasonicDb) : json::Value());
    return v;
}

json::Value qualityToJson (const QualityReport& r)
{
    json::Value thdn { json::Value::Array {} };
    for (const auto& t : r.thdn)
    {
        json::Value v;
        v.set ("hz", t.hz);
        v.set ("db", dbValue (t.db));
        thdn.push (std::move (v));
    }
    json::Value imd;
    imd.set ("bassTwoToneDb", dbValue (r.bassImdDb));
    imd.set ("smpteDb", dbValue (r.smpteImdDb));

    json::Value mtnd { json::Value::Array {} };
    for (const auto& m : r.mtnd)
    {
        json::Value v;
        v.set ("inputRmsDbfs", m.inputRmsDbfs);
        v.set ("outputLufs", dbValue (m.outputLufs));
        v.set ("db", dbValue (m.db));
        mtnd.push (std::move (v));
    }

    json::Value probes { json::Value::Array {} };
    for (const auto& d : r.ducking)
    {
        json::Value v;
        v.set ("hz", d.hz);
        v.set ("spreadDb", dbValue (d.track.spreadDb));
        v.set ("dipDb", dbValue (d.track.dipDb));
        v.set ("liftDb", dbValue (d.track.liftDb));
        v.set ("downPercent", dbValue (d.track.downPercent));
        json::Value mod { json::Value::Array {} };
        for (double m : d.track.modulationDb)
            mod.push (dbValue (m));
        v.set ("modulationDb", std::move (mod));
        probes.push (std::move (v));
    }
    json::Value ducking;
    ducking.set ("probes", std::move (probes));
    ducking.set ("limiterGrMaxDb", dbValue (r.duckingLimiterGrMaxDb));
    ducking.set ("limiterGrMeanDb", dbValue (r.duckingLimiterGrMeanDb));

    json::Value kick;
    kick.set ("onsetLiftDb", dbValue (r.kickOnsetLiftDb));
    kick.set ("bodyLiftDb", dbValue (r.kickBodyLiftDb));
    kick.set ("lateLiftDb", dbValue (r.kickLateLiftDb));
    kick.set ("onsetMinusBodyDb", dbValue (r.kickOnsetLiftDb - r.kickBodyLiftDb));
    kick.set ("centroidShiftMs", dbValue (r.kickCentroidShiftMs));

    json::Value loudness;
    loudness.set ("pinkInLufs", dbValue (r.pinkInLufs));
    loudness.set ("pinkOutLufs", dbValue (r.pinkOutLufs));
    loudness.set ("pinkOutTruePeakDbtp", dbValue (r.pinkOutTruePeakDbtp));

    // The governor on the pink render, as render.stats reports it (docs/11 E06 batch 2).
    json::Value governor;
    governor.set ("scaleMin", std::round (r.pinkStats.governorScaleMin * 1000.0) / 1000.0);
    governor.set ("scaleEnd", std::round (r.pinkStats.governorScaleEnd * 1000.0) / 1000.0);
    governor.set ("stateEnd", governorStateName (r.pinkStats.governorStateEnd));
    governor.set ("measured", governorMeasuredToJson (r.pinkStats));
    loudness.set ("pinkGovernor", std::move (governor));

    json::Value v;
    v.set ("thdn", std::move (thdn));
    v.set ("imd", std::move (imd));
    v.set ("mtnd", std::move (mtnd));
    v.set ("ducking", std::move (ducking));
    v.set ("kick", std::move (kick));
    v.set ("loudness", std::move (loudness));
    return v;
}

std::string formatQuality (const QualityReport& r, const HygieneReport* h)
{
    std::string s = "THD+N   :";
    for (const auto& t : r.thdn)
        s += fmt (" %g Hz", t.hz) + fmt (" %.1f", t.db);
    s += " dB (-6 dBFS sine)\n";
    s += "IMD     : 50 + 63 Hz " + fmt ("%.1f dB", r.bassImdDb) + ", SMPTE 60 Hz + 7 kHz " + fmt ("%.1f dB", r.smpteImdDb) + "\n";
    s += "MTND    :";
    for (const auto& m : r.mtnd)
        s += fmt (" %.0f dBFS", m.inputRmsDbfs) + fmt (" -> %.1f LUFS", m.outputLufs) + fmt (" %.1f dB;", m.db);
    s += "\nDucking : under 55 Hz kicks (limiter GR max " + fmt ("%.1f", r.duckingLimiterGrMaxDb) + fmt (", mean %.1f dB)\n", r.duckingLimiterGrMeanDb);
    for (const auto& d : r.ducking)
        s += fmt ("          %5.0f Hz", d.hz) + fmt (": dip %.1f", d.track.dipDb) + fmt (", lift %.1f", d.track.liftDb)
             + fmt (", p95-p5 %.1f dB", d.track.spreadDb) + fmt (", %.0f %% > 1 dB down", d.track.downPercent)
             + fmt (", 2 Hz modulation %.2f dB\n", d.track.modulationDb[0]);
    s += "Kick    : onset (0-10 ms) " + fmt ("%+.1f", r.kickOnsetLiftDb) + ", body (10-30 ms) " + fmt ("%+.1f", r.kickBodyLiftDb)
         + ", 40-60 ms " + fmt ("%+.1f dB", r.kickLateLiftDb) + ", centroid " + fmt ("%+.2f ms\n", r.kickCentroidShiftMs);
    s += "Loudness: pink -18 dBFS RMS " + fmt ("%.1f", r.pinkInLufs) + " -> " + fmt ("%.1f LUFS", r.pinkOutLufs) + ", true peak "
         + fmt ("%.1f dBTP\n", r.pinkOutTruePeakDbtp);
    s += "Governor: on the pink, " + formatStats (r.pinkStats) + "\n";
    if (h != nullptr)
    {
        s += "Hygiene : at " + fmt ("%g Hz", h->sampleRate) + ": worst alias";
        for (const auto& a : h->alias)
            s += fmt (" %.0f Hz", a.hz) + fmt (" %.1f", a.dbc);
        s += " dBc (-6 dBFS sine), DC " + fmt ("%.1f dBFS", h->dcDbfs);
        if (h->ultrasonicDb)
            s += ", >= 22 kHz " + fmt ("%.1f dB", *h->ultrasonicDb);
        s += "\n";
    }
    return s;
}

int runQuality (const CliOptions& o)
{
    const Log log (o);
    std::string error;
    ResolvedParameters params;
    if (! buildParameters (o.render, params, error))
    {
        log.error (error);
        return kExitUsage;
    }
    logSettings (log, params, o.render);
    QualityReport report;
    HygieneReport hygiene;
    if (! measureQuality (params.values, o.render.blockSize, report, error, {}, o.render.protection, params.smart)
        || ! measureHygiene (params.values, o.rate, o.render.blockSize, hygiene, error, o.render.protection, params.smart))
    {
        log.error (error);
        return kExitFailure;
    }
    if (o.json)
    {
        json::Value v;
        v.set ("preset", params.presetDescription);
        auto q = qualityToJson (report);
        q.set ("hygiene", hygieneToJson (hygiene));
        v.set ("quality", std::move (q));
        printJson (v);
    }
    else
    {
        std::fputs (formatQuality (report, &hygiene).c_str(), stdout); // the result, printed even with --quiet
    }
    return kExitOk;
}

// ===========================================================================
// params
// ===========================================================================
int runParams (const CliOptions& o)
{
    using namespace param;
    const auto& table = layout();

    if (o.json)
    {
        json::Value arr { json::Value::Array {} };
        for (size_t i = 0; i < table.size(); ++i)
        {
            const auto& info = table[i];
            json::Value p;
            p.set ("id", static_cast<int> (i));
            p.set ("key", info.key);
            p.set ("name", info.name);
            p.set ("group", info.group);
            p.set ("unit", jsonUnitName (info.unit));
            p.set ("min", static_cast<double> (info.minValue));
            p.set ("max", static_cast<double> (info.maxValue));
            p.set ("default", static_cast<double> (info.defaultValue));
            p.set ("skewCentre", static_cast<double> (info.skewCentre));
            json::Value choices { json::Value::Array {} };
            for (const auto& c : info.choices)
                choices.push (c);
            p.set ("choices", std::move (choices));
            p.set ("structural", info.structural);
            arr.push (std::move (p));
        }
        json::Value v;
        v.set ("count", static_cast<int> (table.size()));
        v.set ("parameters", std::move (arr));
        printJson (v);
        return kExitOk;
    }

    // Groups in order of first appearance.
    std::vector<std::string> groups;
    for (const auto& info : table)
        if (std::find (groups.begin(), groups.end(), info.group) == groups.end())
            groups.push_back (info.group);

    std::string out = "Flubsound parameter layout: " + std::to_string (table.size()) + " parameters.\n"
                      "Keys are used by --set key=value and in preset files. Percent values are stored as 0..1\n"
                      "(--set accepts 0.4 or 40%); choices accept the label or its index; toggles on/off.\n";

    const size_t kKey = 26, kName = 26, kUnit = 7, kRange = 22, kDefault = 14;
    for (const auto& g : groups)
    {
        out += "\n[" + g + "]\n";
        out += padRight ("KEY", kKey) + padRight ("NAME", kName) + padRight ("UNIT", kUnit) + padRight ("RANGE", kRange)
               + padRight ("DEFAULT", kDefault) + "CHOICES / NOTES\n";
        for (size_t i = 0; i < table.size(); ++i)
        {
            const auto& info = table[i];
            if (info.group != g)
                continue;
            const int id = static_cast<int> (i);

            std::string range, notes;
            switch (info.unit)
            {
                case Unit::Toggle: range = "off / on"; break;
                case Unit::Choice:
                    range = "0 .. " + std::to_string (info.choices.size() - 1);
                    for (size_t c = 0; c < info.choices.size(); ++c)
                        notes += (c > 0 ? " | " : "") + info.choices[c];
                    break;
                case Unit::Percent:
                    range = fmt ("%g", 100.0 * info.minValue) + " .. " + fmt ("%g", 100.0 * info.maxValue) + " %";
                    break;
                case Unit::None:
                case Unit::Db:
                case Unit::Hz:
                case Unit::Ms:
                case Unit::Ratio:
                case Unit::Lufs:
                case Unit::Degrees:
                case Unit::Millimetres:
                case Unit::DbPerSec:
                    range = fmt ("%g", info.minValue) + " .. " + fmt ("%g", info.maxValue);
                    break;
            }
            if (id >= Macro1 && id <= Macro5)
                notes = std::string ("Music: ") + MacroMap::macroName (ModeValue::Music, id - Macro1)
                        + " | Gaming: " + MacroMap::macroName (ModeValue::Gaming, id - Macro1);
            if (info.structural)
                notes += (notes.empty() ? "" : "; ") + std::string ("structural (re-prepares the chain, changes latency)");

            const char* unit = info.unit == Unit::Choice || info.unit == Unit::Toggle ? "" : unitLabel (info.unit);
            std::string def = formatParameterValue (id, info.defaultValue);
            std::string line = padRight (info.key, kKey - 1) + " " + padRight (ellipsize (info.name, kName - 1), kName - 1) + " "
                               + padRight (unit, kUnit) + padRight (range, kRange) + padRight (def, kDefault) + notes;
            line.erase (line.find_last_not_of (' ') + 1); // no trailing blanks
            out += line + "\n";
        }
    }
    std::fputs (out.c_str(), stdout);
    return kExitOk;
}

// ===========================================================================
// presets
// ===========================================================================
int runPresets (const CliOptions& o)
{
    const Log log (o);
    const auto dir = findFactoryPresetDir (o.dir);
    if (! dir)
    {
        std::string msg = o.dir.empty() ? "no factory preset folder found. Searched:" : "preset folder not found: " + o.dir;
        if (o.dir.empty())
            for (const auto& d : presetSearchPath (o.dir))
                msg += "\n    " + io::pathToUtf8 (d);
        log.error (msg);
        return kExitFailure;
    }

    std::vector<std::string> problems;
    const auto entries = scanPresetDir (*dir, problems);
    for (const auto& p : problems)
        std::fputs (("warning: skipped " + p + "\n").c_str(), stderr);

    if (o.json)
    {
        json::Value arr { json::Value::Array {} };
        for (const auto& e : entries)
        {
            json::Value p;
            p.set ("name", e.name);
            p.set ("category", e.category);
            p.set ("mode", e.mode);
            p.set ("author", e.author);
            p.set ("description", e.description);
            json::Value tags { json::Value::Array {} };
            for (const auto& t : e.tags)
                tags.push (t);
            p.set ("tags", std::move (tags));
            p.set ("file", io::pathToUtf8 (e.file));
            arr.push (std::move (p));
        }
        json::Value v;
        v.set ("dir", io::pathToUtf8 (*dir));
        v.set ("presets", std::move (arr));
        printJson (v);
        return kExitOk;
    }

    std::string out = "Factory presets in " + io::pathToUtf8 (*dir) + " (" + std::to_string (entries.size()) + "):\n";
    if (entries.empty())
    {
        out += "  (none)\n";
        std::fputs (out.c_str(), stdout);
        return kExitOk;
    }
    size_t nameW = 4, catW = 8;
    for (const auto& e : entries)
    {
        nameW = std::max (nameW, displayWidth (e.name));
        catW = std::max (catW, displayWidth (e.category));
    }
    out += "\n  " + padRight ("NAME", nameW + 2) + padRight ("CATEGORY", catW + 2) + padRight ("MODE", 8) + "FILE\n";
    for (const auto& e : entries)
    {
        std::error_code ec;
        const auto rel = fs::relative (e.file, *dir, ec);
        out += "  " + padRight (e.name, nameW + 2) + padRight (e.category, catW + 2) + padRight (e.mode, 8)
               + (ec ? io::pathToUtf8 (e.file) : io::pathToUtf8Generic (rel)) + "\n";
        if (! e.description.empty())
            out += "  " + std::string (nameW + 2, ' ') + ellipsize (e.description, 90) + "\n";
    }
    out += "\nUse with: flubsound-cli process -i in.wav -o out.wav --preset \"" + entries.front().name + "\"\n";
    std::fputs (out.c_str(), stdout);
    return kExitOk;
}
} // namespace flub::cli
