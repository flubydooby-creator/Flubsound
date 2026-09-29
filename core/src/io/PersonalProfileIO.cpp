// The personal hearing profile's file (docs/11 E33; format in PersonalProfile.h).
#include "flub/engine/PersonalProfile.h"

#include "flub/io/FilePath.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace flub::personal
{
namespace
{
const char* const kEarKeys[2] = { "left", "right" };

/** A number member (0 when missing); false for any other type or a non-finite value. */
bool readNumber (const json::Value& object, const char* key, float& out, std::string& error)
{
    const json::Value& v = object[key];
    if (v.isNull())
    {
        out = 0.0f;
        return true;
    }
    if (! v.isNumber() || ! std::isfinite (v.asNumber()))
    {
        error = std::string ("\"") + key + "\" must be a number";
        return false;
    }
    out = static_cast<float> (v.asNumber());
    return true;
}
} // namespace

json::Value toJson (const PersonalProfile& profile)
{
    json::Value root;
    root.set ("format", kFormat);
    root.set ("version", kVersion);
    root.set ("enabled", profile.enabled);
    root.set ("balanceDb", static_cast<double> (profile.balanceDb));
    json::Value bands;
    for (double hz : PersonalProfile::kBandHz)
        bands.push (hz);
    root.set ("bandHz", std::move (bands));
    for (size_t e = 0; e < 2; ++e)
    {
        json::Value ear;
        ear.set ("gainDb", static_cast<double> (profile.gainDb[e]));
        json::Value gains;
        for (float g : profile.bandDb[e])
            gains.push (static_cast<double> (g));
        ear.set ("bandsDb", std::move (gains));
        root.set (kEarKeys[e], std::move (ear));
    }
    return root;
}

bool fromJson (const json::Value& root, PersonalProfile& out, std::string& error)
{
    if (! root.isObject() || root["format"].asString() != kFormat)
    {
        error = std::string ("not a personal profile (\"format\" is not \"") + kFormat + "\")";
        return false;
    }
    const json::Value& version = root["version"];
    if (! version.isNumber() || version.asNumber() < 1.0 || std::floor (version.asNumber()) != version.asNumber())
    {
        error = "\"version\" must be a whole number >= 1";
        return false;
    }
    if (version.asNumber() > kVersion)
    {
        error = "the profile was written by a newer Flubsound (version " + std::to_string (static_cast<long long> (version.asNumber())) + ")";
        return false;
    }
    const json::Value& hz = root["bandHz"];
    if (! hz.isNull())
    {
        const auto& list = hz.asArray();
        bool same = hz.isArray() && list.size() == PersonalProfile::kBandHz.size();
        for (size_t b = 0; same && b < list.size(); ++b)
            same = list[b].isNumber() && list[b].asNumber() == PersonalProfile::kBandHz[b];
        if (! same)
        {
            error = "\"bandHz\" must list 250, 500, 1000, 2000, 3000, 4000, 6000 and 8000 Hz";
            return false;
        }
    }

    PersonalProfile p;
    const json::Value& enabled = root["enabled"];
    if (! enabled.isNull() && ! enabled.isBool())
    {
        error = "\"enabled\" must be true or false";
        return false;
    }
    p.enabled = enabled.asBool (false);
    if (! readNumber (root, "balanceDb", p.balanceDb, error))
        return false;
    for (size_t e = 0; e < 2; ++e)
    {
        const json::Value& ear = root[kEarKeys[e]];
        if (ear.isNull())
            continue;
        if (! ear.isObject())
        {
            error = std::string ("\"") + kEarKeys[e] + "\" must be an object";
            return false;
        }
        if (! readNumber (ear, "gainDb", p.gainDb[e], error))
        {
            error = std::string (kEarKeys[e]) + ": " + error;
            return false;
        }
        const json::Value& gains = ear["bandsDb"];
        if (gains.isNull())
            continue;
        const auto& list = gains.asArray();
        if (! gains.isArray() || list.size() != static_cast<size_t> (PersonalProfile::kNumBands))
        {
            error = std::string (kEarKeys[e]) + ": \"bandsDb\" must hold 8 numbers";
            return false;
        }
        for (size_t b = 0; b < list.size(); ++b)
        {
            if (! list[b].isNumber() || ! std::isfinite (list[b].asNumber()))
            {
                error = std::string (kEarKeys[e]) + ": \"bandsDb\" must hold 8 numbers";
                return false;
            }
            p.bandDb[e][b] = static_cast<float> (list[b].asNumber());
        }
    }
    out = p.sanitised();
    return true;
}

bool load (const std::string& path, PersonalProfile& out, std::string& error)
{
    std::ifstream f (io::pathFromUtf8 (path), std::ios::binary);
    if (! f)
    {
        error = "cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    json::Value v;
    if (! json::parse (ss.str(), v, error))
    {
        error = path + ":" + error;
        return false;
    }
    if (! fromJson (v, out, error))
    {
        error = path + ": " + error;
        return false;
    }
    return true;
}

bool save (const std::string& path, const PersonalProfile& profile, std::string& error)
{
    const std::filesystem::path target = io::pathFromUtf8 (path);
    std::filesystem::path temporary = target;
    temporary += ".tmp";
    {
        std::ofstream f (temporary, std::ios::binary | std::ios::trunc);
        if (! f)
        {
            error = "cannot write " + path;
            return false;
        }
        f << json::write (toJson (profile.sanitised()), 2) << "\n";
        f.flush();
        if (! f)
        {
            error = "cannot write " + path;
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename (temporary, target, ec); // replaces an existing profile (MoveFileEx on Windows)
    if (ec)
    {
        std::filesystem::remove (temporary, ec);
        error = "cannot replace " + path;
        return false;
    }
    return true;
}
} // namespace flub::personal
