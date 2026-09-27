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

/** True if a capture object can be created and reports isSupported(). Creates
    and destroys a probe object: call from the message thread, not per block. */
bool isProcessCaptureSupported();

/** SystemTuning wrappers (no-ops without platform services). */
bool disablePowerThrottling();
void* promoteAudioThread();
void revertAudioThread (void* handle);
} // namespace flub::app::platform_bridge
