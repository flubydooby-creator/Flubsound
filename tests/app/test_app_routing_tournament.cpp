// App-level tests: docs/11 E55 anti-cheat-safe process handling.
//   * platform::ProcessInfoCache (the Windows router's session enumeration):
//     a known process is not opened again, a new session of the same process
//     is checked with one open, a reused process id with a new creation time
//     is resolved again, a process whose sessions went away is forgotten.
//   * Tournament mode: AppRouting stops enumerating (no process is touched),
//     moves and captures nothing new and keeps what runs; the automatic
//     profile switcher holds its state.
#include "RoutingTestFakes.h"

#include "engine/AutoProfile.h"
#include "settings/AppSettings.h"

#include <iostream>
#include <map>
#include <string>

using namespace flub::app;
using flub::platform::ProcessInfoCache;
using flubapptest::AudioServiceScript;
using flubapptest::findRoutedApp;
using flubapptest::makeSession;
using flubapptest::ScriptedRouter;

namespace
{
/** The processes the fake OS knows: pid -> (image path, creation time). */
struct FakeProcesses
{
    std::map<uint32_t, std::pair<std::string, uint64_t>> table;
    int opens = 0, descriptions = 0;

    ProcessInfoCache makeCache()
    {
        return ProcessInfoCache (
            [this] (uint32_t pid, std::string& path, uint64_t& startTime)
            {
                ++opens;
                const auto it = table.find (pid);
                if (it == table.end())
                    return false;
                path = it->second.first;
                startTime = it->second.second;
                return true;
            },
            [this] (const std::string& path)
            {
                ++descriptions;
                return path.find ("cs2") != std::string::npos ? std::string ("Counter-Strike 2") : std::string();
            });
    }
};
} // namespace

TEST_CASE ("App: E55 the process cache opens each game process once, re-resolves a reused pid, forgets exited ones")
{
    FakeProcesses os;
    os.table[4242] = { "C:\\Games\\cs2\\cs2.exe", 133000000000000001ull };
    os.table[777] = { "C:\\Apps\\Spotify\\Spotify.exe", 133000000000000500ull };
    auto cache = os.makeCache();

    // One hour of 2 s passes with cs2 and Spotify playing: each is opened once
    // (before: every pass opened each session's process twice, image path and
    // creation time, i.e. 2 x 1800 = 3600 opens per process per hour).
    constexpr int kPassesPerHour = 1800;
    for (int pass = 0; pass < kPassesPerHour; ++pass)
    {
        cache.beginPass();
        const auto& cs2 = cache.lookup (4242, "{0.0.0.00000000}.{ep}|\\Device\\HarddiskVolume3\\Games\\cs2\\cs2.exe%b{00000000-0000-0000-0000-000000000000}|1%b4242");
        CHECK (cs2.executableName == "cs2.exe");
        const auto& spotify = cache.lookup (777, "spotify-session-1");
        CHECK (spotify.executablePath == "C:\\Apps\\Spotify\\Spotify.exe");
        cache.endPass();
    }
    std::cerr << "    1 h of passes: " << os.opens << " process opens for 2 processes (before: " << 2 * 2 * kPassesPerHour << ")\n";
    CHECK (os.opens == 2);
    CHECK (os.descriptions == 2); // each image's FileDescription, read once
    CHECK (cache.getProcessOpens() == 2);
    CHECK (cache.size() == 2);

    // cs2 opens a second session (another device): one open to check it is
    // the same process (same creation time): its entry and description stay.
    cache.beginPass();
    const auto& second = cache.lookup (4242, "cs2-session-2");
    CHECK (second.description == "Counter-Strike 2");
    cache.lookup (777, "spotify-session-1");
    cache.endPass();
    CHECK (os.opens == 3);
    CHECK (os.descriptions == 2);

    // cs2 exits and a new process gets pid 4242 without a pass in between
    // (a new session instance): re-resolved from its new creation time.
    os.table[4242] = { "C:\\Windows\\notepad.exe", 133000000099999999ull };
    cache.beginPass();
    const auto& reused = cache.lookup (4242, "notepad-session");
    CHECK (reused.executableName == "notepad.exe");
    CHECK (reused.startTime == 133000000099999999ull);
    CHECK (reused.description.empty());
    cache.lookup (777, "spotify-session-1");
    cache.endPass();
    CHECK (os.opens == 4);

    // Spotify's sessions go away for a pass: forgotten, and opened again
    // when it plays again.
    cache.beginPass();
    cache.lookup (4242, "notepad-session");
    cache.endPass();
    CHECK (cache.size() == 1);
    cache.beginPass();
    cache.lookup (4242, "notepad-session");
    cache.lookup (777, "spotify-session-1");
    cache.endPass();
    CHECK (os.opens == 5);

    // A process that cannot be opened (access denied / gone): empty info; a
    // session without an instance identifier is checked on every pass.
    cache.beginPass();
    const auto& denied = cache.lookup (9999, "x");
    CHECK (denied.executablePath.empty());
    CHECK (denied.startTime == 0);
    cache.lookup (777, {});
    cache.lookup (777, {});
    cache.endPass();
    CHECK (os.opens == 8);
}

TEST_CASE ("App: E55 tournament mode freezes routing: no enumeration, no new capture, running captures kept")
{
    const flubapptest::TempFolder temp;
    AppSettings settings (temp.file ("settings.xml"), false);
    AudioEngineHost host;
    auto starts = flubapptest::useCountingCaptures (host);

    AudioServiceScript script;
    {
        const std::lock_guard<std::mutex> g (script.lock);
        script.movable = false; // Automatic -> process capture
        script.endpoints = { { "ep-speakers", "Speakers (Realtek(R) Audio)" } };
    }
    script.setSessions ({ makeSession (500, "cs2.exe", "ep-speakers") });

    AppRouting routing (host, settings, std::make_unique<ScriptedRouter> (script), true);
    routing.setOutputDeviceSource ([] { return juce::String ("Headset Earphone (Stealth 700 Gen 2)"); });
    routing.setRoute ("cs2", "Game");
    routing.setRoute ("spotify", "Music");
    routing.start();
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        const auto* a = findRoutedApp (routing, 500);
        return a != nullptr && a->captureId >= 0;
    }));

    routing.setTournamentMode (true);
    CHECK (routing.isTournamentMode());
    flubapptest::pumpMessagesUntil ([] { return false; }, 150); // a pass that was running ends
    const int frozenAt = script.getEnumerations();

    // Spotify starts playing and the user presses refresh: nothing happens.
    script.setSessions ({ makeSession (500, "cs2.exe", "ep-speakers"), makeSession (700, "Spotify.exe", "ep-speakers") });
    for (int i = 0; i < 5; ++i)
        routing.refresh();
    flubapptest::pumpMessagesUntil ([] { return false; }, 400);
    std::cerr << "    tournament mode: " << (script.getEnumerations() - frozenAt) << " session enumerations while frozen\n";
    CHECK (script.getEnumerations() == frozenAt);
    CHECK (findRoutedApp (routing, 700) == nullptr);
    CHECK ((*starts)[700] == 0);
    // cs2's capture keeps running.
    CHECK (host.getCaptures().size() == 1);
    CHECK (findRoutedApp (routing, 500)->captureId >= 0);
    CHECK ((*starts)[500] == 1);

    // Off: the next pass runs at once and Spotify is captured.
    routing.setTournamentMode (false);
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        const auto* a = findRoutedApp (routing, 700);
        return a != nullptr && a->captureId >= 0;
    }));
    CHECK (script.getEnumerations() > frozenAt);
    CHECK ((*starts)[500] == 1); // never restarted
    routing.shutdown();
}

TEST_CASE ("App: E55 tournament mode holds automatic profiles: no switch while on, the active rule stays")
{
    AutoProfileRule rule;
    rule.executable = "cs2.exe";
    rule.stripName = "Game";
    rule.presetId = "competitive";
    rule.restoreOnExit = true;
    AutoProfileSwitcher switcher;
    switcher.setRules ({ rule });

    AutoProfileSwitcher::Sample game;
    game.valid = true;
    game.processId = 1;
    game.executable = "C:\\Games\\cs2.exe";
    AutoProfileSwitcher::Sample desktop;
    desktop.valid = true;
    desktop.processId = 2;
    desktop.executable = "explorer.exe";

    CHECK (switcher.update (game).empty());
    CHECK (switcher.update (game).size() == 1); // applied after 2 polls
    REQUIRE (switcher.getActiveRule() != nullptr);

    switcher.setTournamentMode (true);
    for (int i = 0; i < 10; ++i)
        CHECK (switcher.update (desktop).empty()); // alt-tab mid-match: nothing restored
    CHECK (switcher.getActiveRule() != nullptr);

    switcher.setTournamentMode (false);
    CHECK (switcher.update (desktop).empty()); // the stability count starts over
    const auto actions = switcher.update (desktop);
    REQUIRE (actions.size() == 1);
    CHECK (actions[0].kind == AutoProfileSwitcher::Action::Kind::End);
    CHECK (actions[0].restore);
}
