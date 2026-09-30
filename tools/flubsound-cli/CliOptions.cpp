#include "CliOptions.h"

#include "FactoryPresets.h"

#include "flub/engine/MacroMap.h"
#include "flub/io/PresetIO.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace flub::cli
{
using namespace param;

namespace
{
// ---------------------------------------------------------------------------
// Small string helpers
// ---------------------------------------------------------------------------
std::string toLower (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    return s;
}

std::string trim (const std::string& s)
{
    const auto b = s.find_first_not_of (" \t");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of (" \t");
    return s.substr (b, e - b + 1);
}

/** Lower-case alphanumerics only: "Voice & Score" -> "voicescore". */
std::string looseKey (const std::string& s)
{
    std::string k;
    for (unsigned char c : s)
        if (std::isalnum (c))
            k += static_cast<char> (std::tolower (c));
    return k;
}

bool endsWith (const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && s.compare (s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/** Strict finite number: the whole string must be consumed. */
bool parseNumber (const std::string& text, double& value)
{
    const std::string t = trim (text);
    // Decimal only: strtod would also take hex ("0x100") and "inf" / "nan".
    if (t.empty() || t.find_first_not_of ("0123456789+-.eE") != std::string::npos)
        return false;
    errno = 0;
    char* end = nullptr;
    value = std::strtod (t.c_str(), &end);
    return errno == 0 && end == t.c_str() + t.size() && std::isfinite (value);
}

bool parseInt (const std::string& text, int& value)
{
    double d = 0.0;
    if (! parseNumber (text, d) || d != std::floor (d) || std::abs (d) > 1.0e9)
        return false;
    value = static_cast<int> (d);
    return true;
}

/** Number with an optional trailing '%' (always a 0..100 scale here). */
bool parsePercent (const std::string& text, float& percent)
{
    std::string t = trim (text);
    if (! t.empty() && t.back() == '%')
        t.pop_back();
    double d = 0.0;
    if (! parseNumber (t, d))
        return false;
    percent = static_cast<float> (d);
    return true;
}

std::string formatNumber (double v)
{
    char buf[48];
    std::snprintf (buf, sizeof (buf), "%g", v);
    return buf;
}

// ---------------------------------------------------------------------------
// Option table
// ---------------------------------------------------------------------------
enum CommandMask : unsigned
{
    kProcess = 1u << 0,
    kBatch = 1u << 1,
    kAnalyze = 1u << 2,
    kParams = 1u << 3,
    kPresets = 1u << 4,
    kQuality = 1u << 5,
    kSoak = 1u << 6,
    kDemo = 1u << 7,
    kRender = kProcess | kBatch,
    kChain = kRender | kQuality | kSoak, // options that choose the chain's settings
    kAll = kProcess | kBatch | kAnalyze | kParams | kPresets | kQuality | kSoak
};

enum class Opt
{
    Input,
    Output,
    Preset,
    PresetDir,
    Mode,
    Boost,
    Macro,
    Set,
    TargetLufs,
    Ceiling,
    Profile,
    Protection,
    Rate,
    Format,
    Block,
    Jobs,
    Recursive,
    Bands,
    Events,
    EventBand,
    Glitches,
    Spatial,
    FocusIld,
    Seconds,
    Minutes,
    Seed,
    Automation,
    Interval,
    Json,
    Dir,
    Quiet,
    Help
};

struct OptionSpec
{
    Opt id;
    const char* longName;
    const char* shortName; // may be nullptr
    bool takesValue;
    bool repeatableList; // --set a=1 b=2 ... / --macro 1=50 2=30 ...
    unsigned commands;
};

constexpr OptionSpec kOptions[] = {
    { Opt::Input, "--input", "-i", true, false, kProcess | kBatch | kAnalyze | kDemo },
    { Opt::Output, "--output", "-o", true, false, kRender | kDemo },
    { Opt::Output, "--out", nullptr, true, false, kDemo }, // `demo --out <dir>`
    { Opt::Preset, "--preset", "-p", true, false, kChain },
    { Opt::PresetDir, "--preset-dir", nullptr, true, false, kChain | kDemo },
    { Opt::Mode, "--mode", "-m", true, false, kChain },
    { Opt::Boost, "--boost", "-b", true, false, kChain },
    { Opt::Macro, "--macro", nullptr, true, true, kChain },
    { Opt::Set, "--set", "-s", true, true, kChain },
    { Opt::TargetLufs, "--target-lufs", "-t", true, false, kRender },
    { Opt::Ceiling, "--ceiling", "-c", true, false, kChain },
    { Opt::Profile, "--profile", nullptr, true, false, kChain },
    { Opt::Protection, "--protection", nullptr, true, false, kChain },
    { Opt::Rate, "--rate", nullptr, true, false, kQuality | kSoak },
    { Opt::Format, "--format", "-f", true, false, kRender | kDemo },
    { Opt::Block, "--block", nullptr, true, false, kChain | kDemo },
    { Opt::Jobs, "--jobs", "-j", true, false, kBatch | kDemo },
    { Opt::Recursive, "--recursive", "-r", false, false, kBatch },
    { Opt::Bands, "--bands", nullptr, false, false, kProcess | kAnalyze },
    { Opt::Events, "--events", nullptr, false, false, kAnalyze },
    { Opt::EventBand, "--event-band", nullptr, true, false, kAnalyze },
    { Opt::Glitches, "--glitches", nullptr, false, false, kAnalyze },
    { Opt::Spatial, "--spatial", nullptr, false, false, kAnalyze },
    { Opt::FocusIld, "--focus-ild", nullptr, false, false, kAnalyze },
    { Opt::Seconds, "--seconds", nullptr, true, false, kSoak | kDemo },
    { Opt::Minutes, "--minutes", nullptr, true, false, kSoak },
    { Opt::Seed, "--seed", nullptr, true, false, kSoak },
    { Opt::Automation, "--automation", nullptr, true, false, kSoak },
    { Opt::Interval, "--interval", nullptr, true, false, kSoak },
    { Opt::Json, "--json", nullptr, false, false, kAll },
    { Opt::Dir, "--dir", "-d", true, false, kPresets },
    { Opt::Quiet, "--quiet", "-q", false, false, kChain | kDemo },
    { Opt::Help, "--help", "-h", false, false, kAll },
};

const OptionSpec* findOption (const std::string& name)
{
    for (const auto& o : kOptions)
        if (name == o.longName || (o.shortName != nullptr && name == o.shortName))
            return &o;
    return nullptr;
}

unsigned maskFor (Command c) noexcept
{
    switch (c)
    {
        case Command::Process: return kProcess;
        case Command::Batch: return kBatch;
        case Command::Analyze: return kAnalyze;
        case Command::Params: return kParams;
        case Command::Presets: return kPresets;
        case Command::Quality: return kQuality;
        case Command::Soak: return kSoak;
        case Command::Demo: return kDemo;
        case Command::None:
        case Command::Help:
        case Command::Version: break;
    }
    return 0;
}

Command commandFromName (const std::string& name)
{
    const std::string n = toLower (name);
    if (n == "process")
        return Command::Process;
    if (n == "batch")
        return Command::Batch;
    if (n == "analyze" || n == "analyse")
        return Command::Analyze;
    if (n == "quality")
        return Command::Quality;
    if (n == "soak")
        return Command::Soak;
    if (n == "params" || n == "parameters")
        return Command::Params;
    if (n == "presets")
        return Command::Presets;
    if (n == "demo")
        return Command::Demo;
    if (n == "help" || n == "--help" || n == "-h")
        return Command::Help;
    if (n == "version" || n == "--version" || n == "-v")
        return Command::Version;
    return Command::None;
}

/** Applies one option value. */
bool applyOption (const OptionSpec& spec, const std::string& value, CliOptions& o, std::string& error)
{
    auto& r = o.render;
    const std::string v = trim (value);
    switch (spec.id)
    {
        case Opt::Input: o.input = value; return true;
        case Opt::Output: o.output = value; return true;
        case Opt::Preset: r.presetSpec = value; return true;
        case Opt::PresetDir: r.presetDir = value; return true;
        case Opt::Dir: o.dir = value; return true;

        case Opt::Mode:
        {
            const std::string m = toLower (v);
            if (m == "music")
                r.mode = ModeValue::Music;
            else if (m == "gaming" || m == "game")
                r.mode = ModeValue::Gaming;
            else
            {
                error = "--mode expects 'music' or 'gaming', got '" + value + "'";
                return false;
            }
            return true;
        }

        case Opt::Boost:
        {
            float p = 0.0f;
            if (! parsePercent (v, p) || p < 0.0f || p > 100.0f)
            {
                error = "--boost expects a value from 0 to 100, got '" + value + "'";
                return false;
            }
            r.boostPercent = p;
            return true;
        }

        case Opt::Macro:
        {
            const auto eq = v.find ('=');
            float p = 0.0f;
            if (eq == std::string::npos || eq == 0 || ! parsePercent (v.substr (eq + 1), p) || p < 0.0f || p > 100.0f)
            {
                error = "--macro expects N=0..100 (N = 1..5 or a macro name such as punch=60), got '" + value + "'";
                return false;
            }
            r.macros.push_back ({ trim (v.substr (0, eq)), p });
            return true;
        }

        case Opt::Set:
        {
            const auto eq = v.find ('=');
            if (eq == std::string::npos || eq == 0)
            {
                error = "--set expects key=value (see `flubsound-cli params` for keys), got '" + value + "'";
                return false;
            }
            r.sets.push_back ({ trim (v.substr (0, eq)), trim (v.substr (eq + 1)) });
            return true;
        }

        case Opt::TargetLufs:
        {
            double d = 0.0;
            std::string t = toLower (v);
            if (endsWith (t, "lufs"))
                t = trim (t.substr (0, t.size() - 4));
            if (! parseNumber (t, d) || d < -60.0 || d > -1.0)
            {
                error = "--target-lufs expects a loudness from -60 to -1 LUFS, got '" + value + "'";
                return false;
            }
            r.targetLufs = static_cast<float> (d);
            return true;
        }

        case Opt::Ceiling:
        {
            double d = 0.0;
            std::string t = toLower (v);
            if (endsWith (t, "dbtp"))
                t = trim (t.substr (0, t.size() - 4));
            const auto& info = layout()[static_cast<size_t> (MaxCeilingDb)];
            if (! parseNumber (t, d) || d < info.minValue || d > info.maxValue)
            {
                error = "--ceiling expects a true-peak ceiling from " + formatNumber (info.minValue) + " to "
                        + formatNumber (info.maxValue) + " dBTP, got '" + value + "'";
                return false;
            }
            r.ceilingDb = static_cast<float> (d);
            return true;
        }

        case Opt::Profile:
        {
            const std::string k = looseKey (v);
            if (k == "quality" || k == "q")
                r.profile = LatencyProfileValue::Quality;
            else if (k == "balanced" || k == "b")
                r.profile = LatencyProfileValue::Balanced;
            else if (k == "lowlatency" || k == "low" || k == "ll")
                r.profile = LatencyProfileValue::LowLatency;
            else
            {
                error = "--profile expects quality, balanced or low-latency, got '" + value + "'";
                return false;
            }
            return true;
        }

        case Opt::Protection:
        {
            const std::string k = looseKey (v);
            if (k == "off")
                r.protection = ProtectionStrength::Off;
            else if (k == "normal")
                r.protection = ProtectionStrength::Normal;
            else if (k == "strict")
                r.protection = ProtectionStrength::Strict;
            else
            {
                error = "--protection expects off, normal or strict, got '" + value + "'";
                return false;
            }
            return true;
        }

        case Opt::Format:
        {
            const std::string k = looseKey (v);
            if (k == "f32" || k == "float" || k == "float32")
                r.format = io::SampleFormat::Float32;
            else if (k == "pcm24" || k == "24" || k == "s24" || k == "int24")
                r.format = io::SampleFormat::Pcm24;
            else if (k == "pcm16" || k == "16" || k == "s16" || k == "int16")
                r.format = io::SampleFormat::Pcm16;
            else
            {
                error = "--format expects f32, pcm24 or pcm16, got '" + value + "'";
                return false;
            }
            o.demoFormatSet = true;
            return true;
        }

        case Opt::Block:
        {
            int n = 0;
            if (! parseInt (v, n) || n < 16 || n > 16384)
            {
                error = "--block expects a block size from 16 to 16384 samples, got '" + value + "'";
                return false;
            }
            r.blockSize = n;
            return true;
        }

        case Opt::Rate:
        {
            double d = 0.0;
            if (! parseNumber (v, d) || d < 8000.0 || d > 768000.0 || d != std::floor (d))
            {
                error = "--rate expects a sample rate in Hz from 8000 to 768000, got '" + value + "'";
                return false;
            }
            o.rate = d;
            return true;
        }

        case Opt::Jobs:
        {
            int n = 0;
            if (! parseInt (v, n) || n < 1 || n > 256)
            {
                error = "--jobs expects a number of parallel jobs from 1 to 256, got '" + value + "'";
                return false;
            }
            o.jobs = n;
            return true;
        }

        case Opt::EventBand:
        {
            double d = 0.0;
            std::string t = toLower (v);
            double scale = 1.0;
            if (endsWith (t, "khz") || endsWith (t, "k"))
            {
                t = trim (t.substr (0, t.size() - (endsWith (t, "khz") ? 3 : 1)));
                scale = 1000.0;
            }
            else if (endsWith (t, "hz"))
                t = trim (t.substr (0, t.size() - 2));
            if (! parseNumber (t, d) || d * scale < 20.0 || d * scale > 20000.0)
            {
                error = "--event-band expects a band centre from 20 Hz to 20 kHz, got '" + value + "'";
                return false;
            }
            o.eventBandHz = d * scale;
            o.events = true;
            return true;
        }

        case Opt::Seconds:
        case Opt::Minutes:
        {
            double d = 0.0;
            const double scale = spec.id == Opt::Minutes ? 60.0 : 1.0;
            if (o.command == Command::Demo) // the length of each built-in demo programme
            {
                if (! parseNumber (v, d) || d < 0.5 || d > 120.0)
                {
                    error = "--seconds expects a programme length from 0.5 to 120 s, got '" + value + "'";
                    return false;
                }
                o.demoSeconds = d;
                return true;
            }
            if (! parseNumber (v, d) || d * scale < 0.1 || d * scale > 7.0 * 24.0 * 3600.0)
            {
                error = std::string (spec.longName) + " expects a duration from 0.1 s to 7 days, got '" + value + "'";
                return false;
            }
            o.soakSeconds = d * scale;
            return true;
        }

        case Opt::Seed:
        {
            double d = 0.0;
            if (! parseNumber (v, d) || d < 1.0 || d > 4294967295.0 || d != std::floor (d))
            {
                error = "--seed expects a whole number from 1 to 4294967295, got '" + value + "'";
                return false;
            }
            o.seed = static_cast<uint32_t> (d);
            return true;
        }

        case Opt::Automation:
        {
            const std::string k = looseKey (v);
            if (k != "off" && k != "user" && k != "all")
            {
                error = "--automation expects off, user or all, got '" + value + "'";
                return false;
            }
            o.automation = k;
            return true;
        }

        case Opt::Interval:
        {
            double d = 0.0;
            std::string t = toLower (v);
            if (endsWith (t, "ms"))
                t = trim (t.substr (0, t.size() - 2));
            if (! parseNumber (t, d) || d < 10.0 || d > 600000.0)
            {
                error = "--interval expects a mean time between automation actions from 10 to 600000 ms, got '" + value + "'";
                return false;
            }
            o.intervalMs = d;
            return true;
        }

        case Opt::Recursive: o.recursive = true; return true;
        case Opt::Bands: o.bands = true; return true;
        case Opt::Events: o.events = true; return true;
        case Opt::Glitches: o.glitches = true; return true;
        case Opt::Spatial: o.spatial = true; return true;
        case Opt::FocusIld: o.focusIld = true; return true;
        case Opt::Json: o.json = true; return true;
        case Opt::Quiet: o.quiet = true; return true;
        case Opt::Help: return true; // handled by the caller
    }
    return true;
}

// ---------------------------------------------------------------------------
// Macro name resolution
// ---------------------------------------------------------------------------
/** Returns 0..4, or -1 (and error) if `which` does not name a macro of `mode`. */
int resolveMacro (const std::string& which, ModeValue mode, std::string& error)
{
    int index = 0;
    if (parseInt (which, index))
    {
        if (index >= 1 && index <= 5)
            return index - 1;
        error = "macro number must be 1..5, got '" + which + "'";
        return -1;
    }

    const std::string key = looseKey (which);
    auto find = [&key] (ModeValue m) {
        int hit = -1, count = 0;
        for (int i = 0; i < 5; ++i)
        {
            const std::string name = looseKey (MacroMap::macroName (m, i));
            if (! key.empty() && name.rfind (key, 0) == 0) // exact or unique prefix
            {
                if (name == key)
                    return i;
                hit = i;
                ++count;
            }
        }
        return count == 1 ? hit : -1;
    };

    if (const int i = find (mode); i >= 0)
        return i;

    const ModeValue other = mode == ModeValue::Music ? ModeValue::Gaming : ModeValue::Music;
    const char* modeName = mode == ModeValue::Music ? "Music" : "Gaming";
    const char* otherName = mode == ModeValue::Music ? "Gaming" : "Music";
    if (const int i = find (other); i >= 0)
    {
        error = std::string ("macro '") + which + "' (" + MacroMap::macroName (other, i) + ") belongs to " + otherName
                + " mode, but the render uses " + modeName + " mode (add --mode " + toLower (otherName) + ")";
        return -1;
    }

    error = "unknown macro '" + which + "'; " + modeName + " macros are:";
    for (int i = 0; i < 5; ++i)
        error += std::string (" ") + std::to_string (i + 1) + "=" + MacroMap::macroName (mode, i);
    return -1;
}
} // namespace

// ---------------------------------------------------------------------------
const char* unitLabel (Unit unit) noexcept
{
    switch (unit)
    {
        case Unit::None: return "";
        case Unit::Db: return "dB";
        case Unit::Hz: return "Hz";
        case Unit::Ms: return "ms";
        case Unit::Percent: return "%";
        case Unit::Ratio: return ":1";
        case Unit::Lufs: return "LUFS";
        case Unit::Degrees: return "deg";
        case Unit::Millimetres: return "mm";
        case Unit::DbPerSec: return "dB/s";
        case Unit::Choice: return "choice";
        case Unit::Toggle: return "on/off";
    }
    return "";
}

std::string formatParameterValue (int id, float value)
{
    const auto& info = layout()[static_cast<size_t> (id)];
    switch (info.unit)
    {
        case Unit::Toggle: return value >= 0.5f ? "on" : "off";
        case Unit::Choice:
        {
            const auto i = static_cast<size_t> (std::lround (value));
            return i < info.choices.size() ? info.choices[i] : formatNumber (value);
        }
        case Unit::Percent: return formatNumber (std::round (static_cast<double> (value) * 1000.0) / 10.0) + " %";
        case Unit::Ratio: return formatNumber (value) + ":1";
        case Unit::None: return formatNumber (value);
        case Unit::Db:
        case Unit::Hz:
        case Unit::Ms:
        case Unit::Lufs:
        case Unit::Degrees:
        case Unit::Millimetres:
        case Unit::DbPerSec: break;
    }
    return formatNumber (value) + " " + unitLabel (info.unit);
}

bool parseParameterValue (int id, const std::string& text, float& value, std::string& error)
{
    const auto& info = layout()[static_cast<size_t> (id)];
    const std::string t = trim (text);
    const std::string lower = toLower (t);

    if (info.unit == Unit::Toggle)
    {
        if (lower == "on" || lower == "true" || lower == "yes" || lower == "1")
            value = 1.0f;
        else if (lower == "off" || lower == "false" || lower == "no" || lower == "0")
            value = 0.0f;
        else
        {
            error = info.key + " is a toggle: use on/off, true/false, yes/no or 1/0 (got '" + text + "')";
            return false;
        }
        return true;
    }

    if (info.unit == Unit::Choice)
    {
        for (size_t i = 0; i < info.choices.size(); ++i)
            if (looseKey (info.choices[i]) == looseKey (t) && ! looseKey (t).empty())
            {
                value = static_cast<float> (i);
                return true;
            }
        int index = -1;
        if (parseInt (t, index) && index >= 0 && index < static_cast<int> (info.choices.size()))
        {
            value = static_cast<float> (index);
            return true;
        }
        error = info.key + " expects one of:";
        for (size_t i = 0; i < info.choices.size(); ++i)
            error += " '" + info.choices[i] + "'";
        error += " (or index 0.." + std::to_string (info.choices.size() - 1) + "), got '" + text + "'";
        return false;
    }

    // Numeric: strip an optional unit suffix, then range-check.
    std::string number = lower;
    double scale = 1.0;
    auto stripSuffix = [&number] (const char* suffix) {
        if (endsWith (number, suffix) && number.size() > std::char_traits<char>::length (suffix))
        {
            number = trim (number.substr (0, number.size() - std::char_traits<char>::length (suffix)));
            return true;
        }
        return false;
    };

    bool percentSuffix = false;
    switch (info.unit)
    {
        case Unit::Percent: percentSuffix = stripSuffix ("%"); break;
        case Unit::Hz:
            if (stripSuffix ("khz") || stripSuffix ("k"))
                scale = 1000.0;
            else
                stripSuffix ("hz");
            break;
        case Unit::Db: stripSuffix ("db"); break;
        case Unit::Ms: stripSuffix ("ms"); break;
        case Unit::Lufs: stripSuffix ("lufs"); break;
        case Unit::Degrees: stripSuffix ("deg"); break;
        case Unit::Millimetres: stripSuffix ("mm"); break;
        case Unit::DbPerSec: stripSuffix ("db/s"); break;
        case Unit::Ratio: stripSuffix (":1"); break;
        case Unit::None:
        case Unit::Choice:
        case Unit::Toggle: break;
    }

    double d = 0.0;
    if (! parseNumber (number, d))
    {
        error = info.key + " expects a number";
        if (*unitLabel (info.unit) != '\0')
            error += std::string (" in ") + unitLabel (info.unit);
        error += ", got '" + text + "'";
        return false;
    }
    d *= scale;
    if (percentSuffix)
        d /= 100.0;

    // Tiny tolerance so "100%" of a 0..1 range never fails on rounding.
    const double tolerance = 1.0e-6 * std::max (1.0, static_cast<double> (info.maxValue - info.minValue));
    if (d < info.minValue - tolerance || d > info.maxValue + tolerance)
    {
        error = info.key + " = " + text + " is out of range [" + formatParameterValue (id, info.minValue) + " .. "
                + formatParameterValue (id, info.maxValue) + "]";
        if (info.unit == Unit::Percent && ! percentSuffix)
            error += " (percent parameters are stored as 0.." + formatNumber (info.maxValue) + "; write e.g. '"
                     + formatNumber (std::min (100.0, d)) + "%' for a percentage)";
        return false;
    }
    value = info.clamp (static_cast<float> (d));
    return true;
}

// ---------------------------------------------------------------------------
bool parseCommandLine (const std::vector<std::string>& args, CliOptions& out, std::string& error)
{
    out = CliOptions();
    if (args.empty())
    {
        out.command = Command::Help;
        return true;
    }

    out.commandName = args[0];
    out.command = commandFromName (args[0]);
    if (out.command == Command::None)
    {
        error = "unknown command '" + args[0] + "' (commands: process, batch, analyze, quality, soak, params, presets, demo, help)";
        return false;
    }
    if (out.command == Command::Help)
    {
        if (args.size() > 1)
            out.helpTopic = args[1];
        return true;
    }
    if (out.command == Command::Version)
        return true;

    const unsigned mask = maskFor (out.command);
    std::vector<std::string> positional;

    for (size_t i = 1; i < args.size(); ++i)
    {
        std::string arg = args[i];
        if (arg.size() < 2 || arg[0] != '-' || (arg.size() > 1 && (std::isdigit (static_cast<unsigned char> (arg[1])) || arg[1] == '.')))
        {
            positional.push_back (arg);
            continue;
        }

        std::string inlineValue;
        bool hasInlineValue = false;
        if (arg.rfind ("--", 0) == 0)
            if (const auto eq = arg.find ('='); eq != std::string::npos)
            {
                inlineValue = arg.substr (eq + 1);
                arg.resize (eq);
                hasInlineValue = true;
            }

        const OptionSpec* spec = findOption (arg);
        if (spec == nullptr)
        {
            error = "unknown option '" + arg + "' (see `flubsound-cli " + out.commandName + " --help`)";
            return false;
        }
        if (spec->id == Opt::Help)
        {
            out.helpTopic = out.commandName;
            out.command = Command::Help;
            return true;
        }
        if ((spec->commands & mask) == 0)
        {
            error = std::string ("option ") + spec->longName + " is not valid for '" + out.commandName + "'";
            return false;
        }

        if (! spec->takesValue)
        {
            if (hasInlineValue)
            {
                error = std::string ("option ") + spec->longName + " does not take a value";
                return false;
            }
            if (! applyOption (*spec, {}, out, error))
                return false;
            continue;
        }

        std::string value;
        if (hasInlineValue)
            value = inlineValue;
        else if (i + 1 < args.size())
            value = args[++i]; // taken verbatim, so "--ceiling -1" works
        else
        {
            error = std::string ("option ") + spec->longName + " needs a value";
            return false;
        }
        if (! applyOption (*spec, value, out, error))
            return false;

        // "--set a=1 b=2" / "--macro 1=50 2=30": keep consuming key=value words.
        if (spec->repeatableList)
            while (i + 1 < args.size() && ! args[i + 1].empty() && args[i + 1][0] != '-'
                   && args[i + 1].find ('=') != std::string::npos)
                if (! applyOption (*spec, args[++i], out, error))
                    return false;
    }

    // Friendly positional form: `analyze song.wav`, `process in.wav out.wav`.
    for (const auto& p : positional)
    {
        const bool takesOutput = (mask & (kRender | kDemo)) != 0;
        if (out.input.empty() && (mask & (kRender | kAnalyze | kDemo)) != 0)
            out.input = p;
        else if (out.output.empty() && takesOutput)
            out.output = p;
        else
        {
            error = "unexpected argument '" + p + "'";
            return false;
        }
    }

    switch (out.command)
    {
        case Command::Process:
        case Command::Batch:
            if (out.input.empty() || out.output.empty())
            {
                error = out.commandName + " needs -i <" + (out.command == Command::Batch ? "input dir" : "input.wav") + "> and -o <"
                        + (out.command == Command::Batch ? "output dir" : "output.wav") + ">";
                return false;
            }
            break;
        case Command::Analyze:
            if (out.input.empty())
            {
                error = "analyze needs -i <file.wav>";
                return false;
            }
            break;
        case Command::Quality:
        case Command::Soak:
        case Command::Params:
        case Command::Presets:
        case Command::Demo:
        case Command::None:
        case Command::Help:
        case Command::Version: break;
    }
    return true;
}

// ---------------------------------------------------------------------------
bool buildParameters (const RenderOptions& o, ResolvedParameters& out, std::string& error)
{
    out = ResolvedParameters();

    preset::Preset p = preset::makeDefault();
    out.presetDescription = "defaults";
    if (! o.presetSpec.empty())
    {
        if (! resolvePreset (o.presetSpec, o.presetDir, p, out.presetDescription, error))
            return false;
        if (p.values.size() != static_cast<size_t> (kNumParams))
        {
            error = "internal error: preset table has the wrong size";
            return false;
        }
        // Keys and values the preset reader ignored or changed (docs/11 E52):
        // stderr (survives --quiet) and --json render.notes.
        for (const auto& w : p.warnings)
            out.notes.push_back ("warning: preset: " + w);
    }
    auto& v = out.values;
    v = p.values;

    // Bypass is application state, never preset state (see PresetManager).
    const auto& info = layout();
    v[static_cast<size_t> (BypassAll)] = info[static_cast<size_t> (BypassAll)].defaultValue;

    // Resolve every --set up front so errors surface before any work starts.
    std::vector<std::pair<int, float>> sets;
    for (const auto& s : o.sets)
    {
        const int id = findByKey (s.key);
        if (id < 0)
        {
            error = "unknown parameter key '" + s.key + "' (run `flubsound-cli params` for the list)";
            return false;
        }
        float value = 0.0f;
        if (! parseParameterValue (id, s.value, value, error))
            return false;
        sets.emplace_back (id, value);
    }
    auto explicitlySet = [&sets] (int id) {
        return std::any_of (sets.begin(), sets.end(), [id] (const auto& s) { return s.first == id; });
    };
    auto set = [&v] (int id, float value) { v[static_cast<size_t> (id)] = layout()[static_cast<size_t> (id)].clamp (value); };

    // Mode first: macro names are resolved against the final mode (a
    // `--set mode=...` wins over --mode like every other --set).
    if (o.mode)
        set (Mode, static_cast<float> (static_cast<int> (*o.mode)));
    for (const auto& [id, value] : sets)
        if (id == Mode)
            set (Mode, value);
    const auto mode = static_cast<ModeValue> (std::lround (v[static_cast<size_t> (Mode)]));

    if (o.boostPercent)
        set (BoostIntensity, *o.boostPercent / 100.0f);
    for (const auto& m : o.macros)
    {
        const int index = resolveMacro (m.which, mode, error);
        if (index < 0)
            return false;
        set (Macro1 + index, m.percent / 100.0f);
    }
    if (o.profile)
        set (LatencyProfile, static_cast<float> (static_cast<int> (*o.profile)));
    if (o.ceilingDb)
        set (MaxCeilingDb, *o.ceilingDb);

    for (const auto& [id, value] : sets)
        set (id, value);

    // A ceiling / loudness target is only guaranteed by the maximizer.
    if ((o.ceilingDb || o.targetLufs) && v[static_cast<size_t> (MaximizerOn)] < 0.5f)
    {
        if (explicitlySet (MaximizerOn))
            out.notes.push_back ("warning: max.on=off was requested, so the true-peak ceiling is NOT guaranteed");
        else
        {
            set (MaximizerOn, 1.0f);
            out.notes.push_back ("loudness maximizer enabled (required for --ceiling / --target-lufs)");
        }
    }

    // The offline loudness loop replaces the real-time AutoDrive loop, which
    // could otherwise fight it (AutoDrive only ever reduces drive).
    if (o.targetLufs && v[static_cast<size_t> (MaxAutoDrive)] >= 0.5f)
    {
        set (MaxAutoDrive, 0.0f);
        out.notes.push_back ("max.autoDrive disabled (--target-lufs sets the maximizer drive offline)");
    }

    if (v[static_cast<size_t> (BypassAll)] >= 0.5f)
        out.notes.push_back ("bypass=on: the output is the latency-aligned dry signal (turned down to the processed loudness where it is louder, unless bypass.matched=off; never raised)");
    return true;
}
} // namespace flub::cli
