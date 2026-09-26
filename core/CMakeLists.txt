# flub_core - the JUCE-free DSP engine (static library).
#
# Contains everything that runs on the audio thread plus metering, presets
# and file I/O. It has no third-party dependencies so it can be hosted by the
# JUCE app, the plug-in, the batch CLI, unit tests, and (roadmap) a Windows
# APO or a PipeWire filter node without dragging a GUI framework along.

file(GLOB_RECURSE FLUB_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE FLUB_CORE_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/flub/*.h")

add_library(flub_core STATIC ${FLUB_CORE_SOURCES} ${FLUB_CORE_HEADERS})
add_library(flub::core ALIAS flub_core)

target_include_directories(flub_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(flub_core PUBLIC cxx_std_20)
target_link_libraries(flub_core PRIVATE flub::compiler_settings)
set_target_properties(flub_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)
target_link_libraries(flub_core PUBLIC Threads::Threads)
