# Flubsound Pro - shared compiler settings, exposed as an INTERFACE target.

add_library(flub_compiler_settings INTERFACE)
add_library(flub::compiler_settings ALIAS flub_compiler_settings)

# RealtimeSanitizer: upstream Clang >= 20 only (GCC and MSVC have none; Apple
# Clang's version numbers do not map onto upstream ones, so it is refused too).
# Clang cannot combine it with ASan / UBSan.
if(FLUB_RTSAN)
    if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 20))
        message(FATAL_ERROR
            "FLUB_RTSAN=ON needs Clang >= 20 (-fsanitize=realtime), but the C++ compiler is "
            "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}. Configure a fresh build directory with "
            "CC=clang-20 CXX=clang++-20 (Ubuntu: apt-get install clang-20 libclang-rt-20-dev), or set FLUB_RTSAN=OFF.")
    endif()
    if(FLUB_SANITIZE)
        message(FATAL_ERROR "FLUB_RTSAN and FLUB_SANITIZE cannot be combined (Clang rejects -fsanitize=realtime "
            "together with address / undefined); use two build directories.")
    endif()
    if(FLUB_BUILD_PLUGIN)
        message(FATAL_ERROR "FLUB_RTSAN cannot be combined with FLUB_BUILD_PLUGIN: the RTSan-instrumented flub_core "
            "needs the RTSan runtime, which Clang links only into executables, so the VST3 shared module fails to "
            "link. Build the plug-in in a separate build directory.")
    endif()
endif()

if(MSVC)
    # /fp:precise (not /fp:fast): fast-math style folding would break the chain's
    # NaN/Inf input guard and the metering maths.
    target_compile_options(flub_compiler_settings INTERFACE /W4 /permissive- /Zc:__cplusplus /utf-8 /fp:precise)
    target_compile_definitions(flub_compiler_settings INTERFACE _USE_MATH_DEFINES NOMINMAX)
    if(FLUB_WARNINGS_AS_ERRORS)
        target_compile_options(flub_compiler_settings INTERFACE /WX)
    endif()
else()
    # No -ffast-math: it breaks NaN/Inf guards and denormal handling assumptions
    # in metering code. We opt into the safe subset instead.
    target_compile_options(flub_compiler_settings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
        -fno-math-errno)
    if(FLUB_WARNINGS_AS_ERRORS)
        target_compile_options(flub_compiler_settings INTERFACE -Werror)
    endif()
    if(FLUB_SANITIZE)
        target_compile_options(flub_compiler_settings INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(flub_compiler_settings INTERFACE -fsanitize=address,undefined)
    endif()
    if(FLUB_RTSAN)
        # RealtimeSanitizer (CI job 'rtsan'). FLUB_RTSAN=1 turns FLUB_NONBLOCKING
        # (flub/common/Realtime.h) into [[clang::nonblocking]] on the audio
        # entry points - ProcessingChain::process, MixEngine::process and every
        # Processor::process override - and RTSan aborts when anything they
        # reach at run time allocates, frees, locks or blocks. C++ only: the
        # C89 driver-header test has nothing to check. -Wno-function-effects:
        # Clang's compile-time effect analysis (off by default in Clang 20)
        # reports every call from an annotated function into a helper that is
        # not annotated itself (e.g. ModuleSlot::process); the
        # run-time check is the one we use, so -Werror builds keep it off.
        # See docs/08-pitfalls-and-solutions.md (E).
        target_compile_definitions(flub_compiler_settings INTERFACE FLUB_RTSAN=1)
        target_compile_options(flub_compiler_settings INTERFACE
            $<$<COMPILE_LANGUAGE:CXX>:-fsanitize=realtime -Wno-function-effects>)
        target_link_options(flub_compiler_settings INTERFACE -fsanitize=realtime)
    endif()
endif()
