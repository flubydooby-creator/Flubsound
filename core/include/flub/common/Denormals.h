// Flubsound Pro - flush-to-zero / denormals-are-zero guard for the audio thread.
//
// Recursive IIR filters and envelope followers decay towards zero and would
// otherwise produce subnormal floats, which are 10-100x slower on x86. Every
// realtime entry point (device callback, plug-in processBlock, batch render)
// must hold a ScopedNoDenormals for the duration of processing.
#pragma once

#include <cstdint>

#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86_FP)
    #include <xmmintrin.h>
    #define FLUB_HAS_SSE_CSR 1
#endif

namespace flub
{
class ScopedNoDenormals
{
public:
    ScopedNoDenormals() noexcept
    {
#if defined(FLUB_HAS_SSE_CSR)
        previous = _mm_getcsr();
        _mm_setcsr (static_cast<unsigned int> (previous | 0x8040u)); // FTZ (bit 15) | DAZ (bit 6)
#elif defined(__aarch64__)
        uint64_t fpcr;
        asm volatile ("mrs %0, fpcr" : "=r"(fpcr));
        previous = fpcr;
        asm volatile ("msr fpcr, %0" : : "r"(fpcr | (1ull << 24))); // FZ
#endif
    }

    ~ScopedNoDenormals()
    {
#if defined(FLUB_HAS_SSE_CSR)
        _mm_setcsr (static_cast<unsigned int> (previous));
#elif defined(__aarch64__)
        asm volatile ("msr fpcr, %0" : : "r"(previous));
#endif
    }

    ScopedNoDenormals (const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator= (const ScopedNoDenormals&) = delete;

private:
    [[maybe_unused]] uint64_t previous = 0;
};
} // namespace flub
