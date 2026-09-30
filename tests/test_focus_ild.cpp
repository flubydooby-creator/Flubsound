// The HRTF-rendered ILD method for the positional focus (docs/11 E24;
// flub/analysis/SpatialMetrics.h): sources rendered with the parametric
// renderer's own HRIRs at 15 degree azimuth steps, through the focus at off /
// 50 / 100 %, and the per-1/3-octave deviation from the source's own ILD. It
// replaces the flat-ILD test signal (a 6 dB level difference with no time
// difference, which no real source has) and prepares the pointing task.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/analysis/SpatialMetrics.h"
#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace flub;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;

std::vector<float> noise (double seconds, uint32_t seed = 11)
{
    return whiteNoise (static_cast<int> (std::lround (seconds * kFs)), 0.2f, seed);
}

size_t bandIndex (const std::vector<double>& bands, double hz)
{
    return static_cast<size_t> (std::find (bands.begin(), bands.end(), hz) - bands.begin());
}
} // namespace

TEST_CASE ("Focus ILD method: a synthetic known ILD reads back per band, and the HRTF-rendered sources carry the model's own ILD")
{
    const auto x = noise (1.0);
    const auto bands = thirdOctaveBands (kFs, 250.0, 16000.0);

    // A flat 6.02 dB level difference reads 6.02 dB in every band.
    std::vector<float> half (x);
    for (auto& v : half)
        v *= 0.5f;
    for (double ild : bandIldDb (x, half, kFs, bands))
        CHECK_NEAR (ild, 6.0206, 0.001);

    // A known frequency-dependent change: the left ear through the focus
    // bell's own shape at full scale (+3 dB, 3 kHz, Q 0.5; docs/03 §7.3.2
    // gives +1.06 dB at 1 kHz, +3.00 at 3 kHz, +1.82 at 6 kHz) reads as that
    // deviation from the source.
    const auto source = hrtfRenderedSource (x, 45.0f, kFs);
    auto lifted = source;
    {
        const double a = std::pow (10.0, 3.0 / 40.0), w0 = kTwoPi * 3000.0 / kFs, alpha = std::sin (w0) / (2.0 * 0.5);
        const double a0 = 1.0 + alpha / a, b0 = (1.0 + alpha * a) / a0, b1 = -2.0 * std::cos (w0) / a0, b2 = (1.0 - alpha * a) / a0;
        const double a1 = b1, a2 = (1.0 - alpha / a) / a0;
        double z1 = 0.0, z2 = 0.0;
        for (auto& v : lifted[0])
        {
            const double y = b0 * v + z1;
            z1 = b1 * v - a1 * y + z2;
            z2 = b2 * v - a2 * y;
            v = static_cast<float> (y);
        }
    }
    const auto d = ildDeviation (source, lifted, kFs);
    CHECK_NEAR (d.deviationDb[bandIndex (d.bandsHz, 1000.0)], 1.06, 0.1);
    CHECK_NEAR (d.deviationDb[bandIndex (d.bandsHz, 3150.0)], 3.0, 0.1);
    CHECK_NEAR (d.deviationDb[bandIndex (d.bandsHz, 6300.0)], 1.7, 0.15);
    CHECK_NEAR (d.maxAbsDb, 3.0, 0.1);
    CHECK (d.maxAbsHz == 3150.0 || d.maxAbsHz == 2500.0);

    // The rendered sources' band ILD is the HRIR pair's own (the model's
    // analytic head shadow; 1/3-octave levels of the two impulse responses)
    // within 0.3 dB at every azimuth and band: right-hand sources read
    // negative (left over right), 0 and 180 degrees read 0.
    for (float az : focusIldAzimuths())
    {
        const auto s = hrtfRenderedSource (x, az, kFs);
        std::vector<float> hl, hr;
        HeadphoneVirtualizer::parametricHrir (az, 87.5f, kFs, 512, hl, hr);
        const auto il = thirdOctaveLevelsDb (hl, kFs, bands), ir = thirdOctaveLevelsDb (hr, kFs, bands);
        const auto measured = bandIldDb (s[0], s[1], kFs, bands);
        double worst = 0.0;
        for (size_t b = 0; b < bands.size(); ++b)
            worst = std::max (worst, std::abs (measured[b] - (il[b] - ir[b])));
        CHECK (worst <= 0.3);
        if (az == 0.0f || az == 180.0f)
            CHECK (std::abs (measured[bandIndex (bands, 4000.0)]) < 0.01);
        else
            CHECK (measured[bandIndex (bands, 4000.0)] < -5.0); // the head shadow: 5.5 .. 15.4 dB at 4 kHz
    }
}

TEST_CASE ("Focus ILD method: HRTF-rendered sources at 15 degree steps through the focus off / 50 / 100 %")
{
    const auto x = noise (1.0, 23);
    // Focus off is transparent; at 50 / 100 % the ILD of an HRTF-rendered
    // source moves by at most 0.1 dB in any band at any azimuth (measured:
    // <= 0.09 dB). The polarity guard (docs/03 §7.3.2) passes the lift only
    // while the band's M envelope leads S's, and with the interaural time
    // difference of a real source S is as strong as M, or stronger, in much
    // of 1 - 6 kHz. So today's focus does not add ILD to game-rendered
    // (HRTF) sources: the redesign decision (E24) starts from this.
    for (float az : focusIldAzimuths())
    {
        const auto r = focusIldDeviation (hrtfRenderedSource (x, az, kFs), kFs);
        for (double v : r[0].deviationDb)
            CHECK (std::abs (v) < 0.01);
        CHECK (r[1].maxAbsDb < 0.1);
        CHECK (r[2].maxAbsDb < 0.15);
        for (const auto& d : r)
        {
            CHECK (d.bandsHz.front() == 250.0 && d.bandsHz.back() == 16000.0);
            for (double v : d.deviationDb)
                CHECK (std::abs (v) < 0.15);
        }
    }

    // The flat-ILD source of the Phase 1 slice (6 dB, no time difference):
    // +2.82 dB at 3.15 kHz at 100 % (the slice's 2.86 dB at 3 kHz), +1.25 dB
    // at 50 %, nothing below 250 Hz or at 16 kHz.
    std::vector<std::vector<float>> flat { x, x };
    for (auto& v : flat[1])
        v *= 0.5f;
    const auto f = focusIldDeviation (flat, kFs);
    const size_t b3 = bandIndex (f[2].bandsHz, 3150.0);
    CHECK_NEAR (f[2].deviationDb[b3], 2.82, 0.1);
    CHECK_NEAR (f[1].deviationDb[b3], 1.25, 0.1);
    CHECK (f[2].maxAbsHz == 3150.0);
    CHECK (f[0].maxAbsDb < 0.01);
    CHECK (std::abs (f[2].deviationDb.back()) < 0.15);
}
