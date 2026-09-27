// Flubsound Pro - clarity & transient-detail enhancer.
//
// Stages in series, all zero latency:
//
//   1. TransientShaper, full band, detector max_c |x_c| (see its header).
//   2. De-mud: detector = unity band-pass 250 Hz Q 1 and the broadband signal,
//      both as linked mean squares (20 ms). Overshoot of the band level over
//      (broadband level - 12 dB) -> 6 dB soft knee -> ratio 2:1 -> cut of at
//      most 4 dB, faded out below -70..-60 dB RMS broadband. GainSmoother
//      15 ms attack (cut deepening) / 150 ms release. EQ: bell 250 Hz Q 1 at
//      deMud * cut.
//   3. Dynamic presence: detector = unity band-pass at presenceFrequency
//      Q 0.8, linked mean square (20 ms). boost = clamp ((-18 dB - L) / 4,
//      0, 6 dB) (i.e. full 6 dB at <= -42 dB RMS, none at >= -18 dB RMS),
//      faded out towards the -80 dB RMS noise floor so hiss is never lifted.
//      GainSmoother 5 ms (boost withdrawing) / 100 ms. EQ: bell Q 0.8 at
//      presence * boost.
//   4. Air exciter, per channel: h = HP4 3.5 kHz (x), b = LP4 7 kHz (h)
//      (Butterworth). Envelopes E(.) = 7.5 ms peak hold -> 40 ms release;
//      env = smooth_0.5ms (max (E(|b|), -3 dB * E(|h|))); xn = clamp (b / env,
//      -1, 1); y = env (T2(xn) + 0.5 T3(xn)) (2nd + 3rd harmonic of a
//      sinusoid, level tracking the band linearly) -> HP4 7 kHz -> + air *
//      -12 dB. Then a high shelf 10 kHz Q 0.707 at 2 dB * air.
//      The polynomial is of order 3 and its input is band limited to 7 kHz,
//      so products stay below 21 kHz: no oversampling at 44.1 kHz. The peak
//      hold makes env constant on steady tones, so no modulation products.
//      The -3 dB * E(|h|) floor matters for content in the band's upper skirt
//      (> 7 kHz, where the LP4 is below -3 dB): normalised against its own
//      small band level it would be shaped at full depth, and its 3rd (above
//      fs / 6) or 2nd (above fs / 4) harmonic would alias. Against the floor
//      it is shaped only gently (a 9 kHz tone's alias drops by ~13 dB), while
//      everything below 7 kHz is normalised by the band envelope unchanged.
//      It also backs the exciter off when the top octave is already bright.
//
// Control rate: every kControlInterval samples of absolute stream time the
// gain computers run, the smoothers advance and EQ designs are refreshed;
// new designs are reached by a per-sample glide across the next interval
// (TransientShaper::SvfGlide), so there is no zipper noise and the output does
// not depend on how the host splits blocks.
//
// Neutral parameters: the transient shaper returns exactly 1 and every other
// stage is inactive (skipped), so the output is bit-exact. A stage switched
// on starts from clean state with its amount ramping from 0 (20 ms); switched
// off it runs until its amount and EQ gain are exactly 0, then stops.
#include "flub/dsp/ClarityEnhancer.h"

#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>

namespace flub
{
namespace
{
constexpr float kParamSmoothMs = 20.0f;
constexpr float kFreqGlideMs = 25.0f;
constexpr float kDetectorMs = 20.0f;   // mean-square time constant
constexpr float kTaperDb = 10.0f;      // gate / noise-floor fades
constexpr float kKneeDb = 6.0f;

// 2. De-mud.
constexpr double kDeMudHz = 250.0;
constexpr double kDeMudQ = 1.0;
constexpr float kDeMudRelativeDb = -12.0f; // threshold relative to the broadband level
constexpr float kDeMudSlope = 0.5f;        // 1 - 1 / ratio, ratio 2
constexpr float kDeMudRangeDb = 4.0f;
constexpr float kDeMudGateDb = -70.0f;     // broadband RMS below which it fades out
constexpr float kDeMudAttackMs = 15.0f;
constexpr float kDeMudReleaseMs = 150.0f;

// 3. Presence.
constexpr double kPresenceQ = 0.8;
constexpr float kPresenceMaxDb = 6.0f;
constexpr float kPresenceThresholdDb = -18.0f; // band RMS at which the boost is gone
constexpr float kPresenceSlope = 0.25f;        // dB of boost per dB below threshold
constexpr float kPresenceFloorDb = -80.0f;     // no lift at / below the noise floor
constexpr float kPresenceAttackMs = 5.0f;
constexpr float kPresenceReleaseMs = 100.0f;

// 4. Air.
constexpr double kAirLowHz = 3500.0;
constexpr double kAirHighHz = 7000.0;
constexpr double kAirShelfHz = 10000.0;
constexpr double kAirShelfQ = 0.70710678118654752;
constexpr float kAirShelfMaxDb = 2.0f;
constexpr float kAirMixMax = 0.251188643f;     // -12 dB
constexpr float kAirW2 = 1.0f, kAirW3 = 0.5f;  // 2nd / 3rd harmonic weights
constexpr double kAirHoldMs = 7.5;             // >= 26 periods of the band's lowest frequency
constexpr float kAirSkirtFloor = 0.70710678f;  // -3 dB: the LP4 7 kHz level at its corner
constexpr float kAirReleaseMs = 40.0f;
constexpr float kAirSmoothMs = 0.5f;

constexpr float kStateFlush = 1.0e-20f;
constexpr float kEnvFlush = 1.0e-15f;

float clampOr (float v, float lo, float hi, float fallback) noexcept
{
    return std::isnan (v) ? fallback : std::clamp (v, lo, hi);
}

ClarityParams sanitise (const ClarityParams& in, const ClarityParams& prev) noexcept
{
    ClarityParams p;
    p.attackDb = clampOr (in.attackDb, -12.0f, 12.0f, prev.attackDb);
    p.sustainDb = clampOr (in.sustainDb, -12.0f, 12.0f, prev.sustainDb);
    p.presence = clampOr (in.presence, 0.0f, 1.0f, prev.presence);
    p.presenceFrequency = clampOr (in.presenceFrequency, 1000.0f, 6000.0f, prev.presenceFrequency);
    p.air = clampOr (in.air, 0.0f, 1.0f, prev.air);
    p.deMud = clampOr (in.deMud, 0.0f, 1.0f, prev.deMud);
    return p;
}

float softKnee (float overDb) noexcept
{
    constexpr float halfKnee = 0.5f * kKneeDb;
    if (overDb <= -halfKnee)
        return 0.0f;
    if (overDb >= halfKnee)
        return overDb;
    const float t = overDb + halfKnee;
    return t * t / (2.0f * kKneeDb);
}

/** De-mud gain computer (dB, <= 0). */
float deMudGainDb (float bandDb, float broadDb) noexcept
{
    const float over = bandDb - (broadDb + kDeMudRelativeDb);
    const float cut = std::min (kDeMudRangeDb, softKnee (over) * kDeMudSlope);
    const float gate = std::clamp ((broadDb - kDeMudGateDb) / kTaperDb, 0.0f, 1.0f);
    return -cut * gate;
}

/** Presence gain computer (dB, >= 0): inverse level, tapered at the noise floor. */
float presenceGainDb (float bandDb) noexcept
{
    const float boost = std::clamp ((kPresenceThresholdDb - bandDb) * kPresenceSlope, 0.0f, kPresenceMaxDb);
    const float taper = std::clamp ((bandDb - kPresenceFloorDb) / kTaperDb, 0.0f, 1.0f);
    return boost * taper;
}

/** GainSmoother step that lands exactly on the target (no endless tail). */
float settle (GainSmoother& s, float targetDb) noexcept
{
    const float v = s.process (targetDb);
    if (std::abs (v - targetDb) < 1.0e-5f)
    {
        s.reset (targetDb);
        return targetDb;
    }
    return v;
}

float flushTiny (SvfState& s) noexcept
{
    if (std::abs (s.ic1) < kStateFlush)
        s.ic1 = 0.0f;
    if (std::abs (s.ic2) < kStateFlush)
        s.ic2 = 0.0f;
    return s.ic1 + s.ic2;
}

float flushTiny (float& v) noexcept
{
    if (std::abs (v) < kEnvFlush)
        v = 0.0f;
    return v;
}
} // namespace

//==============================================================================
void ClarityEnhancer::prepare (const ProcessSpec& newSpec)
{
    spec = newSpec;
    spec.numChannels = std::clamp (spec.numChannels, 1, kMaxChannels);
    if (! (spec.sampleRate > 0.0))
        spec.sampleRate = 48000.0;
    controlRate = spec.sampleRate / static_cast<double> (kControlInterval);
    const double sr = spec.sampleRate;

    msCoeff = onePoleCoeff (kDetectorMs, sr);
    shaper.prepare (sr);

    deMud.detector = SvfCoeffs::make (FilterType::BandPass, kDeMudHz, kDeMudQ, 0.0, sr);
    deMud.gain.prepare (controlRate, kDeMudAttackMs, kDeMudReleaseMs, false);
    presence.gain.prepare (controlRate, kPresenceAttackMs, kPresenceReleaseMs, false);

    airFilters[0] = SvfCoeffs::make (FilterType::HighPass, kAirLowHz, butterworthQ (2, 0), 0.0, sr);
    airFilters[1] = SvfCoeffs::make (FilterType::HighPass, kAirLowHz, butterworthQ (2, 1), 0.0, sr);
    airFilters[2] = SvfCoeffs::make (FilterType::LowPass, kAirHighHz, butterworthQ (2, 0), 0.0, sr);
    airFilters[3] = SvfCoeffs::make (FilterType::LowPass, kAirHighHz, butterworthQ (2, 1), 0.0, sr);
    airFilters[4] = SvfCoeffs::make (FilterType::HighPass, kAirHighHz, butterworthQ (2, 0), 0.0, sr);
    airFilters[5] = SvfCoeffs::make (FilterType::HighPass, kAirHighHz, butterworthQ (2, 1), 0.0, sr);
    for (auto& ch : airChannels)
    {
        ch.bandHold.prepare (sr, kAirHoldMs);
        ch.highHold.prepare (sr, kAirHoldMs);
    }
    airReleaseCoeff = onePoleCoeff (kAirReleaseMs, sr);
    airSmoothCoeff = onePoleCoeff (kAirSmoothMs, sr);
    airMix.reset (sr, kParamSmoothMs, 0.0f);

    reset();
}

void ClarityEnhancer::reset() noexcept FLUB_NONBLOCKING
{
    controlCountdown = kControlInterval;
    const double sr = spec.sampleRate;

    // After a reset there is no previous output to click against: amounts
    // start at their targets; dynamic gains start neutral.
    shaper.reset();

    presenceHz = params.presenceFrequency;
    logPresenceHz.reset (controlRate, kFreqGlideMs, std::log (presenceHz));
    presence.detector = SvfCoeffs::make (FilterType::BandPass, presenceHz, kPresenceQ, 0.0, sr);

    auto resetBell = [this, sr] (DynamicBell& bell, float amount, double hz, double q)
    {
        bell.active = amount > 0.0f;
        bell.amount.reset (controlRate, kParamSmoothMs, amount);
        bell.gain.reset (0.0f);
        bell.appliedDb = 0.0f;
        bell.eq.setImmediate (SvfCoeffs::make (FilterType::Bell, hz, q, 0.0, sr));
    };
    resetBell (deMud, params.deMud, kDeMudHz, kDeMudQ);
    resetBell (presence, params.presence, presenceHz, kPresenceQ);

    airActive = params.air > 0.0f;
    airAmount.reset (controlRate, kParamSmoothMs, params.air);
    airMix.setImmediate (params.air * kAirMixMax);
    airShelfDb = kAirShelfMaxDb * params.air;
    airShelf.setImmediate (SvfCoeffs::make (FilterType::HighShelf, kAirShelfHz, kAirShelfQ, airShelfDb, sr));

    clearAllStates();
}

//==============================================================================
void ClarityEnhancer::setParams (const ClarityParams& newParams) noexcept FLUB_NONBLOCKING
{
    const ClarityParams p = sanitise (newParams, params);
    if (p == params)
        return; // the chain pushes every block; unchanged values cost nothing
    params = p;

    shaper.setAttackDb (p.attackDb);
    shaper.setSustainDb (p.sustainDb);
    logPresenceHz.setTarget (std::log (p.presenceFrequency));

    if (p.deMud > 0.0f && ! deMud.active)
        activateBell (deMud, kDeMudHz, kDeMudQ);
    deMud.amount.setTarget (p.deMud);

    if (p.presence > 0.0f && ! presence.active)
        activateBell (presence, presenceHz, kPresenceQ);
    presence.amount.setTarget (p.presence);

    if (p.air > 0.0f && ! airActive)
        activateAir();
    airAmount.setTarget (p.air);
    airMix.setTarget (p.air * kAirMixMax);
}

void ClarityEnhancer::activateBell (DynamicBell& bell, double hz, double q) noexcept
{
    // An inactive bell has landed on amount 0 / 0 dB; start from clean state.
    bell.active = true;
    bell.amount.setImmediate (0.0f);
    bell.gain.reset (0.0f);
    bell.appliedDb = 0.0f;
    bell.bandMs = bell.broadMs = 0.0f;
    bell.detectorState.fill ({});
    bell.eqState.fill ({});
    bell.eq.setImmediate (SvfCoeffs::make (FilterType::Bell, hz, q, 0.0, spec.sampleRate));
}

void ClarityEnhancer::activateAir() noexcept
{
    airActive = true;
    airAmount.setImmediate (0.0f);
    airMix.setImmediate (0.0f);
    airShelfDb = 0.0f;
    airShelf.setImmediate (SvfCoeffs::make (FilterType::HighShelf, kAirShelfHz, kAirShelfQ, 0.0, spec.sampleRate));
    for (auto& ch : airChannels)
    {
        ch.filters.fill ({});
        ch.bandHold.reset();
        ch.highHold.reset();
        ch.bandRelease = ch.highRelease = ch.env = 0.0f;
    }
    airShelfState.fill ({});
}

void ClarityEnhancer::clearAllStates() noexcept
{
    for (auto* bell : { &deMud, &presence })
    {
        bell->bandMs = bell->broadMs = 0.0f;
        bell->detectorState.fill ({});
        bell->eqState.fill ({});
    }
    for (auto& ch : airChannels)
    {
        ch.filters.fill ({});
        ch.bandHold.reset();
        ch.highHold.reset();
        ch.bandRelease = ch.highRelease = ch.env = 0.0f;
    }
    airShelfState.fill ({});
}

float ClarityEnhancer::flushStates() noexcept
{
    float sum = 0.0f;
    for (auto* bell : { &deMud, &presence })
    {
        if (! bell->active)
            continue;
        sum += flushTiny (bell->bandMs) + flushTiny (bell->broadMs);
        for (int c = 0; c < spec.numChannels; ++c)
        {
            const size_t ch = static_cast<size_t> (c);
            sum += flushTiny (bell->detectorState[ch]) + flushTiny (bell->eqState[ch]);
        }
    }
    if (airActive)
    {
        for (int c = 0; c < spec.numChannels; ++c)
        {
            auto& ch = airChannels[static_cast<size_t> (c)];
            for (auto& s : ch.filters)
                sum += flushTiny (s);
            sum += flushTiny (ch.bandRelease) + flushTiny (ch.highRelease) + flushTiny (ch.env)
                 + flushTiny (airShelfState[static_cast<size_t> (c)]);
        }
    }
    return sum;
}

//==============================================================================
void ClarityEnhancer::updateBell (DynamicBell& bell, float gainDb, double hz, double q, bool moved) noexcept
{
    if (gainDb != bell.appliedDb || moved)
    {
        bell.appliedDb = gainDb;
        bell.eq.glideTo (SvfCoeffs::make (FilterType::Bell, hz, q, gainDb, spec.sampleRate));
    }
    else
    {
        bell.eq.ramping = false;
    }

    // Switched off and landed on an exact 0 dB identity: stop processing.
    if (bell.amount.getTarget() == 0.0f && bell.amount.getCurrent() == 0.0f && bell.eq.isIdentity())
        bell.active = false;
}

void ClarityEnhancer::controlTick() noexcept
{
    const double sr = spec.sampleRate;

    bool presenceMoved = false;
    if (logPresenceHz.isSmoothing())
    {
        presenceHz = std::exp (logPresenceHz.next());
        presence.detector = SvfCoeffs::make (FilterType::BandPass, presenceHz, kPresenceQ, 0.0, sr);
        presenceMoved = true;
    }

    if (deMud.active)
    {
        const float amount = deMud.amount.next();
        const float target = deMudGainDb (powerToDb (deMud.bandMs), powerToDb (deMud.broadMs));
        updateBell (deMud, amount * settle (deMud.gain, target), kDeMudHz, kDeMudQ, false);
    }

    if (presence.active)
    {
        const float amount = presence.amount.next();
        const float target = presenceGainDb (powerToDb (presence.bandMs));
        updateBell (presence, amount * settle (presence.gain, target), presenceHz, kPresenceQ, presenceMoved);
    }

    if (airActive)
    {
        const float amount = airAmount.next();
        const float shelfDb = kAirShelfMaxDb * amount;
        if (shelfDb != airShelfDb)
        {
            airShelfDb = shelfDb;
            airShelf.glideTo (SvfCoeffs::make (FilterType::HighShelf, kAirShelfHz, kAirShelfQ, shelfDb, sr));
        }
        else
        {
            airShelf.ramping = false;
        }

        if (airAmount.getTarget() == 0.0f && amount == 0.0f && airMix.getTarget() == 0.0f
            && ! airMix.isSmoothing() && airShelf.isIdentity())
            airActive = false;
    }

    if (! std::isfinite (flushStates()))
        clearAllStates();
}

//==============================================================================
void ClarityEnhancer::process (const AudioBlock& block) noexcept FLUB_NONBLOCKING
{
    const int numSamples = block.numSamples;
    const int numCh = std::min ({ block.numChannels, spec.numChannels, kMaxChannels });
    if (numSamples <= 0 || numCh <= 0)
        return;

    for (int pos = 0; pos < numSamples;)
    {
        const int len = std::min (numSamples - pos, controlCountdown);
        const int phase = kControlInterval - controlCountdown;

        // 1. Transient shaper: one linked gain per sample. Neutral -> exactly 1.
        for (int i = pos; i < pos + len; ++i)
        {
            float linked = 0.0f;
            for (int c = 0; c < numCh; ++c)
                linked = std::max (linked, std::abs (block.channel (c)[i]));
            const float g = shaper.computeGain (linked);
            if (g != 1.0f)
                for (int c = 0; c < numCh; ++c)
                    block.channel (c)[i] *= g;
        }

        // 2. De-mud, 3. presence, 4. air (each skipped while inactive).
        if (deMud.active)
            processBell (deMud, block, numCh, pos, len, phase, true);
        if (presence.active)
            processBell (presence, block, numCh, pos, len, phase, false);
        if (airActive)
            processAir (block, numCh, pos, len, phase);

        pos += len;
        controlCountdown -= len;
        if (controlCountdown == 0)
        {
            controlCountdown = kControlInterval;
            controlTick();
        }
    }
}

void ClarityEnhancer::processBell (DynamicBell& bell, const AudioBlock& block, int numCh, int pos, int len, int phase,
                                   bool trackBroadband) noexcept
{
    const size_t n = static_cast<size_t> (len);

    // Linked detection on this stage's input: per sample, max over channels
    // of the squared band-pass output (and of the squared input for the
    // broadband reference), then one mean-square follower each.
    std::fill_n (scratchA.begin(), n, 0.0f);
    const SvfCoeffs det = bell.detector;
    for (int c = 0; c < numCh; ++c)
    {
        SvfState s = bell.detectorState[static_cast<size_t> (c)];
        const float* x = block.channel (c) + pos;
        for (size_t i = 0; i < n; ++i)
        {
            const float d = svfTick (det, s, x[i]);
            scratchA[i] = std::max (scratchA[i], d * d);
        }
        bell.detectorState[static_cast<size_t> (c)] = s;
    }
    for (size_t i = 0; i < n; ++i)
        bell.bandMs = scratchA[i] + msCoeff * (bell.bandMs - scratchA[i]);

    if (trackBroadband)
    {
        std::fill_n (scratchB.begin(), n, 0.0f);
        for (int c = 0; c < numCh; ++c)
        {
            const float* x = block.channel (c) + pos;
            for (size_t i = 0; i < n; ++i)
                scratchB[i] = std::max (scratchB[i], x[i] * x[i]);
        }
        for (size_t i = 0; i < n; ++i)
            bell.broadMs = scratchB[i] + msCoeff * (bell.broadMs - scratchB[i]);
    }

    // The EQ runs whenever the stage is active (also at 0 dB, where it is an
    // exact identity) so its state is always current when the gain moves.
    applyGlide (bell.eq, bell.eqState, block, numCh, pos, len, phase);
}

void ClarityEnhancer::processAir (const AudioBlock& block, int numCh, int pos, int len, int phase) noexcept
{
    const size_t n = static_cast<size_t> (len);
    for (size_t i = 0; i < n; ++i)
        scratchA[i] = airMix.next();

    for (int c = 0; c < numCh; ++c)
    {
        AirChannel& st = airChannels[static_cast<size_t> (c)];
        float* d = block.channel (c) + pos;
        for (size_t i = 0; i < n; ++i)
        {
            float high = svfTick (airFilters[0], st.filters[0], d[i]);
            high = svfTick (airFilters[1], st.filters[1], high); // everything above 3.5 kHz
            float b = svfTick (airFilters[2], st.filters[2], high);
            b = svfTick (airFilters[3], st.filters[3], b);       // the 3.5 - 7 kHz band

            // Envelopes: instant rise through the hold, 40 ms fall; then a
            // 0.5 ms smoother so level changes never step (a stepping env
            // would splatter through the -env term of T2).
            st.bandRelease = std::max (st.bandHold.process (std::abs (b)), st.bandRelease * airReleaseCoeff);
            st.highRelease = std::max (st.highHold.process (std::abs (high)), st.highRelease * airReleaseCoeff);
            const float target = std::max (st.bandRelease, kAirSkirtFloor * st.highRelease);
            st.env = target + airSmoothCoeff * (st.env - target);

            const float xn = st.env > 0.0f ? std::clamp (b / st.env, -1.0f, 1.0f) : 0.0f;
            const float x2 = xn * xn;
            const float h = kAirW2 * (2.0f * x2 - 1.0f) + kAirW3 * xn * (4.0f * x2 - 3.0f);
            float y = h * st.env;
            y = svfTick (airFilters[4], st.filters[4], y);
            y = svfTick (airFilters[5], st.filters[5], y);
            d[i] += scratchA[i] * y;
        }
    }

    applyGlide (airShelf, airShelfState, block, numCh, pos, len, phase);
}

void ClarityEnhancer::applyGlide (TransientShaper::SvfGlide& glide, std::array<SvfState, kMaxChannels>& state,
                                  const AudioBlock& block, int numCh, int pos, int len, int phase) noexcept
{
    const size_t n = static_cast<size_t> (len);
    if (glide.ramping)
    {
        // Ramp position from the global control phase: independent of block splits.
        constexpr float invInterval = 1.0f / static_cast<float> (kControlInterval);
        for (size_t i = 0; i < n; ++i)
            rampScratch[i] = glide.at (static_cast<float> (phase + static_cast<int> (i) + 1) * invInterval);
        for (int c = 0; c < numCh; ++c)
        {
            SvfState s = state[static_cast<size_t> (c)];
            float* d = block.channel (c) + pos;
            for (size_t i = 0; i < n; ++i)
                d[i] = svfTick (rampScratch[i], s, d[i]);
            state[static_cast<size_t> (c)] = s;
        }
    }
    else
    {
        const SvfCoeffs cf = glide.end;
        for (int c = 0; c < numCh; ++c)
        {
            SvfState s = state[static_cast<size_t> (c)];
            float* d = block.channel (c) + pos;
            for (size_t i = 0; i < n; ++i)
                d[i] = svfTick (cf, s, d[i]);
            state[static_cast<size_t> (c)] = s;
        }
    }
}
} // namespace flub
