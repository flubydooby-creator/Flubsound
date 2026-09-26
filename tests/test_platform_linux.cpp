// Tests for the Linux platform services (headless: no audio server, no
// display needed). The implementation files are compiled into this test TU
// directly so the pactl JSON parsing and argument sanitising - which live in
// an unnamed namespace - can be tested without widening the app's interface.
// On other operating systems this file compiles to nothing.
#if defined(__linux__)

#include "TestFramework.h"

#include "../app/Source/platform/PlatformServices_common.cpp"
#include "../app/Source/platform/PlatformServices_linux.cpp"

#include <atomic>
#include <chrono>
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
    // Global hotkeys need an X11 display (and no Wayland session); headless
    // runs must report them unsupported instead of failing.
    auto hotkeys = GlobalHotkeys::create();
    REQUIRE (hotkeys != nullptr);
#if FLUB_HAVE_X11_HEADERS
    const bool expectUnsupported = std::getenv ("DISPLAY") == nullptr || isWaylandSession();
#else
    const bool expectUnsupported = true; // built without the X11 headers
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

#endif // __linux__
