// Top-octave transparency of the oversampled nonlinear stages (delta
// oversampling): the programme must not pass the half-band filters, so a
// 20 kHz tone at 44.1 kHz keeps its level through the maximizer (not
// clipping) and through the saturator at small-signal level. And the bit-
// exact contract of Clarity's 3-band transient shaper (docs/11 E04 step 3):
// with its band offsets at 0 the stage is the full-band shaper it was.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/dsp/ClarityEnhancer.h"
#include "flub/dsp/LoudnessMaximizer.h"
#include "flub/dsp/Saturator.h"
#include "flub/dsp/TransientShaper.h"
#include "flub/engine/Parameters.h"
#include "flub/engine/ProcessingChain.h"

using namespace flub;
using namespace flub::param;
using namespace flubtest;

namespace
{
double toneGainDb (Processor& p, double fs, double freq, float amp, int latency)
{
    const int n = static_cast<int> (fs); // 1 s
    Planar buf (2, n);
    auto s = sine (freq, fs, n, amp);
    std::copy (s.begin(), s.end(), buf.ch[0].begin());
    std::copy (s.begin(), s.end(), buf.ch[1].begin());
    processInBlocks (p, buf, 256);
    const int start = n / 2;
    const int len = n / 2 - latency - 1;
    return toDb (toneAmplitude (buf.ch[0].data() + start, len, freq, fs) / amp);
}
} // namespace

TEST_CASE ("Transparency: the maximizer's oversampled clipper does not droop the top octave")
{
    for (double fs : { 44100.0, 48000.0 })
        for (int factor : { 2, 4 })
            for (auto q : { Oversampler::Quality::Low, Oversampler::Quality::High })
            {
                LoudnessMaximizer m;
                m.setClipOversampling (factor, q);
                m.setLookaheadMs (1.0f);
                m.prepare ({ fs, 256, 2 });
                MaximizerParams p;
                p.driveDb = 0.0f;
                p.clipAmount = 1.0f; // clipper engaged, but the tone stays below its threshold
                m.setParams (p);
                for (double f : { 1000.0, 15000.0, 19500.0 })
                {
                    m.reset();
                    CHECK_NEAR (toneGainDb (m, fs, f, dbToGain (-20.0f), m.latencySamples()), 0.0, 0.05);
                }
            }
}

TEST_CASE ("Transparency: the saturator keeps a quiet 19.5 kHz tone at unity gain with drive engaged")
{
    for (double fs : { 44100.0, 48000.0 })
        for (auto q : { Oversampler::Quality::Low, Oversampler::Quality::High })
            for (auto type : { SaturationType::Tape, SaturationType::Tube, SaturationType::Digital })
            {
                Saturator sat;
                sat.setOversampling (2, q);
                sat.prepare ({ fs, 256, 2 });
                SaturatorParams p;
                p.type = type;
                p.driveDb = 12.0f;
                sat.setParams (p);
                CHECK_NEAR (toneGainDb (sat, fs, 19500.0, dbToGain (-40.0f), sat.latencySamples()), 0.0, 0.1);
            }
}

TEST_CASE ("Transparency (E04 step 3): with clarity.attackLow / attackHigh at 0 Clarity's stage 1 is the full-band shaper bit for bit; neutral is an exact pass-through; the chain hands the offsets over")
{
    // Kicks, noise hits and a steady tone, stereo.
    const double fs = 48000.0;
    const int n = static_cast<int> (fs * 0.8);
    Planar in (2, n);
    {
        const auto noise = whiteNoise (n, 0.3f, 21);
        const auto tone = sine (70.0, fs, n, 0.3f);
        for (int i = 0; i < n; ++i)
        {
            const size_t k = static_cast<size_t> (i);
            const double t = static_cast<double> (i % 7200) / fs; // a hit every 150 ms
            const float hit = static_cast<float> (std::exp (-t / 0.02)) * noise[k];
            in.ch[0][k] = hit + tone[k];
            in.ch[1][k] = 0.5f * hit - tone[k];
        }
    }
    auto copyOf = [&] {
        Planar p (2, n);
        p.ch = in.ch;
        p.ptrs.clear();
        for (auto& c : p.ch)
            p.ptrs.push_back (c.data());
        return p;
    };

    for (float attack : { 9.0f, -7.0f })
    {
        ClarityEnhancer ce;
        ce.prepare ({ fs, 256, 2 });
        ClarityParams p;
        p.attackDb = attack;
        p.sustainDb = -5.0f;
        p.lowSplitHz = 90.0f;      // the split and the speed act only on the bands
        p.transientSpeed = 1.7f;
        ce.setParams (p);
        ce.reset();
        Planar a = copyOf();
        processInBlocks (ce, a, 256);
        CHECK (! ce.isBandPathActive());

        TransientShaper ts;
        ts.prepare (fs);
        ts.setAttackDb (attack);
        ts.setSustainDb (-5.0f);
        ts.reset();
        Planar b = copyOf();
        for (int pos = 0; pos < n; pos += 256)
            ts.process (b.block (pos, std::min (256, n - pos)));
        CHECK (a.ch == b.ch);
    }

    // Neutral: exact.
    {
        ClarityEnhancer ce;
        ce.prepare ({ fs, 256, 2 });
        ce.setParams (ClarityParams {});
        ce.reset();
        Planar a = copyOf();
        processInBlocks (ce, a, 256);
        CHECK (a.ch == in.ch);
    }

    // The chain: offsets at 0 (the defaults) and set to 0 explicitly are the
    // same render; an offset changes it.
    auto render = [&] (float low, float high, bool setExplicitly)
    {
        ParameterStore store;
        store.set (ClarityOn, 1.0f);
        store.set (ClarityAttackDb, 6.0f);
        if (setExplicitly)
        {
            store.set (ClarityAttackLowDb, low);
            store.set (ClarityAttackHighDb, high);
        }
        ProcessingChain chain (store);
        chain.prepare ({ fs, 256, 2 });
        Planar a = copyOf();
        for (int pos = 0; pos < n; pos += 256)
            chain.process (a.block (pos, std::min (256, n - pos)));
        return a.ch;
    };
    const auto defaults = render (0.0f, 0.0f, false);
    CHECK (render (0.0f, 0.0f, true) == defaults);
    CHECK (render (6.0f, 0.0f, true) != defaults);
    CHECK (render (0.0f, -6.0f, true) != defaults);
    CHECK (layout()[static_cast<size_t> (ClarityAttackLowDb)].sinceVersion == 8);
    CHECK (layout()[static_cast<size_t> (ClarityAttackHighDb)].sinceVersion == 8);
    CHECK (layout()[static_cast<size_t> (ClarityAttackLowDb)].defaultValue == 0.0f);
    CHECK (layout()[static_cast<size_t> (ClarityAttackHighDb)].defaultValue == 0.0f);
    CHECK (findByKey ("clarity.attackLow") == ClarityAttackLowDb);
    CHECK (findByKey ("clarity.attackHigh") == ClarityAttackHighDb);
}
