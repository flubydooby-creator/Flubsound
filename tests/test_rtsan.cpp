// RealtimeSanitizer build checks (-DFLUB_RTSAN=ON, CI job 'rtsan'; in every
// other build this file compiles to nothing). They make sure that job is
// green because the audio entry points really are checked, not because the
// annotation or the sanitizer silently went missing:
//  * compile time: ProcessingChain::process, MixEngine::process and every
//    Processor::process override carry [[clang::nonblocking]] (in Clang the
//    effect is part of the function type, so std::is_same sees it);
//  * run time: a nonblocking function that allocates is stopped by RTSan.
//    That runs in a forked child so the expected report does not end this
//    test run; it needs halt_on_error=1 (RTSan's default, and what CI sets).
// Everything else in flub_tests then runs under RTSan unchanged: the tests
// call the annotated process() functions from ordinary (non-real-time) test
// code, and only what happens inside those calls is checked.
#if defined(FLUB_RTSAN)

#include "TestFramework.h"

#include "flub/common/Realtime.h"
#include "flub/dsp/BassEngine.h"
#include "flub/dsp/ClarityEnhancer.h"
#include "flub/dsp/Compressor.h"
#include "flub/dsp/DynamicEq.h"
#include "flub/dsp/HeadphoneVirtualizer.h"
#include "flub/dsp/LoudnessMaximizer.h"
#include "flub/dsp/ParametricEq.h"
#include "flub/dsp/Saturator.h"
#include "flub/dsp/SpectralNoiseGate.h"
#include "flub/dsp/StereoSpatializer.h"
#include "flub/dsp/TruePeakLimiter.h"
#include "flub/engine/MixEngine.h"
#include "flub/engine/ProcessingChain.h"

#include <type_traits>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
    #include <fcntl.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

using namespace flub;

namespace
{
template <class Module>
constexpr bool hasNonblockingProcess = std::is_same_v<decltype (&Module::process), void (Module::*) (const AudioBlock&) noexcept FLUB_NONBLOCKING>;

// Not vacuous: without the attribute the type differs.
static_assert (! std::is_same_v<void (Processor::*) (const AudioBlock&) noexcept, void (Processor::*) (const AudioBlock&) noexcept FLUB_NONBLOCKING>,
               "FLUB_RTSAN is defined but FLUB_NONBLOCKING expanded to nothing");

static_assert (hasNonblockingProcess<Processor>);
static_assert (hasNonblockingProcess<BassEngine>);
static_assert (hasNonblockingProcess<ClarityEnhancer>);
static_assert (hasNonblockingProcess<Compressor>);
static_assert (hasNonblockingProcess<DynamicEq>);
static_assert (hasNonblockingProcess<HeadphoneVirtualizer>);
static_assert (hasNonblockingProcess<LoudnessMaximizer>);
static_assert (hasNonblockingProcess<ParametricEq>);
static_assert (hasNonblockingProcess<Saturator>);
static_assert (hasNonblockingProcess<SpectralNoiseGate>);
static_assert (hasNonblockingProcess<StereoSpatializer>);
static_assert (hasNonblockingProcess<TruePeakLimiter>);
static_assert (hasNonblockingProcess<ProcessingChain>);
static_assert (std::is_same_v<decltype (&MixEngine::process),
                              void (MixEngine::*) (const AudioBlock* const*, const AudioBlock&) noexcept FLUB_NONBLOCKING>);

#if defined(__linux__) || defined(__APPLE__)
/** Deliberately breaks the real-time contract: the vector escapes, so the
    allocation cannot be optimised away. */
[[gnu::noinline]] void growInRealtimeContext (std::vector<float>& v) noexcept FLUB_NONBLOCKING
{
    v.resize (v.size() + 4096);
}
#endif
} // namespace

#if defined(__linux__) || defined(__APPLE__)
TEST_CASE ("RTSan: an allocation inside a nonblocking function stops the process (sanitizer self-test)")
{
    const pid_t child = fork();
    REQUIRE (child >= 0);
    if (child == 0)
    {
        // The report is expected; keep it out of the test log.
        if (const int devNull = open ("/dev/null", O_WRONLY); devNull >= 0)
            dup2 (devNull, STDERR_FILENO);
        std::vector<float> v;
        growInRealtimeContext (v);
        _exit (0); // reached only if RTSan did not intervene
    }
    int status = 0;
    REQUIRE (waitpid (child, &status, 0) == child);
    CHECK (! (WIFEXITED (status) && WEXITSTATUS (status) == 0));
}
#endif

#endif // FLUB_RTSAN
