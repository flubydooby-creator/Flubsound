// Flubsound Pro - clarity & transient-detail enhancer.
//
//   1. Transient shaper (full band, linked): attackDb / sustainDb.
//   2. De-mud: dynamic cut, bell ~250 Hz Q 1.0, CutAbove, range up to -4 dB
//      scaled by deMud, threshold tracks the broadband level (-12 dB rel.)
//      so it only acts when the low-mids are disproportionately loud.
//   3. Dynamic presence: bell at presenceFrequency (default 3.2 kHz, Q 0.8),
//      BoostBelow-style (inverse level): up to +6 dB * presence when the
//      band is quiet, withdrawn as it gets loud -> intelligibility without
//      harshness on loud passages.
//   4. Air exciter: band 3.5 - 7 kHz (24 dB/oct each side) -> envelope-
//      normalised polynomial of order <= 3 (2nd + 3rd harmonics) -> high-
//      pass 7 kHz -> mixed in at up to -12 dB * air, plus a +2 dB * air high
//      shelf at 10 kHz. Order <= 3 on content <= 7 kHz keeps every product
//      below 21 kHz, so no oversampling is needed even at 44.1 kHz.
// Zero latency.
#pragma once

#include "Processor.h"

namespace flub
{
struct ClarityParams
{
    float attackDb = 0.0f;            // -12 .. +12
    float sustainDb = 0.0f;           // -12 .. +12
    float presence = 0.0f;            // 0 .. 1
    float presenceFrequency = 3200.0f;// 1000 .. 6000 Hz
    float air = 0.0f;                 // 0 .. 1
    float deMud = 0.0f;               // 0 .. 1

    bool operator== (const ClarityParams&) const = default;
};

class ClarityEnhancer final : public Processor
{
public:
    void prepare (const ProcessSpec& spec) override;
    void reset() noexcept override;
    void process (const AudioBlock& block) noexcept override;
    const char* name() const noexcept override { return "Clarity"; }

    void setParams (const ClarityParams& p) noexcept;
    const ClarityParams& getParams() const noexcept { return params; }

private:
    // ---- implementation-defined below this line ----
    ProcessSpec spec;
    ClarityParams params;
};
} // namespace flub
