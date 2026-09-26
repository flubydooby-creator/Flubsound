#include "flub/engine/ProcessingChain.h"

#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>

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
void configureModeBands (DynamicEq& dyn, ModeValue mode, const float* e) noexcept
{
    if (mode == ModeValue::Gaming)
    {
        const float footsteps = e[Macro1];
        const float voice = e[Macro5];
        // Quiet high-frequency detail (steps, reloads, cloth) is lifted by upward
        // compression; loud events above threshold are untouched.
        dyn.setBand (4, modeBand (DynEqMode::BoostBelow, EqBandType::Bell, 3200.0f, 0.9f, -42.0f, 3.0f, 7.0f * footsteps, 3.0f, 120.0f, -75.0f));
        // Footstep "body" (heel impact) for heavier footwear / surfaces.
        dyn.setBand (5, modeBand (DynEqMode::BoostBelow, EqBandType::Bell, 260.0f, 1.2f, -45.0f, 2.5f, 3.0f * footsteps, 5.0f, 150.0f, -75.0f));
        // Anti-masking: very loud low end (explosions, vehicles) is tamed so it
        // does not bury the steps that follow; normal bass is unaffected.
        dyn.setBand (6, modeBand (DynEqMode::CutAbove, EqBandType::LowShelf, 90.0f, 0.7f, -22.0f, 3.0f, 6.0f * footsteps, 10.0f, 250.0f, -80.0f));
        // Voice comms / dialogue / score intelligibility.
        dyn.setBand (7, modeBand (DynEqMode::BoostBelow, EqBandType::Bell, 2000.0f, 0.7f, -36.0f, 2.0f, 4.0f * voice, 5.0f, 150.0f, -70.0f));
    }
    else
    {
        const float clarity = e[Macro3];
        const float boost = e[BoostIntensity];
        // Presence/air boosts are paired with a dynamic de-harsh band.
        dyn.setBand (4, modeBand (DynEqMode::CutAbove, EqBandType::Bell, 3500.0f, 1.2f, -22.0f, 3.0f, 3.0f * clarity, 2.0f, 80.0f, -80.0f));
        dyn.setBand (5, modeBand (DynEqMode::BoostBelow, EqBandType::HighShelf, 12000.0f, 0.7f, -45.0f, 2.0f, 3.0f * clarity, 10.0f, 200.0f, -80.0f));
        // Bass boost is paired with a dynamic de-boom band.
        dyn.setBand (6, modeBand (DynEqMode::CutAbove, EqBandType::Bell, 120.0f, 1.0f, -14.0f, 2.5f, 4.0f * boost, 10.0f, 150.0f, -80.0f));
        dyn.setBand (7, modeBand (DynEqMode::CutAbove, EqBandType::Bell, 1000.0f, 1.0f, 0.0f, 1.0f, 0.0f, 5.0f, 80.0f, -80.0f));
    }
}
} // namespace

ProcessingChain::ProcessingChain (ParameterStore& s) : store (s)
{
    base.assign (static_cast<size_t> (kNumParams), 0.0f);
    effective.assign (static_cast<size_t> (kNumParams), 0.0f);
}

void ProcessingChain::prepare (const ChainConfig& cfg)
{
    config = cfg;
    config.inputChannels = std::clamp (config.inputChannels, 2, kMaxChannels);
    const double sr = config.sampleRate;
    const int maxB = config.maxBlockSize;

    store.snapshot (base.data());
    MacroMap::apply (base.data(), effective.data(), 1.0f);
    const float* e = effective.data();

    profileAtPrepare = idx (e, LatencyProfile);
    switch (static_cast<LatencyProfileValue> (profileAtPrepare))
    {
        case LatencyProfileValue::Quality:
            gateInChain = true;
            gate.setFftSize (1024);
            saturator.setOversampling (2, Oversampler::Quality::High);
            compressor.setLookaheadMs (3.0f);
            maximizer.setClipOversampling (4, Oversampler::Quality::High);
            maximizer.setLookaheadMs (2.0f);
            maximizer.setTruePeakDetection (true);
            break;
        case LatencyProfileValue::LowLatency:
            gateInChain = false;
            saturator.setOversampling (2, Oversampler::Quality::Low);
            compressor.setLookaheadMs (0.5f);
            maximizer.setClipOversampling (2, Oversampler::Quality::Low);
            maximizer.setLookaheadMs (0.5f);
            maximizer.setTruePeakDetection (true);
            break;
        case LatencyProfileValue::Balanced:
        default:
            gateInChain = false;
            saturator.setOversampling (2, Oversampler::Quality::Low);
            compressor.setLookaheadMs (1.0f);
            maximizer.setClipOversampling (4, Oversampler::Quality::High);
            maximizer.setLookaheadMs (1.5f);
            maximizer.setTruePeakDetection (true);
            break;
    }

    virtualizer.prepare ({ sr, maxB, config.inputChannels });

    const ProcessSpec stereo { sr, maxB, 2 };
    if (gateInChain)
        slots[SGate].prepare (gate, stereo, 20.0f, on (e, GateOn));
    slots[SEq].prepare (paramEq, stereo, 20.0f, on (e, EqOn));
    slots[SDynEq].prepare (dynEq, stereo, 20.0f, on (e, DynEqOn));
    slots[SBass].prepare (bass, stereo, 20.0f, on (e, BassOn));
    slots[SClarity].prepare (clarity, stereo, 20.0f, on (e, ClarityOn));
    slots[SSat].prepare (saturator, stereo, 20.0f, on (e, SaturationOn));
    slots[SSpatial].prepare (spatial, stereo, 20.0f, on (e, SpatialOn));
    slots[SComp].prepare (compressor, stereo, 20.0f, on (e, CompressorOn));
    slots[SMax].prepare (maximizer, stereo, 20.0f, on (e, MaximizerOn));

    totalLatency = 0;
    for (int s = 0; s < kNumSlots; ++s)
        if (s != SGate || gateInChain)
            totalLatency += slots[static_cast<size_t> (s)].latencySamples();

    dryBuffer.setSize (2, maxB);
    dryDelay.prepare (2, totalLatency);

    inputGain.reset (sr, 20.0f, dbToGain (e[InputGainDb]));
    outputGain.reset (sr, 20.0f, dbToGain (e[OutputGainDb]));
    bypassMix.reset (sr, 30.0f, on (e, BypassAll) ? 1.0f : 0.0f);
    dryMatchGain.reset (sr, 50.0f, 1.0f);

    autoLevel.prepare (sr, config.inputChannels);
    autoDrive.prepare (sr, 2);
    governor.prepare (sr);
    loudnessMatch.prepare (sr, 2);

    inLevel.prepare (sr, 2);
    outLevel.prepare (sr, 2);
    outTruePeak.prepare (2);
    outLoudness.prepare (sr, 2);
    inLoudness.prepare (sr, 2, 3000.0f);
    tapScratch.assign (static_cast<size_t> (maxB), 0.0f);
    dryPeakRelease = onePoleCoeff (2000.0f, sr);

    meterBus.latencyMs.store (static_cast<float> (1000.0 * totalLatency / sr), std::memory_order_relaxed);
    reset();
}

void ProcessingChain::reset() noexcept
{
    for (int s = 0; s < kNumSlots; ++s)
        if (s != SGate || gateInChain)
            slots[static_cast<size_t> (s)].reset();
    virtualizer.reset();
    dryDelay.reset();
    autoLevel.reset();
    autoDrive.reset();
    governor.reset();
    loudnessMatch.reset();
    inLevel.reset();
    outLevel.reset();
    outTruePeak.reset();
    outLoudness.reset();
    inLoudness.reset();
    dryPeakHold = 0.0f;
}

bool ProcessingChain::needsReprepare() const noexcept
{
    return static_cast<int> (std::lround (store.get (LatencyProfile))) != profileAtPrepare;
}

void ProcessingChain::applyParameters() noexcept
{
    MacroMap::apply (base.data(), effective.data(), governor.getScale());
    float* e = effective.data();
    const auto mode = static_cast<ModeValue> (idx (e, Mode));
    const bool binaural = config.inputChannels > 2 && on (e, VirtualizerOn);

    inputGain.setTarget (dbToGain (e[InputGainDb]));
    outputGain.setTarget (dbToGain (e[OutputGainDb]));
    autoLevel.setEnabled (on (e, AutoLevelOn));
    autoLevel.setTargetLufs (e[AutoLevelTargetLufs]);
    bypassMix.setTarget (on (e, BypassAll) ? 1.0f : 0.0f);

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
        slots[SGate].setActive (on (e, GateOn));
    }

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
    slots[SEq].setActive (on (e, EqOn));

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
    configureModeBands (dynEq, mode, e);
    slots[SDynEq].setActive (on (e, DynEqOn));

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
    bass.setParams (bp);
    slots[SBass].setActive (on (e, BassOn));

    // ---- Clarity ----
    ClarityParams cp;
    cp.attackDb = e[ClarityAttackDb];
    cp.sustainDb = e[ClaritySustainDb];
    cp.presence = e[ClarityPresence];
    cp.presenceFrequency = e[ClarityPresenceFreq];
    // The air exciter is alias-free only because its <= 3rd-order products of
    // <= 7 kHz content stay below 21 kHz. Headsets running at low rates (USB
    // 32 kHz modes, Bluetooth hands-free at 16 / 8 kHz) would fold them back,
    // so it is disabled below 40 kHz.
    cp.air = config.sampleRate >= 40000.0 ? e[ClarityAir] : 0.0f;
    cp.deMud = e[ClarityDeMud];
    clarity.setParams (cp);
    slots[SClarity].setActive (on (e, ClarityOn));

    // ---- Saturation ----
    SaturatorParams sp;
    sp.type = static_cast<SaturationType> (std::clamp (idx (e, SatType), 0, 2));
    sp.driveDb = e[SatDriveDb];
    sp.mix = e[SatMix];
    sp.outputDb = e[SatOutputDb];
    saturator.setParams (sp);
    slots[SSat].setActive (on (e, SaturationOn));

    // ---- Stereo & space (mode / binaural policy) ----
    SpatializerParams wp;
    wp.width = e[SpatialWidth];
    wp.widthLowCutHz = e[SpatialWidthLowCut];
    wp.positionalFocus = e[SpatialFocus];
    wp.space = e[SpatialSpace];
    wp.crossfeed = e[SpatialCrossfeed];
    wp.autoMonoSafety = on (e, SpatialMonoSafety);
    wp.minCorrelation = e[SpatialMinCorrelation];
    if (mode == ModeValue::Gaming)
        wp.crossfeed = 0.0f; // crossfeed blurs lateral cues: never in gaming
    if (binaural)
    {
        // Binaural output already carries exact interaural cues; widening,
        // decorrelation or crossfeed would corrupt them. Focus (an ILD
        // emphasis) is still allowed.
        wp.width = 1.0f;
        wp.space = 0.0f;
        wp.crossfeed = 0.0f;
    }
    spatial.setParams (wp);
    slots[SSpatial].setActive (on (e, SpatialOn));

    // ---- Virtualiser ----
    VirtualizerParams vp;
    vp.layout = layoutFor (config.inputChannels);
    vp.frontAngleDeg = e[VirtFrontAngle];
    vp.sideAngleDeg = e[VirtSideAngle];
    vp.rearAngleDeg = e[VirtRearAngle];
    vp.headRadiusMm = e[VirtHeadRadius];
    vp.roomAmount = e[VirtRoom];
    vp.lfeGainDb = e[VirtLfeGainDb];
    virtualizer.setParams (vp);

    // ---- Compressor ----
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
    compressor.setParams (kp);
    slots[SComp].setActive (on (e, CompressorOn));

    // ---- Maximizer (AutoDrive may only reduce the requested drive) ----
    MaximizerParams mp;
    mp.driveDb = std::max (0.0f, e[MaxDriveDb] + autoDrive.getReductionDb());
    mp.ceilingDb = e[MaxCeilingDb];
    mp.clipAmount = e[MaxClipAmount];
    mp.clipKnee = e[MaxClipKnee];
    mp.glue = e[MaxGlue];
    mp.releaseMs = e[MaxReleaseMs];
    mp.autoRelease = on (e, MaxAutoRelease);
    maximizer.setParams (mp);
    slots[SMax].setActive (on (e, MaximizerOn));
}

void ProcessingChain::downmixToStereo (const AudioBlock& io) noexcept
{
    // ITU-R BS.775 downmix (LFE dropped) with -3 dB overall to limit overload.
    // 5.1: FL FR FC LFE SL SR | 7.1: FL FR FC LFE BL BR SL SR
    constexpr float k = 0.70710678f;
    const int nch = io.numChannels;
    float* l = io.channel (0);
    float* r = io.channel (1);
    const float* c = nch > 2 ? io.channel (2) : nullptr;
    for (int i = 0; i < io.numSamples; ++i)
    {
        float lo = l[i], ro = r[i];
        if (c != nullptr)
        {
            lo += k * c[i];
            ro += k * c[i];
        }
        for (int s = 4; s + 1 < nch; s += 2)
        {
            lo += k * io.channel (s)[i];
            ro += k * io.channel (s + 1)[i];
        }
        l[i] = k * lo;
        r[i] = k * ro;
    }
}

void ProcessingChain::process (const AudioBlock& io) noexcept
{
    const int n = io.numSamples;
    if (n <= 0)
        return;

    // A single NaN/Inf from a misbehaving driver or upstream plug-in would latch
    // forever in IIR state: drop the block and restart the chain cleanly.
    float checksum = 0.0f;
    for (int c = 0; c < io.numChannels; ++c)
        for (int i = 0; i < n; ++i)
            checksum += io.channel (c)[i] * 0.0f;
    if (! std::isfinite (checksum))
    {
        io.clear();
        reset();
        return;
    }

    store.snapshot (base.data());
    if (meterBus.resetLoudnessRequest.exchange (false, std::memory_order_acq_rel))
    {
        outLoudness.resetIntegrated();
        outTruePeak.reset();
    }
    applyParameters();
    const float* e = effective.data();

    // ---- 1. Input stage (all input channels) ----
    const AudioBlock in = io.firstChannels (config.inputChannels);
    const float g0 = inputGain.getCurrent();
    in.applyGainRamp (g0, inputGain.skip (n));
    autoLevel.process (in);

    // ---- 2. Fold to stereo ----
    if (config.inputChannels > 2)
    {
        if (on (e, VirtualizerOn))
            virtualizer.process (in);
        else
            downmixToStereo (in);
    }
    const AudioBlock st = io.firstChannels (2);

    // ---- 3. Dry reference for global bypass / A-B ----
    const AudioBlock dry = dryBuffer.block (2, n);
    dry.copyFrom (st);
    loudnessMatch.measureDry (dry);
    dryDelay.process (dry);

    inLevel.process (st);
    inLoudness.process (st);
    for (int i = 0; i < n; ++i)
        tapScratch[static_cast<size_t> (i)] = 0.5f * (st.channel (0)[i] + st.channel (1)[i]);
    analyzerTaps.pre.push (tapScratch.data(), static_cast<size_t> (n));

    // ---- 4. Module slots ----
    for (int s = 0; s < kNumSlots; ++s)
        if (s != SGate || gateInChain)
            slots[static_cast<size_t> (s)].process (st);

    // ---- 5. Output gain ----
    const float o0 = outputGain.getCurrent();
    st.applyGainRamp (o0, outputGain.skip (n));

    // ---- 6. Control loops for the next block ----
    const bool maxActive = ! slots[SMax].isFullyBypassed();
    governor.update (maxActive ? maximizer.getGainReductionDb() : 0.0f, maxActive ? maximizer.getClipEnergyRatioDb() : kMinusInfDb, n);
    autoDrive.update (st, e[MaxTargetLufs], on (e, MaxAutoDrive));
    loudnessMatch.measureWet (st);

    // ---- 7. Global bypass (latency-aligned, optionally loudness matched) ----
    float dryPeak = 0.0f;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i)
            dryPeak = std::max (dryPeak, std::abs (dry.channel (c)[i]));
    // Instant attack, ~2 s release; the per-sample coefficient is raised to the
    // block length because this runs once per block.
    const float blockRelease = std::pow (dryPeakRelease, static_cast<float> (n));
    dryPeakHold = dryPeak > dryPeakHold ? dryPeak : dryPeak + blockRelease * (dryPeakHold - dryPeak);

    float matchDb = on (e, LoudnessMatchBypass) ? loudnessMatch.getDryGainDb (n) : 0.0f;
    if (matchDb > 0.0f && dryPeakHold > 0.0f)
        matchDb = std::min (matchDb, std::max (0.0f, e[MaxCeilingDb] - gainToDb (dryPeakHold))); // never push the reference into clipping
    dryMatchGain.setTarget (dbToGain (matchDb));

    if (bypassMix.getCurrent() > 0.0f || bypassMix.isSmoothing())
    {
        for (int i = 0; i < n; ++i)
        {
            const float b = bypassMix.next();
            const float dg = dryMatchGain.next();
            for (int c = 0; c < 2; ++c)
            {
                float* w = st.channel (c);
                w[i] += b * (dg * dry.channel (c)[i] - w[i]);
            }
        }
    }
    else
    {
        dryMatchGain.skip (n);
    }

    // ---- 8. Output meters / analyser ----
    publishMeters (st, n);
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
    m.compGainReductionDb.store (compressor.getGainReductionDb(), rl);
    m.compUpwardGainDb.store (compressor.getUpwardGainDb(), rl);
    m.maxGainReductionDb.store (maximizer.getGainReductionDb(), rl);
    m.glueGainReductionDb.store (maximizer.getGlueReductionDb(), rl);
    m.clipEnergyRatioDb.store (maximizer.getClipEnergyRatioDb(), rl);
    m.bassProtectionDb.store (bass.getProtectionDb(), rl);
    for (int b = 0; b < DynamicEq::kMaxBands && b < MeterBus::kMaxDynBands; ++b)
        m.dynEqGainDb[static_cast<size_t> (b)].store (dynEq.getBandGainDb (b), rl);
    m.governorScale.store (governor.getScale(), rl);
    m.autoLevelGainDb.store (autoLevel.getGainDb(), rl);
    m.autoDriveDb.store (autoDrive.getReductionDb(), rl);
}
} // namespace flub
