#include "flub/engine/HearingGuard.h"

#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>

namespace flub
{
namespace
{
// IEC 61672-1 Annex E pole frequencies of the A-weighting curve.
constexpr double kF1 = 20.598997, kF2 = 107.65265, kF3 = 737.86223, kF4 = 12194.217;
constexpr double kGainSnap = 1.0e-4; // a gain this close to 1 becomes 1 (-0.0009 dB)
constexpr float kSilentDbA = -100.0f; // the readings' floor

double designRate (double fs) noexcept
{
    if (! std::isfinite (fs) || fs <= 0.0)
        return 48000.0;
    return std::max (fs, 8000.0);
}

/** Bilinear transform (no prewarp) of (B2 s^2 + B1 s + B0) / (A2 s^2 + A1 s + A0). */
BiquadCoeffs bilinear2 (double B2, double B1, double B0, double A2, double A1, double A0, double fs) noexcept
{
    const double K = 2.0 * fs, K2 = K * K;
    const double d0 = A2 * K2 + A1 * K + A0;
    BiquadCoeffs c;
    c.b0 = (B2 * K2 + B1 * K + B0) / d0;
    c.b1 = 2.0 * (B0 - B2 * K2) / d0;
    c.b2 = (B2 * K2 - B1 * K + B0) / d0;
    c.a1 = 2.0 * (A0 - A2 * K2) / d0;
    c.a2 = (A2 * K2 - A1 * K + A0) / d0;
    return c;
}

/** Relative power (1 = 80 dB(A)) -> dB(A), floored. */
float toDbA (double relPower) noexcept
{
    if (! (relPower > 0.0))
        return kSilentDbA;
    return std::max (kSilentDbA, static_cast<float> (Dosimeter::kReferenceDbA + 10.0 * std::log10 (relPower)));
}
} // namespace

//==============================================================================
double Dosimeter::doseFor (double levelDbA, double secs) noexcept
{
    if (! std::isfinite (levelDbA) || ! (secs > 0.0))
        return 0.0;
    return secs / kReferenceSeconds * std::pow (10.0, (levelDbA - kReferenceDbA) / 10.0);
}

double Dosimeter::secondsFor (double levelDbA, double fraction) noexcept
{
    if (! std::isfinite (levelDbA) || ! (fraction > 0.0))
        return 0.0;
    return fraction * kReferenceSeconds / std::pow (10.0, (levelDbA - kReferenceDbA) / 10.0);
}

void Dosimeter::add (double levelDbA, double secs) noexcept
{
    if (! (secs > 0.0))
        return;
    dose += doseFor (levelDbA, secs);
    elapsed += secs;
}

double Dosimeter::equivalentLevelDbA() const noexcept
{
    if (! (elapsed > 0.0) || ! (dose > 0.0))
        return HearingMeters::kUnknown;
    return kReferenceDbA + 10.0 * std::log10 (dose * kReferenceSeconds / elapsed);
}

//==============================================================================
HearingGuard::SensitivitySource HearingGuard::chooseSensitivity (float userDbSpl, float profileDbSpl, float& chosenDbSpl) noexcept
{
    if (std::isfinite (userDbSpl))
    {
        chosenDbSpl = std::clamp (userDbSpl, kMinSensitivityDbSpl, kMaxSensitivityDbSpl);
        return SensitivitySource::User;
    }
    if (std::isfinite (profileDbSpl))
    {
        chosenDbSpl = std::clamp (profileDbSpl, kMinSensitivityDbSpl, kMaxSensitivityDbSpl);
        return SensitivitySource::Profile;
    }
    chosenDbSpl = std::numeric_limits<float>::quiet_NaN();
    return SensitivitySource::Unknown;
}

std::array<BiquadCoeffs, 3> HearingGuard::aWeighting (double sr) noexcept
{
    const double fs = designRate (sr);
    const double w1 = kTwoPi * kF1, w2 = kTwoPi * kF2, w3 = kTwoPi * kF3, w4 = kTwoPi * kF4;
    std::array<BiquadCoeffs, 3> c;
    c[0] = bilinear2 (1.0, 0.0, 0.0, 1.0, 2.0 * w1, w1 * w1, fs); // s^2 / (s + w1)^2
    c[1] = bilinear2 (1.0, 0.0, 0.0, 1.0, w2 + w3, w2 * w3, fs);  // s^2 / ((s + w2)(s + w3))
    // 1 / (s + w4)^2: its double pole by the matched z-transform, and a
    // double zero at z = -beta fitted to the analog curve (beta 0 leaves
    // the top octave too loud, the bilinear transform's beta 1 falls to
    // -inf at fs / 2): the least worst-case error from 20 Hz to 12.5 kHz
    // (or 0.45 fs), all sections together, normalised at 1 kHz.
    const double p = std::exp (-w4 / fs);
    c[2].a1 = -2.0 * p;
    c[2].a2 = p * p;
    constexpr int kPoints = 48;
    const double top = std::min (12500.0, 0.45 * fs);
    std::array<double, kPoints> freq {}, target {}, fixedDb {};
    for (int i = 0; i < kPoints; ++i)
    {
        const double f = 20.0 * std::pow (top / 20.0, i / static_cast<double> (kPoints - 1));
        freq[static_cast<size_t> (i)] = f;
        target[static_cast<size_t> (i)] = aWeightingCurveDb (f);
        fixedDb[static_cast<size_t> (i)] = 20.0 * std::log10 (std::abs (c[0].response (f, fs) * c[1].response (f, fs)));
    }
    const double fixed1k = 20.0 * std::log10 (std::abs (c[0].response (1000.0, fs) * c[1].response (1000.0, fs)));
    const auto withZero = [&c] (double beta) {
        BiquadCoeffs s = c[2];
        s.b0 = 1.0;
        s.b1 = 2.0 * beta;
        s.b2 = beta * beta;
        return s;
    };
    const auto worstError = [&] (double beta) {
        const BiquadCoeffs s = withZero (beta);
        const double at1k = fixed1k + 20.0 * std::log10 (std::abs (s.response (1000.0, fs)));
        double worst = 0.0;
        for (size_t i = 0; i < static_cast<size_t> (kPoints); ++i)
            worst = std::max (worst, std::abs (fixedDb[i] + 20.0 * std::log10 (std::abs (s.response (freq[i], fs))) - at1k - target[i]));
        return worst;
    };
    double best = 0.0, bestError = worstError (0.0);
    for (double step : { 0.01, 0.0002 })
    {
        const double centre = best;
        for (int k = -50; k <= 100; ++k)
        {
            const double beta = centre + step * k;
            if (beta < 0.0 || beta > 1.0)
                continue;
            if (const double e = worstError (beta); e < bestError)
            {
                bestError = e;
                best = beta;
            }
        }
    }
    c[2] = withZero (best);
    const double g1k = std::abs (c[0].response (1000.0, fs) * c[1].response (1000.0, fs) * c[2].response (1000.0, fs));
    c[2].b0 /= g1k;
    c[2].b1 /= g1k;
    c[2].b2 /= g1k;
    return c;
}

double HearingGuard::aWeightingCurveDb (double f) noexcept
{
    const auto ra = [] (double fr) {
        const double q = fr * fr;
        return kF4 * kF4 * q * q
               / ((q + kF1 * kF1) * std::sqrt ((q + kF2 * kF2) * (q + kF3 * kF3)) * (q + kF4 * kF4));
    };
    return 20.0 * std::log10 (ra (f) / ra (1000.0));
}

//==============================================================================
void HearingGuard::prepare (double sr)
{
    doseCarried = sessionDose(); // keep counting across a re-prepare
    sessionEnergy = {};
    sessionSamples = 0.0;
    sampleRate = designRate (sr);
    aw = aWeighting (sampleRate);
    windowSamples = kCapWindowSeconds * sampleRate;
    segmentLength = std::max (1, static_cast<int> (std::ceil (windowSamples / kCapSegments)));
    // The filters and the window restart; the cap's gain and its detector
    // (rate-independent) carry on, so a re-prepare does not jump the level.
    preState = {};
    postState = {};
    clearWindow();
    publish (std::isfinite (sensitivity.load (std::memory_order_relaxed)));
}

void HearingGuard::reset() noexcept FLUB_NONBLOCKING
{
    preState = {};
    postState = {};
    slowPower = {};
    fastPower = {};
    fastPre = {};
    clearWindow();
    gain = slowGain = planGain = 1.0;
}

void HearingGuard::plan (double budget) noexcept
{
    // The constant output power that keeps every future window within the
    // budget: after j more segments the oldest j have left the window, so
    // P <= (budget - what stays) / (j segments), for every j; at j =
    // kCapSegments that is the cap itself.
    const double segment = static_cast<double> (segmentLength);
    for (size_t e = 0; e < 2; ++e)
    {
        double left = 0.0, lowest = budget / (segment * kCapSegments);
        for (int j = 1; j < kCapSegments; ++j)
        {
            left += ring[e][static_cast<size_t> ((ringPos + j - 1) % kCapSegments)];
            lowest = std::min (lowest, (budget - (windowSum[e] + segmentEnergy[e] - left)) / (segment * j));
        }
        sustainable[e] = std::max (0.0, lowest);
    }
}

void HearingGuard::clearWindow() noexcept
{
    for (auto& r : ring)
        r.fill (0.0);
    windowSum = {};
    segmentEnergy = {};
    segmentPos = 0;
    ringPos = 0;
    needPlan = true;
}

double HearingGuard::sessionDose() const noexcept
{
    return doseCarried + std::max (sessionEnergy[0], sessionEnergy[1]) / sampleRate / Dosimeter::kReferenceSeconds;
}

void HearingGuard::setSensitivityDbSpl (float dbSpl) noexcept
{
    sensitivity.store (std::isfinite (dbSpl) ? std::clamp (dbSpl, kMinSensitivityDbSpl, kMaxSensitivityDbSpl)
                                             : std::numeric_limits<float>::quiet_NaN(),
                       std::memory_order_relaxed);
}

void HearingGuard::setEndpointVolumeDb (float db) noexcept
{
    // An unreadable volume (NaN) counts as full volume: the loudest case.
    endpointVolume.store (std::isnan (db) ? 0.0f : std::clamp (db, -200.0f, 24.0f), std::memory_order_relaxed);
}

void HearingGuard::setCap (bool enabled, float levelDbA) noexcept
{
    capLevel.store (std::isfinite (levelDbA) ? std::clamp (levelDbA, kCapMinDbA, kCapMaxDbA) : kDefaultCapDbA, std::memory_order_relaxed);
    capEnabled.store (enabled, std::memory_order_relaxed);
}

void HearingGuard::setDoseBaseline (double fraction) noexcept
{
    pendingBaseline.store (std::isfinite (fraction) ? std::max (0.0, fraction) : 0.0, std::memory_order_relaxed);
    baselineSeq.fetch_add (1, std::memory_order_release);
}

void HearingGuard::carryFrom (const HearingGuard& previous) noexcept
{
    sensitivity.store (previous.sensitivity.load (std::memory_order_relaxed), std::memory_order_relaxed);
    endpointVolume.store (previous.endpointVolume.load (std::memory_order_relaxed), std::memory_order_relaxed);
    capLevel.store (previous.capLevel.load (std::memory_order_relaxed), std::memory_order_relaxed);
    capEnabled.store (previous.capEnabled.load (std::memory_order_relaxed), std::memory_order_relaxed);
    const uint32_t seq = previous.baselineSeq.load (std::memory_order_acquire);
    pendingBaseline.store (previous.pendingBaseline.load (std::memory_order_relaxed), std::memory_order_relaxed);
    baselineSeq.store (seq, std::memory_order_relaxed);
    seenBaselineSeq = seq;

    // The dose continues: the session dose from where the old engine's
    // stands, today's dose on the same baseline.
    const auto& pm = previous.published;
    doseCarried = pm.sessionDose.load (std::memory_order_relaxed);
    sessionEnergy = {};
    baseline = pm.doseToday.load (std::memory_order_relaxed);
    baselineMark = doseCarried;

    // The cap continues where it stands (the new window starts empty), its
    // detector seeded with the programme level the old gain was holding down.
    const float sens = sensitivity.load (std::memory_order_relaxed);
    wasKnown = std::isfinite (sens);
    lastSensitivity = sens;
    lastCapOn = wasKnown && capEnabled.load (std::memory_order_relaxed);
    lastCapDb = capLevel.load (std::memory_order_relaxed);
    gain = slowGain = std::clamp (static_cast<double> (dbToGain (pm.capGainDb.load (std::memory_order_relaxed))), 0.0, 1.0);
    planGain = 1.0;
    const float level = pm.levelDbA.load (std::memory_order_relaxed);
    if (lastCapOn && level > kSilentDbA && gain > 0.0)
    {
        slowPower.fill (std::pow (10.0, (static_cast<double> (level) - Dosimeter::kReferenceDbA) / 10.0) / (gain * gain));
        fastPre = slowPower;
    }
}

//==============================================================================
void HearingGuard::process (const AudioBlock& block) noexcept FLUB_NONBLOCKING
{
    const int n = block.numSamples;
    const int ears = std::min (2, block.numChannels);
    if (n <= 0 || ears <= 0)
        return;

    if (const uint32_t seq = baselineSeq.load (std::memory_order_acquire); seq != seenBaselineSeq)
    {
        seenBaselineSeq = seq;
        baseline = pendingBaseline.load (std::memory_order_relaxed);
        baselineMark = sessionDose();
    }

    const float sens = sensitivity.load (std::memory_order_relaxed);
    const bool known = std::isfinite (sens);
    const bool capOn = known && capEnabled.load (std::memory_order_relaxed);
    const float capDb = capLevel.load (std::memory_order_relaxed);
    // A new sensitivity (a new device), a new cap or the cap switched on:
    // the energy counted so far was held to another bound.
    if (known != wasKnown || (known && sens != lastSensitivity) || (capOn && (! lastCapOn || capDb != lastCapDb)))
        clearWindow();
    if (capOn && ! lastCapOn)
    {
        // The detector starts from the output's level: the gain was 1 (or
        // gliding back to it), so the programme and the output agree.
        preState = postState;
        slowPower = fastPre = fastPower;
    }
    wasKnown = known;
    lastSensitivity = sens;
    lastCapOn = capOn;
    lastCapDb = capDb;

    if (! known && gain >= 1.0)
    {
        publish (false); // off: not a sample touched
        return;
    }

    // Relative power (1 = 80 dB(A) SPL) per unit of A-weighted squared
    // sample: a full-scale sine's mean square is 1/2, so x 2.
    const float vol = endpointVolume.load (std::memory_order_relaxed);
    const double toRel = known && vol > kMutedVolumeDb
                             ? 2.0 * std::pow (10.0, (static_cast<double> (vol) + static_cast<double> (sens) - Dosimeter::kReferenceDbA) / 10.0)
                             : 0.0;
    const double capRel = std::pow (10.0, (static_cast<double> (capDb) - Dosimeter::kReferenceDbA) / 10.0);
    const double budget = capRel * windowSamples * std::pow (10.0, -static_cast<double> (kCapMarginDb) / 10.0);
    const double target = capRel * std::pow (10.0, -static_cast<double> (kCapTargetUnderDb) / 10.0);

    for (int pos = 0; pos < n;)
    {
        const int m = std::min ({ kChunk, n - pos, segmentLength - segmentPos });
        const bool endsSegment = segmentPos + m == segmentLength;
        const double invM = 1.0 / static_cast<double> (m), md = static_cast<double> (m);
        double g0 = gain, g1 = 1.0;

        if (capOn)
        {
            // Per ear: the programme's power for the detectors, and the exact
            // A-weighted output energy of this chunk under a linear ramp
            // g0 -> g1: the filter's zero-input response u plus g0 v plus
            // g1 w (v, w: its zero-state responses to (1 - t) x and t x),
            // E (g1) = q2 g1^2 + 2 q1 g1 + q0; r: the chunk's energy at gain
            // 1 (the reserve for ramping the next chunk to zero); uu, us for
            // a constant gain (a forced step).
            std::array<double, 2> q0 {}, q1 {}, q2 {}, r {}, uu {}, us {};
            for (int e = 0; e < ears; ++e)
            {
                const auto ei = static_cast<size_t> (e);
                const float* x = block.channel (e) + pos;
                State zi = postState[ei], zv {}, zw {};
                double sp = 0.0;
                for (int k = 0; k < m; ++k)
                {
                    const double t = static_cast<double> (k + 1) * invM, xd = x[k];
                    const double pre = tick (aw, preState[ei], xd);
                    const double u = tick (aw, zi, 0.0), v = tick (aw, zv, (1.0 - t) * xd), w = tick (aw, zw, t * xd);
                    const double base = u + g0 * v, sum = v + w;
                    sp += pre * pre;
                    q2[ei] += w * w;
                    q1[ei] += w * base;
                    q0[ei] += base * base;
                    r[ei] += sum * sum;
                    uu[ei] += u * u;
                    us[ei] += u * sum;
                }
                const double pc = sp * toRel * invM;
                chunkPower[ei] = pc;
                slowPower[ei] += (pc - slowPower[ei]) * (1.0 - std::exp (-md / (kDetectorSeconds * sampleRate)));
                fastPre[ei] += (pc - fastPre[ei]) * (1.0 - std::exp (-md / (kPlanDetectorSeconds * sampleRate)));
            }

            // The slow gain: aims the louder ear kCapTargetUnderDb under the cap.
            const double loudest = std::max (slowPower[0], slowPower[1]);
            const double gt = loudest > target ? std::sqrt (target / loudest) : 1.0;
            slowGain = gt + (slowGain - gt) * std::exp (-md / ((gt < slowGain ? kAttackSeconds : kReleaseSeconds) * sampleRate));
            if (gt >= 1.0 && slowGain > 1.0 - kGainSnap)
                slowGain = 1.0;

            // The planned gain: the programme at the slow gain may play no
            // louder than the window can sustain (plan(), at every segment),
            // and no chunk louder than spends what is left of the window
            // within kSpendSeconds, so a sudden loud start cannot use the
            // whole window in a few milliseconds and leave seconds of
            // silence to pay for it.
            if (needPlan)
            {
                plan (budget);
                needPlan = false;
            }
            double bt = 1.0;
            for (size_t e = 0; e < static_cast<size_t> (ears); ++e)
            {
                const double s2 = slowGain * slowGain;
                const double out = fastPre[e] * s2, allowed = kPlanShare * sustainable[e];
                if (out > allowed)
                    bt = std::min (bt, allowed > 0.0 ? std::sqrt (allowed / out) : 0.0);
                const double now = chunkPower[e] * s2, left = std::max (0.0, budget - windowSum[e] - segmentEnergy[e]) / (kSpendSeconds * sampleRate);
                if (now > left)
                    bt = std::min (bt, std::sqrt (left / now));
            }
            planGain = bt + (planGain - bt) * std::exp (-md / ((bt < planGain ? kPlanAttackSeconds : kPlanReleaseSeconds) * sampleRate));
            if (bt >= 1.0 && planGain > 1.0 - kGainSnap)
                planGain = 1.0;
            g1 = slowGain * planGain;

            // The hard bound: the last kCapSegments segments, this segment so
            // far and this chunk fit the cap's 5 s energy, and so does the
            // next chunk ramped to zero, with the segment that leaves the
            // window at the boundary freed.
            bool step = false;
            for (size_t e = 0; e < static_cast<size_t> (ears); ++e)
            {
                const double room = budget - windowSum[e] - segmentEnergy[e];
                const double freed = endsSegment ? ring[e][static_cast<size_t> (ringPos)] : 0.0;
                const double a = q2[e] * toRel, b = 2.0 * q1[e] * toRel, c0 = q0[e] * toRel;
                if (! (a > 0.0))
                    continue; // nothing (more) reaches this ear in this chunk
                if (c0 > room)
                {
                    step = true; // even a ramp to 0 from g0 is too much
                    continue;
                }
                const auto largest = [] (double qa, double qb, double qc) { // qa g^2 + qb g + qc <= 0
                    return (-qb + std::sqrt (std::max (0.0, qb * qb - 4.0 * qa * qc))) / (2.0 * qa);
                };
                g1 = std::min ({ g1, largest (a, b, c0 - room), largest (a + r[e] * toRel, b, c0 - room - freed) });
            }
            if (step)
            {
                // A constant gain g for the whole chunk (a step from g0):
                // r g^2 + 2 us g + uu <= room, per ear.
                double g = g1;
                for (size_t e = 0; e < static_cast<size_t> (ears); ++e)
                {
                    const double room = budget - windowSum[e] - segmentEnergy[e];
                    const double a = r[e] * toRel, b = 2.0 * us[e] * toRel, c = uu[e] * toRel - room;
                    if (! (a > 0.0))
                        continue;
                    g = std::min (g, c < 0.0 ? (-b + std::sqrt (std::max (0.0, b * b - 4.0 * a * c))) / (2.0 * a) : 0.0);
                }
                g0 = g1 = std::clamp (g, 0.0, 1.0);
                ++capSteps;
            }
            g1 = std::clamp (g1, 0.0, 1.0);
            // Whatever the bound took off is the planned gain's to give back
            // (at its release), never the slow gain's.
            if (g1 < slowGain * planGain)
                planGain = slowGain > 0.0 ? g1 / slowGain : 0.0;
        }
        else if (g0 < 1.0)
        {
            // Cap off or sensitivity gone while the cap held the level down:
            // back to 1 at the release rate, then untouched.
            g1 = 1.0 + (g0 - 1.0) * std::exp (-md / (kReleaseSeconds * sampleRate));
            if (g1 > 1.0 - kGainSnap)
                g1 = 1.0;
            slowGain = g1;
            planGain = 1.0;
        }

        if (! (g0 == 1.0 && g1 == 1.0))
        {
            const double dg = (g1 - g0) * invM;
            for (int c = 0; c < block.numChannels; ++c)
            {
                float* d = block.channel (c) + pos;
                for (int k = 0; k < m; ++k)
                    d[k] = static_cast<float> (static_cast<double> (d[k]) * (g0 + dg * static_cast<double> (k + 1)));
            }
        }
        gain = g1;

        if (known)
        {
            // What reaches the ear (after the cap): the window, the dose and
            // the readings.
            for (size_t e = 0; e < static_cast<size_t> (ears); ++e)
            {
                const float* y = block.channel (static_cast<int> (e)) + pos;
                double sum = 0.0;
                for (int k = 0; k < m; ++k)
                {
                    const double a = tick (aw, postState[e], y[k]);
                    sum += a * a;
                }
                sum *= toRel;
                segmentEnergy[e] += sum;
                sessionEnergy[e] += sum;
                fastPower[e] += (sum * invM - fastPower[e]) * (1.0 - std::exp (-md / (kFastSeconds * sampleRate)));
            }
        }

        segmentPos += m;
        if (endsSegment)
        {
            for (size_t e = 0; e < 2; ++e)
            {
                ring[e][static_cast<size_t> (ringPos)] = segmentEnergy[e];
                segmentEnergy[e] = 0.0;
                double sum = 0.0;
                for (double v : ring[e])
                    sum += v;
                windowSum[e] = sum;
            }
            ringPos = (ringPos + 1) % kCapSegments;
            segmentPos = 0;
            if (capOn)
                plan (budget);
        }
        pos += m;
    }

    if (known)
        sessionSamples += n;
    publish (known);
}

void HearingGuard::publish (bool known) noexcept
{
    constexpr auto rx = std::memory_order_relaxed;
    published.known.store (known, rx);
    if (known)
    {
        const double windowLength = static_cast<double> (kCapSegments) * static_cast<double> (segmentLength);
        published.levelDbA.store (toDbA (std::max (fastPower[0], fastPower[1])), rx);
        published.leq5sDbA.store (toDbA (std::max (windowSum[0], windowSum[1]) / windowLength), rx);
        published.sessionLeqDbA.store (sessionSamples > 0.0 ? toDbA (std::max (sessionEnergy[0], sessionEnergy[1]) / sessionSamples)
                                                            : kSilentDbA,
                                       rx);
    }
    else
    {
        published.levelDbA.store (HearingMeters::kUnknown, rx);
        published.leq5sDbA.store (HearingMeters::kUnknown, rx);
        published.sessionLeqDbA.store (HearingMeters::kUnknown, rx);
    }
    const double session = sessionDose();
    published.sessionDose.store (session, rx);
    published.doseToday.store (baseline + session - baselineMark, rx);
    published.capGainDb.store (gain > 0.0 ? static_cast<float> (20.0 * std::log10 (gain)) : -200.0f, rx);
    published.capActive.store (gain < 1.0, rx);
    published.capSteps.store (capSteps, rx);
}
} // namespace flub
