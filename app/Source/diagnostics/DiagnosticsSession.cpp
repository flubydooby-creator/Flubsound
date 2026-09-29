#include "DiagnosticsSession.h"

#include "CrashHandler.h"

namespace flub::app::diagnostics
{
DiagnosticsSession::DiagnosticsSession (Options o)
    : options (std::move (o))
{
    RotatingFileLogger::Options logOptions;
    logOptions.folder = options.folder;
    logOptions.maxFileBytes = options.maxLogFileBytes;
    logger = std::make_unique<RotatingFileLogger> (logOptions);

    // Reports written after the previous session's last line: that session
    // crashed (its log has no "stopped" line either).
    const auto previousEnd = logger->getPreviousSessionEnd();
    for (const auto& report : crash::findReports (options.folder))
        if (previousEnd != juce::Time() && report.getLastModificationTime() >= previousEnd)
            previousCrashes.add (report);

    logger->logMessage ("==== Flubsound Pro started\n" + describeBuildAndSystem().trimEnd());
    for (const auto& report : previousCrashes)
        logger->logMessage ("The previous session crashed: " + report.getFileName());
    juce::Logger::setCurrentLogger (logger.get());

    if (options.crashHandler)
    {
        crash::Config config;
        config.folder = options.folder;
        config.header = describeBuildAndSystem();
        crashHandlerArmed = crash::install (config);
        if (! crashHandlerArmed)
            logger->logMessage ("Crash reports are off: cannot create " + options.folder.getFullPathName());
    }
}

DiagnosticsSession::~DiagnosticsSession()
{
    detach();
    if (crashHandlerArmed)
        crash::disarm();
    logger->logMessage ("==== Flubsound Pro stopped");
    if (juce::Logger::getCurrentLogger() == logger.get())
        juce::Logger::setCurrentLogger (nullptr);
}

void DiagnosticsSession::attach (EngineController& controller)
{
    monitor = std::make_unique<DiagnosticsMonitor> (controller, [this] (const juce::String& line) { logger->logMessage (line); });
}

void DiagnosticsSession::detach()
{
    monitor.reset(); // writes the counters not logged yet
}
} // namespace flub::app::diagnostics
