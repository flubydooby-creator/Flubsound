#include "flub/engine/MixEngine.h"

#include "flub/common/Math.h"

#include <algorithm>

namespace flub
{
void MixEngine::configure (const std::vector<StripConfig>& configs, double sr, int maxBlockSize, const ChainSetup& setup)
{
    // Keep existing strips' parameter stores (their profiles/presets) when the
    // layout is re-configured, e.g. after a device sample-rate change.
    build (configs, sr, maxBlockSize, strips, setup);
}

void MixEngine::configureFrom (const MixEngine& previous, const std::vector<StripConfig>& configs, double sr, int maxBlockSize,
                               const ChainSetup& setup)
{
    correction.setSettingsNow (previous.correction.getSettings()); // prepared at the new rate in build()
    build (configs, sr, maxBlockSize, previous.strips, setup);
}

void MixEngine::build (const std::vector<StripConfig>& configs, double sr, int maxBlockSize,
                       const std::vector<std::unique_ptr<Strip>>& storesFrom, const ChainSetup& setup)
{
    sampleRate = sr;
    maxBlock = maxBlockSize;

    // storesFrom is this engine's own strips (configure) or a running engine's
    // (configureFrom): only its store pointers are copied, so it is untouched
    // until `strips` is replaced below.
    std::vector<std::unique_ptr<Strip>> next;
    const size_t count = std::min (configs.size(), static_cast<size_t> (kMaxStrips));
    for (size_t i = 0; i < count; ++i)
    {
        auto s = std::make_unique<Strip>();
        s->config = configs[i];
        s->config.inputChannels = std::clamp (s->config.inputChannels, 2, kMaxChannels);
        if (i < storesFrom.size() && storesFrom[i]->store != nullptr)
            s->store = storesFrom[i]->store;
        else
            s->store = std::make_shared<param::ParameterStore>();
        s->chain = std::make_unique<ProcessingChain> (*s->store);
        if (setup != nullptr)
            setup (static_cast<int> (i), *s->chain);
        s->chain->prepare ({ sr, maxBlockSize, s->config.inputChannels });
        if (i < storesFrom.size() && storesFrom[i]->chain != nullptr) s->chain->adoptGovernorState (*storesFrom[i]->chain); // docs/11 E06 (2): strength + learned governor state
        s->gain.reset (sr, 20.0f, s->config.muted ? 0.0f : dbToGain (s->config.gainDb));
        next.push_back (std::move (s));
    }
    strips = std::move (next);

    // Padding within sync groups only (docs/11 E40 part 3 / E42a): a strip
    // is delayed to the slowest chain of its group; one in no group is not.
    maxStripLatency = 0;
    for (auto& s : strips)
    {
        int groupLatency = s->chain->getLatencySamples();
        if (s->config.syncGroup != StripConfig::kNoSyncGroup)
            for (const auto& other : strips)
                if (other->config.syncGroup == s->config.syncGroup)
                    groupLatency = std::max (groupLatency, other->chain->getLatencySamples());
        s->padSamples = groupLatency - s->chain->getLatencySamples();
        s->pad.prepare (2, s->padSamples);
        maxStripLatency = std::max (maxStripLatency, groupLatency);
    }

    // Master safety limiter: only engages when the summed strips overshoot.
    // Its look-ahead follows the strips so that Low Latency stays low end to
    // end: 0.5 ms (as in the Low Latency maximizer) when every strip runs that
    // profile - the app applies one profile to all strips - and 1 ms
    // otherwise. A profile change makes the strip's chain report
    // needsReprepare(), which brings the host back here.
    const auto isLowLatency = [] (const auto& s) { return s->chain->getLatencyProfile() == param::LatencyProfileValue::LowLatency; };
    const bool allLowLatency = ! strips.empty() && std::all_of (strips.begin(), strips.end(), isLowLatency);
    correction.prepare ({ sr, maxBlockSize, 2 });
    master.setLookaheadMs (allLowLatency ? kMasterLookaheadLowLatencyMs : kMasterLookaheadMs);
    master.setTruePeakDetection (true);
    // The LF-safe envelope (docs/11 E05; TruePeakLimiter.h), as on the
    // maximizer and the bypass reference: when the sum does overshoot - a
    // game's explosion over music - its bass is held flat between its peaks
    // instead of rippling at their rate (40 Hz 6 / 10 dB over: THD+N -32.9 /
    // -29.2 dB -> below -300 dB, tests/test_limiter.cpp). The latency is
    // unchanged.
    master.setEnvelope ({ true, true, true });
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

    correction.process (mix);
    master.process (mix);
    out.firstChannels (2).subBlock (0, n).copyFrom (mix);
    for (int c = 2; c < out.numChannels; ++c)
        std::fill (out.channel (c), out.channel (c) + n, 0.0f);
}

int MixEngine::getLatencySamples() const noexcept
{
    return maxStripLatency + master.latencySamples();
}

int MixEngine::getStripLatencySamples (int strip) const noexcept
{
    if (strip < 0 || strip >= getNumStrips())
        return 0;
    const auto& s = *strips[static_cast<size_t> (strip)];
    return s.chain->getLatencySamples() + s.padSamples + master.latencySamples();
}

int MixEngine::getStripPaddingSamples (int strip) const noexcept
{
    return strip >= 0 && strip < getNumStrips() ? strips[static_cast<size_t> (strip)]->padSamples : 0;
}

bool MixEngine::needsReprepare() const noexcept
{
    return std::any_of (strips.begin(), strips.end(), [] (const auto& s) { return s->chain->needsReprepare(); });
}
} // namespace flub
