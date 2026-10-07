// App-level tests: HotkeyManager's per-action registration status (R4.4)
// without an OS, and what the actions do (docs/11 E56: the hotkey strip,
// Focus, ChatMix, Night, Bypass). HotkeyManager runs with a fake platform::GlobalHotkeys
// (injected through its second constructor) that answers either at once, as
// Windows / macOS / X11 do, or later from another thread, as the Wayland
// GlobalShortcuts portal does. The message-thread hand-off is the real one;
// the test pumps the message loop until the expected state is published (the
// timeouts are hang guards).
//
// R4.4 conflicts (2026-10-07): chords the fake says another application
// holds, Flubsound's own duplicates and invalid chords, "Pick a free
// combination", the once-only notice, recording a chord on the Settings >
// Hotkeys page (key presses delivered straight to the recorder, no OS
// events), the new Bypass default and its persistence; on Windows also the
// real RegisterHotKey service against a second registrant in this process.
// After the review: every control on the page ends a recording before the
// hotkeys are registered again, chords other applications use are refused,
// the notice lists what still fails, and with a display the Settings window
// is really open on the Hotkeys page (its 4 Hz poll included).
#include "AppTestSupport.h"
#include "DisplayTestSupport.h"

#include "engine/EngineController.h"
#include "platform/PlatformBridge.h"
#include "settings/AppSettings.h"
#include "shell/HotkeyManager.h"
#include "ui/HotkeyCapture.h"
#include "ui/MainComponent.h"
#include "ui/NoticeBanners.h"
#include "ui/SettingsDialog.h"
#include "ui/Widgets.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace flub::app;
using flub::platform::GlobalHotkeys;
using flub::platform::KeyChord;
using Result = GlobalHotkeys::BindingResult;

namespace
{
/** Accepts every chord except the 'taken' ids. Synchronous mode reports
    each registration before returning (Windows / macOS / X11); asynchronous
    mode leaves the answer to reportFromAnotherThread (Wayland portal).
    Owned by the HotkeyManager and used on the test's (message) thread. */
class FakeHotkeys final : public GlobalHotkeys
{
public:
    using GlobalHotkeys::registerHotkey;

    bool isSupported() const override { return supported; }

    bool registerHotkey (int id, const KeyChord& chord, const std::string& description, std::function<void()> callback) override
    {
        descriptions[id] = description;
        ++attempts;
        const auto text = AppSettings::chordToString (chord).toStdString();
        if (taken.count (id) != 0 || takenChords.count (text) != 0)
        {
            reportBinding (id, Result::Status::Unavailable);
            return false;
        }
        callbacks[id] = std::move (callback);
        chords[id] = text;
        if (! asynchronous)
            reportBinding (id, Result::Status::Registered);
        return true;
    }

    void unregisterHotkey (int id) override
    {
        callbacks.erase (id);
        chords.erase (id);
    }
    void unregisterAll() override
    {
        callbacks.clear();
        chords.clear();
    }

    /** Reports from a worker thread, as the portal's D-Bus thread does; the
        worker is joined before this returns. */
    void reportFromAnotherThread (const std::vector<Result>& results)
    {
        std::thread worker ([this, results]
                            {
                                for (const auto& r : results)
                                    reportBinding (r.id, r.status, r.trigger);
                            });
        worker.join();
    }

    bool supported = true;
    bool asynchronous = false;
    std::set<int> taken;                // ids refused ("another application owns it")
    std::set<std::string> takenChords;  // chords another application holds ("Ctrl+Alt+B")
    std::map<int, std::string> descriptions;
    std::map<int, std::function<void()>> callbacks;
    std::map<int, std::string> chords;  // registered now
    int attempts = 0;                   // registerHotkey calls
};

EngineController::Options headlessOptions (const flubapptest::TempFolder& temp)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = false;
    return o;
}

int idOf (HotkeyAction action) { return static_cast<int> (action); }

juce::String chordOf (EngineController& controller, HotkeyAction action)
{
    return AppSettings::chordToString (controller.getSettings().getHotkey (action));
}

/** A foreground application that is always `path` (automatic profiles). */
class FixedForegroundApp final : public flub::platform::ForegroundApp
{
public:
    explicit FixedForegroundApp (std::string p) : path (std::move (p)) {}
    bool isSupported() const override { return true; }
    bool query (flub::platform::ForegroundAppInfo& info) override
    {
        info = {};
        info.processId = 4242;
        info.executablePath = path;
        info.executableName = path;
        return true;
    }
    std::string unsupportedReason() const override { return {}; }

private:
    std::string path;
};

float boostOf (EngineController& controller, const char* strip) { return controller.getParams (controller.findStrip (strip)).get (flub::param::BoostIntensity); }

/** Runs the message loop until every message posted so far has been handled. */
void drainMessages()
{
    bool reached = false;
    juce::MessageManager::callAsync ([&reached] { reached = true; });
    CHECK (flubapptest::pumpMessagesUntil ([&reached] { return reached; }));
}
} // namespace

TEST_CASE ("App: HotkeyManager registers each action under its name and shows per-action status (registered, in use, not assigned, off, not supported)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    auto& settings = controller.getSettings();
    settings.setHotkeysEnabled (true);
    for (const auto action : AppSettings::getAllHotkeyActions())
        settings.setHotkey (action, AppSettings::getDefaultHotkey (action));
    settings.setHotkey (HotkeyAction::NextPreset, {}); // unassigned

    auto service = std::make_unique<FakeHotkeys>();
    auto& fake = *service;
    fake.taken = { idOf (HotkeyAction::BoostDown) }; // "another application owns it"
    HotkeyManager manager (controller, std::move (service));
    int changes = 0;
    manager.onStatusChanged = [&changes] { ++changes; };
    CHECK (manager.isSupported());
    CHECK (manager.getStatusText (HotkeyAction::ToggleEnable) == "Not assigned"); // before registerAll()

    manager.registerAll();
    CHECK (changes > 0);

    // Every assigned action is registered under its name, so desktops that
    // list shortcuts (the Wayland portal) show "Boost +10%", not the chord.
    for (const auto action : AppSettings::getAllHotkeyActions())
    {
        if (action == HotkeyAction::NextPreset)
            CHECK (fake.descriptions.count (idOf (action)) == 0);
        else
            CHECK (fake.descriptions[idOf (action)] == AppSettings::getHotkeyActionName (action).toStdString());
    }
    CHECK (fake.descriptions[idOf (HotkeyAction::BoostUp)] == "Boost +10%");

    // Synchronous answers are applied before registerAll() returns.
    CHECK (manager.getStatusText (HotkeyAction::ToggleEnable) == "Registered");
    CHECK (manager.getStatusText (HotkeyAction::BoostUp) == "Registered");
    CHECK (manager.getStatusText (HotkeyAction::BoostDown) == "Could not register");
    CHECK (manager.getStatusText (HotkeyAction::NextPreset) == "Not assigned");
    CHECK (manager.getStatus (HotkeyAction::BoostDown).status == HotkeyManager::Status::Unavailable);
    CHECK (manager.getStatus (HotkeyAction::BoostDown).chord == chordOf (controller, HotkeyAction::BoostDown));
    CHECK (manager.getFailures()
           == juce::StringArray { "Boost -10% (" + chordOf (controller, HotkeyAction::BoostDown)
                                  + ") could not be registered: another application may already use it, or the system does not allow that key" });

    // A registered chord runs its action.
    juce::String feedback;
    manager.onActionPerformed = [&feedback] (HotkeyAction, const juce::String& text) { feedback = text; };
    const bool wasEnabled = controller.isEnabled();
    REQUIRE (fake.callbacks.count (idOf (HotkeyAction::ToggleEnable)) == 1);
    fake.callbacks[idOf (HotkeyAction::ToggleEnable)]();
    CHECK (controller.isEnabled() != wasEnabled);
    CHECK (feedback.isNotEmpty());

    // Hotkeys switched off: nothing is registered, every assigned row is "Off".
    settings.setHotkeysEnabled (false);
    manager.registerAll();
    CHECK (fake.callbacks.empty());
    CHECK (manager.getStatusText (HotkeyAction::ToggleEnable) == "Off");
    CHECK (manager.getStatusText (HotkeyAction::BoostDown) == "Off");
    CHECK (manager.getStatusText (HotkeyAction::NextPreset) == "Not assigned");
    CHECK (manager.getFailures().isEmpty());
    settings.setHotkeysEnabled (true);

    // No service at all, or one that is unsupported here.
    for (const bool withService : { false, true })
    {
        auto unsupported = std::make_unique<FakeHotkeys>();
        unsupported->supported = false;
        HotkeyManager none (controller, withService ? std::move (unsupported) : nullptr);
        CHECK (! none.isSupported());
        none.registerAll();
        CHECK (none.getStatusText (HotkeyAction::ToggleMode) == "Not supported here");
        CHECK (none.getFailures() == juce::StringArray { "Global hotkeys are not supported on this system" });
    }
}

TEST_CASE ("App: HotkeyManager shows the desktop's later answers per action (waiting, bound as another key, declined) on the message thread")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    auto& settings = controller.getSettings();
    settings.setHotkeysEnabled (true);
    for (const auto action : AppSettings::getAllHotkeyActions())
        settings.setHotkey (action, AppSettings::getDefaultHotkey (action));
    settings.setHotkey (HotkeyAction::NextPreset, {});

    auto service = std::make_unique<FakeHotkeys>();
    auto& fake = *service;
    fake.asynchronous = true;
    HotkeyManager manager (controller, std::move (service));
    int changes = 0;
    manager.onStatusChanged = [&changes] { ++changes; };

    manager.registerAll();
    for (const auto action : { HotkeyAction::ToggleEnable, HotkeyAction::ToggleMode, HotkeyAction::BoostUp, HotkeyAction::BoostDown })
        CHECK (manager.getStatusText (action) == "Waiting for the desktop");
    CHECK (manager.getFailures().isEmpty());

    // The answers come from the service's thread: they are posted to the
    // message thread, not applied on the reporting thread.
    changes = 0;
    fake.reportFromAnotherThread ({
        { idOf (HotkeyAction::ToggleEnable), Result::Status::Registered, {} },
        { idOf (HotkeyAction::BoostUp), Result::Status::Reassigned, "Ctrl+Alt+PgUp" },
        { idOf (HotkeyAction::ToggleMode), Result::Status::Declined, {} },
        { idOf (HotkeyAction::NextPreset), Result::Status::Registered, {} }, // not requested: ignored
    });
    CHECK (manager.getStatusText (HotkeyAction::ToggleEnable) == "Waiting for the desktop");
    CHECK (changes == 0);
    drainMessages();

    CHECK (changes == 3);
    CHECK (manager.getStatusText (HotkeyAction::ToggleEnable) == "Registered");
    CHECK (manager.getStatusText (HotkeyAction::BoostUp) == "Bound by the desktop as Ctrl+Alt+PgUp");
    CHECK (manager.getStatus (HotkeyAction::BoostUp).trigger == "Ctrl+Alt+PgUp");
    CHECK (manager.getStatusText (HotkeyAction::ToggleMode) == "Declined by the desktop");
    CHECK (manager.getStatusText (HotkeyAction::BoostDown) == "Waiting for the desktop");
    CHECK (manager.getStatusText (HotkeyAction::NextPreset) == "Not assigned");
    CHECK (manager.getFailures()
           == juce::StringArray { "Toggle Music / Gaming (" + chordOf (controller, HotkeyAction::ToggleMode)
                                  + ") was declined by the desktop: bind it in the desktop's keyboard settings, or choose another chord" });

    // The same answer again (a batch that needed no new binding) changes nothing.
    fake.reportFromAnotherThread ({ { idOf (HotkeyAction::ToggleEnable), Result::Status::Registered, {} } });
    drainMessages();
    CHECK (changes == 3);

    // A refusal is final for its registerAll(): an answer to an earlier one,
    // still queued for the message thread, does not overwrite it.
    fake.reportFromAnotherThread ({ { idOf (HotkeyAction::BoostDown), Result::Status::Registered, {} } }); // posted, not yet run
    fake.taken = { idOf (HotkeyAction::BoostDown) };
    manager.registerAll();
    CHECK (manager.getStatusText (HotkeyAction::BoostDown) == "Could not register");
    drainMessages();
    CHECK (manager.getStatusText (HotkeyAction::BoostDown) == "Could not register");
    fake.taken.clear();

    // Answers for actions no longer requested (hotkeys switched off since) are dropped.
    settings.setHotkeysEnabled (false);
    manager.registerAll();
    fake.reportFromAnotherThread ({ { idOf (HotkeyAction::BoostDown), Result::Status::Registered, {} } });
    drainMessages();
    CHECK (manager.getStatusText (HotkeyAction::BoostDown) == "Off");

    // An answer still queued when its manager is destroyed is dropped safely.
    {
        auto lateService = std::make_unique<FakeHotkeys>();
        auto& late = *lateService;
        late.asynchronous = true;
        settings.setHotkeysEnabled (true);
        auto doomed = std::make_unique<HotkeyManager> (controller, std::move (lateService));
        doomed->registerAll();
        late.reportFromAnotherThread ({ { idOf (HotkeyAction::ToggleEnable), Result::Status::Declined, {} } });
        doomed.reset();
    }
    drainMessages();
}

// =============================================================================
// docs/11 E56 Phase A: what the actions do
// =============================================================================
TEST_CASE ("App: hotkeys act on the hotkey strip, never the GUI selection, and the feedback names it (E56)")
{
    using flub::param::ModeValue;
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    HotkeyManager manager (controller, std::make_unique<FakeHotkeys>());
    juce::String feedback;
    manager.onActionPerformed = [&feedback] (HotkeyAction, const juce::String& text) { feedback = text; };

    const int game = controller.findStrip ("Game"), music = controller.findStrip ("Music"), chat = controller.findStrip ("Chat");
    REQUIRE (game >= 0);
    REQUIRE (music >= 0);
    REQUIRE (chat >= 0);
    controller.setSelectedStrip (music); // the last strip clicked in the window
    CHECK (controller.getSettings().getHotkeyStripName() == "Game");
    CHECK (controller.getHotkeyStrip() == game);

    // Done-when: selection = Music, hotkey strip = Game: Boost+ changes only Game.
    const float musicBoost = boostOf (controller, "Music"), chatBoost = boostOf (controller, "Chat");
    controller.setBoost (0.5f, game);
    manager.perform (HotkeyAction::BoostUp);
    CHECK (boostOf (controller, "Game") == 0.6f);
    CHECK (boostOf (controller, "Music") == musicBoost);
    CHECK (boostOf (controller, "Chat") == chatBoost);
    CHECK (feedback == "Game: Boost 60%");
    manager.perform (HotkeyAction::BoostDown);
    CHECK (boostOf (controller, "Game") == 0.5f);

    const auto musicMode = controller.getMode (music);
    const auto gameMode = controller.getMode (game);
    manager.perform (HotkeyAction::ToggleMode);
    CHECK (controller.getMode (game) != gameMode);
    CHECK (controller.getMode (music) == musicMode);
    CHECK (feedback.startsWith ("Game: "));
    CHECK (feedback.endsWith (" mode"));

    manager.perform (HotkeyAction::NextPreset);
    CHECK (controller.getCurrentPresetId (game).isNotEmpty());
    CHECK (controller.getCurrentPresetId (music).isEmpty());
    CHECK (feedback == "Game: preset " + controller.getCurrentPresetName (game));
    CHECK (controller.getSelectedStrip() == music); // the window's selection is untouched

    // Another hotkey strip (Settings > Hotkeys), and one the layout lacks (-> the first strip).
    controller.setHotkeyStripName ("Chat");
    CHECK (controller.getHotkeyStrip() == chat);
    const float gameBoost = boostOf (controller, "Game");
    manager.perform (HotkeyAction::BoostUp);
    CHECK (feedback.startsWith ("Chat: Boost "));
    CHECK (boostOf (controller, "Game") == gameBoost);
    controller.setHotkeyStripName ("Nonexistent");
    CHECK (controller.getHotkeyStrip() == 0);

    // An active automatic profile's strip wins: the game in front plays there.
    controller.setHotkeyStripName ("Game");
    EngineController::Options o = headlessOptions (temp);
    o.settingsFile = temp.file ("auto.xml");
    o.foregroundAppFactory = [] { return std::make_unique<FixedForegroundApp> ("mygame.exe"); };
    EngineController withRule (o);
    REQUIRE (withRule.addAutoProfileRule ({ "mygame.exe", "Chat", "factory:music-voice-chat" }));
    for (int i = 0; i < 20 && withRule.getActiveAutoProfile() == nullptr; ++i) // hang guard; applies once stable
        withRule.pollForegroundApp();
    REQUIRE (withRule.getActiveAutoProfile() != nullptr);
    CHECK (withRule.getHotkeyStrip() == withRule.findStrip ("Chat"));
}

TEST_CASE ("App: Focus latches Footsteps at 100 % on the hotkey strip and puts the old value back (E56)")
{
    using namespace flub::param;
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    HotkeyManager manager (controller, std::make_unique<FakeHotkeys>());
    juce::String feedback;
    manager.onActionPerformed = [&feedback] (HotkeyAction, const juce::String& text) { feedback = text; };
    const int game = controller.findStrip ("Game");
    auto& store = controller.getParams (game);
    controller.setMode (ModeValue::Gaming, game);
    store.set (Bank::A, Macro1, 0.3f);
    store.set (Bank::B, Macro1, 0.4f);

    manager.perform (HotkeyAction::ToggleFocus);
    CHECK (controller.isFocused (game));
    CHECK (store.get (Bank::A, Macro1) == 1.0f);
    CHECK (store.get (Bank::B, Macro1) == 1.0f);
    CHECK (feedback == "Game: Focus on (Footsteps 100%)");
    manager.perform (HotkeyAction::ToggleFocus);
    CHECK (! controller.isFocused (game));
    CHECK (store.get (Bank::A, Macro1) == 0.3f);
    CHECK (store.get (Bank::B, Macro1) == 0.4f);
    CHECK (feedback == "Game: Focus off");

    // A value changed while focused is the user's and stays.
    controller.setFocus (game, true);
    store.set (Bank::A, Macro1, 0.8f);
    controller.setFocus (game, false);
    CHECK (store.get (Bank::A, Macro1) == 0.8f);
    CHECK (store.get (Bank::B, Macro1) == 0.4f);

    // A preset loaded while focused ends it without restoring over the preset.
    controller.setFocus (game, true);
    juce::String error;
    REQUIRE (controller.loadPreset ("factory:gaming-competitive-fps", game, error));
    CHECK (! controller.isFocused (game));
    const float presetValue = store.get (Macro1);
    controller.setFocus (game, false);
    CHECK (store.get (Macro1) == presetValue);

    // Music mode: Macro 1 is Punch there, so Focus refuses and changes nothing.
    controller.setMode (ModeValue::Music, game);
    manager.perform (HotkeyAction::ToggleFocus);
    CHECK (! controller.isFocused (game));
    CHECK (store.get (Macro1) == presetValue);
    CHECK (feedback == "Game: Focus needs Gaming mode");
}

TEST_CASE ("App: the ChatMix hotkeys move the MixEngine balance, Game and Chat oppositely only, never a strip gain (E56, E22)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    HotkeyManager manager (controller, std::make_unique<FakeHotkeys>());
    juce::String feedback;
    manager.onActionPerformed = [&feedback] (HotkeyAction, const juce::String& text) { feedback = text; };
    auto& host = controller.getHost();
    const int game = controller.findStrip ("Game"), chat = controller.findStrip ("Chat");
    controller.setStripGainDb (game, -2.0f);

    std::vector<float> before;
    for (int s = 0; s < controller.getNumStrips(); ++s)
        before.push_back (host.getStripGainDb (s));
    const auto gainsUnchanged = [&]
    {
        for (int s = 0; s < controller.getNumStrips(); ++s)
            if (host.getStripGainDb (s) != before[static_cast<size_t> (s)])
                return false;
        return true;
    };

    // One press towards Chat: the engine's balance (the tray flyout's and the
    // Chat row's too), Game 1 - 0.2 in amplitude, Chat and every other strip 0 dB.
    manager.perform (HotkeyAction::ChatMixToChat);
    CHECK (controller.getChatMix() == 0.2f);
    CHECK (host.getMixEngine().getChatMix() == 0.2f);
    CHECK_NEAR (controller.getChatMixGainDb (game), 20.0 * std::log10 (0.8), 1.0e-4);
    for (int s = 0; s < controller.getNumStrips(); ++s)
        if (s != game)
            CHECK (controller.getChatMixGainDb (s) == 0.0f);
    CHECK (feedback == "ChatMix Game -1.9 dB, Chat 0 dB");
    CHECK (gainsUnchanged()); // the strip gains (fader, settings, the host) never move
    CHECK (controller.getStripGainDb (game) == -2.0f);
    CHECK (controller.getSettings().getStripGainDb ("Game") == -2.0f);

    // The user's gain still works while mixed.
    controller.setStripGainDb (game, -4.0f);
    CHECK (host.getStripGainDb (game) == -4.0f);
    before[static_cast<size_t> (game)] = -4.0f;

    // To the end (Game muted) and back past the centre; it stops at the ends.
    for (int i = 0; i < 10; ++i)
        manager.perform (HotkeyAction::ChatMixToChat);
    CHECK (controller.getChatMix() == 1.0f);
    CHECK (feedback == "ChatMix Game muted, Chat 0 dB");
    for (int i = 0; i < 6; ++i)
        manager.perform (HotkeyAction::ChatMixToGame);
    CHECK (controller.getChatMix() < 0.0f);
    CHECK (controller.getChatMixGainDb (game) == 0.0f); // towards Game: Chat down, Game stays
    CHECK (controller.getChatMixGainDb (chat) < 0.0f);
    CHECK (host.getMixEngine().getChatMix() == controller.getChatMix());
    CHECK (gainsUnchanged());
    controller.setChatMix (0.0f);
    CHECK (controller.describeChatMix() == "centred");
    CHECK (host.getMixEngine().getChatMix() == 0.0f);

    // No Chat strip: nothing to balance.
    controller.setStripLayout ({ { "Game", 8 }, { "Music", 2 } });
    manager.perform (HotkeyAction::ChatMixToChat);
    CHECK (feedback == "ChatMix needs a Game and a Chat strip");
    CHECK (controller.getChatMix() == 0.0f);
    CHECK (host.getMixEngine().getChatMix() == 0.0f);
}

TEST_CASE ("App: Night latches the Night Mode dynamics on the hotkey strip, Bypass bypasses that strip only (E56)")
{
    using namespace flub::param;
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    HotkeyManager manager (controller, std::make_unique<FakeHotkeys>());
    juce::String feedback;
    manager.onActionPerformed = [&feedback] (HotkeyAction, const juce::String& text) { feedback = text; };
    const int game = controller.findStrip ("Game");
    auto& store = controller.getParams (game);
    const float compOn = store.get (CompressorOn), ratio = store.get (CompRatio);

    manager.perform (HotkeyAction::ToggleNight);
    CHECK (controller.isNight (game));
    CHECK (feedback == "Game: Night listening on");
    for (const auto bank : { Bank::A, Bank::B })
    {
        CHECK (store.get (bank, AutoLevelOn) == 1.0f);
        CHECK (store.get (bank, AutoLevelTargetLufs) == -14.0f); // Night Mode Gaming since docs/11 E21 Phase 3
        CHECK (store.get (bank, CompRatio) == 3.0f);
    }
    for (int s = 0; s < controller.getNumStrips(); ++s)
        if (s != game)
            CHECK (controller.getParams (s).get (AutoLevelOn) == 0.0f);
    manager.perform (HotkeyAction::ToggleNight);
    CHECK (! controller.isNight (game));
    CHECK (store.get (AutoLevelOn) == 0.0f);
    CHECK (store.get (CompressorOn) == compOn);
    CHECK (store.get (CompRatio) == ratio);

    // Bypass: the Game strip only, on top of the master enable.
    manager.perform (HotkeyAction::ToggleBypass);
    CHECK (controller.isStripBypassed (game));
    CHECK (feedback.startsWith ("Game: bypassed"));
    for (int s = 0; s < controller.getNumStrips(); ++s)
        for (const auto bank : { Bank::A, Bank::B })
            CHECK (controller.getParams (s).get (bank, BypassAll) == (s == game ? 1.0f : 0.0f));
    controller.setEnabled (false);
    controller.setEnabled (true); // the master switch does not undo the strip's bypass
    CHECK (store.get (BypassAll) == 1.0f);
    CHECK (controller.getParams (controller.findStrip ("Music")).get (BypassAll) == 0.0f);
    manager.perform (HotkeyAction::ToggleBypass);
    CHECK (! controller.isStripBypassed (game));
    CHECK (store.get (BypassAll) == 0.0f);
    CHECK (feedback == "Game: processing");

    // Latched overrides never reach the saved state: shutdown releases them.
    controller.setNight (game, true);
    controller.shutdown();
    CHECK (store.get (AutoLevelOn) == 0.0f);
}

TEST_CASE ("App: the new hotkey actions have names, distinct default chords and persisted keys (E56)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    std::set<std::string> names, chords;
    for (const auto action : AppSettings::getAllHotkeyActions())
    {
        names.insert (AppSettings::getHotkeyActionName (action).toStdString());
        chords.insert (AppSettings::chordToString (AppSettings::getDefaultHotkey (action)).toStdString());
    }
    CHECK (AppSettings::getAllHotkeyActions().size() == 11);
    CHECK (names.size() == 11);
    CHECK (chords.size() == 11); // one press never runs two actions
    CHECK (AppSettings::chordToString (AppSettings::getDefaultHotkey (HotkeyAction::ToggleFocus)) == "Ctrl+Alt+S");
    CHECK (AppSettings::chordToString (AppSettings::getDefaultHotkey (HotkeyAction::ChatMixToChat)) == "Ctrl+Alt+PageUp");

    auto& settings = controller.getSettings();
    settings.setHotkey (HotkeyAction::ToggleNight, {});
    CHECK (settings.getHotkey (HotkeyAction::ToggleNight).keyCode == 0);
    CHECK (settings.getHotkey (HotkeyAction::ToggleBypass).keyCode == 'B'); // other actions keep their own key
    settings.setHotkeyStripName ("  Music ");
    CHECK (settings.getHotkeyStripName() == "Music");
}

// =============================================================================
// R4.4: conflicts are first-class (2026-10-07)
// =============================================================================
namespace
{
KeyChord chordNamed (const char* text)
{
    KeyChord chord;
    REQUIRE (AppSettings::chordFromString (text, chord));
    return chord;
}

juce::String bypassChord (EngineController& controller) { return chordOf (controller, HotkeyAction::ToggleBypass); }

/** Every action on its default chord (as a fresh install has them). */
void useDefaults (AppSettings& settings)
{
    settings.setHotkeysEnabled (true);
    for (const auto action : AppSettings::getAllHotkeyActions())
        settings.setHotkey (action, AppSettings::getDefaultHotkey (action));
}

template <typename T>
T* findChild (juce::Component& root, const std::function<bool (T&)>& match)
{
    for (auto* child : root.getChildren())
    {
        if (auto* c = dynamic_cast<T*> (child); c != nullptr && match (*c))
            return c;
        if (auto* found = findChild<T> (*child, match))
            return found;
    }
    return nullptr;
}

ui::HotkeyHooks hooksFor (HotkeyManager& manager)
{
    ui::HotkeyHooks hooks;
    hooks.isSupported = [&manager] { return manager.isSupported(); };
    hooks.getFailures = [&manager] { return manager.getFailures(); };
    hooks.reRegister = [&manager] { manager.registerAll(); };
    hooks.getStatus = [&manager] (HotkeyAction action) { return manager.getStatus (action); };
    hooks.pickFreeChord = [&manager] (HotkeyAction action) { return manager.pickFreeChord (action); };
    hooks.setSuspended = [&manager] (bool suspended) { manager.setSuspended (suspended); };
    return hooks;
}

juce::KeyPress press (int key, int modifiers) { return juce::KeyPress (key, juce::ModifierKeys (modifiers), 0); }
constexpr int kCtrlAlt = juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::altModifier;
constexpr int kCtrlAltShift = kCtrlAlt | juce::ModifierKeys::shiftModifier;
} // namespace

TEST_CASE ("App: the Bypass default is Ctrl+Alt+Shift+B; a saved Ctrl+Alt+B is kept, an unsaved one follows the new default, and a rebind persists (R4.4)")
{
    const flubapptest::TempFolder temp;
    CHECK (AppSettings::chordToString (AppSettings::getDefaultHotkey (HotkeyAction::ToggleBypass)) == "Ctrl+Alt+Shift+B");

    // An existing user who saved Ctrl+Alt+B keeps it.
    const auto saved = temp.file ("saved.settings");
    {
        AppSettings settings (saved, true);
        settings.setHotkey (HotkeyAction::ToggleBypass, chordNamed ("Ctrl+Alt+B"));
        settings.save();
    }
    {
        AppSettings settings (saved, true);
        CHECK (settings.hasSavedHotkey (HotkeyAction::ToggleBypass));
        CHECK (AppSettings::chordToString (settings.getHotkey (HotkeyAction::ToggleBypass)) == "Ctrl+Alt+B");
        CHECK (! settings.hasSavedHotkey (HotkeyAction::ToggleMode));
        CHECK (AppSettings::chordToString (settings.getHotkey (HotkeyAction::ToggleMode)) == "Ctrl+Alt+M");

        // A rebind (recorded or picked) is written and read back.
        settings.setHotkey (HotkeyAction::ToggleBypass, chordNamed ("Ctrl+Alt+Shift+Y"));
        settings.setHotkey (HotkeyAction::ToggleNight, {}); // cleared: "None"
        settings.save();
    }
    {
        AppSettings settings (saved, true);
        CHECK (AppSettings::chordToString (settings.getHotkey (HotkeyAction::ToggleBypass)) == "Ctrl+Alt+Shift+Y");
        CHECK (settings.hasSavedHotkey (HotkeyAction::ToggleNight));
        CHECK (settings.getHotkey (HotkeyAction::ToggleNight).keyCode == 0);
    }

    // One who never changed it (no key in the file, the owner's case) gets the new default.
    AppSettings fresh (temp.file ("fresh.settings"), true);
    CHECK (! fresh.hasSavedHotkey (HotkeyAction::ToggleBypass));
    CHECK (AppSettings::chordToString (fresh.getHotkey (HotkeyAction::ToggleBypass)) == "Ctrl+Alt+Shift+B");

    // The alternatives: the default first, all valid, and no chord is an
    // alternative of two actions or another action's default (two failing
    // actions never compete for one combination).
    std::map<std::string, int> owners;
    for (const auto action : AppSettings::getAllHotkeyActions())
    {
        const auto list = AppSettings::getAlternativeHotkeys (action);
        REQUIRE (list.size() >= 3);
        CHECK (AppSettings::sameChord (list.front(), AppSettings::getDefaultHotkey (action)));
        for (const auto& chord : list)
        {
            CHECK (HotkeyManager::validateChord (chord).isEmpty());
            ++owners[AppSettings::chordToString (chord).toStdString()];
            for (const auto other : AppSettings::getAllHotkeyActions())
                if (other != action)
                    CHECK (! AppSettings::sameChord (chord, AppSettings::getDefaultHotkey (other)));
        }
    }
    for (const auto& [chord, count] : owners)
        CHECK (count == 1);
    CHECK (AppSettings::chordToString (AppSettings::getAlternativeHotkeys (HotkeyAction::ToggleBypass)[1]) == "Ctrl+Alt+Shift+Y");
    CHECK (AppSettings::chordToString (AppSettings::getAlternativeHotkeys (HotkeyAction::ToggleBypass).back()) == "Ctrl+Alt+Shift+F11");
    CHECK (AppSettings::chordToString (AppSettings::getAlternativeHotkeys (HotkeyAction::ToggleEnable)[1]) == "Ctrl+Alt+Shift+F");
    CHECK (AppSettings::chordToString (AppSettings::getAlternativeHotkeys (HotkeyAction::ToggleEnable).back()) == "Ctrl+Alt+Shift+F1");

    // After the default only Ctrl+Alt+Shift chords: Ctrl+Alt+letter is
    // AltGr+letter on Windows (Ctrl+Alt+E types the euro sign on German or
    // French layouts), and no alternative is a key other applications use.
    for (const auto action : AppSettings::getAllHotkeyActions())
    {
        const auto list = AppSettings::getAlternativeHotkeys (action);
        for (size_t i = 1; i < list.size(); ++i)
            CHECK (list[i].modifiers == (KeyChord::Ctrl | KeyChord::Alt | KeyChord::Shift));
        for (const auto& chord : list)
            CHECK (HotkeyManager::commonShortcutProblem (chord, true).isEmpty());
    }
}

TEST_CASE ("App: hotkeys: a chord an earlier action has, or an invalid one, is named as such and never blamed on another application (R4.4)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    auto& settings = controller.getSettings();
    useDefaults (settings);
    settings.setHotkey (HotkeyAction::ToggleNight, chordNamed ("Ctrl+Alt+M")); // Toggle Music / Gaming's
    settings.setHotkey (HotkeyAction::NextPreset, chordNamed ("Shift+A"));     // would swallow typing

    auto service = std::make_unique<FakeHotkeys>();
    auto& fake = *service;
    HotkeyManager manager (controller, std::move (service));
    manager.registerAll();

    CHECK (manager.getStatus (HotkeyAction::ToggleMode).status == HotkeyManager::Status::Registered); // the first one keeps it
    CHECK (manager.getStatus (HotkeyAction::ToggleNight).status == HotkeyManager::Status::Conflict);
    CHECK (manager.getStatusText (HotkeyAction::ToggleNight) == "Same chord as Toggle Music / Gaming");
    CHECK (manager.getStatus (HotkeyAction::NextPreset).status == HotkeyManager::Status::Invalid);
    CHECK (manager.getStatusText (HotkeyAction::NextPreset) == "Not a valid shortcut");
    CHECK (manager.getStatus (HotkeyAction::NextPreset).detail.contains ("need Ctrl, Alt"));
    CHECK (fake.descriptions.count (idOf (HotkeyAction::ToggleNight)) == 0); // never sent to the system
    CHECK (fake.descriptions.count (idOf (HotkeyAction::NextPreset)) == 0);
    CHECK (HotkeyManager::isProblem (HotkeyManager::Status::Conflict));
    CHECK (HotkeyManager::isProblem (HotkeyManager::Status::Invalid));
    CHECK (! HotkeyManager::isProblem (HotkeyManager::Status::Pending));

    const auto failures = manager.getFailureList();
    REQUIRE (failures.size() == 2);
    CHECK (failures[0].action == HotkeyAction::NextPreset); // in the settings' action order
    CHECK (HotkeyManager::describeFailure (failures[0]).startsWith ("Next Preset (Shift+A) is not a valid shortcut: "));
    CHECK (HotkeyManager::describeFailure (failures[1]) == "Night listening (Ctrl+Alt+M) is the same chord as Toggle Music / Gaming");
    CHECK (manager.getFailures().contains ("Night listening (Ctrl+Alt+M) is not registered: Toggle Music / Gaming has the same chord"));
    CHECK (HotkeyManager::findConflict (settings, HotkeyAction::ToggleBypass, chordNamed ("Ctrl+Alt+F")) == HotkeyAction::ToggleEnable);
    CHECK (! HotkeyManager::findConflict (settings, HotkeyAction::ToggleBypass, chordNamed ("Ctrl+Alt+Shift+B")).has_value());

    // A late answer for an action that was never sent changes nothing.
    fake.reportFromAnotherThread ({ { idOf (HotkeyAction::ToggleNight), Result::Status::Registered, {} } });
    drainMessages();
    CHECK (manager.getStatus (HotkeyAction::ToggleNight).status == HotkeyManager::Status::Conflict);
}

TEST_CASE ("App: Pick a free combination tries the alternatives in order and keeps the first one no other application holds (R4.4)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    auto& settings = controller.getSettings();
    useDefaults (settings);
    settings.setHotkey (HotkeyAction::ToggleBypass, chordNamed ("Ctrl+Alt+B")); // an existing user's saved chord

    auto service = std::make_unique<FakeHotkeys>();
    auto& fake = *service;
    fake.takenChords = { "Ctrl+Alt+B", "Ctrl+Alt+Shift+B" }; // the owner's PC holds Ctrl+Alt+B
    HotkeyManager manager (controller, std::move (service));
    int changes = 0;
    manager.onStatusChanged = [&changes] { ++changes; };
    manager.registerAll();

    CHECK (manager.getStatusText (HotkeyAction::ToggleBypass) == "Could not register");
    REQUIRE (manager.getFailureList().size() == 1);
    CHECK (HotkeyManager::describeFailure (manager.getFailureList()[0])
           == "Bypass hotkey strip (Ctrl+Alt+B) could not be registered (another application may hold it, or the system reserves it)");

    // The list: the default, then Y and F11 with Ctrl+Alt+Shift (the current chord left out).
    juce::StringArray candidates;
    for (const auto& chord : HotkeyManager::freeChordCandidates (settings, HotkeyAction::ToggleBypass))
        candidates.add (AppSettings::chordToString (chord));
    CHECK (candidates == (juce::StringArray { "Ctrl+Alt+Shift+B", "Ctrl+Alt+Shift+Y", "Ctrl+Alt+Shift+F11" }));

    changes = 0;
    auto result = manager.pickFreeChord (HotkeyAction::ToggleBypass);
    CHECK (result.found);
    CHECK (result.tried == (juce::StringArray { "Ctrl+Alt+Shift+B", "Ctrl+Alt+Shift+Y" }));
    CHECK (result.message == "Bypass hotkey strip is now Ctrl+Alt+Shift+Y.");
    CHECK (changes == 1); // the refused candidate is not reported on its own
    CHECK (bypassChord (controller) == "Ctrl+Alt+Shift+Y");  // saved
    CHECK (manager.getStatus (HotkeyAction::ToggleBypass).status == HotkeyManager::Status::Registered);
    CHECK (manager.getStatus (HotkeyAction::ToggleBypass).chord == "Ctrl+Alt+Shift+Y");
    CHECK (fake.chords[idOf (HotkeyAction::ToggleBypass)] == "Ctrl+Alt+Shift+Y");
    CHECK (manager.getFailureList().empty());
    for (const auto action : AppSettings::getAllHotkeyActions()) // nothing else moved
        if (action != HotkeyAction::ToggleBypass)
            CHECK (manager.getStatus (action).status == HotkeyManager::Status::Registered);

    // A candidate another Flubsound action has is skipped.
    settings.setHotkey (HotkeyAction::ToggleBypass, chordNamed ("Ctrl+Alt+B"));
    settings.setHotkey (HotkeyAction::ToggleFocus, chordNamed ("Ctrl+Alt+Shift+Y"));
    manager.registerAll();
    result = manager.pickFreeChord (HotkeyAction::ToggleBypass);
    CHECK (result.found);
    CHECK (bypassChord (controller) == "Ctrl+Alt+Shift+F11");
    CHECK (! result.tried.contains ("Ctrl+Alt+Shift+Y"));

    // None free: nothing changes, and the message says so.
    settings.setHotkey (HotkeyAction::ToggleBypass, chordNamed ("Ctrl+Alt+B"));
    fake.takenChords.insert ("Ctrl+Alt+Shift+F11");
    manager.registerAll();
    const int attempts = fake.attempts;
    result = manager.pickFreeChord (HotkeyAction::ToggleBypass);
    CHECK (! result.found);
    CHECK (fake.attempts - attempts == 2); // Ctrl+Alt+Shift+B and Ctrl+Alt+Shift+F11 (Ctrl+Alt+Shift+Y is Focus's)
    CHECK (result.message.startsWith ("None of Ctrl+Alt+Shift+B, Ctrl+Alt+Shift+F11 is free"));
    CHECK (bypassChord (controller) == "Ctrl+Alt+B");
    CHECK (manager.getStatus (HotkeyAction::ToggleBypass).status == HotkeyManager::Status::Unavailable);
    CHECK (manager.getStatus (HotkeyAction::ToggleBypass).chord == "Ctrl+Alt+B");
    fake.reportFromAnotherThread ({ { idOf (HotkeyAction::ToggleBypass), Result::Status::Registered, {} } }); // a stale answer
    drainMessages();
    CHECK (manager.getStatus (HotkeyAction::ToggleBypass).status == HotkeyManager::Status::Unavailable);

    // An explicit list (the real Windows case below uses one).
    result = manager.pickFreeChord (HotkeyAction::ToggleBypass, { chordNamed ("Ctrl+Alt+B"), chordNamed ("Ctrl+Alt+Shift+F13") });
    CHECK (result.found);
    CHECK (bypassChord (controller) == "Ctrl+Alt+Shift+F13");

    // Off, or no service: nothing is tried.
    settings.setHotkeysEnabled (false);
    manager.registerAll();
    CHECK (manager.pickFreeChord (HotkeyAction::ToggleBypass).message.startsWith ("Hotkeys are switched off"));
    settings.setHotkeysEnabled (true);
    HotkeyManager none (controller, nullptr);
    none.registerAll();
    CHECK (! none.pickFreeChord (HotkeyAction::ToggleBypass).found);

    // The desktop answers later (Wayland portal): the first candidate it is
    // handed is kept, and the message says it waits for the desktop.
    auto lateService = std::make_unique<FakeHotkeys>();
    lateService->asynchronous = true;
    lateService->takenChords = { "Ctrl+Alt+B" };
    HotkeyManager portal (controller, std::move (lateService));
    settings.setHotkey (HotkeyAction::ToggleBypass, chordNamed ("Ctrl+Alt+B"));
    portal.registerAll();
    result = portal.pickFreeChord (HotkeyAction::ToggleBypass);
    CHECK (result.found);
    CHECK (result.message == "Bypass hotkey strip is now Ctrl+Alt+Shift+B (waiting for the desktop to confirm it).");
    CHECK (portal.getStatusText (HotkeyAction::ToggleBypass) == "Waiting for the desktop");
}

TEST_CASE ("App: a hotkey failure is announced once per action and chord, also after a restart, and again after it was fixed (R4.4)")
{
    const flubapptest::TempFolder temp;
    auto options = headlessOptions (temp);
    options.persistSettings = true;
    std::set<std::string> taken { "Ctrl+Alt+Shift+B" };
    const auto makeManager = [&taken] (EngineController& c)
    {
        auto service = std::make_unique<FakeHotkeys>();
        service->takenChords = taken;
        auto manager = std::make_unique<HotkeyManager> (c, std::move (service));
        manager->registerAll();
        return manager;
    };

    {
        EngineController controller (options);
        useDefaults (controller.getSettings());
        auto manager = makeManager (controller);
        const auto fresh = manager->takeUnannouncedFailures();
        REQUIRE (fresh.size() == 1);
        CHECK (fresh[0].action == HotkeyAction::ToggleBypass);
        CHECK (manager->takeUnannouncedFailures().empty()); // said once

        // The notice under the header names it, with the fix one click away.
        const auto notice = ui::NoticeBar::hotkeyNotice (fresh, [] {});
        CHECK (notice.key == ui::NoticeBar::kHotkeysKey);
        CHECK (notice.kind == ui::NoticeBar::Notice::Kind::Warning);
        CHECK (notice.text == "Hotkey not active: Bypass hotkey strip (Ctrl+Alt+Shift+B) could not be registered (another application may hold it, or the system reserves it).");
        CHECK (notice.actionLabel == "Fix in Settings");
        CHECK (notice.action != nullptr);
        CHECK (notice.seconds == 0.0); // stays until dismissed or fixed
        CHECK (notice.detail.contains ("pick a free combination"));
        CHECK (ui::NoticeBar::hotkeyNotice ({}, [] {}).text.isEmpty());
        auto two = fresh;
        two.push_back ({ HotkeyAction::ToggleMode, { HotkeyManager::Status::Declined, "Ctrl+Alt+M", {}, {} } });
        CHECK (ui::NoticeBar::hotkeyNotice (two, [] {}).text.startsWith ("Hotkeys not active: Bypass hotkey strip"));
        CHECK (ui::NoticeBar::hotkeyNotice (two, [] {}).text.endsWith ("(+1 other)"));
        controller.shutdown();
    }
    {
        // Restarted: the same failure is not news.
        EngineController controller (options);
        auto manager = makeManager (controller);
        CHECK (manager->getFailureList().size() == 1);
        CHECK (manager->takeUnannouncedFailures().empty());

        // Fixed (the other program gave it up): forgotten, so a later failure is named again.
        taken.clear();
        manager = makeManager (controller);
        CHECK (manager->getFailureList().empty());
        CHECK (manager->takeUnannouncedFailures().empty());
        CHECK (! controller.getSettings().isHotkeyFailureAnnounced (HotkeyAction::ToggleBypass, "Ctrl+Alt+Shift+B"));
        taken = { "Ctrl+Alt+Shift+B" };
        manager = makeManager (controller);
        CHECK (manager->takeUnannouncedFailures().size() == 1);

        // Another chord that fails is news too.
        controller.getSettings().setHotkey (HotkeyAction::ToggleBypass, chordNamed ("Ctrl+Alt+Shift+F"));
        controller.getSettings().setHotkey (HotkeyAction::ToggleEnable, chordNamed ("Ctrl+Alt+Shift+F"));
        manager->registerAll();
        const auto fresh = manager->takeUnannouncedFailures();
        REQUIRE (fresh.size() == 1);
        CHECK (fresh[0].status.status == HotkeyManager::Status::Conflict);
        controller.shutdown();
    }
}

TEST_CASE ("App: the hotkey notice is posted once per new failure, lists every hotkey still failing, drops fixed ones, goes when all work, and is not posted while Settings > Hotkeys is open (R4.4)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    auto& settings = controller.getSettings();
    useDefaults (settings);
    auto service = std::make_unique<FakeHotkeys>();
    auto& fake = *service;
    fake.takenChords = { "Ctrl+Alt+Shift+B" };
    HotkeyManager manager (controller, std::move (service));
    manager.registerAll();

    ui::MainComponent main (controller);
    main.setSize (1280, 820);
    main.setHotkeyHooks (hooksFor (manager));
    auto& notices = main.getNoticeBar();
    CHECK (main.announceHotkeyFailures (manager));
    REQUIRE (notices.current() != nullptr);
    CHECK (notices.current()->key == ui::NoticeBar::kHotkeysKey);
    CHECK (notices.current()->text
           == "Hotkey not active: Bypass hotkey strip (Ctrl+Alt+Shift+B) could not be registered (another application may hold it, or the system "
              "reserves it).");
    CHECK (notices.getActionButton().isVisible());
    CHECK (notices.getActionButton().getButtonText() == "Fix in Settings");
    CHECK (! main.announceHotkeyFailures (manager)); // once
    CHECK (notices.hasNotice (ui::NoticeBar::kHotkeysKey));

    // Started minimised (the window not seen): the tray bubble says it too.
    CHECK (main.getHotkeyTrayText (false) == notices.current()->text);
    CHECK (main.getHotkeyTrayText (true).isEmpty());

    // A later failure: posted, naming the new one first, Bypass still listed.
    fake.takenChords.insert ("Ctrl+Alt+M");
    manager.registerAll();
    CHECK (main.announceHotkeyFailures (manager));
    CHECK (notices.getNumNotices() == 1);
    CHECK (notices.current()->text.startsWith ("Hotkeys not active: Toggle Music / Gaming (Ctrl+Alt+M) could not be registered"));
    CHECK (notices.current()->text.endsWith ("(+1 other)"));
    CHECK (notices.current()->detail.contains ("Bypass hotkey strip (Ctrl+Alt+Shift+B)"));

    // One of them works again: nothing new is posted, the notice drops it.
    fake.takenChords.erase ("Ctrl+Alt+Shift+B");
    manager.registerAll();
    CHECK (! main.announceHotkeyFailures (manager));
    REQUIRE (notices.current() != nullptr);
    CHECK (notices.current()->text.startsWith ("Hotkey not active: Toggle Music / Gaming (Ctrl+Alt+M)"));
    CHECK (! notices.current()->detail.contains ("Bypass"));

    // All work: the notice goes by itself.
    fake.takenChords.clear();
    manager.registerAll();
    CHECK (! main.announceHotkeyFailures (manager));
    CHECK (! notices.hasNotice (ui::NoticeBar::kHotkeysKey));

    // Failing again is news again; dismissed by the user, a change with
    // nothing new does not bring it back.
    fake.takenChords = { "Ctrl+Alt+Shift+B" };
    manager.registerAll();
    CHECK (main.announceHotkeyFailures (manager));
    notices.dismiss (ui::NoticeBar::kHotkeysKey);
    manager.registerAll();
    CHECK (! main.announceHotkeyFailures (manager));
    CHECK (! notices.hasNotice (ui::NoticeBar::kHotkeysKey));

    // Settings > Hotkeys open (a real window): the page shows a new failure
    // itself - its 4 Hz poll picks it up - so no notice is posted, and the
    // failure counts as named.
    if (! flubapptest::haveDisplay())
    {
        std::cerr << "    (the Settings window part skipped: no display)\n";
        return;
    }
    [[maybe_unused]] const flubapptest::TolerateXErrors tolerateXErrors; // empty off X11
    main.openSettingsPage (ui::SettingsDialog::Page::Hotkeys);
    REQUIRE (main.isSettingsPageShowing (ui::SettingsDialog::Page::Hotkeys));
    auto* dialog = main.getSettingsDialog();
    REQUIRE (dialog != nullptr);
    fake.takenChords.insert ("Ctrl+Alt+N");
    manager.registerAll();
    CHECK (! main.announceHotkeyFailures (manager));
    CHECK (! notices.hasNotice (ui::NoticeBar::kHotkeysKey));
    CHECK (settings.isHotkeyFailureAnnounced (HotkeyAction::ToggleNight, "Ctrl+Alt+N"));
    CHECK (flubapptest::pumpMessagesUntil ([dialog] { return dialog->getHotkeysSummary().contains ("Night listening (Ctrl+Alt+N)"); }, 1500));
    // Seen there: not posted after the page is left either.
    dialog->showPage (ui::SettingsDialog::Page::General);
    CHECK (! main.isSettingsPageShowing (ui::SettingsDialog::Page::Hotkeys));
    CHECK (! main.announceHotkeyFailures (manager));
}

TEST_CASE ("App: a key press becomes a chord: letters in either case, digits, F1-F24 and navigation keys with their modifiers; others are refused (R4.4)")
{
    KeyChord chord;
    REQUIRE (ui::chordFromKeyPress (press ('b', kCtrlAltShift), chord));
    CHECK (AppSettings::chordToString (chord) == "Ctrl+Alt+Shift+B");
    REQUIRE (ui::chordFromKeyPress (press ('M', kCtrlAlt), chord));
    CHECK (AppSettings::chordToString (chord) == "Ctrl+Alt+M");
    REQUIRE (ui::chordFromKeyPress (press ('7', juce::ModifierKeys::altModifier), chord));
    CHECK (AppSettings::chordToString (chord) == "Alt+7");
    REQUIRE (ui::chordFromKeyPress (press (juce::KeyPress::F13Key, 0), chord));
    CHECK (AppSettings::chordToString (chord) == "F13");
    REQUIRE (ui::chordFromKeyPress (press (juce::KeyPress::F24Key, juce::ModifierKeys::ctrlModifier), chord));
    CHECK (AppSettings::chordToString (chord) == "Ctrl+F24");
    REQUIRE (ui::chordFromKeyPress (press (juce::KeyPress::upKey, kCtrlAltShift), chord));
    CHECK (AppSettings::chordToString (chord) == "Ctrl+Alt+Shift+Up");
    REQUIRE (ui::chordFromKeyPress (press (juce::KeyPress::pageDownKey, kCtrlAlt), chord));
    CHECK (AppSettings::chordToString (chord) == "Ctrl+Alt+PageDown");
    REQUIRE (ui::chordFromKeyPress (press (juce::KeyPress::spaceKey, kCtrlAlt), chord));
    CHECK (AppSettings::chordToString (chord) == "Ctrl+Alt+Space");
    REQUIRE (ui::chordFromKeyPress (press (juce::KeyPress::deleteKey, kCtrlAlt), chord));
    CHECK (AppSettings::chordToString (chord) == "Ctrl+Alt+Delete");
    CHECK (! ui::chordFromKeyPress (press (juce::KeyPress::numberPad4, kCtrlAlt), chord));
    CHECK (! ui::chordFromKeyPress (press (juce::KeyPress::tabKey, kCtrlAlt), chord));
    CHECK (! ui::chordFromKeyPress (press (',', kCtrlAlt), chord, [] (uint32_t) { return 0u; }));
    CHECK (ui::describeHeldModifiers (juce::ModifierKeys (kCtrlAltShift)) == "Ctrl+Alt+Shift+");

    // A character key goes through the layout: on AZERTY the digit keys type
    // '&', 0xE9 (e acute), ..., 0xE0 (a grave) unshifted, which is what JUCE
    // reports on Windows.
    const auto azerty = [] (uint32_t character) -> uint32_t
    {
        const std::map<uint32_t, uint32_t> keys { { '&', '1' }, { 0xE9, '2' }, { 0xE0, '0' }, { ',', 0xBC } };
        const auto it = keys.find (character);
        return it != keys.end() ? it->second : 0u;
    };
    REQUIRE (ui::chordFromKeyPress (press ('&', kCtrlAltShift), chord, azerty));
    CHECK (AppSettings::chordToString (chord) == "Ctrl+Alt+Shift+1");
    REQUIRE (ui::chordFromKeyPress (press (0xE9, kCtrlAlt), chord, azerty));
    CHECK (AppSettings::chordToString (chord) == "Ctrl+Alt+2");
    REQUIRE (ui::chordFromKeyPress (press (0xE0, kCtrlAlt), chord, azerty));
    CHECK (AppSettings::chordToString (chord) == "Ctrl+Alt+0");
    CHECK (! ui::chordFromKeyPress (press (',', kCtrlAlt), chord, azerty)); // a key that is no letter or digit
    CHECK (! ui::chordFromKeyPress (press (juce::KeyPress::numberPad4, kCtrlAlt), chord, azerty)); // never asked
   #if JUCE_WINDOWS
    // The real lookup (VkKeyScanW on this PC's layout): a lower-case letter
    // is its key; a character needing Shift, or on no letter / digit key, is not.
    CHECK (flub::app::platform_bridge::keyCodeForCharacter ('a') == 'A');
    CHECK (flub::app::platform_bridge::keyCodeForCharacter (',') == 0);
    CHECK (flub::app::platform_bridge::keyCodeForCharacter ('!') == 0);
   #else
    CHECK (flub::app::platform_bridge::keyCodeForCharacter ('&') == 0); // not known here: such chords are typed
   #endif

    // Chords the system would accept but that would take a key from every
    // other application: refused by Settings > Hotkeys (never by
    // registerAll, so a saved one keeps working).
    using HM = HotkeyManager;
    CHECK (HM::commonShortcutProblem (chordNamed ("Alt+F4"), false) == "Alt+F4 closes the active window in every application.");
    CHECK (HM::commonShortcutProblem (chordNamed ("Alt+Space"), false).contains ("window menu"));
    CHECK (HM::commonShortcutProblem (chordNamed ("Ctrl+Alt+Delete"), false).isNotEmpty());
    CHECK (HM::commonShortcutProblem (chordNamed ("Super+L"), false).contains ("lock"));
    CHECK (HM::commonShortcutProblem (chordNamed ("F5"), false).startsWith ("F5 is a key applications use"));
    CHECK (HM::commonShortcutProblem (chordNamed ("Shift+F10"), false).isNotEmpty());
    CHECK (HM::commonShortcutProblem (chordNamed ("F13"), true).isEmpty());
    CHECK (HM::commonShortcutProblem (chordNamed ("Alt+F1"), false).isEmpty());   // typed: on purpose
    CHECK (HM::commonShortcutProblem (chordNamed ("Alt+F1"), true).isNotEmpty()); // recorded: one reflex press
    CHECK (HM::commonShortcutProblem (chordNamed ("Ctrl+C"), true).startsWith ("Ctrl+C is a shortcut other applications use"));
    CHECK (HM::commonShortcutProblem (chordNamed ("Ctrl+Shift+M"), true).isNotEmpty());
    CHECK (HM::commonShortcutProblem (chordNamed ("Ctrl+Shift+M"), false).isEmpty());
    CHECK (HM::commonShortcutProblem (chordNamed ("Alt+Left"), true).isNotEmpty());
    CHECK (HM::commonShortcutProblem (chordNamed ("Ctrl+F13"), true).isEmpty());
    CHECK (HM::commonShortcutProblem (chordNamed ("Ctrl+Alt+Shift+B"), true).isEmpty());
    CHECK (HM::commonShortcutProblem (chordNamed ("Ctrl+Alt+M"), true).isEmpty());
    CHECK (HM::commonShortcutProblem ({}, true).isEmpty());
    for (const auto action : AppSettings::getAllHotkeyActions()) // no default is refused
        CHECK (HM::commonShortcutProblem (AppSettings::getDefaultHotkey (action), true).isEmpty());
    CHECK (ui::describeHeldModifiers ({}).isEmpty());
   #if JUCE_MAC
    REQUIRE (ui::chordFromKeyPress (press ('k', juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier), chord));
    CHECK (AppSettings::chordToString (chord) == "Alt+Super+K");
   #endif

    // Valid as a hotkey: a modifier other than Shift for letters, digits and navigation keys.
    CHECK (HotkeyManager::validateChord (chordNamed ("Ctrl+Alt+Shift+B")).isEmpty());
    CHECK (HotkeyManager::validateChord (chordNamed ("F13")).isEmpty());
    CHECK (HotkeyManager::validateChord (chordNamed ("Shift+B")).isNotEmpty());
    CHECK (HotkeyManager::validateChord (chordNamed ("Up")).isNotEmpty());
    CHECK (HotkeyManager::validateChord ({}).isNotEmpty());
}

TEST_CASE ("App: Settings > Hotkeys records a chord by pressing it: hotkeys suspended meanwhile, another action's or an invalid chord refused, Esc cancels, Backspace clears, reset restores the default (R4.4)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    auto& settings = controller.getSettings();
    useDefaults (settings);
    auto service = std::make_unique<FakeHotkeys>();
    auto& fake = *service;
    HotkeyManager manager (controller, std::move (service));
    manager.registerAll();

    ui::SettingsDialog dialog (controller, hooksFor (manager), [] (ui::MeterPalette) {}, ui::MeterPalette::Standard);
    dialog.setSize (ui::SettingsDialog::kMinWidth, ui::SettingsDialog::kMinHeight);
    dialog.showPage (ui::SettingsDialog::Page::Hotkeys);
    CHECK (dialog.getHotkeysSummary() == "All shortcuts are registered.");

    auto* field = findChild<ui::HotkeyCaptureField> (dialog, [] (ui::HotkeyCaptureField& f) { return f.getTitle() == "Bypass hotkey strip shortcut"; });
    auto* pick = findChild<juce::TextButton> (dialog, [] (juce::TextButton& b) { return b.getTitle() == "Pick a free combination for Bypass hotkey strip"; });
    auto* reset = findChild<ui::IconButton> (dialog, [] (ui::IconButton& b) { return b.getTitle() == "Reset Bypass hotkey strip to default"; });
    REQUIRE (field != nullptr);
    REQUIRE (pick != nullptr);
    REQUIRE (reset != nullptr);
    CHECK (field->getChordText() == "Ctrl+Alt+Shift+B");
    CHECK (! pick->isVisible());
    CHECK (field->getBottom() <= field->getParentComponent()->getHeight()); // every row fits the smallest dialog

    // Recording: Flubsound's own chords are released, so pressing one reaches the field.
    field->startCapture();
    CHECK (field->isCapturing());
    CHECK (manager.isSuspended());
    CHECK (fake.callbacks.empty());
    CHECK (field->getDisplayText() == "Press a shortcut...");
    CHECK (dialog.getHotkeysSummary().startsWith ("Press the new shortcut for Bypass hotkey strip"));
    field->modifierKeysChanged (juce::ModifierKeys (kCtrlAlt));
    CHECK (field->getDisplayText() == "Ctrl+Alt+...");

    // Another action's chord: refused, recording goes on.
    CHECK (field->keyPressed (press ('M', kCtrlAlt)));
    CHECK (field->isCapturing());
    CHECK (dialog.getHotkeysSummary().startsWith ("Ctrl+Alt+M is already Toggle Music / Gaming's shortcut."));
    CHECK (bypassChord (controller) == "Ctrl+Alt+Shift+B");
    // Not a valid hotkey, and a key that cannot be one: refused too.
    CHECK (field->keyPressed (press ('A', juce::ModifierKeys::shiftModifier)));
    CHECK (field->isCapturing());
    CHECK (dialog.getHotkeysSummary().startsWith ("Shift+A cannot be a hotkey: "));
    CHECK (field->keyPressed (press (juce::KeyPress::tabKey, 0)));
    CHECK (field->isCapturing());
    CHECK (dialog.getHotkeysSummary().contains ("cannot be part of a hotkey"));

    // A free chord: saved, recording ends, the hotkeys are registered again with it.
    CHECK (field->keyPressed (press ('k', kCtrlAltShift)));
    CHECK (! field->isCapturing());
    CHECK (! manager.isSuspended());
    CHECK (bypassChord (controller) == "Ctrl+Alt+Shift+K");
    CHECK (field->getChordText() == "Ctrl+Alt+Shift+K");
    CHECK (fake.chords[idOf (HotkeyAction::ToggleBypass)] == "Ctrl+Alt+Shift+K");
    CHECK (fake.callbacks.size() == AppSettings::getAllHotkeyActions().size());
    CHECK (dialog.getHotkeysSummary() == "Bypass hotkey strip is now Ctrl+Alt+Shift+K.\nAll shortcuts are registered.");

    // Esc cancels: nothing changes.
    field->startCapture();
    CHECK (field->keyPressed (press (juce::KeyPress::escapeKey, 0)));
    CHECK (! field->isCapturing());
    CHECK (! manager.isSuspended());
    CHECK (bypassChord (controller) == "Ctrl+Alt+Shift+K");
    CHECK (dialog.getHotkeysSummary() == "All shortcuts are registered.");

    // Backspace clears it.
    field->startCapture();
    CHECK (field->keyPressed (press (juce::KeyPress::backspaceKey, 0)));
    CHECK (settings.getHotkey (HotkeyAction::ToggleBypass).keyCode == 0);
    CHECK (field->getChordText() == "None");
    CHECK (manager.getStatusText (HotkeyAction::ToggleBypass) == "Not assigned");
    CHECK (fake.chords.count (idOf (HotkeyAction::ToggleBypass)) == 0);

    // Reset: the default again.
    REQUIRE (reset->onClick != nullptr);
    reset->onClick();
    CHECK (bypassChord (controller) == "Ctrl+Alt+Shift+B");
    CHECK (field->getChordText() == "Ctrl+Alt+Shift+B");
    CHECK (manager.getStatusText (HotkeyAction::ToggleBypass) == "Registered");

    // Another application takes it: the row turns red with the reason and
    // "Pick a free combination"; one click picks the next free one.
    fake.takenChords = { "Ctrl+Alt+Shift+B" };
    manager.registerAll();
    CHECK (flubapptest::pumpMessagesUntil ([pick] { return pick->isVisible(); }, 1500)); // the page's 4 Hz poll picks it up
    CHECK (field->hasProblem());
    CHECK (pick->isVisible());
    CHECK (pick->getTooltip().contains ("Ctrl+Alt+Shift+Y"));
    // Under the last edit's outcome (the reset's), which the poll keeps.
    CHECK (dialog.getHotkeysSummary().startsWith ("Bypass hotkey strip is back to its default, Ctrl+Alt+Shift+B.\nNot active: Bypass hotkey strip "
                                                  "(Ctrl+Alt+Shift+B) could not be registered (another application may hold it, or the system reserves it)."));
    REQUIRE (pick->onClick != nullptr);
    pick->onClick();
    CHECK (bypassChord (controller) == "Ctrl+Alt+Shift+Y");
    CHECK (field->getChordText() == "Ctrl+Alt+Shift+Y");
    CHECK (! field->hasProblem());
    CHECK (! pick->isVisible());
    CHECK (dialog.getHotkeysSummary() == "Bypass hotkey strip is now Ctrl+Alt+Shift+Y.\nAll shortcuts are registered.");

    // A right click types it instead (Win / Super chords cannot be recorded
    // on Windows and Linux); the same refusals apply.
    auto* typed = findChild<juce::TextEditor> (dialog, [] (juce::TextEditor& e) { return e.getTitle() == "Bypass hotkey strip shortcut as text"; });
    REQUIRE (typed != nullptr);
    CHECK (! typed->isVisible());
    field->clicked (juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier));
    CHECK (typed->isVisible());
    CHECK (! field->isVisible());
    CHECK (! field->isCapturing());
    CHECK (typed->getText() == "Ctrl+Alt+Shift+Y");
    typed->setText ("Ctrl+Alt+M", false);
    REQUIRE (typed->onReturnKey != nullptr);
    typed->onReturnKey();
    CHECK (dialog.getHotkeysSummary().startsWith ("Ctrl+Alt+M is already Toggle Music / Gaming's shortcut."));
    CHECK (bypassChord (controller) == "Ctrl+Alt+Shift+Y");
    CHECK (! typed->isVisible());
    CHECK (field->isVisible());
    field->clicked (juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier));
    typed->setText ("Super+F5", false);
    typed->onReturnKey();
    CHECK (bypassChord (controller) == "Super+F5");
    CHECK (fake.chords[idOf (HotkeyAction::ToggleBypass)] == "Super+F5");
    CHECK (field->getChordText() == "Super+F5");
    CHECK (dialog.getHotkeysSummary() == "Bypass hotkey strip is now Super+F5.\nAll shortcuts are registered.");
    field->clicked (juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier));
    REQUIRE (typed->onEscapeKey != nullptr);
    typed->onEscapeKey();
    CHECK (! typed->isVisible());
    CHECK (bypassChord (controller) == "Super+F5");

    // Leaving the page while recording cancels it (the hotkeys come back).
    field->startCapture();
    CHECK (manager.isSuspended());
    dialog.showPage (ui::SettingsDialog::Page::General);
    CHECK (! field->isCapturing());
    CHECK (! manager.isSuspended());

    // Closed while recording (the window's close button): the hotkeys come back too.
    {
        ui::SettingsDialog closing (controller, hooksFor (manager), [] (ui::MeterPalette) {}, ui::MeterPalette::Standard);
        closing.setSize (ui::SettingsDialog::kMinWidth, ui::SettingsDialog::kMinHeight);
        closing.showPage (ui::SettingsDialog::Page::Hotkeys);
        auto* recorder = findChild<ui::HotkeyCaptureField> (closing, [] (ui::HotkeyCaptureField& f) { return f.getTitle() == "Night listening shortcut"; });
        REQUIRE (recorder != nullptr);
        recorder->startCapture();
        CHECK (manager.isSuspended());
    }
    CHECK (! manager.isSuspended());
    CHECK (fake.callbacks.size() == AppSettings::getAllHotkeyActions().size());
}

TEST_CASE ("App: Settings > Hotkeys: reset, the switch, Pick a free one and typed entry end a recording first, so a chord recorded afterwards is registered; chords other applications use are refused (R4.4)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    auto& settings = controller.getSettings();
    useDefaults (settings);
    auto service = std::make_unique<FakeHotkeys>();
    auto& fake = *service;
    HotkeyManager manager (controller, std::move (service));
    manager.registerAll();

    ui::SettingsDialog dialog (controller, hooksFor (manager), [] (ui::MeterPalette) {}, ui::MeterPalette::Standard);
    dialog.setSize (ui::SettingsDialog::kMinWidth, ui::SettingsDialog::kMinHeight);
    dialog.showPage (ui::SettingsDialog::Page::Hotkeys);
    const auto fieldOf = [&dialog] (const juce::String& name)
    { return findChild<ui::HotkeyCaptureField> (dialog, [name] (ui::HotkeyCaptureField& f) { return f.getTitle() == name + " shortcut"; }); };
    auto* field = fieldOf ("Bypass hotkey strip");
    auto* night = fieldOf ("Night listening");
    auto* reset = findChild<ui::IconButton> (dialog, [] (ui::IconButton& b) { return b.getTitle() == "Reset Bypass hotkey strip to default"; });
    auto* pick = findChild<juce::TextButton> (dialog, [] (juce::TextButton& b) { return b.getTitle() == "Pick a free combination for Bypass hotkey strip"; });
    auto* enable = findChild<juce::ToggleButton> (dialog, [] (juce::ToggleButton& t) { return t.getButtonText() == "Enable system-wide hotkeys"; });
    auto* nightTyped = findChild<juce::TextEditor> (dialog, [] (juce::TextEditor& e) { return e.getTitle() == "Night listening shortcut as text"; });
    REQUIRE (field != nullptr);
    REQUIRE (night != nullptr);
    REQUIRE (reset != nullptr);
    REQUIRE (pick != nullptr);
    REQUIRE (enable != nullptr);
    REQUIRE (nightTyped != nullptr);
    const auto all = AppSettings::getAllHotkeyActions().size();
    // Never a recording field while Flubsound's chords are registered.
    const auto consistent = [&] { return ! field->isCapturing() || (manager.isSuspended() && fake.callbacks.empty()); };

    // The review's sequence: recording, then the reset button (which takes no
    // keyboard focus, so the field never loses it), then a chord.
    field->startCapture();
    REQUIRE (manager.isSuspended());
    reset->onClick();
    CHECK (! field->isCapturing()); // the recording ended first
    CHECK (! manager.isSuspended());
    CHECK (fake.callbacks.size() == all);
    CHECK (fake.chords[idOf (HotkeyAction::ToggleBypass)] == "Ctrl+Alt+Shift+B");
    CHECK (! field->keyPressed (press ('k', kCtrlAltShift))); // no longer recording: nothing is saved
    CHECK (bypassChord (controller) == "Ctrl+Alt+Shift+B");
    // Recording again: the chord is saved and registered, and the page says so truthfully.
    field->startCapture();
    CHECK (field->keyPressed (press ('k', kCtrlAltShift)));
    CHECK (bypassChord (controller) == "Ctrl+Alt+Shift+K");
    CHECK (fake.chords[idOf (HotkeyAction::ToggleBypass)] == "Ctrl+Alt+Shift+K");
    CHECK (manager.getStatus (HotkeyAction::ToggleBypass).chord == "Ctrl+Alt+Shift+K");
    CHECK (manager.getStatusText (HotkeyAction::ToggleBypass) == "Registered");
    CHECK (dialog.getHotkeysSummary() == "Bypass hotkey strip is now Ctrl+Alt+Shift+K.\nAll shortcuts are registered.");

    // Resuming registers from the settings even when a registerAll() in
    // between already ended the suspension (HotkeyManager::setSuspended).
    manager.setSuspended (true);
    manager.registerAll();
    settings.setHotkey (HotkeyAction::ToggleBypass, chordNamed ("Ctrl+Alt+Shift+J"));
    manager.setSuspended (false);
    CHECK (fake.chords[idOf (HotkeyAction::ToggleBypass)] == "Ctrl+Alt+Shift+J");
    settings.setHotkey (HotkeyAction::ToggleBypass, chordNamed ("Ctrl+Alt+Shift+K"));
    manager.registerAll();

    // The switch while recording: off, then on again.
    field->startCapture();
    enable->setToggleState (false, juce::dontSendNotification);
    REQUIRE (enable->onClick != nullptr);
    enable->onClick();
    CHECK (! field->isCapturing());
    CHECK (consistent());
    CHECK (manager.getStatusText (HotkeyAction::ToggleBypass) == "Off");
    field->startCapture();
    enable->setToggleState (true, juce::dontSendNotification);
    enable->onClick();
    CHECK (! field->isCapturing());
    CHECK (! manager.isSuspended());
    CHECK (fake.callbacks.size() == all);

    // Pick a free one while recording (the row failed meanwhile).
    fake.takenChords = { "Ctrl+Alt+Shift+K" };
    manager.registerAll();
    CHECK (flubapptest::pumpMessagesUntil ([pick] { return pick->isVisible(); }, 1500)); // the page's 4 Hz poll
    field->startCapture();
    REQUIRE (pick->onClick != nullptr);
    pick->onClick();
    CHECK (! field->isCapturing());
    CHECK (! manager.isSuspended());
    CHECK (bypassChord (controller) == "Ctrl+Alt+Shift+B"); // the first free candidate, the default
    CHECK (fake.chords[idOf (HotkeyAction::ToggleBypass)] == "Ctrl+Alt+Shift+B");
    CHECK (fake.callbacks.size() == all);

    // Typed entry for another row (right click, or Shift+F10 from the
    // keyboard) while recording: the recording ends first.
    field->startCapture();
    night->clicked (juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier));
    CHECK (! field->isCapturing());
    CHECK (! manager.isSuspended());
    CHECK (nightTyped->isVisible());
    REQUIRE (nightTyped->onEscapeKey != nullptr);
    nightTyped->onEscapeKey();
    CHECK (! nightTyped->isVisible());
    field->startCapture();
    CHECK (night->keyPressed (press (juce::KeyPress::F10Key, juce::ModifierKeys::shiftModifier)));
    CHECK (! field->isCapturing());
    CHECK (nightTyped->isVisible());
    // Typed on purpose, a chord other applications use is taken; a system chord never is.
    nightTyped->setText ("Ctrl+Shift+F9", false);
    nightTyped->onReturnKey();
    CHECK (chordOf (controller, HotkeyAction::ToggleNight) == "Ctrl+Shift+F9");
    CHECK (fake.chords[idOf (HotkeyAction::ToggleNight)] == "Ctrl+Shift+F9");
    night->keyPressed (press (juce::KeyPress::F10Key, juce::ModifierKeys::shiftModifier));
    nightTyped->setText ("Alt+F4", false);
    nightTyped->onReturnKey();
    CHECK (dialog.getHotkeysSummary().startsWith ("Not taken: Alt+F4 closes the active window in every application."));
    CHECK (chordOf (controller, HotkeyAction::ToggleNight) == "Ctrl+Shift+F9");

    // From the keyboard: Space or Return starts recording (juce::Button alone
    // knows only Return); a screen reader's press records, its "show menu" types.
    CHECK (field->keyPressed (press (juce::KeyPress::spaceKey, 0)));
    CHECK (field->isCapturing());
    CHECK (consistent());
    // Recorded, a reflex press of a system chord or of a key other
    // applications use is refused and recording goes on.
    CHECK (field->keyPressed (press (juce::KeyPress::F4Key, juce::ModifierKeys::altModifier)));
    CHECK (field->isCapturing());
    CHECK (dialog.getHotkeysSummary().startsWith ("Not taken: Alt+F4 closes the active window in every application. Press another combination"));
    CHECK (field->keyPressed (press (juce::KeyPress::F5Key, 0)));
    CHECK (dialog.getHotkeysSummary().startsWith ("Not taken: F5 is a key applications use"));
    CHECK (field->keyPressed (press ('c', juce::ModifierKeys::ctrlModifier)));
    CHECK (field->isCapturing());
    CHECK (dialog.getHotkeysSummary().startsWith ("Not taken: Ctrl+C is a shortcut other applications use"));
    CHECK (bypassChord (controller) == "Ctrl+Alt+Shift+B");
    CHECK (consistent());
    CHECK (field->keyPressed (press (juce::KeyPress::F13Key, 0))); // F13 alone is free to take
    CHECK (! field->isCapturing());
    CHECK (bypassChord (controller) == "F13");
    CHECK (fake.chords[idOf (HotkeyAction::ToggleBypass)] == "F13");
    CHECK (field->keyPressed (press (juce::KeyPress::returnKey, 0)));
    CHECK (field->isCapturing());
    CHECK (field->keyPressed (press (juce::KeyPress::escapeKey, 0)));
    CHECK (! field->isCapturing());

    const auto handler = field->createAccessibilityHandler();
    REQUIRE (handler != nullptr);
    CHECK (handler->getRole() == juce::AccessibilityRole::button);
    CHECK (handler->getActions().invoke (juce::AccessibilityActionType::press));
    CHECK (field->isCapturing());
    field->cancelCapture();
    auto* bypassTyped = findChild<juce::TextEditor> (dialog, [] (juce::TextEditor& e) { return e.getTitle() == "Bypass hotkey strip shortcut as text"; });
    REQUIRE (bypassTyped != nullptr);
    CHECK (handler->getActions().invoke (juce::AccessibilityActionType::showMenu));
    CHECK (bypassTyped->isVisible());
    bypassTyped->onEscapeKey();
    CHECK (! manager.isSuspended());
    CHECK (fake.callbacks.size() == all);
}

#if JUCE_WINDOWS
TEST_CASE ("App: Windows hotkeys: RegisterHotKey refuses a chord another registrant holds, the row says so, and Pick a free combination takes the next one (R4.4, real service)")
{
    // Two real services in this process: `blocker` plays the other
    // application. Chords on F13 - F16 with Ctrl+Alt+Shift, which no keyboard
    // types by accident; nothing of the user's is touched.
    auto blocker = platform_bridge::createGlobalHotkeys();
    REQUIRE (blocker != nullptr);
    if (! blocker->isSupported())
    {
        std::cerr << "    (skipped: no hotkey window in this session)\n";
        return;
    }
    const auto held = chordNamed ("Ctrl+Alt+Shift+F13");
    if (! blocker->registerHotkey (1, held, [] {}))
    {
        std::cerr << "    (skipped: Ctrl+Alt+Shift+F13 is already taken on this machine)\n";
        return;
    }

    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    auto& settings = controller.getSettings();
    settings.setHotkeysEnabled (true);
    for (const auto action : AppSettings::getAllHotkeyActions())
        settings.setHotkey (action, {}); // only the one under test
    settings.setHotkey (HotkeyAction::ToggleBypass, held);

    HotkeyManager manager (controller); // the real Windows service
    REQUIRE (manager.isSupported());
    manager.registerAll();
    CHECK (manager.getStatus (HotkeyAction::ToggleBypass).status == HotkeyManager::Status::Unavailable);
    CHECK (manager.getStatusText (HotkeyAction::ToggleBypass) == "Could not register");
    CHECK (manager.getFailureList().size() == 1);

    const auto result = manager.pickFreeChord (
        HotkeyAction::ToggleBypass, { held, chordNamed ("Ctrl+Alt+Shift+F14"), chordNamed ("Ctrl+Alt+Shift+F15"), chordNamed ("Ctrl+Alt+Shift+F16") });
    std::cerr << "    tried " << result.tried.joinIntoString (", ") << ": " << result.message << "\n";
    CHECK (result.found);
    CHECK (! AppSettings::sameChord (result.chord, held));
    CHECK (manager.getStatus (HotkeyAction::ToggleBypass).status == HotkeyManager::Status::Registered);
    CHECK (manager.getFailureList().empty());

    // The other application lets go: the original chord registers.
    blocker->unregisterAll();
    settings.setHotkey (HotkeyAction::ToggleBypass, held);
    manager.registerAll();
    CHECK (manager.getStatus (HotkeyAction::ToggleBypass).status == HotkeyManager::Status::Registered);
    manager.unregisterAll();
}
#endif
