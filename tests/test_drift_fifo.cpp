// Tests for the app's clock-drift compensating capture FIFO
// (app/Source/engine/DriftCompensatedFifo). It only depends on core headers,
// so its implementation file is compiled into this test TU directly (like
// test_platform_linux.cpp does with the Linux platform services) and the
// test runs on every OS.
//
// Event-driven simulation: a capture thread whose clock runs at
// 48 kHz * (1 + drift) delivers fixed-size packets; the audio callback pulls
// fixed-size blocks at exactly 48 kHz. After the PI loop has settled there
// must be no underruns or overflows, the fill must stay at its target, the
// correction must equal the true drift, and the output must be a continuous
// sine (no skipped or repeated frames).
#include "TestFramework.h"
#include "TestSignals.h"

#include "../app/Source/engine/DriftCompensatedFifo.cpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

using flub::app::DriftCompensatedFifo;
using namespace flubtest;

namespace
{
constexpr double kFifoFs = 48000.0;
constexpr double kToneHz = 997.0;
constexpr float kToneAmp = 0.5f;
// Largest sample-to-sample step of the clean 997 Hz test sine (+5 %).
const double kCleanStep = kToneAmp * flub::kTwoPi * kToneHz / kFifoFs * 1.05;

struct Scenario
{
    double driftPpm = 0.0;  // producer clock error relative to the device clock
    int packet = 480;       // producer frames per push (480 = 10 ms)
    int block = 128;        // device frames per callback
    double seconds = 90.0;  // simulated time
    double settle = 45.0;   // checks below start here
    double average = 15.0;  // correction averaged over the last `average` seconds
    int sourceChannels = 2, fifoChannels = 2;
    std::array<double, 8> toneHz { kToneHz, kToneHz, kToneHz, kToneHz, kToneHz, kToneHz, kToneHz, kToneHz }; // per source channel
    double producerStallAt = -1.0, producerStallMs = 0.0; // capture stall: its audio is lost
    double consumerStallAt = -1.0, consumerStallMs = 0.0; // device stall: callbacks resume later
    double recordSeconds = 0.0; // keep the end of the output for spectral checks
};

struct Outcome
{
    DriftCompensatedFifo::Stats atSettle, atEnd;
    double meanCorrectionPpm = 0.0;
    double minFillMs = 1.0e9, maxFillMs = 0.0; // smoothed fill after settling
    double maxStep = 0.0;                      // channel 0, whole run
    int64_t allocations = 0;                   // inside push() / pull()
    Planar recorded { 2, 0 };
};

/** Sine oscillator by complex rotation (cheap; drift over 90 s is ~1e-10). */
struct Oscillator
{
    double re = 1.0, im = 0.0, c = 1.0, s = 0.0;
    void setFrequency (double hz, double fs)
    {
        c = std::cos (flub::kTwoPi * hz / fs);
        s = std::sin (flub::kTwoPi * hz / fs);
    }
    float next() noexcept
    {
        const auto v = static_cast<float> (im);
        const double r = re * c - im * s;
        im = im * c + re * s;
        re = r;
        return v;
    }
};

Outcome simulate (const Scenario& sc)
{
    const double producerRate = kFifoFs * (1.0 + sc.driftPpm * 1.0e-6);
    DriftCompensatedFifo fifo;
    fifo.prepare (sc.fifoChannels, kFifoFs, kFifoFs, sc.block); // nominal rates equal, as the host requests

    std::array<Oscillator, 8> osc;
    for (size_t c = 0; c < osc.size(); ++c)
        osc[c].setFrequency (sc.toneHz[c], producerRate);

    std::vector<float> packet (static_cast<size_t> (sc.packet) * static_cast<size_t> (sc.sourceChannels));
    Planar out (sc.fifoChannels, sc.block);

    Outcome o;
    const int recordFrames = static_cast<int> (sc.recordSeconds * kFifoFs);
    o.recorded = Planar (sc.fifoChannels, recordFrames);
    const double recordFrom = sc.seconds - sc.recordSeconds - 0.05; // a little early: the window must fill
    int recordPos = 0;

    double nextPush = sc.packet / producerRate; // a packet is delivered once it has been recorded
    double nextPull = sc.block / kFifoFs;
    bool consumerStalled = false, settled = false;
    float last = 0.0f;
    double correctionSum = 0.0;
    int correctionCount = 0;

    while (std::min (nextPush, nextPull) < sc.seconds)
    {
        if (nextPush <= nextPull)
        {
            for (int i = 0; i < sc.packet; ++i)
                for (int c = 0; c < sc.sourceChannels; ++c)
                    packet[static_cast<size_t> (i * sc.sourceChannels + c)] = kToneAmp * osc[static_cast<size_t> (c)].next();
            const bool stalled = sc.producerStallAt >= 0.0 && nextPush >= sc.producerStallAt && nextPush < sc.producerStallAt + sc.producerStallMs * 1.0e-3;
            if (! stalled)
            {
                AllocationGuard guard;
                fifo.push (packet.data(), sc.packet, sc.sourceChannels);
                o.allocations += guard.allocations();
            }
            nextPush += sc.packet / producerRate;
            continue;
        }

        if (! consumerStalled && sc.consumerStallAt >= 0.0 && nextPull >= sc.consumerStallAt)
        {
            consumerStalled = true;
            nextPull += sc.consumerStallMs * 1.0e-3;
            continue;
        }

        {
            AllocationGuard guard;
            fifo.pull (out.ptrs.data(), sc.fifoChannels, sc.block, false);
            o.allocations += guard.allocations();
        }
        for (int i = 0; i < sc.block; ++i)
        {
            const float v = out.ch[0][static_cast<size_t> (i)];
            o.maxStep = std::max (o.maxStep, static_cast<double> (std::abs (v - last)));
            last = v;
        }
        if (recordFrames > 0 && nextPull >= recordFrom)
            for (int i = 0; i < sc.block && recordPos < recordFrames; ++i, ++recordPos)
                for (int c = 0; c < sc.fifoChannels; ++c)
                    o.recorded.ch[static_cast<size_t> (c)][static_cast<size_t> (recordPos)] = out.ch[static_cast<size_t> (c)][static_cast<size_t> (i)];

        const auto st = fifo.getStats();
        if (! settled && nextPull >= sc.settle)
        {
            settled = true;
            o.atSettle = st;
        }
        if (settled)
        {
            o.minFillMs = std::min (o.minFillMs, static_cast<double> (st.fillMs));
            o.maxFillMs = std::max (o.maxFillMs, static_cast<double> (st.fillMs));
        }
        if (nextPull >= sc.seconds - sc.average)
        {
            correctionSum += st.correctionPpm;
            ++correctionCount;
        }
        nextPull += sc.block / kFifoFs;
    }

    o.atEnd = fifo.getStats();
    o.meanCorrectionPpm = correctionCount > 0 ? correctionSum / correctionCount : 0.0;
    return o;
}
} // namespace

// ---------------------------------------------------------------------------
TEST_CASE ("DriftFifo: +-200 and +-2000 ppm drift, 10 ms and 441-frame packets, 128 and 512 blocks: settles clean")
{
    for (double drift : { 200.0, -200.0, 2000.0, -2000.0 })
        for (int packet : { 480, 441 })
            for (int block : { 128, 512 })
            {
                Scenario sc;
                sc.driftPpm = drift;
                sc.packet = packet;
                sc.block = block;
                // The loop (critically damped, 0.15 rad/s) needs longer to
                // learn a large drift to within a few ppm.
                sc.seconds = std::abs (drift) > 1000.0 ? 120.0 : 90.0;
                sc.settle = sc.seconds / 2.0;
                sc.average = sc.seconds / 6.0;
                const auto o = simulate (sc);

                // Target = max(2 blocks, largest packet + 1 block) + 4 frames.
                const double targetMs = 1000.0 * (std::max (2 * block, packet + block) + 4) / kFifoFs;
                CHECK_NEAR (o.atEnd.targetMs, targetMs, 0.05);
                CHECK (o.atEnd.streaming);
                // After settling: no xruns, fill held at the target, and the
                // correction equals the true drift.
                CHECK (o.atEnd.underruns == o.atSettle.underruns);
                CHECK (o.atEnd.overflows == 0);
                CHECK (o.atEnd.droppedFrames == 0);
                CHECK_GE (o.minFillMs, targetMs - 1.0);
                CHECK_LE (o.maxFillMs, targetMs + 1.0);
                CHECK_NEAR (o.meanCorrectionPpm, drift, 5.0);
                // A producer faster than the device never underruns; a slower
                // one may underrun while the loop learns the drift, not after.
                if (drift > 0.0)
                    CHECK (o.atEnd.underruns == 0);
                // Continuity over the whole run, including the initial prime
                // and any re-prime (fade-out / fade-in): no step larger than
                // the clean sine's.
                CHECK_LE (o.maxStep, kCleanStep);
                CHECK (o.allocations == 0);
            }
}

TEST_CASE ("DriftFifo: a capture stall is one counted underrun, then the stream recovers")
{
    Scenario sc;
    sc.driftPpm = -150.0;
    sc.block = 256;
    sc.seconds = 90.0;
    sc.settle = 25.0;
    sc.producerStallAt = 30.0;
    sc.producerStallMs = 200.0;
    sc.average = 15.0;
    const auto o = simulate (sc);

    CHECK (o.atSettle.underruns == 0);
    CHECK (o.atEnd.underruns == 1);  // exactly one fade to silence, then a re-prime
    CHECK (o.atEnd.overflows == 0);
    CHECK (o.atEnd.streaming);
    CHECK_NEAR (o.atEnd.fillMs, 1000.0 * (480 + 256 + 4) / kFifoFs, 1.0);
    CHECK_NEAR (o.meanCorrectionPpm, sc.driftPpm, 10.0); // the learned drift survived the re-prime
    CHECK_LE (o.maxStep, kCleanStep);                    // fade-out and fade-in are click-free
    CHECK (o.allocations == 0);
}

TEST_CASE ("DriftFifo: a device stall drops the oldest audio (counted overflow), then the stream recovers")
{
    Scenario sc;
    sc.driftPpm = 150.0;
    sc.block = 256;
    sc.seconds = 90.0;
    sc.settle = 25.0;
    sc.consumerStallAt = 30.0;
    sc.consumerStallMs = 300.0;
    sc.average = 15.0;
    const auto o = simulate (sc);

    const double targetMs = 1000.0 * (480 + 256 + 4) / kFifoFs;
    CHECK (o.atSettle.overflows == 0);
    CHECK (o.atEnd.overflows == 1);
    CHECK (o.atEnd.underruns == 0);
    // Roughly the 300 ms that piled up during the stall were dropped.
    CHECK_NEAR (static_cast<double> (o.atEnd.droppedFrames), 0.3 * kFifoFs, 1000.0);
    CHECK (o.atEnd.streaming);
    CHECK_NEAR (o.atEnd.fillMs, targetMs, 1.0); // back at the target, not 300 ms late
    CHECK_NEAR (o.meanCorrectionPpm, sc.driftPpm, 10.0);
    CHECK (o.allocations == 0);

    // A stall longer than the ring holds (1 s requested, 65535 frames after
    // rounding up to a power of two): the producer has to drop the newest
    // frames as well; everything lost is counted and the stream recovers.
    sc.consumerStallMs = 1500.0;
    const auto full = simulate (sc);
    CHECK (full.atEnd.overflows == 1);
    CHECK (full.atEnd.underruns == 0);
    CHECK_NEAR (static_cast<double> (full.atEnd.droppedFrames), 1.5 * kFifoFs, 1000.0);
    CHECK (full.atEnd.streaming);
    CHECK_NEAR (full.atEnd.fillMs, targetMs, 1.0);
}

TEST_CASE ("DriftFifo: 7.1 capture into a stereo FIFO is downmixed per ITU-R BS.775 (-3 dB, the LFE at virt.lfe's default)")
{
    // One tone per source channel (FL FR FC LFE BL BR SL SR), each a whole
    // number of periods in the 0.25 s analysis window. The window is short so
    // that the few ppm the loop is still correcting cannot smear the tones.
    Scenario sc;
    sc.driftPpm = 0.0;
    sc.sourceChannels = 8;
    sc.fifoChannels = 2;
    sc.toneHz = { 500.0, 700.0, 900.0, 100.0, 1100.0, 1300.0, 1500.0, 1700.0 };
    sc.seconds = 20.0;
    sc.settle = 10.0;
    sc.average = 5.0;
    sc.recordSeconds = 0.25;
    const auto o = simulate (sc);
    CHECK (o.atEnd.underruns == 0);
    CHECK (o.atEnd.overflows == 0);
    CHECK (o.allocations == 0);

    const int n = o.recorded.numSamples();
    const auto amp = [&] (int ch, double hz) { return toneAmplitude (o.recorded.ch[static_cast<size_t> (ch)].data(), n, hz, kFifoFs) / kToneAmp; };
    const double front = 0.70710678, other = 0.5; // -3 dB overall; FC / surrounds another -3 dB
    CHECK_NEAR (amp (0, 500.0), front, 0.01);  // FL -> L
    CHECK_NEAR (amp (1, 700.0), front, 0.01);  // FR -> R
    CHECK_NEAR (amp (0, 900.0), other, 0.01);  // FC -> both
    CHECK_NEAR (amp (1, 900.0), other, 0.01);
    CHECK_NEAR (amp (0, 1100.0), other, 0.01); // BL -> L
    CHECK_NEAR (amp (1, 1300.0), other, 0.01); // BR -> R
    CHECK_NEAR (amp (0, 1500.0), other, 0.01); // SL -> L
    CHECK_NEAR (amp (1, 1700.0), other, 0.01); // SR -> R
    for (double hz : { 700.0, 1300.0, 1700.0 }) // right-only sources stay out of L
        CHECK_LE (amp (0, hz), 0.01);
    for (double hz : { 500.0, 1100.0, 1500.0 }) // left-only sources stay out of R
        CHECK_LE (amp (1, hz), 0.01);
    // The LFE (docs/11 E01, dropped before): both sides, +10 dB re FL (the
    // default virt.lfe) through the 120 Hz 4th-order Butterworth (-0.91 dB
    // at 100 Hz). tests/app/test_app_lfe_fold.cpp compares it with the chain.
    const double lfe = front * 3.16227766 / std::sqrt (1.0 + std::pow (100.0 / 120.0, 8.0));
    CHECK_NEAR (amp (0, 100.0), lfe, 0.02);
    CHECK_NEAR (amp (1, 100.0), lfe, 0.02);
}

TEST_CASE ("DriftFifo: a mono capture is duplicated to both channels of a stereo FIFO")
{
    Scenario sc;
    sc.driftPpm = 100.0;
    sc.sourceChannels = 1;
    sc.fifoChannels = 2;
    sc.seconds = 12.0;
    sc.settle = 6.0;
    sc.average = 2.0;
    sc.recordSeconds = 1.0;
    const auto o = simulate (sc);
    CHECK (o.atEnd.underruns == 0);
    CHECK (o.atEnd.overflows == 0);
    CHECK (o.allocations == 0);
    CHECK_LE (o.maxStep, kCleanStep);

    const int n = o.recorded.numSamples();
    CHECK (o.recorded.ch[0] == o.recorded.ch[1]);
    CHECK_NEAR (peakAbs (o.recorded.ch[0].data(), n), kToneAmp, 0.01);
    CHECK_NEAR (rms (o.recorded.ch[0].data(), n), kToneAmp / std::sqrt (2.0), 0.005);
}
