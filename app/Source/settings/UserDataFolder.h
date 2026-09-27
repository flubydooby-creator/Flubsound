// Flubsound Pro - the per-user data folder shared by the desktop app and the
// plug-in: settings, user presets and the device-profile override live in
// <userDataFolder()>/..., i.e.
//   Windows : %APPDATA%\Flubsound
//   macOS   : ~/Library/Application Support/Flubsound (via JUCE's app data dir)
//   Linux   : $XDG_CONFIG_HOME/Flubsound, else ~/.config/Flubsound
//   any OS  : $FLUB_USER_DATA_DIR when set to an absolute path
// Header-only so the plug-in can use it without linking the app sources.
#pragma once

#include <juce_core/juce_core.h>

#include <cstdlib>

namespace flub::app
{
inline juce::File userDataFolder()
{
    // Explicit override on every OS (tests, portable installs): an absolute
    // path used as-is instead of <OS default>/Flubsound. Read through JUCE,
    // which uses GetEnvironmentVariableW on Windows: the narrow CRT copy of
    // the environment is in the ANSI code page, not UTF-8, so std::getenv +
    // fromUTF8 would garble (or miss) a non-ASCII path there.
    if (const auto dir = juce::SystemStats::getEnvironmentVariable ("FLUB_USER_DATA_DIR", {}); juce::File::isAbsolutePath (dir))
        return juce::File (dir);

   #if JUCE_LINUX || JUCE_BSD
    // JUCE 9 resolves userApplicationDataDirectory from ~/.config/user-dirs.dirs
    // and never reads the XDG_CONFIG_HOME environment variable. The XDG Base
    // Directory spec only honours absolute values.
    if (const char* xdg = std::getenv ("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] == '/')
        return juce::File (juce::String::fromUTF8 (xdg)).getChildFile ("Flubsound");
   #endif
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Flubsound");
}
} // namespace flub::app
