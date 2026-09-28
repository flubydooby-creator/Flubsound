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
//   upRelativeFloor (Gaming, docs/11 E19): the floor also follows the
//   programme's background B - the detector level through a
//   BackgroundTracker (BackgroundTracker.h: rises <= 5 dB/s, falls
//   with 400 ms, never under upFloorDb) - and the lift is further scaled by
//   relTaper(x - B), 0 at 3 dB and 1 at 9 dB over B. A stationary bed (rain,
//   wind, room tone, a held tone) is its own background and is not lifted;
//   quiet sounds that rise out of it are. Switching it glides over 20 ms.
// Total gain g = gDown + gUp, smoothed by GainSmoother (attack/release).
//   autoRelease: release time scales from releaseMs/4 (short transient
//   reduction) to releaseMs (sustained reduction > 100 ms) - program
//   dependent, avoids both pumping and "stuck" gain reduction.
// Look-ahead: audio is delayed by lookahead samples so the smoothed gain is
//   already moving when the transient arrives (latency = lookahead).
// Output: makeup (manual, or auto = -gDown at 0 dBFS / 2), dry/wet mix.
#pragma once

#include "BackgroundTracker.h"
#include "Processor.h"
#include "Svf.h"
#include "flub/common/DelayLine.h"
#include "flub/common/SmoothedValue.h"

#include <array>
#include <atomic>
#include <cstdint>

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
    bool upRelativeFloor = false; // the upward floor follows the background (see above)

    bool operator== (const CompressorParams&) const = default;
};

class Compressor final : public Processor
{
public:
    /** Structural: call before prepare(). 0 .. 10 ms. */
    void setLookaheadMs (float ms) noexcept { lookaheadMs = ms; }

    void prepare (const ProcessSpec& spec) override;
    void reset() noexcept FLUB_NONBLOCKING override;
    void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING override;
    int latencySamples() const noexcept override;
    const char* name() const noexcept override { return "Compressor"; }

    void setParams (const CompressorParams& p) noexcept FLUB_NONBLOCKING;
    const CompressorParams& getParams() const noexcept { return params; }

    /** Static curve (dB in -> total gain dB), exposed for tests and the GUI.
        With upRelativeFloor, the background is taken as upFloorDb (the
        curve out of silence) unless backgroundDb is given. */
    static float computeGainDb (const CompressorParams& p, float levelDb) noexcept;
    static float computeGainDb (const CompressorParams& p, float levelDb, float backgroundDb) noexcept;

    /** Most negative (down) / most positive (up) gain applied in the last block. */
    float getGainReductionDb() const noexcept { return grDb.load (std::memory_order_relaxed); }
    float getUpwardGainDb() const noexcept { return upDb.load (std::memory_order_relaxed); }
    /** The upward section's background (dB) at the end of the last block. */
    float getUpwardBackgroundDb() const noexcept { return bgDb.load (std::memory_order_relaxed); }

private:
    // ---- implementation-defined below this line ----
    /** Static-curve parameters in the form the per-sample gain computer uses.
        Ratios are carried as slopes (1 - 1/R), which is what the curve needs
        and what glides most evenly when a ratio is smoothed. */
    struct Curve
    {
        float thresholdDb = -18.0f, kneeDb = 6.0f, slope = 0.6f;
        float upThresholdDb = -45.0f, upSlope = 0.5f, upMaxGainDb = 0.0f, upFloorDb = -75.0f;
        float upRelative = 0.0f; // 0 = fixed floor, 1 = background-relative (glides)
    };

    struct CurveGain
    {
        float down = 0.0f, up = 0.0f;
    };

    static CompressorParams sanitised (const CompressorParams& p, const CompressorParams& fallback) noexcept;
    static Curve makeCurve (const CompressorParams& p) noexcept;
    static CurveGain evaluateCurve (const Curve& c, float levelDb, float backgroundDb) noexcept;
    static float autoMakeupDb (const CompressorParams& p) noexcept;
    static float reductionOnsetGain (const Curve& c) noexcept;

    int holdSamplesFor (float sidechainHpHz) const noexcept;
    double timeCoeff (float ms) const noexcept;
    void updateTimeConstants() noexcept;
    void updateHpCoeffs() noexcept;
    void advanceSmoothers() noexcept;
    void housekeeping() noexcept;

    float lookaheadMs = 2.0f;
    ProcessSpec spec;
    CompressorParams params;
    std::atomic<float> grDb { 0.0f }, upDb { 0.0f }, bgDb { kMinusInfDb };

    int latency = 0;
    DelayLine delay;                                  // look-ahead: the dry and the wet path

    // Sidechain high-pass (12 dB/oct Butterworth SVF), crossfaded in / out.
    SvfCoeffs hpCoeffs;
    std::array<SvfState, kMaxChannels> hpState {};
    OnePoleSmoother logHpFreq;                        // ln(Hz)
    LinearSmoothedValue hpMix;                        // 0 = raw input, 1 = high-passed
    bool hpRunning = false;

    // Continuous parameters glide per sample (curve, makeup dB, mix).
    OnePoleSmoother thresholdS, kneeS, slopeS, upThresholdS, upSlopeS, upMaxS, upFloorS, upRelativeS, makeupS, mixS;
    Curve curve;
    bool smoothing = false, curveDirty = true, onsetDirty = true;

    // Linked peak detector with a two-bucket hold (see the .cpp).
    float bucketPeak = 0.0f, prevBucketPeak = 0.0f, heldPeak = -1.0f, levelDb = kMinusInfDb;
    int bucketLength = 1, bucketCountdown = 1;
    CurveGain target;

    // The upward section's background: the detector level through a
    // BackgroundTracker, stepped at control rate (always, so switching
    // upRelativeFloor on finds it current).
    BackgroundTracker background;
    float backgroundDb = kMinusInfDb;

    // Gain smoothing (dB) and program-dependent release. The state and the
    // coefficients are double: a float one-pole with c ~ 1 - 1e-5 stalls
    // (c (y - t) rounds back to y - t) up to ~0.7 dB short of its target.
    double gainDb = 0.0;
    double attackCoeff = 0.0, releaseCoeff = 0.0;
    float sustain = 0.0f, sustainStep = 0.0f;         // 0..1: how long the current reduction has lasted
    int activeRun = 0, loudRun = 0, sustainSamples = 4800;
    float onsetGain = 1.0f;                           // detector peak above which gDown < -0.5 dB
    float autoCoeffSustain = -1.0f;                   // cache of the auto-release coefficient
    double autoCoeff = 0.0;

    // Output gain cache: (1 - mix) + mix * 10^((gain + makeup) / 20).
    float lastWetDb = 0.0f, lastMix = -1.0f, outFactor = 1.0f;

    uint32_t tick = 0;                                // running sample counter (control-rate phase)
};
} // namespace flub
