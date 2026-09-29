// RealtimeSanitizer build checks (-DFLUB_RTSAN=ON, CI job 'rtsan'; in every
// other build this file compiles to nothing). They make sure that job is
// green because the audio entry points really are checked, not because the
// annotation or the sanitizer silently went missing:
//  * compile time: ProcessingChain::process, MixEngine::process, every
//    Processor::process and Processor::reset override, and the module
//    setters the audio thread calls (ProcessingChain::applyParameters, the
//    master limiter's setParams from MixEngine::setMasterCeilingDb, and the
//    TransientShaper setters inside BassEngine / ClarityEnhancer::setParams)
//    carry [[clang::nonblocking]] (in Clang the effect is part of the
//    function type, so std::is_same sees it);
//  * run time: a nonblocking function that allocates is stopped by RTSan.
//    That runs in a forked child so the expected report does not end this
//    test run; it needs halt_on_error=1 (RTSan's default, and what CI sets).
// Everything else in flub_tests then runs under RTSan unchanged: the tests
// call the annotated process() functions from ordinary (non-real-time) test
// code, and only what happens inside those calls is checked.
#if defined(FLUB_RTSAN)

#include "TestFramework.h"

#include "flub/analysis/CallbackTiming.h"
#include "flub/analysis/Discontinuity.h"
#include "flub/common/Realtime.h"
#include "flub/dsp/BackgroundTracker.h"
#include "flub/dsp/BassEngine.h"
#include "flub/dsp/ClarityEnhancer.h"
#include "flub/dsp/Compressor.h"
#include "flub/dsp/DeviceCorrection.h"
#include "flub/dsp/DynamicEq.h"
#include "flub/dsp/HeadphoneVirtualizer.h"
#include "flub/dsp/LoudnessMaximizer.h"
#include "flub/dsp/ParametricEq.h"
#include "flub/dsp/Saturator.h"
#include "flub/dsp/SpectralNoiseGate.h"
#include "flub/dsp/StereoSpatializer.h"
#include "flub/dsp/TransientShaper.h"
#include "flub/dsp/TruePeakLimiter.h"
#include "flub/engine/MixEngine.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/neural/AsyncModelProcessor.h"

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
static_assert (hasNonblockingProcess<DeviceCorrection>);
static_assert (hasNonblockingProcess<DynamicEq>);
static_assert (hasNonblockingProcess<HeadphoneVirtualizer>);
static_assert (hasNonblockingProcess<LoudnessMaximizer>);
static_assert (hasNonblockingProcess<ParametricEq>);
static_assert (hasNonblockingProcess<Saturator>);
static_assert (hasNonblockingProcess<SpectralNoiseGate>);
static_assert (hasNonblockingProcess<StereoSpatializer>);
static_assert (hasNonblockingProcess<TruePeakLimiter>);
static_assert (hasNonblockingProcess<AsyncModelProcessor>);
static_assert (hasNonblockingProcess<ProcessingChain>);
static_assert (std::is_same_v<decltype (&MixEngine::process),
                              void (MixEngine::*) (const AudioBlock* const*, const AudioBlock&) noexcept FLUB_NONBLOCKING>);

// reset(): ProcessingChain::resetSignalState (a NaN / Inf block), ModuleSlot::reset and
// ModuleSlot::process (re-activation) and applyParameters (virtualiser
// format change) call it on the audio thread.
template <class Module>
constexpr bool hasNonblockingReset = std::is_same_v<decltype (&Module::reset), void (Module::*)() noexcept FLUB_NONBLOCKING>;

static_assert (! std::is_same_v<void (Processor::*)() noexcept, void (Processor::*)() noexcept FLUB_NONBLOCKING>);
static_assert (hasNonblockingReset<Processor>);
static_assert (hasNonblockingReset<BassEngine>);
static_assert (hasNonblockingReset<ClarityEnhancer>);
static_assert (hasNonblockingReset<Compressor>);
static_assert (hasNonblockingReset<DeviceCorrection>);
static_assert (hasNonblockingReset<DynamicEq>);
static_assert (hasNonblockingReset<HeadphoneVirtualizer>);
static_assert (hasNonblockingReset<LoudnessMaximizer>);
static_assert (hasNonblockingReset<ParametricEq>);
static_assert (hasNonblockingReset<Saturator>);
static_assert (hasNonblockingReset<SpectralNoiseGate>);
static_assert (hasNonblockingReset<StereoSpatializer>);
static_assert (hasNonblockingReset<TruePeakLimiter>);
static_assert (hasNonblockingReset<TransientShaper>);
static_assert (hasNonblockingReset<AsyncModelProcessor>);
// The Compressor's relative upward floor (docs/11 E19) steps its background
// tracker per control tick.
static_assert (hasNonblockingReset<BackgroundTracker>);
static_assert (std::is_same_v<decltype (&BackgroundTracker::update), float (BackgroundTracker::*) (float, float) noexcept FLUB_NONBLOCKING>);
// The glitch detector (docs/11 E53) can watch a real-time stream.
static_assert (std::is_same_v<decltype (&DiscontinuityDetector::process), void (DiscontinuityDetector::*) (const float* const*, int) noexcept FLUB_NONBLOCKING>);
// The device callback's timing histograms (docs/11 E45): recorded once per
// callback by the app's AudioEngineHost.
static_assert (std::is_same_v<decltype (&CallbackTiming::record), void (CallbackTiming::*) (uint64_t, uint64_t, uint64_t) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&CallbackTiming::restartIntervals), void (CallbackTiming::*)() noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&TimingHistogram::add), void (TimingHistogram::*) (uint64_t) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&TimingHistogram::bucketFor), int (*) (uint64_t) noexcept FLUB_NONBLOCKING>);

// Parameter setters called once per block by ProcessingChain::applyParameters
// (TruePeakLimiter::setParams also by MixEngine::setMasterCeilingDb, which the
// app's device callback calls; the TransientShaper setters from BassEngine /
// ClarityEnhancer::setParams).
template <class Module, class Params>
constexpr bool hasNonblockingSetParams =
    std::is_same_v<decltype (&Module::setParams), void (Module::*) (const Params&) noexcept FLUB_NONBLOCKING>;

static_assert (hasNonblockingSetParams<BassEngine, BassEngineParams>);
static_assert (hasNonblockingSetParams<ClarityEnhancer, ClarityParams>);
static_assert (hasNonblockingSetParams<Compressor, CompressorParams>);
static_assert (hasNonblockingSetParams<HeadphoneVirtualizer, VirtualizerParams>);
static_assert (hasNonblockingSetParams<LoudnessMaximizer, MaximizerParams>);
static_assert (std::is_same_v<decltype (&LoudnessMaximizer::setUpstreamLiftDb), void (LoudnessMaximizer::*) (float) noexcept FLUB_NONBLOCKING>);
static_assert (hasNonblockingSetParams<Saturator, SaturatorParams>);
static_assert (hasNonblockingSetParams<SpectralNoiseGate, NoiseGateParams>);
static_assert (hasNonblockingSetParams<StereoSpatializer, SpatializerParams>);
static_assert (hasNonblockingSetParams<TruePeakLimiter, LimiterParams>);
static_assert (std::is_same_v<decltype (&DynamicEq::setBand), void (DynamicEq::*) (int, const DynEqBandParams&) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&ParametricEq::setBand), void (ParametricEq::*) (int, const EqBandParams&) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&ParametricEq::setOutputGainDb), void (ParametricEq::*) (float) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&TransientShaper::setAttackDb), void (TransientShaper::*) (float) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&TransientShaper::setSustainDb), void (TransientShaper::*) (float) noexcept FLUB_NONBLOCKING>);
// The automatic preamp's static-boost prediction (docs/11 E11), run by
// ProcessingChain::applyParameters when its inputs change.
static_assert (std::is_same_v<decltype (&ProcessingChain::buildStaticBoostModel),
                              void (*) (const float*, double, bool, ProcessingChain::StaticBoostModel&) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&ProcessingChain::predictStaticBoost),
                              headroom::Prediction (*) (const ProcessingChain::StaticBoostModel&, headroom::Weighting) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&ProcessingChain::StaticBoostModel::responseDb),
                              double (ProcessingChain::StaticBoostModel::*) (double) const noexcept FLUB_NONBLOCKING>);

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

// Measured THD+N (tests/test_distortion.cpp): the estimator and its 25 ms
// window, which the Saturator and the maximizer's clipper run inside
// process(), and the monitor / governor updates ProcessingChain::process
// calls every block.
#include "flub/dsp/DistortionEstimator.h"
#include "flub/dsp/WeightedResidual.h"
#include "flub/engine/Protection.h"

static_assert (std::is_same_v<decltype (&DistortionSums::add), void (DistortionSums::*) (float, float) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&DistortionSums::residualEnergy), double (DistortionSums::*)() const noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&DistortionSums::outputEnergy), double (DistortionSums::*)() const noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&DistortionSums::merge), void (DistortionSums::*) (const DistortionSums&) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&DistortionSums::isFinite), bool (DistortionSums::*)() const noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&DistortionWindow::advance), bool (DistortionWindow::*) (int, float&, float*) noexcept FLUB_NONBLOCKING>);
static_assert (hasNonblockingReset<DistortionWindow>);
static_assert (std::is_same_v<decltype (&DistortionEnergy::add), void (DistortionEnergy::*) (const DistortionSums&) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&DistortionEnergy::ratioDb), float (DistortionEnergy::*)() const noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&DistortionMonitor::update), float (DistortionMonitor::*) (float, float, int) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&DistortionMonitor::combineDb), float (*) (float, float) noexcept FLUB_NONBLOCKING>);
static_assert (hasNonblockingReset<DistortionMonitor>);
static_assert (std::is_same_v<decltype (&SafetyGovernor::update), void (SafetyGovernor::*) (float, float, int) noexcept FLUB_NONBLOCKING>);
static_assert (hasNonblockingReset<SafetyGovernor>);
// The governor's 10 ms tick grid, protection strength and hidden-block
// path (docs/11 E06 slice / E10 Phase 1, tests/test_protection_gaps.cpp).
static_assert (std::is_same_v<decltype (&SafetyGovernor::skip), void (SafetyGovernor::*) (int) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&SafetyGovernor::setStrength), void (SafetyGovernor::*) (ProtectionStrength) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&SafetyGovernor::restartTickGrid), void (SafetyGovernor::*)() noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&SafetyGovernor::samplesToNextTick), int (SafetyGovernor::*)() const noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&ProcessingChain::setProtectionStrength), void (ProcessingChain::*) (ProtectionStrength) noexcept FLUB_NONBLOCKING>);
// The measured loop at Normal / Strict (docs/11 E06 Phase 3,
// tests/test_protection_measured.cpp): the governor's update and settings,
// the feed-forward's and the PLR meter's per-tick work, and the span
// analyser ProcessingChain::process feeds every segment.
static_assert (std::is_same_v<decltype (&SafetyGovernor::updateMeasured), void (SafetyGovernor::*) (const SafetyGovernor::Readings&, int) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&SafetyGovernor::setMusicMode), void (SafetyGovernor::*) (bool) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&SafetyGovernor::setHarmonicsReplaceFundamental), void (SafetyGovernor::*) (bool) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&DriveFeedForward::push), void (DriveFeedForward::*) (float) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&DriveFeedForward::driveForBudget), float (DriveFeedForward::*) (float, float) const noexcept FLUB_NONBLOCKING>);
static_assert (hasNonblockingReset<DriveFeedForward>);
static_assert (hasNonblockingProcess<PlrMeter>);
static_assert (std::is_same_v<decltype (&PlrMeter::tick), void (PlrMeter::*)() noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&PlrMeter::getPlrDb), float (PlrMeter::*)() const noexcept FLUB_NONBLOCKING>);
static_assert (hasNonblockingReset<PlrMeter>);
static_assert (std::is_same_v<decltype (&WeightedResidual::process), void (WeightedResidual::*) (const float*, const float*, int) noexcept FLUB_NONBLOCKING>);
static_assert (hasNonblockingReset<WeightedResidual>);

// Neural slot (tests/test_neural_slot.cpp): ProcessingChain::process runs the
// slot's AsyncModelProcessor (process / reset asserted above) and publishes
// its counters; the bypass switch and the status / counter reads are atomics
// that any thread, the audio thread included, may call.
static_assert (std::is_same_v<decltype (&ProcessingChain::setNeuralBypass), void (ProcessingChain::*) (bool) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&ProcessingChain::getNeuralStatus), NeuralSlotStatus (ProcessingChain::*)() const noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&ProcessingChain::getNeuralCounters), NeuralSlotCounters (ProcessingChain::*)() const noexcept FLUB_NONBLOCKING>);

// The intentional harmonic generators (tests/test_distortion.cpp): the
// two-reference estimator the bass engine's harmonics and the air exciter
// run inside process(), their readings, and the monitor's harmonics update
// ProcessingChain::process calls every block.
#include "flub/dsp/ParallelDistortion.h"

static_assert (std::is_same_v<decltype (&ParallelDistortionSums::add), void (ParallelDistortionSums::*) (float, float, float) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&ParallelDistortionSums::residualEnergy), double (ParallelDistortionSums::*)() const noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&ParallelDistortionSums::outputEnergy), double (ParallelDistortionSums::*)() const noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&ParallelDistortionSums::merge), void (ParallelDistortionSums::*) (const ParallelDistortionSums&) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&ParallelDistortionSums::isFinite), bool (ParallelDistortionSums::*)() const noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&ParallelDistortionWindow::advance), bool (ParallelDistortionWindow::*) (int, float&) noexcept FLUB_NONBLOCKING>);
static_assert (hasNonblockingReset<ParallelDistortionWindow>);
static_assert (std::is_same_v<decltype (&BassEngine::getDistortionDb), float (BassEngine::*)() const noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&ClarityEnhancer::getDistortionDb), float (ClarityEnhancer::*)() const noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&DistortionMonitor::updateHarmonics), float (DistortionMonitor::*) (float, float, int) noexcept FLUB_NONBLOCKING>);

// The levelling and comparison loops ProcessingChain::process runs every
// block (tests/test_protection_gaps.cpp, docs/11 E21 / E37): AutoLevel and
// its gated measure with the upper gate, and the loudness-matched bypass.
static_assert (hasNonblockingProcess<GatedLoudness>);
static_assert (hasNonblockingProcess<AutoLevel>);
static_assert (std::is_same_v<decltype (&ComparisonMatcher::measureDry), void (ComparisonMatcher::*) (const AudioBlock&) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&ComparisonMatcher::measureWet), void (ComparisonMatcher::*) (const AudioBlock&) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&ComparisonMatcher::update), void (ComparisonMatcher::*) (bool, bool, int) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&ComparisonMatcher::updateUnmeasured), void (ComparisonMatcher::*) (bool, bool, int) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&AutoLevel::processUnmeasured), void (AutoLevel::*) (const AudioBlock&) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&ComparisonMatcher::applyWetTrim), void (ComparisonMatcher::*) (const AudioBlock&) noexcept FLUB_NONBLOCKING>);
// The Startle Guard ProcessingChain::process runs around the compressor slot
// (tests/test_dynamics_guard.cpp, docs/11 E21) and its per-block settings.
static_assert (std::is_same_v<decltype (&StartleGuard::measure), void (StartleGuard::*) (const AudioBlock&, bool) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&StartleGuard::apply), void (StartleGuard::*) (const AudioBlock&) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&StartleGuard::setCeilingLu), void (StartleGuard::*) (float) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&StartleGuard::setLevelOffsetDb), void (StartleGuard::*) (float) noexcept FLUB_NONBLOCKING>);
static_assert (hasNonblockingReset<StartleGuard>);

// The loudness contour ProcessingChain::process runs after the preamp
// (tests/test_contour.cpp, docs/11 E32): its per-block setParams re-designs
// on the audio thread (ISO 226, the fit, the headroom prediction), the
// chain reads its target for the automatic preamp, and a host sets the
// listening level from any thread.
#include "flub/dsp/LoudnessContour.h"
static_assert (hasNonblockingProcess<LoudnessContour>);
static_assert (hasNonblockingReset<LoudnessContour>);
static_assert (hasNonblockingSetParams<LoudnessContour, LoudnessContourParams>);
static_assert (std::is_same_v<decltype (&LoudnessContour::targetLiftDb), double (LoudnessContour::*) (double) const noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&LoudnessContour::getTargetSections), int (LoudnessContour::*) (SvfCoeffs*) const noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&iso226::splDb), double (*) (int, double) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&iso226::relativeGainDb), double (*) (int, double, double) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&ProcessingChain::setListeningLevelDb), void (ProcessingChain::*) (float) noexcept FLUB_NONBLOCKING>);

// The surround fold and input-channel detection (tests/test_virtualizer_fold.cpp,
// docs/11 E01 / E27): ProcessingChain::process runs the detector and the
// BS.775 fold every block of a 5.1 / 7.1 strip, applyParameters sets the
// fold's LFE level, HeadphoneVirtualizer renders its LFE through the same
// LfeFold, and a host may ask for re-detection from any thread.
#include "flub/dsp/ActiveChannelDetector.h"
#include "flub/dsp/Bs775Fold.h"

static_assert (hasNonblockingProcess<ActiveChannelDetector>);
static_assert (hasNonblockingReset<ActiveChannelDetector>);
static_assert (std::is_same_v<decltype (&Bs775Fold::process), void (Bs775Fold::*) (const AudioBlock&, float) noexcept FLUB_NONBLOCKING>);
static_assert (hasNonblockingReset<Bs775Fold>);
static_assert (std::is_same_v<decltype (&Bs775Fold::setLfeGain), void (Bs775Fold::*) (float) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&LfeFold::addTo), void (LfeFold::*) (const float*, float*, float*, int) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&LfeFold::addToMono), void (LfeFold::*) (const float*, float*, int) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&LfeFold::skip), void (LfeFold::*) (int) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&LfeFold::setGain), void (LfeFold::*) (float) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&LfeFold::clearState), void (LfeFold::*)() noexcept FLUB_NONBLOCKING>);
static_assert (hasNonblockingReset<LfeFold>);
static_assert (std::is_same_v<decltype (&ProcessingChain::redetectInputChannels), void (ProcessingChain::*)() noexcept FLUB_NONBLOCKING>);

// The Smoothness stage (its slot, the reference the chain hands it every
// segment) and the tonal-balance rule's meter, fed and ticked by
// ProcessingChain::process at protection strength Normal / Strict
// (docs/11 E07, tests/test_smoothness.cpp).
#include "flub/dsp/SmoothnessGuard.h"
#include "flub/dsp/TonalBalanceMeter.h"

static_assert (hasNonblockingProcess<SmoothnessGuard>);
static_assert (hasNonblockingReset<SmoothnessGuard>);
static_assert (hasNonblockingSetParams<SmoothnessGuard, SmoothnessParams>);
static_assert (std::is_same_v<decltype (&SmoothnessGuard::setReference), void (SmoothnessGuard::*) (const AudioBlock&) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&TonalBalanceMeter::processReference), void (TonalBalanceMeter::*) (const AudioBlock&) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&TonalBalanceMeter::processOutput), void (TonalBalanceMeter::*) (const AudioBlock&) noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&TonalBalanceMeter::tick), void (TonalBalanceMeter::*)() noexcept FLUB_NONBLOCKING>);
static_assert (std::is_same_v<decltype (&TonalBalanceMeter::getLiftDb), float (TonalBalanceMeter::*) (int) const noexcept FLUB_NONBLOCKING>);
static_assert (hasNonblockingReset<TonalBalanceMeter>);


// The device callback's kernel thread id, recorded on the first callback of
// each new device thread for the message thread's RealtimeKit request
// (docs/11 E44, app/Source/platform; tests/test_platform_linux.cpp calls it).
#include "../app/Source/platform/PlatformServices.h"

static_assert (std::is_same_v<decltype (&flub::platform::RealtimeScheduling::currentThreadId), uint64_t (*)() noexcept FLUB_NONBLOCKING>);

#endif // FLUB_RTSAN
