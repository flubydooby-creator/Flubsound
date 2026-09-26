// Flubsound Pro - the single consumer of a chain's analyser taps.
//
// flub::AnalyzerTaps are single-producer / single-consumer rings (pre and
// post processing, mono mid samples). Both the spectrum analyser and the
// waveform history need the post stream, so exactly one AnalyzerFeed drains
// the rings on the message thread once per display frame and fans the blocks
// out to its sinks. After a long stall (window hidden, debugger) the backlog
// is trimmed so the views jump to "now" instead of replaying stale audio.
#pragma once

#include "flub/engine/MeterBus.h"

#include <functional>
#include <vector>

namespace flub::app::ui
{
class AnalyzerFeed
{
public:
    enum class Stream
    {
        Pre,
        Post
    };

    using Sink = std::function<void (Stream stream, const float* samples, int numSamples)>;

    AnalyzerFeed();

    void addSink (Sink sink) { sinks.push_back (std::move (sink)); }

    /** Drains both rings (message thread; the only consumer). */
    void pull (flub::AnalyzerTaps& taps);

    /** Discards whatever is queued (strip switch / engine rebuilt). */
    static void discard (flub::AnalyzerTaps& taps);

private:
    void drain (flub::SpscRing<float>& ring, Stream stream);

    std::vector<Sink> sinks;
    std::vector<float> scratch;
};
} // namespace flub::app::ui
