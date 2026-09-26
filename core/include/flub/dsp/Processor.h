// Flubsound Pro - the contract every DSP module implements.
//
// Threading contract. Enforced by review, by the allocation-counting tests
// in tests/ (flubtest::AllocationGuard around process() / reset() /
// setters) and, for process(), by RealtimeSanitizer: the base and every
// override are declared FLUB_NONBLOCKING (flub/common/Realtime.h), which the
// FLUB_RTSAN build (Clang 20, CI job 'rtsan') turns into
// [[clang::nonblocking]], so an allocation, free, lock or blocking system
// call anywhere below process() aborts the test run. Declare new overrides
// the same way (and list them in tests/test_rtsan.cpp):
//   void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING override;
//
//   prepare()  : non-realtime thread only. May allocate, may be slow.
//   reset()    : audio thread allowed. No allocation, no locks, no I/O.
//   process()  : audio thread. No allocation, no locks, no I/O, no exceptions,
//                bounded execution time (no data-dependent loops over history).
//   setters    : called on the audio thread at block start by ProcessingChain
//                (it copies values out of the lock-free ParameterStore), so they
//                must be RT-safe too; modules smooth internally.
//   latencySamples(): constant between prepare() calls. The chain sums module
//                latencies and compensates dry paths, so a module must never
//                change its latency without being re-prepared.
#pragma once

#include "flub/common/AudioBlock.h"
#include "flub/common/Realtime.h"

namespace flub
{
struct ProcessSpec
{
    double sampleRate = 48000.0;
    int maxBlockSize = 512;
    int numChannels = 2;
};

class Processor
{
public:
    virtual ~Processor() = default;

    virtual void prepare (const ProcessSpec& spec) = 0;
    virtual void reset() noexcept = 0;

    /** In-place processing. block.numSamples <= spec.maxBlockSize and
        block.numChannels <= spec.numChannels are guaranteed by the caller. */
    virtual void process (const AudioBlock& block) noexcept FLUB_NONBLOCKING = 0;

    virtual int latencySamples() const noexcept { return 0; }
    virtual const char* name() const noexcept = 0;
};
} // namespace flub
