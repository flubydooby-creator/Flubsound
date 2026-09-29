// App-level tests: docs/11 E55 Tournament mode in the app (the engine half,
// AppRouting and AutoProfileSwitcher, is in test_app_routing_tournament.cpp).
//
// * EngineController switches Tournament mode on by itself while a known
//   anti-cheat service runs (a fake AntiCheatServices list,
//   Options::antiCheatServices) and returns to the user's choice when the
//   services have been gone for two polls. While it is on the foreground
//   application is not queried and the routing worker makes no pass, so the
//   process cache opens no process (a counting fake router built on
//   platform::ProcessInfoCache, like the Windows router).
// * The user's switch persists across a restart and freezes routing before
//   its first pass; switching it off while a service holds it on lasts until
//   the service stops; the automatic switch-on can be turned off.
// * The header's TOURNAMENT badge (full width and narrow) and the tray
//   menu's items follow the state.
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "platform/PlatformServices.h"
#include "shell/TrayIcon.h"
#include "ui/HeaderBar.h"

#include <atomic>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using namespace flub::app;
using flub::platform::ProcessInfoCache;

namespace
{
/** The OS's audio sessions and processes, shared with the routing worker. */
struct SessionScript
{
    std::mutex lock;
    std::vector<std::pair<uint32_t, std::string>> sessions;         // pid, instance identifier
    std::map<uint32_t, std::pair<std::string, uint64_t>> processes; // pid -> image path, creation time
    std::atomic<int> enumerations { 0 }, opens { 0 };

    void play (uint32_t pid, const char* path)
    {
        const std::lock_guard<std::mutex> g (lock);
        processes[pid] = { path, 133000000000000000ull + pid };
        sessions.emplace_back (pid, "session-" + std::to_string (pid));
    }
};

/** A router that resolves its sessions through platform::ProcessInfoCache,
    as WinAppAudioRouter::enumerateSessions does; `opens` counts the
    OpenProcess calls the cache makes. Lists, never moves. */
class CachingRouter final : public flub::platform::AppAudioRouter
{
public:
    explicit CachingRouter (SessionScript& s)
        : script (s),
          cache (
              [this] (uint32_t pid, std::string& path, uint64_t& startTime)
              {
                  ++script.opens;
                  const auto it = script.processes.find (pid); // (lock held by enumerateSessions)
                  if (it == script.processes.end())
                      return false;
                  path = it->second.first;
                  startTime = it->second.second;
                  return true;
              },
              [] (const std::string&) { return std::string(); })
    {
    }

    bool isSupported() const override { return false; }
    bool canList() const override { return true; }
    bool canMoveEndpoint() const override { return false; }

    std::vector<flub::platform::AudioSessionInfo> enumerateSessions() override
    {
        const std::lock_guard<std::mutex> g (script.lock);
        ++script.enumerations;
        std::vector<flub::platform::AudioSessionInfo> out;
        cache.beginPass();
        for (const auto& [pid, key] : script.sessions)
        {
            const auto& info = cache.lookup (pid, key);
            flub::platform::AudioSessionInfo s;
            s.processId = pid;
            s.executableName = info.executableName;
            s.executablePath = info.executablePath;
            s.processStartTime = info.startTime;
            s.isActive = true;
            out.push_back (s);
        }
        cache.endPass();
        return out;
    }

    bool setAppEndpoint (uint32_t, const std::string&, std::string& error) override
    {
        error = "not supported";
        return false;
    }

    void openSystemRoutingSettings() override {}

private:
    SessionScript& script;
    ProcessInfoCache cache;
};

struct ForegroundScript
{
    int queries = 0;
};

class CountingForegroundApp final : public flub::platform::ForegroundApp
{
public:
    explicit CountingForegroundApp (ForegroundScript& s) : script (s) {}
    bool isSupported() const override { return true; }
    bool query (flub::platform::ForegroundAppInfo& info) override
    {
        ++script.queries;
        info = {};
        info.processId = 4242;
        info.executableName = "cs2.exe";
        info.executablePath = "C:\\Games\\cs2\\cs2.exe";
        return true;
    }
    std::string unsupportedReason() const override { return {}; }

private:
    ForegroundScript& script;
};

struct Services
{
    std::vector<std::string> running;
    int polls = 0;
};

EngineController::Options options (const flubapptest::TempFolder& temp, Services& services, ForegroundScript& foreground, bool persist = false)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = persist;
    o.foregroundAppFactory = [&foreground] { return std::make_unique<CountingForegroundApp> (foreground); };
    o.antiCheatServices = [&services]
    {
        ++services.polls;
        return services.running;
    };
    return o;
}

/** Starts the controller's routing on the caching router, listing apps. */
void startRouting (EngineController& controller, SessionScript& script)
{
    auto& routing = controller.getRouting();
    routing.setRouter (std::make_unique<CachingRouter> (script), false);
    routing.setLiveUpdates (true);
    routing.start();
}

struct SettingsCounter final : EngineController::Listener
{
    int settings = 0;
    void engineControllerChanged (EngineController::Change change) override
    {
        if (change == EngineController::Change::Settings)
            ++settings;
    }
};

void idle (int ms)
{
    flubapptest::pumpMessagesUntil ([] { return false; }, ms);
}
} // namespace

TEST_CASE ("App: E55 Tournament mode switches on while an anti-cheat service runs and back to the user's choice; no foreground poll and no process open while on")
{
    const flubapptest::TempFolder temp;
    Services services;
    ForegroundScript foreground;
    SessionScript script;
    script.play (500, "C:\\Games\\cs2\\cs2.exe");

    EngineController controller (options (temp, services, foreground));
    SettingsCounter counter;
    controller.addListener (&counter);
    CHECK (services.polls == 1); // at start, before routing's first pass
    CHECK (! controller.isTournamentActive());
    CHECK (controller.describeTournament().isEmpty());

    startRouting (controller, script);
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return script.opens.load() == 1 && controller.getRouting().getApps().size() == 1; }));
    controller.pollForegroundApp();
    controller.pollForegroundApp();
    CHECK (foreground.queries == 2);

    // Vanguard starts: on at the first poll that sees it, with a notice.
    services.running = { "vgc" };
    controller.pollAntiCheatServices();
    CHECK (controller.isTournamentActive());
    const auto& state = controller.getTournamentState();
    CHECK (state.automatic);
    CHECK (! state.userChoice);
    CHECK (controller.describeTournament() == "Tournament mode on: Vanguard is running");
    CHECK (counter.settings >= 1);
    CHECK (controller.getRouting().isTournamentMode());
    CHECK (! controller.getSettings().getTournamentMode()); // the user's choice is untouched

    // While on: a new game process plays, the user refreshes, the foreground
    // is polled: no enumeration, no process open, no foreground query.
    idle (100); // a pass that was running ends
    const int enumerationsAt = script.enumerations.load(), opensAt = script.opens.load(), queriesAt = foreground.queries;
    script.play (700, "C:\\Games\\Valorant\\VALORANT-Win64-Shipping.exe");
    for (int i = 0; i < 5; ++i)
    {
        controller.getRouting().refresh();
        controller.pollForegroundApp();
    }
    idle (300);
    std::cerr << "    tournament mode on: " << (script.enumerations.load() - enumerationsAt) << " session enumerations, "
              << (script.opens.load() - opensAt) << " process opens, " << (foreground.queries - queriesAt) << " foreground queries\n";
    CHECK (script.enumerations.load() == enumerationsAt);
    CHECK (script.opens.load() == opensAt);
    CHECK (foreground.queries == queriesAt);

    // Two services: both named.
    services.running = { "vgc", "BEService" };
    controller.pollAntiCheatServices();
    CHECK (controller.describeTournament() == "Tournament mode on: Vanguard, BattlEye are running");

    // The services stop: one quiet poll keeps it on (a service restarting
    // between matches), the second returns to the user's choice (off).
    services.running.clear();
    controller.pollAntiCheatServices();
    CHECK (controller.isTournamentActive());
    controller.pollAntiCheatServices();
    CHECK (! controller.isTournamentActive());
    CHECK (! controller.getRouting().isTournamentMode());
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return script.opens.load() == opensAt + 1 && controller.getRouting().getApps().size() == 2; }));
    CHECK (script.enumerations.load() > enumerationsAt);
    controller.pollForegroundApp();
    CHECK (foreground.queries == queriesAt + 1);

    // The user's switch: on with no service; a service coming and going does
    // not turn it off.
    controller.setTournamentMode (true);
    CHECK (controller.isTournamentActive());
    CHECK (! controller.getTournamentState().automatic);
    CHECK (controller.describeTournament() == "Tournament mode on");
    CHECK (controller.getSettings().getTournamentMode());
    services.running = { "EasyAntiCheat_EOS" };
    controller.pollAntiCheatServices();
    services.running.clear();
    controller.pollAntiCheatServices();
    controller.pollAntiCheatServices();
    CHECK (controller.isTournamentActive());
    controller.setTournamentMode (false);
    CHECK (! controller.isTournamentActive());

    // Switched off by the user while a service holds it on: off until the
    // service stops; the next start switches it on again.
    services.running = { "FACEITService" };
    controller.pollAntiCheatServices();
    REQUIRE (controller.isTournamentActive());
    controller.setTournamentMode (false);
    CHECK (! controller.isTournamentActive());
    controller.pollAntiCheatServices();
    CHECK (! controller.isTournamentActive());
    services.running.clear();
    controller.pollAntiCheatServices();
    controller.pollAntiCheatServices();
    services.running = { "FACEITService" };
    controller.pollAntiCheatServices();
    CHECK (controller.describeTournament() == "Tournament mode on: FACEIT is running");

    // The automatic switch-on turned off: a running service does nothing.
    controller.setTournamentAuto (false);
    CHECK (! controller.isTournamentActive());
    CHECK (! controller.getSettings().getTournamentAuto());
    controller.setTournamentAuto (true);
    CHECK (controller.isTournamentActive());

    controller.removeListener (&counter);
    controller.shutdown();
}

TEST_CASE ("App: E55 the Tournament mode switch persists across a restart and freezes routing before its first pass")
{
    const flubapptest::TempFolder temp;
    Services services;
    ForegroundScript foreground;
    {
        EngineController controller (options (temp, services, foreground, true));
        CHECK (! controller.getSettings().getTournamentMode());
        CHECK (controller.getSettings().getTournamentAuto());
        controller.setTournamentMode (true);
        controller.setTournamentAuto (false);
        controller.shutdown();
    }
    {
        SessionScript script;
        script.play (500, "C:\\Games\\cs2\\cs2.exe");
        EngineController controller (options (temp, services, foreground, true));
        CHECK (controller.isTournamentActive());
        CHECK (controller.getTournamentState().userChoice);
        CHECK (! controller.getTournamentState().autoEnabled);
        startRouting (controller, script);
        idle (300);
        controller.pollForegroundApp();
        CHECK (script.enumerations.load() == 0);
        CHECK (script.opens.load() == 0);
        CHECK (foreground.queries == 0);
        controller.setTournamentMode (false);
        controller.shutdown();
    }
    {
        // Off again, automatic still off; an anti-cheat running at start
        // with the automatic switch-on (the default) turns it on at once.
        EngineController controller (options (temp, services, foreground, true));
        CHECK (! controller.isTournamentActive());
        controller.setTournamentAuto (true);
        controller.shutdown();
    }
    services.running = { "vgc" };
    SessionScript script;
    script.play (500, "C:\\Games\\cs2\\cs2.exe");
    EngineController controller (options (temp, services, foreground, true));
    CHECK (controller.getTournamentState().automatic);
    startRouting (controller, script);
    idle (300);
    CHECK (script.enumerations.load() == 0);
    CHECK (script.opens.load() == 0);
    controller.shutdown();
}

TEST_CASE ("App UI: E55 the header's TOURNAMENT badge and the tray menu follow Tournament mode")
{
    const flubapptest::TempFolder temp;
    Services services;
    ForegroundScript foreground;
    EngineController controller (options (temp, services, foreground));
    ui::HeaderBar header (controller);
    header.setSize (1280, 56);
    header.updateStatus();
    auto& badge = header.getTournamentBadge();
    CHECK (! badge.isVisible());
    std::map<int, int> presetWidths; // the preset box without the badge
    for (const int w : { 1600, 1280, 1100, 800 })
    {
        header.setSize (w, 56);
        presetWidths[w] = header.getPresetBox().getWidth();
    }

    services.running = { "vgc" };
    controller.pollAntiCheatServices();
    header.updateStatus();
    for (const int w : { 1600, 1280, 1100, 800 })
    {
        header.setSize (w, 56);
        CHECK (badge.isVisible());
        CHECK (header.getLocalBounds().contains (badge.getBounds()));
        CHECK (badge.getWidth() >= (w >= ui::HeaderBar::kTournamentPillWidth ? 90 : 18));
        CHECK (! badge.getBounds().intersects (header.getPresetBox().getBounds()));
        CHECK (! badge.getBounds().intersects (header.getViewButton().getBounds()));
        if (header.isNarrow())
            CHECK (! badge.getBounds().intersects (header.getStripBox().getBounds()));
        // Below the pill width the preset box keeps its room; from it up, 180 px or more.
        std::cerr << "    " << w << " px: badge " << badge.getWidth() << " px, preset box " << header.getPresetBox().getWidth() << " px (without the badge "
                  << presetWidths[w] << ")\n";
        CHECK (header.getPresetBox().getWidth() >= (w >= ui::HeaderBar::kTournamentPillWidth ? 180 : presetWidths[w]));
    }
    CHECK (badge.getTooltip().startsWith ("Tournament mode on: Vanguard is running."));
    CHECK (badge.getTitle() == "Tournament mode");

    // The tray menu: the switch named after the anti-cheat, ticked; the
    // automatic switch-on, ticked. Choosing the switch turns it off.
    juce::PopupMenu menu;
    TrayIcon::addTournamentItems (menu, controller);
    std::vector<juce::PopupMenu::Item> items;
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        items.push_back (it.getItem());
    REQUIRE (items.size() == 2);
    CHECK (items[0].text == "Tournament mode on: Vanguard is running");
    CHECK (items[0].isTicked);
    CHECK (items[1].isTicked);
    REQUIRE (items[0].action != nullptr);
    items[0].action();
    CHECK (! controller.isTournamentActive());
    header.updateStatus();
    CHECK (! badge.isVisible());

    juce::PopupMenu off;
    TrayIcon::addTournamentItems (off, controller);
    juce::PopupMenu::MenuItemIterator it (off);
    REQUIRE (it.next());
    CHECK (it.getItem().text == "Tournament mode");
    CHECK (! it.getItem().isTicked);
    controller.shutdown();
}
