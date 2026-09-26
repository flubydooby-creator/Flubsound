#include "FactoryPresets.h"

#include "flub/engine/Parameters.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <system_error>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#elif defined(__APPLE__)
    #include <mach-o/dyld.h>
#endif

#ifndef FLUB_SOURCE_PRESET_DIR
    #define FLUB_SOURCE_PRESET_DIR ""
#endif

namespace fs = std::filesystem;

namespace flub::cli
{
namespace
{
std::string toLower (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    return s;
}

/** "Punchy Pop!" -> "punchypop": the loose key used for forgiving matches. */
std::string looseKey (const std::string& s)
{
    std::string k;
    for (unsigned char c : s)
        if (std::isalnum (c))
            k += static_cast<char> (std::tolower (c));
    return k;
}

/** "competitive-fps.flubpreset.json" -> "competitive-fps". */
std::string presetStem (const fs::path& file)
{
    std::string stem = file.stem().string();
    const std::string suffix = ".flubpreset";
    if (stem.size() > suffix.size() && toLower (stem.substr (stem.size() - suffix.size())) == suffix)
        stem.resize (stem.size() - suffix.size());
    return stem;
}

/** getenv without MSVC's C4996 deprecation warning. */
std::string environmentVariable (const char* name)
{
#if defined(_MSC_VER)
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s (&value, &length, name) != 0 || value == nullptr)
        return {};
    std::string result (value);
    std::free (value);
    return result;
#else
    const char* value = std::getenv (name);
    return value != nullptr ? std::string (value) : std::string();
#endif
}

bool isDirectory (const fs::path& p)
{
    std::error_code ec;
    return ! p.empty() && fs::is_directory (p, ec);
}

std::string describeCandidates (const std::vector<const FactoryPresetEntry*>& list)
{
    std::string s;
    for (const auto* e : list)
        s += "\n    " + e->name + "  [" + e->category + "]  (" + e->file.filename().string() + ")";
    return s;
}
} // namespace

fs::path executablePath()
{
    std::error_code ec;
#if defined(_WIN32)
    std::wstring buffer (32768, L'\0');
    const DWORD length = GetModuleFileNameW (nullptr, buffer.data(), static_cast<DWORD> (buffer.size()));
    if (length == 0 || length >= buffer.size())
        return {};
    buffer.resize (length);
    return fs::path (buffer);
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath (nullptr, &size);
    std::string buffer (size, '\0');
    if (_NSGetExecutablePath (buffer.data(), &size) != 0)
        return {};
    buffer.resize (std::char_traits<char>::length (buffer.c_str()));
    const auto canonical = fs::weakly_canonical (fs::path (buffer), ec);
    return ec ? fs::path (buffer) : canonical;
#elif defined(__linux__)
    const auto p = fs::read_symlink ("/proc/self/exe", ec);
    return ec ? fs::path() : p;
#else
    return {};
#endif
}

std::vector<fs::path> presetSearchPath (const std::string& explicitDir)
{
    std::vector<fs::path> dirs;
    if (! explicitDir.empty())
    {
        dirs.emplace_back (explicitDir);
        return dirs; // an explicit folder is the only candidate
    }

    if (const auto env = environmentVariable ("FLUBSOUND_PRESET_DIR"); ! env.empty())
        dirs.emplace_back (env);

    const fs::path exe = executablePath();
    if (! exe.empty())
    {
        const fs::path exeDir = exe.parent_path();
        // Next to the executable and below a few parents (covers build trees
        // such as <repo>/build/tools/flubsound-cli[/Release] -> <repo>/presets/factory).
        fs::path d = exeDir;
        for (int level = 0; level < 5 && ! d.empty(); ++level)
        {
            dirs.push_back (d / "presets" / "factory");
            if (d == d.parent_path())
                break;
            d = d.parent_path();
        }
        dirs.push_back (exeDir.parent_path() / "share" / "flubsound" / "presets" / "factory"); // installed
        dirs.push_back (exeDir.parent_path() / "Resources" / "presets" / "factory");            // macOS bundle
    }

    const std::string sourceDir = FLUB_SOURCE_PRESET_DIR;
    if (! sourceDir.empty())
        dirs.emplace_back (sourceDir);
    return dirs;
}

std::optional<fs::path> findFactoryPresetDir (const std::string& explicitDir)
{
    for (const auto& d : presetSearchPath (explicitDir))
        if (isDirectory (d))
            return d;
    return std::nullopt;
}

std::vector<FactoryPresetEntry> scanPresetDir (const fs::path& dir, std::vector<std::string>& problems)
{
    std::vector<FactoryPresetEntry> entries;
    std::error_code ec;
    const auto options = fs::directory_options::skip_permission_denied;
    // Top level only, like every other consumer (the app's BinaryData glob,
    // the install rule, tests/test_factory_presets.cpp): a preset in a
    // sub-folder would be listed here without ever having been validated.
    for (fs::directory_iterator it (dir, options, ec), end; ! ec && it != end; it.increment (ec))
    {
        std::error_code fileEc;
        if (! it->is_regular_file (fileEc) || toLower (it->path().extension().string()) != ".json")
            continue;

        preset::Preset p;
        std::string error;
        if (! preset::load (it->path().string(), p, error))
        {
            // Parse errors already start with the path; semantic ones do not.
            const std::string path = it->path().string();
            problems.push_back (error.rfind (path, 0) == 0 ? error : path + ": " + error);
            continue;
        }

        FactoryPresetEntry e;
        e.name = p.name.empty() ? presetStem (it->path()) : p.name;
        e.category = p.category;
        e.author = p.author;
        e.description = p.description;
        e.tags = p.tags;
        e.file = it->path();
        const auto& modeInfo = param::layout()[static_cast<size_t> (param::Mode)];
        const auto modeIndex = static_cast<size_t> (std::lround (p.values[static_cast<size_t> (param::Mode)]));
        e.mode = modeIndex < modeInfo.choices.size() ? modeInfo.choices[modeIndex] : "?";
        entries.push_back (std::move (e));
    }
    if (ec)
        problems.push_back (dir.string() + ": " + ec.message());

    std::sort (entries.begin(), entries.end(), [] (const FactoryPresetEntry& a, const FactoryPresetEntry& b) {
        const auto ca = toLower (a.category), cb = toLower (b.category);
        return ca != cb ? ca < cb : toLower (a.name) < toLower (b.name);
    });
    return entries;
}

bool resolvePreset (const std::string& spec, const std::string& explicitPresetDir, preset::Preset& out, std::string& resolvedFrom,
                    std::string& error)
{
    if (spec.empty())
    {
        error = "empty --preset value";
        return false;
    }

    // 1. A preset file on disk.
    std::error_code ec;
    const fs::path asPath (spec);
    if (fs::is_regular_file (asPath, ec))
    {
        if (! preset::load (asPath.string(), out, error))
            return false;
        resolvedFrom = "preset file " + asPath.string();
        return true;
    }
    const bool looksLikePath = spec.find ('/') != std::string::npos || spec.find ('\\') != std::string::npos
                               || toLower (asPath.extension().string()) == ".json";
    if (looksLikePath)
    {
        error = "preset file not found: " + spec;
        return false;
    }

    // 2. A factory preset name.
    const auto dir = findFactoryPresetDir (explicitPresetDir);
    if (! dir)
    {
        error = "'" + spec + "' is not a file and no factory preset folder was found. Searched:";
        for (const auto& d : presetSearchPath (explicitPresetDir))
            error += "\n    " + d.string();
        error += "\nUse --preset-dir <dir>, set FLUBSOUND_PRESET_DIR, or pass a preset file path.";
        return false;
    }

    std::vector<std::string> problems;
    const auto entries = scanPresetDir (*dir, problems);
    if (entries.empty())
    {
        error = "no factory presets found in " + dir->string();
        return false;
    }

    const std::string lowerSpec = toLower (spec);
    const std::string looseSpec = looseKey (spec);

    // Matching stages from strict to forgiving; the first stage with any hit
    // decides (and must be unambiguous).
    using Matcher = bool (*) (const FactoryPresetEntry&, const std::string&, const std::string&);
    const Matcher stages[] = {
        [] (const FactoryPresetEntry& e, const std::string& l, const std::string&) { return toLower (e.name) == l; },
        [] (const FactoryPresetEntry& e, const std::string& l, const std::string&) { return toLower (presetStem (e.file)) == l; },
        [] (const FactoryPresetEntry& e, const std::string&, const std::string& k) {
            return ! k.empty() && (looseKey (e.name) == k || looseKey (presetStem (e.file)) == k);
        },
        [] (const FactoryPresetEntry& e, const std::string&, const std::string& k) {
            return ! k.empty() && (looseKey (e.name).rfind (k, 0) == 0 || looseKey (presetStem (e.file)).rfind (k, 0) == 0);
        },
        [] (const FactoryPresetEntry& e, const std::string&, const std::string& k) {
            return ! k.empty() && (looseKey (e.name).find (k) != std::string::npos || looseKey (presetStem (e.file)).find (k) != std::string::npos);
        },
    };

    for (const auto matches : stages)
    {
        std::vector<const FactoryPresetEntry*> hits;
        for (const auto& e : entries)
            if (matches (e, lowerSpec, looseSpec))
                hits.push_back (&e);
        if (hits.empty())
            continue;
        if (hits.size() > 1)
        {
            error = "preset name '" + spec + "' is ambiguous; candidates:" + describeCandidates (hits);
            return false;
        }
        if (! preset::load (hits.front()->file.string(), out, error))
            return false;
        resolvedFrom = "factory preset '" + hits.front()->name + "' (" + hits.front()->file.string() + ")";
        return true;
    }

    std::vector<const FactoryPresetEntry*> all;
    for (const auto& e : entries)
        all.push_back (&e);
    error = "unknown preset '" + spec + "' (not a file, not a factory preset in " + dir->string() + "). Available:"
            + describeCandidates (all);
    return false;
}
} // namespace flub::cli
