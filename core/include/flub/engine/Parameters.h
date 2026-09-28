// Flubsound Pro - parameter layout + lock-free parameter store.
//
// * Every user-facing value is a float in a flat, fixed-size table indexed by
//   Id. IDs are an in-memory index only: saved data (presets, plug-in state,
//   host automation, IPC) uses the string keys, so IDs may shift between
//   versions when parameters are inserted. The keys are the stable contract:
//   never rename or reuse one.
// * The store holds TWO banks (A and B) for A/B comparison. The GUI thread
//   writes with relaxed atomic stores; the audio thread reads the active bank
//   once per block. Continuous values glide inside the modules (smoothing),
//   discrete ones are crossfaded, so switching banks is click-free.
// * "Structural" parameters (latency profile) cannot be applied on the audio
//   thread: ProcessingChain::needsReprepare() reports them so the engine host
//   re-prepares the chain on a background thread with a short fade.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace flub::param
{
enum class Unit : uint8_t
{
    None,
    Db,
    Hz,
    Ms,
    Percent, // stored 0..1, displayed 0..100 %
    Ratio,
    Lufs,
    Degrees,
    Millimetres,
    DbPerSec,
    Choice,
    Toggle
};

struct Info
{
    std::string key;   // stable preset key, e.g. "eq.3.freq"
    std::string name;  // display name, e.g. "Band 4 Frequency"
    std::string group; // "Global", "EQ", "Bass", ... (generic editors / docs)
    Unit unit = Unit::None;
    float minValue = 0.0f, maxValue = 1.0f, defaultValue = 0.0f;
    float skewCentre = 0.0f;          // value at slider mid-point (0 = linear)
    std::vector<std::string> choices; // Unit::Choice labels
    bool structural = false;          // needs chain re-prepare (latency changes)

    /** Plug-in parameter version hint (JUCE ParameterID version, which orders
        AU parameters): the parameter-layout version that introduced this
        parameter. Everything in the first release is 1. Rule: parameters
        added later get the next version (highest in use + 1, shared by all
        parameters added in the same release); a shipped value never changes. */
    int sinceVersion = 1;

    /** Into [minValue, maxValue]; NaN (which no comparison catches) maps to the default. */
    float clamp (float v) const noexcept { return v != v ? defaultValue : (v < minValue ? minValue : (v > maxValue ? maxValue : v)); }
};

// -------------------------------------------------------------------------
// IDs (index into layout(); not persisted - saved data uses Info::key)
// -------------------------------------------------------------------------
enum Id : int
{
    // Global
    InputGainDb = 0,
    OutputGainDb,
    Mode,              // Choice: Music, Gaming
    BoostIntensity,    // 0..1
    Macro1,            // Music: Punch     | Gaming: Footsteps
    Macro2,            // Music: Width     | Gaming: Positional
    Macro3,            // Music: Clarity   | Gaming: Impact
    Macro4,            // Music: Loudness  | Gaming: Detail
    Macro5,            // Music: Warmth    | Gaming: Voice & Score
    AutoLevelOn,       // LUFS input levelling
    AutoLevelTargetLufs,
    LoudnessMatchBypass, // global bypass is loudness matched
    BypassAll,
    LatencyProfile,    // Choice: Quality, Balanced, Low Latency (structural)
    AutoPreampOn,      // Toggle: automatic preamp from the chain's predicted static boost (docs/11 E11)
    AutoPreampAllowanceDb, // boost the automatic preamp leaves in (dB)

    // Module enables (bypass with click-free, latency-compensated crossfade)
    GateOn,
    EqOn,
    DynEqOn,
    BassOn,
    ClarityOn,
    SaturationOn,
    SpatialOn,
    VirtualizerOn,
    CompressorOn,
    MaximizerOn,

    // Spectral noise gate
    GateThresholdDb,
    GateReductionDb,
    GateAttackMs,
    GateReleaseMs,
    GateFloorRise,
    GateFreeze,

    // Parametric EQ (global part)
    EqOutputGainDb,

    // Bass engine
    BassBoostDb,
    BassBoostFreq,
    BassProtectDb,
    BassHarmonics,
    BassHarmonicsCutoff,
    BassHarmonicsCharacter,
    BassReplaceFundamental,
    BassTighten,
    BassMonoBelow,
    BassSubsonic,

    // Clarity
    ClarityAttackDb,
    ClaritySustainDb,
    ClarityPresence,
    ClarityPresenceFreq,
    ClarityAir,
    ClarityDeMud,

    // Saturation
    SatType, // Choice: Tape, Tube, Digital
    SatDriveDb,
    SatMix,
    SatOutputDb,

    // Stereo & space
    SpatialWidth,
    SpatialWidthLowCut,
    SpatialFocus,
    SpatialSpace,
    SpatialCrossfeed,
    SpatialMonoSafety,
    SpatialMinCorrelation,

    // Headphone virtualiser (active only for 5.1 / 7.1 input)
    VirtFrontAngle,
    VirtSideAngle,
    VirtRearAngle,
    VirtHeadRadius,
    VirtRoom,
    VirtLfeGainDb,     // LFE level re one main channel, in every fold (docs/11 E01)
    VirtLfeFold,       // Toggle: fold the LFE into the stereo output at all (off = v1 BS.775 downmix)
    VirtInputMode,     // Choice: Auto, Force Surround, Force Stereo (5.1 / 7.1 input fold, docs/11 E27)
    VirtOwnHrtf,       // Toggle: the game renders its own HRTF (stereo fold, no virtualiser / width / focus / crossfeed / space)

    // Compressor
    CompThresholdDb,
    CompRatio,
    CompKneeDb,
    CompAttackMs,
    CompReleaseMs,
    CompAutoRelease,
    CompMakeupDb,
    CompAutoMakeup,
    CompSidechainHp,
    CompMix,
    CompUpThresholdDb,
    CompUpRatio,
    CompUpMaxGainDb,
    CompUpFloorDb,

    // Loudness maximizer
    MaxDriveDb,
    MaxCeilingDb,
    MaxClipAmount,
    MaxClipKnee,
    MaxGlue,
    MaxReleaseMs,
    MaxAutoRelease,
    MaxAutoDrive,     // drive becomes a maximum; a slow loop targets MaxTargetLufs
    MaxTargetLufs,
    MaxClipCrestDb,   // clipper crest gate: threshold >= this far over the short-term RMS (0 = off, docs/11 E05)
    MaxClipMaxDb,     // clipper depth cap: no sample loses more than this (24 = uncapped)
    MaxStyle,         // Choice: Custom, Transparent, Punchy, Aggressive, Safe (maxStyleValues)

    kNumScalarParams
};

// Banded parameters follow the scalar block.
inline constexpr int kEqBands = 10;
inline constexpr int kDynEqBands = 4;

enum EqField : int { EqFieldOn = 0, EqFieldType, EqFieldFreq, EqFieldGain, EqFieldQ, EqFieldSlope, kEqFields };
enum DynField : int
{
    DynFieldOn = 0,
    DynFieldMode,
    DynFieldShape,
    DynFieldFreq,
    DynFieldQ,
    DynFieldThreshold,
    DynFieldRatio,
    DynFieldRange,
    DynFieldStaticGain,
    DynFieldAttack,
    DynFieldRelease,
    DynFieldNoiseFloor,
    kDynFields
};

inline constexpr int kEqBase = kNumScalarParams;
inline constexpr int kDynBase = kEqBase + kEqBands * kEqFields;
inline constexpr int kNumParams = kDynBase + kDynEqBands * kDynFields;

constexpr int eq (int band, EqField f) noexcept { return kEqBase + band * kEqFields + f; }
constexpr int dyn (int band, DynField f) noexcept { return kDynBase + band * kDynFields + f; }

enum class ModeValue : int { Music = 0, Gaming = 1 };
enum class LatencyProfileValue : int { Quality = 0, Balanced = 1, LowLatency = 2 };
enum class InputModeValue : int { Auto = 0, ForceSurround = 1, ForceStereo = 2 }; // VirtInputMode
enum class MaxStyleValue : int { Custom = 0, Transparent, Punchy, Aggressive, Safe };  // MaxStyle

/** A named maximizer style (docs/11 E05 step 4): the values it gives the six
    maximizer controls it owns while it is selected. Custom owns none (the
    controls' own values apply, as in every preset saved before styles). */
struct MaxStyleValues
{
    float clipAmount, clipKnee, clipCrestDb, clipMaxDb, releaseMs;
    bool autoRelease;
};
/** nullptr for Custom (or an out-of-range value). */
const MaxStyleValues* maxStyleValues (MaxStyleValue style) noexcept;

/** The full, ordered table (index == Id). Built once, immutable afterwards. */
const std::vector<Info>& layout();

/** Key -> id lookup (linear scan over a sorted index; non-RT). -1 if unknown. */
int findByKey (const std::string& key);

// -------------------------------------------------------------------------
// Lock-free store
// -------------------------------------------------------------------------
enum class Bank : int { A = 0, B = 1 };

class ParameterStore
{
public:
    ParameterStore();

    /** Active bank, relaxed atomics: RT-safe on any thread. */
    float get (int id) const noexcept;
    void set (int id, float value) noexcept; // clamps, bumps version; NaN is ignored (keeps the value)

    float get (Bank b, int id) const noexcept;
    void set (Bank b, int id, float value) noexcept;

    void setActiveBank (Bank b) noexcept;
    Bank getActiveBank() const noexcept;
    void copyBank (Bank from, Bank to) noexcept;
    void resetToDefaults (Bank b) noexcept;

    /** Increments on every change; GUIs poll it to refresh controls. */
    uint32_t version() const noexcept { return changeCounter.load (std::memory_order_acquire); }

    /** Copies the active bank into dest (size kNumParams). RT-safe. */
    void snapshot (float* dest) const noexcept;

private:
    std::unique_ptr<std::atomic<float>[]> values; // [bank * kNumParams + id]
    std::atomic<int> activeBank { 0 };
    std::atomic<uint32_t> changeCounter { 0 };
};
} // namespace flub::param
