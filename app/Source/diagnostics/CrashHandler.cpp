#include "CrashHandler.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <dbghelp.h>
#else
 #include <fcntl.h>
 #include <signal.h>
 #include <unistd.h>
 #include <ctime>
 #if __has_include(<execinfo.h>)
  #include <execinfo.h>
  #define FLUB_CRASH_HAVE_EXECINFO 1
 #else
  #define FLUB_CRASH_HAVE_EXECINFO 0
 #endif
 #if JUCE_MAC
  #include <mach-o/dyld.h>
 #endif
#endif

namespace flub::app::diagnostics::crash
{
namespace
{
//==============================================================================
// Allocation-free text output: fixed buffer, flushed to the report file.
class Writer
{
public:
   #if JUCE_WINDOWS
    explicit Writer (HANDLE f) noexcept : file (f) {}
   #else
    explicit Writer (int f) noexcept : fd (f) {}
   #endif
    ~Writer() { flush(); }

    Writer& raw (const char* text, size_t size) noexcept
    {
        for (size_t i = 0; i < size; ++i)
        {
            if (used == sizeof (buffer))
                flush();
            buffer[used++] = text[i];
        }
        return *this;
    }

    Writer& str (const char* text) noexcept { return text != nullptr ? raw (text, std::strlen (text)) : *this; }

    Writer& dec (unsigned long long value) noexcept
    {
        char digits[24];
        size_t n = 0;
        do
        {
            digits[n++] = static_cast<char> ('0' + static_cast<int> (value % 10));
            value /= 10;
        } while (value != 0 && n < sizeof (digits));
        while (n > 0)
            raw (&digits[--n], 1);
        return *this;
    }

    Writer& sdec (long long value) noexcept
    {
        if (value < 0)
            str ("-");
        return dec (value < 0 ? 0ULL - static_cast<unsigned long long> (value) : static_cast<unsigned long long> (value));
    }

    Writer& hex (unsigned long long value, int width = 16) noexcept
    {
        static constexpr char kDigits[] = "0123456789abcdef";
        str ("0x");
        for (int shift = (width - 1) * 4; shift >= 0; shift -= 4)
            raw (&kDigits[(value >> shift) & 0xf], 1);
        return *this;
    }

    Writer& address (const void* p) noexcept { return hex (static_cast<unsigned long long> (reinterpret_cast<uintptr_t> (p))); }

    void flush() noexcept
    {
        const char* data = buffer;
        size_t size = used;
        used = 0;
       #if JUCE_WINDOWS
        while (size > 0)
        {
            DWORD written = 0;
            if (! WriteFile (file, data, static_cast<DWORD> (size), &written, nullptr) || written == 0)
                return;
            data += written;
            size -= written;
        }
       #else
        while (size > 0)
        {
            const auto n = ::write (fd, data, size);
            if (n <= 0)
                return; // best effort
            data += n;
            size -= static_cast<size_t> (n);
        }
       #endif
    }

private:
   #if JUCE_WINDOWS
    HANDLE file;
   #else
    int fd;
   #endif
    char buffer[1024];
    size_t used = 0;
};

/** Copies a C string into a fixed buffer, always terminated. */
template <size_t N>
void copyText (char (&dest)[N], const char* source) noexcept
{
    size_t i = 0;
    for (; source != nullptr && source[i] != 0 && i + 1 < N; ++i)
        dest[i] = source[i];
    dest[i] = 0;
}

template <size_t N>
void appendText (char (&dest)[N], const char* source) noexcept
{
    size_t start = std::strlen (dest);
    for (size_t i = 0; source != nullptr && source[i] != 0 && start + 1 < N; ++i)
        dest[start++] = source[i];
    dest[start] = 0;
}

//==============================================================================
struct State
{
    bool prepared = false, armed = false;
    int exitCode = 0;
    char prefix[1024] {};  // "<folder>/crash-" (UTF-8)
    char header[4096] {};
    char reason[512] {};   // std::terminate's exception text
    std::terminate_handler previousTerminate = nullptr;
   #if JUCE_WINDOWS
    using MiniDumpWriteDumpFn = BOOL (WINAPI*) (HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                                PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    wchar_t prefixW[1024] {};
    MiniDumpWriteDumpFn miniDumpWriteDump = nullptr;
    LPTOP_LEVEL_EXCEPTION_FILTER previousFilter = nullptr;
    void (*previousAbort) (int) = nullptr;
   #else
    static constexpr int kSignals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    static constexpr size_t kNumSignals = sizeof (kSignals) / sizeof (kSignals[0]);
    static constexpr size_t kAltStackBytes = 64 * 1024; // SIGSTKSZ is not a constant on newer glibc
    struct sigaction previous[kNumSignals] {};
    void* altStack = nullptr; // allocated once, kept for the process lifetime
    stack_t previousAltStack {};
    bool altStackInstalled = false;
   #endif
};

State state;
std::atomic_flag handling = ATOMIC_FLAG_INIT; // the first crash writes; a second one goes straight to the OS

[[noreturn]] void onTerminate() noexcept
{
    // Not a signal handler: find out what was thrown, keep it for the report
    // and abort (the SIGABRT handler writes the report).
    char text[sizeof (state.reason)] = "std::terminate called without an active exception";
    try
    {
        if (auto current = std::current_exception())
            std::rethrow_exception (current);
    }
    catch (const std::exception& e)
    {
        copyText (text, "uncaught exception: ");
        appendText (text, e.what());
    }
    catch (...)
    {
        copyText (text, "uncaught exception of an unknown type");
    }
    copyText (state.reason, text);
    std::abort();
}

void deleteOldReports (const juce::File& folder, int keep)
{
    const auto reports = findReports (folder);
    for (int i = juce::jmax (0, keep); i < reports.size(); ++i)
    {
        reports[i].withFileExtension ("dmp").deleteFile();
        reports[i].deleteFile();
    }
}

//==============================================================================
#if JUCE_WINDOWS
void appendWide (wchar_t* dest, size_t capacity, const wchar_t* text) noexcept
{
    size_t n = 0;
    while (n < capacity && dest[n] != 0)
        ++n;
    for (size_t i = 0; text[i] != 0 && n + 1 < capacity; ++i)
        dest[n++] = text[i];
    if (n < capacity)
        dest[n] = 0;
}

void appendWideNumber (wchar_t* dest, size_t capacity, unsigned long long value) noexcept
{
    wchar_t digits[24];
    size_t n = 0;
    do
    {
        digits[n++] = static_cast<wchar_t> (L'0' + static_cast<int> (value % 10));
        value /= 10;
    } while (value != 0 && n < 23);
    wchar_t text[24];
    for (size_t i = 0; i < n; ++i)
        text[i] = digits[n - 1 - i];
    text[n] = 0;
    appendWide (dest, capacity, text);
}

const char* exceptionName (DWORD code) noexcept
{
    switch (code)
    {
        case EXCEPTION_ACCESS_VIOLATION: return "access violation";
        case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
        case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
        case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
        case EXCEPTION_BREAKPOINT: return "breakpoint";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned data";
        case 0xC0000409: return "stack buffer overrun / fail-fast";
        case 0xC0000374: return "heap corruption";
        case 0xE06D7363: return "C++ exception";
        default: return "exception";
    }
}

/** "Module.dll + 0x1234" (file name only: the folder can contain the user
    name), or "?" outside every module. */
void writeModuleAndOffset (Writer& w, const void* address) noexcept
{
    HMODULE module = nullptr;
    if (! GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR> (address), &module)
        || module == nullptr)
    {
        w.str ("?");
        return;
    }
    wchar_t path[MAX_PATH] {};
    const DWORD length = GetModuleFileNameW (module, path, MAX_PATH);
    const wchar_t* name = path;
    for (DWORD i = 0; i < length; ++i)
        if (path[i] == L'\\' || path[i] == L'/')
            name = path + i + 1;
    char utf8[MAX_PATH * 3] {};
    if (WideCharToMultiByte (CP_UTF8, 0, name, -1, utf8, static_cast<int> (sizeof (utf8)), nullptr, nullptr) <= 0)
        copyText (utf8, "?");
    const auto offset = reinterpret_cast<uintptr_t> (address) - reinterpret_cast<uintptr_t> (module);
    w.str (utf8).str (" + ").hex (static_cast<unsigned long long> (offset), 8);
}

struct DumpRequest
{
    HANDLE file;
    DWORD threadId;
    EXCEPTION_POINTERS* exception;
    BOOL ok;
};

DWORD WINAPI dumpThread (LPVOID parameter)
{
    // On its own thread: a fresh stack (the crashing thread may have
    // overflowed its own), and the crashing thread is dumped from outside.
    auto& request = *static_cast<DumpRequest*> (parameter);
    MINIDUMP_EXCEPTION_INFORMATION info {};
    info.ThreadId = request.threadId;
    info.ExceptionPointers = request.exception;
    info.ClientPointers = FALSE;
    request.ok = state.miniDumpWriteDump (GetCurrentProcess(), GetCurrentProcessId(), request.file, MiniDumpNormal,
                                          request.exception != nullptr ? &info : nullptr, nullptr, nullptr);
    return 0;
}

bool writeReport (EXCEPTION_POINTERS* exception, const char* what) noexcept
{
    FILETIME now {};
    GetSystemTimeAsFileTime (&now);
    ULARGE_INTEGER ticks {};
    ticks.LowPart = now.dwLowDateTime;
    ticks.HighPart = now.dwHighDateTime;
    const auto unixSeconds = (ticks.QuadPart - 116444736000000000ULL) / 10000000ULL;
    const auto pid = GetCurrentProcessId();

    wchar_t path[1200] {};
    appendWide (path, 1200, state.prefixW);
    appendWideNumber (path, 1200, unixSeconds);
    appendWide (path, 1200, L"-");
    appendWideNumber (path, 1200, pid);
    const size_t stemLength = wcslen (path);
    appendWide (path, 1200, L".txt");

    const HANDLE file = CreateFileW (path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    {
        Writer w (file);
        w.str ("Flubsound Pro crash report\r\n\r\n").str (state.header).str ("\r\n");
        if (exception != nullptr && exception->ExceptionRecord != nullptr)
        {
            const auto& record = *exception->ExceptionRecord;
            w.str ("Exception: ").hex (record.ExceptionCode, 8).str (" (").str (exceptionName (record.ExceptionCode)).str (")\r\n");
            w.str ("Address: ").address (record.ExceptionAddress).str (" (");
            writeModuleAndOffset (w, record.ExceptionAddress);
            w.str (")\r\n");
            if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2)
            {
                const auto kind = record.ExceptionInformation[0];
                w.str ("Access: ").str (kind == 0 ? "read" : kind == 1 ? "write" : "execute").str (" at ")
                    .hex (static_cast<unsigned long long> (record.ExceptionInformation[1])).str ("\r\n");
            }
        }
        else
        {
            w.str ("Exception: ").str (what != nullptr ? what : "unknown").str ("\r\n");
        }
        w.str ("Time: ").dec (unixSeconds).str (" (Unix time, UTC)\r\n");
        w.str ("Process: ").dec (pid).str ("  thread ").dec (GetCurrentThreadId()).str ("\r\n");
        if (state.reason[0] != 0)
            w.str ("Reason: ").str (state.reason).str ("\r\n");
        w.str ("\r\nStack of the crashing thread (innermost first; the minidump has the full context):\r\n");
        void* frames[62] {};
        const USHORT count = CaptureStackBackTrace (0, 62, frames, nullptr);
        for (USHORT i = 0; i < count; ++i)
        {
            w.str ("  #").dec (i).str (" ").address (frames[i]).str ("  ");
            writeModuleAndOffset (w, frames[i]);
            w.str ("\r\n");
        }
        w.str ("\r\nMinidump: same name, .dmp\r\n");
    }
    CloseHandle (file);

    // The minidump last: on a damaged heap it may fail, the text report is
    // already on disk.
    if (state.miniDumpWriteDump != nullptr)
    {
        path[stemLength] = 0;
        appendWide (path, 1200, L".dmp");
        const HANDLE dump = CreateFileW (path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (dump != INVALID_HANDLE_VALUE)
        {
            DumpRequest request { dump, GetCurrentThreadId(), exception, FALSE };
            if (const HANDLE thread = CreateThread (nullptr, 0, dumpThread, &request, 0, nullptr); thread != nullptr)
            {
                WaitForSingleObject (thread, 30000);
                CloseHandle (thread);
            }
            CloseHandle (dump);
        }
    }
    return true;
}

LONG WINAPI onUnhandledException (EXCEPTION_POINTERS* exception)
{
    if (! handling.test_and_set())
        writeReport (exception, nullptr);
    if (state.exitCode != 0)
        TerminateProcess (GetCurrentProcess(), static_cast<UINT> (state.exitCode));
    return state.previousFilter != nullptr ? state.previousFilter (exception) : EXCEPTION_CONTINUE_SEARCH;
}

void onAbort (int)
{
    if (! handling.test_and_set())
        writeReport (nullptr, "abort()");
    if (state.exitCode != 0)
        TerminateProcess (GetCurrentProcess(), static_cast<UINT> (state.exitCode));
    // Returning lets abort() end the process as it would have.
}

#else
//==============================================================================
const char* signalName (int sig) noexcept
{
    switch (sig)
    {
        case SIGSEGV: return "SIGSEGV (invalid memory access)";
        case SIGBUS: return "SIGBUS (bus error)";
        case SIGILL: return "SIGILL (illegal instruction)";
        case SIGFPE: return "SIGFPE (arithmetic error)";
        case SIGABRT: return "SIGABRT (abort)";
        default: return "signal";
    }
}

#if JUCE_LINUX || JUCE_BSD
/** Copies the executable mappings of /proc/self/maps (address range, offset
    and file): with them, addr2line resolves the stack's addresses. */
void writeExecutableMappings (Writer& w) noexcept
{
    const int maps = ::open ("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (maps < 0)
        return;
    w.str ("\nExecutable mappings (/proc/self/maps):\n");
    char chunk[2048];
    char line[512];
    size_t lineLength = 0;
    for (;;)
    {
        const auto n = ::read (maps, chunk, sizeof (chunk));
        if (n <= 0)
            break;
        for (ssize_t i = 0; i < n; ++i)
        {
            const char c = chunk[i];
            if (c != '\n')
            {
                if (lineLength < sizeof (line))
                    line[lineLength++] = c;
                continue;
            }
            // "start-end perms offset dev inode path": keep "r-xp" mappings.
            size_t space = 0;
            while (space < lineLength && line[space] != ' ')
                ++space;
            if (space + 5 <= lineLength && std::memcmp (line + space + 1, "r-xp", 4) == 0)
                w.str ("  ").raw (line, lineLength).str ("\n");
            lineLength = 0;
        }
    }
    ::close (maps);
}
#endif

void writeReport (int sig, const siginfo_t* info) noexcept
{
    const auto now = static_cast<unsigned long long> (::time (nullptr));
    const auto pid = static_cast<unsigned long long> (::getpid());

    char path[1200];
    {
        // "<prefix><seconds>-<pid>.txt" without snprintf (not async-signal-safe).
        char number[24];
        auto format = [&number] (unsigned long long v)
        {
            char digits[24];
            size_t n = 0;
            do
            {
                digits[n++] = static_cast<char> ('0' + static_cast<int> (v % 10));
                v /= 10;
            } while (v != 0 && n < 23);
            for (size_t i = 0; i < n; ++i)
                number[i] = digits[n - 1 - i];
            number[n] = 0;
            return number;
        };
        copyText (path, state.prefix);
        appendText (path, format (now));
        appendText (path, "-");
        appendText (path, format (pid));
        appendText (path, ".txt");
    }

    const int fd = ::open (path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    {
        Writer w (fd);
        w.str ("Flubsound Pro crash report\n\n").str (state.header).str ("\n");
        w.str ("Signal: ").dec (static_cast<unsigned long long> (sig)).str (" ").str (signalName (sig));
        if (info != nullptr)
            w.str (", code ").sdec (info->si_code);
        w.str ("\n");
        if (info != nullptr && (info->si_code == SI_USER || info->si_code <= 0))
            w.str ("Sent by: process ").dec (static_cast<unsigned long long> (info->si_pid)).str (" (kill / raise)\n");
        else if (info != nullptr)
            w.str ("Fault address: ").address (info->si_addr).str ("\n");
        w.str ("Time: ").dec (now).str (" (Unix time, UTC)\n");
        w.str ("Process: ").dec (pid).str ("\n");
        if (state.reason[0] != 0)
            w.str ("Reason: ").str (state.reason).str ("\n");
        w.str ("\nStack of the crashing thread (innermost first):\n");
        w.flush();
       #if FLUB_CRASH_HAVE_EXECINFO
        void* frames[64];
        const int count = ::backtrace (frames, 64);
        ::backtrace_symbols_fd (frames, count, fd);
       #else
        w.str ("  (no backtrace support in this C library)\n");
       #endif
       #if JUCE_LINUX || JUCE_BSD
        writeExecutableMappings (w);
       #endif
    }
    ::close (fd);
}

void onSignal (int sig, siginfo_t* info, void*)
{
    if (! handling.test_and_set())
        writeReport (sig, info);
    if (state.exitCode != 0)
        ::_exit (state.exitCode);
    // The default action, so the OS still sees the crash (core file, crash
    // reporter, exit status). A fault re-executes and ends the process; a
    // raised signal is delivered when the handler returns.
    struct sigaction defaultAction {};
    defaultAction.sa_handler = SIG_DFL;
    sigemptyset (&defaultAction.sa_mask);
    ::sigaction (sig, &defaultAction, nullptr);
    ::raise (sig);
}
#endif
} // namespace

//==============================================================================
juce::Array<juce::File> findReports (const juce::File& folder)
{
    auto files = folder.findChildFiles (juce::File::findFiles, false, "crash-*.txt");
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
    {
        const auto ta = a.getLastModificationTime(), tb = b.getLastModificationTime();
        return ta != tb ? ta > tb : a.getFileName() > b.getFileName();
    });
    return files;
}

bool prepare (const Config& config)
{
    disarm();
    state.prepared = false;
    if (! config.folder.createDirectory())
        return false;
    deleteOldReports (config.folder, config.keepReports);

    const auto prefix = config.folder.getChildFile ("crash-").getFullPathName();
    copyText (state.prefix, prefix.toRawUTF8());
    auto header = config.header.trimEnd() + "\n";
   #if JUCE_MAC
    // atos -o <binary> -l <load address> <stack addresses>
    header << "Main image: " << juce::String::fromUTF8 (_dyld_get_image_name (0)) << " loaded at 0x"
           << juce::String::toHexString (static_cast<juce::int64> (reinterpret_cast<uintptr_t> (_dyld_get_image_header (0)))) << "\n";
   #endif
   #if JUCE_WINDOWS
    header = header.replace ("\n", "\r\n");
   #endif
    copyText (state.header, header.toRawUTF8());
    state.reason[0] = 0;
    state.exitCode = config.exitCode;

   #if JUCE_WINDOWS
    const auto wide = prefix.toWideCharPointer();
    size_t i = 0;
    for (; wide[i] != 0 && i + 1 < sizeof (state.prefixW) / sizeof (wchar_t); ++i)
        state.prefixW[i] = wide[i];
    state.prefixW[i] = 0;
    if (state.miniDumpWriteDump == nullptr)
        if (const HMODULE dbghelp = LoadLibraryW (L"dbghelp.dll"); dbghelp != nullptr) // kept loaded
            state.miniDumpWriteDump = reinterpret_cast<State::MiniDumpWriteDumpFn> (
                reinterpret_cast<void (*)()> (GetProcAddress (dbghelp, "MiniDumpWriteDump")));
   #else
    if (state.altStack == nullptr)
        state.altStack = std::malloc (State::kAltStackBytes);
   #if FLUB_CRASH_HAVE_EXECINFO
    // The first backtrace() loads the unwinder (dlopen, malloc): do it now,
    // not in the signal handler.
    void* frames[4];
    ::backtrace (frames, 4);
   #endif
   #endif
    state.prepared = true;
    return true;
}

void arm()
{
    if (! state.prepared || state.armed)
        return;
    handling.clear();
   #if JUCE_WINDOWS
    state.previousFilter = SetUnhandledExceptionFilter (onUnhandledException);
    state.previousAbort = std::signal (SIGABRT, onAbort);
   #else
    if (state.altStack != nullptr)
    {
        stack_t stack {};
        stack.ss_sp = state.altStack;
        stack.ss_size = State::kAltStackBytes;
        stack.ss_flags = 0;
        state.altStackInstalled = ::sigaltstack (&stack, &state.previousAltStack) == 0;
    }
    for (size_t i = 0; i < State::kNumSignals; ++i)
    {
        struct sigaction action {};
        action.sa_sigaction = onSignal;
        sigemptyset (&action.sa_mask);
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        ::sigaction (State::kSignals[i], &action, &state.previous[i]);
    }
   #endif
    state.previousTerminate = std::set_terminate (onTerminate);
    state.armed = true;
}

void disarm()
{
    if (! state.armed)
        return;
   #if JUCE_WINDOWS
    SetUnhandledExceptionFilter (state.previousFilter);
    std::signal (SIGABRT, state.previousAbort != nullptr ? state.previousAbort : SIG_DFL);
   #else
    for (size_t i = 0; i < State::kNumSignals; ++i)
        ::sigaction (State::kSignals[i], &state.previous[i], nullptr);
    if (state.altStackInstalled)
        ::sigaltstack (&state.previousAltStack, nullptr);
    state.altStackInstalled = false;
   #endif
    std::set_terminate (state.previousTerminate);
    state.armed = false;
}

bool isArmed() noexcept
{
    return state.armed;
}

bool install (const Config& config)
{
    if (! prepare (config))
        return false;
    arm();
    return true;
}

#if JUCE_WINDOWS
bool writeTestReport()
{
    if (! state.prepared)
        return false;
    CONTEXT context {};
    RtlCaptureContext (&context);
    EXCEPTION_RECORD record {};
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.ExceptionAddress = reinterpret_cast<void*> (&writeTestReport);
    record.NumberParameters = 2;
    record.ExceptionInformation[0] = 1; // write
    record.ExceptionInformation[1] = 0x10;
    EXCEPTION_POINTERS pointers { &record, &context };
    return writeReport (&pointers, nullptr);
}
#endif
} // namespace flub::app::diagnostics::crash
