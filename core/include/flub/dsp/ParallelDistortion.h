// Flubsound Pro - distortion estimate of a parallel harmonic generator (RT-safe).
//
// The bass harmonics and the air exciter add a generated signal a to a dry
// (linear) path x: y = x + a. The generator shapes a band-limited copy b of
// the input and filters the result (band-split filters on both sides), so a
// can contain a LINEAR branch: the part of the shaper output proportional to
// b (an odd polynomial on a sinusoid below full scale, e.g. content on a
// band's skirt, or programme where the envelope is not the tone's own
// amplitude), phase-shifted by the filters. A scalar fit of y against x
// (DistortionEstimator.h) would count that linear filtering as distortion.
// Here the linear model of the output has two references, the dry path x
// and the generator's linear branch p (b through the same post filters and
// mix as the shaper output), and only what neither explains is counted:
//
//   residual = min over (alpha, beta) of || y - alpha x - beta p ||^2
//            = <r, r> - <r, q>^2 / <q, q>,   r = a - (<x, a> / <x, x>) x,
//                                            q = p - (<x, p> / <x, x>) x
//   ratioDb  = 10 log10 (sum_ch residual / sum_ch <y, y>)
//
// (Gram-Schmidt: a and p are first freed of their parts along x, then r of
// its part along q; q is dropped when p is (nearly) collinear with x or
// zero.) For a sinusoid the residual is exactly the energy of the harmonics
// the generator adds (plus aliases), whatever its linear branch; on
// programme it also contains intermodulation. Like DistortionWindow, the
// sums run over an analysis window of at least 25 ms that closes at the
// first block boundary at or after that (up to one block longer). Cost: six
// multiply-adds (in double) per sample and channel.
#pragma once

#include "DistortionEstimator.h"

namespace flub
{
/** Running sums of one channel: x = dry path, p = linear branch, a = added signal. */
struct ParallelDistortionSums
{
    double xx = 0.0, xp = 0.0, pp = 0.0, xa = 0.0, pa = 0.0, aa = 0.0;

    void add (float x, float p, float a) noexcept FLUB_NONBLOCKING
    {
        const auto xv = static_cast<double> (x), pv = static_cast<double> (p), av = static_cast<double> (a);
        xx += xv * xv;
        xp += xv * pv;
        pp += pv * pv;
        xa += xv * av;
        pa += pv * av;
        aa += av * av;
    }

    /** Energy of y = x + a left after the least-squares fit alpha x + beta p. */
    double residualEnergy() const noexcept FLUB_NONBLOCKING
    {
        // a and p freed of their parts along x ...
        const double ux = xx > 0.0 ? 1.0 / xx : 0.0;
        const double rr = std::max (0.0, aa - xa * xa * ux);
        const double qq = std::max (0.0, pp - xp * xp * ux);
        // ... then r freed of its part along q, unless p adds no direction.
        if (! (qq > kCollinear * pp))
            return rr;
        const double rq = pa - xa * xp * ux;
        return std::max (0.0, rr - rq * rq / qq);
    }

    /** Energy of y = x + a. */
    double outputEnergy() const noexcept FLUB_NONBLOCKING { return std::max (0.0, xx + 2.0 * xa + aa); }

    bool isFinite() const noexcept FLUB_NONBLOCKING
    {
        return std::isfinite (xx) && std::isfinite (xp) && std::isfinite (pp) && std::isfinite (xa) && std::isfinite (pa)
            && std::isfinite (aa);
    }

    /** Adds another stretch of the same channel. */
    void merge (const ParallelDistortionSums& o) noexcept FLUB_NONBLOCKING
    {
        xx += o.xx;
        xp += o.xp;
        pp += o.pp;
        xa += o.xa;
        pa += o.pa;
        aa += o.aa;
    }

    /** p counts as a second direction only if more than this share of its
        energy lies outside x (double rounding of the sums is ~1e-16). */
    static constexpr double kCollinear = 1.0e-9;
};

/** Per-channel sums over an analysis window of at least
    DistortionWindow::kWindowSeconds, closed at the first block boundary at or
    after getLength() samples, as in DistortionWindow. */
class ParallelDistortionWindow
{
public:
    /** Non-RT (prepare()). */
    void prepare (double sampleRate) noexcept
    {
        length = std::max (1, static_cast<int> (std::lround (sampleRate * DistortionWindow::kWindowSeconds)));
        reset();
    }

    void reset() noexcept FLUB_NONBLOCKING
    {
        sums.fill ({});
        count = 0;
    }

    /** The running sums of channel c (0 <= c < kMaxChannels). */
    ParallelDistortionSums& channel (int c) noexcept { return sums[static_cast<size_t> (c)]; }

    /** Counts n more base-rate samples. When the window is full, sets ratioDb
        to its reading over every channel (non-finite channels skipped;
        kMinusInfDb when nothing was added), starts the next window and
        returns true. */
    bool advance (int n, float& ratioDb) noexcept FLUB_NONBLOCKING
    {
        count += n;
        if (count < length)
            return false;
        DistortionEnergy e;
        for (const auto& s : sums)
        {
            const double r = s.residualEnergy(), o = s.outputEnergy();
            if (s.isFinite() && std::isfinite (r) && std::isfinite (o))
            {
                e.residual += r;
                e.output += o;
            }
        }
        ratioDb = e.ratioDb();
        reset();
        return true;
    }

    int getLength() const noexcept { return length; }

private:
    std::array<ParallelDistortionSums, kMaxChannels> sums {};
    int length = 1200, count = 0;
};
} // namespace flub
