// Flubsound Pro - discontinuity (glitch) detector (see the header).
#include "flub/analysis/Discontinuity.h"

#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>

namespace flub
{
namespace
{
// The residual is the 4th-order difference; its pattern for a break spans
// kOrder + 1 samples, and the RMS windows stay kGuard samples clear of it.
constexpr int kOrder = 4;
constexpr int kGuard = kOrder + 2;

constexpr double kActivityMs = 10.0; // Dropout: RMS time constant before a run
constexpr double kDcHz = 2.0;        // DcStep: low-pass corner
constexpr int kDcDecimation = 32;    // DcStep: history step (frames)

double dbToLinear (double db) noexcept { return std::pow (10.0, db / 20.0); }

float linearToDb (double v) noexcept { return static_cast<float> (20.0 * std::log10 (std::max (v, 1.0e-12))); }
} // namespace

const char* discontinuityName (DiscontinuityType type) noexcept
{
    switch (type)
    {
        case DiscontinuityType::Click: return "click";
        case DiscontinuityType::Dropout: return "dropout";
        case DiscontinuityType::NonFinite: return "non-finite";
        case DiscontinuityType::DcStep: return "dc-step";
    }
    return "";
}

void DiscontinuityDetector::prepare (double sampleRate, int numChannels, const DiscontinuitySettings& s)
{
    settings = s;
    fs = sampleRate > 0.0 && std::isfinite (sampleRate) ? sampleRate : 48000.0;
    const auto samplesOf = [this] (double ms, int minimum) {
        return std::max (minimum, static_cast<int> (std::lround ((std::isfinite (ms) ? ms : 0.0) * 0.001 * fs)));
    };
    block = std::max (2, samplesOf (s.windowMs, 8) / kBlocks);
    window = block * kBlocks;
    minDropout = samplesOf (s.minDropoutMs, 1);
    guard = kGuard;
    // A centre is judged once its after-window, and a dropout that starts
    // there, can be seen: minDropout samples at least.
    lookAhead = guard + std::max (window, minDropout);
    ringSize = guard + window + lookAhead + 2;
    clickRatio = dbToLinear (s.clickRatioDb);
    clickFloor = dbToLinear (s.clickFloorDb);
    activityThreshold = dbToLinear (2.0 * s.dropoutActivityDb); // mean square
    dcStep = dbToLinear (s.dcStepDb);
    activityCoeff = 1.0 - std::exp (-1000.0 / (kActivityMs * fs));

    // 2nd-order Butterworth low-pass at kDcHz (bilinear), direct form I.
    const double k = std::tan (kPi * kDcHz / fs), norm = 1.0 / (1.0 + std::sqrt (2.0) * k + k * k);
    lpB = { k * k * norm, 2.0 * k * k * norm, k * k * norm };
    lpA = { 1.0, 2.0 * (k * k - 1.0) * norm, (1.0 - std::sqrt (2.0) * k + k * k) * norm };
    dcDelay = std::max (1, static_cast<int> (std::lround (std::max (1.0, s.dcStepWindowMs) * 0.001 * fs / kDcDecimation)));

    // Cubic fits on each side of a candidate break: P = (A^T A)^-1 A^T for
    // rows [1, t, t^2, t^3], t = -kFit..-1 (left) or 1..kFit (right).
    for (int side = 0; side < 2; ++side)
    {
        std::array<std::array<double, 4>, kFit> a {};
        for (int i = 0; i < kFit; ++i)
        {
            const double t = side == 0 ? static_cast<double> (i - kFit) : static_cast<double> (i + 1);
            a[static_cast<size_t> (i)] = { 1.0, t, t * t, t * t * t };
        }
        // Normal matrix, inverted by Gauss-Jordan (4 x 4, well conditioned for these t).
        std::array<std::array<double, 8>, 4> m {};
        for (size_t r = 0; r < 4; ++r)
        {
            for (size_t c = 0; c < 4; ++c)
                for (size_t i = 0; i < static_cast<size_t> (kFit); ++i)
                    m[r][c] += a[i][r] * a[i][c];
            m[r][4 + r] = 1.0;
        }
        for (size_t p = 0; p < 4; ++p)
        {
            const double d = m[p][p];
            for (auto& v : m[p])
                v /= d;
            for (size_t r = 0; r < 4; ++r)
                if (r != p)
                {
                    const double f = m[r][p];
                    for (size_t c = 0; c < 8; ++c)
                        m[r][c] -= f * m[p][c];
                }
        }
        auto& fit = side == 0 ? fitLeft : fitRight;
        std::array<std::array<double, kFit>, 4> proj {}; // P
        for (size_t r = 0; r < 4; ++r)
            for (size_t i = 0; i < static_cast<size_t> (kFit); ++i)
                for (size_t c = 0; c < 4; ++c)
                    proj[r][i] += m[r][4 + c] * a[i][c];
        fit.value = proj[0];
        for (size_t i = 0; i < static_cast<size_t> (kFit); ++i)
            for (size_t j = 0; j < static_cast<size_t> (kFit); ++j)
                for (size_t c = 0; c < 4; ++c)
                    fit.hat[i][j] += a[i][c] * proj[c][j];
    }

    chans.assign (static_cast<size_t> (std::clamp (numChannels, 1, 32)), Channel {});
    for (auto& ch : chans)
    {
        ch.r.assign (static_cast<size_t> (ringSize), 0.0);
        ch.xs.assign (static_cast<size_t> (ringSize), 0.0);
        ch.dcHistory.assign (static_cast<size_t> (dcDelay + 1), 0.0);
    }
    list.clear();
    list.reserve (static_cast<size_t> (std::max (0, s.maxReported)));
    reset();
}

void DiscontinuityDetector::reset() noexcept
{
    for (auto& ch : chans)
    {
        auto r = std::move (ch.r);
        auto xs = std::move (ch.xs);
        auto dc = std::move (ch.dcHistory);
        std::fill (r.begin(), r.end(), 0.0);
        std::fill (xs.begin(), xs.end(), 0.0);
        std::fill (dc.begin(), dc.end(), 0.0);
        ch = Channel {};
        ch.r = std::move (r);
        ch.xs = std::move (xs);
        ch.dcHistory = std::move (dc);
    }
    frames = 0;
    kinkCount = 0;
    counts = {};
    list.clear();
}

int64_t DiscontinuityDetector::total() const noexcept
{
    int64_t t = 0;
    for (auto c : counts)
        t += c;
    return t;
}

void DiscontinuityDetector::report (const Discontinuity& d) noexcept
{
    ++counts[static_cast<size_t> (d.type)];
    // Within the capacity reserved in prepare(): no allocation.
    if (list.size() < static_cast<size_t> (std::max (0, settings.maxReported)) && list.size() < list.capacity())
        list.push_back (d);
}

void DiscontinuityDetector::process (const float* const* channels, int numFrames) noexcept
{
    if (channels == nullptr || numFrames <= 0)
        return;
    for (size_t c = 0; c < chans.size(); ++c)
    {
        const float* x = channels[c];
        if (x == nullptr)
            continue;
        for (int i = 0; i < numFrames; ++i)
            push (chans[c], static_cast<int> (c), static_cast<double> (x[i]), frames + i);
    }
    frames += numFrames;
}

void DiscontinuityDetector::push (Channel& ch, int c, double sample, int64_t n) noexcept
{
    // ---- non-finite runs: read as the last finite sample held ---------------
    if (! std::isfinite (sample))
    {
        if (ch.nanStart < 0)
            ch.nanStart = n;
        sample = ch.lastFinite;
    }
    else
    {
        if (ch.nanStart >= 0)
            closeNanRun (ch, c, n);
        ch.lastFinite = sample;
    }

    // ---- dropouts: runs of exact zeros after activity -----------------------
    const auto at = [&ch, this] (int64_t f) noexcept { return f < 0 ? 0.0 : ch.r[static_cast<size_t> (f % ringSize)]; };
    if (sample == 0.0)
    {
        if (ch.zeroStart < 0)
        {
            ch.zeroStart = n;
            ch.activityAtRun = ch.activity;
            ch.edgePeak = 0.0;
            // The residual's RMS before the run: the second-quietest block
            // of the window, as a Click is judged (rare: once per run).
            std::array<double, kBlocks> sums {};
            for (int k = 0; k < window; ++k)
                sums[static_cast<size_t> (k / block)] += at (n - 1 - k) * at (n - 1 - k);
            std::sort (sums.begin(), sums.end());
            ch.residualBeforeRun = std::sqrt (sums[1] / block);
        }
    }
    else if (ch.zeroStart >= 0)
        closeZeroRun (ch, c, n);
    ch.activity += (sample * sample - ch.activity) * activityCoeff;

    // ---- residual: 4th-order difference -------------------------------------
    double r = 0.0;
    if (ch.primed >= kOrder)
        r = sample - 4.0 * ch.x[0] + 6.0 * ch.x[1] - 4.0 * ch.x[2] + ch.x[3];
    else
        ++ch.primed;
    ch.x = { sample, ch.x[0], ch.x[1], ch.x[2] };
    ch.r[static_cast<size_t> (n % ringSize)] = r;
    ch.xs[static_cast<size_t> (n % ringSize)] = sample;
    if (ch.zeroStart >= 0 && n - ch.zeroStart <= kOrder)
        ch.edgePeak = std::max (ch.edgePeak, std::abs (r));

    // ---- DC steps: the 2 Hz low-pass against its value dcStepWindowMs ago ---
    const double lp = lpB[0] * sample + lpB[1] * ch.lpIn1 + lpB[2] * ch.lpIn2 - lpA[1] * ch.lp1 - lpA[2] * ch.lp2;
    ch.lpIn2 = ch.lpIn1;
    ch.lpIn1 = sample;
    ch.lp2 = ch.lp1;
    ch.lp1 = lp;
    if (n % kDcDecimation == 0)
    {
        const size_t size = ch.dcHistory.size();
        const double old = ch.dcHistory[static_cast<size_t> (ch.dcPos)]; // dcDelay steps ago once filled
        ch.dcHistory[static_cast<size_t> (ch.dcPos)] = lp;
        ch.dcPos = static_cast<int> ((static_cast<size_t> (ch.dcPos) + 1) % size);
        if (ch.dcFilled < static_cast<int> (size))
            ++ch.dcFilled;
        else if (n >= ch.dcHoldUntil && std::abs (lp - old) >= dcStep)
        {
            report ({ DiscontinuityType::DcStep, c, n, 0, linearToDb (std::abs (lp - old)), 0.0f });
            ch.dcHoldUntil = n + 2 * static_cast<int64_t> (dcDelay) * kDcDecimation;
        }
    }

    // ---- clicks: judge the centre lookAhead frames back ---------------------
    // Block j of the window before the centre is [c - guard - (j + 1) block,
    // c - guard - j block - 1], of the window after it [c + guard + 1 +
    // j block, c + guard + (j + 1) block].
    const int64_t centre = n - lookAhead;
    const auto sq = [&at] (int64_t f) noexcept { const double v = at (f); return v * v; };
    for (int j = 0; j < kBlocks; ++j)
    {
        auto& b = ch.before[static_cast<size_t> (j)];
        auto& a = ch.after[static_cast<size_t> (j)];
        if (centre % ringSize == 0)
        {
            // Exact sums now and then, so the running sums cannot drift.
            b = a = 0.0;
            for (int k = 0; k < block; ++k)
            {
                b += sq (centre - guard - j * block - 1 - k);
                a += sq (centre + guard + 1 + j * block + k);
            }
        }
        else
        {
            b = std::max (0.0, b + sq (centre - guard - j * block - 1) - sq (centre - guard - (j + 1) * block - 1));
            a = std::max (0.0, a + sq (centre + guard + (j + 1) * block) - sq (centre + guard + j * block));
        }
    }
    judgeClick (ch, c, centre);
}

void DiscontinuityDetector::judgeClick (Channel& ch, int c, int64_t centre) noexcept
{
    if (centre < kOrder + guard + window || centre < ch.holdUntil)
        return;
    if (centre >= ch.quietFrom && centre <= ch.quietTo)
        return;
    // An open dropout (long enough by now) or non-finite run owns its edges.
    const int64_t now = centre + lookAhead;
    if (ch.zeroStart >= 0 && centre >= ch.zeroStart - guard && isDropout (ch, now - ch.zeroStart))
        return;
    if (ch.nanStart >= 0 && centre >= ch.nanStart - guard)
        return;

    const auto at = [&ch, this] (int64_t f) noexcept { return ch.r[static_cast<size_t> (f % ringSize)]; };
    const double v = std::abs (at (centre));
    if (v < clickFloor)
        return;
    // Each side: its second-quietest block (one more break close by, or the
    // edge of a sound, fills one block, not the side).
    const auto sideSum = [] (std::array<double, kBlocks> sums) noexcept {
        std::sort (sums.begin(), sums.end());
        return sums[1];
    };
    const double ref = std::sqrt (std::max (sideSum (ch.before), sideSum (ch.after)) / block);
    if (v < clickRatio * ref)
        return;
    double peak = v;
    for (int k = 1; k <= kOrder; ++k)
        peak = std::max (peak, std::abs (at (centre + k)));
    ch.holdUntil = centre + window + guard;
    if (isValueBreak (ch, centre, peak))
        report ({ DiscontinuityType::Click, c, centre, 0, linearToDb (peak), linearToDb (peak / std::max (ref, 1.0e-12)) });
    else
        ++kinkCount;
}

bool DiscontinuityDetector::isValueBreak (const Channel& ch, int64_t centre, double peak) const noexcept
{
    // The break sits within a few samples of the first residual sample over
    // the threshold. For each position b there, fit a cubic to the kFit
    // samples on each side (b itself excluded) and keep the b whose fits are
    // best: the signal is smooth on both sides of a break (the residual was
    // 24 dB down there), so the fits are good only at the break.
    const auto x = [&ch, this] (int64_t f) noexcept { return ch.xs[static_cast<size_t> (f % ringSize)]; };
    double bestError = -1.0, bestJump = 0.0, bestOff = 0.0;
    for (int64_t b = centre - 2; b <= centre + kOrder - 1; ++b)
    {
        std::array<double, kFit> left {}, right {};
        for (int i = 0; i < kFit; ++i)
        {
            left[static_cast<size_t> (i)] = x (b - kFit + i);
            right[static_cast<size_t> (i)] = x (b + 1 + i);
        }
        double vL = 0.0, vR = 0.0, error = 0.0;
        for (size_t i = 0; i < static_cast<size_t> (kFit); ++i)
        {
            vL += fitLeft.value[i] * left[i];
            vR += fitRight.value[i] * right[i];
            double eL = left[i], eR = right[i];
            for (size_t j = 0; j < static_cast<size_t> (kFit); ++j)
            {
                eL -= fitLeft.hat[i][j] * left[j];
                eR -= fitRight.hat[i][j] * right[j];
            }
            error += eL * eL + eR * eR;
        }
        if (bestError < 0.0 || error < bestError)
        {
            const double xb = x (b);
            bestError = error;
            bestJump = vR - vL;                                    // a step
            bestOff = std::min (std::abs (xb - vL), std::abs (xb - vR)); // one sample off both sides
        }
    }
    // What a jump in value leaves in the 4th difference: a step J peaks at
    // 3 |J|, an impulse D at 6 |D| (a slope change K, at 2 |K|, is a kink).
    const double valueShare = std::max (3.0 * std::abs (bestJump), 6.0 * bestOff);
    return valueShare >= 0.5 * peak;
}

bool DiscontinuityDetector::isDropout (const Channel& ch, int64_t length) const noexcept
{
    return length >= minDropout && ch.activityAtRun >= activityThreshold && ch.edgePeak >= clickFloor
           && ch.edgePeak >= clickRatio * ch.residualBeforeRun;
}

void DiscontinuityDetector::closeZeroRun (Channel& ch, int c, int64_t end) noexcept
{
    const int64_t length = end - ch.zeroStart;
    if (isDropout (ch, length))
    {
        report ({ DiscontinuityType::Dropout, c, ch.zeroStart, length, static_cast<float> (10.0 * std::log10 (ch.activityAtRun)), 0.0f });
        const int64_t from = ch.zeroStart - guard, to = end + guard + kOrder;
        if (ch.quietTo < 0 || from > ch.quietTo + lookAhead)
            ch.quietFrom = from;
        ch.quietTo = to;
    }
    ch.zeroStart = -1;
}

void DiscontinuityDetector::closeNanRun (Channel& ch, int c, int64_t end) noexcept
{
    report ({ DiscontinuityType::NonFinite, c, ch.nanStart, end - ch.nanStart, 0.0f, 0.0f });
    const int64_t from = ch.nanStart - guard, to = end + guard + kOrder;
    if (ch.quietTo < 0 || from > ch.quietTo + lookAhead)
        ch.quietFrom = from;
    ch.quietTo = to;
    ch.nanStart = -1;
}

void DiscontinuityDetector::finish() noexcept
{
    for (size_t c = 0; c < chans.size(); ++c)
    {
        auto& ch = chans[c];
        if (ch.nanStart >= 0)
            closeNanRun (ch, static_cast<int> (c), frames);
        if (ch.zeroStart >= 0)
            closeZeroRun (ch, static_cast<int> (c), frames);
    }
}
} // namespace flub
