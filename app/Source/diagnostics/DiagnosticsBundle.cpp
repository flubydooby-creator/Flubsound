#include "DiagnosticsBundle.h"

#include "CrashHandler.h"
#include "DiagnosticLog.h"
#include "DiagnosticsMonitor.h"
#include "engine/EngineController.h"

namespace flub::app::diagnostics
{
namespace
{
const juce::String kRoot ("Flubsound-diagnostics/");

bool isText (const juce::File& file)
{
    return file.hasFileExtension ("txt;log;xml;json;settings;cfg;ini");
}

void addText (juce::ZipFile::Builder& zip, const juce::String& text, const juce::String& path)
{
    const auto utf8 = redactPersonalData (text).toStdString();
    zip.addEntry (std::make_unique<juce::MemoryInputStream> (utf8.data(), utf8.size(), true), 9, kRoot + path, juce::Time::getCurrentTime());
}

void addFile (juce::ZipFile::Builder& zip, const juce::File& file, const juce::String& folder)
{
    if (! file.existsAsFile())
        return;
    const auto path = folder + file.getFileName();
    if (isText (file))
    {
        const auto utf8 = redactPersonalData (file.loadFileAsString()).toStdString();
        zip.addEntry (std::make_unique<juce::MemoryInputStream> (utf8.data(), utf8.size(), true), 9, kRoot + path, file.getLastModificationTime());
    }
    else
    {
        zip.addFile (file, 6, kRoot + path);
    }
}

juce::String readme()
{
    return "Flubsound Pro diagnostics\n"
           "=========================\n\n"
           "Made by Settings > Diagnostics > Export diagnostics. Attach it to a report.\n\n"
           "  system.txt   versions, operating system, CPU and memory, the audio device\n"
           "               and its state, the device lists, the strips and their counters\n"
           "  settings/    the settings file (devices, routing, hotkeys, profile rules)\n"
           "               and the route journal\n"
           "  logs/        the diagnostic log: start and stop, device changes and errors,\n"
           "               glitch, overload and safety counts\n"
           "  crashes/     the newest crash reports, if there were crashes\n\n"
           "No audio and no preset files. In the text files the home folder, login name\n"
           "and computer name are replaced by ~, <user> and <computer>. A Windows crash\n"
           "dump (.dmp) holds the program's stack memory at the crash and is included\n"
           "as it is.\n";
}
} // namespace

juce::String defaultBundleName()
{
    return "Flubsound-diagnostics-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".zip";
}

juce::String writeBundle (const BundleSources& sources, const juce::File& zipFile)
{
    juce::ZipFile::Builder zip;
    addText (zip, readme(), "README.txt");
    addText (zip, sources.systemReport, "system.txt");
    for (const auto& f : sources.settingsFiles)
        addFile (zip, f, "settings/");
    for (const auto& f : sources.logFiles)
        addFile (zip, f, "logs/");
    if (sources.crashFolder.isDirectory())
    {
        const auto reports = crash::findReports (sources.crashFolder);
        for (int i = 0; i < juce::jmin (reports.size(), sources.maxCrashReports); ++i)
        {
            addFile (zip, reports[i], "crashes/");
            addFile (zip, reports[i].withFileExtension ("dmp"), "crashes/");
        }
    }

    if (! zipFile.getParentDirectory().createDirectory())
        return "Cannot create the folder " + zipFile.getParentDirectory().getFullPathName();
    juce::TemporaryFile temp (zipFile);
    {
        juce::FileOutputStream out (temp.getFile());
        if (out.failedToOpen())
            return "Cannot write " + zipFile.getFullPathName() + ": " + out.getStatus().getErrorMessage();
        if (! zip.writeToStream (out, nullptr))
            return "Writing the zip failed";
        out.flush();
        if (out.getStatus().failed())
            return "Cannot write " + zipFile.getFullPathName() + ": " + out.getStatus().getErrorMessage();
    }
    if (! temp.overwriteTargetFileWithTemporary())
        return "Cannot replace " + zipFile.getFullPathName();
    return {};
}

juce::String describeEngine (EngineController& controller, bool listDevices)
{
    juce::String s = describeBuildAndSystem();
    const auto snapshot = takeSnapshot (controller);
    const auto status = controller.getStatus();
    s << "\nAudio device: " << describeDevice (snapshot) << "\n";
    if (snapshot.deviceError.isNotEmpty())
        s << "Device error: " << snapshot.deviceError << "\n";
    s << "Engine: " << (status.running ? "running" : "not running") << ", " << status.numInputChannels << " in / "
      << status.numOutputChannels << " out, CPU " << juce::roundToInt (status.cpuLoad * 100.0) << " %, "
      << (status.xruns >= 0 ? juce::String (status.xruns) + " xruns, " : juce::String ("xruns not reported, ")) << status.glitches
      << " glitches, " << static_cast<juce::int64> (status.callbacks) << " callbacks\n";
    const auto& overload = controller.getOverloadState();
    s << "Overload watchdog: " << (overload.overloaded ? "OVERLOADED" : "ok") << ", " << static_cast<juce::int64> (overload.episodes)
      << " episodes this session, peak load " << juce::roundToInt (overload.peakLoad * 100.0) << " %\n";

    s << "\nStrips:\n";
    for (const auto& strip : snapshot.strips)
    {
        const int index = controller.findStrip (strip.name);
        s << "  " << strip.name << ": " << (index >= 0 ? controller.getStripChannels (index) : 0) << " channels, "
          << (index >= 0 && controller.isStripActive (index) ? "active" : "idle") << ", " << static_cast<juce::int64> (strip.safetyClips)
          << " safety clips, " << static_cast<juce::int64> (strip.droppedBlocks) << " blocks dropped (NaN / Inf), "
          << static_cast<juce::int64> (strip.corruptSamples) << " samples muted (> +24 dBFS)\n";
    }

    if (listDevices)
    {
        s << "\nDevices:\n";
        for (auto* type : controller.getDeviceManager().getAvailableDeviceTypes())
        {
            s << "  " << type->getTypeName() << "\n";
            s << "    outputs: " << type->getDeviceNames (false).joinIntoString (" | ") << "\n";
            if (type->hasSeparateInputsAndOutputs())
                s << "    inputs:  " << type->getDeviceNames (true).joinIntoString (" | ") << "\n";
        }
    }
    return s;
}
} // namespace flub::app::diagnostics
