// Flubsound Pro - helpers shared by the app-level tests (flub_app_tests).
//
// RealtimeProbe counts, on the CALLING thread only, heap allocations and
// frees (global operator new / delete are replaced in AppTestMain.cpp) and,
// on Linux / glibc, pthread mutex locks (pthread_mutex_lock / _trylock are
// interposed there). Other threads (JUCE's timer thread, the routing worker)
// never disturb a count. Wrap exactly the code that must be real-time safe:
//
//   flubapptest::RealtimeProbe probe;
//   host.audioDeviceIOCallbackWithContext (...);
//   CHECK (probe.allocations() == 0);
#pragma once

#include "TestFramework.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>

namespace flubapptest
{
int64_t deallocationCount() noexcept;
int64_t lockCount() noexcept;
/** True when lockCount() really counts (Linux / glibc builds). */
bool lockCountingAvailable() noexcept;

class RealtimeProbe
{
public:
    RealtimeProbe() noexcept
        : allocStart (flubtest::allocationCount()), freeStart (deallocationCount()), lockStart (lockCount())
    {
    }

    int64_t allocations() const noexcept { return flubtest::allocationCount() - allocStart; }
    int64_t deallocations() const noexcept { return deallocationCount() - freeStart; }
    int64_t locks() const noexcept { return lockCount() - lockStart; }

private:
    int64_t allocStart, freeStart, lockStart;
};

/** Dispatches messages on this (the message) thread until `done` returns
    true. The timeout is only a hang guard; returns false if it expired. */
inline bool pumpMessagesUntil (const std::function<bool()>& done, int timeoutMs = 10000)
{
    auto* mm = juce::MessageManager::getInstance();
    const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32> (timeoutMs);
    while (! done())
    {
        if (juce::Time::getMillisecondCounter() > deadline)
            return false;
        mm->runDispatchLoopUntil (2);
    }
    return true;
}

/** A fresh, empty temporary folder that is deleted with this object. */
class TempFolder
{
public:
    TempFolder()
        : folder (juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("flub_app_tests", {}, false))
    {
        folder.createDirectory();
    }

    ~TempFolder() { folder.deleteRecursively(); }

    juce::File file (const char* name) const { return folder.getChildFile (name); }
    juce::File file (const juce::String& name) const { return folder.getChildFile (name); }

private:
    juce::File folder;

    JUCE_DECLARE_NON_COPYABLE (TempFolder)
};

inline float gainToDb (float gain) noexcept
{
    return 20.0f * std::log10 (std::max (gain, 1.0e-9f));
}
} // namespace flubapptest
