// Flubsound Pro - multi-strip mixer (per-application profiles and routing).
//
// Each strip = one source stream (a virtual endpoint such as "Flubsound Game",
// "Flubsound Music", "Flubsound Chat", or a per-process loopback capture) with
// its own ParameterStore (= profile) and ProcessingChain. Strips are summed
// and the sum passes a master safety true-peak limiter (-1 dBTP default)
// before the physical output. Different strips may run different latency
// profiles; each is padded to the largest strip latency so relative A/V sync
// between applications is preserved. The master look-ahead follows the
// strips: 0.5 ms when every strip runs the Low Latency profile, 1 ms
// otherwise (24 / 48 samples + the 20-sample detector at 48 kHz).
//
//   strip 0 (Game, 7.1) --chain--> pad --+
//   strip 1 (Music, 2)  --chain--> pad --+--> sum -> master limiter -> out
//   strip 2 (Chat, 2)   --chain--> pad --+
//
// Threading: configure() is non-RT (allocates, prepares chains). process() is
// the device callback. Profile/preset changes only touch ParameterStores.
#pragma once

#include "ProcessingChain.h"
#include "flub/common/Realtime.h"
#include "flub/dsp/TruePeakLimiter.h"

#include <memory>
#include <string>
#include <vector>

namespace flub
{
struct StripConfig
{
    std::string name = "Main";
    int inputChannels = 2;
    float gainDb = 0.0f;
    bool muted = false;
};

class MixEngine
{
public:
    static constexpr int kMaxStrips = 4;
    /** Master limiter look-ahead: all strips on Low Latency / any other mix. */
    static constexpr float kMasterLookaheadLowLatencyMs = 0.5f, kMasterLookaheadMs = 1.0f;

    /** Non-RT. Creates (or re-creates) strips and prepares everything; also
        picks the master look-ahead from the strips' latency profiles. */
    void configure (const std::vector<StripConfig>& strips, double sampleRate, int maxBlockSize);

    int getNumStrips() const noexcept { return static_cast<int> (strips.size()); }
    param::ParameterStore& params (int strip) noexcept { return *strips[static_cast<size_t> (strip)]->store; }
    ProcessingChain& chain (int strip) noexcept { return *strips[static_cast<size_t> (strip)]->chain; }

    /** AUDIO THREAD ONLY (between process() calls): these write plain fields
        and smoothers. Other threads hand values over through atomics, as
        the app's AudioEngineHost does (applyPendingMixSettings). */
    void setStripGainDb (int strip, float db) noexcept;
    void setStripMuted (int strip, bool muted) noexcept;
    void setMasterCeilingDb (float db) noexcept;

    /** RT. inputs[i] feeds strip i (channel count per its config, may be
        modified in place); out is stereo. Strips with no input pass nullptr. */
    void process (const AudioBlock* const* inputs, const AudioBlock& out) noexcept FLUB_NONBLOCKING;

    /** Total output latency (max strip latency + master limiter). */
    int getLatencySamples() const noexcept;
    float getMasterGainReductionDb() const noexcept { return master.getGainReductionDb(); }
    uint64_t getMasterSafetyClipCount() const noexcept { return master.getSafetyClipCount(); }

    /** Any chain that needs a structural re-prepare (host polls this). A
        latency-profile change is structural, so re-configuring also
        re-decides the master look-ahead. */
    bool needsReprepare() const noexcept;

private:
    struct Strip
    {
        StripConfig config;
        std::unique_ptr<param::ParameterStore> store;
        std::unique_ptr<ProcessingChain> chain;
        DelayLine pad;
        LinearSmoothedValue gain;
    };

    std::vector<std::unique_ptr<Strip>> strips;
    TruePeakLimiter master;
    AudioBuffer mixBuffer;
    double sampleRate = 48000.0;
    int maxBlock = 512, maxStripLatency = 0;
};
} // namespace flub
