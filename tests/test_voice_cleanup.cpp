// Tests for ControlKind::BandGains (AsyncModelProcessor's STFT renderer) and the
// neural voice cleanup model on it (flub/neural/VoiceCleanupRunner.h): exact
// reconstruction at unity gains, gains landing on the window they were computed
// from, the fallback, the real worker thread with the app's block size, the
// chain's eligibility rules, allocation-free inference, the cost per frame, and
// a quality floor on a held-out clip (tests/data/neural/voice-cleanup-clip.wav,
// written by tools/neural/train_voice_cleanup.py export).
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/dsp/SpectralNoiseGate.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/io/WavFile.h"
#include "flub/neural/AsyncModelProcessor.h"
#include "flub/neural/VoiceCleanupRunner.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

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

    ModelDescription describe() const override
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
        std::fill (out, out + VoiceCleanupRunner::kNumBands, g);
        return true;
    }

private:
    Mode mode;
    int failFrom = 0, frames = 0;
};

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

TEST_CASE ("VoiceCleanup: on the real worker thread with 480-sample blocks it adds 960 samples, misses no frame and matches the offline render")
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
