// Flubsound Pro - ExportJob: the desktop app's "Export / batch process"
// engine (roadmap 2.10). The ExportDialog is only its UI.
//
// What it does, per job:
//   * Inputs: files and / or folders (+ sub-folders with `recursive`). Every
//     file whose extension one of juce::AudioFormatManager's basic formats
//     claims (WAV, AIFF, FLAC, Ogg Vorbis, and MP3 when JUCE_USE_MP3AUDIOFORMAT
//     is on - JUCE 9's default, which the app keeps) is a job item; other
//     files are listed as skipped. A folder scan leaves out hidden files and
//     names starting with '.' (macOS "._x.wav" AppleDouble files). Items are
//     sorted by display name (the path relative to the input folder) and
//     mapped to that relative path below the output folder with the output
//     format's extension. On a clash (a.wav and a.flac -> a.wav) the file
//     that already has the output extension keeps the name and the other
//     gets its source extension appended ("a_flac.wav").
//   * Never overwrites an input: the output folder must not be an input
//     folder or the folder of an input file (validate()), and an output path
//     that is also an input (an output folder above a recursive input folder)
//     refuses the whole job. Files inside an output folder nested in an input
//     folder (lexically or through a symlink / "..") are ignored (never
//     re-process our own results), like the CLI.
//   * Decoding: juce::AudioFormatReader, at the file's own sample rate (no
//     resampling, as in the CLI); 1, 2, 6 or 8 channels (flub::cli::checkRenderable).
//   * Rendering: flub::cli::renderFile (tools/flubsound-cli/OfflineRenderer),
//     the CLI's code - parameter priming, latency compensation (the output has
//     the input's length), the loudness-target loop and the ceiling hold - on
//     a private ParameterStore + ProcessingChain filled from `values` (a
//     snapshot of a strip or a preset: the live engine is never touched).
//   * Writing: WAV float32 / PCM24 / PCM16 through flub::cli::writeRender (the
//     CLI's writer: the same TPDF dither, so a WAV is byte-identical to
//     `flubsound-cli process` for the same decoded input and parameters);
//     FLAC 16 / 24 through juce::FlacAudioFormat with the same TPDF dither
//     generator. Files are written to a temporary file next to the target
//     and moved into place when complete; a write failure (also one while
//     the FLAC encoder finishes the stream) fails the item. The output
//     report measures the file as written (PCM / FLAC read back).
//
// Threading: start() is called on the message thread and returns at once;
// one juce::Thread works through the items in order. Progress and results
// are read with getProgress() / getItems() (copies taken under a lock; never
// the audio thread) and every change is announced through the
// ChangeBroadcaster (asynchronously, on the message thread). cancel() stops
// after the file being rendered (it is completed and written; the remaining
// items become Cancelled). abort() - and the destructor, which then waits
// for the thread - also abandons the current item between two blocks or
// stages (decode, render through RenderSettings::abort, encode, read-back);
// a single loudness analysis still runs to its end. waitForCompletion() is
// the explicit completion signal for tests and headless callers.
#pragma once

#include "OfflineRenderer.h"

#include "flub/engine/Parameters.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace flub::app
{
class PresetManager;
struct PresetInfo;

enum class ExportFormat
{
    WavFloat32,
    WavPcm24,
    WavPcm16,
    Flac24,
    Flac16
};

struct ExportSettings
{
    juce::Array<juce::File> inputs; // files and / or folders
    bool recursive = false;         // folders: include sub-folders
    juce::File outputFolder;        // created when missing
    ExportFormat format = ExportFormat::WavFloat32;
    std::optional<float> targetLufs; // -60 .. -1 LUFS (the CLI's --target-lufs range)
    std::optional<float> ceilingDb;  // true-peak ceiling (the max.ceiling range, -12 .. 0 dBTP)
    std::vector<float> values;       // param::kNumParams base values (snapshotStrip / presetValues)
    bool smartMacros = false;        // Smart macros (docs/11 E34): the preset's "smart" flag or the strip's switch
    int blockSize = 512;
};

struct ExportItem
{
    enum class Status
    {
        Queued,
        Running,
        Done,
        Failed,
        Skipped,  // not an audio file
        Cancelled // not started: the job was cancelled first
    };

    juce::File input, output;  // output: empty for skipped items
    juce::String displayName;  // path relative to its input folder ('/' separators)
    Status status = Status::Queued;
    juce::String error;        // Failed: why; Skipped: the reason
    juce::StringArray notes;   // renderer notes (loudness loop, ceiling trim)
    juce::String inputFormat;  // "WAV 24-bit, 44100 Hz, 1 ch"
    double sampleRate = 0.0;
    int numChannels = 0;
    juce::int64 numFrames = 0;
    float inLufs = -160.0f, inTruePeakDbtp = -160.0f;   // decoded input
    float outLufs = -160.0f, outTruePeakDbtp = -160.0f; // the file as written
    int passes = 0;            // renderer passes (loudness loop)
};

class ExportJob final : public juce::ChangeBroadcaster, private juce::Thread
{
public:
    struct Progress
    {
        int total = 0;       // items to render (skipped files excluded)
        int finished = 0;    // Done + Failed
        int failed = 0;
        int skipped = 0;
        int current = -1;    // index into getItems() of the file being rendered, -1 when none
        bool running = false, completed = false, cancelled = false;
        juce::String error;  // the whole job was refused (e.g. an output would overwrite an input)
        juce::StringArray notes; // parameter changes the target / ceiling needed (applyTargetRules)
    };

    ExportJob();
    ~ExportJob() override;

    /** Checks the settings without touching the disk beyond existence tests:
        inputs present, an output folder that is not a file, not an input
        folder and not the folder of an input file, a full parameter table,
        target / ceiling in range. Returns false with a user-facing message. */
    static bool validate (const ExportSettings& settings, juce::String& error);

    /** The item list for these settings (folder walk, skipped files, output
        names). Returns false with a message when an output path would be an
        input. Runs on the worker; public for tests. */
    static bool plan (const ExportSettings& settings, const juce::AudioFormatManager& formats, std::vector<ExportItem>& items,
                      juce::String& error);

    /** Starts the job (validate() first). False with `error` when the
        settings are refused or a job is still running. */
    bool start (ExportSettings settings, juce::String& error);

    /** Stop after the file being rendered. Thread-safe, idempotent. */
    void cancel() noexcept { cancelRequested.store (true); }
    /** Stop now: cancel() plus the item in progress is abandoned at its next
        block or stage (decode, render, encode, read-back; a loudness analysis
        already running finishes first). That item fails as "Aborted" and
        nothing is written under its name. The destructor does this.
        Thread-safe, idempotent. */
    void abort() noexcept
    {
        cancelRequested.store (true);
        abortRequested.store (true);
    }

    /** False from the moment the final progress (completed) is published. */
    bool isRunning() const noexcept { return running.load(); }
    /** Blocks until the job has finished (or `timeoutMs` passed, a hang
        guard; -1 = forever). True if it finished. */
    bool waitForCompletion (int timeoutMs) const { return finishedEvent.wait (timeoutMs); }

    Progress getProgress() const;
    std::vector<ExportItem> getItems() const;

    /** Test / diagnostics hooks, called on the WORKER thread right before an
        item is rendered and right after its result is stored. Set before start(). */
    std::function<void (int itemIndex)> onItemStarted, onItemFinished;

    // ---- Building blocks (public for tests) ----------------------------------
    /** The active bank of a strip's store, as a render's base values. "Bypass
        All" is application state (master enable), not the strip's sound: it
        is set off, like the CLI does for presets. */
    static std::vector<float> snapshotStrip (const flub::param::ParameterStore& store);
    /** A preset's values (the app's PresetManager rules; Bypass All off);
        `smart` (optional) receives its "smart" flag (docs/11 E34). */
    static bool presetValues (const PresetManager& presets, const PresetInfo& preset, std::vector<float>& values, juce::String& error,
                              bool* smart = nullptr);
    /** The CLI's rules for --ceiling / --target-lufs (CliOptions.cpp
        buildParameters): the ceiling goes into max.ceiling, the maximizer is
        switched on when a ceiling or target needs it, and max.autoDrive is
        switched off under a loudness target (the offline loop replaces it).
        Adds a note for each change. */
    static void applyTargetRules (std::vector<float>& values, std::optional<float> targetLufs, std::optional<float> ceilingDb,
                                  juce::StringArray& notes);
    /** The renderer settings `flubsound-cli process` would use (makeRenderSettings). */
    static flub::cli::RenderSettings makeRenderSettings (const ExportSettings& settings, const std::vector<float>& values);

    /** Decodes a whole file to planar float (any registered format). A
        non-null `abortFlag` is polled per block: set, it fails as "Aborted". */
    static bool decode (const juce::AudioFormatManager& formats, const juce::File& file, flub::io::AudioFileData& out,
                        juce::String& description, juce::String& error, const std::atomic<bool>* abortFlag = nullptr);
    /** Writes a render in `format` to `file` (through a temporary file) and
        sets result.outputReport to the analysis of the file as written.
        `abortFlag` is polled before, per FLAC block, during the read-back and
        before the file is moved into place ("Aborted": nothing is written). */
    static bool encode (const juce::AudioFormatManager& formats, const juce::File& file, ExportFormat format,
                        flub::cli::RenderResult& result, juce::String& error, const std::atomic<bool>* abortFlag = nullptr);
    /** encode()'s FLAC writer: `audio` (stereo) as dithered 16 / 24-bit FLAC
        into `stream`. Fails on any stream failure, including the ones JUCE's
        FlacWriter ignores while it finishes the stream (last frame, STREAMINFO,
        final flush), and with "Aborted" when `abortFlag` is set. */
    static bool writeFlac (std::unique_ptr<juce::OutputStream> stream, int bitsPerSample, const flub::io::AudioFileData& audio,
                           juce::String& error, const std::atomic<bool>* abortFlag = nullptr);

    static juce::String extensionFor (ExportFormat format);      // ".wav" / ".flac"
    static juce::String describeFormat (ExportFormat format);    // "WAV 32-bit float", ...
    /** "*.wav;*.aiff;..." for a file chooser (the registered formats). */
    static juce::String inputWildcard (const juce::AudioFormatManager& formats);
    static juce::String statusText (ExportItem::Status status);

    const juce::AudioFormatManager& getFormatManager() const noexcept { return formats; }

private:
    void run() override;
    void processItem (size_t index, const std::vector<float>& values, const flub::cli::RenderSettings& renderSettings);
    void publish (const std::function<void()>& change);

    juce::AudioFormatManager formats; // registerBasicFormats(); used by the worker while running
    ExportSettings settings;          // written by start() before the thread runs

    mutable juce::CriticalSection lock; // guards items and progress
    std::vector<ExportItem> items;
    Progress progress;

    std::atomic<bool> cancelRequested { false }, abortRequested { false }, running { false };
    juce::WaitableEvent finishedEvent { true }; // manual reset: stays signalled until the next start()

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExportJob)
};
} // namespace flub::app
