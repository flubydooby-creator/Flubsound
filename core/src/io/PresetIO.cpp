#include "flub/io/PresetIO.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace flub::preset
{
using namespace param;

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
    if (v["version"].asNumber (0.0) > 1.0)
    {
        error = "preset was saved by a newer Flubsound version";
        return false;
    }

    out = makeDefault();
    out.name = v["name"].asString();
    out.category = v["category"].asString();
    out.author = v["author"].asString();
    out.description = v["description"].asString();
    for (const auto& tag : v["tags"].asArray())
        if (tag.isString())
            out.tags.push_back (tag.asString());

    const auto& t = layout();
    for (const auto& [key, value] : v["params"].asObject())
    {
        const int id = findByKey (key);
        if (id < 0)
            continue; // forward compatible: ignore unknown keys
        const auto& info = t[static_cast<size_t> (id)];
        float f = info.defaultValue;
        if (info.unit == Unit::Choice && value.isString())
        {
            const auto it = std::find (info.choices.begin(), info.choices.end(), value.asString());
            if (it == info.choices.end())
                continue;
            f = static_cast<float> (it - info.choices.begin());
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
    }
    return true;
}

json::Value toJson (const Preset& p, bool full)
{
    json::Value root;
    root.set ("format", "flubsound-preset");
    root.set ("version", 1);
    root.set ("name", p.name);
    root.set ("category", p.category);
    root.set ("author", p.author);
    root.set ("description", p.description);
    json::Value tags { json::Value::Array {} };
    for (const auto& t : p.tags)
        tags.push (t);
    root.set ("tags", std::move (tags));

    json::Value params { json::Value::Object {} };
    const auto& t = layout();
    for (size_t i = 0; i < t.size() && i < p.values.size(); ++i)
    {
        const auto& info = t[i];
        const float v = p.values[i];
        if (! full && v == info.defaultValue)
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
    std::ifstream f (path, std::ios::binary);
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
    std::ofstream f (path, std::ios::binary | std::ios::trunc);
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
