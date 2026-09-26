// Flubsound Pro - UTF-8 file paths on every platform.
//
// Every path in core's file APIs (WavFile, PresetIO, DeviceProfiles) is a
// UTF-8 std::string: what JUCE's File::getFullPathName().toStdString() and
// the CLI hand over. std::fstream / std::filesystem take a narrow string in
// the process code page on Windows (ANSI), which mangles any non-ASCII
// character, so paths go through std::filesystem::path built from a
// std::u8string (interpreted as UTF-8 everywhere in C++20) instead.
#pragma once

#include <filesystem>
#include <string>

namespace flub::io
{
inline std::filesystem::path pathFromUtf8 (const std::string& utf8)
{
    return std::filesystem::path (std::u8string (utf8.begin(), utf8.end()));
}

inline std::string pathToUtf8 (const std::filesystem::path& path)
{
    const std::u8string u8 = path.u8string();
    return std::string (u8.begin(), u8.end());
}

/** Forward-slash form, for display and reports. */
inline std::string pathToUtf8Generic (const std::filesystem::path& path)
{
    const std::u8string u8 = path.generic_u8string();
    return std::string (u8.begin(), u8.end());
}
} // namespace flub::io
