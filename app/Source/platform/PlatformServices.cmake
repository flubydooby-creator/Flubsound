# Flubsound Pro - build settings for the platform services.
#
# Included (OPTIONAL) by app/CMakeLists.txt right after the OS implementation
# (PlatformServices_<os>) and PlatformServices_common.cpp were added to the
# FlubsoundPro target as FLUB_PLATFORM_SOURCES.

if(NOT TARGET FlubsoundPro OR NOT FLUB_HAS_PLATFORM_SERVICES)
    return()
endif()

option(FLUB_ENABLE_UNDOCUMENTED_ROUTING
    "Windows: move apps between output devices through the undocumented IAudioPolicyConfigFactory API (validate on every supported Windows build first)"
    OFF)

if(WIN32)
    # See the header comment of PlatformServices_win.cpp for what each is used for.
    target_link_libraries(FlubsoundPro PRIVATE ole32 user32 shell32 shlwapi version avrt mmdevapi)

    if(FLUB_ENABLE_UNDOCUMENTED_ROUTING)
        set_property(SOURCE "${FLUB_PLATFORM_DIR}/PlatformServices_win.cpp"
            APPEND PROPERTY COMPILE_DEFINITIONS FLUB_ENABLE_UNDOCUMENTED_ROUTING=1)
        message(STATUS "Flubsound: undocumented per-app routing adapter ENABLED (Windows)")
    endif()
elseif(APPLE)
    # Carbon: RegisterEventHotKey. AppKit: NSWorkspace (already linked by JUCE).
    target_link_libraries(FlubsoundPro PRIVATE "-framework Carbon" "-framework AppKit")
endif()

# Our platform sources compile warning-free with the core's strict set.
if(MSVC)
    set(FLUB_PLATFORM_WARNINGS /W4 /permissive- /utf-8)
else()
    set(FLUB_PLATFORM_WARNINGS -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion)
endif()
set_source_files_properties(${FLUB_PLATFORM_SOURCES} PROPERTIES COMPILE_OPTIONS "${FLUB_PLATFORM_WARNINGS}")
