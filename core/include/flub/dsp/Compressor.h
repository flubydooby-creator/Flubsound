// Flubsound Pro - look-ahead compressor with optional upward compression.
//
// Detector: sidechain high-pass (sidechainHpHz, 12 dB/oct; stops bass and
//   explosions from pumping everything) -> peak |x| linked across ALL
//   channels (max) -> dB. Linking is mandatory: unlinked gain on a game mix
//   moves sounds between ears and corrupts positional cues.
// Downward gain computer (Giannoulis/Massberg/Reiss 2012, soft knee W):
//   2(x-T) < -W        : y = x
//   |2(x-T)| <= W      : y = x + (1/R - 1)(x - T + W/2)^2 / (2W)
//   2(x-T) > W         : y = T + (x - T)/R
//   gDown = y - x (<= 0)
// Upward computer ("environment detail" / night mode), when upMaxGainDb > 0:
//   x < upT : gUp = min(upMax, (upT - x)(1 - 1/upR)) * taper(x)
//   taper fades linearly from 1 at upFloorDb+12 to 0 at upFloorDb, so
//   silence and noise floors are not lifted.
// Total gain g = gDown + gUp, smoothed by GainSmoother (attack/release).
//   autoRelease: release time scales from releaseMs/4 (short transient
//   reduction) to releaseMs (sustained reduction > 100 ms) - program
//   dependent, avoids both pumping and "stuck" gain reduction.
// Look-ahead: audio is delayed by lookahead samples so the smoothed gain is
//   already moving when the transient arrives (latency = lookahead).
// Output: makeup (manual, or auto = -gDown at 0 dBFS / 2), dry/wet mix.
#pragma once

#include "Processor.h"

#include <atomic>

namespace flub
{
struct CompressorParams
{
    float thresholdDb = -18.0f; // -60 .. 0
    float ratio = 2.5f;         // 1 .. 20
    float kneeDb = 6.0f;        // 0 .. 24
    float attackMs = 10.0f;     // 0.1 .. 200
    float releaseMs = 120.0f;   // 10 .. 2000
    bool autoRelease = false;
    float makeupDb = 0.0f;      // -12 .. +24
    bool autoMakeup = false;
    float sidechainHpHz = 80.0f;// 0 = off, else 20 .. 300
    float mix = 1.0f;           // 0 .. 1 (parallel compression)

    float upThresholdDb = -45.0f; // -80 .. -10
    float upRatio = 2.0f;         // 1 .. 10
    float upMaxGainDb = 0.0f;     // 0 = off .. 18
    float upFloorDb = -75.0f;     // -100 .. -40

    bool operator== (const CompressorParams&) const = default;
};

class Compressor final : public Processor
{
public:
    /** Structural: call before prepare(). 0 .. 10 ms. */
    void setLookaheadMs (float ms) noexcept { lookaheadMs = ms; }

    void prepare (const ProcessSpec& spec) override;
    void reset() noexcept override;
    void process (const AudioBlock& block) noexcept override;
    int latencySamples() const noexcept override;
    const char* name() const noexcept override { return "Compressor"; }

    void setParams (const CompressorParams& p) noexcept;
    const CompressorParams& getParams() const noexcept { return params; }

    /** Static curve (dB in -> total gain dB), exposed for tests and the GUI. */
    static float computeGainDb (const CompressorParams& p, float levelDb) noexcept;

    /** Most negative (down) / most positive (up) gain applied in the last block. */
    float getGainReductionDb() const noexcept { return grDb.load (std::memory_order_relaxed); }
    float getUpwardGainDb() const noexcept { return upDb.load (std::memory_order_relaxed); }

private:
    // ---- implementation-defined below this line ----
    float lookaheadMs = 2.0f;
    ProcessSpec spec;
    CompressorParams params;
    std::atomic<float> grDb { 0.0f }, upDb { 0.0f };
};
} // namespace flub
