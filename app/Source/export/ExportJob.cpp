#include "ExportJob.h"

#include "presets/PresetManager.h"

#include "flub/io/FilePath.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <set>

namespace flub::app
{
using namespace flub::param;

namespace
{
constexpr int kDecodeBlock = 65536; // frames per reader call
constexpr int kEncodeBlock = 4096;  // frames per FLAC writer call
constexpr int kFlacQuality = 5;     // FlacAudioFormat's "5 (Default)" compression level

float& valueOf (std::vector<float>& values, int id) { return values[static_cast<size_t> (id)]; }

/** Symlink- and ".."-resolved path for "is this output one of the inputs?"
    (the CLI's collectBatchJobs uses the same comparison), case-folded where
    file names are not case-sensitive (Windows; macOS by default), so
    "Song.WAV" and "song.wav" name the same file there. */
juce::String canonical (const juce::File& f)
{
    std::error_code ec;
    const auto p = flub::io::pathFromUtf8 (f.getFullPathName().toStdString());
    const auto c = std::filesystem::weakly_canonical (p, ec);
    const auto key = juce::String::fromUTF8 (flub::io::pathToUtf8 (ec ? p.lexically_normal() : c).c_str());
    return juce::File::areFileNamesCaseSensitive() ? key : key.toLowerCase();
}

bool sameFolder (const juce::File& a, const juce::File& b)
{
    return a == b || canonical (a) == canonical (b);
}

/** canonical() of a folder with a trailing separator: "is below" by prefix. */
juce::String canonicalFolder (const juce::File& folder)
{
    const auto sep = juce::File::getSeparatorString();
    const auto key = canonical (folder);
    return key.endsWith (sep) ? key : key + sep;
}

constexpr const char* kAborted = "Aborted"; // an item stopped through abort()

bool isAborted (const std::atomic<bool>* flag) noexcept
{
    return flag != nullptr && flag->load (std::memory_order_relaxed);
}

/** The writer's stream for writeFlac(): forwards everything to `inner` and
    records any failure in `failed`, which outlives the writer. JUCE's
    FlacWriter ignores the results of its last writes (the final frame and
    the STREAMINFO rewrite in FLAC__stream_encoder_finish) and of its final
    flush(), and deletes the stream with itself. */
class FailureTrackingStream final : public juce::OutputStream
{
public:
    FailureTrackingStream (std::unique_ptr<juce::OutputStream> innerStream, bool& failedFlag)
        : inner (std::move (innerStream)), failed (failedFlag)
    {
    }

    ~FailureTrackingStream() override { flush(); } // the inner stream's own destructor cannot report

    void flush() override
    {
        inner->flush();
        if (auto* file = dynamic_cast<juce::FileOutputStream*> (inner.get()); file != nullptr && file->getStatus().failed())
            failed = true;
    }
    bool setPosition (juce::int64 pos) override { return record (inner->setPosition (pos)); }
    juce::int64 getPosition() override { return inner->getPosition(); }
    bool write (const void* data, size_t numBytes) override { return record (inner->write (data, numBytes)); }
    bool writeRepeatedByte (juce::uint8 byte, size_t numTimesToRepeat) override
    {
        return record (inner->writeRepeatedByte (byte, numTimesToRepeat));
    }

private:
    bool record (bool ok) noexcept
    {
        if (! ok)
            failed = true;
        return ok;
    }

    std::unique_ptr<juce::OutputStream> inner;
    bool& failed;
};

/** The TPDF dither of flub::io::writeWav (core/src/io/WavFile.cpp): splitmix64
    with the same fixed seed, two uniforms per sample, +-1 LSB triangular.
    FLAC exports are dithered exactly like the WAV writer's PCM formats. */
class TpdfDither
{
public:
    double next() noexcept
    {
        state += 0x9E3779B97F4A7C15ull;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        constexpr double scale = 1.0 / 4294967296.0;
        return static_cast<double> (z >> 32) * scale - static_cast<double> (z & 0xFFFFFFFFull) * scale;
    }

private:
    uint64_t state = 0x2545F4914F6CDD1Dull;
};

int quantise (float x, double scale, TpdfDither& dither) noexcept
{
    double v = static_cast<double> (x);
    if (std::isnan (v))
        v = 0.0;
    const double q = std::floor (v * scale + dither.next() + 0.5);
    return static_cast<int> (std::clamp (q, -scale, scale - 1.0));
}

juce::String toJuce (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }
} // namespace

// =============================================================================
// Settings, parameters, formats
// =============================================================================
ExportJob::ExportJob()
    : juce::Thread ("Flubsound export")
{
    formats.registerBasicFormats();
    finishedEvent.signal(); // nothing to wait for until start()
}

ExportJob::~ExportJob()
{
    // Quitting / closing the dialog must not wait for a long file: abort the
    // item between blocks or stages - decode, render, encode, read-back (one
    // loudness analysis still runs to its end) - and never move its partial
    // file into place.
    abort();
    stopThread (-1);
}

juce::String ExportJob::extensionFor (ExportFormat format)
{
    return format == ExportFormat::Flac16 || format == ExportFormat::Flac24 ? ".flac" : ".wav";
}

juce::String ExportJob::describeFormat (ExportFormat format)
{
    switch (format)
    {
        case ExportFormat::WavFloat32: return "WAV 32-bit float";
        case ExportFormat::WavPcm24: return "WAV 24-bit PCM";
        case ExportFormat::WavPcm16: return "WAV 16-bit PCM";
        case ExportFormat::Flac24: return "FLAC 24-bit";
        case ExportFormat::Flac16: return "FLAC 16-bit";
    }
    return {};
}

juce::String ExportJob::inputWildcard (const juce::AudioFormatManager& manager)
{
    return manager.getWildcardForAllFormats();
}

juce::String ExportJob::statusText (ExportItem::Status status)
{
    switch (status)
    {
        case ExportItem::Status::Queued: return "Queued";
        case ExportItem::Status::Running: return "Rendering";
        case ExportItem::Status::Done: return "Done";
        case ExportItem::Status::Failed: return "Failed";
        case ExportItem::Status::Skipped: return "Skipped";
        case ExportItem::Status::Cancelled: return "Cancelled";
    }
    return {};
}

std::vector<float> ExportJob::snapshotStrip (const ParameterStore& store)
{
    std::vector<float> values (static_cast<size_t> (kNumParams));
    store.snapshot (values.data());
    valueOf (values, BypassAll) = layout()[static_cast<size_t> (BypassAll)].defaultValue;
    return values;
}

bool ExportJob::presetValues (const PresetManager& presets, const PresetInfo& preset, std::vector<float>& values, juce::String& error,
                              bool* smart)
{
    // A fresh store (defaults, Bypass All off) receives the preset through the
    // PresetManager's own rules (loadIntoBank keeps the bank's bypass value).
    auto store = std::make_unique<ParameterStore>();
    if (! presets.loadIntoBank (preset, *store, Bank::A, error, smart))
        return false;
    store->setActiveBank (Bank::A);
    values = snapshotStrip (*store);
    return true;
}

void ExportJob::applyTargetRules (std::vector<float>& values, std::optional<float> targetLufs, std::optional<float> ceilingDb,
                                  juce::StringArray& notes)
{
    if (values.size() != static_cast<size_t> (kNumParams))
        return;
    auto set = [&values] (int id, float v) { valueOf (values, id) = layout()[static_cast<size_t> (id)].clamp (v); };

    if (ceilingDb)
        set (MaxCeilingDb, *ceilingDb);
    if ((ceilingDb || targetLufs) && valueOf (values, MaximizerOn) < 0.5f)
    {
        set (MaximizerOn, 1.0f);
        notes.add ("Loudness maximizer enabled (required for a ceiling / loudness target)");
    }
    if (targetLufs && valueOf (values, MaxAutoDrive) >= 0.5f)
    {
        set (MaxAutoDrive, 0.0f);
        notes.add ("Maximizer loudness target (auto drive) off: the export sets the drive offline");
    }
}

flub::cli::RenderSettings ExportJob::makeRenderSettings (const ExportSettings& s, const std::vector<float>& values)
{
    flub::cli::RenderSettings rs;
    rs.blockSize = s.blockSize;
    rs.targetLufs = s.targetLufs;
    rs.smartMacros = s.smartMacros;
    if ((s.ceilingDb || s.targetLufs) && values.size() == static_cast<size_t> (kNumParams))
        rs.verifyCeilingDb = values[static_cast<size_t> (MaxCeilingDb)];
    return rs;
}

bool ExportJob::validate (const ExportSettings& s, juce::String& error)
{
    if (s.inputs.isEmpty())
    {
        error = "Add input files or a folder.";
        return false;
    }
    for (const auto& in : s.inputs)
        if (! in.exists())
        {
            error = "Input not found: " + in.getFullPathName();
            return false;
        }
    if (s.outputFolder == juce::File())
    {
        error = "Choose an output folder.";
        return false;
    }
    if (s.outputFolder.existsAsFile())
    {
        error = "The output folder is a file: " + s.outputFolder.getFullPathName();
        return false;
    }
    for (const auto& in : s.inputs)
    {
        const auto folder = in.isDirectory() ? in : in.getParentDirectory();
        if (sameFolder (folder, s.outputFolder))
        {
            error = "The output folder is " + juce::String (in.isDirectory() ? "an input folder" : "the folder of an input file")
                    + ". Choose another folder: exports never go next to their sources.";
            return false;
        }
    }
    if (s.values.size() != static_cast<size_t> (kNumParams))
    {
        error = "Internal error: the parameter table has the wrong size.";
        return false;
    }
    if (s.targetLufs && ! (*s.targetLufs >= -60.0f && *s.targetLufs <= -1.0f))
    {
        error = "The loudness target must be between -60 and -1 LUFS.";
        return false;
    }
    const auto& ceiling = layout()[static_cast<size_t> (MaxCeilingDb)];
    if (s.ceilingDb && ! (*s.ceilingDb >= ceiling.minValue && *s.ceilingDb <= ceiling.maxValue))
    {
        error = "The ceiling must be between " + juce::String (ceiling.minValue, 0) + " and " + juce::String (ceiling.maxValue, 0) + " dBTP.";
        return false;
    }
    return true;
}

bool ExportJob::plan (const ExportSettings& s, const juce::AudioFormatManager& manager, std::vector<ExportItem>& out, juce::String& error)
{
    out.clear();

    struct Candidate
    {
        juce::File file;
        juce::String displayName;
    };
    std::vector<Candidate> candidates;
    std::set<juce::String> seen; // full paths: an input added twice is rendered once

    auto add = [&] (const juce::File& file, const juce::String& displayName)
    {
        if (seen.insert (file.getFullPathName()).second)
            candidates.push_back ({ file, displayName.replaceCharacter ('\\', '/') });
    };

    const auto outputKey = canonicalFolder (s.outputFolder);
    for (const auto& in : s.inputs)
    {
        if (in.isDirectory())
        {
            // Only an output folder nested in the input folder holds files the
            // scan would pick up (one above the input folder contains every
            // input, so it must not be used as an exclusion). Lexically or
            // through a symlink / ".." (canonical paths).
            const bool outputNested = s.outputFolder.isAChildOf (in) || outputKey.startsWith (canonicalFolder (in));
            const int whatToFind = juce::File::findFiles | juce::File::ignoreHiddenFiles;
            for (const auto& f : in.findChildFiles (whatToFind, s.recursive, "*", juce::File::FollowSymlinks::noCycles))
            {
                if (f.getFileName().startsWithChar ('.'))
                    continue; // macOS AppleDouble "._x.wav" (not hidden on Windows), our hidden temporary files
                if (outputNested && (f.isAChildOf (s.outputFolder) || canonical (f).startsWith (outputKey)))
                    continue; // never re-process our own results
                add (f, f.getRelativePathFrom (in));
            }
        }
        else
        {
            add (in, in.getFileName());
        }
    }
    std::stable_sort (candidates.begin(), candidates.end(),
                      [] (const Candidate& a, const Candidate& b) { return a.displayName < b.displayName; });

    const auto ext = extensionFor (s.format);
    std::set<juce::String> taken; // output paths (lower case: case-insensitive file systems)
    std::set<juce::String> inputs; // canonical()
    for (const auto& c : candidates)
        inputs.insert (canonical (c.file));

    for (const auto& c : candidates)
    {
        ExportItem item;
        item.input = c.file;
        item.displayName = c.displayName;
        if (! c.file.existsAsFile())
        {
            item.status = ExportItem::Status::Failed;
            item.error = "File not found";
        }
        else if (manager.findFormatForFileExtension (c.file.getFileExtension()) == nullptr)
        {
            item.status = ExportItem::Status::Skipped;
            item.error = "Not an audio file this build can read";
        }
        out.push_back (std::move (item));
    }

    // Output names: the same relative path below the output folder, with the
    // output extension. Files that already have that extension choose first,
    // so on a clash (a.wav + a.flac -> WAV) a.wav keeps "a.wav" and the other
    // gets its source extension appended ("a_flac.wav").
    for (const bool matchingExtension : { true, false })
    {
        for (auto& item : out)
        {
            if (item.status != ExportItem::Status::Queued || item.input.hasFileExtension (ext) != matchingExtension)
                continue;
            const auto relative = item.displayName.upToLastOccurrenceOf (".", false, false);
            const auto stem = relative.isNotEmpty() ? relative : item.displayName;
            auto candidate = s.outputFolder.getChildFile (stem + ext);
            if (taken.count (candidate.getFullPathName().toLowerCase()) != 0)
            {
                const auto source = item.input.getFileExtension().trimCharactersAtStart (".").toLowerCase();
                candidate = s.outputFolder.getChildFile (stem + "_" + source + ext);
                for (int n = 2; taken.count (candidate.getFullPathName().toLowerCase()) != 0; ++n)
                    candidate = s.outputFolder.getChildFile (stem + "_" + source + "_" + juce::String (n) + ext);
            }
            taken.insert (candidate.getFullPathName().toLowerCase());
            item.output = candidate;
        }
    }

    for (const auto& item : out)
    {
        if (item.output != juce::File() && inputs.count (canonical (item.output)) != 0)
        {
            error = "The output file " + item.output.getFullPathName() + " would overwrite an input file. Choose an output folder "
                    "outside the input folder tree.";
            out.clear();
            return false;
        }
    }
    return true;
}

// =============================================================================
// Decoding / encoding
// =============================================================================
bool ExportJob::decode (const juce::AudioFormatManager& manager, const juce::File& file, flub::io::AudioFileData& out, juce::String& description,
                        juce::String& error, const std::atomic<bool>* abortFlag)
{
    // createReaderFor (File) is not const, although it only reads the list.
    std::unique_ptr<juce::AudioFormatReader> reader (const_cast<juce::AudioFormatManager&> (manager).createReaderFor (file));
    if (reader == nullptr)
    {
        error = "Cannot decode: not a supported audio file, or the file is damaged";
        return false;
    }

    const int numChannels = static_cast<int> (reader->numChannels);
    const juce::int64 length = reader->lengthInSamples;
    description = reader->getFormatName().upToLastOccurrenceOf (" file", false, true) + " "
                  + (reader->usesFloatingPointData ? juce::String (reader->bitsPerSample) + "-bit float"
                                                   : juce::String (reader->bitsPerSample) + "-bit")
                  + ", " + juce::String (juce::roundToInt (reader->sampleRate)) + " Hz, " + juce::String (numChannels) + " ch";

    if (numChannels < 1 || numChannels > 8)
    {
        error = "Unsupported channel count " + juce::String (numChannels) + " (supported: 1, 2, 6 = 5.1, 8 = 7.1)";
        return false;
    }
    if (length <= 0)
    {
        error = "The file has no audio";
        return false;
    }
    if (length > std::numeric_limits<int>::max())
    {
        error = "The file is too long";
        return false;
    }

    out = flub::io::AudioFileData();
    out.sampleRate = reader->sampleRate;
    out.numChannels = numChannels;
    out.sourceFormat = reader->usesFloatingPointData ? flub::io::SampleFormat::Float32 : flub::io::SampleFormat::Pcm24;
    out.channels.assign (static_cast<size_t> (numChannels), std::vector<float> (static_cast<size_t> (length), 0.0f));

    std::vector<float*> dest (static_cast<size_t> (numChannels));
    for (juce::int64 pos = 0; pos < length; pos += kDecodeBlock)
    {
        if (isAborted (abortFlag))
        {
            error = kAborted;
            return false;
        }
        const int n = static_cast<int> (std::min<juce::int64> (kDecodeBlock, length - pos));
        for (size_t c = 0; c < dest.size(); ++c)
            dest[c] = out.channels[c].data() + pos;
        if (! reader->read (dest.data(), numChannels, pos, n))
        {
            error = "Read error at frame " + juce::String (pos);
            return false;
        }
    }
    return true;
}

bool ExportJob::encode (const juce::AudioFormatManager& manager, const juce::File& file, ExportFormat format, flub::cli::RenderResult& result,
                        juce::String& error, const std::atomic<bool>* abortFlag)
{
    if (isAborted (abortFlag))
    {
        error = kAborted;
        return false;
    }

    // Written next to the target and moved into place when complete, so a
    // failed or aborted export never leaves a truncated file under the real name.
    juce::TemporaryFile temp (file, juce::TemporaryFile::useHiddenFile);
    const auto tempPath = temp.getFile().getFullPathName();

    if (format == ExportFormat::WavFloat32 || format == ExportFormat::WavPcm24 || format == ExportFormat::WavPcm16)
    {
        const auto sampleFormat = format == ExportFormat::WavFloat32 ? flub::io::SampleFormat::Float32
                                  : format == ExportFormat::WavPcm24 ? flub::io::SampleFormat::Pcm24
                                                                     : flub::io::SampleFormat::Pcm16;
        std::string e;
        if (! flub::cli::writeRender (tempPath.toStdString(), sampleFormat, result, e))
        {
            error = toJuce (e).replace (tempPath, file.getFullPathName());
            return false;
        }
    }
    else
    {
        auto fileStream = temp.getFile().createOutputStream();
        if (fileStream == nullptr || fileStream->failedToOpen())
        {
            error = "Cannot write " + file.getFullPathName();
            return false;
        }
        if (! writeFlac (std::move (fileStream), format == ExportFormat::Flac24 ? 24 : 16, result.output, error, abortFlag))
        {
            if (error != kAborted)
                error << ": " << file.getFullPathName();
            return false;
        }

        // Report what was delivered: the quantised, dithered samples.
        flub::io::AudioFileData written;
        juce::String description;
        if (! decode (manager, temp.getFile(), written, description, error, abortFlag))
        {
            if (error != kAborted)
                error = "Cannot read back " + file.getFullPathName() + ": " + error;
            return false;
        }
        result.outputReport = flub::cli::analyse (written.channels, written.sampleRate);
    }

    if (isAborted (abortFlag))
    {
        error = kAborted; // the temporary file is deleted, nothing is moved into place
        return false;
    }
    if (! temp.overwriteTargetFileWithTemporary())
    {
        error = "Cannot replace " + file.getFullPathName();
        return false;
    }
    return true;
}

bool ExportJob::writeFlac (std::unique_ptr<juce::OutputStream> stream, int bitsPerSample, const flub::io::AudioFileData& audio,
                           juce::String& error, const std::atomic<bool>* abortFlag)
{
    const auto& channels = audio.channels;
    if (channels.size() != 2)
    {
        error = "Internal error: the render is not stereo";
        return false;
    }
    if (stream == nullptr)
    {
        error = "Cannot write";
        return false;
    }

    bool streamFailed = false;
    {
        std::unique_ptr<juce::OutputStream> tracked = std::make_unique<FailureTrackingStream> (std::move (stream), streamFailed);
        juce::FlacAudioFormat flac;
        auto writer = flac.createWriterFor (tracked, juce::AudioFormatWriterOptions()
                                                         .withSampleRate (audio.sampleRate)
                                                         .withNumChannels (2)
                                                         .withBitsPerSample (bitsPerSample)
                                                         .withQualityOptionIndex (kFlacQuality));
        if (writer == nullptr)
        {
            error = "The FLAC encoder cannot write " + juce::String (audio.sampleRate, 0) + " Hz audio";
            return false;
        }

        // Dithered integers (interleaved dither order, like writeWav),
        // left-aligned in 32 bits as AudioFormatWriter::write expects.
        TpdfDither dither;
        const double scale = bitsPerSample == 24 ? 8388608.0 : 32768.0;
        const int shift = 1 << (32 - bitsPerSample);
        const auto numFrames = channels[0].size();
        std::vector<int> left (kEncodeBlock), right (kEncodeBlock);
        for (size_t frame = 0; frame < numFrames;)
        {
            if (isAborted (abortFlag))
            {
                error = kAborted;
                return false;
            }
            const auto n = std::min (static_cast<size_t> (kEncodeBlock), numFrames - frame);
            for (size_t i = 0; i < n; ++i)
            {
                left[i] = quantise (channels[0][frame + i], scale, dither) * shift;
                right[i] = quantise (channels[1][frame + i], scale, dither) * shift;
            }
            const int* data[] = { left.data(), right.data(), nullptr };
            if (! writer->write (data, static_cast<int> (n)))
            {
                error = "Write error (disk full?)";
                return false;
            }
            frame += n;
        }
    } // the writer finishes the stream (last frame, STREAMINFO), flushes and closes it here

    if (streamFailed) // a failure the writer itself does not report
    {
        error = "Write error (disk full?)";
        return false;
    }
    return true;
}

// =============================================================================
// The job
// =============================================================================
bool ExportJob::start (ExportSettings newSettings, juce::String& error)
{
    if (running.load())
    {
        error = "An export is already running.";
        return false;
    }
    if (! validate (newSettings, error))
        return false;
    stopThread (-1); // the previous run has returned (running == false); join it

    Progress fresh;
    applyTargetRules (newSettings.values, newSettings.targetLufs, newSettings.ceilingDb, fresh.notes);
    fresh.running = true;
    settings = std::move (newSettings);
    {
        const juce::ScopedLock sl (lock);
        items.clear();
        progress = fresh;
    }
    cancelRequested.store (false);
    abortRequested.store (false);
    running.store (true);
    finishedEvent.reset();
    if (! startThread())
    {
        running.store (false);
        finishedEvent.signal();
        const juce::ScopedLock sl (lock);
        progress.running = false;
        error = progress.error = "Cannot start the export thread.";
        return false;
    }
    sendChangeMessage();
    return true;
}

ExportJob::Progress ExportJob::getProgress() const
{
    const juce::ScopedLock sl (lock);
    return progress;
}

std::vector<ExportItem> ExportJob::getItems() const
{
    const juce::ScopedLock sl (lock);
    return items;
}

void ExportJob::publish (const std::function<void()>& change)
{
    {
        const juce::ScopedLock sl (lock);
        change();
    }
    sendChangeMessage(); // async: coalesced and delivered on the message thread
}

void ExportJob::run()
{
    std::vector<ExportItem> planned;
    juce::String planError;
    const bool planOk = plan (settings, formats, planned, planError);
    publish ([&]
             {
                 if (! planOk)
                 {
                     progress.error = planError;
                     return;
                 }
                 items = std::move (planned);
                 for (const auto& item : items)
                 {
                     if (item.status == ExportItem::Status::Skipped)
                         ++progress.skipped;
                     else
                         ++progress.total;
                     if (item.status == ExportItem::Status::Failed) // e.g. vanished before the scan
                     {
                         ++progress.finished;
                         ++progress.failed;
                     }
                 }
             });

    if (planOk)
    {
        flub::cli::RenderSettings renderSettings = makeRenderSettings (settings, settings.values);
        renderSettings.abort = &abortRequested;
        const auto count = getItems().size();

        for (size_t i = 0; i < count; ++i)
        {
            ExportItem::Status status;
            {
                const juce::ScopedLock sl (lock);
                status = items[i].status;
            }
            if (status != ExportItem::Status::Queued)
                continue;

            if (cancelRequested.load() || threadShouldExit())
            {
                publish ([&]
                         {
                             for (auto& item : items)
                                 if (item.status == ExportItem::Status::Queued)
                                     item.status = ExportItem::Status::Cancelled;
                             progress.cancelled = true;
                         });
                break;
            }

            publish ([&]
                     {
                         items[i].status = ExportItem::Status::Running;
                         progress.current = static_cast<int> (i);
                     });
            if (onItemStarted)
                onItemStarted (static_cast<int> (i));
            processItem (i, settings.values, renderSettings);
            if (onItemFinished)
                onItemFinished (static_cast<int> (i));
        }
    }

    publish ([&]
             {
                 progress.current = -1;
                 progress.running = false;
                 progress.completed = true;
                 // Cleared before the change message is posted: a listener
                 // reading isRunning() for this message must see false.
                 running.store (false);
             });
    finishedEvent.signal();
}

void ExportJob::processItem (size_t index, const std::vector<float>& values, const flub::cli::RenderSettings& renderSettings)
{
    ExportItem item;
    {
        const juce::ScopedLock sl (lock);
        item = items[index];
    }

    auto fail = [&item] (const juce::String& message)
    {
        item.status = ExportItem::Status::Failed;
        item.error = message;
    };

    try
    {
        flub::io::AudioFileData input;
        juce::String error;
        std::string e;
        if (! decode (formats, item.input, input, item.inputFormat, error, &abortRequested))
        {
            fail (error);
        }
        else if (! flub::cli::checkRenderable (input, e))
        {
            fail (toJuce (e));
        }
        else
        {
            item.sampleRate = input.sampleRate;
            item.numChannels = input.numChannels;
            item.numFrames = input.numFrames();
            const auto inReport = flub::cli::analyse (input.channels, input.sampleRate);
            item.inLufs = inReport.integratedLufs;
            item.inTruePeakDbtp = inReport.truePeakDbtp;

            flub::cli::RenderResult rr;
            if (abortRequested.load())
            {
                fail (kAborted);
            }
            else if (! flub::cli::renderFile (input, values, renderSettings, rr, e))
            {
                fail (e == flub::cli::kAbortedError ? juce::String (kAborted) : toJuce (e));
            }
            else
            {
                input = flub::io::AudioFileData(); // free the source before encoding
                item.passes = rr.passes;
                for (const auto& n : rr.notes)
                    item.notes.add (toJuce (n));
                const auto folder = item.output.getParentDirectory();
                if (const auto r = folder.createDirectory(); r.failed())
                    fail ("Cannot create " + folder.getFullPathName() + ": " + r.getErrorMessage());
                else if (! encode (formats, item.output, settings.format, rr, error, &abortRequested))
                    fail (error);
                else
                {
                    item.status = ExportItem::Status::Done;
                    item.outLufs = rr.outputReport.integratedLufs;
                    item.outTruePeakDbtp = rr.outputReport.truePeakDbtp;
                }
            }
        }
    }
    catch (const std::bad_alloc&)
    {
        fail ("Out of memory");
    }
    catch (const std::exception& ex)
    {
        fail (ex.what());
    }

    publish ([&]
             {
                 items[index] = item;
                 ++progress.finished;
                 if (item.status == ExportItem::Status::Failed)
                     ++progress.failed;
             });
}
} // namespace flub::app
