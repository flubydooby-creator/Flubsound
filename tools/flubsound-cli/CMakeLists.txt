# flubsound-cli - headless batch processor / loudness analyser (no JUCE).
#
# Hosts exactly the same flub::ProcessingChain as the app and the plug-in, so
# an offline render sounds like real-time playback, sample for sample.
#
# Factory presets are looked up at run time (see FactoryPresets.cpp):
#   --dir / --preset-dir, $FLUBSOUND_PRESET_DIR, then presets/factory next to
#   the executable (or in any parent folder, or ../share/flubsound/...), and
#   finally the source tree this binary was built from (FLUB_SOURCE_PRESET_DIR).

set(FLUB_CLI_SOURCES
    main.cpp
    Analysis.cpp
    Analysis.h
    CliOptions.cpp
    CliOptions.h
    Commands.cpp
    Commands.h
    FactoryPresets.cpp
    FactoryPresets.h
    OfflineRenderer.cpp
    OfflineRenderer.h
    Utf8Windows.h)

add_executable(flubsound-cli ${FLUB_CLI_SOURCES})

target_link_libraries(flubsound-cli PRIVATE flub::core flub::compiler_settings)
target_compile_features(flubsound-cli PRIVATE cxx_std_20)
target_compile_definitions(flubsound-cli PRIVATE
    FLUB_CLI_VERSION="${PROJECT_VERSION}"
    FLUB_SOURCE_PRESET_DIR="${PROJECT_SOURCE_DIR}/presets/factory")

# std::thread for `batch --jobs N` (flub::core already links Threads publicly;
# stated again because the CLI uses it directly).
find_package(Threads REQUIRED)
target_link_libraries(flubsound-cli PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(flubsound-cli PRIVATE shell32) # CommandLineToArgvW
endif()

# Installed layout: bin/flubsound-cli + share/flubsound/presets/factory/*.json
# Top-level *.json only - the same set the app embeds and
# tests/test_factory_presets.cpp validates (sub-folders never ship).
include(GNUInstallDirs)
install(TARGETS flubsound-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
file(GLOB FLUB_CLI_FACTORY_PRESETS CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/presets/factory/*.json")
if(FLUB_CLI_FACTORY_PRESETS)
    install(FILES ${FLUB_CLI_FACTORY_PRESETS}
            DESTINATION "${CMAKE_INSTALL_DATADIR}/flubsound/presets/factory")
endif()
