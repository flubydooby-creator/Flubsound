#include "flub/engine/ModuleSlot.h"

#include <algorithm>

namespace flub
{
void ModuleSlot::prepare (Processor& processor, const ProcessSpec& spec, float fadeMs, bool startActive)
{
    proc = &processor;
    processor.prepare (spec);
    latency = processor.latencySamples();
    dry.setSize (spec.numChannels, spec.maxBlockSize);
    dryDelay.prepare (spec.numChannels, latency);
    step = 1.0f / std::max (1.0f, static_cast<float> (fadeMs * 0.001 * spec.sampleRate));
    wantActive = startActive;
    processing = startActive;
    mix = startActive ? 1.0f : 0.0f;
    preRoll = 0;
}

void ModuleSlot::reset() noexcept
{
    if (proc != nullptr && processing)
        proc->reset();
    dryDelay.reset();
    mix = wantActive ? 1.0f : 0.0f;
    processing = wantActive;
    preRoll = 0;
}

void ModuleSlot::process (const AudioBlock& block) noexcept
{
    if (proc == nullptr)
        return;

    const int n = block.numSamples;

    // Re-activation from full bypass: start from a clean state and pre-roll so
    // look-ahead lines and filters are primed before the wet signal is heard.
    if (wantActive && ! processing)
    {
        proc->reset();
        processing = true;
        preRoll = latency + 64;
    }
    if (! wantActive)
        preRoll = 0;

    // Dry path always runs so its delay line is valid whenever a fade starts.
    const AudioBlock d = dry.block (block.numChannels, n);
    d.copyFrom (block);
    dryDelay.process (d);

    if (! processing)
    {
        block.copyFrom (d); // fully bypassed: latency-compensated dry
        return;
    }

    proc->process (block);

    const float target = wantActive ? 1.0f : 0.0f;
    if (mix == target && preRoll == 0)
    {
        if (mix == 0.0f)
        {
            block.copyFrom (d);
            processing = false;
        }
        return; // steady state: fully wet (or just finished fading out)
    }

    for (int i = 0; i < n; ++i)
    {
        if (preRoll > 0)
            --preRoll; // hold (mix stays where it is, normally 0)
        else if (mix < target)
            mix = std::min (target, mix + step);
        else if (mix > target)
            mix = std::max (target, mix - step);

        for (int c = 0; c < block.numChannels; ++c)
        {
            float* w = block.channel (c);
            const float x = d.channel (c)[i];
            w[i] = x + mix * (w[i] - x);
        }
    }

    if (! wantActive && mix == 0.0f)
        processing = false;
}
} // namespace flub
