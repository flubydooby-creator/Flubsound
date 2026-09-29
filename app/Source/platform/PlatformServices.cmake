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
    target_link_libraries(FlubsoundPro PRIVATE ole32 user32 shell32 shlwapi version avrt mmdevapi advapi32)

    if(FLUB_ENABLE_UNDOCUMENTED_ROUTING)
        set_property(SOURCE "${FLUB_PLATFORM_DIR}/PlatformServices_win.cpp"
            APPEND PROPERTY COMPILE_DEFINITIONS FLUB_ENABLE_UNDOCUMENTED_ROUTING=1)
        message(STATUS "Flubsound: undocumented per-app routing adapter ENABLED (Windows)")
    endif()
elseif(APPLE)
    # Carbon: RegisterEventHotKey. AppKit: NSWorkspace (already linked by JUCE).
    # ServiceManagement: SMAppService (start at login, macOS 13+; weak-referenced
    # through @available, the framework itself exists on every supported macOS).
    target_link_libraries(FlubsoundPro PRIVATE "-framework Carbon" "-framework AppKit" "-framework ServiceManagement")
else()
    # docs/11 E48: the native PipeWire node, registry linking and the
    # "PipeWire" device type, built when pkg-config finds libpipewire-0.3
    # (Debian / Ubuntu libpipewire-0.3-dev, Fedora pipewire-devel, Arch
    # libpipewire). Optional: without it the app keeps the JUCE ALSA / JACK
    # path, links with pw-dump / pw-link, and NativeAudioNode::isSupported()
    # is false.
    option(FLUB_WITH_PIPEWIRE "Linux: build the native PipeWire node when libpipewire-0.3 is found" ON)
    set(FLUB_HAS_PIPEWIRE 0)
    if(FLUB_WITH_PIPEWIRE)
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(FLUB_PIPEWIRE QUIET IMPORTED_TARGET libpipewire-0.3>=0.3.48)
        endif()
        if(FLUB_PIPEWIRE_FOUND)
            set(FLUB_HAS_PIPEWIRE 1)
            set(FLUB_PIPEWIRE_SOURCES
                "${FLUB_PLATFORM_DIR}/pipewire/PipeWireNative.cpp"
                "${FLUB_PLATFORM_DIR}/pipewire/PipeWireDeviceType.cpp")
            target_sources(FlubsoundPro PRIVATE ${FLUB_PIPEWIRE_SOURCES})
            list(APPEND FLUB_PLATFORM_SOURCES ${FLUB_PIPEWIRE_SOURCES})
            # An imported target: its include directories are system headers,
            # so the strict warnings below do not apply to libpipewire's own.
            target_link_libraries(FlubsoundPro PRIVATE PkgConfig::FLUB_PIPEWIRE)
            message(STATUS "Flubsound: native PipeWire node enabled (libpipewire-0.3 ${FLUB_PIPEWIRE_VERSION})")
        else()
            message(STATUS "Flubsound: libpipewire-0.3 not found - no native PipeWire node (JUCE ALSA / JACK and pw-link only)")
        endif()
    endif()
    target_compile_definitions(FlubsoundPro PRIVATE FLUB_HAS_PIPEWIRE=${FLUB_HAS_PIPEWIRE})
endif()

# Our platform sources compile warning-free with the core's strict set.
if(MSVC)
    set(FLUB_PLATFORM_WARNINGS /W4 /permissive- /utf-8)
else()
    set(FLUB_PLATFORM_WARNINGS -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion)
endif()
set_source_files_properties(${FLUB_PLATFORM_SOURCES} PROPERTIES COMPILE_OPTIONS "${FLUB_PLATFORM_WARNINGS}")

# The PipeWire device type is JUCE code (juce::AudioIODeviceType): the app's
# own warning set (app/CMakeLists.txt), since JUCE's headers are not clean
# under -Wconversion.
if(FLUB_HAS_PIPEWIRE)
    set_source_files_properties("${FLUB_PLATFORM_DIR}/pipewire/PipeWireDeviceType.cpp" PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wshadow;-Wno-sign-conversion")
endif()
