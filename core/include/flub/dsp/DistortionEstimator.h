// Flubsound Pro - per-block THD+N estimate of a nonlinear stage (RT-safe).
//
// For a stage with input x and output y, taken where the two are
// time-aligned and at the same rate (inside the stage, around its curve):
//
//   g       = <x, y> / <x, x>                   least-squares linear gain over the block
//   r       = y - g x                           nonlinear residual (harmonics, IM, noise)
//   ratioDb = 10 log10( <r, r> / <y, y> )       THD+N relative to the output energy
//
// The stages accumulate x and the deviation d = y - x that the curve adds,
// which avoids the cancellation of <y,y> - <x,y>^2 / <x,x> at low distortion:
//
//   <r, r> = <d, d> - <x, d>^2 / <x, x>         <y, y> = <x, x> + 2 <x, d> + <d, d>
//
// Every channel gets its own g; residual and output energies are summed over
// the channels before the ratio is taken. A linear stage (d = k x) reads the
// rounding floor (far below -150 dB); no output or no residual reads
// kMinusInfDb (-160 dB). Cost: three multiply-adds (in double) per sample and
// channel.
//
// The stages accumulate the sums over a DistortionWindow of at least 25 ms
// (kWindowSeconds), not over the host block: over a stretch much shorter than
// a bass period, the fundamental and its harmonics are nearly collinear, so g
// absorbs most of the harmonics (a 55 Hz sine through 12 dB of tape drive
// read 8 dB low in 64-sample blocks and 14 dB low in 32-sample blocks). From
// about 10 ms on the reading at 55 Hz no longer depends on the length.
#pragma once

#include "flub/common/AudioBlock.h"
#include "flub/common/Math.h"
#include "flub/common/Realtime.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace flub
{
/** Running sums of one channel over one stretch: x = stage input, d = y - x. */
struct DistortionSums
{
    double xx = 0.0, xd = 0.0, dd = 0.0;

    void add (float x, float d) noexcept FLUB_NONBLOCKING
    {
        const auto xv = static_cast<double> (x), dv = static_cast<double> (d);
        xx += xv * xv;
        xd += xv * dv;
        dd += dv * dv;
    }

    /** Energy of y - g x with the least-squares g (0 when nothing is left). */
    double residualEnergy() const noexcept FLUB_NONBLOCKING
    {
        return xx > 0.0 ? std::max (0.0, dd - xd * xd / xx) : dd;
    }

    /** Energy of y = x + d. */
    double outputEnergy() const noexcept FLUB_NONBLOCKING { return std::max (0.0, xx + 2.0 * xd + dd); }

    bool isFinite() const noexcept FLUB_NONBLOCKING { return std::isfinite (xx) && std::isfinite (xd) && std::isfinite (dd); }

    /** Adds another stretch of the same channel. */
    void merge (const DistortionSums& o) noexcept FLUB_NONBLOCKING
    {
        xx += o.xx;
        xd += o.xd;
        dd += o.dd;
    }
};

/** Residual and output energy of a stage over one stretch, summed over channels. */
struct DistortionEnergy
{
    double residual = 0.0, output = 0.0;

    /** Adds one channel's sums; non-finite sums (non-finite input) are skipped. */
    void add (const DistortionSums& s) noexcept FLUB_NONBLOCKING
    {
        const double r = s.residualEnergy(), o = s.outputEnergy();
        if (std::isfinite (r) && std::isfinite (o))
        {
            residual += r;
            output += o;
        }
    }

    /** THD+N ratio in dB (kMinusInfDb when there is no residual or no output). */
    float ratioDb() const noexcept FLUB_NONBLOCKING
    {
        if (! (residual > 0.0) || ! (output > 0.0))
            return kMinusInfDb;
        return static_cast<float> (std::max (static_cast<double> (kMinusInfDb), 10.0 * std::log10 (residual / output)));
    }

    /** Convenience (tests, offline analysis): the ratio of y against x over n samples. */
    static float measureDb (const float* x, const float* y, int n) noexcept
    {
        DistortionSums s;
        for (int i = 0; i < n; ++i)
            s.add (x[i], y[i] - x[i]);
        DistortionEnergy e;
        e.add (s);
        return e.ratioDb();
    }
};

/** Per-channel sums over an analysis window of a fixed length (base-rate
    samples), independent of how the host splits the stream into blocks. */
class DistortionWindow
{
public:
    static constexpr double kWindowSeconds = 0.025;

    /** Non-RT (prepare()). */
    void prepare (double sampleRate) noexcept
    {
        length = std::max (1, static_cast<int> (std::lround (sampleRate * kWindowSeconds)));
        reset();
    }

    void reset() noexcept FLUB_NONBLOCKING
    {
        sums.fill ({});
        count = 0;
    }

    /** The running sums of channel c (0 <= c < kMaxChannels). */
    DistortionSums& channel (int c) noexcept { return sums[static_cast<size_t> (c)]; }

    /** Counts n more base-rate samples. When the window is full, sets ratioDb
        to its THD+N over every channel (and deviationDb, if given, to
        10 log10(sum <d, d> / sum <x, x>), the deviation energy relative to the
        input: for the clipper, its clip energy ratio over the same window),
        starts the next window and returns true. */
    bool advance (int n, float& ratioDb, float* deviationDb = nullptr) noexcept FLUB_NONBLOCKING
    {
        count += n;
        if (count < length)
            return false;
        DistortionEnergy e;
        double dd = 0.0, xx = 0.0;
        for (const auto& s : sums)
        {
            e.add (s);
            if (s.isFinite())
            {
                dd += s.dd;
                xx += s.xx;
            }
        }
        ratioDb = e.ratioDb();
        if (deviationDb != nullptr)
            *deviationDb = dd > 0.0 && xx > 0.0 ? static_cast<float> (std::max (static_cast<double> (kMinusInfDb), 10.0 * std::log10 (dd / xx)))
                                                : kMinusInfDb;
        reset();
        return true;
    }

    int getLength() const noexcept { return length; }

private:
    std::array<DistortionSums, kMaxChannels> sums {};
    int length = 1200, count = 0;
};
} // namespace flub
