#include "DeviceSoak.h"

#include "diagnostics/ProcessStats.h"

#include "flub/common/AudioBlock.h"
#include "flub/engine/Protection.h"
#include "flub/io/WavFile.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <map>

namespace flub::app
{
using namespace flub::param;

namespace
{
constexpr int kReportVersion = 1;
constexpr double kTapSeconds = 4.0;         // the tap's ring
constexpr double kStatusIntervalMs = 1000.0; // callback timing, CPU load
constexpr double kProcessIntervalMs = 10000.0; // memory, process CPU
constexpr double kStallSeconds = 15.0;       // no frames for this long: the device stalled
constexpr double kRestartWindowSeconds = 1.0;
constexpr double kProgrammeGainDb = -6.0;
constexpr int kMaxListedTimes = 200;          // late / over-budget times kept in the report
constexpr int kVirtualBlocksPerStep = 32;
constexpr int kVirtualIdleStepsLimit = 64;    // virtual clock: steps in a row without a new frame before it aborts

const char* profileName (LatencyProfileValue p) noexcept
{
    switch (p)
    {
        case LatencyProfileValue::Quality: return "Quality";
        case LatencyProfileValue::LowLatency: return "Low Latency";
        case LatencyProfileValue::Balanced: break;
    }
    return "Balanced";
}

const char* profileKey (LatencyProfileValue p) noexcept
{
    switch (p)
    {
        case LatencyProfileValue::Quality: return "quality";
        case LatencyProfileValue::LowLatency: return "low";
        case LatencyProfileValue::Balanced: break;
    }
    return "balanced";
}

std::optional<LatencyProfileValue> profileFromKey (const juce::String& key)
{
    const auto k = key.trim().toLowerCase();
    if (k == "quality")
        return LatencyProfileValue::Quality;
    if (k == "balanced")
        return LatencyProfileValue::Balanced;
    if (k == "low" || k == "low-latency" || k == "lowlatency" || k == "low latency")
        return LatencyProfileValue::LowLatency;
    return std::nullopt;
}

const char* protectionName (int index) noexcept
{
    return index == 1 ? "Normal" : (index == 2 ? "Strict" : "Off");
}

juce::String defaultDeviceType()
{
   #if JUCE_WINDOWS
    return "Windows Audio (Low Latency Mode)";
   #else
    return {};
   #endif
}

/** The value after `flag`, or empty (missing, or another flag). */
juce::String valueOf (const juce::StringArray& args, const char* flag)
{
    const int i = args.indexOf (flag);
    if (i < 0 || i + 1 >= args.size() || args[i + 1].startsWith ("--"))
        return {};
    return args[i + 1].unquoted();
}

/** A programme time in seconds: digits with at most one decimal point,
    0 .. 24 h (no sign, no exponent, nothing else). */
bool parseSeconds (const juce::String& token, double& seconds)
{
    const auto t = token.trim();
    if (t.isEmpty() || ! t.containsOnly ("0123456789.") || t.indexOfChar ('.') != t.lastIndexOfChar ('.') || t == ".")
        return false;
    seconds = t.getDoubleValue();
    return std::isfinite (seconds) && seconds >= 0.0 && seconds <= 24.0 * 3600.0;
}

juce::var v64 (int64_t value)
{
    return juce::var (static_cast<juce::int64> (value));
}

double nowMs() noexcept
{
    return juce::Time::getMillisecondCounterHiRes();
}

double round2 (double v) noexcept
{
    return std::round (v * 100.0) / 100.0;
}

double round3 (double v) noexcept
{
    return std::round (v * 1000.0) / 1000.0;
}

float uniform01 (flub::FastRandom& rng) noexcept
{
    return 0.5f * (rng.nextBipolar() + 1.0f);
}

int uniformInt (flub::FastRandom& rng, int count) noexcept
{
    return count <= 1 ? 0 : static_cast<int> (rng.nextU32() % static_cast<uint32_t> (count));
}

juce::String typeName (flub::DiscontinuityType type)
{
    return flub::discontinuityName (type);
}

// The kinds an automation round draws (some more than once: what users touch
// most). Every kind comes up once per round.
constexpr SoakAction::Kind kBag[] = {
    SoakAction::Kind::Preset,     SoakAction::Kind::Preset,  SoakAction::Kind::Boost,       SoakAction::Kind::Boost,
    SoakAction::Kind::Macro,      SoakAction::Kind::Macro,   SoakAction::Kind::Macro,       SoakAction::Kind::Bypass,
    SoakAction::Kind::StripBypass, SoakAction::Kind::Bank,   SoakAction::Kind::Mute,        SoakAction::Kind::Gain,
    SoakAction::Kind::Profile,    SoakAction::Kind::Mode,    SoakAction::Kind::Audition,    SoakAction::Kind::Night,
    SoakAction::Kind::Focus,      SoakAction::Kind::Protection, SoakAction::Kind::Smart,   SoakAction::Kind::Gain,
};

// The module ears (ModuleCard's audition bypass).
constexpr int kAuditionModules[] = { EqOn, BassOn, ClarityOn, SaturationOn, SpatialOn, VirtualizerOn, CompressorOn, MaximizerOn };
} // namespace

// =============================================================================
// Command line
// =============================================================================
bool parseDeviceSoakCommandLine (const juce::StringArray& args, DeviceSoakOptions& options, juce::String& error)
{
    if (! args.contains ("--device-soak"))
        return false;

    options.list = args.contains ("--list");
    options.ui = args.contains ("--ui");
    options.device = valueOf (args, "--device");
    options.type = valueOf (args, "--type");

    if (args.contains ("--replay"))
    {
        const auto path = valueOf (args, "--replay");
        if (path.isEmpty())
        {
            error = "--replay needs a device-soak report, e.g. --replay soak.json";
            return false;
        }
        options.replay = juce::File::getCurrentWorkingDirectory().getChildFile (path);
    }

    if (args.contains ("--report"))
    {
        const auto path = valueOf (args, "--report");
        if (path.isEmpty())
        {
            error = "--report needs a file, e.g. --report soak.json";
            return false;
        }
        options.report = juce::File::getCurrentWorkingDirectory().getChildFile (path);
    }

    if (args.contains ("--buffer"))
    {
        const auto value = valueOf (args, "--buffer").toLowerCase();
        if (value == "min" || value == "smallest")
            options.smallestBuffer = true;
        else if (value != "default")
        {
            options.bufferSize = value.getIntValue();
            if (! value.containsOnly ("0123456789") || options.bufferSize < 16 || options.bufferSize > 16384)
            {
                error = "--buffer must be a size in samples (16 .. 16384), 'min' or 'default'";
                return false;
            }
        }
    }

    if (args.contains ("--rate"))
    {
        options.sampleRate = valueOf (args, "--rate").getDoubleValue();
        if (options.sampleRate < 8000.0 || options.sampleRate > 384000.0)
        {
            error = "--rate must be a sample rate in Hz, e.g. --rate 48000";
            return false;
        }
    }

    if (args.contains ("--minutes"))
    {
        options.minutes = valueOf (args, "--minutes").getDoubleValue();
        if (! (options.minutes >= 0.05 && options.minutes <= 24.0 * 60.0))
        {
            error = "--minutes must be between 0.05 and 1440";
            return false;
        }
    }

    if (args.contains ("--profile"))
    {
        const auto profile = profileFromKey (valueOf (args, "--profile"));
        if (! profile)
        {
            error = "--profile must be 'quality', 'balanced' or 'low'";
            return false;
        }
        options.profile = *profile;
    }

    if (args.contains ("--seed"))
    {
        const auto value = valueOf (args, "--seed");
        if (value.isEmpty() || ! value.containsOnly ("0123456789"))
        {
            error = "--seed must be a whole number";
            return false;
        }
        options.seed = static_cast<uint32_t> (value.getLargeIntValue());
    }

    if (args.contains ("--interval"))
    {
        options.intervalMs = valueOf (args, "--interval").getDoubleValue();
        if (! (options.intervalMs >= 50.0 && options.intervalMs <= 600000.0))
        {
            error = "--interval must be the mean time between actions in ms (50 .. 600000)";
            return false;
        }
    }

    if (args.contains ("--dump"))
    {
        // Every time must read as seconds >= 0 (a negative or a non-numeric
        // one would otherwise silently become a time the stream never had).
        bool valid = true;
        for (const auto& t : juce::StringArray::fromTokens (valueOf (args, "--dump"), ",", {}))
        {
            double seconds = 0.0;
            if (parseSeconds (t, seconds))
                options.dumpAt.push_back (seconds);
            else
                valid = false;
        }
        if (! valid || options.dumpAt.empty())
        {
            error = "--dump needs programme times in seconds (0 or more), e.g. --dump 199.68,350.26";
            return false;
        }
    }
    options.allowAudible = args.contains ("--allow-audible");

    if (args.contains ("--automation"))
    {
        const auto value = valueOf (args, "--automation").toLowerCase();
        if (value != "user" && value != "off")
        {
            error = "--automation must be 'user' or 'off'";
            return false;
        }
        options.automation = value == "user";
    }

    if (options.replay == juce::File() && ! options.list && options.device.isEmpty())
    {
        error = "--device-soak needs --device \"<output name>\" (see --device-soak --list), or --replay <report.json>";
        return false;
    }
    return true;
}

// =============================================================================
// SoakAction
// =============================================================================
const char* SoakAction::kindName (Kind kind) noexcept
{
    switch (kind)
    {
        case Kind::Preset: return "preset";
        case Kind::Boost: return "boost";
        case Kind::Macro: return "macro";
        case Kind::Bypass: return "bypass";
        case Kind::StripBypass: return "strip-bypass";
        case Kind::Bank: return "bank";
        case Kind::Mute: return "mute";
        case Kind::Gain: return "gain";
        case Kind::Profile: return "profile";
        case Kind::Mode: return "mode";
        case Kind::Audition: return "audition";
        case Kind::Night: return "night";
        case Kind::Focus: return "focus";
        case Kind::Protection: return "protection";
        case Kind::Smart: return "smart";
        case Kind::Param: return "param";
    }
    return "?";
}

std::optional<SoakAction::Kind> SoakAction::kindFromName (const juce::String& name)
{
    for (int k = 0; k < kNumKinds; ++k)
        if (name == kindName (static_cast<Kind> (k)))
            return static_cast<Kind> (k);
    return std::nullopt;
}

juce::String SoakAction::describe (const EngineController& controller) const
{
    const auto stripName = controller.getStripName (strip);
    const auto onOff = [this] { return value >= 0.5f ? juce::String ("on") : juce::String ("off"); };
    // A parameter's key, or its number when it is no parameter (never indexes out of the layout).
    const auto key = [this] { return param >= 0 && param < kNumParams ? juce::String (layout()[static_cast<size_t> (param)].key) : juce::String (param); };
    juce::String text = juce::String (kindName (kind)) + " ";
    switch (kind)
    {
        case Kind::Preset: return text + stripName + " " + presetName;
        case Kind::Boost: return text + stripName + " " + juce::String (value, 2);
        case Kind::Macro: return text + stripName + " " + key() + " " + juce::String (value, 2);
        case Kind::Bypass: return text + onOff();
        case Kind::StripBypass:
        case Kind::Mute:
        case Kind::Night:
        case Kind::Focus:
        case Kind::Smart: return text + stripName + " " + onOff();
        case Kind::Param: return text + stripName + " " + key() + " " + juce::String (value, 3);
        case Kind::Bank: return text + stripName + (value >= 0.5f ? " B" : " A");
        case Kind::Gain: return text + stripName + " " + juce::String (value, 1) + " dB";
        case Kind::Profile: return text + profileName (static_cast<LatencyProfileValue> (std::clamp (juce::roundToInt (value), 0, 2)));
        case Kind::Mode: return text + stripName + (juce::roundToInt (value) == static_cast<int> (ModeValue::Gaming) ? " Gaming" : " Music");
        case Kind::Audition: return text + stripName + " " + key() + " " + onOff();
        case Kind::Protection: return text + protectionName (juce::roundToInt (value));
    }
    return text;
}

// =============================================================================
// Virtual device
// =============================================================================
SoakVirtualDevice::SoakVirtualDevice (const juce::String& outputName, const juce::String& typeName)
    : juce::AudioIODevice (outputName, typeName)
{
}

SoakVirtualDevice::~SoakVirtualDevice()
{
    close();
}

juce::String SoakVirtualDevice::open (const juce::BigInteger&, const juce::BigInteger& outputChannels, double sampleRate, int bufferSizeSamples)
{
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    blockSize = bufferSizeSamples > 0 ? bufferSizeSamples : getDefaultBufferSize();
    activeOutputs = outputChannels;
    activeOutputs.setRange (2, 30, false);
    if (activeOutputs.isZero())
        activeOutputs.setRange (0, 2, true);
    for (auto& o : outputs)
        o.assign (static_cast<size_t> (blockSize), 0.0f);
    opened = true;
    return {};
}

void SoakVirtualDevice::close()
{
    stop();
    opened = false;
}

void SoakVirtualDevice::start (juce::AudioIODeviceCallback* cb)
{
    if (cb != nullptr && callback == nullptr && opened)
    {
        cb->audioDeviceAboutToStart (this);
        callback = cb;
    }
}

void SoakVirtualDevice::stop()
{
    if (auto* cb = std::exchange (callback, nullptr))
        cb->audioDeviceStopped();
}

bool SoakVirtualDevice::process()
{
    if (callback == nullptr)
        return false;
    std::array<float*, 2> outs { outputs[0].data(), outputs[1].data() };
    callback->audioDeviceIOCallbackWithContext (nullptr, 0, outs.data(), activeOutputs.countNumberOfSetBits(), blockSize,
                                                juce::AudioIODeviceCallbackContext {});
    return true;
}

SoakVirtualDeviceType::SoakVirtualDeviceType (juce::StringArray outputNames)
    : juce::AudioIODeviceType (kTypeName), outputs (std::move (outputNames))
{
}

int SoakVirtualDeviceType::getIndexOfDevice (juce::AudioIODevice* device, bool asInput) const
{
    return device != nullptr && ! asInput ? outputs.indexOf (device->getName()) : -1;
}

juce::AudioIODevice* SoakVirtualDeviceType::createDevice (const juce::String& outputDeviceName, const juce::String& inputDeviceName)
{
    if (inputDeviceName.isNotEmpty() || ! outputs.contains (outputDeviceName))
        return nullptr;
    return new SoakVirtualDevice (outputDeviceName, kTypeName);
}

// =============================================================================
// The programme: the device callback's strip source
// =============================================================================
class DeviceSoak::Programme final : public StripSignalSource
{
public:
    Programme (double rate, int game, int music, const flub::StreamTap& streamTap, int64_t pulseFrame)
        : generator (rate), gameStrip (game), musicStrip (music), tap (streamTap), pulseAt (pulseFrame)
    {
        assign (generator, gameStrip, musicStrip);
    }

    static void assign (TestSignalGenerator& g, int game, int music)
    {
        g.setProgramme (game, TestSignalGenerator::Programme::Game71, static_cast<float> (kProgrammeGainDb));
        if (music != game)
            g.setProgramme (music, TestSignalGenerator::Programme::Music, static_cast<float> (kProgrammeGainDb));
    }

    // Audio thread: allocation- and lock-free (TestSignalGenerator renders
    // sample by sample into the block).
    bool renderStrip (int strip, const flub::AudioBlock& block) override
    {
        if (startFrame.load (std::memory_order_relaxed) < 0)
            startFrame.store (tap.framesWritten(), std::memory_order_release); // the tap runs from before the first render
        const bool fed = generator.renderStrip (strip, block);
        if (strip == musicStrip && fed)
        {
            // Tests: a one-sample impulse of +0.5 (a hard break in value).
            const int64_t pos = musicPos;
            musicPos += block.numSamples;
            if (pulseAt >= pos && pulseAt < musicPos)
                for (int c = 0; c < block.numChannels; ++c)
                    block.channel (c)[pulseAt - pos] += 0.5f;
        }
        return fed;
    }

    /** The stream frame of the programme's first frame; -1 before it started. */
    int64_t getStartFrame() const noexcept { return startFrame.load (std::memory_order_acquire); }

private:
    TestSignalGenerator generator;
    const int gameStrip, musicStrip;
    const flub::StreamTap& tap;
    const int64_t pulseAt;
    int64_t musicPos = 0;
    std::atomic<int64_t> startFrame { -1 };
};

// =============================================================================
// The analysis (message thread)
// =============================================================================
struct DeviceSoak::Analysis
{
    flub::DiscontinuitySettings settings;
    double rate = 48000.0;

    // The device stream, in segments between tap gaps.
    flub::DiscontinuityDetector detector;
    int64_t firstFrame = -1, segmentStart = -1, expected = -1;
    std::array<int64_t, flub::kNumDiscontinuityTypes> counts {};
    int64_t kinks = 0, recurring = 0, frames = 0;
    std::vector<flub::Discontinuity> events;
    std::vector<std::pair<int64_t, int64_t>> gaps; // (stream frame, frames missing)
    double peak = 0.0, sumSquares = 0.0;

    // The dry programme (self-check): the same generator rendered again.
    std::unique_ptr<TestSignalGenerator> dry;
    int gameStrip = 0, musicStrip = 1;
    flub::DiscontinuityDetector dryDetector;
    // The dry programme read 6 dB more sensitively for DC steps: the
    // triage's "programme" class (the game scene's explosions move the 2 Hz
    // low-pass by about the threshold itself, so the processed and the dry
    // reading land on either side of it).
    flub::DiscontinuityDetector dryNearDetector;
    static constexpr float kNearDb = 6.0f;
    int64_t dryStart = -1; // stream frame of the programme's first frame
    int64_t dryRendered = 0;
    flub::AudioBuffer gameBuffer, musicBuffer;
    std::vector<float> left, right, dryLeft, dryRight;
    // The last kHistorySeconds of the stream (--dump).
    static constexpr double kHistorySeconds = 2.0;
    std::vector<float> historyLeft, historyRight;
    int64_t historyEnd = 0; // stream frame after the newest one kept

    void prepare (double fs, int block, int game, int music)
    {
        rate = fs;
        settings.blockSize = block;
        settings.maxReported = 2000;
        detector.prepare (rate, 2, settings);
        dryDetector.prepare (rate, 2, settings);
        auto sensitive = settings;
        sensitive.dcStepDb -= kNearDb;
        dryNearDetector.prepare (rate, 2, sensitive);
        gameStrip = game;
        musicStrip = music;
        dry = std::make_unique<TestSignalGenerator> (rate);
        Programme::assign (*dry, gameStrip, musicStrip);
        gameBuffer.setSize (2, flub::StreamTap::kChunkFrames);
        musicBuffer.setSize (2, flub::StreamTap::kChunkFrames);
        left.resize (flub::StreamTap::kChunkFrames);
        right.resize (flub::StreamTap::kChunkFrames);
        dryLeft.resize (flub::StreamTap::kChunkFrames);
        dryRight.resize (flub::StreamTap::kChunkFrames);
        historyLeft.assign (static_cast<size_t> (kHistorySeconds * fs) + 1, 0.0f);
        historyRight.assign (historyLeft.size(), 0.0f);
    }

    void harvest()
    {
        for (int t = 0; t < flub::kNumDiscontinuityTypes; ++t)
            counts[static_cast<size_t> (t)] += detector.count (static_cast<flub::DiscontinuityType> (t));
        kinks += detector.kinks();
        recurring += detector.recurring();
        for (auto e : detector.events())
        {
            e.frame += segmentStart;
            events.push_back (e);
        }
    }

    void closeSegment()
    {
        if (segmentStart < 0)
            return;
        detector.finish();
        harvest();
        detector.reset();
        segmentStart = -1;
    }

    /** The dry programme for stream frames [from, from + n) (silence before it started). */
    void renderDry (int64_t from, int n)
    {
        std::fill (dryLeft.begin(), dryLeft.begin() + n, 0.0f);
        std::fill (dryRight.begin(), dryRight.begin() + n, 0.0f);
        if (dryStart < 0 || from + n <= dryStart)
            return;
        const int skip = from < dryStart ? static_cast<int> (dryStart - from) : 0;
        const int count = n - skip;
        // The generator is rendered in stream order; frames dropped by the tap are rendered and discarded.
        const int64_t wanted = from + skip - dryStart;
        while (dryRendered < wanted)
        {
            const int k = static_cast<int> (std::min<int64_t> (wanted - dryRendered, flub::StreamTap::kChunkFrames));
            dry->renderStrip (gameStrip, gameBuffer.block (2, k));
            dry->renderStrip (musicStrip, musicBuffer.block (2, k));
            dryRendered += k;
        }
        const auto g = gameBuffer.block (2, count);
        const auto m = musicBuffer.block (2, count);
        const bool gameFed = dry->renderStrip (gameStrip, g);
        const bool musicFed = musicStrip != gameStrip && dry->renderStrip (musicStrip, m);
        for (int i = 0; i < count; ++i)
        {
            dryLeft[static_cast<size_t> (skip + i)] = (gameFed ? g.channel (0)[i] : 0.0f) + (musicFed ? m.channel (0)[i] : 0.0f);
            dryRight[static_cast<size_t> (skip + i)] = (gameFed ? g.channel (1)[i] : 0.0f) + (musicFed ? m.channel (1)[i] : 0.0f);
        }
        dryRendered += count;
    }

    /** The stream frame of the dry detector's frame `f`: it reads the frames
        the tap delivered, from the first one on, without the gaps. */
    int64_t dryToStream (int64_t f) const noexcept
    {
        int64_t pos = f + std::max<int64_t> (0, firstFrame);
        for (const auto& gap : gaps)
            if (gap.first <= pos)
                pos += gap.second;
        return pos;
    }

    void process (const flub::StreamTap::Chunk& chunk)
    {
        const int n = chunk.numFrames;
        if (firstFrame < 0)
            firstFrame = chunk.frame;
        if (expected >= 0 && chunk.frame != expected)
        {
            // Frames dropped by the tap: a new segment (the splice is not a click).
            gaps.emplace_back (expected, chunk.frame - expected);
            closeSegment();
        }
        if (segmentStart < 0)
            segmentStart = chunk.frame;
        expected = chunk.frame + n;

        for (int i = 0; i < n; ++i)
        {
            const float l = chunk.samples[2 * i], r = chunk.samples[2 * i + 1];
            left[static_cast<size_t> (i)] = l;
            right[static_cast<size_t> (i)] = r;
            const auto h = static_cast<size_t> ((chunk.frame + i) % static_cast<int64_t> (historyLeft.size()));
            historyLeft[h] = l;
            historyRight[h] = r;
            if (std::isfinite (l) && std::isfinite (r))
            {
                peak = std::max ({ peak, static_cast<double> (std::abs (l)), static_cast<double> (std::abs (r)) });
                sumSquares += 0.5 * (static_cast<double> (l) * l + static_cast<double> (r) * r);
            }
        }
        const std::array<const float*, 2> ch { left.data(), right.data() };
        detector.process (ch.data(), n);
        frames += n;
        historyEnd = chunk.frame + n;

        renderDry (chunk.frame, n);
        const std::array<const float*, 2> dryCh { dryLeft.data(), dryRight.data() };
        dryDetector.process (dryCh.data(), n);
        dryNearDetector.process (dryCh.data(), n);
    }

    void finish()
    {
        closeSegment();
        dryDetector.finish();
        dryNearDetector.finish();
    }

    int64_t total() const noexcept
    {
        int64_t t = 0;
        for (const auto c : counts)
            t += c;
        return t;
    }
};

// =============================================================================
// Set-up helpers
// =============================================================================
EngineController::Options DeviceSoak::makeEngineOptions (const DeviceSoakOptions& options, Clock clock, const juce::File& settingsFile)
{
    EngineController::Options o;
    o.openAudioDevice = true;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.persistSettings = false;
    o.settingsFile = settingsFile;
    o.foregroundAppFactory = [] { return std::unique_ptr<flub::platform::ForegroundApp>(); }; // no automatic profiles
    o.antiCheatServices = [] { return std::vector<std::string>(); };                         // no Tournament switch

    const bool isVirtual = clock == Clock::Virtual;
    const juce::String output = isVirtual ? juce::String (SoakVirtualDeviceType::kOutputName) : options.device;
    auto xml = std::make_shared<juce::XmlElement> ("DEVICESETUP");
    xml->setAttribute ("deviceType", isVirtual ? juce::String (SoakVirtualDeviceType::kTypeName) : options.type);
    xml->setAttribute ("audioOutputDeviceName", output);
    xml->setAttribute ("audioInputDeviceName", juce::String());
    if (options.bufferSize > 0)
        xml->setAttribute ("audioDeviceBufferSize", options.bufferSize);
    if (options.sampleRate > 0.0)
        xml->setAttribute ("audioDeviceRate", options.sampleRate);
    o.deviceState = std::move (xml);
    o.outputPin = output;

    o.beforeDeviceOpen = [isVirtual] (AudioEngineHost& host)
    {
        // docs/11 E42c / E53: the soak runs at the buffer it asked for (--buffer,
        // or the device's default) for the whole run. Automatic buffer size is
        // off before the device opens (the temporary settings have no stored
        // choice, so the upgrade path would turn it on at a device-default
        // size): its latency-profile actions are engine swaps only and the
        // glitch back-off never re-opens the device, so a device start during
        // the soak stays a finding. Off after the open would keep the size the
        // host had already asked for.
        host.setAutomaticBufferSize (false);
        if (isVirtual)
        {
            host.getDeviceManager().addAudioDeviceType (std::make_unique<SoakVirtualDeviceType>());
            host.setDeviceWatcher (nullptr); // no OS device events
        }
    };
    if (isVirtual)
        o.outputEndpoints = [] { return std::vector<flub::platform::OutputEndpointIdentity>(); }; // no OS endpoints
    return o;
}

namespace
{
juce::AudioIODeviceType* findType (juce::AudioDeviceManager& manager, const juce::String& name)
{
    for (auto* type : manager.getAvailableDeviceTypes())
        if (type->getTypeName() == name)
            return type;
    return nullptr;
}
} // namespace

bool DeviceSoak::resolveDevice (DeviceSoakOptions& options, juce::String& error, juce::AudioDeviceManager* given)
{
    std::unique_ptr<juce::AudioDeviceManager> own;
    if (given == nullptr)
        own = std::make_unique<juce::AudioDeviceManager>();
    auto& manager = given != nullptr ? *given : *own;
    const auto& types = manager.getAvailableDeviceTypes();
    if (types.isEmpty())
    {
        error = "no audio device types are available";
        return false;
    }
    if (options.type.isEmpty())
    {
        options.type = defaultDeviceType();
        if (options.type.isEmpty() || findType (manager, options.type) == nullptr)
            options.type = types.getFirst()->getTypeName();
    }
    auto* type = findType (manager, options.type);
    if (type == nullptr)
    {
        juce::StringArray names;
        for (auto* t : types)
            names.add (t->getTypeName());
        error = "no device type \"" + options.type + "\" (available: " + names.joinIntoString (", ") + ")";
        return false;
    }
    type->scanForDevices();
    const auto outputs = type->getDeviceNames (false);
    if (! outputs.contains (options.device))
    {
        error = "\"" + options.device + "\" is not an output of \"" + options.type + "\"; nothing was opened (outputs: "
                + outputs.joinIntoString (" | ") + ")";
        return false;
    }
    // The system default output is what people listen to; the soak's
    // programme reaches full scale (the 7.1 fold over 0 dBFS, strip gain up
    // to +6 dB, protection cycled through Off).
    if (const int defaultIndex = type->getDefaultDeviceIndex (false);
        ! options.allowAudible && defaultIndex >= 0 && defaultIndex < outputs.size() && outputs[defaultIndex] == options.device)
    {
        error = "\"" + options.device + "\" is the system default output of \"" + options.type
                + "\", likely the one you listen to; the soak plays a game scene and music up to full scale on it. Nothing was "
                  "opened. Pick an output nothing is connected to (see --list), or pass --allow-audible";
        return false;
    }
    if (options.smallestBuffer)
    {
        // The device object only (no stream is opened), for its buffer sizes.
        std::unique_ptr<juce::AudioIODevice> device (type->createDevice (options.device, {}));
        if (device == nullptr)
        {
            error = "\"" + options.device + "\" could not be inspected for its buffer sizes";
            return false;
        }
        const auto sizes = device->getAvailableBufferSizes();
        if (sizes.isEmpty())
        {
            error = "\"" + options.device + "\" lists no buffer sizes";
            return false;
        }
        options.bufferSize = *std::min_element (sizes.begin(), sizes.end());
    }
    return true;
}

juce::String DeviceSoak::listDevices (const DeviceSoakOptions& options)
{
    juce::String text;
    juce::AudioDeviceManager manager;
    for (auto* type : manager.getAvailableDeviceTypes())
    {
        type->scanForDevices();
        text << type->getTypeName() << "\n";
        const auto outputs = type->getDeviceNames (false);
        const int defaultIndex = type->getDefaultDeviceIndex (false);
        for (int i = 0; i < outputs.size(); ++i)
        {
            text << "  " << outputs[i] << (i == defaultIndex ? "   (system default)" : "") << "\n";
            if (outputs[i] == options.device && (options.type.isEmpty() || options.type == type->getTypeName()))
            {
                // The device object only: no stream is opened.
                std::unique_ptr<juce::AudioIODevice> device (type->createDevice (outputs[i], {}));
                if (device != nullptr)
                {
                    juce::StringArray sizes, rates;
                    for (const auto s : device->getAvailableBufferSizes())
                        sizes.add (juce::String (s));
                    for (const auto r : device->getAvailableSampleRates())
                        rates.add (juce::String (r, 0));
                    text << "      buffer sizes: " << sizes.joinIntoString (" ") << " (default " << device->getDefaultBufferSize() << ")\n";
                    text << "      sample rates: " << rates.joinIntoString (" ") << "\n";
                }
            }
        }
    }
    if (options.device.isNotEmpty())
    {
        // What a soak with these options would say (nothing is opened).
        auto check = options;
        check.smallestBuffer = false;
        juce::String error;
        text << "--device \"" << options.device << "\": "
             << (resolveDevice (check, error) ? "a soak would run on it [" + check.type + "]" : "a soak would refuse it: " + error) << "\n";
    }
    return text;
}

bool DeviceSoak::readReplay (const juce::File& reportFile, DeviceSoakOptions& options, std::vector<SoakAction>& logged, juce::String& error)
{
    const auto json = juce::JSON::parse (reportFile.loadFileAsString());
    if (! json.isObject() || static_cast<int> (json.getProperty ("flubsoundDeviceSoak", 0)) < 1)
    {
        error = reportFile.getFullPathName() + " is not a device-soak report";
        return false;
    }
    const auto config = json.getProperty ("config", {});
    const auto device = json.getProperty ("device", {});
    options.device = config.getProperty ("device", "").toString();
    options.type = config.getProperty ("type", "").toString();
    options.bufferSize = static_cast<int> (device.getProperty ("bufferSize", 0));
    options.sampleRate = static_cast<double> (device.getProperty ("sampleRate", 48000.0));
    options.minutes = static_cast<double> (json.getProperty ("framesAnalysed", 0)) / std::max (1.0, options.sampleRate) / 60.0;
    if (auto profile = profileFromKey (config.getProperty ("profile", "balanced").toString()))
        options.profile = *profile;
    options.seed = static_cast<uint32_t> (static_cast<juce::int64> (config.getProperty ("seed", 1)));
    options.intervalMs = static_cast<double> (config.getProperty ("intervalMs", 2000.0));
    options.automation = static_cast<bool> (config.getProperty ("automation", true));
    options.warmupSeconds = static_cast<double> (config.getProperty ("warmupSeconds", 5.0));
    options.injectPulseAtSeconds = static_cast<double> (config.getProperty ("injectPulseAtSeconds", -1.0)); // tests
    if (options.bufferSize <= 0 || options.minutes <= 0.0)
    {
        error = reportFile.getFullPathName() + " has no device format or no frames to replay";
        return false;
    }

    // Each action at its logged frame, relative to the programme's start.
    // A report this tool wrote names only strips and parameters that exist;
    // anything else is a damaged or hand-made file and is refused as a
    // whole (a replay that skipped actions would not be the session).
    const int64_t programmeStart = static_cast<juce::int64> (json.getProperty ("programmeStartFrame", 0));
    const auto isAuditionModule = [] (int id) { return std::find (std::begin (kAuditionModules), std::end (kAuditionModules), id) != std::end (kAuditionModules); };
    logged.clear();
    if (const auto* list = json.getProperty ("actions", {}).getArray())
        for (int i = 0; i < list->size(); ++i)
        {
            const auto& a = list->getReference (i);
            const auto kind = SoakAction::kindFromName (a.getProperty ("kind", "").toString());
            if (! kind)
                continue;
            SoakAction action;
            action.kind = *kind;
            action.frame = static_cast<juce::int64> (a.getProperty ("appliedFrame", 0)) - programmeStart;
            action.strip = static_cast<int> (a.getProperty ("strip", 0));
            action.param = static_cast<int> (a.getProperty ("param", -1));
            action.value = static_cast<float> (static_cast<double> (a.getProperty ("value", 0.0)));
            action.preset = a.getProperty ("preset", "").toString();
            action.presetName = a.getProperty ("presetName", "").toString();

            juce::String problem;
            if (action.strip < 0 || action.strip >= AudioEngineHost::kMaxStrips)
                problem = "strip " + juce::String (action.strip) + " (0 .. " + juce::String (AudioEngineHost::kMaxStrips - 1) + ")";
            else if (! std::isfinite (action.value))
                problem = "a value that is not a number";
            else if (action.kind == SoakAction::Kind::Macro && (action.param < Macro1 || action.param > Macro5))
                problem = "parameter " + juce::String (action.param) + ", not a macro";
            else if (action.kind == SoakAction::Kind::Audition && ! isAuditionModule (action.param))
                problem = "parameter " + juce::String (action.param) + ", not a module's ear";
            else if (action.kind == SoakAction::Kind::Param && (action.param < 0 || action.param >= kNumParams))
                problem = "parameter " + juce::String (action.param) + " (0 .. " + juce::String (kNumParams - 1) + ")";
            if (problem.isNotEmpty())
            {
                error = reportFile.getFullPathName() + ": action " + juce::String (i) + " (" + SoakAction::kindName (action.kind) + ") names "
                        + problem + "; not replayed";
                logged.clear();
                return false;
            }
            logged.push_back (action);
        }
    return true;
}

// =============================================================================
// The driver
// =============================================================================
DeviceSoak::DeviceSoak (EngineController& c, DeviceSoakOptions o, Clock k, Completion done)
    : controller (c), options (std::move (o)), clock (k), onFinished (std::move (done))
{
    heldAudition.fill (-1);
}

DeviceSoak::~DeviceSoak()
{
    stopTimer();
    cancelPendingUpdate();
    // The callback must not touch the source or the tap once they are gone.
    releaseHooks();
}

void DeviceSoak::releaseHooks()
{
    if (! hooksAttached)
        return;
    hooksAttached = false;
    auto& host = controller.getHost();
    // The soak's device ends with it. Closing it first removes the host's
    // callback from the device manager, which waits for a callback in flight
    // (JUCE's callback lock), and the backend stops its thread: no callback
    // can still be using the programme or the tap afterwards, even one that
    // stalled beyond the bounded wait of setDeviceSignalSource / setOutputTap
    // (which would return false then). With the virtual clock the callbacks
    // run on this thread, so none is in flight anyway, and the setters do not
    // wait for one that will not come.
    host.closeDevice();
    const bool sourceReleased = host.setDeviceSignalSource (nullptr);
    const bool tapReleased = host.setOutputTap (nullptr);
    jassert (sourceReleased && tapReleased); // nothing runs once the device is closed
    juce::ignoreUnused (sourceReleased, tapReleased);
}

void DeviceSoak::setScriptedActions (std::vector<SoakAction> list)
{
    scripted = true;
    script = std::move (list);
    std::stable_sort (script.begin(), script.end(), [] (const SoakAction& a, const SoakAction& b) { return a.frame < b.frame; });
    scriptPos = 0;
}

bool DeviceSoak::start (juce::String& error)
{
    auto& host = controller.getHost();
    auto* device = controller.getDeviceManager().getCurrentAudioDevice();
    if (device == nullptr || host.isPinBlocked())
    {
        error = "the output is not open" + (controller.getLastDeviceError().isNotEmpty() ? ": " + controller.getLastDeviceError() : juce::String());
        return false;
    }
    if (clock == Clock::Virtual && dynamic_cast<SoakVirtualDevice*> (device) == nullptr)
    {
        error = "the virtual soak device is not open";
        return false;
    }

    deviceName = device->getName();
    deviceType = device->getTypeName();
    sampleRate = device->getCurrentSampleRate() > 0.0 ? device->getCurrentSampleRate() : 48000.0;
    blockSize = device->getCurrentBufferSizeSamples();
    totalFrames = framesFor (options.minutes * 60.0);

    setUpScene();

    // Analysis first, then the tap, then the source: a callback that sees
    // the source also sees the tap (both seq_cst), so the programme's first
    // frame has a stream position (Programme::getStartFrame).
    analysis = std::make_unique<Analysis>();
    analysis->prepare (sampleRate, blockSize, gameStrip, musicStrip);
    tap.prepare (static_cast<int> (kTapSeconds * sampleRate));
    const int64_t pulse = options.injectPulseAtSeconds >= 0.0 ? framesFor (options.injectPulseAtSeconds) : -1;
    programme = std::make_unique<Programme> (sampleRate, gameStrip, musicStrip, tap, pulse);
    hooksAttached = true;
    host.setOutputTap (&tap);
    host.setDeviceSignalSource (programme.get());

    dumps.clear();
    for (const auto t : options.dumpAt)
    {
        Dump d;
        d.seconds = t;
        dumps.push_back (d);
    }

    rng = flub::FastRandom (options.seed * 2654435761u + 1u);
    bag.clear();
    nextActionFrame = -1;

    const auto status = controller.getStatus();
    timing0 = timingPrev = status.callbackTiming;
    xrunsDevice0 = xrunsDevice = status.xruns;
    glitches0 = glitches = status.glitches;
    startCount0 = seenStarts = host.getDeviceStartCount();
    errorCount0 = host.getDeviceErrorCount();
    swaps0 = host.getCompletedSwaps();
    watchdogEpisodes0 = controller.getOverloadState().episodes;

    startedAt = juce::Time::getCurrentTime();
    wallStartMs = lastStatusMs = lastProcessMs = lastProgressMs = lastFramesChangeMs = nowMs();
    const auto stats = diagnostics::readProcessStats();
    cpu0 = cpuLast = stats.cpuSeconds;
    busy0 = busyLast = stats.systemBusySeconds;
    total0 = totalLast = stats.systemTotalSeconds;
    memory.push_back ({ 0.0, stats.privateBytes / 1048576.0, stats.workingSetBytes / 1048576.0 });

    started = true;
    if (clock == Clock::Device)
        startTimerHz (50);
    else
        triggerAsyncUpdate();
    return true;
}

void DeviceSoak::setUpScene()
{
    const int numStrips = controller.getNumStrips();
    gameStrip = controller.findStrip ("Game");
    for (int i = 0; gameStrip < 0 && i < numStrips; ++i)
        if (controller.getStripChannels (i) >= 6)
            gameStrip = i;
    if (gameStrip < 0)
        gameStrip = 0;
    musicStrip = controller.findStrip ("Music");
    for (int i = 0; musicStrip < 0 && i < numStrips; ++i)
        if (i != gameStrip)
            musicStrip = i;
    if (musicStrip < 0)
        musicStrip = gameStrip;

    // The starting profile, then each strip's first-run preset.
    controller.setLatencyProfile (options.profile);
    if (controller.getHost().needsReprepare())
        controller.getHost().reconfigure();

    factoryPresets = controller.getPresetManager().getFactoryPresets();
    const auto load = [this] (int strip, const char* name, ModeValue mode)
    {
        juce::String error;
        if (const auto* preset = controller.getPresetManager().findByName (name))
            controller.loadPreset (*preset, strip, error);
        controller.setMode (mode, strip);
        controller.setBoost (0.5f, strip);
    };
    load (gameStrip, "Competitive FPS", ModeValue::Gaming);
    if (musicStrip != gameStrip)
        load (musicStrip, "Signature", ModeValue::Music);
    controller.setSelectedStrip (musicStrip);
    controller.takePresetWarnings();
    controller.takeLatencySuggestion();
}

void DeviceSoak::stop (const juce::String& reason)
{
    if (! finished)
        finish (reason);
}

void DeviceSoak::timerCallback()
{
    if (finished)
        return;
    auto& host = controller.getHost();

    drainTap();
    const int64_t position = tap.framesWritten();
    applyDueActions (position);
    sampleHeadroom();
    sampleStatus (false);

    const double now = nowMs();
    if (position != lastFramesSeen)
    {
        lastFramesSeen = position;
        lastFramesChangeMs = now;
    }

    if (host.getPinViolations() > 0)
        return finish ("aborted: a device other than the pinned output started (silenced and closed)");
    if (now - lastFramesChangeMs > kStallSeconds * 1000.0)
        return finish ("aborted: the device delivered no callbacks for " + juce::String (kStallSeconds, 0) + " s");
    if (now - wallStartMs > (options.minutes * 60.0 * 1.5 + 120.0) * 1000.0)
        return finish ("aborted: the run took 1.5 x its length");
    if (analysis->frames >= totalFrames)
        return finish ({});

    if (now - lastProgressMs >= 60000.0)
    {
        lastProgressMs = now;
        std::printf ("device soak: %.0f / %.0f s, %lld detections, %llu late callbacks\n", secondsAt (analysis->frames),
                     options.minutes * 60.0, static_cast<long long> (analysis->total()),
                     static_cast<unsigned long long> (controller.getStatus().callbackTiming.since (timing0).late));
        std::fflush (stdout);
    }
}

void DeviceSoak::handleAsyncUpdate()
{
    if (! finished && clock == Clock::Virtual)
        pumpVirtual();
}

void DeviceSoak::pumpVirtual()
{
    auto* device = dynamic_cast<SoakVirtualDevice*> (controller.getDeviceManager().getCurrentAudioDevice());
    if (device == nullptr)
        return finish ("aborted: the virtual device closed");

    // A step of blocks, ended early after an action so that what it posts
    // (an engine swap's onEngineConfigured) runs before the next step, as it
    // would between two real callbacks.
    const int64_t framesBefore = analysis->frames;
    bool running = true;
    for (int b = 0; b < kVirtualBlocksPerStep; ++b)
    {
        const int64_t position = tap.framesWritten();
        if (position >= totalFrames + framesFor (0.5))
            break;
        const size_t before = actions.size();
        applyDueActions (position);
        running = device->process();
        if (! running)
            break; // stopped: no callback ran
        sampleHeadroom();
        drainTap();
        if (actions.size() != before)
            break;
    }
    sampleStatus (false);

    if (controller.getHost().getPinViolations() > 0)
        return finish ("aborted: a device other than the pinned output started (silenced and closed)");
    if (analysis->frames >= totalFrames)
        return finish ({});
    // The virtual clock's stall guards (the device clock has its own): a
    // stopped device, or steps that bring no new frame to the analysis.
    if (! running)
        return finish ("aborted: the virtual device is not running");
    virtualIdleSteps = analysis->frames > framesBefore ? 0 : virtualIdleSteps + 1;
    if (virtualIdleSteps >= kVirtualIdleStepsLimit)
        return finish ("aborted: the virtual device delivered no frames for " + juce::String (kVirtualIdleStepsLimit) + " steps");
    triggerAsyncUpdate();
}

void DeviceSoak::drainTap()
{
    flub::StreamTap::Chunk chunk;
    while (analysis->frames < totalFrames && tap.read (chunk))
    {
        if (analysis->dryStart < 0)
            analysis->dryStart = programme->getStartFrame();
        if (analysis->frames + chunk.numFrames > totalFrames)
            chunk.numFrames = static_cast<int32_t> (totalFrames - analysis->frames);
        analysis->process (chunk);
        serviceDumps();
    }
}

void DeviceSoak::serviceDumps()
{
    // --dump: the device output around a programme time (a WAV for
    // `flubsound-cli analyze --glitches`), and both strips' states (both
    // banks) as the stream passed it.
    const int64_t origin = programme != nullptr ? programme->getStartFrame() : -1;
    if (origin < 0 || options.report == juce::File())
        return;
    const int64_t half = framesFor (0.5);
    for (auto& d : dumps)
    {
        if (d.written)
            continue;
        const int64_t centre = origin + framesFor (d.seconds);
        if (d.states.isEmpty() && analysis->historyEnd >= centre)
            for (const int strip : { gameStrip, musicStrip })
                d.states << "\"" << controller.getStripName (strip) << "\": " << controller.getPersistedStripState (strip) << ",\n";
        if (analysis->historyEnd < centre + half)
            continue;
        d.written = true;
        // The frames kept: from the tap's first frame (never before the
        // stream's start: a time less than 0.5 s into it gives a shorter
        // file) and within the history ring.
        const auto size = static_cast<int64_t> (analysis->historyLeft.size());
        const int64_t from = std::max ({ centre - half, analysis->historyEnd - size + 1, std::max<int64_t> (0, analysis->firstFrame) });
        const int64_t to = centre + half;
        flub::io::AudioFileData wav;
        wav.sampleRate = sampleRate;
        wav.numChannels = 2;
        wav.channels.assign (2, {});
        for (int64_t f = from; f < to; ++f)
        {
            const auto h = static_cast<size_t> (f % size); // f >= 0
            wav.channels[0].push_back (analysis->historyLeft[h]);
            wav.channels[1].push_back (analysis->historyRight[h]);
        }
        const auto stem = options.report.getFileNameWithoutExtension() + "-dump-" + juce::String (d.seconds, 3);
        d.file = stem + ".wav";
        d.fromSeconds = secondsAt (from - origin);
        d.toSeconds = secondsAt (to - origin);
        std::string error;
        flub::io::writeWav (options.report.getSiblingFile (d.file).getFullPathName().toStdString(), wav, flub::io::SampleFormat::Float32, error);
        options.report.getSiblingFile (stem + "-strips.json").replaceWithText ("{\n" + d.states.dropLastCharacters (2) + "\n}\n");
    }
}

// ---- Automation ------------------------------------------------------------------
void DeviceSoak::scheduleNext (int64_t after)
{
    const double ms = options.intervalMs * (1.0 + 0.5 * static_cast<double> (rng.nextBipolar()));
    nextActionFrame = after + std::max<int64_t> (1, framesFor (ms * 0.001));
}

void DeviceSoak::applyDueActions (int64_t position)
{
    const int64_t origin = programme != nullptr ? programme->getStartFrame() : -1;
    if (origin < 0)
        return; // the programme has not started: actions count from its first frame
    const int64_t programmeFrame = position - origin;
    const int64_t end = totalFrames - std::min (framesFor (2.0), totalFrames / 10); // the end stays still (2 s, a tenth of a short run)

    if (scripted)
    {
        while (scriptPos < script.size() && script[scriptPos].frame <= programmeFrame)
        {
            auto action = script[scriptPos++];
            action.frame += origin;
            action.appliedFrame = position;
            apply (action);
            actions.push_back (action);
        }
        return;
    }

    if (! options.automation)
        return;
    if (nextActionFrame < 0)
        nextActionFrame = origin + framesFor (options.warmupSeconds);
    if (position < nextActionFrame || position >= end)
        return;

    auto action = nextAction (nextActionFrame);
    action.appliedFrame = position;
    apply (action);
    actions.push_back (action);
    scheduleNext (nextActionFrame);
}

SoakAction DeviceSoak::nextAction (int64_t frame)
{
    if (bag.empty())
    {
        for (const auto kind : kBag)
            bag.push_back (static_cast<int> (kind));
        for (size_t i = bag.size() - 1; i > 0; --i) // seeded shuffle
            std::swap (bag[i], bag[static_cast<size_t> (uniformInt (rng, static_cast<int> (i + 1)))]);
    }
    const auto kind = static_cast<SoakAction::Kind> (bag.back());
    bag.pop_back();

    SoakAction a;
    a.kind = kind;
    a.frame = frame;
    a.strip = rng.nextU32() & 1u ? musicStrip : gameStrip;
    const int s = a.strip;
    switch (kind)
    {
        case SoakAction::Kind::Preset:
        {
            if (! factoryPresets.empty())
            {
                const auto& p = factoryPresets[static_cast<size_t> (uniformInt (rng, static_cast<int> (factoryPresets.size())))];
                a.preset = p.id;
                a.presetName = p.name;
            }
            break;
        }
        case SoakAction::Kind::Boost: a.value = uniform01 (rng); break;
        case SoakAction::Kind::Macro:
            a.param = Macro1 + uniformInt (rng, 5);
            a.value = uniform01 (rng);
            break;
        case SoakAction::Kind::Bypass: a.value = controller.isEnabled() ? 1.0f : 0.0f; break;
        case SoakAction::Kind::StripBypass: a.value = controller.isStripBypassed (s) ? 0.0f : 1.0f; break;
        case SoakAction::Kind::Bank: a.value = controller.getActiveBank (s) == Bank::A ? 1.0f : 0.0f; break;
        case SoakAction::Kind::Mute: a.value = controller.isStripMuted (s) ? 0.0f : 1.0f; break;
        case SoakAction::Kind::Gain: a.value = std::round (-12.0f + 18.0f * uniform01 (rng)); break;
        case SoakAction::Kind::Profile:
        {
            const int current = static_cast<int> (controller.getLatencyProfile());
            a.value = static_cast<float> ((current + 1 + uniformInt (rng, 2)) % 3);
            break;
        }
        case SoakAction::Kind::Mode:
            a.value = controller.getMode (s) == ModeValue::Music ? static_cast<float> (ModeValue::Gaming) : static_cast<float> (ModeValue::Music);
            break;
        case SoakAction::Kind::Audition:
        {
            const int held = heldAudition[static_cast<size_t> (s)];
            if (held >= 0)
            {
                a.param = held;
                a.value = 0.0f;
            }
            else
            {
                a.param = kAuditionModules[uniformInt (rng, static_cast<int> (std::size (kAuditionModules)))];
                a.value = 1.0f;
            }
            break;
        }
        case SoakAction::Kind::Night: a.value = controller.isNight (s) ? 0.0f : 1.0f; break;
        case SoakAction::Kind::Focus: a.value = controller.isFocused (s) ? 0.0f : 1.0f; break;
        case SoakAction::Kind::Protection:
            a.value = static_cast<float> ((static_cast<int> (controller.getProtectionStrength()) + 1) % 3);
            break;
        case SoakAction::Kind::Smart: a.value = controller.getSmartMacros (s) ? 0.0f : 1.0f; break;
        case SoakAction::Kind::Param: break; // never drawn
    }
    return a;
}

void DeviceSoak::apply (SoakAction& a)
{
    // The automation draws the Game or the Music strip, and readReplay
    // refuses strips past kMaxStrips; a scripted strip this layout lacks is
    // skipped (it is still logged, as given).
    const int s = a.strip;
    if (s < 0 || s >= controller.getNumStrips() || ! std::isfinite (a.value))
        return;
    const bool on = a.value >= 0.5f;
    switch (a.kind)
    {
        case SoakAction::Kind::Preset:
        {
            juce::String error;
            if (a.preset.isNotEmpty())
                controller.loadPreset (a.preset, s, error);
            controller.takePresetWarnings();
            controller.takeLatencySuggestion();
            break;
        }
        case SoakAction::Kind::Boost: controller.setBoost (a.value, s); break;
        case SoakAction::Kind::Macro:
            if (a.param >= Macro1 && a.param <= Macro5)
                controller.getParams (s).set (a.param, a.value);
            break;
        case SoakAction::Kind::Bypass: controller.setEnabled (! on); break;
        case SoakAction::Kind::StripBypass: controller.setStripBypassed (s, on); break;
        case SoakAction::Kind::Bank: controller.setActiveBank (on ? Bank::B : Bank::A, s); break;
        case SoakAction::Kind::Mute: controller.setStripMuted (s, on); break;
        case SoakAction::Kind::Gain: controller.setStripGainDb (s, a.value); break;
        case SoakAction::Kind::Profile:
            controller.setLatencyProfile (static_cast<LatencyProfileValue> (std::clamp (juce::roundToInt (a.value), 0, 2)));
            // The host's 5 Hz poll would start the swap within 200 ms; now, so a replay starts it at the same frame.
            if (controller.getHost().needsReprepare())
                controller.getHost().reconfigure();
            break;
        case SoakAction::Kind::Mode: controller.setMode (static_cast<ModeValue> (std::clamp (juce::roundToInt (a.value), 0, 1)), s); break;
        case SoakAction::Kind::Audition:
            if (a.param >= 0 && a.param < kNumParams && static_cast<size_t> (s) < heldAudition.size())
            {
                controller.setAuditionBypass (s, a.param, on);
                heldAudition[static_cast<size_t> (s)] = on ? a.param : -1;
            }
            break;
        case SoakAction::Kind::Night: controller.setNight (s, on); break;
        case SoakAction::Kind::Focus: controller.setFocus (s, on); break;
        case SoakAction::Kind::Protection:
            controller.setProtectionStrength (static_cast<flub::ProtectionStrength> (std::clamp (juce::roundToInt (a.value), 0, 2)));
            break;
        case SoakAction::Kind::Smart: controller.setSmartMacros (on, s); break;
        case SoakAction::Kind::Param:
            if (a.param >= 0 && a.param < kNumParams && a.param != LatencyProfile)
                controller.getParams (s).set (a.param, a.value);
            break;
    }
}

// ---- Status ------------------------------------------------------------------------
void DeviceSoak::sampleHeadroom()
{
    // MeterBus::foldHeadroomDb: the deepest fold-headroom gain of the chain's
    // last host block (docs/11 E28a). Read after every block with the
    // virtual clock, at the timer's rate with a device.
    for (const int strip : { gameStrip, musicStrip })
    {
        if (strip >= controller.getNumStrips())
            continue;
        const float db = controller.getChain (strip).meters().foldHeadroomDb.load (std::memory_order_relaxed);
        if (db < -0.01f)
        {
            ++headroomBlocks;
            headroomMinDb = std::min (headroomMinDb, static_cast<double> (db));
            if (headroomEvents.size() < 2000)
                headroomEvents.push_back ({ tap.framesWritten(), strip, static_cast<double> (db) });
        }
        if (musicStrip == gameStrip)
            break;
    }
}

void DeviceSoak::sampleStatus (bool force)
{
    const double now = nowMs();
    auto& host = controller.getHost();

    // Device (re)starts and errors as they happen.
    if (const auto starts = host.getDeviceStartCount(); starts != seenStarts)
    {
        for (uint32_t i = seenStarts; i < starts; ++i)
            deviceStartFrames.push_back (tap.framesWritten());
        seenStarts = starts;
    }
    if (const auto error = host.getLastDeviceError(); error.isNotEmpty() && error != lastErrorSeen)
    {
        lastErrorSeen = error;
        deviceErrors.add (juce::String (secondsAt (tap.framesWritten()), 1) + " s: " + error);
    }

    if (! force && now - lastStatusMs < kStatusIntervalMs)
        return;
    lastStatusMs = now;

    const auto status = controller.getStatus();
    const auto window = status.callbackTiming.since (timingPrev);
    timingPrev = status.callbackTiming;
    const double at = secondsAt (tap.framesWritten());
    if (window.callbacks > 0)
    {
        const double load = window.loadAt (1.0);
        if (load > worstWindowLoad)
        {
            worstWindowLoad = load;
            worstWindowSeconds = at;
        }
        const auto total = status.callbackTiming.since (timing0);
        if (static_cast<int64_t> (total.late) > lastLate && lateTimes.size() < static_cast<size_t> (kMaxListedTimes))
            lateTimes.emplace_back (at, window.interval.maxNs * 1.0e-6);
        if (static_cast<int64_t> (total.overBudget) > lastOverBudget && overBudgetTimes.size() < static_cast<size_t> (kMaxListedTimes))
            overBudgetTimes.emplace_back (at, load);
        lastLate = static_cast<int64_t> (total.late);
        lastOverBudget = static_cast<int64_t> (total.overBudget);
    }
    if (status.deviceOpen)
    {
        cpuSum += status.cpuLoad;
        cpuMax = std::max (cpuMax, status.cpuLoad);
        ++cpuSamples;
        xrunsDevice = status.xruns;
        glitches = status.glitches;
    }

    if (force || now - lastProcessMs >= kProcessIntervalMs)
    {
        lastProcessMs = now;
        sampleProcess ((now - wallStartMs) * 0.001);
    }
}

void DeviceSoak::sampleProcess (double elapsedSeconds)
{
    const auto stats = diagnostics::readProcessStats();
    cpuLast = stats.cpuSeconds;
    busyLast = stats.systemBusySeconds;
    totalLast = stats.systemTotalSeconds;
    memory.push_back ({ elapsedSeconds, stats.privateBytes / 1048576.0, stats.workingSetBytes / 1048576.0 });
}

// ---- Triage --------------------------------------------------------------------------
juce::String DeviceSoak::classify (int64_t frame, flub::DiscontinuityType type, juce::String& lastAction, double& ageMs, bool& bypassed) const
{
    // The master bypass at that moment, and the last action before it.
    bypassed = false;
    lastAction = {};
    ageMs = -1.0;
    for (const auto& a : actions)
    {
        if (a.appliedFrame > frame)
            break;
        lastAction = a.describe (controller);
        ageMs = 1000.0 * secondsAt (frame - a.appliedFrame);
        if (a.kind == SoakAction::Kind::Bypass)
            bypassed = a.value >= 0.5f;
    }

    for (const auto restartFrame : deviceStartFrames)
        if (frame >= restartFrame - framesFor (0.05) && frame <= restartFrame + framesFor (kRestartWindowSeconds))
            return "restart";
    for (const auto& gap : analysis->gaps)
        if (std::abs (frame - gap.first) <= framesFor (0.01))
            return "gap";
    // The fold's zero-latency headroom limiter (docs/11 E28a) dropped its
    // gain at once in the block before (an over of the 7.1 fold).
    for (const auto& h : headroomEvents)
        if (frame >= h.frame - blockSize - framesFor (0.005) && frame <= h.frame + framesFor (0.05) + blockSize)
            return "headroom";
    if (ageMs >= 0.0 && ageMs <= kTransitionMs)
        return "transition";
    // The dry programme breaks there too: up to the engine's latency
    // (Quality: about 30 ms) and a block later, or a little earlier. A DC
    // step is reported up to its window (250 ms) after it moved, so the
    // processed and the dry one may land that far apart.
    const int64_t early = framesFor (0.005) + blockSize, late = framesFor (0.05) + blockSize;
    const int64_t dcWindow = framesFor (0.001 * analysis->settings.dcStepWindowMs) + late;
    if (type == flub::DiscontinuityType::DcStep)
    {
        for (const auto& e : analysis->dryNearDetector.events())
            if (e.type == flub::DiscontinuityType::DcStep && std::abs (frame - analysis->dryToStream (e.frame)) <= dcWindow)
                return "programme";
        return "static";
    }
    for (const auto& e : analysis->dryDetector.events())
    {
        const int64_t at = analysis->dryToStream (e.frame);
        if (e.type == type && frame >= at - early && frame <= at + late)
            return "programme";
    }
    return "static";
}

// ---- The end ---------------------------------------------------------------------------
void DeviceSoak::finish (const juce::String& reason)
{
    if (finished)
        return;
    finished = true;
    stopTimer();
    cancelPendingUpdate();

    // What is in the tap still belongs to the run.
    if (analysis != nullptr)
        drainTap();
    sampleStatus (true);
    if (analysis != nullptr)
        analysis->finish();

    // The report reads the device and the engine as they ran (latency,
    // counters), so it is built while the device is still open; it is
    // written before the device is closed, so a close that has to wait for
    // a stalled callback does not cost the report.
    buildReport (reason);
    if (options.report != juce::File())
    {
        options.report.getParentDirectory().createDirectory();
        options.report.replaceWithText (juce::JSON::toString (report, false));
        options.report.withFileExtension ("txt").replaceWithText (summary);
    }

    releaseHooks(); // closes the device, then detaches the programme and the tap

    if (onFinished)
        onFinished (exitCode, summary);
}

void DeviceSoak::buildReport (const juce::String& reason)
{
    auto* root = new juce::DynamicObject();
    report = juce::var (root);
    const auto obj = [] { return new juce::DynamicObject(); };

    const bool aborted = reason.isNotEmpty();
    const auto status = controller.getStatus();
    const auto timing = status.callbackTiming.since (timing0);
    const double periodMs = blockSize > 0 ? 1000.0 * blockSize / sampleRate : 0.0;
    const int64_t frames = analysis != nullptr ? analysis->frames : 0;
    const uint32_t starts = controller.getHost().getDeviceStartCount() - startCount0;
    const uint32_t errors = controller.getHost().getDeviceErrorCount() - errorCount0;
    const int64_t tapDropped = tap.framesDropped();
    const uint32_t swaps = controller.getHost().getCompletedSwaps() - swaps0;
    const auto watchdog = controller.getOverloadState();

    root->setProperty ("flubsoundDeviceSoak", kReportVersion);
    root->setProperty ("clock", clock == Clock::Device ? "device" : "virtual");
    root->setProperty ("started", startedAt.toISO8601 (true));
    root->setProperty ("ended", juce::Time::getCurrentTime().toISO8601 (true));

    auto* config = obj();
    config->setProperty ("device", options.device.isNotEmpty() ? options.device : deviceName);
    config->setProperty ("type", options.type.isNotEmpty() ? options.type : deviceType);
    config->setProperty ("bufferRequested", options.smallestBuffer ? juce::var ("min") : juce::var (options.bufferSize));
    config->setProperty ("rateRequested", options.sampleRate);
    config->setProperty ("minutes", options.minutes);
    config->setProperty ("profile", profileKey (options.profile));
    config->setProperty ("seed", v64 (options.seed));
    config->setProperty ("intervalMs", options.intervalMs);
    config->setProperty ("automation", options.automation && ! scripted);
    config->setProperty ("replayOf", options.replay.getFullPathName());
    config->setProperty ("warmupSeconds", options.warmupSeconds);
    if (options.injectPulseAtSeconds >= 0.0)
        config->setProperty ("injectPulseAtSeconds", options.injectPulseAtSeconds); // tests; --replay repeats it
    config->setProperty ("allowAudible", options.allowAudible);
    config->setProperty ("ui", options.ui);
    config->setProperty ("programme", "TestSignalGenerator: Game71 on " + controller.getStripName (gameStrip) + ", Music on "
                                          + controller.getStripName (musicStrip) + ", each at " + juce::String (kProgrammeGainDb, 0) + " dB");
    root->setProperty ("config", juce::var (config));

    auto* dev = obj();
    dev->setProperty ("name", deviceName);
    dev->setProperty ("type", deviceType);
    dev->setProperty ("sampleRate", sampleRate);
    dev->setProperty ("bufferSize", blockSize);
    dev->setProperty ("periodMs", round3 (periodMs));
    const auto latency = controller.getLatencyInfo();
    dev->setProperty ("outputLatencyMs", round2 (latency.deviceOutputMs));
    dev->setProperty ("engineLatencyMs", round2 (latency.engineMs));
    dev->setProperty ("restarts", static_cast<int> (starts)); // device starts during the soak (the first was before it)
    dev->setProperty ("errors", static_cast<int> (errors));
    juce::Array<juce::var> errorList;
    for (const auto& e : deviceErrors)
        errorList.add (e);
    dev->setProperty ("errorMessages", errorList);
    dev->setProperty ("pinViolations", static_cast<int> (controller.getHost().getPinViolations()));
    dev->setProperty ("audioThread", status.audioThread.describe());
    root->setProperty ("device", juce::var (dev));

    const int64_t programmeStart = programme != nullptr ? programme->getStartFrame() : -1;
    root->setProperty ("result", aborted ? "aborted" : "completed");
    root->setProperty ("reason", reason);
    root->setProperty ("framesAnalysed", v64 (frames));
    root->setProperty ("secondsAnalysed", round2 (secondsAt (frames)));
    root->setProperty ("wallSeconds", round2 ((nowMs() - wallStartMs) * 0.001));
    root->setProperty ("programmeStartFrame", v64 (programmeStart));

    // Callback timing
    auto* t = obj();
    const auto ms = [] (double ns) { return round3 (ns * 1.0e-6); };
    t->setProperty ("callbacks", v64 (timing.callbacks));
    t->setProperty ("durationMeanMs", ms (timing.duration.meanNs()));
    t->setProperty ("durationP50Ms", ms (timing.duration.percentileNs (0.5)));
    t->setProperty ("durationP99Ms", ms (timing.duration.percentileNs (0.99)));
    t->setProperty ("durationP999Ms", ms (timing.duration.percentileNs (0.999)));
    t->setProperty ("durationMaxMs", ms (static_cast<double> (timing.duration.maxNs)));
    t->setProperty ("maxLoad", round3 (timing.loadAt (1.0)));
    t->setProperty ("p999Load", round3 (timing.loadAt (0.999)));
    t->setProperty ("meanLoad", round3 (timing.meanLoad()));
    t->setProperty ("intervalMeanMs", ms (timing.interval.meanNs()));
    t->setProperty ("intervalP999Ms", ms (timing.interval.percentileNs (0.999)));
    t->setProperty ("intervalMaxMs", ms (static_cast<double> (timing.interval.maxNs)));
    t->setProperty ("overBudget", v64 (timing.overBudget));
    t->setProperty ("late", v64 (timing.late));
    t->setProperty ("worstSecondLoad", round3 (worstWindowLoad));
    t->setProperty ("worstSecondAt", round2 (worstWindowSeconds));
    const auto timesToVar = [] (const std::vector<std::pair<double, double>>& list)
    {
        juce::Array<juce::var> a;
        for (const auto& p : list)
            a.add (juce::Array<juce::var> { round2 (p.first), round3 (p.second) });
        return a;
    };
    t->setProperty ("lateAt", timesToVar (lateTimes));             // [seconds, longest interval ms of that second]
    t->setProperty ("overBudgetAt", timesToVar (overBudgetTimes)); // [seconds, peak load of that second]
    t->setProperty ("xrunsDevice", xrunsDevice < 0 ? -1 : xrunsDevice - std::max (0, xrunsDevice0));
    t->setProperty ("glitchesJuce", glitches - glitches0);
    t->setProperty ("cpuLoadMean", cpuSamples > 0 ? round3 (cpuSum / cpuSamples) : 0.0);
    t->setProperty ("cpuLoadMax", round3 (cpuMax));
    const double wall = std::max (1.0e-3, (nowMs() - wallStartMs) * 0.001);
    t->setProperty ("processCpuPercent", cpu0 >= 0.0 && cpuLast >= 0.0 ? round2 (100.0 * (cpuLast - cpu0) / wall) : -1.0);
    t->setProperty ("systemCpuPercent",
                    busy0 >= 0.0 && totalLast > total0 ? round2 (100.0 * (busyLast - busy0) / (totalLast - total0)) : -1.0);
    t->setProperty ("overloadEpisodes", v64 (watchdog.episodes - watchdogEpisodes0));
    // How late and how long: callbacks whose interval / duration was at least
    // so many periods (whole histogram buckets above the mark: at least
    // these many; a bucket is 1/8 octave).
    const auto atLeast = [] (const flub::TimingHistogram::Snapshot& h, double ns)
    {
        juce::int64 n = 0;
        for (int b = 0; b < flub::TimingHistogram::kNumBuckets; ++b)
            if (static_cast<double> (flub::TimingHistogram::bucketLowerNs (b)) >= ns)
                n += static_cast<juce::int64> (h.counts[static_cast<size_t> (b)]);
        return n;
    };
    const double periodNs = periodMs * 1.0e6;
    auto* lateBy = obj();
    for (const double x : { 1.5, 2.0, 3.0, 5.0, 10.0 })
        lateBy->setProperty (juce::String (x, 1) + "x", atLeast (timing.interval, x * periodNs));
    t->setProperty ("intervalsAtLeast", juce::var (lateBy)); // periods -> callbacks
    auto* longBy = obj();
    for (const double x : { 0.5, 0.75, 1.0 })
        longBy->setProperty (juce::String (x, 2) + "x", atLeast (timing.duration, x * periodNs));
    t->setProperty ("durationsAtLeast", juce::var (longBy));
    root->setProperty ("timing", juce::var (t));

    auto* eng = obj();
    eng->setProperty ("swapsCompleted", static_cast<int> (swaps));
    eng->setProperty ("profileAtEnd", profileKey (controller.getLatencyProfile()));
    root->setProperty ("engine", juce::var (eng));

    // Automation
    auto* aut = obj();
    std::map<juce::String, int> perKind;
    juce::Array<juce::var> actionLog;
    for (const auto& a : actions)
    {
        ++perKind[SoakAction::kindName (a.kind)];
        auto* e = obj();
        e->setProperty ("frame", v64 (a.frame));
        e->setProperty ("appliedFrame", v64 (a.appliedFrame));
        e->setProperty ("seconds", round3 (secondsAt (a.appliedFrame)));
        e->setProperty ("kind", SoakAction::kindName (a.kind));
        e->setProperty ("strip", a.strip);
        e->setProperty ("param", a.param);
        e->setProperty ("value", static_cast<double> (a.value));
        if (a.preset.isNotEmpty())
        {
            e->setProperty ("preset", a.preset);
            e->setProperty ("presetName", a.presetName);
        }
        e->setProperty ("text", a.describe (controller));
        actionLog.add (juce::var (e));
    }
    auto* kinds = obj();
    for (int k = 0; k < SoakAction::kNumKinds; ++k)
    {
        const auto* name = SoakAction::kindName (static_cast<SoakAction::Kind> (k));
        kinds->setProperty (name, perKind.count (name) > 0 ? perKind[name] : 0);
    }
    aut->setProperty ("actions", static_cast<int> (actions.size()));
    aut->setProperty ("perKind", juce::var (kinds));
    root->setProperty ("automation", juce::var (aut));
    root->setProperty ("actions", actionLog);

    // Discontinuities
    auto* dis = obj();
    std::map<juce::String, int> perClass;
    juce::Array<juce::var> found;
    juce::String findingsText;
    int64_t detections = 0;
    if (analysis != nullptr)
    {
        detections = analysis->total();
        for (int k = 0; k < flub::kNumDiscontinuityTypes; ++k)
            dis->setProperty (juce::String (flub::discontinuityName (static_cast<flub::DiscontinuityType> (k))),
                              v64 (analysis->counts[static_cast<size_t> (k)]));
        dis->setProperty ("kinks", v64 (analysis->kinks));
        dis->setProperty ("recurring", v64 (analysis->recurring));
        dis->setProperty ("tapGaps", static_cast<int> (analysis->gaps.size()));
        dis->setProperty ("tapFramesDropped", v64 (tapDropped));
        dis->setProperty ("outputPeakDbfs", round2 (20.0 * std::log10 (std::max (analysis->peak, 1.0e-9))));
        dis->setProperty ("outputRmsDbfs",
                          round2 (10.0 * std::log10 (std::max (analysis->sumSquares / std::max<double> (1.0, static_cast<double> (frames)), 1.0e-18))));
        auto* dry = obj();
        for (int k = 0; k < flub::kNumDiscontinuityTypes; ++k)
            dry->setProperty (juce::String (flub::discontinuityName (static_cast<flub::DiscontinuityType> (k))),
                              v64 (analysis->dryDetector.count (static_cast<flub::DiscontinuityType> (k))));
        dry->setProperty ("aligned", starts == 0);
        juce::Array<juce::var> dryEvents;
        for (const auto& e : analysis->dryDetector.events())
        {
            if (dryEvents.size() >= 200)
                break;
            auto* f = obj();
            f->setProperty ("seconds", round3 (secondsAt (analysis->dryToStream (e.frame))));
            f->setProperty ("type", typeName (e.type));
            f->setProperty ("channel", e.channel);
            f->setProperty ("levelDb", round2 (e.levelDb));
            dryEvents.add (juce::var (f));
        }
        dry->setProperty ("events", dryEvents);
        dis->setProperty ("dryProgramme", juce::var (dry));

        int listed = 0;
        for (const auto& e : analysis->events)
        {
            juce::String last;
            double age = -1.0;
            bool bypassed = false;
            const auto cls = classify (e.frame, e.type, last, age, bypassed);
            ++perClass[cls];
            auto* f = obj();
            f->setProperty ("seconds", round3 (secondsAt (e.frame)));
            f->setProperty ("frame", v64 (e.frame));
            f->setProperty ("programmeFrame", v64 (programmeStart >= 0 ? e.frame - programmeStart : e.frame));
            f->setProperty ("type", typeName (e.type));
            f->setProperty ("channel", e.channel);
            f->setProperty ("levelDb", round2 (e.levelDb));
            f->setProperty ("overDb", round2 (e.overDb));
            f->setProperty ("lengthMs", round2 (1000.0 * secondsAt (e.length)));
            f->setProperty ("class", cls);
            f->setProperty ("lastAction", last);
            f->setProperty ("lastActionAgeMs", round2 (age));
            f->setProperty ("bypassed", bypassed);
            found.add (juce::var (f));
            if (++listed <= 60)
                findingsText << "    " << juce::String (secondsAt (e.frame), 3).paddedLeft (' ', 9) << " s  " << typeName (e.type) << " ch"
                             << e.channel << "  " << juce::String (e.levelDb, 1) << " dBFS"
                             << (e.type == flub::DiscontinuityType::Click ? " (+" + juce::String (e.overDb, 1) + " dB)" : juce::String())
                             << "  " << cls << (last.isNotEmpty() ? "  after \"" + last + "\" " + juce::String (age, 0) + " ms before" : juce::String())
                             << (bypassed ? "  [bypassed]" : "") << "\n";
        }
    }
    auto* classes = obj();
    for (const auto* c : { "transition", "static", "programme", "restart", "gap", "headroom" })
        classes->setProperty (c, perClass.count (c) > 0 ? perClass[c] : 0);
    dis->setProperty ("perClass", juce::var (classes));
    dis->setProperty ("events", found);
    root->setProperty ("discontinuities", juce::var (dis));

    auto* head = obj();
    head->setProperty ("blocksSeen", v64 (headroomBlocks));
    head->setProperty ("deepestDb", round2 (headroomMinDb));
    head->setProperty ("everyBlock", clock == Clock::Virtual); // the device clock samples the meter at 50 Hz
    juce::Array<juce::var> headList;
    for (const auto& h : headroomEvents)
        headList.add (juce::Array<juce::var> { round3 (secondsAt (h.frame)), h.strip, round2 (h.db) });
    head->setProperty ("events", headList); // [seconds, strip, gain dB], at most the first 2000
    root->setProperty ("foldHeadroom", juce::var (head));

    // --dump: what was written (a time the run did not reach, or with no
    // --report, is not).
    juce::Array<juce::var> dumpList;
    for (const auto& d : dumps)
    {
        auto* e = obj();
        e->setProperty ("seconds", d.seconds);
        e->setProperty ("written", d.written);
        if (d.written)
        {
            e->setProperty ("wav", d.file);
            e->setProperty ("fromSeconds", round3 (d.fromSeconds)); // programme time
            e->setProperty ("toSeconds", round3 (d.toSeconds));
        }
        dumpList.add (juce::var (e));
    }
    root->setProperty ("dumps", dumpList);

    // Replay: which of the original run's detections came back here.
    juce::String replayText;
    if (replayReference.isObject())
    {
        const int64_t tolerance = blockSize + framesFor (0.003);
        const auto matches = [tolerance] (const juce::var& a, const juce::var& b)
        {
            const int64_t fa = static_cast<juce::int64> (a.getProperty ("programmeFrame", 0));
            const int64_t fb = static_cast<juce::int64> (b.getProperty ("programmeFrame", 0));
            return a.getProperty ("type", "") == b.getProperty ("type", "") && std::abs (fa - fb) <= tolerance;
        };
        juce::Array<juce::var> back, notBack;
        int originals = 0;
        if (const auto* list = replayReference.getProperty ("discontinuities", {}).getProperty ("events", {}).getArray())
            for (const auto& original : *list)
            {
                ++originals;
                const bool again = std::any_of (found.begin(), found.end(), [&] (const juce::var& r) { return matches (original, r); });
                (again ? back : notBack).add (original);
            }
        int replayOnly = 0;
        if (const auto* list = replayReference.getProperty ("discontinuities", {}).getProperty ("events", {}).getArray())
            for (const auto& r : found)
                if (! std::any_of (list->begin(), list->end(), [&] (const juce::var& original) { return matches (original, r); }))
                    ++replayOnly;
        auto* cmp = obj();
        cmp->setProperty ("originalEvents", originals);
        cmp->setProperty ("reproduced", back.size());
        cmp->setProperty ("notReproduced", notBack.size());
        cmp->setProperty ("replayOnly", replayOnly);
        cmp->setProperty ("notReproducedEvents", notBack);
        root->setProperty ("replayComparison", juce::var (cmp));
        cmp->setProperty ("matchedBy", "type and programme frame within " + juce::String (tolerance) + " frames (one block + 3 ms); levels are not compared");
        replayText << "  replay    of " << originals << " detections in the original run, " << back.size() << " came back here (the processing's own), "
                   << notBack.size() << " did not (the real-time path); " << replayOnly << " here only (matched: same type within one block + 3 ms of the same programme frame)\n";
        for (const auto& e : notBack)
            replayText << "    not reproduced: " << e.getProperty ("seconds", 0).toString() << " s " << e.getProperty ("type", "").toString() << " "
                       << e.getProperty ("class", "").toString() << " " << e.getProperty ("lastAction", "").toString() << "\n";
    }

    // Memory
    auto* mem = obj();
    juce::Array<juce::var> samples;
    for (const auto& m : memory)
        samples.add (juce::Array<juce::var> { round2 (m.seconds), round2 (m.privateMB), round2 (m.workingSetMB) });
    double growthAfterWarmup = 0.0, slopePerHour = 0.0;
    if (! memory.empty())
    {
        mem->setProperty ("privateStartMB", round2 (memory.front().privateMB));
        mem->setProperty ("privateEndMB", round2 (memory.back().privateMB));
        mem->setProperty ("workingSetStartMB", round2 (memory.front().workingSetMB));
        mem->setProperty ("workingSetEndMB", round2 (memory.back().workingSetMB));
        double maxPrivate = 0.0;
        for (const auto& m : memory)
            maxPrivate = std::max (maxPrivate, m.privateMB);
        mem->setProperty ("privateMaxMB", round2 (maxPrivate));
        // After the first minute (pools, caches and the analysis' own lists fill early).
        const auto first = std::find_if (memory.begin(), memory.end(), [] (const MemorySample& m) { return m.seconds >= 60.0; });
        if (first != memory.end())
        {
            growthAfterWarmup = memory.back().privateMB - first->privateMB;
            // Least-squares slope over the samples after the first minute.
            double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (auto it = first; it != memory.end(); ++it)
            {
                n += 1;
                sx += it->seconds;
                sy += it->privateMB;
                sxx += it->seconds * it->seconds;
                sxy += it->seconds * it->privateMB;
            }
            const double den = n * sxx - sx * sx;
            if (n >= 3 && den > 0.0)
                slopePerHour = 3600.0 * (n * sxy - sx * sy) / den;
        }
    }
    // The engine's size follows the latency profile the automation picked
    // (each switch builds a new engine), so the private bytes saw-tooth; a
    // leak raises the floor: the lowest value of the last third of the run
    // (after the first minute) against that of the first third.
    double floorGrowth = 0.0;
    {
        std::vector<MemorySample> after;
        for (const auto& m : memory)
            if (m.seconds >= 60.0)
                after.push_back (m);
        if (after.size() >= 6)
        {
            const size_t third = after.size() / 3;
            double firstFloor = std::numeric_limits<double>::max(), lastFloor = std::numeric_limits<double>::max();
            for (size_t i = 0; i < third; ++i)
            {
                firstFloor = std::min (firstFloor, after[i].privateMB);
                lastFloor = std::min (lastFloor, after[after.size() - 1 - i].privateMB);
            }
            floorGrowth = lastFloor - firstFloor;
        }
    }
    mem->setProperty ("privateFloorGrowthMB", round2 (floorGrowth));
    mem->setProperty ("privateGrowthAfterFirstMinuteMB", round2 (growthAfterWarmup));
    mem->setProperty ("privateSlopeMBPerHour", round2 (slopePerHour));
    mem->setProperty ("samples", samples); // [seconds, private MB, working set MB]
    root->setProperty ("memory", juce::var (mem));

    // Verdict
    // A virtual clock runs as fast as it can between message-loop passes:
    // its intervals and loads say nothing about real time.
    const bool timingFindings = clock == Clock::Device
                                && (timing.late > 0 || timing.overBudget > 0 || (xrunsDevice > 0 && xrunsDevice > xrunsDevice0));
    const bool deviceFindings = errors > 0 || starts > 0;
    const bool findings = detections > 0 || timingFindings || deviceFindings || tapDropped > 0;
    exitCode = aborted ? 4 : (findings ? 1 : 0);
    root->setProperty ("verdict", aborted ? "aborted" : (findings ? "findings" : "clean"));
    root->setProperty ("exitCode", exitCode);

    // Summary
    juce::String s;
    s << "Flubsound device soak (" << (clock == Clock::Device ? "real device" : "virtual device") << ")\n";
    s << "  output    " << deviceName << "  [" << deviceType << "]\n";
    s << "  format    " << juce::String (sampleRate, 0) << " Hz, " << blockSize << " samples (" << juce::String (periodMs, 2)
      << " ms), engine " << juce::String (latency.engineMs, 1) << " ms; profile " << profileName (options.profile) << " at start\n";
    s << "  ran       " << juce::String (secondsAt (frames), 1) << " s of " << juce::String (options.minutes * 60.0, 1) << " s (wall "
      << juce::String ((nowMs() - wallStartMs) * 0.001, 1) << " s): " << (aborted ? reason : juce::String ("completed")) << "\n";
    if (clock == Clock::Virtual)
        s << "  (virtual clock: the callback timing below is not real time and not judged)\n";
    s << "  callbacks " << static_cast<juce::int64> (timing.callbacks) << ": duration mean " << juce::String (timing.duration.meanNs() * 1.0e-6, 3)
      << " ms, p99.9 " << juce::String (timing.duration.percentileNs (0.999) * 1.0e-6, 3) << " ms, max "
      << juce::String (static_cast<double> (timing.duration.maxNs) * 1.0e-6, 3) << " ms (" << juce::String (100.0 * timing.loadAt (1.0), 1)
      << " % of the period)\n";
    s << "            interval p99.9 " << juce::String (timing.interval.percentileNs (0.999) * 1.0e-6, 2) << " ms, max "
      << juce::String (static_cast<double> (timing.interval.maxNs) * 1.0e-6, 2) << " ms; late " << static_cast<juce::int64> (timing.late)
      << ", over budget " << static_cast<juce::int64> (timing.overBudget) << "; xruns: device "
      << (xrunsDevice < 0 ? juce::String ("not reported") : juce::String (xrunsDevice - std::max (0, xrunsDevice0))) << ", JUCE "
      << (glitches - glitches0) << "\n";
    s << "            intervals of at least 2 / 3 / 5 / 10 periods: " << atLeast (timing.interval, 2.0 * periodNs) << " / "
      << atLeast (timing.interval, 3.0 * periodNs) << " / " << atLeast (timing.interval, 5.0 * periodNs) << " / "
      << atLeast (timing.interval, 10.0 * periodNs) << "; durations of at least half a period: " << atLeast (timing.duration, 0.5 * periodNs) << "\n";
    s << "  CPU       JUCE load mean " << juce::String (cpuSamples > 0 ? 100.0 * cpuSum / cpuSamples : 0.0, 1) << " %, max "
      << juce::String (100.0 * cpuMax, 1) << " %; process " << juce::String (cpu0 >= 0.0 ? 100.0 * (cpuLast - cpu0) / wall : -1.0, 1)
      << " % of one core; system " << juce::String (busy0 >= 0.0 && totalLast > total0 ? 100.0 * (busyLast - busy0) / (totalLast - total0) : -1.0, 1)
      << " %; overload episodes " << static_cast<juce::int64> (watchdog.episodes - watchdogEpisodes0) << "\n";
    s << "  device    " << static_cast<int> (starts) << " restart(s), " << static_cast<int> (errors) << " error(s); engine swaps " << static_cast<int> (swaps)
      << "; " << status.audioThread.describe() << "\n";
    juce::StringArray kindText;
    for (const auto& [name, count] : perKind)
        kindText.add (name + " " + juce::String (count));
    s << "  actions   " << static_cast<int> (actions.size()) << (scripted ? " (replayed)" : "") << ": " << kindText.joinIntoString (", ") << "\n";
    if (analysis != nullptr)
    {
        s << "  output    " << analysis->counts[0] << " clicks, " << analysis->counts[1] << " dropouts, " << analysis->counts[2] << " non-finite, "
          << analysis->counts[3] << " DC steps (set aside: " << analysis->kinks << " kinks, " << analysis->recurring << " recurring); tap gaps "
          << static_cast<int> (analysis->gaps.size()) << " (" << tapDropped << " frames)\n";
        s << "            classes: transition " << perClass["transition"] << ", static " << perClass["static"] << ", programme "
          << perClass["programme"] << ", restart " << perClass["restart"] << ", gap " << perClass["gap"] << ", headroom "
          << perClass["headroom"] << "\n";
        s << "  headroom  the fold's zero-latency headroom limiter acted in " << headroomBlocks << " blocks seen (deepest "
          << juce::String (headroomMinDb, 2) << " dB)" << (clock == Clock::Device ? " - sampled at 50 Hz, not every block" : "") << "\n";
        s << "  dry       the programme alone: " << analysis->dryDetector.count (flub::DiscontinuityType::Click) << " clicks, "
          << analysis->dryDetector.count (flub::DiscontinuityType::Dropout) << " dropouts, "
          << analysis->dryDetector.count (flub::DiscontinuityType::DcStep) << " DC steps\n";
    }
    if (! memory.empty())
        s << "  memory    private " << juce::String (memory.front().privateMB, 1) << " -> " << juce::String (memory.back().privateMB, 1)
          << " MB (after the first minute " << juce::String (growthAfterWarmup, 2) << " MB, " << juce::String (slopePerHour, 2)
          << " MB/h; floor, last third - first third: " << juce::String (floorGrowth, 2) << " MB), working set " << juce::String (memory.front().workingSetMB, 1) << " -> " << juce::String (memory.back().workingSetMB, 1) << " MB\n";
    s << replayText;
    for (const auto& d : dumps)
        s << "  dump      " << juce::String (d.seconds, 3) << " s: "
          << (d.written ? d.file + " (" + juce::String (d.fromSeconds, 3) + " .. " + juce::String (d.toSeconds, 3) + " s)"
                        : juce::String ("not written (the run did not reach it, or no --report)"))
          << "\n";
    s << "  verdict   " << (aborted ? "ABORTED" : (findings ? "FINDINGS" : "CLEAN")) << " (exit " << exitCode << ")\n";
    if (findingsText.isNotEmpty())
        s << "  detections (first 60):\n" << findingsText;
    summary = s;
}
} // namespace flub::app
