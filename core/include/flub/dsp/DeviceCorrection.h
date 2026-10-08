// Flubsound Pro - per-output-device correction (docs/11 E15) and the static
// headroom predictor (docs/11 E11).
//
// A correction curve is what Equalizer APO / Peace / AutoEQ call a
// ParametricEQ: up to 16 minimum-phase filters per channel (peaking, shelves,
// cuts, band-pass, notch, all-pass; RBJ cookbook responses, which the TPT SVF
// of Svf.h implements exactly) plus a per-channel gain (the file's "Preamp").
// Filters carry a channel mask, so a unit's left / right imbalance can be
// corrected (APO's "Channel: L" / "Channel: R").
//
// DeviceCorrection runs one curve on the stereo master sum. It is a separate
// stage owned by MixEngine (between the strip sum and the master limiter),
// keyed by the host to the output endpoint, and never part of a strip's
// ProcessingChain, a preset, an A/B bank or an automatic-profile restore.
//
//   L --preamp--> section 0 --> ... --> section n-1 --> L'
//   R --preamp--> section 0 --> ... --> section m-1 --> R'
//
// Headroom (E11): the curve's static maximum boost is predicted on a
// 1/12-octave grid from 20 Hz to 20 kHz, refined at every local maximum
// (golden-section search on log frequency, so a narrow peak between grid
// points is found), on the exact digital response at the running sample
// rate. The automatic preamp is -max(0, max boost - allowance), the same for
// both channels (it keeps their balance). A sine at any frequency therefore
// leaves the correction no louder than it entered (allowance 0, the default).
//
// Threading: prepare() and the setters are control-thread only (one thread;
// they design the filters). A new curve reaches the audio thread through a
// wait-free SPSC ring; process() then crossfades from the running filters to
// the new ones over kCrossfadeMs (equal-gain raised cosine: both paths carry
// the same programme, so equal gain never exceeds the louder of the two).
// A curve that arrives during a crossfade waits for it to end; only the
// newest waiting one is kept. Zero latency. process() costs nothing while
// the correction is off (flat and unity) and no crossfade runs.
#pragma once

#include "Processor.h"
#include "Svf.h"
#include "flub/common/SpscRing.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace flub
{
enum class CorrectionFilterType : uint8_t
{
    Peak = 0,  // APO PK / PEQ
    LowShelf,  // APO LSC / LS with Q (centre frequency = half the gain)
    HighShelf, // APO HSC / HS with Q
    LowPass,   // APO LP / LPQ
    HighPass,  // APO HP / HPQ
    BandPass,  // APO BP
    Notch,     // APO NO
    AllPass    // APO AP
};

struct CorrectionFilter
{
    static constexpr uint8_t kLeft = 1, kRight = 2, kBoth = kLeft | kRight;

    CorrectionFilterType type = CorrectionFilterType::Peak;
    float frequency = 1000.0f; // Hz
    float gainDb = 0.0f;       // Peak / shelves only
    float q = 0.7071f;         // RBJ Q (shelves: the cookbook's shelf Q)
    uint8_t channels = kBoth;  // bit 0 left, bit 1 right

    bool operator== (const CorrectionFilter&) const = default;
};

/** A complete correction for one output endpoint (trivially copyable). */
struct CorrectionCurve
{
    static constexpr int kMaxFiltersPerChannel = 16;
    static constexpr int kMaxFilters = 2 * kMaxFiltersPerChannel;

    std::array<CorrectionFilter, kMaxFilters> filters {};
    int numFilters = 0;
    /** Per-channel gain in dB (left, right): the file's Preamp lines. */
    std::array<float, 2> gainDb {};

    bool operator== (const CorrectionCurve&) const = default;

    /** Filters that act on channel 0 (left) / 1 (right). */
    int countFor (int channel) const noexcept;
    /** Appends a filter; false (curve unchanged) when it would put more than
        kMaxFiltersPerChannel filters on a channel or acts on no channel. */
    bool add (const CorrectionFilter& filter) noexcept;
    bool isEmpty() const noexcept { return numFilters == 0 && gainDb[0] == 0.0f && gainDb[1] == 0.0f; }

    /** The SVF design of one filter (Svf.h; the RBJ cookbook response). */
    static SvfCoeffs design (const CorrectionFilter& filter, double sampleRate) noexcept;
    /** Exact magnitude (dB) of channel 0 / 1 at freqHz: gainDb[channel] plus
        every filter on it, as the digital filters at sampleRate implement
        it. Any thread. */
    double responseDb (int channel, double freqHz, double sampleRate) const noexcept;
};

/** docs/11 E11: static maximum-boost prediction and the automatic preamp. */
namespace headroom
{
    /** Flat: every frequency may carry full-scale programme (the guarantee a
        correction needs). Programme: boosts are weighed against a typical
        long-term programme spectrum (programmeEnvelopeDb), for a chain-wide
        preamp that should not give away level for boosts where programme
        rarely has energy. */
    enum class Weighting
    {
        Flat,
        Programme
    };

    /** Typical long-term programme level per 1/12 octave relative to its
        maximum (dB, <= 0): -6 dB/oct below 40 Hz, 0 from 40 Hz to 1 kHz,
        -3 dB/oct above (-9 dB at 8 kHz). A heuristic envelope, not a
        measurement of any particular programme. */
    double programmeEnvelopeDb (double freqHz) noexcept;

    struct Prediction
    {
        double maxBoostDb = 0.0; // maximum of response (+ envelope) over the range; may be negative
        double atHz = 1000.0;    // where it occurs
    };

    /** Grid (kPointsPerOctave per octave from loHz to hiHz, both ends
        included) plus a golden-section refinement at every local maximum.
        responseDb is called ~50 times per octave plus ~40 per local
        maximum. Control thread. */
    static constexpr int kPointsPerOctave = 12;
    Prediction predictMaxBoost (const std::function<double (double)>& responseDb, Weighting weighting = Weighting::Flat,
                                double loHz = 20.0, double hiHz = 20000.0);

    /** -max(0, maxBoostDb - allowanceDb): <= 0 dB. */
    float preampDb (const Prediction& prediction, double allowanceDb = 0.0) noexcept;

    /** predictMaxBoost's grid and refinement (the same points, so the same
        result) without allocating and without std::function: for the audio
        thread (ProcessingChain's automatic preamp). responseDb is any
        callable double (double freqHz) that is itself real-time safe. */
    template <typename Response>
    Prediction predictMaxBoostWith (const Response& responseDb, Weighting weighting = Weighting::Flat, double loHz = 20.0,
                                    double hiHz = 20000.0) noexcept
    {
        loHz = std::max (loHz, 1.0);
        hiHz = std::max (hiHz, loHz);
        const auto eval = [&] (double log2Hz) {
            const double f = std::exp2 (log2Hz);
            const double r = responseDb (f);
            return (std::isfinite (r) ? r : 0.0) + (weighting == Weighting::Programme ? programmeEnvelopeDb (f) : 0.0);
        };
        // Golden-section search for the maximum of a unimodal eval on [a, b].
        const auto goldenMax = [&eval] (double a, double b) {
            constexpr double kInvPhi = 0.6180339887498949;
            constexpr int kIterations = 40;
            double c = b - kInvPhi * (b - a), d = a + kInvPhi * (b - a);
            double fc = eval (c), fd = eval (d);
            for (int i = 0; i < kIterations; ++i)
            {
                if (fc >= fd)
                {
                    b = d;
                    d = c;
                    fd = fc;
                    c = b - kInvPhi * (b - a);
                    fc = eval (c);
                }
                else
                {
                    a = c;
                    c = d;
                    fc = fd;
                    d = a + kInvPhi * (b - a);
                    fd = eval (d);
                }
            }
            return fc >= fd ? std::pair { c, fc } : std::pair { d, fd }; // (log2 Hz, dB)
        };

        const double x0 = std::log2 (loHz), span = std::log2 (hiHz) - x0;
        const int steps = std::max (1, static_cast<int> (std::ceil (span * kPointsPerOctave - 1.0e-9)));
        const auto xAt = [x0, span, steps] (int i) { return x0 + span * static_cast<double> (i) / static_cast<double> (steps); };

        // A sliding window over the grid: point i is judged once point i + 1
        // is known, in the same order as predictMaxBoost.
        double yPrev = eval (xAt (0)), yCur = yPrev;
        Prediction best { yCur, std::exp2 (xAt (0)) };
        for (int i = 0; i <= steps; ++i)
        {
            const double yNext = i < steps ? eval (xAt (i + 1)) : yCur;
            if (yCur > best.maxBoostDb)
                best = { yCur, std::exp2 (xAt (i)) };
            if (! (yCur < yPrev || yCur < yNext || (yCur == yPrev && yCur == yNext)))
            {
                const auto [x, y] = goldenMax (xAt (i > 0 ? i - 1 : i), xAt (std::min (i + 1, steps)));
                if (y > best.maxBoostDb)
                    best = { y, std::exp2 (x) };
            }
            yPrev = yCur;
            yCur = yNext;
        }
        return best;
    }
} // namespace headroom

struct DeviceCorrectionSettings
{
    CorrectionCurve curve;
    bool enabled = true;  // false: the correction is off (flat, unity gain)
    bool compare = false; // momentary A/B: flat filters, the automatic preamp kept (level-fair)
    float allowanceDb = 0.0f; // headroom the automatic preamp may leave out (E11)

    bool operator== (const DeviceCorrectionSettings&) const = default;
};

class DeviceCorrection final : public Processor
{
public:
    static constexpr float kCrossfadeMs = 20.0f;
    static constexpr int kMaxSections = CorrectionCurve::kMaxFiltersPerChannel;

    DeviceCorrection();

    /** Non-RT. Designs the current settings at the new rate; no crossfade
        (the next process() starts on them). Channels 0 and 1 are corrected;
        any others pass untouched. */
    void prepare (const ProcessSpec& spec) override;
    /** Clears the filter state and finishes a running crossfade on its target. */
    void reset() noexcept FLUB_NONBLOCKING override;
    void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING override;
    const char* name() const noexcept override { return "Device correction"; }

    /** Control thread: designs `settings` and hands them to the audio thread,
        which crossfades to them over kCrossfadeMs. Returns false (and keeps
        them as the settings to apply) if the hand-off ring was full, i.e.
        the audio thread has not run for several changes; retryPending(),
        the next call or prepare() hands them over. */
    bool setSettings (const DeviceCorrectionSettings& settings);
    /** Control thread: hands the current settings over again if the last
        setSettings() found the ring full; true when nothing is left waiting. */
    bool retryPending();
    /** Non-RT, only while process() cannot run (an engine that is not yet
        published): takes effect at once, no crossfade. */
    void setSettingsNow (const DeviceCorrectionSettings& settings);
    /** Control thread: the settings last given (not necessarily heard yet). */
    const DeviceCorrectionSettings& getSettings() const noexcept { return settings; }
    /** Control thread: the automatic preamp of the last design (dB, <= 0;
        0 while off) and the prediction it came from. */
    float getPreampDb() const noexcept { return preampDb; }
    const headroom::Prediction& getPrediction() const noexcept { return prediction; }

    /** AUDIO THREAD (or while process() cannot run): the largest
        steady-state gain (linear) of the curve now running, after its
        automatic preamp: the larger of the running and the incoming one
        during a crossfade (and of a design waiting for it); 1 while off or
        flat. A sine at any frequency leaves the stage at most this much
        louder; a transient's sample peak can still grow through a curve's
        phase. MixEngine lowers docs/11 E22's room by it. */
    float getMaxGain() const noexcept FLUB_NONBLOCKING;

    /** Designs handed to the audio thread that it has finished crossfading
        to (any thread; for tests and "applied" indicators). */
    uint32_t getCompletedTransitions() const noexcept { return completed.load (std::memory_order_acquire); }

private:
    struct Design
    {
        std::array<std::array<SvfCoeffs, kMaxSections>, 2> sections {};
        std::array<int, 2> numSections {};
        std::array<float, 2> gain { 1.0f, 1.0f };
        bool identity = true; // no sections, unity gain: nothing to do
        float maxGain = 1.0f; // getMaxGain(): the prediction's maximum after the preamp, linear
    };
    using State = std::array<std::array<SvfState, kMaxSections>, 2>;

    Design makeDesign (const DeviceCorrectionSettings& s);
    static void runPath (const Design& design, State& state, int channel, float* data, int n) noexcept;
    static void cleanStates (State& state) noexcept;
    void finishTransition() noexcept;

    // Control thread
    DeviceCorrectionSettings settings;
    float preampDb = 0.0f;
    headroom::Prediction prediction;
    double sampleRate = 48000.0;
    bool unsent = false; // settings not yet handed over (ring full)

    // Audio thread
    Design current, incoming, waiting;
    State currentState {}, incomingState {};
    bool fading = false, hasWaiting = false;
    int fadePos = 0;
    std::vector<float> fadeIn; // raised cosine 0 -> 1 over kCrossfadeMs
    AudioBuffer scratch;       // the incoming path during a crossfade
    int maxBlock = 512;

    SpscRing<Design> pending;
    std::atomic<uint32_t> completed { 0 };
};
} // namespace flub
