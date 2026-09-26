#include "flub/engine/Parameters.h"

#include <algorithm>
#include <cassert>
#include <unordered_map>

namespace flub::param
{
namespace
{
Info make (std::string key, std::string name, std::string group, Unit unit, float mn, float mx, float def, float skew = 0.0f)
{
    Info i;
    i.key = std::move (key);
    i.name = std::move (name);
    i.group = std::move (group);
    i.unit = unit;
    i.minValue = mn;
    i.maxValue = mx;
    i.defaultValue = def;
    i.skewCentre = skew;
    return i;
}

Info toggle (std::string key, std::string name, std::string group, bool def)
{
    return make (std::move (key), std::move (name), std::move (group), Unit::Toggle, 0.0f, 1.0f, def ? 1.0f : 0.0f);
}

Info choice (std::string key, std::string name, std::string group, std::vector<std::string> labels, int def)
{
    Info i = make (std::move (key), std::move (name), std::move (group), Unit::Choice, 0.0f,
                   static_cast<float> (labels.size() - 1), static_cast<float> (def));
    i.choices = std::move (labels);
    return i;
}

std::vector<Info> buildLayout()
{
    std::vector<Info> t (static_cast<size_t> (kNumParams));
    auto set = [&t] (int id, Info info) { t[static_cast<size_t> (id)] = std::move (info); };

    // ---- Global ----------------------------------------------------------
    set (InputGainDb, make ("input.gain", "Input Gain", "Global", Unit::Db, -24.0f, 24.0f, 0.0f));
    set (OutputGainDb, make ("output.gain", "Output Gain", "Global", Unit::Db, -24.0f, 12.0f, 0.0f));
    set (Mode, choice ("mode", "Mode", "Global", { "Music", "Gaming" }, 0));
    set (BoostIntensity, make ("boost", "Boost Intensity", "Global", Unit::Percent, 0.0f, 1.0f, 0.0f));
    set (Macro1, make ("macro.1", "Macro 1", "Macros", Unit::Percent, 0.0f, 1.0f, 0.0f));
    set (Macro2, make ("macro.2", "Macro 2", "Macros", Unit::Percent, 0.0f, 1.0f, 0.0f));
    set (Macro3, make ("macro.3", "Macro 3", "Macros", Unit::Percent, 0.0f, 1.0f, 0.0f));
    set (Macro4, make ("macro.4", "Macro 4", "Macros", Unit::Percent, 0.0f, 1.0f, 0.0f));
    set (Macro5, make ("macro.5", "Macro 5", "Macros", Unit::Percent, 0.0f, 1.0f, 0.0f));
    set (AutoLevelOn, toggle ("autolevel.on", "Auto Level", "Global", false));
    set (AutoLevelTargetLufs, make ("autolevel.target", "Auto Level Target", "Global", Unit::Lufs, -30.0f, -10.0f, -18.0f));
    set (LoudnessMatchBypass, toggle ("bypass.matched", "Loudness-Matched Bypass", "Global", true));
    set (BypassAll, toggle ("bypass", "Bypass All", "Global", false));
    {
        Info lp = choice ("latency.profile", "Latency Profile", "Global", { "Quality", "Balanced", "Low Latency" }, 1);
        lp.structural = true;
        set (LatencyProfile, std::move (lp));
    }

    // ---- Module enables ----------------------------------------------------
    set (GateOn, toggle ("gate.on", "Noise Gate", "Modules", false));
    set (EqOn, toggle ("eq.on", "Parametric EQ", "Modules", true));
    set (DynEqOn, toggle ("dyneq.on", "Dynamic EQ", "Modules", true));
    set (BassOn, toggle ("bass.on", "Bass Engine", "Modules", true));
    set (ClarityOn, toggle ("clarity.on", "Clarity", "Modules", true));
    set (SaturationOn, toggle ("sat.on", "Saturation", "Modules", false));
    set (SpatialOn, toggle ("spatial.on", "Stereo & Space", "Modules", true));
    set (VirtualizerOn, toggle ("virt.on", "Headphone Virtualizer", "Modules", true));
    set (CompressorOn, toggle ("comp.on", "Compressor", "Modules", false));
    set (MaximizerOn, toggle ("max.on", "Loudness Maximizer", "Modules", true));

    // ---- Spectral noise gate ------------------------------------------------
    set (GateThresholdDb, make ("gate.threshold", "Gate Threshold", "Noise Gate", Unit::Db, 0.0f, 20.0f, 6.0f));
    set (GateReductionDb, make ("gate.reduction", "Gate Reduction", "Noise Gate", Unit::Db, 0.0f, 40.0f, 12.0f));
    set (GateAttackMs, make ("gate.attack", "Gate Attack", "Noise Gate", Unit::Ms, 1.0f, 50.0f, 5.0f, 10.0f));
    set (GateReleaseMs, make ("gate.release", "Gate Release", "Noise Gate", Unit::Ms, 10.0f, 500.0f, 80.0f, 100.0f));
    set (GateFloorRise, make ("gate.floorRise", "Floor Adapt Rate", "Noise Gate", Unit::DbPerSec, 0.5f, 20.0f, 3.0f, 4.0f));
    set (GateFreeze, toggle ("gate.freeze", "Freeze Noise Profile", "Noise Gate", false));

    // ---- EQ global -----------------------------------------------------------
    set (EqOutputGainDb, make ("eq.output", "EQ Output", "EQ", Unit::Db, -24.0f, 12.0f, 0.0f));

    // ---- Bass engine -----------------------------------------------------------
    set (BassBoostDb, make ("bass.boost", "Bass Boost", "Bass", Unit::Db, 0.0f, 15.0f, 0.0f));
    set (BassBoostFreq, make ("bass.freq", "Boost Frequency", "Bass", Unit::Hz, 30.0f, 200.0f, 70.0f, 80.0f));
    set (BassProtectDb, make ("bass.protect", "Headroom Protect", "Bass", Unit::Db, -30.0f, 0.0f, -12.0f));
    set (BassHarmonics, make ("bass.harmonics", "Harmonic Bass", "Bass", Unit::Percent, 0.0f, 1.0f, 0.0f));
    set (BassHarmonicsCutoff, make ("bass.harmonicsCutoff", "Speaker Low Limit", "Bass", Unit::Hz, 40.0f, 250.0f, 120.0f, 100.0f));
    set (BassHarmonicsCharacter, make ("bass.character", "Harmonic Character", "Bass", Unit::Percent, 0.0f, 1.0f, 0.5f));
    set (BassReplaceFundamental, toggle ("bass.replaceFundamental", "Small Speaker Mode", "Bass", false));
    set (BassTighten, make ("bass.tighten", "Tighten", "Bass", Unit::Percent, 0.0f, 1.0f, 0.0f));
    set (BassMonoBelow, make ("bass.monoBelow", "Mono Bass Below", "Bass", Unit::Hz, 0.0f, 250.0f, 0.0f));
    set (BassSubsonic, make ("bass.subsonic", "Subsonic Filter", "Bass", Unit::Hz, 0.0f, 40.0f, 20.0f));

    // ---- Clarity ---------------------------------------------------------------
    set (ClarityAttackDb, make ("clarity.attack", "Transient Attack", "Clarity", Unit::Db, -12.0f, 12.0f, 0.0f));
    set (ClaritySustainDb, make ("clarity.sustain", "Transient Sustain", "Clarity", Unit::Db, -12.0f, 12.0f, 0.0f));
    set (ClarityPresence, make ("clarity.presence", "Presence", "Clarity", Unit::Percent, 0.0f, 1.0f, 0.0f));
    set (ClarityPresenceFreq, make ("clarity.presenceFreq", "Presence Frequency", "Clarity", Unit::Hz, 1000.0f, 6000.0f, 3200.0f, 2500.0f));
    set (ClarityAir, make ("clarity.air", "Air", "Clarity", Unit::Percent, 0.0f, 1.0f, 0.0f));
    set (ClarityDeMud, make ("clarity.demud", "De-Mud", "Clarity", Unit::Percent, 0.0f, 1.0f, 0.0f));

    // ---- Saturation --------------------------------------------------------------
    set (SatType, choice ("sat.type", "Saturation Type", "Saturation", { "Tape", "Tube", "Digital" }, 0));
    set (SatDriveDb, make ("sat.drive", "Drive", "Saturation", Unit::Db, 0.0f, 24.0f, 0.0f));
    set (SatMix, make ("sat.mix", "Mix", "Saturation", Unit::Percent, 0.0f, 1.0f, 1.0f));
    set (SatOutputDb, make ("sat.output", "Saturation Output", "Saturation", Unit::Db, -12.0f, 12.0f, 0.0f));

    // ---- Stereo & space -----------------------------------------------------------
    set (SpatialWidth, make ("spatial.width", "Width", "Stereo", Unit::Percent, 0.0f, 2.0f, 1.0f));
    set (SpatialWidthLowCut, make ("spatial.lowCut", "Width Low Cut", "Stereo", Unit::Hz, 60.0f, 500.0f, 180.0f, 180.0f));
    set (SpatialFocus, make ("spatial.focus", "Positional Focus", "Stereo", Unit::Percent, 0.0f, 1.0f, 0.0f));
    set (SpatialSpace, make ("spatial.space", "Space", "Stereo", Unit::Percent, 0.0f, 1.0f, 0.0f));
    set (SpatialCrossfeed, make ("spatial.crossfeed", "Headphone Crossfeed", "Stereo", Unit::Percent, 0.0f, 1.0f, 0.0f));
    set (SpatialMonoSafety, toggle ("spatial.monoSafety", "Mono Safety", "Stereo", true));
    set (SpatialMinCorrelation, make ("spatial.minCorrelation", "Min Correlation", "Stereo", Unit::None, -1.0f, 1.0f, 0.0f));

    // ---- Headphone virtualiser -------------------------------------------------------
    set (VirtFrontAngle, make ("virt.front", "Front Speaker Angle", "Virtualizer", Unit::Degrees, 22.0f, 45.0f, 30.0f));
    set (VirtSideAngle, make ("virt.side", "Side Speaker Angle", "Virtualizer", Unit::Degrees, 80.0f, 120.0f, 100.0f));
    set (VirtRearAngle, make ("virt.rear", "Rear Speaker Angle", "Virtualizer", Unit::Degrees, 120.0f, 165.0f, 145.0f));
    set (VirtHeadRadius, make ("virt.headRadius", "Head Radius", "Virtualizer", Unit::Millimetres, 70.0f, 105.0f, 87.5f));
    set (VirtRoom, make ("virt.room", "Room", "Virtualizer", Unit::Percent, 0.0f, 1.0f, 0.15f));
    set (VirtLfeGainDb, make ("virt.lfe", "LFE Level", "Virtualizer", Unit::Db, -20.0f, 10.0f, 0.0f));

    // ---- Compressor --------------------------------------------------------------------
    set (CompThresholdDb, make ("comp.threshold", "Threshold", "Compressor", Unit::Db, -60.0f, 0.0f, -18.0f));
    set (CompRatio, make ("comp.ratio", "Ratio", "Compressor", Unit::Ratio, 1.0f, 20.0f, 2.5f, 4.0f));
    set (CompKneeDb, make ("comp.knee", "Knee", "Compressor", Unit::Db, 0.0f, 24.0f, 6.0f));
    set (CompAttackMs, make ("comp.attack", "Attack", "Compressor", Unit::Ms, 0.1f, 200.0f, 10.0f, 10.0f));
    set (CompReleaseMs, make ("comp.release", "Release", "Compressor", Unit::Ms, 10.0f, 2000.0f, 120.0f, 150.0f));
    set (CompAutoRelease, toggle ("comp.autoRelease", "Auto Release", "Compressor", false));
    set (CompMakeupDb, make ("comp.makeup", "Makeup", "Compressor", Unit::Db, -12.0f, 24.0f, 0.0f));
    set (CompAutoMakeup, toggle ("comp.autoMakeup", "Auto Makeup", "Compressor", false));
    set (CompSidechainHp, make ("comp.scHp", "Sidechain High-Pass", "Compressor", Unit::Hz, 0.0f, 300.0f, 80.0f));
    set (CompMix, make ("comp.mix", "Mix", "Compressor", Unit::Percent, 0.0f, 1.0f, 1.0f));
    set (CompUpThresholdDb, make ("comp.upThreshold", "Upward Threshold", "Compressor", Unit::Db, -80.0f, -10.0f, -45.0f));
    set (CompUpRatio, make ("comp.upRatio", "Upward Ratio", "Compressor", Unit::Ratio, 1.0f, 10.0f, 2.0f, 3.0f));
    set (CompUpMaxGainDb, make ("comp.upMax", "Upward Max Gain", "Compressor", Unit::Db, 0.0f, 18.0f, 0.0f));
    set (CompUpFloorDb, make ("comp.upFloor", "Upward Floor", "Compressor", Unit::Db, -100.0f, -40.0f, -75.0f));

    // ---- Loudness maximizer ---------------------------------------------------------------
    set (MaxDriveDb, make ("max.drive", "Drive", "Maximizer", Unit::Db, 0.0f, 24.0f, 0.0f));
    set (MaxCeilingDb, make ("max.ceiling", "Ceiling", "Maximizer", Unit::Db, -12.0f, 0.0f, -1.0f));
    set (MaxClipAmount, make ("max.clip", "Clipper Share", "Maximizer", Unit::Percent, 0.0f, 1.0f, 0.5f));
    set (MaxClipKnee, make ("max.clipKnee", "Clipper Softness", "Maximizer", Unit::Percent, 0.0f, 1.0f, 0.5f));
    set (MaxGlue, make ("max.glue", "Multiband Glue", "Maximizer", Unit::Percent, 0.0f, 1.0f, 0.0f));
    set (MaxReleaseMs, make ("max.release", "Release", "Maximizer", Unit::Ms, 5.0f, 1000.0f, 60.0f, 80.0f));
    set (MaxAutoRelease, toggle ("max.autoRelease", "Auto Release", "Maximizer", true));
    set (MaxAutoDrive, toggle ("max.autoDrive", "Loudness Target", "Maximizer", false));
    set (MaxTargetLufs, make ("max.target", "Target Loudness", "Maximizer", Unit::Lufs, -24.0f, -6.0f, -14.0f));

    // ---- Parametric EQ bands (ISO octave centres, all bells at 0 dB) ------------------------
    static const float eqFreqs[kEqBands] = { 32.0f, 64.0f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f };
    for (int b = 0; b < kEqBands; ++b)
    {
        const std::string k = "eq." + std::to_string (b) + ".";
        const std::string n = "Band " + std::to_string (b + 1) + " ";
        set (eq (b, EqFieldOn), toggle (k + "on", n + "On", "EQ", true));
        set (eq (b, EqFieldType), choice (k + "type", n + "Type", "EQ", { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Band Pass" }, 0));
        set (eq (b, EqFieldFreq), make (k + "freq", n + "Frequency", "EQ", Unit::Hz, 20.0f, 20000.0f, eqFreqs[b], 1000.0f));
        set (eq (b, EqFieldGain), make (k + "gain", n + "Gain", "EQ", Unit::Db, -24.0f, 24.0f, 0.0f));
        set (eq (b, EqFieldQ), make (k + "q", n + "Q", "EQ", Unit::None, 0.1f, 18.0f, 1.0f, 1.0f));
        set (eq (b, EqFieldSlope), choice (k + "slope", n + "Slope", "EQ", { "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct" }, 1));
    }

    // ---- Dynamic EQ user bands --------------------------------------------------------------
    struct DynDefault
    {
        float freq;
        int mode, shape;
    };
    static const DynDefault dynDefaults[kDynEqBands] = { { 90.0f, 0, 0 }, { 350.0f, 0, 0 }, { 3500.0f, 0, 0 }, { 10000.0f, 1, 2 } };
    for (int b = 0; b < kDynEqBands; ++b)
    {
        const std::string k = "dyneq." + std::to_string (b) + ".";
        const std::string n = "Dyn " + std::to_string (b + 1) + " ";
        set (dyn (b, DynFieldOn), toggle (k + "on", n + "On", "Dynamic EQ", false));
        set (dyn (b, DynFieldMode), choice (k + "mode", n + "Mode", "Dynamic EQ", { "Cut Above", "Boost Below", "Boost Above", "Cut Below" }, dynDefaults[b].mode));
        set (dyn (b, DynFieldShape), choice (k + "shape", n + "Shape", "Dynamic EQ", { "Bell", "Low Shelf", "High Shelf" }, dynDefaults[b].shape));
        set (dyn (b, DynFieldFreq), make (k + "freq", n + "Frequency", "Dynamic EQ", Unit::Hz, 20.0f, 20000.0f, dynDefaults[b].freq, 1000.0f));
        set (dyn (b, DynFieldQ), make (k + "q", n + "Q", "Dynamic EQ", Unit::None, 0.1f, 10.0f, 1.0f, 1.0f));
        set (dyn (b, DynFieldThreshold), make (k + "threshold", n + "Threshold", "Dynamic EQ", Unit::Db, -80.0f, 0.0f, -24.0f));
        set (dyn (b, DynFieldRatio), make (k + "ratio", n + "Ratio", "Dynamic EQ", Unit::Ratio, 1.0f, 20.0f, 2.0f, 4.0f));
        set (dyn (b, DynFieldRange), make (k + "range", n + "Range", "Dynamic EQ", Unit::Db, 0.0f, 24.0f, 6.0f));
        set (dyn (b, DynFieldStaticGain), make (k + "staticGain", n + "Static Gain", "Dynamic EQ", Unit::Db, -12.0f, 12.0f, 0.0f));
        set (dyn (b, DynFieldAttack), make (k + "attack", n + "Attack", "Dynamic EQ", Unit::Ms, 0.1f, 200.0f, 5.0f, 10.0f));
        set (dyn (b, DynFieldRelease), make (k + "release", n + "Release", "Dynamic EQ", Unit::Ms, 5.0f, 2000.0f, 80.0f, 150.0f));
        set (dyn (b, DynFieldNoiseFloor), make (k + "noiseFloor", n + "Noise Floor", "Dynamic EQ", Unit::Db, -100.0f, -30.0f, -70.0f));
    }

#ifndef NDEBUG
    for (const auto& i : t)
        assert (! i.key.empty() && i.defaultValue >= i.minValue && i.defaultValue <= i.maxValue);
#endif
    return t;
}
} // namespace

const std::vector<Info>& layout()
{
    static const std::vector<Info> table = buildLayout();
    return table;
}

int findByKey (const std::string& key)
{
    static const std::unordered_map<std::string, int> index = [] {
        std::unordered_map<std::string, int> m;
        const auto& t = layout();
        for (int i = 0; i < static_cast<int> (t.size()); ++i)
            m.emplace (t[static_cast<size_t> (i)].key, i);
        return m;
    }();
    const auto it = index.find (key);
    return it == index.end() ? -1 : it->second;
}

// ---------------------------------------------------------------------------
ParameterStore::ParameterStore() : values (std::make_unique<std::atomic<float>[]> (2 * static_cast<size_t> (kNumParams)))
{
    resetToDefaults (Bank::A);
    resetToDefaults (Bank::B);
}

float ParameterStore::get (int id) const noexcept
{
    return get (getActiveBank(), id);
}

void ParameterStore::set (int id, float value) noexcept
{
    set (getActiveBank(), id, value);
}

float ParameterStore::get (Bank b, int id) const noexcept
{
    assert (id >= 0 && id < kNumParams);
    return values[static_cast<size_t> (static_cast<int> (b) * kNumParams + id)].load (std::memory_order_relaxed);
}

void ParameterStore::set (Bank b, int id, float value) noexcept
{
    assert (id >= 0 && id < kNumParams);
    const float v = layout()[static_cast<size_t> (id)].clamp (value);
    values[static_cast<size_t> (static_cast<int> (b) * kNumParams + id)].store (v, std::memory_order_relaxed);
    changeCounter.fetch_add (1, std::memory_order_release);
}

void ParameterStore::setActiveBank (Bank b) noexcept
{
    activeBank.store (static_cast<int> (b), std::memory_order_release);
    changeCounter.fetch_add (1, std::memory_order_release);
}

Bank ParameterStore::getActiveBank() const noexcept
{
    return static_cast<Bank> (activeBank.load (std::memory_order_acquire));
}

void ParameterStore::copyBank (Bank from, Bank to) noexcept
{
    for (int i = 0; i < kNumParams; ++i)
        values[static_cast<size_t> (static_cast<int> (to) * kNumParams + i)].store (get (from, i), std::memory_order_relaxed);
    changeCounter.fetch_add (1, std::memory_order_release);
}

void ParameterStore::resetToDefaults (Bank b) noexcept
{
    const auto& t = layout();
    for (int i = 0; i < kNumParams; ++i)
        values[static_cast<size_t> (static_cast<int> (b) * kNumParams + i)].store (t[static_cast<size_t> (i)].defaultValue, std::memory_order_relaxed);
    changeCounter.fetch_add (1, std::memory_order_release);
}

void ParameterStore::snapshot (float* dest) const noexcept
{
    const int bank = activeBank.load (std::memory_order_acquire);
    for (int i = 0; i < kNumParams; ++i)
        dest[i] = values[static_cast<size_t> (bank * kNumParams + i)].load (std::memory_order_relaxed);
}
} // namespace flub::param
