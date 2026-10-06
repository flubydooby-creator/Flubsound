#include "Demo.h"

#include "Commands.h"
#include "FactoryPresets.h"

#include "flub/common/Denormals.h"
#include "flub/common/Math.h"
#include "flub/engine/MacroMap.h"
#include "flub/engine/MixEngine.h"
#include "flub/io/FilePath.h"
#include "flub/io/ParametricEqText.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>

namespace flub::cli
{
namespace fs = std::filesystem;
using namespace param;

namespace
{
constexpr double kRate = 48000.0; // the built-in programmes

// ===========================================================================
// Synthesis helpers (double precision, fixed seeds: identical every run)
// ===========================================================================
/** RBJ biquad, transposed direct form II. */
struct Biquad
{
    enum Type
    {
        LowPass,
        HighPass,
        BandPass // constant 0 dB peak
    };

    Biquad (Type type, double freqHz, double q) noexcept
    {
        const double w0 = kTwoPi * freqHz / kRate, cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        switch (type)
        {
            case LowPass: b0 = b2 = (1.0 - cw) / 2.0 / a0; b1 = (1.0 - cw) / a0; break;
            case HighPass: b0 = b2 = (1.0 + cw) / 2.0 / a0; b1 = -(1.0 + cw) / a0; break;
            case BandPass: b0 = alpha / a0; b1 = 0.0; b2 = -alpha / a0; break;
        }
        a1 = -2.0 * cw / a0;
        a2 = (1.0 - alpha) / a0;
    }

    double process (double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0, z1 = 0.0, z2 = 0.0;
};

/** Paul Kellet's refined pink filter on FastRandom white noise. */
struct Pink
{
    explicit Pink (uint32_t seed) noexcept : rng (seed) {}

    double next() noexcept
    {
        const double w = rng.nextBipolar();
        b0 = 0.99886 * b0 + w * 0.0555179;
        b1 = 0.99332 * b1 + w * 0.0750759;
        b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856;
        b4 = 0.55000 * b4 + w * 0.5329522;
        b5 = -0.7616 * b5 - w * 0.0168980;
        const double p = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
        b6 = w * 0.115926;
        return p * 0.17; // about 0.3 RMS
    }

    FastRandom rng;
    double b0 = 0.0, b1 = 0.0, b2 = 0.0, b3 = 0.0, b4 = 0.0, b5 = 0.0, b6 = 0.0;
};

int framesFor (double seconds) noexcept
{
    return std::max (1, static_cast<int> (std::lround (seconds * kRate)));
}

/** Raised-cosine attack / release envelope of a segment [0, length) at s. */
double segmentEnvelope (double s, double length, double attack, double release) noexcept
{
    if (s < 0.0 || s >= length)
        return 0.0;
    double g = 1.0;
    if (s < attack)
        g = 0.5 - 0.5 * std::cos (kPi * s / attack);
    if (length - s < release)
        g = std::min (g, 0.5 - 0.5 * std::cos (kPi * (length - s) / release));
    return g;
}

/** Scales every channel so the programme's sample peak is `peakDb` dBFS. */
void normalisePeak (std::vector<std::vector<float>>& channels, double peakDb)
{
    float peak = 0.0f;
    for (const auto& c : channels)
        for (float v : c)
            peak = std::max (peak, std::abs (v));
    if (peak <= 0.0f)
        return;
    const float g = static_cast<float> (std::pow (10.0, peakDb / 20.0) / static_cast<double> (peak));
    for (auto& c : channels)
        for (float& v : c)
            v *= g;
}

// ---- A formant voice (speech programme, the game scene's voice line) --------
struct Vowel
{
    double f1, f2, f3;
};
constexpr Vowel kVowels[] = { { 730.0, 1090.0, 2440.0 }, { 530.0, 1840.0, 2480.0 }, { 270.0, 2290.0, 3010.0 },
                              { 570.0, 840.0, 2410.0 },  { 300.0, 870.0, 2240.0 } };

double formantGain (double hz, const Vowel& v) noexcept
{
    auto peak = [hz] (double f, double bw) { return 1.0 / (1.0 + ((hz - f) / bw) * ((hz - f) / bw)); };
    return peak (v.f1, 90.0) + 0.7 * peak (v.f2, 110.0) + 0.4 * peak (v.f3, 170.0);
}

/** Adds syllables (a sibilant 's' / 'sh', a plosive or no consonant, then a
    vowel on a harmonic source through three formants) in phrases with pauses
    to out[begin, end), peak about `peak`. */
void addVoice (std::vector<float>& out, int begin, int end, uint32_t seed, double f0Base, double peak)
{
    FastRandom rng (seed);
    auto uniform = [&rng] { return 0.5 + 0.5 * static_cast<double> (rng.nextBipolar()); }; // [0, 1)
    Biquad sBand (Biquad::BandPass, 6500.0, 1.0), shBand (Biquad::BandPass, 3500.0, 1.2);
    std::vector<double> voice (static_cast<size_t> (std::max (0, end - begin)), 0.0);
    const int n = static_cast<int> (voice.size());
    int pos = static_cast<int> (0.05 * kRate);
    double phase = 0.0;
    while (pos < n)
    {
        const int syllables = 5 + static_cast<int> (uniform() * 5.0);
        for (int syl = 0; syl < syllables && pos < n; ++syl)
        {
            const double phrasePos = static_cast<double> (syl) / static_cast<double> (syllables);
            const int consonant = static_cast<int> (uniform() * 4.0); // 0 none, 1 's', 2 'sh', 3 plosive
            if (consonant == 1 || consonant == 2)
            {
                const double len = (consonant == 1 ? 0.11 : 0.10) + 0.03 * uniform();
                auto& band = consonant == 1 ? sBand : shBand;
                const int frames = framesFor (len);
                for (int i = 0; i < frames && pos + i < n; ++i)
                {
                    const double s = i / kRate;
                    voice[static_cast<size_t> (pos + i)] += 0.9 * segmentEnvelope (s, len, 0.02, 0.03) * band.process (rng.nextBipolar());
                }
                pos += frames;
            }
            else if (consonant == 3)
            {
                const int frames = framesFor (0.008);
                for (int i = 0; i < frames && pos + i < n; ++i)
                    voice[static_cast<size_t> (pos + i)] += 0.35 * std::exp (-i / (0.002 * kRate)) * rng.nextBipolar();
                pos += frames + framesFor (0.02);
            }

            const Vowel& v = kVowels[static_cast<size_t> (uniform() * 5.0) % 5];
            const double len = 0.12 + 0.10 * uniform();
            const double f0Start = f0Base * (1.12 - 0.25 * phrasePos) * (0.97 + 0.06 * uniform());
            const int frames = framesFor (len);
            for (int i = 0; i < frames && pos + i < n; ++i)
            {
                const double s = i / kRate;
                const double f0 = f0Start * (1.0 - 0.06 * s / len) * (1.0 + 0.004 * std::sin (kTwoPi * 5.5 * s));
                phase += kTwoPi * f0 / kRate;
                if (phase > kTwoPi * 1024.0)
                    phase -= kTwoPi * 1024.0;
                double x = 0.0;
                for (int k = 1; k * f0 < 4000.0; ++k)
                    x += formantGain (k * f0, v) / k * std::sin (k * phase);
                voice[static_cast<size_t> (pos + i)] += 0.5 * segmentEnvelope (s, len, 0.015, 0.03) * x;
            }
            pos += frames;
        }
        pos += framesFor (0.25 + 0.2 * uniform()); // pause between phrases
    }

    double maxAbs = 0.0;
    for (double x : voice)
        maxAbs = std::max (maxAbs, std::abs (x));
    const double g = maxAbs > 0.0 ? peak / maxAbs : 0.0;
    for (int i = 0; i < n; ++i)
        out[static_cast<size_t> (begin + i)] += static_cast<float> (g * voice[static_cast<size_t> (i)]);
}

// ---- Chords (the music programme's pad, the game scene's score) -------------
struct Bar
{
    double root;       // bass (Hz)
    double chord[3];   // pad / melody notes (Hz)
};
constexpr Bar kBars[] = { { 55.0, { 220.0, 261.63, 329.63 } },   // A minor
                          { 43.65, { 174.61, 220.0, 261.63 } },  // F
                          { 65.41, { 261.63, 329.63, 392.0 } },  // C
                          { 49.0, { 196.0, 246.94, 293.66 } } }; // G
constexpr double kBarSeconds = 2.0, kBeatSeconds = 0.5;

const Bar& barAt (double t) noexcept
{
    return kBars[static_cast<size_t> (t / kBarSeconds) % 4];
}

/** The chord pad at t: three notes, eight harmonics each, detuned by 0.4 Hz
    on the right; faded over 30 ms at each chord change. */
void padAt (double t, double& left, double& right) noexcept
{
    const Bar& bar = barAt (t);
    const double inBar = std::fmod (t, kBarSeconds);
    const double env = segmentEnvelope (inBar, kBarSeconds, 0.03, 0.03);
    left = right = 0.0;
    for (double f : bar.chord)
        for (int k = 1; k <= 8; ++k)
        {
            const double a = 1.0 / (k * k);
            left += a * std::sin (kTwoPi * k * f * t);
            right += a * std::sin (kTwoPi * k * (f + 0.4) * t + 0.7);
        }
    left *= env;
    right *= env;
}

// ===========================================================================
// Programmes
// ===========================================================================
io::AudioFileData stereoData (std::vector<float> left, std::vector<float> right)
{
    io::AudioFileData d;
    d.sampleRate = kRate;
    d.numChannels = 2;
    d.sourceFormat = io::SampleFormat::Float32;
    d.channels.push_back (std::move (left));
    d.channels.push_back (std::move (right));
    return d;
}

/** 120 BPM: a chirp kick on every beat, snare on 2 and 4, eighth-note hats
    (right of centre), a plucked bass line, a wide chord pad and a sung lead
    (formant vowels, vibrato) in the centre; A minor - F - C - G; peak -1 dBFS. */
io::AudioFileData makeMusic (double seconds)
{
    const int n = framesFor (seconds);
    std::vector<float> left (static_cast<size_t> (n)), right (static_cast<size_t> (n));
    FastRandom rng (1234);
    Biquad snareBand (Biquad::BandPass, 1800.0, 0.7), hatHigh (Biquad::HighPass, 7000.0, 0.7);
    double leadPhase = 0.0, leadF0 = 220.0;
    Vowel vowel = kVowels[0];
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kRate;
        const double beat = std::fmod (t, kBeatSeconds);
        const auto beatIndex = static_cast<int> (t / kBeatSeconds);
        const Bar& bar = barAt (t);

        const double kick = std::exp (-beat * 18.0) * std::sin (kTwoPi * (50.0 + 80.0 * std::exp (-beat * 30.0)) * beat);

        const double inTwoBeats = std::fmod (t, 2.0 * kBeatSeconds) - kBeatSeconds; // snare on beats 2 and 4
        const double snareNoise = snareBand.process (rng.nextBipolar());
        double snare = 0.0;
        if (inTwoBeats >= 0.0 && inTwoBeats < 0.25)
            snare = std::exp (-inTwoBeats / 0.04) * std::sin (kTwoPi * 185.0 * inTwoBeats) + 1.6 * std::exp (-inTwoBeats / 0.06) * snareNoise;

        const double hatNoise = hatHigh.process (rng.nextBipolar());
        const double eighth = std::fmod (t, 0.25);
        const double hat = eighth < 0.06 ? std::exp (-eighth / 0.012) * hatNoise : 0.0;

        const double bassEnv = segmentEnvelope (beat, kBeatSeconds, 0.005, 0.01) * (0.5 + 0.5 * std::exp (-beat * 4.0));
        double bass = 0.0;
        for (int k = 1; k <= 5; ++k)
            bass += std::sin (kTwoPi * k * bar.root * t) / k;
        bass *= bassEnv;

        double padL = 0.0, padR = 0.0;
        padAt (t, padL, padR);

        // The lead: one chord tone per beat, an octave up, gliding (30 ms).
        const double target = bar.chord[static_cast<size_t> (beatIndex % 3)] * (beatIndex % 4 == 3 ? 1.0 : 2.0);
        leadF0 += (target - leadF0) * (1.0 - std::exp (-1.0 / (0.03 * kRate)));
        const Vowel& want = kVowels[static_cast<size_t> (beatIndex % 4)];
        const double vc = 1.0 - std::exp (-1.0 / (0.04 * kRate));
        vowel = { vowel.f1 + (want.f1 - vowel.f1) * vc, vowel.f2 + (want.f2 - vowel.f2) * vc, vowel.f3 + (want.f3 - vowel.f3) * vc };
        const double f0 = leadF0 * (1.0 + 0.006 * std::sin (kTwoPi * 5.5 * t));
        leadPhase += kTwoPi * f0 / kRate;
        if (leadPhase > kTwoPi * 1024.0)
            leadPhase -= kTwoPi * 1024.0;
        double lead = 0.0;
        for (int k = 1; k * f0 < 6000.0; ++k)
            lead += formantGain (k * f0, vowel) / k * std::sin (k * leadPhase);
        lead *= 0.7 + 0.3 * segmentEnvelope (beat, kBeatSeconds, 0.03, 0.05);

        const double centre = 0.9 * kick + 0.35 * snare + 0.3 * bass + 0.12 * lead;
        left[static_cast<size_t> (i)] = static_cast<float> (centre + 0.09 * hat + 0.05 * padL);
        right[static_cast<size_t> (i)] = static_cast<float> (centre + 0.13 * hat + 0.05 * padR);
    }
    auto d = stereoData (std::move (left), std::move (right));
    normalisePeak (d.channels, -1.0);
    return d;
}

/** A formant voice (male, about 120 Hz) with sibilants, plosives and
    pauses between phrases, centred; peak -3 dBFS. */
io::AudioFileData makeSpeech (double seconds)
{
    const int n = framesFor (seconds);
    std::vector<float> mono (static_cast<size_t> (n));
    addVoice (mono, 0, n, 2718, 120.0, 1.0);
    auto d = stereoData (mono, mono);
    normalisePeak (d.channels, -3.0);
    return d;
}

/** The music programme mastered loud, for Smart macros: 14 dB into a tanh
    soft clipper, then peak -1 dBFS (a limited master: Smart's analysis reads
    its PLR under 7.5 LU, where it takes the macros' attack back). */
io::AudioFileData makeLoudMusic (double seconds)
{
    auto d = makeMusic (seconds);
    const double drive = std::pow (10.0, 14.0 / 20.0);
    for (auto& c : d.channels)
        for (float& v : c)
            v = static_cast<float> (std::tanh (drive * static_cast<double> (v)));
    normalisePeak (d.channels, -1.0);
    return d;
}

/** The speech programme over a steady hiss floor (pink, -50 dBFS RMS per
    channel, uncorrelated), for the noise gate. */
io::AudioFileData makeSpeechHiss (double seconds)
{
    auto d = makeSpeech (seconds);
    const double level = std::pow (10.0, -50.0 / 20.0) / 0.3;
    for (size_t c = 0; c < d.channels.size(); ++c)
    {
        Pink pink (static_cast<uint32_t> (909 + c));
        for (float& v : d.channels[c])
            v += static_cast<float> (level * pink.next());
    }
    return d;
}

/** The chat scene's teammate: a higher formant voice (about 180 Hz) talking
    from 25 to 75 % of the programme, centred, peak -9 dBFS. */
io::AudioFileData makeChatVoice (double seconds)
{
    const int n = framesFor (seconds);
    std::vector<float> mono (static_cast<size_t> (n));
    addVoice (mono, framesFor (0.25 * seconds), framesFor (0.75 * seconds), 3141, 180.0, 1.0);
    auto d = stereoData (mono, mono);
    normalisePeak (d.channels, -9.0);
    return d;
}

// Speaker azimuths (degrees) of the 7.1 bed, WAVE_FORMAT_EXTENSIBLE order
// FL FR FC LFE BL BR SL SR (the LFE has none).
constexpr int kLfe = 3;
constexpr double kBedAzimuth[8] = { -30.0, 30.0, 0.0, 0.0, -150.0, 150.0, -90.0, 90.0 };

/** Per-channel gains of a source at `azimuth` (degrees, 0 = front, + = right):
    constant-power stereo panning on sin(azimuth), or pairwise constant-power
    panning between the two adjacent speakers of the 7.1 ring. */
std::vector<double> panGains (double azimuth, bool surround)
{
    if (! surround)
    {
        const double p = std::sin (azimuth * kPi / 180.0);
        return { std::cos ((p + 1.0) * kPi / 4.0), std::sin ((p + 1.0) * kPi / 4.0) };
    }
    std::vector<double> g (8, 0.0);
    constexpr int ring[] = { 4, 6, 0, 2, 1, 7, 5 }; // BL SL FL FC FR SR BR, by azimuth
    double a = std::fmod (azimuth + 180.0, 360.0);
    if (a < 0.0)
        a += 360.0;
    a -= 180.0; // [-180, 180)
    for (size_t k = 0; k < std::size (ring); ++k)
    {
        const int c0 = ring[k], c1 = ring[(k + 1) % std::size (ring)];
        const double a0 = kBedAzimuth[c0];
        double a1 = kBedAzimuth[c1], x = a;
        if (a1 <= a0)
            a1 += 360.0;
        if (x < a0)
            x += 360.0;
        if (x >= a0 && x < a1)
        {
            const double frac = (x - a0) / (a1 - a0);
            g[static_cast<size_t> (c0)] = std::cos (frac * kPi / 2.0);
            g[static_cast<size_t> (c1)] = std::sin (frac * kPi / 2.0);
            break;
        }
    }
    return g;
}

void addSource (std::vector<std::vector<float>>& out, const std::vector<double>& gains, int start, const std::vector<double>& mono)
{
    const auto n = static_cast<int> (out[0].size());
    for (size_t c = 0; c < gains.size(); ++c)
        if (gains[c] != 0.0)
            for (int i = 0; i < static_cast<int> (mono.size()) && start + i < n; ++i)
                out[c][static_cast<size_t> (start + i)] += static_cast<float> (gains[c] * mono[static_cast<size_t> (i)]);
}

/** The game scene: an ambience bed (-46 dBFS RMS pink per channel, slowly
    moving), footsteps every 420 ms (a thump and a crunch) walking from left
    to right (7.1: once around the listener) over the first 35 % and again
    from 50 %, three gunshots right of centre at 20 %, explosions (a crack, a
    rumble to 150 Hz and a 38 Hz tone, sent to the LFE in 7.1) at 42 % and
    80 %, a voice line from 55 to 75 % and a quiet chord pad as the score
    (front left / right). */
io::AudioFileData makeGameScene (double seconds, bool surround)
{
    const int n = framesFor (seconds);
    const size_t numChannels = surround ? 8 : 2;
    std::vector<std::vector<float>> out (numChannels, std::vector<float> (static_cast<size_t> (n), 0.0f));
    FastRandom rng (4242);

    // Ambience and score.
    for (size_t c = 0; c < numChannels; ++c)
    {
        if (static_cast<int> (c) == kLfe && surround)
            continue;
        Pink pink (static_cast<uint32_t> (777 + c));
        const double level = std::pow (10.0, (surround ? -49.0 : -46.0) / 20.0) / 0.3;
        for (int i = 0; i < n; ++i)
        {
            const double t = i / kRate;
            out[c][static_cast<size_t> (i)] += static_cast<float> (level * (0.75 + 0.25 * std::sin (kTwoPi * 0.13 * t + static_cast<double> (c))) * pink.next());
        }
    }
    for (int i = 0; i < n; ++i)
    {
        double l = 0.0, r = 0.0;
        padAt (i / kRate, l, r);
        out[0][static_cast<size_t> (i)] += static_cast<float> (0.004 * l);
        out[1][static_cast<size_t> (i)] += static_cast<float> (0.004 * r);
    }

    // Footsteps.
    Biquad crunchBand (Biquad::BandPass, 2500.0, 0.8);
    auto walk = [&] (double from, double to) {
        const int first = framesFor (from), last = framesFor (to);
        const int step = framesFor (0.42);
        const int count = std::max (1, (last - first) / step);
        for (int s = 0; s < count; ++s)
        {
            const double progress = count > 1 ? static_cast<double> (s) / (count - 1) : 0.5;
            const double azimuth = surround ? -180.0 + 360.0 * progress : -80.0 + 160.0 * progress;
            const double gain = 0.85 + 0.15 * static_cast<double> (rng.nextBipolar());
            std::vector<double> x (static_cast<size_t> (framesFor (0.08)));
            for (size_t i = 0; i < x.size(); ++i)
            {
                const double t = static_cast<double> (i) / kRate;
                x[i] = gain * (0.12 * std::exp (-t / 0.02) * std::sin (kTwoPi * 80.0 * t)
                               + 0.25 * std::exp (-t / 0.025) * crunchBand.process (rng.nextBipolar()));
            }
            addSource (out, panGains (azimuth, surround), first + s * step, x);
        }
    };
    walk (0.02 * seconds, 0.35 * seconds);
    walk (0.50 * seconds, 0.98 * seconds);

    // Gunshots.
    for (int shot = 0; shot < 3; ++shot)
    {
        std::vector<double> x (static_cast<size_t> (framesFor (0.12)));
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = static_cast<double> (i) / kRate;
            x[i] = 0.45 * std::exp (-t / 0.015) * rng.nextBipolar() + 0.25 * std::exp (-t / 0.03) * std::sin (kTwoPi * 120.0 * t);
        }
        addSource (out, panGains (40.0, surround), framesFor (0.2 * seconds + 0.12 * shot), x);
    }

    // Explosions.
    for (double at : { 0.42, 0.80 })
    {
        Biquad rumbleLow (Biquad::LowPass, 150.0, 0.707);
        std::vector<double> x (static_cast<size_t> (framesFor (2.5))), low (x.size());
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = static_cast<double> (i) / kRate;
            const double attack = 1.0 - std::exp (-t / 0.004);
            low[i] = attack * (2.2 * std::exp (-t / 0.7) * rumbleLow.process (rng.nextBipolar()) + 0.3 * std::exp (-t / 0.9) * std::sin (kTwoPi * 38.0 * t));
            x[i] = low[i] + 0.3 * std::exp (-t / 0.03) * rng.nextBipolar();
        }
        const int start = framesFor (at * seconds);
        addSource (out, panGains (0.0, surround), start, x);
        if (surround)
        {
            std::vector<double> lfe (8, 0.0);
            lfe[kLfe] = 0.5;
            addSource (out, lfe, start, low);
        }
    }

    // The voice line.
    {
        std::vector<float> voice (static_cast<size_t> (n), 0.0f);
        addVoice (voice, framesFor (0.55 * seconds), framesFor (0.75 * seconds), 1618, 140.0, 0.12);
        std::vector<double> x (voice.begin(), voice.end());
        addSource (out, panGains (0.0, surround), 0, x);
    }

    // Keep the scene under -1 dBFS (the explosions are its peaks).
    float peak = 0.0f;
    for (const auto& c : out)
        for (float v : c)
            peak = std::max (peak, std::abs (v));
    if (peak > 0.89f)
        normalisePeak (out, -1.0);

    io::AudioFileData d;
    d.sampleRate = kRate;
    d.numChannels = static_cast<int> (numChannels);
    d.sourceFormat = io::SampleFormat::Float32;
    d.channels = std::move (out);
    return d;
}

// ===========================================================================
// The pair list
// ===========================================================================
ParamSetting setting (const char* key, std::string value)
{
    return { key, std::move (value) };
}

std::string numberText (float v)
{
    char buf[32];
    std::snprintf (buf, sizeof (buf), "%g", static_cast<double> (v));
    return buf;
}

/** The dynamics the app's Night latch copies from Night Mode Gaming
    (EngineController::getNightOverrides, docs/11 E21), with its values as a
    fallback when the factory preset cannot be read. */
constexpr std::pair<int, float> kNightDynamics[] = {
    { AutoLevelOn, 1.0f },     { AutoLevelTargetLufs, -14.0f },   { GuardRange, static_cast<float> (GuardRangeValue::Lu20) },
    { CompressorOn, 1.0f },    { CompThresholdDb, -18.0f },       { CompRatio, 3.0f },
    { CompKneeDb, 10.0f },     { CompAttackMs, 3.0f },            { CompReleaseMs, 250.0f },
    { CompAutoRelease, 1.0f }, { CompMakeupDb, 0.0f },            { CompUpThresholdDb, -32.0f },
    { CompUpRatio, 2.5f },     { CompUpMaxGainDb, 6.0f },         { CompUpFloorDb, -62.0f },
};

std::string formatSettings (const std::vector<ParamSetting>& settings)
{
    std::string s = "--set";
    for (const auto& p : settings)
    {
        const std::string word = p.key + "=" + p.value;
        s += word.find (' ') != std::string::npos ? " \"" + word + "\"" : " " + word;
    }
    return s;
}

const char* modeName (ModeValue mode) noexcept
{
    return mode == ModeValue::Music ? "Music" : "Gaming";
}

std::string lowerSlug (const std::string& name)
{
    std::string s;
    for (unsigned char c : name)
    {
        if (std::isalnum (c))
            s += static_cast<char> (std::tolower (c));
        else if (! s.empty() && s.back() != '-')
            s += '-';
    }
    while (! s.empty() && s.back() == '-')
        s.pop_back();
    return s;
}
} // namespace

std::vector<DemoPairSpec> demoPairs (const std::string& presetDir, std::string* note)
{
    std::vector<DemoPairSpec> pairs;
    auto add = [&pairs] (std::string slug, std::string title, std::string programme, std::vector<ParamSetting> before,
                         std::vector<ParamSetting> after, bool levelFeature, std::string listenFor) -> DemoPairSpec& {
        DemoPairSpec p;
        p.slug = std::move (slug);
        p.title = std::move (title);
        p.programme = std::move (programme);
        p.before = std::move (before);
        p.after = std::move (after);
        p.levelFeature = levelFeature;
        p.listenFor = std::move (listenFor);
        pairs.push_back (std::move (p));
        return pairs.back();
    };

    // ---- The macros, 0 -> 100 % at Boost 0 ----------------------------------
    static const char* const kMusicListen[5] = {
        "The kick and the snare hit harder at their start - more attack, a firmer thump - while the level between the hits "
        "stays where it was. Listen to the first few milliseconds of each drum hit.",
        "The pad and the hats spread further to the sides; the kick, the bass and the voice stay in the centre. On headphones "
        "the image gets wider; switch your player to mono and nothing should disappear.",
        "The voice and the hats come forward and the low mids (the boxy 200 - 500 Hz region) get cleaner. Too much reads as "
        "thin or bright: check the snare and the voice's vowels.",
        "Not level-matched (a level feature): the after file is louder by design. Listen past the level: the drums lose some "
        "of their jump (flatter dynamics) - the price of loudness; the readouts show how hard the limiter worked.",
        "A darker, softer top and a fuller low end with a slight tube colour: the hats get gentler, the voice rounder. "
        "Nothing should sound muffled.",
    };
    static const char* const kGamingListen[5] = {
        "The footsteps (short thumps with a crunch, walking from left to right) stand out more when they happen, while the "
        "ambience between them stays where it was.",
        "The steps' path from left to right and the gunshots right of centre get easier to place; each sound sits at a "
        "sharper point in the image.",
        "The gunshots and the two explosions hit bigger: more low end and a harder attack on the loud events; the quiet "
        "parts stay as they were.",
        "Quiet sounds - the ambience, the steps - come up closer to the loud ones; the explosions stand less far above "
        "the rest.",
        "The voice line (about 55 - 75 % into the scene) and the quiet music under it come through the effects more "
        "clearly, with less low-mid boom.",
    };
    for (int mode = 0; mode < 2; ++mode)
    {
        const auto m = static_cast<ModeValue> (mode);
        for (int i = 0; i < 5; ++i)
        {
            const std::string name = MacroMap::macroName (m, i);
            const std::string key = "macro." + std::to_string (i + 1);
            const bool music = m == ModeValue::Music;
            add (std::string (music ? "music-" : "gaming-") + lowerSlug (name), std::string (modeName (m)) + " - " + name + " 0 -> 100 %",
                 music ? "music" : "game", { setting ("mode", modeName (m)) }, { setting ("mode", modeName (m)), setting (key.c_str(), "100%") },
                 music && i == 3, music ? kMusicListen[i] : kGamingListen[i]);
        }
    }

    // ---- Boost 0 -> 50 / 100 ------------------------------------------------
    for (int mode = 0; mode < 2; ++mode)
    {
        const auto m = static_cast<ModeValue> (mode);
        const bool music = m == ModeValue::Music;
        for (int boost : { 50, 100 })
            add (std::string (music ? "music" : "gaming") + "-boost-" + std::to_string (boost),
                 std::string (modeName (m)) + " - Boost 0 -> " + std::to_string (boost) + " %", music ? "music" : "game",
                 { setting ("mode", modeName (m)) }, { setting ("mode", modeName (m)), setting ("boost", std::to_string (boost) + "%") }, false,
                 boost == 50 ? (music ? "Boost's first half: clarity and width first, then some bass. Matched, so listen for detail "
                                        "and space rather than level."
                                      : "Boost's first half: detail and direction first, then some impact. Matched, so listen for "
                                        "the steps and the placement rather than level.")
                             : (music ? "Full Boost: clarity, width, bass and loudness processing together, watched by the safety "
                                        "governor. Listen for denser drums and for any harshness or pumping; the readouts say how "
                                        "hard the limiter worked."
                                      : "Full Boost: detail, direction, impact and loudness together, watched by the safety governor. "
                                        "Listen for the steps against the explosions and for any pumping of the ambience after a "
                                        "blast."));
    }

    // ---- Module switches ----------------------------------------------------
    add ("smoothness", "Music - Smoothness 0 -> 100 % (at Boost 100, Clarity 100)", "speech",
         { setting ("mode", "Music"), setting ("boost", "100%"), setting ("macro.3", "100%") },
         { setting ("mode", "Music"), setting ("boost", "100%"), setting ("macro.3", "100%"), setting ("smooth.amount", "100%") }, false,
         "Both sides run Boost 100 and Clarity 100, which lift the sibilant band; Smoothness takes that back. Listen to the "
         "'s' and 'sh' sounds: softer and less piercing after, while the vowels keep their clarity.");
    add ("crossfeed", "Music - Headphone crossfeed 0 -> 50 %", "music", { setting ("mode", "Music") },
         { setting ("mode", "Music"), setting ("spatial.crossfeed", "50%") }, false,
         "Headphones only. A little of each side reaches the other ear, as from speakers: the hats and the wide pad move "
         "slightly inwards and the sound feels less inside your head. The centre does not change.");
    add ("virtualiser", "Gaming - Headphone virtualiser off -> on (7.1 scene)", "game-7.1",
         { setting ("mode", "Gaming"), setting ("virt.on", "off") }, { setting ("mode", "Gaming"), setting ("virt.on", "on") }, false,
         "Headphones only. Before: the 7.1 scene folded to plain stereo (ITU-R BS.775). After: the binaural virtualiser - "
         "the footsteps that circle you should sound outside and around your head, the rear positions behind you.");
    add ("contour", "Music - Loudness contour off -> on (listening 30 dB below the reference)", "music",
         { setting ("mode", "Music"), setting ("contour.level", "-30") },
         { setting ("mode", "Music"), setting ("contour.level", "-30"), setting ("contour.on", "on") }, false,
         "Play this pair quietly. Both sides are set 30 dB below the reference listening level (contour.level), as at a "
         "quiet evening volume, where the ear loses the bass first. The contour lifts the low end against the mids and "
         "highs, and its level trim keeps the loudness about the same: after, the bass line and the kick stay audible "
         "and the mids sit back.");
    add ("startle-guard", "Gaming - Startle Guard (Dynamic Range) Off -> 10 LU", "game", { setting ("mode", "Gaming") },
         { setting ("mode", "Gaming"), setting ("guard.range", "10 LU (Balanced)") }, true,
         "Not level-matched (a level feature). The gunshots and explosions are held to about 10 LU over what you were "
         "hearing just before them; the steps and the ambience stay the same.");

    std::vector<ParamSetting> night { setting ("mode", "Gaming") };
    {
        preset::Preset p;
        std::string from, error;
        const bool fromPreset = resolvePreset ("gaming-night-mode", presetDir, p, from, error) && p.values.size() == static_cast<size_t> (kNumParams);
        for (const auto& [id, fallback] : kNightDynamics)
            night.push_back ({ layout()[static_cast<size_t> (id)].key, numberText (fromPreset ? p.values[static_cast<size_t> (id)] : fallback) });
        if (note != nullptr && ! fromPreset)
            *note = "Night uses its built-in copy of Night Mode Gaming's dynamics (the factory preset was not found: " + error + ")";
    }
    add ("night", "Gaming - Night off -> on", "game", { setting ("mode", "Gaming") }, std::move (night), true,
         "Not level-matched (a level feature). Night copies the Night Mode Gaming dynamics (Auto Level to -14 LUFS, a "
         "compressor that also lifts quiet sounds, Startle Guard at 20 LU), meant to make the scene even: judge whether "
         "the quiet parts come up and the explosions stop jumping out; the readouts give the compressor's and Auto "
         "Level's work.");

    static const char* const kStyleListen[4] = {
        "Transparent leaves the work to the limiter, with a little soft clipping and a slow release: the cleanest loudness. "
        "Listen for the drums keeping their shape.",
        "The clipper takes more of each transient, so the limiter ducks the kick's start less: harder drum attacks at the "
        "same loudness.",
        "Most of the drive goes into the clipper: loud and dense. Listen for grit on the drums and on the voice.",
        "Limiter only, no clipping, a slow release: the fewest artefacts; the drums may sound a little softer than Custom.",
    };
    const auto& styles = layout()[static_cast<size_t> (MaxStyle)].choices;
    for (int s = 1; s <= 4 && static_cast<size_t> (s) < styles.size(); ++s)
        add ("max-style-" + lowerSlug (styles[static_cast<size_t> (s)]), "Music - Maximizer style Custom -> " + styles[static_cast<size_t> (s)] + " (drive 9 dB)",
             "music", { setting ("mode", "Music"), setting ("max.drive", "9") },
             { setting ("mode", "Music"), setting ("max.drive", "9"), setting ("max.style", styles[static_cast<size_t> (s)]) }, false,
             std::string ("Both sides drive the maximizer by 9 dB; before uses the Custom (default) clipper and release. ") + kStyleListen[s - 1]);

    // ---- Batch 4 - 5 features and the module cards (docs/12-feature-guide.md) ----
    const ParamSetting music = setting ("mode", "Music"), gaming = setting ("mode", "Gaming");

    // Punch and Impact against a driven maximizer (docs/11 E04 step 4, E20; Punch's attack is off from
    // Boost 70 %, docs/11 E04 owner decision 2026-10-06, so the Punch pair's sides are the same).
    add ("music-punch-boost-100", "Music - Punch 0 -> 100 % at Boost 100", "music", { music, setting ("boost", "100%") },
         { music, setting ("boost", "100%"), setting ("macro.1", "100%") }, false,
         "Both sides run Boost 100, so the maximizer is working hard. Punch's attack fades out from Boost 60 to 70 % "
         "(owner decision 2026-10-06: its onset lift ticked in the maximizer up there), so the two sides should sound "
         "the same: no ticks at the start of the kicks and snares on the after side. Punch's own sound is in music-punch.");
    add ("gaming-impact-boost-100", "Gaming - Impact 0 -> 100 % at Boost 100", "game", { gaming, setting ("boost", "100%") },
         { gaming, setting ("boost", "100%"), setting ("macro.3", "100%") }, false,
         "Both sides run Boost 100. The explosions and gunshots start with a bigger low-end thump; the rumble after them "
         "and the ambience stay where they were, and the footsteps right after a blast should stay as audible as before.");

    // The transient shaper's bands (docs/11 E04 step 3).
    add ("attack-low", "Music - Transient attack, low band (clarity.attackLow) 0 -> +6 dB", "music", { music },
         { music, setting ("clarity.attackLow", "6") }, false,
         "Only the kick's and the bass's onsets get harder: a firmer thump at the start of each kick. The hats and the "
         "voice do not change.");
    add ("attack-high", "Music - Transient attack, high band (clarity.attackHigh) 0 -> +6 dB", "music", { music },
         { music, setting ("clarity.attackHigh", "6") }, false,
         "Only the onsets above 4 kHz get sharper: the hats' ticks and the snare's crack. The kick and the bass line do "
         "not change.");

    // Presence against the programme's level (docs/11 E07 step 3).
    add ("relative-presence", "Music - Presence mode Absolute -> Relative (Clarity 100 on a quiet master, input -20 dB)", "music",
         { music, setting ("input.gain", "-20"), setting ("macro.3", "100%") },
         { music, setting ("input.gain", "-20"), setting ("macro.3", "100%"), setting ("clarity.presenceMode", "Relative") }, false,
         "Both sides play the music 20 dB quieter than mastered, with Clarity at 100. Absolute presence lifts a quiet "
         "master more than a loud one; Relative reads the presence band against the music's own body and lifts it as it "
         "would at a normal level. After should sound less bright and less forward in the 2 - 5 kHz region (voice, snare), "
         "with nothing else changed.");

    // Crossfeed types (docs/11 E12).
    add ("crossfeed-meier", "Music - Crossfeed type Bs2b -> Meier (crossfeed 50 %)", "music",
         { music, setting ("spatial.crossfeed", "50%") }, { music, setting ("spatial.crossfeed", "50%"), setting ("spatial.crossfeedType", "Meier") },
         false,
         "Headphones only. Both sides feed a little of each side to the other ear; Meier's model keeps more of the top "
         "end and the centre's tone. Listen to the hats and the pad at the sides: a slightly clearer, less dull image.");
    add ("crossfeed-mono-safe", "Music - Crossfeed type Bs2b -> Mono-safe (crossfeed 50 %)", "music",
         { music, setting ("spatial.crossfeed", "50%") },
         { music, setting ("spatial.crossfeed", "50%"), setting ("spatial.crossfeedType", "Mono-safe") }, false,
         "Headphones only. Mono-safe crossfeeds without changing what a mono sum hears: the centre (kick, bass, voice) "
         "keeps its level and tone exactly; the sides move inwards a little.");

    // The virtualiser's renderer (docs/11 E28).
    add ("enhanced-renderer", "Gaming - Virtualiser renderer Classic -> Enhanced (7.1 scene)", "game-7.1", { gaming },
         { gaming, setting ("virt.renderer", "Enhanced") }, false,
         "Headphones only. Enhanced adds direction cues: sounds in front get a brighter 4 kHz and 13 kHz edge, sounds "
         "behind a 1 kHz and 10 kHz colour, and the centre voice keeps the sides' tone. The footsteps circling you should "
         "be easier to place in front or behind.");
    add ("virt-front-back", "Gaming - Enhanced renderer, front/back contrast 50 -> 100 % (7.1 scene)", "game-7.1",
         { gaming, setting ("virt.renderer", "Enhanced") },
         { gaming, setting ("virt.renderer", "Enhanced"), setting ("virt.frontBack", "100%") }, false,
         "Headphones only. Both sides use the Enhanced renderer; after doubles its front / back colour. The steps behind "
         "you should sound more clearly behind, at the price of a more coloured sound in front and behind.");

    // The five genre presets (docs/11 E14 step 3) and the voice / night presets.
    struct PresetPair
    {
        const char *slug, *file, *name, *programme;
        bool level;
        const char* listen;
    };
    static const PresetPair kPresets[] = {
        { "preset-rock-metal", "music-rock-metal", "Rock & Metal", "music", false,
          "Less boxiness at 250 - 500 Hz (about -1 dB), a little more bite at 1 - 2 kHz and a softer fizz above 4 kHz: "
          "guitars and drums with edge, without mud." },
        { "preset-orchestral-film", "music-orchestral-film", "Orchestral & Film", "music", false,
          "A deeper low end (+1.4 - 1.7 dB at 31 - 63 Hz), a slightly softer 2 - 4 kHz and headphone crossfeed; no "
          "maximizer drive, clipper or compression, so the drums keep their full jump." },
        { "preset-acoustic-singer-songwriter", "music-acoustic-singer-songwriter", "Acoustic & Singer-Songwriter", "music", false,
          "Warmth at 35 %: more body at 125 - 250 Hz (about +0.8 dB) and a gentler top, with a light tube colour on the "
          "voice. It should sound closer and rounder, not muffled." },
        { "preset-rnb-vocal", "music-rnb-vocal", "R&B & Vocal", "music", false,
          "A smooth, deep low end (+1.1 - 1.4 dB at 31 - 63 Hz, mono below 100 Hz), the voice a little forward at 2 kHz and "
          "a softer top above 8 kHz." },
        { "preset-electronic-ambient", "music-electronic-ambient", "Electronic & Ambient", "music", false,
          "A deeper sub (+2.3 dB at 31 Hz), the rest of the spectrum within 0.3 dB, and a wider, more spacious pad." },
        { "preset-late-night", "music-late-night-low-volume", "Late Night Low Volume", "music", true,
          "Not level-matched (a level feature): Late Night aims at about -20 LUFS with the quiet parts lifted and the loud "
          "ones held down. Play it quietly: every part of the song should stay audible without the drums jumping out." },
        { "preset-podcast-voice", "music-podcast-voice", "Podcast & Voice", "speech", true,
          "Not level-matched (a level feature): speech levelled and brought up, with the rumble and boxiness removed and "
          "the consonants clearer. Quiet and loud phrases should come out at about the same level." },
        { "preset-voice-chat", "music-voice-chat", "Voice Chat", "speech", true,
          "Not level-matched (a level feature): the Chat strip's preset. Rumble below 110 Hz and boxiness go, presence "
          "comes up, and Auto Level evens the talker out: easier to understand at a lower volume." },
    };
    for (const auto& pp : kPresets)
        add (pp.slug, std::string ("Music - defaults -> ") + pp.name + " (factory preset)", pp.programme, { music }, {}, pp.level, pp.listen)
            .afterPreset = pp.file;

    // Module cards (docs/12 section 4).
    add ("noise-gate", "Speech - Noise gate off -> on (Quality profile)", "speech-hiss", { music, setting ("latency.profile", "Quality") },
         { music, setting ("latency.profile", "Quality"), setting ("gate.on", "on") }, false,
         "The hiss under the voice drops in the pauses between phrases and under the voice it is lower; the voice itself "
         "should not sound watery or chopped. (The gate runs only in the Quality latency profile.)");
    add ("eq-bell", "Music - Parametric EQ band 8 (4 kHz) 0 -> +6 dB bell", "music", { music }, { music, setting ("eq.7.gain", "6") }, false,
         "A plain tone change: the 2 - 8 kHz region (the voice's edge, the snare's crack, the hats' body) comes forward.");
    add ("dynamic-eq", "Music - Dynamic EQ band 3 (3.5 kHz) cuts above -30 dB", "music", { music },
         { music, setting ("dyneq.2.on", "on"), setting ("dyneq.2.threshold", "-30"), setting ("dyneq.2.range", "6") }, false,
         "The 3.5 kHz region is turned down only while it is loud: the loud vowels and snare hits lose their edge, the "
         "quiet passages keep theirs.");
    add ("bass-boost", "Music - Bass engine boost 0 -> +6 dB (70 Hz; input -12 dB)", "music", { music, setting ("input.gain", "-12") },
         { music, setting ("input.gain", "-12"), setting ("bass.boost", "6") }, false,
         "More low end below about 100 Hz: the kick's thump and the bass line's weight. Both sides play the music 12 dB "
         "down, as a quieter recording: on a loud master the engine's headroom protection (-12 dB) holds almost all of "
         "the boost back instead of letting it distort.");
    add ("bass-harmonics", "Music - Harmonic bass 0 -> 60 %", "music", { music }, { music, setting ("bass.harmonics", "60%") }, false,
         "Overtones of the bass line and the kick: they sound deeper on small speakers and earbuds that cannot play the "
         "lowest notes. On good headphones: a little more growl on the bass.");
    add ("bass-tighten", "Music - Bass Tighten 0 -> 50 %", "music", { music }, { music, setting ("bass.tighten", "50%") }, false,
         "Each kick's tail gets shorter while its first hit stays: a drier, tighter low end with less boom between the "
         "beats.");
    add ("saturation", "Music - Saturation off -> Tube, drive 12 dB", "music", { music },
         { music, setting ("sat.on", "on"), setting ("sat.type", "Tube"), setting ("sat.drive", "12") }, false,
         "Added harmonics: a denser, rounder, slightly gritty sound on the voice and the bass. Too much sounds fuzzy.");
    add ("tape-grit", "Music - Warmth 100 %: tone -> Tape grit", "music", { music, setting ("macro.5", "100%") },
         { music, setting ("macro.5", "100%"), setting ("warmth.tapeGrit", "on") }, false,
         "Both sides at Warmth 100. Before is Warmth's tone tilt (more body at 200 Hz, a softer top); after is the classic "
         "tape grit (hard tape drive, more bass and harmonics, no tilt): more distortion and bass, less of the soft top.");
    add ("compressor", "Music - Compressor off -> on (-24 dB, 4:1, automatic make-up)", "music", { music },
         { music, setting ("comp.on", "on"), setting ("comp.threshold", "-24"), setting ("comp.ratio", "4"), setting ("comp.autoMakeup", "on") },
         false,
         "Flatter dynamics: the drums jump less above the rest and the quiet pad and voice come closer to them. Listen "
         "for the drum hits losing their snap, the price of compression.");
    add ("auto-preamp", "Music - Automatic preamp off -> on (Boost 100)", "music", { music, setting ("boost", "100%") },
         { music, setting ("boost", "100%"), setting ("auto.preamp", "on") }, false,
         "Both sides run Boost 100. The automatic preamp turns the input down by what the enhancement adds, so the "
         "maximizer works less: the drums keep more of their attack and the limiter's pumping is less. The readouts give "
         "the limiter's work.");
    add ("latency-profile", "Music - Latency profile Low Latency -> Quality (Boost 100, Warmth 100)", "music",
         { music, setting ("boost", "100%"), setting ("macro.5", "100%"), setting ("latency.profile", "Low Latency") },
         { music, setting ("boost", "100%"), setting ("macro.5", "100%"), setting ("latency.profile", "Quality") }, false,
         "Quality oversamples the saturator and clipper more and gives the limiters more look-ahead: a slightly cleaner "
         "top end (hats, sibilants) on driven material. The difference is small; a blind A/B/X is the fair test.");
    add ("protection-normal", "Music - Protection strength Off -> Normal (Boost 100, every macro 100 %)", "music",
         { music, setting ("boost", "100%"), setting ("macro.1", "100%"), setting ("macro.2", "100%"), setting ("macro.3", "100%"),
           setting ("macro.4", "100%"), setting ("macro.5", "100%") },
         { music, setting ("boost", "100%"), setting ("macro.1", "100%"), setting ("macro.2", "100%"), setting ("macro.3", "100%"),
           setting ("macro.4", "100%"), setting ("macro.5", "100%") },
         false,
         "Everything at 100. At Normal the safety governor also measures the distortion, the dynamics and the brightness "
         "it adds and backs off: less grit, less harshness and more jump in the drums, at a slightly less dense sound.")
        .afterHost.protection = ProtectionStrength::Normal;

    // The app's own settings: rendered through the mix engine (DemoHost).
    DemoHost engine;
    engine.engine = true;
    {
        auto& p = add ("smart-macros", "Music - Smart macros off -> on (Boost 100, Punch 100, Loudness 60 on a loud master)", "music-loud",
                       { music, setting ("boost", "100%"), setting ("macro.1", "100%"), setting ("macro.4", "60%") },
                       { music, setting ("boost", "100%"), setting ("macro.1", "100%"), setting ("macro.4", "60%") }, false,
                       "The programme is already mastered loud. Smart reads that and takes back most of the attack and drive "
                       "the macros add: after should sound less squashed and less distorted, with the drums no flatter than "
                       "the master itself. On an open, dynamic recording Smart changes nothing.");
        p.beforeHost = p.afterHost = engine;
        p.afterHost.smartMacros = true;
    }
    {
        auto& p = add ("onboard-cap", "Gaming - Headset enhancement cap off -> on (Footsteps 100, Detail 100)", "game",
                       { gaming, setting ("macro.1", "100%"), setting ("macro.4", "100%") },
                       { gaming, setting ("macro.1", "100%"), setting ("macro.4", "100%") }, false,
                       "What happens when you tell Flubsound the headset's own enhancement (Superhuman Hearing, on-board EQ) "
                       "is ON: Footsteps and Detail are held at 30 % so the two do not stack. After, the steps and the "
                       "ambience are lifted less - the headset is expected to add its own lift on top.");
        p.beforeHost = p.afterHost = engine;
        p.afterHost.onboardCap = true;
    }
    {
        auto& p = add ("safe-speaker-cap", "Music - Safe speaker bass cap off -> on (+3 dB; bass boost 9 dB, Boost 100, input -12 dB)", "music",
                       { music, setting ("input.gain", "-12"), setting ("boost", "100%"), setting ("bass.boost", "9") },
                       { music, setting ("input.gain", "-12"), setting ("boost", "100%"), setting ("bass.boost", "9") }, false,
                       "What plays when the headset disconnects and Flubsound falls back to speakers: the bass lift is held "
                       "to +3 dB so small speakers are not overdriven. After has much less low end; the mids and highs stay.");
        p.beforeHost = p.afterHost = engine;
        p.afterHost.safeSpeakerBassCapDb = 3.0f;
    }
    {
        auto& p = add ("device-correction", "Music - Headphone correction off -> on (an example curve: +6 dB bass shelf, -4 dB at 3 kHz)", "music",
                       { music }, { music }, false,
                       "A headphone correction curve (Settings > Correction imports AutoEQ / Equalizer APO files) on the "
                       "whole output: here an example curve with more bass and a dip at 3 kHz. Its automatic preamp keeps it "
                       "from clipping. With your own headset's file the change should sound like a more neutral headphone.");
        p.beforeHost = p.afterHost = engine;
        p.afterHost.correctionText = "Preamp: -6.0 dB\nFilter 1: ON LSC Fc 105 Hz Gain 6.0 dB Q 0.70\nFilter 2: ON PK Fc 3000 Hz Gain -4.0 dB Q 1.40\n";
    }
    {
        auto& p = add ("per-ear", "Music - Personal profile off -> right ear +9 dB at 4 - 8 kHz", "music", { music }, { music }, false,
                       "Headphones only. The right ear gets brighter (4, 6 and 8 kHz +9 dB); both ears are turned down by the "
                       "headroom reservation first, so the left ear sounds a little quieter. Listen with each ear: the hats "
                       "and the voice's edge move towards the right.");
        p.beforeHost = p.afterHost = engine;
        p.afterHost.personal.enabled = true;
        for (int b = 5; b < PersonalProfile::kNumBands; ++b)
            p.afterHost.personal.bandDb[1][static_cast<size_t> (b)] = 9.0f;
    }
    {
        auto& p = add ("hearing-cap", "Music - Listening-level cap off -> on at 75 dB(A) (sensitivity 110 dB SPL, full volume)", "music",
                       { music }, { music }, true,
                       "Not level-matched (a level feature). With a known sensitivity the hearing guard estimates the level "
                       "at your ear; the cap holds its 5 s average at 75 dB(A). After is quieter by the amount the estimate "
                       "was over, gliding down over about a second, with the tone unchanged.");
        p.beforeHost = p.afterHost = engine;
        p.beforeHost.sensitivityDbSpl = p.afterHost.sensitivityDbSpl = 110.0f;
        p.afterHost.capOn = true;
        p.afterHost.capDbA = 75.0f;
    }
    {
        auto& p = add ("chat-duck", "Game + Chat - Duck game under voice chat off -> on (4.5 dB)", "chat-scene", { gaming, setting ("macro.5", "100%") },
                       { gaming, setting ("macro.5", "100%") }, true,
                       "Not level-matched (a level feature). A teammate talks from 25 to 75 %. With the duck on, the game dips "
                       "4.5 dB around 1 - 2.4 kHz while the teammate talks (the footsteps' bands stay), Voice & Score's lift "
                       "is taken back and the game's peaks are held under the voice: the voice is easier to follow and the "
                       "steps are still there. Before and after the talking nothing changes.");
        p.beforeHost = p.afterHost = engine;
        p.beforeHost.chatStrip = p.afterHost.chatStrip = true;
        p.afterHost.chatDuck = true;
    }
    {
        auto& p = add ("chatmix", "Game + Chat - ChatMix centre -> 50 % towards Chat", "chat-scene", { gaming }, { gaming }, true,
                       "Not level-matched (a level feature). ChatMix moves one balance: towards Chat the game falls "
                       "(-6 dB at 50 %) and the voice stays at 0 dB. The game scene should be quieter under the voice; the "
                       "voice itself does not change.");
        p.beforeHost = p.afterHost = engine;
        p.beforeHost.chatStrip = p.afterHost.chatStrip = true;
        p.afterHost.chatMix = 0.5f;
    }
    return pairs;
}

namespace
{
// ===========================================================================
// Rendering
// ===========================================================================
/** Runs fn (0 .. count - 1) on `workers` threads (the calling thread is one). */
void forEachParallel (size_t count, size_t workers, const std::function<void (size_t)>& fn)
{
    std::atomic<size_t> next { 0 };
    auto work = [&] {
        for (size_t i = next.fetch_add (1); i < count; i = next.fetch_add (1))
            fn (i);
    };
    std::vector<std::thread> threads;
    for (size_t w = 1; w < workers; ++w)
    {
        try
        {
            threads.emplace_back (work);
        }
        catch (const std::system_error&)
        {
            break; // fewer workers: the remaining ones take the jobs
        }
    }
    work();
    for (auto& t : threads)
        t.join();
}

/** One distinct render (sides shared by pairs are rendered once), kept
    until the last pair that uses it has been written. */
/** What the mix engine read during an engine render (DemoHost::engine). */
struct EngineReadings
{
    float masterGrMaxDb = 0.0f;          // the master limiter, deepest (dB <= 0)
    MacroModulation smartMin;            // Smart's multipliers, lowest
    bool onboardCapActive = false;       // the CAPPED chip (MeterBus::onboardCapActive) at any time
    float bassBoostDb = 0.0f;            // bass.boost as the chain applied it, at the end
    float personalReservationDb = 0.0f;  // the per-ear stage's headroom reservation
    bool hearingKnown = false;
    float leq5sMaxDbA = HearingMeters::kUnknown, capGainMinDb = 0.0f;
    float voicePercent = 0.0f, duckMax = 0.0f; // the Chat strip's voice detector, the duck's depth (0..1)
};

struct RenderSlot
{
    size_t programme = 0;
    std::string preset;
    std::vector<ParamSetting> settings;
    DemoHost host;
    std::vector<float> values;
    std::mutex mutex;
    bool done = false, ok = false;
    std::string error;
    RenderResult result;
    EngineReadings readings;
    std::atomic<int> uses { 0 };
};

struct Programme
{
    std::string name, description;
    io::AudioFileData audio;
    io::AudioFileData chat; // "chat-scene": the Chat strip's voice
};

std::string describeBuiltIn (const std::string& name)
{
    if (name == "music")
        return "built-in: 120 BPM drums (kick on every beat, snare, hats right of centre), a plucked bass line, a wide chord "
               "pad and a sung lead in the centre; peak -1 dBFS";
    if (name == "speech")
        return "built-in: a synthetic male voice (formant vowels, 's' / 'sh' sibilants, plosives, pauses between phrases), "
               "centred; peak -3 dBFS";
    if (name == "game")
        return "built-in: ambience, footsteps walking left to right, three gunshots right of centre at 20 %, explosions at "
               "42 % and 80 %, a voice line at 55 - 75 %, a quiet score";
    if (name == "music-loud")
        return "built-in: the music programme mastered loud (14 dB into a soft clipper, peak -1 dBFS; PLR under 7.5 LU)";
    if (name == "speech-hiss")
        return "built-in: the synthetic voice over a steady hiss floor (pink noise, -50 dBFS RMS per channel)";
    if (name == "chat-scene")
        return "built-in: the game scene on a Game strip and a teammate's voice (a higher formant voice, peak -9 dBFS, "
               "talking from 25 to 75 %) on a Chat strip, mixed as the app mixes them";
    return "built-in: the game scene as a 7.1 bed (FL FR FC LFE BL BR SL SR): the footsteps circle the listener, the "
           "explosions reach the LFE, the voice is on the centre";
}

/** A side as `flubsound-cli process` options, plus its app settings. */
std::string formatSide (const std::string& preset, const std::vector<ParamSetting>& settings, const DemoHost& host)
{
    std::string s;
    if (! preset.empty())
        s = "--preset " + preset;
    if (! settings.empty())
        s += (s.empty() ? "" : " ") + formatSettings (settings);
    if (const auto app = host.describe(); ! app.empty())
        s += (s.empty() ? "" : " ") + std::string ("+ app: ") + app;
    return s.empty() ? std::string ("the defaults") : s;
}

/** Renders `p` through a MixEngine as the app runs it (DemoHost::engine):
    the main strip named after the mode (so it takes the Game or Music
    role), a Chat strip fed p.chat when host.chatStrip (its own defaults),
    the host settings given before the first block, the idle freeze off.
    Primed like renderPass (silence through the engine, then every chain
    reset() onto its targets) and compensated by the main strip's latency,
    so the output has the input's length and alignment. Non-RT, allocates. */
bool renderThroughEngine (const Programme& p, const std::vector<float>& values, const DemoHost& host, int blockSize,
                          RenderResult& result, EngineReadings& readings, std::string& error)
{
    result = RenderResult();
    readings = EngineReadings();
    const io::AudioFileData& in = p.audio;
    if (! checkRenderable (in, error))
        return false;
    if (values.size() != static_cast<size_t> (kNumParams))
    {
        error = "internal error: parameter table has the wrong size";
        return false;
    }
    const int inChannels = in.numChannels, mainChannels = inChannels == 1 ? 2 : inChannels;
    const int64_t numFrames = in.numFrames();
    blockSize = std::clamp (blockSize, 16, 16384);
    const bool gaming = std::lround (values[static_cast<size_t> (Mode)]) == static_cast<long> (ModeValue::Gaming);

    std::vector<StripConfig> strips (1);
    strips[0].name = gaming ? "Game" : "Music";
    strips[0].inputChannels = mainChannels;
    if (host.chatStrip)
    {
        StripConfig chat;
        chat.name = "Chat";
        strips.push_back (chat);
    }

    CorrectionCurve curve;
    if (! host.correctionText.empty())
    {
        const auto parsed = eqtext::parse (host.correctionText, curve);
        if (! parsed.ok)
        {
            error = "demo headphone correction: " + parsed.error;
            return false;
        }
    }

    // The main strip's values must be in its store before its chain is
    // prepared (latency.profile is structural): a first engine makes the
    // stores, the engine that renders shares them (configureFrom).
    MixEngine stores;
    stores.configure (strips, in.sampleRate, blockSize);
    for (int id = 0; id < kNumParams; ++id)
        stores.params (0).set (Bank::A, id, values[static_cast<size_t> (id)]);
    stores.params (0).setActiveBank (Bank::A);
    auto engine = std::make_unique<MixEngine>();
    engine->configureFrom (stores, strips, in.sampleRate, blockSize);
    // After configureFrom, whose chains adopt the first engine's host
    // settings (adoptGovernorState); each is taken by the next process()
    // (the per-ear profile crossfades in 20 ms, inside the priming below).
    for (int s = 0; s < engine->getNumStrips(); ++s)
        engine->chain (s).setProtectionStrength (host.protection);
    auto& chain = engine->chain (0);
    chain.setSmartMacros (host.smartMacros);
    chain.setOnboardEnhancementCap (host.onboardCap);
    chain.setSafeSpeakerBassCapDb (host.safeSpeakerBassCapDb);
    if (host.personal.enabled)
        chain.setPersonalProfile (host.personal);
    engine->setIdleFreeze (false);
    if (! host.correctionText.empty())
    {
        DeviceCorrectionSettings settings;
        settings.curve = curve;
        engine->getDeviceCorrection().setSettingsNow (settings);
    }
    auto& guard = engine->getHearingGuard();
    guard.setSensitivityDbSpl (host.sensitivityDbSpl);
    guard.setEndpointVolumeDb (host.endpointVolumeDb);
    guard.setCap (host.capOn, host.capDbA);
    engine->setChatDuck (host.chatDuck, host.chatDuckDepthDb);
    engine->setChatMix (host.chatMix);

    const int latency = engine->getStripLatencySamples (0);
    AudioBuffer mainIo (mainChannels, blockSize), chatIo (2, blockSize), out (2, blockSize);
    std::vector<std::vector<float>> outStereo (2, std::vector<float> (static_cast<size_t> (numFrames), 0.0f));
    ScopedNoDenormals noDenormals;
    auto processBlock = [&] (int n) {
        const AudioBlock mainBlock = mainIo.block (mainChannels, n), chatBlock = chatIo.block (2, n);
        const AudioBlock* inputs[2] = { &mainBlock, &chatBlock };
        engine->process (inputs, out.block (2, n));
    };

    // Prime: 0.25 s of silence (the ChatMix and cap glides land), then snap
    // every chain's smoothers onto their targets.
    mainIo.clear();
    chatIo.clear();
    for (int64_t pos = 0; pos < static_cast<int64_t> (0.25 * in.sampleRate); pos += blockSize)
        processBlock (blockSize);
    for (int s = 0; s < engine->getNumStrips(); ++s)
        engine->chain (s).reset();

    int64_t voiceBlocks = 0, programmeBlocks = 0;
    const int64_t totalFrames = numFrames + latency;
    for (int64_t pos = 0; pos < totalFrames; pos += blockSize)
    {
        const int n = static_cast<int> (std::min<int64_t> (blockSize, totalFrames - pos));
        const int available = static_cast<int> (std::clamp<int64_t> (numFrames - pos, 0, n));
        for (int c = 0; c < mainChannels; ++c)
        {
            float* dst = mainIo.channel (c);
            const auto& src = in.channels[static_cast<size_t> (inChannels == 1 ? 0 : c)];
            if (available > 0)
                std::memcpy (dst, src.data() + pos, sizeof (float) * static_cast<size_t> (available));
            if (available < n)
                std::memset (dst + available, 0, sizeof (float) * static_cast<size_t> (n - available));
        }
        for (int c = 0; c < 2; ++c)
        {
            float* dst = chatIo.channel (c);
            const int64_t chatFrames = p.chat.channels.empty() ? 0 : p.chat.numFrames();
            const int chatAvailable = static_cast<int> (std::clamp<int64_t> (chatFrames - pos, 0, n));
            if (chatAvailable > 0)
                std::memcpy (dst, p.chat.channels[static_cast<size_t> (std::min (c, p.chat.numChannels - 1))].data() + pos,
                             sizeof (float) * static_cast<size_t> (chatAvailable));
            if (chatAvailable < n)
                std::memset (dst + chatAvailable, 0, sizeof (float) * static_cast<size_t> (n - chatAvailable));
        }
        processBlock (n);

        if (available > 0)
        {
            ++programmeBlocks;
            readings.masterGrMaxDb = std::min (readings.masterGrMaxDb, -std::abs (engine->getMasterGainReductionDb()));
            const auto smart = engine->chain (0).getSmartModulation();
            readings.smartMin = { std::min (readings.smartMin.attack, smart.attack), std::min (readings.smartMin.drive, smart.drive),
                                  std::min (readings.smartMin.bass, smart.bass), std::min (readings.smartMin.air, smart.air) };
            readings.onboardCapActive = readings.onboardCapActive || engine->chain (0).meters().onboardCapActive.load (std::memory_order_relaxed);
            const auto& hm = guard.meters();
            if (hm.known.load (std::memory_order_relaxed))
            {
                readings.hearingKnown = true;
                readings.leq5sMaxDbA = std::max (readings.leq5sMaxDbA, hm.leq5sDbA.load (std::memory_order_relaxed));
                readings.capGainMinDb = std::min (readings.capGainMinDb, hm.capGainDb.load (std::memory_order_relaxed));
            }
            if (engine->isChatVoiceActive())
                ++voiceBlocks;
            readings.duckMax = std::max (readings.duckMax, engine->getChatDuckAmount());
        }

        // Keep the main strip's output frames [latency, latency + numFrames).
        const int64_t firstOut = pos - latency;
        const int skip = static_cast<int> (std::clamp<int64_t> (-firstOut, 0, n));
        const int64_t dstStart = firstOut + skip;
        const int count = static_cast<int> (std::clamp<int64_t> (std::min<int64_t> (n - skip, numFrames - dstStart), 0, n));
        if (count > 0)
            for (int c = 0; c < 2; ++c)
                std::memcpy (outStereo[static_cast<size_t> (c)].data() + dstStart, out.channel (c) + skip, sizeof (float) * static_cast<size_t> (count));
    }
    readings.voicePercent = programmeBlocks > 0 ? 100.0f * static_cast<float> (voiceBlocks) / static_cast<float> (programmeBlocks) : 0.0f;
    readings.bassBoostDb = engine->chain (0).effectiveValue (BassBoostDb);
    readings.personalReservationDb = engine->chain (0).getPersonalReservationDb();

    result.output.sampleRate = in.sampleRate;
    result.output.numChannels = 2;
    result.output.sourceFormat = io::SampleFormat::Float32;
    result.output.channels = std::move (outStereo);
    result.outputReport = analyse (result.output.channels, in.sampleRate);
    result.latencySamples = latency;
    result.chainInputChannels = mainChannels;
    result.passes = 1;
    return true;
}

std::string signedDb (float db)
{
    char buf[32];
    std::snprintf (buf, sizeof (buf), "%+.1f", std::abs (db) < 0.05f ? 0.0 : static_cast<double> (db));
    return buf;
}

/** The engine's readouts of one side, for the features the pair's two sides set. */
std::string describeEngine (const EngineReadings& r, const DemoHost& a, const DemoHost& b)
{
    std::string s = "master limiter GR max " + signedDb (r.masterGrMaxDb) + " dB";
    if (a.smartMacros || b.smartMacros)
    {
        char buf[128];
        std::snprintf (buf, sizeof (buf), "; Smart multipliers min: attack %.2f, drive %.2f, bass %.2f, air %.2f", static_cast<double> (r.smartMin.attack),
                       static_cast<double> (r.smartMin.drive), static_cast<double> (r.smartMin.bass), static_cast<double> (r.smartMin.air));
        s += buf;
    }
    if (a.onboardCap || b.onboardCap)
        s += std::string ("; CAPPED ") + (r.onboardCapActive ? "on" : "off");
    if (std::isfinite (a.safeSpeakerBassCapDb) || std::isfinite (b.safeSpeakerBassCapDb))
        s += "; bass boost as applied " + signedDb (r.bassBoostDb) + " dB";
    if (a.personal.enabled || b.personal.enabled)
        s += "; per-ear headroom reservation " + signedDb (r.personalReservationDb) + " dB";
    if (std::isfinite (a.sensitivityDbSpl) || std::isfinite (b.sensitivityDbSpl))
        s += r.hearingKnown ? "; estimate: loudest 5 s " + numberText (std::round (r.leq5sMaxDbA * 10.0f) / 10.0f) + " dB(A), cap gain min "
                                  + signedDb (r.capGainMinDb) + " dB"
                            : std::string ("; estimate unknown");
    if (a.chatStrip || b.chatStrip)
    {
        char buf[96];
        std::snprintf (buf, sizeof (buf), "; voice held %.0f %% of the time, duck depth max %.0f %%", static_cast<double> (r.voicePercent),
                       static_cast<double> (100.0f * r.duckMax));
        s += buf;
    }
    return s;
}
} // namespace

std::string DemoHost::describe() const
{
    std::vector<std::string> parts;
    if (protection != ProtectionStrength::Off)
        parts.push_back (std::string ("protection strength ") + (protection == ProtectionStrength::Normal ? "Normal" : "Strict"));
    if (smartMacros)
        parts.push_back ("Smart macros on");
    if (onboardCap)
        parts.push_back ("headset enhancement is ON (cap)");
    if (std::isfinite (safeSpeakerBassCapDb))
        parts.push_back ("safe speaker bass cap +" + numberText (safeSpeakerBassCapDb) + " dB");
    if (! correctionText.empty())
    {
        std::string text = correctionText;
        while (! text.empty() && text.back() == '\n')
            text.pop_back();
        for (size_t i = text.find ('\n'); i != std::string::npos; i = text.find ('\n', i))
            text.replace (i, 1, " / ");
        parts.push_back ("headphone correction \"" + text + "\"");
    }
    if (personal.enabled)
    {
        std::string ears;
        for (int ear = 0; ear < 2; ++ear)
        {
            std::string e;
            if (personal.gainDb[static_cast<size_t> (ear)] != 0.0f)
                e += "gain " + numberText (personal.gainDb[static_cast<size_t> (ear)]) + " dB";
            for (int b = 0; b < PersonalProfile::kNumBands; ++b)
                if (const float g = personal.bandDb[static_cast<size_t> (ear)][static_cast<size_t> (b)]; g != 0.0f)
                    e += (e.empty() ? "" : ", ") + numberText (static_cast<float> (PersonalProfile::kBandHz[static_cast<size_t> (b)])) + " Hz "
                         + (g > 0.0f ? "+" : "") + numberText (g) + " dB";
            if (! e.empty())
                ears += (ears.empty() ? "" : "; ") + std::string (ear == 0 ? "left " : "right ") + e;
        }
        if (personal.balanceDb != 0.0f)
            ears += (ears.empty() ? "" : "; ") + std::string ("balance ") + numberText (personal.balanceDb) + " dB";
        parts.push_back ("personal profile (" + (ears.empty() ? std::string ("flat") : ears) + ")");
    }
    if (std::isfinite (sensitivityDbSpl))
        parts.push_back ("sensitivity " + numberText (sensitivityDbSpl) + " dB SPL, system volume " + numberText (endpointVolumeDb) + " dB");
    if (capOn)
        parts.push_back ("listening-level cap " + numberText (capDbA) + " dB(A)");
    if (chatStrip)
        parts.push_back ("a Chat strip with the voice");
    if (chatDuck)
        parts.push_back ("duck game under voice chat " + numberText (chatDuckDepthDb) + " dB");
    if (chatMix != 0.0f)
        parts.push_back ("ChatMix " + numberText (chatMix));
    if (engine)
        parts.push_back ("through the app's mix engine");
    std::string s;
    for (const auto& part : parts)
        s += (s.empty() ? "" : ", ") + part;
    return s;
}

bool makeDemoPack (const DemoOptions& o, DemoResult& result, std::string& error, const std::function<void (const std::string&)>& progress)
{
    result = DemoResult();
    std::string nightNote;
    auto specs = demoPairs (o.presetDir, &nightNote);
    if (! o.only.empty())
    {
        for (const auto& slug : o.only)
            if (std::none_of (specs.begin(), specs.end(), [&slug] (const DemoPairSpec& spec) { return spec.slug == slug; }))
            {
                error = "unknown demo pair '" + slug + "'";
                return false;
            }
        specs.erase (std::remove_if (specs.begin(), specs.end(),
                                     [&o] (const DemoPairSpec& spec) { return std::find (o.only.begin(), o.only.end(), spec.slug) == o.only.end(); }),
                     specs.end());
    }
    if (! nightNote.empty() && std::any_of (specs.begin(), specs.end(), [] (const DemoPairSpec& spec) { return spec.slug == "night"; }))
        result.notes.push_back (nightNote);

    // ---- Programmes ----
    std::vector<Programme> programmes;
    io::AudioFileData user;
    std::string userName;
    if (! o.input.empty())
    {
        if (! io::readWav (o.input, user, error) || ! checkRenderable (user, error))
            return false;
        userName = io::pathToUtf8 (io::pathFromUtf8 (o.input).filename());
    }
    auto programmeIndex = [&] (const std::string& name) -> size_t {
        const bool userFits = ! o.input.empty() && name != "chat-scene" && (name != "game-7.1" || user.numChannels > 2);
        const std::string key = userFits ? "user" : name;
        for (size_t i = 0; i < programmes.size(); ++i)
            if (programmes[i].name == key)
                return i;
        Programme p;
        p.name = key;
        if (userFits)
        {
            p.description = "your file (" + userName + ")";
            p.audio = user;
        }
        else
        {
            p.description = name + " (" + describeBuiltIn (name) + ")";
            p.audio = name == "music"         ? makeMusic (o.seconds)
                      : name == "music-loud"  ? makeLoudMusic (o.seconds)
                      : name == "speech"      ? makeSpeech (o.seconds)
                      : name == "speech-hiss" ? makeSpeechHiss (o.seconds)
                                              : makeGameScene (o.seconds, name == "game-7.1");
            if (name == "chat-scene")
                p.chat = makeChatVoice (o.seconds);
        }
        programmes.push_back (std::move (p));
        return programmes.size() - 1;
    };

    // ---- Distinct renders ----
    std::vector<std::unique_ptr<RenderSlot>> slots;
    auto slotFor = [&] (size_t programme, const std::string& preset, const std::vector<ParamSetting>& settings, const DemoHost& host) -> size_t {
        const std::string key = formatSide (preset, settings, host);
        for (size_t i = 0; i < slots.size(); ++i)
            if (slots[i]->programme == programme && formatSide (slots[i]->preset, slots[i]->settings, slots[i]->host) == key)
                return i;
        auto slot = std::make_unique<RenderSlot>();
        slot->programme = programme;
        slot->preset = preset;
        slot->settings = settings;
        slot->host = host;
        slots.push_back (std::move (slot));
        return slots.size() - 1;
    };
    std::vector<std::pair<size_t, size_t>> pairSlots;
    for (const auto& spec : specs)
    {
        const size_t p = programmeIndex (spec.programme);
        const size_t before = slotFor (p, spec.beforePreset, spec.before, spec.beforeHost);
        const size_t after = slotFor (p, spec.afterPreset, spec.after, spec.afterHost);
        ++slots[before]->uses;
        ++slots[after]->uses;
        pairSlots.emplace_back (before, after);
    }
    const auto uses = [&specs] (const char* programme) {
        return std::any_of (specs.begin(), specs.end(), [programme] (const DemoPairSpec& spec) { return spec.programme == programme; });
    };
    if (! o.input.empty() && user.numChannels <= 2 && uses ("game-7.1"))
        result.notes.push_back ("the virtualiser pairs use the built-in 7.1 game scene: the virtualiser runs only on 5.1 / 7.1 input, and "
                                + userName + " has " + std::to_string (user.numChannels) + " channel(s)");
    if (! o.input.empty() && uses ("chat-scene"))
        result.notes.push_back ("the chat pairs use the built-in game scene and voice: they need a game and a voice on two strips");
    // Every side's parameters are resolved up front, so a bad setting fails before any work.
    for (auto& slot : slots)
    {
        RenderOptions ro;
        ro.presetSpec = slot->preset;
        ro.sets = slot->settings;
        ro.presetDir = o.presetDir;
        ResolvedParameters params;
        if (! buildParameters (ro, params, error))
        {
            error = "demo settings " + formatSide (slot->preset, slot->settings, slot->host) + ": " + error;
            return false;
        }
        slot->values = std::move (params.values);
    }
    result.renders = static_cast<int> (slots.size());

    std::error_code ec;
    const fs::path outDir = io::pathFromUtf8 (o.outDir);
    fs::create_directories (outDir, ec);
    if (! fs::is_directory (outDir, ec))
    {
        error = "cannot create the output folder '" + o.outDir + "'";
        return false;
    }

    RenderSettings rs;
    rs.blockSize = o.blockSize;
    std::mutex progressMutex;
    auto acquire = [&] (size_t index) -> RenderSlot& {
        RenderSlot& slot = *slots[index];
        const std::lock_guard<std::mutex> lock (slot.mutex);
        if (! slot.done)
        {
            try
            {
                if (slot.host.engine)
                    slot.ok = renderThroughEngine (programmes[slot.programme], slot.values, slot.host, o.blockSize, slot.result, slot.readings,
                                                   slot.error);
                else
                {
                    RenderSettings settings = rs;
                    settings.protection = slot.host.protection;
                    slot.ok = renderFile (programmes[slot.programme].audio, slot.values, settings, slot.result, slot.error);
                }
            }
            catch (const std::exception& e)
            {
                slot.ok = false;
                slot.error = e.what();
            }
            slot.done = true;
            if (progress)
            {
                const std::lock_guard<std::mutex> plock (progressMutex);
                progress ("rendered " + programmes[slot.programme].name + " " + formatSide (slot.preset, slot.settings, slot.host));
            }
        }
        return slot;
    };
    auto release = [&] (size_t index) {
        RenderSlot& slot = *slots[index];
        if (--slot.uses == 0)
        {
            const std::lock_guard<std::mutex> lock (slot.mutex);
            std::vector<std::vector<float>>().swap (slot.result.output.channels);
        }
    };

    // ---- Pairs: render (or reuse) both sides, match, write, read back, measure ----
    result.pairs.resize (specs.size());
    std::vector<std::string> errors (specs.size());
    forEachParallel (specs.size(), batchWorkerCount (specs.size(), o.jobs), [&] (size_t i) {
        DemoPairResult& r = result.pairs[i];
        r.spec = specs[i];
        char prefix[16];
        std::snprintf (prefix, sizeof (prefix), "%02d-", static_cast<int> (i + 1));
        r.beforeFile = prefix + r.spec.slug + ".1-before.wav";
        r.afterFile = prefix + r.spec.slug + ".2-after.wav";
        const size_t beforeIndex = pairSlots[i].first, afterIndex = pairSlots[i].second;
        r.programmeUsed = programmes[slots[beforeIndex]->programme].description;
        try
        {
            const RenderSlot& before = acquire (beforeIndex);
            const RenderSlot& after = acquire (afterIndex);
            if (! before.ok || ! after.ok)
                errors[i] = r.spec.slug + ": " + (before.ok ? after.error : before.error);
            else
            {
                r.beforeStats = before.result.stats;
                r.afterStats = after.result.stats;
                if (r.spec.beforeHost.engine || r.spec.afterHost.engine)
                {
                    r.beforeEngine = describeEngine (before.readings, r.spec.beforeHost, r.spec.afterHost);
                    r.afterEngine = describeEngine (after.readings, r.spec.beforeHost, r.spec.afterHost);
                }
                const float lb = before.result.outputReport.integratedLufs, la = after.result.outputReport.integratedLufs;
                r.matched = ! r.spec.levelFeature && lb > kMinusInfDb && la > kMinusInfDb;
                if (r.matched)
                    (la > lb ? r.afterTrimDb : r.beforeTrimDb) = -std::abs (la - lb);

                auto writeSide = [&] (const RenderResult& rr, float trimDb, const std::string& name, LoudnessReport& report,
                                      std::vector<BandLevel>& bands) {
                    io::AudioFileData data;
                    data.sampleRate = rr.output.sampleRate;
                    data.numChannels = rr.output.numChannels;
                    data.channels = rr.output.channels;
                    if (trimDb < 0.0f)
                    {
                        const float g = std::pow (10.0f, trimDb / 20.0f);
                        for (auto& c : data.channels)
                            for (float& v : c)
                                v *= g;
                    }
                    const std::string path = io::pathToUtf8 (outDir / io::pathFromUtf8 (name));
                    std::string err;
                    io::AudioFileData written;
                    if (! io::writeWav (path, data, o.format, err) || ! io::readWav (path, written, err))
                    {
                        errors[i] = err;
                        return;
                    }
                    report = analyse (written.channels, written.sampleRate);
                    bands = octaveBands (written.channels, written.sampleRate);
                };
                writeSide (before.result, r.beforeTrimDb, r.beforeFile, r.beforeReport, r.beforeBands);
                if (errors[i].empty())
                    writeSide (after.result, r.afterTrimDb, r.afterFile, r.afterReport, r.afterBands);
            }
        }
        catch (const std::exception& e)
        {
            errors[i] = r.spec.slug + ": " + e.what();
        }
        release (beforeIndex);
        release (afterIndex);
    });
    for (const auto& e : errors)
        if (! e.empty())
        {
            error = e;
            return false;
        }

    result.indexPath = io::pathToUtf8 (outDir / "index.txt");
    const std::string index = formatDemoIndex (o, result);
    std::ofstream file (outDir / "index.txt", std::ios::binary | std::ios::trunc);
    file.write (index.data(), static_cast<std::streamsize> (index.size()));
    file.close();
    if (! file)
    {
        error = "cannot write '" + result.indexPath + "'";
        return false;
    }
    return true;
}

namespace
{
std::string fixed (double v, int decimals, bool sign = false)
{
    char buf[48];
    std::snprintf (buf, sizeof (buf), sign ? "%+.*f" : "%.*f", decimals, v);
    return buf;
}

std::string bandLabel (float hz)
{
    char buf[24];
    if (hz >= 1000.0f)
        std::snprintf (buf, sizeof (buf), "%gk", static_cast<double> (hz) / 1000.0);
    else
        std::snprintf (buf, sizeof (buf), "%g", static_cast<double> (hz));
    return buf;
}

/** "31.5 +0.3  63 +2.1 ... 16k -0.4": after minus before per octave band
    (n/a where either side has no level). */
std::string formatBandDelta (const std::vector<BandLevel>& before, const std::vector<BandLevel>& after)
{
    std::string s;
    for (size_t b = 0; b < std::min (before.size(), after.size()); ++b)
    {
        const bool measured = before[b].levelDb > kMinusInfDb && after[b].levelDb > kMinusInfDb;
        s += (s.empty() ? "" : "  ") + bandLabel (before[b].centreHz) + " "
             + (measured ? fixed (static_cast<double> (after[b].levelDb) - static_cast<double> (before[b].levelDb), 1, true) : "n/a");
    }
    return s;
}

std::string describeLevel (const DemoPairResult& r)
{
    const double lb = r.beforeReport.integratedLufs, la = r.afterReport.integratedLufs;
    if (r.spec.levelFeature)
        return "not matched (a level feature): after " + fixed (la - lb, 2, true) + " LU re before";
    if (! r.matched)
        return "not matched (no measurable integrated loudness)";
    if (r.afterTrimDb < 0.0f)
        return "matched: after turned down " + fixed (-r.afterTrimDb, 2) + " dB (it rendered that much louder)";
    if (r.beforeTrimDb < 0.0f)
        return "matched: before turned down " + fixed (-r.beforeTrimDb, 2) + " dB (it rendered that much louder)";
    return "matched: both sides rendered equally loud";
}

std::string describeReadouts (const DemoPairResult& r)
{
    const auto& b = r.beforeStats;
    const auto& a = r.afterStats;
    auto arrow = [] (float x, float y, int decimals) { return fixed (x, decimals) + " -> " + fixed (y, decimals); };
    std::string s = "limiter GR max " + arrow (b.limiterGrMaxDb, a.limiterGrMaxDb, 1) + " dB (mean " + arrow (b.limiterGrMeanDb, a.limiterGrMeanDb, 2)
                    + ")";
    if (b.compGrMaxDb < -0.05f || a.compGrMaxDb < -0.05f || b.compUpwardMaxDb > 0.05f || a.compUpwardMaxDb > 0.05f)
        s += "; compressor GR max " + arrow (b.compGrMaxDb, a.compGrMaxDb, 1) + " dB, upward max " + arrow (b.compUpwardMaxDb, a.compUpwardMaxDb, 1)
             + " dB";
    if (b.clipActivePercent > 0.05f || a.clipActivePercent > 0.05f)
        s += "; clipper active " + arrow (b.clipActivePercent, a.clipActivePercent, 1) + " %";
    if (b.smoothnessCutMaxDb < -0.05f || a.smoothnessCutMaxDb < -0.05f)
        s += "; Smoothness cut max " + arrow (b.smoothnessCutMaxDb, a.smoothnessCutMaxDb, 1) + " dB";
    if (b.governorScaleMin < 0.99f || a.governorScaleMin < 0.99f)
        s += "; governor Boost scale min " + arrow (b.governorScaleMin, a.governorScaleMin, 2);
    if (b.autoLevelMinDb < -0.05f || a.autoLevelMinDb < -0.05f || b.autoLevelMaxDb > 0.05f || a.autoLevelMaxDb > 0.05f)
        s += "; Auto Level " + fixed (b.autoLevelMinDb, 1) + ".." + fixed (b.autoLevelMaxDb, 1) + " -> " + fixed (a.autoLevelMinDb, 1) + ".."
             + fixed (a.autoLevelMaxDb, 1) + " dB";
    return s;
}

std::string wrap (const std::string& text, size_t indent, size_t width = 100)
{
    std::string out, line;
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t end = text.find (' ', pos);
        if (end == std::string::npos)
            end = text.size();
        const std::string word = text.substr (pos, end - pos);
        if (! line.empty() && indent + line.size() + 1 + word.size() > width)
        {
            out += line + "\n" + std::string (indent, ' ');
            line.clear();
        }
        line += (line.empty() ? "" : " ") + word;
        pos = end + 1;
    }
    return out + line;
}
} // namespace

std::string formatDemoIndex (const DemoOptions& o, const DemoResult& result)
{
    std::string s;
    s += "Flubsound demo pack - before / after pairs to judge by ear\n";
    s += "==========================================================\n\n";
    s += wrap ("Made by `flubsound-cli demo`. Each pair is the same programme through the Flubsound processing chain twice: "
               "'before' without the feature, 'after' with it; everything else is the same. Play both files of a pair in turn "
               "(headphones for the crossfeed and virtualiser pairs) and listen for what the pair's 'Listen for' line names.", 0)
         + "\n\n";
    s += wrap ("Loudness: in a pair, only the louder file is turned down to the quieter's integrated loudness (EBU R128), so the "
               "comparison is not won by the louder side and nothing is raised into clipping. Level features (the Loudness "
               "macro, Startle Guard, Night, the levelling presets, the listening-level cap, the chat duck and ChatMix) are not "
               "matched, because their job is the level; their 'Level' line gives the difference.", 0)
         + "\n\n";
    s += wrap ("App settings: a side marked '+ app:' also has settings the app keeps outside the presets (Smart macros, the "
               "headset enhancement cap, the safe speaker bass cap, a headphone correction, the personal per-ear profile, the "
               "hearing guard, the chat duck, ChatMix). Both sides of such a pair are rendered through the app's mix engine "
               "(the strip, a Chat strip where the pair has one, the master limiter at -1 dBTP and the hearing guard), and its "
               "'Readouts' are the engine's own.", 0)
         + "\n\n";
    s += wrap ("Numbers: every file is read back and measured as `flubsound-cli analyze --bands <file>` measures it. 'Band "
               "delta' is after minus before, dB, per octave band (31.5 Hz .. 16 kHz, the mean of the channels). 'Readouts' are "
               "the chain's own meters over each render (before -> after), as `flubsound-cli process --json` gives them in "
               "render.stats. 'Before' / 'After' are the settings, as `flubsound-cli process` options: "
               "`flubsound-cli process -i <programme> -o out.wav <options>` renders the same file before the loudness match.", 0)
         + "\n\n";
    s += "Programmes (" + fixed (o.seconds, 1) + " s each when built in; 48 kHz):\n";
    std::vector<std::string> seen;
    for (const auto& p : result.pairs)
        if (std::find (seen.begin(), seen.end(), p.programmeUsed) == seen.end())
        {
            seen.push_back (p.programmeUsed);
            s += "  - " + wrap (p.programmeUsed, 4) + "\n";
        }
    for (const auto& n : result.notes)
        s += "Note: " + wrap (n, 6) + "\n";
    s += "\n" + std::to_string (result.pairs.size()) + " pairs, " + std::to_string (result.renders) + " renders.\n";

    for (size_t i = 0; i < result.pairs.size(); ++i)
    {
        const auto& r = result.pairs[i];
        char head[16];
        std::snprintf (head, sizeof (head), "[%02d] ", static_cast<int> (i + 1));
        s += "\n" + std::string (head) + r.spec.title + "\n";
        s += "  Files      : " + r.beforeFile + " | " + r.afterFile + "\n";
        const std::string programme = r.programmeUsed.substr (0, r.programmeUsed.find (" ("));
        s += "  Programme  : " + (programme == "your file" ? r.programmeUsed : programme) + "\n";
        s += "  Before     : " + wrap (formatSide (r.spec.beforePreset, r.spec.before, r.spec.beforeHost), 15) + "\n";
        s += "  After      : " + wrap (formatSide (r.spec.afterPreset, r.spec.after, r.spec.afterHost), 15) + "\n";
        s += "  Level      : " + describeLevel (r) + "\n";
        s += "  Loudness   : before " + formatDb (r.beforeReport.integratedLufs, 2) + " LUFS, LRA " + fixed (r.beforeReport.loudnessRangeLu, 1)
             + " LU, true peak " + formatDb (r.beforeReport.truePeakDbtp, 1) + " dBTP | after " + formatDb (r.afterReport.integratedLufs, 2)
             + " LUFS, LRA " + fixed (r.afterReport.loudnessRangeLu, 1) + " LU, true peak " + formatDb (r.afterReport.truePeakDbtp, 1)
             + " dBTP\n";
        s += "  Band delta : " + formatBandDelta (r.beforeBands, r.afterBands) + "\n";
        if (r.beforeEngine.empty())
            s += "  Readouts   : " + wrap (describeReadouts (r), 15) + "\n";
        else
            s += "  Readouts   : " + wrap ("before: " + r.beforeEngine + " | after: " + r.afterEngine, 15) + "\n";
        s += "  Listen for : " + wrap (r.spec.listenFor, 15) + "\n";
    }
    return s;
}

int runDemo (const CliOptions& options)
{
    DemoOptions d;
    d.input = options.input;
    if (! options.output.empty())
        d.outDir = options.output;
    d.seconds = options.demoSeconds;
    d.jobs = options.jobs;
    d.presetDir = options.render.presetDir;
    d.blockSize = options.render.blockSize;
    if (options.demoFormatSet)
        d.format = options.render.format;

    const bool quiet = options.quiet;
    std::mutex printMutex;
    auto progress = [&] (const std::string& line) {
        if (quiet)
            return;
        const std::lock_guard<std::mutex> lock (printMutex);
        std::printf ("  %s\n", line.c_str());
        std::fflush (stdout);
    };
    if (! quiet)
        std::printf ("Rendering the demo pack into %s ...\n", d.outDir.c_str());

    DemoResult result;
    std::string error;
    if (! makeDemoPack (d, result, error, progress))
    {
        std::fprintf (stderr, "error: %s\n", error.c_str());
        return kExitFailure;
    }
    for (const auto& n : result.notes)
        std::fprintf (stderr, "note: %s\n", n.c_str());
    if (! quiet)
        std::printf ("Wrote %zu pairs (%d renders) and %s\n", result.pairs.size(), result.renders, result.indexPath.c_str());
    return kExitOk;
}
} // namespace flub::cli
