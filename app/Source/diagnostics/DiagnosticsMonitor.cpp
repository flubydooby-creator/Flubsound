#include "DiagnosticsMonitor.h"

#include "engine/EngineController.h"

namespace flub::app::diagnostics
{
namespace
{
/** Growth of a counter that may restart from 0 (a new device or engine). */
uint64_t grown (uint64_t before, uint64_t now) noexcept
{
    return now >= before ? now - before : now;
}

juce::String plural (uint64_t n, const char* one, const char* many)
{
    return juce::String (static_cast<juce::int64> (n)) + " " + (n == 1 ? one : many);
}

bool sameDevice (const EngineSnapshot& a, const EngineSnapshot& b)
{
    return a.deviceOpen == b.deviceOpen && a.deviceType == b.deviceType && a.outputName == b.outputName && a.inputName == b.inputName
           && a.sampleRate == b.sampleRate && a.bufferSize == b.bufferSize;
}
} // namespace

EngineSnapshot takeSnapshot (EngineController& controller)
{
    EngineSnapshot s;
    const auto status = controller.getStatus();
    s.deviceOpen = status.deviceOpen;
    s.deviceType = status.deviceTypeName;
    s.glitches = status.glitches;
    if (auto* device = controller.getDeviceManager().getCurrentAudioDevice(); device != nullptr && status.deviceOpen)
    {
        const auto setup = controller.getDeviceManager().getAudioDeviceSetup();
        s.outputName = setup.outputDeviceName.isNotEmpty() ? setup.outputDeviceName : status.deviceName;
        s.inputName = setup.inputDeviceName;
        s.sampleRate = device->getCurrentSampleRate();
        s.bufferSize = device->getCurrentBufferSizeSamples();
    }
    s.deviceError = controller.getLastDeviceError();
    const auto& overload = controller.getOverloadState();
    s.overloaded = overload.overloaded;
    s.overloadEpisodes = overload.episodes;
    for (int i = 0; i < controller.getNumStrips(); ++i)
    {
        const auto& meters = controller.getChain (i).meters();
        EngineSnapshot::Strip strip;
        strip.name = controller.getStripName (i);
        strip.safetyClips = meters.safetyClipCount.load (std::memory_order_relaxed);
        strip.corruptSamples = meters.corruptSampleCount.load (std::memory_order_relaxed);
        strip.droppedBlocks = meters.droppedBlockCount.load (std::memory_order_relaxed);
        s.strips.push_back (strip);
    }
    return s;
}

juce::String describeDevice (const EngineSnapshot& s)
{
    if (! s.deviceOpen)
        return "no audio device open";
    juce::String text;
    text << (s.deviceType.isNotEmpty() ? s.deviceType + " / " : juce::String()) << (s.outputName.isNotEmpty() ? s.outputName : "(no output)");
    if (s.inputName.isNotEmpty())
        text << ", in: " << s.inputName;
    if (s.sampleRate > 0.0)
        text << ", " << juce::roundToInt (s.sampleRate) << " Hz";
    if (s.bufferSize > 0)
        text << ", " << s.bufferSize << " samples";
    return text;
}

//==============================================================================
bool EventLogBuilder::Pending::any() const noexcept
{
    if (glitches > 0)
        return true;
    for (const auto& s : strips)
        if (s.safetyClips > 0 || s.corruptSamples > 0 || s.droppedBlocks > 0)
            return true;
    return false;
}

juce::String EventLogBuilder::counterLine (juce::uint32 nowMs) const
{
    // The window this line covers: since the previous counter line, or since
    // the first count when the previous line is long ago.
    const auto since = counterLineWritten && nowMs - lastCounterLineMs <= 2 * interval ? lastCounterLineMs : pendingSinceMs;
    const auto seconds = juce::jmax (1, juce::roundToInt ((nowMs - since) / 1000.0));
    juce::StringArray parts;
    if (pending.glitches > 0)
        parts.add (plural (pending.glitches, "glitch (xrun or late callback)", "glitches (xruns or late callbacks)"));
    for (const auto& s : pending.strips)
    {
        juce::StringArray strip;
        if (s.safetyClips > 0)
            strip.add (plural (s.safetyClips, "safety clip", "safety clips"));
        if (s.droppedBlocks > 0)
            strip.add (plural (s.droppedBlocks, "block dropped (NaN / Inf)", "blocks dropped (NaN / Inf)"));
        if (s.corruptSamples > 0)
            strip.add (plural (s.corruptSamples, "sample beyond +24 dBFS muted", "samples beyond +24 dBFS muted"));
        if (! strip.isEmpty())
            parts.add (s.name + ": " + strip.joinIntoString (", "));
    }
    return "Last " + juce::String (seconds) + " s: " + parts.joinIntoString ("; ");
}

juce::StringArray EventLogBuilder::update (const EngineSnapshot& now, juce::uint32 nowMs)
{
    juce::StringArray lines;
    if (! started)
    {
        started = true;
        previous = now;
        lastCounterLineMs = pendingSinceMs = nowMs;
        pending.strips.assign (now.strips.size(), {});
        for (size_t i = 0; i < now.strips.size(); ++i)
            pending.strips[i].name = now.strips[i].name;
        lines.add ("Audio device: " + describeDevice (now));
        if (now.deviceError.isNotEmpty())
            lines.add ("Audio device error: " + now.deviceError);
        return lines;
    }

    // Counters: sum what grew since the previous poll. A changed strip list
    // (new engine) writes what is pending and starts a new baseline.
    if (! pending.any())
        pendingSinceMs = nowMs;
    pending.glitches += grown (static_cast<uint64_t> (juce::jmax (0, previous.glitches)), static_cast<uint64_t> (juce::jmax (0, now.glitches)));
    if (now.strips.size() == previous.strips.size())
    {
        for (size_t i = 0; i < now.strips.size(); ++i)
        {
            auto& p = pending.strips[i];
            p.safetyClips += grown (previous.strips[i].safetyClips, now.strips[i].safetyClips);
            p.corruptSamples += grown (previous.strips[i].corruptSamples, now.strips[i].corruptSamples);
            p.droppedBlocks += grown (previous.strips[i].droppedBlocks, now.strips[i].droppedBlocks);
        }
    }
    else
    {
        lines.addArray (flush (nowMs));
        pending.strips.assign (now.strips.size(), {});
        for (size_t i = 0; i < now.strips.size(); ++i)
            pending.strips[i].name = now.strips[i].name;
    }

    // Device events at once, after the counts of the device they happened on.
    if (! sameDevice (previous, now))
    {
        lines.addArray (flush (nowMs));
        if (! now.deviceOpen)
            lines.add ("Audio device closed");
        else
            lines.add (juce::String (previous.deviceOpen ? "Audio device changed: " : "Audio device opened: ") + describeDevice (now));
    }
    if (now.deviceError != previous.deviceError)
        lines.add (now.deviceError.isNotEmpty() ? "Audio device error: " + now.deviceError : juce::String ("Audio device error cleared"));
    if (now.overloaded != previous.overloaded)
        lines.add (now.overloaded ? "CPU overload started (episode " + juce::String (static_cast<juce::int64> (now.overloadEpisodes)) + " this session)"
                                  : juce::String ("CPU overload ended"));

    // The first counts at once, later ones at most once per interval.
    if (pending.any() && (! counterLineWritten || nowMs - lastCounterLineMs >= interval))
        lines.addArray (flush (nowMs));

    previous = now;
    return lines;
}

juce::StringArray EventLogBuilder::flush (juce::uint32 nowMs)
{
    if (! pending.any())
        return {};
    juce::StringArray lines { counterLine (nowMs) };
    pending.glitches = 0;
    for (auto& s : pending.strips)
        s.safetyClips = s.corruptSamples = s.droppedBlocks = 0;
    lastCounterLineMs = nowMs;
    counterLineWritten = true;
    return lines;
}

//==============================================================================
DiagnosticsMonitor::DiagnosticsMonitor (EngineController& c, std::function<void (const juce::String&)> w)
    : controller (c), write (std::move (w))
{
    pollNow();
    startTimerHz (2);
}

DiagnosticsMonitor::~DiagnosticsMonitor()
{
    stopTimer();
    for (const auto& line : builder.flush (juce::Time::getMillisecondCounter()))
        write (line);
}

void DiagnosticsMonitor::pollNow()
{
    for (const auto& line : builder.update (takeSnapshot (controller), juce::Time::getMillisecondCounter()))
        write (line);
}
} // namespace flub::app::diagnostics
