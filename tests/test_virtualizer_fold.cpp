// The shared surround fold (Bs775Fold / LfeFold, docs/11 E01) and the
// input-channel detector (ActiveChannelDetector, docs/11 E27), as modules.
// The chain-level behaviour (crossfades, overrides, presets) is in
// tests/test_engine.cpp; the 7.1 LFE and 8-channel stereo metrics in
// tests/test_known_gaps.cpp.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/common/Math.h"
#include "flub/dsp/ActiveChannelDetector.h"
#include "flub/dsp/Bs775Fold.h"
#include "flub/dsp/HeadphoneVirtualizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace flub;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;
constexpr int kLfe = 3;

/** The chain's v1 downmix (ProcessingChain::downmixToStereo before E01),
    verbatim: BS.775 with the LFE dropped, -3 dB overall. */
void v1Downmix (Planar& io, int n)
{
    constexpr float k = 0.70710678f;
    const int nch = static_cast<int> (io.ch.size());
    for (int i = 0; i < n; ++i)
    {
        const auto s = static_cast<size_t> (i);
        float lo = io.ch[0][s], ro = io.ch[1][s];
        if (nch > 2)
        {
            lo += k * io.ch[2][s];
            ro += k * io.ch[2][s];
        }
        for (int c = 4; c + 1 < nch; c += 2)
        {
            lo += k * io.ch[static_cast<size_t> (c)][s];
            ro += k * io.ch[static_cast<size_t> (c + 1)][s];
        }
        io.ch[0][s] = k * lo;
        io.ch[1][s] = k * ro;
    }
}

double maxDiff (const std::vector<float>& a, const std::vector<float>& b)
{
    double m = 0.0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        m = std::max (m, static_cast<double> (std::abs (a[i] - b[i])));
    return m;
}

Planar noiseOn (int channels, int n, uint32_t seed)
{
    Planar p (channels, n);
    for (int c = 0; c < channels; ++c)
    {
        const auto x = whiteNoise (n, 0.2f, seed + static_cast<uint32_t> (c));
        std::copy (x.begin(), x.end(), p.ch[static_cast<size_t> (c)].begin());
    }
    return p;
}

void foldInBlocks (Bs775Fold& fold, Planar& buf, int blockSize, float overall)
{
    const int n = buf.numSamples();
    for (int pos = 0; pos < n; pos += blockSize)
        fold.process (buf.block (pos, std::min (blockSize, n - pos)), overall);
}

/** Runs the detector over `seconds` of the given per-channel levels (peak
    amplitude of a sine per channel, 0 = silent) in 512-sample blocks; returns
    the first time (s) the fold was `target`, or -1. */
double runDetector (ActiveChannelDetector& d, const std::array<float, 8>& amps, double seconds, ActiveChannelDetector::Fold target, int64_t& t0)
{
    constexpr int kBlock = 512;
    Planar buf (8, kBlock);
    const auto blocks = static_cast<int> (std::ceil (seconds * kFs / kBlock));
    double first = -1.0;
    for (int b = 0; b < blocks; ++b, t0 += kBlock)
    {
        for (int c = 0; c < 8; ++c)
            for (int i = 0; i < kBlock; ++i)
                buf.ch[static_cast<size_t> (c)][static_cast<size_t> (i)] =
                    amps[static_cast<size_t> (c)] * static_cast<float> (std::sin (kTwoPi * (300.0 + 70.0 * c) * static_cast<double> (t0 + i) / kFs));
        d.process (buf.block());
        if (first < 0.0 && d.getFold() == target)
            first = static_cast<double> (b + 1) * kBlock / kFs;
    }
    return first;
}
} // namespace

TEST_CASE ("Bs775Fold: with the LFE fold off it is the v1 downmix bit for bit (5.1 and 7.1)")
{
    for (int channels : { 6, 8 })
    {
        Bs775Fold fold;
        fold.prepare (kFs, LfeFold::gainFor (false, 6.0f));
        auto a = noiseOn (channels, 4096, 11);
        auto b = a;
        foldInBlocks (fold, a, 333, Bs775Fold::kMatrixGain);
        v1Downmix (b, 4096);
        CHECK (maxDiff (a.ch[0], b.ch[0]) == 0.0);
        CHECK (maxDiff (a.ch[1], b.ch[1]) == 0.0);
        // Channels >= 2 are inputs only: the chain clears them later.
        CHECK (maxDiff (a.ch[3], b.ch[3]) == 0.0);
    }
}

TEST_CASE ("Bs775Fold: passthrough (overall 1) is the identity on FL/FR-only input, and the LFE sits at virt.lfe re one main")
{
    // FL/FR-only content through the unity fold: exactly the input.
    {
        Bs775Fold fold;
        fold.prepare (kFs, LfeFold::gainFor (true, 6.0f));
        auto a = noiseOn (8, 4096, 21);
        for (int c = 2; c < 8; ++c)
            std::fill (a.ch[static_cast<size_t> (c)].begin(), a.ch[static_cast<size_t> (c)].end(), 0.0f);
        const auto ref = a;
        foldInBlocks (fold, a, 512, 1.0f);
        CHECK (maxDiff (a.ch[0], ref.ch[0]) == 0.0);
        CHECK (maxDiff (a.ch[1], ref.ch[1]) == 0.0);
    }
    // 50 Hz on the LFE against the same tone on FL, both folds, several levels.
    for (float lfeDb : { -12.0f, 0.0f, 6.0f, 10.0f, 16.0f })
        for (float overall : { Bs775Fold::kMatrixGain, 1.0f })
        {
            const int n = 48000;
            auto level = [&] (int channel, double freq) {
                Bs775Fold fold;
                fold.prepare (kFs, LfeFold::gainFor (true, lfeDb));
                Planar p (8, n);
                const auto s = sine (freq, kFs, n, 0.25f);
                std::copy (s.begin(), s.end(), p.ch[static_cast<size_t> (channel)].begin());
                foldInBlocks (fold, p, 256, overall);
                CHECK (maxDiff (p.ch[0], p.ch[1]) == 0.0 || channel == 0);
                return toDb (toneAmplitude (p.ch[0].data() + n / 2, n / 2, freq, kFs));
            };
            CHECK_NEAR (level (kLfe, 50.0) - level (0, 50.0), lfeDb, 0.02);
            CHECK_LE (level (kLfe, 1000.0) - level (0, 1000.0), -60.0 + lfeDb); // 24 dB/oct above 120 Hz
        }
    // gainFor: off -> 0, NaN -> 0 dB, clamped to -20 .. +16 dB.
    CHECK (LfeFold::gainFor (false, 6.0f) == 0.0f);
    CHECK (LfeFold::gainFor (true, std::nanf ("")) == 1.0f);
    CHECK_NEAR (toDb (LfeFold::gainFor (true, 40.0f)), 16.0, 1e-4);
    CHECK_NEAR (toDb (LfeFold::gainFor (true, -40.0f)), -20.0, 1e-4);
}

TEST_CASE ("Bs775Fold: switching the LFE fold off and on ramps over 20 ms (no step) and is exactly silent in between")
{
    Bs775Fold fold;
    fold.prepare (kFs, LfeFold::gainFor (true, 6.0f));
    constexpr int kBlock = 256;
    const int n = kBlock * 200;
    Planar p (8, n);
    const auto s = sine (60.0, kFs, n, 0.25f);
    std::copy (s.begin(), s.end(), p.ch[kLfe].begin());
    const int off = kBlock * 60, on = kBlock * 120;
    AllocationGuard guard;
    for (int pos = 0; pos < n; pos += kBlock)
    {
        if (pos == off)
            fold.setLfeGain (LfeFold::gainFor (false, 6.0f));
        if (pos == on)
            fold.setLfeGain (LfeFold::gainFor (true, 6.0f));
        fold.process (p.block (pos, kBlock), Bs775Fold::kMatrixGain);
    }
    CHECK (guard.allocations() == 0);
    const auto& l = p.ch[0];
    double steadyStep = 0.0, switchStep = 0.0;
    for (int i = kBlock * 20; i < off; ++i)
        steadyStep = std::max (steadyStep, static_cast<double> (std::abs (l[static_cast<size_t> (i)] - l[static_cast<size_t> (i - 1)])));
    for (int i = off; i < n; ++i)
        switchStep = std::max (switchStep, static_cast<double> (std::abs (l[static_cast<size_t> (i)] - l[static_cast<size_t> (i - 1)])));
    // A 60 Hz sine's own slope bounds every step (plus the 20 ms ramp's share).
    CHECK_LE (switchStep, 1.2 * steadyStep);
    // 20 ms after switching off: exact silence until switched back on.
    CHECK (peakAbs (l.data() + off + 960, on - off - 960) == 0.0);
    // Back on: the full level again after the ramp.
    CHECK_NEAR (toDb (toneAmplitude (l.data() + on + 9600, 9600, 60.0, kFs)), toDb (toneAmplitude (l.data() + off - 9600, 9600, 60.0, kFs)), 0.01);
}

TEST_CASE ("HeadphoneVirtualizer: the LFE uses the shared LfeFold - up to +16 dB, and lfeOn = false fades it to exact silence")
{
    // virt.lfe widened to +16 dB (docs/11 E01): the module no longer clamps at +10.
    const int n = 48000;
    VirtualizerParams p;
    p.layout = ChannelLayout::Surround71;
    p.roomAmount = 0.0f;
    p.lfeGainDb = 16.0f;
    p.foldHeadroom = false; // the tone reaches +1 dBFS: the level itself, not the E28a headroom
    HeadphoneVirtualizer v;
    v.prepare ({ kFs, 256, 8 });
    v.setParams (p);
    v.reset();
    Planar buf (8, n);
    const auto s = sine (50.0, kFs, n, 0.25f);
    std::copy (s.begin(), s.end(), buf.ch[kLfe].begin());
    processInBlocks (v, buf, 256);
    CHECK_NEAR (toDb (toneAmplitude (buf.ch[0].data() + n / 2, n / 2, 50.0, kFs) / 0.25), -3.0 + 16.0, 0.1);

    // Off: a 20 ms ramp, then silence (no LFE rendered at all).
    p.lfeGainDb = 6.0f;
    HeadphoneVirtualizer w;
    w.prepare ({ kFs, 256, 8 });
    w.setParams (p);
    w.reset();
    Planar b2 (8, n);
    std::copy (s.begin(), s.end(), b2.ch[kLfe].begin());
    const int off = 256 * 47;
    for (int pos = 0; pos < n; pos += 256)
    {
        if (pos == off)
        {
            p.lfeOn = false;
            w.setParams (p);
        }
        w.process (b2.block (pos, std::min (256, n - pos)));
    }
    double steadyStep = 0.0, switchStep = 0.0;
    const auto& l = b2.ch[0];
    for (int i = 4800; i < off; ++i)
        steadyStep = std::max (steadyStep, static_cast<double> (std::abs (l[static_cast<size_t> (i)] - l[static_cast<size_t> (i - 1)])));
    for (int i = off; i < off + 2400; ++i)
        switchStep = std::max (switchStep, static_cast<double> (std::abs (l[static_cast<size_t> (i)] - l[static_cast<size_t> (i - 1)])));
    CHECK_LE (switchStep, 1.2 * steadyStep);
    CHECK (peakAbs (l.data() + off + 960, n - off - 960) == 0.0);
}

TEST_CASE ("ActiveChannelDetector: FL/FR-only content folds as stereo after 2 s; rear content confirms surround within 300 ms and latches it")
{
    ActiveChannelDetector d;
    d.prepare (kFs, 8);
    CHECK (d.getFold() == ActiveChannelDetector::Fold::Surround); // start: surround, unconfirmed
    CHECK (! d.isSurroundConfirmed());
    int64_t t = 0;
    const std::array<float, 8> stereoOnly { 0.1f, 0.1f, 0, 0, 0, 0, 0, 0 };
    const double toStereo = runDetector (d, stereoOnly, 3.0, ActiveChannelDetector::Fold::Stereo, t);
    CHECK_GE (toStereo, 2.0);
    CHECK_LE (toStereo, 2.0 + 512.0 / kFs + 1e-9);
    CHECK (d.getActiveMask() == 0x3u);

    // A rear sound 30 dB under the fronts: surround within 300 ms, confirmed.
    std::array<float, 8> rear = stereoOnly;
    rear[6] = 0.1f * dbToGain (-30.0f);
    const double toSurround = runDetector (d, rear, 1.0, ActiveChannelDetector::Fold::Surround, t);
    CHECK_GE (toSurround, 0.25);
    CHECK_LE (toSurround, 0.30);
    CHECK (d.isSurroundConfirmed());
    CHECK (d.getActiveMask() == ((1u << 6) | 0x3u));

    // 30 s with silent rears afterwards: stays surround (the latch).
    CHECK (runDetector (d, stereoOnly, 30.0, ActiveChannelDetector::Fold::Stereo, t) < 0.0);
    CHECK (d.getFold() == ActiveChannelDetector::Fold::Surround);

    // reset() (a new device or routed process) forgets the latch.
    d.reset();
    CHECK (! d.isSurroundConfirmed());
    CHECK (runDetector (d, stereoOnly, 2.1, ActiveChannelDetector::Fold::Stereo, t) > 0.0);
}

TEST_CASE ("ActiveChannelDetector: -45 dB ambience keeps the surround fold, the -50..-60 dB band holds it, silence holds the stereo run")
{
    int64_t t = 0;
    // Ambience at -45 dB re the fronts is surround content: never stereo.
    {
        ActiveChannelDetector d;
        d.prepare (kFs, 8);
        std::array<float, 8> amb { 0.1f, 0.1f, 0, 0, 0, 0, 0, 0 };
        amb[4] = amb[5] = 0.1f * dbToGain (-45.0f);
        CHECK (runDetector (d, amb, 5.0, ActiveChannelDetector::Fold::Stereo, t) < 0.0);
        CHECK (d.isSurroundConfirmed());
    }
    // -55 dB: neither evidence - surround, unconfirmed, for as long as it lasts.
    {
        ActiveChannelDetector d;
        d.prepare (kFs, 8);
        std::array<float, 8> faint { 0.1f, 0.1f, 0, 0, 0, 0, 0, 0 };
        faint[2] = 0.1f * dbToGain (-55.0f);
        CHECK (runDetector (d, faint, 5.0, ActiveChannelDetector::Fold::Stereo, t) < 0.0);
        CHECK (! d.isSurroundConfirmed());
    }
    // Digital silence neither advances nor resets the stereo run: 1 s of
    // stereo, 3 s of silence (the 50 ms follower's decay stays above the
    // -70 dBFS front floor for 0.54 s of it), then 0.6 s more of stereo
    // switches.
    {
        ActiveChannelDetector d;
        d.prepare (kFs, 8);
        const std::array<float, 8> stereoOnly { 0.1f, 0.1f, 0, 0, 0, 0, 0, 0 };
        const std::array<float, 8> silence {};
        CHECK (runDetector (d, stereoOnly, 1.0, ActiveChannelDetector::Fold::Stereo, t) < 0.0);
        CHECK (runDetector (d, silence, 3.0, ActiveChannelDetector::Fold::Stereo, t) < 0.0);
        CHECK (runDetector (d, stereoOnly, 0.6, ActiveChannelDetector::Fold::Stereo, t) > 0.0);
    }
    // 5.1: channels 2..5 are the rest; an LFE-only signal is surround content.
    {
        ActiveChannelDetector d;
        d.prepare (kFs, 6);
        std::array<float, 8> lfeOnly {};
        lfeOnly[kLfe] = 0.2f;
        CHECK (runDetector (d, lfeOnly, 1.0, ActiveChannelDetector::Fold::Stereo, t) < 0.0);
        CHECK (d.isSurroundConfirmed());
        CHECK (d.getActiveMask() == (1u << kLfe));
    }
}
