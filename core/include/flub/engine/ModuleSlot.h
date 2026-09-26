// Flubsound Pro - click-free, latency-compensated bypass wrapper.
//
// Wraps one Processor in the chain. Bypass never changes the chain latency:
// the dry path is delayed by the module's latency, and switching crossfades
// dry <-> wet (equal-gain, default 20 ms). States:
//
//   Active --setActive(false)--> FadingOut --(mix hits 0)--> Bypassed
//   Bypassed --setActive(true)--> PreRoll --> FadingIn --(mix hits 1)--> Active
//
// While Bypassed the module is NOT processed (saves CPU). On re-activation it
// is reset() and pre-rolled for latency + 64 samples with the output still
// fully dry, so look-ahead lines / filters are primed before being heard.
// The dry delay line always runs so its content is valid at any moment.
#pragma once

#include "flub/common/AudioBlock.h"
#include "flub/common/DelayLine.h"
#include "flub/dsp/Processor.h"

namespace flub
{
class ModuleSlot
{
public:
    /** Non-RT. The processor must already be configured (structural setters);
        this prepares it with spec. */
    void prepare (Processor& processor, const ProcessSpec& spec, float fadeMs = 20.0f, bool startActive = true);

    /** RT-safe; call once per block before process(). */
    void setActive (bool shouldBeActive) noexcept { wantActive = shouldBeActive; }

    void process (const AudioBlock& block) noexcept;
    void reset() noexcept;

    int latencySamples() const noexcept { return latency; }
    bool isActive() const noexcept { return wantActive; }
    bool isFullyBypassed() const noexcept { return ! processing && mix == 0.0f; }
    float getMix() const noexcept { return mix; }
    Processor* getProcessor() const noexcept { return proc; }

private:
    Processor* proc = nullptr;
    AudioBuffer dry;
    DelayLine dryDelay;
    int latency = 0, preRoll = 0;
    float mix = 1.0f, step = 1.0f / 960.0f;
    bool wantActive = true, processing = true;
};
} // namespace flub
