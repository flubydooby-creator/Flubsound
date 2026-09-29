// Flubsound Pro - listening-level estimate, dose estimate and listening-level
// cap on the mix output (docs/11 E32 (c), docs/03 §14.12).
//
// An ESTIMATE, not a measurement and not a medical device: it knows the
// digital signal, the operating system's endpoint volume and a sensitivity
// the host hands in, never the headset's own volume dial, its fit on the
// head or its on-board EQ (those can put the estimate 10 - 20 dB off; only
// a coupler measurement calibrates it, and the device lab has not run yet).
//
//   estimate  A-weighted level of each ear's signal as it leaves MixEngine
//             (after the master limiter), in dB re a full-scale sine
//             (a 0 dBFS sine reads 0 dB), plus the endpoint volume (dB, 0
//             = full volume) plus the sensitivity (dB SPL a 0 dBFS sine
//             plays at full endpoint volume). The louder ear counts.
//             A-weighting: IEC 61672-1's analog A curve as two bilinear
//             high-pass sections (20.6 Hz double, 107.7 / 737.9 Hz) and a
//             matched-z double pole at 12.2 kHz, normalised to 0 dB at 1 kHz
//             (within 0.05 dB of the curve from 20 Hz to 4 kHz and within
//             0.6 dB up to 12.5 kHz at 44.1 / 48 kHz; tests/test_hearing_guard.cpp).
//   readings  the level now (125 ms, "Fast"), the Leq over the last 5 s, the
//             session Leq (since prepare()) and the dose.
//   dose      equal-energy (3 dB exchange) against the WHO / ITU-T H.870
//             reference of 80 dB(A) for 40 h a week: 1.0 is the whole
//             weekly allowance, so 2 h at 86 dB(A) is 2 / 40 x 10^0.6 =
//             0.199. The engine's clock is the sample count (Dosimeter is
//             the same arithmetic on a synthetic clock). The host owns the
//             calendar: it passes the day's persisted dose in
//             (setDoseBaseline) and reads the monotonic session dose to add
//             to it; the rolling 7-day sum and any notification are the
//             host's.
//   cap       optional (off by default): a slow limiter that holds the
//             estimated A-weighted Leq over ANY 5 s window at or below the
//             cap (kCapMinDbA .. kCapMaxDbA dB(A)). A feed-forward gain
//             aims the level kCapTargetUnderDb under the cap (1 s detector,
//             kAttackSeconds / kReleaseSeconds), and an energy budget is the
//             hard bound behind it: the output energy of the last 5 s (in
//             kCapSegments segments, so every 5 s window lies inside the
//             last kCapSegments + 1 of them) plus the next chunk's exact
//             energy (the A filter's zero-input response plus its responses
//             to the chunk's linear gain ramp, kChunk samples at most)
//             plus a reserve for ramping the chunk after it to zero must fit
//             the cap's energy. The gain glides linearly within each chunk;
//             only a signal that jumps within one chunk from nothing to far
//             over a full budget forces a step (counted in capSteps). The
//             bound holds for every window that starts after the cap, the
//             sensitivity or the output was last set, at the endpoint volume
//             the host reported (a volume change reaches the guard at the
//             host's next poll). Gain <= 1, so it adds no overs and no latency.
//   off / unknown  with no sensitivity (NaN, the default) nothing is
//             estimated, every reading is HearingMeters::kUnknown and the
//             signal is not touched; with a sensitivity and the cap off (or
//             the cap on and the level under it) the signal is not touched
//             either: bit-identical. A cap switched off, or a sensitivity
//             cleared, while the cap holds the level down glides back up at
//             the release rate first.
//
// Threading: prepare() before the audio runs (or between runs); process()
// on the audio thread; the setters from any thread (relaxed atomics, read
// once per block); readings through meters() from any thread.
#pragma once

#include "flub/common/AudioBlock.h"
#include "flub/common/Realtime.h"
#include "flub/dsp/Biquad.h"
#include "flub/engine/MeterBus.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

namespace flub
{
/** The dose arithmetic on a synthetic clock: exposures at a steady A-weighted
    level for a span of seconds. Pure; any thread. */
class Dosimeter
{
public:
    static constexpr double kReferenceDbA = 80.0;               // WHO / ITU-T H.870
    static constexpr double kReferenceSeconds = 40.0 * 3600.0; // a week's allowance at the reference

    /** The fraction of the weekly allowance `seconds` at `levelDbA` use. */
    static double doseFor (double levelDbA, double seconds) noexcept;
    /** How long `levelDbA` may play before `fraction` of the allowance is used. */
    static double secondsFor (double levelDbA, double fraction) noexcept;

    void add (double levelDbA, double seconds) noexcept;
    void reset() noexcept { dose = 0.0; elapsed = 0.0; }
    double fraction() const noexcept { return dose; }
    double seconds() const noexcept { return elapsed; }
    /** The equal-energy level of everything added (HearingMeters::kUnknown before any time). */
    double equivalentLevelDbA() const noexcept;

private:
    double dose = 0.0, elapsed = 0.0;
};

class HearingGuard
{
public:
    static constexpr double kCapWindowSeconds = 5.0;
    static constexpr int kCapSegments = 500;         // 10 ms segments at 48 kHz
    static constexpr int kChunk = 32;                // the longest linear gain ramp
    static constexpr float kCapMinDbA = 60.0f, kCapMaxDbA = 100.0f, kDefaultCapDbA = 85.0f;
    static constexpr float kCapTargetUnderDb = 1.0f; // where the slow gain aims
    static constexpr float kCapMarginDb = 0.05f;     // the budget's rounding margin
    static constexpr double kDetectorSeconds = 1.0, kAttackSeconds = 0.5, kReleaseSeconds = 4.0;
    static constexpr double kFastSeconds = 0.125;
    /** The planned gain (see above): the share of the sustainable power it
        aims at, its programme detector and its own attack / release. */
    static constexpr double kPlanShare = 0.98;
    static constexpr double kPlanDetectorSeconds = 0.05;
    static constexpr double kPlanAttackSeconds = 0.002, kPlanReleaseSeconds = 0.2;
    static constexpr double kSpendSeconds = 0.5; // no faster than the room left in this long
    static constexpr float kMinSensitivityDbSpl = 60.0f, kMaxSensitivityDbSpl = 150.0f;
    static constexpr float kMutedVolumeDb = -150.0f; // at or below: nothing reaches the ear

    enum class SensitivitySource
    {
        Unknown,
        User,   // the user's own figure or calibration (the host's setting)
        Profile // the matched device profile's (device::splAtFullScale)
    };
    /** The user's figure wins over the profile's; NaN = none. Non-RT helper. */
    static SensitivitySource chooseSensitivity (float userDbSpl, float profileDbSpl, float& chosenDbSpl) noexcept;

    /** A-weighting sections (IEC 61672-1), normalised to 0 dB at 1 kHz. */
    static std::array<BiquadCoeffs, 3> aWeighting (double sampleRate) noexcept;
    /** The analog A-weighting curve in dB (IEC 61672-1 Annex E, 0 dB at 1 kHz). */
    static double aWeightingCurveDb (double freqHz) noexcept;

    /** Non-RT: the rate-dependent design; clears the filters and the
        window. The settings, the dose counted so far (folded into the
        session dose) and the cap's gain are kept. */
    void prepare (double sampleRate);
    /** Clears the filters, the window and the gain (1). Audio thread. */
    void reset() noexcept FLUB_NONBLOCKING;

    /** In place on the first two channels' estimate; the cap's gain on every
        channel of `block`. */
    void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING;

    // ---- Settings (any thread) ----
    /** dB SPL of a 0 dBFS sine at full endpoint volume; NaN (the default) =
        unknown: everything off. Clamped to kMin..kMaxSensitivityDbSpl. */
    void setSensitivityDbSpl (float dbSpl) noexcept;
    float getSensitivityDbSpl() const noexcept { return sensitivity.load (std::memory_order_relaxed); }
    /** The output endpoint's volume (dB, 0 = full; the E32 poll's reading);
        0 dB until the host passes one (the loudest case). -inf / at or below
        kMutedVolumeDb = muted. */
    void setEndpointVolumeDb (float db) noexcept;
    float getEndpointVolumeDb() const noexcept { return endpointVolume.load (std::memory_order_relaxed); }
    /** The listening-level cap, off by default; the level clamped to kCapMin..kCapMaxDbA. */
    void setCap (bool enabled, float levelDbA = kDefaultCapDbA) noexcept;
    bool getCapEnabled() const noexcept { return capEnabled.load (std::memory_order_relaxed); }
    float getCapDbA() const noexcept { return capLevel.load (std::memory_order_relaxed); }
    /** The day's dose so far (fraction of the weekly allowance, persisted by
        the host); HearingMeters::doseToday = this + the session dose since. */
    void setDoseBaseline (double fraction) noexcept;

    /** Non-RT: the settings, the session dose and the cap's gain of an engine
        this one replaces (`previous` may be running: only its atomics are read). */
    void carryFrom (const HearingGuard& previous) noexcept;

    const HearingMeters& meters() const noexcept { return published; }

private:
    using Sections = std::array<BiquadCoeffs, 3>;
    using State = std::array<BiquadState, 3>;
    static double tick (const Sections& c, State& s, double x) noexcept
    {
        return biquadTick (c[2], s[2], biquadTick (c[1], s[1], biquadTick (c[0], s[0], x)));
    }

    void clearWindow() noexcept;
    void plan (double budget) noexcept;
    void publish (bool known) noexcept;
    double sessionDose() const noexcept;

    // Settings (written by any thread).
    std::atomic<float> sensitivity { std::numeric_limits<float>::quiet_NaN() };
    std::atomic<float> endpointVolume { 0.0f };
    std::atomic<bool> capEnabled { false };
    std::atomic<float> capLevel { kDefaultCapDbA };
    std::atomic<double> pendingBaseline { 0.0 };
    std::atomic<uint32_t> baselineSeq { 0 };

    // Audio thread.
    Sections aw {};
    std::array<State, 2> preState {}, postState {};
    double sampleRate = 48000.0, windowSamples = 240000.0;
    int segmentLength = 480, segmentPos = 0, ringPos = 0;
    std::array<std::array<double, kCapSegments>, 2> ring {};
    std::array<double, 2> windowSum {}, segmentEnergy {}, slowPower {}, fastPower {}, sessionEnergy {};
    double sessionSamples = 0.0;
    std::array<double, 2> fastPre {}, chunkPower {}, sustainable {};
    double gain = 1.0, slowGain = 1.0, planGain = 1.0; // gain: the applied one
    double doseCarried = 0.0;       // the session dose before the last prepare() / carryFrom()
    double baseline = 0.0, baselineMark = 0.0;
    uint32_t seenBaselineSeq = 0;
    float lastSensitivity = 0.0f, lastCapDb = 0.0f;
    bool wasKnown = false, lastCapOn = false, needPlan = true;
    uint64_t capSteps = 0;

    HearingMeters published;
};
} // namespace flub
