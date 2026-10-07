#include "LatencyMeasurement.h"

#include <cstdarg>
#include <cstdio>

namespace flub::app::latency
{
namespace
{
#if defined(__GNUC__)
__attribute__ ((format (printf, 1, 2)))
#endif
std::string fmt (const char* pattern, ...)
{
    char buffer[512];
    va_list args;
    va_start (args, pattern);
    std::vsnprintf (buffer, sizeof (buffer), pattern, args);
    va_end (args);
    return buffer;
}

double toMs (double samples, double rate) noexcept
{
    return rate > 0.0 ? 1000.0 * samples / rate : 0.0;
}

/** A result first, then the most accepted runs, then the highest median SNR. */
bool better (const flub::latency::Result& a, const flub::latency::Result& b) noexcept
{
    if (a.ok != b.ok)
        return a.ok;
    if (a.acceptedRuns != b.acceptedRuns)
        return a.acceptedRuns > b.acceptedRuns;
    return a.medianSnrDb > b.medianSnrDb;
}

float peakOf (const std::vector<float>& x) noexcept
{
    float p = 0.0f;
    for (const float v : x)
        p = std::max (p, std::abs (v));
    return p;
}
} // namespace

flub::latency::ProbeSettings liveProbeSettings (double sampleRate)
{
    flub::latency::ProbeSettings s;
    s.sampleRate = sampleRate;
    s.sweepSeconds = 0.5;
    s.startHz = 20.0;
    s.endHz = 20000.0;
    s.levelDbfs = kProbeLevelDbfs;
    s.maxDelayMs = 500.0;
    s.leadInSeconds = 0.3;
    s.runs = 5;
    s.minSnrDb = 30.0;
    return s;
}

// =============================================================================
ProbeSession::ProbeSession (const flub::latency::ProbeSettings& probeSettings, Path probePath, int probeStrip, int channelsToRecord)
    : settings (probeSettings), path (probePath), strip (probeStrip),
      recordChannels (std::clamp (channelsToRecord, 0, kMaxRecordChannels))
{
    probe = flub::latency::makeProbe (settings);
    for (int c = 0; c < recordChannels; ++c)
        rec[static_cast<size_t> (c)].assign (probe.size(), 0.0f);
    fade = std::max<int64_t> (1, std::llround (kFadeMs * 0.001 * settings.sampleRate));
    tail = std::llround (kCancelTailMs * 0.001 * settings.sampleRate);
    end = stopAt = length();
    capLinear = static_cast<float> (std::pow (10.0, kThroughCapDbfs / 20.0));
}

double ProbeSession::getCapGainDb() const noexcept
{
    const float g = capGainStat.load (std::memory_order_relaxed);
    return g >= 1.0f ? 0.0 : 20.0 * std::log10 (std::max (g, 1.0e-6f));
}

// =============================================================================
const char* confidenceName (Confidence c) noexcept
{
    switch (c)
    {
        case Confidence::High: return "high";
        case Confidence::Medium: return "medium";
        case Confidence::Low: return "low";
        case Confidence::None: break;
    }
    return "none";
}

PassReport analyse (const ProbeSession& session, const DeviceContext& context)
{
    PassReport report;
    report.path = session.getPath();
    report.context = context;
    const auto& settings = session.getSettings();
    const double rate = settings.sampleRate;
    report.runs = settings.runs;
    report.capGainDb = session.getCapGainDb();
    if (context.glitchesBefore >= 0 && context.glitchesAfter >= context.glitchesBefore)
        report.glitches = context.glitchesAfter - context.glitchesBefore;

    if (session.wasCancelled())
    {
        report.error = "Cancelled.";
        return report;
    }
    if (session.getRecordChannels() <= 0)
    {
        report.error = "No input is open, so nothing was recorded. Choose the microphone (or the line input your cable goes to) as the "
                       "input device in Settings > Audio.";
        return report;
    }

    flub::latency::LatencyProbe probe (settings);
    float loudest = 0.0f;
    for (int c = 0; c < session.getRecordChannels(); ++c)
    {
        const auto& recording = session.recording (c);
        loudest = std::max (loudest, peakOf (recording));
        const auto result = probe.analyse (recording.data(), static_cast<int64_t> (recording.size()));
        if (c == 0 || better (result, report.result))
        {
            report.result = result;
            report.channel = c;
        }
    }

    const auto& r = report.result;
    if (! r.ok)
    {
        if (loudest < 1.0e-6f)
            report.error = "The input recorded silence: check that \"" + context.inputDevice
                           + "\" is the microphone (or the line input your cable goes to) and that it is not muted, then measure again.";
        else
            report.error = fmt ("The sweep was not found reliably in the recording (%d of %d runs clear the noise by %.0f dB). Hold the "
                                "microphone against the earcup (or check the cable and the input device), turn the volume up a little and "
                                "measure again in a quiet room.",
                                r.acceptedRuns, static_cast<int> (r.runs.size()), settings.minSnrDb);
        return report;
    }

    report.ok = true;
    report.roundTripSamples = r.delaySamples;
    report.roundTripMs = r.delayMs;
    report.spreadMs = toMs (r.spreadSamples, rate);
    report.reportedOutputMs = toMs (context.reportedOutputSamples, rate);
    report.reportedInputMs = toMs (context.reportedInputSamples, rate);
    report.engineMs = report.path == Path::ThroughFlubsound ? toMs (context.stripLatencySamples, rate) : 0.0;
    report.unexplainedMs = report.roundTripMs - (report.reportedOutputMs + report.reportedInputMs + report.engineMs);
    const double base = report.reportedOutputMs + report.engineMs;
    report.playbackMs = base + 0.5 * report.unexplainedMs;
    report.playbackMinMs = base + std::min (0.0, report.unexplainedMs);
    report.playbackMaxMs = base + std::max (0.0, report.unexplainedMs);
    report.withEngineMs = report.path == Path::DeviceOnly ? report.playbackMs + toMs (context.engineSamples, rate) : report.playbackMs;

    // The grade.
    bool low = false, medium = false;
    if (report.spreadMs > kLowMinSpreadMs)
    {
        low = true;
        report.warnings.push_back (fmt ("The runs disagree by %.2f ms: the input and output clocks drift apart (separate devices), or "
                                        "the device changed its buffering. Measure again; with a cable, use one device's own output and input.",
                                        report.spreadMs));
    }
    if (r.secondaryDb > kAmbiguousSecondaryDb)
    {
        low = true;
        double lagMs = 0.0;
        for (const auto& run : r.runs)
            if (run.accepted && run.secondaryDb >= r.secondaryDb - 1.0e-9)
                lagMs = toMs (run.secondaryLagSamples, rate);
        report.warnings.push_back (fmt ("A second arrival only %.1f dB below the first (at %.1f ms) makes the result ambiguous: an echo, "
                                        "or the headset's microphone monitoring (sidetone). Turn mic monitoring off and measure again.",
                                        -r.secondaryDb, lagMs));
    }
    if (report.glitches > 0)
    {
        low = true;
        report.warnings.push_back (fmt ("%lld audio glitch%s during the measurement: the result may be off by a buffer. Close other "
                                        "programs and measure again.",
                                        static_cast<long long> (report.glitches), report.glitches == 1 ? "" : "es"));
    }
    if (r.acceptedRuns < static_cast<int> (r.runs.size()))
    {
        medium = true;
        report.warnings.push_back (fmt ("%d of %d runs were too noisy and left out.", static_cast<int> (r.runs.size()) - r.acceptedRuns,
                                        static_cast<int> (r.runs.size())));
    }
    if (report.spreadMs > kHighMaxSpreadMs && report.spreadMs <= kLowMinSpreadMs)
    {
        medium = true;
        report.warnings.push_back (fmt ("The runs differ by %.2f ms.", report.spreadMs));
    }
    if (r.medianSnrDb < kHighMinSnrDb)
    {
        medium = true;
        report.warnings.push_back (fmt ("The signal was weak (median SNR %.0f dB): hold the microphone closer or turn the volume up a little.",
                                        r.medianSnrDb));
    }
    report.confidence = low ? Confidence::Low : (medium ? Confidence::Medium : Confidence::High);

    // Notes that do not change the grade.
    if (report.capGainDb < -0.05)
        report.warnings.push_back (fmt ("Note: the strip's processing raised the sweep; the safety cap held the output at %.0f dBFS "
                                        "(%.1f dB down). The delay is not affected.",
                                        kThroughCapDbfs, report.capGainDb));
    if (report.unexplainedMs < -0.5)
        report.warnings.push_back (fmt ("Note: the device reports %.1f ms more than the whole round trip measured, so its reported "
                                        "figures are an upper bound.",
                                        -report.unexplainedMs));
    return report;
}

// =============================================================================
std::string describe (const PassReport& report)
{
    const auto& c = report.context;
    const bool through = report.path == Path::ThroughFlubsound;
    std::string s = through ? (c.stripName.empty() ? std::string ("Through Flubsound") : "Through Flubsound (" + c.stripName + " strip)")
                            : std::string ("Device only");
    s += ": " + (c.outputDevice.empty() ? std::string ("?") : c.outputDevice) + " -> "
         + (c.inputDevice.empty() ? std::string ("?") : c.inputDevice);
    if (! c.deviceType.empty())
        s += ", " + c.deviceType;
    s += fmt (", %.1f kHz, %d-sample buffer\n", c.sampleRate / 1000.0, c.bufferSize);

    if (! report.ok)
        return s + "No result: " + report.error;

    const auto& r = report.result;
    s += fmt ("Round trip: %.1f ms (%.1f samples), median of %d / %d runs, spread %.2f ms, SNR %.0f dB, confidence %s\n",
              report.roundTripMs, report.roundTripSamples, r.acceptedRuns, static_cast<int> (r.runs.size()), report.spreadMs, r.medianSnrDb,
              confidenceName (report.confidence));
    if (through)
        s += fmt ("Split: device out %.1f + engine %.1f + device in %.1f ms reported, ", report.reportedOutputMs, report.engineMs,
                  report.reportedInputMs);
    else
        s += fmt ("Split: device out %.1f + device in %.1f ms reported, ", report.reportedOutputMs, report.reportedInputMs);
    s += report.unexplainedMs >= 0.0 ? fmt ("%.1f ms not reported (converters, USB / wireless link, air or cable)\n", report.unexplainedMs)
                                     : fmt ("%.1f ms less than reported\n", -report.unexplainedMs);
    s += fmt ("Playback latency (what you hear): about %.1f ms (%.1f .. %.1f ms)", report.playbackMs, report.playbackMinMs,
              report.playbackMaxMs);
    if (! through)
        s += fmt (" without Flubsound; through it add the engine's %.1f ms: about %.1f ms", report.withEngineMs - report.playbackMs,
                  report.withEngineMs);
    s += ". The part the device does not report is assumed to split evenly between playback and recording.";
    if (c.captureBufferMs > 0.05)
        s += fmt (" Apps captured per process add their capture buffer, %.1f ms (reported, not measured).", c.captureBufferMs);
    for (const auto& w : report.warnings)
        s += "\n" + w;
    return s;
}

std::string describeBoth (const PassReport& deviceOnly, const PassReport& through)
{
    std::string s = describe (deviceOnly) + "\n\n" + describe (through);
    if (deviceOnly.ok && through.ok)
    {
        const double rate = through.context.sampleRate;
        const double measured = through.roundTripMs - deviceOnly.roundTripMs;
        s += fmt ("\n\nFlubsound's engine, measured (through - device only): %.2f ms (%.1f samples); it reports %.2f ms (%d samples).",
                  measured, through.roundTripSamples - deviceOnly.roundTripSamples, toMs (through.context.stripLatencySamples, rate),
                  through.context.stripLatencySamples);
    }
    return s;
}

std::string summarise (const PassReport& report)
{
    const auto& c = report.context;
    std::string s = "Latency measured (";
    s += report.path == Path::ThroughFlubsound ? "through Flubsound" + (c.stripName.empty() ? std::string() : ", " + c.stripName) : "device only";
    s += "): ";
    if (! report.ok)
        return s + "no result: " + report.error;
    s += fmt ("round trip %.1f ms, playback about %.1f ms", report.roundTripMs, report.playbackMs);
    if (report.path == Path::DeviceOnly)
        s += fmt (" (%.1f ms with the engine)", report.withEngineMs);
    s += fmt (", confidence %s; ", confidenceName (report.confidence));
    s += c.outputDevice + " -> " + c.inputDevice + fmt (", %d samples at %.0f Hz", c.bufferSize, c.sampleRate);
    return s;
}
} // namespace flub::app::latency
