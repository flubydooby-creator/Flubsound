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
#pragma once

#include "Parameters.h"

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

class MacroMap
{
public:
    static std::span<const MacroEntry> table (param::ModeValue mode) noexcept;
    static const char* macroName (param::ModeValue mode, int macroIndex) noexcept; // 0..4

    /** effective[] <- base[] with all macro contributions applied and clamped.
        Both arrays have param::kNumParams entries. RT-safe. */
    static void apply (const float* base, float* effective, float governorScale) noexcept;

    /** True when a macro source that can raise paramId in the current mode is
        above zero, even if it has not reached its entry's start point yet
        (e.g. Boost Intensity at 20 % "arms" the glue that begins at 40 %).
        RT-safe. */
    static bool isArmed (const float* base, int paramId) noexcept;
};
} // namespace flub
