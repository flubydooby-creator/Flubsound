// Flubsound Pro - AutoEQ / Equalizer APO ParametricEQ text (see ParametricEqText.h).
#include "flub/io/ParametricEqText.h"

#include "flub/io/Json.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <optional>

namespace flub::eqtext
{
namespace
{
constexpr float kMaxGainDb = 30.0f, kMinFreq = 1.0f, kMaxFreq = 100000.0f, kMaxQ = 100.0f;
constexpr float kDefaultQ = 0.70710678f;

std::string lower (std::string_view s)
{
    std::string out (s);
    std::transform (out.begin(), out.end(), out.begin(), [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    return out;
}

bool isSpace (char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

std::string_view trim (std::string_view s) noexcept
{
    while (! s.empty() && isSpace (s.front()))
        s.remove_prefix (1);
    while (! s.empty() && isSpace (s.back()))
        s.remove_suffix (1);
    return s;
}

std::vector<std::string> tokens (std::string_view s)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size())
    {
        while (i < s.size() && isSpace (s[i]))
            ++i;
        const size_t start = i;
        while (i < s.size() && ! isSpace (s[i]))
            ++i;
        if (i > start)
            out.emplace_back (s.substr (start, i - start));
    }
    return out;
}

/** Locale-independent number: "1.5", "-3", "+2", ".5", "5.", "1e3", "0,7". */
std::optional<float> number (std::string token)
{
    if (token.find ('.') == std::string::npos)
        std::replace (token.begin(), token.end(), ',', '.');
    if (! token.empty() && token.front() == '+')
        token.erase (0, 1);
    const bool negative = ! token.empty() && token.front() == '-';
    std::string body = negative ? token.substr (1) : token;
    if (body.empty() || body.find_first_not_of ("0123456789.eE+-") != std::string::npos)
        return std::nullopt;
    if (body.front() == '.')
        body.insert (0, "0");
    if (const auto dot = body.find ('.'); dot != std::string::npos && (dot + 1 == body.size() || ! std::isdigit (static_cast<unsigned char> (body[dot + 1]))))
        body.insert (dot + 1, "0");
    // JSON's number grammar (leading zeros aside) is the strict form of the above.
    while (body.size() > 1 && body[0] == '0' && std::isdigit (static_cast<unsigned char> (body[1])))
        body.erase (0, 1);

    json::Value v;
    std::string error;
    if (! json::parse ((negative ? "-" : "") + body, v, error) || ! v.isNumber())
        return std::nullopt;
    const double d = v.asNumber();
    if (! std::isfinite (d) || std::abs (d) > 1.0e9)
        return std::nullopt;
    return static_cast<float> (d);
}

/** A number token that may carry its unit ("105Hz", "-3dB"). */
std::optional<float> numberWithUnit (const std::string& token, std::string_view unit)
{
    const auto t = lower (token);
    if (t.size() > unit.size() && t.compare (t.size() - unit.size(), unit.size(), unit) == 0)
        return number (token.substr (0, token.size() - unit.size()));
    return number (token);
}

std::string lineError (int line, const std::string& message)
{
    return "line " + std::to_string (line) + ": " + message;
}

/** RBJ shelf slope S -> Q at the shelf's gain; nullopt when S is too steep. */
std::optional<float> shelfQFromSlope (float slope, float gainDb)
{
    const double A = std::pow (10.0, gainDb / 40.0);
    const double radicand = (A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0;
    if (! (slope > 0.0f) || ! (radicand > 0.0))
        return std::nullopt;
    return static_cast<float> (1.0 / std::sqrt (radicand));
}

/** Octave bandwidth -> Q (the analog relation; exact at low frequencies). */
float qFromBandwidth (float octaves)
{
    const double p = std::exp2 (static_cast<double> (octaves));
    return static_cast<float> (std::sqrt (p) / (p - 1.0));
}

struct TypeInfo
{
    CorrectionFilterType type;
    bool needsGain, needsQ, shelf;
};

std::optional<TypeInfo> typeFor (const std::string& t)
{
    using T = CorrectionFilterType;
    if (t == "pk" || t == "peq" || t == "modal")
        return TypeInfo { T::Peak, true, true, false };
    if (t == "lsc" || t == "ls")
        return TypeInfo { T::LowShelf, true, false, true };
    if (t == "hsc" || t == "hs")
        return TypeInfo { T::HighShelf, true, false, true };
    if (t == "lp" || t == "lpq")
        return TypeInfo { T::LowPass, false, false, false };
    if (t == "hp" || t == "hpq")
        return TypeInfo { T::HighPass, false, false, false };
    if (t == "bp")
        return TypeInfo { T::BandPass, false, true, false };
    if (t == "no")
        return TypeInfo { T::Notch, false, true, false };
    if (t == "ap")
        return TypeInfo { T::AllPass, false, true, false };
    return std::nullopt;
}

const char* refusal (const std::string& command)
{
    if (command == "graphiceq")
        return "GraphicEQ is not supported yet (it needs the FIR convolver); import the headphone's ParametricEQ.txt instead";
    if (command == "include")
        return "Include is not supported: import the included file itself";
    if (command == "convolution")
        return "Convolution (impulse responses) is not supported";
    if (command == "stage")
        return "Stage is not supported: a device correction always runs on the output, after the mix";
    if (command == "delay" || command == "copy" || command == "eval" || command == "if" || command == "elseif" || command == "else"
        || command == "endif" || command == "loudnesscorrection")
        return "Equalizer APO routing / scripting commands are not supported";
    return nullptr;
}

/** "Filter", "Filter 1", "Filter1". */
bool isFilterCommand (const std::string& command)
{
    if (command.rfind ("filter", 0) != 0)
        return false;
    const auto rest = trim (std::string_view (command).substr (6));
    return std::all_of (rest.begin(), rest.end(), [] (char c) { return std::isdigit (static_cast<unsigned char> (c)) != 0; });
}

const char* channelName (uint8_t mask)
{
    return mask == CorrectionFilter::kLeft ? "L" : mask == CorrectionFilter::kRight ? "R" : "all";
}

std::string formatNumber (float v)
{
    char buf[64];
    const auto result = std::to_chars (buf, buf + sizeof (buf), v); // shortest round-trip form
    return std::string (buf, result.ptr);
}

const char* typeName (CorrectionFilterType t)
{
    switch (t)
    {
        case CorrectionFilterType::Peak: return "PK";
        case CorrectionFilterType::LowShelf: return "LSC";
        case CorrectionFilterType::HighShelf: return "HSC";
        case CorrectionFilterType::LowPass: return "LPQ";
        case CorrectionFilterType::HighPass: return "HPQ";
        case CorrectionFilterType::BandPass: return "BP";
        case CorrectionFilterType::Notch: return "NO";
        case CorrectionFilterType::AllPass: return "AP";
    }
    return "PK";
}
} // namespace

ParseResult parse (std::string_view text, CorrectionCurve& curve)
{
    ParseResult result;
    CorrectionCurve out;
    uint8_t selection = CorrectionFilter::kBoth;
    bool selectionHasOthers = false, warnedDropped = false, anyCommand = false;

    if (text.size() >= 3 && static_cast<unsigned char> (text[0]) == 0xEF && static_cast<unsigned char> (text[1]) == 0xBB
        && static_cast<unsigned char> (text[2]) == 0xBF)
        text.remove_prefix (3);

    const auto fail = [&result] (int line, const std::string& message)
    {
        result.ok = false;
        result.error = lineError (line, message);
        return result;
    };

    int lineNumber = 0;
    size_t pos = 0;
    while (pos <= text.size())
    {
        const size_t end = std::min (text.find ('\n', pos), text.size());
        const auto line = trim (text.substr (pos, end - pos));
        pos = end + 1;
        ++lineNumber;
        if (line.empty() || line.front() == '#')
            continue;

        const auto colon = line.find (':');
        if (colon == std::string_view::npos)
            return fail (lineNumber, "not a ParametricEQ line (expected \"Command: ...\")");
        const auto command = lower (trim (line.substr (0, colon)));
        const auto args = tokens (line.substr (colon + 1));

        if (const char* why = refusal (command))
            return fail (lineNumber, why);

        if (command == "device")
        {
            if (args.size() != 1 || lower (args[0]) != "all")
                result.warnings.push_back (lineError (lineNumber, "Device ignored: the correction applies to the output it is imported for"));
            continue;
        }

        if (command == "channel")
        {
            selection = 0;
            selectionHasOthers = false;
            warnedDropped = false;
            if (args.empty())
                return fail (lineNumber, "Channel needs at least one channel (L, R, 1, 2 or all)");
            for (const auto& a : args)
            {
                const auto c = lower (a);
                if (c == "all")
                    selection = CorrectionFilter::kBoth;
                else if (c == "l" || c == "1")
                    selection |= CorrectionFilter::kLeft;
                else if (c == "r" || c == "2")
                    selection |= CorrectionFilter::kRight;
                else if (c == "c" || c == "lfe" || c == "sub" || c == "rl" || c == "rr" || c == "rc" || c == "sl" || c == "sr"
                         || (number (c).has_value() && *number (c) >= 3.0f && *number (c) <= 32.0f))
                    selectionHasOthers = true;
                else
                    return fail (lineNumber, "unknown channel \"" + a + "\"");
            }
            continue;
        }

        const auto dropUnselected = [&]
        {
            if (selection != 0)
                return false;
            if (! warnedDropped)
                result.warnings.push_back (lineError (lineNumber, "lines for channels other than L / R are ignored (stereo output)"));
            warnedDropped = true;
            return true;
        };

        if (command == "preamp")
        {
            if (args.empty() || args.size() > 2 || (args.size() == 2 && lower (args[1]) != "db"))
                return fail (lineNumber, "expected \"Preamp: <gain> dB\"");
            const auto g = numberWithUnit (args[0], "db");
            if (! g || std::abs (*g) > kMaxGainDb)
                return fail (lineNumber, "preamp must be a number within +-30 dB");
            anyCommand = true;
            if (dropUnselected())
                continue;
            for (int c = 0; c < 2; ++c)
                if ((selection & (c == 0 ? CorrectionFilter::kLeft : CorrectionFilter::kRight)) != 0)
                {
                    auto& total = out.gainDb[static_cast<size_t> (c)];
                    total += *g;
                    if (std::abs (total) > kMaxGainDb) // Preamp lines add up
                        return fail (lineNumber, "the Preamp lines add up to more than +-30 dB");
                }
            continue;
        }

        if (! isFilterCommand (command))
            return fail (lineNumber, "unknown command \"" + std::string (trim (line.substr (0, colon))) + "\"");

        if (args.size() < 2)
            return fail (lineNumber, "expected \"Filter: ON <type> Fc <f> Hz ...\"");
        const auto state = lower (args[0]);
        if (state != "on" && state != "off")
            return fail (lineNumber, "a filter is ON or OFF");
        anyCommand = true;
        if (state == "off" || dropUnselected())
            continue;
        if (selectionHasOthers && ! warnedDropped)
        {
            result.warnings.push_back (lineError (lineNumber, "channels other than L / R are ignored (stereo output)"));
            warnedDropped = true;
        }

        const auto typeToken = lower (args[1]);
        const auto info = typeFor (typeToken);
        if (! info)
            return fail (lineNumber, "unsupported filter type \"" + args[1] + "\"");

        CorrectionFilter f;
        f.type = info->type;
        f.channels = selection;
        std::optional<float> fc, gain, q, bandwidth, slopeDb;
        size_t i = 2;

        // "LSC 12 dB" / "HSC 6.0 dB" slope form; "LS 6dB" / "HS 12dB" corner forms are refused.
        if (info->shelf && i < args.size() && numberWithUnit (args[i], "db"))
        {
            if (typeToken == "ls" || typeToken == "hs")
                return fail (lineNumber, "corner-frequency shelves (\"LS 6dB\" / \"LS 12dB\" / \"HS ...\") are not supported; use LSC / HSC");
            slopeDb = numberWithUnit (args[i], "db");
            ++i;
            if (i < args.size() && lower (args[i]) == "db")
                ++i;
        }

        while (i < args.size())
        {
            const auto key = lower (args[i]);
            const auto value = [&] (std::string_view unit) -> std::optional<float>
            {
                if (i + 1 >= args.size())
                    return std::nullopt;
                auto v = numberWithUnit (args[i + 1], unit);
                i += 2;
                if (v && ! unit.empty() && i < args.size() && lower (args[i]) == unit)
                    ++i;
                return v;
            };

            if (key == "fc")
            {
                if (! (fc = value ("hz")))
                    return fail (lineNumber, "Fc needs a frequency in Hz");
            }
            else if (key == "gain")
            {
                if (! (gain = value ("db")))
                    return fail (lineNumber, "Gain needs a value in dB");
            }
            else if (key == "q")
            {
                if (! (q = value ({})))
                    return fail (lineNumber, "Q needs a number");
            }
            else if (key == "bw")
            {
                if (i + 1 < args.size() && lower (args[i + 1]) == "oct")
                    ++i;
                if (! (bandwidth = value ({})) || ! (*bandwidth > 0.0f) || *bandwidth > 10.0f)
                    return fail (lineNumber, "BW Oct needs a bandwidth in octaves (0 .. 10]");
            }
            else
            {
                return fail (lineNumber, "unexpected \"" + args[i] + "\"");
            }
        }

        if (! fc || *fc < kMinFreq || *fc > kMaxFreq)
            return fail (lineNumber, "Fc must be 1 Hz .. 100 kHz");
        f.frequency = *fc;
        if (info->needsGain && ! gain)
            return fail (lineNumber, "this filter type needs a Gain");
        if (gain && std::abs (*gain) > kMaxGainDb)
            return fail (lineNumber, "gain must be within +-30 dB");
        f.gainDb = info->needsGain ? *gain : 0.0f;

        if (bandwidth && ! q)
            q = qFromBandwidth (*bandwidth);
        if (slopeDb)
        {
            if (q)
                return fail (lineNumber, "give a shelf either a slope or a Q, not both");
            q = shelfQFromSlope (*slopeDb / 12.0f, f.gainDb);
            if (! q)
                return fail (lineNumber, "shelf slope too steep for its gain");
        }
        if (! q)
        {
            if (info->needsQ)
                return fail (lineNumber, "this filter type needs a Q (or BW Oct)");
            if (info->shelf)
                result.warnings.push_back (lineError (lineNumber, "shelf without Q: Q 0.71 assumed"));
            q = kDefaultQ;
        }
        if (! (*q > 0.0f) || *q > kMaxQ)
            return fail (lineNumber, "Q must be within (0, 100]");
        f.q = *q;

        if (! out.add (f))
            return fail (lineNumber, "more than 16 filters on one channel");
    }

    if (! anyCommand)
    {
        result.error = "no Preamp or Filter lines found";
        return result;
    }
    curve = out;
    result.ok = true;
    return result;
}

std::string format (const CorrectionCurve& curve)
{
    std::string s = "# Flubsound device correction (Equalizer APO / AutoEQ ParametricEQ syntax)\n";
    const auto preamp = [&s] (float db) { s += "Preamp: " + formatNumber (db) + " dB\n"; };
    if (curve.gainDb[0] == curve.gainDb[1])
    {
        // A flat curve still gets one command: parse() refuses text without
        // any, and the app stores a correction as this text.
        if (curve.gainDb[0] != 0.0f || curve.numFilters == 0)
            preamp (curve.gainDb[0]);
    }
    else
    {
        s += "Channel: L\n";
        preamp (curve.gainDb[0]);
        s += "Channel: R\n";
        preamp (curve.gainDb[1]);
        s += "Channel: all\n";
    }

    uint8_t selection = CorrectionFilter::kBoth;
    for (int i = 0; i < curve.numFilters; ++i)
    {
        const auto& f = curve.filters[static_cast<size_t> (i)];
        if (f.channels != selection)
        {
            selection = f.channels;
            s += std::string ("Channel: ") + channelName (selection) + "\n";
        }
        s += "Filter " + std::to_string (i + 1) + ": ON " + typeName (f.type) + " Fc " + formatNumber (f.frequency) + " Hz";
        if (f.type == CorrectionFilterType::Peak || f.type == CorrectionFilterType::LowShelf || f.type == CorrectionFilterType::HighShelf)
            s += " Gain " + formatNumber (f.gainDb) + " dB";
        s += " Q " + formatNumber (f.q) + "\n";
    }
    return s;
}
} // namespace flub::eqtext
