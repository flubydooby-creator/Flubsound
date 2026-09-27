// Flubsound Pro - runner for the app-level tests (flub_app_tests).
//
//   flub_app_tests [substring-filter]      exit code = number of failed cases
//
// Same TEST_CASE registry as flub_tests (tests/TestFramework.h), plus:
//   * JUCE is initialised once (juce::ScopedJuceInitialiser_GUI: a message
//     manager, no window) and this thread is the message thread;
//   * global operator new / delete count per thread (flubapptest::RealtimeProbe);
//   * Linux / glibc: pthread_mutex_lock / _trylock are interposed and counted
//     per thread, so a probe also sees juce::CriticalSection, std::mutex and
//     any other pthread mutex taken on the probed thread;
//   * FLUB_USER_DATA_DIR (and XDG_CONFIG_HOME on Linux) point at a temporary
//     folder, so the app's user data folder (user presets, device-profile
//     override) is empty and the real user's files are never read or written.
//     Its name is not ASCII (like a temp folder under a non-ASCII Windows user
//     name), and the run stops unless userDataFolder() resolves to exactly
//     that folder.
#include "AppTestSupport.h"

#include "settings/UserDataFolder.h"

#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(_WIN32)
    #include <malloc.h>
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#endif

#if defined(__linux__) && defined(__GLIBC__)
    #include <dlfcn.h>
    #include <pthread.h>
    #define FLUB_APP_TESTS_COUNT_LOCKS 1
#else
    #define FLUB_APP_TESTS_COUNT_LOCKS 0
#endif

// ---- allocation counting ---------------------------------------------------
namespace
{
thread_local int64_t tlAllocations = 0;
thread_local int64_t tlDeallocations = 0;
thread_local int64_t tlLocks = 0;

void* allocate (std::size_t size)
{
    ++tlAllocations;
    if (void* p = std::malloc (size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}

void release (void* p) noexcept
{
    if (p != nullptr)
        ++tlDeallocations;
    std::free (p);
}

// The Windows C runtimes (MSVC, MinGW) have no std::aligned_alloc; aligned
// blocks come from _aligned_malloc and must be freed with _aligned_free.
void* alignedAllocate (std::size_t size, std::size_t alignment)
{
    ++tlAllocations;
#if defined(_WIN32)
    void* p = _aligned_malloc (size == 0 ? 1 : size, alignment);
#else
    void* p = std::aligned_alloc (alignment, (std::max<std::size_t> (size, 1) + alignment - 1) / alignment * alignment);
#endif
    if (p == nullptr)
        throw std::bad_alloc();
    return p;
}

void alignedRelease (void* p) noexcept
{
    if (p != nullptr)
        ++tlDeallocations;
#if defined(_WIN32)
    _aligned_free (p);
#else
    std::free (p);
#endif
}
} // namespace

int64_t flubtest::allocationCount() noexcept { return tlAllocations; }
int64_t flubapptest::deallocationCount() noexcept { return tlDeallocations; }
int64_t flubapptest::lockCount() noexcept { return tlLocks; }
bool flubapptest::lockCountingAvailable() noexcept { return FLUB_APP_TESTS_COUNT_LOCKS != 0; }

void* operator new (std::size_t size) { return allocate (size); }
void* operator new[] (std::size_t size) { return allocate (size); }
void* operator new (std::size_t size, std::align_val_t al) { return alignedAllocate (size, static_cast<std::size_t> (al)); }
void* operator new[] (std::size_t size, std::align_val_t al) { return alignedAllocate (size, static_cast<std::size_t> (al)); }

void operator delete (void* p) noexcept { release (p); }
void operator delete[] (void* p) noexcept { release (p); }
void operator delete (void* p, std::size_t) noexcept { release (p); }
void operator delete[] (void* p, std::size_t) noexcept { release (p); }
void operator delete (void* p, std::align_val_t) noexcept { alignedRelease (p); }
void operator delete[] (void* p, std::align_val_t) noexcept { alignedRelease (p); }
void operator delete (void* p, std::size_t, std::align_val_t) noexcept { alignedRelease (p); }
void operator delete[] (void* p, std::size_t, std::align_val_t) noexcept { alignedRelease (p); }

// ---- lock counting (Linux / glibc) --------------------------------------------
#if FLUB_APP_TESTS_COUNT_LOCKS
namespace
{
using MutexFunction = int (*) (pthread_mutex_t*);

// Resolved lazily without a function-local static (its guard could itself
// lock). dlsym does not call the public pthread_mutex_lock.
MutexFunction nextMutexFunction (std::atomic<MutexFunction>& cache, const char* name) noexcept
{
    auto fn = cache.load (std::memory_order_acquire);
    if (fn == nullptr)
    {
        void* symbol = dlsym (RTLD_NEXT, name);
        std::memcpy (&fn, &symbol, sizeof (fn));
        cache.store (fn, std::memory_order_release);
    }
    return fn;
}

std::atomic<MutexFunction> realMutexLock { nullptr }, realMutexTrylock { nullptr };
} // namespace

extern "C" int pthread_mutex_lock (pthread_mutex_t* mutex) noexcept
{
    ++tlLocks;
    return nextMutexFunction (realMutexLock, "pthread_mutex_lock") (mutex);
}

extern "C" int pthread_mutex_trylock (pthread_mutex_t* mutex) noexcept
{
    ++tlLocks;
    return nextMutexFunction (realMutexTrylock, "pthread_mutex_trylock") (mutex);
}
#endif

// ---- runner -----------------------------------------------------------------
int main (int argc, char** argv)
{
    const std::string filter = argc > 1 ? argv[1] : "";
    int run = 0, failedCases = 0;

    juce::ScopedJuceInitialiser_GUI juceInitialiser; // this thread becomes the message thread

    // The app's user data folder (settings, user presets, device-profile
    // override) points at a temporary folder on every OS. The name has
    // characters outside ASCII and outside most ANSI code pages ("Flubsound
    // e-acute + Japanese 'data'"), so a narrow / code-page read of the
    // variable cannot pass the check below.
    const flubapptest::TempFolder configHome;
    const auto dataFolder = configHome.file (juce::String (juce::CharPointer_UTF8 ("Flubsound \xc3\xa9\xe3\x83\x87\xe3\x83\xbc\xe3\x82\xbf")));
    const auto dataDir = dataFolder.getFullPathName();
   #if JUCE_WINDOWS
    // The CRT copy (_wgetenv) and the OS environment block, which
    // userDataFolder() reads through GetEnvironmentVariableW.
    _wputenv_s (L"FLUB_USER_DATA_DIR", dataDir.toWideCharPointer());
    SetEnvironmentVariableW (L"FLUB_USER_DATA_DIR", dataDir.toWideCharPointer());
   #else
    setenv ("FLUB_USER_DATA_DIR", dataDir.toRawUTF8(), 1);
    setenv ("XDG_CONFIG_HOME", configHome.file ("config").getFullPathName().toRawUTF8(), 1);
   #endif
    if (flub::app::userDataFolder() != dataFolder)
    {
        std::cout << "FLUB_USER_DATA_DIR is not read back as set: userDataFolder() = "
                  << flub::app::userDataFolder().getFullPathName().toStdString() << ", expected " << dataDir.toStdString() << std::endl;
        return 1;
    }

    const auto started = std::chrono::steady_clock::now();
    for (const auto& tc : flubtest::registry())
    {
        if (! filter.empty() && tc.name.find (filter) == std::string::npos)
            continue;
        ++run;
        const int before = flubtest::currentFailures();
        std::cout << "[ RUN  ] " << tc.name << std::endl;
        try
        {
            tc.fn();
        }
        catch (const flubtest::Failure&)
        {
        }
        catch (const std::exception& e)
        {
            flubtest::reportFailure (tc.file, tc.line, std::string ("uncaught exception: ") + e.what());
        }
        if (flubtest::currentFailures() != before)
        {
            ++failedCases;
            std::cout << "[ FAIL ] " << tc.name << std::endl;
        }
        else
        {
            std::cout << "[  OK  ] " << tc.name << std::endl;
        }
    }

    const auto seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - started).count();
    std::cout << "\n" << run - failedCases << "/" << run << " test cases passed (" << seconds << " s)" << std::endl;
    return failedCases;
}
