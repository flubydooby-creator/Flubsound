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

    bool setAppEndpoint (uint32_t, const std::string&, std::string& error) override
    {
        error = "Per-application routing is not supported on this operating system.";
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
} // namespace

std::unique_ptr<GlobalHotkeys> GlobalHotkeys::create() { return std::make_unique<UnsupportedHotkeys>(); }
std::unique_ptr<AppAudioRouter> AppAudioRouter::create() { return std::make_unique<UnsupportedRouter>(); }
std::unique_ptr<ProcessLoopbackCapture> ProcessLoopbackCapture::create() { return std::make_unique<UnsupportedCapture>(); }
std::unique_ptr<AutoStart> AutoStart::create() { return std::make_unique<UnsupportedAutoStart>(); }

bool SystemTuning::disablePowerThrottling() { return true; }
void* SystemTuning::promoteAudioThread() { return nullptr; }
void SystemTuning::revertAudioThread (void*) {}
#endif
} // namespace flub::platform
