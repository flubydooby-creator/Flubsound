// Flubsound Pro - the libpipewire entry points, resolved at run time (R1.2).
//
// Internal to PipeWireLibrary.cpp and PipeWireNative.cpp; include it after
// the libpipewire headers. Only the library's exported functions are here:
// what the headers implement inline (pw_core_*, pw_registry_*, pw_loop_*,
// pw_metadata_*, spa_*) calls through the objects' method tables and needs no
// symbol. Each member has the type the installed headers declare
// (decltype), so the table follows the PipeWire version built against.
// PipeWireNative.cpp redirects its calls to this table with one macro per
// entry point; a call that is missing there fails the link (the app does not
// link libpipewire), so nothing can reach the library by another way.
#pragma once

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>

// X (name): the entry point pw_<name>.
#define FLUB_PIPEWIRE_ENTRY_POINTS(X) \
    X (init)                          \
    X (get_library_version)           \
    X (thread_loop_new)               \
    X (thread_loop_get_loop)          \
    X (thread_loop_start)             \
    X (thread_loop_stop)              \
    X (thread_loop_destroy)           \
    X (thread_loop_lock)              \
    X (thread_loop_unlock)            \
    X (thread_loop_signal)            \
    X (thread_loop_get_time)          \
    X (thread_loop_timed_wait_full)   \
    X (context_new)                   \
    X (context_destroy)               \
    X (context_connect)               \
    X (core_disconnect)               \
    X (proxy_destroy)                 \
    X (proxy_add_listener)            \
    X (properties_new)                \
    X (properties_setf)               \
    X (properties_free)               \
    X (filter_new)                    \
    X (filter_add_listener)           \
    X (filter_add_port)               \
    X (filter_connect)                \
    X (filter_disconnect)             \
    X (filter_destroy)                \
    X (filter_get_node_id)            \
    X (filter_get_dsp_buffer)         \
    X (filter_update_properties)

namespace flub::platform::pipewire
{
struct Api
{
#define FLUB_PIPEWIRE_MEMBER(name) decltype (&::pw_##name) name = nullptr;
    FLUB_PIPEWIRE_ENTRY_POINTS (FLUB_PIPEWIRE_MEMBER)
#undef FLUB_PIPEWIRE_MEMBER
};

/** The resolved entry points; all null unless library().loaded. */
const Api& api();
} // namespace flub::platform::pipewire
