// App-level tests: the per-application routing model (R4.5) without an OS.
// AppRouting runs with a fake platform::AppAudioRouter (injected through its
// second constructor) and, for the process-capture method, fake captures
// through AudioEngineHost::setCaptureFactory. The worker thread and the
// message-thread hand-off are the real ones; the test pumps the message loop
// until the expected state is published (the timeouts are hang guards).
#include "AppTestSupport.h"

#include "engine/AppRouting.h"
#include "engine/AudioEngineHost.h"
#include "settings/AppSettings.h"

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace flub::app;
using flub::platform::AudioSessionInfo;

namespace
{
/** Shared state of a FakeRouter (the router itself is owned by AppRouting
    and called from its worker thread). */
struct RouterScript
{
    std::mutex lock;
    bool supported = true;
    std::vector<AudioSessionInfo> sessions;
    std::map<uint32_t, std::string> failures;          // pid -> error returned by setAppEndpoint
    std::vector<std::pair<uint32_t, std::string>> moves; // every setAppEndpoint call (pid, endpoint)
    int enumerations = 0;
    uint32_t flickeringPid = 0; // this session's isActive flips on every enumeration

    // A held enumeration blocks the worker (a pass "in flight") until release().
    std::condition_variable holdChanged;
    bool holdNext = false, holding = false;
    int holds = 0;

    void setSessions (std::vector<AudioSessionInfo> s)
    {
        const std::lock_guard<std::mutex> g (lock);
        sessions = std::move (s);
    }

    /** The next enumeration blocks until release(). */
    void holdNextEnumeration()
    {
        const std::lock_guard<std::mutex> g (lock);
        holdNext = true;
    }

    /** Lets a held enumeration return; holdAgain also blocks the one after. */
    void release (bool holdAgain = false)
    {
        {
            const std::lock_guard<std::mutex> g (lock);
            holding = false;
            holdNext = holdAgain;
        }
        holdChanged.notify_all();
    }

    /** Number of enumerations that have blocked so far (the last may still be). */
    int getHolds()
    {
        const std::lock_guard<std::mutex> g (lock);
        return holds;
    }

    std::vector<std::pair<uint32_t, std::string>> getMoves()
    {
        const std::lock_guard<std::mutex> g (lock);
        return moves;
    }

    int getEnumerations()
    {
        const std::lock_guard<std::mutex> g (lock);
        return enumerations;
    }
};

class FakeRouter final : public flub::platform::AppAudioRouter
{
public:
    explicit FakeRouter (RouterScript& s) : script (s) {}

    bool isSupported() const override
    {
        const std::lock_guard<std::mutex> g (script.lock);
        return script.supported;
    }

    std::vector<AudioSessionInfo> enumerateSessions() override
    {
        std::unique_lock<std::mutex> g (script.lock);
        ++script.enumerations;
        if (script.holdNext)
        {
            script.holdNext = false;
            script.holding = true;
            ++script.holds;
            script.holdChanged.wait (g, [this] { return ! script.holding; });
        }
        for (auto& s : script.sessions)
            if (s.processId == script.flickeringPid)
                s.isActive = ! s.isActive;
        return script.sessions;
    }

    bool setAppEndpoint (uint32_t processId, const std::string& endpointId, std::string& error) override
    {
        const std::lock_guard<std::mutex> g (script.lock);
        script.moves.emplace_back (processId, endpointId);
        if (const auto f = script.failures.find (processId); f != script.failures.end() && ! endpointId.empty())
        {
            error = f->second;
            return false;
        }
        return true;
    }

    void openSystemRoutingSettings() override {}

private:
    RouterScript& script;
};

AudioSessionInfo session (uint32_t pid, const char* exe, bool active = true)
{
    AudioSessionInfo s;
    s.processId = pid;
    s.executableName = exe;
    s.isActive = active;
    return s;
}

const AppRouting::AppState* findApp (const AppRouting& routing, uint32_t pid)
{
    for (const auto& a : routing.getApps())
        if (a.processId == pid)
            return &a;
    return nullptr;
}

int countMoves (RouterScript& script, uint32_t pid, const std::string& endpoint)
{
    int n = 0;
    for (const auto& m : script.getMoves())
        n += m.first == pid && m.second == endpoint ? 1 : 0;
    return n;
}

bool wasMoved (RouterScript& script, uint32_t pid, const std::string& endpoint)
{
    return countMoves (script, pid, endpoint) > 0;
}

/** Shared state of the fake captures (created and started on the message
    thread, like the test body). */
struct CaptureScript
{
    std::set<uint32_t> refused;       // start() fails for these pids
    std::map<uint32_t, int> starts;   // pid -> start() calls
};

/** Fake capture: start() fails for the pids in the script's `refused`. */
class FakeCapture final : public flub::platform::ProcessLoopbackCapture
{
public:
    explicit FakeCapture (std::shared_ptr<CaptureScript> s) : script (std::move (s)) {}

    bool isSupported() const override { return true; }

    bool start (uint32_t processId, bool, double, int, FrameCallback, std::string& error) override
    {
        ++script->starts[processId];
        if (script->refused.count (processId) != 0)
        {
            error = "The process has no audio session (test)";
            return false;
        }
        running = true;
        return true;
    }

    void stop() override { running = false; }
    bool isRunning() const override { return running; }

private:
    std::shared_ptr<CaptureScript> script;
    bool running = false;
};

std::shared_ptr<CaptureScript> useFakeCaptures (AudioEngineHost& host, std::set<uint32_t> refused = {})
{
    auto script = std::make_shared<CaptureScript>();
    script->refused = std::move (refused);
    host.setCaptureFactory ([script] { return std::make_unique<FakeCapture> (script); });
    return script;
}

/** Waits for `count` more worker passes (a flickering session makes every
    pass publish a change, counted by `passes`). */
void runPasses (AppRouting& routing, const int& passes, int count)
{
    for (int i = 0; i < count; ++i)
    {
        const int before = passes;
        routing.refresh();
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return passes > before; }));
    }
}
} // namespace

// =============================================================================
TEST_CASE ("App: AppRouting maps sessions to strips and routes them to the strip endpoints (fake router)")
{
    const flubapptest::TempFolder temp;
    AppSettings settings (temp.file ("settings.xml"), false);
    AudioEngineHost host;

    RouterScript script;
    script.failures[400] = "Access denied (test)";
    script.setSessions ({ session (100, "cs2.exe"), session (200, "Spotify.exe"), session (200, "Spotify.exe", false),
                          session (300, "Discord.exe"), session (400, "locked.exe") });

    AppRouting routing (host, settings, std::make_unique<FakeRouter> (script), false);
    int changes = 0;
    routing.onChanged = [&changes] { ++changes; };

    CHECK (routing.canEnumerateApps());
    CHECK (routing.isEndpointRoutingSupported());
    CHECK (routing.getEffectiveMethod() == AppRouting::Method::EndpointRouting); // Automatic -> endpoint routing

    // Routes by executable (path / case / ".exe" ignored) -> strip by name (case ignored).
    routing.setRoute ("CS2.EXE", "Game");
    routing.setRoute ("C:\\Program Files\\Spotify\\Spotify.exe", "music");
    routing.setRoute ("locked", "Chat");
    CHECK (routing.getRoutes().size() == 3);

    const auto gameEndpoint = settings.getStripEndpointId ("Game").toStdString();
    const auto musicEndpoint = settings.getStripEndpointId ("Music").toStdString();
    CHECK (! gameEndpoint.empty());
    CHECK (gameEndpoint != musicEndpoint);

    routing.start();
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        const auto* cs2 = findApp (routing, 100);
        const auto* locked = findApp (routing, 400);
        return routing.getApps().size() == 5 && cs2 != nullptr && cs2->routed && locked != nullptr && locked->error.isNotEmpty();
    }));
    CHECK (changes >= 1);

    const auto* cs2 = findApp (routing, 100);
    const auto* spotify = findApp (routing, 200);
    const auto* discord = findApp (routing, 300);
    const auto* locked = findApp (routing, 400);
    REQUIRE (cs2 != nullptr);
    REQUIRE (spotify != nullptr);
    REQUIRE (discord != nullptr);
    REQUIRE (locked != nullptr);

    CHECK (cs2->strip == 0);
    CHECK (cs2->routed);
    CHECK (cs2->executable == "cs2.exe");
    CHECK (spotify->strip == 1);
    CHECK (spotify->routed);
    CHECK (discord->strip == -1);
    CHECK (! discord->routed);
    CHECK (discord->error.isEmpty());
    CHECK (locked->strip == 2);
    CHECK (! locked->routed);
    CHECK (locked->error == "Access denied (test)"); // the OS error reaches the UI

    CHECK (wasMoved (script, 100, gameEndpoint));
    CHECK (wasMoved (script, 200, musicEndpoint));
    int spotifyMoves = 0;
    for (const auto& m : script.getMoves())
        spotifyMoves += m.first == 200 ? 1 : 0;
    CHECK (spotifyMoves == 1); // once per process (it has two sessions), and not again on later passes
    CHECK (! wasMoved (script, 300, gameEndpoint) && ! wasMoved (script, 300, musicEndpoint));

    // Un-mapping an app moves it back to the system default ("" endpoint).
    routing.removeRoute ("cs2");
    CHECK (routing.getStripNameForExecutable ("cs2.exe").isEmpty());
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        const auto* a = findApp (routing, 100);
        return a != nullptr && ! a->routed && a->strip == -1;
    }));
    CHECK (wasMoved (script, 100, ""));

    // Shutdown restores every endpoint it moved (Spotify), and only those.
    const auto movesBefore = script.getMoves().size();
    routing.shutdown();
    const auto moves = script.getMoves();
    std::set<uint32_t> restored;
    for (size_t i = movesBefore; i < moves.size(); ++i)
        if (moves[i].second.empty())
            restored.insert (moves[i].first);
    CHECK (restored == std::set<uint32_t> { 200 });

    // After shutdown nothing runs any more.
    const int enumerations = script.getEnumerations();
    routing.refresh();
    flubapptest::pumpMessagesUntil ([] { return false; }, 50);
    CHECK (script.getEnumerations() == enumerations);
}

TEST_CASE ("App: AppRouting persists routes and the method through AppSettings")
{
    const flubapptest::TempFolder temp;
    const auto file = temp.file ("settings.xml");
    RouterScript script;

    {
        AppSettings settings (file, true);
        AudioEngineHost host;
        AppRouting routing (host, settings, std::make_unique<FakeRouter> (script), false);
        routing.setRoute ("cs2.exe", "Game");
        routing.setRoute ("Spotify", "Music");
        routing.setRoute ("/usr/bin/firefox", "System");
        routing.setRoute ("SPOTIFY.EXE", "Chat"); // same app: replaces the Music route
        routing.setRoute ("   ", "Game");         // ignored
        routing.setMethod (AppRouting::Method::ProcessCapture);
        settings.setStripEndpointId ("Game", "Custom Game Endpoint");
        settings.save();
    }

    AppSettings reloaded (file, false);
    CHECK (reloaded.getRoutingMethod() == AppSettings::RoutingMethod::ProcessCapture);
    CHECK (reloaded.getStripEndpointId ("Game") == "Custom Game Endpoint");

    AudioEngineHost host;
    AppRouting routing (host, reloaded, std::make_unique<FakeRouter> (script), false);
    CHECK (routing.getMethod() == AppRouting::Method::ProcessCapture);
    CHECK (routing.getRoutes().size() == 3);
    CHECK (routing.getStripNameForExecutable ("C:\\Games\\CS2.EXE") == "Game");
    CHECK (routing.getStripNameForExecutable ("spotify") == "Chat");
    CHECK (routing.getStripNameForExecutable ("firefox") == "System");
    CHECK (routing.getStripNameForExecutable ("firefox-bin").isEmpty());

    CHECK (AppRouting::executablesMatch ("C:\\Program Files\\Game\\Game.exe", "game"));
    CHECK (AppRouting::executablesMatch ("/opt/app/Tool", "tool.EXE"));
    CHECK (! AppRouting::executablesMatch ("game.exe", "game2.exe"));
    CHECK (! AppRouting::executablesMatch ("exe", "game.exe"));
}

TEST_CASE ("App: AppRouting method resolution, process-capture fallback and capture error reporting")
{
    const flubapptest::TempFolder temp;
    AppSettings settings (temp.file ("settings.xml"), false);
    AudioEngineHost host;

    // No router and no capture: nothing can be enumerated or routed.
    {
        AppRouting none (host, settings, nullptr, false);
        CHECK (! none.canEnumerateApps());
        CHECK (! none.isEndpointRoutingSupported());
        CHECK (none.getEffectiveMethod() == AppRouting::Method::Disabled);
        none.setMethod (AppRouting::Method::EndpointRouting);
        CHECK (none.getEffectiveMethod() == AppRouting::Method::Disabled);
        none.setMethod (AppRouting::Method::Automatic);
    }

    // Sessions can be enumerated, endpoints cannot be moved, captures work
    // (e.g. Windows without the undocumented routing adapter).
    RouterScript script;
    script.supported = false;
    script.flickeringPid = 900; // every pass publishes a change, so passes can be counted
    script.setSessions ({ session (500, "game.exe"), session (600, "exited.exe"), session (700, "music.exe"), session (900, "idle.exe") });

    useFakeCaptures (host, { 600 });

    AppRouting routing (host, settings, std::make_unique<FakeRouter> (script), true);
    int passes = 0;
    routing.onChanged = [&passes] { ++passes; };
    CHECK (routing.getMethod() == AppRouting::Method::Automatic);
    CHECK (routing.getEffectiveMethod() == AppRouting::Method::ProcessCapture);

    routing.setRoute ("game", "Game");
    routing.setRoute ("exited", "Chat");
    routing.setRoute ("music", "Music");
    routing.start();

    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        const auto* game = findApp (routing, 500);
        const auto* music = findApp (routing, 700);
        const auto* exited = findApp (routing, 600);
        return game != nullptr && game->captureId >= 0 && music != nullptr && music->captureId >= 0 && exited != nullptr
               && exited->error.isNotEmpty();
    }));

    const auto* exited = findApp (routing, 600);
    CHECK (exited->captureId == -1);
    CHECK (exited->error == "The process has no audio session (test)");
    CHECK (! findApp (routing, 500)->routed); // captured, not moved
    CHECK (script.getMoves().empty());

    // The failing capture is retried a few times, then given up - and the
    // reason stays visible (it used to vanish once the retries ran out).
    for (int i = 0; i < 4; ++i)
    {
        const int before = passes;
        routing.refresh();
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return passes > before; }));
    }
    exited = findApp (routing, 600);
    REQUIRE (exited != nullptr);
    CHECK (exited->captureId == -1);
    CHECK (exited->error == "The process has no audio session (test)");

    auto captures = host.getCaptures();
    REQUIRE (captures.size() == 2);
    std::map<uint32_t, int> stripOf;
    for (const auto& c : captures)
        stripOf[c.processId] = c.strip;
    CHECK (stripOf[500] == 0);
    CHECK (stripOf[700] == 1);

    // Re-mapping to a strip of the same width re-points the running capture.
    const int musicCapture = findApp (routing, 700)->captureId;
    routing.setRoute ("music", "System");
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        const auto* music = findApp (routing, 700);
        return music != nullptr && music->strip == 3;
    }));
    CHECK (findApp (routing, 700)->captureId == musicCapture);
    captures = host.getCaptures();
    for (const auto& c : captures)
        if (c.processId == 700)
            CHECK (c.strip == 3);

    // The app exits: its capture stops.
    script.setSessions ({ session (500, "game.exe"), session (900, "idle.exe") });
    routing.refresh();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return routing.getApps().size() == 2 && host.getCaptures().size() == 1; }));

    // Routing off: every capture stops.
    routing.setMethod (AppRouting::Method::Disabled);
    CHECK (settings.getRoutingMethod() == AppSettings::RoutingMethod::Disabled);
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return host.getCaptures().empty(); }));
    routing.shutdown();
    CHECK (host.getCaptures().empty());
}

TEST_CASE ("App: AppRouting undoes the endpoint routes of apps that exited while routed, in the same run and the next")
{
    const flubapptest::TempFolder temp;
    const auto file = temp.file ("settings.xml");
    AudioEngineHost host;
    RouterScript script;
    const auto gameEndpoint = AppSettings (temp.file ("defaults.xml"), false).getStripEndpointId ("Game").toStdString();

    {
        AppSettings settings (file, true);
        AppRouting routing (host, settings, std::make_unique<FakeRouter> (script), false);
        routing.setRoute ("cs2.exe", "Game");
        routing.setLiveUpdates (true); // keep enumerating once no route is left
        script.setSessions ({ session (100, "cs2.exe"), session (300, "Discord.exe") });
        routing.start();
        REQUIRE (flubapptest::pumpMessagesUntil ([&]
        {
            const auto* cs2 = findApp (routing, 100);
            return cs2 != nullptr && cs2->routed;
        }));
        CHECK (countMoves (script, 100, gameEndpoint) == 1);

        // An empty list is taken as a failed enumeration, not as every app
        // gone: the route of pid 100 is kept (no second move when it is back).
        script.setSessions ({});
        routing.refresh();
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return routing.getApps().empty(); }));
        script.setSessions ({ session (100, "cs2.exe"), session (300, "Discord.exe") });
        routing.refresh();
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return findApp (routing, 100) != nullptr; }));
        CHECK (findApp (routing, 100)->routed);
        CHECK (countMoves (script, 100, gameEndpoint) == 1);

        // cs2 exits while routed (its route stays with the OS), and the user
        // un-maps it while it is closed.
        script.setSessions ({ session (300, "Discord.exe") });
        routing.refresh();
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return routing.getApps().size() == 1; }));
        routing.removeRoute ("cs2");
        CHECK (! wasMoved (script, 100, "")); // the process is gone: nothing to move back yet

        // Its next process is moved back to the system default, once.
        script.setSessions ({ session (101, "cs2.exe"), session (300, "Discord.exe") });
        routing.refresh();
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return wasMoved (script, 101, "") && findApp (routing, 101) != nullptr; }));
        CHECK (! findApp (routing, 101)->routed);
        CHECK (findApp (routing, 101)->strip == -1);
        CHECK (! wasMoved (script, 300, ""));

        // Done for that executable: a later process is left alone.
        script.setSessions ({ session (102, "cs2.exe"), session (300, "Discord.exe") });
        routing.refresh();
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return findApp (routing, 102) != nullptr; }));
        CHECK (countMoves (script, 101, "") == 1);
        CHECK (! wasMoved (script, 102, ""));

        // Routed again, and it exits before Flubsound quits: shutdown cannot
        // move a process that is gone, so the executable stays remembered.
        routing.setRoute ("cs2", "Game");
        REQUIRE (flubapptest::pumpMessagesUntil ([&]
        {
            const auto* cs2 = findApp (routing, 102);
            return cs2 != nullptr && cs2->routed;
        }));
        script.setSessions ({ session (300, "Discord.exe") });
        routing.refresh();
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return routing.getApps().size() == 1; }));
        routing.shutdown();
        CHECK (! wasMoved (script, 102, ""));
        settings.save();
    }

    // Next run (settings reloaded from disk): the route was removed, and the
    // app's next process is moved back although no route is left at all.
    {
        AppSettings settings (file, true);
        AppRouting routing (host, settings, std::make_unique<FakeRouter> (script), false);
        routing.removeRoute ("cs2");
        CHECK (routing.getRoutes().empty());
        script.setSessions ({ session (200, "C:\\Games\\CS2.EXE"), session (300, "Discord.exe") });
        routing.start();
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return wasMoved (script, 200, ""); }));
        routing.shutdown();
        settings.save();
    }

    // And the run after that has nothing left to undo.
    {
        AppSettings settings (file, false);
        AppRouting routing (host, settings, std::make_unique<FakeRouter> (script), false);
        script.setSessions ({ session (201, "cs2.exe"), session (300, "Discord.exe") });
        routing.setLiveUpdates (true);
        routing.start();
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return findApp (routing, 201) != nullptr; }));
        routing.shutdown();
        CHECK (! wasMoved (script, 201, ""));
    }
}

TEST_CASE ("App: AppRouting retries a given-up capture after a re-mapping, a method change or a stopped capture")
{
    const flubapptest::TempFolder temp;
    AppSettings settings (temp.file ("settings.xml"), false);
    AudioEngineHost host;
    auto captures = useFakeCaptures (host, { 600 });

    RouterScript script;
    script.supported = false;   // Automatic -> process capture
    script.flickeringPid = 900; // every pass publishes a change, so passes can be counted
    script.setSessions ({ session (500, "game.exe"), session (600, "voice.exe"), session (900, "idle.exe") });

    AppRouting routing (host, settings, std::make_unique<FakeRouter> (script), true);
    int passes = 0;
    routing.onChanged = [&passes] { ++passes; };
    routing.setRoute ("game", "Game");
    routing.setRoute ("voice", "Chat");
    routing.start();
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        const auto* game = findApp (routing, 500);
        return game != nullptr && game->captureId >= 0 && captures->starts[600] >= 1;
    }));

    // Three failed starts, then given up: fixing the cause alone changes nothing.
    const auto giveUp = [&]
    {
        runPasses (routing, passes, 4);
        CHECK (captures->starts[600] == 3);
        captures->refused.clear();
        runPasses (routing, passes, 2);
        CHECK (captures->starts[600] == 3);
        REQUIRE (findApp (routing, 600) != nullptr);
        CHECK (findApp (routing, 600)->captureId == -1);
        CHECK (findApp (routing, 600)->error == "The process has no audio session (test)");
    };
    const auto capturedAfter = [&] (int startsBefore)
    {
        REQUIRE (flubapptest::pumpMessagesUntil ([&]
        {
            const auto* voice = findApp (routing, 600);
            return voice != nullptr && voice->captureId >= 0;
        }));
        CHECK (captures->starts[600] == startsBefore + 1);
        CHECK (findApp (routing, 600)->error.isEmpty());
    };
    const auto refuseAgain = [&]
    {
        captures->refused = { 600 };
        captures->starts[600] = 0;
        routing.setRoute ("voice", {}); // stops the capture...
        REQUIRE (flubapptest::pumpMessagesUntil ([&]
        {
            const auto* voice = findApp (routing, 600);
            return voice != nullptr && voice->captureId == -1 && voice->strip == -1;
        }));
        routing.setRoute ("voice", "Chat"); // ...and maps it again: failing starts
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return captures->starts[600] >= 1; }));
    };

    // 1. Re-mapping (here: to another strip) retries it.
    giveUp();
    routing.setRoute ("voice", "Music");
    capturedAfter (3);

    // 2. A method change retries it.
    refuseAgain();
    giveUp();
    routing.setMethod (AppRouting::Method::ProcessCapture); // was Automatic (-> process capture)
    capturedAfter (3);

    // 3. Another capture stopping (a slot is free now) retries it.
    refuseAgain();
    giveUp();
    script.setSessions ({ session (600, "voice.exe"), session (900, "idle.exe") }); // the game exits
    runPasses (routing, passes, 2); // its capture stops, then voice.exe is started again
    capturedAfter (3);
    CHECK (findApp (routing, 500) == nullptr);

    routing.shutdown();
    CHECK (host.getCaptures().empty());
}

TEST_CASE ("App: AppRouting discards a worker pass computed from an outdated configuration")
{
    const flubapptest::TempFolder temp;
    AppSettings settings (temp.file ("settings.xml"), false);
    AudioEngineHost host;
    auto captures = useFakeCaptures (host);

    RouterScript script;
    script.supported = false; // Automatic -> process capture
    script.flickeringPid = 900;
    script.setSessions ({ session (500, "game.exe"), session (700, "music.exe"), session (900, "idle.exe") });

    AppRouting routing (host, settings, std::make_unique<FakeRouter> (script), true);
    struct Unhold // never leave the worker blocked in the fake router
    {
        RouterScript& s;
        ~Unhold() { s.release(); }
    } unhold { script };

    int passes = 0;
    routing.onChanged = [&passes] { ++passes; };
    routing.setRoute ("game", "Game");
    routing.start();
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        const auto* game = findApp (routing, 500);
        return game != nullptr && game->captureId >= 0 && findApp (routing, 700) != nullptr;
    }));

    // A pass starts with music.exe mapped to Music and is held in the router...
    script.holdNextEnumeration();
    routing.setRoute ("music", "Music");
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return script.getHolds() == 1; }));

    // ...while the user removes that route again. The held pass then finishes
    // (its result is posted) and the next one is held, so the message thread
    // sees the outdated result on its own.
    routing.removeRoute ("music");
    script.release (true);
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return script.getHolds() == 2; }));
    bool drained = false;
    juce::MessageManager::callAsync ([&drained] { drained = true; }); // queued after that result
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return drained; }));

    CHECK (captures->starts[700] == 0); // no capture into Music from the outdated mapping
    CHECK (findApp (routing, 700)->strip == -1);

    // The fresh pass publishes the current state.
    const int before = passes;
    script.release();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return passes > before; }));
    CHECK (findApp (routing, 700)->strip == -1);
    CHECK (captures->starts[700] == 0);
    CHECK (host.getCaptures().size() == 1);

    routing.shutdown();
}

TEST_CASE ("App: AppRouting does not take a reused process id for the routed process it replaced")
{
    const flubapptest::TempFolder temp;
    AppSettings settings (temp.file ("settings.xml"), false);
    AudioEngineHost host;
    RouterScript script;
    const auto gameEndpoint = settings.getStripEndpointId ("Game").toStdString();

    AppRouting routing (host, settings, std::make_unique<FakeRouter> (script), false);
    routing.setRoute ("cs2", "Game");
    routing.setRoute ("spotify", "Game");
    routing.setLiveUpdates (true);
    script.setSessions ({ session (100, "cs2.exe"), session (300, "Discord.exe") });
    routing.start();
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        const auto* cs2 = findApp (routing, 100);
        return cs2 != nullptr && cs2->routed;
    }));
    CHECK (countMoves (script, 100, gameEndpoint) == 1);

    const auto emptyEnumeration = [&]
    {
        script.setSessions ({}); // kept as a failed enumeration: pid 100 stays "routed"
        routing.refresh();
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return routing.getApps().empty(); }));
    };
    const auto appearsAs = [&] (uint32_t pid, const char* exe)
    {
        script.setSessions ({ session (pid, exe), session (300, "Discord.exe") });
        routing.refresh();
        REQUIRE (flubapptest::pumpMessagesUntil ([&]
        {
            const auto* app = findApp (routing, pid);
            return app != nullptr && app->executable == exe;
        }));
    };

    // cs2 exited unseen and its pid now belongs to Spotify, mapped to the same
    // strip: Spotify was never moved, so it is moved now.
    emptyEnumeration();
    appearsAs (100, "spotify.exe");
    CHECK (countMoves (script, 100, gameEndpoint) == 2);
    CHECK (findApp (routing, 100)->routed);

    // Spotify exited unseen too and the pid is reused by an un-mapped program:
    // that program is not "moved back", and Spotify's move stays remembered.
    emptyEnumeration();
    appearsAs (100, "notepad.exe");
    CHECK (! wasMoved (script, 100, ""));
    CHECK (! findApp (routing, 100)->routed);

    // Un-mapped while closed, both apps are moved back on their next launch.
    routing.removeRoute ("cs2");
    routing.removeRoute ("spotify");
    script.setSessions ({ session (101, "cs2.exe"), session (102, "spotify.exe"), session (300, "Discord.exe") });
    routing.refresh();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return wasMoved (script, 101, "") && wasMoved (script, 102, ""); }));

    routing.shutdown();
}
