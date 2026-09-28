// Tests for the 5.1 / 7.1 -> binaural headphone virtualiser (parametric
// Brown-Duda renderer and direct-convolution HRIR renderer).
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/analysis/LoudnessMeter.h"
#include "flub/dsp/HeadphoneVirtualizer.h"
#include "flub/dsp/TruePeakDetector.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>

using namespace flub;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;
constexpr double kC = 343.0;
constexpr float kTrim = 0.70794578f; // -3 dB headroom trim

// 7.1 channel indices (Windows order): FL FR FC LFE BL BR SL SR
constexpr int FL = 0, FC = 2, LFE = 3, BL = 4, SL = 6, SR = 7;

/** The renderer on its own: most tests check the model's exact paths, so the
    level match and the fold headroom (docs/11 E28a) are off unless a test
    turns them on (they have their own tests at the end of this file). */
VirtualizerParams paramsFor (ChannelLayout layout, float room = 0.0f)
{
    VirtualizerParams p;
    p.layout = layout;
    p.roomAmount = room;
    p.levelMatch = false;
    p.foldHeadroom = false;
    return p;
}

/** Prepares a virtualiser, applies params, resets (no glide from defaults). */
void setUp (HeadphoneVirtualizer& v, const VirtualizerParams& p, double fs = kFs, int maxBlock = 512, int channels = 8)
{
    v.setParams (p);
    v.prepare ({ fs, maxBlock, channels });
    v.reset();
}

/** Copies a signal into a Planar channel. (Assigning a new vector would
    leave Planar's channel pointers dangling.) */
void fill (Planar& buf, int channel, const std::vector<float>& signal)
{
    auto& dst = buf.ch[static_cast<size_t> (channel)];
    std::copy (signal.begin(), signal.begin() + static_cast<std::ptrdiff_t> (std::min (signal.size(), dst.size())), dst.begin());
}

Planar monoSource (int channels, int n, int sourceChannel, const std::vector<float>& signal)
{
    Planar buf (channels, n);
    fill (buf, sourceChannel, signal);
    return buf;
}

/** Woodworth ITD, written independently of the implementation's per-ear
    formula: ITD = (a/c)(phi + sin phi), phi = lateral angle (0 .. 90 deg). */
double woodworthItdSamples (double azimuthDeg, double radiusMm, double fs)
{
    const double az = std::abs (azimuthDeg);
    const double lateral = (az > 90.0 ? 180.0 - az : az) * kPi / 180.0;
    return radiusMm * 0.001 / kC * (lateral + std::sin (lateral)) * fs;
}

/** Lag (samples) that maximises sum x[n] y[n + lag]; positive = y lags x. */
int crossCorrelationLag (const float* x, const float* y, int n, int maxLag)
{
    int best = 0;
    double bestValue = -std::numeric_limits<double>::infinity();
    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        double acc = 0.0;
        for (int i = maxLag; i < n - maxLag; ++i)
            acc += static_cast<double> (x[i]) * y[i + lag];
        if (acc > bestValue)
        {
            bestValue = acc;
            best = lag;
        }
    }
    return best;
}

bool allFinite (const Planar& buf)
{
    for (const auto& c : buf.ch)
        for (float s : c)
            if (! std::isfinite (s))
                return false;
    return true;
}

double maxAbsDiff (const std::vector<float>& a, const std::vector<float>& b, int from = 0)
{
    double m = 0.0;
    for (size_t i = static_cast<size_t> (from); i < std::min (a.size(), b.size()); ++i)
        m = std::max (m, static_cast<double> (std::abs (a[i] - b[i])));
    return m;
}

/** Tone level (dB re input amplitude) at one ear for a sine on one channel. */
double earToneDb (const VirtualizerParams& p, int channel, int ear, double freq, double fs = kFs)
{
    HeadphoneVirtualizer v;
    setUp (v, p, fs);
    const int n = static_cast<int> (fs); // 1 s
    auto buf = monoSource (8, n, channel, sine (freq, fs, n, 0.25f));
    processInBlocks (v, buf, 256);
    return toDb (toneAmplitude (buf.ch[static_cast<size_t> (ear)].data() + n / 2, n / 2, freq, fs) / 0.25);
}

/** Synthetic HRIR set: for every speaker/ear a single impulse at a known tap
    with a known gain. The 7.1 LFE entry (index 3) holds garbage that must be
    ignored. With omitLfe the set lists the 7 speakers without an LFE entry. */
struct ImpulseSpec
{
    int delay[8][2];
    float gain[8][2];
};

ImpulseSpec makeImpulseSpec()
{
    ImpulseSpec s {};
    for (int c = 0; c < 8; ++c)
        for (int e = 0; e < 2; ++e)
        {
            s.delay[c][e] = (7 * c + 13 * e) % 50; // FL/left: 0 (zero latency path)
            s.gain[c][e] = (e == 0 ? 1.0f : -1.0f) * 0.125f * static_cast<float> (c + 1);
        }
    return s;
}

std::shared_ptr<HrirSet> makeImpulseSet (const ImpulseSpec& s, double fs, bool omitLfe, int length = 64)
{
    auto set = std::make_shared<HrirSet>();
    set->sampleRate = fs;
    set->layout = ChannelLayout::Surround71;
    set->length = length;
    for (int c = 0; c < 8; ++c)
    {
        if (omitLfe && c == LFE)
            continue;
        std::vector<float> l (static_cast<size_t> (length), 0.0f), r (static_cast<size_t> (length), 0.0f);
        if (c == LFE)
        {
            l[0] = r[0] = 100.0f; // must never be heard
        }
        else
        {
            l[static_cast<size_t> (s.delay[c][0])] = s.gain[c][0];
            r[static_cast<size_t> (s.delay[c][1])] = s.gain[c][1];
        }
        set->left.push_back (std::move (l));
        set->right.push_back (std::move (r));
    }
    return set;
}

Planar noiseOnAll (int channels, int n, float amp, bool silentLfe, uint32_t seed = 77)
{
    Planar buf (channels, n);
    for (int c = 0; c < channels; ++c)
        if (! (silentLfe && c == LFE))
            fill (buf, c, whiteNoise (n, amp, seed + static_cast<uint32_t> (c) * 101u));
    return buf;
}
} // namespace

//==============================================================================
TEST_CASE ("HeadphoneVirtualizer: speaker azimuth mapping for stereo, 5.1 and 7.1")
{
    const VirtualizerParams p;
    const auto az = [&p] (ChannelLayout l, int c) { return HeadphoneVirtualizer::speakerAzimuthDeg (l, c, p); };

    // 5.1: FL FR FC LFE SL SR
    CHECK (az (ChannelLayout::Surround51, 0) == -30.0f);
    CHECK (az (ChannelLayout::Surround51, 1) == 30.0f);
    CHECK (az (ChannelLayout::Surround51, 2) == 0.0f);
    CHECK (std::isnan (az (ChannelLayout::Surround51, 3)));
    CHECK (az (ChannelLayout::Surround51, 4) == -100.0f);
    CHECK (az (ChannelLayout::Surround51, 5) == 100.0f);
    CHECK (std::isnan (az (ChannelLayout::Surround51, 6)));

    // 7.1: FL FR FC LFE BL BR SL SR
    CHECK (az (ChannelLayout::Surround71, 0) == -30.0f);
    CHECK (az (ChannelLayout::Surround71, 1) == 30.0f);
    CHECK (az (ChannelLayout::Surround71, 2) == 0.0f);
    CHECK (std::isnan (az (ChannelLayout::Surround71, 3)));
    CHECK (az (ChannelLayout::Surround71, 4) == -145.0f);
    CHECK (az (ChannelLayout::Surround71, 5) == 145.0f);
    CHECK (az (ChannelLayout::Surround71, 6) == -100.0f);
    CHECK (az (ChannelLayout::Surround71, 7) == 100.0f);
    CHECK (std::isnan (az (ChannelLayout::Surround71, 8)));
    CHECK (std::isnan (az (ChannelLayout::Surround71, -1)));

    // Stereo: two virtual speakers at -/+ front angle.
    CHECK (az (ChannelLayout::Stereo, 0) == -30.0f);
    CHECK (az (ChannelLayout::Stereo, 1) == 30.0f);
    CHECK (std::isnan (az (ChannelLayout::Stereo, 2)));

    // Custom angles follow the parameters; out-of-range values are clamped.
    VirtualizerParams q;
    q.frontAngleDeg = 40.0f;
    q.sideAngleDeg = 110.0f;
    q.rearAngleDeg = 200.0f; // -> 165
    CHECK (HeadphoneVirtualizer::speakerAzimuthDeg (ChannelLayout::Surround71, 1, q) == 40.0f);
    CHECK (HeadphoneVirtualizer::speakerAzimuthDeg (ChannelLayout::Surround71, 6, q) == -110.0f);
    CHECK (HeadphoneVirtualizer::speakerAzimuthDeg (ChannelLayout::Surround71, 4, q) == -165.0f);
    q.frontAngleDeg = std::numeric_limits<float>::quiet_NaN(); // -> default
    CHECK (HeadphoneVirtualizer::speakerAzimuthDeg (ChannelLayout::Surround71, 0, q) == -30.0f);
}

TEST_CASE ("HeadphoneVirtualizer: side-left source is louder at 4 kHz and leads at the left ear by the Woodworth ITD")
{
    const auto p = paramsFor (ChannelLayout::Surround71, 0.15f);

    // ILD at 4 kHz (head shadow: alpha 1.98 near ear, 0.18 far ear).
    const double leftDb = earToneDb (p, SL, 0, 4000.0);
    const double rightDb = earToneDb (p, SL, 1, 4000.0);
    CHECK_GE (leftDb - rightDb, 6.0);
    CHECK_LE (leftDb - rightDb, 25.0);

    // ITD: broadband cross-correlation of the two ears.
    HeadphoneVirtualizer v;
    setUp (v, p);
    const int n = 48000;
    auto buf = monoSource (8, n, SL, whiteNoise (n, 0.3f));
    processInBlocks (v, buf, 512);
    const int lag = crossCorrelationLag (buf.ch[0].data(), buf.ch[1].data(), n, 64);
    const double expected = woodworthItdSamples (100.0, 87.5, kFs); // ~29.2 samples
    CHECK (lag > 0); // right ear lags: the left ear leads
    CHECK_NEAR (lag, expected, 2.0);

    // Mirror image: the direct path of SR is the exact mirror of SL (the
    // reflections alternate ears with different delays, so room = 0 here).
    HeadphoneVirtualizer a, b;
    setUp (a, paramsFor (ChannelLayout::Surround71, 0.0f));
    setUp (b, paramsFor (ChannelLayout::Surround71, 0.0f));
    auto sl = monoSource (8, n, SL, whiteNoise (n, 0.3f));
    auto sr = monoSource (8, n, SR, whiteNoise (n, 0.3f));
    processInBlocks (a, sl, 512);
    processInBlocks (b, sr, 512);
    CHECK (maxAbsDiff (sr.ch[0], sl.ch[1]) == 0.0);
    CHECK (maxAbsDiff (sr.ch[1], sl.ch[0]) == 0.0);
}

TEST_CASE ("HeadphoneVirtualizer: ITD follows the head radius and the sample rate")
{
    for (double fs : { 44100.0, 96000.0 })
        for (float radius : { 70.0f, 105.0f })
        {
            auto p = paramsFor (ChannelLayout::Surround71);
            p.headRadiusMm = radius;
            p.sideAngleDeg = 90.0f;
            HeadphoneVirtualizer v;
            setUp (v, p, fs);
            const int n = static_cast<int> (fs * 0.5);
            auto buf = monoSource (8, n, SL, whiteNoise (n, 0.3f));
            processInBlocks (v, buf, 512);
            const int maxLag = static_cast<int> (fs * 0.0012);
            const int lag = crossCorrelationLag (buf.ch[0].data(), buf.ch[1].data(), n, maxLag);
            CHECK_NEAR (lag, woodworthItdSamples (90.0, radius, fs), 2.0 * fs / kFs);
        }
}

TEST_CASE ("HeadphoneVirtualizer: centre speaker reaches both ears identically")
{
    // Without reflections the two ears are sample-identical.
    {
        HeadphoneVirtualizer v;
        setUp (v, paramsFor (ChannelLayout::Surround71, 0.0f));
        const int n = 48000;
        auto buf = monoSource (8, n, FC, whiteNoise (n, 0.3f));
        processInBlocks (v, buf, 512);
        CHECK (maxAbsDiff (buf.ch[0], buf.ch[1]) == 0.0);
        CHECK (crossCorrelationLag (buf.ch[0].data(), buf.ch[1].data(), n, 64) == 0);
        CHECK_NEAR (toDb (rms (buf.ch[0].data(), n) / rms (buf.ch[1].data(), n)), 0.0, 0.1);
        CHECK_GE (rms (buf.ch[0].data(), n), 0.05);
    }

    // With the default room the reflections alternate ears, but their energy
    // is balanced: still equal levels and zero lag.
    {
        HeadphoneVirtualizer v;
        setUp (v, paramsFor (ChannelLayout::Surround71, 0.15f));
        const int n = 96000;
        auto buf = monoSource (8, n, FC, whiteNoise (n, 0.3f));
        processInBlocks (v, buf, 512);
        CHECK_NEAR (toDb (rms (buf.ch[0].data(), n) / rms (buf.ch[1].data(), n)), 0.0, 0.1);
        CHECK (crossCorrelationLag (buf.ch[0].data(), buf.ch[1].data(), n, 64) == 0);
        for (double f : { 500.0, 3000.0 })
            CHECK_NEAR (earToneDb (paramsFor (ChannelLayout::Surround71, 0.0f), FC, 0, f),
                        earToneDb (paramsFor (ChannelLayout::Surround71, 0.0f), FC, 1, f), 1e-4);
    }
}

TEST_CASE ("HeadphoneVirtualizer: rear cue - BL is darker than FL at the left ear")
{
    const auto p = paramsFor (ChannelLayout::Surround71);
    const double loFL = earToneDb (p, FL, 0, 300.0), loBL = earToneDb (p, BL, 0, 300.0);
    const double hiFL = earToneDb (p, FL, 0, 10000.0), hiBL = earToneDb (p, BL, 0, 10000.0);
    CHECK_NEAR (loBL - loFL, 0.0, 0.5);        // same level in the bass
    CHECK_LE ((hiBL - loBL) - (hiFL - loFL), -2.5); // ~-4 dB shelf above 4 kHz
    // Side speakers (100 deg) are behind the ear axis too and get the cue;
    // at 80 deg they do not.
    auto front = p;
    front.sideAngleDeg = 80.0f;
    const double tiltBehind = earToneDb (p, SL, 0, 10000.0) - earToneDb (p, SL, 0, 300.0);
    const double tiltAhead = earToneDb (front, SL, 0, 10000.0) - earToneDb (front, SL, 0, 300.0);
    CHECK_LE (tiltBehind - tiltAhead, -2.5);
}

TEST_CASE ("HeadphoneVirtualizer: LFE reaches both ears equally, low-passed, at lfeGainDb")
{
    for (float lfeDb : { 0.0f, 6.0f, -12.0f })
    {
        auto p = paramsFor (ChannelLayout::Surround51, 0.15f);
        p.lfeGainDb = lfeDb;
        for (double f : { 50.0, 1000.0 })
        {
            HeadphoneVirtualizer v;
            setUp (v, p);
            const int n = 48000;
            auto buf = monoSource (8, n, LFE, sine (f, kFs, n, 0.25f));
            processInBlocks (v, buf, 256);
            CHECK (maxAbsDiff (buf.ch[0], buf.ch[1]) == 0.0);
            const double db = toDb (toneAmplitude (buf.ch[0].data() + n / 2, n / 2, f, kFs) / 0.25);
            if (f < 100.0)
                CHECK_NEAR (db, -3.0 + lfeDb, 0.1); // passband: trim + LFE gain
            else
                CHECK_LE (db, -60.0 + lfeDb); // 24 dB/oct above 120 Hz
        }
    }
}

TEST_CASE ("HeadphoneVirtualizer: channels >= 2 are zero after processing")
{
    for (auto layout : { ChannelLayout::Stereo, ChannelLayout::Surround51, ChannelLayout::Surround71 })
    {
        HeadphoneVirtualizer v;
        setUp (v, paramsFor (layout, 0.5f));
        auto buf = noiseOnAll (8, 4096, 0.5f, false);
        processInBlocks (v, buf, 300);
        CHECK (rms (buf.ch[0].data(), 4096) > 0.01);
        CHECK (rms (buf.ch[1].data(), 4096) > 0.01);
        for (int c = 2; c < 8; ++c)
            CHECK (peakAbs (buf.ch[static_cast<size_t> (c)].data(), 4096) == 0.0);
    }
}

TEST_CASE ("HeadphoneVirtualizer: stereo layout renders two mirrored virtual speakers")
{
    HeadphoneVirtualizer a, b;
    setUp (a, paramsFor (ChannelLayout::Stereo), kFs, 512, 2);
    setUp (b, paramsFor (ChannelLayout::Stereo), kFs, 512, 2);
    const int n = 24000;
    Planar left (2, n), right (2, n);
    fill (left, 0, whiteNoise (n, 0.4f));
    fill (right, 1, whiteNoise (n, 0.4f));
    processInBlocks (a, left, 512);
    processInBlocks (b, right, 512);
    CHECK (maxAbsDiff (left.ch[0], right.ch[1]) < 1e-6);
    CHECK (maxAbsDiff (left.ch[1], right.ch[0]) < 1e-6);
    CHECK_GE (earToneDb (paramsFor (ChannelLayout::Stereo), 0, 0, 4000.0) - earToneDb (paramsFor (ChannelLayout::Stereo), 0, 1, 4000.0), 3.0);
    const int lag = crossCorrelationLag (left.ch[0].data(), left.ch[1].data(), n, 64);
    CHECK_NEAR (lag, woodworthItdSamples (30.0, 87.5, kFs), 2.0);
}

TEST_CASE ("HeadphoneVirtualizer: HRIR renderer produces exactly the expected delayed/scaled copies")
{
    const ImpulseSpec spec = makeImpulseSpec();
    for (bool omitLfe : { false, true })
        for (int blockSize : { 100, 512 })
        {
            HeadphoneVirtualizer v;
            v.setHrirSet (makeImpulseSet (spec, kFs, omitLfe));
            setUp (v, paramsFor (ChannelLayout::Surround71, 0.0f));
            const int n = 4000;
            auto buf = noiseOnAll (8, n, 0.5f, true);
            const auto input = buf.ch;
            processInBlocks (v, buf, blockSize);

            double err = 0.0;
            for (int e = 0; e < 2; ++e)
                for (int i = 0; i < n; ++i)
                {
                    double expected = 0.0;
                    for (int c = 0; c < 8; ++c)
                    {
                        const int k = i - spec.delay[c][e];
                        if (c != LFE && k >= 0)
                            expected += static_cast<double> (spec.gain[c][e]) * input[static_cast<size_t> (c)][static_cast<size_t> (k)];
                    }
                    expected *= static_cast<double> (kTrim);
                    err = std::max (err, std::abs (expected - buf.ch[static_cast<size_t> (e)][static_cast<size_t> (i)]));
                }
            CHECK_LE (err, 2e-6);
        }
}

TEST_CASE ("HeadphoneVirtualizer: HRIRs longer than 1024 taps are truncated with a half-cosine fade-out")
{
    // Direct form is O(taps): sets are capped at 1024 taps, the last 64 kept
    // taps faded to zero (no abrupt truncation), everything later dropped.
    constexpr int kLength = 2048;
    auto set = std::make_shared<HrirSet>();
    set->sampleRate = kFs;
    set->layout = ChannelLayout::Surround71;
    set->length = kLength;
    for (int c = 0; c < 8; ++c)
    {
        std::vector<float> l (static_cast<size_t> (kLength), 0.0f), r (static_cast<size_t> (kLength), 0.0f);
        if (c == 0)
            l[10] = l[1000] = l[1500] = 0.5f;
        set->left.push_back (std::move (l));
        set->right.push_back (std::move (r));
    }
    HeadphoneVirtualizer v;
    v.setHrirSet (set);
    setUp (v, paramsFor (ChannelLayout::Surround71, 0.0f));
    const int n = 4096;
    Planar buf (8, n);
    buf.ch[0][0] = 1.0f; // unit impulse on FL
    processInBlocks (v, buf, 512);
    const float* y = buf.ch[0].data();
    CHECK_NEAR (y[10], 0.5 * kTrim, 1e-6);                                                     // before the fade: exact
    CHECK_NEAR (y[1000], 0.5 * kTrim * (0.5 + 0.5 * std::cos (kPi * 41.0 / 64.0)), 1e-6);      // inside the fade
    double tail = 0.0;
    for (int i = 1024; i < n; ++i)
        tail = std::max (tail, static_cast<double> (std::abs (y[i])));
    CHECK (tail == 0.0); // the tap at 1500 is beyond the cap
}

TEST_CASE ("HeadphoneVirtualizer: HRIR renderer matches a reference convolution for dense responses")
{
    const int length = 37; // not a multiple of the 4-way unrolled dot product
    auto set = std::make_shared<HrirSet>();
    set->sampleRate = kFs;
    set->layout = ChannelLayout::Surround51;
    set->length = length;
    for (int c = 0; c < 6; ++c)
    {
        auto l = whiteNoise (length, 0.3f, 500u + static_cast<uint32_t> (c));
        auto r = whiteNoise (length, 0.3f, 900u + static_cast<uint32_t> (c));
        set->left.push_back (l);
        set->right.push_back (r);
    }

    HeadphoneVirtualizer v;
    v.setHrirSet (set);
    setUp (v, paramsFor (ChannelLayout::Surround51, 0.0f), kFs, 512, 6);
    const int n = 3000;
    auto buf = noiseOnAll (6, n, 0.5f, true, 5);
    const auto input = buf.ch;
    processInBlocks (v, buf, 7);

    double err = 0.0;
    for (int e = 0; e < 2; ++e)
    {
        const auto& irs = e == 0 ? set->left : set->right;
        for (int i = 0; i < n; ++i)
        {
            double expected = 0.0;
            for (int c = 0; c < 6; ++c)
                if (c != LFE)
                    for (int k = 0; k < length && k <= i; ++k)
                        expected += static_cast<double> (irs[static_cast<size_t> (c)][static_cast<size_t> (k)])
                                    * input[static_cast<size_t> (c)][static_cast<size_t> (i - k)];
            err = std::max (err, std::abs (expected * kTrim - buf.ch[static_cast<size_t> (e)][static_cast<size_t> (i)]));
        }
    }
    CHECK_LE (err, 1e-5);
    for (int c = 2; c < 6; ++c)
        CHECK (peakAbs (buf.ch[static_cast<size_t> (c)].data(), n) == 0.0);
}

TEST_CASE ("HeadphoneVirtualizer: HRIR set with another sample rate or layout falls back to the parametric renderer")
{
    const ImpulseSpec spec = makeImpulseSpec();
    const int n = 6000;
    auto reference = noiseOnAll (8, n, 0.5f, false);
    {
        HeadphoneVirtualizer plain;
        setUp (plain, paramsFor (ChannelLayout::Surround71, 0.3f));
        processInBlocks (plain, reference, 256);
    }

    // Sample-rate mismatch (set at 44.1 kHz, session at 48 kHz).
    {
        HeadphoneVirtualizer v;
        v.setHrirSet (makeImpulseSet (spec, 44100.0, false));
        setUp (v, paramsFor (ChannelLayout::Surround71, 0.3f));
        auto buf = noiseOnAll (8, n, 0.5f, false);
        processInBlocks (v, buf, 256);
        CHECK (maxAbsDiff (buf.ch[0], reference.ch[0]) == 0.0);
        CHECK (maxAbsDiff (buf.ch[1], reference.ch[1]) == 0.0);
    }

    // Layout mismatch (7.1 set, 5.1 session): parametric 5.1.
    {
        HeadphoneVirtualizer plain51, v;
        setUp (plain51, paramsFor (ChannelLayout::Surround51, 0.3f));
        v.setHrirSet (makeImpulseSet (spec, kFs, false));
        setUp (v, paramsFor (ChannelLayout::Surround51, 0.3f));
        auto a = noiseOnAll (8, n, 0.5f, false);
        auto b = noiseOnAll (8, n, 0.5f, false);
        processInBlocks (plain51, a, 256);
        processInBlocks (v, b, 256);
        CHECK (maxAbsDiff (a.ch[0], b.ch[0]) == 0.0);
        CHECK (maxAbsDiff (a.ch[1], b.ch[1]) == 0.0);
    }

    // Malformed set (too few entries) and a NaN tap: parametric as well.
    {
        auto bad = makeImpulseSet (spec, kFs, false);
        bad->left.pop_back();
        bad->right.pop_back();
        bad->left.pop_back();
        bad->right.pop_back();
        auto nanSet = makeImpulseSet (spec, kFs, false);
        nanSet->left[1][3] = std::numeric_limits<float>::quiet_NaN();
        for (const auto& s : { bad, nanSet })
        {
            HeadphoneVirtualizer v;
            v.setHrirSet (s);
            setUp (v, paramsFor (ChannelLayout::Surround71, 0.3f));
            auto buf = noiseOnAll (8, n, 0.5f, false);
            processInBlocks (v, buf, 256);
            CHECK (maxAbsDiff (buf.ch[0], reference.ch[0]) == 0.0);
        }
    }
}

//==============================================================================
TEST_CASE ("HeadphoneVirtualizer: no allocation in reset / setParams / process")
{
    for (bool withHrir : { false, true })
    {
        HeadphoneVirtualizer v;
        if (withHrir)
            v.setHrirSet (makeImpulseSet (makeImpulseSpec(), kFs, false));
        v.prepare ({ kFs, 512, 8 });
        auto buf = noiseOnAll (8, 512 * 12, 0.5f, false);

        AllocationGuard guard;
        v.reset();
        auto p = paramsFor (ChannelLayout::Surround71, 0.4f);
        for (int b = 0; b < 12; ++b)
        {
            p.frontAngleDeg = 22.0f + 2.0f * static_cast<float> (b);
            p.sideAngleDeg = b % 2 == 0 ? 80.0f : 120.0f;
            p.headRadiusMm = 70.0f + 3.0f * static_cast<float> (b);
            p.roomAmount = 0.1f * static_cast<float> (b % 5);
            p.lfeGainDb = static_cast<float> (b) - 6.0f;
            p.layout = b == 5 ? ChannelLayout::Surround51 : (b == 8 ? ChannelLayout::Stereo : ChannelLayout::Surround71);
            v.setParams (p);
            v.process (buf.block (b * 512, b == 3 ? 17 : 512));
        }
        CHECK (guard.allocations() == 0);
    }
}

TEST_CASE ("HeadphoneVirtualizer: robustness - silence, DC, full-scale noise, impulses, extreme params, all rates")
{
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();

    VirtualizerParams extremes[4];
    extremes[0] = paramsFor (ChannelLayout::Surround71, 0.15f);
    extremes[1].frontAngleDeg = -inf; // everything clamped to the minimum
    extremes[1].sideAngleDeg = -1000.0f;
    extremes[1].rearAngleDeg = 0.0f;
    extremes[1].headRadiusMm = 0.0f;
    extremes[1].roomAmount = -5.0f;
    extremes[1].lfeGainDb = -inf;
    extremes[2].frontAngleDeg = inf; // ... and the maximum
    extremes[2].sideAngleDeg = 1000.0f;
    extremes[2].rearAngleDeg = 720.0f;
    extremes[2].headRadiusMm = 1.0e9f;
    extremes[2].roomAmount = 100.0f;
    extremes[2].lfeGainDb = 100.0f;
    extremes[3].frontAngleDeg = nan; // ... or the defaults
    extremes[3].sideAngleDeg = nan;
    extremes[3].rearAngleDeg = nan;
    extremes[3].headRadiusMm = nan;
    extremes[3].roomAmount = nan;
    extremes[3].lfeGainDb = nan;
    extremes[3].layout = static_cast<ChannelLayout> (200);

    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (const auto& p : extremes)
        {
            HeadphoneVirtualizer v;
            setUp (v, p, fs, 1024);
            const auto& got = v.getParams();
            CHECK (std::isfinite (got.frontAngleDeg) && std::isfinite (got.headRadiusMm) && std::isfinite (got.lfeGainDb));
            CHECK (got.roomAmount >= 0.0f && got.roomAmount <= 1.0f);

            const int n = 8192;
            // Silence stays exactly silent.
            Planar silence (8, n);
            processInBlocks (v, silence, 1024);
            for (int c = 0; c < 8; ++c)
                CHECK (peakAbs (silence.ch[static_cast<size_t> (c)].data(), n) == 0.0);

            // DC on every channel.
            Planar dc (8, n);
            for (auto& c : dc.ch)
                std::fill (c.begin(), c.end(), 1.0f);
            processInBlocks (v, dc, 1024);
            CHECK (allFinite (dc));
            CHECK_LE (peakAbs (dc.ch[0].data(), n), 12.0);

            // Full-scale independent noise on every channel.
            auto noise = noiseOnAll (8, n, 1.0f, false, 3);
            processInBlocks (v, noise, 1000);
            CHECK (allFinite (noise));
            CHECK_LE (peakAbs (noise.ch[0].data(), n), 24.0);
            CHECK_LE (peakAbs (noise.ch[1].data(), n), 24.0);

            // Full-scale noise on one speaker only: a sane gain.
            auto single = monoSource (8, n, SL, whiteNoise (n, 1.0f, 9));
            processInBlocks (v, single, 64);
            CHECK (allFinite (single));
            CHECK_LE (peakAbs (single.ch[0].data(), n), 4.0);

            // Single-sample full-scale impulses on every channel.
            Planar imp (8, n);
            for (auto& c : imp.ch)
                c[100] = 1.0f;
            processInBlocks (v, imp, 1024);
            CHECK (allFinite (imp));
            CHECK_LE (peakAbs (imp.ch[0].data(), n), 12.0);

            // After the input stops, everything decays to exact zero (no
            // lingering subnormal tails).
            Planar tail (8, static_cast<int> (fs));
            processInBlocks (v, tail, 1024);
            CHECK (peakAbs (tail.ch[0].data() + tail.numSamples() - 256, 256) == 0.0);
            CHECK (peakAbs (tail.ch[1].data() + tail.numSamples() - 256, 256) == 0.0);
        }
}

TEST_CASE ("HeadphoneVirtualizer: fewer channels than the layout, mono blocks and odd blocks are safe")
{
    HeadphoneVirtualizer v;
    setUp (v, paramsFor (ChannelLayout::Surround71, 0.2f), kFs, 4096, 8);
    for (int channels : { 8, 6, 3, 2, 1, 8 })
    {
        auto buf = noiseOnAll (channels, 4096, 0.5f, false, static_cast<uint32_t> (channels));
        processInBlocks (v, buf, channels == 3 ? 4096 : 333);
        CHECK (allFinite (buf));
        CHECK (rms (buf.ch[0].data(), 4096) > 0.01);
        CHECK_LE (peakAbs (buf.ch[0].data(), 4096), 8.0);
        for (int c = 2; c < channels; ++c)
            CHECK (peakAbs (buf.ch[static_cast<size_t> (c)].data(), 4096) == 0.0);
    }
    // Zero-length and single-sample blocks.
    Planar one (8, 1);
    one.ch[FC][0] = 1.0f;
    v.process (one.block (0, 0));
    v.process (one.block());
    CHECK (allFinite (one));
}

TEST_CASE ("HeadphoneVirtualizer: output is independent of the host block size")
{
    for (bool withHrir : { false, true })
    {
        const int n = 3584 * 3; // 3584 = lcm (7, 512): parameter changes land on every block grid
        std::vector<std::vector<float>> ref;
        for (int blockSize : { 512, 1, 7, 64 })
        {
            HeadphoneVirtualizer v;
            if (withHrir)
                v.setHrirSet (makeImpulseSet (makeImpulseSpec(), kFs, false));
            setUp (v, paramsFor (ChannelLayout::Surround71, 0.3f));
            auto buf = noiseOnAll (8, n, 0.5f, false, 11);
            for (int pos = 0; pos < n; pos += blockSize)
            {
                if (pos == 3584)
                {
                    auto p = paramsFor (ChannelLayout::Surround71, 0.8f);
                    p.sideAngleDeg = 85.0f; // crosses 90 deg: rear-cue shelf glide
                    p.frontAngleDeg = 40.0f;
                    p.headRadiusMm = 100.0f;
                    p.lfeGainDb = 6.0f;
                    v.setParams (p);
                }
                if (pos == 7168)
                {
                    auto p = v.getParams();
                    p.layout = ChannelLayout::Surround51; // swap fade
                    v.setParams (p);
                }
                v.process (buf.block (pos, std::min (blockSize, n - pos)));
            }
            if (ref.empty())
            {
                ref = { buf.ch[0], buf.ch[1] };
                continue;
            }
            CHECK_LE (maxAbsDiff (buf.ch[0], ref[0]), 1e-5);
            CHECK_LE (maxAbsDiff (buf.ch[1], ref[1]), 1e-5);
        }
    }
}

TEST_CASE ("HeadphoneVirtualizer: zero latency - near-ear and HRIR paths respond at sample 0")
{
    CHECK (HeadphoneVirtualizer().latencySamples() == 0);

    // Side speaker exactly at the left ear (90 deg): Woodworth delay 0 to the
    // left ear, (a/c)(1 + pi/2) to the right ear.
    auto p = paramsFor (ChannelLayout::Surround71);
    p.sideAngleDeg = 90.0f;
    HeadphoneVirtualizer v;
    setUp (v, p);
    CHECK (v.latencySamples() == 0);
    Planar buf (8, 256);
    buf.ch[SL][0] = 0.5f;
    v.process (buf.block());
    CHECK (std::abs (buf.ch[0][0]) > 0.1f);
    int peakR = 0;
    for (int i = 0; i < 256; ++i)
        if (std::abs (buf.ch[1][static_cast<size_t> (i)]) > std::abs (buf.ch[1][static_cast<size_t> (peakR)]))
            peakR = i;
    CHECK_NEAR (peakR, 0.0875 / kC * (1.0 + kPi / 2.0) * kFs, 1.5);

    // HRIR renderer: the FL/left impulse is at tap 0 -> output at sample 0.
    HeadphoneVirtualizer h;
    h.setHrirSet (makeImpulseSet (makeImpulseSpec(), kFs, false));
    setUp (h, paramsFor (ChannelLayout::Surround71, 0.0f));
    CHECK (h.latencySamples() == 0);
    Planar imp (8, 64);
    imp.ch[FL][0] = 1.0f;
    h.process (imp.block());
    CHECK_NEAR (imp.ch[0][0], 0.125 * kTrim, 1e-7);
    CHECK (imp.ch[0][1] == 0.0f);
}

TEST_CASE ("HeadphoneVirtualizer: angle / head-radius changes are click-free and land on the target design")
{
    const int n = 48000;
    const auto input = sine (100.0, kFs, n, 0.5f);

    auto p0 = paramsFor (ChannelLayout::Surround71);
    HeadphoneVirtualizer v;
    setUp (v, p0);
    auto buf = monoSource (8, n, SL, input);
    const int changeA = 12000, changeB = 24000;
    auto p1 = p0;
    p1.sideAngleDeg = 80.0f; // crosses 90 deg: the rear-cue shelf switches off
    p1.headRadiusMm = 70.0f;
    auto p2 = p0;
    p2.sideAngleDeg = 120.0f;
    p2.headRadiusMm = 105.0f;
    for (int pos = 0; pos < n; pos += 64)
    {
        if (pos == changeA)
            v.setParams (p1);
        if (pos == changeB)
            v.setParams (p2);
        v.process (buf.block (pos, 64));
    }

    // Second difference of a 100 Hz sine is ~w^2 A; a click (value or slope
    // step) would dwarf it.
    const auto maxSecondDiff = [] (const std::vector<float>& y, int from, int to)
    {
        double m = 0.0;
        for (int i = from + 2; i < to; ++i)
            m = std::max (m, static_cast<double> (std::abs (y[static_cast<size_t> (i)] - 2.0f * y[static_cast<size_t> (i - 1)] + y[static_cast<size_t> (i - 2)])));
        return m;
    };
    for (int e = 0; e < 2; ++e)
    {
        const auto& y = buf.ch[static_cast<size_t> (e)];
        const double steady = maxSecondDiff (y, 4000, changeA);
        CHECK (steady > 0.0);
        CHECK_LE (maxSecondDiff (y, changeA, n), 2.0 * steady);
    }

    // After the glide the output equals a virtualiser prepared at p2.
    HeadphoneVirtualizer fresh;
    setUp (fresh, p2);
    auto ref = monoSource (8, n, SL, input);
    processInBlocks (fresh, ref, 64);
    CHECK_LE (maxAbsDiff (buf.ch[0], ref.ch[0], n - 4000), 1e-4);
    CHECK_LE (maxAbsDiff (buf.ch[1], ref.ch[1], n - 4000), 1e-4);
}

TEST_CASE ("HeadphoneVirtualizer: layout changes fade out / swap / fade in without a click")
{
    const int n = 24000;
    const auto input = sine (100.0, kFs, n, 0.5f);
    HeadphoneVirtualizer v;
    setUp (v, paramsFor (ChannelLayout::Surround71, 0.0f));
    auto buf = monoSource (8, n, FC, input);
    const int change = 9600;
    for (int pos = 0; pos < n; pos += 128)
    {
        if (pos == change)
            v.setParams (paramsFor (ChannelLayout::Stereo, 0.0f)); // FC is not part of stereo
        v.process (buf.block (pos, std::min (128, n - pos)));
    }
    const auto& y = buf.ch[0];
    double steadyStep = 0.0, changeStep = 0.0;
    for (int i = 1; i < n; ++i)
    {
        const double d = std::abs (y[static_cast<size_t> (i)] - y[static_cast<size_t> (i - 1)]);
        (i < change ? steadyStep : changeStep) = std::max (i < change ? steadyStep : changeStep, d);
    }
    CHECK_LE (changeStep, 1.5 * steadyStep);
    // The swap is complete within ~10 ms: FC is silent in the stereo layout.
    CHECK (peakAbs (y.data() + change + 960, n - change - 960) == 0.0);
    CHECK (v.getParams().layout == ChannelLayout::Stereo);

    // Switching 7.1 -> 5.1 moves channel 4 from BL (145 deg) to SL (100 deg).
    HeadphoneVirtualizer w;
    setUp (w, paramsFor (ChannelLayout::Surround71, 0.0f));
    auto b2 = monoSource (8, n, 4, input);
    for (int pos = 0; pos < n; pos += 128)
    {
        if (pos == change)
            w.setParams (paramsFor (ChannelLayout::Surround51, 0.0f));
        w.process (b2.block (pos, std::min (128, n - pos)));
    }
    HeadphoneVirtualizer ref;
    setUp (ref, paramsFor (ChannelLayout::Surround51, 0.0f));
    auto r2 = monoSource (8, n, 4, input);
    processInBlocks (ref, r2, 128);
    CHECK_LE (maxAbsDiff (b2.ch[0], r2.ch[0], n - 4000), 1e-4);
    CHECK_LE (maxAbsDiff (b2.ch[1], r2.ch[1], n - 4000), 1e-4);
}

TEST_CASE ("HeadphoneVirtualizer: room amount adds reflections 4-19 ms after the direct sound")
{
    for (float room : { 0.0f, 1.0f })
    {
        HeadphoneVirtualizer v;
        setUp (v, paramsFor (ChannelLayout::Surround71, room));
        const int n = 2048;
        Planar buf (8, n);
        buf.ch[FC][0] = 1.0f;
        processInBlocks (v, buf, 512); // blocks <= spec.maxBlockSize (Processor contract)
        const double early = peakAbs (buf.ch[0].data(), 150);            // direct (< 3 ms)
        const double reflections = peakAbs (buf.ch[0].data() + 180, 800); // 3.75 .. 20.4 ms
        const double late = peakAbs (buf.ch[0].data() + 1300, n - 1300);  // > 27 ms
        CHECK (early > 0.1);
        if (room == 0.0f)
            CHECK_LE (reflections, 1e-6);
        else
            CHECK_GE (reflections, 0.02);
        CHECK_LE (late, 1e-3);
    }
}

//==============================================================================
// ---- adversarial review tests ----
namespace
{
/** Reference model of one parametric (speaker, ear) path at an exact integer
    or fractional Woodworth delay, written from the header formulas: 4-tap
    Lagrange fractional delay followed by the Brown-Duda head shadow. */
std::vector<double> referenceEarImpulse (double delaySamples, double thetaDeg, double radiusM, double fs, int n)
{
    const int base = std::max (0, static_cast<int> (std::floor (delaySamples)) - 1);
    const double d = delaySamples - base;
    const double h[4] = { -(d - 1) * (d - 2) * (d - 3) / 6.0, d * (d - 2) * (d - 3) / 2.0,
                          -d * (d - 1) * (d - 3) / 2.0, d * (d - 1) * (d - 2) / 6.0 };
    std::vector<double> x (static_cast<size_t> (n), 0.0);
    for (int k = 0; k < 4; ++k)
        if (base + k < n)
            x[static_cast<size_t> (base + k)] = h[k];

    const double w0 = kC / radiusM;
    const double alpha = 1.05 + 0.95 * std::cos (thetaDeg * 1.2 * kPi / 180.0);
    const auto c = BiquadCoeffs::fromAnalogFirstOrder (1.0, alpha / (2.0 * w0), 1.0, 1.0 / (2.0 * w0), fs);
    BiquadState s;
    for (auto& v : x)
        v = biquadTick (c, s, v);
    return x;
}

/** Brown-Duda head-shadow magnitude (dB) of the bilinear design at f. */
double shadowDb (double thetaDeg, double radiusM, double f, double fs)
{
    const double w0 = kC / radiusM;
    const double alpha = 1.05 + 0.95 * std::cos (thetaDeg * 1.2 * kPi / 180.0);
    const auto c = BiquadCoeffs::fromAnalogFirstOrder (1.0, alpha / (2.0 * w0), 1.0, 1.0 / (2.0 * w0), fs);
    return 20.0 * std::log10 (std::abs (c.response (f, fs)));
}

/** Deterministic random partition of [0, n) into blocks of 1 .. maxBlock. */
std::vector<int> randomPartition (int n, int maxBlock, uint32_t seed)
{
    FastRandom rng (seed);
    std::vector<int> sizes;
    for (int pos = 0; pos < n;)
    {
        const uint32_t r = rng.nextU32();
        int len = r % 4 == 0 ? 1 : static_cast<int> (1 + (r >> 8) % static_cast<uint32_t> (maxBlock));
        len = std::min (len, n - pos);
        sizes.push_back (len);
        pos += len;
    }
    return sizes;
}
} // namespace

TEST_CASE ("HeadphoneVirtualizer [adversarial]: centre and ear-axis paths match the header model sample by sample")
{
    // a/c = 12 samples exactly at 48 kHz: the centre speaker reaches both
    // ears through an integer 12-sample delay and the theta = 90 deg shadow.
    const double radiusM = 12.0 * kC / kFs; // 85.75 mm
    auto p = paramsFor (ChannelLayout::Surround71, 0.0f);
    p.headRadiusMm = static_cast<float> (radiusM * 1000.0);
    p.sideAngleDeg = 90.0f;
    const int n = 256;

    {
        HeadphoneVirtualizer v;
        setUp (v, p);
        Planar buf (8, n);
        buf.ch[FC][0] = 1.0f;
        v.process (buf.block());
        const auto ref = referenceEarImpulse (12.0, 90.0, radiusM, kFs, n);
        double err = 0.0;
        for (int e = 0; e < 2; ++e)
            for (int i = 0; i < n; ++i)
                err = std::max (err, std::abs (kTrim * ref[static_cast<size_t> (i)] - buf.ch[static_cast<size_t> (e)][static_cast<size_t> (i)]));
        CHECK_LE (err, 1e-6);
        for (int i = 0; i < 12; ++i)
            CHECK (buf.ch[0][static_cast<size_t> (i)] == 0.0f);
    }

    // SL exactly on the left ear axis: delay 0 / theta 0 to the left ear,
    // (a/c)(1 + pi/2) / theta 180 to the right ear (fractional delay). Both
    // behind-the-ear-axis rules are off at exactly 90 deg (no rear shelf).
    {
        HeadphoneVirtualizer v;
        setUp (v, p);
        Planar buf (8, n);
        buf.ch[SL][0] = 1.0f;
        v.process (buf.block());
        const auto near = referenceEarImpulse (0.0, 0.0, radiusM, kFs, n);
        const auto far = referenceEarImpulse (12.0 * (1.0 + kPi / 2.0), 180.0, radiusM, kFs, n);
        double errL = 0.0, errR = 0.0;
        for (int i = 0; i < n; ++i)
        {
            errL = std::max (errL, std::abs (kTrim * near[static_cast<size_t> (i)] - buf.ch[0][static_cast<size_t> (i)]));
            errR = std::max (errR, std::abs (kTrim * far[static_cast<size_t> (i)] - buf.ch[1][static_cast<size_t> (i)]));
        }
        CHECK_LE (errL, 1e-6);
        CHECK_LE (errR, 1e-6);
    }
}

TEST_CASE ("HeadphoneVirtualizer [adversarial]: ILD matches the Brown-Duda response at every rate")
{
    // Shelf and trim are common to both ears, so the ILD of a side source is
    // the ratio of the two head-shadow responses (plus a small Lagrange
    // ripple at 48 kHz for the far ear's fractional delay).
    for (double fs : { 48000.0, 192000.0 })
        for (double f : { 1000.0, 4000.0, 10000.0 })
        {
            const auto p = paramsFor (ChannelLayout::Surround71, 0.0f);
            const double ild = earToneDb (p, SL, 0, f, fs) - earToneDb (p, SL, 1, f, fs);
            const double expected = shadowDb (10.0, 0.0875, f, fs) - shadowDb (170.0, 0.0875, f, fs);
            CHECK_NEAR (ild, expected, fs > 100000.0 ? 0.1 : 0.75);
        }
}

TEST_CASE ("HeadphoneVirtualizer [adversarial]: largest ITD at 192 kHz fits the delay line without wrapping")
{
    auto p = paramsFor (ChannelLayout::Surround71, 0.0f);
    p.headRadiusMm = 105.0f;
    p.sideAngleDeg = 90.0f;
    const double fs = 192000.0;
    HeadphoneVirtualizer v;
    setUp (v, p, fs, 1024);
    Planar buf (8, 1024);
    buf.ch[SR][0] = 1.0f; // right ear axis: far (left) ear gets the maximum delay
    v.process (buf.block());
    const double expectedDelay = 0.105 / kC * (1.0 + kPi / 2.0) * fs; // ~151.1
    const auto ref = referenceEarImpulse (expectedDelay, 180.0, 0.105, fs, 1024);
    double err = 0.0;
    for (int i = 0; i < 1024; ++i)
        err = std::max (err, std::abs (kTrim * ref[static_cast<size_t> (i)] - buf.ch[0][static_cast<size_t> (i)]));
    CHECK_LE (err, 1e-6);
    CHECK (peakAbs (buf.ch[0].data(), 140) == 0.0); // nothing arrives before the path delay
}

TEST_CASE ("HeadphoneVirtualizer [adversarial]: bit-exact under random block partitions with continuous automation")
{
    // Parameters change at fixed stream positions (every 700 samples:
    // angles, head, room, LFE and two layout swaps); each interval is cut
    // into random blocks of 1 .. 4096 samples. The header promises output
    // that is sample-identical for any host block size.
    // Also with the level match and the fold headroom (docs/11 E28a) on and
    // toggled: 0.5 noise on every channel drives the headroom gain too.
    for (int variant = 0; variant < 3; ++variant)
    {
        const bool withHrir = variant == 1;
        const bool matched = variant == 2;
        const int interval = 700, numIntervals = 40, n = interval * numIntervals;
        std::vector<std::vector<float>> ref;
        for (uint32_t seed : { 0u, 1u, 2u, 3u })
        {
            HeadphoneVirtualizer v;
            if (withHrir)
                v.setHrirSet (makeImpulseSet (makeImpulseSpec(), kFs, false));
            setUp (v, paramsFor (ChannelLayout::Surround71, 0.3f), kFs, 4096);
            auto buf = noiseOnAll (8, n, 0.5f, false, 21);
            for (int k = 0; k < numIntervals; ++k)
            {
                auto p = paramsFor (k >= 15 && k < 22 ? ChannelLayout::Surround51 : ChannelLayout::Surround71, 0.1f * static_cast<float> (k % 7));
                p.levelMatch = matched && k % 9 != 8;
                p.foldHeadroom = matched && k % 11 != 10;
                const float ph = static_cast<float> (k) * 0.7f;
                p.frontAngleDeg = 33.5f + 11.5f * std::sin (ph);
                p.sideAngleDeg = 100.0f + 20.0f * std::sin (1.3f * ph);
                p.rearAngleDeg = 142.5f + 22.5f * std::cos (ph);
                p.headRadiusMm = 87.5f + 17.5f * std::sin (0.4f * ph);
                p.lfeGainDb = -5.0f + 15.0f * std::sin (0.9f * ph);
                v.setParams (p);
                const int start = k * interval;
                // Interval 30: a 3-channel host bus (LFE and surrounds missing).
                // The 20 ms LFE ramp started there is still running when the
                // LFE comes back.
                const int channels = k == 30 ? 3 : 8;
                if (seed == 0)
                {
                    v.process (buf.block (start, interval).firstChannels (channels));
                    continue;
                }
                int pos = start;
                for (int len : randomPartition (interval, 4096, seed * 977u + static_cast<uint32_t> (k)))
                {
                    v.process (buf.block (pos, len).firstChannels (channels));
                    pos += len;
                }
            }
            CHECK (allFinite (buf));
            if (ref.empty())
            {
                ref = { buf.ch[0], buf.ch[1] };
                continue;
            }
            CHECK (maxAbsDiff (buf.ch[0], ref[0]) == 0.0);
            CHECK (maxAbsDiff (buf.ch[1], ref[1]) == 0.0);
        }
    }
}

TEST_CASE ("HeadphoneVirtualizer [adversarial]: continuous angle automation on every block stays click-free")
{
    const int n = 48000;
    const auto input = sine (150.0, kFs, n, 0.5f);
    for (int channel : { FL, BL, SL })
    {
        HeadphoneVirtualizer v;
        auto p = paramsFor (ChannelLayout::Surround71, 0.3f);
        setUp (v, p);
        auto buf = monoSource (8, n, channel, input);
        const int start = 12000;
        for (int pos = 0; pos < n; pos += 32)
        {
            if (pos >= start)
            {
                // Full-range triangle-ish sweeps, pushed every 32 samples.
                const float ph = static_cast<float> (pos - start) / 6000.0f;
                p.frontAngleDeg = 33.5f + 11.5f * std::sin (6.0f * ph);
                p.sideAngleDeg = 100.0f + 20.0f * std::sin (5.0f * ph); // crosses 90 deg repeatedly
                p.rearAngleDeg = 142.5f + 22.5f * std::sin (7.0f * ph);
                p.headRadiusMm = 87.5f + 17.5f * std::sin (4.0f * ph);
                v.setParams (p);
            }
            v.process (buf.block (pos, 32));
        }
        for (int e = 0; e < 2; ++e)
        {
            const auto& y = buf.ch[static_cast<size_t> (e)];
            double steady = 0.0, moving = 0.0;
            for (int i = 4002; i < n; ++i)
            {
                const double d2 = std::abs (y[static_cast<size_t> (i)] - 2.0f * y[static_cast<size_t> (i - 1)] + y[static_cast<size_t> (i - 2)]);
                (i < start ? steady : moving) = std::max (i < start ? steady : moving, d2);
            }
            CHECK (steady > 0.0);
            CHECK_LE (moving, 2.0 * steady);
        }
    }
}

TEST_CASE ("HeadphoneVirtualizer [adversarial]: layout toggled every block never clicks and settles on the final layout")
{
    const int n = 24000;
    const auto input = sine (120.0, kFs, n, 0.5f);
    HeadphoneVirtualizer v;
    setUp (v, paramsFor (ChannelLayout::Surround71, 0.0f));
    auto buf = monoSource (8, n, FL, input);
    const int start = 6000, stop = 12000;
    for (int pos = 0; pos < n; pos += 48)
    {
        if (pos >= start && pos < stop)
            v.setParams (paramsFor ((pos / 48) % 3 == 0 ? ChannelLayout::Surround51 : ((pos / 48) % 3 == 1 ? ChannelLayout::Stereo : ChannelLayout::Surround71), 0.0f));
        if (pos == stop)
            v.setParams (paramsFor (ChannelLayout::Stereo, 0.0f));
        v.process (buf.block (pos, 48));
    }
    CHECK (allFinite (buf));
    double steady = 0.0, toggling = 0.0;
    for (int e = 0; e < 2; ++e)
        for (int i = 1; i < n; ++i)
        {
            const double d = std::abs (buf.ch[static_cast<size_t> (e)][static_cast<size_t> (i)] - buf.ch[static_cast<size_t> (e)][static_cast<size_t> (i - 1)]);
            (i < start ? steady : toggling) = std::max (i < start ? steady : toggling, d);
        }
    CHECK_LE (toggling, 1.5 * steady);

    // FL of 7.1 is ch 0 of stereo: after settling, identical to a stereo instance.
    HeadphoneVirtualizer ref;
    setUp (ref, paramsFor (ChannelLayout::Stereo, 0.0f));
    auto r = monoSource (8, n, FL, input);
    processInBlocks (ref, r, 48);
    CHECK_LE (maxAbsDiff (buf.ch[0], r.ch[0], n - 4000), 1e-5);
    CHECK_LE (maxAbsDiff (buf.ch[1], r.ch[1], n - 4000), 1e-5);
}

TEST_CASE ("HeadphoneVirtualizer [adversarial]: 7.1 fed only FL/FR equals the stereo layout exactly")
{
    const int n = 12000;
    HeadphoneVirtualizer a, b;
    setUp (a, paramsFor (ChannelLayout::Surround71, 0.4f), kFs, 512, 2);
    setUp (b, paramsFor (ChannelLayout::Stereo, 0.4f), kFs, 512, 2);
    auto x = noiseOnAll (2, n, 0.5f, false, 31);
    auto y = noiseOnAll (2, n, 0.5f, false, 31);
    processInBlocks (a, x, 256);
    processInBlocks (b, y, 256);
    CHECK (maxAbsDiff (x.ch[0], y.ch[0]) == 0.0);
    CHECK (maxAbsDiff (x.ch[1], y.ch[1]) == 0.0);

    // Mono host bus: (L + R) / 2 of the binaural pair.
    HeadphoneVirtualizer m;
    setUp (m, paramsFor (ChannelLayout::Stereo, 0.4f), kFs, 512, 1);
    Planar mono (1, n);
    mono.ch[0] = noiseOnAll (2, n, 0.5f, false, 31).ch[0];
    mono.ptrs[0] = mono.ch[0].data();
    HeadphoneVirtualizer s;
    setUp (s, paramsFor (ChannelLayout::Stereo, 0.4f), kFs, 512, 2);
    auto st = noiseOnAll (2, n, 0.5f, false, 31);
    std::fill (st.ch[1].begin(), st.ch[1].end(), 0.0f);
    processInBlocks (m, mono, 256);
    processInBlocks (s, st, 256);
    double err = 0.0;
    for (int i = 0; i < n; ++i)
        err = std::max (err, std::abs (0.5 * (st.ch[0][static_cast<size_t> (i)] + st.ch[1][static_cast<size_t> (i)]) - mono.ch[0][static_cast<size_t> (i)]));
    CHECK_LE (err, 1e-6);
}

TEST_CASE ("HeadphoneVirtualizer [adversarial]: a NaN / Inf input sample does not latch the module")
{
    // The chain drops non-finite blocks, but a module on its own must still
    // recover once finite input resumes (IIR state must not stay NaN).
    for (bool withHrir : { false, true })
        for (float bad : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() })
        {
            HeadphoneVirtualizer v;
            if (withHrir)
                v.setHrirSet (makeImpulseSet (makeImpulseSpec(), kFs, false));
            setUp (v, paramsFor (ChannelLayout::Surround71, 0.5f));
            const int n = 48000;
            auto buf = noiseOnAll (8, n, 0.3f, false, 41);
            for (int c = 0; c < 8; ++c)
                buf.ch[static_cast<size_t> (c)][1000] = bad;
            processInBlocks (v, buf, 256);
            // Everything after 0.25 s is finite and at a sane level.
            bool finite = true;
            for (int c = 0; c < 2; ++c)
                for (int i = 12000; i < n; ++i)
                    finite = finite && std::isfinite (buf.ch[static_cast<size_t> (c)][static_cast<size_t> (i)]);
            CHECK (finite);
            CHECK (rms (buf.ch[0].data() + 12000, n - 12000) > 0.01);
        }
}

TEST_CASE ("HeadphoneVirtualizer [adversarial]: re-prepare switches the renderer with the session rate")
{
    const ImpulseSpec spec = makeImpulseSpec();
    auto set = makeImpulseSet (spec, kFs, true);
    HeadphoneVirtualizer v;
    v.setHrirSet (set);
    const auto p = paramsFor (ChannelLayout::Surround71, 0.0f);
    v.setParams (p);

    const auto flImpulseAtZero = [&v]
    {
        Planar imp (8, 64);
        imp.ch[FL][0] = 1.0f;
        v.process (imp.block());
        return std::abs (imp.ch[0][0] - 0.125f * kTrim) < 1e-7f && imp.ch[0][1] == 0.0f;
    };

    v.prepare ({ kFs, 512, 8 });
    CHECK (flImpulseAtZero()); // HRIR at 48 kHz
    v.prepare ({ 44100.0, 512, 8 });
    CHECK (! flImpulseAtZero()); // mismatched rate: parametric
    v.prepare ({ kFs, 512, 8 });
    CHECK (flImpulseAtZero()); // HRIR again

    // An HRIR set handed over after prepare() only applies at the next prepare().
    HeadphoneVirtualizer w;
    w.setParams (p);
    w.prepare ({ kFs, 512, 8 });
    w.setHrirSet (set);
    Planar a (8, 64);
    a.ch[FL][0] = 1.0f;
    w.process (a.block());
    CHECK (a.ch[0][0] != 0.125f * kTrim);
}

TEST_CASE ("HeadphoneVirtualizer [adversarial]: absurd sample rates in prepare() do not hang or produce NaN")
{
    for (double fs : { 0.0, -1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), 1.0e12 })
    {
        HeadphoneVirtualizer v;
        v.setParams (paramsFor (ChannelLayout::Surround71, 0.5f));
        v.prepare ({ fs, 256, 8 });
        auto buf = noiseOnAll (8, 1024, 0.5f, false, 51);
        processInBlocks (v, buf, 256);
        CHECK (allFinite (buf));
    }
}

TEST_CASE ("HeadphoneVirtualizer [adversarial]: tiny inputs decay to exact zero at every rate (no subnormal crawl)")
{
    for (double fs : { 44100.0, 192000.0 })
    {
        HeadphoneVirtualizer v;
        setUp (v, paramsFor (ChannelLayout::Surround71, 1.0f), fs, 4096);
        const int n = static_cast<int> (fs);
        auto buf = noiseOnAll (8, n, 1.0e-20f, false, 61);
        for (auto& c : buf.ch)
            std::fill (c.begin() + n / 4, c.end(), 0.0f);
        processInBlocks (v, buf, 4096);
        for (int e = 0; e < 2; ++e)
            CHECK (peakAbs (buf.ch[static_cast<size_t> (e)].data() + n - 4096, 4096) == 0.0);
    }
}

TEST_CASE ("HeadphoneVirtualizer [adversarial]: switching renderer (HRIR 7.1 -> parametric stereo) is click-free")
{
    const int n = 24000, change = 9600;
    const auto input = sine (200.0, kFs, n, 0.5f);
    HeadphoneVirtualizer v;
    v.setHrirSet (makeImpulseSet (makeImpulseSpec(), kFs, false));
    setUp (v, paramsFor (ChannelLayout::Surround71, 0.5f));
    auto buf = monoSource (8, n, FL, input);
    for (int pos = 0; pos < n; pos += 64)
    {
        if (pos == change)
            v.setParams (paramsFor (ChannelLayout::Stereo, 0.5f));
        v.process (buf.block (pos, 64));
    }
    CHECK (allFinite (buf));
    for (int e = 0; e < 2; ++e)
    {
        const auto& y = buf.ch[static_cast<size_t> (e)];
        double before = 0.0, during = 0.0, after = 0.0;
        for (int i = 4001; i < n; ++i)
        {
            const double d = std::abs (y[static_cast<size_t> (i)] - y[static_cast<size_t> (i - 1)]);
            double& slot = i < change ? before : (i < change + 2400 ? during : after);
            slot = std::max (slot, d);
        }
        CHECK (before > 0.0 && after > 0.0);
        CHECK_LE (during, 1.5 * std::max (before, after));
    }
}

//==============================================================================
// Level match and fold headroom (docs/11 E28a).
namespace
{
/** K-weighted power (BS.1770 filters, channels 0 and 1 summed) from `from`
    on, in dB: the integrated loudness of stationary content + 0.691. */
double kPowerDb (const Planar& buf, int from)
{
    const auto s1 = LoudnessMeter::kWeightingStage1 (kFs), s2 = LoudnessMeter::kWeightingStage2 (kFs);
    double e = 0.0;
    const int n = static_cast<int> (buf.ch[0].size());
    for (size_t c = 0; c < 2; ++c)
    {
        BiquadState a, b;
        for (int i = 0; i < n; ++i)
        {
            const double y = biquadTick (s2, b, biquadTick (s1, a, static_cast<double> (buf.ch[c][static_cast<size_t> (i)])));
            if (i >= from)
                e += y * y;
        }
    }
    return 10.0 * std::log10 (std::max (1.0e-30, e / (n - from)));
}

/** Pink noise on every speaker of a layout (LFE silent): one signal on all
    (correlated) or one per speaker, each at rmsLevel. */
Planar pinkOnSpeakers (ChannelLayout layout, int n, bool correlated, float rmsLevel = 0.0316f)
{
    const int channels = channelCount (layout);
    Planar buf (channels, n);
    for (int c = 0; c < channels; ++c)
        if (c != LFE)
            fill (buf, c, pinkNoise (n, rmsLevel, correlated ? 4242u : 5000u + static_cast<uint32_t> (c)));
    return buf;
}

/** The chain's alternative with virt off: 0.7071 x the BS.775 matrix. */
Planar downmixOf (const Planar& in)
{
    Planar d = in;
    Bs775Fold fold;
    fold.prepare (kFs, 0.0f);
    fold.process (d.block (0, static_cast<int> (d.ch[0].size())), Bs775Fold::kMatrixGain);
    return d;
}

VirtualizerParams matchedFor (ChannelLayout layout, float room = 0.15f)
{
    auto p = paramsFor (layout, room);
    p.levelMatch = true;
    p.foldHeadroom = true;
    return p;
}

double largestStep (const std::vector<float>& y, int from, int to)
{
    double m = 0.0;
    for (int i = std::max (1, from); i < to; ++i)
        m = std::max (m, static_cast<double> (std::abs (y[static_cast<size_t> (i)] - y[static_cast<size_t> (i - 1)])));
    return m;
}
} // namespace

TEST_CASE ("HeadphoneVirtualizer: level match - 5.1 and 7.1 pink, correlated or not, lands within 0.5 LU of the BS.775 downmix, also for an HRIR set (docs/11 E28a)")
{
    // docs/11 E28a Done-when: virt on vs off within 1 LU for correlated and
    // uncorrelated 7.1 and 5.1 pink. -30 dBFS RMS pink on every speaker,
    // room 0.15 (the default); loudness over 3..6 s (the servo averages 3 s).
    // Before: the renderer with the fixed -3 dB trim only.
    struct Case
    {
        ChannelLayout layout;
        bool correlated, hrir;
        double before;
    };
    const Case cases[] = {
        { ChannelLayout::Surround51, true, false, 0.81 }, { ChannelLayout::Surround51, false, false, 3.63 },
        { ChannelLayout::Surround71, true, false, 1.39 }, { ChannelLayout::Surround71, false, false, 3.83 },
        { ChannelLayout::Surround71, true, true, -2.98 }, { ChannelLayout::Surround71, false, true, 0.71 },
    };
    const int n = static_cast<int> (6.0 * kFs), from = static_cast<int> (3.0 * kFs);
    for (const auto& k : cases)
    {
        const Planar in = pinkOnSpeakers (k.layout, n, k.correlated);
        const double ref = kPowerDb (downmixOf (in), from);
        double diff[2] {};
        for (bool match : { false, true })
        {
            HeadphoneVirtualizer v;
            if (k.hrir)
                v.setHrirSet (makeImpulseSet (makeImpulseSpec(), kFs, false));
            auto p = matchedFor (k.layout);
            p.levelMatch = p.foldHeadroom = match;
            setUp (v, p, kFs, 512, channelCount (k.layout));
            Planar out = in;
            processInBlocks (v, out, 512);
            diff[match ? 1 : 0] = kPowerDb (out, from) - ref;
            if (match)
            {
                // The make-up stays within +-4 dB of the diffuse-field gain.
                CHECK_LE (std::abs (v.getMakeupDb() - v.getDiffuseMakeupDb()), 4.0f + 1e-4f);
                std::cout << "    measured " << (k.layout == ChannelLayout::Surround51 ? "5.1" : "7.1") << (k.hrir ? " HRIR" : "")
                          << (k.correlated ? " correlated" : " uncorrelated") << ": virt re downmix " << diff[0] << " -> " << diff[1]
                          << " LU (diffuse " << v.getDiffuseMakeupDb() << " dB, make-up " << v.getMakeupDb() << " dB)\n";
            }
        }
        CHECK_NEAR (diff[0], k.before, 0.3);
        CHECK_LE (std::abs (diff[1]), 0.5);
    }
}

TEST_CASE ("HeadphoneVirtualizer: level match - diffuse-field gain per layout; the make-up moves at most 6 dB/s, stops 4 dB from it, and silence freezes it (docs/11 E28a)")
{
    // The diffuse-field gains of the default design (room 0.15).
    for (auto [layout, db] : { std::pair { ChannelLayout::Stereo, -2.87f }, std::pair { ChannelLayout::Surround51, -3.84f },
                               std::pair { ChannelLayout::Surround71, -4.03f } })
    {
        HeadphoneVirtualizer v;
        setUp (v, matchedFor (layout), kFs, 512, channelCount (layout));
        std::cout << "    measured diffuse-field gain " << channelCount (layout) << " ch: " << v.getDiffuseMakeupDb() << " dB\n";
        CHECK_NEAR (v.getDiffuseMakeupDb(), db, 0.05);
        CHECK_NEAR (v.getMakeupDb(), v.getDiffuseMakeupDb(), 1e-4); // the starting point
    }

    // FL = -FR at 100 Hz: the downmix keeps both, the two ears nearly cancel
    // it (both speakers reach each ear at about unity), so the servo asks for
    // far more than +4 dB: it slews there at 6 dB/s and stops at the clamp.
    // Then 2 s of silence: the make-up does not move.
    const int n = static_cast<int> (3.0 * kFs), quiet = static_cast<int> (2.0 * kFs);
    HeadphoneVirtualizer v;
    setUp (v, matchedFor (ChannelLayout::Surround71));
    const float diffuse = v.getDiffuseMakeupDb();
    Planar buf (8, n + quiet);
    const auto x = sine (100.0, kFs, n, 0.1f);
    for (int i = 0; i < n; ++i)
    {
        buf.ch[0][static_cast<size_t> (i)] = x[static_cast<size_t> (i)];
        buf.ch[1][static_cast<size_t> (i)] = -x[static_cast<size_t> (i)];
    }
    float prev = v.getMakeupDb(), fastest = 0.0f;
    for (int pos = 0; pos < n + quiet; pos += 64)
    {
        v.process (buf.block (pos, 64));
        fastest = std::max (fastest, std::abs (v.getMakeupDb() - prev));
        prev = v.getMakeupDb();
        if (pos + 64 == n)
        {
            CHECK_NEAR (v.getMakeupDb(), diffuse + 4.0f, 1e-3);
        }
    }
    std::cout << "    measured largest make-up change per 64 samples: " << fastest << " dB\n";
    CHECK_LE (fastest, 6.0f * 64.0f / 48000.0f + 1e-3f);
    CHECK_NEAR (v.getMakeupDb(), diffuse + 4.0f, 1e-3); // silence froze it
}

TEST_CASE ("HeadphoneVirtualizer: level match - reset() keeps what it learned, a layout change starts over at the new diffuse-field gain (docs/11 E28a)")
{
    const int n = static_cast<int> (2.0 * kFs);
    HeadphoneVirtualizer v;
    setUp (v, matchedFor (ChannelLayout::Surround71));
    Planar in = pinkOnSpeakers (ChannelLayout::Surround71, n, true);
    processInBlocks (v, in, 512);
    const float learned = v.getMakeupDb();
    CHECK (std::abs (learned - v.getDiffuseMakeupDb()) > 1.0f); // correlated content needs about 2.6 dB more
    v.reset(); // what the chain does whenever virt comes back on
    CHECK_NEAR (v.getMakeupDb(), learned, 1e-6);

    // 5.1: the swap (fade out, swap at silence) re-seeds at the 5.1 gain.
    v.setParams (matchedFor (ChannelLayout::Surround51));
    Planar silence (8, 2048);
    processInBlocks (v, silence, 64);
    CHECK_NEAR (v.getMakeupDb(), v.getDiffuseMakeupDb(), 1e-4);
    CHECK_NEAR (v.getDiffuseMakeupDb(), -3.84f, 0.05);
}

TEST_CASE ("HeadphoneVirtualizer: fold headroom - full-scale correlated 5.1 / 7.1 stays at 0 dBFS (true peak <= +1 dBTP), content below 0 dBFS is untouched (docs/11 E28a)")
{
    // docs/11 E28a Done-when: pre-limiter peak of full-scale correlated
    // 7-speaker content <= +1 dBFS. Pink noise normalised to a 0 dBFS peak on
    // every speaker, level match on; peaks after the first 0.5 s.
    const int n = static_cast<int> (3.0 * kFs), from = static_cast<int> (0.5 * kFs);
    for (auto [layout, before] : { std::pair { ChannelLayout::Surround51, 7.81 }, std::pair { ChannelLayout::Surround71, 10.11 } })
    {
        Planar in = pinkOnSpeakers (layout, n, true, 0.2f);
        const double scale = 1.0 / peakAbs (in.ch[0].data(), n);
        for (auto& c : in.ch)
            for (auto& s : c)
                s = static_cast<float> (s * scale);
        double peak[2] {}, truePeak = 0.0;
        for (bool headroom : { false, true })
        {
            HeadphoneVirtualizer v;
            auto p = matchedFor (layout);
            p.foldHeadroom = headroom;
            setUp (v, p, kFs, 512, channelCount (layout));
            Planar out = in;
            processInBlocks (v, out, 512);
            CHECK (allFinite (out));
            for (size_t e = 0; e < 2; ++e)
                peak[headroom ? 1 : 0] = std::max (peak[headroom ? 1 : 0], peakAbs (out.ch[e].data() + from, n - from));
            if (headroom)
            {
                TruePeakDetector tp;
                tp.prepare (2);
                for (int e = 0; e < 2; ++e)
                    for (int i = 0; i < n; ++i)
                    {
                        const float y = tp.processSample (e, out.ch[static_cast<size_t> (e)][static_cast<size_t> (i)]);
                        if (i >= from)
                            truePeak = std::max (truePeak, static_cast<double> (std::abs (y)));
                    }
                CHECK_LE (v.getHeadroomGainDb(), -3.0f);
            }
        }
        std::cout << "    measured full-scale correlated " << channelCount (layout) << " ch: peak " << toDb (peak[0]) << " -> "
                  << toDb (peak[1]) << " dBFS, true peak " << toDb (truePeak) << " dBTP\n";
        CHECK_NEAR (toDb (peak[0]), before, 0.3);
        CHECK_LE (peak[1], 1.0 + 1e-6);
        CHECK_LE (toDb (truePeak), 1.0);
    }

    // -34 dBFS RMS: the headroom gain never moves, the output is bit-identical.
    const int m = static_cast<int> (1.0 * kFs);
    const Planar in = pinkOnSpeakers (ChannelLayout::Surround71, m, true, 0.02f);
    std::vector<std::vector<float>> outs;
    for (bool headroom : { false, true })
    {
        HeadphoneVirtualizer v;
        auto p = matchedFor (ChannelLayout::Surround71);
        p.foldHeadroom = headroom;
        setUp (v, p);
        Planar out = in;
        processInBlocks (v, out, 256);
        CHECK (v.getHeadroomGainDb() == 0.0f);
        outs.push_back (out.ch[0]);
    }
    CHECK (peakAbs (outs[0].data(), m) < 1.0);
    CHECK (maxAbsDiff (outs[0], outs[1]) == 0.0);
}

TEST_CASE ("HeadphoneVirtualizer: switching the level match and the fold headroom on and off is click-free (docs/11 E28a)")
{
    // A 200 Hz sine on every speaker (correlated). Level match: off at 1 s,
    // on at 2 s: the make-up glides at 6 dB/s. Fold headroom on a 0.5 sine
    // (about +7 dBFS without it): off at 1 s, on at 2 s: it releases over
    // 150 ms, and when it comes back its first gain drop is a single
    // flattened rising edge. The largest sample step around each switch
    // stays within 1.25x the steady maximum of the louder side.
    const int n = static_cast<int> (3.0 * kFs), off = static_cast<int> (1.0 * kFs), on = static_cast<int> (2.0 * kFs);
    for (bool headroomCase : { false, true })
    {
        const float amp = headroomCase ? 0.5f : 0.1f;
        Planar buf (8, n);
        const auto x = sine (200.0, kFs, n, amp);
        for (int c = 0; c < 8; ++c)
            if (c != LFE)
                fill (buf, c, x);
        HeadphoneVirtualizer v;
        auto p = matchedFor (ChannelLayout::Surround71);
        setUp (v, p);
        for (int pos = 0; pos < n; pos += 128)
        {
            if (pos == off || pos == on)
            {
                (headroomCase ? p.foldHeadroom : p.levelMatch) = pos == on;
                v.setParams (p);
            }
            v.process (buf.block (pos, 128));
        }
        CHECK (allFinite (buf));
        for (size_t e = 0; e < 2; ++e)
        {
            const auto& y = buf.ch[e];
            const double steadyOn = largestStep (y, off - 9600, off), steadyOff = largestStep (y, on - 9600, on);
            const double atOff = largestStep (y, off, off + 9600), atOn = largestStep (y, on, on + 9600);
            std::cout << "    measured " << (headroomCase ? "fold headroom" : "level match") << " ear " << e << ": steady steps "
                      << steadyOn << " / " << steadyOff << ", at the switches " << atOff << " / " << atOn << "\n";
            CHECK_LE (atOff, 1.25 * std::max (steadyOn, steadyOff));
            CHECK_LE (atOn, 1.25 * std::max (steadyOn, steadyOff));
        }
    }
}
