// App-level tests: automatic profile switching by foreground application
// (roadmap 2.5).
//
// * AutoProfileSwitcher is the pure decision logic (no platform calls, no
//   clock): one call per foreground poll with a chosen sample; it answers
//   with End / Apply actions.
// * EngineController::pollForegroundApp is the 2 Hz poll; the tests inject a
//   fake platform::ForegroundApp (Options::foregroundAppFactory) and call the
//   poll directly (no timers, no waiting), then check the strip's preset,
//   mode and parameters, the restore of the previous state, the manual
//   cancel and the persistence of the rules through AppSettings.
// * The routing panel lists the rules and explains an unsupported system.
#include "AppTestSupport.h"

#include "engine/AutoProfile.h"
#include "engine/EngineController.h"
#include "ui/RoutingPanel.h"

#include <functional>
#include <memory>
#include <vector>

using namespace flub::app;
using flub::param::Bank;
using flub::param::ModeValue;
using Kind = AutoProfileSwitcher::Action::Kind;

namespace
{
AutoProfileRule rule (const char* exe, const char* strip, const char* preset, bool restore = false,
                      AutoProfileRule::Mode mode = AutoProfileRule::Mode::Preset)
{
    AutoProfileRule r;
    r.executable = exe;
    r.stripName = strip;
    r.presetId = preset;
    r.restoreOnExit = restore;
    r.mode = mode;
    return r;
}

AutoProfileSwitcher::Sample app (const char* exe, uint32_t pid = 100)
{
    AutoProfileSwitcher::Sample s;
    s.valid = true;
    s.processId = pid;
    s.executable = exe;
    return s;
}

AutoProfileSwitcher::Sample ownWindow()
{
    auto s = app ("/opt/flubsound/Flubsound Pro", 1);
    s.isThisProcess = true;
    return s;
}

AutoProfileSwitcher::Sample noAnswer()
{
    return {};
}

/** Actions as "End:<exe>[:restore]" / "Apply:<exe>" for compact checks. */
std::vector<juce::String> describe (const std::vector<AutoProfileSwitcher::Action>& actions)
{
    std::vector<juce::String> out;
    for (const auto& a : actions)
        out.push_back ((a.kind == Kind::Apply ? "Apply:" : "End:") + a.rule.executable + (a.kind == Kind::End && a.restore ? ":restore" : ""));
    return out;
}

using Names = std::vector<juce::String>;

// ---- Fake foreground application (message thread only, like the real one) --
struct ForegroundScript
{
    bool supported = true;
    bool answer = true; // false: query() finds no foreground app
    flub::platform::ForegroundAppInfo info;
    int queries = 0;

    void show (const char* path, uint32_t pid, bool self = false)
    {
        answer = true;
        info = {};
        info.processId = pid;
        info.executablePath = path;
        info.executableName = juce::String (path).replaceCharacter ('\\', '/').fromLastOccurrenceOf ("/", false, false).toStdString();
        info.isThisProcess = self;
    }
};

class FakeForegroundApp final : public flub::platform::ForegroundApp
{
public:
    explicit FakeForegroundApp (ForegroundScript& s) : script (s) {}

    bool isSupported() const override { return script.supported; }

    bool query (flub::platform::ForegroundAppInfo& info) override
    {
        ++script.queries;
        if (! script.supported || ! script.answer)
            return false;
        info = script.info;
        return true;
    }

    std::string unsupportedReason() const override
    {
        return script.supported ? std::string() : std::string ("Wayland does not let applications see which window is in the foreground (test).");
    }

private:
    ForegroundScript& script;
};

EngineController::Options headlessOptions (const flubapptest::TempFolder& temp, ForegroundScript& script, bool persist = false)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = persist;
    o.foregroundAppFactory = [&script] { return std::make_unique<FakeForegroundApp> (script); };
    return o;
}

struct ChangeCounter final : EngineController::Listener
{
    int preset = 0, routing = 0, settings = 0;

    void engineControllerChanged (EngineController::Change change) override
    {
        if (change == EngineController::Change::Preset)
            ++preset;
        else if (change == EngineController::Change::Routing)
            ++routing;
        else if (change == EngineController::Change::Settings)
            ++settings;
    }
};

/** A factory preset of the given mode ("Music" / "Gaming"), skipping `other`. */
PresetInfo factoryPreset (EngineController& c, const char* mode, const juce::String& other = {})
{
    for (const auto& p : c.getPresetManager().getFactoryPresets())
        if (p.mode == mode && p.id != other)
            return p;
    return {};
}

std::vector<float> bankValues (flub::param::ParameterStore& store, Bank bank)
{
    std::vector<float> v;
    for (int i = 0; i < flub::param::kNumParams; ++i)
        v.push_back (store.get (bank, i));
    return v;
}

void poll (EngineController& c, int times)
{
    for (int i = 0; i < times; ++i)
        c.pollForegroundApp();
}

template <typename T>
void collect (juce::Component& root, std::vector<T*>& found)
{
    for (auto* child : root.getChildren())
    {
        if (auto* t = dynamic_cast<T*> (child))
            found.push_back (t);
        collect (*child, found);
    }
}
} // namespace

// =============================================================================
// AutoProfileSwitcher (pure decision logic)
// =============================================================================
TEST_CASE ("App: AutoProfileSwitcher applies a rule after 2 stable polls, ignores a brief alt-tab and ends it when another app has been in front for 2 polls")
{
    AutoProfileSwitcher s;
    CHECK (s.getConfig().switchAfterPolls == 2);
    CHECK (s.setRules ({ rule ("cs2.exe", "Game", "factory:fps", true) }).empty());
    CHECK (s.getActiveRule() == nullptr);

    // A browser in front: nothing to do, however long.
    for (int i = 0; i < 5; ++i)
        CHECK (s.update (app ("/usr/bin/firefox")).empty());

    // The game comes to the front (path with a directory and any case): one
    // poll is not enough, the second applies the rule.
    CHECK (s.update (app ("C:\\Games\\CS2\\CS2.EXE", 4242)).empty());
    CHECK (describe (s.update (app ("C:\\Games\\CS2\\CS2.EXE", 4242))) == Names { "Apply:cs2.exe" });
    REQUIRE (s.getActiveRule() != nullptr);
    CHECK (s.getActiveRule()->presetId == "factory:fps");
    for (int i = 0; i < 5; ++i)
        CHECK (s.update (app ("C:\\Games\\CS2\\cs2.exe", 4242)).empty()); // still in front: no repeat

    // Alt-tab to the browser for one poll and back: no flapping.
    CHECK (s.update (app ("/usr/bin/firefox")).empty());
    CHECK (s.update (app ("cs2.exe", 4242)).empty());
    CHECK (s.update (app ("cs2.exe", 4242)).empty());
    CHECK (s.getActiveRule() != nullptr);

    // The browser stays in front: the rule ends after 2 polls, restoring.
    CHECK (s.update (app ("/usr/bin/firefox")).empty());
    CHECK (describe (s.update (app ("/usr/bin/firefox"))) == Names { "End:cs2.exe:restore" });
    CHECK (s.getActiveRule() == nullptr);
    CHECK (s.update (app ("/usr/bin/firefox")).empty());

    // A slower configuration (4 polls), and a rule that keeps its preset.
    AutoProfileSwitcher slow (AutoProfileSwitcher::Config { 4 });
    slow.setRules ({ rule ("cs2", "Game", "factory:fps") });
    for (int i = 0; i < 3; ++i)
        CHECK (slow.update (app ("cs2.exe")).empty());
    CHECK (describe (slow.update (app ("cs2.exe"))) == Names { "Apply:cs2" });
    for (int i = 0; i < 3; ++i)
        CHECK (slow.update (app ("explorer.exe")).empty());
    CHECK (describe (slow.update (app ("explorer.exe"))) == Names { "End:cs2" }); // no restore
    CHECK (AutoProfileSwitcher (AutoProfileSwitcher::Config { 0 }).getConfig().switchAfterPolls == 1);
}

TEST_CASE ("App: AutoProfileSwitcher holds while Flubsound or no app is in front, switches one rule at a time and matches paths, names and bundle ids (first rule wins)")
{
    AutoProfileSwitcher s;
    s.setRules ({ rule ("cs2.exe", "Game", "factory:fps", true), rule ("com.spotify.client", "Music", "factory:bass"),
                  rule ("/opt/games/Doom", "Game", "factory:horror"), rule ("CS2", "Music", "factory:never") });

    // Matching: name without directory / ".exe" / case, or the bundle id.
    CHECK (s.findRule (app ("/home/u/.steam/cs2.exe")) == 0); // first rule wins over "CS2" (index 3)
    auto spotify = app ("/Applications/Spotify.app/Contents/MacOS/Spotify");
    CHECK (s.findRule (spotify) == -1);
    spotify.bundleId = "com.spotify.client";
    CHECK (s.findRule (spotify) == 1);
    CHECK (s.findRule (app ("doom")) == 2);
    CHECK (s.findRule (app ("doom-launcher")) == -1);
    CHECK (s.findRule (noAnswer()) == -1);
    CHECK (! AutoProfileSwitcher::matches (rule ("  ", "Game", "x"), app ("")));

    s.update (app ("cs2.exe"));
    CHECK (describe (s.update (app ("cs2.exe"))) == Names { "Apply:cs2.exe" });

    // The user switches to Flubsound to tweak the game's preset, or nothing
    // is focused for a while: the rule stays, nothing is counted.
    for (int i = 0; i < 10; ++i)
    {
        CHECK (s.update (ownWindow()).empty());
        CHECK (s.update (noAnswer()).empty());
    }
    CHECK (s.getActiveRule() != nullptr);

    // Holding does not reset a count in progress either: one poll of
    // Spotify, Flubsound, one more poll of Spotify = stable.
    CHECK (s.update (spotify).empty());
    CHECK (s.update (ownWindow()).empty());
    // One switch at a time: the game's rule ends (restoring) before Spotify's applies.
    CHECK ((describe (s.update (spotify)) == Names { "End:cs2.exe:restore", "Apply:com.spotify.client" }));
    CHECK (s.getActiveRule()->stripName == "Music");

    // Straight from Spotify to Doom (same strip as the game, other rule).
    s.update (app ("/opt/games/Doom", 7));
    CHECK ((describe (s.update (app ("/opt/games/Doom", 7))) == Names { "End:com.spotify.client", "Apply:/opt/games/Doom" }));

    // Switched off: the active rule ends without restoring, samples are ignored.
    CHECK (describe (s.setEnabled (false)) == Names { "End:/opt/games/Doom" });
    CHECK (s.setEnabled (false).empty());
    CHECK (! s.isEnabled());
    for (int i = 0; i < 5; ++i)
        CHECK (s.update (app ("cs2.exe")).empty());
    CHECK (s.setEnabled (true).empty());
    CHECK (s.update (app ("cs2.exe")).empty());
    CHECK (describe (s.update (app ("cs2.exe"))) == Names { "Apply:cs2.exe" });
}

TEST_CASE ("App: AutoProfileSwitcher: a manual preset change cancels the rule until its app has left the front; edited rules end without restoring")
{
    AutoProfileSwitcher s;
    s.setRules ({ rule ("cs2.exe", "Game", "factory:fps", true), rule ("spotify", "Music", "factory:bass") });
    s.update (app ("cs2.exe"));
    REQUIRE (describe (s.update (app ("cs2.exe"))) == Names { "Apply:cs2.exe" });

    // A preset chosen by hand on another strip changes nothing ...
    CHECK (! s.cancelStrip ("Music"));
    CHECK (s.getActiveRule() != nullptr);
    // ... on the rule's strip it cancels the rule (nothing to restore later).
    CHECK (s.cancelStrip ("game"));
    CHECK (s.getActiveRule() == nullptr);
    CHECK (! s.cancelStrip ("Game"));

    // The game stays in front: not applied again.
    for (int i = 0; i < 10; ++i)
        CHECK (s.update (app ("cs2.exe")).empty());
    // Nor after another rule is added or removed (the cancelled one is still listed).
    CHECK (s.setRules ({ rule ("spotify", "Music", "factory:bass"), rule ("cs2.exe", "Game", "factory:fps", true),
                         rule ("doom", "Game", "factory:horror") })
               .empty());
    CHECK (s.setRules ({ rule ("cs2.exe", "Game", "factory:fps", true), rule ("spotify", "Music", "factory:bass") }).empty());
    for (int i = 0; i < 5; ++i)
        CHECK (s.update (app ("cs2.exe")).empty());
    CHECK (s.getActiveRule() == nullptr);
    // Another app (even one without a rule) stable in front, then the game again: applied again.
    s.update (app ("firefox"));
    CHECK (s.update (app ("firefox")).empty()); // nothing active: nothing to end
    s.update (app ("cs2.exe"));
    CHECK (describe (s.update (app ("cs2.exe"))) == Names { "Apply:cs2.exe" });

    // Rule edits: an identical rule keeps the active state (even when it moved) ...
    CHECK (s.setRules ({ rule ("spotify", "Music", "factory:bass"), rule ("cs2.exe", "Game", "factory:fps", true) }).empty());
    REQUIRE (s.getActiveRule() != nullptr);
    CHECK (s.getActiveRule()->executable == "cs2.exe");
    for (int i = 0; i < 5; ++i)
        CHECK (s.update (app ("cs2.exe")).empty());
    // ... a changed one ends the active state without restoring, and the new
    // rule applies once the game has been in front for 2 polls again.
    CHECK (describe (s.setRules ({ rule ("cs2.exe", "Game", "factory:other", true) })) == Names { "End:cs2.exe" });
    CHECK (s.getActiveRule() == nullptr);
    CHECK (s.update (app ("cs2.exe")).empty());
    const auto actions = s.update (app ("cs2.exe"));
    REQUIRE (describe (actions) == Names { "Apply:cs2.exe" });
    CHECK (actions[0].rule.presetId == "factory:other");
    // Removing every rule ends it too.
    CHECK (describe (s.setRules ({})) == Names { "End:cs2.exe" });
    CHECK (s.update (app ("cs2.exe")).empty());
    CHECK (s.update (app ("cs2.exe")).empty());
}

// =============================================================================
// EngineController (fake foreground application)
// =============================================================================
TEST_CASE ("App: automatic profiles: EngineController loads the rule's preset on its strip while the app is in front and restores the previous state when it leaves")
{
    const flubapptest::TempFolder temp;
    ForegroundScript script;
    EngineController controller (headlessOptions (temp, script));
    ChangeCounter changes;
    controller.addListener (&changes);
    CHECK (controller.isAutoProfileSupported());
    CHECK (controller.getAutoProfileUnsupportedReason().isEmpty());
    CHECK (controller.getAutoProfilesEnabled());

    const int game = controller.findStrip ("Game");
    const int music = controller.findStrip ("Music");
    REQUIRE (game >= 0);
    REQUIRE (music >= 0);
    const auto musicPreset = factoryPreset (controller, "Music");
    const auto gamingPreset = factoryPreset (controller, "Gaming");
    REQUIRE (musicPreset.isValid());
    REQUIRE (gamingPreset.isValid());

    // The user's own state on the Game strip: a Music preset, edited, on bank B.
    juce::String error;
    REQUIRE (controller.loadPreset (musicPreset.id, game, error));
    auto& store = controller.getParams (game);
    controller.setActiveBank (Bank::B, game);
    store.set (Bank::B, flub::param::BoostIntensity, 0.37f);
    store.set (Bank::A, flub::param::BoostIntensity, 0.11f);
    CHECK (controller.isPresetModified (game));
    const auto userA = bankValues (store, Bank::A), userB = bankValues (store, Bank::B);
    const auto musicStripPreset = controller.getCurrentPresetId (music);

    controller.setAutoProfileRules ({ rule ("cs2.exe", "Game", gamingPreset.id.toRawUTF8(), true) });
    CHECK (changes.settings == 1);
    REQUIRE (controller.getAutoProfileRules().size() == 1);

    // Another app in front: nothing happens. (The poll also remembers it for the "add rule" menu.)
    script.show ("/usr/bin/firefox", 777);
    poll (controller, 5);
    CHECK (script.queries == 5);
    CHECK (controller.getCurrentPresetId (game) == musicPreset.id);
    CHECK (controller.getActiveAutoProfile() == nullptr);

    // The game comes to the front: after 2 polls (1 s) the Game strip plays its preset.
    script.show ("C:\\Games\\CS2\\cs2.exe", 4242);
    const int presetChanges = changes.preset;
    controller.pollForegroundApp();
    CHECK (controller.getCurrentPresetId (game) == musicPreset.id);
    controller.pollForegroundApp();
    CHECK (controller.getCurrentPresetId (game) == gamingPreset.id);
    CHECK (! controller.isPresetModified (game));
    CHECK (controller.getMode (game) == ModeValue::Gaming);
    CHECK (controller.getActiveBank (game) == Bank::B); // loaded into the bank that is heard
    CHECK (controller.getCurrentPresetId (music) == musicStripPreset);
    CHECK (changes.preset == presetChanges + 1);
    CHECK (controller.getSettings().getLastPreset ("Game") == gamingPreset.id);
    REQUIRE (controller.getActiveAutoProfile() != nullptr);
    CHECK (controller.describeAutoProfile() == "cs2 in front: Game plays " + gamingPreset.name + " (restored when it leaves)");
    CHECK (controller.getRecentForegroundApps() == juce::StringArray ("cs2.exe", "firefox"));

    // Flubsound itself in front (the user tweaks the game's preset), then no
    // focused window: the auto profile stays.
    script.show ("/opt/Flubsound Pro", 1, true);
    poll (controller, 6);
    script.answer = false;
    poll (controller, 6);
    CHECK (controller.getCurrentPresetId (game) == gamingPreset.id);
    CHECK (controller.getRecentForegroundApps().size() == 2); // never lists Flubsound

    // Meanwhile the master switch goes off (Bypass All on every strip): that
    // is application state and survives the restore.
    controller.setEnabled (false);

    // The game leaves the front: after 2 polls the user's state is back -
    // both banks, the active bank, the preset and its "modified" state.
    script.show ("/usr/bin/firefox", 777);
    controller.pollForegroundApp();
    CHECK (controller.getCurrentPresetId (game) == gamingPreset.id);
    controller.pollForegroundApp();
    CHECK (controller.getActiveAutoProfile() == nullptr);
    CHECK (controller.describeAutoProfile().isEmpty());
    CHECK (controller.getCurrentPresetId (game) == musicPreset.id);
    CHECK (controller.getSettings().getLastPreset ("Game") == musicPreset.id);
    CHECK (controller.getActiveBank (game) == Bank::B);
    CHECK (controller.isPresetModified (game));
    CHECK (store.get (Bank::A, flub::param::BypassAll) == 1.0f);
    CHECK (store.get (Bank::B, flub::param::BypassAll) == 1.0f);
    auto nowA = bankValues (store, Bank::A), nowB = bankValues (store, Bank::B);
    for (const int appState : { flub::param::BypassAll, flub::param::LatencyProfile, flub::param::LoudnessMatchBypass })
    {
        const auto i = static_cast<size_t> (appState); // application state: stays as it is now
        nowA[i] = userA[i];
        nowB[i] = userB[i];
    }
    CHECK (nowA == userA);
    CHECK (nowB == userB);
    CHECK (store.get (Bank::B, flub::param::BoostIntensity) == 0.37f);
    CHECK (changes.preset == presetChanges + 2);

    // Unsaved edits are exactly what "restore" gives back; nothing else happens.
    poll (controller, 5);
    CHECK (controller.getCurrentPresetId (game) == musicPreset.id);

    // Switching the feature off stops the polling.
    const int queries = script.queries;
    controller.setAutoProfilesEnabled (false);
    CHECK (! controller.getSettings().getAutoProfilesEnabled());
    script.show ("cs2.exe", 4242);
    poll (controller, 5);
    CHECK (script.queries == queries);
    CHECK (controller.getCurrentPresetId (game) == musicPreset.id);

    controller.removeListener (&changes);
}

TEST_CASE ("App: automatic profiles: a keep rule leaves its preset and mode, a manual preset change cancels the rule, and rules persist through AppSettings")
{
    const flubapptest::TempFolder temp;
    ForegroundScript script;
    const auto musicId = [&]
    {
        EngineController probe (headlessOptions (temp, script));
        return factoryPreset (probe, "Music").id;
    }();
    juce::String gamingId, otherGamingId;

    {
        EngineController controller (headlessOptions (temp, script, true));
        gamingId = factoryPreset (controller, "Gaming").id;
        otherGamingId = factoryPreset (controller, "Gaming", gamingId).id;
        REQUIRE (gamingId.isNotEmpty());
        REQUIRE (otherGamingId.isNotEmpty());
        const int musicStrip = controller.findStrip ("Music");
        const int game = controller.findStrip ("Game");
        juce::String error;
        REQUIRE (controller.loadPreset (musicId, musicStrip, error));
        REQUIRE (controller.loadPreset (otherGamingId, game, error));

        // Keep rule with a mode override: a Gaming preset played in Music mode
        // on the Music strip while the game is in front, and kept afterwards.
        // Rules without an executable, strip or preset are dropped.
        controller.setAutoProfileRules ({ rule ("", "Game", "x"), rule ("racer.exe", "Music", gamingId.toRawUTF8(), false, AutoProfileRule::Mode::Music),
                                          rule ("cs2.exe", "Game", musicId.toRawUTF8(), true, AutoProfileRule::Mode::Gaming) });
        REQUIRE (controller.getAutoProfileRules().size() == 2);

        script.show ("D:\\Racer\\racer.exe", 10);
        poll (controller, 2);
        CHECK (controller.getCurrentPresetId (musicStrip) == gamingId);
        CHECK (controller.getMode (musicStrip) == ModeValue::Music);
        CHECK (controller.isPresetModified (musicStrip)); // the preset's own mode is Gaming
        script.show ("/usr/bin/firefox", 777);
        poll (controller, 4);
        CHECK (controller.getActiveAutoProfile() == nullptr);
        CHECK (controller.getCurrentPresetId (musicStrip) == gamingId); // kept
        CHECK (controller.getMode (musicStrip) == ModeValue::Music);

        // Restore rule with a mode override, cancelled by a preset chosen by
        // hand (next preset, as from the hotkey) while the game is in front.
        script.show ("cs2.exe", 4242);
        poll (controller, 2);
        CHECK (controller.getCurrentPresetId (game) == musicId);
        CHECK (controller.getMode (game) == ModeValue::Gaming);
        REQUIRE (controller.getActiveAutoProfile() != nullptr);
        REQUIRE (controller.nextPreset (game));
        const auto chosenByHand = controller.getCurrentPresetId (game);
        CHECK (controller.getActiveAutoProfile() == nullptr);
        poll (controller, 10); // the game stays in front: not applied again
        CHECK (controller.getCurrentPresetId (game) == chosenByHand);
        script.show ("/usr/bin/firefox", 777);
        poll (controller, 4); // leaves: nothing restored
        CHECK (controller.getCurrentPresetId (game) == chosenByHand);

        // Back in front: applied again (and cancelled by a preset loaded from
        // the preset menu, which goes through loadPreset()).
        script.show ("cs2.exe", 4242);
        poll (controller, 2);
        CHECK (controller.getCurrentPresetId (game) == musicId);
        REQUIRE (controller.loadPreset (gamingId, game, error));
        CHECK (controller.getActiveAutoProfile() == nullptr);
        // ... or by a change that bypasses the controller (Reset strip clears
        // the preset id): noticed on the next poll.
        script.show ("/usr/bin/firefox", 777);
        poll (controller, 2);
        script.show ("cs2.exe", 4242);
        poll (controller, 2);
        REQUIRE (controller.getActiveAutoProfile() != nullptr);
        controller.getPresetManager().setCurrentPresetId (game, {}, &controller.getParams (game));
        controller.pollForegroundApp();
        CHECK (controller.getActiveAutoProfile() == nullptr);
        script.show ("/usr/bin/firefox", 777);
        poll (controller, 4);
        CHECK (controller.getCurrentPresetId (game).isEmpty());

        // A rule whose preset no longer exists reports it instead of loading.
        controller.setAutoProfileRules ({ rule ("ghost.exe", "Game", "user:deleted"), controller.getAutoProfileRules()[0],
                                          controller.getAutoProfileRules()[1] });
        script.show ("ghost.exe", 5);
        poll (controller, 2);
        CHECK (controller.describeAutoProfile().contains ("user:deleted"));
        CHECK (controller.getCurrentPresetId (game).isEmpty());
        controller.setAutoProfilesEnabled (false);
        controller.saveState();
    }

    // Persisted: the settings file holds the rules in order, and a new
    // controller starts with them (and the switch).
    AppSettings reread (temp.file ("settings.xml"), false);
    CHECK (! reread.getAutoProfilesEnabled());
    const auto rules = reread.getAutoProfileRules();
    REQUIRE (rules.size() == 3);
    CHECK (rules[0] == rule ("ghost.exe", "Game", "user:deleted"));
    CHECK (rules[1] == rule ("racer.exe", "Music", gamingId.toRawUTF8(), false, AutoProfileRule::Mode::Music));
    CHECK (rules[2] == rule ("cs2.exe", "Game", musicId.toRawUTF8(), true, AutoProfileRule::Mode::Gaming));

    EngineController again (headlessOptions (temp, script));
    CHECK (again.getAutoProfileRules() == rules);
    CHECK (! again.getAutoProfilesEnabled());
}

TEST_CASE ("App: automatic profiles: the routing panel lists the rules and explains a system where the foreground app cannot be detected")
{
    const flubapptest::TempFolder temp;
    ForegroundScript script;
    EngineController controller (headlessOptions (temp, script));
    const auto gamingPreset = factoryPreset (controller, "Gaming");
    REQUIRE (gamingPreset.isValid());
    controller.setAutoProfileRules ({ rule ("C:\\Games\\cs2.exe", "Game", gamingPreset.id.toRawUTF8(), true) });

    ui::RoutingPanel panel (controller);
    panel.setSize (340, 900);
    panel.refreshRouting();

    const auto labels = [&panel]
    {
        std::vector<juce::Label*> found;
        collect (panel, found);
        juce::StringArray texts;
        for (auto* l : found)
            if (l->isVisible())
                texts.add (l->getText());
        return texts;
    };
    const auto buttonNamed = [&panel] (const juce::String& name) -> juce::Button*
    {
        std::vector<juce::Button*> found;
        collect (panel, found);
        for (auto* b : found)
            if (b->getTitle() == name || b->getButtonText() == name)
                return b;
        return nullptr;
    };

    // One line per rule: app, strip, preset, what happens on exit.
    const auto text = labels().joinIntoString ("\n");
    CHECK (text.contains ("cs2 -> Game: " + gamingPreset.name + ", restores on exit"));
    auto* add = buttonNamed ("Add automatic profile...");
    REQUIRE (add != nullptr);
    CHECK (add->isEnabled());
    auto* remove = buttonNamed ("Remove the automatic profile for cs2");
    REQUIRE (remove != nullptr);
    remove->triggerClick(); // posted; the removal itself is deferred once more (it rebuilds the list)
    CHECK (flubapptest::pumpMessagesUntil ([&] { return controller.getAutoProfileRules().empty(); }));
    CHECK (buttonNamed ("Remove the automatic profile for cs2") == nullptr);
    CHECK (labels().joinIntoString ("\n").contains ("No automatic profiles"));

    // Unsupported (e.g. Wayland): the reason is shown and adding is disabled.
    ForegroundScript unsupported;
    unsupported.supported = false;
    const flubapptest::TempFolder temp2;
    EngineController wayland (headlessOptions (temp2, unsupported));
    CHECK (! wayland.isAutoProfileSupported());
    CHECK (wayland.getAutoProfileUnsupportedReason().startsWith ("Wayland does not let applications see"));
    wayland.pollForegroundApp();
    CHECK (unsupported.queries == 0);
    ui::RoutingPanel waylandPanel (wayland);
    waylandPanel.setSize (340, 900);
    waylandPanel.refreshRouting();
    std::vector<juce::Label*> found;
    collect (waylandPanel, found);
    bool reasonShown = false;
    for (auto* l : found)
        reasonShown = reasonShown || (l->isVisible() && l->getText().startsWith ("Wayland does not let applications see"));
    CHECK (reasonShown);
    std::vector<juce::Button*> buttons;
    collect (waylandPanel, buttons);
    for (auto* b : buttons)
        if (b->getTitle() == "Add automatic profile..." || b->getButtonText() == "Add automatic profile...")
            CHECK (! b->isEnabled());
}

TEST_CASE ("App: automatic profiles: the add form offers recently focused apps, builds the rule, and adding replaces a rule for the same app")
{
    const flubapptest::TempFolder temp;
    ForegroundScript script;
    EngineController controller (headlessOptions (temp, script));
    const auto musicPreset = factoryPreset (controller, "Music");
    const auto gamingPreset = factoryPreset (controller, "Gaming");
    REQUIRE (musicPreset.isValid());
    REQUIRE (gamingPreset.isValid());
    const int game = controller.findStrip ("Game");
    REQUIRE (game >= 0);
    juce::String error;
    REQUIRE (controller.loadPreset (musicPreset.id, game, error));
    controller.setSelectedStrip (game);

    // Nothing seen yet: an empty application field, no suggestions.
    {
        ui::AutoProfileForm empty (controller);
        CHECK (empty.getSuggestions().isEmpty());
        CHECK (empty.application.getText().isEmpty());
        CHECK (! empty.recent.isEnabled());
        CHECK (empty.getRule().executable.isEmpty());
        CHECK (! controller.addAutoProfileRule (empty.getRule()));
        CHECK (controller.getAutoProfileRules().empty());
    }

    // The foreground polls remember applications (never Flubsound itself).
    script.show ("C:\\Games\\CS2\\cs2.exe", 4242);
    controller.pollForegroundApp();
    script.show ("/opt/Flubsound Pro", 1, true);
    controller.pollForegroundApp();
    script.show ("/usr/lib/firefox/firefox", 777);
    controller.pollForegroundApp();

    ui::AutoProfileForm form (controller);
    CHECK (form.getSuggestions() == juce::StringArray ("firefox", "cs2.exe"));
    CHECK (form.application.getText() == "firefox"); // the newest is proposed
    CHECK (form.recent.isEnabled());
    CHECK (form.strip.getText() == "Game");         // the selected strip
    auto built = form.getRule();
    CHECK (built.executable == "firefox");
    CHECK (built.stripName == "Game");
    CHECK (built.presetId == musicPreset.id);        // the strip's current preset
    CHECK (built.mode == AutoProfileRule::Mode::Preset);
    CHECK (! built.restoreOnExit);                   // default: the preset stays

    // Picking a recent application fills the field; the rest as chosen.
    form.recent.setSelectedId (2, juce::sendNotificationSync);
    CHECK (form.application.getText() == "cs2.exe");
    for (int id = 1; id <= form.preset.getNumItems() && form.getRule().presetId != gamingPreset.id; ++id) // ids 1..N (headings have none)
        form.preset.setSelectedId (id, juce::dontSendNotification);
    form.mode.setSelectedId (3, juce::dontSendNotification);
    form.restore.setToggleState (true, juce::dontSendNotification);
    built = form.getRule();
    CHECK (built.executable == "cs2.exe");
    CHECK (built.presetId == gamingPreset.id);
    CHECK (built.mode == AutoProfileRule::Mode::Gaming);
    CHECK (built.restoreOnExit);
    REQUIRE (controller.addAutoProfileRule (built));
    REQUIRE (controller.getAutoProfileRules().size() == 1);
    CHECK (controller.getAutoProfileRules()[0] == built);

    // A second rule for the same application (other spelling) replaces the
    // first; one for another application is appended after it.
    REQUIRE (controller.addAutoProfileRule (rule ("firefox", "Music", musicPreset.id.toRawUTF8())));
    REQUIRE (controller.addAutoProfileRule (rule ("D:\\Steam\\CS2.EXE", "Music", musicPreset.id.toRawUTF8(), false)));
    const auto& rules = controller.getAutoProfileRules();
    REQUIRE (rules.size() == 2);
    CHECK (rules[0].executable == "firefox");
    CHECK (rules[1].executable == "D:\\Steam\\CS2.EXE");
    CHECK (controller.getSettings().getAutoProfileRules() == rules);
}

