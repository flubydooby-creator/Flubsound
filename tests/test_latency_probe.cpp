// Loopback latency probe (docs/11 E42d): exponential-sweep deconvolution
// with flub::Fft, parabolic peak, median of the runs, runs under 30 dB SNR
// rejected. Checked on synthetic "recordings": the probe delayed by a known
// integer or fractional number of samples, filtered like a device, inverted,
// buried in noise, with a start offset against a reference channel, and
// through the processing chain itself, whose latency is known exactly.
#include "TestFramework.h"
#include "TestSignals.h"

#include "LatencyProbeCommand.h"

#include "flub/analysis/LatencyProbe.h"
#include "flub/common/Math.h"
#include "flub/dsp/Biquad.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/io/FilePath.h"
#include "flub/io/Json.h"
#include "flub/io/PresetIO.h"
#include "flub/io/WavFile.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace flub;
using namespace flub::latency;
using namespace flubtest;

namespace
{
/** Short sweeps keep each case fast; the defaults (1 s sweeps) behave the same. */
ProbeSettings quickSettings (double sampleRate)
{
    ProbeSettings s;
    s.sampleRate = sampleRate;
    s.sweepSeconds = 0.25;
    s.maxDelayMs = 100.0;
    s.leadInSeconds = 0.1;
    s.runs = 10;
    return s;
}

/** x delayed by `delay` samples (integer or fractional: Blackman-windowed
    sinc, 64 taps a side), same length. */
std::vector<float> delayed (const std::vector<float>& x, double delay)
{
    std::vector<float> y (x.size(), 0.0f);
    const int whole = static_cast<int> (std::floor (delay));
    const double frac = delay - whole;
    if (frac == 0.0)
    {
        for (size_t i = 0; i + static_cast<size_t> (whole) < x.size(); ++i)
            y[i + static_cast<size_t> (whole)] = x[i];
        return y;
    }
    constexpr int kHalf = 64;
    std::vector<double> taps;
    for (int k = -kHalf + 1; k <= kHalf; ++k)
    {
        const double t = k - frac;
        const double sinc = std::sin (kPi * t) / (kPi * t);
        const double w = 0.42 + 0.5 * std::cos (kPi * t / kHalf) + 0.08 * std::cos (kTwoPi * t / kHalf);
        taps.push_back (sinc * w);
    }
    const auto n = static_cast<int64_t> (x.size());
    for (int64_t i = 0; i < n; ++i)
    {
        double acc = 0.0;
        for (int k = -kHalf + 1; k <= kHalf; ++k)
        {
            const int64_t src = i - whole - k;
            if (src >= 0 && src < n)
                acc += taps[static_cast<size_t> (k + kHalf - 1)] * x[static_cast<size_t> (src)];
        }
        y[static_cast<size_t> (i)] = static_cast<float> (acc);
    }
    return y;
}

/** RBJ cookbook 2nd-order low / high-pass. */
BiquadCoeffs rbjPass (bool high, double hz, double q, double fs)
{
    const double w = kTwoPi * hz / fs, alpha = std::sin (w) / (2.0 * q), c = std::cos (w);
    const double a0 = 1.0 + alpha;
    BiquadCoeffs k;
    k.b0 = (high ? (1.0 + c) : (1.0 - c)) / 2.0 / a0;
    k.b1 = (high ? -(1.0 + c) : (1.0 - c)) / a0;
    k.b2 = k.b0;
    k.a1 = -2.0 * c / a0;
    k.a2 = (1.0 - alpha) / a0;
    return k;
}

void addNoise (std::vector<float>& x, float rms, uint32_t seed, size_t from = 0, size_t to = SIZE_MAX)
{
    FastRandom rng (seed);
    const float scale = rms * std::sqrt (3.0f); // uniform: rms = a / sqrt 3
    for (size_t i = from; i < std::min (to, x.size()); ++i)
        x[i] += scale * rng.nextBipolar();
}
} // namespace

TEST_CASE ("LatencyProbe (E42d): integer delays are recovered to 0.05 samples at 44.1, 48 and 96 kHz")
{
    for (const double fs : { 44100.0, 48000.0, 96000.0 })
    {
        const auto settings = quickSettings (fs);
        REQUIRE (validate (settings).empty());
        const auto probe = makeProbe (settings);
        REQUIRE (static_cast<int64_t> (probe.size()) == probeLength (settings));
        LatencyProbe lp (settings);
        for (const int delay : { 0, 1, 257, static_cast<int> (0.080 * fs) })
        {
            const auto recording = delayed (probe, delay);
            const auto r = lp.analyse (recording.data(), static_cast<int64_t> (recording.size()));
            REQUIRE (r.ok);
            CHECK (r.acceptedRuns == 10);
            CHECK_NEAR (r.delaySamples, delay, 0.05);
            CHECK_NEAR (r.delayMs, 1000.0 * delay / fs, 0.05 * 1000.0 / fs);
            CHECK (r.spreadSamples < 0.01);
            CHECK (! r.inverted);
            for (const auto& run : r.runs)
                CHECK (run.snrDb > 60.0);
        }
    }
}

TEST_CASE ("LatencyProbe (E42d): a fractional delay, attenuated, inverted and noisy, within 0.1 samples; through device-like filters within 0.1 ms")
{
    const auto settings = quickSettings (48000.0);
    const auto probe = makeProbe (settings);
    LatencyProbe lp (settings);

    // 1234.37 samples (25.716 ms), 30 dB of level loss, polarity inverted,
    // white noise 40 dB under the sweep; then also a 2nd-order 60 Hz
    // high-pass and 18 kHz low-pass (a small interface's coupling and
    // anti-alias filters), whose own group delay (about 0.6 samples at low
    // frequencies for the low-pass) is part of the path.
    constexpr double kDelay = 1234.37;
    const float sweepRms = static_cast<float> (std::pow (10.0, settings.levelDbfs / 20.0) / std::sqrt (2.0));
    for (const bool filtered : { false, true })
    {
        auto recording = delayed (probe, kDelay);
        const auto hp = rbjPass (true, 60.0, 0.7071, 48000.0), lp18 = rbjPass (false, 18000.0, 0.7071, 48000.0);
        BiquadState hpState, lpState;
        for (auto& v : recording)
            v = static_cast<float> (-0.0316 * (filtered ? biquadTick (lp18, lpState, biquadTick (hp, hpState, v)) : v));
        addNoise (recording, 0.0316f * sweepRms * 0.01f, 99);

        const auto r = lp.analyse (recording.data(), static_cast<int64_t> (recording.size()));
        REQUIRE (r.ok);
        CHECK (r.acceptedRuns == 10);
        CHECK (r.inverted);
        CHECK (r.spreadSamples < 0.1);
        CHECK_NEAR (r.delayMs, kDelay / 48.0, 0.1); // the Done-when's 0.1 ms
        CHECK_NEAR (r.delaySamples, kDelay, filtered ? 1.0 : 0.1);
    }
}

TEST_CASE ("LatencyProbe (E42d): runs under 30 dB SNR are rejected; the median of the rest stays exact")
{
    const auto settings = quickSettings (48000.0);
    const auto probe = makeProbe (settings);
    LatencyProbe lp (settings);
    constexpr int kDelay = 480; // 10 ms
    const float sweepRms = static_cast<float> (std::pow (10.0, settings.levelDbfs / 20.0) / std::sqrt (2.0));

    // Runs 1, 4 and 7 buried in noise 30 dB over the sweep (they read 10 to
    // 13 dB, the largest noise peak; equal noise and sweep still read 35 dB);
    // a loud click in run 5.
    auto recording = delayed (probe, kDelay);
    for (const int run : { 1, 4, 7 })
    {
        const auto from = static_cast<size_t> (runStart (settings, run));
        addNoise (recording, 31.6f * sweepRms, static_cast<uint32_t> (run + 1), from,
                  from + static_cast<size_t> (sweepLength (settings) + gapLength (settings)));
    }
    recording[static_cast<size_t> (runStart (settings, 5) + 3000)] += 0.9f;

    auto r = lp.analyse (recording.data(), static_cast<int64_t> (recording.size()));
    REQUIRE (r.ok);
    REQUIRE (r.runs.size() == 10);
    CHECK (r.acceptedRuns == 7);
    for (const int run : { 1, 4, 7 })
    {
        CHECK (! r.runs[static_cast<size_t> (run)].accepted);
        CHECK (r.runs[static_cast<size_t> (run)].snrDb < 30.0);
    }
    CHECK_NEAR (r.delaySamples, kDelay, 0.05);

    // Everything that noisy: no result, and the reason says so.
    auto noisy = delayed (probe, kDelay);
    addNoise (noisy, 31.6f * sweepRms, 5);
    r = lp.analyse (noisy.data(), static_cast<int64_t> (noisy.size()));
    CHECK (! r.ok);
    CHECK (r.acceptedRuns < 5);
    CHECK (r.error.find ("30 dB") != std::string::npos);

    // A recording that does not reach the first sweep, and silence.
    r = lp.analyse (noisy.data(), runStart (settings, 0) + 10);
    CHECK (! r.ok);
    std::vector<float> silence (probe.size(), 0.0f);
    r = lp.analyse (silence.data(), static_cast<int64_t> (silence.size()));
    CHECK (! r.ok);
}

TEST_CASE ("LatencyProbe (E42d): against a reference channel an unknown start offset cancels")
{
    const auto settings = quickSettings (48000.0);
    const auto probe = makeProbe (settings);
    LatencyProbe lp (settings);

    // The recorder started 1.3 s before playback (far beyond maxDelayMs):
    // the direct path arrives 62 samples later, the measured one 62 + 700.25.
    std::vector<float> padded (static_cast<size_t> (1.3 * 48000.0), 0.0f);
    padded.insert (padded.end(), probe.begin(), probe.end());
    const auto reference = delayed (padded, 62.0);
    auto measured = delayed (padded, 762.25);
    addNoise (measured, 1.0e-4f, 3);

    const auto located = lp.locate (reference.data(), static_cast<int64_t> (reference.size()));
    CHECK (std::llabs (located - (static_cast<int64_t> (padded.size() - probe.size()) + 62 + runStart (settings, 0))) <= 2);

    const auto r = lp.analyseRelative (measured.data(), reference.data(), static_cast<int64_t> (measured.size()));
    REQUIRE (r.ok);
    CHECK (r.acceptedRuns >= 9);
    CHECK_NEAR (r.delaySamples, 700.25, 0.25);

    // Against itself: 0.
    const auto self = lp.analyseRelative (reference.data(), reference.data(), static_cast<int64_t> (reference.size()));
    REQUIRE (self.ok);
    CHECK_NEAR (self.delaySamples, 0.0, 1.0e-9);

    // The absolute analysis of that recording cannot see 1.3 s: it fails
    // instead of reporting a wrong delay.
    CHECK (! lp.analyse (measured.data(), static_cast<int64_t> (measured.size())).ok);
}

TEST_CASE ("LatencyProbe (E42d): measures the processing chain's reported latency on every profile to 0.1 samples")
{
    // The probe through the real chain (defaults: EQ, dynamics, maximizer
    // all running) on each latency profile: the measured delay is the
    // latency the chain reports, what the plug-in tells its host.
    const auto settings = quickSettings (48000.0);
    const auto probe = makeProbe (settings);
    LatencyProbe lp (settings);
    for (const auto profile : { param::LatencyProfileValue::Quality, param::LatencyProfileValue::Balanced, param::LatencyProfileValue::LowLatency })
    {
        param::ParameterStore store;
        store.set (param::LatencyProfile, static_cast<float> (profile));
        ProcessingChain chain (store);
        chain.prepare ({ 48000.0, 256, 2 });
        const int reported = chain.getLatencySamples();

        Planar buf (2, static_cast<int> (probe.size()));
        buf.ch[0] = probe;
        buf.ch[1] = probe;
        for (int pos = 0; pos < buf.numSamples(); pos += 256)
            chain.process (buf.block (pos, std::min (256, buf.numSamples() - pos)));
        const auto r = lp.analyse (buf.ch[0].data(), static_cast<int64_t> (buf.ch[0].size()));
        REQUIRE (r.ok);
        CHECK (r.acceptedRuns == 10);
        CHECK_NEAR (r.delaySamples, reported, 0.1); // measured 1351.997 / 191.997 / 99.997
    }
}

TEST_CASE ("LatencyProbe (E42d): the strongest other arrival reports an echo nearly as strong as the direct path")
{
    // The app's live measurement warns when a second path is nearly as
    // strong as the first (a headset's sidetone loop, an echo): the delay is
    // then ambiguous although every run has a high SNR.
    const auto settings = quickSettings (48000.0);
    const auto probe = makeProbe (settings);
    LatencyProbe lp (settings);

    // A clean path: nothing else within 0 .. maxDelayMs (the band-limited
    // impulse's own skirt stays far down).
    const auto clean = delayed (probe, 300.0);
    auto r = lp.analyse (clean.data(), static_cast<int64_t> (clean.size()));
    REQUIRE (r.ok);
    CHECK (r.secondaryDb < -40.0);
    CHECK (r.medianSnrDb > 60.0);
    const double cleanSecondary = r.secondaryDb;

    // The direct path plus an echo 3 dB down, 10 ms later: the delay is
    // still the direct path's, and the echo is reported where it is.
    auto echo = delayed (probe, 300.0);
    const auto late = delayed (probe, 780.0);
    for (size_t i = 0; i < echo.size(); ++i)
        echo[i] += 0.7079f * late[i];
    r = lp.analyse (echo.data(), static_cast<int64_t> (echo.size()));
    REQUIRE (r.ok);
    CHECK (r.acceptedRuns == 10);
    CHECK_NEAR (r.delaySamples, 300.0, 0.05);
    CHECK_NEAR (r.secondaryDb, -3.0, 0.2);
    for (const auto& run : r.runs)
        CHECK_NEAR (run.secondaryLagSamples, 780.0, 1.0);
    std::printf ("    strongest other arrival: clean path %.1f dB, with a -3 dB echo %.2f dB at %.0f samples\n", cleanSecondary, r.secondaryDb,
                 r.runs.front().secondaryLagSamples);
}

TEST_CASE ("LatencyProbe (E42d): settings are validated")
{
    ProbeSettings s;
    CHECK (validate (s).empty());
    CHECK (gapLength (s) == 36000); // 500 + 250 ms at 48 kHz
    CHECK (probeLength (s) == 24000 + 10 * (48000 + 36000));
    s.runs = 0;
    CHECK (! validate (s).empty());
    s = {};
    s.sampleRate = 8000.0; // 0.45 fs = 3.6 kHz: the sweep ends there
    CHECK (validate (s).empty());
    s.startHz = 1000.0; // 1 .. 3.6 kHz is under two octaves
    CHECK (! validate (s).empty());
    s = {};
    s.maxDelayMs = 0.0;
    CHECK (! validate (s).empty());
    s = {};
    s.levelDbfs = 3.0;
    CHECK (! validate (s).empty());
}

TEST_CASE ("CLI: latency-probe generate / analyze on files (E42d)")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("flub-latency-probe-" + preset::makeUuid());
    fs::create_directories (dir);
    const auto probePath = io::pathToUtf8 (dir / "probe.wav");
    const auto recPath = io::pathToUtf8 (dir / "recorded.wav");
    const std::vector<std::string> quick { "--sweep-ms", "250", "--max-delay-ms", "100", "--runs", "10" };

    auto args = std::vector<std::string> { "generate", "-o", probePath };
    args.insert (args.end(), quick.begin(), quick.end());
    REQUIRE (cli::runLatencyProbe (args) == 0);

    io::AudioFileData data;
    std::string error;
    REQUIRE (io::readWav (probePath, data, error));
    CHECK (data.numChannels == 2);
    CHECK (data.sampleRate == 48000.0);

    // "Recorded": channel 0 the device path (+300 samples), channel 1 the
    // direct reference (+20 samples), both behind 0.4 s of silence.
    io::AudioFileData rec;
    rec.sampleRate = 48000.0;
    rec.numChannels = 2;
    std::vector<float> padded (19200, 0.0f);
    padded.insert (padded.end(), data.channels[0].begin(), data.channels[0].end());
    rec.channels = { delayed (padded, 300.0), delayed (padded, 20.0) };
    REQUIRE (io::writeWav (recPath, rec, io::SampleFormat::Float32, error));

    // Relative to channel 1: 280 samples.
    std::string out;
    args = { "analyze", "-i", recPath, "--channel", "1", "--ref-channel", "2", "--json" };
    args.insert (args.end(), quick.begin(), quick.end());
    REQUIRE (cli::runLatencyProbe (args, &out) == 0);
    json::Value v;
    REQUIRE (json::parse (out, v, error));
    CHECK_NEAR (v["delaySamples"].asNumber(), 280.0, 0.05);
    CHECK_NEAR (v["delayMs"].asNumber(), 280.0 / 48.0, 0.01);
    CHECK (v["acceptedRuns"].asNumber() == 10.0);
    CHECK (v["runs"].asArray().size() == 10);
    CHECK (v["relative"].asBool());

    // Absolute on a file that does not start with the probe: the 0.4 s
    // offset is out of reach (exit 1, measurement failed); on a locked file
    // it is exact.
    args = { "analyze", "-i", recPath };
    args.insert (args.end(), quick.begin(), quick.end());
    CHECK (cli::runLatencyProbe (args, &out) == 1);
    rec.channels = { delayed (data.channels[0], 37.0) };
    rec.numChannels = 1;
    REQUIRE (io::writeWav (recPath, rec, io::SampleFormat::Pcm24, error));
    REQUIRE (cli::runLatencyProbe (args, &out) == 0);
    CHECK (out.find ("37.0 samples") != std::string::npos);

    // Usage errors.
    CHECK (cli::runLatencyProbe ({ "analyze" }, &out) == 2);
    CHECK (cli::runLatencyProbe ({ "analyze", "-i", recPath, "--channel", "3" }, &out) == 2);
    CHECK (cli::runLatencyProbe ({ "generate", "-o", probePath, "--runs", "0" }, &out) == 2);
    CHECK (cli::runLatencyProbe ({ "frobnicate" }, &out) == 2);
    CHECK (cli::runLatencyProbe ({ "--help" }, &out) == 0);

    std::error_code ec;
    fs::remove_all (dir, ec);
}
