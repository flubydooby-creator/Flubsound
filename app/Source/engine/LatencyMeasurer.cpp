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
    plan.clear();
    if (r.mode != Mode::ThroughFlubsound)
        plan.push_back (latency::Path::DeviceOnly);
    if (r.mode != Mode::DeviceOnly)
        plan.push_back (latency::Path::ThroughFlubsound);
    planIndex = 0;
    passes.clear();
    playedBefore = 0;
    totalSamples = flub::latency::probeLength (settingsFor (request, host.getSampleRate())) * static_cast<int64_t> (plan.size());

    const auto generation = state.generation;
    state = {};
    state.generation = generation + 1;
    state.mode = r.mode;
    state.passes = static_cast<int> (plan.size());
    state.phase = Phase::Running;
    if (! startPass (plan.front()))
    {
        state.phase = Phase::Idle;
        return "The audio device did not take the measurement (it may have just restarted). Try again.";
    }
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

bool LatencyMeasurer::startPass (latency::Path path)
{
    const int channels = std::min (host.getDeviceInputChannels(), latency::ProbeSession::kMaxRecordChannels);
    auto session = std::make_unique<latency::ProbeSession> (settingsFor (request, host.getSampleRate()), path, request.strip, channels);
    context = snapshot();
    if (! host.startLatencyProbe (session))
        return false;
    state.pass = static_cast<int> (planIndex) + 1;
    ++state.generation;
    return true;
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

    const auto* session = host.getLatencyProbe();
    if (session == nullptr)
    {
        fail ("The measurement was lost (the audio device restarted). Measure again.");
        return true;
    }

    // Progress over every pass.
    const double rate = std::max (1.0, host.getSampleRate());
    const int64_t played = playedBefore + session->position();
    const double progress = totalSamples > 0 ? std::clamp (static_cast<double> (played) / static_cast<double> (totalSamples), 0.0, 1.0) : 0.0;
    const double left = static_cast<double> (std::max<int64_t> (0, totalSamples - played)) / rate;
    bool changed = std::abs (progress - state.progress) > 1.0e-9;
    state.progress = progress;
    state.secondsLeft = left;

    const bool finished = session->finished();
    // The device stopped, closed or restarted under it (a device, rate or
    // buffer change, a re-open): the session no longer advances.
    const bool stopped = ! host.getStatus().running || ! host.isLatencyProbeLive();
    if (! finished && ! stopped)
    {
        if (changed)
            ++state.generation;
        return changed;
    }

    auto taken = host.takeLatencyProbe();
    if (taken == nullptr)
        return changed; // the audio thread has not let go yet: next poll
    if (cancelling || taken->wasCancelled())
    {
        fail ("Cancelled.");
        return true;
    }
    if (! taken->finished())
    {
        fail ("The audio device stopped during the measurement (a device, rate or buffer change). Measure again.");
        return true;
    }

    const auto status = host.getStatus();
    context.glitchesAfter = status.deviceOpen ? static_cast<int64_t> (status.glitches) : -1;
    playedBefore += taken->length();
    passes.emplace_back (std::move (taken), context);
    ++planIndex;
    ++state.generation;

    if (planIndex < plan.size())
    {
        if (! startPass (plan[planIndex]))
            fail ("The audio device did not take the second pass (it may have restarted). Measure again.");
        return true;
    }

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
