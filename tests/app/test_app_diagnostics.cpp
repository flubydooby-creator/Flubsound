// App-level tests: diagnostics (docs/11 E54).
//
// * RotatingFileLogger: size-limited files, rotation keeps the newest N,
//   every line redacted (home folder, login and computer name).
// * EventLogBuilder: device opens, changes, closes and errors, overloads at
//   once; glitch and safety counters summed into one line per interval.
// * Crash handler: a crash writes crash-<time>-<pid>.txt with the signal,
//   the fault address and the crashing thread's stack, then ends the process
//   (POSIX: in a forked child, SIGSEGV and an uncaught exception; Windows: a
//   synthetic access violation through the filter's own writer, plus the
//   minidump). Old reports beyond the limit are deleted.
// * DiagnosticsSession: start / stop lines, a crash report the previous
//   session left, engine state from the controller.
// * Settings > Diagnostics: the export zip holds system details, settings,
//   logs and crash reports, redacted, and no audio.
#include "AppTestSupport.h"

#include "diagnostics/CrashHandler.h"
#include "diagnostics/DiagnosticLog.h"
#include "diagnostics/DiagnosticsMonitor.h"
#include "diagnostics/DiagnosticsSession.h"
#include "engine/EngineController.h"
#include "ui/SettingsDialog.h"

#include <cstdint>
#include <stdexcept>

#if ! JUCE_WINDOWS
 #include <sys/wait.h>
 #include <unistd.h>
#endif

using namespace flub::app;

namespace
{
EngineController::Options headless (const flubapptest::TempFolder& temp)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = true;
    o.foregroundAppFactory = [] { return std::unique_ptr<flub::platform::ForegroundApp>(); };
    return o;
}

diagnostics::EngineSnapshot closedDevice()
{
    diagnostics::EngineSnapshot s;
    for (const char* name : { "Game", "Music", "Chat", "System" })
    {
        diagnostics::EngineSnapshot::Strip strip;
        strip.name = name;
        s.strips.push_back (strip);
    }
    return s;
}

juce::String homePath()
{
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName();
}

juce::String readZipEntry (juce::ZipFile& zip, const juce::String& name)
{
    const int index = zip.getIndexOfFileName ("Flubsound-diagnostics/" + name);
    if (index < 0)
        return {};
    std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (index));
    return in != nullptr ? in->readEntireStreamAsString() : juce::String();
}
} // namespace

TEST_CASE ("App diagnostics: redactPersonalData replaces the home folder in both slash styles and whole-word login and computer names (E54)")
{
    const auto text = diagnostics::redactPersonalData ("C:\\Users\\Flubes\\AppData\\Roaming\\Flubsound\\a.log; C:/Users/Flubes/Music/x.wav; "
                                                       "user FLUBES on flubes-pc; Flubesound stays; flubes2 stays",
                                                       "C:\\Users\\Flubes", "flubes", "FLUBES-PC");
    CHECK (text == "~\\AppData\\Roaming\\Flubsound\\a.log; ~/Music/x.wav; user <user> on <computer>; Flubesound stays; flubes2 stays");
    // Names shorter than 3 characters would hit ordinary words: left alone.
    CHECK (diagnostics::redactPersonalData ("an audio device", "/home/an", "an", "") == "an audio device");
}

TEST_CASE ("App diagnostics: the log rotates before a file passes its size, keeps three files and redacts every line (E54)")
{
    const flubapptest::TempFolder temp;
    diagnostics::RotatingFileLogger::Options options;
    options.folder = temp.file ("Logs");
    options.maxFileBytes = 2000;
    options.keepFiles = 3;
    diagnostics::RotatingFileLogger logger (options);
    CHECK (logger.getPreviousSessionEnd() == juce::Time());

    for (int i = 0; i < 100; ++i)
        logger.logMessage ("line " + juce::String (i) + " " + juce::String::repeatedString ("x", 30));
    const auto files = logger.getLogFiles();
    REQUIRE (files.size() == 3);
    CHECK (files[0] == logger.getCurrentFile());
    for (const auto& f : files)
        CHECK_LE (f.getSize(), 2000);
    const auto newest = files[0].loadFileAsString();
    CHECK (newest.contains ("line 99 "));
    CHECK (! files[2].loadFileAsString().contains ("line 0 ")); // the oldest lines are gone
    // Each line is "yyyy-mm-dd hh:mm:ss.mmm  message".
    const auto first = juce::StringArray::fromLines (newest)[0];
    CHECK (first.length() > 25 && first[4] == '-' && first[10] == ' ' && first[19] == '.' && first.substring (23, 25) == "  ");

    // Personal paths and names never reach the file.
    const auto home = homePath();
    logger.logMessage ("opened " + home + "/Music/song.flac\nsecond line");
    const auto last = logger.getCurrentFile().loadFileAsString();
    CHECK (last.contains ("opened ~/Music/song.flac"));
    CHECK (last.contains ("  second line")); // a multi-line message: one stamped line each
    if (home.length() > 1)
        CHECK (! last.contains (home));

    // A new logger on the same folder knows when the previous session wrote last.
    diagnostics::RotatingFileLogger again (options);
    CHECK (again.getPreviousSessionEnd() != juce::Time());
}

TEST_CASE ("App diagnostics: engine events are logged at once, glitch and safety counters summed into one line per interval (E54)")
{
    diagnostics::EventLogBuilder builder (10000);
    auto s = closedDevice();
    auto lines = builder.update (s, 0);
    CHECK (lines == juce::StringArray { "Audio device: no audio device open" });

    s.deviceOpen = true;
    s.deviceType = "ALSA";
    s.outputName = "Headphones";
    s.sampleRate = 48000.0;
    s.bufferSize = 256;
    CHECK (builder.update (s, 500) == juce::StringArray { "Audio device opened: ALSA / Headphones, 48000 Hz, 256 samples" });
    CHECK (builder.update (s, 1000).isEmpty()); // nothing changed

    // The first counts are written at once, the next ones wait for the interval.
    s.glitches = 3;
    s.strips[0].safetyClips = 2;
    s.strips[1].droppedBlocks = 1;
    CHECK (builder.update (s, 1500)
           == juce::StringArray { "Last 1 s: 3 glitches (xruns or late callbacks); Game: 2 safety clips; Music: 1 block dropped (NaN / Inf)" });
    s.glitches = 4;
    CHECK (builder.update (s, 2000).isEmpty());
    s.strips[3].corruptSamples = 5;
    CHECK (builder.update (s, 6000).isEmpty());
    CHECK (builder.update (s, 11500)
           == juce::StringArray { "Last 10 s: 1 glitch (xrun or late callback); System: 5 samples beyond +24 dBFS muted" });

    // Errors and overloads at once.
    s.deviceError = "Device is busy";
    CHECK (builder.update (s, 12000) == juce::StringArray { "Audio device error: Device is busy" });
    s.deviceError = {};
    s.overloaded = true;
    s.overloadEpisodes = 1;
    lines = builder.update (s, 12500);
    CHECK (lines == juce::StringArray ({ "Audio device error cleared", "CPU overload started (episode 1 this session)" }));

    // A device change writes the counts of the old device first; a counter
    // that restarts (new device or engine) counts from 0.
    s.glitches = 6;
    s.overloaded = false;
    s.inputName = "CABLE Output";
    lines = builder.update (s, 13000);
    CHECK (lines
           == juce::StringArray ({ "Last 2 s: 2 glitches (xruns or late callbacks)",
                                   "Audio device changed: ALSA / Headphones, in: CABLE Output, 48000 Hz, 256 samples", "CPU overload ended" }));
    s.glitches = 1;
    CHECK (builder.update (s, 30000) == juce::StringArray { "Last 17 s: 1 glitch (xrun or late callback)" });
    s.strips[0].safetyClips = 7; // 2 -> 7
    s.deviceOpen = false;
    lines = builder.update (s, 30500);
    CHECK (lines == juce::StringArray ({ "Last 1 s: Game: 5 safety clips", "Audio device closed" }));
    CHECK (builder.flush (31000).isEmpty());
}

TEST_CASE ("App diagnostics: a crash writes a report with the signal, the stack and the reason, then the process ends (E54)")
{
    const flubapptest::TempFolder temp;
    const auto folder = temp.file ("Logs");
    diagnostics::crash::Config config;
    config.folder = folder;
    config.header = "Flubsound Pro test header\nOS: test";
    config.exitCode = 42; // no core dump, no system crash reporter
    REQUIRE (diagnostics::crash::prepare (config));
    CHECK (! diagnostics::crash::isArmed());

   #if JUCE_WINDOWS
    CHECK (diagnostics::crash::writeTestReport());
    const auto reports = diagnostics::crash::findReports (folder);
    REQUIRE (reports.size() == 1);
    const auto text = reports[0].loadFileAsString();
    CHECK (text.startsWith ("Flubsound Pro crash report"));
    CHECK (text.contains ("Flubsound Pro test header\r\nOS: test"));
    CHECK (text.contains ("Exception: 0xc0000005 (access violation)"));
    CHECK (text.contains ("Access: write at 0x0000000000000010"));
    CHECK (text.contains ("flub_app_tests.exe + 0x"));
    CHECK (text.contains ("Stack of the crashing thread"));
    const auto dump = reports[0].withFileExtension ("dmp");
    CHECK (dump.existsAsFile() && dump.getSize() > 1000);
    std::cerr << "    report: " << reports[0].getFileName() << ", minidump " << dump.getSize() << " bytes\n";
   #else
    auto crashChild = [] (int how)
    {
        const pid_t child = ::fork();
        if (child == 0)
        {
            // Only async-signal-safe calls before the crash (the parent has threads).
            diagnostics::crash::arm();
            if (how == 0)
            {
                volatile uintptr_t address = 0x10;
                *reinterpret_cast<volatile int*> (address) = 1; // a real invalid write
            }
            try
            {
                throw std::runtime_error ("diagnostics test");
            }
            catch (...)
            {
                std::terminate(); // what an exception escaping a thread or a noexcept function does
            }
            ::_exit (1); // never reached: the child must not run the other tests
        }
        int status = 0;
        ::waitpid (child, &status, 0);
        return WIFEXITED (status) ? WEXITSTATUS (status) : -1;
    };

    CHECK (crashChild (0) == 42);
    auto reports = diagnostics::crash::findReports (folder);
    REQUIRE (reports.size() == 1);
    auto text = reports[0].loadFileAsString();
    CHECK (text.startsWith ("Flubsound Pro crash report\n\nFlubsound Pro test header\nOS: test\n"));
    CHECK (text.contains ("SIGSEGV (invalid memory access)") || text.contains ("SIGBUS (bus error)"));
    CHECK (text.contains ("Fault address: 0x0000000000000010\n"));
    CHECK (text.contains ("Stack of the crashing thread (innermost first):\n"));
    CHECK (text.contains ("flub_app_tests")); // backtrace_symbols_fd names the module
   #if JUCE_LINUX
    CHECK (text.contains ("Executable mappings (/proc/self/maps):") && text.contains (" r-xp "));
   #endif
    CHECK (reports[0].getFileName().matchesWildcard ("crash-*-*.txt", false));

    reports[0].setLastModificationTime (juce::Time::getCurrentTime() - juce::RelativeTime::seconds (10)); // the second one is newer
    CHECK (crashChild (1) == 42);
    reports = diagnostics::crash::findReports (folder);
    REQUIRE (reports.size() == 2);
    text = reports[0].loadFileAsString();
    CHECK (text.contains ("SIGABRT (abort)"));
    CHECK (text.contains ("Reason: uncaught exception: diagnostics test"));
    CHECK (text.contains ("Sent by: process ")); // abort() raises the signal itself
    CHECK (! text.contains ("Fault address"));
    std::cerr << "    " << reports.size() << " reports, the last " << text.length() << " characters\n";

    // prepare() keeps only the newest reports.
    config.keepReports = 1;
    REQUIRE (diagnostics::crash::prepare (config));
    reports = diagnostics::crash::findReports (folder);
    REQUIRE (reports.size() == 1);
    CHECK (reports[0].loadFileAsString().contains ("SIGABRT"));
   #endif
    diagnostics::crash::disarm();
    CHECK (! diagnostics::crash::isArmed());
}

TEST_CASE ("App diagnostics: a session logs start and stop, a crash the previous session left and the controller's device state (E54)")
{
    const flubapptest::TempFolder temp;
    diagnostics::DiagnosticsSession::Options options;
    options.folder = temp.file ("Logs");
    options.crashHandler = false; // the test process keeps its own crash behaviour
    auto* const loggerBefore = juce::Logger::getCurrentLogger();
    const auto log = options.folder.getChildFile ("flubsound.log");
    {
        diagnostics::DiagnosticsSession session (options);
        CHECK (juce::Logger::getCurrentLogger() == &session.getLogger());
        CHECK (session.getPreviousCrashReports().isEmpty());
        juce::Logger::writeToLog ("through juce::Logger");
    }
    CHECK (juce::Logger::getCurrentLogger() == loggerBefore);
    auto text = log.loadFileAsString();
    CHECK (text.contains ("==== Flubsound Pro started"));
    CHECK (text.contains ("  Flubsound Pro " JUCE_APPLICATION_VERSION_STRING));
    CHECK (text.contains ("through juce::Logger"));
    CHECK (text.contains ("==== Flubsound Pro stopped"));

    // A report written after the last line: the previous session crashed.
    const auto now = juce::Time::getCurrentTime();
    const auto report = options.folder.getChildFile ("crash-1-1.txt");
    REQUIRE (report.replaceWithText ("Flubsound Pro crash report\n"));
    log.setLastModificationTime (now - juce::RelativeTime::seconds (60));
    report.setLastModificationTime (now - juce::RelativeTime::seconds (30));
    {
        diagnostics::DiagnosticsSession session (options);
        REQUIRE (session.getPreviousCrashReports().size() == 1);
        EngineController controller (headless (temp));
        session.attach (controller);
        session.detach();
    }
    text = log.loadFileAsString();
    CHECK (text.contains ("The previous session crashed: crash-1-1.txt"));
    CHECK (text.contains ("Audio device: no audio device open"));
    {
        diagnostics::DiagnosticsSession session (options); // the crash was reported once
        CHECK (session.getPreviousCrashReports().isEmpty());
    }
}

TEST_CASE ("App diagnostics: Settings > Diagnostics exports one zip with system details, settings, logs and crash reports, redacted (E54)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headless (temp));
    controller.getSettings().save();
    REQUIRE (controller.getSettings().getFile().existsAsFile());

    const auto logs = temp.file ("Logs");
    CHECK (ui::SettingsDialog::describeCrashReports (logs) == "None");
    {
        diagnostics::RotatingFileLogger::Options options;
        options.folder = logs;
        diagnostics::RotatingFileLogger logger (options);
        logger.logMessage ("opened " + homePath() + "/Music/song.flac");
    }
    REQUIRE (logs.getChildFile ("crash-100-7.txt").replaceWithText ("Flubsound Pro crash report\nSignal: 11\n"));
    CHECK (ui::SettingsDialog::describeCrashReports (logs).startsWith ("1  -  the latest on "));

    const auto zipFile = temp.file ("out/diagnostics.zip");
    CHECK (ui::SettingsDialog::exportDiagnostics (controller, logs, zipFile, false) == juce::String());
    REQUIRE (zipFile.existsAsFile());

    juce::ZipFile zip (zipFile);
    juce::StringArray names;
    for (int i = 0; i < zip.getNumEntries(); ++i)
        names.add (zip.getEntry (i)->filename);
    std::cerr << "    " << names.joinIntoString (", ") << "\n";
    CHECK (names.contains ("Flubsound-diagnostics/README.txt"));
    CHECK (names.contains ("Flubsound-diagnostics/system.txt"));
    CHECK (names.contains ("Flubsound-diagnostics/settings/settings.xml"));
    CHECK (names.contains ("Flubsound-diagnostics/logs/flubsound.log"));
    CHECK (names.contains ("Flubsound-diagnostics/crashes/crash-100-7.txt"));
    for (const auto& n : names)
        CHECK (! n.endsWithIgnoreCase (".wav") && ! n.endsWithIgnoreCase (".flac"));

    const auto system = readZipEntry (zip, "system.txt");
    CHECK (system.startsWith ("Flubsound Pro " JUCE_APPLICATION_VERSION_STRING));
    CHECK (system.contains ("\nOS: ") && system.contains ("\nCPU: ") && system.contains ("\nMemory: "));
    CHECK (system.contains ("Audio device: no audio device open"));
    CHECK (system.contains ("\nStrips:\n  Game: "));
    CHECK (system.contains ("Output device profile:"));
    const auto log = readZipEntry (zip, "logs/flubsound.log");
    CHECK (log.contains ("opened ~/Music/song.flac"));
    if (homePath().length() > 1)
        CHECK (! log.contains (homePath()) && ! system.contains (homePath()));
    CHECK (readZipEntry (zip, "settings/settings.xml").contains ("PROPERTIES"));

    // The page itself.
    ui::HotkeyHooks hooks;
    hooks.isSupported = [] { return false; };
    hooks.getFailures = [] { return juce::StringArray(); };
    hooks.reRegister = [] {};
    ui::SettingsDialog settings (controller, hooks, [] (ui::MeterPalette) {}, ui::MeterPalette::Standard);
    settings.setSize (ui::SettingsDialog::kMinWidth, ui::SettingsDialog::kMinHeight);
    settings.showPage (ui::SettingsDialog::Page::Diagnostics);
    const auto image = settings.createComponentSnapshot (settings.getLocalBounds(), true, 1.0f);
    CHECK (image.isValid());
    // FLUB_APP_TEST_SNAPSHOT_DIR=<dir>: also write the page as a PNG to look at.
    if (const auto dir = juce::SystemStats::getEnvironmentVariable ("FLUB_APP_TEST_SNAPSHOT_DIR", {}); dir.isNotEmpty())
    {
        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (dir).getChildFile ("settings-diagnostics.png");
        file.getParentDirectory().createDirectory();
        file.deleteFile();
        juce::FileOutputStream out (file);
        juce::PNGImageFormat png;
        CHECK (out.openedOk() && png.writeImageToStream (image, out));
    }
}
