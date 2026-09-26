// flubsound-cli - UTF-8 command line, environment and console on Windows.
//
// Everything the CLI passes around is UTF-8 (core's file APIs expect it, see
// flub/io/FilePath.h). On Windows the narrow argv and getenv() use the ANSI
// code page, which cannot represent most non-ASCII file names, so the wide
// versions are read and converted; the console is switched to UTF-8 output.
// Elsewhere the narrow strings are already UTF-8 and these are pass-throughs.
#pragma once

#include <string>
#include <vector>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <shellapi.h>
#else
    #include <cstdlib>
#endif

namespace flub::cli
{
#if defined(_WIN32)
inline std::string wideToUtf8 (const wchar_t* wide)
{
    if (wide == nullptr || *wide == L'\0')
        return {};
    const int bytes = WideCharToMultiByte (CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1)
        return {};
    std::string out (static_cast<size_t> (bytes), '\0');
    WideCharToMultiByte (CP_UTF8, 0, wide, -1, out.data(), bytes, nullptr, nullptr);
    out.resize (static_cast<size_t> (bytes - 1)); // drop the terminator
    return out;
}

inline std::wstring utf8ToWide (const std::string& utf8)
{
    if (utf8.empty())
        return {};
    const int chars = MultiByteToWideChar (CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (chars <= 1)
        return {};
    std::wstring out (static_cast<size_t> (chars), L'\0');
    MultiByteToWideChar (CP_UTF8, 0, utf8.c_str(), -1, out.data(), chars);
    out.resize (static_cast<size_t> (chars - 1));
    return out;
}
#endif

/** argv[1..] as UTF-8 strings. */
inline std::vector<std::string> utf8Arguments (int argc, char** argv)
{
    std::vector<std::string> args;
#if defined(_WIN32)
    int count = 0;
    if (LPWSTR* wide = CommandLineToArgvW (GetCommandLineW(), &count))
    {
        for (int i = 1; i < count; ++i)
            args.push_back (wideToUtf8 (wide[i]));
        LocalFree (wide);
        return args;
    }
#endif
    for (int i = 1; i < argc; ++i)
        args.emplace_back (argv[i]);
    return args;
}

/** Environment variable as UTF-8 ("" when unset). */
inline std::string utf8Environment (const char* name)
{
#if defined(_WIN32)
    const std::wstring wideName = utf8ToWide (name);
    const DWORD needed = GetEnvironmentVariableW (wideName.c_str(), nullptr, 0); // incl. terminator
    if (needed == 0)
        return {};
    std::wstring value (needed, L'\0');
    const DWORD written = GetEnvironmentVariableW (wideName.c_str(), value.data(), needed);
    if (written == 0 || written >= needed)
        return {};
    value.resize (written);
    return wideToUtf8 (value.c_str());
#else
    const char* value = std::getenv (name);
    return value != nullptr ? std::string (value) : std::string();
#endif
}

/** Lets a Windows console show the UTF-8 the CLI prints (no-op elsewhere). */
inline void useUtf8Console()
{
#if defined(_WIN32)
    SetConsoleOutputCP (CP_UTF8);
#endif
}
} // namespace flub::cli
