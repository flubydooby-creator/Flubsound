// Flubsound Pro - Boost Intensity + Music/Gaming mode macros.
//
// The store holds BASE values (from the preset / user). Each block the chain
// computes EFFECTIVE values:
//
//   eff[p] = clamp( base[p] + sum_e  amount_e * curve_e(source_e) * gov_e )
//   curve(v) = smoothstep(start, end, v) ^ exponent
//   gov_e = governorScale if the entry is "governed" (adds loudness/drive)
//
// Sources: Boost Intensity (0..1) and the five mode macros (0..1) whose
// meaning depends on Mode:
//   Music : Punch, Width, Clarity, Loudness, Warmth
//   Gaming: Footsteps, Positional, Impact, Detail, Voice & Score
//
// Staggered start/end points make Boost Intensity "intelligent": the first
// third mostly adds clarity/width/detail (cheap, clean), the middle adds bass
// and harmonics, and loudness (maximizer drive, saturation) ramps in last -
// so low settings never cost dynamics and high settings stay controlled by
// the SafetyGovernor.
//
// Music Warmth (docs/11 E14) has two row sets, chosen per block by the base
// value of warmth.tapeGrit: by default the level-compensated tone tilt
// (warmth.tone, ToneTilt.h) with a gentle, mostly 2nd-order Tube colour
// (sat.drive up to +0.9 dB, sat.type Tube by an override row, below); with
// warmth.tapeGrit on, the v1 rows (tape drive +9 dB, bass boost +2 dB, bass
// harmonics +0.2, no tilt), which Lo-Fi Chill and Warm Vinyl keep.
//
// Override rows (MacroOverride) make a choice for a parameter the user or
// the preset left alone: while the source is off zero, the base value is
// still the parameter's default and the module it belongs to is not switched
// on in the base values (a preset or user that engaged the saturator chose
// its type, even the default one), the effective value is `value`. Presets
// are sparse (a missing key is the default), so an explicit default on a
// module left off reads as "not chosen"; warmth.tapeGrit keeps Tape anyway.
#pragma once

#include "Parameters.h"
#include "flub/analysis/ContentAnalysis.h"

#include <span>
#include <vector>

namespace flub
{
enum class MacroSource : uint8_t
{
    Boost = 0,
    M1,
    M2,
    M3,
    M4,
    M5
};

struct MacroEntry
{
    MacroSource source;
    int paramId;
    float amount;   // added to the base value at curve = 1 (param units)
    float start;    // source value where the contribution starts (0..1)
    float end;      // source value where it reaches full amount (0..1)
    float exponent; // curve shaping (1 = smoothstep)
    bool governed;  // scaled by the SafetyGovernor
};

struct MacroOverride
{
    MacroSource source;
    int paramId;
    float value;   // the effective value while the source is off zero and the base is the default ...
    int unlessOnId; // ... and this toggle (the module's enable) is off in the base values
};

/** Smart macro scaling (docs/11 E34): multipliers on what the macro rows
    ADD to four groups of parameters (never on base values), from the
    content analysis tap (ContentAnalysis.h) through smartModulation():
      attack : clarity.attack, clarity.attackLow, clarity.attackHigh
      drive  : max.drive, sat.drive
      bass   : bass.boost, bass.harmonics
      air    : clarity.air
    Each is in [0, 1]: Smart only takes back. All 1 is bit-identical to no
    modulation (x * 1 is exact). */
struct MacroModulation
{
    float attack = 1.0f, drive = 1.0f, bass = 1.0f, air = 1.0f;

    bool isIdentity() const noexcept { return attack == 1.0f && drive == 1.0f && bass == 1.0f && air == 1.0f; }
    /** The multiplier on a row that targets paramId (1 outside the groups). */
    float forParam (int paramId) const noexcept;
};

class MacroMap
{
public:
    /** The additive rows of `mode` without Music Warmth's (warmthRows). */
    static std::span<const MacroEntry> table (param::ModeValue mode) noexcept;
    /** Music Warmth's additive rows for these base values (the tone set, or
        the v1 set while warmth.tapeGrit is on); empty in Gaming. */
    static std::span<const MacroEntry> warmthRows (const float* base) noexcept;
    /** The override rows that apply to these base values (Music Warmth's
        Tube, not with warmth.tapeGrit); empty in Gaming. */
    static std::span<const MacroOverride> overrides (const float* base) noexcept;
    static const char* macroName (param::ModeValue mode, int macroIndex) noexcept; // 0..4

    /** The on-board enhancement cap (docs/11 E16): the most of Gaming
        Footsteps and Detail that reaches the chain while it is fully on. */
    static constexpr float kOnboardCapMacroLimit = 0.30f;

    /** Music Punch at high Boost (docs/11 E04 / E53, owner decision
        2026-10-06): what Punch's two attack rows (clarity.attack +6 dB,
        clarity.attackHigh +2.5 dB) add is multiplied by
          punchHighBoostScale (b) = 1 - smoothstep (kPunchEaseFrom, kPunchEaseTo, b)
        so it is exactly 1 up to Boost 60 % (bit-identical) and 0 from 70 %:
        the onset lift goes into a maximizer driven hard enough to tick on
        it (the soak's Music / Loud scenes, docs/11 E53). Boost's own attack
        row is not scaled, nor are Gaming's M1 rows (Footsteps). RT-safe. */
    static constexpr float kPunchEaseFrom = 0.60f, kPunchEaseTo = 0.70f;
    static float punchHighBoostScale (float boost) noexcept;

    /** effective[] <- base[] with all macro contributions applied and clamped.
        Both arrays have param::kNumParams entries. RT-safe.
        onboardCap (docs/11 E16, ProcessingChain::setOnboardEnhancementCap),
        0..1 as it glides: above 0 the Gaming Footsteps (M1) and Detail (M4)
        inputs, and their effective values, are clamped to a limit that
        moves from 1 to kOnboardCapMacroLimit at 1, and virt.on is held off
        in either mode; base[] is never changed. 0 is bit-identical to no cap.
        modulation (docs/11 E34, Smart macros): each row's contribution is
        multiplied by modulation->forParam (row.paramId), after the
        governor's scale; nullptr (or the identity) is bit-identical to none. */
    static void apply (const float* base, float* effective, float governorScale, float onboardCap = 0.0f,
                       const MacroModulation* modulation = nullptr) noexcept;

    /** The Smart scaling law (docs/11 E34, docs/03 §14.16): the targets for
        a content state (identity while it is not valid).
          attack = smoothstep (kSmartPlrZero, kSmartPlrFull, PLR)
          drive  = kSmartDriveFloor + (1 - kSmartDriveFloor) x attack
          bass   = 1 - kSmartBassCut x smoothstep (kSmartLowShareFrom, kSmartLowShareTo, lowShare)
          air    = 1 - kSmartAirCut x smoothstep (kSmartHighTiltFrom, kSmartHighTiltTo, highTilt)
        RT-safe. */
    static MacroModulation smartModulation (const AnalysisState& state) noexcept;
    static constexpr float kSmartPlrZero = 7.5f, kSmartPlrFull = 10.5f; // LU
    static constexpr float kSmartDriveFloor = 0.25f;
    static constexpr float kSmartLowShareFrom = -3.0f, kSmartLowShareTo = 0.0f, kSmartBassCut = 0.5f; // dB
    static constexpr float kSmartHighTiltFrom = -6.0f, kSmartHighTiltTo = 0.0f, kSmartAirCut = 0.5f;   // dB

    /** True when a macro source that can raise paramId in the current mode is
        above zero, even if it has not reached its entry's start point yet
        (e.g. Boost Intensity at 20 % "arms" the glue that begins at 40 %).
        RT-safe. */
    static bool isArmed (const float* base, int paramId) noexcept;
};
} // namespace flub
