// docs/11 E11 Phase 3 batch 2: the automatic preamp on hot programme
// (auto.preampHot, ProcessingChain.h): with auto.preamp on, programme whose
// held peaks leave no room under the maximizer's ceiling also loses what the
// allowance leaves in, the maximizer's drive and half the transient
// shaper's attack; programme with room keeps them, bit for bit.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/common/Denormals.h"
#include "flub/engine/Parameters.h"
#include "flub/engine/ProcessingChain.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace flub;
using namespace flub::param;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;

void run (ProcessingChain& chain, Planar& buf, int blockSize)
{
    ScopedNoDenormals noDenormals;
    const int n = buf.numSamples();
    for (int pos = 0; pos < n; pos += blockSize)
        chain.process (buf.block (pos, std::min (blockSize, n - pos)));
}

Planar stereoOf (const std::vector<float>& x)
{
    Planar p (2, static_cast<int> (x.size()));
    std::copy (x.begin(), x.end(), p.ch[0].begin());
    std::copy (x.begin(), x.end(), p.ch[1].begin());
    return p;
}

/** EQ +6 dB at 1 kHz (predicted 6 dB, preamp -5 dB at the 1 dB allowance)
    into the maximizer at 2 dB drive and a -1 dBTP ceiling, nothing else. */
void boostedChain (ParameterStore& s, bool hot)
{
    for (int id : { GateOn, EqOn, DynEqOn, BassOn, ClarityOn, SaturationOn, SpatialOn, VirtualizerOn, CompressorOn, MaximizerOn })
        s.set (id, 0.0f);
    s.set (EqOn, 1.0f);
    s.set (eq (5, EqFieldGain), 6.0f);
    s.set (MaximizerOn, 1.0f);
    s.set (MaxDriveDb, 2.0f);
    s.set (MaxCeilingDb, -1.0f);
    s.set (AutoPreampOn, 1.0f);
    s.set (AutoPreampAllowanceDb, 1.0f);
    s.set (AutoPreampHot, hot ? 1.0f : 0.0f);
}
} // namespace

//==============================================================================
TEST_CASE ("Parameters (E11): auto.preampHot - layout version 5, a toggle, off by default")
{
    const auto& t = layout();
    REQUIRE (findByKey ("auto.preampHot") == AutoPreampHot);
    const auto& info = t[static_cast<size_t> (AutoPreampHot)];
    CHECK (info.sinceVersion == 5);
    CHECK (info.unit == Unit::Toggle);
    CHECK (info.defaultValue == 0.0f);
    CHECK (! info.structural);
}

//==============================================================================
TEST_CASE ("Chain (E11): auto.preampHot leaves programme with room under the ceiling bit for bit, and does nothing without auto.preamp")
{
    const int n = static_cast<int> (kFs * 1.0);
    const auto render = [&] (ParameterStore& s, const std::vector<float>& x) {
        ProcessingChain chain (s);
        chain.prepare ({ kFs, 512, 2 });
        Planar buf = stereoOf (x);
        run (chain, buf, 480);
        return buf;
    };
    // Peaks about -14 dBFS: 14 + ceiling - (allowance + drive) = 10 dB of room.
    const auto quiet = pinkNoise (n, 0.05f, 17);
    ParameterStore a, b;
    boostedChain (a, false);
    boostedChain (b, true);
    CHECK (render (a, quiet).ch == render (b, quiet).ch);

    // A hot sine without auto.preamp: the hot switch alone changes nothing.
    const auto hot = sine (1000.0, kFs, n, 0.966f); // -0.3 dBFS
    a.set (AutoPreampOn, 0.0f);
    b.set (AutoPreampOn, 0.0f);
    CHECK (render (a, hot).ch == render (b, hot).ch);
}

//==============================================================================
TEST_CASE ("Chain (E11): on a -0.3 dBFS sine auto.preampHot takes the allowance and the drive back (the limiter only takes the master's own 0.7 dB over the ceiling), holds 2 s after it and releases at 1 dB/s")
{
    ParameterStore off, on;
    boostedChain (off, false);
    boostedChain (on, true);
    ProcessingChain chainOff (off), chainOn (on);
    chainOff.prepare ({ kFs, 512, 2 });
    chainOn.prepare ({ kFs, 512, 2 });
    const int n = static_cast<int> (kFs * 1.0);
    Planar a = stereoOf (sine (1000.0, kFs, n, 0.966f)), b = a;
    run (chainOff, a, 512);
    run (chainOn, b, 512);
    // The static preamp -5 dB; hot = -clamp (-0.3 + (1 + 2) + 1, 0, 3) = -3 dB.
    CHECK_NEAR (chainOff.getAutoPreampDb(), -5.0, 0.01);
    CHECK_NEAR (chainOn.getAutoPreampDb(), -8.0, 0.02);
    const float grOff = chainOff.meters().maxGainReductionDb.load(), grOn = chainOn.meters().maxGainReductionDb.load();
    std::printf ("    measured hot sine: maximizer GR %.2f dB without, %.2f dB with auto.preampHot\n", grOff, grOn);
    CHECK (grOff < -3.0f);
    CHECK (grOn > -1.0f);

    // Then -20 dBFS: held for 2 s, then released at 1 dB/s until the drive is back.
    const auto preampAfter = [&] (double seconds) {
        Planar q = stereoOf (sine (1000.0, kFs, static_cast<int> (kFs * seconds), 0.1f));
        run (chainOn, q, 512);
        return chainOn.getAutoPreampDb();
    };
    CHECK_NEAR (preampAfter (1.9), -8.0, 0.02);   // 1.9 s: held
    CHECK_NEAR (preampAfter (1.1), -7.7, 0.05);   // 3.0 s: 1 dB released: -clamp (-1.3 + 3 + 1, 0, 3) = -2.7 dB
    CHECK_NEAR (preampAfter (4.0), -5.0, 0.01);   // 7.0 s: the peak 4.3 dB down, room again: the static preamp
}
