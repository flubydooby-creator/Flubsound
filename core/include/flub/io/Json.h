// Flubsound Pro - minimal JSON value, parser and writer (presets / settings).
//
// Full RFC 8259 grammar (objects, arrays, strings with escapes incl. \uXXXX
// and surrogate pairs, numbers, true/false/null). Objects preserve insertion
// order so saved presets diff cleanly. Not for the audio thread.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace flub::json
{
class Value
{
public:
    using Array = std::vector<Value>;
    using Object = std::vector<std::pair<std::string, Value>>; // ordered

    Value() = default; // null
    Value (std::nullptr_t) {}
    Value (bool b) : data (b) {}
    Value (double d) : data (d) {}
    Value (int i) : data (static_cast<double> (i)) {}
    Value (const char* s) : data (std::string (s)) {}
    Value (std::string s) : data (std::move (s)) {}
    Value (Array a) : data (std::move (a)) {}
    Value (Object o) : data (std::move (o)) {}

    bool isNull() const noexcept { return std::holds_alternative<std::monostate> (data); }
    bool isBool() const noexcept { return std::holds_alternative<bool> (data); }
    bool isNumber() const noexcept { return std::holds_alternative<double> (data); }
    bool isString() const noexcept { return std::holds_alternative<std::string> (data); }
    bool isArray() const noexcept { return std::holds_alternative<Array> (data); }
    bool isObject() const noexcept { return std::holds_alternative<Object> (data); }

    bool asBool (bool fallback = false) const noexcept;
    double asNumber (double fallback = 0.0) const noexcept;
    const std::string& asString() const; // empty string if not a string
    const Array& asArray() const;        // empty if not an array
    const Object& asObject() const;      // empty if not an object

    /** Object member lookup; returns a null Value if missing. */
    const Value& operator[] (const std::string& key) const;
    /** Sets (or appends) an object member; converts null to an object. */
    void set (const std::string& key, Value v);
    /** Appends to an array; converts null to an array. */
    void push (Value v);

private:
    std::variant<std::monostate, bool, double, std::string, Array, Object> data;
};

/** Parses text; on failure returns false and sets error ("line:col: msg"). */
bool parse (const std::string& text, Value& out, std::string& error);

/** Serialises; indent > 0 pretty-prints. Numbers use shortest round-trip form. */
std::string write (const Value& v, int indent = 2);
} // namespace flub::json
