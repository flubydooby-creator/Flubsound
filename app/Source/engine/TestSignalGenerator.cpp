#include "TestSignalGenerator.h"

#include "flub/common/Math.h"
#include "flub/dsp/Bs775Fold.h"
#include "flub/engine/Parameters.h"

#include <cmath>

namespace flub::app
{
namespace
{
constexpr double kTwoPiD = 6.283185307179586;

struct OnePoleLowPass
{
    float a = 0.0f, y = 0.0f;
    void setCutoff (double hz, double fs) { a = static_cast<float> (1.0 - std::exp (-kTwoPiD * hz / fs)); }
    float process (float x) noexcept { return y += a * (x - y); }
};

struct OnePoleHighPass
{
    OnePoleLowPass lp;
    void setCutoff (double hz, double fs) { lp.setCutoff (hz, fs); }
    float process (float x) noexcept { return x - lp.process (x); }
};

/** RBJ band-pass (0 dB peak), transposed direct form II. */
struct BandPass
{
    float b0 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    void set (double hz, double q, double fs)
    {
        const double w0 = kTwoPiD * hz / fs;
        const double alpha = std::sin (w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        b0 = static_cast<float> (alpha / a0);
        b2 = static_cast<float> (-alpha / a0);
        a1 = static_cast<float> (-2.0 * std::cos (w0) / a0);
        a2 = static_cast<float> ((1.0 - alpha) / a0);
    }

    float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = -a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

/** Band-limited sawtooth (PolyBLEP). */
struct SawOscillator
{
    double phase = 0.0;

    static double blep (double t, double dt) noexcept
    {
        if (t < dt)
        {
            t /= dt;
            return t + t - t * t - 1.0;
        }
        if (t > 1.0 - dt)
        {
            t = (t - 1.0) / dt;
            return t * t + t + t + 1.0;
        }
        return 0.0;
    }

    float next (double frequency, double fs) noexcept
    {
        const double dt = frequency / fs;
        const double v = 2.0 * phase - 1.0 - blep (phase, dt);
        phase += dt;
        if (phase >= 1.0)
            phase -= 1.0;
        return static_cast<float> (v);
    }
};

inline float decay (double tau, double timeConstant) noexcept
{
    return static_cast<float> (std::exp (-tau / timeConstant));
}

// Channel order FL FR FC LFE BL BR SL SR; speakers around the listener,
// sorted by azimuth (degrees, negative = left).
constexpr int kRingChannels[] = { 4, 6, 0, 2, 1, 7, 5 };
constexpr double kRingAngles[] = { -150.0, -90.0, -30.0, 0.0, 30.0, 90.0, 150.0 };

/** Constant-power pair-wise panning to the 7 main speakers. */
void panGains (double azimuthDeg, std::array<float, 8>& gains)
{
    gains.fill (0.0f);
    double az = std::fmod (azimuthDeg + 150.0, 360.0);
    if (az < 0.0)
        az += 360.0;
    az -= 150.0; // [-150, 210)

    for (int i = 0; i < 7; ++i)
    {
        const double a0 = kRingAngles[i];
        const double a1 = i + 1 < 7 ? kRingAngles[i + 1] : kRingAngles[0] + 360.0;
        if (az >= a0 && az < a1)
        {
            const double frac = (az - a0) / (a1 - a0);
            gains[static_cast<size_t> (kRingChannels[i])] = static_cast<float> (std::cos (frac * 0.5 * flub::kPi));
            gains[static_cast<size_t> (kRingChannels[(i + 1) % 7])] = static_cast<float> (std::sin (frac * 0.5 * flub::kPi));
            return;
        }
    }
    gains[2] = 1.0f;
}
} // namespace

// =============================================================================
// Music: 120 BPM, Am - F - C - G
// =============================================================================
class TestSignalGenerator::MusicSynth
{
public:
    explicit MusicSynth (double rate)
        : fs (rate), rng (0x5eed1234u)
    {
        beatLength = static_cast<int64_t> (std::llround (fs * 0.5));
        eighthLength = beatLength / 2;
        snareHp.setCutoff (1200.0, fs);
        snareLp.setCutoff (7000.0, fs);
        hatHp1.setCutoff (6500.0, fs);
        hatHp2.setCutoff (6500.0, fs);
        bassLp.setCutoff (700.0, fs);
        padLpL.setCutoff (1800.0, fs);
        padLpR.setCutoff (1800.0, fs);
    }

    void render (float& left, float& right) noexcept
    {
        static constexpr double roots[] = { 55.0, 43.65, 65.41, 49.0 };
        static constexpr double chords[4][3] = {
            { 220.0, 261.63, 329.63 }, // Am
            { 174.61, 220.0, 261.63 }, // F
            { 261.63, 329.63, 392.0 }, // C
            { 196.0, 246.94, 293.66 }, // G
        };
        static constexpr double bassPattern[] = { 1.0, 1.0, 2.0, 1.0, 1.0, 2.0, 1.0, 1.5 };

        const int64_t beat = n / beatLength;
        const int beatInBar = static_cast<int> (beat % 4);
        const int chord = static_cast<int> ((beat / 4) % 4);
        const double tauBeat = static_cast<double> (n % beatLength) / fs;
        const double tauEighth = static_cast<double> (n % eighthLength) / fs;
        const int eighthInBar = static_cast<int> ((n / eighthLength) % 8);
        const double tauBar = static_cast<double> (n % (4 * beatLength)) / fs;
        const double barSeconds = 4.0 * static_cast<double> (beatLength) / fs;
        const float noise = rng.nextBipolar();

        // Kick: pitch-swept sine + click.
        const double kickPhase = kTwoPiD * (48.0 * tauBeat + 110.0 * 0.035 * (1.0 - std::exp (-tauBeat / 0.035)));
        const float kick = 0.85f * static_cast<float> (std::sin (kickPhase)) * decay (tauBeat, 0.28) + 0.25f * noise * decay (tauBeat, 0.002);

        // Snare on 2 and 4.
        float snare = 0.0f;
        const float snareNoise = snareLp.process (snareHp.process (noise));
        if (beatInBar == 1 || beatInBar == 3)
            snare = 0.55f * snareNoise * decay (tauBeat, 0.14)
                    + 0.3f * static_cast<float> (std::sin (kTwoPiD * 185.0 * tauBeat)) * decay (tauBeat, 0.07);

        // Hats on 8ths, off-beats accented.
        const float hatNoise = hatHp2.process (hatHp1.process (noise));
        const float hat = (eighthInBar % 2 == 1 ? 0.3f : 0.18f) * hatNoise * decay (tauEighth, 0.03);

        // Bass: additive saw-ish tone, gated 8ths.
        const double bassHz = roots[chord] * bassPattern[eighthInBar];
        bassPhase += bassHz / fs;
        if (bassPhase >= 1.0)
            bassPhase -= 1.0;
        float bassRaw = 0.0f;
        for (int h = 1; h <= 5; ++h)
            bassRaw += static_cast<float> (std::sin (kTwoPiD * h * bassPhase)) / static_cast<float> (h);
        const float bassEnv = static_cast<float> (std::min (1.0, tauEighth / 0.004)) * (0.35f + 0.65f * decay (tauEighth, 0.18))
                              * (tauEighth < 0.21 ? 1.0f : decay (tauEighth - 0.21, 0.01));
        const float bass = 0.32f * bassLp.process (bassRaw) * bassEnv;

        // Pad: detuned saws, slow filter sweep, gentle bar crossfade.
        const double lfo = 0.5 + 0.5 * std::sin (kTwoPiD * 0.11 * static_cast<double> (n) / fs);
        if ((n & 63) == 0)
        {
            padLpL.setCutoff (1100.0 + 1500.0 * lfo, fs);
            padLpR.setCutoff (1150.0 + 1500.0 * lfo, fs);
        }
        float padL = 0.0f, padR = 0.0f;
        for (int v = 0; v < 3; ++v)
        {
            const double f = chords[chord][v];
            padL += padOsc[v][0].next (f * 0.9977, fs) + padOsc[v][1].next (f * 1.0012, fs);
            padR += padOsc[v][2].next (f * 1.0023, fs) + padOsc[v][3].next (f * 0.9988, fs);
        }
        const float padEnv = static_cast<float> (std::min (1.0, tauBar / 0.08) * std::min (1.0, (barSeconds - tauBar) / 0.08));
        padL = 0.05f * padLpL.process (padL) * padEnv;
        padR = 0.05f * padLpR.process (padR) * padEnv;

        const float centre = kick + snare + bass;
        left = 0.55f * (centre + 0.8f * hat + padL);
        right = 0.55f * (centre + 1.2f * hat + padR);
        ++n;
    }

private:
    double fs;
    int64_t n = 0, beatLength = 24000, eighthLength = 12000;
    flub::FastRandom rng;
    OnePoleHighPass snareHp, hatHp1, hatHp2;
    OnePoleLowPass snareLp, bassLp, padLpL, padLpR;
    double bassPhase = 0.0;
    SawOscillator padOsc[3][4];
};

// =============================================================================
// Game: 7.1 scene
// =============================================================================
class TestSignalGenerator::GameSynth
{
public:
    explicit GameSynth (double rate)
        : fs (rate), rng (0x9a3e5eedu)
    {
        for (size_t c = 0; c < ambience.size(); ++c)
        {
            ambience[c].setCutoff (450.0 + 60.0 * static_cast<double> (c), fs);
            ambienceRng[c] = flub::FastRandom (0x1000u + static_cast<uint32_t> (c) * 7919u);
        }
        stepHp.setCutoff (350.0, fs);
        stepLp.setCutoff (3500.0, fs);
        boomLp.setCutoff (260.0, fs);
        crackHp.setCutoff (2500.0, fs);
        formant[0].set (650.0, 4.0, fs);
        formant[1].set (1100.0, 5.0, fs);
        formant[2].set (2500.0, 6.0, fs);
        panGains (-60.0, gunGains);
        panGains (125.0, boomGains);
        // The stereo downmix's LFE path: the chain's LfeFold at virt.lfe's
        // default (docs/11 E01).
        const auto& lfeLevel = flub::param::layout()[static_cast<size_t> (flub::param::VirtLfeGainDb)];
        lfe.prepare (fs, flub::LfeFold::gainFor (true, lfeLevel.defaultValue));
    }

    /** ITU-R BS.775 downmix of a rendered frame for stereo strips, -3 dB
        overall, the LFE folded by the chain's law (Bs775Fold.h). */
    void downmix (float* frame) noexcept
    {
        constexpr float k = flub::Bs775Fold::kMatrixGain;
        const float low = lfe.next (frame[3]);
        const float l = k * (frame[0] + low + k * frame[2] + k * frame[4] + k * frame[6]);
        const float r = k * (frame[1] + low + k * frame[2] + k * frame[5] + k * frame[7]);
        for (int c = 0; c < 8; ++c)
            frame[c] = 0.0f;
        frame[0] = l;
        frame[1] = r;
    }

    void render (float* frame) noexcept
    {
        for (int c = 0; c < 8; ++c)
            frame[c] = 0.0f;

        const double t = static_cast<double> (n) / fs;
        const float noise = rng.nextBipolar();

        // Ambience bed: decorrelated, slowly breathing noise on the 7 mains.
        for (size_t c = 0; c < 8; ++c)
        {
            if (c == 3)
                continue;
            const float breath = 0.7f + 0.3f * static_cast<float> (std::sin (kTwoPiD * 0.07 * t + static_cast<double> (c)));
            frame[c] += 0.035f * breath * ambience[c].process (ambienceRng[c].nextBipolar());
        }

        // Footsteps circling the listener (23 degrees per step).
        const auto stepLength = static_cast<int64_t> (std::llround (0.42 * fs));
        const int64_t step = n / stepLength;
        const double tauStep = static_cast<double> (n % stepLength) / fs;
        if (n % stepLength == 0)
            panGains (-180.0 + static_cast<double> ((step * 23) % 360), stepGains);
        const float stepNoise = stepLp.process (stepHp.process (noise));
        const float footstep = 0.55f * stepNoise * decay (tauStep, 0.02)
                               + 0.35f * static_cast<float> (std::sin (kTwoPiD * 95.0 * tauStep)) * decay (tauStep, 0.035);
        for (size_t c = 0; c < 8; ++c)
            frame[c] += stepGains[c] * footstep;

        // Gunfire: bursts of 3 shots every 1.7 s, front-left side.
        const double burstT = std::fmod (t + 0.4, 1.7);
        float gun = 0.0f;
        for (int shot = 0; shot < 3; ++shot)
        {
            const double tau = burstT - 0.11 * shot;
            if (tau >= 0.0 && tau < 0.3)
                gun += 0.6f * noise * decay (tau, 0.035) + 0.4f * static_cast<float> (std::sin (kTwoPiD * 70.0 * tau)) * decay (tau, 0.05);
        }
        const float gunCrack = crackHp.process (gun);
        for (size_t c = 0; c < 8; ++c)
            frame[c] += gunGains[c] * (0.7f * gun + 0.3f * gunCrack);

        // Explosion every 3.2 s, right rear, with LFE.
        const double boomT = std::fmod (t + 2.1, 3.2);
        const float boomNoise = boomLp.process (noise);
        if (boomT < 2.5)
        {
            const float body = 1.4f * boomNoise * decay (boomT, 0.6) + 0.35f * noise * decay (boomT, 0.008);
            for (size_t c = 0; c < 8; ++c)
                frame[c] += boomGains[c] * body;
            frame[3] += 0.8f * static_cast<float> (std::sin (kTwoPiD * 36.0 * boomT)) * decay (boomT, 0.8);
        }

        // Dialogue-like formant voice on the centre channel.
        const double voiceT = std::fmod (t, 2.6);
        if (voiceT < 1.6)
        {
            const double f0 = 118.0 + 9.0 * std::sin (kTwoPiD * 0.9 * t) + 4.0 * std::sin (kTwoPiD * 5.2 * t);
            const float glottal = voiceOsc.next (f0, fs);
            const float voiced = formant[0].process (glottal) + 0.7f * formant[1].process (glottal) + 0.35f * formant[2].process (glottal);
            const float syllables = static_cast<float> (0.5 - 0.5 * std::cos (kTwoPiD * 4.3 * voiceT));
            const float edge = static_cast<float> (std::min (1.0, voiceT / 0.05) * std::min (1.0, (1.6 - voiceT) / 0.05));
            frame[2] += 0.6f * voiced * syllables * edge;
        }
        else
        {
            voiceOsc.next (118.0, fs);
            for (auto& f : formant)
                f.process (0.0f);
        }

        ++n;
    }

private:
    double fs;
    int64_t n = 0;
    flub::FastRandom rng;
    std::array<OnePoleLowPass, 8> ambience;
    std::array<flub::FastRandom, 8> ambienceRng;
    OnePoleHighPass stepHp, crackHp;
    OnePoleLowPass stepLp, boomLp;
    BandPass formant[3];
    SawOscillator voiceOsc;
    std::array<float, 8> stepGains {}, gunGains {}, boomGains {};
    flub::LfeFold lfe;
};

// =============================================================================
TestSignalGenerator::TestSignalGenerator (double sr)
    : sampleRate (sr > 0.0 ? sr : 48000.0)
{
}

TestSignalGenerator::~TestSignalGenerator() = default;

void TestSignalGenerator::setProgramme (int strip, Programme programme, float gainDb)
{
    if (strip < 0 || strip >= AudioEngineHost::kMaxStrips)
        return;

    auto& s = strips[static_cast<size_t> (strip)];
    s.programme = programme;
    s.gain = flub::dbToGain (gainDb);
    s.music.reset();
    s.game.reset();
    if (programme == Programme::Music)
        s.music = std::make_unique<MusicSynth> (sampleRate);
    else if (programme == Programme::Game71)
        s.game = std::make_unique<GameSynth> (sampleRate);
}

bool TestSignalGenerator::renderStrip (int strip, const flub::AudioBlock& block)
{
    if (strip < 0 || strip >= AudioEngineHost::kMaxStrips)
        return false;

    auto& s = strips[static_cast<size_t> (strip)];
    if (s.programme == Programme::Silence)
        return false;

    const int channels = block.numChannels;
    std::array<float, 8> frame {};

    for (int i = 0; i < block.numSamples; ++i)
    {
        frame.fill (0.0f);
        if (s.music != nullptr)
        {
            s.music->render (frame[0], frame[1]);
        }
        else if (s.game != nullptr)
        {
            s.game->render (frame.data());
            if (channels < 8)
                s.game->downmix (frame.data()); // stereo strips
        }

        for (int c = 0; c < channels; ++c)
            block.channel (c)[i] = (c < 8 ? frame[static_cast<size_t> (c)] : 0.0f) * s.gain;
    }
    return true;
}
} // namespace flub::app
