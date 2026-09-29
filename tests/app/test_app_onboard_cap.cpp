// App-level tests: "Headset enhancement (Superhuman Hearing / on-board EQ)
// is ON" (docs/11 E16, the app half). The core half caps a chain
// (ProcessingChain::setOnboardEnhancementCap, tests/test_onboard_cap.cpp);
// here the app decides when:
//   * EngineController keeps the user's answer per output endpoint in the
//     settings (AppSettings::setDeviceEndpoint), keyed by the endpoint's
//     identity (docs/11 E51: endpoint id, hardware id, the name as the
//     fallback), and applies or removes it on every output change
//     (simulateOutputDevice stands in for the device, Options::
//     outputEndpoints for the platform's endpoint list) and on every engine
//     the host builds.
//   * The device banner asks for a profile with on-board DSP until the user
//     answers; the Settings > Audio page has the same switch; the Boost
//     panel shows CAPPED on Footsteps and Detail while the chain caps.
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "shell/ScreenshotDriver.h"
#include "ui/BoostPanel.h"
#include "ui/DeviceAdviceBanner.h"
#include "ui/SettingsDialog.h"

#include "flub/engine/MacroMap.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace flub::app;
using namespace flub::param;

namespace
{
constexpr const char* kHeadset = "Headset Earphone (Stealth 700 Gen 2)";
constexpr const char* kHeadsetReplugged = "Headset Earphone (2- Stealth 700 Gen 2)"; // Windows' instance number: another USB port
constexpr const char* kHeadsetHardware = "USB\\VID_10F5&PID_0210&MI_00";
constexpr const char* kSpeakers = "Speakers (Realtek(R) Audio)";
constexpr const char* kAtlas = "Headphones (Elite Atlas Aero)";

using Endpoints = std::vector<flub::platform::OutputEndpointIdentity>;

/** What the platform lists; the test changes it between device changes. */
struct FakeEndpoints
{
    Endpoints now;
};

EngineController::Options headlessOptions (const flubapptest::TempFolder& temp, std::shared_ptr<FakeEndpoints> endpoints, bool persist = false)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = persist;
    o.outputEndpoints = [endpoints] { return endpoints != nullptr ? endpoints->now : Endpoints {}; };
    return o;
}

flub::platform::OutputEndpointIdentity endpoint (const char* id, const char* name, const char* hardwareId)
{
    flub::platform::OutputEndpointIdentity e;
    e.id = id;
    e.name = name;
    e.hardwareId = hardwareId;
    e.transport = flub::platform::EndpointTransport::Usb;
    return e;
}

/** Quiet noise on every channel of one strip. */
class NoiseSource final : public StripSignalSource
{
public:
    explicit NoiseSource (int stripIndex) : strip (stripIndex) {}

    bool renderStrip (int s, const flub::AudioBlock& block) override
    {
        if (s != strip)
            return false;
        for (int c = 0; c < block.numChannels; ++c)
            for (int i = 0; i < block.numSamples; ++i)
            {
                state = state * 1664525u + 1013904223u;
                block.channel (c)[i] = 0.05f * (static_cast<float> (state >> 8) / static_cast<float> (1u << 24) - 0.5f);
            }
        return true;
    }

private:
    int strip;
    uint32_t state = 12345u;
};

/** Plays `seconds` of noise on `strip` (the cap's 250 ms glide settles in 0.4 s). */
void play (EngineController& controller, int strip, double seconds = 0.4)
{
    NoiseSource source (strip);
    controller.renderOffline (source, static_cast<int> (seconds * controller.getHost().getSampleRate()));
}

bool everyChainCapped (EngineController& controller, bool capped)
{
    bool all = true;
    for (int s = 0; s < controller.getNumStrips(); ++s)
        all = all && controller.getChain (s).getOnboardEnhancementCap() == capped;
    return all;
}

template <typename T>
T* findChild (juce::Component& root, const std::function<bool (T&)>& match)
{
    for (auto* child : root.getChildren())
    {
        if (auto* t = dynamic_cast<T*> (child); t != nullptr && match (*t))
            return t;
        if (auto* found = findChild<T> (*child, match))
            return found;
    }
    return nullptr;
}
} // namespace

TEST_CASE ("App: headset enhancement (E16): offered for an on-board-DSP headset; 'on' caps every strip; a simulated device change removes and re-applies it")
{
    const flubapptest::TempFolder temp;
    auto endpoints = std::make_shared<FakeEndpoints>();
    endpoints->now = { endpoint ("{0.0.0.00000000}.{aaaa}", kHeadset, kHeadsetHardware),
                       endpoint ("{0.0.0.00000000}.{bbbb}", kSpeakers, "HDAUDIO\\FUNC_01&VEN_10EC&DEV_0897") };
    EngineController controller (headlessOptions (temp, endpoints));
    const int game = controller.findStrip ("Game");
    REQUIRE (game >= 0);
    REQUIRE (controller.getMode (game) == ModeValue::Gaming);
    auto& store = controller.getParams (game);
    store.set (Macro1, 1.0f); // Footsteps 100
    store.set (Macro4, 1.0f); // Detail 100
    store.set (VirtualizerOn, 1.0f);

    // No output: nothing to answer.
    CHECK (controller.getOnboardEnhancement().endpoint.isEmpty());
    CHECK (! controller.setOnboardEnhancement (true));
    CHECK (everyChainCapped (controller, false));

    // A Stealth headset (Superhuman Hearing): offered, not yet answered, not applied.
    controller.simulateOutputDevice (kHeadset, 48000.0, 2);
    auto info = controller.getOnboardEnhancement();
    CHECK (info.endpoint == kHeadset);
    CHECK (info.offered);
    CHECK (! info.answered);
    CHECK (! info.on);
    CHECK (controller.getOutputIdentity().id == "{0.0.0.00000000}.{aaaa}");
    CHECK (controller.getOutputIdentity().hardwareId == kHeadsetHardware);
    CHECK (everyChainCapped (controller, false));

    // "Yes, it is ON": every strip's chain caps at once, the store keeps its values.
    REQUIRE (controller.setOnboardEnhancement (true));
    info = controller.getOnboardEnhancement();
    CHECK (info.answered);
    CHECK (info.on);
    CHECK (controller.isOnboardCapApplied());
    CHECK (everyChainCapped (controller, true));
    play (controller, game);
    auto& chain = controller.getChain (game);
    CHECK (chain.meters().onboardCapActive.load());
    CHECK (chain.effectiveValue (Macro1) == flub::MacroMap::kOnboardCapMacroLimit);
    CHECK (chain.effectiveValue (Macro4) == flub::MacroMap::kOnboardCapMacroLimit);
    CHECK (chain.effectiveValue (VirtualizerOn) == 0.0f);
    CHECK (store.get (Macro1) == 1.0f);
    CHECK (store.get (VirtualizerOn) == 1.0f);
    const auto& stored = controller.getSettings().getDeviceEndpoints();
    REQUIRE (stored.size() == 1);
    CHECK (stored.front().endpointId == "{0.0.0.00000000}.{aaaa}");
    CHECK (stored.front().hardwareId == kHeadsetHardware);
    CHECK (stored.front().name == kHeadset);
    CHECK (stored.front().onboardEnhancement);

    // The output changes to speakers: removed (and not offered there).
    controller.simulateOutputDevice (kSpeakers, 48000.0, 2);
    info = controller.getOnboardEnhancement();
    CHECK (! info.offered);
    CHECK (! info.answered);
    CHECK (! info.on);
    CHECK (everyChainCapped (controller, false));
    play (controller, game);
    CHECK (! controller.getChain (game).meters().onboardCapActive.load());
    CHECK (controller.getChain (game).effectiveValue (Macro1) == 1.0f);
    CHECK (controller.getChain (game).effectiveValue (VirtualizerOn) == 1.0f);

    // Back to the headset: re-applied from the stored answer.
    controller.simulateOutputDevice (kHeadset, 48000.0, 2);
    CHECK (controller.getOnboardEnhancement().on);
    CHECK (everyChainCapped (controller, true));

    // Engines the host builds start capped too: a reconfiguration and a new strip layout.
    controller.getHost().reconfigure();
    CHECK (everyChainCapped (controller, true));
    controller.setStripLayout (AudioEngineHost::defaultStripLayout());
    CHECK (everyChainCapped (controller, true));
    play (controller, game);
    CHECK (controller.getChain (game).meters().onboardCapActive.load());

    // Another headset answered "No": stored, not capped; the first keeps its "on".
    controller.simulateOutputDevice (kAtlas, 48000.0, 2);
    CHECK (controller.getOnboardEnhancement().offered);
    CHECK (! controller.getOnboardEnhancement().answered);
    REQUIRE (controller.setOnboardEnhancement (false));
    CHECK (controller.getOnboardEnhancement().answered);
    CHECK (! controller.getOnboardEnhancement().on);
    CHECK (everyChainCapped (controller, false));
    CHECK (controller.getSettings().getDeviceEndpoints().size() == 2);
    controller.simulateOutputDevice (kHeadset, 48000.0, 2);
    CHECK (everyChainCapped (controller, true));

    // "Off" for the headset removes it at once.
    REQUIRE (controller.setOnboardEnhancement (false));
    CHECK (everyChainCapped (controller, false));
    CHECK (controller.getSettings().getDeviceEndpoints().size() == 2);
}

TEST_CASE ("App: headset enhancement (E16): the answer persists per endpoint and survives a Windows '2- ' re-plug, a rename and no endpoint ids")
{
    const flubapptest::TempFolder temp;
    auto endpoints = std::make_shared<FakeEndpoints>();
    {
        endpoints->now = { endpoint ("{0.0.0.00000000}.{aaaa}", kHeadset, kHeadsetHardware) };
        EngineController first (headlessOptions (temp, endpoints, true));
        first.simulateOutputDevice (kHeadset, 48000.0, 2);
        REQUIRE (first.setOnboardEnhancement (true));
        first.shutdown();
    }

    // Restart with the dongle in another USB port: a new endpoint id and
    // Windows' "2- " in its name, the same hardware id.
    {
        endpoints->now = { endpoint ("{0.0.0.00000000}.{cccc}", kHeadsetReplugged, kHeadsetHardware) };
        EngineController replugged (headlessOptions (temp, endpoints, true));
        CHECK (everyChainCapped (replugged, false)); // no output yet
        replugged.simulateOutputDevice (kHeadsetReplugged, 48000.0, 2);
        CHECK (replugged.getOnboardEnhancement().answered);
        CHECK (replugged.getOnboardEnhancement().on);
        CHECK (everyChainCapped (replugged, true));

        // Answered again there: the one entry follows the endpoint's new identity.
        REQUIRE (replugged.setOnboardEnhancement (true));
        const auto stored = replugged.getSettings().getDeviceEndpoints();
        REQUIRE (stored.size() == 1);
        CHECK (stored.front().endpointId == "{0.0.0.00000000}.{cccc}");
        CHECK (stored.front().name == kHeadsetReplugged);
        replugged.shutdown();
    }

    // The same endpoint id under another name (a rename in Windows' Sound settings).
    {
        endpoints->now = { endpoint ("{0.0.0.00000000}.{cccc}", "Game headset", kHeadsetHardware) };
        EngineController renamed (headlessOptions (temp, endpoints));
        renamed.simulateOutputDevice ("Game headset", 48000.0, 2);
        CHECK (renamed.getOnboardEnhancement().on);
        CHECK (everyChainCapped (renamed, true));
    }

    // No endpoint ids (macOS, Linux): the name without the instance number.
    {
        EngineController byName (headlessOptions (temp, nullptr));
        byName.simulateOutputDevice ("Headset Earphone (3- Stealth 700 Gen 2)", 48000.0, 2);
        CHECK (byName.getOutputIdentity().id.empty());
        CHECK (byName.getOnboardEnhancement().on);
        CHECK (everyChainCapped (byName, true));
    }

    // Another product that happens to carry the name (another hardware id): not this answer.
    {
        endpoints->now = { endpoint ("{0.0.0.00000000}.{dddd}", kHeadset, "USB\\VID_1234&PID_5678&MI_00") };
        EngineController other (headlessOptions (temp, endpoints));
        other.simulateOutputDevice (kHeadset, 48000.0, 2);
        CHECK (! other.getOnboardEnhancement().answered);
        CHECK (everyChainCapped (other, false));
    }
}

TEST_CASE ("App: headset enhancement (E16): the device banner asks until answered; Settings > Audio switches it; the Boost panel shows CAPPED")
{
    const flubapptest::TempFolder temp;
    auto endpoints = std::make_shared<FakeEndpoints>();
    EngineController controller (headlessOptions (temp, endpoints));
    const int game = controller.findStrip ("Game");
    REQUIRE (game >= 0);
    controller.setSelectedStrip (game);

    // Banner: a generic output never asks; the Stealth does, instead of the preset offer.
    ui::DeviceAdviceBanner banner (controller);
    banner.setSize (1100, ui::DeviceAdviceBanner::kHeight);
    controller.simulateOutputDevice (kSpeakers, 48000.0, 2);
    banner.refresh();
    CHECK (! banner.isAskingEnhancement());
    controller.simulateOutputDevice (kHeadset, 48000.0, 2);
    banner.refresh();
    CHECK (banner.shouldShow());
    CHECK (banner.isAskingEnhancement());
    CHECK (banner.getEnhancementOnButton().isVisible());
    CHECK (banner.getEnhancementOffButton().isVisible());
    CHECK (banner.getAdviceText().contains ("Superhuman Hearing"));
    CHECK (! banner.getEnhancementOnButton().getBounds().isEmpty());
    CHECK (! banner.getEnhancementOnButton().getBounds().intersects (banner.getEnhancementOffButton().getBounds()));

    // Boost panel before the answer: no chip.
    ui::BoostPanel panel (controller);
    panel.setSize (960, 250);
    panel.setMode (ModeValue::Gaming);
    play (controller, game);
    panel.update ({});
    CHECK (! panel.getCappedChip (0).isVisible());
    CHECK (! panel.getCappedChip (1).isVisible());

    // "Yes, it is ON" (the button's own handler).
    banner.getEnhancementOnButton().onClick();
    CHECK (controller.isOnboardCapApplied());
    CHECK (! banner.isAskingEnhancement());
    CHECK (! banner.getEnhancementOnButton().isVisible());
    CHECK (banner.getAdviceText().startsWith ("Headset enhancement is ON"));

    // The chips beside Footsteps and Detail, inside the panel, in Gaming mode only.
    play (controller, game);
    panel.update ({});
    for (int i = 0; i < 2; ++i)
    {
        const auto& chip = panel.getCappedChip (i);
        CHECK (chip.isVisible());
        CHECK (chip.getButtonText().startsWith ("CAP")); // "CAPPED", or "CAP" in a narrow cell
        CHECK (panel.getLocalBounds().contains (chip.getBounds()));
    }
    CHECK (panel.getCappedChip (0).getX() < panel.getCappedChip (1).getX()); // Footsteps (macro 1) left of Detail (macro 4)
    panel.setMode (ModeValue::Music);
    CHECK (! panel.getCappedChip (0).isVisible());
    panel.setMode (ModeValue::Gaming);
    CHECK (panel.getCappedChip (0).isVisible());

    // Settings > Audio: the switch shows the answer and changes it.
    ui::HotkeyHooks hooks;
    hooks.isSupported = [] { return false; };
    hooks.getFailures = [] { return juce::StringArray(); };
    hooks.reRegister = [] {};
    ui::SettingsDialog settings (controller, hooks, [] (ui::MeterPalette) {}, ui::MeterPalette::Standard);
    settings.setSize (ui::SettingsDialog::kMinWidth, ui::SettingsDialog::kMinHeight);
    settings.showPage (ui::SettingsDialog::Page::Audio);
    auto* toggle = findChild<juce::ToggleButton> (settings, [] (juce::ToggleButton& b)
                                                  { return b.getButtonText() == "Headset enhancement (Superhuman Hearing / on-board EQ) is ON"; });
    REQUIRE (toggle != nullptr);
    CHECK (toggle->isEnabled());
    CHECK (toggle->getToggleState());
    CHECK (toggle->getWidth() > 200);
    toggle->triggerClick();
    flubapptest::pumpMessagesUntil ([&] { return ! controller.isOnboardCapApplied(); }, 2000);
    CHECK (! controller.isOnboardCapApplied());
    CHECK (everyChainCapped (controller, false));

    // The glide out ends; then the chips go.
    play (controller, game);
    panel.update ({});
    CHECK (! panel.getCappedChip (0).isVisible());
    CHECK (! panel.getCappedChip (1).isVisible());

    // The screenshot driver's state for it parses.
    ScreenshotDriver::Options options;
    juce::String error;
    CHECK (ScreenshotDriver::parseCommandLine ({ "--screenshot", "out.png", "--mode", "gaming", "--state", "onboard-cap,settings-audio" }, options, error));
    CHECK (options.states.contains ("onboard-cap"));
}
