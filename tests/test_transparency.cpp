// Top-octave transparency of the oversampled nonlinear stages (delta
// oversampling): the programme must not pass the half-band filters, so a
// 20 kHz tone at 44.1 kHz keeps its level through the maximizer (not
// clipping) and through the saturator at small-signal level.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/dsp/LoudnessMaximizer.h"
#include "flub/dsp/Saturator.h"

using namespace flub;
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
