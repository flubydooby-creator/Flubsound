// Tests for the JSON value / parser / writer: preset-like round trips,
// escapes and UTF-8 / surrogate pairs, shortest round-trip numbers, the
// RFC 8259 grammar edge cases, "line:col" error reporting, the nesting
// limit, accessor semantics, fuzzed input and allocation-free lookups.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/io/Json.h"

#include <bit>
#include <cctype>
#include <cfloat>
#include <limits>

using namespace flub;
using namespace flub::json;
using namespace flubtest;

namespace
{
/** JSON text with "~u" standing for the \\u escape. MSVC rejects surrogate
    universal-character-names (\\uD800..\\uDFFF) even inside raw string
    literals, so the tests spell them this way and expand them at run time. */
std::string unicodeEscapes (std::string s)
{
    for (size_t i = 0; (i = s.find ("~u", i)) != std::string::npos; i += 2)
        s[i] = '\\';
    return s;
}

bool sameNumber (double a, double b) { return std::bit_cast<uint64_t> (a) == std::bit_cast<uint64_t> (b); }

/** Structural equality; numbers compared bit for bit (so -0 != 0). */
bool deepEqual (const Value& a, const Value& b)
{
    if (a.isNull() || b.isNull())
        return a.isNull() && b.isNull();
    if (a.isBool() || b.isBool())
        return a.isBool() && b.isBool() && a.asBool() == b.asBool();
    if (a.isNumber() || b.isNumber())
        return a.isNumber() && b.isNumber() && sameNumber (a.asNumber(), b.asNumber());
    if (a.isString() || b.isString())
        return a.isString() && b.isString() && a.asString() == b.asString();
    if (a.isArray() || b.isArray())
    {
        if (! (a.isArray() && b.isArray()) || a.asArray().size() != b.asArray().size())
            return false;
        for (size_t i = 0; i < a.asArray().size(); ++i)
            if (! deepEqual (a.asArray()[i], b.asArray()[i]))
                return false;
        return true;
    }
    const auto& ao = a.asObject();
    const auto& bo = b.asObject();
    if (! (a.isObject() && b.isObject()) || ao.size() != bo.size())
        return false;
    for (size_t i = 0; i < ao.size(); ++i)
        if (ao[i].first != bo[i].first || ! deepEqual (ao[i].second, bo[i].second))
            return false;
    return true;
}

Value parseOrFail (const std::string& text)
{
    Value v;
    std::string error;
    if (! parse (text, v, error))
        reportFailure (__FILE__, __LINE__, "unexpected parse failure: " + error + " for: " + text.substr (0, 80));
    return v;
}

/** Parses `text`, expecting failure with an error of the form "<line>:<col>: ...". */
bool failsAt (const std::string& text, int line, int column, const char* expectedMessagePart = "")
{
    Value v;
    std::string error;
    if (parse (text, v, error))
    {
        std::cerr << "    expected a parse error for: " << text.substr (0, 60) << "\n";
        return false;
    }
    const std::string prefix = std::to_string (line) + ":" + std::to_string (column) + ": ";
    if (error.rfind (prefix, 0) != 0 || error.find (expectedMessagePart) == std::string::npos)
    {
        std::cerr << "    expected \"" << prefix << "..." << expectedMessagePart << "...\", got \"" << error << "\"\n";
        return false;
    }
    return true;
}

/** Any error at all, still in line:col form. */
bool fails (const std::string& text)
{
    Value v;
    std::string error;
    if (parse (text, v, error))
        return false;
    const auto colon = error.find (':');
    const auto second = error.find (": ");
    return colon != std::string::npos && colon > 0 && second != std::string::npos && second > colon
           && std::isdigit (static_cast<unsigned char> (error[0])) && std::isdigit (static_cast<unsigned char> (error[colon + 1]));
}

const char* kPresetText = R"({
  "format": "flubsound-preset",
  "version": 1,
  "name": "Competitive FPS",
  "category": "Gaming",
  "author": "Flubsound",
  "description": "Footsteps forward, explosions tamed.\nUse with closed headphones.",
  "tags": ["fps", "footsteps", "competitive"],
  "params": {
    "mode": "Gaming",
    "boost": 0.4,
    "eq.0.freq": 90.0,
    "eq.0.gain": -2.5,
    "eq.1.q": 0.7071067811865476,
    "limiter.enabled": true,
    "virtualizer.enabled": false,
    "noise.floor": -1.0e-7,
    "reserved": null,
    "curve": [[20, -3.0], [1000, 0], [16000, 2.25]],
    "empty.obj": {},
    "empty.arr": []
  }
})";
} // namespace

//==============================================================================
TEST_CASE ("Json: preset-like document round trips through compact and pretty output")
{
    const Value doc = parseOrFail (kPresetText);
    REQUIRE (doc.isObject());
    CHECK (doc["format"].asString() == "flubsound-preset");
    CHECK (doc["version"].asNumber() == 1.0);
    CHECK (doc["description"].asString() == "Footsteps forward, explosions tamed.\nUse with closed headphones.");
    CHECK (doc["tags"].asArray().size() == 3);
    CHECK (doc["tags"].asArray()[1].asString() == "footsteps");
    CHECK (doc["params"]["boost"].asNumber() == 0.4);
    CHECK (doc["params"]["eq.1.q"].asNumber() == 0.7071067811865476);
    CHECK (doc["params"]["limiter.enabled"].asBool() == true);
    CHECK (doc["params"]["virtualizer.enabled"].isBool());
    CHECK (doc["params"]["reserved"].isNull());
    CHECK (doc["params"]["curve"].asArray()[2].asArray()[1].asNumber() == 2.25);
    CHECK (doc["params"]["empty.obj"].isObject() && doc["params"]["empty.obj"].asObject().empty());
    CHECK (doc["params"]["empty.arr"].isArray() && doc["params"]["empty.arr"].asArray().empty());

    for (int indent : { 0, 2, 4 })
    {
        const std::string text = write (doc, indent);
        const Value again = parseOrFail (text);
        CHECK (deepEqual (doc, again));
        CHECK (write (again, indent) == text); // serialisation is stable
    }

    // Exact layout of both styles.
    Value small;
    small.set ("a", 1);
    small.set ("b", Value::Array { Value (true), Value(), Value ("x") });
    small.set ("c", Value::Object {});
    CHECK (write (small, 0) == R"({"a":1,"b":[true,null,"x"],"c":{}})");
    CHECK (write (small, 2) == "{\n  \"a\": 1,\n  \"b\": [\n    true,\n    null,\n    \"x\"\n  ],\n  \"c\": {}\n}");
    CHECK (write (small, -3) == write (small, 0));
    CHECK (write (Value(), 2) == "null");
    CHECK (write (Value::Array {}, 2) == "[]");
}

TEST_CASE ("Json: objects preserve insertion order; duplicate keys resolve to the last one")
{
    const Value v = parseOrFail (R"({"zeta": 1, "alpha": 2, "mid": 3})");
    const auto& o = v.asObject();
    REQUIRE (o.size() == 3);
    CHECK (o[0].first == "zeta" && o[1].first == "alpha" && o[2].first == "mid");
    CHECK (write (v, 0) == R"({"zeta":1,"alpha":2,"mid":3})");

    Value dup = parseOrFail (R"({"a": 1, "b": 2, "a": 3})");
    CHECK (dup.asObject().size() == 3); // kept verbatim
    CHECK (dup["a"].asNumber() == 3.0);
    dup.set ("a", 4);
    CHECK (dup["a"].asNumber() == 4.0);
    CHECK (dup.asObject().size() == 3);
}

TEST_CASE ("Json: escapes, unicode and surrogate pairs round trip")
{
    const std::string text = unicodeEscapes (R"("q\" b\\ s\/ \b\f\n\r\t A=~u0041 e=~u00e9 E=~u20AC note=~ud83c~udfb5 nul=~u0000 end")");
    const Value v = parseOrFail (text);
    REQUIRE (v.isString());
    const std::string expected = std::string ("q\" b\\ s/ \b\f\n\r\t A=A e=\xC3\xA9 E=\xE2\x82\xAC note=\xF0\x9F\x8E\xB5 nul=") + '\0' + " end";
    CHECK (v.asString() == expected);

    // Writer: named escapes for the common controls, \u00XX for the rest, UTF-8 passes through.
    const std::string written = write (v);
    CHECK (written == "\"q\\\" b\\\\ s/ \\b\\f\\n\\r\\t A=A e=\xC3\xA9 E=\xE2\x82\xAC note=\xF0\x9F\x8E\xB5 nul=\\u0000 end\"");
    CHECK (parseOrFail (written).asString() == expected);

    // Raw (unescaped) multi-byte UTF-8 is accepted as-is.
    CHECK (parseOrFail ("\"\xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x8E\xA7\"").asString() == "\xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x8E\xA7");
    // Highest code point via a surrogate pair.
    CHECK (parseOrFail (unicodeEscapes (R"("~uDBFF~uDFFF")")).asString() == "\xF4\x8F\xBF\xBF");

    // Every control character is escaped and survives.
    std::string controls;
    for (int c = 1; c < 0x20; ++c)
        controls += static_cast<char> (c);
    controls += "\x7F";
    const std::string w = write (Value (controls));
    for (char c : w)
        CHECK (static_cast<unsigned char> (c) >= 0x20);
    CHECK (w.find ("\\u001f") != std::string::npos);
    CHECK (parseOrFail (w).asString() == controls);

    // Invalid UTF-8 built in code is repaired with U+FFFD so the output always parses.
    const std::string bad = "ok\xC3(\xFF\xED\xA0\x80z";
    const std::string repaired = write (Value (bad));
    Value back;
    std::string error;
    REQUIRE (parse (repaired, back, error));
    CHECK (back.asString().find ("\xEF\xBF\xBD") != std::string::npos);
    CHECK (back.asString().front() == 'o' && back.asString().back() == 'z');
}

TEST_CASE ("Json: numbers round trip exactly via shortest formatting")
{
    CHECK (write (Value (-0.5)) == "-0.5");
    CHECK (write (Value (1e-7)) == "1e-07");
    CHECK (write (Value (12345678901.0)) == "12345678901");
    CHECK (write (Value (3.14159)) == "3.14159");
    CHECK (write (Value (0.1)) == "0.1");
    CHECK (write (Value (1)) == "1");
    CHECK (write (Value (-0.0)) == "-0");

    const double specials[] = { -0.5, 1e-7, 12345678901.0, 3.14159, 0.1, 1.0 / 3.0, -0.0, 0.0, 2.0 / 3.0 * 1e-300, DBL_MAX, -DBL_MAX,
                                DBL_MIN, std::numeric_limits<double>::denorm_min(), 9007199254740993.0, 1e21, 1e22, 123456789.125 };
    for (double d : specials)
    {
        const Value back = parseOrFail (write (Value (d)));
        CHECK (back.isNumber() && sameNumber (back.asNumber(), d));
    }

    // Random bit patterns: every finite double survives write -> parse bit for bit.
    FastRandom rng (2024);
    int tested = 0;
    for (int i = 0; i < 20000; ++i)
    {
        const uint64_t bits = (static_cast<uint64_t> (rng.nextU32()) << 32) | rng.nextU32();
        const double d = std::bit_cast<double> (bits);
        if (! std::isfinite (d))
            continue;
        ++tested;
        Value back;
        std::string error;
        const std::string text = write (Value (d));
        if (! parse (text, back, error) || ! sameNumber (back.asNumber(), d))
        {
            reportFailure (__FILE__, __LINE__, "number did not round trip: " + text);
            break;
        }
    }
    CHECK (tested > 19000);
}

TEST_CASE ("Json: number grammar follows RFC 8259")
{
    CHECK (parseOrFail ("0").asNumber() == 0.0);
    CHECK (std::signbit (parseOrFail ("-0").asNumber()));
    CHECK (parseOrFail ("-0.0e0").asNumber() == 0.0);
    CHECK (parseOrFail ("1E5").asNumber() == 1e5);
    CHECK (parseOrFail ("1e+5").asNumber() == 1e5);
    CHECK (parseOrFail ("25e-1").asNumber() == 2.5);
    CHECK (parseOrFail ("0.5e-3").asNumber() == 0.0005);
    CHECK (parseOrFail ("1e-07").asNumber() == 1e-7);
    CHECK (parseOrFail ("-12345678901").asNumber() == -12345678901.0);
    CHECK (parseOrFail ("1.7976931348623157e308").asNumber() == DBL_MAX);
    CHECK (parseOrFail ("4.9e-324").asNumber() == std::numeric_limits<double>::denorm_min());

    // Underflow rounds to (signed) zero like strtod; overflow is an error.
    CHECK (parseOrFail ("1e-400").asNumber() == 0.0);
    CHECK (std::signbit (parseOrFail ("-1e-400").asNumber()));
    CHECK (parseOrFail ("0.000000000000000000000000000000000000000000000001e-300").asNumber() == 0.0);
    CHECK (failsAt ("1e400", 1, 1, "out of range"));
    CHECK (failsAt ("-1.8e308", 1, 1, "out of range"));
    CHECK (failsAt ("100000000000000000000000000000000000000000000000000e300", 1, 1, "out of range"));
    CHECK (parseOrFail ("1e-99999999999999999999999").asNumber() == 0.0);
    CHECK (failsAt ("1e99999999999999999999999", 1, 1, "out of range"));

    CHECK (failsAt ("01", 1, 1, "leading zeros"));
    CHECK (failsAt ("-01", 1, 1, "leading zeros"));
    CHECK (failsAt ("[1, 007]", 1, 5, "leading zeros"));
    CHECK (failsAt ("-", 1, 2, "expected a digit"));
    CHECK (failsAt ("1.", 1, 3, "after '.'"));
    CHECK (failsAt ("1.e5", 1, 3, "after '.'"));
    CHECK (failsAt ("1e", 1, 3, "exponent"));
    CHECK (failsAt ("1e+", 1, 4, "exponent"));
    CHECK (failsAt (".5", 1, 1, "expected a value"));
    CHECK (failsAt ("+1", 1, 1, "expected a value"));
    CHECK (failsAt ("0x10", 1, 2, "after the end"));
    CHECK (failsAt ("NaN", 1, 1, "expected a value"));
    CHECK (failsAt ("-Infinity", 1, 2, "expected a digit"));
    CHECK (failsAt ("1 2", 1, 3, "after the end"));
}

TEST_CASE ("Json: malformed documents fail with line:col messages")
{
    CHECK (failsAt ("[1, 2,]", 1, 7, "trailing comma"));
    CHECK (failsAt ("{\"a\": 1,}", 1, 9, "trailing comma"));
    CHECK (failsAt ("{\n  \"a\": 1,\n  \"b\": 2,\n}", 4, 1, "trailing comma"));
    CHECK (failsAt ("\"abc", 1, 1, "unterminated string"));
    CHECK (failsAt ("{\"key\": \"value", 1, 9, "unterminated string"));
    CHECK (failsAt ("\"a\\", 1, 1, "unterminated string"));
    CHECK (failsAt ("\"\\x\"", 1, 2, "invalid escape"));
    CHECK (failsAt ("\"ab\\'\"", 1, 4, "invalid escape"));
    CHECK (failsAt ("\"\\u12G4\"", 1, 2, "4 hex digits"));
    CHECK (failsAt ("\"\\u12\"", 1, 2, "4 hex digits"));
    CHECK (failsAt ("\"\\ud800\"", 1, 2, "high surrogate"));
    CHECK (failsAt ("\"\\ud800\\u0041\"", 1, 2, "high surrogate"));
    CHECK (failsAt ("\"\\udc00\"", 1, 2, "low surrogate"));
    CHECK (failsAt ("\"tab\there\"", 1, 5, "control character U+0009"));
    CHECK (failsAt ("\"line\nbreak\"", 1, 6, "control character"));
    CHECK (failsAt ("\"bad \xC3\x28\"", 1, 6, "invalid UTF-8"));
    CHECK (failsAt ("\"overlong \xC0\xAF\"", 1, 11, "invalid UTF-8"));
    CHECK (failsAt ("\"surrogate \xED\xA0\x80\"", 1, 12, "invalid UTF-8"));
    CHECK (failsAt ("\"too big \xF4\x90\x80\x80\"", 1, 10, "invalid UTF-8"));
    CHECK (failsAt ("\"cut \xE2\x82\"", 1, 6, "invalid UTF-8"));
    CHECK (failsAt ("{\"a\" 1}", 1, 6, "expected ':'"));
    CHECK (failsAt ("{1: 2}", 1, 2, "expected a string key"));
    CHECK (failsAt ("{'a': 2}", 1, 2, "expected a string key"));
    CHECK (failsAt ("[1 2]", 1, 4, "expected ',' or ']'"));
    CHECK (failsAt ("{\"a\": 1 \"b\": 2}", 1, 9, "expected ',' or '}'"));
    CHECK (failsAt ("[1, 2", 1, 1, "unterminated array"));
    CHECK (failsAt ("{\"a\": [1, {\"b\": 2}", 1, 7, "unterminated array")); // innermost open container
    CHECK (failsAt ("{\"a\": 1", 1, 1, "unterminated object"));
    CHECK (failsAt ("[", 1, 1, "unterminated array"));
    CHECK (failsAt ("tru", 1, 1, "invalid literal"));
    CHECK (failsAt ("nul", 1, 1, "invalid literal"));
    CHECK (failsAt ("True", 1, 1, "expected a value"));
    CHECK (failsAt ("", 1, 1, "unexpected end of input"));
    CHECK (failsAt ("   \n  ", 2, 3, "unexpected end of input"));
    CHECK (failsAt ("{} x", 1, 4, "after the end"));
    CHECK (failsAt ("[1]]", 1, 4, "after the end"));
    CHECK (failsAt (std::string ("[1,\0 2]", 7), 1, 4, "byte 0x00"));
    CHECK (failsAt ("[\"\xC3\xA9\xC3\xA9\", x]", 1, 8, "'x'")); // column counts code points, not bytes
    CHECK (failsAt ("{\n\t\"a\": 1,\r\n\t\"b\": 01\r\n}", 3, 7, "leading zeros"));

    // A failed parse leaves the output untouched.
    Value keep ("kept");
    std::string error;
    CHECK (! parse ("[1,", keep, error));
    CHECK (keep.asString() == "kept");
    CHECK (! error.empty());
}

TEST_CASE ("Json: nesting depth is limited to 256 without stack overflow")
{
    const auto nested = [] (int depth, char open, char close, const std::string& inner)
    { return std::string (static_cast<size_t> (depth), open) + inner + std::string (static_cast<size_t> (depth), close); };

    CHECK (parseOrFail (nested (256, '[', ']', "1")).isArray());
    CHECK (failsAt (nested (257, '[', ']', "1"), 1, 257, "deeper than 256"));
    CHECK (failsAt (nested (1000, '[', ']', ""), 1, 257, "deeper than 256"));
    CHECK (fails (std::string (1000000, '['))); // stops at level 257, no deep recursion

    std::string objects;
    for (int i = 0; i < 1000; ++i)
        objects += "{\"k\":";
    CHECK (failsAt (objects, 1, 256 * 5 + 1, "deeper than 256"));

    // 256 levels of mixed containers still parse and re-serialise identically.
    std::string mixed;
    for (int i = 0; i < 128; ++i)
        mixed += "{\"a\":[";
    mixed += "null";
    for (int i = 0; i < 128; ++i)
        mixed += "]}";
    const Value v = parseOrFail (mixed);
    CHECK (write (v, 0) == mixed);
}

TEST_CASE ("Json: accessors, operator[] on missing keys, set() and push()")
{
    const Value number (2.5);
    CHECK (number.isNumber() && ! number.isString());
    CHECK (number.asNumber() == 2.5);
    CHECK (number.asNumber (7.0) == 2.5);
    CHECK (number.asBool (true) == true); // fallback: not a bool
    CHECK (number.asString().empty());
    CHECK (number.asArray().empty());
    CHECK (number.asObject().empty());
    CHECK (&number.asString() == &Value ("x")["missing"].asString()); // shared static empty string
    CHECK (number["key"].isNull());

    const Value text ("hello");
    CHECK (text.asNumber (-1.0) == -1.0);
    CHECK (text.asBool() == false);
    CHECK (Value (false).asBool (true) == false);
    CHECK (Value (7).asNumber() == 7.0);

    Value obj = parseOrFail (R"({"a": {"b": {"c": 5}}, "arr": [1, 2]})");
    CHECK (obj["a"]["b"]["c"].asNumber() == 5.0);
    CHECK (obj["missing"].isNull());
    CHECK (obj["missing"]["deeper"]["still"].isNull());
    CHECK (obj["a"]["missing"].asNumber (3.0) == 3.0);
    CHECK (obj["arr"]["x"].isNull()); // operator[] on an array is a null, not a crash
    CHECK (&obj["missing"] == &obj["other"]); // one shared static null

    // set(): null becomes an object, existing keys are replaced in place, new keys append.
    Value v;
    CHECK (v.isNull());
    v.set ("x", 1);
    v.set ("y", "two");
    v.set ("x", 3.5);
    REQUIRE (v.isObject());
    REQUIRE (v.asObject().size() == 2);
    CHECK (v.asObject()[0].first == "x" && v.asObject()[0].second.asNumber() == 3.5);
    CHECK (v.asObject()[1].first == "y" && v["y"].asString() == "two");
    Value copy = v["y"];
    copy.set ("nested", true); // a string is replaced by an object so set() always applies
    CHECK (copy.isObject() && copy["nested"].asBool());
    CHECK (v["y"].asString() == "two"); // the copy is independent

    // push(): null becomes an array; values keep order.
    Value arr;
    arr.push (1);
    arr.push ("b");
    arr.push (Value::Object {});
    REQUIRE (arr.isArray());
    CHECK (arr.asArray().size() == 3);
    CHECK (arr.asArray()[1].asString() == "b");
    CHECK (write (arr, 0) == R"([1,"b",{}])");
    Value num (4.0);
    num.push (true);
    CHECK (num.isArray() && num.asArray().size() == 1);

    // Values built in code serialise and parse back identically.
    Value root;
    root.set ("list", arr);
    root.set ("obj", v);
    root.set ("nothing", nullptr);
    CHECK (deepEqual (parseOrFail (write (root)), root));
}

TEST_CASE ("Json: BOM, whitespace and top-level scalars")
{
    CHECK (parseOrFail ("\xEF\xBB\xBF{\"a\": 1}")["a"].asNumber() == 1.0);
    CHECK (parseOrFail (" \t\r\n true \n").asBool() == true);
    CHECK (parseOrFail ("null").isNull());
    CHECK (parseOrFail ("\"s\"").asString() == "s");
    CHECK (parseOrFail ("-7.25").asNumber() == -7.25);
    CHECK (parseOrFail ("[ ]").asArray().empty());
    CHECK (parseOrFail ("{ \n }").asObject().empty());
    CHECK (fails ("\xEF\xBB\xBF"));
    CHECK (failsAt ("\xEF\xBB\xBF[1,]", 1, 4, "trailing comma")); // the BOM is not a column
    CHECK (fails ("\f[1]")); // form feed is not JSON whitespace
}

TEST_CASE ("Json: extreme and non-finite numbers are written as valid JSON")
{
    // JSON cannot express NaN / Infinity; they serialise as null so the output always parses.
    CHECK (write (Value (std::numeric_limits<double>::quiet_NaN())) == "null");
    CHECK (write (Value (std::numeric_limits<double>::infinity())) == "null");
    CHECK (write (Value (-std::numeric_limits<double>::infinity())) == "null");

    Value arr;
    for (double d : { DBL_MAX, -DBL_MAX, DBL_MIN, std::numeric_limits<double>::denorm_min(), -0.0, 1e308, -1e-308, 0.0 })
        arr.push (d);
    arr.push (std::numeric_limits<double>::quiet_NaN());
    const Value back = parseOrFail (write (arr, 2));
    REQUIRE (back.asArray().size() == 9);
    for (size_t i = 0; i < 8; ++i)
        CHECK (sameNumber (back.asArray()[i].asNumber(), arr.asArray()[i].asNumber()));
    CHECK (back.asArray()[8].isNull());
}

TEST_CASE ("Json: fuzzed inputs never crash; accepted documents re-serialise stably")
{
    const std::string base = kPresetText;
    FastRandom rng (99);
    const char alphabet[] = "{}[]\",:\\/ 0123456789.eE+-tfnulrsa\n\t\x01\xC3\xA9\xF0\x9F";
    int accepted = 0;
    for (int iter = 0; iter < 3000; ++iter)
    {
        std::string text = base;
        const int mutations = 1 + static_cast<int> (rng.nextU32() % 4);
        for (int m = 0; m < mutations; ++m)
        {
            const size_t at = rng.nextU32() % text.size();
            switch (rng.nextU32() % 3)
            {
                case 0: text[at] = alphabet[rng.nextU32() % (sizeof (alphabet) - 1)]; break;
                case 1: text.erase (at, 1 + rng.nextU32() % 3); break;
                default: text.insert (at, 1, alphabet[rng.nextU32() % (sizeof (alphabet) - 1)]); break;
            }
            if (text.empty())
                text = "x";
        }
        if ((rng.nextU32() & 7u) == 0)
            text.resize (rng.nextU32() % text.size());

        Value v;
        std::string error;
        if (parse (text, v, error))
        {
            ++accepted;
            const std::string once = write (v, 2);
            Value again;
            REQUIRE (parse (once, again, error));
            CHECK (deepEqual (v, again));
            CHECK (write (again, 2) == once);
        }
        else
        {
            CHECK (fails (text));
        }
    }
    CHECK (accepted > 0);

    // Pure random bytes.
    for (int iter = 0; iter < 500; ++iter)
    {
        std::string text (static_cast<size_t> (rng.nextU32() % 64), ' ');
        for (auto& c : text)
            c = static_cast<char> (rng.nextU32() >> 24);
        Value v;
        std::string error;
        if (! parse (text, v, error))
            CHECK (! error.empty());
    }
}

TEST_CASE ("Json: const accessors and lookups do not allocate")
{
    const Value doc = parseOrFail (R"({"a": 1, "b": "str", "c": [1, 2], "d": {"e": true}})");
    const std::string keyA = "a", keyD = "d", keyE = "e", missing = "zz";
    const Value number (1.0);
    (void) number.asString(); // make sure the static empties exist before measuring
    (void) number.asArray();
    (void) number.asObject();
    (void) number[keyA];

    double sink = 0.0;
    size_t sizes = 0;
    {
        AllocationGuard guard;
        for (int i = 0; i < 100; ++i)
        {
            sink += doc[keyA].asNumber();
            sink += doc[keyD][keyE].asBool() ? 1.0 : 0.0;
            sink += doc[missing].asNumber (0.5);
            sizes += doc["b"].asString().size(); // short literal key: SSO, no heap
            sizes += number.asString().size() + number.asArray().size() + number.asObject().size();
            sizes += doc["c"].asArray().size() + doc[keyD].asObject().size();
            sizes += doc.isObject() && number[keyA].isNull() ? 1u : 0u;
        }
        CHECK (guard.allocations() == 0);
    }
    CHECK (sink > 0.0);
    CHECK (sizes > 0);
}

// ---- adversarial review tests ----

TEST_CASE ("Json review: set() with a key that aliases the value being replaced")
{
    // v is a string and the key is that very string: converting v to an object used to
    // destroy the key before it was copied into the new member (use after free).
    Value v (std::string ("a key long enough to live on the heap, not in SSO"));
    v.set (v.asString(), 1);
    REQUIRE (v.isObject());
    REQUIRE (v.asObject().size() == 1);
    CHECK (v.asObject()[0].first == "a key long enough to live on the heap, not in SSO");
    CHECK (v["a key long enough to live on the heap, not in SSO"].asNumber() == 1.0);

    // Key taken from a member's own value; appending may reallocate the member vector.
    Value o;
    o.set ("k", "another key long enough to avoid small string optimisation");
    for (int i = 0; i < 20; ++i)
        o.set ("pad" + std::to_string (i), i);
    o.set (o["k"].asString(), true);
    CHECK (o["another key long enough to avoid small string optimisation"].asBool());
    // Key aliasing the value it replaces.
    o.set ("self", "self");
    o.set (o["self"].asString(), 2);
    CHECK (o["self"].asNumber() == 2.0);
}

TEST_CASE ("Json review: lone CR line endings count as new lines in error positions")
{
    CHECK (failsAt ("[1,\r2,\r]", 3, 1, "trailing comma"));
    CHECK (failsAt ("[1,\r\n2,\r\n]", 3, 1, "trailing comma")); // CRLF is still one line
    CHECK (failsAt ("\r\r\n\n x", 4, 2, "expected a value"));
}

TEST_CASE ("Json review: numbers at the edges of double precision")
{
    CHECK (sameNumber (parseOrFail ("1e-310").asNumber(), 1e-310)); // subnormal, not flushed to 0
    CHECK (sameNumber (parseOrFail ("2.2250738585072011e-308").asNumber(), 2.2250738585072011e-308));
    CHECK (sameNumber (parseOrFail ("9007199254740993").asNumber(), 9007199254740992.0)); // round-half-even
    CHECK (sameNumber (parseOrFail ("1.7976931348623158e308").asNumber(), DBL_MAX)); // rounds down to max
    CHECK (failsAt ("1.7976931348623159e308", 1, 1, "out of range"));
    // Very long mantissas: correct rounding, and underflow / overflow classification by magnitude.
    CHECK (parseOrFail ("0." + std::string (1000, '0') + "1").asNumber() == 0.0);
    CHECK (parseOrFail ("0." + std::string (300, '0') + "1e300").asNumber() == 1e-1);
    CHECK (failsAt ("1" + std::string (400, '0'), 1, 1, "out of range"));
    CHECK (sameNumber (parseOrFail ("1" + std::string (300, '0') + "e-300").asNumber(), 1.0));
    CHECK (sameNumber (parseOrFail ("-" + std::string ("0.") + std::string (400, '0') + "1").asNumber(), -0.0));
}

TEST_CASE ("Json review: string edge cases")
{
    // Raw NUL / DEL inside strings: NUL is a control character, DEL is allowed.
    CHECK (failsAt (std::string ("\"a\0b\"", 5), 1, 3, "U+0000"));
    CHECK (parseOrFail ("\"a\x7F\"").asString() == "a\x7F");
    // High surrogate followed by a broken \u escape, or by a second high surrogate.
    CHECK (failsAt ("\"\\ud83c\\u12\"", 1, 2, "high surrogate"));
    CHECK (failsAt ("\"\\ud83c\\ud83c\"", 1, 2, "high surrogate"));
    CHECK (failsAt ("\"\\ud83c", 1, 2, "high surrogate"));
    // Escaped keys round trip and are found by operator[].
    const Value v = parseOrFail ("{\"a\\nb\\u00e9\": 1, \"\": 2}");
    CHECK (v["a\nb\xC3\xA9"].asNumber() == 1.0);
    CHECK (v[""].asNumber() == 2.0);
    CHECK (deepEqual (parseOrFail (write (v, 3)), v));
    // Invalid UTF-8 in keys built in code is repaired too.
    Value k;
    k.set ("\xFF", 1);
    CHECK (parseOrFail (write (k))["\xEF\xBF\xBD"].asNumber() == 1.0);
}

TEST_CASE ("Json review: writer indentation is clamped and output is always re-parseable")
{
    Value v;
    v.set ("a", Value::Array { Value (1), Value::Array {}, Value::Object {} });
    const std::string big = write (v, 1000);
    CHECK (big == write (v, 32));
    CHECK (big.find ("\n" + std::string (64, ' ') + "1") != std::string::npos); // level 2 x 32 spaces
    CHECK (big.find (std::string (65, ' ')) == std::string::npos);
    CHECK (deepEqual (parseOrFail (big), v));
    CHECK (write (v, std::numeric_limits<int>::min()) == write (v, 0));

    // A 256-deep document (the parse limit) pretty-prints and parses back.
    Value deep = Value (1);
    for (int i = 0; i < 256; ++i)
        deep = Value (Value::Array { deep });
    CHECK (deepEqual (parseOrFail (write (deep, 2)), deep));
    Value tooDeep = Value (Value::Array { deep });
    CHECK (fails (write (tooDeep, 0))); // 257 levels: the writer can emit it, the parser refuses it cleanly
}
