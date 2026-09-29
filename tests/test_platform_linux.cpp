// Tests for the Linux platform services (headless: no audio server, no
// display needed). The implementation files are compiled into this test TU
// directly so the pactl JSON parsing and argument sanitising - which live in
// an unnamed namespace - can be tested without widening the app's interface.
// On other operating systems this file compiles to nothing.
#if defined(__linux__)

#include "TestFramework.h"

#include "../app/Source/platform/PlatformServices_common.cpp"
#include "../app/Source/platform/PlatformServices_linux.cpp"

#include <spawn.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>

using namespace flub::platform;
using namespace flubtest;

namespace
{
KeyChord chord (uint32_t modifiers, uint32_t keyCode)
{
    KeyChord c;
    c.modifiers = modifiers;
    c.keyCode = keyCode;
    return c;
}

constexpr uint32_t kF1 = 0x70;

// Trimmed real-world `pactl --format=json list sinks` output (PipeWire 1.0).
const char* const kSinksJson = R"([
  {"index":48,"state":"RUNNING","name":"alsa_output.pci-0000_00_1f.3.analog-stereo","description":"Built-in Audio Analog Stereo"},
  {"index":61,"state":"IDLE","name":"flubsound_game","description":"Flubsound Game"}
])";

// Trimmed `pactl --format=json list sink-inputs` output: two streams from one
// game (one corked), a browser, a stream without pid (a loopback module) and
// one with a string sink index.
const char* const kSinkInputsJson = R"([
  {"index":101,"driver":"PipeWire","sink":61,"corked":true,
   "properties":{"application.name":"Counter-Strike 2","application.process.id":"4242","application.process.binary":"cs2"}},
  {"index":102,"driver":"PipeWire","sink":48,"corked":false,
   "properties":{"application.name":"Counter-Strike 2","application.process.id":"4242","application.process.binary":"cs2"}},
  {"index":103,"driver":"PipeWire","sink":48,"corked":false,
   "properties":{"application.name":"Firefox","pipewire.sec.pid":"777","application.process.binary":"firefox"}},
  {"index":104,"driver":"PipeWire","sink":48,"corked":false,
   "properties":{"media.name":"loopback-1-12"}},
  {"index":105,"driver":"PipeWire","sink":"61","corked":false,
   "properties":{"application.process.id":"9001","application.process.binary":"spotify"}}
])";
} // namespace

TEST_CASE ("Platform: KeyChord::toString uses Linux modifier names and F-key numbering")
{
    CHECK (chord (KeyChord::Ctrl | KeyChord::Shift, 'M').toString() == "Ctrl+Shift+M");
    CHECK (chord (KeyChord::Super | KeyChord::Alt, kF1 + 12).toString() == "Alt+Super+F13");
    CHECK (chord (KeyChord::None, kF1).toString() == "F1");
    CHECK (chord (KeyChord::None, kF1 + 23).toString() == "F24");
    CHECK (chord (KeyChord::Ctrl, '7').toString() == "Ctrl+7");
    CHECK (chord (KeyChord::None, 0).toString().empty());
    CHECK (chord (KeyChord::Ctrl | KeyChord::Alt, 0x26).toString() == "Ctrl+Alt+Up");
    CHECK (chord (KeyChord::Ctrl, 0x20).toString() == "Ctrl+Space");
    CHECK (chord (KeyChord::Ctrl, 0x21).toString() == "Ctrl+PageUp");
    CHECK (chord (KeyChord::Ctrl, 0x2E).toString() == "Ctrl+Delete");
    CHECK (chord (KeyChord::Ctrl, 0x2A).toString() == "Ctrl+Key 0x2A");
}

TEST_CASE ("Platform: chord validation rejects shortcuts that would swallow typing")
{
    CHECK (! detail::isValidChord (chord (KeyChord::None, 'A')));
    CHECK (! detail::isValidChord (chord (KeyChord::Shift, 'A')));
    CHECK (detail::isValidChord (chord (KeyChord::Ctrl, 'A')));
    CHECK (detail::isValidChord (chord (KeyChord::Alt | KeyChord::Shift, '9')));
    CHECK (detail::isValidChord (chord (KeyChord::None, kF1 + 12)));
    CHECK (! detail::isValidChord (chord (KeyChord::Ctrl, 0x2A)));
    CHECK (! detail::isValidChord (chord (KeyChord::Ctrl, 0x29)));
    // The app's default Boost / preset chords (Ctrl+Alt+arrows) and the other
    // navigation keys are valid with a modifier, never bare or Shift-only.
    for (const uint32_t nav : { 0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u, 0x27u, 0x28u, 0x2Du, 0x2Eu })
    {
        CHECK (detail::isValidChord (chord (KeyChord::Ctrl | KeyChord::Alt, nav)));
        CHECK (! detail::isValidChord (chord (KeyChord::None, nav)));
        CHECK (! detail::isValidChord (chord (KeyChord::Shift, nav)));
#if FLUB_HAVE_X11_HEADERS
        CHECK (keysymForChord (nav) != 0);
#endif
    }
    CHECK (! detail::isValidChord (chord (KeyChord::Ctrl, kF1 + 24)));
    CHECK (! detail::isValidChord (chord (1u << 7, 'A')));

    std::string reason;
    CHECK (! detail::isValidChord (chord (KeyChord::None, 'Q'), &reason));
    CHECK (! reason.empty());
}

TEST_CASE ("Platform: pactl argument whitelist blocks shell and option injection")
{
    CHECK (pactl::isSafeToken ("flubsound_game"));
    CHECK (pactl::isSafeToken ("alsa_output.pci-0000_00_1f.3.analog-stereo"));
    CHECK (pactl::isSafeToken ("bluez_output.AA_BB_CC_DD_EE_FF.1"));
    CHECK (pactl::isSafeToken ("@DEFAULT_SINK@"));
    CHECK (pactl::isSafeToken ("42"));

    CHECK (! pactl::isSafeToken (""));
    CHECK (! pactl::isSafeToken ("-h"));
    CHECK (! pactl::isSafeToken ("--server=evil"));
    CHECK (! pactl::isSafeToken ("a b"));
    CHECK (! pactl::isSafeToken ("x;rm -rf ~"));
    CHECK (! pactl::isSafeToken ("x'y"));
    CHECK (! pactl::isSafeToken ("$(id)"));
    CHECK (! pactl::isSafeToken ("`id`"));
    CHECK (! pactl::isSafeToken ("a\nb"));
    CHECK (! pactl::isSafeToken ("a|b"));
    CHECK (! pactl::isSafeToken (std::string (256, 'a')));
    CHECK (pactl::shellQuote ("flubsound_game") == "'flubsound_game'");
}

TEST_CASE ("Platform: pactl JSON is turned into one session per process")
{
    std::string error;
    std::map<uint32_t, std::string> sinkNames;
    REQUIRE (pactl::parseSinks (kSinksJson, sinkNames, error));
    CHECK (sinkNames.size() == 2);
    CHECK (sinkNames[61] == "flubsound_game");

    std::vector<pactl::SinkInput> inputs;
    REQUIRE (pactl::parseSinkInputs (kSinkInputsJson, inputs, error));
    REQUIRE (inputs.size() == 5);
    CHECK (inputs[0].index == 101 && inputs[0].processId == 4242 && inputs[0].corked);
    CHECK (inputs[2].processId == 777); // pipewire.sec.pid fallback
    CHECK (inputs[3].processId == 0);
    CHECK (inputs[4].sinkIndex == 61); // sink index given as a string

    // Pretend we are the Spotify process: our own streams must be hidden.
    const auto sessions = pactl::toSessions (inputs, sinkNames, 9001);
    REQUIRE (sessions.size() == 2);

    CHECK (sessions[0].processId == 4242);
    CHECK (sessions[0].executableName == "cs2");
    CHECK (sessions[0].displayName == "Counter-Strike 2");
    CHECK (sessions[0].isActive);
    // The playing stream (on the hardware sink) wins over the corked one.
    CHECK (sessions[0].currentEndpointId == "alsa_output.pci-0000_00_1f.3.analog-stereo");

    CHECK (sessions[1].processId == 777);
    CHECK (sessions[1].displayName == "Firefox");

    // Unknown sink index falls back to the numeric id.
    const auto withoutNames = pactl::toSessions (inputs, {}, 1);
    REQUIRE (withoutNames.size() == 3);
    CHECK (withoutNames[2].currentEndpointId == "61");
    CHECK (withoutNames[2].displayName == "spotify"); // no application.name -> binary
}

TEST_CASE ("Platform: PipeWire's host pid wins over a sandbox-local application.process.id")
{
    // A Flatpak app reports its pid inside the sandbox's pid namespace (here 2);
    // pipewire.sec.pid is the host pid from the socket credentials.
    const char* const json =
        R"([{"index":201,"sink":48,"corked":false,"properties":{"application.process.id":"2","pipewire.sec.pid":"31337"}},
            {"index":202,"sink":48,"corked":false,"properties":{"application.process.id":"2","pipewire.sec.pid":"31338"}},
            {"index":203,"sink":48,"corked":false,"properties":{"application.process.id":"555","pipewire.sec.pid":"0"}}])";

    std::string error;
    std::vector<pactl::SinkInput> inputs;
    REQUIRE (pactl::parseSinkInputs (json, inputs, error));
    REQUIRE (inputs.size() == 3);
    CHECK (inputs[0].processId == 31337);
    CHECK (inputs[1].processId == 31338);
    CHECK (inputs[2].processId == 555); // a zero credential pid falls back

    // Two sandboxed apps must not be merged into one "pid 2" session.
    CHECK (pactl::toSessions (inputs, {}, 1).size() == 3);
}

TEST_CASE ("Platform: malformed pactl output is reported, not crashed on")
{
    std::string error;
    std::vector<pactl::SinkInput> inputs;
    CHECK (! pactl::parseSinkInputs ("pactl: unrecognized option '--format=json'", inputs, error));
    CHECK (! error.empty());

    error.clear();
    CHECK (! pactl::parseSinkInputs (R"({"index":1})", inputs, error));
    CHECK (! error.empty());

    // Entries with junk ids are skipped, valid ones kept.
    inputs.clear();
    const char* const junk =
        R"([{"index":-1},{"index":"x"},{"index":1.5},{"index":7,"properties":{"application.process.id":"12abc"}}])";
    CHECK (pactl::parseSinkInputs (junk, inputs, error));
    REQUIRE (inputs.size() == 1);
    CHECK (inputs[0].index == 7);
    CHECK (inputs[0].processId == 0);
}

TEST_CASE ("Platform: unsupported Linux services report themselves as such")
{
    // Global hotkeys need an X11 display, or in a Wayland session the
    // GlobalShortcuts portal (tested against a mock portal below); headless
    // X11-less runs must report them unsupported instead of failing.
    auto hotkeys = GlobalHotkeys::create();
    REQUIRE (hotkeys != nullptr);
#if FLUB_HAVE_X11_HEADERS
    const bool expectUnsupported = ! isWaylandSession() && std::getenv ("DISPLAY") == nullptr;
#else
    const bool expectUnsupported = ! isWaylandSession(); // built without the X11 headers
#endif
    if (expectUnsupported)
    {
        CHECK (! hotkeys->isSupported());
        CHECK (! hotkeys->registerHotkey (1, chord (KeyChord::Ctrl, 'G'), [] {}));
    }
    hotkeys->unregisterAll();

    auto capture = ProcessLoopbackCapture::create();
    REQUIRE (capture != nullptr);
    CHECK (! capture->isSupported());
    std::string error;
    CHECK (! capture->start (1, true, 48000.0, 2, [] (const float*, int, int) {}, error));
    CHECK (! error.empty());
    CHECK (! capture->isRunning());

    auto router = AppAudioRouter::create();
    REQUIRE (router != nullptr);
    error.clear();
    CHECK (! router->setAppEndpoint (0, "flubsound_game", error));
    CHECK (! error.empty());
    error.clear();
    CHECK (! router->setAppEndpoint (static_cast<uint32_t> (::getpid()), "flubsound_game", error));
    CHECK (! error.empty());
    error.clear();
    CHECK (! router->setAppEndpoint (4242, "x; touch /tmp/pwned", error));
    CHECK (! error.empty());

    // Must not crash without a sound server; never lists our own process.
    for (const auto& s : router->enumerateSessions())
        CHECK (s.processId != static_cast<uint32_t> (::getpid()));
}

TEST_CASE ("Platform: SystemTuning promote/revert restores the thread's policy")
{
    CHECK (SystemTuning::disablePowerThrottling());
    SystemTuning::revertAudioThread (nullptr); // no-op

    bool restored = true;
    std::thread worker (
        [&restored]
        {
            int before = -1, after = -1;
            sched_param p {};
            pthread_getschedparam (pthread_self(), &before, &p);

            // Succeeds only with RT rights (root/CAP_SYS_NICE/rtprio limit).
            if (void* handle = SystemTuning::promoteAudioThread())
            {
                int during = -1;
                pthread_getschedparam (pthread_self(), &during, &p);
                restored = (during & ~SCHED_RESET_ON_FORK) == SCHED_FIFO;
                SystemTuning::revertAudioThread (handle);
            }

            pthread_getschedparam (pthread_self(), &after, &p);
            restored = restored && (after == before);
        });
    worker.join();
    CHECK (restored);
}

// ---------------------------------------------------------------------------
// Shared helpers (environment, temporary directories, bounded waits)
// ---------------------------------------------------------------------------
namespace
{
/** Sets (or, with nullptr, unsets) an environment variable for one scope and
    restores the previous value afterwards, also when a REQUIRE throws. */
class ScopedEnv
{
public:
    ScopedEnv (const char* nameIn, const char* value)
        : name (nameIn)
    {
        if (const char* old = std::getenv (name))
            previous = std::string (old);
        if (value != nullptr)
            ::setenv (name, value, 1);
        else
            ::unsetenv (name);
    }

    ~ScopedEnv()
    {
        if (previous)
            ::setenv (name, previous->c_str(), 1);
        else
            ::unsetenv (name);
    }

    ScopedEnv (const ScopedEnv&) = delete;
    ScopedEnv& operator= (const ScopedEnv&) = delete;

private:
    const char* name;
    std::optional<std::string> previous;
};

/** A fresh directory under $TMPDIR (or /tmp), removed with its contents.
    One that will hold Unix sockets ('holdsSockets') is made in /tmp instead
    when $TMPDIR is so long that a socket path in it would not fit
    sockaddr_un::sun_path (108 bytes): bind() would fail, and dbus-daemon
    would not start (which the D-Bus tests report as a failure). */
struct TempDir
{
    explicit TempDir (bool holdsSockets = false)
    {
        // The longest socket name used below it: dbus-daemon's
        // "/dbus-XXXXXXXXXX" or the triggers test's "/run dir/bus".
        constexpr size_t kSocketNameRoom = 32;
        const char* base = std::getenv ("TMPDIR");
        std::string root = base != nullptr && base[0] == '/' ? base : "/tmp";
        const std::string name = holdsSockets ? "/flub-XXXXXX" : "/flub-autostart-XXXXXX";
        if (holdsSockets && root.size() + name.size() + kSocketNameRoom >= sizeof (sockaddr_un::sun_path))
            root = "/tmp";
        std::string pattern = root + name;
        if (::mkdtemp (pattern.data()) != nullptr)
            path = pattern;
    }
    ~TempDir()
    {
        std::error_code ignored;
        if (! path.empty())
            std::filesystem::remove_all (path, ignored);
    }
    TempDir (const TempDir&) = delete;
    TempDir& operator= (const TempDir&) = delete;

    std::string path;
};

/** Bound for every wait on the bus, the daemon, the X server or a service
    thread: a hang guard only, never a timing assertion. */
constexpr auto kHangGuard = std::chrono::seconds (10);

template <typename Predicate>
bool waitUntil (Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + kHangGuard;
    while (! predicate())
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    return true;
}

std::string findInPath (const char* name)
{
    const char* path = std::getenv ("PATH");
    std::istringstream dirs (path != nullptr ? path : "/usr/local/bin:/usr/bin:/bin");
    for (std::string dir; std::getline (dirs, dir, ':');)
        if (! dir.empty() && ::access ((dir + "/" + name).c_str(), X_OK) == 0)
            return dir + "/" + name;
    return {};
}

} // namespace

#if FLUB_HAVE_X11_HEADERS
TEST_CASE ("Platform: X11 global hotkeys fire once per press, refuse a chord another client holds, and release on unregister")
{
    // Needs an X server (CI runs the platform tests under Xvfb too) and
    // libXtst to synthesise key events; skipped otherwise. Not a Wayland
    // session, or the factory would pick the desktop's real portal.
    ScopedEnv noWayland ("WAYLAND_DISPLAY", nullptr);
    ScopedEnv session ("XDG_SESSION_TYPE", nullptr);
    auto hotkeys = GlobalHotkeys::create();
    if (! hotkeys->isSupported())
    {
        std::cerr << "    (no X11 display: skipped)\n";
        return;
    }
    void* xtst = ::dlopen ("libXtst.so.6", RTLD_NOW | RTLD_LOCAL);
    if (xtst == nullptr)
    {
        std::cerr << "    (libXtst not available: skipped)\n";
        return;
    }
    using FakeKey = int (*) (Display*, unsigned int, Bool, unsigned long);
    const auto fakeKey = reinterpret_cast<FakeKey> (::dlsym (xtst, "XTestFakeKeyEvent"));
    REQUIRE (fakeKey != nullptr);
    const X11Api* x = X11Api::get();
    REQUIRE (x != nullptr);
    Display* d = x->openDisplay (nullptr);
    REQUIRE (d != nullptr);

    std::atomic<int> fired { 0 };
    const auto chordG = chord (KeyChord::Ctrl | KeyChord::Alt, 'G');
    std::vector<GlobalHotkeys::BindingResult> results; // reported synchronously, on this thread
    hotkeys->setBindingListener ([&results] (const GlobalHotkeys::BindingResult& r) { results.push_back (r); });
    REQUIRE (hotkeys->registerHotkey (7, chordG, "Boost +10%", [&fired] { ++fired; }));
    CHECK (results == (std::vector<GlobalHotkeys::BindingResult> { { 7, GlobalHotkeys::BindingResult::Status::Registered, {} } }));

    const auto key = [&] (KeySym sym, bool down) {
        fakeKey (d, x->keysymToKeycode (d, sym), down ? True : False, 0);
        x->flush (d);
    };
    const auto waitFor = [&fired] (int count) {
        waitUntil ([&fired, count] { return fired.load() >= count; });
        return fired.load();
    };
    const auto press = [&] (int repeats) {
        key (XK_Control_L, true);
        key (XK_Alt_L, true);
        for (int i = 0; i < repeats; ++i)
            key (XK_g, true); // key repeat: presses without a release
        key (XK_g, false);
        key (XK_Alt_L, false);
        key (XK_Control_L, false);
    };

    press (1);
    CHECK (waitFor (1) == 1);
    press (5); // held key: one action, not five
    CHECK (waitFor (2) == 2);
    std::this_thread::sleep_for (std::chrono::milliseconds (100));
    CHECK (fired.load() == 2);

    // Other modifiers do not fire.
    key (XK_Control_L, true);
    key (XK_g, true);
    key (XK_g, false);
    key (XK_Control_L, false);
    std::this_thread::sleep_for (std::chrono::milliseconds (100));
    CHECK (fired.load() == 2);

    // Modifiers let go before the key (a common way to release a chord):
    // that release still ends the press, so the next press fires again.
    key (XK_Control_L, true);
    key (XK_Alt_L, true);
    key (XK_g, true);
    key (XK_Alt_L, false);
    key (XK_Control_L, false);
    key (XK_g, false);
    CHECK (waitFor (3) == 3);
    press (1);
    CHECK (waitFor (4) == 4);

    // The same chord for a second action is refused (one press would run
    // both; Windows refuses it too); the first keeps working, and the same
    // id may take its own chord again.
    results.clear();
    CHECK (! hotkeys->registerHotkey (11, chordG, [] {}));
    CHECK (results == (std::vector<GlobalHotkeys::BindingResult> { { 11, GlobalHotkeys::BindingResult::Status::Unavailable, {} } }));
    REQUIRE (hotkeys->registerHotkey (7, chordG, "Boost +10%", [&fired] { ++fired; }));
    press (1);
    CHECK (waitFor (5) == 5);

    // Another client (a second service) cannot take the same chord ...
    auto other = GlobalHotkeys::create();
    REQUIRE (other->isSupported());
    std::vector<GlobalHotkeys::BindingResult> otherResults;
    other->setBindingListener ([&otherResults] (const GlobalHotkeys::BindingResult& r) { otherResults.push_back (r); });
    CHECK (! other->registerHotkey (1, chordG, [] {}));
    // ... until it is released here.
    hotkeys->unregisterHotkey (7);
    CHECK (other->registerHotkey (1, chordG, [] {}));
    other->unregisterAll();
    CHECK (otherResults == (std::vector<GlobalHotkeys::BindingResult> { { 1, GlobalHotkeys::BindingResult::Status::Unavailable, {} },
                                                                        { 1, GlobalHotkeys::BindingResult::Status::Registered, {} } }));

    // Navigation keys work too (the app's defaults are Ctrl+Alt+arrows).
    std::atomic<int> arrow { 0 };
    REQUIRE (hotkeys->registerHotkey (10, chord (KeyChord::Ctrl | KeyChord::Alt, 0x26), [&arrow] { ++arrow; }));
    key (XK_Control_L, true);
    key (XK_Alt_L, true);
    key (XK_Up, true);
    key (XK_Up, false);
    key (XK_Alt_L, false);
    key (XK_Control_L, false);
    CHECK (waitUntil ([&arrow] { return arrow.load() >= 1; }));
    CHECK (arrow.load() == 1);
    hotkeys->unregisterHotkey (10);

    // Bare keys and unmappable key codes are refused.
    results.clear();
    CHECK (! hotkeys->registerHotkey (8, chord (KeyChord::None, 'G'), [] {}));
    CHECK (! hotkeys->registerHotkey (9, chord (KeyChord::Ctrl, 0x13), [] {}));
    CHECK (! hotkeys->registerHotkey (9, chord (KeyChord::Shift, 'G'), [] {}));
    CHECK (results.size() == 3);
    for (const auto& r : results)
        CHECK (r.status == GlobalHotkeys::BindingResult::Status::Unavailable);

    x->closeDisplay (d);
    ::dlclose (xtst);
}
#endif

// ---------------------------------------------------------------------------
// Start with the OS: XDG autostart entry
// ---------------------------------------------------------------------------
namespace
{
std::string readText (const std::string& path)
{
    std::ifstream in (path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void writeText (const std::string& path, const std::string& text)
{
    std::ofstream out (path, std::ios::binary | std::ios::trunc);
    out << text;
}

/** Reference decoder for an Exec value, written from the Desktop Entry spec
    independently of the implementation: string-value unescaping (\s \n \t
    \r \; anything else is invalid), then field codes (only %% is expected
    here), then the quoting rules (inside "...", a backslash may only precede
    " ` $ or \). Returns the argument vector; ok = false on any violation. */
std::vector<std::string> decodeExec (const std::string& value, bool& ok)
{
    ok = true;
    const auto fail = [&ok]
    {
        ok = false;
        return std::vector<std::string>();
    };

    std::string unescaped;
    for (size_t i = 0; i < value.size(); ++i)
    {
        if (value[i] != '\\')
        {
            unescaped += value[i];
            continue;
        }
        if (++i >= value.size())
            return fail();
        switch (value[i])
        {
            case 's': unescaped += ' '; break;
            case 'n': unescaped += '\n'; break;
            case 't': unescaped += '\t'; break;
            case 'r': unescaped += '\r'; break;
            case '\\': unescaped += '\\'; break;
            default: return fail();
        }
    }

    std::string expanded;
    for (size_t i = 0; i < unescaped.size(); ++i)
    {
        if (unescaped[i] != '%')
        {
            expanded += unescaped[i];
            continue;
        }
        if (i + 1 >= unescaped.size() || unescaped[i + 1] != '%')
            return fail(); // a field code we never write
        expanded += '%';
        ++i;
    }

    const std::string reserved = " \t\n\"'\\><~|&;$*?#()`";
    std::vector<std::string> args;
    size_t i = 0;
    while (i < expanded.size())
    {
        if (expanded[i] == ' ')
        {
            ++i;
            continue;
        }
        std::string arg;
        if (expanded[i] == '"')
        {
            ++i;
            bool closed = false;
            while (i < expanded.size())
            {
                const char c = expanded[i++];
                if (c == '"')
                {
                    closed = true;
                    break;
                }
                if (c == '\\')
                {
                    if (i >= expanded.size() || std::string ("\"`$\\").find (expanded[i]) == std::string::npos)
                        return fail();
                    arg += expanded[i++];
                    continue;
                }
                if (c == '`' || c == '$')
                    return fail(); // must have been escaped
                arg += c;
            }
            if (! closed)
                return fail();
        }
        else
        {
            while (i < expanded.size() && expanded[i] != ' ')
            {
                if (reserved.find (expanded[i]) != std::string::npos)
                    return fail(); // needs quoting
                arg += expanded[i++];
            }
        }
        args.push_back (arg);
    }
    return args;
}

std::vector<std::string> decodeExecOf (const std::string& program)
{
    std::string exec, error;
    if (! autostart::execValueFor (program, exec, error))
        return {};
    bool ok = false;
    auto args = decodeExec (exec, ok);
    return ok ? args : std::vector<std::string>();
}
} // namespace

TEST_CASE ("Platform: XDG autostart Exec quoting follows the Desktop Entry spec for spaces, quotes and shell characters")
{
    std::string exec, error;

    // The spec's own examples: a quote in a quoted argument is \\" in the
    // file, a dollar \\$, a backslash \\\\.
    REQUIRE (autostart::execValueFor ("/opt/My \"Apps\"/Flub$ound\\x", exec, error));
    CHECK (exec == R"("/opt/My \\"Apps\\"/Flub\\$ound\\\\x")");
    REQUIRE (autostart::execValueFor ("/usr/bin/flubsound-pro", exec, error));
    CHECK (exec == "\"/usr/bin/flubsound-pro\"");
    REQUIRE (autostart::execValueFor ("/opt/100%/run", exec, error));
    CHECK (exec == "\"/opt/100%%/run\"");

    // Every path round-trips through an independent decoder as ONE argument.
    const std::string paths[] = {
        "/usr/bin/flubsound-pro",
        "/home/alex/Flubsound Pro/FlubsoundPro",
        "/home/o'neil/\"quoted\" dir/`tick`/$HOME/back\\slash/50%/FlubsoundPro",
        "/home/j\xC3\xBCrgen/Musik & Spiele/(x86)/a;b|c<d>e*f?g#h~i/Flubsound Pro",
        "/tmp/ends with backslash\\",
    };
    for (const auto& path : paths)
    {
        const auto args = decodeExecOf (path);
        REQUIRE (args.size() == 1);
        CHECK (args[0] == path);
    }

    // What an Exec line cannot carry is refused with a message.
    for (const auto& bad : { std::string(), std::string ("FlubsoundPro"), std::string ("relative/dir/FlubsoundPro"),
                           std::string ("/opt/a\nb"), std::string ("/opt/a\tb"), std::string ("/opt/bad\xFF/utf8"),
                           std::string ("/opt/overlong\xC0\xAF"), std::string ("/opt/surrogate\xED\xA0\x80"),
                           std::string ("/opt/truncated\xE2\x82") })
    {
        error.clear();
        CHECK (! autostart::execValueFor (bad, exec, error));
        CHECK (! error.empty());
    }

    // The group reader ignores comments, other groups and CRLF line ends.
    const auto keys = autostart::parseDesktopEntryGroup ("# c\r\n[Desktop Entry]\r\nName = Flubsound Pro\r\nHidden=false\r\n"
                                                         "[Desktop Action x]\nHidden=true\n");
    CHECK (keys.size() == 2);
    CHECK (keys.count ("Name") == 1 && keys.at ("Name") == "Flubsound Pro");
    CHECK (keys.count ("Hidden") == 1 && keys.at ("Hidden") == "false");
}

TEST_CASE ("Platform: XDG autostart entry is written atomically under $XDG_CONFIG_HOME, read back, switched off and removed")
{
    TempDir temp;
    REQUIRE (! temp.path.empty());
    const std::string config = temp.path + "/con fig"; // does not exist yet: created with the autostart folder
    ScopedEnv xdg ("XDG_CONFIG_HOME", config.c_str());
    ScopedEnv appImage ("APPIMAGE", nullptr);

    const std::string file = config + "/autostart/flubsound-pro.desktop";
    CHECK (autostart::entryPath() == file);

    auto autoStart = AutoStart::create();
    REQUIRE (autoStart != nullptr);
    CHECK (autoStart->isSupported());
    CHECK (! autoStart->isEnabled());

    const std::string program = temp.path + "/Flubsound \"Pro\" $1/FlubsoundPro";
    std::string error;
    REQUIRE (autoStart->setEnabled (true, program, error));
    CHECK (error.empty());
    CHECK (autoStart->isEnabled());

    // A parseable entry with the required keys; Exec decodes to the program.
    const auto text = readText (file);
    CHECK (text.rfind ("[Desktop Entry]\n", 0) == 0);
    const auto keys = autostart::parseDesktopEntryGroup (text);
    CHECK (keys.count ("Type") == 1 && keys.at ("Type") == "Application");
    CHECK (keys.count ("Name") == 1 && keys.at ("Name") == "Flubsound Pro");
    CHECK (keys.count ("X-GNOME-Autostart-enabled") == 1 && keys.at ("X-GNOME-Autostart-enabled") == "true");
    REQUIRE (keys.count ("Exec") == 1);
    bool ok = false;
    const auto args = decodeExec (keys.at ("Exec"), ok);
    CHECK (ok);
    REQUIRE (args.size() == 1); // the app has no start-to-tray switch: no arguments
    CHECK (args[0] == program);

    // Nothing but the entry is left in the folder (the temp file was renamed).
    size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator (config + "/autostart"))
    {
        ++files;
        CHECK (entry.path().filename() == "flubsound-pro.desktop");
    }
    CHECK (files == 1);

    // Enabling again is harmless; an empty path means the running executable.
    REQUIRE (autoStart->setEnabled (true, {}, error));
    const auto selfArgs = decodeExec (autostart::parseDesktopEntryGroup (readText (file))["Exec"], ok);
    CHECK (ok);
    REQUIRE (selfArgs.size() == 1);
    CHECK (std::filesystem::equivalent (selfArgs[0], "/proc/self/exe"));

    // Switched off by the desktop's own start-up settings: reads as disabled,
    // and enabling from Flubsound switches it back on.
    writeText (file, "[Desktop Entry]\nType=Application\nName=Flubsound Pro\nExec=/x\nX-GNOME-Autostart-enabled=false\n");
    CHECK (! autoStart->isEnabled());
    writeText (file, "[Desktop Entry]\nType=Application\nName=Flubsound Pro\nExec=/x\nHidden=true\n");
    CHECK (! autoStart->isEnabled());
    writeText (file, "not a desktop entry\n");
    CHECK (! autoStart->isEnabled());
    REQUIRE (autoStart->setEnabled (true, program, error));
    CHECK (autoStart->isEnabled());

    // Disable removes the file; disabling twice is fine.
    REQUIRE (autoStart->setEnabled (false, program, error));
    CHECK (! std::filesystem::exists (file));
    CHECK (! autoStart->isEnabled());
    CHECK (autoStart->setEnabled (false, program, error));

    // Failures are reported, not swallowed: the config "folder" is a file.
    const std::string blocker = temp.path + "/blocker";
    writeText (blocker, "x");
    ScopedEnv blocked ("XDG_CONFIG_HOME", blocker.c_str());
    error.clear();
    CHECK (! autoStart->setEnabled (true, program, error));
    CHECK (! error.empty());
    CHECK (! autoStart->isEnabled());
    error.clear();
    CHECK (! autoStart->setEnabled (true, "relative/FlubsoundPro", error));
    CHECK (! error.empty());
}

TEST_CASE ("Platform: XDG autostart falls back to ~/.config when $XDG_CONFIG_HOME is unset, empty or relative")
{
    TempDir temp;
    REQUIRE (! temp.path.empty());
    ScopedEnv home ("HOME", (temp.path + "/").c_str());
    const std::string expected = temp.path + "/.config/autostart/flubsound-pro.desktop";

    {
        ScopedEnv xdg ("XDG_CONFIG_HOME", nullptr);
        CHECK (autostart::entryPath() == expected);
    }
    {
        ScopedEnv xdg ("XDG_CONFIG_HOME", "");
        CHECK (autostart::entryPath() == expected);
    }
    {
        ScopedEnv xdg ("XDG_CONFIG_HOME", "relative/config");
        CHECK (autostart::entryPath() == expected);

        auto autoStart = AutoStart::create();
        REQUIRE (autoStart != nullptr);
        std::string error;
        REQUIRE (autoStart->setEnabled (true, "/usr/bin/flubsound-pro", error));
        CHECK (std::filesystem::exists (expected));
        CHECK (! std::filesystem::exists ("relative/config"));
        CHECK (autoStart->isEnabled());
        REQUIRE (autoStart->setEnabled (false, {}, error));
        CHECK (! std::filesystem::exists (expected));
    }
    {
        ScopedEnv xdg ("XDG_CONFIG_HOME", (temp.path + "/xdg//").c_str());
        CHECK (autostart::entryPath() == temp.path + "/xdg/autostart/flubsound-pro.desktop");
    }
}

// ---------------------------------------------------------------------------
// Wayland: xdg-desktop-portal GlobalShortcuts (against a mock portal on a
// private session bus)
// ---------------------------------------------------------------------------
namespace
{
/** A private dbus-daemon (session bus configuration) for one test: started
    with --nofork, its address read from a pipe, SIGTERMed and reaped on
    destruction. address stays empty when dbus-daemon is missing
    (installed == false) or fails to start. */
class PrivateSessionBus
{
public:
    PrivateSessionBus()
    {
        const std::string daemon = findInPath ("dbus-daemon");
        installed = ! daemon.empty();
        if (daemon.empty() || dir.path.empty())
            return;
        const std::string config = dir.path + "/session.conf";
        writeText (config, "<busconfig>\n  <type>session</type>\n  <listen>unix:dir=" + dir.path + "</listen>\n"
                           "  <auth>EXTERNAL</auth>\n  <policy context=\"default\">\n    <allow send_destination=\"*\" eavesdrop=\"true\"/>\n"
                           "    <allow eavesdrop=\"true\"/>\n    <allow own=\"*\"/>\n  </policy>\n</busconfig>\n");

        int fds[2] = { -1, -1 };
        if (::pipe2 (fds, O_CLOEXEC) != 0)
            return;
        // Everything the child needs is prepared before fork (async-signal-safe child).
        const std::string configArg = "--config-file=" + config;
        const std::string printArg = "--print-address=" + std::to_string (fds[1]);
        const char* argv[] = { daemon.c_str(), configArg.c_str(), "--nofork", printArg.c_str(), nullptr };
        const pid_t parent = ::getpid();
        pid = ::fork();
        if (pid == 0)
        {
            // The daemon must not outlive a test binary that crashes or is killed.
            ::prctl (PR_SET_PDEATHSIG, SIGTERM);
            if (::getppid() != parent)
                ::_exit (127);
            ::fcntl (fds[1], F_SETFD, 0); // the address pipe survives exec
            ::execv (argv[0], const_cast<char* const*> (argv));
            ::_exit (127);
        }
        ::close (fds[1]);

        std::string line;
        pollfd readable { fds[0], POLLIN, 0 };
        const auto deadline = std::chrono::steady_clock::now() + kHangGuard;
        while (pid > 0 && line.find ('\n') == std::string::npos && std::chrono::steady_clock::now() < deadline)
        {
            if (::poll (&readable, 1, 100) <= 0)
                continue;
            char buffer[256];
            const ssize_t n = ::read (fds[0], buffer, sizeof (buffer));
            if (n <= 0)
                break; // daemon exited
            line.append (buffer, static_cast<size_t> (n));
        }
        ::close (fds[0]);
        if (const size_t end = line.find ('\n'); end != std::string::npos)
            address = line.substr (0, end);
    }

    ~PrivateSessionBus() { stop(); }

    /** Ends the daemon (every connection to it is lost). */
    void stop()
    {
        if (pid > 0)
        {
            ::kill (pid, SIGTERM);
            int status = 0;
            ::waitpid (pid, &status, 0);
        }
        pid = -1;
    }

    PrivateSessionBus (const PrivateSessionBus&) = delete;
    PrivateSessionBus& operator= (const PrivateSessionBus&) = delete;

    TempDir dir { true }; // holds the bus socket
    pid_t pid = -1;
    bool installed = false;
    std::string address;
};

/** A private connection to $DBUS_SESSION_BUS_ADDRESS (nullptr on failure). */
dbus::Connection* connectToSessionBus (const dbus::Api& api)
{
    dbus::Error error;
    api.errorInit (&error);
    dbus::Connection* connection = api.connectionOpenPrivate (portal::sessionBusAddress().c_str(), &error);
    if (connection != nullptr && api.busRegister (connection, &error) == 0)
    {
        api.connectionClose (connection);
        api.connectionUnref (connection);
        connection = nullptr;
    }
    api.errorFree (&error);
    return connection;
}

void disconnect (const dbus::Api& api, dbus::Connection* connection)
{
    if (connection == nullptr)
        return;
    api.connectionClose (connection);
    api.connectionUnref (connection);
}

/** libdbus calls only the mock portal needs (the service never sends
    replies or signals). */
struct MockDBusApi
{
    int (*requestName) (dbus::Connection*, const char*, unsigned int, dbus::Error*) = nullptr;
    dbus::Message* (*newMethodReturn) (dbus::Message*) = nullptr;
    dbus::Message* (*newSignal) (const char*, const char*, const char*) = nullptr;
    dbus::Message* (*newError) (dbus::Message*, const char*, const char*) = nullptr;
    dbus::Boolean (*setDestination) (dbus::Message*, const char*) = nullptr;

    explicit MockDBusApi (const dbus::Api& api)
    {
        const auto sym = [&api] (auto& fn, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype (fn)>> (::dlsym (api.lib, name)); return fn != nullptr; };
        ok = sym (requestName, "dbus_bus_request_name") && sym (newMethodReturn, "dbus_message_new_method_return")
             && sym (newSignal, "dbus_message_new_signal") && sym (newError, "dbus_message_new_error")
             && sym (setDestination, "dbus_message_set_destination");
    }
    bool ok = false;
};

/** In-process stand-in for xdg-desktop-portal: owns
    org.freedesktop.portal.Desktop on the private bus and implements
    GlobalShortcuts (CreateSession, BindShortcuts, the version property),
    Session.Close and the Request::Response signals like the real portal
    (unicast to the caller, predictable request paths). Served by its own
    thread, which is the only one that touches its connection: libdbus lets
    a thread blocked in dbus_connection_flush starve while another thread
    keeps taking the connection's I/O path, so emit() queues the signal for
    the mock's thread instead of sending it itself. The test reads what the
    mock recorded under its mutex. */
class MockPortal
{
public:
    struct Shortcut
    {
        std::string id, description, preferredTrigger;
        bool operator== (const Shortcut&) const = default;
    };
    struct Bind
    {
        std::string session;
        std::vector<Shortcut> shortcuts;
    };

    explicit MockPortal (bool withGlobalShortcuts) : globalShortcuts (withGlobalShortcuts)
    {
        if (api == nullptr || ! extra.ok || (connection = connectToSessionBus (*api)) == nullptr)
            return;
        dbus::Error error;
        api->errorInit (&error);
        const int reply = extra.requestName (connection, portal::kService, 4 /* DO_NOT_QUEUE */, &error);
        api->errorFree (&error);
        if (reply != 1 /* PRIMARY_OWNER */)
            return;
        uniqueName = dbus::str (api->busGetUniqueName (connection));
        if (api->connectionGetUnixFd (connection, &busFd) == 0 || ::pipe2 (wakePipe, O_CLOEXEC | O_NONBLOCK) != 0)
            return;
        running = true;
        thread = std::thread ([this] { serve(); });
    }

    ~MockPortal()
    {
        running = false;
        if (thread.joinable())
        {
            wake();
            thread.join();
        }
        disconnect (*api, connection);
        for (const int fd : wakePipe)
            if (fd >= 0)
                ::close (fd);
    }

    bool ok() const { return thread.joinable(); }

    /** Sends GlobalShortcuts.Activated / Deactivated to the client, as the
        portal does when the user presses / releases a bound shortcut. */
    void emit (const std::string& member, const std::string& session, const std::string& shortcutId)
    {
        {
            std::lock_guard<std::mutex> guard (mutex);
            jobs.push_back ([this, member, session, shortcutId, client = clientName]
                            { sendShortcutSignal (*api, extra, connection, client, member.c_str(), session, shortcutId); });
        }
        wake();
    }

    static void sendShortcutSignal (const dbus::Api& dbusApi, const MockDBusApi& mockApi, dbus::Connection* from, const std::string& to,
                                    const char* member, const std::string& session, const std::string& shortcutId)
    {
        dbus::MessageRef signal (mockApi.newSignal (portal::kObjectPath, portal::kShortcutsInterface, member));
        dbus::Iter args, options;
        const uint64_t timestamp = 0;
        mockApi.setDestination (signal.get(), to.c_str());
        dbusApi.iterInitAppend (signal.get(), &args);
        dbus::appendBasic (dbusApi, &args, dbus::kTypeObjectPath, session);
        dbus::appendBasic (dbusApi, &args, dbus::kTypeString, shortcutId);
        dbusApi.iterAppendBasic (&args, 't', &timestamp);
        dbusApi.iterOpenContainer (&args, dbus::kTypeArray, "{sv}", &options);
        dbusApi.iterCloseContainer (&args, &options);
        dbusApi.connectionSend (from, signal.get(), nullptr);
        dbusApi.connectionFlush (from);
    }

    /** Shortcut ids BindShortcuts leaves out of its result (not bound). */
    void refuse (const std::string& shortcutId)
    {
        std::lock_guard<std::mutex> guard (mutex);
        refused.insert (shortcutId);
    }

    /** The trigger_description BindShortcuts returns for a shortcut (by
        default its preferred_trigger), as when the user picks another key or
        the desktop words the same key its own way. */
    void describeTrigger (const std::string& shortcutId, const std::string& triggerDescription)
    {
        std::lock_guard<std::mutex> guard (mutex);
        triggerDescriptions[shortcutId] = triggerDescription;
    }

    /** BindShortcuts answers without trigger_description for a shortcut, as
        GNOME does when the user removed its key. */
    void omitTrigger (const std::string& shortcutId)
    {
        std::lock_guard<std::mutex> guard (mutex);
        omittedTriggers.insert (shortcutId);
    }

    /** Holds back the answers to the version probe (Properties.Get) until
        releaseVersion(), as for a portal that D-Bus is still starting. */
    void holdVersion()
    {
        std::lock_guard<std::mutex> guard (mutex);
        holdingVersion = true;
    }
    void releaseVersion() { release (holdingVersion); }

    /** Holds back BindShortcuts' Response until releaseBinds(), as while the
        desktop's dialog is open. The method reply is sent at once. */
    void holdBinds()
    {
        std::lock_guard<std::mutex> guard (mutex);
        holdingBinds = true;
    }
    void releaseBinds() { release (holdingBinds); }

    /** Version probes received so far. */
    size_t versionProbes()
    {
        std::lock_guard<std::mutex> guard (mutex);
        return probes;
    }

    /** Response code of the next BindShortcuts (1 = the user cancelled the
        dialog, 2 = other failure; no results then). */
    void setBindResponse (uint32_t code)
    {
        std::lock_guard<std::mutex> guard (mutex);
        bindResponse = code;
    }

    /** Sends Session.Closed for 'session', as a desktop ending it does. */
    void closeSession (const std::string& session)
    {
        {
            std::lock_guard<std::mutex> guard (mutex);
            jobs.push_back ([this, session, client = clientName]
                            {
                                dbus::MessageRef signal (extra.newSignal (session.c_str(), portal::kSessionInterface, "Closed"));
                                dbus::Iter args, details;
                                extra.setDestination (signal.get(), client.c_str());
                                api->iterInitAppend (signal.get(), &args);
                                api->iterOpenContainer (&args, dbus::kTypeArray, "{sv}", &details);
                                api->iterCloseContainer (&args, &details);
                                send (signal.get());
                            });
        }
        wake();
    }

    std::vector<std::string> createdSessions()
    {
        std::lock_guard<std::mutex> guard (mutex);
        return created;
    }
    std::vector<std::string> closedSessions()
    {
        std::lock_guard<std::mutex> guard (mutex);
        return closed;
    }
    std::vector<Bind> bindCalls()
    {
        std::lock_guard<std::mutex> guard (mutex);
        return binds;
    }
    std::string client()
    {
        std::lock_guard<std::mutex> guard (mutex);
        return clientName;
    }

    const dbus::Api* api = dbus::Api::get();
    MockDBusApi extra { *dbus::Api::get() };
    dbus::Connection* connection = nullptr;
    std::string uniqueName;

private:
    void wake()
    {
        const char byte = 0;
        [[maybe_unused]] const auto written = ::write (wakePipe[1], &byte, 1);
    }

    /** Stops holding (flag = false) and sends what was held, in order, from
        the mock's thread. */
    void release (bool& holding)
    {
        {
            std::lock_guard<std::mutex> guard (mutex);
            holding = false;
            jobs.push_back ([this]
                            {
                                for (auto& message : held)
                                    send (message.get());
                                held.clear();
                            });
        }
        wake();
    }

    /** Sends 'message' now, or keeps it for release() while 'holding'
        (mock thread). */
    void sendOrHold (dbus::MessageRef message, bool holding)
    {
        if (holding)
            held.push_back (std::move (message));
        else
            send (message.get());
    }

    /** Same loop shape as the service: poll the bus fd and the wake pipe,
        never block inside libdbus. */
    void serve()
    {
        pollfd fds[2] = { { busFd, POLLIN, 0 }, { wakePipe[0], POLLIN, 0 } };
        while (running && api->connectionReadWrite (connection, 0) != 0)
        {
            while (dbus::Message* message = api->connectionPopMessage (connection))
            {
                const dbus::MessageRef owner (message);
                if (api->messageGetType (message) == 1 /* method call */)
                    handleCall (message);
            }
            std::vector<std::function<void()>> queued;
            {
                std::lock_guard<std::mutex> guard (mutex);
                queued.swap (jobs);
            }
            for (auto& job : queued)
                job();
            if (api->connectionGetDispatchStatus (connection) == dbus::kDispatchDataRemains)
                continue;
            fds[0].events = static_cast<short> (POLLIN | (api->connectionHasMessagesToSend (connection) != 0 ? POLLOUT : 0));
            ::poll (fds, 2, -1);
            char buffer[16];
            while (::read (wakePipe[0], buffer, sizeof (buffer)) > 0) {}
        }
    }

    void handleCall (dbus::Message* call)
    {
        const std::string interfaceName = dbus::str (api->messageGetInterface (call));
        const std::string member = dbus::str (api->messageGetMember (call));
        const std::string sender = dbus::str (api->messageGetSender (call));
        dbus::Iter args;
        api->iterInit (call, &args);

        if (interfaceName == "org.freedesktop.DBus.Properties" && member == "Get")
        {
            std::string iface, property;
            dbus::readString (*api, &args, iface);
            api->iterNext (&args);
            dbus::readString (*api, &args, property);
            bool hold = false;
            {
                std::lock_guard<std::mutex> guard (mutex);
                ++probes;
                hold = holdingVersion;
            }
            if (! globalShortcuts || iface != portal::kShortcutsInterface || property != "version")
                return replyError (call, "org.freedesktop.DBus.Error.InvalidArgs", "No such interface or property");
            dbus::MessageRef reply (extra.newMethodReturn (call));
            dbus::Iter out, variant;
            const uint32_t version = 1;
            api->iterInitAppend (reply.get(), &out);
            api->iterOpenContainer (&out, dbus::kTypeVariant, "u", &variant);
            api->iterAppendBasic (&variant, dbus::kTypeUInt32, &version);
            api->iterCloseContainer (&out, &variant);
            return sendOrHold (std::move (reply), hold);
        }

        const std::string requestBase = std::string (portal::kObjectPath) + "/request/" + portal::busPathElement (sender) + "/";
        if (interfaceName == portal::kShortcutsInterface && member == "CreateSession" && globalShortcuts)
        {
            std::string token, sessionToken;
            dbus::readVardictString (*api, &args, "handle_token", token);
            dbus::readVardictString (*api, &args, "session_handle_token", sessionToken);
            const std::string session = std::string (portal::kObjectPath) + "/session/" + portal::busPathElement (sender) + "/" + sessionToken;
            if (token.empty() || ! portal::isValidObjectPath (session))
                return replyError (call, "org.freedesktop.DBus.Error.InvalidArgs", "tokens missing");
            {
                std::lock_guard<std::mutex> guard (mutex);
                clientName = sender;
                created.push_back (session);
            }
            replyPath (call, requestBase + token);
            // The spec types session_handle as 's'.
            return respond (sender, requestBase + token, 0, false,
                            [&] (dbus::Iter* results) { dbus::appendDictEntry (*api, results, "session_handle", dbus::kTypeString, session); });
        }

        if (interfaceName == portal::kShortcutsInterface && member == "BindShortcuts" && globalShortcuts)
        {
            Bind bind;
            dbus::readString (*api, &args, bind.session);
            api->iterNext (&args);
            dbus::Iter items;
            api->iterRecurse (&args, &items);
            for (; api->iterGetArgType (&items) == dbus::kTypeStruct; api->iterNext (&items))
            {
                dbus::Iter item;
                api->iterRecurse (&items, &item);
                Shortcut shortcut;
                dbus::readString (*api, &item, shortcut.id);
                api->iterNext (&item);
                dbus::readVardictString (*api, &item, "description", shortcut.description);
                dbus::readVardictString (*api, &item, "preferred_trigger", shortcut.preferredTrigger);
                bind.shortcuts.push_back (shortcut);
            }
            std::string parentWindow = "unset", token;
            api->iterNext (&args);
            dbus::readString (*api, &args, parentWindow);
            api->iterNext (&args);
            dbus::readVardictString (*api, &args, "handle_token", token);
            std::set<std::string> refusedNow, omittedNow;
            std::map<std::string, std::string> triggersNow;
            uint32_t code = 0;
            bool hold = false;
            {
                std::lock_guard<std::mutex> guard (mutex);
                binds.push_back (bind);
                refusedNow = refused;
                omittedNow = omittedTriggers;
                triggersNow = triggerDescriptions;
                code = bindResponse;
                hold = holdingBinds;
            }
            replyPath (call, requestBase + token);
            return respond (sender, requestBase + token, code, hold,
                            [&] (dbus::Iter* results)
                            {
                                if (code != 0)
                                    return;
                                dbus::Iter entry, variant, list;
                                api->iterOpenContainer (results, dbus::kTypeDictEntry, nullptr, &entry);
                                dbus::appendBasic (*api, &entry, dbus::kTypeString, "shortcuts");
                                api->iterOpenContainer (&entry, dbus::kTypeVariant, "a(sa{sv})", &variant);
                                api->iterOpenContainer (&variant, dbus::kTypeArray, "(sa{sv})", &list);
                                for (const auto& shortcut : bind.shortcuts)
                                {
                                    if (refusedNow.count (shortcut.id) != 0)
                                        continue;
                                    dbus::Iter item, properties;
                                    api->iterOpenContainer (&list, dbus::kTypeStruct, nullptr, &item);
                                    dbus::appendBasic (*api, &item, dbus::kTypeString, shortcut.id);
                                    api->iterOpenContainer (&item, dbus::kTypeArray, "{sv}", &properties);
                                    dbus::appendDictEntry (*api, &properties, "description", dbus::kTypeString, shortcut.description);
                                    const auto described = triggersNow.find (shortcut.id);
                                    if (omittedNow.count (shortcut.id) == 0)
                                        dbus::appendDictEntry (*api, &properties, "trigger_description", dbus::kTypeString,
                                                               described != triggersNow.end() ? described->second : shortcut.preferredTrigger);
                                    api->iterCloseContainer (&item, &properties);
                                    api->iterCloseContainer (&list, &item);
                                }
                                api->iterCloseContainer (&variant, &list);
                                api->iterCloseContainer (&entry, &variant);
                                api->iterCloseContainer (results, &entry);
                            });
        }

        if (interfaceName == portal::kSessionInterface && member == "Close")
        {
            {
                std::lock_guard<std::mutex> guard (mutex);
                closed.push_back (dbus::str (api->messageGetPath (call)));
            }
            dbus::MessageRef reply (extra.newMethodReturn (call));
            return send (reply.get());
        }

        replyError (call, "org.freedesktop.DBus.Error.UnknownMethod", "not implemented by the mock portal");
    }

    void send (dbus::Message* message)
    {
        api->connectionSend (connection, message, nullptr);
        api->connectionFlush (connection);
    }

    void replyError (dbus::Message* call, const char* name, const char* text)
    {
        dbus::MessageRef reply (extra.newError (call, name, text));
        send (reply.get());
    }

    void replyPath (dbus::Message* call, const std::string& requestPath)
    {
        dbus::MessageRef reply (extra.newMethodReturn (call));
        dbus::Iter out;
        api->iterInitAppend (reply.get(), &out);
        dbus::appendBasic (*api, &out, dbus::kTypeObjectPath, requestPath);
        send (reply.get());
    }

    /** Request::Response (code, results) to the caller only (held while
        'hold', see holdBinds()). */
    template <typename AddResults>
    void respond (const std::string& to, const std::string& requestPath, uint32_t code, bool hold, AddResults addResults)
    {
        dbus::MessageRef signal (extra.newSignal (requestPath.c_str(), portal::kRequestInterface, "Response"));
        dbus::Iter args, results;
        extra.setDestination (signal.get(), to.c_str());
        api->iterInitAppend (signal.get(), &args);
        api->iterAppendBasic (&args, dbus::kTypeUInt32, &code);
        api->iterOpenContainer (&args, dbus::kTypeArray, "{sv}", &results);
        addResults (&results);
        api->iterCloseContainer (&args, &results);
        sendOrHold (std::move (signal), hold);
    }

    const bool globalShortcuts;
    int busFd = -1;
    int wakePipe[2] = { -1, -1 };
    std::vector<std::function<void()>> jobs;
    std::atomic<bool> running { false };
    std::thread thread;
    std::mutex mutex;
    std::string clientName;
    std::vector<std::string> created, closed;
    std::vector<Bind> binds;
    std::set<std::string> refused, omittedTriggers;
    std::map<std::string, std::string> triggerDescriptions;
    uint32_t bindResponse = 0;
    bool holdingVersion = false, holdingBinds = false;
    size_t probes = 0;
    std::vector<dbus::MessageRef> held; // mock thread only
};

/** Records what a service reports to its binding listener, and on which
    thread. */
struct BindingLog
{
    using Result = GlobalHotkeys::BindingResult;

    void attach (GlobalHotkeys& hotkeys)
    {
        hotkeys.setBindingListener ([this] (const Result& result)
                                    {
                                        std::lock_guard<std::mutex> guard (mutex);
                                        results.push_back (result);
                                        threads.push_back (std::this_thread::get_id());
                                    });
    }

    /** Everything reported since the last take(). */
    std::vector<Result> take (std::vector<std::thread::id>* reportedOn = nullptr)
    {
        std::lock_guard<std::mutex> guard (mutex);
        if (reportedOn != nullptr)
            *reportedOn = std::exchange (threads, {});
        threads.clear();
        return std::exchange (results, {});
    }

    size_t size()
    {
        std::lock_guard<std::mutex> guard (mutex);
        return results.size();
    }

    std::mutex mutex;
    std::vector<Result> results;
    std::vector<std::thread::id> threads;
};

using Status = GlobalHotkeys::BindingResult::Status;

std::vector<std::string> ids (const MockPortal::Bind& bind)
{
    std::vector<std::string> result;
    for (const auto& shortcut : bind.shortcuts)
        result.push_back (shortcut.id);
    return result;
}

/** Skips (true) when libdbus-1 or dbus-daemon is missing. A dbus-daemon
    that is installed but does not start is a failure, not a skip, so the
    portal tests cannot turn green without running. */
bool skipWithoutDBus (const PrivateSessionBus& bus)
{
    if (dbus::Api::get() == nullptr)
    {
        std::cerr << "    (libdbus-1.so.3 not available: skipped)\n";
        return true;
    }
    if (! bus.installed)
    {
        std::cerr << "    (dbus-daemon not available: skipped)\n";
        return true;
    }
    if (bus.address.empty())
    {
        ::flubtest::reportFailure (__FILE__, __LINE__, "dbus-daemon is installed but the private session bus did not start");
        return true;
    }
    return false;
}
} // namespace

TEST_CASE ("Platform: portal shortcut triggers use the XDG shortcuts format and object paths are validated")
{
    CHECK (portal::triggerFor (chord (KeyChord::Ctrl | KeyChord::Alt, 0x26)) == "CTRL+ALT+Up");
    CHECK (portal::triggerFor (chord (KeyChord::Ctrl | KeyChord::Alt, 0x28)) == "CTRL+ALT+Down");
    CHECK (portal::triggerFor (chord (KeyChord::Ctrl | KeyChord::Shift, 'M')) == "CTRL+SHIFT+m");
    CHECK (portal::triggerFor (chord (KeyChord::Super | KeyChord::Alt, kF1 + 12)) == "ALT+LOGO+F13");
    CHECK (portal::triggerFor (chord (KeyChord::None, kF1)) == "F1");
    CHECK (portal::triggerFor (chord (KeyChord::Ctrl, '7')) == "CTRL+7");
    CHECK (portal::triggerFor (chord (KeyChord::Ctrl, 0x20)) == "CTRL+space");
    CHECK (portal::triggerFor (chord (KeyChord::Ctrl, 0x21)) == "CTRL+Page_Up");
    CHECK (portal::triggerFor (chord (KeyChord::Ctrl, 0x22)) == "CTRL+Page_Down");
    CHECK (portal::triggerFor (chord (KeyChord::Ctrl, 0x2E)) == "CTRL+Delete");
    CHECK (portal::triggerFor (chord (KeyChord::Ctrl, 0x2A)).empty());
    // Every chord the settings accept has a trigger.
    for (const uint32_t nav : { 0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u, 0x27u, 0x28u, 0x2Du, 0x2Eu })
        CHECK (! portal::triggerFor (chord (KeyChord::Ctrl, nav)).empty());

    CHECK (portal::shortcutId (3) == "flubsound-3");
    CHECK (portal::busPathElement (":1.42") == "1_42");
    CHECK (portal::isValidObjectPath ("/org/freedesktop/portal/desktop/session/1_42/flubsound2"));
    CHECK (portal::isValidObjectPath ("/"));
    CHECK (! portal::isValidObjectPath (""));
    CHECK (! portal::isValidObjectPath ("relative/path"));
    CHECK (! portal::isValidObjectPath ("/double//slash"));
    CHECK (! portal::isValidObjectPath ("/trailing/"));
    CHECK (! portal::isValidObjectPath ("/with-dash"));

    // Without DBUS_SESSION_BUS_ADDRESS the systemd user bus socket is used
    // (escaped as a D-Bus address), never X11 autolaunch.
    TempDir temp (true); // holds a socket
    REQUIRE (! temp.path.empty());
    const std::string runtime = temp.path + "/run dir";
    REQUIRE (::mkdir (runtime.c_str(), 0700) == 0);
    ScopedEnv noAddress ("DBUS_SESSION_BUS_ADDRESS", nullptr);
    ScopedEnv runtimeDir ("XDG_RUNTIME_DIR", runtime.c_str());
    CHECK (portal::sessionBusAddress().empty()); // no socket yet
    const int listener = ::socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    REQUIRE (listener >= 0);
    sockaddr_un socketAddress {};
    socketAddress.sun_family = AF_UNIX;
    const std::string socketPath = runtime + "/bus";
    REQUIRE (socketPath.size() < sizeof (socketAddress.sun_path));
    std::memcpy (socketAddress.sun_path, socketPath.c_str(), socketPath.size() + 1);
    REQUIRE (::bind (listener, reinterpret_cast<const sockaddr*> (&socketAddress), sizeof (socketAddress)) == 0);
    CHECK (portal::sessionBusAddress() == "unix:path=" + temp.path + "/run%20dir/bus");
    ::close (listener);
    ScopedEnv relative ("XDG_RUNTIME_DIR", "relative");
    CHECK (portal::sessionBusAddress().empty());
}

TEST_CASE ("Platform: Wayland global hotkeys bind through the GlobalShortcuts portal, fire once per activation and rebind in a new session")
{
    PrivateSessionBus bus;
    if (skipWithoutDBus (bus))
        return;
    ScopedEnv busAddress ("DBUS_SESSION_BUS_ADDRESS", bus.address.c_str());
    ScopedEnv wayland ("WAYLAND_DISPLAY", "wayland-flubtest");
    MockPortal mock (true);
    REQUIRE (mock.ok());

    // The factory picks the portal in a Wayland session (also when XWayland
    // provides a DISPLAY, e.g. under xvfb-run).
    {
        auto viaFactory = GlobalHotkeys::create();
        REQUIRE (dynamic_cast<PortalGlobalHotkeys*> (viaFactory.get()) != nullptr);
        CHECK (viaFactory->isSupported());
    }
    CHECK (mock.createdSessions().empty()); // nothing registered, no session

    // A long settle time: only applyNow() sends, so every step is one
    // deterministic batch (production coalesces bursts for 50 ms).
    PortalGlobalHotkeys hotkeys (std::chrono::hours (1));
    REQUIRE (hotkeys.waitUntilProbed (kHangGuard));
    REQUIRE (hotkeys.isSupported());
    CHECK (hotkeys.getPortalVersion() == 1);

    std::atomic<int> boost { 0 }, mode { 0 }, f13 { 0 }, down { 0 };
    CHECK (hotkeys.registerHotkey (3, chord (KeyChord::Ctrl | KeyChord::Alt, 0x26), [&boost] { ++boost; }));
    CHECK (hotkeys.registerHotkey (5, chord (KeyChord::Ctrl | KeyChord::Shift, 'M'), [&mode] { ++mode; }));
    CHECK (hotkeys.registerHotkey (9, chord (KeyChord::None, kF1 + 12), [&f13] { ++f13; })); // bare F13: allowed
    CHECK (! hotkeys.registerHotkey (4, chord (KeyChord::None, 'G'), [] {}));                // would swallow typing
    CHECK (! hotkeys.registerHotkey (4, chord (KeyChord::Ctrl, 0x2A), [] {}));               // no key name
    CHECK (! hotkeys.registerHotkey (4, chord (KeyChord::Ctrl, 'G'), nullptr));
    hotkeys.applyNow();
    REQUIRE (hotkeys.waitUntilSettled (kHangGuard));

    auto binds = mock.bindCalls();
    REQUIRE (binds.size() == 1);
    REQUIRE (mock.createdSessions().size() == 1);
    const std::string session1 = mock.createdSessions()[0];
    CHECK (binds[0].session == session1);
    CHECK (mock.closedSessions().empty());
    const std::vector<MockPortal::Shortcut> expected {
        { "flubsound-3", "Flubsound Pro: Ctrl+Alt+Up", "CTRL+ALT+Up" },
        { "flubsound-5", "Flubsound Pro: Ctrl+Shift+M", "CTRL+SHIFT+m" },
        { "flubsound-9", "Flubsound Pro: F13", "F13" },
    };
    CHECK (binds[0].shortcuts == expected);

    // One callback per Activated; Deactivated (release) does nothing. Signals
    // arrive in order, so once the later "mode" activation has fired, any
    // duplicate "boost" call would have happened already.
    mock.emit ("Activated", session1, "flubsound-3");
    mock.emit ("Deactivated", session1, "flubsound-3");
    mock.emit ("Activated", session1, "flubsound-5");
    REQUIRE (waitUntil ([&mode] { return mode.load() == 1; }));
    CHECK (boost.load() == 1);
    CHECK (f13.load() == 0);

    // Another bus client cannot press our shortcuts. Its GetId round trip
    // makes the daemon route the forged signal before the portal's next one.
    {
        dbus::Connection* intruder = connectToSessionBus (*mock.api);
        REQUIRE (intruder != nullptr);
        MockPortal::sendShortcutSignal (*mock.api, mock.extra, intruder, mock.client(), "Activated", session1, "flubsound-3");
        dbus::MessageRef ping (mock.api->messageNewMethodCall ("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetId"));
        dbus::Error error;
        mock.api->errorInit (&error);
        dbus::MessageRef pong (mock.api->sendWithReplyAndBlock (intruder, ping.get(), 10000, &error));
        mock.api->errorFree (&error);
        CHECK (pong != nullptr);
        disconnect (*mock.api, intruder);
    }
    mock.emit ("Activated", session1, "flubsound-9");
    REQUIRE (waitUntil ([&f13] { return f13.load() == 1; }));
    CHECK (boost.load() == 1);

    // Unregistering re-creates the session with the remaining set; the old
    // session is closed and its late signals are ignored.
    hotkeys.unregisterHotkey (9);
    hotkeys.applyNow();
    REQUIRE (hotkeys.waitUntilSettled (kHangGuard));
    binds = mock.bindCalls();
    REQUIRE (binds.size() == 2);
    REQUIRE (mock.createdSessions().size() == 2);
    const std::string session2 = mock.createdSessions()[1];
    CHECK (session2 != session1);
    CHECK (binds[1].session == session2);
    CHECK (ids (binds[1]) == (std::vector<std::string> { "flubsound-3", "flubsound-5" }));
    CHECK (mock.closedSessions() == std::vector<std::string> { session1 });
    mock.emit ("Activated", session1, "flubsound-3");
    mock.emit ("Activated", session2, "flubsound-9"); // no longer registered
    mock.emit ("Activated", session2, "flubsound-5");
    REQUIRE (waitUntil ([&mode] { return mode.load() == 2; }));
    CHECK (boost.load() == 1);
    CHECK (f13.load() == 1);

    // HotkeyManager::registerAll() = unregisterAll() + the same chords again:
    // nothing is re-bound (no new session, no new dialog); the new callbacks
    // take over.
    hotkeys.unregisterAll();
    CHECK (hotkeys.registerHotkey (3, chord (KeyChord::Ctrl | KeyChord::Alt, 0x26), [&down] { ++down; }));
    CHECK (hotkeys.registerHotkey (5, chord (KeyChord::Ctrl | KeyChord::Shift, 'M'), [&mode] { ++mode; }));
    hotkeys.applyNow();
    REQUIRE (hotkeys.waitUntilSettled (kHangGuard));
    CHECK (mock.createdSessions().size() == 2);
    CHECK (mock.bindCalls().size() == 2);
    mock.emit ("Activated", session2, "flubsound-3");
    mock.emit ("Activated", session2, "flubsound-5");
    REQUIRE (waitUntil ([&mode] { return mode.load() == 3; }));
    CHECK (down.load() == 1);
    CHECK (boost.load() == 1);

    // A changed chord is a rebind with the new preferred trigger. The mock
    // leaves one shortcut unbound (reported to the binding listener, see the
    // next test; registerHotkey already returned true), which does not
    // affect the others.
    mock.refuse ("flubsound-7");
    CHECK (hotkeys.registerHotkey (5, chord (KeyChord::Ctrl | KeyChord::Alt, 0x28), [&mode] { ++mode; }));
    CHECK (hotkeys.registerHotkey (7, chord (KeyChord::Ctrl | KeyChord::Alt, 'P'), [] {}));
    hotkeys.applyNow();
    REQUIRE (hotkeys.waitUntilSettled (kHangGuard));
    binds = mock.bindCalls();
    REQUIRE (binds.size() == 3);
    REQUIRE (mock.createdSessions().size() == 3);
    const std::string session3 = mock.createdSessions()[2];
    CHECK (binds[2].session == session3);
    REQUIRE (binds[2].shortcuts.size() == 3);
    CHECK (binds[2].shortcuts[1] == (MockPortal::Shortcut { "flubsound-5", "Flubsound Pro: Ctrl+Alt+Down", "CTRL+ALT+Down" }));
    CHECK (mock.closedSessions() == (std::vector<std::string> { session1, session2 }));
    mock.emit ("Activated", session3, "flubsound-5");
    REQUIRE (waitUntil ([&mode] { return mode.load() == 4; }));

    // Unregistering everything closes the session without opening a new one.
    hotkeys.unregisterAll();
    hotkeys.applyNow();
    REQUIRE (hotkeys.waitUntilSettled (kHangGuard));
    CHECK (waitUntil ([&mock] { return mock.closedSessions().size() == 3; }));
    const auto closedAtEnd = mock.closedSessions();
    CHECK (! closedAtEnd.empty() && closedAtEnd.back() == session3);
    CHECK (mock.createdSessions().size() == 3);
}

TEST_CASE ("Platform: portal trigger descriptions compare by modifiers and key, and shortcut descriptions are made valid UTF-8")
{
    // What desktops answer in trigger_description for the trigger we asked for.
    CHECK (portal::sameTrigger ("CTRL+ALT+Up", "CTRL+ALT+Up"));
    CHECK (portal::sameTrigger ("CTRL+ALT+Up", "Ctrl+Alt+Up"));
    CHECK (portal::sameTrigger ("CTRL+ALT+Up", "<Control><Alt>Up"));
    CHECK (portal::sameTrigger ("CTRL+ALT+Up", "Alt + Ctrl + Up"));
    CHECK (portal::sameTrigger ("CTRL+SHIFT+m", "Shift+Ctrl+M"));
    CHECK (portal::sameTrigger ("CTRL+Page_Up", "Ctrl+PgUp"));
    CHECK (portal::sameTrigger ("CTRL+Page_Down", "Ctrl+Page Down"));
    CHECK (portal::sameTrigger ("ALT+LOGO+F13", "Meta+Alt+F13"));
    CHECK (portal::sameTrigger ("LOGO+F13", "Super+F13"));
    CHECK (portal::sameTrigger ("CTRL+Delete", "Ctrl+Del"));
    CHECK (portal::sameTrigger ("CTRL+space", "Ctrl+Space"));

    // GNOME (xdg-desktop-portal-gnome): GTK accelerators in a localised
    // sentence, one or two of them ("Press %s", "Press %s or %s").
    CHECK (portal::sameTrigger ("CTRL+ALT+Up", "Press <Control><Alt>Up"));
    CHECK (portal::sameTrigger ("CTRL+ALT+Up", "Dr\xc3\xbc" "cken Sie <Control><Alt>Up"));
    CHECK (portal::sameTrigger ("CTRL+ALT+Up", "Press <Control><Alt>Up or <Super>u"));
    CHECK (portal::sameTrigger ("LOGO+u", "Press <Control><Alt>Up or <Super>u"));
    CHECK (portal::sameTrigger ("CTRL+Page_Up", "Press <Primary>Page_Up"));
    CHECK (portal::sameTrigger ("F13", "Press F13"));
    // Qt's native text in other languages (KDE), and lists of triggers.
    CHECK (portal::sameTrigger ("CTRL+ALT+Up", "Strg+Alt+Hoch"));
    CHECK (portal::sameTrigger ("CTRL+SHIFT+Page_Up", "Strg+Umschalt+Bild auf"));
    CHECK (portal::sameTrigger ("CTRL+ALT+Page_Down", "Strg+Alt+Bild ab"));
    CHECK (portal::sameTrigger ("CTRL+ALT+Home", "Strg+Alt+Pos1"));
    CHECK (portal::sameTrigger ("CTRL+ALT+Delete", "Strg+Alt+Entf"));
    CHECK (portal::sameTrigger ("CTRL+SHIFT+Up", "Ctrl+Maj+Haut"));
    CHECK (portal::sameTrigger ("CTRL+ALT+Left", "Ctrl+Alt+Gauche"));
    CHECK (portal::sameTrigger ("CTRL+SHIFT+Down", "Ctrl+May\xc3\xbas+Abajo"));
    CHECK (portal::sameTrigger ("CTRL+ALT+Page_Up", "Ctrl+Alt+Re P\xc3\xa1g"));
    CHECK (portal::sameTrigger ("CTRL+ALT+Up", "Ctrl+Alt+Cima"));
    CHECK (portal::sameTrigger ("CTRL+space", "Strg+Leertaste"));
    CHECK (portal::sameTrigger ("CTRL+ALT+Up", "Ctrl+Alt+Up, Ctrl+U")); // either key fires
    CHECK (portal::sameTrigger ("CTRL+ALT+Up", "Ctrl+U; Ctrl+Alt+Up"));

    CHECK (! portal::sameTrigger ("CTRL+ALT+Up", "Ctrl+Alt+Down"));      // another key
    CHECK (! portal::sameTrigger ("CTRL+ALT+Up", "Ctrl+Up"));            // other modifiers
    CHECK (! portal::sameTrigger ("CTRL+ALT+Up", "Ctrl+Alt+Shift+Up"));
    CHECK (! portal::sameTrigger ("CTRL+ALT+Up", "Ctrl+Alt"));           // no key
    CHECK (! portal::sameTrigger ("CTRL+ALT+Up", "Ctrl+Alt+Down, Ctrl+U")); // neither key
    CHECK (! portal::sameTrigger ("CTRL+ALT+Up", "Strg+Alt+Runter"));    // localised, another key
    CHECK (! portal::sameTrigger ("CTRL+ALT+Up", "Press <Control><Alt>Down"));
    CHECK (! portal::sameTrigger ("CTRL+ALT+Up", "Press <Control>Up"));
    CHECK (! portal::sameTrigger ("CTRL+ALT+Up", "Press <Control><Alt>"));
    CHECK (! portal::sameTrigger ("CTRL+ALT+Up", "Hoch"));               // modifiers missing
    CHECK (! portal::sameTrigger ("F13", "Umschalt F13"));               // a modifier word is not a prefix
    CHECK (! portal::sameTrigger ("CTRL+ALT+Up", "Strg+Alt+Oben"));      // unknown name: shown as reassigned
    CHECK (! portal::sameTrigger ("CTRL+ALT+Up", ""));

    // libdbus aborts on invalid UTF-8, so a caller's description is repaired.
    CHECK (portal::validUtf8 ("Boost +10%") == "Boost +10%");
    CHECK (portal::validUtf8 ("Caf\xC3\xA9 \xE2\x86\x91 \xF0\x9F\x8E\xA7") == "Caf\xC3\xA9 \xE2\x86\x91 \xF0\x9F\x8E\xA7");
    CHECK (portal::validUtf8 ("a\xFF" "b") == "a?b");                   // not a lead byte
    CHECK (portal::validUtf8 ("\xC0\xAF") == "??");                    // overlong '/'
    CHECK (portal::validUtf8 ("\xED\xA0\x80") == "???");               // surrogate U+D800
    CHECK (portal::validUtf8 ("\xF4\x90\x80\x80") == "????");          // above U+10FFFF
    CHECK (portal::validUtf8 ("\xEF\xBF\xBF") == "???");               // noncharacter U+FFFF
    CHECK (portal::validUtf8 ("\xE2\x86") == "??");                    // truncated
    CHECK (portal::validUtf8 (std::string ("a\0b", 3)) == "a?b");
    CHECK (portal::shortcutDescription ("Boost +10%", chord (KeyChord::Ctrl | KeyChord::Alt, 0x26)) == "Flubsound Pro: Boost +10%");
    CHECK (portal::shortcutDescription ({}, chord (KeyChord::Ctrl | KeyChord::Alt, 0x26)) == "Flubsound Pro: Ctrl+Alt+Up");
}

TEST_CASE ("Platform: Wayland portal shortcuts carry the action's description and report registered, reassigned and declined bindings")
{
    PrivateSessionBus bus;
    if (skipWithoutDBus (bus))
        return;
    ScopedEnv busAddress ("DBUS_SESSION_BUS_ADDRESS", bus.address.c_str());
    ScopedEnv wayland ("WAYLAND_DISPLAY", "wayland-flubtest");
    MockPortal mock (true);
    REQUIRE (mock.ok());

    PortalGlobalHotkeys hotkeys (std::chrono::hours (1)); // only applyNow() sends
    REQUIRE (hotkeys.waitUntilProbed (kHangGuard));
    REQUIRE (hotkeys.isSupported());
    BindingLog log;
    log.attach (hotkeys);
    const auto testThread = std::this_thread::get_id();
    std::vector<std::thread::id> threads;

    // A refusal is reported at once, on the calling thread.
    CHECK (! hotkeys.registerHotkey (4, chord (KeyChord::None, 'G'), "Bare G", [] {}));
    CHECK (log.take (&threads) == (std::vector<BindingLog::Result> { { 4, Status::Unavailable, {} } }));
    CHECK (threads == std::vector<std::thread::id> { testThread });

    // The desktop words one trigger its own way (still the requested key),
    // lets the user pick another key for a second and leaves a third unbound.
    mock.describeTrigger ("flubsound-3", "Ctrl+Alt+Up");
    mock.describeTrigger ("flubsound-5", "Ctrl+Alt+PgUp");
    mock.refuse ("flubsound-7");
    const auto registerSet = [&hotkeys]
    {
        CHECK (hotkeys.registerHotkey (3, chord (KeyChord::Ctrl | KeyChord::Alt, 0x26), "Boost +10%", [] {}));
        CHECK (hotkeys.registerHotkey (5, chord (KeyChord::Ctrl | KeyChord::Alt, 0x28), "Boost -10%", [] {}));
        CHECK (hotkeys.registerHotkey (7, chord (KeyChord::Ctrl | KeyChord::Alt, 'P'), "Next Preset", [] {}));
        CHECK (hotkeys.registerHotkey (9, chord (KeyChord::None, kF1 + 12), [] {})); // no description: the chord
    };
    registerSet();
    CHECK (log.size() == 0); // asynchronous: nothing is known before the portal answers
    hotkeys.applyNow();
    REQUIRE (hotkeys.waitUntilSettled (kHangGuard));

    auto binds = mock.bindCalls();
    REQUIRE (binds.size() == 1);
    CHECK (binds[0].shortcuts == (std::vector<MockPortal::Shortcut> {
                                     { "flubsound-3", "Flubsound Pro: Boost +10%", "CTRL+ALT+Up" },
                                     { "flubsound-5", "Flubsound Pro: Boost -10%", "CTRL+ALT+Down" },
                                     { "flubsound-7", "Flubsound Pro: Next Preset", "CTRL+ALT+p" },
                                     { "flubsound-9", "Flubsound Pro: F13", "F13" },
                                 }));
    const std::vector<BindingLog::Result> firstOutcome {
        { 3, Status::Registered, {} },
        { 5, Status::Reassigned, "Ctrl+Alt+PgUp" },
        { 7, Status::Declined, {} },
        { 9, Status::Registered, {} },
    };
    CHECK (log.take (&threads) == firstOutcome); // reported before the batch settles
    REQUIRE (threads.size() == 4);
    CHECK (threads[0] != testThread); // the service's D-Bus thread

    // Nothing to rebind (HotkeyManager::registerAll() with the same set): no
    // new BindShortcuts, and the previous outcome is reported again.
    hotkeys.unregisterAll();
    registerSet();
    hotkeys.applyNow();
    REQUIRE (hotkeys.waitUntilSettled (kHangGuard));
    CHECK (mock.bindCalls().size() == 1);
    CHECK (log.take() == firstOutcome);

    // The user cancels the dialog (response 1): the whole batch is declined.
    mock.setBindResponse (1);
    CHECK (hotkeys.registerHotkey (3, chord (KeyChord::Ctrl | KeyChord::Alt, 0x24), "Boost +10%", [] {}));
    hotkeys.applyNow();
    REQUIRE (hotkeys.waitUntilSettled (kHangGuard));
    REQUIRE (mock.bindCalls().size() == 2);
    CHECK (log.take() == (std::vector<BindingLog::Result> {
                             { 3, Status::Declined, {} }, { 5, Status::Declined, {} }, { 7, Status::Declined, {} }, { 9, Status::Declined, {} } }));

    // A caller's description that is not valid UTF-8 is repaired before it
    // reaches libdbus (which would abort the process).
    mock.setBindResponse (0);
    hotkeys.unregisterAll();
    CHECK (hotkeys.registerHotkey (11, chord (KeyChord::Ctrl | KeyChord::Alt, 'K'), "Caf\xC3\xA9 \xFF mix", [] {}));
    hotkeys.applyNow();
    REQUIRE (hotkeys.waitUntilSettled (kHangGuard));
    binds = mock.bindCalls();
    REQUIRE (binds.size() == 3);
    CHECK (binds[2].shortcuts == (std::vector<MockPortal::Shortcut> { { "flubsound-11", "Flubsound Pro: Caf\xC3\xA9 ? mix", "CTRL+ALT+k" } }));
    CHECK (log.take() == (std::vector<BindingLog::Result> { { 11, Status::Registered, {} } }));

    // The desktop ends the session: its shortcuts no longer work.
    REQUIRE (mock.createdSessions().size() == 3);
    mock.closeSession (mock.createdSessions()[2]);
    REQUIRE (waitUntil ([&log] { return log.size() == 1; }));
    CHECK (log.take() == (std::vector<BindingLog::Result> { { 11, Status::Declined, {} } }));
}

TEST_CASE ("Platform: Wayland global hotkeys are unsupported without a GlobalShortcuts portal")
{
    PrivateSessionBus bus;
    if (skipWithoutDBus (bus))
        return;
    ScopedEnv busAddress ("DBUS_SESSION_BUS_ADDRESS", bus.address.c_str());
    ScopedEnv wayland ("WAYLAND_DISPLAY", "wayland-flubtest");

    // No portal on the bus at all (the probe's answer, ServiceUnknown, is
    // waited for: the constructor itself only waits briefly).
    {
        auto hotkeys = GlobalHotkeys::create();
        auto* portalHotkeys = dynamic_cast<PortalGlobalHotkeys*> (hotkeys.get());
        REQUIRE (portalHotkeys != nullptr);
        REQUIRE (portalHotkeys->waitUntilProbed (kHangGuard));
        CHECK (! hotkeys->isSupported());
        CHECK (! hotkeys->registerHotkey (1, chord (KeyChord::Ctrl | KeyChord::Alt, 'G'), [] {}));
        hotkeys->unregisterAll();
    }

    // A portal without the GlobalShortcuts interface (no "version" property).
    {
        MockPortal mock (false);
        REQUIRE (mock.ok());
        auto hotkeys = GlobalHotkeys::create();
        auto* portalHotkeys = dynamic_cast<PortalGlobalHotkeys*> (hotkeys.get());
        REQUIRE (portalHotkeys != nullptr);
        REQUIRE (portalHotkeys->waitUntilProbed (kHangGuard));
        CHECK (! hotkeys->isSupported());
        CHECK (! hotkeys->registerHotkey (1, chord (KeyChord::Ctrl | KeyChord::Alt, 'G'), [] {}));
        CHECK (mock.createdSessions().empty());
    }

    // No session bus.
    {
        ScopedEnv noAddress ("DBUS_SESSION_BUS_ADDRESS", nullptr);
        ScopedEnv noRuntimeDir ("XDG_RUNTIME_DIR", nullptr);
        auto hotkeys = GlobalHotkeys::create();
        CHECK (! hotkeys->isSupported());
    }
}

TEST_CASE ("Platform: Wayland portal probe does not block start-up: a portal that answers late or appears later is used")
{
    PrivateSessionBus bus;
    if (skipWithoutDBus (bus))
        return;
    ScopedEnv busAddress ("DBUS_SESSION_BUS_ADDRESS", bus.address.c_str());
    ScopedEnv wayland ("WAYLAND_DISPLAY", "wayland-flubtest");
    const std::vector<BindingLog::Result> registered { { 3, Status::Registered, {} } };

    // A portal still being started at login answers the probe late. The
    // constructor does not wait for it (here: not at all): the service
    // counts as supported and the request waits for the answer.
    {
        MockPortal mock (true);
        mock.holdVersion();
        REQUIRE (mock.ok());
        BindingLog log; // outlives the service, which may report until destroyed
        PortalGlobalHotkeys hotkeys (std::chrono::hours (1), std::chrono::milliseconds (0));
        log.attach (hotkeys);
        CHECK (hotkeys.isSupported());
        REQUIRE (waitUntil ([&mock] { return mock.versionProbes() == 1; }));
        CHECK (hotkeys.registerHotkey (3, chord (KeyChord::Ctrl | KeyChord::Alt, 0x26), "Boost +10%", [] {}));
        hotkeys.applyNow();
        CHECK (! hotkeys.waitUntilSettled (std::chrono::milliseconds (100))); // nothing can be bound yet
        CHECK (mock.bindCalls().empty());
        CHECK (log.size() == 0);

        mock.releaseVersion();
        REQUIRE (hotkeys.waitUntilSettled (kHangGuard));
        CHECK (hotkeys.getPortalVersion() == 1);
        CHECK (mock.bindCalls().size() == 1);
        CHECK (log.take() == registered);
    }

    // No portal at start-up: unsupported, until one appears on the bus
    // (NameOwnerChanged), which is probed and then used.
    BindingLog log; // outlives the service, which reports the portal leaving
    PortalGlobalHotkeys hotkeys (std::chrono::hours (1));
    REQUIRE (hotkeys.waitUntilProbed (kHangGuard));
    CHECK (! hotkeys.isSupported());
    CHECK (! hotkeys.registerHotkey (3, chord (KeyChord::Ctrl | KeyChord::Alt, 0x26), [] {}));

    MockPortal mock (true);
    REQUIRE (mock.ok());
    REQUIRE (waitUntil ([&hotkeys] { return hotkeys.isSupported(); }));
    CHECK (hotkeys.getPortalVersion() == 1);
    log.attach (hotkeys);
    std::atomic<int> boost { 0 };
    CHECK (hotkeys.registerHotkey (3, chord (KeyChord::Ctrl | KeyChord::Alt, 0x26), "Boost +10%", [&boost] { ++boost; }));
    hotkeys.applyNow();
    REQUIRE (hotkeys.waitUntilSettled (kHangGuard));
    CHECK (log.take() == registered);
    REQUIRE (mock.createdSessions().size() == 1);
    mock.emit ("Activated", mock.createdSessions()[0], "flubsound-3");
    REQUIRE (waitUntil ([&boost] { return boost.load() == 1; }));
}

TEST_CASE ("Platform: Wayland portal: after the session bus is lost, shortcuts are reported unavailable and new requests are refused")
{
    PrivateSessionBus bus;
    if (skipWithoutDBus (bus))
        return;
    ScopedEnv busAddress ("DBUS_SESSION_BUS_ADDRESS", bus.address.c_str());
    ScopedEnv wayland ("WAYLAND_DISPLAY", "wayland-flubtest");
    MockPortal mock (true);
    REQUIRE (mock.ok());
    BindingLog log;
    PortalGlobalHotkeys hotkeys (std::chrono::hours (1));
    REQUIRE (hotkeys.waitUntilProbed (kHangGuard));
    REQUIRE (hotkeys.isSupported());
    log.attach (hotkeys);
    CHECK (hotkeys.registerHotkey (3, chord (KeyChord::Ctrl | KeyChord::Alt, 0x26), "Boost +10%", [] {}));
    hotkeys.applyNow();
    REQUIRE (hotkeys.waitUntilSettled (kHangGuard));
    CHECK (log.take() == (std::vector<BindingLog::Result> { { 3, Status::Registered, {} } }));

    bus.stop(); // dbus restart: the connection is gone
    REQUIRE (waitUntil ([&hotkeys] { return ! hotkeys.isSupported(); }));
    // Refused and reported at once, not left "waiting for the desktop".
    CHECK (! hotkeys.registerHotkey (5, chord (KeyChord::Ctrl | KeyChord::Alt, 0x28), "Boost -10%", [] {}));
    const auto reportedFor = [&log] (int id)
    {
        std::lock_guard<std::mutex> guard (log.mutex);
        return std::any_of (log.results.begin(), log.results.end(), [id] (const BindingLog::Result& r) { return r.id == id; });
    };
    REQUIRE (waitUntil ([&reportedFor] { return reportedFor (3) && reportedFor (5); }));
    hotkeys.unregisterAll();
    CHECK (hotkeys.waitUntilSettled (kHangGuard));
    // The bus may first announce the portal leaving (reported as well).
    for (const auto& r : log.take())
        CHECK (r.status == Status::Unavailable);
}

TEST_CASE ("Platform: Wayland portal: an answer for a set changed since, or for a session the desktop closed, is not reported as the outcome")
{
    PrivateSessionBus bus;
    if (skipWithoutDBus (bus))
        return;
    ScopedEnv busAddress ("DBUS_SESSION_BUS_ADDRESS", bus.address.c_str());
    ScopedEnv wayland ("WAYLAND_DISPLAY", "wayland-flubtest");
    MockPortal mock (true);
    REQUIRE (mock.ok());
    BindingLog log;
    PortalGlobalHotkeys hotkeys (std::chrono::hours (1));
    REQUIRE (hotkeys.waitUntilProbed (kHangGuard));
    log.attach (hotkeys);

    // The user changes the chord while the desktop's dialog for the first
    // one is open, then cancels that dialog: the new chord is bound, and
    // only its outcome is reported (not "Declined" for the old chord).
    mock.holdBinds();
    mock.setBindResponse (1);
    CHECK (hotkeys.registerHotkey (3, chord (KeyChord::Ctrl | KeyChord::Alt, 0x26), "Boost +10%", [] {}));
    hotkeys.applyNow();
    REQUIRE (waitUntil ([&mock] { return mock.bindCalls().size() == 1; }));
    mock.setBindResponse (0);
    CHECK (hotkeys.registerHotkey (3, chord (KeyChord::Ctrl | KeyChord::Alt, 0x24), "Boost +10%", [] {}));
    hotkeys.applyNow();
    mock.releaseBinds();
    REQUIRE (hotkeys.waitUntilSettled (kHangGuard));
    auto binds = mock.bindCalls();
    REQUIRE (binds.size() == 2);
    CHECK (binds[1].shortcuts.at (0).preferredTrigger == "CTRL+ALT+Home");
    CHECK (log.take() == (std::vector<BindingLog::Result> { { 3, Status::Registered, {} } }));

    // The desktop closes the session while BindShortcuts waits for the
    // user: declined at once, and the late success is ignored (those keys
    // could never fire). Only the next batch's outcome (declined by the
    // user here) follows.
    mock.holdBinds();
    CHECK (hotkeys.registerHotkey (3, chord (KeyChord::Ctrl | KeyChord::Alt, 0x26), "Boost +10%", [] {}));
    hotkeys.applyNow();
    REQUIRE (waitUntil ([&mock] { return mock.bindCalls().size() == 3; }));
    REQUIRE (mock.createdSessions().size() == 3);
    mock.closeSession (mock.createdSessions()[2]);
    REQUIRE (waitUntil ([&log] { return log.size() == 1; }));
    CHECK (log.take() == (std::vector<BindingLog::Result> { { 3, Status::Declined, {} } }));
    mock.releaseBinds(); // response 0 for the closed session
    mock.setBindResponse (1);
    CHECK (hotkeys.registerHotkey (3, chord (KeyChord::Ctrl | KeyChord::Alt, 0x28), "Boost +10%", [] {}));
    hotkeys.applyNow();
    REQUIRE (hotkeys.waitUntilSettled (kHangGuard));
    CHECK (mock.bindCalls().size() == 4);
    CHECK (log.take() == (std::vector<BindingLog::Result> { { 3, Status::Declined, {} } }));
}

TEST_CASE ("Platform: Wayland portal: GNOME's and localised descriptions of the requested key count as registered, a GNOME shortcut without a key does not")
{
    PrivateSessionBus bus;
    if (skipWithoutDBus (bus))
        return;
    ScopedEnv busAddress ("DBUS_SESSION_BUS_ADDRESS", bus.address.c_str());
    ScopedEnv wayland ("WAYLAND_DISPLAY", "wayland-flubtest");
    MockPortal mock (true);
    REQUIRE (mock.ok());
    mock.describeTrigger ("flubsound-3", "Strg+Alt+Hoch");                     // KDE in German
    mock.describeTrigger ("flubsound-5", "Press <Control><Alt>Down");          // GNOME
    mock.describeTrigger ("flubsound-7", "Appuyez sur <Control><Alt>p ou <Super>u"); // GNOME, localised, two keys
    mock.describeTrigger ("flubsound-8", "Strg+Alt+Oben");                     // not understood
    mock.omitTrigger ("flubsound-9");                                          // no trigger_description
    const auto registerSet = [] (PortalGlobalHotkeys& hotkeys)
    {
        CHECK (hotkeys.registerHotkey (3, chord (KeyChord::Ctrl | KeyChord::Alt, 0x26), "Boost +10%", [] {}));
        CHECK (hotkeys.registerHotkey (5, chord (KeyChord::Ctrl | KeyChord::Alt, 0x28), "Boost -10%", [] {}));
        CHECK (hotkeys.registerHotkey (7, chord (KeyChord::Ctrl | KeyChord::Alt, 'P'), "Next Preset", [] {}));
        CHECK (hotkeys.registerHotkey (8, chord (KeyChord::Ctrl | KeyChord::Alt, 0x24), "Mode", [] {}));
        CHECK (hotkeys.registerHotkey (9, chord (KeyChord::Ctrl | KeyChord::Alt, 'B'), "Bypass", [] {}));
        hotkeys.applyNow();
    };

    // GNOME leaves trigger_description out when the user removed the key.
    {
        ScopedEnv desktop ("XDG_CURRENT_DESKTOP", "ubuntu:GNOME");
        BindingLog log;
        PortalGlobalHotkeys hotkeys (std::chrono::hours (1));
        REQUIRE (hotkeys.waitUntilProbed (kHangGuard));
        log.attach (hotkeys);
        registerSet (hotkeys);
        REQUIRE (hotkeys.waitUntilSettled (kHangGuard));
        CHECK (log.take() == (std::vector<BindingLog::Result> { { 3, Status::Registered, {} },
                                                                { 5, Status::Registered, {} },
                                                                { 7, Status::Registered, {} },
                                                                { 8, Status::Reassigned, "Strg+Alt+Oben" },
                                                                { 9, Status::Declined, {} } }));
    }
    // Elsewhere a missing description means the requested key.
    {
        ScopedEnv desktop ("XDG_CURRENT_DESKTOP", "KDE");
        BindingLog log;
        PortalGlobalHotkeys hotkeys (std::chrono::hours (1));
        REQUIRE (hotkeys.waitUntilProbed (kHangGuard));
        log.attach (hotkeys);
        registerSet (hotkeys);
        REQUIRE (hotkeys.waitUntilSettled (kHangGuard));
        const auto results = log.take();
        REQUIRE (results.size() == 5);
        CHECK (results[4] == (BindingLog::Result { 9, Status::Registered, {} }));
    }
}

// ---------------------------------------------------------------------------
// Foreground application (automatic profiles)
// ---------------------------------------------------------------------------
TEST_CASE ("Platform: foreground process names: Wine / Proton programs by their Windows executable, others by /proc exe, comm or argv[0]")
{
    using namespace std::string_literals;
    ForegroundAppInfo info;

    // Wine and Proton loaders run the program named by argv[0].
    foreground::describeExecutable ("/usr/bin/wine64-preloader", "C:\\Games\\CS2\\cs2.exe\0-steam\0"s, "cs2.exe", info);
    CHECK (info.executablePath == "C:\\Games\\CS2\\cs2.exe");
    CHECK (info.executableName == "cs2.exe");
    foreground::describeExecutable ("/home/u/.steam/steam/steamapps/common/Proton 9.0/files/bin/wine-preloader",
                                    "Z:\\home\\u\\Games\\Game.EXE\0"s, "Game.EXE", info);
    CHECK (info.executableName == "Game.EXE");

    // A wine loader that is not running a Windows program is itself.
    foreground::describeExecutable ("/usr/bin/wine64", "/usr/bin/wine64\0--version\0"s, "wine64", info);
    CHECK (info.executablePath == "/usr/bin/wine64");
    CHECK (info.executableName == "wine64");

    // Native programs: the resolved /proc/<pid>/exe link, whatever argv[0] says.
    foreground::describeExecutable ("/usr/lib/firefox/firefox", "firefox.exe\0-new-tab\0"s, "firefox", info);
    CHECK (info.executablePath == "/usr/lib/firefox/firefox");
    CHECK (info.executableName == "firefox");

    // Another user's process (exe unreadable): comm, then argv[0]'s file name.
    foreground::describeExecutable ("", "/usr/sbin/sshd\0-D\0"s, "sshd", info);
    CHECK (info.executablePath.empty());
    CHECK (info.executableName == "sshd");
    foreground::describeExecutable ("", "/opt/tool/bin/tool\0"s, "", info);
    CHECK (info.executableName == "tool");

    // This process, and one that does not exist.
    REQUIRE (foreground::describeProcess (static_cast<uint32_t> (::getpid()), info));
    CHECK (info.isThisProcess);
    CHECK (info.processId == static_cast<uint32_t> (::getpid()));
    CHECK (info.executablePath == std::filesystem::read_symlink ("/proc/self/exe").string());
    CHECK (info.executableName == std::filesystem::read_symlink ("/proc/self/exe").filename().string());
    CHECK (info.bundleId.empty());
    CHECK (! foreground::describeProcess (4294967290u, info));
}

TEST_CASE ("Platform: foreground application detection is unsupported under Wayland and without an X display, and says why")
{
    {
        ScopedEnv wayland ("WAYLAND_DISPLAY", "wayland-flubtest");
        auto foregroundApp = ForegroundApp::create();
        REQUIRE (foregroundApp != nullptr);
        CHECK (! foregroundApp->isSupported());
        ForegroundAppInfo info;
        CHECK (! foregroundApp->query (info));
        CHECK (foregroundApp->unsupportedReason().find ("Wayland") != std::string::npos);
    }
    {
        ScopedEnv noWayland ("WAYLAND_DISPLAY", nullptr);
        ScopedEnv session ("XDG_SESSION_TYPE", "x11");
        ScopedEnv noDisplay ("DISPLAY", nullptr);
        auto foregroundApp = ForegroundApp::create();
        REQUIRE (foregroundApp != nullptr);
        CHECK (! foregroundApp->isSupported());
        ForegroundAppInfo info;
        CHECK (! foregroundApp->query (info));
        CHECK (! foregroundApp->unsupportedReason().empty());
        CHECK (foregroundApp->unsupportedReason().find ("Wayland") == std::string::npos);
    }
}

#if FLUB_HAVE_X11_HEADERS
TEST_CASE ("Platform: X11 foreground app follows _NET_ACTIVE_WINDOW and _NET_WM_PID, and reports none for a missing or destroyed window")
{
    // Needs a bare X server (CI: Xvfb). Xvfb has no window manager, so the
    // test publishes _NET_ACTIVE_WINDOW itself; with a window manager
    // running (_NET_SUPPORTING_WM_CHECK set) it would fight over the
    // property, so the test is skipped then.
    ScopedEnv noWayland ("WAYLAND_DISPLAY", nullptr);
    ScopedEnv session ("XDG_SESSION_TYPE", nullptr);
    auto foregroundApp = ForegroundApp::create();
    if (! foregroundApp->isSupported())
    {
        std::cerr << "    (no X11 display: skipped)\n";
        return;
    }
    CHECK (foregroundApp->unsupportedReason().empty());

    const X11Api* x = X11Api::get();
    REQUIRE (x != nullptr);
    using CreateWindowFn = Window (*) (Display*, Window, int, int, unsigned int, unsigned int, unsigned int, unsigned long, unsigned long);
    using ChangePropertyFn = int (*) (Display*, Window, Atom, Atom, int, int, const unsigned char*, int);
    using DeletePropertyFn = int (*) (Display*, Window, Atom);
    using DestroyWindowFn = int (*) (Display*, Window);
    const auto createWindow = reinterpret_cast<CreateWindowFn> (::dlsym (x->lib, "XCreateSimpleWindow"));
    const auto changeProperty = reinterpret_cast<ChangePropertyFn> (::dlsym (x->lib, "XChangeProperty"));
    const auto deleteProperty = reinterpret_cast<DeletePropertyFn> (::dlsym (x->lib, "XDeleteProperty"));
    const auto destroyWindow = reinterpret_cast<DestroyWindowFn> (::dlsym (x->lib, "XDestroyWindow"));
    REQUIRE (createWindow != nullptr);
    REQUIRE (changeProperty != nullptr);
    REQUIRE (deleteProperty != nullptr);
    REQUIRE (destroyWindow != nullptr);

    Display* d = x->openDisplay (nullptr);
    REQUIRE (d != nullptr);
    const Window root = x->defaultRootWindow (d);
    const Atom activeAtom = x->internAtom (d, "_NET_ACTIVE_WINDOW", False);
    const Atom pidAtom = x->internAtom (d, "_NET_WM_PID", False);
    {
        Atom type = 0;
        int format = 0;
        unsigned long count = 0, remaining = 0;
        unsigned char* data = nullptr;
        x->getWindowProperty (d, root, x->internAtom (d, "_NET_SUPPORTING_WM_CHECK", False), 0, 1, False, XA_WINDOW, &type, &format, &count,
                              &remaining, &data);
        if (data != nullptr)
            x->free (data);
        if (count > 0)
        {
            std::cerr << "    (a window manager is running: skipped)\n";
            x->closeDisplay (d);
            return;
        }
    }

    const auto setProperty = [&] (Window w, Atom property, Atom type, unsigned long value)
    {
        const long item = static_cast<long> (value); // format 32 items are longs in Xlib
        changeProperty (d, w, property, type, 32, PropModeReplace, reinterpret_cast<const unsigned char*> (&item), 1);
        x->sync (d, False);
    };
    const auto makeWindow = [&] (uint32_t pid)
    {
        const Window w = createWindow (d, root, 0, 0, 16, 16, 0, 0, 0);
        if (pid != 0)
            setProperty (w, pidAtom, XA_CARDINAL, pid);
        x->sync (d, False);
        return w;
    };
    const auto activate = [&] (Window w) { setProperty (root, activeAtom, XA_WINDOW, w); };

    ForegroundAppInfo info;

    // No _NET_ACTIVE_WINDOW at all (no window manager): no answer.
    deleteProperty (d, root, activeAtom);
    x->sync (d, False);
    CHECK (! foregroundApp->query (info));

    // A window of this process.
    const auto self = static_cast<uint32_t> (::getpid());
    const Window own = makeWindow (self);
    activate (own);
    REQUIRE (foregroundApp->query (info));
    CHECK (info.processId == self);
    CHECK (info.isThisProcess);
    CHECK (info.executablePath == std::filesystem::read_symlink ("/proc/self/exe").string());
    CHECK (info.executableName == std::filesystem::read_symlink ("/proc/self/exe").filename().string());

    // A window of another process (posix_spawn returns after the exec). The
    // kernel's /proc/<pid>/exe names the resolved binary, which is not
    // "sleep" where sleep links to a multi-call binary (busybox, uutils
    // coreutils) or /bin to /usr/bin: compare with the canonical path.
    const std::string sleepProgram = findInPath ("sleep"); // not always /bin/sleep (NixOS)
    REQUIRE (! sleepProgram.empty());
    pid_t child = 0;
    char arg0[] = "sleep", arg1[] = "30";
    char* childArgs[] = { arg0, arg1, nullptr };
    REQUIRE (::posix_spawn (&child, sleepProgram.c_str(), nullptr, nullptr, childArgs, environ) == 0);
    struct ChildGuard
    {
        pid_t pid;
        ~ChildGuard()
        {
            ::kill (pid, SIGKILL);
            int status = 0;
            ::waitpid (pid, &status, 0);
        }
    } childGuard { child };
    const auto sleepBinary = std::filesystem::canonical (sleepProgram);
    const Window other = makeWindow (static_cast<uint32_t> (child));
    activate (other);
    REQUIRE (foregroundApp->query (info));
    CHECK (info.processId == static_cast<uint32_t> (child));
    CHECK (! info.isThisProcess);
    CHECK (info.executablePath == sleepBinary.string());
    CHECK (info.executableName == sleepBinary.filename().string());

    // Back to our own window: the cached description is not reused for it.
    activate (own);
    REQUIRE (foregroundApp->query (info));
    CHECK (info.processId == self);
    CHECK (info.isThisProcess);

    // A window without _NET_WM_PID, a destroyed window (BadWindow must be
    // swallowed, not abort the process) and "no active window" (0).
    const Window anonymous = makeWindow (0);
    activate (anonymous);
    CHECK (! foregroundApp->query (info));
    activate (other);
    REQUIRE (foregroundApp->query (info));
    destroyWindow (d, other);
    x->sync (d, False);
    CHECK (! foregroundApp->query (info));
    activate (0);
    CHECK (! foregroundApp->query (info));

    destroyWindow (d, own);
    destroyWindow (d, anonymous);
    deleteProperty (d, root, activeAtom);
    x->sync (d, False);
    x->closeDisplay (d);
}
#endif

// ---------------------------------------------------------------------------
// PipeWire quantum request and monitor links (docs/11 E48a). No PipeWire is
// needed: the helpers are pure, and the router runs against fake pw-dump /
// pw-link scripts on PATH.
// ---------------------------------------------------------------------------
TEST_CASE ("Platform: PipeWire quantum request: 256/48000 on Balanced, 128/48000 locked on Low Latency, a user's value kept")
{
    using pipewire::QuantumRequest;

    auto plan = pipewire::planLatencyEnvironment (QuantumRequest::Balanced, nullptr, nullptr);
    CHECK (plan.latency == "256/48000");
    CHECK (plan.props.empty()); // Balanced never locks the quantum

    plan = pipewire::planLatencyEnvironment (QuantumRequest::LowLatency, nullptr, nullptr);
    CHECK (plan.latency == "128/48000");
    CHECK (plan.props == "{ node.lock-quantum = true }");

    // An empty variable counts as unset; the user's own props are kept.
    plan = pipewire::planLatencyEnvironment (QuantumRequest::LowLatency, "", "{ node.name = x }");
    CHECK (plan.latency == "128/48000");
    CHECK (plan.props.empty());

    // A value the user exported wins, whatever the request.
    plan = pipewire::planLatencyEnvironment (QuantumRequest::LowLatency, "1024/48000", nullptr);
    CHECK (plan.latency.empty());
    CHECK (plan.props.empty());

    CHECK (pipewire::isValidLatency (pipewire::kBalancedLatency));
    CHECK (pipewire::isValidLatency (pipewire::kLowLatency));
    CHECK (pipewire::isValidLatency ("1/8000"));
    CHECK (pipewire::isValidLatency ("8192/768000"));
    for (const char* bad : { "", "256", "/48000", "256/", "0/48000", "-1/48000", "256/48000x", "25 6/48000", "9000/48000", "256/100", "256/48000/1",
                             "00000256/48000" })
        CHECK (! pipewire::isValidLatency (bad));

    {
        ScopedEnv latency ("PIPEWIRE_LATENCY", nullptr);
        ScopedEnv props ("PIPEWIRE_PROPS", nullptr);
        pipewire::applyLatencyEnvironment (QuantumRequest::LowLatency);
        REQUIRE (std::getenv ("PIPEWIRE_LATENCY") != nullptr && std::getenv ("PIPEWIRE_PROPS") != nullptr);
        CHECK (std::string (std::getenv ("PIPEWIRE_LATENCY")) == "128/48000");
        CHECK (std::string (std::getenv ("PIPEWIRE_PROPS")) == "{ node.lock-quantum = true }");
    }
    {
        // The app's router asks for Balanced when it is created, before the
        // audio device opens, and leaves a user's value alone.
        ScopedEnv latency ("PIPEWIRE_LATENCY", nullptr);
        ScopedEnv props ("PIPEWIRE_PROPS", nullptr);
        auto router = AppAudioRouter::create();
        REQUIRE (std::getenv ("PIPEWIRE_LATENCY") != nullptr);
        CHECK (std::string (std::getenv ("PIPEWIRE_LATENCY")) == "256/48000");
        CHECK (std::getenv ("PIPEWIRE_PROPS") == nullptr);
    }
    {
        ScopedEnv latency ("PIPEWIRE_LATENCY", "2048/48000");
        auto router = AppAudioRouter::create();
        CHECK (std::string (std::getenv ("PIPEWIRE_LATENCY")) == "2048/48000");
    }
}

namespace
{
/** A trimmed `pw-dump` (PipeWire 1.0) with "@PID@" for this process: the
    Game sink (8 monitor ports and a playback port), the Music sink (its
    ports listed out of order), a Chat sink without port.monitor flags,
    Flubsound's JACK client (pid only on its client object; 10 audio inputs,
    an output and a MIDI input), another process's JACK client, a microphone,
    and two links: Game FL -> in_1 (made by JUCE) and the microphone -> in_2
    (someone else's). */
std::string pwDumpFixture (uint32_t pid, const std::string& extraLinks = {})
{
    std::string json = R"([
  {"id":0,"type":"PipeWire:Interface:Core","info":{"name":"pipewire-0"}},
  {"id":"junk","type":"PipeWire:Interface:Node"},
  {"id":30,"type":"PipeWire:Interface:Client","info":{"props":{"pipewire.sec.pid":@PID@,"application.process.id":"2"}}},
  {"id":40,"type":"PipeWire:Interface:Node","info":{"props":{"node.name":"flubsound_game","media.class":"Audio/Sink","object.id":40}}},
)";
    const char* const channels[] = { "FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR" };
    for (int k = 0; k < 8; ++k)
        json += R"(  {"id":)" + std::to_string (41 + k) + R"(,"type":"PipeWire:Interface:Port","info":{"direction":"output","props":{"port.name":"monitor_)"
                + channels[k] + R"(","port.id":)" + std::to_string (k)
                + R"(,"node.id":40,"port.monitor":true,"format.dsp":"32 bit float mono audio"}}},
)";
    json += R"(  {"id":49,"type":"PipeWire:Interface:Port","info":{"direction":"input","props":{"port.name":"playback_FL","port.id":0,"node.id":40}}},
  {"id":60,"type":"PipeWire:Interface:Node","info":{"props":{"node.name":"flubsound_music","media.class":"Audio/Sink"}}},
  {"id":61,"type":"PipeWire:Interface:Port","info":{"direction":"output","props":{"port.name":"monitor_FR","port.id":"1","node.id":"60","port.monitor":true}}},
  {"id":62,"type":"PipeWire:Interface:Port","info":{"direction":"output","props":{"port.name":"monitor_FL","port.id":"0","node.id":"60","port.monitor":true}}},
  {"id":70,"type":"PipeWire:Interface:Node","info":{"props":{"node.name":"flubsound_chat","media.class":"Audio/Sink"}}},
  {"id":71,"type":"PipeWire:Interface:Port","info":{"props":{"port.direction":"out","port.name":"monitor_FL","port.id":0,"node.id":70}}},
  {"id":72,"type":"PipeWire:Interface:Port","info":{"props":{"port.direction":"out","port.name":"monitor_FR","port.id":1,"node.id":70}}},
  {"id":80,"type":"PipeWire:Interface:Node","info":{"props":{"node.name":"JUCEJack","media.class":"Stream/Duplex/Audio","client.id":30}}},
)";
    for (int k = 0; k < 10; ++k)
        json += R"(  {"id":)" + std::to_string (81 + k) + R"(,"type":"PipeWire:Interface:Port","info":{"direction":"input","props":{"port.name":"in_)"
                + std::to_string (k + 1) + R"(","port.id":)" + std::to_string (k) + R"(,"node.id":80,"format.dsp":"32 bit float mono audio"}}},
)";
    json += R"(  {"id":91,"type":"PipeWire:Interface:Port","info":{"direction":"output","props":{"port.name":"out_1","port.id":0,"node.id":80}}},
  {"id":92,"type":"PipeWire:Interface:Port","info":{"direction":"input","props":{"port.name":"midi_in","port.id":10,"node.id":80,"format.dsp":"8 bit raw midi"}}},
  {"id":100,"type":"PipeWire:Interface:Node","info":{"props":{"node.name":"JUCEJack","media.class":"Stream/Duplex/Audio","application.process.id":"1"}}},
  {"id":101,"type":"PipeWire:Interface:Port","info":{"direction":"input","props":{"port.name":"in_1","port.id":0,"node.id":100}}},
  {"id":110,"type":"PipeWire:Interface:Node","info":{"props":{"node.name":"alsa_input.usb-mic","media.class":"Audio/Source"}}},
  {"id":111,"type":"PipeWire:Interface:Port","info":{"direction":"output","props":{"port.name":"capture_FL","port.id":0,"node.id":110}}},
  {"id":120,"type":"PipeWire:Interface:Link","info":{"output-node-id":40,"output-port-id":41,"input-node-id":80,"input-port-id":81,"state":"active"}},
  {"id":121,"type":"PipeWire:Interface:Link","info":{"output-node-id":110,"output-port-id":111,"input-node-id":80,"input-port-id":82}})"
            + extraLinks + R"(
])";
    const std::string marker = "@PID@";
    json.replace (json.find (marker), marker.size(), std::to_string (pid));
    return json;
}

constexpr uint32_t kFixturePid = 4321;

std::vector<AppAudioRouter::EndpointInput> stripInputs (int game, int music, int chat, int system)
{
    return { { "flubsound_game", game }, { "flubsound_music", music }, { "flubsound_chat", chat }, { "flubsound_system", system } };
}

bool hasLink (const std::vector<pipewire::PortLink>& links, uint32_t out, uint32_t in)
{
    return std::find (links.begin(), links.end(), pipewire::PortLink { out, in }) != links.end();
}
} // namespace

TEST_CASE ("Platform: pw-dump JSON is read into nodes, ports and links; malformed output is reported")
{
    pipewire::Graph graph;
    std::string error;
    REQUIRE (pipewire::parseDump (pwDumpFixture (kFixturePid), graph, error));
    CHECK (graph.nodes.size() == 6);
    CHECK (graph.nodes[40].name == "flubsound_game");
    CHECK (graph.nodes[40].mediaClass == "Audio/Sink");
    CHECK (graph.nodes[80].processId == kFixturePid); // from its client's pipewire.sec.pid, not the sandbox-local "2"
    CHECK (graph.nodes[100].processId == 1);
    CHECK (graph.links.size() == 2);
    CHECK (graph.links[0].outputPort == 41 && graph.links[0].inputPort == 81);

    const auto monitors = pipewire::audioPorts (graph, 60, false, true);
    REQUIRE (monitors.size() == 2);
    CHECK (monitors[0]->id == 62); // port.id 0 first, although listed second
    CHECK (pipewire::audioPorts (graph, 70, false, true).size() == 2); // "monitor_" names without port.monitor
    CHECK (pipewire::audioPorts (graph, 40, false, true).size() == 8);
    CHECK (pipewire::audioPorts (graph, 80, true, false).size() == 10); // the MIDI input is not audio

    graph = {};
    CHECK (! pipewire::parseDump ("pw-dump: unknown option", graph, error));
    CHECK (! error.empty());
    error.clear();
    CHECK (! pipewire::parseDump (R"({"id":1})", graph, error));
    CHECK (! error.empty());
    CHECK (pipewire::parseDump ("[]", graph, error));
    CHECK (graph.nodes.empty());
}

TEST_CASE ("Platform: PipeWire monitor links follow the device input map, up to the next strip and the engine's last input")
{
    pipewire::Graph graph;
    std::string error;
    REQUIRE (pipewire::parseDump (pwDumpFixture (kFixturePid), graph, error));

    // Game=0;Music=8: 8 + 2 links into Flubsound's own node, not the other JACK client's.
    auto plan = pipewire::planMonitorLinks (graph, kFixturePid, stripInputs (0, 8, -1, -1));
    CHECK (plan.engineNode == 80);
    CHECK (plan.engineInputs == 10);
    CHECK (plan.problems.empty());
    REQUIRE (plan.wanted.size() == 10);
    for (uint32_t k = 0; k < 8; ++k)
        CHECK (hasLink (plan.wanted, 41 + k, 81 + k));
    CHECK (hasLink (plan.wanted, 62, 89)); // FL -> input channel 9
    CHECK (hasLink (plan.wanted, 61, 90));
    REQUIRE (plan.sinks.size() == 2);
    CHECK (plan.sinks[1].sinkName == "flubsound_music" && plan.sinks[1].firstChannel == 8 && plan.sinks[1].channels == 2);

    // Music at channel 4 cuts the Game sink to four links, and says so.
    plan = pipewire::planMonitorLinks (graph, kFixturePid, stripInputs (0, 4, -1, -1));
    CHECK (plan.wanted.size() == 6);
    CHECK (! hasLink (plan.wanted, 45, 85));
    CHECK (hasLink (plan.wanted, 62, 85));
    REQUIRE (plan.problems.size() == 1);
    CHECK (plan.problems[0].find ("Only 4 of the 8 channels of 'flubsound_game'") != std::string::npos);

    // Past the engine's inputs, a missing sink, and the last inputs.
    plan = pipewire::planMonitorLinks (graph, kFixturePid, stripInputs (-1, 9, 10, 2));
    CHECK (plan.wanted.size() == 1); // Music at 9: only its FL fits
    CHECK (hasLink (plan.wanted, 62, 90));
    REQUIRE (plan.problems.size() == 3);
    CHECK (plan.problems[0].find ("Only 1 of the 2 channels of 'flubsound_music'") != std::string::npos);
    CHECK (plan.problems[1].find ("feeds input channel 11, but Flubsound's input has only 10 channels") != std::string::npos);
    CHECK (plan.problems[2].find ("no sink 'flubsound_system'") != std::string::npos);

    // Nothing mapped: nothing to do, not even a problem.
    plan = pipewire::planMonitorLinks (graph, kFixturePid, stripInputs (-1, -1, -1, -1));
    CHECK (plan.wanted.empty() && plan.problems.empty());

    // Another process: Flubsound has no PipeWire input node (e.g. an ALSA hw: device).
    plan = pipewire::planMonitorLinks (graph, 999, stripInputs (0, -1, -1, -1));
    CHECK (plan.engineNode == 0);
    CHECK (plan.wanted.empty());
    REQUIRE (plan.problems.size() == 1);
    CHECK (plan.problems[0].find ("JACK device type") != std::string::npos);

    // The command lines carry port ids only.
    CHECK (pipewire::linkCommand ({ 41, 81 }, false) == "LC_ALL=C pw-link 41 81 2>&1");
    CHECK (pipewire::linkCommand ({ 62, 89 }, true) == "LC_ALL=C pw-link -d 62 89 2>&1");
    CHECK (pipewire::alreadyLinked ("failed to link ports: File exists"));
    CHECK (! pipewire::alreadyLinked ("failed to link ports: No such file or directory"));
}

TEST_CASE ("Platform: the Linux router links the sink monitors with pw-link, re-checks, unlinks on a map change and explains missing tools")
{
    TempDir temp;
    REQUIRE (! temp.path.empty());
    const auto write = [] (const std::string& path, const std::string& text)
    {
        std::ofstream out (path, std::ios::trunc);
        out << text;
    };
    const auto read = [] (const std::string& path)
    {
        std::ifstream in (path);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    };
    const auto lines = [&read] (const std::string& path)
    {
        std::vector<std::string> result;
        std::istringstream in (read (path));
        for (std::string line; std::getline (in, line);)
            result.push_back (line);
        return result;
    };

    // Fake tools: pw-dump prints the fixture and counts its runs, pw-link logs its arguments.
    const std::string bin = temp.path + "/bin", dumpFile = temp.path + "/dump.json", linkLog = temp.path + "/pw-link.log",
                      dumpCount = temp.path + "/pw-dump.count";
    std::filesystem::create_directory (bin);
    write (bin + "/pw-dump", "#!/bin/sh\necho run >> '" + dumpCount + "'\ncat '" + dumpFile + "'\n");
    write (bin + "/pw-link", "#!/bin/sh\necho \"$*\" >> '" + linkLog + "'\n");
    std::filesystem::permissions (bin + "/pw-dump", std::filesystem::perms::owner_all);
    std::filesystem::permissions (bin + "/pw-link", std::filesystem::perms::owner_all);

    const auto ownPid = static_cast<uint32_t> (::getpid());
    write (dumpFile, pwDumpFixture (ownPid));

    const std::string path = bin + ":/usr/bin:/bin";
    ScopedEnv pathEnv ("PATH", path.c_str());
    auto router = AppAudioRouter::create();
    REQUIRE (router != nullptr);

    // Game=0;Music=8: nine new links (Game FL -> in_1 exists already).
    std::string status;
    CHECK (router->connectEndpointInputs (stripInputs (0, 8, -1, -1), status));
    CHECK (status.empty());
    auto linked = lines (linkLog);
    REQUIRE (linked.size() == 9);
    CHECK (linked[0] == "42 82"); // the microphone's link into in_2 stays; ours is added
    CHECK (linked[7] == "62 89");
    CHECK (linked[8] == "61 90");
    CHECK (lines (dumpCount).size() == 1);

    // The next pass confirms them (they exist now) and changes nothing ...
    std::string extra;
    for (uint32_t k = 1; k < 8; ++k)
        extra += ",\n  {\"id\":" + std::to_string (200 + k) + ",\"type\":\"PipeWire:Interface:Link\",\"info\":{\"output-port-id\":" + std::to_string (41 + k)
                 + ",\"input-port-id\":" + std::to_string (81 + k) + "}}";
    extra += R"(,
  {"id":210,"type":"PipeWire:Interface:Link","info":{"output-port-id":62,"input-port-id":89}},
  {"id":211,"type":"PipeWire:Interface:Link","info":{"output-port-id":61,"input-port-id":90}})";
    write (dumpFile, pwDumpFixture (ownPid, extra));
    CHECK (router->connectEndpointInputs (stripInputs (0, 8, -1, -1), status));
    CHECK (lines (linkLog).size() == 9);
    CHECK (lines (dumpCount).size() == 2);

    // ... and an unchanged map is not re-read before the re-check interval.
    CHECK (router->connectEndpointInputs (stripInputs (0, 8, -1, -1), status));
    CHECK (lines (dumpCount).size() == 2);

    // Music moves to channel 6 (Game shrinks to 6): the links the router made
    // and no longer wants are removed, the new ones made; JUCE's Game FL link
    // and the microphone's are not touched.
    CHECK (! router->connectEndpointInputs (stripInputs (0, 6, -1, -1), status));
    CHECK (status.find ("Only 6 of the 8 channels of 'flubsound_game'") != std::string::npos);
    CHECK (lines (dumpCount).size() == 3);
    linked = lines (linkLog);
    const std::vector<std::string> changes (linked.begin() + 9, linked.end());
    const auto made = [&changes] (const std::string& line) { return std::find (changes.begin(), changes.end(), line) != changes.end(); };
    CHECK (made ("-d 47 87"));
    CHECK (made ("-d 48 88"));
    CHECK (made ("-d 62 89"));
    CHECK (made ("-d 61 90"));
    CHECK (made ("62 87"));
    CHECK (made ("61 88"));
    CHECK (! made ("-d 41 81"));
    CHECK (! made ("-d 111 82"));
    CHECK (changes.size() == 6);

    // Device input switched off: the router's own links go, nothing else.
    write (dumpFile, pwDumpFixture (ownPid, extra + R"(,
  {"id":212,"type":"PipeWire:Interface:Link","info":{"output-port-id":62,"input-port-id":87}},
  {"id":213,"type":"PipeWire:Interface:Link","info":{"output-port-id":61,"input-port-id":88}})"));
    CHECK (router->connectEndpointInputs (stripInputs (-1, -1, -1, -1), status));
    linked = lines (linkLog);
    const std::vector<std::string> removals (linked.begin() + 15, linked.end());
    CHECK (removals.size() == 7); // 42..46 -> 82..86 and Music's two
    CHECK (std::all_of (removals.begin(), removals.end(), [] (const std::string& l) { return l.rfind ("-d ", 0) == 0; }));
    const auto countBefore = lines (dumpCount).size();
    CHECK (router->connectEndpointInputs (stripInputs (-1, -1, -1, -1), status));
    CHECK (lines (dumpCount).size() == countBefore); // nothing mapped, nothing ours: no pw-dump

    // Without the PipeWire tools the router says what to do instead.
    const std::string emptyBin = temp.path + "/empty";
    std::filesystem::create_directory (emptyBin);
    ScopedEnv noTools ("PATH", emptyBin.c_str());
    auto bare = AppAudioRouter::create();
    CHECK (! bare->connectEndpointInputs (stripInputs (0, -1, -1, -1), status));
    CHECK (status.find ("pw-dump and pw-link were not found") != std::string::npos);
    CHECK (status.find ("qpwgraph") != std::string::npos);
    CHECK (bare->connectEndpointInputs (stripInputs (-1, -1, -1, -1), status)); // nothing to link: fine without them
    CHECK (status.empty());
}

// ---------------------------------------------------------------------------
// docs/11 E44: real-time scheduling of the audio thread - no allocation when
// the device thread promotes itself, and RealtimeKit asked from another
// thread (a mock rtkit on a private dbus-daemon standing in for the system
// bus).
// ---------------------------------------------------------------------------
TEST_CASE ("Platform: promoteAudioThread allocates nothing, and a thread's scheduling is read back by its kernel thread id (E44)")
{
    CHECK (RealtimeScheduling::currentThreadId() == static_cast<uint64_t> (::syscall (SYS_gettid)));
    CHECK (! RealtimeScheduling::queryThread (0).known);
    CHECK (! RealtimeScheduling::queryThread (uint64_t { 1 } << 40).known);

    int64_t allocations = -1;
    bool readBack = false, restored = false;
    std::thread worker (
        [&]
        {
            const uint64_t self = RealtimeScheduling::currentThreadId();
            const auto before = RealtimeScheduling::queryThread (self);
            // The first device callback's promotion: the saved policy lives in
            // the thread's own storage, nothing is allocated.
            void* handle = nullptr;
            {
                flubtest::AllocationGuard guard;
                handle = SystemTuning::promoteAudioThread();
                allocations = guard.allocations();
            }
            const auto during = RealtimeScheduling::queryThread (self);
            // Without RT rights (CI) the thread stays SCHED_OTHER; with them it
            // runs FIFO 20 (or at the user's lower rtprio limit).
            readBack = before.known && ! before.realtime && before.policy == "OTHER" && before.priority == 0 && during.known
                       && (handle != nullptr ? during.realtime && during.policy == "FIFO" && during.priority >= 1 && during.priority <= 20
                                             : ! during.realtime);
            SystemTuning::revertAudioThread (handle);
            restored = ! RealtimeScheduling::queryThread (self).realtime;
        });
    worker.join();
    CHECK (allocations == 0);
    CHECK (readBack);
    CHECK (restored);
}

namespace
{
/** A private connection to the bus at 'address' (nullptr on failure). */
dbus::Connection* connectToBus (const dbus::Api& api, const std::string& address)
{
    dbus::Error error;
    api.errorInit (&error);
    dbus::Connection* connection = api.connectionOpenPrivate (address.c_str(), &error);
    if (connection != nullptr && api.busRegister (connection, &error) == 0)
    {
        api.connectionClose (connection);
        api.connectionUnref (connection);
        connection = nullptr;
    }
    api.errorFree (&error);
    return connection;
}

/** The "Max realtime timeout" hard limit in /proc/<pid>/limits, in us; -1 =
    unlimited or unreadable. */
int64_t rttimeHardLimit (uint32_t pid)
{
    std::ifstream limits ("/proc/" + std::to_string (pid) + "/limits");
    for (std::string line; std::getline (limits, line);)
        if (line.rfind ("Max realtime timeout", 0) == 0)
        {
            std::istringstream fields (line.substr (std::strlen ("Max realtime timeout")));
            std::string soft, hard;
            fields >> soft >> hard;
            return hard == "unlimited" || hard.empty() ? -1 : std::stoll (hard);
        }
    return -1;
}

/** In-process stand-in for rtkit-daemon: owns org.freedesktop.RealtimeKit1 on
    the private bus, answers the MaxRealtimePriority / RTTimeUSecMax
    properties and checks MakeThreadRealtime the way rtkit does: the caller's
    pid from the bus, the thread among that process's tasks, the priority at
    most MaxRealtimePriority and the process's RLIMIT_RTTIME hard limit at
    most RTTimeUSecMax (read from /proc/<pid>/limits, as rtkit reads it).
    Served by its own thread; the test reads what it recorded under its
    mutex. */
class MockRtkit
{
public:
    struct Call
    {
        std::string member;     // "Get:<property>" or "MakeThreadRealtime"
        uint64_t thread = 0;    // MakeThreadRealtime
        uint32_t priority = 0;  // MakeThreadRealtime
        uint32_t pid = 0;       // the caller's process (MakeThreadRealtime)
        int64_t rttimeHard = 0; // its RLIMIT_RTTIME hard limit at the call; -1 = unlimited
        bool threadInProcess = false;
    };

    MockRtkit (const std::string& address, bool withPropertiesIn, int32_t maxPriorityIn, int64_t rttimeMaxIn)
        : withProperties (withPropertiesIn), maxPriority (maxPriorityIn), rttimeMax (rttimeMaxIn)
    {
        if (api == nullptr || ! extra.ok || (connection = connectToBus (*api, address)) == nullptr)
            return;
        dbus::Error error;
        api->errorInit (&error);
        const int reply = extra.requestName (connection, rtkit::kService, 4 /* DO_NOT_QUEUE */, &error);
        api->errorFree (&error);
        if (reply != 1 /* PRIMARY_OWNER */)
            return;
        if (api->connectionGetUnixFd (connection, &busFd) == 0 || ::pipe2 (wakePipe, O_CLOEXEC | O_NONBLOCK) != 0)
            return;
        running = true;
        thread = std::thread ([this] { serve(); });
        // Its first messages (NameAcquired) handled, it waits in poll(): a
        // child forked from here on finds no libdbus lock held by it.
        waitUntil ([this] { return idle.load(); });
    }

    ~MockRtkit()
    {
        running = false;
        if (thread.joinable())
        {
            const char byte = 0;
            [[maybe_unused]] const auto written = ::write (wakePipe[1], &byte, 1);
            thread.join();
        }
        disconnect (*api, connection);
        for (const int fd : wakePipe)
            if (fd >= 0)
                ::close (fd);
    }

    bool ok() const { return thread.joinable(); }

    /** MakeThreadRealtime answers org.freedesktop.DBus.Error.AccessDenied,
        as rtkit does over its burst limit or for another user's thread. */
    void refuseAll()
    {
        std::lock_guard<std::mutex> guard (mutex);
        refusing = true;
    }

    std::vector<Call> calls()
    {
        std::lock_guard<std::mutex> guard (mutex);
        return recorded;
    }

private:
    void serve()
    {
        pollfd fds[2] = { { busFd, POLLIN, 0 }, { wakePipe[0], POLLIN, 0 } };
        while (running && api->connectionReadWrite (connection, 0) != 0)
        {
            while (dbus::Message* message = api->connectionPopMessage (connection))
            {
                const dbus::MessageRef owner (message);
                if (api->messageGetType (message) == 1 /* method call */)
                    handleCall (message);
            }
            if (api->connectionGetDispatchStatus (connection) == dbus::kDispatchDataRemains)
                continue;
            fds[0].events = static_cast<short> (POLLIN | (api->connectionHasMessagesToSend (connection) != 0 ? POLLOUT : 0));
            idle = true;
            ::poll (fds, 2, -1);
            char buffer[16];
            while (::read (wakePipe[0], buffer, sizeof (buffer)) > 0) {}
        }
    }

    void send (dbus::Message* message)
    {
        api->connectionSend (connection, message, nullptr);
        api->connectionFlush (connection);
    }

    void replyError (dbus::Message* call, const char* name, const char* text)
    {
        dbus::MessageRef reply (extra.newError (call, name, text));
        send (reply.get());
    }

    /** The caller's pid, asked of the bus (GetConnectionUnixProcessID). */
    uint32_t callerPid (const std::string& sender)
    {
        dbus::MessageRef request (api->messageNewMethodCall ("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                                              "GetConnectionUnixProcessID"));
        dbus::Iter args;
        api->iterInitAppend (request.get(), &args);
        dbus::appendBasic (*api, &args, dbus::kTypeString, sender);
        dbus::Error error;
        api->errorInit (&error);
        dbus::MessageRef reply (api->sendWithReplyAndBlock (connection, request.get(), 5000, &error));
        api->errorFree (&error);
        uint32_t pid = 0;
        dbus::Iter result;
        if (reply != nullptr && api->iterInit (reply.get(), &result) != 0 && api->iterGetArgType (&result) == dbus::kTypeUInt32)
            api->iterGetBasic (&result, &pid);
        return pid;
    }

    void handleCall (dbus::Message* call)
    {
        const std::string interfaceName = dbus::str (api->messageGetInterface (call));
        const std::string member = dbus::str (api->messageGetMember (call));
        dbus::Iter args;
        api->iterInit (call, &args);

        if (interfaceName == "org.freedesktop.DBus.Properties" && member == "Get")
        {
            std::string iface, property;
            dbus::readString (*api, &args, iface);
            api->iterNext (&args);
            dbus::readString (*api, &args, property);
            {
                std::lock_guard<std::mutex> guard (mutex);
                recorded.push_back ({ "Get:" + property });
            }
            // rtkit before 0.11 has no properties at all.
            if (! withProperties)
                return replyError (call, "org.freedesktop.DBus.Error.UnknownMethod", "No such method 'Get'");
            dbus::MessageRef reply (extra.newMethodReturn (call));
            dbus::Iter out, variant;
            api->iterInitAppend (reply.get(), &out);
            if (iface == rtkit::kInterface && property == "MaxRealtimePriority")
            {
                api->iterOpenContainer (&out, dbus::kTypeVariant, "i", &variant);
                api->iterAppendBasic (&variant, 'i', &maxPriority);
            }
            else if (iface == rtkit::kInterface && property == "RTTimeUSecMax")
            {
                api->iterOpenContainer (&out, dbus::kTypeVariant, "x", &variant);
                api->iterAppendBasic (&variant, 'x', &rttimeMax);
            }
            else
                return replyError (call, "org.freedesktop.DBus.Error.InvalidArgs", "No such property");
            api->iterCloseContainer (&out, &variant);
            return send (reply.get());
        }

        if (interfaceName == rtkit::kInterface && member == "MakeThreadRealtime")
        {
            Call made;
            made.member = member;
            if (api->iterGetArgType (&args) == 't')
                api->iterGetBasic (&args, &made.thread);
            api->iterNext (&args);
            if (api->iterGetArgType (&args) == dbus::kTypeUInt32)
                api->iterGetBasic (&args, &made.priority);
            made.pid = callerPid (dbus::str (api->messageGetSender (call)));
            made.rttimeHard = rttimeHardLimit (made.pid);
            struct stat info {};
            made.threadInProcess = made.pid != 0
                                   && ::stat (("/proc/" + std::to_string (made.pid) + "/task/" + std::to_string (made.thread)).c_str(), &info) == 0;
            bool refuse = false;
            {
                std::lock_guard<std::mutex> guard (mutex);
                recorded.push_back (made);
                refuse = refusing;
            }
            const int32_t limit = withProperties ? maxPriority : 20;
            const int64_t rttimeLimit = withProperties ? rttimeMax : 200000;
            if (refuse || ! made.threadInProcess || made.priority < 1 || made.priority > static_cast<uint32_t> (limit)
                || made.rttimeHard < 0 || made.rttimeHard > rttimeLimit)
                return replyError (call, "org.freedesktop.DBus.Error.AccessDenied", "Operation not permitted");
            dbus::MessageRef reply (extra.newMethodReturn (call));
            return send (reply.get());
        }

        replyError (call, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method");
    }

    const dbus::Api* api = dbus::Api::get();
    MockDBusApi extra { *dbus::Api::get() };
    dbus::Connection* connection = nullptr;
    const bool withProperties;
    int32_t maxPriority;
    int64_t rttimeMax;
    int busFd = -1;
    int wakePipe[2] = { -1, -1 };
    std::atomic<bool> running { false }, idle { false };
    std::thread thread;
    std::mutex mutex;
    std::vector<Call> recorded;
    bool refusing = false;
};

/** What a RealtimeKit request made in a child process came to. */
struct ChildRequest
{
    bool ran = false;
    RealtimeKitResult result;
    uint64_t thread = 0;
    uint32_t pid = 0;
    int64_t rttimeSoft = -2, rttimeHard = -2; // the child's RLIMIT_RTTIME afterwards; -1 = unlimited
};

/** Runs requestRealtimeKit (for the child's own thread) in a forked child:
    it lowers the process's RLIMIT_RTTIME, which must not stay lowered in
    the test binary. The child is single-threaded and only talks to the bus;
    the mock rtkit's thread sits in poll() while it runs. */
ChildRequest requestInChild (int wantedPriority)
{
    ChildRequest out;
    int fds[2] = { -1, -1 };
    if (::pipe2 (fds, O_CLOEXEC) != 0)
        return out;
    const pid_t child = ::fork();
    if (child == 0)
    {
        ::close (fds[0]);
        const uint64_t self = RealtimeScheduling::currentThreadId();
        const auto result = RealtimeScheduling::requestRealtimeKit (self, wantedPriority);
        rlimit limit {};
        ::getrlimit (RLIMIT_RTTIME, &limit);
        const auto asUsec = [] (rlim_t v) { return v == RLIM_INFINITY ? int64_t { -1 } : static_cast<int64_t> (v); };
        std::ostringstream text;
        text << static_cast<int> (result.outcome) << ' ' << result.priority << ' ' << result.rttimeUsec << ' ' << self << ' '
             << asUsec (limit.rlim_cur) << ' ' << asUsec (limit.rlim_max) << ' ' << result.message;
        const std::string line = text.str();
        [[maybe_unused]] const auto written = ::write (fds[1], line.data(), line.size());
        ::_exit (0);
    }
    ::close (fds[1]);
    if (child < 0)
    {
        ::close (fds[0]);
        return out;
    }
    std::string line;
    pollfd readable { fds[0], POLLIN, 0 };
    const auto deadline = std::chrono::steady_clock::now() + kHangGuard;
    for (;;)
    {
        if (std::chrono::steady_clock::now() > deadline)
        {
            ::kill (child, SIGKILL);
            break;
        }
        if (::poll (&readable, 1, 100) <= 0)
            continue;
        char buffer[512];
        const ssize_t n = ::read (fds[0], buffer, sizeof (buffer));
        if (n <= 0)
            break;
        line.append (buffer, static_cast<size_t> (n));
    }
    ::close (fds[0]);
    int status = 0;
    ::waitpid (child, &status, 0);
    std::istringstream fields (line);
    int outcome = -1;
    if (! (fields >> outcome >> out.result.priority >> out.result.rttimeUsec >> out.thread >> out.rttimeSoft >> out.rttimeHard))
        return out;
    out.result.outcome = static_cast<RealtimeKitResult::Outcome> (outcome);
    std::getline (fields >> std::ws, out.result.message);
    out.pid = static_cast<uint32_t> (child);
    out.ran = WIFEXITED (status) && WEXITSTATUS (status) == 0;
    return out;
}
} // namespace

TEST_CASE ("Platform: RealtimeKit is asked for the audio thread at <= MaxRealtimePriority, after RLIMIT_RTTIME is lowered to RTTimeUSecMax (E44)")
{
    PrivateSessionBus bus;
    if (skipWithoutDBus (bus))
        return;
    ScopedEnv systemBus ("DBUS_SYSTEM_BUS_ADDRESS", bus.address.c_str());
    using Outcome = RealtimeKitResult::Outcome;

    {
        // rtkit 0.11+: priority 20 asked, capped at its maximum of 15;
        // RLIMIT_RTTIME (unlimited in a test process) lowered to its 150 ms.
        MockRtkit rtkitd (bus.address, true, 15, 150000);
        REQUIRE (rtkitd.ok());
        const auto request = requestInChild (20);
        REQUIRE (request.ran);
        CHECK (request.result.outcome == Outcome::Granted);
        CHECK (request.result.message.empty());
        CHECK (request.result.priority == 15);
        CHECK (request.result.rttimeUsec == 150000);
        CHECK (request.rttimeHard == 150000);
        CHECK (request.rttimeSoft >= 0);
        CHECK (request.rttimeSoft <= 150000);

        const auto calls = rtkitd.calls();
        REQUIRE (calls.size() == 3);
        CHECK (calls[0].member == "Get:MaxRealtimePriority");
        CHECK (calls[1].member == "Get:RTTimeUSecMax");
        CHECK (calls[2].member == "MakeThreadRealtime");
        CHECK (calls[2].thread == request.thread);
        CHECK (calls[2].pid == request.pid);
        CHECK (calls[2].threadInProcess);
        CHECK (calls[2].priority == 15);
        // The precondition held when rtkit looked, not only afterwards.
        CHECK (calls[2].rttimeHard == 150000);
    }

    {
        // Asking for less than the maximum asks for exactly that.
        MockRtkit rtkitd (bus.address, true, 15, 150000);
        REQUIRE (rtkitd.ok());
        const auto request = requestInChild (5);
        CHECK (request.result.outcome == Outcome::Granted);
        CHECK (request.result.priority == 5);
    }

    {
        // rtkit older than 0.11: no properties, its defaults (20, 200 ms).
        MockRtkit rtkitd (bus.address, false, 0, 0);
        REQUIRE (rtkitd.ok());
        const auto request = requestInChild (20);
        CHECK (request.result.outcome == Outcome::Granted);
        CHECK (request.result.priority == 20);
        CHECK (request.rttimeHard == 200000);
        const auto calls = rtkitd.calls();
        REQUIRE (! calls.empty());
        CHECK (calls.back().member == "MakeThreadRealtime");
        CHECK (calls.back().rttimeHard == 200000);
    }

    {
        // rtkit refuses (its burst limit, or policy): Refused, and why.
        MockRtkit rtkitd (bus.address, true, 20, 200000);
        REQUIRE (rtkitd.ok());
        rtkitd.refuseAll();
        const auto request = requestInChild (20);
        CHECK (request.result.outcome == Outcome::Refused);
        CHECK (request.result.message.find ("RealtimeKit refused real-time priority 20") != std::string::npos);
        CHECK (request.result.message.find ("Operation not permitted") != std::string::npos);
    }

    // No rtkit on the bus: Unavailable before any limit is touched (so this
    // runs in the test process itself).
    rlimit before {};
    ::getrlimit (RLIMIT_RTTIME, &before);
    auto missing = RealtimeScheduling::requestRealtimeKit (RealtimeScheduling::currentThreadId(), 20);
    CHECK (missing.outcome == Outcome::Unavailable);
    CHECK (missing.message.find ("RealtimeKit is not running") != std::string::npos);
    rlimit after {};
    ::getrlimit (RLIMIT_RTTIME, &after);
    CHECK (after.rlim_max == before.rlim_max);
    CHECK (after.rlim_cur == before.rlim_cur);

    // No system bus at all.
    {
        ScopedEnv noBus ("DBUS_SYSTEM_BUS_ADDRESS", ("unix:path=" + bus.dir.path + "/no-such-socket").c_str());
        const auto unreachable = RealtimeScheduling::requestRealtimeKit (RealtimeScheduling::currentThreadId(), 20);
        CHECK (unreachable.outcome == Outcome::Unavailable);
        CHECK (unreachable.message.find ("system D-Bus is not reachable") != std::string::npos);
    }
    CHECK (RealtimeScheduling::requestRealtimeKit (0, 20).outcome == Outcome::Unavailable);
}

// ---------------------------------------------------------------------------
// docs/11 E27 step 4: the ALSA capture channel map of a card PCM.
// ---------------------------------------------------------------------------
TEST_CASE ("Platform: ALSA channel maps - card PCM names, JUCE's device names and chmap positions (E27)")
{
    std::string card;
    int device = -1, subdevice = -1;
    CHECK (alsa::parseHwName ("hw:CARD=PCH,DEV=3", card, device, subdevice));
    CHECK ((card == "PCH" && device == 3 && subdevice == -1));
    CHECK (alsa::parseHwName ("hw:1,0,2", card, device, subdevice));
    CHECK ((card == "1" && device == 0 && subdevice == 2));
    CHECK (alsa::parseHwName ("hw:CARD=Generic", card, device, subdevice));
    CHECK ((card == "Generic" && device == 0 && subdevice == -1));
    CHECK (! alsa::parseHwName ("hw:CARD=PCH,DEV=x", card, device, subdevice));
    for (const char* plugin : { "default", "pipewire", "pulse", "plughw:0,0", "surround71:CARD=PCH,DEV=0", "hw:" })
        CHECK (! alsa::parseHwName (plugin, card, device, subdevice));

    CHECK (alsa::hintDeviceName ("hw:CARD=PCH,DEV=0", "HDA Intel PCH, ALC892 Analog\nDirect hardware device without any conversions")
           == "HDA Intel PCH, ALC892 Analog; Direct hardware device without any conversions");
    CHECK (alsa::hintDeviceName ("pipewire", "") == "pipewire");

    // snd_pcm_chmap_query_t lists as libasound returns them: a USB audio
    // class capture PCM reporting its 7.1 channels in WAVE order and a stereo
    // map, then one whose positions are all unknown.
    using P = SpeakerPosition;
    const auto query = [] (int type, std::vector<unsigned int> positions)
    {
        std::vector<unsigned int> words { static_cast<unsigned int> (type), static_cast<unsigned int> (positions.size()) };
        words.insert (words.end(), positions.begin(), positions.end());
        return words;
    };
    auto wave71 = query (1 /* FIXED */, { 3, 4, 7, 8, 5, 6, 9, 10 });
    auto stereo = query (2 /* VAR */, { 3, 4 });
    auto unknown6 = query (1, { 0, 0, 0, 0, 1, 2 });
    const alsa::ChmapQuery* maps[] = { reinterpret_cast<const alsa::ChmapQuery*> (wave71.data()),
                                       reinterpret_cast<const alsa::ChmapQuery*> (stereo.data()),
                                       reinterpret_cast<const alsa::ChmapQuery*> (unknown6.data()), nullptr };
    CHECK ((alsa::positionsFromChmaps (maps, 8) == std::vector<P> { P::FL, P::FR, P::FC, P::LFE, P::RL, P::RR, P::SL, P::SR }));
    CHECK ((alsa::positionsFromChmaps (maps, 2) == std::vector<P> { P::FL, P::FR }));
    CHECK (alsa::positionsFromChmaps (maps, 6).empty()); // nothing known
    CHECK (alsa::positionsFromChmaps (maps, 4).empty()); // no map for 4
    CHECK (alsa::positionsFromChmaps (nullptr, 8).empty());
    auto alsa71 = query (1, { 3, 4, 5, 6, 7, 8, 9, 10 });
    const alsa::ChmapQuery* alsaMaps[] = { reinterpret_cast<const alsa::ChmapQuery*> (alsa71.data()), nullptr };
    CHECK ((alsa::positionsFromChmaps (alsaMaps, 8) == std::vector<P> { P::FL, P::FR, P::RL, P::RR, P::FC, P::LFE, P::SL, P::SR }));

    // Only JUCE ALSA devices on a sound card have a map here; this machine's
    // cards (if any) have no device of these names.
    CHECK (AudioChannelMaps::queryInputPositions ("JACK", "Flubsound Game", 8).empty());
    CHECK (AudioChannelMaps::queryInputPositions ("ALSA", "Flubsound test: no such device", 8).empty());
    CHECK (AudioChannelMaps::queryInputPositions ("ALSA HW", "Flubsound test: no such card, no such PCM", 8).empty());
    CHECK (AudioChannelMaps::queryInputPositions ("ALSA", "", 8).empty());
}

#endif // __linux__
