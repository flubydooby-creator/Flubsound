#include "flub/engine/ProcessingChain.h"

#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>

namespace flub
{
using namespace param;

namespace
{
inline bool on (const float* e, int id) noexcept { return e[id] >= 0.5f; }
inline int idx (const float* e, int id) noexcept { return static_cast<int> (std::lround (e[id])); }

ChannelLayout layoutFor (int channels) noexcept
{
    return channels >= 8 ? ChannelLayout::Surround71 : (channels >= 6 ? ChannelLayout::Surround51 : ChannelLayout::Stereo);
}

// Stereo passthrough fold <-> surround fold (docs/11 E27): a 300-500 ms
// ramp, so the level change between the folds (BS.775 -3.01 dB, virtualiser
// about -0.8 dB on FL/FR-only content) is a glide, not a step.
constexpr float kPassFadeMs = 400.0f;
// Smoothing of the block statistics that power-compensate that ramp.
constexpr double kPassStatsMs = 50.0;

/** Whether a 5.1 / 7.1 strip folds as stereo: the manual override, else a
    game that renders its own HRTF, else the detector. */
bool stereoFoldFor (const float* e, ActiveChannelDetector::Fold detected) noexcept
{
    switch (static_cast<InputModeValue> (idx (e, VirtInputMode)))
    {
        case InputModeValue::ForceSurround: return false;
        case InputModeValue::ForceStereo: return true;
        case InputModeValue::Auto:
        default: return on (e, VirtOwnHrtf) || detected == ActiveChannelDetector::Fold::Stereo;
    }
}

DynEqBandParams modeBand (DynEqMode mode, EqBandType shape, float freq, float q, float thr, float ratio, float range,
                          float attack, float release, float floor) noexcept
{
    DynEqBandParams p;
    p.enabled = range > 0.01f;
    p.mode = mode;
    p.shape = shape;
    p.frequency = freq;
    p.q = q;
    p.thresholdDb = thr;
    p.ratio = ratio;
    p.rangeDb = range;
    p.attackMs = attack;
    p.releaseMs = release;
    p.noiseFloorDb = floor;
    return p;
}

/** Dynamic-EQ bands 4..7 belong to the mode policy, not to the user: they are
    the fixed-function "footsteps / anti-masking / voice" (Gaming) and
    "de-harsh / air / de-boom" (Music) processors, scaled by the macros. */
// Centre / corner frequencies of the internal dynamic-EQ mode bands 4..7
// (also reported to the GUI through ProcessingChain::modeBandFrequency).
constexpr std::array<float, ProcessingChain::kNumModeBands> kGamingModeBandHz { 3200.0f, 260.0f, 90.0f, 2000.0f };
constexpr std::array<float, ProcessingChain::kNumModeBands> kMusicModeBandHz { 3500.0f, 12000.0f, 120.0f, 1000.0f };

// Footsteps bands 4 / 5 are the cue enhancer (docs/11 E19): DynEqMode::CueLift
// lifts what rises out of the band's own background (a step under a bed from
// its first milliseconds, at any programme level) and neither the stationary
// bed nor loud events (see DynamicEq.h). Attack is the lift's rise once a
// cue is detected, release its fall after the 30 ms hold.
constexpr float kCueDetailAttackMs = 1.0f, kCueDetailReleaseMs = 40.0f;
constexpr float kCueBodyAttackMs = 2.0f, kCueBodyReleaseMs = 60.0f;
// At and below this rate the output is a Bluetooth hands-free / speech link
// (8 / 16 / 32 kHz): the 3.2 kHz footsteps bell would sit at 0.2..0.8 x
// Nyquist of an already harsh narrowband channel, so it is off (docs/11 E17).
constexpr double kSpeechLinkMaxRate = 32000.0;

// Latency profiles are defined in time, not samples (docs/11 E42a), so a
// profile costs about the same milliseconds at every rate:
//  * The compressor / maximizer look-aheads are set in ms (setLookaheadMs).
//  * The noise gate's STFT frame is kGateFrameMs (1024 samples at 48 kHz),
//    rounded to the nearest power of two in log terms: 1024 at 44.1 / 48 kHz,
//    2048 at 88.2 / 96 kHz, 4096 at 176.4 / 192 kHz (21.3 - 23.2 ms). Its
//    bin spacing in Hz, and so the gate's behaviour, stays the same too.
//  * The oversampler FIRs and the true-peak detector stay in samples: they
//    are filters defined relative to Nyquist and shrink at higher rates.
// Below kQualityMinRate (Bluetooth hands-free / speech links, 8 / 16 / 22.05
// kHz) Quality runs as Balanced: a 1024-sample frame alone was 128 ms at
// 8 kHz, and the chain 1152 samples (144 ms), which breaks lip sync.
constexpr double kGateFrameMs = 1024.0 * 1000.0 / 48000.0;
constexpr double kQualityMinRate = 32000.0;

// max.bedLift at its maximum means no budget (LoudnessMaximizer.h).
constexpr float kNoBedLiftBudgetDb = 24.0f;
constexpr double kBedLiftHoldSeconds = 2.0;
constexpr float kBedEventLu = 6.0f;
constexpr float kBedShortMs = 50.0f, kBedEventMs = 400.0f; // level fed to the background trackers; event loudness
constexpr double kBedStepMs = 10.0;                          // background tracker step
constexpr float kBedFloorLufs = -100.0f;

// The governor ticks where the maximizer's limiter-GR windows close (docs/11 E06).
static_assert (SafetyGovernor::kTickMs == LoudnessMaximizer::kGrWindowMs);

// The parameters the static-boost prediction reads (docs/11 E11); a change
// of any of them (or of the fold) re-runs it.
constexpr int kHeadroomScalarIds[] = { EqOn, EqOutputGainDb, DynEqOn, BassOn, BassBoostDb, BassBoostFreq, BassSubsonic, BassSubsonicOrder, ClarityOn,
                                       ClarityPresence, ClarityPresenceFreq, ClarityAir, SaturationOn, SatMix, SatOutputDb,
                                       AutoPreampOn, AutoPreampAllowanceDb, WarmthTone };
constexpr EqField kHeadroomEqFields[] = { EqFieldOn, EqFieldType, EqFieldFreq, EqFieldGain, EqFieldQ, EqFieldSlope };
constexpr DynField kHeadroomDynFields[] = { DynFieldOn, DynFieldShape, DynFieldFreq, DynFieldQ, DynFieldStaticGain };
constexpr int kHeadroomParamCount = static_cast<int> (std::size (kHeadroomScalarIds) + kEqBands * std::size (kHeadroomEqFields)
                                                      + kDynEqBands * std::size (kHeadroomDynFields));

// The loudness contour (docs/11 E32): the lift it may take out of the
// headroom (programme-weighted, see LoudnessContour.h) before it trims the
// level instead; at protection strength Normal / Strict the SafetyGovernor's
// scale shrinks it, so a limiter or distortion overload takes the lift out
// of the level rather than out of the limiter. And the maximizer's LF-first
// limiter (docs/11 E05 step 5) while the contour lifts the bass: armed once
// the lift at 50 Hz exceeds kContourLfArmDb (released under
// kContourLfDisarmDb, so a volume held near the threshold does not toggle
// the splitter), its amount the lift over kContourFullLfLimitDb, at least
// the glue's floor. Not at the reference level: the splitter's all-pass
// raises the crest factor of mastered programme (1-3 dB, see the glue
// floor below), which cost 0.4 dB more limiting and 0.1-0.6 LU on hot
// programme with the contour flat; with the bass lifted 12 dB (-30 dB) it
// halved the limiter's reduction on a kick programme (1.99 -> 1.09 dB).
constexpr float kContourAllowanceDb = 3.0f;
constexpr float kContourFullLfLimitDb = 12.0f, kContourLfArmDb = 4.0f, kContourLfDisarmDb = 2.0f;
constexpr float kLfLimitArmedFloor = 0.001f;
// The LFE fold's headroom (docs/11 E01): above +6 dB (the default before
// preset schema 3, and every level presets saved before it load) the LFE of
// a surround fold drives the maximizer's LF-first limiter, fully at +10 dB,
// so an explosion's LFE is limited in the low band instead of ducking the
// whole mix. Explosion-heavy 7.1 at +10 dB, BS.775 fold: time over 1 dB of
// limiting 63.5 % -> 2.8 %, deepest 6.3 -> 3.0 dB (at +6 dB without it: 7.3 %,
// 3.2 dB). At +6 dB and below nothing changes.
constexpr float kLfeLfLimitFromDb = 6.0f, kLfeLfLimitFullDb = 10.0f;
// Boost's transient coupling (docs/11 E05 step 6): in Music, Boost's top
// half adds attack to the Clarity shaper in proportion to how deep the
// maximizer's limiter reaches into transients - its deepest GR per 10 ms
// tick over its programme GR (a 1 s mean of the same readings; a steady
// tone's GR is its own programme, so steady material adds nothing) - so
// a kick's onset keeps its place over its body under heavy limiting. A
// positive-feedback loop (more attack, deeper transient GR): hard-capped at
// kAttackCoupleMaxDb, slew-limited both ways, and governed (the
// SafetyGovernor's scale multiplies it like Boost's drive). Tuned on the
// E59 quality suite, see docs/11 E05's Status.
constexpr float kAttackCoupleBoostStart = 0.5f, kAttackCoupleBoostEnd = 1.0f;
constexpr float kAttackCouplePerDb = 1.0f, kAttackCoupleMaxDb = 1.0f;
constexpr float kAttackCoupleSlewDbPerTick = 0.04f;   // 4 dB/s
constexpr float kProgramGrCoeff = 0.00995017f;        // 1 - exp (-10 ms / 1 s)
constexpr float kTransientGrReleasePerTick = 0.99f;   // ~1 s

// The clarity presence bell and air shelf, the bass shelf and subsonic
// filter as their modules design them (ClarityEnhancer.cpp, BassEngine.cpp).
constexpr double kPresenceQ = 0.8, kPresenceMaxDb = 6.0;
constexpr double kAirShelfHz = 10000.0, kAirShelfQ = 0.70710678118654752, kAirShelfMaxDb = 2.0;
constexpr double kBassShelfQ = 0.7, kBassMaxBoostDb = 15.0;

/** A named maximizer style (docs/11 E05 step 4) owns its six controls: their
    effective values become the style's (published like every other
    override, so the GUI's markers show what is applied). */
void applyMaxStyle (float* e) noexcept
{
    if (const MaxStyleValues* s = maxStyleValues (static_cast<MaxStyleValue> (idx (e, MaxStyle))))
    {
        e[MaxClipAmount] = s->clipAmount;
        e[MaxClipKnee] = s->clipKnee;
        e[MaxClipCrestDb] = s->clipCrestDb;
        e[MaxClipMaxDb] = s->clipMaxDb;
        e[MaxReleaseMs] = s->releaseMs;
        e[MaxAutoRelease] = s->autoRelease ? 1.0f : 0.0f;
    }
}

int gateFftSizeFor (double sampleRate) noexcept
{
    const double frame = kGateFrameMs * 0.001 * sampleRate;
    const int log2Size = static_cast<int> (std::lround (std::log2 (std::max (frame, 1.0))));
    return 1 << std::clamp (log2Size, 8, 12); // SpectralNoiseGate: 256 .. 4096
}

/** tame / tameThresholdDb: the Gaming anti-masking band's depth (0..1) and
    threshold, keyed to the Dynamic Range control (docs/11 E20 / E21,
    StartleGuard.h). tonalScale: the governor's tonal-balance scale (docs/11
    E07) on the macros' ungoverned lifts here, the Gaming Voice & Score band
    and the Music air band (1 at protection strength Off). */
void configureModeBands (DynamicEq& dyn, ModeValue mode, const float* e, double sampleRate, float tame, float tameThresholdDb,
                         float tonalScale) noexcept
{
    const auto& hz = mode == ModeValue::Gaming ? kGamingModeBandHz : kMusicModeBandHz;
    if (mode == ModeValue::Gaming)
    {
        const float footsteps = e[Macro1];
        const float voice = e[Macro5];
        const float detailRange = sampleRate > kSpeechLinkMaxRate ? 7.0f * footsteps : 0.0f;
        // Cue detail (steps, reloads, cloth) and footstep "body" (heel
        // impact): cue enhancer bands (threshold / ratio unused, see above).
        dyn.setBand (4, modeBand (DynEqMode::CueLift, EqBandType::Bell, hz[0], 0.9f, 0.0f, 1.0f, detailRange, kCueDetailAttackMs, kCueDetailReleaseMs, -75.0f));
        dyn.setBand (5, modeBand (DynEqMode::CueLift, EqBandType::Bell, hz[1], 1.2f, 0.0f, 1.0f, 3.0f * footsteps, kCueBodyAttackMs, kCueBodyReleaseMs, -75.0f));
        // Anti-masking (a CutAbove low shelf at 90 Hz) no longer follows
        // Footsteps (docs/11 E20): Footsteps 100 changed the explosion level.
        // It is the Tame stage of the Dynamic Range control (guard.range,
        // docs/11 E21): off (range 0) with the guard, as before E21; the
        // presets that tamed loud LF carry their old band as user band 0.
        dyn.setBand (6, modeBand (DynEqMode::CutAbove, EqBandType::LowShelf, hz[2], 0.7f, tameThresholdDb,
                                  tame > 0.0f ? StartleGuard::kTameRatio : 3.0f, tame * StartleGuard::kTameMaxRangeDb, 10.0f, 250.0f, -80.0f));
        // Voice comms / dialogue / score intelligibility.
        dyn.setBand (7, modeBand (DynEqMode::BoostBelow, EqBandType::Bell, hz[3], 0.7f, -36.0f, 2.0f, 4.0f * voice * tonalScale, 5.0f, 150.0f, -70.0f));
    }
    else
    {
        const float clarity = e[Macro3];
        const float boost = e[BoostIntensity];
        // Presence/air boosts are paired with a dynamic de-harsh band.
        dyn.setBand (4, modeBand (DynEqMode::CutAbove, EqBandType::Bell, hz[0], 1.2f, -22.0f, 3.0f, 3.0f * clarity, 2.0f, 80.0f, -80.0f));
        dyn.setBand (5, modeBand (DynEqMode::BoostBelow, EqBandType::HighShelf, hz[1], 0.7f, -45.0f, 2.0f, 3.0f * clarity * tonalScale, 10.0f, 200.0f, -80.0f));
        // Bass boost is paired with a dynamic de-boom band.
        dyn.setBand (6, modeBand (DynEqMode::CutAbove, EqBandType::Bell, hz[2], 1.0f, -14.0f, 2.5f, 4.0f * boost, 10.0f, 150.0f, -80.0f));
        dyn.setBand (7, modeBand (DynEqMode::CutAbove, EqBandType::Bell, hz[3], 1.0f, 0.0f, 1.0f, 0.0f, 5.0f, 80.0f, -80.0f));
    }
}
} // namespace

namespace
{
/** Bit of a module's enable parameter in ProcessingChain::auditionMask, -1 if none. */
int auditionBit (int enableParamId) noexcept
{
    static constexpr int ids[] = { GateOn, EqOn, DynEqOn, BassOn, ClarityOn, SaturationOn, SpatialOn, CompressorOn, MaximizerOn, VirtualizerOn };
    for (int b = 0; b < static_cast<int> (std::size (ids)); ++b)
        if (ids[b] == enableParamId)
            return b;
    return -1;
}
} // namespace

void ProcessingChain::setAuditionBypass (int enableParamId, bool bypassed) noexcept
{
    const int b = auditionBit (enableParamId);
    if (b < 0)
        return;
    const uint32_t bit = 1u << static_cast<uint32_t> (b);
    if (bypassed)
        auditionMask.fetch_or (bit, std::memory_order_relaxed);
    else
        auditionMask.fetch_and (~bit, std::memory_order_relaxed);
}

bool ProcessingChain::isAuditionBypassed (int enableParamId) const noexcept
{
    const int b = auditionBit (enableParamId);
    return b >= 0 && (auditionMask.load (std::memory_order_relaxed) & (1u << static_cast<uint32_t> (b))) != 0;
}

float ProcessingChain::modeBandFrequency (ModeValue mode, int band) noexcept
{
    const int i = band - kFirstModeBand;
    if (i < 0 || i >= kNumModeBands)
        return 0.0f;
    return (mode == ModeValue::Gaming ? kGamingModeBandHz : kMusicModeBandHz)[static_cast<size_t> (i)];
}

ProcessingChain::ProcessingChain (ParameterStore& s) : store (s)
{
    base.assign (static_cast<size_t> (kNumParams), 0.0f);
    effective.assign (static_cast<size_t> (kNumParams), 0.0f);
    governedBase.assign (static_cast<size_t> (kNumParams), 0.0f);
    baseAtPrepare.assign (static_cast<size_t> (kNumParams), 0.0f);
    headroomInput.assign (static_cast<size_t> (kNumParams), 0.0f);
    ungoverned.assign (static_cast<size_t> (kNumParams), 0.0f);
    quarterBase.assign (static_cast<size_t> (kNumParams), 0.0f);
    quarterScale.assign (static_cast<size_t> (kNumParams), 0.0f);
    publishedEffective = std::make_unique<std::atomic<float>[]> (static_cast<size_t> (kNumParams));
    for (auto& lift : tonalLiftDb)
        lift.store (TonalBalanceMeter::kNoReading, std::memory_order_relaxed);
    store.snapshot (base.data());
    MacroMap::apply (base.data(), effective.data(), 1.0f);
    publishEffective();
}

const char* neuralSlotReason (NeuralSlotState state) noexcept
{
    switch (state)
    {
        case NeuralSlotState::Empty: return "No neural model installed.";
        case NeuralSlotState::Active: return "Neural model active.";
        case NeuralSlotState::Ineligible: return "Bypassed: the model's latency exceeds the latency profile's budget.";
        case NeuralSlotState::SampleRateMismatch: return "Bypassed: the model was trained for a different sample rate.";
        case NeuralSlotState::InvalidModel: return "Bypassed: the model's description is invalid.";
        case NeuralSlotState::PrepareFailed: return "Bypassed: the model failed to start.";
        case NeuralSlotState::BlockTooLarge: return "Bypassed: the audio buffer is longer than the model's safety margin.";
    }
    return "";
}

void ProcessingChain::setNeuralModel (std::unique_ptr<ModelRunner> runner, const NeuralSlotConfig& neuralConfig)
{
    // Built now (describe() only: no allocation of queues, no thread), swapped
    // in by the next prepare(), so the audio thread never sees it change.
    // Offline rendering runs the model inside process() (no deadline to miss).
    AsyncModelConfig processorConfig = neuralConfig.processor;
    processorConfig.offline = neuralConfig.context == ModelContext::Offline;
    pendingNeural = runner != nullptr ? std::make_unique<AsyncModelProcessor> (std::move (runner), processorConfig) : nullptr;
    pendingContext = neuralConfig.context;
    neuralChangePending.store (true, std::memory_order_release);
}

void ProcessingChain::clearNeuralModel()
{
    setNeuralModel (nullptr);
}

NeuralSlotStatus ProcessingChain::getNeuralStatus() const noexcept FLUB_NONBLOCKING
{
    NeuralSlotStatus st;
    st.state = static_cast<NeuralSlotState> (neuralState.load (std::memory_order_acquire));
    st.modelLatencySamples = neuralModelLatency.load (std::memory_order_relaxed);
    st.changePending = neuralChangePending.load (std::memory_order_acquire);
    return st;
}

NeuralSlotCounters ProcessingChain::getNeuralCounters() const noexcept FLUB_NONBLOCKING
{
    NeuralSlotCounters c;
    c.deadlineMisses = neuralMisses.load (std::memory_order_relaxed);
    c.modelFailures = neuralFailures.load (std::memory_order_relaxed);
    c.framesProcessed = neuralFrames.load (std::memory_order_relaxed);
    return c;
}

void ProcessingChain::prepareNeuralSlot (const ProcessSpec& stereo)
{
    if (neuralChangePending.exchange (false, std::memory_order_acq_rel))
    {
        neural = std::move (pendingNeural); // the old processor (if any) joins its worker here
        neuralContext = pendingContext;
    }

    neuralInChain = false;
    neuralMisses.store (0, std::memory_order_relaxed);
    neuralFailures.store (0, std::memory_order_relaxed);
    neuralFrames.store (0, std::memory_order_relaxed);

    auto state = NeuralSlotState::Empty;
    int modelLatency = 0;
    if (neural != nullptr)
    {
        const ModelDescription& d = neural->getDescription();
        modelLatency = neural->latencySamples();
        if (modelLatency <= 0)
            state = NeuralSlotState::InvalidModel; // AsyncModelProcessor is inert (0 latency) exactly then
        else if (d.sampleRate != 0.0 && d.sampleRate != stereo.sampleRate)
            state = NeuralSlotState::SampleRateMismatch; // it would only delay: keep it out
        else if (! isEligible (static_cast<LatencyProfileValue> (profileAtPrepare), modelLatency, stereo.sampleRate, neuralContext))
            state = NeuralSlotState::Ineligible;
        else if (stereo.maxBlockSize > neural->getMaxBlockSizeWithoutMisses())
            state = NeuralSlotState::BlockTooLarge; // a result can only arrive in a later block than its frame's
        else
        {
            try
            {
                slots[SNeural].prepare (*neural, stereo, 20.0f, ! neuralBypass.load (std::memory_order_relaxed));
                neuralInChain = true;
                state = NeuralSlotState::Active;
            }
            catch (...) // the runner's prepare() or std::thread: run without the model
            {
                state = NeuralSlotState::PrepareFailed;
            }
        }
        if (! neuralInChain)
            neural->releaseResources(); // an out-of-chain model keeps no worker running
    }
    neuralModelLatency.store (modelLatency, std::memory_order_relaxed);
    neuralState.store (static_cast<int> (state), std::memory_order_release);
}

void ProcessingChain::publishEffective() noexcept
{
    for (int i = 0; i < kNumParams; ++i)
        publishedEffective[static_cast<size_t> (i)].store (effective[static_cast<size_t> (i)], std::memory_order_relaxed);
}

void ProcessingChain::prepare (const ChainConfig& cfg)
{
    config = cfg;
    config.inputChannels = std::clamp (config.inputChannels, 2, kMaxChannels);
    const double sr = config.sampleRate;
    const int maxB = config.maxBlockSize;

    store.snapshot (base.data());
    baseAtPrepare = base;
    onboardCap = onboardCapRequest.load (std::memory_order_relaxed) ? 1.0f : 0.0f; // docs/11 E16: nothing heard yet, no glide
    onboardCapSnap = true;
    MacroMap::apply (base.data(), effective.data(), 1.0f, onboardCap);
    publishEffective();
    const float* e = effective.data();

    requestedProfileAtPrepare = idx (e, LatencyProfile);
    profileAtPrepare = requestedProfileAtPrepare;
    if (static_cast<LatencyProfileValue> (profileAtPrepare) == LatencyProfileValue::Quality && sr < kQualityMinRate)
        profileAtPrepare = static_cast<int> (LatencyProfileValue::Balanced);
    switch (static_cast<LatencyProfileValue> (profileAtPrepare))
    {
        case LatencyProfileValue::Quality:
            gateInChain = true;
            gate.setFftSize (gateFftSizeFor (sr));
            saturator.setOversampling (Oversampler::forProfile (Oversampler::Profile::Quality, sr));
            compressor.setLookaheadMs (3.0f);
            maximizer.setClipOversampling (Oversampler::forClipper (Oversampler::Profile::Quality, sr));
            maximizer.setLookaheadMs (2.0f);
            maximizer.setTruePeakDetection (true);
            break;
        case LatencyProfileValue::LowLatency:
            gateInChain = false;
            saturator.setOversampling (Oversampler::forProfile (Oversampler::Profile::LowLatency, sr));
            compressor.setLookaheadMs (0.5f);
            maximizer.setClipOversampling (Oversampler::forClipper (Oversampler::Profile::LowLatency, sr));
            maximizer.setLookaheadMs (0.5f);
            maximizer.setTruePeakDetection (true);
            break;
        case LatencyProfileValue::Balanced:
        default:
            gateInChain = false;
            saturator.setOversampling (Oversampler::forProfile (Oversampler::Profile::Balanced, sr));
            compressor.setLookaheadMs (1.0f);
            maximizer.setClipOversampling (Oversampler::forClipper (Oversampler::Profile::Balanced, sr));
            maximizer.setLookaheadMs (1.5f);
            maximizer.setTruePeakDetection (true);
            break;
    }

    virtualizer.prepare ({ sr, maxB, config.inputChannels });
    fold.prepare (sr, LfeFold::gainFor (on (e, VirtLfeFold), e[VirtLfeGainDb]));
    foldHeadroom.prepare (sr);
    inputDetector.prepare (sr, config.inputChannels);

    const ProcessSpec stereo { sr, maxB, 2 };
    if (gateInChain)
        slots[SGate].prepare (gate, stereo, 20.0f, on (e, GateOn));
    prepareNeuralSlot (stereo);
    slots[SEq].prepare (paramEq, stereo, 20.0f, on (e, EqOn));
    slots[SDynEq].prepare (dynEq, stereo, 20.0f, on (e, DynEqOn));
    slots[SBass].prepare (bass, stereo, 20.0f, on (e, BassOn));
    slots[SClarity].prepare (clarity, stereo, 20.0f, on (e, ClarityOn));
    slots[SSat].prepare (saturator, stereo, 20.0f, on (e, SaturationOn));
    // The Smoothness stage (docs/11 E07) measures against the dynamic EQ's
    // input, delayed by the slots in between.
    smoothness.setReferenceDelay (slots[SDynEq].latencySamples() + slots[SBass].latencySamples() + slots[SClarity].latencySamples()
                                  + slots[SSat].latencySamples());
    slots[SSpatial].prepare (spatial, stereo, 20.0f, on (e, SpatialOn));
    slots[SComp].prepare (compressor, stereo, 20.0f, on (e, CompressorOn));
    slots[SMax].prepare (maximizer, stereo, 20.0f, on (e, MaximizerOn));
    // ... and follows the chain's output, delayed by the slots after it
    // (prepared first for their latencies).
    smoothness.setDownstreamDelay (slots[SSpatial].latencySamples() + slots[SComp].latencySamples() + slots[SMax].latencySamples());
    slots[SSmooth].prepare (smoothness, stereo, 20.0f, e[SmoothAmount] > 0.0f);
    smoothReference.setSize (2, maxB);
    contour.prepare (stereo); // docs/11 E32: after the preamp, ahead of the slots; no latency
    warmthTilt.prepare (stereo); // docs/11 E14: ahead of the parametric EQ slot; no latency
    // docs/11 E33: the per-ear stage, re-designed at this rate; latency only
    // at the measured AfterMaximizer placement (its per-ear limiters).
    personal.prepare (stereo, personalPlacement);
    personalView.setSize (2, maxB);

    totalLatency = personal.latencySamples();
    for (int s = 0; s < kNumSlots; ++s)
        if (inChain (s))
            totalLatency += slots[static_cast<size_t> (s)].latencySamples();

    dryBuffer.setSize (2, maxB);
    // The bypass reference's safety limiter lives inside the chain latency:
    // up to 1 ms look-ahead plus the 4x detector delay when the latency
    // allows (every profile at 22.05 kHz and above), sample-peak otherwise.
    {
        const int detector = TruePeakDetector::kDelay;
        const bool truePeak = totalLatency >= detector + 8;
        const int lookahead = truePeak ? std::min (totalLatency - detector, std::max (1, static_cast<int> (std::lround (0.001 * sr))))
                                       : totalLatency;
        dryLimiter.setTruePeakDetection (truePeak);
        dryLimiter.setLookaheadMs (static_cast<float> (1000.0 * lookahead / sr));
        // The maximizer's LF-safe envelope (docs/11 E05 / E10): a hot
        // reference is limited without rippling its bass, so the bypass
        // side carries no limiter THD or DC either. Unity while idle.
        dryLimiter.setEnvelope ({ true, true, true });
        dryLimiter.prepare ({ sr, maxB, 2 });
    }
    dryDelay.prepare (2, std::max (0, totalLatency - dryLimiter.latencySamples()));
    foldScratch.setSize (config.inputChannels, maxB);
    virtMix.reset (sr, 20.0f, on (e, VirtualizerOn) && ! on (e, VirtOwnHrtf) ? 1.0f : 0.0f);
    passMix.reset (sr, kPassFadeMs, config.inputChannels > 2 && stereoFoldFor (e, ActiveChannelDetector::Fold::Surround) ? 1.0f : 0.0f);

    inputGain.reset (sr, 20.0f, dbToGain (e[InputGainDb]));
    // The first block predicts at once and starts the preamp at its value
    // (no glide from unity: nothing has been heard yet).
    preampGain.reset (sr, 20.0f, 1.0f);
    headroomKeyValid = false;
    headroomHoldoff = 0;
    // The hot-programme peak (auto.preampHot) is held again from the first
    // block; a reset() keeps it, like the governor's learned state.
    hotPeakDb = kMinusInfDb;
    hotPreampDb = 0.0f;
    hotHoldLeft = 0;
    warmthTrimModelDb = 0.0f;
    outputGain.reset (sr, 20.0f, dbToGain (e[OutputGainDb]));
    bypassMix.reset (sr, 30.0f, on (e, BypassAll) ? 1.0f : 0.0f);
    dryMatchGain.reset (sr, 50.0f, 1.0f);

    autoLevel.prepare (sr, config.inputChannels);
    // The guard measures ahead of the compressor slot and applies its gains
    // to what leaves it: the slot's latency is its look-ahead.
    startleGuard.prepare (sr, maxB, slots[SComp].latencySamples());
    autoDrive.prepare (sr, 2);
    governor.prepare (sr);
    distortion.prepare (sr);
    {
        // The governor's spans (docs/11 E06 Phase 3): the bass engine alone,
        // and the saturator to the maximizer, each input delayed by the
        // latency of its slots.
        int driveLatency = 0;
        for (int s = SSat; s <= SMax; ++s)
            driveLatency += slots[static_cast<size_t> (s)].latencySamples();
        bassSpanDelay.prepare (1, slots[SBass].latencySamples());
        driveSpanDelay.prepare (1, driveLatency);
        bassSpan.prepare (sr);
        driveSpan.prepare (sr);
        bassSpanInput.assign (static_cast<size_t> (maxB), 0.0f);
        driveSpanInput.assign (static_cast<size_t> (maxB), 0.0f);
        spanOutput.assign (static_cast<size_t> (maxB), 0.0f);
        plrMeter.prepare (sr, 2);
        inputPlrMeter.prepare (sr, 2);
        tonalMeter.prepare (sr); // the tonal-balance rule (docs/11 E07)
    }
    loudnessMatch.prepare (sr, 2);

    inLevel.prepare (sr, 2);
    outLevel.prepare (sr, 2);
    outTruePeak.prepare (2);
    outLoudness.prepare (sr, 2);
    inLoudness.prepare (sr, 2, 3000.0f);
    bedInShort.prepare (sr, 2, kBedShortMs);
    preMaxShort.prepare (sr, 2, kBedShortMs);
    bedEventLoudness.prepare (sr, 2, kBedEventMs);
    bedStepLength = std::max (1, static_cast<int> (std::lround (kBedStepMs * 0.001 * sr)));
    bedInBackground.prepare (1000.0 / kBedStepMs);
    preMaxBackground.prepare (1000.0 / kBedStepMs);
    tapScratch.assign (static_cast<size_t> (maxB), 0.0f);

    meterBus.latencyMs.store (static_cast<float> (1000.0 * totalLatency / sr), std::memory_order_relaxed);
    corruptSamples = droppedBlocks = 0;
    meterBus.corruptSampleCount.store (0, std::memory_order_relaxed);
    meterBus.droppedBlockCount.store (0, std::memory_order_relaxed);
    reset();
}

void ProcessingChain::reset() noexcept
{
    // Boost's transient coupling starts again from 0, and the Clarity
    // shaper's attack from the value without it (its smoother restarts on
    // its current target), so a reset chain does not depend on its history.
    if (attackCoupleDb != 0.0f)
    {
        ClarityParams cp = clarity.getParams();
        cp.attackDb = attackBeforeCoupleDb;
        clarity.setParams (cp);
    }
    programGrDb = transientGrDb = attackCoupleDb = 0.0f;
    resetSignalState();
    inputDetector.reset();
    autoLevel.reset();
    startleGuard.reset();
    contourLfArmed = false;
    autoDrive.reset();
    governor.restart(); // at Normal / Strict the governor keeps what it has learned (docs/11 E06 (2))
    loudnessMatch.reset();
}

void ProcessingChain::adoptGovernorState (const ProcessingChain& previous) noexcept
{
    // The strength first: the governor applies the learned state only at the
    // strength it was learned at, and a host re-applies its setting to a new
    // engine a little later (the app's onEngineConfigured is asynchronous).
    setProtectionStrength (previous.getProtectionStrength());
    governor.setStrength (previous.getProtectionStrength()); // before its first block: not yet on the audio thread
    setOnboardEnhancementCap (previous.getOnboardEnhancementCap()); // docs/11 E16, taken at once on the first block
    setSafeSpeakerBassCapDb (previous.getSafeSpeakerBassCapDb());   // docs/11 E51
    personal.setProfileNow (previous.getPersonalProfile());         // docs/11 E33, at this chain's rate
    SafetyGovernor::Memory m;
    if (previous.governorMemory.read (m) && m.valid)
        governor.restoreMemory (m);
}

void ProcessingChain::GovernorMemoryBox::publish (const SafetyGovernor::Memory& m) noexcept FLUB_NONBLOCKING
{
    // A seqlock of relaxed words: odd while writing (audio thread only).
    std::array<uint32_t, kWords> w {};
    std::memcpy (w.data(), &m, sizeof (m));
    const uint32_t s = sequence.load (std::memory_order_relaxed);
    sequence.store (s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence (std::memory_order_release);
    for (size_t i = 0; i < kWords; ++i)
        words[i].store (w[i], std::memory_order_relaxed);
    sequence.store (s + 2, std::memory_order_release);
}

bool ProcessingChain::GovernorMemoryBox::read (SafetyGovernor::Memory& m) const noexcept
{
    for (int attempt = 0; attempt < 64; ++attempt)
    {
        const uint32_t s0 = sequence.load (std::memory_order_acquire);
        if ((s0 & 1u) != 0u)
            continue;
        std::array<uint32_t, kWords> w {};
        for (size_t i = 0; i < kWords; ++i)
            w[i] = words[i].load (std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_acquire);
        if (sequence.load (std::memory_order_relaxed) == s0)
        {
            if (s0 == 0u)
                return false; // never published
            std::memcpy (static_cast<void*> (&m), w.data(), sizeof (m)); // trivially copyable (asserted)
            return true;
        }
    }
    return false;
}

void ProcessingChain::resetSignalState() noexcept
{
    for (int s = 0; s < kNumSlots; ++s)
        if (inChain (s))
            slots[static_cast<size_t> (s)].reset();
    virtualizer.reset();
    fold.reset();
    contour.reset();
    warmthTilt.reset();
    personal.reset();
    virtMix.setImmediate (virtMix.getTarget());
    passMix.setImmediate (passMix.getTarget());
    virtRan = foldRan = passStatsValid = false;
    foldHeadroom.reset();
    foldHeadroomRan = false;
    headroomBlockMinDb = 0.0f;
    dryDelay.reset();
    dryLimiter.reset();
    dryLimiterRunning = false;
    dryWarmup = 0;
    distortion.reset();
    governor.restartTickGrid(); // the maximizer's window grid restarts with its slot
    spanRunning = false;        // the governor's span measurements restart with the signal path
    inLevel.reset();
    outLevel.reset();
    outTruePeak.reset();
    outLoudness.reset();
    inLoudness.reset();
    bedInShort.reset();
    preMaxShort.reset();
    bedEventLoudness.reset();
    bedInBackground.reset();
    preMaxBackground.reset();
    bedStepCount = bedLiftHold = 0;
}

bool ProcessingChain::needsReprepare() const noexcept
{
    // Every structural parameter (today: the latency profile) changes the
    // module configuration or latency, so any of them needs a re-prepare; so
    // does installing or removing a neural model.
    if (neuralChangePending.load (std::memory_order_acquire))
        return true;
    const auto& info = layout();
    for (int i = 0; i < kNumParams; ++i)
        if (info[static_cast<size_t> (i)].structural && store.get (i) != baseAtPrepare[static_cast<size_t> (i)])
            return true;
    return false;
}

void ProcessingChain::applyParameters() noexcept
{
    // ---- Governor (docs/11 E06): the scale multiplies every governed macro
    // amount and, at protection strength Normal / Strict, the base drives
    // too (the store is never written) ----
    const auto strength = static_cast<ProtectionStrength> (protectionStrength.load (std::memory_order_relaxed));
    governor.setStrength (strength);
    appliedStrength = strength;
    const float governorScale = governor.getScale();
    const float* governed = base.data();
    if (strength != ProtectionStrength::Off && governorScale < 1.0f)
    {
        std::copy (base.begin(), base.end(), governedBase.begin());
        for (int id : { MaxDriveDb, SatDriveDb, BassHarmonics }) // all >= 0: scaled towards their minimum, 0
            governedBase[static_cast<size_t> (id)] *= governorScale;
        governed = governedBase.data();
    }
    MacroMap::apply (governed, effective.data(), governorScale, onboardCap);
    float* e = effective.data();
    // The measured loop (Normal / Strict, docs/11 E06 Phase 3): its own
    // scale on the bass harmonics, the budgets of the mode, Small Speaker
    // Mode, and the maximizer drive at the full scale for the feed-forward.
    if (strength != ProtectionStrength::Off)
    {
        e[BassHarmonics] *= governor.getHarmonicsScale();
        governor.setMusicMode (static_cast<ModeValue> (idx (e, Mode)) == ModeValue::Music);
        governor.setHarmonicsReplaceFundamental (on (e, BassReplaceFundamental));
        // The maximizer drive per unit of scale (docs/11 E06 batch 2): the
        // base drive and every governed contribution scale with it, but
        // their sum is clamped at max.drive's maximum, so the drive at the
        // full scale understated it (Gaming Boost 100 on a 24 dB base: 30 dB,
        // clamped to 24; the feed-forward's share read 3 dB high). At a
        // quarter of the scale the sum is under the clamp.
        std::copy (base.begin(), base.end(), quarterBase.begin());
        for (int id : { MaxDriveDb, SatDriveDb, BassHarmonics })
            quarterBase[static_cast<size_t> (id)] *= 0.25f;
        MacroMap::apply (quarterBase.data(), quarterScale.data(), 0.25f, onboardCap);
        driveAtFullScale = 4.0f * quarterScale[static_cast<size_t> (MaxDriveDb)];
    }
    if (strength == ProtectionStrength::Off)
    {
        spanRunning = false;
    }
    else if (! spanRunning)
    {
        bassSpanDelay.reset();
        driveSpanDelay.reset();
        bassSpan.reset();
        driveSpan.reset();
        feedForward.reset();
        plrMeter.reset();
        inputPlrMeter.reset();
        preMaxPeak = 0.0f;
        preMaxEnergy = {};
        preMaxSamples = 0;
        bassShareSmoothedPow = 0.0;
        lastHarmonicsResidualDb = kMinusInfDb;
        tonalMeter.reset();
        spanRunning = true;
    }
    // The tonal-balance rule (docs/11 E07): its scale on what the macros add
    // to presence and air (never on base values: the preset's or the user's
    // own presence stays). The voice and air mode bands follow below.
    const float tonalScale = strength != ProtectionStrength::Off ? governor.getTonalScale() : 1.0f;
    if (tonalScale < 1.0f)
        for (int id : { ClarityPresence, ClarityAir })
        {
            const float b = base[static_cast<size_t> (id)];
            e[id] = b + std::max (0.0f, e[id] - b) * tonalScale;
        }
    // The safe speaker profile's bass cap (docs/11 E51), on what the macros
    // and the governor left.
    applySafeSpeakerBassCap (e);
    // Mode / format policies below write their overrides into e, so the
    // values published at the end (effectiveValue(), GUI ghost markers) are
    // the ones actually applied.
    // A module is active when it is (effectively) on and not held off by the
    // GUI's audition bypass.
    const uint32_t audition = auditionMask.load (std::memory_order_relaxed);
    const auto active = [e, audition] (int enableId) {
        const int b = auditionBit (enableId);
        return on (e, enableId) && (b < 0 || (audition & (1u << static_cast<uint32_t> (b))) == 0);
    };
    const auto mode = static_cast<ModeValue> (idx (e, Mode));
    // A game that renders its own HRTF (docs/11 E27): no second head model on
    // top of it (and the stereo fold below, unless surround is forced).
    const bool ownHrtf = on (e, VirtOwnHrtf);
    if (ownHrtf)
        e[VirtualizerOn] = 0.0f;
    // 5.1 / 7.1 input: stereo passthrough or surround fold, and the binaural
    // lock only while the virtualiser actually renders the surround fold.
    const bool stereoFold = config.inputChannels > 2 && stereoFoldFor (e, inputDetector.getFold());
    const bool binaural = config.inputChannels > 2 && ! stereoFold && active (VirtualizerOn);

    inputGain.setTarget (dbToGain (e[InputGainDb]));
    outputGain.setTarget (dbToGain (e[OutputGainDb]));
    autoLevel.setEnabled (on (e, AutoLevelOn));
    autoLevel.setTargetLufs (e[AutoLevelTargetLufs]);
    bypassMix.setTarget (on (e, BypassAll) ? 1.0f : 0.0f);
    dryLimiter.setParams ({ e[MaxCeilingDb], 80.0f, true });
    virtMix.setTarget (active (VirtualizerOn) ? 1.0f : 0.0f);
    passMix.setTarget (stereoFold ? 1.0f : 0.0f);
    fold.setLfeGain (LfeFold::gainFor (on (e, VirtLfeFold), e[VirtLfeGainDb]));

    // ---- Spectral gate ----
    if (gateInChain)
    {
        NoiseGateParams gp;
        gp.thresholdDb = e[GateThresholdDb];
        gp.reductionDb = e[GateReductionDb];
        gp.attackMs = e[GateAttackMs];
        gp.releaseMs = e[GateReleaseMs];
        gp.floorRiseDbPerSec = e[GateFloorRise];
        gp.freezeFloor = on (e, GateFreeze);
        gate.setParams (gp);
        slots[SGate].setActive (active (GateOn));
    }

    // ---- Neural slot ----
    if (neuralInChain)
        slots[SNeural].setActive (! neuralBypass.load (std::memory_order_relaxed));

    // ---- Parametric EQ ----
    for (int b = 0; b < kEqBands; ++b)
    {
        EqBandParams bp;
        bp.enabled = on (e, eq (b, EqFieldOn));
        bp.type = static_cast<EqBandType> (idx (e, eq (b, EqFieldType)));
        bp.frequency = e[eq (b, EqFieldFreq)];
        bp.gainDb = e[eq (b, EqFieldGain)];
        bp.q = e[eq (b, EqFieldQ)];
        bp.slopeDbPerOct = 12 * (idx (e, eq (b, EqFieldSlope)) + 1);
        paramEq.setBand (b, bp);
    }
    paramEq.setOutputGainDb (e[EqOutputGainDb]);
    slots[SEq].setActive (active (EqOn));

    // ---- Dynamic EQ: user bands 0..3, mode bands 4..7 ----
    static constexpr EqBandType shapes[] = { EqBandType::Bell, EqBandType::LowShelf, EqBandType::HighShelf };
    for (int b = 0; b < kDynEqBands; ++b)
    {
        DynEqBandParams dp;
        dp.enabled = on (e, dyn (b, DynFieldOn));
        dp.mode = static_cast<DynEqMode> (idx (e, dyn (b, DynFieldMode)));
        dp.shape = shapes[std::clamp (idx (e, dyn (b, DynFieldShape)), 0, 2)];
        dp.frequency = e[dyn (b, DynFieldFreq)];
        dp.q = e[dyn (b, DynFieldQ)];
        dp.thresholdDb = e[dyn (b, DynFieldThreshold)];
        dp.ratio = e[dyn (b, DynFieldRatio)];
        dp.rangeDb = e[dyn (b, DynFieldRange)];
        dp.staticGainDb = e[dyn (b, DynFieldStaticGain)];
        dp.attackMs = e[dyn (b, DynFieldAttack)];
        dp.releaseMs = e[dyn (b, DynFieldRelease)];
        dp.noiseFloorDb = e[dyn (b, DynFieldNoiseFloor)];
        dynEq.setBand (b, dp);
    }
    // ---- Startle Guard and Tame (docs/11 E21 / E20; StartleGuard.h) ----
    const int guardRange = idx (e, GuardRange);
    startleGuard.setCeilingLu (StartleGuard::ceilingLuFor (guardRange));
    const float tame = StartleGuard::tameAmountFor (guardRange);
    float tameThresholdDb = StartleGuard::kTameMaxThresholdDb;
    if (tame > 0.0f && startleGuard.getReferenceLufs() > -60.0f)
        tameThresholdDb = std::min (tameThresholdDb, startleGuard.getReferenceLufs() + StartleGuard::kTameOverReferenceDb);
    configureModeBands (dynEq, mode, e, config.sampleRate, tame, tameThresholdDb, tonalScale);
    slots[SDynEq].setActive (active (DynEqOn));

    // ---- Bass ----
    BassEngineParams bp;
    bp.boostDb = e[BassBoostDb];
    bp.boostFrequency = e[BassBoostFreq];
    bp.protectThresholdDb = e[BassProtectDb];
    bp.harmonicsAmount = e[BassHarmonics];
    bp.harmonicsCutoff = e[BassHarmonicsCutoff];
    bp.harmonicsCharacter = e[BassHarmonicsCharacter];
    bp.replaceFundamental = on (e, BassReplaceFundamental);
    bp.tighten = e[BassTighten];
    bp.monoBelowHz = e[BassMonoBelow];
    bp.subsonicHz = e[BassSubsonic];
    bp.subsonicOrder = idx (e, BassSubsonicOrder) == static_cast<int> (SubsonicOrderValue::Slope12) ? 2 : 4;
    bp.splitProtection = on (e, BassSplitProtect);
    bass.setParams (bp);
    slots[SBass].setActive (active (BassOn));

    // ---- Boost's transient coupling (docs/11 E05 step 6; see the constants) ----
    attackBeforeCoupleDb = e[ClarityAttackDb];
    if (mode == ModeValue::Music && attackCoupleDb > 0.0f)
        e[ClarityAttackDb] = layout()[static_cast<size_t> (ClarityAttackDb)].clamp (e[ClarityAttackDb] + attackCoupleDb);

    // ---- Clarity ----
    ClarityParams cp;
    cp.attackDb = e[ClarityAttackDb];
    cp.sustainDb = e[ClaritySustainDb];
    cp.presence = e[ClarityPresence];
    cp.presenceFrequency = e[ClarityPresenceFreq];
    // docs/11 E07 step 3: the band against a fixed level or against the
    // programme's body (the module crossfades the two laws).
    cp.presenceMode = idx (e, ClarityPresenceMode) == static_cast<int> (PresenceModeValue::Relative) ? PresenceMode::Relative
                                                                                                    : PresenceMode::Absolute;
    // The air exciter is alias-free only because its <= 3rd-order products of
    // <= 7 kHz content stay below 21 kHz. Headsets running at low rates (USB
    // 32 kHz modes, Bluetooth hands-free at 16 / 8 kHz) would fold them back,
    // so it is disabled below 42 kHz (3 x 7 kHz = 21 kHz < fs / 2).
    if (config.sampleRate < 42000.0)
        e[ClarityAir] = 0.0f;
    cp.air = e[ClarityAir];
    cp.deMud = e[ClarityDeMud];
    clarity.setParams (cp);
    slots[SClarity].setActive (active (ClarityOn));

    // ---- Warmth tilt (docs/11 E14; ToneTilt.h) ----
    warmthTilt.setParams ({ e[WarmthTone] });

    // ---- Saturation ----
    SaturatorParams sp;
    sp.type = static_cast<SaturationType> (std::clamp (idx (e, SatType), 0, 2));
    sp.driveDb = e[SatDriveDb];
    sp.mix = e[SatMix];
    sp.outputDb = e[SatOutputDb];
    saturator.setParams (sp);
    slots[SSat].setActive (active (SaturationOn));

    // ---- Smoothness (docs/11 E07; SmoothnessGuard.h): Gaming gets the light guard ----
    smoothness.setParams ({ e[SmoothAmount], mode == ModeValue::Gaming });
    slots[SSmooth].setActive (e[SmoothAmount] > 0.0f);

    // ---- Stereo & space (mode / binaural policy) ----
    if (mode == ModeValue::Gaming)
        e[SpatialCrossfeed] = 0.0f; // crossfeed blurs lateral cues: never in gaming
    if (binaural || ownHrtf)
    {
        // Binaural output - the virtualiser's, or a game's own HRTF render -
        // already carries exact interaural cues; widening, decorrelation,
        // crossfeed or the focus ILD bell on top of them would corrupt them
        // (docs/11 E24 (i), E27).
        e[SpatialWidth] = 1.0f;
        e[SpatialSpace] = 0.0f;
        e[SpatialCrossfeed] = 0.0f;
        e[SpatialFocus] = 0.0f;
    }
    SpatializerParams wp;
    wp.width = e[SpatialWidth];
    wp.widthLowCutHz = e[SpatialWidthLowCut];
    wp.positionalFocus = e[SpatialFocus];
    wp.space = e[SpatialSpace];
    wp.crossfeed = e[SpatialCrossfeed];
    // CrossfeedTypeValue is in CrossfeedType's order; a type change fades
    // the crossfeed in the spatializer (the old model out, the new one in).
    wp.crossfeedType = static_cast<CrossfeedType> (std::clamp (idx (e, SpatialCrossfeedType), 0, static_cast<int> (CrossfeedTypeValue::MonoSafe)));
    wp.autoMonoSafety = on (e, SpatialMonoSafety);
    wp.minCorrelation = e[SpatialMinCorrelation];
    spatial.setParams (wp);
    slots[SSpatial].setActive (active (SpatialOn));

    // ---- Virtualiser ----
    VirtualizerParams vp;
    vp.layout = layoutFor (config.inputChannels);
    vp.frontAngleDeg = e[VirtFrontAngle];
    vp.sideAngleDeg = e[VirtSideAngle];
    vp.rearAngleDeg = e[VirtRearAngle];
    vp.headRadiusMm = e[VirtHeadRadius];
    vp.roomAmount = e[VirtRoom];
    vp.lfeGainDb = e[VirtLfeGainDb];
    vp.lfeOn = on (e, VirtLfeFold);
    virtualizer.setParams (vp);

    // ---- Compressor ----
    // The Gaming Detail macro switches the compressor on for its UPWARD
    // section: quiet detail comes up. When only a macro engaged it
    // and nobody chose a downward ratio (comp.ratio still at its default), the
    // downward section stays off, so gunshots and explosions keep their
    // dynamics. Presets that set a ratio (e.g. 1.5:1 glue) keep it.
    if (mode == ModeValue::Gaming && ! (base[CompressorOn] >= 0.5f)
        && base[CompRatio] == layout()[static_cast<size_t> (CompRatio)].defaultValue)
        e[CompRatio] = 1.0f;
    CompressorParams kp;
    kp.thresholdDb = e[CompThresholdDb];
    kp.ratio = e[CompRatio];
    kp.kneeDb = e[CompKneeDb];
    kp.attackMs = e[CompAttackMs];
    kp.releaseMs = e[CompReleaseMs];
    kp.autoRelease = on (e, CompAutoRelease);
    kp.makeupDb = e[CompMakeupDb];
    kp.autoMakeup = on (e, CompAutoMakeup);
    kp.sidechainHpHz = e[CompSidechainHp];
    kp.mix = e[CompMix];
    kp.upThresholdDb = e[CompUpThresholdDb];
    kp.upRatio = e[CompUpRatio];
    kp.upMaxGainDb = e[CompUpMaxGainDb];
    kp.upFloorDb = e[CompUpFloorDb];
    kp.upRelativeFloor = mode == ModeValue::Gaming; // docs/11 E19: Detail does not lift the bed
    compressor.setParams (kp);
    slots[SComp].setActive (active (CompressorOn));

    // ---- The LFE fold's headroom (docs/11 E01): the maximizer's LF-first
    // limiter above +6 dB of LFE on a surround fold ----
    if (config.inputChannels > 2 && ! stereoFold && on (e, VirtLfeFold) && e[VirtLfeGainDb] > kLfeLfLimitFromDb)
        e[MaxLfLimit] = std::max ({ e[MaxLfLimit], kLfLimitArmedFloor,
                                    std::min (1.0f, (e[VirtLfeGainDb] - kLfeLfLimitFromDb) / (kLfeLfLimitFullDb - kLfeLfLimitFromDb)) });

    // ---- Loudness contour (docs/11 E32; LoudnessContour.h): the level is
    // contour.level plus the host's offset (the OS output volume re the
    // user's reference volume) ----
    LoudnessContourParams lc;
    lc.enabled = on (e, ContourOn);
    lc.referencePhon = e[ContourReferencePhon];
    lc.levelDb = e[ContourLevelDb] + listeningLevelDb.load (std::memory_order_relaxed);
    lc.maxLiftDb = e[ContourMaxLiftDb];
    lc.allowanceDb = kContourAllowanceDb * (strength != ProtectionStrength::Off ? governorScale : 1.0f);
    contour.setParams (lc);
    const float contourLift = contour.getAppliedLiftAt50HzDb();
    contourLfArmed = lc.enabled && contourLift > (contourLfArmed ? kContourLfDisarmDb : kContourLfArmDb);
    if (contourLfArmed)
        e[MaxLfLimit] = std::max ({ e[MaxLfLimit], kLfLimitArmedFloor, std::min (1.0f, contourLift / kContourFullLfLimitDb) });

    // ---- Maximizer (AutoDrive may only reduce the requested drive) ----
    applyMaxStyle (e);
    MaximizerParams mp;
    mp.driveDb = std::max (0.0f, e[MaxDriveDb] + autoDrive.getReductionDb());
    mp.ceilingDb = e[MaxCeilingDb];
    mp.clipAmount = e[MaxClipAmount];
    mp.clipKnee = e[MaxClipKnee];
    // While glue is armed - set in the preset, or a macro that raises it
    // (Boost Intensity, Loudness) is off zero - a tiny floor keeps the
    // maximizer's 3-band splitter engaged. Switching glue fully off/on
    // crossfades the input against its own all-pass-shifted band sum, which
    // comb-nulls 120 Hz and 4 kHz for the fade; without the floor that would
    // happen every time Boost crossed its glue start point (40 %). 0.001 of
    // 2:1 band compression is inaudible. With glue disarmed the splitter is
    // out of the path: its all-pass rotation raises the crest factor of
    // flat-topped (mastered) material by 1-3 dB, which the limiter would
    // otherwise have to take back.
    constexpr float kGlueFloor = 0.001f;
    const bool glueArmed = base[MaxGlue] > 0.0f || MacroMap::isArmed (base.data(), MaxGlue);
    mp.glue = glueArmed ? std::max (kGlueFloor, e[MaxGlue]) : e[MaxGlue];
    mp.releaseMs = e[MaxReleaseMs];
    mp.autoRelease = on (e, MaxAutoRelease);
    mp.clipCrestDb = e[MaxClipCrestDb];
    mp.clipMaxDepthDb = e[MaxClipMaxDb];
    mp.lfLimit = e[MaxLfLimit];
    mp.bedLiftDb = e[MaxBedLiftDb];
    maximizer.setParams (mp);
    slots[SMax].setActive (active (MaximizerOn));

    // ---- Automatic preamp (docs/11 E11): the prediction's inputs are the
    // applied values, with the bass boost the macros ask for before the
    // governor scales it (a preamp that followed the governor would feed
    // its loop). A momentary audition bypass does not move it, so holding
    // "listen without" a module plays exactly that module's own effect ----
    // The same holds for the tonal-balance rule's scale on presence and air
    // (docs/11 E07): the preamp sees what the macros ask for.
    const float* h = e;
    if (governorScale < 1.0f || tonalScale < 1.0f)
    {
        MacroMap::apply (base.data(), ungoverned.data(), 1.0f, onboardCap);
        std::copy (effective.begin(), effective.end(), headroomInput.begin());
        for (int id : { BassBoostDb, ClarityPresence, ClarityAir })
            headroomInput[static_cast<size_t> (id)] = ungoverned[static_cast<size_t> (id)];
        applySafeSpeakerBassCap (headroomInput.data()); // docs/11 E51: the macros' boost, capped as applied
        if (config.sampleRate < 42000.0)
            headroomInput[static_cast<size_t> (ClarityAir)] = 0.0f; // as applied (see Clarity)
        h = headroomInput.data();
    }
    updateHeadroom (h, config.inputChannels > 2 && ! stereoFold);

    publishEffective();
}

void ProcessingChain::applySafeSpeakerBassCap (float* e) const noexcept FLUB_NONBLOCKING
{
    // docs/11 E51: the bass lift towards DC - the bass engine's shelf plus the
    // parametric EQ's positive low shelves and low bells - scaled down
    // together to the cap; within the cap nothing moves.
    const float cap = safeSpeakerBassCapDb.load (std::memory_order_relaxed);
    if (! (cap < kNoBassCap))
        return;
    const bool bassOn = on (e, BassOn), eqOn = on (e, EqOn);
    const auto lowLift = [e] (int b)
    {
        if (! on (e, eq (b, EqFieldOn)) || e[eq (b, EqFieldGain)] <= 0.0f)
            return false;
        const auto type = static_cast<EqBandType> (idx (e, eq (b, EqFieldType)));
        return type == EqBandType::LowShelf || (type == EqBandType::Bell && e[eq (b, EqFieldFreq)] <= kSafeSpeakerBassBandHz);
    };
    float total = bassOn ? std::max (0.0f, e[BassBoostDb]) : 0.0f;
    if (eqOn)
        for (int b = 0; b < kEqBands; ++b)
            if (lowLift (b))
                total += e[eq (b, EqFieldGain)];
    if (total <= cap)
        return;
    const float scale = cap / total;
    if (bassOn && e[BassBoostDb] > 0.0f)
        e[BassBoostDb] *= scale;
    if (eqOn)
        for (int b = 0; b < kEqBands; ++b)
            if (lowLift (b))
                e[eq (b, EqFieldGain)] *= scale;
}

void ProcessingChain::updateHeadroom (const float* h, bool surroundFold) noexcept FLUB_NONBLOCKING
{
    static_assert (kHeadroomKeySize == kHeadroomParamCount + 3 + LoudnessContour::kNumSections);
    std::array<float, kHeadroomKeySize> key {};
    size_t k = 0;
    for (int id : kHeadroomScalarIds)
        key[k++] = h[id];
    for (int b = 0; b < kEqBands; ++b)
        for (EqField f : kHeadroomEqFields)
            key[k++] = h[eq (b, f)];
    for (int b = 0; b < kDynEqBands; ++b)
        for (DynField f : kHeadroomDynFields)
            key[k++] = h[dyn (b, f)];
    key[k++] = surroundFold ? 1.0f : 0.0f;
    // The loudness contour's target (docs/11 E32): its sections and trim.
    for (int s = 0; s < LoudnessContour::kNumSections; ++s)
        key[k++] = contour.getTargetGainDb (s);
    key[k++] = contour.getTargetTrimDb();
    // The Warmth tilt's level compensation at the target amount (docs/11
    // E14): -amount x its measured L1, followed in 0.25 dB steps (the
    // measure moves over seconds; a step re-runs the prediction once).
    {
        const float trim = h[WarmthTone] > 0.0f ? -h[WarmthTone] * warmthTilt.getFullTiltLoudnessDb() : 0.0f;
        if (trim == 0.0f || std::abs (trim - warmthTrimModelDb) >= 0.25f)
            warmthTrimModelDb = trim;
    }
    key[k] = warmthTrimModelDb;

    const bool first = ! headroomKeyValid;
    if (! first && (key == headroomKey || headroomHoldoff > 0))
        return;
    headroomKey = key;
    headroomKeyValid = true;
    headroomHoldoff = std::max (1, static_cast<int> (kHeadroomUpdateMs * 0.001 * config.sampleRate));

    buildStaticBoostModel (h, config.sampleRate, surroundFold, headroomModel);
    // The contour's lift net of its own trim (docs/11 E32): what it leaves
    // in counts against the allowance like any other static boost.
    {
        std::array<SvfCoeffs, LoudnessContour::kNumSections> sections {};
        const int count = contour.getTargetSections (sections.data());
        for (int s = 0; s < count && headroomModel.numSections < StaticBoostModel::kMaxSections; ++s)
            headroomModel.sections[static_cast<size_t> (headroomModel.numSections++)] = sections[static_cast<size_t> (s)];
        headroomModel.gainDb += contour.getTargetTrimDb();
    }
    // The Warmth tilt's trim (docs/11 E14): on bass-heavy programme it
    // already takes most of its sections' lift back.
    headroomModel.gainDb += warmthTrimModelDb;
    const headroom::Prediction p = predictStaticBoost (headroomModel, headroom::Weighting::Programme);
    const float preamp = on (h, AutoPreampOn) ? headroom::preampDb (p, h[AutoPreampAllowanceDb]) : 0.0f;
    predictedBoostDb.store (static_cast<float> (p.maxBoostDb), std::memory_order_relaxed);
    predictedBoostHz.store (static_cast<float> (p.atHz), std::memory_order_relaxed);
    staticPreampDb = preamp;
    staticGainDb = static_cast<float> (p.maxBoostDb) + preamp; // what the allowance leaves in (hot programme)
    autoPreampDb.store (preamp + hotPreampDb, std::memory_order_relaxed);
    preampGain.setTarget (dbToGain (preamp + hotPreampDb));
    if (first)
        preampGain.setImmediate (preampGain.getTarget());
}

void ProcessingChain::updateHotPreamp (const AudioBlock& st, bool contaminated) noexcept FLUB_NONBLOCKING
{
    // Hot programme (auto.preampHot, docs/11 E11; see the header comment).
    const float* e = effective.data();
    if (! (on (e, AutoPreampOn) && on (e, AutoPreampHot)))
    {
        if (hotPreampDb != 0.0f || hotPeakDb > kMinusInfDb)
        {
            hotPreampDb = 0.0f;
            hotPeakDb = kMinusInfDb;
            hotHoldLeft = 0;
            autoPreampDb.store (staticPreampDb, std::memory_order_relaxed);
            preampGain.setTarget (dbToGain (staticPreampDb));
        }
        return;
    }
    const int n = st.numSamples;
    if (! contaminated)
    {
        float peak = 0.0f;
        for (int c = 0; c < st.numChannels; ++c)
        {
            const float* x = st.channel (c);
            for (int i = 0; i < n; ++i)
                peak = std::max (peak, std::abs (x[i]));
        }
        const float peakDb = peak > 0.0f ? std::max (kMinusInfDb, gainToDb (peak)) : kMinusInfDb;
        if (peakDb >= hotPeakDb)
        {
            hotPeakDb = peakDb;
            hotHoldLeft = static_cast<int> (kHotHoldSeconds * config.sampleRate);
        }
        else if (hotHoldLeft > 0)
        {
            hotHoldLeft = std::max (0, hotHoldLeft - n);
        }
        else
        {
            hotPeakDb = std::max (peakDb, hotPeakDb - kHotReleaseDbPerSecond * static_cast<float> (n / config.sampleRate));
        }
    }
    const bool maxOn = on (e, MaximizerOn);
    const float attack = on (e, ClarityOn) ? 0.5f * std::max (0.0f, e[ClarityAttackDb]) : 0.0f; // the transient shaper's lift of an onset
    const float gain = staticGainDb + attack + (maxOn ? e[MaxDriveDb] : 0.0f);
    const float ceiling = maxOn ? e[MaxCeilingDb] : 0.0f;
    const float hot = hotPeakDb > kMinusInfDb ? -std::clamp (hotPeakDb + gain - ceiling, 0.0f, std::max (0.0f, gain)) : 0.0f;
    if (hot != hotPreampDb)
    {
        hotPreampDb = hot;
        autoPreampDb.store (staticPreampDb + hot, std::memory_order_relaxed);
        preampGain.setTarget (dbToGain (staticPreampDb + hot));
    }
}

double ProcessingChain::StaticBoostModel::responseDb (double freqHz) const noexcept FLUB_NONBLOCKING
{
    // |H|^2 of each SVF section at s = j W, W = tan (pi f / fs) / g (Svf.h):
    //   H = (m0 (1 - W^2) + m2 + j (m0 k + m1) W) / (1 - W^2 + j k W)
    const double t = std::tan (kPi * std::clamp (freqHz, 0.0, 0.4999 * sampleRate) / sampleRate);
    double power = 1.0;
    for (int i = 0; i < numSections; ++i)
    {
        const SvfCoeffs& c = sections[static_cast<size_t> (i)];
        const double w = t / c.g, w2 = w * w;
        const double m0 = c.m0, m1 = c.m1, m2 = c.m2;
        const double nr = m0 * (1.0 - w2) + m2, ni = (m0 * c.k + m1) * w;
        const double dr = 1.0 - w2, di = c.k * w;
        power *= (nr * nr + ni * ni) / std::max (dr * dr + di * di, 1.0e-300);
    }
    return gainDb + 10.0 * std::log10 (std::max (power, 1.0e-30));
}

void ProcessingChain::buildStaticBoostModel (const float* e, double sampleRate, bool surroundFold,
                                             StaticBoostModel& m) noexcept FLUB_NONBLOCKING
{
    m.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    m.numSections = 0;
    m.gainDb = surroundFold ? 20.0 * std::log10 (static_cast<double> (Bs775Fold::kMatrixGain)) : 0.0;
    const double sr = m.sampleRate;
    const auto add = [&m] (const SvfCoeffs& c) {
        if (m.numSections < StaticBoostModel::kMaxSections)
            m.sections[static_cast<size_t> (m.numSections++)] = c;
    };
    const auto addButterworth = [&add, sr] (FilterType type, double hz, int numSections) {
        for (int s = 0; s < numSections; ++s)
            add (SvfCoeffs::make (type, hz, butterworthQ (numSections, s), 0.0, sr));
    };

    if (on (e, EqOn))
    {
        for (int b = 0; b < kEqBands; ++b)
        {
            if (! on (e, eq (b, EqFieldOn)))
                continue;
            const double f = e[eq (b, EqFieldFreq)], q = e[eq (b, EqFieldQ)], g = e[eq (b, EqFieldGain)];
            switch (static_cast<EqBandType> (std::clamp (idx (e, eq (b, EqFieldType)), 0, 6)))
            {
                case EqBandType::Bell: if (g != 0.0) add (SvfCoeffs::make (FilterType::Bell, f, q, g, sr)); break;
                case EqBandType::LowShelf: if (g != 0.0) add (SvfCoeffs::make (FilterType::LowShelf, f, q, g, sr)); break;
                case EqBandType::HighShelf: if (g != 0.0) add (SvfCoeffs::make (FilterType::HighShelf, f, q, g, sr)); break;
                case EqBandType::Notch: add (SvfCoeffs::make (FilterType::Notch, f, q, 0.0, sr)); break;
                case EqBandType::BandPass: add (SvfCoeffs::make (FilterType::BandPass, f, q, 0.0, sr)); break;
                case EqBandType::LowCut: addButterworth (FilterType::HighPass, f, std::clamp (idx (e, eq (b, EqFieldSlope)) + 1, 1, 4)); break;
                case EqBandType::HighCut: addButterworth (FilterType::LowPass, f, std::clamp (idx (e, eq (b, EqFieldSlope)) + 1, 1, 4)); break;
            }
        }
        m.gainDb += e[EqOutputGainDb];
    }
    if (on (e, DynEqOn))
    {
        static constexpr FilterType shapes[] = { FilterType::Bell, FilterType::LowShelf, FilterType::HighShelf };
        for (int b = 0; b < kDynEqBands; ++b)
            if (on (e, dyn (b, DynFieldOn)) && e[dyn (b, DynFieldStaticGain)] != 0.0f)
                add (SvfCoeffs::make (shapes[std::clamp (idx (e, dyn (b, DynFieldShape)), 0, 2)], e[dyn (b, DynFieldFreq)],
                                      e[dyn (b, DynFieldQ)], e[dyn (b, DynFieldStaticGain)], sr));
    }
    if (on (e, BassOn))
    {
        if (e[BassSubsonic] > 0.0f)
            addButterworth (FilterType::HighPass, std::clamp (static_cast<double> (e[BassSubsonic]), 10.0, 40.0),
                            idx (e, BassSubsonicOrder) == static_cast<int> (SubsonicOrderValue::Slope12) ? 1 : 2);
        if (e[BassBoostDb] > 0.0f)
            add (SvfCoeffs::make (FilterType::LowShelf, std::clamp (static_cast<double> (e[BassBoostFreq]), 30.0, 200.0), kBassShelfQ,
                                  std::min (static_cast<double> (e[BassBoostDb]), kBassMaxBoostDb), sr));
    }
    if (on (e, ClarityOn))
    {
        if (e[ClarityPresence] > 0.0f)
            add (SvfCoeffs::make (FilterType::Bell, e[ClarityPresenceFreq], kPresenceQ, kPresenceMaxDb * e[ClarityPresence], sr));
        if (e[ClarityAir] > 0.0f)
            add (SvfCoeffs::make (FilterType::HighShelf, kAirShelfHz, kAirShelfQ, kAirShelfMaxDb * e[ClarityAir], sr));
    }
    if (e[WarmthTone] > 0.0f)
    {
        // The Warmth tilt's sections (docs/11 E14); its level compensation
        // follows the programme, so updateHeadroom() adds it as measured.
        SvfCoeffs body, high;
        ToneTilt::sections (e[WarmthTone], sr, body, high);
        add (body);
        add (high);
    }
    if (on (e, SaturationOn))
    {
        // Unity small-signal curve; the wet path carries the make-up.
        const double mix = std::clamp (static_cast<double> (e[SatMix]), 0.0, 1.0);
        m.gainDb += 20.0 * std::log10 (std::max (1.0 - mix + mix * std::pow (10.0, e[SatOutputDb] / 20.0), 1.0e-6));
    }
}

headroom::Prediction ProcessingChain::predictStaticBoost (const StaticBoostModel& m, headroom::Weighting weighting) noexcept FLUB_NONBLOCKING
{
    return headroom::predictMaxBoostWith ([&m] (double f) { return m.responseDb (f); }, weighting, 20.0,
                                          std::min (20000.0, 0.49 * m.sampleRate));
}

void ProcessingChain::foldToStereo (const AudioBlock& in) noexcept
{
    constexpr float k = Bs775Fold::kMatrixGain;
    const bool virtSmoothing = virtMix.isSmoothing(), passSmoothing = passMix.isSmoothing();
    const float v0 = virtMix.getCurrent(), p0 = passMix.getCurrent();
    // A path that did not run in the previous block starts from silence
    // rather than from stale history.
    const auto runVirtualizer = [this] (const AudioBlock& b) {
        if (! virtRan)
            virtualizer.reset();
        virtualizer.process (b);
        virtRan = true;
    };
    const auto runFold = [this] (const AudioBlock& b, float overall) {
        if (! foldRan)
            fold.reset();
        fold.process (b, overall);
        foldRan = true;
    };
    // Fold headroom (docs/11 E28a) of the surround fold k D, like the
    // virtualiser's of B: it starts from unity when k D comes back.
    const auto startHeadroom = [this] {
        if (! foldHeadroomRan)
            foldHeadroom.reset();
        foldHeadroomRan = true;
    };
    // The deepest headroom gain of the folds that ran, for the meters.
    const auto noteHeadroom = [this] {
        if (virtRan)
            headroomBlockMinDb = std::min (headroomBlockMinDb, virtualizer.getHeadroomGainDb());
        if (foldHeadroomRan)
            headroomBlockMinDb = std::min (headroomBlockMinDb, foldHeadroom.getGainDb());
    };

    if (! virtSmoothing && ! passSmoothing)
    {
        if (p0 > 0.5f)
        {
            runFold (in, 1.0f); // stereo passthrough (no headroom: exactly the 2-channel stream)
            virtRan = foldHeadroomRan = false;
        }
        else if (v0 > 0.5f)
        {
            runVirtualizer (in);
            foldRan = foldHeadroomRan = false;
        }
        else
        {
            runFold (in, k); // BS.775 downmix
            startHeadroom();
            foldHeadroom.process (in.channel (0), in.channel (1), in.numSamples);
            virtRan = false;
        }
        noteHeadroom();
        passStatsValid = false;
        return;
    }

    // Crossfade, so that no fold change ever clicks: the folds differ in
    // level and timing (ITD, head shadow). D (unity matrix) goes to the
    // scratch copy, B (binaural) is rendered in place when it has weight.
    const bool virtWeight = (virtSmoothing || v0 > 0.0f) && (passSmoothing || p0 < 1.0f);
    const int n = in.numSamples;
    const AudioBlock d = foldScratch.block (config.inputChannels, n);
    d.copyFrom (in);
    runFold (d, 1.0f);
    if (virtWeight)
        runVirtualizer (in);
    else
        virtRan = false;

    // The fold headroom's gain per sample for k D (the steady BS.775 path's
    // law and peaks: |k D| = k |D|), in d's channel 2: the fold has read FC.
    float* const headroom = d.channel (2);
    startHeadroom();
    for (int i = 0; i < n; ++i)
        headroom[i] = foldHeadroom.next (k * std::max (std::abs (d.channel (0)[i]), std::abs (d.channel (1)[i])));
    noteHeadroom();

    // The surround fold S = (1 - w) h k D + w B ramps linearly with virt.on
    // (w, 20 ms; h the fold headroom, 1 below 0 dBFS). The stereo fold ramps
    // linearly from S to D (p, 400 ms) and is power-compensated: g scales
    // the mix so that its RMS follows (1 - p) RMS_S + p RMS_D whatever the
    // correlation of S and D (for fully correlated folds g is 1: the plain
    // linear ramp). A plain linear
    // ramp dips by up to 3 dB mid-fade where they are uncorrelated (the
    // binaural render's highs), an equal-power one swells by up to 3 dB where
    // they are correlated (its lows: +3.4 dB on centred pink noise). The
    // statistics are this block's (both paths are rendered before the mix),
    // smoothed over 50 ms; g is exactly 1 at either end of the ramp.
    if (passSmoothing)
    {
        auto ramp = virtMix; // a copy: the mix below replays the same ramp
        double ss = 0.0, dd = 0.0, sd = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const float w = ramp.next();
            const float ka = (1.0f - w) * k, a = headroom[i] < 1.0f ? ka * headroom[i] : ka, b = virtWeight ? w : 0.0f;
            for (int ch = 0; ch < 2; ++ch)
            {
                const double dv = d.channel (ch)[i];
                const double sv = a * d.channel (ch)[i] + b * in.channel (ch)[i];
                ss += sv * sv;
                dd += dv * dv;
                sd += sv * dv;
            }
        }
        const double norm = 1.0 / (2.0 * n);
        ss *= norm;
        dd *= norm;
        sd *= norm;
        if (! passStatsValid)
        {
            passSs = ss;
            passDd = dd;
            passSd = sd;
            passStatsValid = true;
        }
        else
        {
            const double a = 1.0 - std::exp (-n / (kPassStatsMs * 0.001 * config.sampleRate));
            passSs += a * (ss - passSs);
            passDd += a * (dd - passDd);
            passSd += a * (sd - passSd);
        }
    }
    else
    {
        passStatsValid = false;
    }

    for (int i = 0; i < n; ++i)
    {
        const float w = virtMix.next();
        const float p = std::clamp (passMix.next(), 0.0f, 1.0f);
        const float ka = (1.0f - w) * k, a = headroom[i] < 1.0f ? ka * headroom[i] : ka, b = virtWeight ? w : 0.0f;
        float g = 1.0f;
        if (passSmoothing)
        {
            const double q = 1.0 - p;
            const double mix = q * q * passSs + static_cast<double> (p) * p * passDd + 2.0 * p * q * passSd;
            const double rms = q * std::sqrt (passSs) + p * std::sqrt (passDd);
            const double target = rms * rms;
            if (mix > 1.0e-30)
                g = static_cast<float> (std::clamp (std::sqrt (target / mix), 0.5, 2.0));
        }
        for (int ch = 0; ch < 2; ++ch)
        {
            float* y = in.channel (ch);
            const float dv = d.channel (ch)[i];
            const float sv = a * dv + b * y[i];
            y[i] = g * (sv + p * (dv - sv));
        }
    }
}

void ProcessingChain::process (const AudioBlock& io) noexcept FLUB_NONBLOCKING
{
    const int n = io.numSamples;
    if (n <= 0)
        return;

    // ---- 0. Input sanitiser (docs/11 E10; see the header comment) ----
    // A single NaN/Inf from a misbehaving driver or upstream plug-in would latch
    // forever in IIR state: drop the block and restart the signal path cleanly.
    // The control loops (governor, AutoLevel, AutoDrive, ComparisonMatcher) have
    // not seen this block, so their converged state is kept.
    float checksum = 0.0f, peak = 0.0f;
    for (int c = 0; c < io.numChannels; ++c)
    {
        const float* x = io.channel (c);
        for (int i = 0; i < n; ++i)
        {
            checksum += x[i] * 0.0f;
            peak = std::max (peak, std::abs (x[i]));
        }
    }
    if (! std::isfinite (checksum))
    {
        io.clear();
        resetSignalState();
        meterBus.droppedBlockCount.store (++droppedBlocks, std::memory_order_relaxed);
        return;
    }
    // Finite garbage (1e30 from a decoder fault) passes the check above:
    // mute each such sample and hide the block from the control loops.
    const bool contaminated = peak > kSanitiseLimit;
    if (contaminated)
    {
        for (int c = 0; c < io.numChannels; ++c)
        {
            float* x = io.channel (c);
            for (int i = 0; i < n; ++i)
                if (std::abs (x[i]) > kSanitiseLimit)
                {
                    x[i] = 0.0f;
                    ++corruptSamples;
                }
        }
        meterBus.corruptSampleCount.store (corruptSamples, std::memory_order_relaxed);
    }

    // The block runs in segments that end where the governor ticks (its 10 ms
    // grid, docs/11 E06), so a new scale takes effect at the same sample
    // whatever the host block size: a 4096-sample block no longer holds a
    // scale for 85 ms that a 64-sample host would have updated at the tick.
    blockReadings = {};
    for (int start = 0; start < n;)
    {
        const int length = std::min (n - start, governor.samplesToNextTick());
        processSegment (io.subBlock (start, length), contaminated);
        start += length;
    }
    publishMeters (io.firstChannels (2), n);
}

void ProcessingChain::accumulateReadings() noexcept
{
    // The modules' "deepest in the last block" readings cover one segment:
    // keep the host block's extremes, so the meters read as they did before
    // the block was split (the clip energy ratio over the whole block).
    auto& r = blockReadings;
    if (! slots[SSmooth].isFullyBypassed())
        r.smoothCutDb = std::min (r.smoothCutDb, smoothness.getCutDb());
    if (! slots[SComp].isFullyBypassed())
    {
        r.compGrDb = std::min (r.compGrDb, compressor.getGainReductionDb());
        r.compUpDb = std::max (r.compUpDb, compressor.getUpwardGainDb());
    }
    if (! slots[SMax].isFullyBypassed())
    {
        r.maxGrDb = std::min (r.maxGrDb, maximizer.getGainReductionDb());
        r.glueGrDb = std::min (r.glueGrDb, maximizer.getGlueReductionDb());
        // Removed energy = ratio x input energy, summed over the segments.
        const double in = maximizer.getClipInputEnergy();
        r.clipRemoved += in * std::pow (10.0, 0.1 * static_cast<double> (maximizer.getClipEnergyRatioDb()));
        r.clipInput += in;
    }
}

void ProcessingChain::protectionTap (const AudioBlock& st, int slot, bool contaminated) noexcept FLUB_NONBLOCKING
{
    const int n = st.numSamples;
    const float* l = st.channel (0);
    const float* r = st.channel (1);
    const auto mid = [n, l, r] (std::vector<float>& to) {
        for (int i = 0; i < n; ++i)
            to[static_cast<size_t> (i)] = 0.5f * (l[i] + r[i]);
    };
    const auto delayed = [n] (std::vector<float>& x, DelayLine& d) {
        float* ch[1] = { x.data() };
        d.process (AudioBlock (ch, 1, n));
    };
    switch (slot)
    {
        case SDynEq:
            // The tonal-balance rule's reference (docs/11 E07).
            if (! contaminated)
                tonalMeter.processReference (st);
            return;
        case SBass:
            // The bass span's input (and the input's PLR).
            mid (bassSpanInput);
            delayed (bassSpanInput, bassSpanDelay);
            if (! contaminated)
                inputPlrMeter.process (st);
            return;
        case SClarity:
            // The bass span's output.
            mid (spanOutput);
            if (! contaminated)
                bassSpan.process (bassSpanInput.data(), spanOutput.data(), n);
            return;
        case SSat:
            mid (driveSpanInput);
            delayed (driveSpanInput, driveSpanDelay);
            return;
        case SMax:
            if (! contaminated)
                for (int i = 0; i < n; ++i)
                {
                    preMaxPeak = std::max ({ preMaxPeak, std::abs (l[i]), std::abs (r[i]) });
                    preMaxEnergy[0] += static_cast<double> (l[i]) * l[i];
                    preMaxEnergy[1] += static_cast<double> (r[i]) * r[i];
                }
            preMaxSamples += n;
            return;
        default: break;
    }
    // The drive span's output and the output's PLR; the feed-forward's and
    // the PLRs' windows step with the governor's ticks (segments end on them).
    if (! contaminated)
    {
        mid (spanOutput);
        driveSpan.process (driveSpanInput.data(), spanOutput.data(), n);
        plrMeter.process (st);
        tonalMeter.processOutput (st);
    }
    if (governor.samplesToNextTick() == n)
    {
        // The tick's peak and RMS (the louder channel, as the clipper's crest gate links them).
        const double energy = std::max (preMaxEnergy[0], preMaxEnergy[1]);
        feedForward.pushTick (preMaxPeak, preMaxSamples > 0 ? static_cast<float> (std::sqrt (energy / preMaxSamples)) : 0.0f);
        preMaxEnergy = {};
        preMaxSamples = 0;
        plrMeter.tick();
        inputPlrMeter.tick();
        tonalMeter.tick();
        preMaxPeak = 0.0f;
        // The bass harmonics' share, smoothed per tick (300 ms): its window
        // closes on this grid, so the reading does not depend on the host's blocks.
        const float share = ! slots[SBass].isFullyBypassed() ? bass.getDistortionDb() : kMinusInfDb;
        bassShareSmoothedPow = kBassShareSmoothing * bassShareSmoothedPow
                               + (1.0 - kBassShareSmoothing) * (share > kMinusInfDb ? std::pow (10.0, 0.1 * static_cast<double> (share)) : 0.0);
    }
}

SafetyGovernor::Readings ProcessingChain::governorReadings (float limiterGrDb, float distortionDb, bool closesTick) noexcept FLUB_NONBLOCKING
{
    SafetyGovernor::Readings r;
    r.limiterGrDb = limiterGrDb;
    r.distortionDb = distortionDb;
    if (driveSpan.hasReading())
        r.driveResidualDb = driveSpan.getWeightedDb();
    // The bass engine's audible residual is split by the share its harmonics
    // generator's own meter gives the harmonics (measured exactly around the
    // generator): that share is the harmonics scale's; the rest - the
    // protection riding the boost shelf (on a steady 50 Hz tone its 10 ms
    // envelope ripples and adds a 3rd harmonic), the generator's envelope on
    // noise - follows the bass boost, which the drive scale governs, so it
    // joins the drive span's reading.
    if (bassSpan.hasReading())
    {
        const float flat = bassSpan.getPlainResidualDb(), audible = bassSpan.getWeightedDb();
        const float harmonics = bassShareSmoothedPow > 0.0 ? static_cast<float> (10.0 * std::log10 (bassShareSmoothedPow)) : kMinusInfDb;
        const double share = flat > kMinusInfDb && harmonics > kMinusInfDb ? std::min (1.0, std::pow (10.0, 0.1 * static_cast<double> (harmonics - flat))) : 0.0;
        if (share > 0.0)
            r.harmonicsResidualDb = audible + static_cast<float> (10.0 * std::log10 (share));
        if (share < 1.0)
            r.driveResidualDb = DistortionMonitor::combineDb (r.driveResidualDb, audible + static_cast<float> (10.0 * std::log10 (1.0 - share)));
    }
    lastHarmonicsResidualDb = r.harmonicsResidualDb;
    lastDriveLoopResidualDb = r.driveResidualDb;
    r.plrDb = plrMeter.getPlrDb();
    r.inputPlrDb = inputPlrMeter.getPlrDb();
    r.presenceLiftDb = tonalMeter.getLiftDb (TonalBalanceMeter::Presence);
    r.harshLiftDb = tonalMeter.getLiftDb (TonalBalanceMeter::Harsh);
    r.airLiftDb = tonalMeter.getLiftDb (TonalBalanceMeter::Air);
    // Feed-forward: the drive the pre-maximizer peaks allow at the limiter
    // loop's set point, as a share of the drive at the full scale; only for
    // a segment that closes a tick (the governor reads nothing else, and
    // the prediction models the limiter's envelope over 3 s of ticks).
    if (closesTick && driveAtFullScale > 0.0f)
    {
        const auto b = SafetyGovernor::budgetsFor (appliedStrength, idx (effective.data(), Mode) == static_cast<int> (ModeValue::Music));
        // The clipper as the maximizer sets it (LoudnessMaximizer: headroom
        // lerp (+6, +0.3 dB, amount) over the ceiling; amount 0 = off).
        const float clipAmount = effective[static_cast<size_t> (MaxClipAmount)];
        feedForward.setClipper (clipAmount > 0.0f ? 6.0f + (0.3f - 6.0f) * std::min (1.0f, clipAmount) : std::numeric_limits<float>::infinity(),
                                effective[static_cast<size_t> (MaxClipCrestDb)], effective[static_cast<size_t> (MaxClipMaxDb)]);
        const float allowed = feedForward.driveForBudget (effective[static_cast<size_t> (MaxCeilingDb)], b.grDb + SafetyGovernor::kSetPointMarginDb);
        if (std::isfinite (allowed))
            r.feedForwardScale = std::clamp (allowed / driveAtFullScale, 0.0f, 1.0f);
    }
    r.feedForwardValid = ! (driveAtFullScale > 0.0f) || feedForward.hasReading(); // nothing to predict counts as read
    r.driveAtFullScaleDb = std::max (0.0f, driveAtFullScale); // the release tie's unit (docs/11 E06 batch 2)
    r.driveMaxDb = layout()[static_cast<size_t> (MaxDriveDb)].maxValue;
    return r;
}

void ProcessingChain::updateAttackCoupling (float limiterGrDb, bool active) noexcept FLUB_NONBLOCKING
{
    // Once per governor tick (the maximizer's GR window): the programme GR
    // follows the tick's deepest GR with 1 s, the transient GR is the
    // excess over it, held with a ~1 s release.
    const float gr = active && std::isfinite (limiterGrDb) ? std::clamp (-limiterGrDb, 0.0f, 24.0f) : 0.0f;
    programGrDb += kProgramGrCoeff * (gr - programGrDb);
    transientGrDb = std::max (gr - programGrDb, transientGrDb * kTransientGrReleasePerTick);
    const float* e = effective.data();
    float target = 0.0f;
    if (active && static_cast<ModeValue> (idx (e, Mode)) == ModeValue::Music)
    {
        const float amount = smoothstep (kAttackCoupleBoostStart, kAttackCoupleBoostEnd, base[static_cast<size_t> (BoostIntensity)]);
        target = std::min (kAttackCoupleMaxDb, kAttackCouplePerDb * transientGrDb) * amount * governor.getScale();
    }
    attackCoupleDb += std::clamp (target - attackCoupleDb, -kAttackCoupleSlewDbPerTick, kAttackCoupleSlewDbPerTick);
}

void ProcessingChain::processSegment (const AudioBlock& io, bool contaminated) noexcept FLUB_NONBLOCKING
{
    const int n = io.numSamples;
    store.snapshot (base.data());
    if (redetectRequest.exchange (false, std::memory_order_relaxed))
        inputDetector.reset();
    if (meterBus.resetLoudnessRequest.exchange (false, std::memory_order_acq_rel))
    {
        outLoudness.resetIntegrated();
        outTruePeak.reset();
    }
    // The on-board enhancement cap (docs/11 E16) glides like a macro knob
    // turned over kOnboardCapGlideMs (the modules smooth what it moves).
    const float capTarget = onboardCapRequest.load (std::memory_order_relaxed) ? 1.0f : 0.0f;
    if (onboardCapSnap)
        onboardCap = capTarget;
    else if (onboardCap != capTarget)
    {
        const float step = static_cast<float> (n / (0.001 * kOnboardCapGlideMs * config.sampleRate));
        onboardCap = capTarget > onboardCap ? std::min (capTarget, onboardCap + step) : std::max (capTarget, onboardCap - step);
    }
    onboardCapSnap = false;
    meterBus.onboardCapActive.store (onboardCap > 0.0f, std::memory_order_relaxed);
    applyParameters();
    headroomHoldoff = std::max (0, headroomHoldoff - n);
    const float* e = effective.data();

    // ---- 1. Input stage (all input channels) ----
    const AudioBlock in = io.firstChannels (config.inputChannels);
    const float g0 = inputGain.getCurrent();
    in.applyGainRamp (g0, inputGain.skip (n));
    if (contaminated)
        autoLevel.processUnmeasured (in);
    else
        autoLevel.process (in);
    // The cue enhancer's and the upward compressor's backgrounds (docs/11
    // E19 step 4) are kept in the terms of the level before AutoLevel's gain
    // (0 dB while it is off: nothing changes).
    dynEq.setReferenceOffsetDb (autoLevel.getGainDb());
    compressor.setReferenceOffsetDb (autoLevel.getGainDb());

    // ---- 2. Fold to stereo ----
    inputDetector.process (in);
    if (config.inputChannels > 2)
        foldToStereo (in);
    const AudioBlock st = io.firstChannels (2);

    // ---- 3. Dry reference for global bypass / A-B ----
    const AudioBlock dry = dryBuffer.block (2, n);
    dry.copyFrom (st);
    if (! contaminated)
        loudnessMatch.measureDry (dry);
    dryDelay.process (dry);

    inLevel.process (st);
    inLoudness.process (st);
    // The bed-lift budget (docs/11 E19 step 3, see the header comment):
    // the input's short-term level, and whether this is the programme's bed
    // (not an event: a 400 ms loudness more than kBedEventLu over the
    // input's background; not loud programme; not within the hold after).
    const bool bedBudget = e[MaxBedLiftDb] < kNoBedLiftBudgetDb;
    if (bedBudget)
    {
        bedInShort.process (st);
        bedEventLoudness.process (st);
        if (bedInBackground.get() > kMinusInfDb && ! bedInBackground.isLearning()
            && bedEventLoudness.getLufs() > bedInBackground.get() + kBedEventLu)
            bedLiftHold = static_cast<int> (kBedLiftHoldSeconds * config.sampleRate);
    }
    const bool bedMeasure = bedBudget && ! contaminated && bedLiftHold == 0;
    for (int i = 0; i < n; ++i)
        tapScratch[static_cast<size_t> (i)] = 0.5f * (st.channel (0)[i] + st.channel (1)[i]);
    analyzerTaps.pre.push (tapScratch.data(), static_cast<size_t> (n));

    // ---- 4. Automatic preamp (docs/11 E11; unity and untouched while off), module slots ----
    updateHotPreamp (st, contaminated);
    if (preampGain.isSmoothing() || preampGain.getCurrent() != 1.0f)
    {
        const float p0 = preampGain.getCurrent();
        st.applyGainRamp (p0, preampGain.skip (n));
    }
    // The loudness contour (docs/11 E32): untouched while it idles.
    contour.process (st);
    bool smoothProcessed = false;
    for (int s = 0; s < kNumSlots; ++s)
        if (inChain (s))
        {
            // The maximizer's bed-lift budget counts the lift of everything
            // ahead of it: the background of the programme's level here
            // against that of the chain's input, stepped every 10 ms while
            // the programme is its bed.
            if (s == SMax && bedBudget)
            {
                preMaxShort.process (st);
                for (bedStepCount += n; bedStepCount >= bedStepLength; bedStepCount -= bedStepLength)
                    if (bedMeasure)
                    {
                        bedInBackground.update (bedInShort.getLufs(), kBedFloorLufs);
                        preMaxBackground.update (preMaxShort.getLufs(), kBedFloorLufs);
                        if (! bedInBackground.isLearning() && bedInBackground.get() > kBedFloorLufs)
                            maximizer.setUpstreamLiftDb (preMaxBackground.get() - bedInBackground.get());
                    }
            }
            // The per-ear stage (docs/11 E33) ahead of the compressor slot,
            // the Startle Guard and the maximizer: their stereo-linked gains
            // keep the difference between the ears that it sets.
            if (s == SComp && personal.getPlacement() == PersonalPlacement::BeforeCompressor)
                personal.process (st);
            // The Startle Guard (docs/11 E21) measures what enters the
            // compressor slot and turns down what leaves it, one slot
            // latency later: its look-ahead, without latency of its own.
            if (s == SComp)
            {
                startleGuard.setLevelOffsetDb (autoLevel.getGainDb());
                startleGuard.measure (st, contaminated);
            }
            // The Warmth tilt (docs/11 E14) ahead of the parametric EQ: after
            // the gate and the neural slot, before every stage the governor
            // scales or taps, so its open-loop measure stays open loop and
            // its body bell does not lift the bass engine's harmonics;
            // untouched while it idles.
            if (s == SEq)
                warmthTilt.process (st);
            if (spanRunning && (s == SDynEq || s == SBass || s == SClarity || s == SSat || s == SMax))
                protectionTap (st, s, contaminated); // the governor's spans and pre-maximizer peak (docs/11 E06), tonal reference (E07)
            // The Smoothness stage's reference (docs/11 E07): the dynamic
            // EQ's input, while the stage runs.
            const bool smoothRunning = slots[SSmooth].isActive() || ! slots[SSmooth].isFullyBypassed();
            if (s == SDynEq && smoothRunning)
                smoothReference.block (2, n).copyFrom (st);
            if (s == SSmooth && smoothRunning)
            {
                smoothness.setReference (smoothReference.block (2, n));
                smoothProcessed = true;
            }
            slots[static_cast<size_t> (s)].process (st);
            if (s == SComp)
                startleGuard.apply (st);
        }
    if (neuralInChain)
    {
        neuralMisses.store (neural->getDeadlineMisses(), std::memory_order_relaxed);
        neuralFailures.store (neural->getModelFailures(), std::memory_order_relaxed);
        neuralFrames.store (neural->getFramesProcessed(), std::memory_order_relaxed);
    }

    if (bedBudget)
        bedLiftHold = maximizer.getBedQuietWeight() < 0.999f ? static_cast<int> (kBedLiftHoldSeconds * config.sampleRate)
                                                             : std::max (0, bedLiftHold - n);

    // The chain's own measures of its output read it with the per-ear stage
    // undone (docs/11 E33, PersonalProfile.h): the listener's ear correction
    // is neither distortion nor brightness the chain adds. Without a
    // profile the inverse is the identity and the view equals st.
    const bool personalFirst = personal.getPlacement() == PersonalPlacement::BeforeCompressor;
    AudioBlock chainView = st;
    if (personalFirst && (spanRunning || smoothProcessed))
    {
        chainView = personalView.block (2, n);
        chainView.copyFrom (st);
        personal.processInverse (chainView);
    }
    if (spanRunning)
        protectionTap (chainView, kNumSlots, contaminated); // the governor's drive span output and PLR (docs/11 E06)
    // The Smoothness stage follows what the slots after it made of its
    // output (docs/11 E07 batch 2), before the output gain.
    if (smoothProcessed)
        smoothness.processDownstream (chainView);
    // The measured alternative placement (docs/11 E33): after the maximizer,
    // with its own per-ear limiters at the maximizer's ceiling.
    if (! personalFirst)
    {
        personal.setCeilingDb (e[MaxCeilingDb]);
        personal.process (st);
    }

    // ---- 5. Output gain ----
    const float o0 = outputGain.getCurrent();
    st.applyGainRamp (o0, outputGain.skip (n));

    // ---- 6. Control loops for the next block ----
    const bool couplingTick = governor.samplesToNextTick() <= n; // segments end on the governor's 10 ms grid
    const bool maxActive = ! slots[SMax].isFullyBypassed();
    const bool satActive = ! slots[SSat].isFullyBypassed();
    const float satDistortionDb = satActive ? saturator.getDistortionDb() : kMinusInfDb;
    const float clipDistortionDb = maxActive ? maximizer.getDistortionDb() : kMinusInfDb;
    distortion.update (satDistortionDb, clipDistortionDb, n); // measured THD+N (meters)
    // The bass harmonics and the air exciter are measured the same way, but
    // add their harmonics on purpose: tracked apart, neither in the THD+N
    // meter nor in the governor input (docs/03 §14.5).
    distortion.updateHarmonics (! slots[SBass].isFullyBypassed() ? bass.getDistortionDb() : kMinusInfDb,
                                ! slots[SClarity].isFullyBypassed() ? clarity.getDistortionDb() : kMinusInfDb, n);
    // The governor sees the clipper's share floored at its clip energy ratio
    // over the same analysis window, the former proxy: on a steady tone that
    // reads above the THD+N (it also counts the in-phase part of the removed
    // signal, a gain change), so the governor does not back off later on
    // clipping than it did on the proxy; the saturator's THD+N is added.
    float clipGovernorDb = maxActive ? std::max (clipDistortionDb, maximizer.getWindowClipEnergyDb()) : kMinusInfDb;
    // At protection strength Normal / Strict the maximizer's share is its
    // whole-stage residual when that reads higher (docs/11 E06 step 1): the
    // clipper and the limiter's gain modulation (IMD) together, which the
    // clipper's own THD+N does not see. Off keeps the clipper's reading, so
    // the default sounds as before.
    if (maxActive && appliedStrength != ProtectionStrength::Off)
        clipGovernorDb = std::max (clipGovernorDb, maximizer.getResidualDistortionDb());
    // Its GR input is the deepest limiting per fixed 10 ms window, not per
    // host block, and it ticks once per such window (docs/11 E06), so
    // neither the budget nor the scale's steps depend on the buffer size.
    // A block the sanitiser hid from the control loops only advances the grid.
    if (contaminated)
    {
        governor.skip (n);
    }
    else
    {
        const float grDb = maxActive ? maximizer.getWindowGainReductionDb() : 0.0f;
        const float stageDb = DistortionMonitor::combineDb (satDistortionDb, clipGovernorDb);
        if (appliedStrength == ProtectionStrength::Off)
            governor.update (grDb, stageDb, n);
        else
            governor.updateMeasured (governorReadings (grDb, stageDb, governor.samplesToNextTick() <= n), n);
        autoDrive.update (st, e[MaxTargetLufs], on (e, MaxAutoDrive), e[MaxDriveDb]);
        loudnessMatch.measureWet (st);
    }

    if (couplingTick && ! contaminated)
        updateAttackCoupling (maxActive ? maximizer.getWindowGainReductionDb() : 0.0f, maxActive);

    // ---- 7. Global bypass (latency-aligned, optionally loudness matched) ----
    // The louder side is turned down, never the quieter one up (docs/11 E37).
    if (contaminated)
        loudnessMatch.updateUnmeasured (on (e, BypassAll), on (e, LoudnessMatchBypass), n);
    else
        loudnessMatch.update (on (e, BypassAll), on (e, LoudnessMatchBypass), n);
    loudnessMatch.applyWetTrim (st);
    dryMatchGain.setTarget (dbToGain (loudnessMatch.getDryTrimDb()));

    if (bypassMix.getCurrent() > 0.0f || bypassMix.isSmoothing())
    {
        // The match only ever turns the reference down, but the input itself
        // can peak above the ceiling: the reference therefore passes a
        // true-peak limiter at the ceiling. It runs only while bypass is
        // engaged; started cold, it outputs silence for its latency
        // (<= ~1.4 ms), so the crossfade waits that long before it moves
        // (the dry side would otherwise enter it as a step: docs/11 E53's
        // soak found a click there).
        if (! dryLimiterRunning)
        {
            dryLimiter.reset();
            dryLimiterRunning = true;
            dryWarmup = bypassMix.getCurrent() > 0.0f ? 0 : dryLimiter.latencySamples();
        }
        for (int i = 0; i < n; ++i)
        {
            const float dg = dryMatchGain.next();
            for (int c = 0; c < 2; ++c)
                dry.channel (c)[i] *= dg;
        }
        dryLimiter.process (dry);
        for (int i = 0; i < n; ++i)
        {
            float b = bypassMix.getCurrent();
            if (dryWarmup > 0)
                --dryWarmup;
            else
                b = bypassMix.next();
            for (int c = 0; c < 2; ++c)
            {
                float* w = st.channel (c);
                w[i] += b * (dry.channel (c)[i] - w[i]);
            }
        }
    }
    else
    {
        dryLimiterRunning = false;
        dryWarmup = 0;
        dryMatchGain.skip (n);
    }

    // ---- 8. Analyser (the meters are published once per host block) ----
    accumulateReadings();
    for (int i = 0; i < n; ++i)
        tapScratch[static_cast<size_t> (i)] = 0.5f * (st.channel (0)[i] + st.channel (1)[i]);
    analyzerTaps.post.push (tapScratch.data(), static_cast<size_t> (n));

    for (int c = 2; c < io.numChannels; ++c)
        std::fill (io.channel (c), io.channel (c) + n, 0.0f);
}

void ProcessingChain::publishMeters (const AudioBlock& out, int) noexcept
{
    outLevel.process (out);
    outTruePeak.process (out);
    outLoudness.process (out);

    auto& m = meterBus;
    constexpr auto rl = std::memory_order_relaxed;
    for (int c = 0; c < 2; ++c)
    {
        m.inPeakDb[static_cast<size_t> (c)].store (inLevel.getPeakDb (c), rl);
        m.inRmsDb[static_cast<size_t> (c)].store (inLevel.getRmsDb (c), rl);
        m.outPeakDb[static_cast<size_t> (c)].store (outLevel.getPeakDb (c), rl);
        m.outRmsDb[static_cast<size_t> (c)].store (outLevel.getRmsDb (c), rl);
    }
    m.outTruePeakDb.store (std::max (outTruePeak.getBlockDb (0), outTruePeak.getBlockDb (1)), rl);
    m.outTruePeakMaxDb.store (outTruePeak.getMaxDbAllChannels(), rl);
    m.inShortTermLufs.store (inLoudness.getLufs(), rl);
    m.momentaryLufs.store (outLoudness.getMomentaryLufs(), rl);
    m.shortTermLufs.store (outLoudness.getShortTermLufs(), rl);
    m.integratedLufs.store (outLoudness.getIntegratedLufs(), rl);
    m.loudnessRangeLu.store (outLoudness.getLoudnessRangeLu(), rl);
    m.correlation.store (outLevel.getCorrelation(), rl);
    m.effectiveWidth.store (spatial.getEffectiveWidth(), rl);
    // A fully bypassed slot is not processed, so its module's readings would
    // hold their last value: publish "no action" instead (the same gate as
    // the governor and distortion inputs).
    const auto active = [this] (int slot) { return ! slots[static_cast<size_t> (slot)].isFullyBypassed(); };
    const bool compActive = active (SComp), maxActive = active (SMax), dynEqActive = active (SDynEq);
    const auto& r = blockReadings; // this host block's extremes over its segments
    m.compGainReductionDb.store (compActive ? r.compGrDb : 0.0f, rl);
    m.compUpwardGainDb.store (compActive ? r.compUpDb : 0.0f, rl);
    m.maxGainReductionDb.store (maxActive ? r.maxGrDb : 0.0f, rl);
    m.glueGainReductionDb.store (maxActive ? r.glueGrDb : 0.0f, rl);
    m.clipEnergyRatioDb.store (maxActive && r.clipInput > 0.0 && r.clipRemoved > 0.0
                                   ? static_cast<float> (std::max (static_cast<double> (kMinusInfDb), 10.0 * std::log10 (r.clipRemoved / r.clipInput)))
                                   : kMinusInfDb,
                               rl);
    m.distortionDb.store (distortion.getSmoothedDb(), rl);
    m.harmonicsDb.store (distortion.getSmoothedHarmonicsDb(), rl);
    m.bassProtectionDb.store (active (SBass) ? bass.getProtectionDb() : 0.0f, rl);
    for (int b = 0; b < DynamicEq::kMaxBands && b < MeterBus::kMaxDynBands; ++b)
        m.dynEqGainDb[static_cast<size_t> (b)].store (dynEqActive ? dynEq.getBandGainDb (b) : 0.0f, rl);
    m.governorScale.store (governor.getScale(), rl);
    m.governorState.store (static_cast<int> (governor.getState()), rl);
    m.governorReason.store (governor.getReason(), rl);
    m.governorGrDb.store (governor.getAverageGainReductionDb(), rl);
    m.governorDistortionDb.store (governor.getAverageDistortionDb(), rl);
    // The measured loop's readouts (docs/11 E06 batch 2) and brightness (E07).
    m.governorStrength.store (static_cast<int> (appliedStrength), rl);
    m.governorHarmonicsScale.store (governor.getHarmonicsScale(), rl);
    m.governorTonalScale.store (governor.getTonalScale(), rl);
    m.governorDriveResidualDb.store (spanRunning ? lastDriveLoopResidualDb : kMinusInfDb, rl);
    m.governorHarmonicsResidualDb.store (spanRunning ? lastHarmonicsResidualDb : kMinusInfDb, rl);
    m.governorBassResidualDb.store (spanRunning && bassSpan.hasReading() ? bassSpan.getWeightedDb() : kMinusInfDb, rl);
    m.governorPlrDb.store (spanRunning ? plrMeter.getPlrDb() : PlrMeter::kNoReading, rl);
    {
        const auto b = SafetyGovernor::budgetsFor (appliedStrength, idx (effective.data(), Mode) == static_cast<int> (ModeValue::Music));
        m.governorResidualBudgetDb.store (b.residualDb, rl);
        m.governorGrBudgetDb.store (b.grDb, rl);
        m.governorPlrBudgetDb.store (b.plrDb, rl);
        const float budgets[] = { b.presenceDb, b.harshDb, b.airDb };
        for (int k = 0; k < 3; ++k)
        {
            m.tonalLiftDb[static_cast<size_t> (k)].store (spanRunning ? tonalMeter.getLiftDb (TonalBalanceMeter::Presence + k) : kMinusInfDb, rl);
            m.tonalBudgetDb[static_cast<size_t> (k)].store (budgets[k], rl);
        }
    }
    governorMemory.publish (governor.getMemory()); // for a chain that takes over (adoptGovernorState)
    driveResidualDb.store (spanRunning ? driveSpan.getWeightedDb() : kMinusInfDb, rl);
    driveResidualFlatDb.store (spanRunning ? driveSpan.getPlainResidualDb() : kMinusInfDb, rl);
    harmonicsResidualDb.store (spanRunning ? lastHarmonicsResidualDb : kMinusInfDb, rl);
    governorHarmonicsScale.store (governor.getHarmonicsScale(), rl);
    governorTonalScale.store (governor.getTonalScale(), rl);
    for (int b = TonalBalanceMeter::Presence; b < TonalBalanceMeter::kNumBands; ++b)
        tonalLiftDb[static_cast<size_t> (b)].store (spanRunning ? tonalMeter.getLiftDb (b) : TonalBalanceMeter::kNoReading, rl);
    smoothnessCutDb.store (r.smoothCutDb, rl);
    m.smoothnessCutDb.store (r.smoothCutDb, rl);
    outputPlrDb.store (spanRunning ? plrMeter.getPlrDb() : PlrMeter::kNoReading, rl);
    m.autoLevelGainDb.store (autoLevel.getGainDb(), rl);
    m.autoDriveDb.store (autoDrive.getReductionDb(), rl);
    m.activeChannelMask.store (inputDetector.getActiveMask(), rl);
    m.inputFold.store (config.inputChannels > 2 && passMix.getTarget() > 0.5f ? 1 : 0, rl);
    m.surroundConfirmed.store (inputDetector.isSurroundConfirmed(), rl);
    // docs/11 E28a: the virtualiser's level-match make-up while it runs, and
    // the deepest fold-headroom gain of the block (either fold).
    m.virtMakeupDb.store (virtRan ? virtualizer.getMakeupDb() : 0.0f, rl);
    m.foldHeadroomDb.store (headroomBlockMinDb, rl);
    headroomBlockMinDb = 0.0f;
    m.safetyClipCount.store (maximizer.getSafetyClipCount(), rl);
}
} // namespace flub
