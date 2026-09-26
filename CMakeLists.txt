cmake_minimum_required(VERSION 3.22)

project(FlubsoundPro
    VERSION 0.1.0
    DESCRIPTION "Flubsound Pro - Music & Gaming Edition: real-time audio enhancement"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(FLUB_BUILD_TESTS "Build the DSP unit tests" ON)
option(FLUB_BUILD_TOOLS "Build the flubsound-cli batch processor" ON)
option(FLUB_BUILD_APP "Build the JUCE desktop application (fetches JUCE)" OFF)
option(FLUB_BUILD_PLUGIN "Build the VST3/AU/Standalone plug-in (fetches JUCE)" OFF)
option(FLUB_BUILD_APP_TESTS "Build the app-level tests (flub_app_tests) together with the app" ON)
option(FLUB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(FLUB_SANITIZE "Build with Address/UndefinedBehavior sanitizers" OFF)
option(FLUB_RTSAN "Build with Clang RealtimeSanitizer (clang >= 20)" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(FlubCompilerSettings)

add_subdirectory(core)

if(FLUB_BUILD_TESTS OR (FLUB_BUILD_APP AND FLUB_BUILD_APP_TESTS))
    enable_testing()
endif()

if(FLUB_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(FLUB_BUILD_TOOLS)
    add_subdirectory(tools/flubsound-cli)
endif()

if(FLUB_BUILD_APP OR FLUB_BUILD_PLUGIN)
    include(FlubJuce)
endif()

if(FLUB_BUILD_APP)
    add_subdirectory(app)
endif()

if(FLUB_BUILD_PLUGIN)
    add_subdirectory(plugin)
endif()
