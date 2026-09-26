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
    FactoryPresets.cpp
    FactoryPresets.h
    OfflineRenderer.cpp
    OfflineRenderer.h)

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

# Installed layout: bin/flubsound-cli + share/flubsound/presets/factory/*.json
include(GNUInstallDirs)
install(TARGETS flubsound-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(EXISTS "${PROJECT_SOURCE_DIR}/presets/factory")
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/presets/factory/"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/flubsound/presets/factory"
            FILES_MATCHING PATTERN "*.json")
endif()
