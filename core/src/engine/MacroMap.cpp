#include "flub/engine/MacroMap.h"

#include "flub/common/Math.h"

#include <array>
#include <cmath>

namespace flub
{
using namespace param;

namespace
{
// Toggles are engaged by a macro as soon as it moves off zero (curve 0 -> 1
// between 0 and 0.02), so e.g. turning up "Warmth" switches Saturation on.
constexpr float kEngage = 0.02f;

// clang-format off
constexpr std::array<MacroEntry, 30> kMusicTable {{
    // ---- Boost Intensity: clarity/width first, bass next, loudness last ----
    { MacroSource::Boost, ClarityPresence,   0.35f, 0.00f, 0.50f, 1.0f, false },
    { MacroSource::Boost, ClarityAir,        0.30f, 0.10f, 0.60f, 1.0f, false },
    { MacroSource::Boost, ClarityAttackDb,   2.00f, 0.10f, 0.60f, 1.0f, false },
    { MacroSource::Boost, SpatialWidth,      0.20f, 0.00f, 0.50f, 1.0f, false },
    { MacroSource::Boost, BassBoostDb,       5.00f, 0.20f, 0.80f, 1.0f, true  },
    { MacroSource::Boost, BassHarmonics,     0.30f, 0.35f, 0.90f, 1.0f, true  },
    { MacroSource::Boost, MaxDriveDb,        8.00f, 0.30f, 1.00f, 1.2f, true  },
    { MacroSource::Boost, MaxGlue,           0.30f, 0.40f, 1.00f, 1.0f, false },
    { MacroSource::Boost, SatDriveDb,        4.00f, 0.60f, 1.00f, 1.0f, true  },
    // The LF-first limiter (docs/11 E05 step 5) in Boost's top half (no
    // factory preset goes past 45 %): kicks are limited in the low band
    // instead of ducking the whole mix. Ungoverned: it only takes level away.
    // Full from 75 % (docs/11 E05 step 6; it was 100 %): the drive reaches
    // the limiter from about 60 %, and the kick's onset / body there came
    // from the wideband limiter (E59 quality suite, Boost 70 / 80 / 90:
    // -2.14 / -0.89 / -0.05 -> -1.46 / -0.30 / +0.16 dB).
    { MacroSource::Boost, MaxLfLimit,        1.00f, 0.50f, 0.75f, 1.0f, false },
    // ---- M1 Punch ----
    // No BassTighten (docs/11 E04): Tighten 0.5 cut the kick's first 10 ms
    // by 2 dB, the opposite of punch.
    { MacroSource::M1, ClarityOn,            1.00f, 0.00f, kEngage, 1.0f, false },
    { MacroSource::M1, ClarityAttackDb,      6.00f, 0.00f, 1.00f, 1.0f, false },
    // ---- M2 Width ----
    { MacroSource::M2, SpatialOn,            1.00f, 0.00f, kEngage, 1.0f, false },
    { MacroSource::M2, SpatialWidth,         0.60f, 0.00f, 1.00f, 1.0f, false },
    { MacroSource::M2, SpatialSpace,         0.35f, 0.40f, 1.00f, 1.0f, false },
    // ---- M3 Clarity ----
    { MacroSource::M3, ClarityOn,            1.00f, 0.00f, kEngage, 1.0f, false },
    { MacroSource::M3, ClarityPresence,      0.80f, 0.00f, 1.00f, 1.0f, false },
    { MacroSource::M3, ClarityAir,           0.70f, 0.20f, 1.00f, 1.0f, false },
    { MacroSource::M3, ClarityDeMud,         0.50f, 0.00f, 0.70f, 1.0f, false },
    { MacroSource::M3, DynEqOn,              1.00f, 0.00f, kEngage, 1.0f, false },
    // ---- M4 Loudness ----
    { MacroSource::M4, MaximizerOn,          1.00f, 0.00f, kEngage, 1.0f, false },
    { MacroSource::M4, MaxDriveDb,          10.00f, 0.00f, 1.00f, 1.3f, true  },
    { MacroSource::M4, MaxGlue,              0.50f, 0.30f, 1.00f, 1.0f, false },
    // ---- M5 Warmth ----
    { MacroSource::M5, SaturationOn,         1.00f, 0.00f, kEngage, 1.0f, false },
    { MacroSource::M5, SatDriveDb,           9.00f, 0.00f, 1.00f, 1.0f, true  },
    { MacroSource::M5, BassHarmonics,        0.20f, 0.40f, 1.00f, 1.0f, true  },
    { MacroSource::M5, BassBoostDb,          2.00f, 0.30f, 1.00f, 1.0f, true  },
    { MacroSource::M5, BassOn,               1.00f, 0.00f, kEngage, 1.0f, false },
    // Boost Intensity engages the dynamic EQ so the music "de-boom" mode band
    // can follow it (see ProcessingChain::configureModeBands).
    { MacroSource::Boost, DynEqOn,           1.00f, 0.00f, kEngage, 1.0f, false },
    { MacroSource::Boost, MaximizerOn,       1.00f, 0.25f, 0.27f, 1.0f, false },
}};

// Gaming: the broadband upward compressor lifts the whole bed with the cues
// (docs/11 E19), so only Detail - whose job is the environment - drives it.
// Footsteps is the cue enhancer alone (mode bands 4 / 5, see
// ProcessingChain::configureModeBands), and Boost no longer engages the
// compressor.
constexpr std::array<MacroEntry, 23> kGamingTable {{
    // ---- Boost Intensity: detail/positional first, impact next, loudness last ----
    { MacroSource::Boost, ClarityPresence,   0.30f, 0.00f, 0.50f, 1.0f, false },
    { MacroSource::Boost, ClarityAttackDb,   2.00f, 0.20f, 0.70f, 1.0f, false },
    { MacroSource::Boost, SpatialFocus,      0.30f, 0.00f, 0.60f, 1.0f, false },
    { MacroSource::Boost, BassBoostDb,       3.00f, 0.30f, 0.90f, 1.0f, true  },
    { MacroSource::Boost, MaxDriveDb,        6.00f, 0.30f, 1.00f, 1.2f, true  },
    // ---- M1 Footsteps (the internal cue-enhancer dynamic-EQ bands) ----
    { MacroSource::M1, DynEqOn,              1.00f, 0.00f, kEngage, 1.0f, false },
    // ---- M2 Positional ----
    { MacroSource::M2, SpatialOn,            1.00f, 0.00f, kEngage, 1.0f, false },
    { MacroSource::M2, SpatialFocus,         0.90f, 0.00f, 1.00f, 1.0f, false },
    { MacroSource::M2, SpatialWidth,         0.25f, 0.30f, 1.00f, 1.0f, false },
    // ---- M3 Impact (explosions, gunshots) ----
    { MacroSource::M3, BassOn,               1.00f, 0.00f, kEngage, 1.0f, false },
    { MacroSource::M3, BassBoostDb,          6.00f, 0.00f, 1.00f, 1.0f, true  },
    { MacroSource::M3, BassHarmonics,        0.25f, 0.40f, 1.00f, 1.0f, true  },
    { MacroSource::M3, ClarityOn,            1.00f, 0.00f, kEngage, 1.0f, false },
    { MacroSource::M3, ClarityAttackDb,      4.00f, 0.20f, 1.00f, 1.0f, false },
    // ---- M4 Detail (environment, quiet cues) ----
    { MacroSource::M4, CompressorOn,         1.00f, 0.00f, kEngage, 1.0f, false },
    { MacroSource::M4, CompUpMaxGainDb,      8.00f, 0.00f, 1.00f, 1.0f, false },
    { MacroSource::M4, ClarityOn,            1.00f, 0.00f, kEngage, 1.0f, false },
    { MacroSource::M4, ClarityAir,           0.40f, 0.20f, 1.00f, 1.0f, false },
    // ---- M5 Voice & Score (plus the internal voice dynamic-EQ band) ----
    { MacroSource::M5, ClarityOn,            1.00f, 0.00f, kEngage, 1.0f, false },
    { MacroSource::M5, ClarityPresence,      0.70f, 0.00f, 1.00f, 1.0f, false },
    { MacroSource::M5, ClarityDeMud,         0.40f, 0.20f, 1.00f, 1.0f, false },
    { MacroSource::M5, DynEqOn,              1.00f, 0.00f, kEngage, 1.0f, false },
    { MacroSource::Boost, MaximizerOn,       1.00f, 0.25f, 0.27f, 1.0f, false },
}};
// clang-format on

float sourceValue (const float* base, MacroSource s) noexcept
{
    switch (s)
    {
        case MacroSource::Boost: return base[BoostIntensity];
        case MacroSource::M1: return base[Macro1];
        case MacroSource::M2: return base[Macro2];
        case MacroSource::M3: return base[Macro3];
        case MacroSource::M4: return base[Macro4];
        case MacroSource::M5: return base[Macro5];
    }
    return 0.0f;
}
} // namespace

std::span<const MacroEntry> MacroMap::table (ModeValue mode) noexcept
{
    if (mode == ModeValue::Gaming)
        return { kGamingTable.data(), kGamingTable.size() };
    return { kMusicTable.data(), kMusicTable.size() };
}

const char* MacroMap::macroName (ModeValue mode, int macroIndex) noexcept
{
    static const char* music[] = { "Punch", "Width", "Clarity", "Loudness", "Warmth" };
    static const char* gaming[] = { "Footsteps", "Positional", "Impact", "Detail", "Voice & Score" };
    if (macroIndex < 0 || macroIndex > 4)
        return "";
    return mode == ModeValue::Gaming ? gaming[macroIndex] : music[macroIndex];
}

bool MacroMap::isArmed (const float* base, int paramId) noexcept
{
    const auto mode = static_cast<ModeValue> (static_cast<int> (std::lround (base[Mode])));
    for (const auto& e : table (mode))
        if (e.paramId == paramId && e.amount > 0.0f && sourceValue (base, e.source) > 0.0f)
            return true;
    return false;
}

void MacroMap::apply (const float* base, float* effective, float governorScale) noexcept
{
    for (int i = 0; i < kNumParams; ++i)
        effective[i] = base[i];

    const auto mode = static_cast<ModeValue> (static_cast<int> (std::lround (base[Mode])));
    const auto& info = layout();

    for (const auto& e : table (mode))
    {
        const float v = sourceValue (base, e.source);
        if (v <= 0.0f)
            continue;
        float c = smoothstep (e.start, e.end, v);
        if (e.exponent != 1.0f)
            c = std::pow (c, e.exponent);
        const float g = e.governed ? governorScale : 1.0f;
        effective[e.paramId] += e.amount * c * g;
    }

    for (const auto& e : table (mode))
        effective[e.paramId] = info[static_cast<size_t> (e.paramId)].clamp (effective[e.paramId]);
}
} // namespace flub
