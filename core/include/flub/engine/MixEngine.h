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
// Idle freeze (docs/11 E45). A strip that is FED but silent (a VB-Cable or
// loopback input streaming zeros is the default Windows setup; the host
// passes nullptr only for a strip nobody feeds) costs a full chain per
// block. Once its fed input has stayed below kIdleFloorDb (-120 dBFS, every
// channel, sample peak) AND its own output (chain + pad, before the strip
// gain) has stayed below the same floor for the idle hold, the strip is
// frozen: its chain and pad are not run and it adds latency-aligned zeros
// to the mix (what its output already was). The hold is kIdleHoldSeconds
// plus the strip's latency. 10 s errs long on tails on purpose: the output
// criterion covers the audible ones (HRIR, gate hangover, auto-release),
// and by then the slow control state that silence still moves has settled
// too (measured: the Late Night compressor's release and the 7.1 fold's
// level match need 4-10 s), so a wake continues from the state continuous
// processing would have reached. While frozen only the input's peak is
// scanned, its last kIdlePreRollMs are kept and the strip gain keeps
// gliding. The first block with a sample at or above the floor (or a NaN)
// wakes it in the same block: the kept pre-roll runs through the chain and
// the pad first (output dropped: it belongs to blocks already played as
// zeros), so their short memory holds what continuous processing would
// have left there; then the block is processed as usual and the strip's
// output fades in over kIdleFadeMs (raised cosine), capped at the strip's
// latency so the fade covers only output of the silent input before the
// wake, never the new signal's direct path. A strip that never meets the
// floor for the hold is processed exactly as before, bit for bit.
// What a freeze cannot keep: the chain's control grids (the governor's
// 10 ms ticks, 25 / 50 / 100 ms meter and loudness steps) count processed
// samples, so after a freeze they tick at a different phase against the new
// signal, as if it had started up to one grid step earlier or later; and a
// state that never settles in silence (Gaming presets' footstep path) holds
// the value it had at the freeze. tests/test_idle_freeze.cpp has the
// numbers.
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

#include <atomic>
#include <cstdint>
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
    /** Idle freeze (docs/11 E45, see above): the silence floor, the hold on
        top of the strip's latency, the pre-roll run at a wake and the fade. */
    static constexpr float kIdleFloorDb = -120.0f;
    static constexpr double kIdleHoldSeconds = 10.0;
    static constexpr float kIdlePreRollMs = 10.0f, kIdleFadeMs = 5.0f;

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
    /** Idle freeze on (the default) or off; off wakes a frozen strip at the
        next block. Same thread rule as the setters above. */
    void setIdleFreeze (bool enabled) noexcept FLUB_NONBLOCKING { idleFreeze = enabled; }
    bool getIdleFreeze() const noexcept { return idleFreeze; }
    /** The hold on top of each strip's latency (kIdleHoldSeconds by default;
        tests shorten it). Same thread rule as the setters above. */
    void setIdleHoldSeconds (double seconds) noexcept FLUB_NONBLOCKING;
    /** Whether strip `strip` is frozen (docs/11 E45). Any thread (relaxed). */
    bool isStripFrozen (int strip) const noexcept;
    /** Blocks the strip spent frozen since configure() (any thread, relaxed). */
    uint64_t getStripFrozenBlocks (int strip) const noexcept;

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

        // Idle freeze (docs/11 E45); audio thread only, but for the atomics.
        AudioBuffer preRoll;              // the input's last preRollLength samples while frozen (a ring)
        AudioBuffer wakeScratch;          // the pre-roll in time order, run through the chain at a wake
        int preRollLength = 0, preRollPos = 0, preRollFill = 0;
        int64_t silentSamples = 0;        // consecutive fed input below the floor
        int64_t quietSamples = 0;         // consecutive output (chain + pad) below the floor
        int64_t holdSamples = 0;          // both must reach this to freeze
        int fadeLength = 0, fadePos = 0;  // the wake's fade-in; fadePos == fadeLength: none running
        bool frozen = false;
        std::atomic<bool> frozenFlag { false };
        std::atomic<uint64_t> frozenBlocks { 0 };
    };

    /** Idle freeze: the pre-roll through the chain and the pad, then the fade. */
    void wake (Strip& s) noexcept FLUB_NONBLOCKING;
    int64_t idleHoldFor (const Strip& s) const noexcept FLUB_NONBLOCKING;

    void build (const std::vector<StripConfig>& configs, double sr, int maxBlockSize,
                const std::vector<std::unique_ptr<Strip>>& storesFrom, const ChainSetup& setup);

    std::vector<std::unique_ptr<Strip>> strips;
    DeviceCorrection correction;
    TruePeakLimiter master;
    AudioBuffer mixBuffer;
    double sampleRate = 48000.0;
    int maxBlock = 512, maxStripLatency = 0;
    bool idleFreeze = true;
    double idleHoldSeconds = kIdleHoldSeconds;
};
} // namespace flub
