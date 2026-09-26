# Flubsound Pro - shared compiler settings, exposed as an INTERFACE target.

add_library(flub_compiler_settings INTERFACE)
add_library(flub::compiler_settings ALIAS flub_compiler_settings)

if(MSVC)
    target_compile_options(flub_compiler_settings INTERFACE /W4 /permissive- /Zc:__cplusplus /utf-8 /fp:fast
        $<$<CONFIG:Release>:/O2 /Oi /GL>)
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
        # RealtimeSanitizer: functions marked [[clang::nonblocking]] abort on
        # malloc/free, locks, syscalls. See docs/08-pitfalls-and-solutions.md.
        target_compile_options(flub_compiler_settings INTERFACE -fsanitize=realtime)
        target_link_options(flub_compiler_settings INTERFACE -fsanitize=realtime)
    endif()
endif()
