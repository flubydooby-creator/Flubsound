#include "Soak.h"

#include "CliOptions.h"

#include "flub/common/AudioBlock.h"
#include "flub/common/Denormals.h"
#include "flub/common/Math.h"
#include "flub/engine/MacroMap.h"
#include "flub/engine/Parameters.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/io/PresetIO.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>

namespace flub::cli
{
using namespace param;

// ===========================================================================
// Programme
// ===========================================================================
namespace
{
/** A raised-cosine ramp of `frames` from 0 to 1 (0 before, 1 after). */
double rise (double t, double frames) noexcept
{
    return t <= 0.0 ? 0.0 : t >= frames ? 1.0 : 0.5 - 0.5 * std::cos (kPi * t / frames);
}

struct Voice
{
    enum Kind
    {
        Tone,     // sine, optionally swept (freq -> freqEnd, exponential) and with vibrato
        Harmonic, // harmonic series on freq (speech-like), formant-weighted
        Noise     // white noise through two one-pole low-passes at freq
    };
    Kind kind = Tone;
    double freq = 440.0, freqEnd = 440.0, amp = 0.1, panL = 0.7071, panR = 0.7071;
    double attack = 96.0, hold = 0.0, release = 96.0, decayFrames = 0.0; // frames; decay: e^-t/decay (0 = none)
    double vibratoHz = 0.0, vibratoDepth = 0.0;
    int harmonics = 0;                // Harmonic: fixed count, so none switches on mid-glide
    std::array<double, 40> weights {}; // Harmonic: amplitude of harmonic k + 1
    int64_t start = 0;
    double phase = 0.0, vibratoPhase = 0.0, lp1 = 0.0, lp2 = 0.0;

    double length() const noexcept { return attack + hold + release; }
};

enum class Scene
{
    Music,
    Game,
    Speech,
    Loud,
    Fade
};
constexpr Scene kSceneCycle[] = { Scene::Music, Scene::Game, Scene::Speech, Scene::Loud, Scene::Fade };
} // namespace

struct SoakProgramme::Impl
{
    double fs;
    FastRandom rng;
    std::vector<Voice> voices;
    int64_t frame = 0, nextEvent = 0;
    int64_t sceneFrames;
    int beat = 0;
    // Programme gain: raised-cosine ramps between targets.
    double gainFrom = 1.0, gainTo = 1.0;
    int64_t rampStart = 0, rampFrames = 1;

    Impl (double sampleRate, uint32_t seed)
        : fs (sampleRate), rng (seed ^ 0x5eed50a4u), sceneFrames (static_cast<int64_t> (std::lround (kSceneSeconds * sampleRate)))
    {
        voices.reserve (128);
    }

    double uniform (double lo, double hi) noexcept { return lo + (hi - lo) * 0.5 * (1.0 + rng.nextBipolar()); }
    int64_t ms (double v) const noexcept { return static_cast<int64_t> (std::lround (v * 0.001 * fs)); }
    double msF (double v) const noexcept { return v * 0.001 * fs; }

    void pan (Voice& v, double position /* 0 = left .. 1 = right */) noexcept
    {
        v.panL = std::cos (0.5 * kPi * position);
        v.panR = std::sin (0.5 * kPi * position);
    }

    Voice& add (Voice::Kind kind, double freq, double amp, double attackMs, double holdMs, double releaseMs)
    {
        Voice v;
        v.kind = kind;
        v.freq = v.freqEnd = freq;
        v.amp = amp;
        v.attack = std::max (2.0, attackMs) * 0.001 * fs; // >= 2 ms: the programme has no clicks of its own
        v.hold = holdMs * 0.001 * fs;
        v.release = std::max (2.0, releaseMs) * 0.001 * fs;
        v.start = frame;
        v.phase = uniform (0.0, kTwoPi);
        voices.push_back (v);
        return voices.back();
    }

    void rampGain (double target, double rampMs) noexcept
    {
        gainFrom = currentGain();
        gainTo = target;
        rampStart = frame;
        rampFrames = std::max<int64_t> (1, ms (rampMs));
    }

    double currentGain() const noexcept
    {
        return gainFrom + (gainTo - gainFrom) * rise (static_cast<double> (frame - rampStart), static_cast<double> (rampFrames));
    }

    Scene sceneAt (int64_t f) const noexcept { return kSceneCycle[(f / sceneFrames) % 5]; }

    // ---- one musical beat (0.5 s) ------------------------------------------
    void musicBeat()
    {
        static constexpr double kBass[] = { 41.2, 49.0, 55.0, 61.7, 73.4 };
        static constexpr double kRoot[] = { 220.0, 246.9, 261.6, 293.7 };
        static constexpr double kLead[] = { 784.0, 880.0, 997.0, 1175.0, 1319.0 };
        auto& kick = add (Voice::Tone, 90.0, 0.22, 3.0, 0.0, 300.0);
        kick.freqEnd = 45.0;
        kick.decayFrames = msF (110.0);
        auto& bass = add (Voice::Tone, kBass[rng.nextU32() % 5], 0.11, 10.0, 380.0, 60.0);
        pan (bass, 0.5);
        if (beat % 4 == 0)
        {
            const double root = kRoot[rng.nextU32() % 4];
            for (double ratio : { 1.0, 1.26, 1.5 })
                pan (add (Voice::Tone, root * ratio, 0.035, 60.0, 1750.0, 150.0), uniform (0.2, 0.8));
        }
        for (int half = 0; half < 2; ++half)
            if (uniform (0.0, 1.0) < 0.7)
            {
                auto& lead = add (Voice::Tone, kLead[rng.nextU32() % 5], 0.03, 5.0, 150.0, 60.0);
                lead.start += ms (250.0) * half;
                lead.vibratoHz = 5.0;
                lead.vibratoDepth = 0.005;
                pan (lead, uniform (0.3, 0.7));
            }
        auto& hat = add (Voice::Noise, 6000.0, 0.012, 2.0, 0.0, 40.0);
        hat.start += ms (250.0);
        pan (hat, uniform (0.3, 0.7));
    }

    void gameBeat (int64_t inScene)
    {
        if (inScene == 0)
        {
            auto& bed = add (Voice::Noise, 800.0, 0.03, 200.0, kSceneSeconds * 1000.0 - 400.0, 200.0);
            pan (bed, 0.5);
            // An explosion in the middle of the scene.
            auto& boom = add (Voice::Tone, 60.0, 0.35, 10.0, 0.0, 1500.0);
            boom.start += ms (3000.0);
            boom.freqEnd = 28.0;
            boom.decayFrames = msF (400.0);
            auto& rumble = add (Voice::Noise, 400.0, 0.2, 10.0, 0.0, 1200.0);
            rumble.start += ms (3000.0);
            rumble.decayFrames = msF (300.0);
        }
        // Steps: 3.2 kHz +-10 %, 25 ms, panned, one per 350 ms.
        auto& step = add (Voice::Tone, 3200.0 * uniform (0.9, 1.1), 0.05, 3.0, 20.0, 25.0);
        pan (step, uniform (0.0, 1.0));
        // Now and then a burst of five shots, 100 ms apart.
        if (uniform (0.0, 1.0) < 0.15)
            for (int k = 0; k < 5; ++k)
            {
                auto& crack = add (Voice::Noise, 3000.0, 0.18, 2.0, 0.0, 80.0);
                crack.start += ms (100.0) * k;
                crack.decayFrames = msF (25.0);
                auto& thump = add (Voice::Tone, 200.0, 0.18, 2.0, 0.0, 120.0);
                thump.start += ms (100.0) * k;
                thump.freqEnd = 60.0;
                thump.decayFrames = msF (40.0);
                pan (crack, 0.35);
                pan (thump, 0.35);
            }
    }

    void syllable()
    {
        auto& v = add (Voice::Harmonic, uniform (110.0, 180.0), 0.12, 20.0, uniform (80.0, 150.0), 40.0);
        v.freqEnd = v.freq * uniform (0.85, 1.15);
        // Harmonics up to 4 kHz, 1/k weighted plus two formant peaks (a vowel).
        const double formant1 = uniform (300.0, 800.0), formant2 = uniform (900.0, 2200.0);
        v.harmonics = std::min (static_cast<int> (v.weights.size()), static_cast<int> (4000.0 / std::max (v.freq, v.freqEnd)));
        for (int k = 1; k <= v.harmonics; ++k)
        {
            const double hz = k * v.freq;
            v.weights[static_cast<size_t> (k - 1)] = 0.25 * (1.0 / k + 1.5 * std::exp (-std::pow ((hz - formant1) / 150.0, 2.0))
                                                             + std::exp (-std::pow ((hz - formant2) / 250.0, 2.0)));
        }
        pan (v, 0.5);
    }

    /** Spawns the voices due at `frame` and returns the frame of the next event. */
    int64_t schedule()
    {
        const Scene scene = sceneAt (frame);
        const int64_t inScene = frame % sceneFrames;
        const int64_t toSceneEnd = sceneFrames - inScene;
        if (inScene == 0)
        {
            beat = 0;
            rampGain (scene == Scene::Loud ? 1.6 : 1.0, 200.0);
        }
        switch (scene)
        {
            case Scene::Music:
            case Scene::Loud:
                musicBeat();
                ++beat;
                return frame + std::min (ms (500.0), toSceneEnd);
            case Scene::Game:
                gameBeat (inScene);
                return frame + std::min (ms (350.0), toSceneEnd);
            case Scene::Speech:
                if (uniform (0.0, 1.0) < 0.85)
                    syllable();
                return frame + std::min (ms (uniform (180.0, 300.0)), toSceneEnd);
            case Scene::Fade:
            {
                // Music at -10 dB until 2.9 s, a 100 ms fade into digital
                // silence until 4 s, then speech fading in over 100 ms.
                const int64_t sceneStart = frame - inScene;
                if (inScene == 0)
                    rampGain (0.3, 200.0);
                if (inScene < ms (2900.0))
                {
                    musicBeat();
                    return std::min (frame + ms (500.0), sceneStart + ms (2900.0));
                }
                if (inScene < ms (4000.0))
                {
                    rampGain (0.0, 100.0);
                    return sceneStart + ms (4000.0);
                }
                if (inScene == ms (4000.0))
                    rampGain (1.0, 100.0);
                syllable();
                return frame + std::min (ms (uniform (180.0, 300.0)), toSceneEnd);
            }
        }
        return frame + ms (500.0);
    }

    double voiceSample (Voice& v, double t) noexcept
    {
        double env = rise (t, v.attack) * (1.0 - rise (t - v.attack - v.hold, v.release));
        if (v.decayFrames > 0.0)
            env *= std::exp (-t / v.decayFrames);
        const double total = v.length();
        double f = v.freq;
        if (v.freqEnd != v.freq)
            f *= std::pow (v.freqEnd / v.freq, std::min (1.0, t / total));
        if (v.vibratoHz > 0.0)
        {
            f *= 1.0 + v.vibratoDepth * std::sin (v.vibratoPhase);
            v.vibratoPhase += kTwoPi * v.vibratoHz / fs;
        }
        double x = 0.0;
        switch (v.kind)
        {
            case Voice::Tone: x = std::sin (v.phase); break;
            case Voice::Harmonic:
            {
                // sum_k w_k sin (k phase) (Chebyshev recurrence).
                const double c = 2.0 * std::cos (v.phase);
                double s1 = std::sin (v.phase), s0 = 0.0;
                for (int k = 0; k < v.harmonics; ++k)
                {
                    x += v.weights[static_cast<size_t> (k)] * s1;
                    const double s2 = c * s1 - s0;
                    s0 = s1;
                    s1 = s2;
                }
                break;
            }
            case Voice::Noise:
            {
                const double a = 1.0 - std::exp (-kTwoPi * v.freq / fs);
                v.lp1 += a * (rng.nextBipolar() - v.lp1);
                v.lp2 += a * (v.lp1 - v.lp2);
                x = 2.0 * v.lp2;
                break;
            }
        }
        if (v.kind != Voice::Noise)
            v.phase = std::fmod (v.phase + kTwoPi * f / fs, kTwoPi);
        return env * v.amp * x;
    }

    void render (float* left, float* right, int numFrames)
    {
        for (int i = 0; i < numFrames; ++i, ++frame)
        {
            while (frame >= nextEvent)
                nextEvent = schedule();
            double l = 0.0, r = 0.0;
            for (auto& v : voices)
            {
                const double t = static_cast<double> (frame - v.start);
                if (t < 0.0)
                    continue;
                const double x = voiceSample (v, t);
                l += v.panL * x;
                r += v.panR * x;
            }
            const double g = currentGain();
            left[i] = static_cast<float> (g * l);
            right[i] = static_cast<float> (g * r);
            if ((frame & 1023) == 0)
                voices.erase (std::remove_if (voices.begin(), voices.end(),
                                              [this] (const Voice& v) { return static_cast<double> (frame - v.start) > v.length(); }),
                              voices.end());
        }
    }
};

SoakProgramme::SoakProgramme (double sampleRate, uint32_t seed) : impl (std::make_unique<Impl> (sampleRate, seed)) {}
SoakProgramme::~SoakProgramme() = default;

void SoakProgramme::render (float* left, float* right, int numFrames) { impl->render (left, right, numFrames); }

// ===========================================================================
// Automation
// ===========================================================================
namespace
{
constexpr int kModuleToggles[] = { GateOn, EqOn, DynEqOn, BassOn, ClarityOn, SaturationOn, SpatialOn, VirtualizerOn, CompressorOn, MaximizerOn };

class Automation
{
public:
    Automation (ParameterStore& s, const SoakSettings& settings) : store (s), set (settings), rng (settings.seed * 2654435761u + 17u) {}

    double uniform (double lo, double hi) noexcept { return lo + (hi - lo) * 0.5 * (1.0 + rng.nextBipolar()); }

    /** Frames until the next action: interval x U(0.5, 1.5). */
    int64_t nextGap (double sampleRate) noexcept
    {
        return std::max<int64_t> (1, static_cast<int64_t> (std::lround (set.intervalMs * 0.001 * sampleRate * uniform (0.5, 1.5))));
    }

    /** Applies one action; returns (kind, description). */
    std::pair<std::string, std::string> act()
    {
        if (set.automation == SoakAutomation::All)
            return fuzz();
        const double p = uniform (0.0, 1.0);
        if (p < 0.30)
        {
            const int which = static_cast<int> (rng.nextU32() % 6); // Boost or a macro
            const int id = which == 0 ? BoostIntensity : Macro1 + which - 1;
            const float v = static_cast<float> (uniform (0.0, 1.0));
            store.set (id, v);
            const auto mode = static_cast<ModeValue> (std::lround (store.get (Mode)));
            const std::string name = which == 0 ? "boost" : std::string ("macro ") + MacroMap::macroName (mode, which - 1);
            return { "macro", name + " = " + formatParameterValue (id, v) };
        }
        if (p < 0.45)
        {
            static const int kContinuous[] = { InputGainDb, OutputGainDb, eq (0, EqFieldGain), eq (2, EqFieldGain), eq (4, EqFieldGain),
                                               eq (6, EqFieldGain), eq (8, EqFieldGain), BassBoostDb, BassBoostFreq, ClarityPresence,
                                               ClarityAir, SpatialWidth, SpatialCrossfeed, CompThresholdDb, CompRatio, MaxDriveDb, SatDriveDb };
            return { "parameter", setRandom (kContinuous[rng.nextU32() % std::size (kContinuous)]) };
        }
        if (p < 0.60)
        {
            const int id = kModuleToggles[rng.nextU32() % std::size (kModuleToggles)];
            store.set (id, store.get (id) >= 0.5f ? 0.0f : 1.0f);
            return { "toggle", layout()[static_cast<size_t> (id)].key + " -> " + formatParameterValue (id, store.get (id)) };
        }
        if (p < 0.70)
        {
            store.set (Mode, store.get (Mode) >= 0.5f ? 0.0f : 1.0f);
            return { "mode", "mode -> " + formatParameterValue (Mode, store.get (Mode)) };
        }
        if (p < 0.85 && ! set.presets.empty())
        {
            const size_t k = rng.nextU32() % set.presets.size();
            const auto& values = set.presets[k];
            for (int id = 0; id < kNumParams && static_cast<size_t> (id) < values.size(); ++id)
                if (! preset::isAppState (id) && ! layout()[static_cast<size_t> (id)].structural)
                    store.set (id, values[static_cast<size_t> (id)]);
            return { "preset", "preset '" + (k < set.presetNames.size() ? set.presetNames[k] : std::to_string (k)) + "'" };
        }
        if (p < 0.93)
        {
            store.set (BypassAll, store.get (BypassAll) >= 0.5f ? 0.0f : 1.0f);
            return { "bypass", "bypass -> " + formatParameterValue (BypassAll, store.get (BypassAll)) };
        }
        // A/B: the other bank gets the current settings with another Boost, then becomes active.
        const Bank from = store.getActiveBank(), to = from == Bank::A ? Bank::B : Bank::A;
        store.copyBank (from, to);
        const float boost = static_cast<float> (uniform (0.0, 1.0));
        store.set (to, BoostIntensity, boost);
        store.setActiveBank (to);
        return { "bank", std::string ("bank -> ") + (to == Bank::A ? "A" : "B") + " (boost " + formatParameterValue (BoostIntensity, boost) + ")" };
    }

private:
    std::string setRandom (int id)
    {
        const auto& info = layout()[static_cast<size_t> (id)];
        float v = 0.0f;
        if (info.unit == Unit::Toggle)
            v = store.get (id) >= 0.5f ? 0.0f : 1.0f;
        else if (info.unit == Unit::Choice)
            v = static_cast<float> (rng.nextU32() % std::max<size_t> (1, info.choices.size()));
        else
            v = static_cast<float> (uniform (info.minValue, info.maxValue));
        store.set (id, v);
        return info.key + " = " + formatParameterValue (id, store.get (id));
    }

    std::pair<std::string, std::string> fuzz()
    {
        for (;;)
        {
            const int id = static_cast<int> (rng.nextU32() % static_cast<uint32_t> (kNumParams));
            if (! layout()[static_cast<size_t> (id)].structural)
                return { "fuzz", setRandom (id) };
        }
    }

    ParameterStore& store;
    const SoakSettings& set;
    FastRandom rng;
};

float toDb (double v) noexcept { return static_cast<float> (20.0 * std::log10 (std::max (v, 1.0e-8))); }

json::Value rounded (double v, int decimals)
{
    if (! std::isfinite (v) || v <= -159.5)
        return json::Value();
    const double scale = std::pow (10.0, decimals);
    return json::Value (std::round (v * scale) / scale);
}

json::Value countsToJson (const std::array<int64_t, kNumDiscontinuityTypes>& counts)
{
    json::Value v;
    int64_t total = 0;
    for (int t = 0; t < kNumDiscontinuityTypes; ++t)
    {
        v.set (discontinuityName (static_cast<DiscontinuityType> (t)), static_cast<double> (counts[static_cast<size_t> (t)]));
        total += counts[static_cast<size_t> (t)];
    }
    v.set ("total", static_cast<double> (total));
    return v;
}

std::string countsToText (const std::array<int64_t, kNumDiscontinuityTypes>& counts)
{
    std::string s;
    for (int t = 0; t < kNumDiscontinuityTypes; ++t)
        s += (t == 0 ? "" : ", ") + std::to_string (counts[static_cast<size_t> (t)]) + " " + discontinuityName (static_cast<DiscontinuityType> (t));
    return s;
}
} // namespace

// ===========================================================================
// Runner
// ===========================================================================
int64_t SoakReport::outputTotal() const noexcept
{
    int64_t t = 0;
    for (auto c : output)
        t += c;
    return t;
}

int64_t SoakReport::inputTotal() const noexcept
{
    int64_t t = 0;
    for (auto c : input)
        t += c;
    return t;
}

const char* soakAutomationName (SoakAutomation a) noexcept
{
    switch (a)
    {
        case SoakAutomation::Off: return "off";
        case SoakAutomation::User: return "user";
        case SoakAutomation::All: return "all";
    }
    return "";
}

bool soakChain (const std::vector<float>& baseValues, const SoakSettings& s, SoakReport& report, std::string& error)
{
    report = SoakReport();
    if (baseValues.size() != static_cast<size_t> (kNumParams))
    {
        error = "internal error: parameter table has the wrong size";
        return false;
    }
    if (! (s.seconds > 0.0) || ! std::isfinite (s.seconds) || ! (s.sampleRate >= 8000.0 && s.sampleRate <= 768000.0))
    {
        error = "soak needs a positive duration and a sample rate of 8000 .. 768000 Hz";
        return false;
    }
    const int blockSize = std::clamp (s.blockSize, 16, 16384);
    const double fs = s.sampleRate;
    const auto total = static_cast<int64_t> (std::llround (s.seconds * fs));

    auto store = std::make_unique<ParameterStore>();
    for (int id = 0; id < kNumParams; ++id)
    {
        store->set (Bank::A, id, baseValues[static_cast<size_t> (id)]);
        store->set (Bank::B, id, baseValues[static_cast<size_t> (id)]);
    }
    store->setActiveBank (Bank::A);
    auto chain = std::make_unique<ProcessingChain> (*store);
    chain->setProtectionStrength (s.protection);
    chain->prepare ({ fs, blockSize, 2 });

    AudioBuffer io (2, blockSize);
    ScopedNoDenormals noDenormals;
    io.clear();
    chain->process (io.block (2, blockSize)); // prime, as renderPass does
    chain->reset();

    SoakProgramme programme (fs, s.seed);
    Automation automation (*store, s);
    DiscontinuityDetector in, out;
    in.prepare (fs, 2, s.detector);
    out.prepare (fs, 2, s.detector);

    std::vector<std::pair<int64_t, std::string>> actions; // (frame, description)
    std::map<std::string, int64_t> kinds;
    int64_t nextAction = s.automation == SoakAutomation::Off ? total : automation.nextGap (fs);
    double peak = 0.0, blockMsSum = 0.0;
    const double budgetMs = 1000.0 * blockSize / fs;
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    int64_t blocks = 0;

    for (int64_t pos = 0; pos < total; pos += blockSize)
    {
        const int n = static_cast<int> (std::min<int64_t> (blockSize, total - pos));
        while (nextAction <= pos)
        {
            auto [kind, text] = automation.act();
            ++kinds[kind];
            actions.emplace_back (pos, std::move (text));
            nextAction += automation.nextGap (fs);
        }
        float* ch[] = { io.channel (0), io.channel (1) };
        programme.render (ch[0], ch[1], n);
        in.process (ch, n);

        const auto t0 = Clock::now();
        chain->process (io.block (2, n));
        const double blockMs = std::chrono::duration<double, std::milli> (Clock::now() - t0).count();
        blockMsSum += blockMs;
        report.maxBlockMs = std::max (report.maxBlockMs, blockMs);
        if (blockMs > budgetMs * n / blockSize)
            ++report.blocksOverBudget;
        ++blocks;

        if (s.inject)
            s.inject (pos, ch, n);
        if (s.progress && (pos + n) / static_cast<int64_t> (10.0 * fs) != pos / static_cast<int64_t> (10.0 * fs))
            s.progress (static_cast<double> (pos + n) / fs);
        out.process (ch, n);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
                if (std::isfinite (ch[c][i]))
                    peak = std::max (peak, static_cast<double> (std::abs (ch[c][i])));
    }
    in.finish();
    out.finish();

    report.seconds = static_cast<double> (total) / fs;
    report.sampleRate = fs;
    report.blockSize = blockSize;
    report.latencySamples = chain->getLatencySamples();
    report.seed = s.seed;
    report.automation = s.automation;
    report.frames = total;
    report.actions = static_cast<int64_t> (actions.size());
    for (const auto& [kind, count] : kinds)
        report.actionCounts.emplace_back (kind, count);
    for (int t = 0; t < kNumDiscontinuityTypes; ++t)
    {
        report.output[static_cast<size_t> (t)] = out.count (static_cast<DiscontinuityType> (t));
        report.input[static_cast<size_t> (t)] = in.count (static_cast<DiscontinuityType> (t));
    }
    for (const auto& e : out.events())
    {
        SoakDetection d;
        d.event = e;
        d.seconds = static_cast<double> (e.frame) / fs;
        // The latest action applied at or before the block that holds the event.
        const auto it = std::upper_bound (actions.begin(), actions.end(), e.frame, [] (int64_t f, const auto& a) { return f < a.first; });
        if (it != actions.begin())
        {
            d.lastAction = std::prev (it)->second;
            d.lastActionAgeMs = 1000.0 * static_cast<double> (e.frame - std::prev (it)->first) / fs;
        }
        report.detections.push_back (std::move (d));
    }
    report.outputPeakDbfs = peak > 0.0 ? toDb (peak) : -160.0f;
    report.wallSeconds = std::chrono::duration<double> (Clock::now() - start).count();
    report.meanBlockMs = blocks > 0 ? blockMsSum / static_cast<double> (blocks) : 0.0;
    report.blockBudgetMs = budgetMs;
    return true;
}

// ===========================================================================
// Reports
// ===========================================================================
json::Value soakToJson (const SoakReport& r)
{
    json::Value v;
    v.set ("seconds", rounded (r.seconds, 3));
    v.set ("sampleRate", r.sampleRate);
    v.set ("blockSize", r.blockSize);
    v.set ("latencySamples", r.latencySamples);
    v.set ("seed", static_cast<double> (r.seed));
    v.set ("automation", soakAutomationName (r.automation));
    json::Value actions;
    actions.set ("total", static_cast<double> (r.actions));
    for (const auto& [kind, count] : r.actionCounts)
        actions.set (kind, static_cast<double> (count));
    v.set ("actions", std::move (actions));
    v.set ("output", countsToJson (r.output));
    v.set ("input", countsToJson (r.input));
    json::Value list { json::Value::Array {} };
    for (const auto& d : r.detections)
    {
        json::Value e;
        e.set ("type", discontinuityName (d.event.type));
        e.set ("channel", d.event.channel);
        e.set ("seconds", rounded (d.seconds, 4));
        e.set ("frame", static_cast<double> (d.event.frame));
        if (d.event.length > 0)
            e.set ("lengthMs", rounded (1000.0 * static_cast<double> (d.event.length) / r.sampleRate, 2));
        e.set ("levelDb", rounded (d.event.levelDb, 2));
        if (d.event.type == DiscontinuityType::Click)
            e.set ("overDb", rounded (d.event.overDb, 2));
        e.set ("lastAction", d.lastAction.empty() ? json::Value() : json::Value (d.lastAction));
        e.set ("lastActionAgeMs", d.lastAction.empty() ? json::Value() : rounded (d.lastActionAgeMs, 1));
        list.push (std::move (e));
    }
    v.set ("detections", std::move (list));
    v.set ("outputPeakDbfs", rounded (r.outputPeakDbfs, 2));
    json::Value timing;
    timing.set ("wallSeconds", rounded (r.wallSeconds, 3));
    timing.set ("realtimeFactor", rounded (r.wallSeconds > 0.0 ? r.seconds / r.wallSeconds : 0.0, 1));
    timing.set ("blockBudgetMs", rounded (r.blockBudgetMs, 3));
    timing.set ("maxBlockMs", rounded (r.maxBlockMs, 3));
    timing.set ("meanBlockMs", rounded (r.meanBlockMs, 4));
    timing.set ("blocksOverBudget", static_cast<double> (r.blocksOverBudget));
    v.set ("timing", std::move (timing));
    return v;
}

std::string formatSoak (const SoakReport& r)
{
    char buf[256];
    std::string s;
    std::snprintf (buf, sizeof (buf), "Soak    : %.1f s at %g Hz, block %d, latency %d samples, seed %u, automation %s (%lld actions)\n",
                   r.seconds, r.sampleRate, r.blockSize, r.latencySamples, r.seed, soakAutomationName (r.automation),
                   static_cast<long long> (r.actions));
    s += buf;
    if (! r.actionCounts.empty())
    {
        s += "Actions :";
        for (const auto& [kind, count] : r.actionCounts)
            s += " " + kind + " " + std::to_string (count);
        s += "\n";
    }
    s += "Output  : " + countsToText (r.output) + "\n";
    s += "Input   : " + countsToText (r.input) + " (programme self-check)\n";
    for (const auto& d : r.detections)
    {
        std::snprintf (buf, sizeof (buf), "  %10.4f s  ch %d  %-10s %7.1f dB", d.seconds, d.event.channel, discontinuityName (d.event.type),
                       static_cast<double> (d.event.levelDb));
        s += buf;
        if (d.event.type == DiscontinuityType::Click)
        {
            std::snprintf (buf, sizeof (buf), " (+%.1f dB over the residual)", static_cast<double> (d.event.overDb));
            s += buf;
        }
        if (d.event.length > 0)
        {
            std::snprintf (buf, sizeof (buf), " %.2f ms", 1000.0 * static_cast<double> (d.event.length) / r.sampleRate);
            s += buf;
        }
        if (! d.lastAction.empty())
        {
            std::snprintf (buf, sizeof (buf), "  after: %s (%.0f ms before)", d.lastAction.c_str(), d.lastActionAgeMs);
            s += buf;
        }
        s += "\n";
    }
    if (static_cast<int64_t> (r.detections.size()) < r.outputTotal())
        s += "  ... (" + std::to_string (r.outputTotal() - static_cast<int64_t> (r.detections.size())) + " more)\n";
    std::snprintf (buf, sizeof (buf), "Peak    : %.2f dBFS\nTiming  : %.1f x real time; block %.3f ms mean, %.3f ms max of a %.3f ms budget (%lld over)\n",
                   static_cast<double> (r.outputPeakDbfs), r.wallSeconds > 0.0 ? r.seconds / r.wallSeconds : 0.0, r.meanBlockMs, r.maxBlockMs,
                   r.blockBudgetMs, static_cast<long long> (r.blocksOverBudget));
    s += buf;
    s += r.outputTotal() == 0 ? "Result  : no discontinuities\n" : "Result  : DISCONTINUITIES FOUND\n";
    return s;
}
} // namespace flub::cli
