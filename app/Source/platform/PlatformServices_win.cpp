// Flubsound Pro - Windows implementation of PlatformServices.h
//
// Plain Win32 / COM only: no JUCE, no WRL, no ATL, so this translation unit
// builds with MSVC, clang-cl and MinGW-w64 alike and can be unit-tested
// outside the app.
//
// Libraries the app must link on Windows
// ---------------------------------------
//   ole32     CoInitializeEx, CoCreateInstance, CoTaskMemFree, PropVariantClear
//   user32    message-only window, RegisterHotKey / UnregisterHotKey
//   shell32   ShellExecuteW (ms-settings: fallback page)
//   shlwapi   SHLoadIndirectString ("@%SystemRoot%\...,-202" session names)
//   version   GetFileVersionInfoW / VerQueryValueW (app "FileDescription")
//   avrt      AvSetMmThreadCharacteristicsW / AvSetMmThreadPriority (MMCSS)
//   mmdevapi  ActivateAudioInterfaceAsync (MSVC: Mmdevapi.lib, MinGW: -lmmdevapi)
//   advapi32  RegCreateKeyExW / RegSetValueExW / RegDeleteValueW (start with Windows)
// NOT needed: uuid (every IID comes from __uuidof or a local GUID constant) and
// runtimeobject/combase (the optional WinRT entry points of the undocumented
// routing adapter are resolved at run time from combase.dll).
//
//   CMake:  target_link_libraries (<app> PRIVATE ole32 user32 shell32 shlwapi version avrt mmdevapi advapi32)
//   MSVC additionally picks them up from the #pragma comment(lib) lines below.
//
// Threading
// ---------
//   GlobalHotkeys   : create / use / destroy on the app's message thread.
//   AppAudioRouter  : any thread (each call initialises COM for its duration);
//                     calls block for a few ms - prefer a background thread.
//   ProcessLoopbackCapture : start/stop from any non-RT thread; frames arrive
//                     on an internal MMCSS "Pro Audio" thread.
//   SystemTuning    : promote/revert must be called on the thread concerned.
//   AutoStart       : any thread (registry calls only; message thread in the app).
//   ForegroundApp   : one thread (the message thread in the app); user32 only.
#if defined(_WIN32)

#include "PlatformServices.h"
#include "PlatformServicesInternal.h"

#ifndef NOMINMAX
    #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif

// clang-format off
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <winver.h>
// clang-format on

#if __has_include(<audioclientactivationparams.h>)
    #include <audioclientactivationparams.h>
    #define FLUB_HAVE_SDK_ACTIVATION_PARAMS 1
#else
    #define FLUB_HAVE_SDK_ACTIVATION_PARAMS 0
#endif

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
    #pragma comment(lib, "ole32.lib")
    #pragma comment(lib, "user32.lib")
    #pragma comment(lib, "shell32.lib")
    #pragma comment(lib, "shlwapi.lib")
    #pragma comment(lib, "version.lib")
    #pragma comment(lib, "avrt.lib")
    #pragma comment(lib, "mmdevapi.lib")
    #pragma comment(lib, "advapi32.lib")
#endif

//==============================================================================
// Process-loopback activation types (Windows SDK 10.0.20348+,
// <audioclientactivationparams.h>). Older SDKs and current MinGW-w64 lack the
// header, so we declare the identical ABI locally. Values are copied from the
// Windows SDK and must not be changed.
#if ! FLUB_HAVE_SDK_ACTIVATION_PARAMS
typedef enum AUDIOCLIENT_ACTIVATION_TYPE
{
    AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0,
    AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1
} AUDIOCLIENT_ACTIVATION_TYPE;

typedef enum PROCESS_LOOPBACK_MODE
{
    PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
    PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1
} PROCESS_LOOPBACK_MODE;

typedef struct AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS
{
    DWORD TargetProcessId;
    PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
} AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS;

typedef struct AUDIOCLIENT_ACTIVATION_PARAMS
{
    AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
    union
    {
        AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
    };
} AUDIOCLIENT_ACTIVATION_PARAMS;

static_assert (sizeof (AUDIOCLIENT_ACTIVATION_PARAMS) == 12, "must match the Windows SDK layout");
#endif

#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
    #define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
    #define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
    #define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif
#ifndef MOD_NOREPEAT
    #define MOD_NOREPEAT 0x4000
#endif

// Power throttling (EcoQoS) - Windows 10 1709+. Guarded for older SDKs; the
// enum value and struct layout are fixed by the OS ABI.
#ifdef PROCESS_POWER_THROTTLING_CURRENT_VERSION
    #define FLUB_POWER_THROTTLING_VERSION PROCESS_POWER_THROTTLING_CURRENT_VERSION
    #define FLUB_POWER_THROTTLING_EXECUTION_SPEED PROCESS_POWER_THROTTLING_EXECUTION_SPEED
    #define FLUB_PROCESS_POWER_THROTTLING_CLASS ProcessPowerThrottling
#else
    #define FLUB_POWER_THROTTLING_VERSION 1
    #define FLUB_POWER_THROTTLING_EXECUTION_SPEED 0x1
    #define FLUB_PROCESS_POWER_THROTTLING_CLASS static_cast<PROCESS_INFORMATION_CLASS> (4)
#endif
#ifdef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
    #define FLUB_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#else
    #define FLUB_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

namespace flub::platform
{
namespace
{
//==============================================================================
// Small COM / Win32 helpers
//==============================================================================

/** Minimal owning COM pointer (we deliberately avoid WRL/ATL). */
template <typename T>
class ComPtr
{
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }

    ComPtr (const ComPtr&) = delete;
    ComPtr& operator= (const ComPtr&) = delete;

    ComPtr (ComPtr&& other) noexcept : ptr (std::exchange (other.ptr, nullptr)) {}

    ComPtr& operator= (ComPtr&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            ptr = std::exchange (other.ptr, nullptr);
        }
        return *this;
    }

    T* get() const noexcept { return ptr; }
    T* operator->() const noexcept { return ptr; }
    explicit operator bool() const noexcept { return ptr != nullptr; }

    /** Releases the current object and returns the slot for an out-parameter. */
    T** put() noexcept
    {
        reset();
        return &ptr;
    }

    void** putVoid() noexcept { return reinterpret_cast<void**> (put()); }

    void reset() noexcept
    {
        if (ptr != nullptr)
            std::exchange (ptr, nullptr)->Release();
    }

private:
    T* ptr = nullptr;
};

/** Initialises COM on the calling thread for the lifetime of the object.
    If the thread already lives in the other apartment type (RPC_E_CHANGED_MODE)
    COM is still usable and we simply must not uninitialise it. The default is
    STA so that calling this from the GUI thread never fights with
    OleInitialize (drag & drop) done by JUCE. */
class ScopedComInit
{
public:
    explicit ScopedComInit (DWORD model = COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)
    {
        result = CoInitializeEx (nullptr, model);
        mustUninitialise = SUCCEEDED (result); // S_OK and S_FALSE must both be balanced
    }

    ~ScopedComInit()
    {
        if (mustUninitialise)
            CoUninitialize();
    }

    ScopedComInit (const ScopedComInit&) = delete;
    ScopedComInit& operator= (const ScopedComInit&) = delete;

    bool isUsable() const noexcept { return SUCCEEDED (result) || result == RPC_E_CHANGED_MODE; }
    HRESULT status() const noexcept { return result; }

private:
    HRESULT result = E_FAIL;
    bool mustUninitialise = false;
};

/** Owns a string returned through CoTaskMemAlloc (GetId, GetDisplayName...). */
struct CoTaskString
{
    LPWSTR text = nullptr;

    CoTaskString() = default;
    ~CoTaskString() { CoTaskMemFree (text); }
    CoTaskString (const CoTaskString&) = delete;
    CoTaskString& operator= (const CoTaskString&) = delete;
};

/** Owns a kernel HANDLE. */
struct ScopedHandle
{
    HANDLE handle = nullptr;

    explicit ScopedHandle (HANDLE h = nullptr) noexcept : handle (h) {}
    ~ScopedHandle() { reset(); }
    ScopedHandle (const ScopedHandle&) = delete;
    ScopedHandle& operator= (const ScopedHandle&) = delete;

    void reset (HANDLE h = nullptr) noexcept
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
            CloseHandle (handle);
        handle = h;
    }
};

/** Converts a GetProcAddress result to a typed function pointer. memcpy of the
    pointer representation sidesteps -Wcast-function-type(-strict) on GCC and
    Clang without any pedantic function/object pointer casts. */
template <typename Fn>
Fn loadFunction (HMODULE module, const char* name) noexcept
{
    static_assert (sizeof (Fn) == sizeof (FARPROC), "function pointer size mismatch");

    Fn function = nullptr;
    if (module != nullptr)
    {
        const FARPROC address = GetProcAddress (module, name);
        std::memcpy (&function, &address, sizeof (function));
    }
    return function;
}

std::string toUtf8 (const wchar_t* text, size_t length)
{
    if (text == nullptr || length == 0 || length > static_cast<size_t> (INT_MAX))
        return {};

    const int wideLength = static_cast<int> (length);
    const int bytes = WideCharToMultiByte (CP_UTF8, 0, text, wideLength, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
        return {};

    std::string out (static_cast<size_t> (bytes), '\0');
    WideCharToMultiByte (CP_UTF8, 0, text, wideLength, out.data(), bytes, nullptr, nullptr);
    return out;
}

std::string toUtf8 (const std::wstring& text) { return toUtf8 (text.data(), text.size()); }
std::string toUtf8 (const wchar_t* text) { return text != nullptr ? toUtf8 (text, wcslen (text)) : std::string(); }

std::wstring toWide (const std::string& text)
{
    if (text.empty() || text.size() > static_cast<size_t> (INT_MAX))
        return {};

    const int length = static_cast<int> (text.size());
    const int chars = MultiByteToWideChar (CP_UTF8, 0, text.data(), length, nullptr, 0);
    if (chars <= 0)
        return {};

    std::wstring out (static_cast<size_t> (chars), L'\0');
    MultiByteToWideChar (CP_UTF8, 0, text.data(), length, out.data(), chars);
    return out;
}

/** "HRESULT 0x88890004 (AUDCLNT_E_DEVICE_INVALIDATED)" or the system message. */
std::string hresultToString (HRESULT hr)
{
    struct Known
    {
        HRESULT code;
        const char* name;
    };

    static const Known known[] = {
        { AUDCLNT_E_NOT_INITIALIZED, "AUDCLNT_E_NOT_INITIALIZED" },
        { AUDCLNT_E_ALREADY_INITIALIZED, "AUDCLNT_E_ALREADY_INITIALIZED" },
        { AUDCLNT_E_WRONG_ENDPOINT_TYPE, "AUDCLNT_E_WRONG_ENDPOINT_TYPE" },
        { AUDCLNT_E_DEVICE_INVALIDATED, "AUDCLNT_E_DEVICE_INVALIDATED" },
        { AUDCLNT_E_NOT_STOPPED, "AUDCLNT_E_NOT_STOPPED" },
        { AUDCLNT_E_BUFFER_TOO_LARGE, "AUDCLNT_E_BUFFER_TOO_LARGE" },
        { AUDCLNT_E_OUT_OF_ORDER, "AUDCLNT_E_OUT_OF_ORDER" },
        { AUDCLNT_E_UNSUPPORTED_FORMAT, "AUDCLNT_E_UNSUPPORTED_FORMAT" },
        { AUDCLNT_E_INVALID_SIZE, "AUDCLNT_E_INVALID_SIZE" },
        { AUDCLNT_E_DEVICE_IN_USE, "AUDCLNT_E_DEVICE_IN_USE" },
        { AUDCLNT_E_BUFFER_OPERATION_PENDING, "AUDCLNT_E_BUFFER_OPERATION_PENDING" },
        { AUDCLNT_E_THREAD_NOT_REGISTERED, "AUDCLNT_E_THREAD_NOT_REGISTERED" },
        { AUDCLNT_E_ENDPOINT_CREATE_FAILED, "AUDCLNT_E_ENDPOINT_CREATE_FAILED" },
        { AUDCLNT_E_SERVICE_NOT_RUNNING, "AUDCLNT_E_SERVICE_NOT_RUNNING" },
        { AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED, "AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED" },
        { AUDCLNT_E_INVALID_STREAM_FLAG, "AUDCLNT_E_INVALID_STREAM_FLAG" },
        { AUDCLNT_E_EVENTHANDLE_NOT_SET, "AUDCLNT_E_EVENTHANDLE_NOT_SET" },
        { AUDCLNT_E_BUFFER_SIZE_ERROR, "AUDCLNT_E_BUFFER_SIZE_ERROR" },
        { AUDCLNT_E_RESOURCES_INVALIDATED, "AUDCLNT_E_RESOURCES_INVALIDATED" },
        { E_NOINTERFACE, "E_NOINTERFACE" },
        { E_NOTIMPL, "E_NOTIMPL" },
        { E_INVALIDARG, "E_INVALIDARG" },
        { E_ACCESSDENIED, "E_ACCESSDENIED" },
        { E_OUTOFMEMORY, "E_OUTOFMEMORY" },
        { E_POINTER, "E_POINTER" },
        { E_UNEXPECTED, "E_UNEXPECTED" },
        { E_FAIL, "E_FAIL" },
        { REGDB_E_CLASSNOTREG, "REGDB_E_CLASSNOTREG" },
        { CO_E_NOTINITIALIZED, "CO_E_NOTINITIALIZED" },
        { RPC_E_CHANGED_MODE, "RPC_E_CHANGED_MODE" },
    };

    char code[32];
    std::snprintf (code, sizeof (code), "HRESULT 0x%08lX", static_cast<unsigned long> (hr));
    std::string text (code);

    for (const auto& k : known)
    {
        if (k.code == hr)
            return text + " (" + k.name + ")";
    }

    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW (FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                         nullptr,
                                         static_cast<DWORD> (hr),
                                         0,
                                         reinterpret_cast<LPWSTR> (&message),
                                         0,
                                         nullptr);
    if (message != nullptr)
    {
        auto utf8 = toUtf8 (message, length);
        LocalFree (message);

        while (! utf8.empty() && (utf8.back() == '\n' || utf8.back() == '\r' || utf8.back() == ' ' || utf8.back() == '.'))
            utf8.pop_back();

        if (! utf8.empty())
            text += " (" + utf8 + ")";
    }

    return text;
}

/** The real OS build number. GetVersionEx lies to un-manifested processes, so
    ask ntdll's RtlGetVersion (exported, documented for drivers, stable). */
DWORD windowsBuildNumber()
{
    static const DWORD build = []
    {
        using RtlGetVersionFn = LONG (WINAPI*) (OSVERSIONINFOW*); // RTL_OSVERSIONINFOW has the same layout

        if (auto rtlGetVersion = loadFunction<RtlGetVersionFn> (GetModuleHandleW (L"ntdll.dll"), "RtlGetVersion"))
        {
            OSVERSIONINFOW info {};
            info.dwOSVersionInfoSize = sizeof (info);
            if (rtlGetVersion (&info) == 0)
                return info.dwBuildNumber;
        }
        return DWORD { 0 };
    }();

    return build;
}

/** Process loopback capture exists since Windows 10 build 20348 (Server 2022)
    and in every Windows 11 build; Windows 10 21H2/22H2 (1904x) lack it. */
constexpr DWORD kFirstProcessLoopbackBuild = 20348;

/** The per-app endpoint API changed its interface id in build 21390. */
[[maybe_unused]] constexpr DWORD kAudioPolicyConfigIidChangeBuild = 21390;

std::wstring processImagePath (DWORD processId)
{
    ScopedHandle process (OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
    if (process.handle == nullptr)
        return {};

    std::wstring path (32768, L'\0');
    auto size = static_cast<DWORD> (path.size());
    if (! QueryFullProcessImageNameW (process.handle, 0, path.data(), &size))
        return {};

    path.resize (size);
    return path;
}

std::wstring fileNameOf (const std::wstring& path)
{
    const auto slash = path.find_last_of (L"\\/");
    return slash == std::wstring::npos ? path : path.substr (slash + 1);
}

std::wstring withoutExtension (const std::wstring& name)
{
    const auto dot = name.find_last_of (L'.');
    return dot == std::wstring::npos || dot == 0 ? name : name.substr (0, dot);
}

/** The "FileDescription" version resource ("Counter-Strike 2", "Spotify") -
    what the Windows volume mixer shows for sessions without a display name. */
std::wstring fileDescription (const std::wstring& path)
{
    if (path.empty())
        return {};

    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW (path.c_str(), &ignored);
    if (size == 0)
        return {};

    std::vector<BYTE> data (size);
    if (! GetFileVersionInfoW (path.c_str(), 0, size, data.data()))
        return {};

    struct LangCodePage
    {
        WORD language;
        WORD codePage;
    };

    std::vector<LangCodePage> candidates;
    LangCodePage* translations = nullptr;
    UINT translationBytes = 0;
    if (VerQueryValueW (data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**> (&translations), &translationBytes)
        && translations != nullptr)
    {
        candidates.assign (translations, translations + translationBytes / sizeof (LangCodePage));
    }
    candidates.push_back ({ 0x0409, 0x04B0 }); // en-US, Unicode: the most common fallback

    const auto hex4 = [] (unsigned value)
    {
        static constexpr wchar_t digits[] = L"0123456789abcdef";
        std::wstring s (4, L'0');
        for (int i = 3; i >= 0; --i, value >>= 4)
            s[static_cast<size_t> (i)] = digits[value & 0xfu];
        return s;
    };

    for (const auto& c : candidates)
    {
        const auto key = L"\\StringFileInfo\\" + hex4 (c.language) + hex4 (c.codePage) + L"\\FileDescription";
        wchar_t* value = nullptr;
        UINT chars = 0;
        if (VerQueryValueW (data.data(), key.c_str(), reinterpret_cast<void**> (&value), &chars) && value != nullptr && chars > 1)
        {
            std::wstring description (value, wcsnlen (value, chars));
            while (! description.empty() && description.back() == L' ')
                description.pop_back();
            if (! description.empty())
                return description;
        }
    }

    return {};
}

// PKEY_Device_FriendlyName (functiondiscoverykeys_devpkey.h), defined locally
// so we do not depend on INITGUID / uuid.lib.
const PROPERTYKEY kDeviceFriendlyNameKey = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

std::wstring endpointId (IMMDevice* device)
{
    CoTaskString id;
    if (device == nullptr || FAILED (device->GetId (&id.text)) || id.text == nullptr)
        return {};
    return id.text;
}

std::wstring endpointFriendlyName (IMMDevice* device)
{
    ComPtr<IPropertyStore> properties;
    if (device == nullptr || FAILED (device->OpenPropertyStore (STGM_READ, properties.put())))
        return {};

    PROPVARIANT value;
    PropVariantInit (&value);
    std::wstring name;
    if (SUCCEEDED (properties->GetValue (kDeviceFriendlyNameKey, &value)) && value.vt == VT_LPWSTR && value.pwszVal != nullptr)
        name = value.pwszVal;
    PropVariantClear (&value);
    return name;
}

ComPtr<IMMDeviceEnumerator> createDeviceEnumerator (std::string& error)
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    const HRESULT hr =
        CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof (IMMDeviceEnumerator), enumerator.putVoid());
    if (FAILED (hr))
        error = "Cannot create the Windows audio device enumerator: " + hresultToString (hr);
    return enumerator;
}

/** Accepts an MMDevice endpoint id ("{0.0.0.00000000}.{guid}") or the exact
    friendly name of an active render endpoint (e.g. "Flubsound Game (Flubsound
    Virtual Audio)") and returns the verified endpoint id. */
[[maybe_unused]] bool resolveRenderEndpoint (IMMDeviceEnumerator* enumerator,
                                             const std::string& idOrName,
                                             std::wstring& resolvedId,
                                             std::string& error)
{
    const auto wanted = toWide (idOrName);
    if (wanted.empty())
    {
        error = "Empty or invalid output device id.";
        return false;
    }

    // 1) Direct lookup by endpoint id.
    {
        ComPtr<IMMDevice> device;
        if (SUCCEEDED (enumerator->GetDevice (wanted.c_str(), device.put())) && device)
        {
            DWORD state = 0;
            ComPtr<IMMEndpoint> endpoint;
            EDataFlow flow = eAll;
            if (SUCCEEDED (device->QueryInterface (__uuidof (IMMEndpoint), endpoint.putVoid())) && SUCCEEDED (endpoint->GetDataFlow (&flow))
                && flow != eRender)
            {
                error = "'" + idOrName + "' is not an output device.";
                return false;
            }

            if (FAILED (device->GetState (&state)) || state != DEVICE_STATE_ACTIVE)
            {
                error = "Output device '" + idOrName + "' is disabled or unplugged.";
                return false;
            }

            resolvedId = wanted;
            return true;
        }
    }

    // 2) Lookup by friendly name among the active render endpoints.
    ComPtr<IMMDeviceCollection> devices;
    UINT count = 0;
    if (SUCCEEDED (enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, devices.put())) && SUCCEEDED (devices->GetCount (&count)))
    {
        for (UINT i = 0; i < count; ++i)
        {
            ComPtr<IMMDevice> device;
            if (FAILED (devices->Item (i, device.put())))
                continue;

            const auto name = endpointFriendlyName (device.get());
            const int nameLength = static_cast<int> (name.size());
            const int wantedLength = static_cast<int> (wanted.size());
            if (! name.empty() && CompareStringOrdinal (name.c_str(), nameLength, wanted.c_str(), wantedLength, TRUE) == CSTR_EQUAL)
            {
                resolvedId = endpointId (device.get());
                return ! resolvedId.empty();
            }
        }
    }

    error = "Output device '" + idOrName + "' was not found.";
    return false;
}

//==============================================================================
// GlobalHotkeys
//==============================================================================
/*  Design: RegisterHotKey() posts WM_HOTKEY to the message queue of the thread
    that owns the target window. We create a hidden *message-only* window
    (parent HWND_MESSAGE: never visible, not enumerated, receives no
    broadcasts) on the calling thread - the app's message thread - and let its
    window procedure dispatch the callbacks.

    No extra message loop is needed: JUCE's Windows message loop
    (juce_Messaging_windows.cpp, dispatchNextMessage) calls
    GetMessage (&m, nullptr, 0, 0) - i.e. with hwnd == nullptr, which retrieves
    messages for *every* window created by the thread - followed by
    TranslateMessage / DispatchMessage. DispatchMessage routes our WM_HOTKEY to
    windowProc() below, so callbacks run on the message thread exactly as
    the interface promises. The same holds for modal loops and any other
    standard Win32 message pump running on that thread.

    Caller ids are mapped to private native ids in the application range
    0x0001-0xBFFF required by RegisterHotKey. MOD_NOREPEAT stops auto-repeat
    from firing the callback continuously while the chord is held. */
class WinGlobalHotkeys final : public GlobalHotkeys
{
public:
    WinGlobalHotkeys() : ownerThread (GetCurrentThreadId())
    {
        const HINSTANCE instance = thisModule();

        WNDCLASSEXW wc {};
        wc.cbSize = sizeof (wc);
        wc.lpfnWndProc = &WinGlobalHotkeys::windowProc;
        wc.hInstance = instance;
        wc.lpszClassName = kWindowClassName;

        // Registering twice (second instance) fails with ERROR_CLASS_ALREADY_EXISTS, which is fine.
        if (RegisterClassExW (&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return;

        window = CreateWindowExW (0, kWindowClassName, L"Flubsound hotkeys", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
    }

    ~WinGlobalHotkeys() override
    {
        unregisterAll();

        if (window != nullptr)
        {
            SetWindowLongPtrW (window, GWLP_USERDATA, 0);
            DestroyWindow (window);
        }
    }

    using GlobalHotkeys::registerHotkey;
    bool isSupported() const override { return window != nullptr; }

    /** Windows keeps no list of global shortcuts: 'description' is unused. */
    bool registerHotkey (int id, const KeyChord& chord, const std::string&, std::function<void()> callback) override
    {
        const bool ok = registerChord (id, chord, std::move (callback));
        reportBinding (id, ok ? BindingResult::Status::Registered : BindingResult::Status::Unavailable);
        return ok;
    }

    void unregisterHotkey (int id) override
    {
        const auto it = entries.find (id);
        if (it == entries.end())
            return;

        if (window != nullptr)
            UnregisterHotKey (window, it->second.nativeId);
        entries.erase (it);
    }

    void unregisterAll() override
    {
        for (const auto& [id, entry] : entries)
        {
            if (window != nullptr)
                UnregisterHotKey (window, entry.nativeId);
        }
        entries.clear();
    }

private:
    bool registerChord (int id, const KeyChord& chord, std::function<void()> callback)
    {
        // Win32 hotkeys and the window are thread-affine.
        if (window == nullptr || GetCurrentThreadId() != ownerThread || ! callback || ! detail::isValidChord (chord))
            return false;

        // "The F12 key is reserved for use by the debugger at all times, so it
        //  should not be registered as a hot key." (RegisterHotKey docs)
        if (chord.keyCode == detail::kFunctionKeyBase + 11 && chord.modifiers == KeyChord::None)
            return false;

        // Rebinding: release the old chord first (it may be the same chord),
        // but put it back if the new one cannot be registered, so a failed
        // rebind never silently loses a working shortcut.
        const auto previous = entries.find (id);
        if (previous != entries.end())
            UnregisterHotKey (window, previous->second.nativeId);

        const int nativeId = allocateNativeId();
        if (nativeId == 0
            || ! RegisterHotKey (window, nativeId, toWin32Modifiers (chord.modifiers) | MOD_NOREPEAT, chord.keyCode))
        {
            // Typically ERROR_HOTKEY_ALREADY_REGISTERED: another app owns this chord.
            if (previous != entries.end()
                && ! RegisterHotKey (window,
                                     previous->second.nativeId,
                                     toWin32Modifiers (previous->second.chord.modifiers) | MOD_NOREPEAT,
                                     previous->second.chord.keyCode))
                entries.erase (previous); // lost in the meantime: do not keep a dead entry
            return false;
        }

        entries[id] = Entry { nativeId, chord, std::move (callback) };
        return true;
    }

    struct Entry
    {
        int nativeId = 0;
        KeyChord chord;
        std::function<void()> callback;
    };

    static constexpr const wchar_t* kWindowClassName = L"FlubsoundGlobalHotkeyWindow";
    static constexpr int kMaxNativeId = 0xBFFF;

    static HINSTANCE thisModule()
    {
        // The module containing this code (the .exe, or a DLL if we are ever
        // hosted in one), so the window class belongs to the right module.
        static const int marker = 0;
        HMODULE module = nullptr;
        GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR> (&marker),
                            &module);
        return module != nullptr ? module : GetModuleHandleW (nullptr);
    }

    static UINT toWin32Modifiers (uint32_t modifiers)
    {
        UINT flags = 0;
        if ((modifiers & KeyChord::Ctrl) != 0)
            flags |= MOD_CONTROL;
        if ((modifiers & KeyChord::Alt) != 0)
            flags |= MOD_ALT;
        if ((modifiers & KeyChord::Shift) != 0)
            flags |= MOD_SHIFT;
        if ((modifiers & KeyChord::Super) != 0)
            flags |= MOD_WIN;
        return flags;
    }

    int allocateNativeId()
    {
        for (int attempt = 0; attempt < kMaxNativeId; ++attempt)
        {
            nextNativeId = nextNativeId >= kMaxNativeId ? 1 : nextNativeId + 1;
            const bool inUse =
                std::any_of (entries.begin(), entries.end(), [this] (const auto& e) { return e.second.nativeId == nextNativeId; });
            if (! inUse)
                return nextNativeId;
        }
        return 0;
    }

    void dispatch (int nativeId)
    {
        for (const auto& [id, entry] : entries)
        {
            if (entry.nativeId == nativeId)
            {
                // Copy first: the callback may unregister (and destroy) itself.
                const auto callback = entry.callback;
                try
                {
                    callback();
                }
                catch (...)
                {
                    // Never let an exception unwind through the OS window procedure.
                }
                return;
            }
        }
    }

    static LRESULT CALLBACK windowProc (HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*> (lParam);
            SetWindowLongPtrW (hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR> (create->lpCreateParams));
        }
        else if (message == WM_HOTKEY)
        {
            if (auto* self = reinterpret_cast<WinGlobalHotkeys*> (GetWindowLongPtrW (hwnd, GWLP_USERDATA)))
            {
                self->dispatch (static_cast<int> (wParam));
                return 0;
            }
        }

        return DefWindowProcW (hwnd, message, wParam, lParam);
    }

    const DWORD ownerThread;
    HWND window = nullptr;
    int nextNativeId = 0;
    std::map<int, Entry> entries;
};

//==============================================================================
// Undocumented per-application default endpoint adapter
//==============================================================================
#if defined(FLUB_ENABLE_UNDOCUMENTED_ROUTING)
/*  !!! UNDOCUMENTED WINDOWS API - OPT-IN ONLY (FLUB_ENABLE_UNDOCUMENTED_ROUTING) !!!

    Windows has NO documented API to choose the output device of another
    application. Settings > System > Sound > Volume mixer (Win11) / "App volume
    and device preferences" (Win10 1803+) uses the internal WinRT class
    "Windows.Media.Internal.AudioPolicyConfig", whose activation factory
    implements IAudioPolicyConfigFactory:

        SetPersistedDefaultAudioEndpoint (UINT processId, EDataFlow, ERole, HSTRING deviceInterfacePath)

    The setting is persisted per executable by the audio service, and running
    streams that use the default device are moved to the new endpoint.

    The interface layout used below is the one published by the open-source
    EarTrumpet project (MIT, Interop/MMDeviceAPI/IAudioPolicyConfigFactory.cs)
    and used by several other tools; it is not published by Microsoft:
        IUnknown (3) + IInspectable (3) + 19 methods we never call
        (add_CtxVolumeChange ... remove_ChatContextChanged), then
        SetPersistedDefaultAudioEndpoint, GetPersistedDefaultAudioEndpoint,
        ClearAllPersistedApplicationDefaultEndpoints.

    The interface id is BUILD DEPENDENT:
        builds <  21390 : {2a59116d-6c4f-45e0-a74f-707e3fef9258}
        builds >= 21390 : {ab3d4648-e242-459f-b02f-541c70306324}
    The factory is requested *by IID*, so a build whose interface changed again
    (and therefore - by COM rules - got a new IID) fails cleanly with
    E_NOINTERFACE instead of calling into a wrong vtable slot. That is the
    only safety net: the vtable slot index itself is an assumption.

    Risks / obligations:
      - Microsoft may change or remove this at any Windows update; it must be
        validated on EVERY supported Windows build (CI matrix: Win10 22H2
        19045, Win11 23H2 22631, Win11 24H2 26100 and each new feature update)
        before a release enables FLUB_ENABLE_UNDOCUMENTED_ROUTING.
      - A wrong vtable layout would crash the calling process, so this runs
        only on explicit user action, never automatically at start-up.
      - The device argument is a device *interface path*, not the MMDevice id:
        "\\?\SWD#MMDEVAPI#<mmdevice id>#{e6327cad-dcec-4949-ae8a-991e976a79d2}"
        (render interface class GUID); a null HSTRING clears the override.
      - Store policy / anti-cheat products do not care, but some enterprise
        lockdowns block the class (RoGetActivationFactory fails) -> fallback.
    On any failure the caller gets an error and the UI falls back to
    openSystemRoutingSettings(). */
namespace undocumented_routing
{
struct OpaqueHString;
using HStringHandle = OpaqueHString*; // ABI-identical to HSTRING (an opaque pointer)

using RoGetActivationFactoryFn = HRESULT (WINAPI*) (HStringHandle, REFIID, void**);
using WindowsCreateStringFn = HRESULT (WINAPI*) (LPCWSTR, UINT32, HStringHandle*);
using WindowsDeleteStringFn = HRESULT (WINAPI*) (HStringHandle);

/** Explicit C-style vtable so the assumed slot index is visible and checked. */
struct AudioPolicyConfigFactoryVtbl
{
    // IUnknown
    HRESULT (STDMETHODCALLTYPE* QueryInterface) (void* self, REFIID riid, void** object);
    ULONG (STDMETHODCALLTYPE* AddRef) (void* self);
    ULONG (STDMETHODCALLTYPE* Release) (void* self);
    // IInspectable (never called)
    void* GetIids;
    void* GetRuntimeClassName;
    void* GetTrustLevel;
    // add_CtxVolumeChange ... remove_ChatContextChanged (never called)
    void* notUsed[19];
    HRESULT (STDMETHODCALLTYPE* SetPersistedDefaultAudioEndpoint) (void* self, UINT pid, EDataFlow, ERole, HStringHandle deviceId);
    HRESULT (STDMETHODCALLTYPE* GetPersistedDefaultAudioEndpoint) (void* self, UINT pid, EDataFlow, ERole, HStringHandle* deviceId);
    HRESULT (STDMETHODCALLTYPE* ClearAllPersistedApplicationDefaultEndpoints) (void* self);
};

static_assert (offsetof (AudioPolicyConfigFactoryVtbl, SetPersistedDefaultAudioEndpoint) == 25 * sizeof (void*),
               "SetPersistedDefaultAudioEndpoint is assumed to be vtable slot 25");

struct AudioPolicyConfigFactory
{
    const AudioPolicyConfigFactoryVtbl* vtbl;
};

// {2a59116d-6c4f-45e0-a74f-707e3fef9258} - Windows 10 builds < 21390
const GUID kIidFactoryDownlevel = { 0x2a59116d, 0x6c4f, 0x45e0, { 0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58 } };
// {ab3d4648-e242-459f-b02f-541c70306324} - builds >= 21390 (Windows 11, Server 2022+)
const GUID kIidFactory21390 = { 0xab3d4648, 0xe242, 0x459f, { 0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24 } };

constexpr const wchar_t* kClassName = L"Windows.Media.Internal.AudioPolicyConfig";
constexpr const wchar_t* kMmDevApiPrefix = L"\\\\?\\SWD#MMDEVAPI#";
constexpr const wchar_t* kRenderInterfaceSuffix = L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";

/** WinRT string/activation entry points, resolved at run time from combase.dll
    (always present on Windows 8+) so the app needs no runtimeobject.lib. */
class WinRt
{
public:
    WinRt()
    {
        module = LoadLibraryExW (L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        getActivationFactory = loadFunction<RoGetActivationFactoryFn> (module, "RoGetActivationFactory");
        createString = loadFunction<WindowsCreateStringFn> (module, "WindowsCreateString");
        deleteString = loadFunction<WindowsDeleteStringFn> (module, "WindowsDeleteString");
    }

    ~WinRt()
    {
        if (module != nullptr)
            FreeLibrary (module);
    }

    WinRt (const WinRt&) = delete;
    WinRt& operator= (const WinRt&) = delete;

    bool isValid() const noexcept { return getActivationFactory != nullptr && createString != nullptr && deleteString != nullptr; }

    HMODULE module = nullptr;
    RoGetActivationFactoryFn getActivationFactory = nullptr;
    WindowsCreateStringFn createString = nullptr;
    WindowsDeleteStringFn deleteString = nullptr;
};

/** RAII HSTRING; an empty text yields the null HSTRING (== empty string). */
class HString
{
public:
    HString (const WinRt& api, const std::wstring& text) : winrt (api)
    {
        if (! text.empty())
            result = winrt.createString (text.c_str(), static_cast<UINT32> (text.size()), &handle);
    }

    ~HString()
    {
        if (handle != nullptr)
            winrt.deleteString (handle);
    }

    HString (const HString&) = delete;
    HString& operator= (const HString&) = delete;

    HStringHandle get() const noexcept { return handle; }
    HRESULT status() const noexcept { return result; }

private:
    const WinRt& winrt;
    HStringHandle handle = nullptr;
    HRESULT result = S_OK;
};

/** Requires COM to be initialised on the calling thread (any apartment). */
bool setPersistedDefaultEndpoint (DWORD processId, const std::wstring& mmDeviceId, std::string& error)
{
    const WinRt winrt;
    if (! winrt.isValid())
    {
        error = "Windows Runtime functions are unavailable (combase.dll).";
        return false;
    }

    const HString className (winrt, kClassName);
    if (FAILED (className.status()))
    {
        error = "Cannot create WinRT class name: " + hresultToString (className.status());
        return false;
    }

    // Prefer the IID that matches this build, but accept the other one: the
    // successful IID match is what guarantees the layout, not the build number.
    const bool newer = windowsBuildNumber() >= kAudioPolicyConfigIidChangeBuild;
    const GUID* const preferred = newer ? &kIidFactory21390 : &kIidFactoryDownlevel;
    const GUID* const alternative = newer ? &kIidFactoryDownlevel : &kIidFactory21390;
    const GUID* const candidates[] = { preferred, alternative };

    AudioPolicyConfigFactory* factory = nullptr;
    HRESULT hr = E_NOINTERFACE;
    for (const GUID* iid : candidates)
    {
        hr = winrt.getActivationFactory (className.get(), *iid, reinterpret_cast<void**> (&factory));
        if (SUCCEEDED (hr) && factory != nullptr)
            break;
        factory = nullptr;
    }

    if (factory == nullptr)
    {
        error = "The per-app audio device API is not available on this Windows build (" + std::to_string (windowsBuildNumber())
              + "): " + hresultToString (hr);
        return false;
    }

    const std::wstring devicePath = mmDeviceId.empty() ? std::wstring() : kMmDevApiPrefix + mmDeviceId + kRenderInterfaceSuffix;
    const HString device (winrt, devicePath);

    if (FAILED (device.status()))
    {
        hr = device.status();
    }
    else
    {
        // Same roles the Settings page writes: console (games, most apps) and multimedia.
        for (const ERole role : { eConsole, eMultimedia })
        {
            hr = factory->vtbl->SetPersistedDefaultAudioEndpoint (factory, static_cast<UINT> (processId), eRender, role, device.get());
            if (FAILED (hr))
                break;
        }
    }

    factory->vtbl->Release (factory);

    if (FAILED (hr))
    {
        error = "Windows refused the per-app output device change: " + hresultToString (hr);
        return false;
    }

    return true;
}
} // namespace undocumented_routing
#endif // FLUB_ENABLE_UNDOCUMENTED_ROUTING

//==============================================================================
// AppAudioRouter
//==============================================================================
/*  Session enumeration is fully documented:
        IMMDeviceEnumerator -> every active render endpoint
          -> IAudioSessionManager2 -> IAudioSessionEnumerator
          -> IAudioSessionControl(2): process id, display name, state.
    A session lives on the endpoint the app renders to, so currentEndpointId
    tells the UI where each app is routed right now (e.g. "Flubsound Game").
    Sessions are merged per process (an active session wins). The system
    sounds session and Flubsound's own sessions are skipped - routing our
    own output into our virtual devices would create a feedback loop.

    isSupported() is true: listing works everywhere. setAppEndpoint() needs the
    opt-in undocumented adapter above; without it (or when it fails) it returns
    false with an explanation and the UI offers openSystemRoutingSettings(). */
class WinAppAudioRouter final : public AppAudioRouter
{
public:
    bool isSupported() const override { return true; }

    std::vector<AudioSessionInfo> enumerateSessions() override
    {
        std::vector<AudioSessionInfo> sessions;

        const ScopedComInit com;
        if (! com.isUsable())
            return sessions;

        std::string error;
        auto enumerator = createDeviceEnumerator (error);
        if (! enumerator)
            return sessions;

        ComPtr<IMMDeviceCollection> devices;
        UINT deviceCount = 0;
        if (FAILED (enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, devices.put()))
            || FAILED (devices->GetCount (&deviceCount)))
            return sessions;

        const DWORD ownProcessId = GetCurrentProcessId();
        std::map<DWORD, size_t> indexByProcess;

        for (UINT d = 0; d < deviceCount; ++d)
        {
            ComPtr<IMMDevice> device;
            if (FAILED (devices->Item (d, device.put())))
                continue;

            ComPtr<IAudioSessionManager2> manager;
            if (FAILED (device->Activate (__uuidof (IAudioSessionManager2), CLSCTX_ALL, nullptr, manager.putVoid())))
                continue;

            ComPtr<IAudioSessionEnumerator> sessionEnumerator;
            int sessionCount = 0;
            if (FAILED (manager->GetSessionEnumerator (sessionEnumerator.put())) || FAILED (sessionEnumerator->GetCount (&sessionCount)))
                continue;

            const auto deviceIdUtf8 = toUtf8 (endpointId (device.get()));

            for (int s = 0; s < sessionCount; ++s)
            {
                ComPtr<IAudioSessionControl> control;
                ComPtr<IAudioSessionControl2> control2;
                if (FAILED (sessionEnumerator->GetSession (s, control.put()))
                    || FAILED (control->QueryInterface (__uuidof (IAudioSessionControl2), control2.putVoid())))
                    continue;

                if (control2->IsSystemSoundsSession() == S_OK)
                    continue;

                AudioSessionState state = AudioSessionStateInactive;
                if (FAILED (control->GetState (&state)) || state == AudioSessionStateExpired)
                    continue;

                // AUDCLNT_S_NO_SINGLE_PROCESS still yields the initial process id.
                DWORD processId = 0;
                if (FAILED (control2->GetProcessId (&processId)) || processId == 0 || processId == ownProcessId)
                    continue;

                const bool active = (state == AudioSessionStateActive);
                const auto existing = indexByProcess.find (processId);

                if (existing != indexByProcess.end())
                {
                    auto& info = sessions[existing->second];
                    if (active && ! info.isActive)
                    {
                        info.isActive = true;
                        info.currentEndpointId = deviceIdUtf8;
                    }
                    continue;
                }

                AudioSessionInfo info;
                info.processId = static_cast<uint32_t> (processId);
                info.isActive = active;
                info.currentEndpointId = deviceIdUtf8;

                const auto imagePath = processImagePath (processId);
                info.executableName = toUtf8 (fileNameOf (imagePath));
                info.displayName = sessionDisplayName (control.get());
                if (info.displayName.empty())
                    info.displayName = toUtf8 (fileDescription (imagePath));
                if (info.displayName.empty())
                    info.displayName = toUtf8 (withoutExtension (fileNameOf (imagePath)));

                indexByProcess.emplace (processId, sessions.size());
                sessions.push_back (std::move (info));
            }
        }

        return sessions;
    }

    bool setAppEndpoint (uint32_t processId, const std::string& endpointIdOrName, std::string& error) override
    {
        if (processId == 0)
        {
            error = "Invalid process id.";
            return false;
        }

        if (processId == GetCurrentProcessId())
        {
            error = "Flubsound's own output cannot be routed into its virtual devices (feedback loop).";
            return false;
        }

#if defined(FLUB_ENABLE_UNDOCUMENTED_ROUTING)
        const ScopedComInit com;
        if (! com.isUsable())
        {
            error = "COM initialisation failed: " + hresultToString (com.status());
            return false;
        }

        std::wstring mmDeviceId; // empty = back to the system default
        if (! endpointIdOrName.empty())
        {
            auto enumerator = createDeviceEnumerator (error);
            if (! enumerator || ! resolveRenderEndpoint (enumerator.get(), endpointIdOrName, mmDeviceId, error))
                return false;
        }

        return undocumented_routing::setPersistedDefaultEndpoint (static_cast<DWORD> (processId), mmDeviceId, error);
#else
        (void) endpointIdOrName;
        error = "This build of Flubsound cannot move applications between output devices by itself (Windows only offers "
                "an undocumented API for it). Choose the output for this app in Windows Settings > System > Sound > "
                "Volume mixer (Windows 11) or App volume and device preferences (Windows 10).";
        return false;
#endif
    }

    void openSystemRoutingSettings() override
    {
        // ShellExecute may delegate to shell extensions and wants COM (STA).
        const ScopedComInit com;
        ShellExecuteW (nullptr, L"open", L"ms-settings:apps-volume", nullptr, nullptr, SW_SHOWNORMAL);
    }

private:
    static std::string sessionDisplayName (IAudioSessionControl* control)
    {
        CoTaskString name;
        if (FAILED (control->GetDisplayName (&name.text)) || name.text == nullptr || name.text[0] == L'\0')
            return {};

        // Indirect strings ("@%SystemRoot%\System32\AudioSrv.Dll,-202") are resource references.
        if (name.text[0] == L'@')
        {
            wchar_t resolved[512] = {};
            if (SUCCEEDED (SHLoadIndirectString (name.text, resolved, static_cast<UINT> (std::size (resolved)), nullptr)))
                return toUtf8 (resolved);
            return {};
        }

        return toUtf8 (name.text);
    }
};

//==============================================================================
// ProcessLoopbackCapture
//==============================================================================

/** Completion handler for ActivateAudioInterfaceAsync. Hand-written COM object
    with manual reference counting. The API requires an agile handler ("the
    implementation must be agile (aggregating a free-threaded marshaler)"), so
    the audio stack may call ActivateCompleted directly on its worker thread
    without marshalling it back to our apartment. We do both things WRL's
    FtmBase does: answer IAgileObject (a marker interface without methods) and
    aggregate the free-threaded marshaler for IMarshal.
    Must be created on a thread that has initialised COM. */
class ActivationCompletionHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject
{
public:
    ActivationCompletionHandler() : completed (CreateEventW (nullptr, TRUE, FALSE, nullptr))
    {
        // Aggregation: the FTM's IUnknown is our private inner object; it does
        // not AddRef us (the controlling unknown), so no reference cycle.
        if (FAILED (CoCreateFreeThreadedMarshaler (static_cast<IActivateAudioInterfaceCompletionHandler*> (this), &freeThreadedMarshaler)))
            freeThreadedMarshaler = nullptr;
    }

    bool isValid() const noexcept { return completed.handle != nullptr && freeThreadedMarshaler != nullptr; }

    /** Waits for completion; on success returns the IAudioClient (caller owns a ref). */
    HRESULT waitForClient (DWORD timeoutMs, IAudioClient** client)
    {
        *client = nullptr;

        if (WaitForSingleObject (completed.handle, timeoutMs) != WAIT_OBJECT_0)
            return HRESULT_FROM_WIN32 (ERROR_TIMEOUT);

        // The event (SetEvent/Wait) orders these reads after the writes in ActivateCompleted.
        if (FAILED (result))
            return result;

        *client = std::exchange (audioClient, nullptr);
        return *client != nullptr ? S_OK : E_UNEXPECTED;
    }

    // IUnknown -----------------------------------------------------------------
    HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void** object) override
    {
        if (object == nullptr)
            return E_POINTER;

        if (riid == __uuidof (IUnknown) || riid == __uuidof (IActivateAudioInterfaceCompletionHandler))
            *object = static_cast<IActivateAudioInterfaceCompletionHandler*> (this);
        else if (riid == __uuidof (IAgileObject))
            *object = static_cast<IAgileObject*> (this);
        else if (riid == __uuidof (IMarshal) && freeThreadedMarshaler != nullptr)
            return freeThreadedMarshaler->QueryInterface (riid, object); // AddRefs us through aggregation
        else
        {
            *object = nullptr;
            return E_NOINTERFACE;
        }

        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG> (InterlockedIncrement (&refCount)); }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG remaining = InterlockedDecrement (&refCount);
        if (remaining == 0)
            delete this;
        return static_cast<ULONG> (remaining);
    }

    // IActivateAudioInterfaceCompletionHandler ---------------------------------
    HRESULT STDMETHODCALLTYPE ActivateCompleted (IActivateAudioInterfaceAsyncOperation* operation) override
    {
        HRESULT activateResult = E_UNEXPECTED;
        IUnknown* activated = nullptr;

        HRESULT hr = operation != nullptr ? operation->GetActivateResult (&activateResult, &activated) : E_POINTER;
        if (SUCCEEDED (hr))
            hr = activateResult;
        if (SUCCEEDED (hr) && activated != nullptr)
            hr = activated->QueryInterface (__uuidof (IAudioClient), reinterpret_cast<void**> (&audioClient));
        else if (SUCCEEDED (hr))
            hr = E_NOINTERFACE;

        if (activated != nullptr)
            activated->Release();

        result = hr;
        SetEvent (completed.handle);
        return S_OK;
    }

private:
    ~ActivationCompletionHandler()
    {
        if (audioClient != nullptr)
            audioClient->Release();
        if (freeThreadedMarshaler != nullptr)
            freeThreadedMarshaler->Release();
    }

    LONG refCount = 1;
    IUnknown* freeThreadedMarshaler = nullptr; // inner (non-delegating) unknown of the aggregated FTM
    ScopedHandle completed;
    HRESULT result = E_PENDING;
    IAudioClient* audioClient = nullptr;
};

/*  Per-process capture through the documented process-loopback virtual device
    (Windows 10 build 20348+ / Windows 11; see Microsoft's ApplicationLoopback
    sample):

      ActivateAudioInterfaceAsync (VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
          IAudioClient, PROPVARIANT{VT_BLOB: AUDIOCLIENT_ACTIVATION_PARAMS})
      -> IAudioClient::Initialize (shared, LOOPBACK | EVENTCALLBACK |
             AUTOCONVERTPCM | SRC_DEFAULT_QUALITY, explicit float32 format)
      -> event-driven IAudioCaptureClient reads on an MMCSS thread.

    includeProcessTree maps onto PROCESS_LOOPBACK_MODE:
        true  -> INCLUDE_TARGET_PROCESS_TREE: the process and all its children
                 (games often render from a child/launcher process);
        false -> EXCLUDE_TARGET_PROCESS_TREE: everything the system plays
                 EXCEPT that process tree. Passing Flubsound's own pid gives
                 the full system mix without our own output (no feedback).
    Windows offers no "this process only, without children" mode.

    GetMixFormat() is not implemented by the process-loopback device, so we
    ask for float32 at the caller's rate/channel count and let the audio
    engine convert (AUTOCONVERTPCM + SRC_DEFAULT_QUALITY).

    Lifetime: start()/stop()/destruction from any non-real-time thread, but
    never destroy or restart the object from inside the FrameCallback (stop()
    from the callback only requests the stop; it cannot join its own thread).
    If the stream dies (e.g. the audio service restarts), the thread exits and
    isRunning() turns false; start() may then simply be called again. */
class WinProcessLoopbackCapture final : public ProcessLoopbackCapture
{
public:
    ~WinProcessLoopbackCapture() override { stop(); }

    bool isSupported() const override { return windowsBuildNumber() >= kFirstProcessLoopbackBuild; }

    bool start (uint32_t processId,
                bool includeProcessTree,
                double sampleRate,
                int numChannels,
                FrameCallback frameCallback,
                std::string& error) override
    {
        if (isRunning())
        {
            error = "Capture is already running; call stop() first.";
            return false;
        }

        stop(); // reap a thread that ended on its own (device invalidated)

        if (! isSupported())
        {
            error = "Per-application capture needs Windows 11 or Windows 10 build 20348+ (this is build "
                  + std::to_string (windowsBuildNumber()) + ").";
            return false;
        }

        if (processId == 0 || ! frameCallback || ! (sampleRate >= 8000.0 && sampleRate <= 384000.0) || numChannels < 1
            || numChannels > kMaxChannels)
        {
            error = "Invalid capture parameters (process id, callback, 8-384 kHz, 1-8 channels).";
            return false;
        }

        {
            // Fail early with a readable message rather than capturing silence forever.
            ScopedHandle target (OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
            if (target.handle == nullptr && GetLastError() == ERROR_INVALID_PARAMETER)
            {
                error = "Process " + std::to_string (processId) + " does not exist.";
                return false;
            }
        }

        config.processId = static_cast<DWORD> (processId);
        config.includeTree = includeProcessTree;
        config.sampleRate = static_cast<DWORD> (std::lround (sampleRate));
        config.numChannels = numChannels;
        callback = std::move (frameCallback);
        startupError.clear();

        stopEvent.reset (CreateEventW (nullptr, TRUE, FALSE, nullptr));
        startedEvent.reset (CreateEventW (nullptr, TRUE, FALSE, nullptr));
        if (stopEvent.handle == nullptr || startedEvent.handle == nullptr)
        {
            error = "CreateEvent failed (" + std::to_string (GetLastError()) + ").";
            return false;
        }

        thread.reset (CreateThread (nullptr, 0, &WinProcessLoopbackCapture::threadEntry, this, 0, &threadId));
        if (thread.handle == nullptr)
        {
            error = "CreateThread failed (" + std::to_string (GetLastError()) + ").";
            return false;
        }

        // The thread either signals "started" or exits with startupError set.
        const HANDLE waitFor[] = { startedEvent.handle, thread.handle };
        const DWORD which = WaitForMultipleObjects (2, waitFor, FALSE, INFINITE);

        if (which != WAIT_OBJECT_0)
        {
            error = startupError.empty() ? std::string ("Capture thread failed to start.") : startupError;
            stop();
            return false;
        }

        return true;
    }

    void stop() override
    {
        if (thread.handle == nullptr)
            return;

        if (stopEvent.handle != nullptr)
            SetEvent (stopEvent.handle);

        // stop() from inside the frame callback cannot join its own thread:
        // the loop sees the stop event and exits after the callback returns.
        if (GetCurrentThreadId() == threadId)
            return;

        WaitForSingleObject (thread.handle, INFINITE);
        thread.reset();
        threadId = 0;
        running = false;
    }

    bool isRunning() const override { return running.load (std::memory_order_acquire); }

private:
    static constexpr int kMaxChannels = 8;
    static constexpr DWORD kActivationTimeoutMs = 5000;
    static constexpr REFERENCE_TIME kBufferDuration = 50 * 10000; // 50 ms: headroom only - we drain on every event

    struct Config
    {
        DWORD processId = 0;
        bool includeTree = true;
        DWORD sampleRate = 48000;
        int numChannels = 2;
    };

    static DWORD WINAPI threadEntry (void* self)
    {
        static_cast<WinProcessLoopbackCapture*> (self)->threadMain();
        return 0;
    }

    static void nameCurrentThread()
    {
        using SetThreadDescriptionFn = HRESULT (WINAPI*) (HANDLE, PCWSTR); // Windows 10 1607+
        if (auto setDescription = loadFunction<SetThreadDescriptionFn> (GetModuleHandleW (L"kernel32.dll"), "SetThreadDescription"))
            setDescription (GetCurrentThread(), L"Flubsound process capture");
    }

    static DWORD channelMaskFor (int channels)
    {
        switch (channels)
        {
            case 1: return SPEAKER_FRONT_CENTER;
            case 2: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
            case 4: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
            case 6:
                return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT
                     | SPEAKER_BACK_RIGHT;
            case 8:
                return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT
                     | SPEAKER_BACK_RIGHT | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
            default: return 0; // unspecified order
        }
    }

    /** Resources owned by the capture thread (all COM objects live in its MTA). */
    struct Stream
    {
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> capture;
        ScopedHandle samplesReady;
        std::vector<float> silence; // pre-allocated so SILENT packets never allocate
        UINT32 silenceFrames = 0;
    };

    bool openStream (Stream& stream, std::string& error)
    {
        // --- activation ------------------------------------------------------
        AUDIOCLIENT_ACTIVATION_PARAMS activation {};
        activation.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        activation.ProcessLoopbackParams.TargetProcessId = config.processId;
        activation.ProcessLoopbackParams.ProcessLoopbackMode =
            config.includeTree ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

        // The blob points at our stack struct: do NOT PropVariantClear() it.
        PROPVARIANT params;
        PropVariantInit (&params);
        params.vt = VT_BLOB;
        params.blob.cbSize = sizeof (activation);
        params.blob.pBlobData = reinterpret_cast<BYTE*> (&activation);

        auto* handler = new ActivationCompletionHandler(); // refCount = 1 (ours)
        if (! handler->isValid())
        {
            handler->Release();
            error = "Cannot create the activation completion handler (CreateEvent / CoCreateFreeThreadedMarshaler failed).";
            return false;
        }

        IActivateAudioInterfaceAsyncOperation* operation = nullptr;
        HRESULT hr =
            ActivateAudioInterfaceAsync (VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof (IAudioClient), &params, handler, &operation);

        if (SUCCEEDED (hr))
            hr = handler->waitForClient (kActivationTimeoutMs, stream.client.put());

        // The OS keeps its own references until the callback has run, so
        // releasing ours here is safe even after a timeout.
        if (operation != nullptr)
            operation->Release();
        handler->Release();

        if (FAILED (hr))
        {
            error = "Process-loopback activation failed: " + hresultToString (hr);
            return false;
        }

        // --- format + initialisation --------------------------------------------
        WAVEFORMATEXTENSIBLE format {};
        format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        format.Format.nChannels = static_cast<WORD> (config.numChannels);
        format.Format.nSamplesPerSec = config.sampleRate;
        format.Format.wBitsPerSample = 32;
        format.Format.nBlockAlign = static_cast<WORD> (config.numChannels * static_cast<int> (sizeof (float)));
        format.Format.nAvgBytesPerSec = config.sampleRate * format.Format.nBlockAlign;
        format.Format.cbSize = static_cast<WORD> (sizeof (WAVEFORMATEXTENSIBLE) - sizeof (WAVEFORMATEX));
        format.Samples.wValidBitsPerSample = 32;
        format.dwChannelMask = channelMaskFor (config.numChannels);
        // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT {00000003-0000-0010-8000-00aa00389b71}, local to avoid ksuser/uuid.
        format.SubFormat = GUID { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

        const DWORD streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                                | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

        hr = stream.client->Initialize (AUDCLNT_SHAREMODE_SHARED, streamFlags, kBufferDuration, 0, &format.Format, nullptr);
        if (FAILED (hr))
        {
            error = "IAudioClient::Initialize (process loopback, float32 " + std::to_string (config.sampleRate) + " Hz, "
                  + std::to_string (config.numChannels) + " ch) failed: " + hresultToString (hr);
            return false;
        }

        stream.samplesReady.reset (CreateEventW (nullptr, FALSE, FALSE, nullptr));
        if (stream.samplesReady.handle == nullptr)
        {
            error = "CreateEvent failed.";
            return false;
        }

        hr = stream.client->SetEventHandle (stream.samplesReady.handle);
        if (SUCCEEDED (hr))
            hr = stream.client->GetService (__uuidof (IAudioCaptureClient), stream.capture.putVoid());
        if (FAILED (hr))
        {
            error = "Process-loopback capture service unavailable: " + hresultToString (hr);
            return false;
        }

        // Size the silence buffer from the stream buffer (fallback: 100 ms).
        UINT32 bufferFrames = 0;
        if (FAILED (stream.client->GetBufferSize (&bufferFrames)) || bufferFrames == 0)
            bufferFrames = config.sampleRate / 10;
        stream.silenceFrames = bufferFrames;
        stream.silence.assign (static_cast<size_t> (bufferFrames) * static_cast<size_t> (config.numChannels), 0.0f);

        hr = stream.client->Start();
        if (FAILED (hr))
        {
            error = "IAudioClient::Start failed: " + hresultToString (hr);
            return false;
        }

        return true;
    }

    /** Real-time loop: no allocation, locks or logging past this point. */
    void captureLoop (Stream& stream)
    {
        const HANDLE waitFor[] = { stopEvent.handle, stream.samplesReady.handle };
        const int channels = config.numChannels;

        for (;;)
        {
            // The timeout only guards against a stalled device; a silent target
            // process may legitimately deliver no events for a while.
            const DWORD which = WaitForMultipleObjects (2, waitFor, FALSE, 2000);
            if (which == WAIT_OBJECT_0)
                return; // stop requested
            if (which == WAIT_TIMEOUT)
                continue;
            if (which != WAIT_OBJECT_0 + 1)
                return;

            UINT32 packetFrames = 0;
            HRESULT hr = S_OK;
            while (SUCCEEDED (hr = stream.capture->GetNextPacketSize (&packetFrames)) && packetFrames > 0)
            {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = stream.capture->GetBuffer (&data, &frames, &flags, nullptr, nullptr);
                if (FAILED (hr))
                    break;
                if (hr == AUDCLNT_S_BUFFER_EMPTY || frames == 0)
                {
                    // Success code without a packet. AUDCLNT_S_BUFFER_EMPTY
                    // acquires nothing; a (theoretical) S_OK with 0 frames is
                    // closed with ReleaseBuffer (0) so the next GetBuffer is
                    // never out of order. Then wait for the next event.
                    hr = (hr == AUDCLNT_S_BUFFER_EMPTY) ? S_OK : stream.capture->ReleaseBuffer (0);
                    break;
                }

                // AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY only reports a glitch
                // *before* this packet (e.g. we were late); the packet itself
                // is valid and is delivered normally; the consumer side
                // (DriftCompensatedFifo) absorbs the timing jump.
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr)
                {
                    // Deliver silence explicitly so downstream timing stays continuous.
                    for (UINT32 done = 0; done < frames;)
                    {
                        const UINT32 chunk = std::min (frames - done, stream.silenceFrames);
                        callback (stream.silence.data(), static_cast<int> (chunk), channels);
                        done += chunk;
                    }
                }
                else
                {
                    callback (reinterpret_cast<const float*> (data), static_cast<int> (frames), channels);
                }

                hr = stream.capture->ReleaseBuffer (frames);
                if (FAILED (hr))
                    break;
            }

            if (FAILED (hr))
                return; // e.g. AUDCLNT_E_DEVICE_INVALIDATED: audio service restarted
        }
    }

    void threadMain()
    {
        nameCurrentThread();

        const ScopedComInit com (COINIT_MULTITHREADED);
        Stream stream;

        if (! com.isUsable())
        {
            startupError = "COM initialisation failed: " + hresultToString (com.status());
            return; // thread exit wakes start()
        }

        if (! openStream (stream, startupError))
            return;

        running = true;
        SetEvent (startedEvent.handle);

        void* mmcss = SystemTuning::promoteAudioThread();
        captureLoop (stream);
        SystemTuning::revertAudioThread (mmcss);

        stream.client->Stop();
        running = false;
    }

    Config config;
    FrameCallback callback;
    std::string startupError; // written by the thread before it signals/exits
    std::atomic<bool> running { false };
    ScopedHandle thread, stopEvent, startedEvent;
    DWORD threadId = 0;
};

//==============================================================================
// AutoStart - HKCU\Software\Microsoft\Windows\CurrentVersion\Run
//==============================================================================
/*  The per-user Run key is what Settings > Apps > Startup and Task Manager's
    "Startup apps" list. Their on/off switch does not touch the Run value: it
    writes a REG_BINARY of the same name under Explorer\StartupApproved\Run
    whose first byte is even (02) for enabled and odd (03) for disabled. That
    value is undocumented but unchanged since Windows 8; it is only read (so
    isEnabled() tells the truth) and deleted (so turning the feature on in
    Flubsound also undoes a "disabled" switch - a missing value means
    enabled). No elevation needed: everything is under HKEY_CURRENT_USER. */
namespace autostart
{
constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kApprovedKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr const wchar_t* kValueName = L"Flubsound Pro";

struct ScopedKey
{
    ScopedKey() = default;
    ~ScopedKey()
    {
        if (key != nullptr)
            RegCloseKey (key);
    }
    ScopedKey (const ScopedKey&) = delete;
    ScopedKey& operator= (const ScopedKey&) = delete;

    HKEY key = nullptr;
};

std::string errorText (LONG code) { return hresultToString (HRESULT_FROM_WIN32 (static_cast<unsigned long> (code))); }

/** The Run value's command line, or empty when there is none. */
std::wstring readRunCommand()
{
    ScopedKey run;
    if (RegOpenKeyExW (HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &run.key) != ERROR_SUCCESS)
        return {};

    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW (run.key, kValueName, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)
        || bytes == 0 || bytes > 65536)
        return {};

    // REG_SZ data is not guaranteed to be terminated: leave room for one.
    std::wstring command (bytes / sizeof (wchar_t) + 1, L'\0');
    DWORD size = bytes;
    if (RegQueryValueExW (run.key, kValueName, nullptr, &type, reinterpret_cast<BYTE*> (command.data()), &size) != ERROR_SUCCESS)
        return {};
    command.resize (size / sizeof (wchar_t));
    while (! command.empty() && command.back() == L'\0')
        command.pop_back();
    return command;
}

/** True when Task Manager / Settings switched the entry off. */
bool isSwitchedOffByUser()
{
    ScopedKey approved;
    if (RegOpenKeyExW (HKEY_CURRENT_USER, kApprovedKey, 0, KEY_QUERY_VALUE, &approved.key) != ERROR_SUCCESS)
        return false;

    BYTE data[64] = {};
    DWORD type = 0, size = sizeof (data);
    if (RegQueryValueExW (approved.key, kValueName, nullptr, &type, data, &size) != ERROR_SUCCESS || type != REG_BINARY || size == 0)
        return false;
    return (data[0] & 1u) != 0;
}

/** Deletes a value; a missing key or value counts as success. */
LONG deleteValue (const wchar_t* subKey)
{
    ScopedKey key;
    LONG result = RegOpenKeyExW (HKEY_CURRENT_USER, subKey, 0, KEY_SET_VALUE, &key.key);
    if (result == ERROR_SUCCESS)
        result = RegDeleteValueW (key.key, kValueName);
    return result == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : result;
}

std::wstring runningExecutable()
{
    std::wstring path (MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD length = GetModuleFileNameW (nullptr, path.data(), static_cast<DWORD> (path.size()));
        if (length == 0)
            return {};
        if (length < path.size())
        {
            path.resize (length);
            return path;
        }
        if (path.size() >= 32768)
            return {};
        path.resize (path.size() * 2); // truncated: retry with a larger buffer
    }
}
} // namespace autostart

class WinAutoStart final : public AutoStart
{
public:
    bool isSupported() const override { return true; }
    bool isEnabled() const override { return ! autostart::readRunCommand().empty() && ! autostart::isSwitchedOffByUser(); }

    bool setEnabled (bool shouldStart, const std::string& executablePath, std::string& error) override
    {
        using namespace autostart;

        if (! shouldStart)
        {
            const LONG result = deleteValue (kRunKey);
            deleteValue (kApprovedKey); // tidy up; a leftover switch is harmless
            if (result != ERROR_SUCCESS)
            {
                error = "Could not remove the start-up entry: " + errorText (result);
                return false;
            }
            return true;
        }

        const std::wstring path = executablePath.empty() ? runningExecutable() : toWide (executablePath);
        if (path.empty() || (! executablePath.empty() && toUtf8 (path) != executablePath))
        {
            error = "The program path is empty or not valid UTF-8.";
            return false;
        }
        if (path.find (L'"') != std::wstring::npos || PathIsRelativeW (path.c_str()))
        {
            error = "The program path for the start-up entry must be an absolute path: \"" + toUtf8 (path) + "\".";
            return false;
        }

        // Quoted, so a path with spaces ("C:\Program Files\...") is one argument.
        const std::wstring command = L"\"" + path + L"\"";
        ScopedKey run;
        LONG result = RegCreateKeyExW (HKEY_CURRENT_USER, kRunKey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &run.key, nullptr);
        if (result == ERROR_SUCCESS)
            result = RegSetValueExW (run.key,
                                     kValueName,
                                     0,
                                     REG_SZ,
                                     reinterpret_cast<const BYTE*> (command.c_str()),
                                     static_cast<DWORD> ((command.size() + 1) * sizeof (wchar_t)));
        if (result != ERROR_SUCCESS)
        {
            error = "Could not write the start-up entry: " + errorText (result);
            return false;
        }

        result = deleteValue (kApprovedKey);
        if (result != ERROR_SUCCESS)
        {
            error = "The start-up entry was written, but it is switched off in Task Manager > Startup apps: " + errorText (result);
            return false;
        }
        return true;
    }
};

//==============================================================================
// ForegroundApp - GetForegroundWindow + QueryFullProcessImageNameW
//==============================================================================
/*  The foreground window's owning process. PROCESS_QUERY_LIMITED_INFORMATION
    is granted for processes of other users and most elevated ones, so games
    started "as administrator" are still recognised; protected processes and
    the secure desktop (UAC prompt, lock screen: no foreground window) are
    not, and query() returns false for them. UWP / packaged apps draw inside
    an ApplicationFrameHost.exe frame; the process of the hosted child
    window is reported instead (the frame itself while the app is still
    starting, looked up again on the next poll). The image path is cached
    while the same window and process stay in front. Message thread (any one thread). */
class WinForegroundApp final : public ForegroundApp
{
public:
    bool isSupported() const override { return true; }
    std::string unsupportedReason() const override { return {}; }

    bool query (ForegroundAppInfo& info) override
    {
        const HWND window = GetForegroundWindow();
        if (window == nullptr)
            return false;
        DWORD processId = 0;
        GetWindowThreadProcessId (window, &processId);
        if (processId == 0)
            return false;

        if (window != cachedWindow || processId != cachedFrameProcess)
        {
            cachedWindow = nullptr;
            cachedFrameProcess = processId;
            auto path = processImagePath (processId);
            bool resolved = true;
            if (_wcsicmp (fileNameOf (path).c_str(), L"ApplicationFrameHost.exe") == 0)
            {
                if (const DWORD hosted = hostedProcess (window, processId); hosted != 0)
                {
                    processId = hosted;
                    path = processImagePath (hosted);
                }
                else
                {
                    resolved = false; // app still starting (or suspended): look again next poll
                }
            }
            if (path.empty())
                return false;
            cached.processId = static_cast<uint32_t> (processId);
            cached.executablePath = toUtf8 (path);
            cached.executableName = toUtf8 (fileNameOf (path));
            cached.bundleId.clear();
            cached.isThisProcess = processId == GetCurrentProcessId();
            cachedWindow = resolved ? window : nullptr;
        }
        info = cached;
        return true;
    }

private:
    /** The first child window of a UWP frame that belongs to another process
        (the app's CoreWindow); 0 if there is none (e.g. a suspended app). */
    static DWORD hostedProcess (HWND frame, DWORD frameProcess)
    {
        struct Search
        {
            DWORD frame = 0, found = 0;
        } search { frameProcess, 0 };

        EnumChildWindows (
            frame,
            [] (HWND child, LPARAM context) -> BOOL
            {
                auto& s = *reinterpret_cast<Search*> (context);
                DWORD pid = 0;
                GetWindowThreadProcessId (child, &pid);
                if (pid != 0 && pid != s.frame)
                {
                    s.found = pid;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM> (&search));
        return search.found;
    }

    HWND cachedWindow = nullptr;
    DWORD cachedFrameProcess = 0;
    ForegroundAppInfo cached;
};

// Token returned by promoteAudioThread when MMCSS was unavailable and we fell
// back to a plain thread-priority boost.
char threadPriorityFallbackToken = 0;
} // namespace

//==============================================================================
// SystemTuning
//==============================================================================
// =============================================================================
// AudioEndpoints: output transport (for headset device profiles)
// =============================================================================
namespace
{
// PKEY_Device_EnumeratorName (same fmtid as FriendlyName, pid 24) and
// PKEY_AudioEndpoint_FormFactor, defined locally (no INITGUID / uuid.lib).
const PROPERTYKEY kDeviceEnumeratorNameKey = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 24 };
const PROPERTYKEY kEndpointFormFactorKey = { { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e } }, 0 };
constexpr UINT kFormFactorDigitalAudioDisplayDevice = 9; // EndpointFormFactor::DigitalAudioDisplayDevice (HDMI / DP)

std::wstring readStringProperty (IPropertyStore* store, const PROPERTYKEY& key)
{
    PROPVARIANT value;
    PropVariantInit (&value);
    std::wstring text;
    if (SUCCEEDED (store->GetValue (key, &value)) && value.vt == VT_LPWSTR && value.pwszVal != nullptr)
        text = value.pwszVal;
    PropVariantClear (&value);
    return text;
}

UINT readUIntProperty (IPropertyStore* store, const PROPERTYKEY& key, UINT fallback)
{
    PROPVARIANT value;
    PropVariantInit (&value);
    UINT result = fallback;
    if (SUCCEEDED (store->GetValue (key, &value)) && value.vt == VT_UI4)
        result = value.ulVal;
    PropVariantClear (&value);
    return result;
}

EndpointTransport transportFromEnumerator (const std::wstring& enumerator, UINT formFactor)
{
    std::wstring e = enumerator;
    for (auto& ch : e)
        ch = static_cast<wchar_t> (towupper (ch));

    if (e.find (L"BTHHFENUM") != std::wstring::npos)
        return EndpointTransport::BluetoothHandsFree; // Bluetooth hands-free profile
    if (e.rfind (L"BTH", 0) == 0)
        return EndpointTransport::Bluetooth;          // BTHENUM (A2DP), BTHLEDEVICE (LE Audio)
    if (e == L"USB")
        return EndpointTransport::Usb;
    if (formFactor == kFormFactorDigitalAudioDisplayDevice)
        return EndpointTransport::Hdmi;
    if (e == L"HDAUDIO" || e == L"INTELAUDIO" || e == L"ACPI" || e == L"PCI")
        return EndpointTransport::Analog;
    if (e == L"SWD" || e == L"ROOT")
        return EndpointTransport::Virtual;            // software / root-enumerated (virtual cables, our driver)
    return EndpointTransport::Unknown;
}
} // namespace

EndpointTransport AudioEndpoints::queryOutputTransport (const std::string& deviceName)
{
    const ScopedComInit com;
    if (! com.isUsable() || deviceName.empty())
        return EndpointTransport::Unknown;

    std::string error;
    auto enumerator = createDeviceEnumerator (error);
    if (! enumerator)
        return EndpointTransport::Unknown;

    ComPtr<IMMDeviceCollection> devices;
    UINT count = 0;
    if (FAILED (enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, devices.put())) || FAILED (devices->GetCount (&count)))
        return EndpointTransport::Unknown;

    const std::wstring wanted = toWide (deviceName);
    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<IMMDevice> device;
        if (FAILED (devices->Item (i, device.put())))
            continue;
        if (endpointFriendlyName (device.get()) != wanted)
            continue;

        ComPtr<IPropertyStore> store;
        if (FAILED (device->OpenPropertyStore (STGM_READ, store.put())))
            return EndpointTransport::Unknown;
        return transportFromEnumerator (readStringProperty (store.get(), kDeviceEnumeratorNameKey),
                                        readUIntProperty (store.get(), kEndpointFormFactorKey, 10));
    }
    return EndpointTransport::Unknown;
}

bool SystemTuning::disablePowerThrottling()
{
    /*  EcoQoS: Windows 11 (and 10 on hybrid CPUs) may run processes it thinks are
        "background" at reduced clock / on efficiency cores, which causes
        dropouts in a tray-resident audio app. ControlMask selects the policy
        we take control of; StateMask = 0 means "never throttle".
        IGNORE_TIMER_RESOLUTION (Windows 11) additionally keeps our timer
        resolution requests honoured while the window is minimised/occluded;
        older builds reject that bit, so retry with execution speed only. */
    struct PowerThrottlingState
    {
        ULONG Version;
        ULONG ControlMask;
        ULONG StateMask;
    };
    static_assert (sizeof (PowerThrottlingState) == 12, "PROCESS_POWER_THROTTLING_STATE layout");

    PowerThrottlingState state {};
    state.Version = FLUB_POWER_THROTTLING_VERSION;
    state.ControlMask = FLUB_POWER_THROTTLING_EXECUTION_SPEED | FLUB_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    state.StateMask = 0;

    if (SetProcessInformation (GetCurrentProcess(), FLUB_PROCESS_POWER_THROTTLING_CLASS, &state, sizeof (state)))
        return true;

    state.ControlMask = FLUB_POWER_THROTTLING_EXECUTION_SPEED;
    return SetProcessInformation (GetCurrentProcess(), FLUB_PROCESS_POWER_THROTTLING_CLASS, &state, sizeof (state)) != FALSE;
}

void* SystemTuning::promoteAudioThread()
{
    // MMCSS boosts the thread into the real-time priority band (and exempts it
    // from EcoQoS) while the Multimedia Class Scheduler service is running.
    for (const wchar_t* task : { L"Pro Audio", L"Audio" })
    {
        DWORD taskIndex = 0;
        if (HANDLE handle = AvSetMmThreadCharacteristicsW (task, &taskIndex))
        {
            AvSetMmThreadPriority (handle, AVRT_PRIORITY_HIGH);
            return handle;
        }
    }

    // MMCSS unavailable (service disabled, task missing): best-effort boost.
    if (SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL))
        return &threadPriorityFallbackToken;

    return nullptr;
}

void SystemTuning::revertAudioThread (void* handle)
{
    if (handle == nullptr)
        return;

    if (handle == &threadPriorityFallbackToken)
    {
        SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        return;
    }

    AvRevertMmThreadCharacteristics (static_cast<HANDLE> (handle));
}

//==============================================================================
std::unique_ptr<GlobalHotkeys> GlobalHotkeys::create() { return std::make_unique<WinGlobalHotkeys>(); }
std::unique_ptr<AppAudioRouter> AppAudioRouter::create() { return std::make_unique<WinAppAudioRouter>(); }
std::unique_ptr<ProcessLoopbackCapture> ProcessLoopbackCapture::create() { return std::make_unique<WinProcessLoopbackCapture>(); }
std::unique_ptr<AutoStart> AutoStart::create() { return std::make_unique<WinAutoStart>(); }
std::unique_ptr<ForegroundApp> ForegroundApp::create() { return std::make_unique<WinForegroundApp>(); }
} // namespace flub::platform

#endif // _WIN32
