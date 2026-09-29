// Flubsound Pro CLI - `flubsound-cli latency-probe` (docs/11 E42d): writes
// the loopback probe (exponential sweeps) to a WAV file and measures the
// delay of a recording of it with flub::latency::LatencyProbe. main.cpp
// hands it the arguments after the command word; it parses its own options.
#pragma once

#include <string>
#include <vector>

namespace flub::cli
{
extern const char* const kLatencyProbeHelp;

/** Runs `latency-probe <args>` and returns the exit code (0 ok, 1 I/O error or
    no valid measurement, 2 usage error). The report (text, or JSON with
    --json) goes to *out when given (tests), else to stdout; errors and
    progress go to stderr. */
int runLatencyProbe (const std::vector<std::string>& args, std::string* out = nullptr);
} // namespace flub::cli
