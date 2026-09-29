// Flubsound Pro - the Smoothness stage: a post-enhancement, source-relative de-esser (docs/11 E07).
//
// Boost and Clarity raise sibilance: the presence bell, the air exciter and
// shelf, the transient shaper's attack and the saturator's harmonics all add
// more to an "s" than to the vowel around it (sibilance over voice +2.7 dB at
// Music Boost 100 + Clarity 100, docs/11 E07). This stage sits after them
// (ProcessingChain slot SSmooth, after the saturator and before the
// compressor and the maximizer's clipper) and takes back what they added to
// the sibilant band, never what the programme brought with it:
//
//   Two signals, both stereo: the stage's input x (after the enhancement)
//   and a reference r, the chain's signal before the enhancement (the
//   dynamic EQ's input, after the user's parametric EQ; the dynamic EQ's
//   mode bands count as enhancement), delayed by the slots in between
//   (setReferenceDelay) so the two line up.
//   Per signal, two bands: the sibilant band (a unity-peak SVF band-pass at
//   7 kHz, Q 0.8: -3 dB at about 4.5 and 11 kHz) and the body (the voice,
//   150 Hz - 2 kHz: 2nd-order Butterworth high- and low-pass; steep enough
//   that the presence lift at 3 kHz does not count as body), their squares
//   summed over the channels and averaged over kDetectorMs (one-poles): P_h,
//   P_b (input) and R_h, R_b (reference). Every kControlInterval samples of
//   absolute stream time:
//     ratio_x = 10 log10 (P_h / P_b), ratio_r = 10 log10 (R_h / R_b)   (dB)
//     excess  = ratio_x - max (ratio_r, threshold)
//     cut     = min (maxCut, amount x max (0, excess))                (dB >= 0)
//   so only moments where the sibilant band is at least `threshold` over
//   the body (an "s", a hi-hat; not dense programme: pink noise reads
//   -2.8 dB, a spectrum 3 dB / octave steeper -13.6 dB, so a bright master
//   keeps its lift up to there) are touched, and those only down to
//   the source's own balance: an "s" the enhancement made 3 dB hotter than
//   its vowel loses those 3 dB; a natural "s" is not de-essed. Level
//   relative on both sides, so it acts the same at any programme level.
//   The reference below -100 dB (silence, or the first milliseconds after
//   a reset while the delay fills) gives no cut.
//   The cut is smoothed in dB (GainSmoother: kAttackMs deepening, kReleaseMs
//   recovering) and applied as a gain G on the sibilant band of the input,
//     y = x + (G - 1) bp (x),
//   the same band-pass the detector reads (feed-forward). With a fixed
//   band-pass the response 1 + (G - 1) BP (jw) never exceeds unity (no bump
//   at the skirts) and G glides linearly across each control interval, so
//   the gain moves without zipper noise and no filter is ever re-designed.
//   The bell is narrower than the band an "s" fills (5 - 10 kHz), so G is
//   sized for the band: prepare() tabulates, for band cuts of 0 .. kMaxCutDb
//   in kTableStepDb steps, the G whose response lowers the mean power of
//   flat 5 - 10 kHz content by that much (bisection on the exact digital
//   response; G >= kMinGainDb, which reaches about 10 dB, hence kMaxCutDb);
//   the control tick interpolates it.
//   Linked over the channels: the image does not move.
//   Music: threshold 0 dB, maxCut 9 dB. Gaming gets a light guard
//   (docs/11 E07, risk P4: a gunshot or a step keeps its edge): threshold
//   +3 dB, maxCut 6 dB. Below kMinSampleRate (hands-free links) the band has
//   no room under Nyquist and the stage does nothing.
//   amount (smooth.amount, 0..1) glides over 20 ms; at 0 with the cut back
//   at 0 dB the output is the input, bit for bit (the chain also bypasses
//   the slot then).
//
// Zero latency, RT-safe: process() and the setters neither allocate nor
// lock; prepare() allocates the reference delay.
#pragma once

#include "EnvelopeFollower.h"
#include "Processor.h"
#include "Svf.h"
#include "flub/common/DelayLine.h"
#include "flub/common/SmoothedValue.h"

#include <array>

namespace flub
{
struct SmoothnessParams
{
    float amount = 0.0f; // 0..1 (smooth.amount)
    bool gaming = false; // light guard: higher threshold, smaller range
};

class SmoothnessGuard final : public Processor
{
public:
    static constexpr double kBandHz = 7000.0, kBandQ = 0.8;   // sibilant band (detector and cut)
    static constexpr double kBodyLowHz = 150.0, kBodyHighHz = 2000.0; // body band (detector)
    static constexpr float kDetectorMs = 5.0f;
    static constexpr float kAttackMs = 1.0f, kReleaseMs = 60.0f;
    static constexpr float kThresholdDb = 0.0f, kMaxCutDb = 9.0f;              // Music
    static constexpr float kGamingThresholdDb = 3.0f, kGamingMaxCutDb = 6.0f;  // Gaming (light)
    static constexpr double kMinSampleRate = 20000.0;
    static constexpr float kTableStepDb = 0.5f, kMinGainDb = -30.0f;
    static constexpr int kTableSize = 19; // 0 .. kMaxCutDb
    static_assert (static_cast<int> (kMaxCutDb / kTableStepDb) + 1 == kTableSize);
    static constexpr int kControlInterval = 16;

    /** Non-RT, before prepare(): the delay that aligns the reference with
        the stage's input (the latency of the slots in between). */
    void setReferenceDelay (int samples) noexcept { referenceDelay = samples; }

    void prepare (const ProcessSpec& spec) override;
    void reset() noexcept FLUB_NONBLOCKING override;
    void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING override;
    const char* name() const noexcept override { return "Smoothness"; }

    void setParams (const SmoothnessParams& p) noexcept FLUB_NONBLOCKING;
    /** The reference for the next process() call (same length, not yet
        delayed); the chain sets it right before the slot runs. A block
        processed without one is measured against silence (no cut). */
    void setReference (const AudioBlock& ref) noexcept FLUB_NONBLOCKING { reference = ref; hasReference = true; }

    /** Deepest band cut applied during the last process() (dB <= 0: how
        far the 5 - 10 kHz band was lowered). */
    float getCutDb() const noexcept { return blockCutDb; }
    /** The G (linear, at the band-pass centre) that lowers flat 5 - 10 kHz
        content by bandCutDb (>= 0; the table above). RT-safe. */
    float gainForBandCut (float bandCutDb) const noexcept FLUB_NONBLOCKING;

private:
    void controlTick() noexcept FLUB_NONBLOCKING;

    double sr = 48000.0;
    int referenceDelay = 0;
    bool enabledForRate = true;
    SmoothnessParams params;
    SvfCoeffs band, bodyHp, bodyLp;
    float bandK = 1.0f; // unity-peak band-pass: k v1
    std::array<SvfState, kMaxChannels> xBand {}, rBand {};
    std::array<std::array<SvfState, 2>, kMaxChannels> xBody {}, rBody {}; // [channel][hp, lp]
    DelayLine refDelay;
    AudioBlock reference;
    bool hasReference = false;
    double powerCoeff = 0.0; // 1 - one-pole coefficient of the detectors
    double pBand = 0.0, pBody = 0.0, rBandPow = 0.0, rBodyPow = 0.0;
    OnePoleSmoother amount;    // control rate
    GainSmoother cut;          // dB, control rate
    std::array<float, kTableSize> gainTable {};           // G per band cut (prepare)
    float gainStart = 1.0f, gainStep = 0.0f, gain = 1.0f; // per-sample glide of G across the interval
    int controlCountdown = kControlInterval, controlPhase = 0;
    float blockCutDb = 0.0f, cutDb = 0.0f; // deepest in the last block; the smoothed band cut now
};
} // namespace flub
