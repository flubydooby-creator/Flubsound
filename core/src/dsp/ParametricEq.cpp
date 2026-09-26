// Flubsound Pro - fully parametric EQ.
//
// Signal flow (bands run in series, band 0 first, then the output gain):
//
//   y = x + mix * (H(x) - x)        H = cascade of 1..4 TPT SVF sections
//
// mix is the band's wet amount: exactly 1 in steady state, ramped linearly
// over ~5 ms (a whole number of control periods) for discrete changes.
//
// Control rate: every kControlInterval samples, counted in absolute stream
// time (the counter survives across process() calls), each band that is
// "busy" advances its one-pole smoothers (log2 frequency, gain dB, log2 Q),
// redesigns its coefficients and steps its crossfade state machine. Across
// the following control period the six SVF coefficients are interpolated
// linearly per sample from the previous design to the new one, so a sweep is
// a chain of short ramps instead of 16-sample steps (no zipper noise). When no
// band is busy the ticks would be no-ops, so the whole remaining block is
// processed as one segment; the output is sample-identical either way, which
// makes the EQ independent of the host block size. A smoother that stalls
// at float resolution short of its target lands on it (stepSmoother()), so
// every glide ends and the band returns to the exact static design.
//
// Discrete changes: while a swap is pending the fading-out band keeps its
// current parameter values; the new ones arrive with the new topology under
// mix 0. A band that is currently skipped (silent, or a 0 dB identity) swaps
// at once and only fades in.
//
// CPU: a band costs nothing while it is disabled and faded out, or while it
// is a bell / shelf at exactly 0 dB that is not gliding (an exact identity:
// m0 = 1, m1 = m2 = 0). When such a skipped 0 dB band starts to glide it
// resumes from that identity: its low-pass integrator is primed with the
// input (DC equilibrium) and its output mix is ramped from identity to the
// new design across one control period.
//
// State hygiene: at the end of every segment integrator values below
// -300 dBFS are flushed to zero (no subnormal crawl without FTZ) and a
// non-finite state is reset, so a NaN / Inf input cannot latch.
#include "flub/dsp/ParametricEq.h"

#include <algorithm>
#include <cmath>

namespace flub
{
namespace
{
constexpr float kMinFreq = 20.0f, kMaxFreq = 20000.0f;
constexpr float kMinGainDb = -24.0f, kMaxGainDb = 24.0f;
constexpr float kMinQ = 0.1f, kMaxQ = 18.0f;
constexpr float kMinOutputDb = -24.0f, kMaxOutputDb = 24.0f;

constexpr float kSmoothingMs = 20.0f;      // one-pole time constant: frequency, gain, Q
constexpr float kFadeMs = 5.0f;            // discrete-change crossfade (each direction)
constexpr float kOutputGainRampMs = 20.0f; // linear output gain ramp

// Filter state below -300 dB re full scale is flushed to zero at the end of a
// segment. Far below float resolution of any audible signal, but it stops a
// slowly decaying low-frequency section from crawling through subnormals when
// the host did not enable FTZ.
constexpr float kStateFloor = 1.0e-15f;

/** NaN -> fallback; everything else (including +-inf) clamps into range. */
float clampOr (float v, float lo, float hi, float fallback) noexcept
{
    return std::isnan (v) ? fallback : std::clamp (v, lo, hi);
}

/** 12 dB/oct per 2nd-order section; other values round to the nearest slope. */
int sectionsForSlope (int slopeDbPerOct) noexcept
{
    return (std::clamp (slopeDbPerOct, 12, 48) + 6) / 12;
}

bool isCut (EqBandType t) noexcept { return t == EqBandType::LowCut || t == EqBandType::HighCut; }

bool isGainType (EqBandType t) noexcept
{
    return t == EqBandType::Bell || t == EqBandType::LowShelf || t == EqBandType::HighShelf;
}

EqBandParams sanitise (const EqBandParams& p) noexcept
{
    const EqBandParams defaults;
    EqBandParams s = p;
    if (static_cast<int> (p.type) > static_cast<int> (EqBandType::BandPass))
        s.type = EqBandType::BandPass;
    s.frequency = clampOr (p.frequency, kMinFreq, kMaxFreq, defaults.frequency);
    s.gainDb = clampOr (p.gainDb, kMinGainDb, kMaxGainDb, defaults.gainDb);
    s.q = clampOr (p.q, kMinQ, kMaxQ, defaults.q);
    s.slopeDbPerOct = 12 * sectionsForSlope (p.slopeDbPerOct);
    return s;
}

/** Designs the SVF sections of one band. Shared by the running filter and
    responseDb(), so the GUI curve is exactly the response that is heard.
    Q is ignored by the (Butterworth) cuts; gain only affects Bell / shelves. */
int designBand (EqBandType type, int numSections, double freq, double q, double gainDb, double sampleRate, SvfCoeffs* out) noexcept
{
    switch (type)
    {
        case EqBandType::Bell:      out[0] = SvfCoeffs::make (FilterType::Bell, freq, q, gainDb, sampleRate); return 1;
        case EqBandType::LowShelf:  out[0] = SvfCoeffs::make (FilterType::LowShelf, freq, q, gainDb, sampleRate); return 1;
        case EqBandType::HighShelf: out[0] = SvfCoeffs::make (FilterType::HighShelf, freq, q, gainDb, sampleRate); return 1;
        case EqBandType::Notch:     out[0] = SvfCoeffs::make (FilterType::Notch, freq, q, 0.0, sampleRate); return 1;
        case EqBandType::BandPass:  out[0] = SvfCoeffs::make (FilterType::BandPass, freq, q, 0.0, sampleRate); return 1;
        case EqBandType::LowCut:
        case EqBandType::HighCut:
            break;
    }

    // Order-2N Butterworth = N 2nd-order sections at the same cutoff with the
    // pole-pair Qs of butterworthQ(): maximally flat, exactly -3 dB at fc.
    const FilterType ft = type == EqBandType::LowCut ? FilterType::HighPass : FilterType::LowPass;
    for (int s = 0; s < numSections; ++s)
        out[s] = SvfCoeffs::make (ft, freq, butterworthQ (numSections, s), 0.0, sampleRate);
    return numSections;
}

/** Integrator value to keep after a segment: tiny values are flushed (see
    kStateFloor), and a non-finite one (a NaN / Inf that reached the input)
    restarts the section from rest. Without that, one bad input sample would
    latch in the recursive state and silence the band until the next reset();
    with it the damage ends with the segment. Costs one test per state per
    segment, not per sample. */
float cleanState (float v) noexcept
{
    return (std::abs (v) < kStateFloor || ! std::isfinite (v)) ? 0.0f : v;
}

void storeState (SvfState& state, const SvfState& s) noexcept
{
    state.ic1 = cleanState (s.ic1);
    state.ic2 = cleanState (s.ic2);
}

/** One control-rate step of a parameter smoother. OnePoleSmoother snaps to
    its target only within 1e-6 (1 + |target|), which is finer than float lets
    a slow one-pole get: once coeff * (cur - target) rounds back to cur the
    recursion stalls, tens of ulps short of any target with |target| >~ 1
    (log2 Hz is 4.3 .. 14.3; most gains are several dB). The band would stay
    "busy" forever: coefficients redesigned every control period, the ramped
    path always in use and the running filter never equal to the design
    responseDb() shows. So a step that no longer moves the value lands on
    the target. The remaining distance is at most ~0.5 ulp / (1 - coeff),
    i.e. < 3e-4 dB or octaves even at 192 kHz, and the coefficient ramp
    spreads that final step over one control period. */
void stepSmoother (OnePoleSmoother& s) noexcept
{
    if (! s.isSmoothing())
        return;
    const float before = s.getCurrent();
    if (s.next() == before)
        s.setImmediate (s.getTarget());
}

/** One SVF section in place. Local copies keep coefficients and state in
    registers (the output pointer could otherwise alias them). */
void runSection (const SvfCoeffs& coeffs, SvfState& state, float* d, int n) noexcept
{
    const SvfCoeffs c = coeffs;
    SvfState s = state;
    for (int i = 0; i < n; ++i)
        d[i] = svfTick (c, s, d[i]);
    storeState (state, s);
}

/** One SVF section whose coefficients move linearly from `from` (end of the
    previous control period) to `to` over kControlInterval samples; rampPos is
    the number of ramp samples already done. The TPT structure keeps its state
    meaningful under coefficient changes, so the ramp is all that is needed
    for a smooth sweep. */
void runSectionRamped (const SvfCoeffs& from, const SvfCoeffs& to, SvfState& state, float* d, int n, int rampPos) noexcept
{
    constexpr int len = ParametricEq::kControlInterval;
    constexpr float inv = 1.0f / static_cast<float> (len);
    const float da1 = (to.a1 - from.a1) * inv, da2 = (to.a2 - from.a2) * inv, da3 = (to.a3 - from.a3) * inv;
    const float dm0 = (to.m0 - from.m0) * inv, dm1 = (to.m1 - from.m1) * inv, dm2 = (to.m2 - from.m2) * inv;
    SvfCoeffs c = from;
    SvfState s = state;
    for (int i = 0; i < n; ++i)
    {
        const float t = static_cast<float> (std::min (rampPos + i + 1, len));
        c.a1 = from.a1 + t * da1;
        c.a2 = from.a2 + t * da2;
        c.a3 = from.a3 + t * da3;
        c.m0 = from.m0 + t * dm0;
        c.m1 = from.m1 + t * dm1;
        c.m2 = from.m2 + t * dm2;
        d[i] = svfTick (c, s, d[i]);
    }
    storeState (state, s);
}
} // namespace

//==============================================================================
void ParametricEq::prepare (const ProcessSpec& newSpec)
{
    spec = newSpec;
    spec.numChannels = std::clamp (spec.numChannels, 1, kMaxChannels);
    if (! (spec.sampleRate > 0.0) || ! std::isfinite (spec.sampleRate))
        spec.sampleRate = 48000.0;

    // Crossfade length is a whole number of control periods, so a fade that
    // starts on a tick always lands exactly on 0 or 1 on a later tick.
    const long periods = std::lround (static_cast<double> (kFadeMs) * 0.001 * spec.sampleRate / kControlInterval);
    fadeSamples = std::max (1, static_cast<int> (periods)) * kControlInterval;
    invFadeSamples = 1.0f / static_cast<float> (fadeSamples);

    // The smoothers are stepped once per control period, so their coefficient
    // is computed for the control rate (== the per-sample one-pole ^ 16).
    const double controlRate = spec.sampleRate / kControlInterval;
    for (auto& band : bandDsp)
    {
        band.logFreq.reset (controlRate, kSmoothingMs, 0.0f);
        band.gainDb.reset (controlRate, kSmoothingMs, 0.0f);
        band.logQ.reset (controlRate, kSmoothingMs, 0.0f);
    }

    outputGain.reset (spec.sampleRate, kOutputGainRampMs, dbToGain (outputGainDb));
    reset();
}

void ParametricEq::reset() noexcept
{
    for (size_t b = 0; b < bandDsp.size(); ++b)
        snapBand (bandDsp[b], targets[b]);
    outputGain.setImmediate (outputGain.getTarget());
    samplesToTick = 0;
    anyBusy = false;
}

//==============================================================================
void ParametricEq::setBand (int index, const EqBandParams& params) noexcept
{
    if (index < 0 || index >= kMaxBands)
        return;

    const EqBandParams p = sanitise (params);
    auto& target = targets[static_cast<size_t> (index)];
    if (p == target)
        return; // the chain pushes every parameter every block: unchanged is free

    target = p;
    auto& band = bandDsp[static_cast<size_t> (index)];
    band.logFreq.setTarget (std::log2 (p.frequency));
    band.gainDb.setTarget (p.gainDb);
    band.logQ.setTarget (std::log2 (p.q));
    // Everything else (gliding, crossfading, swapping) happens on the control
    // ticks, which sit at fixed positions in the stream.
    band.busy = true;
    anyBusy = true;
}

const EqBandParams& ParametricEq::getBand (int index) const noexcept
{
    return targets[static_cast<size_t> (std::clamp (index, 0, kMaxBands - 1))];
}

void ParametricEq::setOutputGainDb (float db) noexcept
{
    outputGainDb = clampOr (db, kMinOutputDb, kMaxOutputDb, 0.0f);
    outputGain.setTarget (dbToGain (outputGainDb));
}

//==============================================================================
double ParametricEq::responseDb (const EqBandParams* bandList, int numBands, double freqHz, double sampleRate) noexcept
{
    if (bandList == nullptr || numBands <= 0 || ! (sampleRate > 0.0) || ! std::isfinite (sampleRate) || std::isnan (freqHz))
        return 0.0;

    double db = 0.0;
    std::array<SvfCoeffs, kMaxSections> sections {};
    for (int b = 0; b < numBands; ++b)
    {
        const EqBandParams p = sanitise (bandList[b]);
        if (! p.enabled)
            continue; // a disabled band is an exact identity
        const int n = designBand (p.type, isCut (p.type) ? sectionsForSlope (p.slopeDbPerOct) : 1,
                                  p.frequency, p.q, p.gainDb, sampleRate, sections.data());
        for (int s = 0; s < n; ++s)
            db += sections[static_cast<size_t> (s)].magnitudeDb (freqHz, sampleRate);
    }
    return db;
}

//==============================================================================
ParametricEq::Topology ParametricEq::topologyOf (const EqBandParams& params) noexcept
{
    return { params.enabled, params.type, isCut (params.type) ? sectionsForSlope (params.slopeDbPerOct) : 1 };
}

/** Bell / shelves at exactly 0 dB (and not moving) are an exact identity:
    m1 = m2 = 0 and m0 = 1, so they are skipped entirely. */
bool ParametricEq::isTransparent (const Band& band) noexcept
{
    return isGainType (band.running.type) && band.gainDb.getCurrent() == 0.0f && ! band.gainDb.isSmoothing();
}

void ParametricEq::snapBand (Band& band, const EqBandParams& target) noexcept
{
    band.running = topologyOf (target);
    band.logFreq.setImmediate (std::log2 (target.frequency));
    band.gainDb.setImmediate (target.gainDb);
    band.logQ.setImmediate (std::log2 (target.q));
    band.fadePos = band.running.enabled ? fadeSamples : 0;
    band.fadeDir = 0;
    for (auto& ch : band.state)
        for (auto& s : ch)
            s.reset();
    band.stateValid = true;
    band.primeOnStart = false;
    band.ramping = false;
    band.rampPos = 0;
    band.audible = band.running.enabled && ! isTransparent (band);
    band.coeffsDirty = true;
    if (band.audible)
        refreshCoefficients (band, target, false);
    band.busy = false;
}

void ParametricEq::swapTopology (Band& band, const Topology& wanted) noexcept
{
    // Only called while the band is inaudible (wet mix 0) or transparent, so
    // everything can jump: new topology, smoothers at their targets, clean state.
    band.running = wanted;
    band.fadePos = 0;
    band.logFreq.setImmediate (band.logFreq.getTarget());
    band.gainDb.setImmediate (band.gainDb.getTarget());
    band.logQ.setImmediate (band.logQ.getTarget());
    band.stateValid = false;
    band.primeOnStart = false;
    band.ramping = false;
    band.coeffsDirty = true;
}

void ParametricEq::refreshCoefficients (Band& band, const EqBandParams& target, bool ramp) const noexcept
{
    if (ramp)
        band.prevCoeffs = band.coeffs;
    // Settled smoothers use the exact target values, so in steady state the
    // running coefficients are bit-identical to what responseDb() designs.
    const double freq = band.logFreq.isSmoothing() ? std::exp2 (static_cast<double> (band.logFreq.getCurrent()))
                                                   : static_cast<double> (target.frequency);
    const double q = band.logQ.isSmoothing() ? std::exp2 (static_cast<double> (band.logQ.getCurrent()))
                                             : static_cast<double> (target.q);
    const double gain = static_cast<double> (band.gainDb.getCurrent());
    designBand (band.running.type, band.running.numSections, freq, q, gain, spec.sampleRate, band.coeffs.data());
    band.coeffsDirty = false;
    band.ramping = ramp;
    band.rampPos = 0;
}

void ParametricEq::updateBand (Band& band, const EqBandParams& target) noexcept
{
    if (! band.busy)
        return;

    band.ramping = false; // a coefficient ramp spans exactly one control period, which ends here

    const Topology wanted = topologyOf (target);

    // 1. Discrete changes: fade out, swap at mix 0, fade back in. A band that
    //    is already silent, or was skipped during the last control period as
    //    an exact 0 dB identity, swaps immediately (nothing to hear). The
    //    test is "not audible", not isTransparent(): a pending gain change
    //    (a preset load: new type AND new gain) makes the gain smoother busy,
    //    and fading out would first glide the OLD type towards the new gain
    //    and play it for ~5 ms.
    if (! (wanted == band.running))
    {
        if (band.fadePos == 0 || ! band.audible)
            swapTopology (band, wanted);
        else
            band.fadeDir = -1;
    }
    if (wanted == band.running)
    {
        const int goal = band.running.enabled ? fadeSamples : 0;
        band.fadeDir = band.fadePos < goal ? 1 : (band.fadePos > goal ? -1 : 0);
    }

    // 2. Continuous parameters. While a swap is pending the fading-out band
    //    keeps its current values: the fade-out should only remove the old
    //    sound, not morph the old type towards values meant for the new one
    //    (the smoothers jump to their targets at the swap, under mix 0). If
    //    the discrete change is reverted, the glide simply resumes.
    const bool swapPending = ! (wanted == band.running);
    const bool smoothing = band.logFreq.isSmoothing() || band.gainDb.isSmoothing() || band.logQ.isSmoothing();
    const bool silent = band.fadePos == 0 && band.fadeDir <= 0;
    if (smoothing && ! swapPending)
    {
        if (silent)
        {
            // Nobody hears this band: jump straight to the targets.
            band.logFreq.setImmediate (band.logFreq.getTarget());
            band.gainDb.setImmediate (band.gainDb.getTarget());
            band.logQ.setImmediate (band.logQ.getTarget());
        }
        else
        {
            stepSmoother (band.logFreq);
            stepSmoother (band.gainDb);
            stepSmoother (band.logQ);
        }
        band.coeffsDirty = true;
    }

    // 3. Decide whether the filter runs during the coming control period.
    //    Coefficients ramp only when the same filter keeps running; after a
    //    swap, a resume or a (re)start the state is cleared and they jump.
    //    A filter that starts while the wet mix is 0 (just swapped / enabled)
    //    is hidden by the fade-in. One that starts while the mix is above 0
    //    can only be a 0 dB bell / shelf leaving 0 dB: it was skipped as an
    //    exact identity, so it must resume from that identity (below). This
    //    holds whether its old state is stale or was already invalidated by
    //    an earlier swap (e.g. disabled, re-enabled at 0 dB, then boosted).
    const bool audibleNow = ! silent && ! isTransparent (band);
    const bool continuous = band.audible && audibleNow && band.stateValid;
    const bool resuming = audibleNow && ! band.audible && band.fadePos > 0;
    band.audible = audibleNow;
    if (band.audible && band.coeffsDirty)
        refreshCoefficients (band, target, continuous);

    if (resuming)
    {
        // Neither a stale nor a zero state matches the signal (a zero-state
        // high shelf, for instance, starts at m0 = A^2 instead of unity at
        // low frequencies: a step). Prime the low-pass integrator with the
        // input (DC equilibrium, band-pass = 0) and ramp the output mix from
        // identity (m0 = 1, m1 = m2 = 0) to the new design across this
        // control period, so the output leaves the identity path without a
        // step while the remaining state error is weighted by a tiny m1 / m2.
        band.stateValid = false;
        band.primeOnStart = true;
        band.prevCoeffs = band.coeffs;
        for (auto& c : band.prevCoeffs)
        {
            c.m0 = 1.0f;
            c.m1 = 0.0f;
            c.m2 = 0.0f;
        }
        band.ramping = true;
        band.rampPos = 0;
    }

    band.busy = band.fadeDir != 0 || band.ramping || swapPending || band.logFreq.isSmoothing()
                || band.gainDb.isSmoothing() || band.logQ.isSmoothing();
}

void ParametricEq::controlTick() noexcept
{
    bool busy = false;
    for (size_t b = 0; b < bandDsp.size(); ++b)
    {
        updateBand (bandDsp[b], targets[b]);
        busy = busy || bandDsp[b].busy;
    }
    anyBusy = busy;
}

//==============================================================================
void ParametricEq::processBand (Band& band, const AudioBlock& block, int start, int length, int numChannels) noexcept
{
    if (! band.stateValid)
    {
        for (auto& ch : band.state)
            for (auto& s : ch)
                s.reset();
        if (band.primeOnStart)
            for (int c = 0; c < numChannels; ++c)
                band.state[static_cast<size_t> (c)][0].ic2 = block.channel (c)[start];
        band.stateValid = true;
        band.primeOnStart = false;
    }

    const int numSections = band.running.numSections;
    const bool crossfading = band.fadeDir != 0 || band.fadePos != fadeSamples;

    // Runs the section cascade over d[0, n); `offset` = position of d[0] in the segment.
    const auto runCascade = [&band, numSections] (std::array<SvfState, kMaxSections>& st, float* d, int n, int offset) noexcept
    {
        for (int s = 0; s < numSections; ++s)
        {
            const auto si = static_cast<size_t> (s);
            if (band.ramping)
                runSectionRamped (band.prevCoeffs[si], band.coeffs[si], st[si], d, n, band.rampPos + offset);
            else
                runSection (band.coeffs[si], st[si], d, n);
        }
    };

    for (int c = 0; c < numChannels; ++c)
    {
        float* d = block.channel (c) + start;
        auto& st = band.state[static_cast<size_t> (c)];

        if (! crossfading)
        {
            // Fully wet: section-major, in place.
            runCascade (st, d, length, 0);
            continue;
        }

        // Crossfading segments never span a control tick, so they are at most
        // kControlInterval long; the loop only guards against misuse.
        for (int off = 0; off < length; off += kControlInterval)
        {
            const int n = std::min (kControlInterval, length - off);
            std::array<float, kControlInterval> dry;
            std::copy (d + off, d + off + n, dry.begin());
            runCascade (st, d + off, n, off);
            for (int i = 0; i < n; ++i)
            {
                // Linear per-sample ramp; integer position -> exact 0 / 1 endpoints.
                const int pos = std::clamp (band.fadePos + band.fadeDir * (off + i + 1), 0, fadeSamples);
                const float mix = static_cast<float> (pos) * invFadeSamples;
                const float x = dry[static_cast<size_t> (i)];
                d[off + i] = x + mix * (d[off + i] - x);
            }
        }
    }

    if (band.ramping)
        band.rampPos = std::min (band.rampPos + length, kControlInterval);
}

void ParametricEq::applyOutputGain (const AudioBlock& block, int numChannels) noexcept
{
    const int numSamples = block.numSamples;
    if (outputGain.isSmoothing())
    {
        // Sample-outer so every channel gets the identical ramp and the ramp
        // position depends only on the number of samples processed.
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = outputGain.next();
            for (int c = 0; c < numChannels; ++c)
                block.channel (c)[i] *= g;
        }
        return;
    }

    const float g = outputGain.getCurrent();
    if (g == 1.0f)
        return;
    for (int c = 0; c < numChannels; ++c)
    {
        float* d = block.channel (c);
        for (int i = 0; i < numSamples; ++i)
            d[i] *= g;
    }
}

void ParametricEq::process (const AudioBlock& block) noexcept
{
    const int numSamples = block.numSamples;
    const int numChannels = std::clamp (block.numChannels, 0, spec.numChannels);
    if (numSamples <= 0)
        return;

    int pos = 0;
    while (pos < numSamples)
    {
        if (samplesToTick == 0)
        {
            if (anyBusy)
                controlTick();
            samplesToTick = kControlInterval;
        }

        // While nothing is busy every tick would be a no-op, so the rest of the
        // block is one segment (identical output, far fewer loop set-ups).
        const int len = anyBusy ? std::min (samplesToTick, numSamples - pos) : numSamples - pos;

        for (auto& band : bandDsp)
        {
            if (band.audible)
                processBand (band, block, pos, len, numChannels);
            if (band.fadeDir != 0)
                band.fadePos = std::clamp (band.fadePos + band.fadeDir * len, 0, fadeSamples);
        }

        // Keep the tick grid aligned to absolute stream time.
        samplesToTick = ((samplesToTick - len) % kControlInterval + kControlInterval) % kControlInterval;
        pos += len;
    }

    applyOutputGain (block, numChannels);
}
} // namespace flub
