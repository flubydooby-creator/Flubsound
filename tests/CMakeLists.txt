file(GLOB FLUB_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(flub_tests ${FLUB_TEST_SOURCES})
target_link_libraries(flub_tests PRIVATE flub::core flub::compiler_settings)
target_include_directories(flub_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME flub_tests COMMAND flub_tests)

# Factory presets are validated by tests/test_factory_presets.cpp.
target_compile_definitions(flub_tests PRIVATE FLUB_PRESET_DIR="${PROJECT_SOURCE_DIR}/presets/factory")
target_compile_definitions(flub_tests PRIVATE FLUB_DEVICE_PROFILES="${PROJECT_SOURCE_DIR}/presets/devices/device-profiles.json")

# platform/windows/driver/FlubVirtualAudioShared.h (driver <-> engine ABI) is
# plain C that needs no Windows headers outside kernel mode:
# test_driver_shared.cpp checks its layout on every platform, and
# test_driver_shared_c.c compiles it as strict C89 like the driver does
# (gcc / clang only: MSVC has no C89 mode).
target_include_directories(flub_tests PRIVATE "${PROJECT_SOURCE_DIR}/platform/windows/driver")
if(NOT MSVC)
    target_sources(flub_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/test_driver_shared_c.c")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/test_driver_shared_c.c" PROPERTIES COMPILE_OPTIONS "-std=c89")
    target_compile_definitions(flub_tests PRIVATE FLUB_TEST_DRIVER_SHARED_C=1)
endif()

# tools/flubsound-cli is plain C++ on flub::core: test_offline_render.cpp runs
# its offline renderer, `process` / `batch` glue and reports directly, so its
# sources (all but main.cpp) are compiled into the tests. This does not depend
# on FLUB_BUILD_TOOLS, so the sanitizer jobs cover it too.
set(FLUB_CLI_DIR "${PROJECT_SOURCE_DIR}/tools/flubsound-cli")
target_sources(flub_tests PRIVATE
    "${FLUB_CLI_DIR}/Analysis.cpp"
    "${FLUB_CLI_DIR}/CliOptions.cpp"
    "${FLUB_CLI_DIR}/Commands.cpp"
    "${FLUB_CLI_DIR}/FactoryPresets.cpp"
    "${FLUB_CLI_DIR}/OfflineRenderer.cpp"
    "${FLUB_CLI_DIR}/Soak.cpp")
target_include_directories(flub_tests PRIVATE "${FLUB_CLI_DIR}")
target_compile_definitions(flub_tests PRIVATE FLUB_SOURCE_PRESET_DIR="${PROJECT_SOURCE_DIR}/presets/factory")
if(WIN32)
    target_link_libraries(flub_tests PRIVATE shell32) # Utf8Windows.h: CommandLineToArgvW
endif()
