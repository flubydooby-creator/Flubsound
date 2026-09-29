// Flubsound Pro - the personal hearing profile and its per-ear stage
// (docs/11 E33 slice, docs/03 §14.15).
//
// A LISTENING PREFERENCE, not a hearing test, a fitting or a medical device:
// the listener enters, per ear, a broadband gain and gains at the eight
// audiometric frequencies, plus a balance, by hand. Nothing here measures
// hearing or prescribes a gain.
//
//   storage   outside ParameterStore: preset loads, A/B banks, the macros
//             and the automatic profiles never touch it. The host keeps it in
//             its own JSON file (loadPersonalProfile / savePersonalProfile,
//             core/src/io/PersonalProfileIO.cpp) and hands it to every strip's
//             chain (ProcessingChain::setPersonalProfile).
//   targets   per ear e and band b (kBandHz: 250 Hz .. 8 kHz):
//               B_e    = gain_e + balance_e          (the broadband part)
//               T_e,b  = clamp (B_e + band_e,b, kMinTargetDb, kMaxBoostDb)
//             balance > 0 turns the left ear down by it, < 0 the right ear
//             (a balance never boosts). The per-ear difference is capped at
//             kMaxEarDifferenceDb (docs/11 E33: ~12 dB): where |T_R - T_L| is
//             larger, the higher ear is lowered to the cap (never the lower
//             one raised), for B as for every T.
//   design    per ear: the broadband gain B_e and one bell per band at kBandHz
//             (Q from the band spacing, kBandQ) whose gains are solved
//             (Newton on the exact digital response, Svf.h; each bell within
//             +-36 dB) so the response at every band frequency is T_e,b
//             within 0.01 dB; between the bands it ripples (+-2 - 3 dB on a
//             steep shape), and outside 250 Hz .. 8 kHz it returns to B_e (no
//             extrapolated boost).
//             Bands at or above kMaxBandFraction of the sample rate are left
//             out (hands-free rates). The design is a CorrectionCurve
//             (DeviceCorrection.h) with channel masks.
//   headroom  the stage lowers both ears alike by the reservation
//             R = -max (0, programme-weighted maximum boost of the louder ear)
//             (headroom::Weighting::Programme, as the automatic preamp of
//             docs/11 E11 weighs the chain's boosts), so the boosted ear
//             reaches the stereo-linked compressor and the maximizer at about
//             the level it had without the profile: the dynamics react to the
//             programme as before instead of ducking both ears whenever the
//             boosted ear gets loud (docs/08 C1). What the reservation takes
//             from the level, the maximizer's drive, the upward compressor or
//             the volume gives back.
//   placement (docs/11 E33 "measure both"): BeforeCompressor (the default and
//             the only one a host uses) runs the stage after the stereo
//             spatializer and ahead of the Startle Guard, the compressor and
//             the maximizer: the chain's stereo-linked dynamics apply the same
//             gain to both ears, so a source's ILD keeps the static per-ear
//             gain, and the maximizer's true-peak limiter still holds the
//             ceiling. AfterMaximizer runs it after the maximizer with its own
//             true-peak limiter per ear (unlinked, kLimiterLookaheadMs, at the
//             maximizer's ceiling; its latency joins the chain's): kept for the
//             measurement only - at Boost 100 the maximizer's output already
//             sits at the ceiling, so the boosted ear's limiter takes the
//             boost back on loud material and the ILD collapses
//             (tests/test_personal_profile.cpp, docs/03 §14.15).
//   hand-over the host's thread designs the curve; it reaches the audio
//             thread through DeviceCorrection's wait-free ring and crossfades
//             over DeviceCorrection::kCrossfadeMs (click-free). A neutral or
//             disabled profile is the identity: the stage does not touch the
//             signal (bit-identical to a chain that never had one).
//   inverse   at BeforeCompressor the chain's own measures - the governor's
//             drive span, the output PLR, the tonal-balance rule's output and
//             the Smoothness stage's downstream view - read the output with
//             the stage undone (processInverse(): the exact reciprocal
//             bells and gains on a copy), so the listener's ear correction is
//             never taken for distortion or for brightness the chain adds.
#pragma once

#include "flub/common/AudioBlock.h"
#include "flub/common/Realtime.h"
#include "flub/dsp/DeviceCorrection.h"
#include "flub/dsp/TruePeakLimiter.h"
#include "flub/io/Json.h"

#include <array>
#include <string>

namespace flub
{
struct PersonalProfile
{
    static constexpr int kNumBands = 8;
    /** The audiometric frequencies the per-ear EQ is entered at (Hz). */
    static constexpr std::array<double, kNumBands> kBandHz { 250.0, 500.0, 1000.0, 2000.0, 3000.0, 4000.0, 6000.0, 8000.0 };
    /** Bell Q per band: a bandwidth of 1.25 x the mean spacing to the
        neighbouring bands (1.25 octaves up to 1 kHz, 0.99 at 2 kHz, 0.625
        from 3 kHz; 8 kHz as 6 kHz). Wider bells ripple less between the
        bands (a +12 dB plateau from 2 to 8 kHz dips 2.2 dB between them
        instead of 3.5 dB at 1 x); any profile inside the ranges, a +-15 dB
        zigzag included, is still met with bells of at most 28 dB. */
    static constexpr std::array<double, kNumBands> kBandQ { 1.1188, 1.1188, 1.1188, 1.4282, 2.2904, 2.2904, 2.2904, 2.2904 };
    static constexpr float kGainRangeDb = 12.0f;     // broadband gain per ear: +-
    static constexpr float kBalanceRangeDb = 12.0f;  // balance: +-
    static constexpr float kBandRangeDb = 15.0f;     // band gain per ear: +-
    static constexpr float kMaxBoostDb = 15.0f;      // each ear's target at most (docs/11 E33's fitting cap)
    static constexpr float kMinTargetDb = -30.0f;    // each ear's target at least
    static constexpr float kMaxEarDifferenceDb = 12.0f;
    static constexpr double kMaxBandFraction = 0.4;  // bands at or above 0.4 fs are left out

    bool enabled = false;
    std::array<float, 2> gainDb {}; // left, right
    float balanceDb = 0.0f;         // > 0: the left ear down by it; < 0: the right ear down
    std::array<std::array<float, kNumBands>, 2> bandDb {};

    bool operator== (const PersonalProfile&) const = default;

    /** Every value clamped to its range; NaN / Inf -> 0. */
    PersonalProfile sanitised() const noexcept;
    /** B_e (dB) after the ranges and the difference cap; 0 while disabled. */
    float broadbandDb (int ear) const noexcept;
    /** T_e,b (dB) after the ranges and the caps; 0 while disabled. */
    float targetDb (int ear, int band) const noexcept;
    /** Disabled, or every target 0 dB: the stage is the identity. */
    bool isNeutral() const noexcept;
};

/** The per-ear curve of `profile` at `sampleRate` (see the header comment),
    without the reservation. Empty for a neutral profile. Control thread. */
CorrectionCurve designPersonalCurve (const PersonalProfile& profile, double sampleRate);

/** The headroom reservation of a curve (dB <= 0; see the header comment). */
float personalReservationDb (const CorrectionCurve& curve, double sampleRate) noexcept;

/** Where the chain runs the per-ear stage (see the header comment). */
enum class PersonalPlacement : int
{
    BeforeCompressor = 0,
    AfterMaximizer = 1
};

/** The per-ear stage one ProcessingChain runs: the profile's curve with the
    reservation (DeviceCorrection: per-channel sections, crossfaded
    hand-over), its exact inverse for the chain's measures, and at
    AfterMaximizer a true-peak limiter per ear. Zero latency at
    BeforeCompressor. */
class PersonalEarStage
{
public:
    static constexpr float kLimiterLookaheadMs = 1.0f;

    /** Non-RT. Designs the current profile at the rate (no crossfade); the
        placement is structural (it decides the latency). */
    void prepare (const ProcessSpec& spec, PersonalPlacement placement);
    /** Filter and limiter states cleared; a running crossfade ends on its target. */
    void reset() noexcept FLUB_NONBLOCKING;

    /** Control thread: designs `profile` (sanitised) and hands it over; the
        audio thread crossfades to it. False if the hand-over ring was full
        (kept: retryPending(), the next call or prepare() hands it over). */
    bool setProfile (const PersonalProfile& profile);
    /** Control thread: hands the current profile over again if the last
        setProfile() found the ring full; true when nothing is left waiting. */
    bool retryPending();
    /** Non-RT while process() cannot run: takes effect at once. */
    void setProfileNow (const PersonalProfile& profile);
    /** Control thread: the profile last given (sanitised). */
    const PersonalProfile& getProfile() const noexcept { return profile; }
    /** Control thread: the last design's curve and reservation (dB <= 0). */
    const CorrectionCurve& getCurve() const noexcept { return curve; }
    float getReservationDb() const noexcept { return reservationDb; }

    /** Audio thread: the stage on the chain's stereo signal (channel 0 =
        left ear); at AfterMaximizer followed by the per-ear limiters. */
    void process (const AudioBlock& stereo) noexcept FLUB_NONBLOCKING;
    /** Audio thread: the exact inverse of the curve and the reservation on a
        copy the chain's measures read (BeforeCompressor; see the header
        comment). Call it once per process(), on the same samples' copy. */
    void processInverse (const AudioBlock& copy) noexcept FLUB_NONBLOCKING;
    /** Audio thread, at block start: the per-ear limiters' ceiling (dBTP). */
    void setCeilingDb (float ceilingDb) noexcept FLUB_NONBLOCKING;

    PersonalPlacement getPlacement() const noexcept { return placement; }
    int latencySamples() const noexcept;

private:
    DeviceCorrectionSettings settingsFor (const CorrectionCurve& c, float reservation, bool inverse) const noexcept;
    void design();

    PersonalPlacement placement = PersonalPlacement::BeforeCompressor;
    double sampleRate = 48000.0;
    // Control thread
    PersonalProfile profile;
    CorrectionCurve curve;
    float reservationDb = 0.0f;
    // Both hand designs over through their own ring (DeviceCorrection).
    DeviceCorrection forward, inverse;
    std::array<TruePeakLimiter, 2> earLimiter; // AfterMaximizer only
    // Audio thread: processInverse() ran since the last process(); if not,
    // process() drains the inverse's hand-over ring (a chain whose measures
    // are off never runs the inverse, and its ring must not fill up).
    bool inverseRan = false;
};

// ---- The profile file (core/src/io/PersonalProfileIO.cpp) ----------------
// { "format": "flubsound-personal-profile", "version": 1, "enabled": true,
//   "balanceDb": 0, "bandHz": [250, 500, 1000, 2000, 3000, 4000, 6000, 8000],
//   "left":  { "gainDb": 0, "bandsDb": [0, 0, 0, 0, 0, 0, 0, 0] },
//   "right": { "gainDb": 0, "bandsDb": [0, 0, 0, 0, 3, 6, 9, 12] } }
// Values outside their ranges load clamped (PersonalProfile::sanitised); a
// missing key reads 0 / false. Not for the audio thread.
namespace personal
{
    constexpr const char* kFormat = "flubsound-personal-profile";
    constexpr int kVersion = 1;

    json::Value toJson (const PersonalProfile& profile);
    /** False (out untouched) on a wrong format, a newer version, a band list
        that is not kBandHz or a value of the wrong type. */
    bool fromJson (const json::Value& root, PersonalProfile& out, std::string& error);
    /** UTF-8 paths. save() writes a temporary file next to `path` and renames
        it over the old one, so an interrupted save keeps the previous profile. */
    bool load (const std::string& path, PersonalProfile& out, std::string& error);
    bool save (const std::string& path, const PersonalProfile& profile, std::string& error);
} // namespace personal
} // namespace flub
