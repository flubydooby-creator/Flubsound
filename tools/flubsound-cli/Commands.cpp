#include "Commands.h"

#include "FactoryPresets.h"

#include "flub/engine/MacroMap.h"
#include "flub/engine/Parameters.h"
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

json::Value renderInfoJson (const RenderResult& rr, const RenderOptions& o, const ResolvedParameters& p, double sampleRate,
                            double audioSeconds)
{
    json::Value r;
    r.set ("preset", p.presetDescription);
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
    if (o.ceilingDb || o.targetLufs)
        rs.verifyCeilingDb = p.values[static_cast<size_t> (param::MaxCeilingDb)];
    return rs;
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
    logNotes (log, rr.notes);

    if (o.json)
    {
        json::Value v;
        v.set ("input", reportToJson (inReport, o.input, inFormat));
        v.set ("output", reportToJson (rr.outputReport, o.output, sampleFormatName (o.render.format)));
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
    const std::string format = sampleFormatName (input.sourceFormat);
    if (o.json)
        printJson (reportToJson (r, o.input, format));
    else
        std::fputs (formatReport (r, o.input, format).c_str(), stdout);
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
