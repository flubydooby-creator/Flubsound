// Flubsound Pro - libpipewire-0.3 opened at run time (R1.2). See
// PipeWireLibrary.h. Compiled only with FLUB_HAS_PIPEWIRE (pkg-config found
// libpipewire-0.3's headers); the app links no libpipewire.
#if defined(__linux__) && defined(FLUB_HAS_PIPEWIRE) && FLUB_HAS_PIPEWIRE

#include "PipeWireLibrary.h"

#include "PipeWireApi.h"

#include <dlfcn.h>

#include <cstring>
#include <vector>

namespace flub::platform::pipewire
{
namespace
{
struct Loaded
{
    LibraryStatus status;
    Api entryPoints;
};

/** dlsym into a function pointer of the declared type (memcpy: a cast
    between object and function pointers is only conditionally supported). */
template <typename Function>
void resolve (void* handle, const char* symbol, Function& function, std::vector<std::string>& missing)
{
    static_assert (sizeof (Function) == sizeof (void*), "a function pointer must fit a data pointer (POSIX dlsym)");
    void* address = ::dlsym (handle, symbol);
    std::memcpy (&function, &address, sizeof (function));
    if (address == nullptr)
        missing.emplace_back (symbol);
}

Loaded load (const char* name)
{
    Loaded result;
    result.status.name = name != nullptr ? name : "";
    ::dlerror(); // clear an earlier message
    // RTLD_LOCAL: nothing else in the process sees libpipewire's symbols;
    // RTLD_NOW: a broken install fails here, not in the middle of a call.
    void* handle = name != nullptr ? ::dlopen (name, RTLD_NOW | RTLD_LOCAL) : nullptr;
    if (handle == nullptr)
    {
        const char* why = ::dlerror();
        result.status.error = "PipeWire's client library (" + result.status.name + ") is not installed"
                            + (why != nullptr ? std::string (": ") + why : std::string());
        return result;
    }

    std::vector<std::string> missing;
    auto& table = result.entryPoints;
#define FLUB_PIPEWIRE_RESOLVE(entry) resolve (handle, "pw_" #entry, table.entry, missing);
    FLUB_PIPEWIRE_ENTRY_POINTS (FLUB_PIPEWIRE_RESOLVE)
#undef FLUB_PIPEWIRE_RESOLVE
    if (! missing.empty())
    {
        std::string list;
        for (const auto& symbol : missing)
            list += (list.empty() ? "" : ", ") + symbol;
        result.status.error = result.status.name + " lacks " + list + " (PipeWire older than 0.3.48?)";
        result.entryPoints = {};
        return result; // the handle stays open: nothing calls into it
    }

    result.status.loaded = true;
    if (const char* version = table.get_library_version())
        result.status.version = version;
    // Never closed: libpipewire keeps loop threads, plug-ins and
    // atexit-time state for the process's lifetime.
    return result;
}

const Loaded& instance()
{
    static const Loaded loaded = load (kLibraryName);
    return loaded;
}
} // namespace

const LibraryStatus& library() { return instance().status; }

const Api& api() { return instance().entryPoints; }

LibraryStatus probeLibrary (const char* name) { return load (name).status; }
} // namespace flub::platform::pipewire

#endif
