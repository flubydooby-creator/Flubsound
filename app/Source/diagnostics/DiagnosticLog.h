// Flubsound Pro - the diagnostic log (docs/11 E54): a rotating text file in
// the user data folder that a user can send with a report.
//
//   <userDataFolder()>/Logs/flubsound.log     the current session(s)
//   <userDataFolder()>/Logs/flubsound.1.log   older, .2 older still
//   <userDataFolder()>/Logs/crash-*.txt/.dmp  crash reports (CrashHandler.h)
//
// What goes in: start / stop with versions, device opens, changes and
// errors, glitch and overload counts, the safety counters (DiagnosticsMonitor)
// and the messages the app printed on stderr. Never audio, never preset
// contents. Every line passes redactPersonalData(): the home folder becomes
// "~", the login and computer names "<user>" / "<computer>".
//
// Threads: RotatingFileLogger::logMessage may be called from any thread
// except the audio thread (it locks and writes a file). Nothing on the audio
// thread logs; the monitor reads the engine's atomic counters from a timer.
#pragma once

#include <juce_core/juce_core.h>

namespace flub::app::diagnostics
{
/** <userDataFolder()>/Logs: the log files and the crash reports. */
juce::File logFolder();

/** Replaces `homePath` (and the same path with the other slash style) by
    "~", then whole-word `userName` by "<user>" and `computerName` by
    "<computer>" (case-insensitive; names shorter than 3 characters are left
    alone, they would match ordinary words). */
juce::String redactPersonalData (const juce::String& text, const juce::String& homePath, const juce::String& userName,
                                 const juce::String& computerName);
/** The same with this machine's home folder, login and computer name. */
juce::String redactPersonalData (const juce::String& text);

class RotatingFileLogger final : public juce::Logger
{
public:
    struct Options
    {
        juce::File folder;                 // created on first write
        juce::String baseName = "flubsound";
        juce::int64 maxFileBytes = 512 * 1024; // rotate before a line would pass this
        int keepFiles = 3;                 // the current file plus .1 .. .(keepFiles - 1)
        bool redact = true;                // redactPersonalData() on every line
    };

    explicit RotatingFileLogger (Options options);
    ~RotatingFileLogger() override;

    /** Writes "yyyy-mm-dd hh:mm:ss.mmm  <message>" (one line per message
        line), rotating first when the file would grow past maxFileBytes. */
    void logMessage (const juce::String& message) override;

    /** The file being written (it may not exist before the first line). */
    juce::File getCurrentFile() const;
    /** Existing log files, newest first. */
    juce::Array<juce::File> getLogFiles() const;
    /** The last modification time the current file had when this logger was
        created (the end of the previous session), or a null Time. */
    juce::Time getPreviousSessionEnd() const noexcept { return previousSessionEnd; }

private:
    juce::File rotatedFile (int index) const;
    void rotate();

    const Options options;
    juce::Time previousSessionEnd;
    juce::CriticalSection lock;
    std::unique_ptr<juce::FileOutputStream> stream;
    juce::String homePath, userName, computerName;

    JUCE_DECLARE_NON_COPYABLE (RotatingFileLogger)
};

/** One "key: value" block describing the build and the machine (versions,
    OS, CPU, memory); used for the session header and the diagnostics bundle.
    No personal data. */
juce::String describeBuildAndSystem();
} // namespace flub::app::diagnostics
