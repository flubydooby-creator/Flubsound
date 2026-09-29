// Flubsound Pro - the diagnostics of one app session (docs/11 E54), owned by
// FlubsoundApplication in interactive mode (not in headless screenshot runs):
//
//   1. opens the rotating log (DiagnosticLog.h) and makes it juce::Logger's
//      current logger, writes a start line with versions and machine and
//      notes crash reports the previous session left;
//   2. arms the crash handler (CrashHandler.h), reports into the same folder;
//   3. attach(): logs engine events (DiagnosticsMonitor.h) until detach();
//   4. the destructor writes a stop line (a log without one ended in a crash
//      or was killed), then removes the logger and disarms the handler.
#pragma once

#include "DiagnosticLog.h"
#include "DiagnosticsMonitor.h"

#include <memory>

namespace flub::app
{
class EngineController;
}

namespace flub::app::diagnostics
{
class DiagnosticsSession
{
public:
    struct Options
    {
        juce::File folder = logFolder();
        bool crashHandler = true;
        juce::int64 maxLogFileBytes = 512 * 1024;
    };

    DiagnosticsSession() : DiagnosticsSession (Options()) {}
    explicit DiagnosticsSession (Options options);
    ~DiagnosticsSession();

    void attach (EngineController& controller);
    void detach();

    void log (const juce::String& message) { logger->logMessage (message); }
    RotatingFileLogger& getLogger() noexcept { return *logger; }
    /** Crash reports newer than the previous session's last log line. */
    const juce::Array<juce::File>& getPreviousCrashReports() const noexcept { return previousCrashes; }

private:
    const Options options;
    std::unique_ptr<RotatingFileLogger> logger;
    std::unique_ptr<DiagnosticsMonitor> monitor;
    juce::Array<juce::File> previousCrashes;
    bool crashHandlerArmed = false;

    JUCE_DECLARE_NON_COPYABLE (DiagnosticsSession)
};
} // namespace flub::app::diagnostics
