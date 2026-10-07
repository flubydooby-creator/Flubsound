// Flubsound Pro - OS-independent parts of the platform services:
// KeyChord formatting/validation, and "unsupported" fallbacks for operating
// systems that have no dedicated PlatformServices_<os> implementation.
//
// Compile this file on every platform, together with exactly one of
// PlatformServices_win.cpp / PlatformServices_mac.mm / PlatformServices_linux.cpp
// (each of those is also guarded by its own OS macro, so globbing all of them
// into one target is harmless).
#include "PlatformServices.h"
#include "PlatformServicesInternal.h"

#include <algorithm>
#include <cctype>
#include <iterator>

namespace flub::platform
{
namespace detail
{
std::string keyName (uint32_t keyCode)
{
    if (isLetterKey (keyCode) || isDigitKey (keyCode))
        return std::string (1, static_cast<char> (keyCode));

    if (const int n = functionKeyNumber (keyCode); n > 0)
        return "F" + std::to_string (n);

    switch (keyCode)
    {
        case 0x20: return "Space";
        case 0x21: return "PageUp";
        case 0x22: return "PageDown";
        case 0x23: return "End";
        case 0x24: return "Home";
        case 0x25: return "Left";
        case 0x26: return "Up";
        case 0x27: return "Right";
        case 0x28: return "Down";
        case 0x2D: return "Insert";
        case 0x2E: return "Delete";
        default: break;
    }
    return {};
}

bool isValidChord (const KeyChord& chord, std::string* reason)
{
    const auto fail = [reason] (const char* why)
    {
        if (reason != nullptr)
            *reason = why;
        return false;
    };

    if ((chord.modifiers & ~kAllModifiers) != 0)
        return fail ("Unknown modifier flags in shortcut.");

    if (keyName (chord.keyCode).empty())
        return fail ("Shortcuts must use a letter, a digit, F1-F24, an arrow key, Space, Home, End, PageUp, PageDown, Insert or Delete.");

    const bool isFKey = isFunctionKey (chord.keyCode);
    const uint32_t nonShiftModifiers = chord.modifiers & (KeyChord::Ctrl | KeyChord::Alt | KeyChord::Super);

    if (! isFKey && nonShiftModifiers == 0)
        return fail ("Letter, digit and navigation-key shortcuts need Ctrl, Alt or the Win/Cmd key.");

    return true;
}
} // namespace detail

//==============================================================================
std::string KeyChord::toString() const
{
    // Platform-conventional names for the Super / Alt modifiers. The order
    // follows each OS's own menus (Ctrl, Alt/Option, Shift, Win/Cmd/Super).
#if defined(_WIN32)
    static constexpr const char* altName = "Alt";
    static constexpr const char* superName = "Win";
#elif defined(__APPLE__)
    static constexpr const char* altName = "Option";
    static constexpr const char* superName = "Cmd";
#else
    static constexpr const char* altName = "Alt";
    static constexpr const char* superName = "Super";
#endif

    std::string text;
    const auto append = [&text] (const std::string& part)
    {
        if (part.empty())
            return;
        if (! text.empty())
            text += '+';
        text += part;
    };

    if ((modifiers & Ctrl) != 0)
        append ("Ctrl");
    if ((modifiers & Alt) != 0)
        append (altName);
    if ((modifiers & Shift) != 0)
        append ("Shift");
    if ((modifiers & Super) != 0)
        append (superName);

    if (keyCode != 0)
    {
        auto key = detail::keyName (keyCode);

        if (key.empty())
        {
            // Not a code KeyChord defines; show it rather than hiding it.
            static constexpr char hex[] = "0123456789ABCDEF";
            key = "Key 0x";
            key += hex[(keyCode >> 4) & 0xfu];
            key += hex[keyCode & 0xfu];
        }

        append (key);
    }

    return text;
}

//==============================================================================
std::string AppAudioRouter::matchOutputDeviceName (const std::vector<OutputEndpoint>& endpoints, const std::string& deviceName)
{
    if (deviceName.empty())
        return {};

    // JUCE's StringArray::appendNumbersToDuplicates (ignoreCase = false,
    // appendNumberToFirstInstance = false): the first of equal names keeps
    // it, the later ones get " (2)", " (3)" ... in list order.
    std::vector<std::string> names;
    names.reserve (endpoints.size());
    for (const auto& e : endpoints)
        names.push_back (e.name);
    for (size_t i = 0; i + 1 < names.size(); ++i)
    {
        const auto original = names[i];
        int number = 1;
        for (size_t j = i + 1; j < names.size(); ++j)
            if (names[j] == original)
                names[j] += " (" + std::to_string (++number) + ")";
    }

    for (size_t i = 0; i < names.size(); ++i)
        if (! endpoints[i].name.empty() && names[i] == deviceName)
            return endpoints[i].id;
    return {};
}

std::string AppAudioRouter::endpointIdFromInterfacePath (const std::string& path)
{
    // "\\?\SWD#MMDEVAPI#" + id + "#{interface class guid}"; the case of the
    // prefix varies between Windows components, the id keeps its own.
    static constexpr char prefix[] = "\\\\?\\SWD#MMDEVAPI#";
    constexpr size_t prefixLength = sizeof (prefix) - 1;
    if (path.size() <= prefixLength)
        return path;
    for (size_t i = 0; i < prefixLength; ++i)
        if (std::tolower (static_cast<unsigned char> (path[i])) != std::tolower (static_cast<unsigned char> (prefix[i])))
            return path;
    const auto end = path.find ('#', prefixLength);
    return path.substr (prefixLength, end == std::string::npos ? std::string::npos : end - prefixLength);
}

//==============================================================================
// docs/11 E51: output endpoint identities
namespace
{
char lowerAscii (char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char> (c - 'A' + 'a') : c;
}

char upperAscii (char c) noexcept
{
    return c >= 'a' && c <= 'z' ? static_cast<char> (c - 'a' + 'A') : c;
}

bool equalsIgnoringCase (const std::string& a, const std::string& b) noexcept
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lowerAscii (a[i]) != lowerAscii (b[i]))
            return false;
    return true;
}

bool isDigit (char c) noexcept
{
    return c >= '0' && c <= '9';
}

bool isHexDigit (char c) noexcept
{
    return isDigit (c) || (lowerAscii (c) >= 'a' && lowerAscii (c) <= 'f');
}
} // namespace

std::string AudioDeviceWatcher::withoutInstanceNumber (const std::string& friendlyName)
{
    std::string name = friendlyName;

    // JUCE's duplicate number: "Speakers (Stealth) (2)" -> "Speakers (Stealth)".
    if (name.size() > 4 && name.back() == ')')
    {
        const auto open = name.rfind (" (");
        if (open != std::string::npos && open + 3 < name.size())
        {
            bool digits = true;
            for (size_t i = open + 2; i + 1 < name.size(); ++i)
                digits = digits && isDigit (name[i]);
            if (digits)
                name.erase (open);
        }
    }

    // Windows' instance number inside the parentheses: "(2- Stealth 700)".
    // It is added to the device's interface name, so it follows the first "(".
    if (const auto open = name.find ('('); open != std::string::npos)
    {
        size_t i = open + 1;
        while (i < name.size() && isDigit (name[i]))
            ++i;
        if (i > open + 1 && i + 1 < name.size() && name[i] == '-' && name[i + 1] == ' ')
            name.erase (open + 1, i + 2 - (open + 1));
    }
    return name;
}

AudioDeviceWatcher::Match AudioDeviceWatcher::matchIdentity (const OutputEndpointIdentity& remembered, const OutputEndpointIdentity& candidate)
{
    if (! remembered.id.empty() && remembered.id == candidate.id)
        return Match::Exact;

    const bool bothHardware = ! remembered.hardwareId.empty() && ! candidate.hardwareId.empty();
    if (bothHardware && ! equalsIgnoringCase (remembered.hardwareId, candidate.hardwareId))
        return Match::None;

    const auto a = withoutInstanceNumber (remembered.name), b = withoutInstanceNumber (candidate.name);
    if (a.empty() || ! equalsIgnoringCase (a, b))
        return Match::None;
    return bothHardware ? Match::Hardware : Match::Name;
}

int AudioDeviceWatcher::findEndpoint (const std::vector<OutputEndpointIdentity>& endpoints, const OutputEndpointIdentity& remembered,
                                      Match* how)
{
    const auto rank = [] (Match m) { return m == Match::Exact ? 3 : m == Match::Hardware ? 2 : m == Match::Name ? 1 : 0; };
    int best = -1;
    Match bestMatch = Match::None;
    for (size_t i = 0; i < endpoints.size(); ++i)
    {
        const auto m = matchIdentity (remembered, endpoints[i]);
        if (rank (m) > rank (bestMatch))
        {
            best = static_cast<int> (i);
            bestMatch = m;
        }
    }
    if (how != nullptr)
        *how = bestMatch;
    return best;
}

std::string AudioDeviceWatcher::hardwareIdFromDevicePath (const std::string& devicePath)
{
    std::string lower;
    lower.reserve (devicePath.size());
    for (const char c : devicePath)
        lower += lowerAscii (c);

    // The 4-hex-digit value after `key` at `from`, or empty.
    const auto hexAfter = [&lower] (const std::string& key, size_t from) -> std::string
    {
        const auto at = lower.find (key, from);
        if (at == std::string::npos || at + key.size() + 4 > lower.size())
            return {};
        const auto digits = lower.substr (at + key.size(), 4);
        for (const char c : digits)
            if (! isHexDigit (c))
                return {};
        return digits;
    };
    const auto upper = [] (std::string s)
    {
        for (auto& c : s)
            c = upperAscii (c);
        return s;
    };

    // Bluetooth: the device's address is stable, whatever the adapter.
    for (const char* bus : { "bthenum", "bthhfenum", "bthledevice" })
    {
        if (const auto at = lower.find (bus); at != std::string::npos)
        {
            // ...#{0000110b-...}_vid&0002054c_pid&0e45#8&...&0&<address>_c00000000#{...} or ...\<address>...
            // The first run of exactly 12 hex digits outside {...} (the
            // service GUID holds one too: 00805f9b34fb).
            std::string address;
            int braces = 0;
            for (size_t i = at; i < lower.size() && address.empty();)
            {
                const char c = lower[i];
                braces += c == '{' ? 1 : c == '}' && braces > 0 ? -1 : 0;
                size_t n = 0;
                while (braces == 0 && i + n < lower.size() && isHexDigit (lower[i + n]))
                    ++n;
                if (n == 12)
                    address = lower.substr (i, 12);
                i += std::max<size_t> (n, 1);
            }
            if (! address.empty())
                return upper (std::string (bus) + "\\" + address);
        }
    }

    const auto vid = hexAfter ("vid_", 0);
    const auto pid = hexAfter ("pid_", 0);
    if (vid.empty() || pid.empty())
        return {};

    std::string bus = "USB";
    if (lower.find ("hid") == 0 || lower.find ("#hid#") != std::string::npos || lower.find ("\\hid#") != std::string::npos)
        bus = "HID";
    std::string id = bus + "\\VID_" + upper (vid) + "&PID_" + upper (pid);
    if (const auto mi = lower.find ("&mi_"); mi != std::string::npos && mi + 6 <= lower.size() && isHexDigit (lower[mi + 4])
                                             && isHexDigit (lower[mi + 5]))
        id += "&MI_" + upper (lower.substr (mi + 4, 2));
    return id;
}

//==============================================================================
// docs/11 E55: process information cache
ProcessInfoCache::ProcessInfoCache (ProcessQuery processQueryToUse, DescriptionQuery descriptionQueryToUse)
    : processQuery (std::move (processQueryToUse)), descriptionQuery (std::move (descriptionQueryToUse))
{
}

void ProcessInfoCache::beginPass()
{
    for (auto& e : entries)
        e.second.seen = false;
}

ProcessInfoCache::Info ProcessInfoCache::resolve (uint32_t, const std::string& path, uint64_t startTime)
{
    Info info;
    info.executablePath = path;
    const auto slash = path.find_last_of ("\\/");
    info.executableName = slash == std::string::npos ? path : path.substr (slash + 1);
    info.startTime = startTime;
    if (descriptionQuery && ! path.empty())
        info.description = descriptionQuery (path);
    return info;
}

const ProcessInfoCache::Info& ProcessInfoCache::lookup (uint32_t processId, const std::string& sessionKey)
{
    auto it = std::find_if (entries.begin(), entries.end(), [processId] (const auto& e) { return e.first == processId; });
    if (it != entries.end() && ! sessionKey.empty()
        && std::find (it->second.sessionKeys.begin(), it->second.sessionKeys.end(), sessionKey) != it->second.sessionKeys.end())
    {
        it->second.seen = true;
        return it->second.info; // a known session of a known process: nothing is opened
    }

    std::string path;
    uint64_t startTime = 0;
    ++processOpens;
    const bool opened = processQuery && processQuery (processId, path, startTime);

    if (it == entries.end())
    {
        entries.push_back ({ processId, Entry {} });
        it = std::prev (entries.end());
        it->second.info = resolve (processId, opened ? path : std::string(), opened ? startTime : 0);
    }
    else if (opened && (startTime != it->second.info.startTime || path != it->second.info.executablePath))
    {
        // The id now names another process: resolve it again, forget the old sessions.
        it->second.info = resolve (processId, path, startTime);
        it->second.sessionKeys.clear();
    }
    // (Not opened: a process that went away or denies access keeps what is known.)

    if (! sessionKey.empty())
        it->second.sessionKeys.push_back (sessionKey);
    it->second.seen = true;
    return it->second.info;
}

void ProcessInfoCache::endPass()
{
    entries.erase (std::remove_if (entries.begin(), entries.end(), [] (const auto& e) { return ! e.second.seen; }), entries.end());
}

#if ! defined(_WIN32)
//==============================================================================
// docs/11 E51 / E55 on systems without their implementation (Linux, macOS:
// JUCE's own device notifications and the name-based selection remain; no
// anti-cheat services to look for).
namespace
{
class UnsupportedDeviceWatcher final : public AudioDeviceWatcher
{
public:
    bool isSupported() const override { return false; }
    std::vector<OutputEndpointIdentity> listOutputs() override { return {}; }
    bool start (Listener) override { return false; }
    void stop() override {}
};
} // namespace

std::unique_ptr<AudioDeviceWatcher> AudioDeviceWatcher::create() { return std::make_unique<UnsupportedDeviceWatcher>(); }
std::vector<std::string> AntiCheatServices::running() { return {}; }
#endif

//==============================================================================
// Fallback for operating systems without a dedicated implementation (e.g. the
// BSDs): every service reports isSupported() == false so the UI hides it.
#if ! defined(_WIN32) && ! defined(__APPLE__) && ! defined(__linux__)
namespace
{
class UnsupportedHotkeys final : public GlobalHotkeys
{
public:
    using GlobalHotkeys::registerHotkey;
    bool isSupported() const override { return false; }

    bool registerHotkey (int id, const KeyChord&, const std::string&, std::function<void()>) override
    {
        reportBinding (id, BindingResult::Status::Unavailable);
        return false;
    }

    void unregisterHotkey (int) override {}
    void unregisterAll() override {}
};

class UnsupportedRouter final : public AppAudioRouter
{
public:
    bool isSupported() const override { return false; }
    std::vector<AudioSessionInfo> enumerateSessions() override { return {}; }
    std::string cannotMoveReason() const override { return "Per-application routing is not supported on this operating system. Choose a Flubsound output device per app in the system sound settings instead."; }

    bool setAppEndpoint (uint32_t, const std::string&, std::string& error) override
    {
        error = cannotMoveReason();
        return false;
    }

    void openSystemRoutingSettings() override {}
};

class UnsupportedCapture final : public ProcessLoopbackCapture
{
public:
    bool isSupported() const override { return false; }

    bool start (uint32_t, bool, double, int, FrameCallback, std::string& error) override
    {
        error = "Per-process audio capture is not supported on this operating system.";
        return false;
    }

    void stop() override {}
    bool isRunning() const override { return false; }
};

class UnsupportedAutoStart final : public AutoStart
{
public:
    bool isSupported() const override { return false; }
    bool isEnabled() const override { return false; }

    bool setEnabled (bool, const std::string&, std::string& error) override
    {
        error = "Starting with the operating system is not supported here.";
        return false;
    }
};

class UnsupportedForegroundApp final : public ForegroundApp
{
public:
    bool isSupported() const override { return false; }
    bool query (ForegroundAppInfo&) override { return false; }
    std::string unsupportedReason() const override { return "The foreground application cannot be detected on this operating system."; }
};
} // namespace

std::unique_ptr<GlobalHotkeys> GlobalHotkeys::create() { return std::make_unique<UnsupportedHotkeys>(); }
std::unique_ptr<AppAudioRouter> AppAudioRouter::create() { return std::make_unique<UnsupportedRouter>(); }
std::unique_ptr<ProcessLoopbackCapture> ProcessLoopbackCapture::create() { return std::make_unique<UnsupportedCapture>(); }
std::unique_ptr<AutoStart> AutoStart::create() { return std::make_unique<UnsupportedAutoStart>(); }
std::unique_ptr<ForegroundApp> ForegroundApp::create() { return std::make_unique<UnsupportedForegroundApp>(); }

bool SystemTuning::disablePowerThrottling() { return true; }
void* SystemTuning::promoteAudioThread() { return nullptr; }
void SystemTuning::revertAudioThread (void*) {}
#endif
} // namespace flub::platform
