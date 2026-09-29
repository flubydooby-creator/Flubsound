// Flubsound Pro - relative loudness contour: level-dependent LF / HF
// compensation that follows the playback level (docs/11 E32, relative mode).
//
// The ear loses bass (and some treble) faster than mids as the level falls:
// ISO 226:2023's equal-loudness contours. Music balanced at a reference level
// P phon, played L dB quieter (L < 0), reaches the ear at P + L phon, where a
// tone at f needs Lp (f, P + L) dB SPL to be as loud as the 1 kHz tone, but
// gets Lp (f, P) + L. The contour therefore applies
//
//   G (f) = Lp (f, P + L) - Lp (f, P) - L        (0 at 1 kHz by definition)
//
// e.g. with P = 80 phon: +12.1 dB at 50 Hz, +1.8 dB at 500 Hz, -1.2 dB at
// 3 kHz, +4.4 dB at 12.5 kHz for L = -30 dB. P + L is kept >= 20 phon, the
// lowest contour the standard tabulates at every frequency; the lift is
// capped at maxLiftDb.
//
//   design    G at the standard's 29 one-third-octave frequencies (20 Hz ..
//             12.5 kHz), capped, fitted in dB by four SVF sections with
//             fixed shapes - low shelves at 40 Hz and 250 Hz (Q 0.6), a bell
//             at 3 kHz (Q 0.6) and a high shelf at 10 kHz (Q 0.7) - by least
//             squares on unit-gain basis responses (prepare()) and three
//             Gauss-Newton steps on the exact digital responses. Fit error
//             <= 0.5 dB at 50 Hz and <= 1.1 dB anywhere for L down to -60 dB
//             (48 kHz). Points above 0.45 fs are left out, and so is the
//             high shelf when 10 kHz is above 0.4 fs.
//   headroom  the lift is budgeted like the chain's static boosts (docs/11
//             E11): its programme-weighted maximum (headroom::
//             predictMaxBoostWith, Programme weighting) above allowanceDb
//             comes off the level as a broadband trim, so the lift does not
//             drive the limiter; the allowance is the lift the stage may take
//             out of the headroom (ProcessingChain scales it with the
//             SafetyGovernor, docs/11 E06).
//   glide     every section gain and the trim move towards their targets by
//             at most kSlewDbPerSecond, re-designed every kUpdateSamples and
//             interpolated sample by sample in between (coefficients and
//             trim), so a volume ramp, a reference change or on / off is a
//             glide, never a step. The design runs only when an input changes (at most once
//             per kDesignIntervalMs; the latest input is always designed).
//   off       glides flat, then idles: flat sections are the identity
//             (m0 = 1, m1 = m2 = 0), and once every gain and the trim sit at
//             0 dB the stage does not touch the signal (bit-exact). While on,
//             the filters run even when flat (at the reference level), so
//             their state follows the signal when a lift starts.
//
// Latency 0. Stereo-linked coefficients, one filter state per channel.
#pragma once

#include "flub/common/AudioBlock.h"
#include "flub/common/Realtime.h"
#include "flub/dsp/Processor.h"
#include "flub/dsp/Svf.h"

#include <array>
#include <atomic>

namespace flub
{
/** ISO 226:2023 normal equal-loudness-level contours (Table 1 parameters,
    Formula (1)). The standard's tables are this formula rounded to 0.1 dB;
    it is valid from 20 phon (all frequencies) to 90 phon (to 4 kHz: 80 phon
    above, 100 phon at 20 Hz .. 4 kHz as information). RT-safe, pure. */
namespace iso226
{
inline constexpr int kNumFrequencies = 29;
/** Preferred one-third-octave frequencies, 20 Hz .. 12.5 kHz (Hz). */
extern const std::array<double, kNumFrequencies> kFrequencies;

/** Sound pressure level (dB SPL) of a pure tone at kFrequencies[index] that
    is `phon` loud. Lp (index of 1 kHz, L) = L; at 2.4 phon it is the
    threshold of hearing Tf. */
double splDb (int index, double phon) noexcept FLUB_NONBLOCKING;

/** G (f) of the header comment at kFrequencies[index]: the gain that keeps a
    tone as loud relative to 1 kHz at referencePhon + levelDb as it was at
    referencePhon. */
double relativeGainDb (int index, double referencePhon, double levelDb) noexcept FLUB_NONBLOCKING;
} // namespace iso226

struct LoudnessContourParams
{
    bool enabled = false;
    float referencePhon = 80.0f; // loudness at the reference playback level (contour.reference)
    float levelDb = 0.0f;        // playback level re the reference, <= 0 (contour.level + the endpoint's offset)
    float maxLiftDb = 18.0f;     // cap on the lift at any frequency (contour.maxLift)
    float allowanceDb = 3.0f;    // programme-weighted lift left in before the trim (see the header comment)
};

class LoudnessContour final : public Processor
{
public:
    static constexpr int kNumSections = 4;
    static constexpr float kMinLevelDb = -60.0f, kMinPhon = 20.0f;
    static constexpr float kSlewDbPerSecond = 60.0f;
    static constexpr int kUpdateSamples = 32;
    static constexpr float kDesignIntervalMs = 10.0f;

    struct Section
    {
        FilterType type;
        double hz, q;
    };
    /** The fixed section shapes (see the header comment). */
    static constexpr std::array<Section, kNumSections> kSections { { { FilterType::LowShelf, 40.0, 0.6 },
                                                                     { FilterType::LowShelf, 250.0, 0.6 },
                                                                     { FilterType::Bell, 3000.0, 0.6 },
                                                                     { FilterType::HighShelf, 10000.0, 0.7 } } };

    /** Non-RT: the fit's basis at this rate. The first design after it
        (the first setParams()) applies without a glide, like the chain's
        preamp. */
    void prepare (const ProcessSpec& spec) override;
    /** Filter state cleared; gains and trim jump to their targets. */
    void reset() noexcept FLUB_NONBLOCKING override;
    void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING override;
    const char* name() const noexcept override { return "Loudness contour"; }

    /** Audio thread, at block start. Re-designs when enabled, reference,
        level or cap changed (at most once per kDesignIntervalMs); the
        allowance only moves the trim. */
    void setParams (const LoudnessContourParams& p) noexcept FLUB_NONBLOCKING;

    /** True while enabled, and after that while a gain or the trim is still
        gliding back to 0 dB. process() is a no-op otherwise. */
    bool isRunning() const noexcept { return running; }

    /** The target design (what the stage glides to): section gains (dB), the
        response of the sections at freqHz (dB, without the trim) and the
        trim (dB <= 0). RT-safe. */
    float getTargetGainDb (int section) const noexcept { return targetGain[static_cast<size_t> (section)]; }
    double targetLiftDb (double freqHz) const noexcept FLUB_NONBLOCKING;
    float getTargetTrimDb() const noexcept { return targetTrim; }
    /** The programme-weighted maximum of the target lift (dB, before the allowance). */
    float getPredictedLiftDb() const noexcept { return predictedLiftDb; }
    /** Target sections as SvfCoeffs (only the ones with a gain; returns the
        count written, <= kNumSections), for the chain's static-boost model. RT-safe. */
    int getTargetSections (SvfCoeffs* out) const noexcept FLUB_NONBLOCKING;

    /** What is applied now: the sections' response at freqHz without the
        trim, and the trim. Published once per block; any thread. */
    float getAppliedLiftAt50HzDb() const noexcept { return appliedLift50.load (std::memory_order_relaxed); }
    float getAppliedTrimDb() const noexcept { return appliedTrimDb.load (std::memory_order_relaxed); }

    /** The level actually designed for: clamped to [kMinLevelDb, 0] and to
        referencePhon + level >= kMinPhon. */
    static float effectiveLevelDb (const LoudnessContourParams& p) noexcept;

private:
    void design() noexcept FLUB_NONBLOCKING;
    double responseDb (const std::array<float, kNumSections>& gains, int point) const noexcept;
    void updateCoefficients() noexcept;

    double sampleRate = 48000.0;
    int numChannels = 2;
    // Fit: tan (pi f / fs) per ISO point, the active points and sections, and
    // the pseudo-inverse of the unit-gain basis (sections x points).
    std::array<double, iso226::kNumFrequencies> tanAt {};
    std::array<bool, iso226::kNumFrequencies> pointActive {};
    std::array<bool, kNumSections> sectionActive {};
    std::array<std::array<double, iso226::kNumFrequencies>, kNumSections> pinv {};

    LoudnessContourParams params, designed;
    bool designValid = false, designPending = false, snapPending = false;
    int designHoldoff = 0;
    std::array<float, kNumSections> targetGain {}, gain {};
    float targetTrim = 0.0f, trim = 0.0f, predictedLiftDb = 0.0f;
    int untilUpdate = 0;
    bool running = false;

    std::array<SvfCoeffs, kNumSections> coeffs {}, coeffsFrom {}; // this update's and the previous one's (the glide)
    float trimFrom = 0.0f;
    std::array<std::array<SvfState, kMaxChannels>, kNumSections> state {};
    std::atomic<float> appliedLift50 { 0.0f }, appliedTrimDb { 0.0f };
};
} // namespace flub
