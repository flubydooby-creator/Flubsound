// flubsound-cli render / export / batch path (tools/flubsound-cli, compiled
// into flub_tests by tests/CMakeLists.txt): OfflineRenderer against the
// ProcessingChain run directly, the loudness-target loop, `process` writing
// float32 / PCM24 / PCM16 files, the report of the written file, and `batch`
// (folder walk, parallel jobs, per-file results, a corrupt file).
//
// Every test works in its own folder below the system temp path, named after
// the test plus a unique suffix and removed afterwards.
#include "TestFramework.h"

#include "Analysis.h"
#include "CliOptions.h"
#include "Commands.h"
#include "OfflineRenderer.h"

#include "flub/common/Denormals.h"
#include "flub/common/Math.h"
#include "flub/engine/Parameters.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/io/FilePath.h"
#include "flub/io/WavFile.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <set>
#include <thread>

using namespace flub;
using namespace flub::cli;
using namespace flub::param;

namespace
{
namespace fs = std::filesystem;

/** A fresh folder below the system temp path, removed (with its contents) on
    destruction. The name is unique to this instance (ticks, random, counter)
    and the folder must not exist yet, so test runs on the same machine at the
    same time never share, wipe or fill each other's folders. */
struct TempDir
{
    explicit TempDir (const std::string& name)
    {
        static std::atomic<int> counter { 0 };
        for (int attempt = 0;; ++attempt)
        {
            const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
            path = fs::temp_directory_path()
                   / ("flub_test_offline_render_" + name + "_" + std::to_string (ticks) + "_" + std::to_string (std::random_device {}())
                      + "_" + std::to_string (++counter));
            std::error_code ec;
            if (fs::create_directory (path, ec)) // false: the name exists already (or an error)
                break;
            REQUIRE (attempt < 100);
        }
    }

    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all (path, ec);
    }

    TempDir (const TempDir&) = delete;
    TempDir& operator= (const TempDir&) = delete;

    /** UTF-8 path of `relative` (forward slashes, may contain UTF-8) inside the folder. */
    std::string file (const std::string& relative) const { return io::pathToUtf8 (path / io::pathFromUtf8 (relative)); }

    fs::path path;
};

/** Drum-like programme (kick, noise hats, bass, pad) - deterministic for a seed. */
io::AudioFileData makeProgramme (int numChannels, double sampleRate, double seconds, float level, uint32_t seed)
{
    io::AudioFileData d;
    d.sampleRate = sampleRate;
    d.numChannels = numChannels;
    const auto n = static_cast<size_t> (std::lround (seconds * sampleRate));
    d.channels.assign (static_cast<size_t> (numChannels), std::vector<float> (n, 0.0f));
    FastRandom rng (seed);
    for (size_t i = 0; i < n; ++i)
    {
        const double t = static_cast<double> (i) / sampleRate;
        const double beat = std::fmod (t, 0.5);
        const double kick = std::exp (-beat * 18.0) * std::sin (kTwoPi * (50.0 + 80.0 * std::exp (-beat * 30.0)) * beat);
        const double hat = (std::fmod (t + 0.25, 0.5) < 0.03 ? 0.3 : 0.0) * rng.nextBipolar();
        const double tone = 0.4 * std::sin (kTwoPi * 55.0 * t) + 0.15 * std::sin (kTwoPi * 440.0 * t);
        for (int c = 0; c < numChannels; ++c)
            d.channels[static_cast<size_t> (c)][i] = level * static_cast<float> (kick + (c == 1 ? 0.8 : 1.0) * hat + tone * (1.0 - 0.1 * c));
    }
    return d;
}

std::vector<float> defaultValues()
{
    std::vector<float> v (static_cast<size_t> (kNumParams));
    for (int id = 0; id < kNumParams; ++id)
        v[static_cast<size_t> (id)] = layout()[static_cast<size_t> (id)].defaultValue;
    return v;
}

/** The base values `--boost <percent>` resolves to (no preset). */
std::vector<float> boostedValues (float percent)
{
    RenderOptions o;
    o.boostPercent = percent;
    ResolvedParameters p;
    std::string error;
    const bool ok = buildParameters (o, p, error);
    CHECK (ok);
    return ok ? p.values : defaultValues();
}

void bypassAllModules (std::vector<float>& v)
{
    for (int id : { GateOn, EqOn, DynEqOn, BassOn, ClarityOn, SaturationOn, SpatialOn, VirtualizerOn, CompressorOn, MaximizerOn })
        v[static_cast<size_t> (id)] = 0.0f;
}

double maxAbsDiff (const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size())
        return 1.0e9;
    double m = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        m = std::max (m, static_cast<double> (std::abs (a[i] - b[i])));
    return m;
}

/** The chain run by hand, the way the renderer documents it: one silent
    priming block + reset(), then the input followed by L samples of silence
    in blocks of `blockSize`; returns the raw 2-channel chain output
    (input length + L). */
std::vector<std::vector<float>> runChainDirectly (const io::AudioFileData& input, const std::vector<float>& values, int blockSize,
                                                  int& latency)
{
    ParameterStore store;
    for (int id = 0; id < kNumParams; ++id)
        store.set (Bank::A, id, values[static_cast<size_t> (id)]);
    store.setActiveBank (Bank::A);
    ProcessingChain chain (store);
    chain.prepare ({ input.sampleRate, blockSize, 2 });
    latency = chain.getLatencySamples();

    const auto n = static_cast<size_t> (input.numFrames()) + static_cast<size_t> (latency);
    std::vector<std::vector<float>> buf (2, std::vector<float> (n, 0.0f));
    for (size_t c = 0; c < 2; ++c)
        std::copy (input.channels[input.numChannels == 1 ? 0 : c].begin(), input.channels[input.numChannels == 1 ? 0 : c].end(),
                   buf[c].begin());

    ScopedNoDenormals noDenormals;
    AudioBuffer silence (2, blockSize);
    chain.process (silence.block (2, blockSize));
    chain.reset();
    float* ptrs[2] = { buf[0].data(), buf[1].data() };
    for (size_t pos = 0; pos < n; pos += static_cast<size_t> (blockSize))
        chain.process (AudioBlock (ptrs, 2, static_cast<int> (std::min<size_t> (static_cast<size_t> (blockSize), n - pos)),
                                   static_cast<int> (pos)));
    return buf;
}

void writeBytes (const std::string& utf8Path, const std::string& bytes)
{
    std::ofstream f (io::pathFromUtf8 (utf8Path), std::ios::binary);
    f.write (bytes.data(), static_cast<std::streamsize> (bytes.size()));
}

bool contains (const std::string& s, const std::string& part) { return s.find (part) != std::string::npos; }
} // namespace

// ---------------------------------------------------------------------------
TEST_CASE ("OfflineRenderer: a pass is the ProcessingChain run directly, latency-compensated to the input length")
{
    const auto input = makeProgramme (2, 48000.0, 1.5, 0.3f, 11);
    const auto values = boostedValues (60.0f);

    std::vector<std::vector<float>> out;
    int latency = -1;
    std::string error;
    REQUIRE (renderPass (input, values, 256, out, latency, error));
    REQUIRE (out.size() == 2);
    CHECK (out[0].size() == input.channels[0].size());
    CHECK (out[1].size() == input.channels[0].size());

    int directLatency = -1;
    const auto direct = runChainDirectly (input, values, 256, directLatency);
    CHECK (latency == directLatency);
    CHECK (latency == 192); // Balanced at 48 kHz (ProcessingChain.h)

    // Bit-exact: output frame i is chain output frame i + L.
    size_t mismatches = 0;
    for (size_t c = 0; c < 2; ++c)
        for (size_t i = 0; i < out[c].size(); ++i)
            mismatches += out[c][i] != direct[c][i + static_cast<size_t> (latency)] ? 1 : 0;
    CHECK (mismatches == 0);
    // ... and the processing really did something (boost 60 %).
    CHECK (maxAbsDiff (out[0], input.channels[0]) > 0.01);
}

TEST_CASE ("OfflineRenderer: bypassed modules render the input sample for sample, first and last frames included")
{
    // Impulses on the first and the very last input frame: the latency drop
    // keeps the first, the L-sample silence flush brings out the last.
    auto values = defaultValues();
    bypassAllModules (values);
    for (int profile = 0; profile < 3; ++profile)
    {
        values[static_cast<size_t> (LatencyProfile)] = static_cast<float> (profile);
        for (double rate : { 44100.0, 96000.0 })
        {
            auto input = makeProgramme (1, rate, 0.25, 0.25f, 3); // mono: duplicated to both output channels
            input.channels[0].front() = 0.9f;
            input.channels[0].back() = -0.9f;
            std::vector<std::vector<float>> out;
            int latency = 0;
            std::string error;
            REQUIRE (renderPass (input, values, 100, out, latency, error));
            CHECK (latency > 0);
            CHECK_LE (maxAbsDiff (out[0], input.channels[0]), 1e-6);
            CHECK_LE (maxAbsDiff (out[1], input.channels[0]), 1e-6);
            CHECK_NEAR (out[0].front(), 0.9, 1e-6);
            CHECK_NEAR (out[1].back(), -0.9, 1e-6);
        }
    }
}

TEST_CASE ("OfflineRenderer: the render is bit-identical at every block size")
{
    // Priming snaps every smoother onto its target, so with fixed parameters
    // (AutoLevel and AutoDrive off, their defaults) and the SafetyGovernor
    // idle (scale 1: it is updated once per block and applied from the next,
    // so once it moves the block size shifts it) --block changes only the
    // speed, never the samples.
    const auto input = makeProgramme (2, 48000.0, 1.5, 0.3f, 5);
    const auto values = boostedValues (60.0f);
    std::vector<std::vector<float>> ref;
    int latency = 0;
    std::string error;
    REQUIRE (renderPass (input, values, 512, ref, latency, error));
    for (int block : { 64, 333, 1000, 4096 })
    {
        std::vector<std::vector<float>> out;
        int l = 0;
        REQUIRE (renderPass (input, values, block, out, l, error));
        CHECK (l == latency);
        CHECK (out == ref); // bit-exact
    }
}

// ---------------------------------------------------------------------------
TEST_CASE ("OfflineRenderer: --target-lufs lands within 0.3 LU of the target, louder through max.drive, quieter through output.gain")
{
    RenderSettings settings;
    settings.targetLufs = -14.0f;
    settings.verifyCeilingDb = -1.0f;
    std::string error;

    // Louder: a quiet programme is driven into the maximizer.
    {
        const auto input = makeProgramme (2, 48000.0, 3.0, 0.08f, 21);
        const float inLufs = analyse (input.channels, input.sampleRate).integratedLufs;
        CHECK (inLufs < -22.0f);
        RenderResult rr;
        REQUIRE (renderFile (input, defaultValues(), settings, rr, error));
        CHECK (rr.targetReached);
        CHECK_NEAR (rr.outputReport.integratedLufs, -14.0, 0.3);
        CHECK (rr.passes >= 2 && rr.passes <= 1 + settings.maxIterations);
        CHECK (rr.driveDb > 3.0f);
        CHECK (rr.outputGainDb == 0.0f);
        CHECK_LE (rr.outputReport.truePeakDbtp, -1.0 + 0.1);
        CHECK_LE (rr.ceilingTrimDb, 0.0);
        // The report describes the delivered audio, which has the input's length.
        CHECK (rr.output.numChannels == 2 && rr.output.numFrames() == input.numFrames());
        CHECK (analyse (rr.output.channels, rr.output.sampleRate).integratedLufs == rr.outputReport.integratedLufs);
    }

    // Quieter: max.drive is already 0 dB, so output.gain comes down.
    {
        settings.targetLufs = -24.0f;
        const auto input = makeProgramme (2, 48000.0, 3.0, 0.5f, 22);
        CHECK (analyse (input.channels, input.sampleRate).integratedLufs > -18.0f);
        RenderResult rr;
        REQUIRE (renderFile (input, defaultValues(), settings, rr, error));
        CHECK (rr.targetReached);
        CHECK_NEAR (rr.outputReport.integratedLufs, -24.0, 0.3);
        CHECK (rr.driveDb == 0.0f);
        CHECK (rr.outputGainDb < -3.0f);
        CHECK (std::any_of (rr.notes.begin(), rr.notes.end(), [] (const std::string& n) { return contains (n, "output.gain lowered"); }));
    }
}

TEST_CASE ("OfflineRenderer: a quiet --target-lufs beyond output.gain's -24 dB is reached by lowering input.gain")
{
    // A hot programme (about -8 LUFS) to -45 LUFS needs more than output.gain's
    // 24 dB of attenuation; input.gain supplies the rest, with the maximizer on
    // (drive already 0 dB) and off.
    const auto input = makeProgramme (2, 48000.0, 3.0, 0.9f, 23);
    CHECK (analyse (input.channels, input.sampleRate).integratedLufs > -12.0f);
    RenderSettings settings;
    settings.targetLufs = -45.0f;
    std::string error;
    for (const bool maximizer : { true, false })
    {
        auto values = defaultValues();
        values[static_cast<size_t> (MaximizerOn)] = maximizer ? 1.0f : 0.0f;
        RenderResult rr;
        REQUIRE (renderFile (input, values, settings, rr, error));
        CHECK (rr.targetReached);
        CHECK_NEAR (rr.outputReport.integratedLufs, -45.0, 0.3);
        CHECK (rr.outputGainDb == layout()[static_cast<size_t> (OutputGainDb)].minValue);
        CHECK (rr.inputGainDb < -1.0f);
        CHECK (std::any_of (rr.notes.begin(), rr.notes.end(), [] (const std::string& n) { return contains (n, "input.gain lowered"); }));
        CHECK (std::none_of (rr.notes.begin(), rr.notes.end(), [] (const std::string& n) { return contains (n, "not reachable"); }));
    }
}

TEST_CASE ("OfflineRenderer: --target-lufs is skipped with a warning for bypass=on and for silence (one pass)")
{
    RenderSettings settings;
    settings.targetLufs = -14.0f;
    std::string error;

    auto values = defaultValues();
    values[static_cast<size_t> (BypassAll)] = 1.0f;
    RenderResult rr;
    REQUIRE (renderFile (makeProgramme (2, 48000.0, 1.0, 0.1f, 4), values, settings, rr, error));
    CHECK (rr.passes == 1);
    CHECK (! rr.targetReached);
    REQUIRE (! rr.notes.empty());
    CHECK (contains (rr.notes[0], "warning: loudness target skipped: bypass=on"));

    auto silence = makeProgramme (2, 48000.0, 1.0, 0.0f, 4);
    REQUIRE (renderFile (silence, defaultValues(), settings, rr, error));
    CHECK (rr.passes == 1);
    CHECK (! rr.targetReached);
    REQUIRE (! rr.notes.empty());
    CHECK (contains (rr.notes[0], "no measurable integrated loudness"));
}

// ---------------------------------------------------------------------------
TEST_CASE ("CLI process: writes float32 / PCM24 / PCM16 files that read back with that format, the input rate and length, in stereo")
{
    TempDir dir ("process_formats");
    const std::string in = dir.file ("in.wav");
    std::string error;
    REQUIRE (io::writeWav (in, makeProgramme (1, 44100.0, 1.0, 0.2f, 31), io::SampleFormat::Pcm16, error));

    // What `process --boost 40` renders, computed directly.
    CliOptions o;
    o.command = Command::Process;
    o.input = in;
    o.quiet = true;
    o.render.boostPercent = 40.0f;
    ResolvedParameters params;
    REQUIRE (buildParameters (o.render, params, error));
    io::AudioFileData input;
    REQUIRE (io::readWav (in, input, error));
    RenderResult expected;
    REQUIRE (renderFile (input, params.values, makeRenderSettings (o.render, params), expected, error));

    struct Case
    {
        io::SampleFormat format;
        const char* name;
        double lsb; // 1 LSB of the format (0: float, bit-exact)
    };
    for (const Case& fc : { Case { io::SampleFormat::Float32, "out-f32.wav", 0.0 }, Case { io::SampleFormat::Pcm24, "out-pcm24.wav", 1.0 / 8388608.0 },
                            Case { io::SampleFormat::Pcm16, "out-pcm16.wav", 1.0 / 32768.0 } })
    {
        o.output = dir.file (fc.name);
        o.render.format = fc.format;
        CHECK (runProcess (o) == kExitOk);

        io::AudioFileData written;
        REQUIRE (io::readWav (o.output, written, error));
        CHECK (written.sourceFormat == fc.format);
        CHECK (written.sampleRate == 44100.0);
        CHECK (written.numChannels == 2);
        CHECK (written.numFrames() == input.numFrames());
        REQUIRE (written.channels.size() == 2 && expected.output.channels.size() == 2);
        // Float32 is the render itself; PCM is it plus rounding and +-1 LSB TPDF dither (<= 1.5 LSB).
        const double worst = std::max (maxAbsDiff (written.channels[0], expected.output.channels[0]),
                                       maxAbsDiff (written.channels[1], expected.output.channels[1]));
        if (fc.lsb == 0.0)
            CHECK (worst == 0.0);
        else
            CHECK_LE (worst, 1.5 * fc.lsb + 1e-9);
    }

    // Exit codes: a missing input is a failure (1); writing over the input is a
    // usage error (2). Both print an expected "error: ..." line to stderr.
    o.input = dir.file ("missing.wav");
    o.output = dir.file ("x.wav");
    CHECK (runProcess (o) == kExitFailure);
    o.input = in;
    o.output = in;
    CHECK (runProcess (o) == kExitUsage);
    CHECK (! fs::exists (io::pathFromUtf8 (dir.file ("x.wav"))));
}

TEST_CASE ("CLI export: the output report measures the written PCM file (quantised and dithered), not the float render")
{
    // A -100 dBFS programme through bypassed modules: in PCM16 the dither
    // (about -96 dBFS rms) dominates, so the two readings differ clearly.
    TempDir dir ("export_report");
    auto values = defaultValues();
    bypassAllModules (values);
    const auto input = makeProgramme (2, 48000.0, 1.0, 2.0e-5f, 41);
    RenderResult rr;
    std::string error;
    REQUIRE (renderFile (input, values, RenderSettings(), rr, error));
    const LoudnessReport floatReport = rr.outputReport;

    RenderResult asPcm = rr;
    const std::string pcmPath = dir.file ("quiet-pcm16.wav");
    REQUIRE (writeRender (pcmPath, io::SampleFormat::Pcm16, asPcm, error));
    io::AudioFileData back;
    REQUIRE (io::readWav (pcmPath, back, error));
    const LoudnessReport fileReport = analyse (back.channels, back.sampleRate);
    CHECK (asPcm.outputReport.rmsDbfs == fileReport.rmsDbfs);
    CHECK (asPcm.outputReport.samplePeakDbfs == fileReport.samplePeakDbfs);
    CHECK (asPcm.outputReport.truePeakDbtp == fileReport.truePeakDbtp);
    CHECK (asPcm.outputReport.integratedLufs == fileReport.integratedLufs);
    CHECK (asPcm.outputReport.numFrames == input.numFrames());
    CHECK (asPcm.outputReport.rmsDbfs > floatReport.rmsDbfs + 3.0f);

    // Float32 files are the render bit for bit: the render's analysis stands.
    RenderResult asFloat = rr;
    const std::string floatPath = dir.file ("quiet-f32.wav");
    REQUIRE (writeRender (floatPath, io::SampleFormat::Float32, asFloat, error));
    REQUIRE (io::readWav (floatPath, back, error));
    CHECK (asFloat.outputReport.rmsDbfs == floatReport.rmsDbfs);
    CHECK (analyse (back.channels, back.sampleRate).rmsDbfs == floatReport.rmsDbfs);

    // A write failure is reported (a folder is not a file).
    RenderResult failing = rr;
    CHECK (! writeRender (io::pathToUtf8 (dir.path), io::SampleFormat::Pcm16, failing, error));
    CHECK (! error.empty());
}

// ---------------------------------------------------------------------------
namespace
{
// "ünï_日本_🎧.wav"
const std::string kUtf8Name = "\xC3\xBC\xC3\xB1\xC3\xAF_\xE6\x97\xA5\xE6\x9C\xAC_\xF0\x9F\x8E\xA7.wav";

std::set<std::string> displayNames (const std::vector<BatchJob>& jobs)
{
    std::set<std::string> s;
    for (const auto& j : jobs)
        s.insert (j.displayName);
    return s;
}
} // namespace

TEST_CASE ("CLI batch: the folder walk takes .wav / .wave files (UTF-8 names, sub-folders with --recursive) and skips the rest")
{
    TempDir dir ("batch_walk");
    const fs::path in = dir.path / "in", out = dir.path / "out";
    for (const std::string& f : std::vector<std::string> { "a.wav", "B.WAV", "bad.wav", "notes.txt", "sub/c.wave", "sub/readme.md", kUtf8Name })
    {
        fs::create_directories ((in / io::pathFromUtf8 (f)).parent_path());
        writeBytes (io::pathToUtf8 (in / io::pathFromUtf8 (f)), "not audio: the walk does not read files");
    }
    fs::create_directories (in / "folder.wav"); // a folder, not a file

    std::vector<BatchJob> jobs;
    std::vector<std::string> skipped;
    std::string error;
    REQUIRE (collectBatchJobs (in, out, false, jobs, skipped, error) == kExitOk);
    REQUIRE (jobs.size() == 4);
    CHECK (jobs[0].displayName == "B.WAV"); // sorted by name (bytes)
    CHECK (jobs[1].displayName == "a.wav");
    CHECK (jobs[2].displayName == "bad.wav");
    CHECK (jobs[3].displayName == kUtf8Name);
    CHECK (jobs[3].input == in / io::pathFromUtf8 (kUtf8Name));
    CHECK (jobs[3].output == out / io::pathFromUtf8 (kUtf8Name));
    CHECK (skipped == std::vector<std::string> { "notes.txt" });

    REQUIRE (collectBatchJobs (in, out, true, jobs, skipped, error) == kExitOk);
    CHECK ((displayNames (jobs) == std::set<std::string> { "B.WAV", "a.wav", "bad.wav", "sub/c.wave", kUtf8Name }));
    for (const auto& j : jobs)
        if (j.displayName == "sub/c.wave")
            CHECK (j.output == out / "sub" / "c.wave"); // sub-folder layout kept
    CHECK ((std::set<std::string> (skipped.begin(), skipped.end()) == std::set<std::string> { "notes.txt", "sub/readme.md" }));

    // An output folder inside the input folder: earlier results are never re-processed.
    const fs::path nested = in / "fx";
    fs::create_directories (nested);
    writeBytes (io::pathToUtf8 (nested / "a.wav"), "an earlier result");
    REQUIRE (collectBatchJobs (in, nested, true, jobs, skipped, error) == kExitOk);
    CHECK (jobs.size() == 5);
    CHECK (displayNames (jobs).count ("fx/a.wav") == 0);

    // An output folder above the input folder that would overwrite an input: usage error.
    const fs::path upper = dir.path / "A", lower = upper / "B";
    fs::create_directories (lower / "B");
    writeBytes (io::pathToUtf8 (lower / "x.wav"), "x");
    writeBytes (io::pathToUtf8 (lower / "B" / "x.wav"), "x");
    CHECK (collectBatchJobs (lower, upper, true, jobs, skipped, error) == kExitUsage);
    CHECK (contains (error, "would overwrite an input file"));
    CHECK (collectBatchJobs (lower, upper, false, jobs, skipped, error) == kExitOk);

    CHECK (batchWorkerCount (2, 8) == 2);
    CHECK (batchWorkerCount (9, 3) == 3);
    CHECK (batchWorkerCount (9, 0) >= 1);
    CHECK (batchWorkerCount (9, 0) <= 9);
}

TEST_CASE ("CLI batch: parallel jobs give per-file results at the loudness target; a corrupt WAV fails without stopping the others")
{
    TempDir dir ("batch_run");
    const fs::path in = dir.path / "in", out = dir.path / "out";
    fs::create_directories (in / "sub");
    struct Source
    {
        std::string name;
        io::AudioFileData audio;
        io::SampleFormat format;
    };
    const std::vector<Source> sources { { "a.wav", makeProgramme (2, 48000.0, 1.2, 0.1f, 51), io::SampleFormat::Float32 },
                                        { "b.wav", makeProgramme (1, 44100.0, 1.0, 0.05f, 52), io::SampleFormat::Pcm16 },
                                        { "sub/c.wav", makeProgramme (2, 32000.0, 0.9, 0.3f, 53), io::SampleFormat::Pcm24 },
                                        { kUtf8Name, makeProgramme (2, 48000.0, 1.1, 0.2f, 54), io::SampleFormat::Pcm16 } };
    std::string error;
    for (const auto& s : sources)
        REQUIRE (io::writeWav (io::pathToUtf8 (in / io::pathFromUtf8 (s.name)), s.audio, s.format, error));
    writeBytes (io::pathToUtf8 (in / "bad.wav"), std::string ("RIFF\x24\x00\x00\x00WAVEfmt ", 16) + "truncated");
    writeBytes (io::pathToUtf8 (in / "notes.txt"), "not audio");

    std::vector<BatchJob> jobs;
    std::vector<std::string> skipped;
    REQUIRE (collectBatchJobs (in, out, true, jobs, skipped, error) == kExitOk);
    REQUIRE (jobs.size() == 5);

    CliOptions o;
    o.command = Command::Batch;
    o.render.targetLufs = -16.0f;
    o.render.format = io::SampleFormat::Pcm24;
    ResolvedParameters params;
    REQUIRE (buildParameters (o.render, params, error));
    const RenderSettings settings = makeRenderSettings (o.render, params);
    const size_t numWorkers = batchWorkerCount (jobs.size(), 3);
    REQUIRE (numWorkers == 3);

    // Proof of parallelism: the first finished job waits until a second one
    // finishes on another worker (a serial runner would stall here until the
    // hang guard gives up and the test fails).
    std::mutex mutex;
    std::condition_variable cv;
    std::set<std::thread::id> threads;
    std::vector<size_t> finishedOrder;
    bool overlapSeen = false, stalled = false;
    std::vector<std::string> notes;
    std::vector<BatchResult> results;
    runBatchJobs (jobs, params.values, settings, o.render.format, numWorkers, results,
                  [&] (size_t index) {
                      std::unique_lock<std::mutex> lock (mutex);
                      threads.insert (std::this_thread::get_id());
                      finishedOrder.push_back (index);
                      cv.notify_all();
                      if (finishedOrder.size() == 1 && ! cv.wait_for (lock, std::chrono::seconds (60), [&] { return finishedOrder.size() >= 2; }))
                          stalled = true;
                      if (finishedOrder.size() >= 2 && threads.size() >= 2)
                          overlapSeen = true;
                  },
                  [&] (const std::string& note) { notes.push_back (note); });

    CHECK (! stalled);
    CHECK (overlapSeen);
    CHECK (threads.size() >= 2);
    CHECK (notes.empty());
    CHECK (std::set<size_t> (finishedOrder.begin(), finishedOrder.end()).size() == jobs.size());
    REQUIRE (results.size() == jobs.size());

    size_t okCount = 0;
    for (size_t i = 0; i < jobs.size(); ++i)
    {
        const auto& job = jobs[i];
        const auto& r = results[i];
        if (job.displayName == "bad.wav")
        {
            CHECK (! r.ok);
            CHECK (! r.error.empty());
            CHECK (! fs::exists (job.output));
            continue;
        }
        const auto source = std::find_if (sources.begin(), sources.end(), [&] (const Source& s) { return s.name == job.displayName; });
        REQUIRE (source != sources.end());
        CHECK (r.ok);
        CHECK (r.error.empty());
        okCount += r.ok ? 1 : 0;
        // results[i] belongs to jobs[i].
        CHECK (r.inReport.numFrames == source->audio.numFrames());
        CHECK (r.inFormat == sampleFormatName (source->format));
        CHECK (r.render.targetReached);
        CHECK_NEAR (r.outReport.integratedLufs, -16.0, 0.3);
        CHECK_LE (r.outReport.truePeakDbtp, -1.0 + 0.1);
        CHECK (r.render.output.channels.empty()); // audio released after writing

        io::AudioFileData written;
        REQUIRE (io::readWav (io::pathToUtf8 (job.output), written, error));
        CHECK (written.sourceFormat == io::SampleFormat::Pcm24);
        CHECK (written.numChannels == 2);
        CHECK (written.sampleRate == source->audio.sampleRate);
        CHECK (written.numFrames() == source->audio.numFrames());
        const LoudnessReport fileReport = analyse (written.channels, written.sampleRate);
        CHECK (r.outReport.integratedLufs == fileReport.integratedLufs); // the report is the written file's
        CHECK (r.outReport.truePeakDbtp == fileReport.truePeakDbtp);
    }
    CHECK (okCount == 4);
    CHECK (fs::exists (out / "sub" / "c.wav"));
    CHECK (! fs::exists (out / "notes.txt"));

    // A job on a worker thread renders exactly what a lone render of that file does.
    {
        const auto& a = sources[0];
        RenderResult alone;
        REQUIRE (renderFile (a.audio, params.values, settings, alone, error));
        const std::string path = io::pathToUtf8 (dir.path / "a-alone.wav");
        REQUIRE (writeRender (path, io::SampleFormat::Pcm24, alone, error));
        io::AudioFileData x, y;
        REQUIRE (io::readWav (path, x, error));
        REQUIRE (io::readWav (io::pathToUtf8 (out / "a.wav"), y, error));
        CHECK (x.channels == y.channels);
    }

    // Per-file reporting: the summary table and the --json document.
    const std::string table = formatBatchSummary (jobs, results, skipped, 1.0);
    CHECK (contains (table, "4 ok, 1 failed, 1 skipped (non-WAV)"));
    CHECK (contains (table, "FAILED: "));
    CHECK (contains (table, "Skipped : notes.txt"));
    CHECK (contains (table, "sub/c.wav"));
    CHECK (contains (table, "-16.0")); // Out LUFS column

    const json::Value doc = batchResultsToJson (jobs, results, skipped, numWorkers, 1.0, o.render, params);
    const auto& files = doc["files"].asArray();
    REQUIRE (files.size() == jobs.size());
    for (size_t i = 0; i < files.size(); ++i)
    {
        const bool bad = jobs[i].displayName == "bad.wav";
        CHECK (files[i]["status"].asString() == (bad ? "failed" : "ok"));
        CHECK (files[i]["file"].asString() == io::pathToUtf8 (jobs[i].input));
        CHECK (files[i]["error"].isNull() != bad);
        if (! bad)
        {
            CHECK_NEAR (files[i]["output"]["integratedLufs"].asNumber(), -16.0, 0.35);
            CHECK (files[i]["output"]["sourceFormat"].asString() == "pcm24");
            CHECK (files[i]["render"]["targetReached"].asBool());
        }
    }
    CHECK (doc["summary"]["ok"].asNumber() == 4.0);
    CHECK (doc["summary"]["failed"].asNumber() == 1.0);
    CHECK (doc["summary"]["skipped"].asNumber() == 1.0);
    CHECK (doc["summary"]["jobs"].asNumber() == 3.0);
    REQUIRE (doc["skipped"].asArray().size() == 1);
    CHECK (doc["skipped"].asArray()[0].asString() == "notes.txt");
}

namespace
{
/** Strict UTF-8 check (well-formed lead / continuation structure). */
bool isValidUtf8 (const std::string& s)
{
    for (size_t i = 0; i < s.size();)
    {
        const auto c = static_cast<unsigned char> (s[i]);
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + len > s.size())
            return false;
        for (size_t k = 1; k < len; ++k)
            if ((static_cast<unsigned char> (s[i + k]) & 0xC0) != 0x80)
                return false;
        i += len;
    }
    return true;
}

size_t codePoints (const std::string& s)
{
    return static_cast<size_t> (std::count_if (s.begin(), s.end(), [] (char c) { return (static_cast<unsigned char> (c) & 0xC0) != 0x80; }));
}
} // namespace

TEST_CASE ("CLI batch: the summary table shortens long UTF-8 names on a code point boundary and keeps the columns aligned")
{
    // "日本語の曲名" (6 code points, 18 bytes) x 4 + "a.wav": 29 code points,
    // 77 bytes. A byte-based cut to 48 columns would start inside a 3-byte
    // sequence (45 bytes kept, 40 of them CJK) and misalign the columns.
    const std::string cjk = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE6\x9B\xB2\xE5\x90\x8D";
    std::string longName;
    for (int i = 0; i < 4; ++i)
        longName += cjk;
    longName += "a.wav";
    const std::string veryLong = "sub/" + longName + longName; // 62 code points: ellipsized

    std::vector<BatchJob> jobs (3);
    jobs[0].displayName = longName;
    jobs[1].displayName = veryLong;
    jobs[2].displayName = "short.wav";
    std::vector<BatchResult> results (3);
    for (auto& r : results)
        r.error = "boom";

    const std::string table = formatBatchSummary (jobs, results, {}, 1.0);
    CHECK (isValidUtf8 (table));
    CHECK (contains (table, longName)); // 29 code points fit the column uncut
    CHECK (contains (table, "..." + cjk.substr (3) + cjk + "a.wav" + longName)); // the last 45 code points of veryLong

    // "Status" in the header and "FAILED" in every row start in the same column.
    std::vector<std::string> lines;
    for (size_t pos = 0, next; pos < table.size(); pos = next + 1)
    {
        next = table.find ('\n', pos);
        if (next == std::string::npos)
            next = table.size();
        lines.push_back (table.substr (pos, next - pos));
    }
    const auto header = std::find_if (lines.begin(), lines.end(), [] (const std::string& l) { return contains (l, "Status"); });
    REQUIRE (header != lines.end());
    const size_t statusColumn = codePoints (header->substr (0, header->find ("Status")));
    CHECK (statusColumn == 48 + 55); // the name column is 48 wide
    int rows = 0;
    for (const auto& l : lines)
        if (contains (l, "FAILED: boom"))
        {
            ++rows;
            CHECK (codePoints (l.substr (0, l.find ("FAILED"))) == statusColumn);
        }
    CHECK (rows == 3);
}

TEST_CASE ("CLI batch: the command writes the folder tree and exits 1 when a file failed, 2 on usage errors")
{
    TempDir dir ("batch_command");
    const fs::path in = dir.path / "in", out = dir.path / "out";
    fs::create_directories (in / "sub");
    std::string error;
    REQUIRE (io::writeWav (io::pathToUtf8 (in / "a.wav"), makeProgramme (2, 48000.0, 0.5, 0.2f, 61), io::SampleFormat::Float32, error));
    REQUIRE (io::writeWav (io::pathToUtf8 (in / "sub" / "b.wav"), makeProgramme (1, 48000.0, 0.5, 0.2f, 62), io::SampleFormat::Pcm16, error));
    REQUIRE (io::writeWav (io::pathToUtf8 (in / io::pathFromUtf8 (kUtf8Name)), makeProgramme (2, 48000.0, 0.5, 0.2f, 63), io::SampleFormat::Pcm24, error));
    writeBytes (io::pathToUtf8 (in / "bad.wav"), "RIFF");
    writeBytes (io::pathToUtf8 (in / "notes.txt"), "not audio");

    CliOptions o;
    o.command = Command::Batch;
    o.input = io::pathToUtf8 (in);
    o.output = io::pathToUtf8 (out);
    o.recursive = true;
    o.jobs = 2;
    o.quiet = true;
    o.render.format = io::SampleFormat::Pcm16;
    CHECK (runBatch (o) == kExitFailure); // bad.wav failed, the rest were written
    for (const fs::path& p : { out / "a.wav", out / "sub" / "b.wav", out / io::pathFromUtf8 (kUtf8Name) })
    {
        io::AudioFileData d;
        CHECK (io::readWav (io::pathToUtf8 (p), d, error));
        CHECK (d.sourceFormat == io::SampleFormat::Pcm16);
        CHECK (d.numChannels == 2);
        CHECK (d.numFrames() == 24000);
    }
    CHECK (! fs::exists (out / "bad.wav"));
    CHECK (! fs::exists (out / "notes.txt"));

    fs::remove (in / "bad.wav");
    CHECK (runBatch (o) == kExitOk);

    // Usage errors (each prints an expected "error: ..." line to stderr).
    o.output = o.input;
    CHECK (runBatch (o) == kExitUsage); // output folder == input folder
    o.input = io::pathToUtf8 (dir.path / "missing");
    o.output = io::pathToUtf8 (out);
    CHECK (runBatch (o) == kExitUsage); // no input folder
}
