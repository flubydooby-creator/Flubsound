// Flubsound Pro - per-output-device correction and the headroom predictor
// (docs/11 E15 / E11). See DeviceCorrection.h for the signal flow and the
// threading contract.
#include "flub/dsp/DeviceCorrection.h"

#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace flub
{
namespace
{
constexpr size_t kPendingDesigns = 4; // hand-off ring: more changes than this between two callbacks wait

// Filter state below -300 dB re full scale is flushed after each block (no
// subnormal crawl without FTZ); a non-finite state restarts from rest, so a
// NaN / Inf that reached the input ends with the block.
constexpr float kStateFloor = 1.0e-15f;

float cleanState (float v) noexcept
{
    return (std::abs (v) < kStateFloor || ! std::isfinite (v)) ? 0.0f : v;
}

bool actsOn (const CorrectionFilter& f, int channel) noexcept
{
    return (f.channels & (channel == 0 ? CorrectionFilter::kLeft : CorrectionFilter::kRight)) != 0;
}

bool hasGain (CorrectionFilterType t) noexcept
{
    return t == CorrectionFilterType::Peak || t == CorrectionFilterType::LowShelf || t == CorrectionFilterType::HighShelf;
}

/** Maximum of a unimodal function on [a, b] (golden-section search). */
template <typename Fn>
std::pair<double, double> goldenMax (Fn&& fn, double a, double b)
{
    constexpr double kInvPhi = 0.6180339887498949;
    constexpr int kIterations = 40; // interval shrinks by 0.618^40 ~ 4e-9 (of 1/6 octave)
    double c = b - kInvPhi * (b - a), d = a + kInvPhi * (b - a);
    double fc = fn (c), fd = fn (d);
    for (int i = 0; i < kIterations; ++i)
    {
        if (fc >= fd)
        {
            b = d;
            d = c;
            fd = fc;
            c = b - kInvPhi * (b - a);
            fc = fn (c);
        }
        else
        {
            a = c;
            c = d;
            fc = fd;
            d = a + kInvPhi * (b - a);
            fd = fn (d);
        }
    }
    return fc >= fd ? std::pair { c, fc } : std::pair { d, fd };
}
} // namespace

// =============================================================================
// CorrectionCurve
// =============================================================================
int CorrectionCurve::countFor (int channel) const noexcept
{
    int n = 0;
    for (int i = 0; i < numFilters; ++i)
        if (actsOn (filters[static_cast<size_t> (i)], channel))
            ++n;
    return n;
}

bool CorrectionCurve::add (const CorrectionFilter& filter) noexcept
{
    const bool valid = std::isfinite (filter.frequency) && filter.frequency > 0.0f && std::isfinite (filter.gainDb) && std::isfinite (filter.q)
                       && filter.q > 0.0f && (filter.channels & CorrectionFilter::kBoth) != 0
                       && static_cast<int> (filter.type) <= static_cast<int> (CorrectionFilterType::AllPass);
    if (! valid || numFilters >= kMaxFilters)
        return false;
    for (int c = 0; c < 2; ++c)
        if (actsOn (filter, c) && countFor (c) >= kMaxFiltersPerChannel)
            return false;
    auto f = filter;
    f.channels &= CorrectionFilter::kBoth;
    filters[static_cast<size_t> (numFilters++)] = f;
    return true;
}

SvfCoeffs CorrectionCurve::design (const CorrectionFilter& filter, double sampleRate) noexcept
{
    FilterType type = FilterType::Bell;
    switch (filter.type)
    {
        case CorrectionFilterType::Peak: type = FilterType::Bell; break;
        case CorrectionFilterType::LowShelf: type = FilterType::LowShelf; break;
        case CorrectionFilterType::HighShelf: type = FilterType::HighShelf; break;
        case CorrectionFilterType::LowPass: type = FilterType::LowPass; break;
        case CorrectionFilterType::HighPass: type = FilterType::HighPass; break;
        case CorrectionFilterType::BandPass: type = FilterType::BandPass; break;
        case CorrectionFilterType::Notch: type = FilterType::Notch; break;
        case CorrectionFilterType::AllPass: type = FilterType::AllPass; break;
    }
    return SvfCoeffs::make (type, filter.frequency, filter.q, hasGain (filter.type) ? filter.gainDb : 0.0, sampleRate);
}

double CorrectionCurve::responseDb (int channel, double freqHz, double sampleRate) const noexcept
{
    const int ch = std::clamp (channel, 0, 1);
    double db = gainDb[static_cast<size_t> (ch)];
    for (int i = 0; i < numFilters; ++i)
    {
        const auto& f = filters[static_cast<size_t> (i)];
        if (actsOn (f, ch))
            db += design (f, sampleRate).magnitudeDb (freqHz, sampleRate);
    }
    return db;
}

// =============================================================================
// Headroom predictor (E11)
// =============================================================================
namespace headroom
{
double programmeEnvelopeDb (double freqHz) noexcept
{
    const double f = std::max (freqHz, 1.0);
    if (f < 40.0)
        return -6.0 * std::log2 (40.0 / f);
    if (f <= 1000.0)
        return 0.0;
    return -3.0 * std::log2 (f / 1000.0);
}

Prediction predictMaxBoost (const std::function<double (double)>& responseDb, Weighting weighting, double loHz, double hiHz)
{
    loHz = std::max (loHz, 1.0);
    hiHz = std::max (hiHz, loHz);
    const auto eval = [&] (double log2Hz)
    {
        const double f = std::exp2 (log2Hz);
        const double r = responseDb (f);
        return (std::isfinite (r) ? r : 0.0) + (weighting == Weighting::Programme ? programmeEnvelopeDb (f) : 0.0);
    };

    const double x0 = std::log2 (loHz), span = std::log2 (hiHz) - x0;
    const int steps = std::max (1, static_cast<int> (std::ceil (span * kPointsPerOctave - 1.0e-9)));
    std::vector<double> xs (static_cast<size_t> (steps) + 1), ys (xs.size());
    for (size_t i = 0; i < xs.size(); ++i)
    {
        xs[i] = x0 + span * static_cast<double> (i) / static_cast<double> (steps);
        ys[i] = eval (xs[i]);
    }

    Prediction best { ys[0], std::exp2 (xs[0]) };
    for (size_t i = 0; i < xs.size(); ++i)
    {
        if (ys[i] > best.maxBoostDb)
            best = { ys[i], std::exp2 (xs[i]) };

        // Refine at each local maximum of the grid (a plateau is exact already).
        const double left = i > 0 ? ys[i - 1] : ys[i], right = i + 1 < ys.size() ? ys[i + 1] : ys[i];
        if (ys[i] < left || ys[i] < right || (ys[i] == left && ys[i] == right))
            continue;
        const auto [x, y] = goldenMax (eval, xs[i > 0 ? i - 1 : i], xs[std::min (i + 1, ys.size() - 1)]);
        if (y > best.maxBoostDb)
            best = { y, std::exp2 (x) };
    }
    return best;
}

float preampDb (const Prediction& prediction, double allowanceDb) noexcept
{
    const double excess = prediction.maxBoostDb - std::max (0.0, allowanceDb);
    return static_cast<float> (-std::max (0.0, std::isfinite (excess) ? excess : 0.0)) + 0.0f; // + 0: never -0
}
} // namespace headroom

// =============================================================================
// DeviceCorrection
// =============================================================================
DeviceCorrection::DeviceCorrection()
{
    pending.allocate (kPendingDesigns);
    prepare ({});
}

void DeviceCorrection::prepare (const ProcessSpec& spec)
{
    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 48000.0;
    maxBlock = std::max (1, spec.maxBlockSize);
    scratch.setSize (2, maxBlock);

    const auto fadeLength = static_cast<size_t> (std::max (1L, std::lround (kCrossfadeMs * 0.001 * sampleRate)));
    fadeIn.resize (fadeLength);
    for (size_t k = 0; k < fadeLength; ++k)
        fadeIn[k] = static_cast<float> (0.5 - 0.5 * std::cos (kPi * (static_cast<double> (k) + 0.5) / static_cast<double> (fadeLength)));

    setSettingsNow (settings);
}

void DeviceCorrection::setSettingsNow (const DeviceCorrectionSettings& s)
{
    settings = s;
    current = makeDesign (s);
    Design discard;
    while (pending.pop (discard))
    {
    }
    currentState = {};
    incomingState = {};
    fading = hasWaiting = false;
    fadePos = 0;
    unsent = false;
}

bool DeviceCorrection::setSettings (const DeviceCorrectionSettings& s)
{
    settings = s;
    unsent = ! pending.push (makeDesign (s));
    return ! unsent;
}

bool DeviceCorrection::retryPending()
{
    if (unsent)
        unsent = ! pending.push (makeDesign (settings));
    return ! unsent;
}

DeviceCorrection::Design DeviceCorrection::makeDesign (const DeviceCorrectionSettings& s)
{
    Design d;
    preampDb = 0.0f;
    prediction = {};
    if (! s.enabled)
        return d;

    const auto& curve = s.curve;
    const double sr = sampleRate;
    prediction = headroom::predictMaxBoost ([&curve, sr] (double f) { return std::max (curve.responseDb (0, f, sr), curve.responseDb (1, f, sr)); });
    preampDb = headroom::preampDb (prediction, s.allowanceDb);

    for (int ch = 0; ch < 2; ++ch)
    {
        const auto c = static_cast<size_t> (ch);
        d.gain[c] = dbToGain (preampDb + curve.gainDb[c]);
        if (s.compare)
            continue; // the correction's broadband gain, without its filters
        int n = 0;
        for (int i = 0; i < curve.numFilters && n < kMaxSections; ++i)
        {
            const auto& f = curve.filters[static_cast<size_t> (i)];
            if (! actsOn (f, ch) || (hasGain (f.type) && f.gainDb == 0.0f))
                continue; // a 0 dB bell / shelf is an exact identity
            d.sections[c][static_cast<size_t> (n++)] = CorrectionCurve::design (f, sr);
        }
        d.numSections[c] = n;
    }
    d.identity = d.numSections[0] == 0 && d.numSections[1] == 0 && d.gain[0] == 1.0f && d.gain[1] == 1.0f;
    return d;
}

void DeviceCorrection::reset() noexcept FLUB_NONBLOCKING
{
    if (fading)
        finishTransition();
    currentState = {};
    incomingState = {};
}

void DeviceCorrection::finishTransition() noexcept
{
    current = incoming;
    currentState = incomingState;
    fading = false;
    fadePos = 0;
    completed.fetch_add (1, std::memory_order_acq_rel);
}

void DeviceCorrection::runPath (const Design& design, State& state, int channel, float* data, int n) noexcept
{
    const auto c = static_cast<size_t> (channel);
    const float g = design.gain[c];
    if (g != 1.0f)
        for (int k = 0; k < n; ++k)
            data[k] *= g;

    const auto& sections = design.sections[c];
    auto& states = state[c];
    for (int s = 0; s < design.numSections[c]; ++s)
    {
        const auto& coeffs = sections[static_cast<size_t> (s)];
        SvfState local = states[static_cast<size_t> (s)];
        for (int k = 0; k < n; ++k)
            data[k] = svfTick (coeffs, local, data[k]);
        states[static_cast<size_t> (s)] = local;
    }
}

void DeviceCorrection::cleanStates (State& state) noexcept
{
    for (auto& channel : state)
        for (auto& s : channel)
        {
            s.ic1 = cleanState (s.ic1);
            s.ic2 = cleanState (s.ic2);
        }
}

void DeviceCorrection::process (const AudioBlock& block) noexcept FLUB_NONBLOCKING
{
    // The newest design handed over wins; it waits while a crossfade runs.
    while (pending.pop (waiting))
        hasWaiting = true;

    const int n = std::min (block.numSamples, maxBlock);
    const int channels = std::min (block.numChannels, 2);
    const int fadeLength = static_cast<int> (fadeIn.size());
    int pos = 0;
    while (pos < n)
    {
        if (! fading && hasWaiting)
        {
            incoming = waiting;
            incomingState = {};
            hasWaiting = false;
            fading = true;
            fadePos = 0;
        }

        if (! fading)
        {
            if (! current.identity)
                for (int c = 0; c < channels; ++c)
                    runPath (current, currentState, c, block.channel (c) + pos, n - pos);
            break;
        }

        const int len = std::min (n - pos, fadeLength - fadePos);
        for (int c = 0; c < channels; ++c)
        {
            float* x = block.channel (c) + pos;
            float* y = scratch.channel (c);
            std::copy (x, x + len, y);
            runPath (current, currentState, c, x, len);
            runPath (incoming, incomingState, c, y, len);
            const float* g = fadeIn.data() + fadePos;
            for (int k = 0; k < len; ++k)
                x[k] += g[k] * (y[k] - x[k]);
        }
        fadePos += len;
        pos += len;
        if (fadePos >= fadeLength)
            finishTransition();
    }

    cleanStates (currentState);
    if (fading)
        cleanStates (incomingState);
}
} // namespace flub
