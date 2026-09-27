// Flubsound Pro - real-time annotations for the audio-thread entry points.
//
// FLUB_NONBLOCKING marks a function [[clang::nonblocking]] in a
// RealtimeSanitizer build (-DFLUB_RTSAN=ON, Clang >= 20, which defines
// FLUB_RTSAN; see cmake/FlubCompilerSettings.cmake) and expands to nothing
// in every other build. RTSan treats a call to such a function as entering a
// real-time context: until it returns, anything it reaches at run time that
// allocates or frees memory, locks a mutex or makes a blocking system call
// aborts with a stack trace (CI job 'rtsan'). Code a test calls outside
// that extent is not checked.
//
// Annotated today: ProcessingChain::process, MixEngine::process, every
// Processor::process and Processor::reset override, and the parameter
// setters the audio thread calls from ProcessingChain::applyParameters
// (tests/test_rtsan.cpp lists them). Placement is after noexcept, on the
// declaration and the definition:
//   void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING override;
#pragma once

#if defined(FLUB_RTSAN) && defined(__clang__) && defined(__has_cpp_attribute)
    #if __has_cpp_attribute(clang::nonblocking)
        #define FLUB_NONBLOCKING [[clang::nonblocking]]
    #endif
#endif

#ifndef FLUB_NONBLOCKING
    #define FLUB_NONBLOCKING
#endif
