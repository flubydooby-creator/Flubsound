// Flubsound Pro - flush-to-zero / denormals-are-zero guard for the audio thread.
//
// Recursive IIR filters and envelope followers decay towards zero and would
// otherwise produce subnormal floats, which are 10-100x slower on x86. Every
// realtime entry point (device callback, plug-in processBlock, batch render)
// must hold a ScopedNoDenormals for the duration of processing.
//
// x86 / x64: MXCSR FTZ + DAZ. ARM64: FPCR.FZ, through inline asm (gcc, clang,
// clang-cl) or the _ReadStatusReg / _WriteStatusReg intrinsics (MSVC).
// Anything else: no-op.
#pragma once

#include <cstdint>

#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86_FP)
    #include <xmmintrin.h>
    #define FLUB_HAS_SSE_CSR 1
#elif defined(_M_ARM64) && ! defined(__aarch64__)
    // MSVC on ARM64 has no GNU inline asm: FPCR through the system-register
    // intrinsics. (clang-cl defines __aarch64__ too and takes the asm path.)
    #include <intrin.h>
    #define FLUB_HAS_MSVC_ARM64_FPCR 1
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
        asm volatile ("msr fpcr, %0" : : "r"(fpcr | kFpcrFz));
#elif defined(FLUB_HAS_MSVC_ARM64_FPCR)
        previous = static_cast<uint64_t> (_ReadStatusReg (kMsvcFpcr));
        _WriteStatusReg (kMsvcFpcr, static_cast<long long> (previous | kFpcrFz));
#endif
    }

    ~ScopedNoDenormals()
    {
#if defined(FLUB_HAS_SSE_CSR)
        _mm_setcsr (static_cast<unsigned int> (previous));
#elif defined(__aarch64__)
        asm volatile ("msr fpcr, %0" : : "r"(previous));
#elif defined(FLUB_HAS_MSVC_ARM64_FPCR)
        _WriteStatusReg (kMsvcFpcr, static_cast<long long> (previous));
#endif
    }

    ScopedNoDenormals (const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator= (const ScopedNoDenormals&) = delete;

private:
    [[maybe_unused]] static constexpr uint64_t kFpcrFz = 1ull << 24; // AArch64 FPCR.FZ (flush-to-zero)
#if defined(FLUB_HAS_MSVC_ARM64_FPCR)
    // ARM64_FPCR from <winnt.h>, spelled out so this header does not need
    // <windows.h>: ARM64_SYSREG (op0 3, op1 3, CRn 4, CRm 4, op2 0) =
    // (op0 & 1) << 14 | op1 << 11 | CRn << 7 | CRm << 3 | op2 = 0x5A20.
    static constexpr int kMsvcFpcr = (1 << 14) | (3 << 11) | (4 << 7) | (4 << 3) | 0;
#endif
    [[maybe_unused]] uint64_t previous = 0;
};
} // namespace flub
