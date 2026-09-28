// Flubsound Pro - multi-strip mixer (per-application profiles and routing).
//
// Each strip = one source stream (a virtual endpoint such as "Flubsound Game",
// "Flubsound Music", "Flubsound Chat", or a per-process loopback capture) with
// its own ParameterStore (= profile) and ProcessingChain. Strips are summed
// and the sum passes a master safety true-peak limiter (-1 dBTP default)
// before the physical output. Different strips may run different latency
// profiles. Strips that share A/V content are put in one sync group
// (StripConfig::syncGroup) and padded to the slowest strip of that group, so
// their relative sync is preserved; a strip in no group (the default) keeps
// its own chain latency, so a Quality Music strip no longer delays a Low
// Latency Game strip (docs/11 E40 / E42a). The master look-ahead follows the
// strips: 0.5 ms when every strip runs the Low Latency profile, 1 ms
// otherwise (24 / 48 samples + the 20-sample detector at 48 kHz).
//
//   strip 0 (Game, 7.1) --chain--> pad --+
//   strip 1 (Music, 2)  --chain--> pad --+--> sum -> device correction -> master limiter -> out
//   strip 2 (Chat, 2)   --chain--> pad --+
//   (pad = slowest chain in the strip's sync group - own chain; 0 alone)
//
// The device correction (docs/11 E15, DeviceCorrection.h) is the output
// endpoint's headphone / speaker correction curve with its automatic
// preamp: zero latency, flat and free until the host gives it a curve,
// never touched by presets, A/B banks or strip parameters. configure()
// keeps its settings; configureFrom() copies the running engine's.
//
// Threading: configure() is non-RT (allocates, prepares chains). process() is
// the device callback. Profile/preset changes only touch ParameterStores.
// configureFrom() builds a second engine beside a running one (sharing its
// ParameterStores), so a host can swap engines without stopping the audio.
#pragma once

#include "ProcessingChain.h"
#include "flub/common/Realtime.h"
#include "flub/dsp/DeviceCorrection.h"
#include "flub/dsp/TruePeakLimiter.h"

#include <functional>
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
    /** Strips with the same syncGroup >= 0 are padded to the slowest of them
        (they carry one A/V programme, e.g. a game and its separately routed
        voice or cinematics); kNoSyncGroup (the default) = a group of its own:
        never padded, never delays another strip. Applied at configure(). */
    static constexpr int kNoSyncGroup = -1;
    int syncGroup = kNoSyncGroup;
};

class MixEngine
{
public:
    static constexpr int kMaxStrips = 4;
    /** Master limiter look-ahead: all strips on Low Latency / any other mix. */
    static constexpr float kMasterLookaheadLowLatencyMs = 0.5f, kMasterLookaheadMs = 1.0f;

    /** Non-RT. Called for every new strip's chain before its prepare(), to
        set up what takes effect at a prepare, e.g. a neural model
        (ProcessingChain::setNeuralModel), so it is part of the engine from
        its first block and in its latency. */
    using ChainSetup = std::function<void (int strip, ProcessingChain& chain)>;

    /** Non-RT. Creates (or re-creates) strips and prepares everything; also
        picks the master look-ahead from the strips' latency profiles. */
    void configure (const std::vector<StripConfig>& strips, double sampleRate, int maxBlockSize, const ChainSetup& setup = {});

    /** Non-RT. As configure(), for a NEW engine that is to replace `previous`
        while `previous` may still be running process() on the audio thread:
        strip i shares previous's ParameterStore i (strips beyond previous's
        count get a fresh one), so profiles carry over and both engines follow
        the same parameters during a crossfaded swap. `previous` is only read
        (its store pointers are copied, never moved); the stores live as long
        as either engine. */
    void configureFrom (const MixEngine& previous, const std::vector<StripConfig>& strips, double sampleRate, int maxBlockSize,
                        const ChainSetup& setup = {});

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

    /** The largest output latency of any strip (its sync group's slowest
        chain + the master limiter); constant until the next configure(). */
    int getLatencySamples() const noexcept;

    /** Per strip (docs/11 E42a), constant until the next configure(). Output
        latency = the strip's own chain + its padding + the master limiter. */
    int getStripLatencySamples (int strip) const noexcept;
    /** Delay added to align the strip with the slowest strip of its sync
        group; 0 for a strip in no group or the slowest of its group. */
    int getStripPaddingSamples (int strip) const noexcept;
    /** The master safety limiter's latency (look-ahead + detector). */
    int getMasterLatencySamples() const noexcept { return master.latencySamples(); }
    float getMasterGainReductionDb() const noexcept { return master.getGainReductionDb(); }
    uint64_t getMasterSafetyClipCount() const noexcept { return master.getSafetyClipCount(); }

    /** The output endpoint's correction on the stereo sum, before the master
        limiter (docs/11 E15). Its setters are control-thread only
        (DeviceCorrection::setSettings hands designs to the audio thread);
        use setSettingsNow() only on an engine that is not yet running. */
    DeviceCorrection& getDeviceCorrection() noexcept { return correction; }
    const DeviceCorrection& getDeviceCorrection() const noexcept { return correction; }

    /** Any chain that needs a structural re-prepare (host polls this). A
        latency-profile change is structural, so re-configuring also
        re-decides the master look-ahead. */
    bool needsReprepare() const noexcept;

private:
    struct Strip
    {
        StripConfig config;
        std::shared_ptr<param::ParameterStore> store; // shared with the engine it was configured from
        std::unique_ptr<ProcessingChain> chain;
        DelayLine pad;
        int padSamples = 0;
        LinearSmoothedValue gain;
    };

    void build (const std::vector<StripConfig>& configs, double sr, int maxBlockSize,
                const std::vector<std::unique_ptr<Strip>>& storesFrom, const ChainSetup& setup);

    std::vector<std::unique_ptr<Strip>> strips;
    DeviceCorrection correction;
    TruePeakLimiter master;
    AudioBuffer mixBuffer;
    double sampleRate = 48000.0;
    int maxBlock = 512, maxStripLatency = 0;
};
} // namespace flub
