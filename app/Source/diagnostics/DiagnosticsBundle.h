// Flubsound Pro - "Export diagnostics" (Settings > Diagnostics, docs/11 E54):
// one zip a user can attach to a report.
//
//   Flubsound-diagnostics/README.txt     what is in it and what is not
//   Flubsound-diagnostics/system.txt     versions, OS, CPU, the audio device,
//                                        the device lists, strips, counters
//   Flubsound-diagnostics/settings/...   the settings file and the route
//                                        journal next to it
//   Flubsound-diagnostics/logs/...       the diagnostic log files
//   Flubsound-diagnostics/crashes/...    the newest crash reports (+ .dmp)
//
// No audio and no preset files. Text files pass through redactPersonalData
// (home folder, login and computer name); a Windows minidump holds the
// crashing program's stack memory and is copied as it is.
#pragma once

#include <juce_core/juce_core.h>

namespace flub::app
{
class EngineController;
}

namespace flub::app::diagnostics
{
struct BundleSources
{
    juce::String systemReport;          // system.txt
    juce::Array<juce::File> settingsFiles; // settings/<name>
    juce::Array<juce::File> logFiles;   // logs/<name>
    juce::File crashFolder;             // crashes/: its newest crash-* reports
    int maxCrashReports = 3;
};

/** Writes the zip (replacing `zipFile` only once it is complete). Returns an
    empty string on success, else what failed. */
juce::String writeBundle (const BundleSources& sources, const juce::File& zipFile);

/** "Flubsound-diagnostics-<yyyymmdd-hhmmss>.zip". */
juce::String defaultBundleName();

/** Versions and machine (describeBuildAndSystem), the audio device and its
    state, counters and strips; with `listDevices` also every device type's
    input and output names (this may scan the devices). */
juce::String describeEngine (EngineController& controller, bool listDevices);
} // namespace flub::app::diagnostics
