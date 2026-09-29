// The shared surround fold (Bs775Fold / LfeFold, docs/11 E01) and the
// input-channel detector (ActiveChannelDetector, docs/11 E27), as modules.
// The chain-level behaviour (crossfades, overrides, presets) is in
// tests/test_engine.cpp; the 7.1 LFE and 8-channel stereo metrics in
// tests/test_known_gaps.cpp. The BS.775 fold's headroom in the chain and the
// surround folds' gains on MeterBus and in render.stats (docs/11 E28a) are
// at the end.
#include "TestFramework.h"
#include "TestSignals.h"

#include "Commands.h"
#include "OfflineRenderer.h"

#include "flub/analysis/LoudnessMeter.h"
#include "flub/common/Denormals.h"
#include "flub/common/Math.h"
#include "flub/dsp/ActiveChannelDetector.h"
#include "flub/dsp/Bs775Fold.h"
#include "flub/dsp/HeadphoneVirtualizer.h"
#include "flub/dsp/TruePeakDetector.h"
#include "flub/engine/Parameters.h"
#include "flub/engine/ProcessingChain.h"

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

//==============================================================================
// Fold headroom of the BS.775 downmix (virt off) and the fold gains on
// MeterBus / in render.stats (docs/11 E28a remainder).
namespace
{
using namespace flub::param;

/** Pink noise with a 0 dBFS sample peak, the same on every main speaker
    (full-scale correlated, docs/11 E28a Done-when); LFE silent. */
Planar fullScaleCorrelated (int channels, int n, float peak = 1.0f)
{
    auto x = pinkNoise (n, 0.2f, 4242u);
    const double scale = peak / peakAbs (x.data(), n);
    for (auto& v : x)
        v = static_cast<float> (v * scale);
    Planar p (channels, n);
    for (int c = 0; c < channels; ++c)
        if (c != kLfe)
            std::copy (x.begin(), x.end(), p.ch[static_cast<size_t> (c)].begin());
    return p;
}

/** A chain with every module off (the fold alone reaches the output). */
struct FoldChain
{
    ParameterStore store;
    ProcessingChain chain { store };

    FoldChain (int channels, bool virt)
    {
        for (int id : { GateOn, EqOn, DynEqOn, BassOn, ClarityOn, SaturationOn, SpatialOn, CompressorOn, MaximizerOn })
            store.set (id, 0.0f);
        store.set (VirtualizerOn, virt ? 1.0f : 0.0f);
        chain.prepare ({ kFs, 512, channels });
    }

    /** Runs buf in blocks; after each block, `watch (end position)`. */
    template <typename Watch>
    void run (Planar& buf, int block, Watch&& watch)
    {
        ScopedNoDenormals noDenormals;
        const int n = buf.numSamples();
        for (int pos = 0; pos < n; pos += block)
        {
            chain.process (buf.block (pos, std::min (block, n - pos)));
            watch (std::min (n, pos + block));
        }
    }
    void run (Planar& buf, int block = 512)
    {
        run (buf, block, [] (int) {});
    }
};

/** K-weighted power of channels 0 and 1 from `from` on (dB; the integrated
    loudness of stationary content + 0.691). */
double kPowerDb (const Planar& buf, int from)
{
    const auto s1 = LoudnessMeter::kWeightingStage1 (kFs), s2 = LoudnessMeter::kWeightingStage2 (kFs);
    double e = 0.0;
    const int n = buf.numSamples();
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

bool allFiniteIn (const Planar& buf)
{
    for (const auto& c : buf.ch)
        for (float v : c)
            if (! std::isfinite (v))
                return false;
    return true;
}

double largestStepIn (const std::vector<float>& y, int from, int to)
{
    double m = 0.0;
    for (int i = std::max (1, from); i < to; ++i)
        m = std::max (m, static_cast<double> (std::abs (y[static_cast<size_t> (i)] - y[static_cast<size_t> (i - 1)])));
    return m;
}

std::vector<float> defaultValues()
{
    std::vector<float> v;
    for (const auto& info : layout())
        v.push_back (info.defaultValue);
    return v;
}
} // namespace

TEST_CASE ("FoldHeadroom: an over drops the gain at once to exactly 0 dBFS, holds 10 ms, releases over 150 ms; below 0 dBFS it is the identity (docs/11 E28a)")
{
    FoldHeadroom h;
    h.prepare (kFs);
    const int n = 60000, over = 1000;
    std::vector<float> l (static_cast<size_t> (n), 0.5f), r (static_cast<size_t> (n), -0.25f);
    l[static_cast<size_t> (over)] = 2.0f; // a +6 dBFS sample
    const auto l0 = l, r0 = r;
    for (int pos = 0; pos < n; pos += 333) // any partition
        h.process (l.data() + pos, r.data() + pos, std::min (333, n - pos));
    for (int i = 0; i < over; ++i)
        CHECK (l[static_cast<size_t> (i)] == l0[static_cast<size_t> (i)]); // bit-identical below 0 dBFS
    CHECK (l[static_cast<size_t> (over)] == 1.0f);                         // exactly the ceiling
    CHECK (r[static_cast<size_t> (over)] == -0.125f);                      // linked
    const int hold = msToSamples (FoldHeadroom::kHoldMs, kFs);
    CHECK (l[static_cast<size_t> (over + hold)] == 0.25f); // held at 0.5 for 10 ms
    // Then the one-pole release: 1 - 0.5 e^(-t / 150 ms).
    const int t = msToSamples (FoldHeadroom::kReleaseMs, kFs);
    CHECK_NEAR (l[static_cast<size_t> (over + hold + t)] / 0.5, 1.0 - 0.5 * std::exp (-1.0), 2e-3);
    CHECK_NEAR (h.getGainDb(), 0.0, 0.01); // 1.2 s later: recovered
    h.reset();
    CHECK (h.getGain() == 1.0f);
}

TEST_CASE ("Chain (docs/11 E28a): the BS.775 fold (virt off) holds full-scale correlated 5.1 / 7.1 at 0 dBFS before the limiter; below 0 dBFS it is untouched")
{
    // Done-when: the pre-limiter peak of full-scale correlated 7.1 with virt
    // off <= +1 dBFS (was +6.88: 0.7071 (1 + 3 x 0.7071) on 7.1, 0.7071
    // (1 + 2 x 0.7071) = +4.65 dBFS on 5.1). Every module off, so the chain's
    // output is its fold; its peaks after the first 0.5 s. The sample peak is
    // the Done-when; the true peak (no look-ahead, like the virtualiser's)
    // is pinned, the maximizer's true-peak limiter takes the rest when on.
    const int n = static_cast<int> (2.0 * kFs), from = static_cast<int> (0.5 * kFs);
    for (auto [channels, before] : { std::pair { 6, 4.65 }, std::pair { 8, 6.88 } })
    {
        const Planar in = fullScaleCorrelated (channels, n);
        Planar raw = in;
        Bs775Fold fold;
        fold.prepare (kFs, 0.0f);
        fold.process (raw.block (0, n), Bs775Fold::kMatrixGain);
        const double rawPeak = std::max (peakAbs (raw.ch[0].data(), n), peakAbs (raw.ch[1].data(), n));

        FoldChain fc (channels, false);
        Planar out = in;
        float deepest = 0.0f, makeup = 0.0f;
        fc.run (out, 512, [&] (int) {
            deepest = std::min (deepest, fc.chain.meters().foldHeadroomDb.load());
            makeup = std::max (makeup, std::abs (fc.chain.meters().virtMakeupDb.load()));
        });
        double peak = 0.0, truePeak = 0.0;
        TruePeakDetector tp;
        tp.prepare (2);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
            {
                const float y = out.ch[static_cast<size_t> (c)][static_cast<size_t> (i)];
                const float t = tp.processSample (c, y);
                if (i >= from)
                {
                    peak = std::max (peak, static_cast<double> (std::abs (y)));
                    truePeak = std::max (truePeak, static_cast<double> (std::abs (t)));
                }
            }
        std::cout << "    measured full-scale correlated " << channels << " ch, virt off: pre-limiter peak " << toDb (rawPeak) << " -> "
                  << toDb (peak) << " dBFS, true peak " << toDb (truePeak) << " dBTP, deepest fold headroom " << deepest << " dB\n";
        CHECK_NEAR (toDb (rawPeak), before, 0.05);
        CHECK_LE (peak, 1.0 + 1e-6);
        CHECK_LE (toDb (truePeak), 1.5);
        CHECK_LE (deepest, -(before - 1.0)); // the meter shows the headroom gain
        CHECK (makeup == 0.0f);              // the virtualiser does not run
    }

    // Below 0 dBFS (a -6 dBFS peak on every speaker: the fold peaks at
    // +0.9 dBFS without the headroom on 7.1, so -8 dBFS here): the headroom
    // never moves and the output is the fold of the input, bit for bit.
    const Planar in = fullScaleCorrelated (8, n, 0.35f);
    Planar ref = in;
    Bs775Fold fold;
    fold.prepare (kFs, LfeFold::gainFor (true, layout()[static_cast<size_t> (VirtLfeGainDb)].defaultValue));
    fold.process (ref.block (0, n), Bs775Fold::kMatrixGain);
    FoldChain fc (8, false);
    Planar out = in;
    float deepest = 0.0f;
    fc.run (out, 512, [&] (int) { deepest = std::min (deepest, fc.chain.meters().foldHeadroomDb.load()); });
    CHECK (deepest == 0.0f);
    const int latency = fc.chain.getLatencySamples();
    double err = 0.0;
    for (size_t c = 0; c < 2; ++c)
        for (int i = 0; i + latency < n; ++i)
            err = std::max (err, static_cast<double> (std::abs (out.ch[c][static_cast<size_t> (i + latency)] - ref.ch[c][static_cast<size_t> (i)])));
    std::cout << "    measured below 0 dBFS: largest difference to the plain fold " << err << "\n";
    CHECK (err == 0.0);
}

TEST_CASE ("Chain (docs/11 E28a): on full-scale correlated 5.1 / 7.1 overs, virt on vs off 3.8 / 5.6 -> 2.1 / 2.2 LU")
{
    // The unit's Done-when asks for 1 LU on those overs; not met. Every module
    // off, K-weighted power over 3..6 s (the level match averages 3 s).
    // Before: the BS.775 fold passed its overs unlimited, the virtualiser
    // held its own at 0 dBFS. Now both hold 0 dBFS by the same law, but at
    // the same loudness (the level match) the binaural render peaks 3.1-3.2
    // dB higher than the downmix (+10.05 / +7.76 against +6.88 / +4.65 dBFS),
    // so the same ceiling takes about 2 LU more from it. Closing that needs a
    // loudness-linked law across both folds (docs/11 E28, open).
    const int n = static_cast<int> (6.0 * kFs), from = static_cast<int> (3.0 * kFs);
    for (const int channels : { 6, 8 })
    {
        const Planar in = fullScaleCorrelated (channels, n);
        Planar raw = in;
        Bs775Fold fold;
        fold.prepare (kFs, 0.0f);
        fold.process (raw.block (0, n), Bs775Fold::kMatrixGain);
        double level[2] {};
        for (bool virt : { false, true })
        {
            FoldChain fc (channels, virt);
            Planar out = in;
            fc.run (out);
            level[virt ? 1 : 0] = kPowerDb (out, from);
        }
        const double before = level[1] - kPowerDb (raw, from), after = level[1] - level[0];
        std::cout << "    measured full-scale correlated " << channels << " ch: virt on - off " << before << " -> " << after << " LU\n";
        CHECK_NEAR (before, channels == 8 ? -5.60 : -3.76, 0.3);
        CHECK_NEAR (after, channels == 8 ? -2.21 : -2.13, 0.3);
    }
}

TEST_CASE ("Chain (docs/11 E28a): virt on / off and the stereo passthrough crossfade without a click while the fold headroom holds overs")
{
    // A 200 Hz sine at 0 dBFS on every main speaker (the folds reach +6.9 /
    // +10 dBFS before their headroom), 7.1, every module off. virt.on off at
    // 1 s and on at 2 s (20 ms crossfades); then virt off with virt.input
    // Force Stereo at 1 s and Force Surround at 2 s (400 ms). The largest
    // sample step around each switch stays within 1.25x the steady maximum of
    // the louder side (before: the crossfades mixed the fold at unity, so the
    // step was set by the unlimited +6.9 dBFS downmix).
    const int n = static_cast<int> (3.0 * kFs), off = static_cast<int> (1.0 * kFs), on = static_cast<int> (2.0 * kFs);
    for (bool passCase : { false, true })
    {
        Planar buf (8, n);
        const auto x = sine (200.0, kFs, n, 1.0f);
        for (int c = 0; c < 8; ++c)
            if (c != kLfe)
                std::copy (x.begin(), x.end(), buf.ch[static_cast<size_t> (c)].begin());
        FoldChain fc (8, ! passCase);
        fc.store.set (VirtInputMode, static_cast<float> (InputModeValue::ForceSurround));
        fc.run (buf, 128, [&] (int end) {
            if (end == off)
            {
                if (passCase)
                    fc.store.set (VirtInputMode, static_cast<float> (InputModeValue::ForceStereo));
                else
                    fc.store.set (VirtualizerOn, 0.0f);
            }
            if (end == on)
            {
                if (passCase)
                    fc.store.set (VirtInputMode, static_cast<float> (InputModeValue::ForceSurround));
                else
                    fc.store.set (VirtualizerOn, 1.0f);
            }
        });
        CHECK (allFiniteIn (buf));
        const int latency = fc.chain.getLatencySamples(), span = passCase ? 24000 : 9600;
        for (size_t e = 0; e < 2; ++e)
        {
            const auto& y = buf.ch[e];
            const double steadyA = largestStepIn (y, off + latency - 9600, off + latency), steadyB = largestStepIn (y, on + latency - 9600, on + latency);
            const double atOff = largestStepIn (y, off + latency, off + latency + span), atOn = largestStepIn (y, on + latency, on + latency + span);
            std::cout << "    measured " << (passCase ? "passthrough" : "virt") << " ear " << e << ": steady steps " << steadyA << " / " << steadyB
                      << ", at the switches " << atOff << " / " << atOn << "\n";
            CHECK_LE (atOff, 1.25 * std::max (steadyA, steadyB));
            CHECK_LE (atOn, 1.25 * std::max (steadyA, steadyB));
        }
        if (! passCase)
        {
            // Both folds held at 0 dBFS: no over anywhere after the first period.
            CHECK_LE (peakAbs (buf.ch[0].data() + 480, n - 480), 1.0 + 1e-6);
            CHECK_LE (peakAbs (buf.ch[1].data() + 480, n - 480), 1.0 + 1e-6);
        }
    }
}

TEST_CASE ("CLI render.stats (docs/11 E28a): fold.virtMakeup* and fold.headroom* - the virtualiser's make-up and the fold headroom")
{
    // Full-scale correlated 7.1, every module off: virt off holds the BS.775
    // fold's overs, virt on the binaural render's (and learns a make-up
    // around the 7.1 diffuse-field gain, -4.03 dB); a stereo file has neither.
    const int n = static_cast<int> (3.0 * kFs);
    const Planar in = fullScaleCorrelated (8, n);
    io::AudioFileData file;
    file.sampleRate = kFs;
    file.numChannels = 8;
    file.channels = in.ch;
    auto values = defaultValues();
    for (int id : { GateOn, EqOn, DynEqOn, BassOn, ClarityOn, SaturationOn, SpatialOn, CompressorOn, MaximizerOn })
        values[static_cast<size_t> (id)] = 0.0f;
    const auto render = [&] (const io::AudioFileData& f, bool virt) {
        auto v = values;
        v[static_cast<size_t> (VirtualizerOn)] = virt ? 1.0f : 0.0f;
        cli::RenderSettings settings;
        cli::RenderResult rr;
        std::string error;
        REQUIRE (cli::renderFile (f, v, settings, rr, error));
        return cli::renderStatsToJson (rr.stats);
    };
    for (bool virt : { false, true })
    {
        const auto st = render (file, virt);
        const auto& fold = st["fold"];
        std::cout << "    measured 7.1 virt " << (virt ? "on" : "off") << ": headroom deepest " << fold["headroomMaxDb"].asNumber() << " dB, active "
                  << fold["headroomActivePercent"].asNumber() << " %, make-up " << fold["virtMakeupMinDb"].asNumber() << " .. "
                  << fold["virtMakeupMaxDb"].asNumber() << " dB (end " << fold["virtMakeupEndDb"].asNumber() << ")\n";
        CHECK_LE (fold["headroomMaxDb"].asNumber(), -3.0);
        CHECK_GE (fold["headroomActivePercent"].asNumber(), 50.0);
        if (virt)
        {
            CHECK_NEAR (fold["virtMakeupMinDb"].asNumber(), -4.03, 0.1); // it starts at the diffuse-field gain
            CHECK_GE (fold["virtMakeupMaxDb"].asNumber(), fold["virtMakeupEndDb"].asNumber());
            CHECK_LE (std::abs (fold["virtMakeupEndDb"].asNumber() + 4.03), 4.0 + 0.01); // within the servo's +-4 dB
            CHECK (fold["virtMakeupEndDb"].asNumber() > -4.0);                           // correlated content: more than the diffuse gain
        }
        else
        {
            for (const char* key : { "virtMakeupMinDb", "virtMakeupMaxDb", "virtMakeupEndDb" })
                CHECK (fold[key].asNumber (1.0) == 0.0);
        }
    }
    io::AudioFileData stereo;
    stereo.sampleRate = kFs;
    stereo.numChannels = 2;
    stereo.channels = { in.ch[0], in.ch[1] };
    const auto st = render (stereo, true);
    for (const char* key : { "virtMakeupMinDb", "virtMakeupMaxDb", "virtMakeupEndDb", "headroomMaxDb", "headroomActivePercent" })
        CHECK (st["fold"][key].asNumber (1.0) == 0.0);
}

