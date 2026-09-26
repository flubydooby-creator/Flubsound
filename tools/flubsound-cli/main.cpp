// Flubsound Pro - flubsound-cli: batch processor, loudness analyser and
// parameter / preset browser. JUCE-free: links flub::core only.
//
//   flubsound-cli process -i in.wav -o out.wav [render options]
//   flubsound-cli batch   -i <in dir> -o <out dir> [render options] [--jobs N]
//   flubsound-cli analyze -i file.wav [--json]
//   flubsound-cli params  [--json]
//   flubsound-cli presets [--dir <dir>] [--json]
//
// Exit codes: 0 success, 1 processing / I/O failure (for batch: at least one
// file failed), 2 usage error (bad option, unknown preset or parameter).
//
// With --json the machine-readable result goes to stdout and all progress /
// human-readable text goes to stderr, so `... --json > result.json` works.

#include "Analysis.h"
#include "CliOptions.h"
#include "FactoryPresets.h"
#include "OfflineRenderer.h"

#include "flub/engine/MacroMap.h"
#include "flub/engine/Parameters.h"
#include "flub/io/Json.h"
#include "flub/io/WavFile.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef FLUB_CLI_VERSION
    #define FLUB_CLI_VERSION "0.0.0"
#endif

namespace fs = std::filesystem;
using namespace flub;
using namespace flub::cli;

namespace
{
enum ExitCode : int
{
    kExitOk = 0,
    kExitFailure = 1,
    kExitUsage = 2
};

// ===========================================================================
// Help
// ===========================================================================
const char* const kGeneralHelp = R"(flubsound-cli - Flubsound Pro batch processor and loudness analyser

Usage:
  flubsound-cli process -i in.wav -o out.wav [render options]
  flubsound-cli batch   -i <in dir> -o <out dir> [render options] [--jobs N] [--recursive]
  flubsound-cli analyze -i file.wav [--json]
  flubsound-cli params  [--json]
  flubsound-cli presets [--dir <dir>] [--json]
  flubsound-cli help <command>        detailed help for one command
  flubsound-cli --version

Render options (process / batch):
  -p, --preset <file.json|name>  preset file, or factory preset name (see `presets`)
      --preset-dir <dir>         factory preset folder for --preset <name>
  -m, --mode music|gaming        processing mode (selects the macro set)
  -b, --boost 0-100              Boost Intensity
      --macro N=0-100            mode macro, N = 1..5 or its name (punch=60,
                                 footsteps=40, ...); repeatable, "--macro 1=50 3=20"
  -s, --set key=value            any parameter (keys: `flubsound-cli params`);
                                 repeatable, "--set eq.0.gain=3 bass.freq=60Hz"
  -t, --target-lufs L            integrated loudness target (iterative render)
  -c, --ceiling dBTP             true-peak ceiling of the maximizer (-12..0)
      --profile quality|balanced|low-latency
                                 latency profile (default: from the preset,
                                 Balanced = what the real-time engine uses)
  -f, --format f32|pcm24|pcm16   output sample format (default f32; PCM is
                                 TPDF dithered)
      --block N                  processing block size (default 512)
  -q, --quiet                    only print errors (and --json output)
      --json                     machine-readable result on stdout

Precedence: defaults < --preset < --mode < --boost/--macro/--profile/--ceiling
< --set. Bypass is never taken from a preset (use --set bypass=on).

Exit codes: 0 ok, 1 processing or I/O failure, 2 usage error.
)";

const char* const kProcessHelp = R"(flubsound-cli process -i in.wav -o out.wav [render options]

Renders one WAV file through the Flubsound processing chain - exactly the
code the real-time engine runs - and writes a stereo WAV of the same length
and sample rate.

  * Sample-aligned: the chain latency is compensated (the first L output
    samples are dropped and L samples of silence flush the tail), so the
    output lines up with the input sample for sample.
  * Channels: mono is duplicated to stereo; 5.1 (6 ch) and 7.1 (8 ch) files
    are virtualised binaurally (virt.on, default) or downmixed (ITU-R BS.775)
    to stereo.
  * --target-lufs L: render, measure the integrated loudness (EBU R128),
    move max.drive (0..24 dB) by the error and render again - up to 4 more
    passes, stopping within 0.3 LU. The maximizer's true-peak limiter holds
    the ceiling (--ceiling, default from the preset / -1 dBTP).
  * Percent parameters are stored as 0..1: --set clarity.air=0.4 or =40%.

Examples:
  flubsound-cli process -i song.wav -o song-fx.wav --preset "Punchy Pop" --boost 60
  flubsound-cli process -i song.wav -o master.wav --target-lufs -14 --ceiling -1 --format pcm24
  flubsound-cli process -i clip.wav -o clip-fx.wav --mode gaming --macro footsteps=70 --set eq.3.gain=-2
)";

const char* const kBatchHelp = R"(flubsound-cli batch -i <in dir> -o <out dir> [render options] [--jobs N] [--recursive]

Processes every .wav file of a folder with the same settings (see `help
process`). Non-WAV files are skipped. Output files keep their names (and,
with --recursive, their sub-folders) below the output folder, which is
created if needed and must differ from the input folder.

  -j, --jobs N       parallel jobs (default: number of CPU cores). Every job
                     has its own parameter store and processing chain.
  -r, --recursive    include sub-folders

A summary table (loudness in/out, true peak, passes, time) is printed at the
end; with --json the per-file results are written to stdout instead.
Exit code 1 if any file failed.

Example:
  flubsound-cli batch -i ./album -o ./album-fx --mode music --boost 40 --target-lufs -14 --jobs 4 --format pcm24
)";

const char* const kAnalyzeHelp = R"(flubsound-cli analyze -i file.wav [--json]

Measures a WAV file with the engine's meters:
  integrated loudness (LUFS, EBU R128 gating), loudness range (LU, EBU Tech
  3342), maximum momentary (400 ms) and short-term (3 s) loudness, true peak
  (dBTP, 4x oversampled), sample peak and RMS (dBFS; a full-scale sine reads
  -3.01 dBFS RMS), per channel as well, plus duration, rate and channels.
5.1 / 7.1 files use the BS.1770 channel weights (LFE excluded).
Values that cannot be measured (silence, < 400 ms) print as -inf / null.
)";

const char* const kParamsHelp = R"(flubsound-cli params [--json]

Lists every parameter of the processing chain: key (for --set and preset
files), name, unit, range, default and choices. Percent values are stored as
0..1 and shown x100. --json prints the raw layout (stored units).
)";

const char* const kPresetsHelp = R"(flubsound-cli presets [--dir <dir>] [--json]

Lists the factory presets (presets/factory/**/*.json). The folder is searched
in this order: --dir, $FLUBSOUND_PRESET_DIR, presets/factory next to the
executable or up to four parent folders above it, ../share/flubsound/presets/
factory, and the source tree the CLI was built from.
`--preset <name>` in process / batch resolves names against the same folder
(exact name, file name, then a unique loose prefix / substring match).
)";

void printHelp (const std::string& topic, std::FILE* stream)
{
    std::string t = topic;
    std::transform (t.begin(), t.end(), t.begin(), [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    const char* text = kGeneralHelp;
    if (t == "process")
        text = kProcessHelp;
    else if (t == "batch")
        text = kBatchHelp;
    else if (t == "analyze" || t == "analyse")
        text = kAnalyzeHelp;
    else if (t == "params" || t == "parameters")
        text = kParamsHelp;
    else if (t == "presets")
        text = kPresetsHelp;
    std::fputs (text, stream);
}

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

std::string padRight (std::string s, size_t width)
{
    if (s.size() < width)
        s.append (width - s.size(), ' ');
    return s;
}

std::string padLeft (std::string s, size_t width)
{
    if (s.size() < width)
        s.insert (0, width - s.size(), ' ');
    return s;
}

/** Keeps the end of long names ("...ong-file-name.wav"). */
std::string ellipsize (const std::string& s, size_t width)
{
    if (s.size() <= width || width < 4)
        return s;
    return "..." + s.substr (s.size() - (width - 3));
}

void printJson (const json::Value& v)
{
    std::fputs ((json::write (v, 2) + "\n").c_str(), stdout);
    std::fflush (stdout);
}

std::string lowerExtension (const fs::path& p)
{
    std::string e = p.extension().string();
    std::transform (e.begin(), e.end(), e.begin(), [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    return e;
}

bool isWavFile (const fs::path& p)
{
    const auto e = lowerExtension (p);
    return e == ".wav" || e == ".wave";
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
    for (const auto& n : p.notes)
        s += "Note    : " + n + "\n";
    return s;
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
    r.set ("outputGainDb", std::round (rr.outputGainDb * 100.0) / 100.0);
    r.set ("targetLufs", o.targetLufs ? json::Value (static_cast<double> (*o.targetLufs)) : json::Value());
    r.set ("targetReached", rr.targetReached);
    r.set ("renderSeconds", std::round (rr.renderSeconds * 1000.0) / 1000.0);
    r.set ("realtimeFactor", rr.renderSeconds > 0.0 ? std::round (10.0 * audioSeconds * rr.passes / rr.renderSeconds) / 10.0 : 0.0);
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
    if (sameFile (o.input, o.output))
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
    log.info (describeSettings (params, o.render));
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
    if (! io::writeWav (o.output, rr.output, o.render.format, error))
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
              + (rr.outputGainDb != params.values[static_cast<size_t> (param::OutputGainDb)] ? ", output.gain " + fmt ("%.2f dB", rr.outputGainDb) : std::string())
              + (rr.renderSeconds > 0.0 ? fmt (", %.1fx realtime", audioSeconds * rr.passes / rr.renderSeconds) : std::string()) + "\n");
    for (const auto& n : rr.notes)
        log.info ("Note    : " + n + "\n");

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
struct BatchJob
{
    fs::path input, output;
    std::string displayName; // path relative to the input folder
};

struct BatchResult
{
    bool ok = false;
    std::string error;
    std::string inFormat;
    LoudnessReport inReport, outReport;
    RenderResult render; // output audio released after writing
    double seconds = 0.0; // wall time for the job
};

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

void runBatchJob (const BatchJob& job, const std::vector<float>& values, const RenderSettings& settings, io::SampleFormat format,
                  BatchResult& result)
{
    const auto t0 = std::chrono::steady_clock::now();
    try
    {
        io::AudioFileData input;
        std::string error;
        if (! io::readWav (job.input.string(), input, error) || ! checkRenderable (input, error))
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
                if (! io::writeWav (job.output.string(), result.render.output, format, error))
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

int runBatch (const CliOptions& o)
{
    const Log log (o);
    std::string error;
    std::error_code ec;

    const fs::path inDir (o.input), outDir (o.output);
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
    const fs::path outCanonical = fs::weakly_canonical (outDir, ec);
    std::vector<BatchJob> jobs;
    std::vector<std::string> skipped;
    auto consider = [&] (const fs::directory_entry& entry) {
        std::error_code fileEc;
        if (! entry.is_regular_file (fileEc))
            return;
        const fs::path rel = entry.path().lexically_relative (inDir);
        if (! isWavFile (entry.path()))
        {
            skipped.push_back (rel.generic_string());
            return;
        }
        if (o.recursive && isInside (fs::weakly_canonical (entry.path(), fileEc), outCanonical))
            return; // output folder nested in the input folder: never re-process our own results
        jobs.push_back ({ entry.path(), outDir / rel, rel.generic_string() });
    };

    const auto dirOptions = fs::directory_options::skip_permission_denied;
    if (o.recursive)
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
        log.error ("cannot list " + o.input + ": " + ec.message());
        return kExitFailure;
    }
    std::sort (jobs.begin(), jobs.end(), [] (const BatchJob& a, const BatchJob& b) { return a.displayName < b.displayName; });

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

    const unsigned hw = std::max (1u, std::thread::hardware_concurrency());
    const size_t numWorkers = std::min (jobs.size(), static_cast<size_t> (o.jobs > 0 ? static_cast<unsigned> (o.jobs) : hw));

    log.info (describeSettings (params, o.render));
    log.info ("Batch   : " + std::to_string (jobs.size()) + " WAV file(s), " + std::to_string (numWorkers) + " job(s)"
              + (skipped.empty() ? std::string() : ", skipping " + std::to_string (skipped.size()) + " non-WAV file(s)") + "\n");

    // ---- Thread pool: workers pull the next job index -------------------------
    const RenderSettings settings = makeRenderSettings (o.render, params);
    std::vector<BatchResult> results (jobs.size());
    std::atomic<size_t> nextJob { 0 };
    std::atomic<size_t> finished { 0 };
    const auto wallStart = std::chrono::steady_clock::now();
    const size_t countWidth = std::to_string (jobs.size()).size();

    auto worker = [&] {
        for (;;)
        {
            const size_t index = nextJob.fetch_add (1);
            if (index >= jobs.size())
                return;
            BatchResult& r = results[index];
            runBatchJob (jobs[index], params.values, settings, o.render.format, r);

            const size_t done = finished.fetch_add (1) + 1;
            std::string line = "[" + padLeft (std::to_string (done), countWidth) + "/" + std::to_string (jobs.size()) + "] ";
            if (r.ok)
                line += "ok      " + jobs[index].displayName + "  " + formatDb (r.inReport.integratedLufs, 1) + " -> "
                        + formatDb (r.outReport.integratedLufs, 1) + " LUFS, TP " + formatDb (r.outReport.truePeakDbtp, 1) + " dBTP"
                        + fmt (", %.1f s", r.seconds) + (r.render.targetReached ? "" : "  (target missed)") + "\n";
            else
                line += "FAILED  " + jobs[index].displayName + ": " + r.error + "\n";
            log.info (line);
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
        log.info (std::string ("Note    : could only start ") + std::to_string (pool.size()) + " worker(s): " + e.what() + "\n");
        if (pool.empty())
            worker();
    }
    for (auto& t : pool)
        t.join();
    const double wallSeconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - wallStart).count();

    // ---- Summary ------------------------------------------------------------------
    size_t okCount = 0;
    double audioSeconds = 0.0;
    for (const auto& r : results)
        if (r.ok)
        {
            ++okCount;
            audioSeconds += r.inReport.durationSeconds;
        }
    const size_t failCount = results.size() - okCount;

    if (! o.quiet)
    {
        size_t nameWidth = 4;
        for (const auto& j : jobs)
            nameWidth = std::max (nameWidth, std::min<size_t> (j.displayName.size(), 48));

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
        log.info (table);
    }

    if (o.json)
    {
        json::Value files { json::Value::Array {} };
        for (size_t i = 0; i < jobs.size(); ++i)
        {
            const auto& r = results[i];
            json::Value f;
            f.set ("file", jobs[i].input.string());
            f.set ("outputFile", jobs[i].output.string());
            f.set ("status", r.ok ? "ok" : "failed");
            if (! r.ok)
                f.set ("error", r.error);
            if (r.ok)
            {
                f.set ("input", reportToJson (r.inReport, jobs[i].input.string(), r.inFormat));
                f.set ("output", reportToJson (r.outReport, jobs[i].output.string(), sampleFormatName (o.render.format)));
                f.set ("render", renderInfoJson (r.render, o.render, params, r.inReport.sampleRate, r.inReport.durationSeconds));
            }
            f.set ("seconds", std::round (r.seconds * 1000.0) / 1000.0);
            files.push (std::move (f));
        }
        json::Value skippedJson { json::Value::Array {} };
        for (const auto& s : skipped)
            skippedJson.push (s);

        json::Value summary;
        summary.set ("ok", static_cast<int> (okCount));
        summary.set ("failed", static_cast<int> (failCount));
        summary.set ("skipped", static_cast<int> (skipped.size()));
        summary.set ("jobs", static_cast<int> (numWorkers));
        summary.set ("wallSeconds", std::round (wallSeconds * 1000.0) / 1000.0);

        json::Value v;
        v.set ("files", std::move (files));
        v.set ("skipped", std::move (skippedJson));
        v.set ("summary", std::move (summary));
        printJson (v);
    }
    return failCount == 0 ? kExitOk : kExitFailure;
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
            out += padRight (info.key, kKey - 1) + " " + padRight (ellipsize (info.name, kName - 1), kName - 1) + " "
                   + padRight (unit, kUnit) + padRight (range, kRange) + padRight (def, kDefault) + notes + "\n";
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
                msg += "\n    " + d.string();
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
            p.set ("file", e.file.string());
            arr.push (std::move (p));
        }
        json::Value v;
        v.set ("dir", dir->string());
        v.set ("presets", std::move (arr));
        printJson (v);
        return kExitOk;
    }

    std::string out = "Factory presets in " + dir->string() + " (" + std::to_string (entries.size()) + "):\n";
    if (entries.empty())
    {
        out += "  (none)\n";
        std::fputs (out.c_str(), stdout);
        return kExitOk;
    }
    size_t nameW = 4, catW = 8;
    for (const auto& e : entries)
    {
        nameW = std::max (nameW, e.name.size());
        catW = std::max (catW, e.category.size());
    }
    out += "\n  " + padRight ("NAME", nameW + 2) + padRight ("CATEGORY", catW + 2) + padRight ("MODE", 8) + "FILE\n";
    for (const auto& e : entries)
    {
        std::error_code ec;
        const auto rel = fs::relative (e.file, *dir, ec);
        out += "  " + padRight (e.name, nameW + 2) + padRight (e.category, catW + 2) + padRight (e.mode, 8)
               + (ec ? e.file.string() : rel.generic_string()) + "\n";
        if (! e.description.empty())
            out += "  " + std::string (nameW + 2, ' ') + ellipsize (e.description, 90) + "\n";
    }
    out += "\nUse with: flubsound-cli process -i in.wav -o out.wav --preset \"" + entries.front().name + "\"\n";
    std::fputs (out.c_str(), stdout);
    return kExitOk;
}
} // namespace

// ===========================================================================
int main (int argc, char** argv)
{
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.emplace_back (argv[i]);

    CliOptions options;
    std::string error;
    if (! parseCommandLine (args, options, error))
    {
        std::fprintf (stderr, "error: %s\nRun `flubsound-cli --help` for usage.\n", error.c_str());
        return kExitUsage;
    }

    try
    {
        switch (options.command)
        {
            case Command::Help:
                if (args.empty())
                {
                    printHelp ({}, stderr); // bare invocation is a usage error
                    return kExitUsage;
                }
                printHelp (options.helpTopic, stdout);
                return kExitOk;
            case Command::Version:
                std::printf ("flubsound-cli %s (Flubsound Pro - Music & Gaming Edition)\n", FLUB_CLI_VERSION);
                return kExitOk;
            case Command::Process: return runProcess (options);
            case Command::Batch: return runBatch (options);
            case Command::Analyze: return runAnalyze (options);
            case Command::Params: return runParams (options);
            case Command::Presets: return runPresets (options);
            case Command::None: break;
        }
    }
    catch (const std::bad_alloc&)
    {
        std::fputs ("error: out of memory\n", stderr);
        return kExitFailure;
    }
    catch (const std::exception& e)
    {
        std::fprintf (stderr, "error: %s\n", e.what());
        return kExitFailure;
    }
    printHelp ({}, stderr);
    return kExitUsage;
}
