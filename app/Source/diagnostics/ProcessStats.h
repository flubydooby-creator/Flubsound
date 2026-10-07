// Flubsound Pro - this process's memory and CPU time, and the system's CPU
// time (docs/11 E53: the real-device soak's memory growth and load).
//
// One cheap read of OS counters, no JUCE, any thread:
//   Windows : GetProcessMemoryInfo (private bytes = commit charge, working
//             set), GetProcessTimes, GetSystemTimes
//   Linux   : /proc/self/status (RssAnon, VmRSS), getrusage, /proc/stat
//   macOS   : task_info (phys_footprint, resident size), getrusage; the
//             system's CPU time is not read (unknown)
// A value the platform does not give is negative. CPU percentages come from
// two reads: (cpu1 - cpu0) / wall for this process (100 % = one core), and
// (busy1 - busy0) / (total1 - total0) for the whole system (100 % = every
// core busy).
//
// Also attachToParentConsole(): the device soak's console output on Windows.
#pragma once

namespace flub::app::diagnostics
{
struct ProcessStats
{
    double privateBytes = -1.0;       // memory only this process uses (commit charge / anonymous / footprint)
    double workingSetBytes = -1.0;    // resident in RAM
    double cpuSeconds = -1.0;         // this process, user + kernel, all threads
    double systemBusySeconds = -1.0;  // every core, since boot
    double systemTotalSeconds = -1.0; // busy + idle
};

ProcessStats readProcessStats();

/** Windows: the app is a windowed (GUI-subsystem) program, which may be
    started with no standard output or error at all (nothing leads anywhere;
    a shell usually hands down its console, a redirection a file or a pipe).
    For its command-line modes (the device soak) this attaches to the console
    of the process that started it and points a stdout / stderr that has no
    handle at that console; a handle that leads somewhere stays as it is. An
    interactive prompt does not wait for a windowed program, so the text may
    appear under the next prompt. True when it attached. Elsewhere: nothing,
    false. */
bool attachToParentConsole();
} // namespace flub::app::diagnostics
