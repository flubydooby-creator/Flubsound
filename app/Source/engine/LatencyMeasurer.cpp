#include "LatencyMeasurer.h"

#include <algorithm>
#include <cmath>

namespace flub::app
{
LatencyMeasurer::LatencyMeasurer (AudioEngineHost& h)
    : host (h)
{
}

LatencyMeasurer::~LatencyMeasurer()
{
    if (state.phase == Phase::Running)
        host.cancelLatencyProbe(); // the host keeps (and later frees) the session
    if (worker.joinable())
        worker.join();
}

flub::latency::ProbeSettings LatencyMeasurer::settingsFor (const Request& r, double sampleRate)
{
    auto s = r.settings.has_value() ? *r.settings : latency::liveProbeSettings (sampleRate);
    s.sampleRate = sampleRate;
    return s;
}

double LatencyMeasurer::durationSeconds (Mode mode, double sampleRate, const std::optional<flub::latency::ProbeSettings>& settings)
{
    auto s = settings.has_value() ? *settings : latency::liveProbeSettings (sampleRate);
    s.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    const double one = static_cast<double> (flub::latency::probeLength (s)) / s.sampleRate;
    return mode == Mode::Both ? 2.0 * one : one;
}

juce::String LatencyMeasurer::whyNot (const Request& r) const
{
    if (isBusy())
        return "A measurement is already running.";
    // docs/11 E53: a device soak plays its own programme into the strips on
    // a pinned output; the probe would be heard in its analysis (and the
    // Through pass would get no input).
    if (host.getOutputPin().isNotEmpty() || host.hasDeviceSignalSource())
        return "A device soak is running on this output: measure latency in a normal start of the app.";
    if (! host.getStatus().running)
        return "No audio device is running. Choose an output and an input above first.";
    if (host.isOutputMutedByGuard())
        return "The output is muted by the feedback-loop guard: choose an output that is not the input's loopback partner.";
    if (host.getDeviceInputChannels() <= 0)
        return "No input is open. Choose the microphone (or the line input your cable goes to) as the input device above.";
    if (r.strip < 0 || r.strip >= host.getNumStrips())
        return "There is no such strip.";
    if (r.mode != Mode::DeviceOnly && host.isStripMuted (r.strip))
        return "The " + r.stripName + " strip is muted: unmute it to measure through Flubsound.";
    if (const auto problem = flub::latency::validate (settingsFor (r, host.getSampleRate())); ! problem.empty())
        return "The probe does not fit this device: " + juce::String (problem);
    return {};
}

juce::String LatencyMeasurer::start (const Request& r)
{
    if (const auto reason = whyNot (r); reason.isNotEmpty())
        return reason;
    if (worker.joinable())
        worker.join();

    request = r;
    cancelling = false;
    failure.clear();
    plan.clear();
    if (r.mode != Mode::ThroughFlubsound)
        plan.push_back (latency::Path::DeviceOnly);
    if (r.mode != Mode::DeviceOnly)
        plan.push_back (latency::Path::ThroughFlubsound);
    passes.clear();
    playedBefore = 0;
    const auto probeSettings = settingsFor (request, host.getSampleRate());
    totalSamples = flub::latency::probeLength (probeSettings) * static_cast<int64_t> (plan.size());

    // Every pass is handed over at once, chained (Both: device only, then
    // through Flubsound from the very next callback), so the programme never
    // plays between them and no poll can delay the second.
    context = snapshot();
    glitchesAtLastPoll = context.glitchesBefore;
    const int channels = std::min (host.getDeviceInputChannels(), latency::ProbeSession::kMaxRecordChannels);
    std::vector<std::unique_ptr<latency::ProbeSession>> sessions;
    for (const auto path : plan)
    {
        sessions.push_back (std::make_unique<latency::ProbeSession> (probeSettings, path, request.strip, channels));
        if (path == latency::Path::ThroughFlubsound)
            sessions.back()->setOutputLatency (context.stripLatencySamples);
    }
    if (sessions.size() == 2)
        sessions.front()->chainTo (*sessions.back());

    const auto generation = state.generation;
    state = {};
    state.generation = generation + 1;
    state.mode = r.mode;
    state.passes = static_cast<int> (plan.size());
    if (! host.startLatencyProbe (sessions))
        return "The audio device did not take the measurement (it may have just restarted). Try again.";
    state.phase = Phase::Running;
    state.pass = 1;
    return {};
}

latency::DeviceContext LatencyMeasurer::snapshot() const
{
    latency::DeviceContext c;
    const auto status = host.getStatus();
    auto& manager = host.getDeviceManager();
    const auto setup = manager.getAudioDeviceSetup();
    c.outputDevice = (setup.outputDeviceName.isNotEmpty() ? setup.outputDeviceName : status.deviceName).toStdString();
    c.inputDevice = setup.inputDeviceName.toStdString();
    c.deviceType = status.deviceTypeName.toStdString();
    c.stripName = request.stripName.toStdString();
    c.sampleRate = host.getSampleRate();
    c.bufferSize = host.getBlockSize();
    c.reportedOutputSamples = host.getReportedOutputLatency();
    c.reportedInputSamples = host.getReportedInputLatency();
    auto& engine = host.getMixEngine();
    if (request.strip >= 0 && request.strip < engine.getNumStrips())
        c.stripLatencySamples = engine.getStripLatencySamples (request.strip);
    c.engineSamples = c.stripLatencySamples;
    c.captureBufferMs = host.getLatencyInfo().captureBufferMs;
    c.glitchesBefore = status.deviceOpen ? static_cast<int64_t> (status.glitches) : -1;
    return c;
}

void LatencyMeasurer::cancel()
{
    if (state.phase != Phase::Running)
        return;
    cancelling = true; // also when the pass ends before the probe sees it
    host.cancelLatencyProbe();
}

void LatencyMeasurer::fail (const juce::String& error)
{
    state.phase = Phase::Failed;
    state.error = error;
    ++state.generation;
    passes.clear();
    if (onFinished != nullptr)
        onFinished (state);
}

bool LatencyMeasurer::poll()
{
    if (state.phase == Phase::Analysing)
    {
        if (! workerDone.load (std::memory_order_acquire))
            return false;
        finishAnalysis();
        return true;
    }
    if (state.phase != Phase::Running)
        return false;

    bool changed = false;
    const auto now = host.getStatus();
    const int64_t glitchesNow = now.deviceOpen ? static_cast<int64_t> (now.glitches) : -1;
    for (;;)
    {
        const auto* session = host.getLatencyProbe();
        if (session == nullptr)
            break; // every pass handed back

        // Progress over every pass.
        if (failure.isEmpty())
        {
            const double rate = std::max (1.0, host.getSampleRate());
            const int64_t played = playedBefore + session->position();
            const double progress = totalSamples > 0 ? std::clamp (static_cast<double> (played) / static_cast<double> (totalSamples), 0.0, 1.0) : 0.0;
            changed = changed || std::abs (progress - state.progress) > 1.0e-9;
            state.progress = progress;
            state.secondsLeft = static_cast<double> (std::max<int64_t> (0, totalSamples - played)) / rate;
        }

        // The device stopped, closed or restarted under it (a device, rate or
        // buffer change, a re-open): the session no longer advances.
        const bool stopped = ! host.getStatus().running || ! host.isLatencyProbeLive();
        if (! session->finished() && ! stopped)
            break;
        auto taken = host.takeLatencyProbe();
        if (taken == nullptr)
            break; // the audio thread has not let go yet: next poll
        changed = true;

        // After a cancel or a stop the rest is only handed back (a pass that
        // had not started ends at its first callback).
        if (failure.isNotEmpty())
            continue;
        if (cancelling || taken->wasCancelled())
        {
            failure = "Cancelled.";
            host.cancelLatencyProbe();
            continue;
        }
        if (! taken->finished())
        {
            failure = "The audio device stopped during the measurement (a device, rate or buffer change). Measure again.";
            continue;
        }

        // A pass recorded; the next one (chained) is already playing. It ended
        // between the last poll and this one, so a glitch in between counts
        // for both passes (a warning too many rather than one missed).
        auto passContext = context;
        passContext.glitchesAfter = glitchesNow;
        context.glitchesBefore = glitchesAtLastPoll;
        playedBefore += taken->length();
        passes.emplace_back (std::move (taken), passContext);
        state.pass = std::min (static_cast<int> (passes.size()) + 1, state.passes);
    }

    if (host.getLatencyProbe() != nullptr)
    {
        glitchesAtLastPoll = glitchesNow;
        if (changed)
            ++state.generation;
        return changed;
    }
    if (failure.isNotEmpty())
    {
        fail (failure);
        return true;
    }
    if (passes.size() < plan.size())
    {
        fail ("The measurement was lost (the audio device restarted). Measure again.");
        return true;
    }
    ++state.generation;

    // Every pass recorded: analyse them off the message thread.
    state.phase = Phase::Analysing;
    state.progress = 1.0;
    state.secondsLeft = 0.0;
    workerDone.store (false, std::memory_order_relaxed);
    workerReports.clear();
    auto jobs = std::make_shared<std::vector<std::pair<std::unique_ptr<latency::ProbeSession>, latency::DeviceContext>>> (std::move (passes));
    passes.clear();
    worker = std::thread ([this, jobs]
    {
        std::vector<latency::PassReport> reports;
        for (const auto& job : *jobs)
            reports.push_back (latency::analyse (*job.first, job.second));
        workerReports = std::move (reports);
        workerDone.store (true, std::memory_order_release);
    });
    return true;
}

void LatencyMeasurer::finishAnalysis()
{
    if (worker.joinable())
        worker.join();
    for (auto& report : workerReports)
    {
        if (report.path == latency::Path::DeviceOnly)
            state.deviceOnly = std::move (report);
        else
            state.through = std::move (report);
    }
    workerReports.clear();
    state.phase = Phase::Done;
    ++state.generation;
    if (onFinished != nullptr)
        onFinished (state);
}

juce::String LatencyMeasurer::State::describe() const
{
    if (phase != Phase::Done)
        return {};
    if (deviceOnly.has_value() && through.has_value())
        return juce::String::fromUTF8 (latency::describeBoth (*deviceOnly, *through).c_str());
    if (through.has_value())
        return juce::String::fromUTF8 (latency::describe (*through).c_str());
    if (deviceOnly.has_value())
        return juce::String::fromUTF8 (latency::describe (*deviceOnly).c_str());
    return {};
}
} // namespace flub::app
