// The SafetyGovernor's learned state and readouts (docs/11 E06 batch 2,
// Protection.h, ProcessingChain.h):
//   * a chain's reset() at protection strength Normal keeps what the
//     governor has learned (the scales hold until the readings are back), at
//     Off it starts again from 1 as before;
//   * the crossfaded engine swap (MixEngine::configureFrom) hands each
//     strip's protection strength and learned state to the new engine;
//   * MeterBus, render.stats and `flubsound-cli quality` carry the measured
//     loop's scales, audible residuals, PLR, brightness, budgets and the
//     kReasonDynamics / kReasonHarmonics bits.
#include "TestFramework.h"
#include "TestSignals.h"

#include "Commands.h"
#include "OfflineRenderer.h"

#include "flub/common/Denormals.h"
#include "flub/common/Math.h"
#include "flub/engine/MixEngine.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/engine/Protection.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

using namespace flub;
using namespace flub::param;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;
constexpr int kBlock = 480;

float scaleDb (float scale) { return gainToDb (std::max (scale, 1.0e-6f)); }

/** docs/11 E06's limiter-bound scene: Gaming, Boost 100, base max.drive
    24 dB, clipper off (the limiter loop alone). */
void limiterBound (ParameterStore& store)
{
    store.set (Mode, static_cast<float> (ModeValue::Gaming));
    store.set (BoostIntensity, 1.0f);
    store.set (MaxDriveDb, 24.0f);
    store.set (MaxClipAmount, 0.0f);
}

/** Feeds `seconds` of the stereo copy of `x` from sample `from` through
    `chain` in kBlock blocks; perBlock (endSample) after each. */
template <class Fn>
void feed (ProcessingChain& chain, const std::vector<float>& x, int from, double seconds, Fn&& perBlock)
{
    const int n = static_cast<int> (seconds * kFs);
    Planar buf (2, kBlock);
    ScopedNoDenormals noDenormals;
    for (int pos = 0; pos < n; pos += kBlock)
    {
        for (int i = 0; i < kBlock; ++i)
            buf.ch[0][static_cast<size_t> (i)] = buf.ch[1][static_cast<size_t> (i)] = x[static_cast<size_t> (from + pos + i)];
        chain.process (buf.block());
        perBlock (from + pos + kBlock);
    }
}
} // namespace

TEST_CASE ("Chain: at Normal a reset() keeps what the governor has learned (no second loud start and back-off); at Off it starts from 1 as before (E06)")
{
    const auto pink = pinkNoise (static_cast<int> (10.0 * kFs), std::pow (10.0f, -18.0f / 20.0f), 2468);
    const auto run = [&] (ProtectionStrength strength, float& learned, float& afterReset, float& deepestAfter, float& deepestBefore) {
        ParameterStore store;
        limiterBound (store);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, kBlock, 2 });
        chain.setProtectionStrength (strength);
        deepestBefore = 0.0f;
        feed (chain, pink, 0, 6.0, [&] (int end) {
            if (end > static_cast<int> (5.0 * kFs))
                deepestBefore = std::min (deepestBefore, chain.meters().maxGainReductionDb.load());
        });
        learned = chain.meters().governorScale.load();
        chain.reset(); // a host's transport jump
        afterReset = 1.0f;
        deepestAfter = 0.0f;
        feed (chain, pink, static_cast<int> (6.0 * kFs), 1.0, [&] (int) {
            afterReset = std::max (afterReset == 1.0f ? 0.0f : afterReset, chain.meters().governorScale.load());
            deepestAfter = std::min (deepestAfter, chain.meters().maxGainReductionDb.load());
        });
    };
    float learned = 0.0f, after = 0.0f, deepAfter = 0.0f, deepBefore = 0.0f;
    run (ProtectionStrength::Normal, learned, after, deepAfter, deepBefore);
    std::cout << "    measured reset at Normal: scale " << scaleDb (learned) << " dB before, at most " << scaleDb (after)
              << " dB in the second after; deepest limiter GR " << deepBefore << " dB in the second before, " << deepAfter << " dB after\n";
    CHECK_LE (scaleDb (learned), -3.0f);                  // it had backed off ...
    CHECK_NEAR (scaleDb (after), scaleDb (learned), 0.5f); // ... and holds that through the reset
    CHECK_GE (deepAfter, deepBefore - 3.0f);              // no second over-driven start

    // Off: the stepwise loop starts again from 1 (unchanged).
    run (ProtectionStrength::Off, learned, after, deepAfter, deepBefore);
    CHECK_LE (learned, 0.99f);
    CHECK_GE (after, 0.99f);
}

TEST_CASE ("MixEngine: the crossfaded engine swap hands each strip's protection strength and the governor's learned state to the new engine (E06)")
{
    const auto pink = pinkNoise (static_cast<int> (8.0 * kFs), std::pow (10.0f, -18.0f / 20.0f), 97);
    const std::vector<StripConfig> layout { { "Game", 2, 0.0f, false } };
    MixEngine running;
    running.configure (layout, kFs, kBlock);
    limiterBound (running.params (0));
    running.chain (0).setProtectionStrength (ProtectionStrength::Normal);

    Planar in (2, kBlock), out (2, kBlock);
    const AudioBlock ib = in.block();
    const AudioBlock* inputs[] = { &ib };
    ScopedNoDenormals noDenormals;
    int t = 0;
    const auto run = [&] (MixEngine& engine, double seconds) {
        for (int k = 0; k < static_cast<int> (seconds * kFs) / kBlock; ++k, t += kBlock)
        {
            for (int i = 0; i < kBlock; ++i)
                in.ch[0][static_cast<size_t> (i)] = in.ch[1][static_cast<size_t> (i)] = pink[static_cast<size_t> (t + i)];
            engine.process (inputs, out.block());
        }
    };
    run (running, 5.0);
    const float learned = running.chain (0).meters().governorScale.load();

    auto next = std::make_unique<MixEngine>();
    next->configureFrom (running, layout, kFs, kBlock);
    CHECK (next->chain (0).getProtectionStrength() == ProtectionStrength::Normal);
    MixEngine fresh; // the same engine without the hand-over
    fresh.configure (layout, kFs, kBlock);
    limiterBound (fresh.params (0));
    fresh.chain (0).setProtectionStrength (ProtectionStrength::Normal);

    const int t0 = t;
    float handedHigh = 0.0f, freshHigh = 0.0f;
    for (int k = 0; k < static_cast<int> (0.5 * kFs) / kBlock; ++k)
    {
        run (*next, static_cast<double> (kBlock) / kFs);
        handedHigh = std::max (handedHigh, next->chain (0).meters().governorScale.load());
    }
    t = t0;
    for (int k = 0; k < static_cast<int> (0.5 * kFs) / kBlock; ++k)
    {
        run (fresh, static_cast<double> (kBlock) / kFs);
        freshHigh = std::max (freshHigh, fresh.chain (0).meters().governorScale.load());
    }
    std::cout << "    measured engine swap: scale " << scaleDb (learned) << " dB in the running engine; the new engine's highest in its first 0.5 s "
              << scaleDb (handedHigh) << " dB (without the hand-over " << scaleDb (freshHigh) << " dB)\n";
    CHECK_LE (scaleDb (learned), -3.0f);
    CHECK_NEAR (scaleDb (handedHigh), scaleDb (learned), 0.5f);
    CHECK_GE (freshHigh, 0.99f);

    // At Off the stepwise scale carries over too (a swap is not a render: Off renders are unchanged).
    MixEngine offRunning;
    offRunning.configure (layout, kFs, kBlock);
    limiterBound (offRunning.params (0));
    offRunning.params (0).set (Macro4, 1.0f);
    t = 0;
    run (offRunning, 5.0);
    const float offLearned = offRunning.chain (0).meters().governorScale.load();
    MixEngine offNext;
    offNext.configureFrom (offRunning, layout, kFs, kBlock);
    CHECK (offNext.chain (0).getProtectionStrength() == ProtectionStrength::Off);
    run (offNext, 0.02);
    CHECK_LE (offLearned, 0.9f);
    CHECK_NEAR (offNext.chain (0).meters().governorScale.load(), offLearned, 0.01f);
}

TEST_CASE ("Chain: MeterBus carries the measured loop's readouts - strength, harmonics and tonal scales, audible residuals, PLR, brightness, budgets and the kReasonDynamics / kReasonHarmonics bits; Off reads 'none' (E06)")
{
    // (a) Music Boost 100 + Loudness 100 on pink at Normal: residual, PLR
    // and the dynamics budget.
    {
        ParameterStore store;
        store.set (Mode, static_cast<float> (ModeValue::Music));
        store.set (BoostIntensity, 1.0f);
        store.set (Macro4, 1.0f);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, kBlock, 2 });
        chain.setProtectionStrength (ProtectionStrength::Normal);
        const auto pink = pinkNoise (static_cast<int> (4.0 * kFs), std::pow (10.0f, -18.0f / 20.0f), 2468);
        uint32_t reasons = 0;
        feed (chain, pink, 0, 4.0, [&] (int) { reasons |= chain.meters().governorReason.load(); });
        const auto& m = chain.meters();
        const auto b = SafetyGovernor::budgetsFor (ProtectionStrength::Normal, true);
        std::cout << "    measured MeterBus at Normal (Music Loudness 100): drive residual " << m.governorDriveResidualDb.load() << " dB, bass "
                  << m.governorBassResidualDb.load() << " dB, PLR " << m.governorPlrDb.load() << " dB, presence lift " << m.tonalLiftDb[0].load()
                  << " dB, reasons 0x" << std::hex << reasons << std::dec << "\n";
        CHECK (m.governorStrength.load() == static_cast<int> (ProtectionStrength::Normal));
        CHECK (m.governorDriveResidualDb.load() > -160.0f);
        CHECK (m.governorDriveResidualDb.load() == chain.getDriveResidualDb() || m.governorDriveResidualDb.load() >= chain.getDriveResidualDb());
        CHECK (m.governorBassResidualDb.load() > -160.0f);
        CHECK (m.governorPlrDb.load() < MeterBus::governorNoReading);
        CHECK_NEAR (m.governorPlrDb.load(), chain.getOutputPlrDb(), 1.0e-6f);
        CHECK (m.governorResidualBudgetDb.load() == b.residualDb);
        CHECK (m.governorPlrBudgetDb.load() == b.plrDb);
        CHECK (m.governorGrBudgetDb.load() == b.grDb);
        CHECK (m.tonalBudgetDb[0].load() == b.presenceDb);
        CHECK (m.tonalLiftDb[0].load() > -160.0f);
        CHECK ((reasons & (SafetyGovernor::kReasonDynamics | SafetyGovernor::kReasonDistortion)) != 0u);
    }
    // (b) Every Music macro at 100 on a 50 Hz sine: the harmonics loop.
    {
        ParameterStore store;
        store.set (Mode, static_cast<float> (ModeValue::Music));
        for (int id : { BoostIntensity, Macro1, Macro2, Macro3, Macro4, Macro5 })
            store.set (id, 1.0f);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, kBlock, 2 });
        chain.setProtectionStrength (ProtectionStrength::Normal);
        const auto tone = sine (50.0, kFs, static_cast<int> (4.0 * kFs), std::pow (10.0f, -12.0f / 20.0f));
        uint32_t reasons = 0;
        feed (chain, tone, 0, 4.0, [&] (int) { reasons |= chain.meters().governorReason.load(); });
        const auto& m = chain.meters();
        CHECK ((reasons & SafetyGovernor::kReasonHarmonics) != 0u);
        CHECK (m.governorHarmonicsScale.load() < 0.5f);
        CHECK (m.governorHarmonicsScale.load() == chain.getGovernorHarmonicsScale());
        CHECK (m.governorHarmonicsResidualDb.load() > -160.0f);
    }
    // (c) Off: nothing measured.
    {
        ParameterStore store;
        store.set (Mode, static_cast<float> (ModeValue::Music));
        store.set (BoostIntensity, 1.0f);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, kBlock, 2 });
        const auto pink = pinkNoise (static_cast<int> (1.0 * kFs), 0.1f, 5);
        feed (chain, pink, 0, 1.0, [] (int) {});
        const auto& m = chain.meters();
        CHECK (m.governorStrength.load() == 0);
        CHECK (m.governorHarmonicsScale.load() == 1.0f);
        CHECK (m.governorTonalScale.load() == 1.0f);
        CHECK (m.governorDriveResidualDb.load() == -160.0f);
        CHECK (m.governorPlrDb.load() == MeterBus::governorNoReading);
        CHECK (m.tonalLiftDb[2].load() == -160.0f);
    }
}

TEST_CASE ("CLI: render.stats and `quality` carry the measured loop's readouts (governor.measured, the dynamics / harmonics reasons) at Normal, and read 'none' at Off (E06)")
{
    using namespace flub::cli;
    const auto render = [] (const std::vector<float>& mono, const std::vector<float>& values, ProtectionStrength strength) {
        io::AudioFileData d;
        d.sampleRate = kFs;
        d.numChannels = 2;
        d.channels = { mono, mono };
        RenderSettings s;
        s.protection = strength;
        RenderResult rr;
        std::string error;
        REQUIRE (renderFile (d, values, s, rr, error));
        return rr;
    };
    std::vector<float> values (static_cast<size_t> (kNumParams));
    for (int i = 0; i < kNumParams; ++i)
        values[static_cast<size_t> (i)] = layout()[static_cast<size_t> (i)].defaultValue;
    values[static_cast<size_t> (Mode)] = static_cast<float> (ModeValue::Music);
    for (int id : { BoostIntensity, Macro1, Macro2, Macro3, Macro4, Macro5 })
        values[static_cast<size_t> (id)] = 1.0f;

    const auto tone = sine (50.0, kFs, static_cast<int> (4.0 * kFs), std::pow (10.0f, -12.0f / 20.0f));
    const auto normal = render (tone, values, ProtectionStrength::Normal);
    const auto json = renderStatsToJson (normal.stats);
    const auto& measured = json["governor"]["measured"];
    CHECK (measured["strength"].asString() == "normal");
    CHECK (measured["harmonicsScale"]["min"].asNumber() < 0.5);
    CHECK (measured["reasonPercent"]["harmonics"].asNumber() > 10.0);
    CHECK (measured["driveResidual"]["budgetDb"].asNumber() == -35.0);
    CHECK (measured["harmonicsResidual"]["maxDb"].isNumber());
    CHECK (measured["plr"]["budgetDb"].asNumber() == 8.0);
    CHECK (measured["tonalLift"]["presence"]["budgetDb"].asNumber() == 3.0);
    bool harmonicsReason = false;
    for (const auto& v : json["governor"]["end"]["reasons"].asArray())
        harmonicsReason = harmonicsReason || v.asString() == "harmonics";
    CHECK (harmonicsReason);
    CHECK (formatStats (normal.stats).find ("protection normal") != std::string::npos);

    const auto off = renderStatsToJson (render (tone, values, ProtectionStrength::Off).stats);
    CHECK (off["governor"]["measured"]["strength"].asString() == "off");
    CHECK (off["governor"]["measured"]["driveResidual"]["maxDb"].isNull());
    CHECK (off["governor"]["measured"]["plr"]["endDb"].isNull());
    CHECK (off["governor"]["measured"]["harmonicsScale"]["min"].asNumber() == 1.0);

    // `quality`: the pink render's governor, with the measured readouts.
    QualityReport q;
    q.pinkStats = normal.stats;
    const auto qj = qualityToJson (q);
    CHECK (qj["loudness"]["pinkGovernor"]["measured"]["strength"].asString() == "normal");
    CHECK (formatQuality (q).find ("Governor: on the pink") != std::string::npos);
}
