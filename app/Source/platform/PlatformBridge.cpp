#include "PlatformBridge.h"

#ifndef FLUB_HAS_PLATFORM_SERVICES
    #define FLUB_HAS_PLATFORM_SERVICES 0
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

std::unique_ptr<flub::platform::AppAudioRouter> createAppAudioRouter()
{
    return flub::platform::AppAudioRouter::create();
}

std::unique_ptr<flub::platform::ProcessLoopbackCapture> createProcessLoopbackCapture()
{
    return flub::platform::ProcessLoopbackCapture::create();
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
std::unique_ptr<flub::platform::AppAudioRouter> createAppAudioRouter() { return {}; }
std::unique_ptr<flub::platform::ProcessLoopbackCapture> createProcessLoopbackCapture() { return {}; }
bool disablePowerThrottling() { return false; }
void* promoteAudioThread() { return nullptr; }
void revertAudioThread (void*) {}

#endif

bool isProcessCaptureSupported()
{
    const auto probe = createProcessLoopbackCapture();
    return probe != nullptr && probe->isSupported();
}
} // namespace flub::app::platform_bridge
