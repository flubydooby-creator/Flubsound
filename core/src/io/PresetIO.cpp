#include "flub/io/PresetIO.h"

#include "flub/io/FilePath.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <sstream>

namespace flub::preset
{
using namespace param;

namespace
{
constexpr int kAppState[] = { BypassAll, LoudnessMatchBypass, LatencyProfile };

// Schema version written by toJson(). Version 2 (docs/11 E01): a missing key
// means TODAY's default. A version-1 file (or one without "version") omitted
// values equal to the version-1 defaults, so its missing keys take the frozen
// version-1 default instead: a parameter default change never re-voices a
// sparse preset saved before it. Every future default change appends its old
// value here (docs/11 E52).
constexpr int kSchemaVersion = 2;
struct FrozenDefault
{
    const char* key;
    float value;
};
constexpr FrozenDefault kV1Defaults[] = {
    { "virt.lfe", 0.0f }, // E01: +6 dB since version 2
};

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
} // namespace

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
    const double version = v["version"].asNumber (1.0);
    if (version > kSchemaVersion)
    {
        error = "preset was saved by a newer Flubsound version";
        return false;
    }

    out = makeDefault();
    if (version < 2.0)
        for (const auto& d : kV1Defaults)
            if (const int id = findByKey (d.key); id >= 0)
                out.values[static_cast<size_t> (id)] = d.value;
    out.name = v["name"].asString();
    out.category = v["category"].asString();
    out.author = v["author"].asString();
    out.description = v["description"].asString();
    for (const auto& tag : v["tags"].asArray())
        if (tag.isString())
            out.tags.push_back (tag.asString());

    const auto& t = layout();
    const auto& profileInfo = t[static_cast<size_t> (LatencyProfile)];
    if (const int s = choiceFromJson (profileInfo, v["suggestedLatencyProfile"]); s >= 0)
        out.suggestedLatencyProfile = static_cast<LatencyProfileValue> (s);

    std::vector<int> carried;
    for (const auto& [key, value] : v["params"].asObject())
    {
        const int id = findByKey (key);
        if (id < 0)
            continue; // forward compatible: ignore unknown keys
        const auto& info = t[static_cast<size_t> (id)];
        float f = info.defaultValue;
        if (info.unit == Unit::Choice && value.isString())
        {
            const int c = choiceFromJson (info, value);
            if (c < 0)
                continue;
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
            continue;
        }
        if (! std::isfinite (f))
            continue;
        out.values[static_cast<size_t> (id)] = info.clamp (f);
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
    root.set ("version", kSchemaVersion);
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
} // namespace flub::preset
