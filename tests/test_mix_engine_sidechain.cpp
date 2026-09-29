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
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>

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
            m.process (ptrs.data(), dst);
            if (o.allocations != nullptr)
                *o.allocations += guard.allocations();
        }
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
/** Game: an ambience at about -45 dBFS and, every 2 s from 1 s, an
    explosion (low rumble plus a broadband crack, 0.8 s decay), limited the
    way a game's own mix is (a tanh stage) and scaled to a -1 dBFS sample
    peak; Chat: speech throughout at `lufs`. Returns how far the chat's
    short-term level falls at the output, at worst from 3 s on: its input
    through the master limiter's gain (the deepest of each 64-sample block)
    against the chat alone. */
double explosionChatDropDb (bool duck, double lufs, float& gameCeilingGainDb)
{
    const int n = samplesOf (6.0), block = 64;
    Planar game (2, n);
    {
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
            game.ch[0][static_cast<size_t> (i)] = v;
            game.ch[1][static_cast<size_t> (i)] = v;
        }
        scale (game, dbToGain (-1.0f) / peakAbs (game.ch[0].data(), n));
    }
    Planar chat = stereo (formantSpeech (n, 7));
    scaleToLufs (chat, lufs);
    {
        auto m = makeEngine (block);
        m->setChatDuck (duck);
        std::vector<float> gr;
        RunOptions o;
        o.block = block;
        gameCeilingGainDb = 0.0f;
        o.afterBlock = [&] (int) {
            gr.push_back (m->getMasterGainReductionDb());
            gameCeilingGainDb = std::min (gameCeilingGainDb, m->getChatDucker (kGame)->getCeilingGainDb());
        };
        run (*m, { &game, nullptr, &chat }, n, o);
        Planar through = chat;
        const int latency = m->getLatencySamples();
        for (int i = 0; i < n; ++i)
        {
            const size_t k = static_cast<size_t> (std::min (n - 1, i + latency) / block);
            const float g = dbToGain (std::min (0.0f, gr[std::min (k, gr.size() - 1)]));
            through.ch[0][static_cast<size_t> (i)] *= g;
            through.ch[1][static_cast<size_t> (i)] *= g;
        }
        LoudnessMeter alone, ducked;
        alone.prepare (kFs, 2);
        ducked.prepare (kFs, 2);
        double worst = 0.0;
        const int step = samplesOf (0.1);
        for (int pos = 0; pos + step <= n; pos += step)
        {
            alone.process (chat.block (pos, step));
            ducked.process (through.block (pos, step));
            if (pos >= samplesOf (3.0))
                worst = std::max (worst, static_cast<double> (alone.getShortTermLufs() - ducked.getShortTermLufs()));
        }
        return worst;
    }
}

void checkExplosions (double lufs)
{
    {
        float ceilingOff = 0.0f, ceilingOn = 0.0f;
        const double before = explosionChatDropDb (false, lufs, ceilingOff), after = explosionChatDropDb (true, lufs, ceilingOn);
                std::cout << "    [E22] chat at " << lufs << " LUFS, short-term level under -1 dBTP explosions: duck off -" << before << " dB, on -" << after
                  << " dB (Game ceiling offset deepest " << ceilingOn << " dB)\n";
        // The Done-when row is the Voice Chat level; a teammate 6 dB louder
        // still overshoots the master with the Game 3 dB down (the
        // sub-limiter that would hold it is not built, docs/11 E22).
        if (lufs <= -20.0)
            CHECK (after < 0.5);
        CHECK (after < before);
        CHECK (ceilingOff == 0.0f);
        CHECK (ceilingOn < -2.5f);
    }
}
} // namespace

TEST_CASE ("E22 duck: -1 dBTP explosions on Game change the Chat short-term level < 0.5 dB (Chat at -20 LUFS)")
{
    checkExplosions (-20.0); // E23's Voice Chat level
}

TEST_CASE ("E22 duck: -1 dBTP explosions on Game, a loud teammate (Chat at -14 LUFS): less ducking than without")
{
    checkExplosions (-14.0);
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
