// App-level tests: docs/11 E47's route journal. Every endpoint move is written
// to a journal next to the settings before it is made; a crash (simulated at
// each point of a move, and a real SIGKILL / TerminateProcess of a child
// process) leaves a journal from which the next start moves the app back.
#include "RoutingTestFakes.h"

#include "settings/AppSettings.h"

#include <cstdlib>
#include <mutex>
#include <set>

using namespace flub::app;
using flubapptest::AudioServiceScript;
using flubapptest::findRoutedApp;
using flubapptest::makeSession;
using flubapptest::ScriptedRouter;

namespace
{
std::vector<RouteJournal::Entry> readJournal (const juce::File& file)
{
    return RouteJournal (file).load();
}

juce::String gameEndpointId (const flubapptest::TempFolder& temp)
{
    return AppSettings (temp.file ("defaults.xml"), false).getStripEndpointId ("Game");
}

/** Waits for `count` more enumerations of the fake audio service. */
void waitForPasses (AppRouting& routing, AudioServiceScript& script, int count)
{
    for (int i = 0; i < count; ++i)
    {
        const int before = script.getEnumerations();
        routing.refresh();
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return script.getEnumerations() > before; }));
    }
    bool drained = false; // the last pass's result has reached the message thread
    juce::Timer::callAfterDelay (20, [&drained] { drained = true; });
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return drained; }));
}
} // namespace

// =============================================================================
TEST_CASE ("App: E47 route journal is written before every endpoint move and removed once the moves are undone")
{
    const flubapptest::TempFolder temp;
    const auto settingsFile = temp.file ("settings.xml");
    const auto journalFile = settingsFile.getSiblingFile ("route-journal.json");
    const auto game = gameEndpointId (temp);
    AppSettings settings (settingsFile, false);
    AudioEngineHost host;

    AudioServiceScript script;
    script.setSessions ({ makeSession (100, "cs2.exe", "ep-headset", 7001), makeSession (300, "Discord.exe", "ep-headset", 7003) });
    std::mutex seenLock;
    std::vector<std::vector<RouteJournal::Entry>> atMoves; // the journal on disk at each move
    script.onMove = [&] (uint32_t, const std::string&)
    {
        auto entries = readJournal (journalFile);
        const std::lock_guard<std::mutex> g (seenLock);
        atMoves.push_back (std::move (entries));
    };

    AppRouting routing (host, settings, std::make_unique<ScriptedRouter> (script), false);
    CHECK (routing.getJournalFile() == journalFile);
    routing.setRoute ("cs2", "Game");
    routing.start();
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        const auto* cs2 = findRoutedApp (routing, 100);
        return cs2 != nullptr && cs2->routed;
    }));

    // At the move the journal already named the process, the target and where it played before.
    {
        const std::lock_guard<std::mutex> g (seenLock);
        REQUIRE (atMoves.size() == 1);
        REQUIRE (atMoves[0].size() == 1);
        const auto& e = atMoves[0][0];
        CHECK (e.processId == 100);
        CHECK (e.processStartTime == 7001);
        CHECK (e.executable == "cs2");
        CHECK (e.endpoint == game);
        CHECK (e.previousEndpoint == "ep-headset");
        CHECK (e.pending);
    }
    // After the pass it is confirmed.
    auto entries = readJournal (journalFile);
    REQUIRE (entries.size() == 1);
    CHECK (entries[0].processId == 100);
    CHECK (! entries[0].pending);

    // Un-mapped: moved back, and nothing is left to recover.
    routing.removeRoute ("cs2");
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return script.countMoves (100, "") == 1 && ! journalFile.exists(); }));
    {
        const std::lock_guard<std::mutex> g (seenLock);
        REQUIRE (atMoves.size() == 2);
        CHECK (atMoves[1].size() == 1); // the move back happens while the entry is still there
    }

    // Routed again and still routed at a clean shutdown: shutdown undoes it and empties the journal.
    routing.setRoute ("cs2", "Game");
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return journalFile.existsAsFile() && script.countMoves (100, game.toStdString()) == 2; }));
    routing.shutdown();
    CHECK (script.countMoves (100, "") == 2);
    CHECK (! journalFile.exists());
}

TEST_CASE ("App: E47 an endpoint move whose journal entry cannot be written is not made")
{
    const flubapptest::TempFolder temp;
    const auto settingsFile = temp.file ("settings.xml");
    const auto game = gameEndpointId (temp);
    // A folder (not empty, so it cannot be renamed over) where the file should go.
    REQUIRE (settingsFile.getSiblingFile ("route-journal.json").getChildFile ("keep").create().wasOk());
    AppSettings settings (settingsFile, false);
    AudioEngineHost host;

    AudioServiceScript script;
    script.setSessions ({ makeSession (100, "cs2.exe", "ep-headset", 7001) });
    AppRouting routing (host, settings, std::make_unique<ScriptedRouter> (script), false);
    routing.setRoute ("cs2", "Game");
    routing.start();
    REQUIRE (flubapptest::pumpMessagesUntil ([&]
    {
        const auto* cs2 = findRoutedApp (routing, 100);
        return cs2 != nullptr && cs2->error.isNotEmpty();
    }));
    CHECK (findRoutedApp (routing, 100)->error.contains ("route journal"));
    CHECK (! findRoutedApp (routing, 100)->routed);
    CHECK (script.countMoves (100, game.toStdString()) == 0);
    routing.shutdown();
    CHECK (script.getMoves().empty());
}

TEST_CASE ("App: E47 a crash right after the journal write or right after the move is undone at the next start")
{
    const flubapptest::TempFolder temp;
    const auto game = gameEndpointId (temp).toStdString();
    const auto beforeMove = temp.file ("crash-before-move.json"), afterMove = temp.file ("crash-after-move.json");

    // A run that moves cs2 (pid 100, started at 7001) to the Game endpoint; the
    // journal is copied at the two points a crash can hit.
    {
        const auto settingsFile = temp.file ("run1.xml");
        const auto journalFile = settingsFile.getSiblingFile ("route-journal.json");
        AppSettings settings (settingsFile, false);
        AudioEngineHost host;
        AudioServiceScript script;
        script.setSessions ({ makeSession (100, "cs2.exe", "ep-headset", 7001) });
        script.onMove = [&] (uint32_t, const std::string& endpoint)
        {
            if (endpoint == game)
                journalFile.copyFileTo (beforeMove);
        };
        AppRouting routing (host, settings, std::make_unique<ScriptedRouter> (script), false);
        routing.setRoute ("cs2", "Game");
        routing.start();
        REQUIRE (flubapptest::pumpMessagesUntil ([&]
        {
            const auto* cs2 = findRoutedApp (routing, 100);
            return cs2 != nullptr && cs2->routed;
        }));
        REQUIRE (journalFile.copyFileTo (afterMove));
        routing.shutdown(); // (a crash would have skipped this)
    }
    REQUIRE (beforeMove.existsAsFile());
    REQUIRE (readJournal (beforeMove).size() == 1);
    CHECK (readJournal (beforeMove)[0].pending);
    CHECK (! readJournal (afterMove)[0].pending);

    struct Restart
    {
        juce::File settingsFile, journalFile;
        AppSettings settings;
        AudioEngineHost host;
        AudioServiceScript script;
        std::unique_ptr<AppRouting> routing;

        Restart (const juce::File& folder, const juce::File& crashedJournal)
            : settingsFile (folder.getChildFile ("settings.xml")), journalFile (folder.getChildFile ("route-journal.json")),
              settings (settingsFile, false)
        {
            folder.createDirectory();
            crashedJournal.copyFileTo (journalFile);
        }

        void start (std::vector<flub::platform::AudioSessionInfo> sessions, const char* route = nullptr)
        {
            script.setSessions (std::move (sessions));
            routing = std::make_unique<AppRouting> (host, settings, std::make_unique<ScriptedRouter> (script), false);
            if (route != nullptr)
                routing->setRoute ("cs2", route);
            routing->start();
        }
    };

    // Restarted without the route (the app still runs, on the Game endpoint):
    // moved back to the system default and the journal is emptied, from
    // either crash point.
    int n = 0;
    for (const auto& crashed : { beforeMove, afterMove })
    {
        Restart r (temp.file ("restart" + juce::String (++n)), crashed);
        r.start ({ makeSession (100, "cs2.exe", game.c_str(), 7001), makeSession (300, "Discord.exe", "ep-headset", 7003) });
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return r.script.countMoves (100, "") == 1 && ! r.journalFile.exists(); }));
        CHECK (r.script.getMoves().size() == 1); // nothing else moved (Discord never was)
        r.routing->shutdown();
        CHECK (r.script.getMoves().size() == 1);
    }

    // Restarted with the route still assigned: the very process (pid and start
    // time match) is still on the Game endpoint, so nothing moves; a crash
    // before the move leaves "pending", which is moved again (the move may
    // never have happened).
    {
        Restart r (temp.file ("restart-assigned"), afterMove);
        r.start ({ makeSession (100, "cs2.exe", game.c_str(), 7001) }, "Game");
        REQUIRE (flubapptest::pumpMessagesUntil ([&]
        {
            const auto* cs2 = findRoutedApp (*r.routing, 100);
            return cs2 != nullptr && cs2->routed;
        }));
        CHECK (r.script.getMoves().empty());
        CHECK (readJournal (r.journalFile).size() == 1);
        r.routing->shutdown();
        CHECK (r.script.countMoves (100, "") == 1);
    }
    {
        Restart r (temp.file ("restart-assigned-pending"), beforeMove);
        r.start ({ makeSession (100, "cs2.exe", "ep-headset", 7001) }, "Game");
        REQUIRE (flubapptest::pumpMessagesUntil ([&]
        {
            const auto* cs2 = findRoutedApp (*r.routing, 100);
            return cs2 != nullptr && cs2->routed;
        }));
        CHECK (r.script.countMoves (100, game) == 1);
        CHECK (! readJournal (r.journalFile).front().pending);
        r.routing->shutdown();
    }

    // The pid now belongs to another program (another start time): it is not
    // "moved back"; cs2 stays in the journal until a process of it appears.
    {
        Restart r (temp.file ("restart-reused-pid"), afterMove);
        r.start ({ makeSession (100, "notepad.exe", "ep-headset", 9999) });
        waitForPasses (*r.routing, r.script, 2);
        CHECK (r.script.getMoves().empty());
        auto entries = readJournal (r.journalFile);
        REQUIRE (entries.size() == 1);
        CHECK (entries[0].executable == "cs2");
        CHECK (entries[0].processId == 0); // no process of it runs now

        r.script.setSessions ({ makeSession (100, "notepad.exe", "ep-headset", 9999), makeSession (101, "cs2.exe", game.c_str(), 9998) });
        REQUIRE (flubapptest::pumpMessagesUntil ([&]
        {
            r.routing->refresh();
            return r.script.countMoves (101, "") == 1 && ! r.journalFile.exists();
        }));
        CHECK (r.script.countMoves (100, "") == 0);
        r.routing->shutdown();
    }
}

TEST_CASE ("App: E47 RouteJournal keeps a start time beyond 2^53 and sets an unreadable file aside")
{
    const flubapptest::TempFolder temp;
    const auto file = temp.file ("route-journal.json");
    RouteJournal journal (file);
    RouteJournal::Entry e;
    e.processId = 4242;
    e.processStartTime = 133700000000000123ull; // a FILETIME: more digits than a double holds
    e.executable = "cs2";
    e.executablePath = juce::String (juce::CharPointer_UTF8 ("C:\\Spiele\\Counter-Strike \xc3\xa9\\cs2.exe"));
    e.endpoint = "{0.0.0.00000000}.{flub-game}";
    e.previousEndpoint = "{0.0.0.00000000}.{headset}";
    e.pending = true;
    REQUIRE (journal.write ({ e }));
    const auto back = RouteJournal (file).load();
    REQUIRE (back.size() == 1);
    CHECK (back[0] == e);

    REQUIRE (journal.write ({}));
    CHECK (! file.exists());

    REQUIRE (file.replaceWithText ("{\"version\": 1, \"entries\": [ {\"pid\": 1, "));
    CHECK (RouteJournal (file).load().empty());
    CHECK (! file.exists());
    CHECK (file.withFileExtension ("json.bad").existsAsFile());

    // No file (settings that are not persisted): writes succeed, nothing is kept.
    RouteJournal none ((juce::File()));
    CHECK (none.write ({ e }));
    CHECK (none.load().empty());
}

// =============================================================================
// A real crash: a child process (this test binary, running the "crash child"
// case below) routes cs2 through a file-backed fake audio service, and the
// parent kills it (SIGKILL / TerminateProcess) as soon as the move is in the
// service's state. The restart then moves the app back.
namespace
{
constexpr uint32_t kCrashPid = 4242;
constexpr uint64_t kCrashStartTime = 777;
constexpr const char* kCrashDirVariable = "FLUB_ROUTE_CRASH_DIR";
constexpr const char* kCrashChildCase = "App: E47 route journal crash child";

/** The fake OS audio service's state, in files both processes read: which
    endpoint pid 4242 (cs2) is assigned to ("" = the default) and every move. */
class FileRouter final : public flub::platform::AppAudioRouter
{
public:
    explicit FileRouter (juce::File folder) : dir (std::move (folder)) {}

    static juce::String assignedEndpoint (const juce::File& folder) { return folder.getChildFile ("service.txt").loadFileAsString(); }
    static juce::StringArray moves (const juce::File& folder)
    {
        auto lines = juce::StringArray::fromLines (folder.getChildFile ("moves.txt").loadFileAsString());
        lines.removeEmptyStrings();
        return lines;
    }

    bool isSupported() const override { return true; }

    std::vector<flub::platform::AudioSessionInfo> enumerateSessions() override
    {
        const auto assigned = assignedEndpoint (dir);
        return { makeSession (kCrashPid, "cs2.exe", assigned.isEmpty() ? "ep-headset" : assigned.toRawUTF8(), kCrashStartTime) };
    }

    bool setAppEndpoint (uint32_t processId, const std::string& endpointId, std::string& error) override
    {
        if (processId != kCrashPid)
        {
            error = "no such process (test)";
            return false;
        }
        dir.getChildFile ("moves.txt").appendText (juce::String (processId) + " [" + juce::String (endpointId) + "]\n");
        return dir.getChildFile ("service.txt").replaceWithText (juce::String (endpointId));
    }

    void openSystemRoutingSettings() override {}

private:
    juce::File dir;
};

void setCrashDirVariable (const juce::String& value)
{
#if defined(_WIN32)
    _putenv_s (kCrashDirVariable, value.toRawUTF8()); // "" removes it; the CRT keeps the process environment in step
#else
    if (value.isEmpty())
        unsetenv (kCrashDirVariable);
    else
        setenv (kCrashDirVariable, value.toRawUTF8(), 1);
#endif
}
} // namespace

TEST_CASE ("App: E47 route journal crash child (acts only as the child process of the SIGKILL / TerminateProcess test)")
{
    const auto dir = juce::SystemStats::getEnvironmentVariable (kCrashDirVariable, {});
    if (dir.isEmpty())
        return;

    const juce::File folder (dir);
    AppSettings settings (folder.getChildFile ("settings.xml"), false); // never saved: the restart starts without the route
    AudioEngineHost host;
    AppRouting routing (host, settings, std::make_unique<FileRouter> (folder), false);
    routing.setRoute ("cs2", "Game");
    routing.start();
    // The parent kills this process once the move is made; this is only a hang guard.
    flubapptest::pumpMessagesUntil ([] { return false; }, 10000);
}

TEST_CASE ("App: E47 route journal: killed (SIGKILL / TerminateProcess) after a move, the restart moves the app back")
{
    const flubapptest::TempFolder temp;
    const auto folder = temp.file ("crash");
    REQUIRE (folder.createDirectory().wasOk());
    const auto game = gameEndpointId (temp);

    juce::ChildProcess child;
    setCrashDirVariable (folder.getFullPathName());
    const bool started = child.start (juce::StringArray { juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName(),
                                                          kCrashChildCase },
                                      0);
    setCrashDirVariable ({});
    REQUIRE (started);

    // Kill it the moment the audio service has the app on the Game endpoint.
    const auto deadline = juce::Time::getMillisecondCounter() + 10000;
    while (FileRouter::assignedEndpoint (folder) != game && juce::Time::getMillisecondCounter() < deadline)
        juce::Thread::sleep (2);
    REQUIRE (FileRouter::assignedEndpoint (folder) == game);
    REQUIRE (child.kill());
    CHECK (child.waitForProcessToFinish (5000));

    // What the crash left: cs2 on the Game endpoint, and the journal naming it.
    const auto journalFile = folder.getChildFile ("route-journal.json");
    const auto entries = readJournal (journalFile);
    REQUIRE (entries.size() == 1);
    CHECK (entries[0].processId == kCrashPid);
    CHECK (entries[0].processStartTime == kCrashStartTime);
    CHECK (entries[0].executable == "cs2");
    CHECK (entries[0].endpoint == game);
    CHECK (entries[0].previousEndpoint == "ep-headset");
    CHECK ((FileRouter::moves (folder) == juce::StringArray { juce::String (kCrashPid) + " [" + game + "]" }));

    // The restart (the route was never saved) moves it back to the system
    // default, the endpoint it played to before, and empties the journal.
    AppSettings settings (folder.getChildFile ("settings.xml"), false);
    AudioEngineHost host;
    AppRouting routing (host, settings, std::make_unique<FileRouter> (folder), false);
    CHECK (routing.getRoutes().empty());
    routing.start();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return FileRouter::assignedEndpoint (folder).isEmpty() && ! journalFile.exists(); }));
    CHECK ((FileRouter::moves (folder) == juce::StringArray { juce::String (kCrashPid) + " [" + game + "]", juce::String (kCrashPid) + " []" }));
    CHECK (findRoutedApp (routing, kCrashPid) == nullptr || ! findRoutedApp (routing, kCrashPid)->routed);
    routing.shutdown();
    CHECK (FileRouter::moves (folder).size() == 2);
}
