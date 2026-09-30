// `flubsound-cli ctl` (tools/flubsound-cli/Ctl.h, docs/11 E56): the argument
// rules, the wire format, and the client against a real Server on a
// temporary socket with a scripted handler (the app's handler is tested in
// tests/app/test_app_ctl.cpp). Exit codes: 0 ok, 1 failed, 2 usage (refused
// by the CLI or by the app), 3 nothing listening.
#include "TestFramework.h"

#include "Ctl.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace flub::cli::ctl;

namespace
{
namespace fs = std::filesystem;

/** A private folder (owner only, as the server requires) with a socket path. */
struct SocketDir
{
    SocketDir()
    {
        std::random_device rd;
        folder = fs::temp_directory_path() / ("flub_ctl_" + std::to_string (rd()) + std::to_string (rd() % 1000));
        fs::create_directories (folder);
        fs::permissions (folder, fs::perms::owner_all, fs::perm_options::replace);
    }
    ~SocketDir()
    {
        std::error_code ec;
        fs::remove_all (folder, ec);
    }
    std::string socket() const { return (folder / "ctl.sock").string(); }

    fs::path folder;
};

int run (const std::vector<std::string>& args, std::string& out, std::string& err)
{
    out.clear();
    err.clear();
    return runCtl (args, out, err);
}
} // namespace

TEST_CASE ("Ctl: arguments - strip, value, canonical names and refusals")
{
    Request r;
    std::string error;
    REQUIRE (parseArguments ({ "boostup", "Game" }, r, error));
    CHECK (r.action == "BoostUp");
    CHECK (r.strip == "Game");
    CHECK (r.value.empty());

    REQUIRE (parseArguments ({ "BoostUp" }, r, error)); // the hotkey strip
    CHECK (r.strip.empty());

    REQUIRE (parseArguments ({ "Boost", "60" }, r, error)); // one argument: the value
    CHECK (r.strip.empty());
    CHECK (r.value == "60");
    REQUIRE (parseArguments ({ "Boost", "Music", "35" }, r, error));
    CHECK (r.strip == "Music");
    CHECK (r.value == "35");
    REQUIRE (parseArguments ({ "LoadPreset", "Game", "3f0c2a" }, r, error));
    CHECK (r.value == "3f0c2a");
    REQUIRE (parseArguments ({ "osd", "off" }, r, error));
    CHECK (r.action == "Osd");
    CHECK (r.value == "off");

    CHECK (! parseArguments ({}, r, error));
    CHECK (! parseArguments ({ "Explode" }, r, error));
    CHECK (error.find ("unknown action 'Explode'") != std::string::npos);
    CHECK (! parseArguments ({ "ToggleEnable", "Game" }, r, error)); // acts on every strip
    CHECK (! parseArguments ({ "ChatMixToChat", "Chat" }, r, error));
    CHECK (! parseArguments ({ "BoostUp", "Game", "10" }, r, error)); // no value
    CHECK (! parseArguments ({ "Boost" }, r, error));
    CHECK (error.find ("needs a value") != std::string::npos);
    CHECK (! parseArguments ({ "Boost", "Game", "60", "extra" }, r, error));
    CHECK (! parseArguments ({ "Osd", "Game", "on" }, r, error)); // no strip
    CHECK (! parseArguments ({ "BoostUp", "" }, r, error));
    CHECK (! parseArguments ({ "BoostUp", "Ga\tme" }, r, error));
    CHECK (! parseArguments ({ "Boost", "Game", std::string (2000, '5') }, r, error)); // over kMaxLineBytes

    // Every hotkey action (ids 1..11) is here once.
    std::vector<int> ids;
    for (const auto& a : actions())
        if (a.hotkeyId != 0)
            ids.push_back (a.hotkeyId);
    CHECK (ids.size() == 11);
    for (int id = 1; id <= 11; ++id)
        CHECK (std::count (ids.begin(), ids.end(), id) == 1);
}

TEST_CASE ("Ctl: wire format round trip and malformed lines")
{
    Request in { "BoostUp", "Game", "" };
    Request out;
    std::string error;
    const auto line = encodeRequest (in);
    CHECK (line == "FLUBCTL1\tBoostUp\tGame\t\n");
    REQUIRE (decodeRequest (line, out, error));
    CHECK (out.action == "BoostUp");
    CHECK (out.strip == "Game");

    CHECK (! decodeRequest ("GET / HTTP/1.1", out, error)); // e.g. a browser
    CHECK (! decodeRequest ("FLUBCTL1\tBoostUp\tGame", out, error));
    CHECK (! decodeRequest ("FLUBCTL1\tBoostUp\tGame\t\t", out, error));
    CHECK (! decodeRequest ("FLUBCTL1\tBoost\tGame\t", out, error)); // value missing
    CHECK (! decodeRequest ("FLUBCTL1\tToggleEnable\tGame\t", out, error));
    CHECK (! decodeRequest ("FLUBCTL1\tBoostUp\tGa\x01me\t", out, error));

    Reply reply { Status::Usage, "Unknown strip 'Gme'\nsecond line" };
    Reply back;
    const auto replyLine = encodeReply (reply);
    CHECK (replyLine == "usage\tUnknown strip 'Gme' second line\n"); // control characters flattened
    REQUIRE (decodeReply (replyLine, back));
    CHECK (back.status == Status::Usage);
    CHECK (back.message == "Unknown strip 'Gme' second line");
    CHECK (! decodeReply ("maybe\tx", back));
    CHECK (exitCodeFor (Status::Ok) == 0);
    CHECK (exitCodeFor (Status::Failed) == 1);
    CHECK (exitCodeFor (Status::Usage) == 2);

    CHECK (! defaultSocketPath().empty());
}

TEST_CASE ("Ctl: client and server - reply printed, exit codes, not running")
{
    SocketDir dir;
    std::vector<Request> seen;
    Server server;
    std::string error;
    REQUIRE (server.start (dir.socket(), [&seen] (const Request& r) -> Reply
                           {
                               seen.push_back (r);
                               if (r.strip == "Nowhere")
                                   return { Status::Usage, "Unknown strip 'Nowhere'" };
                               if (r.action == "NextPreset")
                                   return { Status::Failed, "No presets available" };
                               return { Status::Ok, (r.strip.empty() ? "Game" : r.strip) + ": " + r.action };
                           },
                           error));
    CHECK (server.isRunning());

    std::string out, err;
    CHECK (run ({ "BoostUp", "Game", "--socket", dir.socket() }, out, err) == kCtlOk);
    CHECK (out == "Game: BoostUp\n");
    CHECK (err.empty());
    CHECK (run ({ "--socket", dir.socket(), "boost", "Music", "35" }, out, err) == kCtlOk);
    CHECK (out == "Music: Boost\n");
    CHECK (run ({ "BoostUp", "Nowhere", "--socket", dir.socket() }, out, err) == kCtlUsage); // refused by the app
    CHECK (err == "error: Unknown strip 'Nowhere'\n");
    CHECK (run ({ "NextPreset", "--socket", dir.socket() }, out, err) == kCtlFailed);
    REQUIRE (seen.size() == 4);
    CHECK (seen[1].action == "Boost");
    CHECK (seen[1].value == "35");

    // Refused before sending: nothing reaches the app.
    CHECK (run ({ "Explode", "--socket", dir.socket() }, out, err) == kCtlUsage);
    CHECK (err.find ("unknown action 'Explode'") != std::string::npos);
    CHECK (run ({ "BoostUp", "--bogus" }, out, err) == kCtlUsage);
    CHECK (seen.size() == 4);
    CHECK (server.getHandledCount() == 4);

    // A second server on a live socket is refused.
    Server second;
    CHECK (! second.start (dir.socket(), [] (const Request&) { return Reply { Status::Ok, "" }; }, error));
    CHECK (error.find ("another Flubsound instance") != std::string::npos);

    const auto started = std::chrono::steady_clock::now();
    server.stop();
    CHECK (std::chrono::steady_clock::now() - started < std::chrono::milliseconds (500));
    CHECK (! fs::exists (dir.socket()));
    CHECK (run ({ "BoostUp", "--socket", dir.socket() }, out, err) == kCtlNotRunning);
    CHECK (err.find ("Flubsound is not running") != std::string::npos);

    // --list and --help need no running app.
    CHECK (run ({ "--list", "--socket", dir.socket() }, out, err) == kCtlOk);
    for (const auto& a : actions())
        CHECK (out.find (std::string ("  ") + a.name) != std::string::npos);
    CHECK (out.find ("Boost [strip] <0-100>") != std::string::npos);
    CHECK (run ({ "--help" }, out, err) == kCtlOk);
    CHECK (out.find ("Exit codes") != std::string::npos);
}

TEST_CASE ("Ctl: a stale socket file is replaced; a shared folder is refused")
{
    SocketDir dir;
    { std::ofstream (dir.socket()) << "stale"; } // left by a crash
    Server server;
    std::string error;
    REQUIRE (server.start (dir.socket(), [] (const Request&) { return Reply { Status::Ok, "fine" }; }, error));
    std::string out, err;
    CHECK (run ({ "Show", "--socket", dir.socket() }, out, err) == kCtlOk);
    CHECK (out == "fine\n");
    server.stop();

#if ! defined(_WIN32)
    // A folder others can write to could hold someone else's socket.
    fs::permissions (dir.folder, fs::perms::owner_all | fs::perms::group_all | fs::perms::others_all, fs::perm_options::replace);
    Server open;
    CHECK (! open.start (dir.socket(), [] (const Request&) { return Reply {}; }, error));
    CHECK (error.find ("not a private folder") != std::string::npos);
#endif
}
