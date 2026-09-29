// Flubsound Pro - shared helper of the libFuzzer targets (docs/11 E53 step 1).
//
// FUZZ_CHECK states a property every accepted input must have (a round trip,
// a range). A failed check prints the condition and aborts, which libFuzzer
// reports like a crash and saves the input as crash-<sha1>.
#pragma once

#include <cstdio>
#include <cstdlib>

#define FUZZ_CHECK(cond)                                                                        \
    do                                                                                          \
    {                                                                                           \
        if (! (cond))                                                                           \
        {                                                                                       \
            std::fprintf (stderr, "%s:%d: FUZZ_CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                                       \
        }                                                                                       \
    } while (false)
