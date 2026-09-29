// App-level tests: HotkeyManager's per-action registration status (R4.4)
// without an OS, and what the actions do (docs/11 E56: the hotkey strip,
// Focus, ChatMix, Night, Bypass). HotkeyManager runs with a fake platform::GlobalHotkeys
// (injected through its second constructor) that answers either at once, as
// Windows / macOS / X11 do, or later from another thread, as the Wayland
// GlobalShortcuts portal does. The message-thread hand-off is the real one;
// the test pumps the message loop until the expected state is published (the
// timeouts are hang guards).
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "settings/AppSettings.h"
#include "shell/HotkeyManager.h"

#include <cmath>
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

    bool registerHotkey (int id, const KeyChord&, const std::string& description, std::function<void()> callback) override
    {
        descriptions[id] = description;
        if (taken.count (id) != 0)
        {
            reportBinding (id, Result::Status::Unavailable);
            return false;
        }
        callbacks[id] = std::move (callback);
        if (! asynchronous)
            reportBinding (id, Result::Status::Registered);
        return true;
    }

    void unregisterHotkey (int id) override { callbacks.erase (id); }
    void unregisterAll() override { callbacks.clear(); }

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
    std::set<int> taken;
    std::map<int, std::string> descriptions;
    std::map<int, std::function<void()>> callbacks;
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
    CHECK (manager.getStatusText (HotkeyAction::BoostDown) == "In use / could not register");
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
    CHECK (manager.getStatusText (HotkeyAction::BoostDown) == "In use / could not register");
    drainMessages();
    CHECK (manager.getStatusText (HotkeyAction::BoostDown) == "In use / could not register");
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
