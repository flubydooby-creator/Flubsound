// App-level tests: HotkeyManager's per-action registration status (R4.4)
// without an OS. HotkeyManager runs with a fake platform::GlobalHotkeys
// (injected through its second constructor) that answers either at once, as
// Windows / macOS / X11 do, or later from another thread, as the Wayland
// GlobalShortcuts portal does. The message-thread hand-off is the real one;
// the test pumps the message loop until the expected state is published (the
// timeouts are hang guards).
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "settings/AppSettings.h"
#include "shell/HotkeyManager.h"

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
