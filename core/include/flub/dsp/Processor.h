// Flubsound Pro - the contract every DSP module implements.
//
// Threading contract. Enforced by review and, for "no allocation", by the
// allocation-counting tests in tests/ (flubtest::AllocationGuard around
// process() / reset() / setters). FLUB_RTSAN is an optional local build
// (clang >= 20), not a CI job, and RealtimeSanitizer only checks functions
// marked [[clang::nonblocking]] - none are yet.
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
    virtual void process (const AudioBlock& block) noexcept = 0;

    virtual int latencySamples() const noexcept { return 0; }
    virtual const char* name() const noexcept = 0;
};
} // namespace flub
