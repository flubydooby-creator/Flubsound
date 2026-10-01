// Plug-in tests: Flubsound FX's state round-trip (docs/11 E53 step 3).
//
// A host saves a project with getStateInformation and reopens it with
// setStateInformation into a fresh instance. The reopened instance must hold
// exactly the same values (bit for bit), save exactly the same bytes and
// process audio to exactly the same samples:
// * every parameter, at four settings (minimum, maximum and two interior
//   points as a host sets them through the normalised value; toggles both
//   ways; choices through every entry);
// * every factory preset, imported as a user does (Import preset); the
//   imported values are the preset's own, bit for bit, and a new instance
//   holds the exact defaults.
// The variants also set the latency profile (a structural parameter), so the
// reopened instance must report the same latency too.
// Built when the plug-in is (FLUB_BUILD_PLUGIN); the Flubsound FX processor's
// sources are compiled into this test binary (tests/app/CMakeLists.txt).
#include "AppTestSupport.h"

#if FLUB_APP_TESTS_HAVE_PLUGIN

 #include "PluginProcessor.h"

 #include "flub/engine/Parameters.h"
 #include "flub/io/Json.h"
 #include "flub/io/PresetIO.h"

 #if FLUB_HAS_FACTORY_PRESETS
  #include "FlubsoundPresetData.h"
 #endif

 #include <cmath>
 #include <cstring>
 #include <iostream>
 #include <memory>
 #include <vector>

namespace
{
using flub::plugin::FlubsoundProcessor;

constexpr double kRate = 48000.0;
constexpr int kBlock = 512;
constexpr int kBlocks = 4;

std::vector<float> rawValues (FlubsoundProcessor& p)
{
    std::vector<float> values;
    for (const auto& info : flub::param::layout())
        values.push_back (p.getValueTreeState().getRawParameterValue (juce::String (info.key))->load());
    return values;
}

/** Keys of the parameters whose values differ in their bits. */
juce::StringArray differingKeys (const std::vector<float>& a, const std::vector<float>& b)
{
    juce::StringArray keys;
    const auto& table = flub::param::layout();
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        if (std::memcmp (&a[i], &b[i], sizeof (float)) != 0)
            keys.add (juce::String (table[i].key) + " " + juce::String (a[i], 9) + " != " + juce::String (b[i], 9));
    return keys;
}

juce::MemoryBlock saveState (FlubsoundProcessor& p)
{
    juce::MemoryBlock block;
    p.getStateInformation (block);
    return block;
}

/** kBlocks blocks of a deterministic stereo programme (a tone plus noise at
    about -12 dBFS) through `p`, prepared here; the output samples. */
std::vector<float> render (FlubsoundProcessor& p)
{
    p.prepareToPlay (kRate, kBlock);
    juce::Random random (7);
    juce::AudioBuffer<float> buffer (2, kBlock);
    juce::MidiBuffer midi;
    std::vector<float> out;
    for (int b = 0; b < kBlocks; ++b)
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const double t = static_cast<double> (b * kBlock + i) / kRate;
            const auto tone = static_cast<float> (0.2 * std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * t));
            buffer.setSample (0, i, tone + 0.05f * (random.nextFloat() - 0.5f));
            buffer.setSample (1, i, 0.8f * tone + 0.05f * (random.nextFloat() - 0.5f));
        }
        p.processBlock (buffer, midi);
        for (int c = 0; c < 2; ++c)
            out.insert (out.end(), buffer.getReadPointer (c), buffer.getReadPointer (c) + kBlock);
    }
    p.releaseResources();
    return out;
}

bool sameBits (const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0;
}

/** Saves `a`, loads the state into a fresh instance and checks the values,
    the saved bytes and (with `renderToo`) the audio; `what` names the case
    in a failure. Returns the number of failed checks. */
int checkRoundTrip (FlubsoundProcessor& a, const juce::String& what, bool renderToo)
{
    const auto state = saveState (a);
    FlubsoundProcessor b;
    b.setStateInformation (state.getData(), static_cast<int> (state.getSize()));

    int failures = 0;
    const auto differing = differingKeys (rawValues (a), rawValues (b));
    if (! differing.isEmpty())
    {
        ++failures;
        std::cerr << "    " << what << ": values differ after the round-trip: " << differing.joinIntoString ("; ") << "\n";
    }
    if (saveState (b) != state)
    {
        ++failures;
        std::cerr << "    " << what << ": the reopened instance saves different bytes\n";
    }
    if (renderToo)
    {
        const auto ra = render (a);
        const auto rb = render (b);
        if (! sameBits (ra, rb))
        {
            ++failures;
            std::cerr << "    " << what << ": the reopened instance processes to different samples\n";
        }
        if (b.getChainLatencySamples() != a.getChainLatencySamples())
        {
            ++failures;
            std::cerr << "    " << what << ": latency " << b.getChainLatencySamples() << " != " << a.getChainLatencySamples() << "\n";
        }
    }
    return failures;
}

/** Sets every parameter as a host would (normalised value, with the change
    gesture): variant 0 minimum, 1 maximum, 2 and 3 interior points; toggles
    alternate; choices step through their entries. Bypass All stays off so
    the render compares processing. */
void setVariant (FlubsoundProcessor& p, int variant)
{
    static constexpr float kInterior[] = { 0.0f, 1.0f, 0.3f, 0.77f };
    const auto& table = flub::param::layout();
    for (size_t id = 0; id < table.size(); ++id)
    {
        const auto& info = table[id];
        auto* parameter = p.getValueTreeState().getParameter (juce::String (info.key));
        if (parameter == nullptr || static_cast<int> (id) == flub::param::BypassAll)
            continue;
        float normalised = kInterior[variant];
        if (info.unit == flub::param::Unit::Toggle)
            normalised = ((static_cast<int> (id) + variant) % 2) != 0 ? 1.0f : 0.0f;
        else if (info.unit == flub::param::Unit::Choice)
        {
            const int count = static_cast<int> (info.choices.size());
            const int index = (static_cast<int> (id) + variant) % juce::jmax (1, count);
            normalised = parameter->convertTo0to1 (static_cast<float> (index));
        }
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost (normalised);
        parameter->endChangeGesture();
    }
}

struct FactoryPreset
{
    juce::String name;
    juce::String json;
};

std::vector<FactoryPreset> factoryPresets()
{
    std::vector<FactoryPreset> presets;
 #if FLUB_HAS_FACTORY_PRESETS
    for (int i = 0; i < FlubsoundPresetData::namedResourceListSize; ++i)
    {
        int size = 0;
        const char* data = FlubsoundPresetData::getNamedResource (FlubsoundPresetData::namedResourceList[i], size);
        if (data != nullptr && size > 0)
            presets.push_back ({ FlubsoundPresetData::getNamedResourceOriginalFilename (FlubsoundPresetData::namedResourceList[i]),
                                 juce::String::fromUTF8 (data, size) });
    }
 #endif
    return presets;
}

/** Imports every factory preset whose file name starts with one of
    `prefixes` and round-trips it. Returns how many were checked. */
int roundTripFactoryPresets (const juce::StringArray& prefixes)
{
    flubapptest::TempFolder temp;
    int checked = 0;
    for (const auto& preset : factoryPresets())
    {
        bool wanted = false;
        for (const auto& prefix : prefixes)
            wanted = wanted || preset.name.startsWith (prefix);
        if (! wanted)
            continue;
        const auto file = temp.file (preset.name);
        REQUIRE (file.replaceWithText (preset.json));
        FlubsoundProcessor a;
        juce::String error;
        if (! a.importPreset (file, error))
        {
            std::cerr << "    " << preset.name << ": " << error << "\n";
            CHECK (false);
            continue;
        }
        CHECK (checkRoundTrip (a, preset.name, true) == 0);
        // The imported values are the preset's own, bit for bit (the app
        // plays the same), not their trip through the normalised 0..1.
        flub::json::Value root;
        std::string parseError;
        flub::preset::Preset parsed;
        REQUIRE (flub::json::parse (preset.json.toStdString(), root, parseError) && flub::preset::fromJson (root, parsed, parseError));
        const auto imported = rawValues (a);
        juce::StringArray notExact;
        for (size_t id = 0; id < imported.size(); ++id)
            if (! flub::preset::isAppState (static_cast<int> (id)) && std::memcmp (&imported[id], &parsed.values[id], sizeof (float)) != 0)
                notExact.add (juce::String (flub::param::layout()[id].key) + " " + juce::String (imported[id], 9));
        if (! notExact.isEmpty())
            std::cerr << "    " << preset.name << ": imported values differ from the preset: " << notExact.joinIntoString ("; ") << "\n";
        CHECK (notExact.isEmpty());
        ++checked;
    }
    return checked;
}
} // namespace

TEST_CASE ("Plug-in state: every parameter at its minimum, maximum and two interior points round-trips bit-identical (E53)")
{
    for (int variant = 0; variant < 4; ++variant)
    {
        FlubsoundProcessor a;
        setVariant (a, variant);
        CHECK (checkRoundTrip (a, "variant " + juce::String (variant), variant >= 2) == 0);
    }
}

TEST_CASE ("Plug-in state: a new instance holds the exact defaults; a state saved twice is the same bytes and round-trips (E53)")
{
    FlubsoundProcessor a;
    std::vector<float> defaults;
    for (const auto& info : flub::param::layout())
        defaults.push_back (info.defaultValue);
    const auto differing = differingKeys (rawValues (a), defaults);
    if (! differing.isEmpty())
        std::cerr << "    not the defaults: " << differing.joinIntoString ("; ") << "\n";
    CHECK (differing.isEmpty());
    const auto first = saveState (a);
    CHECK (saveState (a) == first);
    CHECK (checkRoundTrip (a, "defaults", true) == 0);
}

TEST_CASE ("Plug-in state: every Music factory preset round-trips bit-identical in values, state and audio (E53)")
{
    const int checked = roundTripFactoryPresets ({ "music-" });
    CHECK (checked >= 15);
}

TEST_CASE ("Plug-in state: every Gaming and device factory preset round-trips bit-identical in values, state and audio (E53)")
{
    const int checked = roundTripFactoryPresets ({ "gaming-", "device-" });
    CHECK (checked >= 12);
    CHECK (factoryPresets().size() >= 25); // the Done-when's 25+
}

TEST_CASE ("Plug-in Smart macros (docs/11 E34): a preset's \"smart\" flag is imported, exported and kept in the state; a state without it loads with Smart off")
{
    flubapptest::TempFolder temp;
    auto smartPreset = flub::preset::makeDefault();
    smartPreset.name = "Smart";
    smartPreset.smart = true;
    const auto smartFile = temp.file ("smart.flubpreset.json");
    REQUIRE (smartFile.replaceWithText (juce::String::fromUTF8 (flub::json::write (flub::preset::toJson (smartPreset), 2).c_str())));
    auto plainPreset = smartPreset;
    plainPreset.smart = false;
    const auto plainFile = temp.file ("plain.flubpreset.json");
    REQUIRE (plainFile.replaceWithText (juce::String::fromUTF8 (flub::json::write (flub::preset::toJson (plainPreset), 2).c_str())));

    FlubsoundProcessor a;
    const auto offState = saveState (a);
    CHECK (! a.getSmartMacros());
    juce::String error;
    REQUIRE (a.importPreset (smartFile, error));
    CHECK (a.getSmartMacros());
    CHECK (checkRoundTrip (a, "smart preset", false) == 0);
    const auto smartState = saveState (a);
    FlubsoundProcessor b;
    b.setStateInformation (smartState.getData(), static_cast<int> (smartState.getSize()));
    CHECK (b.getSmartMacros());

    // Export writes it; importing a preset without it turns Smart off.
    const auto exported = temp.file ("exported.flubpreset.json");
    REQUIRE (a.exportPreset (exported, error));
    flub::json::Value root;
    std::string parseError;
    REQUIRE (flub::json::parse (exported.loadFileAsString().toStdString(), root, parseError));
    CHECK (root["smart"].asBool (false));
    REQUIRE (a.importPreset (plainFile, error));
    CHECK (! a.getSmartMacros());
    CHECK (saveState (a) == offState); // Smart off writes no property: older states keep their bytes

    // A state without the property (an older project) loads with Smart off.
    b.setStateInformation (offState.getData(), static_cast<int> (offState.getSize()));
    CHECK (! b.getSmartMacros());
}

#endif // FLUB_APP_TESTS_HAVE_PLUGIN
