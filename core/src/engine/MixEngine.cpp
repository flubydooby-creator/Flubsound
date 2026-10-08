#include "flub/engine/MixEngine.h"

#include "flub/common/Math.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

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
    idleFreeze = previous.idleFreeze; // docs/11 E45; the new strips start awake
    idleHoldSeconds = previous.idleHoldSeconds;
    hearing.carryFrom (previous.hearing); // docs/11 E32 (c): its settings, dose and cap gain
    // docs/11 E22: the chat settings, and an active talker with the duck
    // where it stands (`previous` may be running: its atomics only).
    requestedDuck.store (previous.getChatDuck(), std::memory_order_relaxed);
    requestedDuckDepthDb.store (previous.getChatDuckDepthDb(), std::memory_order_relaxed);
    requestedChatMix.store (previous.getChatMix(), std::memory_order_relaxed);
    requestedRoomFloorDb.store (previous.getChatRoomFloorDb(), std::memory_order_relaxed);
    build (configs, sr, maxBlockSize, previous.strips, setup);
    if (chatStrip >= 0 && previous.isChatVoiceActive())
        voice.seedActive();
    if (chatDuck)
    {
        const float carried = previous.getChatDuckAmount();
        for (auto& s : strips)
            if (s->role == StripRole::Game || s->role == StripRole::Music)
                s->ducker.reset (carried);
        duckAmount.store (carried, std::memory_order_relaxed);
    }
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
    chatDuck = requestedDuck.load (std::memory_order_relaxed); // docs/11 E22: a new engine starts on the requests
    chatDuckDepthDb = requestedDuckDepthDb.load (std::memory_order_relaxed);
    chatMix = requestedChatMix.load (std::memory_order_relaxed);
    roomFloorDb = requestedRoomFloorDb.load (std::memory_order_relaxed);
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
        // docs/11 E06 (2): the replaced chain's protection strength and what
        // its governor has learned (it may still be running: a seqlock read).
        if (i < storesFrom.size() && storesFrom[i]->chain != nullptr)
            s->chain->adoptGovernorState (*storesFrom[i]->chain);
        s->gain.reset (sr, 20.0f, s->config.muted ? 0.0f : dbToGain (s->config.gainDb));
        s->preRollLength = std::max (1, static_cast<int> (std::lround (kIdlePreRollMs * 0.001 * sr)));
        s->preRoll.setSize (s->config.inputChannels, s->preRollLength);
        s->wakeScratch.setSize (s->config.inputChannels, s->preRollLength);
        // docs/11 E22: the strip's role, its duck and its ChatMix gain.
        s->role = roleForName (s->config.name);
        s->ducker.prepare (sr, s->role == StripRole::Game ? ChatDucker::Shape::Game : ChatDucker::Shape::Music);
        s->mixGain.reset (sr, kChatMixRampMs, chatMixGain (chatMix, s->role));
        next.push_back (std::move (s));
    }
    strips = std::move (next);
    chatStrip = -1;
    for (size_t i = 0; i < strips.size() && chatStrip < 0; ++i)
        if (strips[i]->role == StripRole::Chat)
            chatStrip = static_cast<int> (i);
    hasGameStrip = std::any_of (strips.begin(), strips.end(), [] (const auto& s) { return s->role == StripRole::Game; });
    voice.prepare (sr);
    room.prepare (sr, maxBlockSize);
    duckAmount.store (0.0f, std::memory_order_relaxed);

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
        // Idle freeze (docs/11 E45): the hold outlasts the strip's longest
        // tail; the wake's fade never reaches past its latency.
        s->holdSamples = idleHoldFor (*s);
        s->fadeLength = std::min (static_cast<int> (std::lround (kIdleFadeMs * 0.001 * sr)), groupLatency);
        s->fadePos = s->fadeLength;
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
    hearing.prepare (sr); // docs/11 E32 (c): keeps its settings, the dose and the cap's gain
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

MixEngine::StripRole MixEngine::roleForName (const std::string& name) noexcept
{
    const auto is = [&name] (const char* role) {
        size_t i = 0;
        for (; role[i] != '\0'; ++i)
            if (i >= name.size() || std::tolower (static_cast<unsigned char> (name[i])) != role[i])
                return false;
        return i == name.size();
    };
    if (is ("game"))
        return StripRole::Game;
    if (is ("music"))
        return StripRole::Music;
    if (is ("chat"))
        return StripRole::Chat;
    return StripRole::Other;
}

MixEngine::StripRole MixEngine::getStripRole (int strip) const noexcept
{
    return strip >= 0 && strip < getNumStrips() ? strips[static_cast<size_t> (strip)]->role : StripRole::Other;
}

float MixEngine::chatMixGain (float balance, StripRole role) noexcept
{
    const float b = std::clamp (balance, -1.0f, 1.0f);
    if (role == StripRole::Game)
        return 1.0f - std::max (0.0f, b);
    if (role == StripRole::Chat)
        return 1.0f + std::min (0.0f, b);
    return 1.0f;
}

void MixEngine::setChatDuck (bool enabled, float depthDb) noexcept FLUB_NONBLOCKING
{
    requestedDuckDepthDb.store (std::isfinite (depthDb) ? std::clamp (depthDb, ChatDucker::kMinDepthDb, ChatDucker::kMaxDepthDb)
                                                        : ChatDucker::kDefaultDepthDb,
                                std::memory_order_relaxed);
    requestedDuck.store (enabled, std::memory_order_relaxed);
}

void MixEngine::setChatMix (float balance) noexcept FLUB_NONBLOCKING
{
    requestedChatMix.store (std::isfinite (balance) ? std::clamp (balance, -1.0f, 1.0f) : 0.0f, std::memory_order_relaxed);
}

void MixEngine::setChatRoomFloorDb (float floorDb) noexcept FLUB_NONBLOCKING
{
    requestedRoomFloorDb.store (std::isfinite (floorDb) ? std::clamp (floorDb, ChatDucker::kMinRoomFloorDb, 0.0f) : ChatDucker::kDefaultRoomFloorDb,
                                std::memory_order_relaxed);
}

void MixEngine::applyChatRequests() noexcept FLUB_NONBLOCKING
{
    chatDuck = requestedDuck.load (std::memory_order_relaxed);
    chatDuckDepthDb = requestedDuckDepthDb.load (std::memory_order_relaxed);
    roomFloorDb = requestedRoomFloorDb.load (std::memory_order_relaxed);
    const float balance = requestedChatMix.load (std::memory_order_relaxed);
    if (balance != chatMix)
    {
        chatMix = balance;
        for (auto& s : strips)
            s->mixGain.setTarget (chatMixGain (chatMix, s->role));
    }
}

const ChatDucker* MixEngine::getChatDucker (int strip) const noexcept
{
    if (strip < 0 || strip >= getNumStrips())
        return nullptr;
    const auto& s = *strips[static_cast<size_t> (strip)];
    return s.role == StripRole::Game || s.role == StripRole::Music ? &s.ducker : nullptr;
}

namespace
{
/** True when every sample of `b` is below `floor` in magnitude (a NaN is not). */
bool isBelowFloor (const AudioBlock& b, float floor) noexcept
{
    for (int c = 0; c < b.numChannels; ++c)
    {
        const float* x = b.channel (c);
        for (int k = 0; k < b.numSamples; ++k)
            if (! (std::abs (x[k]) < floor))
                return false;
    }
    return true;
}
} // namespace

void MixEngine::wake (Strip& s) noexcept FLUB_NONBLOCKING
{
    // The pre-roll (the input's last samples while frozen, all below the
    // floor) in time order through the chain and the pad; what they give out
    // belongs to blocks already played as zeros and is dropped.
    const int fill = s.preRollFill;
    const AudioBlock roll = s.wakeScratch.block (s.config.inputChannels, fill);
    const int start = (s.preRollPos - fill + s.preRollLength) % s.preRollLength;
    for (int c = 0; c < roll.numChannels; ++c)
    {
        const float* src = s.preRoll.block().channel (c);
        float* dst = roll.channel (c);
        for (int k = 0; k < fill; ++k)
            dst[k] = src[(start + k) % s.preRollLength];
    }
    for (int pos = 0; pos < fill; pos += maxBlock)
    {
        const AudioBlock part = roll.subBlock (pos, std::min (maxBlock, fill - pos));
        s.chain->process (part);
        s.pad.process (part.firstChannels (2));
    }
    s.frozen = false;
    s.frozenFlag.store (false, std::memory_order_relaxed);
    s.silentSamples = s.quietSamples = 0;
    s.fadePos = 0;
}

void MixEngine::process (const AudioBlock* const* inputs, const AudioBlock& out) noexcept FLUB_NONBLOCKING
{
    const int n = std::min (out.numSamples, maxBlock);
    const AudioBlock mix = mixBuffer.block (2, n);
    mix.clear();
    const float floor = dbToGain (kIdleFloorDb);
    applyChatRequests(); // docs/11 E22: setChatDuck / setChatMix from any thread

    // Chat sidechain (docs/11 E22): the Chat strip's input, before its chain
    // (which processes in place) and before any strip it ducks.
    if (chatStrip >= 0)
    {
        const AudioBlock* chatIn = inputs != nullptr ? inputs[chatStrip] : nullptr;
        if (chatIn != nullptr && chatIn->numChannels >= strips[static_cast<size_t> (chatStrip)]->config.inputChannels)
            voice.process (chatIn->firstChannels (2).subBlock (0, n));
        else
            voice.processSilence (n);
    }
    const bool voiceActive = chatDuck && voice.isActive();
    const float masterCeilingDb = master.getParams().ceilingDb;
    float duckNow = 0.0f;

    // Two passes (docs/11 E22's room needs the Chat strip's block before a
    // Game strip's duck): first every strip's chain, pad, wake fade and idle
    // decision, in place in the caller's blocks; then, in strip order, each
    // strip's duck, gain and its share of the sum. Each step touches only
    // its own strip and the sum is taken in the same order as in one pass,
    // so the output is the same bit for bit.
    std::array<bool, kMaxStrips> processed {};
    for (size_t i = 0; i < strips.size(); ++i)
    {
        auto& s = *strips[i];
        const AudioBlock* in = inputs != nullptr ? inputs[i] : nullptr;
        if (in == nullptr || in->numChannels < s.config.inputChannels)
            continue;

        const AudioBlock io = in->firstChannels (s.config.inputChannels).subBlock (0, n);
        // Idle freeze (docs/11 E45): a frozen strip only watches its input.
        const bool silent = idleFreeze && isBelowFloor (io, floor);
        if (s.frozen)
        {
            if (silent)
            {
                for (int c = 0; c < io.numChannels; ++c)
                {
                    const float* src = io.channel (c);
                    float* ring = s.preRoll.block().channel (c);
                    int pos = s.preRollPos;
                    for (int k = 0; k < n; ++k)
                    {
                        ring[pos] = src[k];
                        pos = pos + 1 == s.preRollLength ? 0 : pos + 1;
                    }
                }
                s.preRollPos = static_cast<int> ((s.preRollPos + n) % s.preRollLength);
                s.preRollFill = std::min (s.preRollLength, s.preRollFill + n);
                s.gain.skip (n);
                s.mixGain.skip (n);
                if (s.role == StripRole::Game || s.role == StripRole::Music)
                {
                    s.ducker.skip (n, voiceActive);
                    duckNow = std::max (duckNow, s.ducker.getAmount());
                }
                s.frozenBlocks.fetch_add (1, std::memory_order_relaxed);
                continue;
            }
            wake (s);
        }

        s.chain->process (io);
        const AudioBlock st = io.firstChannels (2);
        s.pad.process (st);

        if (s.fadePos < s.fadeLength)
        {
            // The wake's fade-in (raised cosine over fadeLength samples).
            const int len = std::min (n, s.fadeLength - s.fadePos);
            const double w = kPi / static_cast<double> (s.fadeLength);
            for (int k = 0; k < len; ++k)
            {
                const float g = static_cast<float> (0.5 - 0.5 * std::cos (w * static_cast<double> (s.fadePos + k)));
                st.channel (0)[k] *= g;
                st.channel (1)[k] *= g;
            }
            s.fadePos += len;
        }

        if (idleFreeze)
        {
            s.silentSamples = silent ? s.silentSamples + n : 0;
            s.quietSamples = isBelowFloor (st, floor) ? s.quietSamples + n : 0;
            if (s.silentSamples >= s.holdSamples && s.quietSamples >= s.holdSamples)
            {
                // Its output is already zero to within the floor: from the
                // next block on it adds nothing (latency-aligned zeros).
                s.frozen = true;
                s.frozenFlag.store (true, std::memory_order_relaxed);
                s.preRollPos = s.preRollFill = 0;
            }
        }
        processed[i] = true;
    }

    // The room (docs/11 E22): the Chat strip's share of this block's sum
    // (its output after its gain, ChatRoomEnvelope), for the Game strips'
    // ducks; only while one of them may duck, so off it costs nothing.
    const float* chatLevel = nullptr;
    float roomCeiling = 1.0f;
    const bool anyGameDucking = std::any_of (strips.begin(), strips.end(), [] (const auto& s) {
        return s->role == StripRole::Game && ! s->ducker.isIdle();
    });
    if (chatDuck && roomFloorDb < 0.0f && chatStrip >= 0 && hasGameStrip && (voiceActive || anyGameDucking))
    {
        const auto& c = *strips[static_cast<size_t> (chatStrip)];
        if (processed[static_cast<size_t> (chatStrip)])
        {
            const AudioBlock chatOut = inputs[chatStrip]->firstChannels (2).subBlock (0, n);
            LinearSmoothedValue gain = c.gain, mixGain = c.mixGain; // the glide the sum below takes
            const float g0 = gain.getCurrent() * mixGain.getCurrent();
            const float g1 = gain.skip (n) * mixGain.skip (n);
            chatLevel = room.process (&chatOut, n, g0, g1);
        }
        else
            chatLevel = room.process (nullptr, n, 0.0f, 0.0f); // frozen or not fed: silence
        // The device correction sits between the sum and the master limiter:
        // a boost after its preamp lowers the room by that much (a curve
        // that only cuts counts as flat).
        const float correctionGain = correction.getMaxGain();
        roomCeiling = dbToGain (masterCeilingDb) / (std::isfinite (correctionGain) ? std::max (1.0f, correctionGain) : 1.0f);
    }
    else
        room.reset();

    for (size_t i = 0; i < strips.size(); ++i)
    {
        if (! processed[i])
            continue;
        auto& s = *strips[i];
        const AudioBlock st = inputs[i]->firstChannels (2).subBlock (0, n);

        // The voice-keyed duck (docs/11 E22), after the idle decision (it
        // judges the strip's own output) and before the strip gain. Off,
        // or idle with no voice, it does not touch the signal.
        if (s.role == StripRole::Game || s.role == StripRole::Music)
        {
            ChatDucker::Control dc;
            dc.voiceActive = voiceActive;
            dc.depthDb = chatDuckDepthDb;
            dc.ceilingDb = masterCeilingDb;
            if (s.role == StripRole::Game && chatLevel != nullptr)
            {
                // The room: the strip's gain after the duck, as the sum below glides it.
                LinearSmoothedValue gain = s.gain, mixGain = s.mixGain;
                dc.postGain0 = gain.getCurrent() * mixGain.getCurrent();
                dc.postGain1 = gain.skip (n) * mixGain.skip (n);
                dc.chatLevel = chatLevel;
                dc.roomCeiling = roomCeiling;
                dc.roomFloorDb = roomFloorDb;
            }
            // The Game chain's Voice & Score lift (Gaming mode band 7, as
            // its MeterBus published it for this block).
            if (s.role == StripRole::Game && (voiceActive || ! s.ducker.isIdle())
                && std::lround (s.chain->effectiveValue (param::Mode)) == static_cast<long> (param::ModeValue::Gaming))
                dc.liftDb = std::max (0.0f, s.chain->meters().dynEqGainDb[static_cast<size_t> (ProcessingChain::kFirstModeBand + 3)].load (
                                                std::memory_order_relaxed));
            s.ducker.process (st, dc);
            duckNow = std::max (duckNow, s.ducker.getAmount());
        }

        // The strip gain and ChatMix's gain (1 unless it moved: then the
        // product glides linearly across the block).
        const float g0 = s.gain.getCurrent() * s.mixGain.getCurrent();
        const float g1 = s.gain.skip (n) * s.mixGain.skip (n);
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

    duckAmount.store (duckNow, std::memory_order_relaxed);

    correction.process (mix);
    master.process (mix);
    hearing.process (mix); // docs/11 E32 (c): the level estimate and the optional cap
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

int64_t MixEngine::idleHoldFor (const Strip& s) const noexcept FLUB_NONBLOCKING
{
    return static_cast<int64_t> (std::ceil (idleHoldSeconds * sampleRate)) + s.chain->getLatencySamples() + s.padSamples;
}

void MixEngine::setIdleHoldSeconds (double seconds) noexcept FLUB_NONBLOCKING
{
    idleHoldSeconds = std::max (0.0, seconds);
    for (auto& s : strips)
        s->holdSamples = idleHoldFor (*s);
}

bool MixEngine::isStripFrozen (int strip) const noexcept
{
    return strip >= 0 && strip < getNumStrips() && strips[static_cast<size_t> (strip)]->frozenFlag.load (std::memory_order_relaxed);
}

uint64_t MixEngine::getStripFrozenBlocks (int strip) const noexcept
{
    return strip >= 0 && strip < getNumStrips() ? strips[static_cast<size_t> (strip)]->frozenBlocks.load (std::memory_order_relaxed) : 0;
}

bool MixEngine::needsReprepare() const noexcept
{
    return std::any_of (strips.begin(), strips.end(), [] (const auto& s) { return s->chain->needsReprepare(); });
}
} // namespace flub
