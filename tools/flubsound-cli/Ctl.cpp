// Flubsound Pro CLI - `flubsound-cli ctl` and its socket transport (docs/11
// E56); see Ctl.h for the protocol.
#include "Ctl.h"

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <afunix.h>
    #include <windows.h>
    #if defined(_MSC_VER)
        #pragma comment(lib, "ws2_32.lib")
    #endif
#else
    #include <cerrno>
    #include <fcntl.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <sys/un.h>
    #include <unistd.h>
#endif

#include "Utf8Windows.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace flub::cli::ctl
{
namespace
{
// ---- action table ----------------------------------------------------------
// hotkeyId is the AppSettings::HotkeyAction value (tests/app/test_app_ctl.cpp
// checks every hotkey action is here under its enumerator's name).
const std::vector<ActionInfo> kActions = {
    { "ToggleEnable", 1, false, false, "", "enable / disable processing (every strip)" },
    { "ToggleMode", 2, true, false, "", "toggle Music / Gaming mode" },
    { "BoostUp", 3, true, false, "", "Boost Intensity +10 %" },
    { "BoostDown", 4, true, false, "", "Boost Intensity -10 %" },
    { "NextPreset", 5, true, false, "", "next preset" },
    { "PreviousPreset", 6, true, false, "", "previous preset" },
    { "ToggleFocus", 7, true, false, "", "Focus: latched Footsteps 100 % (Gaming mode)" },
    { "ChatMixToChat", 8, false, false, "", "ChatMix one step towards Chat" },
    { "ChatMixToGame", 9, false, false, "", "ChatMix one step towards Game" },
    { "ToggleNight", 10, true, false, "", "Night listening on / off" },
    { "ToggleBypass", 11, true, false, "", "bypass that strip only / process it again" },
    { "Boost", 0, true, true, "<0-100>", "set Boost Intensity (percent)" },
    { "LoadPreset", 0, true, true, "<uuid>", "load a preset by its uuid (or id; see the preset's file)" },
    { "Show", 0, false, false, "", "show the main window" },
    { "Osd", 0, false, true, "<on|off>", "the on-screen display of hotkey actions" },
    { "Earcon", 0, false, true, "<off|fullscreen|always>", "a short sound with each action: never, while a game runs in exclusive fullscreen (the display cannot show there), or always" },
};

constexpr const char* kRequestMagic = "FLUBCTL1";

bool equalsIgnoringCase (const std::string& a, const char* b)
{
    const size_t n = std::strlen (b);
    if (a.size() != n)
        return false;
    for (size_t i = 0; i < n; ++i)
    {
        const auto ca = static_cast<unsigned char> (a[i]);
        const auto cb = static_cast<unsigned char> (b[i]);
        const auto la = (ca >= 'A' && ca <= 'Z') ? ca + 32 : ca;
        const auto lb = (cb >= 'A' && cb <= 'Z') ? cb + 32 : cb;
        if (la != lb)
            return false;
    }
    return true;
}

bool hasControlCharacter (const std::string& s)
{
    for (const char ch : s)
        if (static_cast<unsigned char> (ch) < 0x20 || ch == 0x7f)
            return true;
    return false;
}

/** The request's fields against the action's rules (parseArguments and
    decodeRequest). Writes the canonical action name. */
bool validate (Request& request, std::string& error)
{
    const auto* info = findAction (request.action);
    if (info == nullptr)
    {
        error = "unknown action '" + request.action + "' (see `flubsound-cli ctl --list`)";
        return false;
    }
    request.action = info->name;
    const std::string name = info->name;
    if (hasControlCharacter (request.strip) || hasControlCharacter (request.value))
    {
        error = "control characters are not allowed in an argument";
        return false;
    }
    if (! info->takesStrip && ! request.strip.empty())
    {
        error = name + (info->needsValue ? " takes only a value " + std::string (info->valueHint) : " takes no strip");
        return false;
    }
    if (info->needsValue && request.value.empty())
    {
        error = name + " needs a value " + info->valueHint;
        return false;
    }
    if (! info->needsValue && ! request.value.empty())
    {
        error = name + (info->takesStrip ? " takes at most a strip name" : " takes no arguments");
        return false;
    }
    if (encodeRequest (request).size() > kMaxLineBytes)
    {
        error = "the request is too long";
        return false;
    }
    return true;
}

std::string sanitised (const std::string& s)
{
    std::string out = s;
    for (auto& ch : out)
        if (static_cast<unsigned char> (ch) < 0x20 || ch == 0x7f)
            ch = ' ';
    return out;
}

// ---- sockets ---------------------------------------------------------------
#if defined(_WIN32)
using Socket = SOCKET;
const Socket kNoSocket = INVALID_SOCKET;
using SockLen = int;

void closeSocket (Socket s) { closesocket (s); }

bool ensureSockets()
{
    static const bool ok = []
    {
        WSADATA data {};
        return WSAStartup (MAKEWORD (2, 2), &data) == 0;
    }();
    return ok;
}

std::string lastSocketError()
{
    return "socket error " + std::to_string (WSAGetLastError());
}

bool setBlocking (Socket s, bool blocking)
{
    u_long nonBlocking = blocking ? 0 : 1;
    return ioctlsocket (s, FIONBIO, &nonBlocking) == 0;
}

void setTimeouts (Socket s, int ms)
{
    const DWORD t = static_cast<DWORD> (ms);
    setsockopt (s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*> (&t), sizeof (t));
    setsockopt (s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*> (&t), sizeof (t));
}

Socket makeSocket() { return ::socket (AF_UNIX, SOCK_STREAM, 0); }

long long sendSome (Socket s, const char* data, size_t size)
{
    return ::send (s, data, static_cast<int> (size), 0);
}

long long receiveSome (Socket s, char* data, size_t size)
{
    return ::recv (s, data, static_cast<int> (size), 0);
}

bool interrupted() { return false; }

void removeSocketFile (const std::string& path)
{
    DeleteFileW (utf8ToWide (path).c_str());
}
#else
using Socket = int;
constexpr Socket kNoSocket = -1;
using SockLen = socklen_t;

void closeSocket (Socket s) { ::close (s); }

bool ensureSockets() { return true; }

std::string lastSocketError() { return std::strerror (errno); }

bool setBlocking (Socket s, bool blocking)
{
    const int flags = ::fcntl (s, F_GETFL, 0);
    if (flags < 0)
        return false;
    return ::fcntl (s, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK)) == 0;
}

void setTimeouts (Socket s, int ms)
{
    timeval tv {};
    tv.tv_sec = static_cast<decltype (tv.tv_sec)> (ms / 1000);
    tv.tv_usec = static_cast<decltype (tv.tv_usec)> ((ms % 1000) * 1000);
    ::setsockopt (s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv));
    ::setsockopt (s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof (tv));
}

/** macOS / BSD have no MSG_NOSIGNAL: a write to a closed peer must not
    raise SIGPIPE (it would end the app). Set on every socket, accepted ones
    included. */
void noSigPipe ([[maybe_unused]] Socket s)
{
   #if defined(SO_NOSIGPIPE)
    const int on = 1;
    ::setsockopt (s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof (on));
   #endif
}

Socket makeSocket()
{
   #if defined(__linux__)
    return ::socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
   #else
    const Socket s = ::socket (AF_UNIX, SOCK_STREAM, 0);
    if (s != kNoSocket)
    {
        ::fcntl (s, F_SETFD, FD_CLOEXEC);
        noSigPipe (s);
    }
    return s;
   #endif
}

long long sendSome (Socket s, const char* data, size_t size)
{
   #if defined(MSG_NOSIGNAL)
    return static_cast<long long> (::send (s, data, size, MSG_NOSIGNAL));
   #else
    return static_cast<long long> (::send (s, data, size, 0));
   #endif
}

long long receiveSome (Socket s, char* data, size_t size)
{
    return static_cast<long long> (::recv (s, data, size, 0));
}

bool interrupted() { return errno == EINTR; }

void removeSocketFile (const std::string& path) { ::unlink (path.c_str()); }

/** The peer runs as this user (Linux SO_PEERCRED; getpeereid elsewhere). */
bool peerIsThisUser (Socket s)
{
   #if defined(__linux__)
    ucred cred {};
    socklen_t len = sizeof (cred);
    if (::getsockopt (s, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
        return false;
    return cred.uid == ::geteuid();
   #else
    uid_t uid = 0;
    gid_t gid = 0;
    if (::getpeereid (s, &uid, &gid) != 0)
        return false;
    return uid == ::geteuid();
   #endif
}
#endif

struct ScopedSocket
{
    explicit ScopedSocket (Socket s = kNoSocket) : socket (s) {}
    ~ScopedSocket() { reset(); }
    ScopedSocket (const ScopedSocket&) = delete;
    ScopedSocket& operator= (const ScopedSocket&) = delete;

    void reset (Socket s = kNoSocket)
    {
        if (socket != kNoSocket)
            closeSocket (socket);
        socket = s;
    }

    Socket socket;
};

bool makeAddress (const std::string& path, sockaddr_un& address, SockLen& length, std::string& error)
{
    std::memset (&address, 0, sizeof (address));
    address.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof (address.sun_path))
    {
        error = path.empty() ? "no socket path" : "the socket path is too long: " + path;
        return false;
    }
    std::memcpy (address.sun_path, path.data(), path.size());
    length = static_cast<SockLen> (offsetof (sockaddr_un, sun_path) + path.size() + 1);
    return true;
}

bool sendAll (Socket s, const std::string& data)
{
    size_t done = 0;
    while (done < data.size())
    {
        const long long n = sendSome (s, data.data() + done, data.size() - done);
        if (n < 0 && interrupted())
            continue;
        if (n <= 0)
            return false;
        done += static_cast<size_t> (n);
    }
    return true;
}

/** Reads up to the first '\n' (dropped); false on a timeout, an early end or
    more than maxBytes without one. */
bool receiveLine (Socket s, std::string& line, size_t maxBytes)
{
    line.clear();
    char buffer[256];
    while (line.size() <= maxBytes)
    {
        const long long n = receiveSome (s, buffer, sizeof (buffer));
        if (n < 0 && interrupted())
            continue;
        if (n <= 0)
            return false;
        line.append (buffer, static_cast<size_t> (n));
        if (const auto end = line.find ('\n'); end != std::string::npos)
        {
            line.resize (end);
            return true;
        }
    }
    return false;
}

/** Connected socket, or kNoSocket with `error`. */
Socket connectTo (const std::string& path, int timeoutMs, std::string& error)
{
    sockaddr_un address {};
    SockLen length = 0;
    if (! ensureSockets() || ! makeAddress (path, address, length, error))
    {
        if (error.empty())
            error = "sockets are not available";
        return kNoSocket;
    }
    ScopedSocket s (makeSocket());
    if (s.socket == kNoSocket)
    {
        error = lastSocketError();
        return kNoSocket;
    }
    if (::connect (s.socket, reinterpret_cast<const sockaddr*> (&address), length) != 0)
    {
        error = lastSocketError();
        return kNoSocket;
    }
    setTimeouts (s.socket, timeoutMs);
    const Socket connected = s.socket;
    s.socket = kNoSocket;
    return connected;
}

std::string parentFolder (const std::string& path)
{
   #if defined(_WIN32)
    const auto slash = path.find_last_of ("\\/");
   #else
    const auto slash = path.find_last_of ('/');
   #endif
    return slash == std::string::npos || slash == 0 ? std::string() : path.substr (0, slash);
}

/** Creates the socket's folder if needed; on POSIX it must be this user's
    and writable by nobody else. */
bool prepareFolder (const std::string& path, std::string& error)
{
    const auto folder = parentFolder (path);
    if (folder.empty())
        return true;
   #if defined(_WIN32)
    const auto wide = utf8ToWide (folder);
    if (! CreateDirectoryW (wide.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        error = "cannot create " + folder;
        return false;
    }
    return true;
   #else
    if (::mkdir (folder.c_str(), 0700) != 0 && errno != EEXIST)
    {
        error = "cannot create " + folder + ": " + lastSocketError();
        return false;
    }
    struct stat info {};
    if (::lstat (folder.c_str(), &info) != 0 || ! S_ISDIR (info.st_mode) || info.st_uid != ::geteuid()
        || (info.st_mode & (S_IWGRP | S_IWOTH)) != 0)
    {
        error = folder + " is not a private folder of this user";
        return false;
    }
    return true;
   #endif
}

bool waitReadable (Socket s, int timeoutMs)
{
   #if defined(_WIN32)
    fd_set readable;
    FD_ZERO (&readable);
    FD_SET (s, &readable);
    timeval tv {};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    return ::select (0, &readable, nullptr, nullptr, &tv) > 0;
   #else
    pollfd fd {};
    fd.fd = s;
    fd.events = POLLIN;
    return ::poll (&fd, 1, timeoutMs) > 0 && (fd.revents & POLLIN) != 0;
   #endif
}

#if defined(_WIN32)
bool isAscii (const std::string& s)
{
    for (const char ch : s)
        if (static_cast<unsigned char> (ch) >= 0x80)
            return false;
    return true;
}
#endif
} // namespace

// ---- actions and wire format ----------------------------------------------
const std::vector<ActionInfo>& actions() { return kActions; }

const ActionInfo* findAction (const std::string& name)
{
    for (const auto& info : kActions)
        if (equalsIgnoringCase (name, info.name))
            return &info;
    return nullptr;
}

bool parseArguments (const std::vector<std::string>& args, Request& request, std::string& error)
{
    request = {};
    if (args.empty())
    {
        error = "no action given (see `flubsound-cli ctl --list`)";
        return false;
    }
    for (const auto& arg : args)
        if (arg.empty())
        {
            error = "empty argument";
            return false;
        }

    const auto* info = findAction (args[0]);
    if (info == nullptr)
    {
        error = "unknown action '" + args[0] + "' (see `flubsound-cli ctl --list`)";
        return false;
    }
    request.action = info->name;
    const std::string name = info->name;
    const size_t rest = args.size() - 1;
    if (info->needsValue)
    {
        if (rest == 0)
        {
            error = name + " needs a value " + info->valueHint;
            return false;
        }
        if (rest == 1)
            request.value = args[1];
        else if (rest == 2 && info->takesStrip)
        {
            request.strip = args[1];
            request.value = args[2];
        }
        else
        {
            error = name + " takes " + (info->takesStrip ? "[strip] " : "") + info->valueHint;
            return false;
        }
    }
    else if (rest == 1 && info->takesStrip)
        request.strip = args[1];
    else if (rest != 0)
    {
        error = name + (info->takesStrip ? " takes at most a strip name" : " takes no arguments");
        return false;
    }
    return validate (request, error);
}

std::string encodeRequest (const Request& request)
{
    return std::string (kRequestMagic) + '\t' + request.action + '\t' + request.strip + '\t' + request.value + '\n';
}

bool decodeRequest (const std::string& line, Request& request, std::string& error)
{
    request = {};
    std::string text = line;
    while (! text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();

    std::vector<std::string> fields;
    size_t start = 0;
    for (;;)
    {
        const auto tab = text.find ('\t', start);
        fields.push_back (text.substr (start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos)
            break;
        start = tab + 1;
    }
    if (fields.size() != 4 || fields[0] != kRequestMagic)
    {
        error = "not a Flubsound control request";
        return false;
    }
    request.action = fields[1];
    request.strip = fields[2];
    request.value = fields[3];
    return validate (request, error);
}

std::string encodeReply (const Reply& reply)
{
    const char* word = reply.status == Status::Ok ? "ok" : (reply.status == Status::Usage ? "usage" : "failed");
    return std::string (word) + '\t' + sanitised (reply.message) + '\n';
}

bool decodeReply (const std::string& line, Reply& reply)
{
    const auto tab = line.find ('\t');
    if (tab == std::string::npos)
        return false;
    const auto word = line.substr (0, tab);
    if (word == "ok")
        reply.status = Status::Ok;
    else if (word == "usage")
        reply.status = Status::Usage;
    else if (word == "failed")
        reply.status = Status::Failed;
    else
        return false;
    reply.message = line.substr (tab + 1);
    while (! reply.message.empty() && (reply.message.back() == '\n' || reply.message.back() == '\r'))
        reply.message.pop_back();
    return true;
}

int exitCodeFor (Status status) noexcept
{
    switch (status)
    {
        case Status::Ok: return kCtlOk;
        case Status::Usage: return kCtlUsage;
        case Status::Failed: return kCtlFailed;
    }
    return kCtlFailed;
}

std::string defaultSocketPath()
{
   #if defined(_WIN32)
    wchar_t buffer[MAX_PATH + 1] {};
    const DWORD n = GetEnvironmentVariableW (L"LOCALAPPDATA", buffer, MAX_PATH);
    if (n == 0 || n > MAX_PATH)
        return {};
    std::wstring folder (buffer, n);
    auto utf8 = wideToUtf8 (folder.c_str());
    if (! isAscii (utf8))
    {
        // sockaddr_un holds a narrow path: prefer the 8.3 form of a
        // non-ASCII profile folder where the volume has one.
        wchar_t shortPath[MAX_PATH + 1] {};
        const DWORD m = GetShortPathNameW (folder.c_str(), shortPath, MAX_PATH);
        if (m > 0 && m <= MAX_PATH)
            if (const auto candidate = wideToUtf8 (shortPath); isAscii (candidate))
                utf8 = candidate;
    }
    return utf8 + "\\Flubsound\\ctl.sock";
   #else
    const auto fallback = "/tmp/flubsound-" + std::to_string (static_cast<unsigned long> (::geteuid())) + "/ctl.sock";
    constexpr size_t kMaxPath = sizeof (sockaddr_un::sun_path) - 1;
    #if defined(__APPLE__)
    const char* base = std::getenv ("TMPDIR"); // per-user, private (/var/folders/...)
    #else
    const char* base = std::getenv ("XDG_RUNTIME_DIR"); // per-user, 0700 (systemd-logind)
    #endif
    if (base != nullptr && base[0] == '/')
    {
        std::string folder (base);
        while (folder.size() > 1 && folder.back() == '/')
            folder.pop_back();
        const auto path = folder + "/flubsound-ctl.sock";
        if (path.size() <= kMaxPath)
            return path;
    }
    return fallback;
   #endif
}

SendResult sendRequest (const std::string& socketPath, const Request& request, Reply& reply, std::string& error, int timeoutMs)
{
    reply = {};
    ScopedSocket s (connectTo (socketPath, timeoutMs, error));
    if (s.socket == kNoSocket)
        return SendResult::NotRunning;

    if (! sendAll (s.socket, encodeRequest (request)))
    {
        error = "could not send the request: " + lastSocketError();
        return SendResult::Error;
    }
    std::string line;
    if (! receiveLine (s.socket, line, kMaxLineBytes * 4))
    {
        error = "no reply from Flubsound (timed out)";
        return SendResult::Error;
    }
    if (! decodeReply (line, reply))
    {
        error = "unreadable reply from Flubsound";
        return SendResult::Error;
    }
    return SendResult::Ok;
}

// ---- server ----------------------------------------------------------------
struct Server::Native
{
    ScopedSocket listener;
};

Server::Server() : native (std::make_unique<Native>()) {}

Server::~Server() { stop(); }

bool Server::start (const std::string& socketPath, Handler newHandler, std::string& error)
{
    if (isRunning())
    {
        error = "already listening on " + path;
        return false;
    }
    sockaddr_un address {};
    SockLen length = 0;
    if (! ensureSockets())
    {
        error = "sockets are not available";
        return false;
    }
    if (! makeAddress (socketPath, address, length, error) || ! prepareFolder (socketPath, error))
        return false;

    // A live server (another instance) keeps its socket; a stale file left
    // by a crash is replaced.
    {
        std::string ignored;
        ScopedSocket probe (connectTo (socketPath, 200, ignored));
        if (probe.socket != kNoSocket)
        {
            error = "another Flubsound instance is listening on " + socketPath;
            return false;
        }
    }
    removeSocketFile (socketPath);

    ScopedSocket listener (makeSocket());
    if (listener.socket == kNoSocket || ::bind (listener.socket, reinterpret_cast<const sockaddr*> (&address), length) != 0)
    {
        error = "cannot listen on " + socketPath + ": " + lastSocketError();
        return false;
    }
   #if ! defined(_WIN32)
    ::chmod (socketPath.c_str(), 0600);
   #endif
    if (::listen (listener.socket, 4) != 0 || ! setBlocking (listener.socket, false))
    {
        error = "cannot listen on " + socketPath + ": " + lastSocketError();
        removeSocketFile (socketPath);
        return false;
    }

    native->listener.reset (listener.socket);
    listener.socket = kNoSocket;
    path = socketPath;
    handler = std::move (newHandler);
    stopping = false;
    thread = std::thread ([this] { run(); });
    return true;
}

void Server::stop()
{
    if (! thread.joinable())
        return;
    stopping = true;
    thread.join();
    native->listener.reset();
    removeSocketFile (path);
    handler = nullptr;
}

void Server::run()
{
    const Socket listener = native->listener.socket;
    while (! stopping.load())
    {
        if (! waitReadable (listener, 100))
            continue;
        ScopedSocket client (::accept (listener, nullptr, nullptr));
        if (client.socket == kNoSocket)
            continue;
        // Accepted sockets inherit non-blocking mode on some systems.
        setBlocking (client.socket, true);
        setTimeouts (client.socket, 1000);
       #if ! defined(_WIN32)
        noSigPipe (client.socket);
        if (! peerIsThisUser (client.socket))
            continue;
       #endif

        Reply reply;
        std::string line, error;
        Request request;
        if (! receiveLine (client.socket, line, kMaxLineBytes))
            reply = { Status::Usage, "incomplete request" };
        else if (! decodeRequest (line, request, error))
            reply = { Status::Usage, error };
        else
        {
            try
            {
                reply = handler != nullptr ? handler (request) : Reply { Status::Failed, "not ready" };
            }
            catch (...)
            {
                reply = { Status::Failed, "the action failed" };
            }
        }
        sendAll (client.socket, encodeReply (reply));
        ++handled;
    }
}

// ---- the command -------------------------------------------------------------
const char* const kCtlHelp = R"(Usage: flubsound-cli ctl <action> [strip] [value] [--socket <path>]
       flubsound-cli ctl --list

Sends one action to the running Flubsound desktop app and prints its
feedback (the same text the on-screen display shows), e.g.

  flubsound-cli ctl BoostUp Game        Game: Boost 60%
  flubsound-cli ctl Boost Music 35      Music: Boost 35%
  flubsound-cli ctl LoadPreset Game 3f0c...   (a preset's uuid)
  flubsound-cli ctl ToggleFocus         (the hotkey strip)

Without a strip, a strip action goes to the hotkey strip (Settings >
Hotkeys, or the active automatic profile's). `--list` prints every action.
The app answers on a socket only this user can reach (--socket: another
path; the default is printed by --list).

Exit codes: 0 ok, 1 the action failed, 2 usage error (unknown action, strip
or value), 3 Flubsound is not running.
)";

int runCtl (const std::vector<std::string>& args, std::string& out, std::string& err)
{
    std::vector<std::string> words;
    std::string socketPath;
    bool list = false;
    for (size_t i = 0; i < args.size(); ++i)
    {
        const auto& a = args[i];
        if (a == "--help" || a == "-h")
        {
            out += kCtlHelp;
            return kCtlOk;
        }
        if (a == "--list")
            list = true;
        else if (a == "--socket")
        {
            if (i + 1 >= args.size())
            {
                err += "error: --socket needs a path\n";
                return kCtlUsage;
            }
            socketPath = args[++i];
        }
        else if (a.size() > 2 && a.compare (0, 2, "--") == 0)
        {
            err += "error: unknown option " + a + " (see `flubsound-cli ctl --help`)\n";
            return kCtlUsage;
        }
        else
            words.push_back (a);
    }
    if (socketPath.empty())
        socketPath = defaultSocketPath();

    if (list)
    {
        out += "Actions ([strip]: a strip name such as Game or Chat; the hotkey strip when left out):\n";
        for (const auto& info : kActions)
        {
            std::string usage = info.name;
            if (info.takesStrip)
                usage += " [strip]";
            if (info.needsValue)
                usage += std::string (" ") + info.valueHint;
            if (usage.size() < 34)
                usage.resize (34, ' ');
            out += "  " + usage + " " + info.help + "\n";
        }
        out += "Socket: " + (socketPath.empty() ? std::string ("(none)") : socketPath) + "\n";
        return kCtlOk;
    }

    Request request;
    std::string error;
    if (! parseArguments (words, request, error))
    {
        err += "error: " + error + "\n";
        return kCtlUsage;
    }
    if (socketPath.empty())
    {
        err += "error: no location for the control socket\n";
        return kCtlNotRunning;
    }

    Reply reply;
    switch (sendRequest (socketPath, request, reply, error))
    {
        case SendResult::NotRunning:
            err += "error: Flubsound is not running (nothing answers on " + socketPath + ")\n";
            return kCtlNotRunning;
        case SendResult::Error:
            err += "error: " + error + "\n";
            return kCtlFailed;
        case SendResult::Ok: break;
    }
    if (reply.status == Status::Ok)
    {
        out += reply.message + "\n";
        return kCtlOk;
    }
    err += "error: " + reply.message + "\n";
    return exitCodeFor (reply.status);
}

int runCtl (const std::vector<std::string>& args)
{
    std::string out, err;
    const int code = runCtl (args, out, err);
    std::fputs (out.c_str(), stdout);
    std::fputs (err.c_str(), stderr);
    return code;
}
} // namespace flub::cli::ctl
