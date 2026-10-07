#include "ProcessStats.h"

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <psapi.h>
#elif defined(__APPLE__)
    #include <mach/mach.h>
    #include <sys/resource.h>
#else
    #include <cstdio>
    #include <cstring>
    #include <sys/resource.h>
    #include <unistd.h>
#endif

namespace flub::app::diagnostics
{
#if defined(_WIN32)
namespace
{
double seconds (const FILETIME& t) noexcept
{
    ULARGE_INTEGER v;
    v.LowPart = t.dwLowDateTime;
    v.HighPart = t.dwHighDateTime;
    return static_cast<double> (v.QuadPart) * 1.0e-7; // 100 ns units
}
} // namespace

ProcessStats readProcessStats()
{
    ProcessStats s;
    PROCESS_MEMORY_COUNTERS_EX memory {};
    memory.cb = sizeof (memory);
    if (K32GetProcessMemoryInfo (GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*> (&memory), sizeof (memory)))
    {
        s.privateBytes = static_cast<double> (memory.PrivateUsage);
        s.workingSetBytes = static_cast<double> (memory.WorkingSetSize);
    }
    FILETIME created {}, exited {}, kernel {}, user {};
    if (GetProcessTimes (GetCurrentProcess(), &created, &exited, &kernel, &user))
        s.cpuSeconds = seconds (kernel) + seconds (user);
    FILETIME idle {}, systemKernel {}, systemUser {};
    if (GetSystemTimes (&idle, &systemKernel, &systemUser))
    {
        // The kernel time includes the idle time.
        s.systemTotalSeconds = seconds (systemKernel) + seconds (systemUser);
        s.systemBusySeconds = s.systemTotalSeconds - seconds (idle);
    }
    return s;
}

#elif defined(__APPLE__)

ProcessStats readProcessStats()
{
    ProcessStats s;
    task_vm_info_data_t vm {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info (mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t> (&vm), &count) == KERN_SUCCESS)
    {
        s.privateBytes = static_cast<double> (vm.phys_footprint);
        s.workingSetBytes = static_cast<double> (vm.resident_size);
    }
    rusage usage {};
    if (getrusage (RUSAGE_SELF, &usage) == 0)
        s.cpuSeconds = static_cast<double> (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec)
                       + 1.0e-6 * static_cast<double> (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec);
    return s;
}

#else

namespace
{
/** "<key>   1234 kB" in /proc/self/status, in bytes; < 0 when absent. */
double statusKiB (const char* text, const char* key) noexcept
{
    const char* at = std::strstr (text, key);
    if (at == nullptr)
        return -1.0;
    long long kib = -1;
    if (std::sscanf (at + std::strlen (key), " %lld", &kib) != 1)
        return -1.0;
    return 1024.0 * static_cast<double> (kib);
}
} // namespace

ProcessStats readProcessStats()
{
    ProcessStats s;
    if (std::FILE* f = std::fopen ("/proc/self/status", "r"))
    {
        char text[8192] {};
        const size_t n = std::fread (text, 1, sizeof (text) - 1, f);
        std::fclose (f);
        text[n] = '\0';
        s.privateBytes = statusKiB (text, "RssAnon:");
        s.workingSetBytes = statusKiB (text, "VmRSS:");
    }
    rusage usage {};
    if (getrusage (RUSAGE_SELF, &usage) == 0)
        s.cpuSeconds = static_cast<double> (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec)
                       + 1.0e-6 * static_cast<double> (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec);
    if (std::FILE* f = std::fopen ("/proc/stat", "r"))
    {
        // cpu  user nice system idle iowait irq softirq steal (clock ticks)
        unsigned long long v[8] {};
        if (std::fscanf (f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) == 8)
        {
            const long ticksPerSecond = sysconf (_SC_CLK_TCK);
            const double tick = 1.0 / static_cast<double> (ticksPerSecond > 0 ? ticksPerSecond : 100);
            const double idle = static_cast<double> (v[3] + v[4]);
            double total = 0.0;
            for (const auto t : v)
                total += static_cast<double> (t);
            s.systemTotalSeconds = total * tick;
            s.systemBusySeconds = (total - idle) * tick;
        }
        std::fclose (f);
    }
    return s;
}

#endif
} // namespace flub::app::diagnostics
