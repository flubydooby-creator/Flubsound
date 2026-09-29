// The SafetyGovernor's measured loop at protection strength Normal / Strict
// (docs/11 E06 Phase 3, Protection.h, WeightedResidual.h):
//   * WeightedResidual reads a linear span as nothing, a 1 % cubic at its
//     analytic THD, a masked noise residual lower than an exposed one, and
//     does not take slow gain riding for distortion (meta-validation);
//   * the feed-forward's drive for a GR budget and the PLR meter on signals
//     with a known answer;
//   * the PI loop with its hold band settles on a synthetic plant without
//     hunting, and Off still runs the stepwise loop, unchanged;
//   * through the chain: the harmonics policy (Small Speaker Mode), the
//     Done-when rows for stationary programme, Loudness 100 and the
//     dynamics budget, and block-size independence at Normal.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/analysis/LoudnessMeter.h"
#include "flub/common/Denormals.h"
#include "flub/dsp/Biquad.h"
#include "flub/dsp/WeightedResidual.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/engine/Protection.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <iostream>
#include <vector>

using namespace flub;
using namespace flub::param;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;

struct SpanReading
{
    float plain = 0.0f, loudness = 0.0f, audible = 0.0f;
};

SpanReading measureSpan (const std::vector<float>& x, const std::vector<float>& y, int block = 512)
{
    WeightedResidual w;
    w.prepare (kFs);
    for (size_t p = 0; p < x.size(); p += static_cast<size_t> (block))
    {
        const int n = static_cast<int> (std::min (static_cast<size_t> (block), x.size() - p));
        w.process (x.data() + p, y.data() + p, n);
    }
    REQUIRE (w.hasReading());
    return { w.getPlainResidualDb(), w.getResidualDb(), w.getWeightedDb() };
}

std::vector<float> tone (double f, double amp, int n)
{
    std::vector<float> x (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
        x[static_cast<size_t> (i)] = static_cast<float> (amp * std::sin (kTwoPi * f * i / kFs));
    return x;
}

/** Every Music macro and Boost at 100 % (docs/11 E06's Done-when scene). */
void allMusicMacros (ParameterStore& store)
{
    store.set (Mode, static_cast<float> (ModeValue::Music));
    for (int id : { BoostIntensity, Macro1, Macro2, Macro3, Macro4, Macro5 })
        store.set (id, 1.0f);
}

/** Renders a stereo copy of `mono` through a chain at `strength`; calls
    perBlock (chain, endSample) after every block. */
template <class Fn>
std::vector<float> renderChain (ParameterStore& store, const std::vector<float>& mono, ProtectionStrength strength, int block, Fn&& perBlock)
{
    ProcessingChain chain (store);
    chain.prepare ({ kFs, block, 2 });
    chain.setProtectionStrength (strength);
    const int n = static_cast<int> (mono.size());
    Planar buf (2, n);
    buf.ch[0] = mono;
    buf.ch[1] = mono;
    buf.rebind();
    ScopedNoDenormals noDenormals;
    for (int pos = 0; pos < n; pos += block)
    {
        const int len = std::min (block, n - pos);
        chain.process (buf.block (pos, len));
        perBlock (chain, pos + len);
    }
    return buf.ch[0];
}

double rmsDb (const std::vector<float>& x, int from, int to)
{
    return toDb (rms (x.data() + from, to - from));
}
} // namespace

// =============================================================================
TEST_CASE ("WeightedResidual: a linear span reads nothing at any block size, a 1 % cubic its analytic THD, masked noise less than exposed harmonics, and slow gain riding is not distortion (meta-validation)")
{
    const int n = static_cast<int> (4.0 * kFs);
    const auto pink = pinkNoise (n, 0.1f, 11);

    // A linear filter and gain: under -55 dB (the frame edges of its impulse
    // response), identically at 64 / 512 / 4096-sample blocks.
    {
        auto y = pink;
        Biquad b;
        b.setCoeffs (BiquadCoeffs { 1.2, -1.5, 0.6, -1.4, 0.55 });
        for (auto& v : y)
            v = 0.7f * static_cast<float> (b.processSample (0, v));
        const auto r64 = measureSpan (pink, y, 64), r512 = measureSpan (pink, y, 512), r4096 = measureSpan (pink, y, 4096);
        std::cout << "    measured linear span: flat " << r512.plain << " dB, audible " << r512.audible << " dB\n";
        CHECK_LE (r512.plain, -55.0f);
        CHECK_LE (r512.audible, -80.0f);
        CHECK (r64.plain == r512.plain);
        CHECK (r4096.plain == r512.plain);
        CHECK (r64.audible == r4096.audible);
    }
    // y = x + a x^3 on a sine: the harmonic is a A^3 / 4 against the
    // fundamental A + 3 a A^3 / 4, at 55 Hz and 1 kHz (flat within 0.5 dB).
    for (double f0 : { 55.0, 1000.0 })
    {
        const double amp = 0.5, a = 0.2;
        const auto x = tone (f0, amp, n);
        std::vector<float> y (x.size());
        for (size_t i = 0; i < x.size(); ++i)
            y[i] = x[i] + static_cast<float> (a) * x[i] * x[i] * x[i];
        const double analytic = 20.0 * std::log10 ((a * amp * amp * amp / 4.0) / (amp + 3.0 * a * amp * amp * amp / 4.0));
        const auto r = measureSpan (x, y);
        std::cout << "    measured cubic at " << f0 << " Hz: flat " << r.plain << " dB (analytic " << analytic << "), K-weighted " << r.loudness << ", audible " << r.audible << "\n";
        CHECK_NEAR (r.plain, analytic, 0.5);
        CHECK_NEAR (r.audible, r.loudness, 0.1); // the harmonic is alone in its band: not masked
    }
    // Harmonics of a 50 Hz tone at -30 dB: exposed, so the audible reading is
    // the whole (K-weighted) residual; independent pink noise 30 dB under a
    // pink programme is in every band with the programme: masked, >= 12 dB lower.
    {
        std::vector<float> x (static_cast<size_t> (n)), y (static_cast<size_t> (n));
        const double h = 0.25 * std::pow (10.0, -30.0 / 20.0) / std::sqrt (2.0);
        for (int i = 0; i < n; ++i)
        {
            const double t = i / kFs, v = 0.25 * std::sin (kTwoPi * 50.0 * t);
            x[static_cast<size_t> (i)] = static_cast<float> (v);
            y[static_cast<size_t> (i)] = static_cast<float> (v + h * (std::sin (kTwoPi * 150.0 * t + 0.3) + std::sin (kTwoPi * 250.0 * t + 1.1)));
        }
        const auto exposed = measureSpan (x, y);
        auto noisy = pink;
        const auto e = pinkNoise (n, 0.1f * std::pow (10.0f, -30.0f / 20.0f), 77);
        for (size_t i = 0; i < noisy.size(); ++i)
            noisy[i] += e[i];
        const auto masked = measureSpan (pink, noisy);
        std::cout << "    measured exposed harmonics: flat " << exposed.plain << ", audible " << exposed.audible << "; masked noise: flat "
                  << masked.plain << ", audible " << masked.audible << " dB\n";
        CHECK_NEAR (exposed.plain, -30.0f, 0.5);
        CHECK_NEAR (exposed.audible, exposed.loudness, 0.1);
        CHECK_NEAR (masked.plain, -30.0f, 1.5); // the averaged fit takes about 1 dB of a noise residual
        CHECK_LE (masked.audible, masked.plain - 12.0f);
    }
    // A 3 dB/s gain ramp and 10 % 4 Hz tremolo on pink: gain riding, not
    // distortion (the ramp read -19 dB before the per-band gain normalisation).
    {
        auto ramp = pink, tremolo = pink;
        for (int i = 0; i < n; ++i)
        {
            ramp[static_cast<size_t> (i)] *= static_cast<float> (std::pow (10.0, -3.0 * i / kFs / 20.0));
            tremolo[static_cast<size_t> (i)] *= static_cast<float> (1.0 + 0.1 * std::sin (kTwoPi * 4.0 * i / kFs));
        }
        const auto r = measureSpan (pink, ramp), t = measureSpan (pink, tremolo);
        std::cout << "    measured 3 dB/s ramp: flat " << r.plain << " dB; 4 Hz tremolo: flat " << t.plain << " dB, audible " << t.audible << "\n";
        CHECK_LE (r.plain, -40.0f);
        CHECK_LE (t.audible, -45.0f);
    }
}

TEST_CASE ("Protection: the feed-forward finds the drive that holds the limiter at its GR budget (its programme envelope and the clipper modelled); the PLR meter reads a sine's crest against its loudness")
{
    DriveFeedForward ff;
    ff.reset();
    const float minus10 = std::pow (10.0f, -10.0f / 20.0f), minus16 = std::pow (10.0f, -16.0f / 20.0f);
    for (int i = 0; i < DriveFeedForward::kMinTicks - 1; ++i)
        ff.push (minus10);
    CHECK (std::isinf (ff.driveForBudget (-1.0f, -6.0f))); // under kMinTicks of programme: no prediction
    CHECK (! ff.hasReading());
    ff.push (minus10);
    CHECK (ff.hasReading());
    // Every tick's peak at -10 dBFS, ceiling -1: the limiter holds p + D + 1
    // dB whatever its envelope, 6 dB at D = 15 (the bisection's step: 0.015 dB).
    CHECK_NEAR (ff.driveForBudget (-1.0f, -6.0f), 15.0f, 0.02f);
    // The clipper ahead of it (headroom 0.3 dB, no crest gate, depth 3 dB)
    // takes 3 dB off each peak first: D = 18. A crest gate of 6 dB over
    // ticks whose RMS equals their peak leaves them unclipped: 15 again.
    ff.setClipper (0.3f, 0.0f, 3.0f);
    CHECK_NEAR (ff.driveForBudget (-1.0f, -6.0f), 18.0f, 0.02f);
    ff.reset();
    ff.setClipper (0.3f, 6.0f, 3.0f);
    for (int i = 0; i < 300; ++i)
        ff.pushTick (minus10, minus10);
    CHECK_NEAR (ff.driveForBudget (-1.0f, -6.0f), 15.0f, 0.02f);
    ff.setClipper (std::numeric_limits<float>::infinity(), 0.0f, 24.0f); // no clipper
    // Ticks alternating between -10 and -16 dBFS: the mean of the peaks'
    // excess, max (0, p + D + 1), is 6 dB at D = 18 (batch 1's prediction),
    // but the programme envelope (150 ms attack, 800 ms release) holds the
    // louder peaks' reduction over the quieter ticks: a lower drive.
    for (int i = 0; i < 400; ++i)
        ff.push (i % 2 == 0 ? minus10 : minus16);
    const float held = ff.driveForBudget (-1.0f, -6.0f);
    CHECK (held > 15.0f && held < 17.0f);
    ff.reset();
    for (int i = 0; i < 300; ++i)
        ff.push (1.0e-5f); // silence: no prediction
    CHECK (std::isinf (ff.driveForBudget (-1.0f, -6.0f)));

    PlrMeter plr;
    plr.prepare (kFs, 2);
    const int n = static_cast<int> (12.0 * kFs);
    Planar buf (2, n);
    buf.ch[0] = tone (1000.0, 0.25, n);
    buf.ch[1] = buf.ch[0];
    buf.rebind();
    const int tick = 480;
    float early = 0.0f;
    for (int pos = 0; pos < n; pos += tick)
    {
        plr.process (buf.block (pos, tick));
        plr.tick();
        if (pos + tick == tick * 50)
            early = plr.getPlrDb();
    }
    CHECK (early == PlrMeter::kNoReading); // under 1 s
    // A stereo 1 kHz sine of amplitude A reads 20 log10 (A) LUFS (the -0.691
    // offset cancels the K-weighting at 1 kHz) and peaks at A: PLR 0 dB, once
    // the 3 s follower has settled (12 s: 0.08 dB).
    CHECK_NEAR (plr.getPlrDb(), 0.0f, 0.15f);
}

TEST_CASE ("Protection: the measured loop settles on a steep synthetic plant without hunting; Off still runs the stepwise loop")
{
    // Plant: the audible residual is 20 log10 (harmonics scale) - 12 dB,
    // plus a 1 dB/dB-steeper term that switches in above -10 dB of scale (a
    // stage with a threshold). Budget (Music, Normal) -35 dB, set point
    // -36.5: the scale must settle between -27.5 and -24.5 dB (the hold band)
    // within 3 s and then hold still.
    SafetyGovernor g;
    g.prepare (kFs);
    g.setStrength (ProtectionStrength::Normal);
    g.setMusicMode (true);
    std::vector<float> trace;
    for (int tick = 0; tick < 1000; ++tick)
    {
        SafetyGovernor::Readings r;
        const float u = gainToDb (std::max (g.getHarmonicsScale(), 1.0e-6f));
        r.harmonicsResidualDb = u - 12.0f + std::max (0.0f, 12.0f * (u + 10.0f));
        g.updateMeasured (r, 480);
        trace.push_back (u);
    }
    const float settled = trace.back();
    std::cout << "    measured synthetic plant: harmonics scale " << settled << " dB after 10 s, " << trace[300] << " dB at 3 s\n";
    CHECK_GE (settled, -27.6f);
    CHECK_LE (settled, -24.4f);
    CHECK_NEAR (trace[300], settled, 1.0f);
    const auto [lo, hi] = std::minmax_element (trace.begin() + 500, trace.end());
    CHECK_LE (*hi - *lo, 0.2f);
    CHECK (g.getScale() == 1.0f); // no drive reading
    CHECK (g.getState() == SafetyGovernor::State::Holding);
    CHECK ((g.getReason() & SafetyGovernor::kReasonHarmonics) != 0u);

    // Off: the same readings through updateMeasured() tick the stepwise loop
    // on the GR and stage THD+N alone, exactly as update (gr, thd).
    SafetyGovernor a, b;
    a.prepare (kFs);
    b.prepare (kFs);
    for (int tick = 0; tick < 600; ++tick)
    {
        SafetyGovernor::Readings r;
        r.limiterGrDb = tick < 300 ? -9.0f : -2.0f;
        r.distortionDb = -25.0f;
        r.driveResidualDb = r.harmonicsResidualDb = 0.0f; // ignored at Off
        a.updateMeasured (r, 480);
        b.update (r.limiterGrDb, r.distortionDb, 480);
        REQUIRE (a.getScale() == b.getScale());
    }
    CHECK (a.getHarmonicsScale() == 1.0f);
    CHECK (a.getScale() == SafetyGovernor::kMinScale);
}

// =============================================================================
// Through the chain
// =============================================================================
TEST_CASE ("Chain: the harmonics policy - at Normal exposed bass harmonics get their own scale, Small Speaker Mode's are left alone, Strict governs both; Off never moves it")
{
    // All Music macros on a -12 dBFS 50 Hz sine (the E06 Done-when scene), 6 s.
    const auto sine50 = tone (50.0, std::pow (10.0, -12.0 / 20.0), static_cast<int> (6.0 * kFs));
    const auto endScale = [&] (ProtectionStrength strength, bool smallSpeaker) {
        ParameterStore store;
        allMusicMacros (store);
        store.set (BassReplaceFundamental, smallSpeaker ? 1.0f : 0.0f);
        float hs = 1.0f, lowest = 1.0f;
        renderChain (store, sine50, strength, 512, [&] (ProcessingChain& c, int) {
            hs = c.getGovernorHarmonicsScale();
            lowest = std::min (lowest, hs);
        });
        std::cout << "    measured harmonics scale, strength " << static_cast<int> (strength) << (smallSpeaker ? ", Small Speaker Mode" : "") << ": " << hs << "\n";
        return std::pair { hs, lowest };
    };
    CHECK (endScale (ProtectionStrength::Off, false).second == 1.0f);
    CHECK_LE (endScale (ProtectionStrength::Normal, false).first, 0.1f); // exposed: taken down
    CHECK (endScale (ProtectionStrength::Normal, true).second == 1.0f);  // they replace the fundamental: intended
    CHECK_LE (endScale (ProtectionStrength::Strict, true).first, 0.5f);  // Strict governs them anyway
}

TEST_CASE ("Chain: Done-when rows at Normal - stationary limiter-bound programme settles within 3 s, then holds still, and never sags more than 1 dB under its settled level; Loudness 100's audible residual is under budget within 3 s; the dynamics budget holds the output PLR")
{
    const auto pink = pinkNoise (static_cast<int> (8.0 * kFs), std::pow (10.0f, -18.0f / 20.0f), 2468);

    // (a) Gaming (no dynamics budget), Boost 100, base max.drive 24 dB, clipper
    // off: the limiter loop (feed-forward + trim) alone. Scale in dB per 10 ms.
    {
        ParameterStore store;
        store.set (Mode, static_cast<float> (ModeValue::Gaming));
        store.set (BoostIntensity, 1.0f);
        store.set (MaxDriveDb, 24.0f);
        store.set (MaxClipAmount, 0.0f);
        std::vector<float> u;
        const auto out = renderChain (store, pink, ProtectionStrength::Normal, 480, [&] (ProcessingChain& c, int) {
            u.push_back (gainToDb (std::max (c.meters().governorScale.load(), 1.0e-6f)));
        });
        const float settled = u.back();
        const auto [lo, hi] = std::minmax_element (u.begin() + 400, u.end());
        const double first = rmsDb (out, static_cast<int> (0.5 * kFs), static_cast<int> (1.0 * kFs));
        const double last = rmsDb (out, static_cast<int> (7.0 * kFs), static_cast<int> (8.0 * kFs));
        // Sag: the deepest 0.5 s window from 0.5 s on (the loop's first
        // reading is at 80 ms) under the settled level. Batch 1's loop (6 dB/s
        // of scale) dipped 4.3 dB around 2 s while the limiter's programme
        // envelope released; the release tie (SafetyGovernor::kSagAllowanceDb) holds it.
        double dip = 0.0;
        for (double t = 0.5; t < 7.5; t += 0.5)
            dip = std::max (dip, last - rmsDb (out, static_cast<int> (t * kFs), static_cast<int> ((t + 0.5) * kFs)));
        std::cout << "    measured limiter-bound: scale " << u[100] << " / " << u[300] << " / " << settled << " dB at 1 / 3 / 8 s, range after 4 s " << *hi - *lo
                  << " dB; output 7-8 s against 0.5-1 s " << last - first << " dB, deepest 0.5 s dip under the settled level " << dip << " dB\n";
        CHECK_LE (settled, -3.0f);                  // it backed off ...
        CHECK_NEAR (u[300], settled, 0.5f);         // ... settled within 3 s ...
        CHECK_LE (*hi - *lo, 0.2f);                 // ... and holds still (Done-when: oscillation <= 0.2 dB)
        CHECK_LE (dip, 1.0);                        // Done-when: sag <= 1 dB
        CHECK_GE (last - first, -1.5);              // the settled output within 1.5 dB of its first second
    }

    // (b) Music Loudness 100 + Boost 100 on the same pink: the drive span's
    // audible residual is under the Music budget by 3 s (Done-when: "residual
    // under budget within 3 s"), and the output PLR at the end is at least
    // the dynamics budget less its tolerance.
    {
        ParameterStore store;
        store.set (Mode, static_cast<float> (ModeValue::Music));
        store.set (BoostIntensity, 1.0f);
        store.set (Macro4, 1.0f);
        float at3 = 0.0f, plr = 0.0f;
        renderChain (store, pink, ProtectionStrength::Normal, 480, [&] (ProcessingChain& c, int end) {
            if (end == static_cast<int> (3.0 * kFs))
                at3 = c.getDriveResidualDb();
            plr = c.getOutputPlrDb();
        });
        const auto b = SafetyGovernor::budgetsFor (ProtectionStrength::Normal, true);
        std::cout << "    measured Loudness 100 at Normal: drive-span audible residual " << at3 << " dB at 3 s (budget " << b.residualDb
                  << "), output PLR " << plr << " dB at 8 s (budget " << b.plrDb << ")\n";
        CHECK_LE (at3, b.residualDb);
        CHECK_GE (plr, b.plrDb - 0.5f);
    }
}

TEST_CASE ("Chain: at Normal the output does not depend on the host block size while the measured loop acts (the spans, the PLR and the feed-forward step on the governor's grid)")
{
    const auto render = [] (std::initializer_list<int> pattern) {
        ParameterStore store;
        allMusicMacros (store);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, 4096, 2 });
        chain.setProtectionStrength (ProtectionStrength::Normal);
        Planar buf (2, static_cast<int> (4.0 * kFs));
        buf.ch[0] = pinkNoise (buf.numSamples(), 0.1f, 97);
        buf.ch[1] = pinkNoise (buf.numSamples(), 0.1f, 98);
        buf.rebind();
        ScopedNoDenormals noDenormals;
        float lowest = 1.0f;
        int pos = 0;
        while (pos < buf.numSamples())
            for (int n : pattern)
            {
                const int len = std::min (n, buf.numSamples() - pos);
                if (len <= 0)
                    break;
                chain.process (buf.block (pos, len));
                lowest = std::min ({ lowest, chain.meters().governorScale.load(), chain.getGovernorHarmonicsScale() });
                pos += len;
            }
        CHECK (lowest < 0.9f); // the loop acted
        return buf;
    };
    const auto a = render ({ 480 }), b = render ({ 4096 }), c = render ({ 1000, 37, 4096, 5 });
    double maxDiff = 0.0;
    for (size_t ch = 0; ch < 2; ++ch)
        for (size_t i = 0; i < a.ch[ch].size(); ++i)
            maxDiff = std::max ({ maxDiff, static_cast<double> (std::abs (a.ch[ch][i] - b.ch[ch][i])), static_cast<double> (std::abs (a.ch[ch][i] - c.ch[ch][i])) });
    CHECK_LE (maxDiff, 1e-4);
}
