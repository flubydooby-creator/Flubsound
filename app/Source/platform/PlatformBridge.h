// Flubsound Pro - single access point to the optional OS services.
//
// The OS implementations of PlatformServices.h (PlatformServices_<os>.cpp/.mm)
// are developed separately. app/CMakeLists.txt defines
// FLUB_HAS_PLATFORM_SERVICES=1 only when the implementation for the current OS
// exists, so the app always compiles and links. Every other file in the app
// goes through this bridge instead of calling the flub::platform factories
// directly; when the services are missing the factories return nullptr and
// the tuning calls are no-ops, and callers must treat nullptr exactly like
// isSupported() == false (grey out / hide the feature).
#pragma once

#include "PlatformServices.h"

#include <memory>
#include <string>

namespace flub::app::platform_bridge
{
/** True when the build contains an OS implementation of PlatformServices.h. */
bool servicesCompiledIn() noexcept;

/** Factories: nullptr when the services are not compiled in, or when the OS
    implementation returned nullptr. The object may still report
    isSupported() == false (e.g. per-process capture on older Windows). */
std::unique_ptr<flub::platform::GlobalHotkeys> createGlobalHotkeys();
std::unique_ptr<flub::platform::AppAudioRouter> createAppAudioRouter();
std::unique_ptr<flub::platform::ProcessLoopbackCapture> createProcessLoopbackCapture();
/** "Start with the OS" (sign-in) entry; nullptr = treat as unsupported. */
std::unique_ptr<flub::platform::AutoStart> createAutoStart();
/** Foreground application (automatic profiles); nullptr = unsupported. */
std::unique_ptr<flub::platform::ForegroundApp> createForegroundApp();

/** Why `chord` cannot be a global hotkey on any OS (the shared validator,
    detail::isValidChord: a supported key, and a modifier other than Shift
    for letters, digits and navigation keys); empty when it can. Without the
    services, only an unassigned chord is refused. */
std::string chordProblem (const flub::platform::KeyChord& chord);

/** The KeyChord key ('A' - 'Z' or '0' - '9') that types `character` on the
    current keyboard layout without Shift (or, for a capital letter, its
    lower case), 0 when there is none or it is not known. Windows only
    (VkKeyScanW): JUCE names a key by its unshifted character there, so an
    AZERTY digit key arrives as '&' or 'é' and a Cyrillic letter key as its
    letter. Other systems: 0 (such keys are typed, not recorded). Call on the
    message thread: the layout is that of the window with the focus. */
uint32_t keyCodeForCharacter (uint32_t character);

/** True if a capture object can be created and reports isSupported(). Creates
    and destroys a probe object: call from the message thread, not per block. */
bool isProcessCaptureSupported();

/** SystemTuning wrappers (no-ops without platform services). */
bool disablePowerThrottling();
void* promoteAudioThread();
void revertAudioThread (void* handle);
} // namespace flub::app::platform_bridge
