// Tests for the STFT spectral noise gate: perfect reconstruction and latency,
// noise-floor learning and gating, tone preservation, floor rise rate, freeze
// and digital-silence hold, attack / release behaviour, channel independence,
// click-free parameter changes, real-time safety, robustness and block-size
// invariance.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/dsp/Fft.h"
#include "flub/dsp/SpectralNoiseGate.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <vector>

using namespace flub;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;

float dbfs (double db) { return static_cast<float> (std::pow (10.0, db / 20.0)); }

int seconds (double s, double fs = kFs) { return static_cast<int> (s * fs); }

/** Uniform white noise with the given RMS level in dBFS. */
std::vector<float> noiseAt (int numSamples, double rmsDb, uint32_t seed)
{
    return whiteNoise (numSamples, dbfs (rmsDb) * std::sqrt (3.0f), seed);
}

void prepareGate (SpectralNoiseGate& gate, double fs = kFs, int channels = 1, int fftSize = 512, int maxBlock = 512)
{
    gate.setFftSize (fftSize);
    ProcessSpec spec;
    spec.sampleRate = fs;
    spec.maxBlockSize = maxBlock;
    spec.numChannels = channels;
    gate.prepare (spec);
}

/** Copies into an existing channel (never replace Planar::ch storage: the
    channel pointers refer to it). */
void setChannel (Planar& buf, int c, const std::vector<float>& v)
{
    auto& dst = buf.ch[static_cast<size_t> (c)];
    std::copy (v.begin(), v.begin() + static_cast<std::ptrdiff_t> (std::min (v.size(), dst.size())), dst.begin());
}

/** Processes samples [from, to) of buf in blocks of blockSize. */
void processRange (Processor& proc, Planar& buf, int from, int to, int blockSize = 256)
{
    for (int pos = from; pos < to; pos += blockSize)
        proc.process (buf.block (pos, std::min (blockSize, to - pos)));
}

std::vector<float> runMono (SpectralNoiseGate& gate, const std::vector<float>& x, int blockSize = 256)
{
    Planar buf (1, static_cast<int> (x.size()));
    setChannel (buf, 0, x);
    processInBlocks (gate, buf, blockSize);
    return buf.ch[0];
}

double energy (const float* x, int n)
{
    double e = 0.0;
    for (int i = 0; i < n; ++i)
        e += static_cast<double> (x[i]) * x[i];
    return e;
}

/** Output / input energy (dB) over input samples [from, to), latency aligned. */
double gainDb (const std::vector<float>& in, const std::vector<float>& out, int latency, int from, int to)
{
    const double eIn = energy (in.data() + from, to - from);
    const double eOut = energy (out.data() + from + latency, to - from);
    return 10.0 * std::log10 (std::max (1.0e-30, eOut) / std::max (1.0e-30, eIn));
}

/** Energy inside (or outside) [loHz, hiHz]: Hann-windowed segments with 50 %
    overlap, summed over the selected FFT bins. */
double bandEnergy (const float* x, int n, double fs, double loHz, double hiHz, bool inside, int fftLen = 4096)
{
    Fft fft;
    fft.prepare (fftLen);
    std::vector<float> seg (static_cast<size_t> (fftLen));
    std::vector<std::complex<float>> spec (static_cast<size_t> (fftLen / 2 + 1));
    double e = 0.0;
    for (int start = 0; start + fftLen <= n; start += fftLen / 2)
    {
        for (int i = 0; i < fftLen; ++i)
            seg[static_cast<size_t> (i)] = x[start + i] * static_cast<float> (0.5 - 0.5 * std::cos (kTwoPi * i / fftLen));
        fft.forwardReal (seg.data(), spec.data());
        for (int k = 1; k < fftLen / 2; ++k)
        {
            const double f = k * fs / fftLen;
            if ((f >= loHz && f <= hiHz) == inside)
                e += std::norm (spec[static_cast<size_t> (k)]);
        }
    }
    return e;
}

double maxAbsDiff (const float* a, const float* b, int n)
{
    double m = 0.0;
    for (int i = 0; i < n; ++i)
        m = std::max (m, static_cast<double> (std::abs (a[i] - b[i])));
    return m;
}

bool allFiniteAndBounded (const Planar& buf, double bound)
{
    for (const auto& c : buf.ch)
        for (float v : c)
            if (! std::isfinite (v) || std::abs (v) > bound)
                return false;
    return true;
}

/** First time (s, relative to `from`) at which the latency-aligned short-term
    gain (50 ms windows, 10 ms steps) falls below `thresholdDb`; -1 if never. */
double timeToReach (const std::vector<float>& in, const std::vector<float>& out, int latency, int from, int to, double thresholdDb, double fs)
{
    const int win = seconds (0.05, fs);
    const int step = seconds (0.01, fs);
    for (int s = from; s + win <= to; s += step)
        if (gainDb (in, out, latency, s, s + win) < thresholdDb)
            return static_cast<double> (s - from) / fs;
    return -1.0;
}
} // namespace

//==============================================================================
TEST_CASE ("SpectralNoiseGate: reductionDb 0 is a perfect reconstruction delayed by fftSize")
{
    for (int size : { 256, 512, 1024, 2048, 4096 })
    {
        SpectralNoiseGate gate;
        NoiseGateParams p;
        p.reductionDb = 0.0f;
        gate.setParams (p);
        prepareGate (gate, kFs, 2, size);
        REQUIRE (gate.latencySamples() == size);

        const int n = seconds (0.5);
        Planar buf (2, n);
        auto tone = sine (997.0, kFs, n, 0.3f);
        auto noise = noiseAt (n, -30.0, 11);
        for (int i = 0; i < n; ++i)
        {
            buf.ch[0][static_cast<size_t> (i)] = tone[static_cast<size_t> (i)] + noise[static_cast<size_t> (i)];
            buf.ch[1][static_cast<size_t> (i)] = (i == 3000 ? 0.9f : 0.0f) + 0.5f * noise[static_cast<size_t> (i)];
        }
        const Planar in = buf;
        processInBlocks (gate, buf, 333);

        for (int c = 0; c < 2; ++c)
        {
            const auto& x = in.ch[static_cast<size_t> (c)];
            const auto& y = buf.ch[static_cast<size_t> (c)];
            CHECK_LE (maxAbsDiff (y.data() + size, x.data(), n - size), 1.0e-4);
            CHECK_LE (peakAbs (y.data(), size), 1.0e-4); // pre-roll: the zero history
        }
        // One sample off is clearly wrong: the delay really is fftSize.
        CHECK_GE (maxAbsDiff (buf.ch[0].data() + size - 1, in.ch[0].data(), n - size), 1.0e-2);
        CHECK_GE (maxAbsDiff (buf.ch[0].data() + size + 1, in.ch[0].data(), n - size - 1), 1.0e-2);
    }
}

TEST_CASE ("SpectralNoiseGate: fftSize is validated (power of two 256..4096, else 512)")
{
    for (int bad : { 0, -512, 100, 128, 300, 1000, 8192 })
    {
        SpectralNoiseGate gate;
        gate.setFftSize (bad);
        CHECK (gate.latencySamples() == 512);
        prepareGate (gate, kFs, 1, bad);
        CHECK (gate.latencySamples() == 512);
    }

    // Structural: latency is fixed at prepare(); a later setFftSize() only
    // takes effect with the next prepare().
    SpectralNoiseGate gate;
    gate.setFftSize (1024);
    CHECK (gate.latencySamples() == 1024);
    prepareGate (gate, kFs, 1, 1024);
    gate.setFftSize (2048);
    CHECK (gate.latencySamples() == 1024);
    prepareGate (gate, kFs, 1, 2048);
    CHECK (gate.latencySamples() == 2048);
}

TEST_CASE ("SpectralNoiseGate: a low-level impulse appears exactly latencySamples() later")
{
    struct Config
    {
        double fs;
        int size;
    };
    for (const auto& cfg : { Config { 48000.0, 512 }, Config { 96000.0, 1024 }, Config { 44100.0, 256 } })
    {
        SpectralNoiseGate gate; // default params: 12 dB reduction, nothing learned yet
        prepareGate (gate, cfg.fs, 1, cfg.size);
        const int latency = gate.latencySamples();
        REQUIRE (latency == cfg.size);

        for (int pos : { 100, 1037, 2222 }) // different phases within a hop
        {
            gate.reset();
            std::vector<float> x (static_cast<size_t> (pos + latency + 500), 0.0f);
            x[static_cast<size_t> (pos)] = 0.01f; // -40 dBFS
            const auto y = runMono (gate, x, 64);

            int argMax = 0;
            for (int i = 1; i < static_cast<int> (y.size()); ++i)
                if (std::abs (y[static_cast<size_t> (i)]) > std::abs (y[static_cast<size_t> (argMax)]))
                    argMax = i;
            CHECK (argMax == pos + latency);
            CHECK_NEAR (y[static_cast<size_t> (pos + latency)], 0.01, 1.0e-5);

            double rest = 0.0;
            for (int i = 0; i < static_cast<int> (y.size()); ++i)
                if (i != pos + latency)
                    rest = std::max (rest, static_cast<double> (std::abs (y[static_cast<size_t> (i)])));
            CHECK_LE (rest, 1.0e-6);
        }
    }
}

//==============================================================================
TEST_CASE ("SpectralNoiseGate: noise-only input converges towards -reductionDb")
{
    struct Config
    {
        double fs;
        int size;
        float reduction;
    };
    const Config configs[] = { { 48000.0, 512, 12.0f }, { 48000.0, 512, 24.0f }, { 48000.0, 1024, 12.0f },
                               { 48000.0, 1024, 24.0f }, { 44100.0, 512, 12.0f }, { 96000.0, 512, 12.0f },
                               { 48000.0, 4096, 12.0f } };
    for (const auto& cfg : configs)
    {
        SpectralNoiseGate gate;
        NoiseGateParams p;
        p.reductionDb = cfg.reduction;
        gate.setParams (p);
        prepareGate (gate, cfg.fs, 1, cfg.size);

        const int n = seconds (3.5, cfg.fs);
        const auto x = noiseAt (n, -60.0, 21);
        const auto y = runMono (gate, x);
        const int latency = gate.latencySamples();

        // Learned within the first second; measured over the following two.
        const double g = gainDb (x, y, latency, seconds (1.0, cfg.fs), n - latency);
        CHECK_LE (g, -0.85 * cfg.reduction);
        CHECK_GE (g, -cfg.reduction - 0.5);
    }
}

TEST_CASE ("SpectralNoiseGate: -60 dBFS noise is reduced, a -20 dBFS tone passes untouched")
{
    for (int size : { 512, 1024 })
    {
        SpectralNoiseGate gate; // defaults: threshold 6 dB, reduction 12 dB
        prepareGate (gate, kFs, 1, size);

        const int learn = seconds (2.0);
        const int total = seconds (4.0);
        const auto noise = noiseAt (total, -60.0, 5);
        const auto tone = sine (1000.0, kFs, total, 0.1f);
        std::vector<float> x (static_cast<size_t> (total));
        for (int i = 0; i < total; ++i)
            x[static_cast<size_t> (i)] = noise[static_cast<size_t> (i)] + (i >= learn ? tone[static_cast<size_t> (i)] : 0.0f);
        const auto y = runMono (gate, x);
        const int latency = gate.latencySamples();

        // Measure over the last 1.5 s of the tone section (input time).
        const int from = learn + seconds (0.5);
        const int len = total - latency - from;
        const float* xin = x.data() + from;
        const float* yout = y.data() + from + latency;

        const double toneIn = toneAmplitude (xin, len, 1000.0, kFs);
        const double toneOut = toneAmplitude (yout, len, 1000.0, kFs);
        CHECK_NEAR (toDb (toneOut / toneIn), 0.0, 0.5);

        // Broadband noise away from the tone (outside 0.5 .. 2 kHz).
        const double noiseIn = bandEnergy (xin, len, kFs, 500.0, 2000.0, false);
        const double noiseOut = bandEnergy (yout, len, kFs, 500.0, 2000.0, false);
        CHECK_LE (10.0 * std::log10 (noiseOut / noiseIn), -8.0);
    }
}

//==============================================================================
TEST_CASE ("SpectralNoiseGate: the floor climbs at floorRiseDbPerSec, independent of the hop length")
{
    // Learn -60 dBFS noise, then step the noise up by 20 dB. The gate opens,
    // and closes again once the floor has climbed to within ~6 dB of the new
    // level: ~14 dB at r dB/s, plus ~0.15 s for the release and the 50 ms
    // measuring window. The time must not depend on the hop length (fftSize,
    // sample rate): the per-hop rise factor is derived from the hop duration.
    struct Config
    {
        double fs;
        int size;
    };
    for (float rise : { 10.0f, 20.0f })
    {
        const double expected = 14.0 / rise + 0.15;
        double tMin = 1.0e9, tMax = -1.0e9;
        for (const auto& cfg : { Config { 48000.0, 512 }, Config { 48000.0, 2048 }, Config { 96000.0, 512 }, Config { 44100.0, 1024 } })
        {
            SpectralNoiseGate gate;
            NoiseGateParams p;
            p.floorRiseDbPerSec = rise;
            gate.setParams (p);
            prepareGate (gate, cfg.fs, 1, cfg.size);

            const int step = seconds (2.0, cfg.fs);
            const int total = step + seconds (3.0, cfg.fs);
            auto x = noiseAt (total, -60.0, 8);
            for (int i = step; i < total; ++i)
                x[static_cast<size_t> (i)] *= 10.0f;
            const auto y = runMono (gate, x);
            const int latency = gate.latencySamples();

            // Right after the step the gate is open ...
            CHECK_GE (gainDb (x, y, latency, step + seconds (0.1, cfg.fs), step + seconds (0.3, cfg.fs)), -2.0);
            // ... and it closes again after the floor has climbed.
            const double t = timeToReach (x, y, latency, step, total - latency, -6.0, cfg.fs);
            CHECK_GE (t, 0.85 * expected);
            CHECK_LE (t, 1.15 * expected);
            tMin = std::min (tMin, t);
            tMax = std::max (tMax, t);
        }
        CHECK_LE (tMax - tMin, 0.1 * expected);
    }
}

TEST_CASE ("SpectralNoiseGate: the floor follows a falling noise level at once")
{
    SpectralNoiseGate gate;
    prepareGate (gate);
    const int step = seconds (2.0);
    const int total = step + seconds (1.5);
    auto x = noiseAt (total, -40.0, 9);
    for (int i = step; i < total; ++i)
        x[static_cast<size_t> (i)] *= 0.1f; // -60 dBFS
    const auto y = runMono (gate, x);
    const int latency = gate.latencySamples();
    CHECK_LE (gainDb (x, y, latency, seconds (1.0), step), -10.0);
    CHECK_LE (gainDb (x, y, latency, step + seconds (0.3), total - latency), -10.0);
}

TEST_CASE ("SpectralNoiseGate: freezeFloor holds the learned floor when the input level rises")
{
    for (bool freeze : { false, true })
    {
        SpectralNoiseGate gate;
        NoiseGateParams p;
        p.floorRiseDbPerSec = 20.0f;
        gate.setParams (p);
        prepareGate (gate);

        const int learn = seconds (2.0);
        const int loud = learn + seconds (3.0);
        const int total = loud + seconds (1.5);
        auto x = noiseAt (total, -60.0, 13);
        for (int i = learn; i < loud; ++i)
            x[static_cast<size_t> (i)] *= 10.0f; // +20 dB for 3 s, then back to -60 dBFS

        Planar buf (1, total);
        setChannel (buf, 0, x);
        processRange (gate, buf, 0, learn);
        p.freezeFloor = freeze; // freeze the profile learned so far
        gate.setParams (p);
        processRange (gate, buf, learn, total);
        const auto& y = buf.ch[0];
        const int latency = gate.latencySamples();

        CHECK_LE (gainDb (x, y, latency, seconds (1.0), learn), -10.0);
        const double loudGain = gainDb (x, y, latency, loud - seconds (1.0), loud);
        if (freeze)
            CHECK_GE (loudGain, -1.0); // floor still at -60 dBFS: +20 dB noise passes
        else
            CHECK_LE (loudGain, -9.0); // floor has followed (20 dB at 20 dB/s)

        // Back at -60 dBFS: the frozen profile still gates it at once.
        if (freeze)
            CHECK_LE (gainDb (x, y, latency, loud + seconds (0.3), total - latency), -10.0);
    }
}

TEST_CASE ("SpectralNoiseGate: a frozen gate still learns its first profile after reset")
{
    SpectralNoiseGate gate;
    NoiseGateParams p;
    p.freezeFloor = true;
    gate.setParams (p);
    prepareGate (gate);

    const int learn = seconds (2.0);
    const int total = learn + seconds (2.0);
    auto x = noiseAt (total, -60.0, 17);
    for (int i = learn; i < total; ++i)
        x[static_cast<size_t> (i)] *= 10.0f;
    const auto y = runMono (gate, x);
    const int latency = gate.latencySamples();

    CHECK_LE (gainDb (x, y, latency, seconds (0.5), learn), -8.0);
    CHECK_GE (gainDb (x, y, latency, learn + seconds (1.0), total - latency), -1.0);
}

TEST_CASE ("SpectralNoiseGate: digital silence does not wipe the learned floor")
{
    // Learn -60 dBFS noise, pause, resume. A digitally silent pause (a gap
    // between tracks) holds the profile; a merely quiet pause (-100 dBFS noise)
    // is real material, and the floor follows it down.
    for (bool digitalSilence : { true, false })
    {
        SpectralNoiseGate gate;
        prepareGate (gate);
        const int learn = seconds (2.0);
        const int resume = learn + seconds (1.0);
        const int total = resume + seconds (1.5);
        auto x = noiseAt (total, -60.0, 19);
        for (int i = learn; i < resume; ++i)
            x[static_cast<size_t> (i)] = digitalSilence ? 0.0f : x[static_cast<size_t> (i)] * 1.0e-2f;
        const auto y = runMono (gate, x);
        const int latency = gate.latencySamples();

        const double g = gainDb (x, y, latency, resume + seconds (0.2), total - latency);
        if (digitalSilence)
            CHECK_LE (g, -10.0);
        else
            CHECK_GE (g, -2.0); // floor at -100 dBFS now, climbing at 3 dB/s
    }
}

//==============================================================================
TEST_CASE ("SpectralNoiseGate: bins open fast (attack) and close slowly (release)")
{
    const int learn = seconds (2.0);
    const int toneOff = learn + seconds (0.3);
    const int total = toneOff + seconds (2.5);
    const auto noise = noiseAt (total, -60.0, 23);
    const auto tone = sine (1000.0, kFs, total, 0.1f);
    std::vector<float> x (noise);
    for (int i = learn; i < toneOff; ++i)
        x[static_cast<size_t> (i)] += tone[static_cast<size_t> (i)];

    auto run = [&x] (float attackMs, float releaseMs)
    {
        SpectralNoiseGate gate;
        NoiseGateParams p;
        p.attackMs = attackMs;
        p.releaseMs = releaseMs;
        gate.setParams (p);
        prepareGate (gate);
        auto y = runMono (gate, x);
        y.erase (y.begin(), y.begin() + gate.latencySamples()); // latency aligned
        y.resize (x.size(), 0.0f);
        return y;
    };

    // Attack: with 1 ms the tone is at full level 20 ms after its onset; with
    // 50 ms the first 20 ms are clearly attenuated.
    const auto fast = run (1.0f, 80.0f);
    const auto slow = run (50.0f, 80.0f);
    const int onsetLen = seconds (0.02);
    const double inOnset = toneAmplitude (x.data() + learn, onsetLen, 1000.0, kFs);
    const double fastOnset = toneAmplitude (fast.data() + learn + onsetLen, onsetLen, 1000.0, kFs);
    const double slowOnset = toneAmplitude (slow.data() + learn, onsetLen, 1000.0, kFs);
    CHECK_NEAR (toDb (fastOnset / inOnset), 0.0, 0.5);
    CHECK_LE (toDb (slowOnset / toneAmplitude (fast.data() + learn, onsetLen, 1000.0, kFs)), -2.0);

    // Release: after the tone stops, the bins it opened close over the release
    // time, so with 500 ms the noise around 1 kHz stays much louder than with
    // 10 ms (identical input noise, so the ratio is the gain difference).
    const auto shortRel = run (5.0f, 10.0f);
    const auto longRel = run (5.0f, 500.0f);
    const int from = toneOff + seconds (0.2);
    const int len = seconds (0.2);
    const double eShort = bandEnergy (shortRel.data() + from, len, kFs, 800.0, 1200.0, true, 1024);
    const double eLong = bandEnergy (longRel.data() + from, len, kFs, 800.0, 1200.0, true, 1024);
    CHECK_GE (10.0 * std::log10 (eLong / eShort), 6.0);

    // Long after, both are back to the gated floor.
    const int late = total - seconds (0.3);
    const double eShortLate = bandEnergy (shortRel.data() + late, seconds (0.25), kFs, 800.0, 1200.0, true, 1024);
    const double eLongLate = bandEnergy (longRel.data() + late, seconds (0.25), kFs, 800.0, 1200.0, true, 1024);
    CHECK_NEAR (10.0 * std::log10 (eLongLate / eShortLate), 0.0, 2.0);
}

TEST_CASE ("SpectralNoiseGate: parameter changes are click-free")
{
    // A steady tone becomes the floor (it is stationary). With reductionDb 0
    // it passes; switching to 40 dB reduction (and back) must fade it, never
    // step it: no sample-to-sample jump beyond that of the tone itself.
    SpectralNoiseGate gate;
    NoiseGateParams p;
    p.reductionDb = 0.0f;
    gate.setParams (p);
    prepareGate (gate);
    const int latency = gate.latencySamples();

    const int seg = seconds (1.0);
    const float amp = 0.1f;
    const auto x = sine (1000.0, kFs, 3 * seg, amp);
    Planar buf (1, 3 * seg);
    setChannel (buf, 0, x);

    processRange (gate, buf, 0, seg);
    p.reductionDb = 40.0f;
    p.thresholdDb = 20.0f;
    gate.setParams (p);
    processRange (gate, buf, seg, 2 * seg);
    p.reductionDb = 0.0f;
    p.thresholdDb = 0.0f;
    gate.setParams (p);
    processRange (gate, buf, 2 * seg, 3 * seg);
    const auto& y = buf.ch[0];

    const double naturalStep = amp * 2.0 * kPi * 1000.0 / kFs;
    double maxStep = 0.0;
    for (size_t i = static_cast<size_t> (latency) + 1; i < y.size(); ++i)
        maxStep = std::max (maxStep, static_cast<double> (std::abs (y[i] - y[i - 1])));
    CHECK_LE (maxStep, naturalStep * 1.05);

    // It did fade: ~-40 dB at the end of the reduced section, back at unity at the end.
    const int endMid = 2 * seg + latency;
    CHECK_LE (toDb (toneAmplitude (y.data() + endMid - seconds (0.2), seconds (0.2), 1000.0, kFs) / amp), -35.0);
    CHECK_NEAR (toDb (toneAmplitude (y.data() + 3 * seg - seconds (0.2), seconds (0.2), 1000.0, kFs) / amp), 0.0, 0.1);
}

TEST_CASE ("SpectralNoiseGate: setParams clamps to the documented ranges and ignores non-finite values")
{
    SpectralNoiseGate gate;
    prepareGate (gate);
    NoiseGateParams p;
    p.thresholdDb = 99.0f;
    p.reductionDb = -5.0f;
    p.attackMs = 0.0f;
    p.releaseMs = 1.0e6f;
    p.floorRiseDbPerSec = 100.0f;
    gate.setParams (p);
    CHECK_NEAR (gate.getParams().thresholdDb, 20.0, 0.0);
    CHECK_NEAR (gate.getParams().reductionDb, 0.0, 0.0);
    CHECK_NEAR (gate.getParams().attackMs, 1.0, 0.0);
    CHECK_NEAR (gate.getParams().releaseMs, 500.0, 0.0);
    CHECK_NEAR (gate.getParams().floorRiseDbPerSec, 20.0, 0.0);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    NoiseGateParams bad;
    bad.thresholdDb = nan;
    bad.reductionDb = inf;
    bad.attackMs = -inf;
    bad.releaseMs = nan;
    bad.floorRiseDbPerSec = nan;
    gate.setParams (bad);
    CHECK_NEAR (gate.getParams().thresholdDb, 20.0, 0.0);
    CHECK_NEAR (gate.getParams().reductionDb, 0.0, 0.0);
    CHECK_NEAR (gate.getParams().attackMs, 1.0, 0.0);
    CHECK_NEAR (gate.getParams().releaseMs, 500.0, 0.0);
    CHECK_NEAR (gate.getParams().floorRiseDbPerSec, 20.0, 0.0);
}

//==============================================================================
TEST_CASE ("SpectralNoiseGate: stereo channels are processed independently")
{
    const int learn = seconds (1.5);
    const int total = seconds (3.0);
    const auto left = noiseAt (total, -60.0, 29);
    auto right = noiseAt (total, -50.0, 31);
    const auto tone = sine (440.0, kFs, total, 0.2f);
    for (int i = learn; i < total; ++i)
        right[static_cast<size_t> (i)] += tone[static_cast<size_t> (i)];

    SpectralNoiseGate stereo;
    prepareGate (stereo, kFs, 2);
    Planar buf (2, total);
    setChannel (buf, 0, left);
    setChannel (buf, 1, right);
    processInBlocks (stereo, buf, 256);

    SpectralNoiseGate monoL, monoR;
    prepareGate (monoL);
    prepareGate (monoR);
    const auto yl = runMono (monoL, left);
    const auto yr = runMono (monoR, right);

    CHECK_LE (maxAbsDiff (buf.ch[0].data(), yl.data(), total), 1.0e-7);
    CHECK_LE (maxAbsDiff (buf.ch[1].data(), yr.data(), total), 1.0e-7);

    // Silence on one side stays exactly silent next to loud material.
    SpectralNoiseGate gate;
    prepareGate (gate, kFs, 2);
    Planar quiet (2, total);
    setChannel (quiet, 1, right);
    processInBlocks (gate, quiet, 256);
    CHECK_NEAR (peakAbs (quiet.ch[0].data(), total), 0.0, 0.0);
}

TEST_CASE ("SpectralNoiseGate: a channel that drops out and returns restarts cleanly")
{
    SpectralNoiseGate gate;
    NoiseGateParams p;
    p.reductionDb = 0.0f; // transparent, so the expected output is exact
    gate.setParams (p);
    prepareGate (gate, kFs, 2);
    const int latency = gate.latencySamples();

    const int n = seconds (0.25);
    const auto x0 = noiseAt (n, -20.0, 37);
    const auto x1 = sine (300.0, kFs, n, 0.5f);
    Planar buf (2, n);
    setChannel (buf, 0, x0);
    setChannel (buf, 1, x1);

    const int dropAt = 3000, returnAt = 7001; // channel 1 absent in between
    gate.process (buf.block (0, dropAt));
    gate.process (buf.block (dropAt, returnAt - dropAt).firstChannels (1));
    gate.process (buf.block (returnAt, n - returnAt));

    // Channel 0 never noticed.
    CHECK_LE (maxAbsDiff (buf.ch[0].data() + latency, x0.data(), n - latency), 1.0e-4);
    // Channel 1 after its return: silence for the samples it missed, then its
    // new input delayed by exactly the latency.
    double err = 0.0;
    for (int t = returnAt; t < n; ++t)
    {
        const int s = t - latency;
        const float expected = s >= returnAt ? x1[static_cast<size_t> (s)] : 0.0f;
        err = std::max (err, static_cast<double> (std::abs (buf.ch[1][static_cast<size_t> (t)] - expected)));
    }
    CHECK_LE (err, 1.0e-4);
}

//==============================================================================
TEST_CASE ("SpectralNoiseGate: reset, setters and process do not allocate")
{
    SpectralNoiseGate gate;
    prepareGate (gate, kFs, 2, 1024, 512);

    Planar buf (2, 512);
    const auto noise = noiseAt (512, -40.0, 41);
    setChannel (buf, 0, noise);
    setChannel (buf, 1, noise);

    NoiseGateParams p;
    AllocationGuard guard;
    gate.reset();
    for (int b = 0; b < 40; ++b)
    {
        p.thresholdDb = static_cast<float> (b % 20);
        p.reductionDb = static_cast<float> (b % 40);
        p.attackMs = 1.0f + static_cast<float> (b);
        p.releaseMs = 10.0f + 10.0f * static_cast<float> (b);
        p.floorRiseDbPerSec = 0.5f + 0.5f * static_cast<float> (b % 40);
        p.freezeFloor = (b % 7) == 0;
        gate.setParams (p);
        gate.process (buf.block (0, 1 + (b * 37) % 512));
        gate.process (buf.block().firstChannels (1 + (b % 2)));
    }
    gate.reset();
    gate.process (buf.block());
    CHECK (guard.allocations() == 0);
}

TEST_CASE ("SpectralNoiseGate: silence, DC, full-scale noise, impulses and extreme settings stay finite")
{
    const double rates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };
    const int sizes[] = { 256, 4096 };

    NoiseGateParams extremes[3];
    extremes[0].thresholdDb = 0.0f;
    extremes[0].reductionDb = 40.0f;
    extremes[0].attackMs = 1.0f;
    extremes[0].releaseMs = 10.0f;
    extremes[0].floorRiseDbPerSec = 20.0f;
    extremes[1].thresholdDb = 20.0f;
    extremes[1].reductionDb = 0.0f;
    extremes[1].attackMs = 50.0f;
    extremes[1].releaseMs = 500.0f;
    extremes[1].floorRiseDbPerSec = 0.5f;
    extremes[1].freezeFloor = true;
    extremes[2].thresholdDb = std::numeric_limits<float>::quiet_NaN();
    extremes[2].reductionDb = 1.0e9f;
    extremes[2].attackMs = -1.0f;
    extremes[2].releaseMs = std::numeric_limits<float>::infinity();
    extremes[2].floorRiseDbPerSec = 0.0f;

    for (double fs : rates)
    {
        for (int size : sizes)
        {
            for (const auto& p : extremes)
            {
                SpectralNoiseGate gate;
                gate.setParams (p);
                prepareGate (gate, fs, 2, size);

                const int n = seconds (0.4, fs);
                Planar buf (2, n);
                const auto loud = whiteNoise (n, 1.0f, 43);
                for (int i = 0; i < n; ++i)
                {
                    const int q = i / (n / 4);
                    float v = 0.0f;                                                  // silence
                    if (q == 1)
                        v = 0.8f;                                                    // DC
                    else if (q == 2)
                        v = loud[static_cast<size_t> (i)];                           // full-scale noise
                    else if (q == 3)
                        v = (i % 997) == 0 ? ((i / 997) % 2 == 0 ? 1.0f : -1.0f) : 0.0f; // impulses
                    buf.ch[0][static_cast<size_t> (i)] = v;
                    buf.ch[1][static_cast<size_t> (i)] = q == 0 ? 0.0f : -v;
                }
                processInBlocks (gate, buf, 480);
                // Every bin gain is <= 1, but a time-varying spectral gain can
                // reshape peaks (a partly gated square wave overshoots a little).
                CHECK (allFiniteAndBounded (buf, 2.0));
                // The first quarter is digital silence: it stays exactly silent.
                CHECK_NEAR (peakAbs (buf.ch[0].data(), n / 4), 0.0, 0.0);
            }
        }
    }
}

TEST_CASE ("SpectralNoiseGate: output is identical for any host block size")
{
    const int n = seconds (0.75);
    Planar ref (2, n);
    const auto noise = noiseAt (n, -50.0, 47);
    const auto noise2 = noiseAt (n, -45.0, 53);
    const auto tone = sine (1500.0, kFs, n, 0.2f);
    for (int i = 0; i < n; ++i)
    {
        const bool burst = i > seconds (0.3) && i < seconds (0.5);
        ref.ch[0][static_cast<size_t> (i)] = noise[static_cast<size_t> (i)] + (burst ? tone[static_cast<size_t> (i)] : 0.0f);
        ref.ch[1][static_cast<size_t> (i)] = noise2[static_cast<size_t> (i)] - (i > seconds (0.6) ? tone[static_cast<size_t> (i)] : 0.0f);
    }

    NoiseGateParams p;
    p.thresholdDb = 4.0f;
    p.reductionDb = 18.0f;
    p.attackMs = 3.0f;
    p.releaseMs = 60.0f;
    p.floorRiseDbPerSec = 6.0f;

    std::vector<Planar> outs;
    for (int blockSize : { 1, 7, 64, 512 })
    {
        SpectralNoiseGate gate;
        gate.setParams (p);
        prepareGate (gate, kFs, 2, 512, 512);
        Planar buf = ref;
        buf.ptrs.clear();
        for (auto& c : buf.ch)
            buf.ptrs.push_back (c.data());
        for (int pos = 0; pos < n; pos += blockSize)
        {
            gate.setParams (p); // the chain pushes parameters every block
            gate.process (buf.block (pos, std::min (blockSize, n - pos)));
        }
        outs.push_back (std::move (buf));
    }

    // The gate actually did something (so the comparison is meaningful) ...
    CHECK_LE (gainDb (ref.ch[0], outs[0].ch[0], 512, seconds (0.15), seconds (0.3)), -6.0);
    // ... and every block size gives bit-identical output.
    for (size_t k = 1; k < outs.size(); ++k)
        for (int c = 0; c < 2; ++c)
            CHECK_NEAR (maxAbsDiff (outs[k].ch[static_cast<size_t> (c)].data(), outs[0].ch[static_cast<size_t> (c)].data(), n), 0.0, 0.0);
}
