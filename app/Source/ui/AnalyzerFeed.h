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

    /** The post tap's side signal (L - R) / 2 (the analyser's stereo-width
        view). Kept apart from the mid sinks above, which see exactly the
        pre / post mid streams they always did. Without a side sink the side
        ring is drained and dropped. */
    using SideSink = std::function<void (const float* samples, int numSamples)>;
    void setSideSink (SideSink sink) { sideSink = std::move (sink); }

    /** Drains the rings (message thread; the only consumer). */
    void pull (flub::AnalyzerTaps& taps);

    /** Discards whatever is queued (strip switch / engine rebuilt). */
    static void discard (flub::AnalyzerTaps& taps);

private:
    void drain (flub::SpscRing<float>& ring, Stream stream);
    void drainSide (flub::SpscRing<float>& ring);

    std::vector<Sink> sinks;
    SideSink sideSink;
    std::vector<float> scratch;
};
} // namespace flub::app::ui
