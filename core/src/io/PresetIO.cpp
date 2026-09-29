#include "flub/io/PresetIO.h"

#include "flub/io/FilePath.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <system_error>

namespace flub::preset
{
using namespace param;

namespace
{
constexpr int kAppState[] = { BypassAll, LoudnessMatchBypass, LatencyProfile };

// Newest minor of each OLDER major this build knows (index = major). A file
// of an older major with a newer minor than listed here loads with a warning.
constexpr int kKnownMinor[] = { 0, 0, 0 };

// Frozen defaults per older major (docs/11 E01 / E52). A version-N file
// omitted values equal to the version-N defaults, so the N -> N+1 migration
// writes the version-N default for each of these keys the file does not
// carry: a default change never re-voices a sparse preset saved before it.
// Only keys whose default changed in the next major are listed (a key the
// next step does not list keeps what the earlier step wrote). The next
// default change bumps the major, adds kV3Defaults with the old values and a
// 3 -> 4 step in migrations() (tests/test_presets_golden.cpp fails until then).
struct FrozenDefault
{
    const char* key;
    float value;
};
constexpr FrozenDefault kV1Defaults[] = {
    { "virt.lfe", 0.0f }, // E01: +6 dB in version 2
};
constexpr FrozenDefault kV2Defaults[] = {
    { "virt.lfe", 6.0f }, // E01: +10 dB since version 3
};

template <size_t N>
json::Value fillFrozenDefaults (const json::Value& root, const FrozenDefault (&table)[N], int toMajor)
{
    json::Value out = root;
    json::Value params = root["params"].isObject() ? root["params"] : json::Value { json::Value::Object {} };
    for (const auto& d : table)
        if (params[d.key].isNull())
            params.set (d.key, static_cast<double> (d.value));
    out.set ("params", std::move (params));
    out.set ("version", toMajor);
    return out;
}

json::Value migrateV1ToV2 (const json::Value& root) { return fillFrozenDefaults (root, kV1Defaults, 2); }
json::Value migrateV2ToV3 (const json::Value& root) { return fillFrozenDefaults (root, kV2Defaults, 3); }

bool isUnset (const Preset& p, int id)
{
    return std::find (p.unsetAppState.begin(), p.unsetAppState.end(), id) != p.unsetAppState.end();
}

/** A choice value written as a label or an index; -1 when it is neither. */
int choiceFromJson (const Info& info, const json::Value& value)
{
    if (value.isString())
    {
        const auto it = std::find (info.choices.begin(), info.choices.end(), value.asString());
        return it == info.choices.end() ? -1 : static_cast<int> (it - info.choices.begin());
    }
    const double v = value.isNumber() ? value.asNumber() : -1.0;
    if (std::isfinite (v) && v > -0.5 && v < static_cast<double> (info.choices.size()) - 0.5)
        return static_cast<int> (std::lround (v));
    return -1;
}

/** Shortest round-trip text of a number ("1.5", "24"). */
std::string numberText (double v)
{
    char buf[32];
    const auto result = std::to_chars (buf, buf + sizeof (buf), v);
    return result.ec == std::errc() ? std::string (buf, result.ptr) : std::string ("?");
}

/** Levenshtein distance, capped: returns > cap as soon as it is exceeded. */
size_t editDistance (const std::string& a, const std::string& b, size_t cap)
{
    if ((a.size() > b.size() ? a.size() - b.size() : b.size() - a.size()) > cap)
        return cap + 1;
    std::vector<size_t> row (b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j)
        row[j] = j;
    for (size_t i = 1; i <= a.size(); ++i)
    {
        size_t diagonal = row[0];
        row[0] = i;
        size_t best = row[0];
        for (size_t j = 1; j <= b.size(); ++j)
        {
            const size_t above = row[j];
            row[j] = std::min ({ row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0u : 1u) });
            diagonal = above;
            best = std::min (best, row[j]);
        }
        if (best > cap)
            return cap + 1;
    }
    return row[b.size()];
}

/** The known key closest to `key` (at most 2 edits), or "" when none is. */
std::string closestKey (const std::string& key)
{
    std::string best;
    size_t bestDistance = 3;
    for (const auto& info : layout())
    {
        const size_t d = editDistance (key, info.key, bestDistance - 1);
        if (d < bestDistance)
        {
            bestDistance = d;
            best = info.key;
        }
    }
    return best;
}

std::string unknownKeyWarning (const std::string& key)
{
    std::string w = "unknown parameter \"" + key + "\" ignored";
    if (const auto near = closestKey (key); ! near.empty())
        w += " (did you mean \"" + near + "\"?)";
    return w;
}

std::string toLowerAscii (std::string s)
{
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char> (c - 'A' + 'a');
    return s;
}

/** Clamps `f` to the parameter's range and reports a change. */
float clampReported (const Info& info, float f, std::vector<std::string>& warnings)
{
    const float clamped = info.clamp (f);
    if (clamped != f)
        warnings.push_back ("\"" + info.key + "\" = " + numberText (static_cast<double> (f)) + " is out of range ["
                            + numberText (static_cast<double> (info.minValue)) + ", " + numberText (static_cast<double> (info.maxValue))
                            + "]: clamped to " + numberText (static_cast<double> (clamped)));
    return clamped;
}
} // namespace

std::string toString (SchemaVersion v)
{
    return std::to_string (v.majorVersion) + (v.minorVersion != 0 ? "." + std::to_string (v.minorVersion) : std::string());
}

bool parseSchemaVersion (const json::Value& version, SchemaVersion& out)
{
    std::string text;
    if (version.isNull())
        text = "1";
    else if (version.isNumber())
        text = numberText (version.asNumber()); // 2.1 -> "2.1"
    else if (version.isString())
        text = version.asString();
    else
        return false;

    const auto dot = text.find ('.');
    const std::string majorText = text.substr (0, dot);
    const std::string minorText = dot == std::string::npos ? std::string ("0") : text.substr (dot + 1);
    auto parseInt = [] (const std::string& t, int& v) {
        if (t.empty() || t.size() > 6)
            return false;
        v = 0;
        for (const char c : t)
        {
            if (c < '0' || c > '9')
                return false;
            v = v * 10 + (c - '0');
        }
        return true;
    };
    SchemaVersion v;
    if (! parseInt (majorText, v.majorVersion) || ! parseInt (minorText, v.minorVersion) || v.majorVersion < 1)
        return false;
    out = v;
    return true;
}

const std::vector<Migration>& migrations()
{
    static const std::vector<Migration> registry {
        { 1, "fill keys a version-1 file omits with the frozen version-1 defaults (virt.lfe 0 dB)", &migrateV1ToV2 },
        { 2, "fill keys a version-2 file omits with the frozen version-2 defaults (virt.lfe +6 dB)", &migrateV2ToV3 },
    };
    return registry;
}

bool migrate (const json::Value& root, json::Value& out, SchemaVersion& from, std::string& error)
{
    if (! parseSchemaVersion (root["version"], from))
    {
        error = "invalid preset \"version\" (expected major.minor, e.g. 2 or 2.1)";
        return false;
    }
    if (from.majorVersion > kSchemaVersion.majorVersion)
    {
        error = "preset was saved by a newer Flubsound version (preset schema " + toString (from) + "; this build reads up to "
                + std::to_string (kSchemaVersion.majorVersion) + ".x): update Flubsound to load it";
        return false;
    }
    out = root;
    for (int major = from.majorVersion; major < kSchemaVersion.majorVersion; ++major)
    {
        const auto& registry = migrations();
        const auto step = std::find_if (registry.begin(), registry.end(), [major] (const Migration& m) { return m.fromMajor == major; });
        if (step == registry.end())
        {
            error = "no migration from preset schema " + std::to_string (major);
            return false;
        }
        out = step->apply (out);
    }
    return true;
}

std::vector<std::pair<std::string, float>> frozenDefaults (int majorVersion)
{
    std::vector<std::pair<std::string, float>> table;
    if (majorVersion == 1)
        for (const auto& d : kV1Defaults)
            table.emplace_back (d.key, d.value);
    if (majorVersion == 2)
        for (const auto& d : kV2Defaults)
            table.emplace_back (d.key, d.value);
    return table;
}

std::string contentHash (const Preset& p)
{
    // 64-bit FNV-1a over (key bytes, 0, the value's IEEE-754 bits little
    // endian) of every sound parameter in layout order. -0 hashes as +0.
    uint64_t h = 14695981039346656037ull;
    auto mix = [&h] (uint8_t byte) {
        h ^= byte;
        h *= 1099511628211ull;
    };
    const auto& t = layout();
    for (size_t i = 0; i < t.size(); ++i)
    {
        if (isAppState (static_cast<int> (i)))
            continue;
        for (const char c : t[i].key)
            mix (static_cast<uint8_t> (c));
        mix (0);
        float v = i < p.values.size() ? p.values[i] : t[i].defaultValue;
        if (v == 0.0f)
            v = 0.0f;
        uint32_t bits = 0;
        std::memcpy (&bits, &v, sizeof (bits));
        for (int b = 0; b < 4; ++b)
            mix (static_cast<uint8_t> (bits >> (8 * b)));
    }
    char buf[17];
    std::snprintf (buf, sizeof (buf), "%016llx", static_cast<unsigned long long> (h));
    return buf;
}

std::string makeUuid()
{
    std::random_device device;
    std::mt19937_64 rng ((static_cast<uint64_t> (device()) << 32) ^ device());
    uint8_t bytes[16];
    for (size_t i = 0; i < 16; i += 8)
    {
        const uint64_t r = rng();
        for (size_t b = 0; b < 8; ++b)
            bytes[i + b] = static_cast<uint8_t> (r >> (8 * b));
    }
    bytes[6] = static_cast<uint8_t> ((bytes[6] & 0x0f) | 0x40); // version 4
    bytes[8] = static_cast<uint8_t> ((bytes[8] & 0x3f) | 0x80); // RFC 4122 variant
    static const char* const hex = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < 16; ++i)
    {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            s += '-';
        s += hex[bytes[i] >> 4];
        s += hex[bytes[i] & 0x0f];
    }
    return s;
}

bool isValidUuid (const std::string& text)
{
    if (text.size() != 36)
        return false;
    for (size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23)
        {
            if (c != '-')
                return false;
        }
        else if (! ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
        {
            return false;
        }
    }
    return true;
}

const Preset* findByUuid (const std::vector<Preset>& presets, const std::string& uuid)
{
    if (! isValidUuid (uuid))
        return nullptr;
    const auto wanted = toLowerAscii (uuid);
    for (const auto& p : presets)
        if (p.uuid == wanted)
            return &p;
    return nullptr;
}

bool isAppState (int paramId) noexcept
{
    return std::find (std::begin (kAppState), std::end (kAppState), paramId) != std::end (kAppState);
}

Preset makeDefault()
{
    Preset p;
    p.name = "Default";
    p.category = "General";
    p.author = "Flubsound";
    const auto& t = layout();
    p.values.resize (t.size());
    for (size_t i = 0; i < t.size(); ++i)
        p.values[i] = t[i].defaultValue;
    return p;
}

bool fromJson (const json::Value& v, Preset& out, std::string& error)
{
    if (! v.isObject())
    {
        error = "preset root must be an object";
        return false;
    }
    if (v["format"].asString() != "flubsound-preset")
    {
        error = "not a flubsound preset (missing \"format\": \"flubsound-preset\")";
        return false;
    }
    json::Value root;
    SchemaVersion version;
    if (! migrate (v, root, version, error))
        return false;

    out = makeDefault();
    out.loadedVersion = version;
    const int knownMinor = version.majorVersion == kSchemaVersion.majorVersion
                               ? kSchemaVersion.minorVersion
                               : kKnownMinor[std::min<size_t> (static_cast<size_t> (version.majorVersion), std::size (kKnownMinor) - 1)];
    if (version.minorVersion > knownMinor)
        out.warnings.push_back ("preset schema " + toString (version) + " is newer than this build knows ("
                                + std::to_string (version.majorVersion) + "." + std::to_string (knownMinor)
                                + "): settings added since are ignored");

    out.name = root["name"].asString();
    out.category = root["category"].asString();
    out.author = root["author"].asString();
    out.description = root["description"].asString();
    for (const auto& tag : root["tags"].asArray())
        if (tag.isString())
            out.tags.push_back (tag.asString());

    if (const auto& uuid = root["uuid"]; ! uuid.isNull())
    {
        if (uuid.isString() && isValidUuid (uuid.asString()))
            out.uuid = toLowerAscii (uuid.asString());
        else
            out.warnings.push_back ("invalid \"uuid\" ignored");
    }
    if (root["contentHash"].isString())
        out.savedContentHash = root["contentHash"].asString();

    const auto& t = layout();
    const auto& profileInfo = t[static_cast<size_t> (LatencyProfile)];
    if (const auto& suggestion = root["suggestedLatencyProfile"]; ! suggestion.isNull())
    {
        if (const int s = choiceFromJson (profileInfo, suggestion); s >= 0)
            out.suggestedLatencyProfile = static_cast<LatencyProfileValue> (s);
        else
            out.warnings.push_back ("unknown \"suggestedLatencyProfile\" ignored");
    }

    std::vector<int> carried;
    for (const auto& [key, value] : root["params"].asObject())
    {
        const int id = findByKey (key);
        if (id < 0)
        {
            out.warnings.push_back (unknownKeyWarning (key)); // forward compatible: ignored
            continue;
        }
        const auto& info = t[static_cast<size_t> (id)];
        float f = info.defaultValue;
        if (info.unit == Unit::Choice && value.isString())
        {
            const int c = choiceFromJson (info, value);
            if (c < 0)
            {
                out.warnings.push_back ("\"" + key + "\": unknown choice \"" + value.asString() + "\" ignored");
                continue;
            }
            f = static_cast<float> (c);
        }
        else if (value.isBool())
        {
            f = value.asBool() ? 1.0f : 0.0f;
        }
        else if (value.isNumber())
        {
            f = static_cast<float> (value.asNumber());
        }
        else
        {
            out.warnings.push_back ("\"" + key + "\": expected " + (info.unit == Unit::Choice ? "a label" : (info.unit == Unit::Toggle ? "true or false" : "a number")) + ", ignored");
            continue;
        }
        if (! std::isfinite (f))
        {
            out.warnings.push_back ("\"" + key + "\": not a finite number, ignored");
            continue;
        }
        f = clampReported (info, f, out.warnings);
        // A number for a choice or a toggle is read as the chain reads it (the
        // nearest index; on at >= 0.5), which is also what toJson writes back,
        // so a saved and reloaded preset keeps its values and contentHash
        // ("mode": 0.4 was kept as 0.4 and saved as "Music"; tests/fuzz).
        if (info.unit == Unit::Choice)
            f = static_cast<float> (std::lround (f));
        else if (info.unit == Unit::Toggle)
            f = f >= 0.5f ? 1.0f : 0.0f;
        out.values[static_cast<size_t> (id)] = f;
        if (isAppState (id))
            carried.push_back (id);
    }

    for (const int id : kAppState)
        if (std::find (carried.begin(), carried.end(), id) == carried.end())
            out.unsetAppState.push_back (id);

    // Migration (docs/11 E40): a profile in "params" is what older files and
    // presets saved from the store carry. It stays in `values` so saved strip
    // state still restores it, and becomes the suggestion when the file names
    // none, because applyPresetToStore never applies it.
    if (! out.suggestedLatencyProfile && ! isUnset (out, LatencyProfile))
        out.suggestedLatencyProfile = static_cast<LatencyProfileValue> (std::lround (out.values[static_cast<size_t> (LatencyProfile)]));
    return true;
}

json::Value toJson (const Preset& p, bool full)
{
    json::Value root;
    root.set ("format", "flubsound-preset");
    // A number, so builds from before major.minor (which read "version" as a
    // number) refuse a newer file instead of reading it as version 1. Minors
    // 1..9 only: 2.10 would read back as 2.1.
    static_assert (kSchemaVersion.minorVersion >= 0 && kSchemaVersion.minorVersion <= 9);
    root.set ("version", kSchemaVersion.majorVersion + kSchemaVersion.minorVersion / 10.0);
    if (isValidUuid (p.uuid))
        root.set ("uuid", toLowerAscii (p.uuid));
    root.set ("contentHash", contentHash (p));
    root.set ("name", p.name);
    root.set ("category", p.category);
    root.set ("author", p.author);
    root.set ("description", p.description);
    json::Value tags { json::Value::Array {} };
    for (const auto& t : p.tags)
        tags.push (t);
    root.set ("tags", std::move (tags));
    if (p.suggestedLatencyProfile)
    {
        const auto& choices = layout()[static_cast<size_t> (LatencyProfile)].choices;
        const auto c = static_cast<size_t> (*p.suggestedLatencyProfile);
        if (c < choices.size())
            root.set ("suggestedLatencyProfile", choices[c]);
    }

    json::Value params { json::Value::Object {} };
    const auto& t = layout();
    for (size_t i = 0; i < t.size() && i < p.values.size(); ++i)
    {
        const auto& info = t[i];
        const float v = p.values[i];
        if ((! full && v == info.defaultValue) || isUnset (p, static_cast<int> (i)))
            continue;
        if (info.unit == Unit::Choice)
        {
            const auto c = static_cast<size_t> (std::lround (v));
            params.set (info.key, c < info.choices.size() ? json::Value (info.choices[c]) : json::Value (static_cast<double> (v)));
        }
        else if (info.unit == Unit::Toggle)
        {
            params.set (info.key, v >= 0.5f);
        }
        else
        {
            params.set (info.key, static_cast<double> (v));
        }
    }
    root.set ("params", std::move (params));
    return root;
}

bool load (const std::string& path, Preset& out, std::string& error)
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
    return fromJson (v, out, error);
}

bool save (const std::string& path, const Preset& p, std::string& error, bool full)
{
    std::ofstream f (io::pathFromUtf8 (path), std::ios::binary | std::ios::trunc);
    if (! f)
    {
        error = "cannot write " + path;
        return false;
    }
    f << json::write (toJson (p, full), 2) << "\n";
    return static_cast<bool> (f);
}

void applyToStore (const Preset& p, ParameterStore& store, Bank bank)
{
    for (int i = 0; i < kNumParams && i < static_cast<int> (p.values.size()); ++i)
        if (! isUnset (p, i))
            store.set (bank, i, p.values[static_cast<size_t> (i)]);
}

void applyPresetToStore (const Preset& p, ParameterStore& store, Bank bank)
{
    for (int i = 0; i < kNumParams && i < static_cast<int> (p.values.size()); ++i)
        if (! isAppState (i))
            store.set (bank, i, p.values[static_cast<size_t> (i)]);
}

Preset captureFromStore (const ParameterStore& store, Bank bank)
{
    Preset p = makeDefault();
    p.name = "Untitled";
    for (int i = 0; i < kNumParams; ++i)
        p.values[static_cast<size_t> (i)] = store.get (bank, i);
    return p;
}

std::vector<float> resolveSavedState (const std::vector<std::pair<std::string, float>>& saved, std::vector<std::string>* warnings)
{
    const auto& t = layout();
    std::vector<float> values (t.size());
    for (size_t i = 0; i < t.size(); ++i)
        values[i] = t[i].defaultValue; // absent from the state: the default, not the previous value
    std::vector<std::string> ignored;
    for (const auto& [key, value] : saved)
    {
        const int id = findByKey (key);
        if (id < 0)
        {
            ignored.push_back (unknownKeyWarning (key));
            continue;
        }
        if (! std::isfinite (value))
        {
            ignored.push_back ("\"" + key + "\": not a finite number, ignored");
            continue;
        }
        values[static_cast<size_t> (id)] = clampReported (t[static_cast<size_t> (id)], value, ignored);
    }
    if (warnings != nullptr)
        *warnings = std::move (ignored);
    return values;
}
} // namespace flub::preset
