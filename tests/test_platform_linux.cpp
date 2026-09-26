// Tests for the Linux platform services (headless: no audio server, no
// display needed). The implementation files are compiled into this test TU
// directly so the pactl JSON parsing and argument sanitising - which live in
// an unnamed namespace - can be tested without widening the app's interface.
// On other operating systems this file compiles to nothing.
#if defined(__linux__)

#include "TestFramework.h"

#include "../app/Source/platform/PlatformServices_common.cpp"
#include "../app/Source/platform/PlatformServices_linux.cpp"

#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <thread>

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

#if FLUB_HAVE_X11_HEADERS
TEST_CASE ("Platform: X11 global hotkeys fire once per press, refuse a chord another client holds, and release on unregister")
{
    // Needs an X server (CI runs the platform tests under Xvfb too) and
    // libXtst to synthesise key events; skipped otherwise.
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
    REQUIRE (hotkeys->registerHotkey (7, chordG, [&fired] { ++fired; }));

    const auto key = [&] (KeySym sym, bool down) {
        fakeKey (d, x->keysymToKeycode (d, sym), down ? True : False, 0);
        x->flush (d);
    };
    const auto waitFor = [&fired] (int count) {
        for (int i = 0; i < 200 && fired.load() < count; ++i)
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
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

    // Another client (a second service) cannot take the same chord ...
    auto other = GlobalHotkeys::create();
    REQUIRE (other->isSupported());
    CHECK (! other->registerHotkey (1, chordG, [] {}));
    // ... until it is released here.
    hotkeys->unregisterHotkey (7);
    CHECK (other->registerHotkey (1, chordG, [] {}));
    other->unregisterAll();

    // Navigation keys work too (the app's defaults are Ctrl+Alt+arrows).
    std::atomic<int> arrow { 0 };
    REQUIRE (hotkeys->registerHotkey (10, chord (KeyChord::Ctrl | KeyChord::Alt, 0x26), [&arrow] { ++arrow; }));
    key (XK_Control_L, true);
    key (XK_Alt_L, true);
    key (XK_Up, true);
    key (XK_Up, false);
    key (XK_Alt_L, false);
    key (XK_Control_L, false);
    for (int i = 0; i < 200 && arrow.load() < 1; ++i)
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    CHECK (arrow.load() == 1);
    hotkeys->unregisterHotkey (10);

    // Bare keys and unmappable key codes are refused.
    CHECK (! hotkeys->registerHotkey (8, chord (KeyChord::None, 'G'), [] {}));
    CHECK (! hotkeys->registerHotkey (9, chord (KeyChord::Ctrl, 0x13), [] {}));
    CHECK (! hotkeys->registerHotkey (9, chord (KeyChord::Shift, 'G'), [] {}));

    x->closeDisplay (d);
    ::dlclose (xtst);
}
#endif

// ---------------------------------------------------------------------------
// Start with the OS: XDG autostart entry
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

/** A fresh directory under $TMPDIR (or /tmp), removed with its contents. */
struct TempDir
{
    TempDir()
    {
        const char* base = std::getenv ("TMPDIR");
        std::string pattern = std::string (base != nullptr && base[0] == '/' ? base : "/tmp") + "/flub-autostart-XXXXXX";
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
/** Bound for every wait on the bus, the daemon or the service thread: a hang
    guard only, never a timing assertion. */
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

/** A private dbus-daemon (session bus configuration) for one test: started
    with --nofork, its address read from a pipe, SIGTERMed and reaped on
    destruction. address stays empty when dbus-daemon is missing or fails. */
class PrivateSessionBus
{
public:
    PrivateSessionBus()
    {
        const std::string daemon = findInPath ("dbus-daemon");
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

    ~PrivateSessionBus()
    {
        if (pid > 0)
        {
            ::kill (pid, SIGTERM);
            int status = 0;
            ::waitpid (pid, &status, 0);
        }
    }

    PrivateSessionBus (const PrivateSessionBus&) = delete;
    PrivateSessionBus& operator= (const PrivateSessionBus&) = delete;

    TempDir dir;
    pid_t pid = -1;
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
            if (! globalShortcuts || iface != portal::kShortcutsInterface || property != "version")
                return replyError (call, "org.freedesktop.DBus.Error.InvalidArgs", "No such interface or property");
            dbus::MessageRef reply (extra.newMethodReturn (call));
            dbus::Iter out, variant;
            const uint32_t version = 1;
            api->iterInitAppend (reply.get(), &out);
            api->iterOpenContainer (&out, dbus::kTypeVariant, "u", &variant);
            api->iterAppendBasic (&variant, dbus::kTypeUInt32, &version);
            api->iterCloseContainer (&out, &variant);
            return send (reply.get());
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
            return respond (sender, requestBase + token,
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
            std::set<std::string> refusedNow;
            {
                std::lock_guard<std::mutex> guard (mutex);
                binds.push_back (bind);
                refusedNow = refused;
            }
            replyPath (call, requestBase + token);
            return respond (sender, requestBase + token,
                            [&] (dbus::Iter* results)
                            {
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
                                    dbus::appendDictEntry (*api, &properties, "trigger_description", dbus::kTypeString, shortcut.preferredTrigger);
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

    /** Request::Response (0 = success, results) to the caller only. */
    template <typename AddResults>
    void respond (const std::string& to, const std::string& requestPath, AddResults addResults)
    {
        dbus::MessageRef signal (extra.newSignal (requestPath.c_str(), portal::kRequestInterface, "Response"));
        dbus::Iter args, results;
        const uint32_t code = 0;
        extra.setDestination (signal.get(), to.c_str());
        api->iterInitAppend (signal.get(), &args);
        api->iterAppendBasic (&args, dbus::kTypeUInt32, &code);
        api->iterOpenContainer (&args, dbus::kTypeArray, "{sv}", &results);
        addResults (&results);
        api->iterCloseContainer (&args, &results);
        send (signal.get());
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
    std::set<std::string> refused;
};

std::vector<std::string> ids (const MockPortal::Bind& bind)
{
    std::vector<std::string> result;
    for (const auto& shortcut : bind.shortcuts)
        result.push_back (shortcut.id);
    return result;
}

/** Skips (true) when libdbus-1 or dbus-daemon is missing. */
bool skipWithoutDBus (const PrivateSessionBus& bus)
{
    if (dbus::Api::get() == nullptr)
    {
        std::cerr << "    (libdbus-1.so.3 not available: skipped)\n";
        return true;
    }
    if (bus.address.empty())
    {
        std::cerr << "    (dbus-daemon not available: skipped)\n";
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
    TempDir temp;
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
    // leaves one shortcut unbound (logged to stderr; registerHotkey already
    // returned true), which does not affect the others.
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

TEST_CASE ("Platform: Wayland global hotkeys are unsupported without a GlobalShortcuts portal")
{
    PrivateSessionBus bus;
    if (skipWithoutDBus (bus))
        return;
    ScopedEnv busAddress ("DBUS_SESSION_BUS_ADDRESS", bus.address.c_str());
    ScopedEnv wayland ("WAYLAND_DISPLAY", "wayland-flubtest");

    // No portal on the bus at all.
    {
        auto hotkeys = GlobalHotkeys::create();
        REQUIRE (hotkeys != nullptr);
        CHECK (! hotkeys->isSupported());
        CHECK (! hotkeys->registerHotkey (1, chord (KeyChord::Ctrl | KeyChord::Alt, 'G'), [] {}));
        hotkeys->unregisterAll();
    }

    // A portal without the GlobalShortcuts interface (no "version" property).
    {
        MockPortal mock (false);
        REQUIRE (mock.ok());
        auto hotkeys = GlobalHotkeys::create();
        REQUIRE (dynamic_cast<PortalGlobalHotkeys*> (hotkeys.get()) != nullptr);
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

#endif // __linux__
