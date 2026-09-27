#include "PluginProcessor.h"

#include "PluginEditor.h"

#include "flub/common/Denormals.h"
#include "flub/io/Json.h"
#include "flub/io/PresetIO.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

namespace flub::plugin
{
using flub::param::Info;
using flub::param::Unit;

namespace
{
/** Re-prepare poll rate for structural parameter changes. */
constexpr int kStructuralPollHz = 5;

const juce::Identifier kStateType { "FlubsoundFX" };
const juce::Identifier kStateVersionProperty { "flubStateVersion" };
constexpr int kStateVersion = 1;
// AudioProcessorValueTreeState's tree layout: <PARAM id="key" value="..."/>
// per parameter (the denormalised value).
const juce::Identifier kParamType { "PARAM" };
const juce::Identifier kParamIdProperty { "id" };
const juce::Identifier kParamValueProperty { "value" };

//==============================================================================
// Display helpers for float parameters
juce::String unitLabel (Unit u)
{
    switch (u)
    {
        case Unit::Db: return "dB";
        case Unit::Hz: return "Hz";
        case Unit::Ms: return "ms";
        case Unit::Percent: return "%";
        case Unit::Ratio: return ":1";
        case Unit::Lufs: return "LUFS";
        case Unit::Degrees: return "deg";
        case Unit::Millimetres: return "mm";
        case Unit::DbPerSec: return "dB/s";
        case Unit::None:
        case Unit::Choice:
        case Unit::Toggle: break;
    }
    return {};
}

int decimalsFor (Unit u, float value)
{
    const float a = std::abs (value);
    switch (u)
    {
        case Unit::Hz: return a < 100.0f ? 1 : 0;
        case Unit::Ms: return a < 10.0f ? 2 : 1;
        case Unit::Percent: return 0;
        case Unit::Degrees: return 0;
        case Unit::None: return 2;
        case Unit::Db:
        case Unit::Ratio:
        case Unit::Lufs:
        case Unit::Millimetres:
        case Unit::DbPerSec:
        case Unit::Choice:
        case Unit::Toggle: break;
    }
    return 1;
}

juce::String groupIdFor (const std::string& name)
{
    // Group IDs: basic characters only (JUCE: no '.', not a pure integer).
    juce::String id ("grp_");
    for (auto c : juce::String (name))
    {
        if (juce::CharacterFunctions::isLetterOrDigit (c))
            id << juce::String::charToString (juce::CharacterFunctions::toLowerCase (c));
        else
            id << "_";
    }
    return id; // "Noise Gate" -> "grp_noise_gate"
}

std::unique_ptr<juce::RangedAudioParameter> makeParameter (const Info& info)
{
    // ID = the preset key (never changes). Version hint = the layout version
    // that introduced the parameter (Info::sinceVersion). AUv2 orders
    // parameters by version hint, then by ID hash, so as long as a parameter
    // added in a later release gets the next version, Logic / GarageBand
    // automation of the existing parameters still recalls.
    const juce::ParameterID pid { juce::String (info.key), info.sinceVersion };
    const juce::String name (info.name);

    if (info.unit == Unit::Toggle)
        return std::make_unique<juce::AudioParameterBool> (pid, name, info.defaultValue >= 0.5f,
                                                           juce::AudioParameterBoolAttributes().withAutomatable (! info.structural));

    if (info.unit == Unit::Choice)
    {
        juce::StringArray choices;
        for (const auto& c : info.choices)
            choices.add (juce::String (c));
        return std::make_unique<juce::AudioParameterChoice> (pid, name, choices, static_cast<int> (std::lround (info.defaultValue)),
                                                             juce::AudioParameterChoiceAttributes().withAutomatable (! info.structural));
    }

    juce::NormalisableRange<float> range (info.minValue, info.maxValue);
    if (info.skewCentre > info.minValue && info.skewCentre < info.maxValue)
        range.setSkewForCentre (info.skewCentre);

    const Unit unit = info.unit;
    const bool percent = unit == Unit::Percent;
    auto attributes = juce::AudioParameterFloatAttributes()
                          .withLabel (unitLabel (unit))
                          .withAutomatable (! info.structural)
                          .withStringFromValueFunction ([unit, percent] (float v, int maxLength) {
                              const float shown = percent ? v * 100.0f : v;
                              auto text = juce::String (shown, decimalsFor (unit, shown));
                              return maxLength > 0 ? text.substring (0, maxLength) : text;
                          })
                          .withValueFromStringFunction ([unit, percent] (const juce::String& text) {
                              auto t = text.trim();
                              // "1.5k" / "1.5 kHz" for frequencies; getFloatValue() ignores a trailing unit.
                              const bool kilo = unit == Unit::Hz && (t.endsWithIgnoreCase ("k") || t.endsWithIgnoreCase ("khz"));
                              const float v = t.getFloatValue() * (kilo ? 1000.0f : 1.0f);
                              return percent ? v / 100.0f : v;
                          });
    return std::make_unique<juce::AudioParameterFloat> (pid, name, range, info.defaultValue, attributes);
}
} // namespace

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout FlubsoundProcessor::createParameterLayout()
{
    using namespace flub::param;
    const auto& table = layout();

    // Groups in order of first appearance; the EQ and dynamic-EQ bands get one
    // sub-group per band so generic editors / host menus stay navigable.
    struct GroupEntry
    {
        std::string name;
        std::unique_ptr<juce::AudioProcessorParameterGroup> group;
        std::vector<std::unique_ptr<juce::AudioProcessorParameterGroup>> bands;
    };
    std::vector<GroupEntry> groups;

    auto groupFor = [&groups] (const std::string& name) -> GroupEntry& {
        for (auto& g : groups)
            if (g.name == name)
                return g;
        GroupEntry e;
        e.name = name;
        e.group = std::make_unique<juce::AudioProcessorParameterGroup> (groupIdFor (name), juce::String (name), "|");
        groups.push_back (std::move (e));
        return groups.back();
    };

    for (int id = 0; id < kNumParams; ++id)
    {
        const auto& info = table[static_cast<size_t> (id)];
        auto& g = groupFor (info.group);
        auto parameter = makeParameter (info);

        int band = -1;
        if (id >= kEqBase && id < kDynBase)
            band = (id - kEqBase) / kEqFields;
        else if (id >= kDynBase)
            band = (id - kDynBase) / kDynFields;

        if (band < 0)
        {
            g.group->addChild (std::move (parameter));
            continue;
        }
        while (static_cast<int> (g.bands.size()) <= band)
        {
            const auto n = static_cast<int> (g.bands.size());
            const juce::String sub = (id >= kDynBase ? "Dyn " : "Band ") + juce::String (n + 1);
            g.bands.push_back (std::make_unique<juce::AudioProcessorParameterGroup> (groupIdFor (info.group) + "_" + juce::String (n + 1),
                                                                                    sub, "|"));
        }
        g.bands[static_cast<size_t> (band)]->addChild (std::move (parameter));
    }

    juce::AudioProcessorValueTreeState::ParameterLayout result;
    for (auto& g : groups)
    {
        for (auto& b : g.bands)
            g.group->addChild (std::move (b));
        result.add (std::move (g.group));
    }
    return result;
}

//==============================================================================
FlubsoundProcessor::FlubsoundProcessor()
    : juce::AudioProcessor (BusesProperties()
                                .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, kStateType, createParameterLayout())
{
    const auto& table = flub::param::layout();
    for (int id = 0; id < flub::param::kNumParams; ++id)
    {
        const auto& info = table[static_cast<size_t> (id)];
        rawValues[static_cast<size_t> (id)] = apvts.getRawParameterValue (juce::String (info.key));
        jassert (rawValues[static_cast<size_t> (id)] != nullptr);
        lastPushed[static_cast<size_t> (id)] = std::numeric_limits<float>::quiet_NaN(); // forces the first push
    }
    for (int c = 0; c < flub::kMaxChannels; ++c)
        channelMap[static_cast<size_t> (c)] = c;

    bypassParameter = apvts.getParameter (juce::String (table[static_cast<size_t> (flub::param::BypassAll)].key));
    jassert (bypassParameter != nullptr);

    pushParametersToStore();
    startTimerHz (kStructuralPollHz);
}

FlubsoundProcessor::~FlubsoundProcessor()
{
    stopTimer();
}

const juce::String FlubsoundProcessor::getName() const
{
    return JucePlugin_Name;
}

//==============================================================================
bool FlubsoundProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    const auto in = layouts.getMainInputChannelSet();
    return in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo()
           || in == juce::AudioChannelSet::create5point1() || in == juce::AudioChannelSet::create7point1();
}

void FlubsoundProcessor::pushParametersToStore() noexcept
{
    for (size_t id = 0; id < rawValues.size(); ++id)
    {
        const float v = rawValues[id]->load (std::memory_order_relaxed);
        if (v != lastPushed[id]) // NaN sentinel never compares equal
        {
            store.set (static_cast<int> (id), v); // clamps; relaxed atomic store
            lastPushed[id] = v;
        }
    }
}

void FlubsoundProcessor::prepareChain (double sampleRate, int maxBlockSize)
{
    using juce::AudioChannelSet;

    // Map chain channels (WAVE_FORMAT_EXTENSIBLE order) onto JUCE buffer channels.
    const auto inSet = getChannelLayoutOfBus (true, 0);
    const int numIn = inSet.size();
    duplicateMono = numIn == 1;
    chainInputChannels = (numIn == 6 || numIn == 8) ? numIn : 2;
    for (int c = 0; c < flub::kMaxChannels; ++c)
        channelMap[static_cast<size_t> (c)] = c;

    if (chainInputChannels > 2)
    {
        // 5.1 : FL FR FC LFE SL SR          7.1 : FL FR FC LFE BL BR SL SR
        const AudioChannelSet::ChannelType order51[] = { AudioChannelSet::left, AudioChannelSet::right, AudioChannelSet::centre,
                                                         AudioChannelSet::LFE, AudioChannelSet::leftSurround, AudioChannelSet::rightSurround };
        const AudioChannelSet::ChannelType order71[] = { AudioChannelSet::left,
                                                         AudioChannelSet::right,
                                                         AudioChannelSet::centre,
                                                         AudioChannelSet::LFE,
                                                         AudioChannelSet::leftSurroundRear,
                                                         AudioChannelSet::rightSurroundRear,
                                                         AudioChannelSet::leftSurroundSide,
                                                         AudioChannelSet::rightSurroundSide };
        const auto* order = chainInputChannels == 8 ? order71 : order51;
        bool complete = true;
        std::array<int, flub::kMaxChannels> map {};
        for (int c = 0; c < chainInputChannels; ++c)
        {
            map[static_cast<size_t> (c)] = inSet.getChannelIndexForType (order[c]);
            complete = complete && map[static_cast<size_t> (c)] >= 0;
        }
        if (complete)
            channelMap = map;
        // else: unknown variant - keep the host order (identity), still stereo out.
    }

    preparedSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    preparedBlockSize = juce::jmax (32, maxBlockSize);

    // Structural parameters (latency profile) are read by prepare().
    std::fill (lastPushed.begin(), lastPushed.end(), std::numeric_limits<float>::quiet_NaN());
    pushParametersToStore();

    chain.prepare ({ preparedSampleRate, preparedBlockSize, chainInputChannels });
    for (size_t id = 0; id < preparedValues.size(); ++id)
        preparedValues[id] = store.get (static_cast<int> (id));

    const int latency = chain.getLatencySamples();
    reportedLatency.store (latency, std::memory_order_relaxed);
    setLatencySamples (latency);
}

void FlubsoundProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const std::lock_guard<std::mutex> lock (prepareMutex);
    prepared.store (false, std::memory_order_release);
    prepareChain (sampleRate, samplesPerBlock);
    prepared.store (true, std::memory_order_release);
}

void FlubsoundProcessor::releaseResources()
{
    const std::lock_guard<std::mutex> lock (prepareMutex);
    prepared.store (false, std::memory_order_release);
}

void FlubsoundProcessor::reset()
{
    // Hosts call this to flush tails (e.g. on transport jumps); never
    // concurrently with a block we are processing.
    const juce::ScopedLock sl (getCallbackLock());
    if (prepared.load (std::memory_order_acquire))
        chain.reset();
}

bool FlubsoundProcessor::structuralParameterChanged() const noexcept
{
    // Loops over every Info::structural parameter (no hard-coded list), so a
    // future structural parameter needs no change here. Two views:
    //  * the chain's own check sees the values the audio thread has pushed
    //    into the store;
    //  * the APVTS values also catch a change made while the host is not
    //    processing, so the re-prepare (and the new latency) lands before
    //    playback resumes.
    if (chain.needsReprepare())
        return true;

    const auto& table = flub::param::layout();
    for (size_t id = 0; id < rawValues.size(); ++id)
    {
        if (! table[id].structural)
            continue;
        const float v = rawValues[id]->load (std::memory_order_relaxed);
        if (! std::isnan (v) && table[id].clamp (v) != preparedValues[id]) // the store ignores NaN too
            return true;
    }
    return false;
}

void FlubsoundProcessor::timerCallback()
{
    // Structural parameter changed (latency profile)? Re-prepare off the audio
    // thread: suspendProcessing() waits for the current block (callback lock),
    // the host receives silence for the few ms the prepare takes, then the new
    // latency is reported. Profiles are a deliberate, rare user action.
    if (! prepared.load (std::memory_order_acquire))
        return;

    {
        // The prepared state is written by prepareToPlay (possibly on another thread).
        const std::lock_guard<std::mutex> lock (prepareMutex);
        if (! structuralParameterChanged())
            return;
    }

    // Lock order: the callback lock is never requested while prepareMutex is
    // held. Some wrappers call prepareToPlay with the callback lock held (AU
    // offline-render switch), so the opposite order could deadlock.
    suspendProcessing (true);
    {
        const std::lock_guard<std::mutex> lock (prepareMutex);
        if (prepared.load (std::memory_order_acquire) && structuralParameterChanged())
            prepareChain (preparedSampleRate, preparedBlockSize);
    }
    suspendProcessing (false);
}

//==============================================================================
void FlubsoundProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    processInternal (buffer, false);
}

void FlubsoundProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // Only reached with hosts that bypass without our bypass parameter: use
    // the chain's own bypass so latency and the crossfade stay consistent.
    processInternal (buffer, true);
}

void FlubsoundProcessor::processInternal (juce::AudioBuffer<float>& buffer, bool forceBypass) noexcept
{
    const flub::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    if (! prepared.load (std::memory_order_acquire) || numSamples <= 0)
    {
        buffer.clear();
        return;
    }

    const int needed = juce::jmax (2, chainInputChannels);
    if (buffer.getNumChannels() < needed)
    {
        buffer.clear(); // host violated the negotiated layout
        return;
    }

    pushParametersToStore();
    if (forceBypass)
    {
        store.set (flub::param::BypassAll, 1.0f);
        lastPushed[static_cast<size_t> (flub::param::BypassAll)] = std::numeric_limits<float>::quiet_NaN(); // restore next block
    }

    // Mono in -> stereo: the second buffer channel is an output-only channel
    // (undefined content), so it receives a copy of the input.
    if (duplicateMono)
        buffer.copyFrom (1, 0, buffer, 0, 0, numSamples);

    std::array<float*, flub::kMaxChannels> pointers {};
    for (int c = 0; c < chainInputChannels; ++c)
        pointers[static_cast<size_t> (c)] = buffer.getWritePointer (channelMap[static_cast<size_t> (c)]);

    for (int pos = 0; pos < numSamples; pos += preparedBlockSize)
    {
        const int n = juce::jmin (preparedBlockSize, numSamples - pos);
        chain.process (flub::AudioBlock (pointers.data(), chainInputChannels, n, pos));
    }

    // Anything beyond the stereo output (surround inputs) is cleared by the
    // chain; clear extra buffer channels the chain does not know about.
    for (int c = chainInputChannels; c < buffer.getNumChannels(); ++c)
        buffer.clear (c, 0, numSamples);
}

//==============================================================================
juce::AudioProcessorParameter* FlubsoundProcessor::getBypassParameter() const
{
    return bypassParameter; // RT-safe: called by the wrappers on the audio thread
}

double FlubsoundProcessor::getTailLengthSeconds() const
{
    // Release stages (compressor up to 2 s, maximizer up to 1 s) and the
    // virtualiser room decay; 0.5 s covers the audible tail at defaults.
    return 0.5;
}

//==============================================================================
void FlubsoundProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty (kStateVersionProperty, kStateVersion, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void FlubsoundProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;
    const auto saved = juce::ValueTree::fromXml (*xml);

    // Deterministic recall (docs/11 E52 Phase A): every parameter the saved
    // state does not carry (an older state, written before the parameter
    // existed) takes its DEFAULT, never the value it had before the load.
    // What APVTS::replaceState does with a missing PARAM depends on JUCE
    // internals (the child-added notification of the tree it appends), so the
    // loaded tree is rebuilt with one PARAM per parameter, its value resolved
    // (and clamped) by the core, the same rule the tests check.
    // A state from a newer build (flubStateVersion > kStateVersion) loads the
    // same way: unknown parameters are ignored, values are clamped.
    std::vector<std::pair<std::string, float>> carried;
    std::vector<std::string> warnings;
    for (auto child : saved)
    {
        if (! child.hasType (kParamType) || ! child.hasProperty (kParamIdProperty))
            continue;
        // Attributes read back from XML are text: accept a whole number only
        // (a missing or malformed value leaves the parameter at its default).
        const auto key = child[kParamIdProperty].toString().toStdString();
        const auto text = child[kParamValueProperty].toString().trim().toStdString();
        char* end = nullptr;
        const double value = std::strtod (text.c_str(), &end);
        if (text.empty() || end != text.c_str() + text.size())
            warnings.push_back ("\"" + key + "\": no numeric value, default used");
        else
            carried.emplace_back (key, static_cast<float> (value));
    }
    std::vector<std::string> resolveWarnings; // unknown parameters, clamped values
    const auto values = flub::preset::resolveSavedState (carried, &resolveWarnings);
    warnings.insert (warnings.end(), resolveWarnings.begin(), resolveWarnings.end());
    if (static_cast<int> (saved.getProperty (kStateVersionProperty, 0)) > kStateVersion)
        warnings.insert (warnings.begin(), "state written by a newer Flubsound FX (flubStateVersion "
                                               + saved.getProperty (kStateVersionProperty).toString().toStdString() + ")");

    juce::ValueTree state (apvts.state.getType());
    state.copyPropertiesFrom (saved, nullptr);
    for (auto child : saved)
        if (! child.hasType (kParamType))
            state.appendChild (child.createCopy(), nullptr);
    const auto& table = flub::param::layout();
    for (size_t id = 0; id < table.size(); ++id)
    {
        juce::ValueTree param (kParamType);
        param.setProperty (kParamIdProperty, juce::String (table[id].key), nullptr);
        param.setProperty (kParamValueProperty, static_cast<double> (values[id]), nullptr);
        state.appendChild (param, nullptr);
    }
    apvts.replaceState (state); // the audio thread picks the values up through the raw values

    for ([[maybe_unused]] const auto& w : warnings)
        DBG ("Flubsound FX state: " << juce::String (w));
}

//==============================================================================
bool FlubsoundProcessor::importPreset (const juce::File& file, juce::String& error, juce::StringArray* warnings)
{
    // Read through juce::File (Unicode paths on every platform), parse with
    // the core so the app, the CLI and the plug-in agree on the format.
    if (! file.existsAsFile())
    {
        error = "File not found: " + file.getFullPathName();
        return false;
    }
    flub::json::Value root;
    std::string parseError;
    if (! flub::json::parse (file.loadFileAsString().toStdString(), root, parseError))
    {
        error = file.getFileName() + ": " + juce::String (parseError);
        return false;
    }
    flub::preset::Preset preset;
    if (! flub::preset::fromJson (root, preset, parseError))
    {
        error = file.getFileName() + ": " + juce::String (parseError);
        return false;
    }

    const auto& table = flub::param::layout();
    for (int id = 0; id < flub::param::kNumParams; ++id)
    {
        if (flub::preset::isAppState (id))
            continue; // bypass, bypass.matched, latency.profile: application state, not preset state (docs/11 E40)
        if (auto* p = apvts.getParameter (juce::String (table[static_cast<size_t> (id)].key)))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (preset.values[static_cast<size_t> (id)]));
            p->endChangeGesture();
        }
    }
    if (warnings != nullptr)
    {
        warnings->clear();
        for (const auto& w : preset.warnings) // unknown keys, clamped values, a newer minor (docs/11 E52)
            warnings->add (juce::String::fromUTF8 (w.c_str()));
    }
    return true;
}

bool FlubsoundProcessor::exportPreset (const juce::File& file, juce::String& error) const
{
    auto preset = flub::preset::makeDefault();
    auto name = file.getFileNameWithoutExtension();
    if (name.endsWithIgnoreCase (".flubpreset"))
        name = name.dropLastCharacters (11);
    preset.name = name.toStdString();
    preset.category = "User";
    preset.author = "User";
    preset.uuid = flub::preset::makeUuid(); // a new preset: its own stable identity (docs/11 E52)

    // Host normalisation (skewed ranges) leaves float dust such as
    // 5.0000005 for a default of 5: snap such values back onto the default so
    // the preset only lists what the user really changed.
    const auto& table = flub::param::layout();
    for (int id = 0; id < flub::param::kNumParams; ++id)
    {
        const auto& info = table[static_cast<size_t> (id)];
        float v = rawValues[static_cast<size_t> (id)]->load (std::memory_order_relaxed);
        if (std::abs (v - info.defaultValue) <= 1.0e-5f * (info.maxValue - info.minValue))
            v = info.defaultValue;
        preset.values[static_cast<size_t> (id)] = v;
    }
    const auto bypass = static_cast<size_t> (flub::param::BypassAll);
    preset.values[bypass] = table[bypass].defaultValue;

    // Numbers are floats: write them with float precision ("0.6", not
    // "0.6000000238418579") so presets stay readable and diff cleanly.
    auto root = flub::preset::toJson (preset);
    flub::json::Value params { flub::json::Value::Object {} };
    for (const auto& [key, value] : root["params"].asObject())
    {
        if (! value.isNumber())
        {
            params.set (key, value);
            continue;
        }
        char buf[32];
        std::snprintf (buf, sizeof (buf), "%.6g", value.asNumber());
        params.set (key, flub::json::Value (std::strtod (buf, nullptr)));
    }
    root.set ("params", std::move (params));

    const auto text = flub::json::write (root, 2) + "\n";
    if (! file.replaceWithText (juce::String::fromUTF8 (text.c_str()), false, false, "\n"))
    {
        error = "Cannot write " + file.getFullPathName();
        return false;
    }
    return true;
}

//==============================================================================
juce::AudioProcessorEditor* FlubsoundProcessor::createEditor()
{
    return new FlubsoundEditor (*this);
}
} // namespace flub::plugin

//==============================================================================
// The plug-in client wrappers create the processor through this factory.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new flub::plugin::FlubsoundProcessor();
}
