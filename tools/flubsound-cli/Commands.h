// Flubsound Pro CLI - the commands behind `process`, `batch`, `analyze`,
// `quality`, `soak`, `params` and `presets` (main.cpp only parses the command
// line, prints help and dispatches here).
//
// The batch building blocks are declared here so tests/test_offline_render.cpp
// can run them directly: the folder walk (collectBatchJobs), one file
// (runBatchJob), the worker pool (runBatchJobs) and the per-file reports
// (formatBatchSummary, batchResultsToJson). The render-and-write glue
// (renderFile, writeRender) is in OfflineRenderer.h, which the desktop app's
// Export / batch dialog compiles too.
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
#include <array>
#include <functional>
#include <optional>
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
int runQuality (const CliOptions& options);
int runSoak (const CliOptions& options);
int runParams (const CliOptions& options);
int runPresets (const CliOptions& options);

/** .wav / .wave, case-insensitive. */
bool isWavFile (const std::filesystem::path& path);

/** Render settings for these options: block size, loudness target, and the
    ceiling check when --ceiling or --target-lufs was given. */
RenderSettings makeRenderSettings (const RenderOptions& options, const ResolvedParameters& params);

/** The `render.stats` object of `process` / `batch --json` (RenderStats,
    dB rounded to 0.01, null at the -160 dB floor). */
json::Value renderStatsToJson (const RenderStats& stats);

/** "idle", "backingOff", "holding", "recovering" (SafetyGovernor::State order). */
const char* governorStateName (int state) noexcept;

/** One-line human-readable summary of the stats ("Stats   : ..." in `process`). */
std::string formatStats (const RenderStats& stats);

// ---- quality (docs/11 E59) ---------------------------------------------------
// `flubsound-cli quality` renders a fixed set of pinned stimuli (48 kHz,
// stereo, identical channels, generated here from fixed seeds) through one
// parameter table and measures the mid of each render with the Analysis.h
// quality metrics. tests/test_known_gaps.cpp runs the same function, so a
// number printed by the CLI is the number a test pins.
//
//   thdn     sines at 40 / 60 / 100 / 1000 Hz, -6 dBFS peak, 2 s: THD+N over 1..2 s
//   imd      50 + 63 Hz at -12 dBFS peak each (a bass third): products up to
//            5th order; 60 Hz + 7 kHz 4:1 at -6 dBFS peak (SMPTE): sidebands
//            7 kHz +- k 60 Hz, k = 1..4; both over 1..2 s
//   mtnd     31 log-spaced tones 40 Hz..16 kHz (integer Hz, pink amplitudes,
//            seeded phases) at -24 / -18 / -12 dBFS RMS, 2 s: residual after
//            the tones over 1..2 s, and the output's integrated loudness
//   ducking  55 Hz kicks (peak -6 dBFS, tau 100 ms, 350 ms) every 500 ms from
//            250 ms under 1 / 2 / 4 / 8 kHz probes at -26 dBFS peak each, 6 s:
//            each probe's gain track over 1..6 s (GainTrackStats, modulation
//            at k x 2 Hz), plus the limiter's GR over the scene
//   kick     50 + 80 Hz chirp kick (e^-18t, peak -6 dBFS) every 500 ms, 6 s:
//            output vs input power 0-10 / 10-30 / 40-60 ms after each onset
//            from 1 s on, and the shift of the energy centroid of 0-150 ms
//   loudness pink noise at -18 dBFS RMS, 6 s: integrated loudness in and out,
//            output true peak
struct QualityReport
{
    struct Thdn
    {
        double hz = 0.0, db = 0.0;
    };
    struct Mtnd
    {
        double inputRmsDbfs = 0.0, outputLufs = 0.0, db = 0.0;
    };
    struct Ducking
    {
        double hz = 0.0;
        GainTrackStats track;
    };

    std::vector<Thdn> thdn;
    double bassImdDb = 0.0, smpteImdDb = 0.0;
    std::vector<Mtnd> mtnd;
    std::vector<Ducking> ducking;
    float duckingLimiterGrMaxDb = 0.0f, duckingLimiterGrMeanDb = 0.0f;
    double kickOnsetLiftDb = 0.0, kickBodyLiftDb = 0.0, kickLateLiftDb = 0.0, kickCentroidShiftMs = 0.0;
    double pinkInLufs = 0.0, pinkOutLufs = 0.0, pinkOutTruePeakDbtp = 0.0;
};

/** Called on every rendered stimulus (2 planar channels) before it is
    measured: tests inject known artefacts into a pass-through render to
    check that each metric reads them (docs/11 E59 meta-validation). */
using QualityInjector = std::function<void (std::vector<std::vector<float>>& outStereo)>;

/** Renders and measures every quality stimulus with `values` (param::kNumParams
    base values) at `blockSize`. Non-RT, allocates; about 34 s of audio. */
bool measureQuality (const std::vector<float>& values, int blockSize, QualityReport& report, std::string& error,
                     const QualityInjector& inject = {}, ProtectionStrength protection = ProtectionStrength::Off);

/** Signal hygiene (docs/11 E10) at any sample rate: `quality`'s hygiene
    family, measured at `--rate` (default 48 kHz) on the output mid.
      alias       sines at 1 / 5 / 7 / 10 kHz (those below 0.45 fs), -6 dBFS
                  peak, on odd FFT bins of a 65536-point analysis after
                  0.5 s: worstAliasDbc (Analysis.h) in 20 Hz .. 20 kHz
      dc          0.35 sin (100 Hz) + 0.35 cos (200 Hz) (an asymmetric
                  waveform with no DC), 4 s: output DC over 2..4 s
      ultrasonic  at 88.2 kHz and above: the 31-tone pink multitone (40 Hz ..
                  16 kHz) at -18 dBFS RMS, 2 s: power at and above 22 kHz
                  re the whole output over the last second */
struct HygieneReport
{
    struct Alias
    {
        double hz = 0.0, dbc = 0.0; // hz: the exact (bin-centred) tone
    };
    double sampleRate = 48000.0;
    std::vector<Alias> alias;
    double worstAliasDbc = -160.0; // over the tones
    double dcDbfs = -160.0;
    std::optional<double> ultrasonicDb; // only at 88.2 kHz and above
};

bool measureHygiene (const std::vector<float>& values, double sampleRate, int blockSize, HygieneReport& report, std::string& error,
                     ProtectionStrength protection = ProtectionStrength::Off);

/** The hygiene object of `quality --json` (dB rounded to 0.01). */
json::Value hygieneToJson (const HygieneReport& report);

/** The `quality --json` object (dB rounded to 0.01). */
json::Value qualityToJson (const QualityReport& report);

/** Human-readable multi-line report (the hygiene lines when `hygiene` is given). */
std::string formatQuality (const QualityReport& report, const HygieneReport* hygiene = nullptr);

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
