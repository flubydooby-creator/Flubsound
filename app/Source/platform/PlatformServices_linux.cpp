// Flubsound Pro - Linux implementation of PlatformServices.h
//
// Linux audio model (see platform/linux/README.md): every strip owns a
// PipeWire/PulseAudio null sink ("Flubsound Game", "Flubsound Music", ...).
// Applications are routed by moving their sink-inputs to those sinks; the
// engine reads each sink's monitor, processes it and plays the result on the
// real device. Per-process loopback capture is therefore not needed.
//
// Dependencies: flub_core (flub::json) and the C library only. Routing shells
// out to `pactl` (pulseaudio-utils >= 16 or pipewire-pulse's pactl), which
// works identically against PulseAudio and PipeWire's pulse server. Global
// hotkeys use the X11 headers when present at build time and load libX11
// with dlopen at run time (no link dependency); in Wayland sessions they go
// through the xdg-desktop-portal GlobalShortcuts interface over D-Bus, with
// libdbus-1 loaded the same way (no headers, no link dependency).
//
// Start with the OS: an XDG autostart entry (Desktop Application Autostart
// Specification), honoured by GNOME, KDE Plasma, Xfce, Cinnamon, MATE and
// LXQt; bare window managers need a helper such as dex.
//
// Threading: AppAudioRouter calls block while pactl runs (typically 5-30 ms);
// call them from a background thread if that matters. SystemTuning must be
// called on the thread it tunes. GlobalHotkeys callbacks run on the
// service's own X event / D-Bus thread, not on the thread that created it.
// AutoStart does
// small blocking file IO: message thread, on user action.
#if defined(__linux__)

#include "PlatformServices.h"
#include "PlatformServicesInternal.h"

#include "flub/io/Json.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

// X11 global hotkeys (see LinuxGlobalHotkeys): headers only, libX11 itself is
// loaded at run time.
// FLUB_NO_X11 forces the header-less build (no global hotkeys) for testing.
#if ! defined(FLUB_NO_X11) && __has_include(<X11/Xlib.h>) && __has_include(<X11/keysym.h>)
    #define FLUB_HAVE_X11_HEADERS 1
    #include <X11/Xlib.h>
    #include <X11/keysym.h>
    // X.h defines None as a macro, which would break KeyChord::None (used
    // below and by tests/test_platform_linux.cpp, which includes this file).
    #undef None
#else
    #define FLUB_HAVE_X11_HEADERS 0
#endif

namespace flub::platform
{
namespace
{
//==============================================================================
// pactl helpers (unnamed namespace; tests reach them by including this file)
//==============================================================================
namespace pactl
{
/** Upper bound for captured command output (a list of a few hundred streams
    is ~1 MB of JSON); protects against a runaway child. */
constexpr size_t kMaxOutputBytes = 16u * 1024u * 1024u;

/** Sink names/indices we are willing to put on a shell command line.
    Whitelist only - PulseAudio/PipeWire node names use [A-Za-z0-9_.:@+-]
    (e.g. "alsa_output.pci-0000_00_1f.3.analog-stereo", "@DEFAULT_SINK@",
    "bluez_output.AA_BB_CC_DD_EE_FF.1"). A leading '-' is rejected so the
    value can never be parsed by pactl as an option. */
bool isSafeToken (const std::string& token)
{
    if (token.empty() || token.size() > 255 || token.front() == '-')
        return false;

    return std::all_of (token.begin(),
                        token.end(),
                        [] (char c)
                        {
                            const bool alphanumeric = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
                            return alphanumeric || c == '_' || c == '.' || c == ':' || c == '@' || c == '+' || c == '-';
                        });
}

/** Belt and braces on top of isSafeToken(): single-quote for /bin/sh. The
    whitelist already excludes the quote character itself. */
std::string shellQuote (const std::string& safeToken) { return "'" + safeToken + "'"; }

struct CommandResult
{
    int exitCode = -1; // -1: could not run / killed by a signal
    std::string output;
};

/** Runs a fixed, fully sanitised command line through popen. Never pass
    unsanitised input here: every variable part must have gone through
    isSafeToken() + shellQuote() or be a formatted integer. */
CommandResult run (const std::string& commandLine)
{
    CommandResult result;

    // "e" = O_CLOEXEC on the pipe, so other children never inherit it.
    FILE* pipe = ::popen (commandLine.c_str(), "re");
    if (pipe == nullptr)
        return result;

    char buffer[4096];
    size_t bytes = 0;
    while ((bytes = std::fread (buffer, 1, sizeof (buffer), pipe)) > 0)
    {
        if (result.output.size() + bytes <= kMaxOutputBytes)
            result.output.append (buffer, bytes);
    }

    const int status = ::pclose (pipe);
    if (status != -1 && WIFEXITED (status))
        result.exitCode = WEXITSTATUS (status);

    return result;
}

/** Reads an unsigned id that pactl prints either as a JSON number or as a
    string property ("application.process.id": "1234"). */
bool toUInt32 (const json::Value& value, uint32_t& out)
{
    if (value.isNumber())
    {
        const double d = value.asNumber();
        if (! (d >= 0.0 && d <= 4294967295.0) || std::floor (d) != d)
            return false;
        out = static_cast<uint32_t> (d);
        return true;
    }

    if (value.isString())
    {
        const auto& s = value.asString();
        if (s.empty() || s.size() > 10 || ! std::all_of (s.begin(), s.end(), [] (char c) { return c >= '0' && c <= '9'; }))
            return false;
        const unsigned long long v = std::strtoull (s.c_str(), nullptr, 10);
        if (v > 4294967295ull)
            return false;
        out = static_cast<uint32_t> (v);
        return true;
    }

    return false;
}

struct SinkInput
{
    uint32_t index = 0;
    uint32_t sinkIndex = 0;
    uint32_t processId = 0; // 0 = unknown
    bool corked = true;
    std::string binary;
    std::string applicationName;
};

/** Parses `pactl --format=json list sinks` into index -> sink name. */
bool parseSinks (const std::string& text, std::map<uint32_t, std::string>& sinkNames, std::string& error)
{
    json::Value root;
    if (! json::parse (text, root, error))
    {
        error = "Unexpected pactl output (sinks): " + error;
        return false;
    }
    if (! root.isArray())
    {
        error = "Unexpected pactl output (sinks): not a JSON array";
        return false;
    }

    for (const auto& sink : root.asArray())
    {
        uint32_t index = 0;
        if (toUInt32 (sink["index"], index) && sink["name"].isString())
            sinkNames[index] = sink["name"].asString();
    }
    return true;
}

/** Parses `pactl --format=json list sink-inputs`. */
bool parseSinkInputs (const std::string& text, std::vector<SinkInput>& inputs, std::string& error)
{
    json::Value root;
    if (! json::parse (text, root, error))
    {
        error = "Unexpected pactl output (sink-inputs): " + error;
        return false;
    }
    if (! root.isArray())
    {
        error = "Unexpected pactl output (sink-inputs): not a JSON array";
        return false;
    }

    for (const auto& item : root.asArray())
    {
        SinkInput input;
        if (! toUInt32 (item["index"], input.index))
            continue;

        toUInt32 (item["sink"], input.sinkIndex);
        input.corked = item["corked"].asBool (false);

        // Prefer PipeWire's socket-credential pid: the kernel reports it in
        // the sound server's (= our) pid namespace. application.process.id is
        // the client's own getpid(), which for Flatpak/Snap/container apps is
        // a sandbox-local pid (often 2 or 3) - it would collide between
        // sandboxes and name an unrelated host process. Classic PulseAudio
        // only has application.process.id.
        const auto& props = item["properties"];
        if (! toUInt32 (props["pipewire.sec.pid"], input.processId) || input.processId == 0)
        {
            input.processId = 0;
            toUInt32 (props["application.process.id"], input.processId);
        }

        input.binary = props["application.process.binary"].asString();
        input.applicationName = props["application.name"].asString();
        inputs.push_back (std::move (input));
    }
    return true;
}

/** One AudioSessionInfo per process (a process may own several streams; a
    playing one wins). Streams without a pid cannot be addressed through the
    pid-based interface and are skipped, as are our own streams. */
std::vector<AudioSessionInfo> toSessions (const std::vector<SinkInput>& inputs,
                                          const std::map<uint32_t, std::string>& sinkNames,
                                          uint32_t ownPid)
{
    std::vector<AudioSessionInfo> sessions;
    std::map<uint32_t, size_t> indexByPid;

    const auto sinkName = [&sinkNames] (uint32_t index)
    {
        const auto it = sinkNames.find (index);
        return it != sinkNames.end() ? it->second : std::to_string (index);
    };

    for (const auto& input : inputs)
    {
        if (input.processId == 0 || input.processId == ownPid)
            continue;

        const bool active = ! input.corked;

        if (const auto it = indexByPid.find (input.processId); it != indexByPid.end())
        {
            auto& existing = sessions[it->second];
            if (active && ! existing.isActive)
            {
                existing.isActive = true;
                existing.currentEndpointId = sinkName (input.sinkIndex);
            }
            continue;
        }

        AudioSessionInfo info;
        info.processId = input.processId;
        info.executableName = input.binary;
        info.displayName = ! input.applicationName.empty() ? input.applicationName : input.binary;
        info.currentEndpointId = sinkName (input.sinkIndex);
        info.isActive = active;

        indexByPid.emplace (input.processId, sessions.size());
        sessions.push_back (std::move (info));
    }

    return sessions;
}

/** First line of pactl's diagnostic output, for error messages. */
std::string firstLine (const std::string& text)
{
    auto line = text.substr (0, text.find ('\n'));
    if (line.size() > 200)
        line.resize (200);
    return line;
}

bool isExecutableInPath (const char* name)
{
    const char* path = std::getenv ("PATH");
    std::string dirs = path != nullptr ? path : "/usr/local/bin:/usr/bin:/bin";

    size_t start = 0;
    while (start <= dirs.size())
    {
        const size_t end = std::min (dirs.find (':', start), dirs.size());
        const std::string dir = dirs.substr (start, end - start);
        if (! dir.empty() && ::access ((dir + "/" + name).c_str(), X_OK) == 0)
            return true;
        start = end + 1;
    }
    return false;
}
} // namespace pactl

//==============================================================================
// GlobalHotkeys - X11 key grabs (XGrabKey on the root window)
//==============================================================================
/*  There is no single global-shortcut API on Linux:
      - X11 (implemented here): XGrabKey on the root window for every chord x
        {none, CapsLock, NumLock, both} so the lock keys do not defeat the
        shortcut. libX11 is loaded at run time (as JUCE itself does), so the
        app and the unit tests build and run without it; the grabs use a
        private Display connection served by one event thread, which calls
        the callbacks (HotkeyManager hops them to the message thread).
        A chord another client already grabbed fails with BadAccess, which is
        caught with a temporary error handler around an XSync and reported
        as "in use" (registerHotkey returns false).
      - Wayland: grabbing keys is forbidden by design (an X grab through
        XWayland only sees keys while an XWayland window has focus, so it is
        not global). The sanctioned route is the xdg-desktop-portal
        GlobalShortcuts interface (KDE Plasma 5.27+, GNOME 48+, Hyprland),
        implemented by PortalGlobalHotkeys below; GlobalHotkeys::create()
        picks it in a Wayland session. Without the portal isSupported() ==
        false and the UI asks users to bind the actions in their desktop's
        keyboard settings instead. */

/** True in a Wayland session, where X key grabs are not global. */
bool isWaylandSession()
{
    const char* type = std::getenv ("XDG_SESSION_TYPE");
    if (type != nullptr && std::string (type) == "wayland")
        return true;
    const char* wayland = std::getenv ("WAYLAND_DISPLAY");
    return wayland != nullptr && *wayland != '\0';
}

#if FLUB_HAVE_X11_HEADERS
/** The libX11 entry points the hotkey service needs, resolved with dlopen. */
struct X11Api
{
    void* lib = nullptr;
    Display* (*openDisplay) (const char*) = nullptr;
    int (*closeDisplay) (Display*) = nullptr;
    Window (*defaultRootWindow) (Display*) = nullptr;
    KeyCode (*keysymToKeycode) (Display*, KeySym) = nullptr;
    int (*grabKey) (Display*, int, unsigned int, Window, Bool, int, int) = nullptr;
    int (*ungrabKey) (Display*, int, unsigned int, Window) = nullptr;
    int (*selectInput) (Display*, Window, long) = nullptr;
    int (*pending) (Display*) = nullptr;
    int (*nextEvent) (Display*, XEvent*) = nullptr;
    int (*sync) (Display*, Bool) = nullptr;
    int (*flush) (Display*) = nullptr;
    int (*connectionNumber) (Display*) = nullptr;
    XErrorHandler (*setErrorHandler) (XErrorHandler) = nullptr;
    Bool (*setDetectableAutoRepeat) (Display*, Bool, Bool*) = nullptr; // Xkb, optional

    static const X11Api* get()
    {
        static const X11Api api = []
        {
            X11Api a;
            a.lib = ::dlopen ("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
            if (a.lib == nullptr)
                return a;
            const auto sym = [&a] (auto& fn, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype (fn)>> (::dlsym (a.lib, name)); return fn != nullptr; };
            const bool ok = sym (a.openDisplay, "XOpenDisplay") && sym (a.closeDisplay, "XCloseDisplay")
                            && sym (a.defaultRootWindow, "XDefaultRootWindow") && sym (a.keysymToKeycode, "XKeysymToKeycode")
                            && sym (a.grabKey, "XGrabKey") && sym (a.ungrabKey, "XUngrabKey") && sym (a.selectInput, "XSelectInput")
                            && sym (a.pending, "XPending") && sym (a.nextEvent, "XNextEvent") && sym (a.sync, "XSync")
                            && sym (a.flush, "XFlush") && sym (a.connectionNumber, "XConnectionNumber")
                            && sym (a.setErrorHandler, "XSetErrorHandler");
            sym (a.setDetectableAutoRepeat, "XkbSetDetectableAutoRepeat");
            if (! ok)
            {
                ::dlclose (a.lib);
                a = X11Api();
            }
            return a;
        }();
        return api.lib != nullptr ? &api : nullptr;
    }
};

/** KeyChord key code (ASCII upper-case letter / digit, F1..F24 as 0x70 + n,
    VK-style navigation keys) to an X keysym; 0 if unmappable. */
KeySym keysymForChord (uint32_t keyCode)
{
    if (keyCode >= 'A' && keyCode <= 'Z')
        return static_cast<KeySym> (XK_a + (keyCode - 'A'));
    if (keyCode >= '0' && keyCode <= '9')
        return static_cast<KeySym> (XK_0 + (keyCode - '0'));
    if (keyCode >= 0x70 && keyCode < 0x70 + 24)
        return static_cast<KeySym> (XK_F1 + (keyCode - 0x70));
    switch (keyCode)
    {
        case 0x20: return XK_space;
        case 0x21: return XK_Prior;
        case 0x22: return XK_Next;
        case 0x23: return XK_End;
        case 0x24: return XK_Home;
        case 0x25: return XK_Left;
        case 0x26: return XK_Up;
        case 0x27: return XK_Right;
        case 0x28: return XK_Down;
        case 0x2D: return XK_Insert;
        case 0x2E: return XK_Delete;
        default: break;
    }
    return 0;
}

unsigned int x11Modifiers (uint32_t modifiers)
{
    unsigned int m = 0;
    if ((modifiers & KeyChord::Ctrl) != 0) m |= ControlMask;
    if ((modifiers & KeyChord::Alt) != 0) m |= Mod1Mask;
    if ((modifiers & KeyChord::Shift) != 0) m |= ShiftMask;
    if ((modifiers & KeyChord::Super) != 0) m |= Mod4Mask;
    return m;
}

// Lock-key variants grabbed for every chord (CapsLock = LockMask, NumLock =
// Mod2Mask on practically every keymap).
constexpr unsigned int kLockVariants[] = { 0u, LockMask, Mod2Mask, LockMask | Mod2Mask };
constexpr unsigned int kChordModifierMask = ControlMask | Mod1Mask | ShiftMask | Mod4Mask;

std::atomic<Display*> grabErrorDisplay { nullptr };
std::atomic<bool> grabFailed { false };
XErrorHandler previousErrorHandler = nullptr;

int onGrabError (Display* display, XErrorEvent* event)
{
    if (display == grabErrorDisplay.load())
    {
        if (event->error_code == BadAccess)
            grabFailed = true;
        return 0;
    }
    return previousErrorHandler != nullptr ? previousErrorHandler (display, event) : 0;
}

class LinuxGlobalHotkeys final : public GlobalHotkeys
{
public:
    LinuxGlobalHotkeys()
    {
        api = X11Api::get();
        const char* displayName = std::getenv ("DISPLAY");
        if (api == nullptr || isWaylandSession() || displayName == nullptr || *displayName == '\0')
            return;
        display = api->openDisplay (nullptr);
        if (display == nullptr)
            return;
        root = api->defaultRootWindow (display);
        api->selectInput (display, root, KeyPressMask | KeyReleaseMask);
        if (api->setDetectableAutoRepeat != nullptr)
        {
            Bool supported = False;
            api->setDetectableAutoRepeat (display, True, &supported); // held keys: one press, not a stream
        }
        if (::pipe2 (wakePipe, O_CLOEXEC | O_NONBLOCK) != 0)
        {
            api->closeDisplay (display);
            display = nullptr;
            return;
        }
        running = true;
        thread = std::thread ([this] { eventLoop(); });
    }

    ~LinuxGlobalHotkeys() override
    {
        unregisterAll();
        if (thread.joinable())
        {
            running = false;
            const char byte = 0;
            [[maybe_unused]] const auto written = ::write (wakePipe[1], &byte, 1);
            thread.join();
        }
        if (display != nullptr)
        {
            api->closeDisplay (display);
            ::close (wakePipe[0]);
            ::close (wakePipe[1]);
        }
    }

    bool isSupported() const override { return display != nullptr; }

    bool registerHotkey (int id, const KeyChord& chord, std::function<void()> callback) override
    {
        if (display == nullptr || ! callback)
            return false;
        const KeySym keysym = keysymForChord (chord.keyCode);
        const unsigned int mods = x11Modifiers (chord.modifiers);
        if (! callback || ! detail::isValidChord (chord) || keysym == 0
            || mods == 0) // bare keys (even F-keys) would steal normal typing under X
            return false;
        unregisterHotkey (id);

        std::lock_guard<std::mutex> guard (mutex);
        const KeyCode keycode = api->keysymToKeycode (display, keysym);
        if (keycode == 0)
            return false;

        // Grab synchronously so a BadAccess (someone else owns the chord) is
        // seen here. The error handler is process-wide, so it is only swapped
        // in for this call (on the message thread, like JUCE's own X calls).
        grabErrorDisplay = display;
        grabFailed = false;
        previousErrorHandler = api->setErrorHandler (&onGrabError);
        for (const unsigned int lock : kLockVariants)
            api->grabKey (display, keycode, mods | lock, root, False, GrabModeAsync, GrabModeAsync);
        api->sync (display, False);
        const bool failed = grabFailed;
        if (failed)
        {
            for (const unsigned int lock : kLockVariants)
                api->ungrabKey (display, keycode, mods | lock, root);
            api->sync (display, False);
        }
        api->setErrorHandler (previousErrorHandler);
        grabErrorDisplay = nullptr;
        if (failed)
            return false;

        bindings[id] = Binding { keycode, mods, std::move (callback), false };
        return true;
    }

    void unregisterHotkey (int id) override
    {
        std::lock_guard<std::mutex> guard (mutex);
        const auto it = bindings.find (id);
        if (it == bindings.end())
            return;
        for (const unsigned int lockMask : kLockVariants)
            api->ungrabKey (display, it->second.keycode, it->second.modifiers | lockMask, root);
        api->flush (display);
        bindings.erase (it);
    }

    void unregisterAll() override
    {
        std::vector<int> ids;
        {
            std::lock_guard<std::mutex> guard (mutex);
            for (const auto& b : bindings)
                ids.push_back (b.first);
        }
        for (const int id : ids)
            unregisterHotkey (id);
    }

private:
    struct Binding
    {
        KeyCode keycode = 0;
        unsigned int modifiers = 0;
        std::function<void()> callback;
        bool down = false; // suppresses key repeat until the release
    };

    void eventLoop()
    {
        pollfd fds[2] = { { api->connectionNumber (display), POLLIN, 0 }, { wakePipe[0], POLLIN, 0 } };
        while (running)
        {
            std::vector<std::function<void()>> fire;
            {
                std::lock_guard<std::mutex> guard (mutex);
                while (api->pending (display) > 0)
                {
                    XEvent event;
                    api->nextEvent (display, &event);
                    if (event.type != KeyPress && event.type != KeyRelease)
                        continue;
                    const auto& key = event.xkey;
                    const unsigned int mods = key.state & kChordModifierMask;
                    for (auto& [id, b] : bindings)
                    {
                        if (b.keycode != key.keycode || b.modifiers != mods)
                            continue;
                        if (event.type == KeyPress && ! b.down)
                            fire.push_back (b.callback);
                        b.down = event.type == KeyPress;
                    }
                }
            }
            for (auto& f : fire) // outside the lock: a callback may (un)register
                f();
            ::poll (fds, 2, -1);
            if ((fds[1].revents & POLLIN) != 0)
            {
                char buffer[16];
                while (::read (wakePipe[0], buffer, sizeof (buffer)) > 0) {}
            }
        }
    }

    const X11Api* api = nullptr;
    Display* display = nullptr;
    Window root = 0;
    int wakePipe[2] = { -1, -1 };
    std::atomic<bool> running { false };
    std::thread thread;
    std::mutex mutex;
    std::map<int, Binding> bindings;
};
#else
class LinuxGlobalHotkeys final : public GlobalHotkeys
{
public:
    bool isSupported() const override { return false; } // built without X11 headers
    bool registerHotkey (int, const KeyChord&, std::function<void()>) override { return false; }
    void unregisterHotkey (int) override {}
    void unregisterAll() override {}
};
#endif

//==============================================================================
// GlobalHotkeys - xdg-desktop-portal GlobalShortcuts (Wayland sessions)
//==============================================================================
/*  Protocol (org.freedesktop.portal.GlobalShortcuts, version 1, served by
    org.freedesktop.portal.Desktop at /org/freedesktop/portal/desktop):
      1. CreateSession ({handle_token, session_handle_token}) returns a
         Request object path; the result arrives later as that Request's
         Response signal (u response, a{sv} results) with
         results["session_handle"].
      2. BindShortcuts (session, [(id, {description, preferred_trigger})],
         parent_window, {handle_token}) answers the same way with
         results["shortcuts"]: the shortcuts actually bound, each as
         (id, {description, trigger_description}). The compositor may show a
         dialog; the user may pick another key or decline, so
         preferred_trigger is only a hint.
      3. Activated / Deactivated (session, shortcut_id, timestamp, options)
         signals while the session lives.
    Response codes: 0 success, 1 cancelled by the user, 2 other failure.
    Request paths are predictable (.../request/<sender>/<handle_token>), so
    the Response is matched even when it overtakes the method reply.

    Rebinding: the interface has no "unbind", and binding a session's
    shortcuts is a one-time step (a second BindShortcuts on the same session
    is not portable across portal back-ends). A changed set is therefore
    applied by closing the session (Session.Close) and binding the whole new
    set in a fresh one. Desktops remember the user's choice per application
    and shortcut id, so re-binding known ids does not ask again. Changes are
    coalesced: HotkeyManager::registerAll() unregisters everything and then
    registers each action in a burst; that becomes one new session
    (settleTime after the last change), and a burst that ends with the set
    already bound is not sent at all. ListShortcuts is not used: the
    BindShortcuts response already lists what was bound.

    Results are asynchronous, so registerHotkey() returns true when the chord
    can be requested (valid chord with a trigger name, portal present). A
    denied dialog, shortcuts the desktop did not bind and portal errors are
    logged to stderr: the GlobalHotkeys interface has no channel back to
    HotkeyManager for failures found after registerHotkey() returned.

    Threading: one private connection to the session bus, serviced by the
    service's own thread (poll on the bus fd + a wake pipe, like the X11
    class). After the constructor all D-Bus traffic happens on that thread;
    registerHotkey / unregister* only edit the wanted set under a mutex and
    wake it. Callbacks run on that thread, once per Activated signal (the
    Deactivated signal, key release, is ignored). Signals count only when
    they come from the portal's unique bus name and name the current
    session. */
namespace dbus
{
/*  libdbus-1 is declared here from its stable C ABI instead of including
    <dbus/dbus.h>. The headers ship in libdbus-1-dev, which most build
    machines (CI, this project's build hosts) do not have; a
    __has_include(<dbus/dbus.h>) switch with a fallback would give two
    declaration paths, one of them never compiled or tested. libdbus-1.so.3
    has kept its ABI since 1.0 (2006) and the D-Bus project guarantees it, so
    these prototypes, the type codes and the two structs the caller
    allocates are fixed:
      - DBusError { const char* name; const char* message; unsigned dummy
        bit-fields; void* padding1; }: declared field for field, plus spare
        room;
      - DBusMessageIter: opaque, 72 bytes on LP64, only ever passed by
        pointer: over-allocated here (libdbus writes at most its real size). */
struct Connection;
struct Message;
using Boolean = uint32_t; // dbus_bool_t ("Bool" is an X11 macro)

struct Error
{
    const char* name = nullptr;
    const char* message = nullptr;
    unsigned int dummyBits = 0;
    void* padding1 = nullptr;
    void* spare[4] {};
};

struct Iter
{
    void* opaque[16] {};
};

constexpr int kTypeUInt32 = 'u';
constexpr int kTypeString = 's';
constexpr int kTypeObjectPath = 'o';
constexpr int kTypeVariant = 'v';
constexpr int kTypeArray = 'a';
constexpr int kTypeStruct = 'r';
constexpr int kTypeDictEntry = 'e';

constexpr int kMessageMethodReturn = 2;
constexpr int kMessageError = 3;
constexpr int kMessageSignal = 4;

constexpr int kDispatchDataRemains = 0;

/** The libdbus-1 entry points the portal client needs, resolved with dlopen. */
struct Api
{
    void* lib = nullptr;
    void (*errorInit) (Error*) = nullptr;
    void (*errorFree) (Error*) = nullptr;
    Connection* (*connectionOpenPrivate) (const char*, Error*) = nullptr;
    Boolean (*busRegister) (Connection*, Error*) = nullptr;
    const char* (*busGetUniqueName) (Connection*) = nullptr;
    void (*busAddMatch) (Connection*, const char*, Error*) = nullptr;
    void (*connectionSetExitOnDisconnect) (Connection*, Boolean) = nullptr;
    void (*connectionClose) (Connection*) = nullptr;
    void (*connectionUnref) (Connection*) = nullptr;
    Boolean (*connectionGetUnixFd) (Connection*, int*) = nullptr;
    Boolean (*connectionReadWrite) (Connection*, int) = nullptr;
    int (*connectionGetDispatchStatus) (Connection*) = nullptr;
    Message* (*connectionPopMessage) (Connection*) = nullptr;
    Boolean (*connectionSend) (Connection*, Message*, uint32_t*) = nullptr;
    void (*connectionFlush) (Connection*) = nullptr;
    Boolean (*connectionHasMessagesToSend) (Connection*) = nullptr;
    Message* (*sendWithReplyAndBlock) (Connection*, Message*, int, Error*) = nullptr;
    Message* (*messageNewMethodCall) (const char*, const char*, const char*, const char*) = nullptr;
    void (*messageUnref) (Message*) = nullptr;
    int (*messageGetType) (Message*) = nullptr;
    const char* (*messageGetPath) (Message*) = nullptr;
    const char* (*messageGetInterface) (Message*) = nullptr;
    const char* (*messageGetMember) (Message*) = nullptr;
    const char* (*messageGetSender) (Message*) = nullptr;
    const char* (*messageGetErrorName) (Message*) = nullptr;
    uint32_t (*messageGetReplySerial) (Message*) = nullptr;
    void (*messageSetNoReply) (Message*, Boolean) = nullptr;
    Boolean (*iterInit) (Message*, Iter*) = nullptr;
    void (*iterInitAppend) (Message*, Iter*) = nullptr;
    int (*iterGetArgType) (Iter*) = nullptr;
    Boolean (*iterNext) (Iter*) = nullptr;
    void (*iterRecurse) (Iter*, Iter*) = nullptr;
    void (*iterGetBasic) (Iter*, void*) = nullptr;
    Boolean (*iterAppendBasic) (Iter*, int, const void*) = nullptr;
    Boolean (*iterOpenContainer) (Iter*, int, const char*, Iter*) = nullptr;
    Boolean (*iterCloseContainer) (Iter*, Iter*) = nullptr;

    static const Api* get()
    {
        static const Api api = []
        {
            Api a;
            a.lib = ::dlopen ("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
            if (a.lib == nullptr)
                return a;
            const auto sym = [&a] (auto& fn, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype (fn)>> (::dlsym (a.lib, name)); return fn != nullptr; };
            Boolean (*threadsInitDefault)() = nullptr;
            const bool ok = sym (threadsInitDefault, "dbus_threads_init_default") && sym (a.errorInit, "dbus_error_init")
                            && sym (a.errorFree, "dbus_error_free") && sym (a.connectionOpenPrivate, "dbus_connection_open_private")
                            && sym (a.busRegister, "dbus_bus_register") && sym (a.busGetUniqueName, "dbus_bus_get_unique_name")
                            && sym (a.busAddMatch, "dbus_bus_add_match")
                            && sym (a.connectionSetExitOnDisconnect, "dbus_connection_set_exit_on_disconnect")
                            && sym (a.connectionClose, "dbus_connection_close") && sym (a.connectionUnref, "dbus_connection_unref")
                            && sym (a.connectionGetUnixFd, "dbus_connection_get_unix_fd")
                            && sym (a.connectionReadWrite, "dbus_connection_read_write")
                            && sym (a.connectionGetDispatchStatus, "dbus_connection_get_dispatch_status")
                            && sym (a.connectionPopMessage, "dbus_connection_pop_message")
                            && sym (a.connectionSend, "dbus_connection_send") && sym (a.connectionFlush, "dbus_connection_flush")
                            && sym (a.connectionHasMessagesToSend, "dbus_connection_has_messages_to_send")
                            && sym (a.sendWithReplyAndBlock, "dbus_connection_send_with_reply_and_block")
                            && sym (a.messageNewMethodCall, "dbus_message_new_method_call") && sym (a.messageUnref, "dbus_message_unref")
                            && sym (a.messageGetType, "dbus_message_get_type") && sym (a.messageGetPath, "dbus_message_get_path")
                            && sym (a.messageGetInterface, "dbus_message_get_interface")
                            && sym (a.messageGetMember, "dbus_message_get_member") && sym (a.messageGetSender, "dbus_message_get_sender")
                            && sym (a.messageGetErrorName, "dbus_message_get_error_name")
                            && sym (a.messageGetReplySerial, "dbus_message_get_reply_serial")
                            && sym (a.messageSetNoReply, "dbus_message_set_no_reply") && sym (a.iterInit, "dbus_message_iter_init")
                            && sym (a.iterInitAppend, "dbus_message_iter_init_append")
                            && sym (a.iterGetArgType, "dbus_message_iter_get_arg_type") && sym (a.iterNext, "dbus_message_iter_next")
                            && sym (a.iterRecurse, "dbus_message_iter_recurse") && sym (a.iterGetBasic, "dbus_message_iter_get_basic")
                            && sym (a.iterAppendBasic, "dbus_message_iter_append_basic")
                            && sym (a.iterOpenContainer, "dbus_message_iter_open_container")
                            && sym (a.iterCloseContainer, "dbus_message_iter_close_container");
            // Several threads use libdbus (this service, and any other
            // libdbus user in the process); must precede every other call.
            if (! ok || threadsInitDefault() == 0)
            {
                ::dlclose (a.lib);
                a = Api();
            }
            return a;
        }();
        return api.lib != nullptr ? &api : nullptr;
    }
};

struct MessageUnref
{
    void operator() (Message* message) const { Api::get()->messageUnref (message); }
};
using MessageRef = std::unique_ptr<Message, MessageUnref>;

inline std::string str (const char* text) { return text != nullptr ? std::string (text) : std::string(); }

/** Appends a string-like basic value ('s' or 'o'). libdbus aborts the
    process on an invalid UTF-8 string or object path, so callers only pass
    ASCII built here or paths checked with portal::isValidObjectPath. */
inline bool appendBasic (const Api& api, Iter* iter, int type, const std::string& value)
{
    const char* text = value.c_str();
    return api.iterAppendBasic (iter, type, &text) != 0;
}

/** Appends {key: variant<type>(value)} to an a{sv} under construction. */
inline bool appendDictEntry (const Api& api, Iter* dict, const char* key, int type, const std::string& value)
{
    Iter entry, variant;
    const char signature[2] = { static_cast<char> (type), '\0' };
    return api.iterOpenContainer (dict, kTypeDictEntry, nullptr, &entry) != 0 && appendBasic (api, &entry, kTypeString, key)
           && api.iterOpenContainer (&entry, kTypeVariant, signature, &variant) != 0 && appendBasic (api, &variant, type, value)
           && api.iterCloseContainer (&entry, &variant) != 0 && api.iterCloseContainer (dict, &entry) != 0;
}

/** Reads an 's' or 'o' argument; false for any other type. */
inline bool readString (const Api& api, Iter* iter, std::string& out)
{
    const int type = api.iterGetArgType (iter);
    if (type != kTypeString && type != kTypeObjectPath)
        return false;
    const char* text = nullptr;
    api.iterGetBasic (iter, &text);
    out = str (text);
    return true;
}

/** Finds 'key' in the a{sv} at 'dict' and points 'value' into its variant. */
inline bool findInVardict (const Api& api, Iter* dict, const char* key, Iter& value)
{
    if (api.iterGetArgType (dict) != kTypeArray)
        return false;
    Iter entries;
    api.iterRecurse (dict, &entries);
    for (; api.iterGetArgType (&entries) == kTypeDictEntry; api.iterNext (&entries))
    {
        Iter entry;
        api.iterRecurse (&entries, &entry);
        std::string name;
        if (readString (api, &entry, name) && name == key && api.iterNext (&entry) != 0 && api.iterGetArgType (&entry) == kTypeVariant)
        {
            api.iterRecurse (&entry, &value);
            return true;
        }
    }
    return false;
}

inline bool readVardictString (const Api& api, Iter* dict, const char* key, std::string& out)
{
    Iter value;
    return findInVardict (api, dict, key, value) && readString (api, &value, out);
}
} // namespace dbus

namespace portal
{
constexpr const char* kService = "org.freedesktop.portal.Desktop";
constexpr const char* kObjectPath = "/org/freedesktop/portal/desktop";
constexpr const char* kShortcutsInterface = "org.freedesktop.portal.GlobalShortcuts";
constexpr const char* kRequestInterface = "org.freedesktop.portal.Request";
constexpr const char* kSessionInterface = "org.freedesktop.portal.Session";

// Broadcast signals only reach a connection with a matching rule. The portal
// sends Response / Activated to its client directly, but the rules keep
// portal back-ends that broadcast working too.
constexpr const char* kMatchRules[] = {
    "type='signal',interface='org.freedesktop.portal.Request',member='Response'",
    "type='signal',interface='org.freedesktop.portal.GlobalShortcuts'",
    "type='signal',interface='org.freedesktop.portal.Session',member='Closed'",
    "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.freedesktop.portal.Desktop'",
};

/** A KeyChord as a trigger in the XDG shortcuts spec format: the modifiers
    CTRL, ALT, SHIFT and LOGO, then the xkb keysym name of the unshifted key,
    joined with '+' ("CTRL+ALT+Up", "CTRL+SHIFT+m", "LOGO+F13"). "" when the
    key has no name. */
std::string triggerFor (const KeyChord& chord)
{
    std::string key;
    if (detail::isLetterKey (chord.keyCode))
        key = std::string (1, static_cast<char> ('a' + (chord.keyCode - 'A')));
    else if (detail::isDigitKey (chord.keyCode))
        key = std::string (1, static_cast<char> (chord.keyCode));
    else if (const int f = detail::functionKeyNumber (chord.keyCode); f > 0)
        key = "F" + std::to_string (f);
    else
    {
        switch (chord.keyCode)
        {
            case 0x20: key = "space"; break;
            case 0x21: key = "Page_Up"; break;
            case 0x22: key = "Page_Down"; break;
            case 0x23: key = "End"; break;
            case 0x24: key = "Home"; break;
            case 0x25: key = "Left"; break;
            case 0x26: key = "Up"; break;
            case 0x27: key = "Right"; break;
            case 0x28: key = "Down"; break;
            case 0x2D: key = "Insert"; break;
            case 0x2E: key = "Delete"; break;
            default: return {};
        }
    }

    std::string trigger;
    if ((chord.modifiers & KeyChord::Ctrl) != 0) trigger += "CTRL+";
    if ((chord.modifiers & KeyChord::Alt) != 0) trigger += "ALT+";
    if ((chord.modifiers & KeyChord::Shift) != 0) trigger += "SHIFT+";
    if ((chord.modifiers & KeyChord::Super) != 0) trigger += "LOGO+";
    return trigger + key;
}

/** Shortcut ids are what desktops store the user's choice under. */
std::string shortcutId (int id) { return "flubsound-" + std::to_string (id); }

/** D-Bus object path syntax: "/" or "/a/b_1" (elements of [A-Za-z0-9_]). */
bool isValidObjectPath (const std::string& path)
{
    if (path.empty() || path.front() != '/')
        return false;
    if (path.size() == 1)
        return true;
    if (path.back() == '/')
        return false;
    for (size_t i = 1; i < path.size(); ++i)
    {
        const char c = path[i];
        const bool element = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (! element && ! (c == '/' && path[i - 1] != '/'))
            return false;
    }
    return true;
}

/** Request / session object path element for a unique bus name: ":1.42"
    becomes "1_42". */
std::string busPathElement (const std::string& uniqueName)
{
    std::string element = uniqueName.substr (uniqueName.empty() || uniqueName.front() != ':' ? 0 : 1);
    std::replace (element.begin(), element.end(), '.', '_');
    return element;
}

/** The session bus address: $DBUS_SESSION_BUS_ADDRESS, else the systemd
    user bus socket $XDG_RUNTIME_DIR/bus. libdbus's own fallback would also
    try X11 autolaunch, which can start a stray bus daemon; that is avoided. */
std::string sessionBusAddress()
{
    if (const char* address = std::getenv ("DBUS_SESSION_BUS_ADDRESS"); address != nullptr && *address != '\0')
        return address;

    const char* runtimeDir = std::getenv ("XDG_RUNTIME_DIR");
    if (runtimeDir == nullptr || runtimeDir[0] != '/')
        return {};
    const std::string socketPath = std::string (runtimeDir) + "/bus";
    struct stat info {};
    if (::stat (socketPath.c_str(), &info) != 0 || ! S_ISSOCK (info.st_mode))
        return {};

    // D-Bus address values escape every byte outside [-0-9A-Za-z_/.\*] as %XX.
    std::string address = "unix:path=";
    for (const char c : socketPath)
    {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'
                           || c == '/' || c == '.' || c == '\\' || c == '*';
        if (plain)
            address += c;
        else
        {
            char escaped[4];
            std::snprintf (escaped, sizeof (escaped), "%%%02X", static_cast<unsigned int> (static_cast<unsigned char> (c)));
            address += escaped;
        }
    }
    return address;
}

void log (const std::string& text) { std::fprintf (stderr, "Flubsound: global shortcuts: %s\n", text.c_str()); }
} // namespace portal

class PortalGlobalHotkeys final : public GlobalHotkeys
{
public:
    /** Changes closer together than this are bound as one set. */
    static constexpr std::chrono::milliseconds kSettleTime { 50 };
    /** Bound for the start-up probe (D-Bus may have to start the portal). */
    static constexpr int kProbeTimeoutMs = 3000;

    explicit PortalGlobalHotkeys (std::chrono::milliseconds settleTimeIn = kSettleTime)
        : settleTime (settleTimeIn)
    {
        api = dbus::Api::get();
        if (api == nullptr || ! start())
            closeConnection();
    }

    ~PortalGlobalHotkeys() override
    {
        if (thread.joinable())
        {
            running = false;
            wake();
            thread.join();
        }
        if (connection != nullptr)
        {
            closeSession();
            api->connectionFlush (connection);
        }
        closeConnection();
        for (int& fd : wakePipe)
            if (fd >= 0)
                ::close (fd);
    }

    bool isSupported() const override { return supported; }

    /** GlobalShortcuts interface version the portal reported (0 = none). */
    uint32_t getPortalVersion() const noexcept { return portalVersion; }

    bool registerHotkey (int id, const KeyChord& chord, std::function<void()> callback) override
    {
        if (! supported || ! callback || ! detail::isValidChord (chord))
            return false;
        Shortcut shortcut { portal::triggerFor (chord), "Flubsound Pro: " + chord.toString() };
        if (shortcut.trigger.empty())
            return false;
        {
            std::lock_guard<std::mutex> guard (mutex);
            wanted[id] = Wanted { std::move (shortcut), std::move (callback) };
            noteChange();
        }
        wake();
        return true;
    }

    void unregisterHotkey (int id) override
    {
        {
            std::lock_guard<std::mutex> guard (mutex);
            if (wanted.erase (id) == 0)
                return;
            noteChange();
        }
        wake();
    }

    void unregisterAll() override
    {
        {
            std::lock_guard<std::mutex> guard (mutex);
            if (wanted.empty())
                return;
            wanted.clear();
            noteChange();
        }
        wake();
    }

    /** Sends pending changes now instead of after the settle time (tests). */
    void applyNow()
    {
        {
            std::lock_guard<std::mutex> guard (mutex);
            applyImmediately = true;
        }
        wake();
    }

    /** Waits, at most 'timeout', until every change so far has been sent and
        the portal has answered (tests). */
    bool waitUntilSettled (std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock (mutex);
        return settledCondition.wait_for (lock, timeout, [this] { return settledRevision == revision; });
    }

private:
    struct Shortcut
    {
        std::string trigger;     // preferred_trigger, XDG shortcuts format
        std::string description; // shown in the desktop's dialog / settings
        bool operator== (const Shortcut&) const = default;
    };

    struct Wanted
    {
        Shortcut shortcut;
        std::function<void()> callback;
    };

    enum class Stage
    {
        Idle,
        CreatingSession,
        Binding
    };

    /** The one portal request in flight (service thread only). */
    struct Pending
    {
        Stage stage = Stage::Idle;
        uint32_t serial = 0;
        std::string requestPath, returnedPath;
        std::map<int, Shortcut> shortcuts;
        uint64_t revision = 0;
    };

    static constexpr uint64_t kReapply = ~uint64_t (0);

    bool start()
    {
        const std::string address = portal::sessionBusAddress();
        if (address.empty())
            return false;
        dbus::Error error;
        api->errorInit (&error);
        connection = api->connectionOpenPrivate (address.c_str(), &error);
        if (connection != nullptr)
        {
            api->connectionSetExitOnDisconnect (connection, 0);
            if (api->busRegister (connection, &error) == 0)
                closeConnection();
        }
        api->errorFree (&error);
        if (connection == nullptr)
            return false;

        uniqueName = dbus::str (api->busGetUniqueName (connection));
        portalVersion = probeVersion();
        int fd = -1;
        if (portalVersion == 0 || api->connectionGetUnixFd (connection, &fd) == 0 || ::pipe2 (wakePipe, O_CLOEXEC | O_NONBLOCK) != 0)
            return false;

        for (const char* rule : portal::kMatchRules)
            api->busAddMatch (connection, rule, nullptr); // asynchronous; ordered before any portal call
        api->connectionFlush (connection);

        busFd = fd;
        supported = true;
        running = true;
        thread = std::thread ([this] { run(); });
        return true;
    }

    /** Properties.Get (GlobalShortcuts, "version"): 0 when the portal is
        missing or has no GlobalShortcuts interface. Also learns the portal's
        unique bus name, the only sender whose signals are accepted. */
    uint32_t probeVersion()
    {
        dbus::MessageRef call (api->messageNewMethodCall (portal::kService, portal::kObjectPath, "org.freedesktop.DBus.Properties", "Get"));
        if (call == nullptr)
            return 0;
        dbus::Iter args;
        api->iterInitAppend (call.get(), &args);
        if (! dbus::appendBasic (*api, &args, dbus::kTypeString, portal::kShortcutsInterface)
            || ! dbus::appendBasic (*api, &args, dbus::kTypeString, "version"))
            return 0;

        dbus::Error error;
        api->errorInit (&error);
        dbus::MessageRef reply (api->sendWithReplyAndBlock (connection, call.get(), kProbeTimeoutMs, &error));
        api->errorFree (&error); // ServiceUnknown (no portal), InvalidArgs (no GlobalShortcuts), timeout
        if (reply == nullptr)
            return 0;

        uint32_t version = 0;
        dbus::Iter it, value;
        if (api->iterInit (reply.get(), &it) != 0 && api->iterGetArgType (&it) == dbus::kTypeVariant)
        {
            api->iterRecurse (&it, &value);
            if (api->iterGetArgType (&value) == dbus::kTypeUInt32)
                api->iterGetBasic (&value, &version);
        }
        portalOwner = dbus::str (api->messageGetSender (reply.get()));
        return portalOwner.empty() ? 0 : version;
    }

    void closeConnection()
    {
        if (connection == nullptr)
            return;
        api->connectionClose (connection); // private connections must be closed before the last unref
        api->connectionUnref (connection);
        connection = nullptr;
    }

    void wake()
    {
        if (wakePipe[1] < 0)
            return;
        const char byte = 0;
        [[maybe_unused]] const auto written = ::write (wakePipe[1], &byte, 1);
    }

    void noteChange() // mutex held
    {
        ++revision;
        lastChange = std::chrono::steady_clock::now();
    }

    void markSettled (uint64_t rev)
    {
        {
            std::lock_guard<std::mutex> guard (mutex);
            settledRevision = rev;
        }
        settledCondition.notify_all();
    }

    //--------------------------------------------------------------------------
    // Service thread
    void run()
    {
        pollfd fds[2] = { { busFd, POLLIN, 0 }, { wakePipe[0], POLLIN, 0 } };
        while (running)
        {
            if (api->connectionReadWrite (connection, 0) == 0) // reads and writes what it can, never blocks
            {
                portal::log ("lost the connection to the session bus; shortcuts stop working");
                break;
            }
            while (dbus::Message* message = api->connectionPopMessage (connection))
            {
                const dbus::MessageRef owner (message);
                handleMessage (message);
            }

            int timeoutMs = applyWantedSet();
            if (api->connectionGetDispatchStatus (connection) == dbus::kDispatchDataRemains)
                timeoutMs = 0; // already read into libdbus's queue: poll would not see it
            fds[0].events = static_cast<short> (POLLIN | (api->connectionHasMessagesToSend (connection) != 0 ? POLLOUT : 0));
            ::poll (fds, 2, timeoutMs);
            if ((fds[1].revents & POLLIN) != 0)
            {
                char buffer[16];
                while (::read (wakePipe[0], buffer, sizeof (buffer)) > 0) {}
            }
        }
    }

    /** Starts binding the wanted set once it has settled; returns the poll
        timeout in ms (-1 = until woken). */
    int applyWantedSet()
    {
        if (pending.stage != Stage::Idle)
            return -1;

        std::map<int, Shortcut> want;
        uint64_t rev = 0;
        {
            std::lock_guard<std::mutex> guard (mutex);
            if (revision == handledRevision)
                return -1;
            const auto sinceChange = std::chrono::steady_clock::now() - lastChange;
            if (! applyImmediately && sinceChange < settleTime)
                return static_cast<int> (std::chrono::ceil<std::chrono::milliseconds> (settleTime - sinceChange).count());
            applyImmediately = false;
            rev = revision;
            for (const auto& [id, w] : wanted)
                want.emplace (id, w.shortcut);
        }
        handledRevision = rev;

        if (want == bound && (want.empty() || ! session.empty()))
        {
            markSettled (rev); // e.g. unregisterAll() + the same registrations again
            return -1;
        }
        closeSession();
        bound.clear();
        if (want.empty() || ! createSession (std::move (want), rev))
            markSettled (rev);
        return -1;
    }

    std::string nextToken() { return "flubsound" + std::to_string (++tokenCounter); }

    bool send (dbus::MessageRef call, Stage stage, const std::string& requestToken, std::map<int, Shortcut> shortcuts, uint64_t rev)
    {
        uint32_t serial = 0;
        if (api->connectionSend (connection, call.get(), &serial) == 0)
        {
            portal::log ("could not send a request to the portal");
            return false;
        }
        pending = Pending { stage, serial, std::string (portal::kObjectPath) + "/request/" + portal::busPathElement (uniqueName) + "/" + requestToken,
                            {}, std::move (shortcuts), rev };
        return true;
    }

    bool createSession (std::map<int, Shortcut> want, uint64_t rev)
    {
        dbus::MessageRef call (api->messageNewMethodCall (portal::kService, portal::kObjectPath, portal::kShortcutsInterface, "CreateSession"));
        if (call == nullptr)
            return false;
        const std::string requestToken = nextToken();
        dbus::Iter args, options;
        api->iterInitAppend (call.get(), &args);
        const bool built = api->iterOpenContainer (&args, dbus::kTypeArray, "{sv}", &options) != 0
                           && dbus::appendDictEntry (*api, &options, "handle_token", dbus::kTypeString, requestToken)
                           && dbus::appendDictEntry (*api, &options, "session_handle_token", dbus::kTypeString, nextToken())
                           && api->iterCloseContainer (&args, &options) != 0;
        return built && send (std::move (call), Stage::CreatingSession, requestToken, std::move (want), rev);
    }

    bool bindShortcuts()
    {
        dbus::MessageRef call (api->messageNewMethodCall (portal::kService, portal::kObjectPath, portal::kShortcutsInterface, "BindShortcuts"));
        if (call == nullptr)
            return false;
        const std::string requestToken = nextToken();
        dbus::Iter args, list, options;
        api->iterInitAppend (call.get(), &args);
        bool built = dbus::appendBasic (*api, &args, dbus::kTypeObjectPath, session)
                     && api->iterOpenContainer (&args, dbus::kTypeArray, "(sa{sv})", &list) != 0;
        for (const auto& [id, shortcut] : pending.shortcuts)
        {
            dbus::Iter entry, properties;
            built = built && api->iterOpenContainer (&list, dbus::kTypeStruct, nullptr, &entry) != 0
                    && dbus::appendBasic (*api, &entry, dbus::kTypeString, portal::shortcutId (id))
                    && api->iterOpenContainer (&entry, dbus::kTypeArray, "{sv}", &properties) != 0
                    && dbus::appendDictEntry (*api, &properties, "description", dbus::kTypeString, shortcut.description)
                    && dbus::appendDictEntry (*api, &properties, "preferred_trigger", dbus::kTypeString, shortcut.trigger)
                    && api->iterCloseContainer (&entry, &properties) != 0 && api->iterCloseContainer (&list, &entry) != 0;
        }
        // parent_window "": the interface gives no window handle to pass.
        built = built && api->iterCloseContainer (&args, &list) != 0 && dbus::appendBasic (*api, &args, dbus::kTypeString, "")
                && api->iterOpenContainer (&args, dbus::kTypeArray, "{sv}", &options) != 0
                && dbus::appendDictEntry (*api, &options, "handle_token", dbus::kTypeString, requestToken)
                && api->iterCloseContainer (&args, &options) != 0;
        return built && send (std::move (call), Stage::Binding, requestToken, std::move (pending.shortcuts), pending.revision);
    }

    void closeSession()
    {
        if (session.empty())
            return;
        if (dbus::MessageRef call { api->messageNewMethodCall (portal::kService, session.c_str(), portal::kSessionInterface, "Close") })
        {
            api->messageSetNoReply (call.get(), 1);
            api->connectionSend (connection, call.get(), nullptr);
        }
        session.clear();
    }

    void finishPending()
    {
        const uint64_t rev = pending.revision;
        pending = Pending();
        markSettled (rev);
    }

    void handleMessage (dbus::Message* message)
    {
        const int type = api->messageGetType (message);
        if ((type == dbus::kMessageMethodReturn || type == dbus::kMessageError) && pending.stage != Stage::Idle
            && api->messageGetReplySerial (message) == pending.serial)
        {
            dbus::Iter it;
            std::string text;
            if (type == dbus::kMessageMethodReturn)
            {
                if (api->iterInit (message, &it) != 0 && dbus::readString (*api, &it, text))
                    pending.returnedPath = text; // normally == requestPath (portals older than 0.9 differ)
                return;
            }
            if (api->iterInit (message, &it) != 0)
                dbus::readString (*api, &it, text);
            portal::log (std::string (pending.stage == Stage::Binding ? "BindShortcuts" : "CreateSession") + " failed: "
                         + dbus::str (api->messageGetErrorName (message)) + " " + text);
            finishPending();
            return;
        }
        if (type != dbus::kMessageSignal)
            return;

        const std::string interfaceName = dbus::str (api->messageGetInterface (message));
        const std::string member = dbus::str (api->messageGetMember (message));
        const std::string path = dbus::str (api->messageGetPath (message));
        const std::string sender = dbus::str (api->messageGetSender (message));

        if (interfaceName == "org.freedesktop.DBus" && member == "NameOwnerChanged" && sender == "org.freedesktop.DBus")
            onPortalOwnerChanged (message);
        else if (sender != portalOwner)
            return; // nobody but the portal may press our shortcuts
        else if (interfaceName == portal::kRequestInterface && member == "Response" && pending.stage != Stage::Idle
                 && (path == pending.requestPath || (! pending.returnedPath.empty() && path == pending.returnedPath)))
            onResponse (message);
        else if (interfaceName == portal::kShortcutsInterface && member == "Activated")
            onActivated (message);
        else if (interfaceName == portal::kSessionInterface && member == "Closed" && ! session.empty() && path == session)
        {
            portal::log ("the desktop closed the session; shortcuts are requested again when the hotkey settings change");
            session.clear();
            bound.clear();
        }
    }

    void onResponse (dbus::Message* message)
    {
        dbus::Iter results;
        uint32_t code = 2;
        if (api->iterInit (message, &results) != 0 && api->iterGetArgType (&results) == dbus::kTypeUInt32)
        {
            api->iterGetBasic (&results, &code);
            api->iterNext (&results);
        }

        if (pending.stage == Stage::CreatingSession)
        {
            std::string handle; // 's' in the spec, 'o' in some back-ends
            if (code == 0 && dbus::readVardictString (*api, &results, "session_handle", handle) && portal::isValidObjectPath (handle))
            {
                session = handle;
                if (bindShortcuts())
                    return;
            }
            else
                portal::log ("the desktop did not open a session (response " + std::to_string (code) + ")");
            finishPending();
            return;
        }

        std::set<std::string> boundIds;
        dbus::Iter list;
        if (code == 0 && dbus::findInVardict (*api, &results, "shortcuts", list) && api->iterGetArgType (&list) == dbus::kTypeArray)
        {
            dbus::Iter items;
            api->iterRecurse (&list, &items);
            for (; api->iterGetArgType (&items) == dbus::kTypeStruct; api->iterNext (&items))
            {
                dbus::Iter item;
                std::string shortcut;
                api->iterRecurse (&items, &item);
                if (dbus::readString (*api, &item, shortcut))
                    boundIds.insert (shortcut);
            }
        }
        if (code != 0)
            portal::log (code == 1 ? "the user declined the shortcuts" : "the desktop refused the shortcuts (response " + std::to_string (code) + ")");
        else
            for (const auto& [id, shortcut] : pending.shortcuts)
                if (boundIds.count (portal::shortcutId (id)) == 0)
                    portal::log ("\"" + shortcut.description + "\" (" + shortcut.trigger + ") was not bound by the desktop");

        // Remembered even when refused, so re-registering the same set does
        // not ask the user again; a changed set starts a new session.
        bound = std::move (pending.shortcuts);
        finishPending();
    }

    void onActivated (dbus::Message* message)
    {
        dbus::Iter it;
        std::string sessionHandle, shortcut;
        if (api->iterInit (message, &it) == 0 || ! dbus::readString (*api, &it, sessionHandle) || api->iterNext (&it) == 0
            || ! dbus::readString (*api, &it, shortcut))
            return;
        if (session.empty() || sessionHandle != session)
            return; // a closed session's late signal

        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> guard (mutex);
            for (const auto& [id, w] : wanted)
                if (portal::shortcutId (id) == shortcut)
                    callback = w.callback;
        }
        if (callback) // outside the lock: a callback may (un)register
            callback();
    }

    /** The portal restarted (or went away): its sessions are gone. Rebind the
        wanted set with the new instance. */
    void onPortalOwnerChanged (dbus::Message* message)
    {
        dbus::Iter it;
        std::string name, oldOwner, newOwner;
        if (api->iterInit (message, &it) == 0 || ! dbus::readString (*api, &it, name) || name != portal::kService
            || api->iterNext (&it) == 0 || ! dbus::readString (*api, &it, oldOwner) || api->iterNext (&it) == 0
            || ! dbus::readString (*api, &it, newOwner))
            return;
        portalOwner = newOwner;
        session.clear();
        bound.clear();
        if (pending.stage != Stage::Idle)
            finishPending();
        if (! newOwner.empty())
            handledRevision = kReapply;
    }

    const dbus::Api* api = nullptr;
    const std::chrono::milliseconds settleTime;
    dbus::Connection* connection = nullptr;
    std::string uniqueName;
    uint32_t portalVersion = 0;
    bool supported = false;
    int busFd = -1;
    int wakePipe[2] = { -1, -1 };
    std::atomic<bool> running { false };
    std::thread thread;

    // Shared with the callers of registerHotkey / unregister* (mutex).
    std::mutex mutex;
    std::condition_variable settledCondition;
    std::map<int, Wanted> wanted;
    uint64_t revision = 0, settledRevision = 0;
    std::chrono::steady_clock::time_point lastChange;
    bool applyImmediately = false;

    // Service thread only (the constructor sets portalOwner before it starts).
    std::string portalOwner;
    std::string session;
    std::map<int, Shortcut> bound;
    Pending pending;
    uint64_t handledRevision = 0;
    unsigned int tokenCounter = 0;
};

//==============================================================================
// AppAudioRouter - pactl (PulseAudio / PipeWire-pulse)
//==============================================================================
class LinuxAppAudioRouter final : public AppAudioRouter
{
public:
    LinuxAppAudioRouter() : havePactl (pactl::isExecutableInPath ("pactl")) {}

    bool isSupported() const override { return havePactl; }

    std::vector<AudioSessionInfo> enumerateSessions() override
    {
        if (! havePactl)
            return {};

        std::string error;
        std::map<uint32_t, std::string> sinkNames;
        std::vector<pactl::SinkInput> inputs;

        const auto sinks = pactl::run ("LC_ALL=C pactl --format=json list sinks 2>/dev/null");
        if (sinks.exitCode == 0)
            pactl::parseSinks (sinks.output, sinkNames, error); // names are cosmetic; indices still work

        if (! listSinkInputs (inputs, error))
            return {};

        return pactl::toSessions (inputs, sinkNames, static_cast<uint32_t> (::getpid()));
    }

    /** Moves every current stream of the process to 'endpointId' (a sink name
        such as "flubsound_game", or a sink index); empty = the default sink.
        This affects running streams; PipeWire/WirePlumber (restore-stream)
        and PulseAudio (module-stream-restore) remember the choice for the
        application's future streams. See platform/linux for a rule file
        that pre-seeds routes without a running stream. */
    bool setAppEndpoint (uint32_t processId, const std::string& endpointId, std::string& error) override
    {
        if (! havePactl)
        {
            error = "pactl was not found. Install pulseaudio-utils (it also works with PipeWire).";
            return false;
        }

        if (processId == 0)
        {
            error = "Invalid process id.";
            return false;
        }

        if (processId == static_cast<uint32_t> (::getpid()))
        {
            error = "Flubsound's own output cannot be routed into its virtual devices (feedback loop).";
            return false;
        }

        const std::string target = endpointId.empty() ? std::string ("@DEFAULT_SINK@") : endpointId;
        if (! pactl::isSafeToken (target))
        {
            error = "Invalid output device name '" + endpointId + "'.";
            return false;
        }

        std::vector<pactl::SinkInput> inputs;
        if (! listSinkInputs (inputs, error))
            return false;

        int moved = 0;
        for (const auto& input : inputs)
        {
            if (input.processId != processId)
                continue;

            // Every variable part is an integer or a whitelisted, quoted token.
            const auto command =
                "LC_ALL=C pactl move-sink-input " + std::to_string (input.index) + " " + pactl::shellQuote (target) + " 2>&1";
            const auto result = pactl::run (command);
            if (result.exitCode != 0)
            {
                error = "pactl could not move the stream to '" + target + "': " + pactl::firstLine (result.output);
                return false;
            }
            ++moved;
        }

        if (moved == 0)
        {
            error = "Process " + std::to_string (processId)
                  + " is not playing audio right now. Start playback in the application and try again.";
            return false;
        }

        return true;
    }

    void openSystemRoutingSettings() override
    {
        // Constant command line (no user input). pavucontrol's tab 1 is
        // "Playback", which has a per-stream output selector. The trailing '&'
        // backgrounds the tool inside the shell, so system() returns at once
        // and the child is re-parented to init (no zombie).
        static constexpr const char* command =
            "( if command -v pavucontrol >/dev/null 2>&1; then exec pavucontrol --tab=1; "
            "elif command -v pwvucontrol >/dev/null 2>&1; then exec pwvucontrol; "
            "elif command -v gnome-control-center >/dev/null 2>&1; then exec gnome-control-center sound; "
            "elif command -v systemsettings >/dev/null 2>&1; then exec systemsettings kcm_pulseaudio; fi ) >/dev/null 2>&1 &";

        const int status = std::system (command);
        (void) status;
    }

private:
    static bool listSinkInputs (std::vector<pactl::SinkInput>& inputs, std::string& error)
    {
        // stderr is discarded so warnings can never corrupt the JSON on stdout.
        const auto result = pactl::run ("LC_ALL=C pactl --format=json list sink-inputs 2>/dev/null");
        if (result.exitCode == 0)
            return pactl::parseSinkInputs (result.output, inputs, error);

        // Tell "no server" apart from "pactl too old for --format=json".
        const auto info = pactl::run ("LC_ALL=C pactl info 2>&1");
        if (info.exitCode != 0)
            error = "Cannot reach the sound server (is PipeWire/pipewire-pulse or PulseAudio running?): " + pactl::firstLine (info.output);
        else
            error = "This pactl cannot print JSON; pactl 16 or newer (pulseaudio-utils 16+) is required.";
        return false;
    }

    const bool havePactl;
};

//==============================================================================
// ProcessLoopbackCapture - not applicable on Linux
//==============================================================================
/*  PipeWire could capture a single application's stream (link a capture
    stream to the app's output node, target.object = its node id), but the
    Linux design routes applications into per-strip null sinks instead and
    reads the sinks' monitors - one mechanism, persistent, visible in every
    mixer. So this reports isSupported() == false. */
class LinuxProcessLoopbackCapture final : public ProcessLoopbackCapture
{
public:
    bool isSupported() const override { return false; }

    bool start (uint32_t, bool, double, int, FrameCallback, std::string& error) override
    {
        error = "Per-application capture is not used on Linux: route the application to a Flubsound sink instead.";
        return false;
    }

    void stop() override {}
    bool isRunning() const override { return false; }
};

//==============================================================================
// AutoStart - XDG autostart entry (unnamed namespace; tests reach the helpers)
//==============================================================================
namespace autostart
{
constexpr const char* kFileName = "flubsound-pro.desktop";
constexpr size_t kMaxEntryBytes = 64u * 1024u; // a sane .desktop file is < 1 KB

std::string withoutTrailingSlashes (std::string path)
{
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    return path;
}

/** $XDG_CONFIG_HOME when set to an absolute path (the XDG Base Directory spec
    says relative values are invalid and must be ignored), else $HOME/.config,
    else the passwd entry's home + "/.config". Empty when no home is known. */
std::string configHome()
{
    if (const char* xdg = std::getenv ("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] == '/')
        return withoutTrailingSlashes (xdg);

    std::string home;
    if (const char* h = std::getenv ("HOME"); h != nullptr && h[0] == '/')
        home = h;
    else if (const passwd* pw = ::getpwuid (::getuid()); pw != nullptr && pw->pw_dir != nullptr && pw->pw_dir[0] == '/')
        home = pw->pw_dir; // message thread only: getpwuid is not reentrant

    return home.empty() ? std::string() : withoutTrailingSlashes (home) + "/.config";
}

std::string directory()
{
    const auto base = configHome();
    return base.empty() ? base : base + "/autostart";
}

std::string entryPath()
{
    const auto dir = directory();
    return dir.empty() ? dir : dir + "/" + kFileName;
}

/** Strict UTF-8 check (no overlong forms, surrogates or code points above
    U+10FFFF): Desktop Entry values must be UTF-8. */
bool isValidUtf8 (const std::string& text)
{
    size_t i = 0;
    while (i < text.size())
    {
        const auto lead = static_cast<unsigned char> (text[i]);
        size_t length = 0;
        uint32_t cp = 0;
        if (lead < 0x80)
        {
            ++i;
            continue;
        }
        if (lead >= 0xC2 && lead <= 0xDF)
        {
            length = 2;
            cp = lead & 0x1Fu;
        }
        else if (lead >= 0xE0 && lead <= 0xEF)
        {
            length = 3;
            cp = lead & 0x0Fu;
        }
        else if (lead >= 0xF0 && lead <= 0xF4)
        {
            length = 4;
            cp = lead & 0x07u;
        }
        else
        {
            return false;
        }

        if (i + length > text.size())
            return false;
        for (size_t k = 1; k < length; ++k)
        {
            const auto c = static_cast<unsigned char> (text[i + k]);
            if ((c & 0xC0u) != 0x80u)
                return false;
            cp = (cp << 6) | (c & 0x3Fu);
        }

        if ((length == 3 && cp < 0x800) || (length == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += length;
    }
    return true;
}

/** The Exec value that starts `program` with no arguments, following the
    Desktop Entry spec ("The Exec key"):
      1. the path is always double-quoted, and inside the quotes the four
         characters " ` $ \ get a backslash;
      2. the value is then a string value, whose escape rule doubles every
         backslash - so a quote is written \\" and a backslash \\\;
      3. '%' starts a field code and is written %%.
    Verified against GLib 2.80 (GKeyFile + g_shell_parse_argv). One GLib
    quirk remains: it checks that the program exists BEFORE expanding %%, so
    GNOME skips an entry whose path contains '%' (KDE starts it).
    Fails (with a user-presentable error) for what an Exec line cannot carry:
    an empty or relative path, invalid UTF-8, control characters. */
bool execValueFor (const std::string& program, std::string& exec, std::string& error)
{
    if (program.empty() || program.front() != '/')
    {
        error = "The program path for the start-up entry must be absolute: \"" + program + "\".";
        return false;
    }
    if (! isValidUtf8 (program))
    {
        error = "The program path is not valid UTF-8, which a start-up (.desktop) entry cannot store.";
        return false;
    }

    exec = "\"";
    for (const char c : program)
    {
        const auto u = static_cast<unsigned char> (c);
        if (u < 0x20 || u == 0x7F)
        {
            error = "The program path contains a control character, which a start-up (.desktop) entry cannot store.";
            return false;
        }

        switch (c)
        {
            case '"':
            case '`':
            case '$': exec += "\\\\"; exec += c; break; // quoting \x, then string-escaped backslash
            case '\\': exec += "\\\\\\\\"; break;       // quoting \\, each string-escaped
            case '%': exec += "%%"; break;
            default: exec += c; break;
        }
    }
    exec += '"';
    return true;
}

std::string desktopEntry (const std::string& execValue)
{
    return "[Desktop Entry]\n"
           "Type=Application\n"
           "Version=1.5\n"
           "Name=Flubsound Pro\n"
           "Comment=Music and gaming audio enhancer, started at sign-in\n"
           "Exec=" + execValue + "\n"
           "Terminal=false\n"
           "X-GNOME-Autostart-enabled=true\n";
}

/** Keys of the [Desktop Entry] group, values as written (not unescaped).
    Comments, blank lines and other groups are skipped. */
std::map<std::string, std::string> parseDesktopEntryGroup (const std::string& text)
{
    std::map<std::string, std::string> keys;
    bool inGroup = false;
    size_t pos = 0;
    while (pos < text.size())
    {
        auto end = text.find ('\n', pos);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr (pos, end - pos);
        pos = end + 1;

        if (! line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line.front() == '#')
            continue;
        if (line.front() == '[')
        {
            inGroup = line == "[Desktop Entry]";
            continue;
        }
        if (! inGroup)
            continue;

        const auto eq = line.find ('=');
        if (eq == std::string::npos)
            continue;
        auto key = line.substr (0, eq);
        auto value = line.substr (eq + 1);
        // The spec allows spaces around '='.
        while (! key.empty() && key.back() == ' ')
            key.pop_back();
        const auto first = value.find_first_not_of (' ');
        value.erase (0, first == std::string::npos ? value.size() : first);
        keys.emplace (std::move (key), std::move (value)); // first occurrence wins
    }
    return keys;
}

bool readFile (const std::string& path, std::string& contents)
{
    FILE* file = std::fopen (path.c_str(), "re");
    if (file == nullptr)
        return false;

    contents.clear();
    char buffer[4096];
    size_t bytes = 0;
    while ((bytes = std::fread (buffer, 1, sizeof (buffer), file)) > 0 && contents.size() <= kMaxEntryBytes)
        contents.append (buffer, bytes);
    const bool ok = std::ferror (file) == 0 && contents.size() <= kMaxEntryBytes;
    std::fclose (file);
    return ok;
}

/** An entry starts the app unless it is missing, has no [Desktop Entry]
    group, or is switched off with Hidden=true (KDE, the spec's way to
    delete) or X-GNOME-Autostart-enabled=false (GNOME Tweaks). */
bool isEntryEnabled (const std::string& path)
{
    std::string text;
    if (path.empty() || ! readFile (path, text))
        return false;

    const auto keys = parseDesktopEntryGroup (text);
    if (keys.empty())
        return false;
    const auto hidden = keys.find ("Hidden");
    const auto gnome = keys.find ("X-GNOME-Autostart-enabled");
    return ! (hidden != keys.end() && hidden->second == "true") && ! (gnome != keys.end() && gnome->second == "false");
}

std::string errnoText (int error) { return std::strerror (error); } // message thread only

/** mkdir -p; new directories get 0700 as the XDG Base Directory spec asks. */
bool makeDirectories (const std::string& path, std::string& error)
{
    for (size_t slash = path.find ('/', 1);; slash = path.find ('/', slash + 1))
    {
        const std::string partial = slash == std::string::npos ? path : path.substr (0, slash);
        if (! partial.empty() && ::mkdir (partial.c_str(), 0700) != 0 && errno != EEXIST)
        {
            error = "Could not create " + partial + ": " + errnoText (errno);
            return false;
        }
        if (slash == std::string::npos)
            break;
    }

    struct stat info {};
    if (::stat (path.c_str(), &info) != 0 || ! S_ISDIR (info.st_mode))
    {
        error = "Could not create the folder " + path + ".";
        return false;
    }
    return true;
}

/** Writes a sibling temp file, fsyncs it and renames it over `path`, so a
    crash or full disk never leaves a truncated entry behind. */
bool writeFileAtomically (const std::string& path, const std::string& contents, std::string& error)
{
    const std::string temp = path + ".tmp-" + std::to_string (static_cast<long> (::getpid()));
    const int fd = ::open (temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0)
    {
        error = "Could not write " + temp + ": " + errnoText (errno);
        return false;
    }

    const auto fail = [&] (const std::string& what, int code)
    {
        ::close (fd);
        ::unlink (temp.c_str());
        error = what + ": " + errnoText (code);
        return false;
    };

    size_t written = 0;
    while (written < contents.size())
    {
        const ssize_t n = ::write (fd, contents.data() + written, contents.size() - written);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return fail ("Could not write " + temp, errno);
        }
        written += static_cast<size_t> (n);
    }

    if (::fsync (fd) != 0)
        return fail ("Could not write " + temp, errno);
    if (::close (fd) != 0)
    {
        const int code = errno;
        ::unlink (temp.c_str());
        error = "Could not write " + temp + ": " + errnoText (code);
        return false;
    }
    if (::rename (temp.c_str(), path.c_str()) != 0)
    {
        const int code = errno;
        ::unlink (temp.c_str());
        error = "Could not create " + path + ": " + errnoText (code);
        return false;
    }
    return true;
}

/** The program to start: $APPIMAGE when running from an AppImage (the
    executable itself lives in a temporary mount), else /proc/self/exe
    without the " (deleted)" suffix it gets after an in-place upgrade. */
std::string runningExecutable()
{
    if (const char* appImage = std::getenv ("APPIMAGE"); appImage != nullptr && appImage[0] == '/')
        return appImage;

    char buffer[PATH_MAX];
    const ssize_t n = ::readlink ("/proc/self/exe", buffer, sizeof (buffer) - 1);
    if (n <= 0)
        return {};
    std::string path (buffer, static_cast<size_t> (n));
    const std::string deleted = " (deleted)";
    if (path.size() > deleted.size() && path.compare (path.size() - deleted.size(), deleted.size(), deleted) == 0)
        path.resize (path.size() - deleted.size());
    return path;
}
} // namespace autostart

class LinuxAutoStart final : public AutoStart
{
public:
    bool isSupported() const override { return ! autostart::entryPath().empty(); }
    bool isEnabled() const override { return autostart::isEntryEnabled (autostart::entryPath()); }

    bool setEnabled (bool shouldStart, const std::string& executablePath, std::string& error) override
    {
        const auto path = autostart::entryPath();
        if (path.empty())
        {
            error = "No home folder is known, so no start-up entry can be written.";
            return false;
        }

        if (! shouldStart)
        {
            if (::unlink (path.c_str()) != 0 && errno != ENOENT)
            {
                error = "Could not remove " + path + ": " + autostart::errnoText (errno);
                return false;
            }
            return true;
        }

        std::string exec;
        const auto program = executablePath.empty() ? autostart::runningExecutable() : executablePath;
        return autostart::execValueFor (program, exec, error) && autostart::makeDirectories (autostart::directory(), error)
               && autostart::writeFileAtomically (path, autostart::desktopEntry (exec), error);
    }
};

//==============================================================================
/** Saved scheduling state, owned by the handle promoteAudioThread returns. */
struct SavedSchedulingPolicy
{
    int policy = SCHED_OTHER;
    sched_param param {};
};

/** Same ceiling rtkit grants by default and that PipeWire's data thread runs
    at under rtkit, so a promoted Flubsound thread never out-ranks the sound
    server it feeds. */
constexpr int kPreferredFifoPriority = 20;
} // namespace

// ============================================================================
// AudioEndpoints
// ============================================================================
EndpointTransport AudioEndpoints::queryOutputTransport (const std::string&)
{
    // JUCE's ALSA/JACK device names do not map 1:1 to PipeWire/Pulse sinks, so
    // the transport (PipeWire "device.bus") is not queried here; headset
    // profiles fall back to flub::device::detectConnection() heuristics.
    return EndpointTransport::Unknown;
}

//==============================================================================
// SystemTuning
//==============================================================================
bool SystemTuning::disablePowerThrottling()
{
    // Linux has no per-process EcoQoS equivalent to opt out of: CPU frequency
    // is governed system-wide (cpufreq governor / power-profiles-daemon) and a
    // SCHED_FIFO audio thread already gets the scheduling it needs.
    return true;
}

void* SystemTuning::promoteAudioThread()
{
    /*  Best effort SCHED_FIFO. The kernel allows it when the user has an
        RLIMIT_RTPRIO allowance (e.g. "@audio - rtprio 95" in limits.conf, the
        'realtime' group, or CAP_SYS_NICE); otherwise it fails with EPERM and
        we silently stay at SCHED_OTHER. Desktop sessions without such limits
        would need RealtimeKit (org.freedesktop.RealtimeKit1.MakeThreadRealtime
        over the system D-Bus) - not wired up here to avoid a D-Bus dependency;
        when Flubsound runs as a JACK / PipeWire client the server already
        calls our process callback on its own real-time thread.

        SCHED_RESET_ON_FORK keeps children (e.g. pactl started via popen) from
        inheriting real-time priority; rtkit requires it as well. */
    const pthread_t self = ::pthread_self();

    int currentPolicy = SCHED_OTHER;
    sched_param currentParam {};
    if (::pthread_getschedparam (self, &currentPolicy, &currentParam) != 0)
        return nullptr;

    // Already real-time (JACK / PipeWire-JACK call us on the server's client
    // thread, typically SCHED_FIFO 70+): leave it alone. Setting our own
    // priority here would DEMOTE the sound server's thread.
    const int basePolicy = currentPolicy & ~SCHED_RESET_ON_FORK;
    if (basePolicy == SCHED_FIFO || basePolicy == SCHED_RR)
        return nullptr;

    auto saved = std::make_unique<SavedSchedulingPolicy>();
    saved->policy = currentPolicy;
    saved->param = currentParam;

    const int minPriority = ::sched_get_priority_min (SCHED_FIFO);
    const int maxPriority = ::sched_get_priority_max (SCHED_FIFO);
    if (minPriority < 0 || maxPriority < minPriority)
        return nullptr;

    const auto tryPriority = [self, minPriority, maxPriority] (int priority)
    {
        sched_param param {};
        param.sched_priority = std::clamp (priority, minPriority, maxPriority);
        return ::pthread_setschedparam (self, SCHED_FIFO | SCHED_RESET_ON_FORK, &param) == 0;
    };

    bool promoted = tryPriority (kPreferredFifoPriority);

    if (! promoted)
    {
        // Allowed, but only up to a lower RLIMIT_RTPRIO? Retry at that ceiling.
        rlimit limit {};
        if (::getrlimit (RLIMIT_RTPRIO, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY && limit.rlim_cur > 0
            && limit.rlim_cur < static_cast<rlim_t> (kPreferredFifoPriority))
            promoted = tryPriority (static_cast<int> (limit.rlim_cur));
    }

    return promoted ? saved.release() : nullptr;
}

void SystemTuning::revertAudioThread (void* handle)
{
    if (handle == nullptr)
        return;

    const std::unique_ptr<SavedSchedulingPolicy> saved (static_cast<SavedSchedulingPolicy*> (handle));
    const pthread_t self = ::pthread_self();

    // Exact restore first. Without CAP_SYS_NICE the kernel refuses to CLEAR
    // SCHED_RESET_ON_FORK once set (sched(7)), i.e. a desktop user promoted via
    // RLIMIT_RTPRIO gets EPERM here and would stay SCHED_FIFO forever. Keeping
    // the flag is harmless for a normal-policy thread, so retry with it.
    if (::pthread_setschedparam (self, saved->policy, &saved->param) != 0)
        ::pthread_setschedparam (self, saved->policy | SCHED_RESET_ON_FORK, &saved->param);
}

//==============================================================================
std::unique_ptr<GlobalHotkeys> GlobalHotkeys::create()
{
    // X key grabs are not global under Wayland (XWayland only sees keys while
    // one of its windows has focus): there the portal is the only route, and
    // without it the service is unsupported.
    if (isWaylandSession())
        return std::make_unique<PortalGlobalHotkeys>();
    return std::make_unique<LinuxGlobalHotkeys>();
}
std::unique_ptr<AppAudioRouter> AppAudioRouter::create() { return std::make_unique<LinuxAppAudioRouter>(); }
std::unique_ptr<ProcessLoopbackCapture> ProcessLoopbackCapture::create() { return std::make_unique<LinuxProcessLoopbackCapture>(); }
std::unique_ptr<AutoStart> AutoStart::create() { return std::make_unique<LinuxAutoStart>(); }
} // namespace flub::platform

#endif // __linux__
