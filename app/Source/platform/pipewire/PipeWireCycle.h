// Flubsound Pro - one PipeWire graph cycle of the native node (docs/11 E48).
//
// Plain C++ (no libpipewire): PipeWireNative.cpp's process callback fetches
// each port's buffer (pw_filter_get_dsp_buffer, which returns null for a
// port without a buffer) and hands the pointers to CycleRunner::run on
// PipeWire's real-time data thread. The runner never allocates, locks or
// blocks: prepare() sizes everything beforehand. Tests drive it with plain
// arrays.
#pragma once

#include "../PlatformServices.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace flub::platform::pipewire
{
class CycleRunner
{
public:
    /** Not real time: sizes the silence, scratch and pointer arrays. */
    void prepare (int inputs, int outputs, int maxBlockFrames)
    {
        numInputs = std::max (0, inputs);
        numOutputs = std::max (0, outputs);
        maxFrames = std::max (1, maxBlockFrames);
        silence.assign (static_cast<size_t> (maxFrames), 0.0f);
        scratch.assign (static_cast<size_t> (numOutputs) * static_cast<size_t> (maxFrames), 0.0f);
        inputPointers.assign (static_cast<size_t> (numInputs), nullptr);
        outputPointers.assign (static_cast<size_t> (numOutputs), nullptr);
    }

    int getNumInputs() const noexcept { return numInputs; }
    int getNumOutputs() const noexcept { return numOutputs; }
    int getMaxBlockFrames() const noexcept { return maxFrames; }

    /** Real time. portInputs / portOutputs: one pointer per port as PipeWire
        gave it (null = no buffer this cycle), each good for 'frames' samples.
        The callback gets never-null pointers in blocks of at most
        maxBlockFrames; a null output is written to scratch and dropped.
        Returns the number of callback calls. */
    int run (const float* const* portInputs, float* const* portOutputs, uint32_t frames, NativeAudioNode::Callback& callback) noexcept FLUB_NONBLOCKING
    {
        int calls = 0;
        uint32_t done = 0;
        while (done < frames)
        {
            const uint32_t block = std::min (frames - done, static_cast<uint32_t> (maxFrames));
            for (int i = 0; i < numInputs; ++i)
            {
                const float* port = portInputs != nullptr ? portInputs[i] : nullptr;
                inputPointers[static_cast<size_t> (i)] = port != nullptr ? port + done : silence.data();
            }
            for (int o = 0; o < numOutputs; ++o)
            {
                float* port = portOutputs != nullptr ? portOutputs[o] : nullptr;
                outputPointers[static_cast<size_t> (o)] = port != nullptr ? port + done : scratch.data() + static_cast<size_t> (o) * static_cast<size_t> (maxFrames);
            }
            callback.nodeProcess (inputPointers.data(), numInputs, outputPointers.data(), numOutputs, static_cast<int> (block));
            done += block;
            ++calls;
        }
        return calls;
    }

private:
    int numInputs = 0, numOutputs = 0, maxFrames = 1;
    std::vector<float> silence, scratch;
    std::vector<const float*> inputPointers;
    std::vector<float*> outputPointers;
};
} // namespace flub::platform::pipewire
