#include "flub/dsp/LoudnessContour.h"

#include "flub/common/Math.h"
#include "flub/dsp/DeviceCorrection.h"

#include <algorithm>
#include <cmath>

namespace flub
{
namespace iso226
{
const std::array<double, kNumFrequencies> kFrequencies { 20.0,   25.0,   31.5,   40.0,   50.0,   63.0,   80.0,   100.0,
                                                          125.0,  160.0,  200.0,  250.0,  315.0,  400.0,  500.0,  630.0,
                                                          800.0,  1000.0, 1250.0, 1600.0, 2000.0, 2500.0, 3150.0, 4000.0,
                                                          5000.0, 6300.0, 8000.0, 10000.0, 12500.0 };

namespace
{
// ISO 226:2023 Table 1: exponent for loudness perception alpha_f, magnitude
// of the linear transfer function normalised at 1 kHz L_U (dB) and threshold
// of hearing T_f (dB).
constexpr std::array<double, kNumFrequencies> kAlpha { 0.635, 0.602, 0.569, 0.537, 0.509, 0.482, 0.456, 0.433, 0.412, 0.391,
                                                       0.373, 0.357, 0.343, 0.330, 0.320, 0.311, 0.303, 0.300, 0.295, 0.292,
                                                       0.290, 0.290, 0.289, 0.289, 0.289, 0.293, 0.303, 0.323, 0.354 };
constexpr std::array<double, kNumFrequencies> kLu { -31.5, -27.2, -23.1, -19.3, -16.1, -13.1, -10.4, -8.2, -6.3, -4.6,
                                                    -3.2,  -2.1,  -1.2,  -0.5,  0.0,   0.4,   0.5,   0.0,  -2.7, -4.2,
                                                    -1.2,  1.4,   2.3,   1.0,   -1.7,  -6.2,  -10.8, -10.2, -3.6 };
constexpr std::array<double, kNumFrequencies> kTf { 78.1, 68.7, 59.5, 51.1, 44.0, 37.5, 31.5, 26.5, 22.1, 17.9,
                                                    14.4, 11.4, 8.6,  6.2,  4.4,  3.0,  2.2,  2.4,  3.5,  1.7,
                                                    -1.3, -4.2, -6.0, -5.4, -1.5, 6.0,  12.6, 13.9, 12.3 };
// Reference sound pressure term (4e-10)^(0.3 - alpha_f) (Formula (1): p_a = 20 uPa,
// T_r = 4e-10 in its normalised units) and the 1 kHz threshold term 10^0.072.
constexpr double kTr = 4.0e-10, kThresholdTerm = 1.1803206356517297; // 10^0.072
} // namespace

double splDb (int index, double phon) noexcept FLUB_NONBLOCKING
{
    const auto i = static_cast<size_t> (std::clamp (index, 0, kNumFrequencies - 1));
    const double a = kAlpha[i];
    const double term = std::pow (kTr, 0.3 - a) * (std::pow (10.0, 0.03 * phon) - kThresholdTerm)
                        + std::pow (10.0, a * (kTf[i] + kLu[i]) / 10.0);
    return 10.0 / a * std::log10 (std::max (term, 1.0e-300)) - kLu[i];
}

double relativeGainDb (int index, double referencePhon, double levelDb) noexcept FLUB_NONBLOCKING
{
    return splDb (index, referencePhon + levelDb) - splDb (index, referencePhon) - levelDb;
}
} // namespace iso226

namespace
{
/** |H|^2 of an SVF section at tan (pi f / fs) = t (Svf.h's bilinear mapping). */
inline double sectionPower (const SvfCoeffs& c, double t) noexcept
{
    const double w = t / c.g, w2 = w * w;
    const double m0 = c.m0, m1 = c.m1, m2 = c.m2;
    const double nr = m0 * (1.0 - w2) + m2, ni = (m0 * c.k + m1) * w;
    const double dr = 1.0 - w2, di = c.k * w;
    return (nr * nr + ni * ni) / std::max (dr * dr + di * di, 1.0e-300);
}

constexpr double kMaxSectionGainDb = 30.0;

template <size_t N>
bool sameCoeffs (const std::array<SvfCoeffs, N>& a, const std::array<SvfCoeffs, N>& b) noexcept
{
    for (size_t i = 0; i < N; ++i)
        if (a[i].a1 != b[i].a1 || a[i].a2 != b[i].a2 || a[i].a3 != b[i].a3 || a[i].m0 != b[i].m0 || a[i].m1 != b[i].m1 || a[i].m2 != b[i].m2)
            return false;
    return true;
}
} // namespace

float LoudnessContour::effectiveLevelDb (const LoudnessContourParams& p) noexcept
{
    const float level = std::isfinite (p.levelDb) ? std::clamp (p.levelDb, kMinLevelDb, 0.0f) : 0.0f;
    const float ref = std::isfinite (p.referencePhon) ? std::clamp (p.referencePhon, kMinPhon, 100.0f) : 80.0f;
    return std::max (level, kMinPhon - ref);
}

void LoudnessContour::prepare (const ProcessSpec& spec)
{
    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 48000.0;
    numChannels = std::clamp (spec.numChannels, 1, kMaxChannels);
    for (int i = 0; i < iso226::kNumFrequencies; ++i)
    {
        const double f = iso226::kFrequencies[static_cast<size_t> (i)];
        tanAt[static_cast<size_t> (i)] = std::tan (kPi * std::min (f, 0.4999 * sampleRate) / sampleRate);
        pointActive[static_cast<size_t> (i)] = f <= 0.45 * sampleRate;
    }
    for (int s = 0; s < kNumSections; ++s)
        sectionActive[static_cast<size_t> (s)] = kSections[static_cast<size_t> (s)].hz <= 0.4 * sampleRate;

    // Unit-gain (1 dB) basis responses B (points x sections) and the
    // least-squares pseudo-inverse (B^T B)^-1 B^T over the active ones.
    std::array<std::array<double, kNumSections>, iso226::kNumFrequencies> basis {};
    for (int s = 0; s < kNumSections; ++s)
    {
        if (! sectionActive[static_cast<size_t> (s)])
            continue;
        const Section& sec = kSections[static_cast<size_t> (s)];
        const SvfCoeffs c = SvfCoeffs::make (sec.type, sec.hz, sec.q, 1.0, sampleRate);
        for (int i = 0; i < iso226::kNumFrequencies; ++i)
            if (pointActive[static_cast<size_t> (i)])
                basis[static_cast<size_t> (i)][static_cast<size_t> (s)] = 10.0 * std::log10 (sectionPower (c, tanAt[static_cast<size_t> (i)]));
    }
    // Normal equations, augmented with B^T: [M | B^T], Gauss-Jordan with
    // partial pivoting; an inactive section gets an identity row (gain 0).
    std::array<std::array<double, kNumSections + iso226::kNumFrequencies>, kNumSections> m {};
    for (int r = 0; r < kNumSections; ++r)
    {
        auto& row = m[static_cast<size_t> (r)];
        if (! sectionActive[static_cast<size_t> (r)])
        {
            row[static_cast<size_t> (r)] = 1.0;
            continue;
        }
        for (int c = 0; c < kNumSections; ++c)
            for (int i = 0; i < iso226::kNumFrequencies; ++i)
                row[static_cast<size_t> (c)] += basis[static_cast<size_t> (i)][static_cast<size_t> (r)] * basis[static_cast<size_t> (i)][static_cast<size_t> (c)];
        for (int i = 0; i < iso226::kNumFrequencies; ++i)
            row[static_cast<size_t> (kNumSections + i)] = basis[static_cast<size_t> (i)][static_cast<size_t> (r)];
    }
    for (int col = 0; col < kNumSections; ++col)
    {
        int pivot = col;
        for (int r = col + 1; r < kNumSections; ++r)
            if (std::abs (m[static_cast<size_t> (r)][static_cast<size_t> (col)]) > std::abs (m[static_cast<size_t> (pivot)][static_cast<size_t> (col)]))
                pivot = r;
        std::swap (m[static_cast<size_t> (col)], m[static_cast<size_t> (pivot)]);
        const double d = m[static_cast<size_t> (col)][static_cast<size_t> (col)];
        if (std::abs (d) < 1.0e-12)
            continue;
        for (auto& v : m[static_cast<size_t> (col)])
            v /= d;
        for (int r = 0; r < kNumSections; ++r)
        {
            if (r == col)
                continue;
            const double factor = m[static_cast<size_t> (r)][static_cast<size_t> (col)];
            for (size_t c = 0; c < m[static_cast<size_t> (r)].size(); ++c)
                m[static_cast<size_t> (r)][c] -= factor * m[static_cast<size_t> (col)][c];
        }
    }
    for (int s = 0; s < kNumSections; ++s)
        for (int i = 0; i < iso226::kNumFrequencies; ++i)
            pinv[static_cast<size_t> (s)][static_cast<size_t> (i)]
                = sectionActive[static_cast<size_t> (s)] && pointActive[static_cast<size_t> (i)] ? m[static_cast<size_t> (s)][static_cast<size_t> (kNumSections + i)] : 0.0;

    designValid = false;
    designPending = true;
    designHoldoff = 0;
    design();
    reset();
    snapPending = true; // the first design after prepare() applies without a glide
}

void LoudnessContour::reset() noexcept FLUB_NONBLOCKING
{
    for (auto& section : state)
        for (auto& s : section)
            s.reset();
    gain = targetGain;
    trim = targetTrim;
    untilUpdate = 0;
    updateCoefficients();
    coeffsFrom = coeffs;
    trimFrom = trim;
    running = trim != 0.0f || std::any_of (gain.begin(), gain.end(), [] (float g) { return g != 0.0f; });
    appliedTrimDb.store (trim, std::memory_order_relaxed);
}

void LoudnessContour::setParams (const LoudnessContourParams& p) noexcept FLUB_NONBLOCKING
{
    params = p;
    const auto changed = [] (float a, float b) { return std::abs (a - b) > 1.0e-4f; };
    if (! designValid || p.enabled != designed.enabled
        || (p.enabled
            && (changed (effectiveLevelDb (p), effectiveLevelDb (designed)) || changed (p.referencePhon, designed.referencePhon)
                || changed (p.maxLiftDb, designed.maxLiftDb))))
        designPending = true;
    if (designPending && designHoldoff <= 0)
    {
        design();
        designHoldoff = std::max (1, static_cast<int> (kDesignIntervalMs * 0.001 * sampleRate));
    }
    const float allowance = std::isfinite (p.allowanceDb) ? std::max (0.0f, p.allowanceDb) : 0.0f;
    targetTrim = designed.enabled ? -std::max (0.0f, predictedLiftDb - allowance) : 0.0f;
    if (snapPending && ! designPending)
    {
        snapPending = false;
        gain = targetGain;
        trim = targetTrim;
        updateCoefficients();
        coeffsFrom = coeffs;
        trimFrom = trim;
        appliedTrimDb.store (trim, std::memory_order_relaxed);
        running = trim != 0.0f || std::any_of (gain.begin(), gain.end(), [] (float g) { return g != 0.0f; });
    }
    // While enabled the filters run even when flat, so their state follows
    // the signal and a lift that starts later starts from it.
    if (designed.enabled || targetTrim != trim || targetGain != gain)
        running = true;
}

void LoudnessContour::design() noexcept FLUB_NONBLOCKING
{
    designed = params;
    designValid = true;
    designPending = false;
    if (! params.enabled)
    {
        targetGain.fill (0.0f);
        predictedLiftDb = 0.0f;
        return;
    }
    const double level = effectiveLevelDb (params);
    const double reference = std::clamp (static_cast<double> (params.referencePhon), static_cast<double> (kMinPhon), 100.0);
    const double cap = std::isfinite (params.maxLiftDb) ? std::max (0.0, static_cast<double> (params.maxLiftDb)) : 0.0;
    std::array<double, iso226::kNumFrequencies> want {};
    for (int i = 0; i < iso226::kNumFrequencies; ++i)
        if (pointActive[static_cast<size_t> (i)])
            want[static_cast<size_t> (i)] = std::min (iso226::relativeGainDb (i, reference, level), cap);

    std::array<float, kNumSections> g {};
    const auto step = [this, &g] (const std::array<double, iso226::kNumFrequencies>& residual) {
        for (int s = 0; s < kNumSections; ++s)
        {
            double sum = static_cast<double> (g[static_cast<size_t> (s)]);
            for (int i = 0; i < iso226::kNumFrequencies; ++i)
                sum += pinv[static_cast<size_t> (s)][static_cast<size_t> (i)] * residual[static_cast<size_t> (i)];
            g[static_cast<size_t> (s)] = static_cast<float> (std::clamp (sum, -kMaxSectionGainDb, kMaxSectionGainDb));
        }
    };
    step (want); // linear fit on the unit-gain basis
    for (int iteration = 0; iteration < 3; ++iteration)
    {
        std::array<SvfCoeffs, kNumSections> c {};
        for (int s = 0; s < kNumSections; ++s)
        {
            const Section& sec = kSections[static_cast<size_t> (s)];
            c[static_cast<size_t> (s)] = SvfCoeffs::make (sec.type, sec.hz, sec.q, g[static_cast<size_t> (s)], sampleRate);
        }
        std::array<double, iso226::kNumFrequencies> residual {};
        for (int i = 0; i < iso226::kNumFrequencies; ++i)
        {
            if (! pointActive[static_cast<size_t> (i)])
                continue;
            double power = 1.0;
            for (int s = 0; s < kNumSections; ++s)
                if (sectionActive[static_cast<size_t> (s)])
                    power *= sectionPower (c[static_cast<size_t> (s)], tanAt[static_cast<size_t> (i)]);
            residual[static_cast<size_t> (i)] = want[static_cast<size_t> (i)] - 10.0 * std::log10 (std::max (power, 1.0e-30));
        }
        step (residual);
    }
    for (int s = 0; s < kNumSections; ++s)
        targetGain[static_cast<size_t> (s)] = sectionActive[static_cast<size_t> (s)] ? g[static_cast<size_t> (s)] : 0.0f;

    const auto p = headroom::predictMaxBoostWith ([this] (double f) { return targetLiftDb (f); }, headroom::Weighting::Programme, 20.0,
                                                  std::min (20000.0, 0.49 * sampleRate));
    predictedLiftDb = static_cast<float> (std::max (0.0, p.maxBoostDb));
}

double LoudnessContour::targetLiftDb (double freqHz) const noexcept FLUB_NONBLOCKING
{
    const double t = std::tan (kPi * std::clamp (freqHz, 0.0, 0.4999 * sampleRate) / sampleRate);
    double power = 1.0;
    for (int s = 0; s < kNumSections; ++s)
    {
        const float g = targetGain[static_cast<size_t> (s)];
        if (g == 0.0f)
            continue;
        const Section& sec = kSections[static_cast<size_t> (s)];
        power *= sectionPower (SvfCoeffs::make (sec.type, sec.hz, sec.q, g, sampleRate), t);
    }
    return 10.0 * std::log10 (std::max (power, 1.0e-30));
}

int LoudnessContour::getTargetSections (SvfCoeffs* out) const noexcept FLUB_NONBLOCKING
{
    int n = 0;
    for (int s = 0; s < kNumSections; ++s)
    {
        const float g = targetGain[static_cast<size_t> (s)];
        if (g == 0.0f)
            continue;
        const Section& sec = kSections[static_cast<size_t> (s)];
        out[n++] = SvfCoeffs::make (sec.type, sec.hz, sec.q, g, sampleRate);
    }
    return n;
}

void LoudnessContour::updateCoefficients() noexcept
{
    for (int s = 0; s < kNumSections; ++s)
    {
        const Section& sec = kSections[static_cast<size_t> (s)];
        coeffs[static_cast<size_t> (s)] = SvfCoeffs::make (sec.type, sec.hz, sec.q, gain[static_cast<size_t> (s)], sampleRate);
    }
    const double t = std::tan (kPi * 50.0 / sampleRate);
    double power = 1.0;
    for (const auto& c : coeffs)
        power *= sectionPower (c, t);
    appliedLift50.store (static_cast<float> (10.0 * std::log10 (std::max (power, 1.0e-30))), std::memory_order_relaxed);
}

void LoudnessContour::process (const AudioBlock& block) noexcept FLUB_NONBLOCKING
{
    const int n = block.numSamples;
    designHoldoff = std::max (0, designHoldoff - n);
    if (designPending && designHoldoff <= 0)
    {
        design(); // the latest input, held off by the interval
        designHoldoff = std::max (1, static_cast<int> (kDesignIntervalMs * 0.001 * sampleRate));
        setParams (params);
    }
    if (! running)
        return;

    const float maxStep = static_cast<float> (kSlewDbPerSecond * kUpdateSamples / sampleRate);
    const int channels = std::min (block.numChannels, numChannels);
    int start = 0;
    while (start < n)
    {
        if (untilUpdate <= 0)
        {
            const auto glide = [maxStep] (float& value, float target) {
                value = std::abs (target - value) <= maxStep ? target : value + std::copysign (maxStep, target - value);
            };
            coeffsFrom = coeffs;
            trimFrom = trim;
            for (int s = 0; s < kNumSections; ++s)
                glide (gain[static_cast<size_t> (s)], targetGain[static_cast<size_t> (s)]);
            glide (trim, targetTrim);
            updateCoefficients();
            untilUpdate = kUpdateSamples;
        }
        // Coefficients and trim move linearly from the previous update's
        // values to this one's over the kUpdateSamples, sample by sample: a
        // step of even 0.04 dB would be a small click on a steady bass tone.
        const int len = std::min (n - start, untilUpdate);
        const int done = kUpdateSamples - untilUpdate;
        const bool gliding = trimFrom != trim || ! sameCoeffs (coeffsFrom, coeffs);
        const float g0 = dbToGain (trimFrom), g1 = dbToGain (trim);
        for (int i = 0; i < len; ++i)
        {
            const float t = static_cast<float> (done + i + 1) / static_cast<float> (kUpdateSamples);
            std::array<SvfCoeffs, kNumSections> c = coeffs;
            if (gliding)
                for (int s = 0; s < kNumSections; ++s)
                {
                    const SvfCoeffs& a = coeffsFrom[static_cast<size_t> (s)];
                    const SvfCoeffs& b = coeffs[static_cast<size_t> (s)];
                    SvfCoeffs& m = c[static_cast<size_t> (s)];
                    m.a1 = a.a1 + t * (b.a1 - a.a1);
                    m.a2 = a.a2 + t * (b.a2 - a.a2);
                    m.a3 = a.a3 + t * (b.a3 - a.a3);
                    m.m0 = a.m0 + t * (b.m0 - a.m0);
                    m.m1 = a.m1 + t * (b.m1 - a.m1);
                    m.m2 = a.m2 + t * (b.m2 - a.m2);
                }
            const float g = gliding ? g0 + t * (g1 - g0) : g1;
            for (int ch = 0; ch < channels; ++ch)
            {
                float* x = block.channel (ch) + start + i;
                float y = *x;
                for (int s = 0; s < kNumSections; ++s)
                    y = svfTick (c[static_cast<size_t> (s)], state[static_cast<size_t> (s)][static_cast<size_t> (ch)], y);
                *x = y * g;
            }
        }
        untilUpdate -= len;
        start += len;
    }
    appliedTrimDb.store (trim, std::memory_order_relaxed);

    const bool atTarget = trim == targetTrim && gain == targetGain;
    if (atTarget && ! designed.enabled && trim == 0.0f && std::all_of (gain.begin(), gain.end(), [] (float g) { return g == 0.0f; }))
    {
        running = false; // flat: idle from here on (see the header comment)
        for (auto& section : state)
            for (auto& s : section)
                s.reset();
    }
}
} // namespace flub
