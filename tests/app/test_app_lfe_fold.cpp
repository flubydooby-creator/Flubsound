// App-level tests: docs/11 E01, the app's own BS.775 copies fold the LFE by
// the chain's law. DriftCompensatedFifo's surround-to-stereo downmix (a
// capture that delivers more channels than it asked for) and
// TestSignalGenerator's stereo downmix of its Game71 scene both dropped the
// LFE; both now take it through the chain's LfeFold (Bs775Fold.h) at
// virt.lfe's default, inside the fold's -3 dB, so their LFE-to-mains ratio
// is the chain's (Done-when: within 0.1 dB).
#include "AppTestSupport.h"
#include "TestSignals.h"

#include "engine/DriftCompensatedFifo.h"
#include "engine/TestSignalGenerator.h"

#include "flub/common/Math.h"
#include "flub/dsp/Bs775Fold.h"
#include "flub/engine/Parameters.h"

#include <array>
#include <cmath>
#include <iostream>
#include <vector>

using flub::app::DriftCompensatedFifo;
using flub::app::TestSignalGenerator;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;
constexpr int kLfe = 3;

float defaultLfeGain()
{
    return flub::LfeFold::gainFor (true, flub::param::layout()[static_cast<size_t> (flub::param::VirtLfeGainDb)].defaultValue);
}

/** The chain's fold (Bs775Fold, the surround fold's overall k) of a planar
    buffer, with the LFE at the default level or silent. */
Planar coreFold (const Planar& in, bool withLfe)
{
    Planar d = in;
    flub::Bs775Fold fold;
    fold.prepare (kFs, withLfe ? defaultLfeGain() : 0.0f);
    fold.process (d.block (0, d.numSamples()), flub::Bs775Fold::kMatrixGain);
    return d;
}

double rmsDiffDb (const std::vector<float>& a, const std::vector<float>& b, int from)
{
    double e = 0.0;
    for (size_t i = static_cast<size_t> (from); i < a.size(); ++i)
    {
        const double d = static_cast<double> (a[i]) - static_cast<double> (b[i]);
        e += d * d;
    }
    return 10.0 * std::log10 (std::max (1.0e-30, e / static_cast<double> (a.size() - static_cast<size_t> (from))));
}
} // namespace

TEST_CASE ("App: the capture FIFO's 5.1 / 7.1 downmix folds the LFE at the chain's LFE-to-mains ratio (E01)")
{
    // A 50 Hz tone at -12 dBFS on FL and FR, then the same tone on the LFE
    // alone, into a stereo FIFO in 480-frame pushes and pulls at the same
    // rate. The LFE-to-mains ratio on each side (the tone's amplitude over
    // the last 0.5 s of the pulls, LFE run over mains run) against the
    // chain's Bs775Fold of the same frames. Before: the LFE was dropped (-inf).
    constexpr int kPacket = 480;
    constexpr double kSeconds = 2.0;
    constexpr float kAmp = 0.25f;
    const int total = static_cast<int> (kSeconds * kFs), window = static_cast<int> (0.5 * kFs), from = total - window;
    const int source = total + 4 * kPacket; // the capture runs ahead of the pulls
    for (const int channels : { 6, 8 })
    {
        // Tone amplitude per side: {FIFO, chain fold} for one source layout.
        const auto amplitudes = [&] (bool lfe) {
            Planar src (channels, source);
            const auto tone = sine (50.0, kFs, source, kAmp);
            for (const int c : lfe ? std::vector<int> { kLfe } : std::vector<int> { 0, 1 })
                src.ch[static_cast<size_t> (c)] = tone;
            const Planar core = coreFold (src, true);

            DriftCompensatedFifo fifo;
            fifo.prepare (2, kFs, kFs, kPacket);
            std::vector<float> packet (static_cast<size_t> (kPacket * channels));
            Planar out (2, total);
            int64_t allocations = 0;
            int pushed = 0;
            for (int pos = 0; pos + kPacket <= total; pos += kPacket)
            {
                while (pushed < pos + 3 * kPacket) // the FIFO primes to two device blocks
                {
                    for (int i = 0; i < kPacket; ++i)
                        for (int c = 0; c < channels; ++c)
                            packet[static_cast<size_t> (i * channels + c)] = src.ch[static_cast<size_t> (c)][static_cast<size_t> (pushed + i)];
                    flubapptest::RealtimeProbe probe;
                    fifo.push (packet.data(), kPacket, channels);
                    allocations += probe.allocations();
                    pushed += kPacket;
                }
                const std::array<float*, 2> dest { out.ch[0].data() + pos, out.ch[1].data() + pos };
                fifo.pull (dest.data(), 2, kPacket, false);
            }
            CHECK (allocations == 0);
            CHECK (fifo.getStats().streaming);
            std::array<std::array<double, 2>, 2> a {};
            for (size_t side = 0; side < 2; ++side)
            {
                a[side][0] = toneAmplitude (out.ch[side].data() + from, window, 50.0, kFs);
                a[side][1] = toneAmplitude (core.ch[side].data() + from, window, 50.0, kFs);
            }
            return a;
        };
        const auto mains = amplitudes (false), lfe = amplitudes (true);
        for (size_t side = 0; side < 2; ++side)
        {
            const double fifoRatio = toDb (lfe[side][0] / mains[side][0]), coreRatio = toDb (lfe[side][1] / mains[side][1]);
            std::cout << "    measured " << (channels == 6 ? "5.1" : "7.1") << " FIFO downmix, side " << side << ": LFE re one main at 50 Hz "
                      << fifoRatio << " dB (chain fold " << coreRatio << " dB; before: dropped)\n";
            CHECK_NEAR (coreRatio, 10.0, 0.05); // +10 dB re one main (each side's), the low-pass flat at 50 Hz
            CHECK_NEAR (fifoRatio, coreRatio, 0.1);
        }
    }
}

TEST_CASE ("App: the test signal's stereo downmix of the Game71 scene is the chain's fold, LFE included (E01)")
{
    // The same Game71 scene (fixed seed, a new synth per setProgramme)
    // rendered on an 8-channel strip and on a stereo strip. The stereo render
    // must be the chain's Bs775Fold of the 8-channel one: its LFE share (the
    // fold with the LFE minus the fold without it, 4 s: one explosion at
    // 3.2 s) at the chain's level within 0.1 dB. Before: it had none.
    const int n = static_cast<int> (4.0 * kFs);
    Planar raw (8, n), stereo (2, n);
    TestSignalGenerator gen (kFs);
    gen.setProgramme (0, TestSignalGenerator::Programme::Game71);
    REQUIRE (gen.renderStrip (0, raw.block (0, n)));
    gen.setProgramme (0, TestSignalGenerator::Programme::Game71);
    REQUIRE (gen.renderStrip (0, stereo.block (0, n)));

    const Planar withLfe = coreFold (raw, true), noLfe = coreFold (raw, false);
    CHECK (peakAbs (raw.ch[kLfe].data(), n) > 0.05); // the scene has LFE content
    for (size_t side = 0; side < 2; ++side)
    {
        const double coreShare = rmsDiffDb (withLfe.ch[side], noLfe.ch[side], 0);
        const double genShare = rmsDiffDb (stereo.ch[side], noLfe.ch[side], 0);
        const double residual = rmsDiffDb (stereo.ch[side], withLfe.ch[side], 0);
        std::cout << "    measured Game71 stereo downmix, side " << side << ": LFE share " << genShare << " dB (chain fold " << coreShare
                  << " dB; before: none), residual against the chain fold " << residual << " dB\n";
        CHECK_NEAR (genShare, coreShare, 0.1);
        CHECK_LE (residual, coreShare - 60.0); // the same fold, up to float rounding
    }
}
