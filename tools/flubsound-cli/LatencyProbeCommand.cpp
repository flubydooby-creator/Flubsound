#include "LatencyProbeCommand.h"

#include "flub/analysis/LatencyProbe.h"
#include "flub/io/Json.h"
#include "flub/io/WavFile.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

namespace flub::cli
{
const char* const kLatencyProbeHelp = R"(flubsound-cli latency-probe generate -o probe.wav [--rate R] [probe options] [--format f32|pcm24|pcm16]
flubsound-cli latency-probe analyze  -i recording.wav [--channel N] [--ref-channel M] [probe options]
                                     [--min-snr dB] [--json]

Measures the real delay of a playback -> capture path (docs/11 E42d): a
loopback cable from an output to an input, or a measurement microphone at
the earcup, with or without Flubsound in the path.

generate writes the probe: --lead-in-ms of silence, then --runs exponential
sine sweeps (20 Hz .. 20 kHz, --sweep-ms long, peak --level dBFS), each
followed by a gap of --max-delay-ms + 250 ms. The same probe is on every
channel (--channels, default 2).

analyze deconvolves each run of the recording (FFT division by the sweep's
spectrum), takes the impulse response's largest peak (either polarity)
between 0 and --max-delay-ms, refined to a fraction of a sample by a
parabola, and reports the median over the runs whose peak is at least
--min-snr dB (default 30) above the rest of the response. Fewer than half
of the runs above it: no result, exit code 1. Use the probe options the
probe was generated with.

  Absolute (default): the recording must start sample-locked with the
  playback of probe.wav - play and record in one duplex stream (a DAW with
  its own latency compensation turned off, Audacity with "latency
  correction" 0, JACK / PipeWire clients started by one transport). The
  result is the path's round trip: output buffering + converters + cable
  or air + input buffering (+ Flubsound when the probe goes through it).
  Relative (--ref-channel M): channel M recorded the probe over a direct
  path (e.g. a second cable from the same output, or the unprocessed
  output), so the start offset cancels; the result is the delay of
  --channel against it, -10 .. max-delay - 10 ms.

Channels are numbered from 1 (--channel defaults to 1).

Probe options: --runs N (1..100, default 10), --sweep-ms (100..10000,
default 1000), --max-delay-ms (1..5000, default 500), --level dBFS (-60..0,
default -12), --lead-in-ms (0..10000, default 500), --start-hz (default 20),
--end-hz (default 20000, at most 0.45 x the rate). --rate (generate,
default 48000); analyze takes the rate of the file.

Examples:
  flubsound-cli latency-probe generate -o probe.wav --rate 48000
  flubsound-cli latency-probe analyze -i loopback.wav
  flubsound-cli latency-probe analyze -i capture.wav --channel 1 --ref-channel 2 --json
)";

namespace
{
constexpr int kOk = 0, kFailure = 1, kUsage = 2;

/** Strict finite decimal number: the whole string must be consumed. */
bool parseNumber (const std::string& text, double& value)
{
    if (text.empty() || text.find_first_not_of ("0123456789+-.eE") != std::string::npos)
        return false;
    errno = 0;
    char* end = nullptr;
    value = std::strtod (text.c_str(), &end);
    return errno == 0 && end == text.c_str() + text.size() && std::isfinite (value);
}

std::string printfString (const char* pattern, double a, double b = 0.0, double c = 0.0, double d = 0.0)
{
    char buf[256];
    std::snprintf (buf, sizeof (buf), pattern, a, b, c, d);
    return buf;
}

double round2 (double v)
{
    return std::round (v * 100.0) / 100.0;
}

struct Options
{
    std::string command, input, output;
    latency::ProbeSettings probe;
    int channel = 1, refChannel = 0, channels = 2;
    io::SampleFormat format = io::SampleFormat::Float32;
    bool json = false, quiet = false;
};

bool parse (const std::vector<std::string>& args, Options& o, std::string& error)
{
    if (args.empty())
    {
        error = "latency-probe needs a subcommand: generate or analyze";
        return false;
    }
    o.command = args[0] == "analyse" ? "analyze" : args[0];
    if (o.command != "generate" && o.command != "analyze")
    {
        error = "unknown latency-probe subcommand '" + args[0] + "' (expected generate or analyze)";
        return false;
    }
    const bool generate = o.command == "generate";
    for (size_t i = 1; i < args.size(); ++i)
    {
        const std::string& a = args[i];
        if (a == "--json")
        {
            o.json = true;
            continue;
        }
        if (a == "-q" || a == "--quiet")
        {
            o.quiet = true;
            continue;
        }
        if (i + 1 >= args.size())
        {
            error = "option '" + a + "' is unknown or needs a value";
            return false;
        }
        const std::string& v = args[++i];
        double d = 0.0;
        const auto number = [&] (double lo, double hi, double& target) {
            if (! parseNumber (v, d) || d < lo || d > hi)
            {
                error = a + " expects a number from " + printfString ("%g", lo) + " to " + printfString ("%g", hi) + ", got '" + v + "'";
                return false;
            }
            target = d;
            return true;
        };
        const auto integer = [&] (int lo, int hi, int& target) {
            double t = 0.0;
            if (! number (lo, hi, t))
                return false;
            if (t != std::floor (t))
            {
                error = a + " expects a whole number, got '" + v + "'";
                return false;
            }
            target = static_cast<int> (t);
            return true;
        };
        double ms = 0.0;
        bool ok = true;
        if (a == "-o" || a == "--output")
            o.output = v;
        else if (a == "-i" || a == "--input")
            o.input = v;
        else if (a == "--runs")
            ok = integer (1, 100, o.probe.runs);
        else if (a == "--sweep-ms")
            ok = number (100.0, 10000.0, ms) && ((o.probe.sweepSeconds = ms * 0.001), true);
        else if (a == "--max-delay-ms")
            ok = number (1.0, 5000.0, o.probe.maxDelayMs);
        else if (a == "--level")
            ok = number (-60.0, 0.0, o.probe.levelDbfs);
        else if (a == "--lead-in-ms")
            ok = number (0.0, 10000.0, ms) && ((o.probe.leadInSeconds = ms * 0.001), true);
        else if (a == "--start-hz")
            ok = number (10.0, 1000.0, o.probe.startHz);
        else if (a == "--end-hz")
            ok = number (1000.0, 384000.0, o.probe.endHz);
        else if (generate && a == "--rate")
            ok = number (8000.0, 768000.0, o.probe.sampleRate);
        else if (generate && a == "--channels")
            ok = integer (1, 8, o.channels);
        else if (generate && (a == "-f" || a == "--format"))
        {
            if (v == "f32")
                o.format = io::SampleFormat::Float32;
            else if (v == "pcm24")
                o.format = io::SampleFormat::Pcm24;
            else if (v == "pcm16")
                o.format = io::SampleFormat::Pcm16;
            else
            {
                error = "--format expects f32, pcm24 or pcm16, got '" + v + "'";
                return false;
            }
        }
        else if (! generate && a == "--min-snr")
            ok = number (0.0, 120.0, o.probe.minSnrDb);
        else if (! generate && a == "--channel")
            ok = integer (1, 64, o.channel);
        else if (! generate && a == "--ref-channel")
            ok = integer (1, 64, o.refChannel);
        else
        {
            error = "unknown option '" + a + "' for latency-probe " + o.command;
            return false;
        }
        if (! ok)
            return false;
    }
    if (generate && o.output.empty())
    {
        error = "latency-probe generate needs -o <probe.wav>";
        return false;
    }
    if (! generate && o.input.empty())
    {
        error = "latency-probe analyze needs -i <recording.wav>";
        return false;
    }
    if (o.refChannel == o.channel)
    {
        error = "--ref-channel must differ from --channel";
        return false;
    }
    return true;
}

int generate (const Options& o)
{
    if (const auto why = latency::validate (o.probe); ! why.empty())
    {
        std::fprintf (stderr, "error: %s\n", why.c_str());
        return kUsage;
    }
    io::AudioFileData data;
    data.sampleRate = o.probe.sampleRate;
    data.numChannels = o.channels;
    data.channels.assign (static_cast<size_t> (o.channels), latency::makeProbe (o.probe));
    std::string error;
    if (! io::writeWav (o.output, data, o.format, error))
    {
        std::fprintf (stderr, "error: %s\n", error.c_str());
        return kFailure;
    }
    if (! o.quiet)
        std::fprintf (stderr, "Wrote %s: %.2f s at %.0f Hz, %d sweeps of %.0f ms (max delay %.0f ms)\n", o.output.c_str(),
                      static_cast<double> (data.numFrames()) / data.sampleRate, data.sampleRate, o.probe.runs,
                      o.probe.sweepSeconds * 1000.0, o.probe.maxDelayMs);
    return kOk;
}

json::Value toJson (const latency::Result& r, const Options& o, double sampleRate)
{
    json::Value v;
    v.set ("ok", r.ok);
    if (! r.ok)
        v.set ("error", r.error);
    v.set ("relative", o.refChannel > 0);
    v.set ("sampleRate", sampleRate);
    v.set ("delaySamples", r.ok ? json::Value (std::round (r.delaySamples * 1000.0) / 1000.0) : json::Value());
    v.set ("delayMs", r.ok ? json::Value (std::round (r.delayMs * 1000.0) / 1000.0) : json::Value());
    v.set ("spreadSamples", r.ok ? json::Value (round2 (r.spreadSamples)) : json::Value());
    v.set ("inverted", r.inverted);
    // The strongest arrival > 1 ms from the peak, dB re the peak (an echo or
    // a second path; -200 when none).
    v.set ("secondaryDb", r.ok ? json::Value (round2 (r.secondaryDb)) : json::Value());
    v.set ("acceptedRuns", r.acceptedRuns);
    v.set ("minSnrDb", o.probe.minSnrDb);
    json::Value runs { json::Value::Array {} };
    for (const auto& run : r.runs)
    {
        json::Value j;
        j.set ("index", run.index);
        j.set ("delaySamples", std::round (run.delaySamples * 1000.0) / 1000.0);
        j.set ("snrDb", round2 (run.snrDb));
        j.set ("secondaryDb", round2 (run.secondaryDb));
        j.set ("inverted", run.inverted);
        j.set ("accepted", run.accepted);
        runs.push (std::move (j));
    }
    v.set ("runs", std::move (runs));
    return v;
}

std::string toText (const latency::Result& r, const Options& o)
{
    std::string s;
    for (const auto& run : r.runs)
        s += printfString ("Run %2.0f  : %9.2f samples  SNR %5.1f dB", run.index + 1.0, run.delaySamples, run.snrDb)
             + (run.inverted ? "  inverted" : "") + (run.accepted ? "" : "  rejected") + "\n";
    if (! r.ok)
        return s + "No result: " + r.error + "\n";
    s += printfString ("Delay   : %.1f samples = %.3f ms", r.delaySamples, r.delayMs)
         + printfString (" (median of %.0f / %.0f runs, spread %.2f samples)", r.acceptedRuns, static_cast<double> (r.runs.size()),
                         r.spreadSamples)
         + (o.refChannel > 0 ? printfString (", channel %.0f against channel %.0f", o.channel, o.refChannel) : std::string())
         + (r.inverted ? ", polarity inverted" : "") + "\n";
    if (r.secondaryDb > latency::Run::kNoSecondaryDb)
        s += printfString ("Echo    : the strongest other arrival is %.1f dB under the peak", -r.secondaryDb)
             + (r.secondaryDb > -6.0 ? " (ambiguous: a second path nearly as strong)" : "") + "\n";
    return s;
}

int analyze (const Options& o, std::string* out)
{
    io::AudioFileData data;
    std::string error;
    if (! io::readWav (o.input, data, error))
    {
        std::fprintf (stderr, "error: %s\n", error.c_str());
        return kFailure;
    }
    if (o.channel > data.numChannels || o.refChannel > data.numChannels)
    {
        std::fprintf (stderr, "error: %s has %d channel(s); --channel / --ref-channel count from 1\n", o.input.c_str(), data.numChannels);
        return kUsage;
    }
    auto settings = o.probe;
    settings.sampleRate = data.sampleRate;
    if (const auto why = latency::validate (settings); ! why.empty())
    {
        std::fprintf (stderr, "error: %s\n", why.c_str());
        return kUsage;
    }

    latency::LatencyProbe probe (settings);
    const auto& measured = data.channels[static_cast<size_t> (o.channel - 1)];
    const auto result = o.refChannel > 0
                            ? probe.analyseRelative (measured.data(), data.channels[static_cast<size_t> (o.refChannel - 1)].data(), data.numFrames())
                            : probe.analyse (measured.data(), data.numFrames());

    const auto text = o.json ? json::write (toJson (result, o, data.sampleRate)) + "\n" : toText (result, o);
    if (out != nullptr)
        *out = text;
    else
        std::fputs (text.c_str(), stdout);
    if (! result.ok && o.json)
        std::fprintf (stderr, "No result: %s\n", result.error.c_str());
    return result.ok ? kOk : kFailure;
}
} // namespace

int runLatencyProbe (const std::vector<std::string>& args, std::string* out)
{
    if (! args.empty() && (args[0] == "--help" || args[0] == "-h" || args[0] == "help"))
    {
        if (out != nullptr)
            *out = kLatencyProbeHelp;
        else
            std::fputs (kLatencyProbeHelp, stdout);
        return kOk;
    }
    Options o;
    std::string error;
    if (! parse (args, o, error))
    {
        std::fprintf (stderr, "error: %s\nRun `flubsound-cli latency-probe --help` for usage.\n", error.c_str());
        return kUsage;
    }
    try
    {
        return o.command == "generate" ? generate (o) : analyze (o, out);
    }
    catch (const std::exception& e) // std::bad_alloc on a huge file, ...
    {
        std::fprintf (stderr, "error: %s\n", e.what());
        return kFailure;
    }
}
} // namespace flub::cli
