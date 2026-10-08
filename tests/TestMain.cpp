#include "TestFramework.h"

#include <algorithm>
#include <cstdlib>
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
    #include <timeapi.h>
#endif

// ---- allocation counting ---------------------------------------------------
namespace
{
thread_local int64_t tlAllocations = 0;
}

int64_t flubtest::allocationCount() noexcept { return tlAllocations; }

void* operator new (std::size_t size)
{
    ++tlAllocations;
    if (void* p = std::malloc (size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}

void* operator new[] (std::size_t size)
{
    ++tlAllocations;
    if (void* p = std::malloc (size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}

namespace
{
// The Windows C runtimes (MSVC, MinGW) have no std::aligned_alloc; aligned
// blocks come from _aligned_malloc and must be freed with _aligned_free.
void* alignedAllocate (std::size_t size, std::size_t alignment) noexcept
{
#if defined(_WIN32)
    return _aligned_malloc (size == 0 ? 1 : size, alignment);
#else
    return std::aligned_alloc (alignment, (std::max<std::size_t> (size, 1) + alignment - 1) / alignment * alignment);
#endif
}

void alignedFree (void* p) noexcept
{
#if defined(_WIN32)
    _aligned_free (p);
#else
    std::free (p);
#endif
}
} // namespace

void* operator new (std::size_t size, std::align_val_t al)
{
    ++tlAllocations;
    if (void* p = alignedAllocate (size, static_cast<std::size_t> (al)))
        return p;
    throw std::bad_alloc();
}

void* operator new[] (std::size_t size, std::align_val_t al) { return operator new (size, al); }

void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }
void operator delete (void* p, std::align_val_t) noexcept { alignedFree (p); }
void operator delete[] (void* p, std::align_val_t) noexcept { alignedFree (p); }
void operator delete (void* p, std::size_t, std::align_val_t) noexcept { alignedFree (p); }
void operator delete[] (void* p, std::size_t, std::align_val_t) noexcept { alignedFree (p); }

// The nothrow forms too: libstdc++'s std::get_temporary_buffer (stable_sort,
// stable_partition, inplace_merge) allocates with new (nothrow) and frees
// with the sized delete, so a sanitizer's own nothrow new would otherwise
// pair with the free() above (ASan: alloc-dealloc-mismatch).
void* operator new (std::size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return operator new (size);
    }
    catch (...)
    {
        return nullptr;
    }
}
void* operator new[] (std::size_t size, const std::nothrow_t& tag) noexcept { return operator new (size, tag); }
void* operator new (std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept
{
    try
    {
        return operator new (size, al);
    }
    catch (...)
    {
        return nullptr;
    }
}
void* operator new[] (std::size_t size, std::align_val_t al, const std::nothrow_t& tag) noexcept { return operator new (size, al, tag); }
void operator delete (void* p, const std::nothrow_t&) noexcept { std::free (p); }
void operator delete[] (void* p, const std::nothrow_t&) noexcept { std::free (p); }
void operator delete (void* p, std::align_val_t, const std::nothrow_t&) noexcept { alignedFree (p); }
void operator delete[] (void* p, std::align_val_t, const std::nothrow_t&) noexcept { alignedFree (p); }

// ---- runner -----------------------------------------------------------------
int main (int argc, char** argv)
{
    const std::string filter = argc > 1 ? argv[1] : "";
    int run = 0, failedCases = 0;

#if defined(_WIN32)
    // A 1 ms system timer, as every JUCE process (the app, the plug-in's
    // hosts, flub_app_tests) has: otherwise a short sleep lasts 15.6 ms. The
    // neural worker's poll (AsyncModelProcessor.h) made the NeuralSlot tests
    // 13 s each without it; the worker is now woken by process(), but the
    // polling reference case and the paced cases' 10 ms device clock still
    // sleep.
    timeBeginPeriod (1);
#endif

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

    std::cout << "\n" << run - failedCases << "/" << run << " test cases passed" << std::endl;
    return failedCases;
}
