// App-level tests: what a preset may change on a strip, and what a strip
// starts with.
//
// * docs/11 E40 (app half): PresetManager::loadIntoBank / EngineController::
//   loadPreset write a preset's sound only. Bypass All, loudness-matched
//   bypass and the latency profile are application state: loading any
//   factory preset, or a user preset that still carries a profile, leaves
//   them alone, so a preset never re-prepares the engine or pads the other
//   strips (MixEngine). A saved user preset names its profile as
//   "suggestedLatencyProfile" instead of writing it.
// * The user's device-profiles.json override is read at start-up from the
//   user data folder, whose name the test runner makes non-ASCII.
// * docs/11 E36 / E23: a strip with no saved state starts from a default
//   preset - Signature (Music, System), Voice Chat (Chat), "First Run - Game"
//   (Competitive FPS capped) - and saved state is never overwritten. The
//   defaults are checked where they are heard: rendered through the app's
//   engine (AudioEngineHost::renderOffline -> MixEngine -> master limiter).
//     - Voice Chat (E23 Done-when): speech at -35 and -12 LUFS ends within
//       3 LU short-term, no maximizer drive, ceiling held.
//     - First Run - Game (E36 Done-when): -50 / -60 dBFS pink beds lifted by
//       at most +3 LU, and FL / FR-only content in the 8-channel Game strip
//       switches to the stereo passthrough fold (E27).
//   The step/bed contrast numbers of the Game default use the E59 slice's
//   burst scenes and are recorded in EngineController.cpp.
#include "AppTestSupport.h"
#include "TestSignals.h"

#include "engine/EngineController.h"
#include "presets/PresetManager.h"
#include "settings/UserDataFolder.h"

#include "flub/analysis/LoudnessMeter.h"
#include "flub/io/Json.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace flub::app;
using namespace flub::param;

namespace
{
constexpr double kFs = 48000.0;
constexpr int kBlock = 512;

EngineController::Options headlessOptions (const flubapptest::TempFolder& temp, bool restoreState, bool persist = false)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = restoreState;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = persist;
    return o;
}

float profileValue (LatencyProfileValue p) { return static_cast<float> (static_cast<int> (p)); }

/** Feeds `left` / `right` (from the start, then silence) into one strip's
    first two channels; every other channel and strip gets nothing. */
class TwoChannelSource final : public StripSignalSource
{
public:
    TwoChannelSource (int stripIndex, const std::vector<float>& l, const std::vector<float>& r) : strip (stripIndex), left (l), right (r) {}

    bool renderStrip (int s, const flub::AudioBlock& block) override
    {
        if (s != strip)
            return false;
        for (int c = 0; c < block.numChannels; ++c)
        {
            float* out = block.channel (c);
            for (int i = 0; i < block.numSamples; ++i)
            {
                const auto idx = static_cast<size_t> (pos + i);
                out[i] = c == 0 && idx < left.size() ? left[idx] : c == 1 && idx < right.size() ? right[idx] : 0.0f;
            }
        }
        pos += block.numSamples;
        return true;
    }

private:
    int strip;
    const std::vector<float>& left;
    const std::vector<float>& right;
    int pos = 0;
};

/** Integrated loudness (LUFS) of a stereo signal from `fromSample` on. */
double integratedLufs (const std::vector<float>& l, const std::vector<float>& r, int fromSample)
{
    flub::LoudnessMeter meter;
    meter.prepare (kFs, 2);
    std::vector<float> a (static_cast<size_t> (kBlock)), b (static_cast<size_t> (kBlock));
    for (int pos = fromSample; pos + kBlock <= static_cast<int> (l.size()); pos += kBlock)
    {
        std::copy_n (l.begin() + pos, kBlock, a.begin());
        std::copy_n (r.begin() + pos, kBlock, b.begin());
        float* ch[] = { a.data(), b.data() };
        meter.process (flub::AudioBlock (ch, 2, kBlock));
    }
    return meter.getIntegratedLufs();
}

struct RenderResult
{
    std::vector<float> left, right;
    double shortTermLufsAtEnd = -200.0; // the last 3 s of the master output
    double peakDb = -200.0;             // sample peak of the master output
};

/** Plays `in` (stereo) on `strip` for its length and returns the master output. */
RenderResult renderThroughEngine (EngineController& controller, int strip, const std::vector<float>& inL, const std::vector<float>& inR)
{
    auto& host = controller.getHost();
    TwoChannelSource source (strip, inL, inR);
    RenderResult r;
    r.left.resize (inL.size());
    r.right.resize (inL.size());
    flub::LoudnessMeter meter;
    meter.prepare (kFs, 2);
    float peak = 0.0f;
    for (size_t pos = 0; pos + static_cast<size_t> (kBlock) <= inL.size(); pos += static_cast<size_t> (kBlock))
    {
        float* outs[] = { r.left.data() + pos, r.right.data() + pos };
        host.renderOffline (source, kBlock, outs, 2);
        meter.process (flub::AudioBlock (outs, 2, kBlock));
        for (int i = 0; i < kBlock; ++i)
            peak = std::max ({ peak, std::abs (outs[0][i]), std::abs (outs[1][i]) });
    }
    r.shortTermLufsAtEnd = meter.getShortTermLufs();
    r.peakDb = flubapptest::gainToDb (peak);
    return r;
}

/** Speech-like test signal: band-limited noise (about 150 Hz - 3.5 kHz) in
    4.3 Hz syllables of random strength, 2.4 s phrases with 0.8 s pauses.
    Deterministic. */
std::vector<float> speechLike (double seconds, uint32_t seed)
{
    const int n = static_cast<int> (seconds * kFs);
    auto x = flubtest::whiteNoise (n, 1.0f, seed);

    // Two one-pole high-passes at 150 Hz, two one-pole low-passes at 3.5 kHz.
    const double hp = std::exp (-flub::kTwoPi * 150.0 / kFs), lp = std::exp (-flub::kTwoPi * 3500.0 / kFs);
    double h1 = 0.0, h1x = 0.0, h2 = 0.0, h2x = 0.0, l1 = 0.0, l2 = 0.0;
    std::vector<double> amps (static_cast<size_t> (seconds * 4.3) + 2);
    uint32_t state = seed * 2654435761u + 1u;
    for (auto& a : amps)
    {
        state = state * 1664525u + 1013904223u;
        a = 0.3 + 0.7 * static_cast<double> (state >> 8) / static_cast<double> (1u << 24);
    }
    for (int i = 0; i < n; ++i)
    {
        const double in = x[static_cast<size_t> (i)];
        h1 = hp * (h1 + in - h1x);
        h1x = in;
        h2 = hp * (h2 + h1 - h2x);
        h2x = h1;
        l1 = (1.0 - lp) * h2 + lp * l1;
        l2 = (1.0 - lp) * l1 + lp * l2;

        const double t = i / kFs;
        const double syllable = std::pow (std::max (0.0, std::sin (flub::kTwoPi * 4.3 * t)), 1.5);
        const double phrase = std::fmod (t, 3.2) < 2.4 ? 1.0 : 0.0;
        x[static_cast<size_t> (i)] = static_cast<float> (l2 * syllable * phrase * amps[static_cast<size_t> (t * 4.3)]);
    }
    return x;
}

/** `x` scaled so the stereo pair (x, x) measures `lufs` integrated. */
std::vector<float> atLoudness (std::vector<float> x, double lufs)
{
    const double now = integratedLufs (x, x, 0);
    const auto g = static_cast<float> (std::pow (10.0, (lufs - now) / 20.0));
    for (auto& v : x)
        v *= g;
    return x;
}

bool writeText (const juce::File& file, const juce::String& text) { return file.replaceWithText (text, false, false, "\n"); }
} // namespace

// =============================================================================
// docs/11 E40: presets never write app state
// =============================================================================
TEST_CASE ("App: loading any factory preset leaves the latency profile, bypass and matched bypass alone and needs no re-prepare (E40)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp, false));
    auto& host = controller.getHost();
    const auto factory = controller.getPresetManager().getFactoryPresets();
    REQUIRE (factory.size() >= 25);

    for (const auto profile : { LatencyProfileValue::Quality, LatencyProfileValue::LowLatency })
    {
        controller.setLatencyProfile (profile);
        host.reconfigure();
        REQUIRE (! host.needsReprepare());

        // App state set to non-default values on both banks of every strip.
        for (int s = 0; s < controller.getNumStrips(); ++s)
            for (const auto bank : { Bank::A, Bank::B })
                controller.getParams (s).set (bank, LoudnessMatchBypass, 0.0f);
        controller.setEnabled (false); // Bypass All = 1 everywhere

        for (const auto& preset : factory)
        {
            for (int s = 0; s < controller.getNumStrips(); ++s)
            {
                juce::String error;
                REQUIRE (controller.loadPreset (preset, s, error));
                auto& store = controller.getParams (s);
                for (const auto bank : { Bank::A, Bank::B })
                {
                    CHECK (store.get (bank, LatencyProfile) == profileValue (profile));
                    CHECK (store.get (bank, BypassAll) == 1.0f);
                    CHECK (store.get (bank, LoudnessMatchBypass) == 0.0f);
                }
                CHECK (! controller.isPresetModified (s));
            }
            CHECK (! host.needsReprepare());
        }
        controller.setEnabled (true);
    }

    // The profile a preset was made for is kept as metadata for a prompt.
    const auto* fps = controller.getPresetManager().findById ("factory:gaming-competitive-fps");
    REQUIRE (fps != nullptr);
    CHECK (fps->suggestedLatencyProfile == std::optional<LatencyProfileValue> (LatencyProfileValue::LowLatency));
    const auto* signature = controller.getPresetManager().findById ("factory:music-flubsound-signature");
    REQUIRE (signature != nullptr);
    CHECK (! signature->suggestedLatencyProfile.has_value());
}

TEST_CASE ("App: a user preset that still carries latency.profile and bypass keys never applies them; saving writes no app state (E40)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp, false));
    auto& manager = controller.getPresetManager();
    auto& host = controller.getHost();
    const auto folder = temp.file ("Presets");
    REQUIRE (folder.createDirectory().wasOk());
    manager.setUserPresetFolder (folder);

    // A file saved before E40 (or edited by hand): Quality, bypassed, matched off.
    REQUIRE (writeText (folder.getChildFile ("Old Habit.flubpreset.json"),
                        R"({ "format": "flubsound-preset", "version": 2, "name": "Old Habit", "category": "User",
                             "params": { "boost": 0.42, "latency.profile": "Quality", "bypass": true, "bypass.matched": false } })"));
    manager.refresh();
    const auto* info = manager.findById ("user:Old Habit.flubpreset.json");
    REQUIRE (info != nullptr);
    CHECK (info->suggestedLatencyProfile == std::optional<LatencyProfileValue> (LatencyProfileValue::Quality));

    const int music = controller.findStrip ("Music");
    REQUIRE (music >= 0);
    auto& store = controller.getParams (music);
    REQUIRE (store.get (LatencyProfile) == profileValue (LatencyProfileValue::Balanced));
    const float matchedBefore = store.get (LoudnessMatchBypass);
    juce::String error;
    REQUIRE (controller.loadPreset (info->id, music, error));
    CHECK (store.get (BoostIntensity) == 0.42f); // the sound is applied ...
    for (const auto bank : { Bank::A, Bank::B })
    {
        CHECK (store.get (bank, LatencyProfile) == profileValue (LatencyProfileValue::Balanced)); // ... app state is not
        CHECK (store.get (bank, BypassAll) == 0.0f);
        CHECK (store.get (bank, LoudnessMatchBypass) == matchedBefore);
    }
    for (int s = 0; s < controller.getNumStrips(); ++s)
        CHECK (controller.getParams (s).get (LatencyProfile) == profileValue (LatencyProfileValue::Balanced));
    CHECK (! host.needsReprepare()); // the Game strip is not padded to a Quality Music strip

    // Saving from a strip in Low Latency: the profile becomes the suggestion,
    // and "params" carries no app-state key at all.
    controller.setLatencyProfile (LatencyProfileValue::LowLatency);
    controller.setEnabled (false);
    const auto id = controller.saveUserPreset ("Made Fast", "User", "", music, error);
    REQUIRE (id.isNotEmpty());
    controller.setEnabled (true);
    flub::json::Value root;
    std::string parseError;
    REQUIRE (flub::json::parse (folder.getChildFile ("Made Fast.flubpreset.json").loadFileAsString().toStdString(), root, parseError));
    CHECK (root["suggestedLatencyProfile"].asString() == "Low Latency");
    for (const char* key : { "latency.profile", "bypass", "bypass.matched" })
        CHECK (root["params"][key].isNull());
    CHECK (root["params"]["boost"].asNumber() == static_cast<double> (0.42f));
    const auto* saved = manager.findById (id);
    REQUIRE (saved != nullptr);
    CHECK (saved->suggestedLatencyProfile == std::optional<LatencyProfileValue> (LatencyProfileValue::LowLatency));
}

// =============================================================================
// docs/11 E36 / E23: first-run defaults
// =============================================================================
TEST_CASE ("App: fresh strips load their default preset - Signature on Music and System, Voice Chat on Chat, capped Competitive FPS on Game (E36)")
{
    const flubapptest::TempFolder temp;
    {
        EngineController controller (headlessOptions (temp, true));
        const int game = controller.findStrip ("Game"), music = controller.findStrip ("Music"), chat = controller.findStrip ("Chat"),
                  system = controller.findStrip ("System");
        REQUIRE (game >= 0);
        REQUIRE (music >= 0);
        REQUIRE (chat >= 0);
        REQUIRE (system >= 0);

        CHECK (controller.getCurrentPresetId (music) == "factory:music-flubsound-signature");
        CHECK (controller.getCurrentPresetId (system) == "factory:music-flubsound-signature");
        CHECK (controller.getCurrentPresetId (chat) == "factory:music-voice-chat");
        CHECK (controller.getCurrentPresetId (game) == "factory:gaming-competitive-fps");
        CHECK (! controller.isPresetModified (music));
        CHECK (! controller.isPresetModified (chat));
        CHECK (controller.isPresetModified (game)); // Competitive FPS with the first-run caps
        CHECK (controller.getMode (game) == ModeValue::Gaming);
        CHECK (controller.getMode (chat) == ModeValue::Music);

        auto& g = controller.getParams (game);
        for (const auto bank : { Bank::A, Bank::B }) // A/B start equal
        {
            CHECK (g.get (bank, BoostIntensity) < 0.25f); // Boost adds no maximizer drive below 0.25
            CHECK (g.get (bank, Macro1) <= 0.30f);          // Footsteps
            CHECK (g.get (bank, Macro4) <= 0.30f);          // Detail
            CHECK (g.get (bank, VirtInputMode) == static_cast<float> (static_cast<int> (InputModeValue::Auto))); // E27 fold follows the input
        }
        CHECK (g.get (Bank::A, Macro2) == 0.55f); // the rest is Competitive FPS

        // No default sets a latency profile: every strip stays Balanced.
        for (int s = 0; s < controller.getNumStrips(); ++s)
            for (const auto bank : { Bank::A, Bank::B })
                CHECK (controller.getParams (s).get (bank, LatencyProfile) == profileValue (LatencyProfileValue::Balanced));
    }

    // Without state restore (headless runs, the other tests): parameter
    // defaults as before, no preset.
    {
        const flubapptest::TempFolder other;
        EngineController controller (headlessOptions (other, false));
        CHECK (controller.getCurrentPresetId (controller.findStrip ("Chat")).isEmpty());
        CHECK (controller.getBoost (controller.findStrip ("Music")) == 0.0f);
    }
}

TEST_CASE ("App: first-run defaults never overwrite saved strip state (E36 / E23)")
{
    const flubapptest::TempFolder temp;
    {
        EngineController first (headlessOptions (temp, true, true));
        const int chat = first.findStrip ("Chat");
        juce::String error;
        REQUIRE (first.loadPreset ("factory:music-podcast-voice", chat, error));
        first.setBoost (0.77f, chat);
        first.shutdown();
    }
    EngineController second (headlessOptions (temp, true, true));
    const int chat = second.findStrip ("Chat");
    CHECK (second.getCurrentPresetId (chat) == "factory:music-podcast-voice");
    CHECK (second.getBoost (chat) == 0.77f);
    CHECK (second.getCurrentPresetId (second.findStrip ("Music")) == "factory:music-flubsound-signature"); // saved on the first run
    second.shutdown();
}

TEST_CASE ("App: Voice Chat on the Chat strip brings speech at -35 and -12 LUFS within 3 LU short-term, ceiling held (E23)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp, true));
    const int chat = controller.findStrip ("Chat");
    REQUIRE (controller.getCurrentPresetId (chat) == "factory:music-voice-chat");

    const auto speech = speechLike (30.0, 17);
    double ends[2] = {};
    int k = 0;
    for (const double lufs : { -35.0, -12.0 })
    {
        controller.getHost().reconfigure(); // a fresh engine: no level history from the previous run
        const auto in = atLoudness (speech, lufs);
        const auto out = renderThroughEngine (controller, chat, in, in);
        ends[k++] = out.shortTermLufsAtEnd;
        std::cerr << "    measured Voice Chat: speech at " << lufs << " LUFS ends at " << out.shortTermLufsAtEnd << " LUFS short-term, peak "
                  << out.peakDb << " dBFS\n";
        CHECK_LE (out.peakDb, -1.0 + 0.05);          // the -1 dBTP ceiling (sample peak)
        CHECK_GE (out.shortTermLufsAtEnd, -24.0);     // levelled towards -18 .. -20 LUFS ...
        CHECK_LE (out.shortTermLufsAtEnd, -15.0);
    }
    CHECK_LE (std::abs (ends[0] - ends[1]), 3.0); // ... from 23 LU apart to within 3 LU

    // No loudness maximizing: the maximizer is only its true-peak limiter
    // (values after Boost and the macros, as the chain ran them).
    auto& chain = controller.getChain (chat);
    CHECK (chain.effectiveValue (MaximizerOn) == 1.0f); // the ceiling guarantee stays
    CHECK (chain.effectiveValue (MaxDriveDb) == 0.0f);
    CHECK (chain.effectiveValue (MaxClipAmount) == 0.0f);
    CHECK (chain.effectiveValue (MaxGlue) == 0.0f);
    CHECK (chain.effectiveValue (SaturationOn) == 0.0f);
}

TEST_CASE ("App: First Run - Game lifts quiet pink beds by at most +3 LU and plays FL/FR-only input through the stereo passthrough fold (E36)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp, true));
    const int game = controller.findStrip ("Game");
    REQUIRE (game >= 0);
    REQUIRE (controller.getStripChannels (game) == 8);

    const int n = static_cast<int> (12.0 * kFs), from = static_cast<int> (6.0 * kFs);
    const auto lift = [&] (double rmsDb)
    {
        controller.getHost().reconfigure();
        const auto amp = static_cast<float> (std::pow (10.0, rmsDb / 20.0));
        const auto l = flubtest::pinkNoise (n, amp, 101), r = flubtest::pinkNoise (n, amp, 202);
        const auto out = renderThroughEngine (controller, game, l, r);
        CHECK (controller.getChain (game).meters().inputFold.load() == 1); // FL / FR only: stereo passthrough (E27)
        return integratedLufs (out.left, out.right, from) - integratedLufs (l, r, from);
    };

    for (const double bed : { -50.0, -60.0 })
    {
        const double lu = lift (bed);
        std::cerr << "    measured First Run - Game: " << bed << " dBFS pink bed lifted " << lu << " LU\n";
        CHECK_LE (lu, 3.0); // docs/11 E36 Done-when
    }

    // Competitive FPS as shipped fails the same measurement: the caps are what pass it.
    juce::String error;
    REQUIRE (controller.loadPreset ("factory:gaming-competitive-fps", game, error));
    const double shipped = lift (-60.0);
    std::cerr << "    measured Competitive FPS: -60 dBFS pink bed lifted " << shipped << " LU\n";
    CHECK_GE (shipped, 6.0);
}

// =============================================================================
// Start-up: the device-profile override
// =============================================================================
TEST_CASE ("App: a device-profiles.json override in the (non-ASCII) user data folder replaces the shipped profiles at start-up")
{
    // AppTestMain points the user data folder at a temporary folder with a
    // non-ASCII name (like a Windows profile "C:\Users\José"): the override is
    // read through juce::File, not a narrow std path.
    const auto folder = userDataFolder();
    REQUIRE (folder.createDirectory().wasOk());
    const auto file = folder.getChildFile ("device-profiles.json");
    REQUIRE (! file.exists());
    struct Remove
    {
        juce::File f;
        ~Remove() { f.deleteFile(); }
    } remove { file };

    REQUIRE (writeText (file, juce::String::fromUTF8 (R"({ "format": "flubsound-device-profiles", "version": 1, "profiles": [
        { "id": "test-héadset", "vendor": "Tëst", "family": "Probe", "displayName": "Tëst Probe headset", "matchAny": ["probe headset"],
          "specificity": 2, "typicalConnection": "usb" } ] })")));

    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp, false));
    const auto& profiles = controller.getDeviceProfiles().profiles();
    REQUIRE (profiles.size() == 1);
    CHECK (profiles[0].displayName == "T\xc3\xabst Probe headset");
    controller.simulateOutputDevice ("Probe Headset (USB)", 48000.0, 2);
    CHECK (controller.getDeviceProfileName() == juce::String::fromUTF8 ("T\xc3\xabst Probe headset"));

    // A damaged override falls back to the shipped database.
    REQUIRE (writeText (file, "{ not json"));
    EngineController fallback (headlessOptions (temp, false));
    CHECK (fallback.getDeviceProfiles().profiles().size() > 1);
}
