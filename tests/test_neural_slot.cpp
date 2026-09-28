// Tests for ProcessingChain's neural slot (docs/09-future-roadmap.md §1.1):
// empty by default and then invisible (latency and output bit-identical to a
// chain without it), an eligible model adds exactly its fixed latency L, an
// ineligible one or one whose safety frames do not cover the host buffer
// stays out with the reason reported, an Offline render with no waiting gets
// every frame's result, a model ahead of the maximizer cannot push past the
// true-peak ceiling, the slot's bypass keeps the latency, process() stays
// allocation-free and clearNeuralModel() gives the original latency back.
//
// Determinism: the models run on the real worker thread. Where the model's
// controls matter, the test waits for the worker after every block
// (AsyncModelProcessor::getPendingFrames() == 0; the bound is a hang guard)
// with host blocks no longer than safetyFrames * frameSize, so every result
// is on time (tests/test_neural.cpp explains why). An identity model's gain
// is 1 whether or not its result arrives, so those renders need no waiting,
// and an Offline slot runs the model inside process(), so it needs none either.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/analysis/PeakMeters.h"
#include "flub/common/Denormals.h"
#include "flub/common/Math.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/neural/ReferenceRunners.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>

using namespace flub;
using namespace flub::param;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;

// The chain's latency per profile at 48 kHz with no model (test_engine.cpp's
// MixEngine latency test and the table in ProcessingChain.h).
constexpr int kQualityLatency = 1352, kBalancedLatency = 192, kLowLatency = 100;

/** Drum-like programme (the one test_engine.cpp uses). */
Planar makeProgramme (int numSamples, float level = 0.5f, uint32_t seed = 7)
{
    Planar p (2, numSamples);
    FastRandom rng (seed);
    for (int i = 0; i < numSamples; ++i)
    {
        const double t = i / kFs;
        const double beat = std::fmod (t, 0.5);
        const double kick = std::exp (-beat * 18.0) * std::sin (kTwoPi * (50.0 + 80.0 * std::exp (-beat * 30.0)) * beat);
        const double hat = (std::fmod (t + 0.25, 0.5) < 0.03 ? 0.3 : 0.0) * rng.nextBipolar();
        const double bassLine = 0.4 * std::sin (kTwoPi * 55.0 * t);
        const double pad = 0.15 * std::sin (kTwoPi * 440.0 * t) + 0.1 * std::sin (kTwoPi * 660.0 * t + 0.3);
        p.ch[0][static_cast<size_t> (i)] = level * static_cast<float> (kick + hat + bassLine + pad);
        p.ch[1][static_cast<size_t> (i)] = level * static_cast<float> (kick + 0.8 * hat + bassLine + 0.7 * pad);
    }
    return p;
}

template <typename Pred>
bool waitUntil (Pred pred)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (20);
    while (! pred())
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

/** Renders buf in place; with waitForModel, lets the worker finish every
    submitted frame after each block (see the file comment). */
void render (ProcessingChain& chain, Planar& buf, int blockSize, bool waitForModel = false)
{
    ScopedNoDenormals noDenormals;
    const AsyncModelProcessor* model = chain.getNeuralProcessor();
    const int n = buf.numSamples();
    for (int pos = 0; pos < n; pos += blockSize)
    {
        chain.process (buf.block (pos, std::min (blockSize, n - pos)));
        if (waitForModel && model != nullptr)
            REQUIRE (waitUntil ([model] { return model->getPendingFrames() == 0; }));
    }
}

void bypassAllModules (ParameterStore& s)
{
    for (int id : { GateOn, EqOn, DynEqOn, BassOn, ClarityOn, SaturationOn, SpatialOn, VirtualizerOn, CompressorOn, MaximizerOn })
        s.set (id, 0.0f);
}

bool bitIdentical (const Planar& a, const Planar& b)
{
    if (a.numChannels() != b.numChannels() || a.numSamples() != b.numSamples())
        return false;
    for (size_t c = 0; c < a.ch.size(); ++c)
        if (std::memcmp (a.ch[c].data(), b.ch[c].data(), sizeof (float) * a.ch[c].size()) != 0)
            return false;
    return true;
}

/** max |a[i + delay] - b[i]| over both channels, for i >= from. */
double maxDelayedError (const Planar& a, const Planar& b, int delay, int from)
{
    double err = 0.0;
    for (size_t c = 0; c < a.ch.size(); ++c)
        for (int i = from; i + delay < a.numSamples(); ++i)
            err = std::max (err, static_cast<double> (std::abs (a.ch[c][static_cast<size_t> (i + delay)] - b.ch[c][static_cast<size_t> (i)])));
    return err;
}

/** A model with a test-chosen description: constant gain, optionally
    throwing from prepare() (a session that fails to load). */
class DescribedRunner : public ModelRunner
{
public:
    DescribedRunner (ModelDescription d, float gainDb = 0.0f, bool throwOnPrepare = false)
        : desc (d), gain (dbToGain (gainDb)), throws (throwOnPrepare) {}

    ModelDescription describe() const override { return desc; }

    void prepare (double) override
    {
        if (throws)
            throw std::runtime_error ("model file missing");
    }

    bool run (const float*, float* outControls) override
    {
        std::fill (outControls, outControls + desc.numControls, gain);
        return true;
    }

private:
    ModelDescription desc;
    float gain;
    bool throws;
};

/** A different gain for every frame (cycling through kGains, restarting at
    the runner's reset()), so a control applied to the wrong frame shows.
    Records the thread run() is called on. */
class PerFrameGainRunner : public ModelRunner
{
public:
    static constexpr float kGains[3] { 0.5f, 0.25f, 0.8f };

    explicit PerFrameGainRunner (int frameSize) : frame (frameSize) {}

    ModelDescription describe() const override
    {
        ModelDescription d;
        d.frameSize = frame;
        return d;
    }

    void reset() override { runs = 0; }

    bool run (const float*, float* outControls) override
    {
        outControls[0] = kGains[runs++ % 3];
        runThread = std::this_thread::get_id();
        return true;
    }

    std::thread::id runThread;

private:
    int frame;
    int runs = 0;
};

ModelDescription description (int frameSize, double sampleRate = 0.0)
{
    ModelDescription d;
    d.frameSize = frameSize;
    d.sampleRate = sampleRate;
    return d;
}

/** One safety frame by default (L = 2 * frameSize), fast worker polling. A
    host block may be at most safetyFrames * frameSize (BlockTooLarge). */
NeuralSlotConfig slotConfig (int safetyFrames = 1)
{
    NeuralSlotConfig c;
    c.processor.safetyFrames = safetyFrames;
    c.processor.workerPollMicroseconds = 100; // keeps the per-block waits short
    return c;
}
} // namespace

// ---------------------------------------------------------------------------
TEST_CASE ("NeuralSlot: with no model the latency is the reference value per profile and the output bit-identical to an emptied slot's")
{
    const int reference[3] { kQualityLatency, kBalancedLatency, kLowLatency };
    for (int profile = 0; profile < 3; ++profile)
    {
        ParameterStore store;
        store.set (LatencyProfile, static_cast<float> (profile));
        store.set (BoostIntensity, 0.6f); // most modules engaged

        ProcessingChain untouched (store);   // setNeuralModel() never called
        ProcessingChain emptied (store);     // a model installed, then cleared before prepare()
        ProcessingChain cleared (store);     // a model active, then cleared and re-prepared
        emptied.setNeuralModel (std::make_unique<IdentityRunner> (256), slotConfig());
        emptied.clearNeuralModel();
        cleared.setNeuralModel (std::make_unique<IdentityRunner> (128), slotConfig (2)); // L = 384 <= 480: eligible everywhere
        cleared.prepare ({ kFs, 256, 2 });
        CHECK (cleared.getNeuralStatus().state == NeuralSlotState::Active);
        CHECK (cleared.getLatencySamples() == reference[profile] + 384);
        cleared.clearNeuralModel();
        CHECK (cleared.needsReprepare());

        for (ProcessingChain* c : { &untouched, &emptied, &cleared })
        {
            c->prepare ({ kFs, 256, 2 });
            CHECK (c->getLatencySamples() == reference[profile]);
            CHECK (c->getNeuralStatus().state == NeuralSlotState::Empty);
            CHECK (! c->getNeuralStatus().changePending);
            CHECK (! c->needsReprepare());
            CHECK (c->getNeuralProcessor() == nullptr);
        }

        const auto input = makeProgramme (48000, 0.5f);
        Planar a = input, b = input, c = input;
        render (untouched, a, 256);
        render (emptied, b, 256);
        render (cleared, c, 256);
        CHECK (bitIdentical (a, b));
        CHECK (bitIdentical (a, c));
        const auto counters = untouched.getNeuralCounters();
        CHECK (counters.framesProcessed == 0 && counters.deadlineMisses == 0 && counters.modelFailures == 0);
    }

    // The existing reference render (test_engine.cpp): every module bypassed,
    // the chain is its latency, as a pure delay.
    ParameterStore store;
    bypassAllModules (store);
    ProcessingChain chain (store);
    chain.prepare ({ kFs, 256, 2 });
    REQUIRE (chain.getLatencySamples() == kBalancedLatency);
    const auto input = makeProgramme (48000, 0.3f);
    Planar out = input;
    render (chain, out, 256);
    CHECK_LE (maxDelayedError (out, input, kBalancedLatency, 0), 1e-6);
}

TEST_CASE ("NeuralSlot: an identity model in Quality adds exactly its latency L and delays the chain's output by L")
{
    // Static settings (no macros) with every shift-invariant module engaged,
    // the maximizer driven into limiting. Everything downstream of the slot
    // then sees the same signal L samples later (L is a whole number of
    // blocks, so the block-rate loops see it at the same block phase); the
    // input stage and the gate, upstream, see it unchanged. The programme
    // starts after 0.2 s of silence so the modules' parameter glides that
    // begin at prepare() are over before any signal arrives in either chain.
    // The compressor and the bass engine are left out of the strict check:
    // their detectors keep converging on silence for longer (a residual that
    // shrinks with the pre-roll: ~6e-3 of full scale after 0.2 s), which the
    // looser whole-chain check with the Boost macro covers.
    const auto configure = [] (ParameterStore& store, bool macros) {
        store.set (LatencyProfile, static_cast<float> (LatencyProfileValue::Quality));
        if (macros)
        {
            store.set (BoostIntensity, 0.6f);
            return;
        }
        store.set (EqOn, 1.0f);
        store.set (eq (0, EqFieldOn), 1.0f);
        store.set (eq (0, EqFieldGain), 4.0f);
        store.set (DynEqOn, 1.0f);
        store.set (ClarityOn, 1.0f);
        store.set (ClarityPresence, 0.5f);
        store.set (SaturationOn, 1.0f);
        store.set (SatDriveDb, 6.0f);
        store.set (SpatialOn, 1.0f);
        store.set (SpatialWidth, 1.4f);
        store.set (MaximizerOn, 1.0f);
        store.set (MaxDriveDb, 6.0f);
    };

    const int L = 512;
    const int silence = 9600;
    Planar input (2, silence + 48000 * 2);
    {
        const auto prog = makeProgramme (48000 * 2, 0.5f);
        for (size_t c = 0; c < 2; ++c)
            std::copy (prog.ch[c].begin(), prog.ch[c].end(), input.ch[c].begin() + silence);
    }

    for (bool macros : { false, true })
    {
        ParameterStore store;
        configure (store, macros);
        ProcessingChain reference (store), withModel (store);
        reference.prepare ({ kFs, 256, 2 });
        withModel.setNeuralModel (std::make_unique<IdentityRunner> (256), slotConfig()); // L = 256 * (1 + 1)
        CHECK (withModel.needsReprepare());
        CHECK (withModel.getNeuralStatus().changePending);
        withModel.prepare ({ kFs, 256, 2 });
        CHECK (! withModel.needsReprepare());

        const auto status = withModel.getNeuralStatus();
        CHECK (status.state == NeuralSlotState::Active);
        CHECK (status.modelLatencySamples == L);
        CHECK (! status.changePending);
        REQUIRE (withModel.getNeuralProcessor() != nullptr);
        CHECK (withModel.getNeuralProcessor()->isModelActive());
        CHECK (reference.getLatencySamples() == kQualityLatency);
        CHECK (withModel.getLatencySamples() == kQualityLatency + L);
        CHECK_NEAR (withModel.meters().latencyMs.load(), 1000.0 * (kQualityLatency + L) / kFs, 1e-3);

        Planar a = input, b = input;
        render (reference, a, 256);
        render (withModel, b, 256, true); // every control frame on time (no timer-dependent misses)
        CHECK_LE (maxDelayedError (b, a, L, 0), macros ? 1e-2 : 1e-6);
        CHECK (rms (b.ch[0].data() + silence + L, 48000) > 0.05);
        for (size_t c = 0; c < 2; ++c)
            CHECK_LE (peakAbs (b.ch[c].data(), L), 1e-9); // silent (anti-denormal offsets aside) until the signal is L late
        CHECK (withModel.getNeuralCounters().framesProcessed > 0);
        CHECK (withModel.getNeuralCounters().deadlineMisses == 0);
        CHECK (withModel.getNeuralCounters().modelFailures == 0);
    }
}

TEST_CASE ("NeuralSlot: a 2-frame model is ineligible in Low Latency: slot bypassed, latency unchanged, status says why")
{
    ParameterStore store;
    store.set (LatencyProfile, static_cast<float> (LatencyProfileValue::LowLatency));
    ProcessingChain reference (store), chain (store);
    reference.prepare ({ kFs, 256, 2 });
    // 480-sample frames + 1 safety frame: L = 960 = two 10 ms reference frames.
    chain.setNeuralModel (std::make_unique<IdentityRunner> (480), slotConfig());
    chain.prepare ({ kFs, 256, 2 });
    CHECK (! isEligible (LatencyProfileValue::LowLatency, 960, kFs));

    auto status = chain.getNeuralStatus();
    CHECK (status.state == NeuralSlotState::Ineligible);
    CHECK (status.modelLatencySamples == 960);
    CHECK (std::strstr (neuralSlotReason (status.state), "latency profile") != nullptr);
    CHECK (chain.getNeuralProcessor() == nullptr);
    CHECK (chain.getLatencySamples() == kLowLatency);
    CHECK (! chain.needsReprepare());

    const auto input = makeProgramme (24000, 0.5f);
    Planar a = input, b = input;
    render (reference, a, 256);
    render (chain, b, 256);
    CHECK (bitIdentical (a, b));
    CHECK (chain.getNeuralCounters().framesProcessed == 0);

    // The model stays installed: Balanced (budget 2 frames) runs it, and back
    // in Low Latency it is out again.
    store.set (LatencyProfile, static_cast<float> (LatencyProfileValue::Balanced));
    CHECK (chain.needsReprepare());
    chain.prepare ({ kFs, 256, 2 });
    CHECK (chain.getNeuralStatus().state == NeuralSlotState::Active);
    CHECK (chain.getLatencySamples() == kBalancedLatency + 960);
    store.set (LatencyProfile, static_cast<float> (LatencyProfileValue::LowLatency));
    chain.prepare ({ kFs, 256, 2 });
    CHECK (chain.getNeuralStatus().state == NeuralSlotState::Ineligible);
    CHECK (chain.getLatencySamples() == kLowLatency);

    // Offline (batch) rendering accepts any model, whatever the profile.
    NeuralSlotConfig offline = slotConfig();
    offline.context = ModelContext::Offline;
    chain.setNeuralModel (std::make_unique<IdentityRunner> (480), offline);
    chain.prepare ({ kFs, 256, 2 });
    CHECK (chain.getNeuralStatus().state == NeuralSlotState::Active);
    CHECK (chain.getLatencySamples() == kLowLatency + 960);
}

TEST_CASE ("NeuralSlot: a sample-rate mismatch, an invalid description or a failing prepare keep the slot out with the reason")
{
    struct Case
    {
        std::unique_ptr<ModelRunner> runner;
        NeuralSlotState expected;
        int modelLatency;
    };
    Case cases[3] {
        { std::make_unique<DescribedRunner> (description (256, 44100.0)), NeuralSlotState::SampleRateMismatch, 512 },
        { std::make_unique<DescribedRunner> (description (0)), NeuralSlotState::InvalidModel, 0 },
        { std::make_unique<DescribedRunner> (description (256), 0.0f, true), NeuralSlotState::PrepareFailed, 512 },
    };
    for (auto& k : cases)
    {
        ParameterStore store;
        store.set (LatencyProfile, static_cast<float> (LatencyProfileValue::Quality));
        ProcessingChain reference (store), chain (store);
        reference.prepare ({ kFs, 256, 2 });
        chain.setNeuralModel (std::move (k.runner), slotConfig());
        chain.prepare ({ kFs, 256, 2 });
        const auto status = chain.getNeuralStatus();
        CHECK (status.state == k.expected);
        CHECK (status.modelLatencySamples == k.modelLatency);
        CHECK (std::strstr (neuralSlotReason (status.state), "Bypassed") != nullptr);
        CHECK (chain.getNeuralProcessor() == nullptr);
        CHECK (chain.getLatencySamples() == kQualityLatency);

        const auto input = makeProgramme (12000, 0.5f);
        Planar a = input, b = input;
        render (reference, a, 256);
        render (chain, b, 256);
        CHECK (bitIdentical (a, b));
    }

    // The same model at its own rate runs.
    ParameterStore store;
    ProcessingChain chain (store);
    chain.setNeuralModel (std::make_unique<DescribedRunner> (description (256, 44100.0)), slotConfig());
    chain.prepare ({ 44100.0, 256, 2 });
    CHECK (chain.getNeuralStatus().state == NeuralSlotState::Active);
}

TEST_CASE ("NeuralSlot: a host buffer longer than the model's safety frames keeps it out in real time (BlockTooLarge)")
{
    // A result can only be picked up by a later process() call than the one
    // that submitted its frame, so with blocks longer than safetyFrames *
    // frameSize a fixed fraction of frames (53 % for 480-sample frames and
    // 1024-sample buffers) would miss even with an instant model, while the
    // status read Active. prepare() keeps such a model out instead.
    struct Case
    {
        LatencyProfileValue profile;
        int frameSize, safetyFrames, maxBlock;
        NeuralSlotState expected;
    };
    const Case cases[] {
        { LatencyProfileValue::Balanced, 480, 1, 1024, NeuralSlotState::BlockTooLarge },
        { LatencyProfileValue::Balanced, 480, 1, 481, NeuralSlotState::BlockTooLarge },
        { LatencyProfileValue::Balanced, 480, 1, 480, NeuralSlotState::Active },
        { LatencyProfileValue::LowLatency, 240, 1, 512, NeuralSlotState::BlockTooLarge },
        { LatencyProfileValue::LowLatency, 240, 1, 256, NeuralSlotState::BlockTooLarge },
        { LatencyProfileValue::LowLatency, 240, 1, 240, NeuralSlotState::Active },
        { LatencyProfileValue::Quality, 240, 3, 720, NeuralSlotState::Active },
        { LatencyProfileValue::Quality, 240, 0, 64, NeuralSlotState::BlockTooLarge }, // no safety frame: every frame would miss
    };
    const int reference[3] { kQualityLatency, kBalancedLatency, kLowLatency };
    for (const auto& k : cases)
    {
        ParameterStore store;
        store.set (LatencyProfile, static_cast<float> (k.profile));
        ProcessingChain chain (store);
        chain.setNeuralModel (std::make_unique<IdentityRunner> (k.frameSize), slotConfig (k.safetyFrames));
        chain.prepare ({ kFs, k.maxBlock, 2 });
        const auto status = chain.getNeuralStatus();
        CHECK (status.state == k.expected);
        CHECK (status.modelLatencySamples == k.frameSize * (1 + k.safetyFrames));
        const int base = reference[static_cast<int> (k.profile)];
        if (k.expected == NeuralSlotState::BlockTooLarge)
        {
            CHECK (std::strstr (neuralSlotReason (status.state), "Bypassed") != nullptr);
            CHECK (std::strstr (neuralSlotReason (status.state), "buffer") != nullptr);
            CHECK (chain.getNeuralProcessor() == nullptr);
            CHECK (chain.getLatencySamples() == base);
        }
        else
        {
            REQUIRE (chain.getNeuralProcessor() != nullptr);
            CHECK (chain.getNeuralProcessor()->getMaxBlockSizeWithoutMisses() >= k.maxBlock);
            CHECK (chain.getLatencySamples() == base + status.modelLatencySamples);
        }
    }

    // The model stays installed: a smaller buffer brings it in, and an
    // Offline render (no real-time deadline) accepts any block length.
    ParameterStore store;
    store.set (LatencyProfile, static_cast<float> (LatencyProfileValue::Balanced));
    ProcessingChain chain (store);
    chain.setNeuralModel (std::make_unique<IdentityRunner> (480), slotConfig());
    chain.prepare ({ kFs, 1024, 2 });
    CHECK (chain.getNeuralStatus().state == NeuralSlotState::BlockTooLarge);
    chain.prepare ({ kFs, 256, 2 });
    CHECK (chain.getNeuralStatus().state == NeuralSlotState::Active);
    NeuralSlotConfig offline = slotConfig();
    offline.context = ModelContext::Offline;
    chain.setNeuralModel (std::make_unique<IdentityRunner> (480), offline);
    chain.prepare ({ kFs, 1024, 2 });
    CHECK (chain.getNeuralStatus().state == NeuralSlotState::Active);
    REQUIRE (chain.getNeuralProcessor() != nullptr);
    CHECK (chain.getNeuralProcessor()->getConfig().offline);
}

TEST_CASE ("NeuralSlot: an Offline render faster than real time applies every frame's model result, reproducibly")
{
    // A heavy model (100 ms frames, no safety frame: L = 4800) that only the
    // Offline context admits in Low Latency, rendered back to back in
    // 1024-sample blocks with no waiting at all: far faster than real time,
    // and with blocks longer than safetyFrames * frameSize. Every other module
    // is bypassed, so the chain is a pure delay and each frame's own gain
    // (0.5, 0.25, 0.8, ...) must be visible on exactly that frame's samples.
    const int frame = 4800, frames = 20, n = frame * frames, block = 1024;
    const auto input = makeProgramme (n, 0.3f);
    NeuralSlotConfig offline = slotConfig (0);
    offline.context = ModelContext::Offline;
    offline.processor.controlRampMs = 1.0f; // 48 samples

    Planar outputs[2] { input, input };
    for (Planar& out : outputs)
    {
        ParameterStore store;
        store.set (LatencyProfile, static_cast<float> (LatencyProfileValue::LowLatency));
        bypassAllModules (store);
        ProcessingChain realtime (store);
        realtime.setNeuralModel (std::make_unique<PerFrameGainRunner> (frame), slotConfig (0));
        realtime.prepare ({ kFs, block, 2 });
        CHECK (realtime.getNeuralStatus().state == NeuralSlotState::Ineligible);

        ProcessingChain chain (store);
        auto runner = std::make_unique<PerFrameGainRunner> (frame);
        const PerFrameGainRunner* model = runner.get();
        chain.setNeuralModel (std::move (runner), offline);
        chain.prepare ({ kFs, block, 2 });
        REQUIRE (chain.getNeuralStatus().state == NeuralSlotState::Active);
        const int total = chain.getLatencySamples();
        CHECK (total == kLowLatency + frame);

        render (chain, out, block); // no waitForModel
        CHECK (model->runThread == std::this_thread::get_id()); // the model ran inside process()
        const auto counters = chain.getNeuralCounters();
        CHECK (counters.deadlineMisses == 0);
        CHECK (counters.modelFailures == 0);
        CHECK (counters.framesProcessed == static_cast<uint64_t> (frames));
        CHECK (chain.getNeuralProcessor()->getPendingFrames() == 0);

        // Output frame k (input frame k, total samples later) carries gain k.
        double worst = 0.0;
        for (int k = 0; k * frame + total < n; ++k)
        {
            const float g = PerFrameGainRunner::kGains[k % 3];
            for (int j = k * frame + 48; j < (k + 1) * frame && j + total < n; ++j)
                for (size_t c = 0; c < 2; ++c)
                    worst = std::max (worst, static_cast<double> (std::abs (out.ch[c][static_cast<size_t> (j + total)] - g * input.ch[c][static_cast<size_t> (j)])));
        }
        CHECK_LE (worst, 1e-6);
    }
    CHECK (bitIdentical (outputs[0], outputs[1])); // the same bytes on every run
}

TEST_CASE ("NeuralSlot: a constant -6 dB (or +12 dB) model before the limiter still holds the true-peak ceiling on hot material")
{
    // Two set-ups: the full Music boost (every macro at 1), and the maximizer
    // alone, where the model's effect on the limited output is measurable.
    for (bool fullBoost : { true, false })
    {
        double referenceRms = 0.0;
        for (float gainDb : { 0.0f, -6.0f, 12.0f }) // 0 dB: the chain without a model
        {
            ParameterStore store;
            store.set (LatencyProfile, static_cast<float> (LatencyProfileValue::Quality));
            store.set (MaxCeilingDb, -1.0f);
            if (fullBoost)
            {
                store.set (Mode, static_cast<float> (ModeValue::Music));
                store.set (BoostIntensity, 1.0f);
                for (int m = Macro1; m <= Macro5; ++m)
                    store.set (m, 1.0f);
            }
            else
            {
                bypassAllModules (store);
                store.set (MaximizerOn, 1.0f);
                store.set (MaxDriveDb, 3.0f);
            }

            ProcessingChain chain (store);
            if (gainDb != 0.0f)
                chain.setNeuralModel (std::make_unique<ConstantGainRunner> (480, gainDb), slotConfig());
            chain.prepare ({ kFs, 256, 2 });
            CHECK (chain.getNeuralStatus().state == (gainDb != 0.0f ? NeuralSlotState::Active : NeuralSlotState::Empty));

            auto out = makeProgramme (48000 * 2, 0.9f);
            render (chain, out, 256, true); // every control frame on time

            TruePeakMeter tp;
            tp.prepare (2);
            tp.process (out.block());
            CHECK_LE (tp.getMaxDbAllChannels(), -1.0 + 0.15);
            for (size_t c = 0; c < 2; ++c)
                CHECK_LE (peakAbs (out.ch[c].data(), out.numSamples()), dbToGain (-1.0f) + 1e-6);
            CHECK (chain.meters().safetyClipCount.load() == 0);
            for (auto& c : out.ch)
                for (float v : c)
                    REQUIRE (std::isfinite (v));

            const double outRms = rms (out.ch[0].data() + 48000, 48000);
            if (gainDb == 0.0f)
            {
                referenceRms = outRms;
                continue;
            }
            const auto counters = chain.getNeuralCounters();
            CHECK (counters.deadlineMisses == 0);
            CHECK (counters.modelFailures == 0);
            CHECK (counters.framesProcessed > 150); // ~200 frames of 10 ms

            // The model did act (maximizer alone: -6 dB before a limiter that
            // was taking ~2-4 dB off leaves the output clearly quieter; +12 dB
            // is squashed to the same ceiling, louder: +0.7 dB RMS since
            // docs/11 E05 stage 1, whose crest-gated clipper leaves this
            // programme's steady parts alone and whose limiter holds its gain
            // over the bass line's periods; more than 1 dB before).
            if (! fullBoost)
            {
                if (gainDb < 0.0f)
                    CHECK (gainToDb (static_cast<float> (outRms / referenceRms)) < -2.0f);
                else
                    CHECK_GE (gainToDb (static_cast<float> (outRms / referenceRms)), 0.4f);
            }
        }
    }
}

TEST_CASE ("NeuralSlot: bypassing an active model fades to the latency-compensated dry path without changing the latency")
{
    // Every other module bypassed: the chain is a pure delay, so the model's
    // -6 dB is directly visible, and so is its bypass.
    ParameterStore store;
    store.set (LatencyProfile, static_cast<float> (LatencyProfileValue::Quality));
    bypassAllModules (store);
    ProcessingChain chain (store);
    chain.setNeuralModel (std::make_unique<ConstantGainRunner> (256, -6.0f), slotConfig());
    chain.prepare ({ kFs, 256, 2 });
    REQUIRE (chain.getNeuralStatus().state == NeuralSlotState::Active);
    const int total = chain.getLatencySamples();
    CHECK (total == kQualityLatency + 512);

    const int n = 256 * 188;
    const auto input = makeProgramme (n, 0.3f);
    Planar out = input;
    const float g = dbToGain (-6.0f);
    {
        ScopedNoDenormals noDenormals;
        const AsyncModelProcessor* model = chain.getNeuralProcessor();
        for (int pos = 0; pos < n; pos += 256)
        {
            if (pos == n / 2)
                chain.setNeuralBypass (true);
            chain.process (out.block (pos, 256));
            REQUIRE (waitUntil ([model] { return model->getPendingFrames() == 0; }));
        }
    }
    CHECK (chain.isNeuralBypassed());
    CHECK (chain.getLatencySamples() == total);

    // Before the bypass: input delayed by the total latency, times -6 dB
    // (after the first control ramp, 5 ms into the audio).
    double errModel = 0.0, errDry = 0.0;
    for (size_t c = 0; c < 2; ++c)
    {
        for (int i = total + 480; i < n / 2; ++i)
            errModel = std::max (errModel, static_cast<double> (std::abs (out.ch[c][static_cast<size_t> (i)] - g * input.ch[c][static_cast<size_t> (i - total)])));
        for (int i = n / 2 + total + 960; i < n; ++i) // after the 20 ms bypass fade has reached the output
            errDry = std::max (errDry, static_cast<double> (std::abs (out.ch[c][static_cast<size_t> (i)] - input.ch[c][static_cast<size_t> (i - total)])));
    }
    CHECK_LE (errModel, 1e-6);
    CHECK_LE (errDry, 1e-6);
}

TEST_CASE ("NeuralSlot: process() with an active model is allocation-free (bypass toggles, NaN reset and mode switch included)")
{
    for (int profile = 0; profile < 3; ++profile)
    {
        ParameterStore store;
        store.set (LatencyProfile, static_cast<float> (profile));
        store.set (BoostIntensity, 0.7f);
        ProcessingChain chain (store);
        chain.setNeuralModel (std::make_unique<ConstantGainRunner> (128, -3.0f), slotConfig (2)); // L = 384: eligible everywhere
        chain.prepare ({ kFs, 256, 2 });
        REQUIRE (chain.getNeuralStatus().state == NeuralSlotState::Active);
        auto buf = makeProgramme (256 * 40, 0.5f);
        AllocationGuard guard;
        for (int b = 0; b < 40; ++b)
        {
            if (b == 10)
                chain.setNeuralBypass (true);
            if (b == 20)
            {
                chain.setNeuralBypass (false); // re-activation: reset + pre-roll
                store.set (Mode, 1.0f);
            }
            if (b == 30)
                buf.ch[0][static_cast<size_t> (b * 256)] = std::numeric_limits<float>::quiet_NaN(); // chain.reset()
            chain.process (buf.block (b * 256, 256));
        }
        CHECK (guard.allocations() == 0);
    }
}

TEST_CASE ("NeuralSlot: clearNeuralModel restores the original latency and output after the next prepare()")
{
    // The reference goes through the same history without a model (prepare,
    // render, prepare): modules keep their last parameter values across a
    // re-prepare, so a re-prepared chain is compared with a re-prepared one.
    ParameterStore store;
    store.set (LatencyProfile, static_cast<float> (LatencyProfileValue::Balanced));
    store.set (BoostIntensity, 0.5f);
    ProcessingChain reference (store), chain (store);
    reference.prepare ({ kFs, 256, 2 });
    chain.prepare ({ kFs, 256, 2 });
    CHECK (chain.getLatencySamples() == kBalancedLatency);
    {
        auto buf = makeProgramme (48000 / 2, 0.5f);
        render (reference, buf, 256);
        reference.prepare ({ kFs, 256, 2 });
    }

    chain.setNeuralModel (std::make_unique<FailingRunner> (480), slotConfig()); // L = 960
    CHECK (chain.getLatencySamples() == kBalancedLatency); // pending until prepare()
    chain.prepare ({ kFs, 256, 2 });
    CHECK (chain.getLatencySamples() == kBalancedLatency + 960);
    {
        auto buf = makeProgramme (48000 / 2, 0.5f);
        render (chain, buf, 256, true);
        const auto counters = chain.getNeuralCounters(); // a failing model: telemetry for the UI
        CHECK (counters.modelFailures > 10);
        CHECK (counters.deadlineMisses == 0);
        CHECK (counters.framesProcessed >= counters.modelFailures);
    }

    chain.clearNeuralModel();
    CHECK (chain.needsReprepare());
    CHECK (chain.getNeuralStatus().changePending);
    CHECK (chain.getNeuralStatus().state == NeuralSlotState::Active); // until prepare()
    CHECK (chain.getLatencySamples() == kBalancedLatency + 960);
    chain.prepare ({ kFs, 256, 2 });
    CHECK (chain.getLatencySamples() == kBalancedLatency);
    CHECK (chain.getNeuralStatus().state == NeuralSlotState::Empty);
    CHECK (chain.getNeuralStatus().modelLatencySamples == 0);
    CHECK (chain.getNeuralProcessor() == nullptr);
    CHECK (chain.getNeuralCounters().modelFailures == 0); // restart at prepare()
    CHECK (! chain.needsReprepare());

    const auto input = makeProgramme (24000, 0.5f);
    Planar a = input, b = input;
    render (reference, a, 256);
    render (chain, b, 256);
    CHECK (bitIdentical (a, b));
}
