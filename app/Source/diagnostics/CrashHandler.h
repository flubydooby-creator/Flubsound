// Flubsound Pro - crash reports (docs/11 E54).
//
// When the process crashes, a text report is written into the log folder
// (DiagnosticLog.h: <userDataFolder()>/Logs) before the OS takes over:
//
//   crash-<unix seconds>-<pid>.txt   build and system, the signal or
//                                    exception, the fault address and the
//                                    crashing thread's stack
//   crash-<unix seconds>-<pid>.dmp   Windows only: a minidump (MiniDumpNormal)
//
// POSIX (Linux, macOS): sigaction handlers for SIGSEGV, SIGBUS, SIGILL,
// SIGFPE and SIGABRT on an alternate signal stack (the arming thread's, so a
// stack overflow on the message thread is still reported). The handler is
// async-signal-safe: everything it needs (file name prefix, report header)
// is prepared beforehand, and it only calls open / write / close, time,
// getpid, backtrace / backtrace_symbols_fd (warmed up in prepare(), so the
// unwinder is loaded) and, on Linux, reads /proc/self/maps (the executable
// mappings, to symbolise the addresses with addr2line). macOS reports carry
// the main image's load address for atos. The handler then restores the
// default action and re-raises, so the OS still sees the crash (core file,
// crash reporter, exit status).
// Windows: SetUnhandledExceptionFilter (the exception code and address, the
// module + offset of every stack frame, a minidump through dbghelp's
// MiniDumpWriteDump, loaded at prepare()) and a SIGABRT handler for abort().
// Both chain to what was installed before.
// Everywhere: std::terminate records the uncaught exception's what() as the
// report's reason and aborts.
//
// A report is written without symbols; a release build's symbols (PDB,
// dSYM, the unstripped binary) turn the module + offset lines into
// functions and lines.
#pragma once

#include <juce_core/juce_core.h>

namespace flub::app::diagnostics::crash
{
struct Config
{
    juce::File folder;      // reports go here (created by prepare)
    juce::String header;    // written at the top of every report (build and system)
    int keepReports = 10;   // prepare() deletes older crash-* reports beyond this many
    /** 0: after writing the report, re-raise / continue the crash so the OS
        handles it. Non-zero: end the process with this exit code instead
        (tests: no core dump, no system crash reporter). */
    int exitCode = 0;
};

/** Everything the handler needs, allocated now (message thread, start-up).
    Returns false if the folder cannot be created. Can be called again to
    change the configuration (disarms first). */
bool prepare (const Config& config);
/** Installs the handlers (after prepare()). POSIX: only sigaction and
    sigaltstack, so a forked child may call it. */
void arm();
/** Restores the handlers that were installed before arm(). */
void disarm();
bool isArmed() noexcept;

/** prepare() + arm(). */
bool install (const Config& config);

/** crash-*.txt reports in `folder`, newest first. */
juce::Array<juce::File> findReports (const juce::File& folder);

#if JUCE_WINDOWS
/** Tests: writes a report and minidump the way the unhandled-exception
    filter does, for a synthetic access violation (a write at 0x10, raised
    at this function) on the calling thread; prepare() first. */
bool writeTestReport();
#endif
} // namespace flub::app::diagnostics::crash
