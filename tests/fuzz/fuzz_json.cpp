// Flubsound Pro - libFuzzer target: flub::json::parse / write (docs/11 E53).
//
// Any byte string: parse must return (no crash, no sanitizer report, no
// unbounded recursion) and give an error message when it refuses. An
// accepted document must write, parse back and write again to the same text,
// compact and pretty-printed.

#include "FuzzCheck.h"

#include "flub/io/Json.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput (const uint8_t* data, size_t size)
{
    const std::string text (reinterpret_cast<const char*> (data), size);
    flub::json::Value value;
    std::string error;
    if (! flub::json::parse (text, value, error))
    {
        FUZZ_CHECK (! error.empty());
        return 0;
    }

    for (const int indent : { 0, 2 })
    {
        const auto written = flub::json::write (value, indent);
        flub::json::Value back;
        FUZZ_CHECK (flub::json::parse (written, back, error));
        FUZZ_CHECK (flub::json::write (back, indent) == written);
    }
    return 0;
}
