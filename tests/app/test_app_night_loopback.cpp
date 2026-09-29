// App-level tests for two EngineController follow-ups of docs/11 Phase 3:
// * E21: the Night listening hotkey latch copies the retuned Night Mode
//   Gaming dynamics (Auto Level -14 LUFS, no compressor make-up, Dynamic
//   Range 20 LU), read from the factory preset.
// * E51: the feedback-loop guard's per-pair override - allowed pairs are
//   persisted, applied to the host at start and removable; Settings > Audio's
//   text and "Allow this pair" candidate.
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "shell/ScreenshotDriver.h"
#include "ui/SettingsDialog.h"

#include "flub/engine/Parameters.h"
#include "flub/io/PresetIO.h"

#include <memory>

using namespace flub::app;
using namespace flub::param;

namespace
{
EngineController::Options headless (const flubapptest::TempFolder& temp, bool persist = false)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = persist;
    o.foregroundAppFactory = [] { return std::unique_ptr<flub::platform::ForegroundApp>(); };
    return o;
}

const juce::String kCableIn = "CABLE Input (VB-Audio Virtual Cable)", kCableOut = "CABLE Output (VB-Audio Virtual Cable)";
} // namespace

TEST_CASE ("App: Night listening copies the retuned Night Mode Gaming dynamics, the Dynamic Range included (E21)")
{
    const flubapptest::TempFolder temp;
    EngineController c (headless (temp));

    // The latch's values are the factory preset's.
    const auto* info = c.getPresetManager().findById ("factory:gaming-night-mode");
    REQUIRE (info != nullptr);
    flub::preset::Preset preset;
    juce::String error;
    REQUIRE (c.getPresetManager().readPreset (*info, preset, error));
    const auto overrides = c.getNightOverrides();
    REQUIRE (overrides.size() == 15);
    for (const auto& [id, value] : overrides)
        CHECK (value == preset.values[static_cast<size_t> (id)]);

    // ... which are the E21 Phase 3 retune, not the old Night dynamics
    // (-20 LUFS, +6 dB make-up at -24 dB, upward below -38 dB, no guard).
    const auto valueOf = [&overrides] (int id)
    {
        for (const auto& [i, v] : overrides)
            if (i == id)
                return v;
        return -1000.0f; // not overridden: fails every check below
    };
    CHECK (valueOf (AutoLevelOn) == 1.0f);
    CHECK (valueOf (AutoLevelTargetLufs) == -14.0f);
    CHECK (valueOf (GuardRange) == static_cast<float> (GuardRangeValue::Lu20));
    CHECK (valueOf (CompThresholdDb) == -18.0f);
    CHECK (valueOf (CompMakeupDb) == 0.0f);
    CHECK (valueOf (CompUpThresholdDb) == -32.0f);
    CHECK (valueOf (CompUpFloorDb) == -62.0f);
    CHECK (valueOf (CompUpMaxGainDb) == 6.0f);

    // Engaged on the Game strip (Competitive FPS with the first-run caps, or
    // defaults here): both banks; released, every value is back.
    const int game = c.findStrip ("Game");
    REQUIRE (game >= 0);
    auto& store = c.getParams (game);
    std::vector<float> beforeA, beforeB;
    for (const auto& [id, value] : overrides)
    {
        beforeA.push_back (store.get (Bank::A, id));
        beforeB.push_back (store.get (Bank::B, id));
    }
    c.setNight (game, true);
    CHECK (c.isNight (game));
    for (const auto& [id, value] : overrides)
        for (const auto bank : { Bank::A, Bank::B })
            CHECK (store.get (bank, id) == layout()[static_cast<size_t> (id)].clamp (value));
    c.setNight (game, false);
    for (size_t i = 0; i < overrides.size(); ++i)
    {
        CHECK (store.get (Bank::A, overrides[i].first) == beforeA[i]);
        CHECK (store.get (Bank::B, overrides[i].first) == beforeB[i]);
    }
}

TEST_CASE ("App: an allowed loopback pair plays, is persisted and applied at the next start; removing it mutes again (E51)")
{
    const flubapptest::TempFolder temp;
    {
        EngineController c (headless (temp, true));
        auto& host = c.getHost();
        host.setDeviceInputRouting (0, 0); // the device input feeds the Game strip
        host.checkLoopbackPair (kCableOut, kCableIn);
        REQUIRE (c.getDeviceSafetyState().kind == DeviceSafetyState::Kind::LoopbackPair);
        CHECK (host.isOutputMutedByGuard());

        // Settings > Audio offers exactly the muted pair.
        const auto candidate = ui::SettingsDialog::loopbackPairToAllow (c);
        CHECK (candidate.input == kCableOut);
        CHECK (candidate.output == kCableIn);
        CHECK (ui::SettingsDialog::describeLoopbackGuard (c).startsWith ("Output muted: \"" + kCableIn + "\" plays back into the input"));

        c.setLoopbackPairAllowed (kCableOut, kCableIn, true);
        CHECK (c.getDeviceSafetyState().kind == DeviceSafetyState::Kind::None);
        CHECK (! host.isOutputMutedByGuard());
        REQUIRE (c.getAllowedLoopbackPairs().size() == 1);
        CHECK (c.getAllowedLoopbackPairs()[0].input == kCableOut);
        CHECK (ui::SettingsDialog::loopbackPairToAllow (c).input.isEmpty()); // nothing left to allow
        c.setLoopbackPairAllowed (kCableOut, kCableIn, true);                 // allowing again adds nothing
        CHECK (c.getAllowedLoopbackPairs().size() == 1);
        c.saveState();
    }

    // Next start: the allowed pair is applied before any device check.
    EngineController c (headless (temp, true));
    auto& host = c.getHost();
    REQUIRE (c.getAllowedLoopbackPairs().size() == 1);
    host.setDeviceInputRouting (0, 0);
    host.checkLoopbackPair (kCableOut, kCableIn);
    CHECK (c.getDeviceSafetyState().kind == DeviceSafetyState::Kind::None);
    CHECK (! host.isOutputMutedByGuard());

    // Another pair is still guarded.
    host.checkLoopbackPair ("BlackHole 2ch", "BlackHole 2ch");
    CHECK (c.getDeviceSafetyState().kind == DeviceSafetyState::Kind::LoopbackPair);
    host.checkLoopbackPair (kCableOut, kCableIn);
    CHECK (c.getDeviceSafetyState().kind == DeviceSafetyState::Kind::None);

    // Removed (names compare ignoring case): the pair is muted again at once.
    c.setLoopbackPairAllowed (kCableOut.toUpperCase(), kCableIn.toLowerCase(), false);
    CHECK (c.getAllowedLoopbackPairs().empty());
    CHECK (c.getDeviceSafetyState().kind == DeviceSafetyState::Kind::LoopbackPair);
    CHECK (host.isOutputMutedByGuard());

    // Empty names are ignored.
    c.setLoopbackPairAllowed ({}, kCableIn, true);
    CHECK (c.getAllowedLoopbackPairs().empty());
}

TEST_CASE ("App settings: allowed loopback pairs round-trip, blank entries are dropped (E51)")
{
    const flubapptest::TempFolder temp;
    const auto file = temp.file ("settings.xml");
    {
        AppSettings settings (file, true);
        CHECK (settings.getAllowedLoopbackPairs().empty());
        settings.setAllowedLoopbackPairs ({ { " CABLE Output ", "CABLE Input" }, { "", "x" }, { "BlackHole 2ch", "BlackHole 2ch" } });
        settings.save();
    }
    AppSettings settings (file, true);
    const auto pairs = settings.getAllowedLoopbackPairs();
    REQUIRE (pairs.size() == 2);
    CHECK ((pairs[0] == AppSettings::LoopbackPair { "CABLE Output", "CABLE Input" }));
    CHECK ((pairs[1] == AppSettings::LoopbackPair { "BlackHole 2ch", "BlackHole 2ch" }));
}

TEST_CASE ("App UI: the screenshot driver accepts --state settings-audio and settings-processing (E51 / E32)")
{
    ScreenshotDriver::Options o;
    juce::String error;
    CHECK (ScreenshotDriver::parseCommandLine (juce::StringArray ({ "--screenshot", "a.png", "--state", "loopback,settings-audio" }), o, error));
    CHECK (o.states.contains ("settings-audio"));
    ScreenshotDriver::Options p;
    CHECK (ScreenshotDriver::parseCommandLine (juce::StringArray ({ "--screenshot", "a.png", "--state", "settings-processing" }), p, error));
    CHECK (p.states.contains ("settings-processing"));
    ScreenshotDriver::Options q;
    CHECK (! ScreenshotDriver::parseCommandLine (juce::StringArray ({ "--screenshot", "a.png", "--state", "settings-hotkeys" }), q, error));
    CHECK (error.contains ("settings-processing"));
}
