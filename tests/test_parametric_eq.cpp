// Tests for the fully parametric EQ: exact nulls, analytic-vs-measured
// response for every band type, Butterworth cut slopes, click-free parameter
// and topology changes, clamping, stability at 20 kHz / 192 kHz, allocation
// freedom, robustness, block-size invariance and zero latency.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/dsp/ParametricEq.h"

#include <array>
#include <cmath>
#include <limits>
#include <memory>

using namespace flub;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;
constexpr std::array<double, 4> kRates { 44100.0, 48000.0, 96000.0, 192000.0 };

EqBandParams makeBand (EqBandType type, float freq, float gainDb = 0.0f, float q = 0.7071f, int slope = 12, bool enabled = true)
{
    EqBandParams p;
    p.enabled = enabled;
    p.type = type;
    p.frequency = freq;
    p.gainDb = gainDb;
    p.q = q;
    p.slopeDbPerOct = slope;
    return p;
}

std::unique_ptr<ParametricEq> makeEq (double sampleRate, int numChannels = 2, int maxBlock = 4096)
{
    auto eq = std::make_unique<ParametricEq>();
    eq->prepare ({ sampleRate, maxBlock, numChannels });
    return eq;
}

bool allFinite (const Planar& buf)
{
    for (const auto& c : buf.ch)
        for (float v : c)
            if (! std::isfinite (v))
                return false;
    return true;
}

double peakOf (const Planar& buf)
{
    double p = 0.0;
    for (const auto& c : buf.ch)
        p = std::max (p, peakAbs (c.data(), static_cast<int> (c.size())));
    return p;
}

/** max |y[n] - y[n-1]| for n in [from, to). */
double maxStep (const std::vector<float>& y, int from, int to)
{
    double m = 0.0;
    for (int n = std::max (1, from); n < to; ++n)
        m = std::max (m, static_cast<double> (std::abs (y[static_cast<size_t> (n)] - y[static_cast<size_t> (n - 1)])));
    return m;
}

/** max |y[n] - 2 y[n-1] + y[n-2]| for n in [from, to): the discrete curvature.
    Far more sensitive to zipper steps and state discontinuities than the first
    difference, because a slow tone has a tiny steady-state curvature. */
double maxCurvature (const std::vector<float>& y, int from, int to)
{
    double m = 0.0;
    for (int n = std::max (2, from); n < to; ++n)
    {
        const auto i = static_cast<size_t> (n);
        m = std::max (m, static_cast<double> (std::abs (y[i] - 2.0f * y[i - 1] + y[i - 2])));
    }
    return m;
}

/** Reference for a gain glide, the smoothest realisation of the specified
    behaviour: from sample `change` on, the band gain (dB) follows a per-sample
    one-pole with the EQ's 20 ms time constant from 0 dB towards gainDb, the
    SVF coefficients are redesigned every `hold` samples (1 = every sample)
    and the filter state is always valid (the filter also runs, as an exact
    identity, before the change). hold = kControlInterval models a naive
    control-rate implementation without coefficient interpolation (zipper). */
std::vector<float> referenceGainGlide (FilterType type, double freq, double q, double gainDb, double fs,
                                       const std::vector<float>& x, int change, int hold)
{
    const double pole = std::exp (-1.0 / (0.020 * fs));
    double db = 0.0;
    SvfState state;
    auto coeffs = SvfCoeffs::make (type, freq, q, 0.0, fs);
    std::vector<float> y (x.size());
    for (int i = 0; i < static_cast<int> (x.size()); ++i)
    {
        if (i >= change)
        {
            db = gainDb + pole * (db - gainDb);
            if ((i - change) % hold == 0)
                coeffs = SvfCoeffs::make (type, freq, q, db, fs);
        }
        y[static_cast<size_t> (i)] = svfTick (coeffs, state, x[static_cast<size_t> (i)]);
    }
    return y;
}

/** Copies into the existing channel storage. (Assigning a temporary vector to
    a Planar channel would move a new buffer in and leave its pointers stale.) */
void load (Planar& buf, int channel, const std::vector<float>& s)
{
    auto& dst = buf.ch[static_cast<size_t> (channel)];
    std::copy (s.begin(), s.begin() + static_cast<std::ptrdiff_t> (std::min (s.size(), dst.size())), dst.begin());
}

void fillAll (Planar& buf, const std::vector<float>& s)
{
    for (int c = 0; c < buf.numChannels(); ++c)
        load (buf, c, s);
}

/** Steady-state gain of a single configured band, measured with a sine. Even
    integer frequencies give a whole number of periods in the 0.5 s window
    used by measureGainDb(), so the single-bin DFT has no leakage. */
double measuredBandDb (double sampleRate, const EqBandParams& p, double freq)
{
    auto eq = makeEq (sampleRate);
    eq->setBand (0, p);
    eq->reset();
    return measureGainDb (*eq, freq, sampleRate);
}
} // namespace

//==============================================================================
TEST_CASE ("ParametricEq: all bands disabled is an exact null")
{
    for (double fs : kRates)
    {
        auto eq = makeEq (fs);
        // Disabled bands with wild settings must not matter.
        for (int b = 0; b < ParametricEq::kMaxBands; ++b)
            eq->setBand (b, makeBand (static_cast<EqBandType> (b % 7), 20.0f + 1000.0f * static_cast<float> (b), 24.0f, 18.0f, 48, false));

        const int n = 8192;
        auto in = whiteNoise (n, 0.9f, 77);
        Planar buf (2, n);
        fillAll (buf, in);
        processInBlocks (*eq, buf, 256);
        double err = 0.0;
        for (int i = 0; i < n; ++i)
            err = std::max (err, static_cast<double> (std::abs (buf.ch[1][static_cast<size_t> (i)] - in[static_cast<size_t> (i)])));
        CHECK_LE (err, 1.0e-6);
    }
}

TEST_CASE ("ParametricEq: bells and shelves at 0 dB are an exact null, also after toggles and glides")
{
    auto eq = makeEq (kFs);
    const EqBandType gainTypes[] = { EqBandType::Bell, EqBandType::LowShelf, EqBandType::HighShelf };
    for (int b = 0; b < ParametricEq::kMaxBands; ++b)
        eq->setBand (b, makeBand (gainTypes[b % 3], 25.0f * std::pow (2.0f, 0.6f * static_cast<float> (b)), 0.0f, 0.1f + 1.1f * static_cast<float> (b), 12, true));

    const int n = 16384;
    const auto in = whiteNoise (n, 1.0f, 5);
    Planar buf (2, n);
    // Runs one pass of the test signal and returns max |out - in| over both channels.
    const auto nullError = [&]
    {
        fillAll (buf, in);
        processInBlocks (*eq, buf, 128);
        double err = 0.0;
        for (const auto& c : buf.ch)
            for (int i = 0; i < n; ++i)
                err = std::max (err, static_cast<double> (std::abs (c[static_cast<size_t> (i)] - in[static_cast<size_t> (i)])));
        return err;
    };

    CHECK_LE (nullError(), 1.0e-6);

    // Enable toggles, type changes between 0 dB gain types and frequency / Q
    // glides of a 0 dB band change nothing audible, so they stay exact.
    eq->setBand (3, makeBand (EqBandType::Bell, 1000.0f, 0.0f, 1.0f, 12, false));
    CHECK_LE (nullError(), 1.0e-6);
    eq->setBand (3, makeBand (EqBandType::LowShelf, 3000.0f, 0.0f, 4.0f, 12, true));
    CHECK_LE (nullError(), 1.0e-6);
    eq->setBand (3, makeBand (EqBandType::HighShelf, 300.0f, 0.0f, 0.2f, 12, true));
    CHECK_LE (nullError(), 1.0e-6);

    // Boost band 3, then return it to 0 dB: once the ~20 ms glide back has
    // settled (16384 samples > 16 time constants) it must be skipped again.
    eq->setBand (3, makeBand (EqBandType::Bell, 1000.0f, 9.0f, 1.0f));
    CHECK_GE (nullError(), 0.1); // really boosted
    eq->setBand (3, makeBand (EqBandType::Bell, 1000.0f, 0.0f, 1.0f));
    (void) nullError(); // the glide back
    CHECK_LE (nullError(), 1.0e-6);
}

//==============================================================================
TEST_CASE ("ParametricEq: measured sine gain matches responseDb() for every band type")
{
    struct Case
    {
        EqBandParams p;
    };
    const Case cases[] = {
        { makeBand (EqBandType::Bell, 1000.0f, 9.0f, 1.5f) },
        { makeBand (EqBandType::Bell, 150.0f, -12.0f, 0.5f) },
        { makeBand (EqBandType::Bell, 8000.0f, 24.0f, 18.0f) },
        { makeBand (EqBandType::LowShelf, 200.0f, 6.0f, 0.7071f) },
        { makeBand (EqBandType::LowShelf, 1000.0f, -10.0f, 2.0f) },
        { makeBand (EqBandType::HighShelf, 4000.0f, 8.0f, 0.7071f) },
        { makeBand (EqBandType::HighShelf, 10000.0f, -12.0f, 1.0f) },
        { makeBand (EqBandType::LowCut, 1000.0f, 0.0f, 0.7071f, 12) },
        { makeBand (EqBandType::LowCut, 1000.0f, 0.0f, 0.7071f, 24) },
        { makeBand (EqBandType::LowCut, 1000.0f, 0.0f, 0.7071f, 36) },
        { makeBand (EqBandType::LowCut, 1000.0f, 0.0f, 0.7071f, 48) },
        { makeBand (EqBandType::HighCut, 1000.0f, 0.0f, 0.7071f, 12) },
        { makeBand (EqBandType::HighCut, 1000.0f, 0.0f, 0.7071f, 24) },
        { makeBand (EqBandType::HighCut, 1000.0f, 0.0f, 0.7071f, 36) },
        { makeBand (EqBandType::HighCut, 1000.0f, 0.0f, 0.7071f, 48) },
        { makeBand (EqBandType::HighCut, 8000.0f, 0.0f, 0.7071f, 24) },
        { makeBand (EqBandType::Notch, 1000.0f, 0.0f, 2.0f) },
        { makeBand (EqBandType::BandPass, 1000.0f, 0.0f, 1.0f) },
        { makeBand (EqBandType::BandPass, 400.0f, 0.0f, 5.0f) },
    };
    const double freqs[] = { 50.0, 150.0, 400.0, 1000.0, 2500.0, 6000.0, 8000.0, 12000.0, 18000.0 };

    int compared = 0;
    for (const auto& c : cases)
    {
        auto eq = makeEq (kFs);
        eq->setBand (5, c.p); // any slot
        for (double f : freqs)
        {
            const double predicted = ParametricEq::responseDb (&c.p, 1, f, kFs);
            if (predicted < -60.0)
                continue; // below the measurement's float precision budget
            const double measured = measureGainDb (*eq, f, kFs);
            CHECK_NEAR (measured, predicted, 0.1);
            ++compared;
        }
    }
    CHECK (compared > 120);
}

TEST_CASE ("ParametricEq: shape sanity - bell centre, shelf plateaus, notch depth, band-pass peak")
{
    const auto bell = makeBand (EqBandType::Bell, 1000.0f, 7.5f, 2.0f);
    CHECK_NEAR (ParametricEq::responseDb (&bell, 1, 1000.0, kFs), 7.5, 1.0e-4);
    CHECK_NEAR (ParametricEq::responseDb (&bell, 1, 30.0, kFs), 0.0, 0.05);

    const auto ls = makeBand (EqBandType::LowShelf, 150.0f, -9.0f);
    CHECK_NEAR (ParametricEq::responseDb (&ls, 1, 10.0, kFs), -9.0, 0.05);
    CHECK_NEAR (ParametricEq::responseDb (&ls, 1, 15000.0, kFs), 0.0, 0.05);

    const auto hs = makeBand (EqBandType::HighShelf, 6000.0f, 6.0f);
    CHECK_NEAR (ParametricEq::responseDb (&hs, 1, 23900.0, kFs), 6.0, 0.1);
    CHECK_NEAR (ParametricEq::responseDb (&hs, 1, 50.0, kFs), 0.0, 0.05);

    const auto notch = makeBand (EqBandType::Notch, 1000.0f, 0.0f, 4.0f);
    CHECK_LE (ParametricEq::responseDb (&notch, 1, 1000.0, kFs), -100.0);
    CHECK_LE (measuredBandDb (kFs, notch, 1000.0), -50.0);

    const auto bp = makeBand (EqBandType::BandPass, 2500.0f, 0.0f, 3.0f);
    CHECK_NEAR (ParametricEq::responseDb (&bp, 1, 2500.0, kFs), 0.0, 1.0e-4);
    CHECK_NEAR (measuredBandDb (kFs, bp, 2500.0), 0.0, 0.05);
}

TEST_CASE ("ParametricEq: LowCut / HighCut are Butterworth (-3 dB at fc, 6 dB/oct per order)")
{
    // Bilinear-transformed Butterworth of order N: |H|^2 = 1 / (1 + r^(+-2N)) with
    // the prewarped frequency ratio r = tan(pi f / fs) / tan(pi fc / fs).
    const auto warped = [] (double f, double fc) { return std::tan (kPi * f / kFs) / std::tan (kPi * fc / kFs); };

    for (int slope : { 12, 24, 36, 48 })
    {
        const int order = slope / 6;
        const double lowExpected = -10.0 * std::log10 (1.0 + std::pow (1.0 / warped (500.0, 1000.0), 2.0 * order));
        const double highExpected = -10.0 * std::log10 (1.0 + std::pow (warped (2000.0, 1000.0), 2.0 * order));
        // The analog prototype one octave into the stop band: -10 log10 (1 + 2^(2N)).
        const double analog = -10.0 * std::log10 (1.0 + std::pow (2.0, 2.0 * order));

        const auto lc = makeBand (EqBandType::LowCut, 1000.0f, 0.0f, 5.0f, slope); // Q is ignored by cuts
        CHECK_NEAR (ParametricEq::responseDb (&lc, 1, 1000.0, kFs), -3.0103, 0.001);
        CHECK_NEAR (ParametricEq::responseDb (&lc, 1, 500.0, kFs), lowExpected, 0.001);
        CHECK_NEAR (ParametricEq::responseDb (&lc, 1, 500.0, kFs), analog, 0.1);
        CHECK_NEAR (measuredBandDb (kFs, lc, 500.0), lowExpected, 0.1);
        CHECK_NEAR (measuredBandDb (kFs, lc, 12000.0), 0.0, 0.02);

        const auto hc = makeBand (EqBandType::HighCut, 1000.0f, 0.0f, 0.3f, slope);
        CHECK_NEAR (ParametricEq::responseDb (&hc, 1, 1000.0, kFs), -3.0103, 0.001);
        CHECK_NEAR (ParametricEq::responseDb (&hc, 1, 2000.0, kFs), highExpected, 0.001);
        CHECK_NEAR (measuredBandDb (kFs, hc, 2000.0), highExpected, 0.1);
        CHECK_NEAR (measuredBandDb (kFs, hc, 50.0), 0.0, 0.02);
    }

    // The headline number from the spec: LowCut 24 dB/oct, one octave down.
    const auto lc24 = makeBand (EqBandType::LowCut, 1000.0f, 0.0f, 0.7071f, 24);
    CHECK_NEAR (measuredBandDb (kFs, lc24, 500.0), -24.1, 0.1);
}

TEST_CASE ("ParametricEq: responseDb() sums enabled bands, ignores disabled ones, clamps and is finite")
{
    const std::array<EqBandParams, 4> bands {
        makeBand (EqBandType::Bell, 300.0f, 6.0f, 1.0f),
        makeBand (EqBandType::HighShelf, 5000.0f, -4.0f),
        makeBand (EqBandType::LowCut, 80.0f, 0.0f, 0.7f, 36),
        makeBand (EqBandType::Bell, 2000.0f, 24.0f, 18.0f, 12, false), // disabled
    };
    for (double f : { 20.0, 90.0, 300.0, 1000.0, 5000.0, 19000.0 })
    {
        double sum = 0.0;
        for (int b = 0; b < 3; ++b)
            sum += ParametricEq::responseDb (&bands[static_cast<size_t> (b)], 1, f, kFs);
        CHECK_NEAR (ParametricEq::responseDb (bands.data(), 4, f, kFs), sum, 1.0e-9);
    }
    CHECK_NEAR (ParametricEq::responseDb (&bands[3], 1, 2000.0, kFs), 0.0, 0.0);
    CHECK_NEAR (ParametricEq::responseDb (nullptr, 3, 1000.0, kFs), 0.0, 0.0);
    CHECK_NEAR (ParametricEq::responseDb (bands.data(), 0, 1000.0, kFs), 0.0, 0.0);

    // Out-of-range parameters are evaluated exactly as the running filter would clamp them.
    const auto wild = makeBand (EqBandType::Bell, 1000.0f, 60.0f, 0.001f);
    const auto clamped = makeBand (EqBandType::Bell, 1000.0f, 24.0f, 0.1f);
    CHECK_NEAR (ParametricEq::responseDb (&wild, 1, 700.0, kFs), ParametricEq::responseDb (&clamped, 1, 700.0, kFs), 1.0e-12);
    CHECK_NEAR (ParametricEq::responseDb (&wild, 1, 1000.0, kFs), 24.0, 1.0e-4);

    // Evaluating at/above Nyquist, at DC, or a total stop is finite.
    const auto stop = makeBand (EqBandType::HighCut, 20.0f, 0.0f, 0.7f, 48);
    for (double f : { 0.0, 24000.0, 96000.0 })
    {
        CHECK (std::isfinite (ParametricEq::responseDb (&stop, 1, f, kFs)));
        CHECK (std::isfinite (ParametricEq::responseDb (&wild, 1, f, kFs)));
    }
}

//==============================================================================
TEST_CASE ("ParametricEq: abrupt gain / Q jumps are click-free (first-difference criterion)")
{
    // A smooth glide between two steady states cannot move the output faster
    // than the faster of the two steady states does, so the largest
    // |y[n] - y[n-1]| during the transition must stay within a small factor
    // (1.1) of the steady-state maximum before / after. A click (step) adds
    // its full height on top. Each case is run in three band histories:
    //   0: set at 0 dB and reset (skipped as an identity until the jump),
    //   1: +6 dB, disabled, re-enabled at 0 dB (state invalidated by the
    //      swaps) and only then boosted,
    //   2: enabled at 0 dB 2 ms before the boost (inside the 5 ms fade-in).
    // A tone at the band frequency sees the largest gain change.
    struct Case
    {
        EqBandParams from, to;
        double toneHz;
    };
    const Case cases[] = {
        { makeBand (EqBandType::Bell, 200.0f, 0.0f, 1.0f), makeBand (EqBandType::Bell, 200.0f, 12.0f, 1.0f), 200.0 },
        { makeBand (EqBandType::Bell, 200.0f, 0.0f, 8.0f), makeBand (EqBandType::Bell, 200.0f, 12.0f, 8.0f), 200.0 },
        { makeBand (EqBandType::LowShelf, 200.0f, 0.0f), makeBand (EqBandType::LowShelf, 200.0f, 12.0f), 200.0 },
        { makeBand (EqBandType::HighShelf, 200.0f, 0.0f), makeBand (EqBandType::HighShelf, 200.0f, 12.0f), 200.0 },
        // Q jump 0.3 -> 10 on a +12 dB bell, heard off-centre (~+11 dB -> ~+0.3 dB).
        { makeBand (EqBandType::Bell, 200.0f, 12.0f, 0.3f), makeBand (EqBandType::Bell, 200.0f, 12.0f, 10.0f), 300.0 },
    };

    for (double fs : { 48000.0, 192000.0 })
        for (const auto& c : cases)
            for (int history = 0; history < 3; ++history)
            {
                if (history > 0 && c.from.gainDb != 0.0f)
                    continue; // the histories are about bands that start at 0 dB

                auto eq = makeEq (fs);
                const int block = 64;
                const int change = static_cast<int> (fs * 0.2) / block * block; // on the control grid
                const int n = change + static_cast<int> (fs * 0.4);
                auto boosted = c.from;
                boosted.gainDb = 6.0f;
                auto disabled = c.from;
                disabled.enabled = false;
                eq->setBand (0, history == 0 ? c.from : (history == 1 ? boosted : disabled));
                eq->reset();

                Planar buf (2, n);
                fillAll (buf, sine (c.toneHz, fs, n, 0.2f));
                for (int pos = 0; pos < n; pos += block)
                {
                    if (history == 1 && pos == block * 20)
                        eq->setBand (0, disabled);
                    if (history == 1 && pos == block * 60)
                        eq->setBand (0, c.from);
                    if (history == 2 && pos == change - static_cast<int> (fs * 0.002) / block * block)
                        eq->setBand (0, c.from);
                    if (pos == change)
                        eq->setBand (0, c.to);
                    eq->process (buf.block (pos, std::min (block, n - pos)));
                }
                CHECK (allFinite (buf));

                const auto& y = buf.ch[0];
                const int tail = static_cast<int> (fs * 0.1);
                const double before = maxStep (y, change - tail, change);
                const double after = maxStep (y, n - tail, n);
                const double transition = maxStep (y, change, n);
                // The glide really arrived: steady-state slope ratio == |H(tone)| ratio.
                const double gainRatio = std::pow (10.0, (ParametricEq::responseDb (&c.to, 1, c.toneHz, fs)
                                                          - ParametricEq::responseDb (&c.from, 1, c.toneHz, fs)) / 20.0);
                CHECK_NEAR (after / before, gainRatio, 0.02 * gainRatio);
                CHECK_LE (transition, 1.1 * std::max (before, after));
            }
}

TEST_CASE ("ParametricEq: gain glides are as smooth as an ideal per-sample glide (no zipper, no stale state)")
{
    // The curvature |y[n] - 2y[n-1] + y[n-2]| of a slow tone is tiny in the
    // steady state (~ A (2 pi f / fs)^2), so it exposes coefficient steps at
    // the 16-sample control rate and state discontinuities when a skipped
    // 0 dB band resumes. It cannot be bounded by the steady-state curvature,
    // though: any exponential glide has a slope kink at its onset of about
    // (ln 10 / 20) (12 dB / 20 ms) A / fs, which relative to the steady state
    // grows with the sample rate. The physically meaningful bound is the
    // curvature of the smoothest possible realisation of the specified
    // smoothing: a per-sample one-pole glide in dB, coefficients redesigned
    // every sample, on a filter whose state was always valid. The EQ may
    // deviate from it only by its linear coefficient interpolation between
    // control ticks (~1 % onset slope difference) and by the approximate
    // (primed) state of a resumed band; 25 % covers both, while a 16-sample
    // zipper exceeds it several-fold (checked below as a negative control)
    // and a zero / stale state on resume by 4x .. 400x.
    const FilterType svfTypes[] = { FilterType::Bell, FilterType::LowShelf, FilterType::HighShelf };
    const EqBandType eqTypes[] = { EqBandType::Bell, EqBandType::LowShelf, EqBandType::HighShelf };
    constexpr double toneHz = 101.25, bandHz = 1000.0, gainDb = 12.0;

    for (double fs : kRates)
    {
        const int block = 64;
        const int change = static_cast<int> (fs * 0.1) / block * block; // on the control grid
        const int n = change + static_cast<int> (fs * 0.3);
        const auto x = sine (toneHz, fs, n, 0.2f);

        for (int t = 0; t < 3; ++t)
            for (float q : { 0.3f, 0.7071f, 2.0f })
            {
                const auto ideal = referenceGainGlide (svfTypes[t], bandHz, q, gainDb, fs, x, change, 1);
                const auto zipper = referenceGainGlide (svfTypes[t], bandHz, q, gainDb, fs, x, change, ParametricEq::kControlInterval);
                const double bound = 1.25 * maxCurvature (ideal, change, n);
                CHECK_GE (maxCurvature (zipper, change, n), 2.0 * bound); // the metric can see zipper noise

                // History 0: skipped at 0 dB since reset (stale state on resume).
                // History 1: +6 dB -> disabled -> re-enabled at 0 dB (state invalidated by the swaps).
                for (int history = 0; history < 2; ++history)
                {
                    auto eq = makeEq (fs, 1);
                    eq->setBand (0, makeBand (eqTypes[t], static_cast<float> (bandHz), history == 0 ? 0.0f : 6.0f, q));
                    eq->reset();
                    Planar buf (1, n);
                    load (buf, 0, x);
                    for (int pos = 0; pos < n; pos += block)
                    {
                        if (history == 1 && pos == block * 10)
                            eq->setBand (0, makeBand (eqTypes[t], static_cast<float> (bandHz), 6.0f, q, 12, false));
                        if (history == 1 && pos == block * 40)
                            eq->setBand (0, makeBand (eqTypes[t], static_cast<float> (bandHz), 0.0f, q));
                        if (pos == change)
                            eq->setBand (0, makeBand (eqTypes[t], static_cast<float> (bandHz), static_cast<float> (gainDb), q));
                        eq->process (buf.block (pos, std::min (block, n - pos)));
                    }
                    const auto& y = buf.ch[0];
                    CHECK_LE (maxCurvature (y, change, n), bound);
                    // ... and it ends where the ideal glide ends.
                    double endErr = 0.0;
                    for (int i = n - 1000; i < n; ++i)
                        endErr = std::max (endErr, static_cast<double> (std::abs (y[static_cast<size_t> (i)] - ideal[static_cast<size_t> (i)])));
                    CHECK_LE (endErr, 1.0e-4);
                }
            }
    }
}

TEST_CASE ("ParametricEq: abrupt type / slope / enable changes are crossfaded without clicks")
{
    const int n = static_cast<int> (kFs * 1.0);
    const int seg = 9600; // 0.2 s per configuration
    const EqBandParams sequence[] = {
        makeBand (EqBandType::Bell, 150.0f, 12.0f, 1.0f),
        makeBand (EqBandType::LowCut, 2000.0f, 0.0f, 0.7071f, 48),
        makeBand (EqBandType::LowCut, 2000.0f, 0.0f, 0.7071f, 12),
        makeBand (EqBandType::HighShelf, 3000.0f, 6.0f, 0.7071f),
        makeBand (EqBandType::HighShelf, 3000.0f, 6.0f, 0.7071f, 12, false),
    };

    auto eq = makeEq (kFs);
    eq->setBand (0, sequence[0]);
    eq->reset();
    // 151.25 Hz puts every switch point (multiples of 0.2 s) on a sine peak,
    // the worst case for a hard swap.
    const auto x = sine (151.25, kFs, n, 0.25f);
    Planar buf (2, n);
    fillAll (buf, x);
    for (int pos = 0; pos < n; pos += 32)
    {
        if (pos % seg == 0)
            eq->setBand (0, sequence[pos / seg]);
        eq->process (buf.block (pos, std::min (32, n - pos)));
    }
    CHECK (allFinite (buf));

    // Largest steady-state sample-to-sample step of any configuration (or dry).
    const auto& y = buf.ch[0];
    double steady = maxStep (x, 1, n);
    for (int s = 0; s < 5; ++s)
        steady = std::max (steady, maxStep (y, s * seg + seg / 2, (s + 1) * seg));
    for (int s = 1; s < 5; ++s)
    {
        const double transition = maxStep (y, s * seg - 16, s * seg + seg / 2);
        CHECK_LE (transition, 1.1 * steady);
    }

    // The metric is sensitive: an unfaded swap Bell(+12) -> LowCut48 (fresh
    // state) at the same instant produces a step far above the bound.
    SvfState bellState;
    const auto bell = SvfCoeffs::make (FilterType::Bell, 150.0, 1.0, 12.0, kFs);
    std::array<SvfCoeffs, 4> hp;
    std::array<SvfState, 4> hpState {};
    for (int s = 0; s < 4; ++s)
        hp[static_cast<size_t> (s)] = SvfCoeffs::make (FilterType::HighPass, 2000.0, butterworthQ (4, s), 0.0, kFs);
    std::vector<float> ref (static_cast<size_t> (2 * seg));
    for (int i = 0; i < 2 * seg; ++i)
    {
        float v = x[static_cast<size_t> (i)];
        if (i < seg)
            v = svfTick (bell, bellState, v);
        else
            for (int s = 0; s < 4; ++s)
                v = svfTick (hp[static_cast<size_t> (s)], hpState[static_cast<size_t> (s)], v);
        ref[static_cast<size_t> (i)] = v;
    }
    CHECK_GE (maxStep (ref, seg - 16, seg + 64), 3.0 * steady);
}

TEST_CASE ("ParametricEq: discrete changes use a linear ~5 ms wet/dry crossfade")
{
    for (double fs : { 44100.0, 48000.0 })
    {
        const int fade = static_cast<int> (std::lround (0.005 * fs / ParametricEq::kControlInterval)) * ParametricEq::kControlInterval;
        auto eq = makeEq (fs);
        eq->setBand (2, makeBand (EqBandType::LowCut, 4000.0f, 0.0f, 0.7071f, 24, false));
        eq->reset();

        const int n = 9600;
        const int t0 = 4800; // on the control grid
        const auto x = sine (100.0, fs, n, 0.5f);
        Planar buf (1, n);
        load (buf, 0, x);
        for (int pos = 0; pos < n; pos += 64)
        {
            if (pos == t0)
                eq->setBand (2, makeBand (EqBandType::LowCut, 4000.0f, 0.0f, 0.7071f, 24, true));
            eq->process (buf.block (pos, std::min (64, n - pos)));
        }
        const auto& y = buf.ch[0];

        // Untouched before the change.
        double preErr = 0.0;
        for (int i = 0; i < t0; ++i)
            preErr = std::max (preErr, static_cast<double> (std::abs (y[static_cast<size_t> (i)] - x[static_cast<size_t> (i)])));
        CHECK_LE (preErr, 0.0);
        // The low-cut removes the 100 Hz tone, so during the fade y = (1 - mix) x.
        double rampErr = 0.0;
        for (int i = 48; i < fade - 8; ++i)
        {
            const double mix = static_cast<double> (i + 1) / fade;
            rampErr = std::max (rampErr, std::abs (y[static_cast<size_t> (t0 + i)] - (1.0 - mix) * x[static_cast<size_t> (t0 + i)]));
        }
        CHECK_LE (rampErr, 2.0e-3);
        // Fully wet after the fade.
        CHECK_LE (peakAbs (y.data() + t0 + fade + 96, n - t0 - fade - 96), 2.0e-3);
    }
}

TEST_CASE ("ParametricEq: frequency glides in the log domain (~20 ms) without clicks")
{
    auto eq = makeEq (kFs);
    eq->setBand (0, makeBand (EqBandType::Bell, 250.0f, 12.0f, 2.0f));
    eq->reset();

    const int n = 24000;
    const int t0 = 9600;
    Planar buf (1, n);
    load (buf, 0, sine (4000.0, kFs, n, 0.1f));
    for (int pos = 0; pos < n; pos += 128)
    {
        if (pos == t0)
            eq->setBand (0, makeBand (EqBandType::Bell, 4000.0f, 12.0f, 2.0f));
        eq->process (buf.block (pos, std::min (128, n - pos)));
    }
    const auto& y = buf.ch[0];
    // 4 kHz is far from the 250 Hz bell: ~0 dB before, and still well short
    // of +12 dB 5 ms after the change (0.25 time constants).
    CHECK_NEAR (toDb (toneAmplitude (y.data() + t0 - 2400, 2400, 4000.0, kFs) / 0.1), 0.0, 0.2);
    CHECK_LE (toDb (toneAmplitude (y.data() + t0 + 240, 96, 4000.0, kFs) / 0.1), 6.0);
    // Settled after 150 ms (7.5 time constants).
    CHECK_NEAR (toDb (toneAmplitude (y.data() + t0 + 7200, 2400, 4000.0, kFs) / 0.1), 12.0, 0.1);
    CHECK_LE (maxStep (y, t0, n), 1.1 * maxStep (y, n - 2400, n));
}

//==============================================================================
TEST_CASE ("ParametricEq: getBand() round-trips clamped values; bad indices are safe")
{
    ParametricEq eq;
    eq.prepare ({ kFs, 512, 2 });

    const auto inRange = makeBand (EqBandType::HighShelf, 3150.0f, -7.25f, 1.3f, 12);
    eq.setBand (4, inRange);
    CHECK (eq.getBand (4) == inRange);

    eq.setBand (0, makeBand (EqBandType::Bell, 5.0f, 99.0f, 0.0f, 0));
    CHECK_NEAR (eq.getBand (0).frequency, 20.0, 0.0);
    CHECK_NEAR (eq.getBand (0).gainDb, 24.0, 0.0);
    CHECK_NEAR (eq.getBand (0).q, 0.1, 1.0e-7);
    CHECK (eq.getBand (0).slopeDbPerOct == 12);

    eq.setBand (1, makeBand (EqBandType::LowCut, 50000.0f, -99.0f, 1000.0f, 1000));
    CHECK_NEAR (eq.getBand (1).frequency, 20000.0, 0.0);
    CHECK_NEAR (eq.getBand (1).gainDb, -24.0, 0.0);
    CHECK_NEAR (eq.getBand (1).q, 18.0, 0.0);
    CHECK (eq.getBand (1).slopeDbPerOct == 48);

    // Slopes round to the nearest supported value.
    eq.setBand (2, makeBand (EqBandType::HighCut, 1000.0f, 0.0f, 0.7f, 30));
    CHECK (eq.getBand (2).slopeDbPerOct == 36);
    eq.setBand (2, makeBand (EqBandType::HighCut, 1000.0f, 0.0f, 0.7f, 17));
    CHECK (eq.getBand (2).slopeDbPerOct == 12);

    // NaN falls back to the defaults, infinities clamp, bad enum clamps.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    auto bad = makeBand (static_cast<EqBandType> (200), nan, inf, -inf, -5);
    eq.setBand (3, bad);
    CHECK (eq.getBand (3).type == EqBandType::BandPass);
    CHECK_NEAR (eq.getBand (3).frequency, 1000.0, 0.0);
    CHECK_NEAR (eq.getBand (3).gainDb, 24.0, 0.0);
    CHECK_NEAR (eq.getBand (3).q, 0.1, 1.0e-7);
    CHECK (eq.getBand (3).slopeDbPerOct == 12);

    // Invalid indices: setBand ignores, getBand clamps.
    const auto before = eq.getBand (ParametricEq::kMaxBands - 1);
    eq.setBand (-1, inRange);
    eq.setBand (ParametricEq::kMaxBands, inRange);
    CHECK (eq.getBand (ParametricEq::kMaxBands - 1) == before);
    CHECK (&eq.getBand (-7) == &eq.getBand (0));
    CHECK (&eq.getBand (99) == &eq.getBand (ParametricEq::kMaxBands - 1));

    // Setting the same values again is a no-op (the chain does it every block).
    const auto again = eq.getBand (4);
    eq.setBand (4, again);
    CHECK (eq.getBand (4) == inRange);
}

TEST_CASE ("ParametricEq: 20 kHz bands are stable and accurate at 44.1 kHz and 192 kHz")
{
    const EqBandParams types[] = {
        makeBand (EqBandType::Bell, 20000.0f, 24.0f, 18.0f),
        makeBand (EqBandType::Bell, 20000.0f, -24.0f, 0.1f),
        makeBand (EqBandType::LowShelf, 20000.0f, 24.0f, 18.0f),
        makeBand (EqBandType::HighShelf, 20000.0f, 24.0f, 18.0f),
        makeBand (EqBandType::LowCut, 20000.0f, 0.0f, 0.7f, 48),
        makeBand (EqBandType::HighCut, 20000.0f, 0.0f, 0.7f, 48),
        makeBand (EqBandType::Notch, 20000.0f, 0.0f, 18.0f),
        makeBand (EqBandType::BandPass, 20000.0f, 0.0f, 0.1f),
    };
    for (double fs : { 44100.0, 192000.0 })
    {
        for (const auto& p : types)
        {
            auto eq = makeEq (fs);
            eq->setBand (0, p);
            eq->reset();
            const int n = static_cast<int> (fs * 0.25);
            Planar buf (2, n);
            fillAll (buf, whiteNoise (n, 1.0f, 99));
            processInBlocks (*eq, buf, 512);
            CHECK (allFinite (buf));
            CHECK_LE (peakOf (buf), 200.0);
            // Energy in the last quarter is not growing (no slow instability).
            const double r1 = rms (buf.ch[0].data() + n / 2, n / 4);
            const double r2 = rms (buf.ch[0].data() + 3 * n / 4, n / 4);
            CHECK_LE (r2, 1.5 * r1 + 1.0e-6);

            for (double f : { 1000.0, 10000.0, 16000.0, 19000.0 })
            {
                const double predicted = ParametricEq::responseDb (&p, 1, f, fs);
                if (predicted > -60.0)
                    CHECK_NEAR (measureGainDb (*eq, f, fs), predicted, 0.1);
            }
        }

        // All 16 bands stacked in the top octave.
        auto eq = makeEq (fs);
        for (int b = 0; b < ParametricEq::kMaxBands; ++b)
            eq->setBand (b, makeBand (static_cast<EqBandType> (b % 7), 10000.0f + 700.0f * static_cast<float> (b), (b % 2) ? 12.0f : -12.0f, 0.5f + static_cast<float> (b), 12 * (1 + b % 4)));
        eq->reset();
        const int n = static_cast<int> (fs * 0.25);
        Planar buf (2, n);
        fillAll (buf, whiteNoise (n, 1.0f, 3));
        processInBlocks (*eq, buf, 333);
        CHECK (allFinite (buf));
        CHECK_LE (peakOf (buf), 1.0e3);
    }
}

TEST_CASE ("ParametricEq: output gain is smoothed, clamped and NaN-safe")
{
    auto eq = makeEq (kFs);
    const int n = 4800;
    Planar buf (1, n);

    eq->setOutputGainDb (-6.0f);
    buf.ch[0].assign (static_cast<size_t> (n), 0.5f); // DC makes the ramp directly visible
    processInBlocks (*eq, buf, 100);
    const auto& y = buf.ch[0];
    CHECK_LE (0.5 - y[0], 0.5 * 0.01); // starts at the old gain
    CHECK_NEAR (y[static_cast<size_t> (n - 1)], 0.5 * std::pow (10.0, -6.0 / 20.0), 1.0e-6);
    // Linear 20 ms ramp (960 samples), no step. The margin covers the float
    // accumulation of LinearSmoothedValue landing exactly on its target.
    CHECK_LE (maxStep (y, 1, n), 0.5 * (1.0 - std::pow (10.0, -6.0 / 20.0)) / 960.0 * 1.1);

    eq->setOutputGainDb (1000.0f);
    eq->reset();
    buf.ch[0].assign (static_cast<size_t> (n), 0.01f);
    processInBlocks (*eq, buf, 100);
    CHECK_NEAR (buf.ch[0][10], 0.01 * std::pow (10.0, 24.0 / 20.0), 1.0e-6);

    eq->setOutputGainDb (std::numeric_limits<float>::quiet_NaN());
    eq->reset();
    buf.ch[0].assign (static_cast<size_t> (n), 0.01f);
    processInBlocks (*eq, buf, 100);
    CHECK_NEAR (buf.ch[0][10], 0.01, 1.0e-7);
}

TEST_CASE ("ParametricEq: channels are independent and fewer channels than prepared work")
{
    const int n = 4096;
    const std::array<EqBandParams, 3> cfg {
        makeBand (EqBandType::Bell, 900.0f, 8.0f, 3.0f),
        makeBand (EqBandType::LowCut, 120.0f, 0.0f, 0.7f, 36),
        makeBand (EqBandType::HighShelf, 7000.0f, -5.0f),
    };

    auto multi = makeEq (kFs, kMaxChannels);
    for (size_t b = 0; b < cfg.size(); ++b)
        multi->setBand (static_cast<int> (b), cfg[b]);
    multi->reset();
    Planar buf (kMaxChannels, n);
    for (int c = 0; c < kMaxChannels; ++c)
        load (buf, c, whiteNoise (n, 0.5f, static_cast<uint32_t> (100 + c)));
    processInBlocks (*multi, buf, 256);

    for (int c = 0; c < kMaxChannels; c += 3)
    {
        auto mono = makeEq (kFs, 1);
        for (size_t b = 0; b < cfg.size(); ++b)
            mono->setBand (static_cast<int> (b), cfg[b]);
        mono->reset();
        Planar m (1, n);
        load (m, 0, whiteNoise (n, 0.5f, static_cast<uint32_t> (100 + c)));
        processInBlocks (*mono, m, 256);
        double err = 0.0;
        for (int i = 0; i < n; ++i)
            err = std::max (err, static_cast<double> (std::abs (m.ch[0][static_cast<size_t> (i)] - buf.ch[static_cast<size_t> (c)][static_cast<size_t> (i)])));
        CHECK_LE (err, 0.0);
    }

    // A 3-channel block through an 8-channel instance.
    Planar three (3, 1024);
    fillAll (three, whiteNoise (1024, 0.5f, 42));
    multi->reset();
    processInBlocks (*multi, three, 100);
    CHECK (allFinite (three));
    CHECK_LE (std::abs (three.ch[0][500] - three.ch[2][500]), 0.0);
}

//==============================================================================
TEST_CASE ("ParametricEq: reset, setters and process are allocation-free")
{
    auto eq = makeEq (kFs, 2, 512);
    const int n = 512;
    Planar buf (2, n);
    const auto noise = whiteNoise (n, 0.5f, 11);
    std::array<EqBandParams, ParametricEq::kMaxBands> cfg {};
    for (int b = 0; b < ParametricEq::kMaxBands; ++b)
        cfg[static_cast<size_t> (b)] = makeBand (static_cast<EqBandType> (b % 7), 40.0f * static_cast<float> (b + 1), 6.0f, 1.0f, 12 * (1 + b % 4));

    AllocationGuard guard;
    eq->reset();
    for (int block = 0; block < 24; ++block)
    {
        for (int b = 0; b < ParametricEq::kMaxBands; ++b)
        {
            auto p = cfg[static_cast<size_t> (b)];
            p.gainDb = static_cast<float> ((block * 3 + b) % 25) - 12.0f;
            p.enabled = ((block / 4 + b) % 3) != 0;
            if (block % 6 == 0)
                p.type = static_cast<EqBandType> ((b + block) % 7);
            eq->setBand (b, p);
        }
        eq->setOutputGainDb (static_cast<float> (block % 5) - 2.0f);
        fillAll (buf, noise);
        eq->process (buf.block());
    }
    (void) ParametricEq::responseDb (cfg.data(), ParametricEq::kMaxBands, 1000.0, kFs);
    CHECK (guard.allocations() == 0);
    CHECK (allFinite (buf));
}

TEST_CASE ("ParametricEq: robustness - silence, DC, full-scale noise, impulses, extreme settings, all rates")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    for (double fs : kRates)
    {
        const int n = static_cast<int> (fs * 0.25);
        std::vector<std::vector<float>> signals;
        signals.push_back (std::vector<float> (static_cast<size_t> (n), 0.0f));  // silence
        signals.push_back (std::vector<float> (static_cast<size_t> (n), 1.0f));  // full-scale DC
        signals.push_back (whiteNoise (n, 1.0f, 1234));                          // full-scale noise
        std::vector<float> impulses (static_cast<size_t> (n), 0.0f);
        for (int i = 0; i < n; i += 997)
            impulses[static_cast<size_t> (i)] = (i / 997) % 2 ? -1.0f : 1.0f;
        signals.push_back (impulses);

        for (int config = 0; config < 3; ++config)
        {
            auto eq = makeEq (fs);
            for (int b = 0; b < ParametricEq::kMaxBands; ++b)
            {
                EqBandParams p;
                if (config == 0) // extreme but legal: edges of every range
                    p = makeBand (static_cast<EqBandType> (b % 7), (b % 2) ? 20000.0f : 20.0f, (b % 4 < 2) ? 24.0f : -24.0f,
                                  (b % 3) ? 18.0f : 0.1f, 48);
                else if (config == 1) // garbage in
                    p = makeBand (static_cast<EqBandType> (b * 37), (b % 2) ? nan : -inf, (b % 2) ? inf : nan, (b % 2) ? nan : inf, -1000 * b);
                else // realistic dense EQ
                    p = makeBand (static_cast<EqBandType> (b % 7), 30.0f * std::pow (1.5f, static_cast<float> (b)), (b % 2) ? 9.0f : -9.0f, 2.0f, 24);
                eq->setBand (b, p);
            }
            eq->setOutputGainDb (config == 1 ? inf : 24.0f);
            eq->reset();

            for (const auto& s : signals)
            {
                Planar buf (2, n);
                fillAll (buf, s);
                eq->reset();
                processInBlocks (*eq, buf, 256);
                CHECK (allFinite (buf));
                CHECK_LE (peakOf (buf), 1.0e5);
                if (&s == &signals[0])
                    CHECK_LE (peakOf (buf), 0.0); // silence stays exactly silent
            }
        }

        // Automation storm: every band re-randomised every 64-sample block.
        auto eq = makeEq (fs);
        FastRandom rng (static_cast<uint32_t> (fs));
        Planar buf (2, n);
        fillAll (buf, whiteNoise (n, 1.0f, 8));
        for (int pos = 0; pos < n; pos += 64)
        {
            for (int b = 0; b < 4; ++b)
            {
                const auto r = [&rng] { return 0.5f + 0.5f * rng.nextBipolar(); };
                eq->setBand (b, makeBand (static_cast<EqBandType> (rng.nextU32() % 7), 20.0f * std::pow (1000.0f, r()), 24.0f * rng.nextBipolar(),
                                          0.1f + 17.9f * r() * r(), 12 * static_cast<int> (1 + rng.nextU32() % 4), (rng.nextU32() % 8) != 0));
            }
            eq->process (buf.block (pos, std::min (64, n - pos)));
        }
        CHECK (allFinite (buf));
        CHECK_LE (peakOf (buf), 1.0e5);
    }
}

TEST_CASE ("ParametricEq: output is independent of the host block size (1, 7, 64, 512)")
{
    const int n = 14336;             // 4 x 3584
    const int event = 3584;          // = lcm (7, 512): a block boundary for every size
    const auto noise = whiteNoise (n, 0.3f, 2024);
    const auto tone = sine (440.0, kFs, n, 0.3f);

    auto run = [&] (int blockSize)
    {
        auto eq = makeEq (kFs, 2, 512);
        eq->setBand (0, makeBand (EqBandType::Bell, 440.0f, 6.0f, 2.0f));
        eq->setBand (1, makeBand (EqBandType::LowCut, 60.0f, 0.0f, 0.7f, 48));
        eq->setBand (2, makeBand (EqBandType::HighShelf, 6000.0f, -4.0f));
        eq->setBand (3, makeBand (EqBandType::Notch, 3000.0f, 0.0f, 4.0f));
        eq->setBand (7, makeBand (EqBandType::BandPass, 2000.0f, 0.0f, 0.5f, 12, false));
        eq->reset();

        Planar buf (2, n);
        load (buf, 0, noise);
        for (int i = 0; i < n; ++i)
            buf.ch[1][static_cast<size_t> (i)] = noise[static_cast<size_t> (i)] * 0.5f + tone[static_cast<size_t> (i)];

        for (int pos = 0; pos < n; pos += blockSize)
        {
            if (pos == event) // continuous glides + topology swap + enable
            {
                eq->setBand (0, makeBand (EqBandType::Bell, 2500.0f, -9.0f, 0.7f));
                eq->setBand (3, makeBand (EqBandType::HighCut, 9000.0f, 0.0f, 0.7f, 24));
                eq->setBand (7, makeBand (EqBandType::BandPass, 2000.0f, 0.0f, 0.5f, 12, true));
            }
            if (pos == 2 * event) // slope change, disable, output gain ramp
            {
                eq->setBand (1, makeBand (EqBandType::LowCut, 60.0f, 0.0f, 0.7f, 12));
                eq->setBand (7, makeBand (EqBandType::BandPass, 2000.0f, 0.0f, 0.5f, 12, false));
                eq->setOutputGainDb (-3.0f);
            }
            if (pos == 3 * event) // back to 0 dB -> band goes transparent (skipped)
                eq->setBand (0, makeBand (EqBandType::Bell, 2500.0f, 0.0f, 0.7f));
            eq->process (buf.block (pos, std::min (blockSize, n - pos)));
        }
        return buf.ch;
    };

    const auto reference = run (1);
    for (int bs : { 7, 64, 512 })
    {
        const auto out = run (bs);
        double err = 0.0;
        for (size_t c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
                err = std::max (err, static_cast<double> (std::abs (out[c][static_cast<size_t> (i)] - reference[c][static_cast<size_t> (i)])));
        CHECK_LE (err, 1.0e-5);
    }
}

TEST_CASE ("ParametricEq: zero latency - the impulse response starts at sample 0 and equals the raw SVF cascade")
{
    auto eq = makeEq (kFs, 1);
    CHECK (eq->latencySamples() == 0);

    const auto bell = makeBand (EqBandType::Bell, 1000.0f, 12.0f, 1.0f);
    const auto cut = makeBand (EqBandType::LowCut, 200.0f, 0.0f, 0.7f, 48);
    eq->setBand (0, bell);
    eq->setBand (1, cut);
    eq->reset();

    const int n = 256;
    Planar buf (1, n);
    buf.ch[0][0] = 1.0e-3f; // low-level impulse
    processInBlocks (*eq, buf, 64);

    // Reference: the same sections run directly.
    std::array<SvfCoeffs, 5> secs;
    std::array<SvfState, 5> st {};
    secs[0] = SvfCoeffs::make (FilterType::Bell, 1000.0, 1.0, 12.0, kFs);
    for (int s = 0; s < 4; ++s)
        secs[static_cast<size_t> (s + 1)] = SvfCoeffs::make (FilterType::HighPass, 200.0, butterworthQ (4, s), 0.0, kFs);
    double err = 0.0;
    for (int i = 0; i < n; ++i)
    {
        float v = i == 0 ? 1.0e-3f : 0.0f;
        for (size_t s = 0; s < secs.size(); ++s)
            v = svfTick (secs[s], st[s], v);
        err = std::max (err, static_cast<double> (std::abs (v - buf.ch[0][static_cast<size_t> (i)])));
    }
    CHECK (std::abs (buf.ch[0][0]) > 1.0e-4f); // energy at n = 0: no delay
    CHECK_LE (err, 1.0e-9);
}
