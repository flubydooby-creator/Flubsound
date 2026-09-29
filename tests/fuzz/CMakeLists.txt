# Flubsound Pro - libFuzzer targets (docs/11 E53 step 1). Off by default:
#
#   CC=clang-20 CXX=clang++-20 cmake -S . -B build-fuzz -G Ninja \
#       -DFLUB_BUILD_FUZZERS=ON -DFLUB_BUILD_TESTS=OFF -DFLUB_BUILD_TOOLS=OFF
#   cmake --build build-fuzz
#   tests/fuzz/run-fuzzers.sh build-fuzz 30      # 30 s per target
#
#   fuzz_json    flub::json::parse / write
#   fuzz_preset  preset::migrate + preset::fromJson, toJson round trip
#   fuzz_state   saved strip A/B state, user device profiles, plug-in state recall
#   fuzz_eqtext  AutoEQ ParametricEQ.txt / Equalizer APO import (eqtext::parse)
#
# The fuzzers link their own copy of the core, built with
# -fsanitize=fuzzer-no-link,address,undefined, so flub_core and the other
# targets of the same build stay uninstrumented. Clang only (libFuzzer); a
# sanitizer report or a failed FUZZ_CHECK aborts and saves the input.

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "FLUB_BUILD_FUZZERS=ON needs Clang with libFuzzer (-fsanitize=fuzzer), but the C++ compiler is "
        "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}. Configure a fresh build directory with "
        "CC=clang-20 CXX=clang++-20 (Ubuntu: apt-get install clang-20 libclang-rt-20-dev).")
endif()
if(FLUB_RTSAN)
    message(FATAL_ERROR "FLUB_BUILD_FUZZERS cannot be combined with FLUB_RTSAN (ASan and RTSan exclude each other).")
endif()

set(FLUB_FUZZ_SANITIZERS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g)

file(GLOB_RECURSE FLUB_FUZZ_CORE_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/core/src/*.cpp")
add_library(flub_core_fuzz STATIC ${FLUB_FUZZ_CORE_SOURCES})
target_include_directories(flub_core_fuzz PUBLIC "${PROJECT_SOURCE_DIR}/core/include")
target_compile_features(flub_core_fuzz PUBLIC cxx_std_20)
target_compile_options(flub_core_fuzz PRIVATE -fsanitize=fuzzer-no-link ${FLUB_FUZZ_SANITIZERS})
target_link_libraries(flub_core_fuzz PRIVATE flub::compiler_settings)
find_package(Threads REQUIRED)
target_link_libraries(flub_core_fuzz PUBLIC Threads::Threads)

foreach(target IN ITEMS fuzz_json fuzz_preset fuzz_state fuzz_eqtext)
    add_executable(${target} "${CMAKE_CURRENT_SOURCE_DIR}/${target}.cpp")
    target_link_libraries(${target} PRIVATE flub_core_fuzz flub::compiler_settings)
    target_compile_options(${target} PRIVATE -fsanitize=fuzzer ${FLUB_FUZZ_SANITIZERS})
    target_link_options(${target} PRIVATE -fsanitize=fuzzer ${FLUB_FUZZ_SANITIZERS})
endforeach()
