// Measured THD+N (docs/TRACEABILITY.md R3.5; docs/03-dsp-design.md §11.3.5,
// §6.4 and §14.5):
//   * the per-block least-squares estimator (flub/dsp/DistortionEstimator.h)
//     against a Goertzel harmonic analysis of a sine through tanh and the
//     tube curve, and on linear gain stages;
//   * the in-stage readings of the Saturator and of the maximizer's soft
//     clipper against a harmonic analysis of what the stages actually do;
//   * the DistortionMonitor (power sum of the stages, 300 ms meter smoothing);
//   * the SafetyGovernor acting on the measured THD+N (budget -30 dB), as a
//     unit and through the chain, where base saturation alone - invisible to
//     the old clip-energy proxy - now trips it;
//   * allocation-free measurement (the RTSan annotations are checked in
//     tests/test_rtsan.cpp).
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/common/Denormals.h"
#include "flub/dsp/DistortionEstimator.h"
#include "flub/dsp/LoudnessMaximizer.h"
#include "flub/dsp/Saturator.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/engine/Protection.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
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

        constexpr int kBlock = 512;
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
    // The clip-energy proxy reads above the clipper's THD+N (it also counts
    // the in-phase part of the removed signal, a gain change). The chain
    // floors the clipper's share at it, so a SafetyGovernor mirrored from the
    // published limiter GR and clip energy ratio - the old governor inputs -
    // never holds a lower scale than the chain's own. Saturation off: the
    // clipper is the only nonlinear stage.
    ParameterStore store;
    store.set (Mode, static_cast<float> (ModeValue::Music));
    store.set (BoostIntensity, 1.0f);
    store.set (MaxDriveDb, 6.0f);
    store.set (SaturationOn, 0.0f);

    constexpr int kBlock = 512;
    ProcessingChain chain (store);
    chain.prepare ({ kFs, kBlock, 2 });
    SafetyGovernor proxyOnly;
    proxyOnly.prepare (kFs);

    const int len = static_cast<int> (kFs * 10.0);
    Planar prog (2, len);
    FastRandom rng (21);
    for (int i = 0; i < len; ++i)
    {
        const double t = i / kFs;
        const double beat = std::fmod (t, 0.5);
        const double kick = std::exp (-beat * 18.0) * std::sin (kTwoPi * (50.0 + 80.0 * std::exp (-beat * 30.0)) * beat);
        const double hat = (std::fmod (t + 0.25, 0.5) < 0.03 ? 0.3 : 0.0) * rng.nextBipolar();
        const double tone = 0.4 * std::sin (kTwoPi * 110.0 * t) + 0.15 * std::sin (kTwoPi * 440.0 * t);
        prog.ch[0][static_cast<size_t> (i)] = 0.35f * static_cast<float> (kick + hat + tone);
        prog.ch[1][static_cast<size_t> (i)] = 0.35f * static_cast<float> (kick + 0.8 * hat + tone);
    }

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
    CHECK_LE (worstExcess, 1e-6);  // every block: chain scale <= the proxy-only scale
    CHECK_LE (proxyMin, 0.9f);     // the proxy trips on this programme ...
    CHECK_LE (chainMin, proxyMin); // ... and the chain backs off at least as far
    // The floor matters here: the measured THD+N alone reads well below the proxy.
    CHECK_GE (10.0 * std::log10 (clipPow / measured), 10.0 * std::log10 (thdPow / measured) + 1.0);
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
