// Flubsound Pro CLI - `flubsound-cli ctl` (docs/11 E56): remote control of
// the running desktop app over a local, user-private socket.
//
//   flubsound-cli ctl <action> [strip] [value]      e.g. ctl BoostUp Game
//   flubsound-cli ctl --list                        the actions
//   FlubsoundPro --ctl <action> [strip] [value]     the same, forwarded by
//                                                   a second app instance
//
// The actions are the hotkey actions (AppSettings::HotkeyAction, the same
// code path and the same feedback text as the hotkeys, including the
// on-screen display) plus Boost <0-100>, LoadPreset <uuid>, Show and the
// OSD settings. `strip` names a strip ("Game", "Chat", ...); without it a
// strip action goes to the hotkey strip (EngineController::getHotkeyStrip).
//
// Transport: one AF_UNIX stream socket per user on every platform (Windows 10
// 1803+ has AF_UNIX too), so the protocol code is shared and nothing listens
// on the network:
//   Linux    $XDG_RUNTIME_DIR/flubsound-ctl.sock (else /tmp/flubsound-<uid>/)
//   macOS    $TMPDIR/flubsound-ctl.sock (per-user; else /tmp/flubsound-<uid>/)
//   Windows  %LOCALAPPDATA%\Flubsound\ctl.sock
// The folder is the user's own (0700 on POSIX, the profile's ACL on Windows);
// on Linux and macOS the server also refuses a peer of another user id.
//
// Protocol (UTF-8, one request per connection, lines end with '\n'):
//   request  "FLUBCTL1\t<action>\t<strip>\t<value>\n"   (fields may be empty)
//   reply    "ok\t<message>\n" | "usage\t<message>\n" | "failed\t<message>\n"
// No field may hold a tab, a line break or another control character; a
// request is at most kMaxLineBytes.
//
// This file is plain C++ (no JUCE): the CLI uses the client, the desktop app
// compiles it too and runs the Server (shell/RemoteControl.*), and both sides
// share the action table, the argument rules and the wire format.
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace flub::cli::ctl
{
/** Exit codes of `ctl`: 0 ok, 1 the action failed in the app, 2 usage error
    (unknown action, strip or value; refused before or by the app), 3 no
    running Flubsound instance answered. */
enum ExitCode : int
{
    kCtlOk = 0,
    kCtlFailed = 1,
    kCtlUsage = 2,
    kCtlNotRunning = 3
};

constexpr size_t kMaxLineBytes = 1024;

struct ActionInfo
{
    const char* name;      // matched ignoring case
    int hotkeyId;          // AppSettings::HotkeyAction value; 0 = not a hotkey action
    bool takesStrip;       // an optional strip name
    bool needsValue;       // a required value
    const char* valueHint; // "<0-100>", "<uuid>", ... (empty without a value)
    const char* help;
};

/** Every action, in the order `ctl --list` prints them. */
const std::vector<ActionInfo>& actions();

/** The action called `name` (ignoring case), or nullptr. */
const ActionInfo* findAction (const std::string& name);

struct Request
{
    std::string action, strip, value;
};

/** `<action> [strip] [value]` -> request. An action with a value and one
    argument takes it as the value (`Boost 60`: the hotkey strip); two
    arguments are strip and value. Refuses an unknown action, a missing
    value, a strip or value the action does not take, and a field with a
    control character. The action is written with its canonical name. */
bool parseArguments (const std::vector<std::string>& args, Request& request, std::string& error);

enum class Status
{
    Ok,
    Usage,
    Failed
};

struct Reply
{
    Status status = Status::Failed;
    std::string message;
};

std::string encodeRequest (const Request& request);
/** Parses one request line (with or without its '\n') and re-checks it with
    parseArguments' rules. */
bool decodeRequest (const std::string& line, Request& request, std::string& error);
std::string encodeReply (const Reply& reply);
bool decodeReply (const std::string& line, Reply& reply);

/** The exit code for a reply's status (kCtlOk / kCtlUsage / kCtlFailed). */
int exitCodeFor (Status status) noexcept;

/** The per-user socket path (see the header comment); empty if no
    location can be determined. */
std::string defaultSocketPath();

enum class SendResult
{
    Ok,         // a reply was received and decoded
    NotRunning, // no socket, or nobody listening on it
    Error       // connected, but the exchange failed (timeout, bad reply)
};

/** Sends one request and waits up to timeoutMs for the reply. */
SendResult sendRequest (const std::string& socketPath, const Request& request, Reply& reply, std::string& error,
                        int timeoutMs = 5000);

/** The listening side (the desktop app). One background thread accepts one
    connection at a time, reads the request (1 s timeout), calls the handler
    ON THAT THREAD and writes its reply. The handler must answer promptly
    (the app hops to its message thread and waits a bounded time). */
class Server
{
public:
    using Handler = std::function<Reply (const Request&)>;

    Server();
    ~Server(); // stop()
    Server (const Server&) = delete;
    Server& operator= (const Server&) = delete;

    /** Creates the socket's folder when needed (0700 on POSIX), refuses a
        path another live server answers on, replaces a stale socket file,
        and starts listening. */
    bool start (const std::string& socketPath, Handler handler, std::string& error);

    /** Stops the thread (within about 100 ms) and removes the socket file.
        Idempotent. */
    void stop();

    bool isRunning() const noexcept { return thread.joinable(); }
    const std::string& getPath() const noexcept { return path; }

    /** Connections handled so far (tests). */
    int getHandledCount() const noexcept { return handled.load(); }

private:
    void run();

    struct Native;
    std::unique_ptr<Native> native;
    std::string path;
    Handler handler;
    std::thread thread;
    std::atomic<bool> stopping { false };
    std::atomic<int> handled { 0 };
};

extern const char* const kCtlHelp;

/** Runs `ctl <args>` (the words after "ctl") and returns the exit code; the
    reply's message goes to `out`, errors to `err` (each line ends with
    '\n'). Options: --socket <path> (default defaultSocketPath()), --list,
    --help. */
int runCtl (const std::vector<std::string>& args, std::string& out, std::string& err);

/** The same, printing to stdout / stderr (main.cpp). */
int runCtl (const std::vector<std::string>& args);
} // namespace flub::cli::ctl
