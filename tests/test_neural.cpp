// Tests for the neural extension point (flub/neural): AsyncModelProcessor's
// fixed latency, the model's control applied to exactly the frame it was
// computed from, deadline misses and model failures with hold-then-fade
// fallback, reset, real-time safety, worker lifecycle and the latency-profile
// eligibility rule. No inference runtime: the models are the reference
// runners plus scripted stand-ins below.
//
// Determinism. Every test drives the real asynchronous path (the worker
// thread). With host blocks no longer than safetyFrames * frameSize, the
// result for a frame is needed in a later process() call than the one that
// submitted it, so a test that waits for the worker between blocks
// (waitForWorker: getPendingFrames() == 0, bounded only as a hang guard)
// gets every result on time. Slowness is scripted with a test-owned gate the
// model blocks on, never with a sleep of some length. Offline mode runs the
// model inside process() and needs no waiting at all.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/common/Math.h"
#include "flub/neural/AsyncModelProcessor.h"
#include "flub/neural/Eligibility.h"
#include "flub/neural/ReferenceRunners.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

using namespace flub;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;
constexpr int kFrame = 64;

/** Spins (yielding) until pred() holds. The bound is a hang guard for a
    broken worker, never a timing assumption: a healthy worker answers within
    one poll interval. Yielding rather than sleeping keeps the test's own wait
    out of the timer granularity (up to 15.6 ms per sleep on Windows). */
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

bool waitForWorker (const AsyncModelProcessor& p)
{
    return waitUntil ([&p] { return p.getPendingFrames() == 0; });
}

/** Knobs a test turns while the worker runs the model. */
struct Script
{
    std::atomic<bool> gateClosed { false };  // run() blocks while set
    std::atomic<int> blockedRuns { 0 };      // run() calls that found the gate closed
    std::atomic<bool> fail { false };        // run() returns false
    std::atomic<bool> produceNan { false };  // run() returns true with a NaN control
    std::atomic<float> gain { 0.5f };
    std::atomic<int> resets { 0 };
};

/** Opens the gate on scope exit (declared after the processor, so a failed
    REQUIRE cannot leave the destructor joining a worker stuck in run()). */
struct GateOpener
{
    Script& script;
    ~GateOpener() { script.gateClosed.store (false, std::memory_order_release); }
};

class ScriptedRunner : public ModelRunner
{
public:
    ScriptedRunner (Script& s, ModelDescription d) : script (s), desc (d) {}

    ModelDescription describe() const override { return desc; }
    void reset() override { script.resets.fetch_add (1); }

    bool run (const float*, float* outControls) override
    {
        if (script.gateClosed.load (std::memory_order_acquire))
        {
            script.blockedRuns.fetch_add (1);
            while (script.gateClosed.load (std::memory_order_acquire))
                std::this_thread::yield();
        }
        if (script.fail.load())
            return false;
        const float g = script.produceNan.load() ? std::numeric_limits<float>::quiet_NaN() : script.gain.load();
        for (int k = 0; k < desc.numControls; ++k)
            outControls[k] = g * static_cast<float> (k + 1); // control k = gain * (k + 1)
        return true;
    }

private:
    Script& script;
    ModelDescription desc;
};

/** Copies every input frame it sees (storage reserved up front). */
class RecordingRunner : public ModelRunner
{
public:
    RecordingRunner (ModelDescription d, int maxFrames, std::vector<float>& store)
        : desc (d), frames (store)
    {
        frames.assign (static_cast<size_t> (maxFrames) * static_cast<size_t> (d.frameSize * d.numInputChannels), 0.0f);
    }

    ModelDescription describe() const override { return desc; }

    bool run (const float* inFrame, float* outControls) override
    {
        const auto floats = static_cast<size_t> (desc.frameSize * desc.numInputChannels);
        if ((recorded + 1) * floats <= frames.size())
            std::copy (inFrame, inFrame + floats, frames.begin() + static_cast<std::ptrdiff_t> (recorded++ * floats));
        std::fill (outControls, outControls + desc.numControls, 1.0f);
        return true;
    }

private:
    ModelDescription desc;
    std::vector<float>& frames;
    size_t recorded = 0;
};

/** A level normaliser: control = 1 / mean |x| of the frame. */
class InverseLevelRunner : public ModelRunner
{
public:
    explicit InverseLevelRunner (int frameSize) : frame (frameSize) {}

    ModelDescription describe() const override
    {
        ModelDescription d;
        d.frameSize = frame;
        return d;
    }

    bool run (const float* inFrame, float* outControls) override
    {
        double sum = 0.0;
        for (int i = 0; i < frame; ++i)
            sum += std::abs (inFrame[i]);
        outControls[0] = sum > 0.0 ? static_cast<float> (frame / sum) : 1.0f;
        return true;
    }

private:
    int frame;
};

ModelDescription description (int frameSize, int numControls = 1, ControlKind kind = ControlKind::BroadbandGain, int inputs = 1)
{
    ModelDescription d;
    d.frameSize = frameSize;
    d.numInputChannels = inputs;
    d.numControls = numControls;
    d.controlKind = kind;
    return d;
}

ProcessSpec spec (int channels = 2, int maxBlock = kFrame)
{
    ProcessSpec s;
    s.sampleRate = kFs;
    s.maxBlockSize = maxBlock;
    s.numChannels = channels;
    return s;
}

Planar noise (int channels, int n)
{
    Planar p (channels, n);
    for (int c = 0; c < channels; ++c)
    {
        const auto v = whiteNoise (n, 0.5f, 77u + static_cast<uint32_t> (c));
        std::copy (v.begin(), v.end(), p.ch[static_cast<size_t> (c)].begin());
    }
    return p;
}

Planar dc (int channels, int n, float value)
{
    Planar p (channels, n);
    for (auto& c : p.ch)
        std::fill (c.begin(), c.end(), value);
    return p;
}

/** Processes buf in place in blocks of the given sizes (cycled), waiting for
    the worker after each block when waitEachBlock is set. */
bool runBlocks (AsyncModelProcessor& p, Planar& buf, int start, int end, const std::vector<int>& sizes, bool waitEachBlock = true)
{
    size_t k = 0;
    for (int pos = start; pos < end;)
    {
        const int n = std::min (sizes[k++ % sizes.size()], end - pos);
        p.process (buf.block (pos, n));
        pos += n;
        if (waitEachBlock && ! waitForWorker (p))
            return false;
    }
    return true;
}

double maxStep (const std::vector<float>& x, int start, int end)
{
    double m = 0.0;
    for (int i = std::max (1, start); i < end; ++i)
        m = std::max (m, static_cast<double> (std::abs (x[static_cast<size_t> (i)] - x[static_cast<size_t> (i - 1)])));
    return m;
}

bool allNear (const std::vector<float>& x, int start, int end, float value, float tol = 1.0e-6f)
{
    for (int i = start; i < end; ++i)
        if (! (std::abs (x[static_cast<size_t> (i)] - value) <= tol))
            return false;
    return true;
}
} // namespace

TEST_CASE ("Neural: latencySamples is frameSize * (1 + safetyFrames) and the identity model delays the input by exactly that")
{
    for (int safety : { 0, 1, 2, 5 })
    {
        AsyncModelConfig cfg;
        cfg.safetyFrames = safety;
        AsyncModelProcessor p (std::make_unique<IdentityRunner> (kFrame), cfg);
        CHECK (p.latencySamples() == kFrame * (1 + safety));
        p.prepare (spec());
        CHECK (p.latencySamples() == kFrame * (1 + safety)); // constant across prepare()
    }

    AsyncModelProcessor p (std::make_unique<IdentityRunner> (kFrame)); // safetyFrames = 1
    const int latency = p.latencySamples();
    REQUIRE (latency == 2 * kFrame);
    p.prepare (spec());
    REQUIRE (p.isModelActive());

    const int n = kFrame * 40;
    const Planar in = noise (2, n);
    Planar buf = in;
    // Odd block sizes, all <= safetyFrames * frameSize.
    REQUIRE (runBlocks (p, buf, 0, n, { 64, 17, 1, 46, 33, 64, 5 }));

    bool exact = true;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i)
        {
            const float expected = i < latency ? 0.0f : in.ch[static_cast<size_t> (c)][static_cast<size_t> (i - latency)];
            exact = exact && buf.ch[static_cast<size_t> (c)][static_cast<size_t> (i)] == expected;
        }
    CHECK (exact);
    CHECK (p.getFramesProcessed() == static_cast<uint64_t> (n / kFrame)); // the worker ran every frame
    CHECK (p.getDeadlineMisses() == 0);
    CHECK (p.getModelFailures() == 0);
}

TEST_CASE ("Neural: the model receives consecutive input frames as a mono downmix or as planar channels")
{
    const int frames = 12;
    const int n = kFrame * frames;
    const Planar in = noise (2, n);

    for (int inputs : { 1, 2, 3 })
    {
        std::vector<float> seen;
        AsyncModelProcessor p (std::make_unique<RecordingRunner> (description (kFrame, 1, ControlKind::BroadbandGain, inputs), frames, seen));
        p.prepare (spec());
        Planar buf = in;
        REQUIRE (runBlocks (p, buf, 0, n, { 29, 64, 3 }));
        REQUIRE (p.getFramesProcessed() == static_cast<uint64_t> (frames));

        bool ok = true;
        for (int k = 0; k < frames; ++k)
            for (int m = 0; m < inputs; ++m)
                for (int i = 0; i < kFrame; ++i)
                {
                    const auto s = static_cast<size_t> (k * kFrame + i);
                    const float l = in.ch[0][s], r = in.ch[1][s];
                    // Mono: mean of the channels; planar: model channel m reads channel min (m, 1).
                    const float expected = inputs == 1 ? (l + r) * 0.5f : (m == 0 ? l : r);
                    const float got = seen[static_cast<size_t> ((k * inputs + m) * kFrame + i)];
                    ok = ok && std::abs (got - expected) <= 1.0e-7f;
                }
        CHECK (ok);
    }
}

TEST_CASE ("Neural: the control computed from a frame is applied to exactly that frame's samples")
{
    // Each input frame is DC at its own level v_k; the model returns 1 / v_k.
    // Aligned, every frame leaves at unity once the ramp (24 samples, starting
    // on the frame's first sample) is done; one frame early or late it would
    // leave at v_k / v_(k +- 1).
    AsyncModelConfig cfg;
    cfg.controlRampMs = 0.5f;
    cfg.maxGain = 16.0f;
    AsyncModelProcessor p (std::make_unique<InverseLevelRunner> (kFrame), cfg);
    p.prepare (spec (1));
    const int latency = p.latencySamples();
    const int frames = 40;
    Planar buf (1, kFrame * frames);
    for (int k = 0; k < frames; ++k)
        std::fill_n (buf.ch[0].begin() + k * kFrame, kFrame, 0.1f + 0.08f * static_cast<float> ((k * 7) % 10));
    REQUIRE (runBlocks (p, buf, 0, kFrame * frames, { 64, 40, 24 }));

    double worst = 0.0;
    for (int k = 0; k + latency / kFrame < frames; ++k)
    {
        const int first = latency + k * kFrame;
        for (int i = first + 24; i < first + kFrame; ++i)
            worst = std::max (worst, std::abs (static_cast<double> (buf.ch[0][static_cast<size_t> (i)]) - 1.0));
    }
    CHECK_LE (worst, 1.0e-5);
    CHECK (p.getDeadlineMisses() == 0);
}

TEST_CASE ("Neural: offline mode runs the model inside process(), so a render faster than real time gets every frame's result")
{
    // The frame-alignment scenario above, rendered back to back with no
    // waiting and with blocks far longer than safetyFrames * frameSize: an
    // asynchronous processor would miss most of these frames (by a number
    // that depends on thread scheduling) and fall back to the dry signal.
    // Offline, each frame's result is there at its boundary, even with no
    // safety frame, and two renders give the same bytes.
    for (int safety : { 0, 1 })
    {
        AsyncModelConfig cfg;
        cfg.safetyFrames = safety;
        cfg.controlRampMs = 0.5f;
        cfg.maxGain = 16.0f;
        cfg.offline = true;
        const int frames = 60;
        Planar input (1, kFrame * frames);
        for (int k = 0; k < frames; ++k)
            std::fill_n (input.ch[0].begin() + k * kFrame, kFrame, 0.1f + 0.08f * static_cast<float> ((k * 7) % 10));

        Planar renders[2] { input, input };
        for (Planar& buf : renders)
        {
            AsyncModelProcessor p (std::make_unique<InverseLevelRunner> (kFrame), cfg);
            p.prepare (spec (1, 1000));
            CHECK (p.getMaxBlockSizeWithoutMisses() == std::numeric_limits<int>::max());
            const int latency = p.latencySamples();
            CHECK (latency == kFrame * (1 + safety));
            REQUIRE (runBlocks (p, buf, 0, kFrame * frames, { 1000, 37, 700 }, false));

            double worst = 0.0;
            for (int k = 0; k + latency / kFrame < frames; ++k)
            {
                const int first = latency + k * kFrame;
                for (int i = first + 24; i < first + kFrame; ++i)
                    worst = std::max (worst, std::abs (static_cast<double> (buf.ch[0][static_cast<size_t> (i)]) - 1.0));
            }
            CHECK_LE (worst, 1.0e-5);
            CHECK (p.getDeadlineMisses() == 0);
            CHECK (p.getFramesProcessed() == static_cast<uint64_t> (frames));
            CHECK (p.getPendingFrames() == 0);
        }
        CHECK (renders[0].ch[0] == renders[1].ch[0]);
    }

    // Asynchronously, a block can be at most safetyFrames * frameSize long for
    // every result to be on time (a result is picked up in a later block).
    for (int safety : { 0, 1, 3 })
    {
        AsyncModelConfig cfg;
        cfg.safetyFrames = safety;
        const AsyncModelProcessor p (std::make_unique<IdentityRunner> (kFrame), cfg);
        CHECK (p.getMaxBlockSizeWithoutMisses() == safety * kFrame);
    }
}

TEST_CASE ("Neural: a constant -6 dB model scales the delayed input by -6 dB once the control ramp has settled")
{
    AsyncModelConfig cfg;
    cfg.controlRampMs = 1.0f; // 48 samples
    AsyncModelProcessor p (std::make_unique<ConstantGainRunner> (kFrame, -6.0f), cfg);
    p.prepare (spec());
    const int latency = p.latencySamples();
    const int ramp = 48;

    const int n = kFrame * 30;
    const Planar in = noise (2, n);
    Planar buf = in;
    REQUIRE (runBlocks (p, buf, 0, n, { 64, 31, 7 }));

    const float g = dbToGain (-6.0f);
    double worst = 0.0;
    for (int c = 0; c < 2; ++c)
        for (int i = latency + ramp; i < n; ++i)
        {
            const auto s = static_cast<size_t> (i);
            worst = std::max (worst, static_cast<double> (std::abs (buf.ch[static_cast<size_t> (c)][s] - g * in.ch[static_cast<size_t> (c)][s - static_cast<size_t> (latency)])));
        }
    CHECK_LE (worst, 1.0e-6);
    const double ratioDb = toDb (rms (buf.ch[0].data() + latency + ramp, n - latency - ramp) / rms (in.ch[0].data() + ramp, n - latency - ramp));
    CHECK_NEAR (ratioDb, -6.0, 0.01);
    CHECK (p.getDeadlineMisses() == 0);
}

TEST_CASE ("Neural: channel-gain controls scale each output channel separately")
{
    Script script;
    script.gain.store (0.25f); // control k = 0.25 * (k + 1): 0.25, 0.5
    AsyncModelConfig cfg;
    cfg.controlRampMs = 1.0f;
    AsyncModelProcessor p (std::make_unique<ScriptedRunner> (script, description (kFrame, 2, ControlKind::ChannelGains)), cfg);
    p.prepare (spec (3)); // the third channel uses the last control
    const int n = kFrame * 12;
    Planar buf = dc (3, n, 1.0f);
    REQUIRE (runBlocks (p, buf, 0, n, { 64 }));
    const int settled = p.latencySamples() + 48;
    CHECK (allNear (buf.ch[0], settled, n, 0.25f));
    CHECK (allNear (buf.ch[1], settled, n, 0.5f));
    CHECK (allNear (buf.ch[2], settled, n, 0.5f));

    // BroadbandGain uses control 0 for every channel.
    Script broad;
    broad.gain.store (0.25f);
    AsyncModelProcessor q (std::make_unique<ScriptedRunner> (broad, description (kFrame, 2, ControlKind::BroadbandGain)), cfg);
    q.prepare (spec (2));
    Planar buf2 = dc (2, n, 1.0f);
    REQUIRE (runBlocks (q, buf2, 0, n, { 64 }));
    CHECK (allNear (buf2.ch[0], settled, n, 0.25f));
    CHECK (allNear (buf2.ch[1], settled, n, 0.25f));
}

TEST_CASE ("Neural: a blocked model counts each missed frame, holds the last good control, then crossfades to neutral after K misses")
{
    Script script; // gain 0.5
    AsyncModelConfig cfg;
    cfg.fallbackAfterFrames = 3;
    cfg.controlRampMs = 1.0f; // 48 samples
    AsyncModelProcessor p (std::make_unique<ScriptedRunner> (script, description (kFrame)), cfg);
    GateOpener opener { script };
    p.prepare (spec (1));
    REQUIRE (p.latencySamples() == 2 * kFrame);

    // Block b (64 samples) submits frame b and consumes the result for frame
    // b - 1, which drives the output of block b + 1 (input block b - 1).
    const int blocks = 40;
    Planar buf = dc (1, kFrame * blocks, 1.0f);
    const auto at = [] (int block) { return block * kFrame; };
    REQUIRE (runBlocks (p, buf, 0, at (10), { kFrame }));
    CHECK (allNear (buf.ch[0], at (3), at (10), 0.5f));
    CHECK (p.getDeadlineMisses() == 0);

    // The model stalls on frame 10.
    script.gateClosed.store (true);
    REQUIRE (runBlocks (p, buf, at (10), at (11), { kFrame }, false));
    REQUIRE (waitUntil ([&script] { return script.blockedRuns.load() == 1; }));
    const uint64_t processedBeforeStall = p.getFramesProcessed();
    CHECK (processedBeforeStall == 10);

    // Blocks 11 .. 18 consume frames 10 .. 17: eight misses, counted exactly.
    REQUIRE (runBlocks (p, buf, at (11), at (19), { kFrame }, false));
    CHECK (p.getDeadlineMisses() == 8);
    CHECK (p.getModelFailures() == 0);
    CHECK (p.getFramesProcessed() == processedBeforeStall); // nothing finished while stalled

    // Misses 1 and 2 (frames 10, 11 -> output blocks 12, 13) hold the last
    // good control; the 3rd (frame 12) starts the ramp to neutral at block 14.
    CHECK (allNear (buf.ch[0], at (11), at (14), 0.5f));
    CHECK (buf.ch[0][static_cast<size_t> (at (14))] > 0.5f);
    CHECK (buf.ch[0][static_cast<size_t> (at (14))] < 0.52f);
    CHECK (allNear (buf.ch[0], at (15), at (19), 1.0f));
    CHECK_LE (maxStep (buf.ch[0], at (11), at (19)), 0.5 / 48.0 + 1.0e-6); // no click

    // Release: the stale frames are skipped and the model recovers.
    script.gateClosed.store (false);
    REQUIRE (waitForWorker (p));
    REQUIRE (runBlocks (p, buf, at (19), at (21), { kFrame }));
    const uint64_t missesAfterRecovery = p.getDeadlineMisses();
    CHECK (missesAfterRecovery <= 10); // at most frames 18, 19 (queued behind the stall or dropped)
    REQUIRE (runBlocks (p, buf, at (21), at (blocks), { kFrame }));
    CHECK (p.getDeadlineMisses() == missesAfterRecovery);
    CHECK (allNear (buf.ch[0], at (25), at (blocks), 0.5f));
    CHECK_LE (maxStep (buf.ch[0], at (19), at (blocks)), 0.5 / 48.0 + 1.0e-6);
}

TEST_CASE ("Neural: a failing model counts failures, holds the last good control and falls back to neutral")
{
    Script script; // gain 0.5
    AsyncModelConfig cfg;
    cfg.fallbackAfterFrames = 3;
    cfg.controlRampMs = 1.0f;
    AsyncModelProcessor p (std::make_unique<ScriptedRunner> (script, description (kFrame)), cfg);
    p.prepare (spec (1));
    const auto at = [] (int block) { return block * kFrame; };
    Planar buf = dc (1, at (30), 1.0f);

    REQUIRE (runBlocks (p, buf, 0, at (10), { kFrame }));
    script.fail.store (true);                               // frames 10 .. 16 fail
    REQUIRE (runBlocks (p, buf, at (10), at (17), { kFrame })); // consumes frames 9 .. 15
    CHECK (p.getModelFailures() == 6);
    CHECK (p.getDeadlineMisses() == 0);
    CHECK (allNear (buf.ch[0], at (11), at (14), 0.5f));  // failures 1, 2: last good control
    CHECK (allNear (buf.ch[0], at (15), at (17), 1.0f));  // from the 3rd: neutral
    script.fail.store (false);
    REQUIRE (runBlocks (p, buf, at (17), at (30), { kFrame }));
    CHECK (p.getModelFailures() == 7);                      // frame 16 was run while failing
    CHECK (allNear (buf.ch[0], at (20), at (30), 0.5f));  // frame 17 is good again
    CHECK_LE (maxStep (buf.ch[0], at (10), at (30)), 0.5 / 48.0 + 1.0e-6);
    CHECK (p.getFramesProcessed() == 30);

    // The reference FailingRunner never gets past neutral: a pure delay.
    AsyncModelProcessor f (std::make_unique<FailingRunner> (kFrame));
    f.prepare (spec());
    const int n = kFrame * 20;
    const Planar in = noise (2, n);
    Planar out = in;
    REQUIRE (runBlocks (f, out, 0, n, { 64, 13 }));
    bool exact = true;
    for (int i = f.latencySamples(); i < n; ++i)
        exact = exact && out.ch[1][static_cast<size_t> (i)] == in.ch[1][static_cast<size_t> (i - f.latencySamples())];
    CHECK (exact);
    CHECK (f.getModelFailures() == 19); // every boundary: frames 0 .. 18
    CHECK (f.getDeadlineMisses() == 0);
}

TEST_CASE ("Neural: non-finite controls count as failures and controls are clamped to [0, maxGain]")
{
    Script script;
    AsyncModelConfig cfg;
    cfg.fallbackAfterFrames = 2;
    cfg.controlRampMs = 1.0f;
    cfg.maxGain = 2.0f;
    AsyncModelProcessor p (std::make_unique<ScriptedRunner> (script, description (kFrame)), cfg);
    p.prepare (spec (1));
    const auto at = [] (int block) { return block * kFrame; };
    Planar buf = dc (1, at (30), 1.0f);

    script.gain.store (8.0f); // clamped to 2
    REQUIRE (runBlocks (p, buf, 0, at (10), { kFrame }));
    CHECK (allNear (buf.ch[0], at (4), at (10), 2.0f));
    script.gain.store (-1.0f); // clamped to 0
    REQUIRE (runBlocks (p, buf, at (10), at (15), { kFrame }));
    CHECK (allNear (buf.ch[0], at (13), at (15), 0.0f));
    script.produceNan.store (true);
    REQUIRE (runBlocks (p, buf, at (15), at (30), { kFrame }));
    CHECK (p.getModelFailures() == 14); // frames 15 .. 28
    bool finite = true;
    for (float v : buf.ch[0])
        finite = finite && std::isfinite (v);
    CHECK (finite);
    CHECK (allNear (buf.ch[0], at (20), at (30), 1.0f)); // neutral after two bad frames
}

TEST_CASE ("Neural: reset() clears the delay and the controls without stopping the worker")
{
    Script script; // gain 0.5
    AsyncModelConfig cfg;
    cfg.controlRampMs = 1.0f;
    AsyncModelProcessor p (std::make_unique<ScriptedRunner> (script, description (kFrame)), cfg);
    p.prepare (spec());
    const int latency = p.latencySamples();

    const int n = kFrame * 20;
    Planar buf = noise (2, n);
    REQUIRE (runBlocks (p, buf, 0, n, { 64 }));
    CHECK (script.resets.load() == 1); // before the first frame after prepare()

    // Reset with a frame still queued (no wait): it is stale and skipped.
    const Planar in = noise (2, n);
    Planar after = in;
    p.process (after.block (0, kFrame));
    p.reset();
    after = in;
    const uint64_t processed = p.getFramesProcessed();
    REQUIRE (runBlocks (p, after, 0, n, { 64, 21 }));
    CHECK (p.getFramesProcessed() >= processed + static_cast<uint64_t> (n / kFrame)); // the worker kept running
    CHECK (p.getDeadlineMisses() == 0);
    CHECK (script.resets.load() == 2); // the model saw the discontinuity

    bool silentThenDelayed = true;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i)
        {
            const auto s = static_cast<size_t> (i);
            const float got = after.ch[static_cast<size_t> (c)][s];
            if (i < latency)
                silentThenDelayed = silentThenDelayed && got == 0.0f; // nothing from before the reset
            else if (i >= latency + 48)
                silentThenDelayed = silentThenDelayed && std::abs (got - 0.5f * in.ch[static_cast<size_t> (c)][s - static_cast<size_t> (latency)]) <= 1.0e-6f;
        }
    CHECK (silentThenDelayed);
}

TEST_CASE ("Neural: process() and reset() are allocation-free")
{
    Script script;
    AsyncModelConfig cfg;
    cfg.fallbackAfterFrames = 2;
    AsyncModelProcessor p (std::make_unique<ScriptedRunner> (script, description (kFrame, 2, ControlKind::ChannelGains)), cfg);
    GateOpener opener { script };
    p.prepare (spec (2, 256));
    const int n = kFrame * 64;
    Planar buf = noise (2, n);

    int64_t allocations = 0;
    const auto run = [&] (int start, int end, bool wait)
    {
        for (int pos = start; pos < end; pos += 37)
        {
            AllocationGuard guard;
            p.process (buf.block (pos, std::min (37, end - pos)));
            allocations += guard.allocations();
            if (wait)
                waitForWorker (p);
        }
    };
    run (0, n / 4, true);                // results on time
    script.gateClosed.store (true);
    run (n / 4, n / 2, false);           // misses, queue overflow, fade to neutral
    script.gateClosed.store (false);
    waitForWorker (p);
    {
        AllocationGuard guard;
        p.reset();
        allocations += guard.allocations();
    }
    run (n / 2, n, true);
    CHECK (allocations == 0);
    CHECK (p.getDeadlineMisses() > 0);
}

TEST_CASE ("Neural: an invalid runner is inert and a sample-rate mismatch keeps the latency without running the model")
{
    const int n = kFrame * 10;
    const Planar in = noise (2, n);

    AsyncModelProcessor none (nullptr);
    none.prepare (spec());
    Planar a = in;
    none.process (a.block (0, n));
    CHECK (none.latencySamples() == 0);
    CHECK (! none.isModelActive());
    CHECK (a.ch[0] == in.ch[0]);

    Script script;
    AsyncModelProcessor bad (std::make_unique<ScriptedRunner> (script, description (0)));
    bad.prepare (spec());
    Planar b = in;
    bad.process (b.block (0, n));
    CHECK (bad.latencySamples() == 0);
    CHECK (b.ch[1] == in.ch[1]);

    auto d = description (kFrame);
    d.sampleRate = 44100.0;
    AsyncModelProcessor other (std::make_unique<ScriptedRunner> (script, d));
    other.prepare (spec()); // 48 kHz
    CHECK (! other.isModelActive());
    const int latency = other.latencySamples();
    CHECK (latency == 2 * kFrame);
    Planar c = in;
    REQUIRE (runBlocks (other, c, 0, n, { 64 }));
    bool delayed = true;
    for (int i = latency; i < n; ++i)
        delayed = delayed && c.ch[0][static_cast<size_t> (i)] == in.ch[0][static_cast<size_t> (i - latency)];
    CHECK (delayed);
    CHECK (other.getFramesProcessed() == 0);
    CHECK (other.getDeadlineMisses() == 0);
}

TEST_CASE ("Neural: eligibility follows the latency-profile rule")
{
    using LP = param::LatencyProfileValue;
    // Low Latency: at most one 10 ms reference frame.
    CHECK (isEligible (LP::LowLatency, 0, 48000.0));
    CHECK (isEligible (LP::LowLatency, 480, 48000.0));
    CHECK (! isEligible (LP::LowLatency, 481, 48000.0));
    CHECK (isEligible (LP::LowLatency, 441, 44100.0));
    CHECK (! isEligible (LP::LowLatency, 442, 44100.0));
    CHECK (isEligible (LP::LowLatency, 960, 96000.0));
    // Balanced: two frames (one model frame plus one frame of safety).
    CHECK (isEligible (LP::Balanced, 960, 48000.0));
    CHECK (! isEligible (LP::Balanced, 961, 48000.0));
    // Heavy models: Quality and offline only.
    CHECK (isEligible (LP::Quality, 48000, 48000.0));
    CHECK (isEligible (LP::LowLatency, 48000, 48000.0, ModelContext::Offline));
    CHECK (isEligible (LP::Balanced, 48000, 48000.0, ModelContext::Offline));
    // Invalid input is never eligible.
    CHECK (! isEligible (LP::Quality, -1, 48000.0));
    CHECK (! isEligible (LP::Quality, 0, 0.0));
    CHECK (! isEligible (LP::Quality, 0, std::numeric_limits<double>::quiet_NaN()));
    CHECK (! isEligible (LP::LowLatency, 0, -48000.0, ModelContext::Offline));
    CHECK (neuralLatencyBudgetFrames (LP::LowLatency) == 1);
    CHECK (neuralLatencyBudgetFrames (LP::Balanced) == 2);

    // Applied to a processor's reported latency (48 kHz): a 5 ms frame with
    // one safety frame fits Low Latency, a 10 ms frame only Balanced.
    const AsyncModelProcessor small (std::make_unique<IdentityRunner> (240));
    const AsyncModelProcessor standard (std::make_unique<IdentityRunner> (480));
    CHECK (isEligible (LP::LowLatency, small.latencySamples(), 48000.0));
    CHECK (! isEligible (LP::LowLatency, standard.latencySamples(), 48000.0));
    CHECK (isEligible (LP::Balanced, standard.latencySamples(), 48000.0));
}

TEST_CASE ("Neural: repeated create, prepare, process and destroy joins the worker cleanly")
{
    const int n = kFrame * 4;
    const Planar in = noise (2, n);
    uint64_t waitedProcessed = 0;
    int waitedIterations = 0;
    for (int iteration = 0; iteration < 50; ++iteration)
    {
        AsyncModelProcessor p (std::make_unique<IdentityRunner> (kFrame));
        if (iteration % 5 == 4)
            continue; // destroyed without ever being prepared
        p.prepare (spec());
        if (iteration % 3 == 0)
            p.prepare (spec (2, 128)); // re-prepare restarts the worker
        Planar buf = in;
        const bool waitEachBlock = iteration % 5 == 0;
        REQUIRE (runBlocks (p, buf, 0, n, { 64 }, waitEachBlock));
        REQUIRE (waitForWorker (p));
        CHECK (p.getFramesProcessed() <= static_cast<uint64_t> (n / kFrame));
        if (waitEachBlock) // without waits a frame may miss its deadline and be skipped
        {
            waitedProcessed += p.getFramesProcessed();
            ++waitedIterations;
        }
        // Destroyed here, possibly while the worker is polling.
    }
    CHECK (waitedIterations == 10);
    CHECK (waitedProcessed == static_cast<uint64_t> (waitedIterations * (n / kFrame)));

    // An explicit poll interval is capped at 100 ms: the worker notices a stop
    // request only between sleeps, so prepare() and the destructor wait that long.
    AsyncModelConfig slow;
    slow.workerPollMicroseconds = std::numeric_limits<int>::max();
    const AsyncModelProcessor capped (std::make_unique<IdentityRunner> (kFrame), slow);
    CHECK (capped.getConfig().workerPollMicroseconds == 100000);
}
