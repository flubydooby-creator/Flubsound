// Flubsound Pro - libpipewire-0.3 opened at run time (R1.2, docs/11 E48).
//
// The app is built against libpipewire's headers (pkg-config,
// FLUB_HAS_PIPEWIRE) but does not link the library: PipeWireLibrary.cpp
// opens libpipewire-0.3.so.0 with dlopen the first time a PipeWire
// connection is wanted and resolves every entry point PipeWireNative.cpp
// calls (PipeWireApi.h). So one binary starts everywhere: on a system without
// PipeWire's client library (PulseAudio only, no sound server) library()
// says why, the "PipeWire" device type is not offered and the app keeps
// JUCE's ALSA / JACK types (PulseAudio through its ALSA plug-in, pulse-alsa).
//
// No libpipewire header here: the device type and the tests include it.
#pragma once

#include <string>

namespace flub::platform::pipewire
{
/** The soname opened (the 0.3 client API; PipeWire 1.x still ships it). */
inline constexpr const char* kLibraryName = "libpipewire-0.3.so.0";

struct LibraryStatus
{
    bool loaded = false; // opened, and every entry point resolved
    std::string name;    // the file asked for (kLibraryName)
    std::string version; // pw_get_library_version(), e.g. "1.0.5", once loaded
    std::string error;   // why not (user-presentable); empty when loaded
};

/** libpipewire, opened on the first call (thread-safe) and kept open for the
    process's lifetime (its loop threads and plug-ins stay loaded). Any
    thread; blocks for the dlopen on the first call only. */
const LibraryStatus& library();

/** Tests: opens 'name' and resolves the entry points exactly as library()
    does, without changing library()'s instance (a library it opens stays
    open). */
LibraryStatus probeLibrary (const char* name);
} // namespace flub::platform::pipewire
