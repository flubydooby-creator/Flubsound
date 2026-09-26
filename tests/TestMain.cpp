#include "TestFramework.h"

#include <atomic>
#include <cstdlib>
#include <new>

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

void* operator new (std::size_t size, std::align_val_t al)
{
    ++tlAllocations;
    const auto a = static_cast<std::size_t> (al);
    if (void* p = std::aligned_alloc (a, (size + a - 1) / a * a))
        return p;
    throw std::bad_alloc();
}

void* operator new[] (std::size_t size, std::align_val_t al) { return operator new (size, al); }

void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }
void operator delete (void* p, std::align_val_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::align_val_t) noexcept { std::free (p); }
void operator delete (void* p, std::size_t, std::align_val_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t, std::align_val_t) noexcept { std::free (p); }

// ---- runner -----------------------------------------------------------------
int main (int argc, char** argv)
{
    const std::string filter = argc > 1 ? argv[1] : "";
    int run = 0, failedCases = 0;

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
