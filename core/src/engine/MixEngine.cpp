#include "flub/engine/MixEngine.h"

#include "flub/common/Math.h"

#include <algorithm>

namespace flub
{
void MixEngine::configure (const std::vector<StripConfig>& configs, double sr, int maxBlockSize)
{
    sampleRate = sr;
    maxBlock = maxBlockSize;

    // Keep existing strips' parameter stores (their profiles/presets) when the
    // layout is re-configured, e.g. after a device sample-rate change.
    std::vector<std::unique_ptr<Strip>> next;
    const size_t count = std::min (configs.size(), static_cast<size_t> (kMaxStrips));
    for (size_t i = 0; i < count; ++i)
    {
        auto s = std::make_unique<Strip>();
        s->config = configs[i];
        s->config.inputChannels = std::clamp (s->config.inputChannels, 2, kMaxChannels);
        if (i < strips.size() && strips[i]->store != nullptr)
            s->store = std::move (strips[i]->store);
        else
            s->store = std::make_unique<param::ParameterStore>();
        s->chain = std::make_unique<ProcessingChain> (*s->store);
        s->chain->prepare ({ sr, maxBlockSize, s->config.inputChannels });
        s->gain.reset (sr, 20.0f, s->config.muted ? 0.0f : dbToGain (s->config.gainDb));
        next.push_back (std::move (s));
    }
    strips = std::move (next);

    maxStripLatency = 0;
    for (const auto& s : strips)
        maxStripLatency = std::max (maxStripLatency, s->chain->getLatencySamples());
    for (auto& s : strips)
        s->pad.prepare (2, maxStripLatency - s->chain->getLatencySamples());

    // Master safety limiter: only engages when the summed strips overshoot.
    // Its look-ahead follows the strips so that Low Latency stays low end to
    // end: 0.5 ms (as in the Low Latency maximizer) when every strip runs that
    // profile - the app applies one profile to all strips - and 1 ms
    // otherwise. A profile change makes the strip's chain report
    // needsReprepare(), which brings the host back here.
    const auto isLowLatency = [] (const auto& s) { return s->chain->getLatencyProfile() == param::LatencyProfileValue::LowLatency; };
    const bool allLowLatency = ! strips.empty() && std::all_of (strips.begin(), strips.end(), isLowLatency);
    master.setLookaheadMs (allLowLatency ? kMasterLookaheadLowLatencyMs : kMasterLookaheadMs);
    master.setTruePeakDetection (true);
    master.prepare ({ sr, maxBlockSize, 2 });
    LimiterParams lp;
    lp.ceilingDb = -1.0f;
    lp.releaseMs = 50.0f;
    lp.autoRelease = true;
    master.setParams (lp);

    mixBuffer.setSize (2, maxBlockSize);
}

void MixEngine::setStripGainDb (int strip, float db) noexcept
{
    auto& s = *strips[static_cast<size_t> (strip)];
    s.config.gainDb = db;
    s.gain.setTarget (s.config.muted ? 0.0f : dbToGain (db));
}

void MixEngine::setStripMuted (int strip, bool muted) noexcept
{
    auto& s = *strips[static_cast<size_t> (strip)];
    s.config.muted = muted;
    s.gain.setTarget (muted ? 0.0f : dbToGain (s.config.gainDb));
}

void MixEngine::setMasterCeilingDb (float db) noexcept
{
    auto lp = master.getParams();
    lp.ceilingDb = db;
    master.setParams (lp);
}

void MixEngine::process (const AudioBlock* const* inputs, const AudioBlock& out) noexcept FLUB_NONBLOCKING
{
    const int n = std::min (out.numSamples, maxBlock);
    const AudioBlock mix = mixBuffer.block (2, n);
    mix.clear();

    for (size_t i = 0; i < strips.size(); ++i)
    {
        auto& s = *strips[i];
        const AudioBlock* in = inputs != nullptr ? inputs[i] : nullptr;
        if (in == nullptr || in->numChannels < s.config.inputChannels)
            continue;

        const AudioBlock io = in->firstChannels (s.config.inputChannels).subBlock (0, n);
        s.chain->process (io);
        const AudioBlock st = io.firstChannels (2);
        s.pad.process (st);

        const float g0 = s.gain.getCurrent();
        const float g1 = s.gain.skip (n);
        const float step = (g1 - g0) / static_cast<float> (n);
        for (int c = 0; c < 2; ++c)
        {
            const float* src = st.channel (c);
            float* dst = mix.channel (c);
            float g = g0;
            for (int k = 0; k < n; ++k, g += step)
                dst[k] += g * src[k];
        }
    }

    master.process (mix);
    out.firstChannels (2).subBlock (0, n).copyFrom (mix);
    for (int c = 2; c < out.numChannels; ++c)
        std::fill (out.channel (c), out.channel (c) + n, 0.0f);
}

int MixEngine::getLatencySamples() const noexcept
{
    return maxStripLatency + master.latencySamples();
}

bool MixEngine::needsReprepare() const noexcept
{
    return std::any_of (strips.begin(), strips.end(), [] (const auto& s) { return s->chain->needsReprepare(); });
}
} // namespace flub
