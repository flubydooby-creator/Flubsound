// MixEngine chat sidechain and ChatMix (docs/11 E22): VoiceActivity on the
// Chat strip's input, the voice-keyed ChatDucker on the Game and Music
// strips (a 1 - 4 kHz dip, the Game footstep band protected, the Game
// chain's Voice & Score lift taken back, the Game peaks held 3 dB under the
// master ceiling) and the ChatMix balance.
//
// The engine runs three strips named Game, Music and Chat. Unless a case
// says otherwise their chains are bypassed (bypass.all), so what is measured
// is the mix bus itself; the chains' own latency (their dry delay) is kept.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/analysis/Discontinuity.h"
#include "flub/analysis/LoudnessMeter.h"
#include "flub/engine/MixEngine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>

using namespace flub;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;
constexpr int kBlock = 256;
enum
{
    kGame = 0,
    kMusic = 1,
    kChat = 2
};

int samplesOf (double seconds) { return static_cast<int> (std::lround (seconds * kFs)); }
double uniform (FastRandom& rng, double lo, double hi) { return lo + (hi - lo) * 0.5 * (static_cast<double> (rng.nextBipolar()) + 1.0); }

/** Two-pole resonator (tests/test_soak_levels.cpp's). */
struct Resonator
{
    double a1 = 0.0, a2 = 0.0, g = 0.0, y1 = 0.0, y2 = 0.0;
    void set (double hz, double bandwidthHz)
    {
        const double r = std::exp (-kPi * bandwidthHz / kFs);
        a1 = 2.0 * r * std::cos (kTwoPi * hz / kFs);
        a2 = -r * r;
        g = 1.0 - r;
    }
    double process (double x)
    {
        const double y = g * x + a1 * y1 + a2 * y2;
        y2 = y1;
        y1 = y;
        return y;
    }
};

/** One-pole low-pass / two cascaded one-pole high-passes, in double. */
std::vector<float> lowPass (const std::vector<float>& x, double fc)
{
    const double a = std::exp (-kTwoPi * fc / kFs);
    std::vector<float> y (x.size());
    double s = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        s = (1.0 - a) * x[i] + a * s;
        y[i] = static_cast<float> (s);
    }
    return y;
}

std::vector<float> highPass (std::vector<float> x, double fc)
{
    for (int stage = 0; stage < 2; ++stage)
    {
        const auto lp = lowPass (x, fc);
        for (size_t i = 0; i < x.size(); ++i)
            x[i] -= lp[i];
    }
    return x;
}

/** Speech-like mono signal (tests/test_soak_levels.cpp's formantSpeech, the
    E60 speech): phrases of 4-7 syllables (an impulse train at 100-170 Hz
    through three vowel formants; every fourth syllable a 4.5 kHz
    fricative), 30-70 ms between syllables, 0.4-0.6 s between phrases,
    from `start` to `end` seconds. */
std::vector<float> formantSpeech (int n, uint32_t seed, double start = 0.1, double end = 1.0e9)
{
    static const double vowels[5][2] = { { 730, 1090 }, { 270, 2290 }, { 300, 870 }, { 530, 1840 }, { 570, 840 } };
    std::vector<float> x (static_cast<size_t> (n), 0.0f);
    FastRandom rng (seed);
    auto hiss = whiteNoise (n, 1.0f, seed + 1);
    {
        const double w0 = kTwoPi * 4500.0 / kFs, alpha = std::sin (w0) / 3.0, a0 = 1.0 + alpha;
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
        for (auto& v : hiss)
        {
            const double in = v, out = (alpha * in - alpha * x2 + 2.0 * std::cos (w0) * y1 - (1.0 - alpha) * y2) / a0;
            x2 = x1;
            x1 = in;
            y2 = y1;
            y1 = out;
            v = static_cast<float> (out);
        }
    }
    const double to = std::min (end, n / kFs);
    double t = start;
    int k = 0;
    while (t < to - 0.8)
    {
        const int syllables = 4 + static_cast<int> (rng.nextU32() % 4u);
        const double f0 = uniform (rng, 100.0, 170.0);
        for (int j = 0; j < syllables && t < to - 0.3; ++j, ++k)
        {
            const double dur = uniform (rng, 0.12, 0.24);
            const int onset = samplesOf (t), len = samplesOf (dur), edge = samplesOf (0.02);
            const auto envelope = [&] (int i) {
                const double a = i < edge ? 0.5 - 0.5 * std::cos (kPi * i / edge) : 1.0;
                const double b = len - i < edge ? 0.5 - 0.5 * std::cos (kPi * (len - i) / edge) : 1.0;
                return a * b;
            };
            if (k % 4 == 3)
            {
                for (int i = 0; i < len; ++i)
                    x[static_cast<size_t> (onset + i)] += static_cast<float> (0.25 * envelope (i) * hiss[static_cast<size_t> (onset + i)]);
            }
            else
            {
                const auto& v = vowels[rng.nextU32() % 5u];
                Resonator r1, r2, r3;
                r1.set (v[0], 90.0);
                r2.set (v[1], 110.0);
                r3.set (2500.0, 150.0);
                double phase = 1.0;
                const double pitch = f0 * uniform (rng, 0.9, 1.1);
                for (int i = 0; i < len; ++i)
                {
                    phase += (pitch * (1.0 - 0.1 * i / len)) / kFs;
                    const double pulse = phase >= 1.0 ? 1.0 : 0.0;
                    if (phase >= 1.0)
                        phase -= 1.0;
                    const double y = r1.process (pulse) + 0.6 * r2.process (pulse) + 0.25 * r3.process (pulse);
                    x[static_cast<size_t> (onset + i)] += static_cast<float> (envelope (i) * y);
                }
            }
            t += dur + uniform (rng, 0.03, 0.07);
        }
        t += uniform (rng, 0.4, 0.6);
    }
    return x;
}

/** Stereo music (tests/test_soak_levels.cpp's makeMusic, the E60 music):
    kick on every beat, snare on 2 and 4, hats on the eighths, a bass note
    and a three-note pad per bar. */
Planar makeMusic (int n, double bpm, uint32_t seed)
{
    Planar m (2, n);
    const auto noise = whiteNoise (n, 1.0f, seed);
    const auto snareNoise = highPass (noise, 1000.0), hatNoise = highPass (highPass (noise, 7000.0), 7000.0);
    const double beat = 60.0 / bpm;
    static const double roots[4] = { 55.0, 43.65, 65.41, 49.0 };
    static const double chords[4][3] = { { 220.0, 261.63, 329.63 }, { 174.61, 220.0, 261.63 }, { 261.63, 329.63, 392.0 }, { 196.0, 246.94, 293.66 } };
    for (int b = 0; b * beat < n / kFs; ++b)
    {
        const int onset = samplesOf (b * beat), bar = (b / 4) % 4;
        const int len = std::min (samplesOf (beat), n - onset);
        for (int i = 0; i < len; ++i)
        {
            const double t = i / kFs, tb = (b * beat) + t;
            double mono = 0.9 * std::exp (-t / 0.12) * std::sin (kTwoPi * (45.0 * t + 10.0 * 0.03 * (1.0 - std::exp (-t / 0.03))));
            if (b % 2 == 1)
                mono += 0.45 * std::exp (-t / 0.08) * snareNoise[static_cast<size_t> (onset + i)];
            const double bass = 0.35 * std::exp (-t / 0.3) * (std::sin (kTwoPi * roots[bar] * tb) + 0.3 * std::sin (kTwoPi * 2.0 * roots[bar] * tb));
            for (int ch = 0; ch < 2; ++ch)
            {
                double pad = 0.0;
                for (double f : chords[bar])
                    for (int h = 1; h <= 4; ++h)
                        pad += std::sin (kTwoPi * f * h * (ch == 0 ? 0.997 : 1.003) * tb) / h;
                const double hatGain = (i < samplesOf (beat / 2) ? 1.0 : 0.7) * (ch == 0 ? 0.8 : 1.2);
                const int half = i % samplesOf (beat / 2);
                const double hat = 0.12 * hatGain * std::exp (-half / (0.02 * kFs)) * hatNoise[static_cast<size_t> (onset + i)];
                m.ch[static_cast<size_t> (ch)][static_cast<size_t> (onset + i)] += static_cast<float> (mono + bass + 0.05 * pad + hat);
            }
        }
    }
    return m;
}

/** A second kind of music, all inside the voice band: a sustained three-voice
    string pad (slow attack, chord changes every 2 s) under a legato lead
    line (a note every 0.25 - 0.5 s, 330 - 880 Hz, with vibrato). */
Planar makePadAndLead (int n, uint32_t seed)
{
    Planar m (2, n);
    FastRandom rng (seed);
    static const double chords[4][3] = { { 220.0, 277.18, 329.63 }, { 246.94, 293.66, 369.99 }, { 196.0, 246.94, 293.66 }, { 220.0, 261.63, 329.63 } };
    std::vector<double> leadHz;
    std::vector<int> leadStart;
    for (int s = 0; s < n;)
    {
        leadStart.push_back (s);
        leadHz.push_back (330.0 * std::pow (2.0, std::floor (uniform (rng, 0.0, 17.0)) / 12.0));
        s += samplesOf (uniform (rng, 0.25, 0.5));
    }
    double leadPhase = 0.0;
    size_t note = 0;
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kFs;
        const int c = static_cast<int> (t / 2.0) % 4;
        const double tc = std::fmod (t, 2.0);
        const double env = std::min (1.0, tc / 0.3);
        while (note + 1 < leadStart.size() && i >= leadStart[note + 1])
            ++note;
        leadPhase += leadHz[note] * (1.0 + 0.004 * std::sin (kTwoPi * 5.5 * t)) / kFs;
        const double lead = 0.25 * (std::sin (kTwoPi * leadPhase) + 0.4 * std::sin (2.0 * kTwoPi * leadPhase) + 0.2 * std::sin (3.0 * kTwoPi * leadPhase));
        for (int ch = 0; ch < 2; ++ch)
        {
            double pad = 0.0;
            for (double f : chords[c])
                for (int h = 1; h <= 5; ++h)
                    pad += std::sin (kTwoPi * f * h * (ch == 0 ? 0.998 : 1.002) * t) / (h * h);
            m.ch[static_cast<size_t> (ch)][static_cast<size_t> (i)] = static_cast<float> (0.12 * env * pad + lead);
        }
    }
    return m;
}

Planar stereo (const std::vector<float>& mono)
{
    Planar p (2, static_cast<int> (mono.size()));
    p.ch[0] = mono;
    p.ch[1] = mono;
    p.rebind();
    return p;
}

void scale (Planar& p, double gain)
{
    for (auto& c : p.ch)
        for (auto& v : c)
            v = static_cast<float> (v * gain);
}

/** Gated integrated loudness (BS.1770) of the whole buffer. */
double loudnessOf (Planar p)
{
    LoudnessMeter meter;
    meter.prepare (kFs, 2);
    meter.process (p.block());
    return meter.getIntegratedLufs();
}

/** Scales p so its gated integrated loudness is `lufs`. */
void scaleToLufs (Planar& p, double lufs)
{
    scale (p, std::pow (10.0, (lufs - loudnessOf (p)) / 20.0));
}

std::unique_ptr<MixEngine> makeEngine (int block = kBlock, bool bypassChains = true, const char* chatName = "Chat")
{
    auto m = std::make_unique<MixEngine>();
    const std::vector<StripConfig> layout { { "Game", 2, 0.0f, false }, { "Music", 2, 0.0f, false }, { chatName, 2, 0.0f, false } };
    m->configure (layout, kFs, block);
    m->setIdleFreeze (false);
    if (bypassChains)
        for (int s = 0; s < m->getNumStrips(); ++s)
            m->params (s).set (param::BypassAll, 1.0f);
    return m;
}

struct RunOptions
{
    int block = kBlock;
    std::function<void (int)> beforeBlock, afterBlock; // called with the block's first sample
    int64_t* allocations = nullptr;                    // heap allocations inside process()
    /** Each fed strip's own output (MixEngine processes in place: after
        its chain, pad and duck, before its gain), one Planar per strip. */
    std::vector<Planar>* stripOutputs = nullptr;
    double* processSeconds = nullptr; // wall-clock time inside process()
};

/** Runs the engine over the inputs (nullptr: that strip is not fed) and
    returns the stereo output. */
Planar run (MixEngine& m, const std::vector<const Planar*>& inputs, int n, const RunOptions& o = {})
{
    Planar out (2, n);
    const int strips = m.getNumStrips();
    std::vector<Planar> scratch;
    for (int s = 0; s < strips; ++s)
        scratch.emplace_back (2, o.block);
    if (o.stripOutputs != nullptr)
        o.stripOutputs->assign (static_cast<size_t> (strips), Planar (2, n));
    std::vector<AudioBlock> blocks;
    std::vector<const AudioBlock*> ptrs (static_cast<size_t> (strips), nullptr);
    for (int s = 0; s < strips; ++s)
        blocks.push_back (scratch[static_cast<size_t> (s)].block());
    for (int pos = 0; pos < n; pos += o.block)
    {
        const int len = std::min (o.block, n - pos);
        for (int s = 0; s < strips; ++s)
        {
            const auto si = static_cast<size_t> (s);
            const Planar* in = si < inputs.size() ? inputs[si] : nullptr;
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < len; ++k)
                    scratch[si].ch[static_cast<size_t> (c)][static_cast<size_t> (k)] = in != nullptr ? in->ch[static_cast<size_t> (c)][static_cast<size_t> (pos + k)] : 0.0f;
            blocks[si] = scratch[si].block (0, len);
            ptrs[si] = in != nullptr ? &blocks[si] : nullptr;
        }
        if (o.beforeBlock)
            o.beforeBlock (pos);
        const AudioBlock dst = out.block (pos, len);
        {
            AllocationGuard guard;
            const auto start = std::chrono::steady_clock::now();
            m.process (ptrs.data(), dst);
            if (o.processSeconds != nullptr)
                *o.processSeconds += std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
            if (o.allocations != nullptr)
                *o.allocations += guard.allocations();
        }
        if (o.stripOutputs != nullptr)
            for (int s = 0; s < strips; ++s)
                for (size_t c = 0; c < 2; ++c)
                    std::copy_n (scratch[static_cast<size_t> (s)].ch[c].begin(), len,
                                 (*o.stripOutputs)[static_cast<size_t> (s)].ch[c].begin() + pos);
        if (o.afterBlock)
            o.afterBlock (pos);
    }
    return out;
}

double toneDb (const Planar& p, int from, int length, double hz)
{
    return toDb (toneAmplitude (p.ch[0].data() + from, length, hz, kFs));
}

/** Sum of sines (same on both channels). */
Planar tones (int n, std::initializer_list<double> hz, float amplitude)
{
    Planar p (2, n);
    for (const double f : hz)
    {
        const auto t = sine (f, kFs, n, amplitude);
        for (auto& c : p.ch)
            for (size_t i = 0; i < c.size(); ++i)
                c[i] += t[i];
    }
    return p;
}

bool identical (const Planar& a, const Planar& b, int from, int to)
{
    for (size_t c = 0; c < 2; ++c)
        for (int i = from; i < to; ++i)
            if (a.ch[c][static_cast<size_t> (i)] != b.ch[c][static_cast<size_t> (i)])
                return false;
    return true;
}
} // namespace

// ---- the dip's shape (ChatDucker's sections, exact responses) ----------------

TEST_CASE ("E22 ChatDucker: Game dip covers 1-2.2 kHz, leaves the footstep band within 0.5 dB")
{
    for (const double depth : { 3.0, 4.5, 6.0 })
    {
        for (const double hz : { 1250.0, 1500.0, 1800.0, 2000.0, 2200.0 })
            CHECK (ChatDucker::dipResponseDb (ChatDucker::Shape::Game, depth, hz, kFs) <= -0.8 * depth);
        CHECK (ChatDucker::dipResponseDb (ChatDucker::Shape::Game, depth, 1000.0, kFs) <= -0.8 * depth);
        // The protected region: the Gaming cue-detail band (3.2 kHz) up to
        // 6 kHz, and the footstep body band (260 Hz).
        for (const double hz : { 2800.0, 3000.0, 3200.0, 3400.0, 3600.0, 4000.0, 5000.0, 6000.0, 180.0, 260.0, 370.0 })
            CHECK (std::abs (ChatDucker::dipResponseDb (ChatDucker::Shape::Game, depth, hz, kFs)) <= 0.5);
        // Music: one 2 kHz bell, about half the depth at 1 and 4 kHz.
        CHECK_NEAR (ChatDucker::dipResponseDb (ChatDucker::Shape::Music, depth, 2000.0, kFs), -depth, 0.01);
        CHECK (ChatDucker::dipResponseDb (ChatDucker::Shape::Music, depth, 1000.0, kFs) <= -0.4 * depth);
        CHECK (ChatDucker::dipResponseDb (ChatDucker::Shape::Music, depth, 4000.0, kFs) <= -0.4 * depth);
    }
    // The lift cancel is the chain's Voice & Score band, inverted.
    CHECK_NEAR (ProcessingChain::modeBandFrequency (param::ModeValue::Gaming, ProcessingChain::kFirstModeBand + 3), ChatDucker::kVoiceLiftHz, 0.01);
}

// ---- VoiceActivity -----------------------------------------------------------

namespace
{
struct VadCase
{
    const char* name;
    Planar signal;
    bool speech;
};

void checkVad (std::vector<VadCase>& cases)
{
    for (auto& c : cases)
    {
        const int n = c.signal.numSamples();
        VoiceActivity vad;
        vad.prepare (kFs);
        // Per 10 ms frame: the detector's verdict, and (speech) whether the
        // frame is inside a syllable (within 30 dB of the loudest frame).
        const int frame = samplesOf (0.01);
        std::vector<char> active, syllable;
        std::vector<double> frameDb;
        for (int pos = 0; pos + frame <= n; pos += frame)
        {
            vad.process (c.signal.block (pos, frame));
            active.push_back (vad.isActive() ? 1 : 0);
            frameDb.push_back (toDb (rms (c.signal.ch[0].data() + pos, frame)));
        }
        const double loudest = *std::max_element (frameDb.begin(), frameDb.end());
        int inSyllable = 0, found = 0, onsets = 0;
        double latency = 0.0;
        for (size_t f = 0; f < frameDb.size(); ++f)
        {
            if (frameDb[f] < loudest - 30.0)
                continue;
            ++inSyllable;
            found += active[f];
            // A phrase onset: 300 ms of quiet before it.
            bool onset = f >= 30;
            for (size_t g = f >= 30 ? f - 30 : 0; g < f && onset; ++g)
                onset = frameDb[g] < loudest - 30.0;
            if (onset)
            {
                size_t a = f;
                while (a < active.size() && ! active[a])
                    ++a;
                latency += 10.0 * static_cast<double> (a - f);
                ++onsets;
            }
        }
        const double duty = 100.0 * static_cast<double> (vad.getActiveFrames()) / static_cast<double> (vad.getFrames());
        std::cout << "    [E22] VAD " << c.name << ": active " << duty << " % of " << n / kFs << " s";
        if (c.speech)
        {
            const double recall = 100.0 * found / std::max (1, inSyllable);
            std::cout << ", " << recall << " % of syllable frames, phrase onset found after " << latency / std::max (1, onsets) << " ms (mean of "
                      << onsets << ")";
            CHECK (recall > 75.0);
            CHECK (latency / std::max (1, onsets) < 250.0);
        }
        else
            CHECK (duty < 5.0);
        std::cout << "\n";
    }
}
} // namespace

TEST_CASE ("E22 VoiceActivity: finds speech (-20 and -35 LUFS) within a syllable")
{
    const int n = samplesOf (8.0);
    std::vector<VadCase> cases;
    for (const double lufs : { -20.0, -35.0 })
    {
        Planar s = stereo (formantSpeech (n, 11));
        scaleToLufs (s, lufs);
        cases.push_back ({ lufs > -30.0 ? "speech -20 LUFS" : "speech -35 LUFS", s, true });
    }
    checkVad (cases);
}

TEST_CASE ("E22 VoiceActivity: false-positive duty < 5 % on 10 s of music (and pink noise)")
{
    const int n = samplesOf (10.0);
    std::vector<VadCase> cases;
    Planar drums = makeMusic (n, 120.0, 21), pad = makePadAndLead (n, 31), quietPad = pad;
    scaleToLufs (drums, -14.0);
    scaleToLufs (pad, -18.0);
    scaleToLufs (quietPad, -40.0);
    cases.push_back ({ "drum music -14 LUFS", drums, false });
    cases.push_back ({ "pad and lead -18 LUFS", pad, false });
    cases.push_back ({ "pad and lead -40 LUFS", quietPad, false });
    Planar pink (2, n);
    pink.ch[0] = pinkNoise (n, 0.05f, 5);
    pink.ch[1] = pinkNoise (n, 0.05f, 6);
    pink.rebind();
    cases.push_back ({ "pink noise", pink, false });
    checkVad (cases);
}

// ---- the duck in the engine --------------------------------------------------

TEST_CASE ("E22 duck: the Game and Music 1-4 kHz dip only while speech is active, footstep band within 0.5 dB, latency unchanged")
{
    const int n = samplesOf (5.6);
    // Game: the footstep body (260 Hz), the dip (1.25 kHz) and the cue
    // detail (3.2 kHz); Music: 2 kHz. Chat: speech from 1 to 3.5 s.
    const Planar game = tones (n, { 260.0, 1250.0, 3200.0 }, 0.05f);
    const Planar music = tones (n, { 2000.0 }, 0.05f);
    Planar chat = stereo (formantSpeech (n, 11, 1.0, 3.5));
    scale (chat, 0.1 / std::max (1.0e-9, peakAbs (chat.ch[0].data(), n)) * 3.0); // speech peaks about -10 dBFS

    auto off = makeEngine(), on = makeEngine();
    // The chat itself is muted in the mix (the detector listens to the
    // strip's input), so the tones are measured without its formants.
    for (auto* m : { off.get(), on.get() })
        m->setStripMuted (kChat, true);
    const int latency = off->getLatencySamples();
    on->setChatDuck (true, 4.5f);
    CHECK (on->getLatencySamples() == latency);
    std::vector<float> amount;
    RunOptions trace;
    trace.afterBlock = [&] (int) { amount.push_back (on->getChatDuckAmount()); };
    int64_t allocations = 0;
    trace.allocations = &allocations;
    const Planar a = run (*off, { &game, &music, &chat }, n);
    const Planar b = run (*on, { &game, &music, &chat }, n, trace);
    CHECK (allocations == 0);
    CHECK (on->getLatencySamples() == latency);

    // When the detector first and last held speech.
    const auto blockAt = [] (int sample) { return static_cast<size_t> (sample / kBlock); };
    size_t firstOn = amount.size(), lastOn = 0;
    for (size_t i = 0; i < amount.size(); ++i)
        if (amount[i] > 0.0f)
        {
            firstOn = std::min (firstOn, i);
            lastOn = i;
        }
    REQUIRE (firstOn < amount.size());
    const double onSeconds = static_cast<double> (firstOn * kBlock) / kFs, offSeconds = static_cast<double> ((lastOn + 1) * kBlock) / kFs;
    std::cout << "    [E22] duck in from " << onSeconds << " s to " << offSeconds << " s (speech from 1.0 s to about 3.1 s)\n";
    CHECK (onSeconds > 1.0);
    CHECK (onSeconds < 1.3);
    CHECK (offSeconds < 5.5);
    // Bit-identical before the duck and once it is idle again.
    CHECK (identical (a, b, 0, static_cast<int> (firstOn) * kBlock));
    CHECK (identical (a, b, static_cast<int> (lastOn + 2) * kBlock + latency, n));

    // 100 ms windows: the dip only where the duck was in; its depth where
    // it was fully in; the footstep bands within 0.5 dB throughout.
    const int w = samplesOf (0.1);
    double deepest1250 = 0.0, music2000 = 0.0, worstProtected = 0.0;
    int full = 0;
    for (int pos = latency; pos + w <= n; pos += w)
    {
        const double d1250 = toneDb (b, pos, w, 1250.0) - toneDb (a, pos, w, 1250.0);
        const double d2000 = toneDb (b, pos, w, 2000.0) - toneDb (a, pos, w, 2000.0);
        const double d260 = toneDb (b, pos, w, 260.0) - toneDb (a, pos, w, 260.0);
        const double d3200 = toneDb (b, pos, w, 3200.0) - toneDb (a, pos, w, 3200.0);
        worstProtected = std::max ({ worstProtected, std::abs (d260), std::abs (d3200) });

        const size_t first = blockAt (pos - latency), last = std::min (amount.size() - 1, blockAt (pos + w - latency));
        bool anyIn = false, allIn = true;
        for (size_t i = first; i <= last; ++i)
        {
            anyIn = anyIn || amount[i] > 0.0f;
            allIn = allIn && amount[i] > 0.97f;
        }
        if (! anyIn)
            CHECK (std::abs (d1250) < 0.01); // no dip while no speech is held
        if (allIn)
        {
            ++full;
            deepest1250 = std::min (deepest1250, d1250);
            music2000 = std::min (music2000, d2000);
            CHECK (d1250 < -0.8 * 4.5 * 0.97);
            CHECK (d2000 < -4.5 * 0.97 + 0.05);

        }
    }
    std::cout << "    [E22] duck 4.5 dB: Game 1.25 kHz " << deepest1250 << " dB, Music 2 kHz " << music2000
              << " dB (" << full << " windows fully in); 260 Hz / 3.2 kHz moved at most " << worstProtected << " dB\n";
    CHECK (full >= 15);
    CHECK (worstProtected <= 0.5);

    // Click-free: the duck's glide in and out (after the tones' own start).
    DiscontinuitySettings ds;
    ds.blockSize = kBlock;
    DiscontinuityDetector det;
    det.prepare (kFs, 2, ds);
    const int skip = samplesOf (0.5);
    const float* from[2] = { b.ch[0].data() + skip, b.ch[1].data() + skip };
    det.process (from, n - skip);
    det.finish();
    CHECK (det.total() == 0);
}

TEST_CASE ("E22 off: bit-identical to an engine without a Chat strip; on with no voice untouched")
{
    const int n = samplesOf (2.0);
    const Planar game = tones (n, { 260.0, 1250.0, 3200.0 }, 0.3f), music = makeMusic (n, 120.0, 21);
    Planar chat = stereo (formantSpeech (n, 11, 0.3));
    scaleToLufs (chat, -20.0);
    Planar chatMusic = makePadAndLead (n, 31);
    scaleToLufs (chatMusic, -20.0);
    auto off = makeEngine(), plain = makeEngine (kBlock, true, "Voice"), on = makeEngine(), onPlain = makeEngine (kBlock, true, "Voice");
    on->setChatDuck (true);
    CHECK (plain->getStripRole (kChat) == MixEngine::StripRole::Other);
    CHECK (off->getStripRole (kChat) == MixEngine::StripRole::Chat);
    CHECK (identical (run (*off, { &game, &music, &chat }, n), run (*plain, { &game, &music, &chat }, n), 0, n));
    // With the duck on and music (not speech) on Chat nothing is ducked.
    CHECK (identical (run (*on, { &game, &music, &chatMusic }, n), run (*onPlain, { &game, &music, &chatMusic }, n), 0, n));
    CHECK (on->getChatDuckAmount() == 0.0f);
}

namespace
{
/** The explosion scene (6 s). Game: an ambience at about -45 dBFS and,
    every 2 s from 1 s, an explosion (low rumble plus a broadband crack,
    0.8 s decay), limited the way a game's own mix is (a tanh stage) and
    scaled to a -1 dBFS sample peak; Chat: speech throughout at `lufs`. */
struct ExplosionScene
{
    Planar game, chat;
    double lufs = 0.0;
};

ExplosionScene explosionScene (double lufs)
{
    const int n = samplesOf (6.0);
    ExplosionScene scene { Planar (2, n), stereo (formantSpeech (n, 7)), lufs };
    const auto rumble = lowPass (lowPass (whiteNoise (n, 1.0f, 3), 150.0), 150.0);
    const auto crack = whiteNoise (n, 1.0f, 4);
    const auto bed = pinkNoise (n, 0.005f, 5);
    const double rumbleRms = rms (rumble.data(), n);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kFs, e = t >= 1.0 ? std::fmod (t - 1.0, 2.0) : 10.0;
        const double env = e < 1.8 ? std::exp (-e / 0.8) : 0.0;
        const double x = env * (rumble[static_cast<size_t> (i)] / rumbleRms + 0.3 * crack[static_cast<size_t> (i)]);
        const float v = static_cast<float> (bed[static_cast<size_t> (i)] + std::tanh (0.8 * x));
        scene.game.ch[0][static_cast<size_t> (i)] = v;
        scene.game.ch[1][static_cast<size_t> (i)] = v;
    }
    scale (scene.game, dbToGain (-1.0f) / peakAbs (scene.game.ch[0].data(), n));
    scaleToLufs (scene.chat, lufs);
    return scene;
}

/** The scene at `lufs`, made once per test process (main thread only). */
const ExplosionScene& sceneFor (double lufs)
{
    static std::map<long, std::unique_ptr<ExplosionScene>> scenes;
    auto& s = scenes[std::lround (lufs * 100.0)];
    if (s == nullptr)
        s = std::make_unique<ExplosionScene> (explosionScene (lufs));
    return *s;
}

struct ExplosionOptions
{
    bool duck = true;
    float floorDb = ChatDucker::kDefaultRoomFloorDb; // the room's floor (0: the offset ceiling alone, as before the room)
    bool chatFirst = false;                           // the layout Chat, Game, Music instead of Game, Music, Chat
    const DeviceCorrectionSettings* correction = nullptr;
};

/** What one run of the scene measured. From 3 s on (the explosions at 3
    and 5 s, the detector long settled): */
struct ExplosionResult
{
    double chatDropDb = 0.0;      // the chat's short-term level at the output against the chat alone, worst
    double masterGrDb = 0.0;      // the master limiter's deepest gain reduction
    float gameCeilingDb = 0.0f;   // the Game duck's limiter, deepest gain
    float roomDb = 0.0f;          // the room's deepest ceiling under the master ceiling (0: never under the offset)
    Planar gameOut { 2, 0 };      // the Game strip's own output (after its duck, before its gain = its share)
    int64_t gameClicks = 0;       // DiscontinuityDetector clicks on gameOut (from 0.5 s)
};

/** Runs the scene through a three-strip engine (64-sample blocks, the
    chains bypassed). The chat drop is its input through the master
    limiter's gain (the deepest of each block) against the chat alone.
    Thread-safe (its own engine; no CHECK). */
ExplosionResult runExplosions (const ExplosionScene& scene, const ExplosionOptions& options)
{
    const int n = scene.game.numSamples(), block = 64;
    ExplosionResult r;
    auto m = std::make_unique<MixEngine>();
    const int game = options.chatFirst ? 1 : kGame, chat = options.chatFirst ? 0 : kChat;
    std::vector<StripConfig> layout { { "Game", 2, 0.0f, false }, { "Music", 2, 0.0f, false }, { "Chat", 2, 0.0f, false } };
    if (options.chatFirst)
        layout = { { "Chat", 2, 0.0f, false }, { "Game", 2, 0.0f, false }, { "Music", 2, 0.0f, false } };
    m->configure (layout, kFs, block);
    m->setIdleFreeze (false);
    for (int s = 0; s < m->getNumStrips(); ++s)
        m->params (s).set (param::BypassAll, 1.0f);
    if (options.correction != nullptr)
        m->getDeviceCorrection().setSettingsNow (*options.correction);
    m->setChatDuck (options.duck);
    m->setChatRoomFloorDb (options.floorDb);

    std::vector<float> gr;
    std::vector<Planar> outs;
    RunOptions o;
    o.block = block;
    o.stripOutputs = &outs;
    o.afterBlock = [&] (int pos) {
        gr.push_back (m->getMasterGainReductionDb());
        if (pos < samplesOf (3.0))
            return;
        r.masterGrDb = std::min (r.masterGrDb, static_cast<double> (std::min (0.0f, m->getMasterGainReductionDb())));
        r.gameCeilingDb = std::min (r.gameCeilingDb, m->getChatDucker (game)->getCeilingGainDb());
        r.roomDb = std::min (r.roomDb, m->getChatDucker (game)->getRoomDb());
    };
    std::vector<const Planar*> inputs (3, nullptr);
    inputs[static_cast<size_t> (game)] = &scene.game;
    inputs[static_cast<size_t> (chat)] = &scene.chat;
    run (*m, inputs, n, o);
    r.gameOut = outs[static_cast<size_t> (game)];

    Planar alone = scene.chat, through = scene.chat;
    const int latency = m->getLatencySamples();
    for (int i = 0; i < n; ++i)
    {
        const size_t k = static_cast<size_t> (std::min (n - 1, i + latency) / block);
        const float g = dbToGain (std::min (0.0f, gr[std::min (k, gr.size() - 1)]));
        through.ch[0][static_cast<size_t> (i)] *= g;
        through.ch[1][static_cast<size_t> (i)] *= g;
    }
    LoudnessMeter aloneMeter, throughMeter;
    aloneMeter.prepare (kFs, 2);
    throughMeter.prepare (kFs, 2);
    const int step = samplesOf (0.1);
    for (int pos = 0; pos + step <= n; pos += step)
    {
        aloneMeter.process (alone.block (pos, step));
        throughMeter.process (through.block (pos, step));
        if (pos >= samplesOf (3.0))
            r.chatDropDb = std::max (r.chatDropDb, static_cast<double> (aloneMeter.getShortTermLufs() - throughMeter.getShortTermLufs()));
    }

    DiscontinuitySettings ds;
    ds.blockSize = block;
    DiscontinuityDetector det;
    det.prepare (kFs, 2, ds);
    const int skip = samplesOf (0.5);
    const float* from[2] = { r.gameOut.ch[0].data() + skip, r.gameOut.ch[1].data() + skip };
    det.process (from, n - skip);
    det.finish();
    r.gameClicks = det.count (DiscontinuityType::Click);
    return r;
}

/** Runs each configuration of `scene`, in parallel (each its own engine),
    and returns the results in order. Runs without a correction are kept
    for the rest of the process (main thread only), so the floor table's
    cases share their duck-off and offset-alone runs; a case run on its own
    makes what it needs. */
std::vector<ExplosionResult> runAll (const ExplosionScene& scene, const std::vector<ExplosionOptions>& configs)
{
    static std::map<std::string, ExplosionResult> memo;
    const auto keyOf = [&scene] (const ExplosionOptions& o) {
        char key[96];
        std::snprintf (key, sizeof (key), "%.2f/%d/%.2f/%d", scene.lufs, o.duck ? 1 : 0, static_cast<double> (o.floorDb), o.chatFirst ? 1 : 0);
        return std::string (key);
    };
    std::vector<std::future<ExplosionResult>> pending (configs.size());
    for (size_t i = 0; i < configs.size(); ++i)
        if (configs[i].correction != nullptr || memo.count (keyOf (configs[i])) == 0)
            pending[i] = std::async (std::launch::async, [&scene, o = configs[i]] { return runExplosions (scene, o); });
    std::vector<ExplosionResult> results;
    for (size_t i = 0; i < configs.size(); ++i)
    {
        if (! pending[i].valid())
            results.push_back (memo.at (keyOf (configs[i])));
        else if (configs[i].correction != nullptr)
            results.push_back (pending[i].get());
        else
            results.push_back (memo[keyOf (configs[i])] = pending[i].get());
    }
    return results;
}

/** How far the duck (and its room) pulled the explosions down against the
    duck off (both the Game strip's own output, from 3 s): the deepest drop
    of a 10 ms window's peak where the explosion is loud (that window over
    -12 dBFS with the duck off), and the drop of the loudest short-term
    loudness (3 s). */
struct PullDown
{
    double peakDb = 0.0, shortTermDb = 0.0;
};

PullDown pullDown (const Planar& off, const Planar& on)
{
    PullDown p;
    const int n = off.numSamples(), w = samplesOf (0.01);
    for (int pos = samplesOf (3.0); pos + w <= n; pos += w)
    {
        const double a = std::max (peakAbs (off.ch[0].data() + pos, w), peakAbs (off.ch[1].data() + pos, w));
        const double b = std::max (peakAbs (on.ch[0].data() + pos, w), peakAbs (on.ch[1].data() + pos, w));
        if (a > dbToGain (-12.0f))
            p.peakDb = std::min (p.peakDb, toDb (b) - toDb (a));
    }
    const auto loudest = [n] (Planar x) {
        LoudnessMeter meter;
        meter.prepare (kFs, 2);
        double most = -200.0;
        const int step = samplesOf (0.1);
        for (int pos = 0; pos + step <= n; pos += step)
        {
            meter.process (x.block (pos, step));
            if (pos >= samplesOf (3.0))
                most = std::max (most, static_cast<double> (meter.getShortTermLufs()));
        }
        return most;
    };
    p.shortTermDb = loudest (on) - loudest (off);
    return p;
}

void printRow (const char* label, const ExplosionResult& r, const PullDown* p)
{
    char line[400];
    std::snprintf (line, sizeof (line), "      %-14s chat short-term -%.2f dB, master GR %.2f dB, Game duck limiter %.2f dB, room %.2f dB", label, r.chatDropDb,
                   r.masterGrDb, static_cast<double> (r.gameCeilingDb), static_cast<double> (r.roomDb));
    std::cout << line;
    if (p != nullptr)
    {
        std::snprintf (line, sizeof (line), ", explosions pulled down: peak %.2f dB, short-term %.2f dB", p->peakDb, p->shortTermDb);
        std::cout << line;
    }
    std::cout << ", Game clicks " << r.gameClicks << "\n";
}
} // namespace

TEST_CASE ("E22 duck: -1 dBTP explosions on Game change the Chat short-term level < 0.5 dB (Chat at -20 LUFS)")
{
    ExplosionOptions off;
    off.duck = false;
    const auto r = runAll (sceneFor (-20.0), { off, {} }); // E23's Voice Chat level
    const auto &before = r[0], &after = r[1];
    std::cout << "    [E22] chat at -20 LUFS, short-term level under -1 dBTP explosions: duck off -" << before.chatDropDb << " dB, on -"
              << after.chatDropDb << " dB (Game duck limiter deepest " << after.gameCeilingDb << " dB)\n";
    CHECK (after.chatDropDb < 0.5);
    CHECK (after.chatDropDb < before.chatDropDb);
    CHECK (before.gameCeilingDb == 0.0f);
    CHECK (after.gameCeilingDb < -2.5f);
}

TEST_CASE ("E22 duck: -1 dBTP explosions on Game, a loud teammate (Chat at -14 LUFS): the room holds it < 0.5 dB")
{
    ExplosionOptions off, offset;
    off.duck = false;
    offset.floorDb = 0.0f; // the offset ceiling alone (before the room)
    const auto r = runAll (sceneFor (-14.0), { off, offset, {} });
    const auto &before = r[0], &withOffset = r[1], &after = r[2];
    std::cout << "    [E22] chat at -14 LUFS, short-term level under -1 dBTP explosions: duck off -" << before.chatDropDb << " dB, offset ceiling alone -"
              << withOffset.chatDropDb << " dB, with the room (floor " << ChatDucker::kDefaultRoomFloorDb << " dB) -" << after.chatDropDb << " dB\n";
    CHECK (after.chatDropDb < 0.5);
    CHECK (withOffset.chatDropDb < before.chatDropDb);
    CHECK (after.chatDropDb < withOffset.chatDropDb);
    CHECK (after.roomDb < -ChatDucker::kCeilingOffsetDb); // the room went under the offset ceiling
    CHECK (withOffset.roomDb == 0.0f);                    // floor 0 dB: no room
}

// ---- the room (the chat sub-limiter, docs/11 E22 (1)) -------------------------

namespace
{
/** One row of the floor table for the owner: chat at `lufs` under the
    explosions with the duck off, the offset ceiling alone (floor 0 dB, as
    before the room) and the room at `floorDb`. Returns the room's run. */
ExplosionResult floorRow (double lufs, float floorDb)
{
    ExplosionOptions off, offset, withRoom;
    off.duck = false;
    offset.floorDb = 0.0f;
    withRoom.floorDb = floorDb;
    const auto r = runAll (sceneFor (lufs), { off, offset, withRoom });
    const auto &a = r[0], &b = r[1], &c = r[2];
    const PullDown pb = pullDown (a.gameOut, b.gameOut), pc = pullDown (a.gameOut, c.gameOut);
    std::cout << "    [E22] room table, chat " << lufs << " LUFS, floor " << floorDb << " dB:\n";
    printRow ("duck off", a, nullptr);
    printRow ("offset alone", b, &pb);
    printRow ("with the room", c, &pc);
    // The room only ever adds protection, within its floor, without clicks.
    CHECK (c.chatDropDb <= b.chatDropDb + 0.01);
    CHECK (c.roomDb < -ChatDucker::kCeilingOffsetDb);
    CHECK (c.roomDb >= floorDb - 0.01f);
    CHECK (pc.peakDb >= static_cast<double> (floorDb) - 1.0); // + the 1 - 2.4 kHz dip's share of a peak
    CHECK (c.gameClicks <= b.gameClicks);
    return c;
}
} // namespace

TEST_CASE ("E22 room: floor table - chat -20 LUFS, floor -6 dB")
{
    // One row at -20 LUFS: the room stays well above any floor there, so
    // -9 and -12 dB give the same run (the floors only matter at -14 LUFS).
    const auto r = floorRow (-20.0, -6.0f);
    CHECK (r.roomDb > -6.0f + 0.5f);
}

TEST_CASE ("E22 room: floor table - chat -14 LUFS, floor -6 dB")
{
    floorRow (-14.0, -6.0f);
}

TEST_CASE ("E22 room: floor table - chat -14 LUFS, floor -9 dB")
{
    floorRow (-14.0, -9.0f);
}

TEST_CASE ("E22 room: floor table - chat -14 LUFS, floor -12 dB")
{
    floorRow (-14.0, -12.0f);
}

TEST_CASE ("E22 room: the same whichever order the strips are in (the Chat strip first)")
{
    ExplosionOptions chatFirst;
    chatFirst.chatFirst = true;
    const auto r = runAll (sceneFor (-14.0), { {}, chatFirst });
    std::cout << "    [E22] room, chat -14 LUFS: Game, Music, Chat -" << r[0].chatDropDb << " dB; Chat, Game, Music -" << r[1].chatDropDb << " dB\n";
    CHECK (r[1].chatDropDb < 0.5);
    CHECK_NEAR (r[0].chatDropDb, r[1].chatDropDb, 0.05);
    CHECK_NEAR (r[0].roomDb, r[1].roomDb, 0.05f);
}

TEST_CASE ("E22 room: ChatRoomEnvelope looks ahead within the block, rises at most full scale per kAttackMs without corners, holds, releases to 0; NaN reads as 0")
{
    ChatRoomEnvelope env;
    env.prepare (kFs, 1024);
    const double slope = 1.0 / (ChatRoomEnvelope::kAttackMs * 0.001 * kFs); // per sample
    const double smoothing = std::exp (-1.0 / (ChatRoomEnvelope::kSmoothMs * 0.001 * kFs));
    const int lead = samplesOf (ChatRoomEnvelope::kLeadMs * 0.001), hold = samplesOf (ChatRoomEnvelope::kHoldMs * 0.001);
    const int ramp = static_cast<int> (std::ceil (0.6 / slope));

    // A peak 700 samples into the block: held kLeadMs before it, a ramp
    // before that, rounded by the smoother, so it is there (within 0.3 %)
    // when the peak is; nothing before the ramp.
    Planar x (2, 1024);
    x.ch[0][700] = 0.6f;
    x.rebind();
    std::vector<float> level (1024);
    {
        const AudioBlock first = x.block (0, 1024);
        const float* l = env.process (&first, 1024, 1.0f, 1.0f);
        std::copy (l, l + 1024, level.begin());
    }
    REQUIRE (lead + ramp < 690);
    CHECK (level[700] >= 0.6f * 0.997f);
    CHECK (level[700] <= 0.6f);
    CHECK (level[static_cast<size_t> (700 - lead - ramp - 1)] == 0.0f);
    // Never faster than the slope, and no corner: the second difference stays
    // at the smoother's (1 - smoothing) x slope, against a full slope for a
    // bare ramp.
    double steepest = 0.0, sharpest = 0.0;
    for (size_t k = 2; k < level.size(); ++k)
    {
        steepest = std::max (steepest, static_cast<double> (level[k] - level[k - 1]));
        sharpest = std::max (sharpest, std::abs (static_cast<double> (level[k]) - 2.0 * level[k - 1] + level[k - 2]));
    }
    CHECK (steepest <= slope * 1.0001);
    CHECK (sharpest <= slope * (1.0 - smoothing) * 1.01);
    CHECK_NEAR (level[1023], 0.6, 1.0e-5); // held (the block ends inside the hold)

    // Then released: one release time after the hold, about 1 / e of it;
    // and, under kSilentLevel (-100 dBFS), exactly 0.
    Planar silence (2, 1024);
    int pos = 1024;
    float atRelease = 0.0f, oneTau = 0.0f, last = 1.0f;
    const int releaseAt = 700 + hold, tauAt = releaseAt + samplesOf (ChatRoomEnvelope::kReleaseMs * 0.001);
    while (pos < samplesOf (2.5))
    {
        const AudioBlock b = silence.block (0, 1024);
        const float* l = env.process (&b, 1024, 1.0f, 1.0f);
        for (int k = 0; k < 1024; ++k)
        {
            if (pos + k == releaseAt)
                atRelease = l[k];
            if (pos + k == tauAt)
                oneTau = l[k];
        }
        last = l[1023];
        pos += 1024;
    }
    std::cout << "    [E22] ChatRoomEnvelope: a 0.6 peak at sample 700: " << level[700] << " there, the ramp " << ramp << " + " << lead
              << " samples ahead, rising at most " << steepest / slope << " x the slope, second difference at most " << sharpest
              << "; at the end of the hold " << atRelease << ", one release time later " << oneTau << ", 2.5 s on " << last << "\n";
    CHECK_NEAR (atRelease, 0.6, 1.0e-4);
    CHECK_NEAR (oneTau, 0.6 * std::exp (-1.0), 0.01);
    CHECK (last == 0.0f);
    CHECK (env.getLevel() == 0.0f);

    // A peak at the very start of a block (not seen ahead): the rise is
    // still rate-limited, from where the level stood, and smoothed.
    env.reset();
    Planar y (2, 64);
    for (auto& c : y.ch)
        std::fill (c.begin(), c.end(), 0.5f);
    y.rebind();
    {
        const AudioBlock b = y.block (0, 64);
        const float* l = env.process (&b, 64, 1.0f, 1.0f);
        bool underSlope = true;
        for (int k = 0; k < 64; ++k)
            underSlope = underSlope && l[k] <= static_cast<float> ((k + 1) * slope * 1.0001);
        CHECK (underSlope);
        CHECK (l[63] >= static_cast<float> ((64.0 - 1.0 / (1.0 - smoothing)) * slope * 0.999));
    }

    // The gain glide is applied (a muted Chat strip adds nothing); a level
    // under kSilentLevel is silence; NaN and Inf read as 0 / capped; nullptr
    // is silence.
    env.reset();
    {
        const AudioBlock b = y.block (0, 64);
        CHECK (env.process (&b, 64, 0.0f, 0.0f)[63] == 0.0f);
    }
    Planar hiss (2, 64);
    for (auto& c : hiss.ch)
        std::fill (c.begin(), c.end(), 0.5f * ChatRoomEnvelope::kSilentLevel);
    hiss.rebind();
    {
        const AudioBlock b = hiss.block (0, 64);
        CHECK (env.process (&b, 64, 1.0f, 1.0f)[63] == 0.0f);
        CHECK (env.process (nullptr, 64, 1.0f, 1.0f)[63] == 0.0f);
    }
    Planar bad (2, 64);
    bad.ch[0][10] = std::numeric_limits<float>::quiet_NaN();
    bad.ch[1][20] = std::numeric_limits<float>::infinity();
    bad.rebind();
    {
        const AudioBlock b = bad.block (0, 64);
        const float* l = env.process (&b, 64, 1.0f, 1.0f);
        bool finite = true;
        for (int k = 0; k < 64; ++k)
            finite = finite && std::isfinite (l[k]) && l[k] <= 16.0f;
        CHECK (finite);
    }
}

TEST_CASE ("E22 room: its level is a ramp, not a step, on a loud sustained Game signal (a stepped room clicks)")
{
    // The Game duck at full amount on a sustained 55 + 110 Hz rumble (peak
    // -2.1 dBFS), held at the offset ceiling (-4 dBFS). Where the rumble is
    // at its loudest, 2 samples into a 64-sample block (so the envelope
    // cannot look ahead: the late case), the chat's level jumps from 0 to
    // 0.7: as a step (a room without its attack) or through ChatRoomEnvelope
    // (the room as MixEngine gives it), floor -12 dB.
    const int n = samplesOf (0.5), block = 64;
    const Planar rumble = tones (n, { 55.0, 110.0 }, 0.445f);
    int at = samplesOf (0.2);
    for (int i = samplesOf (0.2); i < samplesOf (0.2) + samplesOf (1.0 / 55.0); ++i)
        if (std::abs (rumble.ch[0][static_cast<size_t> (i)]) > std::abs (rumble.ch[0][static_cast<size_t> (at)]))
            at = i;
    std::vector<float> stepped (static_cast<size_t> (n), 0.0f), shaped (static_cast<size_t> (n), 0.0f);
    {
        Planar chat (2, n);
        for (int i = at; i < n; ++i)
        {
            stepped[static_cast<size_t> (i)] = 0.7f;
            chat.ch[0][static_cast<size_t> (i)] = chat.ch[1][static_cast<size_t> (i)] = (i / 200) % 2 == 0 ? 0.7f : -0.7f;
        }
        chat.rebind();
        ChatRoomEnvelope env;
        env.prepare (kFs, block);
        for (int pos = 0; pos < n; pos += block)
        {
            const AudioBlock b = chat.block (pos, block);
            const float* l = env.process (&b, block, 1.0f, 1.0f);
            std::copy (l, l + block, shaped.begin() + pos);
        }
    }
    const auto clicksWith = [&] (const std::vector<float>& level) {
        ChatDucker d;
        d.prepare (kFs, ChatDucker::Shape::Game);
        d.reset (1.0f);
        Planar x = rumble;
        ChatDucker::Control dc;
        dc.voiceActive = true;
        dc.ceilingDb = -1.0f;
        dc.roomCeiling = dbToGain (-1.0f);
        dc.roomFloorDb = -12.0f;
        for (int pos = 0; pos < n; pos += block)
        {
            dc.chatLevel = level.data() + pos;
            d.process (x.block (pos, block), dc);
        }
        DiscontinuitySettings ds;
        ds.blockSize = block;
        DiscontinuityDetector det;
        det.prepare (kFs, 2, ds);
        const int skip = samplesOf (0.1);
        const float* from[2] = { x.ch[0].data() + skip, x.ch[1].data() + skip };
        det.process (from, n - skip);
        det.finish();
        return det.count (DiscontinuityType::Click);
    };
    const int64_t steppedClicks = clicksWith (stepped), shapedClicks = clicksWith (shaped);
    std::cout << "    [E22] room on the 55 + 110 Hz rumble at its loudest: a stepped chat level " << steppedClicks << " clicks, through ChatRoomEnvelope "
              << shapedClicks << "\n";
    CHECK (steppedClicks > 0); // the detector sees a step ...
    CHECK (shapedClicks == 0); // ... and the room's ramp is none
}

TEST_CASE ("E22 ChatDucker: the Game limiter releases all the way to 1 and the stage idles after the chat")
{
    // A -1 dBFS 200 Hz tone held at the offset ceiling (-4 dBFS) while the
    // voice is active; then the voice is gone and the tone at -7 dBFS, under
    // every ceiling. The limiter holds 20 ms and releases with 150 ms to 1 / e,
    // all the way to 1, and the stage idles once the amount is 0 (300 ms
    // release to 1e-3: about 2.1 s). Before 2026-10-08 the release
    // `1 - r (1 - g)` stalled in float about 2e-4 under 1 (-0.0019 dB at
    // 48 kHz: the step fell under half an ulp), so the stage never idled.
    ChatDucker d;
    d.prepare (kFs, ChatDucker::Shape::Game);
    d.reset (1.0f);
    ChatDucker::Control dc;
    const int block = 16, n = samplesOf (3.5), quietFrom = samplesOf (0.5);
    Planar x = tones (n, { 200.0 }, 0.89f);
    for (auto& c : x.ch)
        for (int i = quietFrom; i < n; ++i)
            c[static_cast<size_t> (i)] *= 0.5f;
    const int oneTau = quietFrom + samplesOf ((ChatDucker::kHoldMs + ChatDucker::kLimiterReleaseMs) * 0.001);
    float limitedDb = 0.0f, atOneTauDb = 0.0f, lastDb = -1.0f;
    int idleAt = -1;
    for (int pos = 0; pos < n; pos += block)
    {
        dc.voiceActive = pos < quietFrom;
        d.process (x.block (pos, block), dc);
        if (pos < quietFrom)
            limitedDb = std::min (limitedDb, d.getCeilingGainDb());
        if (pos <= oneTau && oneTau < pos + block)
            atOneTauDb = d.getCeilingGainDb();
        lastDb = d.getCeilingGainDb();
        if (idleAt < 0 && d.isIdle())
            idleAt = pos + block;
    }
    // The depth (1 - gain) one release time after the hold: 1 / e of the held one.
    const double held = 1.0 - dbToGain (limitedDb), released = 1.0 - dbToGain (atOneTauDb);
    std::cout << "    [E22] Game limiter: held " << limitedDb << " dB, one release time after the hold " << atOneTauDb << " dB (depth ratio "
              << released / held << "), at the end " << lastDb << " dB, idle " << (idleAt < 0 ? -1.0 : (idleAt - quietFrom) / kFs)
              << " s after the voice\n";
    CHECK (limitedDb < -2.5f);
    CHECK_NEAR (released / held, std::exp (-1.0), 0.03);
    CHECK (lastDb == 0.0f);
    CHECK (idleAt >= 0);
    CHECK (idleAt <= quietFrom + samplesOf (2.3));
    CHECK (d.getAmount() == 0.0f);
}

namespace
{
/** Clicks (DiscontinuityDetector) in p from `from` on. */
int64_t clicksIn (const Planar& p, int block, int from)
{
    DiscontinuitySettings ds;
    ds.blockSize = block;
    DiscontinuityDetector det;
    det.prepare (kFs, 2, ds);
    const float* c[2] = { p.ch[0].data() + from, p.ch[1].data() + from };
    det.process (c, p.numSamples() - from);
    det.finish();
    return det.count (DiscontinuityType::Click);
}

/** A three-strip engine (chains bypassed, no idle freeze) with the duck on,
    the room floor at `floorDb` and the Game strip's gain at `gameGainDb`
    from the start (no glide). */
std::unique_ptr<MixEngine> makeDuckEngine (int block, float floorDb, float gameGainDb)
{
    auto m = std::make_unique<MixEngine>();
    m->configure ({ { "Game", 2, gameGainDb, false }, { "Music", 2, 0.0f, false }, { "Chat", 2, 0.0f, false } }, kFs, block);
    m->setIdleFreeze (false);
    for (int s = 0; s < m->getNumStrips(); ++s)
        m->params (s).set (param::BypassAll, 1.0f);
    m->setChatDuck (true);
    m->setChatRoomFloorDb (floorDb);
    return m;
}
} // namespace

TEST_CASE ("E22 room: in the engine, no click on a loud sustained Game signal under a loud teammate")
{
    // The 55 + 110 Hz rumble (peak -2.1 dBFS) under a -14 LUFS teammate,
    // 64-sample blocks: floors -6 and -12 dB at unity Game gain, and -6 dB
    // with the Game strip at +6 dB (docs/11 E22 review: there the room is
    // measured from the offset ceiling after the gain). Each run its own
    // engine, in parallel.
    const int n = samplesOf (3.0);
    const Planar game = tones (n, { 55.0, 110.0 }, 0.445f);
    Planar chat = stereo (formantSpeech (n, 7, 0.3));
    scaleToLufs (chat, -14.0);
    struct Config
    {
        float floorDb, gameGainDb;
    };
    struct Result
    {
        float roomDb = 0.0f;
        int64_t clicks = 0;
    };
    const Config configs[] = { { ChatDucker::kDefaultRoomFloorDb, 0.0f }, { -12.0f, 0.0f }, { ChatDucker::kDefaultRoomFloorDb, 6.0f } };
    std::vector<std::future<Result>> pending;
    for (const auto& c : configs)
        pending.push_back (std::async (std::launch::async, [&game, &chat, n, c] {
            auto m = makeDuckEngine (64, c.floorDb, c.gameGainDb);
            Result r;
            std::vector<Planar> outs;
            RunOptions o;
            o.block = 64;
            o.stripOutputs = &outs;
            o.afterBlock = [&] (int) { r.roomDb = std::min (r.roomDb, m->getChatDucker (kGame)->getRoomDb()); };
            run (*m, { &game, nullptr, &chat }, n, o);
            r.clicks = clicksIn (outs[kGame], 64, samplesOf (0.2));
            return r;
        }));
    for (size_t i = 0; i < pending.size(); ++i)
    {
        const Result r = pending[i].get();
        const Config& c = configs[i];
        std::cout << "    [E22] room floor " << c.floorDb << " dB, Game strip " << c.gameGainDb << " dB, on the rumble: room down to " << r.roomDb << " dB, "
                  << r.clicks << " clicks on the Game strip\n";
        if (c.gameGainDb == 0.0f)
            CHECK (r.roomDb < -ChatDucker::kCeilingOffsetDb); // under the offset ceiling (the reference is the master ceiling here)
        else
            CHECK (r.roomDb < -1.0f); // the reference is the offset ceiling after the gain
        CHECK (r.roomDb >= c.floorDb - 0.01f);
        CHECK (r.clicks == 0);
    }
}

TEST_CASE ("E22 room: with the chat silent while the hangover holds, the offset ceiling alone (Game strip at 0 and +6 dB)")
{
    // A -1 dBFS 200 Hz tone on Game, a loud teammate from 0.2 s to about
    // 1.7 s, then silence while the detector's hangover holds the duck in:
    // the room engine against one with floor 0 dB (the offset ceiling
    // alone), with the Game strip at 0 and at +6 dB.
    const int n = samplesOf (3.5);
    const Planar game = tones (n, { 200.0 }, 0.89f);
    Planar chat = stereo (formantSpeech (n, 11, 0.2, 2.0));
    scaleToLufs (chat, -14.0);
    int lastChat = 0;
    for (int i = 0; i < n; ++i)
        if (chat.ch[0][static_cast<size_t> (i)] != 0.0f)
            lastChat = i;
    struct BlockReading
    {
        float ceilingDb, roomDb, amount;
        bool voice;
    };
    const auto readings = [&game, &chat, n] (float floorDb, float gameGainDb) {
        auto m = makeDuckEngine (kBlock, floorDb, gameGainDb);
        std::vector<BlockReading> r;
        RunOptions o;
        o.afterBlock = [&] (int) {
            const auto* d = m->getChatDucker (kGame);
            r.push_back ({ d->getCeilingGainDb(), d->getRoomDb(), m->getChatDuckAmount(), m->isChatVoiceActive() });
        };
        run (*m, { &game, nullptr, &chat }, n, o);
        return r;
    };
    // All four runs at once, each its own engine.
    const float gains[] = { 0.0f, 6.0f };
    std::vector<std::future<std::vector<BlockReading>>> pending;
    for (const float gameGainDb : gains)
    {
        pending.push_back (std::async (std::launch::async, readings, ChatDucker::kDefaultRoomFloorDb, gameGainDb));
        pending.push_back (std::async (std::launch::async, readings, 0.0f, gameGainDb));
    }
    for (size_t row = 0; row < 2; ++row)
    {
        const float gameGainDb = gains[row];
        const auto withRoom = pending[2 * row].get(), offsetAlone = pending[2 * row + 1].get();
        float duringSpeech = 0.0f, silentRoom = 0.0f;
        double worstDiff = 0.0, firstDiff = -1.0, lastDiff = 0.0;
        int silentBlocks = 0;
        bool opensOnly = true;
        float previousRoom = -1.0e9f;
        for (size_t b = 0; b < withRoom.size(); ++b)
        {
            const int pos = static_cast<int> (b) * kBlock;
            if (pos < lastChat)
                duringSpeech = std::min (duringSpeech, withRoom[b].roomDb);
            // Silent for longer than the envelope's hold and two of its
            // release times; the hangover still holds the duck fully in.
            if (pos >= lastChat + samplesOf (0.3) && withRoom[b].voice && withRoom[b].amount >= 0.999f)
            {
                ++silentBlocks;
                const double diff = std::abs (static_cast<double> (withRoom[b].ceilingDb - offsetAlone[b].ceilingDb));
                if (firstDiff < 0.0)
                    firstDiff = diff;
                lastDiff = diff;
                worstDiff = std::max (worstDiff, diff);
                opensOnly = opensOnly && withRoom[b].roomDb >= previousRoom - 1.0e-4f;
                previousRoom = silentRoom = withRoom[b].roomDb;
            }
        }
        std::cout << "    [E22] Game strip " << gameGainDb << " dB: room while the chat talks down to " << duringSpeech << " dB; " << silentBlocks
                  << " blocks of the hangover after it: room at its end " << silentRoom << " dB, the Game limiter " << firstDiff << " -> " << lastDiff
                  << " dB (worst " << worstDiff << ") from the offset ceiling alone\n";
        CHECK (silentBlocks >= 20);
        CHECK (opensOnly); // the room only opens while the chat is silent
        if (gameGainDb == 0.0f)
        {
            // Unity gain: the room is measured from the master ceiling, and
            // the chat's released level is far under the 3 dB the offset
            // ceiling leaves: the offset ceiling alone, exactly.
            CHECK (duringSpeech < -ChatDucker::kCeilingOffsetDb);
            CHECK (silentRoom == 0.0f);
            CHECK (worstDiff < 0.01);
        }
        else
        {
            // +6 dB: the reference is the offset ceiling after the gain, so
            // the chat's level takes room off it until its envelope has
            // released (150 ms): that tail only, no hold at the master
            // ceiling over the gain (3 dB under the offset ceiling here).
            CHECK (duringSpeech < -1.0f);
            CHECK (lastDiff <= firstDiff);
            CHECK (lastDiff < 0.25);
        }
    }
}

namespace
{
/** The Game strip turned up (docs/11 E22 review, 2026-10-08): a 55 + 110 Hz
    rumble scaled to a -2 dBFS peak on Game and speech at -20 LUFS on Chat
    from 0.3 s to about 1.6 s, then silence (4.9 s). */
struct GainScene
{
    Planar game, chat;
    int lastChat = 0; // the chat's last non-zero sample
};

const GainScene& gainScene()
{
    static std::unique_ptr<GainScene> scene; // main thread only
    if (scene == nullptr)
    {
        const int n = samplesOf (4.9);
        scene = std::make_unique<GainScene> (GainScene { tones (n, { 55.0, 110.0 }, 0.4f), stereo (formantSpeech (n, 11, 0.3, 1.9)), 0 });
        scale (scene->game, dbToGain (-2.0f) / peakAbs (scene->game.ch[0].data(), n));
        scaleToLufs (scene->chat, -20.0);
        for (int i = 0; i < n; ++i)
            if (scene->chat.ch[0][static_cast<size_t> (i)] != 0.0f)
                scene->lastChat = i;
    }
    return *scene;
}

struct GainRun
{
    Planar gameOut { 2, 0 };       // the Game strip's own output (after its duck, before its gain)
    std::vector<float> roomDb;     // the Game duck's room, per block
    std::vector<char> idle;        // the Game duck idle after the block
    int64_t gameClicks = 0, outClicks = 0; // from 0.2 s
};

/** The scene through an engine with the Game strip at `gameGainDb` and the
    room at `floorDb`, 64-sample blocks. Thread-safe (its own engine; no CHECK). */
GainRun runGainScene (const GainScene& scene, float gameGainDb, float floorDb)
{
    const int n = scene.game.numSamples(), block = 64;
    auto m = makeDuckEngine (block, floorDb, gameGainDb);
    GainRun r;
    std::vector<Planar> outs;
    RunOptions o;
    o.block = block;
    o.stripOutputs = &outs;
    o.afterBlock = [&] (int) {
        const auto* d = m->getChatDucker (kGame);
        r.roomDb.push_back (d->getRoomDb());
        r.idle.push_back (d->isIdle() ? 1 : 0);
    };
    const Planar out = run (*m, { &scene.game, nullptr, &scene.chat }, n, o);
    r.gameOut = outs[kGame];
    r.gameClicks = clicksIn (r.gameOut, block, samplesOf (0.2));
    r.outClicks = clicksIn (out, block, samplesOf (0.2));
    return r;
}

/** One gain row: the room against the offset ceiling alone (floor 0 dB),
    run in parallel. */
void gainRow (float gameGainDb)
{
    const auto& scene = gainScene();
    const int n = scene.game.numSamples(), block = 64;
    REQUIRE (scene.lastChat > samplesOf (1.0));
    REQUIRE (scene.lastChat + samplesOf (3.2) < n);
    auto pendingRoom = std::async (std::launch::async, [&scene, gameGainDb] { return runGainScene (scene, gameGainDb, ChatDucker::kDefaultRoomFloorDb); });
    const GainRun offset = runGainScene (scene, gameGainDb, 0.0f);
    const GainRun room = pendingRoom.get();

    // Once the chat's level has released (its envelope: 20 ms hold, then
    // 150 ms to 1 / e, under -100 dBFS it reads as silence: 1.8 s covers a
    // full-scale peak), the room leaves the offset ceiling alone, while the
    // duck is still releasing.
    const int silentFrom = scene.lastChat + samplesOf (1.8);
    float during = 0.0f, silentRoom = 0.0f;
    bool duckingWhileSilent = false;
    int idleFrom = -1, offsetIdleFrom = -1; // the first sample from which the Game duck stays idle
    for (size_t b = 0; b < room.roomDb.size(); ++b)
    {
        const int pos = static_cast<int> (b) * block;
        if (pos < scene.lastChat)
            during = std::min (during, room.roomDb[b]);
        if (pos >= silentFrom)
        {
            silentRoom = std::min (silentRoom, room.roomDb[b]);
            duckingWhileSilent = duckingWhileSilent || room.idle[b] == 0;
        }
        idleFrom = room.idle[b] == 0 ? -1 : (idleFrom < 0 ? pos + block : idleFrom);
        offsetIdleFrom = offset.idle[b] == 0 ? -1 : (offsetIdleFrom < 0 ? pos + block : offsetIdleFrom);
    }
    const bool sameOnceSilent = identical (room.gameOut, offset.gameOut, silentFrom, n);
    const auto after = [&scene] (int at) { return at < 0 ? -1.0 : (at - scene.lastChat) / kFs; };
    std::cout << "    [E22] Game strip +" << gameGainDb << " dB, chat -20 LUFS: room down to " << during << " dB while it talks; clicks Game / output "
              << room.gameClicks << " / " << room.outClicks << " (offset ceiling alone " << offset.gameClicks << " / " << offset.outClicks
              << "); silent: room " << silentRoom << " dB, Game strip identical to the offset ceiling alone " << sameOnceSilent << "; the duck idle "
              << after (idleFrom) << " s after the chat (offset ceiling alone " << after (offsetIdleFrom) << " s)\n";
    CHECK (during < 0.0f); // the room acted while the chat talked
    CHECK (room.gameClicks == 0);
    CHECK (room.outClicks == 0);
    CHECK (silentRoom == 0.0f);
    CHECK (duckingWhileSilent); // (the window lies before the duck idles)
    CHECK (sameOnceSilent);
    // The hangover (0.6 s) and the duck's release to 1e-3 (300 ms x ln 1000,
    // 2.1 s): idle about 2.7 s after the chat.
    CHECK (idleFrom >= 0);
    CHECK (idleFrom <= scene.lastChat + samplesOf (3.0));
}
} // namespace

TEST_CASE ("E22 room: Game strip at 0 dB - no click under the chat, the offset ceiling alone once it is silent, idle after it")
{
    gainRow (0.0f);
}

TEST_CASE ("E22 room: Game strip at +2 dB - no click under the chat, the offset ceiling alone once it is silent, idle after it")
{
    gainRow (2.0f);
}

TEST_CASE ("E22 room: Game strip at +6 dB - no click under the chat, the offset ceiling alone once it is silent, idle after it")
{
    gainRow (6.0f);
}

TEST_CASE ("E22 room: Game strip at +12 dB - no click under the chat, the offset ceiling alone once it is silent, idle after it")
{
    gainRow (12.0f);
}

TEST_CASE ("E22 room: a muted Game strip and ChatMix fully towards Chat give no inf / NaN, and no room on a silent share")
{
    const auto& scene = sceneFor (-14.0);
    const int n = scene.game.numSamples();
    auto m = makeEngine (64);
    m->setChatDuck (true);
    bool finite = true;
    float roomWhileSilent = 0.0f, roomOtherwise = 0.0f;
    RunOptions o;
    o.block = 64;
    o.beforeBlock = [&] (int pos) {
        m->setChatMix (pos >= samplesOf (2.0) && pos < samplesOf (4.0) ? 1.0f : 0.0f); // the Game at gain 0 from 2.05 s
        m->setStripMuted (kGame, pos >= samplesOf (4.5));
    };
    o.afterBlock = [&] (int pos) {
        const auto* d = m->getChatDucker (kGame);
        finite = finite && std::isfinite (d->getCeilingGainDb()) && std::isfinite (d->getRoomDb());
        if ((pos >= samplesOf (2.1) && pos < samplesOf (4.0)) || pos >= samplesOf (4.6))
            roomWhileSilent = std::min (roomWhileSilent, d->getRoomDb());
        else
            roomOtherwise = std::min (roomOtherwise, d->getRoomDb());
    };
    const Planar out = run (*m, { &scene.game, nullptr, &scene.chat }, n, o);
    for (const auto& c : out.ch)
        for (const float v : c)
            finite = finite && std::isfinite (v);
    std::cout << "    [E22] room with the Game at gain 0 (ChatMix +1, then muted): room " << roomWhileSilent << " dB (" << roomOtherwise
              << " dB otherwise), all finite " << finite << "\n";
    CHECK (finite);
    CHECK (roomWhileSilent == 0.0f);
    CHECK (roomOtherwise < -ChatDucker::kCeilingOffsetDb);
}

TEST_CASE ("E22 room: at the deepest floor (-24 dB) under a full-scale chat its gain stays in 0..1 and leaves a game far under the room alone")
{
    // The Game duck at full amount, the chat's share 0.85 (the room on its
    // floor), the Game a 200 Hz tone stepping through -40, -20, -10 and
    // -2 dBFS; the same duck without the room beside it, so the ratio of
    // their outputs is the room's gain alone (the dip and the ceiling
    // limiter are the same in both). Before the knee's half-width was
    // bounded (2026-10-08) it reached below 0 here: a negative gain on the
    // -40 dBFS tone (and 0.33 instead of 0.7 on the -20 dBFS one).
    const int block = 64, segment = samplesOf (0.25), n = 4 * segment;
    const float levelsDb[] = { -40.0f, -20.0f, -10.0f, -2.0f };
    Planar withRoom (2, n);
    for (int s = 0; s < 4; ++s)
    {
        const auto tone = sine (200.0, kFs, segment, dbToGain (levelsDb[s]));
        for (auto& c : withRoom.ch)
            std::copy (tone.begin(), tone.end(), c.begin() + s * segment);
    }
    Planar without = withRoom;
    const std::vector<float> chatLevel (static_cast<size_t> (n), 0.85f);
    ChatDucker a, b;
    for (auto* d : { &a, &b })
    {
        d->prepare (kFs, ChatDucker::Shape::Game);
        d->reset (1.0f);
    }
    ChatDucker::Control dc;
    dc.voiceActive = true;
    dc.ceilingDb = -1.0f;
    dc.roomCeiling = dbToGain (-1.0f);
    dc.roomFloorDb = ChatDucker::kMinRoomFloorDb;
    for (int pos = 0; pos < n; pos += block)
    {
        dc.chatLevel = chatLevel.data() + pos;
        a.process (withRoom.block (pos, block), dc);
        dc.chatLevel = nullptr;
        b.process (without.block (pos, block), dc);
    }
    bool inRange = true, quietUntouched = true;
    double lowest = 1.0, loudPeak = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const float x = without.ch[0][static_cast<size_t> (i)], y = withRoom.ch[0][static_cast<size_t> (i)];
        inRange = inRange && std::isfinite (y);
        if (i < segment)
            quietUntouched = quietUntouched && x == y;
        if (std::abs (x) < 1.0e-6f)
            continue;
        const double ratio = static_cast<double> (y) / static_cast<double> (x);
        inRange = inRange && ratio >= 0.0 && ratio <= 1.0 + 1.0e-6;
        lowest = std::min (lowest, ratio);
        if (i >= 3 * segment + samplesOf (0.1))
            loudPeak = std::max (loudPeak, static_cast<double> (std::abs (y)));
    }
    const double floorLevel = dbToGain (-1.0f) * dbToGain (ChatDucker::kMinRoomFloorDb);
    std::cout << "    [E22] room at the -24 dB floor under a 0.85 chat share: gain in 0..1 " << inRange << ", the -40 dBFS tone untouched " << quietUntouched
              << ", deepest gain " << toDb (lowest) << " dB, the -2 dBFS tone's peak " << toDb (loudPeak) << " dBFS (the floor " << toDb (floorLevel)
              << " dBFS)\n";
    CHECK (inRange);
    CHECK (quietUntouched);
    CHECK (lowest < 0.2);                     // the room acts on the loud tone
    CHECK (loudPeak >= floorLevel * 0.999);   // never under its floor
    CHECK (loudPeak <= floorLevel * 1.3);     // and near it (the floor's knee)
}

TEST_CASE ("E22 room: the device correction's largest gain after its preamp lowers the room (up to the offset ceiling)")
{
    // DeviceCorrection::getMaxGain: a bell at 80 Hz with its gain as the
    // allowance keeps that gain; with none, the automatic preamp takes it to 0.
    const auto bellAt80 = [] (float gainDb) {
        DeviceCorrectionSettings s;
        CorrectionFilter bell;
        bell.frequency = 80.0f;
        bell.gainDb = gainDb;
        bell.q = 1.0f;
        s.curve.add (bell);
        s.allowanceDb = gainDb;
        return s;
    };
    const DeviceCorrectionSettings boost2 = bellAt80 (2.0f), boost6 = bellAt80 (6.0f);
    REQUIRE (boost2.curve.numFilters == 1);
    REQUIRE (boost6.curve.numFilters == 1);
    {
        DeviceCorrection dc;
        dc.prepare ({ kFs, 256, 2 });
        CHECK (dc.getMaxGain() == 1.0f); // flat
        dc.setSettingsNow (boost6);
        CHECK_NEAR (gainToDb (dc.getMaxGain()), 6.0f, 0.05f);
        dc.setSettingsNow (boost2);
        CHECK_NEAR (gainToDb (dc.getMaxGain()), 2.0f, 0.05f);
        auto none = boost6;
        none.allowanceDb = 0.0f;
        dc.setSettingsNow (none);
        CHECK_NEAR (gainToDb (dc.getMaxGain()), 0.0f, 0.02f);
        auto compare = boost6;
        compare.compare = true;
        dc.setSettingsNow (compare);
        CHECK_NEAR (gainToDb (dc.getMaxGain()), 0.0f, 0.01f); // flat filters, the preamp (0 dB here) kept
    }
    // In the engine, chat at -20 LUFS, the explosions' rumble boosted after
    // the sum. +2 dB: the master ceiling over the correction's gain
    // (-3 dBFS) is above the offset ceiling (-4 dBFS), so the room counts
    // the boost. +6 dB: the offset ceiling is higher (-4 against -7 dBFS);
    // the room is measured from it and takes only the chat's share off it
    // (a silent chat leaves the offset ceiling alone): the rest of the
    // boost is the master limiter's, as with the offset ceiling alone.
    ExplosionOptions offset2, room2, offset6, room6;
    offset2.floorDb = offset6.floorDb = 0.0f;
    offset2.correction = room2.correction = &boost2;
    offset6.correction = room6.correction = &boost6;
    const auto r = runAll (sceneFor (-20.0), { offset2, room2, offset6, room6 });
    std::cout << "    [E22] 80 Hz correction, chat -20 LUFS: +2 dB offset ceiling alone -" << r[0].chatDropDb << " dB, with the room -" << r[1].chatDropDb
              << " dB (room " << r[1].roomDb << " dB); +6 dB offset ceiling alone -" << r[2].chatDropDb << " dB, with the room -" << r[3].chatDropDb
              << " dB (room " << r[3].roomDb << " dB)\n";
    CHECK (r[1].chatDropDb < r[0].chatDropDb - 0.1);
    CHECK (r[1].roomDb < -ChatDucker::kCeilingOffsetDb);
    CHECK (r[3].chatDropDb <= r[2].chatDropDb + 0.01);
    CHECK (r[3].roomDb < 0.0f);
}

TEST_CASE ("E22 room: CPU (a measurement) - the Game duck with and without the room, and the chat envelope")
{
    // A measurement (the numbers vary from run to run and machine to
    // machine): best of five passes over 1 s at 64 and 480-sample blocks,
    // the duck fully in on the rumble, the chat a loud teammate. The check
    // is a loose relative bound: the room may cost the duck a few times its
    // own time, not an order of magnitude (+ 20 ns for the timer's noise).
    const int n = samplesOf (1.0);
    const Planar rumble = tones (n, { 55.0, 110.0 }, 0.445f);
    Planar chat = stereo (formantSpeech (n, 7, 0.0));
    scaleToLufs (chat, -14.0);
    for (const int block : { 64, 480 })
    {
        const auto timeOf = [&] (bool withRoom, bool envelopeOnly) {
            double best = 1.0e9;
            for (int pass = 0; pass < 5; ++pass)
            {
                ChatDucker d;
                d.prepare (kFs, ChatDucker::Shape::Game);
                d.reset (1.0f);
                ChatRoomEnvelope env;
                env.prepare (kFs, block);
                Planar x = rumble, c = chat;
                ChatDucker::Control dc;
                dc.voiceActive = true;
                dc.roomCeiling = dbToGain (-1.0f);
                const auto start = std::chrono::steady_clock::now();
                for (int pos = 0; pos + block <= n; pos += block)
                {
                    const AudioBlock cb = c.block (pos, block);
                    if (withRoom || envelopeOnly)
                        dc.chatLevel = env.process (&cb, block, 1.0f, 1.0f);
                    if (! envelopeOnly)
                    {
                        if (! withRoom)
                            dc.chatLevel = nullptr;
                        d.process (x.block (pos, block), dc);
                    }
                }
                best = std::min (best, std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count());
            }
            return 1.0e9 * best / static_cast<double> (n); // ns per sample
        };
        const double plain = timeOf (false, false), roomed = timeOf (true, false), envelope = timeOf (false, true);
        char line[200];
        std::snprintf (line, sizeof (line), "    [E22] CPU, %d-sample blocks: Game duck %.1f ns / sample, with the room %.1f (the envelope alone %.1f)\n", block, plain,
                       roomed, envelope);
        std::cout << line;
        CHECK (roomed < 4.0 * plain + 20.0);
    }
}

TEST_CASE ("E22 duck: the Game chain's Voice & Score lift is taken back while chat is active")
{
    // The cancel section is the lift's exact inverse: a 2 kHz tone through
    // a +4 dB bell (the lift) and the Game duck at full amount reads the same
    // as without both.
    {
        const int n = samplesOf (1.0);
        const auto lift = SvfCoeffs::make (FilterType::Bell, ChatDucker::kVoiceLiftHz, ChatDucker::kVoiceLiftQ, 4.0, kFs);
        Planar lifted = tones (n, { 2000.0, 700.0 }, 0.01f), plainTone = lifted;
        for (auto& ch : lifted.ch)
        {
            SvfState st;
            for (auto& v : ch)
                v = svfTick (lift, st, v);
        }
        ChatDucker d1, d2;
        d1.prepare (kFs, ChatDucker::Shape::Game);
        d2.prepare (kFs, ChatDucker::Shape::Game);
        d1.reset (1.0f);
        d2.reset (1.0f);
        ChatDucker::Control withLift, noLift;
        withLift.voiceActive = noLift.voiceActive = true;
        withLift.liftDb = 4.0f;
        for (int pos = 0; pos < n; pos += kBlock)
        {
            d1.process (lifted.block (pos, std::min (kBlock, n - pos)), withLift);
            d2.process (plainTone.block (pos, std::min (kBlock, n - pos)), noLift);
        }
        const int tail = samplesOf (0.5);
        for (const double hz : { 2000.0, 700.0 })
            CHECK_NEAR (toneDb (lifted, n - tail, tail, hz), toneDb (plainTone, n - tail, tail, hz), 0.01);
    }

    // In the engine: a Gaming Game strip with Voice & Score 100 on a quiet
    // 2 kHz tone (under the band's -36 dB threshold, so it lifts); the duck
    // reads the chain's band-7 gain and cancels it while chat talks.
    const int n = samplesOf (6.0);
    auto m = makeEngine (kBlock, false);
    for (int s : { kMusic, kChat })
        m->params (s).set (param::BypassAll, 1.0f);
    m->params (kGame).set (param::Mode, static_cast<float> (param::ModeValue::Gaming));
    m->params (kGame).set (param::Macro5, 1.0f);
    m->setChatDuck (true);
    const Planar game = tones (n, { 2000.0 }, 0.003f);
    Planar chat = stereo (formantSpeech (n, 11, 1.0, 5.0));
    scaleToLufs (chat, -20.0);
    float liftDb = 0.0f, cancelDb = 0.0f, idleCancelDb = 1.0f;
    RunOptions o;
    o.afterBlock = [&] (int pos) {
        const float lift = m->chain (kGame).meters().dynEqGainDb[static_cast<size_t> (ProcessingChain::kFirstModeBand + 3)].load();
        if (pos < samplesOf (1.0))
            idleCancelDb = std::min (idleCancelDb, m->getChatDucker (kGame)->getLiftCancelDb() + 1.0f);
        if (m->getChatDuckAmount() > 0.999f && lift > liftDb)
        {
            liftDb = lift;
            cancelDb = m->getChatDucker (kGame)->getLiftCancelDb();
        }
    };
    run (*m, { &game, nullptr, &chat }, n, o);
    std::cout << "    [E22] Voice & Score lift " << liftDb << " dB, cancelled by " << cancelDb << " dB while chat talks\n";
    CHECK (liftDb > 3.0f);
    CHECK_NEAR (cancelDb, -liftDb, 0.2);
    CHECK (idleCancelDb == 1.0f); // nothing before the speech
}

// ---- ChatMix -----------------------------------------------------------------

TEST_CASE ("E22 ChatMix: complementary gains, 0/0 at centre, no zipper noise")
{
    const int n = samplesOf (3.0);
    const Planar game = tones (n, { 1000.0 }, 0.2f), chat = tones (n, { 440.0 }, 0.2f);

    // Complementary, and the law.
    for (const float b : { -1.0f, -0.6f, -0.2f, 0.0f, 0.2f, 0.6f, 1.0f })
    {
        CHECK (MixEngine::chatMixGain (b, MixEngine::StripRole::Game) == MixEngine::chatMixGain (-b, MixEngine::StripRole::Chat));
        CHECK (MixEngine::chatMixGain (b, MixEngine::StripRole::Music) == 1.0f);
        auto m = makeEngine();
        m->setChatMix (b);
        const Planar out = run (*m, { &game, nullptr, &chat }, samplesOf (0.5));
        const int w = samplesOf (0.2), at = samplesOf (0.3);
        const double gGame = std::pow (10.0, (toneDb (out, at, w, 1000.0) - toneDb (game, at, w, 1000.0)) / 20.0);
        const double gChat = std::pow (10.0, (toneDb (out, at, w, 440.0) - toneDb (chat, at, w, 440.0)) / 20.0);
        CHECK_NEAR (gGame, MixEngine::chatMixGain (b, MixEngine::StripRole::Game), 0.002);
        CHECK_NEAR (gChat, MixEngine::chatMixGain (b, MixEngine::StripRole::Chat), 0.002);
        CHECK (std::min (gGame, gChat) >= 0.0);
        CHECK (std::max (gGame, gChat) > 0.998); // one side always at 0 dB
    }

    // 0/0 at the centre: bit-identical to an engine that never heard of
    // ChatMix, also after a round trip away and back.
    {
        auto fresh = makeEngine(), centred = makeEngine(), roundTrip = makeEngine();
        centred->setChatMix (0.0f);
        const Planar a = run (*fresh, { &game, nullptr, &chat }, n);
        const Planar b = run (*centred, { &game, nullptr, &chat }, n);
        RunOptions o;
        o.beforeBlock = [&] (int pos) { roundTrip->setChatMix (pos < samplesOf (0.5) ? 0.7f : 0.0f); };
        const Planar c = run (*roundTrip, { &game, nullptr, &chat }, n, o);
        CHECK (identical (a, b, 0, n));
        CHECK (identical (a, c, samplesOf (1.0), n));
    }

    // A sweep from Game to Chat, a new balance every block, and a jump back:
    // no click (a zipper would be a break at every block).
    {
        auto m = makeEngine();
        RunOptions o;
        o.beforeBlock = [&] (int pos) {
            const double t = pos / kFs;
            m->setChatMix (t < 2.0 ? static_cast<float> (t - 1.0) : -1.0f);
        };
        const Planar out = run (*m, { &game, nullptr, &chat }, n, o);
        DiscontinuitySettings ds;
        ds.blockSize = kBlock;
        DiscontinuityDetector det;
        det.prepare (kFs, 2, ds);
        det.process (out.ptrs.data(), n);
        det.finish();
        std::cout << "    [E22] ChatMix sweep and jump: " << det.count (DiscontinuityType::Click) << " clicks\n";
        CHECK (det.total() == 0);
        // The per-block gain step while sweeping: at most one block's share
        // of the balance (the glide spreads each step over kChatMixRampMs).
        double worstStep = 0.0;
        const int latency = m->getLatencySamples();
        double prev = -1.0;
        for (int pos = samplesOf (0.2) + latency; pos + 1200 <= samplesOf (1.95); pos += kBlock)
        {
            const double g = toneAmplitude (out.ch[0].data() + pos, 1200, 1000.0, kFs); // 25 ms: whole periods of both tones
            if (prev >= 0.0)
                worstStep = std::max (worstStep, std::abs (g - prev));
            prev = g;
        }
        std::cout << "    [E22] ChatMix sweep: largest change of the Game tone between blocks " << worstStep / 0.2 * 100.0 << " % of its level\n";
        CHECK (worstStep < 0.2 * 0.01); // < 1 % of the tone per block
    }
}

// ---- engine swap, idle freeze ------------------------------------------------

TEST_CASE ("E22 engine swap carries the chat settings and an active talker; a frozen Game strip wakes ducked")
{
    const int n = samplesOf (3.0);
    Planar chat = stereo (formantSpeech (n, 11, 0.1));
    scaleToLufs (chat, -20.0);
    const Planar game = tones (n, { 1250.0 }, 0.05f);
    auto m = makeEngine();
    m->setChatDuck (true, 6.0f);
    m->setChatMix (0.3f);
    CHECK (m->getChatRoomFloorDb() == ChatDucker::kDefaultRoomFloorDb);
    m->setChatRoomFloorDb (-9.0f);
    run (*m, { &game, nullptr, &chat }, samplesOf (1.5));
    REQUIRE (m->isChatVoiceActive());
    const float amount = m->getChatDuckAmount();
    CHECK (amount > 0.9f);

    MixEngine next;
    const std::vector<StripConfig> layout { { "Game", 2, 0.0f, false }, { "Music", 2, 0.0f, false }, { "Chat", 2, 0.0f, false } };
    next.configureFrom (*m, layout, kFs, kBlock);
    CHECK (next.getChatDuck());
    CHECK (next.getChatDuckDepthDb() == 6.0f);
    CHECK (next.getChatMix() == 0.3f);
    CHECK (next.getChatRoomFloorDb() == -9.0f);
    {
        MixEngine e;
        e.setChatRoomFloorDb (3.0f);
        CHECK (e.getChatRoomFloorDb() == 0.0f);
        e.setChatRoomFloorDb (-40.0f);
        CHECK (e.getChatRoomFloorDb() == ChatDucker::kMinRoomFloorDb);
        e.setChatRoomFloorDb (std::numeric_limits<float>::quiet_NaN());
        CHECK (e.getChatRoomFloorDb() == ChatDucker::kDefaultRoomFloorDb);
    }
    CHECK (next.isChatVoiceActive());
    CHECK (next.getChatDuckAmount() == amount);
    CHECK (next.getChatDucker (kGame)->getAmount() == amount);

    // Idle freeze: a silent Game strip freezes; the duck moves on while it
    // is frozen, so it wakes with the dip already in.
    auto f = makeEngine();
    f->setIdleFreeze (true);
    f->setIdleHoldSeconds (0.1);
    f->setChatDuck (true);
    const Planar silence (2, n);
    Planar late = tones (n, { 1250.0 }, 0.05f);
    for (auto& c : late.ch)
        std::fill (c.begin(), c.begin() + samplesOf (2.0), 0.0f);
    bool frozenWhileTalking = false;
    float amountAtWake = 0.0f;
    RunOptions o;
    o.afterBlock = [&] (int pos) {
        frozenWhileTalking = frozenWhileTalking || (f->isStripFrozen (kGame) && f->getChatDuckAmount() > 0.9f);
        if (pos / kBlock == samplesOf (2.0) / kBlock)
            amountAtWake = f->getChatDucker (kGame)->getAmount();
    };
    run (*f, { &late, nullptr, &chat }, n, o);
    CHECK (frozenWhileTalking);
    CHECK (amountAtWake > 0.9f);
    CHECK (! f->isStripFrozen (kGame));
}
