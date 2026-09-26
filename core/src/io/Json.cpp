// Flubsound Pro - JSON value, parser and writer (see Json.h).
//
// Parser: recursive descent over the RFC 8259 grammar with a hard nesting
// limit (kMaxDepth) so hostile input cannot overflow the stack. Strings are
// validated as UTF-8 and \uXXXX escapes (incl. surrogate pairs) are decoded
// to UTF-8; numbers are checked against the JSON grammar first and then
// converted with std::from_chars (locale independent, correctly rounded).
// Errors carry the 1-based "line:col" of the offending character, with the
// column counted in code points so it matches what an editor shows.
//
// Writer: numbers use std::to_chars' shortest round-trip form, strings
// escape '"', '\\' and all control characters, and invalid UTF-8 (which can
// only come from values built in code) is replaced by U+FFFD so the output
// always parses again.
#include "flub/io/Json.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <new>
#include <system_error>

namespace flub::json
{
namespace
{
constexpr int kMaxDepth = 256;
constexpr int kMaxIndent = 32;

/** Length (1..4) of the well-formed UTF-8 sequence at p, or 0 if it is invalid
    (stray continuation byte, overlong form, UTF-16 surrogate, > U+10FFFF, truncated). */
size_t utf8SequenceLength (const unsigned char* p, size_t available) noexcept
{
    const unsigned c = p[0];
    if (c < 0x80)
        return 1;

    // Lead byte -> sequence length and the allowed range of the second byte, which
    // is where overlong forms, UTF-16 surrogates and code points > U+10FFFF show up.
    size_t length = 0;
    unsigned lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF)
    {
        length = 2;
    }
    else if (c >= 0xE0 && c <= 0xEF)
    {
        length = 3;
        lo = c == 0xE0 ? 0xA0u : 0x80u; // E0 80..9F would be overlong
        hi = c == 0xED ? 0x9Fu : 0xBFu; // ED A0..BF would encode a surrogate
    }
    else if (c >= 0xF0 && c <= 0xF4)
    {
        length = 4;
        lo = c == 0xF0 ? 0x90u : 0x80u; // F0 80..8F would be overlong
        hi = c == 0xF4 ? 0x8Fu : 0xBFu; // F4 90.. would exceed U+10FFFF
    }
    else
    {
        return 0; // continuation byte, C0/C1 (always overlong) or F5..FF
    }

    if (available < length || p[1] < lo || p[1] > hi)
        return 0;
    for (size_t i = 2; i < length; ++i)
        if (p[i] < 0x80 || p[i] > 0xBF)
            return 0;
    return length;
}

void appendUtf8 (std::string& s, uint32_t cp)
{
    if (cp < 0x80)
    {
        s += static_cast<char> (cp);
    }
    else if (cp < 0x800)
    {
        s += static_cast<char> (0xC0 | (cp >> 6));
        s += static_cast<char> (0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        s += static_cast<char> (0xE0 | (cp >> 12));
        s += static_cast<char> (0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char> (0x80 | (cp & 0x3F));
    }
    else
    {
        s += static_cast<char> (0xF0 | (cp >> 18));
        s += static_cast<char> (0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char> (0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char> (0x80 | (cp & 0x3F));
    }
}

int hexDigitValue (char c) noexcept
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool isDigit (char c) noexcept { return c >= '0' && c <= '9'; }

std::string describeChar (char c)
{
    const auto u = static_cast<unsigned char> (c);
    if (u >= 0x21 && u <= 0x7E)
        return std::string ("'") + c + "'";
    char buf[16];
    std::snprintf (buf, sizeof (buf), "byte 0x%02X", static_cast<unsigned> (u));
    return buf;
}

/** from_chars reports result_out_of_range for both overflow and underflow. A
    grammar-valid number whose decimal magnitude is below 1 can only have
    underflowed (-> signed zero, like strtod); anything else overflowed. */
bool magnitudeBelowOne (const char* first, const char* last) noexcept
{
    const char* p = first;
    if (p < last && *p == '-')
        ++p;

    int64_t index = 0, pointPosition = 0, firstNonZero = -1;
    for (; p < last && isDigit (*p); ++p, ++index, ++pointPosition)
        if (*p != '0' && firstNonZero < 0)
            firstNonZero = index;
    if (p < last && *p == '.')
        for (++p; p < last && isDigit (*p); ++p, ++index)
            if (*p != '0' && firstNonZero < 0)
                firstNonZero = index;

    int64_t exponent = 0;
    if (p < last && (*p == 'e' || *p == 'E'))
    {
        ++p;
        const bool negative = p < last && *p == '-';
        if (p < last && (*p == '-' || *p == '+'))
            ++p;
        for (; p < last && isDigit (*p); ++p)
            exponent = std::min<int64_t> (exponent * 10 + (*p - '0'), 100000000); // saturate
        if (negative)
            exponent = -exponent;
    }

    if (firstNonZero < 0)
        return true;
    // value = 0.d1d2... x 10^(pointPosition - firstNonZero + exponent)
    return pointPosition - firstNonZero + exponent <= 0;
}

//==============================================================================
class Parser
{
public:
    explicit Parser (const std::string& source) noexcept : text (source) {}

    bool parseDocument (Value& out)
    {
        // RFC 8259 s8.1 lets parsers ignore a UTF-8 byte order mark (Windows editors add one).
        if (text.size() >= 3 && text.compare (0, 3, "\xEF\xBB\xBF") == 0)
            pos = contentStart = 3;

        if (! parseValue (out, 0))
            return false;
        skipWhitespace();
        if (pos < text.size())
            return fail (pos, "unexpected " + describeChar (text[pos]) + " after the end of the document");
        return true;
    }

    std::string error;

private:
    const std::string& text;
    size_t pos = 0;
    size_t contentStart = 0; // after the BOM, which editors do not show as a column

    bool fail (size_t offset, const std::string& message)
    {
        // Count lines and code points (not bytes) up to the error so the column
        // matches what a text editor shows for UTF-8 content.
        size_t line = 1, column = 1;
        for (size_t i = contentStart; i < offset && i < text.size(); ++i)
        {
            const auto c = static_cast<unsigned char> (text[i]);
            if (c == '\n')
            {
                ++line;
                column = 1;
            }
            else if ((c & 0xC0) != 0x80)
            {
                ++column;
            }
        }
        error = std::to_string (line) + ":" + std::to_string (column) + ": " + message;
        return false;
    }

    void skipWhitespace() noexcept
    {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r'))
            ++pos;
    }

    bool parseValue (Value& out, int depth)
    {
        skipWhitespace();
        if (pos >= text.size())
            return fail (pos, "unexpected end of input, expected a value");

        const char c = text[pos];
        switch (c)
        {
            case '{':
            case '[':
                if (depth >= kMaxDepth)
                    return fail (pos, "nesting is deeper than " + std::to_string (kMaxDepth) + " levels");
                return c == '{' ? parseObject (out, depth + 1) : parseArray (out, depth + 1);
            case '"':
            {
                std::string s;
                if (! parseString (s))
                    return false;
                out = Value (std::move (s));
                return true;
            }
            case 't': return parseLiteral ("true", Value (true), out);
            case 'f': return parseLiteral ("false", Value (false), out);
            case 'n': return parseLiteral ("null", Value(), out);
            default:
                if (c == '-' || isDigit (c))
                    return parseNumber (out);
                return fail (pos, "unexpected " + describeChar (c) + ", expected a value");
        }
    }

    bool parseLiteral (const char* word, Value value, Value& out)
    {
        const size_t length = std::char_traits<char>::length (word);
        if (text.compare (pos, length, word) != 0)
            return fail (pos, "invalid literal (expected true, false or null)");
        pos += length;
        out = std::move (value);
        return true;
    }

    bool parseObject (Value& out, int depth)
    {
        const size_t start = pos++; // '{'
        Value::Object members;
        skipWhitespace();
        if (pos < text.size() && text[pos] == '}')
        {
            ++pos;
            out = Value (std::move (members));
            return true;
        }

        for (;;)
        {
            skipWhitespace();
            if (pos >= text.size())
                return fail (start, "unterminated object");
            if (text[pos] != '"')
            {
                if (text[pos] == '}' && ! members.empty())
                    return fail (pos, "trailing comma is not allowed in an object");
                return fail (pos, "expected a string key, found " + describeChar (text[pos]));
            }

            std::string key;
            if (! parseString (key))
                return false;
            skipWhitespace();
            if (pos >= text.size())
                return fail (start, "unterminated object");
            if (text[pos] != ':')
                return fail (pos, "expected ':' after object key, found " + describeChar (text[pos]));
            ++pos;

            Value v;
            if (! parseValue (v, depth))
                return false;
            // Duplicate keys are kept in document order; lookups resolve to the last one.
            members.emplace_back (std::move (key), std::move (v));

            skipWhitespace();
            if (pos >= text.size())
                return fail (start, "unterminated object");
            if (text[pos] == ',')
            {
                ++pos;
                continue;
            }
            if (text[pos] == '}')
            {
                ++pos;
                out = Value (std::move (members));
                return true;
            }
            return fail (pos, "expected ',' or '}' in object, found " + describeChar (text[pos]));
        }
    }

    bool parseArray (Value& out, int depth)
    {
        const size_t start = pos++; // '['
        Value::Array items;
        skipWhitespace();
        if (pos < text.size() && text[pos] == ']')
        {
            ++pos;
            out = Value (std::move (items));
            return true;
        }

        for (;;)
        {
            skipWhitespace();
            if (pos < text.size() && text[pos] == ']' && ! items.empty())
                return fail (pos, "trailing comma is not allowed in an array");
            if (pos >= text.size())
                return fail (start, "unterminated array");

            items.emplace_back();
            if (! parseValue (items.back(), depth))
                return false;

            skipWhitespace();
            if (pos >= text.size())
                return fail (start, "unterminated array");
            if (text[pos] == ',')
            {
                ++pos;
                continue;
            }
            if (text[pos] == ']')
            {
                ++pos;
                out = Value (std::move (items));
                return true;
            }
            return fail (pos, "expected ',' or ']' in array, found " + describeChar (text[pos]));
        }
    }

    /** Reads 4 hex digits at `at`; -1 if they are missing or invalid. */
    int32_t readHex4 (size_t at) const noexcept
    {
        if (at + 4 > text.size())
            return -1;
        int32_t v = 0;
        for (size_t i = 0; i < 4; ++i)
        {
            const int d = hexDigitValue (text[at + i]);
            if (d < 0)
                return -1;
            v = v * 16 + d;
        }
        return v;
    }

    bool parseString (std::string& s)
    {
        const size_t start = pos++; // opening quote
        for (;;)
        {
            // Copy runs of plain ASCII in one go; stop at quote, backslash, control or non-ASCII.
            const size_t runStart = pos;
            while (pos < text.size())
            {
                const auto c = static_cast<unsigned char> (text[pos]);
                if (c == '"' || c == '\\' || c < 0x20 || c >= 0x80)
                    break;
                ++pos;
            }
            s.append (text, runStart, pos - runStart);

            if (pos >= text.size())
                return fail (start, "unterminated string");

            const auto c = static_cast<unsigned char> (text[pos]);
            if (c == '"')
            {
                ++pos;
                return true;
            }
            if (c < 0x20)
            {
                char buf[64];
                std::snprintf (buf, sizeof (buf), "control character U+%04X must be escaped in a string", static_cast<unsigned> (c));
                return fail (pos, buf);
            }
            if (c >= 0x80)
            {
                const size_t length = utf8SequenceLength (reinterpret_cast<const unsigned char*> (text.data()) + pos, text.size() - pos);
                if (length == 0)
                    return fail (pos, "invalid UTF-8 byte sequence in string");
                s.append (text, pos, length);
                pos += length;
                continue;
            }

            // Backslash escape.
            if (pos + 1 >= text.size())
                return fail (start, "unterminated string");
            const size_t escapeStart = pos;
            const char e = text[pos + 1];
            pos += 2;
            switch (e)
            {
                case '"': s += '"'; break;
                case '\\': s += '\\'; break;
                case '/': s += '/'; break;
                case 'b': s += '\b'; break;
                case 'f': s += '\f'; break;
                case 'n': s += '\n'; break;
                case 'r': s += '\r'; break;
                case 't': s += '\t'; break;
                case 'u':
                {
                    const int32_t unit = readHex4 (pos);
                    if (unit < 0)
                        return fail (escapeStart, "invalid \\u escape (expected 4 hex digits)");
                    pos += 4;
                    auto cp = static_cast<uint32_t> (unit);
                    if (cp >= 0xDC00 && cp <= 0xDFFF)
                        return fail (escapeStart, "unpaired UTF-16 low surrogate in \\u escape");
                    if (cp >= 0xD800 && cp <= 0xDBFF)
                    {
                        // Characters outside the BMP arrive as a UTF-16 surrogate pair.
                        const int32_t low = (pos + 1 < text.size() && text[pos] == '\\' && text[pos + 1] == 'u') ? readHex4 (pos + 2) : -1;
                        if (low < 0xDC00 || low > 0xDFFF)
                            return fail (escapeStart, "unpaired UTF-16 high surrogate in \\u escape");
                        pos += 6;
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (static_cast<uint32_t> (low) - 0xDC00u);
                    }
                    appendUtf8 (s, cp);
                    break;
                }
                default:
                    return fail (escapeStart, "invalid escape sequence '\\" + std::string (1, e) + "'");
            }
        }
    }

    bool parseNumber (Value& out)
    {
        const size_t start = pos;
        if (text[pos] == '-')
            ++pos;

        if (pos >= text.size() || ! isDigit (text[pos]))
            return fail (pos, "invalid number: expected a digit");
        if (text[pos] == '0')
        {
            ++pos;
            if (pos < text.size() && isDigit (text[pos]))
                return fail (start, "invalid number: leading zeros are not allowed");
        }
        else
        {
            while (pos < text.size() && isDigit (text[pos]))
                ++pos;
        }

        if (pos < text.size() && text[pos] == '.')
        {
            ++pos;
            if (pos >= text.size() || ! isDigit (text[pos]))
                return fail (pos, "invalid number: expected a digit after '.'");
            while (pos < text.size() && isDigit (text[pos]))
                ++pos;
        }

        if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E'))
        {
            ++pos;
            if (pos < text.size() && (text[pos] == '+' || text[pos] == '-'))
                ++pos;
            if (pos >= text.size() || ! isDigit (text[pos]))
                return fail (pos, "invalid number: expected a digit in the exponent");
            while (pos < text.size() && isDigit (text[pos]))
                ++pos;
        }

        const char* first = text.data() + start;
        const char* last = text.data() + pos;
        double value = 0.0;
        const auto result = std::from_chars (first, last, value, std::chars_format::general);
        if (result.ec == std::errc::result_out_of_range)
        {
            if (! magnitudeBelowOne (first, last))
                return fail (start, "number is out of range for a double");
            value = *first == '-' ? -0.0 : 0.0; // underflow
        }
        else if (result.ec != std::errc() || result.ptr != last)
        {
            return fail (start, "invalid number");
        }

        out = Value (value);
        return true;
    }
};

//==============================================================================
void writeString (std::string& out, const std::string& s)
{
    out += '"';
    const auto* bytes = reinterpret_cast<const unsigned char*> (s.data());
    for (size_t i = 0; i < s.size();)
    {
        const unsigned char c = bytes[i];
        if (c >= 0x80)
        {
            const size_t length = utf8SequenceLength (bytes + i, s.size() - i);
            if (length == 0)
            {
                out += "\xEF\xBF\xBD"; // U+FFFD replacement character
                ++i;
            }
            else
            {
                out.append (s, i, length);
                i += length;
            }
            continue;
        }

        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    char buf[8];
                    std::snprintf (buf, sizeof (buf), "\\u%04x", static_cast<unsigned> (c));
                    out += buf;
                }
                else
                {
                    out += static_cast<char> (c);
                }
                break;
        }
        ++i;
    }
    out += '"';
}

void writeNumber (std::string& out, double d)
{
    // JSON has no NaN / Infinity.
    if (! std::isfinite (d))
    {
        out += "null";
        return;
    }
    char buf[64];
    const auto result = std::to_chars (buf, buf + sizeof (buf), d); // shortest round-trip form
    out.append (buf, result.ptr);
}

void newline (std::string& out, int indent, int level)
{
    out += '\n';
    out.append (static_cast<size_t> (indent) * static_cast<size_t> (level), ' ');
}

void writeValue (std::string& out, const Value& v, int indent, int level)
{
    if (v.isNull())
    {
        out += "null";
    }
    else if (v.isBool())
    {
        out += v.asBool() ? "true" : "false";
    }
    else if (v.isNumber())
    {
        writeNumber (out, v.asNumber());
    }
    else if (v.isString())
    {
        writeString (out, v.asString());
    }
    else if (v.isArray())
    {
        const auto& items = v.asArray();
        out += '[';
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (i > 0)
                out += ',';
            if (indent > 0)
                newline (out, indent, level + 1);
            writeValue (out, items[i], indent, level + 1);
        }
        if (indent > 0 && ! items.empty())
            newline (out, indent, level);
        out += ']';
    }
    else
    {
        const auto& members = v.asObject();
        out += '{';
        for (size_t i = 0; i < members.size(); ++i)
        {
            if (i > 0)
                out += ',';
            if (indent > 0)
                newline (out, indent, level + 1);
            writeString (out, members[i].first);
            out += indent > 0 ? ": " : ":";
            writeValue (out, members[i].second, indent, level + 1);
        }
        if (indent > 0 && ! members.empty())
            newline (out, indent, level);
        out += '}';
    }
}
} // namespace

//==============================================================================
bool Value::asBool (bool fallback) const noexcept
{
    const auto* b = std::get_if<bool> (&data);
    return b != nullptr ? *b : fallback;
}

double Value::asNumber (double fallback) const noexcept
{
    const auto* d = std::get_if<double> (&data);
    return d != nullptr ? *d : fallback;
}

const std::string& Value::asString() const
{
    static const std::string empty;
    const auto* s = std::get_if<std::string> (&data);
    return s != nullptr ? *s : empty;
}

const Value::Array& Value::asArray() const
{
    static const Array empty;
    const auto* a = std::get_if<Array> (&data);
    return a != nullptr ? *a : empty;
}

const Value::Object& Value::asObject() const
{
    static const Object empty;
    const auto* o = std::get_if<Object> (&data);
    return o != nullptr ? *o : empty;
}

const Value& Value::operator[] (const std::string& key) const
{
    static const Value null;
    const auto* o = std::get_if<Object> (&data);
    if (o == nullptr)
        return null;
    // Search from the back so duplicate keys resolve to the last one, as in most parsers.
    for (auto it = o->rbegin(); it != o->rend(); ++it)
        if (it->first == key)
            return it->second;
    return null;
}

void Value::set (const std::string& key, Value v)
{
    // Any non-object (not only null) becomes an empty object so set() always takes effect.
    if (! isObject())
        data = Object {};
    auto& members = std::get<Object> (data);
    for (auto it = members.rbegin(); it != members.rend(); ++it)
    {
        if (it->first == key)
        {
            it->second = std::move (v); // replace in place: keeps the member's position
            return;
        }
    }
    members.emplace_back (key, std::move (v));
}

void Value::push (Value v)
{
    // Any non-array (not only null) becomes an empty array so push() always takes effect.
    if (! isArray())
        data = Array {};
    std::get<Array> (data).push_back (std::move (v));
}

//==============================================================================
bool parse (const std::string& text, Value& out, std::string& error)
{
    try
    {
        Parser parser (text);
        Value result;
        if (! parser.parseDocument (result))
        {
            error = parser.error;
            return false;
        }
        out = std::move (result); // `out` is left untouched on failure
        return true;
    }
    catch (const std::bad_alloc&)
    {
        error = "1:1: out of memory while parsing";
    }
    catch (const std::exception& e)
    {
        error = std::string ("1:1: ") + e.what();
    }
    return false;
}

std::string write (const Value& v, int indent)
{
    std::string out;
    writeValue (out, v, std::clamp (indent, 0, kMaxIndent), 0);
    return out;
}
} // namespace flub::json
