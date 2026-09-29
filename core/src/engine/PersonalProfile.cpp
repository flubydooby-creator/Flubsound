#include "flub/engine/PersonalProfile.h"

#include "flub/common/Math.h"
#include "flub/dsp/Svf.h"

#include <algorithm>
#include <cmath>

namespace flub
{
namespace
{
using Targets = std::array<std::array<float, PersonalProfile::kNumBands>, 2>;

// The Newton solve of the band gains (designPersonalCurve).
constexpr int kSolveIterations = 24;
constexpr double kSolveToleranceDb = 1.0e-4;
constexpr double kSolveLimitDb = 36.0; // a band's bell may go beyond the targets to meet them (at most 28 dB in range), not further
constexpr double kSolveStepDb = 0.01;  // the Jacobian's finite difference
// DeviceCorrection's own automatic preamp stays at 0 dB: the stage sets its
// reservation itself (it is programme-weighted, not flat).
constexpr float kNoAutomaticPreamp = 1000.0f;

float clampFinite (float v, float lo, float hi) noexcept
{
    return std::isfinite (v) ? std::clamp (v, lo, hi) : 0.0f;
}

/** Lowers the higher of a / b so they differ by at most the cap. */
void capDifference (float& a, float& b) noexcept
{
    constexpr float cap = PersonalProfile::kMaxEarDifferenceDb;
    if (a - b > cap)
        a = b + cap;
    else if (b - a > cap)
        b = a + cap;
}

/** B_e and T_e,b of a sanitised, enabled profile (see the header comment). */
void computeTargets (const PersonalProfile& s, std::array<float, 2>& broadband, Targets& targets) noexcept
{
    broadband[0] = s.gainDb[0] - std::max (0.0f, s.balanceDb);
    broadband[1] = s.gainDb[1] + std::min (0.0f, s.balanceDb);
    capDifference (broadband[0], broadband[1]);
    for (size_t b = 0; b < static_cast<size_t> (PersonalProfile::kNumBands); ++b)
    {
        for (size_t e = 0; e < 2; ++e)
            targets[e][b] = std::clamp (broadband[e] + s.bandDb[e][b], PersonalProfile::kMinTargetDb, PersonalProfile::kMaxBoostDb);
        capDifference (targets[0][b], targets[1][b]);
    }
}

double bellDb (size_t band, double gainDb, double freqHz, double sampleRate) noexcept
{
    if (gainDb == 0.0)
        return 0.0;
    return SvfCoeffs::make (FilterType::Bell, PersonalProfile::kBandHz[band], PersonalProfile::kBandQ[band], gainDb, sampleRate)
        .magnitudeDb (freqHz, sampleRate);
}

/** Solves A x = b in place (n <= 8, partial pivoting); false if singular. */
template <size_t N>
bool solveLinear (std::array<std::array<double, N>, N>& a, std::array<double, N>& b, size_t n) noexcept
{
    for (size_t col = 0; col < n; ++col)
    {
        size_t pivot = col;
        for (size_t r = col + 1; r < n; ++r)
            if (std::abs (a[r][col]) > std::abs (a[pivot][col]))
                pivot = r;
        if (std::abs (a[pivot][col]) < 1.0e-9)
            return false;
        std::swap (a[pivot], a[col]);
        std::swap (b[pivot], b[col]);
        for (size_t r = col + 1; r < n; ++r)
        {
            const double f = a[r][col] / a[col][col];
            for (size_t c = col; c < n; ++c)
                a[r][c] -= f * a[col][c];
            b[r] -= f * b[col];
        }
    }
    for (size_t k = n; k-- > 0;)
    {
        double sum = b[k];
        for (size_t c = k + 1; c < n; ++c)
            sum -= a[k][c] * b[c];
        b[k] = sum / a[k][k];
    }
    return true;
}

/** The bell gains of the first `n` bands so that their summed response at
    each band frequency is `target` (dB): Newton on the exact digital
    responses, the bells' gains clamped to +-kSolveLimitDb. */
std::array<double, PersonalProfile::kNumBands> solveBandGains (const std::array<double, PersonalProfile::kNumBands>& target, size_t n,
                                                               double sampleRate) noexcept
{
    constexpr size_t kBands = PersonalProfile::kNumBands;
    std::array<double, kBands> g = target;
    for (int iteration = 0; iteration < kSolveIterations; ++iteration)
    {
        std::array<double, kBands> err {};
        double worst = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            double r = 0.0;
            for (size_t j = 0; j < n; ++j)
                r += bellDb (j, g[j], PersonalProfile::kBandHz[i], sampleRate);
            err[i] = target[i] - r;
            worst = std::max (worst, std::abs (err[i]));
        }
        if (worst < kSolveToleranceDb)
            break;
        std::array<std::array<double, kBands>, kBands> jacobian {};
        for (size_t j = 0; j < n; ++j)
            for (size_t i = 0; i < n; ++i)
                jacobian[i][j] = (bellDb (j, g[j] + kSolveStepDb, PersonalProfile::kBandHz[i], sampleRate)
                                  - bellDb (j, g[j], PersonalProfile::kBandHz[i], sampleRate))
                                 / kSolveStepDb;
        if (! solveLinear (jacobian, err, n))
            break;
        for (size_t j = 0; j < n; ++j)
            g[j] = std::clamp (g[j] + err[j], -kSolveLimitDb, kSolveLimitDb);
    }
    return g;
}
} // namespace

// =============================================================================
// PersonalProfile
// =============================================================================
PersonalProfile PersonalProfile::sanitised() const noexcept
{
    PersonalProfile s = *this;
    for (size_t e = 0; e < 2; ++e)
    {
        s.gainDb[e] = clampFinite (gainDb[e], -kGainRangeDb, kGainRangeDb);
        for (size_t b = 0; b < static_cast<size_t> (kNumBands); ++b)
            s.bandDb[e][b] = clampFinite (bandDb[e][b], -kBandRangeDb, kBandRangeDb);
    }
    s.balanceDb = clampFinite (balanceDb, -kBalanceRangeDb, kBalanceRangeDb);
    return s;
}

float PersonalProfile::broadbandDb (int ear) const noexcept
{
    if (! enabled || ear < 0 || ear > 1)
        return 0.0f;
    std::array<float, 2> broadband {};
    Targets targets {};
    computeTargets (sanitised(), broadband, targets);
    return broadband[static_cast<size_t> (ear)];
}

float PersonalProfile::targetDb (int ear, int band) const noexcept
{
    if (! enabled || ear < 0 || ear > 1 || band < 0 || band >= kNumBands)
        return 0.0f;
    std::array<float, 2> broadband {};
    Targets targets {};
    computeTargets (sanitised(), broadband, targets);
    return targets[static_cast<size_t> (ear)][static_cast<size_t> (band)];
}

bool PersonalProfile::isNeutral() const noexcept
{
    if (! enabled)
        return true;
    std::array<float, 2> broadband {};
    Targets targets {};
    computeTargets (sanitised(), broadband, targets);
    for (size_t e = 0; e < 2; ++e)
    {
        if (broadband[e] != 0.0f)
            return false;
        for (float t : targets[e])
            if (t != 0.0f)
                return false;
    }
    return true;
}

CorrectionCurve designPersonalCurve (const PersonalProfile& profile, double sampleRate)
{
    CorrectionCurve curve;
    if (profile.isNeutral() || ! (sampleRate > 0.0))
        return curve;
    std::array<float, 2> broadband {};
    Targets targets {};
    computeTargets (profile.sanitised(), broadband, targets);

    // Bands at or above kMaxBandFraction fs are left out (kBandHz ascends).
    size_t used = 0;
    while (used < static_cast<size_t> (PersonalProfile::kNumBands) && PersonalProfile::kBandHz[used] < PersonalProfile::kMaxBandFraction * sampleRate)
        ++used;

    for (size_t e = 0; e < 2; ++e)
    {
        curve.gainDb[e] = broadband[e];
        std::array<double, PersonalProfile::kNumBands> residual {};
        bool any = false;
        for (size_t b = 0; b < used; ++b)
        {
            residual[b] = static_cast<double> (targets[e][b]) - static_cast<double> (broadband[e]);
            any = any || residual[b] != 0.0;
        }
        if (! any)
            continue;
        const auto gains = solveBandGains (residual, used, sampleRate);
        for (size_t b = 0; b < used; ++b)
        {
            const auto gain = static_cast<float> (gains[b]);
            if (gain == 0.0f)
                continue;
            CorrectionFilter f;
            f.type = CorrectionFilterType::Peak;
            f.frequency = static_cast<float> (PersonalProfile::kBandHz[b]);
            f.q = static_cast<float> (PersonalProfile::kBandQ[b]);
            f.gainDb = gain;
            f.channels = e == 0 ? CorrectionFilter::kLeft : CorrectionFilter::kRight;
            curve.add (f);
        }
    }
    return curve;
}

float personalReservationDb (const CorrectionCurve& curve, double sampleRate) noexcept
{
    if (curve.isEmpty() || ! (sampleRate > 0.0))
        return 0.0f;
    const auto louderEar = [&curve, sampleRate] (double f) {
        return std::max (curve.responseDb (0, f, sampleRate), curve.responseDb (1, f, sampleRate));
    };
    const auto prediction = headroom::predictMaxBoostWith (louderEar, headroom::Weighting::Programme, 20.0, std::min (20000.0, 0.49 * sampleRate));
    return headroom::preampDb (prediction, 0.0);
}

// =============================================================================
// PersonalEarStage
// =============================================================================
DeviceCorrectionSettings PersonalEarStage::settingsFor (const CorrectionCurve& c, float reservation, bool invert) const noexcept
{
    DeviceCorrectionSettings s;
    s.curve = c;
    s.allowanceDb = kNoAutomaticPreamp;
    for (size_t e = 0; e < 2; ++e)
        s.curve.gainDb[e] = c.isEmpty() ? 0.0f : c.gainDb[e] + reservation;
    if (invert)
    {
        // A bell with the negated gain is the exact reciprocal of the bell
        // (Svf.h: (s^2 + (A/q) s + 1) / (s^2 + s/(qA) + 1) with A -> 1/A),
        // and so is a negated gain: the inverse undoes the stage exactly.
        for (int i = 0; i < s.curve.numFilters; ++i)
            s.curve.filters[static_cast<size_t> (i)].gainDb = -s.curve.filters[static_cast<size_t> (i)].gainDb;
        for (auto& g : s.curve.gainDb)
            g = -g + 0.0f; // + 0: never -0
    }
    return s;
}

void PersonalEarStage::design()
{
    curve = designPersonalCurve (profile, sampleRate);
    // Only ahead of the dynamics: after the maximizer the per-ear limiters
    // hold the ceiling, and the good ear keeps its level.
    reservationDb = placement == PersonalPlacement::BeforeCompressor ? personalReservationDb (curve, sampleRate) : 0.0f;
}

void PersonalEarStage::prepare (const ProcessSpec& spec, PersonalPlacement where)
{
    placement = where;
    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 48000.0;
    const ProcessSpec stereo { sampleRate, std::max (1, spec.maxBlockSize), 2 };
    forward.prepare (stereo);
    inverse.prepare (stereo);
    if (placement == PersonalPlacement::AfterMaximizer)
        for (auto& limiter : earLimiter)
        {
            limiter.setLookaheadMs (kLimiterLookaheadMs);
            limiter.setTruePeakDetection (true);
            limiter.setEnvelope ({ true, true, true }); // the maximizer's LF-safe envelope
            limiter.prepare ({ sampleRate, stereo.maxBlockSize, 1 });
        }
    setProfileNow (profile);
}

void PersonalEarStage::reset() noexcept FLUB_NONBLOCKING
{
    forward.reset();
    inverse.reset();
    inverseRan = false;
    if (placement == PersonalPlacement::AfterMaximizer)
        for (auto& limiter : earLimiter)
            limiter.reset();
}

bool PersonalEarStage::setProfile (const PersonalProfile& p)
{
    profile = p.sanitised();
    design();
    const bool sentForward = forward.setSettings (settingsFor (curve, reservationDb, false));
    const bool sentInverse = inverse.setSettings (settingsFor (curve, reservationDb, true));
    return sentForward && sentInverse;
}

bool PersonalEarStage::retryPending()
{
    const bool sentForward = forward.retryPending();
    const bool sentInverse = inverse.retryPending();
    return sentForward && sentInverse;
}

void PersonalEarStage::setProfileNow (const PersonalProfile& p)
{
    profile = p.sanitised();
    design();
    forward.setSettingsNow (settingsFor (curve, reservationDb, false));
    inverse.setSettingsNow (settingsFor (curve, reservationDb, true));
}

void PersonalEarStage::process (const AudioBlock& stereo) noexcept FLUB_NONBLOCKING
{
    const AudioBlock st = stereo.firstChannels (2);
    forward.process (st);
    if (! inverseRan)
    {
        AudioBlock none; // no samples: takes the newest design off the ring
        none.numChannels = 2;
        inverse.process (none);
    }
    inverseRan = false;
    if (placement != PersonalPlacement::AfterMaximizer)
        return;
    for (int e = 0; e < st.numChannels; ++e)
    {
        AudioBlock ear;
        ear.numChannels = 1;
        ear.numSamples = st.numSamples;
        ear.ch[0] = st.channel (e);
        earLimiter[static_cast<size_t> (e)].process (ear);
    }
}

void PersonalEarStage::processInverse (const AudioBlock& copy) noexcept FLUB_NONBLOCKING
{
    inverse.process (copy.firstChannels (2));
    inverseRan = true;
}

void PersonalEarStage::setCeilingDb (float ceilingDb) noexcept FLUB_NONBLOCKING
{
    if (placement != PersonalPlacement::AfterMaximizer)
        return;
    for (auto& limiter : earLimiter)
        if (limiter.getParams().ceilingDb != ceilingDb)
        {
            LimiterParams p = limiter.getParams();
            p.ceilingDb = ceilingDb;
            limiter.setParams (p);
        }
}

int PersonalEarStage::latencySamples() const noexcept
{
    return placement == PersonalPlacement::AfterMaximizer ? earLimiter[0].latencySamples() : 0;
}
} // namespace flub
