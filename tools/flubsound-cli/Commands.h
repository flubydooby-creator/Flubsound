// Flubsound Pro CLI - the commands behind `process`, `batch`, `analyze`,
// `params` and `presets` (main.cpp only parses the command line, prints help
// and dispatches here).
//
// The render-and-write glue and the batch building blocks are declared here
// so tests/test_offline_render.cpp can run them directly: the folder walk
// (collectBatchJobs), one file (runBatchJob), the worker pool (runBatchJobs)
// and the per-file reports (formatBatchSummary, batchResultsToJson).
//
// The output report of `process` / `batch` describes the file as written:
// for PCM16 / PCM24 the written file is read back and measured, so the
// report includes quantisation and TPDF dither (float32 round-trips
// bit-exactly, so the render's own analysis is the file's).
#pragma once

#include "Analysis.h"
#include "CliOptions.h"
#include "OfflineRenderer.h"

#include "flub/io/Json.h"
#include "flub/io/WavFile.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace flub::cli
{
enum ExitCode : int
{
    kExitOk = 0,
    kExitFailure = 1,
    kExitUsage = 2
};

int runProcess (const CliOptions& options);
int runBatch (const CliOptions& options);
int runAnalyze (const CliOptions& options);
int runParams (const CliOptions& options);
int runPresets (const CliOptions& options);

/** .wav / .wave, case-insensitive. */
bool isWavFile (const std::filesystem::path& path);

/** Render settings for these options: block size, loudness target, and the
    ceiling check when --ceiling or --target-lufs was given. */
RenderSettings makeRenderSettings (const RenderOptions& options, const ResolvedParameters& params);

/** Writes result.output to `path` in `format` (Float32, Pcm24 or Pcm16) and
    sets result.outputReport to the analysis of the samples actually written
    (PCM files are read back). Returns false with a message on failure. */
bool writeRender (const std::string& path, io::SampleFormat format, RenderResult& result, std::string& error);

// ---- batch ----------------------------------------------------------------
struct BatchJob
{
    std::filesystem::path input, output;
    std::string displayName; // path relative to the input folder (forward slashes)
};

struct BatchResult
{
    bool ok = false;
    std::string error;
    std::string inFormat;
    LoudnessReport inReport, outReport; // outReport: the written file
    RenderResult render;                // output audio released after writing
    double seconds = 0.0;               // wall time for the job
};

/** Folder walk: every regular .wav / .wave file of inDir (and its sub-folders
    with `recursive`), sorted by displayName, each mapped to the same relative
    path below outDir; other files are listed in `skipped`. Files inside an
    output folder nested in inDir are ignored. Returns kExitOk, kExitFailure
    (folder cannot be listed) or kExitUsage (an output would overwrite an
    input) with `error`. An empty job list is not an error here. */
int collectBatchJobs (const std::filesystem::path& inDir, const std::filesystem::path& outDir, bool recursive,
                      std::vector<BatchJob>& jobs, std::vector<std::string>& skipped, std::string& error);

/** min(numJobs, requestedJobs), or the number of CPU cores for requestedJobs <= 0. */
size_t batchWorkerCount (size_t numJobs, int requestedJobs);

/** Reads, renders and writes one file (creating its output folder). Never
    throws: every failure ends up in result.error with result.ok == false. */
void runBatchJob (const BatchJob& job, const std::vector<float>& values, const RenderSettings& settings, io::SampleFormat format,
                  BatchResult& result);

/** Runs every job on numWorkers std::threads (each job has its own parameter
    store and chain). results[i] belongs to jobs[i]; a failed job does not stop
    the others. onFinished (job index) is called on the worker thread right
    after each job; onNote reports a worker thread that could not be started. */
void runBatchJobs (const std::vector<BatchJob>& jobs, const std::vector<float>& values, const RenderSettings& settings,
                   io::SampleFormat format, size_t numWorkers, std::vector<BatchResult>& results,
                   const std::function<void (size_t)>& onFinished, const std::function<void (const std::string&)>& onNote);

/** The summary table printed at the end of a batch. */
std::string formatBatchSummary (const std::vector<BatchJob>& jobs, const std::vector<BatchResult>& results,
                                const std::vector<std::string>& skipped, double wallSeconds);

/** The `batch --json` document: per-file results, skipped files, summary. */
json::Value batchResultsToJson (const std::vector<BatchJob>& jobs, const std::vector<BatchResult>& results,
                                const std::vector<std::string>& skipped, size_t numWorkers, double wallSeconds,
                                const RenderOptions& options, const ResolvedParameters& params);
} // namespace flub::cli
