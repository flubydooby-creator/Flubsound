#include "flub/neural/WakeEvent.h"

#include <algorithm>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#elif defined(__APPLE__)
    #include <mach/mach.h>
    #include <mach/semaphore.h>
    #include <mach/task.h>
#elif defined(__linux__)
    #include <linux/futex.h>
    #include <sys/syscall.h>
    #include <unistd.h>

    #include <ctime>
#endif

#if defined(FLUB_RTSAN) && defined(__has_include)
    #if __has_include(<sanitizer/rtsan_interface.h>)
        #include <sanitizer/rtsan_interface.h>
        #define FLUB_HAS_RTSAN_DISABLER 1
    #endif
#endif

namespace flub
{
#if defined(__linux__)
namespace
{
// The futex is the state word itself (the kernel compares it with kAsleep
// before it sleeps, so a signal between the announcement and the sleep is
// never lost).
static_assert (sizeof (std::atomic<uint32_t>) == sizeof (uint32_t) && std::atomic<uint32_t>::is_always_lock_free,
               "the futex word must be a plain 32-bit integer");

uint32_t* futexWord (std::atomic<uint32_t>& word) noexcept
{
    return reinterpret_cast<uint32_t*> (&word);
}
} // namespace
#endif

WakeEvent::WakeEvent() noexcept
{
#if defined(_WIN32)
    // Auto-reset, initially not set: a set event releases exactly one wait and resets.
    if (HANDLE h = CreateEventW (nullptr, FALSE, FALSE, nullptr))
    {
        native = reinterpret_cast<std::uintptr_t> (h);
        valid = true;
    }
#elif defined(__APPLE__)
    semaphore_t sem = MACH_PORT_NULL;
    if (semaphore_create (mach_task_self(), &sem, SYNC_POLICY_FIFO, 0) == KERN_SUCCESS)
    {
        native = static_cast<std::uintptr_t> (sem);
        valid = true;
    }
#elif defined(__linux__)
    (void) native; // unused: the futex is the state word, nothing to create
    valid = true;
#else
    (void) native; // no wake-up object on this OS: isValid() stays false and the owner polls
#endif
}

WakeEvent::~WakeEvent()
{
    if (! valid)
        return;
#if defined(_WIN32)
    CloseHandle (reinterpret_cast<HANDLE> (native));
#elif defined(__APPLE__)
    semaphore_destroy (mach_task_self(), static_cast<semaphore_t> (native));
#endif
}

void WakeEvent::signal() noexcept FLUB_NONBLOCKING
{
    if (! valid)
        return;
    // acq_rel: the work published before this call is visible to the waiter
    // that reads Signalled (release), and the read of Asleep is ordered after
    // the waiter's announcement.
    if (state.exchange (kSignalled, std::memory_order_acq_rel) == kAsleep)
        wakeInOs();
}

void WakeEvent::wakeInOs() noexcept
{
#if defined(FLUB_HAS_RTSAN_DISABLER)
    // Intentional RealtimeSanitizer exemption (see the header): the one call
    // below readies the sleeping waiter and returns. It takes no user-space
    // lock and never waits, but RTSan cannot tell it from a blocking system
    // call (on Linux it intercepts syscall()).
    __rtsan::ScopedDisabler wakeDoesNotBlock;
#endif
#if defined(_WIN32)
    SetEvent (reinterpret_cast<HANDLE> (native));
#elif defined(__APPLE__)
    semaphore_signal (static_cast<semaphore_t> (native));
#elif defined(__linux__)
    syscall (SYS_futex, futexWord (state), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
#endif
}

WakeEvent::WaitResult WakeEvent::wait (int timeoutMicroseconds) noexcept
{
    if (! valid)
        return WaitResult::TimedOut;

    // Announce the sleep. Failing that, a signal came while awake: take it
    // (an exchange, not a store, so a second signal racing this one is read
    // too and its work is visible).
    uint32_t expected = kIdle;
    if (! state.compare_exchange_strong (expected, kAsleep, std::memory_order_acq_rel, std::memory_order_acquire))
    {
        state.exchange (kIdle, std::memory_order_acq_rel);
        return WaitResult::Pending;
    }

    sleepInOs (std::max (1, timeoutMicroseconds));
    return state.exchange (kIdle, std::memory_order_acq_rel) == kSignalled ? WaitResult::Woken : WaitResult::TimedOut;
}

void WakeEvent::sleepInOs (int timeoutMicroseconds) noexcept
{
#if defined(_WIN32)
    const DWORD ms = static_cast<DWORD> ((timeoutMicroseconds + 999) / 1000);
    WaitForSingleObject (reinterpret_cast<HANDLE> (native), ms);
#elif defined(__APPLE__)
    mach_timespec_t timeout {};
    timeout.tv_sec = static_cast<unsigned int> (timeoutMicroseconds / 1000000);
    timeout.tv_nsec = static_cast<clock_res_t> ((timeoutMicroseconds % 1000000) * 1000);
    semaphore_timedwait (static_cast<semaphore_t> (native), timeout); // KERN_ABORTED: a spurious return, fine
#elif defined(__linux__)
    // Relative timeout on CLOCK_MONOTONIC. Returns at once (EAGAIN) if a
    // signal already changed the word; EINTR is a spurious return, fine.
    timespec timeout {};
    timeout.tv_sec = static_cast<time_t> (timeoutMicroseconds / 1000000);
    timeout.tv_nsec = static_cast<long> ((timeoutMicroseconds % 1000000) * 1000);
    syscall (SYS_futex, futexWord (state), FUTEX_WAIT_PRIVATE, kAsleep, &timeout, nullptr, 0);
#else
    (void) timeoutMicroseconds;
#endif
}
} // namespace flub
