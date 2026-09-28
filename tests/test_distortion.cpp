// Measured THD+N (docs/TRACEABILITY.md R3.5; docs/03-dsp-design.md §11.3.5,
// §6.4 and §14.5):
//   * the per-block least-squares estimator (flub/dsp/DistortionEstimator.h)
//     against a Goertzel harmonic analysis of a sine through tanh and the
//     tube curve, and on linear gain stages, and the length of its analysis
//     window (closed at a block boundary at or after 25 ms);
//   * the in-stage readings of the Saturator and of the maximizer's soft
//     clipper against a harmonic analysis of what the stages actually do;
//   * the DistortionMonitor (power sum of the stages, 300 ms meter smoothing);
//   * the SafetyGovernor acting on the measured THD+N (budget -30 dB), as a
//     unit and through the chain, where base saturation alone - invisible to
//     the old clip-energy proxy - now trips it;
//   * allocation-free measurement (the RTSan annotations are checked in
//     tests/test_rtsan.cpp);
//   * the intentional harmonic generators (bass harmonics, air exciter):
//     the two-reference estimator (flub/dsp/ParallelDistortion.h), their
//     readings against a harmonic analysis, -160 dB when linear, block-size
//     independence, and the policy that they are tracked apart and are not
//     a governor input.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/common/Denormals.h"
#include "flub/dsp/DistortionEstimator.h"
#include "flub/dsp/LoudnessMaximizer.h"
#include "flub/dsp/ParallelDistortion.h"
#include "flub/dsp/Saturator.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/engine/Protection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <vector>

using namespace flub;
using namespace flub::param;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;
constexpr int kPeriod = 64; // 750 Hz at 48 kHz: every harmonic (and alias) lands on a DFT bin

/** A sin(2 pi i / period) over a whole number of periods, exactly periodic. */
std::vector<float> periodicSine (int period, int numSamples, double amplitude)
{
    std::vector<float> x (static_cast<size_t> (numSamples));
    for (int i = 0; i < numSamples; ++i)
        x[static_cast<size_t> (i)] = static_cast<float> (amplitude * std::sin (kTwoPi * (i % period) / period));
    return x;
}

/** Harmonic analysis of y (n = a whole number of periods of `period`
    samples): single-bin DFTs (Goertzel) at DC and at every harmonic up to
    Nyquist. Returns 10 log10 (non-fundamental energy / total energy), the
    non-fundamental energy being DC + harmonics 2 .. period/2; with an exactly
    periodic signal, every alias of a higher harmonic lands on one of them. */
double harmonicThdNDb (const float* y, int n, int period)
{
    std::vector<double> cosT (static_cast<size_t> (period)), sinT (static_cast<size_t> (period));
    for (int i = 0; i < period; ++i)
    {
        cosT[static_cast<size_t> (i)] = std::cos (kTwoPi * i / period);
        sinT[static_cast<size_t> (i)] = std::sin (kTwoPi * i / period);
    }
    double fundamental = 0.0, others = 0.0;
    for (int k = 0; 2 * k <= period; ++k)
    {
        double re = 0.0, im = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const auto idx = static_cast<size_t> ((static_cast<long long> (k) * (i % period)) % period);
            re += y[i] * cosT[idx];
            im -= y[i] * sinT[idx];
        }
        const double weight = (k == 0 || 2 * k == period) ? 1.0 : 2.0; // one-sided spectrum
        const double e = weight * (re * re + im * im) / n;
        (k == 1 ? fundamental : others) += e;
    }
    return 10.0 * std::log10 (others / (fundamental + others));
}

/** Steady-state reading and harmonic analysis of a Saturator on a 750 Hz sine. */
struct StageReading
{
    double measuredDb = 0.0, analysedDb = 0.0;
};

StageReading saturatorOnSine (SaturationType type, float driveDb, double amplitude)
{
    constexpr int kBlock = 512; // 8 periods at base rate, 16 at 2x
    constexpr int kBlocks = 40;
    Saturator sat;
    sat.prepare ({ kFs, kBlock, 2 });
    sat.setParams ({ type, driveDb, 1.0f, 0.0f });
    sat.reset(); // start at the target drive (no 20 ms ramp)
    Planar buf (2, kBlock * kBlocks);
    const auto s = periodicSine (kPeriod, kBlock * kBlocks, amplitude);
    std::copy (s.begin(), s.end(), buf.ch[0].begin());
    std::copy (s.begin(), s.end(), buf.ch[1].begin());
    processInBlocks (sat, buf, kBlock);
    StageReading r;
    r.measuredDb = sat.getDistortionDb();
    const int window = kBlock * 16; // the last 16 blocks, long after the latency and the filters settled
    r.analysedDb = harmonicThdNDb (buf.ch[0].data() + kBlock * kBlocks - window, window, kPeriod);
    return r;
}
} // namespace

//==============================================================================
TEST_CASE ("Distortion: on a sine through tanh (and the asymmetric tube curve) the estimator matches a Goertzel harmonic analysis within 0.02 dB from -60 to -10 dB")
{
    // The least-squares gain takes exactly the fundamental out of a static
    // curve driven by a sine (the curve keeps its phase), so the residual is
    // DC + every harmonic: the estimate should match the harmonic analysis
    // to rounding. The tube curve adds DC and even harmonics, which count.
    constexpr int kN = kPeriod * 16;
    const auto x = periodicSine (kPeriod, kN, 1.0);
    for (const bool tube : { false, true })
    {
        int inRange = 0;
        double lowest = 0.0, highest = -200.0;
        for (double driveDb = -66.0; driveDb <= 24.0; driveDb += 1.5)
        {
            const auto g = static_cast<float> (std::pow (10.0, driveDb / 20.0));
            std::vector<float> y (x.size());
            for (size_t i = 0; i < x.size(); ++i)
                y[i] = (tube ? Saturator::shape (SaturationType::Tube, g * x[i]) : std::tanh (g * x[i])) / g;
            const double estimated = DistortionEnergy::measureDb (x.data(), y.data(), kN);
            const double analysed = harmonicThdNDb (y.data(), kN, kPeriod);
            if (analysed < -60.0 || analysed > -10.0)
                continue;
            ++inRange;
            lowest = std::min (lowest, analysed);
            highest = std::max (highest, analysed);
            CHECK_NEAR (estimated, analysed, 0.02); // measured: < 0.001 dB
        }
        CHECK_GE (inRange, 12);
        CHECK_LE (lowest, -55.0); // the sweep covers the whole -60 .. -10 dB range
        CHECK_GE (highest, -14.0);
    }
}

TEST_CASE ("Distortion: a linear gain stage reads below -90 dB; the identity, silence and linear stage settings read -160 dB")
{
    const auto noise = whiteNoise (8192, 0.5f, 77);
    const int n = static_cast<int> (noise.size());
    for (const float k : { 0.25f, 0.5f, 0.9f, 1.7f, 3.98f, -1.0f })
    {
        std::vector<float> y (noise.size());
        for (size_t i = 0; i < y.size(); ++i)
            y[i] = k * noise[i];
        CHECK_LE (DistortionEnergy::measureDb (noise.data(), y.data(), n), -90.0);
    }
    CHECK (DistortionEnergy::measureDb (noise.data(), noise.data(), n) == kMinusInfDb);
    const std::vector<float> silence (512, 0.0f);
    CHECK (DistortionEnergy::measureDb (silence.data(), silence.data(), 512) == kMinusInfDb);

    // In the stages: the saturator at 0 dB drive (depth 0, +6 dB make-up) and
    // the maximizer below its clip knee are linear, and read -160 dB; a -80
    // dBFS sine through 12 dB of tape drive stays in the linear region.
    ScopedNoDenormals noDenormals;
    {
        Saturator sat;
        sat.prepare ({ kFs, 256, 2 });
        sat.setParams ({ SaturationType::Tape, 0.0f, 1.0f, 6.0f });
        sat.reset();
        Planar buf (2, 256);
        for (int b = 0; b < 20; ++b)
        {
            const auto s = whiteNoise (256, 0.5f, static_cast<uint32_t> (100 + b));
            buf.ch[0] = s;
            buf.ch[1] = s;
            buf.rebind();
            sat.process (buf.block());
            CHECK (sat.getDistortionDb() == kMinusInfDb);
        }
    }
    {
        Saturator sat;
        sat.prepare ({ kFs, 512, 2 });
        sat.setParams ({ SaturationType::Tape, 12.0f, 1.0f, 0.0f });
        sat.reset();
        Planar buf (2, 512 * 10);
        const auto s = periodicSine (kPeriod, 512 * 10, std::pow (10.0, -80.0 / 20.0));
        buf.ch[0] = s;
        buf.ch[1] = s;
        buf.rebind();
        processInBlocks (sat, buf, 512);
        CHECK_LE (sat.getDistortionDb(), -90.0);
    }
    {
        LoudnessMaximizer m;
        m.prepare ({ kFs, 512, 2 });
        Planar buf (2, 512 * 10);
        buf.ch[0] = whiteNoise (512 * 10, 0.1f, 5);
        buf.ch[1] = whiteNoise (512 * 10, 0.1f, 6);
        buf.rebind();
        for (int b = 0; b < 10; ++b)
        {
            m.process (buf.block (b * 512, 512));
            CHECK (m.getDistortionDb() == kMinusInfDb);
        }
    }
}

TEST_CASE ("Distortion: the saturator's and the soft clipper's in-stage readings match a harmonic analysis of what the stages do within 0.1 dB")
{
    ScopedNoDenormals noDenormals;

    // Saturator (2x): the reading is taken around the oversampled curve; the
    // analysis looks at the stage's actual base-rate output. They differ only
    // by what lies above the base-rate Nyquist (removed by the downsampler)
    // and the tape head bump (a linear 80 Hz bell), both negligible here.
    int saturatorCases = 0;
    for (const auto type : { SaturationType::Digital, SaturationType::Tape })
        for (const float driveDb : { 6.0f, 9.0f, 12.0f, 15.0f })
        {
            const auto r = saturatorOnSine (type, driveDb, 0.25);
            if (r.analysedDb < -60.0 || r.analysedDb > -10.0)
                continue;
            ++saturatorCases;
            CHECK_NEAR (r.measuredDb, r.analysedDb, 0.1); // measured: < 0.01 dB
        }
    CHECK_GE (saturatorCases, 6);

    // Maximizer soft clipper (4x, defaults: t = +2.15 dBFS, knee 0.5): the
    // reading is taken at 192 kHz around the curve; the reference is the
    // harmonic analysis of the same curve on the same sine at 192 kHz
    // (256 samples per period).
    constexpr int kBlock = 512;
    int clipperCases = 0;
    for (const float driveDb : { 0.0f, 3.0f, 6.0f, 9.0f, 12.0f })
    {
        LoudnessMaximizer m;
        m.setClipOversampling (4, Oversampler::Quality::High);
        m.prepare ({ kFs, kBlock, 2 });
        MaximizerParams p;
        p.driveDb = driveDb;
        p.clipCrestDb = 0.0f;     // the plain curve (docs/11 E05's crest gate and
        p.clipMaxDepthDb = 24.0f; // depth cap off): the reference analyses softClip
        m.setParams (p); // fresh: applied at once
        const double amplitude = 0.6;
        Planar buf (2, kBlock * 30);
        const auto s = periodicSine (kPeriod, kBlock * 30, amplitude);
        buf.ch[0] = s;
        buf.ch[1] = s;
        buf.rebind();
        processInBlocks (m, buf, kBlock);

        const float t = dbToGain (p.ceilingDb + lerp (6.0f, 0.3f, p.clipAmount));
        const double drive = std::pow (10.0, driveDb / 20.0);
        constexpr int kOsPeriod = kPeriod * 4;
        auto curve = periodicSine (kOsPeriod, kOsPeriod * 8, amplitude * drive);
        for (auto& v : curve)
            v = LoudnessMaximizer::softClip (v, t, p.clipKnee);
        const double analysed = harmonicThdNDb (curve.data(), static_cast<int> (curve.size()), kOsPeriod);
        if (analysed < -60.0 || analysed > -10.0)
        {
            if (amplitude * drive < t * (1.0 - 0.5 * p.clipKnee)) // entirely below the knee
                CHECK (m.getDistortionDb() == kMinusInfDb);
            continue;
        }
        ++clipperCases;
        CHECK_NEAR (m.getDistortionDb(), analysed, 0.1); // measured: < 0.001 dB
    }
    CHECK_GE (clipperCases, 3);
}

TEST_CASE ("Distortion: the readings do not depend on the host block size: a 55 Hz tone through the saturator and the clipper reads the same in 32- and 4096-sample blocks")
{
    // Over a block much shorter than a bass period, the fundamental and its
    // harmonics are nearly collinear and a per-block least-squares gain
    // would absorb most of the harmonics (per-block, this tone read 14 dB
    // low in 32-sample blocks). The stages accumulate a 25 ms window instead.
    ScopedNoDenormals noDenormals;
    const auto steadyReading = [] (Processor& stage, const std::function<float()>& reading, int block)
    {
        const int len = static_cast<int> (kFs * 1.5);
        Planar buf (2, len);
        for (int i = 0; i < len; ++i)
            buf.ch[0][static_cast<size_t> (i)] = buf.ch[1][static_cast<size_t> (i)] = static_cast<float> (0.3 * std::sin (kTwoPi * 55.0 * i / kFs));
        double power = 0.0;
        int count = 0;
        for (int pos = 0; pos < len; pos += block)
        {
            stage.process (buf.block (pos, std::min (block, len - pos)));
            if (pos >= static_cast<int> (kFs * 0.5))
            {
                power += std::pow (10.0, reading() / 10.0);
                ++count;
            }
        }
        return 10.0 * std::log10 (power / count);
    };

    double satRef = 0.0, clipRef = 0.0;
    for (const int block : { 4096, 1024, 128, 64, 32 })
    {
        Saturator sat;
        sat.prepare ({ kFs, block, 2 });
        sat.setParams ({ SaturationType::Tape, 12.0f, 1.0f, 0.0f });
        sat.reset();
        const double satDb = steadyReading (sat, [&sat] { return sat.getDistortionDb(); }, block);

        LoudnessMaximizer m;
        m.prepare ({ kFs, block, 2 });
        MaximizerParams p;
        p.driveDb = 12.0f;
        p.clipCrestDb = 0.0f; // a steady tone: clipped only with the crest gate off (docs/11 E05)
        m.setParams (p);
        const double clipDb = steadyReading (m, [&m] { return m.getDistortionDb(); }, block);

        if (block == 4096)
        {
            satRef = satDb;
            clipRef = clipDb;
            CHECK_GE (satRef, -30.0); // both measurably distort (measured: -21.1 / -39.1 dB)
            CHECK_GE (clipRef, -45.0);
            continue;
        }
        CHECK_NEAR (satDb, satRef, 0.2); // measured: < 0.05 dB
        CHECK_NEAR (clipDb, clipRef, 0.2);
    }
}

TEST_CASE ("Distortion: an analysis window closes at the first block boundary at or after 25 ms, so it spans ceil(1200 / n) * n samples in n-sample blocks at 48 kHz; the parallel window waits for the 10 ms grid")
{
    // The documented window length (DistortionEstimator.h, docs §14.5): the
    // stages add whole blocks, and the overshoot is not carried over. The
    // parallel-generator window (bass harmonics, air exciter) closes at the
    // first boundary at or after 25 ms that lies on a 10 ms (480-sample) grid
    // counted from reset(), or at the first one past 35 ms when the blocks
    // miss the grid (docs/11 E06 Phase 3: the governor's harmonics loop reads
    // it, and the chain ends its segments on that grid).
    for (const int n : { 1, 7, 480, 600, 1024, 1199, 1200, 1201, 4096 })
    {
        DistortionWindow window;
        ParallelDistortionWindow parallel;
        window.prepare (kFs);
        parallel.prepare (kFs);
        REQUIRE (window.getLength() == 1200);
        REQUIRE (parallel.getLength() == 1200);
        const int expected = (1200 + n - 1) / n * n;
        int counted = 0, closes = 0, parallelCounted = 0, parallelCloses = 0;
        long long total = 0;
        for (int blocks = 0; closes < 3 || parallelCloses < 3; ++blocks)
        {
            REQUIRE (blocks < 8 * 1200);
            counted += n;
            parallelCounted += n;
            total += n;
            float db = 0.0f;
            if (window.advance (n, db))
            {
                CHECK (counted == expected);
                counted = 0;
                ++closes;
            }
            const bool onGrid = total % 480 == 0;
            const bool due = parallelCounted >= 1200 && (onGrid || parallelCounted >= 1680);
            CHECK (parallel.advance (n, db) == due);
            if (due)
            {
                parallelCounted = 0;
                ++parallelCloses;
            }
        }
    }
    // 480-sample segments (the chain's grid): the parallel window closes every 30 ms.
    ParallelDistortionWindow parallel;
    parallel.prepare (kFs);
    float db = 0.0f;
    CHECK (! parallel.advance (480, db));
    CHECK (! parallel.advance (480, db));
    CHECK (parallel.advance (480, db));
}

TEST_CASE ("Distortion: the monitor power-sums the stages and smooths the meter in the power domain with tau = 300 ms")
{
    DistortionMonitor mon;
    mon.prepare (kFs);
    CHECK_NEAR (mon.update (-20.0f, kMinusInfDb, 480), -20.0, 1e-4);
    CHECK_NEAR (mon.update (kMinusInfDb, -25.0f, 480), -25.0, 1e-4);
    CHECK_NEAR (mon.update (-20.0f, -20.0f, 480), -20.0 + 10.0 * std::log10 (2.0), 1e-4);
    CHECK (mon.update (kMinusInfDb, kMinusInfDb, 480) == kMinusInfDb);
    CHECK_NEAR (mon.getBlockDb(), -160.0, 1e-6);

    // A step from nothing to -20 dB: after one time constant (0.3 s) the
    // smoothed power is 1 - 1/e of the step; after another 0.3 s of nothing
    // it has decayed by 1/e.
    mon.reset();
    CHECK (mon.getSmoothedDb() == kMinusInfDb);
    for (int b = 0; b < 30; ++b) // 30 x 10 ms
        mon.update (-20.0f, kMinusInfDb, 480);
    CHECK_NEAR (mon.getSmoothedDb(), -20.0 + 10.0 * std::log10 (1.0 - std::exp (-1.0)), 0.02);
    for (int b = 0; b < 500; ++b)
        mon.update (-20.0f, kMinusInfDb, 480);
    CHECK_NEAR (mon.getSmoothedDb(), -20.0, 0.01);
    for (int b = 0; b < 30; ++b)
        mon.update (kMinusInfDb, kMinusInfDb, 480);
    CHECK_NEAR (mon.getSmoothedDb(), -20.0 + 10.0 * std::log10 (std::exp (-1.0)), 0.02);
    // Block size does not change the time constant.
    mon.reset();
    for (int b = 0; b < 225; ++b) // 225 x 64 samples = 0.3 s
        mon.update (-20.0f, kMinusInfDb, 64);
    CHECK_NEAR (mon.getSmoothedDb(), -20.0 + 10.0 * std::log10 (1.0 - std::exp (-1.0)), 0.02);
}

TEST_CASE ("Distortion: the SafetyGovernor backs off when the measured THD+N of a real stage exceeds the -30 dB budget, and not when it stays under")
{
    // The saturator's own reading on a sine, fed through the monitor to the
    // governor (limiter GR 0 dB), for 10 s at a drive whose THD+N is a few dB
    // over the budget and one a few dB under it.
    ScopedNoDenormals noDenormals;
    float overDrive = -1.0f, underDrive = -1.0f;
    double overDb = 0.0, underDb = 0.0;
    for (float driveDb = 0.0f; driveDb <= 24.0f; driveDb += 0.5f)
    {
        const double measured = saturatorOnSine (SaturationType::Tape, driveDb, 0.25).measuredDb;
        if (measured > -36.0 && measured < -33.0)
        {
            underDrive = driveDb;
            underDb = measured;
        }
        if (overDrive < 0.0f && measured > -27.0 && measured < -24.0)
        {
            overDrive = driveDb;
            overDb = measured;
        }
    }
    REQUIRE (overDrive >= 0.0f);
    REQUIRE (underDrive >= 0.0f);
    CHECK_GE (overDb, SafetyGovernor::kDistortionBudgetDb + 3.0);
    CHECK_LE (underDb, SafetyGovernor::kDistortionBudgetDb - 3.0);

    for (const bool over : { true, false })
    {
        constexpr int kBlock = 480;
        Saturator sat;
        sat.prepare ({ kFs, kBlock, 2 });
        sat.setParams ({ SaturationType::Tape, over ? overDrive : underDrive, 1.0f, 0.0f });
        sat.reset();
        DistortionMonitor mon;
        mon.prepare (kFs);
        SafetyGovernor gov;
        gov.prepare (kFs);
        const auto s = periodicSine (kPeriod, kBlock * 2, 0.25); // 480 is not a multiple of 64: blocks straddle periods
        Planar buf (2, kBlock);
        float minScale = 1.0f;
        for (int b = 0; b < 1000; ++b) // 10 s
        {
            const size_t offset = static_cast<size_t> ((b * kBlock) % kPeriod);
            for (int c = 0; c < 2; ++c)
                std::copy (s.begin() + static_cast<std::ptrdiff_t> (offset), s.begin() + static_cast<std::ptrdiff_t> (offset + kBlock),
                           buf.ch[static_cast<size_t> (c)].begin());
            sat.process (buf.block());
            gov.update (0.0f, mon.update (sat.getDistortionDb(), kMinusInfDb, kBlock), kBlock);
            minScale = std::min (minScale, gov.getScale());
        }
        if (over)
        {
            CHECK_GE (gov.getAverageDistortionDb(), SafetyGovernor::kDistortionBudgetDb + 2.0);
            CHECK (gov.getScale() == 0.3f); // 15 %/s from ~0.1 s on: the floor is reached within 5 s
        }
        else
        {
            CHECK_LE (gov.getAverageDistortionDb(), SafetyGovernor::kDistortionBudgetDb - 2.0);
            CHECK (minScale == 1.0f);
        }
    }
}

TEST_CASE ("Distortion: through the chain, base saturation alone trips the governor on measured THD+N (the clip-energy proxy stays silent) and only the governed Warmth contributions are scaled")
{
    // The maximizer is off, so its limiter GR (0 dB) and clip energy
    // (-160 dB) - the old governor inputs - are silent throughout. Base tape
    // saturation at 12 dB plus Warmth 100 % (+9 dB governed) distorts a hot
    // programme far beyond -30 dB THD+N: the governor now backs off, scaling
    // only Warmth's governed amounts. A mild setting on quiet programme
    // stays under the budget and leaves the scale at exactly 1.
    struct Setting
    {
        float baseDriveDb, warmth, level;
        bool expectOver;
    };
    for (const auto& st : { Setting { 12.0f, 1.0f, 0.35f, true }, Setting { 0.0f, 0.3f, 0.1f, false } })
    {
        ParameterStore store;
        store.set (Mode, static_cast<float> (ModeValue::Music));
        store.set (MaximizerOn, 0.0f);
        store.set (SaturationOn, 1.0f);
        store.set (SatType, static_cast<float> (SaturationType::Tape));
        store.set (SatDriveDb, st.baseDriveDb);
        store.set (Macro5, st.warmth);
        std::vector<float> baseBefore (static_cast<size_t> (kNumParams)), baseAfter (static_cast<size_t> (kNumParams));
        store.snapshot (baseBefore.data());

        // 10 ms blocks: each is one processing segment that ends on a governor
        // tick (docs/11 E06), so the effective values published for a block
        // were computed with the scale published after the previous one.
        constexpr int kBlock = 480;
        ProcessingChain chain (store);
        chain.prepare ({ kFs, kBlock, 2 });

        const int len = static_cast<int> (kFs * 8.0);
        Planar prog (2, len);
        FastRandom rng (9);
        for (int i = 0; i < len; ++i)
        {
            const double t = i / kFs;
            const double beat = std::fmod (t, 0.5);
            const double kick = std::exp (-beat * 18.0) * std::sin (kTwoPi * (50.0 + 80.0 * std::exp (-beat * 30.0)) * beat);
            const double hat = (std::fmod (t + 0.25, 0.5) < 0.03 ? 0.3 : 0.0) * rng.nextBipolar();
            const double tone = 0.4 * std::sin (kTwoPi * 110.0 * t) + 0.15 * std::sin (kTwoPi * 440.0 * t);
            prog.ch[0][static_cast<size_t> (i)] = st.level * static_cast<float> (kick + hat + tone);
            prog.ch[1][static_cast<size_t> (i)] = st.level * static_cast<float> (kick + 0.8 * hat + tone);
        }

        const float warmthAmount = 9.0f * smoothstep (0.0f, 1.0f, st.warmth); // M5 -> sat.drive: +9 dB, smoothstep(0, 1, v)^1
        float prevScale = 1.0f, minScale = 1.0f;
        double maxContributionError = 0.0, peakMeterDb = -200.0;
        bool oldInputsSilent = true;
        ScopedNoDenormals noDenormals;
        for (int pos = 0; pos < len; pos += kBlock)
        {
            chain.process (prog.block (pos, std::min (kBlock, len - pos)));
            const float expected = std::clamp (st.baseDriveDb + warmthAmount * prevScale, 0.0f, 24.0f);
            maxContributionError = std::max (maxContributionError, static_cast<double> (std::abs (chain.effectiveValue (SatDriveDb) - expected)));
            const auto& m = chain.meters();
            oldInputsSilent = oldInputsSilent && m.clipEnergyRatioDb.load() == kMinusInfDb && m.maxGainReductionDb.load() == 0.0f;
            if (pos > kFs) // meter settled
                peakMeterDb = std::max (peakMeterDb, static_cast<double> (m.distortionDb.load()));
            prevScale = m.governorScale.load();
            minScale = std::min (minScale, prevScale);
        }

        CHECK (oldInputsSilent);
        CHECK_LE (maxContributionError, 1e-4);
        store.snapshot (baseAfter.data());
        CHECK (baseAfter == baseBefore); // the base sat.drive is never touched
        if (st.expectOver)
        {
            CHECK_GE (peakMeterDb, SafetyGovernor::kDistortionBudgetDb + 6.0);
            CHECK_LE (prevScale, 0.35f);
            CHECK_LE (chain.effectiveValue (SatDriveDb), st.baseDriveDb + warmthAmount * 0.35f + 1e-4f);
        }
        else
        {
            CHECK_GE (peakMeterDb, -100.0); // the saturator does measurably distort ...
            CHECK_LE (peakMeterDb, SafetyGovernor::kDistortionBudgetDb - 3.0); // ... but stays under the budget
            CHECK (minScale == 1.0f);
        }
    }
}

TEST_CASE ("Distortion: through the chain, the clipper's share of the governor input is floored at its clip energy ratio, so clipping backs the scale off at least as far as the proxy alone did")
{
    // The clip-energy proxy reads above the clipper's THD+N on a periodic
    // waveform (it also counts the in-phase part of the removed signal, a
    // gain change): about 2.6 dB here. The chain floors the clipper's share at it,
    // so the governor never holds a higher scale than a SafetyGovernor fed
    // the published limiter GR and per-block clip energy ratio (the old
    // inputs), except for the one-window delay of the floor. The
    // maximizer is alone, with the clipper at its maximum share and
    // softness, on a 750 Hz pulse (cos k w / k, k = 1..4, peak 0.82) whose
    // clip energy is over the -30 dB budget and whose THD+N is under it: the
    // floor alone makes the chain back off. (A pulse, because the clipper's
    // crest gate, docs/11 E05, leaves a steady sine unclipped.) Boost is 0,
    // so the scale changes no audio and the mirror sees exactly the chain's
    // inputs.
    ParameterStore store;
    for (int id : { GateOn, EqOn, DynEqOn, BassOn, ClarityOn, SaturationOn, SpatialOn, VirtualizerOn, CompressorOn })
        store.set (id, 0.0f);
    store.set (MaximizerOn, 1.0f);
    store.set (MaxClipAmount, 1.0f);
    store.set (MaxClipKnee, 1.0f);

    constexpr int kBlock = 512;
    ProcessingChain chain (store);
    chain.prepare ({ kFs, kBlock, 2 });
    SafetyGovernor proxyOnly;
    proxyOnly.prepare (kFs);

    const int len = static_cast<int> (kFs * 8.0);
    Planar prog (2, len);
    {
        std::array<double, kPeriod> pulse {};
        double peak = 0.0;
        for (int i = 0; i < kPeriod; ++i)
        {
            for (int k = 1; k <= 4; ++k)
                pulse[static_cast<size_t> (i)] += std::cos (kTwoPi * k * i / kPeriod) / k;
            peak = std::max (peak, std::abs (pulse[static_cast<size_t> (i)]));
        }
        for (int i = 0; i < len; ++i)
            prog.ch[0][static_cast<size_t> (i)] = static_cast<float> (0.82 * pulse[static_cast<size_t> (i % kPeriod)] / peak);
    }
    prog.ch[1] = prog.ch[0];
    prog.rebind();

    ScopedNoDenormals noDenormals;
    double worstExcess = -1.0, clipPow = 0.0, thdPow = 0.0;
    float proxyMin = 1.0f, chainMin = 1.0f;
    int measured = 0;
    for (int pos = 0; pos < len; pos += kBlock)
    {
        chain.process (prog.block (pos, std::min (kBlock, len - pos)));
        const auto& m = chain.meters();
        proxyOnly.update (m.maxGainReductionDb.load(), m.clipEnergyRatioDb.load(), kBlock);
        const float scale = m.governorScale.load();
        worstExcess = std::max (worstExcess, static_cast<double> (scale - proxyOnly.getScale()));
        proxyMin = std::min (proxyMin, proxyOnly.getScale());
        chainMin = std::min (chainMin, scale);
        if (pos > kFs) // the 300 ms meter has settled
        {
            clipPow += std::pow (10.0, m.clipEnergyRatioDb.load() / 10.0);
            thdPow += std::pow (10.0, m.distortionDb.load() / 10.0);
            ++measured;
        }
    }
    REQUIRE (measured > 0);
    const double clipDb = 10.0 * std::log10 (clipPow / measured), thdDb = 10.0 * std::log10 (thdPow / measured);
    CHECK_GE (clipDb, SafetyGovernor::kDistortionBudgetDb + 1.0); // measured: -28.2 dB
    CHECK_LE (thdDb, SafetyGovernor::kDistortionBudgetDb - 0.5);  // measured: -30.8 dB
    // Every block, the chain's scale is at most the proxy-only scale, up to
    // the 25 ms window the floor is taken over (the chain learns of the
    // clipping one window later): 0.15 /s * 25 ms = 0.00375.
    CHECK_LE (worstExcess, 0.004);
    CHECK_LE (proxyMin, 0.6f);                                    // the proxy trips ...
    CHECK_LE (chainMin, proxyMin);                                // ... and the chain backs off as far
}

TEST_CASE ("Distortion: measuring in the saturator and the clipper, the monitor and the governor update are allocation-free")
{
    ScopedNoDenormals noDenormals;
    Saturator sat;
    sat.prepare ({ kFs, 512, 2 });
    sat.setParams ({ SaturationType::Tube, 12.0f, 0.7f, -3.0f });
    LoudnessMaximizer maxi;
    maxi.prepare ({ kFs, 512, 2 });
    MaximizerParams mp;
    mp.driveDb = 12.0f;
    mp.clipCrestDb = 0.0f; // white noise stands < 6 dB out of its RMS: clip it anyway
    maxi.setParams (mp);
    DistortionMonitor mon;
    mon.prepare (kFs);
    SafetyGovernor gov;
    gov.prepare (kFs);
    Planar buf (2, 512);
    buf.ch[0] = whiteNoise (512, 0.5f, 1);
    buf.ch[1] = whiteNoise (512, 0.5f, 2);
    buf.rebind();

    AllocationGuard guard;
    float sink = 0.0f;
    for (int b = 0; b < 50; ++b)
    {
        if (b == 25)
            sat.setParams ({ SaturationType::Tape, 18.0f, 1.0f, 0.0f }); // type crossfade path too
        sat.process (buf.block());
        maxi.process (buf.block());
        const float d = mon.update (sat.getDistortionDb(), maxi.getDistortionDb(), 512);
        gov.update (maxi.getGainReductionDb(), d, 512);
        sink += mon.getSmoothedDb() + gov.getScale();
    }
    mon.reset();
    gov.reset();
    CHECK (guard.allocations() == 0);
    CHECK (std::isfinite (sink));
    CHECK (sat.getDistortionDb() > kMinusInfDb);
    CHECK (maxi.getDistortionDb() > kMinusInfDb);
}

//==============================================================================
// The intentional harmonic generators (docs/03-dsp-design.md §4.3.4, §5.3.4
// and §14.5): the bass engine's harmonics and the clarity air exciter measure
// the share of what they add in their output, where it is added (the linear
// path against the added harmonics, so their band-split filters are not
// counted). The chain tracks the readings apart from the THD+N: they are not
// a governor input and not in the THD+N meter.
#include "flub/dsp/BassEngine.h"
#include "flub/dsp/ClarityEnhancer.h"
#include "flub/dsp/ParallelDistortion.h"
#include "flub/dsp/Svf.h"

#include <complex>

namespace
{
/** Energies of y (n = a whole number of periods of `period` samples) at the
    fundamental and at DC + every other harmonic up to Nyquist, each bin
    divided by gain (k) (the power gain of a linear stage after the point of
    measurement at harmonic k, taken out again). */
struct HarmonicSplit
{
    double fundamental = 0.0, others = 0.0;

    double ratioDb() const { return 10.0 * std::log10 (others / (fundamental + others)); }
};

void addHarmonicSplit (HarmonicSplit& split, const float* y, int n, int period, const std::function<double (int)>& gain)
{
    for (int k = 0; 2 * k <= period; ++k)
    {
        double re = 0.0, im = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double phase = kTwoPi * static_cast<double> ((static_cast<long long> (k) * (i % period)) % period) / period;
            re += y[i] * std::cos (phase);
            im -= y[i] * std::sin (phase);
        }
        const double weight = (k == 0 || 2 * k == period) ? 1.0 : 2.0;
        const double e = weight * (re * re + im * im) / n / gain (k);
        (k == 1 ? split.fundamental : split.others) += e;
    }
}

const std::function<double (int)> kUnity = [] (int) { return 1.0; };

/** L = a sin, R = 0.6 a sin (different levels: every channel has its own
    least-squares gain), exactly periodic. */
Planar stereoTone (int period, int numSamples, double amplitude)
{
    Planar buf (2, numSamples);
    buf.ch[0] = periodicSine (period, numSamples, amplitude);
    buf.ch[1] = periodicSine (period, numSamples, 0.6 * amplitude);
    buf.rebind();
    return buf;
}

/** Power average of a stage's reading over every block after the first
    0.5 s of a 1.5 s signal, processed in blocks of `block` samples. */
double averageReading (Processor& stage, const std::function<float()>& reading, const Planar& signal, int block)
{
    Planar buf = signal;
    const int len = buf.numSamples();
    double power = 0.0;
    int count = 0;
    for (int pos = 0; pos < len; pos += block)
    {
        stage.process (buf.block (pos, std::min (block, len - pos)));
        if (pos >= static_cast<int> (kFs * 0.5))
        {
            power += std::pow (10.0, reading() / 10.0);
            ++count;
        }
    }
    return 10.0 * std::log10 (power / count);
}
} // namespace

TEST_CASE ("Distortion: the parallel-generator estimator counts only what neither the dry path nor the generator's linear branch explains: it matches a harmonic analysis where the dry-path fit alone reads high, and falls back to it when the branch adds no direction")
{
    // y = x + a, a = c p + h: the generated signal has a linear branch
    // (c p, phase-shifted against x, as band filters leave it) and
    // harmonics h. The harmonic analysis of y counts x + c p as fundamental
    // and h as the rest; so must the estimate.
    constexpr int kN = kPeriod * 16;
    std::vector<float> x (kN), p (kN), h (kN);
    for (int i = 0; i < kN; ++i)
    {
        const double t = kTwoPi * (i % kPeriod) / kPeriod;
        const auto k = static_cast<size_t> (i);
        x[k] = static_cast<float> (0.5 * std::sin (t));
        p[k] = static_cast<float> (0.3 * std::sin (t + 1.1));
        h[k] = static_cast<float> (0.05 * std::sin (2.0 * t + 0.3) + 0.02 * std::sin (3.0 * t));
    }
    const auto reading = [] (const std::vector<float>& xs, const std::vector<float>& ps, const std::vector<float>& as)
    {
        ParallelDistortionSums sums;
        for (size_t i = 0; i < xs.size(); ++i)
            sums.add (xs[i], ps[i], as[i]);
        return 10.0 * std::log10 (sums.residualEnergy() / sums.outputEnergy());
    };

    for (const float c : { -0.4f, 0.0f, 0.8f })
    {
        std::vector<float> a (kN), y (kN);
        for (size_t k = 0; k < a.size(); ++k)
        {
            a[k] = c * p[k] + h[k];
            y[k] = x[k] + a[k];
        }
        const double analysed = harmonicThdNDb (y.data(), kN, kPeriod);
        CHECK_NEAR (reading (x, p, a), analysed, 0.001);
        if (c != 0.0f) // the dry-path fit alone counts the quadrature part of c p
            CHECK_GE (DistortionEnergy::measureDb (x.data(), y.data(), kN), analysed + 3.0);
        // Without a dry path (the replace-fundamental case taken to the limit).
        const std::vector<float> none (kN, 0.0f);
        CHECK_NEAR (reading (none, p, a), harmonicThdNDb (a.data(), kN, kPeriod), 0.001);
    }

    // A linear branch only: the rounding floor. Nothing added: -160 dB.
    std::vector<float> lin (kN), y (kN);
    for (size_t k = 0; k < lin.size(); ++k)
        lin[k] = 0.7f * p[k];
    CHECK_LE (reading (x, p, lin), -100.0);
    {
        ParallelDistortionWindow window;
        window.prepare (kFs);
        for (int i = 0; i < window.getLength(); ++i)
            window.channel (0).add (x[static_cast<size_t> (i % kN)], p[static_cast<size_t> (i % kN)], 0.0f);
        float db = 0.0f;
        CHECK (window.advance (1440, db)); // 30 ms: on the 10 ms grid
        CHECK (db == kMinusInfDb);
    }

    // No second direction - p silent, or p collinear with x: exactly the
    // dry-path estimator (DistortionEstimator.h).
    std::vector<float> a (kN), p2 (kN);
    const std::vector<float> silentP (kN, 0.0f);
    for (size_t k = 0; k < a.size(); ++k)
    {
        a[k] = h[k] - 0.2f * x[k];
        y[k] = x[k] + a[k];
        p2[k] = -2.0f * x[k];
    }
    const double dryOnly = DistortionEnergy::measureDb (x.data(), y.data(), kN);
    CHECK_NEAR (reading (x, silentP, a), dryOnly, 1e-6);
    CHECK_NEAR (reading (x, p2, a), dryOnly, 1e-6);
}

TEST_CASE ("Distortion: the bass harmonics generator's reading matches a harmonic analysis of the stage output within 0.05 dB (40 / 80 Hz, every character, with and without replacing the fundamental)")
{
    // A steady tone below the 120 Hz cutoff: the stage adds exact harmonics
    // 2 .. 5 (band-passed) to the linear path, so the harmonic share of the
    // output is what the reading should be. Blocks of 600 samples make every
    // 25 ms window exactly 1200 samples: one period of 40 Hz, two of 80 Hz.
    ScopedNoDenormals noDenormals;
    constexpr int kBlock = 600;
    const int len = kBlock * 80; // 1 s
    int cases = 0;
    double lowest = 0.0, highest = -200.0;
    for (const int period : { 1200, 600 })
        for (const float character : { 0.0f, 0.5f, 1.0f })
            for (const float amount : { 0.25f, 1.0f })
                for (const bool replace : { false, true })
                {
                    BassEngine bass;
                    bass.prepare ({ kFs, kBlock, 2 });
                    BassEngineParams p;
                    p.harmonicsAmount = amount;
                    p.harmonicsCharacter = character;
                    p.replaceFundamental = replace;
                    bass.setParams (p);
                    bass.reset(); // at the targets, no ramps
                    Planar buf = stereoTone (period, len, 0.3);
                    processInBlocks (bass, buf, kBlock);

                    HarmonicSplit split;
                    const int window = 2400; // the last two windows, long after the filters settled
                    for (const auto& c : buf.ch)
                        addHarmonicSplit (split, c.data() + len - window, window, period, kUnity);
                    const double analysed = split.ratioDb();
                    CHECK_NEAR (bass.getDistortionDb(), analysed, 0.05); // measured: < 0.001 dB
                    lowest = std::min (lowest, analysed);
                    highest = std::max (highest, analysed);
                    ++cases;
                }
    CHECK (cases == 24);
    CHECK_LE (lowest, -12.0); // the cases span a wide range of harmonic shares (measured: -12.3 dB) ...
    CHECK_GE (highest, -0.1); // ... up to nearly all of the output with the fundamental replaced
}

TEST_CASE ("Distortion: the air exciter's reading matches a harmonic analysis of the stage output (the linear air shelf taken out) within 0.05 dB, also on the band's skirt")
{
    // Tones in the 3.5 - 7 kHz band get their 2nd and 3rd harmonics added;
    // the 9.6 kHz tone on the skirt is shaped against the -3 dB floor, below
    // full scale, so the 3rd-order term also adds a fundamental: a linear
    // branch, phase-shifted by the band filters (the fit against the linear
    // branch takes it out - against the dry path alone this tone read 12 dB
    // high - and the analysis counts it as fundamental), plus aliases (which
    // both count). The reading is taken before the linear 10 kHz air shelf,
    // so the analysis divides every bin by the shelf's power gain. Blocks of
    // 480 samples: every window is 1440 samples, whole periods of each tone.
    ScopedNoDenormals noDenormals;
    constexpr int kBlock = 480;
    const int len = kBlock * 100; // 1 s
    int cases = 0;
    double lowest = 0.0, highest = -200.0;
    for (const int period : { 12, 10, 8, 5 }) // 4, 4.8, 6, 9.6 kHz
        for (const float air : { 0.25f, 0.5f, 1.0f })
        {
            ClarityEnhancer clarity;
            clarity.prepare ({ kFs, kBlock, 2 });
            ClarityParams p;
            p.air = air;
            clarity.setParams (p);
            clarity.reset();
            Planar buf = stereoTone (period, len, 0.3);
            processInBlocks (clarity, buf, kBlock);

            const auto shelf = SvfCoeffs::make (FilterType::HighShelf, 10000.0, 0.70710678118654752, 2.0 * air, kFs);
            const auto shelfPower = [&shelf, period] (int k) { return std::norm (shelf.response (kFs * k / period, kFs)); };
            HarmonicSplit split;
            const int window = 1440;
            for (const auto& c : buf.ch)
                addHarmonicSplit (split, c.data() + len - window, window, period, shelfPower);
            const double analysed = split.ratioDb();
            CHECK_NEAR (clarity.getDistortionDb(), analysed, 0.05); // measured: < 0.001 dB
            lowest = std::min (lowest, analysed);
            highest = std::max (highest, analysed);
            ++cases;
        }
    CHECK (cases == 12);
    CHECK_LE (lowest, -30.0);
    CHECK_GE (highest, -15.0);
}

TEST_CASE ("Distortion: linear settings of the bass engine and the clarity enhancer read -160 dB: harmonics / air off with every other stage engaged, after switching them off, and on silence")
{
    ScopedNoDenormals noDenormals;
    constexpr int kBlock = 256;
    Planar prog (2, kBlock * 200); // ~1.07 s
    {
        const auto noise = whiteNoise (prog.numSamples(), 0.2f, 31);
        for (int i = 0; i < prog.numSamples(); ++i)
        {
            const auto s = static_cast<size_t> (i);
            const auto tone = static_cast<float> (0.3 * std::sin (kTwoPi * 55.0 * i / kFs) + 0.1 * std::sin (kTwoPi * 5000.0 * i / kFs));
            prog.ch[0][s] = tone + noise[s];
            prog.ch[1][s] = tone - 0.5f * noise[s];
        }
    }
    const Planar silence (2, kBlock * 20);

    // Runs the stage over `signal` block by block; returns whether every
    // reading after the first `skip` blocks was exactly -160 dB.
    const auto allSilent = [] (Processor& stage, const std::function<float()>& reading, Planar signal, int skip)
    {
        bool ok = true;
        for (int b = 0; b * kBlock < signal.numSamples(); ++b)
        {
            stage.process (signal.block (b * kBlock, kBlock));
            ok = ok && (b < skip || reading() == kMinusInfDb);
        }
        return ok;
    };

    // Bass: boost with protection, subsonic, mono bass, replace-fundamental
    // high-pass and tighten, but no harmonics.
    BassEngineParams bp;
    bp.boostDb = 12.0f;
    bp.subsonicHz = 30.0f;
    bp.monoBelowHz = 100.0f;
    bp.replaceFundamental = true;
    bp.tighten = 0.8f;
    {
        BassEngine bass;
        bass.prepare ({ kFs, kBlock, 2 });
        bass.setParams (bp);
        const auto reading = [&bass] { return bass.getDistortionDb(); };
        CHECK (allSilent (bass, reading, prog, 0));
        // Harmonics on: measurable. Switched off, the mix ramps out over
        // 20 ms, then the path stops: from the second window after that on
        // (20 blocks = 107 ms) every reading is -160 dB again.
        bp.harmonicsAmount = 0.5f;
        bass.setParams (bp);
        Planar work = prog;
        processInBlocks (bass, work, kBlock);
        CHECK_GE (bass.getDistortionDb(), -30.0);
        bp.harmonicsAmount = 0.0f;
        bass.setParams (bp);
        CHECK (allSilent (bass, reading, prog, 20));
    }
    {
        // Harmonics on, digital silence in: nothing is generated.
        BassEngine bass;
        bass.prepare ({ kFs, kBlock, 2 });
        bp.harmonicsAmount = 1.0f;
        bass.setParams (bp);
        CHECK (allSilent (bass, [&bass] { return bass.getDistortionDb(); }, silence, 0));
    }

    // Clarity: transient shaper, de-mud and presence engaged, no air.
    ClarityParams cp;
    cp.attackDb = 6.0f;
    cp.sustainDb = -6.0f;
    cp.presence = 1.0f;
    cp.deMud = 1.0f;
    {
        ClarityEnhancer clarity;
        clarity.prepare ({ kFs, kBlock, 2 });
        clarity.setParams (cp);
        const auto reading = [&clarity] { return clarity.getDistortionDb(); };
        CHECK (allSilent (clarity, reading, prog, 0));
        cp.air = 1.0f;
        clarity.setParams (cp);
        Planar work = prog;
        processInBlocks (clarity, work, kBlock);
        CHECK_GE (clarity.getDistortionDb(), -60.0);
        cp.air = 0.0f;
        clarity.setParams (cp);
        CHECK (allSilent (clarity, reading, prog, 20));
    }
    {
        ClarityEnhancer clarity;
        clarity.prepare ({ kFs, kBlock, 2 });
        cp.air = 1.0f;
        clarity.setParams (cp);
        CHECK (allSilent (clarity, [&clarity] { return clarity.getDistortionDb(); }, silence, 0));
    }
}

TEST_CASE ("Distortion: the bass harmonics and air exciter readings do not depend on the host block size: a 55 Hz tone and a 4.4 kHz tone read the same in 32- and 4096-sample blocks")
{
    ScopedNoDenormals noDenormals;
    const int len = static_cast<int> (kFs * 1.5);
    Planar low (2, len), high (2, len);
    for (int i = 0; i < len; ++i)
    {
        const auto s = static_cast<size_t> (i);
        low.ch[0][s] = low.ch[1][s] = static_cast<float> (0.3 * std::sin (kTwoPi * 55.0 * i / kFs));
        high.ch[0][s] = high.ch[1][s] = static_cast<float> (0.2 * std::sin (kTwoPi * 4400.0 * i / kFs) + 0.2 * std::sin (kTwoPi * 300.0 * i / kFs));
    }

    double bassRef = 0.0, airRef = 0.0;
    for (const int block : { 4096, 1024, 128, 64, 32 })
    {
        BassEngine bass;
        bass.prepare ({ kFs, block, 2 });
        BassEngineParams bp;
        bp.harmonicsAmount = 0.6f;
        bass.setParams (bp);
        bass.reset();
        const double bassDb = averageReading (bass, [&bass] { return bass.getDistortionDb(); }, low, block);

        ClarityEnhancer clarity;
        clarity.prepare ({ kFs, block, 2 });
        ClarityParams cp;
        cp.air = 1.0f;
        clarity.setParams (cp);
        clarity.reset();
        const double airDb = averageReading (clarity, [&clarity] { return clarity.getDistortionDb(); }, high, block);

        if (block == 4096)
        {
            bassRef = bassDb;
            airRef = airDb;
            CHECK_GE (bassRef, -30.0); // both measurably add harmonics
            CHECK_GE (airRef, -45.0);
            continue;
        }
        CHECK_NEAR (bassDb, bassRef, 0.2); // measured: < 0.13 dB (1.4 periods of 55 Hz per window at 32 samples)
        CHECK_NEAR (airDb, airRef, 0.2);   // measured: < 0.002 dB
    }
}

TEST_CASE ("Distortion: the monitor keeps the harmonic generators apart: power-summed and smoothed with tau = 300 ms, never in the THD+N block value or meter")
{
    DistortionMonitor mon;
    mon.prepare (kFs);
    CHECK_NEAR (mon.updateHarmonics (-20.0f, kMinusInfDb, 480), -20.0, 1e-4);
    CHECK_NEAR (mon.updateHarmonics (kMinusInfDb, -25.0f, 480), -25.0, 1e-4);
    CHECK_NEAR (mon.updateHarmonics (-20.0f, -20.0f, 480), -20.0 + 10.0 * std::log10 (2.0), 1e-4);
    CHECK (mon.updateHarmonics (kMinusInfDb, kMinusInfDb, 480) == kMinusInfDb);
    CHECK_NEAR (mon.getHarmonicsBlockDb(), -160.0, 1e-6);

    // Harmonics alone: the THD+N side stays at -160 dB; and the other way round.
    mon.reset();
    for (int b = 0; b < 30; ++b) // 30 x 10 ms
    {
        mon.update (kMinusInfDb, kMinusInfDb, 480);
        mon.updateHarmonics (-6.0f, -40.0f, 480);
    }
    CHECK (mon.getBlockDb() == kMinusInfDb);
    CHECK (mon.getSmoothedDb() == kMinusInfDb);
    const double step = 10.0 * std::log10 (std::pow (10.0, -0.6) + std::pow (10.0, -4.0));
    CHECK_NEAR (mon.getHarmonicsBlockDb(), step, 1e-4);
    CHECK_NEAR (mon.getSmoothedHarmonicsDb(), step + 10.0 * std::log10 (1.0 - std::exp (-1.0)), 0.02);
    for (int b = 0; b < 30; ++b)
    {
        mon.update (-35.0f, kMinusInfDb, 480);
        mon.updateHarmonics (kMinusInfDb, kMinusInfDb, 480);
    }
    CHECK_NEAR (mon.getBlockDb(), -35.0, 1e-4);
    CHECK_NEAR (mon.getSmoothedHarmonicsDb(), step + 10.0 * std::log10 ((1.0 - std::exp (-1.0)) * std::exp (-1.0)), 0.02);
    mon.reset();
    CHECK (mon.getHarmonicsBlockDb() == kMinusInfDb);
    CHECK (mon.getSmoothedHarmonicsDb() == kMinusInfDb);
}

TEST_CASE ("Distortion: through the chain, the bass harmonics and the air exciter are not a governor input: harmonics far over the -30 dB budget leave the scale at exactly 1 and the THD+N meter at -160 dB, where a governor fed them would back off to its floor")
{
    // Only the bass engine (harmonics at full amount, fundamental replaced
    // above a 150 Hz speaker limit: the laptop setting, which reads about
    // -2 dB on programme) and the clarity enhancer (air at full) run, so the
    // chain's input reaches both unchanged. A mirror of the two stages on the
    // same programme shows what their readings are; a SafetyGovernor fed
    // them backs off, while the chain's governor - which does not take them
    // (docs/03 §14.5: at full or any useful weight, the governor would pin
    // at 0.3 on every harmonic-bass factory preset) - holds exactly 1.
    ParameterStore store;
    store.set (Mode, static_cast<float> (ModeValue::Music));
    for (int id : { GateOn, EqOn, DynEqOn, SaturationOn, SpatialOn, VirtualizerOn, CompressorOn, MaximizerOn })
        store.set (id, 0.0f);
    store.set (BassOn, 1.0f);
    store.set (BassHarmonics, 1.0f);
    store.set (BassHarmonicsCutoff, 150.0f);
    store.set (BassReplaceFundamental, 1.0f);
    store.set (ClarityOn, 1.0f);
    store.set (ClarityAir, 1.0f);

    constexpr int kBlock = 512;
    ProcessingChain chain (store);
    chain.prepare ({ kFs, kBlock, 2 });
    BassEngine bass;
    bass.prepare ({ kFs, kBlock, 2 });
    BassEngineParams bp;
    bp.harmonicsAmount = 1.0f;
    bp.harmonicsCutoff = 150.0f;
    bp.replaceFundamental = true;
    bass.setParams (bp);
    ClarityEnhancer clarity;
    clarity.prepare ({ kFs, kBlock, 2 });
    ClarityParams cp;
    cp.air = 1.0f;
    clarity.setParams (cp);
    SafetyGovernor fedHarmonics;
    fedHarmonics.prepare (kFs);
    DistortionMonitor harmonicsMirror; // what MeterBus::harmonicsDb should read
    harmonicsMirror.prepare (kFs);

    const int len = static_cast<int> (kFs * 8.0);
    Planar prog (2, len);
    FastRandom rng (21);
    for (int i = 0; i < len; ++i)
    {
        const double t = i / kFs;
        const double beat = std::fmod (t, 0.5);
        const double kick = std::exp (-beat * 18.0) * std::sin (kTwoPi * (50.0 + 80.0 * std::exp (-beat * 30.0)) * beat);
        const double hat = (std::fmod (t + 0.25, 0.5) < 0.03 ? 0.3 : 0.0) * rng.nextBipolar();
        const double bassLine = 0.4 * std::sin (kTwoPi * 55.0 * t);
        const double pad = 0.15 * std::sin (kTwoPi * 440.0 * t) + 0.1 * std::sin (kTwoPi * 660.0 * t + 0.3);
        prog.ch[0][static_cast<size_t> (i)] = 0.35f * static_cast<float> (kick + hat + bassLine + pad);
        prog.ch[1][static_cast<size_t> (i)] = 0.35f * static_cast<float> (kick + 0.8 * hat + bassLine + 0.7 * pad);
    }
    Planar mirror = prog;

    ScopedNoDenormals noDenormals;
    bool scaleAtOne = true, meterSilent = true;
    double bassPow = 0.0, airPow = 0.0;
    int measured = 0;
    for (int pos = 0; pos < len; pos += kBlock)
    {
        const int n = std::min (kBlock, len - pos);
        chain.process (prog.block (pos, n));
        bass.process (mirror.block (pos, n));
        clarity.process (mirror.block (pos, n));
        const float bassDb = bass.getDistortionDb(), airDb = clarity.getDistortionDb();
        fedHarmonics.update (0.0f, DistortionMonitor::combineDb (bassDb, airDb), n);
        harmonicsMirror.updateHarmonics (bassDb, airDb, n);
        const auto& m = chain.meters();
        scaleAtOne = scaleAtOne && m.governorScale.load() == 1.0f;
        meterSilent = meterSilent && m.distortionDb.load() == kMinusInfDb && m.clipEnergyRatioDb.load() == kMinusInfDb;
        if (pos > kFs)
        {
            bassPow += std::pow (10.0, bassDb / 10.0);
            airPow += std::pow (10.0, airDb / 10.0);
            ++measured;
        }
    }
    REQUIRE (measured > 0);
    CHECK_GE (10.0 * std::log10 (bassPow / measured), SafetyGovernor::kDistortionBudgetDb + 20.0); // measured: -2.1 dB
    CHECK_GE (10.0 * std::log10 (airPow / measured), -70.0);                                      // measured: -44.9 dB
    CHECK (fedHarmonics.getScale() == 0.3f);
    CHECK (scaleAtOne);
    CHECK (meterSilent);
    // The harmonics reading is published on MeterBus (docs/11 E59 gap): the
    // monitor's 300 ms power-smoothed sum of the two generators. The chain's
    // stages close their 25 ms windows on its 10 ms segment grid, the mirror's
    // on 512-sample blocks, so the two agree within a fraction of a dB.
    const float published = chain.meters().harmonicsDb.load();
    CHECK_GE (published, SafetyGovernor::kDistortionBudgetDb + 20.0f);
    CHECK_NEAR (published, harmonicsMirror.getSmoothedHarmonicsDb(), 0.5);
    // The chain's stages see what the mirror sees: the outputs agree (the
    // chain's output is delayed by its constant latency).
    const int latency = chain.getLatencySamples();
    double maxDiff = 0.0;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i + latency < len; ++i)
            maxDiff = std::max (maxDiff, static_cast<double> (std::abs (prog.ch[static_cast<size_t> (c)][static_cast<size_t> (i + latency)]
                                                                       - mirror.ch[static_cast<size_t> (c)][static_cast<size_t> (i)])));
    CHECK_LE (maxDiff, 1e-5);
}

TEST_CASE ("Distortion: measuring in the bass harmonics generator and the air exciter, and the monitor's harmonics update, are allocation-free")
{
    ScopedNoDenormals noDenormals;
    BassEngine bass;
    bass.prepare ({ kFs, 512, 2 });
    BassEngineParams bp;
    bp.harmonicsAmount = 0.8f;
    bp.boostDb = 6.0f;
    bass.setParams (bp);
    ClarityEnhancer clarity;
    clarity.prepare ({ kFs, 512, 2 });
    ClarityParams cp;
    cp.air = 1.0f;
    cp.presence = 0.5f;
    clarity.setParams (cp);
    DistortionMonitor mon;
    mon.prepare (kFs);
    Planar buf (2, 512);
    buf.ch[0] = whiteNoise (512, 0.5f, 3);
    buf.ch[1] = whiteNoise (512, 0.5f, 4);
    buf.rebind();

    AllocationGuard guard;
    float sink = 0.0f;
    float bassSeen = kMinusInfDb, airSeen = kMinusInfDb;
    for (int b = 0; b < 60; ++b)
    {
        if (b == 30)
        {
            // Switching off and on again (window reset / ramp paths).
            bp.harmonicsAmount = 0.0f;
            cp.air = 0.0f;
            bass.setParams (bp);
            clarity.setParams (cp);
        }
        if (b == 45)
        {
            bp.harmonicsAmount = 0.5f;
            cp.air = 0.5f;
            bass.setParams (bp);
            clarity.setParams (cp);
        }
        bass.process (buf.block());
        clarity.process (buf.block());
        bassSeen = std::max (bassSeen, bass.getDistortionDb());
        airSeen = std::max (airSeen, clarity.getDistortionDb());
        sink += mon.updateHarmonics (bass.getDistortionDb(), clarity.getDistortionDb(), 512) + mon.getSmoothedHarmonicsDb();
    }
    bass.reset();
    clarity.reset();
    mon.reset();
    CHECK (guard.allocations() == 0);
    CHECK (std::isfinite (sink));
    CHECK (bassSeen > kMinusInfDb);
    CHECK (airSeen > kMinusInfDb);
}
