#include "DriftCompensatedFifo.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace flub::app
{
namespace
{
constexpr uint64_t kPhaseOne = 1ull << 32; // 32.32 fixed point
constexpr float kInvPhaseOne = 1.0f / 4294967296.0f;
constexpr float kMinus3dB = 0.70710678f;
} // namespace

// =============================================================================
// Setup (non-RT)
// =============================================================================
void DriftCompensatedFifo::prepare (int numChannels, double producerSampleRate, double consumerSampleRate,
                                    int consumerMaxBlockSize, double bufferSeconds)
{
    channels = std::clamp (numChannels, 1, kMaxChannels);
    producerRate = producerSampleRate > 0.0 ? producerSampleRate : 48000.0;

    const double seconds = std::max (0.1, bufferSeconds);
    const auto capacityFrames = static_cast<size_t> (std::max (8192.0, std::ceil (producerRate * seconds)));
    ring.allocate (capacityFrames * static_cast<size_t> (channels));
    producerScratch.assign (static_cast<size_t> (kChunkFrames) * static_cast<size_t> (channels), 0.0f);

    burstEstimate.store (0.0f, std::memory_order_relaxed);
    framesPushed.store (0, std::memory_order_relaxed);
    ringFullDrops.store (0, std::memory_order_relaxed);
    underruns.store (0, std::memory_order_relaxed);
    overflows.store (0, std::memory_order_relaxed);
    overflowDrops.store (0, std::memory_order_relaxed);

    setConsumerFormat (consumerSampleRate, consumerMaxBlockSize);
}

void DriftCompensatedFifo::setConsumerFormat (double consumerSampleRate, int consumerMaxBlockSize)
{
    consumerRate = consumerSampleRate > 0.0 ? consumerSampleRate : 48000.0;
    consumerBlock = std::max (1, consumerMaxBlockSize);
    nominalRatio = producerRate / consumerRate;
    fadeLength = std::max (16, static_cast<int> (std::lround (0.005 * consumerRate)));

    // Worst case frames consumed by one block: ceil(block * maxRatio) + 1
    // (window shifts), plus a little slack.
    const double maxRatio = nominalRatio * (1.0 + kMaxCorrection);
    const auto maxFrames = static_cast<size_t> (std::ceil (consumerBlock * maxRatio)) + 4;
    staging.assign (maxFrames * static_cast<size_t> (std::max (1, channels)), 0.0f);

    // Data buffered for the previous format is stale (and the target changes):
    // start over. Skipping is a consumer-side operation and the consumer is
    // stopped, so this is safe while the producer keeps pushing.
    if (channels > 0)
        ring.skip (ring.available());

    phase = 0;
    for (auto& h : history)
        h.fill (0.0f);
    lastOut.fill (0.0f);
    smoothedFill = 0.0;
    integral = 0.0;
    fadeInRemaining = 0;
    streamingState = false;
    streamingStat.store (false, std::memory_order_relaxed);
    correctionPpmStat.store (0.0f, std::memory_order_relaxed);
    targetMsStat.store (static_cast<float> (1000.0 * computeTargetFrames() / producerRate), std::memory_order_relaxed);
}

// =============================================================================
// Producer
// =============================================================================
int DriftCompensatedFifo::convertChunk (const float* src, int numFrames, int srcChannels) noexcept
{
    const auto ch = static_cast<size_t> (channels);
    const auto sch = static_cast<size_t> (srcChannels);
    float* out = producerScratch.data();

    for (int i = 0; i < numFrames; ++i)
    {
        const float* s = src + static_cast<size_t> (i) * sch;
        float* d = out + static_cast<size_t> (i) * ch;

        if (srcChannels == 1)
        {
            for (size_t c = 0; c < ch; ++c)
                d[c] = s[0];
        }
        else if (channels == 2 && srcChannels >= 3)
        {
            // ITU-R BS.775 downmix, LFE dropped, -3 dB overall (same policy as
            // the chain). Order: FL FR FC LFE [BL BR] [SL SR].
            float l = s[0] + kMinus3dB * s[2];
            float r = s[1] + kMinus3dB * s[2];
            for (size_t p = 4; p + 1 < sch; p += 2)
            {
                l += kMinus3dB * s[p];
                r += kMinus3dB * s[p + 1];
            }
            d[0] = kMinus3dB * l;
            d[1] = kMinus3dB * r;
        }
        else
        {
            const size_t n = std::min (ch, sch);
            for (size_t c = 0; c < n; ++c)
                d[c] = s[c];
            for (size_t c = n; c < ch; ++c)
                d[c] = 0.0f;
        }
    }
    return numFrames;
}

int DriftCompensatedFifo::push (const float* interleaved, int numFrames, int numSourceChannels) noexcept
{
    if (channels <= 0 || interleaved == nullptr || numFrames <= 0 || numSourceChannels <= 0)
        return 0;

    // Decaying peak of the producer's packet size: it sets the minimum target
    // fill so that packetised delivery never underruns the consumer.
    const float previousBurst = burstEstimate.load (std::memory_order_relaxed);
    burstEstimate.store (std::max (static_cast<float> (numFrames), previousBurst * 0.995f), std::memory_order_relaxed);
    framesPushed.fetch_add (static_cast<uint64_t> (numFrames), std::memory_order_relaxed);

    const auto ch = static_cast<size_t> (channels);
    int accepted = 0;

    if (numSourceChannels == channels)
    {
        const size_t freeFrames = (ring.capacity() - ring.available()) / ch;
        const size_t n = std::min (static_cast<size_t> (numFrames), freeFrames);
        ring.push (interleaved, n * ch);
        accepted = static_cast<int> (n);
    }
    else
    {
        for (int pos = 0; pos < numFrames; pos += kChunkFrames)
        {
            const int chunk = std::min (kChunkFrames, numFrames - pos);
            convertChunk (interleaved + static_cast<size_t> (pos) * static_cast<size_t> (numSourceChannels), chunk,
                          numSourceChannels);
            const size_t freeFrames = (ring.capacity() - ring.available()) / ch;
            const size_t n = std::min (static_cast<size_t> (chunk), freeFrames);
            ring.push (producerScratch.data(), n * ch);
            accepted += static_cast<int> (n);
            if (n < static_cast<size_t> (chunk))
                break;
        }
    }

    // Ring completely full (consumer stalled): the newest frames are lost.
    if (accepted < numFrames)
        ringFullDrops.fetch_add (static_cast<uint64_t> (numFrames - accepted), std::memory_order_relaxed);

    return accepted;
}

// =============================================================================
// Consumer
// =============================================================================
double DriftCompensatedFifo::computeTargetFrames() const noexcept
{
    const double block = static_cast<double> (consumerBlock) * nominalRatio;
    const double burst = static_cast<double> (burstEstimate.load (std::memory_order_relaxed));
    const double target = std::max (2.0 * block, burst + block) + 4.0; // +4: interpolation window
    const double limit = channels > 0 ? 0.5 * static_cast<double> (ring.capacity() / static_cast<size_t> (channels)) : target;
    return std::min (target, limit);
}

void DriftCompensatedFifo::startStreaming (double fillFrames) noexcept
{
    // The integral (learned clock drift) is deliberately kept across re-primes.
    streamingState = true;
    smoothedFill = fillFrames;
    phase = 0;
    for (auto& h : history)
        h.fill (0.0f);
    fadeInRemaining = fadeLength;
    streamingStat.store (true, std::memory_order_relaxed);
}

void DriftCompensatedFifo::renderFadeToSilence (float* const* dest, int numDestChannels, int numFrames, bool addToDest) noexcept
{
    const float fadeStep = 1.0f / static_cast<float> (fadeLength);
    const float endGain = std::max (0.0f, 1.0f - fadeStep * static_cast<float> (numFrames));

    for (int c = 0; c < numDestChannels; ++c)
    {
        float* d = dest[c];
        if (d == nullptr)
            continue;
        const float v = c < channels ? lastOut[static_cast<size_t> (c)] : 0.0f;

        if (v == 0.0f)
        {
            if (! addToDest)
                std::memset (d, 0, sizeof (float) * static_cast<size_t> (numFrames));
            continue;
        }

        float g = 1.0f;
        for (int i = 0; i < numFrames; ++i)
        {
            g = std::max (0.0f, g - fadeStep);
            if (addToDest)
                d[i] += v * g;
            else
                d[i] = v * g;
        }
    }

    for (int c = 0; c < channels; ++c)
        lastOut[static_cast<size_t> (c)] *= endGain;
}

void DriftCompensatedFifo::restart() noexcept
{
    if (channels > 0)
        ring.skip (ring.available());
    streamingState = false;
    streamingStat.store (false, std::memory_order_relaxed);
    lastOut.fill (0.0f);
}

bool DriftCompensatedFifo::pull (float* const* dest, int numDestChannels, int numFrames, bool addToDest) noexcept
{
    if (numFrames <= 0)
        return true;

    numDestChannels = std::clamp (numDestChannels, 0, kMaxChannels);

    if (channels <= 0)
    {
        if (! addToDest)
            for (int c = 0; c < numDestChannels; ++c)
                if (dest[c] != nullptr)
                    std::memset (dest[c], 0, sizeof (float) * static_cast<size_t> (numFrames));
        return false;
    }

    // Blocks larger than the prepared size are rendered in slices.
    if (numFrames > consumerBlock)
    {
        bool ok = true;
        std::array<float*, kMaxChannels> offset {};
        for (int pos = 0; pos < numFrames; pos += consumerBlock)
        {
            for (int c = 0; c < numDestChannels; ++c)
                offset[static_cast<size_t> (c)] = dest[c] != nullptr ? dest[c] + pos : nullptr;
            ok = pull (offset.data(), numDestChannels, std::min (consumerBlock, numFrames - pos), addToDest) && ok;
        }
        return ok;
    }

    const auto ch = static_cast<size_t> (channels);
    double avail = static_cast<double> (ring.available() / ch);
    const double target = computeTargetFrames();
    targetMsStat.store (static_cast<float> (1000.0 * target / producerRate), std::memory_order_relaxed);

    // ---- 1. Overflow: drop the OLDEST frames back down to the target --------
    {
        const double burst = static_cast<double> (burstEstimate.load (std::memory_order_relaxed));
        const double block = static_cast<double> (consumerBlock) * nominalRatio;
        const double ringFrames = static_cast<double> (ring.capacity() / ch);
        const double highWater = std::max (target + block, std::min (target + std::max (4.0 * block, 2.0 * burst), 0.75 * ringFrames));
        if (avail > highWater)
        {
            const auto drop = static_cast<size_t> (avail - target);
            ring.skip (drop * ch);
            overflows.fetch_add (1, std::memory_order_relaxed);
            overflowDrops.fetch_add (drop, std::memory_order_relaxed);
            avail -= static_cast<double> (drop);
            smoothedFill = avail;
        }
    }

    // ---- 2. Priming: wait until the fill reaches the target -----------------
    if (! streamingState)
    {
        if (avail < target)
        {
            renderFadeToSilence (dest, numDestChannels, numFrames, addToDest);
            fillMsStat.store (static_cast<float> (1000.0 * avail / producerRate), std::memory_order_relaxed);
            return false;
        }

        // Packetised delivery: priming completes right after a producer packet
        // arrived, i.e. at the PEAK of the fill sawtooth, overshooting the
        // target by up to one packet. The loop regulates the AVERAGE fill
        // (~ peak - burst / 2), so:
        //  * drop the oldest frames above target + burst / 2 (the stream fades
        //    in from silence here, so this is inaudible and trims latency);
        //  * seed the fill smoother with the average, not the peak.
        // Seeding with the peak made every (re)prime start with a positive
        // error that the 1.5 s smoother never saw drain away, so a producer
        // slower than the correction range wound the integrator up in the
        // WRONG direction (consume faster) and underran ~4x more often.
        const double halfBurst = 0.5 * static_cast<double> (burstEstimate.load (std::memory_order_relaxed));
        if (const double excessFrames = avail - (target + halfBurst); excessFrames >= 1.0)
        {
            const auto excess = static_cast<size_t> (excessFrames);
            ring.skip (excess * ch);
            avail -= static_cast<double> (excess);
        }
        startStreaming (std::max (0.0, avail - halfBurst));
    }

    // ---- 3. PI controller on the smoothed fill level ------------------------
    const double dt = static_cast<double> (numFrames) / consumerRate;
    const double alpha = 1.0 - std::exp (-dt / kFillSmoothingSeconds);
    smoothedFill += alpha * (avail - smoothedFill);
    const double errorSeconds = (smoothedFill - target) / producerRate;
    integral = std::clamp (integral + errorSeconds * dt, -kMaxCorrection / kKi, kMaxCorrection / kKi);
    const double correction = std::clamp (kKp * errorSeconds + kKi * integral, -kMaxCorrection, kMaxCorrection);
    const auto step = static_cast<uint64_t> (std::llround (nominalRatio * (1.0 + correction) * static_cast<double> (kPhaseOne)));

    fillMsStat.store (static_cast<float> (1000.0 * smoothedFill / producerRate), std::memory_order_relaxed);
    correctionPpmStat.store (static_cast<float> (correction * 1.0e6), std::memory_order_relaxed);

    // ---- 4. Exactly how many producer frames this block consumes ------------
    const uint64_t endPhase = phase + step * static_cast<uint64_t> (numFrames);
    const auto needed = static_cast<size_t> (endPhase >> 32);

    if (static_cast<double> (needed) > avail || needed * ch > staging.size())
    {
        // Underrun: fade out from the last output value, then re-prime.
        renderFadeToSilence (dest, numDestChannels, numFrames, addToDest);
        underruns.fetch_add (1, std::memory_order_relaxed);
        streamingState = false;
        streamingStat.store (false, std::memory_order_relaxed);
        return false;
    }

    ring.pop (staging.data(), needed * ch);

    // ---- 5. Cubic Hermite (Catmull-Rom) interpolation -----------------------
    const float* in = staging.data();
    size_t consumed = 0;
    const int fifoChannels = channels;
    const float fadeStep = 1.0f / static_cast<float> (fadeLength);

    for (int i = 0; i < numFrames; ++i)
    {
        const float t = static_cast<float> (phase & (kPhaseOne - 1)) * kInvPhaseOne;
        float gain = 1.0f;
        if (fadeInRemaining > 0)
        {
            gain = 1.0f - static_cast<float> (fadeInRemaining) * fadeStep;
            --fadeInRemaining;
        }

        for (int c = 0; c < fifoChannels; ++c)
        {
            const auto& h = history[static_cast<size_t> (c)];
            const float c1 = 0.5f * (h[2] - h[0]);
            const float c2 = h[0] - 2.5f * h[1] + 2.0f * h[2] - 0.5f * h[3];
            const float c3 = 0.5f * (h[3] - h[0]) + 1.5f * (h[1] - h[2]);
            const float y = (((c3 * t + c2) * t + c1) * t + h[1]) * gain;
            lastOut[static_cast<size_t> (c)] = y;

            if (c < numDestChannels && dest[c] != nullptr)
            {
                if (addToDest)
                    dest[c][i] += y;
                else
                    dest[c][i] = y;
            }
        }

        phase += step;
        while (phase >= kPhaseOne)
        {
            phase -= kPhaseOne;
            const float* frame = in + consumed * ch;
            for (size_t c = 0; c < ch; ++c)
            {
                auto& h = history[c];
                h[0] = h[1];
                h[1] = h[2];
                h[2] = h[3];
                h[3] = frame[c];
            }
            ++consumed;
        }
    }

    if (! addToDest)
        for (int c = fifoChannels; c < numDestChannels; ++c)
            if (dest[c] != nullptr)
                std::memset (dest[c], 0, sizeof (float) * static_cast<size_t> (numFrames));

    return true;
}

DriftCompensatedFifo::Stats DriftCompensatedFifo::getStats() const noexcept
{
    Stats s;
    s.underruns = underruns.load (std::memory_order_relaxed);
    s.overflows = overflows.load (std::memory_order_relaxed);
    s.droppedFrames = overflowDrops.load (std::memory_order_relaxed) + ringFullDrops.load (std::memory_order_relaxed);
    s.framesPushed = framesPushed.load (std::memory_order_relaxed);
    s.fillMs = fillMsStat.load (std::memory_order_relaxed);
    s.targetMs = targetMsStat.load (std::memory_order_relaxed);
    s.correctionPpm = correctionPpmStat.load (std::memory_order_relaxed);
    s.streaming = streamingStat.load (std::memory_order_relaxed);
    return s;
}
} // namespace flub::app
