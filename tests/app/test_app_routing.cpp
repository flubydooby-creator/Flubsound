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

    void setSessions (std::vector<AudioSessionInfo> s)
    {
        const std::lock_guard<std::mutex> g (lock);
        sessions = std::move (s);
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
        const std::lock_guard<std::mutex> g (script.lock);
        ++script.enumerations;
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

bool wasMoved (RouterScript& script, uint32_t pid, const std::string& endpoint)
{
    for (const auto& m : script.getMoves())
        if (m.first == pid && m.second == endpoint)
            return true;
    return false;
}

/** Fake capture: start() fails for the pids in `refused`. */
class FakeCapture final : public flub::platform::ProcessLoopbackCapture
{
public:
    explicit FakeCapture (const std::set<uint32_t>& refusedPids) : refused (refusedPids) {}

    bool isSupported() const override { return true; }

    bool start (uint32_t processId, bool, double, int, FrameCallback, std::string& error) override
    {
        if (refused.count (processId) != 0)
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
    std::set<uint32_t> refused;
    bool running = false;
};
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

    host.setCaptureFactory ([] { return std::make_unique<FakeCapture> (std::set<uint32_t> { 600 }); });

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
