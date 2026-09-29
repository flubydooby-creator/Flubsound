#include "DiagnosticLog.h"

#include "settings/UserDataFolder.h"

namespace flub::app::diagnostics
{
namespace
{
bool isWordChar (juce::juce_wchar c) noexcept
{
    return juce::CharacterFunctions::isLetterOrDigit (c) || c == '_' || c == '-';
}

/** Replaces every case-insensitive occurrence of `word` that is not part of a
    longer word (letters, digits, '_' or '-' on either side). */
juce::String replaceWholeWord (const juce::String& text, const juce::String& word, const juce::String& replacement)
{
    if (word.length() < 3)
        return text;
    juce::String result;
    int from = 0;
    for (;;)
    {
        const int at = text.indexOfIgnoreCase (from, word);
        if (at < 0)
            break;
        const int end = at + word.length();
        const bool startsWord = at == 0 || ! isWordChar (text[at - 1]);
        const bool endsWord = end >= text.length() || ! isWordChar (text[end]);
        result << text.substring (from, at) << (startsWord && endsWord ? replacement : text.substring (at, end));
        from = end;
    }
    return result + text.substring (from);
}
} // namespace

juce::File logFolder()
{
    return userDataFolder().getChildFile ("Logs");
}

juce::String redactPersonalData (const juce::String& text, const juce::String& homePath, const juce::String& userName,
                                 const juce::String& computerName)
{
    auto result = text;
    // The home folder first (it usually contains the login name), in both
    // slash styles: Windows paths show up as C:\Users\x and as C:/Users/x.
    if (homePath.length() > 1)
        for (const auto& path : { homePath, homePath.replaceCharacter ('\\', '/'), homePath.replaceCharacter ('/', '\\') })
            result = result.replace (path, "~", true);
    result = replaceWholeWord (result, userName, "<user>");
    result = replaceWholeWord (result, computerName, "<computer>");
    return result;
}

juce::String redactPersonalData (const juce::String& text)
{
    return redactPersonalData (text, juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName(),
                               juce::SystemStats::getLogonName(), juce::SystemStats::getComputerName());
}

//==============================================================================
RotatingFileLogger::RotatingFileLogger (Options o)
    : options (std::move (o)),
      homePath (juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName()),
      userName (juce::SystemStats::getLogonName()),
      computerName (juce::SystemStats::getComputerName())
{
    const auto current = getCurrentFile();
    if (current.existsAsFile())
        previousSessionEnd = current.getLastModificationTime();
}

RotatingFileLogger::~RotatingFileLogger() = default;

juce::File RotatingFileLogger::getCurrentFile() const
{
    return options.folder.getChildFile (options.baseName + ".log");
}

juce::File RotatingFileLogger::rotatedFile (int index) const
{
    return options.folder.getChildFile (options.baseName + "." + juce::String (index) + ".log");
}

juce::Array<juce::File> RotatingFileLogger::getLogFiles() const
{
    juce::Array<juce::File> files;
    if (getCurrentFile().existsAsFile())
        files.add (getCurrentFile());
    for (int i = 1; i < options.keepFiles; ++i)
        if (rotatedFile (i).existsAsFile())
            files.add (rotatedFile (i));
    return files;
}

void RotatingFileLogger::rotate()
{
    stream.reset(); // Windows cannot rename an open file
    const int keep = juce::jmax (1, options.keepFiles);
    if (keep == 1)
    {
        getCurrentFile().deleteFile();
        return;
    }
    rotatedFile (keep - 1).deleteFile();
    for (int i = keep - 2; i >= 1; --i)
        if (rotatedFile (i).existsAsFile())
            rotatedFile (i).moveFileTo (rotatedFile (i + 1));
    getCurrentFile().moveFileTo (rotatedFile (1));
}

void RotatingFileLogger::logMessage (const juce::String& message)
{
    const auto now = juce::Time::getCurrentTime();
    const auto stamp = now.formatted ("%Y-%m-%d %H:%M:%S.") + juce::String (now.getMilliseconds()).paddedLeft ('0', 3) + "  ";
    const auto text = options.redact ? redactPersonalData (message, homePath, userName, computerName) : message;

    juce::String block;
    for (const auto& line : juce::StringArray::fromLines (text.trimEnd()))
        block << stamp << line << "\n";
    if (block.isEmpty())
        block << stamp << "\n";
    const auto bytes = static_cast<juce::int64> (block.getNumBytesAsUTF8());

    const juce::ScopedLock sl (lock);
    const auto current = getCurrentFile();
    const auto size = stream != nullptr ? stream->getPosition() : (current.existsAsFile() ? current.getSize() : 0);
    if (size > 0 && size + bytes > options.maxFileBytes)
        rotate();

    if (stream == nullptr)
    {
        if (! options.folder.createDirectory())
            return;
        stream = std::make_unique<juce::FileOutputStream> (current); // appends
        if (stream->failedToOpen())
        {
            stream.reset();
            return;
        }
    }
    stream->writeText (block, false, false, nullptr);
    stream->flush(); // a crash right after must not lose the line
}

//==============================================================================
juce::String describeBuildAndSystem()
{
    juce::String s;
    s << "Flubsound Pro " << JUCE_APPLICATION_VERSION_STRING
     #if JUCE_DEBUG
      << " (debug build)"
     #endif
      << "\n";
    s << "JUCE: " << juce::SystemStats::getJUCEVersion() << "\n";
    s << "OS: " << juce::SystemStats::getOperatingSystemName() << (juce::SystemStats::isOperatingSystem64Bit() ? " (64-bit)" : "")
      << "\n";
    s << "CPU: " << juce::SystemStats::getCpuVendor() << " " << juce::SystemStats::getCpuModel() << ", "
      << juce::SystemStats::getNumPhysicalCpus() << " cores / " << juce::SystemStats::getNumCpus() << " threads, "
      << juce::SystemStats::getCpuSpeedInMegahertz() << " MHz"
      << (juce::SystemStats::hasAVX2() ? ", AVX2" : juce::SystemStats::hasSSE41() ? ", SSE4.1" : "")
      << (juce::SystemStats::hasNeon() ? ", NEON" : "") << "\n";
    s << "Memory: " << juce::SystemStats::getMemorySizeInMegabytes() << " MB\n";
    s << "Language: " << juce::SystemStats::getUserLanguage() << "-" << juce::SystemStats::getUserRegion() << "\n";
    return s;
}
} // namespace flub::app::diagnostics
