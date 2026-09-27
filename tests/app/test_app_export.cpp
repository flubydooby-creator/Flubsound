// App-level tests: the Export / batch process job (app/Source/export).
//
// * The job object renders real files in a temporary folder: WAV, AIFF and
//   FLAC inputs written with JUCE's own writers, a corrupt WAV and a .txt;
//   outputs are read back and checked for format, length, rate and channel
//   count, the loudness target, the ceiling and the per-file results.
// * Parity with the CLI: the same WAV and parameters through
//   flub::cli::renderFile + writeRender (what `flubsound-cli process` runs)
//   give byte-identical files (float32 bit-exact, PCM16 with the same dither).
// * Cancel: requested while the first file renders, the job completes that
//   file and marks the rest cancelled; abort abandons it without a file.
// * Safety: an output folder that is an input folder (or would overwrite an
//   input) is refused.
// * The dialog: headless construction, the selected strip's snapshot (Bypass
//   All ignored) or a preset as the parameter source, its layout at the
//   minimum size, and a Start through the dialog.
// The worker is always waited on through ExportJob::waitForCompletion (the
// timeout is only a hang guard); nothing measures wall-clock time.
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "export/ExportDialog.h"
#include "export/ExportJob.h"

#include "Analysis.h"
#include "OfflineRenderer.h"

#include "flub/common/Math.h"
#include "flub/io/WavFile.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <cmath>
#include <vector>

using namespace flub::app;
using namespace flub::param;

namespace
{
constexpr int kHangGuardMs = 120000; // only a guard: the renders take a fraction of this

/** Drum-like programme (kick, noise hats, bass, pad) - deterministic for a
    seed (the generator of tests/test_offline_render.cpp). */
juce::AudioBuffer<float> makeProgramme (int numChannels, double sampleRate, double seconds, float level, uint32_t seed)
{
    const int n = static_cast<int> (std::lround (seconds * sampleRate));
    juce::AudioBuffer<float> b (numChannels, n);
    flub::FastRandom rng (seed);
    for (int i = 0; i < n; ++i)
    {
        const double t = static_cast<double> (i) / sampleRate;
        const double beat = std::fmod (t, 0.5);
        const double kick = std::exp (-beat * 18.0) * std::sin (flub::kTwoPi * (50.0 + 80.0 * std::exp (-beat * 30.0)) * beat);
        const double hat = (std::fmod (t + 0.25, 0.5) < 0.03 ? 0.3 : 0.0) * rng.nextBipolar();
        const double tone = 0.4 * std::sin (flub::kTwoPi * 55.0 * t) + 0.15 * std::sin (flub::kTwoPi * 440.0 * t);
        for (int c = 0; c < numChannels; ++c)
            b.setSample (c, i, level * static_cast<float> (kick + (c == 1 ? 0.8 : 1.0) * hat + tone * (1.0 - 0.1 * c)));
    }
    return b;
}

flub::io::AudioFileData toFileData (const juce::AudioBuffer<float>& b, double sampleRate)
{
    flub::io::AudioFileData d;
    d.sampleRate = sampleRate;
    d.numChannels = b.getNumChannels();
    for (int c = 0; c < b.getNumChannels(); ++c)
        d.channels.emplace_back (b.getReadPointer (c), b.getReadPointer (c) + b.getNumSamples());
    return d;
}

/** Writes with a JUCE writer (WAV / AIFF / FLAC); bits 32 + isFloat = IEEE float WAV. */
bool writeWithJuce (juce::AudioFormat& format, const juce::File& file, const juce::AudioBuffer<float>& b, double sampleRate, int bits,
                    bool isFloat = false)
{
    auto fileStream = file.createOutputStream();
    if (fileStream == nullptr || fileStream->failedToOpen())
        return false;
    std::unique_ptr<juce::OutputStream> stream (std::move (fileStream));
    using SF = juce::AudioFormatWriterOptions::SampleFormat;
    auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                      .withSampleRate (sampleRate)
                                                      .withNumChannels (b.getNumChannels())
                                                      .withBitsPerSample (bits)
                                                      .withSampleFormat (isFloat ? SF::floatingPoint : SF::integral));
    return writer != nullptr && writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

struct Decoded
{
    bool ok = false;
    int numChannels = 0, bitsPerSample = 0;
    bool isFloat = false;
    double sampleRate = 0.0;
    juce::int64 numFrames = 0;
    juce::String formatName;
    flub::io::AudioFileData data;
};

Decoded readWithJuce (const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    Decoded d;
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
        return d;
    d.numChannels = static_cast<int> (reader->numChannels);
    d.bitsPerSample = static_cast<int> (reader->bitsPerSample);
    d.isFloat = reader->usesFloatingPointData;
    d.sampleRate = reader->sampleRate;
    d.numFrames = reader->lengthInSamples;
    d.formatName = reader->getFormatName();
    juce::String description, error;
    d.ok = ExportJob::decode (formats, file, d.data, description, error);
    return d;
}

std::vector<float> defaultValues()
{
    std::vector<float> v (static_cast<size_t> (kNumParams));
    for (int id = 0; id < kNumParams; ++id)
        v[static_cast<size_t> (id)] = layout()[static_cast<size_t> (id)].defaultValue;
    return v;
}

bool runToCompletion (ExportJob& job, ExportSettings settings)
{
    juce::String error;
    const bool started = job.start (std::move (settings), error);
    CHECK (started);
    if (! started)
        return false;
    const bool finished = job.waitForCompletion (kHangGuardMs);
    CHECK (finished);
    return finished && ! job.isRunning();
}

int countFiles (const juce::File& folder)
{
    return folder.getNumberOfChildFiles (juce::File::findFiles | juce::File::ignoreHiddenFiles, "*")
           + folder.getNumberOfChildFiles (juce::File::findFiles, ".*"); // + hidden temporary files, if any were left
}

EngineController::Options headlessOptions (const flubapptest::TempFolder& temp)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = false;
    return o;
}
} // namespace

TEST_CASE ("App export: a folder of WAV / AIFF / FLAC renders each file; a corrupt WAV fails, a .txt is skipped, inputs stay untouched")
{
    flubapptest::TempFolder temp;
    const auto in = temp.file ("in"), out = temp.file ("out");
    REQUIRE (in.createDirectory().wasOk());

    juce::WavAudioFormat wav;
    juce::AiffAudioFormat aiff;
    juce::FlacAudioFormat flac;
    const auto a = makeProgramme (2, 48000.0, 1.2, 0.2f, 71);
    const auto b = makeProgramme (1, 44100.0, 1.0, 0.1f, 72);
    const auto c = makeProgramme (2, 32000.0, 0.9, 0.3f, 73);
    REQUIRE (writeWithJuce (wav, in.getChildFile ("a.wav"), a, 48000.0, 32, true));
    REQUIRE (writeWithJuce (aiff, in.getChildFile ("b.aiff"), b, 44100.0, 24));
    REQUIRE (writeWithJuce (flac, in.getChildFile ("c.flac"), c, 32000.0, 16));
    juce::MemoryBlock garbage;
    for (int i = 0; i < 4096; ++i)
        garbage.append ("\x13\x37\xAB", 3);
    REQUIRE (in.getChildFile ("d.wav").replaceWithData (garbage.getData(), garbage.getSize()));
    REQUIRE (in.getChildFile ("notes.txt").replaceWithText ("not audio"));
    juce::MemoryBlock inputBefore;
    REQUIRE (in.getChildFile ("a.wav").loadFileAsData (inputBefore));

    ExportSettings s;
    s.inputs.add (in);
    s.outputFolder = out; // does not exist yet: created by the job
    s.format = ExportFormat::WavPcm24;
    s.values = defaultValues();

    ExportJob job;
    REQUIRE (runToCompletion (job, s));

    const auto items = job.getItems();
    REQUIRE (items.size() == 5);
    CHECK (items[0].displayName == "a.wav");
    CHECK (items[1].displayName == "b.aiff");
    CHECK (items[2].displayName == "c.flac");
    CHECK (items[3].displayName == "d.wav");
    CHECK (items[4].displayName == "notes.txt");
    for (size_t i = 0; i < 3; ++i)
    {
        CHECK (items[i].status == ExportItem::Status::Done);
        CHECK (items[i].error.isEmpty());
        CHECK (items[i].output == out.getChildFile (items[i].displayName.upToLastOccurrenceOf (".", false, false) + ".wav"));
        CHECK (items[i].inLufs > -40.0f && items[i].inLufs < -5.0f);
        CHECK (items[i].outLufs > -40.0f && items[i].outLufs < 0.0f);
    }
    CHECK (items[0].inputFormat == "WAV 32-bit float, 48000 Hz, 2 ch");
    CHECK (items[1].inputFormat == "AIFF 24-bit, 44100 Hz, 1 ch");
    CHECK (items[2].inputFormat == "FLAC 16-bit, 32000 Hz, 2 ch");
    CHECK (items[3].status == ExportItem::Status::Failed);
    CHECK (items[3].error.contains ("decode"));
    CHECK (items[3].output == out.getChildFile ("d.wav"));
    CHECK (items[4].status == ExportItem::Status::Skipped);
    CHECK (items[4].output == juce::File());

    const auto p = job.getProgress();
    CHECK (p.total == 4);
    CHECK (p.finished == 4);
    CHECK (p.failed == 1);
    CHECK (p.skipped == 1);
    CHECK (p.completed && ! p.running && ! p.cancelled);
    CHECK (p.error.isEmpty());

    // The outputs: stereo PCM24 WAV at the input's rate and length.
    const struct
    {
        const char* name;
        double rate;
        int frames;
    } expected[] = { { "a.wav", 48000.0, a.getNumSamples() }, { "b.wav", 44100.0, b.getNumSamples() }, { "c.wav", 32000.0, c.getNumSamples() } };
    for (const auto& e : expected)
    {
        flub::io::AudioFileData written;
        std::string error;
        REQUIRE (flub::io::readWav (out.getChildFile (e.name).getFullPathName().toStdString(), written, error));
        CHECK (written.sourceFormat == flub::io::SampleFormat::Pcm24);
        CHECK (written.numChannels == 2);
        CHECK (written.sampleRate == e.rate);
        CHECK (written.numFrames() == e.frames);
    }
    CHECK (! out.getChildFile ("d.wav").exists());
    CHECK (countFiles (out) == 3); // no temporary file left behind

    // Inputs untouched.
    juce::MemoryBlock inputAfter;
    REQUIRE (in.getChildFile ("a.wav").loadFileAsData (inputAfter));
    CHECK (inputAfter == inputBefore);
    CHECK (in.getNumberOfChildFiles (juce::File::findFiles) == 5);
}

TEST_CASE ("App export: FLAC 24 / 16 outputs meet the loudness target within 0.3 LU under the ceiling, measured on the file")
{
    flubapptest::TempFolder temp;
    const auto in = temp.file ("in");
    REQUIRE (in.createDirectory().wasOk());
    juce::WavAudioFormat wav;
    juce::AiffAudioFormat aiff;
    const auto quiet = makeProgramme (2, 48000.0, 3.0, 0.08f, 81); // louder through max.drive
    const auto loud = makeProgramme (1, 44100.0, 3.0, 0.5f, 82);   // quieter through output.gain
    REQUIRE (writeWithJuce (wav, in.getChildFile ("quiet.wav"), quiet, 48000.0, 24));
    REQUIRE (writeWithJuce (aiff, in.getChildFile ("loud.aif"), loud, 44100.0, 16));

    for (const auto format : { ExportFormat::Flac24, ExportFormat::Flac16 })
    {
        const int bits = format == ExportFormat::Flac24 ? 24 : 16;
        const auto out = temp.file (format == ExportFormat::Flac24 ? "out24" : "out16");
        ExportSettings s;
        s.inputs.add (in.getChildFile ("quiet.wav"));
        s.inputs.add (in.getChildFile ("loud.aif"));
        s.outputFolder = out;
        s.format = format;
        s.targetLufs = -16.0f;
        s.ceilingDb = -1.0f;
        s.values = defaultValues();
        s.values[static_cast<size_t> (MaximizerOn)] = 0.0f; // the job switches it on for the target / ceiling

        ExportJob job;
        REQUIRE (runToCompletion (job, s));
        CHECK (job.getProgress().notes.size() == 1); // "Loudness maximizer enabled ..."

        const auto items = job.getItems();
        REQUIRE (items.size() == 2);
        const struct
        {
            const char* name;
            double rate;
            int frames;
        } expected[] = { { "loud.flac", 44100.0, loud.getNumSamples() }, { "quiet.flac", 48000.0, quiet.getNumSamples() } };
        for (size_t i = 0; i < 2; ++i)
        {
            CHECK (items[i].status == ExportItem::Status::Done);
            CHECK (items[i].passes >= 2);
            const auto decoded = readWithJuce (out.getChildFile (expected[i].name));
            REQUIRE (decoded.ok);
            CHECK (decoded.formatName.startsWith ("FLAC"));
            CHECK (decoded.bitsPerSample == bits);
            CHECK (decoded.numChannels == 2);
            CHECK (decoded.sampleRate == expected[i].rate);
            CHECK (decoded.numFrames == expected[i].frames);

            const auto report = flub::cli::analyse (decoded.data.channels, decoded.sampleRate);
            CHECK_NEAR (report.integratedLufs, -16.0, 0.3);
            CHECK_LE (report.truePeakDbtp, -1.0 + 0.1);
            // The result table reports the file as written (read back), not the float render.
            CHECK_NEAR (items[i].outLufs, report.integratedLufs, 1.0e-4);
            CHECK_NEAR (items[i].outTruePeakDbtp, report.truePeakDbtp, 1.0e-4);
        }
    }
}

TEST_CASE ("App export: WAV output is byte-identical to flubsound-cli's renderer for the same WAV and parameters (float32 bit-exact)")
{
    flubapptest::TempFolder temp;
    const auto in = temp.file ("in");
    REQUIRE (in.createDirectory().wasOk());
    const auto source = in.getChildFile ("song.wav");
    std::string error;
    REQUIRE (flub::io::writeWav (source.getFullPathName().toStdString(), toFileData (makeProgramme (2, 48000.0, 2.0, 0.15f, 91), 48000.0),
                                 flub::io::SampleFormat::Float32, error));

    // A non-default sound: Gaming mode, 60 % boost, a loudness target.
    auto values = defaultValues();
    values[static_cast<size_t> (Mode)] = static_cast<float> (static_cast<int> (ModeValue::Gaming));
    values[static_cast<size_t> (BoostIntensity)] = 0.6f;
    const std::optional<float> target = -18.0f;

    for (const auto& [format, cliFormat] : { std::pair { ExportFormat::WavFloat32, flub::io::SampleFormat::Float32 },
                                            std::pair { ExportFormat::WavPcm16, flub::io::SampleFormat::Pcm16 } })
    {
        const auto out = temp.file (format == ExportFormat::WavFloat32 ? "app32" : "app16");
        ExportSettings s;
        s.inputs.add (source);
        s.outputFolder = out;
        s.format = format;
        s.targetLufs = target;
        s.values = values;
        ExportJob job;
        REQUIRE (runToCompletion (job, s));
        const auto items = job.getItems();
        REQUIRE (items.size() == 1);
        REQUIRE (items[0].status == ExportItem::Status::Done);

        // What `flubsound-cli process --target-lufs -18` runs: readWav, the
        // CLI's parameter rules, renderFile, writeRender.
        auto cliValues = values;
        juce::StringArray notes;
        ExportJob::applyTargetRules (cliValues, target, std::nullopt, notes);
        flub::cli::RenderSettings rs;
        rs.targetLufs = target;
        rs.verifyCeilingDb = cliValues[static_cast<size_t> (MaxCeilingDb)];
        flub::io::AudioFileData input;
        REQUIRE (flub::io::readWav (source.getFullPathName().toStdString(), input, error));
        flub::cli::RenderResult rr;
        REQUIRE (flub::cli::renderFile (input, cliValues, rs, rr, error));
        const auto cliFile = temp.file (format == ExportFormat::WavFloat32 ? "cli32.wav" : "cli16.wav");
        REQUIRE (flub::cli::writeRender (cliFile.getFullPathName().toStdString(), cliFormat, rr, error));

        const auto appFile = out.getChildFile ("song.wav");
        CHECK (appFile.getSize() == cliFile.getSize());
        CHECK (appFile.hasIdenticalContentTo (cliFile));
        CHECK (items[0].outLufs == rr.outputReport.integratedLufs);
        CHECK (items[0].outTruePeakDbtp == rr.outputReport.truePeakDbtp);
        CHECK (items[0].passes == rr.passes);
        CHECK_NEAR (items[0].outLufs, -18.0, 0.3);

        if (format == ExportFormat::WavFloat32)
        {
            // Sample for sample, too (the float render itself).
            flub::io::AudioFileData app;
            REQUIRE (flub::io::readWav (appFile.getFullPathName().toStdString(), app, error));
            REQUIRE (app.numChannels == 2 && app.numFrames() == input.numFrames());
            bool identical = true;
            for (size_t ch = 0; ch < 2; ++ch)
                identical = identical && app.channels[ch] == rr.output.channels[ch];
            CHECK (identical);
        }
    }
}

TEST_CASE ("App export: cancel while a file renders completes that file and marks the rest cancelled; abort abandons it, writing nothing")
{
    flubapptest::TempFolder temp;
    const auto in = temp.file ("in"), out = temp.file ("out");
    REQUIRE (in.createDirectory().wasOk());
    juce::WavAudioFormat wav;
    for (int i = 0; i < 4; ++i)
        REQUIRE (writeWithJuce (wav, in.getChildFile ("take" + juce::String (i) + ".wav"), makeProgramme (2, 48000.0, 0.5, 0.2f, 100u + static_cast<uint32_t> (i)),
                                48000.0, 16));

    ExportSettings s;
    s.inputs.add (in);
    s.outputFolder = out;
    s.values = defaultValues();

    ExportJob job;
    std::atomic<int> started { 0 }, finished { 0 };
    // Called on the worker right before the first file renders: the cancel
    // arrives while that file is "current".
    job.onItemStarted = [&] (int index)
    {
        ++started;
        if (index == 0)
            job.cancel();
    };
    job.onItemFinished = [&] (int) { ++finished; };
    REQUIRE (runToCompletion (job, s));

    CHECK (started.load() == 1);
    CHECK (finished.load() == 1);
    const auto items = job.getItems();
    REQUIRE (items.size() == 4);
    CHECK (items[0].status == ExportItem::Status::Done);
    CHECK (items[0].output.existsAsFile());
    for (size_t i = 1; i < items.size(); ++i)
    {
        CHECK (items[i].status == ExportItem::Status::Cancelled);
        CHECK (! items[i].output.exists());
    }
    const auto p = job.getProgress();
    CHECK (p.cancelled);
    CHECK (p.finished == 1);
    CHECK (p.total == 4);
    CHECK (countFiles (out) == 1);

    // abort() abandons the file being rendered: it fails as "Aborted" and
    // nothing is written under its name (not even a temporary file).
    const auto aborted = temp.file ("aborted");
    s.outputFolder = aborted;
    job.onItemStarted = [&] (int index)
    {
        if (index == 0)
            job.abort();
    };
    REQUIRE (runToCompletion (job, s));
    const auto abortedItems = job.getItems();
    REQUIRE (abortedItems.size() == 4);
    CHECK (abortedItems[0].status == ExportItem::Status::Failed);
    CHECK (abortedItems[0].error == "Aborted");
    CHECK (abortedItems[1].status == ExportItem::Status::Cancelled);
    CHECK (! aborted.exists() || countFiles (aborted) == 0);
    s.outputFolder = out;

    // The same job object runs again after a cancel.
    job.onItemStarted = nullptr;
    job.onItemFinished = nullptr;
    REQUIRE (runToCompletion (job, s));
    CHECK (job.getProgress().finished == 4);
    CHECK (! job.getProgress().cancelled);
    CHECK (countFiles (out) == 4);
}

TEST_CASE ("App export: an output folder that is an input folder, or would overwrite an input, is refused; nested outputs are not re-read")
{
    flubapptest::TempFolder temp;
    const auto root = temp.file ("A");
    const auto in = root.getChildFile ("B");
    REQUIRE (in.getChildFile ("B").createDirectory().wasOk());
    juce::WavAudioFormat wav;
    const auto programme = makeProgramme (1, 48000.0, 0.3, 0.2f, 111);
    REQUIRE (writeWithJuce (wav, in.getChildFile ("x.wav"), programme, 48000.0, 16));
    REQUIRE (writeWithJuce (wav, in.getChildFile ("B/x.wav"), programme, 48000.0, 16));
    REQUIRE (writeWithJuce (wav, in.getChildFile ("y.wav"), programme, 48000.0, 16));
    juce::FlacAudioFormat flac;
    REQUIRE (writeWithJuce (flac, in.getChildFile ("y.flac"), programme, 48000.0, 16));

    ExportSettings s;
    s.values = defaultValues();
    juce::String error;

    // Output folder == input folder.
    s.inputs = { in };
    s.outputFolder = in;
    CHECK (! ExportJob::validate (s, error));
    CHECK (error.contains ("input folder"));
    // Output folder == the folder of an input file (also through "..").
    s.inputs = { in.getChildFile ("x.wav") };
    s.outputFolder = juce::File (in.getChildFile ("B").getFullPathName() + "/..");
    CHECK (! ExportJob::validate (s, error));
    CHECK (error.contains ("folder of an input file"));

    ExportJob job;
    CHECK (! job.start (s, error));
    CHECK (! job.isRunning());

    // Recursive input A/B with output A: A/B/B/x.wav would be written to
    // A/B/x.wav, itself an input -> the whole job is refused by plan().
    s.inputs = { in };
    s.recursive = true;
    s.outputFolder = root;
    CHECK (ExportJob::validate (s, error));
    std::vector<ExportItem> items;
    CHECK (! ExportJob::plan (s, job.getFormatManager(), items, error));
    CHECK (error.contains ("would overwrite an input"));
    CHECK (items.empty());
    REQUIRE (runToCompletion (job, s));
    CHECK (job.getProgress().error.contains ("would overwrite an input"));
    CHECK (job.getItems().empty());
    CHECK (in.getChildFile ("x.wav").getSize() == in.getChildFile ("B/x.wav").getSize()); // untouched

    // Output folder nested in the input folder: its files are never inputs;
    // y.wav and y.flac both want "y.wav": y.wav keeps it, y.flac becomes "y_flac.wav".
    const auto nested = in.getChildFile ("renders");
    REQUIRE (nested.createDirectory().wasOk());
    REQUIRE (writeWithJuce (wav, nested.getChildFile ("old.wav"), programme, 48000.0, 16));
    s.outputFolder = nested;
    REQUIRE (ExportJob::plan (s, job.getFormatManager(), items, error));
    juce::StringArray names, outputs;
    for (const auto& item : items)
    {
        names.add (item.displayName);
        outputs.add (item.output.getRelativePathFrom (nested).replaceCharacter ('\\', '/'));
    }
    CHECK (names.joinIntoString ("|") == "B/x.wav|x.wav|y.flac|y.wav");
    CHECK (outputs.joinIntoString ("|") == "B/x.wav|x.wav|y_flac.wav|y.wav");
}

TEST_CASE ("App export: the dialog takes dropped folders and renders the selected strip's settings (Bypass All ignored) or a preset; fits 720 x 560")
{
    flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    const int strip = controller.getSelectedStrip();
    auto& store = controller.getParams (strip);
    store.set (BoostIntensity, 0.77f);
    store.set (EqOn, 0.0f);
    controller.setEnabled (false); // master bypass: Bypass All on every strip
    REQUIRE (store.get (BypassAll) >= 0.5f);

    const auto in = temp.file ("in"), out = temp.file ("out");
    REQUIRE (in.createDirectory().wasOk());
    juce::WavAudioFormat wav;
    REQUIRE (writeWithJuce (wav, in.getChildFile ("clip.wav"), makeProgramme (2, 48000.0, 0.5, 0.2f, 121), 48000.0, 24));

    ui::ExportDialog dialog (controller);
    // A folder dropped on the dialog becomes an input (once, however often it is dropped).
    CHECK (dialog.isInterestedInFileDrag ({ in.getFullPathName() }));
    dialog.filesDropped ({ in.getFullPathName() }, 10, 10);
    dialog.filesDropped ({ in.getFullPathName() }, 10, 10);
    dialog.setOutputFolder (out);
    dialog.setFormat (ExportFormat::Flac16);

    ExportSettings s;
    juce::String error;
    REQUIRE (dialog.buildSettings (s, error));
    REQUIRE (s.values.size() == static_cast<size_t> (kNumParams));
    CHECK (s.values[static_cast<size_t> (BoostIntensity)] == store.get (BoostIntensity));
    CHECK (s.values[static_cast<size_t> (EqOn)] == 0.0f);
    CHECK (s.values[static_cast<size_t> (BypassAll)] == 0.0f); // the export renders the strip's sound, not the master bypass
    CHECK (s.format == ExportFormat::Flac16);
    CHECK (s.inputs.size() == 1 && s.inputs[0] == in);
    CHECK (! s.targetLufs && ! s.ceilingDb);

    dialog.setLoudnessTarget (-20.0f);
    dialog.setCeiling (-2.0f);
    REQUIRE (dialog.buildSettings (s, error));
    CHECK (s.targetLufs && *s.targetLufs == -20.0f);
    CHECK (s.ceilingDb && *s.ceilingDb == -2.0f);

    // A preset as the source: its values through the PresetManager's rules.
    const auto& presets = controller.getPresetManager().getPresets();
    if (! presets.empty())
    {
        dialog.setParameterSource (presets.front().id);
        REQUIRE (dialog.buildSettings (s, error));
        std::vector<float> expected;
        REQUIRE (ExportJob::presetValues (controller.getPresetManager(), presets.front(), expected, error));
        CHECK (s.values == expected);
        CHECK (s.values[static_cast<size_t> (BypassAll)] == 0.0f);
        dialog.setParameterSource ({});
    }

    // Refused settings surface as the status line; nothing starts.
    dialog.setOutputFolder (in);
    CHECK (! dialog.startExport());
    CHECK (dialog.getStatusText().contains ("input folder"));
    CHECK (! dialog.getJob().isRunning());
    dialog.setOutputFolder (out);

    // Layout at the minimum size: every control inside, none overlapping.
    dialog.setBounds (0, 0, ui::ExportDialog::kMinWidth, ui::ExportDialog::kMinHeight);
    const auto bounds = dialog.getLocalBounds();
    juce::Component* table = nullptr;
    std::vector<juce::Component*> visible;
    for (auto* child : dialog.getChildren())
        if (child->isVisible())
        {
            visible.push_back (child);
            CHECK (bounds.contains (child->getBounds()));
            CHECK (! child->getBounds().isEmpty());
            if (dynamic_cast<juce::TableListBox*> (child) != nullptr)
                table = child;
        }
    CHECK (visible.size() >= 17);
    for (size_t i = 0; i < visible.size(); ++i)
        for (size_t j = i + 1; j < visible.size(); ++j)
            CHECK (! visible[i]->getBounds().intersects (visible[j]->getBounds()));
    REQUIRE (table != nullptr);
    CHECK (table->getHeight() >= 150); // room for several result rows

    // Start through the dialog: the job runs off the message thread and the
    // dialog follows it through its ChangeListener.
    dialog.setLoudnessTarget (std::nullopt);
    dialog.setCeiling (std::nullopt);
    REQUIRE (dialog.startExport());
    REQUIRE (dialog.getJob().waitForCompletion (kHangGuardMs));
    CHECK (flubapptest::pumpMessagesUntil ([&] { return dialog.getStatusText().startsWith ("1 done"); }));
    CHECK (out.getChildFile ("clip.flac").existsAsFile());
}
