// Tests for the BS.1770-4 / EBU R128 loudness meter and the LoudnessFollower:
// K-weighting coefficients and response, EBU Tech 3341 / 3342 compliance cases
// (generated synthetically as stereo 1 kHz tones), channel weighting, window
// timing, gating, resets, real-time safety, robustness and block-size
// invariance.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/analysis/LoudnessFollower.h"
#include "flub/analysis/LoudnessMeter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

using namespace flub;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;
constexpr unsigned kStereo = 0x3u;

float dbfs (double db) { return static_cast<float> (std::pow (10.0, db / 20.0)); }

/** Copies into a Planar channel in place (move-assigning a new vector would
    leave the Planar's channel pointers dangling). */
void copyInto (std::vector<float>& dst, const std::vector<float>& src)
{
    std::copy (src.begin(), src.begin() + static_cast<std::ptrdiff_t> (std::min (src.size(), dst.size())), dst.begin());
}

/** Phase-continuous 1 kHz sine with piecewise-constant per-channel levels, fed
    to a meter in fixed blocks. EBU Tech 3341/3342 test signals are 1 kHz tones
    with abrupt level changes; one period table keeps 100+ s of audio cheap. */
class ToneSource
{
public:
    ToneSource (double sampleRate, int numChannels, int blockSize = 512)
        : fs (sampleRate), buf (numChannels, blockSize), block (blockSize)
    {
        const int ifs = static_cast<int> (sampleRate);
        const int period = ifs / std::gcd (ifs, 1000);
        table.resize (static_cast<size_t> (period));
        for (int i = 0; i < period; ++i)
            table[static_cast<size_t> (i)] = static_cast<float> (std::sin (kTwoPi * 1000.0 * i / sampleRate));
    }

    /** Same level (dBFS peak) in every channel whose bit is set in `mask`. */
    template <typename Meter>
    void feed (Meter& m, double levelDb, double seconds, unsigned mask = kStereo)
    {
        std::array<float, kMaxChannels> g {};
        for (int c = 0; c < kMaxChannels; ++c)
            g[static_cast<size_t> (c)] = ((mask >> c) & 1u) != 0 ? dbfs (levelDb) : 0.0f;
        feedGains (m, g, seconds);
    }

    template <typename Meter>
    void feedGains (Meter& m, const std::array<float, kMaxChannels>& gains, double seconds)
    {
        feedSamples (m, gains, static_cast<long long> (std::llround (seconds * fs)));
    }

    template <typename Meter>
    void feedSamples (Meter& m, const std::array<float, kMaxChannels>& gains, long long numSamples)
    {
        const int period = static_cast<int> (table.size());
        while (numSamples > 0)
        {
            const int n = static_cast<int> (std::min<long long> (numSamples, block));
            for (int c = 0; c < buf.numChannels(); ++c)
            {
                float* d = buf.ch[static_cast<size_t> (c)].data();
                const float g = gains[static_cast<size_t> (c)];
                int p = phase;
                for (int i = 0; i < n; ++i)
                {
                    d[i] = g * table[static_cast<size_t> (p)];
                    if (++p == period)
                        p = 0;
                }
            }
            phase = (phase + n) % period;
            m.process (buf.block (0, n));
            numSamples -= n;
        }
    }

private:
    double fs;
    Planar buf;
    int block;
    std::vector<float> table;
    int phase = 0;
};

struct Readings
{
    float m, s, i, lra, maxM, maxS;
};

Readings snapshot (const LoudnessMeter& m)
{
    return { m.getMomentaryLufs(), m.getShortTermLufs(), m.getIntegratedLufs(),
             m.getLoudnessRangeLu(), m.getMaxMomentaryLufs(), m.getMaxShortTermLufs() };
}

bool allFinite (const Readings& r)
{
    return std::isfinite (r.m) && std::isfinite (r.s) && std::isfinite (r.i) && std::isfinite (r.lra)
           && std::isfinite (r.maxM) && std::isfinite (r.maxS);
}

double magnitudeDb (const BiquadCoeffs& a, const BiquadCoeffs& b, double freq, double fs)
{
    return 20.0 * std::log10 (std::abs (a.response (freq, fs) * b.response (freq, fs)));
}

/** Integrated loudness of a 1 kHz tone in the channels of `mask` (6 s). */
float toneLoudness (int numChannels, unsigned mask, double levelDb = -20.0)
{
    LoudnessMeter m;
    m.prepare (kFs, numChannels);
    ToneSource src (kFs, numChannels);
    src.feed (m, levelDb, 6.0, mask);
    return m.getIntegratedLufs();
}

/** Processes samples [from, to) of `buf` in blocks of `blockSize`. */
void processRange (LoudnessMeter& m, Planar& buf, int from, int to, int blockSize)
{
    for (int pos = from; pos < to; pos += blockSize)
        m.process (buf.block (pos, std::min (blockSize, to - pos)));
}
} // namespace

//==============================================================================
TEST_CASE ("LoudnessMeter: K-weighting at 48 kHz equals the BS.1770 table")
{
    const auto s1 = LoudnessMeter::kWeightingStage1 (48000.0);
    CHECK_NEAR (s1.b0, 1.53512485958697, 1e-9);
    CHECK_NEAR (s1.b1, -2.69169618940638, 1e-9);
    CHECK_NEAR (s1.b2, 1.19839281085285, 1e-9);
    CHECK_NEAR (s1.a1, -1.69065929318241, 1e-9);
    CHECK_NEAR (s1.a2, 0.73248077421585, 1e-9);

    const auto s2 = LoudnessMeter::kWeightingStage2 (48000.0);
    CHECK_NEAR (s2.b0, 1.0, 1e-12);
    CHECK_NEAR (s2.b1, -2.0, 1e-12);
    CHECK_NEAR (s2.b2, 1.0, 1e-12);
    CHECK_NEAR (s2.a1, -1.99004745483398, 1e-9);
    CHECK_NEAR (s2.a2, 0.99007225036621, 1e-9);
}

TEST_CASE ("LoudnessMeter: K-weighting response shape and consistency across sample rates")
{
    const auto s1 = LoudnessMeter::kWeightingStage1 (kFs);
    const auto s2 = LoudnessMeter::kWeightingStage2 (kFs);
    // The -0.691 dB offset exists to cancel exactly this gain at 997 Hz.
    CHECK_NEAR (magnitudeDb (s1, s2, 997.0, kFs), 0.691, 0.001);
    // +4 dB head shelf at high frequencies, RLB roll-off at low frequencies.
    CHECK_NEAR (magnitudeDb (s1, s2, 10000.0, kFs), 4.04, 0.05);
    CHECK_NEAR (magnitudeDb (s1, s2, 20.0, kFs), -13.3, 0.1);
    CHECK_NEAR (magnitudeDb (s1, s2, 100.0, kFs), -1.13, 0.05);

    // Designed from the analog prototype, so every rate gives the same curve
    // (the residual ~0.03 dB at 192 kHz is the unnormalised [1 -2 1] RLB
    // numerator that BS.1770 / libebur128 use at every rate).
    for (double fs : { 44100.0, 88200.0, 96000.0, 192000.0 })
    {
        const auto r1 = LoudnessMeter::kWeightingStage1 (fs);
        const auto r2 = LoudnessMeter::kWeightingStage2 (fs);
        for (double f : { 20.0, 50.0, 100.0, 500.0, 997.0, 2000.0, 5000.0, 10000.0, 16000.0 })
            CHECK_NEAR (magnitudeDb (r1, r2, f, fs), magnitudeDb (s1, s2, f, kFs), 0.05);
    }
}

TEST_CASE ("LoudnessMeter: 0 dBFS 997 Hz sine in one channel reads -3.01 LUFS")
{
    LoudnessMeter m;
    m.prepare (kFs, 1);
    const int n = static_cast<int> (kFs * 2.0);
    Planar buf (1, n);
    copyInto (buf.ch[0], sine (997.0, kFs, n, 1.0f));
    processRange (m, buf, 0, n, 480);
    CHECK_NEAR (m.getMomentaryLufs(), -3.01, 0.01);
    CHECK_NEAR (m.getIntegratedLufs(), -3.01, 0.01);
}

//==============================================================================
// EBU Tech 3341 minimum requirements (stereo 1 kHz sine, dBFS per channel).
TEST_CASE ("LoudnessMeter: EBU Tech 3341 case 1 (-23 dBFS, 20 s) at 44.1 / 48 / 96 kHz")
{
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        LoudnessMeter m;
        m.prepare (fs, 2);
        ToneSource src (fs, 2);
        src.feed (m, -23.0, 20.0);
        CHECK_NEAR (m.getMomentaryLufs(), -23.0, 0.1);
        CHECK_NEAR (m.getShortTermLufs(), -23.0, 0.1);
        CHECK_NEAR (m.getIntegratedLufs(), -23.0, 0.1);
        CHECK_NEAR (m.getMaxMomentaryLufs(), -23.0, 0.1);
        CHECK_NEAR (m.getMaxShortTermLufs(), -23.0, 0.1);
        CHECK_LE (m.getLoudnessRangeLu(), 0.1); // steady tone
    }
}

TEST_CASE ("LoudnessMeter: EBU Tech 3341 case 2 (-33 dBFS, 20 s)")
{
    LoudnessMeter m;
    m.prepare (kFs, 2);
    ToneSource src (kFs, 2);
    src.feed (m, -33.0, 20.0);
    CHECK_NEAR (m.getMomentaryLufs(), -33.0, 0.1);
    CHECK_NEAR (m.getShortTermLufs(), -33.0, 0.1);
    CHECK_NEAR (m.getIntegratedLufs(), -33.0, 0.1);
}

TEST_CASE ("LoudnessMeter: EBU Tech 3341 case 3 (-36 / -23 / -36 dBFS, relative gate)")
{
    LoudnessMeter m;
    m.prepare (kFs, 2);
    ToneSource src (kFs, 2);
    src.feed (m, -36.0, 10.0);
    src.feed (m, -23.0, 60.0);
    src.feed (m, -36.0, 10.0);
    CHECK_NEAR (m.getIntegratedLufs(), -23.0, 0.1);
}

TEST_CASE ("LoudnessMeter: EBU Tech 3341 case 4 (-72 / -36 / -23 / -36 / -72 dBFS, both gates)")
{
    LoudnessMeter m;
    m.prepare (kFs, 2);
    ToneSource src (kFs, 2);
    src.feed (m, -72.0, 10.0);
    src.feed (m, -36.0, 10.0);
    src.feed (m, -23.0, 60.0);
    src.feed (m, -36.0, 10.0);
    src.feed (m, -72.0, 10.0);
    CHECK_NEAR (m.getIntegratedLufs(), -23.0, 0.1);
}

TEST_CASE ("LoudnessMeter: EBU Tech 3341 case 5 (-26 / -20 / -26 dBFS, 20 / 20.1 / 20 s)")
{
    LoudnessMeter m;
    m.prepare (kFs, 2);
    ToneSource src (kFs, 2);
    src.feed (m, -26.0, 20.0);
    src.feed (m, -20.0, 20.1);
    src.feed (m, -26.0, 20.0);
    CHECK_NEAR (m.getIntegratedLufs(), -23.0, 0.1);
}

TEST_CASE ("LoudnessMeter: EBU Tech 3341 case 6 (5.1: L/R -28, C -24, Ls/Rs -30 dBFS)")
{
    LoudnessMeter m;
    m.prepare (kFs, 6);
    ToneSource src (kFs, 6);
    const std::array<float, kMaxChannels> g { dbfs (-28.0), dbfs (-28.0), dbfs (-24.0), 0.0f,
                                              dbfs (-30.0), dbfs (-30.0), 0.0f, 0.0f };
    src.feedGains (m, g, 20.0);
    CHECK_NEAR (m.getIntegratedLufs(), -23.0, 0.1);
}

//==============================================================================
// EBU Tech 3342 loudness range (stereo 1 kHz sine).
TEST_CASE ("LoudnessMeter: EBU Tech 3342 LRA cases")
{
    struct Case
    {
        std::vector<double> levels;
        double segmentSeconds;
        double expectedLra;
    };
    const Case cases[] = {
        { { -20.0, -30.0 }, 20.0, 10.0 },
        { { -20.0, -15.0 }, 20.0, 5.0 },
        { { -40.0, -20.0 }, 20.0, 20.0 },
        { { -50.0, -35.0, -20.0, -35.0, -50.0 }, 20.0, 15.0 }, // Tech 3342 case 4: -50 is relative-gated
    };
    for (const auto& c : cases)
    {
        LoudnessMeter m;
        m.prepare (kFs, 2);
        ToneSource src (kFs, 2);
        for (double level : c.levels)
            src.feed (m, level, c.segmentSeconds);
        CHECK_NEAR (m.getLoudnessRangeLu(), c.expectedLra, 1.0);
        // The histogram keeps exact bin means, so steady segments land exactly.
        CHECK_NEAR (m.getLoudnessRangeLu(), c.expectedLra, 0.1);
    }
}

//==============================================================================
TEST_CASE ("LoudnessMeter: 5.1 / 7.1 channel weighting (LFE excluded, surrounds +1.5 dB)")
{
    // Tone only in the LFE: contributes nothing.
    {
        LoudnessMeter m;
        m.prepare (kFs, 6);
        ToneSource src (kFs, 6);
        src.feed (m, -10.0, 4.0, 1u << 3);
        CHECK (m.getMomentaryLufs() == kMinusInfDb);
        CHECK (m.getShortTermLufs() == kMinusInfDb);
        CHECK (m.getIntegratedLufs() == kMinusInfDb);
        CHECK (m.getLoudnessRangeLu() == 0.0f);
    }

    const float left = toneLoudness (6, 1u << 0);
    CHECK_NEAR (left, -23.01, 0.05); // -20 dBFS sine, one channel: -20 - 3.01
    const double surroundDb = 10.0 * std::log10 (1.41);
    CHECK_NEAR (toneLoudness (6, 1u << 1) - left, 0.0, 0.001);
    CHECK_NEAR (toneLoudness (6, 1u << 2) - left, 0.0, 0.001);
    CHECK_NEAR (toneLoudness (6, 1u << 4) - left, surroundDb, 0.01);
    CHECK_NEAR (toneLoudness (6, 1u << 5) - left, surroundDb, 0.01);
    CHECK_NEAR (toneLoudness (6, 1u << 4) - left, 1.5, 0.05);

    // 7.1: channels 4..7 are all surrounds; LFE still excluded.
    CHECK_NEAR (toneLoudness (8, 1u << 6) - left, surroundDb, 0.01);
    CHECK_NEAR (toneLoudness (8, 1u << 7) - left, surroundDb, 0.01);
    CHECK (toneLoudness (8, 1u << 3) == kMinusInfDb);

    // Below 6 channels there is no LFE/surround interpretation.
    CHECK_NEAR (toneLoudness (5, 1u << 3) - left, 0.0, 0.001);
    CHECK_NEAR (toneLoudness (5, 1u << 4) - left, 0.0, 0.001);

    // Power summation across channels: identical tones in L and R = +3.01 dB.
    CHECK_NEAR (toneLoudness (2, kStereo) - left, 3.0103, 0.01);
    CHECK_NEAR (toneLoudness (1, 1u), left, 0.001);
}

//==============================================================================
TEST_CASE ("LoudnessMeter: window timing - readings appear exactly when their window is full")
{
    // Momentary needs 400 ms (19200 samples at 48 kHz), short-term 3 s.
    LoudnessMeter m;
    m.prepare (kFs, 2);
    ToneSource src (kFs, 2, 64);
    const std::array<float, kMaxChannels> g { dbfs (-23.0), dbfs (-23.0) };
    src.feedSamples (m, g, 19199);
    CHECK (m.getMomentaryLufs() == kMinusInfDb);
    CHECK (m.getIntegratedLufs() == kMinusInfDb);
    CHECK (m.getMaxMomentaryLufs() == kMinusInfDb);
    src.feedSamples (m, g, 1);
    CHECK_NEAR (m.getMomentaryLufs(), -23.0, 0.1);
    CHECK_NEAR (m.getIntegratedLufs(), -23.0, 0.1);
    CHECK (m.getShortTermLufs() == kMinusInfDb);

    src.feedSamples (m, g, 144000 - 19200 - 1);
    CHECK (m.getShortTermLufs() == kMinusInfDb);
    CHECK (m.getMaxShortTermLufs() == kMinusInfDb);
    src.feedSamples (m, g, 1);
    CHECK_NEAR (m.getShortTermLufs(), -23.0, 0.1);
    CHECK_NEAR (m.getMaxShortTermLufs(), -23.0, 0.1);
}

TEST_CASE ("LoudnessMeter: momentary window is a rectangular 400 ms window on a 100 ms grid")
{
    // Silence for 3 s, then a -23 dBFS tone: at +100/+200/+400 ms the window
    // holds 1/4, 2/4 and 4/4 of tone -> -23 + 10 log10 (fraction).
    LoudnessMeter m;
    m.prepare (kFs, 2);
    ToneSource src (kFs, 2);
    src.feed (m, -200.0, 3.0);
    CHECK (m.getMomentaryLufs() == kMinusInfDb);
    src.feed (m, -23.0, 0.1);
    CHECK_NEAR (m.getMomentaryLufs(), -23.0 + 10.0 * std::log10 (0.25), 0.1);
    src.feed (m, -23.0, 0.1);
    CHECK_NEAR (m.getMomentaryLufs(), -23.0 + 10.0 * std::log10 (0.5), 0.1);
    src.feed (m, -23.0, 0.2);
    CHECK_NEAR (m.getMomentaryLufs(), -23.0, 0.1);
    // Short-term: 0.4 s of tone in a 3 s window.
    CHECK_NEAR (m.getShortTermLufs(), -23.0 + 10.0 * std::log10 (0.4 / 3.0), 0.1);
}

TEST_CASE ("LoudnessMeter: absolute gate - silence does not pull integrated loudness down")
{
    LoudnessMeter m;
    m.prepare (kFs, 2);
    ToneSource src (kFs, 2);
    src.feed (m, -23.0, 10.0);
    src.feed (m, -300.0, 10.0);
    CHECK (m.getMomentaryLufs() == kMinusInfDb);
    CHECK (m.getShortTermLufs() == kMinusInfDb);
    CHECK_NEAR (m.getIntegratedLufs(), -23.0, 0.1);
    CHECK_NEAR (m.getMaxMomentaryLufs(), -23.0, 0.1);

    // Without the -70 LUFS gate, a long near-silent tail would drag the
    // relative threshold down (-34 LUFS mean -> -44 gate) until the -37 LUFS
    // passage counted and I fell to ~-25.8. With it, -37 stays relative-gated
    // and I is the mean of the 47 full -23 blocks plus the 3 gating blocks
    // that straddle the step (3/4, 2/4, 1/4 of -23, rest -37): -23.13.
    LoudnessMeter g;
    g.prepare (kFs, 2);
    ToneSource s2 (kFs, 2);
    s2.feed (g, -23.0, 5.0);
    s2.feed (g, -37.0, 5.0);
    s2.feed (g, -80.0, 60.0);
    const double loud = std::pow (10.0, -2.3), quiet = std::pow (10.0, -3.7);
    const double expected = 10.0 * std::log10 ((48.5 * loud + 1.5 * quiet) / 50.0);
    CHECK_NEAR (g.getIntegratedLufs(), expected, 0.02);
}

TEST_CASE ("LoudnessMeter: max momentary / short-term hold the loudest window")
{
    LoudnessMeter m;
    m.prepare (kFs, 2);
    ToneSource src (kFs, 2);
    src.feed (m, -30.0, 5.0);
    src.feed (m, -15.0, 1.0);
    src.feed (m, -30.0, 5.0);
    CHECK_NEAR (m.getMomentaryLufs(), -30.0, 0.1);
    CHECK_NEAR (m.getMaxMomentaryLufs(), -15.0, 0.1);
    // Loudest 3 s window: 1 s at -15 + 2 s at -30.
    const double e = (std::pow (10.0, -1.5) + 2.0 * std::pow (10.0, -3.0)) / 3.0;
    CHECK_NEAR (m.getMaxShortTermLufs(), 10.0 * std::log10 (e), 0.1);
}

TEST_CASE ("LoudnessMeter: resetIntegrated starts a new measurement, momentary keeps running")
{
    LoudnessMeter m;
    m.prepare (kFs, 2);
    ToneSource src (kFs, 2);
    src.feed (m, -20.0, 10.0);
    CHECK_NEAR (m.getIntegratedLufs(), -20.0, 0.1);

    m.resetIntegrated();
    CHECK (m.getIntegratedLufs() == kMinusInfDb);
    CHECK (m.getMaxMomentaryLufs() == kMinusInfDb);
    CHECK (m.getMaxShortTermLufs() == kMinusInfDb);
    CHECK (m.getLoudnessRangeLu() == 0.0f);
    CHECK_NEAR (m.getMomentaryLufs(), -20.0, 0.1); // live readings are untouched
    CHECK_NEAR (m.getShortTermLufs(), -20.0, 0.1);

    src.feed (m, -30.0, 10.0);
    CHECK_NEAR (m.getIntegratedLufs(), -30.0, 0.1);
    CHECK_NEAR (m.getMaxMomentaryLufs(), -30.0, 0.1); // the -20 windows are not part of it
    CHECK_NEAR (m.getMaxShortTermLufs(), -30.0, 0.1);
    CHECK_LE (m.getLoudnessRangeLu(), 0.1);

    // A reset in the middle of a sub-block: the partly filled sub-block holds
    // pre-reset audio, so the first gating block is [5.1 s, 5.5 s).
    LoudnessMeter r;
    r.prepare (kFs, 2);
    ToneSource s2 (kFs, 2, 480);
    s2.feed (r, -20.0, 5.05);
    r.resetIntegrated();
    s2.feed (r, -30.0, 0.35);
    CHECK (r.getIntegratedLufs() == kMinusInfDb);
    s2.feed (r, -30.0, 0.1);
    CHECK_NEAR (r.getIntegratedLufs(), -30.0, 0.05);
    CHECK_NEAR (r.getMaxMomentaryLufs(), -30.0, 0.05);
}

TEST_CASE ("LoudnessMeter: reset clears everything")
{
    LoudnessMeter m;
    m.prepare (kFs, 2);
    ToneSource src (kFs, 2);
    src.feed (m, -20.0, 4.0);
    m.reset();
    const auto r = snapshot (m);
    CHECK (r.m == kMinusInfDb);
    CHECK (r.s == kMinusInfDb);
    CHECK (r.i == kMinusInfDb);
    CHECK (r.lra == 0.0f);
    CHECK (r.maxM == kMinusInfDb);
    CHECK (r.maxS == kMinusInfDb);
    src.feed (m, -25.0, 4.0);
    CHECK_NEAR (m.getIntegratedLufs(), -25.0, 0.1);
    CHECK_NEAR (m.getMaxMomentaryLufs(), -25.0, 0.1);
}

TEST_CASE ("LoudnessMeter: process() only reads the block")
{
    LoudnessMeter m;
    m.prepare (kFs, 2);
    Planar buf (2, 1000);
    copyInto (buf.ch[0], whiteNoise (1000, 0.7f, 11));
    copyInto (buf.ch[1], whiteNoise (1000, 0.7f, 12));
    const auto copy = buf.ch;
    for (int i = 0; i < 10; ++i)
        m.process (buf.block());
    for (int c = 0; c < 2; ++c)
        CHECK (std::memcmp (copy[static_cast<size_t> (c)].data(), buf.ch[static_cast<size_t> (c)].data(), sizeof (float) * 1000) == 0);
}

//==============================================================================
TEST_CASE ("LoudnessMeter: process, reset and getters do not allocate")
{
    for (int channels : { 2, 6 })
    {
        LoudnessMeter m;
        m.prepare (kFs, channels);
        LoudnessFollower f;
        f.prepare (kFs, channels);
        Planar buf (channels, 512);
        for (int c = 0; c < channels; ++c)
            copyInto (buf.ch[static_cast<size_t> (c)], whiteNoise (512, 0.3f, static_cast<uint32_t> (100 + c)));
        float sink = 0.0f;

        AllocationGuard guard;
        m.reset();
        f.reset();
        for (int b = 0; b < 400; ++b) // > 4 s: exercises the gating and LRA histograms
        {
            if (b == 200)
                m.resetIntegrated();
            m.process (buf.block());
            f.process (buf.block());
            sink += m.getMomentaryLufs() + m.getShortTermLufs() + m.getIntegratedLufs() + m.getLoudnessRangeLu()
                    + m.getMaxMomentaryLufs() + m.getMaxShortTermLufs() + f.getLufs();
        }
        m.reset();
        CHECK (guard.allocations() == 0);
        CHECK (std::isfinite (sink));
    }
}

TEST_CASE ("LoudnessMeter: robustness - silence, DC, full-scale noise, impulses at all rates")
{
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const int n = static_cast<int> (fs * 4.0);
        for (int kind = 0; kind < 5; ++kind)
        {
            Planar buf (2, n);
            for (int c = 0; c < 2; ++c)
            {
                auto& d = buf.ch[static_cast<size_t> (c)];
                if (kind == 1)
                    std::fill (d.begin(), d.end(), c == 0 ? 1.0f : -1.0f); // full-scale DC
                else if (kind == 2)
                    copyInto (d, whiteNoise (n, 1.0f, static_cast<uint32_t> (7 + c))); // full-scale noise
                else if (kind == 3)
                    for (int i = 0; i < n; i += 4800)
                        d[static_cast<size_t> (i)] = 1.0f; // isolated single-sample impulses
                else if (kind == 4)
                    copyInto (d, whiteNoise (n, 1.0e30f, static_cast<uint32_t> (9 + c))); // absurd float overs
            }
            LoudnessMeter m;
            m.prepare (fs, 2);
            processRange (m, buf, 0, n, 1024);
            const auto r = snapshot (m);
            CHECK (allFinite (r));
            for (float v : { r.m, r.s, r.i, r.maxM, r.maxS })
            {
                CHECK_GE (v, kMinusInfDb);
                CHECK_LE (v, 700.0f);
            }
            CHECK_GE (r.lra, 0.0f);
            CHECK_LE (r.lra, 100.0f);

            if (kind == 0) // silence
            {
                CHECK (r.m == kMinusInfDb);
                CHECK (r.s == kMinusInfDb);
                CHECK (r.i == kMinusInfDb);
                CHECK (r.lra == 0.0f);
            }
            else if (kind == 1) // DC is removed by the RLB high-pass
            {
                CHECK_LE (r.m, -100.0f);
            }
            else if (kind == 2) // K-weighted full-scale white noise in stereo ~ +1 LUFS
            {
                CHECK_GE (r.m, -5.0f);
                CHECK_LE (r.m, 6.0f);
                CHECK_LE (r.lra, 1.0f);
            }
            else if (kind == 3)
            {
                CHECK_LE (r.m, -20.0f);
                CHECK_GE (r.m, -60.0f);
            }
        }
    }
}

TEST_CASE ("LoudnessMeter: NaN / Inf input cannot poison the meter")
{
    LoudnessMeter m;
    m.prepare (kFs, 2);
    ToneSource src (kFs, 2);
    src.feed (m, -23.0, 2.0);
    Planar bad (2, 64);
    bad.ch[0][10] = std::numeric_limits<float>::quiet_NaN();
    bad.ch[1][20] = std::numeric_limits<float>::infinity();
    m.process (bad.block());
    // Every reading stays finite at every 100 ms update through the recovery.
    bool finite = true;
    for (int i = 0; i < 40; ++i)
    {
        src.feed (m, -23.0, 0.1);
        finite = finite && allFinite (snapshot (m));
    }
    CHECK (finite);
    const auto r = snapshot (m);
    CHECK (allFinite (r));
    CHECK_NEAR (r.m, -23.0, 0.1);
    CHECK_NEAR (r.s, -23.0, 0.2);
    CHECK_NEAR (r.i, -23.0, 0.2);
}

TEST_CASE ("LoudnessMeter: extreme configuration values are clamped safely")
{
    // Out-of-range channel counts / sample rates, more block channels than prepared.
    const std::pair<double, int> configs[] = { { 0.0, 0 }, { -44100.0, -3 }, { 8000.0, 100 }, { 384000.0, 1 }, { 22050.0, 3 } };
    for (const auto& [fs, ch] : configs)
    {
        LoudnessMeter m;
        m.prepare (fs, ch);
        Planar buf (kMaxChannels, 997);
        for (int c = 0; c < kMaxChannels; ++c)
            copyInto (buf.ch[static_cast<size_t> (c)], whiteNoise (997, 0.5f, static_cast<uint32_t> (3 + c)));
        for (int b = 0; b < 400; ++b)
            m.process (buf.block());
        const auto r = snapshot (m);
        CHECK (allFinite (r));
        CHECK_GE (r.m, -40.0f);
        CHECK_LE (r.m, 20.0f);
    }

    // An unprepared meter ignores audio instead of crashing.
    LoudnessMeter unprepared;
    Planar buf (2, 256);
    unprepared.process (buf.block());
    CHECK (unprepared.getIntegratedLufs() == kMinusInfDb);

    // Zero-length blocks are harmless.
    LoudnessMeter m;
    m.prepare (kFs, 2);
    m.process (buf.block (0, 0));
    CHECK (m.getMomentaryLufs() == kMinusInfDb);
}

TEST_CASE ("LoudnessMeter: 192 kHz still meets Tech 3341 case 1")
{
    LoudnessMeter m;
    m.prepare (192000.0, 2);
    ToneSource src (192000.0, 2, 4096);
    src.feed (m, -23.0, 5.0);
    CHECK_NEAR (m.getMomentaryLufs(), -23.0, 0.1);
    CHECK_NEAR (m.getShortTermLufs(), -23.0, 0.1);
    CHECK_NEAR (m.getIntegratedLufs(), -23.0, 0.1);
}

//==============================================================================
TEST_CASE ("LoudnessMeter: readings are independent of the host block size")
{
    // Level-modulated, decorrelated stereo noise (so the gates and LRA have
    // something to do), with a resetIntegrated() at a sample position that is
    // not on the 100 ms grid.
    const int n = static_cast<int> (kFs * 9.0);
    Planar buf (2, n);
    for (int c = 0; c < 2; ++c)
    {
        auto noise = whiteNoise (n, 1.0f, static_cast<uint32_t> (21 + c));
        for (int i = 0; i < n; ++i)
        {
            const double t = i / kFs;
            const double db = -32.0 + 14.0 * std::sin (kTwoPi * 0.27 * t + c) + (t > 6.0 ? 8.0 : 0.0);
            buf.ch[static_cast<size_t> (c)][static_cast<size_t> (i)] = noise[static_cast<size_t> (i)] * dbfs (db);
        }
    }

    const int resetAt = 112567;
    std::vector<int> checkpoints;
    for (int p = 12001; p < n; p += 12001)
        checkpoints.push_back (p);
    checkpoints.push_back (resetAt);
    checkpoints.push_back (n);
    std::sort (checkpoints.begin(), checkpoints.end());

    std::vector<std::vector<Readings>> results;
    for (int blockSize : { 1, 7, 64, 512 })
    {
        LoudnessMeter m;
        m.prepare (kFs, 2);
        std::vector<Readings> snaps;
        int pos = 0;
        for (int cp : checkpoints)
        {
            processRange (m, buf, pos, cp, blockSize);
            pos = cp;
            snaps.push_back (snapshot (m));
            if (cp == resetAt)
                m.resetIntegrated();
        }
        results.push_back (snaps);
    }

    const auto& ref = results[0];
    CHECK (ref.back().lra > 1.0f);  // the signal really exercises the LRA
    CHECK (ref.back().i > -60.0f);
    for (size_t k = 1; k < results.size(); ++k)
        for (size_t j = 0; j < ref.size(); ++j)
        {
            const auto& a = ref[j];
            const auto& b = results[k][j];
            CHECK_NEAR (b.m, a.m, 1e-5);
            CHECK_NEAR (b.s, a.s, 1e-5);
            CHECK_NEAR (b.i, a.i, 1e-5);
            CHECK_NEAR (b.lra, a.lra, 1e-5);
            CHECK_NEAR (b.maxM, a.maxM, 1e-5);
            CHECK_NEAR (b.maxS, a.maxS, 1e-5);
        }
}

TEST_CASE ("LoudnessMeter: latency is zero - readings refer to the audio already processed")
{
    // The meter is analysis-only (no latencySamples()): a tone is fully
    // reflected as soon as the window covering it has been processed.
    LoudnessMeter m;
    m.prepare (kFs, 2);
    ToneSource src (kFs, 2, 1);
    const std::array<float, kMaxChannels> quiet {};
    const std::array<float, kMaxChannels> g { dbfs (-40.0), dbfs (-40.0) };
    src.feedSamples (m, quiet, 4800 * 6);
    src.feedSamples (m, g, 4800 * 4);
    CHECK_NEAR (m.getMomentaryLufs(), -40.0, 0.1);
}

//==============================================================================
TEST_CASE ("LoudnessFollower: converges to the programme loudness and gates silence")
{
    LoudnessFollower f;
    f.prepare (kFs, 2);
    CHECK (! f.isActive());
    ToneSource src (kFs, 2);
    src.feed (f, -23.0, 20.0);
    CHECK_NEAR (f.getLufs(), -23.0, 0.2);
    CHECK (f.isActive());
    src.feed (f, -300.0, 30.0);
    CHECK (! f.isActive());
    CHECK (std::isfinite (f.getLufs()));

    // Same K-weighting and channel weights as the meter: a surround tone reads +1.5 dB.
    LoudnessFollower l, s;
    l.prepare (kFs, 6, 1000.0f);
    s.prepare (kFs, 6, 1000.0f);
    ToneSource a (kFs, 6), b (kFs, 6);
    a.feed (l, -20.0, 8.0, 1u << 0);
    b.feed (s, -20.0, 8.0, 1u << 4);
    CHECK_NEAR (l.getLufs(), -23.01, 0.1);
    CHECK_NEAR (s.getLufs() - l.getLufs(), 10.0 * std::log10 (1.41), 0.02);
}

//==============================================================================
// ---- adversarial review tests ----
namespace
{
/** Brute-force BS.1770-4 / EBU Tech 3342 reference: keeps every gating block
    and every short-term value and gates / sorts them exactly (no histogram). */
struct ReferenceResult
{
    double integrated, lra, maxM, maxS, m, s;
};

ReferenceResult referenceLoudness (const Planar& x, double fs)
{
    const auto c1 = LoudnessMeter::kWeightingStage1 (fs);
    const auto c2 = LoudnessMeter::kWeightingStage2 (fs);
    const int nch = x.numChannels();
    const int n = x.numSamples();
    const int len = static_cast<int> (std::lround (0.1 * fs));

    std::vector<double> sub;
    std::vector<BiquadState> s1 (static_cast<size_t> (nch)), s2 (static_cast<size_t> (nch));
    std::vector<double> acc (static_cast<size_t> (nch), 0.0);
    for (int i = 0; i < n; ++i)
    {
        for (size_t c = 0; c < static_cast<size_t> (nch); ++c)
        {
            const double y = biquadTick (c2, s2[c], biquadTick (c1, s1[c], static_cast<double> (x.ch[c][static_cast<size_t> (i)])));
            acc[c] += y * y;
        }
        if ((i + 1) % len == 0)
        {
            sub.push_back (std::accumulate (acc.begin(), acc.end(), 0.0));
            std::fill (acc.begin(), acc.end(), 0.0);
        }
    }

    const auto lufs = [] (double e) { return -0.691 + 10.0 * std::log10 (e); };
    const auto window = [&] (size_t end, int count)
    {
        double e = 0.0;
        for (int k = 0; k < count; ++k)
            e += sub[end - static_cast<size_t> (k)];
        return e / (count * static_cast<double> (len));
    };
    std::vector<double> blocks, shortTerm;
    for (size_t j = 3; j < sub.size(); ++j)
        blocks.push_back (window (j, 4));
    for (size_t j = 29; j < sub.size(); ++j)
        shortTerm.push_back (window (j, 30));

    // Absolute gate, then the relative gate `relLu` below the absolute-gated power mean.
    const auto gated = [&] (const std::vector<double>& v, double relLu)
    {
        double sum = 0.0;
        int count = 0;
        for (double e : v)
            if (e > 0.0 && lufs (e) > -70.0)
            {
                sum += e;
                ++count;
            }
        std::vector<double> out;
        if (count == 0)
            return out;
        const double thr = sum / count * std::pow (10.0, relLu / 10.0);
        for (double e : v)
            if (e > 0.0 && lufs (e) > -70.0 && e > thr)
                out.push_back (e);
        return out;
    };

    ReferenceResult r {};
    const auto gi = gated (blocks, -10.0);
    r.integrated = lufs (std::accumulate (gi.begin(), gi.end(), 0.0) / static_cast<double> (gi.size()));
    auto gl = gated (shortTerm, -20.0);
    std::sort (gl.begin(), gl.end());
    const double last = static_cast<double> (gl.size() - 1);
    r.lra = lufs (gl[static_cast<size_t> (std::llround (last * 0.95))]) - lufs (gl[static_cast<size_t> (std::llround (last * 0.10))]);
    r.maxM = lufs (*std::max_element (blocks.begin(), blocks.end()));
    r.maxS = lufs (*std::max_element (shortTerm.begin(), shortTerm.end()));
    r.m = lufs (blocks.back());
    r.s = lufs (shortTerm.back());
    return r;
}

/** Noise with random piecewise-constant levels (-75..-5 dBFS, 50 ms..4 s segments). */
Planar randomProgramme (double fs, int numChannels, double seconds, uint32_t seed)
{
    const int n = static_cast<int> (seconds * fs);
    Planar buf (numChannels, n);
    FastRandom rng (seed);
    int i = 0;
    while (i < n)
    {
        const float g = dbfs (-40.0 + 35.0 * rng.nextBipolar());
        const int len = std::min (n - i, static_cast<int> ((2.025 + 1.975 * rng.nextBipolar()) * fs));
        for (int c = 0; c < numChannels; ++c)
            for (int k = 0; k < len; ++k)
                buf.ch[static_cast<size_t> (c)][static_cast<size_t> (i + k)] = g * rng.nextBipolar();
        i += len;
    }
    return buf;
}
} // namespace

TEST_CASE ("LoudnessMeter (adversarial): histogram gating matches a brute-force BS.1770 / Tech 3342 reference")
{
    // Random programmes with many level steps exercise both gates, the bin
    // that straddles the relative gate and the percentile ranks. Integrated
    // must be exact in practice; LRA may deviate by the documented 0.1 LU bin
    // resolution; max / live readings are plain window sums and must agree.
    const double rates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };
    const int blockSizes[] = { 4096, 1, 333, 1024 };
    for (int k = 0; k < 4; ++k)
    {
        const double fs = rates[k];
        const int numChannels = 1 + k % 2;
        const auto buf = randomProgramme (fs, numChannels, fs >= 96000.0 ? 20.0 : 40.0, static_cast<uint32_t> (77 + k));
        auto& mutableBuf = const_cast<Planar&> (buf);
        LoudnessMeter m;
        m.prepare (fs, numChannels);
        processRange (m, mutableBuf, 0, buf.numSamples(), blockSizes[k]);
        const auto ref = referenceLoudness (buf, fs);
        std::printf ("k=%d I %.5f ref %.5f LRA %.4f ref %.4f\n", k, m.getIntegratedLufs(), ref.integrated, m.getLoudnessRangeLu(), ref.lra);
        CHECK_NEAR (m.getIntegratedLufs(), ref.integrated, 0.005);
        CHECK_NEAR (m.getLoudnessRangeLu(), ref.lra, 0.1);
        CHECK (ref.lra > 5.0); // the programme really has a range
        CHECK_NEAR (m.getMaxMomentaryLufs(), ref.maxM, 1e-3);
        CHECK_NEAR (m.getMaxShortTermLufs(), ref.maxS, 1e-3);
        CHECK_NEAR (m.getMomentaryLufs(), ref.m, 1e-3);
        CHECK_NEAR (m.getShortTermLufs(), ref.s, 1e-3);
    }
}

TEST_CASE ("LoudnessMeter (adversarial): EBU Tech 3342 LRA cases at 44.1 and 96 kHz")
{
    for (double fs : { 44100.0, 96000.0 })
    {
        LoudnessMeter m;
        m.prepare (fs, 2);
        ToneSource src (fs, 2, 4096);
        src.feed (m, -40.0, 20.0);
        src.feed (m, -20.0, 20.0);
        CHECK_NEAR (m.getLoudnessRangeLu(), 20.0, 0.1);
    }
}

TEST_CASE ("LoudnessMeter (adversarial): reset() / resetIntegrated() exactly on and off the 100 ms grid")
{
    const std::array<float, kMaxChannels> g { dbfs (-23.0), dbfs (-23.0) };
    const std::array<float, kMaxChannels> loud { dbfs (-10.0), dbfs (-10.0) };

    // reset() mid sub-block restarts the grid at the reset: momentary appears
    // exactly 400 ms after it, not at the old grid position.
    {
        LoudnessMeter m;
        m.prepare (kFs, 2);
        ToneSource src (kFs, 2, 97);
        src.feedSamples (m, loud, 12345);
        m.reset();
        src.feedSamples (m, g, 19199);
        CHECK (m.getMomentaryLufs() == kMinusInfDb);
        src.feedSamples (m, g, 1);
        CHECK_NEAR (m.getMomentaryLufs(), -23.0, 0.1);
        CHECK_NEAR (m.getMaxMomentaryLufs(), -23.0, 0.1); // nothing of the loud pre-reset audio survives
    }

    // resetIntegrated() exactly on a sub-block boundary: no sub-block is skipped,
    // the first fresh gating block is complete 400 ms later.
    {
        LoudnessMeter m;
        m.prepare (kFs, 2);
        ToneSource src (kFs, 2, 4800);
        src.feedSamples (m, loud, 4800 * 20);
        m.resetIntegrated();
        src.feedSamples (m, g, 4800 * 4 - 1);
        CHECK (m.getIntegratedLufs() == kMinusInfDb);
        src.feedSamples (m, g, 1);
        CHECK_NEAR (m.getIntegratedLufs(), -23.0, 0.05);
        CHECK_NEAR (m.getMaxMomentaryLufs(), -23.0, 0.05);
        // Short-term max / LRA only from windows entirely after the reset (3 s).
        src.feedSamples (m, g, 4800 * 26 - 1);
        CHECK (m.getMaxShortTermLufs() == kMinusInfDb);
        src.feedSamples (m, g, 1);
        CHECK_NEAR (m.getMaxShortTermLufs(), -23.0, 0.05);
    }
}

TEST_CASE ("LoudnessMeter (adversarial): blocks with fewer / more channels than prepared")
{
    // Prepared stereo, fed mono: the missing channel contributes nothing.
    {
        LoudnessMeter m;
        m.prepare (kFs, 2);
        ToneSource src (kFs, 1);
        src.feed (m, -20.0, 4.0, 1u);
        CHECK_NEAR (m.getIntegratedLufs(), -23.01, 0.05);
    }
    // Prepared stereo, fed 4 channels: the extra channels are ignored.
    {
        LoudnessMeter m;
        m.prepare (kFs, 2);
        ToneSource src (kFs, 4);
        src.feed (m, -20.0, 4.0, 0xFu);
        CHECK_NEAR (m.getIntegratedLufs(), -20.0, 0.05);
    }
}

TEST_CASE ("LoudnessMeter (adversarial): NaN in one channel only drops that channel's sub-block")
{
    LoudnessMeter m;
    m.prepare (kFs, 2);
    ToneSource src (kFs, 2, 4800);
    src.feed (m, -23.0, 2.0);
    // One sub-block where L carries a NaN: R still counts, so the newest
    // momentary window loses at most a quarter of L's energy (never -inf).
    Planar bad (2, 4800);
    copyInto (bad.ch[0], sine (1000.0, kFs, 4800, dbfs (-23.0)));
    copyInto (bad.ch[1], sine (1000.0, kFs, 4800, dbfs (-23.0)));
    bad.ch[0][4000] = std::numeric_limits<float>::quiet_NaN();
    m.process (bad.block());
    const double expected = -23.0 + 10.0 * std::log10 (7.0 / 8.0);
    CHECK_NEAR (m.getMomentaryLufs(), expected, 0.1);
    CHECK (std::isfinite (m.getIntegratedLufs()));

    // A NaN in the LFE of a 5.1 stream is never even filtered.
    LoudnessMeter s;
    s.prepare (kFs, 6);
    Planar six (6, 4800);
    std::fill (six.ch[3].begin(), six.ch[3].end(), std::numeric_limits<float>::quiet_NaN());
    copyInto (six.ch[0], sine (1000.0, kFs, 4800, dbfs (-20.0)));
    for (int i = 0; i < 10; ++i)
        s.process (six.block());
    CHECK (std::isfinite (s.getMomentaryLufs()));
    CHECK_GE (s.getMomentaryLufs(), -24.0f);
}

TEST_CASE ("LoudnessMeter (adversarial): K-weighting designs stay finite and stable for any rate")
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (double fs : { nan, inf, -inf, 0.0, -48000.0, 1.0, 8000.0, 11025.0, 768000.0, 1.0e9 })
    {
        for (const auto& c : { LoudnessMeter::kWeightingStage1 (fs), LoudnessMeter::kWeightingStage2 (fs) })
        {
            CHECK (std::isfinite (c.b0) && std::isfinite (c.b1) && std::isfinite (c.b2));
            CHECK (std::isfinite (c.a1) && std::isfinite (c.a2));
            // Jury conditions for a stable second-order denominator.
            CHECK (std::abs (c.a2) < 1.0);
            CHECK (std::abs (c.a1) < 1.0 + c.a2);
        }

        LoudnessMeter m;
        m.prepare (fs, 2);
        Planar buf (2, 4096);
        copyInto (buf.ch[0], whiteNoise (4096, 0.5f, 5));
        copyInto (buf.ch[1], whiteNoise (4096, 0.5f, 6));
        for (int b = 0; b < 60; ++b)
            m.process (buf.block());
        CHECK (allFinite (snapshot (m)));
    }
}

TEST_CASE ("LoudnessFollower (adversarial): recovers from NaN / Inf input")
{
    LoudnessFollower f;
    f.prepare (kFs, 2, 1000.0f);
    ToneSource src (kFs, 2);
    src.feed (f, -23.0, 3.0);
    Planar bad (2, 64);
    bad.ch[0][10] = std::numeric_limits<float>::quiet_NaN();
    bad.ch[1][63] = std::numeric_limits<float>::infinity();
    f.process (bad.block());
    CHECK (std::isfinite (f.getLufs()));
    src.feed (f, -23.0, 8.0);
    CHECK_NEAR (f.getLufs(), -23.0, 0.2);
    CHECK (f.isActive());
}
