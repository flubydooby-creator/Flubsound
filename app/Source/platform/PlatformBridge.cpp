#include "PlatformBridge.h"

#ifndef FLUB_HAS_PLATFORM_SERVICES
    #define FLUB_HAS_PLATFORM_SERVICES 0
#endif

#if FLUB_HAS_PLATFORM_SERVICES
    #include "PlatformServicesInternal.h" // detail::isValidChord
#endif

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h> // VkKeyScanW, CharLowerW (keyCodeForCharacter)
#endif

namespace flub::app::platform_bridge
{
bool servicesCompiledIn() noexcept
{
    return FLUB_HAS_PLATFORM_SERVICES != 0;
}

#if FLUB_HAS_PLATFORM_SERVICES

std::unique_ptr<flub::platform::GlobalHotkeys> createGlobalHotkeys()
{
    return flub::platform::GlobalHotkeys::create();
}

std::string chordProblem (const flub::platform::KeyChord& chord)
{
    if (chord.keyCode == 0)
        return "No key.";
    std::string reason;
    if (! flub::platform::detail::isValidChord (chord, &reason))
        return reason;
   #if defined(_WIN32)
    // PlatformServices_win.cpp refuses it too (RegisterHotKey's documentation).
    if (chord.keyCode == flub::platform::detail::kFunctionKeyBase + 11 && chord.modifiers == flub::platform::KeyChord::None)
        return "F12 alone is reserved by Windows.";
   #endif
    return {};
}

std::unique_ptr<flub::platform::AppAudioRouter> createAppAudioRouter()
{
    return flub::platform::AppAudioRouter::create();
}

std::unique_ptr<flub::platform::ProcessLoopbackCapture> createProcessLoopbackCapture()
{
    return flub::platform::ProcessLoopbackCapture::create();
}

std::unique_ptr<flub::platform::AutoStart> createAutoStart()
{
    return flub::platform::AutoStart::create();
}

std::unique_ptr<flub::platform::ForegroundApp> createForegroundApp()
{
    return flub::platform::ForegroundApp::create();
}

bool disablePowerThrottling()
{
    return flub::platform::SystemTuning::disablePowerThrottling();
}

void* promoteAudioThread()
{
    return flub::platform::SystemTuning::promoteAudioThread();
}

void revertAudioThread (void* handle)
{
    flub::platform::SystemTuning::revertAudioThread (handle);
}

#else // no OS implementation in this build

std::unique_ptr<flub::platform::GlobalHotkeys> createGlobalHotkeys() { return {}; }
std::string chordProblem (const flub::platform::KeyChord& chord) { return chord.keyCode == 0 ? "No key." : std::string(); }
std::unique_ptr<flub::platform::AppAudioRouter> createAppAudioRouter() { return {}; }
std::unique_ptr<flub::platform::ProcessLoopbackCapture> createProcessLoopbackCapture() { return {}; }
std::unique_ptr<flub::platform::AutoStart> createAutoStart() { return {}; }
std::unique_ptr<flub::platform::ForegroundApp> createForegroundApp() { return {}; }
bool disablePowerThrottling() { return false; }
void* promoteAudioThread() { return nullptr; }
void revertAudioThread (void*) {}

#endif

uint32_t keyCodeForCharacter (uint32_t character)
{
   #if defined(_WIN32)
    if (character <= 0x20 || character > 0xFFFF)
        return 0;
    const auto ch = static_cast<WCHAR> (character);
    SHORT scan = VkKeyScanW (ch);
    if (scan != -1 && (scan & 0xFF00) != 0)
    {
        // A capital letter needs Shift: its lower case may not (a Cyrillic
        // 'И' is the B key's 'и'); '&' on a US layout stays Shift+7, refused.
        const auto lower = static_cast<WCHAR> (reinterpret_cast<ULONG_PTR> (CharLowerW (reinterpret_cast<LPWSTR> (static_cast<ULONG_PTR> (ch)))));
        if (lower != ch)
            scan = VkKeyScanW (lower);
    }
    if (scan == -1 || (scan & 0xFF00) != 0)
        return 0; // no key types it, or only with Shift / Ctrl / Alt
    const auto vk = static_cast<uint32_t> (scan & 0xFF);
    return (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') ? vk : 0;
   #else
    (void) character;
    return 0;
   #endif
}

bool isProcessCaptureSupported()
{
    const auto probe = createProcessLoopbackCapture();
    return probe != nullptr && probe->isSupported();
}
} // namespace flub::app::platform_bridge
