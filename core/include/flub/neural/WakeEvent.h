// Flubsound Pro - wakes one sleeping thread from any other thread, the audio
// thread included (AsyncModelProcessor's inference worker; docs/09 §1.1,
// "Waking the worker"; docs/11 E35).
//
// An auto-reset event for exactly one waiting thread. signal() makes the
// current or the next wait() return; signals that arrive while the waiter is
// awake collapse into one. The waiter therefore re-checks its work queue
// after every return (a return without new work is harmless).
//
//   signaller (e.g. the audio thread)            waiter (one thread)
//   publish the work (release)                   check the queue: empty
//   signal(): state := Signalled                 wait(): state Idle -> Asleep
//     was Asleep? one OS wake-up call   ─────►     sleep in the OS (timeout)
//     else nothing (the waiter will see it)        state := Idle, read why
//
// Real-time safety of signal(). It never waits: one atomic exchange, and only
// when the waiter is asleep one non-blocking OS call that readies it:
//   Windows  SetEvent on an auto-reset event,
//   Linux    futex (FUTEX_WAKE_PRIVATE) on the state word itself,
//   macOS    semaphore_signal on a Mach semaphore.
// None of them takes a user-space lock or waits for another thread; the
// kernel's own work is bounded (it readies one thread). This is how real-time
// audio code hands work to other threads (WASAPI's and Core Audio's I/O
// threads, JACK, PipeWire's data loop). RealtimeSanitizer cannot tell a wake
// from a blocking call (on Linux it sees a raw syscall()), so signal() exempts
// that one call (__rtsan::ScopedDisabler) on purpose; tests/test_rtsan.cpp
// shows that RTSan does flag the bare call, and every other test runs
// signal() under RTSan. The state word lets a signal skip the OS call while
// the waiter is awake, so the audio thread makes at most one call per sleep.
//
// No wake-up is ever lost: the waiter announces that it is going to sleep
// (Idle -> Asleep) with the same atomic the signaller exchanges, so either the
// signal comes first (the waiter does not sleep) or the signaller sees Asleep
// and makes the OS call, which the OS object keeps until the waiter's sleep
// consumes it (the event stays set, the semaphore counts, the futex wait
// checks the word). A signal racing a timeout can leave one stale token: the
// next wait() then returns at once, once.
//
// isValid() is false if the OS object could not be created (or on an OS
// without one of the three): the owner must then poll instead
// (AsyncModelProcessor falls back to its earlier poll).
#pragma once

#include "flub/common/Realtime.h"

#include <atomic>
#include <cstdint>

namespace flub
{
class WakeEvent
{
public:
    /** Non-RT: creates the OS object (none on Linux, where the futex is the state word). */
    WakeEvent() noexcept;
    ~WakeEvent();

    WakeEvent (const WakeEvent&) = delete;
    WakeEvent& operator= (const WakeEvent&) = delete;

    bool isValid() const noexcept { return valid; }

    /** Any thread, real-time included: wakes the waiter, or makes its next
        wait() return at once. Never blocks (see above). No-op if invalid. */
    void signal() noexcept FLUB_NONBLOCKING;

    enum class WaitResult
    {
        Pending, // a signal had arrived while the waiter was awake: returned without sleeping
        Woken,   // slept and was woken by a signal
        TimedOut // slept until the timeout (or woke spuriously, e.g. a stale token) with no signal
    };

    /** One thread only: returns once signalled or after timeoutMicroseconds
        (1 .. 2^31 - 1 us). Not real-time (it sleeps). Invalid: returns
        TimedOut at once. */
    WaitResult wait (int timeoutMicroseconds) noexcept;

private:
    void sleepInOs (int timeoutMicroseconds) noexcept;
    void wakeInOs() noexcept;

    static constexpr uint32_t kIdle = 0, kSignalled = 1, kAsleep = 2;
    std::atomic<uint32_t> state { kIdle }; // Linux: the futex word
    std::uintptr_t native = 0;             // Windows: the event HANDLE; macOS: the semaphore_t
    bool valid = false;
};
} // namespace flub
