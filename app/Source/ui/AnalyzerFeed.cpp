#include "AnalyzerFeed.h"

namespace flub::app::ui
{
namespace
{
constexpr size_t kChunk = 2048;
constexpr size_t kMaxBacklog = 16384; // ~0.34 s at 48 kHz: older samples are dropped
} // namespace

AnalyzerFeed::AnalyzerFeed()
{
    scratch.assign (kChunk, 0.0f);
    midScratch.assign (kChunk, 0.0f);
    sideScratch.assign (kChunk, 0.0f);
    stereoScratch.assign (kChunk, flub::StereoTapFrame {});
}

void AnalyzerFeed::pull (flub::AnalyzerTaps& taps)
{
    drain (taps.pre, Stream::Pre);
    drain (taps.post, Stream::Post);
    drainStereo (taps.postStereo);
}

void AnalyzerFeed::discard (flub::AnalyzerTaps& taps)
{
    taps.pre.skip (taps.pre.available());
    taps.post.skip (taps.post.available());
    taps.postStereo.skip (taps.postStereo.available());
}

void AnalyzerFeed::drainStereo (flub::SpscRing<flub::StereoTapFrame>& ring)
{
    const size_t available = ring.available();
    if (sideSink == nullptr && stereoSink == nullptr)
    {
        ring.skip (available);
        return;
    }
    if (available > kMaxBacklog)
        ring.skip (available - kMaxBacklog / 2);

    for (int guard = 0; guard < 32; ++guard)
    {
        const size_t n = ring.pop (stereoScratch.data(), stereoScratch.size());
        if (n == 0)
            break;
        for (size_t i = 0; i < n; ++i)
        {
            midScratch[i] = stereoScratch[i].mid;
            sideScratch[i] = stereoScratch[i].side;
        }
        if (sideSink != nullptr)
            sideSink (sideScratch.data(), static_cast<int> (n));
        if (stereoSink != nullptr)
            stereoSink (midScratch.data(), sideScratch.data(), static_cast<int> (n));
        if (n < stereoScratch.size())
            break;
    }
}

void AnalyzerFeed::drain (flub::SpscRing<float>& ring, Stream stream)
{
    const size_t available = ring.available();
    if (available > kMaxBacklog)
        ring.skip (available - kMaxBacklog / 2);

    // Bounded: never more than the ring capacity per frame.
    for (int guard = 0; guard < 32; ++guard)
    {
        const size_t n = ring.pop (scratch.data(), scratch.size());
        if (n == 0)
            break;
        for (auto& sink : sinks)
            sink (stream, scratch.data(), static_cast<int> (n));
        if (n < scratch.size())
            break;
    }
}
} // namespace flub::app::ui
