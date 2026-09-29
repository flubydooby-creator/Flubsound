// Flubsound Pro - libFuzzer target: AutoEQ ParametricEQ.txt / Equalizer APO
// text import (flub::eqtext, docs/11 E15 / E53).
//
// Any text: parse must return, with an error when it refuses and the curve
// untouched. An accepted curve must respect the documented limits (16 filters
// per channel, |gain| <= 30 dB, 1 Hz <= Fc <= 100 kHz, 0 < Q <= 100), give a
// finite response, and survive format -> parse unchanged (the header's
// "parse (format (c)) == c exactly for every curve parse() can produce").

#include "FuzzCheck.h"

#include "flub/dsp/DeviceCorrection.h"
#include "flub/io/ParametricEqText.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput (const uint8_t* data, size_t size)
{
    using namespace flub;
    const std::string_view text (reinterpret_cast<const char*> (data), size);

    CorrectionCurve curve;
    curve.gainDb = { 1.25f, -2.5f }; // a failed parse must leave it as it is
    const CorrectionCurve before = curve;
    const auto result = eqtext::parse (text, curve);
    if (! result.ok)
    {
        FUZZ_CHECK (! result.error.empty());
        FUZZ_CHECK (curve == before);
        return 0;
    }

    FUZZ_CHECK (curve.numFilters >= 0 && curve.numFilters <= CorrectionCurve::kMaxFilters);
    FUZZ_CHECK (curve.countFor (0) <= CorrectionCurve::kMaxFiltersPerChannel);
    FUZZ_CHECK (curve.countFor (1) <= CorrectionCurve::kMaxFiltersPerChannel);
    for (const float g : curve.gainDb)
        FUZZ_CHECK (std::isfinite (g) && std::fabs (g) <= 30.0f);
    for (int i = 0; i < curve.numFilters; ++i)
    {
        const auto& f = curve.filters[static_cast<size_t> (i)];
        FUZZ_CHECK (std::isfinite (f.frequency) && f.frequency >= 1.0f && f.frequency <= 100000.0f);
        FUZZ_CHECK (std::isfinite (f.gainDb) && std::fabs (f.gainDb) <= 30.0f);
        FUZZ_CHECK (std::isfinite (f.q) && f.q > 0.0f && f.q <= 100.0f);
    }
    for (const double hz : { 20.0, 1000.0, 20000.0 })
        for (const int channel : { 0, 1 })
            FUZZ_CHECK (std::isfinite (curve.responseDb (channel, hz, 48000.0)));

    const auto written = eqtext::format (curve);
    CorrectionCurve back;
    const auto again = eqtext::parse (written, back);
    FUZZ_CHECK (again.ok);
    FUZZ_CHECK (back == curve);
    return 0;
}
