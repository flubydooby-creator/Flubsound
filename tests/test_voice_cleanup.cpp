// Tests for ControlKind::BandGains (AsyncModelProcessor's STFT renderer) and the
// neural voice cleanup model on it (flub/neural/VoiceCleanupRunner.h): exact
// reconstruction at unity gains, gains landing on the window they were computed
// from, the fallback, the band-layout validation, the real worker thread with
// the app's block size (waiting for it: determinism and bookkeeping; paced at
// real time: deadlines), the chain's eligibility rules, allocation-free
// inference, the cost per frame, and a quality floor on a held-out clip
// (tests/data/neural/voice-cleanup-clip.wav, written by
// tools/neural/train_voice_cleanup.py export).
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/dsp/SpectralNoiseGate.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/io/WavFile.h"
#include "flub/neural/AsyncModelProcessor.h"
#include "flub/neural/VoiceCleanupRunner.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

#if defined(__APPLE__)
    #include <mach/mach.h>
    #include <mach/mach_time.h>
    #include <mach/thread_policy.h>
    #include <pthread.h>
#endif

using namespace flub;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;
constexpr int kHop = VoiceCleanupRunner::kFrameSize;

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

/** A BandGains stand-in: every band gets gain (frame), where the frame number is
    read from the input (the test writes value (k + 1) * 1e-3 into every sample of
    input frame k), or a constant; it can fail from a given frame on. */
class ScriptedBandRunner : public ModelRunner
{
public:
    enum class Mode
    {
        Unity,
        InverseFrameNumber, // gain = 1 / (k + 1)
        HalfThenFail        // 0.5 for frames < failFrom, then run() fails
    };
    ScriptedBandRunner (Mode m, int failFromFrame = 0) : mode (m), failFrom (failFromFrame) {}

    /** The voice cleanup's layout: 240-sample hop, 512-point FFT, its 22 band centres. */
    static ModelDescription validDescription()
    {
        ModelDescription d;
        d.frameSize = kHop;
        d.numControls = VoiceCleanupRunner::kNumBands;
        d.controlKind = ControlKind::BandGains;
        d.sampleRate = kFs;
        d.fftSize = 512;
        d.bandCentresHz = VoiceCleanupRunner::kBandCentresHz;
        return d;
    }
    ModelDescription describe() const override { return description; }

    ModelDescription description = validDescription(); // a test may break it

    void reset() override { frames = 0; }
    bool run (const float* in, float* out) override
    {
        const int k = frames++;
        float g = 1.0f;
        if (mode == Mode::InverseFrameNumber)
            g = 1.0f / std::round (in[kHop / 2] * 1000.0f);
        else if (mode == Mode::HalfThenFail)
        {
            if (k >= failFrom)
                return false;
            g = 0.5f;
        }
        std::fill (out, out + description.numControls, g);
        return true;
    }

private:
    Mode mode;
    int failFrom = 0, frames = 0;
};

/** Gives the calling thread the scheduling a device callback has: on macOS
    the time-constraint policy Core Audio's I/O thread runs with (period one
    host block, half of it computation). Elsewhere it changes nothing (an
    ordinary thread keeps time there: TestMain sets Windows' 1 ms timer).
    Returns true when the policy was set. */
bool scheduleLikeAudioCallback (double blockSeconds)
{
#if defined(__APPLE__)
    mach_timebase_info_data_t timebase {};
    if (mach_timebase_info (&timebase) != KERN_SUCCESS || timebase.numer == 0 || timebase.denom == 0)
        return false;
    const auto ticks = [&timebase] (double seconds) {
        return static_cast<uint32_t> (seconds * 1.0e9 * static_cast<double> (timebase.denom) / static_cast<double> (timebase.numer));
    };
    thread_time_constraint_policy_data_t policy {};
    policy.period = ticks (blockSeconds);
    policy.computation = ticks (0.5 * blockSeconds);
    policy.constraint = ticks (blockSeconds);
    policy.preemptible = 1;
    return thread_policy_set (pthread_mach_thread_np (pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, reinterpret_cast<thread_policy_t> (&policy),
                              THREAD_TIME_CONSTRAINT_POLICY_COUNT)
           == KERN_SUCCESS;
#else
    (void) blockSeconds;
    return false;
#endif
}

AsyncModelConfig config (int safetyFrames, bool offline)
{
    AsyncModelConfig c;
    c.safetyFrames = safetyFrames;
    c.offline = offline;
    return c;
}

/** Runs `in` (mono, duplicated to two channels) through p in blocks of `block`;
    returns channel 0. Waits for the worker between blocks when it is real time. */
std::vector<float> runThrough (AsyncModelProcessor& p, const std::vector<float>& in, int block, bool wait)
{
    std::vector<float> l (in), r (in);
    for (size_t pos = 0; pos < in.size(); pos += static_cast<size_t> (block))
    {
        const int n = static_cast<int> (std::min (static_cast<size_t> (block), in.size() - pos));
        AudioBlock b (std::array<float*, 2> { l.data() + pos, r.data() + pos }.data(), 2, n);
        p.process (b);
        if (wait)
            REQUIRE (waitUntil ([&p] { return p.getPendingFrames() == 0; }));
    }
    return l;
}

double snrDb (const std::vector<float>& clean, const std::vector<float>& y, size_t from, size_t to)
{
    double s = 0.0, e = 0.0;
    for (size_t i = from; i < to; ++i)
    {
        s += static_cast<double> (clean[i]) * clean[i];
        const double d = static_cast<double> (y[i]) - clean[i];
        e += d * d;
    }
    return 10.0 * std::log10 (s / std::max (e, 1.0e-30));
}

double energyDb (const std::vector<float>& x, size_t from, size_t to)
{
    double e = 0.0;
    for (size_t i = from; i < to; ++i)
        e += static_cast<double> (x[i]) * x[i];
    return 10.0 * std::log10 (std::max (e, 1.0e-30));
}

/** The model alone, offline (deterministic), output aligned with the input. */
std::vector<float> cleanOffline (const std::vector<float>& x)
{
    AsyncModelProcessor p (std::make_unique<VoiceCleanupRunner>(), config (2, true));
    p.prepare ({ kFs, 480, 2 });
    const int lat = p.latencySamples();
    std::vector<float> in (x);
    in.resize (x.size() + static_cast<size_t> (lat), 0.0f);
    auto y = runThrough (p, in, 480, false);
    return std::vector<float> (y.begin() + lat, y.end());
}
} // namespace

TEST_CASE ("Neural BandGains: unity band gains reconstruct the input delayed by L = frameSize * (2 + safetyFrames), for any block size")
{
    const auto x = whiteNoise (24000, 0.5f, 3);
    for (const int safety : { 0, 1, 2 })
        for (const bool offline : { true, false })
        {
            if (safety == 0 && ! offline)
                continue; // no head room for a worker (AsyncModelProcessor.h)
            for (const int block : { 1, 77, 240, 480 })
            {
                // real time: the longest block the safety frames allow, and an odd one
                if (! offline && (block > safety * kHop || (block != 77 && block != safety * kHop)))
                    continue;
                AsyncModelProcessor p (std::make_unique<ScriptedBandRunner> (ScriptedBandRunner::Mode::Unity), config (safety, offline));
                p.prepare ({ kFs, std::max (block, 1), 2 });
                REQUIRE (p.isModelActive());
                const int lat = p.latencySamples();
                CHECK (lat == kHop * (2 + safety));
                const auto y = runThrough (p, x, block, ! offline);
                // The first L samples are the (silent) delay line, to within the FFT's rounding.
                double err = 0.0, ref = 0.0;
                for (size_t i = 0; i < x.size(); ++i)
                {
                    const float expected = i < static_cast<size_t> (lat) ? 0.0f : x[i - static_cast<size_t> (lat)];
                    err = std::max (err, static_cast<double> (std::abs (y[i] - expected)));
                    ref = std::max (ref, static_cast<double> (std::abs (x[i])));
                }
                const double nullDb = 20.0 * std::log10 (std::max (err, 1.0e-30) / ref);
                if (block == 77)
                    std::printf ("    safety %d %s: L = %d, null %.1f dB\n", safety, offline ? "offline " : "realtime", lat, nullDb);
                CHECK_LE (nullDb, -120.0);
            }
        }
}

TEST_CASE ("Neural BandGains: the gains computed from window k are applied to window k (the overlap-add crossfades them across the hop)")
{
    for (const bool offline : { true, false })
    {
        AsyncModelProcessor p (std::make_unique<ScriptedBandRunner> (ScriptedBandRunner::Mode::InverseFrameNumber), config (2, offline));
        p.prepare ({ kFs, 480, 2 });
        const int frames = 40;
        std::vector<float> x (static_cast<size_t> (frames * kHop));
        for (int k = 0; k < frames; ++k)
            std::fill (x.begin() + k * kHop, x.begin() + (k + 1) * kHop, static_cast<float> (k + 1) * 1.0e-3f);
        const auto y = runThrough (p, x, offline ? 333 : 480, ! offline);
        const int lat = p.latencySamples();
        // Input frame k is the second half of window k (gain 1/(k+1)) and the first half of
        // window k + 1 (gain 1/(k+2)): y = x (w[N + n]^2 / (k + 1) + w[n]^2 / (k + 2)).
        double worst = 0.0;
        for (int k = 1; k + 1 < frames - lat / kHop; ++k)
            for (int n = 0; n < kHop; ++n)
            {
                const double wa = bandgains::vorbisWindow (kHop + n, 2 * kHop), wb = bandgains::vorbisWindow (n, 2 * kHop);
                const double expected = (k + 1) * 1.0e-3 * (wa * wa / (k + 1) + wb * wb / (k + 2));
                const double got = y[static_cast<size_t> (lat + k * kHop + n)];
                worst = std::max (worst, std::abs (got - expected));
            }
        std::printf ("    %s: largest deviation %.3g\n", offline ? "offline" : "realtime", worst);
        CHECK_LE (worst, 2.0e-8);
        CHECK (p.getDeadlineMisses() == 0u);
    }
}

TEST_CASE ("Neural BandGains: a failing model holds the last good band gains, then returns to unity after K frames")
{
    AsyncModelConfig c = config (1, true);
    c.fallbackAfterFrames = 3;
    AsyncModelProcessor p (std::make_unique<ScriptedBandRunner> (ScriptedBandRunner::Mode::HalfThenFail, 20), c);
    p.prepare ({ kFs, 480, 2 });
    std::vector<float> x (static_cast<size_t> (60 * kHop), 0.25f);
    const auto y = runThrough (p, x, 480, false);
    const auto lat = static_cast<size_t> (p.latencySamples());
    // Windows 0..19 get 0.5; 20 and 21 fail and hold it; the third failure in a row
    // (window 22) sets unity. Input frame j is in windows j and j + 1.
    CHECK_NEAR (y[lat + 10 * kHop + 100], 0.125f, 1.0e-6f);
    CHECK_NEAR (y[lat + 20 * kHop + 100], 0.125f, 1.0e-6f);
    CHECK (y[lat + 21 * kHop + 120] > 0.13f);  // the crossfade to unity
    CHECK_NEAR (y[lat + 30 * kHop + 100], 0.25f, 1.0e-6f);
    CHECK (p.getModelFailures() > 0u);
    float steepest = 0.0f;
    for (size_t i = lat + kHop; i < y.size(); ++i) // after the input's own start
        steepest = std::max (steepest, std::abs (y[i] - y[i - 1]));
    CHECK_LE (steepest, 0.125f * 2.5f / kHop); // a crossfade over one hop, no step
}

TEST_CASE ("Neural BandGains: an invalid band layout gives an inert processor (no latency, audio untouched)")
{
    // isValidBandLayout(): fftSize a power of two, 2 * frameSize .. kMaxModelFftSize;
    // numControls >= 2; band centres present, finite, >= 0 and strictly increasing.
    static const float equalCentres[] = { 0.0f, 1000.0f, 1000.0f, 4000.0f };
    static const float fallingCentres[] = { 0.0f, 2000.0f, 1000.0f, 4000.0f };
    static const float negativeCentre[] = { -10.0f, 1000.0f, 2000.0f, 4000.0f };
    static const float nanCentre[] = { 0.0f, std::numeric_limits<float>::quiet_NaN(), 2000.0f, 4000.0f };
    static const float infiniteCentre[] = { 0.0f, 1000.0f, 2000.0f, std::numeric_limits<float>::infinity() };
    static const float goodCentres[] = { 0.0f, 1000.0f, 2000.0f, 4000.0f };
    struct Broken
    {
        const char* what;
        int fftSize, numControls;
        const float* centres;
    };
    const Broken cases[] = {
        { "fftSize 0", 0, 4, goodCentres },
        { "fftSize 500 (not a power of two)", 500, 4, goodCentres },
        { "fftSize 256 < 2 x frameSize", 256, 4, goodCentres },
        { "fftSize 65536 > kMaxModelFftSize", 65536, 4, goodCentres },
        { "1 band", 512, 1, goodCentres },
        { "no band centres", 512, 4, nullptr },
        { "equal centres", 512, 4, equalCentres },
        { "falling centres", 512, 4, fallingCentres },
        { "negative centre", 512, 4, negativeCentre },
        { "NaN centre", 512, 4, nanCentre },
        { "infinite centre", 512, 4, infiniteCentre },
    };
    const auto x = whiteNoise (2400, 0.5f, 21);
    struct Outcome
    {
        int latency = -1;
        bool active = true, untouched = false;
    };
    const auto runCase = [&x] (const ModelDescription& d)
    {
        auto runner = std::make_unique<ScriptedBandRunner> (ScriptedBandRunner::Mode::Unity);
        runner->description = d;
        AsyncModelProcessor p (std::move (runner), config (2, true));
        p.prepare ({ kFs, 480, 2 });
        Outcome o;
        o.latency = p.latencySamples();
        o.active = p.isModelActive();
        const auto y = runThrough (p, x, 480, false);
        o.untouched = std::memcmp (y.data(), x.data(), x.size() * sizeof (float)) == 0;
        return o;
    };

    // The same description with good values is valid (so each case below breaks one rule only).
    auto good = ScriptedBandRunner::validDescription();
    good.numControls = 4;
    good.bandCentresHz = goodCentres;
    const Outcome valid = runCase (good);
    CHECK (valid.latency == 960);
    CHECK (valid.active);
    for (const auto& k : cases)
    {
        auto d = good;
        d.fftSize = k.fftSize;
        d.numControls = k.numControls;
        d.bandCentresHz = k.centres;
        const Outcome o = runCase (d);
        if (o.latency != 0 || o.active || ! o.untouched)
            std::printf ("    %s: latency %d, active %d, untouched %d\n", k.what, o.latency, o.active ? 1 : 0, o.untouched ? 1 : 0);
        CHECK (o.latency == 0);
        CHECK (! o.active);
        CHECK (o.untouched);
    }

    // In the chain such a model is InvalidModel and adds nothing.
    param::ParameterStore store;
    ProcessingChain chain (store);
    auto broken = std::make_unique<ScriptedBandRunner> (ScriptedBandRunner::Mode::Unity);
    broken->description.fftSize = 500;
    chain.setNeuralModel (std::move (broken));
    chain.prepare ({ 48000.0, 480, 2 });
    CHECK (chain.getNeuralStatus().state == NeuralSlotState::InvalidModel);
}

// Waits for the worker after every block, so no deadline can pass: this checks
// the threaded path's determinism and bookkeeping, not real-time head room
// (the paced case below does that).
TEST_CASE ("VoiceCleanup: on the real worker thread (waited for after every block) with 480-sample blocks it adds 960 samples, handles every frame and matches the offline render")
{
    auto telemetry = std::make_shared<VoiceCleanupTelemetry>();
    AsyncModelProcessor live (std::make_unique<VoiceCleanupRunner> (telemetry), config (2, false));
    live.prepare ({ kFs, 480, 2 });
    REQUIRE (live.isModelActive());
    CHECK (live.latencySamples() == 960);
    CHECK (live.getMaxBlockSizeWithoutMisses() == 480);

    // speech-like test signal plus pink-ish noise
    auto x = whiteNoise (96000, 0.02f, 9);
    for (size_t i = 0; i < x.size(); ++i)
        x[i] += static_cast<float> (0.2 * std::sin (2.0 * 3.14159265358979 * 180.0 * static_cast<double> (i) / kFs)
                                    * (std::sin (2.0 * 3.14159265358979 * 2.0 * static_cast<double> (i) / kFs) > 0.0 ? 1.0 : 0.0));
    const auto yLive = runThrough (live, x, 480, true);
    CHECK (live.getDeadlineMisses() == 0u);
    CHECK (live.getModelFailures() == 0u);
    CHECK (live.getFramesProcessed() == static_cast<uint64_t> (96000 / kHop));
    CHECK (telemetry->frames.load() == static_cast<uint64_t> (96000 / kHop));
    CHECK (telemetry->reductionDb.load() <= 0.0f);

    AsyncModelProcessor offline (std::make_unique<VoiceCleanupRunner>(), config (2, true));
    offline.prepare ({ kFs, 480, 2 });
    const auto yOff = runThrough (offline, x, 480, false);
    CHECK (std::memcmp (yLive.data(), yOff.data(), yLive.size() * sizeof (float)) == 0);
    for (float v : yLive)
        CHECK (std::isfinite (v));
}

namespace
{
/** The voice cleanup model, timed: for every run() the worker makes, which
    input frame it got (found in the test signal; the mono downmix of two equal
    channels is the signal itself) and when the run started and ended. */
class TimedVoiceCleanup : public ModelRunner
{
public:
    using Clock = std::chrono::steady_clock;
    struct Record
    {
        int frame = -1; // -1: not found in the signal
        Clock::time_point start, end;
    };

    explicit TimedVoiceCleanup (const std::vector<float>& signal) : input (signal) {}
    ModelDescription describe() const override { return model.describe(); }
    void prepare (double sampleRate) override { model.prepare (sampleRate); }
    void reset() override { model.reset(); }
    bool run (const float* in, float* out) override
    {
        Record rec;
        rec.frame = findFrame (in);
        rec.start = Clock::now();
        const bool ok = model.run (in, out);
        rec.end = Clock::now();
        if (count < records.size())
            records[count++] = rec;
        return ok;
    }

    // Written by the worker; read once it has stopped.
    std::array<Record, 512> records {};
    size_t count = 0;

private:
    int findFrame (const float* in) noexcept
    {
        const auto hop = static_cast<size_t> (kHop);
        for (size_t f = searchFrom; f + hop <= input.size(); f += hop)
            if (std::memcmp (in, input.data() + f, sizeof (float) * hop) == 0)
            {
                searchFrom = f + hop;
                return static_cast<int> (f / hop);
            }
        return -1;
    }

    VoiceCleanupRunner model;
    const std::vector<float>& input;
    size_t searchFrom = 0;
};

double percentile (std::vector<double> v, double q)
{
    if (v.empty())
        return 0.0;
    std::sort (v.begin(), v.end());
    return v[std::min (v.size() - 1, static_cast<size_t> (q * static_cast<double> (v.size())))];
}
} // namespace

// No waiting: 480-sample blocks are processed at the device's pace (one per
// 10 ms) by a thread scheduled like a device callback, and the worker must
// deliver each frame's gains within the two safety frames on its own. On
// macOS the pacing thread takes the time-constraint policy Core Audio's I/O
// thread has: the test's main thread (utility QoS) sleeps 14 - 86 ms for 10 ms on the CI
// runner (its timers are coalesced), so the blocks would come in bursts no
// device produces. The worker's own scheduling is the product's
// (AsyncModelProcessor.h, "Worker scheduling"): the time-constraint policy on
// macOS, the OS default elsewhere. CI machines are shared, so the bound is
// loose (5 % of the frames). Printed: the misses; the worker's response (a
// frame's result ready, counted from the end of the process() call that
// queued it; its deadline is the next call, 10 ms later) and the model's own
// run time on the worker.
TEST_CASE ("VoiceCleanup: paced at real time with 480-sample blocks and no waiting, the worker meets its deadlines")
{
    constexpr int kBlocks = 100; // 1 s of audio, 200 model frames
    auto x = whiteNoise (kBlocks * 480, 0.02f, 13);
    for (size_t i = 0; i < x.size(); ++i)
        x[i] += static_cast<float> (0.2 * std::sin (2.0 * 3.14159265358979 * 180.0 * static_cast<double> (i) / kFs));
    auto runner = std::make_unique<TimedVoiceCleanup> (x);
    const TimedVoiceCleanup* timed = runner.get();
    AsyncModelProcessor live (std::move (runner), config (2, false));
    live.prepare ({ kFs, 480, 2 });
    REQUIRE (live.isModelActive());
    REQUIRE (waitUntil ([&live] { return live.getWorkerScheduling() != NeuralWorkerScheduling::None; }));
#if defined(__APPLE__)
    CHECK (live.getWorkerScheduling() == NeuralWorkerScheduling::TimeConstraint);
#else
    CHECK (live.getWorkerScheduling() == NeuralWorkerScheduling::Default);
#endif

    std::vector<float> l (x), r (x);
    std::vector<TimedVoiceCleanup::Clock::time_point> queued (kBlocks);
    bool deviceScheduled = false;
    std::thread device ([&] {
        deviceScheduled = scheduleLikeAudioCallback (480.0 / kFs);
        const auto period = std::chrono::microseconds (10000);
        auto next = TimedVoiceCleanup::Clock::now();
        for (size_t k = 0; k < queued.size(); ++k)
        {
            std::this_thread::sleep_until (next);
            next += period;
            AudioBlock b (std::array<float*, 2> { l.data() + k * 480u, r.data() + k * 480u }.data(), 2, 480);
            live.process (b);
            queued[k] = TimedVoiceCleanup::Clock::now();
        }
    });
    device.join();
#if defined(__APPLE__)
    CHECK (deviceScheduled);
#endif

    // Frames reach their deadline L = 960 samples after they start: the last two
    // blocks' frames are still in the delay line when the loop ends.
    const auto due = static_cast<uint64_t> ((kBlocks - 2) * 480 / kHop);
    const uint64_t misses = live.getDeadlineMisses();
    const uint64_t failures = live.getModelFailures();
    const uint64_t framesRun = live.getFramesProcessed();
    live.releaseResources(); // joins the worker: its records are complete
    std::vector<double> responseUs, runUs;
    for (size_t i = 0; i < timed->count; ++i)
    {
        const auto& rec = timed->records[i];
        runUs.push_back (std::chrono::duration<double, std::micro> (rec.end - rec.start).count());
        const auto block = static_cast<size_t> (rec.frame / 2); // two model frames per block
        if (rec.frame >= 0 && block < queued.size())
            responseUs.push_back (std::chrono::duration<double, std::micro> (rec.end - queued[block]).count());
    }
    std::printf ("    paced at real time: %llu of %llu due frames missed their deadline, %llu failures, %llu frames run\n",
                 static_cast<unsigned long long> (misses), static_cast<unsigned long long> (due),
                 static_cast<unsigned long long> (failures), static_cast<unsigned long long> (framesRun));
    std::printf ("    worker response (deadline 10000 us): median %.0f us, 90th percentile %.0f us, 99th %.0f us, max %.0f us; model run median %.0f us, "
                 "max %.0f us\n",
                 percentile (responseUs, 0.5), percentile (responseUs, 0.9), percentile (responseUs, 0.99), percentile (responseUs, 1.0),
                 percentile (runUs, 0.5), percentile (runUs, 1.0));
    CHECK (failures == 0u);
    CHECK_LE (misses, due / 20);
    CHECK (framesRun + misses >= due);
    // Every frame that ran has its response time (else the bound below could pass on nothing).
    CHECK (static_cast<uint64_t> (responseUs.size()) + 2 >= framesRun);
#if defined(__APPLE__)
    // The time-constraint worker answers 9 frames in 10 within a quarter of the
    // deadline (macOS CI: about 0.6 ms at the 90th percentile). With the default
    // policy its coalesced poll sleeps took it to 5.0 ms (and to 8.5 ms at the
    // 99th percentile) without a miss in that run: the misses alone would not
    // show the difference.
    CHECK_LE (percentile (responseUs, 0.9), 2500.0);
#endif
    for (float v : l)
        CHECK (std::isfinite (v));
}

TEST_CASE ("VoiceCleanup: in the chain's neural slot it is eligible in Balanced (480-sample buffers) and Quality, not in Low Latency or at 44.1 kHz")
{
    using param::LatencyProfileValue;
    struct Case
    {
        LatencyProfileValue profile;
        double rate;
        int block;
        NeuralSlotState expected;
    };
    const Case cases[] = {
        { LatencyProfileValue::Balanced, 48000.0, 480, NeuralSlotState::Active },
        { LatencyProfileValue::Quality, 48000.0, 480, NeuralSlotState::Active },
        { LatencyProfileValue::LowLatency, 48000.0, 480, NeuralSlotState::Ineligible },
        { LatencyProfileValue::Balanced, 48000.0, 512, NeuralSlotState::BlockTooLarge },
        { LatencyProfileValue::Balanced, 44100.0, 441, NeuralSlotState::SampleRateMismatch },
    };
    for (const auto& k : cases)
    {
        param::ParameterStore store;
        store.set (param::LatencyProfile, static_cast<float> (k.profile));
        ProcessingChain chain (store);
        NeuralSlotConfig nc;
        nc.processor.safetyFrames = 2;
        chain.setNeuralModel (std::make_unique<VoiceCleanupRunner>(), nc);
        chain.prepare ({ k.rate, k.block, 2 });
        const auto st = chain.getNeuralStatus();
        CHECK (st.state == k.expected);
        CHECK (st.modelLatencySamples == 960);
    }
    // An unloadable model is an invalid description: the slot stays out.
    std::vector<unsigned char> broken (voiceCleanupModelData(), voiceCleanupModelData() + voiceCleanupModelSize());
    broken[100] ^= 0xFFu;
    auto runner = std::make_unique<VoiceCleanupRunner> (broken.data(), broken.size());
    CHECK (! runner->isValid());
    CHECK (runner->getLoadError().find ("checksum") != std::string::npos);
    param::ParameterStore store;
    ProcessingChain chain (store);
    chain.setNeuralModel (std::move (runner));
    chain.prepare ({ 48000.0, 480, 2 });
    CHECK (chain.getNeuralStatus().state == NeuralSlotState::InvalidModel);
}

TEST_CASE ("VoiceCleanup: inference is allocation-free and costs well under 1 ms per 5 ms frame")
{
    VoiceCleanupRunner runner;
    REQUIRE (runner.isValid());
    const auto x = whiteNoise (kHop * 400, 0.1f, 5);
    std::vector<float> gains (22);
    {
        AllocationGuard guard;
        runner.reset();
        for (int k = 0; k < 400; ++k)
            runner.processFrame (x.data() + k * kHop, gains.data());
        CHECK (guard.allocations() == 0);
    }
    std::vector<double> us;
    for (int k = 0; k < 400; ++k)
    {
        const auto t0 = std::chrono::steady_clock::now();
        runner.processFrame (x.data() + k * kHop, gains.data());
        us.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count());
    }
    std::sort (us.begin(), us.end());
    const double median = us[us.size() / 2], p99 = us[us.size() * 99 / 100];
    std::printf ("    cost per frame: median %.1f us, 99th percentile %.1f us (budget 5000 us of audio per frame)\n", median, p99);
    CHECK_LE (median, 1000.0);
}

TEST_CASE ("VoiceCleanup: quality floor on the held-out clip (fan noise at 5 dB SNR): SNR gain, clean speech kept, noise reduced")
{
#ifdef FLUB_NEURAL_TEST_CLIP
    io::AudioFileData clip;
    std::string error;
    REQUIRE (io::readWav (FLUB_NEURAL_TEST_CLIP, clip, error));
    REQUIRE (clip.numChannels == 2);
    const auto& speech = clip.channels[0];
    const auto& noise = clip.channels[1];
    std::vector<float> mix (speech.size());
    for (size_t i = 0; i < mix.size(); ++i)
        mix[i] = speech[i] + noise[i];

    const auto y = cleanOffline (mix);
    const auto yClean = cleanOffline (speech);
    const auto yNoise = cleanOffline (noise);
    const size_t from = 4800, to = mix.size(); // skip the first 100 ms (the model starts cold)
    const double snrIn = snrDb (speech, mix, from, to), snrOut = snrDb (speech, y, from, to);
    const double speechChange = energyDb (yClean, from, to) - energyDb (speech, from, to);
    const double noiseChange = energyDb (yNoise, from, to) - energyDb (noise, from, to);

    // The existing light noise reduction (R2.7, SpectralNoiseGate at its defaults) on the
    // same clip, for comparison only (docs/03 §16 has the whole test set).
    const auto gateOffline = [] (const std::vector<float>& x)
    {
        SpectralNoiseGate gate;
        gate.prepare ({ kFs, 480, 2 });
        const auto lat = static_cast<size_t> (gate.latencySamples());
        std::vector<float> l (x), r (x);
        l.resize (x.size() + lat, 0.0f);
        r.resize (x.size() + lat, 0.0f);
        for (size_t pos = 0; pos < l.size(); pos += 480)
        {
            const int n = static_cast<int> (std::min<size_t> (480, l.size() - pos));
            AudioBlock b (std::array<float*, 2> { l.data() + pos, r.data() + pos }.data(), 2, n);
            gate.process (b);
        }
        return std::vector<float> (l.begin() + static_cast<std::ptrdiff_t> (lat), l.end());
    };
    const double snrGate = snrDb (speech, gateOffline (mix), from, to);
    const double noiseGate = energyDb (gateOffline (noise), from, to) - energyDb (noise, from, to);

    std::printf ("    SNR %.2f -> %.2f dB (%+.2f dB; spectral gate %+.2f dB); clean speech %+.2f dB; noise alone %+.2f dB (gate %+.2f dB)\n",
                 snrIn, snrOut, snrOut - snrIn, snrGate - snrIn, speechChange, noiseChange, noiseGate);
    // Measured on Windows / MSVC 19.51 (2026-10-07): +3.22 dB, -0.33 dB, -16.99 dB.
    CHECK_GE (snrOut - snrIn, 3.0);
    CHECK_GE (speechChange, -0.6);
    CHECK_LE (noiseChange, -16.0);
#endif
}
